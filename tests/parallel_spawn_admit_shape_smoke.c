/*
 * Meeting admit (`@parallel spawn`) must not inline a failed sibling.
 * Unmarked cheap join still uses deny_fast.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_file(const char* path, const char* data) {
    FILE* f = fopen(path, "w");
    if (!f)
        return -1;
    if (fputs(data, f) < 0) {
        fclose(f);
        return -1;
    }
    return fclose(f);
}

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    long n;
    char* buf;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (char*)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[n] = 0;
    fclose(f);
    return buf;
}

static int emit_c(const char* fixture, const char* tag, char** out_buf) {
    char src_path[256];
    char out_path[256];
    char log_path[256];
    char cmd[1024];
    char* lowered;

    snprintf(src_path, sizeof(src_path), "tmp/%s_%ld.ccs", tag, (long)getpid());
    snprintf(out_path, sizeof(out_path), "tmp/%s_%ld.c", tag, (long)getpid());
    snprintf(log_path, sizeof(log_path), "tmp/%s_%ld.log", tag, (long)getpid());

    if (write_file(src_path, fixture) != 0) {
        perror("write fixture");
        return 2;
    }
    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc --emit-c-only %s -o %s > %s 2>&1", src_path,
             out_path, log_path);
    if (system(cmd) != 0) {
        fprintf(stderr, "emit-c-only failed; see %s\n", log_path);
        return 2;
    }
    lowered = read_file(out_path);
    remove(src_path);
    remove(out_path);
    remove(log_path);
    if (!lowered) {
        perror("read lowered C");
        return 2;
    }
    *out_buf = lowered;
    return 0;
}

int main(void) {
    const char* spawn_fix =
        "#include <ccc/std/prelude.cch>\n"
        "int main(void) {\n"
        "    @errhandler(CCError e) { return 1; }\n"
        "    int[~ >] tx;\n"
        "    int[~ <] rx;\n"
        "    CCChan* ch = cc_channel_pair(&tx, &rx) !> @destroy;\n"
        "    int got = 0;\n"
        "    @parallel spawn {\n"
        "        @serial { tx.send(1) !>; tx.close(); }\n"
        "        @serial { int v = 0; rx.recv(&v) !>; got = v; }\n"
        "    } !>.wait()!>;\n"
        "    return got == 1 ? 0 : 1;\n"
        "}\n";
    const char* cheap_fix =
        "#include <ccc/std/prelude.cch>\n"
        "int main(void) {\n"
        "    @errhandler(CCError e) { return 1; }\n"
        "    int a = 0, b = 0;\n"
        "    @parallel {\n"
        "        a = 1;\n"
        "        b = 2;\n"
        "    } !>.wait()!>;\n"
        "    return a + b;\n"
        "}\n";
    char* spawn_c = NULL;
    char* cheap_c = NULL;
    int rc;

    if (mkdir("tmp", 0777) != 0 && errno != EEXIST) {
        perror("mkdir tmp");
        return 2;
    }

    rc = emit_c(spawn_fix, "par_spawn_admit", &spawn_c);
    if (rc)
        return rc;
    if (!strstr(spawn_c, "cc_parallel_spawn_admit(")) {
        fprintf(stderr, "FAIL spawn join missing cc_parallel_spawn_admit\n");
        free(spawn_c);
        return 1;
    }
    if (!strstr(spawn_c, "cc_parallel_die(\"@parallel spawn failed\")")) {
        fprintf(stderr, "FAIL spawn join missing die on INVALID\n");
        free(spawn_c);
        return 1;
    }
    if (strstr(spawn_c, "cc_parallel_deny_fast(")) {
        fprintf(stderr, "FAIL spawn join still uses deny_fast\n");
        free(spawn_c);
        return 1;
    }
    if (strstr(spawn_c, "CC_PAR_NOTE_INLINE_ARM()")) {
        fprintf(stderr, "FAIL spawn join still inlines on INVALID\n");
        free(spawn_c);
        return 1;
    }
    free(spawn_c);

    rc = emit_c(cheap_fix, "par_cheap_deny", &cheap_c);
    if (rc)
        return rc;
    if (!strstr(cheap_c, "cc_parallel_deny_fast(")) {
        fprintf(stderr, "FAIL unmarked join lost deny_fast\n");
        free(cheap_c);
        return 1;
    }
    if (strstr(cheap_c, "cc_parallel_spawn_admit(")) {
        fprintf(stderr, "FAIL unmarked join used spawn_admit\n");
        free(cheap_c);
        return 1;
    }
    free(cheap_c);

    puts("parallel_spawn_admit_shape_smoke: OK");
    return 0;
}
