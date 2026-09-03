/* Checkpoint = active child, across overflow tiers: per-object (malloc ctor)
 * and chunked (heap/stack after slab budget). Scratch minted through the
 * parent handle lands in the child and dies at restore; the parent's own
 * overflow (before or after the checkpoint) is untouched by it. */
#include <ccc/std/prelude.cch>
#include <stdio.h>
#include <string.h>

static int fail(int code, const char *msg) {
    printf("FAIL: %s\n", msg);
    return code;
}

static int count_ovf_objects(const CCArenaHost *h) {
    int n = 0;
    const CCArenaOvfHeader *o;
    for (o = h->ovf_head; o; o = o->next) n++;
    return n;
}

static int count_ovf_chunks(const CCArenaHost *h) {
    int n = 0;
    const CCArenaOvfChunk *c;
    for (c = h->ovf_chunks; c; c = c->next) n++;
    return n;
}

static int test_malloc_ctor_scratch_dies_with_child(void) {
    CCArena a = cc_arena_malloc(64);
    void *keep;
    void *drop;
    CCArenaCheckpoint cp;
    if (!a.base) return fail(1, "malloc ctor");

    keep = cc_arena_alloc(a, 128, 8);
    if (!keep) return fail(1, "pre-checkpoint overflow");
    memset(keep, 0x11, 128);
    if (!(a.a->_flags & CC_ARENA_FLAG_USED_HEAP_OVERFLOW))
        return fail(1, "expected overflow before checkpoint");

    cp = cc_arena_checkpoint(a);
    if (cp.arena == NULL) return fail(1, "checkpoint after overflow alloc");
    if (cp.arena == a.a || cp.parent != a.a) return fail(1, "child handle shape");

    drop = cc_arena_alloc(a, 128, 8);
    if (!drop) return fail(1, "post-checkpoint overflow");
    memset(drop, 0x22, 128);
    /* Scratch overflow belongs to the child, not the parent's list. */
    if (count_ovf_objects(a.a) != 1) return fail(1, "scratch must not join the parent's overflow");
    if (cc__arena_owner_host(a.a, drop) != cp.arena) return fail(1, "scratch owned by child");

    if (!cc_arena_restore(cp)) return fail(1, "restore");
    if (((unsigned char *)keep)[0] != 0x11) return fail(1, "keep-set overflow clobbered");
    if (count_ovf_objects(a.a) != 1) return fail(1, "parent overflow count after restore");
    if (a.a->ovf_head != cc__arena_ovf_header(keep)) return fail(1, "keep-set overflow stays on ovf_head");
    if (!cc_arena_release(a, keep)) return fail(1, "keep-set overflow still owned");
    cc_arena_free(&a);
    printf("  malloc ctor: scratch overflow dies with the child OK\n");
    return 0;
}

static int test_release_during_scratch_does_not_touch_restore(void) {
    CCArena a = cc_arena_malloc(256);
    void *slab;
    void *keep;
    void *ovf;
    CCArenaCheckpoint cp;
    if (!a.base) return fail(2, "malloc ctor live");

    slab = cc_arena_alloc(a, 32, 8);
    keep = cc_arena_alloc(a, 512, 8); /* per-object overflow, pre-checkpoint */
    if (!slab || !keep) return fail(2, "pre-checkpoint allocs");
    cp = cc_arena_checkpoint(a);
    if (cp.arena == NULL) return fail(2, "checkpoint");

    ovf = cc_arena_alloc(a, 512, 8); /* scratch overflow in the child */
    if (!ovf) return fail(2, "post-checkpoint overflow");
    if (!cc_arena_release(a, ovf)) return fail(2, "release scratch overflow through the parent handle");
    /* Releasing a pre-checkpoint object during scratch is an ordinary
     * parent release: it neither refuses nor poisons the restore. */
    if (!cc_arena_release(a, keep)) return fail(2, "release keep-set overflow during scratch");
    if (!cc_arena_restore(cp)) return fail(2, "restore after keep-set release");
    if (cc_arena_slab_live(a.a) != 1) return fail(2, "live_allocs after restore");
    if (!cc_arena_release(a, slab)) return fail(2, "pre-checkpoint slab after restore");
    cc_arena_free(&a);
    printf("  releases during scratch leave restore alone OK\n");
    return 0;
}

static int test_discarded_checkpoint_nests_later_ones(void) {
    CCArena a = cc_arena_malloc(64);
    void *keep;
    CCArenaCheckpoint dropped;
    CCArenaCheckpoint later;
    if (!a.base) return fail(7, "malloc ctor discard");

    keep = cc_arena_alloc(a, 128, 8);
    if (!keep) return fail(7, "overflow");
    dropped = cc_arena_checkpoint(a); /* handle kept only to inspect nesting */
    if (!dropped.arena) return fail(7, "dropped checkpoint arms");
    if (!cc_arena_release(a, keep)) return fail(7, "release after discarded checkpoint");
    later = cc_arena_checkpoint(a);
    if (later.arena == NULL) return fail(7, "later checkpoint arms inside the active child");
    if (later.parent != dropped.arena) return fail(7, "later checkpoint nests in the active child");
    if (!cc_arena_restore(later)) return fail(7, "restore later");
    /* The dropped child is still active; it dies with the parent. */
    if (a.a->active != dropped.arena) return fail(7, "dropped child stays active");
    cc_arena_free(&a);
    printf("  discarded checkpoint stays active; later ones nest OK\n");
    return 0;
}

