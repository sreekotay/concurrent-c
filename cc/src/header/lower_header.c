/*
 * lower_header.c - Header lowering implementation
 *
 * Transforms .cch files to .h files by rewriting CC type syntax
 * and emitting guarded type declarations.
 */

#include "lower_header.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "preprocess/preprocess.h"
#include "util/text.h"
#include "visitor/pass_type_syntax.h"

/* Built-in optional types that are already declared in cc_optional.cch */
static const char* cc__builtin_optional_types[] = {
    "int", "bool", "size_t", "intptr_t", "char", "float", "double",
    "voidptr", "charptr", "intptr", "CCSlice",
    NULL
};

static int cc__is_builtin_optional(const char* mangled) {
    for (int i = 0; cc__builtin_optional_types[i]; i++) {
        if (strcmp(mangled, cc__builtin_optional_types[i]) == 0) return 1;
    }
    return 0;
}

/* Mangle a type name for use in CCResult_T_E or CCOptional_T */
static void cc__mangle_type(const char* src, size_t len, char* out, size_t out_sz) {
    cc_result_spec_mangle_type(src, len, out, out_sz);
}

/* Scan back from position to find type start (delimited by ; { } , ( ) newline).
 * Skips over block comments so text inside them (including '*' continuations
 * that mangle to '_ptr_') doesn't get absorbed into the type name. */
