#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <sys/time.h> /* clock() 대신 gettimeofday() 사용 */
#include <unistd.h>

/* =========================================================================
 * 1. GC 엔진 추상화 레이어 (매크로 매핑)
 * ========================================================================= */
#if defined(USE_BOEHM_GC)
    #include <gc.h>
    #define INIT_GC()                          GC_INIT()
    #define ALLOC_LOCAL(size)                  GC_MALLOC(size)
    #define ALLOC_SHARED(size)                 GC_MALLOC(size)
    #define WRITE_BARRIER(obj, field_ptr, val) (*(field_ptr) = (val))
    #define FORCE_GC()                         GC_gcollect()
    #define ENGINE_NAME                        "Commercial Boehm GC"
    #define pthread_create                     GC_pthread_create
    #define pthread_join                       GC_pthread_join

#elif defined(USE_THREAD_LOCAL_GC)
    #include "simple_gc.h"
    #define INIT_GC()                          GC_init()
    #define ALLOC_LOCAL(size)                  GC_malloc((size), 0)
    #define ALLOC_SHARED(size)                 GC_malloc((size), 1)
    #define WRITE_BARRIER(obj, field_ptr, val) GC_write_barrier((obj), (field_ptr), (val))
    #define FORCE_GC()                         GC_collect()
    #define ENGINE_NAME                        "Custom Thread-Local GC (New)"

#else
    #include "simple_gc.h"
    #define INIT_GC()                          GC_init()
    #define ALLOC_LOCAL(size)                  GC_malloc(size)
    #define ALLOC_SHARED(size)                 GC_malloc(size)
    #define WRITE_BARRIER(obj, field_ptr, val) (*(field_ptr) = (val))
    #define FORCE_GC()                         GC_collect()
    #define ENGINE_NAME                        "Custom Global STW GC (Old)"
#endif

#define NUM_THREADS 12         
#define ALLOCS_PER_THREAD 20000 

void blackhole(void* p) { __asm__ __volatile__ ("" : : "g"(p) : "memory"); }

void* volatile global_root[NUM_THREADS];

void* thread_work(void* arg) {
    int tid = *(int*)arg;
    printf("스레드 %d: 할당 시작...\n", tid);

    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        int* p = NULL;

        /* --- 할당 로직 분기 --- */
        if (i == ALLOCS_PER_THREAD - 1) {
            p = (int*)ALLOC_SHARED(sizeof(int)); // 마지막 객체는 Shared 할당
        } else {
            p = (int*)ALLOC_LOCAL(sizeof(int));  // 나머지는 Local 할당
        }

        if (p) *p = tid * 100000 + i;

        if (i == ALLOCS_PER_THREAD - 1) {
            /* 전역 변수 대입 (매크로를 통해 자동으로 쓰기 장벽 적용) */
            WRITE_BARRIER(NULL, (void**)&global_root[tid], p);
        } else {
            if (i % 1000 == 0) blackhole(p);
        }
    }

    printf("스레드 %d: 할당 완료.\n", tid);
    return NULL;
}

int main() {
    INIT_GC();
    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];

    printf("--- [%s] 부하 테스트 ---\n", ENGINE_NAME);

    /* 현실 시간 측정 시작 */
    struct timeval start, end;
    gettimeofday(&start, NULL);

    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, thread_work, &thread_ids[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("\n모든 작업 완료. 최종 GC 실행 중...\n");
    FORCE_GC(); /* 구버전/신버전/Boehm 자동 매핑 */

    /* 현실 시간 측정 종료 */
    gettimeofday(&end, NULL);
    double total_time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;

    printf("\n--- 결과 ---\n");
    printf("총 소요 시간(Wall-clock): %.4f 초\n", total_time);
    
    for (int i = 0; i < NUM_THREADS; i++) {
        if (global_root[i]) {
            printf("스레드 %d 데이터 확인: %d (성공)\n", i, *(int*)global_root[i]);
        } else {
            printf("스레드 %d 데이터 유실! (실패)\n", i);
        }
    }
    return 0;
}
