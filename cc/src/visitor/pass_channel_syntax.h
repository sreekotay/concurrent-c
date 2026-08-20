/* pass_channel_syntax.h - Channel syntax lowering passes.
 *
 * Handles:
 *   - cc_channel_pair(&tx, &rx) -> cc_channel_pair_create_returning(...) (both forms)
 *     `ordered` on the rx is one delivery-order property applied per payload
 *     kind (spec/concurrent-c-channel.md "Ordered channels"): task-handle
 *     channels (CCTask element type, or a tx fed via the send_task family)
 *     use sizeof(CCTask) as elem_size and set the runtime is_ordered flag so
 *     `cc_channel_send_task(...)` and ordered recv agree on wire size; data
 *     channels keep the declared element size and the normal send/recv
 *     machinery (the attribute is a per-sender-FIFO contract marker).
 *   - T[~ ... >] -> CCChanTx
 *   - T[~ ... <] -> CCChanRx  (T[~ ... ordered <] also becomes CCChanRx;
 *     ordered is a runtime flag on the channel, not a distinct type)
 *
 * Legacy visitor pass (linked into libshadow_comptime; not product emit).
 */

#ifndef CC_PASS_CHANNEL_SYNTAX_H
#define CC_PASS_CHANNEL_SYNTAX_H

#include <stddef.h>
#include "visitor/visitor.h"

/* Rewrite `cc_channel_pair(&tx, &rx)` calls to cc_channel_pair_create_returning(...).
 * Both expression-form (CCChan* ch = cc_channel_pair(...)) and statement-form
 * (cc_channel_pair(...);) are handled; both preserve the ordered flag.
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

/* Rewrite `cc_channel_send_task(ch, () => [captures] body)` to spawn task and send handle.
 * Handles v3 syntax with captures AFTER the arrow.
 * Returns newly allocated string, or NULL on error. */
char* cc__rewrite_chan_send_task_text(const CCVisitorCtx* ctx,
                                      const char* src,
                                      size_t len,
                                      size_t* out_len);

#endif /* CC_PASS_CHANNEL_SYNTAX_H */
