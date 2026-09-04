/* Reset frees tier-3 heap overflow without a prior cc_arena_release. */
#include <ccc/std/prelude.cch>
#include <stdio.h>

int main(void) {
    _Alignas(CCArenaHost) uint8_t buf[CC_ARENA_REGION_BYTES(128)];
    CCArena a = cc_arena_create_buffer(buf, sizeof(buf), CC_ARENA_FIXED);
    void *spill;
    if (!a.base) {
        printf("FAIL: init\n");
        return 1;
    }
    if (!cc_arena_set_heap_overflow(a, true)) {
        printf("FAIL: enable overflow\n");
        return 1;
    }
    spill = cc_arena_alloc(a, 512, 8);
    if (!spill) {
        printf("FAIL: overflow alloc\n");
        return 1;
    }
    if (cc_arena_overflow_raw_bytes(a) < 512) {
        printf("FAIL: overflow accounting before reset\n");
        return 1;
    }
    if (!a.a->ovf_head) {
        printf("FAIL: expected ovf_head link\n");
        return 1;
    }
    {
        /* Arms as a mark; dropped here, it stays armed and dies at reset. */
        CCArenaCheckpoint cp = cc_arena_checkpoint(a);
        if (cp.arena == NULL) {
            printf("FAIL: checkpoint should work after overflow alloc\n");
            return 1;
        }
        if (a.a->mark_depth != 1 || !a.a->mark0.armed) {
            printf("FAIL: dropped checkpoint mark should stay armed\n");
            return 1;
        }
    }

    cc_arena_reset(a);

    if (a.a->ovf_head != NULL) {
        printf("FAIL: ovf_head not cleared\n");
        return 1;
    }
    if (cc_arena_overflow_raw_bytes(a) != 0) {
        printf("FAIL: overflow bytes after reset\n");
        return 1;
    }
    if (a.a->_flags & CC_ARENA_FLAG_USED_HEAP_OVERFLOW) {
        printf("FAIL: flags after reset\n");
        return 1;
    }
    if (a.a->active != NULL) {
        printf("FAIL: reset must tear down the active child\n");
        return 1;
    }
    {
        CCArenaCheckpoint cp = cc_arena_checkpoint(a);
        if (cp.arena == NULL) {
            printf("FAIL: checkpoint should work after reset\n");
            return 1;
        }
        if (!cc_arena_restore(cp)) {
            printf("FAIL: restore after reset\n");
            return 1;
        }
    }
    printf("arena_reset_overflow_smoke OK\n");
    return 0;
}
