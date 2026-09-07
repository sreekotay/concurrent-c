/*
 * Executor-backed scheduler facade with cooperative deadlines.
 */

#include <ccc/cc_sched.cch>
#include <ccc/cc_exec.cch>
#include <ccc/std/task.cch>
#include "fiber_internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct CCSpawnTask {
    void* (*fn)(void*);
    void* arg;
    void* result;
    int done;
    int detached;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    /* Fiber-aware join support.
     * done_atomic mirrors done but is safe to use with cc__fiber_park_if.
     * Set with release ordering AFTER result is stored and AFTER done=1. */
    _Atomic int done_atomic;
    cc__fiber* _Atomic waiter_fiber;
};

/* Accessor to set spawn task in CCTask._data */
static inline void cc__set_spawn_task(CCTask* t, struct CCSpawnTask* task) {
    /* CCTaskSpawnInternal layout: just a pointer at offset 0 */
    struct CCSpawnTask** ptr = (struct CCSpawnTask**)t->_data;
    *ptr = task;
}

static CCExec* g_sched_exec = NULL;
static pthread_mutex_t g_sched_mu = PTHREAD_MUTEX_INITIALIZER;

static size_t cc__env_size(const char* name, size_t fallback) {
    const char* v = getenv(name);
    if (!v || !*v) return fallback;
    char* end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    if (!end || end == v || *end != 0) return fallback;
    return (size_t)n;
}

static size_t cc__default_workers(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) return (size_t)n;
    return 4;
}

static CCExec* cc__sched_exec_lazy(void) {
    pthread_mutex_lock(&g_sched_mu);
    if (!g_sched_exec) {
        /* Unify with fiber scheduler: check programmatic setting first, then env */
        size_t workers = cc_sched_get_num_workers();
        if (workers == 0) {
            workers = cc__env_size("CC_WORKERS", cc__default_workers());
        }
        /* qcap=0 means "unbounded, grow on demand". cc_thread_spawn() is
         * fire-and-forget and must never silently drop a task; backpressure
         * here would also risk deadlock (e.g. a fiber that spawns thread
         * work and is the only drainer). Callers that want a hard cap can
         * set CC_SPAWN_QUEUE_CAP explicitly. */
        size_t qcap = cc__env_size("CC_SPAWN_QUEUE_CAP", 0);
        g_sched_exec = cc_exec_create(workers, qcap);
    }
    CCExec* ex = g_sched_exec;
    pthread_mutex_unlock(&g_sched_mu);
    return ex;
}

int cc_scheduler_init(void) {
    return cc__sched_exec_lazy() ? 0 : -1;
}

void cc_scheduler_shutdown(void) {
    pthread_mutex_lock(&g_sched_mu);
    if (g_sched_exec) {
        cc_exec_shutdown(g_sched_exec);
        cc_exec_free(g_sched_exec);
        g_sched_exec = NULL;
    }
    pthread_mutex_unlock(&g_sched_mu);
}

int cc_scheduler_stats(CCSchedulerStats* out) {
    if (!out) return EINVAL;
    CCExec* ex = cc__sched_exec_lazy();
    if (!ex) return ENOMEM;
    CCExecStats stats;
    int err = cc_exec_stats(ex, &stats);
    if (err != 0) return err;
    out->workers = stats.workers;
    out->queue_cap = stats.queue_cap;
    out->queue_len = stats.queue_len;
    return 0;
}

static void cc__spawn_task_free_internal(struct CCSpawnTask* task) {
    if (!task) return;
    pthread_mutex_destroy(&task->mu);
    pthread_cond_destroy(&task->cv);
    free(task);
}

static void cc__spawn_task_job(void* arg) {
    struct CCSpawnTask* task = (struct CCSpawnTask*)arg;
    if (!task) return;
    void* r = NULL;
    if (task->fn) r = task->fn(task->arg);
    pthread_mutex_lock(&task->mu);
    task->result = r;
    task->done = 1;
    /* Grab any waiting fiber under the lock (prevents it missing done=1). */
    cc__fiber* waiter = (cc__fiber*)atomic_exchange_explicit(
        &task->waiter_fiber, NULL, memory_order_acq_rel);
    /* done_atomic release store: ensures result/done are visible to fiber
     * via cc__fiber_park_if's acquire load on done_atomic. */
    atomic_store_explicit(&task->done_atomic, 1, memory_order_release);
    pthread_cond_broadcast(&task->cv);
    int detach = task->detached;
    pthread_mutex_unlock(&task->mu);
    /* Wake fiber outside lock — unpark is safe to call from any thread. */
    if (waiter) cc__fiber_unpark(waiter);
    if (detach) {
        cc__spawn_task_free_internal(task);
    }
}

