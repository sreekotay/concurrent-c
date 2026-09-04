/*
 * Structured concurrency nursery built on the fiber scheduler.
 * 
 * spawn() pushes tasks to global queue, workers execute them.
 * Nursery tracks tasks for join and handles cancellation/deadlines.
 */

#include <ccc/cc_nursery.cch>
#include <ccc/cc_channel.cch>
#include <ccc/cc_arena.cch>
#include <ccc/std/task.cch>

#include "wake_primitive.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fiber_sched_boundary.h"
#include "sched_v2.h"

#ifndef CC__NURSERY_SPAWN_CANCELLED
#define CC__NURSERY_SPAWN_CANCELLED (-1000)
#endif

static CCResult_void_CCError cc__nursery_errno(int err, const char* msg);

/* ============================================================================
 * Nursery spawn timing instrumentation
 * ============================================================================ */

static inline uint64_t nursery_rdtsc(void) {
    #if defined(__TINYC__)
    return cc_cpu_counter_port();
    #elif defined(__x86_64__) || defined(_M_X64)
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
    #elif defined(__aarch64__) || defined(__arm64__)
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
    #else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    #endif
}

typedef struct {
    _Atomic uint64_t setup_cycles;
    _Atomic uint64_t fiber_spawn_cycles;
    _Atomic uint64_t mutex_cycles;
    _Atomic uint64_t spawn_total_cycles;
    _Atomic uint64_t wait_join_cycles;
    _Atomic uint64_t wait_free_cycles;
    _Atomic uint64_t wait_close_cycles;
    _Atomic uint64_t wait_total_cycles;
    _Atomic size_t spawn_count;
    _Atomic size_t wait_calls;
    _Atomic size_t wait_tasks_joined;
    _Atomic size_t wait_channels_closed;
} nursery_timing;

static nursery_timing g_nursery_timing = {0};
static int g_nursery_timing_enabled = -1;

static int nursery_timing_enabled(void) {
    if (g_nursery_timing_enabled < 0) {
        g_nursery_timing_enabled = getenv("CC_SPAWN_TIMING") != NULL;
        if (g_nursery_timing_enabled) {
            extern void cc_nursery_dump_timing(void);
            atexit(cc_nursery_dump_timing);
        }
    }
    return g_nursery_timing_enabled;
}

__attribute__((used)) void cc_nursery_dump_timing(void) {
    size_t spawn_count = atomic_load(&g_nursery_timing.spawn_count);
    size_t wait_calls = atomic_load(&g_nursery_timing.wait_calls);
    uint64_t setup = atomic_load(&g_nursery_timing.setup_cycles);
    uint64_t spawn = atomic_load(&g_nursery_timing.fiber_spawn_cycles);
    uint64_t mutex = atomic_load(&g_nursery_timing.mutex_cycles);
    uint64_t spawn_total = atomic_load(&g_nursery_timing.spawn_total_cycles);
    uint64_t wait_join = atomic_load(&g_nursery_timing.wait_join_cycles);
    uint64_t wait_free = atomic_load(&g_nursery_timing.wait_free_cycles);
    uint64_t wait_close = atomic_load(&g_nursery_timing.wait_close_cycles);
    uint64_t wait_total = atomic_load(&g_nursery_timing.wait_total_cycles);
    size_t wait_tasks = atomic_load(&g_nursery_timing.wait_tasks_joined);
    size_t wait_closes = atomic_load(&g_nursery_timing.wait_channels_closed);

    if (spawn_count) {
        fprintf(stderr, "\n=== NURSERY SPAWN TIMING (%zu spawns) ===\n", spawn_count);
        fprintf(stderr, "  Total:        %8.1f cycles/spawn (100.0%%)\n", (double)spawn_total / spawn_count);
        fprintf(stderr, "  Breakdown:\n");
        fprintf(stderr, "    setup:       %8.1f cycles/spawn (%5.1f%%)\n",
                (double)setup / spawn_count, 100.0 * setup / spawn_total);
        fprintf(stderr, "    fiber_spawn: %8.1f cycles/spawn (%5.1f%%)\n",
                (double)spawn / spawn_count, 100.0 * spawn / spawn_total);
        fprintf(stderr, "    mutex:       %8.1f cycles/spawn (%5.1f%%)\n",
                (double)mutex / spawn_count, 100.0 * mutex / spawn_total);
    }
    if (wait_calls) {
        fprintf(stderr, "\n=== NURSERY WAIT TIMING (%zu waits) ===\n", wait_calls);
        fprintf(stderr, "  Total:        %8.1f cycles/wait (100.0%%)\n", (double)wait_total / wait_calls);
        fprintf(stderr, "  Tasks joined: %8.1f tasks/wait\n",
                wait_calls ? (double)wait_tasks / wait_calls : 0.0);
        fprintf(stderr, "  Ch closed:    %8.1f chans/wait\n",
                wait_calls ? (double)wait_closes / wait_calls : 0.0);
        fprintf(stderr, "  Breakdown:\n");
        fprintf(stderr, "    join:        %8.1f cycles/wait (%5.1f%%)  %.1f cycles/task\n",
                (double)wait_join / wait_calls,
                wait_total ? 100.0 * wait_join / wait_total : 0.0,
                wait_tasks ? (double)wait_join / wait_tasks : 0.0);
        fprintf(stderr, "    free:        %8.1f cycles/wait (%5.1f%%)  %.1f cycles/task\n",
                (double)wait_free / wait_calls,
                wait_total ? 100.0 * wait_free / wait_total : 0.0,
                wait_tasks ? (double)wait_free / wait_tasks : 0.0);
        fprintf(stderr, "    close:       %8.1f cycles/wait (%5.1f%%)  %.1f cycles/chan\n",
                (double)wait_close / wait_calls,
                wait_total ? 100.0 * wait_close / wait_total : 0.0,
                wait_closes ? (double)wait_close / wait_closes : 0.0);
    }
    if (spawn_count || wait_calls) {
        fprintf(stderr, "==========================================\n\n");
    }
}

