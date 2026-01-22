#include "tcc_bridge.h"

#ifdef CC_TCC_EXT_AVAILABLE
#include <tcc.h>
#include <string.h>

/* Access TCC internals */
extern int tok;
extern CValue tokc;
extern struct BufferedFile *file;

/* Use raw error function to avoid macro issues */
#undef tcc_error
#define tcc_error(fmt, ...) do { tcc_enter_state(tcc_state); _tcc_error_noabort(fmt, ##__VA_ARGS__); } while(0)

/* AST recording functions (defined in tccgen.c) */
extern void cc_ast_record_start(int kind);
extern void cc_ast_record_end(void);

/* Forward declarations */
static int cc_try_cc_decl(void);
static int cc_try_cc_stmt(void);
static int cc_try_cc_at_stmt(void);
static int cc_try_cc_unary(void);

const struct TCCExtParser cc_ext_parser = {
    .try_cc_decl = cc_try_cc_decl,
    .try_cc_stmt = cc_try_cc_stmt,
    .try_cc_at_stmt = cc_try_cc_at_stmt,
    .try_cc_unary = cc_try_cc_unary,
};

static int cc_try_cc_decl(void) { return 0; }
static int cc_try_cc_stmt(void) { return 0; }
static int cc_try_cc_unary(void) { return 0; }

/*
 * Parse @ statements: @arena, @arena_init, @defer, @nursery
 * Returns:
 *   0 = not handled (tok unchanged)
 *   1 = handled, caller should go to next statement
 *   2 = handled as block statement, caller should fall through with t='{'
 */