/* NEW unified API: cc_thread_spawn returns CCTask value */
CCTask cc_thread_spawn(void* (*fn)(void*), void* arg) {
    CCTask out;
    memset(&out, 0, sizeof(out));
    if (!fn) return out;
    CCExec* ex = cc__sched_exec_lazy();
    if (!ex) return out;
    struct CCSpawnTask* task = (struct CCSpawnTask*)calloc(1, sizeof(struct CCSpawnTask));
    if (!task) return out;
    task->fn = fn;
    task->arg = arg;
    pthread_mutex_init(&task->mu, NULL);
    pthread_cond_init(&task->cv, NULL);
    /* Use the blocking variant so we never silently drop a task.
     *   - Default config: scheduler exec is unbounded (qcap=0), so this
     *     grows the queue on demand and effectively never blocks.
     *   - If the user explicitly sets CC_SPAWN_QUEUE_CAP, the queue is
     *     bounded and this blocks the caller — real backpressure.
     * Previously the non-blocking submit turned EAGAIN into an INVALID
     * CCTask, which cc_block_on_intptr() silently "joined" as result=0,
     * producing bogus checksums (see perf_spawn_v2_vs_thread). */
    int err = cc_exec_submit_blocking(ex, cc__spawn_task_job, task);
    if (err != 0) {
        cc__spawn_task_free_internal(task);
        return out;
    }
    out.kind = CC_TASK_KIND_SPAWN;
    cc__set_spawn_task(&out, task);
    return out;
}

/* Helper function that unpacks and calls a closure */
static void* cc__closure0_wrapper(void* arg) {
    CCClosure0* pc = (CCClosure0*)arg;
    void* result = pc->fn(pc->env);
    if (pc->drop) pc->drop(pc->env);
    free(pc);
    return result;
}

/* NEW unified API: cc_thread_spawn_closure0 returns CCTask value */
CCTask cc_thread_spawn_closure0(CCClosure0 c) {
    /* Wrap closure in a simple function that calls the closure */
    /* For simplicity, we allocate and pass the closure as arg */
    CCTask out;
    memset(&out, 0, sizeof(out));
    if (!c.fn) return out;
    
    /* Create a heap copy of the closure */
    CCClosure0* heap_c = (CCClosure0*)malloc(sizeof(CCClosure0));
    if (!heap_c) return out;
    *heap_c = c;
    
    return cc_thread_spawn(cc__closure0_wrapper, heap_c);
}

/* Legacy API for backward compatibility */
int cc_thread_spawn_legacy(struct CCSpawnTask** out_task, void* (*fn)(void*), void* arg) {
    if (!out_task || !fn) return EINVAL;
    *out_task = NULL;
    CCExec* ex = cc__sched_exec_lazy();
    if (!ex) return ENOMEM;
    struct CCSpawnTask* task = (struct CCSpawnTask*)calloc(1, sizeof(struct CCSpawnTask));
    if (!task) return ENOMEM;
    task->fn = fn;
    task->arg = arg;
    pthread_mutex_init(&task->mu, NULL);
    pthread_cond_init(&task->cv, NULL);
    int err = cc_exec_submit(ex, cc__spawn_task_job, task);
    if (err != 0) {
        cc__spawn_task_free_internal(task);
        return err;
    }
    *out_task = task;
    return 0;
}

int cc_thread_task_join(struct CCSpawnTask* task) {
    if (!task) return EINVAL;
    pthread_mutex_lock(&task->mu);
    while (!task->done) {
        pthread_cond_wait(&task->cv, &task->mu);
    }
    pthread_mutex_unlock(&task->mu);
    return 0;
}

int cc_thread_task_join_result(struct CCSpawnTask* task, void** out_result) {
    if (!task) return EINVAL;
    pthread_mutex_lock(&task->mu);
    while (!task->done) {
        pthread_cond_wait(&task->cv, &task->mu);
    }
    if (out_result) *out_result = task->result;
    pthread_mutex_unlock(&task->mu);
    return 0;
}

