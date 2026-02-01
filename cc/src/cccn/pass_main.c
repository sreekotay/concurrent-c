/*
 * CCCN Main Pass - Replaces old visitor pipeline
 *
 * Implements cc_run_main_pass() using cccn's clean AST-based approach:
 *   1. Parse source file via TCC bridge
 *   2. Run lowering passes (UFCS, closures, etc.)
 *   3. Emit C code
 */

#include "visitor/pass.h"
#include "comptime/symbols.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cccn/parser/parser.h"
#include "cccn/codegen/codegen.h"
#include "cccn/passes/passes.h"

/* Const pass - extract comptime constants (currently stubbed) */
int cc_run_const_pass(const char* input_path, CCSymbolTable* symbols) {
    (void)input_path;
    (void)symbols;
    /* TODO: Implement comptime constant extraction if needed.
     * For now, this is a no-op - constants are handled elsewhere. */
    return 0;
}

/* Check if path ends with given suffix */
static int ends_with(const char* path, const char* suffix) {
    size_t plen = strlen(path);
    size_t slen = strlen(suffix);
    if (plen < slen) return 0;
    return strcmp(path + plen - slen, suffix) == 0;
}

/* Check if input is a header file */
static int is_header_file(const char* path) {
    return ends_with(path, ".cch") || ends_with(path, ".CCH");
}

int cc_run_main_pass(const char* input_path, CCSymbolTable* symbols, const char* output_path) {
    (void)symbols;  /* TODO: integrate comptime symbols if needed */
    
    if (!input_path || !output_path) return EINVAL;
    
    /* Parse input file using cccn parser (includes preprocessing) */
    CCNFile* file = cc_parse_file(input_path);
    if (!file) {
        fprintf(stderr, "cc: parse failed for %s\n", input_path);
        return EINVAL;
    }
    
    /* Run lowering passes */
    if (cc_pass_lower_ufcs(file) != 0) {
        fprintf(stderr, "cc: UFCS lowering failed\n");
        cc_file_free(file);
        return EINVAL;
    }
    
    if (cc_pass_lower_closures(file) != 0) {
        fprintf(stderr, "cc: closure lowering failed\n");
        cc_file_free(file);
        return EINVAL;
    }
    
    /* TODO: Add more passes as needed
     * - async state machine transform
     * - defer expansion
     * - nursery/spawn lowering
     * - arena lowering
     * Most of these are handled in preprocessing for now.
     */
    
    /* Open output file */
    FILE* out = fopen(output_path, "w");
    if (!out) {
        fprintf(stderr, "cc: cannot open %s for writing\n", output_path);
        cc_file_free(file);
        return errno;
    }
    
    /* Emit C code */
    int result;
    if (is_header_file(input_path)) {
        result = cc_emit_h(file, out, NULL);
    } else {
        result = cc_emit_c(file, out);
    }
    
    fclose(out);
    
    if (result != 0) {
        fprintf(stderr, "cc: codegen failed for %s\n", input_path);
        cc_file_free(file);
        return EINVAL;
    }
    
    cc_file_free(file);
    return 0;
}
