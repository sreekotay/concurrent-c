/*
 * Unified CCTask runtime (async, future, and spawn tasks).
 */

#include <ccc/std/task.cch>
#include <ccc/cc_sched.cch>

#include <ccc/cc_exec.cch>
#include <ccc/cc_channel.cch>
#include <ccc/cc_nursery.cch>
#include <ccc/cc_atomic.cch>

/* Unified deadlock tracking (defined in fiber_sched.c) */
void cc__deadlock_thread_block(void);
void cc__deadlock_thread_unblock(void);
void cc__fiber_set_worker_affinity(int worker_id);

#include <errno.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Internal task data layouts (stored in CCTask._data) */
typedef struct {
    CCChan* done;
    volatile int cancelled;
    intptr_t result;
    CCClosure0 c;
} CCTaskHeap;

#ifndef CC_TASK_INTERNAL_TYPES_DEFINED
#define CC_TASK_INTERNAL_TYPES_DEFINED
/* Internal representation for FUTURE kind tasks */
typedef struct {
    CCFuture fut;
    void* heap;
} CCTaskFutureInternal;

/* Internal representation for POLL kind tasks */
typedef struct {
    cc_task_poll_fn poll;
    int (*wait)(void* frame);
    void* frame;
    void (*drop)(void* frame);
} CCTaskPollInternal;

/* Internal representation for SPAWN kind tasks */
typedef struct {
    struct CCSpawnTask* spawn;
} CCTaskSpawnInternal;

/* Internal representation for FIBER kind tasks */
typedef struct fiber_task fiber_task;
typedef struct {
    fiber_task* fiber;
} CCTaskFiberInternal;

/* Internal representation for POOL kind tasks */
typedef struct {
    int slot_idx;
} CCTaskPoolInternal;

/* Accessor macros to get internal data from CCTask */
#define TASK_FUTURE(t) ((CCTaskFutureInternal*)((t)->_data))
#define TASK_POLL(t) ((CCTaskPollInternal*)((t)->_data))
#define TASK_SPAWN(t) ((CCTaskSpawnInternal*)((t)->_data))
#define TASK_FIBER(t) ((CCTaskFiberInternal*)((t)->_data))
#define TASK_POOL(t)  ((CCTaskPoolInternal*)((t)->_data))
#endif /* CC_TASK_INTERNAL_TYPES_DEFINED */

/* Fiber functions (defined in fiber_sched.c) */
fiber_task* cc_fiber_spawn(void* (*fn)(void*), void* arg);
int cc_fiber_join(fiber_task* f, void** out_result);
void cc_fiber_task_free(fiber_task* f);
int cc_fiber_poll_done(fiber_task* f);
void* cc_fiber_get_result(fiber_task* f);
void* cc_fiber_get_result_buf(fiber_task* f);

/* Spawn task poll functions (defined in scheduler.c) */
int cc_thread_task_poll_done(struct CCSpawnTask* task);
void* cc_thread_task_get_result(struct CCSpawnTask* task);
int cc_thread_task_join_fiber(struct CCSpawnTask* task, void** out_result);

/* Fiber context detection (defined in fiber_sched.c) */
int cc__fiber_in_context(void);
void* cc__fiber_current(void);

extern void cc__fiber_set_pool_slot_buf(char* buf);

/* Fiber park/unpark for pool task wait (defined in fiber_sched.c) */
void cc__fiber_park_if(_Atomic int* flag, int expected,
                       const char* reason, const char* file, int line);
void cc__fiber_unpark(void* fiber_ptr);
/* Cooperative yield to global queue (defined in fiber_sched.c) */
void cc__fiber_yield_global(void);
/* Sysmon heartbeat touch: prevents orphan-threshold detection during
 * long-running pool tasks (defined in fiber_sched.c) */
void cc__fiber_touch_heartbeat(void);
void cc__fiber_pool_task_begin(void);
void cc__fiber_pool_task_end(void);

static CCExec* g_task_exec = NULL;
static pthread_mutex_t g_task_exec_mu = PTHREAD_MUTEX_INITIALIZER;
static cc_atomic_u64 g_task_submit_failures = 0;

/* Optional join/wait instrumentation for debugging throughput gaps.
 * Enable with CC_TASK_WAIT_STATS=1 and dump at exit with CC_TASK_WAIT_STATS_DUMP=1. */
void cc__fiber_unpark_stats(uint64_t* out_calls, uint64_t* out_enqueues);
void cc__fiber_join_park_stats(uint64_t* out_joins, uint64_t* out_loops);
void cc__fiber_join_help_stats(uint64_t* out_attempts, uint64_t* out_hits);

typedef struct {
    _Atomic int mode;              /* -1 unknown, 0 off, 1 on */
    _Atomic int dump_mode;         /* -1 unknown, 0 off, 1 on */
    _Atomic int atexit_registered; /* 0/1 */
    _Atomic uint64_t block_calls_total;
    _Atomic uint64_t block_spawn_calls;
    _Atomic uint64_t block_fiber_calls;
    _Atomic uint64_t block_spawn_wait_ns;
    _Atomic uint64_t block_fiber_wait_ns;
    _Atomic uint64_t fiber_join_calls;
    _Atomic uint64_t fiber_join_wait_ns;
    _Atomic uint64_t fiber_result_tls_copies;
} cc_task_wait_stats;

static cc_task_wait_stats g_cc_task_wait_stats = {
    .mode = -1,
    .dump_mode = -1,
    .atexit_registered = 0
};

static inline uint64_t cc__mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cc_task_wait_stats_enabled(void) {
    int mode = atomic_load_explicit(&g_cc_task_wait_stats.mode, memory_order_acquire);
    if (mode >= 0) return mode;
    mode = (getenv("CC_TASK_WAIT_STATS") || getenv("CC_TASK_WAIT_STATS_DUMP")) ? 1 : 0;
    int expected = -1;
    (void)atomic_compare_exchange_strong_explicit(&g_cc_task_wait_stats.mode,
                                                   &expected,
                                                   mode,
                                                   memory_order_release,
                                                   memory_order_acquire);
    return atomic_load_explicit(&g_cc_task_wait_stats.mode, memory_order_acquire);
}

