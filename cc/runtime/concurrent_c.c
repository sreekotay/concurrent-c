/*
 * Single-translation-unit runtime for Concurrent-C.
 *
 * Consumers can compile and link just this file to get the runtime without
 * worrying about multiple objects. It simply aggregates the other runtime
 * implementation units.
 *
 * Optional features (define before including):
 *   CC_ENABLE_ASYNC - Enable async runtime (poll/io_uring backend)
 *   CC_ENABLE_TLS   - Enable TLS support (requires BearSSL)
 */

#include "channel.c"
#if CC_RUNTIME_V3
#include "fiber_sched_v3.c"
#else
#include "fiber_sched.c"
#endif
#include "scheduler.c"
#include "nursery.c"
#include "fiber_sched_boundary.c"
#include "closure.c"
#include "task.c"
#include "io.c"
#include "string.c"
#include "exec.c"
#include "arena_state.c"
#include "net.c"
#include "dir.c"
#include "process.c"

#ifdef CC_ENABLE_ASYNC
#include "async_chan.c"
#include "async_runtime.c"
#include "async_backend_poll.c"
#endif

#ifdef CC_ENABLE_TLS
#define CC_HAS_BEARSSL 1
#include "tls.c"
#endif

/* HTTP support is header-only - included from http.cch when user code needs it.
 * User must add @link("curl") to their source file to link libcurl. */
