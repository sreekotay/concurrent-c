/*
 * Named exclusive lock table (v1).
 *
 * Lock word: 0 free, 1 locked, 2 contended (locked, waiters may be queued).
 * The uncontended CAS lock and swap unlock are inlined in the header; this
 * file owns the slow paths.
 *
 * Contended protocol (futex-style barging, cf. Drepper "Futexes Are Tricky"):
 *   - A slow-path locker acquires with swap(CONTENDED); only an observed
 *     FREE grants ownership.  Any other value both fails the acquire and
 *     marks the word so the owner's unlock takes the contended path.
 *   - Unlock (header) swaps FREE unconditionally and, if the old value was
 *     CONTENDED, wakes exactly ONE queued waiter.  The lock is available to
 *     other lockers while the woken fiber is being scheduled (barging).
 *   - A woken waiter owns nothing; it loops and re-acquires with
 *     swap(CONTENDED).  Setting CONTENDED before it can ever park again is
 *     the invariant that keeps later unlocks waking the remaining sleepers:
 *     whenever the queue is nonempty, either the word is CONTENDED or at
 *     least one signalled waiter is awake to restore it.
 *   - The enqueue/unlock race (unlock swaps FREE before the waiter's node
 *     is visible, so it wakes nobody) is closed by re-contending once AFTER
 *     enqueue; on success the waiter owns the lock and takes its node back.
 *
 * Fiber wake safety: cc__fiber_unpark -> sched_v2_signal is sticky.  A
 * signal delivered while the target fiber is RUNNING sets SIGNAL_PENDING,
 * which the scheduler consumes at park-commit by requeueing instead of
 * parking.  Waiters therefore park in a "while (!ready) park" loop with no
 * lost-wake window; spurious wakes just re-check ready.
 */

#include <ccc/cc_exclusive.cch>

#include "fiber_internal.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct CCExclusiveWaiter {
    void* fiber;
    _Atomic int ready;
    struct CCExclusiveWaiter* next;
} CCExclusiveWaiter;

/*
 * One cache line per mutex entry: adjacent named locks must not false-share
 * their lock words.  `locked` MUST stay at offset 0 — the header inlines
 * the fast paths by casting the entry pointer to _Atomic int*.
 *
 * Mutex entries come from an arena pool on the section arena (Treiber
 * freelist + 64-byte-aligned bump alloc).  User-explicit free_mutex removes
 * the name (tombstone in the map) and pushes the entry back to the pool.
 * The discovery map grows under create_mu; old tables are retired and
 * arena_release'd on destroy so lock-free lookups never race a free.
 */
#if defined(__TINYC__)
/* TinyCC ignores aligned(N) for sizeof on some targets; pad to one line. */
typedef struct {
    _Atomic int locked;
    _Atomic int wait_spin;
    uint64_t name;
    CCExclusiveWaiter* wait_head;
    CCExclusiveWaiter* wait_tail;
    char _cc_line_pad[64 - (2 * sizeof(int) + sizeof(uint64_t) + 2 * sizeof(void*))];
} CCExclusiveEntry;
#else
typedef struct {
    _Atomic int locked;
    _Atomic int wait_spin;
    uint64_t name;
    CCExclusiveWaiter* wait_head;
    CCExclusiveWaiter* wait_tail;
} __attribute__((aligned(64))) CCExclusiveEntry;
#endif

_Static_assert(offsetof(CCExclusiveEntry, locked) == 0,
               "header casts entry to _Atomic int*");
_Static_assert(sizeof(CCExclusiveEntry) == 64, "one cache line per entry");

#define CC_EXCLUSIVE_DEFAULT_CAP 64

/* Open-addressing tombstone: name was freed; probe continues. */
#define CC_EXCL_TOMBSTONE ((CCExclusiveEntry*)(uintptr_t)1)

/* Bounded spin before queueing: exclusive critical sections are short by
 * contract, so a brief spin usually acquires without a park round-trip. */
#define CC_EXCL_SPIN_TRIES 64

