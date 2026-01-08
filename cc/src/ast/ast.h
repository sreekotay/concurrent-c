#ifndef CC_AST_AST_H
#define CC_AST_AST_H

// Minimal placeholder AST used until TCC hook integration lands.
// Tracks source path and an optional underlying TCC AST handle (when built
// with TCC_EXT=1 against patched TCC).
typedef struct CCASTRoot {
    const char* source_path;
    void* tcc_root; // opaque TCC AST when available
} CCASTRoot;

#endif // CC_AST_AST_H

