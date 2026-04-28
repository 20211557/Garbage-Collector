#define _GNU_SOURCE
#include <stdio.h>
#include <sys/time.h> /* gettimeofday() 사용 */

/* =========================================================================
 * 1. GC 엔진 추상화 레이어 (매크로 매핑)
 * ========================================================================= */
#if defined(USE_BOEHM_GC)
    #include <gc.h>
    #define INIT_GC()                          GC_INIT()
    #define ALLOC_LOCAL(size)                  GC_MALLOC(size)
    #define FORCE_GC_LOCAL()                   GC_gcollect()
    #define ENGINE_NAME                        "Commercial Boehm GC"
#elif defined(USE_THREAD_LOCAL_GC)
    #include "simple_gc.h"
    #define INIT_GC()                          GC_init()
    #define ALLOC_LOCAL(size)                  GC_malloc((size), 0)
    #define FORCE_GC_LOCAL()                   GC_collect_local()
    #define ENGINE_NAME                        "Custom Thread-Local GC (New)"
#else
    #include "simple_gc.h"
    #define INIT_GC()                          GC_init()
    #define ALLOC_LOCAL(size)                  GC_malloc(size)
    #define FORCE_GC_LOCAL()                   GC_collect()
    #define ENGINE_NAME                        "Custom Global STW GC (Old)"
#endif

// 컴파일러 최적화 방지용
void blackhole(void* p) { __asm__ __volatile__ ("" : : "g"(p) : "memory"); }

int main() {
    INIT_GC();

    const int OBJECT_COUNT = 50000; 

    printf("--- [%s] Heavy Workload 테스트 ---\n", ENGINE_NAME);
    printf("%d개의 객체를 할당합니다...\n", OBJECT_COUNT);

    for (int i = 0; i < OBJECT_COUNT; i++) {
        void* p = ALLOC_LOCAL(8); 
        
        if (i % 10000 == 0) printf("%d개 할당 완료...\n", i);
        blackhole(p);
    }

    printf("할당 완료. 이제 수동으로 GC를 실행합니다.\n");

    /* GC 순수 지연 시간(Pause Time) 측정 시작 */
    struct timeval start, end;
    gettimeofday(&start, NULL);

    /* 버전에 맞게 Local 또는 Global GC 호출 */
    FORCE_GC_LOCAL();

    /* 측정 종료 */
    gettimeofday(&end, NULL);
    double elapsed_time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;

    printf("\n--- 결과 ---\n");
    printf("수동 GC 순수 소요 시간: %.6f 초\n", elapsed_time);
    printf("---------------------------\n");

    return 0;
}
