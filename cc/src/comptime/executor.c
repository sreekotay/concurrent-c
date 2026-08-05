#include "../build/host_cc_profile.h"
#include "executor.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../comptime/emit_tpl_prelude.inc.h"
#include "preprocess/emit_limits.h"
#include "preprocess/emit_plan.h"
#include "util/text.h"

#include <ccc/cc_arena.cch>

#define CC_COMPTIME_FN_MAX 32
#define CC_COMPTIME_FN_NAME_MAX 64

typedef struct CCComptimeFnEntry {
    char  name[CC_COMPTIME_FN_NAME_MAX];
    char* def;
    size_t def_len;
    int   def_line;       /* #line-resolved source line of the definition */
    char* def_file;       /* #line-resolved source file, or NULL if none seen */
} CCComptimeFnEntry;

static CCComptimeFnEntry cc__comptime_fns[CC_COMPTIME_FN_MAX];
static size_t cc__comptime_fn_count = 0;
static char* cc__comptime_fn_defs_blob = NULL;
static size_t cc__comptime_fn_defs_len = 0;
static char* cc__comptime_fn_prelude = NULL;
static char cc__comptime_fn_scan_err[512];

static void cc__comptime_fn_rebuild_blob(void);

void cc_comptime_fn_registry_clear(void) {
    cc__comptime_fn_scan_err[0] = '\0';
    for (size_t i = 0; i < cc__comptime_fn_count; i++) {
        free(cc__comptime_fns[i].def);
        free(cc__comptime_fns[i].def_file);
        cc__comptime_fns[i].def_file = NULL;
    }
    cc__comptime_fn_count = 0;
    free(cc__comptime_fn_defs_blob);
    cc__comptime_fn_defs_blob = NULL;
    cc__comptime_fn_defs_len = 0;
    /* Keep cc__comptime_fn_prelude: harvest installs it before scan, and
     * scan() clears the function table on every TU. Prelude is replaced
     * explicitly via cc_comptime_fn_registry_set_prelude. */
    cc__comptime_fn_rebuild_blob();
}

void cc_comptime_fn_registry_set_prelude(const char* prelude) {
    free(cc__comptime_fn_prelude);
    cc__comptime_fn_prelude = NULL;
    if (prelude && prelude[0]) cc__comptime_fn_prelude = strdup(prelude);
    cc__comptime_fn_rebuild_blob();
}

void cc_comptime_fn_registry_append_prelude(const char* text) {
    size_t old_len, add_len;
    char* nb;
    if (!text || !text[0]) return;
    if (!cc__comptime_fn_prelude || !cc__comptime_fn_prelude[0]) {
        cc_comptime_fn_registry_set_prelude(text);
        return;
    }
    if (strstr(cc__comptime_fn_prelude, text)) return;
    old_len = strlen(cc__comptime_fn_prelude);
    add_len = strlen(text);
    nb = (char*)malloc(old_len + 1 + add_len + 1);
    if (!nb) return;
    memcpy(nb, cc__comptime_fn_prelude, old_len);
    nb[old_len] = '\n';
    memcpy(nb + old_len + 1, text, add_len + 1);
    free(cc__comptime_fn_prelude);
    cc__comptime_fn_prelude = nb;
    cc__comptime_fn_rebuild_blob();
}

