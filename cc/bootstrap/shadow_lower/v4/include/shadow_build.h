/* SERDES product build: emit/obj cache + host cc (ccc-compatible) + link.
 * Used by shadow_lower.ccs. Cache root: out/.cc-build/serdes/<fp>/. */
#pragma once

#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

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

/* Runtime object for host link. */
static int shadow_runtime_o(char* dst, size_t cap) {
    const char* env = getenv("SHADOW_RUNTIME_O");
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
    for (i = 0; i < (int)(sizeof(cands) / sizeof(cands[0])); i++) {
        if (access(cands[i], R_OK) == 0) {
            snprintf(dst, cap, "%s", cands[i]);
            return 1;
        }
    }
    fprintf(stderr, "error: missing concurrent_c.o (make -C cc)\n");
    return 0;
}

/* Emit key: input content + tool sig + include roots + comptime/tcc libs.
 * Transitive tapes / harvested .cch / `#pragma cc_depends` are not folded
 * here — they live in emit.deps (+ emit.deps.key) written on cold emit. */
static uint64_t shadow_emit_cache_key(const char* in_path, const char* self_path) {
    uint64_t h = 1469598103934665603ULL;
    ShadowFileSig in_sig = {0}, self_sig = {0};
    h = shadow_fnv1a64_str(h, in_path ? in_path : "");
    if (shadow_stat_sig(in_path, &in_sig) == 0) {
        h = shadow_fnv1a64_i64(h, in_sig.mtime_sec);
        h = shadow_fnv1a64_i64(h, in_sig.size);
    }
    h = shadow_fold_file_content(h, in_path);
    h = shadow_fnv1a64_str(h, self_path ? self_path : "");
    if (self_path && shadow_stat_sig(self_path, &self_sig) == 0) {
        h = shadow_fnv1a64_i64(h, self_sig.mtime_sec);
        h = shadow_fnv1a64_i64(h, self_sig.size);
    }
    h = shadow_fnv1a64_str(h, "out/include");
    h = shadow_fnv1a64_str(h, "cc/include");
    /* Comptime prepare/exec/splice identity (product path uses these libs). */
    h = shadow_fnv1a64_str(h, "\x03" "toolchain:");
    h = shadow_fold_file_content(h, "out/cc/obj/libshadow_comptime.a");
    h = shadow_fold_file_content(h, "third_party/tcc/libtcc.a");
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

/* Obj key: emit key + host cc id + flag fingerprint.
 * Must fold every host-compile input (cc_flags, release/debug, no_runtime)
 * so warm tu.o is never reused across incompatible builds. */
static uint64_t shadow_obj_cache_key(uint64_t emit_key, const char* host_cc) {
    uint64_t h = emit_key;
    ShadowFileSig cc_sig = {0};
    h = shadow_fnv1a64_str(h, host_cc ? host_cc : "cc");
    if (host_cc && host_cc[0] == '/' && shadow_stat_sig(host_cc, &cc_sig) == 0) {
        h = shadow_fnv1a64_i64(h, cc_sig.mtime_sec);
        h = shadow_fnv1a64_i64(h, cc_sig.size);
    }
    h = shadow_fnv1a64_str(h, getenv("CFLAGS"));
    h = shadow_fnv1a64_str(h, getenv("CPPFLAGS"));
    h = shadow_fnv1a64_str(h, "-std=c11-D_DEFAULT_SOURCE-ffunction-sections-fdata-sections");
    h = shadow_fnv1a64_str(h, g_shadow_host_opts.cc_flags);
    h = shadow_fnv1a64_str(h, g_shadow_host_opts.ld_flags);
    h = shadow_fnv1a64_i64(h, g_shadow_host_opts.opt_release ? 1 : 0);
    h = shadow_fnv1a64_i64(h, g_shadow_host_opts.opt_debug ? 1 : 0);
    h = shadow_fnv1a64_i64(h, g_shadow_host_opts.no_runtime ? 1 : 0);
    return h;
}

static void shadow_cache_dir(uint64_t emit_key, char* dst, size_t cap) {
    snprintf(dst, cap, "out/.cc-build/serdes/%016llx", (unsigned long long)emit_key);
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
    if (g_shadow_host_opts.dry_run) {
        fprintf(stderr, "shadow_lower: dry-run compile:\n  %s\n", cmd);
        return 0;
    }
    if (g_shadow_host_opts.verbose)
        fprintf(stderr, "shadow_lower: %s\n", cmd);
    if (system(cmd) != 0) {
        fprintf(stderr, "error: host compile failed\n  %s\n", cmd);
        return -1;
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
    int n;
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
                     "%s \"%s\" -o \"%s\" -lpthread -lm %s",
                     cc, o_path, bin_path, extracted);
    } else {
        n = snprintf(cmd, sizeof(cmd),
                     "%s \"%s\" \"%s\" -o \"%s\" -lpthread -lm %s",
                     cc, o_path, runtime_o, bin_path, extracted);
    }
    if (n < 0 || (size_t)n >= sizeof(cmd)) return -1;
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
 * when reusing a prior sidecar). */
static int shadow_build_host(const char* in_path, const char* bin_path,
                             const char* emit_buf, size_t emit_len,
                             const char* self_path, int no_cache,
                             const char* const* dep_paths, int ndeps) {
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
        (void)shadow_write_u64_file(ekey_path, ekey);
        if (dep_paths && ndeps > 0)
            (void)shadow_emit_deps_save(dir, dep_paths, ndeps);
        else
            (void)shadow_emit_deps_save(dir, NULL, 0);
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
