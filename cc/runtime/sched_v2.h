/*
 * Hybrid Scheduler V2 — global-queue fiber/thread scheduler.
 *
 * Fiber lifecycle:
 *   QUEUED  -> on the global runnable queue
 *   RUNNING -> owned by a BUSY worker
 *   PARKED  -> blocked until an external signal re-enqueues it
 *   DEAD    -> completed
 *
 * A RUNNING fiber may also carry an internal "signal pending" bit so the
 * post-resume commit path can requeue it instead of parking. That bit is an
 * implementation detail, not a separate fiber state.
 */
#ifndef CC_SCHED_V2_H
#define CC_SCHED_V2_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

typedef struct fiber_v2 fiber_v2;
typedef struct CCNursery CCNursery;

/* Fiber states */
enum {
    FIBER_V2_IDLE    = 0,
    FIBER_V2_QUEUED  = 1,
    FIBER_V2_RUNNING = 2,
    FIBER_V2_PARKED  = 3,
    FIBER_V2_DEAD    = 4,
};

/* Public API */
void   sched_v2_ensure_init(void);
fiber_v2* sched_v2_spawn(void* (*fn)(void*), void* arg);
fiber_v2* sched_v2_spawn_in_nursery(void* (*fn)(void*), void* arg, CCNursery* nursery);
int    sched_v2_join(fiber_v2* f, void** out_result);
void   sched_v2_signal(fiber_v2* f);
void   sched_v2_park(void);
void   sched_v2_yield(void);
void   sched_v2_set_park_reason(const char* reason);
int    sched_v2_in_context(void);
fiber_v2* sched_v2_current_fiber(void);
int    sched_v2_current_worker_id(void); /* -1 if not on a V2 worker thread */
void   sched_v2_shutdown(void);

/* Accessors for task.c integration */
int    sched_v2_fiber_done(fiber_v2* f);
void*  sched_v2_fiber_result(fiber_v2* f);
char*  sched_v2_fiber_result_buf(fiber_v2* f);
void   sched_v2_fiber_release(fiber_v2* f);
void*  sched_v2_current_result_buf(size_t size);

/* Debug: dump a V2 fiber's state (for deadlock reports). Safe to call with
 * an arbitrary pointer; callers are expected to provide a live fiber_v2. */
void   sched_v2_debug_dump_fiber(fiber_v2* f, const char* prefix);

/* Debug: dump scheduler-wide V2 state for deadlock reports. */
void   sched_v2_debug_dump_state(const char* prefix);

/* Wait-ticket support (for kqueue / multi-wait integration) */
uint64_t sched_v2_fiber_publish_wait_ticket(fiber_v2* f);
int sched_v2_fiber_wait_ticket_matches(fiber_v2* f, uint64_t ticket);

#endif /* CC_SCHED_V2_H */
