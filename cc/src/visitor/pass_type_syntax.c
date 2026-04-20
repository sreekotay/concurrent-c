/* pass_type_syntax.c - Type syntax lowering passes.
 *
 * Extracted from visit_codegen.c for maintainability.
 */

#include "pass_type_syntax.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/path.h"
#include "util/text.h"
#include "visitor/pass_common.h"

/* Local aliases for shared helpers */
#define cc__sb_append_local cc_sb_append
#define cc__sb_append_cstr_local cc_sb_append_cstr
#define cc__is_ident_char_local cc_is_ident_char
#define cc__is_ident_char_local2 cc_is_ident_char
#define cc__is_ident_start_local2 cc_is_ident_start
#define cc__skip_ws_local2 cc_skip_ws

static size_t cc__strip_leading_cv_qual(const char* s, size_t ty_start, char* out_qual, size_t out_cap) {
    if (!s || !out_qual || out_cap == 0) return ty_start;
    out_qual[0] = 0;
    size_t p = ty_start;
    while (s[p] == ' ' || s[p] == '\t') p++;
    for (;;) {
        int matched = 0;
        if (strncmp(s + p, "typedef", 7) == 0 && !cc__is_ident_char_local(s[p + 7])) {
            strncat(out_qual, "typedef ", out_cap - strlen(out_qual) - 1);
            p += 7;
            while (s[p] == ' ' || s[p] == '\t') p++;
            matched = 1;
        } else if (strncmp(s + p, "const", 5) == 0 && !cc__is_ident_char_local(s[p + 5])) {
            strncat(out_qual, "const ", out_cap - strlen(out_qual) - 1);
            p += 5;
            while (s[p] == ' ' || s[p] == '\t') p++;
            matched = 1;
        } else if (strncmp(s + p, "volatile", 8) == 0 && !cc__is_ident_char_local(s[p + 8])) {
            strncat(out_qual, "volatile ", out_cap - strlen(out_qual) - 1);
            p += 8;
            while (s[p] == ' ' || s[p] == '\t') p++;
            matched = 1;
        }
        if (!matched) break;
    }
    return p;
}

static size_t cc__skip_leading_decl_specs(const char* s, size_t ty_start) {
    size_t p = ty_start;
    if (!s) return p;
    while (s[p] == ' ' || s[p] == '\t') p++;
    for (;;) {
        int matched = 0;
        if (strncmp(s + p, "typedef", 7) == 0 && !cc__is_ident_char_local(s[p + 7])) {
            p += 7;
            matched = 1;
        } else if (strncmp(s + p, "static", 6) == 0 && !cc__is_ident_char_local(s[p + 6])) {
            p += 6;
            matched = 1;
        } else if (strncmp(s + p, "inline", 6) == 0 && !cc__is_ident_char_local(s[p + 6])) {
            p += 6;
            matched = 1;
        } else if (strncmp(s + p, "extern", 6) == 0 && !cc__is_ident_char_local(s[p + 6])) {
            p += 6;
            matched = 1;
        } else if (strncmp(s + p, "const", 5) == 0 && !cc__is_ident_char_local(s[p + 5])) {
            p += 5;
            matched = 1;
        } else if (strncmp(s + p, "volatile", 8) == 0 && !cc__is_ident_char_local(s[p + 8])) {
            p += 8;
            matched = 1;
        }
        if (!matched) break;
        while (s[p] == ' ' || s[p] == '\t') p++;
    }
    return p;
}

char* cc__rewrite_slice_types_text(const CCVisitorCtx* ctx, const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;

    size_t i = 0;
    size_t last_emit = 0;
    int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
    int line = 1, col = 1;

    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (c == '\n') { line++; col = 1; }

        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; col++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; col += 2; continue; } i++; col++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; col += 2; continue; } if (c == '"') in_str = 0; i++; col++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; col += 2; continue; } if (c == '\'') in_chr = 0; i++; col++; continue; }

        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; col += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; col += 2; continue; }
        if (c == '"') { in_str = 1; i++; col++; continue; }
        if (c == '\'') { in_chr = 1; i++; col++; continue; }

        if (c == '[') {
            size_t j = i + 1;
            size_t colon = (size_t)-1;
            size_t close = (size_t)-1;
            while (j < n) {
                if (src[j] == ']') { close = j; break; }
                if (src[j] == ':' && colon == (size_t)-1) colon = j;
                if (src[j] == '\n') break;
                j++;
            }
            if (colon != (size_t)-1 && close != (size_t)-1) {
                int valid_slice = 1;
                int bang_count = 0;
                for (size_t t = i + 1; t < close; t++) {
                    unsigned char ch = (unsigned char)src[t];
                    if (ch == ':') continue;
                    if (ch == '!') { bang_count++; continue; }
                    if (ch == ' ' || ch == '\t') continue;
                    if (isalnum(ch) || ch == '_') continue;
                    valid_slice = 0;
                    break;
                }
                if (bang_count > 1) valid_slice = 0;
                if (!valid_slice) { i++; col++; continue; }
                size_t k = close;
                size_t unique_pos = close;
                int is_unique = 0;
                while (unique_pos > colon && (src[unique_pos - 1] == ' ' || src[unique_pos - 1] == '\t')) unique_pos--;
                if (unique_pos > colon && src[unique_pos - 1] == '!') is_unique = 1;
                if (k >= n || src[k] != ']') {
                    char rel[1024];
                    cc_pass_error_cat(cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col, CC_ERR_TYPE, "unterminated slice type (missing ']')");
                    free(out);
                    return NULL;
                }
                /* Find start of type token sequence and preserve leading cv qualifiers */
                size_t ty_start = i;
                while (ty_start > 0) {
                    char p = src[ty_start - 1];
                    if (p == ';' || p == '{' || p == '}' || p == ',' || p == '(' || p == ')' || p == '\n') break;
                    ty_start--;
                }
                while (ty_start < i && (src[ty_start] == ' ' || src[ty_start] == '\t')) ty_start++;

                if (ty_start >= last_emit) {
                    char quals[64];
                    size_t after_qual = cc__strip_leading_cv_qual(src, ty_start, quals, sizeof(quals));
                    (void)after_qual;
                    cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                    cc__sb_append_cstr_local(&out, &out_len, &out_cap, quals);
                    cc__sb_append_cstr_local(&out, &out_len, &out_cap, is_unique ? "CCSliceUnique" : "CCSlice");
                    last_emit = k + 1;
                }
                while (i < k + 1) { if (src[i] == '\n') { line++; col = 1; } else col++; i++; }
                    continue;
                }
        }

        i++; col++;
    }

    if (last_emit < n) cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

