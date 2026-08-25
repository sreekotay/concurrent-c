/*
 * C peer for stress/compare_arena_memory.sh — fair bump arena protocol.
 *
 * Phases (env knobs):
 *   tip   — tip-grow rounds including reset (full ownership)
 *   bulk  — large-root allocs + reset (bulk_ms); reset_ms is reclaim split
 *   ovf   — tiny-root spill + free/drain (full ownership)
 *   churn — create/fill/destroy arenas
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/types.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

typedef struct Slab {
    uint8_t *base;
    size_t cap;
    size_t off;
    struct Slab *prev;
} Slab;

typedef struct {
    Slab *cur;
    int nslabs;
    int block_max;
    void **ovf;
    size_t ovf_n;
    size_t ovf_cap;
    size_t ovf_bytes;
    size_t live;
} Bump;

static size_t env_zu(const char *k, size_t d) {
    const char *v = getenv(k);
    if (!v || !*v) return d;
    return (size_t)strtoull(v, NULL, 10);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

static size_t align_up(size_t v, size_t a) {
    return (v + a - 1) & ~(a - 1);
}

static int bump_init(Bump *b, size_t root, int block_max) {
    Slab *s = (Slab *)calloc(1, sizeof(Slab));
    if (!s) return -1;
    s->base = (uint8_t *)malloc(root);
    if (!s->base) { free(s); return -1; }
    s->cap = root;
    b->cur = s;
    b->nslabs = 1;
    b->block_max = block_max;
    b->ovf = NULL;
    b->ovf_n = b->ovf_cap = b->ovf_bytes = b->live = 0;
    return 0;
}

static void bump_reset(Bump *b) {
    size_t i;
    Slab *s;
    for (i = 0; i < b->ovf_n; i++) free(b->ovf[i]);
    free(b->ovf);
    b->ovf = NULL;
    b->ovf_n = b->ovf_cap = b->ovf_bytes = 0;
    while (b->cur && b->cur->prev) {
        s = b->cur;
        b->cur = s->prev;
        free(s->base);
        free(s);
        b->nslabs--;
    }
    if (b->cur) b->cur->off = 0;
    b->live = 0;
}

static void bump_free(Bump *b) {
    bump_reset(b);
    if (b->cur) {
        free(b->cur->base);
        free(b->cur);
        b->cur = NULL;
    }
    b->nslabs = 0;
}

static int bump_grow(Bump *b, size_t need) {
    Slab *ns;
    size_t cap;
    if (b->block_max > 0 && b->nslabs >= b->block_max) return -1;
    cap = b->cur->cap + b->cur->cap / 2;
    if (cap < need) cap = need;
    if (cap < 4096) cap = 4096;
    ns = (Slab *)calloc(1, sizeof(Slab));
    if (!ns) return -1;
    ns->base = (uint8_t *)malloc(cap);
    if (!ns->base) { free(ns); return -1; }
    ns->cap = cap;
    ns->prev = b->cur;
    b->cur = ns;
    b->nslabs++;
    return 0;
}

static void *bump_alloc(Bump *b, size_t n, size_t align) {
    size_t off, end;
    void *p;
    if (!b->cur || n == 0) return NULL;
    off = align_up(b->cur->off, align);
    end = off + n;
    if (end > b->cur->cap) {
        if (bump_grow(b, n + align) == 0) {
            off = align_up(b->cur->off, align);
            end = off + n;
        } else {
            if (b->ovf_n == b->ovf_cap) {
                size_t nc = b->ovf_cap ? b->ovf_cap * 2 : 64;
                void **nb = (void **)realloc(b->ovf, nc * sizeof(void *));
                if (!nb) return NULL;
                b->ovf = nb;
                b->ovf_cap = nc;
            }
            p = malloc(n);
            if (!p) return NULL;
            b->ovf[b->ovf_n++] = p;
            b->ovf_bytes += n;
            b->live += n;
            return p;
        }
    }
    p = b->cur->base + off;
    b->cur->off = end;
    b->live += n;
    return p;
}

static void *bump_realloc(Bump *b, void *ptr, size_t old_n, size_t new_n, size_t align, size_t *moves) {
    uint8_t *bp = (uint8_t *)ptr;
    if (!ptr) return bump_alloc(b, new_n, align);
    if (new_n == 0) return NULL;
    if (bp >= b->cur->base && bp < b->cur->base + b->cur->cap) {
        size_t poff = (size_t)(bp - b->cur->base);
        if (poff + old_n == b->cur->off) {
            size_t end = poff + new_n;
            if (new_n <= old_n || end <= b->cur->cap) {
                b->cur->off = end;
                b->live = b->live - old_n + new_n;
                return ptr;
            }
        }
    }
    {
        void *np = bump_alloc(b, new_n, align);
        if (!np) return NULL;
        memcpy(np, ptr, old_n < new_n ? old_n : new_n);
        if (moves) (*moves)++;
        return np;
    }
}

static size_t bump_gross(const Bump *b) {
    size_t g = b->ovf_bytes;
    const Slab *s = b->cur;
    while (s) {
        g += s->cap;
        s = s->prev;
    }
    return g;
}

static size_t bulk_size(size_t i) {
    return 16 + (i * 17) % 240;
}

int main(void) {
    size_t tip_rounds = env_zu("ARENA_MEM_TIP_ROUNDS", 400);
    size_t tip_steps = env_zu("ARENA_MEM_TIP_STEPS", 5000);
    size_t tip_root = env_zu("ARENA_MEM_TIP_ROOT", 8u * 1024u * 1024u);
    size_t bulk_n = env_zu("ARENA_MEM_BULK", 2000000);
    size_t bulk_root = env_zu("ARENA_MEM_BULK_ROOT", 64u * 1024u * 1024u);
    size_t ovf_n = env_zu("ARENA_MEM_OVF", 200000);
    size_t ovf_root = env_zu("ARENA_MEM_OVF_ROOT", 4096);
    size_t churn_n = env_zu("ARENA_MEM_CHURN", 1000);
    size_t churn_each = env_zu("ARENA_MEM_CHURN_EACH", 128);
    int block_max = (int)env_zu("ARENA_MEM_BLOCK_MAX", 4);
    Bump a;
    size_t i, j, r, moves = 0, sz;
    unsigned char *p;
    double t0, tip_ms, bulk_ms, ovf_ms, reset_ms, churn_ms;
    size_t bulk_gross = 0, bulk_ovf = 0, ovf_gross = 0, ovf_bytes = 0;
    volatile size_t sink = 0;

    printf("arena_memory_bench(c): tip=%zux%zu bulk=%zu ovf=%zu block_max=%d\n",
           tip_rounds, tip_steps, bulk_n, ovf_n, block_max);

    if (bump_init(a, tip_root, block_max) != 0) return 1;
    t0 = now_ms();
    for (r = 0; r < tip_rounds; r++) {
        p = NULL;
        sz = 0;
        for (i = 0; i < tip_steps; i++) {
            size_t want = sz + 16 + (i % 17);
            void *np = bump_realloc(&a, p, sz, want, 8, &moves);
            if (!np) { bump_free(&a); return 2; }
            p = (unsigned char *)np;
            p[sz] = (unsigned char)i;
            sz = want;
        }
        sink ^= (size_t)p[0] + sz;
        bump_reset(&a);
    }
    bump_free(&a);
    tip_ms = now_ms() - t0;

    /* bulk_ms / ovf_ms are full ownership: allocate + reclaim. */
    if (bump_init(a, bulk_root, block_max) != 0) return 3;
    t0 = now_ms();
    for (i = 0; i < bulk_n; i++) {
        size_t n = bulk_size(i);
        unsigned char *q = (unsigned char *)bump_alloc(&a, n, 8);
        if (!q) { bump_free(&a); return 4; }
        q[0] = (unsigned char)i;
        sink ^= q[0];
    }
    bulk_gross = bump_gross(&a);
    bulk_ovf = a.ovf_bytes;
    {
        double t_reset = now_ms();
        bump_reset(&a);
        reset_ms = now_ms() - t_reset;
    }
    bump_free(&a);
    bulk_ms = now_ms() - t0;

    if (bump_init(a, ovf_root, block_max) != 0) return 5;
    t0 = now_ms();
    for (i = 0; i < ovf_n; i++) {
        size_t n = 24 + (i * 17) % 512;
        unsigned char *q = (unsigned char *)bump_alloc(&a, n, 8);
        if (!q) { bump_free(&a); return 6; }
        q[0] = (unsigned char)i;
        sink ^= q[0];
    }
    ovf_gross = bump_gross(&a);
    ovf_bytes = a.ovf_bytes;
    bump_free(&a); /* reclaim extents + overflow inside ovf_ms */
    ovf_ms = now_ms() - t0;

    t0 = now_ms();
    for (i = 0; i < churn_n; i++) {
        Bump c;
        if (bump_init(&c, 16 * 1024, block_max) != 0) return 7;
        for (j = 0; j < churn_each; j++) {
            size_t n = 16 + (j * 31) % 256;
            if (!bump_alloc(&c, n, 8)) { bump_free(&c); return 8; }
        }
        bump_free(&c);
    }
    churn_ms = now_ms() - t0;

    printf("RESULT lang=c block_max=%d tip_ms=%.3f tip_moves=%zu bulk_ms=%.3f bulk_gross=%zu bulk_ovf=%zu "
           "ovf_ms=%.3f ovf_gross=%zu ovf_bytes=%zu reset_ms=%.3f churn_ms=%.3f sink=%zu\n",
           block_max, tip_ms, moves, bulk_ms, bulk_gross, bulk_ovf,
           ovf_ms, ovf_gross, ovf_bytes, reset_ms, churn_ms, (size_t)sink);
    return 0;
}
