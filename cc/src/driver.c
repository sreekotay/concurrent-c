#include "driver.h"

#include <stdio.h>
#include <stdlib.h>

#include "comptime/symbols.h"
#include "visitor/pass.h"

// Stub pipeline: const pass then main pass (currently copies input->output).
int cc_compile_with_config(const char *input_path, const char *output_path, const CCCompileConfig* config) {
    if (!input_path || !output_path) {
        return -1;
    }

    CCSymbolTable* symbols = cc_symbols_new();
    if (!symbols) {
        fprintf(stderr, "cc: failed to allocate symbol table\n");
        return -1;
    }

    if (config && config->consts && config->const_count > 0) {
        int err = cc_symbols_add_predefined(symbols, config->consts, config->const_count);
        if (err != 0) {
            fprintf(stderr, "cc: failed to preload consts (err=%d)\n", err);
            cc_symbols_free(symbols);
            return err;
        }
    }

    int err = cc_run_const_pass(input_path, symbols);
    if (err != 0) {
        fprintf(stderr, "cc: const pass failed for %s (err=%d)\n", input_path, err);
        cc_symbols_free(symbols);
        return err;
    }

    err = cc_run_main_pass(input_path, symbols, output_path);
    if (err != 0) {
        fprintf(stderr, "cc: main pass failed for %s (err=%d)\n", input_path, err);
    }

    cc_symbols_free(symbols);
    return err;
}

int cc_compile(const char *input_path, const char *output_path) {
    return cc_compile_with_config(input_path, output_path, NULL);
}
