#ifndef CC_PASS_ARENA_AST_H
#define CC_PASS_ARENA_AST_H

#include <stddef.h>

#include "visitor/visitor.h"
#include "visitor/edit_buffer.h"

/* Lower `@arena(...) { ... }` blocks into plain C using stub-AST spans (no line rewriting). */
int cc__rewrite_arena_blocks_with_nodes(const CCASTRoot* root,
                                       const CCVisitorCtx* ctx,
                                       const char* in_src,
                                       size_t in_len,
                                       char** out_src,
                                       size_t* out_len);

/* NEW: Collect arena edits into EditBuffer without applying.
   Returns number of edits added (>= 0), or -1 on error. */
int cc__collect_arena_edits(const CCASTRoot* root,
                            const CCVisitorCtx* ctx,
                            CCEditBuffer* eb);

#endif /* CC_PASS_ARENA_AST_H */

