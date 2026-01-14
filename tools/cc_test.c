#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

static int file_exists(const char* path) {
    if (!path) return 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int ensure_out_dir(void) {
    if (mkdir("out", 0777) == -1) {
        /* EEXIST is fine; keep it simple and portable. */
        return 0;
    }
    return 0;
}

static int ensure_dir(const char* path) {
    if (!path || !path[0]) return -1;
    if (mkdir(path, 0777) == -1) {
        if (errno == EEXIST) return 0;
        return -1;
    }
    return 0;
}

static int ensure_dir_p(const char* path) {
    if (!path || !path[0]) return -1;
    char tmp[1024];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) return -1;
    memcpy(tmp, path, n + 1);
    // Skip leading slashes.
    for (size_t i = 1; i < n; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (tmp[0] && ensure_dir(tmp) != 0) return -1;
            tmp[i] = '/';
        }
    }
    if (ensure_dir(tmp) != 0) return -1;
    return 0;
}

static int ends_with(const char* s, const char* suf) {
    if (!s || !suf) return 0;
    size_t n = strlen(s), m = strlen(suf);
    if (m > n) return 0;
    return memcmp(s + (n - m), suf, m) == 0;
}

static void basename_no_ext(const char* path, char* out, size_t cap) {
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

static int str_contains(const char* hay, const char* needle) {
    if (!needle || !needle[0]) return 1;
    if (!hay) return 0;
    return strstr(hay, needle) != NULL;
}

static void* memmem_simple(const void* haystack, size_t haystack_len, const void* needle, size_t needle_len) {
    if (!needle || needle_len == 0) return (void*)haystack;
    if (!haystack || haystack_len < needle_len) return NULL;
    const unsigned char* h = (const unsigned char*)haystack;
    const unsigned char* n = (const unsigned char*)needle;
    for (size_t i = 0; i + needle_len <= haystack_len; ++i) {
        if (h[i] == n[0] && memcmp(h + i, n, needle_len) == 0) return (void*)(h + i);
    }
    return NULL;
}

static int read_entire_file_alloc(const char* path, unsigned char** out_buf, size_t* out_len) {
    if (!path || !out_buf || !out_len) return -1;
    *out_buf = NULL;
    *out_len = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    size_t cap = (size_t)sz;
    if (cap > 1024 * 1024) cap = 1024 * 1024;
    unsigned char* buf = (unsigned char*)malloc(cap + 1);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    buf[n] = 0;
    *out_buf = buf;
    *out_len = n;
    return 0;
}

static void trim_trailing_ws_inplace(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            s[n - 1] = '\0';
            n--;
            continue;
        }
        break;
    }
}

static void replace_newlines_with_spaces(char* s) {
    if (!s) return;
    for (char* p = s; *p; ++p) {
        if (*p == '\n' || *p == '\r') *p = ' ';
    }
}

static int wexitstatus_simple(int rc) {
    if (rc == -1) return 127;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    if (WIFSIGNALED(rc)) return 128 + WTERMSIG(rc);
    return 1;
}

static long long now_ms_monotonic(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000LL);
}

