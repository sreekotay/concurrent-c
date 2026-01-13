#ifndef CC_PASS_CLOSURE_CALLS_H
#define CC_PASS_CLOSURE_CALLS_H

#include <stddef.h>

#include "visitor/visitor.h"

/* Closure call rewriting pass: transforms closure calls to __cc_closure_call_N syntax */
int cc__rewrite_all_closure_calls_with_nodes(const CCASTRoot* root,
                                           const CCVisitorCtx* ctx,
                                           const char* in_src,
                                           size_t in_len,
                                           char** out_src,
                                           size_t* out_len);

#endif /* CC_PASS_CLOSURE_CALLS_H */