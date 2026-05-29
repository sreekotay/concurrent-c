#ifndef CC_PASS_AWAIT_NORMALIZE_H
#define CC_PASS_AWAIT_NORMALIZE_H

#include <stddef.h>

#include "visitor/visitor.h"
#include "visitor/edit_buffer.h"

/* Await expression normalization pass: hoists complex `await EXPR` into
 * temp-variable assignments at the enclosing statement's line start so the
 * receiver's call/return site sees a stable lvalue (`tmp`) instead of the
 * full await sub-expression.  Emits per-span edits (insertion at the
 * statement line + replacement of the await expr with the temp) directly
 * into `eb` — no whole-buffer rewrite, so this pass composes safely with
 * other Phase-3 collectors in the stage-2 batched apply (see PIPELINE.md).
 *
 * Returns the number of edits added (>= 0), or -1 on error. */
int cc__collect_await_normalize_edits(const CCASTRoot* root,
                                      const CCVisitorCtx* ctx,
                                      CCEditBuffer* eb);

#endif /* CC_PASS_AWAIT_NORMALIZE_H */