typedef struct {
    _Atomic(CCExclusiveEntry*) entry; /* NULL empty, TOMBSTONE deleted */
} CCExclusiveBucket;

typedef struct CCExclusiveMap {
    size_t cap; /* power of two */
    struct CCExclusiveMap* retired_next;
    CCExclusiveBucket buckets[];
} CCExclusiveMap;

struct CCExclusive {
    CCArena* arena;
    pthread_mutex_t create_mu;
    /* Published under create_mu; lock-free lookups use a plain load.
     * Grows retire the prior table (released at destroy) so readers never
     * observe a freed map. */
    CCExclusiveMap* map;
    CCExclusiveMap* retired;
    size_t count; /* live names; create_mu */
    /* Arena pool for mutex entries (freelist on `arena`; 64-byte allocs). */
    CCArenaPool entry_pool;
};

static int cc__excl_is_tomb(CCExclusiveEntry* e) {
    return e == CC_EXCL_TOMBSTONE;
}

/* Like cc_arena_pool_alloc, but fresh entries are 64-byte aligned. */
static CCExclusiveEntry* cc__excl_entry_alloc(CCExclusive* excl) {
    CCArenaPool* p = &excl->entry_pool;
    uint64_t head = cc_atomic_load(&p->freelist);
    for (;;) {
        void* item = cc__pool_head_ptr(head);
        if (!item) break;
        uint64_t next = cc__pool_head_pack(*(void**)item, head >> CC__POOL_TAG_SHIFT);
        if (cc_atomic_cas(&p->freelist, &head, next)) {
            return (CCExclusiveEntry*)item;
        }
    }
    return (CCExclusiveEntry*)cc_arena_alloc(
        p->arena, sizeof(CCExclusiveEntry), 64);
}

static void cc__excl_entry_free(CCExclusive* excl, CCExclusiveEntry* e) {
    cc_arena_pool_free(&excl->entry_pool, e);
}

