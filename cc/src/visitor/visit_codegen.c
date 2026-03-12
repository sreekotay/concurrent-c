#include "visitor.h"
#include "visit_codegen.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>
#include <dlfcn.h>

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

static char* cc__blank_comptime_blocks_preserve_layout(const char* src, size_t n);

/* Helper: reparse source string to AST (in-memory). */
static CCASTRoot* cc__reparse_source_to_ast(const char* src, size_t src_len,
                                            const char* input_path, CCSymbolTable* symbols) {
    char* pp_buf = cc_preprocess_to_string_ex(src, src_len, input_path, 1);
    if (!pp_buf) return NULL;
    size_t pp_len = strlen(pp_buf);
    char* prep = cc__prepend_reparse_prelude(pp_buf, pp_len, &pp_len);
    free(pp_buf);
    if (!prep) return NULL;
    char rel_path[1024];
    cc_path_rel_to_repo(input_path, rel_path, sizeof(rel_path));
    CCASTRoot* root = cc_tcc_bridge_parse_string_to_ast(prep, rel_path, input_path, symbols);
    free(prep);
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

static int cc__find_matching_paren_codegen(const char* src, size_t len, size_t lpar, size_t* out_rpar) {
    int depth = 0, in_str = 0, in_chr = 0, in_lc = 0, in_bc = 0;
    for (size_t i = lpar; i < len; ++i) {
        char c = src[i];
        char c2 = (i + 1 < len) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; } continue; }
        if (in_str) { if (c == '\\' && c2) { i++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && c2) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c == '(') depth++;
        else if (c == ')') {
            depth--;
            if (depth == 0) {
                *out_rpar = i;
                return 1;
            }
        }
    }
    return 0;
}

static int cc__find_matching_brace_codegen(const char* src, size_t len, size_t lbrace, size_t* out_rbrace) {
    int depth = 0, in_str = 0, in_chr = 0, in_lc = 0, in_bc = 0;
    for (size_t i = lbrace; i < len; ++i) {
        char c = src[i];
        char c2 = (i + 1 < len) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; } continue; }
        if (in_str) { if (c == '\\' && c2) { i++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && c2) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) {
                *out_rbrace = i;
                return 1;
            }
        }
    }
    return 0;
}

static size_t cc__skip_ws_codegen(const char* src, size_t n, size_t i) {
    while (i < n && isspace((unsigned char)src[i])) i++;
    return i;
}

static int cc__match_keyword_codegen(const char* src, size_t n, size_t pos, const char* kw) {
    size_t klen = strlen(kw);
    if (pos + klen > n) return 0;
    if (memcmp(src + pos, kw, klen) != 0) return 0;
    if (pos > 0 && (isalnum((unsigned char)src[pos - 1]) || src[pos - 1] == '_')) return 0;
    if (pos + klen < n && (isalnum((unsigned char)src[pos + klen]) || src[pos + klen] == '_')) return 0;
    return 1;
}

static void cc__trim_range_codegen(const char* src, size_t* start, size_t* end) {
    while (*start < *end && isspace((unsigned char)src[*start])) (*start)++;
    while (*end > *start && isspace((unsigned char)src[*end - 1])) (*end)--;
}

static int cc__parse_string_literal_codegen(const char* src, size_t n, size_t* io_pos, char* out, size_t out_sz) {
    size_t i = *io_pos;
    size_t out_len = 0;
    if (i >= n || src[i] != '"') return 0;
    i++;
    while (i < n) {
        char c = src[i++];
        if (c == '"') {
            if (out_sz > 0) out[out_len < out_sz ? out_len : out_sz - 1] = '\0';
            *io_pos = i;
            return 1;
        }
        if (c == '\\' && i < n) {
            char esc = src[i++];
            c = esc;
        }
        if (out_len + 1 < out_sz) out[out_len] = c;
        out_len++;
    }
    return 0;
}

static int cc__parse_ident_codegen(const char* src, size_t n, size_t* io_pos, char* out, size_t out_sz) {
    size_t i = *io_pos;
    size_t len = 0;
    if (i >= n || !(isalpha((unsigned char)src[i]) || src[i] == '_')) return 0;
    while (i < n && (isalnum((unsigned char)src[i]) || src[i] == '_')) {
        if (len + 1 < out_sz) out[len] = src[i];
        len++;
        i++;
    }
    if (out_sz > 0) out[len < out_sz ? len : out_sz - 1] = '\0';
    *io_pos = i;
    return 1;
}

typedef struct {
    void* dl_handle;
    char obj_path[1024];
    char dylib_path[1024];
} CCComptimeDlModule;

/*
 * Named @comptime UFCS handlers currently execute through a small backend
 * boundary:
 *   1. extract a helper translation unit from the original source
 *   2. build a loadable helper module
 *   3. resolve the exported wrapper and store it as a callable registration
 *
 * On macOS we intentionally use the host C toolchain plus a temporary dylib
 * here rather than libtcc in-process relocation. That keeps the current bridge
 * working while the pure libtcc backend remains a separate follow-up.
 */

static void cc__dl_module_free(void* owner) {
    CCComptimeDlModule* module = (CCComptimeDlModule*)owner;
    if (!module) return;
    if (module->dl_handle) dlclose(module->dl_handle);
    if (module->obj_path[0]) unlink(module->obj_path);
    if (module->dylib_path[0]) unlink(module->dylib_path);
    free(module);
}

