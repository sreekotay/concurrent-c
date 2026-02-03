/* pass_channel_syntax.h - Channel syntax lowering passes.
 *
 * Handles:
 *   - channel_pair(&tx, &rx) -> cc_chan_pair_create_full(...)
 *   - T[~ ... >] -> CCChanTx
 *   - T[~ ... <] -> CCChanRx
 *
 * Extracted from visit_codegen.c for maintainability.
 */

#ifndef CC_PASS_CHANNEL_SYNTAX_H
#define CC_PASS_CHANNEL_SYNTAX_H

#include <stddef.h>
#include "visitor/visitor.h"

/* Rewrite `channel_pair(&tx, &rx)` calls to cc_chan_pair_create_full(...).
 * Returns newly allocated string, or NULL on error.
 * Sets *out_len to output length. */
char* cc__rewrite_channel_pair_calls_text(const CCVisitorCtx* ctx,
                                          const char* src,
                                          size_t len,
                                          size_t* out_len);

/* Rewrite channel handle types `T[~ ... >]` / `T[~ ... <]` to CCChanTx/CCChanRx.
 * T[~ ... ordered <] becomes CCChanRxOrdered.
 * Returns newly allocated string, or NULL on error. */
char* cc__rewrite_chan_handle_types_text(const CCVisitorCtx* ctx,
                                         const char* src,
                                         size_t n);

#endif /* CC_PASS_CHANNEL_SYNTAX_H */
