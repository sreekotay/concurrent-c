#include "executor.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "preprocess/emit_plan.h"

#ifdef CC_TCC_EXT_AVAILABLE
#include <libtcc.h>
#ifdef malloc
#undef malloc
#endif
#ifdef free
#undef free
#endif
#ifdef realloc
#undef realloc
#endif
#ifdef strdup
#undef strdup
#endif
#endif

static jmp_buf cc__exec_jb;
static clock_t cc__exec_start;
static int cc__exec_timeout_ms = 5000;

static void cc__exec_check_timeout(void) {
    if (cc__exec_timeout_ms <= 0) return;
    clock_t now = clock();
    if ((int)((now - cc__exec_start) * 1000 / CLOCKS_PER_SEC) > cc__exec_timeout_ms)
        longjmp(cc__exec_jb, 1);
}

static void cc__host_emit_raw(int anchor, const char* ptr, size_t len) {
    cc__exec_check_timeout();
    cc_emit_plan_host_emit_raw(anchor, ptr, len);
}

static void cc__host_instantiate_vec(const char* elem) {
    cc__exec_check_timeout();
    cc_emit_plan_host_instantiate_vec(elem);
}

static void cc__host_instantiate_map(const char* key, const char* val) {
    cc__exec_check_timeout();
    cc_emit_plan_host_instantiate_map(key, val);
}

static void cc__host_instantiate_chan(const char* elem) {
    cc__exec_check_timeout();
    cc_emit_plan_host_instantiate_chan(elem);
}

#ifdef CC_TCC_EXT_AVAILABLE
static void cc__exec_err_silent(void* opaque, const char* msg) {
    (void)opaque;
    if (getenv("CC_DEBUG_COMPTIME_EXEC"))
        fprintf(stderr, "[cc:comptime-exec] tcc: %s\n", msg);
}

static const char* cc__exec_lib_dir(char* buf, size_t cap) {
    const char* cands[6];
    size_t nc = 0;
    const char* env = getenv("CC_TCC_LIB_PATH");
    if (env && env[0]) cands[nc++] = env;
#ifdef CC_TCC_LIB_DIR
    cands[nc++] = CC_TCC_LIB_DIR;
#endif
    cands[nc++] = "third_party/tcc";
    cands[nc++] = "../third_party/tcc";
    cands[nc++] = "../../third_party/tcc";
    for (size_t i = 0; i < nc; i++) {
        char probe[1024];
        if (snprintf(probe, sizeof(probe), "%s/libtcc1.a", cands[i]) >= (int)sizeof(probe))
            continue;
        if (access(probe, R_OK) == 0) {
            snprintf(buf, cap, "%s", cands[i]);
            return buf;
        }
    }
    return NULL;
}

/* Minimal comptime TU prelude: host API externs + cc_emit_format wrapper. */
static const char CC__EXEC_PRELUDE[] =
    "#include <stddef.h>\n"
    "#include <stdio.h>\n"
    "#include <stdarg.h>\n"
    "#include <string.h>\n"
    "typedef enum { CC_EMIT_AFTER_PRELUDE=0, CC_EMIT_BEFORE_FIRST_USE=1,"
    " CC_EMIT_AT_COMPTIME_SITE=2 } CCEmitAnchor;\n"
    "extern void cc_emit_raw(int anchor, const char* ptr, size_t len);\n"
    "extern void cc_instantiate_vec(const char* elem);\n"
    "extern void cc_instantiate_map(const char* key, const char* val);\n"
    "extern void cc_instantiate_chan(const char* elem);\n"
    "static int cc_emit_cstr(int anchor, const char* cstr) {\n"
    "  if (!cstr) return 0;\n"
    "  cc_emit_raw(anchor, cstr, strlen(cstr));\n"
    "  return 0;\n"
    "}\n"
    "static int cc_emit_format(int anchor, const char* fmt, ...) {\n"
    "  char buf[16384];\n"
    "  va_list ap;\n"
    "  va_start(ap, fmt);\n"
    "  vsnprintf(buf, sizeof(buf), fmt, ap);\n"
    "  va_end(ap);\n"
    "  cc_emit_raw(anchor, buf, strlen(buf));\n"
    "  return 0;\n"
    "}\n";

