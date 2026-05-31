#include "visitor_fileutil.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "util/path.h"
#include "preprocess/cpp_expand.h"

/* ---- Cached, flattened reparse prelude --------------------------------- *
 *
 * The reparse prelude is a compile-time constant whose only inputs are the
 * standard + CC system headers.  Re-preprocessing those headers on every
 * reparse is the dominant reparse cost.  We flatten the prelude once with
 * `-dD` semantics (retains macros + include guards) and persist the result to
 * a disk cache, so:
 *   - the expensive flatten happens at most once per (prelude, headers) tuple,
 *     amortized across every file in a build *and* across separate compiler
 *     invocations (e.g. the test suite spawns one process per test);
 *   - the retained include guards make the user body's own #includes self-skip
 *     on the cached re-parse, so TCC never re-reads the headers.
 *
 * Invalidation: the cache filename embeds a hash of the prelude text (so
 * editing the prelude invalidates it), and the cache is rejected if any header
 * it was built from is newer than the cache file (so editing a .cch/.h
 * invalidates it).  The dependency list is recovered from the `#line N "path"`
 * markers the flatten itself emits. */

static unsigned long long cc__fnv1a64(const char* s, size_t n) {
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int cc__path_mtime(const char* path, time_t* out) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    *out = st.st_mtime;
    return 1;
}

/* Append a NUL-terminated dep path (deduplicated) to a growing array. */
static void cc__deps_add(char*** deps, size_t* n, size_t* cap, const char* path, size_t plen) {
    if (plen == 0 || plen >= 4096) return;
    if (path[0] != '/') return; /* only real on-disk headers, skip <virtual> */
    for (size_t i = 0; i < *n; i++) {
        if (strncmp((*deps)[i], path, plen) == 0 && (*deps)[i][plen] == '\0') return;
    }
    if (*n >= *cap) {
        size_t nc = *cap ? *cap * 2 : 32;
        char** g = (char**)realloc(*deps, nc * sizeof(char*));
        if (!g) return;
        *deps = g;
        *cap = nc;
    }
    char* dup = (char*)malloc(plen + 1);
    if (!dup) return;
    memcpy(dup, path, plen);
    dup[plen] = '\0';
    (*deps)[(*n)++] = dup;
}

/* Parse `#line N "path"` markers out of flattened text into a dep list. */
static char** cc__collect_flat_deps(const char* flat, size_t flen, size_t* out_n) {
    char** deps = NULL;
    size_t n = 0, cap = 0;
    size_t i = 0;
    while (i < flen) {
        size_t ls = i;
        while (i < flen && flat[i] != '\n') i++;
        size_t le = i;
        if (i < flen) i++;
        const char* p = flat + ls;
        size_t plen = le - ls;
        /* match: #line <digits> "..."  (normalized form) */
        if (plen < 9 || p[0] != '#') continue;
        const char* q = p + 1;
        while (q < p + plen && (*q == ' ' || *q == '\t')) q++;
        if ((size_t)(p + plen - q) < 4 || strncmp(q, "line", 4) != 0) continue;
        q += 4;
        const char* dq = memchr(q, '"', (size_t)(p + plen - q));
        if (!dq) continue;
        dq++;
        const char* end = memchr(dq, '"', (size_t)(p + plen - dq));
        if (!end) continue;
        cc__deps_add(&deps, &n, &cap, dq, (size_t)(end - dq));
    }
    *out_n = n;
    return deps;
}

static int cc__flat_cache_is_fresh(const char* cache_path, const char* deps_path) {
    time_t cache_mtime;
    if (!cc__path_mtime(cache_path, &cache_mtime)) return 0;
    char* deps_buf = NULL;
    size_t deps_len = 0;
    if (!cc__read_entire_file(deps_path, &deps_buf, &deps_len)) return 0;
    int fresh = 1;
    size_t i = 0;
    while (i < deps_len && fresh) {
        size_t ls = i;
        while (i < deps_len && deps_buf[i] != '\n') i++;
        size_t le = i;
        if (i < deps_len) i++;
        if (le <= ls) continue;
        char saved = deps_buf[le];
        deps_buf[le] = '\0';
        time_t dep_mtime;
        if (!cc__path_mtime(deps_buf + ls, &dep_mtime) || dep_mtime > cache_mtime) fresh = 0;
        deps_buf[le] = saved;
    }
    free(deps_buf);
    return fresh;
}

