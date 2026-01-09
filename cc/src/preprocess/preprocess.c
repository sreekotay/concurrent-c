#include "preprocess.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int cc_preprocess_file(const char* input_path, char* out_path, size_t out_path_sz) {
    if (!input_path || !out_path || out_path_sz == 0) return -1;
    char tmp_path[] = "/tmp/cc_pp_XXXXXX.c";
    int fd = mkstemps(tmp_path, 2); /* keep .c suffix */
    if (fd < 0) return -1;

    FILE *in = fopen(input_path, "r");
    FILE *out = fdopen(fd, "w");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    char line[1024];

    fprintf(out, "#line 1 \"%s\"\n", input_path);
    while (fgets(line, sizeof(line), in)) {
        fputs(line, out);
    }

    fclose(in);
    fclose(out);

    strncpy(out_path, tmp_path, out_path_sz - 1);
    out_path[out_path_sz - 1] = '\0';
    return 0;
}

