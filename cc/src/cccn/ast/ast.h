#ifndef CC_AST_NODE_H
#define CC_AST_NODE_H

#include <stddef.h>
#include <stdint.h>

/*
 * CC AST - A proper AST for the Concurrent-C language.
 *
 * Naming: Types use `CCN` prefix meaning "CC Node" (not "CCCN").
 * - CCNNode = Concurrent-C Node
 * - CCNSpan = Concurrent-C Span
 * - etc.
 *
 * This distinguishes from the existing transitional types (CCASTNode, CCSpan)
 * in src/ast/ast.h which are used by the old ccc compiler.
 *
 * Design goals:
 * 1. Rich enough to represent all CC constructs before lowering.
 * 2. Supports AST-to-AST transformations (no text patching).
 * 3. Clean separation between parsing, lowering, and codegen.
 *
 * STRING OWNERSHIP CONVENTION:
 * - All string fields (const char*) in CCNNode are OWNED and must be strdup'd.
 * - ccn_node_free() is responsible for freeing all owned strings.
 * - ccn_node_clone() must strdup all strings.
 * - Exception: CCNSpan.file is borrowed from the input filename.
 */

/* ========================================================================== */
/* Source Location                                                            */
/* ========================================================================== */

typedef struct {
    const char* file;  /* Borrowed pointer to filename */
    int line;
    int col;
} CCNLoc;

typedef struct {
    CCNLoc start;
    CCNLoc end;
} CCNSpan;

/* ========================================================================== */
/* Node Kinds                                                                 */
/* ========================================================================== */

typedef enum {
    /* Special */
    CCN_ERROR = 0,
    
    /* Top-level */
    CCN_FILE,
    CCN_FUNC_DECL,
    CCN_VAR_DECL,
    CCN_TYPEDEF,
    CCN_STRUCT_DECL,
    CCN_STRUCT_FIELD,
    CCN_ENUM_DECL,
    CCN_ENUM_VALUE,
    CCN_INCLUDE,
    
    /* Types */
    CCN_TYPE_NAME,        /* int, void, MyStruct */
    CCN_TYPE_PTR,         /* T* */
    CCN_TYPE_ARRAY,       /* T[N] */
    CCN_TYPE_SLICE,       /* T[] */
    CCN_TYPE_CHAN_TX,     /* T[~N >] */
    CCN_TYPE_CHAN_RX,     /* T[~N <] */
    CCN_TYPE_OPTIONAL,    /* T? */
    CCN_TYPE_RESULT,      /* T!>(E) */
    CCN_TYPE_FUNC,        /* fn(...) -> T */
    
    /* Statements */
    CCN_BLOCK,
    CCN_STMT_EXPR,        /* expr; */
    CCN_STMT_RETURN,
    CCN_STMT_IF,
    CCN_STMT_WHILE,
    CCN_STMT_FOR,
    CCN_STMT_FOR_AWAIT,   /* for await ... */
    CCN_STMT_SWITCH,
    CCN_STMT_BREAK,
    CCN_STMT_CONTINUE,
    CCN_STMT_GOTO,
    CCN_STMT_LABEL,
    
    /* CC-specific statements */
    CCN_STMT_NURSERY,     /* @nursery { ... } */
    CCN_STMT_ARENA,       /* @arena { ... } */
    CCN_STMT_DEFER,       /* @defer stmt; */
    CCN_STMT_SPAWN,       /* spawn(closure); */
    CCN_STMT_MATCH,       /* @match { case ... } */
    
    /* Expressions */
    CCN_EXPR_IDENT,
    CCN_EXPR_LITERAL_INT,
    CCN_EXPR_LITERAL_FLOAT,
    CCN_EXPR_LITERAL_STRING,
    CCN_EXPR_LITERAL_CHAR,
    CCN_EXPR_CALL,        /* func(args) */
    CCN_EXPR_METHOD,      /* receiver.method(args) - UFCS before lowering */
    CCN_EXPR_FIELD,       /* expr.field */
    CCN_EXPR_INDEX,       /* expr[index] */
    CCN_EXPR_UNARY,       /* !x, -x, *x, &x */
    CCN_EXPR_BINARY,      /* a + b, a && b */
    CCN_EXPR_TERNARY,     /* a ? b : c */
    CCN_EXPR_CAST,        /* (T)expr */
    CCN_EXPR_SIZEOF,
    CCN_EXPR_ASSIGN,      /* a = b */
    CCN_EXPR_COMPOUND,    /* (T){ ... } */
    CCN_EXPR_INIT_LIST,   /* { .x = 1, .y = 2 } */
    
    /* CC-specific expressions */
    CCN_EXPR_CLOSURE,     /* () => expr, (x) => { ... } */
    CCN_EXPR_AWAIT,       /* await expr */
    CCN_EXPR_CHAN_SEND,   /* tx <- val (sugar) */
    CCN_EXPR_CHAN_RECV,   /* <-rx (sugar) */
    CCN_EXPR_OK,          /* cc_ok(val) */
    CCN_EXPR_ERR,         /* cc_err(err) */
    CCN_EXPR_SOME,        /* cc_some(val) */
    CCN_EXPR_NONE,        /* cc_none() */
    CCN_EXPR_TRY,         /* try expr */
    
    /* Match arms */
    CCN_MATCH_ARM,
    
    /* Parameters */
    CCN_PARAM,
    
    /* Designators (for init lists) */
    CCN_DESIGNATOR,
    
    CCN_KIND_COUNT
} CCNKind;

