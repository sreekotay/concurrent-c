/*
 * Hybrid Scheduler V2 — signal-based fiber/thread scheduler.
 *
 * Design:
 *   - Hybrid V2 work items are pooled and reused.
 *   - Threads are OS threads, pooled and elastic (sysmon expands).
 *   - Single global ready queue. No per-thread inboxes.
 *   - Central wake(worker_hint) drain loop pairs idle workers with ready fibers.
 *   - Invariant: runnable_work > 0 && idle_workers > 0 never persists.
 *
 * Current semantics:
 *   - spawnhybrid is a run-to-completion scheduler.
 *   - V2 work items do not preserve a suspendable C stack.
 *   - Any attempt to park/yield from V2 is a runtime bug and fails fast.
 *
 * wake(worker_hint):
 *   If hint==self: drain ready queue inline (pop, run, repeat).
 *   If hint<0:     wake idle workers while ready queue non-empty.
 *
 * Call sites:
 *   signal(fiber): enqueue to ready_queue, wake(-1)
 *   spawn(fiber):  enqueue to ready_queue, wake(-1)
 *   fiber_park:    move fiber, wake(self)  — this worker is now free
 *   fiber_finish:  wake(self)              — this worker is now free
 */

#include "sched_v2.h"
#include "wake_primitive.h"
#include "fiber_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define V2_MAX_THREADS     64
#define V2_GLOBAL_QUEUE_SIZE 4096
#define V2_FIBER_STACK_SIZE (2 * 1024 * 1024)
#define V2_SYSMON_INTERVAL_MS 100

/* ============================================================================
 * Fiber
 * ============================================================================ */

enum {
    V2_YIELD_PARK = 0,  /* Fiber wants to park (default after yield) */
    V2_YIELD_YIELD = 1, /* Fiber wants to re-enqueue (voluntary yield) */
};

struct fiber_v2 {
    void*      coro; /* Unused in the current run-to-completion prototype. */
    _Atomic int state;
    int        last_thread_id;
    uint64_t   generation;

    void* (*entry_fn)(void*);
    void*      entry_arg;
    void*      result;
    _Atomic int done;         /* Set to 1 when fiber completes, before recycling */
    char       result_buf[48] __attribute__((aligned(8)));
    _Atomic uint64_t wait_ticket;
    int        yield_kind;      /* V2_YIELD_PARK or V2_YIELD_YIELD */
    const char* park_reason;
    wake_primitive done_wake;
    cc__fiber* _Atomic join_waiter_fiber;

    /* Intrusive linked list for free list */
    fiber_v2*  next;
    fiber_v2*  all_next;
};

/* ============================================================================
 * Simple intrusive MPSC queue (mutex + linked list)
 * ============================================================================ */

typedef struct {
    pthread_mutex_t mu;
    fiber_v2* head;
    fiber_v2* tail;
    _Atomic size_t count;
} v2_queue;

static void v2_queue_init(v2_queue* q) {
    pthread_mutex_init(&q->mu, NULL);
    q->head = NULL;
    q->tail = NULL;
    atomic_store_explicit(&q->count, 0, memory_order_relaxed);
}