static void cc__comptime_fn_rebuild_blob(void) {
    free(cc__comptime_fn_defs_blob);
    cc__comptime_fn_defs_blob = NULL;
    cc__comptime_fn_defs_len = 0;
    if (cc__comptime_fn_prelude && cc__comptime_fn_prelude[0]) {
        size_t plen = strlen(cc__comptime_fn_prelude);
        char* nb = (char*)malloc(plen + 2);
        if (!nb) return;
        memcpy(nb, cc__comptime_fn_prelude, plen);
        nb[plen] = '\n';
        nb[plen + 1] = '\0';
        cc__comptime_fn_defs_blob = nb;
        cc__comptime_fn_defs_len = plen + 1;
    }
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

/* Parse a `#line N "file"` (C99) or `# N "file"` (GCC) directive line.  On a
 * match, writes N to *out_n and the optional filename (sans quotes) into fbuf,
 * returning 1.  Returns 0 for any non-directive line. */
static int cc__parse_line_directive(const char* s, size_t len,
                                    long* out_n, char* fbuf, size_t fcap) {
    size_t p = 0;
    while (p < len && (s[p] == ' ' || s[p] == '\t')) p++;
    if (p >= len || s[p] != '#') return 0;
    p++;
    while (p < len && (s[p] == ' ' || s[p] == '\t')) p++;
    if (p + 4 <= len && memcmp(s + p, "line", 4) == 0) {
        p += 4;
        while (p < len && (s[p] == ' ' || s[p] == '\t')) p++;
    }
    if (p >= len || s[p] < '0' || s[p] > '9') return 0;
    long n = 0;
    while (p < len && s[p] >= '0' && s[p] <= '9') n = n * 10 + (s[p++] - '0');
    if (out_n) *out_n = n;
    if (fbuf && fcap) fbuf[0] = '\0';
    while (p < len && (s[p] == ' ' || s[p] == '\t')) p++;
    if (p < len && s[p] == '"') {
        p++;
        size_t fl = 0;
        while (p < len && s[p] != '"') {
            if (fbuf && fl + 1 < fcap) fbuf[fl++] = s[p];
            p++;
        }
        if (fbuf && fcap) fbuf[fl] = '\0';
    }
    return 1;
}

/* Resolve the source origin (file + line) of byte offset `at` in `src`,
 * honoring any `#line`/`# N "file"` directives in between.  Without directives
 * this degrades to a plain newline count (line N of the buffer, file unknown). */
static int cc__resolve_origin(const char* src, size_t at,
                              char* file_out, size_t file_cap, int* line_out) {
    int line = 1;
    char file[1024]; file[0] = '\0';
    if (file_out && file_cap) file_out[0] = '\0';
    if (!src) { if (line_out) *line_out = 1; return 0; }
    size_t line_start = 0;
    size_t len = at + 1; /* only need to scan up to and including `at`'s line */
    for (;;) {
        size_t line_end = line_start;
        while (src[line_end] && src[line_end] != '\n') line_end++;
        if (at <= line_end) break;  /* `at` lies on this physical line */
        long n = 0;
        char fbuf[1024];
        if (cc__parse_line_directive(src + line_start, line_end - line_start, &n, fbuf, sizeof(fbuf))) {
            line = (int)n;                 /* next physical line is source line N */
            if (fbuf[0]) snprintf(file, sizeof(file), "%s", fbuf);
        } else {
            line++;
        }
        if (!src[line_end]) break;
        line_start = line_end + 1;
    }
    (void)len;
    if (line_out) *line_out = line;
    if (file_out && file_cap && file[0]) snprintf(file_out, file_cap, "%s", file);
    return file[0] ? 1 : 0;
}

static int cc__comptime_fn_register(const char* name, const char* def, size_t def_len,
                                    int def_line, const char* def_file) {
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
            free(cc__comptime_fns[i].def_file);
            cc__comptime_fns[i].def_file = (def_file && def_file[0]) ? strdup(def_file) : NULL;
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
        e->def_file = (def_file && def_file[0]) ? strdup(def_file) : NULL;
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

const char* cc_comptime_fn_registry_lookup_file(const char* name) {
    if (!name) return NULL;
    for (size_t i = 0; i < cc__comptime_fn_count; i++)
        if (strcmp(cc__comptime_fns[i].name, name) == 0)
            return cc__comptime_fns[i].def_file;
    return NULL;
}

const char* cc_comptime_fn_registry_scan_error(void) {
    return cc__comptime_fn_scan_err[0] ? cc__comptime_fn_scan_err : NULL;
}

static int cc__try_scan_comptime_fn(const char* src, size_t len, size_t at, size_t* out_end) {
    size_t p, def_start, lparen = 0, rparen = 0, body_l = 0, body_r = 0;
    size_t name_start = 0, name_end = 0;
    char name[CC_COMPTIME_FN_NAME_MAX];
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
    /* The registry copies (and NUL-terminates) the span itself, so the
     * body is passed straight from the source buffer — a fixed
     * intermediary here once imposed a silent 16K body cap for no
     * benefit beyond its own existence. */
    {
        char ofile[1024];
        int oline = 1;
        cc__resolve_origin(src, def_start, ofile, sizeof(ofile), &oline);
        if (cc__comptime_fn_register(name, src + def_start, dlen, oline,
                                     ofile[0] ? ofile : NULL) < 0)
            return -1;
    }
    if (out_end) *out_end = body_r + 1;
    return 1;
}

int cc_comptime_fn_registry_scan(const char* src, size_t len) {
    size_t i = 0;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    if (!src || len == 0) return 0;
    cc_comptime_fn_registry_clear();
    /* Skip comments and string/char literals: `@comptime` in prose (e.g. a
     * comment that says "a @comptime function") or inside a string is not a
     * real definition and must not be scanned as one. */
    while (i < len) {
        char c = src[i];
        char c2 = (i + 1 < len) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\') { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\') { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        if (c == '@') {
            size_t end = 0;
            int sr = cc__try_scan_comptime_fn(src, len, i, &end);
            if (sr < 0) return -1;
            if (sr) { i = end; continue; }
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
/* The timeout longjmp target (cc__exec_jb) is only valid while a @comptime
 * block/eval is running under its setjmp.  Compiled factories are invoked later
 * at use sites (outside any setjmp), so host verbs they call must not longjmp on
 * a stale buffer — gate the check on actually being inside a block run. */
static int cc__exec_in_block = 0;

static void cc__exec_check_timeout(void) {
    if (!cc__exec_in_block) return;
    if (cc__exec_timeout_ms <= 0) return;
    clock_t now = clock();
    if ((int)((now - cc__exec_start) * 1000 / CLOCKS_PER_SEC) > cc__exec_timeout_ms)
        longjmp(cc__exec_jb, 1);
}

static void cc__host_emit_raw(int anchor, const char* ptr, size_t len) {
    cc__exec_check_timeout();
    cc_emit_plan_host_emit_raw(anchor, ptr, len);
}

static void cc__host_emit_raw_at(int anchor, const char* file, int line,
                                 const char* ptr, size_t len) {
    cc__exec_check_timeout();
    cc_emit_plan_host_emit_raw_at(anchor, file, line, ptr, len);
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
/* Capture libtcc diagnostics for the comptime executor TU.  Warnings fire
 * before errors on a typical failure, so keep the first `error:` message
 * when one arrives; otherwise retain the first message as a fallback.
 * CC_DEBUG_COMPTIME_EXEC still mirrors every callback to stderr. */
typedef struct {
    char buf[512];
    int  got_error;
} CCExecErrSink;

static void cc__exec_err_capture(void* opaque, const char* msg) {
    CCExecErrSink* sink = (CCExecErrSink*)opaque;
    if (getenv("CC_DEBUG_COMPTIME_EXEC") && msg)
        fprintf(stderr, "[cc:comptime-exec] tcc: %s\n", msg);
    if (!sink || !msg || !msg[0]) return;
    {
        int is_err = strstr(msg, "error:") != NULL;
        if (sink->got_error && !is_err) return;
        if (sink->got_error && is_err) return; /* first error wins */
        if (is_err) sink->got_error = 1;
        else if (sink->buf[0]) return;         /* keep first warning */
        snprintf(sink->buf, sizeof(sink->buf), "%s", msg);
    }
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

/* Registry defs ride into every block/eval TU so comptime code can call
 * @comptime fns.  Factory bodies among them use the arg() sugar the
 * prelude defines only under CC_COMPTIME_EXEC — so a TU that carries a
 * factory along must switch it on, exactly as the compiled-factory TU
 * does.  Without this, a file that both includes a factory-bearing
 * header (py.cch) and runs its own @comptime block dies with
 * "implicit declaration of function 'arg'" inside header code the user
 * never wrote. */
static int cc__exec_fndefs_need_exec_define(const char* fndefs) {
    return fndefs && strstr(fndefs, "__cc_gfac_") != NULL;
}

static char* cc__exec_build_tu(const char* body, size_t body_len,
                               const char* file, int line) {
    static const char entry[] = "\nvoid __cc_ct_entry(void) {\n";
    static const char tail[] = "\n}\n";
    static const char exec_def[] = "#define CC_COMPTIME_EXEC 1\n";
    const char* fndefs = cc_comptime_fn_registry_defs();
    size_t fndef_len = fndefs ? strlen(fndefs) : 0;
    size_t ed = cc__exec_fndefs_need_exec_define(fndefs) ? sizeof(exec_def) - 1 : 0;
    size_t pre = sizeof(CC__EXEC_PRELUDE) - 1;
    size_t ent = sizeof(entry) - 1;
    size_t tl = sizeof(tail) - 1;
    /* `#line N "file"` before the body so libtcc names the user source, not
     * `<string>:N`.  Line is the 1-based line of the first body byte. */
    char line_dir[1100];
    size_t ld = 0;
    if (file && file[0] && line > 0) {
        int n = snprintf(line_dir, sizeof(line_dir), "#line %d \"%s\"\n", line, file);
        if (n > 0 && (size_t)n < sizeof(line_dir)) ld = (size_t)n;
    }
    char* s = (char*)malloc(ed + pre + fndef_len + ent + ld + body_len + tl + 1);
    if (!s) return NULL;
    size_t o = 0;
    if (ed) { memcpy(s + o, exec_def, ed); o += ed; }
    memcpy(s + o, CC__EXEC_PRELUDE, pre); o += pre;
    if (fndef_len) { memcpy(s + o, fndefs, fndef_len); o += fndef_len; }
    memcpy(s + o, entry, ent); o += ent;
    if (ld) { memcpy(s + o, line_dir, ld); o += ld; }
    memcpy(s + o, body, body_len); o += body_len;
    memcpy(s + o, tail, tl); o += tl;
    s[o] = '\0';
    return s;
}

static char* cc__exec_build_eval_tu(const char* expr) {
    static const char hdr[] = "\nlong long __cc_ce_result;\nvoid __cc_ct_entry(void) {\n"
                              "  __cc_ce_result = (long long)(";
    static const char tail[] = ");\n}\n";
    static const char exec_def[] = "#define CC_COMPTIME_EXEC 1\n";
    const char* fndefs = cc_comptime_fn_registry_defs();
    size_t fndef_len = fndefs ? strlen(fndefs) : 0;
    size_t ed = cc__exec_fndefs_need_exec_define(fndefs) ? sizeof(exec_def) - 1 : 0;
    size_t ex = expr ? strlen(expr) : 0;
    size_t pre = sizeof(CC__EXEC_PRELUDE) - 1;
    size_t hl = sizeof(hdr) - 1;
    size_t tl = sizeof(tail) - 1;
    char* s = (char*)malloc(ed + pre + fndef_len + hl + ex + tl + 1);
    if (!s) return NULL;
    size_t o = 0;
    if (ed) { memcpy(s + o, exec_def, ed); o += ed; }
    memcpy(s + o, CC__EXEC_PRELUDE, pre); o += pre;
    if (fndef_len) { memcpy(s + o, fndefs, fndef_len); o += fndef_len; }
    memcpy(s + o, hdr, hl); o += hl;
    memcpy(s + o, expr, ex); o += ex;
    memcpy(s + o, tail, tl); o += tl;
    s[o] = '\0';
    return s;
}

/* Value-position @comptime(expr) projector.  Uses the same stack-first growable
 * CCArena + CCString pattern as @emit (CC_COMPTIME prelude).  Readback exports
 * a char* into arena-owned storage (__cc_ce_text/__cc_ce_len); the host copies
 * out after the litproj TU runs. */
static const char* cc__litproj_helpers(void) {
    static char* cached = NULL;
    if (cached) return cached;
    {
        size_t cap = 8192;
        char* buf = (char*)malloc(cap);
        if (!buf) return "";
        int n = snprintf(buf, cap,
            "#include <math.h>\n"
            "static CCArena __cc_lit_arena;\n"
            "static uint8_t __cc_lit_stack[%u];\n"
            "static CCString __cc_lit_str;\n"
            "char *__cc_ce_text = 0;\n"
            "unsigned long __cc_ce_len = 0;\n"
            "enum { CC_LIT_OK = 0, CC_LIT_NOT_PROJECTABLE = 1, CC_LIT_OOM = 2 };\n"
            "int __cc_ce_status = CC_LIT_OK;\n"
            "static int __cc_lit_fail(int status) {\n"
            "  if (__cc_ce_status == CC_LIT_OK) __cc_ce_status = status;\n"
            "  return 0;\n"
            "}\n"
            "static int __cc_lit_push_char(char c) {\n"
            "  if (__cc_ce_status != CC_LIT_OK) return 0;\n"
            "  return cc_string_push_char(&__cc_lit_str, c, &__cc_lit_arena) ? 1 : __cc_lit_fail(CC_LIT_OOM);\n"
            "}\n"
            "static int __cc_lit_push_buf(const char* p, uint32_t n) {\n"
            "  if (__cc_ce_status != CC_LIT_OK) return 0;\n"
            "  return cc_string_push_buffer(&__cc_lit_str, p, n, &__cc_lit_arena) ? 1 : __cc_lit_fail(CC_LIT_OOM);\n"
            "}\n"
            "static int __cc_lit_push_cstr(const char* p) {\n"
            "  if (__cc_ce_status != CC_LIT_OK) return 0;\n"
            "  return cc_string_push_cstr(&__cc_lit_str, p, &__cc_lit_arena) ? 1 : __cc_lit_fail(CC_LIT_OOM);\n"
            "}\n"
            "static void __cc_lit_begin(void) {\n"
            "  cc_arena_buffer(&__cc_lit_arena, __cc_lit_stack, sizeof(__cc_lit_stack));\n"
            "  __cc_lit_arena.block_max = 0;\n"
            "  __cc_lit_str = cc_string_new();\n"
            "  __cc_ce_text = 0; __cc_ce_len = 0; __cc_ce_status = CC_LIT_OK;\n"
            "}\n"
            "static void __cc_lit_finish(void) {\n"
            "  if (__cc_ce_status != CC_LIT_OK) return;\n"
            "  __cc_ce_text = (char*)cc_string_cstr(&__cc_lit_str, &__cc_lit_arena);\n"
            "  if (!__cc_ce_text) { __cc_lit_fail(CC_LIT_OOM); return; }\n"
            "  __cc_ce_len = __cc_ce_text ? (unsigned long)strlen(__cc_ce_text) : 0;\n"
            "}\n"
            "void __cc_lit_cleanup(void) { cc_arena_free(&__cc_lit_arena); }\n"
            "static void __cc_lit_push_escaped(const char* s, long n) {\n"
            "  if (n < 0 || !s) { __cc_lit_fail(CC_LIT_NOT_PROJECTABLE); return; }\n"
            "  if (!__cc_lit_push_char(34)) return;\n"
            "  for (long i = 0; i < n; i++) {\n"
            "    unsigned char c = (unsigned char)s[i];\n"
            "    if (c == 92 || c == 34) {\n"
            "      if (!__cc_lit_push_char(92)) return;\n"
            "      if (!__cc_lit_push_char((char)c)) return;\n"
            "    } else if (c == 10) {\n"
            "      if (!__cc_lit_push_char(92)) return;\n"
            "      if (!__cc_lit_push_char(110)) return;\n"
            "    } else if (c == 9) {\n"
            "      if (!__cc_lit_push_char(92)) return;\n"
            "      if (!__cc_lit_push_char(116)) return;\n"
            "    } else if (c == 13) {\n"
            "      if (!__cc_lit_push_char(92)) return;\n"
            "      if (!__cc_lit_push_char(114)) return;\n"
            "    } else if (c >= 32 && c < 127) {\n"
            "      if (!__cc_lit_push_char((char)c)) return;\n"
            "    } else {\n"
            "      char tmp[8]; int rl = snprintf(tmp, sizeof(tmp), \"%%03o\", c);\n"
            "      if (!__cc_lit_push_char(92)) return;\n"
            "      for (int k = 0; k < rl; k++) if (!__cc_lit_push_char(tmp[k])) return;\n"
            "    }\n"
            "  }\n"
            "  (void)__cc_lit_push_char(34);\n"
            "}\n"
            "static void cc__lit_i(long long v){ if (__cc_ce_status == CC_LIT_OK && !cc_string_push_int(&__cc_lit_str, v, &__cc_lit_arena)) __cc_lit_fail(CC_LIT_OOM); }\n"
            "static void cc__lit_u(unsigned long long v){ if (__cc_ce_status == CC_LIT_OK && !cc_string_push_uint(&__cc_lit_str, v, &__cc_lit_arena)) __cc_lit_fail(CC_LIT_OOM); }\n"
            "static void cc__lit_d(double v){\n"
            "  if (!isfinite(v)) { __cc_lit_fail(CC_LIT_NOT_PROJECTABLE); return; }\n"
            "  char tmp[64]; int n = snprintf(tmp, sizeof(tmp), \"%%.17g\", v);\n"
            "  int dot = 0; for (int i = 0; i < n; i++) { char c = tmp[i]; if (c=='.'||c=='e'||c=='E') { dot = 1; break; } }\n"
            "  if (!dot && n + 2 < (int)sizeof(tmp)) { tmp[n++]='.'; tmp[n++]='0'; tmp[n]=0; }\n"
            "  (void)__cc_lit_push_buf(tmp, (uint32_t)n);\n"
            "}\n"
            "static void cc__lit_b(int v){ (void)__cc_lit_push_cstr(v ? \"1\" : \"0\"); }\n"
            "static void cc__lit_s(const char* s){ if (!s){ (void)__cc_lit_push_cstr(\"0\"); return; } __cc_lit_push_escaped(s,(long)strlen(s)); }\n"
            "static void cc__lit_sl(CCSlice s){ __cc_lit_push_escaped((const char*)s.ptr,(long)s.len); }\n"
            "static void cc__lit_bad(){ __cc_lit_fail(CC_LIT_NOT_PROJECTABLE); }\n"
            "#define cc_lit_project(d) _Generic((d), _Bool: cc__lit_b, char: cc__lit_i, signed char: cc__lit_i, short: cc__lit_i, int: cc__lit_i, long: cc__lit_i, long long: cc__lit_i, unsigned char: cc__lit_u, unsigned short: cc__lit_u, unsigned int: cc__lit_u, unsigned long: cc__lit_u, unsigned long long: cc__lit_u, float: cc__lit_d, double: cc__lit_d, char*: cc__lit_s, const char*: cc__lit_s, CCSlice: cc__lit_sl, default: cc__lit_bad)(d)\n",
            (unsigned)CC_EMIT_TPL_BUF_SIZE);
        if (n < 0 || (size_t)n >= cap) { free(buf); return ""; }
        cached = buf;
    }
    return cached;
}

static char* cc__exec_build_litproj_tu(const char* expr) {
    static const char hdr[] =
        "\nvoid __cc_ct_entry(void) {\n  __cc_lit_begin();\n  cc_lit_project((";
    static const char tail[] = "));\n  __cc_lit_finish();\n}\n";
    const char* fndefs = cc_comptime_fn_registry_defs();
    const char* helpers = cc__litproj_helpers();
    size_t fndef_len = fndefs ? strlen(fndefs) : 0;
    size_t ex = expr ? strlen(expr) : 0;
    size_t pre = sizeof(CC__EXEC_PRELUDE) - 1;
    size_t hp = helpers ? strlen(helpers) : 0;
    size_t hl = sizeof(hdr) - 1;
    size_t tl = sizeof(tail) - 1;
    char* s = (char*)malloc(pre + fndef_len + hp + hl + ex + tl + 1);
    if (!s) return NULL;
    size_t o = 0;
    memcpy(s + o, CC__EXEC_PRELUDE, pre); o += pre;
    if (fndef_len) { memcpy(s + o, fndefs, fndef_len); o += fndef_len; }
    if (hp) { memcpy(s + o, helpers, hp); o += hp; }
    memcpy(s + o, hdr, hl); o += hl;
    memcpy(s + o, expr, ex); o += ex;
    memcpy(s + o, tail, tl); o += tl;
    s[o] = '\0';
    return s;
}

static TCCState* cc__exec_new_state(CCExecErrSink* sink, char* err_buf, size_t err_sz);

/* Compile + relocate + run the litproj TU, then read back the projected text
 * (__cc_ce_text/__cc_ce_len) and the projectability flag (__cc_ce_ok).
 * Copies the literal into `arena`.  Returns 0 with *out_lit set, -2 if the
 * value ran but is not projectable, -1 on failure. */
static int cc__exec_run_litproj_tu(const char* tu,
                                   char** out_lit, size_t* out_len,
                                   char* err_buf, size_t err_sz,
                                   CCArena* arena) {
    CCExecErrSink sink;
    if (out_lit) *out_lit = NULL;
    if (out_len) *out_len = 0;
    if (!arena) {
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "comptime value eval requires an arena");
        return -1;
    }
    memset(&sink, 0, sizeof(sink));
    TCCState* s = cc__exec_new_state(&sink, err_buf, err_sz);
    if (!s) return -1;
    if (tcc_compile_string(s, tu) < 0) {
        if (err_buf && err_sz) {
            if (sink.buf[0]) snprintf(err_buf, err_sz, "%s", sink.buf);
            else snprintf(err_buf, err_sz, "comptime value TU compile failed");
        }
        tcc_delete(s);
        return -1;
    }
    if (tcc_relocate(s) < 0) {
        if (err_buf && err_sz) {
            if (sink.buf[0]) snprintf(err_buf, err_sz, "%s", sink.buf);
            else snprintf(err_buf, err_sz, "comptime value TU relocate failed");
        }
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
    void* cleanup_sym = tcc_get_symbol(s, "__cc_lit_cleanup");
    if (!cleanup_sym) cleanup_sym = tcc_get_symbol(s, "___cc_lit_cleanup");
    void* sym_text = tcc_get_symbol(s, "__cc_ce_text");
    if (!sym_text) sym_text = tcc_get_symbol(s, "___cc_ce_text");
    void* plen = tcc_get_symbol(s, "__cc_ce_len");
    if (!plen) plen = tcc_get_symbol(s, "___cc_ce_len");
    void* pstatus = tcc_get_symbol(s, "__cc_ce_status");
    if (!pstatus) pstatus = tcc_get_symbol(s, "___cc_ce_status");
    if (!cleanup_sym || !sym_text || !plen || !pstatus) {
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "comptime value symbols not found");
        tcc_delete(s);
        return -1;
    }
    typedef void (*CCLitCleanupFn)(void);
    CCLitCleanupFn cleanup = (CCLitCleanupFn)cleanup_sym;
    int status = 0;
    memcpy(&status, pstatus, sizeof(int));
    unsigned long ln = 0;
    memcpy(&ln, plen, sizeof(unsigned long));
    char* text = NULL;
    memcpy(&text, sym_text, sizeof(char*));
    if (status == 1) {
        cleanup();
        tcc_delete(s);
        return -2;
    }
    if (status != 0) {
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "OOM projecting comptime literal");
        cleanup();
        tcc_delete(s);
        return -1;
    }
    if (!text || ln == 0) {
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "projected literal is empty");
        cleanup();
        tcc_delete(s);
        return -1;
    }
    char* dest = (char*)cc_arena_alloc(arena, (size_t)ln + 1, 1);
    if (!dest) {
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "OOM allocating projected literal in arena");
        cleanup();
        tcc_delete(s);
        return -1;
    }
    memcpy(dest, text, (size_t)ln);
    dest[ln] = '\0';
    cleanup();
    tcc_delete(s);
    if (out_lit) *out_lit = dest;
    if (out_len) *out_len = (size_t)ln;
    return 0;
}

static int cc__exec_run_tu_ex(const char* tu, char* err_buf, size_t err_sz, int64_t* out_int);

static int cc__exec_run_tu(const char* tu, char* err_buf, size_t err_sz) {
    return cc__exec_run_tu_ex(tu, err_buf, err_sz, NULL);
}

/* Create a TCC_OUTPUT_MEMORY state configured exactly like the comptime block
 * executor: libtcc lib path, the compiler's CC_INCLUDE_PATH, and the full host
 * verb symbol table.  Shared by @comptime block execution and the in-process
 * compiled-factory path so both run in an identical environment. */
static TCCState* cc__exec_new_state(CCExecErrSink* sink, char* err_buf, size_t err_sz) {
    TCCState* s = tcc_new();
    if (!s) {
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "tcc_new failed");
        return NULL;
    }
    tcc_set_error_func(s, sink, cc__exec_err_capture);
    /* Parse at the version the real compile will use; see the constant. */
    tcc_set_options(s, CC_HOST_C_STD_OPTION);
    {
        char dirbuf[1024];
        const char* libdir = cc__exec_lib_dir(dirbuf, sizeof(dirbuf));
        if (libdir) {
            tcc_set_lib_path(s, libdir);
            tcc_add_library_path(s, libdir);
        }
    }
    /* Curated Axis-1: let the comptime TU `#include <ccc/...>` the inline stdlib
     * (slices/arena/string) so `@comptime` code can use CC library functions,
     * not just libc + host verbs (COMPTIME_CAPABILITY_MODEL.md §7a, Axis 1).
     * CC_INCLUDE_PATH is the compiler's own header search path (lowered .h dir
     * then raw .cch dir), colon-separated; mirror it into the executor. */
    {
        const char* inc = getenv("CC_INCLUDE_PATH");
        if (inc && inc[0]) {
            char tmp[2048];
            snprintf(tmp, sizeof(tmp), "%s", inc);
            char* save = NULL;
            for (char* tok = strtok_r(tmp, ":", &save); tok; tok = strtok_r(NULL, ":", &save))
                if (tok[0]) tcc_add_include_path(s, tok);
        }
    }
    if (tcc_set_output_type(s, TCC_OUTPUT_MEMORY) < 0) {
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "tcc_set_output_type failed");
        tcc_delete(s);
        return NULL;
    }
    tcc_add_symbol(s, "cc_emit_raw", (void*)cc__host_emit_raw);
    tcc_add_symbol(s, "cc_instantiate_vec", (void*)cc__host_instantiate_vec);
    tcc_add_symbol(s, "cc_instantiate_map", (void*)cc__host_instantiate_map);
    tcc_add_symbol(s, "cc_instantiate_chan", (void*)cc__host_instantiate_chan);
    tcc_add_symbol(s, "cc_reflect_field_count", (void*)cc_reflect_field_count);
    tcc_add_symbol(s, "cc_reflect_field_name", (void*)cc_reflect_field_name);
    tcc_add_symbol(s, "cc_reflect_field_type", (void*)cc_reflect_field_type);
    tcc_add_symbol(s, "cc_result_box_name", (void*)cc_result_box_name);
    tcc_add_symbol(s, "cc_reflect_method_count", (void*)cc_reflect_method_count);
    tcc_add_symbol(s, "cc_reflect_method_name", (void*)cc_reflect_method_name);
    tcc_add_symbol(s, "cc_reflect_param_count", (void*)cc_reflect_param_count);
    tcc_add_symbol(s, "cc_reflect_param_name", (void*)cc_reflect_param_name);
    tcc_add_symbol(s, "cc_reflect_param_type", (void*)cc_reflect_param_type);
    tcc_add_symbol(s, "cc_reflect_param_default", (void*)cc_reflect_param_default);
    tcc_add_symbol(s, "cc_reflect_params_c_abi", (void*)cc_reflect_params_c_abi);
    tcc_add_symbol(s, "cc_reflect_method_member", (void*)cc_reflect_method_member);
    tcc_add_symbol(s, "cc_reflect_method_params", (void*)cc_reflect_method_params);
    tcc_add_symbol(s, "cc_reflect_method_args", (void*)cc_reflect_method_args);
    tcc_add_symbol(s, "cc_reflect_method_ret", (void*)cc_reflect_method_ret);
    tcc_add_symbol(s, "cc_reflect_method_err", (void*)cc_reflect_method_err);
    tcc_add_symbol(s, "cc_reflect_enum_count", (void*)cc_reflect_enum_count);
    tcc_add_symbol(s, "cc_reflect_enum_name", (void*)cc_reflect_enum_name);
    tcc_add_symbol(s, "cc_reflect_enum_value", (void*)cc_reflect_enum_value);
    tcc_add_symbol(s, "cc_reflect_kind", (void*)cc_reflect_kind);
    tcc_add_symbol(s, "cc_canonical_name", (void*)cc_canonical_name);
    tcc_add_symbol(s, "cc_reflect_tagged_count", (void*)cc_reflect_tagged_count);
    tcc_add_symbol(s, "cc_reflect_tagged_name", (void*)cc_reflect_tagged_name);
    tcc_add_symbol(s, "cc_emit_raw_at", (void*)cc__host_emit_raw_at);
    tcc_add_symbol(s, "cc_emit_error", (void*)cc_emit_error);
    tcc_add_symbol(s, "cc_emit_warning", (void*)cc_emit_warning);
    tcc_add_symbol(s, "cc_emit_error_at", (void*)cc_emit_error_at);
    tcc_add_symbol(s, "cc_emit_warning_at", (void*)cc_emit_warning_at);
    return s;
}

