#include <ccc/std/prelude.cch>
#include <ccc/std/io.cch>
#include <ccc/std/string.cch>

#include <stdio.h>

int main(void) {
    char local[] = "local";
    CCSlice untracked = cc_slice_from_buffer(local, sizeof(local) - 1);
    if (!cc_slice_is_untracked(untracked)) {
        fprintf(stderr, "expected buffer slice to be untracked\n");
        return 1;
    }

    CCSlice stable = cc_slice_from_static((void*)"static", sizeof("static") - 1);
    if (!cc_slice_is_canonical(stable) || cc_slice_is_untracked(stable)) {
        fprintf(stderr, "expected static slice to be canonical tracked\n");
        return 2;
    }

    CCArena arena = cc_arena_heap(1024);
    if (!arena.base) return 3;

    uint64_t old_alloc_id = 0;
    {
        CCSlice cloned = cc_slice_clone(&arena, untracked);
        if (!cloned.ptr || cc_slice_is_untracked(cloned)) {
            fprintf(stderr, "expected cloned slice to carry arena provenance\n");
            return 4;
        }
        if (!cc_slice_is_from_arena_epoch(cloned, &arena)) {
            fprintf(stderr, "expected cloned slice to match current arena epoch\n");
            return 5;
        }
        old_alloc_id = cc_slice_alloc_id(cloned.id);
    }

    cc_arena_reset(&arena);
    /* Stale header reconstructed only to probe epoch bits — borrow scope ended above. */
    {
        CCSlice stale = cc_slice_from_parts((void*)"x", 1, cc_slice_make_id(old_alloc_id, false, false, false), 1);
        if (cc_slice_is_from_arena_epoch(stale, &arena)) {
            fprintf(stderr, "expected reset to invalidate old arena epoch\n");
            return 6;
        }
    }

    CCSlice left = cc_slice_from_cstr("foo");
    CCSlice right = cc_slice_from_cstr("bar");
    CCSlice joined = cc_slice_concat2(left, right, &arena);
    if (!joined.ptr || !cc_slice_is_from_arena_epoch(joined, &arena)) {
        fprintf(stderr, "expected concat slice to carry arena provenance\n");
        return 7;
    }

    CCSlice path = cc_path_join(&arena, cc_slice_from_cstr("/tmp"), cc_slice_from_cstr("file"));
    if (!path.ptr || !cc_slice_is_from_arena_epoch(path, &arena)) {
        fprintf(stderr, "expected path join to carry arena provenance\n");
        return 8;
    }

    CCSlice dir = cc_path_dirname(&arena, path);
    if (!dir.ptr || !cc_slice_is_from_arena_epoch(dir, &arena)) {
        fprintf(stderr, "expected dirname allocation to carry arena provenance\n");
        return 9;
    }

    CCSlice base = cc_path_basename(&arena, path);
    if (!base.ptr || !cc_slice_is_from_arena_epoch(base, &arena)) {
        fprintf(stderr, "expected basename allocation to carry arena provenance\n");
        return 10;
    }

    cc_arena_free(&arena);
    printf("slice_provenance_helpers_smoke: PASS\n");
    return 0;
}
