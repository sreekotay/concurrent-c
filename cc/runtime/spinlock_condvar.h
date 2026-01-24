/*
 * Spinlock-Condvar: Hybrid synchronization primitive
 * 
 * Combines spin-waiting with efficient OS wake primitives:
 *   1. Spin for a configurable number of iterations (avoids syscall for fast paths)
 *   2. Fall back to futex/ulock sleep (single syscall, no mutex overhead)
 * 
 * This is similar to Go's runtime semaphore and parking_lot in Rust.
 * 
 * Usage:
 *   spinlock_condvar cv;
 *   spinlock_condvar_init(&cv);
 *   
 *   // Waiter:
 *   while (!condition) {
 *       spinlock_condvar_wait(&cv, &condition);
 *   }
 *   
 *   // Signaler:
 *   condition = true;
 *   spinlock_condvar_signal(&cv);  // or signal_all
 */

#ifndef SPINLOCK_CONDVAR_H
#define SPINLOCK_CONDVAR_H

#include <stdatomic.h>
#include <stdint.h>
#include <sched.h>

/* ============================================================================
 * Platform-specific efficient wait/wake
 * ============================================================================ */

#if defined(__linux__)
    #define SCV_USE_FUTEX 1
    #include <linux/futex.h>
    #include <sys/syscall.h>
    #include <unistd.h>
#elif defined(__APPLE__)
    #define SCV_USE_ULOCK 1
    extern int __ulock_wait(uint32_t operation, void* addr, uint64_t value, uint32_t timeout);
    extern int __ulock_wake(uint32_t operation, void* addr, uint64_t wake_value);
    #define UL_COMPARE_AND_WAIT 1
    #define ULF_WAKE_ALL        0x00000100
    #define ULF_NO_ERRNO        0x01000000
#else
    #define SCV_USE_CONDVAR 1
    #include <pthread.h>
#endif

/* ============================================================================
 * CPU pause for spin loops
 * ============================================================================ */

static inline void scv_cpu_pause(void) {
    #if defined(__x86_64__) || defined(_M_X64)
    __asm__ volatile("pause");
    #elif defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("yield");
    #endif
}

/* ============================================================================
 * Spin parameters (tunable)
 * ============================================================================ */

#ifndef SCV_SPIN_FAST_ITERS
#define SCV_SPIN_FAST_ITERS 128   /* Fast spins with cpu_pause */
#endif

#ifndef SCV_SPIN_YIELD_ITERS  
#define SCV_SPIN_YIELD_ITERS 8    /* Yield spins before sleep */
#endif

/* ============================================================================
 * Spinlock-Condvar structure
 * ============================================================================ */

typedef struct {
    _Atomic uint32_t seq;        /* Sequence number, incremented on signal */
    _Atomic uint32_t waiters;    /* Number of threads waiting */
#ifdef SCV_USE_CONDVAR
    pthread_mutex_t mutex;
    pthread_cond_t cond;
#endif
} spinlock_condvar;

/* ============================================================================
 * Initialization / Cleanup
 * ============================================================================ */

static inline void spinlock_condvar_init(spinlock_condvar* cv) {
    atomic_store_explicit(&cv->seq, 0, memory_order_relaxed);
    atomic_store_explicit(&cv->waiters, 0, memory_order_relaxed);
#ifdef SCV_USE_CONDVAR
    pthread_mutex_init(&cv->mutex, NULL);
    pthread_cond_init(&cv->cond, NULL);
#endif
}

static inline void spinlock_condvar_destroy(spinlock_condvar* cv) {
#ifdef SCV_USE_CONDVAR
    pthread_mutex_destroy(&cv->mutex);
    pthread_cond_destroy(&cv->cond);
#else
    (void)cv;
#endif
}

/* ============================================================================
 * Wait: Spin then sleep until signaled
 * 
 * Parameters:
 *   cv - the condvar
 *   condition - pointer to atomic condition to check (optional, can be NULL)
 *               If provided, returns early when *condition becomes non-zero
 * 
 * Returns when signaled or condition becomes true.
 * Spurious wakeups are possible - caller should re-check condition.
 * ============================================================================ */

