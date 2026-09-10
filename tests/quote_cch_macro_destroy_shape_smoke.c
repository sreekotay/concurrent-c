/* `quote_cch_macro_destroy_smoke` runs and prints `ok` whether or not the
 * `@destroy` carried by the quoted header's `#define` was honoured: a
 * buffer arena that is never destroyed leaks nothing observable. So the
 * emitted C is read instead. A lowerer that drops the attribute passes
 * the smoke and fails here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    long n;
    char* buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    buf[n] = 0;
    fclose(f);
    return buf;
}

int main(void) {
    char out_path[256];
    char cmd[1024];
    char* lowered;
    int rc = 0;
    snprintf(out_path, sizeof(out_path), "tmp/quote_cch_macro_destroy_%ld.c", (long)getpid());
    snprintf(cmd, sizeof(cmd),
             "./cc/bin/ccc --frontend=native --emit-c-only tests/quote_cch_macro_destroy_smoke.ccs -o %s > /dev/null 2>&1",
             out_path);
    if (system(cmd) != 0) { fprintf(stderr, "emit failed\n"); return 1; }
    lowered = read_file(out_path);
    remove(out_path);
    if (!lowered) { fprintf(stderr, "no output\n"); return 1; }
    if (!strstr(lowered, "cc_arena_create_buffer(")) {
        fprintf(stderr, "the macro was not expanded to its arena\n");
        rc = 1;
    } else if (!strstr(lowered, "cc_arena_destroy(")) {
        fprintf(stderr, "the @destroy carried by the quoted #define was dropped: no cc_arena_destroy in the emitted C\n");
        rc = 1;
    }
    free(lowered);
    if (rc == 0) printf("ok\n");
    return rc;
}
