#include "hook_compile.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "header/lower_header.h"
#include "preprocess/preprocess.h"
#include "preprocess/type_registry.h"
#include "util/path.h"
#include "visitor/visitor_fileutil.h"

extern char** environ;

/*
 * Refcounted owner for dynamically loaded @comptime type-hook helpers.
 *
 * A single compile (batch or single) produces one dylib and returns one owner
 * with refcount 1.  Each symbol-table entry that captures a resolved function
 * pointer retains the owner; each corresponding owner_free decrements the
 * refcount.  At refcount 0 we always dlclose() and unlink the (transient)
 * source file; the dylib itself is removed only when it's not part of the
 * persistent on-disk cache (`is_cached == 0`).
 */
typedef struct {
    void* dl_handle;
    char obj_path[1024];
    char dylib_path[1024];
    int  refcount;
    int  is_cached;
} CCComptimeDlModule;

void cc_comptime_type_hook_owner_retain(void* owner) {
    CCComptimeDlModule* m = (CCComptimeDlModule*)owner;
    if (!m) return;
    m->refcount++;
}

void cc_comptime_type_hook_owner_free(void* owner) {
    CCComptimeDlModule* m = (CCComptimeDlModule*)owner;
    if (!m) return;
    if (--m->refcount > 0) return;
    if (m->dl_handle) dlclose(m->dl_handle);
    if (m->obj_path[0]) unlink(m->obj_path);
    if (m->dylib_path[0] && !m->is_cached) unlink(m->dylib_path);
    free(m);
}

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

/*
 * Emit the preamble + filtered top-level source (with @comptime blocks already
 * blanked) that forms the shared body of a wrapper TU.  Only top-level chunks
 * that don't contain '@' are emitted — CC-only syntax is opaque to the host
 * compiler, so callers must not rely on handlers referencing @-decorated
 * top-level constructs.
 */
static void cc__emit_top_level_filtered(char** out,
                                        size_t* out_len,
                                        size_t* out_cap,
                                        const char* src,
                                        size_t n) {
    size_t i = 0;
    int line_start = 1;
    while (i < n) {
        if (line_start && src[i] == '#') {
            size_t line_end = i;
            while (line_end < n && src[line_end] != '\n') line_end++;
            if (strncmp(src + i, "#line", 5) != 0) {
                cc__hc_sb_append(out, out_len, out_cap, src + i, line_end - i);
                cc__hc_sb_append_cstr(out, out_len, out_cap, "\n");
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
                    if (!cc__find_matching_brace(src, n, j, &body_r)) { return; }
                    if (!saw_top_paren_close) { j = body_r; continue; }
                    if (!cc__chunk_contains_at(src, start, body_r + 1)) {
                        cc__hc_sb_append(out, out_len, out_cap, src + start, body_r + 1 - start);
                        cc__hc_sb_append_cstr(out, out_len, out_cap, "\n");
                    }
                    j = body_r;
                    break;
                }
                if (c == ';' && depth == 0) {
                    if (!cc__chunk_contains_at(src, start, j + 1)) {
                        cc__hc_sb_append(out, out_len, out_cap, src + start, j + 1 - start);
                        cc__hc_sb_append_cstr(out, out_len, out_cap, "\n");
                    }
                    break;
                }
            }
            i = (j < n) ? (j + 1) : j;
            line_start = 1;
        }
    }
}

/*
 * Emit a single lambda expression source "(a, b, ...) => expr_or_{block}" as a
 * `static` function named `lambda_name` with the signature required by `kind`.
 */