static void cc__mangle_type_name(const char* src, size_t len, char* out, size_t out_sz) {
    cc_result_spec_mangle_type(src, len, out, out_sz);
}

/* Scan back from position `from` to find the start of a type token (delimited by ; { } , ( ) newline). */
static size_t cc__scan_back_to_type_start(const char* s, size_t from) {
    /* Walk back to a statement/expression boundary, skipping over block
     * comments so text inside them doesn't get absorbed into the type name
     * (block comments may span multiple lines and contain '*' continuations
     * that would otherwise be mangled into '_ptr_' by the type-name
     * mangler). */
    size_t i = from;
    int in_block_comment = 0;
    while (i > 0) {
        if (!in_block_comment) {
            if (i >= 2 && s[i - 2] == '*' && s[i - 1] == '/') {
                in_block_comment = 1;
                i -= 2;
                continue;
            }
            char p = s[i - 1];
            if (p == ';' || p == '{' || p == '}' || p == ',' || p == '(' || p == ')' || p == '\n') break;
            i--;
        } else {
            if (i >= 2 && s[i - 2] == '/' && s[i - 1] == '*') {
                in_block_comment = 0;
                i -= 2;
                continue;
            }
            i--;
        }
    }
    while (s[i] && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

/* Reset the result-spec registry to empty (retain allocated buffer capacity).
 * Call once per compilation unit in visit_codegen.c before type rewriting begins.
 * The old implicit per-call reset made the registry non-accumulative across calls.
 * (The retired optional registry used to be reset here too.) */
void cc__cg_reset_type_registries(void) {
    cc_result_spec_table_reset(&cc__cg_result_specs);
}

/* Collection of result type pairs for CC_DECL_RESULT_SPEC emission (extern in header).
 * Dynamic array: starts NULL, grows via realloc on demand. */
CCResultSpecTable cc__cg_result_specs = {0};

static void cc__cg_add_result_type(const char* ok, size_t ok_len, const char* err, size_t err_len,
                                    const char* mangled_ok, const char* mangled_err) {
    /* Check if this is a built-in type (in stdlib headers) - skip to avoid redefinition */
    if (cc_result_spec_is_stdlib_predeclared(mangled_ok, mangled_err)) return;
    (void)cc_result_spec_table_add(&cc__cg_result_specs,
                                   ok, ok_len, err, err_len,
                                   mangled_ok, mangled_err);
}

/* Scan for result type patterns and collect type pairs.
   Handles these formats:
   - __CC_RESULT(T, E) - from preprocessor macro approach
   - CCRes(T, E)       - convenience macro (parser mode expands to __CCResultGeneric)
   - CCResPtr(T, E)    - convenience macro for pointer types
   - CCResult_T_E      - legacy or direct usage */
/* Scan for result type patterns and collect type pairs (ACCUMULATES - does not reset).
   Call cc__cg_reset_type_registries() explicitly before a full source scan. */
static void cc__scan_for_existing_result_types(const char* src, size_t n) {
    
    const char* macro_prefix = "__CC_RESULT(";
    size_t macro_prefix_len = strlen(macro_prefix);
    const char* ccres_prefix = "CCRes(";
    size_t ccres_prefix_len = strlen(ccres_prefix);
    const char* ccresptr_prefix = "CCResPtr(";
    size_t ccresptr_prefix_len = strlen(ccresptr_prefix);
    const char* struct_prefix = "CCResult_";
    size_t struct_prefix_len = strlen(struct_prefix);
    
    size_t i = 0;
    int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
    
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        
        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Look for __CC_RESULT(T, E) macro pattern */
        if (i + macro_prefix_len < n && strncmp(src + i, macro_prefix, macro_prefix_len) == 0) {
            size_t j = i + macro_prefix_len;
            
            /* Skip whitespace */
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            
            /* Parse first type (T) */
            size_t ok_start = j;
            while (j < n && cc__is_ident_char_local(src[j])) j++;
            size_t ok_end = j;
            
            /* Skip whitespace and comma */
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j >= n || src[j] != ',') { i++; continue; }
            j++; /* skip comma */
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            
            /* Parse second type (E) */
            size_t err_start = j;
            while (j < n && cc__is_ident_char_local(src[j])) j++;
            size_t err_end = j;
            
            /* Skip to closing paren */
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j >= n || src[j] != ')') { i++; continue; }
            
            if (ok_end > ok_start && err_end > err_start) {
                char ok_type[128], err_type[128];
                size_t ok_len = ok_end - ok_start;
                size_t err_len = err_end - err_start;
                
                if (ok_len < sizeof(ok_type) && err_len < sizeof(err_type)) {
                    memcpy(ok_type, src + ok_start, ok_len);
                    ok_type[ok_len] = '\0';
                    memcpy(err_type, src + err_start, err_len);
                    err_type[err_len] = '\0';
                    
                    /* Skip built-in result types */
                    if (strcmp(err_type, "CCError") != 0) {
                        cc__cg_add_result_type(ok_type, ok_len, err_type, err_len, ok_type, err_type);
                    }
                }
            }
            i = j + 1;
            continue;
        }
        
        /* Look for CCRes(T, E) and CCResPtr(T, E) convenience macros */
        int is_ccres = (i + ccres_prefix_len < n && strncmp(src + i, ccres_prefix, ccres_prefix_len) == 0);
        int is_ccresptr = (!is_ccres && i + ccresptr_prefix_len < n && strncmp(src + i, ccresptr_prefix, ccresptr_prefix_len) == 0);
        if (is_ccres || is_ccresptr) {
            /* Make sure this isn't part of a longer identifier */
            if (i > 0 && cc__is_ident_char_local(src[i-1])) {
                i++;
                continue;
            }
            
            size_t prefix_len = is_ccres ? ccres_prefix_len : ccresptr_prefix_len;
            size_t j = i + prefix_len;
            
            /* Skip whitespace */
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            
            /* Parse first type (T) */
            size_t ok_start = j;
            while (j < n && cc__is_ident_char_local(src[j])) j++;
            size_t ok_end = j;
            
            /* Skip whitespace and comma */
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j >= n || src[j] != ',') { i++; continue; }
            j++; /* skip comma */
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            
            /* Parse second type (E) */
            size_t err_start = j;
            while (j < n && cc__is_ident_char_local(src[j])) j++;
            size_t err_end = j;
            
            /* Skip to closing paren */
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j >= n || src[j] != ')') { i++; continue; }
            
            if (ok_end > ok_start && err_end > err_start) {
                char ok_type[128], err_type[128], mangled_ok[128];
                size_t ok_len = ok_end - ok_start;
                size_t err_len = err_end - err_start;
                
                if (ok_len < sizeof(ok_type) && err_len < sizeof(err_type)) {
                    memcpy(ok_type, src + ok_start, ok_len);
                    ok_type[ok_len] = '\0';
                    memcpy(err_type, src + err_start, err_len);
                    err_type[err_len] = '\0';
                    
                    /* For CCResPtr, the mangled ok type has 'ptr' suffix */
                    if (is_ccresptr) {
                        snprintf(mangled_ok, sizeof(mangled_ok), "%sptr", ok_type);
                        /* The actual C type needs '*' suffix */
                        char actual_ok[128];
                        snprintf(actual_ok, sizeof(actual_ok), "%s*", ok_type);
                        cc__cg_add_result_type(actual_ok, strlen(actual_ok), err_type, err_len, mangled_ok, err_type);
                    } else {
                        cc__cg_add_result_type(ok_type, ok_len, err_type, err_len, ok_type, err_type);
                    }
                }
            }
            i = j + 1;
            continue;
        }
        
        /* Look for CCResult_T_E struct pattern (legacy). Concrete result names are
           already mangled, so reverse-parsing them back into ok/err types is lossy
           once either side contains underscores. Only reuse exact names we already
           know from authoritative result-spec collection. */
        if (i + struct_prefix_len < n && strncmp(src + i, struct_prefix, struct_prefix_len) == 0) {
            /* Make sure this isn't part of a longer identifier */
            if (i > 0 && cc__is_ident_char_local(src[i-1])) {
                i++;
                continue;
            }

            size_t j = i;
            char concrete_name[256];
            const CCResultSpec* spec = NULL;
            CCResultSpecTable* global_specs = cc_result_spec_table_get_global();
            while (j < n && cc__is_ident_char_local(src[j])) j++;
            if ((size_t)(j - i) >= sizeof(concrete_name)) {
                i = j;
                continue;
            }

            memcpy(concrete_name, src + i, j - i);
            concrete_name[j - i] = '\0';

            if (cc_result_spec_is_stdlib_predeclared_name(concrete_name)) {
                i = j;
                continue;
            }

            spec = cc_result_spec_table_find_by_name(&cc__cg_result_specs, concrete_name);
            if (!spec && global_specs) {
                spec = cc_result_spec_table_find_by_name(global_specs, concrete_name);
            }
            if (spec) {
                cc__cg_add_result_type(spec->ok_type, strlen(spec->ok_type),
                                       spec->err_type, strlen(spec->err_type),
                                       spec->mangled_ok, spec->mangled_err);
            }
            i = j;
            continue;
        }
        
        i++;
    }
}

