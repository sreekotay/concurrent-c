#include "pass.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "parser/parse.h"
#include "visitor/checker.h"
#include "visitor/walk.h"
#include "visitor/visitor.h"
#include "util/io.h"

int cc_run_const_pass(const char* input_path, CCSymbolTable* symbols) {
    (void)input_path;
    (void)symbols;
    // Temporarily skip const pass until recorder is hardened.
    return 0;
}

int cc_run_main_pass(const char* input_path, CCSymbolTable* symbols, const char* output_path) {
    if (!input_path || !output_path) return EINVAL;
    CCASTRoot* root = NULL;
    int err = cc_parse_to_ast(input_path, symbols, &root);
    if (err != 0) return err;
    /* Semantic checker stage (slice move/provenance, etc). */
    {
        CCCheckerCtx c = {0};
        c.symbols = symbols;
        c.input_path = input_path;
        err = cc_check_ast(root, &c);
        if (err != 0) {
            cc_free_ast(root);
            return EINVAL;
        }
        if (c.warnings > 0) {
            fprintf(stderr, "cc: %d warning(s)\n", c.warnings);
        }
    }
    err = cc_visit_ast(root, symbols, input_path, output_path);
    cc_free_ast(root);
    return err;
}

