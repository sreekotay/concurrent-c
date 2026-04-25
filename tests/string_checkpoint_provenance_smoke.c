#include <ccc/std/prelude.cch>
#include <assert.h>
#include <string.h>

int main(void) {
    char buffer[512];
    CCArena arena = cc_arena_create_buffer(buffer, sizeof(buffer), CC_ARENA_FIXED);
    assert(arena.base != NULL);

    CCString stable = CCString_new(&arena);
    assert(stable.arena == &arena);
    assert(CCString_push(&stable, "stable-promoted-123") != NULL);

    CCSlice stable_view = CCString_as_slice(&stable);
    uint64_t stable_provenance = CCString_provenance(&stable);
    uint64_t stable_id = cc_slice_make_id(stable_provenance, false, false, false);
    assert(stable_view.id == stable_id);
    assert(stable_provenance == arena.provenance);
    assert(stable_view.len == strlen("stable-promoted-123"));
    assert(memcmp(stable_view.ptr, "stable-promoted-123", stable_view.len) == 0);

    CCString inline_stable = CCString_new(&arena);
    assert(inline_stable.arena == &arena);
    assert(CCString_push(&inline_stable, "short") != NULL);
    CCSlice inline_stable_view = CCString_as_slice(&inline_stable);
    assert(CCString_provenance(&inline_stable) == CC_SLICE_ID_UNTRACKED);
    assert(inline_stable_view.id == CC_SLICE_ID_UNTRACKED);
    assert(memcmp(inline_stable_view.ptr, "short", inline_stable_view.len) == 0);

    CCArenaCheckpoint cp = cc_arena_checkpoint(&arena);
    assert(cp.provenance == stable_provenance);
    assert(arena.provenance != cp.provenance);

    CCString transient = CCString_new(&arena);
    assert(transient.arena == &arena);
    assert(CCString_push(&transient, "temp-promoted-456") != NULL);
    CCSlice transient_view = CCString_as_slice(&transient);
    uint64_t transient_id = cc_slice_make_id(CCString_provenance(&transient), false, false, false);
    assert(transient_view.id == transient_id);
    assert(transient_view.id != stable_view.id);

    cc_arena_restore(cp);
    assert(arena.provenance == cp.provenance);

    /* Pre-checkpoint strings keep their original epoch and remain valid. */
    assert(CCString_provenance(&stable) == cp.provenance);
    stable_view = CCString_as_slice(&stable);
    assert(stable_view.id == stable_id);
    assert(memcmp(stable_view.ptr, "stable-promoted-123", stable_view.len) == 0);

    inline_stable_view = CCString_as_slice(&inline_stable);
    assert(CCString_provenance(&inline_stable) == CC_SLICE_ID_UNTRACKED);
    assert(inline_stable_view.id == CC_SLICE_ID_UNTRACKED);
    assert(memcmp(inline_stable_view.ptr, "short", inline_stable_view.len) == 0);

    CCString after_restore_inline = CCString_new(&arena);
    assert(after_restore_inline.arena == &arena);
    assert(CCString_push(&after_restore_inline, "after") != NULL);
    CCSlice after_restore_inline_view = CCString_as_slice(&after_restore_inline);
    assert(CCString_provenance(&after_restore_inline) == CC_SLICE_ID_UNTRACKED);
    assert(after_restore_inline_view.id == CC_SLICE_ID_UNTRACKED);

    /* New promoted allocations after restore re-enter the restored checkpoint epoch. */
    CCString after_restore_heap = CCString_new(&arena);
    assert(after_restore_heap.arena == &arena);
    assert(CCString_push(&after_restore_heap, "after-promoted-789") != NULL);
    CCSlice after_restore_heap_view = CCString_as_slice(&after_restore_heap);
    assert(CCString_provenance(&after_restore_heap) == cp.provenance);
    assert(after_restore_heap_view.id == stable_id);

    cc_std_out_write(cc_slice_from_buffer("string checkpoint provenance smoke ok\n", 38));
    return 0;
}
