/*
 * Fiber types and API for M:N scheduling.
 * Used by channel.c for fiber-aware blocking.
 */
#ifndef CC_FIBER_INTERNAL_H
#define CC_FIBER_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

/* Opaque fiber handle - actual definition in fiber_sched.c */
typedef struct fiber_task cc__fiber;

/* Wait node for channel blocking */
typedef struct cc__fiber_wait_node {
    cc__fiber* fiber;
    struct cc__fiber_wait_node* next;
    struct cc__fiber_wait_node* prev;
    void* data;
    _Atomic int notified;
} cc__fiber_wait_node;

/* Fiber API - implemented in fiber_sched.c */
int cc__fiber_in_context(void);
void* cc__fiber_current(void);
void cc__fiber_park(void);
void cc__fiber_park_reason(const char* reason, const char* file, int line);
void cc__fiber_unpark(void* fiber);
void cc__fiber_sched_enqueue(void* fiber);
void cc_fiber_dump_state(const char* reason);  /* Debug: dump scheduler state */
int cc__fiber_sched_active(void);

/* Convenience macro to park with source location */
#define CC_FIBER_PARK(reason) cc__fiber_park_reason(reason, __FILE__, __LINE__)

#endif /* CC_FIBER_INTERNAL_H */
