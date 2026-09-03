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
#if defined(_SC_NPROCESSORS_ONLN)
/* sysconf for default --jobs */
#endif

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

static int write_file_from_buf(const char* path, const unsigned char* buf, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    if (len) (void)fwrite(buf, 1, len, f);
    fclose(f);
    return 0;
}

static int read_entire_file_alloc(const char* path, unsigned char** out_buf, size_t* out_len);

static void dump_failure_stream(const char* label, const unsigned char* buf, size_t len) {
    const unsigned char* p = buf;
    size_t n = len;
    if (!buf || !len) return;
    fprintf(stderr, "----- %s -----\n", label);
    if (n > 8192) {
        p += n - 8192;
        n = 8192;
        fputs("... (truncated)\n", stderr);
    }
    fwrite(p, 1, n, stderr);
    if (n && p[n - 1] != '\n') fputc('\n', stderr);
}

static void log_failure_files(const char* stem,
                              const char* out_path,
                              const char* err_path,
                              const char* build_err_path) {
    if (!stem || !stem[0]) return;
    (void)ensure_dir_p("tmp/cc_test_logs");

    char dest[512];
    unsigned char* buf = NULL;
    size_t len = 0;

    if (out_path && read_entire_file_alloc(out_path, &buf, &len) == 0 && buf) {
        snprintf(dest, sizeof(dest), "tmp/cc_test_logs/%s.stdout.txt", stem);
        (void)write_file_from_buf(dest, buf, len);
        dump_failure_stream("stdout", buf, len);
    }
    free(buf); buf = NULL; len = 0;

    if (err_path && read_entire_file_alloc(err_path, &buf, &len) == 0 && buf) {
        snprintf(dest, sizeof(dest), "tmp/cc_test_logs/%s.stderr.txt", stem);
        (void)write_file_from_buf(dest, buf, len);
        dump_failure_stream("stderr", buf, len);
    }
    free(buf); buf = NULL; len = 0;

    if (build_err_path && read_entire_file_alloc(build_err_path, &buf, &len) == 0 && buf) {
        snprintf(dest, sizeof(dest), "tmp/cc_test_logs/%s.build.stderr.txt", stem);
        (void)write_file_from_buf(dest, buf, len);
        dump_failure_stream("build.stderr", buf, len);
    }
    free(buf);
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

static int g_cc_test_profile;

static int run_one_test(const char* stem, const char* input_path, int compile_fail,
                        int verbose,
                        const char* out_dir,
                        const char* bin_dir,
                        int use_cache,
                        int opt_o0,
                        int build_timeout_sec,
                        int run_timeout_sec);

static int run_one_test_maybe_profile(const char* stem, const char* input_path,
                                      int compile_fail, int verbose,
                                      const char* out_dir, const char* bin_dir,
                                      int use_cache, int opt_o0,
                                      int build_timeout_sec, int run_timeout_sec) {
    long long t0;
    int rc;
    if (!g_cc_test_profile)
        return run_one_test(stem, input_path, compile_fail, verbose, out_dir,
                            bin_dir, use_cache, opt_o0, build_timeout_sec,
                            run_timeout_sec);
    t0 = now_ms_monotonic();
    rc = run_one_test(stem, input_path, compile_fail, verbose, out_dir, bin_dir,
                      use_cache, opt_o0, build_timeout_sec, run_timeout_sec);
    fprintf(stderr, "[TIME] %s %lldms\n", stem, now_ms_monotonic() - t0);
    return rc;
}

// Runs command via one `sh -c` with optional timeout. Returns:
// - exit code of the command (0..255)
// - 124 on timeout (like GNU timeout)
// Optional in_path redirects stdin (`< in_path`) when non-NULL.
//
// Historically this nested `sh -c 'sh -c …'`, which doubled process overhead
// on every harness test.  Build the redirect script once and exec a single sh.
static int run_cmd_redirect_timeout(const char* cmd,
                                    const char* in_path,
                                    const char* out_path,
                                    const char* err_path,
                                    int verbose,
                                    int timeout_sec) {
    if (!cmd) return -1;
    char full[4096];
    if (in_path && in_path[0] && out_path && err_path) {
        snprintf(full, sizeof(full), "%s < %s > %s 2> %s",
                 cmd, in_path, out_path, err_path);
    } else if (in_path && in_path[0] && out_path) {
        snprintf(full, sizeof(full), "%s < %s > %s", cmd, in_path, out_path);
    } else if (in_path && in_path[0] && err_path) {
        snprintf(full, sizeof(full), "%s < %s 2> %s", cmd, in_path, err_path);
    } else if (out_path && err_path) {
        snprintf(full, sizeof(full), "%s > %s 2> %s", cmd, out_path, err_path);
    } else if (out_path) {
        snprintf(full, sizeof(full), "%s > %s", cmd, out_path);
    } else if (err_path) {
        snprintf(full, sizeof(full), "%s 2> %s", cmd, err_path);
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
        // 10ms polling contended under high --jobs; 50ms is plenty for suite.
        usleep(50 * 1000);
    }
}

static int default_job_count(void) {
    long n = 0;
#if defined(_SC_NPROCESSORS_ONLN)
    n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (n < 1) n = 6;
    if (n > 16) n = 16;
    return (int)n;
}

/* Parallel shadow_lower smokes (cc/shadow). Default harness skips
 * these — run scripts/test_shadow.sh (or CC_TEST_SHADOW=1 / --filter c_pp_). */
static int test_is_shadow(const char* stem) {
    if (!stem) return 0;
    return strncmp(stem, "c_pp_", 5) == 0;
}

/* Stress / lost-wake / race matrix tests: useful overnight, expensive in the
 * local edit loop.  Skipped by default (--quick); include with --full. */
static int test_is_heavy(const char* stem, const char* path) {
    if (!stem) return 0;
    if (str_contains(stem, "stress")) return 1;
    if (str_contains(stem, "lostwake")) return 1;
    if (str_contains(stem, "_race")) return 1;
    if (path && str_contains(path, "/stress")) return 1;
    return 0;
}

/* Track failed test names for summary */
static char** g_failed_names = NULL;
static int g_failed_count = 0;
static int g_failed_cap = 0;

static void add_failed_name(const char* name) {
    if (!name) return;
    if (g_failed_count >= g_failed_cap) {
        int nc = g_failed_cap ? g_failed_cap * 2 : 32;
        char** nn = (char**)realloc(g_failed_names, (size_t)nc * sizeof(char*));
        if (!nn) return;
        g_failed_names = nn;
        g_failed_cap = nc;
    }
    g_failed_names[g_failed_count++] = strdup(name);
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
        if (rc != 0) {
            (*io_failed)++;
            add_failed_name(names_local[idx]);
        }
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

/* Compute the directory portion of a path like "tests/macro/foo.ccs" →
 * "tests/macro".  Falls back to "tests" if there is no slash.  Output
 * fits in `out` (size `cap`); always NUL-terminated. */
static void test_dir_from_path(const char* path, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!path) return;
    const char* last = strrchr(path, '/');
    if (!last) {
        snprintf(out, cap, "tests");
        return;
    }
    size_t n = (size_t)(last - path);
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

static int test_requires_async(const char* test_dir, const char* stem) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s.requires_async", test_dir, stem);
    return file_exists(p);
}

/* Per-test environment sidecar support.
 *
 * A `tests/STEM.env` file is read line-by-line.  Each non-empty,
 * non-`#`-prefixed line is treated as `KEY=VALUE` and applied with
 * `setenv` before invoking the build step.  A snapshot of the previous
 * value is captured so we can restore the original environment after
 * the test completes, preventing cross-test pollution.
 *
 * The number of keys a test can set is capped; any reasonable test uses
 * one or two. */
#define CC_TEST_ENV_MAX 8

typedef struct {
    int   count;
    char* keys[CC_TEST_ENV_MAX];
    char* prev_values[CC_TEST_ENV_MAX]; /* NULL means unset */
    int   had_prev[CC_TEST_ENV_MAX];
} EnvSidecar;

static void env_sidecar_clear(EnvSidecar* e) {
    if (!e) return;
    for (int i = 0; i < e->count; i++) {
        free(e->keys[i]);
        free(e->prev_values[i]);
        e->keys[i] = NULL;
        e->prev_values[i] = NULL;
        e->had_prev[i] = 0;
    }
    e->count = 0;
}

static void env_sidecar_apply(const char* test_dir, const char* stem, EnvSidecar* e) {
    if (!e) return;
    e->count = 0;
    if (!stem || !stem[0]) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.env", test_dir, stem);
    unsigned char* buf = NULL;
    size_t len = 0;
    if (read_entire_file_alloc(path, &buf, &len) != 0 || !buf) {
        free(buf);
        return;
    }
    size_t i = 0;
    while (i < len && e->count < CC_TEST_ENV_MAX) {
        size_t ls = i;
        while (i < len && buf[i] != '\n') i++;
        size_t le = i;
        if (i < len && buf[i] == '\n') i++;
        if (le > ls && buf[le - 1] == '\r') le--;
        size_t p = ls;
        while (p < le && (buf[p] == ' ' || buf[p] == '\t')) p++;
        if (p >= le) continue;
        if (buf[p] == '#') continue;
        size_t eq = p;
        while (eq < le && buf[eq] != '=') eq++;
        if (eq >= le) continue;
        size_t ke = eq;
        while (ke > p && (buf[ke - 1] == ' ' || buf[ke - 1] == '\t')) ke--;
        if (ke <= p) continue;
        size_t vs = eq + 1;
        while (vs < le && (buf[vs] == ' ' || buf[vs] == '\t')) vs++;
        size_t ve = le;
        while (ve > vs && (buf[ve - 1] == ' ' || buf[ve - 1] == '\t')) ve--;
        char key[128];
        char val[1024];
        size_t kl = ke - p;
        size_t vl = ve - vs;
        if (kl >= sizeof(key)) kl = sizeof(key) - 1;
        if (vl >= sizeof(val)) vl = sizeof(val) - 1;
        memcpy(key, buf + p, kl); key[kl] = 0;
        memcpy(val, buf + vs, vl); val[vl] = 0;
        const char* cur = getenv(key);
        e->keys[e->count] = strdup(key);
        e->had_prev[e->count] = cur ? 1 : 0;
        e->prev_values[e->count] = cur ? strdup(cur) : NULL;
        setenv(key, val, 1);
        e->count++;
    }
    free(buf);
}

static void env_sidecar_restore(EnvSidecar* e) {
    if (!e) return;
    for (int i = 0; i < e->count; i++) {
        if (e->had_prev[i] && e->prev_values[i]) {
            setenv(e->keys[i], e->prev_values[i], 1);
        } else {
            unsetenv(e->keys[i]);
        }
    }
    env_sidecar_clear(e);
}

/* `.shcc` quote/cch smokes shell out to several cold `ccc --emit-c-only`
 * passes plus a `--no-cache` build. ~3s alone; exceeds the 10s default under
 * `--jobs` contention (TIMEOUT, not a wrong oracle). */
static int nested_quote_cch_run_timeout(const char* stem) {
    if (!stem) return 0;
    if (strncmp(stem, "quote_cch_", 10) == 0) return 30;
    if (strcmp(stem, "cch_face_chapter_smoke") == 0) return 30;
    return 0;
}

static int get_run_timeout_for_test(const char* stem, int default_timeout_sec) {
    if (!stem) return default_timeout_sec;
    {
        int t = nested_quote_cch_run_timeout(stem);
        if (t) return t;
    }
    if (strcmp(stem, "chan_park_wake_lostwake_stress_smoke") == 0) return 20;
    /* Inner `cc` + `ccc build --release` + three weekend-image runs. The
     * 10s default trips under --jobs contention (TIMEOUT, not a wrong
     * checksum). */
    if (strcmp(stem, "raytracer_weekend_smoke") == 0) return 60;
    /* Emits both Redis variants; ~7s each under -O0 toolchains. */
    if (strcmp(stem, "redis_phase2_lowering_shape_smoke") == 0) return 30;
    /* Three emit-c-only passes of mut-walk fixtures. */
    if (strcmp(stem, "for_in_mut_walk_peel_smoke") == 0) return 20;
    /* Many nested ccc -e/-E subprocesses; ~11s alone, can exceed 30s under
     * --jobs contention (each child competes for CPU with the suite). */
    if (strcmp(stem, "script_oneliner_smoke") == 0) return 60;
    /* Compact goldens + hostcc + one header beachhead; keep near default. */
    if (strcmp(stem, "c_pp_shadow_emit_smoke") == 0) return 20;
    /* Each shells out to `ccc build` of a py.cch TU: ~9s of backend -O2 on
     * a cold cache, over the 10s default under suite parallelism. */
    if (strcmp(stem, "py_module_import_smoke") == 0) return 30;
    if (strcmp(stem, "py_module_double_result_smoke") == 0) return 30;
    /* Same shape as the py module smokes: shells out to `ccc build` of a
     * js.cch TU, then a node import run. */
    if (strcmp(stem, "js_module_import_smoke") == 0) return 30;
    if (strcmp(stem, "js_module_double_result_smoke") == 0) return 30;
    if (strcmp(stem, "js_module_multi_export_smoke") == 0) return 30;
    /* Inner `ccc build` of the hosting TU, plus a first-use C++ compile
     * of the embedded libnode shim on a cold cache. */
    if (strcmp(stem, "js_host_smoke") == 0) return 60;
    /* Inner `ccc build`, then four node children spawn and one is
     * crashed on purpose. */
    if (strcmp(stem, "js_dom_smoke") == 0) return 60;
    /* Inner `ccc build` plus a first-use shim compile on a cold cache. */
    if (strcmp(stem, "js_dom_hosted_smoke") == 0) return 60;
    if (strcmp(stem, "js_dom_ta_smoke") == 0) return 60;
    if (strcmp(stem, "js_dom_cb_smoke") == 0) return 60;
    /* Three inner `ccc build`s (dual + two narrowing runs) + both hosts. */
    if (strcmp(stem, "dual_module_export_smoke") == 0) return 60;
    /* One dual multiclass `ccc build`, then a node run and a python run. */
    if (strcmp(stem, "dual_multi_export_smoke") == 0) return 60;
    /* Builds the counter addon, then spawns a worker realm. */
    if (strcmp(stem, "js_module_realm_smoke") == 0) return 60;
    /* Inner `ccc build` of the js.cch+py.cch bridge TU, then a node run. */
    if (strcmp(stem, "cc_python_bridge_smoke") == 0) return 60;
    /* Rebuilds the addon, then the articulate-error pack (in-proc + isolated). */
    if (strcmp(stem, "cc_python_bridge_neg_smoke") == 0) return 120;
    /* Same build plus ~30k bridge crossings and GC rounds under load. */
    if (strcmp(stem, "cc_python_bridge_mem_smoke") == 0) return 120;
    /* Bridge build + ~1.5s of deliberate Python sleeps across domains. */
    if (strcmp(stem, "cc_python_bridge_async_smoke") == 0) return 120;
    /* Bridge build + callback round-trips across two domains. */
    if (strcmp(stem, "cc_python_bridge_cb_smoke") == 0) return 120;
    if (strcmp(stem, "cc_python_bridge_await_smoke") == 0) return 120;
    if (strcmp(stem, "cc_python_bridge_aio_smoke") == 0) return 120;
    /* Creates venvs and boots several child interpreters. */
    if (strcmp(stem, "cc_python_bridge_env_smoke") == 0) return 120;
    /* Spawns python children per domain (numpy import each) + a venv. */
    if (strcmp(stem, "cc_python_bridge_proc_smoke") == 0) return 120;
    /* Spawns node children per domain; ~20ms deliberate async waits. */
    if (strcmp(stem, "cc_node_bridge_smoke") == 0) return 60;
    /* Spawns a python child; forged-reply and pairing rungs. */
    if (strcmp(stem, "bridge_wire_smoke") == 0) return 60;
    if (strcmp(stem, "py_module_kwargs_smoke") == 0) return 30;
    /* Inner `ccc build` of DP-only levenshtein_cc_smoke.ccs. */
    if (strcmp(stem, "py_proc_isolate_smoke") == 0) return 60;
    if (strcmp(stem, "py_clone_into_smoke") == 0) return 30;
    if (strcmp(stem, "py_home_cross_refuse_smoke") == 0) return 30;
    if (strcmp(stem, "py_ull_inbound_smoke") == 0) return 30;
    if (strcmp(stem, "py_as_list_bounds_smoke") == 0) return 30;
    if (strcmp(stem, "py_name_cache_reuse_smoke") == 0) return 30;
    if (strcmp(stem, "py_levenshtein_smoke") == 0) return 60;
    return default_timeout_sec;
}

/* Optional `stem.args`: one argv line per run (blank = no args; `#` comments
 * skipped).  Matching `stem.stdout` / `stem.stderr` may split sections on a
 * line that is exactly `---`.  No `.args` → single run, whole sidecar as today.
 * Optional `stem.exit`: expected process exit code(s). One integer → all runs;
 * N integers (one per line) → per-run. Missing → expect 0. */
#define CC_TEST_MAX_RUNS 16
#define CC_TEST_MAX_SECTIONS 16

typedef struct {
    char* lines[CC_TEST_MAX_RUNS];
    int n;
} ArgRuns;

typedef struct {
    int codes[CC_TEST_MAX_RUNS];
    int n;
} ExitExpects;

typedef struct {
    const unsigned char* ptr[CC_TEST_MAX_SECTIONS];
    size_t len[CC_TEST_MAX_SECTIONS];
    int n;
} ExpSections;

static void arg_runs_clear(ArgRuns* a) {
    if (!a) return;
    for (int i = 0; i < a->n; ++i) free(a->lines[i]);
    a->n = 0;
}

static int exit_expects_load(const char* path, ExitExpects* out) {
    if (!out) return -1;
    out->n = 0;
    unsigned char* buf = NULL;
    size_t len = 0;
    if (read_entire_file_alloc(path, &buf, &len) != 0 || !buf) return 0;
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && buf[i] != '\n') i++;
        size_t end = i;
        if (i < len && buf[i] == '\n') i++;
        while (start < end && (buf[start] == ' ' || buf[start] == '\t')) start++;
        while (end > start && (buf[end - 1] == ' ' || buf[end - 1] == '\t' ||
                               buf[end - 1] == '\r'))
            end--;
        if (start == end) continue;
        if (buf[start] == '#') continue;
        if (out->n >= CC_TEST_MAX_RUNS) {
            fprintf(stderr, "cc_test: too many exit codes in %s\n", path);
            free(buf);
            out->n = 0;
            return -1;
        }
        char tmp[64];
        size_t n = end - start;
        if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
        memcpy(tmp, buf + start, n);
        tmp[n] = '\0';
        out->codes[out->n++] = atoi(tmp);
    }
    free(buf);
    return 0;
}

