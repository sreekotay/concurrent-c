/*
 * Scheduler v3 compilation unit scaffold.
 *
 * For now, we keep baseline scheduler behavior by including fiber_sched.c, but
 * v3-only seam functions live in this file so boundary dispatch can evolve
 * without touching call sites.
 */

#include "fiber_sched_boundary.h"
#include "fiber_sched.c"

static __thread uint64_t cc_sched_v3_tls_rng_state = 0;
static _Atomic int g_cc_sched_v3_stats_mode = -1;
static _Atomic int g_cc_sched_v3_stats_atexit = 0;
static _Atomic int g_cc_sched_v3_dump_mode = -1;
static _Atomic uint64_t g_cc_sched_v3_next_calls = 0;
static _Atomic uint64_t g_cc_sched_v3_idle_calls = 0;
static _Atomic uint64_t g_cc_sched_v3_src_local = 0;
static _Atomic uint64_t g_cc_sched_v3_src_inbox = 0;
static _Atomic uint64_t g_cc_sched_v3_src_global = 0;
static _Atomic uint64_t g_cc_sched_v3_src_steal = 0;
static _Atomic uint64_t g_cc_sched_v3_src_empty = 0;
static _Atomic uint64_t g_cc_sched_v3_prefetch_local = 0;
static _Atomic uint64_t g_cc_sched_v3_prefetch_steal_local = 0;

#define CC_SCHED_V3_GLOBAL_PREFETCH 4
#define CC_SCHED_V3_STEAL_PREFETCH 4

static int cc_sched_v3_stats_enabled(void) {
    int mode = atomic_load_explicit(&g_cc_sched_v3_stats_mode, memory_order_acquire);
    if (mode >= 0) return mode;
    mode = (getenv("CC_V3_SCHED_STATS") || getenv("CC_V3_SCHED_STATS_DUMP")) ? 1 : 0;
    int expected = -1;
    (void)atomic_compare_exchange_strong_explicit(&g_cc_sched_v3_stats_mode,
                                                  &expected,
                                                  mode,
                                                  memory_order_release,
                                                  memory_order_acquire);
    return atomic_load_explicit(&g_cc_sched_v3_stats_mode, memory_order_acquire);
}

static int cc_sched_v3_dump_enabled(void) {
    int mode = atomic_load_explicit(&g_cc_sched_v3_dump_mode, memory_order_acquire);
    if (mode >= 0) return mode;
    mode = getenv("CC_V3_SCHED_STATS_DUMP") ? 1 : 0;
    int expected = -1;
    (void)atomic_compare_exchange_strong_explicit(&g_cc_sched_v3_dump_mode,
                                                  &expected,
                                                  mode,
                                                  memory_order_release,
                                                  memory_order_acquire);
    return atomic_load_explicit(&g_cc_sched_v3_dump_mode, memory_order_acquire);
}

static void cc_sched_v3_dump_stats(void) {
    uint64_t next_calls = atomic_load_explicit(&g_cc_sched_v3_next_calls, memory_order_relaxed);
    uint64_t idle_calls = atomic_load_explicit(&g_cc_sched_v3_idle_calls, memory_order_relaxed);
    uint64_t src_local = atomic_load_explicit(&g_cc_sched_v3_src_local, memory_order_relaxed);
    uint64_t src_inbox = atomic_load_explicit(&g_cc_sched_v3_src_inbox, memory_order_relaxed);
    uint64_t src_global = atomic_load_explicit(&g_cc_sched_v3_src_global, memory_order_relaxed);
    uint64_t src_steal = atomic_load_explicit(&g_cc_sched_v3_src_steal, memory_order_relaxed);
    uint64_t src_empty = atomic_load_explicit(&g_cc_sched_v3_src_empty, memory_order_relaxed);
    uint64_t prefetch_local = atomic_load_explicit(&g_cc_sched_v3_prefetch_local, memory_order_relaxed);
    uint64_t prefetch_steal_local = atomic_load_explicit(&g_cc_sched_v3_prefetch_steal_local, memory_order_relaxed);
    uint64_t total = src_local + src_inbox + src_global + src_steal + src_empty;
    if (total == 0) return;
    fprintf(stderr,
            "\n=== V3 SCHED STATS ===\n"
            "next_calls=%llu idle_calls=%llu\n"
            "source local=%llu inbox=%llu global=%llu steal=%llu empty=%llu\n"
            "prefetch_from_global_to_local=%llu\n"
            "prefetch_from_steal_to_local=%llu\n"
            "source_pct local=%.1f inbox=%.1f global=%.1f steal=%.1f empty=%.1f\n"
            "======================\n\n",
            (unsigned long long)next_calls,
            (unsigned long long)idle_calls,
            (unsigned long long)src_local,
            (unsigned long long)src_inbox,
            (unsigned long long)src_global,
            (unsigned long long)src_steal,
            (unsigned long long)src_empty,
            (unsigned long long)prefetch_local,
            (unsigned long long)prefetch_steal_local,
            total ? (100.0 * (double)src_local / (double)total) : 0.0,
            total ? (100.0 * (double)src_inbox / (double)total) : 0.0,
            total ? (100.0 * (double)src_global / (double)total) : 0.0,
            total ? (100.0 * (double)src_steal / (double)total) : 0.0,
            total ? (100.0 * (double)src_empty / (double)total) : 0.0);
}

