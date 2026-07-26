/*
 * Named exclusive lock table (v1).
 *
 * Lock word: 0 free, 1 locked, 2 locked+waiters.
 * Uncontended lock/unlock CAS is inlined in the header.
 * Contended: enqueue fiber, park_if; unlock drains waiters (wake-all) and
 * stores FREE; waiters retry CAS (no ownership handoff).
 *
 * Double-release safety is local to CCExclusiveGuard.held.
 */

#include <ccc/cc_exclusive.cch>

#include "fiber_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct CCExclusiveWaiter {
    void* fiber;
    _Atomic int ready; /* 0 = park, 1 = woken (sticky for park_if) */
    struct CCExclusiveWaiter* next;
} CCExclusiveWaiter;

typedef struct {
    _Atomic int used;
    uint64_t name;
    _Atomic int locked;    /* 0 free, 1 locked, 2 locked+waiters */
    _Atomic int wait_spin; /* 0/1 spinlock for waiter list */
    CCExclusiveWaiter* wait_head;
    CCExclusiveWaiter* wait_tail;
} CCExclusiveEntry;

#define CC_EXCLUSIVE_TABLE_SIZE 1024

struct CCExclusive {
    pthread_mutex_t create_mu;
    CCExclusiveEntry table[CC_EXCLUSIVE_TABLE_SIZE];
};

static void cc__spin_lock(_Atomic int* spin) {
    for (;;) {
        int expected = 0;
        if (atomic_compare_exchange_weak_explicit(
                spin, &expected, 1,
                memory_order_acquire, memory_order_relaxed)) {
            return;
        }
#if defined(__x86_64__) || defined(__i386__)
        __asm__ __volatile__("pause");
#elif defined(__aarch64__)
        __asm__ __volatile__("yield");
#endif
    }
}

static void cc__spin_unlock(_Atomic int* spin) {
    atomic_store_explicit(spin, 0, memory_order_release);
}

static size_t cc__exclusive_slot(uint64_t name) {
    return (size_t)((name * 11400714819323198485ull) >> (64 - 10));
}

static CCExclusiveEntry* cc__exclusive_lookup(CCExclusive* excl, uint64_t name) {
    size_t start = cc__exclusive_slot(name);
    for (size_t i = 0; i < CC_EXCLUSIVE_TABLE_SIZE; i++) {
        size_t idx = (start + i) & (CC_EXCLUSIVE_TABLE_SIZE - 1);
        CCExclusiveEntry* e = &excl->table[idx];
        if (atomic_load_explicit(&e->used, memory_order_acquire) == 0) {
            return NULL;
        }
        if (e->name == name) return e;
    }
    return NULL;
}

static CCExclusiveEntry* cc__exclusive_get_or_create(CCExclusive* excl, uint64_t name) {
    CCExclusiveEntry* hit = cc__exclusive_lookup(excl, name);
    if (hit) return hit;

    pthread_mutex_lock(&excl->create_mu);
    hit = cc__exclusive_lookup(excl, name);
    if (hit) {
        pthread_mutex_unlock(&excl->create_mu);
        return hit;
    }

    size_t start = cc__exclusive_slot(name);
    for (size_t i = 0; i < CC_EXCLUSIVE_TABLE_SIZE; i++) {
        size_t idx = (start + i) & (CC_EXCLUSIVE_TABLE_SIZE - 1);
        CCExclusiveEntry* e = &excl->table[idx];
        if (atomic_load_explicit(&e->used, memory_order_relaxed) != 0) continue;

        e->name = name;
        atomic_store_explicit(&e->locked, CC_EXCL_FREE, memory_order_relaxed);
        atomic_store_explicit(&e->wait_spin, 0, memory_order_relaxed);
        e->wait_head = NULL;
        e->wait_tail = NULL;
        atomic_store_explicit(&e->used, 1, memory_order_release);
        pthread_mutex_unlock(&excl->create_mu);
        return e;
    }
    pthread_mutex_unlock(&excl->create_mu);
    return NULL;
}

