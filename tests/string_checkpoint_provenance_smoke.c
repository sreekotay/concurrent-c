#include <ccc/std/prelude.cch>
#include <assert.h>
#include <string.h>

int main(void) {
    char buffer[512];
    CCArena arena = cc_arena_create_init(buffer, sizeof(buffer));
    assert(arena.base != NULL);

    CCString stable = cc_string_new(&arena);
    assert(stable.data != NULL);
    assert(cc_string_push(&stable, "stable") != NULL);

    CCSlice stable_view = cc_string_as_slice(&stable);
    uint64_t stable_id = cc_slice_make_id(stable.provenance, false, false, false);
    assert(stable_view.id == stable_id);
    assert(stable.provenance == arena.provenance);
    assert(stable_view.len == strlen("stable"));
    assert(memcmp(stable_view.ptr, "stable", stable_view.len) == 0);

    CCArenaCheckpoint cp = cc_arena_checkpoint(&arena);
    assert(cp.provenance == stable.provenance);
    assert(arena.provenance != cp.provenance);

    CCString transient = cc_string_new(&arena);
    assert(transient.data != NULL);
    assert(cc_string_push(&transient, "transient") != NULL);
    CCSlice transient_view = cc_string_as_slice(&transient);
    uint64_t transient_id = cc_slice_make_id(transient.provenance, false, false, false);
    assert(transient_view.id == transient_id);
    assert(transient_view.id != stable_view.id);

    cc_arena_restore(cp);
    assert(arena.provenance == cp.provenance);

    /* Pre-checkpoint strings keep their original epoch and remain valid. */
    assert(stable.provenance == cp.provenance);
    stable_view = cc_string_as_slice(&stable);
    assert(stable_view.id == stable_id);
    assert(memcmp(stable_view.ptr, "stable", stable_view.len) == 0);

    /* New allocations after restore re-enter the restored checkpoint epoch. */
    CCString after_restore = cc_string_new(&arena);
    assert(after_restore.data != NULL);
    assert(cc_string_push(&after_restore, "after") != NULL);
    CCSlice after_restore_view = cc_string_as_slice(&after_restore);
    assert(after_restore.provenance == cp.provenance);
    assert(after_restore_view.id == stable_id);

    cc_std_out_write(cc_slice_from_buffer("string checkpoint provenance smoke ok\n", 38));
    return 0;
}