/* ========================================================================== */
/* Operator Enums                                                             */
/* ========================================================================== */

typedef enum {
    CCN_OP_ADD, CCN_OP_SUB, CCN_OP_MUL, CCN_OP_DIV, CCN_OP_MOD,
    CCN_OP_BAND, CCN_OP_BOR, CCN_OP_BXOR, CCN_OP_SHL, CCN_OP_SHR,
    CCN_OP_LAND, CCN_OP_LOR,
    CCN_OP_EQ, CCN_OP_NE, CCN_OP_LT, CCN_OP_LE, CCN_OP_GT, CCN_OP_GE,
    CCN_OP_ASSIGN, CCN_OP_ADD_ASSIGN, CCN_OP_SUB_ASSIGN,
    CCN_OP_MUL_ASSIGN, CCN_OP_DIV_ASSIGN, CCN_OP_MOD_ASSIGN,
    CCN_OP_COMMA,
} CCNBinaryOp;

typedef enum {
    CCN_OP_NEG, CCN_OP_NOT, CCN_OP_BNOT,
    CCN_OP_DEREF, CCN_OP_ADDR,
    CCN_OP_PRE_INC, CCN_OP_PRE_DEC,
    CCN_OP_POST_INC, CCN_OP_POST_DEC,
} CCNUnaryOp;

/* ========================================================================== */
/* Forward Declarations                                                       */
/* ========================================================================== */

typedef struct CCNNode CCNNode;
typedef struct CCNType CCNType;

/* ========================================================================== */
/* Node List (dynamic array)                                                  */
/* ========================================================================== */

typedef struct {
    CCNNode** items;
    int len;
    int cap;
} CCNNodeList;

/* ========================================================================== */
/* The Main AST Node                                                          */
/* ========================================================================== */

struct CCNNode {
    CCNKind kind;
    CCNSpan span;
    
    /* Type annotation (filled during type checking, NULL before) */
    CCNType* type;
    
    /* Kind-specific data */
    union {
        /* CCN_FILE */
        struct {
            const char* path;
            CCNNodeList items;  /* top-level declarations */
        } file;
        
        /* CCN_FUNC_DECL */
        struct {
            const char* name;
            CCNNode* return_type;   /* CCN_TYPE_* */
            CCNNodeList params;     /* CCN_PARAM nodes */
            CCNNode* body;          /* CCN_BLOCK or NULL for forward decl */
            unsigned is_async : 1;
            unsigned is_static : 1;
            unsigned is_noblock : 1;
        } func;
        
        /* CCN_VAR_DECL */
        struct {
            const char* name;
            CCNNode* type_node;     /* CCN_TYPE_* or NULL for inferred */
            CCNNode* init;          /* initializer or NULL */
            unsigned is_static : 1;
            unsigned is_const : 1;
        } var;
        
        /* CCN_STRUCT_DECL */
        struct {
            const char* name;       /* struct/union tag name */
            CCNNodeList fields;     /* list of field nodes */
            unsigned is_union : 1;  /* 1 if union, 0 if struct */
        } struct_decl;
        
