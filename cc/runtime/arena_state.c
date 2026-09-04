#define CC_ARENA_IMPL 1
#define CC_RESULT_IMPL 1
#include <ccc/cc_arena.cch>
#include "cc_mem_heap.c"
#ifndef __TINYC__
#include "cc_mem.c"
#endif

/* Moves only in whole CC__ARENA_EPOCH_BLOCK steps (hosts draw blocks); epoch 0 is never handed out. */
cc_atomic_u64 cc_arena_prov_counter = 256;

/* ffc.h (fast_float C port): one-TU definition for lazy number accessors /
 * schema float binds. Header-only API via <ccc/vendor/ffc.h>. */
#define FFC_IMPL
#include <ccc/vendor/ffc.h>

#include "slice_gen.c"
