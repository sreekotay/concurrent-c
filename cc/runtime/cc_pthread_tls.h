/*
 * Pthread-backed TLS for host TCC (__TINYC__).
 *
 * Vendored TCC rejects __thread / _Thread_local. When the runtime is compiled
 * with CC=tcc, include this header once (via concurrent_c.c) before the
 * aggregated runtime .c files. Call sites `#define` former __thread names as
 * lvalues into this per-thread bundle (GCC/Clang keep native __thread).
 */
#ifndef CC_PTHREAD_TLS_H
#define CC_PTHREAD_TLS_H

#if defined(__TINYC__)

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include <ccc/cc_sched.cch> /* CCParTls */

#ifndef CC_TLS_WAKE_BATCH_SIZE
#define CC_TLS_WAKE_BATCH_SIZE 32
#endif

typedef struct cc_rt_tls_wake_batch {
    void* fibers[CC_TLS_WAKE_BATCH_SIZE];
    uint32_t attribs[CC_TLS_WAKE_BATCH_SIZE];
    size_t count;
} cc_rt_tls_wake_batch;

typedef struct cc_rt_tls {
    void* current_nursery;   /* CCNurseryHost* */
    void* current_deadline;  /* CCDeadline* */
    int v2_thread_id;
    uint64_t v2_my_generation;
    void* v2_current_fiber;  /* fiber_v2* */
    uint64_t v2_dispatch_seq;
    unsigned deadlock_suppress_depth;
    unsigned external_wait_depth;
    cc_rt_tls_wake_batch wake_batch;
    char task_v2_result[48];
    void* excl_ring;         /* cc__excl_evt_ring_t* */
    void* env_pools;         /* closure.c CCEnvPools* (heap, process-lived) */
    CCParTls par;            /* @parallel gate block (cc__par_tls) */
    void* par_site_fn;
    void* par_site;          /* cc_par_site* */
} cc_rt_tls;

static pthread_key_t cc_rt_tls_key;
static pthread_once_t cc_rt_tls_once = PTHREAD_ONCE_INIT;

static void cc_rt_tls_key_init(void) {
    (void)pthread_key_create(&cc_rt_tls_key, free);
}

static cc_rt_tls* cc_rt_tls_get(void) {
    cc_rt_tls* t;
    (void)pthread_once(&cc_rt_tls_once, cc_rt_tls_key_init);
    t = (cc_rt_tls*)pthread_getspecific(cc_rt_tls_key);
    if (!t) {
        t = (cc_rt_tls*)calloc(1, sizeof(*t));
        if (!t) return NULL;
        t->v2_thread_id = -1; /* match former static __thread init */
        (void)pthread_setspecific(cc_rt_tls_key, t);
    }
    return t;
}

#endif /* __TINYC__ */

#endif /* CC_PTHREAD_TLS_H */
