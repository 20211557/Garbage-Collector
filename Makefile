
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

# 1. Simple test 버전 (단일 스레드, 단일 GC)
simple:
	$(CC) $(CFLAGS) -o simple_gc_test $(SIMPLE_TEST) $(SIMPLE_GC_SRC) $(LDFLAGS)
	@echo "Built: simple_gc_test"

simple_advanced:
	$(CC) $(CFLAGS) -o simple_advanced_gc_test $(SIMPLE_TEST) $(ADVANCED_GC_SRC) $(LDFLAGS)
	@echo "Built: simple_advanced_gc_test"

simple_tlab:
	$(CC) $(CFLAGS) -o simple_tlab_gc_test $(SIMPLE_TEST) $(TLAB_GC_SRC) $(LDFLAGS)
	@echo "Built: simple_tlab_gc_test"

simple_tlab_advanced:
	$(CC) $(CFLAGS) -o simple_tlab_advanced_gc_test $(SIMPLE_TEST) $(TLAB_ADVANCED_GC_SRC) $(LDFLAGS)
	@echo "Built: simple_tlab_advanced_gc_test"

simple_tlab_extreme:
	$(CC) $(CFLAGS) -DUSE_THREAD_LOCAL_GC -o simple_tlab_extreme_gc_test $(SIMPLE_TEST) $(TLAB_EXTREME_GC_SRC) $(LDFLAGS) -ldl
	@echo "Built: simple_tlab_extreme_gc_test"

simple_boehm:
	$(CC) $(CFLAGS) -DUSE_BOEHM_GC -o simple_boehm_gc_test $(SIMPLE_TEST) -lgc $(LDFLAGS)
	@echo "Built: simple_boehm_gc_test"

# 2. thread test 버전 (멀티 스레드, 다양한 GC)
advanced:
	$(CC) $(CFLAGS) -o advanced_gc_test $(THREAD_TEST) $(ADVANCED_GC_SRC) $(LDFLAGS)
	@echo "Built: advanced_gc_test"

tlab:
	$(CC) $(CFLAGS) -o tlab_gc_test $(THREAD_TEST) $(TLAB_GC_SRC) $(LDFLAGS)
	@echo "Built: tlab_gc_test"

tlab_advanced:
	$(CC) $(CFLAGS) -o tlab_advanced_gc_test $(THREAD_TEST) $(TLAB_ADVANCED_GC_SRC) $(LDFLAGS)
	@echo "Built: tlab_advanced_gc_test"

tlab_extreme:
	$(CC) $(CFLAGS) -DUSE_THREAD_LOCAL_GC -o tlab_extreme_gc_test $(THREAD_TEST) $(TLAB_EXTREME_GC_SRC) $(LDFLAGS) -ldl
	@echo "Built: tlab_extreme_gc_test"

boehm:
	$(CC) $(CFLAGS) -DUSE_BOEHM_GC -o boehm_gc_test $(THREAD_TEST) -lgc $(LDFLAGS)
	@echo "Built: boehm_gc_test"

# 3. 시뮬레이션 및 그래프 테스트 타겟

sim_new:
	$(CC) $(CFLAGS) -DUSE_THREAD_LOCAL_GC -o sim_new_test $(SERVER_SIM_TEST) $(TLAB_EXTREME_GC_SRC) $(LDFLAGS) -ldl
	@echo "Built: sim_new_test"

sim_boehm:
	$(CC) $(CFLAGS) -DUSE_BOEHM_GC -o sim_boehm_test $(SERVER_SIM_TEST) -lgc $(LDFLAGS)
	@echo "Built: sim_boehm_test"

graph_new:
	$(CC) $(CFLAGS) -DUSE_THREAD_LOCAL_GC -o graph_new_test $(SHARED_GRAPH_TEST) $(TLAB_EXTREME_GC_SRC) $(LDFLAGS) -ldl
	@echo "Built: graph_new_test"

graph_boehm:
	$(CC) $(CFLAGS) -DUSE_BOEHM_GC -o graph_boehm_test $(SHARED_GRAPH_TEST) -lgc $(LDFLAGS)
	@echo "Built: graph_boehm_test"

# --- 정리 및 도구 --- 

clean:
	rm -f *_test *.o

.PHONY: all simple advanced tlab tlab_advanced tlab_extreme boehm clean