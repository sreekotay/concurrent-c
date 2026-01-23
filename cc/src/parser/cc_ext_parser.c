#include "tcc_bridge.h"

#ifdef CC_TCC_EXT_AVAILABLE
#include <tcc.h>
#include <string.h>

/* Access TCC internals */
extern int tok;
extern int tok_col;
extern CValue tokc;
extern struct BufferedFile *file;
extern int nocode_wanted;

/* Use raw error function to avoid macro issues */
#undef tcc_error
#define tcc_error(fmt, ...) do { tcc_enter_state(tcc_state); _tcc_error_noabort(fmt, ##__VA_ARGS__); } while(0)

/* AST recording functions (defined in tccgen.c) */
extern void cc_ast_record_start(int kind);
extern void cc_ast_record_end(void);

/* TCC parsing functions */
extern void next(void);
extern void skip(int c);
extern const char *get_tok_str(int v, CValue *cv);
extern int tok_alloc_const(const char *str);
extern char *tcc_strdup(const char *str);
extern void *tcc_mallocz(unsigned long size);
extern void tcc_free(void *ptr);
extern void cstr_new(CString *cstr);
extern void cstr_free(CString *cstr);
extern void cstr_cat(CString *cstr, const char *str, int len);
extern void cstr_ccat(CString *cstr, int ch);
extern void expr_eq(void);
extern void vpop(void);
extern void vpushi(int v);
extern void block(int flags);
extern void unget_tok(int last_tok);

/* Forward declarations */
static int cc_try_cc_decl(void);
static int cc_try_cc_stmt(void);
static int cc_try_cc_at_stmt(void);
static int cc_try_cc_unary(void);
static int cc_try_cc_spawn(void);
static int cc_try_cc_closure(void);
static int cc_try_cc_closure_single_param(int ident_tok, int start_line, int start_col);

const struct TCCExtParser cc_ext_parser = {
    .try_cc_decl = cc_try_cc_decl,
    .try_cc_stmt = cc_try_cc_stmt,
    .try_cc_at_stmt = cc_try_cc_at_stmt,
    .try_cc_unary = cc_try_cc_unary,
    .try_cc_spawn = cc_try_cc_spawn,
    .try_cc_closure = cc_try_cc_closure,
    .try_cc_closure_single_param = cc_try_cc_closure_single_param,
};

static int cc_try_cc_decl(void) { return 0; }
static int cc_try_cc_stmt(void) { return 0; }
static int cc_try_cc_unary(void) { return 0; }

/* Helper: parse closure body (block or expression) and record AST */
static void cc_parse_closure_body(void) {
    int saved_ncw = nocode_wanted;
    ++nocode_wanted;
    if (tcc_state) tcc_state->cc_in_closure_body++;
    
    if (tok == '{') {
        block(0);
        if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
            int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
            tcc_state->cc_nodes[idx].aux2 = 1; /* body is block */
        }
    } else {
        expr_eq();
        vpop();
        if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
            int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
            tcc_state->cc_nodes[idx].aux2 = 2; /* body is expr */
        }
    }
    
    if (tcc_state) tcc_state->cc_in_closure_body--;
    nocode_wanted = saved_ncw;
    
    /* Pin end span */
    if (tcc_state && file && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
        int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
        tcc_state->cc_nodes[idx].line_end = file->line_num;
        tcc_state->cc_nodes[idx].col_end = tok_col;
        tcc_state->cc_nodes[idx].aux2 |= (int)(1U << 31);
    }
}

/*
 * Parse closure literal. Handles:
 *   - [@unsafe] [captures](...) => body  (when tok is '@' or '[')
 *   - () => body                          (when tok is ')')
 *   - (x) => body, (x, y) => body         (when tok is identifier)
 *   - (int x) => body                     (when tok is type keyword)
 * 
 * Called after '(' has been consumed (tok is first token inside parens)
 * or when tok is '@' or '['.
 * 
 * Returns: 1 = closure parsed, 0 = not a closure
 */
