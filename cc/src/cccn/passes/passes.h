#ifndef CC_PASSES_H
#define CC_PASSES_H

#include "cccn/ast/ast.h"

/* 
 * AST-to-AST lowering passes.
 * These modify the CC AST in-place.
 */

/* Lower UFCS calls: receiver.method(args) -> method(receiver, args) */
int cc_pass_lower_ufcs(CCNFile* file);

/* Lower @async functions into state machines */
int cc_pass_lower_async(CCNFile* file);

/* Lower closure literals into structs and thunks */
int cc_pass_lower_closures(CCNFile* file);

#endif /* CC_PASSES_H */