static size_t cc__scan_back_to_type_start(const char* s, size_t from) {
    size_t i = from;
    int in_block_comment = 0;
    while (i > 0) {
        if (in_block_comment) {
            if (i >= 2 && s[i - 2] == '/' && s[i - 1] == '*') {
                in_block_comment = 0;
                i -= 2;
                continue;
            }
            i--;
            continue;
        }
        if (i >= 2 && s[i - 2] == '*' && s[i - 1] == '/') {
            in_block_comment = 1;
            i -= 2;
            continue;
        }
        char p = s[i - 1];
        if (p == ';' || p == '{' || p == '}' || p == ',' || p == '(' || p == ')' || p == '\n') break;
        i--;
    }
    while (s[i] && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

static int cc__keyword_eq(const char* s, size_t len, const char* kw) {
    return strlen(kw) == len && strncmp(s, kw, len) == 0;
}

static int cc__is_leading_type_prefix_keyword(const char* s, size_t len) {
    return cc__keyword_eq(s, len, "static") ||
           cc__keyword_eq(s, len, "extern") ||
           cc__keyword_eq(s, len, "inline") ||
           cc__keyword_eq(s, len, "__inline") ||
           cc__keyword_eq(s, len, "__inline__") ||
           cc__keyword_eq(s, len, "register") ||
           cc__keyword_eq(s, len, "auto") ||
           cc__keyword_eq(s, len, "const") ||
           cc__keyword_eq(s, len, "volatile");
}

static size_t cc__skip_leading_type_prefix_keywords(const char* s, size_t start, size_t end) {
    size_t i = start;
    while (i < end) {
        size_t kw_start = i;
        size_t kw_end;
        while (kw_start < end && (s[kw_start] == ' ' || s[kw_start] == '\t')) kw_start++;
        kw_end = kw_start;
        while (kw_end < end && (isalnum((unsigned char)s[kw_end]) || s[kw_end] == '_')) kw_end++;
        if (kw_end == kw_start) break;
        if (!cc__is_leading_type_prefix_keyword(s + kw_start, kw_end - kw_start)) break;
        i = kw_end;
        while (i < end && (s[i] == ' ' || s[i] == '\t')) i++;
    }
    return i;
}

void cc_lower_state_init(CCLowerState* state) {
    if (state) {
        memset(state, 0, sizeof(*state));
        cc_result_spec_table_init(&state->result_specs);
    }
}

void cc_lower_state_add_result(CCLowerState* state,
                                const char* ok_type, size_t ok_len,
                                const char* err_type, size_t err_len,
                                const char* mangled_ok,
                                const char* mangled_err) {
    if (!state || !ok_type || !err_type || !mangled_ok || !mangled_err) return;
    if (cc_result_spec_is_core_builtin(mangled_ok, mangled_err) ||
        cc_result_spec_is_stdlib_predeclared(mangled_ok, mangled_err)) return;
    (void)cc_result_spec_table_add(&state->result_specs,
                                   ok_type, ok_len, err_type, err_len,
                                   mangled_ok, mangled_err);
}

void cc_lower_state_add_optional(CCLowerState* state,
                                  const char* raw_type, size_t raw_len,
                                  const char* mangled_type) {
    if (!state || !raw_type || !mangled_type) return;
    
    /* Skip built-in optional types */
    if (cc__is_builtin_optional(mangled_type)) return;
    
    /* Check for duplicates */
    for (size_t i = 0; i < state->optional_type_count; i++) {
        if (strcmp(state->optional_types[i].mangled_type, mangled_type) == 0) {
            return;
        }
    }
    
    if (state->optional_type_count >= 64) return;
    
    CCLowerOptionalType* p = &state->optional_types[state->optional_type_count++];
    
    if (raw_len >= sizeof(p->raw_type)) raw_len = sizeof(p->raw_type) - 1;
    memcpy(p->raw_type, raw_type, raw_len);
    p->raw_type[raw_len] = '\0';
    
    strncpy(p->mangled_type, mangled_type, sizeof(p->mangled_type) - 1);
    p->mangled_type[sizeof(p->mangled_type) - 1] = '\0';
}

char* cc_lower_state_emit_decls(const CCLowerState* state) {
    if (!state) return NULL;
    if (state->result_specs.count == 0 && state->optional_type_count == 0) return NULL;
    
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    /* Emit optional type declarations */
    for (size_t i = 0; i < state->optional_type_count; i++) {
        const CCLowerOptionalType* p = &state->optional_types[i];
        char decl[512];
        snprintf(decl, sizeof(decl),
            "#ifndef CCOptional_%s_DEFINED\n"
            "#define CCOptional_%s_DEFINED\n"
            "CC_DECL_OPTIONAL(CCOptional_%s, %s)\n"
            "#endif\n",
            p->mangled_type, p->mangled_type, p->mangled_type, p->raw_type);
        cc_sb_append_cstr(&out, &out_len, &out_cap, decl);
    }
    
    /* Emit result type declarations */
    for (size_t i = 0; i < state->result_specs.count; i++) {
        const CCResultSpec* p = cc_result_spec_table_get(&state->result_specs, i);
        char decl[512];
        if (!p) continue;
        cc_result_spec_emit_decl(p, decl, sizeof(decl));
        cc_sb_append_cstr(&out, &out_len, &out_cap, decl);
    }
    
    return out;
}

static char* cc_lower_state_emit_parser_result_aliases(const CCLowerState* state) {
    if (!state || state->result_specs.count == 0) return NULL;

    char* out = NULL;
    size_t out_len = 0, out_cap = 0;

    for (size_t i = 0; i < state->result_specs.count; i++) {
        const CCResultSpec* p = cc_result_spec_table_get(&state->result_specs, i);
        if (!p || !p->mangled_ok[0] || !p->mangled_err[0]) continue;
        if (cc_result_spec_is_core_builtin(p->mangled_ok, p->mangled_err) ||
            cc_result_spec_is_stdlib_predeclared(p->mangled_ok, p->mangled_err)) continue;

        char decl[512];
        snprintf(decl, sizeof(decl),
                 "#ifndef CCResult_%s_%s_DEFINED\n"
                 "#define CCResult_%s_%s_DEFINED 1\n"
                 "typedef __CCResultGeneric CCResult_%s_%s;\n"
                 "#ifndef cc_ok_CCResult_%s_%s\n"
                 "#define cc_ok_CCResult_%s_%s(...) __cc_result_generic_ok()\n"
                 "#endif\n"
                 "#ifndef cc_err_CCResult_%s_%s\n"
                 "#define cc_err_CCResult_%s_%s(...) __cc_result_generic_err()\n"
                 "#endif\n"
                 "#endif\n",
                 p->mangled_ok, p->mangled_err,
                 p->mangled_ok, p->mangled_err,
                 p->mangled_ok, p->mangled_err,
                 p->mangled_ok, p->mangled_err,
                 p->mangled_ok, p->mangled_err,
                 p->mangled_ok, p->mangled_err,
                 p->mangled_ok, p->mangled_err);
        cc_sb_append_cstr(&out, &out_len, &out_cap, decl);
    }

    return out;
}

/*
 * Rewrite T!>(E) syntax to CCResult_T_E and collect type pairs.
 */
static char* cc__lower_result_types(const char* src, size_t n, CCLowerState* state) {
    if (!src || n == 0) return NULL;
    
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0, last_emit = 0;
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
        
        /* Detect T!>(E) pattern */
        if (c == '!' && c2 == '>') {
            size_t sigil_pos = i;
            size_t j = i + 2;  /* skip '!>' */
            
            /* Skip whitespace */
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) j++;
            
            /* Must find '(' */
            if (j < n && src[j] == '(') {
                j++;  /* skip '(' */
                
                /* Skip whitespace inside parens */
                while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) j++;
                
                /* Find matching ')' */
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
                    size_t err_end = j;
                    
                    /* Trim trailing whitespace from error type */
                    while (err_end > err_start && (src[err_end - 1] == ' ' || src[err_end - 1] == '\t' ||
                                                    src[err_end - 1] == '\n' || src[err_end - 1] == '\r')) {
                        err_end--;
                    }
                    
                    j++;  /* skip ')' */
                    
                    /* Scan back from '!>' to find the ok type */
                    size_t ty_end = sigil_pos;
                    while (ty_end > 0 && (src[ty_end - 1] == ' ' || src[ty_end - 1] == '\t')) ty_end--;
                    
                    size_t ty_start = cc__scan_back_to_type_start(src, ty_end);
                    
                    if (ty_start < ty_end && err_start < err_end) {
                        ty_start = cc__skip_leading_type_prefix_keywords(src, ty_start, ty_end);
                        size_t ty_len = ty_end - ty_start;
                        size_t err_len = err_end - err_start;
                        
                        char mangled_ok[256];
                        char mangled_err[256];
                        cc__mangle_type(src + ty_start, ty_len, mangled_ok, sizeof(mangled_ok));
                        cc__mangle_type(src + err_start, err_len, mangled_err, sizeof(mangled_err));
                        
                        if (mangled_ok[0] && mangled_err[0]) {
                            /* Collect this result type pair */
                            if (state) {
                                cc_lower_state_add_result(state,
                                    src + ty_start, ty_len,
                                    src + err_start, err_len,
                                    mangled_ok, mangled_err);
                            }
                            
                            /* Emit: everything up to type start, then CCResult_T_E */
                            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "CCResult_");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, mangled_ok);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "_");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, mangled_err);
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
    
    if (last_emit == 0) return NULL;  /* No rewrites needed */
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/*
 * Rewrite T? syntax to CCOptional_T and collect types.
 */
static char* cc__lower_optional_types(const char* src, size_t n, CCLowerState* state) {
    if (!src || n == 0) return NULL;
    
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0, last_emit = 0;
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
        
        /* Detect T? pattern: identifier followed by '?' (not '?:' ternary or '??') */
        if (c == '?' && c2 != ':' && c2 != '?') {
            if (i > 0) {
                char prev = src[i - 1];
                /* Valid type-ending chars: identifier char, ')', ']', '>' */
                if (cc_is_ident_char(prev) || prev == ')' || prev == ']' || prev == '>') {
                    size_t ty_start = cc__scan_back_to_type_start(src, i);
                    if (ty_start < i) {
                        size_t ty_len = i - ty_start;
                        char mangled[256];
                        cc__mangle_type(src + ty_start, ty_len, mangled, sizeof(mangled));
                        
                        if (mangled[0]) {
                            /* Collect this optional type */
                            if (state) {
                                cc_lower_state_add_optional(state, src + ty_start, ty_len, mangled);
                            }
                            
                            /* Emit: everything up to type start, then CCOptional_T */
                            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "CCOptional_");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, mangled);
                            last_emit = i + 1;  /* skip past '?' */
                        }
                    }
                }
            }
        }
        
        i++;
    }
    
    if (last_emit == 0) return NULL;  /* No rewrites needed */
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