/* Strict predicate wait for fiber context:
 * keep parking until *done becomes non-zero, tolerating unrelated wakeups. */
static inline void cc__fiber_wait_until_done(_Atomic int* done, const char* reason) {
    while (atomic_load_explicit(done, memory_order_acquire) == 0) {
        cc__fiber_clear_pending_unpark();
        CC_FIBER_PARK_IF(done, 0, reason);
    }
}

/* Fiber-aware join: park the calling fiber instead of blocking the worker thread.
 * Must only be called from within a fiber context (cc__fiber_in_context() == 1).
 *
 * Protocol:
 *   1. Lock mutex, check done — fast path if already done.
 *   2. Register current fiber as waiter (under lock, so completion can't race past it).
 *   3. Unlock, then park on done_atomic via cc__fiber_park_if.
 *      If completion fires between unlock and park, pending_unpark or the flag
 *      check in cc__fiber_park_if prevents the park.
 *   4. On wakeup done_atomic==1, result is visible via release/acquire ordering. */
int cc_thread_task_join_fiber(struct CCSpawnTask* task, void** out_result) {
    if (!task) return EINVAL;
    pthread_mutex_lock(&task->mu);
    if (!task->done) {
        /* Register as waiter under lock so the completion handler can see us. */
        atomic_store_explicit(&task->waiter_fiber,
                              (cc__fiber*)cc__fiber_current(),
                              memory_order_relaxed);
        pthread_mutex_unlock(&task->mu);
        /* Park until done_atomic is set.
         *
         * IMPORTANT: clear stale pending_unpark before each park attempt.
         * Otherwise an unrelated wake can cause cc__fiber_park_if() to return
         * immediately while done_atomic is still 0, and we'd read task->result
         * before the spawn job publishes it. */
        cc__fiber_wait_until_done(&task->done_atomic, "spawn_join");
        /* Clear stale waiter registration in case park bailed on flag or
         * pending_unpark without the completion handler clearing it. */
        atomic_store_explicit(&task->waiter_fiber, NULL, memory_order_relaxed);
    } else {
        pthread_mutex_unlock(&task->mu);
    }
    /* done_atomic acquire (in park_if or flag check) pairs with the release store
     * in cc__spawn_task_job, so task->result is visible here. */
    if (out_result) *out_result = task->result;
    return 0;
}

void cc_thread_task_free(struct CCSpawnTask* task) {
    if (!task) return;
    pthread_mutex_lock(&task->mu);
    if (task->done) {
        pthread_mutex_unlock(&task->mu);
        cc__spawn_task_free_internal(task);
        return;
    }
    task->detached = 1;
    pthread_mutex_unlock(&task->mu);
}

/* Non-blocking poll: check if thread task is done without blocking */
int cc_thread_task_poll_done(struct CCSpawnTask* task) {
    if (!task) return 0;
    pthread_mutex_lock(&task->mu);
    int done = task->done;
    pthread_mutex_unlock(&task->mu);
    return done;
}

/* Get result from a completed thread task (caller must ensure task is done) */
void* cc_thread_task_get_result(struct CCSpawnTask* task) {
    if (!task) return NULL;
    pthread_mutex_lock(&task->mu);
    void* result = task->result;
    pthread_mutex_unlock(&task->mu);
    return result;
}

int cc_sleep_ms(unsigned int ms) {
    /* Fiber: park this fiber; the worker runs someone else. Sysmon
     * signals when the deadline is due. Thread: nanosleep. */
    if (cc__fiber_in_context()) {
        cc__fiber_sleep_park(ms);
        return 0;
    }
    /* Thread context: block the OS thread directly. */
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        continue;
    }
    return 0;
}

CCDeadline cc_deadline_none(void) {
    CCDeadline d; d.deadline.tv_sec = 0; d.deadline.tv_nsec = 0; d.cancelled = 0; return d; }

CCDeadline cc_deadline_after_ms(uint64_t ms) {
    CCDeadline d = cc_deadline_none();
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    uint64_t nsec = (uint64_t)now.tv_nsec + (ms % 1000) * 1000000ULL;
    d.deadline.tv_sec = now.tv_sec + (time_t)(ms / 1000) + (time_t)(nsec / 1000000000ULL);
    d.deadline.tv_nsec = (long)(nsec % 1000000000ULL);
    return d;
}

