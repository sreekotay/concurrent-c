#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "build/build.h"
#include "driver.h"

// Forward decls for helpers used by multiple modes.
static int file_exists(const char* path);
static int ensure_out_dir(void);

// Resolved repo-relative paths so `./cc/bin/ccc build ...` works from the repo root.
static int g_paths_inited = 0;
static char g_repo_root[PATH_MAX];
static char g_cc_dir[PATH_MAX];
static char g_cc_include[PATH_MAX];
static char g_cc_runtime_o[PATH_MAX];
static char g_cc_runtime_c[PATH_MAX];
static char g_out_root[PATH_MAX];
static char g_bin_root[PATH_MAX];
static char g_cache_root[PATH_MAX];

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
    if (out_meta && meta_cap) snprintf(out_meta, meta_cap, "%s/%s.meta", g_cache_root, stem);
    if (out_link && link_cap) snprintf(out_link, link_cap, "%s/%s.link", g_cache_root, stem);
}

static void cc_set_out_dir(const char* out_dir_opt, const char* bin_dir_opt) {
    const char* env = getenv("CC_OUT_DIR");
    const char* p = out_dir_opt && out_dir_opt[0] ? out_dir_opt : (env && env[0] ? env : NULL);

    if (!p) {
        snprintf(g_out_root, sizeof(g_out_root), "%s/out", g_repo_root);
    } else if (cc__is_abs_path(p)) {
        strncpy(g_out_root, p, sizeof(g_out_root));
        g_out_root[sizeof(g_out_root) - 1] = '\0';
    } else {
        // Relative paths are interpreted relative to repo root.
        snprintf(g_out_root, sizeof(g_out_root), "%s/%s", g_repo_root, p);
    }

    const char* benv = getenv("CC_BIN_DIR");
    const char* bp = bin_dir_opt && bin_dir_opt[0] ? bin_dir_opt : (benv && benv[0] ? benv : NULL);
    if (!bp) {
        snprintf(g_bin_root, sizeof(g_bin_root), "%s/bin", g_repo_root);
    } else if (cc__is_abs_path(bp)) {
        strncpy(g_bin_root, bp, sizeof(g_bin_root));
        g_bin_root[sizeof(g_bin_root) - 1] = '\0';
    } else {
        // Relative bin dir is interpreted relative to repo root.
        snprintf(g_bin_root, sizeof(g_bin_root), "%s/%s", g_repo_root, bp);
    }

    snprintf(g_cache_root, sizeof(g_cache_root), "%s/.cc-build", g_out_root);
}

static void cc_init_paths(const char* argv0) {
    if (g_paths_inited) return;
    g_paths_inited = 1;

    char exe_abs[PATH_MAX];
    exe_abs[0] = '\0';
    if (argv0 && argv0[0]) {
        // Best effort: if argv0 is a path (common dev case: ./cc/bin/ccc), realpath it.
        if (realpath(argv0, exe_abs) == NULL) {
            // Fallback: accept argv0 as-is.
            strncpy(exe_abs, argv0, sizeof(exe_abs));
            exe_abs[sizeof(exe_abs) - 1] = '\0';
        }
    }

    // Derive repo root from the executable location.
    // Supported layouts:
    //  - <repo>/cc/bin/ccc
    //  - <repo>/out/cc/bin/ccc
    char tmp[PATH_MAX];
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
    // Prefer the compiler-build runtime object (built by `make -C cc`) which now lives under out/.
    snprintf(g_cc_runtime_o, sizeof(g_cc_runtime_o), "%s/out/cc/obj/runtime/concurrent_c.o", g_repo_root);
    snprintf(g_cc_runtime_c, sizeof(g_cc_runtime_c), "%s/cc/runtime/concurrent_c.c", g_repo_root);
    cc_set_out_dir(NULL, NULL);
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s [options] <input.ccs> [output]\n", prog);
    fprintf(stderr, "  %s build [options] <input.ccs> <output>\n", prog);
    fprintf(stderr, "  %s build run [options] <input.ccs> [-o out/<stem>] [-- <args...>]\n", prog);
    fprintf(stderr, "Modes:\n");
    fprintf(stderr, "  --emit-c-only       Stop after emitting C (output defaults to out/<stem>.c)\n");
    fprintf(stderr, "  --compile           Emit C and compile to object (output defaults to out/<stem>.o)\n");
    fprintf(stderr, "  --link              Emit C, compile, and link (default; binary defaults to out/<stem>)\n");
    fprintf(stderr, "Build integration:\n");
    fprintf(stderr, "  -DNAME[=VALUE]      Define comptime const (VALUE defaults to 1, build mode only)\n");
    fprintf(stderr, "  --build-file PATH   Use explicit build.cc path (overrides discovery)\n");
    fprintf(stderr, "  --no-build          Disable build.cc even if present\n");
    fprintf(stderr, "  --dump-consts       Print merged const bindings then compile\n");
    fprintf(stderr, "  --dry-run           Resolve consts / show commands, skip compile/link\n");
    fprintf(stderr, "Toolchain:\n");
    fprintf(stderr, "  -o PATH             Output (mode dependent: C/object/binary)\n");
    fprintf(stderr, "  --obj-out PATH      Object output (for --link)\n");
    fprintf(stderr, "  --cc-bin PATH       C compiler (default: $CC or cc/gcc/clang)\n");
    fprintf(stderr, "  --cc-flags FLAGS    Extra compiler flags\n");
    fprintf(stderr, "  --ld-flags FLAGS    Extra linker flags\n");
    fprintf(stderr, "  --target TRIPLE     Forward target triple to C compiler\n");
    fprintf(stderr, "  --sysroot PATH      Forward sysroot to C compiler\n");
    fprintf(stderr, "  --no-runtime        Do not link runtime (default links bundled runtime)\n");
    fprintf(stderr, "  --keep-c            Do not delete generated C file\n");
    fprintf(stderr, "  --out-dir DIR       Output dir for generated C + objects (default: <repo>/out)\n");
    fprintf(stderr, "  --bin-dir DIR       Output dir for linked executables (default: <repo>/bin)\n");
    fprintf(stderr, "  --no-cache          Disable incremental cache (also: CC_NO_CACHE=1)\n");
    fprintf(stderr, "  --verbose           Print invoked commands\n");
}

