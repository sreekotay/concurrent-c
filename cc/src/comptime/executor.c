#include "executor.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../comptime/emit_tpl_prelude.inc.h"
#include "preprocess/emit_plan.h"
#include "util/text.h"

#define CC_COMPTIME_FN_MAX 32
#define CC_COMPTIME_FN_NAME_MAX 64
#define CC_COMPTIME_FN_DEF_MAX 16384

typedef struct CCComptimeFnEntry {
    char  name[CC_COMPTIME_FN_NAME_MAX];
    char* def;
    size_t def_len;
    int   def_line;
} CCComptimeFnEntry;

static CCComptimeFnEntry cc__comptime_fns[CC_COMPTIME_FN_MAX];
static size_t cc__comptime_fn_count = 0;
static char* cc__comptime_fn_defs_blob = NULL;
static size_t cc__comptime_fn_defs_len = 0;
static char cc__comptime_fn_scan_err[512];

void cc_comptime_fn_registry_clear(void) {
    cc__comptime_fn_scan_err[0] = '\0';
    for (size_t i = 0; i < cc__comptime_fn_count; i++)
        free(cc__comptime_fns[i].def);
    cc__comptime_fn_count = 0;
    free(cc__comptime_fn_defs_blob);
    cc__comptime_fn_defs_blob = NULL;
    cc__comptime_fn_defs_len = 0;
}

static void cc__comptime_fn_rebuild_blob(void) {
    free(cc__comptime_fn_defs_blob);
    cc__comptime_fn_defs_blob = NULL;
    cc__comptime_fn_defs_len = 0;
    for (size_t i = 0; i < cc__comptime_fn_count; i++) {
        const CCComptimeFnEntry* e = &cc__comptime_fns[i];
        size_t need = cc__comptime_fn_defs_len + e->def_len + 2;
        char* nb = (char*)realloc(cc__comptime_fn_defs_blob, need);
        if (!nb) return;
        cc__comptime_fn_defs_blob = nb;
        memcpy(cc__comptime_fn_defs_blob + cc__comptime_fn_defs_len, e->def, e->def_len);
        cc__comptime_fn_defs_len += e->def_len;
        cc__comptime_fn_defs_blob[cc__comptime_fn_defs_len++] = '\n';
    }
    if (cc__comptime_fn_defs_blob)
        cc__comptime_fn_defs_blob[cc__comptime_fn_defs_len] = '\0';
}

static int cc__line_at(const char* src, size_t at) {
    int line = 1;
    if (!src) return 1;
    for (size_t k = 0; k < at && src[k]; k++)
        if (src[k] == '\n') line++;
    return line;
}

static int cc__comptime_fn_register(const char* name, const char* def, size_t def_len,
                                    int def_line) {
    if (!name || !name[0] || !def || def_len == 0) return 0;
    if (cc__comptime_fn_count >= CC_COMPTIME_FN_MAX) {
        snprintf(cc__comptime_fn_scan_err, sizeof(cc__comptime_fn_scan_err),
                 "too many @comptime functions (max %d)", CC_COMPTIME_FN_MAX);
        return -1;
    }
    for (size_t i = 0; i < cc__comptime_fn_count; i++) {
        if (strcmp(cc__comptime_fns[i].name, name) == 0) {
            char* nd = (char*)malloc(def_len + 1);
            if (!nd) return 0;
            memcpy(nd, def, def_len);
            nd[def_len] = '\0';
            free(cc__comptime_fns[i].def);
            cc__comptime_fns[i].def = nd;
            cc__comptime_fns[i].def_len = def_len;
            cc__comptime_fns[i].def_line = def_line;
            cc__comptime_fn_rebuild_blob();
            return 1;
        }
    }
    {
        CCComptimeFnEntry* e = &cc__comptime_fns[cc__comptime_fn_count++];
        snprintf(e->name, sizeof(e->name), "%s", name);
        e->def = (char*)malloc(def_len + 1);
        if (!e->def) { cc__comptime_fn_count--; return 0; }
        memcpy(e->def, def, def_len);
        e->def[def_len] = '\0';
        e->def_len = def_len;
        e->def_line = def_line;
        cc__comptime_fn_rebuild_blob();
    }
    return 1;
}

int cc_comptime_fn_is_registered(const char* name) {
    if (!name) return 0;
    for (size_t i = 0; i < cc__comptime_fn_count; i++)
        if (strcmp(cc__comptime_fns[i].name, name) == 0) return 1;
    return 0;
}

