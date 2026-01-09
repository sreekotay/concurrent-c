#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
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

// Resolved repo-relative paths so `./cc/bin/cc build ...` works from the repo root.
static int g_paths_inited = 0;
static char g_repo_root[PATH_MAX];
static char g_cc_dir[PATH_MAX];
static char g_cc_include[PATH_MAX];
static char g_cc_runtime_o[PATH_MAX];
static char g_cc_runtime_c[PATH_MAX];
static char g_out_root[PATH_MAX];
static char g_out_c[PATH_MAX];
static char g_out_obj[PATH_MAX];
static char g_out_bin[PATH_MAX];

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

static void cc_set_out_dir(const char* out_dir_opt) {
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

    snprintf(g_out_c, sizeof(g_out_c), "%s/c", g_out_root);
    snprintf(g_out_obj, sizeof(g_out_obj), "%s/obj", g_out_root);
    snprintf(g_out_bin, sizeof(g_out_bin), "%s/bin", g_out_root);
}

static void cc_init_paths(const char* argv0) {
    if (g_paths_inited) return;
    g_paths_inited = 1;

    char exe_abs[PATH_MAX];
    exe_abs[0] = '\0';
    if (argv0 && argv0[0]) {
        // Best effort: if argv0 is a path (common dev case: ./cc/bin/cc), realpath it.
        if (realpath(argv0, exe_abs) == NULL) {
            // Fallback: accept argv0 as-is.
            strncpy(exe_abs, argv0, sizeof(exe_abs));
            exe_abs[sizeof(exe_abs) - 1] = '\0';
        }
    }

    // Derive repo root from .../cc/bin/cc.
    char tmp[PATH_MAX];
    strncpy(tmp, exe_abs[0] ? exe_abs : "", sizeof(tmp));
    tmp[sizeof(tmp) - 1] = '\0';
    // tmp = .../cc/bin
    cc__dirname_inplace(tmp);
    // tmp = .../cc
    cc__dirname_inplace(tmp);
    strncpy(g_cc_dir, tmp[0] ? tmp : "", sizeof(g_cc_dir));
    g_cc_dir[sizeof(g_cc_dir) - 1] = '\0';

    // tmp = repo root
    cc__dirname_inplace(tmp);
    if (!tmp[0]) {
        // Final fallback: assume current working directory is the repo root.
        if (getcwd(tmp, sizeof(tmp)) == NULL) {
            strncpy(tmp, ".", sizeof(tmp));
            tmp[sizeof(tmp) - 1] = '\0';
        }
    }

    strncpy(g_repo_root, tmp, sizeof(g_repo_root));
    g_repo_root[sizeof(g_repo_root) - 1] = '\0';

    snprintf(g_cc_include, sizeof(g_cc_include), "%s/cc/include", g_repo_root);
    // Prefer the compiler-build runtime object (built by `make -C cc`) which now lives under out/.
    snprintf(g_cc_runtime_o, sizeof(g_cc_runtime_o), "%s/out/cc/obj/runtime/concurrent_c.o", g_repo_root);
    snprintf(g_cc_runtime_c, sizeof(g_cc_runtime_c), "%s/cc/runtime/concurrent_c.c", g_repo_root);
    cc_set_out_dir(NULL);
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s [options] <input.cc> [output]\n", prog);
    fprintf(stderr, "  %s build [options] <input.cc> <output>\n", prog);
    fprintf(stderr, "  %s build run [options] <input.cc> [-o out/<stem>] [-- <args...>]\n", prog);
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
    fprintf(stderr, "  --out-dir DIR       Output directory root (default: <repo>/out; subdirs: c/, obj/, bin/)\n");
    fprintf(stderr, "  --verbose           Print invoked commands\n");
}

static void usage_build(const char* prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s build [options] <input.cc> [output]\n", prog);
    fprintf(stderr, "  %s build run [options] <input.cc> [-o out/bin/<stem>] [-- <args...>]\n", prog);
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
    // Ensure root + subdirs exist.
    if (cc__mkdir_p(g_out_root) != 0) return -1;
    if (cc__mkdir_p(g_out_c) != 0) return -1;
    if (cc__mkdir_p(g_out_obj) != 0) return -1;
    if (cc__mkdir_p(g_out_bin) != 0) return -1;
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
    size_t dir_len = strlen(g_out_c);
    if (dir_len + 1 + stem_len + 2 > out_buf_size) { // "/" + ".c" + NUL
        return -1;
    }
    memcpy(out_buf, g_out_c, dir_len);
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
    size_t dir_len = strlen(g_out_obj);
    if (dir_len + 1 + stem_len + 3 > out_buf_size) { // "/" + ".o"+NUL
        return -1;
    }
    memcpy(out_buf, g_out_obj, dir_len);
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
    size_t dir_len = strlen(g_out_bin);
    if (dir_len + 1 + stem_len + 1 > out_buf_size) { // "/" + NUL
        return -1;
    }
    memcpy(out_buf, g_out_bin, dir_len);
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
    char** cli_names;
    long long* cli_values;
    size_t cli_count;
} CCBuildOptions;

