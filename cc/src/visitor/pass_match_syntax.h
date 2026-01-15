#ifndef CC_PASS_MATCH_SYNTAX_H
#define CC_PASS_MATCH_SYNTAX_H

#include <stddef.h>

#include "visitor/visitor.h"

/* Rewrite `@match { case ... }` into valid C. This is a text-based pass because `@match`
   is not valid C syntax and must not reach the C compiler.

   Returns:
   - 1 if rewritten (out_src/out_len set)
   - 0 if no changes
   - -1 on hard error (diagnostic already printed) */
int cc__rewrite_match_syntax(const CCVisitorCtx* ctx,
                            const char* in_src,
                            size_t in_len,
                            char** out_src,
                            size_t* out_len);

#endif /* CC_PASS_MATCH_SYNTAX_H */

