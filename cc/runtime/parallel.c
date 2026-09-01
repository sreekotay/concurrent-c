#include <ccc/cc_parallel.cch>
#include <stdlib.h>
#include "sched_v2.h"

fiber_v2* cc_task_fiber_v2(CCTask t);

void cc_parallel_attach(CCParallel* h, CCTask t) {
    fiber_v2* f = cc_task_fiber_v2(t);
    if (f && h)
        sched_v2_fiber_set_par_gate(f, h);
}

void cc_parallel_wake_attached(CCParallel* h) {
    int i;
    if (!h)
        return;
    for (i = 0; i < h->nt; i++) {
        fiber_v2* f = cc_task_fiber_v2(h->tasks[i]);
        if (f)
            sched_v2_signal(f);
    }
}

int cc_parallel_current_cancelled(void) {
    fiber_v2* f = sched_v2_current_fiber();
    CCParallel* h;
    if (!f)
        return 0;
    h = (CCParallel*)sched_v2_fiber_par_gate(f);
    if (!h)
        return 0;
    return cc_atomic_load(&h->cancelled) != 0;
}

CCResult_void_CCError cc_parallel_wait(CCParallel* h) {
    int i;
    if (!h)
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "cc_parallel_wait"));
    if (cc_atomic_load(&h->joined))
        return cc_ok_CCResult_void_CCError();
    if (h->n) {
        CCResult_void_CCError wr = cc_nursery_wait_host(h->n);
        if (!wr.ok) return wr;
    }
    for (i = 0; i < h->nt; i++) {
        cc_parallel_join(h->tasks[i]);
        free(h->envs[i]);
        h->envs[i] = NULL;
    }
    h->nt = 0;
    cc_atomic_store(&h->joined, 1);
    if (h->fail)
        return cc_err_CCResult_void_CCError(h->err);
    return cc_ok_CCResult_void_CCError();
}