typedef struct {
    const char* c_out_path;
    const char* obj_out_path;
    const char* bin_out_path;
    int did_emit_c;
    int did_compile_obj;
    int did_link;
    int runtime_reused;
    const char* runtime_obj_path;
} CCBuildSummary;

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
        CCBuildInputs inputs = {
            .target = &target,
            .envp = NULL
        };
        int err = cc_build_load_consts(build_path, &inputs, bindings, &count);
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
        for (size_t j = 0; j < count; ++j) {
            if (strcmp(bindings[j].name, name) == 0) {
                existed = 1;
                break;
            }
        }
        if (upsert_binding(bindings, &count, max_bindings, name, value) != 0) {
            fprintf(stderr, "cc: too many const bindings (max %d)\n", max_bindings);
            return -1;
        }
        if (existed) {
            fprintf(stderr, "cc: warning: overriding const %s from build.cc with CLI -D\n", name);
        }
    }

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

    // Emit C
    int err = cc_compile_with_config(opt->in_path, opt->c_out_path, &cfg);
    if (err != 0) {
        return err;
    }
    if (summary_out) summary_out->did_emit_c = 1;
    if (opt->mode == CC_MODE_EMIT_C) {
        return 0;
    }

    if (!opt->obj_out_path) {
        fprintf(stderr, "cc: internal error: missing object output path\n");
        return -1;
    }
    const char* cc_bin = pick_cc_bin(opt->cc_bin_override);
    char cmd[2048];

    // Compile to object
    const char* ccflags_env = getenv("CFLAGS");
    const char* cppflags_env = getenv("CPPFLAGS");
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
    char compile_cmd[2048];
    snprintf(compile_cmd, sizeof(compile_cmd), "%s %s %s %s %s -I%s -I%s -I%s -c %s -o %s",
             cc_bin,
             ccflags_env ? ccflags_env : "",
             cppflags_env ? cppflags_env : "",
             target_part,
             sysroot_part,
             g_cc_include,
             g_cc_dir,
             g_repo_root,
             opt->c_out_path,
             opt->obj_out_path);
    if (opt->cc_flags && *opt->cc_flags) {
        // Append extra flags at end
        strncat(compile_cmd, " ", sizeof(compile_cmd) - strlen(compile_cmd) - 1);
        strncat(compile_cmd, opt->cc_flags, sizeof(compile_cmd) - strlen(compile_cmd) - 1);
    }
    if (run_cmd(compile_cmd, opt->verbose) != 0) {
        return -1;
    }
    if (summary_out) summary_out->did_compile_obj = 1;

    if (opt->mode == CC_MODE_COMPILE) {
        return 0;
    }

    if (!opt->bin_out_path) {
        fprintf(stderr, "cc: internal error: missing binary output path\n");
        return -1;
    }
    // Link to binary
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
            snprintf(runtime_obj, sizeof(runtime_obj), "%s/runtime.o", g_out_obj);
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

    snprintf(link_cmd, sizeof(link_cmd), "%s %s %s %s %s %s %s -o %s",
             cc_bin,
             target_part,
             sysroot_part,
             ldflags_env ? ldflags_env : "",
             opt->ld_flags ? opt->ld_flags : "",
             opt->obj_out_path,
             have_runtime ? runtime_obj : "",
             opt->bin_out_path);
    if (run_cmd(link_cmd, opt->verbose) != 0) {
        return -1;
    }
    if (summary_out) summary_out->did_link = 1;

    if (!opt->keep_c && opt->mode != CC_MODE_EMIT_C) {
        // Leave C file in out/ by default; optional cleanup.
        (void)opt;
    }
    return 0;
}