static int cc_try_cc_closure(void) {
    /* Use saved position from before '(' was consumed, or current position for '@'/'[' */
    int start_line = (tok == '@' || tok == '[') ? (file ? file->line_num : 0) : tcc_state->cc_paren_start_line;
    int start_col = (tok == '@' || tok == '[') ? tok_col : tcc_state->cc_paren_start_col;
    
    /* Case 1: @unsafe [...] or [...] closure with capture list */
    if (tok == '@' || tok == '[') {
        if (tok == '@') {
            next(); /* consume '@' */
            const char* kw = get_tok_str(tok, NULL);
            if (!kw || strcmp(kw, "unsafe") != 0) {
                tcc_error("unexpected '@' in expression (expected '@unsafe')");
                return 0;
            }
            next(); /* consume 'unsafe' */
            if (tok != '[') {
                tcc_error("expected '[' after '@unsafe' in closure");
                return 0;
            }
        }
        
        /* Parse capture list */
        next(); /* consume '[' */
        int sq = 1;
        while (tok != TOK_EOF && sq > 0) {
            if (tok == '[') sq++;
            else if (tok == ']') sq--;
            if (sq > 0) next();
        }
        if (tok != ']') {
            tcc_error("unmatched '[' in closure capture list");
            return 0;
        }
        next(); /* consume ']' */
        
        if (tok != '(') {
            tcc_error("expected '(' after capture list in closure");
            return 0;
        }
        next(); /* consume '(' */
        
        /* Skip params until ')' */
        int par = 1;
        while (tok != TOK_EOF && par > 0) {
            if (tok == '(') par++;
            else if (tok == ')') par--;
            if (par > 0) next();
        }
        if (tok != ')') {
            tcc_error("unmatched '(' in closure parameter list");
            return 0;
        }
        next(); /* consume ')' */
        
        if (tok != TOK_CC_ARROW) {
            tcc_error("expected '=>' after closure parameters");
            return 0;
        }
        
        cc_ast_record_start(CC_AST_NODE_CLOSURE);
        if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
            int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
            tcc_state->cc_nodes[idx].line_start = start_line;
            tcc_state->cc_nodes[idx].col_start = start_col;
            tcc_state->cc_nodes[idx].aux1 = 0;
            tcc_state->cc_nodes[idx].aux_s1 = tcc_strdup("closure");
        }
        next(); /* consume '=>' */
        
        cc_parse_closure_body();
        cc_ast_record_end();
        vpushi(0);
        return 1;
    }
    
    /* Case 2: () => body (empty params, tok is ')' after '(' was consumed) */
    if (tok == ')') {
        next(); /* consume ')' */
        if (tok == TOK_CC_ARROW) {
            cc_ast_record_start(CC_AST_NODE_CLOSURE);
            if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
                int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
                tcc_state->cc_nodes[idx].line_start = start_line;
                tcc_state->cc_nodes[idx].col_start = start_col;
                tcc_state->cc_nodes[idx].aux1 = 0;
                tcc_state->cc_nodes[idx].aux_s1 = tcc_strdup("closure");
            }
            next(); /* consume '=>' */
            cc_parse_closure_body();
            cc_ast_record_end();
            vpushi(0);
            return 1;
        }
        /* Not a closure - but () is invalid in C */
        tcc_error("expected '=>' after '()' for closure");
        return 0;
    }
    
    /* Case 3: (x) => body, (x, y) => body, (int x) => body */
    if (tok >= TOK_UIDENT || tok == TOK_INT || tok == TOK_CHAR || tok == TOK_SHORT || tok == TOK_LONG || tok == TOK_VOID) {
        int cc_consumed[64];
        int cc_consumed_n = 0;
        int cc_param_tok[2] = {0, 0};
        char* cc_param_ty[2] = {0, 0};
        int cc_param_line[2] = {0, 0};
        int cc_param_col[2] = {0, 0};
        int cc_param_n = 0;
        int cc_is_arrow = 0;

#define CC_CONSUME_TOK() do { if (cc_consumed_n < 64) cc_consumed[cc_consumed_n++] = tok; next(); } while (0)

        /* Parse up to 2 params */
        for (int pi = 0; pi < 2; pi++) {
            if (tok == TOK_INT || tok == TOK_CHAR || tok == TOK_SHORT || tok == TOK_LONG || tok == TOK_VOID) {
                char tybuf[64];
                tybuf[0] = 0;
                const char* tnm = get_tok_str(tok, NULL);
                if (tnm) strncpy(tybuf, tnm, sizeof(tybuf) - 1);
                tybuf[sizeof(tybuf) - 1] = 0;
                CC_CONSUME_TOK();
                while (tok == '*') {
                    size_t L = strlen(tybuf);
                    if (L + 1 < sizeof(tybuf)) { tybuf[L] = '*'; tybuf[L + 1] = 0; }
                    CC_CONSUME_TOK();
                }
                if (tok < TOK_UIDENT) break;
                cc_param_tok[pi] = tok;
                cc_param_line[pi] = file ? file->line_num : 0;
                cc_param_col[pi] = tok_col;
                cc_param_ty[pi] = tcc_strdup(tybuf);
                cc_param_n = pi + 1;
                CC_CONSUME_TOK();
            } else if (tok >= TOK_UIDENT) {
                cc_param_tok[pi] = tok;
                cc_param_line[pi] = file ? file->line_num : 0;
                cc_param_col[pi] = tok_col;
                cc_param_ty[pi] = NULL;
                cc_param_n = pi + 1;
                CC_CONSUME_TOK();
            } else {
                break;
            }

            if (pi == 0 && tok == ',') {
                CC_CONSUME_TOK();
                continue;
            }
            break;
        }

        if (tok == ')') {
            CC_CONSUME_TOK();
            if (tok == TOK_CC_ARROW) {
                cc_is_arrow = 1;
                CC_CONSUME_TOK();
            } else if (tok == '=') {
                CC_CONSUME_TOK();
                if (tok == '>') {
                    cc_is_arrow = 1;
                    CC_CONSUME_TOK();
                }
            }
        }

        if (cc_is_arrow && cc_param_n > 0) {
            cc_ast_record_start(CC_AST_NODE_CLOSURE);
            if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
                int cidx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
                tcc_state->cc_nodes[cidx].line_start = start_line;
                tcc_state->cc_nodes[cidx].col_start = start_col;
                tcc_state->cc_nodes[cidx].aux1 = cc_param_n;
                tcc_state->cc_nodes[cidx].aux_s1 = tcc_strdup("closure");
            }

            for (int pi = 0; pi < cc_param_n; pi++) {
                cc_ast_record_start(CC_AST_NODE_PARAM);
                if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
                    int pidx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
                    const char *pname = get_tok_str(cc_param_tok[pi], NULL);
                    tcc_state->cc_nodes[pidx].aux1 = cc_param_tok[pi];
                    tcc_state->cc_nodes[pidx].aux_s1 = pname ? tcc_strdup(pname) : NULL;
                    tcc_state->cc_nodes[pidx].aux_s2 = cc_param_ty[pi];
                    tcc_state->cc_nodes[pidx].line_start = cc_param_line[pi];
                    tcc_state->cc_nodes[pidx].col_start = cc_param_col[pi];
                    if (pname) tcc_state->cc_nodes[pidx].col_end = cc_param_col[pi] + (int)strlen(pname);
                }
                cc_ast_record_end();
            }

            cc_parse_closure_body();
            cc_ast_record_end();
            vpushi(0);
            return 1;
        }

        /* Not a closure: restore tokens */
        for (int pi = 0; pi < 2; pi++) if (cc_param_ty[pi]) tcc_free(cc_param_ty[pi]);
        for (int i = cc_consumed_n - 1; i >= 0; i--) unget_tok(cc_consumed[i]);

