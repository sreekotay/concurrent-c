#ifndef CC_PARSER_H
#define CC_PARSER_H

#include "cccn/ast/ast.h"

/*
 * CC Parser - Builds CCNFile from source.
 *
 * Uses TCC to tokenize/parse C constructs, then converts the TCC stub-AST
 * into our rich CCNNode tree.
 */

/* Parse a source file and return a CCNFile. Returns NULL on error. */
CCNFile* cc_parse_file(const char* path);

/* Parse source from memory. Returns NULL on error. */
CCNFile* cc_parse_string(const char* source, const char* filename);

/* Free a parsed file and all its nodes. */
void cc_file_free(CCNFile* file);

#endif /* CC_PARSER_H */
