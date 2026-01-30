#ifndef CC_PARSER_TCC_BRIDGE_H
#define CC_PARSER_TCC_BRIDGE_H

// Bridge declarations for TCC integration. These are expected to be provided
// by the TCC extension hooks once applied. If CC_TCC_EXT_AVAILABLE is not
// defined, the parser will return ENOTSUP.

#include "ast/ast.h"
#include "comptime/symbols.h"

// Parse the given file and return an AST handle. Returns NULL on failure.
CCASTRoot* cc_tcc_bridge_parse_to_ast(const char* preprocessed_path, const char* original_path, CCSymbolTable* symbols);

// Parse from in-memory source string (no temp files). Returns NULL on failure.
CCASTRoot* cc_tcc_bridge_parse_string_to_ast(const char* source_code, const char* virtual_filename, const char* original_path, CCSymbolTable* symbols);

// Free an AST returned by cc_tcc_bridge_parse_to_ast or cc_tcc_bridge_parse_string_to_ast.
void cc_tcc_bridge_free_ast(CCASTRoot* root);

#endif // CC_PARSER_TCC_BRIDGE_H

