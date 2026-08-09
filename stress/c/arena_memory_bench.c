/*
 * C peer for stress/compare_arena_memory.sh
 *
 * Fair protocol (same env knobs as CC/Go/Zig):
 *   bump arena, block_max=ARENA_MEM_BLOCK_MAX (default 4), then malloc overflow;
 *   tip growth; storm+reset; arena churn.
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

static size_t sample_rss(void) {
#if defined(__APPLE__)
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        return (size_t)info.phys_footprint;
#elif defined(__linux__)
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long kb = 0;
        if (sscanf(line, "VmRSS: %lu", &kb) == 1) {
            fclose(f);
            return (size_t)kb * 1024;
        }
    }
    fclose(f);
#endif
    return 0;
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

/* Tip realloc: extend in place when ptr is at tip of current slab. */
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

int main(void) {
    size_t tip_iters = env_zu("ARENA_MEM_TIP_ITERS", 20000);
    size_t tip_root = env_zu("ARENA_MEM_TIP_ROOT", 4u * 1024u * 1024u);
    size_t storm_n = env_zu("ARENA_MEM_STORM", 50000);
    size_t storm_root = env_zu("ARENA_MEM_ROOT_STORM", 4096);
    size_t churn_n = env_zu("ARENA_MEM_CHURN", 200);
    size_t churn_each = env_zu("ARENA_MEM_CHURN_EACH", 64);
    int block_max = (int)env_zu("ARENA_MEM_BLOCK_MAX", 4);
    Bump tip, storm;
    size_t i, moves = 0, sz = 0;
    unsigned char *p = NULL;
    double t0, t1, tip_ms, storm_ms, churn_ms;
    size_t storm_gross = 0, storm_ovf = 0, reset_gross = 0;
    size_t rss0, rss1;
    size_t j;

    printf("arena_memory_bench(c): block_max=%d tip_iters=%zu storm=%zu\n",
           block_max, tip_iters, storm_n);

    if (bump_init(&tip, tip_root, block_max) != 0) return 1;
    t0 = now_ms();
    for (i = 0; i < tip_iters; i++) {
        size_t want = sz + 32 + (i % 17);
        void *np = bump_realloc(&tip, p, sz, want, 8, &moves);
        if (!np) { bump_free(&tip); return 2; }
        p = (unsigned char *)np;
        sz = want;
    }
    tip_ms = now_ms() - t0;
    bump_free(&tip);

    rss0 = sample_rss();
    if (bump_init(&storm, storm_root, block_max) != 0) return 3;
    t0 = now_ms();
    for (i = 0; i < storm_n; i++) {
        size_t n = 24 + (i * 17) % 512;
        void *q = bump_alloc(&storm, n, 8);
        if (!q) { bump_free(&storm); return 4; }
        memset(q, (int)(i & 0xff), n < 16 ? n : 16);
    }
    storm_ms = now_ms() - t0;
    storm_gross = bump_gross(&storm);
    storm_ovf = storm.ovf_bytes;
    bump_reset(&storm);
    reset_gross = bump_gross(&storm);
    bump_free(&storm);
    rss1 = sample_rss();

    t0 = now_ms();
    for (i = 0; i < churn_n; i++) {
        Bump c;
        if (bump_init(&c, 8 * 1024, block_max) != 0) return 5;
        for (j = 0; j < churn_each; j++) {
            size_t n = 16 + (j * 31) % 256;
            if (!bump_alloc(&c, n, 8)) { bump_free(&c); return 6; }
        }
        bump_free(&c);
    }
    churn_ms = now_ms() - t0;

    printf("RESULT lang=c tip_ms=%.3f tip_moves=%zu storm_ms=%.3f storm_gross=%zu "
           "storm_ovf=%zu reset_gross=%zu churn_ms=%.3f rss_delta=%zd\n",
           tip_ms, moves, storm_ms, storm_gross, storm_ovf, reset_gross, churn_ms,
           (ssize_t)rss1 - (ssize_t)rss0);
    return 0;
}
