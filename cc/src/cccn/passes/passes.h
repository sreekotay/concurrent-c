#ifndef CC_CCCN_PASSES_H
#define CC_CCCN_PASSES_H

#include "cccn/ast/ast_new.h"

/* 
 * AST-to-AST lowering passes.
 * These modify the CCCN AST in-place or return a new lowered AST.
 */

/* Lower UFCS calls: receiver.method(args) -> method(receiver, args) */
int cccn_pass_lower_ufcs(CCCNRoot* root);

/* Lower @async functions into state machines */
int cccn_pass_lower_async(CCCNRoot* root);

/* Lower closure literals into structs and thunks */
int cccn_pass_lower_closures(CCCNRoot* root);

#endif /* CC_CCCN_PASSES_H */