/* Nursery child descriptor.
 *
 * Post V1 retirement: every spawned child is a V2 hybrid fiber. The kind
 * field used to switch between V1 (kind=1, u.classic) and V2 (kind=2,
 * u.hybrid); with the V1 spawn path gone, only kind=2 remains. The field is
 * kept (rather than collapsed entirely) so the zero-initialized slots in
 * n->tasks[] stay distinguishable from live ones (kind=0 means empty). */
typedef struct {
    unsigned char kind;      /* 0 = empty slot, 2 = live V2 fiber */
    fiber_v2* hybrid;
} cc_nursery_child;

#ifndef CC_NURSERY_TASK_CAP
#define CC_NURSERY_TASK_CAP 1024
#endif

/* L1 must hold the initial tasks[] — a 1KiB heap grew on the first alloc
 * and HOST_INLINE retire used to free the inline slab as a standalone malloc. */
static size_t cc__nursery_arena_bytes(void) {
    return CC_NURSERY_TASK_CAP * sizeof(cc_nursery_child) + 256;
}

/* Thread-local: current nursery for code running inside nursery-spawned tasks.
   Used by optional runtime deadlock guard in channel.c. */
#if defined(__TINYC__)
#define cc__tls_current_nursery (*(CCNurseryHost**)&(cc_rt_tls_get()->current_nursery))
#else
__thread CCNurseryHost* cc__tls_current_nursery = NULL;
#endif

CCNurseryHost* cc__runtime_current_nursery(void) {
    CCNurseryHost* v2 = sched_v2_current_nursery();
    return v2 ? v2 : cc__tls_current_nursery;
}

struct CCNurseryHost {
    cc_nursery_child* tasks; /* Tasks spawned in this nursery */
    size_t count;
    size_t cap;
    _Atomic int cancelled;  /* release-store on cancel; acquire-load in is_cancelled */
    struct timespec deadline;
    CCChan** closing;
    size_t closing_count;
    size_t closing_cap;
    CCArena arena;
    int owner_placed; /* 1 = handle lives in a parent arena; do not free() */
    pthread_mutex_t mu;
    wake_primitive cancel_wake;  /* Broadcast on cancel for O(1) wake */

    /* Worker-frees-on-DEAD path (default; CC_NURSERY_WORKER_FREES=0 opts
     * out). When the gate is off these fields are inert and the classic
     * nursery-wait iterator continues to own per-child join+release.
     * When on, the v2 worker calls cc_nursery_notify_child_done on
     * MCO_DEAD and cc_nursery_wait becomes a barrier on alive_count. */
    _Atomic size_t alive_count;
    fiber_v2* _Atomic alive_waiter;
    wake_primitive alive_wake;

    /* LIVE → JOINING (wait) or LEFT (leave). After leave the
     * handle is consumed; EMPTY frees the object. */
    _Atomic int end_state;
    _Atomic int finishing;
    void (*leftover_fn)(void*);
    void* leftover_ctx;
};

enum {
    CC_NURSERY_LIVE = 0,
    CC_NURSERY_JOINING = 1,
    CC_NURSERY_LEFT = 2
};

/* LEFT-path EMPTY arbitration lives in ONE atomic word: the high bit
 * of alive_count is the LEFT flag, the low bits are the live-child
 * count. Both exit-qualifying operations (leave's fetch_or, notify's
 * fetch_sub) are RMWs on this word, so they are totally ordered and exactly
 * one of them observes "LEFT and count zero" at its own linearization
 * point. The non-closer must not touch the nursery after its RMW — the
 * closer frees it. (The previous two-variable protocol — end_state stores
 * paired with alive_count loads under seq-cst fences — guaranteed no LOST
 * exit but let both sides qualify; the loser then consulted `finishing`
 * inside an object the winner had already freed. Freed-memory scribble could
 * fake finishing==0 and run a second teardown: the nursery_leave_smoke
 * malloc abort.) end_state remains the user-facing lifecycle state (spawn /
 * leftover / wait refusals, loud-die diagnostics); it no longer arbitrates. */
#define CC_NURSERY_ALIVE_LEFT_BIT ((size_t)1 << (sizeof(size_t) * 8 - 1))
#define CC_NURSERY_ALIVE_COUNT_MASK    (~CC_NURSERY_ALIVE_LEFT_BIT)

static void cc_nursery_die(const char* msg) {
    fprintf(stderr, "cc_nursery: %s\n", msg);
    abort();
}

/* Process-wide gate, latched on first read.  On by default: nursery-
 * spawned fibers go back to the v2 free list the instant a worker
 * observes MCO_DEAD instead of waiting for cc_nursery_wait. Set
 * CC_NURSERY_WORKER_FREES=0 to force the classic join-in-wait path. */
static int cc_nursery_worker_frees_mode(void) {
    static _Atomic int cached = -1;
    int v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (v >= 0) return v;
    const char* s = getenv("CC_NURSERY_WORKER_FREES");
    int newv = !(s && s[0] == '0' && s[1] == 0);
    atomic_store_explicit(&cached, newv, memory_order_relaxed);
    return newv;
}

/* Defined in channel.c (same translation unit via runtime/concurrent_c.c). */
void cc__chan_set_autoclose_owner(CCChan* ch, CCNurseryHost* owner);

/* Channel-close is routed through this hook so that programs which never
 * register a closing channel carry no static reference to cc_chan_close —
 * letting --gc-sections drop the whole channel-close cluster (~4KB) from
 * channel-free binaries. cc_nursery_add_closing_chan — the only path that
 * makes closing_count nonzero — installs it; the teardown loops only consult
 * it when closing_count > 0, so the pointer is always set before it is
 * needed (both writes publish under the same nursery mutex). Cost tracks
 * source behavior: no n.close in the program, no channel code in the
 * binary. */
static void (*g_cc__nursery_chan_close)(CCChan*) = NULL;