static void usage_build(const char* prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s build [options] <input.ccs> [output]\n", prog);
    fprintf(stderr, "  %s build run [options] <input.ccs> [-o bin/<stem>] [-- <args...>]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Steps:\n");
    fprintf(stderr, "  (default)   Build (emit C, compile, link)\n");
    fprintf(stderr, "  run         Build then run the produced binary\n");
    fprintf(stderr, "  test        Run the repo test suite (builds tools/cc_test if needed)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options: same as main help (use `%s --help` for full list)\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Project options:\n");
    fprintf(stderr, "  build.cc may declare options using: CC_OPTION <NAME> <HELP...>\n");
}

// Tiny helper to check for file existence.
static int file_exists(const char* path) {
    if (!path) return 0;
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    fclose(f);
    return 1;
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
    return 0;
}

static int derive_default_output(const char* in_path, char* out_buf, size_t out_buf_size) {
    if (!in_path || !out_buf || out_buf_size == 0) return -1;
    const char* base = strrchr(in_path, '/');
    base = base ? base + 1 : in_path;
    size_t base_len = strlen(base);
    size_t stem_len = base_len;
    const char* dot = strrchr(base, '.');
    if (dot && dot != base) {
        stem_len = (size_t)(dot - base);
    }
    size_t dir_len = strlen(g_out_root);
    if (dir_len + 1 + stem_len + 2 > out_buf_size) { // "/" + ".c" + NUL
        return -1;
    }
    memcpy(out_buf, g_out_root, dir_len);
    out_buf[dir_len] = '/';
    memcpy(out_buf + dir_len + 1, base, stem_len);
    out_buf[dir_len + 1 + stem_len] = '\0';
    strcat(out_buf, ".c");
    return 0;
}

static int derive_default_obj(const char* in_path, char* out_buf, size_t out_buf_size) {
    if (!in_path || !out_buf || out_buf_size == 0) return -1;
    const char* base = strrchr(in_path, '/');
    base = base ? base + 1 : in_path;
    size_t base_len = strlen(base);
    size_t stem_len = base_len;
    const char* dot = strrchr(base, '.');
    if (dot && dot != base) {
        stem_len = (size_t)(dot - base);
    }
    size_t dir_len = strlen(g_out_root);
    if (dir_len + 1 + stem_len + 3 > out_buf_size) { // "/" + ".o"+NUL
        return -1;
    }
    memcpy(out_buf, g_out_root, dir_len);
    out_buf[dir_len] = '/';
    memcpy(out_buf + dir_len + 1, base, stem_len);
    out_buf[dir_len + 1 + stem_len] = '\0';
    strcat(out_buf, ".o");
    return 0;
}

static int derive_default_bin(const char* in_path, char* out_buf, size_t out_buf_size) {
    if (!in_path || !out_buf || out_buf_size == 0) return -1;
    const char* base = strrchr(in_path, '/');
    base = base ? base + 1 : in_path;
    size_t base_len = strlen(base);
    size_t stem_len = base_len;
    const char* dot = strrchr(base, '.');
    if (dot && dot != base) {
        stem_len = (size_t)(dot - base);
    }
    size_t dir_len = strlen(g_bin_root);
    if (dir_len + 1 + stem_len + 1 > out_buf_size) { // "/" + NUL
        return -1;
    }
    memcpy(out_buf, g_bin_root, dir_len);
    out_buf[dir_len] = '/';
    memcpy(out_buf + dir_len + 1, base, stem_len);
    out_buf[dir_len + 1 + stem_len] = '\0';
    return 0;
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
    int no_runtime;
    int keep_c;
    int verbose;
    const char* build_override;
    int no_build;
    int dump_consts;
    int dry_run;
    int summary;
    const char* out_dir;
    const char* bin_dir;
    int no_cache;
    char** cli_names;
    long long* cli_values;
    size_t cli_count;
} CCBuildOptions;

static int cc__compile_c_to_obj(const CCBuildOptions* opt,
                                const char* c_path,
                                const char* obj_path,
                                const char* dep_path,
                                const char* extra_include_dir,
                                const char* target_part,
                                const char* sysroot_part);

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
    snprintf(out, cap, "%s/%s.o", g_out_root, stem);
    return 0;
}

static int cc__derive_d_path_from_stem(const char* stem, char* out, size_t cap) {
    if (!stem || !stem[0] || !out || cap == 0) return -1;
    snprintf(out, cap, "%s/%s.d", g_out_root, stem);
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
        } else if (st_dep.st_mtime > st_obj.st_mtime) {
            rebuild = 1;
        }

        *p = saved;
        if (rebuild) break;
    }

    free(text);
    return rebuild;
}

static int cc__load_const_bindings(const CCBuildOptions* opt, CCConstBinding* bindings, size_t* count);

