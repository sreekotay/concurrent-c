#define CC_ARENA_IMPL 1
#define CC_RESULT_IMPL 1
#include <ccc/cc_arena.cch>
#ifndef __TINYC__
#include "cc_mem.c"
#endif

cc_atomic_u64 cc_arena_prov_counter = 1;

/* ffc.h (fast_float C port): one-TU definition for lazy number accessors /
 * schema float binds. Header-only API via <ccc/vendor/ffc.h>. */
#define FFC_IMPL
#include <ccc/vendor/ffc.h>

#include "slice_gen.c"