static int cc_task_wait_stats_dump_enabled(void) {
    int mode = atomic_load_explicit(&g_cc_task_wait_stats.dump_mode, memory_order_acquire);
    if (mode >= 0) return mode;
    mode = getenv("CC_TASK_WAIT_STATS_DUMP") ? 1 : 0;
    int expected = -1;
    (void)atomic_compare_exchange_strong_explicit(&g_cc_task_wait_stats.dump_mode,
                                                   &expected,
                                                   mode,
                                                   memory_order_release,
                                                   memory_order_acquire);
    return atomic_load_explicit(&g_cc_task_wait_stats.dump_mode, memory_order_acquire);
}

static void cc_task_wait_stats_dump(void) {
    if (!cc_task_wait_stats_enabled()) return;
    uint64_t total = atomic_load_explicit(&g_cc_task_wait_stats.block_calls_total, memory_order_relaxed);
    uint64_t spawn_calls = atomic_load_explicit(&g_cc_task_wait_stats.block_spawn_calls, memory_order_relaxed);
    uint64_t fiber_calls = atomic_load_explicit(&g_cc_task_wait_stats.block_fiber_calls, memory_order_relaxed);
    uint64_t spawn_wait_ns = atomic_load_explicit(&g_cc_task_wait_stats.block_spawn_wait_ns, memory_order_relaxed);
    uint64_t fiber_wait_ns = atomic_load_explicit(&g_cc_task_wait_stats.block_fiber_wait_ns, memory_order_relaxed);
    uint64_t fiber_join_calls = atomic_load_explicit(&g_cc_task_wait_stats.fiber_join_calls, memory_order_relaxed);
    uint64_t fiber_join_wait_ns = atomic_load_explicit(&g_cc_task_wait_stats.fiber_join_wait_ns, memory_order_relaxed);
    uint64_t tls_copies = atomic_load_explicit(&g_cc_task_wait_stats.fiber_result_tls_copies, memory_order_relaxed);
    uint64_t unpark_calls = 0;
    uint64_t unpark_enqueues = 0;
    uint64_t join_park_joins = 0;
    uint64_t join_park_loops = 0;
    uint64_t help_attempts = 0;
    uint64_t help_hits = 0;
    cc__fiber_unpark_stats(&unpark_calls, &unpark_enqueues);
    cc__fiber_join_park_stats(&join_park_joins, &join_park_loops);
    cc__fiber_join_help_stats(&help_attempts, &help_hits);

    fprintf(stderr, "\n=== CC_TASK_WAIT_STATS ===\n");
    fprintf(stderr, "block_on calls: total=%llu spawn=%llu fiber=%llu\n",
            (unsigned long long)total,
            (unsigned long long)spawn_calls,
            (unsigned long long)fiber_calls);
    if (spawn_calls) {
        fprintf(stderr, "spawn join wait: total=%.3f ms avg=%.3f us\n",
                (double)spawn_wait_ns / 1000000.0,
                (double)spawn_wait_ns / (double)spawn_calls / 1000.0);
    }
    if (fiber_calls) {
        fprintf(stderr, "fiber block wait: total=%.3f ms avg=%.3f us\n",
                (double)fiber_wait_ns / 1000000.0,
                (double)fiber_wait_ns / (double)fiber_calls / 1000.0);
    }
    if (fiber_join_calls) {
        fprintf(stderr, "fiber join wait: total=%.3f ms avg=%.3f us\n",
                (double)fiber_join_wait_ns / 1000000.0,
                (double)fiber_join_wait_ns / (double)fiber_join_calls / 1000.0);
        fprintf(stderr, "fiber wake path: unpark_calls=%llu enqueues=%llu per_join=(%.3f/%.3f)\n",
                (unsigned long long)unpark_calls,
                (unsigned long long)unpark_enqueues,
                (double)unpark_calls / (double)fiber_join_calls,
                (double)unpark_enqueues / (double)fiber_join_calls);
        if (join_park_joins) {
            fprintf(stderr, "fiber join park loops: joins=%llu loops=%llu avg_loops=%.3f\n",
                    (unsigned long long)join_park_joins,
                    (unsigned long long)join_park_loops,
                    (double)join_park_loops / (double)join_park_joins);
        }
        if (help_attempts) {
            fprintf(stderr, "fiber help-first join: attempts=%llu hits=%llu hit_rate=%.1f%%\n",
                    (unsigned long long)help_attempts,
                    (unsigned long long)help_hits,
                    100.0 * (double)help_hits / (double)help_attempts);
        }
    }
    fprintf(stderr, "fiber result TLS copies: %llu\n",
            (unsigned long long)tls_copies);
    fprintf(stderr, "==========================\n");
}

static inline void cc_task_wait_stats_maybe_init(void) {
    if (!cc_task_wait_stats_enabled() || !cc_task_wait_stats_dump_enabled()) return;
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_cc_task_wait_stats.atexit_registered,
                                                &expected,
                                                1,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atexit(cc_task_wait_stats_dump);
    }
}

/* cc__env_size defined in scheduler.c */

/* ================================================================
 * Runtime-level fiber task pool
 *
 * cc_fiber_spawn_task routes work to persistent pool-runner fibers
 * instead of spawning a new fiber per task.  Pool runners loop on
 * a shared work channel so the next task is picked up inline
 * (channel buffer hit) with no spawn/route/wake overhead.
 *
 * The work channel sends CCPoolSlot* pointers (8 bytes) — this fits
 * the lock-free fast-path in the channel implementation (≤sizeof(void*)).
 *
 * Disabled with CC_FIBER_POOL=0.  Default: enabled.
 * ================================================================ */

