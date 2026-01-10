#ifndef CC_AST_AST_H
#define CC_AST_AST_H

// Minimal CC AST shim. As parser hooks land, populate these nodes instead of the TCC stub.
//
// Transitional note:
// - The current pipeline uses a patched TCC to return a stub-node side table
//   (see third_party/tcc-patches/0001-cc-ext-hooks.patch) and the visitor consumes
//   `root->nodes/root->node_count` for UFCS and arena lowering.
// - Keep those fields intact until the CC AST `items` array is populated.

struct CCASTStubNode; /* from patched TCC (opaque here) */

typedef enum {
    CC_AST_UNKNOWN = 0,
    CC_AST_FILE,
    CC_AST_FN,
    CC_AST_PARAM,
    CC_AST_BLOCK,
    CC_AST_LET,
    CC_AST_ASSIGN,
    CC_AST_IDENT,
    CC_AST_CALL,
    CC_AST_AWAIT,
    CC_AST_SEND,
    CC_AST_RECV,
    CC_AST_SEND_TAKE,
    CC_AST_SUBSLICE,
    CC_AST_SLICE_LITERAL,
    CC_AST_RESULT_OK,
    CC_AST_RESULT_ERR,
    CC_AST_OPTION_SOME,
    CC_AST_OPTION_NONE,
    CC_AST_MATCH,
    CC_AST_FOR_AWAIT,
    CC_AST_NURSERY,
    CC_AST_RETURN,
    CC_AST_LITERAL,
} CCASTKind;

typedef struct {
    const char* start_path; /* file name */
    int line;
    int column;
} CCSpan;

typedef struct CCASTNode {
    CCASTKind kind;
    CCSpan span;
    // Generic children (placeholder until a full union is defined).
    struct CCASTNode** children;
    int children_len;
    // Ident/call data (temporary minimal fields).
    const char* name;
    struct CCASTNode** args;
    int args_len;
} CCASTNode;

typedef struct CCASTRoot {
    const char* original_path; /* borrowed */
    char* lowered_path;        /* owned (may point at a temp file) */
    int lowered_is_temp;
    // Top-level items (functions, declarations).
    CCASTNode** items;
    int items_len;
    // Opaque handle to upstream TCC stub (for transitional builds).
    void* tcc_root;
    // Stub-node side table (transitional; used by current visitor lowering).
    const struct CCASTStubNode* nodes;
    int node_count;
} CCASTRoot;

#endif // CC_AST_AST_H

