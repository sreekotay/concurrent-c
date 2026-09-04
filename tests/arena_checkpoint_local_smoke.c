/* Owner-only checkpoint pair, epoch blocks, and marks that live beside the
 * host: the local pair restores the tip with no lock; it falls back to the
 * shared pair when the mark stack is full (promotion) and the handle still
 * restores; heap and stack hosts keep their nested marks across a restore
 * to depth 0; a stack host's marks live in the frame; epochs from two hosts
 * never collide; a reset draws a fresh epoch. */
#include <ccc/cc_arena.cch>
#include <stdio.h>

static int fail(int code, const char *msg) {
    printf("FAIL: %s\n", msg);
    return code;
}

static int test_local_round_trip(void) {
    CCArena a = cc_arena_heap(4096);
    CCArenaCheckpoint cp;
    void *p;
    size_t off0;
    if (!a.a) return fail(1, "heap");
    p = cc_arena_alloc(a, 16, 8);
    if (!p) return fail(1, "pre");
    off0 = cc_arena_slab_offset(a.a);
    cp = cc_arena_checkpoint_local(a);
    if (!cp.arena || cp.idx != 0 || cp.offset != off0 || !cp.id) return fail(1, "local mark");
    if (a.a->mark_depth != 1 || cc__arena_mark_at(a.a, 0)->epoch != cp.id) return fail(1, "mark written");
    if (!cc_arena_alloc(a, 64, 8) || !cc_arena_alloc(a, 64, 8)) return fail(1, "scratch");
    if (!cc_arena_restore_local(cp)) return fail(1, "local restore");
    if (cc_arena_slab_offset(a.a) != off0 || a.a->mark_depth != 0) return fail(1, "tip back at the mark");
    if (cc_arena_slab_live(a.a) != 1) return fail(1, "live count back");
    if (cc_arena_restore_local(cp)) return fail(1, "second restore refuses");
    cc_arena_free(&a);
    printf("  local round trip OK\n");
    return 0;
}

static int test_nested_local_keeps_marks(void) {
    CCArena a = cc_arena_heap(4096);
    CCArenaMark *more;
    int i;
    if (!a.a) return fail(2, "heap");
    if (!(a.a->_flags & CC_ARENA_FLAG_MARKS_FIXED) || !a.a->more) return fail(2, "heap host carries its marks");
    more = a.a->more;
    for (i = 0; i < 1000; i++) {
        CCArenaCheckpoint c1 = cc_arena_checkpoint_local(a);
        CCArenaCheckpoint c2;
        if (!c1.arena) return fail(2, "c1");
        if (!cc_arena_alloc(a, 32, 8)) return fail(2, "s1");
        c2 = cc_arena_checkpoint_local(a);
        if (!c2.arena || c2.idx != 1) return fail(2, "c2 nests");
        if (!cc_arena_alloc(a, 32, 8)) return fail(2, "s2");
        if (cc_arena_restore_local(c1)) return fail(2, "outer pinned by inner");
        if (!cc_arena_restore_local(c2) || !cc_arena_restore_local(c1)) return fail(2, "LIFO");
        if (a.a->more != more) return fail(2, "marks stay put across a restore to depth 0");
        if (cc_arena_slab_offset(a.a) != 0) return fail(2, "nothing carved from the slab for marks");
    }
    cc_arena_free(&a);
    printf("  nested local, fixed marks OK\n");
    return 0;
}

static int test_depth_falls_back(void) {
    CCArena a = cc_arena_heap(8192);
    CCArenaCheckpoint cps[CC_ARENA_MARK_DEPTH + 1];
    int i;
    if (!a.a) return fail(3, "heap");
    for (i = 0; i < CC_ARENA_MARK_DEPTH + 1; i++) {
        cps[i] = cc_arena_checkpoint_local(a);
        if (!cps[i].arena) return fail(3, "deep local checkpoint arms (via fallback)");
        if (!cc_arena_alloc(a, 16, 8)) return fail(3, "deep scratch");
    }
    if (!a.a->active) return fail(3, "depth promoted through the shared path");
    for (i = CC_ARENA_MARK_DEPTH; i >= 0; i--) {
        if (!cc_arena_restore_local(cps[i])) return fail(3, "local restore falls back through the child");
    }
    if (a.a->active || a.a->mark_depth || cc_arena_slab_offset(a.a) != 0) return fail(3, "everything rewound");
    cc_arena_free(&a);
    printf("  depth fallback OK\n");
    return 0;
}

static int test_stack_marks_in_frame(void) {
    cc_arena_stack(sc, 1024);
    CCArenaCheckpoint c1, c2;
    uintptr_t lo = (uintptr_t)&sc, hi = lo;
    if (!sc.a) return fail(4, "stack");
    if (!(sc.a->_flags & CC_ARENA_FLAG_MARKS_FIXED) || !sc.a->more) return fail(4, "stack host carries its marks");
    /* the marks are not on the slab */
    if ((uint8_t *)sc.a->more >= sc.a->slab->base &&
        (uint8_t *)sc.a->more < sc.a->slab->base + sc.a->slab->capacity)
        return fail(4, "marks must not live on the slab");
    (void)lo; (void)hi;
    c1 = cc_arena_checkpoint_local(sc);
    if (!cc_arena_alloc(sc, 8, 8)) return fail(4, "s1");
    c2 = cc_arena_checkpoint_local(sc);
    if (!c2.arena || c2.idx != 1) return fail(4, "nested on stack host");
    if (!cc_arena_restore_local(c2) || !cc_arena_restore_local(c1)) return fail(4, "restore");
    if (cc_arena_slab_offset(sc.a) != 0) return fail(4, "slab untouched by marks");
    cc_arena_destroy(&sc);
    printf("  stack marks in frame OK\n");
    return 0;
}

static int test_epochs(void) {
    CCArena a = cc_arena_heap(1024);
    CCArena b = cc_arena_heap(1024);
    uint64_t seen[600];
    int n = 0, i, j;
    if (!a.a || !b.a) return fail(5, "heap");
    seen[n++] = a.a->provenance;
    seen[n++] = b.a->provenance;
    for (i = 0; i < 290; i++) {
        CCArenaCheckpoint ca = cc_arena_checkpoint_local(a);
        CCArenaCheckpoint cb = cc_arena_checkpoint(b);
        if (!ca.arena || !cb.arena) return fail(5, "cp");
        seen[n++] = ca.id;
        seen[n++] = cb.id;
        if (!cc_arena_restore_local(ca) || !cc_arena_restore(cb)) return fail(5, "restore");
    }
    for (i = 0; i < n; i++) {
        if (seen[i] == 0) return fail(5, "epoch 0 handed out");
        for (j = i + 1; j < n; j++) if (seen[i] == seen[j]) return fail(5, "epoch collision across hosts / blocks");
    }
    {
        uint64_t before = a.a->provenance;
        cc_arena_reset(a);
        if (a.a->provenance == before) return fail(5, "reset draws a fresh epoch");
    }
    cc_arena_free(&a);
    cc_arena_free(&b);
    printf("  epochs unique across blocks OK\n");
    return 0;
}

int main(void) {
    int rc;
    if ((rc = test_local_round_trip())) return rc;
    if ((rc = test_nested_local_keeps_marks())) return rc;
    if ((rc = test_depth_falls_back())) return rc;
    if ((rc = test_stack_marks_in_frame())) return rc;
    if ((rc = test_epochs())) return rc;
    printf("arena_checkpoint_local_smoke: OK\n");
    return 0;
}
