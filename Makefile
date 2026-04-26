
INC_DIR = include
SRC_DIR = src
TEST_DIR = tests


CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -D_GNU_SOURCE -I$(INC_DIR)/gc -I$(INC_DIR)/utils
LDFLAGS = -lpthread


UTIL_SRC = $(SRC_DIR)/utils/avl_tree.c $(SRC_DIR)/utils/bitmap.c

SIMPLE_GC_SRC = $(SRC_DIR)/gc/simple_gc.c
ADVANCED_GC_SRC = $(SRC_DIR)/gc/advanced_gc.c $(SRC_DIR)/utils/avl_tree.c
TLAB_GC_SRC = $(SRC_DIR)/gc/tlab_gc.c 
TLAB_ADVANCED_GC_SRC = $(SRC_DIR)/gc/tlab_advanced_gc.c 
TLAB_EXTREME_GC_SRC = $(SRC_DIR)/gc/tlab_extreme_gc.c 


SIMPLE_TEST = $(TEST_DIR)/simple_test.c
THREAD_TEST = $(TEST_DIR)/thread_test.c
BOEHM_TEST = $(TEST_DIR)/boehm_test.c
SERVER_SIM_TEST = $(TEST_DIR)/server_sim_test.c
SHARED_GRAPH_TEST = $(TEST_DIR)/shared_graph_test.c

# 기본 타겟
all: advanced

# --- 주요 타겟 규칙 ---

# 1. Simple GC 버전 
simple:
	$(CC) $(CFLAGS) -o simple_gc_test $(SIMPLE_TEST) $(SIMPLE_GC_SRC) $(LDFLAGS)
	@echo "Built: simple_gc_test"

# 2. Advanced GC 버전 (AVL 기반) [cite: 2]
advanced:
	$(CC) $(CFLAGS) -o advanced_gc_test $(THREAD_TEST) $(ADVANCED_GC_SRC) $(LDFLAGS)
	@echo "Built: advanced_gc_test"

# 3. TLAB GC 버전 [cite: 3]
tlab:
	$(CC) $(CFLAGS) -o tlab_gc_test $(THREAD_TEST) $(TLAB_GC_SRC) $(LDFLAGS)
	@echo "Built: tlab_gc_test"

# 4. TLAB Advanced GC 버전 [cite: 4]
tlab_advanced:
	$(CC) $(CFLAGS) -o tlab_advanced_gc_test $(THREAD_TEST) $(TLAB_ADVANCED_GC_SRC) $(LDFLAGS)
	@echo "Built: tlab_advanced_gc_test"

# 5. TLAB Extreme GC 버전 (Aggressive 최적화) [cite: 5]
tlab_extreme:
	$(CC) $(CFLAGS) -DUSE_THREAD_LOCAL_GC -o tlab_extreme_gc_test $(THREAD_TEST) $(TLAB_EXTREME_GC_SRC) $(LDFLAGS) -ldl
	@echo "Built: tlab_extreme_gc_test"

# 6. Commercial Boehm GC 버전 [cite: 5]
boehm:
	$(CC) $(CFLAGS) -o boehm_gc_test $(BOEHM_TEST) -lgc $(LDFLAGS)
	@echo "Built: boehm_gc_test"

# --- 시뮬레이션 및 그래프 테스트 타겟 --- [cite: 7]

sim_new:
	$(CC) $(CFLAGS) -DUSE_THREAD_LOCAL_GC -o sim_new_test $(SERVER_SIM_TEST) $(TLAB_EXTREME_GC_SRC) $(LDFLAGS) -ldl
	@echo "Built: sim_new_test"

graph_new:
	$(CC) $(CFLAGS) -DUSE_THREAD_LOCAL_GC -o graph_new_test $(SHARED_GRAPH_TEST) $(TLAB_EXTREME_GC_SRC) $(LDFLAGS) -ldl
	@echo "Built: graph_new_test"

# --- 정리 및 도구 --- 

clean:
	rm -f *_test *.o

.PHONY: all simple advanced tlab tlab_advanced tlab_extreme boehm clean