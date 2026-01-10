#include "checker.h"

#include <stdio.h>
#include <string.h>

// Slice flag tracking scaffold. As the parser starts emitting CC AST nodes,
// populate flags on expressions and enforce send_take eligibility.

typedef enum {
    CC_SLICE_UNKNOWN = 0,
    CC_SLICE_UNIQUE = 1 << 0,
    CC_SLICE_TRANSFERABLE = 1 << 1,
    CC_SLICE_SUBSLICE = 1 << 2,
} CCSliceFlags;

int cc_check_ast(const CCASTRoot* root, CCCheckerCtx* ctx) {
    if (!root || !ctx) return -1;
    if (!root->items || root->items_len == 0) {
        // Transitional: no CC AST yet, skip.
        return 0;
    }
    ctx->errors = 0;
    // TODO: walk root->items once populated.
    return ctx->errors ? -1 : 0;
}

