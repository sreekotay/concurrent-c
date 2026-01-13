#ifndef CC_VISITOR_PIPELINE_H
#define CC_VISITOR_PIPELINE_H

#include <stddef.h>

#include "parser/parse.h"
#include "visitor/visitor.h"

struct CCSymbolTable;

/* Main visitor pipeline entry point */
int cc_visit_pipeline(const struct CCASTRoot* root, CCVisitorCtx* ctx, const char* output_path);

#endif /* CC_VISITOR_PIPELINE_H */