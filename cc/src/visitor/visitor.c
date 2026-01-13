#include "visitor.h"
#include "visitor_pipeline.h"

#include <errno.h>

int cc_visit(const CCASTRoot* root, CCVisitorCtx* ctx, const char* output_path) {
    return cc_visit_pipeline(root, ctx, output_path);
}