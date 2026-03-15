#include "hook_compile.h"

#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "preprocess/preprocess.h"
#include "util/path.h"
#include "visitor/visitor_fileutil.h"

typedef struct {
    void* dl_handle;
    char obj_path[1024];
    char dylib_path[1024];
} CCComptimeDlModule;

static void cc__hc_sb_append(char** out, size_t* out_len, size_t* out_cap, const char* s, size_t len) {
    char* nv;
    size_t needed;
    if (!out || !out_len || !out_cap || !s || len == 0) return;
    needed = *out_len + len + 1;
    if (needed > *out_cap) {
        size_t new_cap = *out_cap ? *out_cap * 2 : 256;
        while (new_cap < needed) new_cap *= 2;
        nv = (char*)realloc(*out, new_cap);
        if (!nv) return;
        *out = nv;
        *out_cap = new_cap;
    }
    memcpy(*out + *out_len, s, len);
    *out_len += len;
    (*out)[*out_len] = '\0';
}

static void cc__hc_sb_append_cstr(char** out, size_t* out_len, size_t* out_cap, const char* s) {
    cc__hc_sb_append(out, out_len, out_cap, s, s ? strlen(s) : 0);
}

static size_t cc__skip_ws(const char* src, size_t n, size_t i) {
    while (i < n && isspace((unsigned char)src[i])) i++;
    return i;
}

static void cc__trim_range(const char* src, size_t* start, size_t* end) {
    while (*start < *end && isspace((unsigned char)src[*start])) (*start)++;
    while (*end > *start && isspace((unsigned char)src[*end - 1])) (*end)--;
}

static int cc__parse_ident(const char* src, size_t n, size_t* io_pos, char* out, size_t out_sz) {
    size_t i = *io_pos;
    size_t len = 0;
    if (i >= n || !(isalpha((unsigned char)src[i]) || src[i] == '_')) return 0;
    while (i < n && (isalnum((unsigned char)src[i]) || src[i] == '_')) {
        if (len + 1 < out_sz) out[len] = src[i];
        len++;
        i++;
    }
    if (len >= out_sz) len = out_sz - 1;
    out[len] = '\0';
    *io_pos = i;
    return len > 0;
}

static int cc__find_matching_paren(const char* src, size_t len, size_t lpar, size_t* out_rpar) {
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

static int cc__find_matching_brace(const char* src, size_t len, size_t lbrace, size_t* out_rbrace) {
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

static int cc__match_keyword(const char* src, size_t n, size_t pos, const char* kw) {
    size_t klen = strlen(kw);
    if (!src || !kw || pos + klen > n) return 0;
    if (memcmp(src + pos, kw, klen) != 0) return 0;
    if (pos > 0 && (isalnum((unsigned char)src[pos - 1]) || src[pos - 1] == '_')) return 0;
    if (pos + klen < n && (isalnum((unsigned char)src[pos + klen]) || src[pos + klen] == '_')) return 0;
    return 1;
}

static int cc__chunk_contains_at(const char* src, size_t start, size_t end) {
    if (!src || end <= start) return 0;
    for (size_t i = start; i < end; ++i) {
        if (src[i] == '@') return 1;
    }
    return 0;
}

static char* cc__blank_comptime_blocks_preserve_layout(const char* src, size_t n) {
    char* out;
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
        if (c != '@' || !cc__match_keyword(src, n, i + 1, "comptime")) continue;
        {
            size_t kw_end = i + 1 + strlen("comptime");
            size_t body_l = cc__skip_ws(src, n, kw_end);
            size_t body_r;
            if (body_l >= n || src[body_l] != '{') continue;
            if (!cc__find_matching_brace(src, n, body_l, &body_r)) continue;
            for (size_t j = i; j <= body_r; ++j) {
                if (out[j] != '\n' && out[j] != '\r') out[j] = ' ';
            }
            i = body_r;
        }
    }
    return out;
}

static void cc__dirname(const char* path, char* out, size_t out_sz) {
    size_t len = 0;
    const char* slash;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!path) return;
    slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, out_sz, ".");
        return;
    }
    len = (size_t)(slash - path);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static int cc__find_repo_root(const char* input_path, char* out, size_t out_sz) {
    if (!input_path || !out || out_sz == 0) return 0;
    return cc_path_find_repo_root(input_path, out, out_sz);
}