#define CC_POOL_MAX_SLOTS 128  /* max concurrent pool tasks */
#define CC_POOL_WORK_CAP  128  /* work channel capacity (pointer-sized items) */

/* Per-task slot: stores fn/arg for the runner, result for the waiter */
typedef struct {
    void* (*fn)(void*);       /* task function (set at alloc time) */
    void* arg;                /* task argument */
    volatile intptr_t result; /* result set by pool runner */
    char result_buf_copy[48]; /* copy of pool-runner result_buf (if used) */
    _Atomic int in_use;       /* 0=free, 1=allocated */
    _Atomic int done;         /* 0=pending, 1=complete */
    void* _Atomic waiter;     /* fiber_task* of the blocking fiber */
} CCPoolSlot;

typedef struct {
    CCChan* work_ch;                     /* channel of CCPoolSlot* pointers */
    CCPoolSlot slots[CC_POOL_MAX_SLOTS]; /* result slots */
    _Atomic int next_slot;               /* round-robin allocation hint */
    _Atomic int ready;                   /* 1 = initialized */
    _Atomic int shutting_down;           /* 1 = closing */
} CCFiberPool;

static CCFiberPool g_fpool;
static pthread_once_t g_fpool_once = PTHREAD_ONCE_INIT;

static int cc_pool_enabled(void) {
    static _Atomic int enabled = -1;
    int v = atomic_load_explicit(&enabled, memory_order_relaxed);
    if (v >= 0) return v;
    const char* ev = getenv("CC_FIBER_POOL");
    int nv = (!ev || ev[0] != '0') ? 1 : 0; /* default ON */
    int expected = -1;
    atomic_compare_exchange_strong_explicit(&enabled, &expected, nv,
                                            memory_order_release, memory_order_relaxed);
    return atomic_load_explicit(&enabled, memory_order_relaxed);
}

static int cc_pool_alloc_slot(void) {
    int start = atomic_fetch_add_explicit(&g_fpool.next_slot, 1, memory_order_relaxed)
                % CC_POOL_MAX_SLOTS;
    for (int i = 0; i < CC_POOL_MAX_SLOTS; i++) {
        int idx = (start + i) % CC_POOL_MAX_SLOTS;
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(&g_fpool.slots[idx].in_use, &expected, 1,
                                                    memory_order_acq_rel,
                                                    memory_order_relaxed)) {
            atomic_store_explicit(&g_fpool.slots[idx].done, 0, memory_order_relaxed);
            atomic_store_explicit(&g_fpool.slots[idx].waiter, NULL, memory_order_relaxed);
            return idx;
        }
    }
    return -1; /* all slots in use */
}

static void cc_pool_free_slot(int idx) {
    atomic_store_explicit(&g_fpool.slots[idx].in_use, 0, memory_order_release);
}

/* Pool runner fiber: loops receiving CCPoolSlot* from work_ch, executes fn(arg),
 * publishes result to the slot, wakes the waiter, then yields so the consumer
 * gets a scheduling turn before the runner grabs the next task.
 *
 * Before calling fn, cc__fiber_set_pool_slot_buf directs cc_task_result_ptr
 * into the slot's stable result_buf_copy.  Both accessors are noinline to
 * prevent the compiler from caching TLS descriptor addresses across the
 * cc_chan_recv yield point (fiber migration invalidates cached TLS slots).
 *
 * After fn returns, we detect whether the result landed in the expected slot
 * buffer, the runner fiber's own result_buf (stale-NULL TLS fallback), or a
 * different slot (stale-nonzero TLS), and copy to the correct slot if needed.
 * A plain scalar (non-buffer pointer) passes through as-is. */
static void* cc_pool_runner_fn(void* arg) {
    /* Capture this runner fiber's result_buf address once at startup (before
     * any yield).  If the TLS-based cc_task_result_ptr fallback writes into
     * the fiber's buffer instead of the slot's, we detect and copy below. */
    fiber_task* self = (fiber_task*)cc__fiber_current();
    char* my_fiber_buf = self ? (char*)cc_fiber_get_result_buf(self) : NULL;

    /* Pre-assign this runner to a distinct worker before first park so the
     * corrected saturation bypass routes it to that worker's inbox directly. */
    cc__fiber_set_worker_affinity((int)(intptr_t)arg);

    CCPoolSlot* slot;
    while (cc_chan_recv(g_fpool.work_ch, &slot, sizeof(slot)) == 0) {
        /* Compiler barrier: cc_chan_recv may park/resume this fiber on a
         * different OS thread.  Force the compiler to re-resolve all TLS
         * addresses (tls_current_fiber, cc_tls_pool_slot_result_buf, etc.)
         * rather than reusing stale descriptor addresses from the previous
         * iteration's thread. */
        __asm__ volatile("" ::: "memory");
        if (atomic_load_explicit(&g_fpool.shutting_down, memory_order_acquire)) break;

        cc__fiber_set_pool_slot_buf(slot->result_buf_copy);
        void* result = slot->fn(slot->arg);
        char* expected_buf = slot->result_buf_copy;
        cc__fiber_set_pool_slot_buf(NULL);

        if (result == (void*)expected_buf) {
            /* TLS worked: result already in the slot's stable buffer. */
            slot->result = (intptr_t)expected_buf;
        } else if (my_fiber_buf && result == (void*)my_fiber_buf) {
            /* TLS was stale (NULL) after fiber migration — task wrote into
             * the runner fiber's result_buf.  Copy to the slot's buffer. */
            memcpy(slot->result_buf_copy, my_fiber_buf, 48);
            slot->result = (intptr_t)slot->result_buf_copy;
        } else if ((char*)result >= (char*)&g_fpool.slots[0] &&
                   (char*)result < (char*)&g_fpool.slots[CC_POOL_MAX_SLOTS]) {
            /* TLS pointed to a different slot's result_buf_copy (stale
             * from a previous iteration on the old thread).  Copy. */
            memcpy(slot->result_buf_copy, result, 48);
            slot->result = (intptr_t)slot->result_buf_copy;
        } else {
            /* Raw scalar return (not a result-buffer pointer). */
            slot->result = (intptr_t)result;
        }

        atomic_store_explicit(&slot->done, 1, memory_order_release);
        void* waiter = atomic_exchange_explicit(&slot->waiter, NULL, memory_order_acq_rel);
        if (waiter) {
            cc__fiber_unpark(waiter);
        }
    }
    return NULL;
}

