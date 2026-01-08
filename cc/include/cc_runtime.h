#ifndef CC_RUNTIME_H
#define CC_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "cc_slice.h"
#include "cc_arena.h"
#include "cc_io.h"
#include "cc_result.h"
#include "cc_channel.h"
#include "cc_sched.h"
#include "cc_nursery.h"

// Forward declarations for runtime handles.
typedef struct CCChan CCChan;
typedef struct CCTask CCTask;

#endif // CC_RUNTIME_H