static CCNurseryHost* cc__nursery_alloc(void) {
    CCNurseryHost* n = (CCNurseryHost*)malloc(sizeof(CCNurseryHost));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->arena = cc_arena_heap_c(cc__nursery_arena_bytes());
    if (!n->arena.a) {
        free(n);
        return NULL;
    }
    /* tasks[] and closing[] live in the nursery's own arena: release is one
     * arena free, nothing to track piecemeal. Slots >= count are never read
     * (every reader is count-guarded), so no zeroing of arena memory. */
    n->cap = CC_NURSERY_TASK_CAP;
    n->tasks = (cc_nursery_child*)cc_arena_alloc(
        n->arena.a, n->cap * sizeof(cc_nursery_child),
        _Alignof(cc_nursery_child));
    if (!n->tasks) {
        cc_arena_destroy(&n->arena);
        free(n);
        return NULL;
    }
    n->owner_placed = 0;
    pthread_mutex_init(&n->mu, NULL);
    wake_primitive_init(&n->cancel_wake);
    wake_primitive_init(&n->alive_wake);
    atomic_store_explicit(&n->alive_count, 0, memory_order_relaxed);
    atomic_store_explicit(&n->alive_waiter, NULL, memory_order_relaxed);
    n->deadline.tv_sec = 0;
    n->deadline.tv_nsec = 0;
    n->closing = NULL;
    n->closing_cap = 0;
    n->closing_count = 0;
    return n;
}

static CCResult_CCNursery_CCError cc__nursery_wrap_ok(CCNurseryHost* h) {
    CCResult_CCNursery_CCError r;
    CCNursery w;
    w.p = h;
    r.ok = 1;
    r.u.value = w;
    return r;
}

static CCResult_CCNursery_CCError cc__nursery_wrap_oom(const char* msg) {
    CCResult_CCNursery_CCError r;
    r.ok = 0;
    r.u.error = CC_ERROR(CC_ERR_OUT_OF_MEMORY, msg);
    return r;
}

CCResult_CCNursery_CCError cc_nursery_create(void) {
    CCNurseryHost* n = cc__nursery_alloc();
    if (!n)
        return cc__nursery_wrap_oom("cc_nursery_create: out of memory");
    return cc__nursery_wrap_ok(n);
}

CCResult_CCNursery_CCError cc_nursery_create_child(CCNursery parent) {
    CCNurseryHost* p = parent.p;
    CCNurseryHost* n;
    if (!p)
        cc_nursery_die("cc_nursery_create_child: parent nursery is null");
    n = cc__nursery_alloc();
    if (!n)
        return cc__nursery_wrap_oom("cc_nursery_create_child: out of memory");
    /* Snapshot parent cancellation/deadline at birth. No live parent pointer. */
    if (cc_nursery_is_cancelled_host(p)) {
        atomic_store_explicit(&n->cancelled, 1, memory_order_release);
    }
    {
        struct timespec inherited_deadline;
        if (cc_nursery_deadline_host(p, &inherited_deadline)) {
            n->deadline = inherited_deadline;
        }
    }
    return cc__nursery_wrap_ok(n);
}

void* cc_nursery_closure_env_alloc_host(CCNurseryHost* n, size_t size, size_t align) {
    if (!n || size == 0) return NULL;
    /* TODO: The arena-backed path gives closures under a nursery a clean,
       deterministic lifetime model, but the spawn benchmark breakdown showed
       env alloc/free is not the throughput bottleneck. If we revisit this for
       performance, prototype a nursery-scoped reclaimable allocator (local heap
       or pooled size classes) under the same explicit lowering shape. */
    return cc_arena_alloc(n->arena.a, size, align);
}

void cc_nursery_cancel_host(CCNurseryHost* n) {
    if (!n) return;
    pthread_mutex_lock(&n->mu);
    atomic_store_explicit(&n->cancelled, 1, memory_order_release);
    /* Snapshot tasks while holding lock */
    size_t task_count = n->count;
    cc_nursery_child* tasks_snapshot = NULL;
    if (task_count > 0) {
        tasks_snapshot = (cc_nursery_child*)malloc(task_count * sizeof(cc_nursery_child));
        if (tasks_snapshot) {
            for (size_t i = 0; i < task_count; i++) {
                tasks_snapshot[i] = n->tasks[i];
            }
        }
    }
    pthread_mutex_unlock(&n->mu);
    
    /* Broadcast to wake any fibers waiting on this nursery's cancel primitive */
    wake_primitive_wake_all(&n->cancel_wake);
    
    /* Unpark all tasks in this nursery so they can check cancellation.
     * This is O(n) but ensures no fiber stays parked after cancel. */
    if (tasks_snapshot) {
        for (size_t i = 0; i < task_count; i++) {
            if (tasks_snapshot[i].kind == 2 && tasks_snapshot[i].hybrid) {
                sched_v2_signal(tasks_snapshot[i].hybrid);
            }
        }
        free(tasks_snapshot);
    }
}

typedef struct {
    CCNurseryHost* nursery;
    sigset_t set;
    CCClosure1 handler;
} cc_nursery_signal_ctx;

static void* cc__nursery_cancel_only(void* env, intptr_t arg0) {
    (void)env;
    CCNurseryHost* n = (CCNurseryHost*)(uintptr_t)arg0;
    if (n) cc_nursery_cancel_host(n);
    return NULL;
}

static void* cc__nursery_signal_thread(void* arg) {
    cc_nursery_signal_ctx* ctx = (cc_nursery_signal_ctx*)arg;
    int sig = 0;
    if (sigwait(&ctx->set, &sig) == 0 && ctx->nursery) {
        (void)cc_closure1_call(ctx->handler, (intptr_t)(uintptr_t)ctx->nursery);
    } else {
        cc_closure1_drop(ctx->handler);
    }
    free(ctx);
    return NULL;
}

static CCResult_void_CCError cc__nursery_install_signals(CCNurseryHost* n,
                                                         const int* signos,
                                                         size_t count,
                                                         CCClosure1 handler) {
    if (!n || !signos || count == 0 || !handler.fn) {
        cc_closure1_drop(handler);
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "nursery signal install: bad args"));
    }
    cc_nursery_signal_ctx* ctx =
        (cc_nursery_signal_ctx*)malloc(sizeof(*ctx));
    if (!ctx) {
        cc_closure1_drop(handler);
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_OUT_OF_MEMORY, "nursery signal install: oom"));
    }
    ctx->nursery = n;
    ctx->handler = handler;
    sigemptyset(&ctx->set);
    for (size_t i = 0; i < count; ++i) {
        sigaddset(&ctx->set, signos[i]);
    }
    int rc = pthread_sigmask(SIG_BLOCK, &ctx->set, NULL);
    if (rc != 0) {
        cc_closure1_drop(handler);
        free(ctx);
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_INTERNAL, "nursery signal install: sigmask failed"));
    }
    pthread_t tid;
    rc = pthread_create(&tid, NULL, cc__nursery_signal_thread, ctx);
    if (rc != 0) {
        cc_closure1_drop(handler);
        free(ctx);
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_INTERNAL, "nursery signal install: thread failed"));
    }
    pthread_detach(tid);
    return cc_ok_CCResult_void_CCError();
}

