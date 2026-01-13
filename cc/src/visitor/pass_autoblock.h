#ifndef CC_PASS_AUTOBLOCK_H
#define CC_PASS_AUTOBLOCK_H

#include <stddef.h>

#include "visitor/visitor.h"
struct CCASTRoot;

/* Auto-blocking pass: transforms sync calls in @async functions to await cc_run_blocking_task_intptr(...) */
int cc__rewrite_autoblocking_calls_with_nodes(const struct CCASTRoot* root,
                                             const CCVisitorCtx* ctx,
                                             const char* in_src,
                                             size_t in_len,
                                             char** out_src,
                                             size_t* out_len);

#endif /* CC_PASS_AUTOBLOCK_H */