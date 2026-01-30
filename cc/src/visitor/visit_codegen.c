#include "visitor.h"
#include "visit_codegen.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>

#include "visitor/ufcs.h"
#include "visitor/pass_strip_markers.h"
#include "visitor/pass_await_normalize.h"
#include "visitor/pass_ufcs.h"
#include "visitor/pass_closure_calls.h"
#include "visitor/pass_autoblock.h"
#include "visitor/pass_arena_ast.h"
#include "visitor/pass_nursery_spawn_ast.h"
#include "visitor/pass_closure_literal_ast.h"
#include "visitor/pass_defer_syntax.h"
#include "visitor/pass_channel_syntax.h"
#include "visitor/pass_type_syntax.h"
#include "visitor/pass_match_syntax.h"
#include "visitor/pass_with_deadline_syntax.h"
#include "visitor/edit_buffer.h"
#include "visitor/visitor_fileutil.h"
#include "visitor/text_span.h"
#include "parser/tcc_bridge.h"
#include "preprocess/preprocess.h"
#include "preprocess/type_registry.h"
#include "util/path.h"
#include "util/text.h"

#ifndef CC_TCC_EXT_AVAILABLE
#error "CC_TCC_EXT_AVAILABLE is required (patched TCC stub-AST required)."
#endif

/* Local aliases for the shared helpers */
#define cc__sb_append_local cc_sb_append
#define cc__sb_append_cstr_local cc_sb_append_cstr
#define cc__is_ident_char_local2 cc_is_ident_char
#define cc__is_ident_start_local2 cc_is_ident_start
#define cc__skip_ws_local2 cc_skip_ws

#define cc__is_ident_char_local cc_is_ident_char

/* Helper: reparse source string to AST (file-based for correctness with complex prelude) */
static CCASTRoot* cc__reparse_source_to_ast(const char* src, size_t src_len,
                                            const char* input_path, CCSymbolTable* symbols) {
    /* Use file-based path for reparse - the prelude and path handling is complex */
    char* tmp_path = cc__write_temp_c_file(src, src_len, input_path);
    if (!tmp_path) return NULL;

    char pp_path[128];
    int pp_err = cc_preprocess_file(tmp_path, pp_path, sizeof(pp_path));
    const char* use_path = (pp_err == 0) ? pp_path : tmp_path;

    CCASTRoot* root = cc_tcc_bridge_parse_to_ast(use_path, input_path, symbols);
    if (pp_err == 0 && !getenv("CC_KEEP_REPARSE")) unlink(pp_path);
    if (!getenv("CC_KEEP_REPARSE")) unlink(tmp_path);
    free(tmp_path);

    if (root && pp_err == 0) {
        root->lowered_is_temp = 0; /* Already cleaned up */
    }
    return root;
}

/* AST-driven async lowering (implemented in `cc/src/visitor/async_ast.c`). */
int cc_async_rewrite_state_machine_ast(const CCASTRoot* root,
                                       const CCVisitorCtx* ctx,
                                       const char* in_src,
                                       size_t in_len,
                                       char** out_src,
                                       size_t* out_len);

/* Legacy closure scan/lowering helpers removed - now handled by AST-span passes. */

/* Strip CC decl markers so output is valid C. This is used regardless of whether
   TCC extensions are available, because the output C is compiled by the host compiler. */
/* cc__read_entire_file / cc__write_temp_c_file are implemented in visitor_fileutil.c */

/* UFCS span rewrite lives in pass_ufcs.c now (cc__rewrite_ufcs_spans_with_nodes). */

/* Helper to append to a string buffer */
static void cc__cg_sb_append(char** out, size_t* out_len, size_t* out_cap, const char* s, size_t len) {
    if (!s || len == 0) return;
    while (*out_len + len + 1 > *out_cap) {
        size_t new_cap = (*out_cap == 0) ? 256 : (*out_cap * 2);
        char* new_out = (char*)realloc(*out, new_cap);
        if (!new_out) return;
        *out = new_out;
        *out_cap = new_cap;
    }
    memcpy(*out + *out_len, s, len);
    *out_len += len;
    (*out)[*out_len] = 0;
}

static void cc__cg_sb_append_cstr(char** out, size_t* out_len, size_t* out_cap, const char* s) {
    if (s) cc__cg_sb_append(out, out_len, out_cap, s, strlen(s));
}

/* Rewrite @closing(ch) spawn(...) or @closing(ch) { ... } syntax.
   Transforms into spawned sub-nursery for flat visual structure. */
