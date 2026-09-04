/* Adversarial: every misuse the runtime can see must refuse or no-op, never
 * corrupt. Aliased double destroy, destroy after a manual payload release,
 * regrow through a stale alias, restores of consumed / non-LIFO / stale
 * handles, releases of foreign, wrong-arena, wrong-size and post-restore
 * pointers, checkpoint children under a reset, token registry churn, and
 * concurrent container growth on one shared arena. */
#include <ccc/std/prelude.cch>
#include <ccc/std/vec.cch>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Vec(uint64_t, U64Vec);

static int fail(int code, const char *msg) {
    printf("FAIL: %s\n", msg);
    return code;
}

static int test_owner_misuse(void) {
    CCArena a = cc_arena_heap(kilobytes(8));
    U64Vec v;
    U64Vec alias;
    U64Vec by_value;
    CCArenaOwner *o;
    uint32_t tok;
    size_t live;
    size_t i;
    if (!a.base) return fail(1, "heap");
    v = U64Vec_init(a, 4);
    if (!v.data) return fail(1, "init");
    o = v.own;
    tok = v.token;
    for (i = 0; i < 4; i++) U64Vec_push(&v, i);
    alias = v;

    /* Manual release of the payload out from under the owner: the owner's
     * later release finds the block beyond the tip and refuses, the header
     * still dies once, and the counts stay consistent. */
    live = cc_arena_slab_live(a.a);
    if (!cc_arena_release_sized(a, v.data, 4 * sizeof(uint64_t))) return fail(1, "manual payload release");
    if (cc_arena_slab_live(a.a) != live - 1) return fail(1, "payload counted once");
    U64Vec_destroy(&v);
    if (cc_arena_slab_live(a.a) != live - 1) return fail(1, "destroy after manual release does not double count");
    if (cc_arena_owner_live(o, tok)) return fail(1, "token dead");
    if (a.a->owner_free != o) return fail(1, "header listed");

    /* Stale alias after the destroy: no grow, no view, no second release. */
    if (U64Vec_push(&alias, 7) == 0) return fail(1, "stale alias push refused");
    if (U64Vec_as_slice(&alias).len != 0) return fail(1, "stale alias view empty");
    U64Vec_destroy(&alias);
    if (cc_arena_slab_live(a.a) != live - 1) return fail(1, "stale alias destroy no-op");

    /* The listed header is reborn for a new vec; the old alias still
     * mismatches (fresh token) even though it names the same header. */
    by_value = U64Vec_init(a, 2);
    if (!by_value.data || by_value.own != o) return fail(1, "header reborn");
    if (by_value.token == tok) return fail(1, "fresh token");
    if (U64Vec_push(&alias, 1) == 0) return fail(1, "old alias still refused after rebirth");
    U64Vec_destroy(&alias);
    if (!cc_arena_owner_live(by_value.own, by_value.token)) return fail(1, "old alias destroy cannot kill the reborn owner");
    U64Vec_destroy(&by_value);

    /* Regrow through a stale token is refused; through the live one works. */
    v = U64Vec_init(a, 2);
    tok = v.token;
    (void)cc_arena_alloc(a, 32, 8);
    for (i = 0; i < 40; i++) U64Vec_push(&v, i);   /* moves at least once */
    if (v.token == tok) return fail(1, "moved");
    if (cc_arena_owner_regrow(v.own, tok, 4096)) return fail(1, "stale regrow refused");
    if (!cc_arena_owner_regrow(v.own, v.token, 4096)) return fail(1, "live regrow");
    if (cc_arena_owner_release(v.own, tok)) return fail(1, "stale release refused");
    U64Vec_destroy(&v);
    cc_arena_free(&a);
    printf("  owner misuse OK\n");
    return 0;
}

