#ifndef CC_PASS_RESULT_UNWRAP_H
#define CC_PASS_RESULT_UNWRAP_H

#include <stddef.h>

#include "visitor/visitor.h"

/* Lower the `?>` expression operator to a statement-expression.
 *
 * Forms handled:
 *   EXPR ?> DEFAULT_EXPR
 *   EXPR ?>(e) RHS_EXPR
 *   EXPR ?> DIVERGENT_STMT                (return.../break;/continue;)
 *   EXPR ?>(e) DIVERGENT_STMT
 *
 * The result expression yields `cc_value(EXPR)` on success.  On error:
 *   - with an expression RHS, the expression is evaluated (optionally with
 *     the error value in scope as `e`) and its value becomes the result;
 *   - with a divergent RHS, the statement is executed inside the error
 *     branch, transferring control out of the enclosing statement.
 *
 * Returns 1 if the input was rewritten, 0 if no `?>` occurrences were found,
 * and -1 on allocation failure or malformed input. Emits output via
 * *out_src / *out_len on success; caller owns the buffer. */
int cc__rewrite_result_unwrap(const CCVisitorCtx* ctx,
                              const char* in_src,
                              size_t in_len,
                              char** out_src,
                              size_t* out_len);

#endif /* CC_PASS_RESULT_UNWRAP_H */
