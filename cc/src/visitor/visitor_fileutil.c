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
       (e.g. CCNursery/CCClosure0) even when user source doesn't include the headers.
       cc_result.cch provides __CC_RESULT macro which handles parser-mode fallback. */
    const char* prelude =
        "#line 1 \"<cc-prelude>\"\n"
        "#define __CC__ 1\n"
        "#define CC_PARSER_MODE 1\n"
        "#include <stdlib.h>\n"
        "#include <stdint.h>\n"
        "typedef intptr_t CCAbIntptr;\n"
        "#include <ccc/cc_closure.cch>\n"
        "#include <ccc/cc_nursery.cch>\n"
        "#include <ccc/cc_arena.cch>\n"
        "#include <ccc/cc_result.cch>\n"
        "#include <ccc/cc_optional.cch>\n"
        "/* Minimal std/prelude declarations used by reparse-time arena types/helpers */\n"
        "static inline size_t kilobytes(size_t n);\n"
        "static inline size_t megabytes(size_t n);\n"
        "static inline CCArena cc_heap_arena(size_t bytes);\n"
        "static inline void cc_heap_arena_free(CCArena* a);\n"
        "#include <ccc/cc_slice.cch>\n"
        "#include <ccc/cc_channel.cch>\n"
        "#include <ccc/std/task.cch>\n"
        "typedef struct CCChan CCChan;\n"
        "CCTaskIntptr cc_channel_send_task(CCChan* ch, const void* value, size_t value_size);\n"
        "CCTaskIntptr cc_channel_recv_task(CCChan* ch, void* out_value, size_t value_size);\n"
        "#ifndef CC_TASK_RESULT_PTR_OR_RETURN\n"
        "#define CC_TASK_RESULT_PTR_OR_RETURN(type, var) type* var = (type*)cc_task_result_ptr(sizeof(type)); if (!(var)) return NULL\n"
        "#endif\n"
        "#ifndef CC_SEND_TASK_OR_JOIN\n"
        "#define CC_SEND_TASK_OR_JOIN(tx_expr, task_var) do { int __cc_send_err = cc_chan_send((tx_expr).raw, &(task_var), sizeof(task_var)); if (__cc_send_err != 0) { (void)cc_block_on_intptr(task_var); } } while (0)\n"
        "#endif\n"
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

