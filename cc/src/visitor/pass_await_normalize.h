#ifndef CC_PASS_AWAIT_NORMALIZE_H
#define CC_PASS_AWAIT_NORMALIZE_H

#include <stddef.h>

#include "visitor/visitor.h"
#include "visitor/edit_buffer.h"

/* Await expression normalization pass: transforms complex await expressions into temp variables */
int cc__rewrite_await_exprs_with_nodes(const CCASTRoot* root,
                                      const CCVisitorCtx* ctx,
                                      const char* in_src,
                                      size_t in_len,
                                      char** out_src,
                                      size_t* out_len);

/* NEW: Collect await normalization edits into EditBuffer without applying.
   Returns number of edits added (>= 0), or -1 on error. */
int cc__collect_await_normalize_edits(const CCASTRoot* root,
                                      const CCVisitorCtx* ctx,
                                      CCEditBuffer* eb);

#endif /* CC_PASS_AWAIT_NORMALIZE_H */