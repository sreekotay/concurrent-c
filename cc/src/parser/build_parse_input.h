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
    /* M7.C3: post-CPP-expand but PRE-relower buffer (still has
     * `int[~4 >]` etc.).  Used by the channel-pair scanner to find
     * macro-generated chan handle decls. Owned; NULL when pre-expand
     * is off. */
    char* buffer_pre_relower;
    size_t buffer_pre_relower_len;
    /* Canonical CC buffer (phase-1+3); visit_codegen mutates this and emits host
     * container/result decls at final compile time. */
    char* buffer_codegen;
    size_t buffer_codegen_len;
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
