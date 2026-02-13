#include "fiber_sched_boundary.h"
#include "fiber_internal.h"

#if CC_RUNTIME_V3
CCSchedFiber* cc_sched_v3_worker_next_impl(void);
CCSchedFiber* cc_sched_v3_idle_probe_impl(void);
#endif

typedef enum {
    CC_WAIT_STAGE_RUNNING = 0,
    CC_WAIT_STAGE_PARKING_PUBLISHED = 1,
    CC_WAIT_STAGE_PARKED = 2,
} cc_wait_stage;

/*
 * V2 compatibility shim for the v3 scheduler boundary.
 * This keeps a stable internal contract while the concrete scheduler
 * implementation remains the existing fiber scheduler.
 */

void cc_sched_schedule(CCSchedFiber* fiber) {
    /* LP (§10 Enqueue RUNNABLE): queue publication of runnable visibility. */
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
    cc_wait_stage stage = CC_WAIT_STAGE_RUNNING;
    if (!ops) {
        return CC_SCHED_WAIT_CLOSED;
    }
    /* Stage RUNNING: optimistic completion while still owning execution. */
    if (ops->try_complete && ops->try_complete(waitable, fiber, io)) {
        return CC_SCHED_WAIT_OK;
    }
    if (!ops->publish) {
        return CC_SCHED_WAIT_CLOSED;
    }
    /* LP (§10 Waiter publish LP): waiter becomes discoverable to wakers. */
    if (!ops->publish(waitable, fiber, io)) {
        return CC_SCHED_WAIT_CLOSED;
    }
    stage = CC_WAIT_STAGE_PARKING_PUBLISHED;
    /* Stage PARKING_PUBLISHED: once published, wake ownership may race us. */
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

    /* Final pre-park completion check before committing to a park. */
    if (ops->try_complete && ops->try_complete(waitable, fiber, io)) {
        if (ops->unpublish) {
            ops->unpublish(waitable, fiber);
        }
        return CC_SCHED_WAIT_OK;
    }
    /*
     * The v2 scheduler park/unpark path is used as the temporary backend.
     * Waitables may override the park primitive to preserve flag-guarded
     * semantics (e.g. CC_FIBER_PARK_IF on a waiter notification flag).
     */
    /* LP (§10 Commit PARKED path): park primitive hands off to scheduler-owned
     * RUNNING->PARKING->PARKED substrate (or waitable-specific guarded park). */
    if (ops->park) {
        ops->park(waitable, fiber, io);
    } else {
        cc__fiber_park_reason("cc_sched_fiber_wait", __FILE__, __LINE__);
    }
    stage = CC_WAIT_STAGE_PARKED;
    /*
     * Stage PARKED return path: post-park try_complete models wake_pending
     * recovery where a wake raced the park commit.
     */
    if (ops->try_complete && ops->try_complete(waitable, fiber, io)) {
        return CC_SCHED_WAIT_OK;
    }
    (void)stage;
    return CC_SCHED_WAIT_PARKED;
}

void cc_sched_fiber_wake(CCSchedFiber* fiber) {
    /* LP (§10 Waker claim + wake enqueue): delegated to scheduler wake path. */
    cc__fiber_unpark(fiber);
}