static void cc__atomic_write(const char* path, const char* data, size_t len) {
    char tmp[2048];
    int wn = snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    if (wn <= 0 || (size_t)wn >= sizeof(tmp)) return;
    FILE* f = fopen(tmp, "wb");
    if (!f) return;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    if (w == len) rename(tmp, path); else remove(tmp);
}

/* TCC's `-dD` serialization emits a variadic macro's parameter list as the
 * literal token `__VA_ARGS__` instead of `...`, e.g. it prints
 *   #define __API_AVAILABLE(__VA_ARGS__) ...
 * for a macro that was actually defined as `#define __API_AVAILABLE(...)`.
 * Re-parsing that turns a variadic macro into a 1-parameter macro, so any
 * later use with multiple args fails ("used with too many args").  Repair the
 * *parameter list* (only) of each `#define` line: if the final parameter is
 * the bare token `__VA_ARGS__`, rewrite it to `...`.  The macro body is left
 * untouched (a body may legitimately reference `__VA_ARGS__`).  The edit only
 * ever shrinks the text, so it is performed in place. */
static void cc__fixup_flat_va_args(char* s, size_t* io_len) {
    size_t len = *io_len;
    size_t r = 0, w = 0;
    while (r < len) {
        size_t ls = r;
        while (r < len && s[r] != '\n') r++;
        size_t le = r;               /* line is [ls, le) */
        int had_nl = (r < len);
        if (r < len) r++;            /* consume newline */

        /* Detect a function-like #define and locate its parameter list. */
        size_t p = ls;
        while (p < le && (s[p] == ' ' || s[p] == '\t')) p++;
        int is_define = (le - p >= 8 && memcmp(s + p, "#define ", 8) == 0);
        size_t open = 0, close = 0;
        if (is_define) {
            size_t q = p + 8;
            while (q < le && (s[q] == ' ' || s[q] == '\t')) q++;
            size_t name_start = q;
            while (q < le && (((unsigned char)s[q] >= 0x80) ||
                              s[q] == '_' ||
                              (s[q] >= 'a' && s[q] <= 'z') ||
                              (s[q] >= 'A' && s[q] <= 'Z') ||
                              (s[q] >= '0' && s[q] <= '9'))) q++;
            /* function-like only when '(' immediately follows the name */
            if (q > name_start && q < le && s[q] == '(') {
                open = q;
                int depth = 0;
                for (size_t k = q; k < le; k++) {
                    if (s[k] == '(') depth++;
                    else if (s[k] == ')') { depth--; if (depth == 0) { close = k; break; } }
                }
            }
        }

        if (open && close && close > open + 1) {
            /* Find the last parameter token inside (open, close). */
            size_t pe = close;
            while (pe > open + 1 && (s[pe - 1] == ' ' || s[pe - 1] == '\t')) pe--;
            size_t ps = pe;
            while (ps > open + 1 && s[ps - 1] != ',' && s[ps - 1] != '(') ps--;
            size_t tlen = pe - ps;
            if (tlen == 11 && memcmp(s + ps, "__VA_ARGS__", 11) == 0) {
                /* Copy [ls, ps) then "..." then [pe, le) then newline. */
                memmove(s + w, s + ls, ps - ls); w += ps - ls;
                s[w++] = '.'; s[w++] = '.'; s[w++] = '.';
                memmove(s + w, s + pe, le - pe); w += le - pe;
                if (had_nl) s[w++] = '\n';
                continue;
            }
        }
        /* Unmodified line: copy verbatim including its newline (if any). */
        size_t span = (le - ls) + (had_nl ? 1 : 0);
        memmove(s + w, s + ls, span); w += span;
    }
    s[w] = '\0';
    *io_len = w;
}

/* Returns a malloc'd flattened prelude (caller must NOT free; ownership is held
 * by the per-process static cache in the caller), or NULL on failure. */
