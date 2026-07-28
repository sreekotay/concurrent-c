#ifndef CC_EMIT_LIMITS_H
#define CC_EMIT_LIMITS_H

/* Shared comptime emit / generic lowering buffer limits (hard errors on overflow).
 * Generic factories (base + extensions) accumulate into CC_GENERIC_DEF_MAX;
 * each factory builds into a CC_EMIT_TPL_BUF_SIZE scratch arena. */
#define CC_EMIT_TPL_BUF_SIZE        32768
#define CC_EMIT_FRAGMENT_MAX        4096
#define CC_EMIT_FORMAT_BUF_MAX      16384
#define CC_GENERIC_DEF_MAX          32768
#define CC_EMIT_SPLICE_BLOCK_MAX    32768

#endif /* CC_EMIT_LIMITS_H */