static void cc__dirname_codegen(const char* path, char* out, size_t out_sz) {
    size_t len = 0;
    const char* slash;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!path || !path[0]) return;
    slash = strrchr(path, '/');
    if (!slash) {
        strncpy(out, ".", out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    len = (size_t)(slash - path);
    if (len == 0) len = 1;
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static int cc__file_exists_codegen(const char* path) {
    return path && path[0] && access(path, F_OK) == 0;
}

static void cc__dirname_inplace_codegen(char* path) {
    size_t len;
    char* slash;
    if (!path) return;
    len = strlen(path);
    while (len > 0 && path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }
    if (len == 0) return;
    slash = strrchr(path, '/');
    if (!slash) {
        strcpy(path, ".");
        return;
    }
    if (slash == path) {
        path[1] = '\0';
        return;
    }
    *slash = '\0';
}

static int cc__find_repo_root_codegen(const char* input_path, char* out, size_t out_sz) {
    char dir[1024];
    if (!input_path || !out || out_sz == 0) return 0;
    cc__dirname_codegen(input_path, dir, sizeof(dir));
    for (int depth = 0; depth < 20 && dir[0]; ++depth) {
        char marker[1200];
        size_t len = strlen(dir);
        snprintf(marker, sizeof(marker), "%s/cc/src/cc_main.c", dir);
        if (cc__file_exists_codegen(marker)) {
            strncpy(out, dir, out_sz - 1);
            out[out_sz - 1] = '\0';
            return 1;
        }
        if (strcmp(dir, "/") == 0 || len == 0) break;
        cc__dirname_inplace_codegen(dir);
    }
    return 0;
}

static int cc__chunk_contains_at_codegen(const char* src, size_t start, size_t end) {
    if (!src || end <= start) return 0;
    for (size_t i = start; i < end; ++i) {
        if (src[i] == '@') return 1;
    }
    return 0;
}

static char* cc__build_comptime_tu_from_preprocessed(const char* src,
                                                     size_t n,
                                                     const char* repo_root,
                                                     const char* extra_defs,
                                                     const char* entry_name,
                                                     const char* callable_name,
                                                     int include_top_level_defs) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    int line_start = 1;
    if (!src || !entry_name || !callable_name) return NULL;
    if (repo_root && repo_root[0]) {
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "#include \"");
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, repo_root);
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "/cc/include/ccc/cc_ufcs.cch\"\n");
    }
    while (i < n) {
        if (line_start && src[i] == '#') {
            size_t line_end = i;
            while (line_end < n && src[line_end] != '\n') line_end++;
            if (strncmp(src + i, "#line", 5) == 0 ||
                strncmp(src + i, "#include <ccc/", strlen("#include <ccc/")) == 0 ||
                strncmp(src + i, "#include \"ccc/", strlen("#include \"ccc/")) == 0) {
                /* Prepend stable absolute stdlib includes above and skip source-local CC includes. */
            } else {
                cc__cg_sb_append(&out, &out_len, &out_cap, src + i, line_end - i);
            }
            cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "\n");
            i = (line_end < n) ? line_end + 1 : line_end;
            line_start = 1;
            continue;
        }
        if (isspace((unsigned char)src[i])) {
            line_start = (src[i] == '\n');
            i++;
            continue;
        }
        if (!include_top_level_defs) {
            while (i < n && src[i] != '\n') i++;
            line_start = 1;
            continue;
        }
        {
            size_t start = i;
            size_t j = i;
            int depth = 0, in_str = 0, in_chr = 0, in_lc = 0, in_bc = 0;
            int saw_top_paren_close = 0;
            for (; j < n; ++j) {
                char c = src[j];
                char c2 = (j + 1 < n) ? src[j + 1] : 0;
                if (in_lc) { if (c == '\n') in_lc = 0; continue; }
                if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; j++; } continue; }
                if (in_str) { if (c == '\\' && c2) { j++; continue; } if (c == '"') in_str = 0; continue; }
                if (in_chr) { if (c == '\\' && c2) { j++; continue; } if (c == '\'') in_chr = 0; continue; }
                if (c == '/' && c2 == '/') { in_lc = 1; j++; continue; }
                if (c == '/' && c2 == '*') { in_bc = 1; j++; continue; }
                if (c == '"') { in_str = 1; continue; }
                if (c == '\'') { in_chr = 1; continue; }
                if (c == '(') { depth++; continue; }
                if (c == ')') {
                    if (depth > 0) depth--;
                    if (depth == 0) saw_top_paren_close = 1;
                    continue;
                }
                if (c == '{' && depth == 0) {
                    size_t body_r = 0;
                    if (!cc__find_matching_brace_codegen(src, n, j, &body_r)) {
                        free(out);
                        return NULL;
                    }
                    if (!saw_top_paren_close) {
                        j = body_r;
                        continue;
                    }
                    if (!cc__chunk_contains_at_codegen(src, start, body_r + 1)) {
                        cc__cg_sb_append(&out, &out_len, &out_cap, src + start, body_r + 1 - start);
                        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "\n");
                    }
                    j = body_r;
                    break;
                }
                if (c == ';' && depth == 0) {
                    if (!cc__chunk_contains_at_codegen(src, start, j + 1)) {
                        cc__cg_sb_append(&out, &out_len, &out_cap, src + start, j + 1 - start);
                        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "\n");
                    }
                    break;
                }
            }
            i = (j < n) ? (j + 1) : j;
            line_start = 1;
        }
    }
    if (extra_defs && extra_defs[0]) {
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "\n");
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, extra_defs);
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "\n");
    }
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "\nCCSlice ");
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, entry_name);
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap,
                          "(CCSlice method, CCSliceArray argv, CCArena *arena) {\n    return ");
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, callable_name);
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "(method, argv, arena);\n}\n");
    return out;
}