static inline void spinlock_condvar_wait(spinlock_condvar* cv, const _Atomic int* condition) {
    /* Capture current sequence before checking condition */
    uint32_t seq = atomic_load_explicit(&cv->seq, memory_order_acquire);
    
    /* Fast path: condition already true */
    if (condition && atomic_load_explicit(condition, memory_order_acquire)) {
        return;
    }
    
    /* Phase 1: Fast spin with cpu_pause */
    for (int i = 0; i < SCV_SPIN_FAST_ITERS; i++) {
        if (condition && atomic_load_explicit(condition, memory_order_acquire)) {
            return;
        }
        /* Check if signaled */
        if (atomic_load_explicit(&cv->seq, memory_order_acquire) != seq) {
            return;
        }
        scv_cpu_pause();
    }
    
    /* Phase 2: Yield spin */
    for (int i = 0; i < SCV_SPIN_YIELD_ITERS; i++) {
        if (condition && atomic_load_explicit(condition, memory_order_acquire)) {
            return;
        }
        if (atomic_load_explicit(&cv->seq, memory_order_acquire) != seq) {
            return;
        }
        sched_yield();
    }
    
    /* Phase 3: Register as waiter and sleep */
    atomic_fetch_add_explicit(&cv->waiters, 1, memory_order_relaxed);
    
    /* Re-check after registering (avoid lost wakeup) */
    if (condition && atomic_load_explicit(condition, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&cv->waiters, 1, memory_order_relaxed);
        return;
    }
    if (atomic_load_explicit(&cv->seq, memory_order_acquire) != seq) {
        atomic_fetch_sub_explicit(&cv->waiters, 1, memory_order_relaxed);
        return;
    }
    
#ifdef SCV_USE_FUTEX
    /* Linux: futex wait on seq value */
    syscall(SYS_futex, &cv->seq, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, seq, NULL, NULL, 0);
#elif defined(SCV_USE_ULOCK)
    /* macOS: ulock wait */
    __ulock_wait(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &cv->seq, seq, 0);
#else
    /* Fallback: pthread condvar */
    pthread_mutex_lock(&cv->mutex);
    while (atomic_load_explicit(&cv->seq, memory_order_acquire) == seq) {
        if (condition && atomic_load_explicit(condition, memory_order_acquire)) {
            break;
        }
        pthread_cond_wait(&cv->cond, &cv->mutex);
    }
    pthread_mutex_unlock(&cv->mutex);
#endif
    
    atomic_fetch_sub_explicit(&cv->waiters, 1, memory_order_relaxed);
}

/* ============================================================================
 * Signal: Wake one waiting thread
 * ============================================================================ */

static inline void spinlock_condvar_signal(spinlock_condvar* cv) {
    /* Increment sequence to signal waiters */
    atomic_fetch_add_explicit(&cv->seq, 1, memory_order_release);
    
    /* Only syscall if there are waiters */
    if (atomic_load_explicit(&cv->waiters, memory_order_relaxed) == 0) {
        return;
    }
    
#ifdef SCV_USE_FUTEX
    syscall(SYS_futex, &cv->seq, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, NULL, NULL, 0);
#elif defined(SCV_USE_ULOCK)
    __ulock_wake(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &cv->seq, 0);
#else
    pthread_mutex_lock(&cv->mutex);
    pthread_cond_signal(&cv->cond);
    pthread_mutex_unlock(&cv->mutex);
#endif
}

/* ============================================================================
 * Signal All: Wake all waiting threads
 * ============================================================================ */

static inline void spinlock_condvar_signal_all(spinlock_condvar* cv) {
    /* Increment sequence to signal waiters */
    atomic_fetch_add_explicit(&cv->seq, 1, memory_order_release);
    
    /* Only syscall if there are waiters */
    if (atomic_load_explicit(&cv->waiters, memory_order_relaxed) == 0) {
        return;
    }
    
#ifdef SCV_USE_FUTEX
    syscall(SYS_futex, &cv->seq, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, INT32_MAX, NULL, NULL, 0);
#elif defined(SCV_USE_ULOCK)
    __ulock_wake(UL_COMPARE_AND_WAIT | ULF_WAKE_ALL | ULF_NO_ERRNO, &cv->seq, 0);
#else
    pthread_mutex_lock(&cv->mutex);
    pthread_cond_broadcast(&cv->cond);
    pthread_mutex_unlock(&cv->mutex);
#endif
}

/* ============================================================================
 * Try Signal: Signal only if there are waiters (optimization)
 * Returns 1 if signaled, 0 if no waiters
 * ============================================================================ */

static inline int spinlock_condvar_try_signal(spinlock_condvar* cv) {
    if (atomic_load_explicit(&cv->waiters, memory_order_relaxed) == 0) {
        return 0;
    }
    spinlock_condvar_signal(cv);
    return 1;
}