static char* cc__rewrite_closing_annotation(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    size_t i = 0;
    size_t last_emit = 0;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Look for @closing( */
        if (c == '@' && i + 8 < n && memcmp(src + i, "@closing", 8) == 0) {
            size_t start = i;
            size_t j = i + 8;
            
            /* Skip whitespace */
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
            
            if (j < n && src[j] == '(') {
                /* Found @closing( - parse the channel list */
                size_t paren_start = j;
                j++; /* skip '(' */
                int paren_depth = 1;
                while (j < n && paren_depth > 0) {
                    if (src[j] == '(') paren_depth++;
                    else if (src[j] == ')') paren_depth--;
                    j++;
                }
                if (paren_depth != 0) { i++; continue; }
                
                size_t paren_end = j - 1;
                size_t chans_start = paren_start + 1;
                size_t chans_end = paren_end;
                
                /* Skip whitespace after ) */
                while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
                
                if (j >= n) { i++; continue; }
                
                size_t body_start = j;
                size_t body_end;
                int is_block = 0;
                
                if (src[j] == '{') {
                    is_block = 1;
                    int brace_depth = 1;
                    j++;
                    while (j < n && brace_depth > 0) {
                        if (src[j] == '{') brace_depth++;
                        else if (src[j] == '}') brace_depth--;
                        else if (src[j] == '"') {
                            j++;
                            while (j < n && src[j] != '"') {
                                if (src[j] == '\\' && j + 1 < n) j++;
                                j++;
                            }
                        } else if (src[j] == '\'') {
                            j++;
                            while (j < n && src[j] != '\'') {
                                if (src[j] == '\\' && j + 1 < n) j++;
                                j++;
                            }
                        }
                        j++;
                    }
                    body_end = j;
                } else if (j + 5 < n && memcmp(src + j, "spawn", 5) == 0) {
                    is_block = 0;
                    int paren_d = 0, brace_d = 0;
                    while (j < n) {
                        if (src[j] == '(') paren_d++;
                        else if (src[j] == ')') paren_d--;
                        else if (src[j] == '{') brace_d++;
                        else if (src[j] == '}') { brace_d--; if (brace_d < 0) break; }
                        else if (src[j] == ';' && paren_d == 0 && brace_d == 0) { j++; break; }
                        else if (src[j] == '"') {
                            j++;
                            while (j < n && src[j] != '"') {
                                if (src[j] == '\\' && j + 1 < n) j++;
                                j++;
                            }
                        }
                        j++;
                    }
                    body_end = j;
                } else {
                    i++;
                    continue;
                }
                
                cc__cg_sb_append(&out, &out_len, &out_cap, src + last_emit, start - last_emit);
                cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "spawn(() => { @nursery closing(");
                cc__cg_sb_append(&out, &out_len, &out_cap, src + chans_start, chans_end - chans_start);
                cc__cg_sb_append_cstr(&out, &out_len, &out_cap, ") ");
                
                if (is_block) {
                    cc__cg_sb_append(&out, &out_len, &out_cap, src + body_start, body_end - body_start);
                } else {
                    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "{ ");
                    cc__cg_sb_append(&out, &out_len, &out_cap, src + body_start, body_end - body_start);
                    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, " }");
                }
                
                cc__cg_sb_append_cstr(&out, &out_len, &out_cap, " });");
                
                last_emit = body_end;
                i = body_end;
                continue;
            }
        }
        
        i++;
    }
    
    if (last_emit == 0) return NULL;
    if (last_emit < n) cc__cg_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Rewrite `if @try (T x = expr) { ... } else { ... }` into expanded form:
   { __typeof__(expr) __cc_try_bind = (expr);
     if (__cc_try_bind.ok) { T x = __cc_try_bind.u.value; ... }
     else { ... } }
*/
static char* cc__rewrite_if_try_syntax(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0, last_emit = 0;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    
    while (i < n) {
        char c = src[i], c2 = (i+1 < n) ? src[i+1] : 0;
        /* Skip comments and strings */
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i+1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i+1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Look for `if @try (` */
        if (c == 'i' && c2 == 'f') {
            int ws = (i == 0) || !cc_is_ident_char(src[i-1]);
            int we = (i+2 >= n) || !cc_is_ident_char(src[i+2]);
            if (ws && we) {
                size_t if_start = i, j = i + 2;
                /* Skip whitespace after 'if' */
                while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
                /* Check for '@try' */
                if (j+4 <= n && src[j] == '@' && src[j+1] == 't' && src[j+2] == 'r' && src[j+3] == 'y' &&
                    (j+4 >= n || !cc_is_ident_char(src[j+4]))) {
                    size_t after_try = j + 4;
                    while (after_try < n && (src[after_try] == ' ' || src[after_try] == '\t' || src[after_try] == '\n')) after_try++;
                    /* Expect '(' */
                    if (after_try < n && src[after_try] == '(') {
                        size_t cond_start = after_try + 1;
                        /* Find matching ')' */
                        size_t cond_end = cond_start;
                        int paren = 1, in_s = 0, in_c = 0;
                        while (cond_end < n && paren > 0) {
                            char ec = src[cond_end];
                            if (in_s) { if (ec == '\\' && cond_end+1 < n) cond_end++; else if (ec == '"') in_s = 0; cond_end++; continue; }
                            if (in_c) { if (ec == '\\' && cond_end+1 < n) cond_end++; else if (ec == '\'') in_c = 0; cond_end++; continue; }
                            if (ec == '"') { in_s = 1; cond_end++; continue; }
                            if (ec == '\'') { in_c = 1; cond_end++; continue; }
                            if (ec == '(') paren++;
                            else if (ec == ')') { paren--; if (paren == 0) break; }
                            cond_end++;
                        }
                        if (paren != 0) { i++; continue; }
                        
                        /* Parse T x = expr from cond_start to cond_end */
                        size_t eq = cond_start;
                        while (eq < cond_end && src[eq] != '=') eq++;
                        if (eq >= cond_end) { i++; continue; }
                        
                        /* Type and var before '=' */
                        size_t tv_end = eq;
                        while (tv_end > cond_start && (src[tv_end-1] == ' ' || src[tv_end-1] == '\t')) tv_end--;
                        size_t var_end = tv_end, var_start = var_end;
                        while (var_start > cond_start && cc_is_ident_char(src[var_start-1])) var_start--;
                        if (var_start >= var_end) { i++; continue; }
                        
                        size_t type_end = var_start;
                        while (type_end > cond_start && (src[type_end-1] == ' ' || src[type_end-1] == '\t')) type_end--;
                        size_t type_start = cond_start;
                        while (type_start < type_end && (src[type_start] == ' ' || src[type_start] == '\t')) type_start++;
                        if (type_start >= type_end) { i++; continue; }
                        
                        /* Expr after '=' */
                        size_t expr_start = eq + 1;
                        while (expr_start < cond_end && (src[expr_start] == ' ' || src[expr_start] == '\t')) expr_start++;
                        size_t expr_end = cond_end;
                        while (expr_end > expr_start && (src[expr_end-1] == ' ' || src[expr_end-1] == '\t')) expr_end--;
                        if (expr_start >= expr_end) { i++; continue; }
                        
                        /* Find then-block */
                        size_t k = cond_end + 1; /* skip ')' */
                        while (k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\n')) k++;
                        if (k >= n || src[k] != '{') { i++; continue; }
                        
                        size_t then_start = k;
                        int brace = 1; k++; in_s = 0; in_c = 0;
                        while (k < n && brace > 0) {
                            char ec = src[k];
                            if (in_s) { if (ec == '\\' && k+1 < n) k++; else if (ec == '"') in_s = 0; k++; continue; }
                            if (in_c) { if (ec == '\\' && k+1 < n) k++; else if (ec == '\'') in_c = 0; k++; continue; }
                            if (ec == '"') { in_s = 1; k++; continue; }
                            if (ec == '\'') { in_c = 1; k++; continue; }
                            if (ec == '{') brace++; else if (ec == '}') brace--;
                            k++;
                        }
                        size_t then_end = k;
                        
                        /* Check for else */
                        size_t else_start = 0, else_end = 0, m = k;
                        while (m < n && (src[m] == ' ' || src[m] == '\t' || src[m] == '\n')) m++;
                        if (m+4 <= n && src[m] == 'e' && src[m+1] == 'l' && src[m+2] == 's' && src[m+3] == 'e' &&
                            (m+4 >= n || !cc_is_ident_char(src[m+4]))) {
                            m += 4;
                            while (m < n && (src[m] == ' ' || src[m] == '\t' || src[m] == '\n')) m++;
                            if (m < n && src[m] == '{') {
                                else_start = m; brace = 1; m++; in_s = 0; in_c = 0;
                                while (m < n && brace > 0) {
                                    char ec = src[m];
                                    if (in_s) { if (ec == '\\' && m+1 < n) m++; else if (ec == '"') in_s = 0; m++; continue; }
                                    if (in_c) { if (ec == '\\' && m+1 < n) m++; else if (ec == '\'') in_c = 0; m++; continue; }
                                    if (ec == '"') { in_s = 1; m++; continue; }
                                    if (ec == '\'') { in_c = 1; m++; continue; }
                                    if (ec == '{') brace++; else if (ec == '}') brace--;
                                    m++;
                                }
                                else_end = m;
                            }
                        }
                        
                        /* Emit expansion */
                        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, if_start - last_emit);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "{ __typeof__(");
                        cc_sb_append(&out, &out_len, &out_cap, src + expr_start, expr_end - expr_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, ") __cc_try_bind = (");
                        cc_sb_append(&out, &out_len, &out_cap, src + expr_start, expr_end - expr_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "); if (__cc_try_bind.ok) { ");
                        cc_sb_append(&out, &out_len, &out_cap, src + type_start, type_end - type_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, " ");
                        cc_sb_append(&out, &out_len, &out_cap, src + var_start, var_end - var_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, " = __cc_try_bind.u.value; ");
                        cc_sb_append(&out, &out_len, &out_cap, src + then_start + 1, then_end - then_start - 2);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, " }");
                        if (else_end > else_start) {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, " else ");
                            cc_sb_append(&out, &out_len, &out_cap, src + else_start, else_end - else_start);
                        }
                        cc_sb_append_cstr(&out, &out_len, &out_cap, " }");
                        
                        last_emit = (else_end > 0) ? else_end : then_end;
                        i = last_emit;
                        continue;
                    }
                }
            }
        }
        i++;
    }
    if (last_emit == 0) return NULL;
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

