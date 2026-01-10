#include "parse.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>

#include "tcc_bridge.h"
#include "preprocess/preprocess.h"

int cc_parse_to_ast(const char* input_path, CCSymbolTable* symbols, CCASTRoot** out_root) {
    if (!input_path || !out_root) {
        return EINVAL;
    }
#ifdef CC_TCC_EXT_AVAILABLE
    char tmp_path[128];
    int pp_err = cc_preprocess_file(input_path, tmp_path, sizeof(tmp_path));
    const char* use_path = (pp_err == 0) ? tmp_path : input_path;

    CCASTRoot* root = cc_tcc_bridge_parse_to_ast(use_path, input_path, symbols);
    if (root) {
        root->original_path = input_path;
        if (pp_err == 0) {
            root->lowered_is_temp = 1;
        }
        *out_root = root;
        return 0;
    }
    // Fall back to stub when hook is unavailable or returns NULL.
#endif
    // Fallback: dummy AST so the pipeline keeps running when hooks are absent.
    (void)symbols;
    *out_root = (CCASTRoot*)malloc(sizeof(CCASTRoot));
    if (!*out_root) return ENOMEM;
    (*out_root)->original_path = input_path;
    (*out_root)->lowered_path = NULL;
    (*out_root)->lowered_is_temp = 0;
    (*out_root)->tcc_root = NULL;
    (*out_root)->nodes = NULL;
    (*out_root)->node_count = 0;
    return 0;
}

void cc_free_ast(CCASTRoot* root) {
    if (!root) return;
#ifdef CC_TCC_EXT_AVAILABLE
    if (root->tcc_root) {
        cc_tcc_bridge_free_ast(root);
        return;
    }
#endif
    if (root->lowered_path) {
        if (root->lowered_is_temp) unlink(root->lowered_path);
        free(root->lowered_path);
    }
    free(root);
}