const char* cc_comptime_fn_registry_defs(void) {
    return cc__comptime_fn_defs_blob ? cc__comptime_fn_defs_blob : "";
}

const char* cc_comptime_fn_registry_lookup_def(const char* name) {
    if (!name) return NULL;
    for (size_t i = 0; i < cc__comptime_fn_count; i++)
        if (strcmp(cc__comptime_fns[i].name, name) == 0)
            return cc__comptime_fns[i].def;
    return NULL;
}

int cc_comptime_fn_registry_lookup_line(const char* name) {
    if (!name) return 0;
    for (size_t i = 0; i < cc__comptime_fn_count; i++)
        if (strcmp(cc__comptime_fns[i].name, name) == 0)
            return cc__comptime_fns[i].def_line;
    return 0;
}

const char* cc_comptime_fn_registry_scan_error(void) {
    return cc__comptime_fn_scan_err[0] ? cc__comptime_fn_scan_err : NULL;
}

static int cc__try_scan_comptime_fn(const char* src, size_t len, size_t at, size_t* out_end) {
    size_t p, def_start, lparen = 0, rparen = 0, body_l = 0, body_r = 0;
    size_t name_start = 0, name_end = 0;
    char name[CC_COMPTIME_FN_NAME_MAX];
    char def[CC_COMPTIME_FN_DEF_MAX];
    size_t dlen;

    if (!src || at >= len || src[at] != '@') return 0;
    if (!cc_match_ident_kw(src, len, at + 1, "comptime")) return 0;
    p = cc_skip_ws_and_comments(src, len, at + 1 + (sizeof("comptime") - 1));
    if (p >= len || src[p] == '{') return 0;
    /* `@comptime for` / `@comptime if` are control-flow, not fn definitions. */
    if (cc_match_ident_kw(src, len, p, "for")) return 0;
    if (cc_match_ident_kw(src, len, p, "if")) return 0;

    def_start = p;
    for (p = def_start; p < len; p++) {
        if (src[p] == '(') {
            lparen = p;
            break;
        }
    }
    if (!lparen) return 0;
    if (!cc_find_matching_paren(src, len, lparen, &rparen)) return 0;
    name_end = lparen;
    while (name_end > def_start && (src[name_end - 1] == ' ' || src[name_end - 1] == '\t' ||
                                    src[name_end - 1] == '\n' || src[name_end - 1] == '\r'))
        name_end--;
    name_start = name_end;
    while (name_start > def_start && cc_is_ident_char(src[name_start - 1])) name_start--;
    if (name_start == name_end || name_end - name_start >= sizeof(name)) return 0;
    memcpy(name, src + name_start, name_end - name_start);
    name[name_end - name_start] = '\0';

    p = cc_skip_ws_and_comments(src, len, rparen + 1);
    if (p >= len || src[p] != '{') return 0;
    body_l = p;
    if (!cc_find_matching_brace(src, len, body_l, &body_r)) return 0;

    dlen = body_r + 1 - def_start;
    if (dlen >= sizeof(def)) {
        snprintf(cc__comptime_fn_scan_err, sizeof(cc__comptime_fn_scan_err),
                 "@comptime function '%s' body exceeds %zu bytes (max %zu)",
                 name, dlen, sizeof(def) - 1);
        return -1;
    }
    memcpy(def, src + def_start, dlen);
    def[dlen] = '\0';
    if (cc__comptime_fn_register(name, def, dlen, cc__line_at(src, def_start)) < 0)
        return -1;
    if (out_end) *out_end = body_r + 1;
    return 1;
}

int cc_comptime_fn_registry_scan(const char* src, size_t len) {
    size_t i = 0;
    if (!src || len == 0) return 0;
    cc_comptime_fn_registry_clear();
    while (i < len) {
        size_t end = 0;
        int sr = 0;
        if (src[i] == '@')
            sr = cc__try_scan_comptime_fn(src, len, i, &end);
        if (sr < 0) return -1;
        if (sr) {
            i = end;
            continue;
        }
        i++;
    }
    return (int)cc__comptime_fn_count;
}

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

/* Minimal comptime TU prelude: host API externs + emit-template + cc_emit_format. */
static const char CC__EXEC_PRELUDE[] = CC_COMPTIME_EMIT_TPL_PRELUDE;

