#include <zlib.h>
#include <stdio.h>

#define BASE 65521U
#define LOW16 0xffff

static unsigned long adler32_comb(unsigned long adler1, unsigned long adler2, size_t len2) {
    unsigned long sum1;
    unsigned long sum2;
    unsigned rem;

    rem = (unsigned)(len2 % BASE);
    sum1 = adler1 & LOW16;
    sum2 = (rem * sum1) % BASE;
    sum1 += (adler2 & LOW16) + BASE - 1;
    sum2 += ((adler1 >> 16) & LOW16) + ((adler2 >> 16) & LOW16) + BASE - rem;
    if (sum1 >= BASE) sum1 -= BASE;
    if (sum1 >= BASE) sum1 -= BASE;
    if (sum2 >= (BASE << 1)) sum2 -= (BASE << 1);
    if (sum2 >= BASE) sum2 -= BASE;
    return sum1 | (sum2 << 16);
}

int main() {
    const char *A = "Hello ";
    const char *B = "World";
    unsigned long adlerA = adler32(1L, (const Bytef*)A, 6);
    unsigned long adlerB = adler32(1L, (const Bytef*)B, 5);
    unsigned long adlerAB_manual = adler32_comb(adlerA, adlerB, 5);
    unsigned long adlerAB_real = adler32(1L, (const Bytef*)"Hello World", 11);
    printf("Manual: %lu\nReal:   %lu\n", adlerAB_manual, adlerAB_real);
    return 0;
}