CCResult_void_CCError cc_nursery_on_signals_n_host(CCNurseryHost* n, const int* signos,
                                              size_t count, CCClosure1 handler) {
    return cc__nursery_install_signals(n, signos, count, handler);
}

CCResult_void_CCError cc_nursery_cancel_on_signals_n_host(CCNurseryHost* n,
                                                     const int* signos,
                                                     size_t count) {
    CCClosure1 cancel_only =
        cc_closure1_make(cc__nursery_cancel_only, NULL, NULL);
    return cc_nursery_on_signals_n_host(n, signos, count, cancel_only);
}

void cc_nursery_set_deadline_host(CCNurseryHost* n, struct timespec abs_deadline) {
    if (!n) return;
    pthread_mutex_lock(&n->mu);
    n->deadline = abs_deadline;
    pthread_mutex_unlock(&n->mu);
}

const struct timespec* cc_nursery_deadline_host(const CCNurseryHost* n, struct timespec* out) {
    if (!n || n->deadline.tv_sec == 0) return NULL;
    if (out) *out = n->deadline;
    return out;
}

CCDeadline cc_nursery_as_deadline_host(const CCNurseryHost* n) {
    CCDeadline d = cc_deadline_none();
    if (!n) { d.cancelled = 1; return d; }
    d.cancelled = atomic_load_explicit(&n->cancelled, memory_order_acquire);
    d.deadline = n->deadline;
    return d;
}

bool cc_nursery_is_cancelled_host(const CCNurseryHost* n) {
    if (!n) return false;
    if (atomic_load_explicit(&n->cancelled, memory_order_acquire)) return true;
    if (n->deadline.tv_sec == 0) return false;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (now.tv_sec > n->deadline.tv_sec) return true;
    if (now.tv_sec == n->deadline.tv_sec && now.tv_nsec >= n->deadline.tv_nsec) return true;
    return false;
}

/* Check if current fiber's nursery is cancelled (convenience for user code). */
bool cc_cancelled(void) {
    return cc_nursery_is_cancelled_host(cc__runtime_current_nursery());
}

/* Get the cancel wake generation for the current nursery (0 if none).
 * Used by channel waits to detect cancellation. */
uint32_t cc_nursery_cancel_gen_host(const CCNurseryHost* n) {
    if (!n) return 0;
    /* Cast away const: TCC treats atomic_load through a const object as
     * assignment to a read-only location. */
    return atomic_load_explicit(&((CCNurseryHost*)n)->cancel_wake.value,
                                memory_order_acquire);
}

/* Wait on the nursery's cancel primitive with timeout (ms).
 * Returns immediately if cancel_gen changed (i.e., cancelled). */
void cc_nursery_cancel_wait_host(CCNurseryHost* n, uint32_t expected_gen, uint32_t timeout_ms) {
    if (!n) return;
    wake_primitive_wait_timeout(&n->cancel_wake, expected_gen, timeout_ms);
}

static int cc_nursery_grow(CCNurseryHost* n) {
    /* Arena-backed: allocate the doubled array and copy; the old array is
     * arena garbage until release. Slots >= count are never read. */
    size_t new_cap = n->cap ? n->cap * 2 : 8;
    cc_nursery_child* nt = (cc_nursery_child*)cc_arena_alloc(
        n->arena.a, new_cap * sizeof(cc_nursery_child),
        _Alignof(cc_nursery_child));
    if (!nt) return ENOMEM;
    if (n->count) memcpy(nt, n->tasks, n->count * sizeof(cc_nursery_child));
    n->tasks = nt;
    n->cap = new_cap;
    return 0;
}

static int cc_nursery_append_child(CCNurseryHost* n, cc_nursery_child child) {
    /* Always under n->mu: redis (and any nested spawn into the same nursery)
     * appends from concurrent fibers; the old unlocked "count < cap" fast
     * path raced on tasks[count++]. */
    pthread_mutex_lock(&n->mu);
    if (n->count == n->cap) {
        int grow_err = cc_nursery_grow(n);
        if (grow_err != 0) {
            pthread_mutex_unlock(&n->mu);
            return grow_err;
        }
    }
    n->tasks[n->count++] = child;
    pthread_mutex_unlock(&n->mu);
    return 0;
}

typedef struct {
    CCTask task;
    /* R1 — user-facing task naming (NULL when the public
     * `cc_nursery_spawn_async` API is used directly).  Strings are
     * caller-owned C string literals (emitted by the spawn-site lowering
     * in preprocess.c → `cc_nursery_spawn_async_named`), so no copy. */
    const char* diag_user_name;
    const char* diag_file;
    int         diag_line;
} cc_nursery_async_spawn;

/* Driver fiber for a `spawn_async`-ed @async task.
 *
 * Contract with the @async state-machine lowering in pass `async_ast`:
 *
 *   A poll call returns CC_FUTURE_PENDING if and only if the @async body
 *   is waiting on an awaited task that is not yet ready.  Trivial state
 *   transitions (loop cond, back-edge, if-branch pick, post-await-success
 *   bookkeeping, return-through-case-999) stay on-CPU inside the poll
 *   function via its outer for(;;) + `continue` shape.
 *
 * That invariant is what makes the `cc_yield()` below unconditionally
 * correct: every PENDING we observe here is a real wait, so handing
 * control back to the scheduler is the right response.  If that ever
 * stops being true (e.g. a new lowering shape that returns PENDING for
 * bookkeeping), both the compiler emit and this loop need to change
 * together. */
/* Forward decls from sched_v2.c for the R1 fiber-stamp call below. */
struct fiber_v2;
extern struct fiber_v2* sched_v2_current_fiber(void);
extern void sched_v2_fiber_set_diag_name(struct fiber_v2* f,
                                          const char* name,
                                          const char* file, int line);

