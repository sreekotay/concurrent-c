#ifndef CC_VISITOR_WALK_H
#define CC_VISITOR_WALK_H

#include "ast/ast.h"
#include "visitor.h"

// Bridge to the real visitor; kept for compatibility with pass wiring.
int cc_visit_ast(const CCASTRoot* root, CCSymbolTable* symbols, const char* input_path, const char* output_path);

#endif // CC_VISITOR_WALK_H