static void cc_fpool_atexit(void) {
    atomic_store_explicit(&g_fpool.shutting_down, 1, memory_order_release);
    if (g_fpool.work_ch) {
        cc_chan_close(g_fpool.work_ch);
    }
}

static void cc_fpool_init_impl(void) {
    if (!cc_pool_enabled()) return;

    size_t num_runners = cc_sched_get_num_workers();
    if (num_runners == 0) num_runners = 8;

    /* Work channel sends pointer-sized items — fits the lock-free fast-path */
    g_fpool.work_ch = cc_chan_create(CC_POOL_WORK_CAP);
    if (!g_fpool.work_ch) return;

    /* Spawn pool runner fibers, passing index so each pre-assigns itself
     * to a distinct worker before its first cc_chan_recv park. */
    for (size_t i = 0; i < num_runners; i++) {
        fiber_task* f = cc_fiber_spawn(cc_pool_runner_fn, (void*)(intptr_t)i);
        (void)f;
    }

    atexit(cc_fpool_atexit);
    atomic_store_explicit(&g_fpool.ready, 1, memory_order_release);
}

static void cc_fpool_ensure_init(void) {
    if (atomic_load_explicit(&g_fpool.ready, memory_order_acquire)) return;
    pthread_once(&g_fpool_once, cc_fpool_init_impl);
}

static size_t cc__default_blocking_workers(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0 && n < 4) return (size_t)n;
    return 4;
}

static CCExec* cc__task_exec_lazy(void) {
    pthread_mutex_lock(&g_task_exec_mu);
    if (!g_task_exec) {
        size_t workers = cc__env_size("CC_BLOCKING_WORKERS", cc__default_blocking_workers());
        size_t qcap = cc__env_size("CC_BLOCKING_QUEUE_CAP", 256);
        g_task_exec = cc_exec_create(workers, qcap);
    }
    CCExec* ex = g_task_exec;
    pthread_mutex_unlock(&g_task_exec_mu);
    return ex;
}

static void cc__task_job(void* arg) {
    CCTaskHeap* h = (CCTaskHeap*)arg;
    if (!h) return;

    int err = 0;
    if (h->cancelled) {
        err = ECANCELED;
    } else if (h->c.fn) {
        void* r = h->c.fn(h->c.env);
        h->result = (intptr_t)r;
        if (h->c.drop) h->c.drop(h->c.env);
    } else {
        err = EINVAL;
    }

    if (h->done) {
        (void)cc_chan_send(h->done, &err, sizeof(err));
    }
}

CCTask cc_run_blocking_task(CCClosure0 c) {
    CCTask out;
    memset(&out, 0, sizeof(out));
    if (!c.fn) return out;

    CCExec* ex = cc__task_exec_lazy();
    if (!ex) return out;

    CCTaskHeap* h = (CCTaskHeap*)calloc(1, sizeof(CCTaskHeap));
    if (!h) return out;
    h->done = cc_chan_create(1);
    if (!h->done) {
        free(h);
        return out;
    }
    h->cancelled = 0;
    h->result = 0;
    h->c = c;

    out.kind = CC_TASK_KIND_FUTURE;
    CCTaskFutureInternal* fut = TASK_FUTURE(&out);
    cc_future_init(&fut->fut);
    fut->fut.handle.done = h->done;
    fut->fut.handle.cancelled = 0;
    fut->fut.result = &h->result;
    fut->heap = h;

    int sub = cc_exec_submit(ex, cc__task_job, h);
    if (sub != 0) {
        cc_atomic_fetch_add(&g_task_submit_failures, 1);
        if (h->done) {
            cc_chan_close(h->done);
            cc_chan_free(h->done);
            h->done = NULL;
        }
        free(h);
        memset(&out, 0, sizeof(out));
        return out;
    }
    return out;
}

int cc_blocking_pool_stats(CCExecStats* out_exec, uint64_t* out_submit_failures) {
    if (out_submit_failures) {
        *out_submit_failures = (uint64_t)cc_atomic_load(&g_task_submit_failures);
    }
    CCExec* ex = cc__task_exec_lazy();
    if (!out_exec) return ex ? 0 : ENOMEM;
    if (!ex) {
        memset(out_exec, 0, sizeof(*out_exec));
        return 0;
    }
    return cc_exec_stats(ex, out_exec);
}

