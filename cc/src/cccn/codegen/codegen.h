#ifndef CC_CCCN_CODEGEN_H
#define CC_CCCN_CODEGEN_H

#include "cccn/ast/ast_new.h"
#include <stdio.h>

/* Emit C code from the CCCN AST */
int cccn_emit_c(const CCCNRoot* root, FILE* out);

#endif /* CC_CCCN_CODEGEN_H */
