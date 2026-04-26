#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <stdatomic.h>
#include <signal.h>
#include <errno.h>

#include "simple_gc.h"

/* ═══════════════════════════════════════════════════════════════════
 * 상수 및 타입 정의
 * ═══════════════════════════════════════════════════════════════════ */
#define ALIGN8(x)           (((x) + 7) & ~7)
#define TLAB_SIZE           (64 * 1024)
#define GC_THRESHOLD        (8 * 1024 * 1024)
#define MAGIC_LIVE          0xEE5EE5EE
#define MAGIC_DEAD          0xDEADBEEF
#define MAX_THREADS         64
#define MAX_CHUNKS          (MAX_THREADS * 256)

typedef enum {
    GC_PHASE_IDLE    = 0,
    GC_PHASE_RUNNING = 1,
} GCPhase;

typedef struct ObjectHeader {
    size_t               size;
    uint32_t             marked;
    uint64_t             magic;
    struct ObjectHeader* next_free;
} ObjectHeader;

#define HEADER_SIZE ALIGN8(sizeof(ObjectHeader))

typedef struct {
    char* start;
    char* cursor;
    char* end;
} TLAB;

static __thread TLAB local_tlab         = {NULL, NULL, NULL};
static __thread int  local_thread_index = -1;

typedef struct ChunkNode {
    void* start;
    size_t            size;
    struct ChunkNode* next;
} ChunkNode;

static ChunkNode    chunk_pool[MAX_CHUNKS];
static atomic_int   chunk_pool_next = 0;

static ChunkNode* chunk_alloc(void) {
    int idx = atomic_fetch_add(&chunk_pool_next, 1);
    if (idx >= MAX_CHUNKS) return NULL;
    return &chunk_pool[idx];
}

static ChunkNode* global_chunks    = NULL;
static ObjectHeader* global_free_list = NULL;
static atomic_size_t total_heap_size  = 0;
static pthread_mutex_t gc_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    pthread_t    tid;
    void* stack_bottom;
    void* stack_top;
    int          active;
} ThreadInfo;

static ThreadInfo  thread_table[MAX_THREADS];
static int         thread_count     = 0;
static int         registered_count = 0;
static atomic_int  parked_count     = 0;

static atomic_int gc_phase = GC_PHASE_IDLE;
static pthread_mutex_t stw_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 리눅스/유닉스 시스템에서 제공하는 메모리 세그먼트 경계 심볼 (전역 변수 영역) */
extern char etext; /* 코드 영역의 끝, 초기화된 데이터 영역의 시작 */
extern char end;   /* 비초기화 데이터(BSS) 영역의 끝 */

/* 스레드 종료를 감지하여 GC 명부에서 자동으로 제거하기 위한 TLS 키 */
static pthread_key_t gc_thread_key;

/* ═══════════════════════════════════════════════════════════════════
 * 스레드 관리 & Signal STW 로직
 * ═══════════════════════════════════════════════════════════════════ */

/* 스레드가 종료될 때 OS가 자동으로 호출하는 함수 */
static void gc_thread_destructor(void* arg) {
    GC_unregister_thread();
}

/* GC가 시작될 때 다른 스레드들을 강제로 멈추게 하는 Signal 핸들러 */
static void suspend_signal_handler(int signum) {
    int idx = local_thread_index;
    if (idx < 0) return;

    /* 현재 스택 탑을 기록 */
    volatile char sp_marker;
    thread_table[idx].stack_top = (void*)&sp_marker;

    /* Park 상태 진입을 GC 스레드에게 알림 */
    atomic_fetch_add(&parked_count, 1);

    /* GC가 완전히 끝날 때까지 대기 (IDLE 상태가 될 때까지 Spin) */
    while (atomic_load_explicit(&gc_phase, memory_order_acquire) == GC_PHASE_RUNNING) {
        sched_yield(); 
    }

    /* Park 상태 해제 알림 */
    atomic_fetch_sub(&parked_count, 1);
}

/* ═══════════════════════════════════════════════════════════════════
 * Mark & Sweep 구현
 * ═══════════════════════════════════════════════════════════════════ */

static ChunkNode* find_chunk(void* ptr) {
    for (ChunkNode* c = global_chunks; c; c = c->next) {
        char* s = (char*)c->start;
        char* e = s + c->size;
        if ((char*)ptr >= s && (char*)ptr < e) return c;
    }
    return NULL;
}

static void scan_region(void* start, void* end_ptr);

