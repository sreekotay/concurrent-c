#include <ccc/std/prelude.cch>
#include <stdio.h>

Vec(int, IntVec);

int main(void) {
    CCArena arena = cc_arena_heap(kilobytes(4));
    if (!arena.base) return 1;

    IntVec v = IntVec_init(&arena, 2);
    if (IntVec_push(&v, 10) != 0) return 2;
    if (IntVec_push(&v, 20) != 0) return 3;
    if (IntVec_push(&v, 30) != 0) return 4;

    CCOptional_int popped = IntVec_pop(&v);
    if (!popped.has || popped.u.value != 30) return 5;
    if (IntVec_len(&v) != 2) return 6;

    CCSlice msg = cc_slice_from_buffer("vec smoke ok\n", 14);
    cc_std_out_write(msg);

    cc_arena_free(&arena);
    return 0;
}

