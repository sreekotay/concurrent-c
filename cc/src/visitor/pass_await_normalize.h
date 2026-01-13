#ifndef CC_PASS_AWAIT_NORMALIZE_H
#define CC_PASS_AWAIT_NORMALIZE_H

#include <stddef.h>

#include "visitor/visitor.h"
struct CCASTRoot;

/* Await expression normalization pass: transforms complex await expressions into temp variables */
int cc__rewrite_await_exprs_with_nodes(const struct CCASTRoot* root,
                                      const CCVisitorCtx* ctx,
                                      const char* in_src,
                                      size_t in_len,
                                      char** out_src,
                                      size_t* out_len);

#endif /* CC_PASS_AWAIT_NORMALIZE_H */