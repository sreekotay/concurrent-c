/*
 * Wake Primitive - Platform-specific efficient thread wake mechanism
 * 
 * Replaces pthread condvar with faster OS primitives:
 *   - Linux: futex(FUTEX_WAIT/FUTEX_WAKE) - single syscall, no mutex
 *   - macOS 14+: os_sync_wait_on_address / os_sync_wake_by_address_any
 *   - macOS <14: __ulock_wait / __ulock_wake (private but stable, used by libdispatch)
 *   - Fallback: pthread condvar (slower but portable)
 * 
 * The key advantage over condvar:
 *   condvar:  lock mutex -> check -> unlock+wait -> relock on wake (4 ops)
 *   futex:    atomic check -> syscall if needed (1-2 ops, no mutex)
 */

#ifndef WAKE_PRIMITIVE_H
#define WAKE_PRIMITIVE_H

#include <stdatomic.h>
#include <stdint.h>

/* ============================================================================
 * Platform Detection
 * ============================================================================ */

#if defined(__linux__)
    #define WAKE_USE_FUTEX 1
#elif defined(__APPLE__)
    #include <AvailabilityMacros.h>
    /* os_sync_* is available in macOS 14+ (Sonoma), but __ulock is available earlier */
    #define WAKE_USE_ULOCK 1
#else
    #define WAKE_USE_CONDVAR 1
#endif

/* ============================================================================
 * Wake Primitive Structure
 * ============================================================================ */

typedef struct {
    _Atomic uint32_t value;  /* Counter incremented on each wake */
#ifdef WAKE_USE_CONDVAR
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int initialized;
#endif
} wake_primitive;

/* ============================================================================
 * Platform-Specific Implementations
 * ============================================================================ */

#ifdef WAKE_USE_FUTEX

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>

static inline void wake_primitive_init(wake_primitive* wp) {
    atomic_store_explicit(&wp->value, 0, memory_order_relaxed);
}

static inline void wake_primitive_destroy(wake_primitive* wp) {
    (void)wp;  /* No cleanup needed for futex */
}

/* Wait until value changes from expected. Returns immediately if already changed. */
static inline void wake_primitive_wait(wake_primitive* wp, uint32_t expected) {
    /* FUTEX_WAIT: sleep if *addr == expected, wake when signaled */
    syscall(SYS_futex, &wp->value, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 
            expected, NULL, NULL, 0);
    /* Ignore return value - spurious wakeups are fine, caller will re-check */
}

/* Wake one waiting thread */
static inline void wake_primitive_wake_one(wake_primitive* wp) {
    atomic_fetch_add_explicit(&wp->value, 1, memory_order_release);
    syscall(SYS_futex, &wp->value, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 
            1, NULL, NULL, 0);
}

/* Wake all waiting threads */
static inline void wake_primitive_wake_all(wake_primitive* wp) {
    atomic_fetch_add_explicit(&wp->value, 1, memory_order_release);
    syscall(SYS_futex, &wp->value, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 
            INT32_MAX, NULL, NULL, 0);
}

#elif defined(WAKE_USE_ULOCK)

#include <errno.h>

/* macOS private ulock API - stable since OS X 10.12, used by libdispatch/libc++ */
extern int __ulock_wait(uint32_t operation, void* addr, uint64_t value, uint32_t timeout);
extern int __ulock_wake(uint32_t operation, void* addr, uint64_t wake_value);

/* ulock operations */
#define UL_COMPARE_AND_WAIT 1
#define ULF_WAKE_ALL        0x00000100
#define ULF_NO_ERRNO        0x01000000

static inline void wake_primitive_init(wake_primitive* wp) {
    atomic_store_explicit(&wp->value, 0, memory_order_relaxed);
}

static inline void wake_primitive_destroy(wake_primitive* wp) {
    (void)wp;  /* No cleanup needed for ulock */
}

/* Wait until value changes from expected */
static inline void wake_primitive_wait(wake_primitive* wp, uint32_t expected) {
    /* __ulock_wait: sleep if *addr == value, wake when signaled
     * timeout=0 means wait forever */
    __ulock_wait(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &wp->value, expected, 0);
    /* Ignore return value - spurious wakeups are fine */
}

/* Wake one waiting thread */
static inline void wake_primitive_wake_one(wake_primitive* wp) {
    atomic_fetch_add_explicit(&wp->value, 1, memory_order_release);
    __ulock_wake(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &wp->value, 0);
}

/* Wake all waiting threads */
static inline void wake_primitive_wake_all(wake_primitive* wp) {
    atomic_fetch_add_explicit(&wp->value, 1, memory_order_release);
    __ulock_wake(UL_COMPARE_AND_WAIT | ULF_WAKE_ALL | ULF_NO_ERRNO, &wp->value, 0);
}

#else /* WAKE_USE_CONDVAR - fallback */

#include <pthread.h>

static inline void wake_primitive_init(wake_primitive* wp) {
    atomic_store_explicit(&wp->value, 0, memory_order_relaxed);
    pthread_mutex_init(&wp->mutex, NULL);
    pthread_cond_init(&wp->cond, NULL);
    wp->initialized = 1;
}

static inline void wake_primitive_destroy(wake_primitive* wp) {
    if (wp->initialized) {
        pthread_mutex_destroy(&wp->mutex);
        pthread_cond_destroy(&wp->cond);
        wp->initialized = 0;
    }
}

/* Wait until value changes from expected */
static inline void wake_primitive_wait(wake_primitive* wp, uint32_t expected) {
    pthread_mutex_lock(&wp->mutex);
    while (atomic_load_explicit(&wp->value, memory_order_acquire) == expected) {
        pthread_cond_wait(&wp->cond, &wp->mutex);
    }
    pthread_mutex_unlock(&wp->mutex);
}

/* Wake one waiting thread */
static inline void wake_primitive_wake_one(wake_primitive* wp) {
    atomic_fetch_add_explicit(&wp->value, 1, memory_order_release);
    pthread_mutex_lock(&wp->mutex);
    pthread_cond_signal(&wp->cond);
    pthread_mutex_unlock(&wp->mutex);
}

/* Wake all waiting threads */
static inline void wake_primitive_wake_all(wake_primitive* wp) {
    atomic_fetch_add_explicit(&wp->value, 1, memory_order_release);
    pthread_mutex_lock(&wp->mutex);
    pthread_cond_broadcast(&wp->cond);
    pthread_mutex_unlock(&wp->mutex);
}

#endif /* platform selection */

#endif /* WAKE_PRIMITIVE_H */