static int v2_queue_push(v2_queue* q, fiber_v2* f) {
    f->next = NULL;
    pthread_mutex_lock(&q->mu);
    if (q->tail) {
        q->tail->next = f;
    } else {
        q->head = f;
    }
    q->tail = f;
    atomic_fetch_add_explicit(&q->count, 1, memory_order_relaxed);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

static fiber_v2* v2_queue_pop(v2_queue* q) {
    if (atomic_load_explicit(&q->count, memory_order_relaxed) == 0) return NULL;
    pthread_mutex_lock(&q->mu);
    fiber_v2* f = q->head;
    if (f) {
        q->head = f->next;
        if (!q->head) q->tail = NULL;
        f->next = NULL;
        atomic_fetch_sub_explicit(&q->count, 1, memory_order_relaxed);
    }
    pthread_mutex_unlock(&q->mu);
    return f;
}

/* ============================================================================
 * Thread state
 * ============================================================================ */

typedef struct {
    pthread_t    handle;
    int          id;
    int          alive;
    _Atomic int  idle;   /* 1 = sleeping/available for work */
    wake_primitive wake;
} thread_v2;

/* ============================================================================
 * Global scheduler state
 * ============================================================================ */

struct sched_v2_state {
    thread_v2    threads[V2_MAX_THREADS];
    _Atomic int  num_threads;
    _Atomic int  running;
    _Atomic int  idle_threads;
    int          allow_expand;

    /* Single global ready queue */
    v2_queue ready_queue;

    /* Fiber free list (lock-free CAS stack) */
    fiber_v2* _Atomic free_list;
    pthread_mutex_t all_fibers_mu;
    fiber_v2* all_fibers;
    _Atomic size_t fiber_count;

    /* Sysmon */
    pthread_t    sysmon_handle;
    wake_primitive sysmon_wake;

    /* Init */
    pthread_once_t init_once;
    int            initialized;
};

static struct sched_v2_state g_v2 = {
    .init_once = PTHREAD_ONCE_INIT,
};

static _Atomic uint64_t g_v2_sysmon_stall_detect = 0;
extern void cc__io_wait_dump_kq_diag(void);
extern void cc__fiber_dump_unpark_reason_stats(void);
extern void cc_sched_wait_many_dump_diag(void);
extern void cc__socket_wait_dump_diag(void);

/* Diagnostic counters */
static _Atomic uint64_t g_v2_fibers_alive = 0;
static _Atomic uint64_t g_v2_signal_ok = 0;
static _Atomic uint64_t g_v2_signal_pending = 0;
static _Atomic uint64_t g_v2_signal_running_set = 0;
static _Atomic uint64_t g_v2_signal_running_already_set = 0;
static _Atomic uint64_t g_v2_signal_dropped = 0;
static _Atomic uint64_t g_v2_signal_dropped_state[6] = {0};
static _Atomic uint64_t g_v2_parks = 0;
static _Atomic uint64_t g_v2_park_consumed_pending = 0;
static _Atomic uint64_t g_v2_park_yield_no_pending = 0;
static _Atomic uint64_t g_v2_run_yield_requeue = 0;
static _Atomic uint64_t g_v2_run_yield_requeue_with_pending = 0;
static _Atomic uint64_t g_v2_run_dead = 0;
static _Atomic uint64_t g_v2_run_dead_with_pending = 0;
static _Atomic uint64_t g_v2_run_prepark_pending = 0;
static _Atomic uint64_t g_v2_run_commit_parked = 0;
static _Atomic uint64_t g_v2_run_postpark_pending = 0;
static _Atomic uint64_t g_v2_run_postpark_requeue = 0;
static _Atomic uint64_t g_v2_run_postpark_cas_fail_state[6] = {0};
static _Atomic uint64_t g_v2_mco_resume_fail = 0;
static _Atomic uint64_t g_v2_mco_yield_fail = 0;
static _Atomic uint64_t g_v2_mco_stack_overflow = 0;
static _Atomic uint64_t g_v2_mco_fail_logs = 0;

static void sched_v2_diag_scan_fibers(uint64_t state_counts[6],
                                      uint64_t* parked_wait_many,
                                      uint64_t* parked_wait,
                                      uint64_t* parked_other,
                                      uint64_t* parked_unknown) {
    for (int i = 0; i < 6; ++i) {
        state_counts[i] = 0;
    }
    *parked_wait_many = 0;
    *parked_wait = 0;
    *parked_other = 0;
    *parked_unknown = 0;

    pthread_mutex_lock(&g_v2.all_fibers_mu);
    for (fiber_v2* f = g_v2.all_fibers; f; f = f->all_next) {
        int state = atomic_load_explicit(&f->state, memory_order_acquire);
        if (state >= 0 && state <= 5) {
            state_counts[state]++;
            if (state == FIBER_V2_PARKED) {
                if (!f->park_reason) {
                    (*parked_unknown)++;
                } else if (strcmp(f->park_reason, "cc_sched_fiber_wait_many") == 0) {
                    (*parked_wait_many)++;
                } else if (strcmp(f->park_reason, "cc_sched_fiber_wait") == 0) {
                    (*parked_wait)++;
                } else {
                    (*parked_other)++;
                }
            }
        }
    }
    pthread_mutex_unlock(&g_v2.all_fibers_mu);
}

/* Per-thread state */
static __thread int tls_v2_thread_id = -1;
static __thread fiber_v2* tls_v2_current_fiber = NULL;
static __thread const char* tls_v2_park_reason = NULL;

__attribute__((noreturn))
static void sched_v2_fail_stackless_suspend(const char* op) {
    fprintf(stderr,
            "[sched_v2] stackless spawnhybrid does not support %s; "
            "use explicit async/state-machine suspension instead\n",
            op ? op : "suspension");
    abort();
}

/* ============================================================================
 * Fiber pool
 * ============================================================================ */

static fiber_v2* fiber_v2_alloc(void) {
    /* Try free list first */
    fiber_v2* f = atomic_load_explicit(&g_v2.free_list, memory_order_acquire);
    while (f) {
        fiber_v2* next = f->next;
        if (atomic_compare_exchange_weak_explicit(&g_v2.free_list, &f, next,
                memory_order_release, memory_order_acquire)) {
            f->generation++;
            f->next = NULL;
            f->coro = NULL;
            f->result = NULL;
            f->entry_fn = NULL;
            f->entry_arg = NULL;
            f->last_thread_id = -1;
            atomic_store_explicit(&f->done, 0, memory_order_relaxed);
            atomic_store_explicit(&f->wait_ticket, 0, memory_order_relaxed);
            atomic_store_explicit(&f->join_waiter_fiber, NULL, memory_order_relaxed);
            f->yield_kind = V2_YIELD_PARK;
            f->park_reason = NULL;
            atomic_store_explicit(&f->state, FIBER_V2_IDLE, memory_order_relaxed);
            atomic_fetch_add_explicit(&g_v2_fibers_alive, 1, memory_order_relaxed);
            return f;
        }
    }

    f = (fiber_v2*)calloc(1, sizeof(fiber_v2));
    if (!f) return NULL;
    f->coro = NULL;
    f->last_thread_id = -1;
    f->park_reason = NULL;
    atomic_store_explicit(&f->join_waiter_fiber, NULL, memory_order_relaxed);
    wake_primitive_init(&f->done_wake);

    atomic_fetch_add_explicit(&g_v2.fiber_count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_v2_fibers_alive, 1, memory_order_relaxed);
    pthread_mutex_lock(&g_v2.all_fibers_mu);
    f->all_next = g_v2.all_fibers;
    g_v2.all_fibers = f;
    pthread_mutex_unlock(&g_v2.all_fibers_mu);
    return f;
}

static void fiber_v2_free(fiber_v2* f) {
    atomic_fetch_sub_explicit(&g_v2_fibers_alive, 1, memory_order_relaxed);
    atomic_store_explicit(&f->state, FIBER_V2_IDLE, memory_order_relaxed);
    f->entry_fn = NULL;
    f->entry_arg = NULL;
    f->result = NULL;
    fiber_v2* head;
    do {
        head = atomic_load_explicit(&g_v2.free_list, memory_order_relaxed);
        f->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&g_v2.free_list, &head, f,
            memory_order_release, memory_order_relaxed));
}

