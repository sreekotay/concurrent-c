/* Live generation tokens for owners (Vec / heap String / container tables)
 * and the grower views minted from them. `birth` issues a token that stays
 * live until `kill`; `is_live` answers for a bare view id.
 *
 * Storage is a bitmap over the token space, grown one page at a time on
 * demand. Births are batched per thread: a thread claims one 64-token word
 * at a time (one fetch_add on the shared cursor, one CAS that sets every
 * free bit of that word), then hands those tokens out with no shared
 * access at all. A kill clears its bit with one fetch_and; a page is published with
 * a CAS (a losing publisher frees its copy). A claimed token that is never
 * handed out stays set (at most 63 per thread that exits mid-word); it is
 * never queried, since only handed-out tokens reach is_live. Tokens start
 * at 64 (a CCString tag at or below its inline capacity is never a token).
 * Exhaustion — every token live, or a page allocation failing — returns 0
 * and never disturbs another live token. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ccc/cc_atomic.cch>
#include <ccc/cc_slice.cch>

enum {
    CC__GEN_MIN = 16,
    CC__GEN_WORD0 = 64,                             /* first claimable word */
    CC__GEN_MAX = CC_SLICE_ID_GEN_MAX,
    CC__GEN_PAGE_BITS = 16,                         /* 65536 tokens per page */
    CC__GEN_PAGE_TOKENS = 1u << CC__GEN_PAGE_BITS,
    CC__GEN_PAGE_WORDS = CC__GEN_PAGE_TOKENS / 64,
    CC__GEN_PAGES = (CC__GEN_MAX >> CC__GEN_PAGE_BITS) + 1
};

static cc_atomic_intptr cc__gen_pages[CC__GEN_PAGES];
static cc_atomic_uint cc__gen_next = CC__GEN_WORD0;
static cc_atomic_u64 *cc__gen_word(uint32_t g, int create);

/* This thread's claimed word: the tokens still to hand out (`mine`) above
 * the word's first token (`base`, a multiple of 64). */
static __thread uint64_t cc__gen_tl_mine = 0;
static __thread uint32_t cc__gen_tl_base = 0;

/* Claim the free bits of the next word for this thread. 0 when no page. */
static int cc__gen_claim(void) {
    uint32_t base = cc_atomic_fetch_add(&cc__gen_next, 64u);
    cc_atomic_u64 *w;
    uint64_t cur;
    uint64_t take;
    if (base < CC__GEN_WORD0 || base > (uint32_t)CC__GEN_MAX - 63u) {
        /* Wrapped: start over at the first word; live bits are skipped. */
        base = CC__GEN_WORD0;
        cc_atomic_store(&cc__gen_next, CC__GEN_WORD0 + 64u);
    }
    w = cc__gen_word(base, 1);
    if (!w) return 0;
    cur = cc_atomic_load(w);
    for (;;) {
        take = ~cur;
        if (take == 0) { cc__gen_tl_mine = 0; cc__gen_tl_base = base; return 1; }
        if (cc_atomic_cas(w, &cur, cur | take)) break;
    }
    cc__gen_tl_mine = take;
    cc__gen_tl_base = base;
    return 1;
}

static cc_atomic_u64 *cc__gen_word(uint32_t g, int create) {
    uint32_t page = g >> CC__GEN_PAGE_BITS;
    cc_atomic_u64 *p;
    if (page >= (uint32_t)CC__GEN_PAGES) return NULL;
    p = (cc_atomic_u64 *)cc_atomic_load(&cc__gen_pages[page]);
    if (!p) {
        intptr_t expected = 0;
        cc_atomic_u64 *fresh;
        if (!create) return NULL;
        fresh = (cc_atomic_u64 *)calloc(CC__GEN_PAGE_WORDS, sizeof(cc_atomic_u64));
        if (!fresh) return NULL;
        if (cc_atomic_cas(&cc__gen_pages[page], &expected, (intptr_t)fresh)) {
            p = fresh;
        } else {
            free(fresh);
            p = (cc_atomic_u64 *)expected;
        }
    }
    return &p[(g & (CC__GEN_PAGE_TOKENS - 1u)) / 64u];
}

uint32_t cc_slice_gen_birth(void) {
    uint32_t tries;
    /* Each try claims one word; a word can be fully live after a wrap. */
    for (tries = 0; tries <= (uint32_t)(CC__GEN_MAX / 64u) + 1u; tries++) {
        uint64_t m = cc__gen_tl_mine;
        if (m) {
            unsigned k = (unsigned)__builtin_ctzll(m);
            cc__gen_tl_mine = m & (m - 1);
            return cc__gen_tl_base + k;
        }
        if (!cc__gen_claim()) return 0;
    }
    return 0;
}

void cc_slice_gen_kill(uint32_t gen) {
    cc_atomic_u64 *w;
    uint64_t bit;
    if (gen < CC__GEN_MIN || gen > (uint32_t)CC__GEN_MAX) return;
    w = cc__gen_word(gen, 0);
    if (!w) return;
    bit = 1ull << (gen & 63u);
    (void)cc_atomic_fetch_and(w, ~bit);
}

int cc_slice_gen_is_live(uint32_t gen) {
    cc_atomic_u64 *w;
    if (gen < CC__GEN_MIN || gen > (uint32_t)CC__GEN_MAX) return 0;
    w = cc__gen_word(gen, 0);
    if (!w) return 0;
    return ((cc_atomic_load(w) >> (gen & 63u)) & 1u) != 0;
}