CCFutureStatus cc_task_poll(CCTask* t, intptr_t* out_val, int* out_err) {
    if (!t) return CC_FUTURE_ERR;
    if (t->kind == CC_TASK_KIND_FUTURE) {
        CCTaskFutureInternal* fut = TASK_FUTURE(t);
        CCFutureStatus st = cc_future_poll(&fut->fut, out_err);
        if (st == CC_FUTURE_READY && out_val && fut->fut.result) {
            *out_val = *(const intptr_t*)fut->fut.result;
        }
        return st;
    }
    if (t->kind == CC_TASK_KIND_POLL) {
        CCTaskPollInternal* p = TASK_POLL(t);
        if (!p->poll) return CC_FUTURE_ERR;
        return p->poll(p->frame, out_val, out_err);
    }
    if (t->kind == CC_TASK_KIND_SPAWN) {
        CCTaskSpawnInternal* s = TASK_SPAWN(t);
        if (!s->spawn) return CC_FUTURE_ERR;
        if (cc_thread_task_poll_done(s->spawn)) {
            if (out_val) *out_val = (intptr_t)cc_thread_task_get_result(s->spawn);
            if (out_err) *out_err = 0;
            return CC_FUTURE_READY;
        }
        return CC_FUTURE_PENDING;
    }
    if (t->kind == CC_TASK_KIND_FIBER) {
        CCTaskFiberInternal* fi = TASK_FIBER(t);
        if (!fi->fiber) return CC_FUTURE_ERR;
        if (cc_fiber_poll_done(fi->fiber)) {
            if (out_val) *out_val = (intptr_t)cc_fiber_get_result(fi->fiber);
            if (out_err) *out_err = 0;
            return CC_FUTURE_READY;
        }
        return CC_FUTURE_PENDING;
    }
    if (t->kind == CC_TASK_KIND_POOL) {
        CCTaskPoolInternal* pi = TASK_POOL(t);
        if (atomic_load_explicit(&g_fpool.slots[pi->slot_idx].done, memory_order_acquire)) {
            if (out_val) *out_val = g_fpool.slots[pi->slot_idx].result;
            if (out_err) *out_err = 0;
            return CC_FUTURE_READY;
        }
        return CC_FUTURE_PENDING;
    }
    return CC_FUTURE_ERR;
}

CCTask cc_task_make_poll(cc_task_poll_fn poll, void* frame, void (*drop)(void*)) {
    CCTask t;
    memset(&t, 0, sizeof(t));
    if (!poll || !frame) return t;
    t.kind = CC_TASK_KIND_POLL;
    CCTaskPollInternal* p = TASK_POLL(&t);
    p->poll = poll;
    p->wait = NULL;
    p->frame = frame;
    p->drop = drop;
    return t;
}

CCTask cc_task_make_poll_ex(cc_task_poll_fn poll, int (*wait)(void*), void* frame, void (*drop)(void*)) {
    CCTask t;
    memset(&t, 0, sizeof(t));
    if (!poll || !frame) return t;
    t.kind = CC_TASK_KIND_POLL;
    CCTaskPollInternal* p = TASK_POLL(&t);
    p->poll = poll;
    p->wait = wait;
    p->frame = frame;
    p->drop = drop;
    return t;
}

void cc_task_free(CCTask* t) {
    if (!t) return;
    if (t->kind == CC_TASK_KIND_FUTURE) {
        CCTaskFutureInternal* fut = TASK_FUTURE(t);
        CCTaskHeap* h = (CCTaskHeap*)fut->heap;
        if (fut->fut.handle.done) {
            cc_future_free(&fut->fut);
        }
        if (h) {
            h->cancelled = 1;
            free(h);
        }
    } else if (t->kind == CC_TASK_KIND_POLL) {
        CCTaskPollInternal* p = TASK_POLL(t);
        if (p->drop && p->frame) p->drop(p->frame);
        p->poll = NULL;
        p->frame = NULL;
        p->drop = NULL;
    } else if (t->kind == CC_TASK_KIND_SPAWN) {
        CCTaskSpawnInternal* s = TASK_SPAWN(t);
        if (s->spawn) {
            cc_thread_task_free(s->spawn);
        }
    } else if (t->kind == CC_TASK_KIND_FIBER) {
        CCTaskFiberInternal* fi = TASK_FIBER(t);
        if (fi->fiber) {
            cc_fiber_task_free(fi->fiber);
        }
    } else if (t->kind == CC_TASK_KIND_POOL) {
        /* Pool slot is freed in cc_block_on_intptr after collecting result.
         * If cc_task_free is called on an un-joined pool task (leak path),
         * release the slot so it can be reused. */
        CCTaskPoolInternal* pi = TASK_POOL(t);
        if (atomic_load_explicit(&g_fpool.slots[pi->slot_idx].in_use,
                                  memory_order_acquire)) {
            cc_pool_free_slot(pi->slot_idx);
        }
    }
    memset(t, 0, sizeof(*t));
}

/* Helper to set fiber in task internal data */
static void cc__set_fiber_task(CCTask* t, fiber_task* f) {
    CCTaskFiberInternal* fi = TASK_FIBER(t);
    fi->fiber = f;
}

/* Spawn an M:N fiber task. Returns CCTask with kind=CC_TASK_KIND_FIBER (or POOL).
 *
 * When the fiber pool is ready (CC_FIBER_POOL != 0) and a slot is free, the
 * task is dispatched through the pool: a persistent pool-runner fiber picks it
 * up from the work channel, eliminating the per-task spawn/route/wake cycle.
 * If the pool is full or unavailable, falls back to a normal fiber spawn. */
CCTask cc_fiber_spawn_task(void* (*fn)(void*), void* arg) {
    CCTask out;
    memset(&out, 0, sizeof(out));
    if (!fn) return out;

    /* Attempt pool dispatch */
    cc_fpool_ensure_init();
    if (atomic_load_explicit(&g_fpool.ready, memory_order_acquire)) {
        int slot_idx = cc_pool_alloc_slot();
        if (slot_idx >= 0) {
            CCPoolSlot* slot = &g_fpool.slots[slot_idx];
            slot->fn  = fn;
            slot->arg = arg;
            /* Send pointer (8 bytes) — fits lock-free channel fast-path */
            if (cc_chan_send(g_fpool.work_ch, &slot, sizeof(slot)) == 0) {
                out.kind = CC_TASK_KIND_POOL;
                TASK_POOL(&out)->slot_idx = slot_idx;
                return out;
            }
            /* work_ch closed or error — fall through to direct spawn */
            cc_pool_free_slot(slot_idx);
        }
    }

    /* Fallback: direct fiber spawn */
    fiber_task* f = cc_fiber_spawn(fn, arg);
    if (!f) return out;

    out.kind = CC_TASK_KIND_FIBER;
    cc__set_fiber_task(&out, f);
    return out;
}

