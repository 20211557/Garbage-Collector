# C Garbage Collector with AI

C언어를 활용하여 C/C++ 환경에서 동작하는 Garbage Collector(GC)를 AI를 활용하여 만들어보았다.

-----
## 패키지 설치
`sudo apt install build-essential libgc-dev gdb valgrind`

----
## 🚀 진화 과정 (Evolution History)

### Phase 1: Basic Conservative GC (`simple_gc.c`)
- **구현:** 단순 링크드 리스트를 이용한 메타데이터 관리. `__builtin_frame_address`와 `setjmp`를 활용한 보수적(Conservative) 스택 윈도우 스캔.
- **한계:** 멀티스레드 지원x, 시간 복잡도 O(N)

### Phase 2: AVL Tree Optimization (`advanced_gc.c`)
- **구현:** `active_list`를 AVL 트리 구조로 교체. 
- **성과:** 포인터 검색(`find_and_move`) 시간 복잡도를 O(N)에서 O(log N)으로 최적화.

### Phase 3: TLAB & Signal STW (`tlab_gc.c`)
- **구현:** 스레드별 TLAB 도입 및 `posix_memalign`을 이용한 청크 할당. `SIGUSR1` 시그널을 이용한 강제 프리엠티브(Preemptive) STW 도입.
- **성과:** 멀티스레드 환경에서 할당 속도 비약적 상승.
- **한계:** 시스템 콜 간섭(EINTR) 및 OS 스케줄러와의 충돌 우려 존재.

### Phase 4: Parallel Mark & Sweep (`tlab_advanced_gc.c`)
- **구현:** 전용 GC 워커 스레드 풀 생성. `pthread_barrier`와 Lock-free 큐를 이용해 마킹 및 스윕 단계를 병렬화(Parallel GC).
- **성과:** STW 멈춤 시간(Pause time) 대폭 감소.

### Phase 5: Safepoint & Coalescing (`tlab_extreme_gc.c`)
- **구현:** 시그널을 제거하고 폴링 기반의 Safepoint 로직 도입. 시스템 콜 래퍼(`GC_enter_syscall`)를 통한 데드락 방어 및 단편화 해결을 위한 Coalescing 적용.

---

## 한계점
1. concurrent GC가 아니다. 프로그램이 실행되면서 백그라운드로 GC가 동시에 작동하지 않는다. 모든 스레드가 멈추고 그때 GC가 동작하는 방식이다.

2. Conservative GC 방식을 사용한다. 스택, 데이터 영역을 모두 스캔하여 포인터 후보들을 스캔하는 방식 사용한다. 하지만 상용 GC(JVM GC)는 stack map을 이용하여 precise root set를 사용한다. 즉 힙 주소값을 가지고 있는 지역 변수를 정확히 찾아낸다. 하지만 C언어에서는 stack map을 지원하지 않는다. 이를 구현하려면 컴파일러 자체를 고쳐야 한다. 

3. 리눅스 환경에서만 동작한다.