/* ============================================================================
 * Wake infrastructure: try_wake_one, try_expand_pool, sched_v2_wake
 * ============================================================================ */

static void* thread_v2_main(void* arg);

static inline int sched_v2_finish_join(fiber_v2* f, void** out_result) {
    if (out_result) *out_result = f->result;
    return 0;
}

/* Claim one idle worker thread and wake it. Returns 1 on success. */
static int sched_v2_try_wake_one(void) {
    if (atomic_load_explicit(&g_v2.idle_threads, memory_order_acquire) <= 0) {
        return 0;
    }
    int n = atomic_load_explicit(&g_v2.num_threads, memory_order_acquire);
    for (int i = 0; i < n; i++) {
        int exp = 1;
        if (atomic_compare_exchange_strong_explicit(&g_v2.threads[i].idle, &exp, 0,
                memory_order_acq_rel, memory_order_relaxed)) {
            atomic_fetch_sub_explicit(&g_v2.idle_threads, 1, memory_order_acq_rel);
            wake_primitive_wake_one(&g_v2.threads[i].wake);
            return 1;
        }
    }
    return 0;
}

/* Expand thread pool by one. Returns 1 if a thread was created. */
static int sched_v2_try_expand_pool(void) {
    if (!g_v2.allow_expand) return 0;
    int current = atomic_load_explicit(&g_v2.num_threads, memory_order_acquire);
    if (current >= V2_MAX_THREADS) return 0;
    int new_id = atomic_fetch_add_explicit(&g_v2.num_threads, 1, memory_order_acq_rel);
    if (new_id >= V2_MAX_THREADS) {
        atomic_fetch_sub_explicit(&g_v2.num_threads, 1, memory_order_relaxed);
        return 0;
    }
    g_v2.threads[new_id].id = new_id;
    atomic_store_explicit(&g_v2.threads[new_id].idle, 0, memory_order_relaxed);
    wake_primitive_init(&g_v2.threads[new_id].wake);
    g_v2.threads[new_id].alive = 0;
    pthread_create(&g_v2.threads[new_id].handle, NULL,
                   thread_v2_main, (void*)(intptr_t)new_id);
    return 1;
}

