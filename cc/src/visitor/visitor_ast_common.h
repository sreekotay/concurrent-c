#ifndef CC_VISITOR_AST_COMMON_H
#define CC_VISITOR_AST_COMMON_H

/* 
 * Shared AST node view for visitor passes.
 * This matches the layout emitted by the TCC-based stub-AST recorder.
 */

typedef struct CCNodeView {
    int kind;
    int parent;
    const char* file;
    int line_start;
    int line_end;
    int col_start;
    int col_end;
    int aux1;
    int aux2;
    const char* aux_s1;
    const char* aux_s2;
} CCNodeView;

/* Common AST Node Kinds (from async_ast.c / checker.c) */
enum {
    CC_AST_KIND_UNKNOWN = 0,
    CC_AST_KIND_DECL = 1,
    CC_AST_KIND_BLOCK = 2,
    CC_AST_KIND_STMT = 3,
    CC_AST_KIND_ARENA = 4,
    CC_AST_KIND_CALL = 5,
    CC_AST_KIND_AWAIT = 6,
    CC_AST_KIND_SEND_TAKE = 7,
    CC_AST_KIND_SUBSLICE = 8,
    CC_AST_KIND_CLOSURE = 9,
    CC_AST_KIND_IDENT = 10,
    CC_AST_KIND_CONST = 11,
    CC_AST_KIND_DECL_ITEM = 12,
    CC_AST_KIND_MEMBER = 13,
    CC_AST_KIND_ASSIGN = 14,
    CC_AST_KIND_RETURN = 15,
    CC_AST_KIND_PARAM = 16,
    CC_AST_KIND_FUNC = 17,
};

#endif /* CC_VISITOR_AST_COMMON_H */