static int test_checkpoint_misuse(void) {
    CCArena a = cc_arena_heap(kilobytes(4));
    CCArenaCheckpoint cp;
    CCArenaCheckpoint inner;
    CCArenaCheckpoint copy;
    CCArenaCheckpoint bogus;
    void *s;
    void *p;
    if (!a.base) return fail(2, "heap");
    p = cc_arena_alloc(a, 16, 8);
    cp = cc_arena_checkpoint(a);
    if (!p || !cp.arena) return fail(2, "setup");
    s = cc_arena_alloc(a, 200, 8);
    if (!s) return fail(2, "scratch");
    copy = cp;

    /* Non-LIFO: an armed inner pins the outer, through any copy. */
    inner = cc_arena_checkpoint(a);
    if (!inner.arena) return fail(2, "inner");
    if (cc_arena_restore(copy)) return fail(2, "outer copy refused while inner armed");
    if (!cc_arena_restore(inner)) return fail(2, "inner");

    /* Restore, then every stale spelling of the handle refuses. */
    if (!cc_arena_restore(cp)) return fail(2, "restore");
    if (cc_arena_restore(copy)) return fail(2, "by-value copy of a consumed handle refuses");
    if (cc_arena_checkpoint_restore(&copy)) return fail(2, "pointer restore of consumed refuses");
    if (cc_arena_checkpoint_abandon(&copy)) return fail(2, "abandon of consumed refuses");
    cc_arena_checkpoint_destroy(&copy);            /* no-op, nulls */
    if (copy.arena) return fail(2, "destroy nulls a consumed handle");

    /* Scratch pointers are dead: they sit past the popped tip, so a
     * release refuses instead of touching the count. */
    if (cc_arena_release(a, s)) return fail(2, "post-restore scratch release refused");
    if (cc_arena_release_sized(a, s, 200)) return fail(2, "post-restore sized release refused");
    if (!cc_arena_release(a, p)) return fail(2, "pre-checkpoint object still fine");

    /* A forged handle (wrong parent, or a child pointer that is not a live
     * record) refuses without touching anything. */
    memset(&bogus, 0, sizeof(bogus));
    bogus.arena = a.a;
    bogus.parent = a.a;
    if (cc_arena_restore(bogus)) return fail(2, "self-referential handle refused");
    bogus.arena = (CCArenaHost *)(uintptr_t)0x10;
    if (cc_arena_restore(bogus)) return fail(2, "garbage child refused (never dereferenced)");
    bogus.arena = NULL;
    if (cc_arena_restore(bogus)) return fail(2, "unarmed refused");

    /* Reset under an armed handle: the handle goes stale, refuses. */
    cp = cc_arena_checkpoint(a);
    if (!cp.arena) return fail(2, "cp2");
    cc_arena_reset(a);
    if (cc_arena_restore(cp)) return fail(2, "handle stale after reset");
    cc_arena_free(&a);
    printf("  checkpoint misuse OK\n");
    return 0;
}

static int test_release_misuse(void) {
    CCArena a = cc_arena_heap(kilobytes(2));
    CCArena b = cc_arena_heap(kilobytes(2));
    void *p;
    void *q;
    void *foreign;
    void *foreign_block;
    uint8_t stack_bytes[64];
    if (!a.base || !b.base) return fail(3, "heaps");
    p = cc_arena_alloc(a, 32, 8);
    q = cc_arena_alloc(a, 32, 8);
    if (!p || !q) return fail(3, "allocs");
    /* Release peeks at the bytes before a pointer for an overflow header,
     * so a foreign pointer must have readable bytes before it: point into
     * the middle of a heap block, not at its start (TSan's allocator puts
     * the first block at the edge of a mapping). */
    foreign_block = malloc(256);
    if (!foreign_block) return fail(3, "malloc");
    foreign = (uint8_t *)foreign_block + 128;

    if (cc_arena_release(a, foreign)) return fail(3, "foreign refused");
    if (cc_arena_release(b, p)) return fail(3, "wrong arena refused");
    if (cc_arena_release(a, stack_bytes)) return fail(3, "stack pointer refused");
    if (cc_arena_release_sized(a, p, 1000)) return fail(3, "size beyond the tip refused");
    if (cc_arena_release_sized(a, (uint8_t *)q + 8, 64)) return fail(3, "interior pointer with tip-overrunning size refused");
    if (cc_arena_slab_live(a.a) != 2) return fail(3, "refusals leave the count");
    /* Interior pointers inside the live extent cannot be told apart from
     * an allocation by a bump arena; they become holes, never pops. */
    {
        size_t off = cc_arena_slab_offset(a.a);
        if (!cc_arena_release_sized(a, (uint8_t *)q + 8, 8)) return fail(3, "interior hole accepted");
        if (cc_arena_slab_offset(a.a) != off) return fail(3, "interior release never pops");
        if (cc_arena_slab_live(a.a) != 1) return fail(3, "interior counted as a hole");
    }
    if (!cc_arena_release_sized(a, q, 32)) return fail(3, "real tip release");
    if (cc_arena_slab_offset(a.a) != 0) return fail(3, "last live rewinds");
    if (cc_arena_release(a, p)) return fail(3, "nothing live: refused");
    free(foreign_block);
    cc_arena_free(&a);
    cc_arena_free(&b);
    printf("  release misuse OK\n");
    return 0;
}