void cc_comptime_type_hook_owner_free(void* owner) {
    CCComptimeDlModule* module = (CCComptimeDlModule*)owner;
    if (!module) return;
    if (module->dl_handle) dlclose(module->dl_handle);
    if (module->obj_path[0]) unlink(module->obj_path);
    if (module->dylib_path[0]) unlink(module->dylib_path);
    free(module);
}

static char* cc__build_wrapper_tu(const char* src,
                                  size_t n,
                                  const char* repo_root,
                                  const char* extra_defs,
                                  const char* entry_name,
                                  const char* callable_name,
                                  int include_top_level_defs,
                                  CCComptimeTypeHookKind kind) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    int line_start = 1;
    if (!src || !entry_name || !callable_name) return NULL;
    cc__hc_sb_append_cstr(&out, &out_len, &out_cap, "#ifndef __CC__\n#define __CC__ 1\n#endif\n");
    (void)repo_root;
    while (i < n) {
        if (line_start && src[i] == '#') {
            size_t line_end = i;
            while (line_end < n && src[line_end] != '\n') line_end++;
            if (strncmp(src + i, "#line", 5) != 0) {
                cc__hc_sb_append(&out, &out_len, &out_cap, src + i, line_end - i);
                cc__hc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
            }
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
                if (c == ')') { if (depth > 0) depth--; if (depth == 0) saw_top_paren_close = 1; continue; }
                if (c == '{' && depth == 0) {
                    size_t body_r = 0;
                    if (!cc__find_matching_brace(src, n, j, &body_r)) { free(out); return NULL; }
                    if (!saw_top_paren_close) {
                        j = body_r;
                        continue;
                    }
                    if (!cc__chunk_contains_at(src, start, body_r + 1)) {
                        cc__hc_sb_append(&out, &out_len, &out_cap, src + start, body_r + 1 - start);
                        cc__hc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
                    }
                    j = body_r;
                    break;
                }
                if (c == ';' && depth == 0) {
                    if (!cc__chunk_contains_at(src, start, j + 1)) {
                        cc__hc_sb_append(&out, &out_len, &out_cap, src + start, j + 1 - start);
                        cc__hc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
                    }
                    break;
                }
            }
            i = (j < n) ? (j + 1) : j;
            line_start = 1;
        }
    }
    if (extra_defs && extra_defs[0]) {
        cc__hc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
        cc__hc_sb_append_cstr(&out, &out_len, &out_cap, extra_defs);
        cc__hc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
    }
    cc__hc_sb_append_cstr(&out, &out_len, &out_cap, "\nCCSlice ");
    cc__hc_sb_append_cstr(&out, &out_len, &out_cap, entry_name);
    if (kind == CC_COMPTIME_TYPE_HOOK_UFCS) {
        cc__hc_sb_append_cstr(&out, &out_len, &out_cap,
                              "(CCSlice recv_type, CCSlice method, CCSlice mode, CCSliceArray argv, CCSliceArray arg_types, CCArena *arena) {\n    return ");
        cc__hc_sb_append_cstr(&out, &out_len, &out_cap, callable_name);
        cc__hc_sb_append_cstr(&out, &out_len, &out_cap, "(recv_type, method, mode, argv, arg_types, arena);\n}\n");
    } else {
        cc__hc_sb_append_cstr(&out, &out_len, &out_cap,
                              "(CCSlice type_name, CCSliceArray argv, CCSliceArray arg_types, CCArena *arena) {\n    return ");
        cc__hc_sb_append_cstr(&out, &out_len, &out_cap, callable_name);
        cc__hc_sb_append_cstr(&out, &out_len, &out_cap, "(type_name, argv, arg_types, arena);\n}\n");
    }
    return out;
}