static char* cc__build_flat_prelude(const char* prelude, const char* original_path) {
    size_t flat_len = 0;
    char* flat = cc_cpp_expand_ex(prelude, strlen(prelude), original_path, &flat_len, 1);
    if (!flat || !flat_len) { free(flat); return NULL; }
    cc__fixup_flat_va_args(flat, &flat_len);

    const char* cache_dir = getenv("CC_PRELUDE_CACHE_DIR");
    char dirbuf[1600];
    if (!cache_dir || !cache_dir[0]) {
        const char* tmp = getenv("TMPDIR");
        if (!tmp || !tmp[0]) tmp = "/tmp";
        snprintf(dirbuf, sizeof(dirbuf), "%s", tmp);
        cache_dir = dirbuf;
    }
    unsigned long long h = cc__fnv1a64(prelude, strlen(prelude));
    char cache_path[2048];
    char deps_path[2048];
    snprintf(cache_path, sizeof(cache_path), "%s/cc_flat_prelude_%016llx.c", cache_dir, h);
    snprintf(deps_path, sizeof(deps_path), "%s/cc_flat_prelude_%016llx.deps", cache_dir, h);

    /* Write the cache (and dep list) for future invocations. */
    size_t ndeps = 0;
    char** deps = cc__collect_flat_deps(flat, flat_len, &ndeps);
    if (deps) {
        size_t buf_cap = 0;
        for (size_t i = 0; i < ndeps; i++) buf_cap += strlen(deps[i]) + 1;
        char* depbuf = (char*)malloc(buf_cap + 1);
        if (depbuf) {
            size_t off = 0;
            for (size_t i = 0; i < ndeps; i++) {
                size_t l = strlen(deps[i]);
                memcpy(depbuf + off, deps[i], l); off += l;
                depbuf[off++] = '\n';
            }
            cc__atomic_write(cache_path, flat, flat_len);
            cc__atomic_write(deps_path, depbuf, off);
            free(depbuf);
        }
        for (size_t i = 0; i < ndeps; i++) free(deps[i]);
        free(deps);
    }
    return flat;
}

/* Load the flattened prelude from disk cache when fresh, else (re)build it. */
static const char* cc__cached_flat_prelude(const char* prelude, const char* original_path) {
    static char* g_flat = NULL;
    static int g_tried = 0;
    if (g_tried) return g_flat;
    g_tried = 1;

    const char* cache_dir = getenv("CC_PRELUDE_CACHE_DIR");
    char dirbuf[1600];
    if (!cache_dir || !cache_dir[0]) {
        const char* tmp = getenv("TMPDIR");
        if (!tmp || !tmp[0]) tmp = "/tmp";
        snprintf(dirbuf, sizeof(dirbuf), "%s", tmp);
        cache_dir = dirbuf;
    }
    unsigned long long h = cc__fnv1a64(prelude, strlen(prelude));
    char cache_path[2048];
    char deps_path[2048];
    snprintf(cache_path, sizeof(cache_path), "%s/cc_flat_prelude_%016llx.c", cache_dir, h);
    snprintf(deps_path, sizeof(deps_path), "%s/cc_flat_prelude_%016llx.deps", cache_dir, h);

    if (cc__flat_cache_is_fresh(cache_path, deps_path)) {
        char* buf = NULL;
        size_t len = 0;
        if (cc__read_entire_file(cache_path, &buf, &len) && len) {
            g_flat = buf;
            return g_flat;
        }
        free(buf);
    }
    g_flat = cc__build_flat_prelude(prelude, original_path);
    return g_flat;
}

int cc__read_entire_file(const char* path, char** out_buf, size_t* out_len) {
    if (!path || !out_buf || !out_len) return 0;
    *out_buf = NULL;
    *out_len = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    *out_buf = buf;
    *out_len = n;
    return 1;
}