static char* cc__exec_build_tu(const char* body, size_t body_len) {
    static const char entry[] = "\nvoid __cc_ct_entry(void) {\n";
    static const char tail[] = "\n}\n";
    const char* fndefs = cc_comptime_fn_registry_defs();
    size_t fndef_len = fndefs ? strlen(fndefs) : 0;
    size_t pre = sizeof(CC__EXEC_PRELUDE) - 1;
    size_t ent = sizeof(entry) - 1;
    size_t tl = sizeof(tail) - 1;
    char* s = (char*)malloc(pre + fndef_len + ent + body_len + tl + 1);
    if (!s) return NULL;
    size_t o = 0;
    memcpy(s + o, CC__EXEC_PRELUDE, pre); o += pre;
    if (fndef_len) { memcpy(s + o, fndefs, fndef_len); o += fndef_len; }
    memcpy(s + o, entry, ent); o += ent;
    memcpy(s + o, body, body_len); o += body_len;
    memcpy(s + o, tail, tl); o += tl;
    s[o] = '\0';
    return s;
}

static char* cc__exec_build_eval_tu(const char* expr) {
    static const char hdr[] = "\nlong long __cc_ce_result;\nvoid __cc_ct_entry(void) {\n"
                              "  __cc_ce_result = (long long)(";
    static const char tail[] = ");\n}\n";
    const char* fndefs = cc_comptime_fn_registry_defs();
    size_t fndef_len = fndefs ? strlen(fndefs) : 0;
    size_t ex = expr ? strlen(expr) : 0;
    size_t pre = sizeof(CC__EXEC_PRELUDE) - 1;
    size_t hl = sizeof(hdr) - 1;
    size_t tl = sizeof(tail) - 1;
    char* s = (char*)malloc(pre + fndef_len + hl + ex + tl + 1);
    if (!s) return NULL;
    size_t o = 0;
    memcpy(s + o, CC__EXEC_PRELUDE, pre); o += pre;
    if (fndef_len) { memcpy(s + o, fndefs, fndef_len); o += fndef_len; }
    memcpy(s + o, hdr, hl); o += hl;
    memcpy(s + o, expr, ex); o += ex;
    memcpy(s + o, tail, tl); o += tl;
    s[o] = '\0';
    return s;
}

static int cc__exec_run_tu_ex(const char* tu, char* err_buf, size_t err_sz, int64_t* out_int);

static int cc__exec_run_tu(const char* tu, char* err_buf, size_t err_sz) {
    return cc__exec_run_tu_ex(tu, err_buf, err_sz, NULL);
}

static int cc__exec_run_tu_ex(const char* tu, char* err_buf, size_t err_sz, int64_t* out_int) {
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
    tcc_add_symbol(s, "cc_reflect_field_count", (void*)cc_reflect_field_count);
    tcc_add_symbol(s, "cc_reflect_field_name", (void*)cc_reflect_field_name);
    tcc_add_symbol(s, "cc_reflect_field_type", (void*)cc_reflect_field_type);

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
    if (out_int) {
        void* res = tcc_get_symbol(s, "__cc_ce_result");
        if (!res) res = tcc_get_symbol(s, "___cc_ce_result");
        if (!res) {
            if (err_buf && err_sz) snprintf(err_buf, err_sz, "__cc_ce_result not found");
            tcc_delete(s);
            return -1;
        }
        int64_t v;
        memcpy(&v, res, sizeof(v));
        *out_int = v;
    }
    tcc_delete(s);
    return 0;
}
#endif /* CC_TCC_EXT_AVAILABLE */

int cc_comptime_exec_eval_int(const char* expr,
                              const CCComptimeExecOpts* opts,
                              int64_t* out,
                              char* err_buf, size_t err_sz) {
    if (!expr || !out) {
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "invalid eval_int args");
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
    cc__exec_start = clock();
    char* tu = cc__exec_build_eval_tu(expr);
    if (!tu) {
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "OOM building eval TU");
        return -1;
    }
    int rc = 0;
    if (setjmp(cc__exec_jb) != 0) {
        if (err_buf && err_sz)
            snprintf(err_buf, err_sz, "comptime eval timed out (%dms)", cc__exec_timeout_ms);
        rc = -1;
    } else {
        rc = cc__exec_run_tu_ex(tu, err_buf, err_sz, out);
    }
    free(tu);
    (void)opts;
    return rc;
#endif
}

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

/* --- generated-fragment validation (factory/template emit-site check) --- */

#ifdef CC_TCC_EXT_AVAILABLE

/* Minimal prelude for validating a standalone generated fragment.  Pulls in the
 * common scalar/typedef surface a factory definition is allowed to assume.  A
 * `#line 1` marker follows so libtcc reports fragment-relative line numbers. */