/* Rewrite result types: T!>(E) -> CCResult_T_E, also collect pairs for declaration emission */
char* cc__rewrite_result_types_text(const CCVisitorCtx* ctx, const char* src, size_t n) {
    (void)ctx;
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    /* First, scan for any existing CCResult_T_E patterns (preprocessor may have already rewritten) */
    cc__scan_for_existing_result_types(src, n);

    size_t i = 0;
    size_t last_emit = 0;
    int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
    
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        
        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Detect T!>(E) pattern: type followed by '!>' followed by '(' error type ')' */
        if (c == '!' && c2 == '>') {
            /* Found '!>' sigil - now find the error type in parentheses */
            size_t sigil_pos = i;
            size_t j = i + 2;  /* skip '!>' */

            /* If the non-ws char immediately before `!>` is `)` (a closing
             * paren of a call or expression), this is the binder form
             * `CALL !> (e) BODY` of the `!>` statement operator, not a
             * type annotation.  Let pass_result_unwrap handle it later. */
            {
                size_t bk = sigil_pos;
                while (bk > 0 && (src[bk - 1] == ' ' || src[bk - 1] == '\t' ||
                                  src[bk - 1] == '\r' || src[bk - 1] == '\n')) bk--;
                if (bk > 0 && src[bk - 1] == ')') {
                    i = sigil_pos + 2;
                    continue;
                }
            }

            /* Skip whitespace */
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) j++;
            
            /* Must find '(' */
            if (j < n && src[j] == '(') {
                j++;  /* skip '(' */
                
                /* Skip whitespace inside parens */
                while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) j++;
                
                /* Find matching ')' - track nesting for complex types */
                size_t err_start = j;
                int paren_depth = 1;
                int in_s = 0, in_c = 0;
                while (j < n && paren_depth > 0) {
                    char ch = src[j];
                    if (in_s) { if (ch == '\\' && j + 1 < n) j++; else if (ch == '"') in_s = 0; j++; continue; }
                    if (in_c) { if (ch == '\\' && j + 1 < n) j++; else if (ch == '\'') in_c = 0; j++; continue; }
                    if (ch == '"') { in_s = 1; j++; continue; }
                    if (ch == '\'') { in_c = 1; j++; continue; }
                    if (ch == '(') paren_depth++;
                    else if (ch == ')') paren_depth--;
                    if (paren_depth > 0) j++;
                }
                
                if (paren_depth == 0) {
                    /* Found matching ')' at position j */
                    size_t err_end = j;
                    
                    /* Trim trailing whitespace from error type */
                    while (err_end > err_start && (src[err_end - 1] == ' ' || src[err_end - 1] == '\t' ||
                                                    src[err_end - 1] == '\n' || src[err_end - 1] == '\r')) {
                        err_end--;
                    }
                    
                    j++;  /* skip ')' */
                    
                    /* Scan back from '!>' to find the ok type start */
                    size_t ty_end = sigil_pos;
                    while (ty_end > 0 && (src[ty_end - 1] == ' ' || src[ty_end - 1] == '\t')) ty_end--;
                    
                    size_t ty_start = cc__scan_back_to_type_start(src, ty_end);
                    ty_start = cc__skip_leading_decl_specs(src, ty_start);
                    
                    if (ty_start < ty_end && err_start < err_end) {
                        size_t ty_len = ty_end - ty_start;
                        size_t err_len = err_end - err_start;
                        
                        char mangled_ok[256];
                        char mangled_err[256];
                        cc__mangle_type_name(src + ty_start, ty_len, mangled_ok, sizeof(mangled_ok));
                        cc__mangle_type_name(src + err_start, err_len, mangled_err, sizeof(mangled_err));
                        
                        if (mangled_ok[0] && mangled_err[0]) {
                            /* Collect this result type pair for declaration */
                            cc__cg_add_result_type(src + ty_start, ty_len, 
                                                   src + err_start, err_len,
                                                   mangled_ok, mangled_err);
                            
                            cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, "CCResult_");
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, mangled_ok);
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, "_");
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, mangled_err);
                            last_emit = j;
                            i = j;
                            continue;
                        }
                    }
                }
            }
        }
        
        i++;
    }
    
    if (last_emit < n) cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

