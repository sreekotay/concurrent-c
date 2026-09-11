/*
 * Two same-basename .shcc scripts must not share one script-cache binary.
 * Repro: build A, build B (same stem), run A again — A must still be A.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_capture(const char* cmd, char* out, size_t out_cap, int* exit_code) {
    char wrapped[2048];
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

static int expect_substr(const char* hay, const char* needle, const char* label) {
    if (!hay || !needle || !strstr(hay, needle)) {
        fprintf(stderr, "FAIL %s: missing '%s' in:\n%s\n", label, needle,
                hay ? hay : "(null)");
        return 1;
    }
    return 0;
}

static int write_make_shcc(const char* path, const char* tip) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f,
            "#!/usr/bin/env -S ../../cc/bin/ccc --as=shcc\n"
            "\"%s\".println();\n"
            "return 0;\n",
            tip);
    fclose(f);
    if (chmod(path, 0755) != 0) return -1;
    return 0;
}

int main(void) {
    char out[4096];
    char cmd[2048];
    char root[] = "/tmp/cc_shcc_stem_XXXXXX";
    char dira[512], dirb[512], patha[1024], pathb[1024];
    int ec = 0;
    int failed = 0;

    unsetenv("CC_TEST_FRONTEND");
    unsetenv("CC_NO_CACHE");

    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(dira, sizeof(dira), "%s/proj_a", root);
    snprintf(dirb, sizeof(dirb), "%s/proj_b", root);
    if (mkdir(dira, 0755) != 0 || mkdir(dirb, 0755) != 0) {
        perror("mkdir");
        return 1;
    }
    snprintf(patha, sizeof(patha), "%s/make.shcc", dira);
    snprintf(pathb, sizeof(pathb), "%s/make.shcc", dirb);
    if (write_make_shcc(patha, "stem-smoke-A") != 0 ||
        write_make_shcc(pathb, "stem-smoke-B") != 0) {
        fprintf(stderr, "FAIL write make.shcc\n");
        return 1;
    }

    /* Absolute path: shebang ../../cc from /tmp would miss the driver. */
    snprintf(cmd, sizeof(cmd), "./cc/bin/ccc --as=shcc '%s'", patha);
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL build A (exit %d):\n%s\n", ec, out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "stem-smoke-A", "first A");
    }

    snprintf(cmd, sizeof(cmd), "./cc/bin/ccc --as=shcc '%s'", pathb);
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL build B (exit %d):\n%s\n", ec, out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "stem-smoke-B", "first B");
        if (strstr(out, "stem-smoke-A")) {
            fprintf(stderr, "FAIL first B still printed A:\n%s\n", out);
            failed = 1;
        }
    }

    /* Cache warm: B must not have stolen A's product path. */
    snprintf(cmd, sizeof(cmd), "./cc/bin/ccc --as=shcc '%s'", patha);
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL re-run A (exit %d):\n%s\n", ec, out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "stem-smoke-A", "re-run A");
        if (strstr(out, "stem-smoke-B")) {
            fprintf(stderr, "FAIL re-run A printed B (stem collision):\n%s\n",
                    out);
            failed = 1;
        }
    }

    if (!failed) puts("ok");
    return failed ? 1 : 0;
}