static char* cc__strip_comptime_blocks_header(const char* src, size_t n) {
    char* out = NULL;
    size_t i = 0;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    if (!src || n == 0) return NULL;
    out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n);
    out[n] = '\0';
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && c2) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && c2) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        if (c == '@' && i + strlen("@comptime") < n && strncmp(src + i, "@comptime", strlen("@comptime")) == 0) {
            size_t p = i + strlen("@comptime");
            size_t body_l = 0, body_r = 0;
            int depth = 0, ls = 0, lc = 0, st = 0, ch = 0;
            while (p < n && isspace((unsigned char)src[p])) p++;
            if (p >= n || src[p] != '{') { i++; continue; }
            body_l = p;
            for (p = body_l; p < n; ++p) {
                char d = src[p];
                char d2 = (p + 1 < n) ? src[p + 1] : 0;
                if (lc) { if (d == '\n') lc = 0; continue; }
                if (ls) { if (d == '*' && d2 == '/') { ls = 0; p++; } continue; }
                if (st) { if (d == '\\' && d2) { p++; continue; } if (d == '"') st = 0; continue; }
                if (ch) { if (d == '\\' && d2) { p++; continue; } if (d == '\'') ch = 0; continue; }
                if (d == '/' && d2 == '/') { lc = 1; p++; continue; }
                if (d == '/' && d2 == '*') { ls = 1; p++; continue; }
                if (d == '"') { st = 1; continue; }
                if (d == '\'') { ch = 1; continue; }
                if (d == '{') { depth++; continue; }
                if (d == '}') {
                    if (depth == 0) break;
                    depth--;
                    if (depth == 0) { body_r = p; break; }
                }
            }
            if (body_r <= body_l) { free(out); return NULL; }
            for (size_t k = i; k <= body_r; ++k) {
                if (out[k] != '\n') out[k] = ' ';
            }
            i = body_r + 1;
            continue;
        }
        i++;
    }
    return out;
}