bool cc_deadline_expired(const CCDeadline* d) {
    if (!d || d->cancelled) return true;
    if (d->deadline.tv_sec == 0) return false;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (now.tv_sec > d->deadline.tv_sec) return true;
    if (now.tv_sec == d->deadline.tv_sec && now.tv_nsec >= d->deadline.tv_nsec) return true;
    return false;
}

/* Undefine language-level macros to expose the CCDeadline*-taking API. */
#undef cc_cancel
#undef cc_is_cancelled
void cc_cancel(CCDeadline* d) { if (d) d->cancelled = 1; }
bool cc_is_cancelled(const CCDeadline* d) { return d && d->cancelled; }

const struct timespec* cc_deadline_as_timespec(const CCDeadline* d, struct timespec* out) {
    if (!d || d->deadline.tv_sec == 0) return NULL;
    if (out) *out = d->deadline;
    return out;
}

size_t sched_v2_ready_depth(void);
uint32_t sched_v2_current_fiber_suspends(void);

/* ============================================================================
 * Adaptive spawn gate (CC_PAR_ADAPT, default on).
 *
 * If this site's leaf arms are cheaper than a spawn, do not spawn.
 * Denied arms run inline at the join, so nothing strands. A shallow
 * ready queue is not a reason to admit: a recursive cheap tree keeps
 * the queue empty and would otherwise become a spawn/wake storm.
 *
 * A site is a @parallel thunk pointer. Clean leaf-arm CPU time below
 * CC_PAR_CHURN_NS (default 8us) is cheap; at or above is heavy. Heavy
 * evidence commits REAL immediately — REAL is never denied, so one
 * heavy sample is enough to keep legitimate work spawning. Cheap
 * evidence commits CHURN from virgin on the first sample (a storm is
 * all cheap); demoting REAL to CHURN takes CC_PAR_CHEAP_STREAK
 * consecutive cheap resamples, so one freak-cheap sample cannot
 * serialize a real site. A 1-in-2^20 CHURN resample (inline gate) and
 * a 1-in-1024 REAL wrap keep a wrong verdict from sticking.
 * CC_PAR_ADAPT=0 always spawns.
 *
 * Table: fixed open-addressed, insert-only. Overflow stays virgin and
 * always spawns. */
#define CC_PAR_SITE_SLOTS 256u /* power of two */

enum {
    CC_PAR_SITE_VIRGIN = 0,
    CC_PAR_SITE_CHURN = 1,
    CC_PAR_SITE_REAL = 2,
};
#define CC_PAR_RESAMPLE_MASK 1023u
#define CC_PAR_CHURN_RESAMPLE_MASK 0xfffffu /* 1-in-2^20; see deny_fast */
#define CC_PAR_CHEAP_STREAK 3u /* REAL→CHURN; toward-deny is the starving direction */
#define CC_PAR_FLOOD_DEPTH 512u /* virgin/TCC wrap cap: above any coarse fan-out */

typedef struct {
    /* Prefix is CCParSiteGate (cc_sched.cch). Keep first, keep in order. */
    _Atomic int      state;
    _Atomic int      deny_depth; /* adapt_backlog, written before CHURN */
    _Atomic(void*)   fn;
    _Atomic uint32_t cheap_streak; /* consecutive cheap while REAL */
    /* Set when any wrapped arm suspends (join or channel). The virgin
     * flood bound does not deny those sites. Join is not a meeting:
     * this bit does not commit REAL and does not block CHURN. */
    _Atomic uint32_t saw_suspend;
    _Atomic uint32_t attempts; /* wrapped VIRGIN spawns */
    _Atomic uint64_t min_ns;   /* CC_PAR_ADAPT_DEBUG */
    _Atomic uint64_t max_ns;
    _Atomic uint32_t samples;
} cc_par_site;

_Static_assert(offsetof(cc_par_site, state) == offsetof(CCParSiteGate, state) &&
               offsetof(cc_par_site, deny_depth) ==
                   offsetof(CCParSiteGate, deny_depth) &&
               sizeof(int) == sizeof(_Atomic int),
               "cc_par_site prefix must match public CCParSiteGate");

/* Bound trampoline cost on sites that never yield a clean sample. */
#define CC_PAR_LEARN_ATTEMPTS (1u << 20)

static cc_par_site g_par_sites[CC_PAR_SITE_SLOTS];

