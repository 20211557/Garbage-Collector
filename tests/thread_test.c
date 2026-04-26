#include "simple_gc.h"
#include <stdio.h>
#include <pthread.h>
#include <sys/time.h> /* clock() 대신 gettimeofday() 사용 */
#include <unistd.h>

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
#ifdef USE_THREAD_LOCAL_GC
        if (i == ALLOCS_PER_THREAD - 1) {
            p = (int*)GC_malloc(sizeof(int), 1); // Shared 할당
        } else {
            p = (int*)GC_malloc(sizeof(int), 0); // Local 할당
        }
#else
        p = (int*)GC_malloc(sizeof(int));        // 구버전 단일 할당
#endif

        if (p) *p = tid * 100000 + i;

        if (i == ALLOCS_PER_THREAD - 1) {
            /* 전역 변수 대입 (구버전은 단순 대입, 신버전은 쓰기 장벽 매크로 동작) */
            GC_write_barrier(NULL, (void**)&global_root[tid], p);
        } else {
            if (i % 1000 == 0) blackhole(p);
        }
    }

    printf("스레드 %d: 할당 완료.\n", tid);
    return NULL;
}

int main() {
    GC_init();
    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];

#ifdef USE_THREAD_LOCAL_GC
    printf("--- [신버전] Thread-Local GC 부하 테스트 ---\n");
#else
    printf("--- [구버전] Global Parallel GC 부하 테스트 ---\n");
#endif

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
    GC_collect(); /* 구버전/신버전 자동 매핑 */

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
