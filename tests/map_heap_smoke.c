#include <ccc/std/prelude.cch>
#include <ccc/std/map_heap.cch>
#include <stdio.h>

CC_MAP_DECL_HEAP_SLICE(int, SliceIntHeapMap);

int main(void) {
    SliceIntHeapMap *m = SliceIntHeapMap_init();
    if (!m) {
        printf("map_heap_smoke: FAIL init\n");
        return 1;
    }

    CCSlice alpha = cc_slice_from_cstr("alpha");
    CCSlice beta = cc_slice_from_cstr("beta");

    if (SliceIntHeapMap_insert(m, alpha, 11) != 0) {
        printf("map_heap_smoke: FAIL insert alpha\n");
        return 2;
    }
    if (SliceIntHeapMap_insert(m, beta, 22) != 0) {
        printf("map_heap_smoke: FAIL insert beta\n");
        return 3;
    }

    int *got = SliceIntHeapMap_get_ptr(m, beta);
    if (!got || *got != 22) {
        printf("map_heap_smoke: FAIL get_ptr\n");
        return 4;
    }

    if (!SliceIntHeapMap_remove(m, alpha)) {
        printf("map_heap_smoke: FAIL remove\n");
        return 5;
    }
    if (SliceIntHeapMap_get_ptr(m, alpha) != NULL) {
        printf("map_heap_smoke: FAIL absent after remove\n");
        return 6;
    }

    if (SliceIntHeapMap_len(m) != 1) {
        printf("map_heap_smoke: FAIL len\n");
        return 7;
    }

    SliceIntHeapMap_destroy(m);
    printf("map_heap_smoke: PASS\n");
    return 0;
}
