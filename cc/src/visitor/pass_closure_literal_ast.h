#ifndef CC_PASS_CLOSURE_LITERAL_AST_H
#define CC_PASS_CLOSURE_LITERAL_AST_H

#include <stddef.h>

#include "visitor/visitor.h"

/* Rewrite closure literals (`() => { ... }`, `x => expr`, etc.) using stub-AST spans.
   Also emits top-level closure env/entry/make definitions into out_protos/out_defs.

   Returns:
   - 1 if rewritten (out_src/out_len set; out_protos/out_defs may be set)
   - 0 if no changes
   - -1 on hard error (diagnostic already printed) */
int cc__rewrite_closure_literals_with_nodes(const CCASTRoot* root,
                                           const CCVisitorCtx* ctx,
                                           const char* in_src,
                                           size_t in_len,
                                           char** out_src,
                                           size_t* out_len,
                                           char** out_protos,
                                           size_t* out_protos_len,
                                           char** out_defs,
                                           size_t* out_defs_len);

#endif /* CC_PASS_CLOSURE_LITERAL_AST_H */

