/* Token registry: births are unique across threads, live until killed, dead
 * after, and rebirth keeps working after mass kills. Tokens are at least 64
 * (never a CCString inline tag). */
#include <ccc/cc_slice.cch>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { THREADS = 4, PER = 20000 };
static uint32_t toks[THREADS][PER];

static void *worker(void *p) {
    uint32_t *out = (uint32_t *)p;
    int i;
    for (i = 0; i < PER; i++) {
        out[i] = cc_slice_gen_birth();
        if (out[i] == 0) { fprintf(stderr, "birth returned 0\n"); exit(2); }
    }
    return NULL;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

int main(void) {
    pthread_t th[THREADS];
    static uint32_t all[THREADS * PER];
    int t, i;
    for (t = 0; t < THREADS; t++) pthread_create(&th[t], NULL, worker, toks[t]);
    for (t = 0; t < THREADS; t++) pthread_join(th[t], NULL);
    for (t = 0; t < THREADS; t++) memcpy(all + t * PER, toks[t], sizeof(toks[t]));
    qsort(all, THREADS * PER, sizeof(uint32_t), cmp_u32);
    if (all[0] < 64) { printf("FAIL: token below 64\n"); return 1; }
    for (i = 1; i < THREADS * PER; i++) {
        if (all[i] == all[i - 1]) { printf("FAIL: duplicate token %u\n", all[i]); return 1; }
    }
    for (i = 0; i < THREADS * PER; i++) {
        if (!cc_slice_gen_is_live(all[i])) { printf("FAIL: born token not live\n"); return 1; }
    }
    for (i = 0; i < THREADS * PER; i += 2) cc_slice_gen_kill(all[i]);
    for (i = 0; i < THREADS * PER; i++) {
        int live = cc_slice_gen_is_live(all[i]);
        if ((i & 1) == 0 && live) { printf("FAIL: killed token still live\n"); return 1; }
        if ((i & 1) == 1 && !live) { printf("FAIL: neighbour token died with it\n"); return 1; }
    }
    for (i = 0; i < 1000; i++) {
        uint32_t g = cc_slice_gen_birth();
        if (!g || !cc_slice_gen_is_live(g)) { printf("FAIL: rebirth\n"); return 1; }
        cc_slice_gen_kill(g);
        if (cc_slice_gen_is_live(g)) { printf("FAIL: kill after rebirth\n"); return 1; }
    }
    printf("slice_gen_batch_smoke: OK\n");
    return 0;
}