static void thread_v2_run_fiber(int tid, fiber_v2* f);

/*
 * Central wake primitive.
 *
 * worker_hint >= 0 (must equal tls_v2_thread_id):
 *   Caller offers itself as a free worker. Drains ready queue inline:
 *   pop fiber, run it, repeat until queue empty.
 *
 * worker_hint < 0:
 *   External caller (kqueue thread, channel code, sysmon).
 *   Wakes idle workers while the ready queue has work.
 */
static void sched_v2_wake(int worker_hint) {
    if (worker_hint >= 0 && worker_hint == tls_v2_thread_id) {
        while (atomic_load_explicit(&g_v2.running, memory_order_acquire)) {
            fiber_v2* f = v2_queue_pop(&g_v2.ready_queue);
            if (!f) return;
            thread_v2_run_fiber(worker_hint, f);
        }
        return;
    }

    /* External: wake idle workers while there's work in the queue. */
    if (atomic_load_explicit(&g_v2.idle_threads, memory_order_acquire) <= 0) {
        return;
    }
    while (atomic_load_explicit(&g_v2.ready_queue.count, memory_order_relaxed) > 0 &&
           atomic_load_explicit(&g_v2.idle_threads, memory_order_acquire) > 0) {
        if (!sched_v2_try_wake_one()) {
            sched_v2_try_expand_pool();
            break;
        }
    }
}

/* ============================================================================
 * Run fiber + post-yield commit
 * ============================================================================ */

static void thread_v2_run_fiber(int tid, fiber_v2* f) {
    int expected = atomic_load_explicit(&f->state, memory_order_acquire);
    if (expected != FIBER_V2_QUEUED) {
        fprintf(stderr, "[sched_v2] BUG: thread %d got fiber in state %d\n", tid, expected);
        return;
    }
    atomic_store_explicit(&f->state, FIBER_V2_RUNNING, memory_order_release);
    f->last_thread_id = tid;
    tls_v2_current_fiber = f;
    f->park_reason = NULL;
    f->yield_kind = V2_YIELD_PARK;
    f->result = f->entry_fn ? f->entry_fn(f->entry_arg) : NULL;

    tls_v2_current_fiber = NULL;
    atomic_fetch_add_explicit(&g_v2_sysmon_stall_detect, 1, memory_order_relaxed);
    atomic_store_explicit(&f->state, FIBER_V2_DEAD, memory_order_release);
    atomic_store_explicit(&f->done, 1, memory_order_release);
    cc__fiber* waiter =
        atomic_exchange_explicit(&f->join_waiter_fiber, NULL, memory_order_acq_rel);
    if (waiter) {
        cc__fiber_unpark_tagged(waiter, CC_FIBER_UNPARK_REASON_TASK_DONE);
    }
    wake_primitive_wake_all(&f->done_wake);
    atomic_fetch_add_explicit(&g_v2_run_dead, 1, memory_order_relaxed);
}

/* ============================================================================
 * Signal: publish runnable work through state only
 * ============================================================================ */