static int cc__format_host_comptime_compile_cmd(char* out,
                                                size_t out_sz,
                                                const char* repo_root,
                                                const char* input_dir,
                                                const char* dylib_path,
                                                const char* source_path) {
    if (!out || out_sz == 0 || !dylib_path || !source_path) return -1;
#ifdef __APPLE__
    if (repo_root && repo_root[0]) {
        return snprintf(out, out_sz,
                        "cc -dynamiclib -undefined dynamic_lookup -I\"%s/cc/include\" -I\"%s/out/include\" -o \"%s\" \"%s\"",
                        repo_root, repo_root, dylib_path, source_path) >= (int)out_sz ? -1 : 0;
    }
    if (input_dir && input_dir[0]) {
        return snprintf(out, out_sz,
                        "cc -dynamiclib -undefined dynamic_lookup -I\"%s\" -o \"%s\" \"%s\"",
                        input_dir, dylib_path, source_path) >= (int)out_sz ? -1 : 0;
    }
    return snprintf(out, out_sz,
                    "cc -dynamiclib -undefined dynamic_lookup -o \"%s\" \"%s\"",
                    dylib_path, source_path) >= (int)out_sz ? -1 : 0;
#else
    if (repo_root && repo_root[0]) {
        return snprintf(out, out_sz,
                        "cc -shared -fPIC -I\"%s/cc/include\" -I\"%s/out/include\" -o \"%s\" \"%s\"",
                        repo_root, repo_root, dylib_path, source_path) >= (int)out_sz ? -1 : 0;
    }
    if (input_dir && input_dir[0]) {
        return snprintf(out, out_sz,
                        "cc -shared -fPIC -I\"%s\" -o \"%s\" \"%s\"",
                        input_dir, dylib_path, source_path) >= (int)out_sz ? -1 : 0;
    }
    return snprintf(out, out_sz,
                    "cc -shared -fPIC -o \"%s\" \"%s\"",
                    dylib_path, source_path) >= (int)out_sz ? -1 : 0;
#endif
}

static char* cc__build_lambda_handler_definition_codegen(const char* src,
                                                         size_t n,
                                                         size_t handler_s,
                                                         size_t handler_e,
                                                         const char* lambda_name) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t lpar = 0, rpar = 0, body_s = 0, body_e = handler_e;
    char params[3][64];
    const char* param_types[3] = { "CCSlice", "CCSliceArray", "CCArena *" };
    int param_count = 0;
    if (!src || !lambda_name || handler_s >= handler_e || handler_e > n) return NULL;
    cc__trim_range_codegen(src, &handler_s, &handler_e);
    if (handler_s >= handler_e || src[handler_s] != '(') return NULL;
    lpar = handler_s;
    if (!cc__find_matching_paren_codegen(src, handler_e, lpar, &rpar)) return NULL;
    {
        size_t p = lpar + 1;
        while (p < rpar) {
            p = cc__skip_ws_codegen(src, rpar, p);
            if (p >= rpar) break;
            if (param_count >= 3 || !cc__parse_ident_codegen(src, rpar, &p, params[param_count], sizeof(params[param_count]))) {
                return NULL;
            }
            param_count++;
            p = cc__skip_ws_codegen(src, rpar, p);
            if (p < rpar) {
                if (src[p] != ',') return NULL;
                p++;
            }
        }
    }
    if (param_count != 3) return NULL;
    body_s = cc__skip_ws_codegen(src, handler_e, rpar + 1);
    if (body_s + 1 >= handler_e || src[body_s] != '=' || src[body_s + 1] != '>') return NULL;
    body_s = cc__skip_ws_codegen(src, handler_e, body_s + 2);
    cc__trim_range_codegen(src, &body_s, &body_e);
    if (body_s >= body_e) return NULL;
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "static CCSlice ");
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, lambda_name);
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "(");
    for (int i = 0; i < 3; ++i) {
        if (i) cc__cg_sb_append_cstr(&out, &out_len, &out_cap, ", ");
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, param_types[i]);
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, " ");
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, params[i]);
    }
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, ") ");
    if (src[body_s] == '{') {
        size_t body_r = 0;
        if (!cc__find_matching_brace_codegen(src, handler_e, body_s, &body_r)) {
            free(out);
            return NULL;
        }
        cc__cg_sb_append(&out, &out_len, &out_cap, src + body_s, body_r + 1 - body_s);
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "\n");
        return out;
    }
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "{ return ");
    cc__cg_sb_append(&out, &out_len, &out_cap, src + body_s, body_e - body_s);
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "; }\n");
    return out;
}

