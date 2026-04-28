#!/bin/bash

# 1. Makefile 기반 테스트 타겟 및 실행 파일 매핑 
declare -A TESTS=(
    ["simple"]="simple_gc_test"
    ["simple_advanced"]="simple_advanced_gc_test"
    ["simple_tlab"]="simple_tlab_gc_test"
    ["simple_tlab_extreme"]="simple_tlab_extreme_gc_test"
    ["simple_boehm"]="simple_boehm_gc_test"
    ["tlab_extreme"]="tlab_extreme_gc_test"
    ["boehm"]="boehm_gc_test"
    ["sim_new"]="sim_new_test"
    ["sim_boehm"]="sim_boehm_test"
    ["graph_new"]="graph_new_test"
    ["graph_boehm"]="graph_boehm_test"
)

echo "==========================================================="
echo " Garbage Collector Memory Leak Check (Valgrind)"
echo "==========================================================="
printf "%-25s | %-10s | %-15s\n" "Test Target" "Result" "Details"
echo "-----------------------------------------------------------"

# 이전 빌드 파일 정리 
make clean > /dev/null 2>&1

for TARGET in "${!TESTS[@]}"; do
    BINARY=${TESTS[$TARGET]}

    # Makefile을 이용한 빌드 
    if ! make "$TARGET" > /dev/null 2>&1; then
        echo "Error: Failed to build $TARGET"
        continue
    fi

    # Valgrind 실행 결과 캡처
    VALGRIND_OUT=$(valgrind --leak-check=full ./"$BINARY" 2>&1)

    # --- 판정 로직 강화 ---
    
    # 1. 완벽한 해제 메시지 확인 ("no leaks are possible")
    PERFECT_FREE=$(echo "$VALGRIND_OUT" | grep -c "no leaks are possible")
    
    # 2. 요약 테이블에서 'definitely lost: 0' 확인 (쉼표 제거 처리)
    DEF_LOST=$(echo "$VALGRIND_OUT" | grep "definitely lost:" | awk '{print $4}' | sed 's/,//g')
    
    # 3. 에러 요약에서 '0 errors' 확인
    ERR_COUNT=$(echo "$VALGRIND_OUT" | grep "ERROR SUMMARY:" | awk '{print $4}')

    # 판정 조건:
    # (완벽 해제 문구가 있거나) 또는 (확실한 누수가 0이면서 에러가 0일 때) PASS
    if [ "$PERFECT_FREE" -gt 0 ] || ([ "$DEF_LOST" == "0" ] && [ "$ERR_COUNT" == "0" ]); then
        RESULT="PASS"
        INFO="Clean"
    elif [ "$DEF_LOST" == "0" ] && [ "$ERR_COUNT" -gt 0 ]; then
        # 누수는 없으나 접근 에러 등이 있는 경우 (보수적 GC 특성)
        RESULT="PASS*"
        INFO="No leaks, but $ERR_COUNT errors"
    else
        RESULT="FAIL"
        INFO="${DEF_LOST:-Unknown} lost / $ERR_COUNT err"
    fi

    # 결과 출력
    printf "%-25s | %-10s | %s\n" "$TARGET" "$RESULT" "$INFO"
done

echo "-----------------------------------------------------------"
echo "PASS*: No memory leaks, but Valgrind reported access warnings (Common in Conservative GC)."
echo "Leak Check Completed."