static int cc__format_compile_cmd(char* out, size_t out_sz, const char* repo_root, const char* input_dir, const char* dylib_path, const char* source_path) {
#ifdef __APPLE__
    if (repo_root && repo_root[0]) {
        return snprintf(out, out_sz,
                        "cc -dynamiclib -undefined dynamic_lookup -I\"%s/cc/include\" -I\"%s/out/include\" -o \"%s\" \"%s\"",
                        repo_root, repo_root, dylib_path, source_path) >= (int)out_sz ? -1 : 0;
    }
    return snprintf(out, out_sz,
                    "cc -dynamiclib -undefined dynamic_lookup -I\"%s\" -o \"%s\" \"%s\"",
                    input_dir ? input_dir : ".", dylib_path, source_path) >= (int)out_sz ? -1 : 0;
#else
    if (repo_root && repo_root[0]) {
        return snprintf(out, out_sz,
                        "cc -shared -fPIC -I\"%s/cc/include\" -I\"%s/out/include\" -o \"%s\" \"%s\"",
                        repo_root, repo_root, dylib_path, source_path) >= (int)out_sz ? -1 : 0;
    }
    return snprintf(out, out_sz,
                    "cc -shared -fPIC -I\"%s\" -o \"%s\" \"%s\"",
                    input_dir ? input_dir : ".", dylib_path, source_path) >= (int)out_sz ? -1 : 0;
#endif
}

static char* cc__build_lambda_definition(const char* expr_src, size_t expr_len, const char* lambda_name, CCComptimeTypeHookKind kind) {
    const char* param_types_ufcs[6] = { "CCSlice", "CCSlice", "CCSlice", "CCSliceArray", "CCSliceArray", "CCArena *" };
    const char* param_types_create[4] = { "CCSlice", "CCSliceArray", "CCSliceArray", "CCArena *" };
    const char** param_types = (kind == CC_COMPTIME_TYPE_HOOK_UFCS) ? param_types_ufcs : param_types_create;
    int expected_params = (kind == CC_COMPTIME_TYPE_HOOK_UFCS) ? 6 : 4;
    char params[6][64];
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t lpar = 0, rpar = 0, p = 0, body_s = 0, body_e = expr_len;
    int param_count = 0;
    if (!expr_src || !lambda_name || expr_len == 0) return NULL;
    p = cc__skip_ws(expr_src, expr_len, 0);
    if (p >= expr_len || expr_src[p] != '(') return NULL;
    lpar = p;
    if (!cc__find_matching_paren(expr_src, expr_len, lpar, &rpar)) return NULL;
    p = lpar + 1;
    while (p < rpar) {
        p = cc__skip_ws(expr_src, rpar, p);
        if (p >= rpar) break;
        if (param_count >= expected_params || !cc__parse_ident(expr_src, rpar, &p, params[param_count], sizeof(params[param_count]))) return NULL;
        param_count++;
        p = cc__skip_ws(expr_src, rpar, p);
        if (p < rpar) {
            if (expr_src[p] != ',') return NULL;
            p++;
        }
    }
    if (param_count != expected_params) return NULL;
    body_s = cc__skip_ws(expr_src, expr_len, rpar + 1);
    if (body_s + 1 >= expr_len || expr_src[body_s] != '=' || expr_src[body_s + 1] != '>') return NULL;
    body_s = cc__skip_ws(expr_src, expr_len, body_s + 2);
    cc__trim_range(expr_src, &body_s, &body_e);
    if (body_s >= body_e) return NULL;
    cc__hc_sb_append_cstr(&out, &out_len, &out_cap, "static CCSlice ");
    cc__hc_sb_append_cstr(&out, &out_len, &out_cap, lambda_name);
    cc__hc_sb_append_cstr(&out, &out_len, &out_cap, "(");
    for (int i = 0; i < expected_params; ++i) {
        if (i) cc__hc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
        cc__hc_sb_append_cstr(&out, &out_len, &out_cap, param_types[i]);
        cc__hc_sb_append_cstr(&out, &out_len, &out_cap, " ");
        cc__hc_sb_append_cstr(&out, &out_len, &out_cap, params[i]);
    }
    cc__hc_sb_append_cstr(&out, &out_len, &out_cap, ") ");
    if (expr_src[body_s] == '{') {
        size_t body_r = 0;
        if (!cc__find_matching_brace(expr_src, expr_len, body_s, &body_r)) {
            free(out);
            return NULL;
        }
        cc__hc_sb_append(&out, &out_len, &out_cap, expr_src + body_s, body_r + 1 - body_s);
        cc__hc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
        return out;
    }
    cc__hc_sb_append_cstr(&out, &out_len, &out_cap, "{ return ");
    cc__hc_sb_append(&out, &out_len, &out_cap, expr_src + body_s, body_e - body_s);
    cc__hc_sb_append_cstr(&out, &out_len, &out_cap, "; }\n");
    return out;
}