#undef CC_CONSUME_TOK
    }
    
    return 0;
}

/*
 * Parse single-parameter closure `x => expr` or `x => { block }`
 * Called after identifier has been consumed and next() called.
 * ident_tok is the identifier token, start_line/col are its position.
 * Returns: 1 = closure parsed, 0 = not a closure
 */
static int cc_try_cc_closure_single_param(int ident_tok, int start_line, int start_col) {
    int cc_is_arrow = 0;
    
    if (tok == TOK_CC_ARROW) {
        cc_is_arrow = 1;
    } else if (tok == '=') {
        next();
        if (tok == '>') {
            cc_is_arrow = 1;
        } else {
            unget_tok('=');
            return 0;
        }
    }
    
    if (!cc_is_arrow)
        return 0;
    
    cc_ast_record_start(CC_AST_NODE_CLOSURE);
    if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
        int cidx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
        tcc_state->cc_nodes[cidx].line_start = start_line;
        tcc_state->cc_nodes[cidx].col_start = start_col;
        tcc_state->cc_nodes[cidx].aux1 = 1;
        tcc_state->cc_nodes[cidx].aux_s1 = tcc_strdup("closure");
    }

    cc_ast_record_start(CC_AST_NODE_PARAM);
    if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
        int pidx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
        const char *pname = get_tok_str(ident_tok, NULL);
        tcc_state->cc_nodes[pidx].aux1 = ident_tok;
        tcc_state->cc_nodes[pidx].aux_s1 = pname ? tcc_strdup(pname) : NULL;
        tcc_state->cc_nodes[pidx].line_start = start_line;
        tcc_state->cc_nodes[pidx].col_start = start_col;
        if (pname) tcc_state->cc_nodes[pidx].col_end = start_col + (int)strlen(pname);
    }
    cc_ast_record_end();

    /* consume => */
    if (tok == TOK_CC_ARROW)
        next();
    else
        next(); /* tok is '>' */
    
    cc_parse_closure_body();
    cc_ast_record_end();
    vpushi(0);
    return 1;
}

