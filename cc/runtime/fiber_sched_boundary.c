#include "fiber_sched_boundary.h"
#include "fiber_internal.h"

CCSchedFiber* cc_sched_v3_worker_next_impl(void);
CCSchedFiber* cc_sched_v3_idle_probe_impl(void);

/*
 * V2 compatibility shim for the v3 scheduler boundary.
 * This keeps a stable internal contract while the concrete scheduler
 * implementation remains the existing fiber scheduler.
 */

CCSchedFiber* cc_sched_current(void) {
    return (CCSchedFiber*)cc__fiber_current();
}

void cc_sched_schedule(CCSchedFiber* fiber) {
    /* LP (§10 Enqueue RUNNABLE): queue publication of runnable visibility. */
    cc__fiber_sched_enqueue(fiber);
}

CCSchedFiber* cc_sched_worker_next(void) {
    return cc_sched_v3_worker_next_impl();
}

CCSchedFiber* cc_sched_worker_idle_probe(void) {
    return cc_sched_v3_idle_probe_impl();
}

static void cc_sched_wait_many_unpublish(cc_sched_wait_case* cases,
                                         size_t count,
                                         CCSchedFiber* fiber) {
    if (!cases) return;
    for (size_t i = 0; i < count; ++i) {
        const cc_sched_waitable_ops* ops = cases[i].ops;
        if (ops && ops->unpublish) {
            ops->unpublish(cases[i].waitable, fiber);
        }
    }
}

static cc_sched_wait_result cc_sched_fiber_wait_impl(
    void* waitable,
    void* io,
    const cc_sched_waitable_ops* ops,
    const struct timespec* abs_deadline
) {
    CCSchedFiber* fiber = (CCSchedFiber*)cc__fiber_current();
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
    /* Stage PARKING_PUBLISHED: once published, wake ownership may race us. */
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
    if (abs_deadline) {
        if (ops->park_until) {
            cc_sched_wait_result park_rc = ops->park_until(waitable, fiber, io, abs_deadline);
            if (park_rc == CC_SCHED_WAIT_TIMEOUT || park_rc == CC_SCHED_WAIT_CANCELLED ||
                park_rc == CC_SCHED_WAIT_CLOSED) {
                return park_rc;
            }
        } else {
            return CC_SCHED_WAIT_CLOSED;
        }
    } else if (ops->park) {
        ops->park(waitable, fiber, io);
    } else {
        cc__fiber_park_reason("cc_sched_fiber_wait", __FILE__, __LINE__);
    }
    /*
     * Stage PARKED return path: post-park try_complete models wake_pending
     * recovery where a wake raced the park commit.
     */
    if (ops->try_complete && ops->try_complete(waitable, fiber, io)) {
        return CC_SCHED_WAIT_OK;
    }
    return CC_SCHED_WAIT_PARKED;
}

cc_sched_wait_result cc_sched_fiber_wait(
    void* waitable,
    void* io,
    const cc_sched_waitable_ops* ops
) {
    return cc_sched_fiber_wait_impl(waitable, io, ops, NULL);
}

cc_sched_wait_result cc_sched_fiber_wait_until(
    void* waitable,
    void* io,
    const cc_sched_waitable_ops* ops,
    const struct timespec* abs_deadline
) {
    return cc_sched_fiber_wait_impl(waitable, io, ops, abs_deadline);
}

cc_sched_wait_result cc_sched_fiber_wait_many(
    cc_sched_wait_case* cases,
    size_t count,
    _Atomic int* signaled_flag,
    _Atomic int* selected_index,
    size_t* out_selected_index
) {
    CCSchedFiber* fiber = (CCSchedFiber*)cc__fiber_current();
    if (!cases || count == 0 || !signaled_flag || !selected_index) {
        return CC_SCHED_WAIT_CLOSED;
    }

    if (out_selected_index) *out_selected_index = (size_t)-1;

    for (size_t i = 0; i < count; ++i) {
        const cc_sched_waitable_ops* ops = cases[i].ops;
        if (ops && ops->try_complete && ops->try_complete(cases[i].waitable, fiber, cases[i].io)) {
            if (out_selected_index) *out_selected_index = i;
            return CC_SCHED_WAIT_OK;
        }
    }

    for (size_t i = 0; i < count; ++i) {
        const cc_sched_waitable_ops* ops = cases[i].ops;
        if (!ops || !ops->publish || !ops->publish(cases[i].waitable, fiber, cases[i].io)) {
            cc_sched_wait_many_unpublish(cases, count, fiber);
            return CC_SCHED_WAIT_CLOSED;
        }
    }

    int selected = atomic_load_explicit(selected_index, memory_order_acquire);
    if (selected >= 0 || atomic_load_explicit(signaled_flag, memory_order_acquire) != 0) {
        cc_sched_wait_many_unpublish(cases, count, fiber);
        if (selected >= 0 && out_selected_index) *out_selected_index = (size_t)selected;
        return CC_SCHED_WAIT_OK;
    }

    for (size_t i = 0; i < count; ++i) {
        const cc_sched_waitable_ops* ops = cases[i].ops;
        if (ops && ops->try_complete && ops->try_complete(cases[i].waitable, fiber, cases[i].io)) {
            cc_sched_wait_many_unpublish(cases, count, fiber);
            if (out_selected_index) *out_selected_index = i;
            return CC_SCHED_WAIT_OK;
        }
    }

    if (atomic_load_explicit(signaled_flag, memory_order_acquire) == 0) {
        cc_sched_wait_on_flag(signaled_flag, 0, "cc_sched_fiber_wait_many");
    }

    selected = atomic_load_explicit(selected_index, memory_order_acquire);
    if (selected >= 0) {
        cc_sched_wait_many_unpublish(cases, count, fiber);
        if (out_selected_index) *out_selected_index = (size_t)selected;
        return CC_SCHED_WAIT_OK;
    }

    for (size_t i = 0; i < count; ++i) {
        const cc_sched_waitable_ops* ops = cases[i].ops;
        if (ops && ops->try_complete && ops->try_complete(cases[i].waitable, fiber, cases[i].io)) {
            cc_sched_wait_many_unpublish(cases, count, fiber);
            if (out_selected_index) *out_selected_index = i;
            return CC_SCHED_WAIT_OK;
        }
    }

    cc_sched_wait_many_unpublish(cases, count, fiber);
    return CC_SCHED_WAIT_PARKED;
}

void cc_sched_fiber_wake(CCSchedFiber* fiber) {
    /* LP (§10 Waker claim + wake enqueue): delegated to scheduler wake path. */
    cc__fiber_unpark(fiber);
}

void cc_sched_wait_on_flag(_Atomic int* flag, int expected, const char* reason) {
    cc__fiber_suspend_until_ready(flag,
                                  expected,
                                  reason ? reason : "cc_sched_wait_on_flag",
                                  __FILE__,
                                  __LINE__);
}
