#include "io.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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

int cc_write_file_if_changed(const char *path, const void *data, size_t n) {
    FILE *in = fopen(path, "rb");
    if (in) {
        if (fseek(in, 0, SEEK_END) == 0) {
            long len = ftell(in);
            if (len >= 0 && (size_t)len == n) {
                if (n == 0) {
                    fclose(in);
                    return 0;
                }
                if (fseek(in, 0, SEEK_SET) == 0) {
                    char *old = (char *)malloc(n);
                    if (old) {
                        size_t got = fread(old, 1, n, in);
                        fclose(in);
                        in = NULL;
                        if (got == n && memcmp(old, data, n) == 0) {
                            free(old);
                            return 0;
                        }
                        free(old);
                    }
                }
            }
        }
        if (in) fclose(in);
    }

    FILE *out = fopen(path, "wb");
    if (!out) return errno ? errno : -1;
    if (n > 0 && fwrite(data, 1, n, out) != n) {
        int err = errno ? errno : -1;
        fclose(out);
        return err;
    }
    if (fclose(out) != 0) return errno ? errno : -1;
    return 0;
}

