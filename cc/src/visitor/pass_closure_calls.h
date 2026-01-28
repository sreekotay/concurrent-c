#ifndef CC_PASS_CLOSURE_CALLS_H
#define CC_PASS_CLOSURE_CALLS_H

#include <stddef.h>

#include "visitor/visitor.h"
#include "visitor/edit_buffer.h"

/* Closure call rewriting pass: transforms closure calls to __cc_closure_call_N syntax */
int cc__rewrite_all_closure_calls_with_nodes(const CCASTRoot* root,
                                           const CCVisitorCtx* ctx,
                                           const char* in_src,
                                           size_t in_len,
                                           char** out_src,
                                           size_t* out_len);

/* NEW: Collect closure call edits into EditBuffer without applying.
   Returns number of edits added (>= 0), or -1 on error. */
int cc__collect_closure_calls_edits(const CCASTRoot* root,
                                    const CCVisitorCtx* ctx,
                                    CCEditBuffer* eb);

#endif /* CC_PASS_CLOSURE_CALLS_H */