static void find_and_mark(void* candidate) {
    if (!candidate) return;
    if ((uintptr_t)candidate % 8 != 0) return;

    ChunkNode* chunk = find_chunk(candidate);
    if (!chunk) return;

    if ((char*)candidate < (char*)chunk->start + HEADER_SIZE) return;

    ObjectHeader* h = (ObjectHeader*)((char*)candidate - HEADER_SIZE);

    if (h->magic != MAGIC_LIVE) return;
    if (h->marked) return;

    h->marked = 1;
    
    /* 객체 내부를 재귀적으로 스캔 */
    scan_region((void*)(h + 1), (char*)(h + 1) + h->size);
}

static void scan_region(void* start, void* end_ptr) {
    if (!start || !end_ptr) return;

    /* 스택 방향 고려 (높은 주소 -> 낮은 주소일 경우 스왑) */
    if (start > end_ptr) {
        void* tmp = start;
        start     = end_ptr;
        end_ptr   = tmp;
    }

    void** cur = (void**)ALIGN8((uintptr_t)start);
    while ((char*)(cur + 1) <= (char*)end_ptr) {
        find_and_mark(*cur);
        cur++;
    }
}

static void mark_phase(void) {
    /* 1. 전역 변수 영역(Data & BSS) 스캔 (테스트 코드의 global_root 보호용) */
    scan_region(&etext, &end);

    /* 2. 각 활성 스레드의 스택 스캔 */
    for (int i = 0; i < thread_count; i++) {
        if (!thread_table[i].active) continue;

        void* top    = thread_table[i].stack_top;
        void* bottom = thread_table[i].stack_bottom;

        if (!top || !bottom) continue;

        scan_region(top, bottom);
    }
}

