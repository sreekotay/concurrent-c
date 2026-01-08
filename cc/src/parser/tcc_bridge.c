#include "tcc_bridge.h"

#include <stdlib.h>

// Thin wrappers around patched TCC hooks when available. When built without
// CC_TCC_EXT_AVAILABLE, we provide stubs that return NULL.

#ifdef CC_TCC_EXT_AVAILABLE
#include <tcc.h>

// Call into patched TCC to parse and return an opaque AST root.
CCASTRoot* cc_tcc_parse_to_ast(const char* path, CCSymbolTable* symbols) {
    if (!path) return NULL;
    // symbols currently unused; reserved for constexpr tables.
    (void)symbols;
    struct CCASTStubRoot* r = cc_tcc_parse_to_ast(path, symbols);
    if (!r) return NULL;
    CCASTRoot* root = (CCASTRoot*)malloc(sizeof(CCASTRoot));
    if (!root) {
        cc_tcc_free_ast(r);
        return NULL;
    }
    root->source_path = path;
    root->tcc_root = r;
    return root;
}

void cc_tcc_free_ast(CCASTRoot* root) {
    if (!root) return;
    if (root->tcc_root) {
        cc_tcc_free_ast((struct CCASTStubRoot*)root->tcc_root);
    }
    free(root);
}
#else

CCASTRoot* cc_tcc_parse_to_ast(const char* path, CCSymbolTable* symbols) {
    (void)path;
    (void)symbols;
    return NULL;
}

void cc_tcc_free_ast(CCASTRoot* root) {
    free(root);
}

#endif