static int cc_try_cc_at_stmt(void) {
    if (tok != '@')
        return 0;
    
    next(); /* consume '@' */
    if (tok < TOK_UIDENT) {
        tcc_error("expected identifier after '@'");
        return 0;
    }
    
    const char *cc_at = get_tok_str(tok, NULL);
    
    /* Check for known @ keywords */
    if (strcmp(cc_at, "arena") != 0 &&
        strcmp(cc_at, "arena_init") != 0 &&
        strcmp(cc_at, "nursery") != 0 &&
        strcmp(cc_at, "defer") != 0) {
        tcc_error("unknown '@%s' block", cc_at);
        return 0;
    }
    
    /* --- @defer stmt; --- */
    if (strcmp(cc_at, "defer") == 0) {
        cc_ast_record_start(CC_AST_NODE_STMT);
        if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
            int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
            tcc_state->cc_nodes[idx].aux_s1 = tcc_strdup("defer");
        }
        next(); /* consume 'defer' */
        
        /* Consume tokens until ';' at depth 0 */
        int par = 0, br = 0, sq = 0;
        while (tok && tok != TOK_EOF) {
            if (tok == '(') par++;
            else if (tok == ')' && par > 0) par--;
            else if (tok == '{') br++;
            else if (tok == '}' && br > 0) br--;
            else if (tok == '[') sq++;
            else if (tok == ']' && sq > 0) sq--;
            else if (tok == ';' && par == 0 && br == 0 && sq == 0) {
                next(); /* consume ';' */
                break;
            }
            next();
        }
        cc_ast_record_end();
        return 1; /* handled, go again */
    }
    
    /* --- @nursery { ... } --- */
    if (strcmp(cc_at, "nursery") == 0) {
        cc_ast_record_start(CC_AST_NODE_STMT);
        if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
            int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
            tcc_state->cc_nodes[idx].aux_s1 = tcc_strdup("nursery");
        }
        tcc_state->cc_at_nursery_wrap = 1;
        next(); /* consume 'nursery' */
        
        /* Optional 'closing(...)' clause */
        if (tok >= TOK_IDENT && strcmp(get_tok_str(tok, NULL), "closing") == 0) {
            next(); /* consume 'closing' */
            if (tok != '(')
                tcc_error("expected '(' after @nursery closing");
            int depth = 1;
            next();
            while (tok && tok != TOK_EOF && depth > 0) {
                if (tok == '(') depth++;
                else if (tok == ')') depth--;
                next();
            }
        }
        if (tok != '{')
            tcc_error("expected '{' after @nursery");
        return 2; /* handled, fall through to block */
    }
    
    /* --- @arena or @arena_init { ... } --- */
    if (strcmp(cc_at, "arena") == 0 || strcmp(cc_at, "arena_init") == 0) {
        int is_arena_init = (strcmp(cc_at, "arena_init") == 0);
        next();
        
        tcc_state->cc_at_arena_name_tok = tok_alloc_const("arena");
        tcc_state->cc_at_arena_name_str = NULL;
        tcc_state->cc_at_arena_size_str = NULL;
        
        if (tok == '(') {
            CString sz;
            int depth = 1;
            int had_comma = 0;
            
            cstr_new(&sz);
            next(); /* after '(' */
            
            if (is_arena_init) {
                /* @arena_init(buf, size) */
                CString buf;
                cstr_new(&buf);
                
                while (tok && tok != ',' && depth > 0) {
                    if (tok == '(') depth++;
                    else if (tok == ')') { depth--; if (depth == 0) break; }
                    cstr_cat(&buf, get_tok_str(tok, &tokc), -1);
                    cstr_ccat(&buf, ' ');
                    next();
                }
                if (tok == ',') {
                    next();
                    depth = 1;
                    while (tok && depth > 0) {
                        if (tok == '(') depth++;
                        else if (tok == ')') { depth--; if (depth == 0) break; }
                        cstr_cat(&sz, get_tok_str(tok, &tokc), -1);
                        cstr_ccat(&sz, ' ');
                        next();
                    }
                }
                if (tok != ')') tcc_error("expected ')' after @arena_init(");
                
                if (buf.size >= 1 && buf.data[buf.size - 1] == ' ') buf.data[--buf.size] = '\0';
                if (sz.size >= 1 && sz.data[sz.size - 1] == ' ') sz.data[--sz.size] = '\0';
                cstr_ccat(&buf, '\0');
                cstr_ccat(&sz, '\0');
                
                CString packed;
                cstr_new(&packed);
                cstr_cat(&packed, "@buf:", -1);
                cstr_cat(&packed, buf.data, -1);
                cstr_ccat(&packed, ';');
                cstr_cat(&packed, sz.data, -1);
                cstr_ccat(&packed, '\0');
                tcc_state->cc_at_arena_size_str = tcc_strdup(packed.data);
                tcc_state->cc_at_arena_name_str = tcc_strdup("arena");
                cstr_free(&buf);
                cstr_free(&packed);
                next();
            } else {
                /* @arena(name?, size?) */
                if (tok >= TOK_IDENT) {
                    int first = tok;
                    next();
                    if (tok == ',' || tok == ')') {
                        tcc_state->cc_at_arena_name_tok = first;
                        tcc_state->cc_at_arena_name_str = tcc_strdup(get_tok_str(first, NULL));
                        if (tok == ',') {
                            had_comma = 1;
                            next();
                        }
                    } else {
                        cstr_cat(&sz, get_tok_str(first, NULL), -1);
                        cstr_ccat(&sz, ' ');
                        cstr_cat(&sz, get_tok_str(tok, &tokc), -1);
                        cstr_ccat(&sz, ' ');
                        had_comma = 1;
                        next();
                    }
                } else if (tok != ')') {
                    had_comma = 1;
                }
                
                while (tok && depth > 0) {
                    if (tok == '(') depth++;
                    else if (tok == ')') { depth--; if (depth == 0) break; }
                    if (had_comma) {
                        cstr_cat(&sz, get_tok_str(tok, &tokc), -1);
                        cstr_ccat(&sz, ' ');
                    }
                    next();
                }
                if (depth != 0)
                    tcc_error("expected ')' after @arena(");
                
                if (had_comma && sz.data && sz.size > 0) {
                    if (sz.size >= 1 && sz.data[sz.size - 1] == ' ')
                        sz.data[sz.size - 1] = '\0', sz.size--;
                    cstr_ccat(&sz, '\0');
                    tcc_state->cc_at_arena_size_str = tcc_strdup(sz.data);
                }
                next();
            }
            cstr_free(&sz);
        }
        
        if (tok != '{')
            tcc_error("expected '{' after @arena");
        
        cc_ast_record_start(CC_AST_NODE_ARENA);
        if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
            int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
            tcc_state->cc_nodes[idx].aux_s1 = tcc_state->cc_at_arena_name_str 
                ? tcc_state->cc_at_arena_name_str : tcc_strdup("arena");
            tcc_state->cc_nodes[idx].aux_s2 = tcc_state->cc_at_arena_size_str 
                ? tcc_state->cc_at_arena_size_str : tcc_strdup("kilobytes(4)");
        }
        tcc_state->cc_at_arena_wrap = 1;
        return 2; /* handled, fall through to block */
    }
    
    return 0;
}

#endif /* CC_TCC_EXT_AVAILABLE */