static void* cc__nursery_async_runner(void* arg) {
    cc_nursery_async_spawn* a = (cc_nursery_async_spawn*)arg;
    intptr_t result = 0;
    int err = 0;
    int cancel_sent = 0;
    if (!a) return NULL;

    /* R1: stamp the running fiber with the user-facing async name on
     * first entry, so `cc_rt_diag_current_async_info` answers "what task
     * am I?" for any code running inside this fiber.  Stamped here (not
     * in the spawn caller) because the runner is the first code that
     * executes on the new fiber's own context — `sched_v2_current_fiber`
     * returns this fiber rather than the parent. */
    if (a->diag_user_name || a->diag_file) {
        sched_v2_fiber_set_diag_name(sched_v2_current_fiber(),
                                     a->diag_user_name, a->diag_file,
                                     a->diag_line);
    }

    for (;;) {
        if (!cancel_sent && cc_cancelled()) {
            cc_task_cancel(&a->task);
            cancel_sent = 1;
        }
        CCFutureStatus st = cc_task_poll(&a->task, &result, &err);
        if (st == CC_FUTURE_PENDING) {
            cc_yield();
            continue;
        }
        break;
    }

    cc_task_free(&a->task);
    free(a);
    return NULL;
}

void cc_nursery_notify_child_done(CCNurseryHost* n);

/* V2 is the default scheduler. spawn() routes through sched_v2; spawnhybrid()
 * is kept as an alias for source compatibility during the V1 retirement. */
static int cc__nursery_spawn_errno(CCNurseryHost* n, void* (*fn)(void*), void* arg) {
    if (!n || !fn) return EINVAL;

    int timing = nursery_timing_enabled();
    uint64_t t0 = 0, t1, t2, t3;
    if (timing) t0 = nursery_rdtsc();
    if (timing) t1 = t0;

    /* Worker-frees mode: publish the pending child on alive_count BEFORE
     * enqueuing so a worker that races us to MCO_DEAD always observes a
     * matching increment when it calls cc_nursery_notify_child_done.  In
     * the classic mode this counter is never consulted. The LEFT
     * check and the increment share mu so leave cannot reach EMPTY
     * between them. */
    int worker_frees = cc_nursery_worker_frees_mode();
    pthread_mutex_lock(&n->mu);
    if (atomic_load_explicit(&n->end_state, memory_order_acquire) ==
            CC_NURSERY_LEFT) {
        pthread_mutex_unlock(&n->mu);
        return EINVAL;
    }
    if (cc_nursery_is_cancelled_host(n)) {
        pthread_mutex_unlock(&n->mu);
        return CC__NURSERY_SPAWN_CANCELLED;
    }
    if (worker_frees) {
        atomic_fetch_add_explicit(&n->alive_count, 1, memory_order_relaxed);
    }
    pthread_mutex_unlock(&n->mu);

    fiber_v2* t = sched_v2_spawn_in_nursery(fn, arg, n);
    if (!t) {
        if (worker_frees) {
            /* May last-exit if this increment was the last live child
             * after leave. Do not touch n afterward. */
            cc_nursery_notify_child_done(n);
        }
        return ENOMEM;
    }

    if (timing) t2 = nursery_rdtsc();
    cc_nursery_child child = { .kind = 2, .hybrid = t };
    int append_err = cc_nursery_append_child(n, child);
    if (append_err != 0) {
        if (worker_frees) {
            /* Fiber is enqueued and the worker will notify alive_count on
             * completion — do NOT release here (worker owns the release). */
            return append_err;
        }
        sched_v2_fiber_release(t);
        return append_err;
    }

    if (timing) {
        t3 = nursery_rdtsc();
        atomic_fetch_add_explicit(&g_nursery_timing.setup_cycles, t1 - t0, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.fiber_spawn_cycles, t2 - t1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.mutex_cycles, t3 - t2, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.spawn_total_cycles, t3 - t0, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.spawn_count, 1, memory_order_relaxed);
    }
    return 0;
}

CCResult_void_CCError cc_nursery_spawn_host(CCNurseryHost* n, void* (*fn)(void*), void* arg) {
    return cc__nursery_errno(cc__nursery_spawn_errno(n, fn, arg),
                             "cc_nursery_spawn");
}

CCResult_void_CCError cc_nursery_spawn_async_named_host(CCNurseryHost* n, CCTask task,
                                  const char* diag_user_name,
                                  const char* diag_file,
                                  int diag_line) {
    cc_nursery_async_spawn* a;
    CCResult_void_CCError r;
    if (!n || task.kind == CC_TASK_KIND_INVALID) {
        cc_task_free(&task);
        return cc__nursery_errno(EINVAL, "cc_nursery_spawn_async_named");
    }
    a = (cc_nursery_async_spawn*)malloc(sizeof(*a));
    if (!a) {
        cc_task_free(&task);
        return cc__nursery_errno(ENOMEM, "cc_nursery_spawn_async_named");
    }
    a->task = task;
    a->diag_user_name = diag_user_name;
    a->diag_file = diag_file;
    a->diag_line = diag_line;
    r = cc_nursery_spawn_host(n, cc__nursery_async_runner, a);
    if (!r.ok) {
        cc_task_free(&a->task);
        free(a);
    }
    return r;
}

CCResult_void_CCError cc_nursery_spawnhybrid_async(CCNurseryHost* n, CCTask task) {
    return cc_nursery_spawn_async_named_host(n, task, NULL, NULL, 0);
}

