#define CC_ENABLE_SHORT_NAMES
#include "cc/include/std/prelude.h"
#include <stdint.h>
#include <stdio.h>

static inline uint64_t hash_i32(const int* k) {
    return (uint64_t)(uint32_t)(*k) * 11400714819323198485ull; // cheap mix
}
static inline bool eq_i32(const int* a, const int* b) { return *a == *b; }

Map(int, int, IntMap, hash_i32, eq_i32);

int main(void) {
    CCArena arena = cc_heap_arena(kilobytes(4));
    if (!arena.base) return 1;

    IntMap m = IntMap_init(&arena, 4);
    int k1 = 1, v1 = 42;
    int k2 = 2, v2 = 7;
    int rc = 0;
    if (IntMap_put(&m, &k1, &v1, &rc) < 0 || IntMap_put(&m, &k2, &v2, &rc) < 0) return 2;

    int got = 0;
    bool ok1 = IntMap_get(&m, &k1, &got) && got == v1;
    bool ok2 = IntMap_get(&m, &k2, &got) && got == v2;
    if (!ok1 || !ok2) return 3;

    CCSlice msg = cc_slice_from_buffer("map smoke ok\n", 14);
    cc_std_out_write(msg);

    cc_heap_arena_free(&arena);
    return 0;
}

