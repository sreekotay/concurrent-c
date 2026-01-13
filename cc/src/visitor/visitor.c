#include "visitor.h"
#include "visit_codegen.h"

int cc_visit(const CCASTRoot* root, CCVisitorCtx* ctx, const char* output_path) {
    return cc_visit_codegen(root, ctx, output_path);
}