        /* CCN_STRUCT_FIELD - not a top-level node, used within struct_decl.fields */
        struct {
            const char* name;       /* field name */
            const char* type_str;   /* field type as string */
        } struct_field;
        
        /* CCN_ENUM_DECL */
        struct {
            const char* name;       /* enum tag name */
            CCNNodeList values;     /* list of enum value nodes */
        } enum_decl;
        
        /* CCN_ENUM_VALUE */
        struct {
            const char* name;       /* value name */
            int value;              /* value */
        } enum_value;
        
        /* CCN_TYPEDEF */
        struct {
            const char* name;       /* typedef name */
            const char* type_str;   /* underlying type as string */
        } typedef_decl;
        
        /* CCN_PARAM */
        struct {
            const char* name;
            CCNNode* type_node;
        } param;
        
        /* CCN_TYPE_NAME */
        struct {
            const char* name;
        } type_name;
        
        /* CCN_TYPE_PTR, CCN_TYPE_OPTIONAL */
        struct {
            CCNNode* base;
        } type_ptr;
        
        /* CCN_TYPE_ARRAY, CCN_TYPE_SLICE */
        struct {
            CCNNode* elem;
            CCNNode* size;  /* NULL for slices */
        } type_array;
        
        /* CCN_TYPE_CHAN_TX, CCN_TYPE_CHAN_RX */
        struct {
            CCNNode* elem;
            CCNNode* capacity;
        } type_chan;
        
        /* CCN_TYPE_RESULT */
        struct {
            CCNNode* ok_type;
            CCNNode* err_type;
        } type_result;
        
        /* CCN_BLOCK */
        struct {
            CCNNodeList stmts;
        } block;
        
        /* CCN_STMT_EXPR */
        struct {
            CCNNode* expr;
        } stmt_expr;
        
        /* CCN_STMT_RETURN */
        struct {
            CCNNode* value;  /* NULL for void return */
        } stmt_return;
        
        /* CCN_STMT_IF */
        struct {
            CCNNode* cond;
            CCNNode* then_branch;
            CCNNode* else_branch;  /* NULL if no else */
        } stmt_if;
        
        /* CCN_STMT_WHILE, CCN_STMT_FOR_AWAIT */
        struct {
            CCNNode* cond;
            CCNNode* body;
        } stmt_while;
        
        /* CCN_STMT_FOR */
        struct {
            CCNNode* init;
            CCNNode* cond;
            CCNNode* incr;
            CCNNode* body;
        } stmt_for;
        
        /* CCN_STMT_NURSERY, CCN_STMT_ARENA */
        struct {
            const char* name;       /* optional arena/nursery name */
            CCNNode* size;          /* arena size expr, NULL for default */
            CCNNode* body;
            CCNNodeList closing;    /* channels to close (nursery) */
        } stmt_scope;
        
        /* CCN_STMT_DEFER */
        struct {
            CCNNode* stmt;
        } stmt_defer;
        
        /* CCN_STMT_SPAWN */
        struct {
            CCNNode* closure;
        } stmt_spawn;
        
        /* CCN_STMT_MATCH */
        struct {
            CCNNodeList arms;       /* CCN_MATCH_ARM nodes */
        } stmt_match;
        
        /* CCN_MATCH_ARM */
        struct {
            CCNNode* pattern;       /* e.g., rx.recv(&val) */
            CCNNode* body;
        } match_arm;
        
        /* CCN_EXPR_IDENT */
        struct {
            const char* name;
        } expr_ident;
        
        /* CCN_EXPR_LITERAL_INT */
        struct {
            int64_t value;
        } expr_int;
        
        /* CCN_EXPR_LITERAL_FLOAT */
        struct {
            double value;
        } expr_float;
        
        /* CCN_EXPR_LITERAL_STRING, CCN_EXPR_LITERAL_CHAR */
        struct {
            const char* value;
            size_t len;
        } expr_string;
        
        /* CCN_EXPR_COMPOUND - compound literal { val, val, ... } */
        struct {
            CCNNodeList values;
        } expr_compound;
        
        /* CCN_EXPR_CALL */
        struct {
            CCNNode* callee;        /* function name or expr */
            CCNNodeList args;
        } expr_call;
        
        /* CCN_EXPR_METHOD - UFCS: receiver.method(args) */
        struct {
            CCNNode* receiver;
            const char* method;
            CCNNodeList args;
        } expr_method;
        