char* cc__write_temp_c_file(const char* buf, size_t len, const char* original_path) {
    (void)errno;
    if (!buf || !original_path) return NULL;
    char tmpl[] = "/tmp/cc_reparse_XXXXXX.c";
#ifdef __APPLE__
    int fd = mkstemps(tmpl, 2);
#else
    int fd = mkstemp(tmpl);
#endif
    if (fd < 0) return NULL;
    /* Prelude for reparse: must provide CC runtime types used by intermediate rewrites
       (e.g. CCNursery/CCClosure0) even when user source doesn't include the headers.
       cc_result.cch provides __CC_RESULT macro which handles parser-mode fallback. */
    const char* prelude =
        "#line 1 \"<cc-prelude>\"\n"
        "#define __CC__ 1\n"
        "#define CC_PARSER_MODE 1\n"
        "#include <stdlib.h>\n"
        "#include <stdint.h>\n"
        "typedef intptr_t CCAbIntptr;\n"
        "#include <ccc/cc_closure.cch>\n"
        "#include <ccc/cc_nursery.cch>\n"
        "#include <ccc/cc_arena.cch>\n"
        "#include <ccc/cc_result.cch>\n"
        "/* Minimal std/prelude declarations used by reparse-time arena types/helpers */\n"
        "static inline size_t kilobytes(size_t n);\n"
        "static inline size_t megabytes(size_t n);\n"
        "static inline CCArena cc_heap_arena(size_t bytes);\n"
        "static inline void cc_heap_arena_free(CCArena* a);\n"
        "#include <ccc/cc_slice.cch>\n"
        "#include <ccc/cc_channel.cch>\n"
        "#include <ccc/std/task.cch>\n"
        "typedef struct CCChan CCChan;\n"
        "CCTaskIntptr cc_channel_send_task(CCChan* ch, const void* value, size_t value_size);\n"
        "CCTaskIntptr cc_channel_recv_task(CCChan* ch, void* out_value, size_t value_size);\n"
        "#ifndef CC_TASK_RESULT_PTR_OR_RETURN\n"
        "#define CC_TASK_RESULT_PTR_OR_RETURN(type, var) type* var = (type*)cc_task_result_ptr(sizeof(type)); if (!(var)) return NULL\n"
        "#endif\n"
        "#ifndef CC_SEND_TASK_OR_JOIN\n"
        "#define CC_SEND_TASK_OR_JOIN(tx_expr, task_var) do { int __cc_send_err = cc_chan_send((tx_expr).raw, &(task_var), sizeof(task_var)); if (__cc_send_err != 0) { (void)cc_block_on_intptr(task_var); } } while (0)\n"
        "#endif\n"
        "typedef struct { void (*fn)(void); } __cc_spawn_void_arg;\n"
        "typedef struct { void (*fn)(int); int arg; } __cc_spawn_int_arg;\n"
        "static void* __cc_spawn_thunk_void(void*);\n"
        "static void* __cc_spawn_thunk_int(void*);\n";
    size_t pre_len = strlen(prelude);
    size_t off = 0;
    while (off < pre_len) {
        ssize_t n = write(fd, prelude + off, pre_len - off);
        if (n <= 0) { close(fd); unlink(tmpl); return NULL; }
        off += (size_t)n;
    }
    char rel[1024];
    const char* lp = cc_path_rel_to_repo(original_path, rel, sizeof(rel));
    char line_buf[1024];
    int ln = snprintf(line_buf, sizeof(line_buf), "#line 1 \"%s\"\n", lp);
    if (ln <= 0 || (size_t)ln >= sizeof(line_buf)) { close(fd); unlink(tmpl); return NULL; }
    off = 0;
    while (off < (size_t)ln) {
        ssize_t n = write(fd, line_buf + off, (size_t)ln - off);
        if (n <= 0) { close(fd); unlink(tmpl); return NULL; }
        off += (size_t)n;
    }
    off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n <= 0) { close(fd); unlink(tmpl); return NULL; }
        off += (size_t)n;
    }
    close(fd);
    return strdup(tmpl);
}