static int cc__emit_lambda_definition(char** out,
                                      size_t* out_len,
                                      size_t* out_cap,
                                      const char* expr_src,
                                      size_t expr_len,
                                      const char* lambda_name,
                                      CCComptimeTypeHookKind kind) {
    static const char* param_types_ufcs[6]   = { "CCSlice", "CCSlice", "CCSlice", "CCSliceArray", "CCSliceArray", "CCArena *" };
    static const char* param_types_create[4] = { "CCSlice", "CCSliceArray", "CCSliceArray", "CCArena *" };
    const char** param_types = (kind == CC_COMPTIME_TYPE_HOOK_UFCS) ? param_types_ufcs : param_types_create;
    int expected_params = (kind == CC_COMPTIME_TYPE_HOOK_UFCS) ? 6 : 4;
    char params[6][64];
    size_t lpar = 0, rpar = 0, p = 0, body_s = 0, body_e = expr_len;
    int param_count = 0;
    if (!expr_src || !lambda_name || expr_len == 0) return -1;
    p = cc__skip_ws(expr_src, expr_len, 0);
    if (p >= expr_len || expr_src[p] != '(') return -1;
    lpar = p;
    if (!cc__find_matching_paren(expr_src, expr_len, lpar, &rpar)) return -1;
    p = lpar + 1;
    while (p < rpar) {
        p = cc__skip_ws(expr_src, rpar, p);
        if (p >= rpar) break;
        if (param_count >= expected_params || !cc__parse_ident(expr_src, rpar, &p, params[param_count], sizeof(params[param_count]))) return -1;
        param_count++;
        p = cc__skip_ws(expr_src, rpar, p);
        if (p < rpar) {
            if (expr_src[p] != ',') return -1;
            p++;
        }
    }
    if (param_count != expected_params) return -1;
    body_s = cc__skip_ws(expr_src, expr_len, rpar + 1);
    if (body_s + 1 >= expr_len || expr_src[body_s] != '=' || expr_src[body_s + 1] != '>') return -1;
    body_s = cc__skip_ws(expr_src, expr_len, body_s + 2);
    cc__trim_range(expr_src, &body_s, &body_e);
    if (body_s >= body_e) return -1;

    cc__hc_sb_append_cstr(out, out_len, out_cap, "static CCSlice ");
    cc__hc_sb_append_cstr(out, out_len, out_cap, lambda_name);
    cc__hc_sb_append_cstr(out, out_len, out_cap, "(");
    for (int i = 0; i < expected_params; ++i) {
        if (i) cc__hc_sb_append_cstr(out, out_len, out_cap, ", ");
        cc__hc_sb_append_cstr(out, out_len, out_cap, param_types[i]);
        cc__hc_sb_append_cstr(out, out_len, out_cap, " ");
        cc__hc_sb_append_cstr(out, out_len, out_cap, params[i]);
    }
    cc__hc_sb_append_cstr(out, out_len, out_cap, ") ");
    if (expr_src[body_s] == '{') {
        size_t body_r = 0;
        if (!cc__find_matching_brace(expr_src, expr_len, body_s, &body_r)) return -1;
        cc__hc_sb_append(out, out_len, out_cap, expr_src + body_s, body_r + 1 - body_s);
        cc__hc_sb_append_cstr(out, out_len, out_cap, "\n");
        return 0;
    }
    cc__hc_sb_append_cstr(out, out_len, out_cap, "{ return ");
    cc__hc_sb_append(out, out_len, out_cap, expr_src + body_s, body_e - body_s);
    cc__hc_sb_append_cstr(out, out_len, out_cap, "; }\n");
    return 0;
}

/*
 * Emit a single exported wrapper `CCSlice <entry_name>(...) { return <callable>(...); }`.
 * `kind` selects the signature.
 */
static void cc__emit_wrapper(char** out,
                             size_t* out_len,
                             size_t* out_cap,
                             const char* entry_name,
                             const char* callable_name,
                             CCComptimeTypeHookKind kind) {
    cc__hc_sb_append_cstr(out, out_len, out_cap, "\nCCSlice ");
    cc__hc_sb_append_cstr(out, out_len, out_cap, entry_name);
    if (kind == CC_COMPTIME_TYPE_HOOK_UFCS) {
        cc__hc_sb_append_cstr(out, out_len, out_cap,
                              "(CCSlice recv_type, CCSlice method, CCSlice mode, CCSliceArray argv, CCSliceArray arg_types, CCArena *arena) {\n    return ");
        cc__hc_sb_append_cstr(out, out_len, out_cap, callable_name);
        cc__hc_sb_append_cstr(out, out_len, out_cap, "(recv_type, method, mode, argv, arg_types, arena);\n}\n");
    } else {
        cc__hc_sb_append_cstr(out, out_len, out_cap,
                              "(CCSlice type_name, CCSliceArray argv, CCSliceArray arg_types, CCArena *arena) {\n    return ");
        cc__hc_sb_append_cstr(out, out_len, out_cap, callable_name);
        cc__hc_sb_append_cstr(out, out_len, out_cap, "(type_name, argv, arg_types, arena);\n}\n");
    }
}

/* ---- argv builder + posix_spawnp host-compile invocation ---- */

typedef struct {
    char** items;
    size_t len;
    size_t cap;
} CCArgvBuilder;

