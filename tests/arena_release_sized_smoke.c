/* Release is a signal; the strategy decides what the bytes become. Bump:
 * tip pop, hole, last-live rewind. Reuse: size-class lists. Durable: free. */
#include <ccc/std/prelude.cch>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(int code, const char *msg) {
    printf("FAIL: %s\n", msg);
    return code;
}

static int test_bump(void) {
    CCArena a = cc_arena_heap(4096);
    void *p;
    void *q;
    void *r;
    size_t off;
    if (!a.base) return fail(1, "heap");
    p = cc_arena_alloc(a, 32, 8);
    q = cc_arena_alloc(a, 32, 8);
    r = cc_arena_alloc(a, 32, 8);
    if (!p || !q || !r) return fail(1, "allocs");
    off = cc_atomic_load(&a.a->offset);

    /* Tip pop. */
    if (!cc_arena_release_sized(a, r, 32)) return fail(1, "tip release");
    if (cc_atomic_load(&a.a->offset) != off - 32) return fail(1, "tip popped");
    if (cc_atomic_load(&a.a->live_allocs) != 2) return fail(1, "live after pop");
    if (cc_arena_release_sized(a, r, 32)) return fail(1, "beyond the tip refuses");
    if (cc_arena_release(a, r)) return fail(1, "beyond the tip refuses unsized too");

    /* A size that cannot fit refuses and touches nothing. */
    if (cc_arena_release_sized(a, p, 4096)) return fail(1, "oversize refuses");
    if (cc_atomic_load(&a.a->live_allocs) != 2) return fail(1, "refusal leaves the count");

    /* Hole: not at the tip. */
    if (!cc_arena_release_sized(a, p, 32)) return fail(1, "hole release");
    if (cc_atomic_load(&a.a->offset) != off - 32) return fail(1, "hole keeps the tip");
    if (cc_atomic_load(&a.a->live_allocs) != 1) return fail(1, "live after hole");
    /* (A second hole release of `p` is indistinguishable from a first one
     * in the bump tier — no per-object header. Owners guard that case with
     * their token; see arena_owner_token_smoke.) */
    /* Last live: full rewind. */
    if (!cc_arena_release_sized(a, q, 32)) return fail(1, "last release");
    if (cc_atomic_load(&a.a->offset) != 0) return fail(1, "last-live rewind");

    /* Unsized release of the tip is a hole, not a pop (no size known). */
    p = cc_arena_alloc(a, 32, 8);
    q = cc_arena_alloc(a, 32, 8);
    if (!p || !q) return fail(1, "re-allocs");
    off = cc_atomic_load(&a.a->offset);
    if (!cc_arena_release(a, q)) return fail(1, "unsized tip release");
    if (cc_atomic_load(&a.a->offset) != off) return fail(1, "unsized release cannot pop");
    cc_arena_free(&a);
    printf("  bump: pop / hole / rewind OK\n");
    return 0;
}

