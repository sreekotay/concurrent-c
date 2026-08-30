/*
 * HPC Challenge RandomAccess kernel (local form).
 * Stream: LFSR over GF(2), POLY = x^64 + x^2 + x + 1 (HPCC POLY 0x7).
 * Update: T[ran & (TableSize-1)] ^= ran. nupdate = 4 * TableSize.
 * starts(n) is the generator state after n steps from seed 1.
 */
#ifndef RA_H
#define RA_H

#include <stdint.h>
#include <stdio.h>

#define RA_POLY 0x0000000000000007ULL
#define RA_PERIOD 1317624576693539401LL
#define RA_LOOKAHEAD 1024
#define RA_MAX_WORKERS 16
#define RA_MAX_LOCALES 16
#define RA_MIN_BITS 10
#define RA_MAX_BITS 30

static inline uint64_t ra_step(uint64_t ran) {
    return (ran << 1) ^ (((int64_t)ran < 0) ? RA_POLY : 0);
}

static inline uint64_t ra_index(uint64_t ran, int bits) {
    return ran & (((uint64_t)1 << bits) - 1);
}

static inline uint64_t ra_nwords(int bits) {
    return (uint64_t)1 << bits;
}

static inline uint64_t ra_nupdate(int bits) {
    return (uint64_t)4 << bits;
}

/* HPCC HPCC_starts: generator state after n steps. */
static inline uint64_t ra_starts(int64_t n) {
    int i, j;
    uint64_t m2[64];
    uint64_t temp, ran;

    while (n < 0)
        n += RA_PERIOD;
    while (n > RA_PERIOD)
        n -= RA_PERIOD;
    if (n == 0)
        return 0x1;

    temp = 0x1;
    for (i = 0; i < 64; i++) {
        m2[i] = temp;
        temp = ra_step(temp);
        temp = ra_step(temp);
    }

    for (i = 62; i >= 0; i--) {
        if ((n >> i) & 1)
            break;
    }

    ran = 0x2;
    while (i > 0) {
        temp = 0;
        for (j = 0; j < 64; j++) {
            if ((ran >> j) & 1)
                temp ^= m2[j];
        }
        ran = temp;
        i -= 1;
        if ((n >> i) & 1)
            ran = ra_step(ran);
    }
    return ran;
}

static inline int ra_is_pow2(int n) {
    return n > 0 && (n & (n - 1)) == 0;
}

static inline void ra_report(const char* impl, const char* mode, int bits,
                             uint64_t nupdate, double seconds, int workers,
                             uint64_t errors) {
    double gups = (seconds > 0.0) ? ((double)nupdate / seconds / 1.0e9) : 0.0;
    double frac = (nupdate > 0) ? ((double)errors / (double)nupdate) : 0.0;
    printf("gups impl=%s mode=%s table_bits=%d nupdate=%llu seconds=%.6f "
           "workers=%d errors=%llu error_frac=%.6f gups=%.6f\n",
           impl, mode, bits, (unsigned long long)nupdate, seconds, workers,
           (unsigned long long)errors, frac, gups);
}

#endif