/*
 * Parse spawn statement: spawn(expr, ...);
 * Returns: 1 = handled, 0 = not handled
 */
static int cc_try_cc_spawn(void) {
    if (tok < TOK_IDENT || strcmp(get_tok_str(tok, NULL), "spawn") != 0)
        return 0;
    
    cc_ast_record_start(CC_AST_NODE_STMT);
    if (tcc_state && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
        int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
        tcc_state->cc_nodes[idx].aux_s1 = tcc_strdup("spawn");
    }
    
    next(); /* consume 'spawn' */
    if (tok != '(') {
        tcc_error("expected '(' after spawn");
        return 0;
    }
    next(); /* consume '(' */
    
    /* Parse-only: do not emit code for CC spawn statement */
    {
        int saved_ncw = nocode_wanted;
        ++nocode_wanted;
        /* Parse comma-separated argument list */
        expr_eq();
        vpop();
        while (tok == ',') {
            next(); /* consume ',' */
            expr_eq();
            vpop();
        }
        nocode_wanted = saved_ncw;
    }
    
    skip(')');
    
    /* Record end position at ';' before consuming it */
    if (tcc_state && tok == ';' && file && tcc_state->cc_nodes && tcc_state->cc_node_stack_top >= 0) {
        int idx = tcc_state->cc_node_stack[tcc_state->cc_node_stack_top];
        tcc_state->cc_nodes[idx].line_end = file->line_num;
        tcc_state->cc_nodes[idx].col_end = tok_col + 1;
        tcc_state->cc_nodes[idx].aux2 |= (int)(1U << 31); /* pin end span */
    }
    
    skip(';');
    cc_ast_record_end();
    return 1;
}

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