static int test_reuse(void) {
    CCArena a = cc_arena_heap(8192);
    void *p;
    void *q;
    void *r;
    void *big;
    size_t off;
    if (!a.base) return fail(2, "heap");
    if (!cc_arena_set_reuse(a, true)) return fail(2, "enable reuse");
    if (!(a.a->_flags & CC_ARENA_FLAG_REUSE) || !a.a->reuse_free) return fail(2, "reuse state");
    /* The class table is itself a live slab allocation: it pins the slab
     * against a last-live rewind while lists may point into it. */
    if (cc_atomic_load(&a.a->live_allocs) != 1) return fail(2, "class table counted live");

    p = cc_arena_alloc(a, 40, 8);   /* class 64 */
    q = cc_arena_alloc(a, 24, 8);   /* class 32, keeps p off the tip */
    if (!p || !q) return fail(2, "allocs");
    if (((uintptr_t)p % 16) != 0 || ((uintptr_t)q % 16) != 0) return fail(2, "classed blocks are 16-aligned");
    off = cc_atomic_load(&a.a->offset);
    if (!cc_arena_release_sized(a, p, 40)) return fail(2, "list p");
    if (cc_atomic_load(&a.a->live_allocs) != 3) return fail(2, "listed block stays counted live");
    r = cc_arena_alloc(a, 50, 8);   /* class 64: re-served from the list */
    if (r != p) return fail(2, "same class reuses the block");
    if (cc_atomic_load(&a.a->offset) != off) return fail(2, "reuse did not bump");
    memset(r, 0x5a, 64);            /* the block really is class-sized */

    /* An over-aligned request never pops a list (a listed block may be
     * only 16-aligned) — it bumps fresh. */
    if (!cc_arena_release_sized(a, r, 50)) return fail(2, "list r");
    big = cc_arena_alloc(a, 40, 64);
    if (big == p) return fail(2, "over-aligned request must not reuse a 16-aligned block");
    if (((uintptr_t)big % 64) != 0) return fail(2, "over-aligned honored");
    /* ... and its release lists it under its class like any other (a
     * spacer keeps it off the tip, where a pop would win instead). */
    if (!cc_arena_alloc(a, 24, 8)) return fail(2, "spacer");
    if (!cc_arena_release_sized(a, big, 40)) return fail(2, "list big");
    r = cc_arena_alloc(a, 60, 8);
    if (r != big) return fail(2, "LIFO list serves the last listed");
    r = cc_arena_alloc(a, 60, 8);
    if (r != p) return fail(2, "then the earlier one");

    /* Tip pop wins over listing. */
    off = cc_atomic_load(&a.a->offset);
    r = cc_arena_alloc(a, 100, 8);  /* class 128 at the tip */
    if (!cc_arena_release_sized(a, r, 100)) return fail(2, "tip release under reuse");
    if (cc_atomic_load(&a.a->offset) != off) return fail(2, "tip pop preferred");
    if (a.a->reuse_free[cc__arena_reuse_class(128)] != NULL) return fail(2, "not listed");

    /* Above the largest class: plain bump / hole. */
    big = cc_arena_alloc(a, 100000, 8);
    if (!big) return fail(2, "large");
    if (!cc_arena_release_sized(a, big, 100000)) return fail(2, "large release");

    /* Disabling drops the lists; listed blocks become holes. */
    if (!cc_arena_set_reuse(a, false)) return fail(2, "disable");
    if (a.a->_flags & CC_ARENA_FLAG_REUSE) return fail(2, "flag cleared");
    cc_arena_reset(a);
    if (a.a->reuse_free != NULL || (a.a->_flags & CC_ARENA_FLAG_REUSE)) return fail(2, "reset clears reuse");
    cc_arena_free(&a);
    printf("  reuse classes OK\n");
    return 0;
}

static int test_durable_and_foreign(void) {
    CCArena a = cc_arena_malloc(64);
    CCArena b = cc_arena_heap(256);
    void *ovf;
    void *foreign;
    void *slab;
    if (!a.base || !b.base) return fail(3, "ctors");
    ovf = cc_arena_alloc(a, 512, 8);
    if (!ovf || !a.a->ovf_head) return fail(3, "per-object overflow");
    if (!cc_arena_release_sized(a, ovf, 512)) return fail(3, "sized release frees per-object");
    if (a.a->ovf_head || cc_arena_overflow_raw_bytes(a) != 0) return fail(3, "freed");

    foreign = malloc(64);
    if (!foreign) return fail(3, "malloc");
    if (cc_arena_release(a, foreign) || cc_arena_release_sized(b, foreign, 64))
        return fail(3, "foreign pointer refused");
    free(foreign);

    slab = cc_arena_alloc(b, 32, 8);
    if (!slab) return fail(3, "slab");
    if (cc_arena_release_sized(a, slab, 32)) return fail(3, "wrong arena refused");
    if (!cc_arena_release_sized(b, slab, 32)) return fail(3, "right arena releases");
    cc_arena_free(&a);
    cc_arena_free(&b);
    printf("  durable / foreign OK\n");
    return 0;
}

int main(void) {
    int rc;
    if ((rc = test_bump()) != 0) return rc;
    if ((rc = test_reuse()) != 0) return rc;
    if ((rc = test_durable_and_foreign()) != 0) return rc;
    printf("arena_release_sized_smoke OK\n");
    return 0;
}