static char* cc__rewrite_header_includes(const char* src, size_t n) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t last_emit = 0;
    size_t i = 0;
    if (!src || n == 0) return NULL;

    while (i < n) {
        if (i + 5 <= n && strncmp(src + i, ".cch\"", 5) == 0) {
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
            cc_sb_append_cstr(&out, &out_len, &out_cap, ".h\"");
            i += 5;
            last_emit = i;
            continue;
        }
        if (i + 5 <= n && strncmp(src + i, ".cch>", 5) == 0) {
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
            cc_sb_append_cstr(&out, &out_len, &out_cap, ".h>");
            i += 5;
            last_emit = i;
            continue;
        }
        i++;
    }

    if (last_emit == 0) return NULL;
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

char* cc_lower_header_string(const char* input, size_t input_len, const char* input_path) {
    (void)input_path;  /* For error messages - not used yet */
    
    if (!input || input_len == 0) return NULL;
    
    CCLowerState state;
    cc_lower_state_init(&state);
    
    /* Current processing buffer */
    const char* cur = input;
    size_t cur_len = input_len;
    char* buf0 = NULL;
    char* buf_inc = NULL;
    char* buf_types = NULL;
    char* buf_result_ctors = NULL;
    char* buf2 = NULL;
    char* buf_result_fields = NULL;
    char* buf3 = NULL;
    char* buf_unwrap = NULL;
    
    /* Pass 0: Strip raw @comptime blocks so lowered headers are valid C. */
    buf0 = cc__strip_comptime_blocks_header(cur, cur_len);
    if (buf0) {
        cur = buf0;
        cur_len = strlen(buf0);
    }

    /* Pass 0b: Rewrite .cch includes to .h so lowered headers are self-contained C headers. */
    buf_inc = cc__rewrite_header_includes(cur, cur_len);
    if (buf_inc) {
        cur = buf_inc;
        cur_len = strlen(buf_inc);
    }

    /* Pass 0c: Share header-safe type-syntax lowering with preprocess.c so
       constructs like `char[:]` and generic container types lower the same
       way they do in the main compiler pipeline. */
    buf_types = cc_rewrite_header_type_syntax_shared(cur, cur_len, input_path);
    if (buf_types) {
        cur = buf_types;
        cur_len = strlen(buf_types);
    }

    /* Pass 1: Rewrite T!>(E) -> CCResult_T_E */
    buf2 = cc__lower_result_types(cur, cur_len, &state);
    if (buf2) {
        cur = buf2;
        cur_len = strlen(buf2);
    }

    /* Pass 1a: Once result types are concrete, infer bare constructors using
       the same shared source-lowering pass used for normal source files. */
    buf_result_ctors = cc__rewrite_inferred_result_constructors(cur, cur_len);
    if (buf_result_ctors) {
        cur = buf_result_ctors;
        cur_len = strlen(buf_result_ctors);
    }

    /* Pass 1b: Lower result field sugar using the same shared rewrite as
       source files so `res.value` / `res.error` survive in header bodies. */
    buf_result_fields = cc__rewrite_result_field_sugar_text(NULL, cur, cur_len);
    if (buf_result_fields) {
        cur = buf_result_fields;
        cur_len = strlen(buf_result_fields);
    }
    
    /* Pass 2: Rewrite T? -> CCOptional_T */
    buf3 = cc__lower_optional_types(cur, cur_len, &state);
    if (buf3) {
        cur = buf3;
        cur_len = strlen(buf3);
    }

    buf_unwrap = cc__rewrite_optional_unwrap_text(NULL, cur, cur_len);
    if (buf_unwrap) {
        cur = buf_unwrap;
        cur_len = strlen(buf_unwrap);
    }
    
    /* Build final output */
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    char* parser_result_aliases = cc_lower_state_emit_parser_result_aliases(&state);
    
    /* Emit auto-generated type declarations at the end of the file 
       (before any closing #endif for the include guard) */
    char* decls = cc_lower_state_emit_decls(&state);
    
    if (decls) {
        /* Find the location of the final #endif (include guard) */
        const char* last_endif = NULL;
        const char* p = cur;
        while ((p = strstr(p, "#endif")) != NULL) {
            last_endif = p;
            p++;
        }

        if (last_endif) {
            /* Insert declarations before the final #endif */
            size_t insert_pos = (size_t)(last_endif - cur);
            if (parser_result_aliases && parser_result_aliases[0]) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "#ifdef CC_PARSER_MODE\n");
                cc_sb_append_cstr(&out, &out_len, &out_cap, parser_result_aliases);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "#endif\n");
            }
            cc_sb_append(&out, &out_len, &out_cap, cur, insert_pos);
            cc_sb_append_cstr(&out, &out_len, &out_cap, "\n/* --- CC auto-generated type declarations --- */\n");
            cc_sb_append_cstr(&out, &out_len, &out_cap, "#ifndef CC_PARSER_MODE\n");
            cc_sb_append_cstr(&out, &out_len, &out_cap, decls);
            cc_sb_append_cstr(&out, &out_len, &out_cap, "#endif /* !CC_PARSER_MODE */\n");
            cc_sb_append_cstr(&out, &out_len, &out_cap, "/* --- end auto-generated --- */\n\n");
            cc_sb_append(&out, &out_len, &out_cap, last_endif, cur_len - insert_pos);
        } else {
            /* No include guard found - append at end */
            if (parser_result_aliases && parser_result_aliases[0]) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "#ifdef CC_PARSER_MODE\n");
                cc_sb_append_cstr(&out, &out_len, &out_cap, parser_result_aliases);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "#endif\n");
            }
            cc_sb_append(&out, &out_len, &out_cap, cur, cur_len);
            cc_sb_append_cstr(&out, &out_len, &out_cap, "\n/* --- CC auto-generated type declarations --- */\n");
            cc_sb_append_cstr(&out, &out_len, &out_cap, "#ifndef CC_PARSER_MODE\n");
            cc_sb_append_cstr(&out, &out_len, &out_cap, decls);
            cc_sb_append_cstr(&out, &out_len, &out_cap, "#endif /* !CC_PARSER_MODE */\n");
            cc_sb_append_cstr(&out, &out_len, &out_cap, "/* --- end auto-generated --- */\n");
        }
        free(decls);
    } else if (cur != input) {
        /* No declarations to add, but we did rewrites */
        if (parser_result_aliases && parser_result_aliases[0]) {
            cc_sb_append_cstr(&out, &out_len, &out_cap, "#ifdef CC_PARSER_MODE\n");
            cc_sb_append_cstr(&out, &out_len, &out_cap, parser_result_aliases);
            cc_sb_append_cstr(&out, &out_len, &out_cap, "#endif\n");
            cc_sb_append(&out, &out_len, &out_cap, cur, cur_len);
        } else {
            out = strdup(cur);
            out_len = cur_len;
        }
    }
    
    /* Cleanup */
    free(buf0);
    free(buf_inc);
    free(buf_types);
    free(buf_result_ctors);
    free(buf2);
    free(buf_result_fields);
    free(buf3);
    free(buf_unwrap);
    free(parser_result_aliases);
    cc_result_spec_table_free(&state.result_specs);
    
    return out;
}

int cc_lower_header(const char* cch_path, const char* h_path) {
    if (!cch_path || !h_path) return EINVAL;
    
    /* Read input file */
    FILE* in = fopen(cch_path, "r");
    if (!in) return errno ? errno : -1;
    
    fseek(in, 0, SEEK_END);
    long len = ftell(in);
    fseek(in, 0, SEEK_SET);
    
    if (len <= 0) {
        fclose(in);
        return EINVAL;
    }
    
    char* input = (char*)malloc(len + 1);
    if (!input) {
        fclose(in);
        return ENOMEM;
    }
    
    size_t read_len = fread(input, 1, len, in);
    fclose(in);
    input[read_len] = '\0';
    
    /* Lower the content */
    char* output = cc_lower_header_string(input, read_len, cch_path);
    
    /* If no changes, copy input directly */
    const char* to_write = output ? output : input;
    size_t to_write_len = output ? strlen(output) : read_len;
    
    /* Write output file */
    FILE* out = fopen(h_path, "w");
    if (!out) {
        int err = errno;
        free(input);
        free(output);
        return err ? err : -1;
    }
    
    fwrite(to_write, 1, to_write_len, out);
    fclose(out);
    
    free(input);
    free(output);
    return 0;
}