static int cc__exec_run_tu_ex(const char* tu, char* err_buf, size_t err_sz, int64_t* out_int) {
    CCExecErrSink sink;
    memset(&sink, 0, sizeof(sink));
    TCCState* s = cc__exec_new_state(&sink, err_buf, err_sz);
    if (!s) return -1;

    if (tcc_compile_string(s, tu) < 0) {
        if (err_buf && err_sz) {
            if (sink.buf[0]) snprintf(err_buf, err_sz, "%s", sink.buf);
            else snprintf(err_buf, err_sz, "comptime TU compile failed");
        }
        tcc_delete(s);
        return -1;
    }
    if (tcc_relocate(s) < 0) {
        if (err_buf && err_sz) {
            if (sink.buf[0]) snprintf(err_buf, err_sz, "%s", sink.buf);
            else snprintf(err_buf, err_sz, "comptime TU relocate failed");
        }
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

/* In-process compiled-factory path: compile a full TU (prelude + factory def +
 * wrapper) with libtcc, relocate, and return the live state as an opaque owner.
 * Callers resolve entry points with cc_comptime_exec_lookup_symbol and free the
 * state with cc_comptime_exec_release once no resolved pointer is needed.  This
 * replaces the host-cc spawn + dylib + dlopen for generic factories: first-use
 * compile is in-process (ms) and shares the executor's exact environment. */
int cc_comptime_exec_compile_tu(const char* tu_src, void** out_state,
                                char* err_buf, size_t err_sz) {
    CCExecErrSink sink;
    if (err_buf && err_sz) err_buf[0] = '\0';
    if (!tu_src || !out_state) {
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "invalid compile_tu args");
        return -1;
    }
    *out_state = NULL;
    memset(&sink, 0, sizeof(sink));
    TCCState* s = cc__exec_new_state(&sink, err_buf, err_sz);
    if (!s) return -1;
    if (tcc_compile_string(s, tu_src) < 0) {
        if (err_buf && err_sz) {
            if (sink.buf[0]) snprintf(err_buf, err_sz, "%s", sink.buf);
            else snprintf(err_buf, err_sz, "factory TU compile failed");
        }
        tcc_delete(s);
        return -1;
    }
    if (tcc_relocate(s) < 0) {
        if (err_buf && err_sz) {
            if (sink.buf[0]) snprintf(err_buf, err_sz, "%s", sink.buf);
            else snprintf(err_buf, err_sz, "factory TU relocate failed");
        }
        tcc_delete(s);
        return -1;
    }
    *out_state = s;
    return 0;
}

const void* cc_comptime_exec_lookup_symbol(void* state, const char* name) {
    if (!state || !name) return NULL;
    void* sym = tcc_get_symbol((TCCState*)state, name);
    if (!sym) {
        /* Mach-O underscore-prefixes C symbols; tcc_get_symbol may need it. */
        char alt[160];
        if (snprintf(alt, sizeof(alt), "_%s", name) < (int)sizeof(alt))
            sym = tcc_get_symbol((TCCState*)state, alt);
    }
    return sym;
}

void cc_comptime_exec_release(void* state) {
    if (state) tcc_delete((TCCState*)state);
}
#else  /* !CC_TCC_EXT_AVAILABLE */

int cc_comptime_exec_compile_tu(const char* tu_src, void** out_state,
                                char* err_buf, size_t err_sz) {
    (void)tu_src;
    if (out_state) *out_state = NULL;
    if (err_buf && err_sz) snprintf(err_buf, err_sz, "libtcc not available");
    return -1;
}

const void* cc_comptime_exec_lookup_symbol(void* state, const char* name) {
    (void)state; (void)name;
    return NULL;
}

void cc_comptime_exec_release(void* state) {
    (void)state;
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
        cc__exec_in_block = 0;
        if (err_buf && err_sz)
            snprintf(err_buf, err_sz, "comptime eval timed out (%dms)", cc__exec_timeout_ms);
        rc = -1;
    } else {
        cc__exec_in_block = 1;
        rc = cc__exec_run_tu_ex(tu, err_buf, err_sz, out);
        cc__exec_in_block = 0;
    }
    free(tu);
    (void)opts;
    return rc;
#endif
}

