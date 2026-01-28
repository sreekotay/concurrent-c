/* zlib wrapper - implementations using zlib for parallel gzip */
#include "zlib_wrapper.h"
#include <zlib.h>
#include <string.h>

unsigned long zw_crc32(unsigned long crc, const unsigned char *buf, size_t len) {
    return crc32(crc, buf, (uInt)len);
}

unsigned long zw_crc32_combine(unsigned long crc1, unsigned long crc2, size_t len2) {
    return crc32_combine(crc1, crc2, (z_off_t)len2);
}

/*
 * Compress a block as a complete gzip member.
 * 
 * For parallel gzip, each block is output as a complete gzip member
 * (header + compressed data + trailer). Multiple gzip members can be
 * concatenated, and gunzip will decompress them all. This is the
 * simplest way to achieve valid parallel gzip output.
 *
 * Output format:
 *   [10-byte gzip header][deflate data][8-byte trailer (crc32 + size)]
 */
size_t zw_deflate_block(
    const unsigned char *in, size_t in_len,
    unsigned char *out, size_t out_max,
    const unsigned char *dict, size_t dict_len,
    int level, int last) {
    
    (void)dict;      /* Dictionary not used with independent gzip members */
    (void)dict_len;
    (void)last;      /* Each block is a complete member */
    
    /* Need at least 18 bytes for header(10) + trailer(8) plus some data */
    if (out_max < 32) return 0;
    
    /* Write gzip header */
    unsigned char header[10] = {
        0x1f, 0x8b,  /* Magic */
        0x08,        /* Deflate method */
        0x00,        /* Flags (none) */
        0, 0, 0, 0,  /* Mtime (not set) */
        0x00,        /* Extra flags */
        0x03         /* OS = Unix */
    };
    memcpy(out, header, 10);
    size_t pos = 10;
    
    /* Compress with zlib (raw deflate) */
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    
    int ret = deflateInit2(&strm, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) return 0;
    
    strm.next_in = (Bytef*)in;
    strm.avail_in = (uInt)in_len;
    strm.next_out = out + pos;
    strm.avail_out = (uInt)(out_max - pos - 8);  /* Reserve space for trailer */
    
    ret = deflate(&strm, Z_FINISH);
    size_t deflate_len = strm.total_out;
    deflateEnd(&strm);
    
    if (ret != Z_STREAM_END) return 0;
    
    pos += deflate_len;
    
    /* Calculate CRC32 of uncompressed data */
    unsigned long crc = crc32(0L, in, (uInt)in_len);
    
    /* Write trailer: CRC32 (4 bytes) + original size mod 2^32 (4 bytes) */
    out[pos++] = crc & 0xff;
    out[pos++] = (crc >> 8) & 0xff;
    out[pos++] = (crc >> 16) & 0xff;
    out[pos++] = (crc >> 24) & 0xff;
    out[pos++] = in_len & 0xff;
    out[pos++] = (in_len >> 8) & 0xff;
    out[pos++] = (in_len >> 16) & 0xff;
    out[pos++] = (in_len >> 24) & 0xff;
    
    return pos;
}