static int exit_expects_for_run(const ExitExpects* e, int ri) {
    if (!e || e->n <= 0) return 0;
    if (e->n == 1) return e->codes[0];
    if (ri >= 0 && ri < e->n) return e->codes[ri];
    return 0;
}

static int arg_runs_load(const char* path, ArgRuns* out) {
    if (!out) return -1;
    out->n = 0;
    unsigned char* buf = NULL;
    size_t len = 0;
    if (read_entire_file_alloc(path, &buf, &len) != 0 || !buf) return 0;
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && buf[i] != '\n') i++;
        size_t end = i;
        if (i < len && buf[i] == '\n') i++;
        while (start < end && (buf[start] == ' ' || buf[start] == '\t')) start++;
        while (end > start && (buf[end - 1] == ' ' || buf[end - 1] == '\t' ||
                               buf[end - 1] == '\r'))
            end--;
        if (start == end) {
            /* blank line = run with no args */
        } else if (buf[start] == '#') {
            continue;
        }
        if (out->n >= CC_TEST_MAX_RUNS) {
            fprintf(stderr, "cc_test: too many runs in %s\n", path);
            free(buf);
            arg_runs_clear(out);
            return -1;
        }
        size_t n = end - start;
        char* line = (char*)malloc(n + 1);
        if (!line) {
            free(buf);
            arg_runs_clear(out);
            return -1;
        }
        if (n) memcpy(line, buf + start, n);
        line[n] = '\0';
        out->lines[out->n++] = line;
    }
    free(buf);
    return 0;
}