static const char CC__FRAG_PRELUDE[] =
    "#include <stddef.h>\n"
    "#include <stdint.h>\n"
    "#include <stdbool.h>\n"
    "#include <stdio.h>\n"
    "#include <string.h>\n"
    "#line 1 \"<generic-fragment>\"\n";

typedef struct {
    char buf[512];
    int  got;       /* a message was captured */
    int  line;      /* fragment-relative line parsed from the first message */
} CCFragErrSink;

static void cc__frag_err_capture(void* opaque, const char* msg) {
    CCFragErrSink* sink = (CCFragErrSink*)opaque;
    if (!sink || !msg) return;
    if (getenv("CC_DEBUG_COMPTIME_EXEC"))
        fprintf(stderr, "[cc:frag-validate] tcc: %s\n", msg);
    if (sink->got) return;       /* keep only the first message */
    sink->got = 1;
    snprintf(sink->buf, sizeof(sink->buf), "%s", msg);
    /* Messages look like `<generic-fragment>:LINE: error: ...`; pull the line. */
    {
        const char* tag = "<generic-fragment>:";
        const char* p = strstr(msg, tag);
        if (p) {
            p += strlen(tag);
            int v = 0, any = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
            if (any) sink->line = v;
        }
    }
}

/* True when the message names a syntax defect intrinsic to the fragment, rather
 * than a missing dependency (unknown type, undeclared name) that only the
 * merged TU can resolve.  High-precision allowlist: never blocks on context. */
static int cc__frag_msg_is_syntax(const char* msg) {
    if (!msg || !msg[0]) return 0;
    return strstr(msg, "expected") != NULL ||
           strstr(msg, "stray") != NULL ||
           strstr(msg, "unterminated") != NULL ||
           strstr(msg, "invalid number") != NULL ||
           strstr(msg, "_Generic") != NULL ||
           strstr(msg, "controlling expression") != NULL ||
           strstr(msg, "incompatible types") != NULL;
}

int cc_comptime_validate_c_fragment(const char* fragment,
                                    int* out_frag_line,
                                    char* err_buf, size_t err_sz) {
    if (out_frag_line) *out_frag_line = 0;
    if (err_buf && err_sz) err_buf[0] = '\0';
    if (!fragment || !fragment[0]) return 0;
    if (getenv("CC_NO_FRAGMENT_VALIDATE")) return 0;

    size_t pre = sizeof(CC__FRAG_PRELUDE) - 1;
    size_t fl = strlen(fragment);
    char* tu = (char*)malloc(pre + fl + 2);
    if (!tu) return 0;   /* OOM: don't block compilation on a missing check */
    memcpy(tu, CC__FRAG_PRELUDE, pre);
    memcpy(tu + pre, fragment, fl);
    tu[pre + fl] = '\n';
    tu[pre + fl + 1] = '\0';

    TCCState* s = tcc_new();
    if (!s) { free(tu); return 0; }
    CCFragErrSink sink;
    sink.buf[0] = '\0';
    sink.got = 0;
    sink.line = 0;
    tcc_set_error_func(s, &sink, cc__frag_err_capture);
    {
        char dirbuf[1024];
        const char* libdir = cc__exec_lib_dir(dirbuf, sizeof(dirbuf));
        if (libdir) { tcc_set_lib_path(s, libdir); tcc_add_library_path(s, libdir); }
    }
    if (tcc_set_output_type(s, TCC_OUTPUT_MEMORY) < 0) { tcc_delete(s); free(tu); return 0; }

    int compiled = tcc_compile_string(s, tu);
    tcc_delete(s);
    free(tu);

    if (compiled >= 0) return 0;                 /* clean parse */
    if (!cc__frag_msg_is_syntax(sink.buf)) return 0;  /* missing context: skip */

    if (err_buf && err_sz) snprintf(err_buf, err_sz, "%s", sink.buf);
    if (out_frag_line) *out_frag_line = sink.line;
    return -1;
}

#else  /* !CC_TCC_EXT_AVAILABLE */

int cc_comptime_validate_c_fragment(const char* fragment,
                                    int* out_frag_line,
                                    char* err_buf, size_t err_sz) {
    (void)fragment;
    if (out_frag_line) *out_frag_line = 0;
    if (err_buf && err_sz) err_buf[0] = '\0';
    return 0;   /* no validator without libtcc */
}

#endif /* CC_TCC_EXT_AVAILABLE */