static char* cc__exec_build_tu(const char* body, size_t body_len) {
    static const char entry[] = "\nvoid __cc_ct_entry(void) {\n";
    static const char tail[] = "\n}\n";
    size_t pre = sizeof(CC__EXEC_PRELUDE) - 1;
    size_t ent = sizeof(entry) - 1;
    size_t tl = sizeof(tail) - 1;
    char* s = (char*)malloc(pre + ent + body_len + tl + 1);
    if (!s) return NULL;
    size_t o = 0;
    memcpy(s + o, CC__EXEC_PRELUDE, pre); o += pre;
    memcpy(s + o, entry, ent); o += ent;
    memcpy(s + o, body, body_len); o += body_len;
    memcpy(s + o, tail, tl); o += tl;
    s[o] = '\0';
    return s;
}

static int cc__exec_run_tu(const char* tu, char* err_buf, size_t err_sz) {
    TCCState* s = tcc_new();
    if (!s) {
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "tcc_new failed");
        return -1;
    }
    tcc_set_error_func(s, NULL, cc__exec_err_silent);
    {
        char dirbuf[1024];
        const char* libdir = cc__exec_lib_dir(dirbuf, sizeof(dirbuf));
        if (libdir) {
            tcc_set_lib_path(s, libdir);
            tcc_add_library_path(s, libdir);
        }
    }
    if (tcc_set_output_type(s, TCC_OUTPUT_MEMORY) < 0) {
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "tcc_set_output_type failed");
        tcc_delete(s);
        return -1;
    }
    tcc_add_symbol(s, "cc_emit_raw", (void*)cc__host_emit_raw);
    tcc_add_symbol(s, "cc_instantiate_vec", (void*)cc__host_instantiate_vec);
    tcc_add_symbol(s, "cc_instantiate_map", (void*)cc__host_instantiate_map);
    tcc_add_symbol(s, "cc_instantiate_chan", (void*)cc__host_instantiate_chan);

    if (tcc_compile_string(s, tu) < 0) {
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "comptime TU compile failed");
        tcc_delete(s);
        return -1;
    }
    if (tcc_relocate(s) < 0) {
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "comptime TU relocate failed");
        tcc_delete(s);
        return -1;
    }
    void* sym = tcc_get_symbol(s, "__cc_ct_entry");
    if (!sym) sym = tcc_get_symbol(s, "___cc_ct_entry");
    if (!sym) {
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "__cc_ct_entry not found");
        tcc_delete(s);
        return -1;
    }
    typedef void (*CCCtEntryFn)(void);
    ((CCCtEntryFn)sym)();
    tcc_delete(s);
    return 0;
}
#endif /* CC_TCC_EXT_AVAILABLE */

int cc_comptime_exec_block_body(const char* body, size_t body_len,
                                const CCComptimeExecOpts* opts,
                                char* err_buf, size_t err_sz) {
    if (!body || body_len == 0) {
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "empty @comptime block body");
        return -1;
    }
#ifndef CC_TCC_EXT_AVAILABLE
    (void)opts;
    if (err_buf && err_sz) snprintf(err_buf, err_sz, "libtcc not available");
    return -1;
#else
    {
        const char* tenv = getenv("CC_COMPTIME_EXEC_TIMEOUT_MS");
        if (tenv && tenv[0]) cc__exec_timeout_ms = atoi(tenv);
    }
    cc_emit_plan_host_ctx_begin(opts ? opts->site_pos : 0);
    cc__exec_start = clock();

    char* tu = cc__exec_build_tu(body, body_len);
    if (!tu) {
        cc_emit_plan_host_ctx_end();
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "OOM building comptime TU");
        return -1;
    }
    if (getenv("CC_DEBUG_COMPTIME_EXEC_DUMP")) {
        FILE* df = fopen(getenv("CC_DEBUG_COMPTIME_EXEC_DUMP"), "wb");
        if (df) { fwrite(tu, 1, strlen(tu), df); fclose(df); }
    }

    int rc = 0;
    if (setjmp(cc__exec_jb) != 0) {
        if (err_buf && err_sz)
            snprintf(err_buf, err_sz, "comptime execution timed out (%dms)",
                     cc__exec_timeout_ms);
        rc = -1;
    } else {
        rc = cc__exec_run_tu(tu, err_buf, err_sz);
    }
    free(tu);
    cc_emit_plan_host_ctx_end();
    (void)opts;
    return rc;
#endif
}