int cc_visit_codegen(const CCASTRoot* root, CCVisitorCtx* ctx, const char* output_path) {
    if (!ctx || !ctx->symbols || !output_path) return EINVAL;
    const char* src_path = ctx->input_path ? ctx->input_path : "<cc_input>";
    FILE* out = fopen(output_path, "w");
    if (!out) return errno ? errno : -1;

    /* Optional: dump TCC stub nodes for debugging wiring. */
    if (root && root->nodes && root->node_count > 0) {
        const char* dump = getenv("CC_DUMP_TCC_STUB_AST");
        if (dump && dump[0] == '1') {
            typedef struct {
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
            } CCASTStubNodeView;
            const CCASTStubNodeView* n = (const CCASTStubNodeView*)root->nodes;
            fprintf(stderr, "[cc] stub ast nodes: %d\n", root->node_count);
            int max_dump = root->node_count;
            if (max_dump > 4000) max_dump = 4000;
            for (int i = 0; i < max_dump; i++) {
                fprintf(stderr,
                        "  [%d] kind=%d parent=%d file=%s lines=%d..%d cols=%d..%d aux1=%d aux2=%d aux_s1=%s aux_s2=%s\n",
                        i,
                        n[i].kind,
                        n[i].parent,
                        n[i].file ? n[i].file : "<null>",
                        n[i].line_start,
                        n[i].line_end,
                        n[i].col_start,
                        n[i].col_end,
                        n[i].aux1,
                        n[i].aux2,
                        n[i].aux_s1 ? n[i].aux_s1 : "<null>",
                        n[i].aux_s2 ? n[i].aux_s2 : "<null>");
            }
            if (max_dump != root->node_count)
                fprintf(stderr, "  ... truncated (%d total)\n", root->node_count);
        }
    }

    /* For final codegen we read the original source and lower UFCS/@arena here.
       The preprocessor's temp file exists only to make TCC parsing succeed.
       Note: text-based rewrites like `if @try` run on original source early in this function. */
    /* Read original source once; we may rewrite UFCS spans before @arena lowering. */
    char* src_all = NULL;
    size_t src_len = 0;
    if (ctx->input_path) {
        cc__read_entire_file(ctx->input_path, &src_all, &src_len);
    }

    char* src_ufcs = src_all;
    size_t src_ufcs_len = src_len;

    /* Rewrite @closing(ch) spawn/{ } -> spawned sub-nursery for flat channel closing */
    if (src_ufcs && src_ufcs_len) {
        char* rewritten = cc__rewrite_closing_annotation(src_ufcs, src_ufcs_len);
        if (rewritten) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = strlen(rewritten);
        }
    }

    /* Rewrite `if @try (T x = expr) { ... }` into expanded form */
    if (src_ufcs && src_ufcs_len) {
        char* rewritten = cc__rewrite_if_try_syntax(src_ufcs, src_ufcs_len);
        if (rewritten) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = strlen(rewritten);
        }
    }

    /* Rewrite generic container syntax: Vec<T> -> Vec_T, vec_new<T>() -> Vec_T_init() */
    if (src_ufcs && src_ufcs_len) {
        char* rewritten = cc_rewrite_generic_containers(src_ufcs, src_ufcs_len, ctx->input_path);
        if (rewritten) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = strlen(rewritten);
        }
    }

    /* Rewrite UFCS method calls on containers: v.push(x) -> Vec_int_push(&v, x) */
    if (src_ufcs && src_ufcs_len) {
        char* rewritten = cc_rewrite_ufcs_container_calls(src_ufcs, src_ufcs_len, ctx->input_path);
        if (rewritten) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = strlen(rewritten);
        }
    }

    /* Rewrite std_out.write()/std_err.write() UFCS patterns */
    if (src_ufcs && src_ufcs_len) {
        char* rewritten = cc_rewrite_std_io_ufcs(src_ufcs, src_ufcs_len);
        if (rewritten) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = strlen(rewritten);
        }
    }

    /* Rewrite `with_deadline(expr) { ... }` (not valid C) into CCDeadline scope syntax
       using @defer, so the rest of the pipeline sees valid parseable text. */
    if (src_ufcs && src_ufcs_len) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_with_deadline_syntax(src_ufcs, src_ufcs_len, &rewritten, &rewritten_len) == 0 && rewritten) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }

    /* Rewrite `@match { ... }` into valid C before any node-based rewrites. */
    if (src_ufcs && src_ufcs_len) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        int r = cc__rewrite_match_syntax(ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
        if (r < 0) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            return EINVAL;
        }
        if (r > 0 && rewritten) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }

    /* Produced by the closure-literal AST pass (emitted into the output TU). */
    char* closure_protos = NULL;
    size_t closure_protos_len = 0;
    char* closure_defs = NULL;
    size_t closure_defs_len = 0;