static void cc__exclusive_lock_entry(CCExclusiveEntry* e) {
    for (;;) {
        int expected = CC_EXCL_FREE;
        if (atomic_compare_exchange_weak_explicit(
                &e->locked, &expected, CC_EXCL_LOCKED,
                memory_order_acquire, memory_order_relaxed)) {
            return;
        }

        if (!cc__fiber_in_context()) {
            for (int i = 0; i < 64; i++) {
                expected = CC_EXCL_FREE;
                if (atomic_compare_exchange_weak_explicit(
                        &e->locked, &expected, CC_EXCL_LOCKED,
                        memory_order_acquire, memory_order_relaxed)) {
                    return;
                }
#if defined(__GNUC__) || defined(__clang__)
                __asm__ __volatile__("" ::: "memory");
#endif
            }
            continue;
        }

        CCExclusiveWaiter node;
        node.fiber = cc__fiber_current();
        node.next = NULL;
        atomic_store_explicit(&node.ready, 0, memory_order_relaxed);

        cc__spin_lock(&e->wait_spin);
        expected = CC_EXCL_FREE;
        if (atomic_compare_exchange_strong_explicit(
                &e->locked, &expected, CC_EXCL_LOCKED,
                memory_order_acquire, memory_order_relaxed)) {
            cc__spin_unlock(&e->wait_spin);
            return;
        }
        /* Publish CONTENDED via CAS 1→2 before enqueue. Fast unlock is CAS
         * 1→0; once we land 2 it must take the slow path and serialize on
         * this spin. If the word is already FREE, retry (lost the race). */
        for (;;) {
            int st = atomic_load_explicit(&e->locked, memory_order_relaxed);
            if (st == CC_EXCL_FREE) {
                cc__spin_unlock(&e->wait_spin);
                goto retry_lock;
            }
            if (st == CC_EXCL_CONTENDED) break;
            expected = CC_EXCL_LOCKED;
            if (atomic_compare_exchange_strong_explicit(
                    &e->locked, &expected, CC_EXCL_CONTENDED,
                    memory_order_release, memory_order_relaxed)) {
                break;
            }
            /* expected updated to FREE or CONTENDED — loop. */
        }
        if (!e->wait_head) e->wait_head = &node;
        else e->wait_tail->next = &node;
        e->wait_tail = &node;
        cc__spin_unlock(&e->wait_spin);

        /* V2 has no pending-unpark latch: recheck ready after every park so
         * an unlock that runs between enqueue and park cannot lose the wake. */
        while (atomic_load_explicit(&node.ready, memory_order_acquire) == 0) {
            CC_FIBER_PARK("exclusive_lock");
        }
        /* Woken (or never parked): retry CAS. */
    retry_lock:
        ;
    }
}

void cc_exclusive_guard_unlock_impl(void* entry) {
    CCExclusiveEntry* e = (CCExclusiveEntry*)entry;
    if (!e) abort();

    /* Contended unlock: drain waiters + FREE under the spinlock so we never
     * leave parked fibers behind a free lock word. Wake-all; losers requeue. */
    cc__spin_lock(&e->wait_spin);
    CCExclusiveWaiter* w = e->wait_head;
    e->wait_head = NULL;
    e->wait_tail = NULL;
    atomic_store_explicit(&e->locked, CC_EXCL_FREE, memory_order_release);
    cc__spin_unlock(&e->wait_spin);

    while (w) {
        CCExclusiveWaiter* next = w->next;
        atomic_store_explicit(&w->ready, 1, memory_order_release);
        void* fiber = w->fiber;
        w = next;
        if (fiber) cc__fiber_unpark(fiber);
    }
}

CCExclusive* cc_exclusive_create(void) {
    CCExclusive* excl = (CCExclusive*)calloc(1, sizeof(CCExclusive));
    if (!excl) return NULL;
    pthread_mutex_init(&excl->create_mu, NULL);
    return excl;
}

void cc_exclusive_destroy(CCExclusive* excl) {
    if (!excl) return;
    pthread_mutex_destroy(&excl->create_mu);
    free(excl);
}

CCExclusiveMutex cc_exclusive_mutex(CCExclusive* excl, uint64_t name) {
    CCExclusiveMutex m = {0};
    m.excl = excl;
    m.name = name;
    if (!excl) return m;
    CCExclusiveEntry* e = cc__exclusive_get_or_create(excl, name);
    if (!e) abort();
    m._state = &e->locked;
    m._entry = e;
    return m;
}

CCExclusiveGuard cc_exclusive_mutex_lock_slow(CCExclusiveMutex* m) {
    CCExclusiveGuard g = {0};
    if (!m) abort();
    CCExclusiveEntry* e = (CCExclusiveEntry*)m->_entry;
    if (!e) abort();
    cc__exclusive_lock_entry(e);
    g._state = &e->locked;
    g._entry = e;
    g.held = 1;
    return g;
}
