/*
 * Runtime deadlock detection for Concurrent-C.
 *
 * Strategy:
 * 1. Track how many threads are blocked on channel ops or cc_block_on
 * 2. Track a progress counter that gets bumped on any successful operation
 * 3. A watchdog thread periodically checks:
 *    - If blocked count > 0 AND progress hasn't changed
 *    - If this persists for N seconds, likely deadlock
 *
 * Environment variables:
 *   CC_DEADLOCK_DETECT=1  Enable legacy deadlock detection (default: disabled)
 *   CC_DEADLOCK_ABORT=0   Disable abort, just warn (for debugging)
 *   CC_DEADLOCK_TIMEOUT=N Set timeout in seconds (default: 10)
 *
 * NOTE: This legacy detector is disabled by default. The task scheduler
 * has integrated deadlock detection that works better with the fiber model.
 */

#include <ccc/cc_deadlock_detect.cch>
#include <ccc/cc_sched.cch>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Configuration */
#define WATCHDOG_CHECK_INTERVAL_MS 500
#define DEFAULT_DEADLOCK_TIMEOUT_SEC 10

/* Global state */
static atomic_int g_blocked_count = 0;
static atomic_int g_progress_counter = 0;
static atomic_int g_enabled = 0;
static atomic_int g_abort_on_deadlock = 1;  /* Default: abort on deadlock */
static atomic_int g_deadlock_timeout_ms = DEFAULT_DEADLOCK_TIMEOUT_SEC * 1000;
static atomic_int g_watchdog_running = 0;
static pthread_t g_watchdog_thread;
static pthread_mutex_t g_init_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_watchdog_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_watchdog_cv = PTHREAD_COND_INITIALIZER;

/* Thread-local block reason for diagnostics */
static __thread CCBlockReason tls_block_reason = CC_BLOCK_NONE;
static __thread int tls_is_blocking = 0;

/* Block reason tracking for diagnostics */
#define MAX_TRACKED_THREADS 64
static struct {
    pthread_t tid;
    CCBlockReason reason;
    int active;
} g_block_info[MAX_TRACKED_THREADS];
static pthread_mutex_t g_block_info_mu = PTHREAD_MUTEX_INITIALIZER;

static void record_block_reason(CCBlockReason reason) {
    pthread_t self = pthread_self();
    pthread_mutex_lock(&g_block_info_mu);
    
    /* Find existing or empty slot */
    int empty_slot = -1;
    for (int i = 0; i < MAX_TRACKED_THREADS; i++) {
        if (g_block_info[i].active && pthread_equal(g_block_info[i].tid, self)) {
            g_block_info[i].reason = reason;
            pthread_mutex_unlock(&g_block_info_mu);
            return;
        }
        if (empty_slot < 0 && !g_block_info[i].active) {
            empty_slot = i;
        }
    }
    
    if (empty_slot >= 0) {
        g_block_info[empty_slot].tid = self;
        g_block_info[empty_slot].reason = reason;
        g_block_info[empty_slot].active = 1;
    }
    
    pthread_mutex_unlock(&g_block_info_mu);
}

static void clear_block_reason(void) {
    pthread_t self = pthread_self();
    pthread_mutex_lock(&g_block_info_mu);
    
    for (int i = 0; i < MAX_TRACKED_THREADS; i++) {
        if (g_block_info[i].active && pthread_equal(g_block_info[i].tid, self)) {
            g_block_info[i].active = 0;
            break;
        }
    }
    
    pthread_mutex_unlock(&g_block_info_mu);
}

static const char* block_reason_str(CCBlockReason r) {
    switch (r) {
        case CC_BLOCK_NONE: return "none";
        case CC_BLOCK_CHAN_SEND: return "chan_send (channel full, waiting for receiver)";
        case CC_BLOCK_CHAN_RECV: return "chan_recv (channel empty, waiting for sender)";
        case CC_BLOCK_ON_TASK: return "cc_block_on (waiting for async task)";
        case CC_BLOCK_MUTEX: return "mutex";
        default: return "unknown";
    }
}

static void dump_blocked_threads(void) {
    pthread_mutex_lock(&g_block_info_mu);
    
    int count = 0;
    for (int i = 0; i < MAX_TRACKED_THREADS; i++) {
        if (g_block_info[i].active) {
            fprintf(stderr, "  Thread %d: blocked on %s\n", 
                    count++, block_reason_str(g_block_info[i].reason));
        }
    }
    
    if (count == 0) {
        fprintf(stderr, "  (no blocked threads recorded)\n");
    }
    
    pthread_mutex_unlock(&g_block_info_mu);
}

