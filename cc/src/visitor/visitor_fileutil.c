#include "visitor_fileutil.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int cc__read_entire_file(const char* path, char** out_buf, size_t* out_len) {
    if (!path || !out_buf || !out_len) return 0;
    *out_buf = NULL;
    *out_len = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    *out_buf = buf;
    *out_len = n;
    return 1;
}

char* cc__write_temp_c_file(const char* buf, size_t len, const char* original_path) {
    (void)errno;
    if (!buf || !original_path) return NULL;
    char tmpl[] = "/tmp/cc_reparse_XXXXXX.c";
#ifdef __APPLE__
    int fd = mkstemps(tmpl, 2);
#else
    int fd = mkstemp(tmpl);
#endif
    if (fd < 0) return NULL;
    /* Minimal prelude so patched TCC can type-check rewritten intermediate code during the reparse step. */
    const char* prelude =
        "#define CC_PARSER_MODE 1\n"
        "#include <stdint.h>\n"
        "typedef intptr_t CCAbIntptr;\n";
    size_t pre_len = strlen(prelude);
    size_t off = 0;
    while (off < pre_len) {
        ssize_t n = write(fd, prelude + off, pre_len - off);
        if (n <= 0) { close(fd); unlink(tmpl); return NULL; }
        off += (size_t)n;
    }
    char line_buf[1024];
    int ln = snprintf(line_buf, sizeof(line_buf), "#line 1 \"%s\"\n", original_path);
    if (ln <= 0 || (size_t)ln >= sizeof(line_buf)) { close(fd); unlink(tmpl); return NULL; }
    off = 0;
    while (off < (size_t)ln) {
        ssize_t n = write(fd, line_buf + off, (size_t)ln - off);
        if (n <= 0) { close(fd); unlink(tmpl); return NULL; }
        off += (size_t)n;
    }
    off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n <= 0) { close(fd); unlink(tmpl); return NULL; }
        off += (size_t)n;
    }
    close(fd);
    return strdup(tmpl);
}

