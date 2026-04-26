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
#define NUM_GC_WORKERS      4
#define QUEUE_CAPACITY      16384

typedef struct ObjectHeader {
    size_t               size;
    _Atomic uint32_t     marked;
    uint64_t             magic;
    struct ObjectHeader* next_free;
} ObjectHeader;

#define HEADER_SIZE ALIGN8(sizeof(ObjectHeader))

typedef struct { char *start, *cursor, *end; } TLAB;
static __thread TLAB local_tlab = {NULL, NULL, NULL};
static __thread int  local_thread_index = -1;

typedef struct ChunkNode {
    void* start;
    size_t size;
    struct ChunkNode* next;
} ChunkNode;

static ChunkNode chunk_pool[MAX_CHUNKS];
static atomic_int chunk_pool_next = 0;
static ChunkNode* global_chunks = NULL;
static _Atomic(ObjectHeader*) global_free_list = NULL;
static atomic_size_t total_heap_size = 0;
static pthread_spinlock_t gc_lock;

typedef struct { pthread_t tid; void *stack_bottom, *stack_top; int active; } ThreadInfo;
static ThreadInfo thread_table[MAX_THREADS];
static int thread_count = 0, registered_count = 0;
static atomic_int parked_count = 0, gc_phase = 0;

extern char etext, end;
static pthread_key_t gc_thread_key;

/* ═══════════════════════════════════════════════════════════════════
 * 병렬 GC 워커 및 동기화 구조
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct { void* data[QUEUE_CAPACITY]; int head, tail; } MarkQueue;
typedef struct { pthread_t tid; int worker_id; MarkQueue queue; } GCWorker;

static GCWorker workers[NUM_GC_WORKERS];
static pthread_barrier_t gc_start_barrier, gc_end_barrier, gc_sweep_barrier;
static atomic_int gc_work_count = 0;
static _Atomic(ChunkNode*) sweep_cursor = NULL;

static void push_queue(MarkQueue* q, void* ptr) {
    if (q->tail < QUEUE_CAPACITY) { q->data[q->tail++] = ptr; atomic_fetch_add(&gc_work_count, 1); }
}
static void* pop_queue(MarkQueue* q) {
    if (q->head < q->tail) {
        void* p = q->data[q->head++];
        if (q->head == q->tail) { q->head = 0; q->tail = 0; }
        atomic_fetch_sub(&gc_work_count, 1);
        return p;
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 * Signal 기반 STW (Zero CPU)
 * ═══════════════════════════════════════════════════════════════════ */
static void gc_thread_destructor(void* arg) { GC_unregister_thread(); }
static void resume_signal_handler(int signum) {}
static void suspend_signal_handler(int signum) {
    int idx = local_thread_index; if (idx < 0) return;
    volatile char sp_marker; thread_table[idx].stack_top = (void*)&sp_marker;
    atomic_fetch_add(&parked_count, 1);
    sigset_t mask; sigfillset(&mask); sigdelset(&mask, SIGUSR2); sigdelset(&mask, SIGINT);
    while (atomic_load_explicit(&gc_phase, memory_order_acquire) == 1) sigsuspend(&mask);
    atomic_fetch_sub(&parked_count, 1);
}

/* ═══════════════════════════════════════════════════════════════════
 * 병렬 Mark & Sweep 로직
 * ═══════════════════════════════════════════════════════════════════ */
static ChunkNode* find_chunk(void* ptr) {
    for (ChunkNode* c = global_chunks; c; c = c->next)
        if ((char*)ptr >= (char*)c->start && (char*)ptr < (char*)c->start + c->size) return c;
    return NULL;
}

static void find_and_mark_parallel(void* candidate, MarkQueue* my_queue) {
    if (!candidate || (uintptr_t)candidate % 8 != 0) return;
    ChunkNode* chunk = find_chunk(candidate);
    if (!chunk || (char*)candidate < (char*)chunk->start + HEADER_SIZE) return;
    ObjectHeader* h = (ObjectHeader*)((char*)candidate - HEADER_SIZE);
    if (h->magic != MAGIC_LIVE) return;

    uint32_t expected = 0;
    if (atomic_compare_exchange_strong_explicit(&h->marked, &expected, 1, memory_order_acq_rel, memory_order_relaxed))
        push_queue(my_queue, (void*)(h + 1));
}

