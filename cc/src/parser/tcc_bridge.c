#include "tcc_bridge.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Thin wrappers around patched TCC hooks when available. When built without
// CC_TCC_EXT_AVAILABLE, we provide stubs that return NULL.

#ifdef CC_TCC_EXT_AVAILABLE
#include <tcc.h>
/* tcc.h redefines malloc/free to TCC's internal allocators; we want libc here. */
#ifdef malloc
#undef malloc
#endif
#ifdef free
#undef free
#endif
#ifdef strdup
#undef strdup
#endif

/* The patched TCC should export these. Mark weak so we can still link if the
   extension is absent, and fall back to stubs at runtime. */
__attribute__((weak)) struct CCASTStubRoot* cc_tcc_parse_to_ast(const char* preprocessed_path, const char* original_path, CCSymbolTable* symbols);
__attribute__((weak)) void cc_tcc_free_ast(struct CCASTStubRoot* r);
__attribute__((weak)) void tcc_set_ext_parser(struct TCCExtParser const *p);
extern const struct TCCExtParser cc_ext_parser;

// Call into patched TCC to parse and return an opaque AST root.
CCASTRoot* cc_tcc_bridge_parse_to_ast(const char* preprocessed_path, const char* original_path, CCSymbolTable* symbols) {
    if (!preprocessed_path || !cc_tcc_parse_to_ast) return NULL;
    // symbols currently unused; reserved for constexpr tables.
    (void)symbols;
    if (tcc_set_ext_parser) {
        tcc_set_ext_parser(&cc_ext_parser);
    }
    struct CCASTStubRoot* r = cc_tcc_parse_to_ast(preprocessed_path, original_path, symbols);
    if (!r) return NULL;
    CCASTRoot* root = (CCASTRoot*)malloc(sizeof(CCASTRoot));
    if (!root) {
        cc_tcc_free_ast(r);
        return NULL;
    }
    memset(root, 0, sizeof(*root));
    root->lowered_path = strdup(preprocessed_path);
    if (!root->lowered_path) {
        cc_tcc_free_ast(r);
        free(root);
        return NULL;
    }
    root->tcc_root = r;
    root->nodes = (const struct CCASTStubNode*)r->nodes;
    root->node_count = r->count;
    return root;
}

void cc_tcc_bridge_free_ast(CCASTRoot* root) {
    if (!root) return;
    const char* keep_pp = getenv("CC_KEEP_PP");
    if (root->lowered_is_temp && root->lowered_path && !(keep_pp && keep_pp[0] == '1')) {
        unlink(root->lowered_path);
    }
    if (root->lowered_path) {
        free(root->lowered_path);
        root->lowered_path = NULL;
    }
    if (root->tcc_root) {
        cc_tcc_free_ast((struct CCASTStubRoot*)root->tcc_root);
    }
    free(root);
}
#else

CCASTRoot* cc_tcc_bridge_parse_to_ast(const char* preprocessed_path, const char* original_path, CCSymbolTable* symbols) {
    (void)preprocessed_path;
    (void)original_path;
    (void)symbols;
    return NULL;
}

void cc_tcc_bridge_free_ast(CCASTRoot* root) {
    free(root);
}

#endif