/* Helper function that unpacks and calls a closure for fibers */
static void* cc__fiber_closure0_wrapper(void* arg) {
    CCClosure0* pc = (CCClosure0*)arg;
    void* result = pc->fn(pc->env);
    if (pc->drop) pc->drop(pc->env);
    free(pc);
    return result;
}

/* Spawn a fiber from a 0-arg closure. */
CCTask cc_fiber_spawn_closure0(CCClosure0 c) {
    CCTask out;
    memset(&out, 0, sizeof(out));
    if (!c.fn) return out;
    
    /* Create a heap copy of the closure */
    CCClosure0* heap_c = (CCClosure0*)malloc(sizeof(CCClosure0));
    if (!heap_c) return out;
    *heap_c = c;
    
    return cc_fiber_spawn_task(cc__fiber_closure0_wrapper, heap_c);
}

/* Cancel a task and wake up anyone blocked on it.
   This closes the done channel, causing cc_block_on_intptr to return immediately. */
void cc_task_cancel(CCTask* t) {
    if (!t) return;
    if (t->kind == CC_TASK_KIND_FUTURE) {
        CCTaskFutureInternal* fut = TASK_FUTURE(t);
        CCTaskHeap* h = (CCTaskHeap*)fut->heap;
        if (h) {
            h->cancelled = 1;
            /* Close the done channel to wake up blocked waiters */
            if (h->done) {
                cc_chan_close(h->done);
            }
        }
        /* Also mark the future handle as cancelled */
        fut->fut.handle.cancelled = 1;
    } else if (t->kind == CC_TASK_KIND_POLL) {
        /* For poll-based tasks, we can't easily cancel - just mark frame for cleanup */
        /* The poll function should check cancellation state */
    } else if (t->kind == CC_TASK_KIND_SPAWN) {
        /* Spawn tasks can't be cancelled mid-flight - pthread doesn't support that safely */
    }
}

