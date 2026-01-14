#ifndef CC_PASS_ARENA_AST_H
#define CC_PASS_ARENA_AST_H

#include <stddef.h>

#include "visitor/visitor.h"

/* Lower `@arena(...) { ... }` blocks into plain C using stub-AST spans (no line rewriting). */
int cc__rewrite_arena_blocks_with_nodes(const CCASTRoot* root,
                                       const CCVisitorCtx* ctx,
                                       const char* in_src,
                                       size_t in_len,
                                       char** out_src,
                                       size_t* out_len);

#endif /* CC_PASS_ARENA_AST_H */