static inline void cc__cpu_pause(void) {
#if defined(__TINYC__)
    cc_cpu_pause_port();
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause");
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

static void cc__spin_lock(_Atomic int* spin) {
    for (;;) {
        int expected = 0;
        if (atomic_compare_exchange_weak_explicit(
                spin, &expected, 1,
                memory_order_acquire, memory_order_relaxed)) {
            return;
        }
        cc__cpu_pause();
    }
}

static void cc__spin_unlock(_Atomic int* spin) {
    atomic_store_explicit(spin, 0, memory_order_release);
}

static int cc__excl_dbg_register(CCExclusiveEntry* e, CCExclusiveWaiter* node);
static void cc__excl_dbg_unregister(int slot);
static int g_cc_excl_debug;

/* Debug event ring: reconstructs the exact interleaving of the final
 * pop/unpark/park events when a waiter strands.  CC_EXCL_DEBUG=1 only. */
enum {
    CC_EVT_ENQ = 1,      /* waiter enqueued node          aux=entry name  */
    CC_EVT_PARK = 2,     /* waiter about to park          aux=ready       */
    CC_EVT_WAKE = 3,     /* waiter returned from park     aux=ready       */
    CC_EVT_POP = 4,      /* unlocker popped node          aux=self fiber  */
    CC_EVT_UNPARK = 5,   /* unlocker finished unpark      aux=self fiber  */
    CC_EVT_ACQ2 = 6,     /* waiter acquired at re-contend aux=found       */
    CC_EVT_EXIT = 7,     /* waiter exited ready loop      aux=ready       */
    CC_EVT_EMPTY = 8,    /* unlock found empty queue      aux=self fiber  */
};

typedef struct {
    uint64_t t;
    int kind;
    void* fiber;
    void* node;
    long aux;
} cc__excl_evt_rec;

/* Per-thread rings: no shared cache line in the record path, so the
 * timing of the race under investigation is barely perturbed. */
#define CC_EXCL_EVT_RING 65536
#define CC_EXCL_EVT_MAX_THREADS 64
typedef struct {
    cc__excl_evt_rec recs[CC_EXCL_EVT_RING];
    uint64_t pos;
} cc__excl_evt_ring_t;
static _Atomic(cc__excl_evt_ring_t*) g_cc_excl_evt_rings[CC_EXCL_EVT_MAX_THREADS];
#if defined(__TINYC__)
#define tls_cc_excl_ring (*(cc__excl_evt_ring_t**)&(cc_rt_tls_get()->excl_ring))
#else
static __thread cc__excl_evt_ring_t* tls_cc_excl_ring;
#endif

static inline uint64_t cc__excl_now(void) {
#if defined(__TINYC__)
    return cc_cpu_counter_port();
#elif defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    return (uint64_t)__builtin_ia32_rdtsc();
#elif defined(__aarch64__) || defined(__arm64__)
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    /* ARM32 / other ILP32: no cycle counter in userspace; ns clock is fine
     * for the debug event ring (ordering + coarse deltas only). */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static _Atomic int g_cc_excl_evt_freeze;

static void cc__excl_evt(int kind, void* fiber, void* node, long aux) {
    if (!g_cc_excl_debug) return;
    if (atomic_load_explicit(&g_cc_excl_evt_freeze, memory_order_relaxed)) return;
    cc__excl_evt_ring_t* ring = tls_cc_excl_ring;
    if (!ring) {
        ring = (cc__excl_evt_ring_t*)calloc(1, sizeof(*ring));
        if (!ring) return;
        tls_cc_excl_ring = ring;
        for (int i = 0; i < CC_EXCL_EVT_MAX_THREADS; i++) {
            cc__excl_evt_ring_t* expected = NULL;
            if (atomic_compare_exchange_strong(&g_cc_excl_evt_rings[i],
                                               &expected, ring)) {
                break;
            }
        }
    }
    cc__excl_evt_rec* r = &ring->recs[ring->pos++ % CC_EXCL_EVT_RING];
    r->kind = kind;
    r->fiber = fiber;
    r->node = node;
    r->aux = aux;
    r->t = cc__excl_now();
}

static size_t cc__exclusive_slot_cap(size_t cap, uint64_t name) {
    return (size_t)(name * 11400714819323198485ull) & (cap - 1);
}

static CCExclusiveMap* cc__exclusive_map_alloc(CCArena* arena, size_t cap) {
    size_t bytes = sizeof(CCExclusiveMap) + cap * sizeof(CCExclusiveBucket);
    CCExclusiveMap* m = (CCExclusiveMap*)cc_arena_alloc(
        arena, bytes, _Alignof(CCExclusiveMap));
    if (!m) return NULL;
    memset(m, 0, bytes);
    m->cap = cap;
    return m;
}

static void cc__exclusive_map_insert(CCExclusiveMap* m, CCExclusiveEntry* e) {
    size_t start = cc__exclusive_slot_cap(m->cap, e->name);
    for (size_t i = 0; i < m->cap; i++) {
        size_t idx = (start + i) & (m->cap - 1);
        CCExclusiveEntry* slot = atomic_load_explicit(
            &m->buckets[idx].entry, memory_order_relaxed);
        if (!slot || cc__excl_is_tomb(slot)) {
            atomic_store_explicit(&m->buckets[idx].entry, e, memory_order_relaxed);
            return;
        }
    }
    /* Caller sized the map for existing live entries; must not fill. */
    abort();
}

/* create_mu held.  Retires the old map for release at destroy.  Rehash drops
 * tombstones (only live entries are copied). */
static int cc__exclusive_grow(CCExclusive* excl) {
    CCExclusiveMap* old = excl->map;
    size_t new_cap = old->cap * 2;
    CCExclusiveMap* neu = cc__exclusive_map_alloc(excl->arena, new_cap);
    if (!neu) return -1;

    for (size_t i = 0; i < old->cap; i++) {
        CCExclusiveEntry* e = atomic_load_explicit(
            &old->buckets[i].entry, memory_order_relaxed);
        if (e && !cc__excl_is_tomb(e)) cc__exclusive_map_insert(neu, e);
    }

    atomic_thread_fence(memory_order_release);
    excl->map = neu;
    old->retired_next = excl->retired;
    excl->retired = old;
    return 0;
}

static CCExclusiveEntry* cc__exclusive_lookup(CCExclusive* excl, uint64_t name) {
    CCExclusiveMap* m = excl->map;
    if (!m) return NULL;
    size_t start = cc__exclusive_slot_cap(m->cap, name);
    for (size_t i = 0; i < m->cap; i++) {
        size_t idx = (start + i) & (m->cap - 1);
        CCExclusiveEntry* e = atomic_load_explicit(
            &m->buckets[idx].entry, memory_order_acquire);
        if (!e) return NULL;
        if (cc__excl_is_tomb(e)) continue;
        if (e->name == name) return e;
    }
    return NULL;
}

static CCExclusiveEntry* cc__exclusive_install(
        CCExclusive* excl, CCExclusiveMap* m, uint64_t name) {
    size_t start = cc__exclusive_slot_cap(m->cap, name);
    size_t tomb_idx = (size_t)-1;
    for (size_t i = 0; i < m->cap; i++) {
        size_t idx = (start + i) & (m->cap - 1);
        CCExclusiveEntry* slot = atomic_load_explicit(
            &m->buckets[idx].entry, memory_order_relaxed);
        if (!slot) {
            if (tomb_idx != (size_t)-1) idx = tomb_idx;
            CCExclusiveEntry* e = cc__excl_entry_alloc(excl);
            if (!e) return NULL;
            memset(e, 0, sizeof(*e));
            e->name = name;
            atomic_store_explicit(&e->locked, CC_EXCL_FREE, memory_order_relaxed);
            atomic_store_explicit(&e->wait_spin, 0, memory_order_relaxed);
            atomic_store_explicit(&m->buckets[idx].entry, e, memory_order_release);
            excl->count++;
            return e;
        }
        if (cc__excl_is_tomb(slot) && tomb_idx == (size_t)-1) {
            tomb_idx = idx;
        }
    }
    if (tomb_idx != (size_t)-1) {
        CCExclusiveEntry* e = cc__excl_entry_alloc(excl);
        if (!e) return NULL;
        memset(e, 0, sizeof(*e));
        e->name = name;
        atomic_store_explicit(&e->locked, CC_EXCL_FREE, memory_order_relaxed);
        atomic_store_explicit(&e->wait_spin, 0, memory_order_relaxed);
        atomic_store_explicit(&m->buckets[tomb_idx].entry, e, memory_order_release);
        excl->count++;
        return e;
    }
    return NULL; /* map full of live entries */
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

    for (;;) {
        CCExclusiveMap* m = excl->map;
        /* Keep load comfortable; grow before open-addressing degrades. */
        if (excl->count * 4 >= m->cap * 3) {
            if (cc__exclusive_grow(excl) != 0) {
                pthread_mutex_unlock(&excl->create_mu);
                return NULL;
            }
            continue;
        }

        CCExclusiveEntry* e = cc__exclusive_install(excl, m, name);
        if (e) {
            pthread_mutex_unlock(&excl->create_mu);
            return e;
        }
        /* Completely full — grow and retry. */
        if (cc__exclusive_grow(excl) != 0) {
            pthread_mutex_unlock(&excl->create_mu);
            return NULL;
        }
    }
}

static int cc__exclusive_try_lock(CCExclusiveEntry* e) {
    int expected = CC_EXCL_FREE;
    return atomic_compare_exchange_strong_explicit(
        &e->locked, &expected, CC_EXCL_LOCKED,
        memory_order_acquire, memory_order_relaxed);
}

/* Unlink `node` from the wait queue.  Returns 1 if it was still queued;
 * 0 if a concurrent unlock already popped it. */
static int cc__exclusive_dequeue(CCExclusiveEntry* e, CCExclusiveWaiter* node) {
    cc__spin_lock(&e->wait_spin);
    CCExclusiveWaiter* prev = NULL;
    CCExclusiveWaiter* cur = e->wait_head;
    while (cur && cur != node) {
        prev = cur;
        cur = cur->next;
    }
    int found = (cur == node);
    if (found) {
        if (prev) prev->next = node->next;
        else e->wait_head = node->next;
        if (e->wait_tail == node) e->wait_tail = prev;
        node->next = NULL;
    }
    cc__spin_unlock(&e->wait_spin);
    return found;
}

static void cc__exclusive_lock_entry(CCExclusiveEntry* e) {
    for (int i = 0; i < CC_EXCL_SPIN_TRIES; i++) {
        if (atomic_load_explicit(&e->locked, memory_order_relaxed) == CC_EXCL_FREE
                && cc__exclusive_try_lock(e)) {
            return;
        }
        cc__cpu_pause();
    }

    if (!cc__fiber_in_context()) {
        /* Plain threads cannot park; spin until acquired. */
        for (;;) {
            if (atomic_load_explicit(&e->locked, memory_order_relaxed) == CC_EXCL_FREE
                    && cc__exclusive_try_lock(e)) {
                return;
            }
            cc__cpu_pause();
        }
    }

    for (;;) {
        if (atomic_exchange_explicit(&e->locked, CC_EXCL_CONTENDED,
                                     memory_order_acquire) == CC_EXCL_FREE) {
            return;
        }

        CCExclusiveWaiter node;
        node.fiber = cc__fiber_current();
        node.next = NULL;
        atomic_store_explicit(&node.ready, 0, memory_order_relaxed);
        int dbg_slot = cc__excl_dbg_register(e, &node);

        cc__spin_lock(&e->wait_spin);
        if (!e->wait_head) e->wait_head = &node;
        else e->wait_tail->next = &node;
        e->wait_tail = &node;
        cc__spin_unlock(&e->wait_spin);
        cc__excl_evt(CC_EVT_ENQ, node.fiber, &node, (long)e->name);

        /* Close the enqueue/unlock race: an unlock that swapped FREE before
         * our node was visible woke nobody, and nobody else may come.  Now
         * that we are queued, re-contend once. */
        if (atomic_exchange_explicit(&e->locked, CC_EXCL_CONTENDED,
                                     memory_order_acquire) == CC_EXCL_FREE) {
            int found = cc__exclusive_dequeue(e, &node);
            cc__excl_evt(CC_EVT_ACQ2, node.fiber, &node, found);
            if (!found) {
                /* A concurrent unlock popped our node and will still write
                 * node.ready (and unpark us).  Wait for that write before
                 * the node's stack frame dies; the pending unpark signal is
                 * consumed harmlessly by our next park. */
                while (atomic_load_explicit(&node.ready, memory_order_acquire) == 0) {
                    cc__cpu_pause();
                }
            }
            cc__excl_dbg_unregister(dbg_slot);
            return;
        }

        while (atomic_load_explicit(&node.ready, memory_order_acquire) == 0) {
            cc__excl_evt(CC_EVT_PARK, node.fiber, &node, 0);
            CC_FIBER_PARK("exclusive_lock");
            cc__excl_evt(CC_EVT_WAKE, node.fiber, &node,
                         atomic_load_explicit(&node.ready, memory_order_acquire));
        }
        cc__excl_evt(CC_EVT_EXIT, node.fiber, &node, 1);
        cc__excl_dbg_unregister(dbg_slot);
        /* Woken with no ownership (barging): loop and re-contend. */
    }
}

void cc_exclusive_lock_entry_slow(void* entry) {
    CCExclusiveEntry* e = (CCExclusiveEntry*)entry;
    if (!e) abort();
    cc__exclusive_lock_entry(e);
}

void cc_exclusive_unlock_contended(void* entry) {
    CCExclusiveEntry* e = (CCExclusiveEntry*)entry;
    if (!e) abort();

    cc__spin_lock(&e->wait_spin);
    CCExclusiveWaiter* w = e->wait_head;
    void* fiber = NULL;
    if (w) {
        e->wait_head = w->next;
        if (!e->wait_head) e->wait_tail = NULL;
        w->next = NULL;
        fiber = w->fiber;
    }
    cc__spin_unlock(&e->wait_spin);

    /* Empty queue: the waiter that marked CONTENDED has not enqueued yet;
     * its post-enqueue re-contend will observe FREE. */
    if (!w) {
        cc__excl_evt(CC_EVT_EMPTY, cc__fiber_current(), e, (long)e->name);
        return;
    }

    cc__excl_evt(CC_EVT_POP, fiber, w, (long)(uintptr_t)cc__fiber_current());
    /* Do not touch the stack waiter after publishing ready. */
    atomic_store_explicit(&w->ready, 1, memory_order_release);
    if (fiber) cc__fiber_unpark(fiber);
    cc__excl_evt(CC_EVT_UNPARK, fiber, w, (long)(uintptr_t)cc__fiber_current());
}

/* CC_EXCL_DEBUG=1: dump the lock table on SIGUSR1 (the deadlock detector
 * uses _exit, so atexit does not run). Diagnostic only. */
static CCExclusive* g_cc_excl_debug_last;

#define CC_EXCL_DBG_SLOTS 512
typedef struct {
    _Atomic(CCExclusiveWaiter*) node;
    void* entry;
} cc__excl_dbg_slot;
static cc__excl_dbg_slot g_cc_excl_dbg_waiters[CC_EXCL_DBG_SLOTS];

static int cc__excl_dbg_register(CCExclusiveEntry* e, CCExclusiveWaiter* node) {
    if (!g_cc_excl_debug) return -1;
    for (int i = 0; i < CC_EXCL_DBG_SLOTS; i++) {
        CCExclusiveWaiter* expected = NULL;
        if (atomic_compare_exchange_strong(&g_cc_excl_dbg_waiters[i].node,
                                           &expected, node)) {
            g_cc_excl_dbg_waiters[i].entry = e;
            return i;
        }
    }
    return -1;
}

static void cc__excl_dbg_unregister(int slot) {
    if (slot < 0) return;
    atomic_store(&g_cc_excl_dbg_waiters[slot].node, NULL);
}

static void cc__exclusive_atexit_dump(void) {
    CCExclusive* excl = g_cc_excl_debug_last;
    if (!excl) return;
    CCExclusiveMap* m = excl->map;
    fprintf(stderr, "[cc_exclusive] table dump cap=%zu count=%zu:\n",
            m ? m->cap : 0, excl->count);
    if (!m) return;
    for (size_t i = 0; i < m->cap; i++) {
        CCExclusiveEntry* e = atomic_load_explicit(
            &m->buckets[i].entry, memory_order_acquire);
        if (!e || cc__excl_is_tomb(e)) continue;
        int word = atomic_load_explicit(&e->locked, memory_order_acquire);
        fprintf(stderr, "  name=%llu word=%d queue=[",
                (unsigned long long)e->name, word);
        for (CCExclusiveWaiter* w = e->wait_head; w; w = w->next) {
            fprintf(stderr, "fiber=%p ready=%d; ", w->fiber,
                    atomic_load_explicit(&w->ready, memory_order_acquire));
        }
        fprintf(stderr, "]\n");
    }
    fprintf(stderr, "[cc_exclusive] live waiters (parked or in slow path):\n");
    void* stranded[CC_EXCL_DBG_SLOTS];
    int n_stranded = 0;
    for (int i = 0; i < CC_EXCL_DBG_SLOTS; i++) {
        CCExclusiveWaiter* w = atomic_load(&g_cc_excl_dbg_waiters[i].node);
        if (!w) continue;
        CCExclusiveEntry* e = (CCExclusiveEntry*)g_cc_excl_dbg_waiters[i].entry;
        fprintf(stderr, "  node=%p fiber=%p ready=%d entry_name=%llu word=%d\n",
                (void*)w, w->fiber,
                atomic_load_explicit(&w->ready, memory_order_acquire),
                (unsigned long long)(e ? e->name : 0),
                e ? atomic_load_explicit(&e->locked, memory_order_acquire) : -1);
        if (w->fiber) {
            sched_v2_debug_dump_fiber(
                (void*)((uintptr_t)w->fiber & ~(uintptr_t)1), "    sched: ");
        }
        if (n_stranded < CC_EXCL_DBG_SLOTS) stranded[n_stranded++] = w->fiber;
    }

    static const char* kind_names[] = {"?", "ENQ", "PARK", "WAKE",
                                       "POP", "UNPARK", "ACQ2", "EXIT",
                                       "EMPTY"};
    fprintf(stderr, "[cc_exclusive] event history for stranded fibers "
                    "(unsorted; sort by t):\n");
    for (int ti = 0; ti < CC_EXCL_EVT_MAX_THREADS; ti++) {
        cc__excl_evt_ring_t* ring = atomic_load(&g_cc_excl_evt_rings[ti]);
        if (!ring) continue;
        for (int j = 0; j < CC_EXCL_EVT_RING; j++) {
            cc__excl_evt_rec* r = &ring->recs[j];
            if (r->t == 0) continue;
            int relevant = 0;
            for (int i = 0; i < n_stranded; i++) {
                if (r->fiber == stranded[i]) { relevant = 1; break; }
            }
            if (!relevant) continue;
            fprintf(stderr, "  t=%llu %-6s fiber=%p node=%p aux=%#lx\n",
                    (unsigned long long)r->t,
                    r->kind >= 1 && r->kind <= 8 ? kind_names[r->kind] : "?",
                    r->fiber, r->node, (unsigned long)r->aux);
        }
    }
}

/* Watchdog: a waiter that stays registered with ready==1 for ~300ms is
 * stranded (its wake was lost).  Freeze the event rings immediately so the
 * history around the strand survives, then dump. */
static void* cc__excl_watchdog_main(void* arg) {
    (void)arg;
    CCExclusiveWaiter* prev_node[CC_EXCL_DBG_SLOTS] = {0};
    int count[CC_EXCL_DBG_SLOTS] = {0};
    for (;;) {
        struct timespec ts = {0, 50 * 1000 * 1000};
        nanosleep(&ts, NULL);
        for (int i = 0; i < CC_EXCL_DBG_SLOTS; i++) {
            CCExclusiveWaiter* w = atomic_load(&g_cc_excl_dbg_waiters[i].node);
            if (w && w == prev_node[i] &&
                atomic_load_explicit(&w->ready, memory_order_acquire) == 1) {
                if (++count[i] >= 6) {
                    atomic_store(&g_cc_excl_evt_freeze, 1);
                    fprintf(stderr,
                            "[cc_exclusive] WATCHDOG: stranded waiter node=%p "
                            "fiber=%p; rings frozen, dumping\n",
                            (void*)w, w->fiber);
                    cc__exclusive_atexit_dump();
                    return NULL;
                }
            } else {
                count[i] = 0;
            }
            prev_node[i] = w;
        }
    }
    return NULL;
}

static size_t cc__exclusive_round_cap(size_t initial_cap) {
    size_t cap = initial_cap ? initial_cap : CC_EXCLUSIVE_DEFAULT_CAP;
    if (cap < 2) cap = 2;
    /* Next power of two (cap is never 0 here). */
    cap--;
    cap |= cap >> 1;
    cap |= cap >> 2;
    cap |= cap >> 4;
    cap |= cap >> 8;
    cap |= cap >> 16;
#if SIZE_MAX > 0xffffffffu
    cap |= cap >> 32;
#endif
    cap++;
    return cap;
}

CCExclusive* cc_exclusive_create(CCArena* arena, size_t initial_cap) {
    if (!arena) return NULL;

    CCExclusive* excl = (CCExclusive*)cc_arena_alloc(
        arena, sizeof(CCExclusive), _Alignof(CCExclusive));
    if (!excl) return NULL;
    memset(excl, 0, sizeof(*excl));
    excl->arena = arena;

    CCExclusiveMap* map =
        cc__exclusive_map_alloc(arena, cc__exclusive_round_cap(initial_cap));
    if (!map) return NULL;
    excl->map = map;
    cc_arena_pool_init(&excl->entry_pool, arena, sizeof(CCExclusiveEntry));

    pthread_mutex_init(&excl->create_mu, NULL);
    if (getenv("CC_EXCL_DEBUG")) {
        g_cc_excl_debug_last = excl;
        g_cc_excl_debug = 1;
        static int registered;
        if (!registered) {
            registered = 1;
            signal(SIGUSR1, (void (*)(int))cc__exclusive_atexit_dump);
            pthread_t wd;
            pthread_create(&wd, NULL, cc__excl_watchdog_main, NULL);
            pthread_detach(wd);
        }
    }
    return excl;
}

void cc_exclusive_destroy(CCExclusive* excl) {
    if (!excl) return;
    pthread_mutex_destroy(&excl->create_mu);

    CCExclusiveMap* cur = excl->map;
    excl->map = NULL;
    if (cur) (void)cc_arena_release(excl->arena, cur);

    CCExclusiveMap* r = excl->retired;
    excl->retired = NULL;
    while (r) {
        CCExclusiveMap* next = r->retired_next;
        (void)cc_arena_release(excl->arena, r);
        r = next;
    }
}

CCExclusiveMutex cc_exclusive_mutex(CCExclusive* excl, uint64_t name) {
    CCExclusiveMutex m = {0};
    m.excl = excl;
    m.name = name;
    if (!excl) return m;
    CCExclusiveEntry* e = cc__exclusive_get_or_create(excl, name);
    if (!e) abort();
    m._entry = e;
    return m;
}

void cc_exclusive_mutex_free(CCExclusiveMutex* m) {
    if (!m || !m->excl || !m->_entry) return;

    CCExclusive* excl = m->excl;
    CCExclusiveEntry* e = (CCExclusiveEntry*)m->_entry;
    uint64_t name = m->name;

    pthread_mutex_lock(&excl->create_mu);

    CCExclusiveEntry* cur = cc__exclusive_lookup(excl, name);
    if (cur != e) {
        /* Already freed or replaced; just clear the handle. */
        pthread_mutex_unlock(&excl->create_mu);
        m->_entry = NULL;
        m->excl = NULL;
        m->name = 0;
        return;
    }

    if (atomic_load_explicit(&e->locked, memory_order_acquire) != CC_EXCL_FREE
            || e->wait_head != NULL) {
        pthread_mutex_unlock(&excl->create_mu);
        abort(); /* free while held or waiters queued */
    }

    CCExclusiveMap* map = excl->map;
    size_t start = cc__exclusive_slot_cap(map->cap, name);
    for (size_t i = 0; i < map->cap; i++) {
        size_t idx = (start + i) & (map->cap - 1);
        CCExclusiveEntry* slot = atomic_load_explicit(
            &map->buckets[idx].entry, memory_order_relaxed);
        if (!slot) break;
        if (slot == e) {
            atomic_store_explicit(
                &map->buckets[idx].entry, CC_EXCL_TOMBSTONE, memory_order_release);
            break;
        }
    }

    if (excl->count > 0) excl->count--;
    e->name = 0;
    e->wait_head = NULL;
    e->wait_tail = NULL;
    atomic_store_explicit(&e->locked, CC_EXCL_FREE, memory_order_relaxed);
    atomic_store_explicit(&e->wait_spin, 0, memory_order_relaxed);
    cc__excl_entry_free(excl, e);

    pthread_mutex_unlock(&excl->create_mu);

    m->_entry = NULL;
    m->excl = NULL;
    m->name = 0;
}

CCShardMask cc_shard_mask_auto(size_t max) {
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;
    /* Ceil to next pow2 so non-pow2 host counts (6/12/24) do not silently
     * run at half the shard fan-out; clamp still floors explicit overrides. */
    return cc_shard_mask_ceil((size_t)ncpu, max);
}



