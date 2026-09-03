/* Heap strings are owners: a view carries the arena epoch and the owner
 * token. A checkpoint child has its own epoch, so strings promoted during
 * scratch are stale after restore while pre-checkpoint strings keep theirs. */
#include <ccc/std/prelude.cch>
#include <assert.h>
#include <string.h>

int main(void) {
    /* Heap-rooted: a pre-checkpoint string regrown during scratch needs a
     * parent extent while the child holds the tail. */
    CCArena arena = cc_arena_heap(1024);
    assert(arena.base != NULL);

    CCString stable = cc_string_new();
    assert(cc_string_push(&stable, "stable-promoted-123", arena) != NULL);
    assert(!cc_string_is_inline(&stable));

    CCSlice stable_view = cc_string_as_slice(&stable);
    uint64_t stable_provenance = cc_string_provenance(&stable);
    CCArenaOwner *stable_owner = cc_string_owner(&stable);
    assert(stable_owner != NULL);
    assert(stable_view.id == cc_arena_owner_slice_id(stable_owner));
    assert(cc_slice_is_grower_id(stable_view.id));
    assert(cc_slice_id_gen(stable_view.id) == stable.tag);
    assert(stable_provenance == arena.a->provenance);
    assert(stable_view.len == strlen("stable-promoted-123"));
    assert(memcmp(stable_view.ptr, "stable-promoted-123", stable_view.len) == 0);

    /* SSO fits sizeof(void*) bytes (4 on ILP32, 8 on LP64) — keep payloads
     * short enough for both. */
    CCString inline_stable = cc_string_new();
    assert(cc_string_push(&inline_stable, "ab", arena) != NULL);
    CCSlice inline_stable_view = cc_string_as_slice(&inline_stable);
    assert(cc_string_provenance(&inline_stable) == CC_SLICE_ID_UNTRACKED);
    assert(inline_stable_view.id == CC_SLICE_ID_UNTRACKED);
    assert(memcmp(inline_stable_view.ptr, "ab", inline_stable_view.len) == 0);

    CCArenaCheckpoint cp = cc_arena_checkpoint(arena);
    assert(cp.arena != NULL && cp.parent == arena.a);
    /* The parent's epoch is untouched; the child has its own. */
    assert(arena.a->provenance == stable_provenance);
    assert(cp.arena->provenance != stable_provenance);

    CCString transient = cc_string_new();
    assert(cc_string_push(&transient, "temp-promoted-456", arena) != NULL);
    CCSlice transient_view = cc_string_as_slice(&transient);
    assert(cc_string_provenance(&transient) == cp.arena->provenance);
    assert(cc_slice_id_epoch(transient_view.id) != cc_slice_id_epoch(stable_view.id));
    assert(cc_slice_is_from_arena_epoch(transient_view, arena.a)); /* via the active chain */

    /* A pre-checkpoint string regrown during scratch stays in the parent. */
    assert(cc_string_push(&stable, "-grown-well-past-its-first-capacity", arena) != NULL);
    assert(cc_string_provenance(&stable) == stable_provenance);
    assert(cc_string_owner(&stable)->arena == arena.a);

    assert(cc_arena_restore(cp));
    assert(arena.a->provenance == stable_provenance);
    /* The scratch view's epoch is dead. */
    assert(!cc_slice_is_from_arena_epoch(transient_view, arena.a));

    /* Pre-checkpoint strings keep their epoch and remain valid. */
    assert(cc_string_provenance(&stable) == stable_provenance);
    stable_view = cc_string_as_slice(&stable);
    assert(memcmp(stable_view.ptr, "stable-promoted-123-grown", 25) == 0);

    inline_stable_view = cc_string_as_slice(&inline_stable);
    assert(cc_string_provenance(&inline_stable) == CC_SLICE_ID_UNTRACKED);
    assert(inline_stable_view.id == CC_SLICE_ID_UNTRACKED);
    assert(memcmp(inline_stable_view.ptr, "ab", inline_stable_view.len) == 0);

    /* New promoted allocations after restore are back in the parent's epoch. */
    CCString after_restore_heap = cc_string_new();
    assert(cc_string_push(&after_restore_heap, "after-promoted-789", arena) != NULL);
    CCSlice after_restore_heap_view = cc_string_as_slice(&after_restore_heap);
    assert(cc_string_provenance(&after_restore_heap) == stable_provenance);
    assert(cc_slice_id_epoch(after_restore_heap_view.id) == cc_slice_id_epoch(stable_view.id));
    assert(cc_slice_id_gen(after_restore_heap_view.id) != cc_slice_id_gen(stable_view.id));

    cc_std_out_write(cc_slice_from_buffer("string checkpoint provenance smoke ok\n", 38));
    return 0;
}