static int test_lifo_armed_inner_refuses_outer(void) {
    CCArena a = cc_arena_heap(kilobytes(2));
    CCArenaCheckpoint cp1;
    CCArenaCheckpoint cp2;
    if (!a.base) return fail(8, "heap nested");

    if (!cc_arena_alloc(a, 32, 8)) return fail(8, "first slab");
    cp1 = cc_arena_checkpoint(a);
    if (!cp1.arena) return fail(8, "cp1");
    if (!cc_arena_alloc(a, 32, 8)) return fail(8, "second slab");
    cp2 = cc_arena_checkpoint(a);
    if (!cp2.arena || cp2.parent != cp1.arena) return fail(8, "cp2 nests in cp1");
    if (!cc_arena_alloc(a, 32, 8)) return fail(8, "third slab");
    /* An armed inner checkpoint pins the outer. */
    if (cc_arena_restore(cp1)) return fail(8, "outer restore must refuse while inner is armed");
    if (!cc_arena_restore(cp2)) return fail(8, "inner restore");
    if (cc_arena_restore(cp2)) return fail(8, "consumed inner restore must refuse");
    if (!cc_arena_restore(cp1)) return fail(8, "outer after inner");
    if (cc_arena_restore(cp1)) return fail(8, "consumed outer restore must refuse");
    if (cc_arena_slab_offset(a.a) != 32) return fail(8, "parent tip after both restores");
    cc_arena_free(&a);
    printf("  LIFO: armed inner pins outer OK\n");
    return 0;
}

static int test_stack_chunk_scratch_drains(void) {
    cc_arena_stack(s, 64);
    void *keep;
    void *drop;
    CCArenaCheckpoint cp;

    s.a->block_max = 2; /* root + one grow, then chunk overflow */
    if (!cc_arena_alloc(s, 32, 8)) return fail(4, "stack root fill");
    if (!cc_arena_alloc(s, 128, 8)) return fail(4, "stack grow");
    keep = cc_arena_alloc(s, 5000, 8);
    if (!keep || !s.a->ovf_chunks) return fail(4, "stack chunk overflow keep");
    memset(keep, 0x33, 128);

    cp = cc_arena_checkpoint(s);
    if (cp.arena == NULL) return fail(4, "stack checkpoint after overflow");

    drop = cc_arena_alloc(s, 70000, 8); /* well past the child's tail: its own chunk */
    if (!drop) return fail(4, "stack post-checkpoint overflow");
    if (count_ovf_chunks(s.a) != 1) return fail(4, "scratch chunk must belong to the child");

    if (!cc_arena_restore(cp)) return fail(4, "stack restore");
    if (((unsigned char *)keep)[0] != 0x33) return fail(4, "stack keep-set clobbered");
    if (count_ovf_chunks(s.a) != 1) return fail(4, "parent chunk count after restore");
    if (!cc_arena_release(s, keep)) return fail(4, "stack keep-set still owned");
    cc_arena_free(&s);
    printf("  stack: scratch chunk overflow dies with the child OK\n");
    return 0;
}

static int test_heap_chunk_scratch_drains(void) {
    CCArena a = cc_arena_heap(64);
    void *keep;
    void *drop;
    CCArenaCheckpoint cp;
    if (!a.base) return fail(5, "heap ctor");
    a.a->block_max = 2;

    if (!cc_arena_alloc(a, 32, 8)) return fail(5, "heap root fill");
    if (!cc_arena_alloc(a, 128, 8)) return fail(5, "heap grow");
    keep = cc_arena_alloc(a, 5000, 8);
    if (!keep || !a.a->ovf_chunks) return fail(5, "heap chunk overflow keep");
    memset(keep, 0x44, 200);

    cp = cc_arena_checkpoint(a);
    if (cp.arena == NULL) return fail(5, "heap checkpoint after overflow");
    drop = cc_arena_alloc(a, 70000, 8);
    if (!drop) return fail(5, "heap post-checkpoint overflow");
    if (count_ovf_chunks(a.a) != 1) return fail(5, "scratch chunk must belong to the child");

    if (!cc_arena_restore(cp)) return fail(5, "heap restore");
    if (((unsigned char *)keep)[0] != 0x44) return fail(5, "heap keep-set clobbered");
    if (count_ovf_chunks(a.a) != 1) return fail(5, "parent chunk count after restore");
    if (!cc_arena_release(a, keep)) return fail(5, "heap keep-set still owned");
    cc_arena_free(&a);
    printf("  heap: scratch chunk overflow dies with the child OK\n");
    return 0;
}

int main(void) {
    int rc;
    if ((rc = test_malloc_ctor_scratch_dies_with_child()) != 0) return rc;
    if ((rc = test_release_during_scratch_does_not_touch_restore()) != 0) return rc;
    if ((rc = test_stack_chunk_scratch_drains()) != 0) return rc;
    if ((rc = test_heap_chunk_scratch_drains()) != 0) return rc;
    if ((rc = test_discarded_checkpoint_nests_later_ones()) != 0) return rc;
    if ((rc = test_lifo_armed_inner_refuses_outer()) != 0) return rc;
    printf("arena_checkpoint_overflow_smoke OK\n");
    return 0;
}
