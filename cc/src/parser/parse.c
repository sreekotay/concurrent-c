#include "parse.h"

#include <errno.h>
#include <stdlib.h>

#include "tcc_bridge.h"

int cc_parse_to_ast(const char* input_path, CCSymbolTable* symbols, CCASTRoot** out_root) {
    if (!input_path || !out_root) {
        return EINVAL;
    }
#ifdef CC_TCC_EXT_AVAILABLE
    CCASTRoot* root = cc_tcc_parse_to_ast(input_path, symbols);
    if (root) {
        *out_root = root;
        return 0;
    }
    // Fall back to stub when hook is unavailable or returns NULL.
#endif
    // Fallback: dummy AST so the pipeline keeps running when hooks are absent.
    (void)symbols;
    *out_root = (CCASTRoot*)malloc(sizeof(CCASTRoot));
    if (!*out_root) return ENOMEM;
    (*out_root)->source_path = input_path;
    (*out_root)->tcc_root = NULL;
    return 0;
}

void cc_free_ast(CCASTRoot* root) {
    if (!root) return;
    // TCC AST slab is owned by TCC; leak-safe for now to avoid double-free issues while recorder stabilizes.
    free(root);
}

