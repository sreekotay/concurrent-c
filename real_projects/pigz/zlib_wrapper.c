/* zlib wrapper - actual implementations using zlib */
#include "zlib_wrapper.h"
#include <zlib.h>
#include <string.h>
#include <stdio.h>

unsigned long zw_crc32(unsigned long crc, const unsigned char *buf, size_t len) {
    return crc32(crc, buf, (uInt)len);
}

unsigned long zw_crc32_combine(unsigned long crc1, unsigned long crc2, size_t len2) {
    return crc32_combine(crc1, crc2, (z_off_t)len2);
}

size_t zw_deflate_block(
    const unsigned char *in, size_t in_len,
    unsigned char *out, size_t out_max,
    const unsigned char *dict, size_t dict_len,
    int level, int last) {
    
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    
    /* Initialize deflate for raw deflate (windowBits = -15 means no zlib/gzip header).
       For parallel gzip, each block is compressed independently. */
    int ret = deflateInit2(&strm, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) return 0;
    
    /* Set dictionary if provided (for better compression across blocks) */
    if (dict && dict_len > 0) {
        deflateSetDictionary(&strm, dict, (uInt)dict_len);
    }
    
    strm.next_in = (Bytef*)in;
    strm.avail_in = (uInt)in_len;
    strm.next_out = out;
    strm.avail_out = (uInt)out_max;
    
    /* Each block is finished completely (Z_FINISH).
       The gzip container handles the block concatenation. */
    ret = deflate(&strm, Z_FINISH);
    
    size_t compressed = strm.total_out;
    deflateEnd(&strm);
    
    if (ret == Z_STREAM_END) {
        return compressed;
    }
    
    return 0;
}
