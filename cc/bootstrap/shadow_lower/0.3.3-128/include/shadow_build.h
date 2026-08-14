/* SERDES product build: emit/obj cache + host cc (ccc-compatible) + link.
 * Used by shadow_lower.ccs. Cache root: out/.cc-build/native/<fp>/. */
#pragma once

#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

static int shadow_build_ends_with(const char* s, const char* suf) {
    size_t n, m;
    if (!s || !suf) return 0;
    n = strlen(s);
    m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

typedef struct {
    long long mtime_sec;
    long long size;
} ShadowFileSig;

static int shadow_stat_sig(const char* path, ShadowFileSig* out) {
    struct stat st;
    if (!path || !out) return -1;
    if (stat(path, &st) != 0) return -1;
    out->mtime_sec = (long long)st.st_mtime;
    out->size = (long long)st.st_size;
    return 0;
}

static uint64_t shadow_fnv1a64_update(uint64_t h, const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t shadow_fnv1a64_str(uint64_t h, const char* s) {
    if (!s) s = "";
    return shadow_fnv1a64_update(h, s, strlen(s));
}

static uint64_t shadow_fnv1a64_i64(uint64_t h, long long v) {
    return shadow_fnv1a64_update(h, &v, sizeof(v));
}

static uint64_t shadow_fold_file_content(uint64_t h, const char* path) {
    FILE* f = fopen(path, "rb");
    char buf[4096];
    size_t n;
    if (!f) return shadow_fnv1a64_str(h, "\x01" "<absent>");
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        h = shadow_fnv1a64_update(h, buf, n);
    fclose(f);
    return h;
}

static int shadow_read_u64_file(const char* path, uint64_t* out) {
    FILE* f;
    char buf[64];
    char* end = NULL;
    unsigned long long v;
    if (!path || !out) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1; }
    fclose(f);
    v = strtoull(buf, &end, 16);
    if (end == buf) return -1;
    *out = (uint64_t)v;
    return 0;
}

static int shadow_write_u64_file(const char* path, uint64_t v) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "%016llx\n", (unsigned long long)v);
    fclose(f);
    return 0;
}

