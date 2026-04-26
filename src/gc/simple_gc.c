#define _GNU_SOURCE
#include "simple_gc.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>

typedef struct GCBlock {
    void* start_ptr;
    size_t size;
    struct GCBlock *prev;
    struct GCBlock *next;
} GCBlock;

static GCBlock* active_list = NULL;
static GCBlock* survivor_list = NULL;
static void* stack_bottom = NULL;
static size_t total_allocated = 0;
static const size_t GC_THRESHOLD = 1024 * 1024; 

extern char etext, end; 

static void add_to_list(GCBlock** list_head, GCBlock* new_block) {
    new_block->next = *list_head;
    new_block->prev = NULL;
    if (*list_head != NULL) (*list_head)->prev = new_block;
    *list_head = new_block;
}

static void move_block(GCBlock** from_head, GCBlock** to_head, GCBlock* block) {
    if (block->prev) block->prev->next = block->next;
    if (block->next) block->next->prev = block->prev;
    if (*from_head == block) *from_head = block->next;
    add_to_list(to_head, block);
}

static void find_and_move(void* ptr) {
    uintptr_t p = (uintptr_t)ptr;
    GCBlock* curr = active_list;
    while (curr) {
        GCBlock* next_temp = curr->next;
        uintptr_t start = (uintptr_t)curr->start_ptr;
        if (p >= start && p < start + curr->size) {
            move_block(&active_list, &survivor_list, curr);
            return; 
        }
        curr = next_temp;
    }
}

static void scan_region(void* start, void* end_ptr) {
    if (start > end_ptr) { void* tmp = start; start = end_ptr; end_ptr = tmp; }
    
    // 1바이트씩 이동하며 모든 8바이트 윈도우를 검사 (가장 보수적인 방식)
    char* current = (char*)start;
    // 경계 조건을 조금 더 넉넉하게 설정
    while ((void*)(current + sizeof(void*)) <= end_ptr) {
        void* ptr;
        memcpy(&ptr, current, sizeof(void*));
        find_and_move(ptr);
        current++; 
    }
}

void GC_init(void) {
    // stack_bottom을 조금 더 높게(이전 프레임) 설정하기 위해 dummy 변수 주소 활용
    int dummy;
    stack_bottom = (void*)&dummy;
}

void GC_collect(void) {
    if (!stack_bottom) return;

    // 1. 모든 레지스터를 스택으로 강제 플러시 (GCC 내장 함수)
    __builtin_unwind_init();
    
    jmp_buf env;
    setjmp(env);

    // 2. 현재 스택의 Top 구하기
    void* stack_top = __builtin_frame_address(0);
    
    // 3. 스택 스캔 (상하 128바이트씩 여유를 줌)
    scan_region((char*)stack_top - 128, (char*)stack_bottom + 128);
    
    // 4. 데이터 영역 스캔
    scan_region(&etext, &end);

    // 5. 힙 재귀 스캔 (Transitive Closure)
    GCBlock* curr = survivor_list;
    while (curr) {
        scan_region(curr->start_ptr, (char*)curr->start_ptr + curr->size);
        curr = curr->next;
    }

    // 6. Sweep
    GCBlock* g = active_list;
    while (g) {
        GCBlock* next = g->next;
        free(g->start_ptr);
        free(g);
        g = next;
    }

    active_list = survivor_list;
    survivor_list = NULL;
    total_allocated = 0;
}

void* GC_malloc(size_t size) {
    if (total_allocated > GC_THRESHOLD) GC_collect();
    void* ptr = malloc(size);
    if (!ptr) return NULL;
    GCBlock* block = (GCBlock*)calloc(1, sizeof(GCBlock));
    block->start_ptr = ptr;
    block->size = size;
    add_to_list(&active_list, block);
    total_allocated += size;
    return ptr;
}
