#include "walk.h"

#include <errno.h>
#include <stdio.h>

int cc_visit_ast(const CCASTRoot* root, CCSymbolTable* symbols, const char* input_path, const char* output_path) {
    CCVisitorCtx ctx = {
        .symbols = symbols,
        .input_path = input_path,
    };
    return cc_visit(root, &ctx, output_path);
}

