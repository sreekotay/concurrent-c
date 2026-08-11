#ifndef CC_EMIT_LIMITS_H
#define CC_EMIT_LIMITS_H

/* Shared comptime emit / generic lowering buffer limits (hard errors on overflow).
 * Generic-factory *definitions* accumulate into a growable CCArena (no byte
 * cap); CC_EMIT_TPL_BUF_SIZE is only the per-factory @emit scratch root
 * (heap + overflow slabs). Fixed caps remain for small format/splice temps. */
#define CC_EMIT_TPL_BUF_SIZE        32768
#define CC_EMIT_FRAGMENT_MAX        4096
#define CC_EMIT_FORMAT_BUF_MAX      16384
#define CC_EMIT_SPLICE_BLOCK_MAX    32768

#endif /* CC_EMIT_LIMITS_H */
