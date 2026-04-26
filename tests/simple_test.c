#include "simple_gc.h"
#include <stdio.h>
#include <sys/time.h> /* gettimeofday() 사용 */

// 컴파일러 최적화 방지용
void blackhole(void* p) { __asm__ __volatile__ ("" : : "g"(p) : "memory"); }

int main() {
    GC_init();

    const int OBJECT_COUNT = 50000; 

#ifdef USE_THREAD_LOCAL_GC
    printf("--- [신버전] Thread-Local GC Heavy Workload 테스트 ---\n");
#else
    printf("--- [구버전] Global Parallel GC Heavy Workload 테스트 ---\n");
#endif

    printf("%d개의 객체를 할당합니다...\n", OBJECT_COUNT);

    for (int i = 0; i < OBJECT_COUNT; i++) {
#ifdef USE_THREAD_LOCAL_GC
        void* p = GC_malloc(8, 0); // 로컬 메모리에만 할당
#else
        void* p = GC_malloc(8);    // 구버전 할당
#endif
        
        if (i % 10000 == 0) printf("%d개 할당 완료...\n", i);
        blackhole(p);
    }

    printf("할당 완료. 이제 수동으로 GC를 실행합니다.\n");

    /* GC 순수 지연 시간(Pause Time) 측정 시작 */
    struct timeval start, end;
    gettimeofday(&start, NULL);

#ifdef USE_THREAD_LOCAL_GC
    /* 신버전은 STW 없이 내 스택만 조용히 훑는 Local GC 호출 */
    GC_collect_local();
#else
    /* 구버전은 STW를 걸고 모든 영역을 훑는 Global GC 호출 */
    GC_collect();
#endif

    /* 측정 종료 */
    gettimeofday(&end, NULL);
    double elapsed_time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;

    printf("\n--- 결과 ---\n");
    printf("수동 GC 순수 소요 시간: %.6f 초\n", elapsed_time);
    printf("---------------------------\n");

    return 0;
}
