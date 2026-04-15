#ifndef CC_PASS_ERR_SYNTAX_H
#define CC_PASS_ERR_SYNTAX_H

#include <stddef.h>

#include "visitor/visitor.h"

/* Lower @err / @errhandler / <? or =<! ... @err / same ... : default to plain C.
 * Returns 1 if rewritten, 0 if unchanged, -1 on error (diagnostic emitted). */
int cc__rewrite_err_syntax(const CCVisitorCtx* ctx,
                           const char* in_src,
                           size_t in_len,
                           char** out_src,
                           size_t* out_len);

#endif /* CC_PASS_ERR_SYNTAX_H */
