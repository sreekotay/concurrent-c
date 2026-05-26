#ifndef CC_CPP_EXPAND_H
#define CC_CPP_EXPAND_H

#include <stddef.h>

/* M5.5/M6 alternative spike: pre-expand source via TCC's CPP so that
 * existing text passes operate on macro-expanded text. Returns malloc'd
 * buffer on success (caller frees), NULL on error. */
char* cc_cpp_expand(const char* src, size_t src_len,
                    const char* input_path, size_t* out_len);

#endif