static inline void cc_sched_v3_stats_maybe_init(void) {
    if (!cc_sched_v3_stats_enabled() || !cc_sched_v3_dump_enabled()) return;
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_cc_sched_v3_stats_atexit,
                                                &expected,
                                                1,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atexit(cc_sched_v3_dump_stats);
    }
}

CCSchedFiber* cc_sched_v3_worker_next_impl(void) {
    cc_sched_v3_stats_maybe_init();
    if (cc_sched_v3_stats_enabled()) {
        atomic_fetch_add_explicit(&g_cc_sched_v3_next_calls, 1, memory_order_relaxed);
    }
    int worker_id = tls_worker_id;
    if (worker_id >= 0 && (size_t)worker_id < g_sched.num_workers) {
        local_queue* my_queue = &g_sched.local_queues[worker_id];

        fiber_task* f = lq_pop(my_queue);
        if (f) {
            if (cc_sched_v3_stats_enabled()) {
                atomic_fetch_add_explicit(&g_cc_sched_v3_src_local, 1, memory_order_relaxed);
            }
            return (CCSchedFiber*)f;
        }

        f = iq_pop(&g_sched.inbox_queues[worker_id]);
        if (f) {
            if (cc_sched_v3_stats_enabled()) {
                atomic_fetch_add_explicit(&g_cc_sched_v3_src_inbox, 1, memory_order_relaxed);
            }
            return (CCSchedFiber*)f;
        }
    }

    if (g_sched.run_queue) {
        fiber_task* f = fq_pop(g_sched.run_queue);
        if (f) {
            if (cc_sched_v3_stats_enabled()) {
                atomic_fetch_add_explicit(&g_cc_sched_v3_src_global, 1, memory_order_relaxed);
            }
            if (worker_id >= 0 && (size_t)worker_id < g_sched.num_workers) {
                local_queue* my_queue = &g_sched.local_queues[worker_id];
                for (int i = 0; i < CC_SCHED_V3_GLOBAL_PREFETCH; i++) {
                    fiber_task* extra = fq_pop(g_sched.run_queue);
                    if (!extra) break;
                    if (lq_push(my_queue, extra) != 0) {
                        fq_push_blocking(g_sched.run_queue, extra);
                        break;
                    }
                    if (cc_sched_v3_stats_enabled()) {
                        atomic_fetch_add_explicit(&g_cc_sched_v3_prefetch_local, 1, memory_order_relaxed);
                    }
                }
            }
            return (CCSchedFiber*)f;
        }
    }

    if (worker_id >= 0 && (size_t)worker_id < g_sched.num_workers && g_sched.num_workers > 1) {
        if (cc_sched_v3_tls_rng_state == 0) {
            cc_sched_v3_tls_rng_state = (uint64_t)worker_id * 0x9E3779B97F4A7C15ULL + rdtsc();
        }
        local_queue* my_queue = &g_sched.local_queues[worker_id];
        fiber_task* steal_buf[CC_SCHED_V3_STEAL_PREFETCH];
        size_t victim = (size_t)(xorshift64(&cc_sched_v3_tls_rng_state) % g_sched.num_workers);
        if ((int)victim == worker_id) {
            victim = (victim + 1) % g_sched.num_workers;
        }

        size_t stolen = lq_steal_batch(&g_sched.local_queues[victim],
                                       steal_buf,
                                       CC_SCHED_V3_STEAL_PREFETCH);
        if (stolen > 0) {
            for (size_t i = 1; i < stolen; i++) {
                if (lq_push(my_queue, steal_buf[i]) != 0) {
                    fq_push_blocking(g_sched.run_queue, steal_buf[i]);
                } else if (cc_sched_v3_stats_enabled()) {
                    atomic_fetch_add_explicit(&g_cc_sched_v3_prefetch_steal_local, 1, memory_order_relaxed);
                }
            }
            if (cc_sched_v3_stats_enabled()) {
                atomic_fetch_add_explicit(&g_cc_sched_v3_src_steal, 1, memory_order_relaxed);
            }
            return (CCSchedFiber*)steal_buf[0];
        }
    }

    if (cc_sched_v3_stats_enabled()) {
        atomic_fetch_add_explicit(&g_cc_sched_v3_src_empty, 1, memory_order_relaxed);
    }
    return NULL;
}

CCSchedFiber* cc_sched_v3_idle_probe_impl(void) {
    /*
     * Phase 2 seam: idle transition probe currently reuses the same
     * acquisition order. Keeping this separate allows future v3-specific
     * idle policy without touching worker_main.
     */
    cc_sched_v3_stats_maybe_init();
    if (cc_sched_v3_stats_enabled()) {
        atomic_fetch_add_explicit(&g_cc_sched_v3_idle_calls, 1, memory_order_relaxed);
    }
    return cc_sched_v3_worker_next_impl();
}