static int test_token_registry(void) {
    enum { N = 70000 }; /* past one bitmap page */
    uint32_t *toks = (uint32_t *)malloc(N * sizeof(uint32_t));
    size_t i;
    if (!toks) return fail(4, "malloc");
    for (i = 0; i < N; i++) {
        toks[i] = cc_slice_gen_birth();
        if (toks[i] < 16 || toks[i] > CC_SLICE_ID_GEN_MAX) return fail(4, "token range");
    }
    for (i = 0; i < N; i++) {
        if (!cc_slice_gen_is_live(toks[i])) return fail(4, "live");
    }
    for (i = 0; i < N; i += 2) cc_slice_gen_kill(toks[i]);
    for (i = 0; i < N; i++) {
        if (cc_slice_gen_is_live(toks[i]) != (int)(i & 1)) return fail(4, "kill pattern");
    }
    cc_slice_gen_kill(toks[0]);                  /* double kill: harmless */
    if (cc_slice_gen_is_live(0) || cc_slice_gen_is_live(7)) return fail(4, "reserved tokens never live");
    cc_slice_gen_kill(0);
    cc_slice_gen_kill(UINT32_MAX);
    for (i = 1; i < N; i += 2) cc_slice_gen_kill(toks[i]);
    free(toks);
    printf("  token registry OK\n");
    return 0;
}

/* Concurrent container growth on one shared arena with reuse on: the shared
 * alloc / realloc / release paths and the owner header lists are all under
 * the meta lock; every thread must see only its own bytes. */
enum { THREADS = 8, PER = 4000 };
static CCArena g_shared;
static int g_thread_fail[THREADS];

static void *grow_worker(void *arg) {
    int id = (int)(intptr_t)arg;
    U64Vec v = U64Vec_init(g_shared, 2);
    CCString s = cc_string_new();
    size_t i;
    char buf[32];
    if (!v.data) { g_thread_fail[id] = 1; return NULL; }
    for (i = 0; i < PER; i++) {
        if (U64Vec_push(&v, ((uint64_t)id << 32) | i) != 0) { g_thread_fail[id] = 2; return NULL; }
        if ((i % 64) == 0) {
            snprintf(buf, sizeof(buf), "%d:%zu;", id, i);
            if (!cc_string_push_cstr(&s, buf, g_shared)) { g_thread_fail[id] = 3; return NULL; }
        }
    }
    for (i = 0; i < PER; i++) {
        uint64_t *e = U64Vec_get(&v, i);
        if (!e || *e != (((uint64_t)id << 32) | i)) { g_thread_fail[id] = 4; return NULL; }
    }
    snprintf(buf, sizeof(buf), "%d:0;", id);
    if (strncmp(cc_string_cstr(&s, g_shared), buf, strlen(buf)) != 0) { g_thread_fail[id] = 5; return NULL; }
    U64Vec_destroy(&v);
    cc_string_destroy(&s);
    return NULL;
}

/* Reuse + in-place regrow: a block grown at the tip to a non-class size
 * must still be a class-sized range when it is later listed, or the list
 * hands out bytes that overlap whatever was bumped above it. */
static int test_reuse_regrow(void) {
    CCArena a = cc_arena_heap(kilobytes(64));
    uint8_t *x;
    uint8_t *y;
    uint8_t *z;
    if (!a.base) return fail(6, "heap");
    if (!cc_arena_set_reuse(a, true)) return fail(6, "reuse");
    x = (uint8_t *)cc_arena_alloc(a, 16, 8);
    if (!x) return fail(6, "alloc x");
    /* Tip regrow in place to 40 bytes: under reuse this is a 64-byte block. */
    if (cc_arena_realloc(a, a, x, 16, 40, 8) != x) return fail(6, "regrow in place");
    y = (uint8_t *)cc_arena_alloc(a, 16, 8);
    if (!y) return fail(6, "alloc y");
    if (y < x + 64) return fail(6, "y overlaps the regrown class block");
    if (!cc_arena_release_sized(a, x, 40)) return fail(6, "release x");
    z = (uint8_t *)cc_arena_alloc(a, 64, 8);
    if (!z) return fail(6, "alloc z");
    if (z == x && z + 64 > y) return fail(6, "listed block overlaps y");
    if (z != x) return fail(6, "released class block was not re-served");
    cc_arena_free(&a);
    printf("  reuse regrow OK\n");
    return 0;
}

/* Checkpoint churn with owners and reuse: scratch that fits stays a mark,
 * scratch that outgrows the slab promotes, and after every restore the
 * live count is exactly what the mark saw (nothing above it survives, no
 * header from scratch is listed). */
