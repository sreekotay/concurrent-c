/*
 * Driver smoke: ccc -e/-E, token-gated predecls, -n/-p, --save / ccc @name.
 */
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

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

int main(void) {
    char out[8192];
    char cmd[1024];
    char toolbox[256];
    char tmpdir[] = "/tmp/cc_oneliner_XXXXXX";
    int ec = 0;
    int failed = 0;

    /* Inherit the suite front (native default). Nested -e uses the same
     * driver path as ordinary compiles. */
    unsetenv("CC_TEST_FRONTEND");

    if (!mkdtemp(tmpdir)) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(toolbox, sizeof(toolbox), "%s/toolbox.shcc", tmpdir);

    /* 1) plain -e */
    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc -e 'printf(\"hi-e\\n\");'");
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL plain -e (exit %d):\n%s\n", ec, out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "hi-e", "plain -e");
    }

    /* 2) synthetic argv[0] */
    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc -e 'printf(\"%%s\\n\", argv[0]);'");
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL argv0 (exit %d):\n%s\n", ec, out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "-e", "argv0");
    }

    /* 3) predeclared stdin (+ private arena via setup) */
    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc -e '\"via-stdin\".println();'");
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL stdin predecl (exit %d):\n%s\n", ec, out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "via-stdin", "stdin predecl");
    }

    /* 4) predeclared stdin.read_all */
    snprintf(cmd, sizeof(cmd),
             "printf 'in-data' | ./cc/bin/ccc -e "
             "'char[:] in = stdin.read_all(arena) !>; in.println() !>;'");
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL stdin read_all (exit %d):\n%s\n", ec, out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "in-data", "stdin read_all");
    }

    /* 5) args predecl */
    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc -e '"
             "if (args.len >= 1) printf(\"%%s\\n\", ((char**)(args.ptr))[0]);"
             "' hello");
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL args predecl (exit %d):\n%s\n", ec, out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "hello", "args predecl");
    }

    /* 6) --save + ccc @name + @ list */
    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc --save-to '%s' --save ping --doc 'ping task' "
             "-e 'printf(\"pong\\n\");'",
             toolbox);
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL --save (exit %d):\n%s\n", ec, out);
        failed = 1;
    }

    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc --save-to '%s' @ping", toolbox);
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL @ping (exit %d):\n%s\n", ec, out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "pong", "@ping");
    }

    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc --save-to '%s' @", toolbox);
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL @ list (exit %d):\n%s\n", ec, out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "ping", "@ list name");
        failed |= expect_substr(out, "ping task", "@ list doc");
    }

    /* 7) duplicate --save fails */
    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc --save-to '%s' --save ping -e 'printf(\"x\\n\");'",
             toolbox);
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec == 0) {
        fprintf(stderr, "FAIL duplicate save should fail (exit %d):\n%s\n", ec,
                out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "already exists", "duplicate save");
    }

    /* 7b) keyword --save name rejected */
    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc --save-to '%s' --save int -e 'printf(\"x\\n\");'",
             toolbox);
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec == 0) {
        fprintf(stderr, "FAIL keyword save should fail (exit %d):\n%s\n", ec, out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "non-keyword", "keyword save");
    }

    /* 7c) template prose must not trigger predecl `stdin` (no stdin drain) */
    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc -e 'println(@string(`built in a jiffy`));'");
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL template prose predecl (exit %d):\n%s\n", ec, out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "built in a jiffy", "template prose");
    }

    /* 7d) apostrophe in template must not wedge --save duplicate detection */
    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc --save-to '%s' --save dont -e "
             "'println(@string(`don'\\''t`));'",
             toolbox);
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL save dont (exit %d):\n%s\n", ec, out);
        failed = 1;
    }
    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc --save-to '%s' --save dont -e 'printf(\"x\\n\");'",
             toolbox);
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec == 0) {
        fprintf(stderr, "FAIL duplicate after apostrophe template (exit %d):\n%s\n",
                ec, out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "already exists", "duplicate after template");
    }

    /* 8) -E expr print — token-gated; does not force stdin */
    snprintf(cmd, sizeof(cmd), "./cc/bin/ccc -E '1 + 2'");
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL -E (exit %d):\n%s\n", ec, out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "3", "-E 1+2");
    }

    /* 8b) -E with nested @string template (no backtick nesting in wrap) */
    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc -E '@string(`hi-tpl`, arena)'");
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL -E @string (exit %d):\n%s\n", ec, out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "hi-tpl", "-E @string");
    }

    /* 8c) --doc must escape comment delimiters so CCDoc cannot be closed early.
     * Use a fresh toolbox: shared file after @dont (`don't` in a template body)
     * currently breaks later @task discovery — out of scope for this check. */
    {
        char toolbox_doc[256];
        snprintf(toolbox_doc, sizeof(toolbox_doc), "%s/doc_escape.shcc", tmpdir);
        snprintf(cmd, sizeof(cmd),
                 "./cc/bin/ccc --save-to '%s' --save safe_doc "
                 "--doc 'end */ int main(){return 0;} /*' "
                 "-e 'printf(\"safe\\n\");'",
                 toolbox_doc);
        if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
            fprintf(stderr, "FAIL --doc escape save (exit %d):\n%s\n", ec, out);
            failed = 1;
        } else {
            snprintf(cmd, sizeof(cmd),
                     "./cc/bin/ccc --save-to '%s' @safe_doc", toolbox_doc);
            if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
                fprintf(stderr,
                        "FAIL @safe_doc after hostile --doc (exit %d):\n%s\n",
                        ec, out);
                failed = 1;
            } else {
                failed |= expect_substr(out, "safe", "@safe_doc");
            }
        }
    }

    /* 9) -n -E line numbers */
    snprintf(cmd, sizeof(cmd),
             "printf 'a\\nb\\n' | ./cc/bin/ccc -n -E 'nr'");
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL -n -E nr (exit %d):\n%s\n", ec, out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "1", "-n -E nr:1");
        failed |= expect_substr(out, "2", "-n -E nr:2");
    }

    /* 10) -p passthrough */
    snprintf(cmd, sizeof(cmd),
             "printf 'x\\ny\\n' | ./cc/bin/ccc -p -e ''");
    if (run_capture(cmd, out, sizeof(out), &ec) != 0 || ec != 0) {
        fprintf(stderr, "FAIL -p (exit %d):\n%s\n", ec, out);
        failed = 1;
    } else {
        failed |= expect_substr(out, "x", "-p:x");
        failed |= expect_substr(out, "y", "-p:y");
    }

    if (failed) return 1;
    printf("script_oneliner_smoke ok\n");
    return 0;
}
