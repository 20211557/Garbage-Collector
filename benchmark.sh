#!/bin/bash

# 1. Makefile 기반 테스트 타겟 및 실행 파일 매핑 [cite: 53, 54, 55]
declare -A TESTS=(
    ["simple"]="simple_gc_test"
    ["simple_advanced"]="simple_advanced_gc_test"
    ["simple_tlab"]="simple_tlab_gc_test"
    ["simple_tlab_advanced"]="simple_tlab_advanced_gc_test"
    ["simple_tlab_extreme"]="simple_tlab_extreme_gc_test"
    ["simple_boehm"]="simple_boehm_gc_test"
    ["advanced"]="advanced_gc_test"
    ["tlab"]="tlab_gc_test"
    ["tlab_advanced"]="tlab_advanced_gc_test"
    ["tlab_extreme"]="tlab_extreme_gc_test"
    ["boehm"]="boehm_gc_test"
    ["sim_new"]="sim_new_test"
    ["sim_boehm"]="sim_boehm_test"
    ["graph_new"]="graph_new_test"
    ["graph_boehm"]="graph_boehm_test"
)

ITERATIONS=10

echo "==========================================================="
echo " GC Latency Benchmark (10 Iterations Average)"
echo " Unit: Milliseconds (ms) / Precision: 6 Decimal Places"
echo " (Extracting latency directly from program output)"
echo "==========================================================="
printf "%-25s | %-20s\n" "Test Target" "Avg Latency (ms)"
echo "-----------------------------------------------------------"

# 이전 빌드 파일 정리
make clean > /dev/null 2>&1

for TARGET in "${!TESTS[@]}"; do
    BINARY=${TESTS[$TARGET]}

    # Makefile을 이용한 컴파일 [cite: 53, 54, 55]
    if ! make "$TARGET" > /dev/null 2>&1; then
        echo "Error: Failed to build $TARGET"
        continue
    fi

    TOTAL_TIME_SEC=0

    # 10번 반복 실행
    for i in $(seq 1 $ITERATIONS); do
        # 1. 프로그램을 실행하고 표준 출력에서 "소요 시간"이 포함된 줄을 찾음
        # 2. sed를 사용해 콜론(:) 뒤의 숫자와 "초" 사이의 값만 추출
        RAW_OUTPUT=$(./"$BINARY" | grep "소요 시간")
        LATENCY=$(echo "$RAW_OUTPUT" | sed -E 's/.*: ([0-9.]+) 초.*/\1/')
        
        # 값이 비어있을 경우를 대비해 0으로 초기화
        if [ -z "$LATENCY" ]; then
            LATENCY=0
        fi

        # 누적 합산 (bc -l 사용)
        TOTAL_TIME_SEC=$(echo "$TOTAL_TIME_SEC + $LATENCY" | bc -l)
    done

    # 평균 계산 및 밀리초(ms) 변환 (초 * 1000)
    # scale=9로 중간 계산 정밀도를 높인 후 최종적으로 소수점 6자리까지 출력
    AVG_TIME_MS=$(echo "scale=9; ($TOTAL_TIME_SEC / $ITERATIONS) * 1000" | bc -l)

    # 결과 출력 (소수점 6자리 포맷팅)
    printf "%-25s | %20.6f ms\n" "$TARGET" "$AVG_TIME_MS"
done

echo "==========================================================="
echo "Benchmark Completed."