intptr_t cc_block_on_intptr(CCTask t) {
    intptr_t r = 0;
    int err = 0;
    const int wait_stats = cc_task_wait_stats_enabled();
    if (wait_stats) {
        cc_task_wait_stats_maybe_init();
        atomic_fetch_add_explicit(&g_cc_task_wait_stats.block_calls_total, 1, memory_order_relaxed);
    }
    cc__deadlock_thread_block();  /* Track that this thread is blocking */
    
    /* Handle spawn tasks directly with join */
    if (t.kind == CC_TASK_KIND_SPAWN) {
        CCTaskSpawnInternal* s = TASK_SPAWN(&t);
        uint64_t wait_start_ns = 0;
        if (wait_stats) {
            atomic_fetch_add_explicit(&g_cc_task_wait_stats.block_spawn_calls, 1, memory_order_relaxed);
            wait_start_ns = cc__mono_ns();
        }
        if (s->spawn) {
            void* result = NULL;
            /* Use fiber-aware join when called from fiber context: park the
             * fiber instead of blocking the OS worker thread via condvar.
             * This keeps the worker free to run other fibers during the wait
             * and avoids a kernel condvar round-trip on every completion. */
            if (cc__fiber_in_context()) {
                cc_thread_task_join_fiber(s->spawn, &result);
            } else {
                cc_thread_task_join_result(s->spawn, &result);
            }
            r = (intptr_t)result;
            cc_thread_task_free(s->spawn);
        }
        if (wait_stats) {
            uint64_t wait_ns = cc__mono_ns() - wait_start_ns;
            atomic_fetch_add_explicit(&g_cc_task_wait_stats.block_spawn_wait_ns, wait_ns, memory_order_relaxed);
        }
        cc__deadlock_thread_unblock();
        return r;
    }
    
    /* Handle pool tasks: wait for a pool runner to complete the task.
     *
     * The pool runner copies any result_buf data into slot->result_buf_copy
     * before signalling done, so slot->result is always stable by the time
     * we read it.  We copy to a TLS buffer if result points into result_buf_copy
     * so the slot can be freed immediately without dangling pointers. */
    if (t.kind == CC_TASK_KIND_POOL) {
        static __thread char tls_pool_result[48] __attribute__((aligned(8)));
        CCTaskPoolInternal* pi = TASK_POOL(&t);
        CCPoolSlot* slot = &g_fpool.slots[pi->slot_idx];

        /* Register ourselves as the waiter before checking done, so we don't
         * miss a completion that races with our park. */
        void* cur_fiber = cc__fiber_current();
        if (cur_fiber) {
            void* expected_null = NULL;
            atomic_compare_exchange_strong_explicit(&slot->waiter, &expected_null, cur_fiber,
                                                    memory_order_acq_rel, memory_order_relaxed);
            /* Loop: cc__fiber_park_if may return early if pending_unpark was set
             * by a prior operation (e.g., a channel-recv wake that delivered this
             * task handle).  Re-park until slot->done is actually 1 — only then
             * is slot->result stable.  The acquire load in the loop condition
             * pairs with the release store in cc_pool_runner_fn. */
            while (!atomic_load_explicit(&slot->done, memory_order_acquire)) {
                cc__fiber_park_if(&slot->done, 0, "pool_task", __FILE__, __LINE__);
            }
        } else {
            /* Non-fiber context fallback: spin */
            while (!atomic_load_explicit(&slot->done, memory_order_acquire)) {
                sched_yield();
            }
        }

        /* Discriminate structured vs raw results (mirrors cc_pool_runner_fn):
         * - slot->result == (intptr_t)slot->result_buf_copy: task used
         *   cc_task_result_ptr; copy the 48-byte buffer to TLS so the slot
         *   can be freed and the caller gets a stable pointer.
         * - otherwise: task returned a plain scalar; pass it through directly. */
        if (slot->result == (intptr_t)slot->result_buf_copy) {
            memcpy(tls_pool_result, slot->result_buf_copy, 48);
            r = (intptr_t)tls_pool_result;
        } else {
            r = slot->result;
        }

        cc_pool_free_slot(pi->slot_idx);
        cc__deadlock_thread_unblock();
        return r;
    }

    /* Handle fiber tasks with fiber join.
     *
     * If the thunk used cc_task_result_ptr, its return value points into
     * the fiber's result_buf.  fiber_task_free pools the struct for
     * immediate reuse, so that pointer becomes dangling.  We detect this
     * case (result points into result_buf) and memcpy into a thread-local
     * buffer BEFORE freeing.  If the thunk returned a plain integer cast
     * to void*, we pass it through as-is. */
    if (t.kind == CC_TASK_KIND_FIBER) {
        static __thread char tls_fiber_result[48] __attribute__((aligned(8)));
        CCTaskFiberInternal* fi = TASK_FIBER(&t);
        uint64_t fiber_wait_start_ns = 0;
        if (wait_stats) {
            atomic_fetch_add_explicit(&g_cc_task_wait_stats.block_fiber_calls, 1, memory_order_relaxed);
            fiber_wait_start_ns = cc__mono_ns();
        }
        if (fi->fiber) {
            void* result = NULL;
            uint64_t join_start_ns = 0;
            if (wait_stats) {
                atomic_fetch_add_explicit(&g_cc_task_wait_stats.fiber_join_calls, 1, memory_order_relaxed);
                join_start_ns = cc__mono_ns();
            }
            cc_fiber_join(fi->fiber, &result);
            if (wait_stats) {
                uint64_t join_wait_ns = cc__mono_ns() - join_start_ns;
                atomic_fetch_add_explicit(&g_cc_task_wait_stats.fiber_join_wait_ns, join_wait_ns, memory_order_relaxed);
            }
            if (result) {
                /* Check if result points into the fiber's result_buf */
                char* buf = (char*)cc_fiber_get_result_buf(fi->fiber);
                if (buf && (char*)result >= buf && (char*)result < buf + 48) {
                    memcpy(tls_fiber_result, result, sizeof(tls_fiber_result));
                    if (wait_stats) {
                        atomic_fetch_add_explicit(&g_cc_task_wait_stats.fiber_result_tls_copies, 1, memory_order_relaxed);
                    }
                    r = (intptr_t)tls_fiber_result;
                } else {
                    r = (intptr_t)result;
                }
            }
            cc_fiber_task_free(fi->fiber);
        }
        if (wait_stats) {
            uint64_t fiber_wait_ns = cc__mono_ns() - fiber_wait_start_ns;
            atomic_fetch_add_explicit(&g_cc_task_wait_stats.block_fiber_wait_ns, fiber_wait_ns, memory_order_relaxed);
        }
        cc__deadlock_thread_unblock();
        return r;
    }
    
    for (;;) {
        CCFutureStatus st = cc_task_poll(&t, &r, &err);
        if (st == CC_FUTURE_PENDING) {
            if (t.kind == CC_TASK_KIND_FUTURE) {
                /* For "future" tasks, block directly on the done channel once and then return the result.
                   This avoids spin-polling and avoids needing to preserve the completion for poll(). */
                CCTaskFutureInternal* fut = TASK_FUTURE(&t);
                err = cc_async_wait(&fut->fut.handle);
                if (err == 0 && fut->fut.result) r = *(const intptr_t*)fut->fut.result;
                break;
            } else if (t.kind == CC_TASK_KIND_POLL) {
                CCTaskPollInternal* p = TASK_POLL(&t);
                if (p->wait) {
                    /* Task has a wait function - use it to block efficiently */
                    (void)p->wait(p->frame);
                }
            }
            /* For POLL tasks without wait: tight loop. These are pure state machines
               making progress on every poll (no external blocking). No yield needed. */
            continue;
        }
        break;
    }
    cc__deadlock_thread_unblock();
    cc_task_free(&t);
    (void)err;
    return r;
}


/* --- cc_block_all implementation --- */

typedef struct {
    CCTask task;            /* Copy of the task (we own it) */
    intptr_t* result_slot;  /* Where to store the result */
} CCBlockAllSlot;

static void* cc__block_all_worker(void* arg) {
    CCBlockAllSlot* slot = (CCBlockAllSlot*)arg;
    if (!slot) return NULL;
    intptr_t r = cc_block_on_intptr(slot->task);
    if (slot->result_slot) *slot->result_slot = r;
    return NULL;
}

/* Block until all tasks complete. Runs tasks concurrently using a nursery.
   Returns 0 on success, error code if any task fails.
   Results are stored in the results array (must be at least count elements).
   Note: Takes ownership of the tasks (they are freed after completion). */
int cc_block_all(int count, CCTask* tasks, intptr_t* results) {
    if (count <= 0) return 0;
    if (!tasks) return EINVAL;

    CCNursery* n = cc_nursery_create();
    if (!n) return ENOMEM;

    CCBlockAllSlot* slots = (CCBlockAllSlot*)calloc((size_t)count, sizeof(CCBlockAllSlot));
    if (!slots) {
        cc_nursery_free(n);
        return ENOMEM;
    }

    for (int i = 0; i < count; i++) {
        slots[i].task = tasks[i];  /* Copy task */
        slots[i].result_slot = results ? &results[i] : NULL;
    }

    /* Spawn a thread for each task */
    for (int i = 0; i < count; i++) {
        int err = cc_nursery_spawn(n, cc__block_all_worker, &slots[i]);
        if (err != 0) {
            cc_nursery_cancel(n);
            cc_nursery_wait(n);
            cc_nursery_free(n);
            free(slots);
            return err;
        }
    }

    int err = cc_nursery_wait(n);
    cc_nursery_free(n);
    free(slots);
    return err;
}

/* --- cc_block_race implementation --- */