/* Worker-frees mode: v2 worker calls this on MCO_DEAD for every
 * nursery-owned fiber (the fiber itself was already pushed back onto
 * g_v2.free_list by the worker).  Tick the barrier; on the last
 * outstanding child, wake cc_nursery_wait.
 *
 * Why the wake is `prev == 1`-conditional (do NOT move it outside):
 *
 *   - The fiber waiter is parked exactly once via sched_v2_signal,
 *     which is also gated on the final decrement.  Extra signals
 *     would just be no-ops for an already-running fiber but cost
 *     a v2 scheduler hot-path push every time.
 *   - The non-fiber thread waiter (the `else` branch of
 *     `cc_nursery_wait`) loops on `wake_primitive_wait(gen)` and
 *     only breaks when `alive_count == 0`.  It doesn't care about
 *     intermediate decrements — so an intermediate `wake_all`
 *     would wake the thread, force it to re-check, find
 *     `alive_count > 0`, and re-sleep.  Each `wake_primitive_wake_all`
 *     is a futex/ulock syscall regardless of waiter count, and the
 *     extra round trips would turn an O(1) wait into O(N).
 *   - The Dekker pair below (fence + xchg(alive_waiter)) is the
 *     only ARM64-sensitive piece; once that's correct, the
 *     single wake at the boundary is exactly what's needed.
 *
 *   Concretely: moving `wake_primitive_wake_all` out of the
 *   `if (prev == 1)` block would cost ~6000 extra syscalls per
 *   run of `stress/nested_nursery_deep.ccs` with zero correctness
 *   gain.  Don't. */

static void cc_nursery_close_registered(CCNurseryHost* n) {
    for (size_t i = 0; i < n->closing_count; ++i) {
        if (n->closing[i] && g_cc__nursery_chan_close)
            g_cc__nursery_chan_close(n->closing[i]);
    }
}

static void cc_nursery_release(CCNurseryHost* n) {
    int placed;
    /* tasks[] and closing[] are arena contents — the arena free is their
     * free. Owner-placed husks stay in the parent slab: keep owner_placed
     * so a later local @destroy does not free(n). */
    placed = n->owner_placed;
    cc_arena_destroy(&n->arena);
    pthread_mutex_destroy(&n->mu);
    wake_primitive_destroy(&n->alive_wake);
    wake_primitive_destroy(&n->cancel_wake);
    n->tasks = NULL;
    n->count = 0;
    n->closing = NULL;
    n->closing_count = 0;
    if (placed) {
        n->owner_placed = 1;
        return;
    }
    free(n);
}

CCArena cc_nursery_arena_host(CCNurseryHost* n) {
    return n ? n->arena : cc_arena_handle(NULL);
}

static CCResult_void_CCError cc__nursery_errno(int err, const char* msg) {
    if (err == 0) return cc_ok_CCResult_void_CCError();
    if (err == ENOMEM)
        return cc_err_CCResult_void_CCError(CC_ERROR(CC_ERR_OUT_OF_MEMORY, msg));
    if (err == EINVAL)
        return cc_err_CCResult_void_CCError(CC_ERROR(CC_ERR_INVALID_ARG, msg));
    if (err == CC__NURSERY_SPAWN_CANCELLED
#ifdef ECANCELED
        || err == ECANCELED
#endif
        )
        return cc_err_CCResult_void_CCError(CC_ERROR(CC_ERR_CANCELLED, msg));
    return cc_err_CCResult_void_CCError(CC_ERROR(CC_ERR_INTERNAL, msg));
}

static void cc__nursery_owner_destroy(void* p) {
    CCNurseryHost* n = (CCNurseryHost*)p;
    CCResult_void_CCError w;
    if (!n || !n->arena.a) return;
    w = cc_nursery_wait_host(n);
    cc_nursery_free_host(n);
    if (!w.ok) cc_error_exit(w.u.error);
}

static int cc__nursery_init_body(CCNurseryHost* n) {
    memset(n, 0, sizeof(*n));
    n->arena = cc_arena_heap_c(cc__nursery_arena_bytes());
    if (!n->arena.a) return -1;
    n->cap = CC_NURSERY_TASK_CAP;
    n->tasks = (cc_nursery_child*)cc_arena_alloc(
        n->arena.a, n->cap * sizeof(cc_nursery_child),
        _Alignof(cc_nursery_child));
    if (!n->tasks) {
        cc_arena_destroy(&n->arena);
        return -1;
    }
    pthread_mutex_init(&n->mu, NULL);
    wake_primitive_init(&n->cancel_wake);
    wake_primitive_init(&n->alive_wake);
    atomic_store_explicit(&n->alive_count, 0, memory_order_relaxed);
    atomic_store_explicit(&n->alive_waiter, NULL, memory_order_relaxed);
    n->deadline.tv_sec = 0;
    n->deadline.tv_nsec = 0;
    n->closing = NULL;
    n->closing_cap = 0;
    n->closing_count = 0;
    n->owner_placed = 0;
    return 0;
}

CCResult_CCNursery_CCError cc_nursery_create_on(CCArena a) {
    CCNurseryHost* n;
    CCArenaHost* h = cc_arena_host(a);
    if (!h || !h->base)
        cc_nursery_die("cc_arena_create_nursery: arena is null or dead");
    n = (CCNurseryHost*)cc_arena_alloc(h, sizeof(CCNurseryHost),
                                   _Alignof(CCNurseryHost));
    if (!n) {
        fprintf(stderr, "cc_arena_create_nursery: owner cannot back the handle\n");
        return cc__nursery_wrap_oom("cc_arena_create_nursery: owner cannot back the handle");
    }
    if (cc__nursery_init_body(n) != 0) {
        fprintf(stderr, "cc_arena_create_nursery: nursery init failed\n");
        return cc__nursery_wrap_oom("cc_arena_create_nursery: nursery init failed");
    }
    n->owner_placed = 1;
    if (cc_arena_attach(h, n, cc__nursery_owner_destroy) != 0)
        cc_nursery_die("cc_arena_create_nursery: attach to owner failed");
    return cc__nursery_wrap_ok(n);
}

/* Last child is already dead (fiber returned to the pool). Reach EMPTY:
 * close registered channels, run leftover, free. The single-word protocol
 * guarantees a unique caller; `finishing` is a tripwire, not an arbiter —
 * a second entry means the protocol was broken upstream, and deduping it
 * quietly would hide a use-after-free (no silent degradation).
 * The leftover callback must not touch the nursery: it runs before release,
 * and whoever it wakes races the teardown. */
static void cc_nursery_at_empty(CCNurseryHost* n) {
    int expected = 0;
    void (*fn)(void*);
    void* ctx;
    if (!atomic_compare_exchange_strong_explicit(&n->finishing, &expected, 1,
            memory_order_acq_rel, memory_order_relaxed))
        cc_nursery_die("EMPTY entered twice (leave protocol violation)");
    cc_nursery_close_registered(n);
    fn = n->leftover_fn;
    ctx = n->leftover_ctx;
    n->leftover_fn = NULL;
    n->leftover_ctx = NULL;
    if (fn)
        fn(ctx);
    cc_nursery_release(n);
}