static int cc__argv_push(CCArgvBuilder* a, const char* s) {
    if (!s) return -1;
    if (a->len + 2 > a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 16;
        char** nv = (char**)realloc(a->items, nc * sizeof(*nv));
        if (!nv) return -1;
        a->items = nv;
        a->cap = nc;
    }
    a->items[a->len] = strdup(s);
    if (!a->items[a->len]) return -1;
    a->len++;
    a->items[a->len] = NULL;
    return 0;
}

static void cc__argv_free(CCArgvBuilder* a) {
    if (!a || !a->items) return;
    for (size_t i = 0; i < a->len; ++i) free(a->items[i]);
    free(a->items);
    a->items = NULL;
    a->len = a->cap = 0;
}

static int cc__build_compile_argv(CCArgvBuilder* argv,
                                  const char* repo_root,
                                  const char* input_dir,
                                  const char* dylib_path,
                                  const char* source_path) {
    const char* cc_bin = getenv("CC");
    if (!cc_bin || !cc_bin[0]) cc_bin = "cc";
    char tmp[2048];
    if (cc__argv_push(argv, cc_bin) != 0) return -1;
#ifdef __APPLE__
    if (cc__argv_push(argv, "-dynamiclib") != 0) return -1;
    if (cc__argv_push(argv, "-undefined") != 0) return -1;
    if (cc__argv_push(argv, "dynamic_lookup") != 0) return -1;
#else
    if (cc__argv_push(argv, "-shared") != 0) return -1;
    if (cc__argv_push(argv, "-fPIC") != 0) return -1;
#endif
    /* Comptime helpers are throwaway code: -O0 cuts host compile cost ~3-5x
       and we never benchmark the resulting dylib. */
    if (cc__argv_push(argv, "-O0") != 0) return -1;
    if (repo_root && repo_root[0]) {
        snprintf(tmp, sizeof(tmp), "-I%s/cc/include", repo_root);
        if (cc__argv_push(argv, tmp) != 0) return -1;
        snprintf(tmp, sizeof(tmp), "-I%s/out/include", repo_root);
        if (cc__argv_push(argv, tmp) != 0) return -1;
    } else if (input_dir && input_dir[0]) {
        snprintf(tmp, sizeof(tmp), "-I%s", input_dir);
        if (cc__argv_push(argv, tmp) != 0) return -1;
    }
    if (cc__argv_push(argv, "-o") != 0) return -1;
    if (cc__argv_push(argv, dylib_path) != 0) return -1;
    if (cc__argv_push(argv, source_path) != 0) return -1;
    return 0;
}

static int cc__spawn_and_wait(char* const argv[]) {
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
    if (rc != 0) {
        fprintf(stderr, "posix_spawnp(%s) failed: %s\n", argv[0], strerror(rc));
        return -1;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return 0;
}

/* ---- content-addressed dylib cache ---- */

static uint64_t cc__hash64_fnv(const void* data, size_t n, uint64_t seed) {
    const uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
    const uint64_t FNV_PRIME  = 0x100000001b3ULL;
    uint64_t h = seed ? seed : FNV_OFFSET;
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= FNV_PRIME; }
    return h;
}

static int cc__file_exists(const char* path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0;
}

