#ifndef CC_PASS_UFCS_H
#define CC_PASS_UFCS_H

#include <stddef.h>

#include "visitor/visitor.h"
#include "visitor/edit_buffer.h"
struct CCASTRoot;

/* UFCS rewriting pass: transforms obj.method(args) to method(obj, args) */
int cc__rewrite_ufcs_spans_with_nodes(const struct CCASTRoot* root,
                                     const CCVisitorCtx* ctx,
                                     const char* in_src,
                                     size_t in_len,
                                     char** out_src,
                                     size_t* out_len);

/* NEW: Collect UFCS edits into EditBuffer without applying.
   Returns number of edits added (>= 0), or -1 on error. */
int cc__collect_ufcs_edits(const struct CCASTRoot* root,
                           const CCVisitorCtx* ctx,
                           CCEditBuffer* eb);

#endif /* CC_PASS_UFCS_H */