#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <stdatomic.h>
#include <errno.h>
#include <dlfcn.h> /* dlsym 사용을 위해 추가 */

#include "simple_gc.h"

/* ═══════════════════════════════════════════════════════════════════
 * 1. 상수 및 임계값 설정
 * ═══════════════════════════════════════════════════════════════════ */
#define ALIGN8(x)           (((x) + 7) & ~7)
#define CHUNK_SIZE          (64 * 1024)
#define LOCAL_THRESHOLD     (2 * 1024 * 1024)  /* 2MB */
#define SHARED_THRESHOLD    (16 * 1024 * 1024) /* 16MB */

#define MAGIC_LIVE          0xEE5EE5EE
#define MAGIC_DEAD          0xDEADBEEF
#define MAX_THREADS         64

#define INITIAL_MARK_STACK_CAPACITY 4096

/* ═══════════════════════════════════════════════════════════════════
 * 2. 자료구조 정의
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct ObjectHeader {
    size_t               size;
    uint32_t             marked; /* 0: White, 1: Gray or Black */
    uint8_t              is_shared; 
    uint8_t              padding[3];
    uint64_t             magic;
    struct ObjectHeader* next_free;
} ObjectHeader;

#define HEADER_SIZE ALIGN8(sizeof(ObjectHeader))

typedef struct ChunkNode {
    void* start;
    size_t size;
    struct ChunkNode* next;
} ChunkNode;

typedef struct { char *start, *cursor, *end; } TLAB;

typedef enum {
    THREAD_RUNNING = 0,
    THREAD_IN_SYSCALL,
    THREAD_SUSPENDED
} ThreadState;

typedef struct {
    void** data;
    int capacity;
    int top;
} MarkStack;

/* [전역 공간] */
static ChunkNode* global_chunks = NULL;
static ObjectHeader* global_free_list = NULL;
static TLAB global_tlab = {NULL, NULL, NULL};
static atomic_size_t shared_allocated = 0;
static pthread_spinlock_t global_gc_lock;

/* Safepoint 제어용 변수 (parked_count 제거됨) */
static atomic_int global_safepoint_request = 0;
static pthread_cond_t safepoint_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t safepoint_mutex = PTHREAD_MUTEX_INITIALIZER;

/* [스레드 로컬 공간] */
typedef struct {
    pthread_t tid;
    void *stack_bottom, *stack_top;
    int active;
    atomic_int state; /* 스레드의 현재 상태를 추적 */
    
    ChunkNode* local_chunks;
    ObjectHeader* local_free_list;
    TLAB local_tlab;
    size_t local_allocated;
} ThreadInfo;

static ThreadInfo thread_table[MAX_THREADS];
static int thread_count = 0;
static int registered_count = 0;
static __thread int local_thread_index = -1;

extern char etext, end;
static pthread_key_t gc_thread_key;

/* ═══════════════════════════════════════════════════════════════════
 * 3. 시스템 콜 래핑 (메인 스레드 데드락 방지)
 * ═══════════════════════════════════════════════════════════════════ */
void GC_enter_syscall(void) {
    if (local_thread_index >= 0) 
        atomic_store_explicit(&thread_table[local_thread_index].state, THREAD_IN_SYSCALL, memory_order_release);
}

void GC_exit_syscall(void) {
    if (local_thread_index >= 0) {
        /* 복귀 시점에 GC가 진행 중이라면 즉시 멈춤 */
        if (atomic_load_explicit(&global_safepoint_request, memory_order_acquire)) {
            atomic_store_explicit(&thread_table[local_thread_index].state, THREAD_SUSPENDED, memory_order_release);
            
            pthread_mutex_lock(&safepoint_mutex);
            while (atomic_load_explicit(&global_safepoint_request, memory_order_acquire)) {
                pthread_cond_wait(&safepoint_cond, &safepoint_mutex);
            }
            pthread_mutex_unlock(&safepoint_mutex);
        }
        /* 정상 복귀 */
        atomic_store_explicit(&thread_table[local_thread_index].state, THREAD_RUNNING, memory_order_release);
    }
}

/* 원본 pthread_join을 가로채어 GC 사각지대를 없앰 */
int pthread_join(pthread_t thread, void **retval) {
    static int (*real_join)(pthread_t, void**) = NULL;
    if (!real_join) {
        real_join = dlsym(RTLD_NEXT, "pthread_join");
    }
    
    GC_enter_syscall();
    int res = real_join(thread, retval);
    GC_exit_syscall();
    
    return res;
}

