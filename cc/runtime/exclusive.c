/*
 * Named exclusive lock table (v1).
 *
 * Lock word: 0 free, 1 locked, 2 contended.
 * Uncontended lock CAS and unlock fetch-sub are inlined in the header.
 *
 * A waiter changes LOCKED to CONTENDED under wait_spin before enqueueing.
 * Contended unlock hands ownership directly to one waiter and never
 * publishes FREE while the queue is nonempty.
 */

#include <ccc/cc_exclusive.cch>

#include "fiber_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct CCExclusiveWaiter {
    void* fiber;
    _Atomic int ready;
    struct CCExclusiveWaiter* next;
} CCExclusiveWaiter;

typedef struct {
    _Atomic int used;
    uint64_t name;
    _Atomic int locked;
    _Atomic int wait_spin;
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

static int cc__exclusive_try_lock(CCExclusiveEntry* e) {
    int expected = CC_EXCL_FREE;
    return atomic_compare_exchange_strong_explicit(
        &e->locked, &expected, CC_EXCL_LOCKED,
        memory_order_acquire, memory_order_relaxed);
}

/* Caller changed CONTENDED to LOCKED. Transfer ownership to the queue head. */
static void cc__exclusive_handoff(CCExclusiveEntry* e) {
    cc__spin_lock(&e->wait_spin);
    CCExclusiveWaiter* w = e->wait_head;
    if (!w) abort();

    e->wait_head = w->next;
    if (!e->wait_head) e->wait_tail = NULL;
    w->next = NULL;

    /* The recipient owns the lock; preserve CONTENDED if others remain. */
    atomic_store_explicit(
        &e->locked,
        e->wait_head ? CC_EXCL_CONTENDED : CC_EXCL_LOCKED,
        memory_order_release);
    void* fiber = w->fiber;
    cc__spin_unlock(&e->wait_spin);

    /* Do not dereference the stack waiter after publishing ready. */
    atomic_store_explicit(&w->ready, 1, memory_order_release);
    if (fiber) cc__fiber_unpark(fiber);
}

static void cc__exclusive_lock_entry(CCExclusiveEntry* e) {
    for (;;) {
        if (cc__exclusive_try_lock(e)) return;

        if (!cc__fiber_in_context()) {
            for (int i = 0; i < 64; i++) {
                if (cc__exclusive_try_lock(e)) return;
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
        /*
         * Publish contention in the lock word before enqueueing. The owner
         * either wins LOCKED->FREE first (we retry and acquire), or observes
         * CONTENDED and waits for this critical section before handoff.
         */
        for (;;) {
            if (cc__exclusive_try_lock(e)) {
                cc__spin_unlock(&e->wait_spin);
                return;
            }
            int expected = CC_EXCL_LOCKED;
            if (atomic_compare_exchange_strong_explicit(
                    &e->locked, &expected, CC_EXCL_CONTENDED,
                    memory_order_acq_rel, memory_order_acquire)) {
                break;
            }
            if (expected == CC_EXCL_CONTENDED) break;
        }
        if (!e->wait_head) e->wait_head = &node;
        else e->wait_tail->next = &node;
        e->wait_tail = &node;
        cc__spin_unlock(&e->wait_spin);

        while (atomic_load_explicit(&node.ready, memory_order_acquire) == 0) {
            CC_FIBER_PARK("exclusive_lock");
        }
        /* Handoff: unlocker left locked=1 for us. */
        return;
    }
}

void cc_exclusive_guard_unlock_impl(void* entry) {
    CCExclusiveEntry* e = (CCExclusiveEntry*)entry;
    if (!e) abort();
    cc__exclusive_handoff(e);
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
