#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "build/build.h"
#include "driver.h"

static void usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s [options] <input.cc> [output]\n", prog);
    fprintf(stderr, "  %s build [options] <input.cc> <output>\n", prog);
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
    fprintf(stderr, "  --verbose           Print invoked commands\n");
}

static int run_simple(const char* in_path, const char* out_path) {
    return cc_compile(in_path, out_path);
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
    if (mkdir("out", 0777) == -1) {
        if (errno == EEXIST) return 0;
        return errno ? errno : -1;
    }
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
    const char* dir = "out/";
    size_t dir_len = strlen(dir);
    if (dir_len + stem_len + 2 > out_buf_size) { // ".c" + NUL
        return -1;
    }
    memcpy(out_buf, dir, dir_len);
    memcpy(out_buf + dir_len, base, stem_len);
    out_buf[dir_len + stem_len] = '\0';
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
    const char* dir = "out/";
    size_t dir_len = strlen(dir);
    if (dir_len + stem_len + 3 > out_buf_size) { // ".o"+NUL
        return -1;
    }
    memcpy(out_buf, dir, dir_len);
    memcpy(out_buf + dir_len, base, stem_len);
    out_buf[dir_len + stem_len] = '\0';
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
    const char* dir = "out/";
    size_t dir_len = strlen(dir);
    if (dir_len + stem_len + 1 > out_buf_size) { // +NUL
        return -1;
    }
    memcpy(out_buf, dir, dir_len);
    memcpy(out_buf + dir_len, base, stem_len);
    out_buf[dir_len + stem_len] = '\0';
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
    const char* target_flag;
    const char* sysroot_flag;
    int no_runtime;
    int keep_c;
    int verbose;
    const char* build_override;
    int no_build;
    int dump_consts;
    int dry_run;
    char** cli_names;
    long long* cli_values;
    size_t cli_count;
} CCBuildOptions;

// Core compile helper shared by default and build modes.
static int compile_with_build(const CCBuildOptions* opt) {
    if (!opt || !opt->in_path || !opt->c_out_path) {
        fprintf(stderr, "cc: missing input or c_out_path\n");
        return -1;
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
    char compile_cmd[2048];
    snprintf(compile_cmd, sizeof(compile_cmd), "%s %s %s %s %s -Iinclude -I. -c %s -o %s",
             cc_bin,
             ccflags_env ? ccflags_env : "",
             cppflags_env ? cppflags_env : "",
             opt->target_flag ? opt->target_flag : "",
             opt->sysroot_flag ? opt->sysroot_flag : "",
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
        snprintf(runtime_obj, sizeof(runtime_obj), "out/runtime.o");
        snprintf(cmd, sizeof(cmd), "%s %s %s %s %s -Iinclude -I. -c runtime/concurrent_c.c -o %s",
                 cc_bin,
                 ccflags_env ? ccflags_env : "",
                 cppflags_env ? cppflags_env : "",
                 opt->target_flag ? opt->target_flag : "",
                 opt->sysroot_flag ? opt->sysroot_flag : "",
                 runtime_obj);
        if (opt->cc_flags && *opt->cc_flags) {
            strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
            strncat(cmd, opt->cc_flags, sizeof(cmd) - strlen(cmd) - 1);
        }
        if (run_cmd(cmd, opt->verbose) != 0) {
            return -1;
        }
        have_runtime = 1;
    }

    snprintf(link_cmd, sizeof(link_cmd), "%s %s %s %s %s %s %s -o %s",
             cc_bin,
             opt->target_flag ? opt->target_flag : "",
             opt->sysroot_flag ? opt->sysroot_flag : "",
             ldflags_env ? ldflags_env : "",
             opt->ld_flags ? opt->ld_flags : "",
             opt->obj_out_path,
             have_runtime ? "out/runtime.o" : "",
             opt->bin_out_path);
    if (run_cmd(link_cmd, opt->verbose) != 0) {
        return -1;
    }

    if (!opt->keep_c && opt->mode != CC_MODE_EMIT_C) {
        // Leave C file in out/ by default; optional cleanup.
        (void)opt;
    }
    return 0;
}

static int run_build_mode(int argc, char** argv) {
    // cc build [options] <input.cc> [output]
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
    int dump_consts = 0;
    int dry_run = 0;
    int no_build = 0;
    int no_runtime = 0;
    int keep_c = 1; // default keep C
    int verbose = 0;
    CCMode mode = CC_MODE_LINK;

    for (int i = 2; i < argc; ++i) {
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

    if (!in_path) {
        fprintf(stderr, "cc: missing input\n");
        goto parse_fail;
    }

    if (ensure_out_dir() != 0) {
        fprintf(stderr, "cc: failed to create out/ directory\n");
        goto parse_fail;
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
        .cli_names = cli_names,
        .cli_values = cli_values,
        .cli_count = cli_count,
    };
    int compile_err = compile_with_build(&opt);
    for (size_t i = 0; i < cli_count; ++i) {
        free(cli_names[i]);
    }
    return compile_err;

parse_fail:
    for (size_t i = 0; i < cli_count; ++i) {
        free(cli_names[i]);
    }
    return -1;
}

int main(int argc, char **argv) {
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

    if (ensure_out_dir() != 0) {
        fprintf(stderr, "cc: failed to create out/ directory\n");
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
        .cli_names = NULL,
        .cli_values = NULL,
        .cli_count = 0,
    };
    int err = compile_with_build(&opt);
    return err == 0 ? 0 : 1;
}

