#include "../build/host_cc_profile.h"
#include "const_eval.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef CC_TCC_EXT_AVAILABLE
#include <libtcc.h>
/* libtcc.h may #define malloc/free/strdup to its own allocators for the TCC
 * internals; undo that so this TU uses libc's. */
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

static void cc__ce_err_silent(void* opaque, const char* msg) {
    (void)opaque;
    if (getenv("CC_DEBUG_CONST_EVAL")) {
        fprintf(stderr, "[cc:const-eval] tcc: %s\n", msg);
    }
}

/* Resolve the directory that holds `libtcc1.a` (TCC's runtime support lib,
 * required for TCC_OUTPUT_MEMORY relocation).  Tries the env override, then the
 * build-time absolute default, then a few cwd-relative candidates.  Returns a
 * pointer into `buf` on success, or NULL if none contains libtcc1.a. */
static const char* cc__ce_lib_dir(char* buf, size_t cap) {
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

/* Build "<prelude>\nlong long __cc_ce_result = (long long)(<expr>);\n". */
static char* cc__ce_build_tu(const char* prelude, const char* expr) {
    static const char A[] = "\nlong long __cc_ce_result = (long long)(";
    static const char B[] = ");\n";
    size_t pre = prelude ? strlen(prelude) : 0;
    size_t ex = strlen(expr);
    size_t cap = pre + ex + sizeof(A) + sizeof(B); /* sizeofs include the NULs */
    char* s = (char*)malloc(cap);
    size_t o = 0;
    if (!s) return NULL;
    if (pre) { memcpy(s + o, prelude, pre); o += pre; }
    memcpy(s + o, A, sizeof(A) - 1); o += sizeof(A) - 1;
    memcpy(s + o, expr, ex); o += ex;
    memcpy(s + o, B, sizeof(B) - 1); o += sizeof(B) - 1;
    s[o] = '\0';
    return s;
}
#endif /* CC_TCC_EXT_AVAILABLE */

int cc_tcc_eval_const_expr(const char* prelude, const char* expr, int64_t* out) {
#ifndef CC_TCC_EXT_AVAILABLE
    (void)prelude; (void)expr; (void)out;
    return 0;
#else
    if (!expr || !out) return 0;

    TCCState* s = tcc_new();
    if (!s) return 0;
    tcc_set_error_func(s, NULL, cc__ce_err_silent);
    tcc_set_options(s, CC_HOST_C_STD_OPTION);

    /* Point TCC at its runtime support lib before choosing the output type so
     * relocation can find libtcc1.a. */
    {
        char dirbuf[1024];
        const char* libdir = cc__ce_lib_dir(dirbuf, sizeof(dirbuf));
        if (libdir) {
            tcc_set_lib_path(s, libdir);
            tcc_add_library_path(s, libdir);
        }
    }

    if (tcc_set_output_type(s, TCC_OUTPUT_MEMORY) < 0) {
        tcc_delete(s);
        return 0;
    }

    char* tu = cc__ce_build_tu(prelude, expr);
    if (!tu) { tcc_delete(s); return 0; }
    int rc = tcc_compile_string(s, tu);
    free(tu);
    if (rc < 0) { tcc_delete(s); return 0; }
    if (tcc_relocate(s) < 0) { tcc_delete(s); return 0; }

    /* TCC stores symbols under the C name; some targets prefix a leading
     * underscore.  Try the bare name first, then the underscored form. */
    void* sym = tcc_get_symbol(s, "__cc_ce_result");
    if (!sym) sym = tcc_get_symbol(s, "___cc_ce_result");
    if (!sym) { tcc_delete(s); return 0; }

    int64_t v;
    memcpy(&v, sym, sizeof(v));
    tcc_delete(s);
    *out = v;
    return 1;
#endif
}