void sched_v2_signal(fiber_v2* f) {
    while (1) {
        int expected = atomic_load_explicit(&f->state, memory_order_acquire);
        if (expected == FIBER_V2_QUEUED || expected == FIBER_V2_RUNNING) {
            if (expected == FIBER_V2_RUNNING) {
                atomic_fetch_add_explicit(&g_v2_signal_running_already_set, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&g_v2_signal_pending, 1, memory_order_relaxed);
            } else {
                atomic_fetch_add_explicit(&g_v2_signal_ok, 1, memory_order_relaxed);
            }
            return;
        }
        if (expected == FIBER_V2_PARKED) {
            int desired = FIBER_V2_QUEUED;
            if (!atomic_compare_exchange_weak_explicit(&f->state, &expected, desired,
                    memory_order_acq_rel, memory_order_relaxed)) {
                continue;
            }
            atomic_fetch_add_explicit(&g_v2_signal_ok, 1, memory_order_relaxed);
            v2_queue_push(&g_v2.ready_queue, f);
            sched_v2_wake(-1);
            return;
        }
        atomic_fetch_add_explicit(&g_v2_signal_dropped, 1, memory_order_relaxed);
        if (expected >= 0 && expected <= 5)
            atomic_fetch_add_explicit(&g_v2_signal_dropped_state[expected], 1, memory_order_relaxed);
        return;
    }
}

/* ============================================================================
 * Park / Yield (unchanged — fiber-side API)
 * ============================================================================ */

void sched_v2_park(void) {
    fiber_v2* f = tls_v2_current_fiber;
    if (!f) return;

    atomic_fetch_add_explicit(&g_v2_parks, 1, memory_order_relaxed);
    f->park_reason = tls_v2_park_reason;
    sched_v2_fail_stackless_suspend(f->park_reason ? f->park_reason : "park");
}

void sched_v2_yield(void) {
    fiber_v2* f = tls_v2_current_fiber;
    if (!f) return;

    f->yield_kind = V2_YIELD_YIELD;
    sched_v2_fail_stackless_suspend("yield");
}

void sched_v2_set_park_reason(const char* reason) {
    tls_v2_park_reason = reason;
}

/* ============================================================================
 * Context queries
 * ============================================================================ */

int sched_v2_in_context(void) {
    return tls_v2_current_fiber != NULL;
}

fiber_v2* sched_v2_current_fiber(void) {
    return tls_v2_current_fiber;
}

/* ============================================================================
 * Thread main loop
 * ============================================================================ */

static void* thread_v2_main(void* arg) {
    int tid = (int)(intptr_t)arg;
    tls_v2_thread_id = tid;
    g_v2.threads[tid].alive = 1;

    while (atomic_load_explicit(&g_v2.running, memory_order_acquire)) {
        /* Drain: run ready fibers inline until queue is empty. */
        sched_v2_wake(tid);

        if (!atomic_load_explicit(&g_v2.running, memory_order_acquire)) break;

        /* No work — park this worker.
         * Dekker protocol: mark idle, read wake counter, recheck, sleep. */
        atomic_store_explicit(&g_v2.threads[tid].idle, 1, memory_order_release);
        atomic_fetch_add_explicit(&g_v2.idle_threads, 1, memory_order_acq_rel);
        uint32_t val = atomic_load_explicit(&g_v2.threads[tid].wake.value,
                                            memory_order_acquire);

        /* Recheck: work appeared after we marked ourselves idle. */
        if (atomic_load_explicit(&g_v2.ready_queue.count, memory_order_acquire) > 0) {
            if (atomic_exchange_explicit(&g_v2.threads[tid].idle, 0, memory_order_acq_rel)) {
                atomic_fetch_sub_explicit(&g_v2.idle_threads, 1, memory_order_acq_rel);
            }
            continue;
        }

        wake_primitive_wait_timeout(&g_v2.threads[tid].wake, val, 500);
        if (atomic_exchange_explicit(&g_v2.threads[tid].idle, 0, memory_order_acq_rel)) {
            atomic_fetch_sub_explicit(&g_v2.idle_threads, 1, memory_order_acq_rel);
        }
    }

    g_v2.threads[tid].alive = 0;
    return NULL;
}

/* ============================================================================
 * Sysmon
 * ============================================================================ */

