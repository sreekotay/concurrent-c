#ifndef CC_CODEGEN_H
#define CC_CODEGEN_H

#include "cccn/ast/ast.h"
#include <stdio.h>

/* Emit C code from the CC AST */
int cc_emit_c(const CCNFile* file, FILE* out);

/* Emit C header from the CC AST (adds include guards, no main impl) */
int cc_emit_h(const CCNFile* file, FILE* out, const char* guard_name);

#endif /* CC_CODEGEN_H */