int cc_comptime_compile_type_hook_callable(const char* registration_input_path,
                                           const char* logical_file,
                                           const char* original_src,
                                           size_t original_len,
                                           const char* expr_src,
                                           size_t expr_len,
                                           CCComptimeTypeHookKind kind,
                                           void** out_owner,
                                           const void** out_fn_ptr) {
    char err_buf[1024] = {0};
    char handler[128];
    size_t p = 0;
    int handler_is_ident = 0;
    char* compile_src = NULL;
    size_t compile_len = 0;
    char* logical_src = NULL;
    size_t logical_len = 0;
    char* blanked_src = NULL;
    char* pp_src = NULL;
    char* tu_src = NULL;
    char* lambda_def = NULL;
    char entry_name[128];
    char input_dir[1024];
    char repo_root[1024];
    char tmp_base[] = "/tmp/cc_comptime_type_hook_XXXXXX";
    char compile_cmd[4096];
    CCComptimeDlModule* module = NULL;
    if (!registration_input_path || !original_src || !expr_src || expr_len == 0 || !out_owner || !out_fn_ptr) return -1;
    *out_owner = NULL;
    *out_fn_ptr = NULL;

    if (logical_file && logical_file[0] && strcmp(logical_file, registration_input_path) != 0) {
        cc__read_entire_file(logical_file, &logical_src, &logical_len);
    }
    compile_src = (logical_src && logical_len > 0) ? logical_src : (char*)original_src;
    compile_len = (logical_src && logical_len > 0) ? logical_len : original_len;
    if (cc__parse_ident(expr_src, expr_len, &p, handler, sizeof(handler)) && p == expr_len) {
        handler_is_ident = 1;
    }

    blanked_src = cc__blank_comptime_blocks_preserve_layout(compile_src, compile_len);
    if (!blanked_src) { snprintf(err_buf, sizeof(err_buf), "failed to blank comptime blocks"); goto done; }
    {
        char* lowered_local = cc_rewrite_local_cch_includes_to_lowered_headers(blanked_src, strlen(blanked_src),
                                                                                (logical_src && logical_len > 0) ? logical_file : registration_input_path);
        if (lowered_local) {
            free(blanked_src);
            blanked_src = lowered_local;
        }
    }
    {
        char* lowered_system = cc_rewrite_system_cch_includes_to_lowered_headers(blanked_src, strlen(blanked_src));
        if (lowered_system) {
            free(blanked_src);
            blanked_src = lowered_system;
        }
    }
    snprintf(entry_name, sizeof(entry_name), "__cc_%s_hook_%zu",
             kind == CC_COMPTIME_TYPE_HOOK_UFCS ? "ufcs" : "create",
             expr_len);
    if (handler_is_ident) {
        tu_src = cc__build_wrapper_tu(blanked_src, strlen(blanked_src),
                                      cc__find_repo_root(registration_input_path, repo_root, sizeof(repo_root)) ? repo_root : NULL,
                                      NULL, entry_name, handler, 1, kind);
    } else {
        char lambda_name[128];
        snprintf(lambda_name, sizeof(lambda_name), "__cc_type_hook_lambda_%zu", expr_len);
        lambda_def = cc__build_lambda_definition(expr_src, expr_len, lambda_name, kind);
        if (!lambda_def) { snprintf(err_buf, sizeof(err_buf), "failed to build hook lambda definition"); goto done; }
        tu_src = cc__build_wrapper_tu(blanked_src, strlen(blanked_src),
                                      cc__find_repo_root(registration_input_path, repo_root, sizeof(repo_root)) ? repo_root : NULL,
                                      lambda_def, entry_name, lambda_name, 0, kind);
    }
    if (!tu_src) { snprintf(err_buf, sizeof(err_buf), "failed to build hook wrapper TU"); goto done; }
    cc__dirname((logical_src && logical_len > 0) ? logical_file : registration_input_path, input_dir, sizeof(input_dir));
    {
        int tmp_fd = mkstemp(tmp_base);
        if (tmp_fd < 0) { snprintf(err_buf, sizeof(err_buf), "failed to allocate temp path"); goto done; }
        close(tmp_fd);
    }
    unlink(tmp_base);
    module = (CCComptimeDlModule*)calloc(1, sizeof(*module));
    if (!module) { snprintf(err_buf, sizeof(err_buf), "failed to allocate hook module"); goto done; }
    snprintf(module->obj_path, sizeof(module->obj_path), "%s.c", tmp_base);
    snprintf(module->dylib_path, sizeof(module->dylib_path), "%s.dylib", tmp_base);
    {
        FILE* srcf = fopen(module->obj_path, "w");
        if (!srcf) { snprintf(err_buf, sizeof(err_buf), "failed to write temp hook source"); goto done; }
        fputs(tu_src, srcf);
        fclose(srcf);
    }
    if (cc__format_compile_cmd(compile_cmd, sizeof(compile_cmd),
                               repo_root[0] ? repo_root : NULL,
                               input_dir[0] ? input_dir : NULL,
                               module->dylib_path,
                               module->obj_path) != 0) { snprintf(err_buf, sizeof(err_buf), "failed to format host compile command"); goto done; }
    if (system(compile_cmd) != 0) { snprintf(err_buf, sizeof(err_buf), "host compile failed for %s", module->obj_path); goto done; }
    module->dl_handle = dlopen(module->dylib_path, RTLD_NOW | RTLD_LOCAL);
    if (!module->dl_handle) { snprintf(err_buf, sizeof(err_buf), "dlopen failed: %s", dlerror() ? dlerror() : "unknown error"); goto done; }
    *out_fn_ptr = dlsym(module->dl_handle, entry_name);
    if (!*out_fn_ptr) { snprintf(err_buf, sizeof(err_buf), "dlsym failed: %s", dlerror() ? dlerror() : "missing symbol"); goto done; }
    *out_owner = module;
    module = NULL;
done:
    if (!*out_fn_ptr && err_buf[0]) {
        if (tu_src) {
            FILE* dbg = fopen("/tmp/cc_last_type_hook_fail.c", "w");
            if (dbg) {
                fputs(tu_src, dbg);
                fclose(dbg);
            }
        }
        fprintf(stderr, "%s: error: failed to compile %s hook: %s\n",
                registration_input_path ? registration_input_path : "<input>",
                kind == CC_COMPTIME_TYPE_HOOK_UFCS ? "ufcs" : "create",
                err_buf);
    }
    if (module) cc_comptime_type_hook_owner_free(module);
    free(logical_src);
    free(blanked_src);
    free(pp_src);
    free(tu_src);
    free(lambda_def);
    return *out_fn_ptr ? 0 : -1;
}
