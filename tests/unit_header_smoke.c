/*
 * Driver smoke: first-line unit headers (#!ccc ccs|cch, OS shebang) and
 * version=MAJOR[.MINOR[.PATCH[-SEED]]] pins.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static int expect_parse(const char* line, const char* want, const char* label) {
    char cmd[1024];
    char out[1024];
    int ec = 0;
    snprintf(cmd, sizeof(cmd), "./cc/bin/ccc __unit-header-parse '%s'", line);
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL %s (exit %d):\n%s\n", label, ec, out);
        return 1;
    }
    return expect_substr(out, want, label);
}

int main(void) {
    char out[4096];
    char cmd[1024];
    char ver[64];
    int ec = 0;
    int failed = 0;
    char* nl;

    failed |= expect_parse("#!ccc ccs", "kind=ccs os_shebang=0 version=-",
                           "magic ccs");
    failed |= expect_parse("#!ccc cch version=0.3.2-121",
                           "kind=cch os_shebang=0 version=0.3.2-121",
                           "magic cch pin");
    failed |= expect_parse("#!ccc ccs version=0.3.2",
                           "kind=ccs os_shebang=0 version=0.3.2",
                           "magic ccs prefix pin");
    failed |= expect_parse("#!ccc ccs version=0.3",
                           "kind=ccs os_shebang=0 version=0.3",
                           "magic ccs major.minor pin");
    failed |= expect_parse("#!/usr/bin/env -S ./cc/bin/ccc",
                           "kind=shcc os_shebang=1 version=-", "os shebang");
    failed |= expect_parse(
        "#!/usr/bin/env -S ./cc/bin/ccc --as=shcc version=0.3.1-110",
        "kind=shcc os_shebang=1 version=0.3.1-110", "os shebang pin");

    snprintf(cmd, sizeof(cmd), "./cc/bin/ccc __unit-header-parse '#!ccc shcc'");
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec == 0) {
        fprintf(stderr, "FAIL #!ccc shcc should be ill-formed:\n%s\n", out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "ill_formed", "#!ccc shcc");
    }

    if (run_capture("./cc/bin/ccc --version", out, sizeof(out), &ec) != 0 ||
        ec != 0) {
        fprintf(stderr, "FAIL --version (exit %d):\n%s\n", ec, out);
        failed = 1;
        ver[0] = '\0';
    } else {
        failed |= expect_substr(out, "ccc ", "--version prefix");
        snprintf(ver, sizeof(ver), "%s", out + 4);
        nl = strchr(ver, '\n');
        if (nl) *nl = '\0';
        if (!strchr(ver, '-')) {
            fprintf(stderr, "FAIL --version not MAJOR.MINOR.PATCH-SEED: %s\n",
                    ver);
            failed = 1;
        }
    }

    if (ver[0]) {
        char prefix[64];
        char bad[80];
        char* dash;
        snprintf(prefix, sizeof(prefix), "%s", ver);
        dash = strrchr(prefix, '-');
        if (dash) *dash = '\0';

        snprintf(cmd, sizeof(cmd),
                 "./cc/bin/ccc version=%s --emit-c-only "
                 "tests/unit_header_ccs_smoke.ccs",
                 ver);
        if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
            fprintf(stderr, "FAIL current-version pin (exit %d):\n%s\n", ec,
                    out);
            failed = 1;
        }

        snprintf(cmd, sizeof(cmd),
                 "./cc/bin/ccc version=%s --emit-c-only "
                 "tests/unit_header_ccs_smoke.ccs",
                 prefix);
        if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
            fprintf(stderr, "FAIL prefix pin %s (exit %d):\n%s\n", prefix, ec,
                    out);
            failed = 1;
        }

        snprintf(bad, sizeof(bad), "%s-12", prefix);
        snprintf(cmd, sizeof(cmd),
                 "./cc/bin/ccc version=%s --emit-c-only "
                 "tests/unit_header_ccs_smoke.ccs",
                 bad);
        if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec == 0) {
            fprintf(stderr,
                    "FAIL %s must not match %s (exit %d):\n%s\n",
                    bad, ver, ec, out);
            failed = 1;
        } else {
            snprintf(cmd, sizeof(cmd), "missing bootstrap seed %s", bad);
            failed |= expect_substr(out, cmd, "seed prefix not a match");
        }
    }

    if (failed) return 1;
    printf("unit_header_smoke ok\n");
    return 0;
}
