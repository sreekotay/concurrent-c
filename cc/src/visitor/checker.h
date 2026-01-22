#ifndef CC_VISITOR_CHECKER_H
#define CC_VISITOR_CHECKER_H
/*
 * Minimal type/move checker for slice transfer eligibility.
 * - Tracks slice flags (unique, transferable, subslice) on expressions.
 * - Enforces send_take/send_take_slice requires unique+transferable+!subslice.
 */

#include "ast/ast.h"
#include "comptime/symbols.h"

typedef struct {
    CCSymbolTable* symbols;
    const char* input_path;
    int errors;
    int warnings;
} CCCheckerCtx;

// Run the checker; returns 0 on success, non-zero on error. No-op until CC AST is populated.
int cc_check_ast(const CCASTRoot* root, CCCheckerCtx* ctx);

#endif // CC_VISITOR_CHECKER_H

