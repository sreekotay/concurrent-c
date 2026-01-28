#ifndef CC_PASS_AUTOBLOCK_H
#define CC_PASS_AUTOBLOCK_H

#include <stddef.h>

#include "visitor/visitor.h"
#include "visitor/edit_buffer.h"

/* Auto-blocking pass: transforms sync calls in @async functions to await cc_run_blocking_task_intptr(...) */
int cc__rewrite_autoblocking_calls_with_nodes(const CCASTRoot* root,
                                             const CCVisitorCtx* ctx,
                                             const char* in_src,
                                             size_t in_len,
                                             char** out_src,
                                             size_t* out_len);

/* NEW: Collect autoblocking edits into EditBuffer without applying.
   Returns number of edits added (>= 0), or -1 on error. */
int cc__collect_autoblocking_edits(const CCASTRoot* root,
                                   const CCVisitorCtx* ctx,
                                   CCEditBuffer* eb);

#endif /* CC_PASS_AUTOBLOCK_H */