void cc_nursery_notify_child_done(CCNurseryHost* n) {
    if (!n) return;
    size_t prev = atomic_fetch_sub_explicit(&n->alive_count, 1, memory_order_acq_rel);
    if (prev == (CC_NURSERY_ALIVE_LEFT_BIT | 1)) {
        /* LEFT path, and this decrement took the count to zero: unique
         * closer (see the protocol comment on the bit). No waiter can
         * exist — wait-after-leave dies — so go straight to teardown. */
        cc_nursery_at_empty(n);
        return;
    }
    if (prev == 1) {
        /* Wait-mode boundary. The nursery is pinned here: the LEFT
         * bit was clear in `prev`, so teardown can only come from
         * cc_nursery_wait / cc_nursery_free on the owner's side, and the
         * owner is either parked below or hasn't reached them.
         *
         * Dekker pair with cc_nursery_wait waiter: notifier stores
         * alive_count then loads alive_waiter; waiter stores
         * alive_waiter then loads alive_count.  A release/acq_rel RMW
         * on the two different objects is insufficient on ARM64 — the
         * store buffer can hide our alive_count=0 from the waiter's
         * load while also hiding the waiter's publish from our
         * exchange, yielding a lost wake (one parked fiber per
         * nesting level, which is exactly what
         * `stress/nested_nursery_deep.ccs` was hitting).  Full fence
         * on both sides forces at least one side to observe the
         * other.  Mirrors the same fix in `sched_v2.c` for the
         * `sched_v2_join` Dekker pair (search "Dekker pair with
         * sched_v2_join"). */
        atomic_thread_fence(memory_order_seq_cst);
        fiber_v2* waiter = atomic_exchange_explicit(&n->alive_waiter, NULL, memory_order_acq_rel);
        if (waiter) sched_v2_signal(waiter);
        wake_primitive_wake_all(&n->alive_wake);
    }
    /* Every other `prev`: not the closer. `n` must not be touched again —
     * another child may take the count to zero and free it any time. */
}

CCResult_void_CCError cc_nursery_wait_host(CCNurseryHost* n) {
    if (!n) return cc__nursery_errno(EINVAL, "cc_nursery_wait");
    {
        int expected = CC_NURSERY_LIVE;
        if (!atomic_compare_exchange_strong_explicit(&n->end_state, &expected,
                CC_NURSERY_JOINING, memory_order_acq_rel, memory_order_acquire)) {
            if (expected == CC_NURSERY_LEFT)
                cc_nursery_die("wait after leave");
        }
    }
    int first_err = 0;
    int timing = nursery_timing_enabled();
    uint64_t t0 = 0;
    uint64_t join_cycles = 0;
    uint64_t free_cycles = 0;
    uint64_t close_cycles = 0;
    size_t joined = 0;
    size_t closed = 0;
    if (timing) t0 = nursery_rdtsc();

    if (cc_nursery_worker_frees_mode()) {
        /* Barrier on alive_count.  Worker already pushed each fiber back
         * to the v2 pool on MCO_DEAD; we just wait for the last
         * notify_child_done to tick the counter to zero. */
        fiber_v2* self = sched_v2_current_fiber();
        if (self) {
            atomic_store_explicit(&n->alive_waiter, self, memory_order_release);
            /* Dekker pair with cc_nursery_notify_child_done: see the
             * companion comment there for the full story.  Without
             * this fence on ARM64, the store_release above and the
             * load_acquire below can be observed out-of-order by the
             * notifier, leaving us parked with no signal coming.
             * Stress: `tests/stress/nursery_worker_frees_race_stress_smoke.ccs`. */
            atomic_thread_fence(memory_order_seq_cst);
            /* Mask is hygiene: JOINING excludes LEFT, the bit cannot
             * be set here. */
            while ((atomic_load_explicit(&n->alive_count, memory_order_acquire) &
                    CC_NURSERY_ALIVE_COUNT_MASK) != 0) {
                sched_v2_set_park_reason("nursery_wait");
                sched_v2_park();
                sched_v2_set_park_reason(NULL);
            }
            fiber_v2* expected = self;
            (void)atomic_compare_exchange_strong_explicit(&n->alive_waiter, &expected, NULL,
                    memory_order_acq_rel, memory_order_relaxed);
        } else {
            for (;;) {
                uint32_t gen = atomic_load_explicit(&n->alive_wake.value, memory_order_acquire);
                if ((atomic_load_explicit(&n->alive_count, memory_order_acquire) &
                     CC_NURSERY_ALIVE_COUNT_MASK) == 0) break;
                wake_primitive_wait(&n->alive_wake, gen);
            }
        }
    } else {
        /* Classic path: spec says join children first, then close channels.
         * If cancelled, fibers should exit promptly when they check
         * cc_cancelled() or when channel ops return ECANCELED. */
        for (size_t i = 0; i < n->count; ++i) {
            if (n->tasks[i].kind == 0) continue;
            uint64_t step0 = timing ? nursery_rdtsc() : 0;
            int err = 0;
            if (n->tasks[i].kind == 2 && n->tasks[i].hybrid) {
                err = sched_v2_join(n->tasks[i].hybrid, NULL);
            }
            uint64_t step1 = timing ? nursery_rdtsc() : 0;
            if (first_err == 0 && err != 0) {
                first_err = err;
            }
            if (n->tasks[i].kind == 2 && n->tasks[i].hybrid) {
                sched_v2_fiber_release(n->tasks[i].hybrid);
            }
            uint64_t step2 = timing ? nursery_rdtsc() : 0;
            if (timing) {
                join_cycles += step1 - step0;
                free_cycles += step2 - step1;
            }
            memset(&n->tasks[i], 0, sizeof(n->tasks[i]));
            joined++;
        }
    }

    /* Close registered channels (hook is installed by add_closing_chan,
     * the only path that makes closing_count nonzero). */
    for (size_t i = 0; i < n->closing_count; ++i) {
        if (n->closing[i] && g_cc__nursery_chan_close) {
            uint64_t step0 = timing ? nursery_rdtsc() : 0;
            g_cc__nursery_chan_close(n->closing[i]);
            uint64_t step1 = timing ? nursery_rdtsc() : 0;
            if (timing) {
                close_cycles += step1 - step0;
            }
            closed++;
        }
    }
    n->count = 0;
    if (timing) {
        uint64_t t1 = nursery_rdtsc();
        atomic_fetch_add_explicit(&g_nursery_timing.wait_join_cycles, join_cycles, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.wait_free_cycles, free_cycles, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.wait_close_cycles, close_cycles, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.wait_total_cycles, t1 - t0, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.wait_calls, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.wait_tasks_joined, joined, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_nursery_timing.wait_channels_closed, closed, memory_order_relaxed);
    }
    return cc__nursery_errno(first_err, "cc_nursery_wait");
}

