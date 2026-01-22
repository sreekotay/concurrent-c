# TCC Extension Points

This document describes the hooks added to TCC for Concurrent C integration.

## 1. AST Stub Recording

Records lightweight AST nodes during parsing for CC visitor passes.

```c
// tcc.h - Node types
enum CCASTStubKind {
    CC_AST_NODE_DECL,       // Variable/function declaration
    CC_AST_NODE_EXPR,       // Expression
    CC_AST_NODE_STMT,       // Statement (@defer, @nursery, spawn)
    CC_AST_NODE_ARENA,      // @arena block
    CC_AST_NODE_CALL,       // Function call (with UFCS metadata)
    CC_AST_NODE_AWAIT,      // await expression
    CC_AST_NODE_SEND_TAKE,  // Channel send/take
    CC_AST_NODE_SUBSLICE,   // Slice subscript
    CC_AST_NODE_CLOSURE,    // Closure literal
};

// TCCState extensions
struct TCCState {
    struct CCASTStubNode* cc_nodes;
    int cc_nodes_count, cc_nodes_cap;
    int* cc_node_stack;
    int cc_node_stack_top, cc_node_stack_cap;
    // ...
};
```

## 2. UFCS Support

Tolerates member-call syntax (`x.method(args)`) on receivers without the member.

```c
// TCCState - UFCS scratch
int cc_last_member_tok;       // Method token
int cc_last_member_flags;     // bit0=arrow(->), bit1=recv_is_ptr
int cc_last_member_line, cc_last_member_col;
char cc_last_recv_type[128];  // Receiver type name (e.g., "Point")
```

When `x.method(...)` is encountered and `method` is not a field:
1. Record the call site with UFCS metadata
2. Extract receiver type for type-qualified dispatch
3. Drop receiver, parse as `method(...)` call

## 3. External Parser Hooks

Allows CC frontend to handle custom statements.

```c
// tcc.h
struct TCCExtParser {
    int (*try_cc_stmt)(void);  // Return 1 if handled
};

// libtcc.c
void tcc_set_ext_parser(struct TCCExtParser const *p);
```

## 4. New Tokens

```c
// tcc.h
#define TOK_CC_ARROW 0xa5  // => (closure arrow)
```

## 5. Guarded Extensions

All extensions are behind `CONFIG_CC_EXT`:

```c
#ifdef CONFIG_CC_EXT
    // CC-specific code
#endif
```

This ensures vanilla TCC builds are unaffected.