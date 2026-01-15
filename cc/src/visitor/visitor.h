#ifndef CC_VISITOR_VISITOR_H
#define CC_VISITOR_VISITOR_H

#include "ast/ast.h"
#include "comptime/symbols.h"

// Placeholder structures for type/context state used during visiting.
typedef struct CCVisitorCtx {
    CCSymbolTable* symbols;
    const char* input_path; // used for #line source mapping
    // TODO: add type tables, provenance tracking, arena/async context, codegen state.
} CCVisitorCtx;

// Run the main visitor and emit C to output_path.
int cc_visit(const CCASTRoot* root, CCVisitorCtx* ctx, const char* output_path);

#endif // CC_VISITOR_VISITOR_H

