/* pass_channel_syntax.h - Channel syntax lowering passes.
 *
 * Handles:
 *   - channel_pair(&tx, &rx) -> cc_channel_pair_create_returning(...) (both forms)
 *     The 'ordered' flag on rx channels is preserved; ordered channels use
 *     sizeof(CCTask) as elem_size so chan_send_task and chan_recv agree on wire size.
 *   - T[~ ... >] -> CCChanTx
 *   - T[~ ... <] -> CCChanRx  (T[~ ... ordered <] also becomes CCChanRx;
 *     ordered is a runtime flag on the channel, not a distinct type)
 *
 * Extracted from visit_codegen.c for maintainability.
 */

#ifndef CC_PASS_CHANNEL_SYNTAX_H
#define CC_PASS_CHANNEL_SYNTAX_H

#include <stddef.h>
#include "visitor/visitor.h"

/* Rewrite `channel_pair(&tx, &rx)` calls to cc_channel_pair_create_returning(...).
 * Both expression-form (CCChan* ch = channel_pair(...)) and statement-form
 * (channel_pair(...);) are handled; both preserve the ordered flag.
 * Returns newly allocated string, or NULL on error.
 * Sets *out_len to output length. */
char* cc__rewrite_channel_pair_calls_text(const CCVisitorCtx* ctx,
                                          const char* src,
                                          size_t len,
                                          size_t* out_len);

/* Rewrite channel handle types `T[~ ... >]` / `T[~ ... <]` to CCChanTx/CCChanRx.
 * T[~ ... ordered <] also becomes CCChanRx (ordered is a flag on the channel).
 * Returns newly allocated string, or NULL on error. */
char* cc__rewrite_chan_handle_types_text(const CCVisitorCtx* ctx,
                                         const char* src,
                                         size_t n);

/* Rewrite `chan_send_task(ch, () => [captures] body)` to spawn task and send handle.
 * Handles v3 syntax with captures AFTER the arrow.
 * Returns newly allocated string, or NULL on error. */
char* cc__rewrite_chan_send_task_text(const CCVisitorCtx* ctx,
                                      const char* src,
                                      size_t len,
                                      size_t* out_len);

#endif /* CC_PASS_CHANNEL_SYNTAX_H */