static _Atomic int g_par_adapt_cached = -1;      /* CC_PAR_ADAPT, default 1 */
static _Atomic long g_par_churn_ns_cached = -1;  /* CC_PAR_CHURN_NS */
static _Atomic int g_par_adapt_backlog_cached = -1; /* CC_PAR_ADAPT_BACKLOG */

static void cc_par_adapt_dump(void);

static int cc_parallel_adapt_on(void) {
    int v = atomic_load_explicit(&g_par_adapt_cached, memory_order_relaxed);
    if (v >= 0) return v;
    const char* s = getenv("CC_PAR_ADAPT");
    int newv = (s && s[0] == '0') ? 0 : 1;
    const char* dbg = getenv("CC_PAR_ADAPT_DEBUG");
    if (dbg && dbg[0] && dbg[0] != '0')
        atexit(cc_par_adapt_dump);
    atomic_store_explicit(&g_par_adapt_cached, newv, memory_order_relaxed);
    return newv;
}

/* Below this, a clean leaf is cheaper than spawn+join (~1.5us). Sit
 * ~5x over that so tens-of-us real arms stay REAL. */
static uint64_t cc_parallel_churn_ns(void) {
    long v = atomic_load_explicit(&g_par_churn_ns_cached, memory_order_relaxed);
    if (v >= 0) return (uint64_t)v;
    const char* s = getenv("CC_PAR_CHURN_NS");
    long newv = 8000;
    if (s && s[0]) {
        long n = strtol(s, NULL, 10);
        if (n >= 0 && n <= 1000000000L) newv = n;
    }
    atomic_store_explicit(&g_par_churn_ns_cached, newv, memory_order_relaxed);
    return (uint64_t)newv;
}

/* Ready-queue depth at which a CHURN site denies. */
static int cc_parallel_adapt_backlog(void) {
    int v = atomic_load_explicit(&g_par_adapt_backlog_cached,
                                 memory_order_relaxed);
    if (v >= 0) return v;
    const char* s = getenv("CC_PAR_ADAPT_BACKLOG");
    int newv = 4;
    if (s && s[0]) {
        long n = strtol(s, NULL, 10);
        if (n > 0 && n <= 1000000L) newv = (int)n;
    }
    atomic_store_explicit(&g_par_adapt_backlog_cached, newv,
                          memory_order_relaxed);
    return newv;
}

/* Per-thread denial/spawn counts. Sampler rejects an arm if either
 * moved (the arm absorbed children — not a leaf). Valid because an
 * accepted sample never suspended, so it never migrated. Lowering
 * notes inlined arms via CC_PAR_NOTE_INLINE_ARM. */
#if defined(__TINYC__)
CCParTls* cc__par_tls(void) {
    cc_rt_tls* t = cc_rt_tls_get();
    return t ? &t->par : NULL;
}
#else
_Thread_local CCParTls __cc_par_tls;
#endif
#define tls_par_denials (cc__par_tls()->denials)
#define tls_par_spawn_calls (cc__par_tls()->spawn_calls)
#define tls_par_tick (cc__par_tls()->real_tick) /* REAL resample */

/* CC_PAR_ADAPT_DEBUG=1: dump the site table at exit. */
static void cc_par_adapt_dump(void) {
    fprintf(stderr, "[par_adapt] churn_ns=%llu\n",
            (unsigned long long)cc_parallel_churn_ns());
    for (unsigned i = 0; i < CC_PAR_SITE_SLOTS; i++) {
        cc_par_site* s = &g_par_sites[i];
        void* fn = atomic_load_explicit(&s->fn, memory_order_acquire);
        if (!fn) continue;
        int st = atomic_load_explicit(&s->state, memory_order_relaxed);
        fprintf(stderr,
                "[par_adapt] site fn=%p samples=%u min_ns=%llu "
                "max_ns=%llu (%s)\n",
                fn,
                atomic_load_explicit(&s->samples, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&s->min_ns,
                                                         memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&s->max_ns,
                                                         memory_order_relaxed),
                st == CC_PAR_SITE_CHURN  ? "churn"
                : st == CC_PAR_SITE_REAL ? "real"
                                         : "virgin");
        if (atomic_load_explicit(&s->saw_suspend, memory_order_relaxed))
            fprintf(stderr, "[par_adapt]   (site has suspending arms)\n");
    }
}

/* One-entry TLS memo: recursive constructs call with the same thunk
 * millions of times in a row, and the denial fast path runs per node —
 * the hash probe was ~half the denied-spawn cost. */