char* cc__prepend_reparse_prelude(const char* buf, size_t len, size_t* out_len, const char* original_path) {
    if (!buf || !out_len) return NULL;
    *out_len = 0;
    /* Prelude for reparse: must provide CC runtime types used by intermediate rewrites
       (e.g. CCNursery/CCClosure0) even when user source doesn't include the headers.
       cc_result.cch provides __CC_RESULT macro which handles parser-mode fallback. */
    const char* prelude =
        "#define __CC__ 1\n"
        "#define CC_PARSER_MODE 1\n"
        "#include <stdlib.h>\n"
        "#include <stdint.h>\n"
        "typedef intptr_t CCAbIntptr;\n"
        "#include <ccc/cc_closure.cch>\n"
        /* Closure-lowering macro stubs.  The full definitions live in
         * ccc/cc_closure_helper.h, but including that header in parser-mode
         * drags in function bodies that reference types the reparse AST does
         * not know about.  The final-UFCS reparse splices closure_defs (lifted
         * closure bodies) into src_ufcs, so the reparse must expand these
         * macros into syntactically-valid declarations of the env pointer;
         * otherwise TCC sees CC_CLOSURE_ENV_ALLOC(E, v) as an implicit call,
         * leaves `v` undeclared, and fails later `v->field` with "pointer
         * expected".  Stub to minimal declaration-shaped forms. */
        "#ifndef CC_CLOSURE0_DECL\n"
        "#define CC_CLOSURE0_DECL(n) static void* __cc_closure_entry_##n(void*); static CCClosure0 __cc_closure_make_##n(void)\n"
        "#endif\n"
        "#ifndef CC_CLOSURE0_SIMPLE\n"
        "#define CC_CLOSURE0_SIMPLE(n) static CCClosure0 __cc_closure_make_##n(void) { return cc_closure0_make(__cc_closure_entry_##n, (void*)0, (void*)0); } static void* __cc_closure_entry_##n(void* __p)\n"
        "#endif\n"
        "#ifndef CC_CLOSURE_ENV_ALLOC\n"
        "#define CC_CLOSURE_ENV_ALLOC(env_ty, var) env_ty* var = (env_ty*)0\n"
        "#endif\n"
        "#ifndef CC_CLOSURE_ENV_NURSERY_ALLOC\n"
        "#define CC_CLOSURE_ENV_NURSERY_ALLOC(nursery, env_ty, var) env_ty* var = (env_ty*)0\n"
        "#endif\n"
        "#ifndef CC_TASK_RESULT_PTR_OR_RETURN\n"
        "#define CC_TASK_RESULT_PTR_OR_RETURN(type, var) type* var = (type*)0\n"
        "#endif\n"
        "#ifndef CC_SEND_TASK_OR_JOIN\n"
        "#define CC_SEND_TASK_OR_JOIN(tx_expr, task_var) ((void)0)\n"
        "#endif\n"
        "#ifndef CC_TSAN_RELEASE\n"
        "#define CC_TSAN_RELEASE(addr) ((void)0)\n"
        "#endif\n"
        "#ifndef CC_TSAN_ACQUIRE\n"
        "#define CC_TSAN_ACQUIRE(addr) ((void)0)\n"
        "#endif\n"
        "#ifndef __cc_ret\n"
        "#define __cc_ret(id, value) ((void)(value))\n"
        "#endif\n"
        "#ifndef __cc_ret_ok\n"
        "#define __cc_ret_ok(id, value) ((void)(value))\n"
        "#endif\n"
        "#ifndef __cc_ret_err\n"
        "#define __cc_ret_err(id, err) ((void)(err))\n"
        "#endif\n"
        "#include <ccc/cc_nursery.cch>\n"
        "#include <ccc/cc_arena.cch>\n"
        "#include <ccc/cc_result.cch>\n"
        "#include <ccc/cc_optional.cch>\n"
        "/* Minimal std/prelude declarations used by reparse-time arena types/helpers */\n"
        "static inline size_t kilobytes(size_t n);\n"
        "static inline size_t megabytes(size_t n);\n"
        "static inline CCArena cc_heap_arena(size_t bytes);\n"
        "static inline void cc_heap_arena_free(CCArena* a);\n"
        "#include <ccc/cc_slice.cch>\n"
        "#include <ccc/std/task.cch>\n"
        "typedef struct CCChan CCChan;\n"
        "CCTaskIntptr cc_channel_send_task(CCChan* ch, const void* value, size_t value_size);\n"
        "CCTaskIntptr cc_channel_recv_task(CCChan* ch, void* out_value, size_t value_size);\n"
        "typedef struct { void (*fn)(void); } __cc_spawn_void_arg;\n"
        "typedef struct { void (*fn)(int); int arg; } __cc_spawn_int_arg;\n"
        "static void* __cc_spawn_thunk_void(void*);\n"
        "static void* __cc_spawn_thunk_int(void*);\n";
    char rel[1024];
    const char* lp = cc_path_rel_to_repo(original_path ? original_path : "<input>", rel, sizeof(rel));
    char line_buf[1024];
    int ln = snprintf(line_buf, sizeof(line_buf), "#line 1 \"%s\"\n", lp ? lp : "<input>");
    size_t pre_len = strlen(prelude);
    size_t line_len = (ln > 0 && (size_t)ln < sizeof(line_buf)) ? (size_t)ln : 0;
    size_t total = pre_len + line_len + len;
    char* out = (char*)malloc(total + 1);
    if (!out) return NULL;
    size_t off = 0;
    memcpy(out + off, prelude, pre_len);
    off += pre_len;
    if (line_len > 0) {
        memcpy(out + off, line_buf, line_len);
        off += line_len;
    }
    memcpy(out + off, buf, len);
    off += len;
    out[off] = '\0';
    *out_len = off;
    return out;
}