static void print_build_summary(const CCBuildOptions* opt, const CCBuildSummary* s, const char* step_name) {
    if (!opt || !opt->summary || !s) return;
    fprintf(stderr, "cc build summary:\n");
    if (step_name) fprintf(stderr, "  step: %s\n", step_name);
    if (s->c_out_path) fprintf(stderr, "  c: %s%s\n", s->c_out_path, s->did_emit_c ? "" : " (skipped)");
    if (s->obj_out_path) fprintf(stderr, "  obj: %s%s\n", s->obj_out_path, s->did_compile_obj ? "" : " (skipped)");
    if (s->runtime_obj_path) {
        fprintf(stderr, "  runtime: %s (%s)\n", s->runtime_obj_path, s->runtime_reused ? "reused" : "compiled");
    } else {
        fprintf(stderr, "  runtime: (none)\n");
    }
    if (s->bin_out_path) fprintf(stderr, "  bin: %s%s\n", s->bin_out_path, s->did_link ? "" : " (skipped)");
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
    // cc build [step] [options] <input.cc> [output] [-- args...]
    const int max_cli = 32;
    char* cli_names[max_cli];
    long long cli_values[max_cli];
    size_t cli_count = 0;

    const char* in_path = NULL;
    const char* user_out = NULL;
    const char* obj_out = NULL;
    const char* build_override = NULL;
    const char* cc_bin = NULL;
    const char* cc_flags = NULL;
    const char* ld_flags = NULL;
    const char* target_flag = NULL;
    const char* sysroot_flag = NULL;
    const char* out_dir = NULL;
    int dump_consts = 0;
    int dry_run = 0;
    int no_build = 0;
    int no_runtime = 0;
    int keep_c = 1; // default keep C
    int verbose = 0;
    int summary = 0;
    CCMode mode = CC_MODE_LINK;

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
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage_build(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--summary") == 0) { summary = 1; continue; }
        if (strcmp(argv[i], "--out-dir") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --out-dir requires a path\n"); goto parse_fail; }
            out_dir = argv[++i];
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
            continue;
        }
        if (!in_path) { in_path = argv[i]; continue; }
        if (!user_out) { user_out = argv[i]; continue; }
        fprintf(stderr, "cc: unexpected argument: %s\n", argv[i]);
        goto parse_fail;
    }

    if (step != CC_BUILD_STEP_TEST && !in_path) {
        fprintf(stderr, "cc: missing input\n");
        goto parse_fail;
    }

    // Apply output directory override before creating/deriving any outputs.
    cc_set_out_dir(out_dir);

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
            fprintf(stderr, "cc build summary:\n  step: test\n  tool: %s\n  out_dir: %s\n", tool_path, g_out_root);
        }
        int rc = run_exec(tool_path, exec_argv, verbose);
        return rc;
    }

    char c_out[512];
    char obj_path[512];
    char bin_path[512];

    if (mode == CC_MODE_EMIT_C) {
        if (user_out) {
            strncpy(c_out, user_out, sizeof(c_out));
            c_out[sizeof(c_out)-1] = '\0';
        } else if (derive_default_output(in_path, c_out, sizeof(c_out)) != 0) {
            fprintf(stderr, "cc: failed to derive default C output\n");
            goto parse_fail;
        }
    } else {
        if (derive_default_output(in_path, c_out, sizeof(c_out)) != 0) {
            fprintf(stderr, "cc: failed to derive default C output\n");
            goto parse_fail;
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

    if (step == CC_BUILD_STEP_RUN) {
        mode = CC_MODE_LINK; // run requires a binary
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

    // Default mode: cc [options] <input.cc> [output]
    const char* in_path = NULL;
    const char* user_out = NULL;
    const char* obj_out = NULL;
    const char* build_override = NULL;
    const char* cc_bin = NULL;
    const char* cc_flags = NULL;
    const char* ld_flags = NULL;
    const char* target_flag = NULL;
    const char* sysroot_flag = NULL;
    const char* out_dir = NULL;
    int no_build = 0;
    int no_runtime = 0;
    int dump_consts = 0;
    int dry_run = 0;
    int keep_c = 1;
    int verbose = 0;
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
        if (strcmp(argv[i], "--out-dir") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "cc: --out-dir requires a path\n"); usage(argv[0]); return 1; }
            out_dir = argv[++i];
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
            continue;
        }
        if (!in_path) { in_path = argv[i]; continue; }
        if (!user_out) { user_out = argv[i]; continue; }
        fprintf(stderr, "cc: unexpected argument: %s\n", argv[i]);
        usage(argv[0]);
        return 1;
    }

    if (!in_path) {
        usage(argv[0]);
        return 1;
    }

    cc_set_out_dir(out_dir);
    if (ensure_out_dir() != 0) {
        fprintf(stderr, "cc: failed to create out dirs under: %s\n", g_out_root);
        return 1;
    }

    char c_out[512];
    char obj_path[512];
    char bin_path[512];

    if (mode == CC_MODE_EMIT_C) {
        if (user_out) {
            strncpy(c_out, user_out, sizeof(c_out));
            c_out[sizeof(c_out)-1] = '\0';
        } else if (derive_default_output(in_path, c_out, sizeof(c_out)) != 0) {
            fprintf(stderr, "cc: failed to derive default C output\n");
            return 1;
        }
    } else {
        if (derive_default_output(in_path, c_out, sizeof(c_out)) != 0) {
            fprintf(stderr, "cc: failed to derive default C output\n");
            return 1;
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
        .cli_names = NULL,
        .cli_values = NULL,
        .cli_count = 0,
    };
    CCBuildSummary sum;
    int err = compile_with_build(&opt, &sum);
    return err == 0 ? 0 : 1;
}