static int cc__compile_ufcs_handler_host_backend(const char* input_path,
                                                 const char* original_src,
                                                 size_t original_len,
                                                 const char* symbol_stem,
                                                 const char* callable_name,
                                                 const char* extra_defs,
                                                 int include_top_level_defs,
                                                 void** out_owner,
                                                 const void** out_fn_ptr) {
    char* blanked_src = NULL;
    char* pp_src = NULL;
    char* tu_src = NULL;
    char err_buf[1024] = {0};
    char entry_name[256];
    char input_dir[1024];
    char repo_root[1024];
    char tmp_base[] = "/tmp/cc_comptime_ufcs_XXXXXX";
    char compile_cmd[4096];
    CCComptimeDlModule* module = NULL;
    int rc = -1;
    if (!input_path || !original_src || !symbol_stem || !callable_name || !out_owner || !out_fn_ptr) return -1;
    *out_owner = NULL;
    *out_fn_ptr = NULL;
    repo_root[0] = '\0';
    blanked_src = cc__blank_comptime_blocks_preserve_layout(original_src, original_len);
    if (!blanked_src) goto done;
    pp_src = cc_preprocess_to_string_ex(blanked_src, strlen(blanked_src), input_path, 1);
    if (!pp_src) goto done;
    snprintf(entry_name, sizeof(entry_name), "__cc_comptime_ufcs_%s", symbol_stem);
    tu_src = cc__build_comptime_tu_from_preprocessed(pp_src, strlen(pp_src),
                                                     cc__find_repo_root_codegen(input_path, repo_root, sizeof(repo_root)) ? repo_root : NULL,
                                                     extra_defs, entry_name, callable_name,
                                                     include_top_level_defs);
    if (!tu_src) goto done;
    cc__dirname_codegen(input_path, input_dir, sizeof(input_dir));
    {
        int tmp_fd = mkstemp(tmp_base);
        if (tmp_fd < 0) goto done;
        close(tmp_fd);
    }
    unlink(tmp_base);
    module = (CCComptimeDlModule*)calloc(1, sizeof(*module));
    if (!module) goto done;
    snprintf(module->obj_path, sizeof(module->obj_path), "%s.c", tmp_base);
    snprintf(module->dylib_path, sizeof(module->dylib_path), "%s.dylib", tmp_base);
    {
        FILE* srcf = fopen(module->obj_path, "w");
        if (!srcf) {
            snprintf(err_buf, sizeof(err_buf), "failed to write comptime source");
            goto done;
        }
        fputs(tu_src, srcf);
        fclose(srcf);
    }
    if (cc__format_host_comptime_compile_cmd(compile_cmd, sizeof(compile_cmd),
                                             repo_root[0] ? repo_root : NULL,
                                             input_dir[0] ? input_dir : NULL,
                                             module->dylib_path,
                                             module->obj_path) != 0) {
        snprintf(err_buf, sizeof(err_buf), "failed to format host comptime compile command");
        goto done;
    }
    if (system(compile_cmd) != 0) {
        snprintf(err_buf, sizeof(err_buf), "host comptime compile failed");
        goto done;
    }
    module->dl_handle = dlopen(module->dylib_path, RTLD_NOW | RTLD_LOCAL);
    if (!module->dl_handle) {
        snprintf(err_buf, sizeof(err_buf), "dlopen failed: %s", dlerror() ? dlerror() : "unknown error");
        goto done;
    }
    *out_fn_ptr = dlsym(module->dl_handle, entry_name);
    if (!*out_fn_ptr) {
        snprintf(err_buf, sizeof(err_buf), "dlsym failed: %s", dlerror() ? dlerror() : "missing symbol");
        goto done;
    }
    *out_owner = module;
    module = NULL;
    rc = 0;
done:
    if (tu_src && getenv("CC_DEBUG_COMPTIME_UFCS_DUMP")) {
        FILE* dbg = fopen(getenv("CC_DEBUG_COMPTIME_UFCS_DUMP"), "w");
        if (dbg) {
            fputs(tu_src, dbg);
            fclose(dbg);
        }
    }
    if (rc != 0 && err_buf[0]) {
        fprintf(stderr, "%s: error: comptime UFCS compile failed for '%s': %s\n",
                input_path, symbol_stem, err_buf);
    }
    if (module) cc__dl_module_free(module);
    free(blanked_src);
    free(pp_src);
    free(tu_src);
    return rc;
}

static int cc__compile_named_ufcs_handler(const char* input_path,
                                          const char* original_src,
                                          size_t original_len,
                                          const char* handler_name,
                                          void** out_owner,
                                          const void** out_fn_ptr) {
    return cc__compile_ufcs_handler_host_backend(input_path, original_src, original_len,
                                                 handler_name, handler_name, NULL, 1,
                                                 out_owner, out_fn_ptr);
}

static int cc__compile_lambda_ufcs_handler(const char* input_path,
                                           const char* original_src,
                                           size_t original_len,
                                           size_t handler_s,
                                           size_t handler_e,
                                           void** out_owner,
                                           const void** out_fn_ptr) {
    char lambda_name[128];
    char* lambda_def = NULL;
    int rc = -1;
    snprintf(lambda_name, sizeof(lambda_name), "__cc_ufcs_lambda_%zu", handler_s);
    lambda_def = cc__build_lambda_handler_definition_codegen(original_src, original_len,
                                                            handler_s, handler_e, lambda_name);
    if (!lambda_def) return -1;
    rc = cc__compile_ufcs_handler_host_backend(input_path, original_src, original_len,
                                               lambda_name, lambda_name, lambda_def, 0,
                                               out_owner, out_fn_ptr);
    free(lambda_def);
    return rc;
}