static void scan_region_parallel(void* start, void* end_ptr, MarkQueue* my_queue) {
    if (!start || !end_ptr) return;
    if (start > end_ptr) { void* t = start; start = end_ptr; end_ptr = t; }
    void** cur = (void**)ALIGN8((uintptr_t)start);
    while ((char*)(cur + 1) <= (char*)end_ptr) {
        __builtin_prefetch(*(cur + 1), 0, 0);
        find_and_mark_parallel(*cur, my_queue); cur++;
    }
}

static void parallel_sweep_task(void) {
    ObjectHeader *l_head = NULL, *l_tail = NULL;
    while (1) {
        ChunkNode* chunk = atomic_load_explicit(&sweep_cursor, memory_order_acquire);
        if (!chunk) break;
        if (!atomic_compare_exchange_weak_explicit(&sweep_cursor, &chunk, chunk->next, memory_order_acq_rel, memory_order_acquire)) continue;

        char *p = (char*)chunk->start, *limit = p + chunk->size;
        while (p + HEADER_SIZE <= limit) {
            ObjectHeader* h = (ObjectHeader*)p;
            if (h->magic != MAGIC_LIVE) break;
            size_t total = ALIGN8(h->size + HEADER_SIZE);
            if (atomic_load_explicit(&h->marked, memory_order_relaxed)) atomic_store_explicit(&h->marked, 0, memory_order_relaxed);
            else {
                h->magic = MAGIC_DEAD; h->next_free = l_head; l_head = h;
                if (!l_tail) l_tail = h;
            }
            p += total;
        }
    }
    if (l_head) {
        ObjectHeader* old_h = atomic_load_explicit(&global_free_list, memory_order_acquire);
        do { l_tail->next_free = old_h; } 
        while (!atomic_compare_exchange_weak_explicit(&global_free_list, &old_h, l_head, memory_order_release, memory_order_acquire));
    }
}

