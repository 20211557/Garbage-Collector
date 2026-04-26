#define _GNU_SOURCE
#include "simple_gc.h"
#include "avl_tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <pthread.h>

// --- 전역 상태 관리 ---

static AVLNode* active_root = NULL;    // 현재 할당된 블록 (AVL 트리)
static AVLNode* survivor_root = NULL;  // 생존 확인된 블록 (AVL 트리)

static void* stack_bottom = NULL;
static size_t total_allocated = 0;
static const size_t GC_THRESHOLD = 1024 * 1024; // 1MB

// 동기화를 위한 POSIX 뮤텍스
static pthread_mutex_t gc_lock;

// 리눅스 데이터 영역 심볼
extern char etext, end;

// --- 내부 헬퍼 함수 ---

// 생존한 블록 내부를 다시 스캔하여 연결된 다른 블록을 찾는 함수 (재귀)
static void scan_survivor_tree(AVLNode* node) {
    if (!node) return;
    
    // 현재 노드 내부 스캔
    extern void scan_region(void* start, void* end_ptr);
    scan_region(node->start_ptr, (char*)node->start_ptr + node->size);
    
    // 자식 노드들도 순회하며 스캔
    scan_survivor_tree(node->left);
    scan_survivor_tree(node->right);
}

// 발견된 포인터가 실제 할당된 블록인지 확인하고 이동
static void find_and_move(void* ptr) {
    AVLNode* target = avl_find_range(active_root, ptr);
    if (target) {
        void* s_ptr = target->start_ptr;
        size_t s_size = target->size;
        
        // Survivor 트리로 옮기고 Active 트리에서 제거
        // AVL 트리의 O(log N) 성능 덕분에 매우 빠름
        survivor_root = avl_insert(survivor_root, s_ptr, s_size, 0);
        active_root = avl_delete(active_root, s_ptr);
    }
}

// 메모리 영역 스캔 (8바이트 점프 최적화)
void scan_region(void* start, void* end_ptr) {
    if (start > end_ptr) { void* tmp = start; start = end_ptr; end_ptr = tmp; }
    
    // 8바이트 주소 정렬 (Align to 8 bytes)
    uintptr_t s = (uintptr_t)start;
    s = (s + 7) & ~7; 
    void** current = (void**)s;

    while ((void*)(current + 1) <= end_ptr) {
        find_and_move(*current);
        current++; // 8바이트씩 점프
    }
}

// --- 공용 API 구현 ---

void GC_init(void) {
    // 1. 스택 바텀 저장
    int dummy;
    stack_bottom = (void*)&dummy;
    
    // 2. 뮤텍스 초기화
    pthread_mutex_init(&gc_lock, NULL);
}

void GC_collect(void) {
    // 주의: 이 함수는 호출 시 이미 gc_lock이 잡혀 있어야 함 (또는 내부에서 STW 구현)
    
    // 1. 레지스터 덤프
    __builtin_unwind_init();
    jmp_buf env;
    setjmp(env);

    void* stack_top = __builtin_frame_address(0);
    
    // 2. Root Set 스캔 (Stack & Data)
    scan_region(stack_top, stack_bottom);
    scan_region(&etext, &end);

    // 3. Heap 재귀 스캔 (Transitive Closure)
    // Survivor 트리에 들어온 객체들이 가리키는 다른 객체들을 추적
    scan_survivor_tree(survivor_root);

    // 4. Sweep: active_root에 남은 노드들은 도달 불가능한 가비지
    // avl_free_all은 내부적으로 free()를 호출함
    avl_free_all(active_root);

    // 5. 상태 교체
    active_root = survivor_root;
    survivor_root = NULL;
    total_allocated = 0;
}

void* GC_malloc(size_t size) {
    // 멀티스레드 안전을 위해 뮤텍스 획득
    pthread_mutex_lock(&gc_lock);

    if (total_allocated > GC_THRESHOLD) {
        GC_collect();
    }

    // 8바이트 정렬된 메모리 할당
    void* ptr = NULL;
    if (posix_memalign(&ptr, 8, size) != 0) {
        pthread_mutex_unlock(&gc_lock);
        return NULL;
    }
    memset(ptr, 0, size);

    // AVL 트리에 메타데이터 삽입
    active_root = avl_insert(active_root, ptr, size, 0);
    total_allocated += size;

    pthread_mutex_unlock(&gc_lock);
    return ptr;
}