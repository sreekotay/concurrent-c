#ifndef CC_CCCN_AST_H
#define CC_CCCN_AST_H

#include "ast/ast.h"

/* 
 * CCCN (CCC Next) AST Extensions.
 * We use the base CCASTNode but define richer unions for AST-to-AST lowering.
 */

typedef struct CCCNNode {
    CCASTKind kind;
    CCSpan span;
    
    union {
        struct {
            const char* name;
            struct CCCNNode** params;
            int params_len;
            struct CCCNNode* body;
            int is_async;
        } fn;
        
        struct {
            struct CCCNNode* receiver;
            const char* method;
            struct CCCNNode** args;
            int args_len;
            int is_ufcs;
        } call;
        
        struct {
            const char* name;
            struct CCCNNode* init;
        } let;
        
        struct {
            struct CCCNNode** stmts;
            int stmts_len;
        } block;
        
        struct {
            struct CCCNNode* expr;
        } await_expr;

        struct {
            const char* value;
        } literal;

        struct {
            const char* name;
        } ident;
    } as;

    /* Metadata for lowering */
    void* type_info; 
} CCCNNode;

typedef struct CCCNRoot {
    const char* filename;
    CCCNNode** items;
    int items_len;
} CCCNRoot;

#endif /* CC_CCCN_AST_H */