static int cc__mkdir_p(const char* path) {
    char buf[1024];
    size_t len = path ? strlen(path) : 0;
    if (len == 0 || len + 1 >= sizeof(buf)) return -1;
    memcpy(buf, path, len + 1);
    for (size_t i = 1; i < len; ++i) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
            buf[i] = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int cc__cache_dir(char* out, size_t out_sz) {
    const char* home = getenv("HOME");
    const char* tmp  = getenv("TMPDIR");
    int n;
    if (home && home[0]) {
        n = snprintf(out, out_sz, "%s/.cache/concurrent-c/comptime-hooks", home);
    } else if (tmp && tmp[0]) {
        n = snprintf(out, out_sz, "%s/cc-comptime-hooks", tmp);
    } else {
        n = snprintf(out, out_sz, "/tmp/cc-comptime-hooks");
    }
    if (n < 0 || (size_t)n >= out_sz) return -1;
    return cc__mkdir_p(out);
}

/* Toolchain fingerprint: cc binary mtime+size, computed once.  Ensures the
   cache invalidates when the host compiler changes underneath us. */
static uint64_t cc__toolchain_fingerprint(void) {
    static uint64_t cached = 0;
    static int      computed = 0;
    if (computed) return cached;
    computed = 1;
    const char* cc_bin = getenv("CC");
    if (!cc_bin || !cc_bin[0]) cc_bin = "/usr/bin/cc";
    struct stat st;
    if (stat(cc_bin, &st) == 0) {
        cached = cc__hash64_fnv(&st.st_mtime, sizeof(st.st_mtime), 0);
        cached = cc__hash64_fnv(&st.st_size,  sizeof(st.st_size),  cached);
    } else {
        /* Stable fallback so missing CC still produces a usable key. */
        cached = cc__hash64_fnv(cc_bin, strlen(cc_bin), 0);
    }
    return cached;
}

/* Build the cache key from (TU source bytes, compile argv minus -o/dylib path,
   toolchain fingerprint).  The dylib output path is excluded because it's
   itself a function of the key. */
static uint64_t cc__compute_cache_key(const char* tu_src,
                                      size_t tu_len,
                                      char* const argv[]) {
    uint64_t h = cc__hash64_fnv(tu_src, tu_len, 0);
    if (argv) {
        for (size_t i = 0; argv[i]; ++i) {
            /* Skip the -o token and the dylib path that follows it. */
            if (i + 1 < (size_t)-1 && strcmp(argv[i], "-o") == 0 && argv[i + 1]) {
                ++i; continue;
            }
            h = cc__hash64_fnv(argv[i], strlen(argv[i]) + 1, h);
        }
    }
    h = cc__hash64_fnv("|tc=", 4, h);
    uint64_t tc = cc__toolchain_fingerprint();
    h = cc__hash64_fnv(&tc, sizeof(tc), h);
    return h;
}

/*
 * Build, compile, and dlopen a single TU containing N wrappers.  Resolves one
 * function pointer per spec via dlsym.  On success, returns a module with
 * refcount 1; on failure, writes the offending TU to
 * /tmp/cc_last_type_hook_fail.c and returns -1.
 */
static int cc__build_compile_and_load(const char* input_path,
                                      const char* original_src,
                                      size_t original_len,
                                      const char* lambda_defs,      /* optional, already valid C */
                                      const CCComptimeHookSpec* specs,
                                      size_t n_specs,
                                      CCComptimeDlModule** out_module,
                                      const void** out_fn_ptrs) {
    char err_buf[1024] = {0};
    char* blanked_src = NULL;
    char* pp_src = NULL;
    char* tu_src = NULL;
    size_t tu_len = 0, tu_cap = 0;
    char input_dir[1024];
    char repo_root[1024];
    char tmp_base[] = "/tmp/cc_comptime_type_hook_XXXXXX";
    char cache_dir[1024];
    char cache_dylib_path[1280];
    CCComptimeDlModule* module = NULL;
    CCArgvBuilder argv = {0};
    CCTypeRegistry* saved_reg = NULL;
    CCTypeRegistry* temp_reg = NULL;
    int rc = -1;
    int source_is_header = 0;
    int needs_cc_preprocess = 0;
    int cache_enabled = 1;
    int cache_hit = 0;
    if (getenv("CC_COMPTIME_NO_CACHE")) cache_enabled = 0;

    repo_root[0] = '\0';
    input_dir[0] = '\0';

    /* UFCS hooks need the CC preprocessor to expand @-decorated constructs
       in top-level source (lambdas may reference CC types).  CREATE hooks
       only reference top-level function names, and their TU fails if we
       run the CC preprocessor over host-C headers that it doesn't fully
       understand (e.g. @result macro expansions in cc_result.cch).  Decide
       per batch based on kinds present. */
    for (size_t i = 0; i < n_specs; ++i) {
        if (specs[i].kind == CC_COMPTIME_TYPE_HOOK_UFCS) { needs_cc_preprocess = 1; break; }
    }

    /* Isolate type-registry side-effects of the comptime preprocess step so
       they don't leak into the main-pass registry. */
    if (needs_cc_preprocess) {
        saved_reg = cc_type_registry_get_global();
        temp_reg = cc_type_registry_new();
        if (temp_reg) cc_type_registry_set_global(temp_reg);
    }

    blanked_src = cc__blank_comptime_blocks_preserve_layout(original_src, original_len);
    if (!blanked_src) { snprintf(err_buf, sizeof(err_buf), "failed to blank comptime blocks"); goto done; }
    {
        char* lowered_local = cc_rewrite_local_cch_includes_to_lowered_headers(blanked_src, strlen(blanked_src), input_path);
        if (lowered_local) { free(blanked_src); blanked_src = lowered_local; }
    }
    {
        char* lowered_system = cc_rewrite_system_cch_includes_to_lowered_headers(blanked_src, strlen(blanked_src));
        if (lowered_system) { free(blanked_src); blanked_src = lowered_system; }
    }

    if (needs_cc_preprocess) {
        if (input_path) {
            size_t pl = strlen(input_path);
            if (pl >= 4 && strcmp(input_path + pl - 4, ".cch") == 0) {
                pp_src = cc_lower_header_string(blanked_src, strlen(blanked_src), input_path);
                source_is_header = (pp_src != NULL);
            }
        }
        if (!pp_src) {
            pp_src = cc_preprocess_to_string_ex(blanked_src, strlen(blanked_src), input_path, 1);
        }
        if (!pp_src) { snprintf(err_buf, sizeof(err_buf), "failed to preprocess CC source"); goto done; }
    }

    cc__hc_sb_append_cstr(&tu_src, &tu_len, &tu_cap, "#ifndef __CC__\n#define __CC__ 1\n#endif\n");
    if (needs_cc_preprocess) {
        cc__hc_sb_append_cstr(&tu_src, &tu_len, &tu_cap, "#ifndef CC_PARSER_MODE\n#define CC_PARSER_MODE 1\n#endif\n");
    }
    if (source_is_header) {
        /* cc_lower_header_string output is already a flat, host-C valid header
           (no @-decorated chunks to filter). */
        cc__hc_sb_append_cstr(&tu_src, &tu_len, &tu_cap, pp_src);
        cc__hc_sb_append_cstr(&tu_src, &tu_len, &tu_cap, "\n");
    } else {
        /* Filter out top-level chunks containing @-decorated CC syntax (e.g.
           main()'s @create / @detach), then emit the rest.  Use pp_src when
           available (UFCS batches), otherwise the cch-rewritten blanked_src. */
        const char* base = pp_src ? pp_src : blanked_src;
        cc__emit_top_level_filtered(&tu_src, &tu_len, &tu_cap, base, strlen(base));
    }
    if (lambda_defs && lambda_defs[0]) {
        cc__hc_sb_append_cstr(&tu_src, &tu_len, &tu_cap, "\n");
        cc__hc_sb_append_cstr(&tu_src, &tu_len, &tu_cap, lambda_defs);
        cc__hc_sb_append_cstr(&tu_src, &tu_len, &tu_cap, "\n");
    }
    for (size_t i = 0; i < n_specs; ++i) {
        const CCComptimeHookSpec* s = &specs[i];
        const char* callable = s->handler_name;
        if (!callable) { snprintf(err_buf, sizeof(err_buf), "spec %zu missing handler_name", i); goto done; }
        cc__emit_wrapper(&tu_src, &tu_len, &tu_cap, s->entry_name, callable, s->kind);
    }
    if (!tu_src) { snprintf(err_buf, sizeof(err_buf), "failed to build wrapper TU"); goto done; }

    (void)cc__find_repo_root(input_path, repo_root, sizeof(repo_root));
    cc__dirname(input_path, input_dir, sizeof(input_dir));

    module = (CCComptimeDlModule*)calloc(1, sizeof(*module));
    if (!module) { snprintf(err_buf, sizeof(err_buf), "failed to allocate hook module"); goto done; }
    module->refcount = 1;

    /* Build a "probe" argv (with a placeholder dylib path) used purely to
       compute the cache key.  -o + dylib_path are excluded from the hash
       inside cc__compute_cache_key. */
    if (cc__build_compile_argv(&argv,
                               repo_root[0] ? repo_root : NULL,
                               input_dir[0] ? input_dir : NULL,
                               "<placeholder.dylib>",
                               "<placeholder.c>") != 0) {
        snprintf(err_buf, sizeof(err_buf), "failed to build compile argv");
        goto done;
    }

    if (cache_enabled && cc__cache_dir(cache_dir, sizeof(cache_dir)) == 0) {
        uint64_t key = cc__compute_cache_key(tu_src, tu_len, argv.items);
        snprintf(cache_dylib_path, sizeof(cache_dylib_path),
                 "%s/%016llx.dylib", cache_dir, (unsigned long long)key);
        if (cc__file_exists(cache_dylib_path)) {
            /* Cache hit: skip the host compile entirely. */
            snprintf(module->dylib_path, sizeof(module->dylib_path), "%s", cache_dylib_path);
            module->is_cached = 1;
            cache_hit = 1;
        }
    }

    if (!cache_hit) {
        int tmp_fd = mkstemp(tmp_base);
        if (tmp_fd < 0) { snprintf(err_buf, sizeof(err_buf), "failed to allocate temp path"); goto done; }
        close(tmp_fd);
        unlink(tmp_base);

        snprintf(module->obj_path, sizeof(module->obj_path), "%s.c", tmp_base);
        if (cache_enabled && cache_dir[0]) {
            /* Compile straight to the cache so concurrent builds atomically
               (rename-into-place) share the result. */
            snprintf(module->dylib_path, sizeof(module->dylib_path), "%s", cache_dylib_path);
            module->is_cached = 1;
        } else {
            snprintf(module->dylib_path, sizeof(module->dylib_path), "%s.dylib", tmp_base);
            module->is_cached = 0;
        }

        FILE* srcf = fopen(module->obj_path, "w");
        if (!srcf) { snprintf(err_buf, sizeof(err_buf), "failed to write temp hook source"); goto done; }
        fputs(tu_src, srcf);
        fclose(srcf);

        /* Rebuild argv with the real paths. */
        cc__argv_free(&argv);
        if (cc__build_compile_argv(&argv,
                                   repo_root[0] ? repo_root : NULL,
                                   input_dir[0] ? input_dir : NULL,
                                   module->dylib_path,
                                   module->obj_path) != 0) {
            snprintf(err_buf, sizeof(err_buf), "failed to build compile argv");
            goto done;
        }
        if (cc__spawn_and_wait(argv.items) != 0) {
            snprintf(err_buf, sizeof(err_buf), "host compile failed for %s", module->obj_path);
            goto done;
        }
        /* TU source is no longer needed; the dylib is what we keep. */
        unlink(module->obj_path);
        module->obj_path[0] = '\0';
    }

    module->dl_handle = dlopen(module->dylib_path, RTLD_NOW | RTLD_LOCAL);
    if (!module->dl_handle) { snprintf(err_buf, sizeof(err_buf), "dlopen failed: %s", dlerror() ? dlerror() : "unknown error"); goto done; }

    for (size_t i = 0; i < n_specs; ++i) {
        out_fn_ptrs[i] = dlsym(module->dl_handle, specs[i].entry_name);
        if (!out_fn_ptrs[i]) {
            snprintf(err_buf, sizeof(err_buf), "dlsym failed for '%s': %s",
                     specs[i].entry_name, dlerror() ? dlerror() : "missing symbol");
            goto done;
        }
    }

    *out_module = module;
    module = NULL;
    rc = 0;

done:
    if (rc != 0 && err_buf[0]) {
        if (tu_src) {
            FILE* dbg = fopen("/tmp/cc_last_type_hook_fail.c", "w");
            if (dbg) { fputs(tu_src, dbg); fclose(dbg); }
        }
        fprintf(stderr, "%s: error: failed to compile @comptime hook batch: %s\n",
                input_path ? input_path : "<input>", err_buf);
    }
    if (module) {
        cc_comptime_type_hook_owner_free(module);
    }
    cc__argv_free(&argv);
    free(blanked_src);
    free(pp_src);
    free(tu_src);
    if (temp_reg) {
        cc_type_registry_set_global(saved_reg);
        cc_type_registry_free(temp_reg);
    }
    return rc;
}

int cc_comptime_compile_type_hooks(const char* registration_input_path,
                                   const char* logical_file,
                                   const char* original_src,
                                   size_t original_len,
                                   const CCComptimeHookSpec* specs,
                                   size_t n_specs,
                                   void** out_owner,
                                   const void** out_fn_ptrs) {
    char* logical_src = NULL;
    size_t logical_len = 0;
    char* lambda_defs = NULL;
    size_t lambda_len = 0, lambda_cap = 0;
    char** materialized_names = NULL;  /* owned; dlsym'd entry names */
    int rc = -1;

    if (!registration_input_path || !original_src || !specs || n_specs == 0 ||
        !out_owner || !out_fn_ptrs) return -1;
    *out_owner = NULL;
    for (size_t i = 0; i < n_specs; ++i) out_fn_ptrs[i] = NULL;

    /* If the caller provided a logical_file distinct from the registration
     * input, prefer that file's on-disk source as the compile base (headers
     * registering handlers define them in their own files). */
    const char* compile_src = original_src;
    size_t      compile_len = original_len;
    const char* compile_path = registration_input_path;
    if (logical_file && logical_file[0] && strcmp(logical_file, registration_input_path) != 0) {
        cc__read_entire_file(logical_file, &logical_src, &logical_len);
        if (logical_src && logical_len > 0) {
            compile_src = logical_src;
            compile_len = logical_len;
            compile_path = logical_file;
        }
    }

    /* Synthesize handler names for lambdas that come in as source spans. */
    materialized_names = (char**)calloc(n_specs, sizeof(char*));
    if (!materialized_names) goto done;

    /* Writable specs copy so we can fill in the lambda's generated handler_name. */
    CCComptimeHookSpec* specs_copy = (CCComptimeHookSpec*)calloc(n_specs, sizeof(*specs_copy));
    if (!specs_copy) goto done;
    for (size_t i = 0; i < n_specs; ++i) specs_copy[i] = specs[i];

    for (size_t i = 0; i < n_specs; ++i) {
        CCComptimeHookSpec* s = &specs_copy[i];
        if (!s->entry_name || !s->entry_name[0]) goto done;
        if (s->handler_name && s->handler_name[0]) continue;
        if (!s->lambda_src || s->lambda_len == 0) goto done;

        char name_buf[160];
        snprintf(name_buf, sizeof(name_buf), "__cc_lambda_%s", s->entry_name);
        materialized_names[i] = strdup(name_buf);
        if (!materialized_names[i]) goto done;

        if (cc__emit_lambda_definition(&lambda_defs, &lambda_len, &lambda_cap,
                                       s->lambda_src, s->lambda_len,
                                       materialized_names[i], s->kind) != 0) {
            fprintf(stderr, "%s: error: failed to lower lambda for entry '%s'\n",
                    registration_input_path, s->entry_name);
            goto done;
        }
        s->handler_name = materialized_names[i];
    }

    CCComptimeDlModule* module = NULL;
    if (cc__build_compile_and_load(compile_path,
                                   compile_src, compile_len,
                                   lambda_defs,
                                   specs_copy, n_specs,
                                   &module,
                                   out_fn_ptrs) != 0) {
        goto done;
    }
    *out_owner = module;
    rc = 0;

done:
    if (materialized_names) {
        for (size_t i = 0; i < n_specs; ++i) free(materialized_names[i]);
        free(materialized_names);
    }
    free(specs_copy);
    free(lambda_defs);
    free(logical_src);
    return rc;
}

/*
 * Single-hook API: wraps the batch API with one spec derived from expr_src.
 * expr_src is either an identifier (named handler) or a lambda source span.
 */
int cc_comptime_compile_type_hook_callable(const char* registration_input_path,
                                           const char* logical_file,
                                           const char* original_src,
                                           size_t original_len,
                                           const char* expr_src,
                                           size_t expr_len,
                                           CCComptimeTypeHookKind kind,
                                           void** out_owner,
                                           const void** out_fn_ptr) {
    char handler[128];
    size_t p = 0;
    int handler_is_ident = 0;
    if (!registration_input_path || !original_src || !expr_src || expr_len == 0 ||
        !out_owner || !out_fn_ptr) return -1;
    *out_owner = NULL;
    *out_fn_ptr = NULL;

    if (cc__parse_ident(expr_src, expr_len, &p, handler, sizeof(handler)) && p == expr_len) {
        handler_is_ident = 1;
    }

    char entry_name[160];
    snprintf(entry_name, sizeof(entry_name), "__cc_%s_hook_%zu",
             kind == CC_COMPTIME_TYPE_HOOK_UFCS ? "ufcs" : "create",
             expr_len);

    CCComptimeHookSpec spec = {0};
    spec.kind = kind;
    spec.entry_name = entry_name;
    if (handler_is_ident) {
        spec.handler_name = handler;
    } else {
        spec.lambda_src = expr_src;
        spec.lambda_len = expr_len;
    }

    const void* fn_ptr = NULL;
    int rc = cc_comptime_compile_type_hooks(registration_input_path,
                                            logical_file,
                                            original_src, original_len,
                                            &spec, 1,
                                            out_owner, &fn_ptr);
    if (rc == 0) *out_fn_ptr = fn_ptr;
    return rc;
}
