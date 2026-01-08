#include "io.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

int cc_copy_file(const char *src_path, const char *dst_path) {
    FILE *in = fopen(src_path, "rb");
    if (!in) {
        return errno ? errno : -1;
    }
    FILE *out = fopen(dst_path, "wb");
    if (!out) {
        int err = errno ? errno : -1;
        fclose(in);
        return err;
    }

    char buf[8192];
    size_t nread;
    while ((nread = fread(buf, 1, sizeof(buf), in)) > 0) {
        size_t nwritten = fwrite(buf, 1, nread, out);
        if (nwritten != nread) {
            int err = errno ? errno : -1;
            fclose(in);
            fclose(out);
            return err;
        }
    }

    int err = ferror(in) ? (errno ? errno : -1) : 0;
    fclose(in);
    fclose(out);
    return err;
}