static void sweep_phase(void) {
    global_free_list = NULL;

    for (ChunkNode* chunk = global_chunks; chunk; chunk = chunk->next) {
        char* ptr   = (char*)chunk->start;
        char* limit = ptr + chunk->size;

        while (ptr + HEADER_SIZE <= limit) {
            ObjectHeader* h = (ObjectHeader*)ptr;
            if (h->magic != MAGIC_LIVE) break; /* 청크 끝 또는 미초기화 영역 */
            
            size_t obj_total = ALIGN8(h->size + HEADER_SIZE);
            if (obj_total == 0) break;

            if (h->marked) {
                h->marked = 0; /* 살아있는 객체는 마크 해제 */
            } else {
                /* 죽은 객체는 free list로 회수 */
                h->magic     = MAGIC_DEAD;
                h->next_free = global_free_list;
                global_free_list = h;
            }
            ptr += obj_total;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * GC 코어 함수 (Collect & Malloc)
 * ═══════════════════════════════════════════════════════════════════ */

void GC_collect(void) {
    //pthread_mutex_lock(&stw_mutex);

    /* [데드락 방지] 다른 스레드에게 시그널을 보내기 '전'에 힙 락을 먼저 선점 */
    pthread_mutex_lock(&gc_lock);

    /* GC 상태를 RUNNING으로 변경 */
    atomic_store_explicit(&gc_phase, GC_PHASE_RUNNING, memory_order_release);

    int expected_parked = registered_count - 1; /* GC 호출 스레드 본인 제외 */
    if (expected_parked < 0) expected_parked = 0;

    /* 모든 활성 스레드에게 SIGUSR1 전송하여 강제 STW */
    pthread_t self = pthread_self();
    for (int i = 0; i < thread_count; i++) {
        if (thread_table[i].active && thread_table[i].tid != self) {
            pthread_kill(thread_table[i].tid, SIGUSR1);
        }
    }

    /* 모든 스레드가 Signal Handler 안에서 park 될 때까지 대기 */
    while (atomic_load_explicit(&parked_count, memory_order_acquire) < expected_parked) {
        sched_yield();
    }

    /* GC 스레드 자신의 스택 탑 기록 */
    if (local_thread_index >= 0) {
        volatile char sp_marker;
        thread_table[local_thread_index].stack_top = (void*)&sp_marker;
    }

    /* 안전하게 힙 구조 정리 수행 */
    mark_phase();
    sweep_phase();

    /* STW 해제: 스레드들 재개 */
    atomic_store_explicit(&gc_phase, GC_PHASE_IDLE, memory_order_release);

    /* 모든 스레드가 park 상태를 완전히 벗어날 때까지 대기 */
    while (atomic_load_explicit(&parked_count, memory_order_acquire) > 0) {
        sched_yield();
    }

    pthread_mutex_unlock(&gc_lock);
    //pthread_mutex_unlock(&stw_mutex);
}

static void* try_alloc_from_free_list(size_t size) {
    ObjectHeader** prev = &global_free_list;
    ObjectHeader* cur  = global_free_list;

    while (cur) {
        if (cur->size >= size) {
            *prev = cur->next_free;
            cur->magic     = MAGIC_LIVE;
            cur->marked    = 0;
            cur->next_free = NULL;
            return (void*)(cur + 1);
        }
        prev = &cur->next_free;
        cur  = cur->next_free;
    }
    return NULL;
}

static void* alloc_new_tlab(size_t size) {
    size_t obj_total = ALIGN8(size + HEADER_SIZE);
    if (obj_total > TLAB_SIZE) return NULL;

    void* mem = NULL;
    if (posix_memalign(&mem, 8, TLAB_SIZE) != 0) return NULL;
    memset(mem, 0, TLAB_SIZE);

    ChunkNode* node = chunk_alloc();
    if (!node) {
        free(mem);
        return NULL;
    }
    node->start = mem;
    node->size  = TLAB_SIZE;

    pthread_mutex_lock(&gc_lock);

    node->next    = global_chunks;
    global_chunks = node;
    atomic_fetch_add(&total_heap_size, TLAB_SIZE);

    local_tlab.start  = mem;
    local_tlab.cursor = mem;
    local_tlab.end    = (char*)mem + TLAB_SIZE;

    ObjectHeader* h = (ObjectHeader*)local_tlab.cursor;
    h->size      = size;
    h->marked    = 0;
    h->magic     = MAGIC_LIVE;
    h->next_free = NULL;
    local_tlab.cursor += obj_total;

    pthread_mutex_unlock(&gc_lock);

    return (void*)(h + 1);
}

void* GC_malloc(size_t size) {
    /* 미등록 스레드 자동 등록 */
    if (local_thread_index == -1) {
        GC_register_thread();
    }

    if (size == 0) size = 8;
    size = ALIGN8(size);
    size_t obj_total = ALIGN8(size + HEADER_SIZE);

    /* Fast Path: TLAB에 공간이 있으면 즉시 할당 (Lock Free) */
    if (local_tlab.cursor && local_tlab.cursor + obj_total <= local_tlab.end) {
        ObjectHeader* h = (ObjectHeader*)local_tlab.cursor;
        h->size      = size;
        h->marked    = 0;
        h->magic     = MAGIC_LIVE;
        h->next_free = NULL;
        local_tlab.cursor += obj_total;
        return (void*)(h + 1);
    }

    /* Slow Path: 임계값 초과 시 GC 수행 후 새 공간 탐색 */
    if (atomic_load(&total_heap_size) + TLAB_SIZE > GC_THRESHOLD) {
        GC_collect();
    }

    /* Free list에서 재사용 시도 */
    pthread_mutex_lock(&gc_lock);
    void* recycled = try_alloc_from_free_list(size);
    pthread_mutex_unlock(&gc_lock);

    if (recycled) return recycled;

    /* 새 TLAB 할당 */
    return alloc_new_tlab(size);
}

/* ═══════════════════════════════════════════════════════════════════
 * 초기화 및 등록 API
 * ═══════════════════════════════════════════════════════════════════ */

void GC_register_thread(void) {
    if (local_thread_index != -1) return;

    pthread_mutex_lock(&gc_lock);

    if (local_thread_index == -1 && thread_count < MAX_THREADS) {
        int idx = thread_count++;
        registered_count++;

        thread_table[idx].tid          = pthread_self();
        thread_table[idx].stack_bottom = __builtin_frame_address(0);
        thread_table[idx].stack_top    = NULL;
        thread_table[idx].active       = 1;

        local_thread_index = idx;
        
        /* 스레드 종료 시 자동으로 gc_thread_destructor가 호출되도록 플래그 설정 */
        pthread_setspecific(gc_thread_key, (void*)(intptr_t)1);
    }

    pthread_mutex_unlock(&gc_lock);
}

void GC_unregister_thread(void) {
    if (local_thread_index < 0) return;

    pthread_mutex_lock(&gc_lock);

    thread_table[local_thread_index].active = 0;
    registered_count--;

    pthread_mutex_unlock(&gc_lock);

    local_thread_index = -1;
}

void GC_init(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = suspend_signal_handler;
    sa.sa_flags   = SA_RESTART; /* 시스템 콜 도중 시그널이 걸려도 자동 재시작 */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    /* TLS 키 생성 (소멸자 등록) */
    pthread_key_create(&gc_thread_key, gc_thread_destructor);

    GC_register_thread();
}