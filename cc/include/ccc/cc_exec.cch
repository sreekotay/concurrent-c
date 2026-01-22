/*
 * Minimal executor for async offload (pthread work queue).
 * This is a portability layer; backpressure returns EBUSY/EAGAIN.
 */
#ifndef CC_EXEC_H
#define CC_EXEC_H

#include <stddef.h>
#ifndef __has_include
#define __has_include(x) 0
#endif
#if __has_include(<stdbool.h>)
#include <stdbool.h>
#else
#ifndef __bool_true_false_are_defined
typedef int bool;
#define true 1
#define false 0
#define __bool_true_false_are_defined 1
#endif
#endif
#include <errno.h>

typedef struct CCExec CCExec;

typedef void (*cc_exec_fn)(void *arg);

// Initialize executor with a fixed worker count and queue capacity.
// Returns NULL on failure.
CCExec* cc_exec_create(size_t workers, size_t queue_cap);
static inline CCExec* cc_exec_init(size_t workers, size_t queue_cap) {
    return cc_exec_create(workers, queue_cap);
}

// Submit a job; returns 0 on success, EAGAIN/EBUSY on full, or errno on failure.
int cc_exec_submit(CCExec* ex, cc_exec_fn fn, void *arg);

// Graceful shutdown: no new work, waits for queued tasks to finish.
void cc_exec_shutdown(CCExec* ex);

// Force free (after shutdown).
void cc_exec_free(CCExec* ex);
static inline void cc_exec_destroy(CCExec* ex) { cc_exec_free(ex); }

#endif // CC_EXEC_H