#if defined(__TINYC__)
#define tls_par_site_fn (cc_rt_tls_get()->par_site_fn)
#define tls_par_site (*(cc_par_site**)&(cc_rt_tls_get()->par_site))
#else
static __thread void* tls_par_site_fn = NULL;
static __thread cc_par_site* tls_par_site = NULL;
#endif

static cc_par_site* cc_par_site_get_slow(void* (*fn)(void*)) {
    uintptr_t h = (uintptr_t)fn;
    h ^= h >> 17;
    h *= 0x9E3779B97F4A7C15ull;
    uint32_t idx = (uint32_t)(h >> 32) & (CC_PAR_SITE_SLOTS - 1u);
    for (uint32_t k = 0; k < 8; k++) {
        cc_par_site* s = &g_par_sites[(idx + k) & (CC_PAR_SITE_SLOTS - 1u)];
        void* cur = atomic_load_explicit(&s->fn, memory_order_acquire);
        if (cur == (void*)fn) return s;
        if (cur == NULL) {
            void* expect = NULL;
            if (atomic_compare_exchange_strong_explicit(
                    &s->fn, &expect, (void*)fn,
                    memory_order_acq_rel, memory_order_acquire))
                return s;
            if (expect == (void*)fn) return s;
        }
    }
    return NULL; /* probe window full: stay unclassified, always spawn */
}

static inline cc_par_site* cc_par_site_get(void* (*fn)(void*)) {
    if (tls_par_site_fn == (void*)fn) return tls_par_site;
    cc_par_site* s = cc_par_site_get_slow(fn);
    if (s) {
        tls_par_site_fn = (void*)fn;
        tls_par_site = s;
    }
    return s;
}

/* Gate resolver for the lowering's inline deny path (cc_sched.cch).
 * Never NULL: when the adaptive gate is off or the table is full, hand
 * back a record whose state never becomes CHURN, so the emitted code
 * caches a pointer once and its fast path stays branch-predictable. */
static cc_par_site g_par_site_off; /* state stays VIRGIN forever */

const CCParSiteGate* cc_parallel_site_gate(void* (*fn)(void*)) {
    if (!fn || !cc_parallel_adapt_on())
        return (const CCParSiteGate*)&g_par_site_off;
    cc_par_site* s = cc_par_site_get(fn);
    return (const CCParSiteGate*)(s ? s : &g_par_site_off);
}

/* Thread CPU time: wall time bills stolen timeslices. Valid because a
 * never-suspended arm never migrates. */
static uint64_t cc_par_cpu_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

typedef struct {
    void* (*fn)(void*);
    void*        arg;
    cc_par_site* site;
} cc_par_timed;

static void* cc_par_timed_run(void* p) {
    cc_par_timed w = *(cc_par_timed*)p;
    free(p);
    uint32_t s0 = sched_v2_current_fiber_suspends();
    uint64_t d0 = tls_par_denials;
    uint64_t c0 = tls_par_spawn_calls;
    uint64_t t0 = cc_par_cpu_ns();
    void* r = w.fn(w.arg);
    uint64_t dt = cc_par_cpu_ns() - t0;
    /* Leaf only: suspend, nested spawn, or absorbed denial means the
     * duration is a subtree, not a body. Sites that never sample clean
     * stay virgin and keep spawning. */
    if (sched_v2_current_fiber_suspends() != s0) {
        /* Subtree / join: not a leaf sample. Do not pin REAL — a
         * recursive @parallel joins on every inner node; meetings use
         * `@parallel spawn`. */
        atomic_store_explicit(&w.site->saw_suspend, 1, memory_order_relaxed);
    }
    if (sched_v2_current_fiber_suspends() == s0 && tls_par_denials == d0 &&
        tls_par_spawn_calls == c0) {
        cc_par_site* s = w.site;
        atomic_fetch_add_explicit(&s->samples, 1, memory_order_relaxed);
        uint64_t m = atomic_load_explicit(&s->min_ns, memory_order_relaxed);
        while ((m == 0 || dt < m) &&
               !atomic_compare_exchange_weak_explicit(
                   &s->min_ns, &m, dt, memory_order_relaxed,
                   memory_order_relaxed)) {}
        m = atomic_load_explicit(&s->max_ns, memory_order_relaxed);
        while (dt > m && !atomic_compare_exchange_weak_explicit(
                             &s->max_ns, &m, dt, memory_order_relaxed,
                             memory_order_relaxed)) {}
        int st = atomic_load_explicit(&s->state, memory_order_relaxed);
        if (dt >= cc_parallel_churn_ns()) {
            atomic_store_explicit(&s->cheap_streak, 0, memory_order_relaxed);
            if (st != CC_PAR_SITE_REAL)
                atomic_store_explicit(&s->state, CC_PAR_SITE_REAL,
                                      memory_order_relaxed);
        } else if (st != CC_PAR_SITE_CHURN) {
            uint32_t k = 1;
            if (st == CC_PAR_SITE_REAL)
                k = atomic_fetch_add_explicit(&s->cheap_streak, 1,
                                              memory_order_relaxed) + 1;
            if (st == CC_PAR_SITE_VIRGIN || k >= CC_PAR_CHEAP_STREAK) {
                atomic_store_explicit(&s->cheap_streak, 0,
                                      memory_order_relaxed);
                /* deny_depth before state: inline gate reads state first. */
                atomic_store_explicit(&s->deny_depth,
                                      cc_parallel_adapt_backlog(),
                                      memory_order_relaxed);
                atomic_store_explicit(&s->state, CC_PAR_SITE_CHURN,
                                      memory_order_release);
            }
        }
    }
    return r;
}