static void exp_sections_split(const unsigned char* buf, size_t len, ExpSections* out) {
    out->n = 0;
    if (!buf || len == 0) return;
    size_t i = 0;
    size_t sec_start = 0;
    while (i <= len) {
        size_t line_start = i;
        while (i < len && buf[i] != '\n') i++;
        size_t line_end = i;
        int at_eof = (i >= len);
        if (i < len && buf[i] == '\n') i++;

        int is_sep = 0;
        if (line_end > line_start) {
            size_t a = line_start, b = line_end;
            if (b > a && buf[b - 1] == '\r') b--;
            if (b - a == 3 && buf[a] == '-' && buf[a + 1] == '-' && buf[a + 2] == '-')
                is_sep = 1;
        }

        if (is_sep || at_eof) {
            size_t sec_end = is_sep ? line_start : len;
            if (out->n < CC_TEST_MAX_SECTIONS) {
                out->ptr[out->n] = buf + sec_start;
                out->len[out->n] = sec_end >= sec_start ? sec_end - sec_start : 0;
                out->n++;
            }
            sec_start = i;
            if (at_eof) break;
        }
    }
    if (out->n == 0) {
        out->ptr[0] = buf;
        out->len[0] = len;
        out->n = 1;
    }
}

static int run_one_test(const char* stem,
                        const char* input_path,
                        int compile_fail,
                        int verbose,
                        const char* out_dir,
                        const char* bin_dir,
                        int use_cache,
                        int opt_o0,
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

    /* Sidecars live alongside the test file (e.g. `tests/macro/foo.env`
     * for `tests/macro/foo.ccs`).  Derived from input_path. */
    char test_dir[512];
    test_dir_from_path(input_path, test_dir, sizeof(test_dir));
    char exp_stdout_path[512], exp_stderr_path[512], exp_compile_err_path[512], exp_build_stderr_path[512], ldflags_path[512], args_path[512], stdin_path[512], exit_path[512];
    snprintf(exp_stdout_path, sizeof(exp_stdout_path), "%s/%s.stdout", test_dir, stem);
    snprintf(exp_stderr_path, sizeof(exp_stderr_path), "%s/%s.stderr", test_dir, stem);
    snprintf(exp_compile_err_path, sizeof(exp_compile_err_path), "%s/%s.compile_err", test_dir, stem);
    snprintf(exp_build_stderr_path, sizeof(exp_build_stderr_path), "%s/%s.build_stderr", test_dir, stem);
    snprintf(ldflags_path, sizeof(ldflags_path), "%s/%s.ldflags", test_dir, stem);
    snprintf(args_path, sizeof(args_path), "%s/%s.args", test_dir, stem);
    snprintf(stdin_path, sizeof(stdin_path), "%s/%s.stdin", test_dir, stem);
    snprintf(exit_path, sizeof(exit_path), "%s/%s.exit", test_dir, stem);
    /* Default /dev/null so tests that call read_all()/scanf cannot hang when
     * the harness inherits an open pipe/TTY (parallel make, CI). Explicit
     * `stem.stdin` still wins. */
    const char* run_stdin = file_exists(stdin_path) ? stdin_path : "/dev/null";

    unsigned char *exp_stdout = NULL, *exp_stderr = NULL, *exp_compile_err = NULL, *exp_build_stderr = NULL, *ldflags = NULL;
    size_t exp_stdout_len = 0, exp_stderr_len = 0, exp_compile_err_len = 0, exp_build_stderr_len = 0, ldflags_len = 0;
    (void)read_entire_file_alloc(exp_stdout_path, &exp_stdout, &exp_stdout_len);
    (void)read_entire_file_alloc(exp_stderr_path, &exp_stderr, &exp_stderr_len);
    (void)read_entire_file_alloc(exp_compile_err_path, &exp_compile_err, &exp_compile_err_len);
    (void)read_entire_file_alloc(exp_build_stderr_path, &exp_build_stderr, &exp_build_stderr_len);
    (void)read_entire_file_alloc(ldflags_path, &ldflags, &ldflags_len);

    ArgRuns runs = {0};
    ExitExpects exits = {0};
    if (arg_runs_load(args_path, &runs) != 0) {
        free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(exp_build_stderr); free(ldflags);
        return 1;
    }
    if (exit_expects_load(exit_path, &exits) != 0) {
        arg_runs_clear(&runs);
        free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(exp_build_stderr); free(ldflags);
        return 1;
    }
    if (runs.n > 0 && exits.n > 1 && exits.n != runs.n) {
        fprintf(stderr, "[FAIL] %s: .args has %d runs but .exit has %d codes\n",
                stem, runs.n, exits.n);
        arg_runs_clear(&runs);
        free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(exp_build_stderr); free(ldflags);
        return 1;
    }

    char ldflags_clean[1024];
    ldflags_clean[0] = '\0';
    if (ldflags && ldflags_len) {
        size_t n = ldflags_len < sizeof(ldflags_clean) - 1 ? ldflags_len : sizeof(ldflags_clean) - 1;
        memcpy(ldflags_clean, ldflags, n);
        ldflags_clean[n] = '\0';
        replace_newlines_with_spaces(ldflags_clean);
        trim_trailing_ws_inplace(ldflags_clean);
    }

    /* 1) Build via ccc build (this is the build system under test).
     * ccc is native-only (shadow_lower). Emit cache is keyed by source bytes +
     * toolchain bytes; warm hits replay lowering diagnostics from emit.c.diag.
     * Erroring emits are not cached. Do not force --no-cache for diag/fail
     * tests — that papers over cache bugs. */
    char build_cmd[3072];
    const char* cache_flag = use_cache ? "" : "--no-cache ";
    /* Append after ccc's default -O2 so the host sees `-O2 -O0` and last wins.
     * Space form required: `--cc-flags=-O0` is parsed as an input path. */
    /* ILP32: host bins need 64-bit off_t for readdir on Docker volumes. */
#if defined(__linux__) && defined(__SIZEOF_POINTER__) && (__SIZEOF_POINTER__ == 4)
    const char* o0_flag = opt_o0 ? "--cc-flags -O0 -D_FILE_OFFSET_BITS=64 "
                                 : "--cc-flags -D_FILE_OFFSET_BITS=64 ";
#else
    const char* o0_flag = opt_o0 ? "--cc-flags -O0 " : "";
#endif
    if (ldflags_clean[0]) {
        snprintf(build_cmd, sizeof(build_cmd),
                 "./cc/bin/ccc build %s%s--out-dir %s --bin-dir %s --link %s -o %s --ld-flags \"%s\"",
                 cache_flag, o0_flag,
                 (out_dir && out_dir[0]) ? out_dir : "out",
                 (bin_dir && bin_dir[0]) ? bin_dir : "bin",
                 input_path, bin_out, ldflags_clean);
    } else {
        snprintf(build_cmd, sizeof(build_cmd),
                 "./cc/bin/ccc build %s%s--out-dir %s --bin-dir %s --link %s -o %s",
                 cache_flag, o0_flag,
                 (out_dir && out_dir[0]) ? out_dir : "out",
                 (bin_dir && bin_dir[0]) ? bin_dir : "bin",
                 input_path, bin_out);
    }

    EnvSidecar envsc = {0};
    env_sidecar_apply(test_dir, stem, &envsc);

    int build_rc = run_cmd_redirect_timeout(build_cmd, NULL, NULL, build_err_txt, verbose, build_timeout_sec);

    /* Restore process env now — the build subprocess has completed and
     * already observed the sidecar values.  Running the compiled binary
     * below should not inherit CC_*-style sidecar knobs. */
    env_sidecar_restore(&envsc);

    if (compile_fail) {
        if (build_rc == 0) {
            fprintf(stderr, "[FAIL] %s: expected build to fail\n", stem);
            arg_runs_clear(&runs);
            free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(exp_build_stderr); free(ldflags);
            return 1;
        }
        if (build_rc == 124) {
            fprintf(stderr, "[TIMEOUT] %s: build timed out after %ds\n", stem, build_timeout_sec);
            log_failure_files(stem, out_txt, err_txt, build_err_txt);
            arg_runs_clear(&runs);
            free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(exp_build_stderr); free(ldflags);
            return 1;
        }
        unsigned char* err_buf = NULL;
        size_t err_len = 0;
        (void)read_entire_file_alloc(build_err_txt, &err_buf, &err_len);
        int bad = expect_contains_lines("compile_err", err_buf, err_len, exp_compile_err, exp_compile_err_len);
        free(err_buf);
        arg_runs_clear(&runs);
        free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(exp_build_stderr); free(ldflags);
        if (bad) return 1;
        fprintf(stderr, "[OK] %s\n", stem);
        return 0;
    }

    if (build_rc != 0) {
        if (build_rc == 124) {
            fprintf(stderr, "[TIMEOUT] %s: build timed out after %ds\n", stem, build_timeout_sec);
        }
        fprintf(stderr, "[FAIL] %s: build failed\n", stem);
        log_failure_files(stem, out_txt, err_txt, build_err_txt);
        arg_runs_clear(&runs);
        free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(exp_build_stderr); free(ldflags);
        return 1;
    }

    /* Compile-time diagnostics on successful builds:
     * - `.build_stderr` pin: require those needles (intentional warnings).
     * - `*_smoke` without a pin: build stderr must contain no `warning:`. */
    {
        unsigned char* build_err_buf = NULL;
        size_t build_err_len = 0;
        (void)read_entire_file_alloc(build_err_txt, &build_err_buf, &build_err_len);
        if (exp_build_stderr && exp_build_stderr_len) {
            if (expect_contains_lines("build_stderr", build_err_buf, build_err_len,
                                      exp_build_stderr, exp_build_stderr_len) != 0) {
                log_failure_files(stem, out_txt, err_txt, build_err_txt);
                free(build_err_buf);
                arg_runs_clear(&runs);
                free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(exp_build_stderr); free(ldflags);
                return 1;
            }
        } else if (ends_with(stem, "_smoke") && build_err_buf && build_err_len >= 8) {
            const char* p = (const char*)build_err_buf;
            size_t i;
            int saw_warn = 0;
            for (i = 0; i + 8 <= build_err_len; i++) {
                if (strncmp(p + i, "warning:", 8) == 0) {
                    saw_warn = 1;
                    break;
                }
            }
            if (saw_warn) {
                fprintf(stderr,
                        "[FAIL] %s: smoke build produced warning(s); "
                        "fix them or pin expected needles in %s.build_stderr\n",
                        stem, stem);
                log_failure_files(stem, out_txt, err_txt, build_err_txt);
                free(build_err_buf);
                arg_runs_clear(&runs);
                free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(exp_build_stderr); free(ldflags);
                return 1;
            }
        }
        free(build_err_buf);
    }

    int nruns = runs.n > 0 ? runs.n : 1;
    ExpSections out_secs = {0}, err_secs = {0};
    exp_sections_split(exp_stdout, exp_stdout_len, &out_secs);
    exp_sections_split(exp_stderr, exp_stderr_len, &err_secs);
    if (runs.n > 0 && out_secs.n > 1 && out_secs.n != nruns) {
        fprintf(stderr, "[FAIL] %s: .args has %d runs but .stdout has %d --- sections\n",
                stem, nruns, out_secs.n);
        arg_runs_clear(&runs);
        free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(exp_build_stderr); free(ldflags);
        return 1;
    }
    if (runs.n > 0 && err_secs.n > 1 && err_secs.n != nruns) {
        fprintf(stderr, "[FAIL] %s: .args has %d runs but .stderr has %d --- sections\n",
                stem, nruns, err_secs.n);
        arg_runs_clear(&runs);
        free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(exp_build_stderr); free(ldflags);
        return 1;
    }

    int test_run_timeout_sec = get_run_timeout_for_test(stem, run_timeout_sec);
    int bad = 0;
    for (int ri = 0; ri < nruns; ++ri) {
        char run_cmd[2048];
        const char* args = (runs.n > 0) ? runs.lines[ri] : "";
        if (args && args[0]) {
            snprintf(run_cmd, sizeof(run_cmd), "%s %s", bin_out, args);
        } else {
            snprintf(run_cmd, sizeof(run_cmd), "%s", bin_out);
        }

        int want_exit = exit_expects_for_run(&exits, ri);
        int run_rc = run_cmd_redirect_timeout(run_cmd, run_stdin, out_txt, err_txt, verbose,
                                              test_run_timeout_sec);
        if (run_rc == 124) {
            fprintf(stderr, "[TIMEOUT] %s: run timed out after %ds\n", stem, test_run_timeout_sec);
            log_failure_files(stem, out_txt, err_txt, build_err_txt);
            arg_runs_clear(&runs);
            free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(exp_build_stderr); free(ldflags);
            return 1;
        }
        if (run_rc != want_exit) {
            fprintf(stderr, "[FAIL] %s: exit %d (expected %d)", stem, run_rc, want_exit);
            if (nruns > 1) fprintf(stderr, " (case %d/%d)", ri + 1, nruns);
            fprintf(stderr, "\n");
            log_failure_files(stem, out_txt, err_txt, build_err_txt);
            arg_runs_clear(&runs);
            free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(exp_build_stderr); free(ldflags);
            return 1;
        }

        unsigned char *out_buf = NULL, *err_buf = NULL;
        size_t out_len = 0, err_len = 0;
        (void)read_entire_file_alloc(out_txt, &out_buf, &out_len);
        (void)read_entire_file_alloc(err_txt, &err_buf, &err_len);

        /* Optional-dep smokes print `SKIP (…)` and exit 0. Honor that even
         * when a success-path `.stdout` oracle is present. */
        {
            size_t i = 0;
            while (i < out_len &&
                   (out_buf[i] == ' ' || out_buf[i] == '\t' || out_buf[i] == '\r' ||
                    out_buf[i] == '\n'))
                i++;
            if (i + 5 <= out_len && memcmp(out_buf + i, "SKIP ", 5) == 0) {
                if (verbose) {
                    size_t eol = i;
                    while (eol < out_len && out_buf[eol] != '\n') eol++;
                    fprintf(stderr, "[SKIP] %s: %.*s\n", stem, (int)(eol - i),
                            (const char*)out_buf + i);
                }
                free(out_buf);
                free(err_buf);
                continue;
            }
        }

        const unsigned char* want_out = NULL;
        size_t want_out_len = 0;
        const unsigned char* want_err = NULL;
        size_t want_err_len = 0;
        if (out_secs.n == 0) {
            /* no expectations */
        } else if (out_secs.n == 1) {
            want_out = out_secs.ptr[0];
            want_out_len = out_secs.len[0];
        } else {
            want_out = out_secs.ptr[ri];
            want_out_len = out_secs.len[ri];
        }
        if (err_secs.n == 0) {
        } else if (err_secs.n == 1) {
            want_err = err_secs.ptr[0];
            want_err_len = err_secs.len[0];
        } else {
            want_err = err_secs.ptr[ri];
            want_err_len = err_secs.len[ri];
        }

        if (expect_contains_lines("stdout", out_buf, out_len, want_out, want_out_len) != 0 ||
            expect_contains_lines("stderr", err_buf, err_len, want_err, want_err_len) != 0) {
            if (nruns > 1) {
                fprintf(stderr, "[FAIL] %s: output mismatch (case %d/%d)\n", stem, ri + 1, nruns);
            }
            bad = 1;
        }
        free(out_buf);
        free(err_buf);
        if (bad) {
            log_failure_files(stem, out_txt, err_txt, build_err_txt);
            arg_runs_clear(&runs);
            free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(exp_build_stderr); free(ldflags);
            return 1;
        }
    }

    arg_runs_clear(&runs);
    free(exp_stdout); free(exp_stderr); free(exp_compile_err); free(exp_build_stderr); free(ldflags);
    fprintf(stderr, "[OK] %s\n", stem);
    return 0;
}