static int sched_v2_detect_num_threads(void) {
    const char* env = getenv("CC_V2_THREADS");
    if (env && env[0]) {
        int n = atoi(env);
        if (n > 0 && n <= V2_MAX_THREADS) return n;
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 4;
    if (n > V2_MAX_THREADS) n = V2_MAX_THREADS;
    return (int)n;
}

static int sched_v2_threads_explicitly_configured(void) {
    const char* env = getenv("CC_V2_THREADS");
    return env && env[0];
}

static int sched_v2_count_idle(void) {
    int n = atomic_load_explicit(&g_v2.num_threads, memory_order_relaxed);
    int idle = 0;
    for (int i = 0; i < n; i++) {
        if (atomic_load_explicit(&g_v2.threads[i].idle, memory_order_relaxed))
            idle++;
    }
    return idle;
}

static void* sched_v2_sysmon_main(void* arg) {
    (void)arg;
    uint64_t last_run_count = 0;
    int stall_ticks = 0;
    while (atomic_load_explicit(&g_v2.running, memory_order_acquire)) {
        uint32_t val = atomic_load_explicit(&g_v2.sysmon_wake.value, memory_order_acquire);
        wake_primitive_wait_timeout(&g_v2.sysmon_wake, val, V2_SYSMON_INTERVAL_MS);

        if (!atomic_load_explicit(&g_v2.running, memory_order_acquire)) break;

        uint64_t cur_run = atomic_load_explicit(&g_v2_sysmon_stall_detect, memory_order_relaxed);
        if (cur_run == last_run_count) {
            stall_ticks++;

            /* Safety net: try to wake workers if there's queued work. */
            if (atomic_load_explicit(&g_v2.ready_queue.count, memory_order_relaxed) > 0) {
                sched_v2_wake(-1);
            }

            if (stall_ticks >= 20 && (stall_ticks % 20) == 0) {
                int n = atomic_load_explicit(&g_v2.num_threads, memory_order_relaxed);
                int idle_n = sched_v2_count_idle();
                size_t gq = atomic_load_explicit(&g_v2.ready_queue.count, memory_order_relaxed);
                uint64_t alive = atomic_load_explicit(&g_v2_fibers_alive, memory_order_relaxed);
                uint64_t sig_ok = atomic_load_explicit(&g_v2_signal_ok, memory_order_relaxed);
                uint64_t sig_pend = atomic_load_explicit(&g_v2_signal_pending, memory_order_relaxed);
                uint64_t sig_run_set = atomic_load_explicit(&g_v2_signal_running_set, memory_order_relaxed);
                uint64_t sig_run_already = atomic_load_explicit(&g_v2_signal_running_already_set, memory_order_relaxed);
                uint64_t sig_drop = atomic_load_explicit(&g_v2_signal_dropped, memory_order_relaxed);
                uint64_t parks = atomic_load_explicit(&g_v2_parks, memory_order_relaxed);
                uint64_t park_cp = atomic_load_explicit(&g_v2_park_consumed_pending, memory_order_relaxed);
                uint64_t park_yield = atomic_load_explicit(&g_v2_park_yield_no_pending, memory_order_relaxed);
                uint64_t run_yield = atomic_load_explicit(&g_v2_run_yield_requeue, memory_order_relaxed);
                uint64_t run_dead = atomic_load_explicit(&g_v2_run_dead, memory_order_relaxed);
                uint64_t run_pre = atomic_load_explicit(&g_v2_run_prepark_pending, memory_order_relaxed);
                uint64_t run_parked = atomic_load_explicit(&g_v2_run_commit_parked, memory_order_relaxed);
                uint64_t state_counts[6];
                uint64_t parked_wait_many = 0;
                uint64_t parked_wait = 0;
                uint64_t parked_other = 0;
                uint64_t parked_unknown = 0;
                sched_v2_diag_scan_fibers(state_counts,
                                          &parked_wait_many,
                                          &parked_wait,
                                          &parked_other,
                                          &parked_unknown);
                fprintf(stderr, "[sched_v2 sysmon] STALL #%d: threads=%d idle=%d global_q=%zu fibers_alive=%llu\n",
                        stall_ticks / 20, n, idle_n, gq, (unsigned long long)alive);
                fprintf(stderr, "  signals: ok=%llu pending=%llu dropped=%llu  parks: total=%llu consumed_pending=%llu\n",
                        (unsigned long long)sig_ok, (unsigned long long)sig_pend, (unsigned long long)sig_drop,
                        (unsigned long long)parks, (unsigned long long)park_cp);
                fprintf(stderr, "  wake path: run_to_wake=%llu already_wake=%llu  park(yield=%llu wake_to_run=%llu commit_parked=%llu runwake_to_queue=%llu) exits(dead=%llu yield=%llu) park_fail: IDLE=%llu QUEUED=%llu RUNNING=%llu PARKED=%llu RUN_WAKE=%llu DEAD=%llu\n",
                        (unsigned long long)sig_run_set,
                        (unsigned long long)sig_run_already,
                        (unsigned long long)park_yield,
                        (unsigned long long)park_cp,
                        (unsigned long long)run_parked,
                        (unsigned long long)run_pre,
                        (unsigned long long)run_dead,
                        (unsigned long long)run_yield,
                        (unsigned long long)atomic_load_explicit(&g_v2_run_postpark_cas_fail_state[0], memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_run_postpark_cas_fail_state[1], memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_run_postpark_cas_fail_state[2], memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_run_postpark_cas_fail_state[3], memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_run_postpark_cas_fail_state[4], memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_run_postpark_cas_fail_state[5], memory_order_relaxed));
                fprintf(stderr, "  minicoro: resume_fail=%llu yield_fail=%llu stack_overflow=%llu\n",
                        (unsigned long long)atomic_load_explicit(&g_v2_mco_resume_fail, memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_mco_yield_fail, memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_mco_stack_overflow, memory_order_relaxed));
                fprintf(stderr, "  drop by state: IDLE=%llu QUEUED=%llu RUNNING=%llu PARKED=%llu RUN_WAKE=%llu DEAD=%llu\n",
                        (unsigned long long)atomic_load_explicit(&g_v2_signal_dropped_state[0], memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_signal_dropped_state[1], memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_signal_dropped_state[2], memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_signal_dropped_state[3], memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_signal_dropped_state[4], memory_order_relaxed),
                        (unsigned long long)atomic_load_explicit(&g_v2_signal_dropped_state[5], memory_order_relaxed));
                fprintf(stderr, "  fiber scan: state(IDLE=%llu QUEUED=%llu RUNNING=%llu PARKED=%llu RUN_WAKE=%llu DEAD=%llu)\n",
                        (unsigned long long)state_counts[0],
                        (unsigned long long)state_counts[1],
                        (unsigned long long)state_counts[2],
                        (unsigned long long)state_counts[3],
                        (unsigned long long)state_counts[4],
                        (unsigned long long)state_counts[5]);
                fprintf(stderr, "  parked reasons: wait_many=%llu wait=%llu other=%llu unknown=%llu\n",
                        (unsigned long long)parked_wait_many,
                        (unsigned long long)parked_wait,
                        (unsigned long long)parked_other,
                        (unsigned long long)parked_unknown);
                cc__socket_wait_dump_diag();
                cc__fiber_dump_unpark_reason_stats();
                cc_sched_wait_many_dump_diag();
                cc__io_wait_dump_kq_diag();
            }
        } else {
            stall_ticks = 0;
            last_run_count = cur_run;
        }
    }
    return NULL;
}

/* ============================================================================
 * Init / shutdown
 * ============================================================================ */

static void sched_v2_init_impl(void) {
    atomic_store_explicit(&g_v2.running, 1, memory_order_release);
    atomic_store_explicit(&g_v2.idle_threads, 0, memory_order_relaxed);
    v2_queue_init(&g_v2.ready_queue);
    pthread_mutex_init(&g_v2.all_fibers_mu, NULL);
    g_v2.all_fibers = NULL;
    wake_primitive_init(&g_v2.sysmon_wake);

    int n = sched_v2_detect_num_threads();
    atomic_store_explicit(&g_v2.num_threads, n, memory_order_release);
    g_v2.allow_expand = sched_v2_threads_explicitly_configured() ? 0 : 1;

    for (int i = 0; i < n; i++) {
        g_v2.threads[i].id = i;
        atomic_store_explicit(&g_v2.threads[i].idle, 0, memory_order_relaxed);
        wake_primitive_init(&g_v2.threads[i].wake);
        pthread_create(&g_v2.threads[i].handle, NULL,
                       thread_v2_main, (void*)(intptr_t)i);
    }

    pthread_create(&g_v2.sysmon_handle, NULL, sched_v2_sysmon_main, NULL);
    g_v2.initialized = 1;
}

void sched_v2_ensure_init(void) {
    pthread_once(&g_v2.init_once, sched_v2_init_impl);
}

void sched_v2_shutdown(void) {
    if (!g_v2.initialized) return;
    atomic_store_explicit(&g_v2.running, 0, memory_order_release);

    int n = atomic_load_explicit(&g_v2.num_threads, memory_order_relaxed);
    for (int i = 0; i < n; i++) {
        wake_primitive_wake_one(&g_v2.threads[i].wake);
    }
    wake_primitive_wake_one(&g_v2.sysmon_wake);

    for (int i = 0; i < n; i++) {
        pthread_join(g_v2.threads[i].handle, NULL);
    }
    pthread_join(g_v2.sysmon_handle, NULL);

    fiber_v2* f = g_v2.all_fibers;
    while (f) {
        fiber_v2* next = f->all_next;
        wake_primitive_destroy(&f->done_wake);
        free(f);
        f = next;
    }
    atomic_store_explicit(&g_v2.free_list, NULL, memory_order_relaxed);
    g_v2.all_fibers = NULL;
    g_v2.initialized = 0;
}

/* ============================================================================
 * Accessors
 * ============================================================================ */

int sched_v2_fiber_done(fiber_v2* f) {
    return f && atomic_load_explicit(&f->done, memory_order_acquire);
}

void* sched_v2_fiber_result(fiber_v2* f) {
    return f ? f->result : NULL;
}

char* sched_v2_fiber_result_buf(fiber_v2* f) {
    return f ? f->result_buf : NULL;
}

void sched_v2_fiber_release(fiber_v2* f) {
    if (f) fiber_v2_free(f);
}

void* sched_v2_current_result_buf(size_t size) {
    fiber_v2* f = tls_v2_current_fiber;
    if (!f || size > sizeof(f->result_buf)) return NULL;
    return f->result_buf;
}

/* ============================================================================
 * Join: block until fiber completes
 * ============================================================================ */

int sched_v2_join(fiber_v2* f, void** out_result) {
    if (!f) return -1;

    if (atomic_load_explicit(&f->done, memory_order_acquire)) {
        return sched_v2_finish_join(f, out_result);
    }

    for (int i = 0; i < 256; i++) {
        if (atomic_load_explicit(&f->done, memory_order_acquire)) {
            return sched_v2_finish_join(f, out_result);
        }
        #if defined(__aarch64__) || defined(__arm64__)
        __asm__ volatile("yield");
        #elif defined(__x86_64__)
        __asm__ volatile("pause");
        #endif
    }

    if (tls_v2_current_fiber) {
        sched_v2_fail_stackless_suspend("join");
    }
    if (cc__fiber_in_context()) {
        atomic_store_explicit(&f->join_waiter_fiber,
                              (cc__fiber*)cc__fiber_current(),
                              memory_order_release);
        while (!atomic_load_explicit(&f->done, memory_order_acquire)) {
            cc__fiber_clear_pending_unpark();
            CC_FIBER_PARK_IF(&f->done, 0, "sched_v2_join");
        }
        atomic_store_explicit(&f->join_waiter_fiber, NULL, memory_order_relaxed);
        return sched_v2_finish_join(f, out_result);
    }
    while (!atomic_load_explicit(&f->done, memory_order_acquire)) {
        uint32_t wait_val = atomic_load_explicit(&f->done_wake.value, memory_order_acquire);
        if (atomic_load_explicit(&f->done, memory_order_acquire)) break;
        wake_primitive_wait(&f->done_wake, wait_val);
    }

    return sched_v2_finish_join(f, out_result);
}

/* ============================================================================
 * Spawn: allocate fiber + enqueue + wake
 * ============================================================================ */

fiber_v2* sched_v2_spawn(void* (*fn)(void*), void* arg) {
    sched_v2_ensure_init();

    fiber_v2* f = fiber_v2_alloc();
    if (!f) return NULL;

    f->entry_fn = fn;
    f->entry_arg = arg;
    atomic_store_explicit(&f->state, FIBER_V2_QUEUED, memory_order_release);

    v2_queue_push(&g_v2.ready_queue, f);
    sched_v2_wake(-1);

    return f;
}

/* ============================================================================
 * Wait-ticket support (for kqueue / multi-wait integration)
 * ============================================================================ */

uint64_t sched_v2_fiber_publish_wait_ticket(fiber_v2* f) {
    if (!f) return 0;
    return atomic_fetch_add_explicit(&f->wait_ticket, 1, memory_order_acq_rel) + 1;
}

int sched_v2_fiber_wait_ticket_matches(fiber_v2* f, uint64_t ticket) {
    if (!f) return 0;
    uint64_t cur = atomic_load_explicit(&f->wait_ticket, memory_order_acquire);
    return cur == ticket;
}