char* cc__rewrite_result_field_sugar_text(const CCVisitorCtx* ctx, const char* src, size_t n) {
    (void)ctx;
    if (!src || n == 0) return NULL;

    typedef struct {
        char name[128];
    } CCResultVar;
    CCResultVar* vars = NULL;
    size_t var_count = 0, var_cap = 0;

    /* Pass 1: collect local/global identifiers declared as CCResult_* variables. */
    {
        int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
        size_t i = 0;
        while (i < n) {
            char c = src[i];
            char c2 = (i + 1 < n) ? src[i + 1] : 0;

            if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; continue; }
            if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; continue; } i++; continue; }
            if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
            if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }

            if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; continue; }
            if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; continue; }
            if (c == '"') { in_str = 1; i++; continue; }
            if (c == '\'') { in_chr = 1; i++; continue; }

            if ((i == 0 || !cc__is_ident_char_local(src[i - 1])) &&
                i + 9 < n && memcmp(src + i, "CCResult_", 9) == 0) {
                size_t j = i + 9;
                while (j < n && cc__is_ident_char_local(src[j])) j++;
                while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) j++;
                while (j < n && src[j] == '*') {
                    j++;
                    while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) j++;
                }
                if (j < n && cc__is_ident_start_local2(src[j])) {
                    size_t nm_start = j;
                    j++;
                    while (j < n && cc__is_ident_char_local(src[j])) j++;
                    size_t nm_end = j;

                    size_t k = j;
                    while (k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\n' || src[k] == '\r')) k++;
                    /* Skip function declarations/definitions. */
                    if (!(k < n && src[k] == '(')) {
                        size_t nm_len = nm_end - nm_start;
                        if (nm_len > 0 && nm_len < sizeof(vars[0].name)) {
                            int exists = 0;
                            for (size_t vi = 0; vi < var_count; vi++) {
                                if (strlen(vars[vi].name) == nm_len &&
                                    memcmp(vars[vi].name, src + nm_start, nm_len) == 0) {
                                    exists = 1;
                                    break;
                                }
                            }
                            if (!exists) {
                                if (var_count == var_cap) {
                                    size_t nc = var_cap ? var_cap * 2 : 16;
                                    CCResultVar* nv = (CCResultVar*)realloc(vars, nc * sizeof(CCResultVar));
                                    if (!nv) {
                                        free(vars);
                                        return NULL;
                                    }
                                    vars = nv;
                                    var_cap = nc;
                                }
                                memcpy(vars[var_count].name, src + nm_start, nm_len);
                                vars[var_count].name[nm_len] = '\0';
                                var_count++;
                            }
                        }
                    }
                }
                i = j;
                continue;
            }
            i++;
        }
    }

    if (var_count == 0) {
        free(vars);
        return NULL;
    }

    /* Pass 2: rewrite `res.value`/`res.error` to `res.u.value`/`res.u.error`. */
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t last_emit = 0;
    int changed = 0;
    {
        int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
        size_t i = 0;
        while (i < n) {
            char c = src[i];
            char c2 = (i + 1 < n) ? src[i + 1] : 0;

            if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; continue; }
            if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; continue; } i++; continue; }
            if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
            if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }

            if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; continue; }
            if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; continue; }
            if (c == '"') { in_str = 1; i++; continue; }
            if (c == '\'') { in_chr = 1; i++; continue; }

            if (cc__is_ident_start_local2(c)) {
                size_t id_start = i;
                i++;
                while (i < n && cc__is_ident_char_local(src[i])) i++;
                size_t id_end = i;

                int is_result_var = 0;
                for (size_t vi = 0; vi < var_count; vi++) {
                    size_t vlen = strlen(vars[vi].name);
                    if (vlen == id_end - id_start &&
                        memcmp(vars[vi].name, src + id_start, vlen) == 0) {
                        is_result_var = 1;
                        break;
                    }
                }
                if (!is_result_var) continue;

                size_t j = id_end;
                while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) j++;
                if (j >= n || src[j] != '.') continue;
                size_t dot = j;
                j++;
                while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) j++;
                if (j >= n || !cc__is_ident_start_local2(src[j])) continue;
                size_t mem_start = j;
                j++;
                while (j < n && cc__is_ident_char_local(src[j])) j++;
                size_t mem_end = j;

                size_t mlen = mem_end - mem_start;
                int is_value = (mlen == 5 && memcmp(src + mem_start, "value", 5) == 0);
                int is_error = (mlen == 5 && memcmp(src + mem_start, "error", 5) == 0);
                int is_is_ok = (mlen == 5 && memcmp(src + mem_start, "is_ok", 5) == 0);
                int is_is_err = (mlen == 6 && memcmp(src + mem_start, "is_err", 6) == 0);
                int is_unwrap = (mlen == 6 && memcmp(src + mem_start, "unwrap", 6) == 0);
                int is_unwrap_err = (mlen == 10 && memcmp(src + mem_start, "unwrap_err", 10) == 0);
                if (!is_value && !is_error && !is_is_ok && !is_is_err &&
                    !is_unwrap && !is_unwrap_err) continue;

                size_t after_mem = mem_end;
                while (after_mem < n &&
                       (src[after_mem] == ' ' || src[after_mem] == '\t' ||
                        src[after_mem] == '\n' || src[after_mem] == '\r')) {
                    after_mem++;
                }
                if (after_mem < n && src[after_mem] == '(') {
                    size_t args_start = after_mem + 1;
                    size_t args_end = args_start;
                    while (args_end < n &&
                           (src[args_end] == ' ' || src[args_end] == '\t' ||
                            src[args_end] == '\n' || src[args_end] == '\r')) {
                        args_end++;
                    }
                    if (args_end < n && src[args_end] == ')') {
                        const char* repl = NULL;
                        if (is_value || is_unwrap) repl = "cc_value";
                        else if (is_error || is_unwrap_err) repl = "cc_error";
                        else if (is_is_ok) repl = "cc_is_ok";
                        else if (is_is_err) repl = "cc_is_err";
                        if (repl) {
                            cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, id_start - last_emit);
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, repl);
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, "(");
                            cc__sb_append_local(&out, &out_len, &out_cap, src + id_start, id_end - id_start);
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, ")");
                            last_emit = args_end + 1;
                            changed = 1;
                            i = args_end + 1;
                            (void)dot;
                            continue;
                        }
                    }
                    continue;
                }

                cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, mem_start - last_emit);
                cc__sb_append_cstr_local(&out, &out_len, &out_cap, "u.");
                last_emit = mem_start;
                changed = 1;
                i = mem_end;
                (void)dot;
                continue;
            }
            i++;
        }
    }

    free(vars);
    if (!changed) {
        free(out);
        return NULL;
    }
    if (last_emit < n) cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Rewrite cc_ok(...) and cc_err(...) to fully qualified forms based on enclosing function's return type.
   Inside a function returning CCResult_T_E:
     cc_ok(v)   -> cc_ok_CCResult_T_E(v)
     cc_err(e)  -> cc_err_CCResult_T_E(e)
   This allows users to write just cc_ok(42) instead of cc_ok(int, 42). */