typedef struct {
    CCTask task;
    int index;
    CCChan* done_chan;  /* Shared channel to signal completion */
    intptr_t result;
    int error;
    volatile int* winner_flag;  /* Set to 1 when first completes */
} CCBlockRaceSlot;

typedef struct {
    int index;
    intptr_t result;
    int error;
} CCBlockRaceResult;

static void* cc__block_race_worker(void* arg) {
    CCBlockRaceSlot* slot = (CCBlockRaceSlot*)arg;
    if (!slot) return NULL;
    
    slot->result = cc_block_on_intptr(slot->task);
    slot->error = 0;  /* TODO: capture actual errors */
    
    /* Signal completion */
    CCBlockRaceResult msg = { slot->index, slot->result, slot->error };
    cc_chan_send(slot->done_chan, &msg, sizeof(msg));
    
    return NULL;
}

/* Block until first task completes. Returns immediately when any task finishes.
   winner: index of the task that completed first
   result: result of the winning task
   Returns 0 on success.
   Note: Other tasks continue running in background (cancelled on nursery cleanup). */
int cc_block_race(int count, CCTask* tasks, int* winner, intptr_t* result) {
    if (count <= 0) return EINVAL;
    if (!tasks) return EINVAL;

    CCChan* done_chan = cc_chan_create(count);
    if (!done_chan) return ENOMEM;

    CCNursery* n = cc_nursery_create();
    if (!n) {
        cc_chan_free(done_chan);
        return ENOMEM;
    }

    volatile int winner_flag = 0;
    CCBlockRaceSlot* slots = (CCBlockRaceSlot*)calloc((size_t)count, sizeof(CCBlockRaceSlot));
    if (!slots) {
        cc_nursery_free(n);
        cc_chan_free(done_chan);
        return ENOMEM;
    }

    for (int i = 0; i < count; i++) {
        slots[i].task = tasks[i];
        slots[i].index = i;
        slots[i].done_chan = done_chan;
        slots[i].winner_flag = &winner_flag;
    }

    /* Spawn all tasks */
    for (int i = 0; i < count; i++) {
        int err = cc_nursery_spawn(n, cc__block_race_worker, &slots[i]);
        if (err != 0) {
            cc_nursery_cancel(n);
            cc_nursery_wait(n);
            cc_nursery_free(n);
            cc_chan_free(done_chan);
            free(slots);
            return err;
        }
    }

    /* Wait for first completion */
    CCBlockRaceResult msg;
    int recv_err = cc_chan_recv(done_chan, &msg, sizeof(msg));
    
    if (winner) *winner = msg.index;
    if (result) *result = msg.result;

    /* Cancel remaining tasks - this wakes up workers blocked in cc_block_on_intptr */
    for (int i = 0; i < count; i++) {
        if (i != msg.index) {
            cc_task_cancel(&slots[i].task);
        }
    }
    
    /* Now wait for all workers to finish (they should exit quickly after cancel) */
    cc_nursery_cancel(n);
    cc_nursery_wait(n);
    cc_nursery_free(n);
    cc_chan_close(done_chan);
    cc_chan_free(done_chan);
    free(slots);

    return recv_err;
}

/* --- cc_block_any implementation --- */

/* Block until first SUCCESSFUL task completes. Only fails if ALL tasks fail.
   Current convention: non-negative result indicates success, negative indicates failure.
   winner: index of the first successful task
   result: result of the winning task
   Returns 0 if any task succeeded, ECANCELED if all failed. */
int cc_block_any(int count, CCTask* tasks, int* winner, intptr_t* result) {
    if (count <= 0) return EINVAL;
    if (!tasks) return EINVAL;

    CCChan* done_chan = cc_chan_create(count);
    if (!done_chan) return ENOMEM;

    CCNursery* n = cc_nursery_create();
    if (!n) {
        cc_chan_free(done_chan);
        return ENOMEM;
    }

    CCBlockRaceSlot* slots = (CCBlockRaceSlot*)calloc((size_t)count, sizeof(CCBlockRaceSlot));
    if (!slots) {
        cc_nursery_free(n);
        cc_chan_free(done_chan);
        return ENOMEM;
    }

    for (int i = 0; i < count; i++) {
        slots[i].task = tasks[i];
        slots[i].index = i;
        slots[i].done_chan = done_chan;
    }

    /* Spawn all tasks */
    for (int i = 0; i < count; i++) {
        int err = cc_nursery_spawn(n, cc__block_race_worker, &slots[i]);
        if (err != 0) {
            cc_nursery_cancel(n);
            cc_nursery_wait(n);
            cc_nursery_free(n);
            cc_chan_free(done_chan);
            free(slots);
            return err;
        }
    }

    /* Wait for first SUCCESS (non-negative result).
       Continue draining completions while tasks fail. */
    int found_success = 0;
    int completed = 0;
    CCBlockRaceResult first_result = {0};
    
    while (completed < count && !found_success) {
        CCBlockRaceResult msg;
        int recv_err = cc_chan_recv(done_chan, &msg, sizeof(msg));
        if (recv_err != 0) break;
        
        completed++;
        
        if (msg.result >= 0) {
            found_success = 1;
            first_result = msg;
        }
    }

    if (found_success) {
        if (winner) *winner = first_result.index;
        if (result) *result = first_result.result;
    } else {
        if (winner) *winner = -1;
        if (result) *result = -1;
    }

    /* Cancel remaining tasks - this wakes up workers blocked in cc_block_on_intptr */
    for (int i = 0; i < count; i++) {
        if (!found_success || i != first_result.index) {
            cc_task_cancel(&slots[i].task);
        }
    }
    
    /* Now wait for all workers to finish */
    cc_nursery_cancel(n);
    cc_nursery_wait(n);
    cc_nursery_free(n);
    cc_chan_close(done_chan);
    cc_chan_free(done_chan);
    free(slots);

    return found_success ? 0 : ECANCELED;
}
