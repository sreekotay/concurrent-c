#ifndef CC_PARSER_PARSE_H
#define CC_PARSER_PARSE_H

#include "comptime/symbols.h"
#include "ast/ast.h"

// Stub: parse CC source using TCC frontend (to be implemented).
// Returns 0 on success, non-zero on error.
int cc_parse_to_ast(const char* input_path, CCSymbolTable* symbols, CCASTRoot** out_root);
void cc_free_ast(CCASTRoot* root);

#endif // CC_PARSER_PARSE_H

