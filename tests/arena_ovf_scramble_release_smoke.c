/*
 * Many heap-overflow allocs → scramble release → grow survivors → reset.
 * Guards O(1) ovf unlink (doubly-linked list); a singly-linked walk would
 * make this pathological at N≈20k+.
 */
#include <ccc/std/prelude.cch>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define N       20000
#define ROOT    256

static size_t item_size(size_t i) { return 24 + (i * 17) % 200; }

static uint32_t lcg(uint32_t *s) {
    *s = (*s * 1664525u) + 1013904223u;
    return *s;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

int main(void) {
    /* Per-object ovf (durable ctor) — exercises O(1) DLL unlink, not chunk holes. */
    CCArena a = cc_arena_malloc(ROOT);
    unsigned char **ptrs;
    uint32_t *sizes;
    uint8_t *alive;
    size_t *order;
    size_t i, freed = 0, live = 0;
    uint32_t rng = 0xC0FFEEu;
    double t0, mid_ms, grow_ms;
    volatile size_t sink = 0;

    if (!a.base) {
        printf("FAIL: arena\n");
        return 1;
    }

    ptrs = (unsigned char **)calloc(N, sizeof(*ptrs));
    sizes = (uint32_t *)calloc(N, sizeof(*sizes));
    alive = (uint8_t *)calloc(N, 1);
    order = (size_t *)malloc(N * sizeof(size_t));
    if (!ptrs || !sizes || !alive || !order) return 2;

    for (i = 0; i < N; i++) {
        size_t sz = item_size(i);
        ptrs[i] = (unsigned char *)cc_arena_alloc(a, sz, 8);
        if (!ptrs[i]) {
            printf("FAIL: alloc %zu\n", i);
            return 3;
        }
        ptrs[i][0] = (unsigned char)i;
        sizes[i] = (uint32_t)sz;
        alive[i] = 1;
        live++;
    }
    if (!(a.a->_flags & CC_ARENA_FLAG_USED_HEAP_OVERFLOW) || !a.a->ovf_head) {
        printf("FAIL: expected overflow spill (root=%d n=%d)\n", ROOT, N);
        return 4;
    }

    for (i = 0; i < N; i++) order[i] = i;
    for (i = N; i > 1; i--) {
        size_t j = (size_t)(lcg(&rng) % i);
        size_t tmp = order[i - 1];
        order[i - 1] = order[j];
        order[j] = tmp;
    }

    t0 = now_ms();
    for (i = 0; i < N / 2; i++) {
        size_t idx = order[i];
        if (!alive[idx]) continue;
        if (!cc_arena_release(a, ptrs[idx])) {
            printf("FAIL: release %zu\n", idx);
            return 5;
        }
        alive[idx] = 0;
        ptrs[idx] = NULL;
        live--;
        freed++;
    }
    mid_ms = now_ms() - t0;

    t0 = now_ms();
    for (i = 0; i < N; i++) {
        size_t want;
        void *np;
        if (!alive[i]) continue;
        want = (size_t)sizes[i] + 48 + (i % 32);
        np = cc_arena_realloc(a, a, ptrs[i], sizes[i], want, 8);
        if (!np) {
            printf("FAIL: realloc %zu\n", i);
            return 6;
        }
        ptrs[i] = (unsigned char *)np;
        sink ^= ptrs[i][0];
        sizes[i] = (uint32_t)want;
    }
    grow_ms = now_ms() - t0;

    cc_arena_reset(a);
    if (a.a->ovf_head || cc_arena_overflow_raw_bytes(&a) != 0) {
        printf("FAIL: reset left overflow\n");
        return 7;
    }
    cc_arena_free(&a);

    /* Soft bound: with O(1) unlink this should be well under a second on
     * typical hosts; keep a loose ceiling so CI noise does not flake. */
    if (mid_ms + grow_ms > 5000.0) {
        printf("FAIL: midfree+grow too slow (%.1f + %.1f ms) — ovf unlink regress?\n",
               mid_ms, grow_ms);
        return 8;
    }

    printf("arena_ovf_scramble_release_smoke OK "
           "n=%d freed=%zu live_grew=%zu mid_ms=%.3f grow_ms=%.3f sink=%zu\n",
           N, freed, live, mid_ms, grow_ms, (size_t)sink);
    free(ptrs);
    free(sizes);
    free(alive);
    free(order);
    return 0;
}