int cc_comptime_exec_eval_literal(const char* expr,
                                  const CCComptimeExecOpts* opts,
                                  char** out_lit, size_t* out_len,
                                  char* err_buf, size_t err_sz,
                                  CCArena* arena) {
    if (out_len) *out_len = 0;
    if (out_lit) *out_lit = NULL;
    if (!expr || !out_lit || !arena) {
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "invalid eval_literal args");
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
    char* tu = cc__exec_build_litproj_tu(expr);
    if (!tu) {
        if (err_buf && err_sz) snprintf(err_buf, err_sz, "OOM building value TU");
        return -1;
    }
    int rc = 0;
    if (setjmp(cc__exec_jb) != 0) {
        cc__exec_in_block = 0;
        if (err_buf && err_sz)
            snprintf(err_buf, err_sz, "comptime value eval timed out (%dms)", cc__exec_timeout_ms);
        rc = -1;
    } else {
        cc__exec_in_block = 1;
        rc = cc__exec_run_litproj_tu(tu, out_lit, out_len, err_buf, err_sz, arena);
        cc__exec_in_block = 0;
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

    char* tu = cc__exec_build_tu(body, body_len,
                                 opts ? opts->input_path : NULL,
                                 opts ? opts->site_line : 0);
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
        cc__exec_in_block = 0;
        if (err_buf && err_sz)
            snprintf(err_buf, err_sz, "comptime execution timed out (%dms)",
                     cc__exec_timeout_ms);
        rc = -1;
    } else {
        cc__exec_in_block = 1;
        rc = cc__exec_run_tu(tu, err_buf, err_sz);
        cc__exec_in_block = 0;
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
 * merged TU can resolve.  High-precision allowlist: never blocks on context —
 * which requires the validation TU to CARRY that context, since an unknown type
 * name reports as "expected" too (`T *self` with `T` undeclared parses as an
 * identifier followed by `*`).  See cc__frag_context_prelude. */
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

/* Declare every `CCResult_*` name the fragment mentions.
 *
 * The boxes a fragment references are minted by the Result machinery, not
 * written in the source, so the file's type prelude does not carry them — and
 * a fragment that forward-declares a fallible method must name one.  The names
 * are in the text, so they are read from the text; ok and error spellings are
 * not recoverable from a mangled name, but this is a SYNTAX check, and the
 * documented shape is all that member access needs to parse. */
static void cc__frag_append_box_decls(char** out, size_t* len, size_t* cap,
                                      const char* frag) {
    static const char PFX[] = "CCResult_";
    const size_t PFXN = sizeof(PFX) - 1;
    size_t i = 0, n = frag ? strlen(frag) : 0;
    char seen[64][256];
    size_t nseen = 0;
    while (i + PFXN <= n) {
        size_t e, k;
        int dup = 0;
        char nm[256];
        if (memcmp(frag + i, PFX, PFXN) != 0 ||
            (i > 0 && (cc_is_ident_char(frag[i - 1])))) { i++; continue; }
        e = i;
        while (e < n && cc_is_ident_char(frag[e])) e++;
        if (e - i >= sizeof(nm)) { i = e; continue; }
        memcpy(nm, frag + i, e - i);
        nm[e - i] = '\0';
        for (k = 0; k < nseen; k++) if (strcmp(seen[k], nm) == 0) { dup = 1; break; }
        if (!dup && nseen < sizeof(seen) / sizeof(seen[0])) {
            char line[512];
            snprintf(line, sizeof(line),
                     "typedef struct { int ok; union { long long value; "
                     "long long error; } u; } %s;\n", nm);
            cc_sb_append_cstr(out, len, cap, line);
            snprintf(seen[nseen], sizeof(seen[0]), "%s", nm);
            nseen++;
        }
        i = e;
    }
}

int cc_comptime_validate_c_fragment(const char* fragment,
                                    int* out_frag_line,
                                    char* err_buf, size_t err_sz) {
    if (out_frag_line) *out_frag_line = 0;
    if (err_buf && err_sz) err_buf[0] = '\0';
    if (!fragment || !fragment[0]) return 0;
    if (getenv("CC_NO_FRAGMENT_VALIDATE")) return 0;

    /* The file's own types go in ahead of the fragment.  A generated
     * definition that names a user type is the normal case — a factory exists
     * to write code about a type — and without them `T *self` parses as an
     * unknown identifier followed by `*`, whose message ("',' expected") looks
     * exactly like a syntax defect to the filter below.  Validating against
     * real declarations is also what lets the check mean anything. */
    char* types = cc_emit_plan_reflect_type_prelude();
    {   /* Boxes named by the fragment, appended to the file's own types. */
        size_t tl = types ? strlen(types) : 0, tc = tl + 1;
        cc__frag_append_box_decls(&types, &tl, &tc, fragment);
    }
    size_t tylen = types ? strlen(types) : 0;
    size_t pre = sizeof(CC__FRAG_PRELUDE) - 1;
    size_t fl = strlen(fragment);
    char* tu = (char*)malloc(pre + tylen + fl + 2);
    if (!tu) { free(types); return 0; }  /* OOM: don't block on a missing check */
    {
        /* `#line 1 "<generic-fragment>"` closes the prelude, so the type block
         * goes BEFORE it and fragment line numbers stay 1-based. */
        static const char MARK[] = "#line 1 \"<generic-fragment>\"\n";
        size_t marklen = sizeof(MARK) - 1;
        size_t head = pre - marklen;
        memcpy(tu, CC__FRAG_PRELUDE, head);
        if (tylen) memcpy(tu + head, types, tylen);
        memcpy(tu + head + tylen, MARK, marklen);
        memcpy(tu + head + tylen + marklen, fragment, fl);
        tu[head + tylen + marklen + fl] = '\n';
        tu[head + tylen + marklen + fl + 1] = '\0';
    }
    free(types);

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
    /* Only the fragment is on trial.  The context prepended above is
     * best-effort — a type definition it carries may itself name something the
     * validation TU has never seen — and an error inside it says nothing about
     * the generated code.  Errors there report against `<string>`; the
     * fragment's own report against `<generic-fragment>`, which the `#line`
     * directive establishes. */
    if (!strstr(sink.buf, "<generic-fragment>")) return 0;
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