static int shadow_mkdir_p(const char* path) {
    char tmp[1024];
    size_t n, i;
    if (!path || !path[0]) return -1;
    snprintf(tmp, sizeof(tmp), "%s", path);
    n = strlen(tmp);
    for (i = 1; i < n; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = 0;
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int shadow_write_file(const char* path, const char* data, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

static int shadow_file_exists(const char* path) {
    return path && access(path, R_OK) == 0;
}

static int shadow_cache_disabled(int no_cache_flag) {
    const char* e;
    if (no_cache_flag) return 1;
    e = getenv("CC_NO_CACHE");
    return e && e[0] && e[0] != '0';
}

/* Host cc binary (override: CC). */
static const char* shadow_host_cc(void) {
    const char* cc = getenv("CC");
    return (cc && cc[0]) ? cc : "cc";
}

/* Host TCC needs `-B` for include/ + libtcc1.a (uninstalled tree). Match
 * cc_main / host_cc_profile so CC=…/tcc can compile emitted product C. */
static int shadow_host_is_tcc(const char* cc) {
    size_t n;
    if (!cc || !cc[0]) return 0;
    n = strlen(cc);
    if (n >= 3 && strcmp(cc + n - 3, "tcc") == 0) return 1;
    if (n >= 7 && strcmp(cc + n - 7, "tcc.exe") == 0) return 1;
    if (strstr(cc, "/tcc") || strstr(cc, "\\tcc")) return 1;
    return 0;
}

static const char* shadow_tcc_lib_dir(const char* cc, char* buf, size_t cap) {
    const char* env = getenv("CC_TCC_LIB_PATH");
    char from_bin[512];
    const char* cands[8];
    size_t nc = 0;
    size_t i;
    if (env && env[0]) cands[nc++] = env;
    if (cc && cc[0]) {
        size_t n = strlen(cc);
        if (n + 1 < sizeof(from_bin)) {
            memcpy(from_bin, cc, n + 1);
            char* slash = strrchr(from_bin, '/');
            if (slash && slash != from_bin) {
                *slash = 0;
                cands[nc++] = from_bin;
            }
        }
    }
    cands[nc++] = "third_party/tcc";
    cands[nc++] = "../third_party/tcc";
    cands[nc++] = "../../third_party/tcc";
    for (i = 0; i < nc; i++) {
        char probe[600];
        if (!cands[i] || !cands[i][0]) continue;
        snprintf(probe, sizeof(probe), "%s/include/stdbool.h", cands[i]);
        if (access(probe, R_OK) == 0) {
            snprintf(buf, cap, "%s", cands[i]);
            return buf;
        }
    }
    return NULL;
}

/* Append " -Bdir" when host is TCC; returns 1 if appended. */
static int shadow_append_tcc_B(char* cmd, size_t cap, const char* cc) {
    char dir[512];
    char flag[540];
    const char* libdir;
    size_t n;
    if (!cmd || !cap || !shadow_host_is_tcc(cc)) return 0;
    libdir = shadow_tcc_lib_dir(cc, dir, sizeof(dir));
    if (!libdir) return 0;
    snprintf(flag, sizeof(flag), " -B%s", libdir);
    n = strlen(cmd);
    if (n + strlen(flag) + 1 >= cap) return 0;
    memcpy(cmd + n, flag, strlen(flag) + 1);
    return 1;
}

/* Runtime object for host link.
 * Host TCC cannot link the `make -C cc` Mach-O concurrent_c.o (TCC emits ELF
 * objects on Darwin and links them into Mach-O). Prefer SHADOW_RUNTIME_O from
 * ccc (host-fingerprint cache); refuse the clang prebuilt when CC is tcc. */
static int shadow_runtime_o(char* dst, size_t cap) {
    const char* env = getenv("SHADOW_RUNTIME_O");
    const char* cc = shadow_host_cc();
    const char* cands[] = {
        "out/cc/obj/runtime/concurrent_c.o",
        "cc/obj/runtime/concurrent_c.o",
        /* Nested project dirs: cwd is not the repo root. */
        "../out/cc/obj/runtime/concurrent_c.o",
        "../../out/cc/obj/runtime/concurrent_c.o",
        "../../../out/cc/obj/runtime/concurrent_c.o",
    };
    int i;
    if (env && env[0] && access(env, R_OK) == 0) {
        snprintf(dst, cap, "%s", env);
        return 1;
    }
    if (shadow_host_is_tcc(cc)) {
        fprintf(stderr,
                "error: host TCC needs a TCC-built runtime object\n"
                "  set SHADOW_RUNTIME_O (ccc build does this) or use CC=cc\n");
        return 0;
    }
    for (i = 0; i < (int)(sizeof(cands) / sizeof(cands[0])); i++) {
        if (access(cands[i], R_OK) == 0) {
            snprintf(dst, cap, "%s", cands[i]);
            return 1;
        }
    }
    fprintf(stderr, "error: missing concurrent_c.o (make -C cc)\n");
    return 0;
}

/* Toolchain (compiler) identity: content bytes of shadow_lower + comptime/tcc
 * libs.  mtime is deliberately ignored — a rebuild that produces identical
 * bytes must keep warm hits; a byte change must miss.  Memoized per process
 * (ccc forks a fresh shadow_lower per build). */
static uint64_t shadow_toolchain_content_fp(const char* self_path) {
    static uint64_t cached = 0;
    static int cached_ok = 0;
    static char cached_self[512];
    uint64_t h;
    if (cached_ok && self_path && strcmp(cached_self, self_path) == 0)
        return cached;
    h = 1469598103934665603ULL;
    h = shadow_fnv1a64_str(h, "\x03" "toolchain:");
    h = shadow_fnv1a64_str(h, self_path ? self_path : "");
    h = shadow_fold_file_content(h, self_path);
    h = shadow_fold_file_content(h, "out/cc/obj/libshadow_comptime.a");
    h = shadow_fold_file_content(h, "third_party/tcc/libtcc.a");
    /* Include root *paths* only; header bytes are validated via emit.deps. */
    h = shadow_fnv1a64_str(h, "out/include");
    h = shadow_fnv1a64_str(h, "cc/include");
    cached = h;
    cached_ok = 1;
    if (self_path)
        snprintf(cached_self, sizeof(cached_self), "%s", self_path);
    else
        cached_self[0] = 0;
    return h;
}

/* Emit key: source content bytes + toolchain content bytes.
 * Transitive tapes / harvested .cch / `#pragma cc_depends` are not folded
 * here — they live in emit.deps (+ emit.deps.key) written on cold emit. */
static uint64_t shadow_emit_cache_key(const char* in_path, const char* self_path) {
    uint64_t h = 1469598103934665603ULL;
    h = shadow_fnv1a64_str(h, "\x01" "src:");
    h = shadow_fnv1a64_str(h, in_path ? in_path : "");
    h = shadow_fold_file_content(h, in_path);
    h ^= shadow_toolchain_content_fp(self_path);
    return h;
}

/* Fingerprint of declared transitive deps (path + content). */
static uint64_t shadow_fold_dep_paths(uint64_t h, const char* const* paths,
                                      int n) {
    int i;
    for (i = 0; i < n; i++) {
        ShadowFileSig sig = {0};
        if (!paths[i] || !paths[i][0]) continue;
        h = shadow_fnv1a64_str(h, "\x02" "dep:");
        h = shadow_fnv1a64_str(h, paths[i]);
        if (shadow_stat_sig(paths[i], &sig) == 0) {
            h = shadow_fnv1a64_i64(h, sig.mtime_sec);
            h = shadow_fnv1a64_i64(h, sig.size);
        }
        h = shadow_fold_file_content(h, paths[i]);
    }
    return h;
}

/* Persist dep path list + content fingerprint beside emit.c. */
static int shadow_emit_deps_save(const char* dir, const char* const* paths,
                                 int n) {
    char list_path[640], key_path[640];
    FILE* f;
    uint64_t h = 1469598103934665603ULL;
    int i;
    if (!dir || !dir[0]) return -1;
    snprintf(list_path, sizeof(list_path), "%s/emit.deps", dir);
    snprintf(key_path, sizeof(key_path), "%s/emit.deps.key", dir);
    f = fopen(list_path, "wb");
    if (!f) return -1;
    for (i = 0; i < n; i++) {
        if (!paths[i] || !paths[i][0]) continue;
        fprintf(f, "%s\n", paths[i]);
    }
    fclose(f);
    h = shadow_fold_dep_paths(h, paths, n);
    return shadow_write_u64_file(key_path, h);
}

/* Re-hash paths listed in emit.deps; 1 = still valid, 0 = miss / absent. */
static int shadow_emit_deps_match(const char* dir) {
    char list_path[640], key_path[640];
    char line[1024];
    const char* paths[256];
    char* owned[256];
    int n = 0, i;
    uint64_t prev = 0, h = 1469598103934665603ULL;
    FILE* f;
    if (!dir || !dir[0]) return 0;
    snprintf(list_path, sizeof(list_path), "%s/emit.deps", dir);
    snprintf(key_path, sizeof(key_path), "%s/emit.deps.key", dir);
    /* Legacy cache entries without a sidecar: force cold re-emit once. */
    if (!shadow_file_exists(list_path) || !shadow_file_exists(key_path))
        return 0;
    if (shadow_read_u64_file(key_path, &prev) != 0) return 0;
    f = fopen(list_path, "rb");
    if (!f) return 0;
    while (n < 256 && fgets(line, sizeof(line), f)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (!L) continue;
        owned[n] = (char*)malloc(L + 1);
        if (!owned[n]) {
            fclose(f);
            for (i = 0; i < n; i++) free(owned[i]);
            return 0;
        }
        memcpy(owned[n], line, L + 1);
        paths[n] = owned[n];
        n++;
    }
    fclose(f);
    h = shadow_fold_dep_paths(h, paths, n);
    for (i = 0; i < n; i++) free(owned[i]);
    return h == prev;
}

/* Host-build options forwarded from ccc (see cc__run_shadow_lower). */
typedef struct {
    int no_cache;
    int verbose;
    int opt_release;
    int opt_debug;
    int dry_run;
    int no_runtime; /* omit concurrent_c.o from link */
    const char* cc_flags; /* may be NULL */
    const char* ld_flags; /* may be NULL */
} ShadowHostOpts;

static ShadowHostOpts g_shadow_host_opts;

/* Obj key: emit key + host compiler content bytes + flag fingerprint.
 * Must fold every host-compile input (cc_flags, release/debug, no_runtime)
 * so warm tu.o is never reused across incompatible builds. */
static uint64_t shadow_obj_cache_key(uint64_t emit_key, const char* host_cc) {
    uint64_t h = emit_key;
    h = shadow_fnv1a64_str(h, "\x04" "hostcc:");
    h = shadow_fnv1a64_str(h, host_cc ? host_cc : "cc");
    /* Absolute host cc: fold bytes. PATH-resolved "cc" keeps the name only
     * (same limitation as before; prefer CC=/abs/path for strict identity). */
    if (host_cc && host_cc[0] == '/')
        h = shadow_fold_file_content(h, host_cc);
    h = shadow_fnv1a64_str(h, getenv("CFLAGS"));
    h = shadow_fnv1a64_str(h, getenv("CPPFLAGS"));
    h = shadow_fnv1a64_str(h, "-std=c11-D_DEFAULT_SOURCE-ffunction-sections-fdata-sections");
    if (shadow_host_is_tcc(host_cc)) {
        char tcc_dir[512];
        const char* libdir = shadow_tcc_lib_dir(host_cc, tcc_dir, sizeof(tcc_dir));
        h = shadow_fnv1a64_str(h, "-Btcc");
        h = shadow_fnv1a64_str(h, libdir);
    }
    h = shadow_fnv1a64_str(h, g_shadow_host_opts.cc_flags);
    h = shadow_fnv1a64_str(h, g_shadow_host_opts.ld_flags);
    h = shadow_fnv1a64_i64(h, g_shadow_host_opts.opt_release ? 1 : 0);
    h = shadow_fnv1a64_i64(h, g_shadow_host_opts.opt_debug ? 1 : 0);
    h = shadow_fnv1a64_i64(h, g_shadow_host_opts.no_runtime ? 1 : 0);
    return h;
}

static void shadow_cache_dir(uint64_t emit_key, char* dst, size_t cap) {
    snprintf(dst, cap, "out/.cc-build/native/%016llx", (unsigned long long)emit_key);
}

/* Run host cc -c with ccc-compatible flags. -D_DEFAULT_SOURCE keeps POSIX
 * decls visible under -std=c11 on glibc (usleep/mkstemp/fdopen) while
 * -Werror=implicit-function-declaration stays on. Flavor (-O2/-g/NDEBUG)
 * arrives via forwarded cc_flags / opt_release / opt_debug. */
/* Resolve a repo-relative include root that exists from the current cwd. */
static const char* shadow_find_inc_root(const char* rel, char* buf, size_t cap) {
    static const char* prefixes[] = { "", "../", "../../", "../../../" };
    int i;
    for (i = 0; i < (int)(sizeof(prefixes) / sizeof(prefixes[0])); i++) {
        snprintf(buf, cap, "%s%s", prefixes[i], rel);
        if (access(buf, R_OK) == 0) return buf;
    }
    return rel; /* best-effort fallback */
}

/* First existing colon-separated entry from CC_INCLUDE_PATH (prefix install). */
static const char* shadow_inc_from_env(char* buf, size_t cap) {
    const char* env = getenv("CC_INCLUDE_PATH");
    char paths[1024];
    char* p;
    char* sep;
    if (!env || !env[0]) return NULL;
    snprintf(paths, sizeof(paths), "%s", env);
    p = paths;
    while (p && *p) {
        sep = strchr(p, ':');
        if (sep) *sep = 0;
        if (*p && access(p, R_OK) == 0) {
            snprintf(buf, cap, "%s", p);
            return buf;
        }
        p = sep ? sep + 1 : NULL;
    }
    return NULL;
}

/* Original source line for a #line-mapped path. 0 if unreadable. */
static int shadow_orig_src_line(const char* path, int want, char* out, size_t cap) {
    FILE* f;
    char buf[4096];
    int n = 0;
    if (!path || !path[0] || path[0] == '<' || want < 1 || !out || !cap)
        return 0;
    if (shadow_build_ends_with(path, "emit.c") || strstr(path, "/.cc-build/"))
        return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(buf, sizeof(buf), f)) {
        size_t L;
        n++;
        if (n != want) continue;
        L = strlen(buf);
        while (L && (buf[L - 1] == '\n' || buf[L - 1] == '\r')) buf[--L] = 0;
        snprintf(out, cap, "%s", buf);
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}

/* `path:line:col: ` — last `:N:M: ` wins (message may contain colons). */
static int shadow_diag_parse_loc(const char* line, char* path, size_t pcap,
                                 int* lineno, int* col) {
    const char* p;
    const char* hit = NULL;
    int ln = 0, c = 0;
    if (!line || !path || !pcap || !lineno || !col) return 0;
    for (p = line; *p; p++) {
        if (*p != ':') continue;
        {
            const char* q = p + 1;
            int a = 0, b = 0;
            if (*q < '0' || *q > '9') continue;
            while (*q >= '0' && *q <= '9') a = a * 10 + (*q++ - '0');
            if (*q != ':' || q[1] < '0' || q[1] > '9') continue;
            q++;
            while (*q >= '0' && *q <= '9') b = b * 10 + (*q++ - '0');
            if (*q != ':' || q[1] != ' ') continue;
            hit = p;
            ln = a;
            c = b;
        }
    }
    if (!hit || ln <= 0) return 0;
    {
        size_t n = (size_t)(hit - line);
        if (!n || n >= pcap) return 0;
        memcpy(path, line, n);
        path[n] = 0;
    }
    *lineno = ln;
    *col = c;
    return 1;
}

static int shadow_diag_is_snippet(const char* line, int want_ln,
                                  const char** after_bar) {
    const char* p = line;
    int n = 0;
    if (!line || want_ln < 1) return 0;
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
    if (n != want_ln || *p != ' ' || p[1] != '|') return 0;
    p += 2;
    if (*p == ' ') p++;
    if (after_bar) *after_bar = p;
    return 1;
}

static int shadow_diag_is_caret(const char* line) {
    const char* p = line;
    if (!line) return 0;
    while (*p == ' ') p++;
    return *p == '|';
}

/* Replay host-cc stderr. #line already named the user file; replace the
 * caret snippet with that file's line so the rewritten C is not shown. */
static void shadow_host_diag_replay(const char* text, size_t len) {
    const char* p;
    const char* end;
    char path[1024];
    char orig[4096];
    int expect_ln = 0;
    int expect_col = 0;
    int have_orig = 0;
    if (!text || !len) return;
    p = text;
    end = text + len;
    path[0] = 0;
    orig[0] = 0;
    while (p < end) {
        const char* nl = p;
        char line[4096];
        size_t n;
        while (nl < end && *nl != '\n') nl++;
        n = (size_t)(nl - p);
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = 0;
        if (n && line[n - 1] == '\r') line[--n] = 0;

        if (expect_ln && shadow_diag_is_snippet(line, expect_ln, NULL) &&
            have_orig) {
            const char* bar = strstr(line, " |");
            if (bar) {
                fwrite(line, 1, (size_t)(bar - line), stderr);
                fputs(" | ", stderr);
                fputs(orig, stderr);
                fputc('\n', stderr);
            } else {
                fprintf(stderr, "%s\n", line);
            }
        } else if (expect_ln && have_orig && shadow_diag_is_caret(line)) {
            const char* bar = strchr(line, '|');
            int col = expect_col > 0 ? expect_col : 1;
            int i;
            size_t ol = strlen(orig);
            if ((size_t)col > ol + 1) col = ol ? (int)ol : 1;
            if (bar) {
                fwrite(line, 1, (size_t)(bar - line) + 1, stderr);
                fputc(' ', stderr);
            } else {
                fputs("      | ", stderr);
            }
            for (i = 1; i < col; i++) fputc(' ', stderr);
            fputs("^\n", stderr);
            expect_ln = 0;
            have_orig = 0;
        } else {
            int ln = 0, col = 0;
            fprintf(stderr, "%s\n", line);
            if (shadow_diag_parse_loc(line, path, sizeof(path), &ln, &col) &&
                shadow_orig_src_line(path, ln, orig, sizeof(orig))) {
                expect_ln = ln;
                expect_col = col;
                have_orig = 1;
            } else {
                expect_ln = 0;
                have_orig = 0;
            }
        }
        p = nl < end ? nl + 1 : end;
    }
}

static int shadow_host_compile(const char* c_path, const char* o_path) {
    char cmd[8192];
    char inc_env[512], inc_out[256], inc_cc[256], inc_ccinc[256], inc_ex[256];
    const char* cc = shadow_host_cc();
    const char* env_cflags = getenv("CFLAGS");
    const char* extra = g_shadow_host_opts.cc_flags;
    const char* release = g_shadow_host_opts.opt_release ? "-DNDEBUG" : "";
    const char* debug = g_shadow_host_opts.opt_debug ? "-g" : "";
    const char* i_env = shadow_inc_from_env(inc_env, sizeof(inc_env));
    const char* i_out = shadow_find_inc_root("out/include", inc_out, sizeof(inc_out));
    const char* i_cci = shadow_find_inc_root("cc/include", inc_ccinc, sizeof(inc_ccinc));
    const char* i_cc = shadow_find_inc_root("cc", inc_cc, sizeof(inc_cc));
    const char* i_ex = shadow_find_inc_root("examples", inc_ex, sizeof(inc_ex));
    int n;
    /* Prefer CC_INCLUDE_PATH (ccc sets this for prefix installs). Checkout
     * -I roots stay for in-tree builds; missing dirs are harmless. */
    n = snprintf(cmd, sizeof(cmd),
                 "%s -std=c11 -D_DEFAULT_SOURCE"
                 "%s%s%s"
                 " -I%s -I%s -I%s -I. -I%s"
                 " -ffunction-sections -fdata-sections"
                 " -Werror=implicit-function-declaration"
                 " %s %s %s %s"
                 " -c \"%s\" -o \"%s\"",
                 cc,
                 i_env ? " -I" : "", i_env ? i_env : "", i_env ? "" : "",
                 i_out, i_cci, i_cc, i_ex,
                 (env_cflags && env_cflags[0]) ? env_cflags : "",
                 (extra && extra[0]) ? extra : "",
                 release, debug,
                 c_path, o_path);
    if (n < 0 || (size_t)n >= sizeof(cmd)) return -1;
    /* Insert -B before -c: TCC needs libtcc1 + its include/ for stdbool.h. */
    if (shadow_host_is_tcc(cc)) {
        char* cflag = strstr(cmd, " -c ");
        char dir[512];
        const char* libdir = shadow_tcc_lib_dir(cc, dir, sizeof(dir));
        if (cflag && libdir) {
            char rebuilt[8192];
            size_t head = (size_t)(cflag - cmd);
            int m = snprintf(rebuilt, sizeof(rebuilt), "%.*s -B%s%s",
                             (int)head, cmd, libdir, cflag);
            if (m < 0 || (size_t)m >= sizeof(rebuilt)) return -1;
            snprintf(cmd, sizeof(cmd), "%s", rebuilt);
        } else if (!shadow_append_tcc_B(cmd, sizeof(cmd), cc)) {
            fprintf(stderr,
                    "error: host TCC '%s' needs -B (missing third_party/tcc)\n",
                    cc);
            return -1;
        }
    }
    if (g_shadow_host_opts.dry_run) {
        fprintf(stderr, "shadow_lower: dry-run compile:\n  %s\n", cmd);
        return 0;
    }
    if (g_shadow_host_opts.verbose)
        fprintf(stderr, "shadow_lower: %s\n", cmd);
    {
        char tmpl[] = "/tmp/shadow_hostcc.XXXXXX";
        int fd = mkstemp(tmpl);
        int saved = -1;
        int rc;
        if (fd < 0) {
            if (system(cmd) != 0) {
                fprintf(stderr, "error: host compile failed\n  %s\n", cmd);
                return -1;
            }
            return 0;
        }
        fflush(stderr);
        saved = dup(2);
        if (saved < 0 || dup2(fd, 2) < 0) {
            if (saved >= 0) close(saved);
            close(fd);
            unlink(tmpl);
            if (system(cmd) != 0) {
                fprintf(stderr, "error: host compile failed\n  %s\n", cmd);
                return -1;
            }
            return 0;
        }
        rc = system(cmd);
        fflush(stderr);
        dup2(saved, 2);
        close(saved);
        if (lseek(fd, 0, SEEK_SET) == 0) {
            char chunk[4096];
            char* buf = NULL;
            size_t used = 0, alloc = 0;
            ssize_t n;
            while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
                if (used + (size_t)n + 1 > alloc) {
                    size_t na = alloc ? alloc * 2 : 4096;
                    char* nb;
                    while (na < used + (size_t)n + 1) na *= 2;
                    nb = (char*)realloc(buf, na);
                    if (!nb) {
                        free(buf);
                        buf = NULL;
                        break;
                    }
                    buf = nb;
                    alloc = na;
                }
                memcpy(buf + used, chunk, (size_t)n);
                used += (size_t)n;
            }
            if (buf) {
                buf[used] = 0;
                shadow_host_diag_replay(buf, used);
                free(buf);
            } else if (lseek(fd, 0, SEEK_SET) == 0) {
                while ((n = read(fd, chunk, sizeof(chunk))) > 0)
                    fwrite(chunk, 1, (size_t)n, stderr);
            }
        }
        close(fd);
        unlink(tmpl);
        if (rc != 0) {
            fprintf(stderr, "error: host compile failed\n  %s\n", cmd);
            return -1;
        }
    }
    return 0;
}

/* Append `-lname` if not already present (legacy cc__add_lib_to_flags). */
static void shadow_add_lib_flag(char* ld, size_t cap, const char* lib) {
    char flag[288];
    size_t n;
    if (!ld || !cap || !lib || !lib[0]) return;
    if (strcmp(lib, "pthread") == 0 || strcmp(lib, "m") == 0) return;
    snprintf(flag, sizeof(flag), "-l%s", lib);
    if (strstr(ld, flag)) return;
    n = strlen(ld);
    if (n && n + 1 < cap) {
        ld[n++] = ' ';
        ld[n] = 0;
    }
    if (n + strlen(flag) + 1 < cap)
        memcpy(ld + n, flag, strlen(flag) + 1);
}

/* Scan emit/source text for link("lib") directives and __CC_LINK__ markers.
 * Pattern bytes are built at runtime so this TU's own source never contains
 * the contiguous at-link-paren-quote sequence (legacy host link-scan
 * matches .c text). */
static void shadow_scan_link_directives(const char* content, size_t len,
                                        char* ld, size_t cap) {
    char marker[16];
    char link_pat[8];
    size_t marker_len;
    size_t link_pat_len;
    const char* p;
    const char* end;
    if (!content || !len || !ld || !cap) return;
    /* "__CC_LINK__ " */
    memcpy(marker, "__CC_LINK__", 11);
    marker[11] = ' ';
    marker[12] = 0;
    marker_len = 12;
    /* "@" "link" "(" "\"" */
    link_pat[0] = '@';
    link_pat[1] = 'l';
    link_pat[2] = 'i';
    link_pat[3] = 'n';
    link_pat[4] = 'k';
    link_pat[5] = '(';
    link_pat[6] = '"';
    link_pat[7] = 0;
    link_pat_len = 7;
    p = content;
    end = content + len;
    while (p < end) {
        const char* pos = NULL;
        const char* pos_m = strstr(p, marker);
        const char* pos_l = strstr(p, link_pat);
        if (pos_m && pos_m < end) pos = pos_m;
        if (pos_l && pos_l < end && (!pos || pos_l < pos)) pos = pos_l;
        if (!pos) break;
        if (pos == pos_m) {
            const char* lib = pos + marker_len;
            const char* lib_end = lib;
            while (lib_end < end && *lib_end && *lib_end != ' ' &&
                   *lib_end != '*' && *lib_end != '\n' && *lib_end != '\r')
                lib_end++;
            if (lib_end > lib) {
                char name[256];
                size_t n = (size_t)(lib_end - lib);
                if (n < sizeof(name)) {
                    memcpy(name, lib, n);
                    name[n] = 0;
                    shadow_add_lib_flag(ld, cap, name);
                }
            }
            p = lib_end;
        } else {
            const char* lib = pos + link_pat_len;
            const char* q = lib;
            while (q < end && *q && *q != '"') q++;
            if (q > lib && q < end && *q == '"') {
                char name[256];
                size_t n = (size_t)(q - lib);
                if (n < sizeof(name)) {
                    memcpy(name, lib, n);
                    name[n] = 0;
                    shadow_add_lib_flag(ld, cap, name);
                }
                p = q + 1;
            } else {
                p = pos + link_pat_len;
            }
        }
    }
}

static void shadow_extract_links_from_file(const char* path, char* ld,
                                           size_t cap) {
    FILE* f;
    long sz;
    char* buf;
    size_t nread;
    if (!path || !ld || !cap) return;
    f = fopen(path, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    sz = ftell(f);
    if (sz <= 0) { fclose(f); return; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return; }
    buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nread] = 0;
    shadow_scan_link_directives(buf, nread, ld, cap);
    free(buf);
}

static int shadow_host_link(const char* o_path, const char* bin_path,
                            const char* runtime_o, const char* c_path,
                            const char* in_path) {
    char cmd[8192];
    char extracted[1024];
    const char* cc = shadow_host_cc();
    const char* ld = g_shadow_host_opts.ld_flags;
    const char* strip = "";
    int n;
    /* Dead-strip by default: every object is compiled with
     * -ffunction-sections/-fdata-sections, so the linker flag is pure
     * win.  Debug builds opt out; TCC has no equivalent. */
    if (!g_shadow_host_opts.opt_debug && !shadow_host_is_tcc(cc)) {
#if defined(__APPLE__)
        strip = "-Wl,-dead_strip";
#elif defined(__linux__)
        strip = "-Wl,--gc-sections";
#endif
    }
    extracted[0] = 0;
    if (ld && ld[0]) {
        snprintf(extracted, sizeof(extracted), "%s", ld);
    }
    /* Pull @link / __CC_LINK__ from emit.c and the original source (headers
     * may carry @link in comments that never make it into emit). */
    shadow_extract_links_from_file(c_path, extracted, sizeof(extracted));
    shadow_extract_links_from_file(in_path, extracted, sizeof(extracted));
    if (g_shadow_host_opts.no_runtime) {
        n = snprintf(cmd, sizeof(cmd),
                     "%s \"%s\" -o \"%s\" -lpthread -lm %s %s",
                     cc, o_path, bin_path, strip, extracted);
    } else {
        n = snprintf(cmd, sizeof(cmd),
                     "%s \"%s\" \"%s\" -o \"%s\" -lpthread -lm %s %s",
                     cc, o_path, runtime_o, bin_path, strip, extracted);
    }
    if (n < 0 || (size_t)n >= sizeof(cmd)) return -1;
    if (shadow_host_is_tcc(cc) && !shadow_append_tcc_B(cmd, sizeof(cmd), cc)) {
        fprintf(stderr,
                "error: host TCC '%s' needs -B (missing third_party/tcc)\n",
                cc);
        return -1;
    }
    if (g_shadow_host_opts.dry_run) {
        fprintf(stderr, "shadow_lower: dry-run link:\n  %s\n", cmd);
        return 0;
    }
    if (g_shadow_host_opts.verbose)
        fprintf(stderr, "shadow_lower: %s\n", cmd);
    if (system(cmd) != 0) {
        fprintf(stderr, "error: host link failed\n  %s\n", cmd);
        return -1;
    }
    return 0;
}

/* Capture fd 2 around cold emit so warnings can be persisted beside emit.c.
 * Process-local slot keeps the type out of shadow_lower.ccs (type-pass TCC). */
static int g_shadow_diag_saved_err = -1;
static int g_shadow_diag_cap_fd = -1;
static char g_shadow_diag_tmp[640];

static int shadow_diag_cap_begin(void) {
    char tmpl[] = "/tmp/shadow_diag.XXXXXX";
    int fd;
    if (g_shadow_diag_saved_err >= 0) return -1; /* nested */
    g_shadow_diag_cap_fd = -1;
    g_shadow_diag_tmp[0] = 0;
    fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    snprintf(g_shadow_diag_tmp, sizeof(g_shadow_diag_tmp), "%s", tmpl);
    fflush(stderr);
    g_shadow_diag_saved_err = dup(2);
    if (g_shadow_diag_saved_err < 0 || dup2(fd, 2) < 0) {
        if (g_shadow_diag_saved_err >= 0) close(g_shadow_diag_saved_err);
        close(fd);
        unlink(tmpl);
        g_shadow_diag_saved_err = -1;
        return -1;
    }
    g_shadow_diag_cap_fd = fd;
    return 0;
}

/* Restore stderr, echo capture, optionally return malloc'd bytes (caller frees).
 * Returns capture length (>=0), or -1 if capture was not active. */
static long shadow_diag_cap_end(char** out_buf, size_t* out_len) {
    long cap_len = 0;
    char* buf = NULL;
    if (out_buf) *out_buf = NULL;
    if (out_len) *out_len = 0;
    if (g_shadow_diag_saved_err < 0) return -1;
    fflush(stderr);
    dup2(g_shadow_diag_saved_err, 2);
    close(g_shadow_diag_saved_err);
    g_shadow_diag_saved_err = -1;
    if (lseek(g_shadow_diag_cap_fd, 0, SEEK_SET) == 0) {
        char chunk[4096];
        ssize_t n;
        size_t alloc = 0, used = 0;
        while ((n = read(g_shadow_diag_cap_fd, chunk, sizeof(chunk))) > 0) {
            fwrite(chunk, 1, (size_t)n, stderr);
            if (out_buf) {
                if (used + (size_t)n + 1 > alloc) {
                    size_t na = alloc ? alloc * 2 : 4096;
                    char* nb;
                    while (na < used + (size_t)n + 1) na *= 2;
                    nb = (char*)realloc(buf, na);
                    if (!nb) {
                        free(buf);
                        buf = NULL;
                        used = 0;
                        alloc = 0;
                    } else {
                        buf = nb;
                        alloc = na;
                    }
                }
                if (buf) {
                    memcpy(buf + used, chunk, (size_t)n);
                    used += (size_t)n;
                }
            }
            cap_len += (long)n;
        }
        if (buf) {
            buf[used] = 0;
            if (out_buf) *out_buf = buf;
            if (out_len) *out_len = used;
        }
    }
    close(g_shadow_diag_cap_fd);
    g_shadow_diag_cap_fd = -1;
    if (g_shadow_diag_tmp[0]) unlink(g_shadow_diag_tmp);
    g_shadow_diag_tmp[0] = 0;
    return cap_len;
}

/* Warm-cache diagnostic replay (ccache-style).  Lowering warnings are
 * fprintf(stderr) during cold emit; bytes are persisted as emit.c.diag and
 * replayed on hit so warm builds print the same diagnostics as cold ones. */
static int shadow_diag_sidecar_path(const char* c_path, char* out, size_t cap) {
    int n;
    if (!out || cap == 0) return -1;
    out[0] = 0;
    if (!c_path) return -1;
    n = snprintf(out, cap, "%s.diag", c_path);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = 0;
        return -1;
    }
    return 0;
}

static void shadow_replay_diag_sidecar(const char* c_path) {
    char path[640];
    FILE* f;
    char buf[4096];
    size_t n;
    if (shadow_diag_sidecar_path(c_path, path, sizeof(path)) != 0) return;
    f = fopen(path, "rb");
    if (!f) return;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        fwrite(buf, 1, n, stderr);
    fclose(f);
}

static void shadow_write_diag_sidecar(const char* c_path, const char* diag,
                                      size_t diag_len) {
    char path[640];
    if (shadow_diag_sidecar_path(c_path, path, sizeof(path)) != 0) return;
    if (!diag || diag_len == 0) {
        unlink(path);
        return;
    }
    (void)shadow_write_file(path, diag, diag_len);
}

/* True if emit.c is cached for this input (skip parse/emit).
 * Also requires emit.deps fingerprint still match (transitive headers /
 * harvested .cch / cc_depends). */
static int shadow_emit_cache_hit(const char* in_path, const char* self_path,
                                 int no_cache, char* c_path_out, size_t c_cap,
                                 uint64_t* ekey_out) {
    uint64_t ekey, prev;
    char dir[512], ekey_path[600];
    if (shadow_cache_disabled(no_cache)) return 0;
    ekey = shadow_emit_cache_key(in_path, self_path);
    shadow_cache_dir(ekey, dir, sizeof(dir));
    snprintf(c_path_out, c_cap, "%s/emit.c", dir);
    snprintf(ekey_path, sizeof(ekey_path), "%s/emit.key", dir);
    if (!shadow_file_exists(c_path_out)) return 0;
    if (shadow_read_u64_file(ekey_path, &prev) != 0 || prev != ekey) return 0;
    if (!shadow_emit_deps_match(dir)) return 0;
    if (ekey_out) *ekey_out = ekey;
    return 1;
}

/* Host build from emit buffer or existing cached emit.c.
 * If emit_buf is NULL, require a warm emit cache hit.
 * dep_paths: transitive inputs observed on the cold path (may be NULL/0
 * when reusing a prior sidecar).
 * diag_buf: lowering stderr captured on cold emit (warnings); written as
 * emit.c.diag and replayed on warm reuse. */
static int shadow_build_host(const char* in_path, const char* bin_path,
                             const char* emit_buf, size_t emit_len,
                             const char* self_path, int no_cache,
                             const char* const* dep_paths, int ndeps,
                             const char* diag_buf, size_t diag_len) {
    uint64_t ekey, okey, prev;
    char dir[512], c_path[600], o_path[600], ekey_path[600], okey_path[600];
    char runtime_o[512];
    int reuse_emit = 0;
    int cache_ok = !shadow_cache_disabled(no_cache);

    if (!in_path || !bin_path) return -1;
    runtime_o[0] = 0;
    if (!g_shadow_host_opts.no_runtime &&
        !shadow_runtime_o(runtime_o, sizeof(runtime_o)))
        return -1;

    ekey = shadow_emit_cache_key(in_path, self_path);
    okey = shadow_obj_cache_key(ekey, shadow_host_cc());
    shadow_cache_dir(ekey, dir, sizeof(dir));
    snprintf(c_path, sizeof(c_path), "%s/emit.c", dir);
    snprintf(o_path, sizeof(o_path), "%s/tu.o", dir);
    snprintf(ekey_path, sizeof(ekey_path), "%s/emit.key", dir);
    snprintf(okey_path, sizeof(okey_path), "%s/obj.key", dir);

    if (cache_ok && shadow_file_exists(c_path) &&
        shadow_read_u64_file(ekey_path, &prev) == 0 && prev == ekey &&
        shadow_emit_deps_match(dir)) {
        reuse_emit = 1;
        shadow_replay_diag_sidecar(c_path);
    } else {
        if (!emit_buf) {
            fprintf(stderr, "error: emit cache miss and no emit buffer\n");
            return -1;
        }
        if (shadow_mkdir_p(dir) != 0) {
            fprintf(stderr, "error: cannot mkdir %s\n", dir);
            return -1;
        }
        if (shadow_write_file(c_path, emit_buf, emit_len) != 0) {
            fprintf(stderr, "error: cannot write %s\n", c_path);
            return -1;
        }
        if (cache_ok) {
            (void)shadow_write_u64_file(ekey_path, ekey);
            if (dep_paths && ndeps > 0)
                (void)shadow_emit_deps_save(dir, dep_paths, ndeps);
            else
                (void)shadow_emit_deps_save(dir, NULL, 0);
            shadow_write_diag_sidecar(c_path, diag_buf, diag_len);
        } else {
            /* --no-cache: keep emit.c for this host compile, but do not
             * publish emit.key — a later cached run must not warm-hit a
             * no-sidecar entry left by a forced cold build. */
            unlink(ekey_path);
            shadow_write_diag_sidecar(c_path, NULL, 0);
        }
    }

    if (!(cache_ok && reuse_emit && shadow_file_exists(o_path) &&
          shadow_read_u64_file(okey_path, &prev) == 0 && prev == okey)) {
        if (shadow_mkdir_p(dir) != 0) {
            fprintf(stderr, "error: cannot mkdir %s\n", dir);
            return -1;
        }
        if (shadow_host_compile(c_path, o_path) != 0) return -1;
        (void)shadow_write_u64_file(okey_path, okey);
    }

    return shadow_host_link(o_path, bin_path, runtime_o, c_path, in_path);
}

/* True when -o path looks like a text product (.c / .h), not a binary. */
static int shadow_out_is_text(const char* path) {
    if (!path) return 0;
    if (shadow_build_ends_with(path, ".c") ||
        shadow_build_ends_with(path, ".h") ||
        shadow_build_ends_with(path, ".h") ||
        shadow_build_ends_with(path, ".ccs"))
        return 1;
    /* Goldens: `foo.c.golden` / `foo.h.golden` (not `*.golden` alone). */
    if (strstr(path, ".c.golden") || strstr(path, ".h.golden")) return 1;
    return 0;
}
