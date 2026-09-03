/* Live generation tokens for owners (Vec / heap String / container tables)
 * and the grower views minted from them. `birth` issues a token that stays
 * live until `kill`; `is_live` answers for a bare view id.
 *
 * Storage is a bitmap over the token space, grown one page at a time on
 * demand. Every operation is lock-free: a birth claims its bit with a CAS,
 * a kill clears it with a CAS, a page is published with a CAS (a losing
 * publisher frees its copy). Tokens start at 16 (a CCString tag at or
 * below its inline capacity is never a token). Exhaustion — every token
 * live, or a page allocation failing — returns 0 and never disturbs another
 * live token. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ccc/cc_atomic.cch>
#include <ccc/cc_slice.cch>

enum {
    CC__GEN_MIN = 16,
    CC__GEN_MAX = CC_SLICE_ID_GEN_MAX,
    CC__GEN_PAGE_BITS = 16,                         /* 65536 tokens per page */
    CC__GEN_PAGE_TOKENS = 1u << CC__GEN_PAGE_BITS,
    CC__GEN_PAGE_WORDS = CC__GEN_PAGE_TOKENS / 64,
    CC__GEN_PAGES = (CC__GEN_MAX >> CC__GEN_PAGE_BITS) + 1
};

static cc_atomic_intptr cc__gen_pages[CC__GEN_PAGES];
static cc_atomic_uint cc__gen_next = CC__GEN_MIN;

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
    uint32_t g = cc_atomic_fetch_add(&cc__gen_next, 1u);
    uint32_t tries;
    for (tries = 0; tries <= (uint32_t)(CC__GEN_MAX - CC__GEN_MIN + 1); tries++) {
        cc_atomic_u64 *w;
        uint64_t bit;
        uint64_t cur;
        if (g < CC__GEN_MIN || g > (uint32_t)CC__GEN_MAX) {
            g = CC__GEN_MIN;
            cc_atomic_store(&cc__gen_next, CC__GEN_MIN + 1u);
        }
        w = cc__gen_word(g, 1);
        if (!w) return 0;
        bit = 1ull << (g & 63u);
        cur = cc_atomic_load(w);
        while (!(cur & bit)) {
            if (cc_atomic_cas(w, &cur, cur | bit)) return g;
        }
        g = cc_atomic_fetch_add(&cc__gen_next, 1u);
    }
    return 0;
}

void cc_slice_gen_kill(uint32_t gen) {
    cc_atomic_u64 *w;
    uint64_t bit;
    uint64_t cur;
    if (gen < CC__GEN_MIN || gen > (uint32_t)CC__GEN_MAX) return;
    w = cc__gen_word(gen, 0);
    if (!w) return;
    bit = 1ull << (gen & 63u);
    cur = cc_atomic_load(w);
    while (cur & bit) {
        if (cc_atomic_cas(w, &cur, cur & ~bit)) return;
    }
}

int cc_slice_gen_is_live(uint32_t gen) {
    cc_atomic_u64 *w;
    if (gen < CC__GEN_MIN || gen > (uint32_t)CC__GEN_MAX) return 0;
    w = cc__gen_word(gen, 0);
    if (!w) return 0;
    return ((cc_atomic_load(w) >> (gen & 63u)) & 1u) != 0;
}
