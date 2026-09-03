/*
 * Header `!>` is a use: lower_headers must rewrite it and emit a guarded
 * CCResult_* spec in that .h. A C TU that includes the .h and never
 * unwraps the pair itself must still compile.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_capture(const char* cmd, char* out, size_t out_cap, int* exit_code) {
    char wrapped[4096];
    FILE* f;
    size_t n;
    int st;
    if (out && out_cap) out[0] = '\0';
    snprintf(wrapped, sizeof(wrapped), "%s 2>&1", cmd);
    f = popen(wrapped, "r");
    if (!f) return -1;
    n = fread(out, 1, out_cap > 0 ? out_cap - 1 : 0, f);
    if (out && out_cap) out[n] = '\0';
    st = pclose(f);
    if (exit_code) {
        if (WIFEXITED(st)) *exit_code = WEXITSTATUS(st);
        else *exit_code = 1;
    }
    return 0;
}

static int write_file(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "FAIL write %s\n", path);
        return 1;
    }
    fputs(text, f);
    fclose(f);
    return 0;
}

static char* read_file(const char* path) {
    FILE* f = fopen(path, "r");
    long len;
    char* buf;
    size_t n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static int expect_has(const char* hay, const char* needle, const char* label) {
    if (!hay || !strstr(hay, needle)) {
        fprintf(stderr, "FAIL %s: missing '%s' in:\n%s\n", label, needle,
                hay ? hay : "(null)");
        return 1;
    }
    return 0;
}

static int expect_absent(const char* hay, const char* needle, const char* label) {
    if (hay && strstr(hay, needle)) {
        fprintf(stderr, "FAIL %s: found '%s' in:\n%s\n", label, needle, hay);
        return 1;
    }
    return 0;
}

static int leftover_bang(const char* hay) {
    const char* p;
    if (!hay) return 0;
    for (p = hay; (p = strstr(p, "!>")) != NULL; p += 2) {
        if (p > hay && p[-1] == '/') continue; /* comment noise */
        return 1;
    }
    return 0;
}

static int check_stdlib_file_h(void) {
    const char* path = "out/include/ccc/script/file.h";
    char* text;
    int rc = 0;
    if (access(path, R_OK) != 0) return 0;
    text = read_file(path);
    if (!text) {
        fprintf(stderr, "FAIL read %s\n", path);
        return 1;
    }
    if (leftover_bang(text)) {
        fprintf(stderr, "FAIL %s still contains source !>\n", path);
        rc = 1;
    }
    rc |= expect_absent(text, "__cc_uw_", "stdlib file.h tu unwrap");
    free(text);
    return rc;
}