static int test_checkpoint_churn(void) {
    CCArena a = cc_arena_heap(kilobytes(4));
    size_t live0;
    size_t r;
    if (!a.base) return fail(7, "heap");
    if (!cc_arena_set_reuse(a, true)) return fail(7, "reuse");
    {
        U64Vec keep = U64Vec_init(a, 4);
        size_t i;
        for (i = 0; i < 4; i++) U64Vec_push(&keep, i);
        live0 = cc_arena_slab_live(a.a);
        for (r = 0; r < 64; r++) {
            CCArenaCheckpoint cp = cc_arena_checkpoint(a);
            U64Vec v = U64Vec_init(a, 2);
            CCString s = cc_string_new();
            size_t n = (r % 3 == 0) ? 900 : 8; /* 900 x 8 bytes outgrows 4 KiB: promotes */
            if (!cp.arena || !v.data) return fail(7, "churn setup");
            for (i = 0; i < n; i++) {
                if (U64Vec_push(&v, i) != 0) return fail(7, "churn push");
                if ((i & 7) == 0 && !cc_string_push_cstr(&s, "scratch-bytes", a)) return fail(7, "churn string");
            }
            if (n == 900 && !a.a->active) return fail(7, "outgrowing scratch promotes");
            if (n == 8 && a.a->active) return fail(7, "fitting scratch stays a mark");
            for (i = 0; i < n; i++) {
                uint64_t *e = U64Vec_get(&v, i);
                if (!e || *e != i) return fail(7, "scratch vec intact");
            }
            if ((r & 1) == 0) { U64Vec_destroy(&v); cc_string_destroy(&s); } /* else: die with the mark */
            if (!cc_arena_restore(cp)) return fail(7, "churn restore");
            if (a.a->active || a.a->mark_depth) return fail(7, "restore cleared");
            if (cc_arena_slab_live(a.a) != live0) {
                printf("  live=%zu want=%zu round=%zu\n", cc_arena_slab_live(a.a), live0, r);
                return fail(7, "live count back to the mark");
            }
            for (i = 0; i < 4; i++) {
                uint64_t *e = U64Vec_get(&keep, i);
                if (!e || *e != i) return fail(7, "pre-mark vec intact");
            }
        }
        {
            /* No header minted in scratch survives on the owner list. */
            CCArenaOwner *o;
            for (o = a.a->owner_free; o; o = o->next_free) {
                if ((uint8_t *)o >= a.a->slab->base + cc_arena_slab_offset(a.a) &&
                    (uint8_t *)o < a.a->slab->base + a.a->slab->capacity)
                    return fail(7, "scratch header listed");
            }
        }
        U64Vec_destroy(&keep);
    }
    cc_arena_free(&a);
    printf("  checkpoint churn OK\n");
    return 0;
}

static int test_threads(void) {
    pthread_t th[THREADS];
    int i;
    g_shared = cc_arena_heap(kilobytes(64));
    if (!g_shared.base) return fail(5, "shared heap");
    if (!cc_arena_set_reuse(g_shared, true)) return fail(5, "reuse");
    for (i = 0; i < THREADS; i++) {
        if (pthread_create(&th[i], NULL, grow_worker, (void *)(intptr_t)i) != 0) return fail(5, "spawn");
    }
    for (i = 0; i < THREADS; i++) pthread_join(th[i], NULL);
    for (i = 0; i < THREADS; i++) {
        if (g_thread_fail[i]) {
            printf("  thread %d failed at step %d\n", i, g_thread_fail[i]);
            return fail(5, "worker");
        }
    }
    /* Everything released: what stays counted live is exactly the owner
     * headers (threads rebirth each other's listed headers, so there are
     * at most two per thread), the reuse class table, and blocks listed
     * for reuse. Nothing may still be a payload. */
    {
        size_t headers = 0;
        size_t listed = 0;
        unsigned k;
        CCArenaOwner *o;
        for (o = g_shared.a->owner_free; o; o = o->next_free) {
            headers++;
            if (o->token != 0 || o->payload != NULL) return fail(5, "listed header is dead");
        }
        if (headers < 2 || headers > THREADS * 2) return fail(5, "owner header count");
        for (k = 0; k < CC_ARENA_REUSE_CLASSES; k++) {
            void *b;
            for (b = g_shared.a->reuse_free[k]; b; b = *(void **)b) listed++;
        }
        if (cc_arena_live(g_shared) != headers + listed + 1) {
            printf("  live=%zu headers=%zu listed=%zu\n", cc_arena_live(g_shared), headers, listed);
            return fail(5, "no payload left counted live");
        }
    }
    cc_arena_free(&g_shared);
    printf("  threads OK\n");
    return 0;
}

int main(void) {
    int rc;
    if ((rc = test_owner_misuse()) != 0) return rc;
    if ((rc = test_checkpoint_misuse()) != 0) return rc;
    if ((rc = test_release_misuse()) != 0) return rc;
    if ((rc = test_token_registry()) != 0) return rc;
    if ((rc = test_reuse_regrow()) != 0) return rc;
    if ((rc = test_checkpoint_churn()) != 0) return rc;
    if ((rc = test_threads()) != 0) return rc;
    printf("arena_adversarial_smoke OK\n");
    return 0;
}