        /* CCN_EXPR_FIELD */
        struct {
            CCNNode* object;
            const char* field;
            unsigned is_arrow : 1;  /* -> vs . */
        } expr_field;
        
        /* CCN_EXPR_INDEX */
        struct {
            CCNNode* array;
            CCNNode* index;
        } expr_index;
        
        /* CCN_EXPR_UNARY */
        struct {
            CCNUnaryOp op;
            CCNNode* operand;
            unsigned is_postfix : 1;
        } expr_unary;
        
        /* CCN_EXPR_SIZEOF */
        struct {
            const char* type_str;  /* Type string if sizeof(type), NULL if sizeof(expr) */
            CCNNode* expr;         /* Expression if sizeof(expr), NULL if sizeof(type) */
        } expr_sizeof;
        
        /* CCN_EXPR_BINARY */
        struct {
            CCNBinaryOp op;
            CCNNode* lhs;
            CCNNode* rhs;
        } expr_binary;
        
        /* CCN_EXPR_TERNARY */
        struct {
            CCNNode* cond;
            CCNNode* then_expr;
            CCNNode* else_expr;
        } expr_ternary;
        
        /* CCN_EXPR_CAST */
        struct {
            CCNNode* type_node;
            CCNNode* expr;
        } expr_cast;
        
        /* CCN_EXPR_CLOSURE */
        struct {
            CCNNodeList params;     /* CCN_PARAM nodes */
            CCNNode* body;          /* CCN_BLOCK or single expr */
            CCNNodeList captures;   /* capture list [x, &y] */
            unsigned is_unsafe : 1;
        } expr_closure;
        
        /* CCN_EXPR_AWAIT */
        struct {
            CCNNode* expr;
        } expr_await;
        
        /* CCN_EXPR_OK, CCN_EXPR_ERR, CCN_EXPR_SOME */
        struct {
            CCNNode* value;
        } expr_result;
        
        /* CCN_EXPR_TRY */
        struct {
            CCNNode* expr;
        } expr_try;
        
        /* CCN_INCLUDE */
        struct {
            const char* path;
            unsigned is_system : 1;  /* <...> vs "..." */
        } include;
    } as;
};

/* ========================================================================== */
/* Root File                                                                  */
/* ========================================================================== */

/* Closure definition - collected during lowering, used by codegen */
typedef struct CCClosureDef {
    int id;
    int param_count;
    CCNNodeList captures;   /* captured variable references */
    const char** capture_types;  /* type strings for each capture (parallel to captures) */
    CCNNode* body;          /* closure body */
    CCNNodeList params;     /* closure parameters */
} CCClosureDef;

typedef struct {
    const char* filename;
    CCNNode* root;          /* CCN_FILE node */
    
    /* Closure definitions collected during lowering */
    int closure_count;
    CCClosureDef* closure_defs;
    
    /* Flags for CC constructs used */
    int has_nursery;        /* True if @nursery is used */
    int has_channels;       /* True if channels are used */
    
    /* Memory management: all nodes allocated from this arena */
    void* arena;
} CCNFile;

/* ========================================================================== */
/* AST Construction Helpers                                                   */
/* ========================================================================== */

/* Allocate a new node (zero-initialized) */
CCNNode* ccn_node_new(CCNKind kind);

/* List operations */
void ccn_list_push(CCNNodeList* list, CCNNode* node);
void ccn_list_free(CCNNodeList* list);

/* Create specific node types */
CCNNode* ccn_make_ident(const char* name, CCNSpan span);
CCNNode* ccn_make_int_lit(int64_t value, CCNSpan span);
CCNNode* ccn_make_string_lit(const char* value, size_t len, CCNSpan span);
CCNNode* ccn_make_call(CCNNode* callee, CCNNodeList args, CCNSpan span);
CCNNode* ccn_make_method(CCNNode* receiver, const char* method, CCNNodeList args, CCNSpan span);
CCNNode* ccn_make_block(CCNNodeList stmts, CCNSpan span);
CCNNode* ccn_make_return(CCNNode* value, CCNSpan span);

/* Deep clone a node (for AST transformations) */
CCNNode* ccn_node_clone(CCNNode* node);

/* Free a node tree */
void ccn_node_free(CCNNode* node);

/* Debug printing */
void ccn_node_dump(CCNNode* node, int indent);

#endif /* CC_AST_NODE_H */
