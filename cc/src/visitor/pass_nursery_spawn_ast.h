#ifndef CC_PASS_NURSERY_SPAWN_AST_H
#define CC_PASS_NURSERY_SPAWN_AST_H

#include <stddef.h>

#include "visitor/visitor.h"
#include "visitor/edit_buffer.h"

/* Rewrite `spawn(...)` statements using stub-AST spans.
   Returns:
   - 1 if rewritten and out_* is set
   - 0 if no changes
   - -1 on hard error (diagnostic already printed) */
int cc__rewrite_spawn_stmts_with_nodes(const CCASTRoot* root,
                                      const CCVisitorCtx* ctx,
                                      const char* in_src,
                                      size_t in_len,
                                      char** out_src,
                                      size_t* out_len);

/* Rewrite `@nursery { ... }` blocks into plain C using stub-AST spans.
   Returns 1/0 like above; -1 on hard error. */
int cc__rewrite_nursery_blocks_with_nodes(const CCASTRoot* root,
                                         const CCVisitorCtx* ctx,
                                         const char* in_src,
                                         size_t in_len,
                                         char** out_src,
                                         size_t* out_len);

/* NEW: Collect nursery edits into EditBuffer without applying.
   Returns number of edits added (>= 0), or -1 on error. */
int cc__collect_nursery_edits(const CCASTRoot* root,
                              const CCVisitorCtx* ctx,
                              CCEditBuffer* eb);

/* NEW: Collect spawn edits into EditBuffer without applying.
   Returns number of edits added (>= 0), or -1 on error. */
int cc__collect_spawn_edits(const CCASTRoot* root,
                            const CCVisitorCtx* ctx,
                            CCEditBuffer* eb);

#endif /* CC_PASS_NURSERY_SPAWN_AST_H */

