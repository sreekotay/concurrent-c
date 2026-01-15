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
#include "scheduler.c"
#include "nursery.c"
#include "closure.c"
#include "task_intptr.c"
#include "io.c"
#include "string.c"
#include "exec.c"
#include "arena_state.c"
#include "net.c"

#ifdef CC_ENABLE_ASYNC
#include "async_chan.c"
#include "async_runtime.c"
#include "async_backend_poll.c"
#endif

#ifdef CC_ENABLE_TLS
#define CC_HAS_BEARSSL 1
#include "tls.c"
#endif

/* HTTP support is linked separately - not part of base runtime.
 * User projects that need HTTP must:
 *   1. Add CC_TARGET_LIBS for curl
 *   2. Add CC_TARGET_DEFINE CC_ENABLE_HTTP=1
 * The http.c file compiles as part of their build when http.cch is included.
 */