static void* watchdog_thread_fn(void* arg) {
    (void)arg;
    
    int last_progress = atomic_load(&g_progress_counter);
    int stable_blocked_ms = 0;
    int warned = 0;
    
    while (atomic_load(&g_watchdog_running)) {
        pthread_mutex_lock(&g_watchdog_mu);
        struct timespec wait_ts;
        clock_gettime(CLOCK_REALTIME, &wait_ts);
        wait_ts.tv_nsec += WATCHDOG_CHECK_INTERVAL_MS * 1000000L;
        if (wait_ts.tv_nsec >= 1000000000L) {
            wait_ts.tv_sec += wait_ts.tv_nsec / 1000000000L;
            wait_ts.tv_nsec %= 1000000000L;
        }
        (void)pthread_cond_timedwait(&g_watchdog_cv, &g_watchdog_mu, &wait_ts);
        pthread_mutex_unlock(&g_watchdog_mu);
        
        int blocked = atomic_load(&g_blocked_count);
        int progress = atomic_load(&g_progress_counter);
        
        if (blocked > 0 && progress == last_progress) {
            /* No progress and threads are blocked */
            stable_blocked_ms += WATCHDOG_CHECK_INTERVAL_MS;
            int timeout_ms = atomic_load(&g_deadlock_timeout_ms);
            
            if (stable_blocked_ms >= timeout_ms && !warned) {
                fprintf(stderr, "\n");
                fprintf(stderr, "╔══════════════════════════════════════════════════════════════════╗\n");
                fprintf(stderr, "║                    🔒 DEADLOCK DETECTED 🔒                       ║\n");
                fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
                fprintf(stderr, "║ %d thread(s) blocked for %.1f+ seconds with NO progress.        ║\n",
                        blocked, timeout_ms / 1000.0);
                fprintf(stderr, "║ This is a REAL deadlock - all workers are waiting indefinitely. ║\n");
                fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
                fprintf(stderr, "║ Blocked threads:                                                 ║\n");
                fprintf(stderr, "╚══════════════════════════════════════════════════════════════════╝\n");
                dump_blocked_threads();
                fprintf(stderr, "\n");
                fprintf(stderr, "Common deadlock patterns:\n");
                fprintf(stderr, "  1. @nursery closing(ch) with recv-until-close INSIDE the nursery\n");
                fprintf(stderr, "     Fix: Move consumer OUTSIDE the nursery\n");
                fprintf(stderr, "  2. cc_block_on(async_task) where task waits on channel peers\n");
                fprintf(stderr, "     Fix: Use cc_block_all() to run producer+consumer together\n");
                fprintf(stderr, "  3. Circular channel dependencies (A waits on B, B waits on A)\n");
                fprintf(stderr, "     Fix: Break the cycle with buffered channels or reordering\n");
                fprintf(stderr, "\n");
                
                if (atomic_load(&g_abort_on_deadlock)) {
                    fprintf(stderr, "Exiting with code 124 (set CC_DEADLOCK_ABORT=0 to continue).\n");
                    _exit(124);  /* Exit code 124 = timeout-style exit */
                }
                
                warned = 1;  /* Only warn once per deadlock episode */
            }
        } else {
            /* Progress was made, reset */
            stable_blocked_ms = 0;
            warned = 0;
        }
        
        last_progress = progress;
    }
    
    return NULL;
}

void cc_deadlock_detect_init(void) {
    pthread_mutex_lock(&g_init_mutex);
    
    if (atomic_load(&g_watchdog_running)) {
        pthread_mutex_unlock(&g_init_mutex);
        return;  /* Already initialized */
    }
    
    /* Disabled by default; allow opt-in with CC_DEADLOCK_DETECT=1 */
    const char* env = getenv("CC_DEADLOCK_DETECT");
    if (!env || env[0] != '1') {
        pthread_mutex_unlock(&g_init_mutex);
        return;  /* Disabled (default) */
    }
    
    /* Check if abort should be disabled (default is abort=1) */
    const char* abort_env = getenv("CC_DEADLOCK_ABORT");
    if (abort_env && abort_env[0] == '0') {
        atomic_store(&g_abort_on_deadlock, 0);  /* Just warn, don't abort */
    }
    
    /* Check for custom timeout */
    const char* timeout_env = getenv("CC_DEADLOCK_TIMEOUT");
    if (timeout_env) {
        int timeout_sec = atoi(timeout_env);
        if (timeout_sec > 0) {
            atomic_store(&g_deadlock_timeout_ms, timeout_sec * 1000);
        }
    }
    
    atomic_store(&g_enabled, 1);
    atomic_store(&g_watchdog_running, 1);
    
    /* Clear block info */
    memset(g_block_info, 0, sizeof(g_block_info));
    
    /* Start watchdog thread */
    int err = pthread_create(&g_watchdog_thread, NULL, watchdog_thread_fn, NULL);
    if (err != 0) {
        fprintf(stderr, "CC: failed to start deadlock watchdog thread\n");
        atomic_store(&g_watchdog_running, 0);
    }
    
    pthread_mutex_unlock(&g_init_mutex);
}

void cc_deadlock_detect_shutdown(void) {
    if (!atomic_load(&g_watchdog_running)) return;
    
    atomic_store(&g_watchdog_running, 0);
    pthread_cond_broadcast(&g_watchdog_cv);
    pthread_join(g_watchdog_thread, NULL);
}

void cc_deadlock_enter_blocking(CCBlockReason reason) {
    if (!atomic_load(&g_enabled)) return;
    
    if (!tls_is_blocking) {
        tls_is_blocking = 1;
        tls_block_reason = reason;
        atomic_fetch_add(&g_blocked_count, 1);
        record_block_reason(reason);
        pthread_cond_broadcast(&g_watchdog_cv);
    }
}

void cc_deadlock_exit_blocking(void) {
    if (!atomic_load(&g_enabled)) return;
    
    if (tls_is_blocking) {
        tls_is_blocking = 0;
        tls_block_reason = CC_BLOCK_NONE;
        atomic_fetch_sub(&g_blocked_count, 1);
        clear_block_reason();
        pthread_cond_broadcast(&g_watchdog_cv);
    }
}

void cc_deadlock_progress(void) {
    if (!atomic_load(&g_enabled)) return;
    atomic_fetch_add(&g_progress_counter, 1);
    pthread_cond_broadcast(&g_watchdog_cv);
}

int cc_deadlock_get_blocked_count(void) {
    return atomic_load(&g_blocked_count);
}

int cc_deadlock_is_enabled(void) {
    return atomic_load(&g_enabled);
}