static int cc__collect_comptime_ufcs_registrations(CCSymbolTable* symbols,
                                                   const char* input_path,
                                                   const char* src,
                                                   size_t n) {
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    if (!symbols || !src) return 0;
    for (size_t i = 0; i < n; ++i) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; } continue; }
        if (in_str) { if (c == '\\' && c2) { i++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && c2) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c != '@' || !cc__match_keyword_codegen(src, n, i + 1, "comptime")) continue;
        {
            size_t kw_end = i + 1 + strlen("comptime");
            size_t body_l = cc__skip_ws_codegen(src, n, kw_end);
            size_t body_r;
            if (body_l >= n || src[body_l] != '{') continue;
            if (!cc__find_matching_brace_codegen(src, n, body_l, &body_r)) continue;
            for (size_t j = body_l + 1; j < body_r; ++j) {
                if (!cc__match_keyword_codegen(src, body_r, j, "cc_ufcs_register")) continue;
                size_t lpar = cc__skip_ws_codegen(src, body_r, j + strlen("cc_ufcs_register"));
                size_t rpar, p;
                char pattern[128];
                char handler[128];
                size_t handler_s = 0, handler_e = 0;
                int handler_is_ident = 0;
                if (lpar >= body_r || src[lpar] != '(') continue;
                if (!cc__find_matching_paren_codegen(src, body_r, lpar, &rpar)) continue;
                p = cc__skip_ws_codegen(src, body_r, lpar + 1);
                if (!cc__parse_string_literal_codegen(src, body_r, &p, pattern, sizeof(pattern))) {
                    fprintf(stderr, "%s: error: unsupported @comptime cc_ufcs_register pattern form\n",
                            input_path ? input_path : "<input>");
                    return -1;
                }
                p = cc__skip_ws_codegen(src, body_r, p);
                if (p >= body_r || src[p] != ',') {
                    fprintf(stderr, "%s: error: malformed cc_ufcs_register(...) in @comptime block\n",
                            input_path ? input_path : "<input>");
                    return -1;
                }
                p = cc__skip_ws_codegen(src, body_r, p + 1);
                handler_s = p;
                if (cc__parse_ident_codegen(src, body_r, &p, handler, sizeof(handler))) {
                    handler_is_ident = 1;
                    handler_e = p;
                } else {
                    handler_e = rpar;
                }
                if (handler_is_ident) {
                    void* owner = NULL;
                    const void* fn_ptr = NULL;
                    if (cc__compile_named_ufcs_handler(input_path, src, n, handler, &owner, &fn_ptr) != 0) {
                        fprintf(stderr, "%s: error: unsupported comptime UFCS handler '%s'; expected a plain named function compilable in the comptime subset\n",
                                input_path ? input_path : "<input>", handler);
                        return -1;
                    }
                    if (cc_symbols_add_ufcs_callable(symbols, pattern, fn_ptr, owner, cc__dl_module_free) != 0) {
                        fprintf(stderr, "%s: error: failed to record callable UFCS registration for '%s'\n",
                                input_path ? input_path : "<input>", pattern);
                        cc__dl_module_free(owner);
                        return -1;
                    }
                } else {
                    void* owner = NULL;
                    const void* fn_ptr = NULL;
                    cc__trim_range_codegen(src, &handler_s, &handler_e);
                    if (cc__compile_lambda_ufcs_handler(input_path, src, n, handler_s, handler_e, &owner, &fn_ptr) != 0) {
                        fprintf(stderr, "%s: error: unsupported comptime UFCS lambda; expected a non-capturing lambda compilable in the comptime subset\n",
                                input_path ? input_path : "<input>");
                        return -1;
                    }
                    if (cc_symbols_add_ufcs_callable(symbols, pattern, fn_ptr, owner, cc__dl_module_free) != 0) {
                        fprintf(stderr, "%s: error: failed to record callable UFCS registration for '%s'\n",
                                input_path ? input_path : "<input>", pattern);
                        cc__dl_module_free(owner);
                        return -1;
                    }
                }
                j = rpar;
            }
            i = body_r;
        }
    }
    return 0;
}

static char* cc__blank_comptime_blocks_preserve_layout(const char* src, size_t n) {
    char* out = NULL;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    if (!src) return NULL;
    out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n);
    out[n] = '\0';
    for (size_t i = 0; i < n; ++i) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; } continue; }
        if (in_str) { if (c == '\\' && c2) { i++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && c2) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c != '@' || !cc__match_keyword_codegen(src, n, i + 1, "comptime")) continue;
        {
            size_t kw_end = i + 1 + strlen("comptime");
            size_t body_l = cc__skip_ws_codegen(src, n, kw_end);
            size_t body_r;
            if (body_l >= n || src[body_l] != '{') continue;
            if (!cc__find_matching_brace_codegen(src, n, body_l, &body_r)) continue;
            for (size_t k = i; k <= body_r; ++k) {
                if (out[k] != '\n') out[k] = ' ';
            }
            i = body_r;
        }
    }
    return out;
}

static void cc__collect_registered_ufcs_var_types(CCSymbolTable* symbols, const char* src, size_t n) {
    CCTypeRegistry* reg = cc_type_registry_get_global();
    if (!symbols || !reg || !src) return;
    for (size_t ui = 0; ui < cc_symbols_ufcs_count(symbols); ++ui) {
        int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
        const char* type_name = cc_symbols_ufcs_pattern(symbols, ui);
        size_t type_len = type_name ? strlen(type_name) : 0;
        if (!type_len) continue;
        for (size_t i = 0; i < n; ++i) {
            char c = src[i];
            char c2 = (i + 1 < n) ? src[i + 1] : 0;
            if (in_lc) { if (c == '\n') in_lc = 0; continue; }
            if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; } continue; }
            if (in_str) { if (c == '\\' && c2) { i++; continue; } if (c == '"') in_str = 0; continue; }
            if (in_chr) { if (c == '\\' && c2) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
            if (c == '/' && c2 == '/') { in_lc = 1; i++; continue; }
            if (c == '/' && c2 == '*') { in_bc = 1; i++; continue; }
            if (c == '"') { in_str = 1; continue; }
            if (c == '\'') { in_chr = 1; continue; }
            if (!cc__match_keyword_codegen(src, n, i, type_name)) continue;
            {
                size_t p = cc__skip_ws_codegen(src, n, i + type_len);
                while (p < n && src[p] == '*') p++;
                p = cc__skip_ws_codegen(src, n, p);
                if (p < n && (isalpha((unsigned char)src[p]) || src[p] == '_')) {
                    char var_name[128];
                    size_t v = p;
                    size_t vn = 0;
                    while (v < n && (isalnum((unsigned char)src[v]) || src[v] == '_')) {
                        if (vn + 1 < sizeof(var_name)) var_name[vn] = src[v];
                        vn++;
                        v++;
                    }
                    var_name[vn < sizeof(var_name) ? vn : sizeof(var_name) - 1] = '\0';
                    v = cc__skip_ws_codegen(src, n, v);
                    if (v < n && src[v] == '(') continue; /* function decl, not variable */
                    cc_type_registry_add_var(reg, var_name, type_name);
                }
            }
            i += type_len ? (type_len - 1) : 0;
        }
    }
}

