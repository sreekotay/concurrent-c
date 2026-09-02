#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

#include "build/build.h"
#include "build/host_cc_profile.h"
#include <ccc/cc_build_helpers.h>
#include "comptime/symbols.h"
#include "diag/diag.h"
#include "visitor/pass_common.h"
#include "preprocess/preprocess.h"
#include "preprocess/script_entry.h"
#include "preprocess/script_oneliner.h"
#include "preprocess/unit_header.h"
#include "comptime/const_eval.h"
#include "cccportable.h"

/* The legacy multipass front (driver.c / driver.h) has been removed; ccc is
 * native-only (shadow_lower). This typedef used to live in driver.h and only
 * carries build.cc-preloaded comptime consts through the driver. */
typedef struct {
    const CCConstBinding* consts;
    size_t const_count;
} CCCompileConfig;

// Forward decls for helpers used by multiple modes.
static int file_exists(const char* path);
static int ensure_out_dir(void);
static int cc__selftest_const_eval(int argc, char** argv);
static void cc__stem_from_path(const char* path, char* out, size_t cap);
static int run_build_mode(int argc, char** argv);
static char* cc__read_all_file(const char* path, size_t* out_len);
static const char* cc__version_string(void);
static int cc__write_file_bytes(const char* path, const char* data, size_t len);
static int cc__mkdir_p(const char* path);
static int cc__prof_on(void) {
    static int once = 0, on = 0;
    if (!once) {
        const char* e = getenv("CC_CCC_PROFILE");
        on = e && e[0] && e[0] != '0';
        once = 1;
    }
    return on;
}
static long long cc__now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
static const char* cc__prof_base(const char* path) {
    const char* s;
    if (!path || !path[0]) return "";
    s = strrchr(path, '/');
    return s ? s + 1 : path;
}
static void cc__prof_span(const char* name, long long t0) {
    if (!cc__prof_on()) return;
    {
        long long now = cc__now_ms();
        fprintf(stderr, "ccc_profile: %-24s %5lld ms  wall=%lld\n",
                name, now - t0, now);
    }
}
static void cc__prof_span_arg(const char* name, const char* arg, long long t0) {
    if (!cc__prof_on()) return;
    {
        long long now = cc__now_ms();
        fprintf(stderr, "ccc_profile: %-24s %5lld ms  wall=%lld  %s\n",
                name, now - t0, now, cc__prof_base(arg));
    }
}
static int cc__take_unit_flag(int argc, char** argv, int* i,
                              CCUnitKind* as_kind, char* pin, size_t pin_cap);

// `--emit-c-inspect[=PATH]`: dump the merged translation unit for inspection.
// On a clean build it is the full pre-parse merged TU; on a build that fails in
// a generic factory it is the reconstructed TU up to the first blocking error.
// Communicated to the lowering layer via the CC_EMIT_C_INSPECT env (set per
// input in cc__compile_with_env), so no signatures need widening.
static int g_emit_c_inspect = 0;
static const char* g_emit_c_inspect_path = NULL;
/* When set, `build run` uses this as the child argv[0] (exec path unchanged). */
static const char* g_run_argv0 = NULL;

// Resolved repo-relative paths so `./cc/bin/ccc build ...` works from the repo root.
static int g_paths_inited = 0;
/* Set when paths resolved to a prefix install rather than a checkout.
 * The prefix is read-only as far as the driver is concerned (it may be
 * /usr/local, owned by root). Build outputs are always cwd-relative;
 * this flag still selects toolchain layout (includes, runtime, lowerer). */
static int g_layout_installed = 0;
static int g_no_line = 0;
static const char* g_cccportable_dir = NULL;
static int g_cccportable_cli = 0;
static char g_repo_root[PATH_MAX];
static char g_ccc_path[PATH_MAX];
static char g_ccc_sig_path[PATH_MAX];
static char g_cc_dir[PATH_MAX];
static char g_cc_include[PATH_MAX];
static char g_cc_lowered_include[PATH_MAX];  /* out/include with lowered .h headers */
static char g_cc_runtime_o[PATH_MAX];
static char g_cc_runtime_c[PATH_MAX];
static char g_out_root[PATH_MAX];
static char g_bin_root[PATH_MAX];
static char g_cache_root[PATH_MAX];
/* Host-native .o/.d/runtime + compile/link metas: <cache>/host/<fp>/.
 * Lowered headers stay at <repo>/out/include (shared, host-agnostic). */
static char g_host_fp[17];
static char g_host_obj_root[PATH_MAX];

static void cc__dirname_inplace(char* path) {
    if (!path) return;
    size_t n = strlen(path);
    if (n == 0) return;
    // Strip trailing slashes.
    while (n > 0 && path[n - 1] == '/') {
        path[n - 1] = '\0';
        n--;
    }
    if (n == 0) return;
    char* slash = strrchr(path, '/');
    if (!slash) {
        path[0] = '\0';
        return;
    }
    if (slash == path) {
        // Keep root.
        slash[1] = '\0';
        return;
    }
    *slash = '\0';
}

static int cc__is_abs_path(const char* p) {
    if (!p || !p[0]) return 0;
    return p[0] == '/';
}

static int cc__ends_with(const char* s, const char* suf) {
    if (!s || !suf) return 0;
    size_t n = strlen(s), m = strlen(suf);
    if (m > n) return 0;
    return memcmp(s + (n - m), suf, m) == 0;
}

static int cc__path_is_dir(const char* path) {
    struct stat st;
    if (!path || !path[0]) return 0;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int cc__looks_like_dev_repo_root(const char* path) {
    char include_dir[PATH_MAX];
    char runtime_src[PATH_MAX];
    if (!path || !path[0]) return 0;
    snprintf(include_dir, sizeof(include_dir), "%s/cc/include/ccc", path);
    snprintf(runtime_src, sizeof(runtime_src), "%s/cc/runtime/concurrent_c.c", path);
    return cc__path_is_dir(include_dir) && access(runtime_src, F_OK) == 0;
}

static int cc__search_up_for_dev_repo_root(const char* start_path, char* out, size_t out_sz) {
    char cur[PATH_MAX];
    if (!start_path || !start_path[0] || !out || out_sz == 0) return -1;
    if (realpath(start_path, cur) == NULL) {
        strncpy(cur, start_path, sizeof(cur));
        cur[sizeof(cur) - 1] = '\0';
    }
    while (cur[0]) {
        if (cc__looks_like_dev_repo_root(cur)) {
            strncpy(out, cur, out_sz);
            out[out_sz - 1] = '\0';
            return 0;
        }
        char parent[PATH_MAX];
        strncpy(parent, cur, sizeof(parent));
        parent[sizeof(parent) - 1] = '\0';
        cc__dirname_inplace(parent);
        if (!parent[0] || strcmp(parent, cur) == 0) break;
        strncpy(cur, parent, sizeof(cur));
        cur[sizeof(cur) - 1] = '\0';
    }
    return -1;
}

// Detect if the compiler is TCC (doesn't support -MMD/-MF/-MT or section flags)
static int cc__is_tcc(const char* cc_bin) {
    if (!cc_bin) return 0;
    // Check if path ends with "tcc" or contains "/tcc"
    if (cc__ends_with(cc_bin, "tcc")) return 1;
    if (cc__ends_with(cc_bin, "tcc.exe")) return 1;
    if (strstr(cc_bin, "/tcc") || strstr(cc_bin, "\\tcc")) return 1;
    return 0;
}

/* Resolve the vendored TCC install dir (holds include/ + libtcc1.a). Uninstalled
 * builds have CONFIG_TCCDIR=/usr/local/lib/tcc, so host `CC=…/tcc` needs `-B`
 * pointing here or system headers like stdbool.h are missing. */
static const char* cc__tcc_lib_dir(const char* cc_bin, char* buf, size_t cap) {
    const char* cands[8];
    size_t nc = 0;
    char from_bin[PATH_MAX];
    const char* env = getenv("CC_TCC_LIB_PATH");
    if (env && env[0]) cands[nc++] = env;
#ifdef CC_TCC_LIB_DIR
    cands[nc++] = CC_TCC_LIB_DIR;
#endif
    if (cc_bin && cc_bin[0] && cc__is_tcc(cc_bin)) {
        /* dirname(cc_bin) when the binary lives next to include/ / libtcc1.a */
        size_t n = strlen(cc_bin);
        if (n + 1 < sizeof(from_bin)) {
            memcpy(from_bin, cc_bin, n + 1);
            char* slash = strrchr(from_bin, '/');
            if (slash && slash != from_bin) {
                *slash = '\0';
                cands[nc++] = from_bin;
            }
        }
    }
    cands[nc++] = "third_party/tcc";
    cands[nc++] = "../third_party/tcc";
    if (g_repo_root[0]) {
        static char abs_cand[PATH_MAX];
        if (snprintf(abs_cand, sizeof(abs_cand), "%s/third_party/tcc", g_repo_root) < (int)sizeof(abs_cand))
            cands[nc++] = abs_cand;
    }
    for (size_t i = 0; i < nc; i++) {
        char probe[PATH_MAX];
        if (!cands[i] || !cands[i][0]) continue;
        if (snprintf(probe, sizeof(probe), "%s/include/stdbool.h", cands[i]) >= (int)sizeof(probe))
            continue;
        if (access(probe, R_OK) == 0) {
            if (snprintf(buf, cap, "%s", cands[i]) < (int)cap) return buf;
        }
    }
    return NULL;
}

/* Host flags from the persisted CC profile (probe-once under out/.cc-build/host/). */
static int cc__host_profile_for(const char* cc_bin, CCHostCcProfile* out) {
    if (!out) return -1;
    if (cc_host_cc_profile_ensure(cc_bin, g_cache_root[0] ? g_cache_root : "out/.cc-build",
                                  g_repo_root, out) != 0) {
        memset(out, 0, sizeof(*out));
        /* Fall back to name-based TCC detection so a probe failure is not fatal
         * for clang hosts that somehow failed the probe workspace. */
        out->is_tcc = cc__is_tcc(cc_bin);
        out->ok = 0;
        return -1;
    }
    return 0;
}

static void cc__append_host_cc_flags(char* cmd, size_t cmd_cap, const char* cc_bin) {
    CCHostCcProfile prof;
    if (!cmd || !cmd_cap) return;
    if (cc__host_profile_for(cc_bin, &prof) != 0) {
        /* Legacy TCC path if profile missing. */
        char dir[PATH_MAX];
        const char* libdir;
        if (!cc__is_tcc(cc_bin)) return;
        strncat(cmd, " " CC_TCC_HOST_OPTIONS, cmd_cap - strlen(cmd) - 1);
        libdir = cc__tcc_lib_dir(cc_bin, dir, sizeof(dir));
        if (libdir) {
            char flag[PATH_MAX + 4];
            snprintf(flag, sizeof(flag), " -B%s", libdir);
            strncat(cmd, flag, cmd_cap - strlen(cmd) - 1);
        }
        return;
    }
    cc_host_cc_profile_append_flags(&prof, cmd, cmd_cap);
}

/* Back-compat name used at call sites before the profile existed. */
static void cc__append_tcc_host_flags(char* cmd, size_t cmd_cap, const char* cc_bin) {
    cc__append_host_cc_flags(cmd, cmd_cap, cc_bin);
}

static void cc__append_flag(char* buf, size_t cap, const char* prefix, const char* val) {
    if (!buf || cap == 0 || !val || !val[0]) return;
    if (prefix && prefix[0]) {
        strncat(buf, prefix, cap - strlen(buf) - 1);
    }
    strncat(buf, val, cap - strlen(buf) - 1);
}

static void cc__append_spaced(char* buf, size_t cap, const char* val) {
    if (!buf || cap == 0 || !val || !val[0]) return;
    if (buf[0]) strncat(buf, " ", cap - strlen(buf) - 1);
    strncat(buf, val, cap - strlen(buf) - 1);
}

static int cc__mkdir_one(const char* path) {
    if (!path || !path[0]) return -1;
    if (mkdir(path, 0777) == -1) {
        if (errno == EEXIST) return 0;
        return -1;
    }
    return 0;
}

static int cc__mkdir_p(const char* path) {
    if (!path || !path[0]) return -1;
    char tmp[PATH_MAX];
    strncpy(tmp, path, sizeof(tmp));
    tmp[sizeof(tmp) - 1] = '\0';

    // Strip trailing slashes.
    size_t n = strlen(tmp);
    while (n > 0 && tmp[n - 1] == '/') {
        tmp[n - 1] = '\0';
        n--;
    }
    if (n == 0) return 0;

    // Walk components.
    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (cc__mkdir_one(tmp) != 0) return -1;
            *p = '/';
        }
    }
    if (cc__mkdir_one(tmp) != 0) return -1;
    return 0;
}

typedef struct {
    long long mtime_sec;
    long long size;
} CCFileSig;

static int cc__stat_sig(const char* path, CCFileSig* out) {
    if (!out) return -1;
    out->mtime_sec = 0;
    out->size = 0;
    if (!path || !path[0]) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    out->mtime_sec = (long long)st.st_mtime;
    out->size = (long long)st.st_size;
    return 0;
}

static uint64_t cc__fnv1a64_update(uint64_t h, const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t cc__fnv1a64_str(uint64_t h, const char* s) {
    if (!s) s = "";
    return cc__fnv1a64_update(h, s, strlen(s));
}

static uint64_t cc__fnv1a64_i64(uint64_t h, long long v) {
    return cc__fnv1a64_update(h, &v, sizeof(v));
}

static uint64_t cc__hash_build_dir_u64(const char* build_dir) {
    // Use realpath when possible to make this stable regardless of CWD / symlinks.
    char rp[PATH_MAX];
    rp[0] = '\0';
    if (build_dir && build_dir[0]) {
        if (realpath(build_dir, rp) == NULL) {
            strncpy(rp, build_dir, sizeof(rp));
            rp[sizeof(rp) - 1] = '\0';
        }
    }
    uint64_t h = 1469598103934665603ULL;
    h = cc__fnv1a64_str(h, rp);
    return h;
}

static uint64_t cc__hash_src_path_u64(const char* build_dir, const char* src_abs) {
    // Prefer a build-relative identity when src is under build_dir (nice + stable),
    // else fall back to the absolute path.
    if (!src_abs) src_abs = "";
    if (!build_dir) build_dir = "";
    size_t bd_len = strlen(build_dir);
    const char* p = src_abs;
    if (bd_len && strncmp(src_abs, build_dir, bd_len) == 0 && src_abs[bd_len] == '/') {
        p = src_abs + bd_len + 1; // rel
    }
    uint64_t h = 1469598103934665603ULL;
    h = cc__fnv1a64_str(h, p);
    return h;
}

static void cc__format_u64_hex(char* out, size_t cap, uint64_t v) {
    if (!out || cap == 0) return;
    snprintf(out, cap, "%016llx", (unsigned long long)v);
}

static int cc__read_u64_file(const char* path, uint64_t* out) {
    if (!out) return -1;
    *out = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    unsigned long long v = 0;
    int ok = fscanf(f, "%llu", &v);
    fclose(f);
    if (ok != 1) return -1;
    *out = (uint64_t)v;
    return 0;
}

static int cc__write_u64_file(const char* path, uint64_t v) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "%llu\n", (unsigned long long)v);
    fclose(f);
    return 0;
}

static int cc__cache_disabled(int opt_no_cache) {
    if (opt_no_cache) return 1;
    const char* env = getenv("CC_NO_CACHE");
    return (env && env[0] == '1');
}

static void cc__cache_key_paths(char* out_meta, size_t meta_cap,
                                char* out_link, size_t link_cap,
                                const char* stem) {
    if (!stem) stem = "unknown";
    /* Emit .meta is host-agnostic (shared .c). Link meta is host-native. */
    if (out_meta && meta_cap) snprintf(out_meta, meta_cap, "%s/%s.meta", g_cache_root, stem);
    if (out_link && link_cap) {
        const char* root = g_host_obj_root[0] ? g_host_obj_root : g_cache_root;
        snprintf(out_link, link_cap, "%s/%s.link", root, stem);
    }
}

static const char* pick_cc_bin(const char* override);

/* Isolate host-native compile/link products under <cache>/host/<fp>/. */
static void cc_refresh_host_obj_root(const char* cc_bin_override) {
    const char* cc = pick_cc_bin(cc_bin_override);
    const char* cache = g_cache_root[0] ? g_cache_root : "out/.cc-build";
    if (cc_host_cc_fingerprint(cc, g_host_fp, sizeof(g_host_fp)) != 0) {
        snprintf(g_host_fp, sizeof(g_host_fp), "unresolved");
        snprintf(g_host_obj_root, sizeof(g_host_obj_root), "%s/host/%s", cache, g_host_fp);
        return;
    }
    if (cc_host_cc_obj_root(cc, cache, g_host_obj_root, sizeof(g_host_obj_root)) != 0) {
        snprintf(g_host_obj_root, sizeof(g_host_obj_root), "%s/host/%s", cache, g_host_fp);
    }
}

static void cc_set_out_dir(const char* out_dir_opt, const char* bin_dir_opt) {
    /* Default and relative --out-dir / --bin-dir are always cwd. Toolchain
     * files (includes, runtime, shadow_lower) still come from g_repo_root. */
    char base[PATH_MAX];
    if (getcwd(base, sizeof(base)) == NULL) {
        strncpy(base, ".", sizeof(base));
        base[sizeof(base) - 1] = '\0';
    }

    const char* env = getenv("CC_OUT_DIR");
    const char* p = out_dir_opt && out_dir_opt[0] ? out_dir_opt : (env && env[0] ? env : NULL);

    if (!p) {
        snprintf(g_out_root, sizeof(g_out_root), "%s/out", base);
    } else if (cc__is_abs_path(p)) {
        strncpy(g_out_root, p, sizeof(g_out_root));
        g_out_root[sizeof(g_out_root) - 1] = '\0';
    } else {
        // Relative paths are interpreted relative to the base dir.
        snprintf(g_out_root, sizeof(g_out_root), "%s/%s", base, p);
    }

    const char* benv = getenv("CC_BIN_DIR");
    const char* bp = bin_dir_opt && bin_dir_opt[0] ? bin_dir_opt : (benv && benv[0] ? benv : NULL);
    if (!bp) {
        snprintf(g_bin_root, sizeof(g_bin_root), "%s/bin", base);
    } else if (cc__is_abs_path(bp)) {
        strncpy(g_bin_root, bp, sizeof(g_bin_root));
        g_bin_root[sizeof(g_bin_root) - 1] = '\0';
    } else {
        // Relative bin dir is interpreted relative to the base dir.
        snprintf(g_bin_root, sizeof(g_bin_root), "%s/%s", base, bp);
    }

    snprintf(g_cache_root, sizeof(g_cache_root), "%s/.cc-build", g_out_root);
    cc_refresh_host_obj_root(NULL);
}

/* Shebang / `ccc --as=shcc` scripts are not the product. Keep their
 * compile artifacts out of the project's `out/` so `rm -rf out` does
 * not rebuild make.shcc on every true-cold app build. */
static int cc__input_is_shcc(CCUnitKind unit_kind, const char* path) {
    CCUnitKind k = unit_kind;
    char pin[CC_CCC_VERSION_PIN_CAP];
    char err[256];
    pin[0] = '\0';
    if (k == CC_UNIT_KIND_UNKNOWN && path && path[0]) {
        if (cc_unit_resolve(path, unit_kind, NULL, &k, pin, err, sizeof(err)) != 0)
            return 0;
    }
    return k == CC_UNIT_KIND_SHCC;
}

static int cc__use_script_cache_dirs(void) {
    const char* tmp = getenv("TMPDIR");
    char root[PATH_MAX];
    if (!tmp || !tmp[0]) tmp = "/tmp";
    if (snprintf(root, sizeof(root), "%s/cc-script-%ld", tmp,
                 (long)getuid()) >= (int)sizeof(root))
        return -1;
    snprintf(g_out_root, sizeof(g_out_root), "%s/out", root);
    snprintf(g_bin_root, sizeof(g_bin_root), "%s/bin", root);
    snprintf(g_cache_root, sizeof(g_cache_root), "%s/.cc-build", g_out_root);
    return 0;
}

// Check if a path layout is valid (runtime source exists).
static int cc__check_layout(const char* include_path, const char* runtime_path) {
    char marker[PATH_MAX];
    snprintf(marker, sizeof(marker), "%s/concurrent_c.c", runtime_path);
    if (file_exists(marker)) {
        // Also verify include dir has ccc/cc_runtime.cch.
        snprintf(marker, sizeof(marker), "%s/ccc/cc_runtime.cch", include_path);
        return file_exists(marker);
    }
    return 0;
}

/* Resolve a bare argv[0] (no slash) to an absolute path via PATH. After a
 * prefix install `ccc` is normally invoked by bare name, and the layout probe
 * below derives <prefix> from <prefix>/bin/ccc — so without this the probe
 * never fires and a correct install looks broken. `out` must hold PATH_MAX. */
static int cc__resolve_on_path(const char* name, char* out) {
    const char* path = getenv("PATH");
    if (!name || !name[0] || !path || !path[0]) return -1;
    char* dup = strdup(path);
    if (!dup) return -1;
    int rc = -1;
    char* p = dup;
    while (p && *p) {
        char* sep = strchr(p, ':');
        if (sep) *sep = '\0';
        char cand[PATH_MAX];
        if (*p && (size_t)snprintf(cand, sizeof(cand), "%s/%s", p, name) < sizeof(cand) &&
            access(cand, X_OK) == 0 && realpath(cand, out) != NULL) {
            rc = 0;
            break;
        }
        p = sep ? sep + 1 : NULL;
    }
    free(dup);
    return rc;
}

static void cc_init_paths(const char* argv0) {
    if (g_paths_inited) return;
    g_paths_inited = 1;

    char exe_abs[PATH_MAX];
    exe_abs[0] = '\0';
    if (argv0 && argv0[0]) {
        // Best effort: if argv0 is a path (common dev case: ./cc/bin/ccc), realpath it.
        if (strchr(argv0, '/') == NULL) {
            if (cc__resolve_on_path(argv0, exe_abs) != 0)
                exe_abs[0] = '\0';
        } else if (realpath(argv0, exe_abs) == NULL) {
            // Fallback: accept argv0 as-is.
            strncpy(exe_abs, argv0, sizeof(exe_abs));
            exe_abs[sizeof(exe_abs) - 1] = '\0';
        }
    }

    strncpy(g_ccc_path, exe_abs[0] ? exe_abs : (argv0 ? argv0 : ""), sizeof(g_ccc_path));
    g_ccc_path[sizeof(g_ccc_path) - 1] = '\0';

    // ------------------------------------------------------------------
    // Path resolution priority:
    //   1. CC_HOME env var (explicit override for custom installations)
    //   2. Installed layout: <prefix>/bin/ccc with <prefix>/lib/ccc and <prefix>/include/ccc
    //   3. Dev layout: <repo>/cc/bin/ccc or <repo>/out/cc/bin/ccc (fallback for development)
    // ------------------------------------------------------------------

    int layout_found = 0;
    char tmp[PATH_MAX];

    // Priority 1: CC_HOME environment variable.
    const char* cc_home = getenv("CC_HOME");
    if (cc_home && cc_home[0]) {
        char inc_check[PATH_MAX], rt[PATH_MAX];
        snprintf(inc_check, sizeof(inc_check), "%s/include", cc_home);
        snprintf(rt, sizeof(rt), "%s/lib/ccc/runtime", cc_home);
        if (cc__check_layout(inc_check, rt)) {
            strncpy(g_repo_root, cc_home, sizeof(g_repo_root));
            g_repo_root[sizeof(g_repo_root) - 1] = '\0';
            // g_cc_include points to parent of ccc/ so -I enables <ccc/...> includes
            snprintf(g_cc_include, sizeof(g_cc_include), "%s/include", cc_home);
            // In installed layout, lowered headers are in the same dir (pre-lowered during install)
            snprintf(g_cc_lowered_include, sizeof(g_cc_lowered_include), "%s/include", cc_home);
            snprintf(g_cc_runtime_c, sizeof(g_cc_runtime_c), "%s/concurrent_c.c", rt);
            // No prebuilt runtime .o in installed layout; will compile from source.
            g_cc_runtime_o[0] = '\0';
            snprintf(g_cc_dir, sizeof(g_cc_dir), "%s/lib/ccc", cc_home);
            layout_found = 1;
            g_layout_installed = 1;
        }
    }

    // Priority 2: Installed layout (<prefix>/bin/ccc -> <prefix>/lib/ccc, <prefix>/include/ccc).
    if (!layout_found && exe_abs[0]) {
        strncpy(tmp, exe_abs, sizeof(tmp));
        tmp[sizeof(tmp) - 1] = '\0';
        cc__dirname_inplace(tmp); // <prefix>/bin
        cc__dirname_inplace(tmp); // <prefix>

        char inc_check[PATH_MAX], rt[PATH_MAX];
        snprintf(inc_check, sizeof(inc_check), "%s/include", tmp);
        snprintf(rt, sizeof(rt), "%s/lib/ccc/runtime", tmp);
        if (cc__check_layout(inc_check, rt)) {
            strncpy(g_repo_root, tmp, sizeof(g_repo_root));
            g_repo_root[sizeof(g_repo_root) - 1] = '\0';
            // g_cc_include points to parent of ccc/ so -I enables <ccc/...> includes
            snprintf(g_cc_include, sizeof(g_cc_include), "%s/include", tmp);
            // In installed layout, lowered headers are in the same dir (pre-lowered during install)
            snprintf(g_cc_lowered_include, sizeof(g_cc_lowered_include), "%s/include", tmp);
            snprintf(g_cc_runtime_c, sizeof(g_cc_runtime_c), "%s/concurrent_c.c", rt);
            g_cc_runtime_o[0] = '\0';
            snprintf(g_cc_dir, sizeof(g_cc_dir), "%s/lib/ccc", tmp);
            layout_found = 1;
            g_layout_installed = 1;
        }
    }

    // Priority 3: Dev layout (<repo>/cc/bin/ccc or <repo>/out/cc/bin/ccc).
    if (!layout_found) {
        strncpy(tmp, exe_abs[0] ? exe_abs : "", sizeof(tmp));
        tmp[sizeof(tmp) - 1] = '\0';

        const char* suf1 = "/cc/bin/ccc";
        const char* suf2 = "/out/cc/bin/ccc";
        // Back-compat for older wrapper names.
        const char* suf1_old = "/cc/bin/cc";
        const char* suf2_old = "/out/cc/bin/cc";
        char* cut = NULL;
        if (tmp[0]) {
            char* p2 = strstr(tmp, suf2);
            if (p2) cut = p2;
            else {
                char* p1 = strstr(tmp, suf1);
                if (p1) cut = p1;
                else {
                    char* p2o = strstr(tmp, suf2_old);
                    if (p2o) cut = p2o;
                    else {
                        char* p1o = strstr(tmp, suf1_old);
                        if (p1o) cut = p1o;
                    }
                }
            }
        }
        if (cut) {
            *cut = '\0';
        } else {
            // Fallback: old heuristic (dirname thrice).
            cc__dirname_inplace(tmp); // .../bin
            cc__dirname_inplace(tmp); // .../cc
            cc__dirname_inplace(tmp); // repo root
        }

        if (!cc__looks_like_dev_repo_root(tmp)) {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd)) != NULL) {
                if (cc__search_up_for_dev_repo_root(cwd, tmp, sizeof(tmp)) != 0) {
                    tmp[0] = '\0';
                }
            } else {
                tmp[0] = '\0';
            }
        }

        if (!tmp[0]) {
            // Final fallback: assume current working directory is the repo root.
            if (getcwd(tmp, sizeof(tmp)) == NULL) {
                strncpy(tmp, ".", sizeof(tmp));
                tmp[sizeof(tmp) - 1] = '\0';
            }
        }

        strncpy(g_repo_root, tmp, sizeof(g_repo_root));
        g_repo_root[sizeof(g_repo_root) - 1] = '\0';

        snprintf(g_cc_dir, sizeof(g_cc_dir), "%s/cc", g_repo_root);
        snprintf(g_cc_include, sizeof(g_cc_include), "%s/cc/include", g_repo_root);
        // Lowered headers (.h versions of .cch) live under out/include
        snprintf(g_cc_lowered_include, sizeof(g_cc_lowered_include), "%s/out/include", g_repo_root);
        // Prefer the compiler-build runtime object (built by `make -C cc`) which now lives under out/.
        snprintf(g_cc_runtime_o, sizeof(g_cc_runtime_o), "%s/out/cc/obj/runtime/concurrent_c.o", g_repo_root);
        if (access(g_cc_runtime_o, F_OK) != 0) {
            snprintf(g_cc_runtime_o, sizeof(g_cc_runtime_o),
                     "%s/out/cc-tcc/obj/runtime/concurrent_c.o", g_repo_root);
        }
        /* Prefer rewritten runtime (ccc wrapper rewrites .cch includes to .h).
         * Compiling cc/runtime sources directly feeds host-cc raw @as sugar. */
        snprintf(g_cc_runtime_c, sizeof(g_cc_runtime_c),
                 "%s/out/runtime/concurrent_c.c", g_repo_root);
        if (access(g_cc_runtime_c, F_OK) != 0) {
            snprintf(g_cc_runtime_c, sizeof(g_cc_runtime_c),
                     "%s/cc/runtime/concurrent_c.c", g_repo_root);
        }
    }

    // Fingerprint the REAL compiler binary for emit/obj cache keys.
    // `cc/bin/ccc` is a thin shell wrapper that almost never changes on rebuild;
    // make updates `cc/bin/.ccc-bin`. Keying the wrapper made caches survive
    // lowering changes (e.g. #105 #line resync) and serve stale .c outputs.
    // Installed `ccc` *is* .ccc-bin; do not walk $prefix/cc/bin/.ccc-bin (miss)
    // and do not pick up an app-cwd leftover.
    if (g_layout_installed && g_ccc_path[0]) {
        strncpy(g_ccc_sig_path, g_ccc_path, sizeof(g_ccc_sig_path));
        g_ccc_sig_path[sizeof(g_ccc_sig_path) - 1] = '\0';
    } else {
        snprintf(g_ccc_sig_path, sizeof(g_ccc_sig_path), "%s/cc/bin/.ccc-bin", g_repo_root);
        if (!file_exists(g_ccc_sig_path)) {
            snprintf(g_ccc_sig_path, sizeof(g_ccc_sig_path), "%s/out/cc/bin/.ccc-bin", g_repo_root);
            if (!file_exists(g_ccc_sig_path)) {
                snprintf(g_ccc_sig_path, sizeof(g_ccc_sig_path), "%s/cc/bin/ccc", g_repo_root);
                if (!file_exists(g_ccc_sig_path)) {
                    snprintf(g_ccc_sig_path, sizeof(g_ccc_sig_path), "%s/out/cc/bin/ccc", g_repo_root);
                    if (!file_exists(g_ccc_sig_path)) {
                        strncpy(g_ccc_sig_path, g_ccc_path, sizeof(g_ccc_sig_path));
                        g_ccc_sig_path[sizeof(g_ccc_sig_path) - 1] = '\0';
                    }
                }
            }
        }
    }
    cc_set_out_dir(NULL, NULL);

    /* Locate TCC's support dir (include/ with stddef.h et al, plus libtcc1.a).
     * cc_init_parser_state in libtcc only reaches it through cwd-relative
     * candidates, which hold in a checkout and nowhere else, so publish an
     * absolute path here. CC_TCC_LIB_PATH is the override every consumer
     * already probes first (cc__tcc_lib_dir, comptime const-eval/executor,
     * hook_compile), so one setenv covers all of them. */
    if (!getenv("CC_TCC_LIB_PATH")) {
        const char* tcc_cands[2];
        char installed[PATH_MAX], in_repo[PATH_MAX];
        size_t ntcc = 0;
        if (g_cc_dir[0] &&
            (size_t)snprintf(installed, sizeof(installed), "%s/tcc", g_cc_dir) < sizeof(installed))
            tcc_cands[ntcc++] = installed;
        if (g_repo_root[0] &&
            (size_t)snprintf(in_repo, sizeof(in_repo), "%s/third_party/tcc", g_repo_root) < sizeof(in_repo))
            tcc_cands[ntcc++] = in_repo;
        for (size_t i = 0; i < ntcc; i++) {
            char probe[PATH_MAX];
            if ((size_t)snprintf(probe, sizeof(probe), "%s/include/stddef.h", tcc_cands[i]) >= sizeof(probe))
                continue;
            if (file_exists(probe)) {
                setenv("CC_TCC_LIB_PATH", tcc_cands[i], 1);
                break;
            }
        }
    }

    // Set CC_INCLUDE_PATH for TCC parser mode. Parser-lowered local/system
    // headers can reference both raw `.cch` headers and lowered `.h` headers.
    // TCC's own builtin headers ride along so the parse pass resolves
    // <stddef.h> without depending on the working directory.
    {
        char include_path[3 * PATH_MAX];
        size_t n = 0;
        const char* parts[3];
        size_t nparts = 0;
        if (g_cc_lowered_include[0]) parts[nparts++] = g_cc_lowered_include;
        if (g_cc_include[0] && (nparts == 0 || strcmp(g_cc_include, parts[0]) != 0))
            parts[nparts++] = g_cc_include;
        const char* tcc_lib = getenv("CC_TCC_LIB_PATH");
        char tcc_inc[PATH_MAX];
        if (tcc_lib && tcc_lib[0] &&
            (size_t)snprintf(tcc_inc, sizeof(tcc_inc), "%s/include", tcc_lib) < sizeof(tcc_inc))
            parts[nparts++] = tcc_inc;
        include_path[0] = '\0';
        for (size_t i = 0; i < nparts; i++)
            n += (size_t)snprintf(include_path + n, sizeof(include_path) - n,
                                  "%s%s", i ? ":" : "", parts[i]);
        if (include_path[0]) setenv("CC_INCLUDE_PATH", include_path, 1);
    }
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s [options] <input> [output]\n", prog);
    fprintf(stderr, "  %s <script> [args...]                      (auto-run; shebang-friendly)\n", prog);
    fprintf(stderr, "  %s -e PROGRAM [script-args...]             (unnamed script one-liner)\n", prog);
    fprintf(stderr, "  %s -E EXPR [script-args...]                (print @string(`${EXPR}`))\n", prog);
    fprintf(stderr, "  %s -e - [script-args...]                   (program text from stdin)\n", prog);
    fprintf(stderr, "  %s @name [args...]                         (run toolbox @task)\n", prog);
    fprintf(stderr, "  %s @                                       (list toolbox tasks)\n", prog);
    fprintf(stderr, "  %s run <input> [-- <args...>]              (shorthand for build run)\n", prog);
    fprintf(stderr, "  %s build [options] <input> <output>\n", prog);
    fprintf(stderr, "  %s build run [options] <input> [-o out/<stem>] [-- <args...>]\n", prog);
    fprintf(stderr, "  %s clean [--out-dir DIR] [--bin-dir DIR] [--all]\n", prog);
    fprintf(stderr, "  %s portable-install DIR                    (consumer host-C tree)\n", prog);
    fprintf(stderr, "Modes:\n");
    fprintf(stderr, "  --emit-c-only       Stop after emitting C (output defaults to out/<stem>.c)\n");
    fprintf(stderr, "  --emit-c-inspect[=PATH]  Dump the merged translation unit for inspection\n");
    fprintf(stderr, "                      (out/<stem>.inspect.c by default); best-effort even when\n");
    fprintf(stderr, "                      the build fails in a generic factory. Build still runs.\n");
    fprintf(stderr, "  --compile           Emit C and compile to object (output defaults to out/<stem>.o)\n");
    fprintf(stderr, "  --link              Emit C, compile, and link (default; binary defaults to out/<stem>)\n");
    fprintf(stderr, "  --print-cflags      Print compiler flags for Concurrent-C headers\n");
    fprintf(stderr, "  --print-libs        Print linker flags and runtime source for Concurrent-C\n");
    fprintf(stderr, "  --cccportable DIR   Author-only: --print-cflags/--print-libs use DIR\n");
    fprintf(stderr, "                      (one -I; not a sysroot; not for emit/lower)\n");
    fprintf(stderr, "Build integration:\n");
    fprintf(stderr, "  -DNAME[=VALUE]      Define comptime const (VALUE defaults to 1, build mode only)\n");
    fprintf(stderr, "  --build-file PATH   Use explicit build.cc path (overrides discovery)\n");
    fprintf(stderr, "  --no-build          Disable build.cc even if present\n");
    fprintf(stderr, "  --dump-consts       Print merged const bindings then compile\n");
    fprintf(stderr, "  --dump-comptime     Print consts/targets before compiling (--dump-consts implied)\n");
    fprintf(stderr, "  --dry-run           Resolve consts / show commands, skip compile/link\n");
    fprintf(stderr, "Toolchain:\n");
    fprintf(stderr, "  -o PATH             Output (mode dependent: C/object/binary)\n");
    fprintf(stderr, "  --obj-out PATH      Object output (for --link)\n");
    fprintf(stderr, "  --cc-bin PATH       C compiler (default: $CC or cc/gcc/clang)\n");
    fprintf(stderr, "  --cc-flags FLAGS    Extra compiler flags\n");
    fprintf(stderr, "  --ld-flags FLAGS    Extra linker flags\n");
    fprintf(stderr, "  --target TRIPLE     Forward target triple to C compiler\n");
    fprintf(stderr, "  --sysroot PATH      Forward sysroot to C compiler (host-cc cross)\n");
    fprintf(stderr, "  --no-line           Omit #line / CC_LN from emitted C\n");
    fprintf(stderr, "  --no-runtime        Do not link runtime (default links bundled runtime)\n");
    fprintf(stderr, "  --keep-c            Do not delete generated C file\n");
    fprintf(stderr, "  --out-dir DIR       Output dir for generated C + objects (default: ./out)\n");
    fprintf(stderr, "  --bin-dir DIR       Output dir for linked executables (default: ./bin)\n");
    fprintf(stderr, "  --out-stem NAME     Override the basename/stem used for generated files\n");
    fprintf(stderr, "  --no-cache          Disable incremental cache (also: CC_NO_CACHE=1)\n");
    fprintf(stderr, "  -j[N], --jobs[=N]   Parallel CC_TARGET build (default 4; 0/omit N → ncpu)\n");
    fprintf(stderr, "  --frontend=native   Front end (native only; also: CC_FRONTEND=native)\n");
    fprintf(stderr, "  --version, --v, -V  Print version (MAJOR.MINOR.PATCH-SEED)\n");
    fprintf(stderr, "  --as=ccs|cch|shcc   Unit kind (else first-line header, else suffix)\n");
    fprintf(stderr, "  version=X           Pin lowerer: MAJOR.MINOR (usual), or tighter / >=X / >X / <=X / <X, or both (>=A,<B)\n");
    fprintf(stderr, "  --ccc-version=X     Same as version=X (quote bounds: 'version=>=0.3')\n");
    fprintf(stderr, "  --timeout SECONDS   Kill run/test step after timeout\n");
    fprintf(stderr, "  --verbose           Print invoked commands\n");
    fprintf(stderr, "One-liners:\n");
    fprintf(stderr, "  -e PROGRAM          Compile/run PROGRAM as an unnamed .shcc unit\n");
    fprintf(stderr, "  -E EXPR             Like -e, wrapped as io.println(@string(`${EXPR}`)) !>;\n");
    fprintf(stderr, "  -n                  With -e/-E: loop over stdin lines (binds line, nr)\n");
    fprintf(stderr, "  -p                  -n plus io.println(line) !>; after the body\n");
    fprintf(stderr, "  --save NAME         With -e/-E: append desugared @task NAME to the toolbox\n");
    fprintf(stderr, "  --save-to PATH      Toolbox path override (default: ./tools/toolbox.shcc or ~/.ccc/toolbox.shcc)\n");
    fprintf(stderr, "  --doc TEXT          Summary text for --save (else first program line)\n");
}


/* --no-line / --cccportable in either argv position. 1=consumed, 0=no, -1=err. */
static int cc__take_vendor_flag(int argc, char** argv, int* i) {
    int r;
    if (!argv || !i || *i >= argc) return 0;
    if (strcmp(argv[*i], "--no-line") == 0) {
        g_no_line = 1;
        return 1;
    }
    r = cc_take_cccportable_flag(argc, argv, i, &g_cccportable_dir,
                                 &g_cccportable_cli);
    return r;
}

static int cc__emit_is_c(const char* path) {
    size_t n;
    if (!path) return 0;
    n = strlen(path);
    if (n < 2 || path[n - 2] != '.' || path[n - 1] != 'c') return 0;
    if (n >= 4 && path[n - 4] == '.' && path[n - 3] == 'c' &&
        path[n - 2] == 'c')
        return 0;
    return 1;
}

static int cc__finish_emit_c(const char* orig_in, const char* out_path,
                             int emit_c_only) {
    char* raw = NULL;
    size_t n = 0;
    int po = 0, lo = 0;
    char perr[192];
    int no_line;
    if (!cc__emit_is_c(out_path)) return 0;
    if (orig_in && orig_in[0]) {
        raw = cc__read_all_file(orig_in, &n);
        if (raw) {
            if (cc_file_start_pragmas(raw, n, &po, &lo, NULL, perr, sizeof(perr)) != 0) {
                fprintf(stderr, "%s: %s\n", orig_in, perr);
                free(raw);
                return -1;
            }
            free(raw);
        }
    }
    no_line = g_no_line || lo;
    if (cc_emit_polish_c(out_path, cc__version_string(), po, no_line) != 0) {
        fprintf(stderr, "cc: cannot polish emitted C %s\n", out_path);
        return -1;
    }
    if (emit_c_only && !no_line &&
        !cc_path_under_dir(out_path, g_out_root)) {
        fprintf(stderr,
                "cc: warning: #line still on for %s (not under out/); "
                "pass --no-line so vendored C does not embed author paths\n",
                out_path);
    }
    return 0;
}

static int cc__rm_rf(const char* path) {
    if (!path || !path[0]) return 0;
    // Best-effort portable-ish removal via /bin/rm. This is a dev tool; keep it simple.
    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", path);
    int rc = system(cmd);
    return rc == 0 ? 0 : -1;
}

static int cc__clean_artifacts(int all) {
    // These are rooted under the selected out/bin dirs.
    // Note: We do NOT delete out/cc/ which contains the compiler itself.
    char p1[PATH_MAX], p2[PATH_MAX], p3[PATH_MAX];
    snprintf(p1, sizeof(p1), "%s/.cc-build", g_out_root);
    snprintf(p2, sizeof(p2), "%s/.cc_test", g_out_root);
    snprintf(p3, sizeof(p3), "%s/.cc_test", g_bin_root);

    int bad = 0;
    bad |= cc__rm_rf(p1);
    bad |= cc__rm_rf(p2);
    bad |= cc__rm_rf(p3);
    if (all) {
        // Remove top-level emitted files (common in dev), but preserve out/cc/.
        char cmd[PATH_MAX * 2];
        snprintf(cmd, sizeof(cmd),
                 "rm -f \"%s\"/*.c \"%s\"/*.o \"%s\"/*.d \"%s\"/*.stderr \"%s\"/*.stdout \"%s\"/*.txt 2>/dev/null || true",
                 g_out_root, g_out_root, g_out_root, g_out_root, g_out_root, g_out_root);
        (void)system(cmd);
    }
    return bad ? -1 : 0;
}

static void usage_build(const char* prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s build [step] [options] <input> [output]\n", prog);
    fprintf(stderr, "  %s build run [options] <input> [-o bin/<stem>] [-- <args...>]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Steps:\n");
    fprintf(stderr, "  (default)   Build (emit C, compile, link)\n");
    fprintf(stderr, "  run         Build then run the produced binary\n");
    fprintf(stderr, "  test        Run the repo test suite (builds tools/cc_test if needed)\n");
    fprintf(stderr, "  list        List targets declared in build.cc\n");
    fprintf(stderr, "  graph       Print the target graph (JSON or DOT)\n");
    fprintf(stderr, "  install     Build then install the produced binary (requires CC_INSTALL)\n");
    fprintf(stderr, "  export-make Generate Makefile fragment for legacy build integration\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Build flavors:\n");
    fprintf(stderr, "  (default)       -O2 (asserts kept), dead-strip at link\n");
    fprintf(stderr, "  -g, --debug     Add -O0 -g (and disable dead-stripping)\n");
    fprintf(stderr, "  -O, --release   Add -O2 -DNDEBUG (dead-strip stays on)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options: same as main help (use `%s --help` for full list)\n", prog);
    fprintf(stderr, "  -j[N], --jobs[=N]   Parallel independent CC_TARGETs (default 4; 0 → ncpu)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Project options:\n");
    fprintf(stderr, "  build.cc may declare options using: CC_OPTION <NAME> <HELP...>\n");
    fprintf(stderr, "  --dump-comptime     Print build consts/targets before running the step\n");
    fprintf(stderr, "  --graph-out PATH    Write the graph output (json/dot) to PATH instead of stdout\n");
}

// Tiny helper to check for file existence.
static int file_exists(const char* path) {
    if (!path) return 0;
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* Content-keyed wrap: stable `path`, pid only on the temp. Parallel
 * testers rename onto the same name; bytes already match `path`. */
static int cc__install_wrap_file(const char* path, const void* data, size_t n) {
    char tmp[PATH_MAX];
    int w;
    if (!path || path[0] == '\0' || (!data && n)) return -1;
    if (file_exists(path)) return 0;
    w = snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    if (w < 0 || (size_t)w >= sizeof(tmp)) return -1;
    if (cc__write_file_bytes(tmp, (const char*)data, n) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        if (!file_exists(path)) return -1;
    }
    return 0;
}

static void detect_host_target(CCBuildTarget* t) {
    if (!t) return;
    const char* os = "unknown";
#if defined(__APPLE__)
    os = "macos";
#elif defined(_WIN32)
    os = "windows";
#elif defined(__linux__)
    os = "linux";
#endif

    const char* arch = "unknown";
#if defined(__x86_64__) || defined(_M_X64)
    arch = "x86_64";
#elif defined(__i386__) || defined(__i686__) || defined(_M_IX86)
    arch = "i386";
#elif defined(__aarch64__) || defined(_M_ARM64)
    arch = "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
    arch = "arm";
#endif

    const char* abi = "unknown";
#if defined(__APPLE__)
    abi = "sysv";
#elif defined(__GNUC__)
    abi = "gnu";
#endif

    const char* endian = "unknown";
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__)
    if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) endian = "little";
    else if (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) endian = "big";
#endif

    t->os = os;
    t->arch = arch;
    t->abi = abi;
    t->endian = endian;
    t->ptr_width = (int)(sizeof(void*) * 8);
}

// Picks build.cc location: prefer alongside input, fallback to cwd.
// If both exist, sets *multiple to 1 and returns NULL.
static const char* choose_build_path(const char* in_path, char* buf, size_t buf_size, int* multiple) {
    if (!in_path || !buf || buf_size == 0) return NULL;
    if (multiple) *multiple = 0;
    const char* slash = strrchr(in_path, '/');
    const char* candidate_input = NULL;
    if (slash) {
        size_t dir_len = (size_t)(slash - in_path);
        if (dir_len + strlen("/build.cc") + 1 <= buf_size) {
            memcpy(buf, in_path, dir_len);
            memcpy(buf + dir_len, "/build.cc", strlen("/build.cc") + 1);
            candidate_input = buf;
        }
    }
    int has_input = candidate_input && file_exists(candidate_input);
    int has_cwd = file_exists("build.cc");
    if (has_input && has_cwd) {
        /* Same file spelled two ways (input dir == cwd, e.g. an absolute
         * input path in the repo root) is ONE build.cc, not a conflict. */
        struct stat sa, sb;
        if (stat(candidate_input, &sa) == 0 && stat("build.cc", &sb) == 0 &&
            sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino) {
            return candidate_input;
        }
        if (multiple) *multiple = 1;
        return NULL;
    }
    if (has_input) return candidate_input;
    if (has_cwd) return "build.cc";
    return NULL;
}

static int ensure_out_dir(void) {
    // Ensure output roots exist.
    if (cc__mkdir_p(g_out_root) != 0) return -1;
    if (cc__mkdir_p(g_bin_root) != 0) return -1;
    if (cc__mkdir_p(g_cache_root) != 0) return -1;
    if (g_host_obj_root[0] && cc__mkdir_p(g_host_obj_root) != 0) return -1;
    return 0;
}

static void cc__stem_from_path(const char* path, char* out, size_t cap);
static int cc__unique_stem(const char* desired, char used[][128], size_t* used_count, size_t used_cap, char* out, size_t out_cap);
static int cc__compute_relative_path(const char* path, char* out, size_t cap);
static int cc__resolve_stems(const char** inputs, int count, const char* override, char (*out_stems)[128]);

static int derive_path_from_stem(const char* stem, const char* dir_root, const char* suffix, char* out_buf, size_t out_buf_size) {
    if (!stem || !dir_root || !suffix || !out_buf || out_buf_size == 0) return -1;
    size_t dir_len = strlen(dir_root);
    size_t stem_len = strlen(stem);
    size_t suffix_len = strlen(suffix);
    if (dir_len + 1 + stem_len + suffix_len + 1 > out_buf_size) return -1;
    memcpy(out_buf, dir_root, dir_len);
    out_buf[dir_len] = '/';
    memcpy(out_buf + dir_len + 1, stem, stem_len);
    out_buf[dir_len + 1 + stem_len] = '\0';
    strcat(out_buf, suffix);
    return 0;
}

static int derive_default_output(const char* in_path, char* out_buf, size_t out_buf_size) {
    if (!in_path) return -1;
    char stem[128];
    CCUnitHeader h;
    CCUnitKind k = cc_unit_kind_from_ext(in_path);
    const char* suf = ".c";
    memset(&h, 0, sizeof(h));
    if (cc_unit_header_from_file(in_path, &h) == 0 && !h.ill_formed &&
        h.kind == CC_UNIT_KIND_CCH)
        suf = ".h";
    else if (k == CC_UNIT_KIND_CCH)
        suf = ".h";
    cc__stem_from_path(in_path, stem, sizeof(stem));
    return derive_path_from_stem(stem, g_out_root, suf, out_buf, out_buf_size);
}

static int derive_default_obj(const char* in_path, char* out_buf, size_t out_buf_size) {
    if (!in_path) return -1;
    char stem[128];
    cc__stem_from_path(in_path, stem, sizeof(stem));
    return derive_path_from_stem(stem, g_host_obj_root[0] ? g_host_obj_root : g_out_root, ".o",
                                 out_buf, out_buf_size);
}

static int derive_default_bin(const char* in_path, char* out_buf, size_t out_buf_size) {
    if (!in_path) return -1;
    char stem[128];
    cc__stem_from_path(in_path, stem, sizeof(stem));
    return derive_path_from_stem(stem, g_bin_root, "", out_buf, out_buf_size);
}

/* Extension-module conventions are DECLARED by the stdlib, not built into
 * the driver.  A header the TU includes spells
 *
 *     CC_MODULE_ENTRY("PyInit_*", ".abi3.so")
 *     CC_MODULE_ENTRY("napi_register_module_v1", ".node", "js_module")
 *
 * (the macro itself expands to nothing) and the driver implements one
 * mechanism: a TU that exports a declared entry point and defines no
 * `main` is an extension module — PIC objects, a `-shared` link, the
 * declared suffix as the default output.  The artifact name follows the
 * declaration too: a trailing `*` in the entry means the symbol's own
 * suffix names it (`PyInit_counter` → counter); a third argument names a
 * factory whose `::[T]` type formal names it, camel lowered to snake
 * (`js_module::[Counter]` → counter); with neither, the source stem
 * does.  `main` beats any entry, so an embed-style program that mentions
 * one stays an executable.  Declarations are read from the TU's
 * `<ccc/…>` includes (one nested level), resolved against the input's
 * repo root, the compiler's own install tree, and CC_INCLUDE_PATH; the
 * first declared entry that matches wins. */
typedef struct {
    char entry[96];
    char suffix[32];
    char factory[64];
    char export_dir[64]; /* CC_MODULE_EXPORT directive beside the entry */
    int wildcard;
} CCModuleEntryDecl;

static char* cc__read_file_all(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    char* buf;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 16 * 1024 * 1024) { fclose(f); return NULL; }
    buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[n] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)n;
    return buf;
}

/* Harvest CC_MODULE_ENTRY declarations from one header text. */
static int cc__module_entry_scan_decls(const char* buf, CCModuleEntryDecl* d,
                                       int cap, int nd) {
    const char* p = buf;
    while ((p = strstr(p, "CC_MODULE_ENTRY")) != NULL && nd < cap) {
        const char* q = p + 15;
        char* args[3] = { d[nd].entry, d[nd].suffix, d[nd].factory };
        size_t caps[3] = { sizeof(d[nd].entry), sizeof(d[nd].suffix),
                           sizeof(d[nd].factory) };
        int a;
        p = q;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '(') continue;
        q++;
        d[nd].entry[0] = d[nd].suffix[0] = d[nd].factory[0] = 0;
        d[nd].export_dir[0] = 0;
        d[nd].wildcard = 0;
        for (a = 0; a < 3; a++) {
            size_t m = 0;
            while (*q == ' ' || *q == '\t') q++;
            if (*q != '"') break;
            q++;
            while (*q && *q != '"' && m + 1 < caps[a]) args[a][m++] = *q++;
            args[a][m] = 0;
            if (*q != '"') break;
            q++;
            while (*q == ' ' || *q == '\t') q++;
            if (*q != ',') break;
            q++;
        }
        if (d[nd].entry[0] && d[nd].suffix[0]) {
            size_t el = strlen(d[nd].entry);
            if (d[nd].entry[el - 1] == '*') {
                d[nd].entry[el - 1] = 0;
                d[nd].wildcard = 1;
            }
            if (d[nd].entry[0]) nd++;
        }
    }
    return nd;
}

/* The export directive declared beside the entries in the same header:
 * `CC_MODULE_EXPORT(<ident>, "…")`.  The `#define CC_MODULE_EXPORT(...)`
 * guard has `...` first and parses as nothing. */
static void cc__module_export_dir_scan(const char* text, char* out, size_t cap) {
    const char* p = text;
    out[0] = 0;
    while ((p = strstr(p, "CC_MODULE_EXPORT")) != NULL) {
        const char* q = p + 16;
        size_t m = 0;
        p = q;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '(') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        while ((isalnum((unsigned char)*q) || *q == '_') && m + 1 < cap)
            out[m++] = *q++;
        out[m] = 0;
        while (*q == ' ' || *q == '\t') q++;
        if (m == 0 || *q != ',') { out[0] = 0; continue; }
        return;
    }
}

/* Collect declarations from the TU's `<ccc/…>` includes, one level deep
 * (resolution + include walk shared with the export-directive pass). */
static int cc__module_entry_collect(const char* buf, const char* in_path,
                                    CCModuleEntryDecl* d, int cap) {
    int nd = 0;
    int depth;
    char rels[24][192];
    int nrel, scanned = 0;
    nrel = cc_module_collect_ccc_includes(buf, rels, 24, 0);
    for (depth = 0; depth < 2; depth++) {
        int end = nrel;
        for (; scanned < end; scanned++) {
            char* text = cc_module_header_read_text(rels[scanned], in_path);
            int before;
            if (!text) continue;
            before = nd;
            nd = cc__module_entry_scan_decls(text, d, cap, nd);
            if (nd > before) {
                char dir[64];
                int e;
                cc__module_export_dir_scan(text, dir, sizeof(dir));
                for (e = before; e < nd; e++)
                    snprintf(d[e].export_dir, sizeof(d[e].export_dir), "%s",
                             dir);
            }
            if (depth == 0)
                nrel = cc_module_collect_ccc_includes(text, rels, 24, nrel);
            free(text);
        }
    }
    return nd;
}

/* One buildable artifact of an extension-module TU.  A TU that spells
 * entries for several embeddings (both export directives, or both
 * explicit stanzas) is DUAL-TARGET: the compiled shared object is
 * identical under every entry — all stanzas are always compiled in and
 * every embedding resolves its runtime lazily — so the targets differ
 * only in artifact name.  `tag` narrows via --module=<tag>, derived
 * from the header's export directive (cc_py_export → py). */
typedef struct {
    char name[128];
    char suffix[32];
    char tag[32];
    char entry[96]; /* CC_MODULE_ENTRY prefix (PyInit_ / napi_…) */
    int wildcard;   /* entry was declared with a trailing * */
    /* Wildcard targets mint one entry symbol PER export site, and each
     * needs its own importable file name (import counter; import stats)
     * — the names past the first land here and hardlink to the primary
     * artifact.  Aggregating targets (napi) namespace inside one file
     * and keep nextra 0. */
    char extra[6][128];
    int nextra;
} CCExtModTarget;

static void cc__tag_from_directive(const char* dir, char* out, size_t cap) {
    size_t l;
    out[0] = 0;
    if (!dir || !dir[0]) return;
    l = strlen(dir);
    if (strncmp(dir, "cc_", 3) == 0 && l > 10 &&
        strcmp(dir + l - 7, "_export") == 0) {
        size_t m = l - 10;
        if (m + 1 > cap) m = cap - 1;
        memcpy(out, dir + 3, m);
        out[m] = 0;
    } else {
        snprintf(out, cap, "%s", dir);
    }
}

/* Scan the TU for every declared entry with evidence — an exported entry
 * symbol, or an export directive (which guarantees the emitted entry).
 * Fills tg[] in declaration order (first = primary) and returns the
 * count; 0 when the TU defines `main` (main beats any entry) or spells
 * no entry at all. */
static int cc__detect_ext_module(const char* in_path, CCExtModTarget* tg,
                                 int tcap) {
    char* buf;
    size_t len = 0;
    CCModuleEntryDecl decls[8];
    int found[8] = { 0 };
    char names[8][128];
    char site_names[8][7][128];
    int site_counts[8] = { 0 };
    int nd, has_main = 0;
    if (!in_path || !tg || tcap <= 0) return 0;
    memset(names, 0, sizeof(names));
    memset(site_names, 0, sizeof(site_names));
    buf = cc__read_file_all(in_path, &len);
    if (!buf) return 0;
    nd = cc__module_entry_collect(buf, in_path, decls,
                                  (int)(sizeof(decls) / sizeof(decls[0])));
    if (nd == 0) { free(buf); return 0; }
    /* `@comptime <directive>(...)` sites: an export directive guarantees
     * an emitted entry, so it is entry evidence AND the name source (the
     * first site's type, camel lowered to snake, or its override).
     * Every site's name is kept: a wildcard entry mints one symbol per
     * site, and each needs its own artifact name. */
    {
        int e;
        for (e = 0; e < nd; e++) {
            int ns;
            if (!decls[e].export_dir[0]) continue;
            ns = cc_module_export_tu_artifact_all(buf, len,
                                                  decls[e].export_dir,
                                                  site_names[e], 7);
            if (ns > 0) {
                found[e] = 1;
                site_counts[e] = ns;
                snprintf(names[e], sizeof(names[e]), "%s", site_names[e][0]);
            }
        }
    }
    {
        size_t i = 0;
        while (i < len) {
            char c = buf[i];
            char c2 = (i + 1 < len) ? buf[i + 1] : 0;
            if (c == '/' && c2 == '/') { while (i < len && buf[i] != '\n') i++; continue; }
            if (c == '/' && c2 == '*') {
                i += 2;
                while (i + 1 < len && !(buf[i] == '*' && buf[i + 1] == '/')) i++;
                i = (i + 1 < len) ? i + 2 : len;
                continue;
            }
            if (c == '"' || c == '\'' || c == '`') {
                char q = c;
                i++;
                while (i < len) {
                    if (q != '`' && buf[i] == '\\' && i + 1 < len) { i += 2; continue; }
                    if (buf[i] == q) { i++; break; }
                    i++;
                }
                continue;
            }
            if (i > 0 && (isalnum((unsigned char)buf[i - 1]) || buf[i - 1] == '_')) {
                i++;
                continue;
            }
            if (c == 'm' && i + 4 < len && memcmp(buf + i, "main", 4) == 0 &&
                !(isalnum((unsigned char)buf[i + 4]) || buf[i + 4] == '_')) {
                size_t k = i + 4;
                while (k < len && (buf[k] == ' ' || buf[k] == '\t')) k++;
                if (k < len && buf[k] == '(') has_main = 1;
                i += 4;
                continue;
            }
            {
                int e;
                for (e = 0; e < nd; e++) {
                    size_t el = strlen(decls[e].entry);
                    if (i + el > len || memcmp(buf + i, decls[e].entry, el) != 0)
                        continue;
                    if (decls[e].wildcard) {
                        size_t k = i + el, s = k, m = 0;
                        while (k < len &&
                               (isalnum((unsigned char)buf[k]) || buf[k] == '_'))
                            k++;
                        if (k == s) continue;
                        found[e] = 1;
                        if (!names[e][0]) {
                            while (s + m < k && m + 1 < sizeof(names[e])) {
                                names[e][m] = buf[s + m];
                                m++;
                            }
                            names[e][m] = '\0';
                        }
                        i = k;
                        break;
                    }
                    if (i + el < len &&
                        (isalnum((unsigned char)buf[i + el]) || buf[i + el] == '_'))
                        continue;
                    found[e] = 1;
                    i += el;
                    break;
                }
                if (e < nd) continue;
            }
            /* `<factory>::[T]` — the registration's type formal names the
             * artifact, camel lowered to snake.  A name source only: the
             * entry symbol itself decides module-ness, so a factory used
             * without the entry never silently produces an addon with no
             * registration. */
            {
                int e;
                for (e = 0; e < nd; e++) {
                    size_t fl;
                    size_t k;
                    if (!decls[e].factory[0]) continue;
                    fl = strlen(decls[e].factory);
                    if (i + fl + 3 > len ||
                        memcmp(buf + i, decls[e].factory, fl) != 0)
                        continue;
                    k = i + fl;
                    while (k < len && (buf[k] == ' ' || buf[k] == '\t')) k++;
                    if (k + 2 >= len || buf[k] != ':' || buf[k + 1] != ':' ||
                        buf[k + 2] != '[')
                        continue;
                    k += 3;
                    while (k < len && (buf[k] == ' ' || buf[k] == '\t')) k++;
                    {
                        size_t s = k, m = 0, q;
                        while (k < len &&
                               (isalnum((unsigned char)buf[k]) || buf[k] == '_'))
                            k++;
                        if (k == s) continue;
                        if (!names[e][0]) {
                            for (q = s; q < k && m + 2 < sizeof(names[e]); q++) {
                                char ch = buf[q];
                                if (ch >= 'A' && ch <= 'Z') {
                                    if (q > s) names[e][m++] = '_';
                                    names[e][m++] = (char)(ch - 'A' + 'a');
                                } else {
                                    names[e][m++] = ch;
                                }
                            }
                            names[e][m] = '\0';
                        }
                        i = k;
                        break;
                    }
                }
                if (e < nd) continue;
            }
            i++;
        }
    }
    free(buf);
    if (has_main) return 0;
    {
        int e, nt = 0;
        for (e = 0; e < nd && nt < tcap; e++) {
            int s;
            if (!found[e]) continue;
            /* names[] holds distinct MODULE names now (the directive's
             * first argument, always explicit): the first group names
             * the linked artifact, the rest hardlink it. */
            snprintf(tg[nt].name, sizeof(tg[nt].name), "%s", names[e]);
            snprintf(tg[nt].suffix, sizeof(tg[nt].suffix), "%s",
                     decls[e].suffix);
            cc__tag_from_directive(decls[e].export_dir, tg[nt].tag,
                                   sizeof(tg[nt].tag));
            snprintf(tg[nt].entry, sizeof(tg[nt].entry), "%s",
                     decls[e].entry);
            tg[nt].wildcard = decls[e].wildcard;
            tg[nt].nextra = 0;
            /* Every group past the first is an extra artifact name —
             * wildcard targets select the module by entry symbol,
             * aggregating targets by the loaded basename. */
            for (s = 1; s < site_counts[e] &&
                        tg[nt].nextra < (int)(sizeof(tg[nt].extra) /
                                              sizeof(tg[nt].extra[0]));
                 s++) {
                snprintf(tg[nt].extra[tg[nt].nextra],
                         sizeof(tg[nt].extra[0]), "%s", site_names[e][s]);
                tg[nt].nextra++;
            }
            nt++;
        }
        return nt;
    }
}

/* Plain byte copy under a new name (distinct inode on purpose). */
static int cc__copy_bytes(const char* src, const char* dst);

/* Hardlink dst to src (same bytes under a second name); copy when the
 * filesystem refuses links. */
static int cc__link_or_copy(const char* src, const char* dst) {
    unlink(dst);
    if (link(src, dst) == 0) return 0;
    return cc__copy_bytes(src, dst);
}

static int cc__copy_bytes(const char* src, const char* dst) {
    unlink(dst);
    {
        FILE* in = fopen(src, "rb");
        FILE* out = in ? fopen(dst, "wb") : NULL;
        char buf[65536];
        size_t got;
        if (!in || !out) {
            if (in) fclose(in);
            if (out) fclose(out);
            return -1;
        }
        while ((got = fread(buf, 1, sizeof(buf), in)) > 0) {
            if (fwrite(buf, 1, got, out) != got) {
                fclose(in);
                fclose(out);
                return -1;
            }
        }
        fclose(in);
        if (fclose(out) != 0) return -1;
        return chmod(dst, 0755) == 0 ? 0 : -1;
    }
}

static const char* pick_cc_bin(const char* override) {
    if (override) return override;
    const char* env = getenv("CC");
    if (env && *env) return env;
    // Fallback list.
    return "cc";
}

static int run_cmd(const char* cmd, int verbose) {
    if (verbose) {
        fprintf(stderr, "cc: %s\n", cmd);
    }
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "cc: command failed (rc=%d): %s\n", rc, cmd);
    }
    return rc;
}

static int run_exec(const char* bin_path, char* const* argv, int verbose) {
    if (!bin_path || !argv || !argv[0]) return -1;
    if (verbose) {
        fprintf(stderr, "cc: run:");
        for (int i = 0; argv[i]; ++i) {
            fprintf(stderr, " %s", argv[i]);
        }
        fprintf(stderr, "\n");
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        execv(bin_path, argv);
        perror("execv");
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static int run_exec_timeout(const char* bin_path, char* const* argv, int verbose, int timeout_sec) {
    if (timeout_sec < 0) {
        return run_exec(bin_path, argv, verbose);
    }
    if (!bin_path || !argv || !argv[0]) return -1;
    if (verbose) {
        fprintf(stderr, "cc: run (timeout=%ds):", timeout_sec);
        for (int i = 0; argv[i]; ++i) {
            fprintf(stderr, " %s", argv[i]);
        }
        fprintf(stderr, "\n");
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        execv(bin_path, argv);
        perror("execv");
        _exit(127);
    }
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int status = 0;
    while (1) {
        pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc < 0) {
            perror("waitpid");
            return -1;
        }
        if (rc > 0) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
            return 1;
        }
        if (timeout_sec == 0) break;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L +
                          (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (elapsed_ms >= (long)timeout_sec * 1000L) {
            fprintf(stderr, "cc: run timed out after %d seconds\n", timeout_sec);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return 124;
        }
        struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = 10000000L};
        nanosleep(&sleep_ts, NULL);
    }
    fprintf(stderr, "cc: run timed out after %d seconds\n", timeout_sec);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return 124;
}

// Add/override binding in-place; last writer wins.
static int upsert_binding(CCConstBinding* bindings, size_t* count, size_t max, const char* name, long long value) {
    if (!bindings || !count || !name) return -1;
    for (size_t i = 0; i < *count; ++i) {
        if (strcmp(bindings[i].name, name) == 0) {
            bindings[i].value = value;
            // Keep existing flag; no new allocation needed.
            return 0;
        }
    }
    if (*count >= max) return -1;
    bindings[*count].name = name;
    bindings[*count].value = value;
    *count += 1;
    return 0;
}

static int parse_define(const char* arg, char** out_name, long long* out_value) {
    // arg is after "-D"
    const char* eq = strchr(arg, '=');
    long long value = 1;
    if (eq) {
        char* endptr = NULL;
        value = strtoll(eq + 1, &endptr, 0);
        if (endptr == eq + 1) {
            fprintf(stderr, "cc: invalid -D value: %s\n", arg);
            return -1;
        }
    }
    size_t name_len = eq ? (size_t)(eq - arg) : strlen(arg);
    if (name_len == 0) {
        fprintf(stderr, "cc: empty -D name\n");
        return -1;
    }
    char name_buf[128];
    if (name_len >= sizeof(name_buf)) {
        fprintf(stderr, "cc: -D name too long\n");
        return -1;
    }
    memcpy(name_buf, arg, name_len);
    name_buf[name_len] = '\0';
    // Store pointer to the copied name in static buffer? Instead, strdup is safer here.
    char* stored = strdup(name_buf);
    if (!stored) {
        fprintf(stderr, "cc: out of memory parsing -D\n");
        return -1;
    }
    *out_name = stored;
    *out_value = value;
    return 0;
}

typedef enum {
    CC_MODE_LINK = 0,
    CC_MODE_COMPILE = 1,
    CC_MODE_EMIT_C = 2
} CCMode;

typedef struct {
    const char* in_path;
    const char* c_out_path;
    const char* obj_out_path;
    const char* bin_out_path;
    CCMode mode;
    const char* cc_bin_override;
    const char* cc_flags;
    const char* ld_flags;
    const char* target_flag;  // target triple (forwarded as: --target <triple>)
    const char* sysroot_flag; // sysroot path (forwarded as: --sysroot <path>)
    int opt_release; // enable size/perf oriented defaults (dead-strip, -DNDEBUG)
    int opt_debug;   // enable debug oriented defaults (-g, lower opt)
    int no_runtime;
    int keep_c;
    int verbose;
    const char* build_override;
    int no_build;
    int dump_consts;
    int dump_comptime;
    int dry_run;
    int summary;
    const char* out_dir;
    const char* bin_dir;
    int no_cache;
    char** cli_names;
    long long* cli_values;
    size_t cli_count;
    CCUnitKind unit_kind;          /* UNKNOWN: resolve from header / suffix */
    const char* ccc_version_pin;   /* NULL/empty: running toolchain */
} CCBuildOptions;

/* Extract -I paths from cc_flags and set CC_USER_INCLUDE_PATH (colon-separated).
 * Paths are realpath'd so extract sees them after any later chdir / cache
 * work. An already-set env (cc_test .env) is kept and merged. */
static int cc__append_include_dir(char* dst, size_t cap, size_t* pos,
                                  const char* dir, size_t dir_len) {
    char tmp[PATH_MAX];
    char abs[PATH_MAX];
    const char* use = dir;
    size_t n = dir_len;
    if (!dst || !pos || !dir || dir_len == 0 || cap < 4) return 0;
    if (dir_len >= sizeof(tmp)) return 0;
    memcpy(tmp, dir, dir_len);
    tmp[dir_len] = '\0';
    if (realpath(tmp, abs)) {
        use = abs;
        n = strlen(abs);
    }
    if (*pos + ( *pos ? 1 : 0 ) + n + 1 > cap) return 0;
    if (*pos) dst[(*pos)++] = ':';
    memcpy(dst + *pos, use, n);
    *pos += n;
    dst[*pos] = '\0';
    return 1;
}

/* `-isysroot PATH`, `-isysrootPATH`, `--sysroot PATH`, `--sysroot=PATH`. */
static int cc__flag_path_after(const char** pp, const char* flag, int attached,
                               char* out, size_t cap) {
    const char* p;
    const char* s;
    const char* e;
    size_t fl, n;
    if (!pp || !*pp || !flag || !out || cap < 2) return 0;
    p = *pp;
    fl = strlen(flag);
    if (strncmp(p, flag, fl) != 0) return 0;
    s = p + fl;
    if (*s == '=')
        s++;
    else if (*s == '\0' || *s == ' ' || *s == '\t') {
        while (*s == ' ' || *s == '\t') s++;
    } else if (!attached)
        return 0;
    if (!*s || *s == '-') return 0;
    e = s;
    while (*e && *e != ' ' && *e != '\t') e++;
    n = (size_t)(e - s);
    if (n == 0 || n >= cap) return 0;
    memcpy(out, s, n);
    out[n] = '\0';
    *pp = e;
    return 1;
}

static void cc__apply_sysroot_env(const char* sysroot) {
    char abs[PATH_MAX];
    char inc[PATH_MAX];
    const char* use;
    const char* prior;
    if (!sysroot || !sysroot[0]) return;
    use = realpath(sysroot, abs) ? abs : sysroot;
    prior = getenv("CC_SYSROOT");
    if (!prior || !prior[0])
        setenv("CC_SYSROOT", use, 1);
    if ((size_t)snprintf(inc, sizeof(inc), "%s/usr/include", use) < sizeof(inc) &&
        access(inc, R_OK) == 0) {
        static char include_paths[4096];
        const char* cur = getenv("CC_USER_INCLUDE_PATH");
        size_t pos = 0;
        include_paths[0] = '\0';
        if (cur && cur[0]) {
            const char* e = cur;
            while (e && e[0]) {
                const char* colon = strchr(e, ':');
                size_t n = colon ? (size_t)(colon - e) : strlen(e);
                (void)cc__append_include_dir(include_paths, sizeof(include_paths),
                                             &pos, e, n);
                e = colon ? colon + 1 : NULL;
            }
        }
        (void)cc__append_include_dir(include_paths, sizeof(include_paths), &pos,
                                     inc, strlen(inc));
        if (include_paths[0])
            setenv("CC_USER_INCLUDE_PATH", include_paths, 1);
    }
}

static void cc__apply_user_include_env(const char* cc_flags) {
    static char include_paths[4096];
    const char* prior = getenv("CC_USER_INCLUDE_PATH");
    size_t pos = 0;
    char sysroot[PATH_MAX];
    sysroot[0] = '\0';
    include_paths[0] = '\0';
    if (cc_flags && cc_flags[0]) {
        const char* p = cc_flags;
        while (*p) {
            while (*p && (*p == ' ' || *p == '\t')) p++;
            if (!*p) break;
            if (p[0] == '-' && p[1] == 'I') {
                const char* path_start = p + 2;
                if (*path_start == ' ' || *path_start == '\t') {
                    path_start++;
                    while (*path_start && (*path_start == ' ' || *path_start == '\t'))
                        path_start++;
                }
                if (*path_start && *path_start != '-') {
                    const char* path_end = path_start;
                    while (*path_end && *path_end != ' ' && *path_end != '\t')
                        path_end++;
                    (void)cc__append_include_dir(include_paths,
                                                 sizeof(include_paths), &pos,
                                                 path_start,
                                                 (size_t)(path_end - path_start));
                    p = path_end;
                    continue;
                }
            }
            {
                const char* q = p;
                if (cc__flag_path_after(&q, "-isysroot", 1, sysroot,
                                        sizeof(sysroot)) ||
                    cc__flag_path_after(&q, "--sysroot", 0, sysroot,
                                        sizeof(sysroot))) {
                    p = q;
                    continue;
                }
            }
            while (*p && *p != ' ' && *p != '\t') p++;
        }
    }
    if (prior && prior[0]) {
        const char* e = prior;
        while (e && e[0]) {
            const char* colon = strchr(e, ':');
            size_t n = colon ? (size_t)(colon - e) : strlen(e);
            (void)cc__append_include_dir(include_paths, sizeof(include_paths),
                                         &pos, e, n);
            e = colon ? colon + 1 : NULL;
        }
    }
    if (include_paths[0])
        setenv("CC_USER_INCLUDE_PATH", include_paths, 1);
    if (sysroot[0])
        cc__apply_sysroot_env(sysroot);
}

/* Warm-cache diagnostic replay (ccache-style).  Lowering-time warnings are
 * raw fprintf(stderr) calls scattered across pass code, so the emit stage
 * captures fd 2 around the in-process lowering call and persists the bytes
 * as a sidecar next to the emitted C (<out>.c.diag).  A cache hit that
 * skips lowering replays the sidecar so warm builds print the same
 * diagnostics as cold ones. */
/* 0 on success; -1 if "%s.diag" would not fit in `out` (no silent clip). */
static int cc__diag_sidecar_path(const char* c_out_path, char* out, size_t cap) {
    int n;
    if (!out || cap == 0) return -1;
    out[0] = '\0';
    if (!c_out_path) return -1;
    n = snprintf(out, cap, "%s.diag", c_out_path);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

static void cc__replay_diag_sidecar(const char* c_out_path) {
    char path[PATH_MAX];
    FILE* f;
    char buf[4096];
    size_t n;
    if (cc__diag_sidecar_path(c_out_path, path, sizeof(path)) != 0) {
        fprintf(stderr,
                "cc: error: diag sidecar path truncated for '%s' (PATH_MAX=%d); "
                "refusing silent clip — warm rebuild will omit cached diagnostics\n",
                c_out_path ? c_out_path : "", (int)PATH_MAX);
        return;
    }
    f = fopen(path, "rb");
    if (!f) return;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fwrite(buf, 1, n, stderr);
    }
    fclose(f);
}

static int cc__run_shadow_lower(const CCBuildOptions* opt, const char* out_path);
static int cc__find_shadow_lower(char* dst, size_t cap);

/* Emit .ccs/.shcc via native shadow_lower (legacy multipass driver removed). */
static int cc__compile_with_env(const CCBuildOptions* opt, const char* in_path, const char* out_path, const CCCompileConfig* cfg) {
    cc__apply_user_include_env(opt ? opt->cc_flags : NULL);
    if (opt && opt->sysroot_flag && opt->sysroot_flag[0])
        cc__apply_sysroot_env(opt->sysroot_flag);
    else if (getenv("CC_SYSROOT") && getenv("CC_SYSROOT")[0])
        cc__apply_sysroot_env(getenv("CC_SYSROOT"));
    if (g_emit_c_inspect) {
        if (g_emit_c_inspect_path && g_emit_c_inspect_path[0]) {
            setenv("CC_EMIT_C_INSPECT", g_emit_c_inspect_path, 1);
        } else {
            char stem[128];
            cc__stem_from_path(in_path, stem, sizeof(stem));
            char p[PATH_MAX];
            snprintf(p, sizeof(p), "%s/%s.inspect.c", g_out_root, stem);
            setenv("CC_EMIT_C_INSPECT", p, 1);
        }
    } else {
        unsetenv("CC_EMIT_C_INSPECT");
    }
    {
        long pe0 = cc_pass_error_count();
        int diag0 = cc_diag_error_count();

        /* Redirect fd 2 to a temp file for the duration of lowering; the
         * capture is echoed to the real stderr afterwards so cold-build
         * output is unchanged.  On capture setup failure just lower with
         * live stderr (no replay sidecar for this emit). */
        char diag_path[PATH_MAX];
        char diag_tmp[PATH_MAX];
        int sidecar_ok = (cc__diag_sidecar_path(out_path, diag_path, sizeof(diag_path)) == 0);
        int tmp_n = sidecar_ok
            ? snprintf(diag_tmp, sizeof(diag_tmp), "%s.%d.tmp", diag_path, (int)getpid())
            : -1;
        if (!sidecar_ok || tmp_n < 0 || (size_t)tmp_n >= sizeof(diag_tmp)) {
            fprintf(stderr,
                    "cc: error: diag sidecar path truncated for '%s' (PATH_MAX=%d); "
                    "refusing silent clip — this emit will not write a replay sidecar\n",
                    out_path ? out_path : "", (int)PATH_MAX);
            sidecar_ok = 0;
            diag_path[0] = '\0';
            diag_tmp[0] = '\0';
        }
        int saved_err = -1;
        int cap_fd = -1;
        if (sidecar_ok) {
            cap_fd = open(diag_tmp, O_CREAT | O_TRUNC | O_RDWR, 0644);
            if (cap_fd >= 0) {
                fflush(stderr);
                saved_err = dup(2);
                if (saved_err < 0 || dup2(cap_fd, 2) < 0) {
                    if (saved_err >= 0) { close(saved_err); saved_err = -1; }
                    close(cap_fd);
                    cap_fd = -1;
                    unlink(diag_tmp);
                }
            }
        }

        int rc = -1;
        {
            CCUnitKind uk = CC_UNIT_KIND_UNKNOWN;
            char pin[CC_CCC_VERSION_PIN_CAP];
            char uerr[256];
            pin[0] = '\0';
            if (cc_unit_resolve(in_path,
                                opt ? opt->unit_kind : CC_UNIT_KIND_UNKNOWN,
                                opt ? opt->ccc_version_pin : NULL, &uk, pin,
                                uerr, sizeof(uerr)) != 0) {
                fprintf(stderr, "%s\n", uerr);
            } else if (uk != CC_UNIT_KIND_CCS && uk != CC_UNIT_KIND_SHCC &&
                       uk != CC_UNIT_KIND_CCH) {
                fprintf(stderr,
                        "cc: %s: not a Concurrent-C unit (need #!ccc ccs|cch, "
                        "a ccc shebang, or a .ccs/.cch/.shcc suffix)\n",
                        in_path ? in_path : "(null)");
            } else {
            CCBuildOptions local_opt;
            char flags_buf[2048];
            flags_buf[0] = '\0';
            if (opt) {
                local_opt = *opt;
            } else {
                memset(&local_opt, 0, sizeof(local_opt));
                local_opt.mode = CC_MODE_EMIT_C;
                local_opt.out_dir = g_out_root;
                local_opt.bin_dir = g_bin_root;
                /* Multi-file / target paths preload consts into cfg; fold as -D. */
                if (cfg && cfg->consts && cfg->const_count > 0) {
                    size_t flen = 0;
                    for (size_t i = 0; i < cfg->const_count; ++i) {
                        int n;
                        if (!cfg->consts[i].name || !cfg->consts[i].name[0]) continue;
                        if (cfg->consts[i].value == 1) {
                            n = snprintf(flags_buf + flen, sizeof(flags_buf) - flen,
                                         "%s-D%s", flen ? " " : "", cfg->consts[i].name);
                        } else {
                            n = snprintf(flags_buf + flen, sizeof(flags_buf) - flen,
                                         "%s-D%s=%lld", flen ? " " : "",
                                         cfg->consts[i].name,
                                         cfg->consts[i].value);
                        }
                        if (n < 0 || (size_t)n >= sizeof(flags_buf) - flen) {
                            fprintf(stderr, "cc: cfg -D flags too long for native emit\n");
                            flags_buf[0] = '\0';
                            break;
                        }
                        flen += (size_t)n;
                    }
                    if (flags_buf[0]) local_opt.cc_flags = flags_buf;
                }
            }
            local_opt.in_path = in_path;
            local_opt.c_out_path = out_path;
            rc = cc__run_shadow_lower(&local_opt, out_path);
            }
        }

        long cap_len = 0;
        if (saved_err >= 0) {
            fflush(stderr);
            dup2(saved_err, 2);
            close(saved_err);
            if (lseek(cap_fd, 0, SEEK_SET) == 0) {
                char buf[4096];
                ssize_t n;
                while ((n = read(cap_fd, buf, sizeof(buf))) > 0) {
                    fwrite(buf, 1, (size_t)n, stderr);
                    cap_len += n;
                }
            }
            close(cap_fd);
        }

        /* A pass that prints an error but "recovers" (leaves the construct
         * unlowered) must not count as a successful emit: the .c would be
         * cached, and warm reruns would skip the diagnostic entirely and
         * fail somewhere else (or not at all).  Fail here so no meta is
         * written and every run reprints the diagnostic. */
        long pe = cc_pass_error_count() - pe0;
        long de = (long)(cc_diag_error_count() - diag0);
        int diag_err = (rc == 0 && (pe > 0 || de > 0));

        if (!sidecar_ok) {
            /* Path truncated: leave no partial sidecar; live stderr already
             * received diagnostics (no capture redirect was installed). */
        } else if (rc != 0 || diag_err || cap_len == 0) {
            /* Failing emits are never cached, so they need no sidecar; a
             * quiet emit drops any stale one. */
            if (cap_fd >= 0) unlink(diag_tmp);
            unlink(diag_path);
        } else if (rename(diag_tmp, diag_path) != 0) {
            unlink(diag_tmp);
        }

        if (diag_err) {
            fprintf(stderr,
                    "cc: %ld error diagnostic(s) during lowering of %s; "
                    "failing the build (emitted C not cached)\n",
                    pe + de, in_path);
            return -1;
        }
        return rc;
    }
}

static int cc__compile_c_to_obj(const CCBuildOptions* opt,
                                const char* c_path,
                                const char* obj_path,
                                const char* dep_path,
                                const char* extra_include_dir,
                                const char* target_part,
                                const char* sysroot_part);

static int cc__ensure_runtime_obj(const CCBuildOptions* opt,
                                 const char* target_part,
                                 const char* sysroot_part,
                                 char* out_runtime_path,
                                 size_t out_runtime_cap,
                                 int* out_reused);

typedef struct {
    const char* c_out_path;
    const char* obj_out_path;
    const char* bin_out_path;
    int did_emit_c;
    int did_compile_obj;
    int did_link;
    int runtime_reused;
    const char* runtime_obj_path;
    int reuse_emit_c;
    int reuse_compile_obj;
    int reuse_link;
} CCBuildSummary;

static void cc__stem_from_path(const char* path, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!path) return;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char* dot = strrchr(base, '.');
    size_t n = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    if (n + 1 > cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = '\0';
}

static int cc__compute_relative_path(const char* path, char* out, size_t cap) {
    if (!path || !out || cap == 0) return -1;
    char abs_path[PATH_MAX];
    if (realpath(path, abs_path) == NULL) {
        strncpy(abs_path, path, sizeof(abs_path));
        abs_path[sizeof(abs_path) - 1] = '\0';
    }
    size_t root_len = strlen(g_repo_root);
    const char* rel = abs_path;
    if (root_len && strncmp(abs_path, g_repo_root, root_len) == 0) {
        rel = abs_path + root_len;
        if (*rel == '/' || *rel == '\\') rel++;
    }
    strncpy(out, rel, cap);
    out[cap - 1] = '\0';
    return 0;
}

static int cc__resolve_stems(const char** inputs, int count, const char* override, char (*out_stems)[128]) {
    if (!out_stems || count <= 0) return 0;
    if (override) {
        if (count > 1) return -1;
        strncpy(out_stems[0], override, 128);
        out_stems[0][127] = '\0';
        return 0;
    }
    struct StemEntry {
        char name[128];
        int count;
    } stems[64] = {{0}};
    size_t stem_count = 0;
    char base_stems[64][128] = {{0}};
    for (int i = 0; i < count; ++i) {
        cc__stem_from_path(inputs[i], base_stems[i], sizeof(base_stems[i]));
        int found = -1;
        for (size_t j = 0; j < stem_count; ++j) {
            if (strcmp(stems[j].name, base_stems[i]) == 0) {
                stems[j].count++;
                found = (int)j;
                break;
            }
        }
        if (found < 0) {
            if (stem_count >= 64) return -1;
            strncpy(stems[stem_count].name, base_stems[i], sizeof(stems[stem_count].name));
            stems[stem_count].count = 1;
            stem_count++;
        }
    }

    char used[64][128] = {{0}};
    size_t used_count = 0;
    for (int i = 0; i < count; ++i) {
        char desired[128];
        int duplicate = 0;
        for (size_t j = 0; j < stem_count; ++j) {
            if (strcmp(stems[j].name, base_stems[i]) == 0) {
                duplicate = stems[j].count > 1;
                break;
            }
        }
        if (duplicate) {
            char rel[PATH_MAX];
            if (cc__compute_relative_path(inputs[i], rel, sizeof(rel)) != 0) return -1;
            if (cc_build_make_stem(desired, sizeof(desired), rel) != 0) return -1;
        } else {
            strncpy(desired, base_stems[i], sizeof(desired));
        }
        desired[sizeof(desired) - 1] = '\0';
        if (cc__unique_stem(desired, used, &used_count, 64, out_stems[i], 128) != 0) return -1;
    }
    return 0;
}

static int cc__unique_stem(const char* desired,
                           char used[][128],
                           size_t* used_count,
                           size_t used_cap,
                           char* out,
                           size_t out_cap) {
    if (!desired || !used_count || !out || out_cap == 0) return -1;
    // First try desired.
    int taken = 0;
    for (size_t i = 0; i < *used_count; ++i) {
        if (strcmp(used[i], desired) == 0) { taken = 1; break; }
    }
    if (!taken) {
        strncpy(out, desired, out_cap);
        out[out_cap - 1] = '\0';
        if (*used_count < used_cap) {
            strncpy(used[*used_count], out, sizeof(used[*used_count]));
            used[*used_count][sizeof(used[*used_count]) - 1] = '\0';
            (*used_count)++;
        }
        return 0;
    }
    // Otherwise append _N.
    for (int n = 2; n < 10000; ++n) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s_%d", desired, n);
        taken = 0;
        for (size_t i = 0; i < *used_count; ++i) {
            if (strcmp(used[i], buf) == 0) { taken = 1; break; }
        }
        if (!taken) {
            strncpy(out, buf, out_cap);
            out[out_cap - 1] = '\0';
            if (*used_count < used_cap) {
                strncpy(used[*used_count], out, sizeof(used[*used_count]));
                used[*used_count][sizeof(used[*used_count]) - 1] = '\0';
                (*used_count)++;
            }
            return 0;
        }
    }
    return -1;
}

static int cc__derive_c_path_from_stem(const char* stem, char* out, size_t cap) {
    if (!stem || !stem[0] || !out || cap == 0) return -1;
    snprintf(out, cap, "%s/%s.c", g_out_root, stem);
    return 0;
}

static int cc__derive_o_path_from_stem(const char* stem, char* out, size_t cap) {
    if (!stem || !stem[0] || !out || cap == 0) return -1;
    snprintf(out, cap, "%s/%s.o", g_host_obj_root[0] ? g_host_obj_root : g_out_root, stem);
    return 0;
}

static int cc__derive_d_path_from_stem(const char* stem, char* out, size_t cap) {
    if (!stem || !stem[0] || !out || cap == 0) return -1;
    snprintf(out, cap, "%s/%s.d", g_host_obj_root[0] ? g_host_obj_root : g_out_root, stem);
    return 0;
}

static void cc__dir_of_path(const char* path, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!path) return;
    strncpy(out, path, cap);
    out[cap - 1] = '\0';
    cc__dirname_inplace(out);
}

static void cc__join_path(const char* dir, const char* rel, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!rel || !rel[0]) return;
    if (cc__is_abs_path(rel)) {
        strncpy(out, rel, cap);
        out[cap - 1] = '\0';
        return;
    }
    if (!dir || !dir[0]) {
        strncpy(out, rel, cap);
        out[cap - 1] = '\0';
        return;
    }
    snprintf(out, cap, "%s/%s", dir, rel);
}

static int cc__is_raw_c(const char* path) {
    return path && cc__ends_with(path, ".c");
}

/* Rewrite `#include … .cch` → `.h` so host cc resolves lowered headers under
 * out/include. Bare `@as` stays in .cch source; host never opens those files. */
static char* cc__rewrite_cch_includes_buf(const char* src, size_t n, int* changed) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0, last_emit = 0, i = 0;
    if (changed) *changed = 0;
    if (!src) return NULL;
    while (i < n) {
        if (i + 5 <= n &&
            (strncmp(src + i, ".cch>", 5) == 0 || strncmp(src + i, ".cch\"", 5) == 0)) {
            char closer = src[i + 4];
            if (!out) {
                out_cap = n + 64;
                out = (char*)malloc(out_cap);
                if (!out) return NULL;
            }
            size_t chunk = i - last_emit;
            if (out_len + chunk + 4 > out_cap) {
                out_cap = (out_len + chunk + 4) * 2;
                char* nb = (char*)realloc(out, out_cap);
                if (!nb) {
                    free(out);
                    return NULL;
                }
                out = nb;
            }
            memcpy(out + out_len, src + last_emit, chunk);
            out_len += chunk;
            out[out_len++] = '.';
            out[out_len++] = 'h';
            out[out_len++] = closer;
            i += 5;
            last_emit = i;
            if (changed) *changed = 1;
            continue;
        }
        i++;
    }
    if (!changed || !*changed) {
        free(out);
        return NULL;
    }
    if (last_emit < n) {
        size_t tail = n - last_emit;
        if (out_len + tail + 1 > out_cap) {
            out_cap = out_len + tail + 1;
            char* nb = (char*)realloc(out, out_cap);
            if (!nb) {
                free(out);
                return NULL;
            }
            out = nb;
        }
        memcpy(out + out_len, src + last_emit, tail);
        out_len += tail;
    }
    out[out_len] = '\0';
    return out;
}

/* Write a host-safe copy of a raw .c TU to dst (.cch includes → .h). Always
 * produces dst (copy when unchanged) so compile uses a stable cache path. */
static int cc__materialize_host_c(const char* src, const char* dst) {
    FILE* in;
    char* buf = NULL;
    char* rewritten = NULL;
    long len;
    size_t nread;
    int changed = 0;
    const char* to_write;
    size_t to_write_len;
    FILE* out;
    char dir[PATH_MAX];

    if (!src || !dst || !src[0] || !dst[0]) return -1;
    if (strcmp(src, dst) == 0) {
        fprintf(stderr,
                "cc: internal error: host materialize refuses in-place rewrite "
                "of %s (would leave .cch includes for host cc)\n",
                src);
        return -1;
    }
    in = fopen(src, "rb");
    if (!in) return -1;
    if (fseek(in, 0, SEEK_END) != 0) {
        fclose(in);
        return -1;
    }
    len = ftell(in);
    if (len < 0 || fseek(in, 0, SEEK_SET) != 0) {
        fclose(in);
        return -1;
    }
    buf = (char*)malloc((size_t)len + 1);
    if (!buf) {
        fclose(in);
        return -1;
    }
    nread = fread(buf, 1, (size_t)len, in);
    fclose(in);
    buf[nread] = '\0';

    rewritten = cc__rewrite_cch_includes_buf(buf, nread, &changed);
    to_write = rewritten ? rewritten : buf;
    to_write_len = rewritten ? strlen(rewritten) : nread;

    cc__dir_of_path(dst, dir, sizeof(dir));
    if (dir[0] && cc__mkdir_p(dir) != 0) {
        free(buf);
        free(rewritten);
        return -1;
    }
    out = fopen(dst, "wb");
    if (!out) {
        free(buf);
        free(rewritten);
        return -1;
    }
    if (fwrite(to_write, 1, to_write_len, out) != to_write_len) {
        fclose(out);
        free(buf);
        free(rewritten);
        return -1;
    }
    fclose(out);
    free(buf);
    free(rewritten);
    return 0;
}

/* Fold a file's *content* into an FNV-1a hash.  Missing/unreadable files fold a
 * stable sentinel so that creating the file later changes the hash. */
static uint64_t cc__fold_file_content(uint64_t h, const char* path) {
    FILE* f = path && path[0] ? fopen(path, "rb") : NULL;
    if (!f) return cc__fnv1a64_str(h, "\x01" "<cc_depends:absent>");
    unsigned char buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) h = cc__fnv1a64_update(h, buf, n);
    fclose(f);
    return h;
}

/* Driver identity for emit keys. `.ccc-bin` mtime is second-granular;
 * extract / header UFCS live in this binary, so fold its bytes. */
static uint64_t cc__fold_ccc_driver(uint64_t h) {
    const char* path = g_ccc_sig_path[0] ? g_ccc_sig_path : g_ccc_path;
    h = cc__fnv1a64_str(h, "\x03" "ccc:");
    if (!path || !path[0]) return cc__fnv1a64_str(h, "<absent>");
    h = cc__fnv1a64_str(h, path);
    return cc__fold_file_content(h, path);
}

/* Lowering identity for emit keys. `ccc` is a wrapper; product emit is
 * `shadow_lower`. Fold that binary's bytes (not mtime): a rebuild with
 * identical size in the same second must still miss. */
static uint64_t cc__fold_shadow_lower(uint64_t h) {
    char path[PATH_MAX];
    path[0] = '\0';
    h = cc__fnv1a64_str(h, "\x03" "shadow_lower:");
    if (cc__find_shadow_lower(path, sizeof(path)) != 0) {
        return cc__fnv1a64_str(h, "<absent>");
    }
    h = cc__fnv1a64_str(h, path);
    return cc__fold_file_content(h, path);
}

/* Public toolchain id (`ccc --version` / seed). Binary folds can miss when
 * find_shadow_lower resolves to the same path both sides of an overwrite,
 * or to "<absent>" from an app cwd. A seed bump must always miss. */
static uint64_t cc__fold_toolchain_id(uint64_t h) {
    char ver[64];
    ver[0] = 0;
    cc_ccc_version_current(ver, sizeof(ver));
    h = cc__fnv1a64_str(h, "\x03" "ccc_version:");
    return cc__fnv1a64_str(h, ver[0] ? ver : "<unknown>");
}

/* Match an identifier token `kw` at `src[p..]` not followed by an ident char. */
static int cc__pp_kw_at(const char* src, size_t n, size_t p, const char* kw) {
    size_t kl = strlen(kw);
    if (p + kl > n || memcmp(src + p, kw, kl) != 0) return 0;
    char after = (p + kl < n) ? src[p + kl] : '\0';
    if (after == '_' ||
        (after >= 'a' && after <= 'z') ||
        (after >= 'A' && after <= 'Z') ||
        (after >= '0' && after <= '9'))
        return 0;
    return 1;
}

/* `#pragma cc_depends("path")` — declared comptime build dependency
 * (COMPTIME_CAPABILITY_MODEL.md §2).  Scans the source for line-anchored
 * directives, resolves each path relative to the source file's directory, and
 * folds the dependency's *content* into the emit cache key so that editing a
 * comptime-read input file forces a re-emit.  This is the only build-graph
 * effect of a comptime file read; plain file calls carry no build semantics, so
 * the dependency must be declared explicitly. */
static uint64_t cc__fold_cc_depends(uint64_t h, const char* in_path) {
    if (!in_path || !in_path[0]) return h;
    FILE* f = fopen(in_path, "rb");
    if (!f) return h;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024 * 1024) { fclose(f); return h; }
    char* src = (char*)malloc((size_t)sz + 1);
    if (!src) { fclose(f); return h; }
    size_t rd = fread(src, 1, (size_t)sz, f);
    fclose(f);
    src[rd] = '\0';

    char dir[PATH_MAX];
    cc__dir_of_path(in_path, dir, sizeof(dir));

    size_t i = 0;
    while (i < rd) {
        size_t p = i;
        while (p < rd && (src[p] == ' ' || src[p] == '\t')) p++;
        if (p < rd && src[p] == '#') {
            p++;
            while (p < rd && (src[p] == ' ' || src[p] == '\t')) p++;
            if (cc__pp_kw_at(src, rd, p, "pragma")) {
                p += 6;
                while (p < rd && (src[p] == ' ' || src[p] == '\t')) p++;
                if (cc__pp_kw_at(src, rd, p, "cc_depends")) {
                    p += 10;
                    while (p < rd && (src[p] == ' ' || src[p] == '\t')) p++;
                    if (p < rd && src[p] == '(') {
                        p++;
                        while (p < rd && (src[p] == ' ' || src[p] == '\t')) p++;
                        if (p < rd && src[p] == '"') {
                            char rel[PATH_MAX];
                            size_t o = 0;
                            p++;
                            while (p < rd && src[p] != '"' && o + 1 < sizeof(rel)) rel[o++] = src[p++];
                            rel[o] = '\0';
                            char abs[PATH_MAX];
                            cc__join_path(dir, rel, abs, sizeof(abs));
                            h = cc__fnv1a64_str(h, "\x02" "cc_depends:");
                            h = cc__fnv1a64_str(h, rel);
                            h = cc__fold_file_content(h, abs);
                        }
                    }
                }
            }
        }
        while (i < rd && src[i] != '\n') i++;
        if (i < rd) i++;
    }
    free(src);
    return h;
}

/* `#include`d `.cch` files are lowered into the TU. The emit key used to
 * hash only the `.ccs` and `#pragma cc_depends`, so editing a header reused
 * stale C. Host `.d` files then rebuilt `.o` from that C against the new
 * header — clang blamed the `.cch` line for a call that was not there. */
#define CC_CCH_INC_CAP 256

typedef struct {
    char seen[CC_CCH_INC_CAP][PATH_MAX];
    int n;
    const char* flags;
} CCCchFold;

static int cc__path_is_cch(const char* p) {
    size_t n = p ? strlen(p) : 0;
    return n >= 4 && strcmp(p + (n - 4), ".cch") == 0;
}

static int cc__cch_seen(const CCCchFold* st, const char* path) {
    int i;
    if (!st || !path) return 0;
    for (i = 0; i < st->n; i++) {
        if (strcmp(st->seen[i], path) == 0) return 1;
    }
    return 0;
}

static void cc__cch_note(CCCchFold* st, const char* path) {
    if (!st || !path || !path[0] || st->n >= CC_CCH_INC_CAP) return;
    snprintf(st->seen[st->n], PATH_MAX, "%s", path);
    st->n++;
}

static int cc__try_cch_file(const char* dir, const char* rel, char* out,
                            size_t cap) {
    char cand[PATH_MAX];
    if (!rel || !rel[0] || !out || cap < 2) return 0;
    if (rel[0] == '/') {
        snprintf(cand, sizeof(cand), "%s", rel);
    } else if (dir && dir[0]) {
        cc__join_path(dir, rel, cand, sizeof(cand));
    } else {
        snprintf(cand, sizeof(cand), "%s", rel);
    }
    if (access(cand, R_OK) != 0) return 0;
    if (realpath(cand, out) == NULL)
        snprintf(out, cap, "%s", cand);
    return 1;
}

static int cc__resolve_cch_include(const char* from_file, const char* rel,
                                   int quoted, const char* flags, char* out,
                                   size_t cap) {
    char dir[PATH_MAX];
    const char* p;
    cc__dir_of_path(from_file, dir, sizeof(dir));
    if (quoted && cc__try_cch_file(dir, rel, out, cap)) return 1;
    p = flags ? flags : "";
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] == '-' && p[1] == 'I') {
            const char* s = p + 2;
            char inc[PATH_MAX];
            size_t n = 0;
            if (*s == ' ' || *s == '\t') {
                s++;
                while (*s == ' ' || *s == '\t') s++;
            }
            while (*s && *s != ' ' && *s != '\t' && n + 1 < sizeof(inc))
                inc[n++] = *s++;
            inc[n] = 0;
            if (n && cc__try_cch_file(inc, rel, out, cap)) return 1;
            p = s;
            continue;
        }
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    if (g_cc_include[0] && cc__try_cch_file(g_cc_include, rel, out, cap))
        return 1;
    if (g_cc_lowered_include[0] &&
        cc__try_cch_file(g_cc_lowered_include, rel, out, cap))
        return 1;
    return 0;
}

static uint64_t cc__fold_cch_includes_rec(uint64_t h, const char* path,
                                          CCCchFold* st) {
    FILE* f;
    long sz;
    char* src;
    size_t rd, i;
    if (!path || !path[0] || !st) return h;
    f = fopen(path, "rb");
    if (!f) return h;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024 * 1024) {
        fclose(f);
        return h;
    }
    src = (char*)malloc((size_t)sz + 1);
    if (!src) {
        fclose(f);
        return h;
    }
    rd = fread(src, 1, (size_t)sz, f);
    fclose(f);
    src[rd] = '\0';
    i = 0;
    while (i < rd) {
        size_t p = i;
        int quoted = 0;
        char rel[PATH_MAX];
        size_t o = 0;
        char abs[PATH_MAX];
        char q;
        while (p < rd && (src[p] == ' ' || src[p] == '\t')) p++;
        if (p < rd && src[p] == '#') {
            p++;
            while (p < rd && (src[p] == ' ' || src[p] == '\t')) p++;
            if (cc__pp_kw_at(src, rd, p, "include")) {
                p += 7;
                while (p < rd && (src[p] == ' ' || src[p] == '\t')) p++;
                if (p < rd && (src[p] == '"' || src[p] == '<')) {
                    quoted = (src[p] == '"');
                    q = quoted ? '"' : '>';
                    p++;
                    while (p < rd && src[p] != q && o + 1 < sizeof(rel))
                        rel[o++] = src[p++];
                    rel[o] = '\0';
                    if (cc__path_is_cch(rel) &&
                        cc__resolve_cch_include(path, rel, quoted, st->flags,
                                                abs, sizeof(abs)) &&
                        !cc__cch_seen(st, abs)) {
                        cc__cch_note(st, abs);
                        h = cc__fnv1a64_str(h, "\x03" "cch:");
                        h = cc__fnv1a64_str(h, abs);
                        h = cc__fold_file_content(h, abs);
                        h = cc__fold_cch_includes_rec(h, abs, st);
                    }
                }
            }
        }
        while (i < rd && src[i] != '\n') i++;
        if (i < rd) i++;
    }
    free(src);
    return h;
}

static uint64_t cc__fold_cch_includes(uint64_t h, const char* in_path,
                                      const char* cc_flags) {
    CCCchFold st;
    char abs[PATH_MAX];
    memset(&st, 0, sizeof(st));
    st.flags = cc_flags;
    if (!in_path || !in_path[0]) return h;
    if (realpath(in_path, abs) != NULL)
        cc__cch_note(&st, abs);
    else
        cc__cch_note(&st, in_path);
    return cc__fold_cch_includes_rec(h, in_path, &st);
}

typedef struct {
    char path[PATH_MAX];
    off_t sz;
} CCPrefetchCcs;

static int cc__prefetch_ccs_sz_cmp(const void* a, const void* b) {
    const CCPrefetchCcs* x = (const CCPrefetchCcs*)a;
    const CCPrefetchCcs* y = (const CCPrefetchCcs*)b;
    if (y->sz > x->sz) return 1;
    if (y->sz < x->sz) return -1;
    return 0;
}

typedef struct {
    pid_t pid[24];
    int st[24];
    int done[24];
    int n;
} CCKeepKids;

static void cc__keep_add(CCKeepKids* k, pid_t p) {
    if (!k || p <= 0 || k->n >= (int)(sizeof(k->pid) / sizeof(k->pid[0])))
        return;
    k->pid[k->n] = p;
    k->st[k->n] = 0;
    k->done[k->n] = 0;
    k->n++;
}

static int cc__keep_note(CCKeepKids* k, pid_t p, int st) {
    int i;
    if (!k) return 0;
    for (i = 0; i < k->n; i++) {
        if (k->pid[i] == p) {
            k->st[i] = st;
            k->done[i] = 1;
            return 1;
        }
    }
    return 0;
}

static int cc__keep_wait_all(CCKeepKids* k) {
    int i, rc = 0;
    if (!k) return 0;
    for (i = 0; i < k->n; i++) {
        if (!k->done[i]) {
            if (waitpid(k->pid[i], &k->st[i], 0) < 0) {
                k->st[i] = -1;
                rc = -1;
            }
            k->done[i] = 1;
        }
        if (k->st[i] < 0 || !WIFEXITED(k->st[i]) || WEXITSTATUS(k->st[i]) != 0)
            rc = -1;
    }
    return rc;
}

/* Start include-graph prefetch in the background. TU workers begin
 * immediately; flock still serializes a shared first writer. Fat units
 * first. Prefetch failure is not fatal — emit is the source of truth. */
static int cc__prefetch_ccs_start(char paths[][PATH_MAX], int n, int jobs,
                                  CCKeepKids* keep) {
    CCPrefetchCcs* items;
    int w, k;
    if (n <= 0) return 0;
    if (jobs < 1) jobs = 1;
    if (jobs > 64) jobs = 64;
    if (jobs > n) jobs = n;
    items = (CCPrefetchCcs*)calloc((size_t)n, sizeof(CCPrefetchCcs));
    if (!items) return -1;
    for (k = 0; k < n; k++) {
        struct stat st;
        snprintf(items[k].path, PATH_MAX, "%s", paths[k]);
        items[k].sz = (stat(paths[k], &st) == 0) ? st.st_size : 0;
    }
    qsort(items, (size_t)n, sizeof(items[0]), cc__prefetch_ccs_sz_cmp);
    for (w = 0; w < jobs; w++) {
        pid_t pid = fork();
        if (pid < 0) {
            free(items);
            return -1;
        }
        if (pid == 0) {
            int r = 0;
            for (k = w; k < n; k += jobs) {
                long long t = cc__now_ms();
                if (cc_prefetch_lower_ccs_includes(items[k].path) != 0) {
                    r = 1;
                    break;
                }
                cc__prof_span_arg("prefetch_ccs", items[k].path, t);
            }
            _exit(r);
        }
        cc__keep_add(keep, pid);
    }
    free(items);
    return 0;
}

static int cc__copy_file(const char* src, const char* dst) {
    if (!src || !dst || !src[0] || !dst[0]) return -1;
    FILE* in = fopen(src, "rb");
    if (!in) return -1;
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    unsigned char buf[64 * 1024];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return -1; }
    }
    fclose(in);
    fclose(out);
    return 0;
}

static int cc__stat_mtime_before(const struct stat* a, const struct stat* b) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
    if (a->st_mtimespec.tv_sec != b->st_mtimespec.tv_sec)
        return a->st_mtimespec.tv_sec < b->st_mtimespec.tv_sec;
    return a->st_mtimespec.tv_nsec < b->st_mtimespec.tv_nsec;
#elif defined(__linux__)
    if (a->st_mtim.tv_sec != b->st_mtim.tv_sec)
        return a->st_mtim.tv_sec < b->st_mtim.tv_sec;
    return a->st_mtim.tv_nsec < b->st_mtim.tv_nsec;
#else
    return a->st_mtime < b->st_mtime;
#endif
}

static int cc__deps_require_rebuild(const char* dep_path, const char* obj_path) {
    if (!dep_path || !obj_path) return 1;
    struct stat st_obj;
    if (stat(obj_path, &st_obj) != 0) return 1;
    FILE* f = fopen(dep_path, "rb");
    if (!f) return 1;

    // Extremely simple dep parser: read whole file, strip continuations, skip until ':', then tokenize.
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return 0; } // empty/too big → don't force rebuild
    char* text = (char*)malloc((size_t)sz + 1);
    if (!text) { fclose(f); return 0; }
    size_t rd = fread(text, 1, (size_t)sz, f);
    fclose(f);
    text[rd] = '\0';

    // Remove backslash-newline continuations.
    for (size_t i = 0; i + 1 < rd; ++i) {
        if (text[i] == '\\' && (text[i + 1] == '\n' || text[i + 1] == '\r')) {
            text[i] = ' ';
            text[i + 1] = ' ';
        }
    }

    char* p = strchr(text, ':');
    if (!p) { free(text); return 0; }
    p++; // after ':'

    int rebuild = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        char* start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        char saved = *p;
        *p = '\0';

        struct stat st_dep;
        if (stat(start, &st_dep) != 0) {
            rebuild = 1;
        } else if (cc__stat_mtime_before(&st_obj, &st_dep)) {
            rebuild = 1;
        }

        *p = saved;
        if (rebuild) break;
    }

    free(text);
    return rebuild;
}

static void cc__merge_target_compile_flags(const CCBuildTargetDecl* t, const char* build_dir,
                                          char* cc_flags, size_t cc_cap) {
    if (!t) return;
    // include dirs
    for (size_t i = 0; i < t->include_dir_count; ++i) {
        char inc_abs[PATH_MAX];
        cc__join_path(build_dir, t->include_dirs[i], inc_abs, sizeof(inc_abs));
        cc__append_flag(cc_flags, cc_cap, " -I", inc_abs);
    }
    // defines
    for (size_t i = 0; i < t->define_count; ++i) {
        cc__append_flag(cc_flags, cc_cap, " -D", t->defines[i]);
    }
    // raw cflags
    if (t->cflags && t->cflags[0]) cc__append_spaced(cc_flags, cc_cap, t->cflags);
}

static int cc__is_lib_path(const char* lib) {
    // Check if lib looks like a path (contains / or ends with .a/.so/.dylib)
    if (!lib) return 0;
    if (strchr(lib, '/') != NULL) return 1;
    size_t len = strlen(lib);
    if (len > 2 && strcmp(lib + len - 2, ".a") == 0) return 1;
    if (len > 3 && strcmp(lib + len - 3, ".so") == 0) return 1;
    if (len > 6 && strcmp(lib + len - 6, ".dylib") == 0) return 1;
    return 0;
}

static void cc__merge_target_link_flags(const CCBuildTargetDecl* t, char* ld_flags, size_t ld_cap) {
    if (!t) return;
    if (t->ldflags && t->ldflags[0]) cc__append_spaced(ld_flags, ld_cap, t->ldflags);
    for (size_t i = 0; i < t->lib_count; ++i) {
        const char* lib = t->libs[i];
        if (!lib || !lib[0]) continue;
        if (lib[0] == '-') {
            // Already has flag prefix (-lm, -L/path, etc.)
            cc__append_spaced(ld_flags, ld_cap, lib);
        } else if (cc__is_lib_path(lib)) {
            // Path to library file - pass directly
            cc__append_spaced(ld_flags, ld_cap, lib);
        } else {
            // Short library name - add -l prefix
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "-l%s", lib);
            cc__append_spaced(ld_flags, ld_cap, tmp);
        }
    }
}

// Helper to add a library to ld_flags, avoiding duplicates
static void cc__add_lib_to_flags(const char* lib, char* ld_flags, size_t ld_cap) {
    if (!lib || !lib[0] || !ld_flags) return;
    
    char flag[280];
    if (cc__is_lib_path(lib)) {
        snprintf(flag, sizeof(flag), "%s", lib);
    } else {
        snprintf(flag, sizeof(flag), "-l%s", lib);
    }
    // Simple duplicate check
    if (!strstr(ld_flags, flag)) {
        cc__append_spaced(ld_flags, ld_cap, flag);
    }
}

// Scan text buffer for @link directives and add to ld_flags
static void cc__scan_for_link_directives(const char* content, size_t len, char* ld_flags, size_t ld_cap) {
    if (!content || len == 0 || !ld_flags) return;
    
    const char* marker = "__CC_LINK__ ";
    size_t marker_len = strlen(marker);
    const char* link_pat = "@link(\"";
    size_t link_pat_len = strlen(link_pat);
    
    const char* p = content;
    const char* end = content + len;
    
    while (p < end) {
        // Look for __CC_LINK__ marker
        const char* pos = strstr(p, marker);
        if (pos && pos < end) {
            pos += marker_len;
            const char* lib_end = pos;
            while (lib_end < end && *lib_end && *lib_end != ' ' && *lib_end != '*' && *lib_end != '\n') lib_end++;
            if (lib_end > pos) {
                size_t lib_len = (size_t)(lib_end - pos);
                char lib[256];
                if (lib_len < sizeof(lib)) {
                    memcpy(lib, pos, lib_len);
                    lib[lib_len] = '\0';
                    cc__add_lib_to_flags(lib, ld_flags, ld_cap);
                }
            }
            p = lib_end;
            continue;
        }
        
        // Look for @link("lib") pattern
        pos = strstr(p, link_pat);
        if (pos && pos < end) {
            pos += link_pat_len;
            const char* quote_end = strchr(pos, '"');
            if (quote_end && quote_end < end && quote_end > pos) {
                size_t lib_len = (size_t)(quote_end - pos);
                char lib[256];
                if (lib_len < sizeof(lib)) {
                    memcpy(lib, pos, lib_len);
                    lib[lib_len] = '\0';
                    cc__add_lib_to_flags(lib, ld_flags, ld_cap);
                }
            }
            p = quote_end ? quote_end + 1 : end;
            continue;
        }
        
        // No more patterns found
        break;
    }
}

// Extract @link directives from generated .c file.
// Runs the C preprocessor to expand includes, then scans for:
//   1. Markers: __CC_LINK__ libname
//   2. Comment directives: @link("libname") (anywhere, including in comments)
// Adds discovered libs to ld_flags buffer.
static void cc__extract_link_directives(const char* c_file_path, const char* include_flags, 
                                        char* ld_flags, size_t ld_cap) {
    if (!c_file_path || !ld_flags) return;
    
    // First scan the .c file directly (for markers we've already rewritten)
    FILE* f = fopen(c_file_path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len > 0) {
            char* content = (char*)malloc((size_t)len + 1);
            if (content) {
                size_t nread = fread(content, 1, (size_t)len, f);
                content[nread] = '\0';
                cc__scan_for_link_directives(content, nread, ld_flags, ld_cap);
                free(content);
            }
        }
        fclose(f);
    }
    
    // Run preprocessor to expand includes and scan that too
    char cmd[2048];
    const char* inc = include_flags ? include_flags : "";
    snprintf(cmd, sizeof(cmd), "cc -E %s \"%s\" 2>/dev/null", inc, c_file_path);
    
    FILE* pp = popen(cmd, "r");
    if (!pp) return;
    
    // Read preprocessed output
    size_t cap = 64 * 1024;
    size_t len = 0;
    char* buf = (char*)malloc(cap);
    if (buf) {
        char chunk[4096];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), pp)) > 0) {
            if (len + n >= cap) {
                cap *= 2;
                char* newbuf = (char*)realloc(buf, cap);
                if (!newbuf) break;
                buf = newbuf;
            }
            memcpy(buf + len, chunk, n);
            len += n;
        }
        buf[len] = '\0';
        cc__scan_for_link_directives(buf, len, ld_flags, ld_cap);
        free(buf);
    }
    
    pclose(pp);
}

// Post-process generated .c file to rewrite @link("lib") to markers.
// This handles @link directives that come from included headers.
static int cc__postprocess_link_directives(const char* c_file_path) {
    if (!c_file_path) return -1;
    
    // Read the file
    FILE* f = fopen(c_file_path, "rb");
    if (!f) return -1;
    
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (len <= 0) {
        fclose(f);
        return 0;  // Empty file, nothing to do
    }
    
    char* content = (char*)malloc((size_t)len + 1);
    if (!content) {
        fclose(f);
        return -1;
    }
    
    size_t nread = fread(content, 1, (size_t)len, f);
    fclose(f);
    content[nread] = '\0';
    
    // Rewrite @link directives
    char* rewritten = cc__rewrite_link_directives(content, nread);
    if (!rewritten) {
        // No changes needed
        free(content);
        return 0;
    }
    
    // Write back
    f = fopen(c_file_path, "wb");
    if (!f) {
        free(content);
        free(rewritten);
        return -1;
    }
    
    size_t rewritten_len = strlen(rewritten);
    fwrite(rewritten, 1, rewritten_len, f);
    fclose(f);
    
    free(content);
    free(rewritten);
    return 0;
}

typedef struct {
    int state; // 0=unseen, 1=building, 2=done
    size_t obj_count;
    char** obj_paths;  // heap-allocated array of heap-allocated strings
    uint64_t* obj_keys; // heap-allocated
} CCTargetObjCache;

static int cc__find_target_idx(const CCBuildTargetDecl* targets, size_t target_count, const char* name) {
    if (!targets || !name) return -1;
    for (size_t i = 0; i < target_count; ++i) {
        if (targets[i].name && strcmp(targets[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static int g_build_jobs = 4; /* ccc build -jN; 0 → ncpu; default 4 */

static int cc__ncpu(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    return (int)n;
}

static int cc__resolve_build_jobs(int jobs) {
    if (jobs <= 0) return cc__ncpu();
    if (jobs > 64) return 64;
    return jobs;
}

static void cc__make_cross_parts(const CCBuildTargetDecl* t,
                                 const char* cli_target,
                                 const char* cli_sysroot,
                                 char* target_part,
                                 size_t target_part_cap,
                                 char* sysroot_part,
                                 size_t sysroot_part_cap) {
    target_part[0] = '\0';
    sysroot_part[0] = '\0';
    const char* tgt = (t && t->target_triple && t->target_triple[0]) ? t->target_triple : cli_target;
    const char* sys = (t && t->sysroot && t->sysroot[0]) ? t->sysroot : cli_sysroot;
    if (tgt && tgt[0]) snprintf(target_part, target_part_cap, "--target %s", tgt);
    if (sys && sys[0]) snprintf(sysroot_part, sysroot_part_cap, "--sysroot %s", sys);
}

/* Emit+compile one target's sources. Caller ensures deps are done and owns
 * caches[idx].state. Returns 0 on success. */
static int cc__build_one_target_objs(int idx,
                                     const CCBuildTargetDecl* targets,
                                     size_t target_count,
                                     const char* build_dir,
                                     const CCCompileConfig* cfg,
                                     const CCBuildOptions* base_cli_opt,
                                     const char* cli_target,
                                     const char* cli_sysroot,
                                     const CCFileSig* build_sig_for_key,
                                     const CCFileSig* cc_sig_for_key,
                                     int cache_ok,
                                     CCTargetObjCache* caches) {
    if (idx < 0 || (size_t)idx >= target_count) return -1;
    const CCBuildTargetDecl* t = &targets[(size_t)idx];

    char t_target_part[256];
    char t_sysroot_part[256];
    cc__make_cross_parts(t, cli_target, cli_sysroot, t_target_part, sizeof(t_target_part), t_sysroot_part, sizeof(t_sysroot_part));

    char t_cc_flags[2048];
    t_cc_flags[0] = '\0';
    cc__merge_target_compile_flags(t, build_dir, t_cc_flags, sizeof(t_cc_flags));
    if (base_cli_opt && base_cli_opt->cc_flags && base_cli_opt->cc_flags[0]) cc__append_spaced(t_cc_flags, sizeof(t_cc_flags), base_cli_opt->cc_flags);

    char c_dir[PATH_MAX];
    char o_dir[PATH_MAX];
    uint64_t build_id_u64 = cc__hash_build_dir_u64(build_dir);
    char build_id_hex[32];
    cc__format_u64_hex(build_id_hex, sizeof(build_id_hex), build_id_u64);
    snprintf(c_dir, sizeof(c_dir), "%s/c/%s/%s", g_out_root, build_id_hex, t->name);
    snprintf(o_dir, sizeof(o_dir), "%s/obj/%s/%s",
             g_host_obj_root[0] ? g_host_obj_root : g_out_root, build_id_hex, t->name);
    if (cc__mkdir_p(c_dir) != 0 || cc__mkdir_p(o_dir) != 0) return -1;

    caches[idx].obj_count = 0;
    if (!caches[idx].obj_paths) {
        caches[idx].obj_paths = (char**)calloc(128, sizeof(char*));
        caches[idx].obj_keys = (uint64_t*)calloc(128, sizeof(uint64_t));
        if (!caches[idx].obj_paths || !caches[idx].obj_keys) return -1;
    }

    for (size_t si = 0; si < t->src_count; ++si) {
        if (caches[idx].obj_count >= 128) return -1;
        char src_abs[PATH_MAX];
        cc__join_path(build_dir, t->srcs[si], src_abs, sizeof(src_abs));

        char stem0[128];
        cc__stem_from_path(src_abs, stem0, sizeof(stem0));
        uint64_t src_id_u64 = cc__hash_src_path_u64(build_dir, src_abs);
        char src_id_hex[32];
        cc__format_u64_hex(src_id_hex, sizeof(src_id_hex), src_id_u64);
        char unit[256];
        snprintf(unit, sizeof(unit), "%s__%s", stem0, src_id_hex);

        char c_out[PATH_MAX];
        char o_out[PATH_MAX];
        char d_out[PATH_MAX];
        if (snprintf(c_out, sizeof(c_out), "%s/%s.c", c_dir, unit) <= 0) return -1;
        if (snprintf(o_out, sizeof(o_out), "%s/%s.o", o_dir, unit) <= 0) return -1;
        if (snprintf(d_out, sizeof(d_out), "%s/%s.d", o_dir, unit) <= 0) return -1;

        char src_dir[PATH_MAX];
        cc__dir_of_path(src_abs, src_dir, sizeof(src_dir));

        const int is_raw_c = cc__is_raw_c(src_abs);
        if (is_raw_c) {
            if (cc__materialize_host_c(src_abs, c_out) != 0) {
                fprintf(stderr, "cc: failed to materialize host C %s -> %s\n",
                        src_abs, c_out);
                return -1;
            }
        }
        const char* c_for_compile = c_out;

        char cache_stem[256];
        snprintf(cache_stem, sizeof(cache_stem), "%s__%s__%s", build_id_hex, t->name, unit);
        char meta_path[PATH_MAX];
        char obj_meta_path[PATH_MAX];
        snprintf(meta_path, sizeof(meta_path), "%s/%s.meta", g_cache_root, cache_stem);
        snprintf(obj_meta_path, sizeof(obj_meta_path), "%s/%s.obj",
                 g_host_obj_root[0] ? g_host_obj_root : g_cache_root, cache_stem);

        uint64_t emit_key = 0;
        long long t_emit = cc__now_ms();
        int emit_reused = 0;
        if (!is_raw_c) {
            if (cache_ok) {
                CCFileSig in_sig;
                in_sig.mtime_sec = 0;
                in_sig.size = 0;
                (void)cc__stat_sig(src_abs, &in_sig);
                uint64_t h = 1469598103934665603ULL;
                h = cc__fnv1a64_str(h, src_abs);
                h = cc__fnv1a64_i64(h, in_sig.mtime_sec);
                h = cc__fnv1a64_i64(h, in_sig.size);
                h = cc__fold_file_content(h, src_abs);
                h = cc__fnv1a64_i64(h, build_sig_for_key ? build_sig_for_key->mtime_sec : 0);
                h = cc__fnv1a64_i64(h, build_sig_for_key ? build_sig_for_key->size : 0);
                h = cc__fnv1a64_i64(h, cc_sig_for_key ? cc_sig_for_key->mtime_sec : 0);
                h = cc__fnv1a64_i64(h, cc_sig_for_key ? cc_sig_for_key->size : 0);
                h = cc__fnv1a64_str(h, t_target_part);
                h = cc__fnv1a64_str(h, t_sysroot_part);
                h = cc__fnv1a64_str(h, t_cc_flags);
                h = cc__fnv1a64_str(h, getenv("CFLAGS"));
                h = cc__fnv1a64_str(h, getenv("CPPFLAGS"));
                h = cc__fnv1a64_str(h, getenv("CC_STRICT_RESULT_UNWRAP"));
                if (cfg) {
                    h = cc__fnv1a64_i64(h, (long long)cfg->const_count);
                    for (size_t bi = 0; bi < cfg->const_count; ++bi) {
                        h = cc__fnv1a64_str(h, cfg->consts[bi].name);
                        h = cc__fnv1a64_i64(h, cfg->consts[bi].value);
                    }
                }
                h = cc__fold_cc_depends(h, src_abs);
                h = cc__fold_cch_includes(h, src_abs, t_cc_flags);
                h = cc__fold_ccc_driver(h);
                h = cc__fold_shadow_lower(h);
                h = cc__fold_toolchain_id(h);
                emit_key = h;
                uint64_t prev = 0;
                if (file_exists(c_out) && cc__read_u64_file(meta_path, &prev) == 0 && prev == emit_key) {
                    cc__replay_diag_sidecar(c_out);
                    emit_reused = 1;
                } else {
                    int err = cc__compile_with_env(base_cli_opt, src_abs, c_out, cfg);
                    if (err != 0) return err;
                    (void)cc__write_u64_file(meta_path, emit_key);
                }
            } else {
                int err = cc__compile_with_env(base_cli_opt, src_abs, c_out, cfg);
                if (err != 0) return err;
            }
        }
        cc__prof_span_arg(emit_reused ? "tu_emit_reuse" : "tu_emit", src_abs, t_emit);

        if (base_cli_opt && base_cli_opt->mode != CC_MODE_EMIT_C) {
            uint64_t obj_key = 0;
            long long t_obj = cc__now_ms();
            int obj_reused = 0;
            if (cache_ok) {
                uint64_t h = 1469598103934665603ULL;
                if (is_raw_c) {
                    CCFileSig in_sig;
                    in_sig.mtime_sec = 0;
                    in_sig.size = 0;
                    (void)cc__stat_sig(src_abs, &in_sig);
                    h = cc__fnv1a64_str(h, src_abs);
                    h = cc__fnv1a64_i64(h, in_sig.mtime_sec);
                    h = cc__fnv1a64_i64(h, in_sig.size);
                } else {
                    h = cc__fnv1a64_i64(h, (long long)emit_key);
                }
                h = cc__fnv1a64_str(h, t_target_part);
                h = cc__fnv1a64_str(h, t_sysroot_part);
                h = cc__fnv1a64_str(h, t_cc_flags);
                h = cc__fnv1a64_str(h, getenv("CFLAGS"));
                h = cc__fnv1a64_str(h, getenv("CPPFLAGS"));
                h = cc__fnv1a64_str(h, g_host_fp);
                obj_key = h;
                uint64_t prev = 0;
                if (file_exists(o_out) && cc__read_u64_file(obj_meta_path, &prev) == 0 && prev == obj_key && !cc__deps_require_rebuild(d_out, o_out)) {
                    obj_reused = 1;
                } else {
                    CCBuildOptions opt = *base_cli_opt;
                    opt.in_path = src_abs;
                    opt.cc_flags = t_cc_flags[0] ? t_cc_flags : base_cli_opt->cc_flags;
                    if (cc__compile_c_to_obj(&opt, c_for_compile, o_out, d_out, src_dir, t_target_part, t_sysroot_part) != 0) return -1;
                    (void)cc__write_u64_file(obj_meta_path, obj_key);
                }
            } else {
                CCBuildOptions opt = *base_cli_opt;
                opt.in_path = src_abs;
                opt.cc_flags = t_cc_flags[0] ? t_cc_flags : base_cli_opt->cc_flags;
                if (cc__compile_c_to_obj(&opt, c_for_compile, o_out, d_out, src_dir, t_target_part, t_sysroot_part) != 0) return -1;
            }
            cc__prof_span_arg(obj_reused ? "tu_obj_reuse" : "tu_obj", src_abs, t_obj);

            caches[idx].obj_paths[caches[idx].obj_count] = strdup(o_out);
            caches[idx].obj_keys[caches[idx].obj_count] = obj_key;
            caches[idx].obj_count++;
        }
    }
    return 0;
}

static int cc__write_target_job_manifest(const char* o_dir, const CCTargetObjCache* cache) {
    char path[PATH_MAX];
    FILE* f;
    size_t i;
    if (!o_dir || !cache) return -1;
    snprintf(path, sizeof(path), "%s/.cc_job_objs", o_dir);
    f = fopen(path, "w");
    if (!f) return -1;
    for (i = 0; i < cache->obj_count; ++i) {
        if (!cache->obj_paths[i]) continue;
        fprintf(f, "%llu\t%s\n", (unsigned long long)cache->obj_keys[i], cache->obj_paths[i]);
    }
    fclose(f);
    return 0;
}

static int cc__read_target_job_manifest(const char* o_dir, CCTargetObjCache* cache) {
    char path[PATH_MAX];
    char line[PATH_MAX + 64];
    FILE* f;
    if (!o_dir || !cache) return -1;
    snprintf(path, sizeof(path), "%s/.cc_job_objs", o_dir);
    f = fopen(path, "r");
    if (!f) {
        /* Emit-C-only builds write an empty/absent manifest. */
        cache->obj_count = 0;
        return 0;
    }
    if (!cache->obj_paths) {
        cache->obj_paths = (char**)calloc(128, sizeof(char*));
        cache->obj_keys = (uint64_t*)calloc(128, sizeof(uint64_t));
        if (!cache->obj_paths || !cache->obj_keys) {
            fclose(f);
            return -1;
        }
    }
    cache->obj_count = 0;
    while (fgets(line, sizeof(line), f) && cache->obj_count < 128) {
        unsigned long long key = 0;
        char obj[PATH_MAX];
        if (sscanf(line, "%llu\t%1023s", &key, obj) != 2) continue;
        cache->obj_paths[cache->obj_count] = strdup(obj);
        cache->obj_keys[cache->obj_count] = (uint64_t)key;
        cache->obj_count++;
    }
    fclose(f);
    return 0;
}

static void cc__target_obj_dir(const CCBuildTargetDecl* t, const char* build_dir,
                               char* o_dir, size_t o_cap) {
    uint64_t build_id_u64 = cc__hash_build_dir_u64(build_dir);
    char build_id_hex[32];
    cc__format_u64_hex(build_id_hex, sizeof(build_id_hex), build_id_u64);
    snprintf(o_dir, o_cap, "%s/obj/%s/%s",
             g_host_obj_root[0] ? g_host_obj_root : g_out_root, build_id_hex, t->name);
}

static int cc__mark_needed_targets(int idx, const CCBuildTargetDecl* targets,
                                   size_t target_count, unsigned char* needed,
                                   unsigned char* walk) {
    size_t di;
    if (idx < 0 || (size_t)idx >= target_count) return -3;
    if (walk[idx] == 1) return -2; /* cycle */
    if (needed[idx]) return 0;
    walk[idx] = 1;
    for (di = 0; di < targets[idx].dep_count; ++di) {
        int d = cc__find_target_idx(targets, target_count, targets[idx].deps[di]);
        if (d < 0) return -3;
        int r = cc__mark_needed_targets(d, targets, target_count, needed, walk);
        if (r != 0) return r;
    }
    walk[idx] = 2;
    needed[idx] = 1;
    return 0;
}

/* Parallel scheduler for needed TUs. CC_TARGET_DEPS is a link-set (and
 * cycle check), not a compile barrier: emit+cc of layout does not need
 * document.o. Fail-loud: first child failure SIGTERMs the rest. */
/* waitpid(-1) that does not steal overlapped runtime/prefetch children. */
static pid_t cc__wait_build_job(const pid_t* known, int nknown, int* status,
                                CCKeepKids* keep) {
    for (;;) {
        int st = 0;
        pid_t p = waitpid(-1, &st, 0);
        int i;
        if (p < 0) return -1;
        if (cc__keep_note(keep, p, st)) continue;
        for (i = 0; i < nknown; i++) {
            if (known[i] == p) {
                if (status) *status = st;
                return p;
            }
        }
    }
}

static off_t cc__target_src_bytes(const CCBuildTargetDecl* t,
                                  const char* build_dir) {
    off_t sum = 0;
    size_t si;
    if (!t) return 0;
    for (si = 0; si < t->src_count; si++) {
        char abs[PATH_MAX];
        struct stat st;
        if (!t->srcs[si]) continue;
        cc__join_path(build_dir, t->srcs[si], abs, sizeof(abs));
        if (stat(abs, &st) == 0) sum += st.st_size;
    }
    return sum;
}

static int cc__build_target_objs_parallel(int chosen_idx,
                                          const CCBuildTargetDecl* targets,
                                          size_t target_count,
                                          const char* build_dir,
                                          const CCCompileConfig* cfg,
                                          const CCBuildOptions* base_cli_opt,
                                          const char* cli_target,
                                          const char* cli_sysroot,
                                          const CCFileSig* build_sig_for_key,
                                          const CCFileSig* cc_sig_for_key,
                                          int cache_ok,
                                          CCTargetObjCache* caches,
                                          int jobs,
                                          CCKeepKids* keep) {
    unsigned char needed[64];
    unsigned char walk[64];
    typedef struct {
        pid_t pid;
        int idx;
    } CCBuildJob;
    CCBuildJob running[64];
    int nrun = 0;
    int remaining = 0;
    int fail_rc = 0;
    size_t i;

    if (jobs < 1) jobs = 1;
    if (chosen_idx < 0 || (size_t)chosen_idx >= target_count) return -1;
    memset(needed, 0, sizeof(needed));
    memset(walk, 0, sizeof(walk));
    {
        int mr = cc__mark_needed_targets(chosen_idx, targets, target_count, needed, walk);
        if (mr != 0) return mr;
    }
    for (i = 0; i < target_count; ++i)
        if (needed[i]) remaining++;

    while (remaining > 0 || nrun > 0) {
        while (nrun < jobs && fail_rc == 0) {
            int pick = -1;
            off_t pick_sz = -1;
            for (i = 0; i < target_count; ++i) {
                off_t sz;
                if (!needed[i] || caches[i].state != 0) continue;
                sz = cc__target_src_bytes(&targets[i], build_dir);
                if (pick < 0 || sz > pick_sz) {
                    pick = (int)i;
                    pick_sz = sz;
                }
            }
            if (pick < 0) break;

            caches[pick].state = 1;
            {
                pid_t pid = fork();
                if (pid < 0) {
                    perror("cc: fork");
                    fail_rc = -1;
                    caches[pick].state = 0;
                    break;
                }
                if (pid == 0) {
                    int r = cc__build_one_target_objs(
                        pick, targets, target_count, build_dir, cfg, base_cli_opt,
                        cli_target, cli_sysroot, build_sig_for_key, cc_sig_for_key,
                        cache_ok, caches);
                    if (r == 0) {
                        char o_dir[PATH_MAX];
                        cc__target_obj_dir(&targets[pick], build_dir, o_dir, sizeof(o_dir));
                        if (cc__write_target_job_manifest(o_dir, &caches[pick]) != 0) r = -1;
                    }
                    _exit(r == 0 ? 0 : 1);
                }
                running[nrun].pid = pid;
                running[nrun].idx = pick;
                nrun++;
            }
        }

        if (nrun == 0) {
            if (remaining > 0 && fail_rc == 0) return -2; /* cycle / stuck */
            break;
        }

        {
            int status = 0;
            pid_t known[64];
            pid_t done;
            int slot = -1;
            int idx = -1;
            for (i = 0; i < (size_t)nrun; ++i) known[i] = running[i].pid;
            done = cc__wait_build_job(known, nrun, &status, keep);
            if (done < 0) {
                perror("cc: waitpid");
                fail_rc = -1;
                break;
            }
            for (i = 0; i < (size_t)nrun; ++i) {
                if (running[i].pid == done) {
                    slot = (int)i;
                    idx = running[i].idx;
                    break;
                }
            }
            if (slot < 0) continue;
            running[slot] = running[nrun - 1];
            nrun--;

            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                fail_rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                if (fail_rc == 0) fail_rc = -1;
                caches[idx].state = 0;
                /* Cancel siblings. */
                for (i = 0; i < (size_t)nrun; ++i) kill(running[i].pid, SIGTERM);
                while (nrun > 0) {
                    pid_t kp[64];
                    int st = 0;
                    pid_t p;
                    int k;
                    for (k = 0; k < nrun; k++) kp[k] = running[k].pid;
                    p = cc__wait_build_job(kp, nrun, &st, keep);
                    if (p < 0) break;
                    for (i = 0; i < (size_t)nrun; ++i) {
                        if (running[i].pid == p) {
                            running[i] = running[nrun - 1];
                            nrun--;
                            break;
                        }
                    }
                }
                return fail_rc;
            }

            {
                char o_dir[PATH_MAX];
                cc__target_obj_dir(&targets[idx], build_dir, o_dir, sizeof(o_dir));
                if (cc__read_target_job_manifest(o_dir, &caches[idx]) != 0) {
                    fail_rc = -1;
                    return fail_rc;
                }
            }
            caches[idx].state = 2;
            remaining--;
        }
    }
    return fail_rc;
}

static int cc__build_target_objs_rec(int idx,
                                     const CCBuildTargetDecl* targets,
                                     size_t target_count,
                                     const char* build_dir,
                                     const CCCompileConfig* cfg,
                                     const CCBuildOptions* base_cli_opt,
                                     const char* cli_target,
                                     const char* cli_sysroot,
                                     const CCFileSig* build_sig_for_key,
                                     const CCFileSig* cc_sig_for_key,
                                     int cache_ok,
                                     CCTargetObjCache* caches,
                                     char chain[][128],
                                     size_t chain_len) {
    if (idx < 0 || (size_t)idx >= target_count) return -1;
    if (caches[idx].state == 1) return -2; // cycle
    if (caches[idx].state == 2) return 0;
    caches[idx].state = 1;

    const CCBuildTargetDecl* t = &targets[(size_t)idx];

    /* Recurse deps first (serial path; -j uses the parallel scheduler). */
    for (size_t di = 0; di < t->dep_count; ++di) {
        int d = cc__find_target_idx(targets, target_count, t->deps[di]);
        if (d < 0) return -3;
        if (chain_len < 64) strncpy(chain[chain_len], t->deps[di], sizeof(chain[chain_len]) - 1);
        int r = cc__build_target_objs_rec(d, targets, target_count, build_dir, cfg, base_cli_opt, cli_target, cli_sysroot,
                                          build_sig_for_key, cc_sig_for_key, cache_ok, caches, chain, chain_len + 1);
        if (r != 0) return r;
    }

    {
        int r = cc__build_one_target_objs(idx, targets, target_count, build_dir, cfg, base_cli_opt,
                                          cli_target, cli_sysroot, build_sig_for_key, cc_sig_for_key,
                                          cache_ok, caches);
        if (r != 0) return r;
    }

    caches[idx].state = 2;
    (void)chain;
    return 0;
}

static int cc__gather_obj_closure(int idx,
                                 const CCBuildTargetDecl* targets,
                                 size_t target_count,
                                 const CCTargetObjCache* caches,
                                 unsigned char* vis,
                                 const char** out_paths,
                                 uint64_t* out_keys,
                                 size_t* io_count,
                                 size_t cap) {
    if (idx < 0 || (size_t)idx >= target_count) return -1;
    if (vis[idx]) return 0;
    vis[idx] = 1;
    const CCBuildTargetDecl* t = &targets[(size_t)idx];
    for (size_t di = 0; di < t->dep_count; ++di) {
        int d = cc__find_target_idx(targets, target_count, t->deps[di]);
        if (d < 0) return -3;
        int r = cc__gather_obj_closure(d, targets, target_count, caches, vis, out_paths, out_keys, io_count, cap);
        if (r != 0) return r;
    }
    for (size_t oi = 0; oi < caches[idx].obj_count; ++oi) {
        if (*io_count >= cap) return -4;
        out_paths[*io_count] = caches[idx].obj_paths[oi];
        if (out_keys) out_keys[*io_count] = caches[idx].obj_keys[oi];
        (*io_count)++;
    }
    return 0;
}

static int cc__load_const_bindings(const CCBuildOptions* opt, CCConstBinding* bindings, size_t* count);
static void cc__print_comptime_targets(const char* build_path);
static void cc__print_comptime_state(const CCBuildOptions* opt, const char* build_path, const CCConstBinding* bindings, size_t count);

/* ccc is native-only (shadow_lower). The legacy multipass front is removed;
 * `--frontend=legacy` / `CC_FRONTEND=legacy` are hard errors. */

static int cc__set_frontend_name(const char* v) {
    if (!v || !v[0]) return -1;
    if (strcmp(v, "native") == 0) return 0;
    if (strcmp(v, "legacy") == 0) {
        fprintf(stderr,
                "cc: --frontend=legacy has been removed; the legacy multipass "
                "front no longer exists. ccc is native-only (omit --frontend "
                "or pass --frontend=native).\n");
        exit(2);
    }
    fprintf(stderr, "cc: --frontend must be native (got %s); the legacy front "
                    "has been removed\n", v);
    return -1;
}

/* Native front is the only front. Retained so call sites read clearly; a
 * `CC_FRONTEND=legacy` in the environment is a hard error, not a silent
 * fallback. */
static int cc__want_native_front(void) {
    const char* e = getenv("CC_FRONTEND");
    if (e && e[0] && strcmp(e, "native") != 0) {
        if (strcmp(e, "legacy") == 0) {
            fprintf(stderr,
                    "cc: CC_FRONTEND=legacy has been removed; the legacy "
                    "multipass front no longer exists. ccc is native-only.\n");
        } else {
            fprintf(stderr, "cc: CC_FRONTEND must be native (got %s)\n", e);
        }
        exit(2);
    }
    return 1;
}

static const char* cc__version_string(void) {
    static char buf[64];
    cc_ccc_version_current(buf, sizeof(buf));
    return buf;
}

static void cc__print_version(void) {
    printf("ccc %s\n", cc__version_string());
}

static int cc__arg_is_version(const char* a) {
    return a && (strcmp(a, "--version") == 0 || strcmp(a, "--v") == 0 ||
                 strcmp(a, "-V") == 0);
}

/* Scan so `ccc --frontend=legacy --version` hard-errors before printing.
 * Also rejects `CC_FRONTEND=legacy` in the environment (same hard error).
 * Returns -1 on unknown frontend name (already diagnosed). */
static int cc__scan_frontend_flags(int argc, char** argv) {
    int i;
    (void)cc__want_native_front(); /* env hard-error */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frontend") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "cc: --frontend requires native\n");
                return -1;
            }
            i++;
            if (cc__set_frontend_name(argv[i]) != 0) return -1;
            continue;
        }
        if (strncmp(argv[i], "--frontend=", 11) == 0) {
            if (cc__set_frontend_name(argv[i] + 11) != 0) return -1;
        }
    }
    return 0;
}

/* Resolve native shadow_lower beside ccc (not the ccc-run wrapper).
 * Never take a cwd-relative `out/cc/bin/shadow_lower`: from an app tree that
 * is a miss or a stale copy, and the emit key then fails to track the
 * lowerer that exec actually runs. */
static int cc__find_shadow_lower(char* dst, size_t cap) {
    const char* env = getenv("CC_SHADOW_LOWER");
    if (env && env[0] && access(env, X_OK) == 0) {
        snprintf(dst, cap, "%s", env);
        return 0;
    }
    /* Same directory as the running ccc (install: $PREFIX/bin/ccc). */
    if (g_ccc_path[0]) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s", g_ccc_path);
        cc__dirname_inplace(dir);
        if (dir[0] &&
            (size_t)snprintf(dst, cap, "%s/shadow_lower", dir) < cap &&
            access(dst, X_OK) == 0)
            return 0;
    }
    /* Prefix install: $PREFIX/bin/ccc + $PREFIX/bin/shadow_lower. */
    if (g_layout_installed && g_repo_root[0]) {
        snprintf(dst, cap, "%s/bin/shadow_lower", g_repo_root);
        if (access(dst, X_OK) == 0) return 0;
    }
    if (g_repo_root[0]) {
        snprintf(dst, cap, "%s/out/cc/bin/shadow_lower", g_repo_root);
        if (access(dst, X_OK) == 0) return 0;
        snprintf(dst, cap, "%s/cc/bin/shadow_lower", g_repo_root);
        if (access(dst, X_OK) == 0) return 0;
    }
    return -1;
}

/* Append build.cc CC_CONST bindings as host -D flags. CLI -D names are skipped
 * because build-mode already folded them into opt->cc_flags. */
static int cc__append_build_cc_defines(char* buf, size_t cap, size_t* cflen,
                                       const CCBuildOptions* opt,
                                       const CCConstBinding* bindings,
                                       size_t count) {
    size_t i, j;
    if (!buf || !cflen || !opt || (!bindings && count > 0)) return -1;
    for (i = 0; i < count; ++i) {
        int is_cli = 0;
        int n;
        if (!bindings[i].name || !bindings[i].name[0]) continue;
        for (j = 0; j < opt->cli_count; ++j) {
            if (opt->cli_names[j] &&
                strcmp(opt->cli_names[j], bindings[i].name) == 0) {
                is_cli = 1;
                break;
            }
        }
        if (is_cli) continue;
        if (bindings[i].value == 1) {
            n = snprintf(buf + *cflen, cap - *cflen, "%s-D%s",
                         *cflen ? " " : "", bindings[i].name);
        } else {
            n = snprintf(buf + *cflen, cap - *cflen, "%s-D%s=%lld",
                         *cflen ? " " : "", bindings[i].name,
                         bindings[i].value);
        }
        if (n < 0 || (size_t)n >= cap - *cflen) {
            fprintf(stderr, "cc: build.cc -D flags too long for native forward\n");
            return -1;
        }
        *cflen += (size_t)n;
    }
    return 0;
}

/* .shcc → content-keyed .ccs for native shadow_lower (prelude / main /
 * default @errhandler / @task). Host compile stays on whatever CC= is. */
static int cc__materialize_shcc_for_native(const char* shcc_path, char* out_ccs,
                                           size_t cap) {
    char* raw = NULL;
    char* rewritten = NULL;
    size_t raw_len = 0;
    size_t rw_len = 0;
    uint64_t h;
    char dir[PATH_MAX];
    char path[PATH_MAX];
    if (!shcc_path || !out_ccs || !cap) return -1;
    out_ccs[0] = '\0';
    raw = cc__read_all_file(shcc_path, &raw_len);
    if (!raw) {
        fprintf(stderr, "cc: cannot read %s\n", shcc_path);
        return -1;
    }
    cc_script_set_no_line(g_no_line);
    rewritten = cc_script_rewrite_source(shcc_path, raw, raw_len, &rw_len);
    free(raw);
    if (!rewritten) {
        fprintf(stderr, "cc: .shcc rewrite failed for %s\n", shcc_path);
        return -1;
    }
    /* Same naked print→cc_* alias as legacy canonicalize (shadow has no
     * preprocessor pass for it). Member UFCS left untouched. */
    {
        char* prints = cc_rewrite_naked_print_aliases(rewritten, rw_len);
        if (prints) {
            free(rewritten);
            rewritten = prints;
            rw_len = strlen(rewritten);
        }
    }
    h = 1469598103934665603ULL;
    h = cc__fnv1a64_str(h, shcc_path);
    h = cc__fnv1a64_update(h, rewritten, rw_len);
    snprintf(dir, sizeof(dir), "%s/shcc_native", g_cache_root);
    if (cc__mkdir_p(dir) != 0) {
        free(rewritten);
        return -1;
    }
    /* Content key only. PID is on the temp inside cc__install_wrap_file. */
    snprintf(path, sizeof(path), "%s/%016llx.ccs", dir, (unsigned long long)h);
    if (cc__install_wrap_file(path, rewritten, rw_len) != 0) {
        free(rewritten);
        return -1;
    }
    free(rewritten);
    if (strlen(path) + 1 > cap) {
        fprintf(stderr, "cc: shcc wrap path too long\n");
        return -1;
    }
    snprintf(out_ccs, cap, "%s", path);
    return 0;
}

/* Strip a recognized #!ccc / OS-shebang unit header so last-good shadow_lower
 * (extension-based) never sees the magic line. Stamp #line so diagnostics
 * still name the original file. */
static int cc__materialize_strip_header(const char* in_path, CCUnitKind kind,
                                        char* out_path, size_t cap) {
    char* raw = NULL;
    size_t raw_len = 0;
    size_t skip = 0;
    uint64_t h;
    char dir[PATH_MAX];
    char path[PATH_MAX];
    char line_dir[PATH_MAX + 64];
    const char* ext = (kind == CC_UNIT_KIND_CCH) ? "cch" : "ccs";
    int nline;
    if (!in_path || !out_path || !cap) return -1;
    out_path[0] = '\0';
    raw = cc__read_all_file(in_path, &raw_len);
    if (!raw) {
        fprintf(stderr, "cc: cannot read %s\n", in_path);
        return -1;
    }
    skip = cc_unit_header_skip(raw, raw_len);
    if (skip == 0) {
        free(raw);
        if (strlen(in_path) + 1 > cap) {
            fprintf(stderr, "cc: input path too long\n");
            return -1;
        }
        snprintf(out_path, cap, "%s", in_path);
        return 0;
    }
    if (g_no_line) {
        nline = 0;
        line_dir[0] = '\0';
    } else {
        nline = snprintf(line_dir, sizeof(line_dir), "#line 2 \"%s\"\n", in_path);
        if (nline < 0 || (size_t)nline >= sizeof(line_dir)) {
            free(raw);
            fprintf(stderr, "cc: #line path too long for %s\n", in_path);
            return -1;
        }
    }
    h = 1469598103934665603ULL;
    h = cc__fnv1a64_str(h, in_path);
    h = cc__fnv1a64_update(h, line_dir, (size_t)nline);
    h = cc__fnv1a64_update(h, raw + skip, raw_len - skip);
    snprintf(dir, sizeof(dir), "%s/unit_native", g_cache_root);
    if (cc__mkdir_p(dir) != 0) {
        free(raw);
        return -1;
    }
    snprintf(path, sizeof(path), "%s/%016llx.%s", dir, (unsigned long long)h, ext);
    {
        size_t body = (raw_len > skip) ? (raw_len - skip) : 0;
        size_t total = (size_t)nline + body;
        char* blob = (char*)malloc(total ? total : 1);
        if (!blob) {
            free(raw);
            return -1;
        }
        if (nline > 0)
            memcpy(blob, line_dir, (size_t)nline);
        if (body)
            memcpy(blob + (size_t)nline, raw + skip, body);
        if (cc__install_wrap_file(path, blob, total) != 0) {
            free(blob);
            free(raw);
            return -1;
        }
        free(blob);
    }
    free(raw);
    if (strlen(path) + 1 > cap) {
        fprintf(stderr, "cc: unit wrap path too long\n");
        return -1;
    }
    snprintf(out_path, cap, "%s", path);
    return 0;
}

/* Resolve pin to a bootstrap folder name. A prefix that matches the running
 * toolchain uses that pin; otherwise the newest matching seed folder. */
static int cc__bootstrap_pin_folder(const char* pin, char* folder, size_t cap) {
    char current[64];
    char best[64];
    char dirpath[PATH_MAX];
    char seed_c[PATH_MAX];
    DIR* d;
    struct dirent* de;
    struct stat st;
    int have_best = 0;

    if (!pin || !pin[0] || !folder || !cap) return -1;
    cc_ccc_version_current(current, sizeof(current));
    if (cc_ccc_version_matches(pin, current)) {
        if (strlen(current) + 1 > cap) return -1;
        snprintf(folder, cap, "%s", current);
        return 0;
    }
    if (!g_repo_root[0]) return -1;
    snprintf(dirpath, sizeof(dirpath), "%s/cc/bootstrap/shadow_lower",
             g_repo_root);
    d = opendir(dirpath);
    if (!d) return -1;
    best[0] = '\0';
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!cc_ccc_version_matches(pin, de->d_name)) continue;
        snprintf(seed_c, sizeof(seed_c), "%s/%s/shadow_lower.c", dirpath,
                 de->d_name);
        if (stat(seed_c, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (!have_best || cc_ccc_version_cmp(de->d_name, best) > 0) {
            snprintf(best, sizeof(best), "%s", de->d_name);
            have_best = 1;
        }
    }
    closedir(d);
    if (!have_best) return -1;
    if (strlen(best) + 1 > cap) return -1;
    snprintf(folder, cap, "%s", best);
    return 0;
}

/* Host-cc a bootstrap seed's prelowered shadow_lower.c when the pin does not
 * match the running toolchain. Cache under out/.cc-build/lowerers/<folder>/. */
static int cc__ensure_pinned_shadow_lower(const char* pin, char* dst, size_t cap) {
    char current[64];
    char folder[64];
    char seed_c[PATH_MAX];
    char out_bin[PATH_MAX];
    char cmd[PATH_MAX * 3];
    struct stat st_src, st_bin;
    int have_src, have_bin;

    if (!pin || !pin[0]) return cc__find_shadow_lower(dst, cap);
    if (!cc_ccc_version_spec_ok(pin)) {
        fprintf(stderr, "cc: invalid version pin %s\n", pin);
        return -1;
    }
    if (cc__bootstrap_pin_folder(pin, folder, sizeof(folder)) != 0) {
        fprintf(stderr, "cc: version pin %s: missing bootstrap seed %s\n",
                pin, pin);
        return -1;
    }
    cc_ccc_version_current(current, sizeof(current));
    if (cc_ccc_version_equal(folder, current))
        return cc__find_shadow_lower(dst, cap);

    if (!g_repo_root[0]) {
        fprintf(stderr, "cc: version pin %s: cannot locate bootstrap seeds\n", pin);
        return -1;
    }
    snprintf(seed_c, sizeof(seed_c),
             "%s/cc/bootstrap/shadow_lower/%s/shadow_lower.c", g_repo_root,
             folder);
    if (access(seed_c, R_OK) != 0) {
        fprintf(stderr,
                "cc: version pin %s: missing bootstrap seed %s (%s)\n",
                pin, folder, seed_c);
        return -1;
    }
    snprintf(out_bin, sizeof(out_bin), "%s/lowerers/%s/shadow_lower",
             g_cache_root, folder);
    have_src = stat(seed_c, &st_src) == 0;
    have_bin = stat(out_bin, &st_bin) == 0 && access(out_bin, X_OK) == 0;
    if (have_src && have_bin && st_bin.st_mtime >= st_src.st_mtime) {
        if (strlen(out_bin) + 1 > cap) return -1;
        snprintf(dst, cap, "%s", out_bin);
        return 0;
    }
    {
        int n = snprintf(cmd, sizeof(cmd),
                         "make -C \"%s/cc\" shadow_lower-pin PIN_VER=%s "
                         "PIN_OUT=\"%s\"",
                         g_repo_root, folder, out_bin);
        if (n < 0 || (size_t)n >= sizeof(cmd)) {
            fprintf(stderr, "cc: version pin %s: make command too long\n", pin);
            return -1;
        }
    }
    if (system(cmd) != 0) {
        fprintf(stderr,
                "cc: version pin %s: failed to host-cc bootstrap seed %s\n",
                pin, folder);
        return -1;
    }
    if (access(out_bin, X_OK) != 0) {
        fprintf(stderr, "cc: version pin %s: missing pinned lowerer %s\n",
                pin, out_bin);
        return -1;
    }
    if (strlen(out_bin) + 1 > cap) return -1;
    snprintf(dst, cap, "%s", out_bin);
    return 0;
}

/* Delegate .ccs/.shcc build/emit to native shadow_lower (owns cache + host-cc/link).
 * Driver loads build.cc / CLI -D, handles dumps/dry-run, and forwards host
 * flags. Options contract: forward, handle here, or hard error — never drop.
 * .shcc is rewritten to a temp .ccs first (script entry); host CC is unchanged. */
static int cc__run_shadow_lower(const CCBuildOptions* opt, const char* out_path) {
    long long t_fn = cc__now_ms();
    long long t_span;
    char shadow[PATH_MAX];
    char cc_flags_buf[2048];
    char cc_flags_arg[2200];
    char ld_flags_arg[2048];
    char shcc_wrap[PATH_MAX];
    char unit_wrap[PATH_MAX];
    char orig_in[PATH_MAX];
    char quote_dir[PATH_MAX];
    int set_quote = 0;
    CCBuildOptions opt_local;
    char* argv[28];
    int argc = 0;
    pid_t pid;
    int status;
    size_t cflen = 0;
    CCConstBinding bindings[128];
    size_t binding_count = 0;
    CCUnitKind kind = CC_UNIT_KIND_UNKNOWN;
    char pin[CC_CCC_VERSION_PIN_CAP];
    char resolve_err[256];
    if (!opt || !opt->in_path || !out_path) return -1;

    orig_in[0] = '\0';
    quote_dir[0] = '\0';
    snprintf(orig_in, sizeof(orig_in), "%s", opt->in_path);

    pin[0] = '\0';
    if (cc_unit_resolve(opt->in_path, opt->unit_kind, opt->ccc_version_pin,
                        &kind, pin, resolve_err, sizeof(resolve_err)) != 0) {
        fprintf(stderr, "%s\n", resolve_err);
        return -1;
    }
    if (kind == CC_UNIT_KIND_CCH &&
        (opt->mode == CC_MODE_LINK || opt->mode == CC_MODE_COMPILE)) {
        fprintf(stderr,
                "cc: %s is a header unit (cch); use --emit-c-only to lower "
                "to .h (cannot compile/link a header)\n",
                opt->in_path);
        return -1;
    }

    if (kind == CC_UNIT_KIND_SHCC) {
        t_span = cc__now_ms();
        if (cc__materialize_shcc_for_native(opt->in_path, shcc_wrap,
                                            sizeof(shcc_wrap)) != 0)
            return -1;
        cc__prof_span("wrap_shcc", t_span);
        opt_local = *opt;
        opt_local.in_path = shcc_wrap;
        opt = &opt_local;
        set_quote = 1;
    } else if (kind == CC_UNIT_KIND_CCS || kind == CC_UNIT_KIND_CCH) {
        t_span = cc__now_ms();
        if (cc__materialize_strip_header(opt->in_path, kind, unit_wrap,
                                         sizeof(unit_wrap)) != 0)
            return -1;
        cc__prof_span("wrap_unit", t_span);
        if (strcmp(unit_wrap, opt->in_path) != 0) {
            opt_local = *opt;
            opt_local.in_path = unit_wrap;
            opt = &opt_local;
            set_quote = 1;
        }
    }
    if (set_quote && orig_in[0])
        cc__dir_of_path(orig_in, quote_dir, sizeof(quote_dir));

    t_span = cc__now_ms();
    if (cc__load_const_bindings(opt, bindings, &binding_count) != 0) return -1;
    cc__prof_span("const_bindings", t_span);
    if (opt->dump_consts && !opt->dump_comptime) {
        for (size_t i = 0; i < binding_count; ++i) {
            printf("CONST %s=%lld\n", bindings[i].name, bindings[i].value);
        }
    }
    if (opt->dry_run) return 0;

    t_span = cc__now_ms();
    if (cc__ensure_pinned_shadow_lower(pin[0] ? pin : opt->ccc_version_pin,
                                      shadow, sizeof(shadow)) != 0) {
        if (!pin[0] && !(opt->ccc_version_pin && opt->ccc_version_pin[0])) {
            fprintf(stderr,
                    "cc: native front requires shadow_lower "
                    "(checkout: make -C cc; install: $PREFIX/bin/shadow_lower)\n");
        }
        return -1;
    }
    cc__prof_span("find_shadow_lower", t_span);
    /* Installed / non-prebuilt layouts: build or locate concurrent_c.o and
     * hand it to shadow_lower (it only probes checkout-relative paths). */
    t_span = cc__now_ms();
    {
        char runtime_obj[PATH_MAX];
        int runtime_reused = 0;
        size_t out_n = out_path ? strlen(out_path) : 0;
        /* Emit-C (-o foo.c) does not host_build. Skip the ~1s --release
         * runtime.o so build-graph workers can overlap it with lowering. */
        int emit_c = (out_n >= 2 && out_path[out_n - 2] == '.' &&
                      out_path[out_n - 1] == 'c');
        const char* target_part =
            (opt->target_flag && opt->target_flag[0]) ? opt->target_flag : NULL;
        const char* sysroot_part =
            (opt->sysroot_flag && opt->sysroot_flag[0]) ? opt->sysroot_flag : NULL;
        if (!opt->no_runtime && !emit_c &&
            cc__ensure_runtime_obj(opt, target_part, sysroot_part, runtime_obj,
                                   sizeof(runtime_obj), &runtime_reused) != 0)
            return -1;
        if (!opt->no_runtime && !emit_c && runtime_obj[0]) {
            if (setenv("SHADOW_RUNTIME_O", runtime_obj, 1) != 0) {
                fprintf(stderr, "cc: setenv SHADOW_RUNTIME_O failed\n");
                return -1;
            }
        } else if (!emit_c) {
            unsetenv("SHADOW_RUNTIME_O");
        }
        (void)runtime_reused;
        cc__prof_span_arg(emit_c ? "ensure_runtime_skip" : "ensure_runtime",
                          opt->in_path, t_span);
    }
    /* Fold --target / --sysroot / existing cc_flags (incl. CLI -D) + build.cc. */
    cc_flags_buf[0] = 0;
    if (opt->cc_flags && opt->cc_flags[0]) {
        cflen = strlen(opt->cc_flags);
        if (cflen >= sizeof(cc_flags_buf)) cflen = sizeof(cc_flags_buf) - 1;
        memcpy(cc_flags_buf, opt->cc_flags, cflen);
        cc_flags_buf[cflen] = 0;
    }
    if (cc__append_build_cc_defines(cc_flags_buf, sizeof(cc_flags_buf), &cflen,
                                    opt, bindings, binding_count) != 0)
        return -1;
#if defined(__linux__)
    /* ILP32 readdir needs 64-bit off_t on Docker volumes (EOVERFLOW otherwise). */
    {
        int want_fob64 = (sizeof(void*) == 4);
        size_t bi;
        if (!want_fob64) {
            for (bi = 0; bi < binding_count; ++bi) {
                if (bindings[bi].name &&
                    strcmp(bindings[bi].name, "TARGET_PTR_WIDTH") == 0 &&
                    bindings[bi].value == 32) {
                    want_fob64 = 1;
                    break;
                }
            }
        }
        if (want_fob64 &&
            (!opt->cc_flags || !strstr(opt->cc_flags, "_FILE_OFFSET_BITS"))) {
            int n = snprintf(cc_flags_buf + cflen, sizeof(cc_flags_buf) - cflen,
                             "%s-D_FILE_OFFSET_BITS=64", cflen ? " " : "");
            if (n < 0 || (size_t)n >= sizeof(cc_flags_buf) - cflen) {
                fprintf(stderr, "cc: _FILE_OFFSET_BITS flag too long\n");
                return -1;
            }
            cflen += (size_t)n;
        }
    }
#endif
    if (opt->target_flag && opt->target_flag[0]) {
        int n = snprintf(cc_flags_buf + cflen, sizeof(cc_flags_buf) - cflen,
                         "%s--target %s", cflen ? " " : "", opt->target_flag);
        if (n < 0 || (size_t)n >= sizeof(cc_flags_buf) - cflen) {
            fprintf(stderr, "cc: --target/--cc-flags too long for native forward\n");
            return -1;
        }
        cflen += (size_t)n;
    }
    if (opt->sysroot_flag && opt->sysroot_flag[0]) {
        int n = snprintf(cc_flags_buf + cflen, sizeof(cc_flags_buf) - cflen,
                         "%s--sysroot %s", cflen ? " " : "", opt->sysroot_flag);
        if (n < 0 || (size_t)n >= sizeof(cc_flags_buf) - cflen) {
            fprintf(stderr, "cc: --sysroot/--cc-flags too long for native forward\n");
            return -1;
        }
        cflen += (size_t)n;
    }
    /* Absolute include roots: installed prefix has no checkout-relative
     * out/include or cc/include for shadow_lower's hardcoded -I probes. */
    if (g_cc_lowered_include[0] && file_exists(g_cc_lowered_include)) {
        int n = snprintf(cc_flags_buf + cflen, sizeof(cc_flags_buf) - cflen,
                         "%s-I%s", cflen ? " " : "", g_cc_lowered_include);
        if (n < 0 || (size_t)n >= sizeof(cc_flags_buf) - cflen) {
            fprintf(stderr, "cc: include path too long for native forward\n");
            return -1;
        }
        cflen += (size_t)n;
    }
    if (g_cc_include[0] && file_exists(g_cc_include) &&
        (!g_cc_lowered_include[0] ||
         strcmp(g_cc_include, g_cc_lowered_include) != 0)) {
        int n = snprintf(cc_flags_buf + cflen, sizeof(cc_flags_buf) - cflen,
                         "%s-I%s", cflen ? " " : "", g_cc_include);
        if (n < 0 || (size_t)n >= sizeof(cc_flags_buf) - cflen) {
            fprintf(stderr, "cc: include path too long for native forward\n");
            return -1;
        }
        cflen += (size_t)n;
    }
    if (quote_dir[0]) {
        int n = snprintf(cc_flags_buf + cflen, sizeof(cc_flags_buf) - cflen,
                         "%s-I%s", cflen ? " " : "", quote_dir);
        if (n < 0 || (size_t)n >= sizeof(cc_flags_buf) - cflen) {
            fprintf(stderr, "cc: quote-dir include path too long\n");
            return -1;
        }
        cflen += (size_t)n;
    }
    {
        const char* sr = getenv("CC_SYSROOT");
        char abs[PATH_MAX];
        char probe[PATH_MAX];
        const char* use;
        if (sr && sr[0]) {
            use = realpath(sr, abs) ? abs : sr;
            if ((size_t)snprintf(probe, sizeof(probe), "%s/usr/include", use) <
                    sizeof(probe) &&
                access(probe, R_OK) == 0) {
                int n = snprintf(cc_flags_buf + cflen, sizeof(cc_flags_buf) - cflen,
                                 "%s-I%s", cflen ? " " : "", probe);
                if (n < 0 || (size_t)n >= sizeof(cc_flags_buf) - cflen) {
                    fprintf(stderr, "cc: sysroot include path too long\n");
                    return -1;
                }
                cflen += (size_t)n;
            }
        }
    }
    (void)cflen;
    argv[argc++] = shadow;
    if (opt->no_cache) argv[argc++] = (char*)"--no-cache";
    if (opt->verbose) argv[argc++] = (char*)"--verbose";
    if (opt->opt_release) argv[argc++] = (char*)"--release";
    if (opt->opt_debug) argv[argc++] = (char*)"--debug";
    if (opt->no_runtime) argv[argc++] = (char*)"--no-runtime";
    if (cc_flags_buf[0]) {
        snprintf(cc_flags_arg, sizeof(cc_flags_arg), "--cc-flags=%s",
                 cc_flags_buf);
        argv[argc++] = cc_flags_arg;
    }
    if (opt->ld_flags && opt->ld_flags[0]) {
        snprintf(ld_flags_arg, sizeof(ld_flags_arg), "--ld-flags=%s",
                 opt->ld_flags);
        argv[argc++] = ld_flags_arg;
    }
    argv[argc++] = (char*)opt->in_path;
    argv[argc++] = (char*)"-o";
    argv[argc++] = (char*)out_path;
    argv[argc] = NULL;
    if (opt->verbose) {
        fprintf(stderr, "cc: native:");
        for (int i = 0; argv[i]; ++i) fprintf(stderr, " %s", argv[i]);
        fprintf(stderr, "\n");
    }
    t_span = cc__now_ms();
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "cc: fork failed for shadow_lower\n");
        return -1;
    }
    if (pid == 0) {
        char ver[64];
        ver[0] = 0;
        cc_ccc_version_current(ver, sizeof(ver));
        if (ver[0] && setenv("CCC_VERSION", ver, 1) != 0)
            _exit(127);
        if (quote_dir[0] && setenv("SHADOW_QUOTE_DIR", quote_dir, 1) != 0)
            _exit(127);
        execv(shadow, argv);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "cc: shadow_lower failed (rc=%d)\n",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return -1;
    }
    cc__prof_span_arg("shadow_lower", opt->in_path, t_span);
    t_span = cc__now_ms();
    if (cc__finish_emit_c(orig_in[0] ? orig_in : opt->in_path, out_path,
                          opt->mode == CC_MODE_EMIT_C) != 0)
        return -1;
    cc__prof_span_arg("finish_emit", opt->in_path, t_span);
    cc__prof_span_arg("run_shadow_lower", opt->in_path, t_fn);
    return 0;
}

// Core compile helper shared by default and build modes.
static int compile_with_build(const CCBuildOptions* opt, CCBuildSummary* summary_out) {
    if (!opt || !opt->in_path || !opt->c_out_path) {
        fprintf(stderr, "cc: missing input or c_out_path\n");
        return -1;
    }
    /* Native front: delegate CC units to shadow_lower.
     * Scripts are rewritten to a temp .ccs inside cc__run_shadow_lower.
     * --compile = emit C via shadow_lower, then host cc -c (driver-side).
     * Py modules keep the caller's -fPIC/-shared flags; shadow_lower forwards. */
    if (cc__want_native_front()) {
        CCUnitKind uk = CC_UNIT_KIND_UNKNOWN;
        char pin[CC_CCC_VERSION_PIN_CAP];
        char uerr[256];
        pin[0] = '\0';
        if (cc_unit_resolve(opt->in_path, opt->unit_kind, opt->ccc_version_pin,
                            &uk, pin, uerr, sizeof(uerr)) != 0) {
            fprintf(stderr, "%s\n", uerr);
            return -1;
        }
        if (uk == CC_UNIT_KIND_CCS || uk == CC_UNIT_KIND_SHCC ||
            uk == CC_UNIT_KIND_CCH) {
        if (opt->mode == CC_MODE_LINK && opt->bin_out_path) {
            const char* one[1];
            one[0] = opt->in_path;
            if (cc_check_link_set_faces(one, 1) != 0) return -1;
            if (summary_out) {
                memset(summary_out, 0, sizeof(*summary_out));
                summary_out->bin_out_path = opt->bin_out_path;
            }
            return cc__run_shadow_lower(opt, opt->bin_out_path);
        }
        if (opt->mode == CC_MODE_EMIT_C) {
            if (summary_out) {
                memset(summary_out, 0, sizeof(*summary_out));
                summary_out->c_out_path = opt->c_out_path;
                summary_out->did_emit_c = 1;
            }
            return cc__run_shadow_lower(opt, opt->c_out_path);
        }
        if (opt->mode == CC_MODE_COMPILE) {
            char src_dir[PATH_MAX];
            char target_part[256];
            char sysroot_part[256];
            const char* slash;
            if (!opt->obj_out_path) {
                fprintf(stderr, "cc: --compile requires an object output path\n");
                return -1;
            }
            if (summary_out) {
                memset(summary_out, 0, sizeof(*summary_out));
                summary_out->c_out_path = opt->c_out_path;
                summary_out->obj_out_path = opt->obj_out_path;
                summary_out->did_emit_c = 1;
            }
            if (cc__run_shadow_lower(opt, opt->c_out_path) != 0) return -1;
            if (opt->dry_run) return 0;
            src_dir[0] = 0;
            slash = strrchr(opt->in_path, '/');
            if (slash && slash > opt->in_path) {
                size_t n = (size_t)(slash - opt->in_path);
                if (n + 1 < sizeof(src_dir)) {
                    memcpy(src_dir, opt->in_path, n);
                    src_dir[n] = 0;
                }
            }
            target_part[0] = 0;
            sysroot_part[0] = 0;
            if (opt->target_flag && opt->target_flag[0])
                snprintf(target_part, sizeof(target_part), "--target %s",
                         opt->target_flag);
            if (opt->sysroot_flag && opt->sysroot_flag[0])
                snprintf(sysroot_part, sizeof(sysroot_part), "--sysroot %s",
                         opt->sysroot_flag);
            if (cc__compile_c_to_obj(opt, opt->c_out_path, opt->obj_out_path,
                                     NULL, src_dir,
                                     target_part[0] ? target_part : NULL,
                                     sysroot_part[0] ? sysroot_part : NULL) != 0)
                return -1;
            if (summary_out) summary_out->did_compile_obj = 1;
            return 0;
        }
        fprintf(stderr,
                "cc: native front supports --link, --emit-c-only, and "
                "--compile (unknown mode)\n");
        return -1;
        }
    }
    if (summary_out) {
        memset(summary_out, 0, sizeof(*summary_out));
        summary_out->c_out_path = opt->c_out_path;
        summary_out->obj_out_path = opt->obj_out_path;
        summary_out->bin_out_path = opt->bin_out_path;
    }
    enum { max_bindings = 128 };
    CCConstBinding bindings[max_bindings];
    size_t count = 0;
    int load_err = cc__load_const_bindings(opt, bindings, &count);
    if (load_err != 0) return load_err;

    CCCompileConfig cfg = {
        .consts = bindings,
        .const_count = count
    };
    if (opt->dump_consts && !opt->dump_comptime) {
        for (size_t i = 0; i < count; ++i) {
            printf("CONST %s=%lld\n", bindings[i].name, bindings[i].value);
        }
    }
    if (opt->dry_run) {
        return 0;
    }

    int is_raw_c = cc__is_raw_c(opt->in_path);

    // For raw C inputs, we skip CC lowering and treat the input itself as the C source.
    if (is_raw_c) {
        if (summary_out) {
            summary_out->reuse_emit_c = 1;
            summary_out->did_emit_c = 0;
        }
        if (opt->mode == CC_MODE_EMIT_C) {
            /* Host-safe emit: rewrite .cch includes → .h when writing out. */
            if (opt->c_out_path && strcmp(opt->c_out_path, opt->in_path) != 0) {
                if (cc__materialize_host_c(opt->in_path, opt->c_out_path) != 0) {
                    fprintf(stderr, "cc: failed to materialize host C %s -> %s\n",
                            opt->in_path, opt->c_out_path);
                    return -1;
                }
                if (summary_out) {
                    summary_out->reuse_emit_c = 0;
                    summary_out->did_emit_c = 1;
                }
            }
            return 0;
        }
        /* COMPILE/LINK: materialize before host cc (c_out_path ≠ in_path). */
        if (!opt->c_out_path || strcmp(opt->c_out_path, opt->in_path) == 0) {
            fprintf(stderr,
                    "cc: internal error: raw .c compile needs a distinct host "
                    "materialize path (got %s)\n",
                    opt->c_out_path ? opt->c_out_path : "(null)");
            return -1;
        }
        if (cc__materialize_host_c(opt->in_path, opt->c_out_path) != 0) {
            fprintf(stderr, "cc: failed to materialize host C %s -> %s\n",
                    opt->in_path, opt->c_out_path);
            return -1;
        }
        if (summary_out) {
            summary_out->reuse_emit_c = 0;
            summary_out->did_emit_c = 1;
        }
    }

    // Incremental cache: emit C (for .ccs inputs)
    uint64_t emit_key = 0;
    int cache_ok = !cc__cache_disabled(opt->no_cache);
    char stem[128];
    cc__stem_from_path(opt->in_path, stem, sizeof(stem));
    char meta_path[PATH_MAX];
    cc__cache_key_paths(meta_path, sizeof(meta_path), NULL, 0, stem);
    if (!is_raw_c && cache_ok) {
        CCFileSig in_sig, build_sig, ccc_sig;
        (void)cc__stat_sig(opt->in_path, &in_sig);
        char build_buf[512];
        int multiple = 0;
        const char* build_path = opt->build_override ? opt->build_override : choose_build_path(opt->in_path, build_buf, sizeof(build_buf), &multiple);
        if (multiple) build_path = NULL;
        if (build_path && cc__stat_sig(build_path, &build_sig) != 0) { build_sig.mtime_sec = 0; build_sig.size = 0; }
        /* Emit is host-agnostic: key on the lowering toolchain
         * (shadow_lower content + ccc driver), not the host C compiler. */
        if (cc__stat_sig(g_ccc_sig_path[0] ? g_ccc_sig_path : g_ccc_path, &ccc_sig) != 0) { ccc_sig.mtime_sec = 0; ccc_sig.size = 0; }

        uint64_t h = 1469598103934665603ULL;
        h = cc__fnv1a64_str(h, opt->in_path);
        h = cc__fnv1a64_i64(h, in_sig.mtime_sec);
        h = cc__fnv1a64_i64(h, in_sig.size);
        /* Content fold: mtime is second-granular; same-second same-size edits
         * must still invalidate emit + diagnostic sidecar replay. */
        h = cc__fold_file_content(h, opt->in_path);
        h = cc__fnv1a64_str(h, build_path ? build_path : "");
        h = cc__fnv1a64_i64(h, build_sig.mtime_sec);
        h = cc__fnv1a64_i64(h, build_sig.size);
        h = cc__fnv1a64_i64(h, ccc_sig.mtime_sec);
        h = cc__fnv1a64_i64(h, ccc_sig.size);
        h = cc__fnv1a64_str(h, opt->target_flag);
        h = cc__fnv1a64_str(h, opt->sysroot_flag);
        h = cc__fnv1a64_str(h, opt->cc_flags);
        h = cc__fnv1a64_str(h, getenv("CFLAGS"));
        h = cc__fnv1a64_str(h, getenv("CPPFLAGS"));
        h = cc__fnv1a64_str(h, getenv("CC_STRICT_RESULT_UNWRAP"));
        h = cc__fnv1a64_i64(h, (long long)opt->no_build);
        h = cc__fnv1a64_i64(h, (long long)opt->cli_count);
        for (size_t i = 0; i < opt->cli_count; ++i) {
            h = cc__fnv1a64_str(h, opt->cli_names[i]);
            h = cc__fnv1a64_i64(h, opt->cli_values[i]);
        }
        /* Declared comptime build deps (`#pragma cc_depends("...")`): fold each
         * dependency's content so editing a comptime-read file re-triggers emit. */
        h = cc__fold_cc_depends(h, opt->in_path);
        h = cc__fold_cch_includes(h, opt->in_path, opt->cc_flags);
        h = cc__fold_ccc_driver(h);
        h = cc__fold_shadow_lower(h);
        h = cc__fold_toolchain_id(h);
        emit_key = h;

        uint64_t prev = 0;
        if (file_exists(opt->c_out_path) && cc__read_u64_file(meta_path, &prev) == 0 && prev == emit_key) {
            if (summary_out) { summary_out->reuse_emit_c = 1; summary_out->did_emit_c = 0; }
            cc__replay_diag_sidecar(opt->c_out_path);
        } else {
            int err = cc__compile_with_env(opt, opt->in_path, opt->c_out_path, &cfg);
            if (err != 0) return err;
            (void)cc__write_u64_file(meta_path, emit_key);
            if (summary_out) { summary_out->reuse_emit_c = 0; summary_out->did_emit_c = 1; }
        }
    } else if (!is_raw_c) {
        int err = cc__compile_with_env(opt, opt->in_path, opt->c_out_path, &cfg);
        if (err != 0) return err;
        if (summary_out) { summary_out->reuse_emit_c = 0; summary_out->did_emit_c = 1; }
    }

    // Post-process generated .c to rewrite @link directives from headers
    if (!is_raw_c && opt->c_out_path) {
        cc__postprocess_link_directives(opt->c_out_path);
    }

    if (opt->mode == CC_MODE_EMIT_C) {
        return 0;
    }

    if (!opt->obj_out_path) {
        fprintf(stderr, "cc: internal error: missing object output path\n");
        return -1;
    }
    const char* cc_bin = pick_cc_bin(opt->cc_bin_override);
    const char* ccflags_env = getenv("CFLAGS");

    // Compile to object (with incremental cache)
    char target_part[256];
    char sysroot_part[256];
    target_part[0] = '\0';
    sysroot_part[0] = '\0';
    if (opt->target_flag && *opt->target_flag) {
        snprintf(target_part, sizeof(target_part), "--target %s", opt->target_flag);
    }
    if (opt->sysroot_flag && *opt->sysroot_flag) {
        snprintf(sysroot_part, sizeof(sysroot_part), "--sysroot %s", opt->sysroot_flag);
    }
    char obj_meta_path[PATH_MAX];
    snprintf(obj_meta_path, sizeof(obj_meta_path), "%s/%s.obj",
             g_host_obj_root[0] ? g_host_obj_root : g_cache_root, stem);
    uint64_t obj_key = 0;
    char dep_path[PATH_MAX];
    cc__derive_d_path_from_stem(stem, dep_path, sizeof(dep_path));
    char src_dir[PATH_MAX];
    cc__dir_of_path(opt->in_path, src_dir, sizeof(src_dir));
    /* Raw .c was materialized to c_out_path with .cch→.h includes. */
    const char* c_for_compile = opt->c_out_path;
    if (cache_ok) {
        uint64_t h = 1469598103934665603ULL;
        if (is_raw_c) {
            CCFileSig in_sig;
            (void)cc__stat_sig(opt->in_path, &in_sig);
            h = cc__fnv1a64_str(h, opt->in_path);
            h = cc__fnv1a64_i64(h, in_sig.mtime_sec);
            h = cc__fnv1a64_i64(h, in_sig.size);
            h = cc__fold_file_content(h, opt->in_path);
        } else {
            h = cc__fnv1a64_i64(h, (long long)emit_key);
        }
        h = cc__fnv1a64_str(h, target_part);
        h = cc__fnv1a64_str(h, sysroot_part);
        h = cc__fnv1a64_str(h, opt->cc_flags);
        h = cc__fnv1a64_str(h, getenv("CFLAGS"));
        h = cc__fnv1a64_str(h, getenv("CPPFLAGS"));
        h = cc__fnv1a64_str(h, g_host_fp);
        obj_key = h;
        uint64_t prev = 0;
        if (file_exists(opt->obj_out_path) && cc__read_u64_file(obj_meta_path, &prev) == 0 && prev == obj_key &&
            !cc__deps_require_rebuild(dep_path, opt->obj_out_path)) {
            if (summary_out) { summary_out->reuse_compile_obj = 1; summary_out->did_compile_obj = 0; }
        } else {
            if (cc__compile_c_to_obj(opt, c_for_compile, opt->obj_out_path, dep_path, src_dir, target_part, sysroot_part) != 0) return -1;
            (void)cc__write_u64_file(obj_meta_path, obj_key);
            if (summary_out) { summary_out->reuse_compile_obj = 0; summary_out->did_compile_obj = 1; }
        }
    } else {
        if (cc__compile_c_to_obj(opt, c_for_compile, opt->obj_out_path, dep_path, src_dir, target_part, sysroot_part) != 0) return -1;
        if (summary_out) { summary_out->reuse_compile_obj = 0; summary_out->did_compile_obj = 1; }
    }

    if (opt->mode == CC_MODE_COMPILE) {
        return 0;
    }

    if (!opt->bin_out_path) {
        fprintf(stderr, "cc: internal error: missing binary output path\n");
        return -1;
    }
    // Extract @link directives from generated C file (runs preprocessor to expand includes)
    static char extracted_ld[1024];
    extracted_ld[0] = '\0';
    if (opt->ld_flags && opt->ld_flags[0]) {
        strncpy(extracted_ld, opt->ld_flags, sizeof(extracted_ld) - 1);
        extracted_ld[sizeof(extracted_ld) - 1] = '\0';
    }
    char link_inc_flags[512];
    snprintf(link_inc_flags, sizeof(link_inc_flags), "-I%s -I%s -I%s -I%s",
             g_cc_lowered_include, g_cc_include, g_cc_dir, g_repo_root);
    if (src_dir[0]) {
        strncat(link_inc_flags, " -I", sizeof(link_inc_flags) - strlen(link_inc_flags) - 1);
        strncat(link_inc_flags, src_dir, sizeof(link_inc_flags) - strlen(link_inc_flags) - 1);
    }
    cc__extract_link_directives(opt->c_out_path, link_inc_flags, extracted_ld, sizeof(extracted_ld));
#ifndef _WIN32
    /* libm is ISO C's own standard surface — the libc/libm split is a
     * Unix packaging accident, not a dependency boundary. Link it by
     * default so `x.sqrt()` with <math.h> works without @link("m");
     * modern linkers default --as-needed, so an unreferenced -lm leaves
     * no trace. @link remains the idiom for everything outside ISO C. */
    cc__add_lib_to_flags("m", extracted_ld, sizeof(extracted_ld));
#endif
    const char* final_ld_flags = extracted_ld[0] ? extracted_ld : opt->ld_flags;

    // Link to binary (with incremental cache)
    const char* ldflags_env = getenv("LDFLAGS");
    char link_cmd[2048];
    // Optional runtime object (release builds may need a freshly-built runtime with section flags for dead-strip).
    char runtime_obj[PATH_MAX];
    runtime_obj[0] = '\0';
    int have_runtime = 0;
    int runtime_reused = 0;
    if (cc__ensure_runtime_obj(opt, target_part, sysroot_part, runtime_obj, sizeof(runtime_obj), &runtime_reused) != 0) return -1;
    if (!opt->no_runtime && runtime_obj[0]) {
        have_runtime = 1;
        if (summary_out) {
            summary_out->runtime_reused = runtime_reused;
            /* runtime_obj holds whichever object was chosen — the prebuilt, a
             * cache hit, or a fresh compile. */
            summary_out->runtime_obj_path = runtime_obj;
        }
    }

    const char* link_extra = "";
    int link_is_tcc = cc__is_tcc(cc_bin);
    /* Dead-strip is the default: objects are always compiled with
     * -ffunction-sections/-fdata-sections (prebuilt runtime included), so
     * the linker flag is pure win.  Debug builds opt out — stripped
     * sections make for confusing symbolication. */
#if defined(__APPLE__)
    if (opt && !opt->opt_debug && !link_is_tcc) link_extra = "-Wl,-dead_strip";
#elif defined(__linux__)
    if (opt && !opt->opt_debug && !link_is_tcc) link_extra = "-Wl,--gc-sections";
#endif

    char link_meta_path[PATH_MAX];
    cc__cache_key_paths(NULL, 0, link_meta_path, sizeof(link_meta_path), stem);
    uint64_t link_key = 0;
    if (cache_ok) {
        CCFileSig obj_sig, rt_sig, bin_sig;
        (void)cc__stat_sig(opt->obj_out_path, &obj_sig);
        (void)cc__stat_sig(have_runtime ? runtime_obj : "", &rt_sig);
        (void)cc__stat_sig(opt->bin_out_path, &bin_sig);
        uint64_t h = 1469598103934665603ULL;
        h = cc__fnv1a64_i64(h, (long long)obj_key);
        h = cc__fnv1a64_i64(h, obj_sig.mtime_sec);
        h = cc__fnv1a64_i64(h, obj_sig.size);
        h = cc__fnv1a64_i64(h, rt_sig.mtime_sec);
        h = cc__fnv1a64_i64(h, rt_sig.size);
        // Link output can depend on CFLAGS/--cc-flags (notably -g on macOS controls whether
        // debug info is preserved in the linked binary).
        h = cc__fnv1a64_str(h, opt->cc_flags);
        h = cc__fnv1a64_str(h, ccflags_env);
        h = cc__fnv1a64_str(h, ldflags_env);
        h = cc__fnv1a64_str(h, final_ld_flags);
        h = cc__fnv1a64_str(h, link_extra);
        h = cc__fnv1a64_str(h, target_part);
        h = cc__fnv1a64_str(h, sysroot_part);
        h = cc__fnv1a64_str(h, g_host_fp);
        link_key = h;
        uint64_t prev = 0;
        if (file_exists(opt->bin_out_path) && cc__read_u64_file(link_meta_path, &prev) == 0 && prev == link_key) {
            if (summary_out) { summary_out->reuse_link = 1; summary_out->did_link = 0; }
        } else {
            /* Put -l libs after objects so GNU ld resolves them (macOS ld is laxer). */
            snprintf(link_cmd, sizeof(link_cmd), "%s %s %s %s %s %s %s %s %s %s -o %s",
                     cc_bin,
                     ccflags_env ? ccflags_env : "",
                     opt->cc_flags ? opt->cc_flags : "",
                     target_part,
                     sysroot_part,
                     link_extra,
                     opt->obj_out_path,
                     have_runtime ? runtime_obj : "",
                     ldflags_env ? ldflags_env : "",
                     final_ld_flags ? final_ld_flags : "",
                     opt->bin_out_path);
            if (link_is_tcc) cc__append_tcc_host_flags(link_cmd, sizeof(link_cmd), cc_bin);
            if (run_cmd(link_cmd, opt->verbose) != 0) return -1;

#if defined(__APPLE__)
            // On macOS, DWARF debug info typically lives in a separate dSYM bundle.
            // Generating it here makes LLDB breakpoints reliable (VS Code / Cursor).
            {
                const char* cflags_for_debug = (opt->cc_flags && *opt->cc_flags) ? opt->cc_flags : (ccflags_env ? ccflags_env : "");
                if (cflags_for_debug && strstr(cflags_for_debug, "-g") && !strstr(cflags_for_debug, "-g0")) {
                    char dsym_cmd[PATH_MAX * 2];
                    char dsym_out[PATH_MAX];
                    snprintf(dsym_out, sizeof(dsym_out), "%s.dSYM", opt->bin_out_path);
                    snprintf(dsym_cmd, sizeof(dsym_cmd), "dsymutil %s -o %s", opt->bin_out_path, dsym_out);
                    (void)run_cmd(dsym_cmd, opt->verbose);
                }
            }
#endif
            (void)cc__write_u64_file(link_meta_path, link_key);
            if (summary_out) { summary_out->reuse_link = 0; summary_out->did_link = 1; }
        }
    } else {
        snprintf(link_cmd, sizeof(link_cmd), "%s %s %s %s %s %s %s %s %s %s -o %s",
                 cc_bin,
                 ccflags_env ? ccflags_env : "",
                 opt->cc_flags ? opt->cc_flags : "",
                 target_part,
                 sysroot_part,
                 link_extra,
                 opt->obj_out_path,
                 have_runtime ? runtime_obj : "",
                 ldflags_env ? ldflags_env : "",
                 final_ld_flags ? final_ld_flags : "",
                 opt->bin_out_path);
        if (link_is_tcc) cc__append_tcc_host_flags(link_cmd, sizeof(link_cmd), cc_bin);
        if (run_cmd(link_cmd, opt->verbose) != 0) return -1;

#if defined(__APPLE__)
        {
            const char* cflags_for_debug = (opt->cc_flags && *opt->cc_flags) ? opt->cc_flags : (ccflags_env ? ccflags_env : "");
            if (cflags_for_debug && strstr(cflags_for_debug, "-g") && !strstr(cflags_for_debug, "-g0")) {
                char dsym_cmd[PATH_MAX * 2];
                char dsym_out[PATH_MAX];
                snprintf(dsym_out, sizeof(dsym_out), "%s.dSYM", opt->bin_out_path);
                snprintf(dsym_cmd, sizeof(dsym_cmd), "dsymutil %s -o %s", opt->bin_out_path, dsym_out);
                (void)run_cmd(dsym_cmd, opt->verbose);
            }
        }
#endif
        if (summary_out) { summary_out->reuse_link = 0; summary_out->did_link = 1; }
    }

    if (!opt->keep_c && opt->mode != CC_MODE_EMIT_C) {
        // Leave C file in out/ by default; optional cleanup.
        (void)opt;
    }
    return 0;
}

static int cc__load_const_bindings(const CCBuildOptions* opt, CCConstBinding* bindings, size_t* count) {
    if (!opt || !bindings || !count) return -1;
    *count = 0;
    char build_buf[512];
    int multiple = 0;
    const char* build_path = NULL;
    if (!opt->no_build) {
        build_path = opt->build_override ? opt->build_override : choose_build_path(opt->in_path, build_buf, sizeof(build_buf), &multiple);
        if (multiple) {
            fprintf(stderr, "cc: multiple build.cc files found (cwd and alongside input)\n");
            return -1;
        }
    }
    if (build_path) {
        CCBuildTarget target;
        detect_host_target(&target);
        CCBuildInputs inputs = {.target = &target, .envp = NULL};
        int err = cc_build_load_consts(build_path, &inputs, bindings, count);
        if (err != 0) {
            fprintf(stderr, "cc: build.cc load failed (err=%d)\n", err);
            return err;
        }
    }
    // Apply CLI -D defines (override build.cc) with warning on override.
    for (size_t i = 0; i < opt->cli_count; ++i) {
        const char* name = opt->cli_names[i];
        long long value = opt->cli_values[i];
        int existed = 0;
        for (size_t j = 0; j < *count; ++j) {
            if (strcmp(bindings[j].name, name) == 0) { existed = 1; break; }
        }
        if (upsert_binding(bindings, count, 128, name, value) != 0) {
            fprintf(stderr, "cc: too many const bindings (max %d)\n", 128);
            return -1;
        }
        if (existed) {
            fprintf(stderr, "cc: warning: overriding const %s from build.cc with CLI -D\n", name);
        }
    }
    if (opt->dump_comptime) {
        cc__print_comptime_state(opt, build_path, bindings, *count);
    }
    return 0;
}

static void cc__print_comptime_targets(const char* build_path) {
    if (!build_path) return;
    CCBuildTargetDecl targets[64];
    size_t target_count = 0;
    char* def_name = NULL;
    if (cc_build_list_targets(build_path, targets, &target_count, 64, &def_name) != 0) {
        return;
    }
    printf("COMPTIME targets (%zu):\n", target_count);
    if (def_name) {
        printf("  default: %s\n", def_name);
    }
    for (size_t i = 0; i < target_count; ++i) {
        const char* kind = targets[i].kind == CC_BUILD_TARGET_OBJ ? "obj" : "exe";
        printf("  target=%s kind=%s src=", targets[i].name, kind);
        for (size_t j = 0; j < targets[i].src_count; ++j) {
            if (j) printf(",");
            printf("%s", targets[i].srcs[j]);
        }
        printf(" deps=");
        for (size_t j = 0; j < targets[i].dep_count; ++j) {
            if (j) printf(",");
            printf("%s", targets[i].deps[j]);
        }
        printf("\n");
    }
    cc_build_free_targets(targets, target_count, def_name);
}

static void cc__print_comptime_state(const CCBuildOptions* opt, const char* build_path,
                                    const CCConstBinding* bindings, size_t count) {
    if (!opt || !opt->dump_comptime) return;
    if (build_path && build_path[0]) {
        printf("COMPTIME build_file=%s\n", build_path);
    } else {
        printf("COMPTIME build_file=(none)\n");
    }
    printf("COMPTIME consts (%zu):\n", count);
    for (size_t i = 0; i < count; ++i) {
        printf("  %s=%lld\n", bindings[i].name, bindings[i].value);
    }
    if (build_path) {
        cc__print_comptime_targets(build_path);
    }
}

static int cc__compile_c_to_obj(const CCBuildOptions* opt,
                                const char* c_path,
                                const char* obj_path,
                                const char* dep_path,
                                const char* extra_include_dir,
                                const char* target_part,
                                const char* sysroot_part) {
    const char* cc_bin = pick_cc_bin(opt->cc_bin_override);
    const char* ccflags_env = getenv("CFLAGS");
    const char* cppflags_env = getenv("CPPFLAGS");
    int is_tcc = cc__is_tcc(cc_bin);
    char cmd[2048];

    // TCC doesn't support -MMD/-MF/-MT dependency tracking flags
    // Add lowered include path first so .h versions of .cch are found before originals
    if (is_tcc) {
        snprintf(cmd, sizeof(cmd), "%s %s %s %s %s -I%s -I%s -I%s -I%s",
                 cc_bin,
                 ccflags_env ? ccflags_env : "",
                 cppflags_env ? cppflags_env : "",
                 target_part ? target_part : "",
                 sysroot_part ? sysroot_part : "",
                 g_cc_lowered_include,
                 g_cc_include,
                 g_cc_dir,
                 g_repo_root);
        cc__append_tcc_host_flags(cmd, sizeof(cmd), cc_bin);
    } else {
        snprintf(cmd, sizeof(cmd), "%s %s %s %s %s -MMD -MF %s -MT %s -I%s -I%s -I%s -I%s",
                 cc_bin,
                 ccflags_env ? ccflags_env : "",
                 cppflags_env ? cppflags_env : "",
                 target_part ? target_part : "",
                 sysroot_part ? sysroot_part : "",
                 dep_path ? dep_path : "/dev/null",
                 obj_path ? obj_path : "out.o",
                 g_cc_lowered_include,
                 g_cc_include,
                 g_cc_dir,
                 g_repo_root);
    }
    if (extra_include_dir && *extra_include_dir) {
        // Add -I<dir> so generated C can include headers relative to the original source directory.
        char inc[PATH_MAX + 8];
        snprintf(inc, sizeof(inc), " -I%s", extra_include_dir);
        strncat(cmd, inc, sizeof(cmd) - strlen(cmd) - 1);
    }
    {
        /* Extra system includes from --sysroot / -isysroot / CC_SYSROOT.
         * Not `--sysroot` itself — that would hide the host libc. */
        const char* sr = getenv("CC_SYSROOT");
        char abs[PATH_MAX];
        char probe[PATH_MAX];
        char inc[PATH_MAX + 8];
        const char* use;
        if (sr && sr[0]) {
            use = realpath(sr, abs) ? abs : sr;
            if ((size_t)snprintf(probe, sizeof(probe), "%s/usr/include", use) <
                    sizeof(probe) &&
                access(probe, R_OK) == 0 &&
                (size_t)snprintf(inc, sizeof(inc), " -I%s", probe) < sizeof(inc))
                strncat(cmd, inc, sizeof(cmd) - strlen(cmd) - 1);
        }
    }
    if (opt->cc_flags && *opt->cc_flags) {
        strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, opt->cc_flags, sizeof(cmd) - strlen(cmd) - 1);
    }
    // For smaller release binaries, ensure the compiler emits per-function/data sections.
    // This enables the linker to dead-strip unused runtime code.
    // TCC doesn't support these flags.
    if (!is_tcc) {
        strncat(cmd, " -ffunction-sections -fdata-sections", sizeof(cmd) - strlen(cmd) - 1);
        /* C23 semantics: an undeclared function is a compile error AT THE
         * USER'S LINE (via #line sourcemaps), not an implicit int that
         * survives to an opaque linker error naming the lowered .c file.
         * gcc>=14 / clang>=16 already default to this; pinning the flag
         * makes the diagnostic identical on older hosts. */
        strncat(cmd, " -Werror=implicit-function-declaration", sizeof(cmd) - strlen(cmd) - 1);
    }
    // Finally append the compilation inputs/outputs.
    if (c_path && *c_path) {
        strncat(cmd, " -c ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, c_path, sizeof(cmd) - strlen(cmd) - 1);
    }
    if (obj_path && *obj_path) {
        strncat(cmd, " -o ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, obj_path, sizeof(cmd) - strlen(cmd) - 1);
    }
    {
        long long t_cc = cc__now_ms();
        if (run_cmd(cmd, opt->verbose) != 0) return -1;
        cc__prof_span_arg("host_cc", c_path ? c_path : obj_path, t_cc);
    }
    return 0;
}

/* The command that produces a runtime object (single unity TU). */
typedef struct {
    char compile[4096];
} CCRuntimeCmds;

static uint64_t cc__fnv1a64(const void* data, size_t n, uint64_t h) {
    const unsigned char* p = (const unsigned char*)data;
    size_t i;
    if (!h) h = 14695981039346656037ull;
    for (i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* Newest mtime among regular files in `dir`, recursing `depth` levels. */
static time_t cc__newest_mtime(const char* dir, int depth) {
    if (!dir || !dir[0]) return 0;
    DIR* d = opendir(dir);
    if (!d) return 0;
    time_t newest = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[PATH_MAX];
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >= sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0) {
                time_t sub = cc__newest_mtime(path, depth - 1);
                if (sub > newest) newest = sub;
            }
        } else if (st.st_mtime > newest) {
            newest = st.st_mtime;
        }
    }
    closedir(d);
    return newest;
}

/* Newest mtime across everything a runtime object is built from: the runtime
 * sources (including vendor/), the headers they include, and ccc itself (a new
 * compiler can lower those headers differently). */
static time_t cc__runtime_inputs_mtime(void) {
    time_t newest = 0;
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", g_cc_runtime_c);
    char* slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        time_t t = cc__newest_mtime(dir, 1);
        if (t > newest) newest = t;
    }
    if (g_cc_lowered_include[0]) {
        time_t t = cc__newest_mtime(g_cc_lowered_include, 2);
        if (t > newest) newest = t;
    }
    if (g_cc_include[0] &&
        (!g_cc_lowered_include[0] || strcmp(g_cc_include, g_cc_lowered_include) != 0)) {
        time_t t = cc__newest_mtime(g_cc_include, 2);
        if (t > newest) newest = t;
    }
    struct stat st;
    if (g_ccc_sig_path[0] && stat(g_ccc_sig_path, &st) == 0 && st.st_mtime > newest)
        newest = st.st_mtime;
    return newest;
}

/* Whether `path` contains exactly `text`. */
static int cc__file_text_equals(const char* path, const char* text) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    size_t want = strlen(text);
    int same = 0;
    char* buf = malloc(want + 2);
    if (buf) {
        size_t got = fread(buf, 1, want + 1, f);
        same = (got == want && memcmp(buf, text, want) == 0);
        free(buf);
    }
    fclose(f);
    return same;
}

static void cc__write_file_text(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

/* The flavor the driver injects when neither -O nor -g is given.  Shared
 * with cc__prebuilt_runtime_applies: a cc_flags string equal to exactly
 * this is the driver talking, not the caller. */
#define CC_DEFAULT_FLAVOR_CC "-O2"

/* Whether the prebuilt runtime object can answer this request.
 *
 * out/cc/obj/runtime/concurrent_c.o is whatever `make -C cc` compiled: the
 * default host cc, the Makefile's own flags, a native target. Reusing it for an
 * invocation that supplies compilation inputs of its own would silently drop
 * exactly the flags the caller asked for — the artifact would not be the one
 * they requested. Any such invocation falls through to the per-variant cache,
 * which builds one object per flag set and keeps it.  The driver-injected
 * default flavor is not a caller flag: an unflagged build carries exactly
 * CC_DEFAULT_FLAVOR_CC in cc_flags and still means "the stock runtime". */
static int cc__prebuilt_runtime_applies(const CCBuildOptions* opt,
                                        const char* target_part,
                                        const char* sysroot_part) {
    const char* e;
    if (opt->cc_flags && *opt->cc_flags &&
        strcmp(opt->cc_flags, CC_DEFAULT_FLAVOR_CC) != 0) return 0;
    if (target_part && *target_part) return 0;
    if (sysroot_part && *sysroot_part) return 0;
    /* A host CC override may not be the compiler that built the prebuilt, and
     * nothing records which one did. */
    if (opt->cc_bin_override && *opt->cc_bin_override) return 0;
    if ((e = getenv("CC")) != NULL && *e) return 0;
    if ((e = getenv("CFLAGS")) != NULL && *e) return 0;
    if ((e = getenv("CPPFLAGS")) != NULL && *e) return 0;
    return 1;
}

// Check if prebuilt runtime object is stale (runtime sources newer than object).
static int cc__runtime_obj_is_stale(const char* runtime_obj_path) {
    if (!runtime_obj_path || !file_exists(runtime_obj_path)) return 1;
    struct stat obj_stat;
    if (stat(runtime_obj_path, &obj_stat) != 0) return 1;
    time_t obj_mtime = obj_stat.st_mtime;
    
    // Check key runtime source files
    const char* runtime_sources[] = {
        "concurrent_c.c", "scheduler.c", "fiber_sched.c", "nursery.c",
        "channel.c", "fiber.c", "exec.c", "closure.c", "task_intptr.c",
        "float_format_zmij.c", "slice_gen.c", "cc_mem_heap.c",
        NULL
    };
    char src_path[PATH_MAX];
    for (int i = 0; runtime_sources[i]; ++i) {
        snprintf(src_path, sizeof(src_path), "%s/cc/runtime/%s", g_repo_root, runtime_sources[i]);
        struct stat src_stat;
        if (stat(src_path, &src_stat) == 0 && src_stat.st_mtime > obj_mtime) {
            return 1;  // Source is newer than object
        }
    }
    snprintf(src_path, sizeof(src_path), "%s/cc/runtime/vendor/zmij.c", g_repo_root);
    {
        struct stat src_stat;
        if (stat(src_path, &src_stat) == 0 && src_stat.st_mtime > obj_mtime) {
            return 1;
        }
    }
    return 0;  // Object is up-to-date
}

/* Compose the runtime build commands for one set of output paths. Single source
 * of truth: cc__ensure_runtime_obj calls this twice — once against placeholder
 * paths to derive the variant hash, once against the real paths to run and to
 * record — so the hash cannot miss a dimension the commands encode. */
static void cc__runtime_build_cmds(const CCBuildOptions* opt, const char* cc_bin, int is_tcc,
                                   const CCHostCcProfile* host_prof,
                                   const char* target_part, const char* sysroot_part,
                                   const char* obj, CCRuntimeCmds* out) {
    const char* ccflags_env = getenv("CFLAGS");
    const char* cppflags_env = getenv("CPPFLAGS");
    out->compile[0] = '\0';

    snprintf(out->compile, sizeof(out->compile), "%s %s %s %s %s -DCC_ENABLE_ASYNC -I%s -I%s -I%s -I%s -I%s/runtime -c %s -o %s",
             cc_bin,
             ccflags_env ? ccflags_env : "",
             cppflags_env ? cppflags_env : "",
             target_part ? target_part : "",
             sysroot_part ? sysroot_part : "",
             g_cc_lowered_include,
             g_cc_include,
             g_cc_dir,
             g_repo_root,
             g_cc_dir,
             g_cc_runtime_c,
             obj);
    if (host_prof->ok ? host_prof->no_liblfds : is_tcc) {
        strncat(out->compile, " -DCC_NO_LIBLFDS", sizeof(out->compile) - strlen(out->compile) - 1);
    }
    cc__append_host_cc_flags(out->compile, sizeof(out->compile), cc_bin);
    if (opt->cc_flags && *opt->cc_flags) {
        strncat(out->compile, " ", sizeof(out->compile) - strlen(out->compile) - 1);
        strncat(out->compile, opt->cc_flags, sizeof(out->compile) - strlen(out->compile) - 1);
    }
    if (!is_tcc)
        strncat(out->compile, " -ffunction-sections -fdata-sections",
                sizeof(out->compile) - strlen(out->compile) - 1);
}

static int cc__ensure_runtime_obj(const CCBuildOptions* opt,
                                 const char* target_part,
                                 const char* sysroot_part,
                                 char* out_runtime_path,
                                 size_t out_runtime_cap,
                                 int* out_reused) {
    if (!out_runtime_path || out_runtime_cap == 0) return -1;
    out_runtime_path[0] = '\0';
    if (out_reused) *out_reused = 0;
    if (opt->no_runtime) return 0;

    const char* cc_bin = pick_cc_bin(opt->cc_bin_override);
    CCHostCcProfile host_prof;
    int is_tcc;
    (void)cc__host_profile_for(cc_bin, &host_prof);
    is_tcc = host_prof.ok ? host_prof.is_tcc : cc__is_tcc(cc_bin);

    // NOTE: For release builds, don't reuse the prebuilt monolithic runtime object.
    // It may not be compiled with -ffunction-sections/-fdata-sections, which prevents
    // the linker from dead-stripping unused runtime code.
    // NOTE: For TCC, don't reuse prebuilt - TCC can't link Mach-O/ELF from other compilers.
    // NOTE: Only for invocations the prebuilt actually answers - see
    // cc__prebuilt_runtime_applies; a caller's own flags must not be dropped.
    // NOTE: Check if prebuilt is stale (runtime sources changed but make not run).
    int prebuilt_applies = cc__prebuilt_runtime_applies(opt, target_part, sysroot_part);
    int prebuilt_from_host_tcc = is_tcc && strstr(g_cc_runtime_o, "/cc-tcc/") != NULL;
    if (!opt->opt_release && (!is_tcc || prebuilt_from_host_tcc) && prebuilt_applies &&
        file_exists(g_cc_runtime_o) && !cc__runtime_obj_is_stale(g_cc_runtime_o)) {
        strncpy(out_runtime_path, g_cc_runtime_o, out_runtime_cap);
        out_runtime_path[out_runtime_cap - 1] = '\0';
        if (out_reused) *out_reused = 1;
        return 0;
    }

    // ABORT if prebuilt exists but is stale - force explicit rebuild.
    // Skip for TCC: host TCC never reuses the clang/gcc prebuilt (object format
    // mismatch) and always rebuilds out/<dir>/runtime.o from sources.
    // Skip when the prebuilt does not apply: that build compiles its own runtime
    // from current sources, so the prebuilt's staleness cannot affect it.
    if (!is_tcc && prebuilt_applies &&
        file_exists(g_cc_runtime_o) && cc__runtime_obj_is_stale(g_cc_runtime_o)) {
        fprintf(stderr, "\n");
        fprintf(stderr, "================================================================================\n");
        fprintf(stderr, "STALE RUNTIME DETECTED\n");
        fprintf(stderr, "================================================================================\n");
        fprintf(stderr, "Runtime source files are newer than the prebuilt runtime object.\n");
        fprintf(stderr, "This can cause hard-to-debug issues with stale code.\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "To fix, run:  make -C cc\n");
        fprintf(stderr, "================================================================================\n");
        fprintf(stderr, "\n");
        return -1;
    }

    // Host-native runtime objects under <cache>/host/<fp>/.
    char runtime_obj[PATH_MAX];
    const char* rt_root = g_host_obj_root[0] ? g_host_obj_root : g_out_root;

    CCRuntimeCmds probe;
    char ph_obj[PATH_MAX];
    snprintf(ph_obj, sizeof(ph_obj), "%s/runtime.o", rt_root);
    cc__runtime_build_cmds(opt, cc_bin, is_tcc, &host_prof, target_part, sysroot_part,
                           ph_obj, &probe);
    uint64_t vh = cc__fnv1a64(probe.compile, strlen(probe.compile), 0);
    char variant[17];
    snprintf(variant, sizeof(variant), "%016llx", (unsigned long long)vh);

    snprintf(runtime_obj, sizeof(runtime_obj), "%s/runtime-%s.o", rt_root, variant);

    CCRuntimeCmds cmds;
    cc__runtime_build_cmds(opt, cc_bin, is_tcc, &host_prof, target_part, sysroot_part,
                           runtime_obj, &cmds);

    char recipe_path[PATH_MAX];
    char recipe[sizeof(cmds.compile) + 2];
    snprintf(recipe_path, sizeof(recipe_path), "%s.recipe", runtime_obj);
    snprintf(recipe, sizeof(recipe), "%s\n", cmds.compile);
    if (file_exists(runtime_obj) && cc__file_text_equals(recipe_path, recipe)) {
        struct stat obj_st;
        if (stat(runtime_obj, &obj_st) == 0 && obj_st.st_mtime >= cc__runtime_inputs_mtime()) {
            strncpy(out_runtime_path, runtime_obj, out_runtime_cap);
            out_runtime_path[out_runtime_cap - 1] = '\0';
            if (out_reused) *out_reused = 1;
            return 0;
        }
    }

    {
        char lock_path[PATH_MAX];
        int lock_fd = -1;
        snprintf(lock_path, sizeof(lock_path), "%s.lock", runtime_obj);
        lock_fd = open(lock_path, O_CREAT | O_RDWR, 0644);
        if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
        if (file_exists(runtime_obj) && cc__file_text_equals(recipe_path, recipe)) {
            struct stat obj_st;
            if (stat(runtime_obj, &obj_st) == 0 &&
                obj_st.st_mtime >= cc__runtime_inputs_mtime()) {
                strncpy(out_runtime_path, runtime_obj, out_runtime_cap);
                out_runtime_path[out_runtime_cap - 1] = '\0';
                if (out_reused) *out_reused = 1;
                if (lock_fd >= 0) { flock(lock_fd, LOCK_UN); close(lock_fd); }
                return 0;
            }
        }
        {
            long long t_rt = cc__now_ms();
            if (run_cmd(cmds.compile, opt->verbose) != 0) {
                if (lock_fd >= 0) { flock(lock_fd, LOCK_UN); close(lock_fd); }
                return -1;
            }
            cc__prof_span("runtime_cc", t_rt);
        }
        cc__write_file_text(recipe_path, recipe);
        if (lock_fd >= 0) { flock(lock_fd, LOCK_UN); close(lock_fd); }
    }
    strncpy(out_runtime_path, runtime_obj, out_runtime_cap);
    out_runtime_path[out_runtime_cap - 1] = '\0';
    if (out_reused) *out_reused = 0;
    return 0;
}

static int cc__link_many(const CCBuildOptions* opt,
                         const char* const* obj_paths,
                         size_t obj_count,
                         const char* runtime_obj,
                         const char* target_part,
                         const char* sysroot_part,
                         const char* bin_out_path) {
    const char* cc_bin = pick_cc_bin(opt->cc_bin_override);
    const char* ldflags_env = getenv("LDFLAGS");
    int is_tcc = cc__is_tcc(cc_bin);
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "%s %s %s %s %s",
             cc_bin,
             target_part ? target_part : "",
             sysroot_part ? sysroot_part : "",
             ldflags_env ? ldflags_env : "",
             opt->ld_flags ? opt->ld_flags : "");
    if (is_tcc) cc__append_tcc_host_flags(cmd, sizeof(cmd), cc_bin);
    // TCC doesn't support -Wl,-dead_strip or -Wl,--gc-sections
    if (!is_tcc) {
#if defined(__APPLE__)
        strncat(cmd, " -Wl,-dead_strip", sizeof(cmd) - strlen(cmd) - 1);
#elif defined(__linux__)
        strncat(cmd, " -Wl,--gc-sections", sizeof(cmd) - strlen(cmd) - 1);
#endif
    }
    for (size_t i = 0; i < obj_count; ++i) {
        strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, obj_paths[i], sizeof(cmd) - strlen(cmd) - 1);
    }
    if (runtime_obj && runtime_obj[0]) {
        strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, runtime_obj, sizeof(cmd) - strlen(cmd) - 1);
    }
    strncat(cmd, " -o ", sizeof(cmd) - strlen(cmd) - 1);
    strncat(cmd, bin_out_path, sizeof(cmd) - strlen(cmd) - 1);
    if (run_cmd(cmd, opt->verbose) != 0) return -1;
    return 0;
}

static void print_build_summary(const CCBuildOptions* opt, const CCBuildSummary* s, const char* step_name) {
    if (!opt || !opt->summary || !s) return;
    fprintf(stderr, "cc build summary:\n");
    if (step_name) fprintf(stderr, "  step: %s\n", step_name);
    if (s->c_out_path) fprintf(stderr, "  c: %s (%s)\n", s->c_out_path, s->reuse_emit_c ? "reused" : "built");
    if (s->obj_out_path) fprintf(stderr, "  obj: %s (%s)\n", s->obj_out_path, s->reuse_compile_obj ? "reused" : "built");
    if (s->runtime_obj_path) {
        fprintf(stderr, "  runtime: %s (%s)\n", s->runtime_obj_path, s->runtime_reused ? "reused" : "compiled");
    } else {
        fprintf(stderr, "  runtime: (none)\n");
    }
    if (s->bin_out_path) fprintf(stderr, "  bin: %s (%s)\n", s->bin_out_path, s->reuse_link ? "reused" : "built");
}

static int ensure_cc_test_tool(const char* cc_bin, const char* target_part, const char* sysroot_part, const char* cc_flags, int verbose) {
    // We build tools/cc_test from source if missing (no make required).
    char tool_path[PATH_MAX];
    char tool_src[PATH_MAX];
    snprintf(tool_path, sizeof(tool_path), "%s/tools/cc_test", g_repo_root);
    snprintf(tool_src, sizeof(tool_src), "%s/tools/cc_test.c", g_repo_root);
    if (file_exists(tool_path)) return 0;
    if (!file_exists(tool_src)) {
        fprintf(stderr, "cc: missing test tool source: %s\n", tool_src);
        return -1;
    }
    // Ensure tools/ dir exists (best effort).
    char mk_cmd[PATH_MAX + 64];
    snprintf(mk_cmd, sizeof(mk_cmd), "mkdir -p %s/tools", g_repo_root);
    if (run_cmd(mk_cmd, verbose) != 0) return -1;

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s %s %s -O2 -Wall -Wextra %s -o %s",
             cc_bin,
             target_part ? target_part : "",
             sysroot_part ? sysroot_part : "",
             tool_src,
             tool_path);
    if (cc_flags && *cc_flags) {
        strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, cc_flags, sizeof(cmd) - strlen(cmd) - 1);
    }
    if (run_cmd(cmd, verbose) != 0) return -1;
    return 0;
}

static int run_build_mode(int argc, char** argv) {
    long long t_build = cc__now_ms();
    // cc build [step] [options] <input.ccs> [output] [-- args...]
    enum { max_cli = 32 };
    char* cli_names[max_cli];
    long long cli_values[max_cli];
    size_t cli_count = 0;

    enum { max_pos = 64 };
    const char* pos_args[max_pos];
    int pos_count = 0;
    const char* user_out = NULL; // from -o (preferred)
    int saw_o = 0;
    const char* obj_out = NULL;
    const char* build_override = NULL;
    const char* cc_bin = NULL;
    const char* module_narrow = NULL; // --module=<tag> dual-target narrowing
    const char* cc_flags = NULL;
    const char* ld_flags = NULL;
    const char* target_flag = NULL;
    const char* sysroot_flag = NULL;
    const char* out_dir = NULL;
    const char* bin_dir = NULL;
    const char* graph_out = NULL;
    int opt_release = 0;
    int opt_debug = 0;
    int help = 0;
    int dump_consts = 0;
    int dump_comptime = 0;
    int dry_run = 0;
    int no_build = 0;
    int no_runtime = 0;
    int keep_c = 1; // default keep C
    int verbose = 0;
    int summary = 0;
    CCMode mode = CC_MODE_LINK;
    int no_cache = 0;
    CCUnitKind unit_kind = CC_UNIT_KIND_UNKNOWN;
    char version_pin[CC_CCC_VERSION_PIN_CAP];
    version_pin[0] = '\0';

    enum {
        CC_BUILD_STEP_DEFAULT = 0,
        CC_BUILD_STEP_RUN = 1,
        CC_BUILD_STEP_TEST = 2,
        CC_BUILD_STEP_LIST = 3,
        CC_BUILD_STEP_GRAPH = 4,
        CC_BUILD_STEP_INSTALL = 5,
        CC_BUILD_STEP_EXPORT_MAKE = 6,
    } step = CC_BUILD_STEP_DEFAULT;
    int run_argc = 0;
    char** run_argv = NULL;
    int run_timeout = -1;

    int argi = 2;
    if (argc >= 3 && argv[2] && argv[2][0] && argv[2][0] != '-') {
        if (strcmp(argv[2], "run") == 0) {
            step = CC_BUILD_STEP_RUN;
            argi = 3;
        } else if (strcmp(argv[2], "test") == 0) {
            step = CC_BUILD_STEP_TEST;
            argi = 3;
        } else if (strcmp(argv[2], "list") == 0) {
            step = CC_BUILD_STEP_LIST;
            argi = 3;
        } else if (strcmp(argv[2], "graph") == 0) {
            step = CC_BUILD_STEP_GRAPH;
            argi = 3;
        } else if (strcmp(argv[2], "install") == 0) {
            step = CC_BUILD_STEP_INSTALL;
            argi = 3;
        } else if (strcmp(argv[2], "export-make") == 0) {
            step = CC_BUILD_STEP_EXPORT_MAKE;
            argi = 3;
        } else if (strcmp(argv[2], "help") == 0) {
            usage_build(argv[0]);
            return 0;
        }
    }

    for (int i = argi; i < argc; ++i) {
        {
            int vf = cc__take_vendor_flag(argc, argv, &i);
            if (vf < 0) goto parse_fail;
            if (vf > 0) continue;
        }
        if (strcmp(argv[i], "--") == 0) {
            run_argc = argc - (i + 1);
            run_argv = &argv[i + 1];
            break;
        }
        {
            int tf = cc__take_unit_flag(argc, argv, &i, &unit_kind, version_pin,
                                        sizeof(version_pin));
            if (tf < 0) goto parse_fail;
            if (tf > 0) continue;
        }
        // Allow placing the step name after options (e.g. `cc build --no-cache run ...`).
        if (argv[i] && argv[i][0] && argv[i][0] != '-' && step == CC_BUILD_STEP_DEFAULT && pos_count == 0) {
            if (strcmp(argv[i], "run") == 0) { step = CC_BUILD_STEP_RUN; continue; }
            if (strcmp(argv[i], "test") == 0) { step = CC_BUILD_STEP_TEST; continue; }
            if (strcmp(argv[i], "list") == 0) { step = CC_BUILD_STEP_LIST; continue; }
            if (strcmp(argv[i], "graph") == 0) { step = CC_BUILD_STEP_GRAPH; continue; }
            if (strcmp(argv[i], "install") == 0) { step = CC_BUILD_STEP_INSTALL; continue; }
            if (strcmp(argv[i], "export-make") == 0) { step = CC_BUILD_STEP_EXPORT_MAKE; continue; }
            if (strcmp(argv[i], "help") == 0) { help = 1; continue; }
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            help = 1;
            continue;
        }
        if (strcmp(argv[i], "--release") == 0 || strcmp(argv[i], "-O") == 0) { opt_release = 1; continue; }
        if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-g") == 0) { opt_debug = 1; continue; }
        if (strcmp(argv[i], "--summary") == 0) { summary = 1; continue; }
        if (strcmp(argv[i], "--no-cache") == 0) { no_cache = 1; continue; }
        if (strcmp(argv[i], "--frontend") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "cc: --frontend requires native\n");
                goto parse_fail;
            }
            ++i;
            if (cc__set_frontend_name(argv[i]) != 0) goto parse_fail;
            continue;
        }
        if (strncmp(argv[i], "--frontend=", 11) == 0) {
            const char* v = argv[i] + 11;
            if (cc__set_frontend_name(v) != 0) goto parse_fail;
            continue;
        }
        if (strcmp(argv[i], "--out-dir") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --out-dir requires a path\n"); goto parse_fail; }
            out_dir = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--bin-dir") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --bin-dir requires a path\n"); goto parse_fail; }
            bin_dir = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--graph-out") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --graph-out requires a path\n"); goto parse_fail; }
            graph_out = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--emit-c-only") == 0) { mode = CC_MODE_EMIT_C; continue; }
        if (strcmp(argv[i], "--emit-c-inspect") == 0) { g_emit_c_inspect = 1; continue; }
        if (strncmp(argv[i], "--emit-c-inspect=", 17) == 0) { g_emit_c_inspect = 1; g_emit_c_inspect_path = argv[i] + 17; continue; }
        if (strcmp(argv[i], "--compile") == 0) { mode = CC_MODE_COMPILE; continue; }
        if (strcmp(argv[i], "--link") == 0) { mode = CC_MODE_LINK; continue; }
        if (strcmp(argv[i], "-D") == 0) {
            fprintf(stderr, "cc: -D requires NAME or NAME=VALUE\n");
            goto parse_fail;
        }
        if (strncmp(argv[i], "-D", 2) == 0) {
            if (cli_count >= (size_t)max_cli) {
                fprintf(stderr, "cc: too many -D defines (max %d)\n", max_cli);
                goto parse_fail;
            }
            if (parse_define(argv[i] + 2, &cli_names[cli_count], &cli_values[cli_count]) != 0) {
                goto parse_fail;
            }
            cli_count++;
            continue;
        }
        if (strcmp(argv[i], "--build-file") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --build-file requires a path\n"); goto parse_fail; }
            build_override = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--no-build") == 0) { no_build = 1; continue; }
        if (strcmp(argv[i], "--dump-consts") == 0) { dump_consts = 1; continue; }
        if (strcmp(argv[i], "--dump-comptime") == 0) { dump_comptime = 1; dump_consts = 1; continue; }
        if (strcmp(argv[i], "--dry-run") == 0) { dry_run = 1; continue; }
        if (strcmp(argv[i], "--no-runtime") == 0) { no_runtime = 1; continue; }
        if (strcmp(argv[i], "--keep-c") == 0) { keep_c = 1; continue; }
        if (strcmp(argv[i], "--verbose") == 0) { verbose = 1; continue; }
        if (strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--jobs") == 0) {
            if (i + 1 < argc && argv[i + 1] && argv[i + 1][0] &&
                (argv[i + 1][0] == '-' ? 0 : 1) &&
                (isdigit((unsigned char)argv[i + 1][0]) ||
                 (argv[i + 1][0] == '+' && isdigit((unsigned char)argv[i + 1][1])))) {
                g_build_jobs = atoi(argv[++i]);
            } else {
                g_build_jobs = 0; /* ncpu */
            }
            continue;
        }
        if (strncmp(argv[i], "-j", 2) == 0 && argv[i][2]) {
            g_build_jobs = atoi(argv[i] + 2);
            continue;
        }
        if (strncmp(argv[i], "--jobs=", 7) == 0) {
            g_build_jobs = atoi(argv[i] + 7);
            continue;
        }
        if (strcmp(argv[i], "--cc-bin") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --cc-bin requires a path\n"); goto parse_fail; }
            cc_bin = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--cc-flags") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --cc-flags requires a value\n"); goto parse_fail; }
            cc_flags = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--timeout") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --timeout requires seconds\n"); goto parse_fail; }
            run_timeout = atoi(argv[++i]);
            if (run_timeout < 0) run_timeout = -1;
            continue;
        }
        if (strcmp(argv[i], "--ld-flags") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --ld-flags requires a value\n"); goto parse_fail; }
            ld_flags = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--target") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --target requires a value\n"); goto parse_fail; }
            target_flag = argv[++i];
            continue;
        }
        /* Dual-target module narrowing: build only the named embedding's
         * artifact (tag from the header's export directive: py, js). */
        if (strcmp(argv[i], "--module") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --module requires a target tag (e.g. py, js)\n"); goto parse_fail; }
            module_narrow = argv[++i];
            continue;
        }
        if (strncmp(argv[i], "--module=", 9) == 0) {
            module_narrow = argv[i] + 9;
            continue;
        }
        if (strcmp(argv[i], "--sysroot") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --sysroot requires a path\n"); goto parse_fail; }
            sysroot_flag = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--obj-out") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --obj-out requires a path\n"); goto parse_fail; }
            obj_out = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: -o requires a path\n"); goto parse_fail; }
            user_out = argv[++i];
            saw_o = 1;
            continue;
        }
        // Positional.
        if (pos_count >= max_pos) {
            fprintf(stderr, "cc: too many input files (max %d)\n", max_pos);
            goto parse_fail;
        }
        pos_args[pos_count++] = argv[i];
    }

    if (g_cccportable_cli) {
        fprintf(stderr,
                "cc: --cccportable only applies to --print-cflags / --print-libs "
                "(it does not remap lowerer faces)\n");
        goto parse_fail;
    }

    // If both are provided, debug wins (safe default).
    if (opt_release && opt_debug) opt_release = 0;

    // Inject flavor defaults early (before we fold cc_flags + -D defines).
    // - default: -O2 with asserts kept (use test harness --O0 for fast cold compiles)
    // - release: -O2 -DNDEBUG + dead-strip
    // - debug:   -O0 -g
    const char* flavor_cc = CC_DEFAULT_FLAVOR_CC;
    if (opt_debug) flavor_cc = "-O0 -g";
    else if (opt_release) flavor_cc = "-O2 -DNDEBUG";

    // Build combined cc_flags that includes both --cc-flags and -D defines
    // This ensures defines like -DCC_ENABLE_HTTP=1 are passed to the C compiler
    static char combined_cc_flags[2048];
    combined_cc_flags[0] = '\0';
    if (flavor_cc && flavor_cc[0]) {
        strncat(combined_cc_flags, flavor_cc, sizeof(combined_cc_flags) - 1);
    }
    if (cc_flags && cc_flags[0]) {
        if (combined_cc_flags[0]) strncat(combined_cc_flags, " ", sizeof(combined_cc_flags) - strlen(combined_cc_flags) - 1);
        strncat(combined_cc_flags, cc_flags, sizeof(combined_cc_flags) - 1);
    }
    for (size_t i = 0; i < cli_count; ++i) {
        char def[256];
        if (cli_values[i] == 1) {
            snprintf(def, sizeof(def), " -D%s", cli_names[i]);
        } else {
            snprintf(def, sizeof(def), " -D%s=%lld", cli_names[i], cli_values[i]);
        }
        strncat(combined_cc_flags, def, sizeof(combined_cc_flags) - strlen(combined_cc_flags) - 1);
    }
    cc_flags = combined_cc_flags[0] ? combined_cc_flags : cc_flags;
    cc__apply_user_include_env(cc_flags);

    // Apply output directory override before creating/deriving any outputs.
    cc_set_out_dir(out_dir, bin_dir);
    cc_refresh_host_obj_root(cc_bin);

    // Determine build.cc path (if any) for help/targets.
    const char* build_path_for_help = NULL;
    char build_buf_help[512];
    int multiple_help = 0;
    if (build_override) build_path_for_help = build_override;
    else if (pos_count > 0) build_path_for_help = choose_build_path(pos_args[0], build_buf_help, sizeof(build_buf_help), &multiple_help);
    else if (file_exists("build.cc")) build_path_for_help = "build.cc";
    if (multiple_help) build_path_for_help = NULL;

    if (help) {
        usage_build(argv[0]);
        // If we can determine a build.cc, print CC_OPTION lines.
        if (build_path_for_help) {
            CCBuildOptionDecl opts[64];
            size_t opt_count = 0;
            if (cc_build_list_options(build_path_for_help, opts, &opt_count, 64) == 0 && opt_count) {
                fprintf(stderr, "\nDeclared CC_OPTIONs in %s:\n", build_path_for_help);
                for (size_t k = 0; k < opt_count; ++k) {
                    fprintf(stderr, "  -D%s  %s\n", opts[k].name, opts[k].help ? opts[k].help : "");
                }
            }
            cc_build_free_options(opts, opt_count);

            CCBuildTargetDecl targets[64];
            size_t target_count = 0;
            char* def_name = NULL;
            if (cc_build_list_targets(build_path_for_help, targets, &target_count, 64, &def_name) == 0 && (target_count || def_name)) {
                fprintf(stderr, "\nDeclared CC_TARGETs in %s:\n", build_path_for_help);
                if (def_name) fprintf(stderr, "  default: %s\n", def_name);
                for (size_t i = 0; i < target_count; ++i) {
                    fprintf(stderr, "  %s (%s)  [", targets[i].name, targets[i].kind == CC_BUILD_TARGET_OBJ ? "obj" : "exe");
                    for (size_t j = 0; j < targets[i].src_count; ++j) {
                        fprintf(stderr, "%s%s", (j ? " " : ""), targets[i].srcs[j]);
                    }
                    fprintf(stderr, "]\n");
                }
            }
            cc_build_free_targets(targets, target_count, def_name);
        }
        return 0;
    }

    if (step != CC_BUILD_STEP_TEST && pos_count == 0 && !build_path_for_help) {
        fprintf(stderr, "cc: missing input (and no build.cc in scope)\n");
        goto parse_fail;
    }

    if (!out_dir && !bin_dir && !user_out && pos_count > 0 &&
        cc__input_is_shcc(unit_kind, pos_args[0])) {
        if (cc__use_script_cache_dirs() == 0)
            cc_refresh_host_obj_root(cc_bin);
    }

    if (ensure_out_dir() != 0) {
        fprintf(stderr, "cc: failed to create out dirs under: %s\n", g_out_root);
        goto parse_fail;
    }

    if (step == CC_BUILD_STEP_LIST) {
        if (!build_path_for_help) {
            fprintf(stderr, "cc: no build.cc in scope (use --build-file)\n");
            goto parse_fail;
        }
        CCBuildTargetDecl targets[64];
        size_t target_count = 0;
        char* def_name = NULL;
        int terr = cc_build_list_targets(build_path_for_help, targets, &target_count, 64, &def_name);
        if (terr != 0) {
            cc_build_free_targets(targets, target_count, def_name);
            goto parse_fail;
        }
        printf("build_file=%s\n", build_path_for_help);
        if (def_name) printf("default=%s\n", def_name);
        for (size_t i = 0; i < target_count; ++i) {
            const char* kind = targets[i].kind == CC_BUILD_TARGET_OBJ ? "obj" : "exe";
            printf("target %s kind=%s\n", targets[i].name, kind);
            printf("  src:");
            for (size_t j = 0; j < targets[i].src_count; ++j) printf(" %s", targets[i].srcs[j]);
            printf("\n");
            if (targets[i].dep_count) {
                printf("  deps:");
                for (size_t j = 0; j < targets[i].dep_count; ++j) printf(" %s", targets[i].deps[j]);
                printf("\n");
            }
            if (targets[i].include_dir_count) {
                printf("  include:");
                for (size_t j = 0; j < targets[i].include_dir_count; ++j) printf(" %s", targets[i].include_dirs[j]);
                printf("\n");
            }
            if (targets[i].define_count) {
                printf("  define:");
                for (size_t j = 0; j < targets[i].define_count; ++j) printf(" %s", targets[i].defines[j]);
                printf("\n");
            }
            if (targets[i].lib_count) {
                printf("  libs:");
                for (size_t j = 0; j < targets[i].lib_count; ++j) printf(" %s", targets[i].libs[j]);
                printf("\n");
            }
            if (targets[i].out_name && targets[i].out_name[0]) printf("  out: %s\n", targets[i].out_name);
            if (targets[i].target_triple && targets[i].target_triple[0]) printf("  target: %s\n", targets[i].target_triple);
            if (targets[i].sysroot && targets[i].sysroot[0]) printf("  sysroot: %s\n", targets[i].sysroot);
            if (targets[i].install_dest && targets[i].install_dest[0]) printf("  install: %s\n", targets[i].install_dest);
            if (targets[i].cflags && targets[i].cflags[0]) printf("  cflags: %s\n", targets[i].cflags);
            if (targets[i].ldflags && targets[i].ldflags[0]) printf("  ldflags: %s\n", targets[i].ldflags);
        }
        cc_build_free_targets(targets, target_count, def_name);
        for (size_t i = 0; i < cli_count; ++i) free(cli_names[i]);
        return 0;
    }

    if (step == CC_BUILD_STEP_GRAPH) {
        if (!build_path_for_help) {
            fprintf(stderr, "cc: no build.cc in scope (use --build-file)\n");
            goto parse_fail;
        }
        const char* format = "json";
        // Parse optional `--format json|dot` after the step token.
        for (int i = argi; i < argc; ++i) {
            if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
                format = argv[i + 1];
                break;
            }
        }

        CCBuildTargetDecl targets[64];
        size_t target_count = 0;
        char* def_name = NULL;
        int terr = cc_build_list_targets(build_path_for_help, targets, &target_count, 64, &def_name);
        if (terr != 0) {
            cc_build_free_targets(targets, target_count, def_name);
            goto parse_fail;
        }

        FILE* graph_fp = stdout;
        FILE* graph_file = NULL;
        if (graph_out) {
            graph_file = fopen(graph_out, "w");
            if (!graph_file) {
                fprintf(stderr, "cc: failed to open graph output: %s\n", graph_out);
                cc_build_free_targets(targets, target_count, def_name);
                goto parse_fail;
            }
            graph_fp = graph_file;
        }
        if (strcmp(format, "dot") == 0) {
            fprintf(graph_fp, "digraph build {\n");
            if (def_name) fprintf(graph_fp, "  \"__default__\" [shape=box,label=\"default=%s\"];\n", def_name);
            for (size_t i = 0; i < target_count; ++i) {
                const char* kind = targets[i].kind == CC_BUILD_TARGET_OBJ ? "obj" : "exe";
                fprintf(graph_fp, "  \"%s\" [label=\"%s (%s)\"];\n", targets[i].name, targets[i].name, kind);
            }
            for (size_t i = 0; i < target_count; ++i) {
                for (size_t j = 0; j < targets[i].dep_count; ++j) {
                    fprintf(graph_fp, "  \"%s\" -> \"%s\";\n", targets[i].name, targets[i].deps[j]);
                }
            }
            fprintf(graph_fp, "}\n");
        } else {
            fprintf(graph_fp, "{\"build_file\":\"%s\",\"default\":", build_path_for_help);
            if (def_name) fprintf(graph_fp, "\"%s\"", def_name); else fprintf(graph_fp, "null");
            fprintf(graph_fp, ",\"targets\":[");
            for (size_t i = 0; i < target_count; ++i) {
                const char* kind = targets[i].kind == CC_BUILD_TARGET_OBJ ? "obj" : "exe";
                if (i) fprintf(graph_fp, ",");
                fprintf(graph_fp, "{\"name\":\"%s\",\"kind\":\"%s\",\"src\":[", targets[i].name, kind);
                for (size_t j = 0; j < targets[i].src_count; ++j) {
                    if (j) fprintf(graph_fp, ",");
                    fprintf(graph_fp, "\"%s\"", targets[i].srcs[j]);
                }
                fprintf(graph_fp, "],\"deps\":[");
                for (size_t j = 0; j < targets[i].dep_count; ++j) {
                    if (j) fprintf(graph_fp, ",");
                    fprintf(graph_fp, "\"%s\"", targets[i].deps[j]);
                }
                fprintf(graph_fp, "]}");
            }
            fprintf(graph_fp, "]}\n");
        }
        if (graph_file) fclose(graph_file);

        cc_build_free_targets(targets, target_count, def_name);
        for (size_t i = 0; i < cli_count; ++i) free(cli_names[i]);
        return 0;
    }

    // Export Makefile fragment for legacy build integration.
    if (step == CC_BUILD_STEP_EXPORT_MAKE) {
        if (!build_path_for_help) {
            fprintf(stderr, "cc: no build.cc in scope (use --build-file)\n");
            goto parse_fail;
        }

        CCBuildTargetDecl targets[64];
        size_t target_count = 0;
        char* def_name = NULL;
        int terr = cc_build_list_targets(build_path_for_help, targets, &target_count, 64, &def_name);
        if (terr != 0) {
            cc_build_free_targets(targets, target_count, def_name);
            goto parse_fail;
        }

        char build_dir[PATH_MAX];
        cc__dir_of_path(build_path_for_help, build_dir, sizeof(build_dir));

        // Determine output path (default: out/cc_targets.mk)
        char mk_path[PATH_MAX];
        if (user_out && user_out[0]) {
            strncpy(mk_path, user_out, sizeof(mk_path));
            mk_path[sizeof(mk_path) - 1] = '\0';
        } else {
            snprintf(mk_path, sizeof(mk_path), "%s/cc_targets.mk", g_out_root);
        }

        // Ensure output directory exists
        char mk_dir[PATH_MAX];
        cc__dir_of_path(mk_path, mk_dir, sizeof(mk_dir));
        if (mk_dir[0]) cc__mkdir_p(mk_dir);

        FILE* mk = fopen(mk_path, "w");
        if (!mk) {
            fprintf(stderr, "cc: failed to create %s\n", mk_path);
            cc_build_free_targets(targets, target_count, def_name);
            goto parse_fail;
        }

        fprintf(mk, "# Generated by ccc build export-make\n");
        fprintf(mk, "# Include this file in your Makefile to integrate CC targets\n");
        fprintf(mk, "#\n");
        fprintf(mk, "# Usage:\n");
        fprintf(mk, "#   include out/cc_targets.mk\n");
        fprintf(mk, "#   $(CC) $(CC_CFLAGS) -c $(CC_SRCS_myapp) -o myapp.o\n");
        fprintf(mk, "#\n\n");

        fprintf(mk, "CC_BUILD_FILE := %s\n", build_path_for_help);
        fprintf(mk, "CC_OUT_DIR := %s\n", g_out_root);
        fprintf(mk, "CC_INCLUDE := -I%s -I%s\n", g_cc_lowered_include, g_cc_include);
        fprintf(mk, "CC_RUNTIME_C := %s\n", g_cc_runtime_c);
        if (g_cc_runtime_o[0]) {
            fprintf(mk, "CC_RUNTIME_O := %s\n", g_cc_runtime_o);
        }
        fprintf(mk, "\n");

        if (def_name) {
            fprintf(mk, "CC_DEFAULT_TARGET := %s\n\n", def_name);
        }

        fprintf(mk, "CC_TARGETS :=");
        for (size_t i = 0; i < target_count; ++i) {
            fprintf(mk, " %s", targets[i].name);
        }
        fprintf(mk, "\n\n");

        for (size_t i = 0; i < target_count; ++i) {
            const CCBuildTargetDecl* t = &targets[i];
            const char* name = t->name;

            fprintf(mk, "# Target: %s\n", name);
            fprintf(mk, "CC_KIND_%s := %s\n", name, t->kind == CC_BUILD_TARGET_OBJ ? "obj" : "exe");

            // Source files (original .ccs and .c)
            fprintf(mk, "CC_SRCS_%s :=", name);
            for (size_t j = 0; j < t->src_count; ++j) {
                char abs_src[PATH_MAX];
                cc__join_path(build_dir, t->srcs[j], abs_src, sizeof(abs_src));
                fprintf(mk, " %s", abs_src);
            }
            fprintf(mk, "\n");

            // Generated C files (predict output paths based on naming scheme)
            fprintf(mk, "CC_GEN_C_%s :=", name);
            for (size_t j = 0; j < t->src_count; ++j) {
                const char* src = t->srcs[j];
                // .ccs / .shcc files get lowered to .c
                size_t slen = strlen(src);
                size_t ext_len = 0;
                if (slen > 5 && strcmp(src + slen - 5, ".shcc") == 0) ext_len = 5;
                else if (slen > 4 && strcmp(src + slen - 4, ".ccs") == 0) ext_len = 4;
                if (ext_len) {
                    // Extract basename without extension
                    const char* base = strrchr(src, '/');
                    base = base ? base + 1 : src;
                    char stem[256];
                    size_t stem_len = strlen(base) - ext_len;
                    if (stem_len >= sizeof(stem)) stem_len = sizeof(stem) - 1;
                    memcpy(stem, base, stem_len);
                    stem[stem_len] = '\0';
                    fprintf(mk, " $(CC_OUT_DIR)/c/%s/%s.c", name, stem);
                }
            }
            fprintf(mk, "\n");

            // Include directories
            fprintf(mk, "CC_CFLAGS_%s := $(CC_INCLUDE)", name);
            for (size_t j = 0; j < t->include_dir_count; ++j) {
                char inc_abs[PATH_MAX];
                cc__join_path(build_dir, t->include_dirs[j], inc_abs, sizeof(inc_abs));
                fprintf(mk, " -I%s", inc_abs);
            }
            for (size_t j = 0; j < t->define_count; ++j) {
                fprintf(mk, " -D%s", t->defines[j]);
            }
            if (t->cflags && t->cflags[0]) {
                fprintf(mk, " %s", t->cflags);
            }
            fprintf(mk, "\n");

            // Link flags
            fprintf(mk, "CC_LDFLAGS_%s :=", name);
            if (t->ldflags && t->ldflags[0]) {
                fprintf(mk, " %s", t->ldflags);
            }
            for (size_t j = 0; j < t->lib_count; ++j) {
                const char* lib = t->libs[j];
                if (lib[0] == '-' || strchr(lib, '/')) {
                    fprintf(mk, " %s", lib);
                } else {
                    fprintf(mk, " -l%s", lib);
                }
            }
            fprintf(mk, "\n");

            // Dependencies
            if (t->dep_count > 0) {
                fprintf(mk, "CC_DEPS_%s :=", name);
                for (size_t j = 0; j < t->dep_count; ++j) {
                    fprintf(mk, " %s", t->deps[j]);
                }
                fprintf(mk, "\n");
            }

            fprintf(mk, "\n");
        }

        // Helper target to lower all .ccs files
        fprintf(mk, "# Helper: lower all .ccs to .c\n");
        fprintf(mk, "cc-emit-c:\n");
        fprintf(mk, "\t@for t in $(CC_TARGETS); do \\\n");
        fprintf(mk, "\t  ccc build --emit-c-only $$t; \\\n");
        fprintf(mk, "\tdone\n\n");

        fprintf(mk, "# Helper: list targets\n");
        fprintf(mk, "cc-list:\n");
        fprintf(mk, "\t@echo \"CC targets: $(CC_TARGETS)\"\n");
        fprintf(mk, "\t@echo \"Default: $(CC_DEFAULT_TARGET)\"\n");

        fclose(mk);
        printf("Wrote %s\n", mk_path);

        cc_build_free_targets(targets, target_count, def_name);
        for (size_t i = 0; i < cli_count; ++i) free(cli_names[i]);
        return 0;
    }

    // Special step: test (no input file required).
    if (step == CC_BUILD_STEP_TEST) {
        const char* host_cc = pick_cc_bin(cc_bin);
        char target_part[256];
        char sysroot_part[256];
        target_part[0] = '\0';
        sysroot_part[0] = '\0';
        if (target_flag && *target_flag) snprintf(target_part, sizeof(target_part), "--target %s", target_flag);
        if (sysroot_flag && *sysroot_flag) snprintf(sysroot_part, sizeof(sysroot_part), "--sysroot %s", sysroot_flag);

        if (ensure_cc_test_tool(host_cc, target_part, sysroot_part, cc_flags, verbose) != 0) {
            goto parse_fail;
        }
        char tool_path[PATH_MAX];
        snprintf(tool_path, sizeof(tool_path), "%s/tools/cc_test", g_repo_root);

        char* exec_argv[128];
        int idx = 0;
        exec_argv[idx++] = tool_path;
        for (int j = 0; j < run_argc && idx < (int)(sizeof(exec_argv) / sizeof(exec_argv[0]) - 1); ++j) {
            exec_argv[idx++] = run_argv[j];
        }
        exec_argv[idx] = NULL;

        if (summary) {
            fprintf(stderr, "cc build summary:\n  step: test\n  tool: %s\n  out_dir: %s\n  bin_dir: %s\n", tool_path, g_out_root, g_bin_root);
        }
        int rc = run_exec(tool_path, exec_argv, verbose);
        return rc;
    }

    // Determine inputs + legacy output behavior.
    const char* inputs[max_pos];
    int input_count = 0;
    const char* legacy_out = NULL;
    if (!saw_o && pos_count == 2) {
        // Legacy: cc build <in> <out>
        inputs[0] = pos_args[0];
        input_count = 1;
        legacy_out = pos_args[1];
        // Safety: don't overwrite existing source-like files via legacy two-arg form.
        if (legacy_out && file_exists(legacy_out) &&
            (cc__ends_with(legacy_out, ".c") || cc__ends_with(legacy_out, ".ccs") || cc__ends_with(legacy_out, ".cch"))) {
            fprintf(stderr, "cc: refusing to overwrite existing source file via legacy `cc build <in> <out>`: %s\n", legacy_out);
            fprintf(stderr, "cc: use -o to set outputs, or delete the file explicitly if you really intend to overwrite it.\n");
            goto parse_fail;
        }
    } else {
        for (int i = 0; i < pos_count; ++i) inputs[i] = pos_args[i];
        input_count = pos_count;
    }

    if (step == CC_BUILD_STEP_RUN || step == CC_BUILD_STEP_INSTALL) {
        mode = CC_MODE_LINK; // run/install require a binary
    }

    // Target-graph mode: if build.cc declares targets and user gave either:
    //  - no inputs (build default target), or
    //  - a single positional that is not an existing file (treat as target name).
    if (build_path_for_help) {
        int want_target = 0;
        const char* target_name = NULL;
        if (step != CC_BUILD_STEP_TEST && input_count == 0) {
            want_target = 1;
        } else if (step != CC_BUILD_STEP_TEST && input_count == 1 && inputs[0] && !file_exists(inputs[0])) {
            want_target = 1;
            target_name = inputs[0];
        }
        if (want_target) {
            // Heap-allocate to avoid stack overflow with many targets.
            CCBuildTargetDecl* targets = (CCBuildTargetDecl*)calloc(64, sizeof(CCBuildTargetDecl));
            if (!targets) { fprintf(stderr, "cc: out of memory\n"); goto parse_fail; }
            size_t target_count = 0;
            char* def_name = NULL;
            int terr = cc_build_list_targets(build_path_for_help, targets, &target_count, 64, &def_name);
            if (terr != 0) {
                cc_build_free_targets(targets, target_count, def_name);
                free(targets);
                goto parse_fail;
            }
            if (target_count == 0) {
                fprintf(stderr, "cc: build.cc has no CC_TARGET entries\n");
                cc_build_free_targets(targets, target_count, def_name);
                free(targets);
                goto parse_fail;
            }

            const CCBuildTargetDecl* chosen = NULL;
            if (target_name) {
                for (size_t i = 0; i < target_count; ++i) {
                    if (strcmp(targets[i].name, target_name) == 0) { chosen = &targets[i]; break; }
                }
                if (!chosen) {
                    fprintf(stderr, "cc: unknown target '%s' (see `cc build --help`)\n", target_name);
                    cc_build_free_targets(targets, target_count, def_name);
                    free(targets);
                    goto parse_fail;
                }
            } else {
                if (def_name) {
                    for (size_t i = 0; i < target_count; ++i) {
                        if (strcmp(targets[i].name, def_name) == 0) { chosen = &targets[i]; break; }
                    }
                }
                if (!chosen) {
                    for (size_t i = 0; i < target_count; ++i) {
                        if (strcmp(targets[i].name, "default") == 0) { chosen = &targets[i]; break; }
                    }
                }
                if (!chosen && target_count == 1) chosen = &targets[0];
                if (!chosen) {
                    fprintf(stderr, "cc: no default target; specify one with CC_DEFAULT or pass a target name\n");
                    cc_build_free_targets(targets, target_count, def_name);
                    free(targets);
                    goto parse_fail;
                }
            }

            // Target-graph build (deps compile once into out/{c,obj}/<target>/..., then dependents link objects).
            const int chosen_idx = (int)(chosen - targets);
            char build_dir[PATH_MAX];
            cc__dir_of_path(build_path_for_help, build_dir, sizeof(build_dir));

            // Build signature for caching.
            CCFileSig build_sig;
            build_sig.mtime_sec = 0;
            build_sig.size = 0;
            (void)cc__stat_sig(build_path_for_help, &build_sig);
            CCFileSig cc_sig;
            cc_sig.mtime_sec = 0;
            cc_sig.size = 0;
            (void)cc__stat_sig(g_ccc_sig_path[0] ? g_ccc_sig_path : (g_ccc_path[0] ? g_ccc_path : ""), &cc_sig);
            const int cache_ok = !cc__cache_disabled(no_cache);

            // Load const bindings once.
            CCConstBinding bindings[128];
            size_t binding_count = 0;
            CCBuildOptions base_opt = {
                .in_path = build_path_for_help,
                .cc_bin_override = cc_bin,
                .cc_flags = cc_flags,
                .ld_flags = ld_flags,
                .target_flag = target_flag ? target_flag : "",
                .sysroot_flag = sysroot_flag ? sysroot_flag : "",
                .opt_release = opt_release,
                .opt_debug = opt_debug,
                .no_runtime = no_runtime,
                .keep_c = keep_c,
                .verbose = verbose,
                .build_override = build_path_for_help,
                .no_build = no_build,
                .dump_consts = dump_consts,
                .dump_comptime = dump_comptime,
                .dry_run = dry_run,
                .summary = summary,
                .out_dir = g_out_root,
                .bin_dir = g_bin_root,
                .no_cache = no_cache,
                .cli_names = cli_names,
                .cli_values = cli_values,
                .cli_count = cli_count,
                .unit_kind = unit_kind,
                .ccc_version_pin = version_pin[0] ? version_pin : NULL,
                .mode = mode,
            };
            int berr = cc__load_const_bindings(&base_opt, bindings, &binding_count);
            if (berr != 0) {
                cc_build_free_targets(targets, target_count, def_name);
                free(targets);
                goto parse_fail;
            }
            CCCompileConfig cfg = {.consts = bindings, .const_count = binding_count};
            if (dry_run) {
                cc_build_free_targets(targets, target_count, def_name);
                free(targets);
                for (size_t i = 0; i < cli_count; ++i) free(cli_names[i]);
                return 0;
            }

            if ((step == CC_BUILD_STEP_RUN || step == CC_BUILD_STEP_INSTALL) && chosen->kind != CC_BUILD_TARGET_EXE) {
                fprintf(stderr, "cc: step requires an exe target, but '%s' is obj\n", chosen->name);
                cc_build_free_targets(targets, target_count, def_name);
                free(targets);
                goto parse_fail;
            }

            // Compile deps + chosen (once).
            CCTargetObjCache caches[64];
            memset(caches, 0, sizeof(caches));
            char chain[64][128];
            memset(chain, 0, sizeof(chain));
            char prefetch_ccs[128][PATH_MAX];
            int nprefetch = 0;
            {
                char abs_ccs[128][PATH_MAX];
                const char* ptrs[128];
                int nc = 0;
                unsigned char needed[64];
                unsigned char walk[64];
                size_t ti, sj;
                memset(needed, 0, sizeof(needed));
                memset(walk, 0, sizeof(walk));
                if (cc__mark_needed_targets(chosen_idx, targets, target_count,
                                            needed, walk) == 0) {
                    for (ti = 0; ti < target_count && nc < 128; ti++) {
                        if (!needed[ti]) continue;
                        for (sj = 0; sj < targets[ti].src_count && nc < 128;
                             sj++) {
                            const char* s = targets[ti].srcs[sj];
                            size_t sl;
                            if (!s) continue;
                            sl = strlen(s);
                            if (!((sl > 4 && strcmp(s + sl - 4, ".ccs") == 0) ||
                                  (sl > 5 && strcmp(s + sl - 5, ".shcc") == 0)))
                                continue;
                            cc__join_path(build_dir, s, abs_ccs[nc],
                                          sizeof(abs_ccs[nc]));
                            ptrs[nc] = abs_ccs[nc];
                            snprintf(prefetch_ccs[nc], PATH_MAX, "%s",
                                     abs_ccs[nc]);
                            nc++;
                        }
                    }
                    nprefetch = nc;
                    if (nc > 0 && cc_check_link_set_faces(ptrs, nc) != 0) {
                        cc_build_free_targets(targets, target_count, def_name);
                        free(targets);
                        goto parse_fail;
                    }
                }
            }
            /* Overlap --release runtime.o with TU emit. Workers emit .c and
             * skip ensure_runtime; flock inside ensure serializes leftovers. */
            pid_t rt_pid = -1;
            if (!no_runtime) {
                long long t_fork = cc__now_ms();
                rt_pid = fork();
                if (rt_pid == 0) {
                    char warm_target[256];
                    char warm_sysroot[256];
                    char warm_rt[PATH_MAX];
                    int warm_reused = 0;
                    cc__make_cross_parts(chosen,
                                         target_flag ? target_flag : "",
                                         sysroot_flag ? sysroot_flag : "",
                                         warm_target, sizeof(warm_target),
                                         warm_sysroot, sizeof(warm_sysroot));
                    int rc = cc__ensure_runtime_obj(&base_opt, warm_target,
                                                    warm_sysroot, warm_rt,
                                                    sizeof(warm_rt),
                                                    &warm_reused);
                    _exit(rc == 0 ? 0 : 1);
                }
                if (rt_pid < 0) {
                    char warm_target[256];
                    char warm_sysroot[256];
                    char warm_rt[PATH_MAX];
                    int warm_reused = 0;
                    cc__make_cross_parts(chosen,
                                         target_flag ? target_flag : "",
                                         sysroot_flag ? sysroot_flag : "",
                                         warm_target, sizeof(warm_target),
                                         warm_sysroot, sizeof(warm_sysroot));
                    if (cc__ensure_runtime_obj(&base_opt, warm_target,
                                               warm_sysroot, warm_rt,
                                               sizeof(warm_rt),
                                               &warm_reused) != 0) {
                        cc_build_free_targets(targets, target_count, def_name);
                        free(targets);
                        goto parse_fail;
                    }
                    cc__prof_span("ensure_runtime_once", t_fork);
                } else {
                    cc__prof_span("ensure_runtime_fork", t_fork);
                }
            }
            int jobs = cc__resolve_build_jobs(g_build_jobs);
            int r;
            CCKeepKids keep;
            memset(&keep, 0, sizeof(keep));
            if (rt_pid > 0) cc__keep_add(&keep, rt_pid);
            if (nprefetch > 0) {
                long long t_pf = cc__now_ms();
                if (cc__prefetch_ccs_start(prefetch_ccs, nprefetch, jobs,
                                           &keep) != 0) {
                    /* Start failed: TUs still lower on demand. */
                }
                cc__prof_span("prefetch_ccs_fork", t_pf);
            }
            long long t_objs = cc__now_ms();
            if (jobs <= 1) {
                r = cc__build_target_objs_rec(chosen_idx, targets, target_count, build_dir, &cfg, &base_opt,
                                              target_flag ? target_flag : "", sysroot_flag ? sysroot_flag : "",
                                              &build_sig, &cc_sig, cache_ok, caches, chain, 0);
            } else {
                r = cc__build_target_objs_parallel(chosen_idx, targets, target_count, build_dir, &cfg, &base_opt,
                                                   target_flag ? target_flag : "", sysroot_flag ? sysroot_flag : "",
                                                   &build_sig, &cc_sig, cache_ok, caches, jobs,
                                                   &keep);
            }
            cc__prof_span("build_objs", t_objs);
            {
                long long t_w = cc__now_ms();
                int bg = cc__keep_wait_all(&keep);
                cc__prof_span("bg_wait", t_w);
                if (bg != 0 && r == 0 && rt_pid > 0) {
                    /* Prefetch is best-effort. runtime.o is not. */
                    int i, rt_bad = 0;
                    for (i = 0; i < keep.n; i++) {
                        if (keep.pid[i] == rt_pid &&
                            (keep.st[i] < 0 || !WIFEXITED(keep.st[i]) ||
                             WEXITSTATUS(keep.st[i]) != 0))
                            rt_bad = 1;
                    }
                    if (rt_bad) {
                        fprintf(stderr, "cc: runtime.o compile failed\n");
                        cc_build_free_targets(targets, target_count, def_name);
                        free(targets);
                        goto parse_fail;
                    }
                }
            }
            if (r == -2) {
                fprintf(stderr, "cc: cycle in CC_TARGET_DEPS\n");
                cc_build_free_targets(targets, target_count, def_name);
                free(targets);
                goto parse_fail;
            } else if (r == -3) {
                fprintf(stderr, "cc: unknown dep target in CC_TARGET_DEPS\n");
                cc_build_free_targets(targets, target_count, def_name);
                free(targets);
                goto parse_fail;
            } else if (r != 0) {
                fprintf(stderr, "cc: target build failed for '%s' (err=%d)\n", chosen->name, r);
                cc_build_free_targets(targets, target_count, def_name);
                free(targets);
                goto parse_fail;
            }

            if (mode == CC_MODE_EMIT_C || mode == CC_MODE_COMPILE || chosen->kind == CC_BUILD_TARGET_OBJ) {
                cc_build_free_targets(targets, target_count, def_name);
                free(targets);
                for (size_t i = 0; i < cli_count; ++i) free(cli_names[i]);
                return 0;
            }

            // Determine output path.
            static char default_bin[PATH_MAX];
            const char* out_name = chosen->out_name && chosen->out_name[0] ? chosen->out_name : chosen->name;
            snprintf(default_bin, sizeof(default_bin), "%s/%s", g_bin_root, out_name);
            if (!user_out) user_out = default_bin;
            char out_dirname[PATH_MAX];
            cc__dir_of_path(user_out, out_dirname, sizeof(out_dirname));
            if (out_dirname[0]) (void)cc__mkdir_p(out_dirname);

            // Gather objects for link (dep closure + chosen).
            const char* obj_paths[256];
            uint64_t obj_keys[256];
            size_t obj_count = 0;
            unsigned char vis[64];
            memset(vis, 0, sizeof(vis));
            int gr = cc__gather_obj_closure(chosen_idx, targets, target_count, caches, vis, obj_paths, obj_keys, &obj_count, 256);
            if (gr != 0) {
                fprintf(stderr, "cc: failed to gather dep objects (err=%d)\n", gr);
                cc_build_free_targets(targets, target_count, def_name);
                free(targets);
                goto parse_fail;
            }

            // Merge link flags from closure + CLI.
            static char merged_ld[4096];
            merged_ld[0] = '\0';
            for (size_t i = 0; i < target_count; ++i) {
                if (vis[i]) cc__merge_target_link_flags(&targets[i], merged_ld, sizeof(merged_ld));
            }
            if (ld_flags && ld_flags[0]) cc__append_spaced(merged_ld, sizeof(merged_ld), ld_flags);
            base_opt.ld_flags = merged_ld[0] ? merged_ld : ld_flags;

            // Link.
            char chosen_target_part[256];
            char chosen_sysroot_part[256];
            cc__make_cross_parts(chosen, target_flag ? target_flag : "", sysroot_flag ? sysroot_flag : "",
                                 chosen_target_part, sizeof(chosen_target_part),
                                 chosen_sysroot_part, sizeof(chosen_sysroot_part));

            char runtime_path[PATH_MAX];
            int runtime_reused = 0;
            {
                long long t_rt = cc__now_ms();
                if (cc__ensure_runtime_obj(&base_opt, chosen_target_part, chosen_sysroot_part, runtime_path, sizeof(runtime_path), &runtime_reused) != 0) {
                    cc_build_free_targets(targets, target_count, def_name);
                    free(targets);
                    goto parse_fail;
                }
                cc__prof_span("ensure_runtime_link", t_rt);
            }

            int link_reused = 0;
            long long t_link = cc__now_ms();
            if (cache_ok) {
                char out_stem[128];
                cc__stem_from_path(user_out, out_stem, sizeof(out_stem));
                char link_meta_path[PATH_MAX];
                snprintf(link_meta_path, sizeof(link_meta_path), "%s/%s.link",
                         g_host_obj_root[0] ? g_host_obj_root : g_cache_root, out_stem);
                CCFileSig rt_sig;
                rt_sig.mtime_sec = 0;
                rt_sig.size = 0;
                if (!no_runtime) (void)cc__stat_sig(runtime_path, &rt_sig);
                uint64_t h = 1469598103934665603ULL;
                h = cc__fnv1a64_str(h, chosen_target_part);
                h = cc__fnv1a64_str(h, chosen_sysroot_part);
                h = cc__fnv1a64_str(h, base_opt.ld_flags ? base_opt.ld_flags : "");
                h = cc__fnv1a64_str(h, getenv("LDFLAGS"));
                h = cc__fnv1a64_str(h, g_host_fp);
                h = cc__fnv1a64_i64(h, (long long)rt_sig.mtime_sec);
                h = cc__fnv1a64_i64(h, (long long)rt_sig.size);
                for (size_t i = 0; i < obj_count; ++i) {
                    CCFileSig os;
                    os.mtime_sec = 0;
                    os.size = 0;
                    (void)cc__stat_sig(obj_paths[i], &os);
                    h = cc__fnv1a64_str(h, obj_paths[i]);
                    h = cc__fnv1a64_i64(h, (long long)obj_keys[i]);
                    h = cc__fnv1a64_i64(h, (long long)os.mtime_sec);
                    h = cc__fnv1a64_i64(h, (long long)os.size);
                }
                uint64_t prev = 0;
                if (file_exists(user_out) && cc__read_u64_file(link_meta_path, &prev) == 0 && prev == h) {
                    link_reused = 1;
                } else {
                    if (cc__link_many(&base_opt, obj_paths, obj_count, runtime_path, chosen_target_part, chosen_sysroot_part, user_out) != 0) {
                        cc_build_free_targets(targets, target_count, def_name);
                free(targets);
                        goto parse_fail;
                    }
                    (void)cc__write_u64_file(link_meta_path, h);
                }
            } else {
                if (cc__link_many(&base_opt, obj_paths, obj_count, runtime_path, chosen_target_part, chosen_sysroot_part, user_out) != 0) {
                    cc_build_free_targets(targets, target_count, def_name);
                free(targets);
                    goto parse_fail;
                }
            }
            cc__prof_span(link_reused ? "link_reuse" : "link", t_link);

            if (summary) {
                fprintf(stderr, "cc build summary:\n  step: %s\n  target: %s\n  out_dir: %s\n  obj: %zu\n  bin: %s (%s)\n",
                        step == CC_BUILD_STEP_RUN ? "run" : (step == CC_BUILD_STEP_INSTALL ? "install" : "default"),
                        chosen->name,
                        g_out_root,
                        obj_count,
                        user_out,
                        link_reused ? "reused" : "built");
            }

            if (step == CC_BUILD_STEP_INSTALL) {
                if (!chosen->install_dest || !chosen->install_dest[0]) {
                    fprintf(stderr, "cc: CC_INSTALL missing for target '%s'\n", chosen->name);
                    cc_build_free_targets(targets, target_count, def_name);
                free(targets);
                    goto parse_fail;
                }
                char dst_abs[PATH_MAX];
                if (chosen->install_dest[0] == '/') {
                    strncpy(dst_abs, chosen->install_dest, sizeof(dst_abs));
                    dst_abs[sizeof(dst_abs) - 1] = '\0';
                } else {
                    snprintf(dst_abs, sizeof(dst_abs), "%s/%s", g_repo_root, chosen->install_dest);
                }
                if (cc__copy_file(user_out, dst_abs) != 0) {
                    fprintf(stderr, "cc: install failed: %s -> %s\n", user_out, dst_abs);
                    cc_build_free_targets(targets, target_count, def_name);
                free(targets);
                    goto parse_fail;
                }
                if (summary) fprintf(stderr, "  install: %s\n", dst_abs);
            }

            cc_build_free_targets(targets, target_count, def_name);
                free(targets);
            for (size_t i = 0; i < cli_count; ++i) free(cli_names[i]);
            if (step == CC_BUILD_STEP_RUN) {
                char* exec_argv[64];
                int idx2 = 0;
                exec_argv[idx2++] = (char*)user_out;
                for (int j = 0; j < run_argc && idx2 < (int)(sizeof(exec_argv) / sizeof(exec_argv[0]) - 1); ++j) {
                    exec_argv[idx2++] = run_argv[j];
                }
                exec_argv[idx2] = NULL;
                return run_exec(user_out, exec_argv, verbose);
            }
            return 0;
        }
    }

    if (step == CC_BUILD_STEP_INSTALL) {
        fprintf(stderr, "cc: install is only supported for build.cc targets (use `cc build install <target>`)\n");
        goto parse_fail;
    }

    if (input_count > 1) {
        if (mode == CC_MODE_EMIT_C && saw_o) {
            fprintf(stderr, "cc: -o with multiple inputs in --emit-c-only mode is not supported\n");
            goto parse_fail;
        }
        if (mode == CC_MODE_COMPILE && obj_out) {
            fprintf(stderr, "cc: --obj-out with multiple inputs is not supported\n");
            goto parse_fail;
        }
        if (mode == CC_MODE_LINK && !user_out) {
            fprintf(stderr, "cc: linking multiple inputs requires -o <output>\n");
            goto parse_fail;
        }

        // Load const bindings once (build.cc discovery uses the first input).
        CCConstBinding bindings[128];
        size_t binding_count = 0;
        CCBuildOptions base_opt = {
            .in_path = inputs[0],
            .cc_bin_override = cc_bin,
            .cc_flags = cc_flags,
            .ld_flags = ld_flags,
            .target_flag = target_flag ? target_flag : "",
            .sysroot_flag = sysroot_flag ? sysroot_flag : "",
            .opt_release = opt_release,
            .opt_debug = opt_debug,
            .no_runtime = no_runtime,
            .keep_c = keep_c,
            .verbose = verbose,
            .build_override = build_override,
            .no_build = no_build,
            .dump_consts = dump_consts,
            .dump_comptime = dump_comptime,
            .dry_run = dry_run,
            .summary = summary,
            .out_dir = g_out_root,
            .bin_dir = g_bin_root,
            .no_cache = no_cache,
            .cli_names = cli_names,
            .cli_values = cli_values,
            .cli_count = cli_count,
            .unit_kind = unit_kind,
            .ccc_version_pin = version_pin[0] ? version_pin : NULL,
        };
        int berr = cc__load_const_bindings(&base_opt, bindings, &binding_count);
        if (berr != 0) goto parse_fail;
        if (dump_consts) {
            for (size_t i = 0; i < binding_count; ++i) {
                printf("CONST %s=%lld\n", bindings[i].name, bindings[i].value);
            }
        }
        if (dry_run) {
            for (size_t i = 0; i < cli_count; ++i) free(cli_names[i]);
            return 0;
        }
        CCCompileConfig cfg = {.consts = bindings, .const_count = binding_count};

        char target_part[256]; char sysroot_part[256];
        target_part[0] = '\0'; sysroot_part[0] = '\0';
        if (target_flag && *target_flag) snprintf(target_part, sizeof(target_part), "--target %s", target_flag);
        if (sysroot_flag && *sysroot_flag) snprintf(sysroot_part, sizeof(sysroot_part), "--sysroot %s", sysroot_flag);

        // Emit + compile objects (with incremental cache per input).
        int cache_ok = !cc__cache_disabled(no_cache);
        char build_buf_for_key[512];
        int multiple_for_key = 0;
        const char* build_path_for_key = build_override ? build_override : choose_build_path(inputs[0], build_buf_for_key, sizeof(build_buf_for_key), &multiple_for_key);
        if (multiple_for_key) build_path_for_key = NULL;
        CCFileSig build_sig_for_key;
        build_sig_for_key.mtime_sec = 0;
        build_sig_for_key.size = 0;
        if (build_path_for_key) (void)cc__stat_sig(build_path_for_key, &build_sig_for_key);
        CCFileSig cc_sig_for_key;
        cc_sig_for_key.mtime_sec = 0;
        cc_sig_for_key.size = 0;
        // Cache key should include the ccc binary itself (driver/lowering changes), not just the host C compiler.
        (void)cc__stat_sig(g_ccc_sig_path[0] ? g_ccc_sig_path : (g_ccc_path[0] ? g_ccc_path : "cc"), &cc_sig_for_key);

        int emit_reused = 0, emit_built = 0;
        int obj_reused = 0, obj_built = 0;

        if (mode == CC_MODE_LINK &&
            cc_check_link_set_faces((const char* const*)inputs, input_count) != 0)
            goto parse_fail;

        char used[64][128]; size_t used_count = 0;
        const char* obj_paths[64];
        char obj_bufs[64][PATH_MAX];
        char c_bufs[64][PATH_MAX];
        char dep_bufs[64][PATH_MAX];
        char src_dir_bufs[64][PATH_MAX];
        uint64_t obj_keys[64];
        memset(obj_keys, 0, sizeof(obj_keys));
        for (int i = 0; i < input_count; ++i) {
            char stem0[128];
            char stem[128];
            cc__stem_from_path(inputs[i], stem0, sizeof(stem0));
            if (cc__unique_stem(stem0, used, &used_count, 64, stem, sizeof(stem)) != 0) {
                fprintf(stderr, "cc: failed to derive unique stem for %s\n", inputs[i]);
                goto parse_fail;
            }
            cc__derive_c_path_from_stem(stem, c_bufs[i], sizeof(c_bufs[i]));
            cc__derive_o_path_from_stem(stem, obj_bufs[i], sizeof(obj_bufs[i]));
            cc__derive_d_path_from_stem(stem, dep_bufs[i], sizeof(dep_bufs[i]));
            cc__dir_of_path(inputs[i], src_dir_bufs[i], sizeof(src_dir_bufs[i]));

            uint64_t emit_key = 0;
            char meta_path[PATH_MAX];
            snprintf(meta_path, sizeof(meta_path), "%s/%s.meta", g_cache_root, stem);
            int is_raw_c = cc__is_raw_c(inputs[i]);
            if (is_raw_c) {
                if (cc__materialize_host_c(inputs[i], c_bufs[i]) != 0) {
                    fprintf(stderr, "cc: failed to materialize host C %s -> %s\n",
                            inputs[i], c_bufs[i]);
                    goto parse_fail;
                }
                emit_built++;
            }
            const char* c_for_compile = c_bufs[i];

            if (is_raw_c) {
                /* Host materialize above; no CC lowering. */
            } else if (cache_ok) {
                CCFileSig in_sig;
                in_sig.mtime_sec = 0;
                in_sig.size = 0;
                (void)cc__stat_sig(inputs[i], &in_sig);
                uint64_t h = 1469598103934665603ULL;
                h = cc__fnv1a64_str(h, inputs[i]);
                h = cc__fnv1a64_i64(h, in_sig.mtime_sec);
                h = cc__fnv1a64_i64(h, in_sig.size);
                h = cc__fold_file_content(h, inputs[i]);
                h = cc__fnv1a64_str(h, build_path_for_key ? build_path_for_key : "");
                h = cc__fnv1a64_i64(h, build_sig_for_key.mtime_sec);
                h = cc__fnv1a64_i64(h, build_sig_for_key.size);
                h = cc__fnv1a64_i64(h, cc_sig_for_key.mtime_sec);
                h = cc__fnv1a64_i64(h, cc_sig_for_key.size);
                h = cc__fnv1a64_str(h, target_flag ? target_flag : "");
                h = cc__fnv1a64_str(h, sysroot_flag ? sysroot_flag : "");
                h = cc__fnv1a64_str(h, cc_flags);
                h = cc__fnv1a64_str(h, getenv("CFLAGS"));
                h = cc__fnv1a64_str(h, getenv("CPPFLAGS"));
                h = cc__fnv1a64_str(h, getenv("CC_STRICT_RESULT_UNWRAP"));
                h = cc__fnv1a64_i64(h, (long long)no_build);
                // bake in const bindings + CLI -D (already merged into bindings)
                h = cc__fnv1a64_i64(h, (long long)binding_count);
                for (size_t bi = 0; bi < binding_count; ++bi) {
                    h = cc__fnv1a64_str(h, bindings[bi].name);
                    h = cc__fnv1a64_i64(h, bindings[bi].value);
                }
                h = cc__fold_cc_depends(h, inputs[i]);
                h = cc__fold_cch_includes(h, inputs[i], cc_flags);
                h = cc__fold_ccc_driver(h);
                h = cc__fold_shadow_lower(h);
                h = cc__fold_toolchain_id(h);
                emit_key = h;

                uint64_t prev = 0;
                if (file_exists(c_bufs[i]) && cc__read_u64_file(meta_path, &prev) == 0 && prev == emit_key) {
                    cc__replay_diag_sidecar(c_bufs[i]);
                    emit_reused++;
                } else {
                    int err = cc__compile_with_env(NULL, inputs[i], c_bufs[i], &cfg);
                    if (err != 0) goto parse_fail;
                    (void)cc__write_u64_file(meta_path, emit_key);
                    emit_built++;
                }
            } else {
                if (!is_raw_c) {
                    int err = cc__compile_with_env(NULL, inputs[i], c_bufs[i], &cfg);
                    if (err != 0) goto parse_fail;
                    emit_built++;
                }
            }
            if (mode != CC_MODE_EMIT_C) {
                char obj_meta_path[PATH_MAX];
                snprintf(obj_meta_path, sizeof(obj_meta_path), "%s/%s.obj",
                         g_host_obj_root[0] ? g_host_obj_root : g_cache_root, stem);
                if (cache_ok) {
                    uint64_t h = 1469598103934665603ULL;
                    if (is_raw_c) {
                        CCFileSig in_sig;
                        in_sig.mtime_sec = 0; in_sig.size = 0;
                        (void)cc__stat_sig(inputs[i], &in_sig);
                        h = cc__fnv1a64_str(h, inputs[i]);
                        h = cc__fnv1a64_i64(h, in_sig.mtime_sec);
                        h = cc__fnv1a64_i64(h, in_sig.size);
                    } else {
                        h = cc__fnv1a64_i64(h, (long long)emit_key);
                    }
                    h = cc__fnv1a64_str(h, target_part);
                    h = cc__fnv1a64_str(h, sysroot_part);
                    h = cc__fnv1a64_str(h, cc_flags);
                    h = cc__fnv1a64_str(h, getenv("CFLAGS"));
                    h = cc__fnv1a64_str(h, getenv("CPPFLAGS"));
                    h = cc__fnv1a64_str(h, g_host_fp);
                    uint64_t obj_key = h;
                    obj_keys[i] = obj_key;
                    uint64_t prev = 0;
                    if (file_exists(obj_bufs[i]) && cc__read_u64_file(obj_meta_path, &prev) == 0 && prev == obj_key &&
                        !cc__deps_require_rebuild(dep_bufs[i], obj_bufs[i])) {
                        obj_reused++;
                    } else {
                        if (cc__compile_c_to_obj(&base_opt, c_for_compile, obj_bufs[i], dep_bufs[i], src_dir_bufs[i], target_part, sysroot_part) != 0) goto parse_fail;
                        (void)cc__write_u64_file(obj_meta_path, obj_key);
                        obj_built++;
                    }
                } else {
                    if (cc__compile_c_to_obj(&base_opt, c_for_compile, obj_bufs[i], dep_bufs[i], src_dir_bufs[i], target_part, sysroot_part) != 0) goto parse_fail;
                    obj_built++;
                }
                obj_paths[i] = obj_bufs[i];
            }
        }
        if (mode == CC_MODE_EMIT_C) {
            for (size_t i = 0; i < cli_count; ++i) free(cli_names[i]);
            return 0;
        }
        if (mode == CC_MODE_COMPILE) {
            for (size_t i = 0; i < cli_count; ++i) free(cli_names[i]);
            return 0;
        }

        // Link all objects
        char runtime_path[PATH_MAX];
        int runtime_reused = 0;
        if (cc__ensure_runtime_obj(&base_opt, target_part, sysroot_part, runtime_path, sizeof(runtime_path), &runtime_reused) != 0) goto parse_fail;
        int link_reused = 0;
        if (cache_ok) {
            char out_stem[128];
            cc__stem_from_path(user_out, out_stem, sizeof(out_stem));
            char link_meta_path[PATH_MAX];
            snprintf(link_meta_path, sizeof(link_meta_path), "%s/%s.link",
                     g_host_obj_root[0] ? g_host_obj_root : g_cache_root, out_stem);
            CCFileSig rt_sig;
            rt_sig.mtime_sec = 0; rt_sig.size = 0;
            if (!no_runtime) (void)cc__stat_sig(runtime_path, &rt_sig);
            uint64_t h = 1469598103934665603ULL;
            h = cc__fnv1a64_str(h, target_part);
            h = cc__fnv1a64_str(h, sysroot_part);
            h = cc__fnv1a64_str(h, ld_flags);
            h = cc__fnv1a64_str(h, getenv("LDFLAGS"));
            h = cc__fnv1a64_str(h, g_host_fp);
            h = cc__fnv1a64_i64(h, (long long)rt_sig.mtime_sec);
            h = cc__fnv1a64_i64(h, (long long)rt_sig.size);
            for (int i = 0; i < input_count; ++i) {
                CCFileSig os;
                os.mtime_sec = 0; os.size = 0;
                (void)cc__stat_sig(obj_paths[i], &os);
                h = cc__fnv1a64_str(h, obj_paths[i]);
                h = cc__fnv1a64_i64(h, (long long)obj_keys[i]);
                h = cc__fnv1a64_i64(h, (long long)os.mtime_sec);
                h = cc__fnv1a64_i64(h, (long long)os.size);
            }
            uint64_t prev = 0;
            if (file_exists(user_out) && cc__read_u64_file(link_meta_path, &prev) == 0 && prev == h) {
                link_reused = 1;
            } else {
                if (cc__link_many(&base_opt, obj_paths, (size_t)input_count, runtime_path, target_part, sysroot_part, user_out) != 0) goto parse_fail;
                (void)cc__write_u64_file(link_meta_path, h);
            }
        } else {
            if (cc__link_many(&base_opt, obj_paths, (size_t)input_count, runtime_path, target_part, sysroot_part, user_out) != 0) goto parse_fail;
        }

        if (summary) {
            fprintf(stderr, "cc build summary:\n  step: %s\n  out_dir: %s\n  inputs: %d\n  c: %d built, %d reused\n  obj: %d built, %d reused\n  bin: %s (%s)\n",
                    step == CC_BUILD_STEP_RUN ? "run" : "default",
                    g_out_root,
                    input_count,
                    emit_built, emit_reused,
                    obj_built, obj_reused,
                    user_out,
                    link_reused ? "reused" : "built");
        }

        for (size_t i = 0; i < cli_count; ++i) free(cli_names[i]);
        if (step == CC_BUILD_STEP_RUN) {
            char* exec_argv[64];
            int idx = 0;
            exec_argv[idx++] = (char*)user_out;
            for (int j = 0; j < run_argc && idx < (int)(sizeof(exec_argv) / sizeof(exec_argv[0]) - 1); ++j) {
                exec_argv[idx++] = run_argv[j];
            }
            exec_argv[idx] = NULL;
            return run_exec(user_out, exec_argv, verbose);
        }
        return 0;
    }

    // Single-input path (existing behavior)
    const char* in_path = inputs[0];
    if (!user_out && legacy_out) user_out = legacy_out;

    char c_out[512];
    char obj_path[512];
    char bin_path[512];

    // Normalize input path to an absolute path so debug info (via #line) matches editor paths.
    // This matters for LLDB breakpoint binding in VS Code / Cursor.
    char in_abs[PATH_MAX];
    const char* in_path_abs = in_path;
    if (in_path && in_path[0]) {
        if (realpath(in_path, in_abs) != NULL) {
            in_path_abs = in_abs;
        } else if (!cc__is_abs_path(in_path)) {
            // Best-effort fallback: interpret relative inputs as repo-root-relative.
            snprintf(in_abs, sizeof(in_abs), "%s/%s", g_repo_root, in_path);
            in_path_abs = in_abs;
        }
    }

    /* Extension module: a stdlib-declared entry point exported, no `main`
     * (see CC_MODULE_ENTRY above) — the declaration carries the suffix
     * and the naming rule.  A TU may target several embeddings at once;
     * the object is identical under every entry, so secondaries become
     * hardlinked names after the primary links (below). */
    static CCExtModTarget extmod_tg[4];
    static char extmod_ccflags[1024];
    static char extmod_ldflags[1024];
    int extmod_nt = cc__detect_ext_module(
        in_path_abs, extmod_tg, (int)(sizeof(extmod_tg) / sizeof(extmod_tg[0])));
    if (module_narrow && module_narrow[0]) {
        int w = 0, e;
        for (e = 0; e < extmod_nt; e++)
            if (strcmp(extmod_tg[e].tag, module_narrow) == 0)
                extmod_tg[w++] = extmod_tg[e];
        if (extmod_nt == 0 || w == 0) {
            fprintf(stderr, "cc: --module=%s: this TU spells no such entry"
                            " (targets:",
                    module_narrow);
            for (e = 0; e < extmod_nt; e++)
                fprintf(stderr, " %s",
                        extmod_tg[e].tag[0] ? extmod_tg[e].tag : "<untagged>");
            fprintf(stderr, "%s)\n", extmod_nt ? "" : " none");
            goto parse_fail;
        }
        extmod_nt = w;
    }
    if (extmod_nt > 0) {
        snprintf(extmod_ccflags, sizeof(extmod_ccflags), "%s%s-fPIC",
                 cc_flags ? cc_flags : "", (cc_flags && cc_flags[0]) ? " " : "");
        cc_flags = extmod_ccflags;
        snprintf(extmod_ldflags, sizeof(extmod_ldflags), "%s%s-shared",
                 ld_flags ? ld_flags : "", (ld_flags && ld_flags[0]) ? " " : "");
        /* A -shared link exports every symbol by default, which makes each
         * one a dead-strip root: --gc-sections keeps the entire runtime.
         * Exporting only this TU's embedding entry points (the dynamic
         * loader looks those up by name; nothing else is anyone's ABI)
         * lets the linker collect everything the module never calls —
         * the same artifact drops ~5x.
         *
         * The list is per-TU: Darwin's -exported_symbols_list requires
         * every named symbol to exist, so a Python-only module cannot
         * list _napi_register_module_v1 (and vice versa). Dual-target
         * TUs list every detected entry; --module= narrows the list to
         * the kept artifacts. */
#if defined(__linux__) || defined(__APPLE__)
        if (!cc__is_tcc(pick_cc_bin(cc_bin))) {
            char ver_body[512];
            size_t vb = 0;
            int te;
            char ver_path[PATH_MAX];
            char ver_dir[PATH_MAX];
            int ver_ok = 0;
#if defined(__APPLE__)
            static const char* ver_flag = " -Wl,-exported_symbols_list,";
            ver_body[0] = 0;
            for (te = 0; te < extmod_nt; te++) {
                char line[160];
                int n, x;
                if (!extmod_tg[te].entry[0]) continue;
                if (extmod_tg[te].wildcard && extmod_tg[te].name[0])
                    n = snprintf(line, sizeof(line), "_%s%s\n",
                                 extmod_tg[te].entry, extmod_tg[te].name);
                else if (extmod_tg[te].wildcard)
                    n = snprintf(line, sizeof(line), "_%s*\n",
                                 extmod_tg[te].entry);
                else
                    n = snprintf(line, sizeof(line), "_%s\n",
                                 extmod_tg[te].entry);
                if (n < 0 || (size_t)n >= sizeof(line) ||
                    vb + (size_t)n >= sizeof(ver_body))
                    break;
                memcpy(ver_body + vb, line, (size_t)n);
                vb += (size_t)n;
                ver_body[vb] = 0;
                /* A multiclass wildcard target mints one entry symbol per
                 * export — list every one, or the strip removes it.
                 * Aggregating targets share ONE entry across groups. */
                for (x = 0; extmod_tg[te].wildcard &&
                            x < extmod_tg[te].nextra; x++) {
                    n = snprintf(line, sizeof(line), "_%s%s\n",
                                 extmod_tg[te].entry, extmod_tg[te].extra[x]);
                    if (n < 0 || (size_t)n >= sizeof(line) ||
                        vb + (size_t)n >= sizeof(ver_body))
                        break;
                    memcpy(ver_body + vb, line, (size_t)n);
                    vb += (size_t)n;
                    ver_body[vb] = 0;
                }
            }
#else
            static const char* ver_flag = " -Wl,--version-script=";
            vb = (size_t)snprintf(ver_body, sizeof(ver_body), "{ global:");
            for (te = 0; te < extmod_nt && vb + 2 < sizeof(ver_body); te++) {
                char sym[160];
                int n, x;
                if (!extmod_tg[te].entry[0]) continue;
                if (extmod_tg[te].wildcard && extmod_tg[te].name[0])
                    n = snprintf(sym, sizeof(sym), " %s%s;",
                                 extmod_tg[te].entry, extmod_tg[te].name);
                else if (extmod_tg[te].wildcard)
                    n = snprintf(sym, sizeof(sym), " %s*;",
                                 extmod_tg[te].entry);
                else
                    n = snprintf(sym, sizeof(sym), " %s;",
                                 extmod_tg[te].entry);
                if (n < 0 || (size_t)n >= sizeof(sym) ||
                    vb + (size_t)n >= sizeof(ver_body))
                    break;
                memcpy(ver_body + vb, sym, (size_t)n);
                vb += (size_t)n;
                /* A multiclass wildcard target mints one entry symbol per
                 * export — every one is a dead-strip root, or the strip
                 * removes it and the import fails naming the symbol.
                 * Aggregating targets share ONE entry across groups. */
                for (x = 0; extmod_tg[te].wildcard &&
                            x < extmod_tg[te].nextra; x++) {
                    n = snprintf(sym, sizeof(sym), " %s%s;",
                                 extmod_tg[te].entry, extmod_tg[te].extra[x]);
                    if (n < 0 || (size_t)n >= sizeof(sym) ||
                        vb + (size_t)n >= sizeof(ver_body))
                        break;
                    memcpy(ver_body + vb, sym, (size_t)n);
                    vb += (size_t)n;
                }
            }
            {
                const char* tail = " local: *; };\n";
                size_t tl = strlen(tail);
                if (vb + tl < sizeof(ver_body)) {
                    memcpy(ver_body + vb, tail, tl + 1);
                    vb += tl;
                }
            }
#endif
            snprintf(ver_dir, sizeof(ver_dir), "%s/.cc-build", g_out_root);
            /* Content varies by TU/target — name the file from a short
             * digest of the body so py-only / js-only / dual don't race
             * a shared path, and concurrent links never see a truncate. */
            {
                unsigned h = 2166136261u;
                size_t i;
                for (i = 0; i < vb; i++) {
                    h ^= (unsigned char)ver_body[i];
                    h *= 16777619u;
                }
                snprintf(ver_path, sizeof(ver_path),
                         "%s/extmod-exports-%08x.ver", ver_dir, h);
            }
            (void)cc__mkdir_p(ver_dir);
            if (vb > 0) {
                struct stat ver_st;
                ver_ok = stat(ver_path, &ver_st) == 0 &&
                         ver_st.st_size == (off_t)vb;
                if (!ver_ok) {
                    char tmp_path[PATH_MAX + 32];
                    snprintf(tmp_path, sizeof(tmp_path), "%s.%d.tmp", ver_path,
                             (int)getpid());
                    FILE* vf = fopen(tmp_path, "w");
                    if (vf) {
                        if (fwrite(ver_body, 1, vb, vf) == vb) {
                            fclose(vf);
                            ver_ok = rename(tmp_path, ver_path) == 0;
                            if (!ver_ok) unlink(tmp_path);
                        } else {
                            fclose(vf);
                            unlink(tmp_path);
                        }
                    }
                }
            }
            if (ver_ok) {
                strncat(extmod_ldflags, ver_flag,
                        sizeof(extmod_ldflags) - strlen(extmod_ldflags) - 1);
                strncat(extmod_ldflags, ver_path,
                        sizeof(extmod_ldflags) - strlen(extmod_ldflags) - 1);
            } else {
                fprintf(stderr, "cc: warning: cannot write %s; the module will "
                                "export all symbols (and dead-strip nothing)\n",
                        ver_path);
            }
        }
#endif
        ld_flags = extmod_ldflags;
    }

    int raw_c = cc__is_raw_c(in_path_abs);
    if (mode == CC_MODE_EMIT_C) {
        if (user_out) {
            strncpy(c_out, user_out, sizeof(c_out));
            c_out[sizeof(c_out)-1] = '\0';
        } else if (derive_default_output(in_path_abs, c_out, sizeof(c_out)) != 0) {
            fprintf(stderr, "cc: failed to derive default C output\n");
            goto parse_fail;
        }
    } else {
        /* Raw .c still gets a distinct out path: host materialize rewrites
         * .cch includes → .h before cc sees the TU. */
        if (derive_default_output(in_path_abs, c_out, sizeof(c_out)) != 0) {
            fprintf(stderr, "cc: failed to derive default C output\n");
            goto parse_fail;
        }
        (void)raw_c;
    }

    if (mode != CC_MODE_EMIT_C) {
        if (obj_out) {
            strncpy(obj_path, obj_out, sizeof(obj_path));
            obj_path[sizeof(obj_path)-1] = '\0';
        } else if (derive_default_obj(in_path_abs, obj_path, sizeof(obj_path)) != 0) {
            fprintf(stderr, "cc: failed to derive default object output\n");
            goto parse_fail;
        }
    }

    if (mode == CC_MODE_LINK) {
        if (user_out) {
            strncpy(bin_path, user_out, sizeof(bin_path));
            bin_path[sizeof(bin_path)-1] = '\0';
        } else if (extmod_nt > 0) {
            /* The declaration names the artifact: the export directive's
             * type or override, the entry symbol's suffix (PyInit_counter
             * → counter.abi3.so), the factory's type formal
             * (js_module::[Counter] → counter.node), or the source stem
             * when the declaration offers no name.  The primary target
             * (first declared) names the linked file. */
            char mstem[128];
            if (extmod_tg[0].name[0])
                snprintf(mstem, sizeof(mstem), "%s", extmod_tg[0].name);
            else
                cc__stem_from_path(in_path_abs, mstem, sizeof(mstem));
            if (derive_path_from_stem(mstem, g_bin_root, extmod_tg[0].suffix,
                                      bin_path, sizeof(bin_path)) != 0) {
                fprintf(stderr, "cc: failed to derive module output\n");
                goto parse_fail;
            }
        } else if (derive_default_bin(in_path_abs, bin_path, sizeof(bin_path)) != 0) {
            fprintf(stderr, "cc: failed to derive default binary output\n");
            goto parse_fail;
        }
    }

    CCBuildOptions opt = {
        .in_path = in_path_abs,
        .c_out_path = c_out,
        .obj_out_path = (mode == CC_MODE_EMIT_C) ? NULL : obj_path,
        .bin_out_path = (mode == CC_MODE_LINK) ? bin_path : NULL,
        .mode = mode,
        .cc_bin_override = cc_bin,
        .cc_flags = cc_flags,
        .ld_flags = ld_flags,
        .target_flag = target_flag ? target_flag : "",
        .sysroot_flag = sysroot_flag ? sysroot_flag : "",
        .opt_release = opt_release,
        .opt_debug = opt_debug,
        .no_runtime = no_runtime,
        .keep_c = keep_c,
        .verbose = verbose,
        .build_override = build_override,
        .no_build = no_build,
        .dump_consts = dump_consts,
        .dump_comptime = dump_comptime,
        .dry_run = dry_run,
        .summary = summary,
        .out_dir = g_out_root,
        .bin_dir = g_bin_root,
        .no_cache = no_cache,
        .cli_names = cli_names,
        .cli_values = cli_values,
        .cli_count = cli_count,
        .unit_kind = unit_kind,
        .ccc_version_pin = version_pin[0] ? version_pin : NULL,
    };
    CCBuildSummary sum;
    cc__prof_span("pre_compile", t_build);
    int compile_err = compile_with_build(&opt, &sum);
    print_build_summary(&opt, &sum, step == CC_BUILD_STEP_RUN ? "run" : "default");
    for (size_t i = 0; i < cli_count; ++i) {
        free(cli_names[i]);
    }
    if (compile_err != 0) return compile_err;

    /* Dual-target / multiclass module: every declared entry is compiled
     * into the one shared object, so secondary targets — and, on a
     * wildcard target, every export past the first — are the same file
     * under their own names: hardlinks (copy when the filesystem
     * refuses).  `import counter; import stats` both resolve because
     * stats.abi3.so exists and its PyInit_stats is inside.  `-o` names
     * exactly one artifact and skips siblings; --module narrows. */
    if (mode == CC_MODE_LINK && !user_out && extmod_nt > 0) {
        int e;
        for (e = 0; e < extmod_nt; e++) {
            char sib[PATH_MAX];
            char sstem[128];
            int x;
            if (e > 0) {
                if (extmod_tg[e].name[0])
                    snprintf(sstem, sizeof(sstem), "%s", extmod_tg[e].name);
                else
                    cc__stem_from_path(in_path_abs, sstem, sizeof(sstem));
                if (derive_path_from_stem(sstem, g_bin_root,
                                          extmod_tg[e].suffix, sib,
                                          sizeof(sib)) != 0 ||
                    cc__link_or_copy(bin_path, sib) != 0) {
                    fprintf(stderr, "cc: dual-target: cannot produce %s%s\n",
                            sstem, extmod_tg[e].suffix);
                    return 1;
                }
                if (verbose) fprintf(stderr, "cc: dual-target: %s\n", sib);
            }
            for (x = 0; x < extmod_tg[e].nextra; x++) {
                /* Wildcard extras hardlink (the entry SYMBOL selects the
                 * module, path-blind).  Aggregating extras must be real
                 * copies: dlopen dedupes by inode, so a second hardlink
                 * loaded in the same process would re-run the one entry
                 * with dladdr still reporting the FIRST path — and the
                 * wrong group would register. */
                int rc2;
                if (derive_path_from_stem(extmod_tg[e].extra[x], g_bin_root,
                                          extmod_tg[e].suffix, sib,
                                          sizeof(sib)) != 0)
                    rc2 = -1;
                else if (extmod_tg[e].wildcard)
                    rc2 = cc__link_or_copy(bin_path, sib);
                else
                    rc2 = cc__copy_bytes(bin_path, sib);
                if (rc2 != 0) {
                    fprintf(stderr,
                            "cc: multiclass: cannot produce %s%s\n",
                            extmod_tg[e].extra[x], extmod_tg[e].suffix);
                    return 1;
                }
                if (verbose)
                    fprintf(stderr, "cc: multiclass: %s\n", sib);
            }
        }
    }

    if (step == CC_BUILD_STEP_RUN) {
        // Print project-specific options (if any) when requested explicitly.
        // (Zig-style: `zig build --help` shows options; we keep it minimal for now.)
        char* exec_argv[64];
        int idx = 0;
        exec_argv[idx++] = (char*)(g_run_argv0 ? g_run_argv0 : bin_path);
        for (int j = 0; j < run_argc && idx < (int)(sizeof(exec_argv) / sizeof(exec_argv[0]) - 1); ++j) {
            exec_argv[idx++] = run_argv[j];
        }
        exec_argv[idx] = NULL;
        {
            long long t_run = cc__now_ms();
            int rrc = run_exec_timeout(bin_path, exec_argv, verbose, run_timeout);
            cc__prof_span("run_exec", t_run);
            return rrc;
        }
    }
    return 0;

parse_fail:
    for (size_t i = 0; i < cli_count; ++i) {
        free(cli_names[i]);
    }
    return -1;
}

/* D3.0 seam self-test.  `ccc __eval-const "<expr>" ["<prelude>"]` prints the
 * folded value or NONCONST; `ccc __eval-const --selftest` runs assertions and
 * prints "const-eval selftest ok" on success. */
static int cc__selftest_const_eval(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[2], "--selftest") == 0) {
        struct P_local { int a; double b; }; /* host mirror of the prelude struct */
        struct { const char* prelude; const char* expr; int ok; int64_t want; } cases[] = {
            { NULL, "1 + 2 * 3", 1, 7 },
            { NULL, "sizeof(int)", 1, (int64_t)sizeof(int) },
            { NULL, "_Alignof(double)", 1, (int64_t)_Alignof(double) },
            { NULL, "(1 << 10) | 1", 1, 1025 },
            { "enum { CE_A = 5, CE_B };", "CE_B * 2", 1, 12 },
            { "struct P { int a; double b; };", "sizeof(struct P)", 1, (int64_t)sizeof(struct P_local) },
            { NULL, "some_runtime_symbol", 0, 0 },
        };
        size_t n = sizeof(cases) / sizeof(cases[0]);
        for (size_t i = 0; i < n; i++) {
            int64_t got = 0;
            int ok = cc_tcc_eval_const_expr(cases[i].prelude, cases[i].expr, &got);
            if (ok != cases[i].ok || (ok && got != cases[i].want)) {
                fprintf(stderr,
                        "const-eval selftest FAIL: case %zu expr=\"%s\" ok=%d got=%lld want(ok=%d,val=%lld)\n",
                        i, cases[i].expr, ok, (long long)got, cases[i].ok, (long long)cases[i].want);
                return 1;
            }
        }
        printf("const-eval selftest ok\n");
        return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: ccc __eval-const \"<expr>\" [\"<prelude>\"]\n");
        return 2;
    }
    {
        const char* expr = argv[2];
        const char* prelude = (argc >= 4) ? argv[3] : NULL;
        int64_t v = 0;
        if (cc_tcc_eval_const_expr(prelude, expr, &v)) {
            printf("%lld\n", (long long)v);
            return 0;
        }
        printf("NONCONST\n");
        return 0;
    }
}

/* Flags that consume the NEXT argv token as their value.  The subcommand
 * pre-scan in main() must skip these values so e.g. `-o run` is an output
 * named "run", never run mode.  Keep in sync with the option loops in
 * run_build_mode() and default mode. */
static int cc__flag_takes_value(const char* a) {
    static const char* v[] = {
        "-o", "-D", "--build-file", "--out-stem", "--out-dir", "--bin-dir",
        "--cc-bin", "--cc-flags", "--ld-flags", "--target", "--module",
        "--sysroot", "--obj-out", "--graph-out", "--timeout", "--format",
        "-e", "-E", "--save", "--save-to", "--doc",
        "--as", "--ccc-version",
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); ++i) {
        if (strcmp(a, v[i]) == 0) return 1;
    }
    return 0;
}

/* Consume --as / version= / --ccc-version. Returns 1 if consumed, 0 if not,
 * -1 on error (already diagnosed). May advance *i for a following value. */
static int cc__take_unit_flag(int argc, char** argv, int* i,
                              CCUnitKind* as_kind, char* pin, size_t pin_cap) {
    const char* a;
    const char* val;
    char err[192];
    if (!argv || !i || *i < 0 || *i >= argc) return 0;
    a = argv[*i];
    if (cc_unit_cli_is_as(a)) {
        if (strncmp(a, "--as=", 5) == 0) val = a + 5;
        else {
            if (*i + 1 >= argc) {
                fprintf(stderr, "cc: --as requires ccs, cch, or shcc\n");
                return -1;
            }
            val = argv[++(*i)];
        }
        if (cc_unit_cli_parse_as_value(val, as_kind, err, sizeof(err)) != 0) {
            fprintf(stderr, "%s\n", err);
            return -1;
        }
        return 1;
    }
    if (cc_unit_cli_is_version(a)) {
        if (strncmp(a, "version=", 8) == 0) val = a + 8;
        else if (strncmp(a, "--ccc-version=", 14) == 0) val = a + 14;
        else {
            if (*i + 1 >= argc) {
                fprintf(stderr,
                        "cc: --ccc-version requires a version or bound "
                        "(>=X, >X, <=X, <X, or both)\n");
                return -1;
            }
            val = argv[++(*i)];
        }
        if (cc_unit_cli_parse_version_value(val, pin, pin_cap, err, sizeof(err)) != 0) {
            fprintf(stderr, "%s\n", err);
            return -1;
        }
        return 1;
    }
    return 0;
}

static char* cc__read_all_stdin(size_t* out_len) {
    size_t cap = 4096, n = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        size_t got;
        if (n + 1 >= cap) {
            size_t ncap = cap * 2;
            char* nb = (char*)realloc(buf, ncap);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
            cap = ncap;
        }
        got = fread(buf + n, 1, cap - n - 1, stdin);
        n += got;
        if (got == 0) break;
    }
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

static char* cc__read_all_file(const char* path, size_t* out_len) {
    FILE* f;
    long sz;
    char* buf;
    size_t n;
    if (!path) return NULL;
    f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

static int cc__write_file_bytes(const char* path, const char* data, size_t len) {
    FILE* f;
    if (!path || !data) return -1;
    f = fopen(path, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) return -1;
    return 0;
}

/* Cwd-based repo root for toolbox resolution (markers match script pathx). */
static int cc__cwd_repo_root(char* out, size_t cap) {
    char cwd[PATH_MAX];
    if (!out || cap == 0) return -1;
    out[0] = '\0';
    if (!getcwd(cwd, sizeof(cwd))) return -1;
    return cc__search_up_for_dev_repo_root(cwd, out, cap);
}

static int cc__resolve_toolbox_path(const char* save_to, char* out, size_t cap) {
    char root[PATH_MAX];
    const char* home;
    if (!out || cap == 0) return -1;
    if (save_to && save_to[0]) {
        strncpy(out, save_to, cap - 1);
        out[cap - 1] = '\0';
        return 0;
    }
    if (cc__cwd_repo_root(root, sizeof(root)) == 0 && root[0]) {
        snprintf(out, cap, "%s/tools/toolbox.shcc", root);
        return 0;
    }
    home = getenv("HOME");
    if (!home || !home[0]) {
        fprintf(stderr, "ccc: cannot resolve toolbox path (no repo cwd, no $HOME)\n");
        return -1;
    }
    snprintf(out, cap, "%s/.ccc/toolbox.shcc", home);
    return 0;
}

static int cc__ensure_parent_dir(const char* path) {
    char dir[PATH_MAX];
    if (!path || !path[0]) return -1;
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    cc__dirname_inplace(dir);
    if (!dir[0]) return 0;
    return cc__mkdir_p(dir);
}

/*
 * Materialize an -e/-E unit as content-keyed out/.cc-build/e/<hash>.shcc and
 * return a malloc'd path. `program` is the raw text; opts apply -E/-n/-p.
 */
static char* cc__materialize_e_unit(const char* program, size_t program_len,
                                    const CCScriptOnelinerOpts* opts) {
    char* unit = NULL;
    size_t unit_len = 0;
    uint64_t h;
    char dir[PATH_MAX];
    char path[PATH_MAX];
    char* out_path;

    unit = cc_script_oneliner_lower(program, program_len, opts, &unit_len);
    if (!unit) return NULL;

    h = 1469598103934665603ULL;
    h = cc__fnv1a64_update(h, unit, unit_len);
    snprintf(dir, sizeof(dir), "%s/e", g_cache_root);
    if (cc__mkdir_p(dir) != 0) {
        free(unit);
        return NULL;
    }
    snprintf(path, sizeof(path), "%s/%016llx.shcc", dir, (unsigned long long)h);
    if (cc__install_wrap_file(path, unit, unit_len) != 0) {
        free(unit);
        return NULL;
    }
    free(unit);
    out_path = (char*)malloc(strlen(path) + 1);
    if (!out_path) return NULL;
    memcpy(out_path, path, strlen(path) + 1);
    return out_path;
}

static int cc__toolbox_save(const char* toolbox_path, const char* name,
                            const char* doc, const char* program,
                            size_t program_len,
                            const CCScriptOnelinerOpts* opts) {
    char* existing = NULL;
    size_t existing_len = 0;
    char* task = NULL;
    size_t task_len = 0;
    FILE* f;

    if (!cc_script_oneliner_is_ident(name)) {
        fprintf(stderr, "ccc: --save name must be a non-keyword C identifier\n");
        return 1;
    }
    if (file_exists(toolbox_path)) {
        existing = cc__read_all_file(toolbox_path, &existing_len);
        if (!existing) {
            fprintf(stderr, "ccc: failed to read toolbox %s\n", toolbox_path);
            return 1;
        }
        if (cc_script_oneliner_task_exists(existing, existing_len, name)) {
            fprintf(stderr, "ccc: toolbox task '%s' already exists in %s\n",
                    name, toolbox_path);
            free(existing);
            return 1;
        }
    }
    task = cc_script_oneliner_format_task(name, doc, program, program_len, opts,
                                          &task_len);
    if (!task) {
        free(existing);
        fprintf(stderr, "ccc: failed to format toolbox task\n");
        return 1;
    }
    if (cc__ensure_parent_dir(toolbox_path) != 0) {
        free(existing);
        free(task);
        fprintf(stderr, "ccc: failed to create toolbox directory\n");
        return 1;
    }
    if (!existing) {
        static const char hdr[] =
            "#!/usr/bin/env -S ./cc/bin/ccc\n"
            "/* ccc toolbox — saved -e @tasks */\n\n";
        f = fopen(toolbox_path, "wb");
        if (!f) {
            free(task);
            fprintf(stderr, "ccc: failed to create toolbox %s\n", toolbox_path);
            return 1;
        }
        if (fwrite(hdr, 1, sizeof(hdr) - 1, f) != sizeof(hdr) - 1 ||
            fwrite(task, 1, task_len, f) != task_len) {
            fclose(f);
            free(task);
            fprintf(stderr, "ccc: failed to write toolbox %s\n", toolbox_path);
            return 1;
        }
        fclose(f);
    } else {
        f = fopen(toolbox_path, "ab");
        if (!f) {
            free(existing);
            free(task);
            fprintf(stderr, "ccc: failed to append toolbox %s\n", toolbox_path);
            return 1;
        }
        if (existing_len > 0 && existing[existing_len - 1] != '\n')
            fputc('\n', f);
        fputc('\n', f);
        if (fwrite(task, 1, task_len, f) != task_len) {
            fclose(f);
            free(existing);
            free(task);
            fprintf(stderr, "ccc: failed to append toolbox %s\n", toolbox_path);
            return 1;
        }
        fclose(f);
    }
    free(existing);
    free(task);
    return 0;
}

/* Rewrite argv to `ccc build [flags…] run <unit> -- [script-args…]` and exec mode. */
static int cc__run_shcc_unit(int argc, char** argv, int unit_idx,
                             const char* unit_path, int script_args_from) {
    const char** new_argv;
    int n = 0;
    int ret;
    int room = argc + 6;
    new_argv = (const char**)malloc((size_t)room * sizeof(char*));
    if (!new_argv) {
        fprintf(stderr, "cc: out of memory\n");
        return 1;
    }
    new_argv[n++] = argv[0];
    new_argv[n++] = "build";
    for (int i = 1; i < argc; i++) {
        if (i == unit_idx) continue;
        if (script_args_from >= 0 && i >= script_args_from) continue;
        /* Drop -e PROGRAM and one-liner-only flags from the build argv. */
        if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "-E") == 0) {
            i++;
            continue;
        }
        if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "-p") == 0)
            continue;
        if (strcmp(argv[i], "--save") == 0 || strcmp(argv[i], "--save-to") == 0 ||
            strcmp(argv[i], "--doc") == 0) {
            i++;
            continue;
        }
        new_argv[n++] = argv[i];
    }
    new_argv[n++] = "run";
    new_argv[n++] = unit_path;
    if (script_args_from >= 0 && script_args_from < argc) {
        if (strcmp(argv[script_args_from], "--") != 0)
            new_argv[n++] = "--";
        for (int i = script_args_from; i < argc; i++)
            new_argv[n++] = argv[i];
    }
    new_argv[n] = NULL;
    ret = run_build_mode(n, (char**)new_argv);
    free(new_argv);
    return ret < 0 ? 1 : ret;
}

int main(int argc, char **argv) {
    long long t_main = cc__now_ms();
    cc_init_paths(argv[0]);
    cc__prof_span("init_paths", t_main);
    cc_diag_init();
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (strncmp(arg, "--show-lowered=", 15) == 0) {
            cc_diag_set_show_lowered_phase(arg + 15);
        }
    }
    if (argc < 2) {
        cc__print_version();
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argv[0]);
        return 0;
    }
    {
        int want_cflags = 0;
        int want_libs = 0;
        int i;
        for (i = 1; i < argc; i++) {
            int vf = cc__take_vendor_flag(argc, argv, &i);
            if (vf < 0) return 2;
            if (vf > 0) continue;
            if (strcmp(argv[i], "--print-cflags") == 0) want_cflags = 1;
            else if (strcmp(argv[i], "--print-libs") == 0) want_libs = 1;
        }
        if (!g_cccportable_dir) {
            const char* env = getenv("CCCPORTABLE");
            if (env && env[0]) g_cccportable_dir = env;
        }
        if (want_cflags || want_libs) {
            char vline[80];
            char perr[256];
            snprintf(vline, sizeof(vline), "ccc %s", cc__version_string());
            if (g_cccportable_dir) {
                if (cc_portable_check_tree(g_cccportable_dir, vline, perr,
                                           sizeof(perr)) != 0) {
                    fprintf(stderr, "cc: %s\n", perr);
                    return 2;
                }
                if (want_cflags) cc_portable_print_cflags(g_cccportable_dir);
                if (want_libs) cc_portable_print_libs(g_cccportable_dir);
                return 0;
            }
            if (want_cflags)
                printf("-I%s -I%s\n", g_cc_lowered_include, g_cc_include);
            if (want_libs)
                printf("-DCC_ENABLE_ASYNC %s -lpthread -lm\n", g_cc_runtime_c);
            return 0;
        }
    }
    /* Version may appear with --frontend=… ahead of it; scan once. */
    if (cc__scan_frontend_flags(argc, argv) != 0) return 2;
    {
        int vi;
        for (vi = 1; vi < argc; vi++) {
            if (cc__arg_is_version(argv[vi])) {
                cc__print_version();
                return 0;
            }
        }
    }
    /* D3.0: hidden self-test for the in-process constexpr seam.
     *   ccc __eval-const "<expr>" ["<prelude>"]   -> prints "<int64>" or "NONCONST"
     *   ccc __eval-const --selftest               -> runs built-in assertions */
    if (argc >= 2 && strcmp(argv[1], "__eval-const") == 0) {
        return cc__selftest_const_eval(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "__unit-header-parse") == 0) {
        CCUnitHeader h;
        const char* line;
        if (argc < 3) {
            fprintf(stderr, "cc: __unit-header-parse requires a line\n");
            return 2;
        }
        line = argv[2];
        memset(&h, 0, sizeof(h));
        if (cc_unit_header_parse_line(line, strlen(line), &h) != 0) return 2;
        if (h.ill_formed) {
            printf("ill_formed %s\n", h.err);
            return 1;
        }
        printf("kind=%s os_shebang=%d version=%s\n",
               cc_unit_kind_name(h.kind), h.is_os_shebang,
               h.version[0] ? h.version : "-");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "__ccc-version-match") == 0) {
        if (argc < 4) {
            fprintf(stderr, "cc: __ccc-version-match requires PIN CANDIDATE\n");
            return 2;
        }
        printf("%d\n", cc_ccc_version_matches(argv[2], argv[3]));
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "clean") == 0) {
        int all = 0;
        const char* out_dir = NULL;
        const char* bin_dir = NULL;
        for (int i = 2; i < argc; ++i) {
            if (strcmp(argv[i], "--all") == 0) { all = 1; continue; }
            if (strcmp(argv[i], "--out-dir") == 0) {
                if (i + 1 >= argc) { fprintf(stderr, "cc: clean --out-dir requires a path\n"); usage(argv[0]); return 1; }
                out_dir = argv[++i];
                continue;
            }
            if (strcmp(argv[i], "--bin-dir") == 0) {
                if (i + 1 >= argc) { fprintf(stderr, "cc: clean --bin-dir requires a path\n"); usage(argv[0]); return 1; }
                bin_dir = argv[++i];
                continue;
            }
            usage(argv[0]);
            return 1;
        }
        cc_set_out_dir(out_dir, bin_dir);
        if (cc__clean_artifacts(all) != 0) {
            fprintf(stderr, "cc: clean failed (best-effort) for out_dir=%s bin_dir=%s\n", g_out_root, g_bin_root);
            return 1;
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "portable-install") == 0) {
        char vline[80];
        char perr[256];
        if (argc < 3 || !argv[2] || !argv[2][0]) {
            fprintf(stderr, "cc: portable-install requires a directory\n");
            usage(argv[0]);
            return 1;
        }
        snprintf(vline, sizeof(vline), "ccc %s", cc__version_string());
        if (cc_portable_install(argv[2], g_cc_lowered_include, g_cc_runtime_c,
                                g_repo_root, vline, perr, sizeof(perr)) != 0) {
            fprintf(stderr, "cc: %s\n", perr);
            return 1;
        }
        printf("cccportable %s -> %s\n", cc__version_string(), argv[2]);
        return 0;
    }
    /* Subcommand detection: `build` / `run` as the FIRST POSITIONAL token,
     * not just argv[1].  `ccc --keep-c run x.ccs` must parse as run mode —
     * previously it fell into default mode's legacy `cc <input> <output>`
     * form with an input literally named "run", which then died on a
     * nonsense "multiple build.cc files" error (the absolutized fake input
     * made the root build.cc match itself twice).  Flags and their values
     * are skipped; the scan stops at the first real positional or "--".
     *
     * Shebang / direct invoke: a first positional that is a script unit
     * (OS ccc shebang, --as=shcc, or a `.shcc` suffix) implies `run`.
     * Args after the script path are program args (inserted after `--`), so
     *   #!/usr/bin/env -S ./cc/bin/ccc
     *   ./tools/foo.shcc --flag
     * becomes `ccc build run ./tools/foo.shcc -- --flag`.
     *
     * One-liners: `-e` / `-E` materialize a content-keyed .shcc unit
     * (token-gated predecls, optional -n/-p) and take the same run path.
     * `ccc @name` with no unit path dispatches against the toolbox. */
    {
        int sub_idx = 0;
        int script_idx = 0;
        int e_idx = -1;
        int e_is_expr = 0; /* -E */
        int flag_n = 0;
        int flag_p = 0;
        int toolbox_idx = -1;
        const char* save_name = NULL;
        const char* save_to = NULL;
        const char* save_doc = NULL;
        CCScriptOnelinerOpts ol_opts;
        CCUnitKind pre_as = CC_UNIT_KIND_UNKNOWN;
        char pre_pin[CC_CCC_VERSION_PIN_CAP];
        pre_pin[0] = '\0';

        /* Collect one-liner mode flags anywhere (before or after PROGRAM). */
        for (int i = 1; i < argc; ++i) {
            const char* a = argv[i];
            int tf;
            if (strcmp(a, "--") == 0) break;
            tf = cc__take_unit_flag(argc, argv, &i, &pre_as, pre_pin,
                                    sizeof(pre_pin));
            if (tf < 0) return 1;
            if (tf > 0) continue;
            if (strcmp(a, "-e") == 0 || strcmp(a, "-E") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "ccc: %s requires PROGRAM or -\n", a);
                    return 1;
                }
                if (e_idx >= 0) {
                    fprintf(stderr, "ccc: -e and -E are mutually exclusive\n");
                    return 1;
                }
                e_idx = i;
                e_is_expr = (strcmp(a, "-E") == 0);
                i++; /* skip PROGRAM */
                continue;
            }
            if (strcmp(a, "-n") == 0) { flag_n = 1; continue; }
            if (strcmp(a, "-p") == 0) { flag_p = 1; continue; }
            if (strcmp(a, "--save") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "ccc: --save requires NAME\n");
                    return 1;
                }
                save_name = argv[++i];
                continue;
            }
            if (strcmp(a, "--save-to") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "ccc: --save-to requires PATH\n");
                    return 1;
                }
                save_to = argv[++i];
                continue;
            }
            if (strcmp(a, "--doc") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "ccc: --doc requires TEXT\n");
                    return 1;
                }
                save_doc = argv[++i];
                continue;
            }
            if (a[0] == '-') {
                if (cc__flag_takes_value(a)) i++;
                continue;
            }
            /* First positional after flags (and after -e PROGRAM if any). */
            if (e_idx < 0) {
                if (strcmp(a, "build") == 0 || strcmp(a, "run") == 0) sub_idx = i;
                else if (a[0] == '@') toolbox_idx = i;
                else {
                    CCUnitKind k = CC_UNIT_KIND_UNKNOWN;
                    char pin[CC_CCC_VERSION_PIN_CAP];
                    char uerr[256];
                    pin[0] = '\0';
                    if (pre_as == CC_UNIT_KIND_SHCC) script_idx = i;
                    else if (cc_unit_resolve(a, pre_as, pre_pin, &k, pin, uerr,
                                             sizeof(uerr)) == 0 &&
                             k == CC_UNIT_KIND_SHCC)
                        script_idx = i;
                    else if (cc__ends_with(a, ".shcc")) script_idx = i;
                }
                break;
            }
            /* Script args follow -e PROGRAM; stop positional classify. */
            break;
        }
        if ((flag_n || flag_p) && e_idx < 0) {
            fprintf(stderr, "ccc: -n/-p require -e or -E\n");
            return 1;
        }
        if (flag_p && e_is_expr) {
            fprintf(stderr, "ccc: -p cannot be combined with -E\n");
            return 1;
        }
        if (flag_p) flag_n = 1;

        if (e_idx >= 0) {
            const char* prog_arg = argv[e_idx + 1];
            char* program = NULL;
            size_t program_len = 0;
            char* unit_path = NULL;
            int script_from = e_idx + 2;
            int ret;

            /* Skip trailing one-liner flags that may follow PROGRAM. */
            while (script_from < argc) {
                const char* sa = argv[script_from];
                if (strcmp(sa, "-n") == 0 || strcmp(sa, "-p") == 0) {
                    script_from++;
                    continue;
                }
                if (strcmp(sa, "--save") == 0 || strcmp(sa, "--save-to") == 0 ||
                    strcmp(sa, "--doc") == 0) {
                    script_from += (script_from + 1 < argc) ? 2 : 1;
                    continue;
                }
                break;
            }

            memset(&ol_opts, 0, sizeof(ol_opts));
            ol_opts.expr_print = e_is_expr;
            ol_opts.line_loop = flag_n;
            ol_opts.line_print = flag_p;

            if (strcmp(prog_arg, "-") == 0) {
                program = cc__read_all_stdin(&program_len);
                if (!program) {
                    fprintf(stderr, "ccc: failed to read program from stdin\n");
                    return 1;
                }
            } else {
                program_len = strlen(prog_arg);
                program = (char*)malloc(program_len + 1);
                if (!program) {
                    fprintf(stderr, "cc: out of memory\n");
                    return 1;
                }
                memcpy(program, prog_arg, program_len + 1);
            }

            if (save_name) {
                char toolbox[PATH_MAX];
                if (cc__resolve_toolbox_path(save_to, toolbox, sizeof(toolbox)) != 0) {
                    free(program);
                    return 1;
                }
                ret = cc__toolbox_save(toolbox, save_name, save_doc, program,
                                       program_len, &ol_opts);
                if (ret != 0) {
                    free(program);
                    return ret;
                }
                fprintf(stderr, "ccc: saved @%s to %s\n", save_name, toolbox);
            }

            unit_path = cc__materialize_e_unit(program, program_len, &ol_opts);
            free(program);
            if (!unit_path) {
                fprintf(stderr, "ccc: failed to materialize -e unit\n");
                return 1;
            }
            /* Synthetic unit name visible to the program as argv[0]. */
            g_run_argv0 = e_is_expr ? "-E" : "-e";
            ret = cc__run_shcc_unit(argc, argv, -1, unit_path, script_from);
            free(unit_path);
            return ret;
        }
        if (save_name) {
            fprintf(stderr, "ccc: --save requires -e or -E PROGRAM\n");
            return 1;
        }
        if (toolbox_idx > 0) {
            char toolbox[PATH_MAX];
            if (cc__resolve_toolbox_path(save_to, toolbox, sizeof(toolbox)) != 0)
                return 1;
            if (!file_exists(toolbox)) {
                fprintf(stderr, "ccc: toolbox not found: %s\n", toolbox);
                return 1;
            }
            return cc__run_shcc_unit(argc, argv, toolbox_idx, toolbox,
                                     toolbox_idx);
        }
        if (sub_idx > 0) {
            /* Normalize to `ccc build [flags...] [run] ...`: run_build_mode
             * accepts the step name after options. */
            const char** new_argv = (const char**)malloc((size_t)(argc + 2) * sizeof(char*));
            if (!new_argv) { fprintf(stderr, "cc: out of memory\n"); return 1; }
            int n = 0;
            new_argv[n++] = argv[0];
            new_argv[n++] = "build";
            for (int i = 1; i < argc; i++) {
                if (i == sub_idx && strcmp(argv[i], "build") == 0) continue; /* inserted above */
                new_argv[n++] = argv[i];
            }
            new_argv[n] = NULL;
            /* Preserve program exit status (e.g. .shcc @task unknown → 2).
             * Map internal negatives to 1 for shell-friendly codes. */
            int ret = run_build_mode(n, (char**)new_argv);
            free(new_argv);
            return ret < 0 ? 1 : ret;
        }
        if (script_idx > 0) {
            /* room: + "build" + "run" + optional "--" */
            const char** new_argv = (const char**)malloc((size_t)(argc + 4) * sizeof(char*));
            if (!new_argv) { fprintf(stderr, "cc: out of memory\n"); return 1; }
            int n = 0;
            new_argv[n++] = argv[0];
            new_argv[n++] = "build";
            for (int i = 1; i < script_idx; i++)
                new_argv[n++] = argv[i];
            new_argv[n++] = "run";
            new_argv[n++] = argv[script_idx];
            if (script_idx + 1 < argc) {
                if (strcmp(argv[script_idx + 1], "--") != 0)
                    new_argv[n++] = "--";
                for (int i = script_idx + 1; i < argc; i++)
                    new_argv[n++] = argv[i];
            }
            new_argv[n] = NULL;
            int ret = run_build_mode(n, (char**)new_argv);
            free(new_argv);
            return ret < 0 ? 1 : ret;
        }
    }

    // Default mode: cc [options] <inputs...> [-o out/bin/<stem>] [--obj-out ...]
    enum { max_pos = 64 };
    const char* pos_args[max_pos];
    int pos_count = 0;
    const char* user_out = NULL;
    int saw_o = 0;
    const char* obj_out = NULL;
    const char* build_override = NULL;
    const char* out_stem_override = NULL;
    const char* cc_bin = NULL;
    const char* cc_flags = NULL;
    const char* ld_flags = NULL;
    const char* target_flag = NULL;
    const char* sysroot_flag = NULL;
    const char* out_dir = NULL;
    const char* bin_dir = NULL;
    int opt_release = 0;
    int opt_debug = 0;
    int no_build = 0;
    int no_runtime = 0;
    int dump_consts = 0;
    int dry_run = 0;
    int keep_c = 1;
    int verbose = 0;
    int no_cache = 0;
    int dump_comptime = 0;
    CCMode mode = CC_MODE_LINK;
    CCUnitKind unit_kind = CC_UNIT_KIND_UNKNOWN;
    char version_pin[CC_CCC_VERSION_PIN_CAP];
    char out_stem_buf[128];
    enum { max_cli_main = 64 };
    char* cli_names_main[max_cli_main];
    long long cli_values_main[max_cli_main];
    size_t cli_count_main = 0;
    out_stem_buf[0] = '\0';
    version_pin[0] = '\0';

    for (int i = 1; i < argc; ++i) {
        {
            int vf = cc__take_vendor_flag(argc, argv, &i);
            if (vf < 0) return 1;
            if (vf > 0) continue;
        }
        {
            int tf = cc__take_unit_flag(argc, argv, &i, &unit_kind, version_pin,
                                        sizeof(version_pin));
            if (tf < 0) return 1;
            if (tf > 0) continue;
        }
        if (strcmp(argv[i], "--emit-c-only") == 0) { mode = CC_MODE_EMIT_C; continue; }
        if (strcmp(argv[i], "--emit-c-inspect") == 0) { g_emit_c_inspect = 1; continue; }
        if (strncmp(argv[i], "--emit-c-inspect=", 17) == 0) { g_emit_c_inspect = 1; g_emit_c_inspect_path = argv[i] + 17; continue; }
        if (strcmp(argv[i], "--compile") == 0) { mode = CC_MODE_COMPILE; continue; }
        if (strcmp(argv[i], "--link") == 0) { mode = CC_MODE_LINK; continue; }
        if (strcmp(argv[i], "--release") == 0 || strcmp(argv[i], "-O") == 0) { opt_release = 1; continue; }
        if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-g") == 0) { opt_debug = 1; continue; }
        if (strcmp(argv[i], "-D") == 0) {
            fprintf(stderr, "cc: -D requires NAME or NAME=VALUE\n");
            return 1;
        }
        if (strncmp(argv[i], "-D", 2) == 0) {
            if (cli_count_main >= (size_t)max_cli_main) {
                fprintf(stderr, "cc: too many -D defines (max %d)\n", max_cli_main);
                return 1;
            }
            if (parse_define(argv[i] + 2, &cli_names_main[cli_count_main],
                             &cli_values_main[cli_count_main]) != 0) {
                return 1;
            }
            cli_count_main++;
            continue;
        }
        if (strcmp(argv[i], "--build-file") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --build-file requires a path\n"); usage(argv[0]); return 1; }
            build_override = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--out-stem") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --out-stem requires a value\n"); usage(argv[0]); return 1; }
            char tmp_stem[128];
            if (cc_build_make_stem(tmp_stem, sizeof(tmp_stem), argv[++i]) != 0) {
                fprintf(stderr, "cc: --out-stem value is too long\n");
                return 1;
            }
            strncpy(out_stem_buf, tmp_stem, sizeof(out_stem_buf));
            out_stem_buf[sizeof(out_stem_buf) - 1] = '\0';
            out_stem_override = out_stem_buf;
            continue;
        }
        if (strcmp(argv[i], "--no-build") == 0) { no_build = 1; continue; }
        if (strcmp(argv[i], "--dump-consts") == 0) { dump_consts = 1; continue; }
        if (strcmp(argv[i], "--dump-comptime") == 0) { dump_comptime = 1; dump_consts = 1; continue; }
        if (strcmp(argv[i], "--dry-run") == 0) { dry_run = 1; continue; }
        if (strcmp(argv[i], "--no-runtime") == 0) { no_runtime = 1; continue; }
        if (strcmp(argv[i], "--keep-c") == 0) { keep_c = 1; continue; }
        if (strcmp(argv[i], "--verbose") == 0) { verbose = 1; continue; }
        if (strcmp(argv[i], "--no-cache") == 0) { no_cache = 1; continue; }
        if (strcmp(argv[i], "--frontend") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "cc: --frontend requires native\n");
                usage(argv[0]);
                return 1;
            }
            ++i;
            if (cc__set_frontend_name(argv[i]) != 0) return 1;
            continue;
        }
        if (strncmp(argv[i], "--frontend=", 11) == 0) {
            const char* v = argv[i] + 11;
            if (cc__set_frontend_name(v) != 0) return 1;
            continue;
        }
        if (strcmp(argv[i], "--out-dir") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --out-dir requires a path\n"); usage(argv[0]); return 1; }
            out_dir = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--bin-dir") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --bin-dir requires a path\n"); usage(argv[0]); return 1; }
            bin_dir = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--cc-bin") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --cc-bin requires a path\n"); usage(argv[0]); return 1; }
            cc_bin = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--cc-flags") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --cc-flags requires a value\n"); usage(argv[0]); return 1; }
            cc_flags = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--ld-flags") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --ld-flags requires a value\n"); usage(argv[0]); return 1; }
            ld_flags = argv[++i];
            continue;
        }
        /* Host-cc style flags: never treat as positional inputs. */
        if (strcmp(argv[i], "-I") == 0 ||
            (strncmp(argv[i], "-I", 2) == 0 && argv[i][2] != 0) ||
            strcmp(argv[i], "-fPIC") == 0 || strcmp(argv[i], "-fpic") == 0) {
            fprintf(stderr,
                    "cc: '%s' is not a positional input; pass host flags via "
                    "--cc-flags (e.g. --cc-flags=\"-Ipath -fPIC\")\n",
                    argv[i]);
            return 1;
        }
        if (strcmp(argv[i], "--target") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --target requires a value\n"); usage(argv[0]); return 1; }
            target_flag = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--sysroot") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --sysroot requires a path\n"); usage(argv[0]); return 1; }
            sysroot_flag = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--obj-out") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --obj-out requires a path\n"); usage(argv[0]); return 1; }
            obj_out = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: -o requires a path\n"); usage(argv[0]); return 1; }
            user_out = argv[++i];
            saw_o = 1;
            continue;
        }
        // Positional input.
        if (pos_count >= max_pos) { fprintf(stderr, "cc: too many input files (max %d)\n", max_pos); return 1; }
        pos_args[pos_count++] = argv[i];
    }

    if (g_cccportable_cli) {
        fprintf(stderr,
                "cc: --cccportable only applies to --print-cflags / --print-libs "
                "(it does not remap lowerer faces)\n");
        return 2;
    }

    // If both are provided, debug wins (safe default).
    if (opt_release && opt_debug) opt_release = 0;

    if (pos_count == 0) {
        /* Flags-only (e.g. already-handled --version) should not reach here;
         * bare `ccc` is handled at startup. Remaining empty positionals → help. */
        usage(argv[0]);
        return 1;
    }

    // Flavor defaults (non-build mode): apply before any user-provided
    // --cc-flags so users can override.  Unflagged builds get -O2 with
    // asserts kept; -O/--release still selects -O2 -DNDEBUG.
    const char* flavor_cc = CC_DEFAULT_FLAVOR_CC;
    if (opt_debug) flavor_cc = "-O0 -g";
    else if (opt_release) flavor_cc = "-O2 -DNDEBUG";
    static char combined_cc_flags_main[2048];
    combined_cc_flags_main[0] = '\0';
    if (flavor_cc && flavor_cc[0]) strncat(combined_cc_flags_main, flavor_cc, sizeof(combined_cc_flags_main) - 1);
    if (cc_flags && cc_flags[0]) {
        if (combined_cc_flags_main[0]) strncat(combined_cc_flags_main, " ", sizeof(combined_cc_flags_main) - strlen(combined_cc_flags_main) - 1);
        strncat(combined_cc_flags_main, cc_flags, sizeof(combined_cc_flags_main) - strlen(combined_cc_flags_main) - 1);
    }
    for (size_t i = 0; i < cli_count_main; ++i) {
        char def[256];
        if (cli_values_main[i] == 1) {
            snprintf(def, sizeof(def), " -D%s", cli_names_main[i]);
        } else {
            snprintf(def, sizeof(def), " -D%s=%lld", cli_names_main[i],
                     cli_values_main[i]);
        }
        strncat(combined_cc_flags_main, def,
                sizeof(combined_cc_flags_main) - strlen(combined_cc_flags_main) - 1);
    }
    cc_flags = combined_cc_flags_main[0] ? combined_cc_flags_main : cc_flags;

    cc_set_out_dir(out_dir, bin_dir);
    if (!out_dir && !bin_dir && !user_out && pos_count > 0 &&
        cc__input_is_shcc(unit_kind, pos_args[0]))
        (void)cc__use_script_cache_dirs();
    cc_refresh_host_obj_root(cc_bin);
    if (ensure_out_dir() != 0) {
        fprintf(stderr, "cc: failed to create out dirs under: %s\n", g_out_root);
        for (size_t i = 0; i < cli_count_main; ++i) free(cli_names_main[i]);
        return 1;
    }

    // Interpret legacy `cc <in> <out>` only when -o was not used and exactly 2 positionals.
    const char* inputs[max_pos];
    int input_count = 0;
    const char* legacy_out = NULL;
    if (!saw_o && pos_count == 2) {
        inputs[0] = pos_args[0];
        input_count = 1;
        legacy_out = pos_args[1];
    } else {
        for (int i = 0; i < pos_count; ++i) inputs[i] = pos_args[i];
        input_count = pos_count;
    }
    if (!user_out && legacy_out) user_out = legacy_out;

    if (input_count > 1) {
        if (mode == CC_MODE_EMIT_C && saw_o) {
            fprintf(stderr, "cc: -o with multiple inputs in --emit-c-only mode is not supported\n");
            return 1;
        }
        if (mode == CC_MODE_COMPILE && obj_out) {
            fprintf(stderr, "cc: --obj-out with multiple inputs is not supported\n");
            return 1;
        }
        if (mode == CC_MODE_LINK && !user_out) {
            fprintf(stderr, "cc: linking multiple inputs requires -o <output>\n");
            return 1;
        }
        if (mode == CC_MODE_LINK &&
            cc_check_link_set_faces(inputs, input_count) != 0)
            return 1;

        CCConstBinding bindings[128];
        size_t binding_count = 0;
        CCBuildOptions base_opt = {
            .in_path = inputs[0],
            .cc_bin_override = cc_bin,
            .cc_flags = cc_flags,
            .ld_flags = ld_flags,
            .target_flag = target_flag ? target_flag : "",
            .sysroot_flag = sysroot_flag ? sysroot_flag : "",
            .opt_release = opt_release,
            .opt_debug = opt_debug,
            .no_runtime = no_runtime,
            .keep_c = keep_c,
            .verbose = verbose,
            .build_override = build_override,
            .no_build = no_build,
            .dump_consts = dump_consts,
            .dump_comptime = dump_comptime,
            .dry_run = dry_run,
            .summary = 0,
            .out_dir = g_out_root,
            .bin_dir = g_bin_root,
            .cli_names = cli_names_main,
            .cli_values = cli_values_main,
            .cli_count = cli_count_main,
            .unit_kind = unit_kind,
            .ccc_version_pin = version_pin[0] ? version_pin : NULL,
        };
        int berr = cc__load_const_bindings(&base_opt, bindings, &binding_count);
        if (berr != 0) {
            for (size_t i = 0; i < cli_count_main; ++i) free(cli_names_main[i]);
            return 1;
        }
        if (dump_consts) {
            for (size_t i = 0; i < binding_count; ++i) {
                printf("CONST %s=%lld\n", bindings[i].name, bindings[i].value);
            }
        }
        if (dry_run) {
            for (size_t i = 0; i < cli_count_main; ++i) free(cli_names_main[i]);
            return 0;
        }
        CCCompileConfig cfg = {.consts = bindings, .const_count = binding_count};

        char target_part[256]; char sysroot_part[256];
        target_part[0] = '\0'; sysroot_part[0] = '\0';
        if (target_flag && *target_flag) snprintf(target_part, sizeof(target_part), "--target %s", target_flag);
        if (sysroot_flag && *sysroot_flag) snprintf(sysroot_part, sizeof(sysroot_part), "--sysroot %s", sysroot_flag);

        char resolved_stems[64][128] = {{0}};
        if (cc__resolve_stems(inputs, input_count, out_stem_override, resolved_stems) != 0) {
            fprintf(stderr, "cc: failed to derive unique stems for inputs\n");
            for (size_t i = 0; i < cli_count_main; ++i) free(cli_names_main[i]);
            return 1;
        }
        const char* obj_paths[64];
        char obj_bufs[64][PATH_MAX];
        char c_bufs[64][PATH_MAX];
        char dep_bufs[64][PATH_MAX];
        char src_dir_bufs[64][PATH_MAX];
        for (int i = 0; i < input_count; ++i) {
            const char* stem = resolved_stems[i];
            if (!stem || !stem[0]) {
                fprintf(stderr, "cc: invalid stem for input %s\n", inputs[i]);
                return 1;
            }
            cc__derive_c_path_from_stem(stem, c_bufs[i], sizeof(c_bufs[i]));
            cc__derive_o_path_from_stem(stem, obj_bufs[i], sizeof(obj_bufs[i]));
            cc__derive_d_path_from_stem(stem, dep_bufs[i], sizeof(dep_bufs[i]));
            cc__dir_of_path(inputs[i], src_dir_bufs[i], sizeof(src_dir_bufs[i]));

            int is_raw_c = cc__is_raw_c(inputs[i]);
            if (is_raw_c) {
                if (cc__materialize_host_c(inputs[i], c_bufs[i]) != 0) {
                    fprintf(stderr, "cc: failed to materialize host C %s -> %s\n",
                            inputs[i], c_bufs[i]);
                    return 1;
                }
            }
            const char* c_for_compile = c_bufs[i];
            if (mode == CC_MODE_EMIT_C) {
                if (!is_raw_c) {
                    int err = cc__compile_with_env(NULL, inputs[i], c_bufs[i], &cfg);
                    if (err != 0) return 1;
                }
                continue;
            }
            if (!is_raw_c) {
                int err = cc__compile_with_env(NULL, inputs[i], c_bufs[i], &cfg);
                if (err != 0) return 1;
            }
            if (mode != CC_MODE_EMIT_C) {
                if (cc__compile_c_to_obj(&base_opt, c_for_compile, obj_bufs[i], dep_bufs[i], src_dir_bufs[i], target_part, sysroot_part) != 0) return 1;
                obj_paths[i] = obj_bufs[i];
            }
        }
        if (mode == CC_MODE_EMIT_C || mode == CC_MODE_COMPILE) {
            for (size_t i = 0; i < cli_count_main; ++i) free(cli_names_main[i]);
            return 0;
        }
        char runtime_path[PATH_MAX]; int runtime_reused = 0;
        if (cc__ensure_runtime_obj(&base_opt, target_part, sysroot_part, runtime_path, sizeof(runtime_path), &runtime_reused) != 0) {
            for (size_t i = 0; i < cli_count_main; ++i) free(cli_names_main[i]);
            return 1;
        }
        if (cc__link_many(&base_opt, obj_paths, (size_t)input_count, runtime_path, target_part, sysroot_part, user_out) != 0) {
            for (size_t i = 0; i < cli_count_main; ++i) free(cli_names_main[i]);
            return 1;
        }
        for (size_t i = 0; i < cli_count_main; ++i) free(cli_names_main[i]);
        return 0;
    }

    const char* in_path = inputs[0];

    char c_out[512];
    char obj_path[512];
    char bin_path[512];

    // Normalize input path to an absolute path so debug info (via #line) matches editor paths.
    // This matters for LLDB breakpoint binding in VS Code / Cursor.
    char in_abs[PATH_MAX];
    const char* in_path_abs = in_path;
    if (in_path && in_path[0]) {
        if (realpath(in_path, in_abs) != NULL) {
            in_path_abs = in_abs;
        } else if (!cc__is_abs_path(in_path)) {
            snprintf(in_abs, sizeof(in_abs), "%s/%s", g_repo_root, in_path);
            in_path_abs = in_abs;
        }
    }

    int raw_c = cc__is_raw_c(in_path_abs);
    int use_stem_override = out_stem_override && out_stem_override[0];
    if (mode == CC_MODE_EMIT_C) {
        if (user_out) {
            strncpy(c_out, user_out, sizeof(c_out));
            c_out[sizeof(c_out)-1] = '\0';
        } else if (use_stem_override) {
            if (derive_path_from_stem(out_stem_override, g_out_root, ".c", c_out, sizeof(c_out)) != 0) {
                fprintf(stderr, "cc: failed to derive C output from --out-stem\n");
                return 1;
            }
        } else if (derive_default_output(in_path_abs, c_out, sizeof(c_out)) != 0) {
            fprintf(stderr, "cc: failed to derive default C output\n");
            return 1;
        }
    } else {
        if (use_stem_override) {
            if (derive_path_from_stem(out_stem_override, g_out_root, ".c", c_out, sizeof(c_out)) != 0) {
                fprintf(stderr, "cc: failed to derive C output from --out-stem\n");
                return 1;
            }
        } else if (derive_default_output(in_path_abs, c_out, sizeof(c_out)) != 0) {
            fprintf(stderr, "cc: failed to derive default C output\n");
            return 1;
        }
        (void)raw_c;
    }

    if (mode == CC_MODE_COMPILE) {
        /* -o names the object; --obj-out is an explicit alternate. */
        if (obj_out) {
            strncpy(obj_path, obj_out, sizeof(obj_path));
            obj_path[sizeof(obj_path) - 1] = '\0';
        } else if (user_out) {
            strncpy(obj_path, user_out, sizeof(obj_path));
            obj_path[sizeof(obj_path) - 1] = '\0';
        } else if (derive_default_obj(in_path_abs, obj_path, sizeof(obj_path)) !=
                   0) {
            fprintf(stderr, "cc: failed to derive default object output\n");
            for (size_t i = 0; i < cli_count_main; ++i) free(cli_names_main[i]);
            return 1;
        }
    } else if (mode != CC_MODE_EMIT_C) {
        if (obj_out) {
            strncpy(obj_path, obj_out, sizeof(obj_path));
            obj_path[sizeof(obj_path)-1] = '\0';
        } else if (derive_default_obj(in_path_abs, obj_path, sizeof(obj_path)) != 0) {
            fprintf(stderr, "cc: failed to derive default object output\n");
            for (size_t i = 0; i < cli_count_main; ++i) free(cli_names_main[i]);
            return 1;
        }
    }

    if (mode == CC_MODE_LINK) {
        if (user_out) {
            strncpy(bin_path, user_out, sizeof(bin_path));
            bin_path[sizeof(bin_path)-1] = '\0';
        } else if (derive_default_bin(in_path_abs, bin_path, sizeof(bin_path)) != 0) {
            fprintf(stderr, "cc: failed to derive default binary output\n");
            for (size_t i = 0; i < cli_count_main; ++i) free(cli_names_main[i]);
            return 1;
        }
    }

    CCBuildOptions opt = {
        .in_path = in_path_abs,
        .c_out_path = c_out,
        .obj_out_path = (mode == CC_MODE_EMIT_C) ? NULL : obj_path,
        .bin_out_path = (mode == CC_MODE_LINK) ? bin_path : NULL,
        .mode = mode,
        .cc_bin_override = cc_bin,
        .cc_flags = cc_flags,
        .ld_flags = ld_flags,
        .target_flag = target_flag ? target_flag : "",
        .sysroot_flag = sysroot_flag ? sysroot_flag : "",
        .opt_release = opt_release,
        .opt_debug = opt_debug,
        .no_runtime = no_runtime,
        .keep_c = keep_c,
        .verbose = verbose,
        .build_override = build_override,
        .no_build = no_build,
        .dump_consts = dump_consts,
        .dump_comptime = dump_comptime,
        .dry_run = dry_run,
        .summary = 0,
        .out_dir = g_out_root,
        .bin_dir = g_bin_root,
        .no_cache = no_cache,
        .cli_names = cli_names_main,
        .cli_values = cli_values_main,
        .cli_count = cli_count_main,
        .unit_kind = unit_kind,
        .ccc_version_pin = version_pin[0] ? version_pin : NULL,
    };
    CCBuildSummary sum;
    int err = compile_with_build(&opt, &sum);
    for (size_t i = 0; i < cli_count_main; ++i) free(cli_names_main[i]);
    return err == 0 ? 0 : 1;
}