char* cc__prepend_reparse_prelude(const char* buf, size_t len, size_t* out_len, const char* original_path) {
    if (!buf || !out_len) return NULL;
    *out_len = 0;
    /* Prelude for reparse: must provide CC runtime types used by intermediate rewrites
       (e.g. CCNursery/CCClosure0) even when user source doesn't include the headers.
       cc_result.cch provides __CC_RESULT macro which handles parser-mode fallback. */
    const char* prelude =
        "#define __CC__ 1\n"
        "#define CC_PARSER_MODE 1\n"
        "#include <stdlib.h>\n"
        "#include <stdint.h>\n"
        "typedef intptr_t CCAbIntptr;\n"
        "#include <ccc/cc_closure.cch>\n"
        /* Closure-lowering macro stubs.  The full definitions live in
         * ccc/cc_closure_helper.h, but including that header in parser-mode
         * drags in function bodies that reference types the reparse AST does
         * not know about.  The final-UFCS reparse splices closure_defs (lifted
         * closure bodies) into src_ufcs, so the reparse must expand these
         * macros into syntactically-valid declarations of the env pointer;
         * otherwise TCC sees CC_CLOSURE_ENV_ALLOC(E, v) as an implicit call,
         * leaves `v` undeclared, and fails later `v->field` with "pointer
         * expected".  Stub to minimal declaration-shaped forms. */
        "#ifndef CC_CLOSURE0_DECL\n"
        "#define CC_CLOSURE0_DECL(n) static void* __cc_closure_entry_##n(void*); static CCClosure0 __cc_closure_make_##n(void)\n"
        "#endif\n"
        "#ifndef CC_CLOSURE0_SIMPLE\n"
        "#define CC_CLOSURE0_SIMPLE(n) static CCClosure0 __cc_closure_make_##n(void) { return cc_closure0_make(__cc_closure_entry_##n, (void*)0, (void*)0); } static void* __cc_closure_entry_##n(void* __p)\n"
        "#endif\n"
        "#ifndef CC_CLOSURE_ENV_ALLOC\n"
        "#define CC_CLOSURE_ENV_ALLOC(env_ty, var) env_ty* var = (env_ty*)0\n"
        "#endif\n"
        "#ifndef CC_CLOSURE_ENV_NURSERY_ALLOC\n"
        "#define CC_CLOSURE_ENV_NURSERY_ALLOC(nursery, env_ty, var) env_ty* var = (env_ty*)0\n"
        "#endif\n"
        "#ifndef CC_TASK_RESULT_PTR_OR_RETURN\n"
        "#define CC_TASK_RESULT_PTR_OR_RETURN(type, var) type* var = (type*)0\n"
        "#endif\n"
        "#ifndef CC_SEND_TASK_OR_JOIN\n"
        "#define CC_SEND_TASK_OR_JOIN(tx_expr, task_var) ((void)0)\n"
        "#endif\n"
        "#ifndef CC_TSAN_RELEASE\n"
        "#define CC_TSAN_RELEASE(addr) ((void)0)\n"
        "#endif\n"
        "#ifndef CC_TSAN_ACQUIRE\n"
        "#define CC_TSAN_ACQUIRE(addr) ((void)0)\n"
        "#endif\n"
        "#ifndef __cc_ret\n"
        "#define __cc_ret(id, value) ((void)(value))\n"
        "#endif\n"
        "#ifndef __cc_ret_ok\n"
        "#define __cc_ret_ok(id, value) ((void)(value))\n"
        "#endif\n"
        "#ifndef __cc_ret_err\n"
        "#define __cc_ret_err(id, err) ((void)(err))\n"
        "#endif\n"
        "#include <ccc/cc_nursery.cch>\n"
        "#include <ccc/cc_arena.cch>\n"
        "#include <ccc/cc_result.cch>\n"
        "/* Minimal std/prelude declarations used by reparse-time arena types/helpers */\n"
        "static inline size_t kilobytes(size_t n);\n"
        "static inline size_t megabytes(size_t n);\n"
        "static inline CCArena cc_heap_arena(size_t bytes);\n"
        "static inline void cc_heap_arena_free(CCArena* a);\n"
        "#include <ccc/cc_slice.cch>\n"
        "#include <ccc/std/task.cch>\n"
        "typedef struct CCChan CCChan;\n"
        "CCTaskIntptr cc_channel_send_task(CCChan* ch, const void* value, size_t value_size);\n"
        "CCTaskIntptr cc_channel_recv_task(CCChan* ch, void* out_value, size_t value_size);\n"
        "typedef struct { void (*fn)(void); } __cc_spawn_void_arg;\n"
        "typedef struct { void (*fn)(int); int arg; } __cc_spawn_int_arg;\n"
        "static void* __cc_spawn_thunk_void(void*);\n"
        "static void* __cc_spawn_thunk_int(void*);\n";
    /* Prefer the cached, flattened prelude (see cc__cached_flat_prelude).  The
     * `#line 1` directive below resets coordinates for the user body, so the
     * flattened prelude's larger line count does not perturb AST→source
     * mapping.  Falls back to the raw prelude if flattening fails. */
    const char* eff_prelude = prelude;
    if (!getenv("CC_NO_PRELUDE_CACHE")) {
        const char* flat = cc__cached_flat_prelude(prelude, original_path);
        if (flat) eff_prelude = flat;
    }

    char rel[1024];
    const char* lp = cc_path_rel_to_repo(original_path ? original_path : "<input>", rel, sizeof(rel));
    char line_buf[1024];
    int ln = snprintf(line_buf, sizeof(line_buf), "#line 1 \"%s\"\n", lp ? lp : "<input>");
    size_t pre_len = strlen(eff_prelude);
    size_t line_len = (ln > 0 && (size_t)ln < sizeof(line_buf)) ? (size_t)ln : 0;
    size_t total = pre_len + line_len + len;
    char* out = (char*)malloc(total + 1);
    if (!out) return NULL;
    size_t off = 0;
    memcpy(out + off, eff_prelude, pre_len);
    off += pre_len;
    if (line_len > 0) {
        memcpy(out + off, line_buf, line_len);
        off += line_len;
    }
    memcpy(out + off, buf, len);
    off += len;
    out[off] = '\0';
    *out_len = off;
    return out;
}