static void usage(const char* prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s [--list] [--filter SUBSTR] [--quick|--full] [--verbose] [--jobs N]\n", prog);
    fprintf(stderr, "       [--build-timeout SECONDS] [--run-timeout SECONDS]\n");
    fprintf(stderr, "       [--use-cache|--no-cache] [--O0] [--clean]\n");
    fprintf(stderr, "  --quick  skip stress/lostwake/race tests (default)\n");
    fprintf(stderr, "  --full   include stress/lostwake/race tests (also CC_TEST_FULL=1)\n");
    fprintf(stderr, "  --O0     host-compile test bins with -O0 (faster cold builds; also CC_TEST_O0=1)\n");
    fprintf(stderr, "  c_pp_*   shadow_lower smokes skipped unless CC_TEST_SHADOW=1 or --filter c_pp_\n");
}

int main(int argc, char** argv) {
    const char* filter = NULL;
    int verbose = 0;
    int list_only = 0;
    int jobs = default_job_count();
    int quick = 1; /* default: skip heavy stress/race; --full for the complete set */
    int use_cache = 1; /* default on; cold runs reuse shared concurrent_c.o / .c outs */
    int opt_o0 = 0;    /* host -O0 for faster cold compiles (default stays ccc -O2) */
    int clean = 0;
    int build_timeout_sec = 300;
    int run_timeout_sec = 10;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--verbose") == 0) { verbose = 1; continue; }
        if (strcmp(argv[i], "--list") == 0) { list_only = 1; continue; }
        if (strcmp(argv[i], "--quick") == 0) { quick = 1; continue; }
        if (strcmp(argv[i], "--full") == 0) { quick = 0; continue; }
        if (strcmp(argv[i], "--use-cache") == 0) { use_cache = 1; continue; }
        if (strcmp(argv[i], "--no-cache") == 0) { use_cache = 0; continue; }
        if (strcmp(argv[i], "--O0") == 0 || strcmp(argv[i], "-O0") == 0) {
            opt_o0 = 1;
            continue;
        }
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
            if (jobs > 64) jobs = 64;
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
        if (env && strcmp(env, "0") == 0) use_cache = 0;
    }
    {
        const char* env = getenv("CC_TEST_NO_CACHE");
        if (env && strcmp(env, "1") == 0) use_cache = 0;
    }
    {
        const char* env = getenv("CC_TEST_O0");
        if (env && (strcmp(env, "1") == 0 || strcmp(env, "yes") == 0 ||
                    strcmp(env, "true") == 0))
            opt_o0 = 1;
        if (env && (strcmp(env, "0") == 0 || strcmp(env, "no") == 0 ||
                    strcmp(env, "false") == 0))
            opt_o0 = 0;
    }
    {
        const char* env = getenv("CC_TEST_FULL");
        if (env && strcmp(env, "1") == 0) quick = 0;
    }
    {
        const char* env = getenv("CC_TEST_QUICK");
        if (env && strcmp(env, "1") == 0) quick = 1;
        if (env && strcmp(env, "0") == 0) quick = 0;
    }
    {
        const char* env = getenv("CC_TEST_JOBS");
        if (env && *env) {
            int t = atoi(env);
            if (t >= 1) jobs = t;
            if (jobs > 64) jobs = 64;
        }
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

    {
        const char* env = getenv("CC_TEST_PROFILE");
        if (env && env[0] && strcmp(env, "0") != 0) g_cc_test_profile = 1;
    }

    (void)ensure_out_dir();
    (void)ensure_dir_p("bin");
    if (clean && !list_only) {
        // Best-effort: wipe per-test artifacts + incremental cache (NOT --all which would delete the compiler).
        (void)run_cmd_redirect_timeout("./cc/bin/ccc clean", NULL, NULL, NULL, verbose, 0);
    }

    /* Collect all .c / .ccs / .shcc files under tests/ recursively.
     * Sibling config files (`tests/<stem>.env`, `.stdout`, `.stdin`, `.args`,
     * `.compile_err`, etc.) are still keyed by bare stem in the top-level
     * tests/ dir for back-compat; subdir tests must therefore have stems
     * unique across the whole tree. */
    char** all_paths = NULL;
    int    all_n = 0;
    int    all_cap = 0;
    {
        /* Iterative DFS using an explicit stack of directory paths.
         * Keeps memory bounded and avoids C-stack growth.  Skips dirs
         * starting with '.' (e.g. `.cc_test/`). */
        char** dstack = NULL;
        int    dstack_n = 0;
        int    dstack_cap = 0;
        #define PUSH_DIR(p) do { \
            if (dstack_n == dstack_cap) { \
                int nc = dstack_cap ? dstack_cap * 2 : 16; \
                char** nd = (char**)realloc(dstack, (size_t)nc * sizeof(char*)); \
                if (!nd) { fprintf(stderr, "cc_test: OOM\n"); return 2; } \
                dstack = nd; dstack_cap = nc; \
            } \
            dstack[dstack_n++] = strdup((p)); \
        } while (0)
        PUSH_DIR("tests");
        while (dstack_n > 0) {
            char* dir = dstack[--dstack_n];
            DIR* d = opendir(dir);
            if (!d) { free(dir); continue; }
            struct dirent* ent;
            while ((ent = readdir(d)) != NULL) {
                const char* nm = ent->d_name;
                if (!nm || nm[0] == '.') continue;
                char p[768];
                snprintf(p, sizeof(p), "%s/%s", dir, nm);
                struct stat st;
                if (stat(p, &st) != 0) continue;
                if (S_ISDIR(st.st_mode)) {
                    /* cparse-dump fixtures, not ccc programs. */
                    if (strcmp(nm, "cparse") == 0) continue;
                    PUSH_DIR(p);
                    continue;
                }
                if (!(ends_with(nm, ".c") || ends_with(nm, ".ccs") ||
                      ends_with(nm, ".shcc")))
                    continue;
                /* `<name>_mod.ccs` is a paired smoke's module fixture (an
                 * extension-module TU with no main), not a test — the
                 * owning smoke builds and exercises it. */
                if (ends_with(nm, "_mod.ccs"))
                    continue;
                /* Owner/guest fixtures for static_inline_impl_extract_smoke
                 * — no standalone main; guest needs the owner in the link set. */
                if (strcmp(nm, "static_inline_impl_extract.ccs") == 0 ||
                    strcmp(nm, "static_inline_impl_extract_guest.ccs") == 0)
                    continue;
                if (all_n == all_cap) {
                    int nc = all_cap ? all_cap * 2 : 64;
                    char** np = (char**)realloc(all_paths, (size_t)nc * sizeof(char*));
                    if (!np) { fprintf(stderr, "cc_test: OOM\n"); return 2; }
                    all_paths = np; all_cap = nc;
                }
                all_paths[all_n++] = strdup(p);
            }
            closedir(d);
            free(dir);
        }
        #undef PUSH_DIR
        free(dstack);
    }

    int ran = 0;
    int failed = 0;
    int running = 0;
    pid_t* pids = NULL;
    char** pid_names = NULL;
    int pid_cap = 0;

    for (int ai = 0; ai < all_n; ai++) {
        const char* path = all_paths[ai];
        /* basename for stem extraction */
        const char* name = strrchr(path, '/');
        name = name ? name + 1 : path;
        char stem[256];
        basename_no_ext(name, stem, sizeof(stem));
        if (!stem[0]) continue;

        char tdir[512];
        test_dir_from_path(path, tdir, sizeof(tdir));

        if (test_requires_async(tdir, stem)) {
            const char* env = getenv("CC_ENABLE_ASYNC");
            if (!(env && strcmp(env, "1") == 0)) {
                if (verbose) fprintf(stderr, "[SKIP] %s (set CC_ENABLE_ASYNC=1)\n", stem);
                continue;
            }
        }

        if (filter && !str_contains(stem, filter) && !str_contains(path, filter)) continue;
        /* Default suite = production ccc. c_pp_* only with opt-in or filter. */
        {
            const char* shadow = getenv("CC_TEST_SHADOW");
            int want_shadow = (shadow && strcmp(shadow, "1") == 0) || filter != NULL;
            if (test_is_shadow(stem) && !want_shadow) {
                if (verbose) fprintf(stderr, "[SKIP] %s (shadow: scripts/test_shadow.sh)\n", stem);
                continue;
            }
        }
        if (quick && test_is_heavy(stem, path)) {
            if (verbose) fprintf(stderr, "[SKIP] %s (quick: stress/race)\n", stem);
            continue;
        }

        if (list_only) {
            printf("%s\n", path);
            continue;
        }

        int compile_fail = 0;
        char ce[512];
        snprintf(ce, sizeof(ce), "%s/%s.compile_err", tdir, stem);
        if (file_exists(ce)) compile_fail = 1;
        else if (ends_with(name, "_fail.ccs") || ends_with(name, "_fail.shcc"))
            compile_fail = 1;

        ran++;
        if (jobs <= 1) {
            if (run_one_test_maybe_profile(stem, path, compile_fail, verbose, "out", "bin",
                             use_cache, opt_o0, build_timeout_sec,
                             run_timeout_sec) != 0) {
                failed++;
                add_failed_name(stem);
            }
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
            /* Stem-stable dirs so incremental cache hits across jobs (stems are
             * unique across tests/; see discovery comment above). */
            char out_dir[512];
            char bin_dir[512];
            snprintf(out_dir, sizeof(out_dir), "out/.cc_test/%s", stem);
            snprintf(bin_dir, sizeof(bin_dir), "bin/.cc_test/%s", stem);
            (void)ensure_dir_p(out_dir);
            (void)ensure_dir_p(bin_dir);
            int rc = run_one_test_maybe_profile(stem, path, compile_fail, verbose, out_dir,
                                  bin_dir, use_cache, opt_o0, build_timeout_sec,
                                  run_timeout_sec);
            _exit(rc == 0 ? 0 : 1);
        }

        if (running + 1 > pid_cap) {
            int old_cap = pid_cap;
            int nc = pid_cap ? pid_cap * 2 : 32;
            pid_t* np = (pid_t*)realloc(pids, (size_t)nc * sizeof(pid_t));
            char** nn = (char**)realloc(pid_names, (size_t)nc * sizeof(char*));
            if (!np || !nn) { fprintf(stderr, "cc_test: OOM\n"); return 2; }
            pids = np; pid_names = nn;
            /* Zero the newly grown name slots. The exit cleanup loop iterates
             * the full capacity and frees every non-NULL slot, so leaving
             * these as uninitialised realloc memory frees a garbage pointer —
             * a crash on glibc that macOS malloc happened to tolerate. */
            for (int z = old_cap; z < nc; ++z) pid_names[z] = NULL;
            pid_cap = nc;
        }
        pids[running] = pid;
        pid_names[running] = strdup(stem);
        running++;
    }
    for (int i = 0; i < all_n; i++) free(all_paths[i]);
    free(all_paths);

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
        if (g_failed_count > 0) {
            fprintf(stderr, "\nFailed tests:\n");
            for (int i = 0; i < g_failed_count; ++i) {
                if (g_failed_names[i]) {
                    fprintf(stderr, "  - %s\n", g_failed_names[i]);
                    free(g_failed_names[i]);
                }
            }
            free(g_failed_names);
        }
        return 1;
    }
    fprintf(stderr, "cc_test: %d passed\n", ran);
    return 0;
}