#ifdef CC_TCC_EXT_AVAILABLE
    if (src_ufcs && root && root->nodes && root->node_count > 0) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_ufcs_spans_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len)) {
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }
#endif

    /* Rewrite closure calls anywhere (including nested + multiline) using stub CALL nodes. */
#ifdef CC_TCC_EXT_AVAILABLE
    char* src_calls = NULL;
    size_t src_calls_len = 0;
    if (src_ufcs && root && root->nodes && root->node_count > 0) {
        if (cc__rewrite_all_closure_calls_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &src_calls, &src_calls_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = src_calls;
            src_ufcs_len = src_calls_len;
        }
    }
#endif

    /* Auto-blocking (first cut): inside @async functions, wrap statement-form calls to known
       non-@async/non-@noblock functions in cc_run_blocking_closure0(() => { ... }). */
#ifdef CC_TCC_EXT_AVAILABLE
    if (src_ufcs && root && root->nodes && root->node_count > 0 && ctx->symbols) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        int r = cc__rewrite_autoblocking_calls_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
        if (r < 0) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }
        if (r > 0) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }
#endif

    /* Normalize `await <expr>` used inside larger expressions into temp hoists so the
       text-based async state machine can lower it (AST-driven span rewrite). */
#ifdef CC_TCC_EXT_AVAILABLE
    if (src_ufcs && root && root->nodes && root->node_count > 0) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_await_exprs_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
        if (getenv("CC_DEBUG_AWAIT_REWRITE") && src_ufcs) {
            const char* needle = "@async int f";
            const char* p = strstr(src_ufcs, needle);
            if (!p) p = strstr(src_ufcs, "@async");
            if (p) {
                fprintf(stderr, "CC_DEBUG_AWAIT_REWRITE: ---- snippet ----\n");
                size_t off = (size_t)(p - src_ufcs);
                size_t take = 800;
                if (off + take > src_ufcs_len) take = src_ufcs_len - off;
                fwrite(p, 1, take, stderr);
                fprintf(stderr, "\nCC_DEBUG_AWAIT_REWRITE: ---- end ----\n");
            }
        }
    }