int main(void) {
    char tmpl[] = "/tmp/cc_hdr_unwrap_XXXXXX";
    char* dir;
    char src[512];
    char outdir[512];
    char out_h[512];
    char guest_c[512];
    char guest_bin[512];
    char cmd[2048];
    char captured[8192];
    char* lowered = NULL;
    int ec = 0;
    int rc = 0;
    const char* tool = "./out/cc/bin/lower_headers";
    /* Split so a ccc-built copy of this .c does not rewrite `.cch"` to `.h`. */
    const char* fixture = "tests/header_unwrap_inline." "cch";

    if (access(tool, X_OK) != 0) {
        fprintf(stderr, "FAIL missing %s (build with make cc)\n", tool);
        return 1;
    }
    if (access(fixture, R_OK) != 0) {
        fprintf(stderr, "FAIL missing %s\n", fixture);
        return 1;
    }

    dir = mkdtemp(tmpl);
    if (!dir) {
        fprintf(stderr, "FAIL mkdtemp\n");
        return 1;
    }
    snprintf(src, sizeof(src), "%s/header_unwrap_inline." "cch", dir);
    snprintf(outdir, sizeof(outdir), "%s/out", dir);
    snprintf(out_h, sizeof(out_h), "%s/header_unwrap_inline.h", outdir);
    snprintf(guest_c, sizeof(guest_c), "%s/guest.c", dir);
    snprintf(guest_bin, sizeof(guest_bin), "%s/guest", dir);

    {
        char* face = read_file(fixture);
        if (!face) {
            fprintf(stderr, "FAIL read %s\n", fixture);
            return 1;
        }
        rc = write_file(src, face);
        free(face);
        if (rc) return 1;
    }
    if (mkdir(outdir, 0755) != 0) {
        fprintf(stderr, "FAIL mkdir %s\n", outdir);
        return 1;
    }

    snprintf(cmd, sizeof(cmd), "%s --ordered %s %s", tool, outdir, src);
    if (run_capture(cmd, captured, sizeof(captured), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL lower_headers (exit %d):\n%s\n", ec, captured);
        return 1;
    }

    lowered = read_file(out_h);
    if (!lowered) {
        fprintf(stderr, "FAIL read lowered %s\ntool said:\n%s\n", out_h,
                captured);
        return 1;
    }
    if (leftover_bang(lowered)) {
        fprintf(stderr, "FAIL lowered header still contains source !>\n%s\n",
                lowered);
        rc = 1;
    }
    rc |= expect_has(lowered, "CCResult_int_HdrIdxErr", "result type");
    rc |= expect_has(lowered, "CC_DECL_RESULT_SPEC", "guarded spec");
    rc |= expect_absent(lowered, "__cc_uw_", "tu-level unwrap macros");
    free(lowered);

    /* T?>(E) must lower as a type in headers (same as T!>(E)). Leaving
     * `void ?>(E)` for the unwrap pass mis-rewrites decls and hangs on
     * stdlib faces like stdio — regression for that bug. */
    {
        char opt_src[512];
        char opt_h[512];
        char* opt_lowered = NULL;
        snprintf(opt_src, sizeof(opt_src), "%s/opt_void.cch", dir);
        snprintf(opt_h, sizeof(opt_h), "%s/opt_void.h", outdir);
        if (write_file(opt_src,
                       "typedef struct { int x; } OptErr;\n"
                       "static inline void ?>(OptErr) opt_void_ok(void) {\n"
                       "    return cc_ok();\n"
                       "}\n"))
            return 1;
        snprintf(cmd, sizeof(cmd), "%s --ordered %s %s", tool, outdir, opt_src);
        if (run_capture(cmd, captured, sizeof(captured), &ec) != 0 || ec != 0) {
            fprintf(stderr, "FAIL lower_headers opt void (exit %d):\n%s\n",
                    ec, captured);
            return 1;
        }
        opt_lowered = read_file(opt_h);
        if (!opt_lowered) {
            fprintf(stderr, "FAIL read %s\n", opt_h);
            return 1;
        }
        rc |= expect_has(opt_lowered, "CCResult_void_OptErr", "void ?> type");
        rc |= expect_absent(opt_lowered, "__cc_pu_", "void ?> not unwrap expr");
        if (strstr(opt_lowered, "?>")) {
            fprintf(stderr, "FAIL opt void.h still contains ?>\n%s\n",
                    opt_lowered);
            rc = 1;
        }
        free(opt_lowered);
    }

    if (write_file(guest_c,
                   "#include \"header_unwrap_inline.h\"\n"
                   "int main(void) {\n"
                   "    HdrDoc d;\n"
                   "    d.n = 3;\n"
                   "    if (hdr_prev_sel(&d, 1) != 1) return 1;\n"
                   "    if (hdr_prev_sel(&d, 99) != -1) return 1;\n"
                   "    return 0;\n"
                   "}\n"))
        return 1;

    snprintf(cmd, sizeof(cmd),
             "cc -std=c11 -c -I%s -Iout/include -Icc/include %s -o %s.o",
             outdir, guest_c, guest_bin);
    if (run_capture(cmd, captured, sizeof(captured), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL guest cc -c (exit %d):\n%s\n", ec, captured);
        rc = 1;
    }

    rc |= check_stdlib_file_h();

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    {
        int cleaned = system(cmd);
        (void)cleaned;
    }

    if (rc) return 1;
    printf("header_unwrap_inline_smoke ok\n");
    return 0;
}