static void* gc_worker_thread(void* arg) {
    GCWorker* me = (GCWorker*)arg;
    while (1) {
        pthread_barrier_wait(&gc_start_barrier);
        while (1) {
            void* obj = pop_queue(&me->queue);
            if (obj) scan_region_parallel(obj, (char*)obj + ((ObjectHeader*)obj-1)->size, &me->queue);
            else if (atomic_load_explicit(&gc_work_count, memory_order_acquire) == 0) break;
            else sched_yield();
        }
        pthread_barrier_wait(&gc_end_barrier);
        parallel_sweep_task();
        pthread_barrier_wait(&gc_sweep_barrier);
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 * 메인 GC 및 할당 API
 * ═══════════════════════════════════════════════════════════════════ */
void GC_collect(void) {
    pthread_spin_lock(&gc_lock);
    atomic_store_explicit(&gc_phase, 1, memory_order_release);
    pthread_t self = pthread_self();
    for (int i=0; i<thread_count; i++) if (thread_table[i].active && thread_table[i].tid != self) pthread_kill(thread_table[i].tid, SIGUSR1);
    while (atomic_load_explicit(&parked_count, memory_order_acquire) < registered_count - 1) sched_yield();
    
    if (local_thread_index >= 0) { volatile char sp; thread_table[local_thread_index].stack_top = (void*)&sp; }
    
    atomic_store(&gc_work_count, 0);
    scan_region_parallel(&etext, &end, &workers[0].queue);
    for (int i=0, w=0; i<thread_count; i++) {
        if (!thread_table[i].active) continue;
        scan_region_parallel(thread_table[i].stack_top, thread_table[i].stack_bottom, &workers[w++ % NUM_GC_WORKERS].queue);
    }

    atomic_store(&global_free_list, NULL);
    atomic_store(&sweep_cursor, global_chunks);
    
    pthread_barrier_wait(&gc_start_barrier);
    pthread_barrier_wait(&gc_end_barrier);
    pthread_barrier_wait(&gc_sweep_barrier);

    atomic_store_explicit(&gc_phase, 0, memory_order_release);
    for (int i=0; i<thread_count; i++) if (thread_table[i].active && thread_table[i].tid != self) pthread_kill(thread_table[i].tid, SIGUSR2);
    while (atomic_load_explicit(&parked_count, memory_order_acquire) > 0) sched_yield();
    pthread_spin_unlock(&gc_lock);
}

void* GC_malloc(size_t size) {
    if (local_thread_index == -1) GC_register_thread();
    size = ALIGN8(size == 0 ? 8 : size);
    size_t total = ALIGN8(size + HEADER_SIZE);

    if (local_tlab.cursor && local_tlab.cursor + total <= local_tlab.end) {
        ObjectHeader* h = (ObjectHeader*)local_tlab.cursor;
        h->size = size; atomic_init(&h->marked, 0); h->magic = MAGIC_LIVE;
        local_tlab.cursor += total; return (void*)(h + 1);
    }

    if (atomic_load(&total_heap_size) + TLAB_SIZE > GC_THRESHOLD) GC_collect();

    pthread_spin_lock(&gc_lock);
    ObjectHeader *old_h = atomic_load(&global_free_list), *cur = old_h, *prev = NULL;
    while (cur) {
        if (cur->size >= size) {
            if (prev) prev->next_free = cur->next_free; else atomic_store(&global_free_list, cur->next_free);
            cur->magic = MAGIC_LIVE; atomic_init(&cur->marked, 0);
            pthread_spin_unlock(&gc_lock); return (void*)(cur + 1);
        }
        prev = cur; cur = cur->next_free;
    }
    pthread_spin_unlock(&gc_lock);

    void* mem; if (posix_memalign(&mem, 8, TLAB_SIZE) != 0) return NULL;
    memset(mem, 0, TLAB_SIZE);
    ChunkNode* node = &chunk_pool[atomic_fetch_add(&chunk_pool_next, 1)];
    node->start = mem; node->size = TLAB_SIZE;
    pthread_spin_lock(&gc_lock);
    node->next = global_chunks; global_chunks = node;
    atomic_fetch_add(&total_heap_size, TLAB_SIZE);
    local_tlab.start = mem; local_tlab.cursor = (char*)mem + total; local_tlab.end = (char*)mem + TLAB_SIZE;
    ObjectHeader* h = (ObjectHeader*)mem; h->size = size; atomic_init(&h->marked, 0); h->magic = MAGIC_LIVE;
    pthread_spin_unlock(&gc_lock);
    return (void*)(h + 1);
}

void GC_register_thread(void) {
    pthread_spin_lock(&gc_lock);
    if (local_thread_index == -1 && thread_count < MAX_THREADS) {
        int idx = thread_count++; registered_count++;
        thread_table[idx] = (ThreadInfo){pthread_self(), __builtin_frame_address(0), NULL, 1};
        local_thread_index = idx; pthread_setspecific(gc_thread_key, (void*)1);
    }
    pthread_spin_unlock(&gc_lock);
}

void GC_unregister_thread(void) {
    pthread_spin_lock(&gc_lock);
    if (local_thread_index >= 0) { thread_table[local_thread_index].active = 0; registered_count--; local_thread_index = -1; }
    pthread_spin_unlock(&gc_lock);
}

void GC_init(void) {
    pthread_spin_init(&gc_lock, 0);
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = suspend_signal_handler; sa.sa_flags = SA_RESTART; sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = resume_signal_handler; sigaction(SIGUSR2, &sa, NULL);
    pthread_key_create(&gc_thread_key, gc_thread_destructor);
    pthread_barrier_init(&gc_start_barrier, NULL, NUM_GC_WORKERS + 1);
    pthread_barrier_init(&gc_end_barrier, NULL, NUM_GC_WORKERS + 1);
    pthread_barrier_init(&gc_sweep_barrier, NULL, NUM_GC_WORKERS + 1);
    for (int i=0; i<NUM_GC_WORKERS; i++) {
        workers[i].worker_id = i; workers[i].queue.head = workers[i].queue.tail = 0;
        pthread_create(&workers[i].tid, NULL, gc_worker_thread, &workers[i]);
    }
    GC_register_thread();
}