void cc_nursery_free_host(CCNurseryHost* n) {
    if (!n) return;
    /* Owner walk and a local @destroy may both fire. Second call is a no-op. */
    if (n->owner_placed && !n->arena.a) return;
    if (atomic_load_explicit(&n->end_state, memory_order_acquire) ==
            CC_NURSERY_LEFT)
        cc_nursery_die("free after leave");
    if (!cc_nursery_worker_frees_mode()) {
        /* Classic: tasks[] still owns fiber_v2 references until free. */
        for (size_t i = 0; i < n->count; ++i) {
            if (n->tasks[i].kind == 2 && n->tasks[i].hybrid) {
                sched_v2_fiber_release(n->tasks[i].hybrid);
            }
        }
    }
    /* Worker-frees mode: tasks[] entries are stale pointers; the worker
     * already returned each fiber to the v2 pool on MCO_DEAD. */
    cc_nursery_close_registered(n);
    cc_nursery_release(n);
}

CCResult_void_CCError cc_nursery_register_leftover_host(CCNurseryHost* n, void* ctx, void (*finish)(void*)) {
    if (!n || !finish)
        return cc__nursery_errno(EINVAL, "cc_nursery_register_leftover");
    pthread_mutex_lock(&n->mu);
    if (atomic_load_explicit(&n->end_state, memory_order_acquire) !=
            CC_NURSERY_LIVE) {
        pthread_mutex_unlock(&n->mu);
        return cc__nursery_errno(EINVAL, "cc_nursery_register_leftover");
    }
    if (n->leftover_fn) {
        pthread_mutex_unlock(&n->mu);
        return cc__nursery_errno(EINVAL, "cc_nursery_register_leftover");
    }
    n->leftover_fn = finish;
    n->leftover_ctx = ctx;
    pthread_mutex_unlock(&n->mu);
    return cc_ok_CCResult_void_CCError();
}

/* OPEN → LEFT. From the caller's side the handle is CONSUMED: the last child
 * to exit (or this call, if none are live) reaches EMPTY then DEAD on a
 * worker at an unpredictable time. Calling anything on `n` after leave is a
 * use-after-free unless the caller owns independent proof that a child is
 * still live. A leave-capable nursery must never have a lifetime parent. */
void cc_nursery_leave_host(CCNurseryHost* n) {
    int expected;
    size_t prev;
    if (!n) return;
    if (n->owner_placed)
        cc_nursery_die("leave on an owner-attached nursery "
                       "(cc_arena_create_nursery); use cc_nursery_create()");
    if (!cc_nursery_worker_frees_mode())
        cc_nursery_die("leave requires worker-frees mode");
    pthread_mutex_lock(&n->mu);
    expected = CC_NURSERY_LIVE;
    if (!atomic_compare_exchange_strong_explicit(&n->end_state, &expected,
            CC_NURSERY_LEFT, memory_order_acq_rel, memory_order_acquire)) {
        pthread_mutex_unlock(&n->mu);
        if (expected == CC_NURSERY_JOINING)
            cc_nursery_die("leave while wait in progress");
        cc_nursery_die("double leave");
    }
    /* Single-word arbitration (see the protocol comment on the bit).
     * Under mu so no spawn increment can interleave: spawn's not-LEFT
     * check and its increment share this mutex, so a zero count observed by
     * this RMW is final — no child exists and none can appear. */
    prev = atomic_fetch_or_explicit(&n->alive_count,
                                    CC_NURSERY_ALIVE_LEFT_BIT,
                                    memory_order_acq_rel);
    pthread_mutex_unlock(&n->mu);
    if ((prev & CC_NURSERY_ALIVE_COUNT_MASK) == 0)
        cc_nursery_at_empty(n);
}

CCResult_void_CCError cc_nursery_add_closing_chan_host(CCNurseryHost* n, CCChan* ch) {
    if (!n || !ch)
        return cc__nursery_errno(EINVAL, "cc_nursery_add_closing_chan");
    pthread_mutex_lock(&n->mu);
    if (atomic_load_explicit(&n->end_state, memory_order_acquire) ==
            CC_NURSERY_LEFT) {
        pthread_mutex_unlock(&n->mu);
        return cc__nursery_errno(EINVAL, "cc_nursery_add_closing_chan");
    }
    if (n->closing_count == n->closing_cap) {
        /* Arena-backed like tasks[]: alloc-and-copy, old array is arena
         * garbage until release. Slots >= closing_count are never read. */
        size_t new_cap = n->closing_cap ? n->closing_cap * 2 : 4;
        CCChan** nc = (CCChan**)cc_arena_alloc(
            n->arena.a, new_cap * sizeof(CCChan*), _Alignof(CCChan*));
        if (!nc) {
            pthread_mutex_unlock(&n->mu);
            return cc__nursery_errno(ENOMEM, "cc_nursery_add_closing_chan");
        }
        if (n->closing_count)
            memcpy(nc, n->closing, n->closing_count * sizeof(CCChan*));
        n->closing = nc;
        n->closing_cap = new_cap;
    }
    /* Install the close hook before publishing the count (see decl above). */
    g_cc__nursery_chan_close = cc_chan_close;
    n->closing[n->closing_count++] = ch;
    pthread_mutex_unlock(&n->mu);
    /* Mark channel with its autoclose owner for optional runtime guard. */
    cc__chan_set_autoclose_owner(ch, n);
    return cc_ok_CCResult_void_CCError();
}