char* cc__rewrite_inferred_result_constructors(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t last_emit = 0;
    
    /* Track current function's result type (if any) */
    char current_result_type[256] = {0};  /* e.g., "CCResult_int_CCError" */
    int brace_depth = 0;
    int fn_brace_depth = -1;  /* brace depth when we entered the function body */
    
    int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
    size_t i = 0;
    
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        /* Handle comment/string states */
        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        
        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Track brace depth */
        if (c == '{') {
            brace_depth++;
            i++;
            continue;
        }
        if (c == '}') {
            brace_depth--;
            /* Exit function scope - use <= to catch when we return to the function's starting level */
            if (fn_brace_depth >= 0 && brace_depth <= fn_brace_depth) {
                current_result_type[0] = 0;
                fn_brace_depth = -1;
            }
            i++;
            continue;
        }
        
        /* Detect function definition with result return type.
           Handles: __CC_RESULT(T, E), CCRes(T, E), CCResPtr(T, E), CCResult_T_E */
        if (fn_brace_depth < 0) {
            char detected_type[256] = {0};
            size_t j = i;
            
            /* Check for __CC_RESULT(T, E) pattern */
            if (c == '_' && i + 12 < n && memcmp(src + i, "__CC_RESULT(", 12) == 0) {
                j = i + 12;
                while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                size_t t_start = j;
                while (j < n && cc__is_ident_char_local(src[j])) j++;
                size_t t_end = j;
                while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                if (j < n && src[j] == ',') {
                    j++;
                    while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                    size_t e_start = j;
                    while (j < n && cc__is_ident_char_local(src[j])) j++;
                    size_t e_end = j;
                    while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                    if (j < n && src[j] == ')' && t_end > t_start && e_end > e_start) {
                        j++; /* skip ')' */
                        /* Build CCResult_T_E from the macro args */
                        snprintf(detected_type, sizeof(detected_type), "CCResult_%.*s_%.*s",
                                 (int)(t_end - t_start), src + t_start,
                                 (int)(e_end - e_start), src + e_start);
                    }
                }
            }
            /* Check for CCRes(T, E) or CCResPtr(T, E) convenience macro patterns */
            else if (c == 'C' && i + 6 < n && (memcmp(src + i, "CCRes(", 6) == 0 || 
                     (i + 9 < n && memcmp(src + i, "CCResPtr(", 9) == 0))) {
                int is_ptr = (memcmp(src + i, "CCResPtr(", 9) == 0);
                j = i + (is_ptr ? 9 : 6);
                while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                size_t t_start = j;
                while (j < n && cc__is_ident_char_local(src[j])) j++;
                size_t t_end = j;
                while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                if (j < n && src[j] == ',') {
                    j++;
                    while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                    size_t e_start = j;
                    while (j < n && cc__is_ident_char_local(src[j])) j++;
                    size_t e_end = j;
                    while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                    if (j < n && src[j] == ')' && t_end > t_start && e_end > e_start) {
                        j++; /* skip ')' */
                        /* Build CCResult_Tptr_E or CCResult_T_E from the macro args */
                        if (is_ptr) {
                            snprintf(detected_type, sizeof(detected_type), "CCResult_%.*sptr_%.*s",
                                     (int)(t_end - t_start), src + t_start,
                                     (int)(e_end - e_start), src + e_start);
                        } else {
                            snprintf(detected_type, sizeof(detected_type), "CCResult_%.*s_%.*s",
                                     (int)(t_end - t_start), src + t_start,
                                     (int)(e_end - e_start), src + e_start);
                        }
                    }
                }
            }
            /* Check for CCResult_T_E pattern (legacy) */
            else if (c == 'C' && i + 9 < n && memcmp(src + i, "CCResult_", 9) == 0) {
                size_t type_start = i;
                j = i + 9;
                while (j < n && cc__is_ident_char_local(src[j])) j++;
                if (j < n && src[j] == '_') {
                    j++;
                    while (j < n && cc__is_ident_char_local(src[j])) j++;
                }
                size_t tlen = j - type_start;
                if (tlen < sizeof(detected_type) - 1) {
                    memcpy(detected_type, src + type_start, tlen);
                    detected_type[tlen] = 0;
                }
            }
            
            /* If we detected a result type, check if this is a function definition */
            if (detected_type[0]) {
                while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r' || src[j] == '*')) j++;
                if (j < n && cc__is_ident_start_local2(src[j])) {
                    /* Skip function name */
                    while (j < n && cc__is_ident_char_local(src[j])) j++;
                    while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                    if (j < n && src[j] == '(') {
                        /* Skip params */
                        int pdepth = 1;
                        j++;
                        while (j < n && pdepth > 0) {
                            if (src[j] == '(') pdepth++;
                            else if (src[j] == ')') pdepth--;
                            else if (src[j] == '"') { j++; while (j < n && src[j] != '"') { if (src[j] == '\\' && j+1 < n) j++; j++; } }
                            else if (src[j] == '\'') { j++; while (j < n && src[j] != '\'') { if (src[j] == '\\' && j+1 < n) j++; j++; } }
                            j++;
                        }
                        while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) j++;
                        if (j < n && src[j] == '{') {
                            /* This is a function definition! Save the result type */
                            strcpy(current_result_type, detected_type);
                            fn_brace_depth = brace_depth;  /* Will increment when we hit the '{' */
                        }
                    }
                }
                i++;
                continue;
            }
        }
        
        /* Detect cc_ok(...) or cc_err(...) when inside a result-returning function */
        if (current_result_type[0] && c == 'c' && i + 5 < n) {
            int is_ok = (memcmp(src + i, "cc_ok(", 6) == 0);
            int is_err = (memcmp(src + i, "cc_err(", 7) == 0);
            
            if (is_ok || is_err) {
                /* Check word boundary before */
                int word_start = (i == 0) || !cc__is_ident_char_local(src[i-1]);
                if (word_start) {
                    size_t macro_start = i;
                    size_t paren_pos = i + (is_ok ? 5 : 6);
                    
                    /* Check if it's the short form (no type args) by looking at args */
                    /* Short form: cc_ok(value) or cc_err(error)
                       Long form: cc_ok(Type, value) or cc_ok(Type, ErrType, value) */
                    size_t args_start = paren_pos + 1;
                    size_t j = args_start;
                    int depth = 1;
                    int comma_count = 0;
                    int in_s = 0, in_c = 0;
                    while (j < n && depth > 0) {
                        char ch = src[j];
                        if (in_s) { if (ch == '\\' && j+1 < n) j++; else if (ch == '"') in_s = 0; j++; continue; }
                        if (in_c) { if (ch == '\\' && j+1 < n) j++; else if (ch == '\'') in_c = 0; j++; continue; }
                        if (ch == '"') { in_s = 1; j++; continue; }
                        if (ch == '\'') { in_c = 1; j++; continue; }
                        if (ch == '(') depth++;
                        else if (ch == ')') { depth--; if (depth == 0) break; }
                        else if (ch == ',' && depth == 1) comma_count++;
                        j++;
                    }
                    
                    /* Shorthand error constructors - detect error type and wrap appropriately */
                    size_t crt_len = strlen(current_result_type);
                    int is_default_err = (crt_len >= 8 &&
                                          strcmp(current_result_type + crt_len - 8, "_CCError") == 0);
                    int is_io_err = (crt_len >= 10 &&
                                    strcmp(current_result_type + crt_len - 10, "_CCIoError") == 0);
                    
                    if (is_err && depth == 0) {
                        size_t k = args_start;
                        while (k < j && (src[k] == ' ' || src[k] == '\t')) k++;
                        
                        /* Default-CCError constructors:
                         *   cc_err(KIND)           -> cc_err_...(CC_ERROR(KIND, NULL))
                         *   cc_err(KIND, MSG)      -> cc_err_...(CC_ERROR(KIND, MSG))
                         * The 1-arg form is only taken when KIND is a
                         * `CC_ERR_*` literal, because a 1-arg cc_err(e) with
                         * a CCError-typed `e` is the pass-through short form
                         * handled below.  The 2-arg form is unambiguous —
                         * the second argument is a message, so the first
                         * must be a CCErrorKind (literal or expression). */
                        int is_default_2arg = (is_default_err && comma_count == 1);
                        int is_default_kind_literal =
                            (is_default_err && comma_count == 0 &&
                             k + 7 < j && memcmp(src + k, "CC_ERR_", 7) == 0);
                        if (is_default_2arg || is_default_kind_literal) {
                            cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, macro_start - last_emit);
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, "cc_err_");
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, current_result_type);
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, "(CC_ERROR(");
                            cc__sb_append_local(&out, &out_len, &out_cap, src + args_start, j - args_start);
                            if (comma_count == 0) {
                                cc__sb_append_cstr_local(&out, &out_len, &out_cap, ", NULL");
                            }
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, "))");
                            last_emit = j + 1;
                            i = j + 1;
                            continue;
                        }
                        
                        /* cc_err(CC_IO_*) -> cc_err_...(cc_io_error(CC_IO_*)) */
                        if (is_io_err && k + 6 < j && memcmp(src + k, "CC_IO_", 6) == 0) {
                            cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, macro_start - last_emit);
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, "cc_err_");
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, current_result_type);
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, "(cc_io_error(");
                            cc__sb_append_local(&out, &out_len, &out_cap, src + args_start, j - args_start);
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, "))");
                            last_emit = j + 1;
                            i = j + 1;
                            continue;
                        }
                    }

                    /* Short form has 0 commas for cc_ok(v) or cc_err(e)
                       Long form has 1+ commas for cc_ok(T,v) or cc_ok(T,E,v) */
                    int is_short_form = (is_ok && comma_count == 0) || (is_err && comma_count == 0);
                    
                    if (is_short_form && depth == 0) {
                        /* Rewrite cc_ok(v) -> cc_ok_CCResult_T_E(v) */
                        cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, macro_start - last_emit);
                        if (is_ok) {
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, "cc_ok_");
                        } else {
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, "cc_err_");
                        }
                        cc__sb_append_cstr_local(&out, &out_len, &out_cap, current_result_type);
                        cc__sb_append_cstr_local(&out, &out_len, &out_cap, "(");
                        /* Copy the argument */
                        cc__sb_append_local(&out, &out_len, &out_cap, src + args_start, j - args_start);
                        cc__sb_append_cstr_local(&out, &out_len, &out_cap, ")");
                        last_emit = j + 1;  /* skip past the closing ')' */
                        i = j + 1;
                        continue;
                    }
                }
            }
        }
        
        i++;
    }
    
    if (last_emit == 0) return NULL;  /* No rewrites done */
    if (last_emit < n) cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Rewrite try expressions: try expr -> cc_try(expr) */