/* ═══════════════════════════════════════════════════════════════════
 * 4. Safepoint & 락 유틸리티
 * ═══════════════════════════════════════════════════════════════════ */
void GC_safepoint_poll(void) {
    if (__builtin_expect(atomic_load_explicit(&global_safepoint_request, memory_order_relaxed), 0)) {
        int idx = local_thread_index; 
        if (idx < 0) return;
        
        ThreadInfo* me = &thread_table[idx];
        volatile char sp_marker; 
        me->stack_top = (void*)&sp_marker; 

        atomic_store_explicit(&me->state, THREAD_SUSPENDED, memory_order_release);

        pthread_mutex_lock(&safepoint_mutex);
        while (atomic_load_explicit(&global_safepoint_request, memory_order_acquire)) {
            pthread_cond_wait(&safepoint_cond, &safepoint_mutex);
        }
        pthread_mutex_unlock(&safepoint_mutex);

        atomic_store_explicit(&me->state, THREAD_RUNNING, memory_order_release);
    }
}

static inline void acquire_global_gc_lock(void) {
    while (pthread_spin_trylock(&global_gc_lock) != 0) {
        GC_safepoint_poll();
        sched_yield();
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * 5. 명시적 Mark Stack 유틸리티
 * ═══════════════════════════════════════════════════════════════════ */
static void mark_stack_init(MarkStack* stack) {
    stack->capacity = INITIAL_MARK_STACK_CAPACITY;
    stack->top = -1;
    stack->data = malloc(sizeof(void*) * stack->capacity);
    if (!stack->data) { perror("Mark stack init failed"); abort(); }
}

static inline void mark_stack_push(MarkStack* stack, void* ptr) {
    if (stack->top + 1 >= stack->capacity) {
        stack->capacity *= 2;
        stack->data = realloc(stack->data, sizeof(void*) * stack->capacity);
        if (!stack->data) { perror("Mark stack overflow & realloc failed"); abort(); }
    }
    stack->data[++stack->top] = ptr;
}

static inline void* mark_stack_pop(MarkStack* stack) {
    if (stack->top < 0) return NULL;
    return stack->data[stack->top--];
}

static void mark_stack_destroy(MarkStack* stack) {
    free(stack->data);
}

/* ═══════════════════════════════════════════════════════════════════
 * 6. 안전한 메모리 검사 및 쓰기 장벽
 * ═══════════════════════════════════════════════════════════════════ */
static ChunkNode* is_in_chunks(void* ptr, ChunkNode* list) {
    for (ChunkNode* c = list; c; c = c->next) {
        if ((char*)ptr >= (char*)c->start + HEADER_SIZE && (char*)ptr < (char*)c->start + c->size) {
            return c;
        }
    }
    return NULL;
}

static ObjectHeader* get_object_header_safe(void* ptr, int check_local, int check_shared) {
    if (!ptr || (uintptr_t)ptr % 8 != 0) return NULL;
    
    ChunkNode* found = NULL;
    if (check_local && local_thread_index >= 0) {
        found = is_in_chunks(ptr, thread_table[local_thread_index].local_chunks);
    }
    if (!found && check_shared) {
        found = is_in_chunks(ptr, global_chunks);
    }

    if (found) {
        ObjectHeader* h = (ObjectHeader*)((char*)ptr - HEADER_SIZE);
        if (h->magic == MAGIC_LIVE || h->magic == MAGIC_DEAD) return h;
    }
    return NULL;
}

void GC_write_barrier(void* obj_ptr, void** field_ptr, void* value) {
    if (field_ptr) *field_ptr = value;
    if (!obj_ptr || !value) return;

    ObjectHeader* src = get_object_header_safe(obj_ptr, 1, 1);
    ObjectHeader* dst = get_object_header_safe(value, 1, 1);

    if (src && dst) {
        if (src->is_shared == 1 && dst->is_shared == 0) {
            fprintf(stderr, "\n[FATAL ERROR] Escape Violation Detected!\n");
            abort(); 
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * 7. Thread-Local GC 
 * ═══════════════════════════════════════════════════════════════════ */
void GC_collect_local(void) {
    if (local_thread_index < 0) return;
    ThreadInfo* me = &thread_table[local_thread_index];

    volatile char sp_marker;
    me->stack_top = (void*)&sp_marker;

    MarkStack local_stack;
    mark_stack_init(&local_stack);

    void** cur = (void**)ALIGN8((uintptr_t)me->stack_top);
    void** limit = (void**)me->stack_bottom;
    if (cur > limit) { void** tmp = cur; cur = limit; limit = tmp; }
    
    while (cur < limit) {
        ObjectHeader* h = get_object_header_safe(*cur, 1, 0);
        if (h && !h->marked && h->magic == MAGIC_LIVE && !h->is_shared) {
            h->marked = 1;
            mark_stack_push(&local_stack, *cur);
        }
        cur++;
    }

    while (local_stack.top >= 0) {
        void* obj = mark_stack_pop(&local_stack);
        ObjectHeader* h = get_object_header_safe(obj, 1, 0);
        if (!h) continue;

        void** fields = (void**)(h + 1);
        void** fields_end = (void**)((char*)(h + 1) + h->size);
        while (fields < fields_end) {
            if ((uintptr_t)(*fields) % 8 == 0) {
                ObjectHeader* child = get_object_header_safe(*fields, 1, 0);
                if (child && !child->marked && child->magic == MAGIC_LIVE && !child->is_shared) {
                    child->marked = 1;
                    mark_stack_push(&local_stack, *fields);
                }
            }
            fields++;
        }
    }
    mark_stack_destroy(&local_stack);

    me->local_free_list = NULL;
    for (ChunkNode* c = me->local_chunks; c; c = c->next) {
        char *p = (char*)c->start, *lim = p + c->size;
        ObjectHeader* prev_dead = NULL;

        while (p + HEADER_SIZE <= lim) {
            ObjectHeader* h = (ObjectHeader*)p;
            if (h->magic != MAGIC_LIVE && h->magic != MAGIC_DEAD) break;
            size_t total = ALIGN8(h->size + HEADER_SIZE);

            if (h->marked) {
                h->marked = 0;
                prev_dead = NULL;
            } else {
                if (prev_dead) {
                    prev_dead->size += total;
                } else {
                    h->magic = MAGIC_DEAD; 
                    h->next_free = me->local_free_list; 
                    me->local_free_list = h;
                    prev_dead = h;
                }
            }
            p += total;
        }
    }
    me->local_allocated = 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * 8. Global GC (State-based 동기화 적용)
 * ═══════════════════════════════════════════════════════════════════ */
void GC_collect_global(void) {
    acquire_global_gc_lock();
    
    if (atomic_load_explicit(&shared_allocated, memory_order_acquire) <= SHARED_THRESHOLD) {
        pthread_spin_unlock(&global_gc_lock);
        return;
    }

    atomic_store_explicit(&global_safepoint_request, 1, memory_order_release);
    pthread_t self = pthread_self();
    
    /* [수정됨] 모든 활성 스레드가 RUNNING 상태를 벗어날 때까지 대기 */
    while (1) {
        int all_safe = 1;
        for (int i = 0; i < thread_count; i++) {
            if (thread_table[i].active && thread_table[i].tid != self) {
                ThreadState s = atomic_load_explicit(&thread_table[i].state, memory_order_acquire);
                if (s == THREAD_RUNNING) { 
                    all_safe = 0; break; 
                }
            }
        }
        if (all_safe) break;
        sched_yield();
    }

    if (local_thread_index >= 0) { volatile char sp; thread_table[local_thread_index].stack_top = (void*)&sp; }

    MarkStack global_stack;
    mark_stack_init(&global_stack);

    void add_root(void* ptr) {
        ObjectHeader* h = get_object_header_safe(ptr, 1, 1);
        if (h && !h->marked && h->magic == MAGIC_LIVE) {
            h->marked = 1;
            mark_stack_push(&global_stack, ptr);
        }
    }

    void** cur = (void**)ALIGN8((uintptr_t)&etext);
    while (cur < (void**)&end) { add_root(*cur); cur++; }

    for (int i=0; i < thread_count; i++) {
        if (thread_table[i].active && thread_table[i].stack_top && thread_table[i].stack_bottom) {
            void** t_cur = (void**)ALIGN8((uintptr_t)thread_table[i].stack_top);
            void** t_limit = (void**)thread_table[i].stack_bottom;
            if (t_cur > t_limit) { void** tmp = t_cur; t_cur = t_limit; t_limit = tmp; }
            while (t_cur < t_limit) { add_root(*t_cur); t_cur++; }
        }
    }

    while (global_stack.top >= 0) {
        void* obj = mark_stack_pop(&global_stack);
        ObjectHeader* h = get_object_header_safe(obj, 1, 1);
        if (!h) continue;

        void** fields = (void**)(h + 1);
        void** fields_end = (void**)((char*)(h + 1) + h->size);
        while (fields < fields_end) {
            if ((uintptr_t)(*fields) % 8 == 0) {
                ObjectHeader* child = get_object_header_safe(*fields, 1, 1);
                if (child && !child->marked && child->magic == MAGIC_LIVE) {
                    child->marked = 1; 
                    mark_stack_push(&global_stack, *fields);
                }
            }
            fields++;
        }
    }
    mark_stack_destroy(&global_stack);

    global_free_list = NULL;
    for (ChunkNode* c = global_chunks; c; c = c->next) {
        char *p = (char*)c->start, *lim = p + c->size;
        ObjectHeader* prev_dead = NULL;

        while (p + HEADER_SIZE <= lim) {
            ObjectHeader* h = (ObjectHeader*)p;
            if (h->magic != MAGIC_LIVE && h->magic != MAGIC_DEAD) break;
            size_t total = ALIGN8(h->size + HEADER_SIZE);

            if (h->marked) {
                h->marked = 0;
                prev_dead = NULL;
            } else {
                if (prev_dead) {
                    prev_dead->size += total;
                } else {
                    h->magic = MAGIC_DEAD; 
                    h->next_free = global_free_list; 
                    global_free_list = h;
                    prev_dead = h;
                }
            }
            p += total;
        }
    }
    atomic_store(&shared_allocated, 0);
    
    pthread_mutex_lock(&safepoint_mutex);
    atomic_store_explicit(&global_safepoint_request, 0, memory_order_release);
    pthread_cond_broadcast(&safepoint_cond);
    pthread_mutex_unlock(&safepoint_mutex);
    
    /* [수정됨] 모든 스레드가 SUSPENDED 상태를 벗어날 때까지 대기 */
    while (1) {
        int any_suspended = 0;
        for (int i = 0; i < thread_count; i++) {
            if (thread_table[i].active && thread_table[i].tid != self) {
                if (atomic_load_explicit(&thread_table[i].state, memory_order_acquire) == THREAD_SUSPENDED) {
                    any_suspended = 1; break;
                }
            }
        }
        if (!any_suspended) break;
        sched_yield();
    }
    
    pthread_spin_unlock(&global_gc_lock);
}

/* ═══════════════════════════════════════════════════════════════════
 * 9. 할당 API
 * ═══════════════════════════════════════════════════════════════════ */
extern void GC_register_thread(void);

void* GC_malloc(size_t size, int is_shared) {
    if (local_thread_index == -1) GC_register_thread();
    
    GC_safepoint_poll();

    size = ALIGN8(size == 0 ? 8 : size);
    size_t total = ALIGN8(size + HEADER_SIZE);

    if (!is_shared) {
        ThreadInfo* me = &thread_table[local_thread_index];
        
        if (me->local_tlab.cursor && me->local_tlab.cursor + total <= me->local_tlab.end) {
            ObjectHeader* h = (ObjectHeader*)me->local_tlab.cursor;
            h->size = size; h->is_shared = 0; h->magic = MAGIC_LIVE; h->marked = 0;
            me->local_tlab.cursor += total;
            me->local_allocated += total;
            return (void*)(h + 1);
        }

        if (me->local_allocated > LOCAL_THRESHOLD) GC_collect_local();

        ObjectHeader **prev = &me->local_free_list, *cur = me->local_free_list;
        while (cur) {
            if (cur->size >= size) {
                *prev = cur->next_free;
                cur->magic = MAGIC_LIVE; cur->is_shared = 0; cur->marked = 0;
                me->local_allocated += total;
                return (void*)(cur + 1);
            }
            prev = &cur->next_free; cur = cur->next_free;
        }

        void* mem; 
        if (posix_memalign(&mem, 8, CHUNK_SIZE) != 0) return NULL;
        memset(mem, 0, CHUNK_SIZE);

        ChunkNode* node = malloc(sizeof(ChunkNode));
        node->start = mem; node->size = CHUNK_SIZE; 
        node->next = me->local_chunks; me->local_chunks = node; 

        me->local_tlab.start = mem; 
        me->local_tlab.cursor = (char*)mem + total; 
        me->local_tlab.end = (char*)mem + CHUNK_SIZE;

        ObjectHeader* h = (ObjectHeader*)mem;
        h->size = size; h->is_shared = 0; h->magic = MAGIC_LIVE; h->marked = 0;
        me->local_allocated += total;
        return (void*)(h + 1);

    } else {
        acquire_global_gc_lock();

        if (global_tlab.cursor && global_tlab.cursor + total <= global_tlab.end) {
            ObjectHeader* h = (ObjectHeader*)global_tlab.cursor;
            h->size = size; h->is_shared = 1; h->magic = MAGIC_LIVE; h->marked = 0;
            global_tlab.cursor += total;
            atomic_fetch_add(&shared_allocated, total);
            pthread_spin_unlock(&global_gc_lock);
            return (void*)(h + 1);
        }

        if (atomic_load(&shared_allocated) > SHARED_THRESHOLD) {
            pthread_spin_unlock(&global_gc_lock);
            GC_collect_global();
            acquire_global_gc_lock();
        }

        ObjectHeader **prev = &global_free_list, *cur = global_free_list;
        while (cur) {
            if (cur->size >= size) {
                *prev = cur->next_free;
                cur->magic = MAGIC_LIVE; cur->is_shared = 1; cur->marked = 0;
                atomic_fetch_add(&shared_allocated, total);
                pthread_spin_unlock(&global_gc_lock);
                return (void*)(cur + 1);
            }
            prev = &cur->next_free; cur = cur->next_free;
        }

        void* mem; 
        if (posix_memalign(&mem, 8, CHUNK_SIZE) != 0) {
            pthread_spin_unlock(&global_gc_lock);
            return NULL;
        }
        memset(mem, 0, CHUNK_SIZE);

        ChunkNode* node = malloc(sizeof(ChunkNode));
        node->start = mem; node->size = CHUNK_SIZE; 
        node->next = global_chunks; global_chunks = node; 

        global_tlab.start = mem; 
        global_tlab.cursor = (char*)mem + total; 
        global_tlab.end = (char*)mem + CHUNK_SIZE;

        ObjectHeader* h = (ObjectHeader*)mem;
        h->size = size; h->is_shared = 1; h->magic = MAGIC_LIVE; h->marked = 0;
        atomic_fetch_add(&shared_allocated, total);
        
        pthread_spin_unlock(&global_gc_lock);
        return (void*)(h + 1);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * 10. 초기화 API
 * ═══════════════════════════════════════════════════════════════════ */
extern void GC_unregister_thread(void);
static void gc_thread_destructor(void* arg) { GC_unregister_thread(); }

void GC_register_thread(void) {
    if (local_thread_index != -1) return;
    acquire_global_gc_lock();
    
    if (local_thread_index == -1 && thread_count < MAX_THREADS) {
        int idx = thread_count++; registered_count++;

        thread_table[idx].tid = pthread_self();
        thread_table[idx].stack_bottom = __builtin_frame_address(0);
        thread_table[idx].stack_top = NULL;
        thread_table[idx].active = 1;
        atomic_store_explicit(&thread_table[idx].state, THREAD_RUNNING, memory_order_release);
        
        thread_table[idx].local_chunks = NULL;
        thread_table[idx].local_free_list = NULL;
        thread_table[idx].local_tlab.cursor = NULL;
        thread_table[idx].local_allocated = 0;

        local_thread_index = idx;
        pthread_setspecific(gc_thread_key, (void*)(intptr_t)1);
    }
    pthread_spin_unlock(&global_gc_lock);
}

void GC_unregister_thread(void) {
    if (local_thread_index < 0) return;
    acquire_global_gc_lock();
    thread_table[local_thread_index].active = 0;
    registered_count--;
    pthread_spin_unlock(&global_gc_lock);
    local_thread_index = -1;
}

void GC_init(void) {
    pthread_spin_init(&global_gc_lock, PTHREAD_PROCESS_PRIVATE);
    pthread_key_create(&gc_thread_key, gc_thread_destructor);
    GC_register_thread();
}