/* zlib wrapper - avoids TCC include path issues */
#ifndef ZLIB_WRAPPER_H
#define ZLIB_WRAPPER_H

#include <stddef.h>

/* CRC functions */
unsigned long zw_crc32(unsigned long crc, const unsigned char *buf, size_t len);
unsigned long zw_crc32_combine(unsigned long crc1, unsigned long crc2, size_t len2);

/* Compression - returns compressed size, or 0 on error */
size_t zw_deflate_block(
    const unsigned char *in, size_t in_len,
    unsigned char *out, size_t out_max,
    const unsigned char *dict, size_t dict_len,
    int level, int last
);

#endif
