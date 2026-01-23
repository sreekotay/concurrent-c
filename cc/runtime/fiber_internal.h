/*
 * Internal fiber (stackful coroutine) implementation.
 * Portable: ucontext on POSIX, Fibers API on Windows, assembly on ARM64.
 * NOT part of the public API - used internally by the scheduler.
 */
#ifndef CC_FIBER_INTERNAL_H
#define CC_FIBER_INTERNAL_H

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    typedef LPVOID cc__fiber_handle;
#else
    /* Use void* to store platform-specific context data */
    typedef void* cc__fiber_handle;
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

typedef enum {
    CC__FIBER_CREATED,
    CC__FIBER_READY,
    CC__FIBER_RUNNING,
    CC__FIBER_PARKED,
    CC__FIBER_DONE
} cc__fiber_state;

typedef void* (*cc__fiber_fn)(void*);

/* Forward declaration for wait queue */
struct cc__fiber_wait_node;

typedef struct cc__fiber {
    cc__fiber_handle ctx;
    void* stack;              /* POSIX only - Windows manages its own */
    size_t stack_size;
    cc__fiber_fn fn;
    void* arg;
    void* result;
    _Atomic int state;        /* cc__fiber_state - atomic for cross-thread unpark */
    struct cc__fiber* next;   /* for ready queue */
    
    /* For channel wait queues */
    struct cc__fiber_wait_node* wait_node;
} cc__fiber;

/* Wait node for fiber wait queues (channels, etc.) */
typedef struct cc__fiber_wait_node {
    cc__fiber* fiber;
    struct cc__fiber_wait_node* next;
    struct cc__fiber_wait_node* prev;
    void* data;               /* Optional data (e.g., send value for rendezvous) */
    _Atomic int notified;     /* Set when unparked */
} cc__fiber_wait_node;

/* Default stack size: 64KB */
#define CC__FIBER_STACK_SIZE (64 * 1024)

/* Create a fiber */
cc__fiber* cc__fiber_create(cc__fiber_fn fn, void* arg, size_t stack_size);

/* Free a fiber */
void cc__fiber_free(cc__fiber* f);

/* Initialize the fiber system for current thread (call once per worker) */
void cc__fiber_thread_init(void);

/* Switch to a fiber from scheduler */
void cc__fiber_switch_to(cc__fiber* f);

/* Yield back to scheduler */
void cc__fiber_yield(void);

/* Park current fiber (for channel waits) */
void cc__fiber_park(void);

/* Unpark a fiber (wake it up) - thread-safe */
void cc__fiber_unpark(cc__fiber* f);

/* Get current fiber (NULL if not in fiber context) */
cc__fiber* cc__fiber_current(void);

/* Check if running in fiber context */
int cc__fiber_in_context(void);

/* Mark current fiber as done and switch back */
void cc__fiber_exit(void* result);

/* Scheduler integration: enqueue unparked fiber for execution */
void cc__fiber_sched_enqueue(cc__fiber* f);

/* Check if scheduler is initialized */
int cc__fiber_sched_active(void);

#endif /* CC_FIBER_INTERNAL_H */
