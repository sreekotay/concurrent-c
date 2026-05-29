#ifndef CC_PASS_AUTOBLOCK_H
#define CC_PASS_AUTOBLOCK_H

#include <stddef.h>

#include "visitor/visitor.h"
#include "visitor/edit_buffer.h"

/* Auto-blocking pass: walks blocking CALL sites inside @async functions
 * and emits per-span edits that wrap them in `await cc_run_blocking_task_intptr(...)`
 * dispatch closures.  Coalesces adjacent sync-call statements into a single
 * batched dispatch and folds an optional trailing return/assign into the
 * same closure when contiguous.  All emitted edits target non-overlapping
 * statement ranges so this pass composes with other Phase-3 collectors
 * in the stage-2 batched apply without producing whole-buffer edits.
 *
 * Returns the number of edits added (>= 0), or -1 on error. */
int cc__collect_autoblocking_edits(const CCASTRoot* root,
                                   const CCVisitorCtx* ctx,
                                   CCEditBuffer* eb);

#endif /* CC_PASS_AUTOBLOCK_H */