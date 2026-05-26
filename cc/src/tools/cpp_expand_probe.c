/* Spike driver: read file, run cc_cpp_expand, dump to stdout.
 * Build: cc cpp_expand_probe.c cc/src/preprocess/cpp_expand.c -ltcc
 * Run:   ./cpp_expand_probe path/to/file.ccs */
#include "../preprocess/cpp_expand.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 1;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* src = (char*)malloc((size_t)n + 1);
    if (!src) { fclose(f); return 1; }
    fread(src, 1, (size_t)n, f);
    src[n] = '\0';
    fclose(f);

    size_t out_len = 0;
    char* exp = cc_cpp_expand(src, (size_t)n, argv[1], &out_len);
    free(src);
    if (!exp) {
        fprintf(stderr, "cc_cpp_expand failed\n");
        return 2;
    }
    fwrite(exp, 1, out_len, stdout);
    free(exp);
    return 0;
}
