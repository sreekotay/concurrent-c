#ifndef CC_BUILD_PARSE_INPUT_H
#define CC_BUILD_PARSE_INPUT_H

#include "../diag/source_map.h"
#include "comptime/symbols.h"
#include <stddef.h>

typedef struct CCBuildParseInput {
    char* buffer;
    size_t len;
    CCSourceMap* source_map;
    int primary_file_id;
} CCBuildParseInput;

/* Canonical prep shared by parse.c and visit_codegen.c (M1). Caller frees via
 * cc_build_parse_input_free(). */
int cc_build_parse_input(const char* file_buf,
                         size_t file_len,
                         const char* input_path,
                         CCSymbolTable* symbols,
                         int for_reparse,
                         CCBuildParseInput* out);

void cc_build_parse_input_free(CCBuildParseInput* in);

#endif
