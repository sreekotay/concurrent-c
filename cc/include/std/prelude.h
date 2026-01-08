/*
 * Concurrent-C stdlib prelude.
 *
 * Includes the stdlib headers and, if CC_ENABLE_SHORT_NAMES is defined before
 * inclusion, provides short aliases. By default only prefixed names (CC*)
 * are visible to avoid collisions.
 */
#ifndef CC_STD_PRELUDE_H
#define CC_STD_PRELUDE_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "../cc_arena.h"
#include "../cc_slice.h"
#include "../cc_channel.h"
#include "../cc_exec.h"
#include "string.h"
#include "io.h"
#include "vec.h"
#include "map.h"
#include "async_io.h"
#include "future.h"

#ifdef CC_ENABLE_SHORT_NAMES
typedef CCArena Arena;
typedef CCString String;
typedef CCFile File;
typedef CCSlice Slice;
#define ChanPtr(T, Name) CC_DECL_TYPED_CHAN_PTR(T, Name)
#define ChanVal(T, Name) CC_DECL_TYPED_CHAN_VAL(T, Name)
#define Chan(T, Name) CC_DECL_TYPED_CHAN_PTR(T, Name)
#define Vec(T, Name) CC_VEC_DECL_ARENA(T, Name)
#define Map(K, V, Name, HASH_FN, EQ_FN) CC_MAP_DECL_ARENA(K, V, Name, HASH_FN, EQ_FN)
#endif

static inline size_t kilobytes(size_t n) { return n * 1024; }
static inline size_t megabytes(size_t n) { return n * 1024 * 1024; }

// Allocate an arena with heap-backed storage of given size.
static inline CCArena cc_heap_arena(size_t bytes) {
    CCArena a = {0};
    void* buf = malloc(bytes);
    if (buf && cc_arena_init(&a, buf, bytes) != 0) {
        free(buf);
        a.base = NULL;
    }
    return a;
}

// Free backing storage allocated by cc_heap_arena().
static inline void cc_heap_arena_free(CCArena* a) {
    if (!a || !a->base) return;
    free(a->base);
    a->base = NULL;
    a->capacity = 0;
    a->offset = 0;
}

#endif // CC_STD_PRELUDE_H

