/*
 * Header UFCS must not rewrite a function-pointer field call
 * (`c.drop(c.env)`) into the same-named wrapper (`cc_widget_drop(&c, c.env)`).
 *
 * The class is in-process and order-dependent: leftover include-probe
 * state from an earlier file in the same `lower_headers` process (or a
 * call-site spelling counted as a callee) makes the wrapper look real.
 * A `.ccs` that includes already-lowered stdlib cannot see it — that is
 * why a compile of `cc_closureN_drop` after headers are already `.h`
 * stays green while isolated header lowering breaks. Two sequential
 * `lower_headers` processes cannot see it either.
 *
 * This smoke reproduces the configuration, not the symptom: it forces
 * polluter-then-victim in one process via `lower_headers --ordered`,
 * and keeps the victim field call unparenthesized so a defensive
 * `(c.drop)(c.env)` cannot mask a probe regression.
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

/* Declares the composed wrapper so a leaked include-probe (has_declare)
 * treats it as a real UFCS callee on the next file. */
static const char k_polluter_decl[] =
    "void cc_widget_drop(int x);\n";

/* Call-site only — has_callable would match; has_declare must not. */
static const char k_polluter_call[] =
    "static inline void poke_widget(void) {\n"
    "    cc_widget_drop(0);\n"
    "}\n";

/* `ccc build` rewrites the four-character face suffix in this TU.
 * Build it at runtime so fixtures stay faces, not lowered .h paths. */
static void face_ext(char* out) {
    out[0] = '.';
    out[1] = 'c';
    out[2] = 'c';
    out[3] = 'h';
    out[4] = '\0';
}

/* Parser-mode stub so `drop` is not a known field (same shape as
 * CCClosureN). The field call stays unparenthesized on purpose. */
static int write_victim(const char* path) {
    char ext[8];
    char text[768];
    face_ext(ext);
    snprintf(text, sizeof(text),
             "#include \"polluter_call%s\"\n"
             "/* Incomplete CC* type, no field table. A `drop` member would\n"
             " * make UFCS leave the call alone; a scalar alias peels to\n"
             " * intptr_t and composes the wrong callee. */\n"
             "typedef struct CCWidget CCWidget;\n"
             "static inline void cc_widget_drop(CCWidget c) {\n"
             "    if (c.drop) c.drop(c.env);\n"
             "}\n",
             ext);
    return write_file(path, text);
}

static int check_stdlib_artifact(void) {
    const char* path = "out/include/ccc/cc_closure.h";
    char* text;
    int rc = 0;
    if (access(path, R_OK) != 0) return 0;
    text = read_file(path);
    if (!text) {
        fprintf(stderr, "FAIL read %s\n", path);
        return 1;
    }
    rc |= expect_absent(text, "cc_closure0_drop(&", "stdlib closure0");
    rc |= expect_absent(text, "cc_closure1_drop(&", "stdlib closure1");
    rc |= expect_absent(text, "cc_closure2_drop(&", "stdlib closure2");
    free(text);
    return rc;
}

int main(void) {
    char tmpl[] = "/tmp/cc_ufcs_fnptr_XXXXXX";
    char ext[8];
    char* dir;
    char polluter_decl[512];
    char polluter_call[512];
    char victim[512];
    char outdir[512];
    char victim_h[512];
    char cmd[2048];
    char captured[4096];
    char* lowered = NULL;
    int ec = 0;
    int rc = 0;
    const char* tool = "./out/cc/bin/lower_headers";
    face_ext(ext);

    if (access(tool, X_OK) != 0) {
        fprintf(stderr, "FAIL missing %s (build with make -C cc)\n", tool);
        return 1;
    }

    dir = mkdtemp(tmpl);
    if (!dir) {
        fprintf(stderr, "FAIL mkdtemp\n");
        return 1;
    }
    snprintf(polluter_decl, sizeof(polluter_decl), "%s/polluter_decl%s", dir, ext);
    snprintf(polluter_call, sizeof(polluter_call), "%s/polluter_call%s", dir, ext);
    snprintf(victim, sizeof(victim), "%s/victim%s", dir, ext);
    snprintf(outdir, sizeof(outdir), "%s/out", dir);
    snprintf(victim_h, sizeof(victim_h), "%s/victim.h", outdir);

    if (write_file(polluter_decl, k_polluter_decl) ||
        write_file(polluter_call, k_polluter_call) ||
        write_victim(victim)) {
        return 1;
    }
    if (mkdir(outdir, 0755) != 0) {
        fprintf(stderr, "FAIL mkdir %s\n", outdir);
        return 1;
    }

    snprintf(cmd, sizeof(cmd), "%s --ordered %s %s %s",
             tool, outdir, polluter_decl, victim);
    if (run_capture(cmd, captured, sizeof(captured), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL lower_headers --ordered (exit %d):\n%s\n",
                ec, captured);
        return 1;
    }

    lowered = read_file(victim_h);
    if (!lowered) {
        fprintf(stderr, "FAIL read lowered victim %s\ntool said:\n%s\n",
                victim_h, captured);
        return 1;
    }
    rc |= expect_has(lowered, "c.drop(", "unrewritten field call");
    rc |= expect_absent(lowered, "cc_widget_drop(&c", "wrapper rewrite");
    rc |= expect_absent(lowered, "cc_widget_drop(&c,", "wrapper rewrite comma");
    free(lowered);

    rc |= check_stdlib_artifact();

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    {
        int cleaned = system(cmd);
        (void)cleaned;
    }

    if (rc) return 1;
    printf("ufcs_fnptr_field_header_smoke ok\n");
    return 0;
}
