#ifndef CC_FIBER_SCHED_BOUNDARY_H
#define CC_FIBER_SCHED_BOUNDARY_H

#include <stdbool.h>

/* Opaque runtime scheduler fiber handle. */
typedef struct fiber_task CCSchedFiber;

typedef enum cc_sched_wait_result {
    CC_SCHED_WAIT_OK = 0,
    CC_SCHED_WAIT_CLOSED = 1,
    CC_SCHED_WAIT_PARKED = 2,
} cc_sched_wait_result;

typedef struct cc_sched_waitable_ops {
    bool (*try_complete)(void* waitable, CCSchedFiber* fiber, void* io);
    bool (*publish)(void* waitable, CCSchedFiber* fiber, void* io);
    void (*unpublish)(void* waitable, CCSchedFiber* fiber);
    /*
     * Optional park hook. When provided, this is used instead of the default
     * park path so waitables can preserve flag-guarded park semantics.
     */
    void (*park)(void* waitable, CCSchedFiber* fiber, void* io);
} cc_sched_waitable_ops;

/* Internal runtime boundary for scheduler/channel integration. */
void cc_sched_schedule(CCSchedFiber* fiber);
CCSchedFiber* cc_sched_worker_next(void);
CCSchedFiber* cc_sched_worker_idle_probe(void);
cc_sched_wait_result cc_sched_fiber_wait(
    void* waitable,
    void* io,
    const cc_sched_waitable_ops* ops
);
void cc_sched_fiber_wake(CCSchedFiber* fiber);

#endif /* CC_FIBER_SCHED_BOUNDARY_H */
