#include "visitor_fileutil.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/path.h"

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
    /* Prelude for reparse: must provide CC runtime types used by intermediate rewrites
       (e.g. CCNursery/CCClosure0) even when user source doesn't include the headers. */
    const char* prelude =
        "#define CC_PARSER_MODE 1\n"
        "#include <stdlib.h>\n"
        "#include <stdint.h>\n"
        "typedef intptr_t CCAbIntptr;\n"
        "#include \"cc_closure.cch\"\n"
        "#include \"cc_nursery.cch\"\n"
        "#include \"cc_arena.cch\"\n"
        "/* Minimal std/prelude declarations used by @arena lowering (avoid including the full prelude\n"
        "   here because user code may define CC_ENABLE_SHORT_NAMES before including it). */\n"
        "static inline size_t kilobytes(size_t n);\n"
        "static inline size_t megabytes(size_t n);\n"
        "static inline CCArena cc_heap_arena(size_t bytes);\n"
        "static inline void cc_heap_arena_free(CCArena* a);\n"
        "#include \"cc_slice.cch\"\n"
        "#include \"std/task_intptr.cch\"\n"
        "/* Async channel task functions needed for UFCS rewrites in @async context. */\n"
        "typedef struct CCChan CCChan;\n"
        "CCTaskIntptr cc_chan_send_task(CCChan* ch, const void* value, size_t value_size);\n"
        "CCTaskIntptr cc_chan_recv_task(CCChan* ch, void* out_value, size_t value_size);\n"
        "typedef struct { void (*fn)(void); } __cc_spawn_void_arg;\n"
        "typedef struct { void (*fn)(int); int arg; } __cc_spawn_int_arg;\n"
        "static void* __cc_spawn_thunk_void(void*);\n"
        "static void* __cc_spawn_thunk_int(void*);\n";
    size_t pre_len = strlen(prelude);
    size_t off = 0;
    while (off < pre_len) {
        ssize_t n = write(fd, prelude + off, pre_len - off);
        if (n <= 0) { close(fd); unlink(tmpl); return NULL; }
        off += (size_t)n;
    }
    char rel[1024];
    const char* lp = cc_path_rel_to_repo(original_path, rel, sizeof(rel));
    char line_buf[1024];
    int ln = snprintf(line_buf, sizeof(line_buf), "#line 1 \"%s\"\n", lp);
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

