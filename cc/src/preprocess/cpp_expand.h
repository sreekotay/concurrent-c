#ifndef CC_CPP_EXPAND_H
#define CC_CPP_EXPAND_H

#include <stddef.h>

/* M5.5/M6 alternative spike: pre-expand source via TCC's CPP so that
 * existing text passes operate on macro-expanded text. Returns malloc'd
 * buffer on success (caller frees), NULL on error. */
char* cc_cpp_expand(const char* src, size_t src_len,
                    const char* input_path, size_t* out_len);

/* Like cc_cpp_expand, but when keep_defines is non-zero the expansion mirrors
 * `cpp -dD`: retained `#define` directives (including header include guards)
 * are emitted alongside the expanded text.  Used to flatten the constant
 * reparse prelude once so it can be cached and reused across reparses without
 * re-reading/re-preprocessing the standard + CC headers each time. */
char* cc_cpp_expand_ex(const char* src, size_t src_len,
                       const char* input_path, size_t* out_len, int keep_defines);

#endif