// Runs command via `sh -c <full>` with optional timeout. Returns:
// - exit code of the command (0..255)
// - 124 on timeout (like GNU timeout)
static int run_cmd_redirect_timeout(const char* cmd,
                                    const char* out_path,
                                    const char* err_path,
                                    int verbose,
                                    int timeout_sec) {
    if (!cmd) return -1;
    char full[4096];
    if (out_path && err_path) {
        snprintf(full, sizeof(full), "sh -c '%s > %s 2> %s'", cmd, out_path, err_path);
    } else if (out_path) {
        snprintf(full, sizeof(full), "sh -c '%s > %s'", cmd, out_path);
    } else if (err_path) {
        snprintf(full, sizeof(full), "sh -c '%s 2> %s'", cmd, err_path);
    } else {
        snprintf(full, sizeof(full), "%s", cmd);
    }
    if (verbose) fprintf(stderr, "cc_test: %s\n", full);

    pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        // New process group so we can kill the whole subtree on timeout.
        (void)setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", full, (char*)NULL);
        _exit(127);
    }

    long long start_ms = now_ms_monotonic();
    for (;;) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return wexitstatus_simple(st);
        if (r < 0) return 127;

        if (timeout_sec > 0) {
            long long elapsed_ms = now_ms_monotonic() - start_ms;
            if (elapsed_ms >= (long long)timeout_sec * 1000LL) {
                // Kill the process group (child pid is its pgid).
                (void)kill(-pid, SIGKILL);
                (void)kill(pid, SIGKILL);
                (void)waitpid(pid, &st, 0);
                return 124;
            }
        }
        // Sleep a bit to avoid busy waiting.
        usleep(10 * 1000);
    }
}

static void pid_pop(pid_t* pids_local, char** names_local, int* io_running, int* io_failed) {
    if (!pids_local || !names_local || !io_running || *io_running <= 0) return;
    int st = 0;
    pid_t pid = wait(&st);
    if (pid <= 0) return;
    int idx = -1;
    for (int i = 0; i < *io_running; ++i) {
        if (pids_local[i] == pid) { idx = i; break; }
    }
    if (idx >= 0) {
        int rc = wexitstatus_simple(st);
        if (rc != 0) (*io_failed)++;
        free(names_local[idx]);
        // Swap last into idx.
        int last = *io_running - 1;
        if (idx != last) {
            pids_local[idx] = pids_local[last];
            names_local[idx] = names_local[last];
        }
        // Clear last slot to avoid double frees.
        pids_local[last] = 0;
        names_local[last] = NULL;
    }
    (*io_running)--;
}

static int expect_contains_lines(const char* stream_name,
                                 const unsigned char* hay,
                                 size_t hay_len,
                                 const unsigned char* expectations,
                                 size_t exp_len) {
    if (!expectations || exp_len == 0) return 0;
    size_t i = 0;
    while (i < exp_len) {
        size_t line_start = i;
        while (i < exp_len && expectations[i] != '\n') i++;
        size_t line_end = i;
        if (i < exp_len && expectations[i] == '\n') i++;
        if (line_end > line_start && expectations[line_end - 1] == '\r') line_end--;

        size_t p = line_start;
        while (p < line_end && (expectations[p] == ' ' || expectations[p] == '\t')) p++;
        if (p == line_end) continue;
        if (expectations[p] == '#') continue;

        const unsigned char* needle = expectations + p;
        size_t needle_len = line_end - p;
        if (!memmem_simple(hay, hay_len, needle, needle_len)) {
            fprintf(stderr, "[FAIL] expected %s to contain: %.*s\n",
                    stream_name, (int)needle_len, (const char*)needle);
            return 1;
        }
    }
    return 0;
}

static int test_requires_async(const char* stem) {
    char p[512];
    snprintf(p, sizeof(p), "tests/%s.requires_async", stem);
    return file_exists(p);
}

