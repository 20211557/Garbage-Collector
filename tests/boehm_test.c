#define GC_THREADS
#include <gc.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define NUM_THREADS 12         // 동시에 실행할 스레드 수
#define ALLOCS_PER_THREAD 20000 // 스레드당 할당 횟수

// 컴파일러 최적화 방지용
void blackhole(void* p) { __asm__ __volatile__ ("" : : "g"(p) : "memory"); }

// 모든 스레드가 공유하는 전역 포인터 (GC가 살려야 함)
void* volatile global_root[NUM_THREADS];

void* thread_work(void* arg) {
    int tid = *(int*)arg;
    printf("스레드 %d: 할당 시작...\n", tid);

    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        // 작은 메모리 조각들을 계속 할당
        int* p = (int*)GC_MALLOC(sizeof(int));
        if (p) *p = tid * 100000 + i;

        // 마지막에 할당한 객체는 전역 루트에 저장하여 생존시키기
        if (i == ALLOCS_PER_THREAD - 1) {
            global_root[tid] = p;
        }

        // 아주 가끔씩만 blackhole 호출
        if (i % 1000 == 0) blackhole(p);
    }

    printf("스레드 %d: 할당 완료.\n", tid);
    return NULL;
}

int main() {
    GC_INIT();
    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];

    printf("--- 멀티스레드 GC 부하 테스트 시작 ---\n");
    printf("스레드 수: %d, 스레드당 할당: %d\n", NUM_THREADS, ALLOCS_PER_THREAD);
    printf("총 예상 할당 객체: %d개\n", NUM_THREADS * ALLOCS_PER_THREAD);

    clock_t start_time = clock();

    // 1. 스레드 생성
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, thread_work, &thread_ids[i]);
    }

    // 2. 모든 스레드 종료 대기
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    // 3. 마지막 강제 GC 실행
    printf("\n모든 스레드 작업 완료. 최종 GC 실행 중...\n");
    GC_gcollect();

    clock_t end_time = clock();
    double total_time = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;

    printf("\n--- 결과 ---\n");
    printf("총 소요 시간: %.4f 초\n", total_time);
    
    // 전역 변수에 저장된 데이터가 살아있는지 검증
    for (int i = 0; i < NUM_THREADS; i++) {
        if (global_root[i]) {
            printf("스레드 %d의 마지막 데이터 확인: %d (성공)\n", i, *(int*)global_root[i]);
        } else {
            printf("스레드 %d의 데이터 유실! (실패)\n", i);
        }
    }
    printf("---------------------------\n");

    return 0;
}