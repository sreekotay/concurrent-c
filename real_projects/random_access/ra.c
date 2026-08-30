/*
 * Sequential C reference for HPC Challenge RandomAccess (local kernel).
 *
 *   TABLE_BITS   power-of-two table size (default 26; RA_SMOKE=1 → 16)
 *   RA_SMOKE=1   small table for compare.sh --smoke
 */
#include "ra.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int env_truth(const char* k) {
    const char* s = getenv(k);
    if (!s || !s[0] || strcmp(s, "0") == 0)
        return 0;
    return 1;
}

static int env_int(const char* k, int def) {
    const char* s = getenv(k);
    if (!s || !s[0])
        return def;
    return atoi(s);
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1.0e-9;
}

static void apply_range(uint64_t* words, int bits, uint64_t lo, uint64_t hi) {
    uint64_t ran = ra_starts((int64_t)lo);
    uint64_t i;
    for (i = lo; i < hi; i++) {
        ran = ra_step(ran);
        words[ra_index(ran, bits)] ^= ran;
    }
}

int main(void) {
    int bits = env_int("TABLE_BITS", env_truth("RA_SMOKE") ? 16 : 26);
    uint64_t nwords;
    uint64_t nupdate;
    uint64_t* words;
    uint64_t i;
    uint64_t errors;
    double t0, t1;

    if (bits < RA_MIN_BITS || bits > RA_MAX_BITS) {
        fprintf(stderr, "TABLE_BITS must be %d..%d\n", RA_MIN_BITS, RA_MAX_BITS);
        return 2;
    }

    nwords = ra_nwords(bits);
    nupdate = ra_nupdate(bits);
    words = (uint64_t*)malloc((size_t)nwords * sizeof(uint64_t));
    if (!words) {
        fprintf(stderr, "Table_create: out of memory\n");
        return 1;
    }
    for (i = 0; i < nwords; i++)
        words[i] = i;

    t0 = now_s();
    apply_range(words, bits, 0, nupdate);
    t1 = now_s();

    apply_range(words, bits, 0, nupdate);
    errors = 0;
    for (i = 0; i < nwords; i++) {
        if (words[i] != i)
            errors++;
    }
    free(words);

    ra_report("c", "seq", bits, nupdate, t1 - t0, 1, errors);
    return errors == 0 ? 0 : 1;
}
