#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>

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
    /* pthread_create를 Boehm이 스택을 추적할 수 있는 래퍼로 교체 */
    #define pthread_create                     GC_pthread_create
    #define pthread_join                       GC_pthread_join

#elif defined(USE_THREAD_LOCAL_GC)
    #include "simple_gc.h"
    #define INIT_GC()                          GC_init()
    #define ALLOC_LOCAL(size)                  GC_malloc((size), 0)
    #define ALLOC_SHARED(size)                 GC_malloc((size), 1)
    #define WRITE_BARRIER(obj, field_ptr, val) GC_write_barrier((obj), (field_ptr), (val))
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
 * 2. 시뮬레이션 환경 설정
 * ========================================================================= */
#define NUM_THREADS 8
#define REQUESTS_PER_THREAD 100000 /* 총 80만 건의 트래픽 */

void blackhole(void* p) { __asm__ __volatile__ ("" : : "g"(p) : "memory"); }

typedef struct JsonNode {
    char key[16];
    int value;
    struct JsonNode* next;
} JsonNode;

typedef struct HttpResponse {
    int status_code;
    char body[64];
} HttpResponse;

void* volatile global_session_cache[NUM_THREADS];

/* =========================================================================
 * 3. 워커 스레드 (요청 처리 로직)
 * ========================================================================= */
void* server_worker(void* arg) {
    int tid = *(int*)arg;
    
    for (int req = 0; req < REQUESTS_PER_THREAD; req++) {
        /* [1] 임시 JSON 파싱 (Local) */
        JsonNode* head = NULL;
        for (int i = 0; i < 10; i++) {
            JsonNode* node = (JsonNode*)ALLOC_LOCAL(sizeof(JsonNode));
            if (node) {
                snprintf(node->key, sizeof(node->key), "field_%d", i);
                node->value = req * i;
                node->next = head;
                head = node;
            }
        }

        /* [2] 임시 응답 객체 (Local) */
        HttpResponse* res = (HttpResponse*)ALLOC_LOCAL(sizeof(HttpResponse));
        if (res) {
            res->status_code = 200;
            snprintf(res->body, sizeof(res->body), "{\"status\":\"ok\", \"id\":%d}", req);
            blackhole(res); 
        }

        /* [3] 전역 상태 업데이트 (Shared, 1% 확률) */
        if (req % 100 == 0) {
            int* session_data = (int*)ALLOC_SHARED(sizeof(int));
            if (session_data) *session_data = req;

            /* 쓰기 장벽을 통한 안전한 대입 (Boehm/구버전은 단순 대입으로 치환됨) */
            WRITE_BARRIER(NULL, (void**)&global_session_cache[tid], session_data);
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

    printf("====================================================\n");
    printf(" 벤치마크 엔진: %s\n", ENGINE_NAME);
    printf("====================================================\n");

    struct timeval start, end;
    gettimeofday(&start, NULL);

    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, server_worker, &thread_ids[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    FORCE_GC(); 

    gettimeofday(&end, NULL);
    double total_time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;

    printf("\n--- 시뮬레이션 결과 ---\n");
    printf("처리한 총 요청 수: %d 건\n", NUM_THREADS * REQUESTS_PER_THREAD);
    printf("총 소요 시간(Wall-clock): %.4f 초\n", total_time);
    printf("초당 처리량(TPS): %.0f req/sec\n", (NUM_THREADS * REQUESTS_PER_THREAD) / total_time);
    printf("----------------------------------------------------\n");
    
    return 0;
}