CCTask cc_parallel_spawn(void* (*fn)(void*), void* arg) {
    CCTask invalid;
    memset(&invalid, 0, sizeof(invalid));
    if (!fn) return invalid;
    tls_par_spawn_calls++;

    /* site != NULL past this block means wrap for measurement. */
    cc_par_site* site = NULL;
    if (cc_parallel_adapt_on() && (site = cc_par_site_get(fn)) != NULL) {
        int st = atomic_load_explicit(&site->state, memory_order_relaxed);
        if (st == CC_PAR_SITE_CHURN) {
            /* Native: deny_fast already denied except a 1-in-2^20
             * resample, which lands here to wrap and re-measure. TCC
             * has no inline gate — deny here except that same trickle.
             * Cheap work is not admitted just because the queue is
             * shallow. */
            if (sched_v2_ready_depth() >= CC_PAR_FLOOD_DEPTH) {
                tls_par_denials++;
                return invalid;
            }
#if defined(__TINYC__)
            if ((++tls_par_tick & CC_PAR_CHURN_RESAMPLE_MASK) != 0) {
                tls_par_denials++;
                return invalid;
            }
#endif
        } else if (st == CC_PAR_SITE_REAL) {
            /* Never denied. Rare wrap so a wrong REAL can recover. */
            if ((++tls_par_tick & CC_PAR_RESAMPLE_MASK) != 0)
                site = NULL;
        } else {
            /* VIRGIN: spawn (and wrap to learn). Flood-deny a site
             * that has never parked, past a depth no coarse construct
             * reaches. A parked virgin (channel) keeps spawning. */
            if (sched_v2_ready_depth() >= CC_PAR_FLOOD_DEPTH &&
                !atomic_load_explicit(&site->saw_suspend,
                                      memory_order_relaxed)) {
                tls_par_denials++;
                return invalid;
            }
            if (atomic_fetch_add_explicit(&site->attempts, 1,
                                          memory_order_relaxed) >=
                CC_PAR_LEARN_ATTEMPTS)
                site = NULL;
        }
    } else {
        site = NULL;
    }

    void* (*spawn_fn)(void*) = fn;
    void* spawn_arg = arg;
    cc_par_timed* w = NULL;
    if (site) {
        w = (cc_par_timed*)malloc(sizeof(*w));
        if (w) {
            w->fn = fn;
            w->arg = arg;
            w->site = site;
            spawn_fn = cc_par_timed_run;
            spawn_arg = w;
        }
    }

    CCTask t = cc_fiber_spawn_task(spawn_fn, spawn_arg);
    if (t.kind == CC_TASK_KIND_INVALID) {
        free(w);
        tls_par_denials++;
    }
    return t;
}

CCTask cc_parallel_spawn_admit(void* (*fn)(void*), void* arg) {
    CCTask invalid;
    memset(&invalid, 0, sizeof(invalid));
    if (!fn)
        return invalid;
    tls_par_spawn_calls++;
    return cc_fiber_spawn_task(fn, arg);
}

void cc_parallel_join(CCTask t) {
    (void)cc_block_on_intptr(t);
}

