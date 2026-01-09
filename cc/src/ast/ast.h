#ifndef CC_AST_AST_H
#define CC_AST_AST_H

// Minimal placeholder AST used until full CC AST lands.
// When built with TCC_EXT=1, we also expose the recorded stub nodes.
struct CCASTStubNode;

typedef struct CCASTRoot {
    const char* original_path; /* borrowed */
    char* lowered_path;        /* owned (may point at a temp file) */
    int lowered_is_temp;
    void* tcc_root; // opaque TCC stub root when available
    const struct CCASTStubNode* nodes;
    int node_count;
} CCASTRoot;

#endif // CC_AST_AST_H

