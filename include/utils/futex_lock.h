#ifndef FUTEX_LOCK_H
#define FUTEX_LOCK_H

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdatomic.h>

typedef struct {
    atomic_int lock; // 0: unlocked, 1: locked
} futex_t;

static inline void futex_init(futex_t* f) {
    atomic_init(&f->lock, 0);
}

static inline void futex_lock(futex_t* f) {
    int expected = 0;
    // 0을 1로 바꾸는 데 성공하면 즉시 락 획득 (Fast path)
    while (!atomic_compare_exchange_strong(&f->lock, &expected, 1)) {
        expected = 0;
        // 실패 시 커널에서 대기 (Slow path)
        syscall(SYS_futex, &f->lock, FUTEX_WAIT, 1, NULL, NULL, 0);
    }
}

static inline void futex_unlock(futex_t* f) {
    atomic_store(&f->lock, 0);
    // 대기 중인 스레드 하나를 깨움
    syscall(SYS_futex, &f->lock, FUTEX_WAKE, 1, NULL, NULL, 0);
}

#endif