/* Live generation tokens for grower-minted slices (Vec / heap String).
 * as_slice captures the token; realloc that moves the backing kills it. */
#include <stdint.h>

enum { CC__SLICE_GEN_CAP = 4096 };

static uint32_t cc__slice_gen_tab[CC__SLICE_GEN_CAP];
static uint32_t cc__slice_gen_next = 1;

static unsigned cc__slice_gen_slot(uint32_t g) {
    return (unsigned)(g & (CC__SLICE_GEN_CAP - 1u));
}

uint32_t cc_slice_gen_birth(void) {
    uint32_t g, i, s;
    g = cc__slice_gen_next++;
    if (g == 0 || g > 0x7FFFFu) {
        cc__slice_gen_next = 2;
        g = 1;
    }
    s = cc__slice_gen_slot(g);
    for (i = 0; i < (uint32_t)CC__SLICE_GEN_CAP; i++) {
        unsigned k = (s + i) & (CC__SLICE_GEN_CAP - 1u);
        if (cc__slice_gen_tab[k] == 0) {
            cc__slice_gen_tab[k] = g;
            return g;
        }
    }
    cc__slice_gen_tab[s] = g;
    return g;
}

void cc_slice_gen_kill(uint32_t gen) {
    uint32_t i, s;
    if (!gen) return;
    s = cc__slice_gen_slot(gen);
    for (i = 0; i < (uint32_t)CC__SLICE_GEN_CAP; i++) {
        unsigned k = (s + i) & (CC__SLICE_GEN_CAP - 1u);
        if (cc__slice_gen_tab[k] == gen) {
            cc__slice_gen_tab[k] = 0;
            return;
        }
        if (cc__slice_gen_tab[k] == 0) return;
    }
}

int cc_slice_gen_is_live(uint32_t gen) {
    uint32_t i, s;
    if (!gen) return 0;
    s = cc__slice_gen_slot(gen);
    for (i = 0; i < (uint32_t)CC__SLICE_GEN_CAP; i++) {
        unsigned k = (s + i) & (CC__SLICE_GEN_CAP - 1u);
        if (cc__slice_gen_tab[k] == gen) return 1;
        if (cc__slice_gen_tab[k] == 0) return 0;
    }
    return 0;
}