/* Rewrite @closing(ch) spawn(...) or @closing(ch) { ... } syntax.
   Transforms into explicit nested `@nursery closing(...)` form. */
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
                cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "@nursery closing(");
                cc__cg_sb_append(&out, &out_len, &out_cap, src + chans_start, chans_end - chans_start);
                cc__cg_sb_append_cstr(&out, &out_len, &out_cap, ") ");
                
                if (is_block) {
                    /* Re-run rewrite recursively to handle nested @closing(...) */
                    size_t blen = body_end - body_start;
                    char* inner = cc__rewrite_closing_annotation(src + body_start, blen);
                    if (inner) {
                        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, inner);
                        free(inner);
                    } else {
                        cc__cg_sb_append(&out, &out_len, &out_cap, src + body_start, blen);
                    }
                } else {
                    /* Single-spawn form with recursive rewrite support. */
                    size_t blen = body_end - body_start;
                    char* inner = cc__rewrite_closing_annotation(src + body_start, blen);
                    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "{\n");
                    if (inner) {
                        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, inner);
                        free(inner);
                    } else {
                        cc__cg_sb_append(&out, &out_len, &out_cap, src + body_start, blen);
                    }
                    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "\n}");
                }
                
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

    /* Collect compile-time UFCS registrations from `@comptime { ... }` blocks,
       then blank those blocks out of the emitted TU while preserving layout so
       earlier AST spans remain valid. */
    if (src_ufcs && src_ufcs_len && ctx && ctx->symbols) {
        if (cc__collect_comptime_ufcs_registrations(ctx->symbols, ctx->input_path, src_ufcs, src_ufcs_len) != 0) {
            fclose(out);
            free(src_all);
            return EINVAL;
        }
        if (getenv("CC_DEBUG_COMPTIME_UFCS")) {
            fprintf(stderr, "CC_DEBUG_COMPTIME_UFCS: collected %zu registration(s)\n",
                    cc_symbols_ufcs_count(ctx->symbols));
            for (size_t ui = 0; ui < cc_symbols_ufcs_count(ctx->symbols); ++ui) {
                const char* pat = cc_symbols_ufcs_pattern(ctx->symbols, ui);
                fprintf(stderr, "  pattern[%zu] = %s\n", ui, pat ? pat : "<null>");
            }
        }
        char* blanked = cc__blank_comptime_blocks_preserve_layout(src_ufcs, src_ufcs_len);
        if (blanked) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = blanked;
            src_ufcs_len = src_len;
        }
    }

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

    /* General UFCS lowering now goes through the AST-aware UFCS pass below.
       Synthetic stdio receivers are the remaining exception because they do not
       correspond to ordinary typed receivers. */
    if (src_ufcs && src_ufcs_len) {
        char* rewritten = cc_rewrite_synthetic_std_io_receivers(src_ufcs, src_ufcs_len);
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

    /* UNIFIED Phase 3: Collect all AST-driven edits (UFCS, closure_calls, autoblock, await_normalize)
       via CCEditBuffer, apply once. Replaces 4 sequential rewrite passes. */
#ifdef CC_TCC_EXT_AVAILABLE
    if (src_ufcs && root && root->nodes && root->node_count > 0 && ctx->symbols) {
        cc__collect_registered_ufcs_var_types(ctx->symbols, src_ufcs, src_ufcs_len);
        CCEditBuffer eb;
        cc_edit_buffer_init(&eb, src_ufcs, src_ufcs_len);
        
        /* Collect edits from all Phase 3 passes */
        cc__collect_ufcs_edits(root, ctx, &eb);
        cc__collect_closure_calls_edits(root, ctx, &eb);
        cc__collect_autoblocking_edits(root, ctx, &eb);
        cc__collect_await_normalize_edits(root, ctx, &eb);
        
        /* Apply all edits at once if any were collected */
        if (eb.count > 0) {
            size_t new_len = 0;
            char* rewritten = cc_edit_buffer_apply(&eb, &new_len);
            if (rewritten) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = new_len;
            }
        }
        cc_edit_buffer_free(&eb);
        
        /* Debug output for await rewrite */
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

    /* Text-based rewrites that must happen BEFORE creating root3 AST, so AST positions
       match the transformed source. These are text-based and don't need AST. */
    if (src_ufcs && ctx) {
        /* Lower `channel_pair(&tx, &rx);` BEFORE channel type rewrite (it needs `[~]` patterns). */
        {
            size_t rp_len = 0;
            char* rp = cc__rewrite_channel_pair_calls_text(ctx, src_ufcs, src_ufcs_len, &rp_len);
            if (!rp) {
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(src_all);
                return EINVAL;
            }
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rp;
            src_ufcs_len = rp_len;
        }

        /* Rewrite chan_send_task(ch, closure) BEFORE channel type rewrite.
           This wraps the closure body to store result in fiber-local storage. */
        {
            size_t st_len = 0;
            char* st = cc__rewrite_chan_send_task_text(ctx, src_ufcs, src_ufcs_len, &st_len);
            if (st) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = st;
                src_ufcs_len = st_len;
            }
        }

        /* Rewrite channel handle types (including owned channels) BEFORE creating AST.
           This transforms `int[~4 >]` -> `CCChanTx`, and `T[~N owned {...}]` -> `cc_chan_create_owned(...)`.
           T[~N ordered <] becomes CCChanRx (ordered is a flag on the channel).
           Must happen before AST creation so closure positions are correct. */
        {
            char* rew = cc__rewrite_chan_handle_types_text(ctx, src_ufcs, src_ufcs_len);
            if (rew) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew;
                src_ufcs_len = strlen(src_ufcs);
            }
        }
    }

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

        /* 2+3+4) Batch spawn + nursery + arena using EditBuffer so we don't have stale AST offsets.
           All passes use root4; without batching, each pass changes src_ufcs but subsequent
           passes still reference root4's offsets (which would be wrong).
           ELIMINATED ONE REPARSE by using cc__collect_spawn_edits instead of whole-file rewrite. */
        {
            CCEditBuffer eb;
            cc_edit_buffer_init(&eb, src_ufcs, src_ufcs_len);

            int n_spawn = cc__collect_spawn_edits(root4, ctx, &eb);
            int n_nursery = cc__collect_nursery_edits(root4, ctx, &eb);
            int n_arena = cc__collect_arena_edits(root4, ctx, &eb);

            if (n_spawn < 0 || n_nursery < 0 || n_arena < 0) {
                cc_tcc_bridge_free_ast(root4);
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
        cc_tcc_bridge_free_ast(root4);
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
    /* Include lowered headers (.h) - these are generated from .cch files
       with CC type syntax transformed to valid C. Lowered headers live in
       out/include/ which should be added to include path before cc/include/. */
    fprintf(out, "#include <ccc/cc_nursery.h>\n");
    fprintf(out, "#include <ccc/cc_closure.h>\n");
    fprintf(out, "#include <ccc/cc_slice.h>\n");
    fprintf(out, "#include <ccc/cc_runtime.h>\n");
    fprintf(out, "#include <ccc/std/io.h>\n");  /* CCFile for closure captures */
    fprintf(out, "#include <ccc/std/task.h>\n");
    /* Helper alias: used for auto-blocking arg binding to avoid accidental hoisting of these temps. */
    fprintf(out, "typedef intptr_t CCAbIntptr;\n");
    
    /* TSan macros and spawn helpers */
    fprintf(out, "#include <ccc/cc_closure_helper.h>\n\n");

    /* Emit container type declarations from type registry (populated by generic rewriting) */
    {
        CCTypeRegistry* reg = cc_type_registry_get_global();
        if (reg) {
            size_t n_vec = cc_type_registry_vec_count(reg);
            size_t n_map = cc_type_registry_map_count(reg);
            size_t n_chan = cc_type_registry_channel_count(reg);
            
            if (n_vec > 0 || n_map > 0 || n_chan > 0) {
                fprintf(out, "/* --- CC generic container declarations --- */\n");
                fprintf(out, "#include <ccc/std/vec.h>\n");
                fprintf(out, "#include <ccc/std/map.h>\n");
                fprintf(out, "#include <ccc/cc_channel.cch>\n");
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

                for (size_t i = 0; i < n_chan; i++) {
                    const CCTypeInstantiation* inst = cc_type_registry_get_channel(reg, i);
                    if (inst && inst->type1 && inst->mangled_name) {
                        fprintf(out, "typedef CCChanTx CCChanTx_%s;\n", inst->mangled_name);
                        fprintf(out, "typedef CCChanRx CCChanRx_%s;\n", inst->mangled_name);
                        fprintf(out, "#define CCChanTx_%s_send(tx, value) CC_TYPED_CHAN_SEND((tx), (value))\n", inst->mangled_name);
                        fprintf(out, "#define CCChanRx_%s_recv(rx, out_ptr) CC_TYPED_CHAN_RECV((rx), (out_ptr))\n", inst->mangled_name);
                        fprintf(out, "#define CCChanTx_%s_try_send(tx, value) CC_TYPED_CHAN_TRY_SEND((tx), (value))\n", inst->mangled_name);
                        fprintf(out, "#define CCChanRx_%s_try_recv(rx, out_ptr) CC_TYPED_CHAN_TRY_RECV((rx), (out_ptr))\n", inst->mangled_name);
                        fprintf(out, "#define CCChanTx_%s_close(tx) CC_TYPED_CHAN_CLOSE((tx))\n", inst->mangled_name);
                        fprintf(out, "#define CCChanRx_%s_close(rx) CC_TYPED_CHAN_CLOSE((rx))\n", inst->mangled_name);
                        fprintf(out, "#define CCChanTx_%s_free(tx) CC_TYPED_CHAN_FREE((tx))\n", inst->mangled_name);
                        fprintf(out, "#define CCChanRx_%s_free(rx) CC_TYPED_CHAN_FREE((rx))\n", inst->mangled_name);
                    }
                }
                
                fprintf(out, "#endif /* !CC_PARSER_MODE */\n");
                fprintf(out, "/* --- end container declarations --- */\n\n");
            }
        }
    }

    /* Result type declarations are emitted later, after they're collected during
       cc__rewrite_result_types_text processing. */

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
        /* Rewrite chan_send_task(ch, closure) */
        {
            size_t st_len = 0;
            char* st = cc__rewrite_chan_send_task_text(ctx, src_ufcs, src_ufcs_len, &st_len);
            if (st) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = st;
                src_ufcs_len = st_len;
            }
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
        /* Reset type registries once before the type-rewriting passes so they
           accumulate correctly across the optional and result scans below.
           Previously each scan function reset on entry, losing types collected
           by earlier calls in the same compilation unit. */
        cc__cg_reset_type_registries();
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
            
            /* Result type declarations are now emitted at file scope in the header section,
               so no need to splice them into source here. */
        }
        /* Result field sugar:
           `res.value` / `res.error` -> `res.u.value` / `res.u.error`
           while keeping the compact union ABI in generated C. */
        {
            char* rew_res_fields = cc__rewrite_result_field_sugar_text(ctx, src_ufcs, src_ufcs_len);
            if (rew_res_fields) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_res_fields;
                src_ufcs_len = strlen(src_ufcs);
            }
        }
        /* Insert optional type declarations for custom types.
           Each CC_DECL_OPTIONAL is inserted right before the first use of that specific
           optional type, to ensure the underlying type is defined by then. */
        if (cc__cg_optional_type_count > 0) {
            /* Sort types by their first usage position (descending) so we can insert
               from end to start without invalidating positions */
            size_t* type_positions = (size_t*)malloc(cc__cg_optional_type_count * sizeof(size_t));
            size_t* sorted_indices_buf = (size_t*)malloc(cc__cg_optional_type_count * sizeof(size_t));
            if (!type_positions || !sorted_indices_buf) {
                free(type_positions);
                free(sorted_indices_buf);
                goto skip_optional_decls;
            }
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
            size_t* sorted_indices = sorted_indices_buf;
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
            free(type_positions);
            free(sorted_indices_buf);
        }
        skip_optional_decls:;
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

        /* Final UFCS sweep: earlier statement/syntax rewrites can synthesize new
           method-call surface syntax (notably via @defer / spawn / nursery
           lowering). Reparse the current source and lower any remaining UFCS
           spans before emitting C. */
        if (ctx && ctx->symbols) {
            CCASTRoot* final_ufcs_root = cc__reparse_source_to_ast(src_ufcs, src_ufcs_len, ctx->input_path, ctx->symbols);
            if (!final_ufcs_root) {
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(closure_protos);
                free(closure_defs);
                return EINVAL;
            }

            cc__collect_registered_ufcs_var_types(ctx->symbols, src_ufcs, src_ufcs_len);
            CCEditBuffer eb;
            cc_edit_buffer_init(&eb, src_ufcs, src_ufcs_len);
            cc__collect_ufcs_edits(final_ufcs_root, ctx, &eb);
            if (eb.count > 0) {
                size_t new_len = 0;
                char* rewritten = cc_edit_buffer_apply(&eb, &new_len);
                if (rewritten) {
                    if (src_ufcs != src_all) free(src_ufcs);
                    src_ufcs = rewritten;
                    src_ufcs_len = new_len;
                }
            }
            cc_edit_buffer_free(&eb);
            cc_tcc_bridge_free_ast(final_ufcs_root);
        }
        
        /* Insert result type declarations INTO the source at the right position.
           They must come AFTER custom type definitions but BEFORE functions that use them.
           Find the first CCResult_ usage and insert before that line (at file scope). */
        if (cc__cg_result_type_count > 0) {
            /* Find first usage of any CCResult_T_E type in the source */
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
                /* Back up to start of line */
                while (earliest_pos > 0 && src_ufcs[earliest_pos - 1] != '\n') {
                    earliest_pos--;
                }
                
                /* Build declaration string */
                char* decls = NULL;
                size_t decls_len = 0, decls_cap = 0;
                cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap,
                    "/* --- CC result type declarations (auto-generated) --- */\n");
                cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap,
                    "#ifndef CC_PARSER_MODE\n");
                for (size_t ri = 0; ri < cc__cg_result_type_count; ri++) {
                    CCCodegenResultTypePair* p = &cc__cg_result_types[ri];
                    char line[512];
                    /* Use guards to avoid conflicts with pre-existing CC_DECL_RESULT_SPEC in headers */
                    snprintf(line, sizeof(line), "#ifndef CCResult_%s_%s_DEFINED\n"
                             "#define CCResult_%s_%s_DEFINED 1\n"
                             "CC_DECL_RESULT_SPEC(CCResult_%s_%s, %s, %s)\n"
                             "#endif\n",
                            p->mangled_ok, p->mangled_err,
                            p->mangled_ok, p->mangled_err,
                            p->mangled_ok, p->mangled_err, p->ok_type, p->err_type);
                    cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap, line);
                }
                cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap,
                    "#endif /* !CC_PARSER_MODE */\n");
                cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap,
                    "/* --- end result type declarations --- */\n\n");
                
                /* Build new source: prefix + decls + suffix */
                char* new_src = NULL;
                size_t new_len = 0, new_cap = 0;
                cc__sb_append_local(&new_src, &new_len, &new_cap, src_ufcs, earliest_pos);
                cc__sb_append_local(&new_src, &new_len, &new_cap, decls, decls_len);
                cc__sb_append_local(&new_src, &new_len, &new_cap,
                                    src_ufcs + earliest_pos, src_ufcs_len - earliest_pos);
                
                free(decls);
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = new_src;
                src_ufcs_len = new_len;
            }
        }
        
        fwrite(src_ufcs, 1, src_ufcs_len, out);
        if (src_ufcs_len == 0 || src_ufcs[src_ufcs_len - 1] != '\n') fputc('\n', out);

        free(closure_protos);
        if (closure_defs && closure_defs_len > 0) {
            /* Run @defer lowering on closure definitions too (handles @defer inside spawn closures) */
            char* closure_defs_lowered = NULL;
            size_t closure_defs_lowered_len = 0;
            if (cc__rewrite_defer_syntax(ctx, closure_defs, closure_defs_len, 
                                          &closure_defs_lowered, &closure_defs_lowered_len) > 0) {
                free(closure_defs);
                closure_defs = closure_defs_lowered;
                closure_defs_len = closure_defs_lowered_len;
            }

            /* Closure bodies can preserve raw UFCS after closure lowering.
               Run the narrow generated-code survival rewrites here so helper
               definitions don't leak raw method syntax into emitted C. */
            {
                char* rewritten = cc_rewrite_file_ufcs_survival(closure_defs, closure_defs_len);
                if (rewritten) {
                    free(closure_defs);
                    closure_defs = rewritten;
                    closure_defs_len = strlen(rewritten);
                }
            }
            {
                char* rewritten = cc_rewrite_result_ufcs_survival(closure_defs, closure_defs_len);
                if (rewritten) {
                    free(closure_defs);
                    closure_defs = rewritten;
                    closure_defs_len = strlen(rewritten);
                }
            }
            
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

