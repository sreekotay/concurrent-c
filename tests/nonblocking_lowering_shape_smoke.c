#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_file(const char* path, const char* data) {
    FILE* f = fopen(path, "w");
    if (!f) return -1;
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
    if (!f) return NULL;
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

static int count_substr(const char* s, const char* needle) {
    int count = 0;
    size_t n = strlen(needle);
    const char* p = s;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += n;
    }
    return count;
}

int main(void) {
    const char* fixture =
        "#include <ccc/cc_runtime.cch>\n"
        "#include <ccc/std/task.cch>\n"
        "#include <stdint.h>\n"
        "static void wrapped_before(intptr_t* out) { *out = 1; }\n"
        "static void direct_inside(intptr_t* out) { *out = 2; }\n"
        "static void forced_inside(intptr_t* out) { *out = 3; }\n"
        "static void wrapped_after(intptr_t* out) { *out = 4; }\n"
        "@async int serve(intptr_t* a, intptr_t* b, intptr_t* c, intptr_t* d) {\n"
        "    wrapped_before(a);\n"
        "    @nonblocking {\n"
        "        direct_inside(b);\n"
        "        @blocking forced_inside(c);\n"
        "    }\n"
        "    wrapped_after(d);\n"
        "    return 0;\n"
        "}\n";

    char src_path[256];
    char out_path[256];
    char log_path[256];
    char cmd[1024];
    char* lowered;
    int wraps;

    if (mkdir("tmp", 0777) != 0 && errno != EEXIST) {
        perror("mkdir tmp");
        return 2;
    }

    snprintf(src_path, sizeof(src_path), "tmp/nonblocking_lowering_shape_%ld.ccs", (long)getpid());
    snprintf(out_path, sizeof(out_path), "tmp/nonblocking_lowering_shape_%ld.c", (long)getpid());
    snprintf(log_path, sizeof(log_path), "tmp/nonblocking_lowering_shape_%ld.log", (long)getpid());

    if (write_file(src_path, fixture) != 0) {
        perror("write fixture");
        return 2;
    }

    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc --emit-c-only %s -o %s > %s 2>&1",
             src_path, out_path, log_path);
    if (system(cmd) != 0) {
        fprintf(stderr, "emit-c-only failed; see %s\n", log_path);
        return 2;
    }

    lowered = read_file(out_path);
    if (!lowered) {
        perror("read lowered C");
        return 2;
    }

    wraps = count_substr(lowered, " = (cc_run_blocking_task_intptr(");
    if (wraps != 3) {
        fprintf(stderr, "expected 3 blocking wraps, got %d\n", wraps);
        free(lowered);
        return 1;
    }

    if (!strstr(lowered, "direct_inside(__f->__p_b);")) {
        fprintf(stderr, "missing direct call for @nonblocking block body\n");
        free(lowered);
        return 1;
    }

    if (strstr(lowered, "@nonblocking") || strstr(lowered, "@noblock") || strstr(lowered, "@CC_BLOCK")) {
        fprintf(stderr, "mode markers leaked into emitted C\n");
        free(lowered);
        return 1;
    }

    free(lowered);
    remove(src_path);
    remove(out_path);
    remove(log_path);
    puts("ok");
    return 0;
}
