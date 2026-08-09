/*
 * Raw malloc/realloc baseline for stress/compare_arena_memory.sh.
 *
 * Same size streams as the bump peers, but every allocation is libc malloc
 * (tip uses realloc). Answers "is plain malloc faster on this workload?"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

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

static size_t bulk_size(size_t i) {
    return 16 + (i * 17) % 240;
}

int main(void) {
    size_t tip_rounds = env_zu("ARENA_MEM_TIP_ROUNDS", 400);
    size_t tip_steps = env_zu("ARENA_MEM_TIP_STEPS", 5000);
    size_t bulk_n = env_zu("ARENA_MEM_BULK", 2000000);
    size_t ovf_n = env_zu("ARENA_MEM_OVF", 200000);
    size_t churn_n = env_zu("ARENA_MEM_CHURN", 1000);
    size_t churn_each = env_zu("ARENA_MEM_CHURN_EACH", 128);
    size_t i, j, r, moves = 0, sz;
    unsigned char *p;
    unsigned char **ptrs;
    double t0, tip_ms, bulk_ms, ovf_ms, reset_ms, churn_ms;
    size_t bulk_gross = 0, ovf_gross = 0;
    volatile size_t sink = 0;

    printf("arena_memory_bench(malloc): tip=%zux%zu bulk=%zu ovf=%zu\n",
           tip_rounds, tip_steps, bulk_n, ovf_n);

    t0 = now_ms();
    for (r = 0; r < tip_rounds; r++) {
        p = NULL;
        sz = 0;
        for (i = 0; i < tip_steps; i++) {
            size_t want = sz + 16 + (i % 17);
            unsigned char *np = (unsigned char *)realloc(p, want);
            if (!np) { free(p); return 2; }
            if (p && np != p) moves++;
            p = np;
            p[sz] = (unsigned char)i;
            sz = want;
        }
        sink ^= (size_t)p[0] + sz;
        free(p);
    }
    tip_ms = now_ms() - t0;

    /* bulk_ms / ovf_ms = malloc-all + free-all (full ownership). */
    ptrs = (unsigned char **)malloc(bulk_n * sizeof(*ptrs));
    if (!ptrs) return 3;
    t0 = now_ms();
    for (i = 0; i < bulk_n; i++) {
        size_t n = bulk_size(i);
        ptrs[i] = (unsigned char *)malloc(n);
        if (!ptrs[i]) return 4;
        ptrs[i][0] = (unsigned char)i;
        sink ^= ptrs[i][0];
        bulk_gross += n;
    }
    {
        double t_free = now_ms();
        for (i = 0; i < bulk_n; i++) free(ptrs[i]);
        reset_ms = now_ms() - t_free;
    }
    bulk_ms = now_ms() - t0;
    free(ptrs);

    ptrs = (unsigned char **)malloc(ovf_n * sizeof(*ptrs));
    if (!ptrs) return 5;
    t0 = now_ms();
    for (i = 0; i < ovf_n; i++) {
        size_t n = 24 + (i * 17) % 512;
        ptrs[i] = (unsigned char *)malloc(n);
        if (!ptrs[i]) return 6;
        ptrs[i][0] = (unsigned char)i;
        sink ^= ptrs[i][0];
        ovf_gross += n;
    }
    for (i = 0; i < ovf_n; i++) free(ptrs[i]);
    ovf_ms = now_ms() - t0;
    free(ptrs);

    t0 = now_ms();
    for (i = 0; i < churn_n; i++) {
        for (j = 0; j < churn_each; j++) {
            size_t n = 16 + (j * 31) % 256;
            unsigned char *q = (unsigned char *)malloc(n);
            if (!q) return 8;
            q[0] = (unsigned char)j;
            sink ^= q[0];
            free(q);
        }
    }
    churn_ms = now_ms() - t0;

    /* bulk_ovf/ovf_bytes: N/A for pure malloc — report 0 / live totals as gross. */
    printf("RESULT lang=malloc tip_ms=%.3f tip_moves=%zu bulk_ms=%.3f bulk_gross=%zu bulk_ovf=0 "
           "ovf_ms=%.3f ovf_gross=%zu ovf_bytes=0 reset_ms=%.3f churn_ms=%.3f sink=%zu\n",
           tip_ms, moves, bulk_ms, bulk_gross,
           ovf_ms, ovf_gross, reset_ms, churn_ms, (size_t)sink);
    return 0;
}
