#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>

/* =========================================================================
 * 1. GC 엔진 추상화 레이어
 * ========================================================================= */
#if defined(USE_BOEHM_GC)
    #define GC_THREADS /* 스택 추적을 위한 필수 선언 */
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
    #define WRITE_BARRIER(obj, field_ptr, val) GC_write_barrier((obj), (void**)(field_ptr), (void*)(val))
    #define FORCE_GC()                         GC_collect_global()
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

/* =========================================================================
 * 2. 공유 메시지 큐 시스템 설정
 * ========================================================================= */
#define NUM_THREADS 8
#define MESSAGES_PER_THREAD 50000 /* 각 스레드가 5만 개의 메시지를 다른 스레드에게 발송 */

void blackhole(void* p) { __asm__ __volatile__ ("" : : "g"(p) : "memory"); }

/* 네트워크 패킷이나 메시지를 모방한 구조체 */
typedef struct MessageNode {
    int sender_id;
    int data_payload[16];
    struct MessageNode* next;
} MessageNode;

/* 스레드 간 통신을 위한 8개의 전역 우체통(Queue)과 락 */
MessageNode* volatile global_mailboxes[NUM_THREADS];
pthread_spinlock_t mailbox_locks[NUM_THREADS];

/* =========================================================================
 * 3. 워커 스레드: 우체부이자 수신자
 * ========================================================================= */
void* message_worker(void* arg) {
    int tid = *(int*)arg;
    
    for (int i = 0; i < MESSAGES_PER_THREAD; i++) {
        
        /* ---------------------------------------------------------
         * [쓰기] 다른 스레드에게 보낼 메시지 생성
         * 남의 우체통에 넣어야 하므로 무조건 SHARED로 할당해야 합니다!
         * --------------------------------------------------------- */
        MessageNode* msg = (MessageNode*)ALLOC_SHARED(sizeof(MessageNode));
        if (msg) {
            msg->sender_id = tid;
            msg->data_payload[0] = i;
            
            /* 타겟 스레드 결정 (라운드 로빈으로 골고루 분배) */
            int target_tid = (tid + i) % NUM_THREADS;

            /* 우체통 락을 걸고 메시지 삽입 (연결 리스트의 맨 앞에 추가) */
            pthread_spin_lock(&mailbox_locks[target_tid]);
            
            /* msg->next가 기존 리스트를 가리키게 함 */
            WRITE_BARRIER(msg, &msg->next, global_mailboxes[target_tid]);
            /* 전역 우체통이 새로운 msg를 가리키게 함 */
            WRITE_BARRIER(NULL, &global_mailboxes[target_tid], msg);
            
            pthread_spin_unlock(&mailbox_locks[target_tid]);
        }

        /* ---------------------------------------------------------
         * [읽기 & 폐기] 내 우체통에 쌓인 메시지를 읽고 버림
         * 10번 보낼 때마다 한 번씩 내 우체통을 통째로 비웁니다.
         * 비워진 메시지들은 참조를 잃고 거대한 가비지(쓰레기) 더미가 됩니다.
         * --------------------------------------------------------- */
        if (i % 10 == 0) {
            pthread_spin_lock(&mailbox_locks[tid]);
            MessageNode* my_mail = global_mailboxes[tid];
            
            /* 전역 참조를 끊어버림 -> my_mail에 연결된 수백 개의 노드가 일제히 쓰레기가 됨 */
            WRITE_BARRIER(NULL, &global_mailboxes[tid], NULL);
            pthread_spin_unlock(&mailbox_locks[tid]);

            /* 메시지 소비 흉내 */
            while (my_mail) {
                blackhole(my_mail);
                my_mail = my_mail->next;
            }
        }
    }
    return NULL;
}

/* =========================================================================
 * 4. 메인 실행부
 * ========================================================================= */
int main() {
    INIT_GC();
    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_spin_init(&mailbox_locks[i], PTHREAD_PROCESS_PRIVATE);
        global_mailboxes[i] = NULL;
    }

    printf("====================================================\n");
    printf(" [가혹 테스트] 크로스 스레드 메시징 (100%% Shared)\n");
    printf(" 벤치마크 엔진: %s\n", ENGINE_NAME);
    printf("====================================================\n");

    struct timeval start, end;
    gettimeofday(&start, NULL);

    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, message_worker, &thread_ids[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    FORCE_GC(); 

    gettimeofday(&end, NULL);
    double total_time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;

    printf("\n--- 시뮬레이션 결과 ---\n");
    printf("교환된 총 메시지 수: %d 건\n", NUM_THREADS * MESSAGES_PER_THREAD);
    printf("총 소요 시간(Wall-clock): %.4f 초\n", total_time);
    printf("----------------------------------------------------\n");
    
    return 0;
}