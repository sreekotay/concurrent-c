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

/* Emit out-of-line arena/result sys bodies on the first include of those
 * headers (channel.c etc. pull them in before arena_state.c). */
#define CC_ARENA_IMPL 1
#define CC_RESULT_IMPL 1

#if defined(__TINYC__)
/* ucontext fiber backend (TCC aarch64 has no inline asm for minicoro ASM).
 * Do not set _XOPEN_SOURCE here — it breaks Apple SDK headers under TCC;
 * minicoro defines it locally just before #include <ucontext.h>. */
#define MCO_USE_UCONTEXT
#include "cc_pthread_tls.h"
#include "cc_cpu_port.h"
#endif

#include "channel.c"
#include "sched_v2.c"
#include "fiber_sched.c"
#include "scheduler.c"
#include "nursery.c"
#include "fiber_sched_boundary.c"
#include "closure.c"
#include "task.c"
#include "io.c"
#include "command.c"
#include "string.c"
#include "exec.c"
#include "arena_state.c"
#include "slice_adopt.c"
#include "slice_mem.c"
#include "cc_rt_diag.c"
#include "cc_type_info.c"
#include "cc_dyn_vec.c"
#include "io_wait.c"
#include "net.c"
#include "signal.c"
#include "socket.c"
#include "select.c"
#include "dir.c"
#include "process.c"
#include "exclusive.c"
#include "arc.c"
#include "parallel.c"
#include "future.c"
#include "turnstile.c"
#include "float_format_zmij.c"

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