static int run_one_test(const char* stem,
                        const char* input_path,
                        int compile_fail,
                        int verbose,
                        const char* out_dir,
                        const char* bin_dir,
                        int use_cache,
                        int build_timeout_sec,
                        int run_timeout_sec) {
    char bin_out[512];
    char build_err_txt[512];
    char out_txt[512];
    char err_txt[512];
    snprintf(bin_out, sizeof(bin_out), "%s/%s", (bin_dir && bin_dir[0]) ? bin_dir : "bin", stem);
    snprintf(build_err_txt, sizeof(build_err_txt), "%s/%s.build.stderr", (out_dir && out_dir[0]) ? out_dir : "out", stem);
    snprintf(out_txt, sizeof(out_txt), "%s/%s.stdout", (out_dir && out_dir[0]) ? out_dir : "out", stem);
    snprintf(err_txt, sizeof(err_txt), "%s/%s.stderr", (out_dir && out_dir[0]) ? out_dir : "out", stem);

    /* Sidecars */
    char exp_stdout_path[512], exp_stderr_path[512], exp_compile_err_path[512], ldflags_path[512];
    snprintf(exp_stdout_path, sizeof(exp_stdout_path), "tests/%s.stdout", stem);
    snprintf(exp_stderr_path, sizeof(exp_stderr_path), "tests/%s.stderr", stem);
    snprintf(exp_compile_err_path, sizeof(exp_compile_err_path), "tests/%s.compile_err", stem);
    snprintf(ldflags_path, sizeof(ldflags_path), "tests/%s.ldflags", stem);

    unsigned char *exp_stdout = NULL, *exp_stderr = NULL, *exp_compile_err = NULL, *ldflags = NULL;
    size_t exp_stdout_len = 0, exp_stderr_len = 0, exp_compile_err_len = 0, ldflags_len = 0;
    (void)read_entire_file_alloc(exp_stdout_path, &exp_stdout, &exp_stdout_len);
    (void)read_entire_file_alloc(exp_stderr_path, &exp_stderr, &exp_stderr_len);
    (void)read_entire_file_alloc(exp_compile_err_path, &exp_compile_err, &exp_compile_err_len);
    (void)read_entire_file_alloc(ldflags_path, &ldflags, &ldflags_len);

    char ldflags_clean[1024];
    ldflags_clean[0] = '\0';
    if (ldflags && ldflags_len) {
        size_t n = ldflags_len < sizeof(ldflags_clean) - 1 ? ldflags_len : sizeof(ldflags_clean) - 1;
        memcpy(ldflags_clean, ldflags, n);
        ldflags_clean[n] = '\0';
        replace_newlines_with_spaces(ldflags_clean);
        trim_trailing_ws_inplace(ldflags_clean);
    }

    /* 1) Build via ccc build (this is the build system under test) */
    char build_cmd[3072];
    const char* cache_flag = use_cache ? "" : "--no-cache ";
    if (ldflags_clean[0]) {
        snprintf(build_cmd, sizeof(build_cmd),
                 "./cc/bin/ccc build %s--out-dir %s --bin-dir %s --link %s -o %s --ld-flags \"%s\"",
                 cache_flag,
                 (out_dir && out_dir[0]) ? out_dir : "out",
                 (bin_dir && bin_dir[0]) ? bin_dir : "bin",
                 input_path, bin_out, ldflags_clean);
    } else {
        snprintf(build_cmd, sizeof(build_cmd),
                 "./cc/bin/ccc build %s--out-dir %s --bin-dir %s --link %s -o %s",
                 cache_flag,
                 (out_dir && out_dir[0]) ? out_dir : "out",
                 (bin_dir && bin_dir[0]) ? bin_dir : "bin",
                 input_path, bin_out);
    }

    int build_rc = run_cmd_redirect_timeout(build_cmd, NULL, build_err_txt, verbose, build_timeout_sec);
    if (compile_fail) {
        if (build_rc == 0) {
            fprintf(stderr, "[FAIL] %s: expected build to fail\n", stem);
            free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(ldflags);
            return 1;
        }
        if (build_rc == 124) {
            fprintf(stderr, "[TIMEOUT] %s: build timed out after %ds\n", stem, build_timeout_sec);
            free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(ldflags);
            return 1;
        }
        unsigned char* err_buf = NULL;
        size_t err_len = 0;
        (void)read_entire_file_alloc(build_err_txt, &err_buf, &err_len);
        int bad = expect_contains_lines("compile_err", err_buf, err_len, exp_compile_err, exp_compile_err_len);
        free(err_buf);
        free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(ldflags);
        if (bad) return 1;
        fprintf(stderr, "[OK] %s\n", stem);
        return 0;
    }

    if (build_rc != 0) {
        if (build_rc == 124) {
            fprintf(stderr, "[TIMEOUT] %s: build timed out after %ds\n", stem, build_timeout_sec);
        }
        fprintf(stderr, "[FAIL] %s: build failed\n", stem);
        free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(ldflags);
        return 1;
    }

    int run_rc = run_cmd_redirect_timeout(bin_out, out_txt, err_txt, verbose, run_timeout_sec);
    if (run_rc != 0) {
        if (run_rc == 124) {
            fprintf(stderr, "[TIMEOUT] %s: run timed out after %ds\n", stem, run_timeout_sec);
        }
        fprintf(stderr, "[FAIL] %s: run failed\n", stem);
        free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(ldflags);
        return 1;
    }

    unsigned char *out_buf = NULL, *err_buf = NULL;
    size_t out_len = 0, err_len = 0;
    (void)read_entire_file_alloc(out_txt, &out_buf, &out_len);
    (void)read_entire_file_alloc(err_txt, &err_buf, &err_len);
    int bad = 0;
    bad |= expect_contains_lines("stdout", out_buf, out_len, exp_stdout, exp_stdout_len);
    bad |= expect_contains_lines("stderr", err_buf, err_len, exp_stderr, exp_stderr_len);
    free(out_buf);
    free(err_buf);
    free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(ldflags);
    if (bad) return 1;
    fprintf(stderr, "[OK] %s\n", stem);
    return 0;
}

