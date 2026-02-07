#include <ccc/std/prelude.cch>
#include <stdint.h>
#include <stdio.h>

/* Hash/eq functions for khashl: take values, not pointers */
static inline khint_t hash_i32(int k) {
    return kh_hash_uint32((khint_t)k);
}
static inline int eq_i32(int a, int b) { return a == b; }

Map(int, int, IntMap, hash_i32, eq_i32);

int main(void) {
    CCArena arena = cc_arena_heap(kilobytes(4));
    if (!arena.base) return 1;

    IntMap *m = IntMap_init(&arena);
    if (!m) return 2;
    
    int k1 = 1, v1 = 42;
    int k2 = 2, v2 = 7;
    int rc = 0;
    
    /* Put takes values directly now */
    if (IntMap_put(m, k1, v1, &rc) < 0) return 3;
    if (IntMap_put(m, k2, v2, &rc) < 0) return 4;

    /* Get now returns an optional */
    CCOptional_int opt1 = IntMap_get(m, k1);
    CCOptional_int opt2 = IntMap_get(m, k2);
    bool ok1 = opt1.has && opt1.u.value == v1;
    bool ok2 = opt2.has && opt2.u.value == v2;
    if (!ok1 || !ok2) return 5;
    
    /* Test deletion */
    if (!IntMap_del(m, k1)) return 6;
    CCOptional_int opt3 = IntMap_get(m, k1);
    if (opt3.has) return 7;  /* should not find deleted key */
    
    /* Test len */
    if (IntMap_len(m) != 1) return 8;

    CCSlice msg = cc_slice_from_buffer("map smoke ok\n", 14);
    cc_std_out_write(msg);

    cc_arena_free(&arena);
    return 0;
}