char* cc__rewrite_try_exprs_text(const CCVisitorCtx* ctx, const char* src, size_t n) {
    (void)ctx;
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;

    size_t i = 0;
    size_t last_emit = 0;
    int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
    
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        
        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Detect 'try' keyword followed by space and not followed by '{' (try-block form, handled elsewhere) */
        if (c == 't' && i + 3 < n && src[i+1] == 'r' && src[i+2] == 'y') {
            /* Check word boundary before */
            int word_start = (i == 0) || !cc__is_ident_char_local(src[i-1]);
            if (word_start) {
                size_t after_try = i + 3;
                /* Skip whitespace */
                while (after_try < n && (src[after_try] == ' ' || src[after_try] == '\t')) after_try++;
                
                /* Check it's not try { block } form */
                if (after_try < n && src[after_try] != '{' && cc__is_ident_char_local(src[after_try]) == 0 && src[after_try] != '(') {
                    /* Not a try-block or try identifier, skip */
                } else if (after_try < n && src[after_try] == '{') {
                    /* try { ... } block form - skip, not handled here */
                } else if (after_try < n && (cc__is_ident_start_local2(src[after_try]) || src[after_try] == '(')) {
                    /* 'try expr' form - need to find end of expression */
                    size_t expr_start = after_try;
                    size_t expr_end = expr_start;
                    
                    /* Scan expression with balanced parens/braces */
                    int paren = 0, brace = 0, bracket = 0;
                    int in_s = 0, in_c = 0;
                    while (expr_end < n) {
                        char ec = src[expr_end];
                        
                        if (in_s) { if (ec == '\\' && expr_end + 1 < n) { expr_end += 2; continue; } if (ec == '"') in_s = 0; expr_end++; continue; }
                        if (in_c) { if (ec == '\\' && expr_end + 1 < n) { expr_end += 2; continue; } if (ec == '\'') in_c = 0; expr_end++; continue; }
                        if (ec == '"') { in_s = 1; expr_end++; continue; }
                        if (ec == '\'') { in_c = 1; expr_end++; continue; }
                        
                        if (ec == '(' ) { paren++; expr_end++; continue; }
                        if (ec == ')' ) { if (paren > 0) { paren--; expr_end++; continue; } else break; }
                        if (ec == '{' ) { brace++; expr_end++; continue; }
                        if (ec == '}' ) { if (brace > 0) { brace--; expr_end++; continue; } else break; }
                        if (ec == '[' ) { bracket++; expr_end++; continue; }
                        if (ec == ']' ) { if (bracket > 0) { bracket--; expr_end++; continue; } else break; }
                        
                        /* End expression at ';', ',', or unbalanced ')' */
                        if (paren == 0 && brace == 0 && bracket == 0) {
                            if (ec == ';' || ec == ',') break;
                        }
                        
                        expr_end++;
                    }
                    
                    /* Only rewrite if we found a valid expression */
                    if (expr_end > expr_start) {
                        /* Emit: everything up to 'try', then 'cc_try(', then expr, then ')' */
                        cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                        cc__sb_append_cstr_local(&out, &out_len, &out_cap, "cc_try(");
                        cc__sb_append_local(&out, &out_len, &out_cap, src + expr_start, expr_end - expr_start);
                        cc__sb_append_cstr_local(&out, &out_len, &out_cap, ")");
                        last_emit = expr_end;
                        i = expr_end;
                        continue;
                    }
                }
            }
        }
        
        i++;
    }
    
    if (last_emit < n) cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