/* ============================================================================
 * Timed Wait: Wait with timeout
 * Returns 0 on signal, ETIMEDOUT on timeout
 * ============================================================================ */

#include <errno.h>
#include <time.h>

static inline int spinlock_condvar_timedwait(spinlock_condvar* cv, 
                                              const _Atomic int* condition,
                                              const struct timespec* abstime) {
    if (!abstime) {
        spinlock_condvar_wait(cv, condition);
        return 0;
    }
    
    /* Capture current sequence */
    uint32_t seq = atomic_load_explicit(&cv->seq, memory_order_acquire);
    
    /* Fast path: condition already true */
    if (condition && atomic_load_explicit(condition, memory_order_acquire)) {
        return 0;
    }
    
    /* Get current time for timeout calculation */
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    
    /* Calculate timeout in nanoseconds */
    int64_t timeout_ns = (abstime->tv_sec - now.tv_sec) * 1000000000LL + 
                         (abstime->tv_nsec - now.tv_nsec);
    if (timeout_ns <= 0) {
        return ETIMEDOUT;
    }
    
    /* Phase 1: Fast spin (limited by timeout) */
    int spin_iters = SCV_SPIN_FAST_ITERS;
    if (timeout_ns < 10000) spin_iters = 16;  /* Short timeout: less spinning */
    
    for (int i = 0; i < spin_iters; i++) {
        if (condition && atomic_load_explicit(condition, memory_order_acquire)) {
            return 0;
        }
        if (atomic_load_explicit(&cv->seq, memory_order_acquire) != seq) {
            return 0;
        }
        scv_cpu_pause();
    }
    
    /* Phase 2: Yield spin */
    for (int i = 0; i < SCV_SPIN_YIELD_ITERS; i++) {
        if (condition && atomic_load_explicit(condition, memory_order_acquire)) {
            return 0;
        }
        if (atomic_load_explicit(&cv->seq, memory_order_acquire) != seq) {
            return 0;
        }
        sched_yield();
    }
    
    /* Re-check timeout after spinning */
    clock_gettime(CLOCK_REALTIME, &now);
    timeout_ns = (abstime->tv_sec - now.tv_sec) * 1000000000LL + 
                 (abstime->tv_nsec - now.tv_nsec);
    if (timeout_ns <= 0) {
        return ETIMEDOUT;
    }
    
    /* Phase 3: Sleep with timeout */
    atomic_fetch_add_explicit(&cv->waiters, 1, memory_order_relaxed);
    
    /* Re-check after registering */
    if (condition && atomic_load_explicit(condition, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&cv->waiters, 1, memory_order_relaxed);
        return 0;
    }
    if (atomic_load_explicit(&cv->seq, memory_order_acquire) != seq) {
        atomic_fetch_sub_explicit(&cv->waiters, 1, memory_order_relaxed);
        return 0;
    }
    
    int result = 0;
    
#ifdef SCV_USE_FUTEX
    /* Linux: futex with timeout */
    struct timespec rel_timeout;
    rel_timeout.tv_sec = timeout_ns / 1000000000LL;
    rel_timeout.tv_nsec = timeout_ns % 1000000000LL;
    int rc = syscall(SYS_futex, &cv->seq, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, seq, &rel_timeout, NULL, 0);
    if (rc == -1 && errno == ETIMEDOUT) {
        result = ETIMEDOUT;
    }
#elif defined(SCV_USE_ULOCK)
    /* macOS: ulock with timeout (microseconds) */
    uint32_t timeout_us = (uint32_t)(timeout_ns / 1000);
    if (timeout_us == 0) timeout_us = 1;
    int rc = __ulock_wait(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &cv->seq, seq, timeout_us);
    if (rc == -ETIMEDOUT) {
        result = ETIMEDOUT;
    }
#else
    /* Fallback: pthread condvar */
    pthread_mutex_lock(&cv->mutex);
    while (atomic_load_explicit(&cv->seq, memory_order_acquire) == seq) {
        if (condition && atomic_load_explicit(condition, memory_order_acquire)) {
            break;
        }
        int rc = pthread_cond_timedwait(&cv->cond, &cv->mutex, abstime);
        if (rc == ETIMEDOUT) {
            result = ETIMEDOUT;
            break;
        }
    }
    pthread_mutex_unlock(&cv->mutex);
#endif
    
    atomic_fetch_sub_explicit(&cv->waiters, 1, memory_order_relaxed);
    return result;
}

#endif /* SPINLOCK_CONDVAR_H */
