#include <ccc/std/prelude.cch>
#include <stdio.h>

CC_MAP_DECL_SLICE(int, SliceIntMap);
CC_MAP_DECL_SLICE_FULL(void*, SlicePtrMap, CCOptional_voidptr);

int main(void) {
    CCArena arena = cc_arena_heap(kilobytes(4));
    if (!arena.base) return 1;

    SliceIntMap *ints = SliceIntMap_init(&arena);
    SlicePtrMap *ptrs = SlicePtrMap_init(&arena);
    if (!ints || !ptrs) return 2;

    CCSlice alpha = cc_slice_from_cstr("alpha");
    CCSlice beta = cc_slice_from_cstr("beta");

    if (SliceIntMap_insert(ints, alpha, 11) != 0) return 3;
    if (SliceIntMap_insert(ints, beta, 22) != 0) return 4;

    int *got_i = SliceIntMap_get_ptr(ints, beta);
    if (!got_i || *got_i != 22) return 5;

    int a = 7;
    int b = 13;
    if (SlicePtrMap_insert(ptrs, alpha, &a) != 0) return 6;
    if (SlicePtrMap_insert(ptrs, beta, &b) != 0) return 7;

    void **got_p = SlicePtrMap_get_ptr(ptrs, beta);
    if (!got_p || !*got_p || *(int*)*got_p != 13) return 8;

    if (!SlicePtrMap_remove(ptrs, alpha)) return 9;
    if (SlicePtrMap_get_ptr(ptrs, alpha) != NULL) return 10;

    printf("map_slice_c_smoke: PASS\n");
    cc_arena_free(&arena);
    return 0;
}