// Core compile helper shared by default and build modes.
static int compile_with_build(const CCBuildOptions* opt, CCBuildSummary* summary_out) {
    if (!opt || !opt->in_path || !opt->c_out_path) {
        fprintf(stderr, "cc: missing input or c_out_path\n");
        return -1;
    }
    if (summary_out) {
        memset(summary_out, 0, sizeof(*summary_out));
        summary_out->c_out_path = opt->c_out_path;
        summary_out->obj_out_path = opt->obj_out_path;
        summary_out->bin_out_path = opt->bin_out_path;
    }
    const int max_bindings = 128;
    CCConstBinding bindings[max_bindings];
    size_t count = 0;
    int load_err = cc__load_const_bindings(opt, bindings, &count);
    if (load_err != 0) return load_err;

    CCCompileConfig cfg = {
        .consts = bindings,
        .const_count = count
    };
    if (opt->dump_consts) {
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
            // If user requested an output path, copy; otherwise do nothing.
            if (opt->c_out_path && strcmp(opt->c_out_path, opt->in_path) != 0) {
                if (cc__copy_file(opt->in_path, opt->c_out_path) != 0) {
                    fprintf(stderr, "cc: failed to copy %s -> %s\n", opt->in_path, opt->c_out_path);
                    return -1;
                }
                if (summary_out) {
                    summary_out->reuse_emit_c = 0;
                    summary_out->did_emit_c = 1;
                }
            }
            return 0;
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
        CCFileSig in_sig, build_sig, cc_sig;
        (void)cc__stat_sig(opt->in_path, &in_sig);
        char build_buf[512];
        int multiple = 0;
        const char* build_path = opt->build_override ? opt->build_override : choose_build_path(opt->in_path, build_buf, sizeof(build_buf), &multiple);
        if (multiple) build_path = NULL;
        if (build_path && cc__stat_sig(build_path, &build_sig) != 0) { build_sig.mtime_sec = 0; build_sig.size = 0; }
        if (cc__stat_sig(opt->cc_bin_override ? opt->cc_bin_override : "cc", &cc_sig) != 0) { cc_sig.mtime_sec = 0; cc_sig.size = 0; }

        uint64_t h = 1469598103934665603ULL;
        h = cc__fnv1a64_str(h, opt->in_path);
        h = cc__fnv1a64_i64(h, in_sig.mtime_sec);
        h = cc__fnv1a64_i64(h, in_sig.size);
        h = cc__fnv1a64_str(h, build_path ? build_path : "");
        h = cc__fnv1a64_i64(h, build_sig.mtime_sec);
        h = cc__fnv1a64_i64(h, build_sig.size);
        h = cc__fnv1a64_i64(h, cc_sig.mtime_sec);
        h = cc__fnv1a64_i64(h, cc_sig.size);
        h = cc__fnv1a64_str(h, opt->target_flag);
        h = cc__fnv1a64_str(h, opt->sysroot_flag);
        h = cc__fnv1a64_str(h, opt->cc_flags);
        h = cc__fnv1a64_str(h, getenv("CFLAGS"));
        h = cc__fnv1a64_str(h, getenv("CPPFLAGS"));
        h = cc__fnv1a64_i64(h, (long long)opt->no_build);
        h = cc__fnv1a64_i64(h, (long long)opt->cli_count);
        for (size_t i = 0; i < opt->cli_count; ++i) {
            h = cc__fnv1a64_str(h, opt->cli_names[i]);
            h = cc__fnv1a64_i64(h, opt->cli_values[i]);
        }
        emit_key = h;

        uint64_t prev = 0;
        if (file_exists(opt->c_out_path) && cc__read_u64_file(meta_path, &prev) == 0 && prev == emit_key) {
            if (summary_out) { summary_out->reuse_emit_c = 1; summary_out->did_emit_c = 0; }
            // skip emit
        } else {
            int err = cc_compile_with_config(opt->in_path, opt->c_out_path, &cfg);
            if (err != 0) return err;
            (void)cc__write_u64_file(meta_path, emit_key);
            if (summary_out) { summary_out->reuse_emit_c = 0; summary_out->did_emit_c = 1; }
        }
    } else if (!is_raw_c) {
        int err = cc_compile_with_config(opt->in_path, opt->c_out_path, &cfg);
        if (err != 0) return err;
        if (summary_out) { summary_out->reuse_emit_c = 0; summary_out->did_emit_c = 1; }
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
    const char* cppflags_env = getenv("CPPFLAGS");
    char cmd[2048];

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
    snprintf(obj_meta_path, sizeof(obj_meta_path), "%s/%s.obj", g_cache_root, stem);
    uint64_t obj_key = 0;
    char dep_path[PATH_MAX];
    cc__derive_d_path_from_stem(stem, dep_path, sizeof(dep_path));
    char src_dir[PATH_MAX];
    cc__dir_of_path(opt->in_path, src_dir, sizeof(src_dir));
    const char* c_for_compile = is_raw_c ? opt->in_path : opt->c_out_path;
    if (cache_ok) {
        uint64_t h = 1469598103934665603ULL;
        if (is_raw_c) {
            CCFileSig in_sig;
            (void)cc__stat_sig(opt->in_path, &in_sig);
            h = cc__fnv1a64_str(h, opt->in_path);
            h = cc__fnv1a64_i64(h, in_sig.mtime_sec);
            h = cc__fnv1a64_i64(h, in_sig.size);
        } else {
            h = cc__fnv1a64_i64(h, (long long)emit_key);
        }
        h = cc__fnv1a64_str(h, target_part);
        h = cc__fnv1a64_str(h, sysroot_part);
        h = cc__fnv1a64_str(h, opt->cc_flags);
        h = cc__fnv1a64_str(h, getenv("CFLAGS"));
        h = cc__fnv1a64_str(h, getenv("CPPFLAGS"));
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
    // Link to binary (with incremental cache)
    const char* ldflags_env = getenv("LDFLAGS");
    char link_cmd[2048];
    // Optional runtime object
    char runtime_obj[256];
    int have_runtime = 0;
    if (!opt->no_runtime) {
        // Prefer prebuilt runtime object from the compiler build.
        if (file_exists(g_cc_runtime_o)) {
            strncpy(runtime_obj, g_cc_runtime_o, sizeof(runtime_obj));
            runtime_obj[sizeof(runtime_obj) - 1] = '\0';
            if (summary_out) {
                summary_out->runtime_reused = 1;
                summary_out->runtime_obj_path = g_cc_runtime_o;
            }
        } else {
            snprintf(runtime_obj, sizeof(runtime_obj), "%s/runtime.o", g_out_root);
            snprintf(cmd, sizeof(cmd), "%s %s %s %s %s -I%s -I%s -I%s -c %s -o %s",
                     cc_bin,
                     ccflags_env ? ccflags_env : "",
                     cppflags_env ? cppflags_env : "",
                     target_part,
                     sysroot_part,
                     g_cc_include,
                     g_cc_dir,
                     g_repo_root,
                     g_cc_runtime_c,
                     runtime_obj);
            if (opt->cc_flags && *opt->cc_flags) {
                strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
                strncat(cmd, opt->cc_flags, sizeof(cmd) - strlen(cmd) - 1);
            }
            if (run_cmd(cmd, opt->verbose) != 0) {
                return -1;
            }
            if (summary_out) {
                summary_out->runtime_reused = 0;
                summary_out->runtime_obj_path = "out/obj/runtime.o";
            }
        }
        have_runtime = 1;
    }

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
        h = cc__fnv1a64_str(h, ldflags_env);
        h = cc__fnv1a64_str(h, opt->ld_flags);
        h = cc__fnv1a64_str(h, target_part);
        h = cc__fnv1a64_str(h, sysroot_part);
        link_key = h;
        uint64_t prev = 0;
        if (file_exists(opt->bin_out_path) && cc__read_u64_file(link_meta_path, &prev) == 0 && prev == link_key) {
            if (summary_out) { summary_out->reuse_link = 1; summary_out->did_link = 0; }
        } else {
            snprintf(link_cmd, sizeof(link_cmd), "%s %s %s %s %s %s %s -o %s",
                     cc_bin,
                     target_part,
                     sysroot_part,
                     ldflags_env ? ldflags_env : "",
                     opt->ld_flags ? opt->ld_flags : "",
                     opt->obj_out_path,
                     have_runtime ? runtime_obj : "",
                     opt->bin_out_path);
            if (run_cmd(link_cmd, opt->verbose) != 0) return -1;
            (void)cc__write_u64_file(link_meta_path, link_key);
            if (summary_out) { summary_out->reuse_link = 0; summary_out->did_link = 1; }
        }
    } else {
        snprintf(link_cmd, sizeof(link_cmd), "%s %s %s %s %s %s %s -o %s",
                 cc_bin,
                 target_part,
                 sysroot_part,
                 ldflags_env ? ldflags_env : "",
                 opt->ld_flags ? opt->ld_flags : "",
                 opt->obj_out_path,
                 have_runtime ? runtime_obj : "",
                 opt->bin_out_path);
        if (run_cmd(link_cmd, opt->verbose) != 0) return -1;
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
    return 0;
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
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s %s %s %s %s -MMD -MF %s -MT %s -I%s -I%s -I%s",
             cc_bin,
             ccflags_env ? ccflags_env : "",
             cppflags_env ? cppflags_env : "",
             target_part ? target_part : "",
             sysroot_part ? sysroot_part : "",
             dep_path ? dep_path : "/dev/null",
             obj_path ? obj_path : "out.o",
             g_cc_include,
             g_cc_dir,
             g_repo_root);
    if (extra_include_dir && *extra_include_dir) {
        // Add -I<dir> so generated C can include headers relative to the original source directory.
        char inc[PATH_MAX + 8];
        snprintf(inc, sizeof(inc), " -I%s", extra_include_dir);
        strncat(cmd, inc, sizeof(cmd) - strlen(cmd) - 1);
    }
    if (opt->cc_flags && *opt->cc_flags) {
        strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, opt->cc_flags, sizeof(cmd) - strlen(cmd) - 1);
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
    if (run_cmd(cmd, opt->verbose) != 0) return -1;
    return 0;
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

    if (file_exists(g_cc_runtime_o)) {
        strncpy(out_runtime_path, g_cc_runtime_o, out_runtime_cap);
        out_runtime_path[out_runtime_cap - 1] = '\0';
        if (out_reused) *out_reused = 1;
        return 0;
    }

    // Build a runtime object under out/obj/runtime.o
    char runtime_obj[PATH_MAX];
    snprintf(runtime_obj, sizeof(runtime_obj), "%s/runtime.o", g_out_root);
    const char* cc_bin = pick_cc_bin(opt->cc_bin_override);
    const char* ccflags_env = getenv("CFLAGS");
    const char* cppflags_env = getenv("CPPFLAGS");
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s %s %s %s %s -I%s -I%s -I%s -c %s -o %s",
             cc_bin,
             ccflags_env ? ccflags_env : "",
             cppflags_env ? cppflags_env : "",
             target_part ? target_part : "",
             sysroot_part ? sysroot_part : "",
             g_cc_include,
             g_cc_dir,
             g_repo_root,
             g_cc_runtime_c,
             runtime_obj);
    if (opt->cc_flags && *opt->cc_flags) {
        strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, opt->cc_flags, sizeof(cmd) - strlen(cmd) - 1);
    }
    if (run_cmd(cmd, opt->verbose) != 0) return -1;
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
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "%s %s %s %s %s",
             cc_bin,
             target_part ? target_part : "",
             sysroot_part ? sysroot_part : "",
             ldflags_env ? ldflags_env : "",
             opt->ld_flags ? opt->ld_flags : "");
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
    // cc build [step] [options] <input.ccs> [output] [-- args...]
    const int max_cli = 32;
    char* cli_names[max_cli];
    long long cli_values[max_cli];
    size_t cli_count = 0;

    const int max_pos = 64;
    const char* pos_args[max_pos];
    int pos_count = 0;
    const char* user_out = NULL; // from -o (preferred)
    int saw_o = 0;
    const char* obj_out = NULL;
    const char* build_override = NULL;
    const char* cc_bin = NULL;
    const char* cc_flags = NULL;
    const char* ld_flags = NULL;
    const char* target_flag = NULL;
    const char* sysroot_flag = NULL;
    const char* out_dir = NULL;
    const char* bin_dir = NULL;
    int help = 0;
    int dump_consts = 0;
    int dry_run = 0;
    int no_build = 0;
    int no_runtime = 0;
    int keep_c = 1; // default keep C
    int verbose = 0;
    int summary = 0;
    CCMode mode = CC_MODE_LINK;
    int no_cache = 0;

    enum { CC_BUILD_STEP_DEFAULT = 0, CC_BUILD_STEP_RUN = 1, CC_BUILD_STEP_TEST = 2 } step = CC_BUILD_STEP_DEFAULT;
    int run_argc = 0;
    char** run_argv = NULL;

    int argi = 2;
    if (argc >= 3 && argv[2] && argv[2][0] && argv[2][0] != '-') {
        if (strcmp(argv[2], "run") == 0) {
            step = CC_BUILD_STEP_RUN;
            argi = 3;
        } else if (strcmp(argv[2], "test") == 0) {
            step = CC_BUILD_STEP_TEST;
            argi = 3;
        } else if (strcmp(argv[2], "help") == 0) {
            usage_build(argv[0]);
            return 0;
        }
    }

    for (int i = argi; i < argc; ++i) {
        if (strcmp(argv[i], "--") == 0) {
            run_argc = argc - (i + 1);
            run_argv = &argv[i + 1];
            break;
        }
        // Allow placing the step name after options (e.g. `cc build --no-cache run ...`).
        if (argv[i] && argv[i][0] && argv[i][0] != '-' && step == CC_BUILD_STEP_DEFAULT && pos_count == 0) {
            if (strcmp(argv[i], "run") == 0) { step = CC_BUILD_STEP_RUN; continue; }
            if (strcmp(argv[i], "test") == 0) { step = CC_BUILD_STEP_TEST; continue; }
            if (strcmp(argv[i], "help") == 0) { help = 1; continue; }
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            help = 1;
            continue;
        }
        if (strcmp(argv[i], "--summary") == 0) { summary = 1; continue; }
        if (strcmp(argv[i], "--no-cache") == 0) { no_cache = 1; continue; }
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
        if (strcmp(argv[i], "--emit-c-only") == 0) { mode = CC_MODE_EMIT_C; continue; }
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
        if (strcmp(argv[i], "--dry-run") == 0) { dry_run = 1; continue; }
        if (strcmp(argv[i], "--no-runtime") == 0) { no_runtime = 1; continue; }
        if (strcmp(argv[i], "--keep-c") == 0) { keep_c = 1; continue; }
        if (strcmp(argv[i], "--verbose") == 0) { verbose = 1; continue; }
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

    // Apply output directory override before creating/deriving any outputs.
    cc_set_out_dir(out_dir, bin_dir);

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

            CCBuildTargetDecl targets[32];
            size_t target_count = 0;
            char* def_name = NULL;
            if (cc_build_list_targets(build_path_for_help, targets, &target_count, 32, &def_name) == 0 && (target_count || def_name)) {
                fprintf(stderr, "\nDeclared CC_TARGETs in %s:\n", build_path_for_help);
                if (def_name) fprintf(stderr, "  default: %s\n", def_name);
                for (size_t i = 0; i < target_count; ++i) {
                    fprintf(stderr, "  %s (exe)  [", targets[i].name);
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

    if (ensure_out_dir() != 0) {
        fprintf(stderr, "cc: failed to create out dirs under: %s\n", g_out_root);
        goto parse_fail;
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

    if (step == CC_BUILD_STEP_RUN) {
        mode = CC_MODE_LINK; // run requires a binary
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
            CCBuildTargetDecl targets[32];
            size_t target_count = 0;
            char* def_name = NULL;
            int terr = cc_build_list_targets(build_path_for_help, targets, &target_count, 32, &def_name);
            if (terr != 0) {
                cc_build_free_targets(targets, target_count, def_name);
                goto parse_fail;
            }
            if (target_count == 0) {
                fprintf(stderr, "cc: build.cc has no CC_TARGET entries\n");
                cc_build_free_targets(targets, target_count, def_name);
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
                    goto parse_fail;
                }
            }

            // Resolve source paths relative to the build.cc directory.
            char build_dir[PATH_MAX];
            cc__dir_of_path(build_path_for_help, build_dir, sizeof(build_dir));
            if (chosen->src_count > (size_t)max_pos) {
                fprintf(stderr, "cc: target has too many sources (max %d)\n", max_pos);
                cc_build_free_targets(targets, target_count, def_name);
                goto parse_fail;
            }
            static char resolved[64][PATH_MAX];
            for (size_t i = 0; i < chosen->src_count; ++i) {
                cc__join_path(build_dir, chosen->srcs[i], resolved[i], sizeof(resolved[i]));
                inputs[i] = resolved[i];
            }
            input_count = (int)chosen->src_count;

            if (mode == CC_MODE_LINK) {
                // Default binary output for targets.
                static char default_bin[PATH_MAX];
                snprintf(default_bin, sizeof(default_bin), "%s/%s", g_bin_root, chosen->name);
                if (!user_out) user_out = default_bin;
            }

            cc_build_free_targets(targets, target_count, def_name);
        }
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
            .no_runtime = no_runtime,
            .keep_c = keep_c,
            .verbose = verbose,
            .build_override = build_override,
            .no_build = no_build,
            .dump_consts = dump_consts,
            .dry_run = dry_run,
            .summary = summary,
            .out_dir = g_out_root,
            .bin_dir = g_bin_root,
            .no_cache = no_cache,
            .cli_names = cli_names,
            .cli_values = cli_values,
            .cli_count = cli_count,
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
        (void)cc__stat_sig(cc_bin ? cc_bin : "cc", &cc_sig_for_key);

        int emit_reused = 0, emit_built = 0;
        int obj_reused = 0, obj_built = 0;

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
            const char* c_for_compile = is_raw_c ? inputs[i] : c_bufs[i];

            if (is_raw_c) {
                // No CC lowering for .c inputs.
                emit_reused++;
            } else if (cache_ok) {
                CCFileSig in_sig;
                in_sig.mtime_sec = 0;
                in_sig.size = 0;
                (void)cc__stat_sig(inputs[i], &in_sig);
                uint64_t h = 1469598103934665603ULL;
                h = cc__fnv1a64_str(h, inputs[i]);
                h = cc__fnv1a64_i64(h, in_sig.mtime_sec);
                h = cc__fnv1a64_i64(h, in_sig.size);
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
                h = cc__fnv1a64_i64(h, (long long)no_build);
                // bake in const bindings + CLI -D (already merged into bindings)
                h = cc__fnv1a64_i64(h, (long long)binding_count);
                for (size_t bi = 0; bi < binding_count; ++bi) {
                    h = cc__fnv1a64_str(h, bindings[bi].name);
                    h = cc__fnv1a64_i64(h, bindings[bi].value);
                }
                emit_key = h;

                uint64_t prev = 0;
                if (file_exists(c_bufs[i]) && cc__read_u64_file(meta_path, &prev) == 0 && prev == emit_key) {
                    emit_reused++;
                } else {
                    int err = cc_compile_with_config(inputs[i], c_bufs[i], &cfg);
                    if (err != 0) goto parse_fail;
                    (void)cc__write_u64_file(meta_path, emit_key);
                    emit_built++;
                }
            } else {
                if (!is_raw_c) {
                    int err = cc_compile_with_config(inputs[i], c_bufs[i], &cfg);
                    if (err != 0) goto parse_fail;
                    emit_built++;
                }
            }
            if (mode != CC_MODE_EMIT_C) {
                char obj_meta_path[PATH_MAX];
                snprintf(obj_meta_path, sizeof(obj_meta_path), "%s/%s.obj", g_cache_root, stem);
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
            snprintf(link_meta_path, sizeof(link_meta_path), "%s/%s.link", g_cache_root, out_stem);
            CCFileSig rt_sig;
            rt_sig.mtime_sec = 0; rt_sig.size = 0;
            if (!no_runtime) (void)cc__stat_sig(runtime_path, &rt_sig);
            uint64_t h = 1469598103934665603ULL;
            h = cc__fnv1a64_str(h, target_part);
            h = cc__fnv1a64_str(h, sysroot_part);
            h = cc__fnv1a64_str(h, ld_flags);
            h = cc__fnv1a64_str(h, getenv("LDFLAGS"));
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

    int raw_c = cc__is_raw_c(in_path);
    if (mode == CC_MODE_EMIT_C) {
        if (user_out) {
            strncpy(c_out, user_out, sizeof(c_out));
            c_out[sizeof(c_out)-1] = '\0';
        } else if (derive_default_output(in_path, c_out, sizeof(c_out)) != 0) {
            fprintf(stderr, "cc: failed to derive default C output\n");
            goto parse_fail;
        }
    } else {
        if (raw_c) {
            strncpy(c_out, in_path, sizeof(c_out));
            c_out[sizeof(c_out)-1] = '\0';
        } else {
            if (derive_default_output(in_path, c_out, sizeof(c_out)) != 0) {
                fprintf(stderr, "cc: failed to derive default C output\n");
                goto parse_fail;
            }
        }
    }

    if (mode != CC_MODE_EMIT_C) {
        if (obj_out) {
            strncpy(obj_path, obj_out, sizeof(obj_path));
            obj_path[sizeof(obj_path)-1] = '\0';
        } else if (derive_default_obj(in_path, obj_path, sizeof(obj_path)) != 0) {
            fprintf(stderr, "cc: failed to derive default object output\n");
            goto parse_fail;
        }
    }

    if (mode == CC_MODE_LINK) {
        if (user_out) {
            strncpy(bin_path, user_out, sizeof(bin_path));
            bin_path[sizeof(bin_path)-1] = '\0';
        } else if (derive_default_bin(in_path, bin_path, sizeof(bin_path)) != 0) {
            fprintf(stderr, "cc: failed to derive default binary output\n");
            goto parse_fail;
        }
    }

    CCBuildOptions opt = {
        .in_path = in_path,
        .c_out_path = c_out,
        .obj_out_path = (mode == CC_MODE_EMIT_C) ? NULL : obj_path,
        .bin_out_path = (mode == CC_MODE_LINK) ? bin_path : NULL,
        .mode = mode,
        .cc_bin_override = cc_bin,
        .cc_flags = cc_flags,
        .ld_flags = ld_flags,
        .target_flag = target_flag ? target_flag : "",
        .sysroot_flag = sysroot_flag ? sysroot_flag : "",
        .no_runtime = no_runtime,
        .keep_c = keep_c,
        .verbose = verbose,
        .build_override = build_override,
        .no_build = no_build,
        .dump_consts = dump_consts,
        .dry_run = dry_run,
        .summary = summary,
        .out_dir = g_out_root,
        .bin_dir = g_bin_root,
        .no_cache = no_cache,
        .cli_names = cli_names,
        .cli_values = cli_values,
        .cli_count = cli_count,
    };
    CCBuildSummary sum;
    int compile_err = compile_with_build(&opt, &sum);
    print_build_summary(&opt, &sum, step == CC_BUILD_STEP_RUN ? "run" : "default");
    for (size_t i = 0; i < cli_count; ++i) {
        free(cli_names[i]);
    }
    if (compile_err != 0) return compile_err;

    if (step == CC_BUILD_STEP_RUN) {
        // Print project-specific options (if any) when requested explicitly.
        // (Zig-style: `zig build --help` shows options; we keep it minimal for now.)
        char* exec_argv[64];
        int idx = 0;
        exec_argv[idx++] = (char*)bin_path;
        for (int j = 0; j < run_argc && idx < (int)(sizeof(exec_argv) / sizeof(exec_argv[0]) - 1); ++j) {
            exec_argv[idx++] = run_argv[j];
        }
        exec_argv[idx] = NULL;
        return run_exec(bin_path, exec_argv, verbose);
    }
    return 0;

parse_fail:
    for (size_t i = 0; i < cli_count; ++i) {
        free(cli_names[i]);
    }
    return -1;
}

int main(int argc, char **argv) {
    cc_init_paths(argv[0]);
    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        usage(argv[0]);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "build") == 0) {
        return run_build_mode(argc, argv) == 0 ? 0 : 1;
    }

    // Default mode: cc [options] <inputs...> [-o out/bin/<stem>] [--obj-out ...]
    const int max_pos = 64;
    const char* pos_args[max_pos];
    int pos_count = 0;
    const char* user_out = NULL;
    int saw_o = 0;
    const char* obj_out = NULL;
    const char* build_override = NULL;
    const char* cc_bin = NULL;
    const char* cc_flags = NULL;
    const char* ld_flags = NULL;
    const char* target_flag = NULL;
    const char* sysroot_flag = NULL;
    const char* out_dir = NULL;
    const char* bin_dir = NULL;
    int no_build = 0;
    int no_runtime = 0;
    int dump_consts = 0;
    int dry_run = 0;
    int keep_c = 1;
    int verbose = 0;
    int no_cache = 0;
    CCMode mode = CC_MODE_LINK;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--emit-c-only") == 0) { mode = CC_MODE_EMIT_C; continue; }
        if (strcmp(argv[i], "--compile") == 0) { mode = CC_MODE_COMPILE; continue; }
        if (strcmp(argv[i], "--link") == 0) { mode = CC_MODE_LINK; continue; }
        if (strcmp(argv[i], "--build-file") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --build-file requires a path\n"); usage(argv[0]); return 1; }
            build_override = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--no-build") == 0) { no_build = 1; continue; }
        if (strcmp(argv[i], "--dump-consts") == 0) { dump_consts = 1; continue; }
        if (strcmp(argv[i], "--dry-run") == 0) { dry_run = 1; continue; }
        if (strcmp(argv[i], "--no-runtime") == 0) { no_runtime = 1; continue; }
        if (strcmp(argv[i], "--keep-c") == 0) { keep_c = 1; continue; }
        if (strcmp(argv[i], "--verbose") == 0) { verbose = 1; continue; }
        if (strcmp(argv[i], "--no-cache") == 0) { no_cache = 1; continue; }
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

    if (pos_count == 0) {
        usage(argv[0]);
        return 1;
    }

    cc_set_out_dir(out_dir, bin_dir);
    if (ensure_out_dir() != 0) {
        fprintf(stderr, "cc: failed to create out dirs under: %s\n", g_out_root);
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

        CCConstBinding bindings[128];
        size_t binding_count = 0;
        CCBuildOptions base_opt = {
            .in_path = inputs[0],
            .cc_bin_override = cc_bin,
            .cc_flags = cc_flags,
            .ld_flags = ld_flags,
            .target_flag = target_flag ? target_flag : "",
            .sysroot_flag = sysroot_flag ? sysroot_flag : "",
            .no_runtime = no_runtime,
            .keep_c = keep_c,
            .verbose = verbose,
            .build_override = build_override,
            .no_build = no_build,
            .dump_consts = dump_consts,
            .dry_run = dry_run,
            .summary = 0,
            .out_dir = g_out_root,
            .bin_dir = g_bin_root,
            .cli_names = NULL,
            .cli_values = NULL,
            .cli_count = 0,
        };
        int berr = cc__load_const_bindings(&base_opt, bindings, &binding_count);
        if (berr != 0) return 1;
        if (dump_consts) {
            for (size_t i = 0; i < binding_count; ++i) {
                printf("CONST %s=%lld\n", bindings[i].name, bindings[i].value);
            }
        }
        if (dry_run) return 0;
        CCCompileConfig cfg = {.consts = bindings, .const_count = binding_count};

        char target_part[256]; char sysroot_part[256];
        target_part[0] = '\0'; sysroot_part[0] = '\0';
        if (target_flag && *target_flag) snprintf(target_part, sizeof(target_part), "--target %s", target_flag);
        if (sysroot_flag && *sysroot_flag) snprintf(sysroot_part, sizeof(sysroot_part), "--sysroot %s", sysroot_flag);

        char used[64][128]; size_t used_count = 0;
        const char* obj_paths[64];
        char obj_bufs[64][PATH_MAX];
        char c_bufs[64][PATH_MAX];
        char dep_bufs[64][PATH_MAX];
        char src_dir_bufs[64][PATH_MAX];
        for (int i = 0; i < input_count; ++i) {
            char stem0[128];
            char stem[128];
            cc__stem_from_path(inputs[i], stem0, sizeof(stem0));
            if (cc__unique_stem(stem0, used, &used_count, 64, stem, sizeof(stem)) != 0) return 1;
            cc__derive_c_path_from_stem(stem, c_bufs[i], sizeof(c_bufs[i]));
            cc__derive_o_path_from_stem(stem, obj_bufs[i], sizeof(obj_bufs[i]));
            cc__derive_d_path_from_stem(stem, dep_bufs[i], sizeof(dep_bufs[i]));
            cc__dir_of_path(inputs[i], src_dir_bufs[i], sizeof(src_dir_bufs[i]));

            int is_raw_c = cc__is_raw_c(inputs[i]);
            const char* c_for_compile = is_raw_c ? inputs[i] : c_bufs[i];
            if (mode == CC_MODE_EMIT_C) {
                if (is_raw_c) {
                    if (cc__copy_file(inputs[i], c_bufs[i]) != 0) return 1;
                } else {
                    int err = cc_compile_with_config(inputs[i], c_bufs[i], &cfg);
                    if (err != 0) return 1;
                }
                continue;
            }
            if (!is_raw_c) {
                int err = cc_compile_with_config(inputs[i], c_bufs[i], &cfg);
                if (err != 0) return 1;
            }
            if (mode != CC_MODE_EMIT_C) {
                if (cc__compile_c_to_obj(&base_opt, c_for_compile, obj_bufs[i], dep_bufs[i], src_dir_bufs[i], target_part, sysroot_part) != 0) return 1;
                obj_paths[i] = obj_bufs[i];
            }
        }
        if (mode == CC_MODE_EMIT_C || mode == CC_MODE_COMPILE) return 0;
        char runtime_path[PATH_MAX]; int runtime_reused = 0;
        if (cc__ensure_runtime_obj(&base_opt, target_part, sysroot_part, runtime_path, sizeof(runtime_path), &runtime_reused) != 0) return 1;
        if (cc__link_many(&base_opt, obj_paths, (size_t)input_count, runtime_path, target_part, sysroot_part, user_out) != 0) return 1;
        return 0;
    }

    const char* in_path = inputs[0];

    char c_out[512];
    char obj_path[512];
    char bin_path[512];

    int raw_c = cc__is_raw_c(in_path);
    if (mode == CC_MODE_EMIT_C) {
        if (user_out) {
            strncpy(c_out, user_out, sizeof(c_out));
            c_out[sizeof(c_out)-1] = '\0';
        } else if (derive_default_output(in_path, c_out, sizeof(c_out)) != 0) {
            fprintf(stderr, "cc: failed to derive default C output\n");
            return 1;
        }
    } else {
        // For .c inputs, treat the input itself as the C file (no CC lowering).
        if (raw_c) {
            strncpy(c_out, in_path, sizeof(c_out));
            c_out[sizeof(c_out)-1] = '\0';
        } else {
            if (derive_default_output(in_path, c_out, sizeof(c_out)) != 0) {
                fprintf(stderr, "cc: failed to derive default C output\n");
                return 1;
            }
        }
    }

    if (mode != CC_MODE_EMIT_C) {
        if (obj_out) {
            strncpy(obj_path, obj_out, sizeof(obj_path));
            obj_path[sizeof(obj_path)-1] = '\0';
        } else if (derive_default_obj(in_path, obj_path, sizeof(obj_path)) != 0) {
            fprintf(stderr, "cc: failed to derive default object output\n");
            return 1;
        }
    }

    if (mode == CC_MODE_LINK) {
        if (user_out) {
            strncpy(bin_path, user_out, sizeof(bin_path));
            bin_path[sizeof(bin_path)-1] = '\0';
        } else if (derive_default_bin(in_path, bin_path, sizeof(bin_path)) != 0) {
            fprintf(stderr, "cc: failed to derive default binary output\n");
            return 1;
        }
    }

    CCBuildOptions opt = {
        .in_path = in_path,
        .c_out_path = c_out,
        .obj_out_path = (mode == CC_MODE_EMIT_C) ? NULL : obj_path,
        .bin_out_path = (mode == CC_MODE_LINK) ? bin_path : NULL,
        .mode = mode,
        .cc_bin_override = cc_bin,
        .cc_flags = cc_flags,
        .ld_flags = ld_flags,
        .target_flag = target_flag ? target_flag : "",
        .sysroot_flag = sysroot_flag ? sysroot_flag : "",
        .no_runtime = no_runtime,
        .keep_c = keep_c,
        .verbose = verbose,
        .build_override = build_override,
        .no_build = no_build,
        .dump_consts = dump_consts,
        .dry_run = dry_run,
        .summary = 0,
        .out_dir = g_out_root,
        .bin_dir = g_bin_root,
        .no_cache = no_cache,
        .cli_names = NULL,
        .cli_values = NULL,
        .cli_count = 0,
    };
    CCBuildSummary sum;
    int err = compile_with_build(&opt, &sum);
    return err == 0 ? 0 : 1;
}

