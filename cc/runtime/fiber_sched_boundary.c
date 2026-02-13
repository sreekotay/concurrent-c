#include "fiber_sched_boundary.h"
#include "fiber_internal.h"

#if CC_RUNTIME_V3
CCSchedFiber* cc_sched_v3_worker_next_impl(void);
CCSchedFiber* cc_sched_v3_idle_probe_impl(void);
#endif

/*
 * V2 compatibility shim for the v3 scheduler boundary.
 * This keeps a stable internal contract while the concrete scheduler
 * implementation remains the existing fiber scheduler.
 */

void cc_sched_schedule(CCSchedFiber* fiber) {
    cc__fiber_sched_enqueue(fiber);
}

CCSchedFiber* cc_sched_worker_next(void) {
#if CC_RUNTIME_V3
    return cc_sched_v3_worker_next_impl();
#endif
    return NULL;
}

CCSchedFiber* cc_sched_worker_idle_probe(void) {
#if CC_RUNTIME_V3
    return cc_sched_v3_idle_probe_impl();
#endif
    return NULL;
}

cc_sched_wait_result cc_sched_fiber_wait(
    void* waitable,
    void* io,
    const cc_sched_waitable_ops* ops
) {
    CCSchedFiber* fiber = (CCSchedFiber*)cc__fiber_current();
    if (!ops) {
        return CC_SCHED_WAIT_CLOSED;
    }
    if (ops->try_complete && ops->try_complete(waitable, fiber, io)) {
        return CC_SCHED_WAIT_OK;
    }
    if (!ops->publish) {
        return CC_SCHED_WAIT_CLOSED;
    }
    if (!ops->publish(waitable, fiber, io)) {
        return CC_SCHED_WAIT_CLOSED;
    }
    if (ops->try_complete && ops->try_complete(waitable, fiber, io)) {
        if (ops->unpublish) {
            ops->unpublish(waitable, fiber);
        }
        return CC_SCHED_WAIT_OK;
    }
    if (!cc__fiber_in_context()) {
        if (ops->unpublish) {
            ops->unpublish(waitable, fiber);
        }
        return CC_SCHED_WAIT_CLOSED;
    }

    /*
     * The v2 scheduler park/unpark path is used as the temporary backend.
     * When the fiber resumes, completion/wake happened through normal wake flow.
     */
    cc__fiber_park_reason("cc_sched_fiber_wait", __FILE__, __LINE__);
    if (ops->try_complete && ops->try_complete(waitable, fiber, io)) {
        return CC_SCHED_WAIT_OK;
    }
    return CC_SCHED_WAIT_PARKED;
}

void cc_sched_fiber_wake(CCSchedFiber* fiber) {
    cc__fiber_unpark(fiber);
}
