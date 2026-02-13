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
    uint64_t wait_ticket;
    struct cc__fiber_wait_node* next;
    struct cc__fiber_wait_node* prev;
    void* data;
    _Atomic int notified;
    void* select_group;
    size_t select_index;
    int is_select;
    int in_wait_list;
} cc__fiber_wait_node;

/* Fiber API - implemented in fiber_sched.c */
int cc__fiber_in_context(void);
void* cc__fiber_current(void);
void cc__fiber_park(void);
void cc__fiber_park_reason(const char* reason, const char* file, int line);
void cc__fiber_park_if(_Atomic int* flag, int expected, const char* reason, const char* file, int line);
void cc__fiber_unpark(void* fiber);
void cc__fiber_yield(void);         /* Cooperative yield - push to local queue */
void cc__fiber_yield_global(void);  /* Yield to global queue for fairness */
void cc__fiber_sched_enqueue(void* fiber);
void cc_fiber_dump_state(const char* reason);  /* Debug: dump scheduler state */
int cc__fiber_sched_active(void);
void cc__fiber_set_park_obj(void* obj);
void cc__fiber_clear_pending_unpark(void);  /* Clear stale pending_unpark before new wait */
void cc__fiber_sleep_park(unsigned int ms); /* Park fiber on sleep queue with timer */
uint64_t cc__fiber_publish_wait_ticket(void* fiber_ptr);
int cc__fiber_wait_ticket_matches(void* fiber_ptr, uint64_t ticket);

/* Convenience macro to park with source location */
#define CC_FIBER_PARK(reason) cc__fiber_park_reason(reason, __FILE__, __LINE__)
#define CC_FIBER_PARK_IF(flag, expected, reason) cc__fiber_park_if(flag, expected, reason, __FILE__, __LINE__)

#endif /* CC_FIBER_INTERNAL_H */
