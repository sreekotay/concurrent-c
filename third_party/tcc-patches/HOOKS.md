# TCC Extension Points for Concurrent C

This document describes the hooks added to TCC for Concurrent C integration.
All extensions are guarded by `CONFIG_CC_EXT` to ensure vanilla TCC builds are unaffected.

## 1. External Parser API (`TCCExtParser`)

The primary integration point. CC provides an external parser that handles CC-specific syntax.

```c
// tcc.h
struct TCCExtParser {
    int (*try_cc_decl)(void);      // Try parsing CC declaration (unused currently)
    int (*try_cc_stmt)(void);      // Try parsing CC statement (unused currently)
    int (*try_cc_at_stmt)(void);   // Parse @defer, @nursery, @arena statements
    int (*try_cc_unary)(void);     // Try parsing CC unary expression (unused)
    int (*try_cc_spawn)(void);     // Parse spawn(...) statement
    int (*try_cc_closure)(void);   // Parse closure: [captures](params) => body
    int (*try_cc_closure_single_param)(int ident_tok, int start_line, int start_col);
                                   // Parse single-param closure: x => body
};

// libtcc.c - Set the external parser
PUB_FUNC void tcc_set_ext_parser(struct TCCExtParser const *p);
```

### Hook Return Values
- Return `1` if the construct was handled
- Return `0` to fall back to normal TCC parsing

### Implementation
The CC frontend implements these hooks in `cc/src/parser/cc_ext_parser.c`, which:
- Includes TCC headers to access parser internals (`next()`, `tok`, `expr_eq()`, etc.)
- Calls `cc_ast_record_start()/cc_ast_record_end()` to record AST nodes
- Uses TCC's expression/block parsing for sub-expressions

## 2. AST Stub Recording

Records lightweight AST nodes during parsing for CC visitor passes to rewrite.

```c
// tcc.h - Node types
enum {
    CC_AST_NODE_UNKNOWN = 0,
    CC_AST_NODE_DECL = 1,       // Declaration
    CC_AST_NODE_BLOCK = 2,      // Block scope
    CC_AST_NODE_STMT = 3,       // Statement (@defer, @nursery, spawn)
    CC_AST_NODE_ARENA = 4,      // @arena block
    CC_AST_NODE_CALL = 5,       // Function call (with UFCS metadata)
    CC_AST_NODE_AWAIT = 6,      // await expression
    CC_AST_NODE_SEND_TAKE = 7,  // Channel send/take
    CC_AST_NODE_SUBSLICE = 8,   // Slice subscript
    CC_AST_NODE_CLOSURE = 9,    // Closure literal
    CC_AST_NODE_IDENT = 10,     // Identifier
    CC_AST_NODE_CONST = 11,     // Constant
    CC_AST_NODE_DECL_ITEM = 12, // Declaration item
    CC_AST_NODE_MEMBER = 13,    // Member access
    CC_AST_NODE_ASSIGN = 14,    // Assignment
    CC_AST_NODE_RETURN = 15,    // Return statement
    CC_AST_NODE_PARAM = 16,     // Function parameter
    CC_AST_NODE_FUNC = 17,      // Function definition
};

// Node structure
struct CCASTStubNode {
    int kind;                   // Node type (enum above)
    int parent;                 // Parent node index (-1 for root)
    char* file;                 // Source filename
    int line_start, line_end;   // Line span
    int col_start, col_end;     // Column span
    int aux1, aux2;             // Node-specific metadata
    char* aux_s1;               // Node-specific string (e.g., callee name)
    char* aux_s2;               // Node-specific string (e.g., receiver type)
};

// Recording API (cc_ast_record.h, included by tccgen.c)
void cc_ast_record_start(int kind);  // Push new node
void cc_ast_record_end(void);        // Pop and finalize node
```

## 3. UFCS Support

Tolerates member-call syntax (`x.method(args)`) on receivers without the member,
recording metadata for CC visitor to rewrite as `Type_method(&x, args)`.

```c
// TCCState - UFCS scratch fields
int cc_last_member_tok;       // Method token when UFCS detected
int cc_last_member_flags;     // bit0=arrow(->), bit1=recv_is_ptr
int cc_last_member_line;      // Source location
int cc_last_member_col;
char cc_last_recv_type[128];  // Receiver type name (e.g., "Point", "Vec_int")

// Sequence tracking for multiple UFCS calls on same line
int cc_ufcs_seq_line;
int cc_ufcs_seq_count;
```

### UFCS Flow
1. Parser encounters `x.method` where `method` is not a field of `x`
2. Records method token and receiver type in `cc_last_*` fields
3. On `(` for call, `cc_ast_record_call()` captures UFCS metadata in node's `aux2` and `aux_s2`
4. CC visitor rewrites source: `x.method(args)` → `Type_method(&x, args)`

## 4. New Tokens

```c
// tccpp.c
#define TOK_CC_ARROW 0xa5  // => (closure arrow)
```

Lexed when `=` is followed by `>`.

## 5. Parse-to-AST Entry Point

```c
// libtcc.c
PUB_FUNC struct CCASTStubRoot* cc_tcc_parse_to_ast(
    const char* preprocessed_path,  // File to parse
    const char* original_path,      // Original source (for include paths)
    struct CCSymbolTable* symbols   // Reserved for future use
);
```

Returns the recorded AST nodes for CC visitor passes to process.

## 6. Exposed TCC Internals

For external parser implementation, these TCC functions are exposed:

```c
// tcc.h - Parsing helpers (ST_FUNC = visible to external code)
ST_FUNC void next(void);           // Advance to next token
ST_FUNC void skip(int c);          // Expect and skip token
ST_FUNC void unget_tok(int t);     // Push back token
ST_FUNC void expr_eq(void);        // Parse assignment expression
ST_FUNC void block(int flags);     // Parse block statement
ST_FUNC void vpushi(int v);        // Push int to value stack
ST_FUNC void vpop(void);           // Pop value stack

// Global state accessible
extern int tok;                    // Current token
extern int tok_col;                // Current column
extern CString tokcstr;            // Token string value
extern BufferedFile* file;         // Current file (has line_num)
```

## 7. Build Configuration

Enable CC extensions in TCC build:
```makefile
CFLAGS += -DCONFIG_CC_EXT
```

The CC Makefile handles this automatically when building the bundled TCC.
