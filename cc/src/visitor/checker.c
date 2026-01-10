#include "checker.h"

#include <stdio.h>
#include <string.h>

// Placeholder slice flag tracking. In a full implementation, this would walk the AST,
// track locals/temps, and propagate flags through expressions. The current TCC-based
// stub AST does not expose CC nodes, so this checker is a no-op placeholder to keep
// the pipeline wired while the richer AST lands.

typedef enum {
    CC_SLICE_UNKNOWN = 0,
    CC_SLICE_UNIQUE = 1 << 0,
    CC_SLICE_TRANSFERABLE = 1 << 1,
    CC_SLICE_SUBSLICE = 1 << 2,
} CCSliceFlags;

int cc_check_ast(const CCASTRoot* root, CCCheckerCtx* ctx) {
    if (!root || !ctx) return -1;
    (void)root;
    ctx->errors = 0;
    // TODO: replace with real AST walk once CC nodes are emitted by the parser.
    return 0;
}

