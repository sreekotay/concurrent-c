#ifndef CC_PASS_CLOSURE_CALLS_H
#define CC_PASS_CLOSURE_CALLS_H

#include <stddef.h>

#include "visitor/visitor.h"
#include "visitor/edit_buffer.h"

/* Closure call rewriting pass: walks non-UFCS CALL nodes whose callee
 * resolves to a closure type (CCClosure1 / CCClosure2) and emits per-span
 * `[name_start, rparen_end) → cc_closureN_call(...)` edits directly into
 * `eb`.  Nested closure calls inside another call's argument list are
 * rewritten inline within the outer span's replacement (so each top-level
 * span produces exactly one edit), enabling this pass to compose with
 * other Phase-3 collectors in the stage-2 batched apply without producing
 * whole-buffer (0..len) edits.
 *
 * Returns the number of edits added (>= 0), or -1 on error. */
int cc__collect_closure_calls_edits(const CCASTRoot* root,
                                    const CCVisitorCtx* ctx,
                                    CCEditBuffer* eb);

#endif /* CC_PASS_CLOSURE_CALLS_H */