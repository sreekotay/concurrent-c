/*
 * Single-translation-unit runtime for Concurrent-C.
 *
 * Consumers can compile and link just this file to get the runtime without
 * worrying about multiple objects. It simply aggregates the other runtime
 * implementation units.
 */

#include "channel.c"
#include "scheduler.c"
#include "nursery.c"
#include "io.c"
#include "string.c"
#include "exec.c"
#ifdef CC_ENABLE_ASYNC
#include "async_chan.c"
#include "async_runtime.c"
#include "async_backend_poll.c"
#endif