static void usage(const char* prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s [--list] [--filter SUBSTR] [--verbose] [--jobs N] [--build-timeout SECONDS] [--run-timeout SECONDS] [--use-cache] [--clean]\n", prog);
}

int main(int argc, char** argv) {
    const char* filter = NULL;
    int verbose = 0;
    int list_only = 0;
    int jobs = 1;
    int use_cache = 0;
    int clean = 0;
    int build_timeout_sec = 300;
    int run_timeout_sec = 5;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--verbose") == 0) { verbose = 1; continue; }
        if (strcmp(argv[i], "--list") == 0) { list_only = 1; continue; }
        if (strcmp(argv[i], "--use-cache") == 0) { use_cache = 1; continue; }
        if (strcmp(argv[i], "--clean") == 0) { clean = 1; continue; }
        if (strcmp(argv[i], "--build-timeout") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--build-timeout requires a value\n"); return 2; }
            build_timeout_sec = atoi(argv[++i]);
            if (build_timeout_sec < 0) build_timeout_sec = 0;
            continue;
        }
        if (strcmp(argv[i], "--run-timeout") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--run-timeout requires a value\n"); return 2; }
            run_timeout_sec = atoi(argv[++i]);
            if (run_timeout_sec < 0) run_timeout_sec = 0;
            continue;
        }
        if (strcmp(argv[i], "--filter") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--filter requires a value\n"); return 2; }
            filter = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--jobs") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--jobs requires a value\n"); return 2; }
            jobs = atoi(argv[++i]);
            if (jobs < 1) jobs = 1;
            continue;
        }
        usage(argv[0]);
        return 2;
    }

    if (!file_exists("./cc/bin/ccc")) {
        fprintf(stderr, "cc_test: missing ./cc/bin/ccc (build the compiler first)\n");
        return 2;
    }

    {
        const char* env = getenv("CC_TEST_USE_CACHE");
        if (env && strcmp(env, "1") == 0) use_cache = 1;
    }
    {
        const char* env = getenv("CC_TEST_CLEAN");
        if (env && strcmp(env, "1") == 0) clean = 1;
    }
    {
        const char* env = getenv("CC_TEST_BUILD_TIMEOUT");
        if (env && *env) {
            int t = atoi(env);
            if (t >= 0) build_timeout_sec = t;
        }
    }
    {
        const char* env = getenv("CC_TEST_RUN_TIMEOUT");
        if (env && *env) {
            int t = atoi(env);
            if (t >= 0) run_timeout_sec = t;
        }
    }

    (void)ensure_out_dir();
    (void)ensure_dir_p("bin");
    if (clean && !list_only) {
        // Best-effort: wipe per-test artifacts + incremental cache (matches what most devs want).
        (void)run_cmd_redirect_timeout("./cc/bin/ccc clean --all", NULL, NULL, verbose, 0);
    }

    DIR* d = opendir("tests");
    if (!d) {
        fprintf(stderr, "cc_test: failed to open tests/\n");
        return 2;
    }

    int ran = 0;
    int failed = 0;
    int running = 0;
    pid_t* pids = NULL;
    char** pid_names = NULL;
    int pid_cap = 0;

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        const char* name = ent->d_name;
        if (!name || name[0] == '.') continue;
        if (!(ends_with(name, ".c") || ends_with(name, ".ccs"))) continue;
        char stem[256];
        basename_no_ext(name, stem, sizeof(stem));
        if (!stem[0]) continue;

        if (test_requires_async(stem)) {
            const char* env = getenv("CC_ENABLE_ASYNC");
            if (!(env && strcmp(env, "1") == 0)) {
                if (verbose) fprintf(stderr, "[SKIP] %s (set CC_ENABLE_ASYNC=1)\n", stem);
                continue;
            }
        }

        char path[512];
        snprintf(path, sizeof(path), "tests/%s", name);
        if (filter && !str_contains(stem, filter) && !str_contains(path, filter)) continue;

        if (list_only) {
            printf("%s\n", path);
            continue;
        }

        int compile_fail = 0;
        char ce[512];
        snprintf(ce, sizeof(ce), "tests/%s.compile_err", stem);
        if (file_exists(ce)) compile_fail = 1;
        else if (ends_with(name, "_fail.ccs")) compile_fail = 1;

        ran++;
        if (jobs <= 1) {
            if (run_one_test(stem, path, compile_fail, verbose, "out", "bin", use_cache, build_timeout_sec, run_timeout_sec) != 0)
                failed++;
            continue;
        }

        while (running >= jobs) {
            pid_pop(pids, pid_names, &running, &failed);
        }

        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "cc_test: fork failed\n");
            return 2;
        }
        if (pid == 0) {
            pid_t me = getpid();
            char out_dir[512];
            char bin_dir[512];
            snprintf(out_dir, sizeof(out_dir), "out/.cc_test/%d", (int)me);
            snprintf(bin_dir, sizeof(bin_dir), "bin/.cc_test/%d", (int)me);
            (void)ensure_dir_p(out_dir);
            (void)ensure_dir_p(bin_dir);
            int rc = run_one_test(stem, path, compile_fail, verbose, out_dir, bin_dir, use_cache, build_timeout_sec, run_timeout_sec);
            _exit(rc == 0 ? 0 : 1);
        }

        if (running + 1 > pid_cap) {
            int nc = pid_cap ? pid_cap * 2 : 32;
            pid_t* np = (pid_t*)realloc(pids, (size_t)nc * sizeof(pid_t));
            char** nn = (char**)realloc(pid_names, (size_t)nc * sizeof(char*));
            if (!np || !nn) { fprintf(stderr, "cc_test: OOM\n"); return 2; }
            pids = np; pid_names = nn; pid_cap = nc;
        }
        pids[running] = pid;
        pid_names[running] = strdup(stem);
        running++;
    }
    closedir(d);

    while (running > 0) {
        pid_pop(pids, pid_names, &running, &failed);
    }
    for (int i = 0; i < pid_cap; ++i) {
        if (pid_names && pid_names[i]) { free(pid_names[i]); pid_names[i] = NULL; }
    }
    free(pids);
    free(pid_names);

    if (list_only) return 0;
    if (ran == 0) {
        fprintf(stderr, "cc_test: no tests selected\n");
        return 1;
    }
    if (failed) {
        fprintf(stderr, "cc_test: %d/%d failed\n", failed, ran);
        return 1;
    }
    fprintf(stderr, "cc_test: %d passed\n", ran);
    return 0;
}


