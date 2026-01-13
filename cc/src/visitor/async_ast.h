#ifndef CC_ASYNC_AST_H
#define CC_ASYNC_AST_H

#include <stddef.h>

struct CCASTRoot;
struct CCVisitorCtx;

/* AST-driven async lowering (state machine generation) */
int cc_async_rewrite_state_machine_ast(const struct CCASTRoot* root,
                                      const struct CCVisitorCtx* ctx,
                                      const char* in_src,
                                      size_t in_len,
                                      char** out_src,
                                      size_t* out_len);

#endif /* CC_ASYNC_AST_H */