#endif

    /* Reparse the current TU source to get an up-to-date stub-AST for statement-level lowering
       (@arena/@nursery/spawn). These rewrites run before marker stripping to keep spans stable. */
    if (src_ufcs && ctx && ctx->symbols) {
        CCASTRoot* root3 = cc__reparse_source_to_ast(src_ufcs, src_ufcs_len, ctx->input_path, ctx->symbols);
        if (!root3) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            return EINVAL;
        }

        /* Lower `channel_pair(&tx, &rx);` BEFORE channel type rewrite (it needs `[~]` patterns). */
        {
            size_t rp_len = 0;
            char* rp = cc__rewrite_channel_pair_calls_text(ctx, src_ufcs, src_ufcs_len, &rp_len);
            if (!rp) {
                cc_tcc_bridge_free_ast(root3);
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(src_all);
                return EINVAL;
            }
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rp;
            src_ufcs_len = rp_len;
        }

        /* Rewrite channel handle types BEFORE closure pass so captured CCChanTx/CCChanRx variables
           are correctly recognized. This rewrites `int[~4 >]` -> `CCChanTx`, etc. */
        {
            char* rew = cc__rewrite_chan_handle_types_text(ctx, src_ufcs, src_ufcs_len);
            if (rew) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew;
                src_ufcs_len = strlen(src_ufcs);
            }
        }

        /* 1) closure literals -> __cc_closure_make_N(...) + generated closure defs */
        {
            char* rewritten = NULL;
            size_t rewritten_len = 0;
            char* protos = NULL;
            size_t protos_len = 0;
            char* defs = NULL;
            size_t defs_len = 0;
            int r = cc__rewrite_closure_literals_with_nodes(root3, ctx, src_ufcs, src_ufcs_len,
                                                           &rewritten, &rewritten_len,
                                                           &protos, &protos_len,
                                                           &defs, &defs_len);
            if (r < 0) {
                cc_tcc_bridge_free_ast(root3);
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(src_all);
                return EINVAL;
            }
            if (r > 0) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = rewritten_len;
            } else {
                free(rewritten);
            }
            if (protos) { free(closure_protos); closure_protos = protos; closure_protos_len = protos_len; }
            if (defs) { free(closure_defs); closure_defs = defs; closure_defs_len = defs_len; }
        }
        cc_tcc_bridge_free_ast(root3);

        /* Reparse after closure rewrite so spawn/nursery/arena spans are correct. */
        CCASTRoot* root4 = cc__reparse_source_to_ast(src_ufcs, src_ufcs_len, ctx->input_path, ctx->symbols);
        if (!root4) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }

        /* 2) spawn(...) -> cc_nursery_spawn* (hard error if outside nursery). */
        {
            char* rewritten = NULL;
            size_t rewritten_len = 0;
            int r = cc__rewrite_spawn_stmts_with_nodes(root4, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
            if (r < 0) {
                cc_tcc_bridge_free_ast(root4);
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(src_all);
                free(closure_protos);
                free(closure_defs);
                return EINVAL;
            }
            if (r > 0) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = rewritten_len;
            }
        }
        cc_tcc_bridge_free_ast(root4);

        /* Reparse after spawn rewrite so nursery/arena end braces are correct. */
        CCASTRoot* root5 = cc__reparse_source_to_ast(src_ufcs, src_ufcs_len, ctx->input_path, ctx->symbols);
        if (!root5) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }

        /* 3+4) Batch nursery + arena using EditBuffer so we don't have stale AST offsets.
           Both passes use root5; without batching, nursery changes src_ufcs but arena
           still references root5's offsets (which are now wrong). */
        {
            CCEditBuffer eb;
            cc_edit_buffer_init(&eb, src_ufcs, src_ufcs_len);

            int n_nursery = cc__collect_nursery_edits(root5, ctx, &eb);
            int n_arena = cc__collect_arena_edits(root5, ctx, &eb);

            if (n_nursery < 0 || n_arena < 0) {
                cc_tcc_bridge_free_ast(root5);
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(src_all);
                free(closure_protos);
                free(closure_defs);
                return EINVAL;
            }

            if (eb.count > 0) {
                size_t new_len = 0;
                char* applied = cc_edit_buffer_apply(&eb, &new_len);
                if (applied) {
                    if (src_ufcs != src_all) free(src_ufcs);
                    src_ufcs = applied;
                    src_ufcs_len = new_len;
                }
            }
        }
        cc_tcc_bridge_free_ast(root5);
    }

    /* Lower @defer (and hard-error on cancel) using a syntax-driven pass.
       IMPORTANT: this must run BEFORE async lowering so `@defer` can be made suspend-safe. */
    if (src_ufcs) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        int r = cc__rewrite_defer_syntax(ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
        if (r < 0) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }
        if (r > 0) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }

    /* AST-driven @async lowering (state machine).
       IMPORTANT: run AFTER CC statement-level lowering so @nursery/@arena/spawn/closures are real C. */
    if (src_ufcs && ctx && ctx->symbols) {
        CCASTRoot* root2 = cc__reparse_source_to_ast(src_ufcs, src_ufcs_len, ctx->input_path, ctx->symbols);
        if (getenv("CC_DEBUG_REPARSE")) {
            fprintf(stderr, "CC: reparse: stub ast node_count=%d\n", root2 ? root2->node_count : -1);
        }
        if (!root2) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }

        char* rewritten = NULL;
        size_t rewritten_len = 0;
        int ar = cc_async_rewrite_state_machine_ast(root2, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
        cc_tcc_bridge_free_ast(root2);
        if (ar < 0) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }
        if (ar > 0) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }

    /* Strip CC decl markers so output is valid C (run after async lowering so it can see `@async`). */
    if (src_ufcs) {
        char* stripped = NULL;
        size_t stripped_len = 0;
        if (cc__strip_cc_decl_markers(src_ufcs, src_ufcs_len, &stripped, &stripped_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = stripped;
            src_ufcs_len = stripped_len;
        }
    }

    /* NOTE: slice move/provenance checking is now handled by the stub-AST checker pass
       (`cc/src/visitor/checker.c`) before visitor lowering. */

    fprintf(out, "/* CC visitor: passthrough of lowered C (preprocess + TCC parse) */\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include <ccc/cc_nursery.cch>\n");
    fprintf(out, "#include <ccc/cc_closure.cch>\n");
    fprintf(out, "#include <ccc/cc_slice.cch>\n");
    fprintf(out, "#include <ccc/cc_runtime.cch>\n");
    fprintf(out, "#include <ccc/std/io.cch>\n");  /* CCFile for closure captures */
    fprintf(out, "#include <ccc/std/task_intptr.cch>\n");
    /* Helper alias: used for auto-blocking arg binding to avoid accidental hoisting of these temps. */
    fprintf(out, "typedef intptr_t CCAbIntptr;\n");
    
    /* TSan synchronization for closure captures using atomic fence.
     * This creates a release point after writing captures. The corresponding
     * acquire is in the closure trampoline before reading captures. */
    fprintf(out, "\n/* --- Closure capture synchronization --- */\n");
    fprintf(out, "#include <stdatomic.h>\n");
    fprintf(out, "#define CC_TSAN_RELEASE(addr) atomic_thread_fence(memory_order_release)\n");
    
    /* Spawn thunks are emitted later (after parsing source) as static fns in this TU. */
    fprintf(out, "\n");
    fprintf(out, "/* --- CC spawn lowering helpers (best-effort) --- */\n");
    fprintf(out, "typedef struct { void (*fn)(void); } __cc_spawn_void_arg;\n");
    fprintf(out, "static void* __cc_spawn_thunk_void(void* p) {\n");
    fprintf(out, "  __cc_spawn_void_arg* a = (__cc_spawn_void_arg*)p;\n");
    fprintf(out, "  if (a && a->fn) a->fn();\n");
    fprintf(out, "  free(a);\n");
    fprintf(out, "  return NULL;\n");
    fprintf(out, "}\n");
    fprintf(out, "typedef struct { void (*fn)(int); int arg; } __cc_spawn_int_arg;\n");
    fprintf(out, "static void* __cc_spawn_thunk_int(void* p) {\n");
    fprintf(out, "  __cc_spawn_int_arg* a = (__cc_spawn_int_arg*)p;\n");
    fprintf(out, "  if (a && a->fn) a->fn(a->arg);\n");
    fprintf(out, "  free(a);\n");
    fprintf(out, "  return NULL;\n");
    fprintf(out, "}\n");
    fprintf(out, "/* --- end spawn helpers --- */\n\n");

    /* Emit container type declarations from type registry (populated by generic rewriting) */
    {
        CCTypeRegistry* reg = cc_type_registry_get_global();
        if (reg) {
            size_t n_vec = cc_type_registry_vec_count(reg);
            size_t n_map = cc_type_registry_map_count(reg);
            
            if (n_vec > 0 || n_map > 0) {
                fprintf(out, "/* --- CC generic container declarations --- */\n");
                fprintf(out, "#include <ccc/std/vec.cch>\n");
                fprintf(out, "#include <ccc/std/map.cch>\n");
                /* Vec/Map declarations must be skipped in parser mode where they're
                   already typedef'd to generic placeholders in vec.cch/map.cch */
                fprintf(out, "#ifndef CC_PARSER_MODE\n");
                
                /* Emit Vec declarations */
                for (size_t i = 0; i < n_vec; i++) {
                    const CCTypeInstantiation* inst = cc_type_registry_get_vec(reg, i);
                    if (inst && inst->type1 && inst->mangled_name) {
                        /* Extract mangled element name from Vec_xxx */
                        const char* mangled_elem = inst->mangled_name + 4; /* Skip "Vec_" */
                        
                        /* Skip Vec_char - it's predeclared in string.cch */
                        if (strcmp(mangled_elem, "char") == 0) {
                            continue;
                        }
                        
                        /* Check if type is complex (pointer, struct) - needs FULL macro */
                        int is_complex = (strchr(inst->type1, '*') != NULL || 
                                          strncmp(inst->type1, "struct ", 7) == 0 ||
                                          strncmp(inst->type1, "union ", 6) == 0);
                        if (is_complex) {
                            int opt_predeclared = (strcmp(mangled_elem, "charptr") == 0 ||
                                                   strcmp(mangled_elem, "intptr") == 0 ||
                                                   strcmp(mangled_elem, "voidptr") == 0);
                            if (!opt_predeclared) {
                                fprintf(out, "CC_DECL_OPTIONAL(CCOptional_%s, %s)\n", mangled_elem, inst->type1);
                            }
                            fprintf(out, "CC_VEC_DECL_ARENA_FULL(%s, %s, CCOptional_%s)\n", 
                                    inst->type1, inst->mangled_name, mangled_elem);
                        } else {
                            fprintf(out, "CC_VEC_DECL_ARENA(%s, %s)\n", inst->type1, inst->mangled_name);
                        }
                    }
                }
                
                /* Emit Map declarations */
                for (size_t i = 0; i < n_map; i++) {
                    const CCTypeInstantiation* inst = cc_type_registry_get_map(reg, i);
                    if (inst && inst->type1 && inst->type2 && inst->mangled_name) {
                        /* Use default hash functions for known types */
                        const char* hash_fn = "cc_kh_hash_i32";
                        const char* eq_fn = "cc_kh_eq_i32";
                        if (strcmp(inst->type1, "uint64_t") == 0) {
                            hash_fn = "cc_kh_hash_u64";
                            eq_fn = "cc_kh_eq_u64";
                        }
                        fprintf(out, "CC_MAP_DECL_ARENA(%s, %s, %s, %s, %s)\n", 
                                inst->type1, inst->type2, inst->mangled_name, hash_fn, eq_fn);
                    }
                }
                
                fprintf(out, "#endif /* !CC_PARSER_MODE */\n");
                fprintf(out, "/* --- end container declarations --- */\n\n");
            }
        }
    }

    /* Captures are lowered via __cc_closure_make_N factories. */
    if (closure_protos && closure_protos_len > 0) {
        fputs("/* --- CC closure forward decls --- */\n", out);
        fwrite(closure_protos, 1, closure_protos_len, out);
        fputs("/* --- end closure forward decls --- */\n\n", out);
    }

    /* Preserve diagnostics mapping to the original input (repo-relative for readability). */
    {
        char rel[1024];
        fprintf(out, "#line 1 \"%s\"\n", cc_path_rel_to_repo(src_path, rel, sizeof(rel)));
    }

    if (src_ufcs) {
        /* Lower `channel_pair(&tx, &rx);` into `cc_chan_pair_create(...)` */
        {
            size_t rp_len = 0;
            char* rp = cc__rewrite_channel_pair_calls_text(ctx, src_ufcs, src_ufcs_len, &rp_len);
            if (!rp) {
                fclose(out);
                return EINVAL;
            }
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rp;
            src_ufcs_len = rp_len;
        }
        /* Final safety: ensure invalid surface syntax like `T[~ ... >]` does not reach the C compiler. */
        {
            char* rew_slice = cc__rewrite_slice_types_text(ctx, src_ufcs, src_ufcs_len);
            if (!rew_slice) {
                fclose(out);
                return EINVAL;
            }
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rew_slice;
            src_ufcs_len = strlen(src_ufcs);
        }
        {
            char* rew = cc__rewrite_chan_handle_types_text(ctx, src_ufcs, src_ufcs_len);
            if (!rew) {
            fclose(out);
                return EINVAL;
            }
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rew;
            src_ufcs_len = strlen(src_ufcs);
        }
        /* Rewrite T? -> CCOptional_T */
        {
            if (getenv("CC_DEBUG_OPTIONAL")) fprintf(stderr, "CC: DEBUG: calling cc__rewrite_optional_types_text, len=%zu\n", src_ufcs_len);
            char* rew_opt = cc__rewrite_optional_types_text(ctx, src_ufcs, src_ufcs_len);
            if (getenv("CC_DEBUG_OPTIONAL")) fprintf(stderr, "CC: DEBUG: rew_opt=%p\n", (void*)rew_opt);
            if (rew_opt) {
            if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_opt;
                src_ufcs_len = strlen(src_ufcs);
                if (getenv("CC_DEBUG_OPTIONAL")) fprintf(stderr, "CC: DEBUG: new len=%zu\n", src_ufcs_len);
            }
        }
        /* Rewrite T!>(E) -> CCResult_T_E and collect result type pairs */
        {
            char* rew_res = cc__rewrite_result_types_text(ctx, src_ufcs, src_ufcs_len);
            if (rew_res) {
            if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_res;
                src_ufcs_len = strlen(src_ufcs);
            }
            
            /* Insert result type declarations into source at the right position.
               Find first usage, then back up to before the enclosing function. */
            if (cc__cg_result_type_count > 0) {
                const char* insert_pos = NULL;
                /* Find the first usage of any CCResult_T_E type in the source */
                size_t earliest_pos = src_ufcs_len;
                for (size_t ri = 0; ri < cc__cg_result_type_count; ri++) {
                    CCCodegenResultTypePair* p = &cc__cg_result_types[ri];
                    char pattern[256];
                    snprintf(pattern, sizeof(pattern), "CCResult_%s_%s", p->mangled_ok, p->mangled_err);
                    const char* found = strstr(src_ufcs, pattern);
                    if (found && (size_t)(found - src_ufcs) < earliest_pos) {
                        earliest_pos = (size_t)(found - src_ufcs);
                    }
                }
                if (earliest_pos < src_ufcs_len) {
                    /* Back up to find the enclosing function definition.
                       Look for "int main(" or "type name(" at start of line. */
                    size_t pos = earliest_pos;
                    while (pos > 0) {
                        /* Find start of current line */
                        size_t line_start = pos;
                        while (line_start > 0 && src_ufcs[line_start - 1] != '\n') line_start--;
                        
                        /* Check if this line looks like a function definition */
                        const char* line = src_ufcs + line_start;
                        /* Skip leading whitespace */
                        while (*line == ' ' || *line == '\t') line++;
                        
                        /* Check for common function patterns */
                        if ((strncmp(line, "int ", 4) == 0 || strncmp(line, "void ", 5) == 0 ||
                             strncmp(line, "static ", 7) == 0 || strncmp(line, "CCResult_", 9) == 0) &&
                            strchr(line, '(') != NULL && strchr(line, '{') != NULL) {
                            /* Found a function definition - insert before this line */
                            earliest_pos = line_start;
                            break;
                        }
                        
                        /* Move to previous line */
                        if (line_start == 0) break;
                        pos = line_start - 1;
                    }
                    insert_pos = src_ufcs + earliest_pos;
                } else {
                    /* No usage found (shouldn't happen) - insert at end */
                    insert_pos = src_ufcs + src_ufcs_len;
                }
                
                if (insert_pos) {
                    size_t insert_offset = (size_t)(insert_pos - src_ufcs);
                    
                    /* Build declaration string */
                    char* decls = NULL;
                    size_t decls_len = 0, decls_cap = 0;
                    cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap, 
                        "/* --- CC result type declarations (auto-generated) --- */\n");
                    for (size_t ri = 0; ri < cc__cg_result_type_count; ri++) {
                        CCCodegenResultTypePair* p = &cc__cg_result_types[ri];
                        char line[512];
                        snprintf(line, sizeof(line), "CC_DECL_RESULT_SPEC(CCResult_%s_%s, %s, %s)\n",
                                 p->mangled_ok, p->mangled_err, p->ok_type, p->err_type);
                        cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap, line);
                    }
                    cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap, 
                        "/* --- end result type declarations --- */\n\n");
                    
                    /* Build new source: prefix + decls + suffix */
                    char* new_src = NULL;
                    size_t new_len = 0, new_cap = 0;
                    cc__sb_append_local(&new_src, &new_len, &new_cap, src_ufcs, insert_offset);
                    cc__sb_append_local(&new_src, &new_len, &new_cap, decls, decls_len);
                    cc__sb_append_local(&new_src, &new_len, &new_cap, 
                                        src_ufcs + insert_offset, src_ufcs_len - insert_offset);
                    
                    free(decls);
                    if (src_ufcs != src_all) free(src_ufcs);
                    src_ufcs = new_src;
                    src_ufcs_len = new_len;
                }
            }
        }
        /* Insert optional type declarations for custom types.
           Each CC_DECL_OPTIONAL is inserted right before the first use of that specific
           optional type, to ensure the underlying type is defined by then. */
        if (cc__cg_optional_type_count > 0) {
            /* Sort types by their first usage position (descending) so we can insert
               from end to start without invalidating positions */
            size_t type_positions[64];
            for (size_t oi = 0; oi < cc__cg_optional_type_count; oi++) {
                CCCodegenOptionalType* p = &cc__cg_optional_types[oi];
                char pattern1[256], pattern2[256];
                snprintf(pattern1, sizeof(pattern1), "CCOptional_%s", p->mangled_type);
                snprintf(pattern2, sizeof(pattern2), "__CC_OPTIONAL(%s)", p->mangled_type);
                const char* found1 = strstr(src_ufcs, pattern1);
                const char* found2 = strstr(src_ufcs, pattern2);
                size_t pos = src_ufcs_len;
                if (found1 && (size_t)(found1 - src_ufcs) < pos) {
                    pos = (size_t)(found1 - src_ufcs);
                }
                if (found2 && (size_t)(found2 - src_ufcs) < pos) {
                    pos = (size_t)(found2 - src_ufcs);
                }
                /* Back up to start of line/function */
                if (pos < src_ufcs_len) {
                    size_t search_pos = pos;
                    while (search_pos > 0) {
                        size_t line_start = search_pos;
                        while (line_start > 0 && src_ufcs[line_start - 1] != '\n') line_start--;
                        const char* line = src_ufcs + line_start;
                        while (*line == ' ' || *line == '\t') line++;
                        if ((strncmp(line, "int ", 4) == 0 || strncmp(line, "void ", 5) == 0 ||
                             strncmp(line, "static ", 7) == 0 || strncmp(line, "CCOptional_", 11) == 0 ||
                             strncmp(line, "__CC_OPTIONAL", 13) == 0 || strncmp(line, "typedef ", 8) == 0) &&
                            strchr(line, '(') != NULL) {
                            pos = line_start;
                            break;
                        }
                        if (line_start == 0) break;
                        search_pos = line_start - 1;
                    }
                }
                type_positions[oi] = pos;
            }
            
            /* Sort indices by position descending (bubble sort is fine for small N) */
            size_t sorted_indices[64];
            for (size_t i = 0; i < cc__cg_optional_type_count; i++) sorted_indices[i] = i;
            for (size_t i = 0; i < cc__cg_optional_type_count; i++) {
                for (size_t j = i + 1; j < cc__cg_optional_type_count; j++) {
                    if (type_positions[sorted_indices[j]] > type_positions[sorted_indices[i]]) {
                        size_t tmp = sorted_indices[i];
                        sorted_indices[i] = sorted_indices[j];
                        sorted_indices[j] = tmp;
                    }
                }
            }
            
            /* Insert each declaration at its position (from end to start) */
            for (size_t si = 0; si < cc__cg_optional_type_count; si++) {
                size_t oi = sorted_indices[si];
                size_t insert_offset = type_positions[oi];
                if (insert_offset >= src_ufcs_len) continue;
                
                CCCodegenOptionalType* p = &cc__cg_optional_types[oi];
                char decl[512];
                snprintf(decl, sizeof(decl), 
                    "/* CC optional for %s */\nCC_DECL_OPTIONAL(CCOptional_%s, %s)\n",
                    p->raw_type, p->mangled_type, p->raw_type);
                
                /* Build new source: prefix + decl + suffix */
                char* new_src = NULL;
                size_t new_len = 0, new_cap = 0;
                cc__sb_append_local(&new_src, &new_len, &new_cap, src_ufcs, insert_offset);
                cc__sb_append_cstr_local(&new_src, &new_len, &new_cap, decl);
                cc__sb_append_local(&new_src, &new_len, &new_cap, 
                                    src_ufcs + insert_offset, src_ufcs_len - insert_offset);
                
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = new_src;
                src_ufcs_len = new_len;
            }
        }
        /* Rewrite cc_ok(v) -> cc_ok_CCResult_T_E(v) based on enclosing function return type */
        {
            char* rew_infer = cc__rewrite_inferred_result_constructors(src_ufcs, src_ufcs_len);
            if (rew_infer) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_infer;
                src_ufcs_len = strlen(src_ufcs);
            }
        }
        /* Rewrite try expr -> cc_try(expr) */
        {
            char* rew_try = cc__rewrite_try_exprs_text(ctx, src_ufcs, src_ufcs_len);
            if (rew_try) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_try;
                src_ufcs_len = strlen(src_ufcs);
            }
        }
        /* Rewrite *opt -> cc_unwrap_opt(opt) for optional variables */
        {
            char* rew_unwrap = cc__rewrite_optional_unwrap_text(ctx, src_ufcs, src_ufcs_len);
            if (rew_unwrap) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_unwrap;
                src_ufcs_len = strlen(src_ufcs);
            }
        }
        fwrite(src_ufcs, 1, src_ufcs_len, out);
        if (src_ufcs_len == 0 || src_ufcs[src_ufcs_len - 1] != '\n') fputc('\n', out);

        free(closure_protos);
        if (closure_defs && closure_defs_len > 0) {
            /* Emit closure definitions at end-of-file so global names are in scope. */
            fputs("\n#line 1 \"<cc-generated:closures>\"\n", out);
            fwrite(closure_defs, 1, closure_defs_len, out);
        }
        free(closure_defs);
        if (src_ufcs != src_all) free(src_ufcs);
        free(src_all);
    } else {
        // Fallback stub when input is unavailable.
        fprintf(out,
                "#include \"std/prelude.cch\"\n"
                "int main(void) {\n"
                "  CCArena a = cc_heap_arena(kilobytes(1));\n"
                "  CCString s = cc_string_new(&a);\n"
                "  cc_string_push(&s, cc_slice_from_buffer(\"Hello, \", sizeof(\"Hello, \") - 1));\n"
                "  cc_string_push(&s, cc_slice_from_buffer(\"Concurrent-C via UFCS!\\n\", sizeof(\"Concurrent-C via UFCS!\\n\") - 1));\n"
                "  cc_std_out_write(cc_string_as_slice(&s));\n"
                "  return 0;\n"
                "}\n");
    }

    fclose(out);
    return 0;
}

