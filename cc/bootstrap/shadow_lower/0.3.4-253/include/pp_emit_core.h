/* Shadow emit core: CEmit, bind/chan/generic tables, #line / lead trivia.
 * Included by pp_emit.cch before typehooks + ufcs + stmt. Requires pp_ast.cch. */
#pragma once

#include <ccc/cc_ufcs_families.h>
#include <dirent.h>
#include "util/text.h"
#include "comptime/symbols.h"
#include "comptime/hook_compile.h"

#include "pp_ast_core.h"
#include "pp_tape.h"
char* cc_ct_extract_type_decls_prelude(const char* src, size_t n);

static int g_shadow_prelude_off;
static int g_shadow_no_line;

/* Compiled-factory produce (libshadow_comptime / emit_plan). */
#ifndef CC_GEN_PRODUCE_OK
typedef enum CCGenProduceStatus {
    CC_GEN_PRODUCE_OK = 0,
    CC_GEN_PRODUCE_ENSURE_FAILED = 1,
    CC_GEN_PRODUCE_INVOKE_FAILED = 2
} CCGenProduceStatus;
#endif
int cc_emit_plan_generic_factory_names_csv(char* out, size_t cap);
int cc_emit_plan_has_generic_factory(const char* name);
void cc_emit_plan_snake_name(const char* name, char* out, size_t cap);
CCGenProduceStatus cc_emit_plan_produce_generic_def(
    const char* gname, const char* mangled, const char orig_args[8][128],
    int nargs, const char* reflect_src, size_t reflect_len,
    const char* input_path, CCArena out_ar, char** out_def, char* err,
    size_t err_cap);
int cc_emit_plan_generic_invalid_report_once(const char* mangled);
int cc_emit_plan_take_exec_error(void);
const char* cc_emit_plan_lookup_generic_factory_handler(const char* name);
int cc_comptime_fn_registry_lookup_line(const char* name);
const char* cc_comptime_fn_registry_lookup_file(const char* name);
int cc_comptime_validate_c_fragment(const char* fragment, int* out_line,
                                    char* err, size_t err_cap);

/* Diag helper: `note: in @comptime factory 'handler' at file:line`. */
static void shadow_gfac_note_handler(const char* family, const char* fallback_path) {
    const char* handler =
        family ? cc_emit_plan_lookup_generic_factory_handler(family) : NULL;
    int hline = handler ? cc_comptime_fn_registry_lookup_line(handler) : 0;
    const char* hfile =
        handler ? cc_comptime_fn_registry_lookup_file(handler) : NULL;
    const char* show;
    const char* tests;
    char ofile[256];
    if (!handler || hline <= 0) return;
    show = hfile && hfile[0] ? hfile : (fallback_path ? fallback_path : "");
    tests = strstr(show, "tests/");
    if (tests)
        show = tests;
    else {
        const char* slash = strrchr(show, '/');
        if (slash && slash[1]) show = slash + 1;
    }
    snprintf(ofile, sizeof(ofile), "%s", show);
    fprintf(stderr, "note: in @comptime factory '%s' at %s:%d\n", handler,
            ofile[0] ? ofile : "<factory>", hline);
}

/* ---- shadow emit: AST → C source (product for TCC / host cc) ------------ */

typedef struct {
    char* buf;
    size_t len;
    size_t cap;
    int err;
} CEmit;

static int cemit_reserve(CEmit* e, size_t need) {
    if (e->len + need + 1 <= e->cap) return 1;
    size_t ncap = e->cap ? e->cap * 2 : 4096;
    while (ncap < e->len + need + 1) ncap *= 2;
    char* nbuf = (char*)realloc(e->buf, ncap);
    if (!nbuf) { e->err = 1; return 0; }
    e->buf = nbuf;
    e->cap = ncap;
    return 1;
}

static int cemit_str(CEmit* e, const char* s) {
    size_t n = strlen(s);
    if (!cemit_reserve(e, n)) return 0;
    memcpy(e->buf + e->len, s, n);
    e->len += n;
    e->buf[e->len] = 0;
    return 1;
}

static int cemit_buf(CEmit* e, const char* s, size_t n) {
    if (!s || !n) return 1;
    if (!cemit_reserve(e, n)) return 0;
    memcpy(e->buf + e->len, s, n);
    e->len += n;
    e->buf[e->len] = 0;
    return 1;
}

/* Defined in pp_emit_tpl.cch: printf-shaped shim → ${n} template emit. */
static int cemit_fmt(CEmit* e, const char* fmt, ...);

typedef enum { SHADOW_EMIT_C = 0, SHADOW_EMIT_H = 1 } ShadowEmitKind;

/* Mirror of cc_result_spec_mangle_type (result_spec.c) — keep in sync for goldens. */
static void shadow_mangle_type(const char* src, char* out, size_t out_sz) {
    size_t len = src ? strlen(src) : 0;
    size_t i, j = 0;
    if (!src || len == 0 || !out || out_sz == 0) {
        if (out && out_sz) out[0] = 0;
        return;
    }
    while (len && (*src == ' ' || *src == '\t')) { src++; len--; }
    while (len && (src[len - 1] == ' ' || src[len - 1] == '\t')) len--;
    for (i = 0; i < len && j < out_sz - 1; i++) {
        char c = src[i];
        if (c == ' ' || c == '\t') {
            if (j > 0 && out[j - 1] != '_') out[j++] = '_';
        } else if (c == '*') {
            if (j + 3 < out_sz - 1) { out[j++] = 'p'; out[j++] = 't'; out[j++] = 'r'; }
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_') {
            out[j++] = c;
        } else {
            if (j > 0 && out[j - 1] != '_') out[j++] = '_';
        }
    }
    while (j > 0 && out[j - 1] == '_') j--;
    out[j] = 0;
    if (strcmp(out, "_Bool") == 0) snprintf(out, out_sz, "bool");
}

static void shadow_result_name(const char* ok, const char* err, char* out, size_t out_sz) {
    char mok[128], merr[128];
    /* Mangle pointer ok-types as Tptr (e.g. CCDirIter* → CCDirIterptr), matching
     * CC_DECL_RESULT_SPEC naming in std headers — not a blanket void*. */
    shadow_mangle_type(ok, mok, sizeof(mok));
    shadow_mangle_type(err, merr, sizeof(merr));
    snprintf(out, out_sz, "CCResult_%s_%s", mok, merr);
}

/* Defined after @as / rfn tables. */
static int shadow_cc_err_as_project(const char* err_face, const char* arg,
                                    char* out, size_t cap);
static int shadow_rname_err_face(const char* rname, char* out, size_t cap);

/* In result-fn bodies, rewrite cc_ok/cc_err to typed cc_ok_RNAME/cc_err_RNAME.
 * Single-arg cc_err(e) projects e through a unique @as path when the Result
 * error face differs from typeof(e) (same rule as @errhandler binder). */
static void shadow_rewrite_result_ctors(char* expr, size_t cap, const char* rname) {
    char tmp[4096];
    char err_face[64];
    const char* p;
    char* o;
    size_t rem;
    if (!expr || !cap || !rname || !rname[0]) return;
    err_face[0] = 0;
    (void)shadow_rname_err_face(rname, err_face, sizeof(err_face));
    p = expr;
    o = tmp;
    rem = sizeof(tmp) - 1;
    tmp[0] = 0;
    while (*p && rem > 0) {
        if (strncmp(p, "cc_ok(", 6) == 0) {
            int n = snprintf(o, rem, "cc_ok_%s(", rname);
            if (n < 0 || (size_t)n >= rem) break;
            o += n;
            rem -= (size_t)n;
            p += 6;
            continue;
        }
        if (strncmp(p, "cc_err(", 7) == 0) {
            const char* args = p + 7; /* first char inside parens */
            const char* q = args;
            int dep = 1;
            int has_comma = 0;
            while (*q && dep > 0) {
                if (*q == '(') dep++;
                else if (*q == ')') dep--;
                if (dep == 1 && *q == ',') has_comma = 1;
                q++;
            }
            if (has_comma) {
                int n = snprintf(o, rem, "cc_err_%s(CC_ERROR(", rname);
                if (n < 0 || (size_t)n >= rem) break;
                o += n;
                rem -= (size_t)n;
                p = args;
                while (p < q - 1 && rem > 0) {
                    *o++ = *p++;
                    rem--;
                }
                if (rem < 3) break;
                *o++ = ')';
                *o++ = ')';
                rem -= 2;
            } else {
                char argbuf[512];
                char proj[640];
                size_t alen = (size_t)((q - 1) - args);
                int n;
                if (alen >= sizeof(argbuf)) break;
                memcpy(argbuf, args, alen);
                argbuf[alen] = 0;
                {
                    int pr = 0;
                    if (err_face[0])
                        pr = shadow_cc_err_as_project(err_face, argbuf, proj,
                                                     sizeof(proj));
                    if (pr < 0) break; /* ambiguous — leave incomplete; host */
                    if (pr > 0)
                        n = snprintf(o, rem, "cc_err_%s(%s)", rname, proj);
                    else
                        n = snprintf(o, rem, "cc_err_%s(%s)", rname, argbuf);
                }
                if (n < 0 || (size_t)n >= rem) break;
                o += n;
                rem -= (size_t)n;
            }
            p = q;
            continue;
        }
        *o++ = *p++;
        rem--;
    }
    *o = 0;
    snprintf(expr, cap, "%s", tmp);
}

/* Result temp `__r`: emit `!(__r).ok` / `(__r).u.value` / `(__r).u.error`.
 * Host `_Generic` `__cc_uw_*` is only for AST_PTR_UNWRAP (pointer/null default). */

/* True if s[i] starts an identifier char. */
static int shadow_is_id0(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int shadow_is_id(char c) {
    return shadow_is_id0(c) || (c >= '0' && c <= '9');
}

/* Walk back from end (exclusive) over a primary call/ident for ?> / !> lhs. */
static int shadow_expr_lhs_start(const char* s, int end) {
    int i = end - 1;
    while (i >= 0 && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) i--;
    if (i < 0) return -1;
    if (s[i] == ')') {
        int d = 0;
        while (i >= 0) {
            if (s[i] == ')') d++;
            else if (s[i] == '(') {
                d--;
                if (d == 0) { i--; break; }
            }
            i--;
        }
        while (i >= 0 && (s[i] == ' ' || s[i] == '\t')) i--;
        if (i < 0 || !shadow_is_id(s[i])) return -1;
        while (i >= 0 && shadow_is_id(s[i])) i--;
        return i + 1;
    }
    if (!shadow_is_id(s[i])) return -1;
    while (i >= 0 && shadow_is_id(s[i])) i--;
    return i + 1;
}

/* Walk back from accessor pos over a projection receiver primary:
 * ident, parenthesized expr, call, or `*p` — stop at ops outside groups. */
static int shadow_proj_recv_start(const char* s, int end) {
    int i = end - 1;
    while (i >= 0 && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) i--;
    if (i < 0) return -1;
    if (s[i] == ')' || s[i] == ']') {
        char close = s[i];
        char open = (close == ')') ? '(' : '[';
        int d = 0;
        while (i >= 0) {
            if (s[i] == close) d++;
            else if (s[i] == open) {
                d--;
                if (d == 0) {
                    i--;
                    break;
                }
            }
            i--;
        }
        if (d != 0) return -1;
        while (i >= 0 && (s[i] == ' ' || s[i] == '\t')) i--;
        /* call/callee: ident(...) — include the name; else '(' of `(recv)`. */
        if (i >= 0 && shadow_is_id(s[i])) {
            while (i >= 0 && shadow_is_id(s[i])) i--;
        }
        return i + 1;
    }
    if (!shadow_is_id(s[i])) return -1;
    while (i >= 0 && shadow_is_id(s[i])) i--;
    /* optional unary `*` prefixes (`*p`, `**p`) */
    {
        int j = i;
        while (j >= 0 && (s[j] == ' ' || s[j] == '\t')) j--;
        while (j >= 0 && s[j] == '*') {
            i = j - 1;
            j = i;
            while (j >= 0 && (s[j] == ' ' || s[j] == '\t')) j--;
        }
    }
    return i + 1;
}

/* Walk back from `?>` over `recv.arm` / `recv->arm`. Returns primary start
 * or -1; fills arm / is_arrow / acc_pos (index of '.' or '-' of '->'). */
static int shadow_variant_proj_lhs_start(const char* s, int end, char* arm,
                                        size_t arm_cap, int* is_arrow,
                                        int* acc_pos) {
    int i, arm_s, arm_e, acc;
    if (!s || end <= 0 || !arm || !arm_cap || !is_arrow || !acc_pos) return -1;
    i = end - 1;
    while (i >= 0 && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) i--;
    if (i < 0 || !shadow_is_id(s[i])) return -1;
    arm_e = i + 1;
    while (i >= 0 && shadow_is_id(s[i])) i--;
    arm_s = i + 1;
    if (arm_e <= arm_s || (size_t)(arm_e - arm_s) >= arm_cap) return -1;
    memcpy(arm, s + arm_s, (size_t)(arm_e - arm_s));
    arm[arm_e - arm_s] = 0;
    while (i >= 0 && (s[i] == ' ' || s[i] == '\t')) i--;
    if (i < 0) return -1;
    if (s[i] == '.') {
        *is_arrow = 0;
        acc = i;
    } else if (i > 0 && s[i] == '>' && s[i - 1] == '-') {
        *is_arrow = 1;
        acc = i - 1;
    } else {
        return -1;
    }
    *acc_pos = acc;
    return shadow_proj_recv_start(s, acc);
}

/* First `lead>` (`!>` / `?>`) outside strings/comments/ticks, or NULL. */
static const char* shadow_find_sigil_live(const char* s, char lead) {
    int in_line = 0, in_block = 0, in_dq = 0, in_sq = 0, in_bt = 0;
    if (!s) return NULL;
    for (; *s; s++) {
        char c = *s;
        if (in_line) {
            if (c == '\n') in_line = 0;
            continue;
        }
        if (in_block) {
            if (c == '*' && s[1] == '/') {
                in_block = 0;
                s++;
            }
            continue;
        }
        if (in_dq) {
            if (c == '\\' && s[1]) {
                s++;
                continue;
            }
            if (c == '"') in_dq = 0;
            continue;
        }
        if (in_sq) {
            if (c == '\\' && s[1]) {
                s++;
                continue;
            }
            if (c == '\'') in_sq = 0;
            continue;
        }
        if (in_bt) {
            if (c == '`') in_bt = 0;
            continue;
        }
        if (c == '/' && s[1] == '/') {
            in_line = 1;
            s++;
            continue;
        }
        if (c == '/' && s[1] == '*') {
            in_block = 1;
            s++;
            continue;
        }
        if (c == '"') {
            in_dq = 1;
            continue;
        }
        if (c == '\'') {
            in_sq = 1;
            continue;
        }
        if (c == '`') {
            in_bt = 1;
            continue;
        }
        if (c == lead && s[1] == '>') return s;
    }
    return NULL;
}

/* Rewrite one `lhs ?> [>(bind)] rhs` occurrence. Returns 1 if rewritten. */
static int shadow_rewrite_one_qmark(char* expr, size_t cap) {
    char out[8192];
    const char* p;
    int op;
    int ls, le, rs, re;
    char bind[64];
    int bi;
    if (!expr || !cap) return 0;
    p = shadow_find_sigil_live(expr, '?');
    if (!p) return 0;
    op = (int)(p - expr);
    le = op;
    ls = shadow_expr_lhs_start(expr, le);
    if (ls < 0) return 0;
    bi = 0;
    bind[0] = 0;
    rs = op + 2;
    /* ?>(ident) binder — no space between ?> and ( */
    if (expr[rs] == '(' && shadow_is_id0(expr[rs + 1])) {
        int k = rs + 1;
        while (expr[k] && shadow_is_id(expr[k])) k++;
        if (expr[k] == ')' && k > rs + 1) {
            int n = k - (rs + 1);
            if (n >= (int)sizeof(bind)) n = (int)sizeof(bind) - 1;
            memcpy(bind, expr + rs + 1, (size_t)n);
            bind[n] = 0;
            bi = 1;
            rs = k + 1;
        }
    }
    while (expr[rs] == ' ' || expr[rs] == '\t') rs++;
    {
        int depth = 0;
        re = rs;
        while (expr[re]) {
            char c = expr[re];
            if (c == '(' || c == '[' || c == '{') depth++;
            else if (c == ')' || c == ']' || c == '}') {
                if (depth == 0) break;
                depth--;
            } else if (depth == 0 && (c == ',' || c == ';')) break;
            else if (depth == 0 && (c == '+' || c == '-' || c == '*' || c == '/' ||
                                    c == '%' || c == '|' || c == '&' || c == '^' ||
                                    c == '<' || c == '>' || c == '?' || c == ':') &&
                     !(c == '>' && expr[re + 1] == '=') &&
                     !(c == '<' && expr[re + 1] == '=') &&
                     !(c == '+' && expr[re + 1] == '+') &&
                     !(c == '-' && expr[re + 1] == '-')) {
                /* Stop at binary ops so `a ?> 0 > 10` binds ?> tightly.
                 * Keep unary/paren defaults: `?> (-1)` already consumed '('. */
                if (re > rs) break;
            }
            re++;
        }
    }
    if (re <= rs) return 0;
    if (bi) {
        if (snprintf(out, sizeof(out),
                     "%.*s({ __typeof__(%.*s) __r = (%.*s); "
                     "!__cc_uw_is_err(__r) ? __cc_uw_value(__r) : "
                     "({ CCError %s = __cc_uw_err_at(__r, \"unwrap\", __FILE__, \"0\"); "
                     "(void)%s; (%.*s); }); })%s",
                     ls, expr, le - ls, expr + ls, le - ls, expr + ls,
                     bind, bind, re - rs, expr + rs, expr + re) >= (int)sizeof(out))
            return 0;
    } else {
        if (snprintf(out, sizeof(out),
                     "%.*s({ __typeof__(%.*s) __r = (%.*s); "
                     "!__cc_uw_is_err(__r) ? __cc_uw_value(__r) : (%.*s); })%s",
                     ls, expr, le - ls, expr + ls, le - ls, expr + ls,
                     re - rs, expr + rs, expr + re) >= (int)sizeof(out))
            return 0;
    }
    if (strlen(out) >= cap) return 0;
    snprintf(expr, cap, "%s", out);
    return 1;
}

static int shadow_rewrite_variant_qmark(char* expr, size_t cap);

static int shadow_recv_name_before(const char* expr, const char* dot,
                                   char* name, size_t cap);

/* Sole leftover `?>` text rewrite entry (emit-time). Prefer parse-time
 * AST_* unwrap forms; do not call `shadow_rewrite_one_qmark` from ad-hoc sites. */
static void shadow_emit_leftover_qmark_rewrite(char* expr, size_t cap) {
    int guard = 0;
    if (!expr || !cap) return;
    if (shadow_rewrite_variant_qmark(expr, cap)) return;
    while (guard++ < 8 && shadow_rewrite_one_qmark(expr, cap)) { }
}

/* Fail-loud if emit product still contains Concurrent-C surface tokens.
 * Skips line/block comments, string/char/tick literals, and preprocessor
 * directive lines (`#define` / `#if` / `#line` / …, including `\`
 * continuations) — same inert contract as `CCInertScan` / M7.B. Tick
 * literals skip `${{…}}`. Attributes and exotic type punct in *code* refuse. */
static int shadow_product_host_c_ok(const char* buf, size_t len, const char** hit) {
    size_t i = 0;
    int in_line = 0, in_block = 0, in_dq = 0, in_sq = 0, in_bt = 0;
    int in_pp = 0, pp_continued = 0, at_line_start = 1;
    if (hit) *hit = NULL;
    if (!buf) return 1;
    while (i < len) {
        char c = buf[i];
        if (in_pp) {
            if (c == '\n') {
                if (pp_continued) pp_continued = 0;
                else {
                    in_pp = 0;
                    at_line_start = 1;
                }
                i++;
                continue;
            }
            if (c == '\\') {
                size_t k = i + 1;
                while (k < len && (buf[k] == ' ' || buf[k] == '\t')) k++;
                pp_continued = (k < len && buf[k] == '\n') ? 1 : 0;
                i++;
                continue;
            }
            if (c != ' ' && c != '\t') pp_continued = 0;
            i++;
            continue;
        }
        if (in_line) {
            if (c == '\n') {
                in_line = 0;
                at_line_start = 1;
            }
            i++;
            continue;
        }
        if (in_block) {
            if (c == '*' && i + 1 < len && buf[i + 1] == '/') {
                in_block = 0;
                i += 2;
            } else
                i++;
            continue;
        }
        if (in_dq) {
            if (c == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (c == '"') in_dq = 0;
            i++;
            continue;
        }
        if (in_sq) {
            if (c == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (c == '\'') in_sq = 0;
            i++;
            continue;
        }
        if (in_bt) {
            /* `${{ … }}` — content (including nested ticks) is not code. */
            if (c == '$' && i + 3 < len && buf[i + 1] == '{' &&
                buf[i + 2] == '{') {
                size_t j = i + 3;
                int depth = 1;
                while (j + 1 < len && depth > 0) {
                    if (buf[j] == '{' && buf[j + 1] == '{') {
                        depth++;
                        j += 2;
                        continue;
                    }
                    if (buf[j] == '}' && buf[j + 1] == '}') {
                        depth--;
                        j += 2;
                        continue;
                    }
                    j++;
                }
                i = j;
                continue;
            }
            if (c == '`') in_bt = 0;
            i++;
            continue;
        }
        /* `#define` / `#if 0` / `#line` bodies are inert host-C surface. */
        if (at_line_start) {
            size_t j = i;
            while (j < len && (buf[j] == ' ' || buf[j] == '\t')) j++;
            if (j < len && buf[j] == '#') {
                in_pp = 1;
                pp_continued = 0;
                at_line_start = 0;
                i = j + 1;
                continue;
            }
        }
        if (c == '/' && i + 1 < len && buf[i + 1] == '/') {
            in_line = 1;
            at_line_start = 0;
            i += 2;
            continue;
        }
        if (c == '/' && i + 1 < len && buf[i + 1] == '*') {
            /* Legacy slash-star @as star-slash is leftover sugar — refuse. */
            if (i + 7 <= len && memcmp(buf + i, "/*@as*/", 7) == 0) {
                if (hit) *hit = buf + i;
                return 0;
            }
            in_block = 1;
            at_line_start = 0;
            i += 2;
            continue;
        }
        if (c == '"') {
            in_dq = 1;
            at_line_start = 0;
            i++;
            continue;
        }
        if (c == '\'') {
            in_sq = 1;
            at_line_start = 0;
            i++;
            continue;
        }
        if (c == '`') {
            in_bt = 1;
            at_line_start = 0;
            i++;
            continue;
        }
        if (c == '!' && i + 1 < len && buf[i + 1] == '>') {
            if (hit) *hit = buf + i;
            return 0;
        }
        if (c == '?' && i + 1 < len && buf[i + 1] == '>') {
            if (hit) *hit = buf + i;
            return 0;
        }
        if (c == '=' && i + 1 < len && buf[i + 1] == '>') {
            if (hit) *hit = buf + i;
            return 0;
        }
        /* `T[:]` / `T[:!]` / `T[0:]` slice sugar left in product. */
        if (c == '[' && i + 2 < len && buf[i + 1] == ':') {
            if (hit) *hit = buf + i;
            return 0;
        }
        if (c == '[' && i + 3 < len && buf[i + 1] == '0' && buf[i + 2] == ':') {
            if (hit) *hit = buf + i;
            return 0;
        }
        /* `Family::[args]` leftover in code (not in #define — those are pp). */
        if (c == ':' && i + 1 < len && buf[i + 1] == ':') {
            if (hit) *hit = buf + i;
            return 0;
        }
        if (c == '@') {
            static const char* bans[] = {
                "@as", "@slice", "@await", "@string", "@create", "@scratch",
                "@destroy", "@errhandler", "@async", "@variant", "@comptime",
                "@emit", "@defer", "@err", "@with_deadline", "@parallel",
                "@serial", NULL
            };
            int b;
            for (b = 0; bans[b]; b++) {
                size_t bl = strlen(bans[b]);
                if (i + bl <= len && memcmp(buf + i, bans[b], bl) == 0) {
                    char after = (i + bl < len) ? buf[i + bl] : 0;
                    if (!((after >= 'a' && after <= 'z') ||
                          (after >= 'A' && after <= 'Z') ||
                          (after >= '0' && after <= '9') || after == '_')) {
                        if (hit) *hit = buf + i;
                        return 0;
                    }
                }
            }
        }
        /* Bare `await` left in product (strip path must not resurrect it). */
        if (c == 'a' && i + 5 <= len && memcmp(buf + i, "await", 5) == 0) {
            char before = (i > 0) ? buf[i - 1] : 0;
            char after = (i + 5 < len) ? buf[i + 5] : 0;
            int before_ok = !((before >= 'a' && before <= 'z') ||
                              (before >= 'A' && before <= 'Z') ||
                              (before >= '0' && before <= '9') ||
                              before == '_');
            int after_ok = !((after >= 'a' && after <= 'z') ||
                             (after >= 'A' && after <= 'Z') ||
                             (after >= '0' && after <= '9') || after == '_');
            if (before_ok && after_ok) {
                if (hit) *hit = buf + i;
                return 0;
            }
        }
        if (c == '\n') at_line_start = 1;
        else if (c != ' ' && c != '\t') at_line_start = 0;
        i++;
    }
    return 1;
}

/* Rewrite @slice(lit) → CC_SLICE_LIT(lit). Optional sugar; call-arg lits
 * coerce the same way without @slice. */
/* Surface `char[:…]` / `char[0:]` → CCSlice (grammar seam / raw fields). */
static void shadow_rewrite_char_slice_types(char* text, size_t cap) {
    char tmp[2048];
    const char* pats[] = {"char[:0!]", "char[:0]", "char[0:]", "char[::]",
                          "char[:!]", "char[:]", NULL};
    const char* repls[] = {"CCSliceUnique", "CCSlice", "CCSlice", "CCSlice",
                           "CCSliceUnique", "CCSlice", NULL};
    int pi;
    if (!text || !cap || !text[0]) return;
    snprintf(tmp, sizeof(tmp), "%s", text);
    for (pi = 0; pats[pi]; pi++) {
        char cur[2048];
        char* p;
        size_t plen = strlen(pats[pi]);
        snprintf(cur, sizeof(cur), "%s", tmp);
        tmp[0] = 0;
        p = cur;
        while (*p) {
            char* hit = strstr(p, pats[pi]);
            size_t keep;
            size_t used;
            if (!hit) {
                used = strlen(tmp);
                snprintf(tmp + used, sizeof(tmp) - used, "%s", p);
                break;
            }
            keep = (size_t)(hit - p);
            used = strlen(tmp);
            if (used + keep + strlen(repls[pi]) + 1 >= sizeof(tmp)) break;
            memcpy(tmp + used, p, keep);
            tmp[used + keep] = 0;
            used = strlen(tmp);
            snprintf(tmp + used, sizeof(tmp) - used, "%s", repls[pi]);
            p = hit + plen;
        }
    }
    snprintf(text, cap, "%s", tmp);
}

/* `T[:]` / `T[:!]` in type text (fn ret/params) → CCSlice_T / CCSliceUnique.
 * char[:*] already rewritten above; remaining idents get the typed instance. */
static void shadow_rewrite_elem_slice_types(char* text, size_t cap) {
    char out[2048];
    size_t oi = 0;
    const char* p;
    if (!text || !cap || !text[0]) return;
    p = text;
    while (*p && oi + 1 < sizeof(out)) {
        int is_id0 = ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                      *p == '_');
        int prev_id = (p > text &&
                       ((p[-1] >= 'A' && p[-1] <= 'Z') ||
                        (p[-1] >= 'a' && p[-1] <= 'z') ||
                        (p[-1] >= '0' && p[-1] <= '9') || p[-1] == '_'));
        if (is_id0 && !prev_id) {
            const char* id = p;
            size_t n = 0;
            while ((p[n] >= 'A' && p[n] <= 'Z') || (p[n] >= 'a' && p[n] <= 'z') ||
                   (p[n] >= '0' && p[n] <= '9') || p[n] == '_')
                n++;
            if (p[n] == '[' && p[n + 1] == ':') {
                int unique = (p[n + 2] == '!');
                const char* after = p + n + (unique ? 3 : 2);
                if (*after == ']') {
                    char repl[96];
                    size_t rl;
                    after++;
                    if (unique)
                        snprintf(repl, sizeof(repl), "CCSliceUnique");
                    else
                        snprintf(repl, sizeof(repl), "CCSlice_%.*s", (int)n, id);
                    rl = strlen(repl);
                    if (oi + rl >= sizeof(out)) break;
                    memcpy(out + oi, repl, rl);
                    oi += rl;
                    p = after;
                    continue;
                }
            }
        }
        out[oi++] = *p++;
    }
    out[oi] = 0;
    snprintf(text, cap, "%s", out);
}

/* Type-position slice sugar for signatures (ret + params). */
static void shadow_rewrite_slice_types(char* text, size_t cap) {
    shadow_rewrite_char_slice_types(text, cap);
    shadow_rewrite_elem_slice_types(text, cap);
}

/* `Ok!>(Err)` / `Ok !>(Err)` anywhere in a type spelling (fn-ptr params). */
static void shadow_rewrite_result_types_in_text(char* text, size_t cap) {
    char out[2048];
    const char* p;
    size_t oi = 0;
    if (!text || !cap || !text[0]) return;
    p = text;
    while (*p && oi + 1 < sizeof(out)) {
        const char* bang = strstr(p, "!>(");
        const char* qmark = strstr(p, "?>(");
        const char* hit = bang;
        size_t mark = 3; /* !>( */
        const char* ty_end;
        const char* ty_lo;
        const char* err_p;
        char ok[128], err[128], rname[256];
        size_t ol, el;
        int depth;
        if (qmark && (!hit || qmark < hit)) {
            hit = qmark;
            mark = 3; /* ?>( */
        }
        if (!hit) {
            size_t n = strlen(p);
            if (oi + n >= sizeof(out)) break;
            memcpy(out + oi, p, n);
            oi += n;
            break;
        }
        ty_end = hit;
        while (ty_end > p && (ty_end[-1] == ' ' || ty_end[-1] == '\t'))
            ty_end--;
        ty_lo = ty_end;
        while (ty_lo > p) {
            char c = ty_lo[-1];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '*' ||
                c == ' ' || c == '\t')
                ty_lo--;
            else
                break;
        }
        while (ty_lo < ty_end && (*ty_lo == ' ' || *ty_lo == '\t')) ty_lo++;
        ol = (size_t)(ty_end - ty_lo);
        if (ol == 0 || ol >= sizeof(ok)) {
            size_t keep = (size_t)(hit + mark - p);
            if (oi + keep >= sizeof(out)) break;
            memcpy(out + oi, p, keep);
            oi += keep;
            p = hit + mark;
            continue;
        }
        memcpy(ok, ty_lo, ol);
        ok[ol] = 0;
        while (ol && (ok[ol - 1] == ' ' || ok[ol - 1] == '\t')) ok[--ol] = 0;
        err_p = hit + mark; /* skip !>( / ?>( */
        depth = 1;
        el = 0;
        err[0] = 0;
        while (*err_p && depth > 0) {
            if (*err_p == '(') depth++;
            else if (*err_p == ')') {
                depth--;
                if (depth == 0) {
                    err_p++;
                    break;
                }
            }
            if (depth > 0) {
                if (el + 1 >= sizeof(err)) break;
                err[el++] = *err_p;
            }
            err_p++;
        }
        err[el] = 0;
        while (el && (err[el - 1] == ' ' || err[el - 1] == '\t')) err[--el] = 0;
        if (!el || depth != 0) {
            size_t keep = (size_t)(hit + mark - p);
            if (oi + keep >= sizeof(out)) break;
            memcpy(out + oi, p, keep);
            oi += keep;
            p = hit + mark;
            continue;
        }
        {
            size_t prefix = (size_t)(ty_lo - p);
            size_t rl;
            if (oi + prefix >= sizeof(out)) break;
            if (prefix) {
                memcpy(out + oi, p, prefix);
                oi += prefix;
            }
            shadow_result_name(ok, err, rname, sizeof(rname));
            rl = strlen(rname);
            if (oi + rl >= sizeof(out)) break;
            memcpy(out + oi, rname, rl);
            oi += rl;
            p = err_p;
        }
    }
    out[oi] = 0;
    snprintf(text, cap, "%s", out);
}

/* Result ok-type from AST (`char[:]`) → host spelling for CCResult_* mangle. */
static void shadow_result_ok_buf(char* dst, size_t cap, const char* src) {
    if (!dst || !cap) return;
    snprintf(dst, cap, "%s", src ? src : "");
    shadow_rewrite_slice_types(dst, cap);
}

static void shadow_rewrite_at_slice(char* expr, size_t cap) {
    /* Large enough for unwrap calls that embed @string / long @slice args. */
    char tmp[8192];
    const char* p;
    char* o;
    size_t rem;
    if (!expr || !cap) return;
    p = expr;
    o = tmp;
    rem = sizeof(tmp) - 1;
    tmp[0] = 0;
    while (*p && rem > 0) {
        if (p[0] == '@' && strncmp(p, "@slice(", 7) == 0) {
            const char* args = p + 7;
            const char* q = args;
            int dep = 1;
            int in_dq = 0, in_sq = 0;
            while (*q && dep > 0) {
                if (in_dq) {
                    if (*q == '\\' && q[1]) {
                        q += 2;
                        continue;
                    }
                    if (*q == '"') in_dq = 0;
                    q++;
                    continue;
                }
                if (in_sq) {
                    if (*q == '\\' && q[1]) {
                        q += 2;
                        continue;
                    }
                    if (*q == '\'') in_sq = 0;
                    q++;
                    continue;
                }
                if (*q == '"') {
                    in_dq = 1;
                    q++;
                    continue;
                }
                if (*q == '\'') {
                    in_sq = 1;
                    q++;
                    continue;
                }
                if (*q == '(') dep++;
                else if (*q == ')') dep--;
                q++;
            }
            if (dep == 0) {
                size_t alen = (size_t)((q - 1) - args);
                int n = snprintf(o, rem, "CC_SLICE_LIT(%.*s)", (int)alen, args);
                if (n < 0 || (size_t)n >= rem) break;
                o += n;
                rem -= (size_t)n;
                p = q;
                continue;
            }
        }
        *o++ = *p++;
        rem--;
    }
    *o = 0;
    snprintf(expr, cap, "%s", tmp);
}

/* Rewrite #include <… .h> / "… .h" → .h for host/TCC consumption. */
static void shadow_rewrite_pass_inc(char* dst, size_t cap, const char* src) {
    size_t n = src ? strlen(src) : 0;
    /* "...foo.h>" / "...foo.cch\"" → "...foo.h>" / "...foo.h\"" */
    if (n >= 5 && src[n - 5] == '.' && src[n - 4] == 'c' && src[n - 3] == 'c' &&
        src[n - 2] == 'h' && (src[n - 1] == '>' || src[n - 1] == '"')) {
        snprintf(dst, cap, "%.*s.h%c", (int)(n - 5), src, src[n - 1]);
        return;
    }
    snprintf(dst, cap, "%s", src ? src : "");
}

/* Channel decls for emit-site cc_channel_pair → create_named. */
typedef struct {
    char name[64];
    char cap[64];
    char elem[64]; /* "int", "int*", … */
    char topo[16]; /* "1:1" or empty */
    int ordered;
    int is_sync;
    int bp_mode; /* 0 block, 1 drop_new, 2 drop_old */
} ShadowChanDecl;

enum { SHADOW_CHAN_CAP = 256 };
enum {
    SHADOW_DEFER_SCOPE_MAX = 32,
    SHADOW_DEFER_SCOPE_DEFERS = 16
};
/* One lexical block/loop body: @defer and @destroy sites in source order.
 * Runtime `__cc_shw_<id>` gates cleanups so unreached mid-body sites are
 * skipped on break/continue/soft-return (same contract as fn __cc_defer_hw). */
typedef struct {
    AstNode* life[SHADOW_DEFER_SCOPE_DEFERS];
    int nlife;
    int is_loop;
    int shw_id; /* __cc_shw_<id> */
} ShadowDeferScope;

enum { SHADOW_EH_STACK_CAP = 8 };
typedef struct {
    AstNode* eh;          /* nearest @errhandler (default) */
    AstNode* ehs[SHADOW_EH_STACK_CAP]; /* stacked handlers, outer→inner */
    int ehs_scope[SHADOW_EH_STACK_CAP]; /* lexical block id at push */
    int nehs;
    int eh_scope;         /* current compound; bumped on nested block */
    char eh_proj[128];    /* @as path E→F when pass-2 face match */
    const char* rname;    /* enclosing CCResult_* name, or NULL */
    const char* body_indent; /* fn/closure body indent from source */
    int* deadline_i;      /* @with_deadline id counter */
    int defer_cleanup;    /* result fn uses @defer → goto cleanup */
    int goto_cleanup;     /* fn has @destroy/@defer → returns goto cleanup */
    const char* soft_ret_ty; /* enclosing fn return type for soft cleanup */
    AstNode** defers;     /* borrowed defer nodes (reverse emit) */
    int ndefers;
    AstNode** destroys;   /* borrowed PTR_UNWRAP/NURSERY with @destroy */
    int ndestroys;
    TapeCache* cache;     /* for replaying source trivia */
    /* Last `#line` emitted (dedupe consecutive identical markers). */
    int line_file_id;
    int line_no;
    AstNode* site;        /* current source site for err_at / #line resync */
    ShadowChanDecl* chans; /* TU-scoped channel handle table */
    int* nchans;
    int send_task_ret; /* closure packs int into cc_task_result_ptr */
    unsigned owner_fn_attrs; /* enclosing @async fn attrs for autoblock */
    unsigned block_attrs;    /* lexical @nonblocking/@blocking ambient */
    int defer_emit;          /* emitting @defer body — no autoblock */
    /* `@err(e)` expanding an @errhandler inside a bang binder: legacy
     * statement-expr-in-poll made `return N` escape the poll fn and leave
     * the task result at 0.  Body-helper async emit maps those returns to 0. */
    int err_via_bang;
    struct { char name[64]; char ty[64]; } vdrops[64];
    int nvdrops;
    int vdrop_marks[32];
    int nvdrop_marks;
    ShadowDeferScope defer_stack[SHADOW_DEFER_SCOPE_MAX];
    int defer_depth;
    /* Open call-local `println(@string(..., @scratch))` checkpoints
     * (`__cc_scratch_cpN`). Soft-return / break / continue restore these
     * before leaving — fallthrough still pops via shadow_scratch_cp_pop. */
    int scratch_cp_depth;
    /* Inside a '@parallel wait for' body: 1 = sequential denial path,
     * 2 = per-ticket run. Also 1 while emitting a join @serial arm or
     * @parallel for iteration that owns @stage. @stage is only legal
     * when nonzero. */
    int pw_body;
    int pw_id;            /* wait-for helper id (`__cc_pw_done_<id>`) */
    int pw_loop;          /* inner for/while/do depth (continue targets) */
    int pw_brk;           /* inner loop+switch depth (break targets) */
    char pw_ivar[64];     /* wait-for loop variable */
    int pw_fin;           /* sequential path tracks wait-for break */
    AstNode* pw_stages[16]; /* top-level @stage nodes (seq continue) */
    int pw_nstages;
    /* Passed-flag prefix. Empty → `__cc_pw_stg_` (wait-for). Join/for
     * regions use `__cc_stg_<id>_` so they do not collide. */
    char stg_pfx[32];
    /* Emitting an `@parallel for` body: return/break write a shared
     * exit cell, then `return` from the walk (caller does the fn return). */
    int pf_body;
    char pf_ex[64];
    /* Emitting an assignment-join `@serial` arm: return writes a shared
     * exit cell, then the construct joins siblings and the function
     * returns. Thunk vs caller chooses `return NULL` vs `goto` join. */
    int pj_body;
    int pj_thunk;
    char pj_ex[64];
    char pj_join[64];
} ShadowCtx;

enum { SHADOW_SCRATCH_CP_MAX = 16 };

static int g_shadow_eh_diag;
static void shadow_variant_err_loc(ShadowCtx* ctx, AstNode* st, CEmit* out,
                                  const char* needle, int no_col,
                                  const char* msg);

static int shadow_as_path_to(const char* outer, const char* face, char* path,
                             size_t cap);

static const char* shadow_eh_bind(ShadowCtx* ctx) {
    if (ctx && ctx->eh && ctx->eh->b[0]) return ctx->eh->b;
    return "e";
}

/* Result-fn name → CCResult_* (for stacked @errhandler type match). */
enum { SHADOW_RFN_CAP = 256 };
static struct {
    char name[64];
    char rname[128];
    char err[64];
} g_shadow_rfns[SHADOW_RFN_CAP];
static int g_shadow_nrfns;

static void shadow_rfn_register(const char* name, const char* rname,
                                const char* err) {
    int i;
    if (!name || !name[0]) return;
    if (g_shadow_nrfns >= SHADOW_RFN_CAP) {
        shadow_table_full("rfns", SHADOW_RFN_CAP, name);
        return;
    }
    for (i = 0; i < g_shadow_nrfns; i++) {
        if (strcmp(g_shadow_rfns[i].name, name) == 0) {
            if (rname && rname[0])
                snprintf(g_shadow_rfns[i].rname, sizeof(g_shadow_rfns[i].rname),
                         "%s", rname);
            if (err && err[0])
                snprintf(g_shadow_rfns[i].err, sizeof(g_shadow_rfns[i].err),
                         "%s", err);
            return;
        }
    }
    snprintf(g_shadow_rfns[g_shadow_nrfns].name,
             sizeof(g_shadow_rfns[0].name), "%s", name);
    snprintf(g_shadow_rfns[g_shadow_nrfns].rname,
             sizeof(g_shadow_rfns[0].rname), "%s", rname ? rname : "");
    snprintf(g_shadow_rfns[g_shadow_nrfns].err, sizeof(g_shadow_rfns[0].err),
             "%s", err ? err : "");
    g_shadow_nrfns++;
}

static int shadow_rname_split_ok(const char* rname, const char* err, char* ok,
                                 size_t cap) {
    const char* pref = "CCResult_";
    size_t plen = 9;
    size_t elen, rest, ol;
    if (!rname || !err || !err[0] || !ok || cap < 2) return 0;
    if (strncmp(rname, pref, plen) != 0) return 0;
    elen = strlen(err);
    rest = strlen(rname);
    if (rest <= plen + 1 + elen) return 0;
    if (rname[rest - elen - 1] != '_') return 0;
    if (strcmp(rname + rest - elen, err) != 0) return 0;
    ol = rest - plen - 1 - elen;
    if (ol >= cap) ol = cap - 1;
    memcpy(ok, rname + plen, ol);
    ok[ol] = 0;
    return ok[0] != 0;
}

/* Exact name, else unique `Type_method` suffix for UFCS `recv.method(`. */
static int shadow_rfn_index(const char* name) {
    int i, hit = -1, n = 0;
    size_t nlen;
    if (!name || !name[0]) return -1;
    for (i = 0; i < g_shadow_nrfns; i++) {
        if (strcmp(g_shadow_rfns[i].name, name) == 0) return i;
    }
    nlen = strlen(name);
    for (i = 0; i < g_shadow_nrfns; i++) {
        const char* rn = g_shadow_rfns[i].name;
        size_t rlen = strlen(rn);
        if (rlen > nlen + 1 && rn[rlen - nlen - 1] == '_' &&
            strcmp(rn + rlen - nlen, name) == 0) {
            n++;
            hit = i;
        }
    }
    return n == 1 ? hit : -1;
}

extern int cc_result_fn_registry_get_err_type(const char* name, size_t name_len,
                                              char* out_buf, size_t out_sz);
extern int cc_result_fn_registry_contains(const char* name, size_t len);
extern const char* cc_result_fn_registry_name_at(size_t i);
extern const char* cc_result_fn_registry_err_type_at(size_t i);
extern const char* cc_result_fn_registry_result_type_at(size_t i);
extern size_t cc_result_fn_registry_count(void);
extern void cc_result_fn_registry_scan_source(const char* src, size_t n);
extern size_t cc_included_cch_source_count(void);
extern const char* cc_included_cch_source_path(size_t i);
extern size_t cc_lowered_local_header_count(void);
extern const char* cc_lowered_local_header_source_path(size_t i);

static char g_shadow_rfn_hdr_err[64];

static void shadow_rfn_scan_path(const char* path) {
    FILE* f;
    long sz;
    char* buf;
    if (!path || !path[0]) return;
    f = fopen(path, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    sz = ftell(f);
    if (sz <= 0 || sz > 8 * 1024 * 1024) {
        fclose(f);
        return;
    }
    rewind(f);
    buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return;
    }
    buf[sz] = 0;
    fclose(f);
    cc_result_fn_registry_scan_source(buf, (size_t)sz);
    free(buf);
}

/* shadow_lower is a child process; the driver's header scan does not
 * carry over. Walk include roots for Result protos. */
static void shadow_rfn_scan_dir_faces(const char* dir) {
    DIR* d;
    struct dirent* ent;
    char path[1024];
    if (!dir || !dir[0]) return;
    d = opendir(dir);
    if (!d) return;
    while ((ent = readdir(d)) != NULL) {
        size_t n = strlen(ent->d_name);
        if (n < 3) continue;
        if (strcmp(ent->d_name + n - 4, ".h") != 0 &&
            strcmp(ent->d_name + n - 2, ".h") != 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        shadow_rfn_scan_path(path);
    }
    closedir(d);
}

static void shadow_rfn_scan_included_headers(void) {
    static int once;
    const char* env;
    size_t i, n;
    if (once) return;
    once = 1;
    n = cc_included_cch_source_count();
    for (i = 0; i < n; i++)
        shadow_rfn_scan_path(cc_included_cch_source_path(i));
    n = cc_lowered_local_header_count();
    for (i = 0; i < n; i++)
        shadow_rfn_scan_path(cc_lowered_local_header_source_path(i));
    env = getenv("CC_INCLUDE_PATH");
    if (env && env[0]) {
        char buf[4096];
        char* p;
        char* next;
        snprintf(buf, sizeof(buf), "%s", env);
        for (p = buf; p && *p; p = next) {
            char ccc[1024];
            next = strchr(p, ':');
            if (next) {
                *next = 0;
                next++;
            }
            if (!p[0]) continue;
            snprintf(ccc, sizeof(ccc), "%s/ccc", p);
            shadow_rfn_scan_dir_faces(ccc);
            shadow_rfn_scan_dir_faces(p);
        }
    }
    shadow_rfn_scan_dir_faces("out/include/ccc");
    shadow_rfn_scan_dir_faces("cc/include/ccc");
}

/* Extracted `.cch` Result protos are not AST_RESULT_FN. The driver scan
 * of included / lowered-local headers still recorded their E. */
static const char* shadow_rfn_err_from_hdr(const char* name) {
    size_t k, n, nlen, hits;
    const char* hit;
    if (!name || !name[0]) return NULL;
    shadow_rfn_scan_included_headers();
    if (cc_result_fn_registry_get_err_type(name, strlen(name),
                                           g_shadow_rfn_hdr_err,
                                           sizeof(g_shadow_rfn_hdr_err)) &&
        g_shadow_rfn_hdr_err[0])
        return g_shadow_rfn_hdr_err;
    nlen = strlen(name);
    n = cc_result_fn_registry_count();
    hits = 0;
    hit = NULL;
    for (k = 0; k < n; k++) {
        const char* rn = cc_result_fn_registry_name_at(k);
        size_t rlen;
        const char* e;
        if (!rn || !rn[0]) continue;
        rlen = strlen(rn);
        if (rlen <= nlen + 1 || rn[rlen - nlen - 1] != '_' ||
            strcmp(rn + rlen - nlen, name) != 0)
            continue;
        e = cc_result_fn_registry_err_type_at(k);
        if (!e || !e[0]) continue;
        hits++;
        hit = e;
    }
    if (hits == 1 && hit) {
        snprintf(g_shadow_rfn_hdr_err, sizeof(g_shadow_rfn_hdr_err), "%s", hit);
        return g_shadow_rfn_hdr_err;
    }
    return NULL;
}

static const char* shadow_rfn_err(const char* name) {
    int i = shadow_rfn_index(name);
    if (i >= 0 && g_shadow_rfns[i].err[0]) return g_shadow_rfns[i].err;
    return shadow_rfn_err_from_hdr(name);
}

static int shadow_rfn_ok(const char* name, char* ok, size_t cap) {
    int i = shadow_rfn_index(name);
    ok[0] = 0;
    if (i < 0 || !g_shadow_rfns[i].rname[0] || !g_shadow_rfns[i].err[0])
        return 0;
    return shadow_rname_split_ok(g_shadow_rfns[i].rname, g_shadow_rfns[i].err,
                                 ok, cap);
}

/* Outermost call ident: `outer(inner())` → outer, `recv.method(` → method.
 * Nested args are not the unwrap target (mmap: cc_file_map(cc_slice_cstr)). */
static const char* shadow_outer_call_ident(const char* p) {
    const char* last = NULL;
    const char* s;
    int depth = 0;
    int in_dq = 0, in_sq = 0;
    if (!p) return NULL;
    for (s = p; *s; s++) {
        char c = *s;
        if (in_dq) {
            if (c == '\\' && s[1]) {
                s++;
                continue;
            }
            if (c == '"') in_dq = 0;
            continue;
        }
        if (in_sq) {
            if (c == '\\' && s[1]) {
                s++;
                continue;
            }
            if (c == '\'') in_sq = 0;
            continue;
        }
        if (c == '"') {
            in_dq = 1;
            continue;
        }
        if (c == '\'') {
            in_sq = 1;
            continue;
        }
        if (c == '(' || c == '{') {
            depth++;
            continue;
        }
        if (c == ')' || c == '}') {
            if (depth > 0) depth--;
            continue;
        }
        if (depth != 0) continue;
        if (((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') &&
            (s == p ||
             !((s[-1] >= 'A' && s[-1] <= 'Z') ||
               (s[-1] >= 'a' && s[-1] <= 'z') ||
               (s[-1] >= '0' && s[-1] <= '9') || s[-1] == '_'))) {
            const char* id = s;
            size_t n = 0;
            const char* q;
            while ((id[n] >= 'A' && id[n] <= 'Z') ||
                   (id[n] >= 'a' && id[n] <= 'z') ||
                   (id[n] >= '0' && id[n] <= '9') || id[n] == '_')
                n++;
            q = id + n;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '(') last = id;
        }
    }
    return last;
}

/* Pick stacked @errhandler: exact E first (inner→outer), then unique
 * @typeview as: path (inner→outer). Same-block two face matches are
 * ill-formed. Candidates are declaration-point (pushed when visited). */
static AstNode* shadow_eh_for_call(ShadowCtx* ctx, const char* call) {
    char fname[64];
    size_t ni = 0;
    const char* p;
    const char* err;
    int i;
    if (ctx) ctx->eh_proj[0] = 0;
    if (!ctx || ctx->nehs <= 0) return ctx ? ctx->eh : NULL;
    if (!call || !call[0]) return ctx->eh;
    p = call;
    while (*p == ' ' || *p == '\t') p++;
    /* UFCS coerce stmt-expr: `({ CCSlice __cc_pv = …; cc_slice_println(…); })`
     * — peel the trailing live call so we don't bind innermost Io onto print. */
    if (p[0] == '(' && p[1] == '{') {
        const char* last = NULL;
        const char* s = p;
        int depth = 0;
        int in_dq = 0, in_sq = 0;
        for (; *s; s++) {
            char c = *s;
            if (in_dq) {
                if (c == '\\' && s[1]) {
                    s++;
                    continue;
                }
                if (c == '"') in_dq = 0;
                continue;
            }
            if (in_sq) {
                if (c == '\\' && s[1]) {
                    s++;
                    continue;
                }
                if (c == '\'') in_sq = 0;
                continue;
            }
            if (c == '"') {
                in_dq = 1;
                continue;
            }
            if (c == '\'') {
                in_sq = 1;
                continue;
            }
            if (c == '(' || c == '{') depth++;
            else if (c == ')' || c == '}') {
                if (depth > 0) depth--;
            }
            if (((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') &&
                (s == p ||
                 !((s[-1] >= 'A' && s[-1] <= 'Z') ||
                   (s[-1] >= 'a' && s[-1] <= 'z') ||
                   (s[-1] >= '0' && s[-1] <= '9') || s[-1] == '_'))) {
                const char* id = s;
                size_t n = 0;
                while ((id[n] >= 'A' && id[n] <= 'Z') ||
                       (id[n] >= 'a' && id[n] <= 'z') ||
                       (id[n] >= '0' && id[n] <= '9') || id[n] == '_')
                    n++;
                {
                    const char* q = id + n;
                    while (*q == ' ' || *q == '\t') q++;
                    if (*q == '(') last = id;
                }
            }
            if (depth == 0 && s > p) break;
        }
        if (last) p = last;
    }
    {
        const char* outer = shadow_outer_call_ident(p);
        if (outer) p = outer;
    }
    while (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '_') &&
           ni + 1 < sizeof(fname))
        fname[ni++] = *p++;
    fname[ni] = 0;
    while (*p == ' ' || *p == '\t') p++;
    /* Unparseable callee: prefer CCError (script default), not innermost Io. */
    if (!fname[0] || *p != '(') {
        for (i = ctx->nehs - 1; i >= 0; i--) {
            if (ctx->ehs[i] && ctx->ehs[i]->a[0] &&
                strcmp(ctx->ehs[i]->a, "CCError") == 0)
                return ctx->ehs[i];
        }
        return ctx->eh;
    }
    err = shadow_rfn_err(fname);
    /* Stdlib faces not always AST_RESULT_FN-registered from .cch protos. */
    if (!err || !err[0]) {
        if (strcmp(fname, "cc_command_status") == 0 ||
            strcmp(fname, "cc_command_output") == 0 ||
            strcmp(fname, "cc_command_output_with_input") == 0 ||
            strcmp(fname, "cc_process_spawn") == 0 ||
            strcmp(fname, "cc_process_wait") == 0 ||
            strcmp(fname, "cc_process_write") == 0)
            err = "CCIoError";
        else if (strcmp(fname, "cc_println") == 0 ||
                 strcmp(fname, "cc_eprintln") == 0 ||
                 strcmp(fname, "cc_print") == 0 ||
                 strcmp(fname, "cc_eprint") == 0 ||
                 strcmp(fname, "cc_fprintln") == 0 ||
                 strcmp(fname, "cc_fprint") == 0 ||
                 strncmp(fname, "cc_slice_", 9) == 0 ||
                 strncmp(fname, "cc_string_", 10) == 0 ||
                 strncmp(fname, "cc_char_", 8) == 0 ||
                 strncmp(fname, "cc_const_char_", 14) == 0)
            err = "CCError";
        /* Script parents: .cch protos are often not AST_RESULT_FN-registered
         * into the rfn table; UFCS still lowers to these free names. */
        else if (strncmp(fname, "cc_js_", 6) == 0)
            err = "CCJsError";
        else if (strncmp(fname, "cc_py_", 6) == 0)
            err = "CCPyError";
    }
    if (err && err[0]) {
        /* Pass 1: exact E, inner → outer. Outer exact beats inner face. */
        for (i = ctx->nehs - 1; i >= 0; i--) {
            if (ctx->ehs[i] && ctx->ehs[i]->a[0] &&
                strcmp(ctx->ehs[i]->a, err) == 0)
                return ctx->ehs[i];
        }
        /* Pass 2: unique as: path; hop count is not a rank. */
        {
            int face_i = -1;
            int face_scope = -1;
            int same = 0;
            for (i = ctx->nehs - 1; i >= 0; i--) {
                char path[128];
                int pr;
                if (!ctx->ehs[i] || !ctx->ehs[i]->a[0]) continue;
                if (strcmp(ctx->ehs[i]->a, err) == 0) continue;
                pr = shadow_as_path_to(err, ctx->ehs[i]->a, path, sizeof(path));
                if (pr != 1) continue;
                if (face_i < 0) {
                    face_i = i;
                    face_scope = ctx->ehs_scope[i];
                    same = 1;
                    snprintf(ctx->eh_proj, sizeof(ctx->eh_proj), "%s", path);
                } else if (ctx->ehs_scope[i] == face_scope) {
                    same++;
                }
            }
            if (same > 1) {
                fprintf(stderr,
                        "error: ambiguous '@errhandler' for error type '%s': "
                        "multiple as: faces match in-scope handlers\n",
                        err);
                g_shadow_eh_diag = 1;
                ctx->eh_proj[0] = 0;
                return NULL;
            }
            if (face_i >= 0) return ctx->ehs[face_i];
        }
        /* Known E, no exact/face match: do not bind the innermost handler
         * (that emits `F e = (__r).u.error` and a host C type error). */
        if (ctx->eh && ctx->eh->a[0] && strcmp(ctx->eh->a, err) != 0) {
            char have[192];
            char msg[384];
            int hi;
            size_t hn = 0;
            have[0] = 0;
            for (hi = ctx->nehs - 1; hi >= 0; hi--) {
                const char* t;
                if (!ctx->ehs[hi] || !ctx->ehs[hi]->a[0]) continue;
                t = ctx->ehs[hi]->a;
                if (hn && hn + 2 < sizeof(have)) {
                    have[hn++] = ',';
                    have[hn++] = ' ';
                    have[hn] = 0;
                }
                if (hn + strlen(t) + 1 < sizeof(have)) {
                    memcpy(have + hn, t, strlen(t) + 1);
                    hn += strlen(t);
                }
            }
            {
                char ok[64];
                ok[0] = 0;
                (void)shadow_rfn_ok(fname, ok, sizeof(ok));
                if (ok[0])
                    snprintf(msg, sizeof(msg),
                             "'%s' is '%s !>(%s)'; in-scope '@errhandler' "
                             "is '%s'",
                             fname[0] ? fname : "call", ok, err,
                             have[0] ? have : ctx->eh->a);
                else
                    snprintf(msg, sizeof(msg),
                             "'%s' is '!>(%s)'; in-scope '@errhandler' is '%s'",
                             fname[0] ? fname : "call", err,
                             have[0] ? have : ctx->eh->a);
            }
            shadow_variant_err_loc(ctx, ctx->site, NULL, "!>", 0, msg);
            g_shadow_eh_diag = 1;
            ctx->eh_proj[0] = 0;
            return NULL;
        }
        return ctx->eh;
    }
    /* Unknown callee: prefer CCError (script default / @as face), not the
     * innermost specialty handler. */
    for (i = ctx->nehs - 1; i >= 0; i--) {
        if (ctx->ehs[i] && ctx->ehs[i]->a[0] &&
            strcmp(ctx->ehs[i]->a, "CCError") == 0)
            return ctx->ehs[i];
    }
    return ctx->eh;
}

static int shadow_had_user_diag(void) {
    return g_shadow_eh_diag;
}

/* Bind ctx->eh to the handler for `call`. 0 = unique-E / ambiguous refuse. */
static int shadow_eh_select(ShadowCtx* ctx, const char* call, AstNode** saved) {
    AstNode* matched;
    if (saved) *saved = ctx ? ctx->eh : NULL;
    if (!ctx) return 1;
    matched = shadow_eh_for_call(ctx, call);
    if (g_shadow_eh_diag) {
        ctx->eh = saved ? *saved : ctx->eh;
        return 0;
    }
    if (matched) ctx->eh = matched;
    return 1;
}

static void shadow_eh_push_node(ShadowCtx* ctx, AstNode* eh) {
    if (!ctx || !eh) return;
    if (ctx->nehs >= SHADOW_EH_STACK_CAP) {
        /* Fail loud — do not overwrite ctx->eh (that looked like a push). */
        shadow_table_full("errhandlers", SHADOW_EH_STACK_CAP,
                          eh->a[0] ? eh->a : NULL);
        return;
    }
    ctx->ehs_scope[ctx->nehs] = ctx->eh_scope;
    ctx->ehs[ctx->nehs++] = eh;
    ctx->eh = eh;
}

/* Emit-wide channel table (filled while walking stmts / items). */
static ShadowChanDecl g_shadow_chans[SHADOW_CHAN_CAP];
static int g_shadow_nchans;
/* Tx names that this TU feeds via send_task (ordered-task channel select). */
static char g_shadow_task_send[SHADOW_CHAN_CAP][64];
static int g_shadow_ntask_send;
/* Vec::[T] → factory CCVec_T; table also drives typedef hoist. */
enum { SHADOW_VEC_CAP = 64 };
static char g_shadow_vecs[SHADOW_VEC_CAP][64];
static int g_shadow_nvecs;
/* Map::[K,V] — compact "K_V" → Map_K_V + hash/eq table. */
enum { SHADOW_MAP_CAP = 64 };
static char g_shadow_maps[SHADOW_MAP_CAP][64];
static int g_shadow_nmaps;
/* ArrayMap::[K,V] — compact "K_V" → ArrayMap_K_V + CC_ARRAY_MAP_DECL. */
enum { SHADOW_AMAP_CAP = 64 };
static char g_shadow_amaps[SHADOW_AMAP_CAP][64];
static int g_shadow_namaps;
/* typedef Alias → ArrayMap_/Map_/CCVec_ base — resolve for UFCS binds. */
enum { SHADOW_TD_ALIAS_CAP = 256 };
static char g_shadow_td_alias[SHADOW_TD_ALIAS_CAP][64];
static char g_shadow_td_base[SHADOW_TD_ALIAS_CAP][96];
static int g_shadow_ntd;
/* Non-prelude slice elems queued for the CCSlice family factory (e.g. Pt).
 * Hoist + as: materialize still walk this table; emit prefers the factory. */
enum { SHADOW_SLICE_CAP = 64 };
static char g_shadow_slices[SHADOW_SLICE_CAP][64];
static int g_shadow_nslices;
/* `@variant Name` — arm names, packed layout, drop hooks. */
typedef struct {
    char name[64];
    char arms[8][64];
    char tys[8][96];
    int is_void[8];
    int narm;
    int is_packed;
    int donor_arm;
    unsigned packed_size;
    unsigned packed_align;
    unsigned niche_off;
    unsigned niche_width;
    unsigned long long niche_sentinel;
    int has_drop;
} ShadowVariant;
enum { SHADOW_VARIANT_CAP = 128 };
static ShadowVariant g_shadow_variants[SHADOW_VARIANT_CAP];
static int g_shadow_nvariants;
static int g_shadow_va_tmp_id;

static ShadowVariant* shadow_variant_for_recv(const char* recv);

static const char* shadow_destroy_hook_for(const char* ty);

static int shadow_is_c_keyword(const char* s) {
    static const char* kw[] = {
        "auto", "break", "case", "char", "const", "continue", "default", "do",
        "double", "else", "enum", "extern", "float", "for", "goto", "if",
        "inline", "int", "long", "register", "return", "short", "signed",
        "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned",
        "void", "volatile", "while", NULL
    };
    int i;
    if (!s || !s[0]) return 0;
    for (i = 0; kw[i]; i++) if (strcmp(s, kw[i]) == 0) return 1;
    return 0;
}

static int shadow_type_is_pointer(const char* ty) {
    return ty && strchr(ty, '*') != NULL;
}

static int shadow_mangle_ident_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Compact a `::[args]` interior: commas separate factory args; whitespace
 * between identifier tokens becomes `_` (`long long` → `long_long`).
 * `args[i]` keeps the C spelling (`long long`). */
static void shadow_compact_generic_args_text(const char* a, const char* r,
                                            char* compact, size_t ccap,
                                            char args[][64], int args_cap,
                                            int* nargs) {
    size_t ci = 0;
    size_t cj = 0;
    char cur[64];
    int nout = 0;
    compact[0] = 0;
    cur[0] = 0;
    if (nargs) *nargs = 0;
    while (a < r && ci + 1 < ccap) {
        if (*a == ' ' || *a == '\t') {
            while (a < r && (*a == ' ' || *a == '\t')) a++;
            if (a >= r) break;
            if (*a == ',') continue;
            if (ci && shadow_mangle_ident_char(compact[ci - 1]) &&
                shadow_mangle_ident_char(*a))
                compact[ci++] = '_';
            if (cj && cur[cj - 1] != ' ' && cj + 1 < sizeof(cur) &&
                shadow_mangle_ident_char(*a))
                cur[cj++] = ' ';
            continue;
        }
        if (*a == ',') {
            if (args && nout < args_cap && cj) {
                cur[cj] = 0;
                snprintf(args[nout], 64, "%s", cur);
                nout++;
            }
            if (ci && compact[ci - 1] != '_') compact[ci++] = '_';
            compact[ci] = 0;
            cj = 0;
            cur[0] = 0;
            a++;
            continue;
        }
        compact[ci++] = *a;
        if (cj + 1 < sizeof(cur)) cur[cj++] = *a;
        a++;
        compact[ci] = 0;
        cur[cj] = 0;
    }
    if (args && nout < args_cap && cj) {
        cur[cj] = 0;
        snprintf(args[nout], 64, "%s", cur);
        nout++;
    }
    if (nargs) *nargs = nout;
}

/* Lower leftover `Family::[args]` spellings in opaque expr text. */
static void shadow_rewrite_generic_types_text(char* text, size_t cap) {
    char out[4096];
    size_t o = 0;
    const char* p;
    int changed = 0;
    if (!text || !cap || !text[0]) return;
    p = text;
    while (*p && o + 1 < sizeof(out)) {
        /* Never rewrite inside "…" / '…' (printf "CCVec::[T]" must survive). */
        if (*p == '"' || *p == '\'') {
            char qch = *p;
            out[o++] = *p++;
            while (*p && o + 1 < sizeof(out)) {
                out[o++] = *p;
                if (*p == '\\' && p[1]) {
                    p++;
                    if (o + 1 < sizeof(out)) out[o++] = *p;
                    p++;
                    continue;
                }
                p++;
                if (p[-1] == qch) break;
            }
            continue;
        }
        if (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
             *p == '_') &&
            (p == text ||
             !((p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= 'a' && p[-1] <= 'z') ||
               (p[-1] >= '0' && p[-1] <= '9') || p[-1] == '_'))) {
            const char* name = p;
            size_t nl = 0;
            const char* q;
            while ((p[nl] >= 'A' && p[nl] <= 'Z') ||
                   (p[nl] >= 'a' && p[nl] <= 'z') ||
                   (p[nl] >= '0' && p[nl] <= '9') || p[nl] == '_')
                nl++;
            q = name + nl;
            while (*q == ' ' || *q == '\t') q++;
            if (q[0] == ':' && q[1] == ':' && q[2] == '[') {
                const char* br = q + 2;
                const char* r = br + 1;
                int depth = 1;
                char compact[96];
                size_t ci = 0;
                while (*r && depth > 0) {
                    if (*r == '[') depth++;
                    else if (*r == ']') {
                        depth--;
                        if (depth == 0) break;
                    }
                    r++;
                }
                if (depth == 0 && *r == ']') {
                    const char* a = br + 1;
                    while (a < r && ci + 1 < sizeof(compact)) {
                        if (*a == ',' ||
                            (a[0] == ':' && a[1] == ':' && a[2] == '[')) {
                            if (*a == ',') {
                                if (ci && compact[ci - 1] != '_')
                                    compact[ci++] = '_';
                                a++;
                                while (*a == ' ' || *a == '\t') a++;
                                continue;
                            }
                            /* Nested …Family::[…] — `a` is at `::[`; family
                             * letters were already copied into compact. */
                            {
                                const char* b2 = a + 2; /* '[' */
                                const char* r2;
                                int d2 = 1;
                                if (!(a[0] == ':' && a[1] == ':' &&
                                      a[2] == '['))
                                    break;
                                r2 = b2 + 1;
                                while (*r2 && d2 > 0) {
                                    if (*r2 == '[') d2++;
                                    else if (*r2 == ']') {
                                        d2--;
                                        if (d2 == 0) break;
                                    }
                                    r2++;
                                }
                                if (d2 != 0 || *r2 != ']') break;
                                /* Surface Vec:: → CCVec_ (match top-level). */
                                if (ci >= 3 &&
                                    memcmp(compact + ci - 3, "Vec", 3) == 0 &&
                                    (ci == 3 || compact[ci - 4] == '_') &&
                                    !(ci >= 5 &&
                                      memcmp(compact + ci - 5, "CCVec", 5) ==
                                          0)) {
                                    if (ci + 2 >= sizeof(compact)) break;
                                    memmove(compact + ci - 1, compact + ci - 3,
                                            3);
                                    compact[ci - 3] = 'C';
                                    compact[ci - 2] = 'C';
                                    ci += 2;
                                }
                                if (ci + 1 >= sizeof(compact)) break;
                                compact[ci++] = '_';
                                {
                                    const char* ia = b2 + 1;
                                    while (ia < r2 && ci + 1 < sizeof(compact)) {
                                        if (*ia == ' ' || *ia == '\t') {
                                            while (ia < r2 &&
                                                   (*ia == ' ' || *ia == '\t'))
                                                ia++;
                                            if (ia < r2 && *ia != ',' && ci &&
                                                shadow_mangle_ident_char(
                                                    compact[ci - 1]) &&
                                                shadow_mangle_ident_char(*ia))
                                                compact[ci++] = '_';
                                            continue;
                                        }
                                        if (*ia == ',') {
                                            if (ci && compact[ci - 1] != '_')
                                                compact[ci++] = '_';
                                            ia++;
                                            continue;
                                        }
                                        compact[ci++] = *ia++;
                                    }
                                }
                                a = r2 + 1;
                                continue;
                            }
                        }
                        if (*a == ' ' || *a == '\t') {
                            while (a < r && (*a == ' ' || *a == '\t')) a++;
                            if (a < r && *a != ',' &&
                                !(a[0] == ':' && a[1] == ':' && a[2] == '[') &&
                                ci && shadow_mangle_ident_char(compact[ci - 1]) &&
                                shadow_mangle_ident_char(*a))
                                compact[ci++] = '_';
                            continue;
                        }
                        compact[ci++] = *a++;
                    }
                    compact[ci] = 0;
                    if (compact[0]) {
                        char fam[96];
                        int is_type =
                            strncmp(name, "CCVec", 5) == 0 ||
                            strncmp(name, "Map", 3) == 0 ||
                            strncmp(name, "ArrayMap", 8) == 0 ||
                            strncmp(name, "Vec", 3) == 0 ||
                            (name[0] >= 'A' && name[0] <= 'Z');
                        int is_snake_fac = 0;
                        if (nl + 1 < sizeof(fam)) {
                            memcpy(fam, name, nl);
                            fam[nl] = 0;
                            is_snake_fac = cc_emit_plan_has_generic_factory(fam);
                        }
                        if (is_type || is_snake_fac) {
                            char spelled[160];
                            if (strncmp(name, "Map", 3) == 0 && nl == 3)
                                snprintf(spelled, sizeof(spelled), "Map_%s*",
                                         compact);
                            else if (strncmp(name, "ArrayMap", 8) == 0 &&
                                     nl == 8)
                                snprintf(spelled, sizeof(spelled),
                                         "ArrayMap_%s*", compact);
                            else if ((strncmp(name, "CCVec", 5) == 0 &&
                                      nl == 5) ||
                                     (strncmp(name, "Vec", 3) == 0 && nl == 3))
                                snprintf(spelled, sizeof(spelled), "CCVec_%s",
                                         compact);
                            else
                                snprintf(spelled, sizeof(spelled), "%.*s_%s",
                                         (int)nl, name, compact);
                            o += (size_t)snprintf(out + o, sizeof(out) - o, "%s",
                                                  spelled);
                            p = r + 1;
                            changed = 1;
                            continue;
                        }
                    }
                }
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    if (changed) snprintf(text, cap, "%s", out);
}

/* Fold `type_of(T).size|align|name|kind|nfields` like oracle D1.0/D1.1. */
static void shadow_lower_type_of_constexpr(char* text, size_t cap) {
    char out[4096];
    size_t o = 0;
    const char* p;
    int changed = 0;
    if (!text || !cap || !text[0]) return;
    p = text;
    while (*p && o + 1 < sizeof(out)) {
        if (strncmp(p, "type_of", 7) == 0 &&
            (p == text ||
             !((p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= 'a' && p[-1] <= 'z') ||
               (p[-1] >= '0' && p[-1] <= '9') || p[-1] == '_')) &&
            !(p[7] >= 'A' && p[7] <= 'Z') && !(p[7] >= 'a' && p[7] <= 'z') &&
            p[7] != '_' && !(p >= text + 3 && memcmp(p - 3, "cc_", 3) == 0)) {
            const char* q = p + 7;
            const char* id0;
            size_t idn = 0;
            const char* mem;
            size_t memn = 0;
            while (*q == ' ' || *q == '\t') q++;
            if (*q != '(') goto copy_ch;
            q++;
            while (*q == ' ' || *q == '\t') q++;
            id0 = q;
            while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') ||
                   (*q >= '0' && *q <= '9') || *q == '_')
                q++;
            idn = (size_t)(q - id0);
            if (!idn) goto copy_ch;
            while (*q == ' ' || *q == '\t') q++;
            if (*q != ')') goto copy_ch;
            q++;
            while (*q == ' ' || *q == '\t') q++;
            if (*q != '.') goto copy_ch;
            q++;
            while (*q == ' ' || *q == '\t') q++;
            mem = q;
            while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') ||
                   (*q >= '0' && *q <= '9') || *q == '_')
                q++;
            memn = (size_t)(q - mem);
            if (memn == 4 && memcmp(mem, "size", 4) == 0) {
                o += (size_t)snprintf(out + o, sizeof(out) - o,
                                      "((size_t)sizeof(%.*s))", (int)idn, id0);
            } else if (memn == 5 && memcmp(mem, "align", 5) == 0) {
                o += (size_t)snprintf(out + o, sizeof(out) - o,
                                      "((size_t)_Alignof(%.*s))", (int)idn, id0);
            } else if (memn == 4 && memcmp(mem, "name", 4) == 0) {
                o += (size_t)snprintf(out + o, sizeof(out) - o, "\"%.*s\"",
                                      (int)idn, id0);
            } else if (memn == 4 && memcmp(mem, "kind", 4) == 0) {
                int prim =
                    (idn == 3 && memcmp(id0, "int", 3) == 0) ||
                    (idn == 4 && memcmp(id0, "char", 4) == 0) ||
                    (idn == 5 && memcmp(id0, "short", 5) == 0) ||
                    (idn == 4 && memcmp(id0, "long", 4) == 0) ||
                    (idn == 5 && memcmp(id0, "float", 5) == 0) ||
                    (idn == 6 && memcmp(id0, "double", 6) == 0) ||
                    (idn == 6 && memcmp(id0, "size_t", 6) == 0) ||
                    (idn == 4 && memcmp(id0, "bool", 4) == 0);
                int cont = (idn >= 6 && memcmp(id0, "CCVec_", 6) == 0) ||
                           (idn >= 4 && memcmp(id0, "Map_", 4) == 0) ||
                           (idn >= 9 && memcmp(id0, "ArrayMap_", 9) == 0);
                if (prim)
                    o += (size_t)snprintf(out + o, sizeof(out) - o,
                                          "(CC_TK_PRIMITIVE)");
                else if (cont)
                    o += (size_t)snprintf(out + o, sizeof(out) - o,
                                          "(CC_TK_GENERIC_INST)");
                else
                    o += (size_t)snprintf(out + o, sizeof(out) - o,
                                          "(cc_type_of(\"%.*s\")->kind)",
                                          (int)idn, id0);
            } else if (memn == 7 && memcmp(mem, "nfields", 7) == 0) {
                int prim =
                    (idn == 3 && memcmp(id0, "int", 3) == 0) ||
                    (idn == 4 && memcmp(id0, "char", 4) == 0) ||
                    (idn == 5 && memcmp(id0, "short", 5) == 0) ||
                    (idn == 4 && memcmp(id0, "long", 4) == 0) ||
                    (idn == 5 && memcmp(id0, "float", 5) == 0) ||
                    (idn == 6 && memcmp(id0, "double", 6) == 0) ||
                    (idn == 6 && memcmp(id0, "size_t", 6) == 0) ||
                    (idn == 4 && memcmp(id0, "bool", 4) == 0);
                if (prim)
                    o += (size_t)snprintf(out + o, sizeof(out) - o,
                                          "((size_t)0)");
                else
                    o += (size_t)snprintf(
                        out + o, sizeof(out) - o,
                        "((size_t)cc_type_of(\"%.*s\")->nfields)", (int)idn,
                        id0);
            } else {
                goto copy_ch;
            }
            p = q;
            changed = 1;
            continue;
        }
    copy_ch:
        out[o++] = *p++;
    }
    out[o] = 0;
    if (changed) snprintf(text, cap, "%s", out);
}

/* Forward-declared niche table (defined below with other hook tables). */
typedef struct {
    char ty[64];
    unsigned size;
    unsigned align;
    unsigned off;
    unsigned width;
    unsigned long long sentinel;
} ShadowNicheInfo;
enum { SHADOW_NICHE_CAP = 128 };
static ShadowNicheInfo g_shadow_niches[SHADOW_NICHE_CAP];
static int g_shadow_nniches;

/* Host pointer width — packing must match the ABI that compiles emit.c
 * (ILP32 niches are 4 bytes; LP64 are 8). */
static unsigned shadow_ptr_bytes(void) {
    return (unsigned)sizeof(void*);
}

static unsigned shadow_variant_type_size(const char* ty) {
    int i;
    if (strcmp(ty, "CCString") == 0) return 16;
    if (strcmp(ty, "int64_t") == 0) return 8;
    if (strcmp(ty, "int") == 0) return 4;
    if (strcmp(ty, "Payload") == 0) return 8;
    if (strcmp(ty, "Handle") == 0) return 16;
    if (shadow_type_is_pointer(ty)) return shadow_ptr_bytes();
    for (i = 0; i < g_shadow_nniches; i++) {
        if (strcmp(g_shadow_niches[i].ty, ty) == 0 && g_shadow_niches[i].size)
            return g_shadow_niches[i].size;
    }
    return 0;
}

static unsigned shadow_variant_type_align(const char* ty) {
    unsigned sz;
    if (!ty || !ty[0]) return 1;
    if (shadow_type_is_pointer(ty)) return shadow_ptr_bytes();
    sz = shadow_variant_type_size(ty);
    if (!sz) return 1;
    /* Natural align for the known scalars / CCString SSO block. */
    if (sz >= 8) return 8;
    return sz;
}

static int shadow_variant_type_niche(const char* ty, unsigned* off, unsigned* width,
                                    unsigned long long* sentinel) {
    int i;
    if (!ty || !off || !width || !sentinel) return 0;
    if (strcmp(ty, "CCString") == 0) {
        *off = 12; *width = 4; *sentinel = 0xFFFFFFFFULL; return 1;
    }
    if (shadow_type_is_pointer(ty)) {
        *off = 0; *width = shadow_ptr_bytes(); *sentinel = 0; return 1;
    }
    for (i = 0; i < g_shadow_nniches; i++) {
        if (strcmp(g_shadow_niches[i].ty, ty) == 0) {
            *off = g_shadow_niches[i].off;
            *width = g_shadow_niches[i].width;
            *sentinel = g_shadow_niches[i].sentinel;
            return 1;
        }
    }
    return 0;
}

static void shadow_variant_compute_packed(ShadowVariant* v) {
    int a, d, o;
    unsigned max_size = 0, max_align = 1;
    if (!v || !v->is_packed) return;
    v->donor_arm = -1;
    for (a = 0; a < v->narm; a++) {
        unsigned sz = shadow_variant_type_size(v->tys[a]);
        unsigned al = shadow_variant_type_align(v->tys[a]);
        if (sz > max_size) max_size = sz;
        if (al > max_align) max_align = al;
    }
    if (max_size == 0) max_size = 1;
    if (v->narm == 2) {
        for (d = 0; d < 2; d++) {
            unsigned off, width;
            unsigned long long sentinel;
            o = 1 - d;
            if (v->is_void[o] && shadow_variant_type_niche(v->tys[d], &off, &width, &sentinel)) {
                v->donor_arm = d; v->niche_off = off; v->niche_width = width;
                v->niche_sentinel = sentinel; break;
            } else if (shadow_variant_type_niche(v->tys[d], &off, &width, &sentinel)) {
                if (shadow_variant_type_size(v->tys[o]) <= off) {
                    v->donor_arm = d; v->niche_off = off; v->niche_width = width;
                    v->niche_sentinel = sentinel; break;
                }
            }
        }
    }
    v->packed_size = max_size;
    v->packed_align = max_align;
}

static void shadow_variant_update_drop(ShadowVariant* v) {
    int a;
    if (!v) return;
    v->has_drop = 0;
    for (a = 0; a < v->narm; a++) {
        if (!v->is_void[a] && shadow_destroy_hook_for(v->tys[a])) {
            v->has_drop = 1; return;
        }
    }
}

static void shadow_variant_register_full(const char* name, char arms[][64],
                                         char tys[][96], const int* is_void,
                                         int narm, int is_packed) {
    int i, a;
    ShadowVariant* v;
    if (!name || !name[0] || narm <= 0) return;
    if (g_shadow_nvariants >= SHADOW_VARIANT_CAP) {
        shadow_table_full("variants", SHADOW_VARIANT_CAP, name);
        return;
    }
    for (i = 0; i < g_shadow_nvariants; i++) {
        if (strcmp(g_shadow_variants[i].name, name) == 0) {
            v = &g_shadow_variants[i]; goto upd;
        }
    }
    v = &g_shadow_variants[g_shadow_nvariants++];
    memset(v, 0, sizeof(*v));
    snprintf(v->name, sizeof(v->name), "%s", name);
upd:
    v->narm = narm > 8 ? 8 : narm;
    v->is_packed = is_packed ? 1 : 0;
    for (a = 0; a < v->narm; a++) {
        snprintf(v->arms[a], sizeof(v->arms[0]), "%s", arms[a]);
        if (tys && tys[a][0]) snprintf(v->tys[a], sizeof(v->tys[0]), "%s", tys[a]);
        v->is_void[a] = is_void && is_void[a] ? 1 : 0;
    }
    if (v->is_packed) shadow_variant_compute_packed(v);
    shadow_variant_update_drop(v);
}

static void __attribute__((unused)) shadow_variant_register(const char* name, char arms[][64],
                                    const int* is_void, int narm) {
    char tys[8][96];
    memset(tys, 0, sizeof(tys));
    shadow_variant_register_full(name, arms, tys, is_void, narm, 0);
}

static ShadowVariant* shadow_variant_find(const char* name) {
    char base[64];
    const char* p = name;
    size_t n = 0;
    int i;
    if (!name) return NULL;
    /* Binds are often `Name*` / `const Name*`; variants are registered bare. */
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "const ", 6) == 0) p += 6;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "struct ", 7) == 0) p += 7;
    while (*p && *p != '*' && *p != ' ' && *p != '\t' && n + 1 < sizeof(base))
        base[n++] = *p++;
    base[n] = 0;
    if (!base[0]) return NULL;
    for (i = 0; i < g_shadow_nvariants; i++)
        if (strcmp(g_shadow_variants[i].name, base) == 0)
            return &g_shadow_variants[i];
    return NULL;
}

static ShadowVariant* shadow_variant_find_by_arm(const char* arm) {
    int i, a, hits = 0;
    ShadowVariant* found = NULL;
    if (!arm || !arm[0]) return NULL;
    for (i = 0; i < g_shadow_nvariants; i++) {
        for (a = 0; a < g_shadow_variants[i].narm; a++) {
            if (strcmp(g_shadow_variants[i].arms[a], arm) == 0) {
                found = &g_shadow_variants[i];
                hits++;
            }
        }
    }
    return hits == 1 ? found : NULL;
}

static int shadow_variant_arm_match(ShadowVariant* hint, const char* expr,
                                    const char* site, const char* arm,
                                    size_t alen, ShadowVariant** out_v,
                                    int* out_a);

static void shadow_variant_track_drop(ShadowCtx* ctx, const char* name,
                                      const char* vty) {
    ShadowVariant* v;
    if (!ctx || !name || !name[0] || !vty || !vty[0]) return;
    v = shadow_variant_find(vty);
    if (!v) return;
    shadow_variant_update_drop(v);
    if (!v->has_drop) return;
    if (ctx->nvdrops >= (int)(sizeof(ctx->vdrops) / sizeof(ctx->vdrops[0]))) {
        shadow_table_full("vdrops", (int)(sizeof(ctx->vdrops) / sizeof(ctx->vdrops[0])),
                          name);
        return;
    }
    snprintf(ctx->vdrops[ctx->nvdrops].name, sizeof(ctx->vdrops[0].name),
             "%s", name);
    snprintf(ctx->vdrops[ctx->nvdrops].ty, sizeof(ctx->vdrops[0].ty),
             "%s", vty);
    ctx->nvdrops++;
}

static int shadow_emit_variant_block_drops(CEmit* out, ShadowCtx* ctx,
                                           const char* indent, int from,
                                           int to) {
    int i;
    if (!out || !ctx || from >= to) return 1;
    for (i = to - 1; i >= from; i--) {
        ShadowVariant* v = shadow_variant_find(ctx->vdrops[i].ty);
        if (!v) continue;
        shadow_variant_update_drop(v);
        if (!v->has_drop) continue;
        if (!cemit_fmt(out, "%s%s__cc_drop(&(%s));\n", indent, v->name,
                       ctx->vdrops[i].name))
            return 0;
    }
    return 1;
}

static void shadow_vdrop_mark_push(ShadowCtx* ctx) {
    if (!ctx) return;
    if (ctx->nvdrop_marks >=
        (int)(sizeof(ctx->vdrop_marks) / sizeof(ctx->vdrop_marks[0]))) {
        shadow_table_full("vdrop_marks",
                          (int)(sizeof(ctx->vdrop_marks) / sizeof(ctx->vdrop_marks[0])),
                          NULL);
        return;
    }
    ctx->vdrop_marks[ctx->nvdrop_marks++] = ctx->nvdrops;
}

/* Emit scope-exit drops for the current mark and pop it. */
static int shadow_vdrop_mark_pop_emit(CEmit* out, ShadowCtx* ctx,
                                     const char* indent) {
    int from;
    if (!out || !ctx || ctx->nvdrop_marks <= 0) return 1;
    from = ctx->vdrop_marks[--ctx->nvdrop_marks];
    if (!shadow_emit_variant_block_drops(out, ctx, indent, from, ctx->nvdrops))
        return 0;
    ctx->nvdrops = from;
    return 1;
}

/* `v.arm ?> fb` → if/else statement-expression (spec §5a).
 * Only the projection primary immediately left of `?>` is the LHS — not the
 * whole prefix — so `a || (c.num ?> 0)` keeps `a || (` intact. */
static int shadow_rewrite_variant_qmark(char* expr, size_t cap) {
    const char* q;
    char base[128], arm[64], fb[1536], piece[2048], out[3072];
    char rname[64];
    ShadowVariant* v;
    int is_arrow = 0, a, id, ls, le, rs, re, acc_pos;
    const char* acc;
    size_t bl;
    if (!expr || !cap) return 0;
    q = shadow_find_sigil_live(expr, '?');
    if (!q) return 0;
    le = (int)(q - expr);
    ls = shadow_variant_proj_lhs_start(expr, le, arm, sizeof(arm), &is_arrow,
                                      &acc_pos);
    if (ls < 0 || acc_pos <= ls) return 0;
    bl = (size_t)(acc_pos - ls);
    if (bl == 0 || bl >= sizeof(base)) return 0;
    memcpy(base, expr + ls, bl);
    base[bl] = 0;
    while (bl > 0 && (base[bl - 1] == ' ' || base[bl - 1] == '\t'))
        base[--bl] = 0;
    if (!bl) return 0;
    v = shadow_variant_for_recv(base);
    if (!v &&
        shadow_recv_name_before(expr, expr + acc_pos, rname, sizeof(rname)))
        v = shadow_variant_for_recv(rname);
    if (!v) v = shadow_variant_find_by_arm(arm);
    if (!v) return 0;
    for (a = 0; a < v->narm; a++)
        if (strcmp(v->arms[a], arm) == 0) break;
    if (a >= v->narm || v->is_void[a]) return 0;
    /* Fallback: tight operand, stop at ops / closers outside groups. */
    rs = le + 2;
    while (expr[rs] == ' ' || expr[rs] == '\t') rs++;
    {
        int depth = 0;
        re = rs;
        while (expr[re]) {
            char c = expr[re];
            if (c == '(' || c == '[' || c == '{') depth++;
            else if (c == ')' || c == ']' || c == '}') {
                if (depth == 0) break;
                depth--;
            } else if (depth == 0 && (c == ',' || c == ';')) break;
            else if (depth == 0 &&
                     ((c == '&' && expr[re + 1] == '&') ||
                      (c == '|' && expr[re + 1] == '|') ||
                      (c == '=' && expr[re + 1] == '=') ||
                      (c == '!' && expr[re + 1] == '=') ||
                      (c == '<' && (expr[re + 1] == '=' || expr[re + 1] != '<')) ||
                      (c == '>' && (expr[re + 1] == '=' || expr[re + 1] != '>')) ||
                      c == '?' || c == ':' || c == '+' || c == '-' || c == '*' ||
                      c == '/' || c == '%' || c == '^')) {
                if (re > rs) break;
            }
            re++;
        }
    }
    if (re <= rs || (size_t)(re - rs) >= sizeof(fb)) return 0;
    memcpy(fb, expr + rs, (size_t)(re - rs));
    fb[re - rs] = 0;
    acc = is_arrow ? "->" : ".";
    id = ++g_shadow_va_tmp_id;
    /* Packed: emit getters directly — never insert `.u.` for later materialize. */
    if (v->is_packed) {
        if (is_arrow) {
            if (snprintf(piece, sizeof(piece),
                         "({ __typeof__(%s__cc_get_%s(%s)) __cc_pj%d; "
                         "if (%s__cc_kind(%s) == %s_%s) { __cc_pj%d = "
                         "%s__cc_get_%s(%s); } else { __cc_pj%d = (%s); } "
                         "__cc_pj%d; })",
                         v->name, arm, base, id, v->name, base, v->name, arm,
                         id, v->name, arm, base, id, fb, id) >= (int)sizeof(piece))
                return 0;
        } else {
            if (snprintf(piece, sizeof(piece),
                         "({ __typeof__(%s__cc_get_%s(&(%s))) __cc_pj%d; "
                         "if (%s__cc_kind(&(%s)) == %s_%s) { __cc_pj%d = "
                         "%s__cc_get_%s(&(%s)); } else { __cc_pj%d = (%s); } "
                         "__cc_pj%d; })",
                         v->name, arm, base, id, v->name, base, v->name, arm,
                         id, v->name, arm, base, id, fb, id) >= (int)sizeof(piece))
                return 0;
        }
    } else {
        if (snprintf(piece, sizeof(piece),
                     "({ __typeof__((%s)%su.%s) __cc_pj%d; "
                     "if ((%s)%skind == %s_%s) { __cc_pj%d = (%s)%su.%s; } "
                     "else { __cc_pj%d = (%s); } __cc_pj%d; })",
                     base, acc, arm, id, base, acc, v->name, arm, id, base,
                     acc, arm, id, fb, id) >= (int)sizeof(piece))
            return 0;
    }
    if (snprintf(out, sizeof(out), "%.*s%s%s", ls, expr, piece,
                 expr + re) >= (int)sizeof(out))
        return 0;
    if (strlen(out) >= cap) return 0;
    snprintf(expr, cap, "%s", out);
    return 1;
}

/* Resolve surface `recv.arm` / `recv->arm` to a registered variant projection.
 * Fills base/arm/is_arrow. Returns the variant, or NULL. */
static ShadowVariant* shadow_variant_parse_proj(const char* expr, char* base,
                                               size_t bcap, char* arm,
                                               size_t acap, int* is_arrow) {
    int ls, acc_pos, arrow = 0;
    size_t bl;
    char rname[64];
    ShadowVariant* v;
    int a;
    if (!expr || !base || !bcap || !arm || !acap || !is_arrow) return NULL;
    ls = shadow_variant_proj_lhs_start(expr, (int)strlen(expr), arm, acap,
                                      &arrow, &acc_pos);
    if (ls < 0 || acc_pos <= ls) return NULL;
    /* Peel already-lowered `.u.arm` / `->u.arm` back to the real receiver. */
    if (!arrow && acc_pos >= 2 && expr[acc_pos - 1] == 'u') {
        int uacc = -1;
        if (expr[acc_pos - 2] == '.') {
            uacc = acc_pos - 2;
            arrow = 0;
        } else if (acc_pos >= 3 && expr[acc_pos - 2] == '>' &&
                   expr[acc_pos - 3] == '-') {
            uacc = acc_pos - 3;
            arrow = 1;
        }
        if (uacc >= 0) {
            ls = shadow_proj_recv_start(expr, uacc);
            if (ls < 0) return NULL;
            acc_pos = uacc;
        }
    }
    bl = (size_t)(acc_pos - ls);
    if (bl == 0 || bl >= bcap) return NULL;
    memcpy(base, expr + ls, bl);
    base[bl] = 0;
    while (bl > 0 && (base[bl - 1] == ' ' || base[bl - 1] == '\t'))
        base[--bl] = 0;
    if (!bl) return NULL;
    v = shadow_variant_for_recv(base);
    if (!v &&
        shadow_recv_name_before(expr, expr + acc_pos, rname, sizeof(rname)))
        v = shadow_variant_for_recv(rname);
    /* Require a typed receiver. Global arm-name lookup would rewrite plain
     * struct fields (e.g. `r.data`) whenever any variant has arm `data`
     * (RESP grammar), which then fails host C. */
    if (!v) return NULL;
    for (a = 0; a < v->narm; a++)
        if (strcmp(v->arms[a], arm) == 0) break;
    if (a >= v->narm || v->is_void[a]) return NULL;
    *is_arrow = arrow;
    return v;
}

/* Walk left over a receiver. Include `(expr)` / `[i]` groups, but stop before
 * a cast `(T)recv` so `(int64_t)c.arm` yields `c` (not `(int64_t)c`). */
static const char* shadow_variant_recv_lbound(const char* expr, const char* from) {
    const char* rs = from;
    if (!expr || !from || from < expr) return from;
    while (rs > expr) {
        char c = rs[-1];
        if (shadow_is_id((unsigned char)c) || c == '_') {
            rs--;
            continue;
        }
        /* `)` / `]` only when not a cast: `(T)id` has an id right after `)`. */
        if ((c == ')' || c == ']') && !shadow_is_id((unsigned char)*rs)) {
            rs--;
            continue;
        }
        break;
    }
    return rs;
}

/* Must cover AST_SWITCH opaque bodies (pp_emit_stmt body[8192]). Truncation
 * used to close braces mid-case and host-C then reported nested functions. */
enum { SHADOW_VARIANT_REWRITE_BUF = 8192 };

static void shadow_variant_packed_materialize(char* expr, size_t cap,
                                              ShadowVariant* v) {
    char out[SHADOW_VARIANT_REWRITE_BUF];
    const char* p;
    char* o;
    size_t rem;
    int a;
    if (!expr || !cap || !v || !v->is_packed) return;
    p = expr;
    o = out;
    rem = sizeof(out) - 1;
    out[0] = 0;
    while (*p && rem > 0) {
        if (*p == '.' && p > expr &&
            (shadow_is_id(p[-1]) || p[-1] == ')' || p[-1] == ']')) {
            const char* mem = p + 1;
            const char* e = mem;
            const char* rs = shadow_variant_recv_lbound(expr, p);
            size_t rlen = (size_t)(p - rs);
            while (*e && ((*e >= 'a' && *e <= 'z') || (*e >= 'A' && *e <= 'Z') ||
                          (*e >= '0' && *e <= '9') || *e == '_'))
                e++;
            if ((size_t)(e - mem) == 4 && memcmp(mem, "kind", 4) == 0) {
                if (rlen > 0 && (size_t)(o - out) >= rlen &&
                    memcmp(o - rlen, rs, rlen) == 0) {
                    o -= rlen;
                    rem += rlen;
                }
                {
                    int n = snprintf(o, rem, "%s__cc_kind(&(%.*s))", v->name,
                                     (int)rlen, rs);
                    if (n < 0 || (size_t)n >= rem) { rem = 0; break; }
                    o += n; rem -= (size_t)n; p = e; continue;
                }
            }
            /* `.u.arm` — receiver is before `.u`, not the `u` token itself. */
            if (p >= expr + 2 && p[-1] == 'u' && p[-2] == '.') {
                const char* arm = mem;
                const char* u_dot = p - 2;
                const char* brs = shadow_variant_recv_lbound(expr, u_dot);
                size_t brlen = (size_t)(u_dot - brs);
                int matched = 0;
                for (a = 0; a < v->narm; a++) {
                    size_t al = strlen(v->arms[a]);
                    if ((size_t)(e - arm) == al &&
                        memcmp(arm, v->arms[a], al) == 0) {
                        /* Drop already-copied `recv.u` before emitting overlay. */
                        if (brlen > 0 && (size_t)(o - out) >= brlen + 2 &&
                            o[-1] == 'u' && o[-2] == '.' &&
                            memcmp(o - (brlen + 2), brs, brlen) == 0) {
                            o -= brlen + 2;
                            rem += brlen + 2;
                        } else if ((size_t)(o - out) >= 2 && o[-1] == 'u' &&
                                   o[-2] == '.') {
                            o -= 2;
                            rem += 2;
                        }
                        {
                            int n = snprintf(o, rem,
                                             "((%s__cc_ov_%s*)&(%.*s))->%s",
                                             v->name, v->arms[a], (int)brlen,
                                             brs, v->arms[a]);
                            if (n < 0 || (size_t)n >= rem) { rem = 0; break; }
                            o += n; rem -= (size_t)n; p = e; matched = 1;
                        }
                        break;
                    }
                }
                if (matched) continue;
                /* Not a variant arm — fall through and copy `.` literally. */
            }
            for (a = 0; a < v->narm; a++) {
                size_t al = strlen(v->arms[a]);
                if ((size_t)(e - mem) == al && memcmp(mem, v->arms[a], al) == 0) {
                    if (rlen > 0 && (size_t)(o - out) >= rlen &&
                        memcmp(o - rlen, rs, rlen) == 0) {
                        o -= rlen;
                        rem += rlen;
                    }
                    {
                        int n = snprintf(o, rem,
                                         "((%s__cc_ov_%s*)&(%.*s))->%s",
                                         v->name, v->arms[a], (int)rlen, rs,
                                         v->arms[a]);
                        if (n < 0 || (size_t)n >= rem) { rem = 0; break; }
                        o += n; rem -= (size_t)n; p = e; continue;
                    }
                }
            }
        }
        if (p[0] == '-' && p[1] == '>' && p > expr &&
            (shadow_is_id(p[-1]) || p[-1] == ')' || p[-1] == ']')) {
            const char* mem = p + 2;
            const char* e = mem;
            const char* rs = shadow_variant_recv_lbound(expr, p);
            size_t rlen = (size_t)(p - rs);
            while (*e && ((*e >= 'a' && *e <= 'z') || (*e >= 'A' && *e <= 'Z') ||
                          (*e >= '0' && *e <= '9') || *e == '_'))
                e++;
            if ((size_t)(e - mem) == 4 && memcmp(mem, "kind", 4) == 0) {
                if (rlen > 0 && (size_t)(o - out) >= rlen &&
                    memcmp(o - rlen, rs, rlen) == 0) {
                    o -= rlen;
                    rem += rlen;
                }
                {
                    int n = snprintf(o, rem, "%s__cc_kind(%.*s)", v->name,
                                     (int)rlen, rs);
                    if (n < 0 || (size_t)n >= rem) { rem = 0; break; }
                    o += n; rem -= (size_t)n; p = e; continue;
                }
            }
            if (p[2] == 'u' && p[3] == '.') {
                const char* arm = p + 4;
                int matched = 0;
                e = arm;
                while (*e && ((*e >= 'a' && *e <= 'z') || (*e >= 'A' && *e <= 'Z') ||
                              (*e >= '0' && *e <= '9') || *e == '_'))
                    e++;
                for (a = 0; a < v->narm; a++) {
                    size_t al = strlen(v->arms[a]);
                    if ((size_t)(e - arm) == al &&
                        memcmp(arm, v->arms[a], al) == 0) {
                        if (rlen > 0 && (size_t)(o - out) >= rlen &&
                            memcmp(o - rlen, rs, rlen) == 0) {
                            o -= rlen;
                            rem += rlen;
                        }
                        {
                            int n = snprintf(o, rem,
                                             "((%s__cc_ov_%s*)%.*s)->%s",
                                             v->name, v->arms[a], (int)rlen, rs,
                                             v->arms[a]);
                            if (n < 0 || (size_t)n >= rem) { rem = 0; break; }
                            o += n; rem -= (size_t)n; p = e; matched = 1;
                        }
                        break;
                    }
                }
                if (matched) continue;
            }
            for (a = 0; a < v->narm; a++) {
                size_t al = strlen(v->arms[a]);
                if ((size_t)(e - mem) == al && memcmp(mem, v->arms[a], al) == 0) {
                    if (rlen > 0 && (size_t)(o - out) >= rlen &&
                        memcmp(o - rlen, rs, rlen) == 0) {
                        o -= rlen;
                        rem += rlen;
                    }
                    {
                        int n = snprintf(o, rem,
                                         "((%s__cc_ov_%s*)%.*s)->%s",
                                         v->name, v->arms[a], (int)rlen, rs,
                                         v->arms[a]);
                        if (n < 0 || (size_t)n >= rem) { rem = 0; break; }
                        o += n; rem -= (size_t)n; p = e; continue;
                    }
                }
            }
        }
        *o++ = *p++;
        rem--;
    }
    *o = 0;
    if (strlen(out) < cap) snprintf(expr, cap, "%s", out);
}

/* Rewrite variant designators / arm projections in leftover expr text.
 * `{ .num = 42 }` → `{ .kind = Name_num, .u.num = 42 }` when Name known;
 * `.kind == .num` → `.kind == Name_num`; `v.num` → `v.u.num` for arms. */
static void shadow_rewrite_variant_expr(char* expr, size_t cap, const char* vty) {
    ShadowVariant* v;
    char out[SHADOW_VARIANT_REWRITE_BUF];
    const char* p;
    char* o;
    size_t rem;
    int a;
    if (!expr || !cap || !g_shadow_nvariants) return;
    v = vty ? shadow_variant_find(vty) : NULL;
    if (!v && expr[0]) {
        const char* dot = strchr(expr, '.');
        const char* arr = strstr(expr, "->");
        const char* acc = dot;
        char recv[64];
        if (arr && (!dot || arr < dot)) acc = arr;
        if (acc && acc > expr &&
            shadow_recv_name_before(expr, acc, recv, sizeof(recv)))
            v = shadow_variant_for_recv(recv);
    }
    if (!v && g_shadow_nvariants == 1) v = &g_shadow_variants[0];
    p = expr;
    o = out;
    rem = sizeof(out) - 1;
    out[0] = 0;
    while (*p && rem > 0) {
        /* `.kind == .arm` / `== .arm` */
        if (p[0] == '=' && p[1] == '=' && (p[2] == ' ' || p[2] == '.')) {
            const char* q = p + 2;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '.') {
                const char* arm = q + 1;
                const char* e = arm;
                ShadowVariant* mv;
                int ma;
                while (*e && ((*e >= 'a' && *e <= 'z') || (*e >= 'A' && *e <= 'Z') ||
                              (*e >= '0' && *e <= '9') || *e == '_'))
                    e++;
                if (shadow_variant_arm_match(v, expr, q, arm, (size_t)(e - arm),
                                             &mv, &ma)) {
                    int n = snprintf(o, rem, "== %s_%s", mv->name, mv->arms[ma]);
                    if (n < 0 || (size_t)n >= rem) { rem = 0; break; }
                    o += n;
                    rem -= (size_t)n;
                    p = e;
                    goto cont;
                }
            }
        }
        /* `.arm ==` / `.arm !=` (bare designator on the left — not `v.arm`). */
        if (*p == '.' && ((p[1] >= 'a' && p[1] <= 'z') ||
                          (p[1] >= 'A' && p[1] <= 'Z') || p[1] == '_') &&
            (p == expr ||
             !(shadow_is_id(p[-1]) || p[-1] == ')' || p[-1] == ']'))) {
            const char* arm = p + 1;
            const char* e = arm;
            const char* op;
            ShadowVariant* mv;
            int ma;
            while (*e && ((*e >= 'a' && *e <= 'z') || (*e >= 'A' && *e <= 'Z') ||
                          (*e >= '0' && *e <= '9') || *e == '_'))
                e++;
            op = e;
            while (*op == ' ' || *op == '\t') op++;
            if ((op[0] == '=' && op[1] == '=') ||
                (op[0] == '!' && op[1] == '=')) {
                if (shadow_variant_arm_match(v, expr, p, arm, (size_t)(e - arm),
                                             &mv, &ma)) {
                    int n = snprintf(o, rem, "%s_%s", mv->name, mv->arms[ma]);
                    if (n < 0 || (size_t)n >= rem) { rem = 0; break; }
                    o += n;
                    rem -= (size_t)n;
                    p = e;
                    goto cont;
                }
            }
        }
        /* Designated `.arm =` after `{` / `,` → `.kind = Name_arm, .u.arm =`
         * (void arms: tag only; consume trailing `{}`). Skip `.u.arm =`. */
        if (*p == '.' && ((p[1] >= 'a' && p[1] <= 'z') ||
                          (p[1] >= 'A' && p[1] <= 'Z') || p[1] == '_')) {
            const char* arm = p + 1;
            const char* e = arm;
            const char* eq;
            const char* pre;
            while (*e && ((*e >= 'a' && *e <= 'z') || (*e >= 'A' && *e <= 'Z') ||
                          (*e >= '0' && *e <= '9') || *e == '_'))
                e++;
            eq = e;
            while (*eq == ' ' || *eq == '\t' || *eq == '\n' || *eq == '\r')
                eq++;
            pre = p;
            while (pre > expr &&
                   (pre[-1] == ' ' || pre[-1] == '\t' || pre[-1] == '\n' ||
                    pre[-1] == '\r'))
                pre--;
            if (*eq == '=' && pre > expr &&
                (pre[-1] == '{' || pre[-1] == ',')) {
                /* Nested payload `{ .len=, .data= }` inside `.arm = {…}` /
                 * `.u.arm = {…}` must not re-tag field names that collide with
                 * other variant arms (RESP `.data`). Source still has `.bulk =`
                 * before rewrite inserts `.u.`. */
                const char* q = pre;
                int under_u = 0;
                int depth = 0;
                while (q > expr) {
                    q--;
                    if (*q == '}') {
                        depth++;
                        continue;
                    }
                    if (*q == '{') {
                        if (depth > 0) {
                            depth--;
                            continue;
                        }
                        /* Opening brace of this designator group — look left
                         * for `.u.ident =` or `.arm =` (known variant arm). */
                        const char* r = q;
                        while (r > expr &&
                               (r[-1] == ' ' || r[-1] == '\t' ||
                                r[-1] == '\n' || r[-1] == '\r'))
                            r--;
                        if (r > expr && r[-1] == '=') {
                            const char* id_end;
                            const char* id;
                            /* Step onto `=`, then left onto the last char of
                             * the designator ident (skip spaces around `=`). */
                            r--;
                            if (r > expr) r--;
                            while (r > expr &&
                                   (*r == ' ' || *r == '\t' || *r == '\n' ||
                                    *r == '\r'))
                                r--;
                            /* r is on the last char of the designator ident. */
                            id_end = r + 1; /* exclusive */
                            while (r > expr &&
                                   ((r[-1] >= 'a' && r[-1] <= 'z') ||
                                    (r[-1] >= 'A' && r[-1] <= 'Z') ||
                                    (r[-1] >= '0' && r[-1] <= '9') ||
                                    r[-1] == '_'))
                                r--;
                            id = r; /* first char of designator ident */
                            if (id > expr && id[-1] == '.' && id_end > id) {
                                /* Nested `{ .field = }` under any `.ident = {`
                                 * is a payload struct — never re-tag fields
                                 * that collide with other variant arms
                                 * (RESP `.bulk = { .len, .data }`). */
                                under_u = 1;
                            }
                        }
                        break;
                    }
                }
                ShadowVariant* mv;
                int ma;
                if (!under_u &&
                    shadow_variant_arm_match(v, expr, p, arm, (size_t)(e - arm),
                                             &mv, &ma)) {
                    int n;
                    const char* rest = eq + 1;
                    if (mv->is_packed) {
                        if (mv->is_void[ma]) {
                            n = snprintf(o, rem, "%s__cc_set_%s()",
                                         mv->name, mv->arms[ma]);
                            while (*rest == ' ' || *rest == '\t') rest++;
                            if (*rest == '{') {
                                int dep = 0;
                                do {
                                    if (*rest == '{') dep++;
                                    else if (*rest == '}') dep--;
                                    rest++;
                                } while (*rest && dep > 0);
                            }
                        } else {
                            n = snprintf(o, rem, "%s__cc_set_%s(",
                                         mv->name, mv->arms[ma]);
                        }
                        if (n < 0 || (size_t)n >= rem) { rem = 0; break; }
                        o += n;
                        rem -= (size_t)n;
                        p = mv->is_void[ma] ? rest : (eq + 1);
                        goto cont;
                    }
                    if (mv->is_void[ma]) {
                        n = snprintf(o, rem, ".kind = %s_%s", mv->name,
                                     mv->arms[ma]);
                        while (*rest == ' ' || *rest == '\t') rest++;
                        if (*rest == '{') {
                            int dep = 0;
                            do {
                                if (*rest == '{') dep++;
                                else if (*rest == '}') dep--;
                                rest++;
                            } while (*rest && dep > 0);
                        }
                    } else {
                        n = snprintf(o, rem, ".kind = %s_%s, .u.%s =",
                                     mv->name, mv->arms[ma], mv->arms[ma]);
                    }
                    if (n < 0 || (size_t)n >= rem) { rem = 0; break; }
                    o += n;
                    rem -= (size_t)n;
                    p = mv->is_void[ma] ? rest : (eq + 1);
                    goto cont;
                }
            }
        }
        /* `case .arm` → `case Name_arm` (allow `case . arm` from spell-join). */
        if (strncmp(p, "case", 4) == 0 && !shadow_is_id(p[4] ? p[4] : 0) &&
            (p == expr || !shadow_is_id(p[-1]))) {
            const char* q = p + 4;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '.') {
                const char* arm = q + 1;
                const char* e;
                ShadowVariant* mv;
                int ma;
                while (*arm == ' ' || *arm == '\t') arm++;
                e = arm;
                while (*e && ((*e >= 'a' && *e <= 'z') || (*e >= 'A' && *e <= 'Z') ||
                              (*e >= '0' && *e <= '9') || *e == '_'))
                    e++;
                if (e > arm &&
                    shadow_variant_arm_match(v, expr, q, arm, (size_t)(e - arm),
                                             &mv, &ma)) {
                    int n = snprintf(o, rem, "case %s_%s", mv->name, mv->arms[ma]);
                    if (n < 0 || (size_t)n >= rem) { rem = 0; break; }
                    o += n;
                    rem -= (size_t)n;
                    p = e;
                    goto cont;
                }
            }
        }
        /* `recv->arm` → `recv->u.arm` */
        if (p[0] == '-' && p[1] == '>' &&
            ((p[2] >= 'a' && p[2] <= 'z') || (p[2] >= 'A' && p[2] <= 'Z') ||
             p[2] == '_')) {
            const char* arm = p + 2;
            const char* e = arm;
            ShadowVariant* mv;
            int ma;
            while (*e && ((*e >= 'a' && *e <= 'z') || (*e >= 'A' && *e <= 'Z') ||
                          (*e >= '0' && *e <= '9') || *e == '_'))
                e++;
            if (!(e - arm == 4 && memcmp(arm, "kind", 4) == 0) &&
                !(e - arm == 1 && arm[0] == 'u')) {
                if (shadow_variant_arm_match(v, expr, p, arm, (size_t)(e - arm),
                                             &mv, &ma) &&
                    !mv->is_packed) {
                    int n = snprintf(o, rem, "->u.%s", mv->arms[ma]);
                    if (n < 0 || (size_t)n >= rem) { rem = 0; break; }
                    o += n;
                    rem -= (size_t)n;
                    p = e;
                    goto cont;
                }
            }
        }
        /* Bare `.arm` after `=` / `(` / `,` → `Name_arm` (kind designator).
         * Not `.arm =` (payload designator) and not `recv.arm` (projection). */
        if (*p == '.' && ((p[1] >= 'a' && p[1] <= 'z') ||
                          (p[1] >= 'A' && p[1] <= 'Z') || p[1] == '_')) {
            const char* arm = p + 1;
            const char* e = arm;
            const char* after;
            const char* b;
            while (*e && ((*e >= 'a' && *e <= 'z') || (*e >= 'A' && *e <= 'Z') ||
                          (*e >= '0' && *e <= '9') || *e == '_'))
                e++;
            after = e;
            while (*after == ' ' || *after == '\t') after++;
            b = p;
            while (b > expr && (b[-1] == ' ' || b[-1] == '\t')) b--;
            /* RHS fragments are often just `.arm` (assign/init emit the `=`
             * outside); treat leading designators like after `=`. */
            if (*after != '=' &&
                (b == expr ||
                 (b > expr &&
                  (b[-1] == '=' || b[-1] == '(' || b[-1] == ',')))) {
                ShadowVariant* mv;
                int ma;
                if (shadow_variant_arm_match(v, expr, p, arm, (size_t)(e - arm),
                                             &mv, &ma)) {
                    int n = snprintf(o, rem, "%s_%s", mv->name, mv->arms[ma]);
                    if (n < 0 || (size_t)n >= rem) { rem = 0; break; }
                    o += n;
                    rem -= (size_t)n;
                    p = e;
                    goto cont;
                }
            }
        }
        /* `recv.arm` → `recv.u.arm` when preceded by a receiver token.
         * Skip `.u.arm` (already projected). */
        if (*p == '.' && ((p[1] >= 'a' && p[1] <= 'z') ||
                          (p[1] >= 'A' && p[1] <= 'Z') || p[1] == '_') &&
            p > expr &&
            (shadow_is_id(p[-1]) || p[-1] == ')' || p[-1] == ']') &&
            !(p >= expr + 2 && p[-1] == 'u' &&
              (p[-2] == '.' || p[-2] == '>'))) {
            const char* arm = p + 1;
            const char* e = arm;
            ShadowVariant* mv;
            int ma;
            while (*e && ((*e >= 'a' && *e <= 'z') || (*e >= 'A' && *e <= 'Z') ||
                          (*e >= '0' && *e <= '9') || *e == '_'))
                e++;
            if (!(e - arm == 4 && memcmp(arm, "kind", 4) == 0) &&
                !(e - arm == 1 && arm[0] == 'u')) {
                if (shadow_variant_arm_match(v, expr, p, arm, (size_t)(e - arm),
                                             &mv, &ma) &&
                    !mv->is_packed) {
                    int n = snprintf(o, rem, ".u.%s", mv->arms[ma]);
                    if (n < 0 || (size_t)n >= rem) { rem = 0; break; }
                    o += n;
                    rem -= (size_t)n;
                    p = e;
                    goto cont;
                }
            }
        }
        if (rem == 0) {
            fprintf(stderr,
                    "error: variant rewrite buffer full (%d bytes; refuse "
                    "truncated emit)\n",
                    SHADOW_VARIANT_REWRITE_BUF);
            return;
        }
        *o++ = *p++;
        rem--;
    cont:;
    }
    *o = 0;
    if (*p) {
        fprintf(stderr,
                "error: variant rewrite buffer full (%d bytes; refuse "
                "truncated emit)\n",
                SHADOW_VARIANT_REWRITE_BUF);
        return;
    }
    if (strlen(out) < cap) snprintf(expr, cap, "%s", out);
    /* Brace inits with injected tags → compound literals `(Name){…}`.
     * Covers `= { .kind = Name_…` and bare RHS `{ .kind = Name_…`. */
    if (g_shadow_nvariants) {
        char tmp[SHADOW_VARIANT_REWRITE_BUF];
        const char* q = expr;
        size_t o2 = 0;
        tmp[0] = 0;
        while (*q && o2 + 1 < sizeof(tmp)) {
            int at_eq = (q[0] == '=' && (q[1] == ' ' || q[1] == '{'));
            int at_brace = (*q == '{');
            if (at_eq || at_brace) {
                const char* r = at_eq ? q + 1 : q;
                while (*r == ' ' || *r == '\t') r++;
                /* Already `(Name){` — don't wrap again. */
                if (at_brace && q > expr && q[-1] == ')') {
                    tmp[o2++] = *q++;
                    continue;
                }
                if (*r == '{') {
                    const char* body = r + 1;
                    const char* tag;
                    while (*body == ' ' || *body == '\t') body++;
                    if (strncmp(body, ".kind = ", 8) != 0) {
                        tmp[o2++] = *q++;
                        continue;
                    }
                    tag = body + 8;
                    while (*tag == ' ') tag++;
                    for (a = 0; a < g_shadow_nvariants; a++) {
                        size_t nl = strlen(g_shadow_variants[a].name);
                        if (strncmp(tag, g_shadow_variants[a].name, nl) == 0 &&
                            tag[nl] == '_') {
                            int n;
                            if (at_eq)
                                n = snprintf(tmp + o2, sizeof(tmp) - o2,
                                             "= (%s){",
                                             g_shadow_variants[a].name);
                            else
                                n = snprintf(tmp + o2, sizeof(tmp) - o2,
                                             "(%s){",
                                             g_shadow_variants[a].name);
                            if (n < 0) break;
                            o2 += (size_t)n;
                            q = r + 1; /* skip original `{` */
                            goto vcont;
                        }
                    }
                }
            }
            tmp[o2++] = *q++;
            continue;
        vcont:;
        }
        tmp[o2] = 0;
        if (o2 && o2 < cap) snprintf(expr, cap, "%s", tmp);
    }
    if (v && v->is_packed)
        shadow_variant_packed_materialize(expr, cap, v);
    /* Close / unwrap packed set calls left as
     * `{ Name__cc_set_arm(x }` or `(Name){ Name__cc_set_arm(x }` inside a
     * larger expression (call args, bang typeof). Runs even when the rewrite
     * hint is not the packed variant (find_by_arm path). */
    if (strstr(expr, "__cc_set_")) {
        {
            char outb[SHADOW_VARIANT_REWRITE_BUF];
            const char* p = expr;
            char* o = outb;
            size_t rem = sizeof(outb) - 1;
            const char* setmark = "__cc_set_";
            outb[0] = 0;
            while (*p && rem > 0) {
                const char* hit = strstr(p, setmark);
                const char* cast_lp = NULL;
                const char* brace = NULL;
                const char* call;
                const char* q;
                size_t pre;
                if (!hit) {
                    size_t left = strlen(p);
                    if (left >= rem) left = rem - 1;
                    memcpy(o, p, left);
                    o += left;
                    rem -= left;
                    break;
                }
                call = hit;
                /* Walk back over TypeName before __cc_set_ (Name__cc_set_arm). */
                while (call > p &&
                       ((call[-1] >= 'a' && call[-1] <= 'z') ||
                        (call[-1] >= 'A' && call[-1] <= 'Z') ||
                        (call[-1] >= '0' && call[-1] <= '9') ||
                        call[-1] == '_'))
                    call--;
                /* Optional `(Type){` / `{` immediately before the set call. */
                q = call;
                while (q > p && (q[-1] == ' ' || q[-1] == '\t' ||
                                 q[-1] == '\n' || q[-1] == '\r'))
                    q--;
                if (q > p && q[-1] == '{') {
                    const char* r = q - 1;
                    while (r > p && (r[-1] == ' ' || r[-1] == '\t')) r--;
                    if (r > p && r[-1] == ')') {
                        const char* lp = r - 1;
                        int found = 0;
                        while (lp > p) {
                            if (lp[-1] == '(') {
                                lp--;
                                found = 1;
                                break;
                            }
                            if (lp[-1] == ')' || lp[-1] == ';' ||
                                lp[-1] == '{' || lp[-1] == '}')
                                break;
                            lp--;
                        }
                        if (found) {
                            cast_lp = lp;
                            brace = q - 1;
                        }
                    } else {
                        brace = q - 1;
                    }
                }
                pre = (size_t)((cast_lp ? cast_lp : (brace ? brace : call)) - p);
                if (pre >= rem) break;
                memcpy(o, p, pre);
                o += pre;
                rem -= pre;
                /* Emit from Type__cc_set_… through balanced ')' (add if missing). */
                {
                    const char* start = call;
                    const char* end;
                    int paren = 0;
                    int started = 0;
                    q = start;
                    while (*q) {
                        if (*q == '(') {
                            paren++;
                            started = 1;
                        } else if (*q == ')') {
                            paren--;
                            if (started && paren == 0) {
                                q++;
                                break;
                            }
                        } else if (started && paren == 0)
                            break;
                        else if (started && paren > 0 && *q == '}')
                            break;
                        q++;
                    }
                    end = q;
                    {
                        size_t clen = (size_t)(end - start);
                        int need_close = 0;
                        int d2 = 0;
                        for (q = start; q < end; q++) {
                            if (*q == '(') d2++;
                            else if (*q == ')') d2--;
                        }
                        if (d2 > 0) need_close = 1;
                        if (clen + (size_t)need_close >= rem) break;
                        memcpy(o, start, clen);
                        o += clen;
                        rem -= clen;
                        if (need_close) {
                            *o++ = ')';
                            rem--;
                        }
                    }
                    /* Skip trailing `}` from the compound literal if present. */
                    while (*end == ' ' || *end == '\t') end++;
                    if (*end == '}') end++;
                    p = end;
                }
            }
            *o = 0;
            if (outb[0]) snprintf(expr, cap, "%s", outb);
        }
    }
}

/* User generic factories (CC_GENERIC_FACTORY / _EXTEND). */
typedef struct {
    char family[64];
    char tpl[8192];
    int arity;
    int has_base; /* 0 if only CC_GENERIC_FACTORY_EXTEND seen */
    /* `#line` origin at factory definition (for invalid-C notes). */
    char origin_file[128];
    int origin_line;
} ShadowGenericFactory;
typedef struct {
    char family[64];
    char mangled[96]; /* Family_arg0_arg1 */
    char compact[96]; /* arg0_arg1 */
    char args[8][64]; /* per-comma spellings (long_long stays one arg) */
    int nargs;
} ShadowGenericInst;

enum { SHADOW_GFAC_CAP = 64 };
static ShadowGenericFactory g_shadow_gfac[SHADOW_GFAC_CAP];
static int g_shadow_ngfac;
enum { SHADOW_GINST_CAP = 128 };
static ShadowGenericInst g_shadow_ginst[SHADOW_GINST_CAP];
static int g_shadow_nginst;

/* Name → type bindings (AST+type phase). Drives capture unpack / UFCS.
 * Storage grows on g_shadow_meta_ar (TU heap root, same growth as a
 * stack arena). No fixed CAP. Lexical unwind is g_shadow_nbinds = mark. */
enum {
    SHADOW_BIND_ARRAY = 1,
    SHADOW_BIND_ATOMIC = 2,
    SHADOW_BIND_CACHE = 4
};
typedef struct ShadowBind {
    char name[64];
    char ty[128];
    int flags;
} ShadowBind;
static ShadowBind** g_shadow_binds;
static int g_shadow_nbinds;
static int g_shadow_binds_cap;
enum { SHADOW_CACHE_DECL_CAP = 128 };
static AstNode* g_shadow_cache_decl[SHADOW_CACHE_DECL_CAP];
static int g_shadow_ncache_decl;
/* When set, shadow_bind_name is a no-op (TU prepass still walks fn bodies for
 * type/generic registration without polluting the name→type map). */
static int g_shadow_binds_frozen;
static ShadowBind** g_shadow_cap_binds;
static int g_shadow_ncap_binds;
static int g_shadow_cap_binds_cap;
static int g_shadow_cap_overflow;

/* Outer → @as field path (beachhead: one embed per outer/target). */
typedef struct {
    char outer[64];
    char field[64];
    char target[64]; /* field base type, e.g. CCFile */
    char mode[64];   /* viewed face: as: (Mode)field; empty = full surface */
} ShadowAsEmbed;
enum { SHADOW_AS_CAP = 128 };
static ShadowAsEmbed g_shadow_as[SHADOW_AS_CAP];
static int g_shadow_nas;

/* `@typeview on Pat* { as: field; }` — materialize exact embeds per match. */
typedef struct {
    char pat[64];
    char field[64];
    int hits; /* concrete types that resolved this face */
} ShadowAsGlob;
enum { SHADOW_AS_GLOB_CAP = 64 };
static ShadowAsGlob g_shadow_as_globs[SHADOW_AS_GLOB_CAP];
static int g_shadow_nas_globs;

/* Registered destroy delta from cc_type_register / destroy_call|hooks. */
typedef struct {
    char ty[64];
    char pre[96];  /* optional pre-destroy */
    char hook[96]; /* destroy / post */
} ShadowDestroyHook;
enum { SHADOW_DHOOK_CAP = 128 };
static ShadowDestroyHook g_shadow_dhooks[SHADOW_DHOOK_CAP];
static int g_shadow_ndhooks;

/* Registered create from cc_type_create_call (for @create()). */
typedef struct {
    char ty[64];
    char hook1[96];
    char hook2[96];
} ShadowCreateHook;
enum { SHADOW_CHOOK_CAP = 128 };
static ShadowCreateHook g_shadow_chooks[SHADOW_CHOOK_CAP];
static int g_shadow_nchooks;

/* Compiled `.ufcs` hooks (dylib). Lookup is glob, longest subject wins. */
static CCSymbolTable* g_shadow_ufcs_syms;
static const char* g_shadow_ufcs_compile_src;
static size_t g_shadow_ufcs_compile_len;
static char g_shadow_ufcs_path[1024];
enum { SHADOW_UFCS_SEEN_CAP = 128 };
static char g_shadow_ufcs_seen[SHADOW_UFCS_SEEN_CAP][256];
static int g_shadow_ufcs_nseen;
/* Current structured UFCS site — hook-compose miss uses these for file:line. */
static TapeCache* g_shadow_ufcs_cache;
static AstNode* g_shadow_ufcs_site;

/* Dynamic UFCS sink (.ufcs_sink): unresolved methods lower to
 * callee(&recv, "method", N, WRAP(a), …). Always dest-aware. */
typedef struct {
    char ty[64];
    char callee[96];
    char wrap[64];
    int dest_aware;      /* always 1; kept so emit can stay dest-gated */
    int returns_result;  /* plain callee declares CCResult_ / T !>(E) return */
} ShadowDynSink;
enum { SHADOW_DSINK_CAP = 64 };
static ShadowDynSink g_shadow_dsinks[SHADOW_DSINK_CAP];
static int g_shadow_ndsinks;
/* Active destination type for .ufcs_sink (whole-RHS / cast only). */
static char g_shadow_sink_dest[96];

/* Local fn param bases for @as arg coerce + autoblock unpack casts
 * (fn, arg_index → T / stars). Map lives with UFCS tables below
 * (CC_MAP_DECL_ARENA — growable; fixed CAP silently degraded casts, then
 * failed closed at 512 on large modules). */
typedef struct {
    char fn[64];
    char base[64];
    int stars;
    int argi;
} ShadowFnParam;

/* @typeview / @typeview Mode on Base — allow-list view types
 * (draft_facets.md) plus optional `as:` faces (draft_as.md).
 * Groups: r: (use/call), w: (store), rw: (both), as: (is-a embeds).
 * Patterns: exact, `foo*` / `*suffix`, or `^pat` (subtract). A group of
 * only denies implies `*`. `as:` names are fields of Base — not membership. */
enum {
    SHADOW_RESTRICT_CAP = 32,
    SHADOW_RESTRICT_ALLOW_CAP = 48,
    SHADOW_RESTRICT_R = 1,
    SHADOW_RESTRICT_W = 2,
    SHADOW_RESTRICT_RW = 3,
    SHADOW_RESTRICT_AS = 4
};
typedef struct {
    char pat[64];
    int kind; /* SHADOW_RESTRICT_R / W / RW */
    int deny; /* 1 = `^pat` (subtract after allow) */
} ShadowRestrictEnt;
typedef struct {
    char base[64];
    char mode[64];
    char mangled[128];
    ShadowRestrictEnt allow[SHADOW_RESTRICT_ALLOW_CAP];
    int nallow;
} ShadowRestrict;
static ShadowRestrict g_shadow_restricts[SHADOW_RESTRICT_CAP];
static int g_shadow_nrestricts;
static int g_shadow_restrict_diag;
/* When set, field access in check_text is treated as a store (AST_ASSIGN lhs). */
static int g_shadow_restrict_lhs_store;
static void (*g_shadow_extent_len_store_fn)(const char*);
/* Unnamed @typeview on Base: bodies whose first param is Base/Base* skip
 * allow-list checks for that base (trusted method bodies). */
static char g_shadow_restrict_trusted_base[64];

static const char* shadow_restrict_mode_label(const ShadowRestrict* r) {
    if (!r || !r->mode[0]) return "(default)";
    return r->mode;
}

static int shadow_restrict_is_unnamed(const ShadowRestrict* r) {
    return r && r->mode[0] == 0;
}

static int shadow_restrict_trusted_for(const ShadowRestrict* r) {
    return r && shadow_restrict_is_unnamed(r) && g_shadow_restrict_trusted_base[0] &&
           strcmp(g_shadow_restrict_trusted_base, r->base) == 0;
}

static ShadowRestrict* shadow_restrict_find_mangled(const char* mangled) {
    int i;
    if (!mangled || !mangled[0]) return NULL;
    for (i = 0; i < g_shadow_nrestricts; i++) {
        if (strcmp(g_shadow_restricts[i].mangled, mangled) == 0)
            return &g_shadow_restricts[i];
    }
    return NULL;
}

/* Slice family root for type-family keying (draft_facets § type-family bases).
 * Instantiation must not fork the unnamed facet. */
static const char* shadow_restrict_family_root(const char* leaf) {
    if (!leaf || !leaf[0]) return NULL;
    if (strcmp(leaf, "CCSlice") == 0 || strcmp(leaf, "CCSliceUnique") == 0 ||
        strcmp(leaf, "CCSliceShared") == 0 || strcmp(leaf, "CCSlicePacked") == 0 ||
        strcmp(leaf, "CCSliceHdr") == 0 || strcmp(leaf, "CCSliceArray") == 0)
        return "CCSlice";
    if (strncmp(leaf, "CCSlice_", 8) == 0) return "CCSlice";
    return leaf;
}

static int shadow_restrict_pattern_matches(const char* pattern, const char* name);
static size_t shadow_restrict_pattern_score(const char* pattern);
static void shadow_restrict_leaf_ty(const char* ty, char* leaf, size_t cap);

/* Spelled leaf: strip const/stars, do not walk typedefs. Factory aliases
 * (CCNursery → CCBox_CCNurseryHost) keep their own names for glob views. */
static void shadow_restrict_spelled_leaf(const char* ty, char* leaf, size_t cap) {
    const char* p;
    size_t n;
    if (!leaf || !cap) return;
    leaf[0] = 0;
    if (!ty) return;
    p = ty;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "const ", 6) == 0) p += 6;
    while (*p == ' ' || *p == '\t') p++;
    n = strlen(p);
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '*'))
        n--;
    if (n >= cap) n = cap - 1;
    memcpy(leaf, p, n);
    leaf[n] = 0;
}

static ShadowRestrict* shadow_restrict_find_for_ty(const char* ty) {
    ShadowRestrict* r;
    const char* root;
    char spelled[96];
    char walked[96];
    int i;
    int best = -1;
    ShadowRestrict* best_r = NULL;
    if (!ty || !ty[0]) return NULL;
    shadow_restrict_spelled_leaf(ty, spelled, sizeof(spelled));
    shadow_restrict_leaf_ty(ty, walked, sizeof(walked));
    if (!walked[0] && !spelled[0]) return NULL;
    r = shadow_restrict_find_mangled(walked[0] ? walked : spelled);
    if (r) return r;
    root = shadow_restrict_family_root(walked[0] ? walked : spelled);
    if (root && strcmp(root, walked) != 0) {
        r = shadow_restrict_find_mangled(root);
        if (r) return r;
    }
    /* Glob subjects match the spelled name. `CCNursery` does not inherit
     * `@typeview on CCBox_*`. */
    if (!spelled[0]) return NULL;
    for (i = 0; i < g_shadow_nrestricts; i++) {
        int score;
        ShadowRestrict* cand = &g_shadow_restricts[i];
        if (!strchr(cand->base, '*')) continue;
        if (!shadow_restrict_pattern_matches(cand->base, spelled)) continue;
        score = (int)shadow_restrict_pattern_score(cand->base);
        if (score > best) {
            best = score;
            best_r = cand;
        } else if (score == best && best_r && cand != best_r) {
            fprintf(stderr,
                    "error: type '%s' matched by equal-score @typeview "
                    "subjects '%s' and '%s'\n",
                    spelled, best_r->base, cand->base);
            g_shadow_restrict_diag = 1;
            return NULL;
        }
    }
    return best_r;
}

/* Name/type glob: trailing `foo*` / `Foo_*` (prefix match; `Foo_*` also
 * matches bare `Foo`) or leading `*suffix` (suffix match). Bare `*` matches
 * every name. */
static int shadow_restrict_pattern_matches(const char* pattern, const char* name) {
    size_t plen;
    size_t nlen;
    if (!pattern || !name) return 0;
    plen = strlen(pattern);
    nlen = strlen(name);
    if (plen > 0 && pattern[0] == '*') {
        size_t slen = plen - 1;
        if (slen == 0) return 1;
        if (nlen < slen) return 0;
        return memcmp(name + nlen - slen, pattern + 1, slen) == 0;
    }
    if (plen > 0 && pattern[plen - 1] == '*') {
        size_t prefix_len = plen - 1;
        if (prefix_len > 0 && pattern[prefix_len - 1] == '_') {
            size_t base_len = prefix_len - 1;
            if (strncmp(name, pattern, base_len) == 0 &&
                (name[base_len] == '\0' || name[base_len] == '_'))
                return 1;
        }
        return strncmp(name, pattern, prefix_len) == 0;
    }
    return strcmp(pattern, name) == 0;
}

static size_t shadow_restrict_pattern_score(const char* pattern) {
    size_t plen;
    if (!pattern) return 0;
    plen = strlen(pattern);
    if (plen > 0 && pattern[0] == '*') return plen - 1;
    if (plen > 0 && pattern[plen - 1] == '*') return plen - 1;
    return plen;
}

/* Best matching kind for name, or 0 if none. Sets diag on equal-score conflict.
 * Allow-set first; a matching deny subtracts that group's bits. */
static int shadow_restrict_best_kind(const ShadowRestrict* r, const char* name) {
    int i;
    int best_score = -1;
    int best_kind = 0;
    if (!r || !name || !name[0]) return 0;
    for (i = 0; i < r->nallow; i++) {
        int score;
        if (r->allow[i].deny) continue;
        if (!shadow_restrict_pattern_matches(r->allow[i].pat, name)) continue;
        score = (int)shadow_restrict_pattern_score(r->allow[i].pat);
        if (score > best_score) {
            best_score = score;
            best_kind = r->allow[i].kind;
        } else if (score == best_score && r->allow[i].kind != best_kind) {
            fprintf(stderr,
                    "error: restricted mode '%s' on '%s': name '%s' matched by "
                    "equal-score patterns with disagreeing use-kinds\n",
                    shadow_restrict_mode_label(r), r->base, name);
            g_shadow_restrict_diag = 1;
            return 0;
        }
    }
    if (!best_kind) return 0;
    for (i = 0; i < r->nallow; i++) {
        int dk;
        if (!r->allow[i].deny) continue;
        if (!shadow_restrict_pattern_matches(r->allow[i].pat, name)) continue;
        dk = r->allow[i].kind;
        if (dk == SHADOW_RESTRICT_RW)
            best_kind = 0;
        else if (dk == SHADOW_RESTRICT_R) {
            if (best_kind == SHADOW_RESTRICT_R) best_kind = 0;
            else if (best_kind == SHADOW_RESTRICT_RW)
                best_kind = SHADOW_RESTRICT_W;
        } else if (dk == SHADOW_RESTRICT_W) {
            if (best_kind == SHADOW_RESTRICT_W) best_kind = 0;
            else if (best_kind == SHADOW_RESTRICT_RW)
                best_kind = SHADOW_RESTRICT_R;
        }
        if (!best_kind) return 0;
    }
    return best_kind;
}

/* Field load or UFCS call. */
static int shadow_restrict_allows_use(const ShadowRestrict* r, const char* name) {
    int k = shadow_restrict_best_kind(r, name);
    return k == SHADOW_RESTRICT_R || k == SHADOW_RESTRICT_RW;
}

/* Field store (assignment / update). */
static int shadow_restrict_allows_store(const ShadowRestrict* r, const char* name) {
    int k = shadow_restrict_best_kind(r, name);
    return k == SHADOW_RESTRICT_W || k == SHADOW_RESTRICT_RW;
}

static const char* shadow_td_alias_resolve(const char* ty);

/* Strip trailing stars/spaces → leaf typedef name for restrict lookup.
 * Follows typedef aliases (ConnEnc → Conn_Restrict_Encode*). */
static void shadow_restrict_leaf_ty(const char* ty, char* leaf, size_t cap) {
    char cur[160];
    int guard;
    size_t n;
    if (!leaf || !cap) return;
    leaf[0] = 0;
    if (!ty) return;
    snprintf(cur, sizeof(cur), "%s", ty);
    for (guard = 0; guard < 8; guard++) {
        const char* p = cur;
        const char* resolved;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "const ", 6) == 0) p += 6;
        while (*p == ' ' || *p == '\t') p++;
        n = strlen(p);
        while (n > 0 &&
               (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '*'))
            n--;
        if (n >= cap) n = cap - 1;
        memcpy(leaf, p, n);
        leaf[n] = 0;
        if (!leaf[0]) return;
        resolved = shadow_td_alias_resolve(leaf);
        if (!resolved || strcmp(resolved, leaf) == 0) return;
        /* Alias may carry stars (ConnEnc → Conn_Restrict_Encode*). */
        snprintf(cur, sizeof(cur), "%s", resolved);
    }
}

/* Parse one pattern token (ident, ident*, *, or *suffix). Advances *pp. */
static int shadow_restrict_parse_pat(const char** pp, char* out, size_t cap) {
    const char* p;
    size_t nl = 0;
    if (!pp || !out || !cap) return 0;
    p = *pp;
    out[0] = 0;
    while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\n' || *p == '\r' ||
           *p == ';')
        p++;
    if (!*p || *p == '}') {
        *pp = p;
        return 0;
    }
    if (*p == '*') {
        /* `*` (all) or `*suffix` (name ends with suffix). */
        if (cap < 2) return 0;
        out[0] = '*';
        nl = 1;
        p++;
        while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
               (*p >= '0' && *p <= '9') || *p == '_') {
            if (nl + 1 < cap) out[nl++] = *p;
            p++;
        }
        out[nl] = 0;
        *pp = p;
        return 1;
    }
    while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
           (*p >= '0' && *p <= '9') || *p == '_' || *p == '*') {
        if (nl + 1 < cap) out[nl++] = *p;
        if (*p == '*') {
            p++;
            break;
        }
        p++;
    }
    out[nl] = 0;
    *pp = p;
    return out[0] != 0;
}

static int shadow_restrict_parse_kind_label(const char** pp, int* out_kind) {
    const char* p;
    const char* q;
    if (!pp || !out_kind) return 0;
    p = *pp;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ';')
        p++;
    /* Token spelling inserts spaces: `as : ( Region ) field`. */
    if (p[0] == 'a' && p[1] == 's') {
        q = p + 2;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == ':') {
            *out_kind = SHADOW_RESTRICT_AS;
            *pp = q + 1;
            return 1;
        }
    }
    if (p[0] == 'r' && p[1] == 'w') {
        q = p + 2;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == ':') {
            *out_kind = SHADOW_RESTRICT_RW;
            *pp = q + 1;
            return 1;
        }
    }
    if (p[0] == 'r' || p[0] == 'w') {
        q = p + 1;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == ':') {
            *out_kind = (p[0] == 'r') ? SHADOW_RESTRICT_R : SHADOW_RESTRICT_W;
            *pp = q + 1;
            return 1;
        }
    }
    return 0;
}

static int shadow_field_ty_of(const char* outer, const char* field, char* dst,
                              size_t cap);
static int shadow_ty_restrict_base(const char* ty, char* dst, size_t cap);
static const char* shadow_td_alias_resolve(const char* ty);
static void shadow_ty_base_stars(const char* ty, char* base, size_t bcap,
                                 int* stars);
static int shadow_embed_destroy_append(char* body, size_t* bo, size_t cap,
                                       const char* ty, const char* path,
                                       int skip_pre);
static void shadow_as_register(const char* outer, const char* field,
                               const char* target);
static void shadow_as_register_viewed(const char* outer, const char* field,
                                     const char* target, const char* view);
static void shadow_as_resolve_transitive(void);
static void shadow_as_glob_register(const char* pat, const char* field);
static void shadow_as_materialize_globs_for(const char* outer);
static void shadow_as_materialize_all_globs(void);
static int shadow_as_field_target(const char* outer, const char* field,
                                  char* dst, size_t cap);
static int shadow_ty_is_glob(const char* s) {
    return s && strchr(s, '*') != NULL;
}

static int shadow_restrict_parse_body(const char* body, ShadowRestrictEnt* out,
                                     int cap, int* nout, const char* mode,
                                     const char* base, char as_fields[][64],
                                     char as_modes[][64], int as_cap, int* nas) {
    const char* p = body ? body : "";
    int kind = SHADOW_RESTRICT_R;
    int n = 0;
    int na = 0;
    if (!out || !nout || !cap) return 0;
    *nout = 0;
    if (nas) *nas = 0;
    while (*p == ' ' || *p == '\t' || *p == '{' || *p == '\n' || *p == '\r') p++;
    while (*p && *p != '}') {
        int k;
        char pat[64];
        int i;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ';' ||
               *p == ',')
            p++;
        if (!*p || *p == '}') break;
        if (shadow_restrict_parse_kind_label(&p, &k)) {
            kind = k;
            continue;
        }
        /* `as:` faces — `field` or `(Mode)field`; not allow-list patterns. */
        if (kind == SHADOW_RESTRICT_AS) {
            char face_mode[64];
            face_mode[0] = 0;
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (*p == '^') {
                fprintf(stderr,
                        "error: @typeview %s on %s: as: cannot use '^'\n",
                        mode ? mode : "?", base ? base : "?");
                g_shadow_restrict_diag = 1;
                return 0;
            }
            if (*p == '(') {
                int mi = 0;
                p++;
                while (*p == ' ' || *p == '\t') p++;
                while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                       (*p >= '0' && *p <= '9') || *p == '_') {
                    if (mi + 1 < (int)sizeof(face_mode))
                        face_mode[mi++] = *p;
                    p++;
                }
                face_mode[mi] = 0;
                while (*p == ' ' || *p == '\t') p++;
                if (*p != ')') {
                    fprintf(stderr,
                            "error: @typeview %s on %s: expected ')' after "
                            "as: mode\n",
                            mode ? mode : "?", base ? base : "?");
                    g_shadow_restrict_diag = 1;
                    return 0;
                }
                p++;
                if (!face_mode[0]) {
                    fprintf(stderr,
                            "error: @typeview %s on %s: empty as: mode\n",
                            mode ? mode : "?", base ? base : "?");
                    g_shadow_restrict_diag = 1;
                    return 0;
                }
            }
            if (!shadow_restrict_parse_pat(&p, pat, sizeof(pat))) break;
            if (!as_fields || !nas || as_cap <= 0) {
                fprintf(stderr,
                        "error: @typeview %s on %s: as: faces not supported "
                        "here\n",
                        mode ? mode : "?", base ? base : "?");
                g_shadow_restrict_diag = 1;
                return 0;
            }
            for (i = 0; i < na; i++) {
                if (strcmp(as_fields[i], pat) == 0) {
                    fprintf(stderr,
                            "error: @typeview %s on %s: duplicate as: field "
                            "'%s'\n",
                            mode ? mode : "?", base ? base : "?", pat);
                    g_shadow_restrict_diag = 1;
                    return 0;
                }
            }
            if (na >= as_cap) {
                fprintf(stderr,
                        "error: @typeview %s on %s: as: list too long (>%d)\n",
                        mode ? mode : "?", base ? base : "?", as_cap);
                g_shadow_restrict_diag = 1;
                return 0;
            }
            snprintf(as_fields[na], 64, "%s", pat);
            if (as_modes)
                snprintf(as_modes[na], 64, "%s", face_mode);
            na++;
            continue;
        }
        {
            int deny = 0;
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (*p == '^') {
                deny = 1;
                p++;
            }
            if (!shadow_restrict_parse_pat(&p, pat, sizeof(pat))) break;
            if (deny && strcmp(pat, "*") == 0) {
                fprintf(stderr,
                        "error: @typeview %s on %s: '^*' is ill-formed\n",
                        mode ? mode : "?", base ? base : "?");
                g_shadow_restrict_diag = 1;
                return 0;
            }
            for (i = 0; i < n; i++) {
                if (strcmp(out[i].pat, pat) == 0) {
                    if (out[i].deny == deny)
                        fprintf(stderr,
                                "error: @typeview %s on %s: duplicate "
                                "allow-list pattern '%s%s'\n",
                                mode ? mode : "?", base ? base : "?",
                                deny ? "^" : "", pat);
                    else
                        fprintf(stderr,
                                "error: @typeview %s on %s: pattern '%s' "
                                "disagrees with '^%s'\n",
                                mode ? mode : "?", base ? base : "?", pat,
                                pat);
                    g_shadow_restrict_diag = 1;
                    return 0;
                }
            }
            if (n >= cap) {
                fprintf(stderr,
                        "error: @typeview %s on %s: allow-list too long "
                        "(>%d)\n",
                        mode ? mode : "?", base ? base : "?", cap);
                g_shadow_restrict_diag = 1;
                return 0;
            }
            snprintf(out[n].pat, sizeof(out[n].pat), "%s", pat);
            out[n].kind = kind;
            out[n].deny = deny;
            n++;
        }
    }
    /* A group of only denies implies `*`. */
    {
        int k;
        int i;
        for (k = SHADOW_RESTRICT_R; k <= SHADOW_RESTRICT_RW; k++) {
            int has_pos = 0;
            int has_den = 0;
            int has_star = 0;
            for (i = 0; i < n; i++) {
                if (out[i].kind != k) continue;
                if (out[i].deny)
                    has_den = 1;
                else {
                    has_pos = 1;
                    if (strcmp(out[i].pat, "*") == 0) has_star = 1;
                }
            }
            if (!has_den || has_pos || has_star) continue;
            if (n >= cap) {
                fprintf(stderr,
                        "error: @typeview %s on %s: allow-list too long "
                        "(>%d)\n",
                        mode ? mode : "?", base ? base : "?", cap);
                g_shadow_restrict_diag = 1;
                return 0;
            }
            snprintf(out[n].pat, sizeof(out[n].pat), "*");
            out[n].kind = k;
            out[n].deny = 0;
            n++;
        }
    }
    *nout = n;
    if (nas) *nas = na;
    return 1;
}

static int shadow_restrict_register(const char* base, const char* mode,
                                   const char* body) {
    ShadowRestrict* r;
    char mangled[128];
    ShadowRestrictEnt seen[SHADOW_RESTRICT_ALLOW_CAP];
    char as_fields[SHADOW_RESTRICT_ALLOW_CAP][64];
    char as_modes[SHADOW_RESTRICT_ALLOW_CAP][64];
    int nseen = 0;
    int nas = 0;
    int i;
    const char* mlabel;
    if (!base || !base[0]) return 0;
    if (!mode) mode = "";
    mlabel = mode[0] ? mode : "(default)";
    /* Named → Base_Restrict_Mode; unnamed → keyed on Base itself. */
    if (mode[0])
        snprintf(mangled, sizeof(mangled), "%s_Restrict_%s", base, mode);
    else
        snprintf(mangled, sizeof(mangled), "%s", base);
    memset(as_modes, 0, sizeof(as_modes));
    if (!shadow_restrict_parse_body(body, seen, SHADOW_RESTRICT_ALLOW_CAP,
                                    &nseen, mlabel, base, as_fields, as_modes,
                                    SHADOW_RESTRICT_ALLOW_CAP, &nas))
        return 0;
    /* `as:` → same embed table as field `@as` (unique target type per path).
     * Glob subjects (`CCSlice_*`) store templates; materialize when fields exist. */
    for (i = 0; i < nas; i++) {
        char fty[96];
        char target[64];
        int stars = 0;
        if (shadow_ty_is_glob(base)) {
            if (mode[0]) {
                fprintf(stderr,
                        "error: @typeview %s on %s: named mode cannot use a "
                        "glob subject\n",
                        mlabel, base);
                g_shadow_restrict_diag = 1;
                return 0;
            }
            shadow_as_glob_register(base, as_fields[i]);
            continue;
        }
        if (!shadow_field_ty_of(base, as_fields[i], fty, sizeof(fty))) {
            fprintf(stderr,
                    "error: @typeview %s on %s: as: unknown field '%s'\n",
                    mlabel, base, as_fields[i]);
            g_shadow_restrict_diag = 1;
            return 0;
        }
        shadow_ty_base_stars(fty, target, sizeof(target), &stars);
        if (!target[0]) {
            fprintf(stderr,
                    "error: @typeview %s on %s: as: field '%s' has no type\n",
                    mlabel, base, as_fields[i]);
            g_shadow_restrict_diag = 1;
            return 0;
        }
        if (stars != 0) {
            fprintf(stderr,
                    "error: @typeview %s on %s: as: field '%s' must be a "
                    "value embed, not a pointer\n",
                    mlabel, base, as_fields[i]);
            g_shadow_restrict_diag = 1;
            return 0;
        }
        shadow_as_register_viewed(base, as_fields[i], target, as_modes[i]);
    }
    /* as:-only body: faces without locking the base allow-list. */
    if (nseen == 0) {
        if (shadow_ty_is_glob(base)) shadow_as_materialize_all_globs();
        return 1;
    }
    if (shadow_ty_is_glob(base) && mode[0]) {
        fprintf(stderr,
                "error: @typeview %s on %s: named mode cannot use a glob "
                "subject\n",
                mlabel, base);
        g_shadow_restrict_diag = 1;
        return 0;
    }
    r = shadow_restrict_find_mangled(mangled);
    if (r) {
        if (nseen != r->nallow) {
            fprintf(stderr,
                    "error: @typeview %s on %s: incompatible redeclaration "
                    "(allow-list differs)\n",
                    mlabel, base);
            g_shadow_restrict_diag = 1;
            return 0;
        }
        for (i = 0; i < nseen; i++) {
            int j;
            int ok = 0;
            for (j = 0; j < r->nallow; j++) {
                if (strcmp(r->allow[j].pat, seen[i].pat) == 0 &&
                    r->allow[j].kind == seen[i].kind &&
                    r->allow[j].deny == seen[i].deny) {
                    ok = 1;
                    break;
                }
            }
            if (!ok) {
                fprintf(stderr,
                        "error: @typeview %s on %s: incompatible "
                        "redeclaration (allow-list differs)\n",
                        mlabel, base);
                g_shadow_restrict_diag = 1;
                return 0;
            }
        }
        return 1;
    }
    if (g_shadow_nrestricts >= SHADOW_RESTRICT_CAP) {
        fprintf(stderr, "error: too many @typeview modes (>%d)\n",
                SHADOW_RESTRICT_CAP);
        g_shadow_restrict_diag = 1;
        return 0;
    }
    r = &g_shadow_restricts[g_shadow_nrestricts++];
    memset(r, 0, sizeof(*r));
    snprintf(r->base, sizeof(r->base), "%s", base);
    snprintf(r->mode, sizeof(r->mode), "%s", mode);
    snprintf(r->mangled, sizeof(r->mangled), "%s", mangled);
    for (i = 0; i < nseen; i++) r->allow[i] = seen[i];
    r->nallow = nseen;
    return 1;
}

static const ShadowFnParam* shadow_fnparam_lookup(const char* fn, int argi);
static void shadow_fnparam_ingest_headers(void);
static const ShadowBind* shadow_bind_lookup(const char* name);
static void shadow_restrict_check_call_widen(const char* fn, const char* args);

/* Scan expression text for restricted field / widen violations. */
static void shadow_restrict_check_text(const char* text) {
    int bi;
    const char* cp;
    if (!text || !text[0] || g_shadow_restrict_diag) return;
    /* Call sites embedded in opaque bodies: foo(restricted_arg). */
    cp = text;
    while (*cp && !g_shadow_restrict_diag) {
        char fn[96];
        size_t fl = 0;
        const char* q;
        int depth;
        char args[512];
        size_t ai = 0;
        if (!((cp[0] >= 'A' && cp[0] <= 'Z') || (cp[0] >= 'a' && cp[0] <= 'z') ||
              cp[0] == '_')) {
            cp++;
            continue;
        }
        if (cp > text &&
            ((cp[-1] >= 'A' && cp[-1] <= 'Z') ||
             (cp[-1] >= 'a' && cp[-1] <= 'z') ||
             (cp[-1] >= '0' && cp[-1] <= '9') || cp[-1] == '_')) {
            cp++;
            continue;
        }
        q = cp;
        while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') ||
               (*q >= '0' && *q <= '9') || *q == '_') {
            if (fl + 1 < sizeof(fn)) fn[fl++] = *q;
            q++;
        }
        fn[fl] = 0;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '(') {
            cp = q;
            continue;
        }
        q++;
        depth = 1;
        while (*q && depth > 0 && ai + 1 < sizeof(args)) {
            if (*q == '(') depth++;
            else if (*q == ')') {
                depth--;
                if (depth == 0) break;
            }
            args[ai++] = *q++;
        }
        args[ai] = 0;
        if (depth == 0 && fn[0]) shadow_restrict_check_call_widen(fn, args);
        cp = (*q == ')') ? q + 1 : q;
    }
    if (g_shadow_restrict_diag) return;
    for (bi = 0; bi < g_shadow_nbinds; bi++) {
        ShadowRestrict* rr;
        const char* name = g_shadow_binds[bi] ? g_shadow_binds[bi]->name : NULL;
        const char* p;
        size_t nl;
        if (!name || !name[0]) continue;
        rr = shadow_restrict_find_for_ty(g_shadow_binds[bi]->ty);
        if (!rr) continue;
        if (shadow_restrict_trusted_for(rr)) continue;
        nl = strlen(name);
        p = text;
        while ((p = strstr(p, name)) != NULL) {
            const char* q;
            char field[64];
            size_t fl = 0;
            int arrow;
            if (p > text &&
                ((p[-1] >= 'A' && p[-1] <= 'Z') ||
                 (p[-1] >= 'a' && p[-1] <= 'z') ||
                 (p[-1] >= '0' && p[-1] <= '9') || p[-1] == '_')) {
                p += nl;
                continue;
            }
            q = p + nl;
            if (q[0] == '-' && q[1] == '>') {
                arrow = 1;
                q += 2;
            } else if (q[0] == '.') {
                arrow = 0;
                q += 1;
            } else {
                p += nl;
                continue;
            }
            (void)arrow;
            while (*q == ' ' || *q == '\t') q++;
            while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') ||
                   (*q >= '0' && *q <= '9') || *q == '_') {
                if (fl + 1 < sizeof(field)) field[fl++] = *q;
                q++;
            }
            field[fl] = 0;
            if (!field[0]) {
                p += nl;
                continue;
            }
            /* UFCS call: name->meth( — method filter owns that. */
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '(') {
                p += nl;
                continue;
            }
            /* Store: = / op= / ++ / -- after the field, or AST_ASSIGN lhs when
             * the field is the destination. Indexed use (`m.ptr[i] = …`) is a
             * Gap through the field; a field inside an index (`buf[src.len] =
             * …`) is a use of that field, not a store to it. */
            {
                int is_store = 0;
                const char* s = q;
                if (s[0] == '+' && s[1] == '+') is_store = 1;
                else if (s[0] == '-' && s[1] == '-') is_store = 1;
                else if (s[0] == '=' && s[1] != '=') is_store = 1;
                else if ((s[0] == '+' || s[0] == '-' || s[0] == '*' ||
                          s[0] == '/' || s[0] == '%' || s[0] == '|' ||
                          s[0] == '&' || s[0] == '^') &&
                         s[1] == '=')
                    is_store = 1;
                else if ((s[0] == '<' || s[0] == '>') && s[1] == s[0] &&
                         s[2] == '=')
                    is_store = 1;
                else if (g_shadow_restrict_lhs_store) {
                    /* `m.ptr[i]` / `buf[src.len]` / call args: not the dest. */
                    if (s[0] != '[' && s[0] != ']' && s[0] != ')' &&
                        s[0] != ',')
                        is_store = 1;
                }
                if (is_store) {
                    if (!shadow_restrict_allows_store(rr, field)) {
                        fprintf(stderr,
                                "error: restricted mode '%s' on '%s' does not "
                                "allow store to field '%s'\n",
                                shadow_restrict_mode_label(rr), rr->base, field);
                        g_shadow_restrict_diag = 1;
                        return;
                    }
                } else if (!shadow_restrict_allows_use(rr, field)) {
                    fprintf(stderr,
                            "error: restricted mode '%s' on '%s' does not allow "
                            "field '%s'\n",
                            shadow_restrict_mode_label(rr), rr->base, field);
                    fprintf(stderr, "note: %s.%s\n", name, field);
                    g_shadow_restrict_diag = 1;
                    return;
                }
            }
            p += nl;
        }
    }
    if (g_shadow_extent_len_store_fn) g_shadow_extent_len_store_fn(text);
}

/* Widen: restricted view passed where plain Base* is required. */
static void shadow_restrict_check_call_widen(const char* fn, const char* args) {
    int argi = 0;
    const char* p;
    char arg[192];
    size_t ai;
    if (!fn || !fn[0] || !args || g_shadow_restrict_diag) return;
    p = args;
    while (*p) {
        const ShadowFnParam* fp;
        const ShadowBind* b;
        ShadowRestrict* rr;
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        ai = 0;
        {
            int depth = 0;
            while (*p && ai + 1 < sizeof(arg)) {
                if (*p == '(' || *p == '[' || *p == '{') depth++;
                else if (*p == ')' || *p == ']' || *p == '}') {
                    if (depth) depth--;
                } else if (*p == ',' && depth == 0)
                    break;
                arg[ai++] = *p++;
            }
        }
        arg[ai] = 0;
        while (ai > 0 && (arg[ai - 1] == ' ' || arg[ai - 1] == '\t'))
            arg[--ai] = 0;
        /* Bare ident or &ident */
        {
            char id[64];
            const char* s = arg;
            size_t n = 0;
            while (*s == ' ' || *s == '\t') s++;
            if (*s == '&') s++;
            while (*s == ' ' || *s == '\t') s++;
            while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
                   (*s >= '0' && *s <= '9') || *s == '_') {
                if (n + 1 < sizeof(id)) id[n++] = *s;
                s++;
            }
            id[n] = 0;
            while (*s == ' ' || *s == '\t') s++;
            if (!id[0] || *s != 0) {
                argi++;
                continue;
            }
            b = shadow_bind_lookup(id);
            if (!b) {
                argi++;
                continue;
            }
            rr = shadow_restrict_find_for_ty(b->ty);
            if (!rr) {
                argi++;
                continue;
            }
            fp = shadow_fnparam_lookup(fn, argi);
            if (!fp || fp->stars != 1) {
                argi++;
                continue;
            }
            /* Unnamed: mangled == base; no parallel view to widen from. */
            if (shadow_restrict_is_unnamed(rr)) {
                argi++;
                continue;
            }
            /* Param is Base* (not the restricted mangled type) → widen. */
            if (strcmp(fp->base, rr->mangled) != 0 &&
                strcmp(fp->base, rr->base) == 0) {
                fprintf(stderr,
                        "error: restricted mode '%s' on '%s': cannot widen to "
                        "'%s*' (const-shaped; no cast)\n",
                        shadow_restrict_mode_label(rr), rr->base, rr->base);
                g_shadow_restrict_diag = 1;
                return;
            }
        }
        argi++;
        if (*p == ',') p++;
    }
}

/* Angle-includes are passthrough (not spliced into the AST), so the
 * slice-family unnamed facet is installed here — same meaning as:
 *   @typeview on CCSlice { r: ^ptr, ^id; };
 * `.len` + UFCS open; representation fields hidden; field stores denied;
 * first-arg bodies trusted. Peel is a written cast / as_ptr(). */
static void shadow_restrict_register_builtins(void) {
    (void)shadow_restrict_register("CCSlice", "", "{ r: ^ptr, ^id; }");
    /* Named pointer: methods, not `.p`. Same body as `cc_box.cch`.
     * Header lowering strips the live `@typeview`; pin after table reset. */
    (void)shadow_restrict_register("CCBox_*", "", "{ r: ^p; }");
    /* Typed-slice family face — also declared in cc_slice.cch as
     * `@typeview on CCSlice_* { as: base; }` (stripped from host `.h`). */
    shadow_as_glob_register("CCSlice_*", "base");
    /* Header lowering strips named @typeview on CCArena. Pin after the
     * table reset so `@typeview(Parent) CCArena*` and viewed faces work
     * without a live declaration in the consuming TU. */
    (void)shadow_restrict_register("CCArena", "Alloc",
                                   "{ r: alloc, remaining; }");
    (void)shadow_restrict_register("CCArena", "Parent",
                                   "{ r: adopt, attach, create_*; }");
    (void)shadow_restrict_register("CCArena", "Region",
                                   "{ r: alloc, remaining, adopt, attach, "
                                   "create_*; }");
}

static void shadow_restrict_register_from_items(AstNode** items, int n) {
    int i;
    if (!items) return;
    for (i = 0; i < n; i++) {
        AstNode* it = items[i];
        if (!it) continue;
        if (it->kind == AST_AT_STMT && strcmp(it->a, "typeview") == 0 &&
            it->d[0])
            (void)shadow_restrict_register(it->d, it->b, it->c);
        if (it->kids && it->nkids > 0)
            shadow_restrict_register_from_items(it->kids, it->nkids);
        if (it->nbody > 0)
            shadow_restrict_register_from_items(it->body, it->nbody);
        if (it->ndbody > 0)
            shadow_restrict_register_from_items(it->dbody, it->ndbody);
    }
}

/* Set when a typed UFCS miss was diagnosed — fail the emit product. */
static int g_shadow_ufcs_miss;

/* Set when arena-less @string had an unbounded slot — fail the emit product.
 * Named *_diag (not *_err): Concurrent-C rewrites `_err` assignments. */
static int g_shadow_string_stack_diag;

/* Declared callables for UFCS family/bare-name resolution + fn param types.
 * CC_MAP_DECL_ARENA (lowered host form — soft-parse static bodies stay C).
 * Arena-backed; reset per TU. No fixed CAP. */
typedef struct {
    char name[96];
    char first_ty[128];
} ShadowUfcsFn;

typedef struct {
    char fn[64];
    int argi;
} ShadowFnParamKey;

static size_t cc_map_key_hash_ShadowFnParamKey(ShadowFnParamKey k) {
    size_t h = cc_map_hash_slice_hdr(
        cc_slice_hdr_from_buffer(k.fn, strlen(k.fn)));
    return h ^ ((size_t)(unsigned)k.argi * 0x9e3779b97f4a7c15ULL);
}
static int cc_map_key_eq_ShadowFnParamKey(ShadowFnParamKey a,
                                          ShadowFnParamKey b) {
    return a.argi == b.argi && strcmp(a.fn, b.fn) == 0;
}

CC_MAP_DECL_ARENA(CCSliceHdr, ShadowUfcsFn, ShadowUfcsFnMap,
                  cc_map_hash_slice_hdr, cc_map_eq_slice_hdr);
CC_MAP_DECL_ARENA(ShadowFnParamKey, ShadowFnParam, ShadowFnParamMap,
                  cc_map_key_hash_ShadowFnParamKey,
                  cc_map_key_eq_ShadowFnParamKey);

static CCArena g_shadow_meta_ar;
static int g_shadow_meta_ar_live;
static ShadowUfcsFnMap* g_shadow_ufns;
static ShadowFnParamMap* g_shadow_fnparams;
static int g_shadow_fnparam_oom;

static void shadow_meta_ar_ensure(void) {
    if (!g_shadow_meta_ar_live) {
        /* 1MiB root — large TUs (720-method modules) grow freely. */
        g_shadow_meta_ar = cc_arena_heap(1024 * 1024);
        if (!cc_arena_is_live(g_shadow_meta_ar)) {
            fprintf(stderr, "error: shadow meta arena create failed\n");
            g_shadow_ufcs_miss = 1;
            return;
        }
        g_shadow_meta_ar_live = 1;
    }
    if (!g_shadow_ufns) {
        g_shadow_ufns = ShadowUfcsFnMap_init(g_shadow_meta_ar);
        if (!g_shadow_ufns) {
            fprintf(stderr, "error: UFCS callable map init failed\n");
            g_shadow_ufcs_miss = 1;
        }
    }
    if (!g_shadow_fnparams) {
        g_shadow_fnparams = ShadowFnParamMap_init(g_shadow_meta_ar);
        if (!g_shadow_fnparams) {
            fprintf(stderr, "error: fn param map init failed\n");
            g_shadow_fnparam_oom = 1;
            g_shadow_ufcs_miss = 1;
        }
    }
}

/* Struct/typedef fields — names for callable-field wins; field_ty for
 * `recv.field.meth` UFCS (e.g. db.entries.del → ArrayMap_*_del).
 * Lives on g_shadow_meta_ar (TU-lifetime heap root, same growth as a
 * stack arena). A cc_arena_stack root cannot own this table: it is
 * process-static and reset per TU, not a single stack frame. */
typedef struct {
    const char* ty;
    const char* field;
    const char* field_ty;
} ShadowStructField;
CC_VEC_DECL_ARENA(ShadowStructField, ShadowFieldVec)
static ShadowFieldVec g_shadow_fields;
static int g_shadow_fields_live;

static void shadow_meta_ar_clear_tables(void) {
    g_shadow_ufns = NULL;
    g_shadow_fnparams = NULL;
    g_shadow_fnparam_oom = 0;
    g_shadow_fields_live = 0;
    memset(&g_shadow_fields, 0, sizeof(g_shadow_fields));
    g_shadow_binds = NULL;
    g_shadow_binds_cap = 0;
    g_shadow_cap_binds = NULL;
    g_shadow_cap_binds_cap = 0;
    if (g_shadow_meta_ar_live) cc_arena_reset(g_shadow_meta_ar);
}

/* Grow a ShadowBind pointer table on the TU meta arena. Each bind is
 * arena-owned so lookup pointers stay valid across later pushes. */
static int shadow_bind_tab_grow(ShadowBind*** tab, int* cap, int nlive, int need,
                               const char* table, const char* item) {
    ShadowBind** nbuf;
    int ncap;
    if (!tab || !cap) return 0;
    if (need <= *cap) return 1;
    shadow_meta_ar_ensure();
    if (!g_shadow_meta_ar_live) {
        shadow_table_grow_failed(table, item);
        return 0;
    }
    ncap = *cap ? *cap * 2 : 64;
    while (ncap < need) ncap *= 2;
    nbuf = (ShadowBind**)cc_arena_alloc(g_shadow_meta_ar,
                                        (size_t)ncap * sizeof(ShadowBind*),
                                        _Alignof(ShadowBind*));
    if (!nbuf) {
        shadow_table_grow_failed(table, item);
        return 0;
    }
    if (*tab && nlive > 0)
        memcpy(nbuf, *tab, (size_t)nlive * sizeof(ShadowBind*));
    *tab = nbuf;
    *cap = ncap;
    return 1;
}

static ShadowBind* shadow_bind_new_slot(ShadowBind*** tab, int* cap, int* n,
                                       const char* table, const char* item) {
    ShadowBind* b;
    if (!shadow_bind_tab_grow(tab, cap, *n, *n + 1, table, item)) return NULL;
    shadow_meta_ar_ensure();
    if (!g_shadow_meta_ar_live) {
        shadow_table_grow_failed(table, item);
        return NULL;
    }
    b = (ShadowBind*)cc_arena_alloc(g_shadow_meta_ar, sizeof(ShadowBind),
                                    _Alignof(ShadowBind));
    if (!b) {
        shadow_table_grow_failed(table, item);
        return NULL;
    }
    memset(b, 0, sizeof(*b));
    (*tab)[*n] = b;
    (*n)++;
    return b;
}

static CCSliceHdr shadow_meta_name_key(const char* name) {
    if (!name) return cc_slice_hdr_from_buffer(NULL, 0);
    return cc_slice_hdr_from_buffer((void*)name, strlen(name));
}

static CCSliceHdr shadow_meta_intern_name(const char* name) {
    size_t n;
    char* p;
    CCSliceHdr h = {0};
    if (!name) return h;
    n = strlen(name);
    shadow_meta_ar_ensure();
    if (!g_shadow_meta_ar_live) return h;
    p = (char*)cc_arena_alloc(g_shadow_meta_ar, n + 1, 1);
    if (!p) return h;
    memcpy(p, name, n + 1);
    return cc_slice_hdr_from_buffer(p, n);
}

static const char* shadow_meta_cstr(const char* s) {
    CCSliceHdr h;
    if (!s || !s[0]) return NULL;
    h = shadow_meta_intern_name(s);
    return (const char*)h.ptr;
}

static int shadow_fields_ensure(void) {
    shadow_meta_ar_ensure();
    if (!g_shadow_meta_ar_live) return 0;
    if (!g_shadow_fields_live) {
        g_shadow_fields = ShadowFieldVec_init(g_shadow_meta_ar, 8);
        g_shadow_fields_live = 1;
    }
    return 1;
}

/* Field declarator base: `args[16]` / `args [16]` → `args` for UFCS walks. */
static void shadow_field_ident_norm(const char* field, char* out, size_t cap) {
    shadow_decl_ident(field, out, cap);
}

static ShadowStructField* shadow_field_find(const char* ty, const char* field) {
    size_t i, n;
    const char* resolved;
    if (!ty || !ty[0] || !field || !field[0]) return NULL;
    if (!shadow_fields_ensure()) return NULL;
    n = ShadowFieldVec_len(&g_shadow_fields);
    for (i = 0; i < n; i++) {
        ShadowStructField* f = ShadowFieldVec_get(&g_shadow_fields, i);
        if (!f || !f->ty || !f->field) continue;
        if (strcmp(f->ty, ty) == 0 && strcmp(f->field, field) == 0)
            return f;
    }
    /* `typedef struct tag Alias` — fields are on the tag. */
    resolved = shadow_td_alias_resolve(ty);
    if (resolved && resolved[0] && strcmp(resolved, ty) != 0) {
        if (strncmp(resolved, "struct ", 7) == 0) resolved += 7;
        while (*resolved == ' ' || *resolved == '\t') resolved++;
        if (resolved[0] && strcmp(resolved, ty) != 0)
            return shadow_field_find(resolved, field);
    }
    return NULL;
}

static void shadow_field_register_ex(const char* ty, const char* field,
                                     const char* field_ty) {
    const char* fty = field_ty;
    char fname[64];
    char tagged[160];
    char ptr_ty[160];
    int is_array = 0;
    ShadowStructField* existing;
    ShadowStructField row;
    if (!ty || !ty[0] || !field || !field[0]) return;
    /* `Vec3 p, n` is one AST field with a comma name blob; UFCS walks `n`. */
    if (strchr(field, ',')) {
        const char* p = field;
        char one[96];
        while (shadow_field_next_name(&p, one, sizeof(one)))
            shadow_field_register_ex(ty, one, field_ty);
        return;
    }
    if (strchr(field, '[')) is_array = 1;
    shadow_field_ident_norm(field, fname, sizeof(fname));
    if (!fname[0]) return;
    /* Walker keeps `*` on the name (`RtxNode *next`); UFCS/hoist/safety
     * still want the pointer on the registered type. */
    if (strchr(field, '*') && fty && fty[0] && !strchr(fty, '*')) {
        snprintf(ptr_ty, sizeof(ptr_ty), "%s*", fty);
        fty = ptr_ty;
    }
    field = fname;
    if (!shadow_fields_ensure()) return;
    /* Resolve typedef aliases (DbMap → ArrayMap_CCSliceHdr_Entr*). */
    if (fty && fty[0]) {
        int ai;
        for (ai = 0; ai < g_shadow_ntd; ai++) {
            if (strcmp(g_shadow_td_alias[ai], fty) == 0) {
                fty = g_shadow_td_base[ai];
                break;
            }
        }
    }
    /* Array declarators lose `[n]` in the field name; keep it on the type
     * so destroy will not treat the slot as an owned value embed. */
    if (is_array && fty && fty[0]) {
        snprintf(tagged, sizeof(tagged), "%s[]", fty);
        fty = tagged;
    }
    existing = shadow_field_find(ty, field);
    if (existing) {
        if (fty && fty[0] && (!existing->field_ty || !existing->field_ty[0])) {
            existing->field_ty = shadow_meta_cstr(fty);
            if (fty[0] && !existing->field_ty) {
                fprintf(stderr, "error: shadow field type intern failed\n");
                g_shadow_ufcs_miss = 1;
            }
        }
        return;
    }
    row.ty = shadow_meta_cstr(ty);
    row.field = shadow_meta_cstr(field);
    row.field_ty = (fty && fty[0]) ? shadow_meta_cstr(fty) : NULL;
    if (!row.ty || !row.field || (fty && fty[0] && !row.field_ty)) {
        fprintf(stderr, "error: shadow field intern failed\n");
        g_shadow_ufcs_miss = 1;
        return;
    }
    if (ShadowFieldVec_push(&g_shadow_fields, row) != 0) {
        fprintf(stderr, "error: shadow field vec grow failed\n");
        g_shadow_ufcs_miss = 1;
    }
}

static void shadow_field_register(const char* ty, const char* field) {
    shadow_field_register_ex(ty, field, NULL);
}

static int shadow_type_has_field(const char* ty, const char* field) {
    return shadow_field_find(ty, field) != NULL;
}

enum { SHADOW_FNPTR_TY_CAP = 128 };
static char g_shadow_fnptr_tys[SHADOW_FNPTR_TY_CAP][96];
static int g_shadow_nfnptr_tys;

static void shadow_fnptr_ty_register(const char* name) {
    int i;
    if (!name || !name[0]) return;
    if (g_shadow_nfnptr_tys >= SHADOW_FNPTR_TY_CAP) {
        shadow_table_full("fnptr_tys", SHADOW_FNPTR_TY_CAP, name);
        return;
    }
    for (i = 0; i < g_shadow_nfnptr_tys; i++)
        if (strcmp(g_shadow_fnptr_tys[i], name) == 0) return;
    snprintf(g_shadow_fnptr_tys[g_shadow_nfnptr_tys++],
             sizeof(g_shadow_fnptr_tys[0]), "%s", name);
}

static int shadow_is_fnptr_ty(const char* name) {
    int i;
    if (!name || !name[0]) return 0;
    if (strstr(name, "(*") || strstr(name, "(^")) return 1;
    for (i = 0; i < g_shadow_nfnptr_tys; i++)
        if (strcmp(g_shadow_fnptr_tys[i], name) == 0) return 1;
    return 0;
}

static int shadow_type_has_callable_field(const char* ty, const char* field) {
    ShadowStructField* f = shadow_field_find(ty, field);
    if (!f) return 0;
    return shadow_is_fnptr_ty(f->field_ty);
}

/* `Base_Restrict_Mode` → `Base`. Named views share Base's fields. */
static int shadow_ty_restrict_base(const char* ty, char* dst, size_t cap) {
    const char* hit;
    size_t n, i;
    if (!ty || !dst || !cap) return 0;
    hit = strstr(ty, "_Restrict_");
    if (!hit || hit == ty) return 0;
    n = (size_t)(hit - ty);
    if (n == 0 || n >= cap) return 0;
    for (i = 0; i < n; i++) {
        char c = ty[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    memcpy(dst, ty, n);
    dst[n] = 0;
    return 1;
}

/* Look up registered field type of `outer.field` (borrowed into dst). */
static int shadow_field_ty_of(const char* outer, const char* field, char* dst,
                              size_t cap) {
    ShadowStructField* f;
    char base[128];
    if (!outer || !field || !dst || !cap) return 0;
    dst[0] = 0;
    f = shadow_field_find(outer, field);
    if ((!f || !f->field_ty || !f->field_ty[0]) &&
        shadow_ty_restrict_base(outer, base, sizeof(base)))
        f = shadow_field_find(base, field);
    if (!f || !f->field_ty || !f->field_ty[0]) return 0;
    snprintf(dst, cap, "%s", f->field_ty);
    {
        const char* resolved = shadow_td_alias_resolve(dst);
        if (resolved && resolved[0] && strcmp(resolved, dst) != 0)
            snprintf(dst, cap, "%s", resolved);
    }
    return 1;
}

/* True when `mangled` is a recorded generic-factory instance name. */
static int shadow_ginst_has_mangled(const char* mangled) {
    int i;
    if (!mangled || !mangled[0]) return 0;
    for (i = 0; i < g_shadow_nginst; i++) {
        if (strcmp(g_shadow_ginst[i].mangled, mangled) == 0) return 1;
    }
    return 0;
}

/* Raw field span `int (*cb)(…)` / `void* user` → register field name. */
static void shadow_field_register_raw(const char* ty, const char* span) {
    const char* p;
    char name[64];
    size_t ni = 0;
    if (!ty || !ty[0] || !span || !span[0]) return;
    p = strstr(span, "(*");
    if (p) {
        p += 2;
        while (*p == ' ' || *p == '\t') p++;
        while (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                (*p >= '0' && *p <= '9') || *p == '_') &&
               ni + 1 < sizeof(name))
            name[ni++] = *p++;
        name[ni] = 0;
        if (name[0]) shadow_field_register(ty, name);
        return;
    }
    /* Last identifier in the span. */
    p = span + strlen(span);
    while (p > span &&
           !((p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= 'a' && p[-1] <= 'z') ||
             (p[-1] >= '0' && p[-1] <= '9') || p[-1] == '_'))
        p--;
    while (p > span &&
           ((p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= 'a' && p[-1] <= 'z') ||
            (p[-1] >= '0' && p[-1] <= '9') || p[-1] == '_'))
        p--;
    while (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '_') &&
           ni + 1 < sizeof(name))
        name[ni++] = *p++;
    name[ni] = 0;
    if (name[0]) shadow_field_register(ty, name);
}

static int shadow_ufn_exists(const char* name);
static int shadow_ufcs_callee_declared(const char* name);
static const char* shadow_ufn_unique_for_meth(const char* meth);
static void shadow_as_register(const char* outer, const char* field,
                               const char* target);
static void shadow_as_register_viewed(const char* outer, const char* field,
                                     const char* target, const char* view);
static void shadow_as_resolve_transitive(void);

/* Family member sets from `##_<member>` in family macros (same rule as
 * production preprocess.c). Not an allowlist — derived from the headers. */
typedef struct {
    char suffix[64];
    char csv[1024];
    int loaded;
} ShadowFamilyMembers;
enum { SHADOW_FAMILY_MEM_CAP = 32 };
static ShadowFamilyMembers g_shadow_family_mem[SHADOW_FAMILY_MEM_CAP];

static int shadow_is_ident_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static const char* shadow_family_header_for(const char* base) {
    return cc_ufcs_family_header_suffix(base);
}

static int shadow_family_header_read(const char* suffix, char** out,
                                     size_t* out_len) {
    char cand[512];
    const char* env;
    FILE* f;
    long sz;
    char* buf;
    size_t n;
    if (!suffix || !out || !out_len) return 0;
    *out = NULL;
    *out_len = 0;
    snprintf(cand, sizeof(cand), "cc/include/ccc/%s", suffix);
    f = fopen(cand, "rb");
    if (!f) {
        snprintf(cand, sizeof(cand), "out/include/ccc/%s", suffix);
        f = fopen(cand, "rb");
    }
    if (!f && (env = getenv("CC_INCLUDE_PATH")) && env[0]) {
        char paths[1024];
        char* p;
        char* sep;
        snprintf(paths, sizeof(paths), "%s", env);
        p = paths;
        while (p && *p && !f) {
            sep = strchr(p, ':');
            if (sep) *sep = 0;
            if (*p) {
                snprintf(cand, sizeof(cand), "%s/ccc/%s", p, suffix);
                f = fopen(cand, "rb");
            }
            p = sep ? sep + 1 : NULL;
        }
    }
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    sz = ftell(f);
    if (sz < 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = 0;
    *out = buf;
    *out_len = n;
    return 1;
}

static void shadow_family_scan_members(const char* fsrc, size_t fn,
                                       ShadowFamilyMembers* slot) {
    size_t i;
    if (!fsrc || !slot) return;
    for (i = 0; i + 3 < fn; i++) {
        if (fsrc[i] != '#' || fsrc[i + 1] != '#' || fsrc[i + 2] != '_')
            continue;
        {
            size_t ms = i + 3, me = ms;
            while (me < fn && shadow_is_ident_char(fsrc[me])) me++;
            if (me > ms && me - ms < 96 && fsrc[ms] != '_') {
                char mem[96];
                char pat[100];
                char hay[1100];
                size_t ol = strlen(slot->csv);
                memcpy(mem, fsrc + ms, me - ms);
                mem[me - ms] = 0;
                snprintf(pat, sizeof(pat), ", %s,", mem);
                snprintf(hay, sizeof(hay), ", %s,", slot->csv);
                if (!strstr(hay, pat) && ol + (me - ms) + 3 < sizeof(slot->csv)) {
                    if (ol) {
                        slot->csv[ol++] = ',';
                        slot->csv[ol++] = ' ';
                    }
                    memcpy(slot->csv + ol, mem, me - ms + 1);
                }
            }
            i = me;
        }
    }
}

static const char* shadow_family_members_csv(const char* base) {
    const char* suffix;
    size_t ci;
    ShadowFamilyMembers* slot = NULL;
    char* fsrc = NULL;
    size_t fn = 0;
    suffix = shadow_family_header_for(base);
    if (!suffix) return "";
    for (ci = 0; ci < (size_t)SHADOW_FAMILY_MEM_CAP; ci++) {
        if (g_shadow_family_mem[ci].loaded &&
            strcmp(g_shadow_family_mem[ci].suffix, suffix) == 0)
            return g_shadow_family_mem[ci].csv;
        if (!slot && !g_shadow_family_mem[ci].loaded) slot = &g_shadow_family_mem[ci];
    }
    if (!slot) {
        shadow_table_full("family_mem", SHADOW_FAMILY_MEM_CAP, suffix);
        return "";
    }
    snprintf(slot->suffix, sizeof(slot->suffix), "%s", suffix);
    slot->csv[0] = 0;
    slot->loaded = 1;
    if (shadow_family_header_read(suffix, &fsrc, &fn) && fsrc) {
        shadow_family_scan_members(fsrc, fn, slot);
        free(fsrc);
    }
    return slot->csv;
}

static int shadow_family_has_member(const char* base, const char* method) {
    const char* csv;
    char pat[110];
    char hay[1100];
    if (!base || !method || !method[0]) return 0;
    csv = shadow_family_members_csv(base);
    if (!csv[0]) return 0;
    snprintf(pat, sizeof(pat), ", %s,", method);
    snprintf(hay, sizeof(hay), ", %s,", csv);
    return strstr(hay, pat) != NULL;
}

/* Family member OR TU/header extension by composed declaration. */
static int shadow_family_accepts(const char* base, const char* method) {
    char composed[160];
    char ext[160];
    if (shadow_family_has_member(base, method)) return 1;
    if (!base || !method || !method[0]) return 0;
    snprintf(composed, sizeof(composed), "%s_%s", base, method);
    if (shadow_ufcs_callee_declared(composed)) return 1;
    /* Slice extensions often spell cc_slice_<elem>_<meth>. */
    if (strncmp(base, "CCSlice_", 8) == 0) {
        snprintf(ext, sizeof(ext), "cc_slice_%s_%s", base + 8, method);
        if (shadow_ufcs_callee_declared(ext)) return 1;
    }
    return 0;
}

/* Erased CCSlice helpers (`CCSlice_is_empty` / `cc_slice_is_empty`) live in
 * cc_slice.cch outside the ##_ SPEC set — cache their names for UFCS. */
static int shadow_cc_slice_erased_has(const char* method) {
    static char csv[1100];
    static int loaded = 0;
    char pat[110];
    char hay[1200];
    if (!method || !method[0]) return 0;
    if (!loaded) {
        char* fsrc = NULL;
        size_t fn = 0;
        size_t i;
        csv[0] = 0;
        loaded = 1;
        if (shadow_family_header_read("cc_slice.h", &fsrc, &fn) && fsrc) {
            for (i = 0; i + 10 < fn; i++) {
                const char* pref = NULL;
                size_t plen = 0;
                size_t ms, me, ol;
                if (strncmp(fsrc + i, "CCSlice_", 8) == 0) {
                    pref = fsrc + i + 8;
                    plen = 8;
                } else if (strncmp(fsrc + i, "cc_slice_", 9) == 0) {
                    pref = fsrc + i + 9;
                    plen = 9;
                } else
                    continue;
                ms = 0;
                while (pref[ms] && shadow_is_ident_char(pref[ms])) ms++;
                if (!ms || pref[ms] != '(') continue;
                /* Skip typed instance spellings: CCSlice_double_len( */
                {
                    size_t u;
                    int has_us = 0;
                    for (u = 0; u < ms; u++)
                        if (pref[u] == '_') has_us = 1;
                    if (has_us && plen == 8) continue;
                }
                me = ms;
                ol = strlen(csv);
                snprintf(pat, sizeof(pat), ", %.*s,", (int)me, pref);
                snprintf(hay, sizeof(hay), ", %s,", csv);
                if (!strstr(hay, pat) && ol + me + 3 < sizeof(csv)) {
                    if (ol) {
                        csv[ol++] = ',';
                        csv[ol++] = ' ';
                    }
                    memcpy(csv + ol, pref, me);
                    csv[ol + me] = 0;
                }
                i += plen + me;
            }
            free(fsrc);
        }
    }
    if (!csv[0]) return 0;
    snprintf(pat, sizeof(pat), ", %s,", method);
    snprintf(hay, sizeof(hay), ", %s,", csv);
    return strstr(hay, pat) != NULL;
}

static void shadow_note_task_send(const char* name) {
    int i;
    if (!name || !name[0]) return;
    if (g_shadow_ntask_send >= SHADOW_CHAN_CAP) {
        shadow_table_full("task_send", SHADOW_CHAN_CAP, name);
        return;
    }
    for (i = 0; i < g_shadow_ntask_send; i++) {
        if (strcmp(g_shadow_task_send[i], name) == 0) return;
    }
    snprintf(g_shadow_task_send[g_shadow_ntask_send],
             sizeof(g_shadow_task_send[0]), "%s", name);
    g_shadow_ntask_send++;
}

static int shadow_chan_has_task_send(const char* name) {
    int i;
    if (!name || !name[0]) return 0;
    for (i = 0; i < g_shadow_ntask_send; i++) {
        if (strcmp(g_shadow_task_send[i], name) == 0) return 1;
    }
    return 0;
}

static void shadow_collect_task_sends(AstNode* n) {
    int k;
    if (!n) return;
    if (n->kind == AST_SPAWN_CLOSURE && n->a[0] &&
        (strcmp(n->b, "send_task") == 0 ||
         strcmp(n->b, "send_task_hybrid") == 0)) {
        /* UFCS `tx.send_task` stores tx in a; free-fn callarg keeps formals. */
        if (!strchr(n->a, ' ') && !strchr(n->a, '*') && !strchr(n->a, ','))
            shadow_note_task_send(n->a);
    }
    if (n->kind == AST_UFCS_STMT && n->a[0] &&
        (strcmp(n->b, "send_task") == 0 ||
         strcmp(n->b, "send_task_hybrid") == 0))
        shadow_note_task_send(n->a);
    if (n->kind == AST_CALL_ARGS &&
        (strcmp(n->a, "cc_channel_send_task") == 0 ||
         strcmp(n->a, "cc_channel_send_task_hybrid") == 0) &&
        n->b[0]) {
        char tx[64];
        const char* p = n->b;
        size_t i = 0;
        while (*p == ' ' || *p == '\t') p++;
        while (*p && *p != ',' && *p != ' ' && *p != '\t' &&
               i + 1 < sizeof(tx))
            tx[i++] = *p++;
        tx[i] = 0;
        if (tx[0]) shadow_note_task_send(tx);
    }
    for (k = 0; k < n->nbody; k++) shadow_collect_task_sends(n->body[k]);
    for (k = 0; k < n->ndbody; k++) shadow_collect_task_sends(n->dbody[k]);
    if (n->kids) {
        for (k = 0; k < n->nkids; k++) shadow_collect_task_sends(n->kids[k]);
    }
}

static void shadow_ufn_register(const char* name, const char* first_ty);

static void shadow_chan_reset(void) {
    g_shadow_nchans = 0;
    g_shadow_ntask_send = 0;
    g_shadow_nvecs = 0;
    g_shadow_nmaps = 0;
    g_shadow_namaps = 0;
    g_shadow_ntd = 0;
    g_shadow_nslices = 0;
    g_shadow_nfnptr_tys = 0;
    g_shadow_nvariants = 0;
    g_shadow_ngfac = 0;
    g_shadow_nginst = 0;
    g_shadow_nbinds = 0;
    g_shadow_ncache_decl = 0;
    g_shadow_nrfns = 0;
    g_shadow_binds_frozen = 0;
    g_shadow_ncap_binds = 0;
    g_shadow_cap_overflow = 0;
    shadow_table_overflow_reset();
    shadow_meta_ar_clear_tables();
    /* System libc bare UFCS (math.h / string.h) — shadow does not harvest
     * those headers into ufn; seed first-param types for emit_bare. */
    shadow_ufn_register("fabs", "double");
    shadow_ufn_register("strlen", "const char*");
    g_shadow_nas = 0;
    g_shadow_nas_globs = 0;
    /* Stdlib as: faces — header lowering strips `@typeview { as: base; }`,
     * so cc_err(e) / @errhandler still need the unique path without a
     * live typeview in the TU. */
    shadow_as_register("CCIoError", "base", "CCError");
    /* Opaque nursery: header lowering strips `as: (Region)n`. */
    shadow_as_register_viewed("CCNursery", "arena", "CCArena", "Region");
    g_shadow_ndhooks = 0;
    g_shadow_nchooks = 0;
    if (g_shadow_ufcs_syms) {
        cc_symbols_free(g_shadow_ufcs_syms);
        g_shadow_ufcs_syms = NULL;
    }
    g_shadow_ufcs_nseen = 0;
    g_shadow_ufcs_compile_src = NULL;
    g_shadow_ufcs_compile_len = 0;
    g_shadow_ufcs_path[0] = 0;
    g_shadow_ufcs_cache = NULL;
    g_shadow_ufcs_site = NULL;
    g_shadow_nniches = 0;
    g_shadow_ndsinks = 0;
    g_shadow_sink_dest[0] = 0;
    g_shadow_nrestricts = 0;
    g_shadow_restrict_diag = 0;
    g_shadow_restrict_lhs_store = 0;
    g_shadow_restrict_trusted_base[0] = 0;
    g_shadow_ufcs_miss = 0;
    g_shadow_eh_diag = 0;
    g_shadow_string_stack_diag = 0;
}

static void shadow_destroy_hook_register(const char* ty, const char* pre,
                                        const char* hook) {
    int i;
    if (!ty || !ty[0]) return;
    if ((!pre || !pre[0]) && (!hook || !hook[0])) return;
    if (g_shadow_ndhooks >= SHADOW_DHOOK_CAP) {
        shadow_table_full("destroy_hooks", SHADOW_DHOOK_CAP, ty);
        return;
    }
    for (i = 0; i < g_shadow_ndhooks; i++) {
        if (strcmp(g_shadow_dhooks[i].ty, ty) == 0) {
            if (pre && pre[0])
                snprintf(g_shadow_dhooks[i].pre, sizeof(g_shadow_dhooks[0].pre),
                         "%s", pre);
            if (hook && hook[0])
                snprintf(g_shadow_dhooks[i].hook,
                         sizeof(g_shadow_dhooks[0].hook), "%s", hook);
            return;
        }
    }
    snprintf(g_shadow_dhooks[g_shadow_ndhooks].ty,
             sizeof(g_shadow_dhooks[0].ty), "%s", ty);
    g_shadow_dhooks[g_shadow_ndhooks].pre[0] = 0;
    g_shadow_dhooks[g_shadow_ndhooks].hook[0] = 0;
    if (pre && pre[0])
        snprintf(g_shadow_dhooks[g_shadow_ndhooks].pre,
                 sizeof(g_shadow_dhooks[0].pre), "%s", pre);
    if (hook && hook[0])
        snprintf(g_shadow_dhooks[g_shadow_ndhooks].hook,
                 sizeof(g_shadow_dhooks[0].hook), "%s", hook);
    g_shadow_ndhooks++;
}

static char g_shadow_chook_exp[128];
static ShadowDestroyHook g_shadow_dhook_exp;

static int shadow_hook_is_suffix(const char* hook) {
    return hook && hook[0] == '_' && hook[1] && hook[1] != '_';
}

static void shadow_hook_expand(const char* ty, const char* hook, char* dst,
                               size_t cap) {
    char base[96];
    size_t n;
    if (!dst || !cap) return;
    dst[0] = 0;
    if (!hook || !hook[0]) return;
    if (!shadow_hook_is_suffix(hook) || !ty || !ty[0]) {
        snprintf(dst, cap, "%s", hook);
        return;
    }
    snprintf(base, sizeof(base), "%s", ty);
    n = strlen(base);
    while (n && (base[n - 1] == ' ' || base[n - 1] == '\t' || base[n - 1] == '*'))
        base[--n] = 0;
    snprintf(dst, cap, "%s%s", base, hook);
}

static const char* shadow_td_alias_one(const char* ty);

/* Exact then glob on one spelling. Suffix `_destroy` expands on `ty`. */
static const ShadowDestroyHook* shadow_destroy_hooks_match(const char* ty) {
    int i;
    int best = -1;
    size_t best_score = 0;
    const ShadowDestroyHook* h = NULL;
    if (!ty || !ty[0]) return NULL;
    for (i = 0; i < g_shadow_ndhooks; i++) {
        if (strcmp(g_shadow_dhooks[i].ty, ty) == 0) {
            h = &g_shadow_dhooks[i];
            break;
        }
    }
    if (!h) {
        for (i = 0; i < g_shadow_ndhooks; i++) {
            size_t score;
            if (!shadow_ty_is_glob(g_shadow_dhooks[i].ty)) continue;
            if (!shadow_restrict_pattern_matches(g_shadow_dhooks[i].ty, ty))
                continue;
            score = shadow_restrict_pattern_score(g_shadow_dhooks[i].ty);
            if (score > best_score) {
                best_score = score;
                best = i;
            }
        }
        if (best >= 0) h = &g_shadow_dhooks[best];
    }
    if (!h) return NULL;
    if (!shadow_hook_is_suffix(h->pre) && !shadow_hook_is_suffix(h->hook))
        return h;
    g_shadow_dhook_exp = *h;
    if (shadow_hook_is_suffix(h->pre))
        shadow_hook_expand(ty, h->pre, g_shadow_dhook_exp.pre,
                           sizeof(g_shadow_dhook_exp.pre));
    if (shadow_hook_is_suffix(h->hook))
        shadow_hook_expand(ty, h->hook, g_shadow_dhook_exp.hook,
                           sizeof(g_shadow_dhook_exp.hook));
    return &g_shadow_dhook_exp;
}

/* Typedef aliases use the base type's hooks (try each hop, then glob). */
static const ShadowDestroyHook* shadow_destroy_hooks_for(const char* ty) {
    int guard = 0;
    const char* cur = ty;
    if (!ty || !ty[0]) return NULL;
    while (guard++ < 8) {
        const ShadowDestroyHook* h = shadow_destroy_hooks_match(cur);
        const char* next;
        if (h) return h;
        next = shadow_td_alias_one(cur);
        if (!next || !next[0] || strcmp(next, cur) == 0) break;
        cur = next;
    }
    return NULL;
}

static const char* shadow_destroy_hook_for(const char* ty) {
    const ShadowDestroyHook* h = shadow_destroy_hooks_for(ty);
    return (h && h->hook[0]) ? h->hook : NULL;
}

static const ShadowCreateHook* shadow_create_hook_entry(const char* ty) {
    int i;
    int best = -1;
    size_t best_score = 0;
    if (!ty || !ty[0]) return NULL;
    for (i = 0; i < g_shadow_nchooks; i++) {
        if (strcmp(g_shadow_chooks[i].ty, ty) == 0)
            return &g_shadow_chooks[i];
    }
    for (i = 0; i < g_shadow_nchooks; i++) {
        size_t score;
        if (!shadow_ty_is_glob(g_shadow_chooks[i].ty)) continue;
        if (!shadow_restrict_pattern_matches(g_shadow_chooks[i].ty, ty))
            continue;
        score = shadow_restrict_pattern_score(g_shadow_chooks[i].ty);
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    if (best < 0) return NULL;
    return &g_shadow_chooks[best];
}

static const char* shadow_create_hook_pick(const ShadowCreateHook* e, int arity) {
    if (!e) return NULL;
    if (arity >= 2 && e->hook2[0]) return e->hook2;
    if (e->hook1[0]) return e->hook1;
    if (e->hook2[0]) return e->hook2;
    return NULL;
}

static void shadow_create_hook_register_arity(const char* ty, int arity,
                                             const char* hook) {
    int i;
    ShadowCreateHook* e;
    char* dest;
    if (!ty || !ty[0] || !hook || !hook[0]) return;
    dest = NULL;
    for (i = 0; i < g_shadow_nchooks; i++) {
        if (strcmp(g_shadow_chooks[i].ty, ty) == 0) {
            e = &g_shadow_chooks[i];
            dest = (arity >= 2) ? e->hook2 : e->hook1;
            snprintf(dest, sizeof(e->hook1), "%s", hook);
            return;
        }
    }
    if (g_shadow_nchooks >= SHADOW_CHOOK_CAP) {
        shadow_table_full("create_hooks", SHADOW_CHOOK_CAP, ty);
        return;
    }
    e = &g_shadow_chooks[g_shadow_nchooks];
    memset(e, 0, sizeof(*e));
    snprintf(e->ty, sizeof(e->ty), "%s", ty);
    dest = (arity >= 2) ? e->hook2 : e->hook1;
    snprintf(dest, sizeof(e->hook1), "%s", hook);
    g_shadow_nchooks++;
}

static const char* shadow_create_hook_for_arity(const char* ty, int arity) {
    int guard = 0;
    const char* cur = ty;
    if (!ty || !ty[0]) return NULL;
    while (guard++ < 8) {
        const ShadowCreateHook* e = shadow_create_hook_entry(cur);
        const char* hook = shadow_create_hook_pick(e, arity);
        const char* next;
        if (hook && hook[0]) {
            if (!shadow_hook_is_suffix(hook)) return hook;
            shadow_hook_expand(cur, hook, g_shadow_chook_exp,
                               sizeof(g_shadow_chook_exp));
            return g_shadow_chook_exp[0] ? g_shadow_chook_exp : NULL;
        }
        next = shadow_td_alias_one(cur);
        if (!next || !next[0] || strcmp(next, cur) == 0) break;
        cur = next;
    }
    return NULL;
}

static void shadow_niche_register(const char* ty, unsigned size, unsigned align,
                                  unsigned off, unsigned width,
                                  unsigned long long sentinel) {
    int i;
    if (!ty || !ty[0] || !width) return;
    if (g_shadow_nniches >= SHADOW_NICHE_CAP) {
        shadow_table_full("niches", SHADOW_NICHE_CAP, ty);
        return;
    }
    for (i = 0; i < g_shadow_nniches; i++) {
        if (strcmp(g_shadow_niches[i].ty, ty) == 0) {
            g_shadow_niches[i].size = size;
            g_shadow_niches[i].align = align;
            g_shadow_niches[i].off = off;
            g_shadow_niches[i].width = width;
            g_shadow_niches[i].sentinel = sentinel;
            return;
        }
    }
    snprintf(g_shadow_niches[g_shadow_nniches].ty,
             sizeof(g_shadow_niches[0].ty), "%s", ty);
    g_shadow_niches[g_shadow_nniches].size = size;
    g_shadow_niches[g_shadow_nniches].align = align;
    g_shadow_niches[g_shadow_nniches].off = off;
    g_shadow_niches[g_shadow_nniches].width = width;
    g_shadow_niches[g_shadow_nniches].sentinel = sentinel;
    g_shadow_nniches++;
}

/* Does `text` declare `name` with a Result return?  Decl-shaped probe:
 * an occurrence of `name(` whose declaration head (this line, or this
 * line plus the previous for a wrapped head) spells `CCResult_` or
 * `T !>(E)` with no `=` after it — an assignment from the callee is a
 * use, not a decl. */
static int shadow_text_fn_returns_result(const char* text, const char* name) {
    const char* p = text;
    size_t nl;
    if (!text || !name || !name[0]) return 0;
    nl = strlen(name);
    while ((p = strstr(p, name)) != NULL) {
        const char* q = p + nl;
        const char* w = p;
        if ((p > text && shadow_is_ident_char(p[-1])) ||
            shadow_is_ident_char(*q)) {
            p += nl;
            continue;
        }
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '(') {
            p += nl;
            continue;
        }
        {
            int nls = 0;
            while (w > text && (size_t)(p - w) < 220) {
                if (w[-1] == '\n' && ++nls == 2) break;
                w--;
            }
        }
        {
            char head[224];
            const char* rr;
            size_t hn = (size_t)(p - w);
            if (hn >= sizeof(head)) hn = sizeof(head) - 1;
            memcpy(head, w, hn);
            head[hn] = 0;
            rr = strstr(head, "CCResult_");
            if (rr && !strchr(rr, '=')) return 1;
            /* .cch surface: `CCPyObj !>(CCPyError) cc_py_obj_callm(` */
            rr = strstr(head, "!>(");
            if (rr && !strchr(rr, '=')) return 1;
        }
        p += nl;
    }
    return 0;
}

static void shadow_dyn_sink_register(const char* ty, const char* callee,
                                     const char* wrap, int dest_aware,
                                     int returns_result) {
    int i;
    if (!ty || !ty[0] || !callee || !callee[0] || !wrap || !wrap[0]) return;
    if (g_shadow_ndsinks >= SHADOW_DSINK_CAP) {
        shadow_table_full("dyn_sinks", SHADOW_DSINK_CAP, ty);
        return;
    }
    for (i = 0; i < g_shadow_ndsinks; i++) {
        if (strcmp(g_shadow_dsinks[i].ty, ty) == 0) {
            snprintf(g_shadow_dsinks[i].callee, sizeof(g_shadow_dsinks[0].callee),
                     "%s", callee);
            snprintf(g_shadow_dsinks[i].wrap, sizeof(g_shadow_dsinks[0].wrap),
                     "%s", wrap);
            g_shadow_dsinks[i].dest_aware = dest_aware;
            g_shadow_dsinks[i].returns_result = returns_result;
            return;
        }
    }
    snprintf(g_shadow_dsinks[g_shadow_ndsinks].ty, sizeof(g_shadow_dsinks[0].ty),
             "%s", ty);
    snprintf(g_shadow_dsinks[g_shadow_ndsinks].callee,
             sizeof(g_shadow_dsinks[0].callee), "%s", callee);
    snprintf(g_shadow_dsinks[g_shadow_ndsinks].wrap,
             sizeof(g_shadow_dsinks[0].wrap), "%s", wrap);
    g_shadow_dsinks[g_shadow_ndsinks].dest_aware = dest_aware;
    g_shadow_dsinks[g_shadow_ndsinks].returns_result = returns_result;
    g_shadow_ndsinks++;
}

static const ShadowDynSink* shadow_dyn_sink_for(const char* ty) {
    int i;
    if (!ty || !ty[0]) return NULL;
    for (i = 0; i < g_shadow_ndsinks; i++) {
        if (strcmp(g_shadow_dsinks[i].ty, ty) == 0)
            return &g_shadow_dsinks[i];
    }
    return NULL;
}

/* Header-visible callables: the production resolver's own oracle,
 * linked in via libshadow_comptime.  The ufn table above holds
 * TU-defined functions only, which left compose-then-verify sites blind
 * to header-installed families (cc_py_obj_callm_double,
 * cc_js_callm_int64_t, …) and grew per-header whitelists in the faces.
 * One index answers both the driver and the faces, so the two can never
 * disagree about what a header installs. */
extern int cc_included_cch_contains_fn(const char* name);
extern int cc_included_cch_declares_fn(const char* name);
extern int cc_lowered_local_declares_fn(const char* name);
extern int cc_included_cch_fn_first_param(const char* name, char* out,
                                          size_t out_sz);
extern int cc_included_cch_fn_param(const char* name, int argi, char* out,
                                    size_t out_sz);
extern int cc_included_cch_each_fn_param(int (*cb)(const char* name, int argi,
                                                   const char* ty, void* ctx),
                                         void* ctx);
extern int cc_result_fn_registry_get_err_type(const char* name, size_t name_len,
                                              char* out_buf, size_t out_sz);
extern int cc_result_fn_registry_contains(const char* name, size_t len);
extern int cc_result_fn_registry_get_result_type(const char* name,
                                                 size_t name_len, char* out_buf,
                                                 size_t out_sz);
extern const char* cc_result_fn_registry_name_at(size_t i);
extern const char* cc_result_fn_registry_err_type_at(size_t i);
extern const char* cc_result_fn_registry_result_type_at(size_t i);
extern size_t cc_result_fn_registry_count(void);
extern size_t cc_included_cch_source_count(void);
extern const char* cc_included_cch_source_path(size_t i);
extern size_t cc_lowered_local_header_count(void);
extern const char* cc_lowered_local_header_source_path(size_t i);

/* True when `name(` appears as a decl-shaped callable in a lowered local
 * `.cch` (quoted project header). Those bodies are not in the TU AST, so
 * autoblock / UFCS must consult the original source the lowerer registered. */
static int shadow_hdr_fn_exists(const char* name) {
    if (!name || !name[0]) return 0;
    if (cc_included_cch_contains_fn(name)) return 1;
    return cc_lowered_local_declares_fn(name);
}


/* True when `fty` is a value embed (not pointer / array / fn-ptr).
 * Writes the base type name (const/struct stripped) into `base`. */
static int shadow_field_ty_is_value_embed(const char* fty, char* base,
                                          size_t bcap) {
    int stars = 0;
    if (!base || !bcap) return 0;
    base[0] = 0;
    if (!fty || !fty[0]) return 0;
    if (strchr(fty, '[') || strchr(fty, '(')) return 0;
    shadow_ty_base_stars(fty, base, bcap, &stars);
    return base[0] && stars == 0;
}

/* draft_as §3: this type's pre/destroy, then every value field whose type
 * has hooks, last-declared to first (recursive). `as:` is not required —
 * an is-a face is destroyed only because it is a value embed. Pointer and
 * array fields are not owned. skip_pre: caller already emitted this type's
 * pre (user @destroy body sits between pre and hook). Nested embeds pass 0. */
static int shadow_embed_destroy_append_rec(char* body, size_t* bo, size_t cap,
                                           const char* ty, const char* path,
                                           int skip_pre, const char** visited,
                                           int visited_n, int visited_cap) {
    int emitted = 0;
    int idxs[32];
    int ni = 0;
    int vi, k;
    size_t fi, nfields;
    const ShadowDestroyHook* dh;
    const char* resolved;
    if (!body || !bo || !ty || !ty[0] || !path || !path[0]) return 0;
    if (*bo + 48 >= cap) return 0;
    resolved = shadow_td_alias_resolve(ty);
    if (!resolved || !resolved[0]) resolved = ty;
    for (vi = 0; vi < visited_n; vi++) {
        if (visited[vi] && (strcmp(visited[vi], ty) == 0 ||
                            strcmp(visited[vi], resolved) == 0))
            return 0;
    }
    if (visited_n >= visited_cap) return 0;
    visited[visited_n] = resolved;
    dh = shadow_destroy_hooks_for(ty);
    if (!skip_pre && dh && dh->pre[0]) {
        *bo += (size_t)snprintf(body + *bo, cap - *bo, "%s(&%s); ", dh->pre,
                                path);
        emitted = 1;
    }
    if (dh && dh->hook[0]) {
        *bo += (size_t)snprintf(body + *bo, cap - *bo, "%s(&%s); ", dh->hook,
                                path);
        emitted = 1;
    } else if (!dh && strcmp(ty, "CCFile") == 0) {
        /* io.cch may not be warmed; conventional CCFile destroy. */
        *bo += (size_t)snprintf(body + *bo, cap - *bo, "cc_file_close(&%s); ",
                                path);
        emitted = 1;
    }
    if (!shadow_fields_ensure()) return emitted;
    nfields = ShadowFieldVec_len(&g_shadow_fields);
    for (fi = 0; fi < nfields && ni < 32; fi++) {
        ShadowStructField* f = ShadowFieldVec_get(&g_shadow_fields, fi);
        char base[96];
        if (!f || !f->ty ||
            (strcmp(f->ty, ty) != 0 && strcmp(f->ty, resolved) != 0))
            continue;
        if (!f->field || !f->field[0] || strchr(f->field, '.')) continue;
        if (!shadow_field_ty_is_value_embed(f->field_ty, base, sizeof(base)))
            continue;
        idxs[ni++] = (int)fi;
    }
    for (k = ni - 1; k >= 0; k--) {
        ShadowStructField* f = ShadowFieldVec_get(&g_shadow_fields, idxs[k]);
        char base[96];
        char sub[192];
        int n;
        if (!f) continue;
        if (*bo + 64 >= cap) break;
        if (!shadow_field_ty_is_value_embed(f->field_ty, base, sizeof(base)))
            continue;
        snprintf(sub, sizeof(sub), "%s.%s", path, f->field);
        n = shadow_embed_destroy_append_rec(body, bo, cap, base, sub, 0,
                                           visited, visited_n + 1,
                                           visited_cap);
        if (n > 0) emitted += n;
    }
    return emitted;
}

static int shadow_embed_destroy_append(char* body, size_t* bo, size_t cap,
                                       const char* ty, const char* path,
                                       int skip_pre) {
    const char* visited[32];
    return shadow_embed_destroy_append_rec(body, bo, cap, ty, path, skip_pre,
                                          visited, 0, 32);
}

static int shadow_slice_elem_is_prelude(const char* t) {
    return t && (!strcmp(t, "int") || !strcmp(t, "double") ||
                 !strcmp(t, "float") || !strcmp(t, "char") ||
                 !strcmp(t, "bool") || !strcmp(t, "size_t") ||
                 !strcmp(t, "long") || !strcmp(t, "long_long") ||
                 !strcmp(t, "short") || !strcmp(t, "int16_t") ||
                 !strcmp(t, "int32_t") || !strcmp(t, "int64_t") ||
                 !strcmp(t, "uint16_t") || !strcmp(t, "uint32_t") ||
                 !strcmp(t, "uint64_t"));
}

/* Hand-written CC_DECL_SLICE[_SPEC] already in the TU — suppress auto-splice. */
enum { SHADOW_SLICE_HAVE_CAP = 64 };
static char g_shadow_slice_have[SHADOW_SLICE_HAVE_CAP][64];
static int g_shadow_nslice_have;

static void shadow_slice_mangle_elem(const char* elem, char* dst, size_t cap);
static void shadow_ginst_need(const char* family, const char* compact);
static void shadow_ginst_need_ex(const char* family, const char* compact,
                                 const char args[][64], int nargs);
static void shadow_ginst_need_from_type(const char* ty);
static void shadow_vec_need(const char* mangled_t);

static void shadow_slice_have(const char* elem) {
    int i;
    if (!elem || !elem[0]) return;
    if (g_shadow_nslice_have >= SHADOW_SLICE_HAVE_CAP) {
        shadow_table_full("slice_have", SHADOW_SLICE_HAVE_CAP, elem);
        return;
    }
    for (i = 0; i < g_shadow_nslice_have; i++) {
        if (strcmp(g_shadow_slice_have[i], elem) == 0) return;
    }
    snprintf(g_shadow_slice_have[g_shadow_nslice_have],
             sizeof(g_shadow_slice_have[0]), "%s", elem);
    g_shadow_nslice_have++;
    {
        char mangled[64];
        char ty[96];
        shadow_slice_mangle_elem(elem, mangled, sizeof(mangled));
        if (mangled[0]) {
            snprintf(ty, sizeof(ty), "CCSlice_%s", mangled);
            shadow_field_register_ex(ty, "base", "CCSlice");
            shadow_as_materialize_globs_for(ty);
        }
    }
}

static int shadow_slice_already(const char* elem) {
    int i;
    if (!elem || !elem[0]) return 0;
    if (shadow_slice_elem_is_prelude(elem)) return 1;
    for (i = 0; i < g_shadow_nslice_have; i++) {
        if (strcmp(g_shadow_slice_have[i], elem) == 0) return 1;
    }
    return 0;
}

static void shadow_slice_mangle_elem(const char* elem, char* dst, size_t cap) {
    size_t i, o = 0;
    if (!dst || !cap) return;
    dst[0] = 0;
    if (!elem) return;
    for (i = 0; elem[i] && o + 1 < cap; i++) {
        if (elem[i] == ' ' || elem[i] == '\t') {
            if (o && dst[o - 1] != '_') dst[o++] = '_';
        } else
            dst[o++] = elem[i];
    }
    dst[o] = 0;
}

static void shadow_slice_need(const char* elem) {
    char mangled[64];
    if (!elem || !elem[0]) return;
    shadow_slice_mangle_elem(elem, mangled, sizeof(mangled));
    if (!mangled[0] || shadow_slice_already(mangled)) return;
    /* Multi-word elements need CC_DECL_SLICE_SPEC — hand-written or skip. */
    if (strchr(elem, ' ') || strchr(elem, '\t')) return;
    shadow_ginst_need("CCSlice", mangled);
}

/* Note CC_DECL_SLICE(T) / CC_DECL_SLICE_SPEC(CCSlice_T, T) from RAW_LINE. */
static void shadow_slice_note_raw(const char* line) {
    const char* p;
    char elem[64];
    size_t ni = 0;
    if (!line) return;
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, "CC_DECL_SLICE_SPEC", 17) == 0) {
        p = strchr(line, ',');
        if (!p) return;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        while (*p && *p != ')' && ni + 1 < sizeof(elem)) {
            if (*p == ' ' || *p == '\t') {
                if (ni && elem[ni - 1] != '_') elem[ni++] = '_';
                p++;
                continue;
            }
            elem[ni++] = *p++;
        }
        elem[ni] = 0;
        while (ni && elem[ni - 1] == '_') elem[--ni] = 0;
        if (elem[0]) shadow_slice_have(elem);
        return;
    }
    if (strncmp(line, "CC_DECL_SLICE", 13) != 0) return;
    if (line[13] == '_') return; /* CC_DECL_SLICE_SPEC already handled */
    p = strchr(line, '(');
    if (!p) return;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != ')' && *p != ',' && ni + 1 < sizeof(elem))
        elem[ni++] = *p++;
    elem[ni] = 0;
    if (elem[0]) shadow_slice_have(elem);
}

/* Hand-written CC_MAP_DECL_ARENA(..., Map_K_V, ...) suppresses Map factory splice. */
enum { SHADOW_MAP_HAVE_CAP = 64 };
static char g_shadow_map_have[SHADOW_MAP_HAVE_CAP][64];
static int g_shadow_nmap_have;

static void shadow_map_have(const char* compact) {
    int i;
    if (!compact || !compact[0]) return;
    if (g_shadow_nmap_have >= SHADOW_MAP_HAVE_CAP) {
        shadow_table_full("map_have", SHADOW_MAP_HAVE_CAP, compact);
        return;
    }
    for (i = 0; i < g_shadow_nmap_have; i++) {
        if (strcmp(g_shadow_map_have[i], compact) == 0) return;
    }
    snprintf(g_shadow_map_have[g_shadow_nmap_have],
             sizeof(g_shadow_map_have[0]), "%s", compact);
    g_shadow_nmap_have++;
}

static int shadow_map_already(const char* compact) {
    int i;
    if (!compact || !compact[0]) return 0;
    for (i = 0; i < g_shadow_nmap_have; i++) {
        if (strcmp(g_shadow_map_have[i], compact) == 0) return 1;
    }
    return 0;
}

static void shadow_map_note_raw(const char* line) {
    const char* p;
    char name[64];
    size_t ni = 0;
    int arg = 0;
    int depth = 0;
    if (!line) return;
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, "CC_MAP_DECL_ARENA", 16) != 0) return;
    p = strchr(line, '(');
    if (!p) return;
    p++;
    for (; *p; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') {
            if (depth == 0) break;
            depth--;
        } else if (*p == ',' && depth == 0) {
            arg++;
            if (arg == 2) {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                while (*p && *p != ')' && *p != ',' && *p != ' ' &&
                       *p != '\t' && ni + 1 < sizeof(name))
                    name[ni++] = *p++;
                name[ni] = 0;
                if (strncmp(name, "Map_", 4) == 0 && name[4])
                    shadow_map_have(name + 4);
                return;
            }
        }
    }
}

/* Hand-written CC_ARRAY_MAP_DECL(..., ArrayMap_K_V, ...) suppresses splice. */
enum { SHADOW_AMAP_HAVE_CAP = 64 };
static char g_shadow_amap_have[SHADOW_AMAP_HAVE_CAP][64];
static int g_shadow_namap_have;

static void shadow_amap_have(const char* compact) {
    int i;
    if (!compact || !compact[0]) return;
    if (g_shadow_namap_have >= SHADOW_AMAP_HAVE_CAP) {
        shadow_table_full("amap_have", SHADOW_AMAP_HAVE_CAP, compact);
        return;
    }
    for (i = 0; i < g_shadow_namap_have; i++) {
        if (strcmp(g_shadow_amap_have[i], compact) == 0) return;
    }
    snprintf(g_shadow_amap_have[g_shadow_namap_have],
             sizeof(g_shadow_amap_have[0]), "%s", compact);
    g_shadow_namap_have++;
}

static int shadow_amap_already(const char* compact) {
    int i;
    if (!compact || !compact[0]) return 0;
    for (i = 0; i < g_shadow_namap_have; i++) {
        if (strcmp(g_shadow_amap_have[i], compact) == 0) return 1;
    }
    return 0;
}

static void shadow_amap_note_raw(const char* line) {
    const char* p;
    char name[64];
    size_t ni = 0;
    int arg = 0;
    int depth = 0;
    if (!line) return;
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, "CC_ARRAY_MAP_DECL", 17) != 0) return;
    if (line[17] == '_') return; /* CC_ARRAY_MAP_DECL_UFCS */
    p = strchr(line, '(');
    if (!p) return;
    p++;
    for (; *p; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') {
            if (depth == 0) break;
            depth--;
        } else if (*p == ',' && depth == 0) {
            arg++;
            if (arg == 2) {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                while (*p && *p != ')' && *p != ',' && *p != ' ' &&
                       *p != '\t' && ni + 1 < sizeof(name))
                    name[ni++] = *p++;
                name[ni] = 0;
                if (strncmp(name, "ArrayMap_", 9) == 0 && name[9])
                    shadow_amap_have(name + 9);
                return;
            }
        }
    }
}

static void shadow_ufn_register(const char* name, const char* first_ty) {
    ShadowUfcsFn* existing;
    ShadowUfcsFn v;
    CCSliceHdr k;
    if (!name || !name[0]) return;
    shadow_meta_ar_ensure();
    if (!g_shadow_ufns) return;
    k = shadow_meta_name_key(name);
    existing = ShadowUfcsFnMap_get_ptr(g_shadow_ufns, k);
    if (existing) {
        if (first_ty)
            snprintf(existing->first_ty, sizeof(existing->first_ty), "%s",
                     first_ty);
        return;
    }
    memset(&v, 0, sizeof(v));
    snprintf(v.name, sizeof(v.name), "%s", name);
    snprintf(v.first_ty, sizeof(v.first_ty), "%s", first_ty ? first_ty : "");
    k = shadow_meta_intern_name(name);
    if (!k.ptr) {
        static int reported;
        if (!reported) {
            fprintf(stderr,
                    "error: UFCS callable table grow failed (OOM); later "
                    "methods will not resolve\n");
            reported = 1;
            g_shadow_ufcs_miss = 1;
        }
        return;
    }
    if (ShadowUfcsFnMap_insert(g_shadow_ufns, k, v) != 0) {
        static int reported;
        if (!reported) {
            fprintf(stderr,
                    "error: UFCS callable map grow failed (OOM); later "
                    "methods will not resolve\n");
            reported = 1;
            g_shadow_ufcs_miss = 1;
        }
    }
}

/* Mirror production cc__record_map_decl_ufcs for CC_MAP_DECL_* beachhead. */
static void shadow_map_ufcs_install(const char* map_name) {
    static const char* methods[] = {
        "insert", "put", "get", "get_ptr", "remove", "del", "clear", "destroy",
        "len", "cap", "live_bytes", "at_ptr", "key_ptr"
    };
    char callee[160];
    char fty[96];
    size_t i;
    if (!map_name || !map_name[0]) return;
    snprintf(fty, sizeof(fty), "%s*", map_name);
    for (i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        snprintf(callee, sizeof(callee), "%s_%s", map_name, methods[i]);
        shadow_ufn_register(callee, fty);
    }
}

/* Register `Mangled_meth` definitions from a factory splice. Macro-only
 * families (Map / ArrayMap DECL) have no `Name_insert(` in the fragment —
 * those members come from the header `##_` set. Constructors whose first
 * parameter is not `Mangled` are skipped. */
static void shadow_ginst_ufcs_harvest_body(const char* mangled,
                                          const char* body) {
    const char* p;
    size_t ml;
    if (!mangled || !mangled[0] || !body) return;
    ml = strlen(mangled);
    for (p = body; *p; p++) {
        const char* meth;
        const char* argp;
        char callee[160];
        char fty[96];
        size_t ni = 0;
        if (p > body && shadow_is_ident_char(p[-1])) continue;
        if (strncmp(p, mangled, ml) != 0 || p[ml] != '_') continue;
        meth = p + ml + 1;
        if (!shadow_is_ident_char(*meth)) continue;
        while (shadow_is_ident_char(meth[ni])) ni++;
        if (meth[ni] != '(' || ni == 0) continue;
        if (ml + 1 + ni >= sizeof(callee)) continue;
        memcpy(callee, p, ml + 1 + ni);
        callee[ml + 1 + ni] = 0;
        argp = meth + ni + 1;
        for (;;) {
            while (*argp == ' ' || *argp == '\t' || *argp == '\n' ||
                   *argp == '\r')
                argp++;
            if (strncmp(argp, "const ", 6) == 0)
                argp += 6;
            else if (strncmp(argp, "volatile ", 9) == 0)
                argp += 9;
            else if (strncmp(argp, "struct ", 7) == 0)
                argp += 7;
            else
                break;
        }
        if (strncmp(argp, mangled, ml) != 0) continue;
        argp += ml;
        while (*argp == ' ' || *argp == '\t') argp++;
        if (*argp == '*')
            snprintf(fty, sizeof(fty), "%s*", mangled);
        else
            snprintf(fty, sizeof(fty), "%s", mangled);
        shadow_ufn_register(callee, fty);
    }
}

/* @grammar(schema) types queued during stage1 splice — register Type_method
 * UFCS targets even when matchers live in AST_RAW_LINE tape passthrough. */
extern int cc_grammar_pending_ufcs_type_count(void);
extern const char* cc_grammar_pending_ufcs_type(int i);
extern int cc_grammar_pending_ufcs_field_count(void);
extern const char* cc_grammar_pending_ufcs_field_type(int i);
extern const char* cc_grammar_pending_ufcs_field_name(int i);
extern const char* cc_grammar_pending_ufcs_field_fty(int i);
extern int cc_grammar_ufcs_table_overflow(void);

static void shadow_grammar_ufcs_install(void) {
    static const char* methods[] = {
        "match", "parse", "read", "try_read", "write", "measure", "to_str",
        "reader", "get", "next", "at_end", "_fill", "__wmeasure", "__wput"
    };
    int nt = cc_grammar_pending_ufcs_type_count();
    int nf = cc_grammar_pending_ufcs_field_count();
    int ti, fi;
    size_t mi;
    if (cc_grammar_ufcs_table_overflow()) {
        g_shadow_table_overflow = 1;
        g_shadow_ufcs_miss = 1;
    }
    for (ti = 0; ti < nt; ti++) {
        const char* type = cc_grammar_pending_ufcs_type(ti);
        char callee[160];
        char fty[96];
        if (!type || !type[0]) continue;
        snprintf(fty, sizeof(fty), "const %s*", type);
        for (mi = 0; mi < sizeof(methods) / sizeof(methods[0]); mi++) {
            snprintf(callee, sizeof(callee), "%s_%s", type, methods[mi]);
            shadow_ufn_register(callee, fty);
        }
        snprintf(fty, sizeof(fty), "%s*", type);
        for (mi = 0; mi < sizeof(methods) / sizeof(methods[0]); mi++) {
            snprintf(callee, sizeof(callee), "%s_%s", type, methods[mi]);
            shadow_ufn_register(callee, fty);
        }
    }
    /* Bind fields (incl. union-arm binds flattened onto the schema type). */
    for (fi = 0; fi < nf; fi++) {
        const char* type = cc_grammar_pending_ufcs_field_type(fi);
        const char* field = cc_grammar_pending_ufcs_field_name(fi);
        const char* fty = cc_grammar_pending_ufcs_field_fty(fi);
        if (!type || !field || !fty) continue;
        shadow_field_register_ex(type, field, fty);
    }
}

extern int cc_variant_schema_pending_count(void);
extern const char* cc_variant_schema_pending_name(int i);
extern int cc_variant_schema_pending_narms(int i);
extern const char* cc_variant_schema_pending_arm(int i, int a);
extern int cc_variant_schema_pending_arm_is_void(int i, int a);

static void shadow_grammar_variant_install(void) {
    int ni = cc_variant_schema_pending_count();
    int i, a;
    for (i = 0; i < ni; i++) {
        const char* name = cc_variant_schema_pending_name(i);
        int narm = cc_variant_schema_pending_narms(i);
        char arms[8][64];
        char tys[8][96];
        int is_void[8];
        if (!name || !name[0] || narm <= 0) continue;
        if (shadow_variant_find(name)) continue;
        memset(tys, 0, sizeof(tys));
        memset(is_void, 0, sizeof(is_void));
        for (a = 0; a < narm && a < 8; a++) {
            snprintf(arms[a], sizeof(arms[0]), "%s",
                     cc_variant_schema_pending_arm(i, a));
            is_void[a] = cc_variant_schema_pending_arm_is_void(i, a);
        }
        shadow_variant_register_full(name, arms, tys, is_void, narm, 0);
    }
}

/* Parse Name out of CC_MAP_DECL_UFCS(Name) or CC_MAP_DECL_ARENA(K,V,Name,…). */
static void shadow_map_ufcs_install_from_raw(const char* line) {
    const char* p;
    char name[64];
    size_t ni = 0;
    int arg = 0;
    int depth = 0;
    if (!line) return;
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, "CC_MAP_DECL_UFCS", 15) == 0) {
        p = strchr(line, '(');
        if (!p) return;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        while (*p && *p != ')' && *p != ',' && ni + 1 < sizeof(name))
            name[ni++] = *p++;
        name[ni] = 0;
        if (name[0]) shadow_map_ufcs_install(name);
        return;
    }
    if (strncmp(line, "CC_MAP_DECL_ARENA", 16) != 0 &&
        strncmp(line, "CC_ARRAY_MAP_DECL", 17) != 0)
        return;
    if (line[0] == 'C' && line[17] == '_')
        return; /* CC_ARRAY_MAP_DECL_UFCS */
    p = strchr(line, '(');
    if (!p) return;
    p++;
    /* 3rd arg is Name. */
    for (; *p; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') {
            if (depth == 0) break;
            depth--;
        } else if (*p == ',' && depth == 0) {
            arg++;
            if (arg == 2) {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                while (*p && *p != ')' && *p != ',' && *p != ' ' &&
                       *p != '\t' && ni + 1 < sizeof(name))
                    name[ni++] = *p++;
                name[ni] = 0;
                if (name[0]) shadow_map_ufcs_install(name);
                return;
            }
        }
    }
}

static int shadow_ufn_exists(const char* name) {
    if (!name || !name[0]) return 0;
    shadow_meta_ar_ensure();
    if (!g_shadow_ufns) return 0;
    return ShadowUfcsFnMap_get_ptr(g_shadow_ufns, shadow_meta_name_key(name)) !=
           NULL;
}

/* TU harvest or a header prototype/definition. Family compose for a
 * linked-TU method is `Type_meth` in the .cch — that is a declare, not a
 * static inline wrapper. */
static int shadow_ufcs_callee_declared(const char* name) {
    if (!name || !name[0]) return 0;
    if (shadow_ufn_exists(name)) return 1;
    if (cc_included_cch_declares_fn(name)) return 1;
    if (cc_lowered_local_declares_fn(name)) return 1;
    return 0;
}

static const char* shadow_ufn_first_ty(const char* name) {
    ShadowUfcsFn* fn;
    if (!name || !name[0]) return NULL;
    shadow_meta_ar_ensure();
    if (!g_shadow_ufns) return NULL;
    fn = ShadowUfcsFnMap_get_ptr(g_shadow_ufns, shadow_meta_name_key(name));
    return fn ? fn->first_ty : NULL;
}

/* Unique `Type_meth` (or bare `meth`) in the ufn table.  Skips `store_*`
 * beachheads.  Used when the receiver bind is missing (opaque string-switch
 * / raw bodies) so get/put/delete still lower.  When several `*_meth` exist
 * (ArrayMap_*_get plus cc_shard_map_get), prefer the single `cc_*` hit. */
static const char* shadow_ufn_unique_for_meth(const char* meth) {
    const char* hit = NULL;
    const char* cc_hit = NULL;
    int n = 0;
    int ncc = 0;
    size_t mlen;
    CCSliceHdr k;
    ShadowUfcsFn v;
    if (!meth || !meth[0]) return NULL;
    shadow_meta_ar_ensure();
    if (!g_shadow_ufns) return NULL;
    mlen = strlen(meth);
    CC_MAP_FOREACH(g_shadow_ufns, k, v) {
        const char* nm = v.name;
        ShadowUfcsFn* live;
        size_t nl;
        int match = 0;
        if (!nm || !nm[0]) continue;
        if (strncmp(nm, "store_", 6) == 0) continue;
        nl = strlen(nm);
        if (strcmp(nm, meth) == 0) match = 1;
        else if (nl > mlen + 1 && nm[nl - mlen - 1] == '_' &&
                 strcmp(nm + (nl - mlen), meth) == 0)
            match = 1;
        if (!match) continue;
        /* FOREACH copies V — pin the live row for a stable name pointer. */
        live = ShadowUfcsFnMap_get_ptr(g_shadow_ufns, k);
        if (!live) continue;
        hit = live->name;
        n++;
        if (strncmp(live->name, "cc_", 3) == 0) {
            cc_hit = live->name;
            ncc++;
        }
    }
    if (n == 1) return hit;
    if (ncc == 1) return cc_hit;
    return NULL;
}

static void shadow_td_alias_register(const char* alias, const char* base) {
    int i;
    if (!alias || !alias[0] || !base || !base[0]) return;
    if (g_shadow_ntd >= SHADOW_TD_ALIAS_CAP) {
        shadow_table_full("td_alias", SHADOW_TD_ALIAS_CAP, alias);
        return;
    }
    for (i = 0; i < g_shadow_ntd; i++) {
        if (strcmp(g_shadow_td_alias[i], alias) == 0) {
            snprintf(g_shadow_td_base[i], sizeof(g_shadow_td_base[0]), "%s",
                     base);
            return;
        }
    }
    snprintf(g_shadow_td_alias[g_shadow_ntd], sizeof(g_shadow_td_alias[0]), "%s",
             alias);
    snprintf(g_shadow_td_base[g_shadow_ntd], sizeof(g_shadow_td_base[0]), "%s",
             base);
    g_shadow_ntd++;
}

static const char* shadow_td_alias_one(const char* ty) {
    int i;
    if (!ty || !ty[0]) return ty;
    for (i = 0; i < g_shadow_ntd; i++) {
        if (strcmp(g_shadow_td_alias[i], ty) == 0)
            return g_shadow_td_base[i];
    }
    return ty;
}

static const char* shadow_td_alias_resolve(const char* ty) {
    int guard = 0;
    const char* cur = ty;
    if (!ty || !ty[0]) return ty;
    while (guard++ < 8) {
        const char* next = shadow_td_alias_one(cur);
        if (!next || !next[0] || strcmp(next, cur) == 0) return cur;
        cur = next;
    }
    return cur;
}

/* True when `tag` is a `typedef struct tag Alias` base (after bind resolve). */
static int shadow_td_alias_is_base(const char* tag) {
    int i;
    const char* t;
    if (!tag || !tag[0]) return 0;
    t = tag;
    if (strncmp(t, "struct ", 7) == 0) t += 7;
    while (*t == ' ' || *t == '\t') t++;
    for (i = 0; i < g_shadow_ntd; i++) {
        const char* b = g_shadow_td_base[i];
        size_t n = 0;
        if (strncmp(b, "struct ", 7) == 0) b += 7;
        while (*b == ' ' || *b == '\t') b++;
        while (b[n] && b[n] != '*' && b[n] != ' ' && b[n] != '\t') n++;
        if (n && strncmp(b, t, n) == 0 && t[n] == 0) return 1;
    }
    return 0;
}

/* Binds of `typedef struct tag Alias` resolve to the tag so field UFCS
 * walks tag rows. Type_meth is still spelled `Alias_meth` — try tag_meth,
 * then every typedef that aliases this tag. */
static int shadow_td_alias_ufn_meth(const char* tag, const char* meth,
                                    char* out, size_t cap) {
    int i;
    char tryc[160];
    char base[64];
    const char* t;
    size_t n;
    if (!tag || !tag[0] || !meth || !meth[0] || !out || !cap) return 0;
    t = tag;
    if (strncmp(t, "struct ", 7) == 0) t += 7;
    while (*t == ' ' || *t == '\t') t++;
    snprintf(tryc, sizeof(tryc), "%s_%s", t, meth);
    if (shadow_ufcs_callee_declared(tryc)) {
        snprintf(out, cap, "%s", tryc);
        return 1;
    }
    for (i = 0; i < g_shadow_ntd; i++) {
        const char* b = g_shadow_td_base[i];
        if (strncmp(b, "struct ", 7) == 0) b += 7;
        while (*b == ' ' || *b == '\t') b++;
        n = 0;
        while (b[n] && b[n] != '*' && b[n] != ' ' && n + 1 < sizeof(base)) {
            base[n] = b[n];
            n++;
        }
        base[n] = 0;
        if (!base[0] ||
            (strcmp(base, t) != 0 && strcmp(g_shadow_td_base[i], tag) != 0))
            continue;
        snprintf(tryc, sizeof(tryc), "%s_%s", g_shadow_td_alias[i], meth);
        if (shadow_ufcs_callee_declared(tryc)) {
            snprintf(out, cap, "%s", tryc);
            return 1;
        }
    }
    return 0;
}

/* Full-chain destructor: `Type_destroy` (or a typehooks hook of that name).
 * Delta hooks (`cc_temp_file_unlink`, `outer_dtor`) do not match — those
 * still flatten value embeds. */
static int shadow_type_destroy_fn(const char* ty, char* out, size_t cap) {
    char base[160];
    char resolved_base[160];
    char tryc[160];
    int stars = 0;
    const ShadowDestroyHook* dh;
    const char* resolved;
    if (!ty || !ty[0] || !out || !cap) return 0;
    shadow_ty_base_stars(ty, base, sizeof(base), &stars);
    if (!base[0]) return 0;
    resolved = shadow_td_alias_resolve(base);
    if (!resolved || !resolved[0]) resolved = base;
    shadow_ty_base_stars(resolved, resolved_base, sizeof(resolved_base), &stars);
    if (!resolved_base[0]) snprintf(resolved_base, sizeof(resolved_base), "%s",
                                    resolved);
    snprintf(tryc, sizeof(tryc), "%s_destroy", resolved_base);
    dh = shadow_destroy_hooks_for(base);
    if (dh && dh->hook[0]) {
        int hook_is_full = strcmp(dh->hook, tryc) == 0;
        if (!hook_is_full && strcmp(base, resolved_base) != 0) {
            char alias_try[160];
            snprintf(alias_try, sizeof(alias_try), "%s_destroy", base);
            hook_is_full = strcmp(dh->hook, alias_try) == 0;
        }
        if (hook_is_full) {
            snprintf(out, cap, "%s", dh->hook);
            return 1;
        }
    }
    if (shadow_ufn_exists(tryc) || shadow_hdr_fn_exists(tryc)) {
        snprintf(out, cap, "%s", tryc);
        return 1;
    }
    if (strcmp(base, resolved_base) != 0) {
        snprintf(tryc, sizeof(tryc), "%s_destroy", base);
        if (shadow_ufn_exists(tryc) || shadow_hdr_fn_exists(tryc)) {
            snprintf(out, cap, "%s", tryc);
            return 1;
        }
    }
    if (shadow_td_alias_ufn_meth(resolved_base, "destroy", out, cap)) return 1;
    if (shadow_td_alias_ufn_meth(base, "destroy", out, cap)) return 1;
    return 0;
}

static void shadow_bind_name(const char* name, const char* ty, int flags) {
    ShadowBind* b;
    const char* resolved;
    if (g_shadow_binds_frozen) return;
    if (!name || !name[0] || !ty) return;
    b = shadow_bind_new_slot(&g_shadow_binds, &g_shadow_binds_cap,
                             &g_shadow_nbinds, "binds", name);
    if (!b) return;
    resolved = shadow_td_alias_resolve(ty);
    /* Stack same-name binds (innermost wins via reverse lookup). Never
     * overwrite — that poisoned outer UFCS when a nested local reused a name. */
    snprintf(b->name, sizeof(b->name), "%s", name);
    snprintf(b->ty, sizeof(b->ty), "%s", resolved);
    b->flags = flags;
}

/* Strip `const`/`struct` and count trailing `*` → base name. */
static void shadow_ty_base_stars(const char* ty, char* base, size_t bcap,
                                 int* stars) {
    const char* p = ty ? ty : "";
    size_t n = 0;
    int st = 0;
    if (stars) *stars = 0;
    if (!base || !bcap) return;
    base[0] = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "const ", 6) == 0) p += 6;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "struct ", 7) == 0) p += 7;
    while (*p && *p != '*' && *p != ' ' && *p != '\t' && n + 1 < bcap)
        base[n++] = *p++;
    base[n] = 0;
    while (*p) {
        if (*p == '*') st++;
        p++;
    }
    if (stars) *stars = st;
}

/* Param-shaped binds for closure captures. Last-wins locals in
 * g_shadow_binds must not clobber `Pool* pool` when main has `Pool pool`. */
static void shadow_cap_bind_name(const char* name, const char* ty, int flags) {
    int i;
    const char* resolved;
    char nb[64], ob[64];
    int ns = 0, os = 0;
    if (!name || !name[0] || !ty) return;
    resolved = shadow_td_alias_resolve(ty);
    shadow_ty_base_stars(resolved, nb, sizeof(nb), &ns);
    for (i = 0; i < g_shadow_ncap_binds; i++) {
        ShadowBind* cur = g_shadow_cap_binds[i];
        if (!cur || strcmp(cur->name, name) != 0) continue;
        shadow_ty_base_stars(cur->ty, ob, sizeof(ob), &os);
        /* Prefer more stars so `Pool* pool` beats later `Pool pool`. */
        if (ns < os) return;
        /* Do not degrade a real type to the int fallback. */
        if (strcmp(resolved, "int") == 0 && strcmp(cur->ty, "int") != 0)
            return;
        snprintf(cur->ty, sizeof(cur->ty), "%s", resolved);
        cur->flags = flags;
        return;
    }
    {
        ShadowBind* b = shadow_bind_new_slot(&g_shadow_cap_binds,
                                            &g_shadow_cap_binds_cap,
                                            &g_shadow_ncap_binds, "cap_binds",
                                            name);
        if (!b) {
            g_shadow_cap_overflow = 1;
            return;
        }
        snprintf(b->name, sizeof(b->name), "%s", name);
        snprintf(b->ty, sizeof(b->ty), "%s", resolved);
        b->flags = flags;
    }
}

static const ShadowBind* shadow_cap_bind_lookup(const char* name) {
    int i;
    if (!name) return NULL;
    for (i = 0; i < g_shadow_ncap_binds; i++) {
        if (g_shadow_cap_binds[i] &&
            strcmp(g_shadow_cap_binds[i]->name, name) == 0)
            return g_shadow_cap_binds[i];
    }
    return NULL;
}

static void shadow_as_register(const char* outer, const char* field,
                               const char* target) {
    int i;
    if (!outer || !outer[0] || !field || !field[0] || !target || !target[0])
        return;
    if (strchr(field, ',')) {
        const char* p = field;
        char one[64];
        while (shadow_field_next_name(&p, one, sizeof(one)))
            shadow_as_register(outer, one, target);
        return;
    }
    if (g_shadow_nas >= SHADOW_AS_CAP) {
        shadow_table_full("as_embeds", SHADOW_AS_CAP, outer);
        return;
    }
    for (i = 0; i < g_shadow_nas; i++) {
        if (strcmp(g_shadow_as[i].outer, outer) == 0 &&
            strcmp(g_shadow_as[i].target, target) == 0) {
            if (strcmp(g_shadow_as[i].field, field) != 0) {
                fprintf(stderr,
                        "error: as: / typeview as: conflicting faces on '%s' "
                        "to '%s' ('%s' vs '%s')\n",
                        outer, target, g_shadow_as[i].field, field);
                g_shadow_restrict_diag = 1;
            }
            return;
        }
    }
    snprintf(g_shadow_as[g_shadow_nas].outer, sizeof(g_shadow_as[0].outer), "%s",
             outer);
    snprintf(g_shadow_as[g_shadow_nas].field, sizeof(g_shadow_as[0].field), "%s",
             field);
    snprintf(g_shadow_as[g_shadow_nas].target, sizeof(g_shadow_as[0].target),
             "%s", target);
    g_shadow_as[g_shadow_nas].mode[0] = 0;
    g_shadow_nas++;
}

static void shadow_as_register_viewed(const char* outer, const char* field,
                                     const char* target, const char* view) {
    int i;
    shadow_as_register(outer, field, target);
    if (!view || !view[0] || g_shadow_nas <= 0) return;
    for (i = g_shadow_nas - 1; i >= 0; i--) {
        if (strcmp(g_shadow_as[i].outer, outer) == 0 &&
            strcmp(g_shadow_as[i].field, field) == 0 &&
            strcmp(g_shadow_as[i].target, target) == 0) {
            snprintf(g_shadow_as[i].mode, sizeof(g_shadow_as[i].mode), "%s",
                     view);
            return;
        }
    }
}

static void shadow_as_glob_register(const char* pat, const char* field) {
    int i;
    if (!pat || !pat[0] || !field || !field[0]) return;
    for (i = 0; i < g_shadow_nas_globs; i++) {
        if (strcmp(g_shadow_as_globs[i].pat, pat) == 0 &&
            strcmp(g_shadow_as_globs[i].field, field) == 0)
            return;
    }
    if (g_shadow_nas_globs >= SHADOW_AS_GLOB_CAP) {
        shadow_table_full("as_globs", SHADOW_AS_GLOB_CAP, pat);
        return;
    }
    snprintf(g_shadow_as_globs[g_shadow_nas_globs].pat,
             sizeof(g_shadow_as_globs[0].pat), "%s", pat);
    snprintf(g_shadow_as_globs[g_shadow_nas_globs].field,
             sizeof(g_shadow_as_globs[0].field), "%s", field);
    g_shadow_as_globs[g_shadow_nas_globs].hits = 0;
    g_shadow_nas_globs++;
}

static void shadow_as_materialize_globs_for(const char* outer) {
    int gi, i, nidx = 0;
    int best = -1;
    int idxs[SHADOW_AS_GLOB_CAP];
    if (!outer || !outer[0] || g_shadow_nas_globs <= 0) return;
    for (gi = 0; gi < g_shadow_nas_globs; gi++) {
        int score;
        if (!shadow_restrict_pattern_matches(g_shadow_as_globs[gi].pat, outer))
            continue;
        score = (int)shadow_restrict_pattern_score(g_shadow_as_globs[gi].pat);
        if (score > best) {
            best = score;
            nidx = 0;
            idxs[nidx++] = gi;
        } else if (score == best && nidx < SHADOW_AS_GLOB_CAP) {
            idxs[nidx++] = gi;
        }
    }
    for (i = 0; i < nidx; i++) {
        char fty[96];
        char target[64];
        int stars = 0;
        ShadowAsGlob* g = &g_shadow_as_globs[idxs[i]];
        if (!shadow_field_ty_of(outer, g->field, fty, sizeof(fty))) continue;
        shadow_ty_base_stars(fty, target, sizeof(target), &stars);
        if (!target[0] || stars != 0) continue;
        shadow_as_register(outer, g->field, target);
        g->hits++;
    }
}

static void shadow_as_materialize_all_globs(void) {
    size_t i, n;
    if (g_shadow_nas_globs <= 0) return;
    if (!shadow_fields_ensure()) return;
    n = ShadowFieldVec_len(&g_shadow_fields);
    for (i = 0; i < n; i++) {
        size_t j;
        int dup = 0;
        ShadowStructField* f = ShadowFieldVec_get(&g_shadow_fields, i);
        const char* ty;
        if (!f || !f->ty || !f->ty[0]) continue;
        ty = f->ty;
        for (j = 0; j < i; j++) {
            ShadowStructField* prev = ShadowFieldVec_get(&g_shadow_fields, j);
            if (prev && prev->ty && strcmp(prev->ty, ty) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) continue;
        shadow_as_materialize_globs_for(ty);
    }
    for (int si = 0; si < g_shadow_nslices; si++) {
        char ty[96];
        snprintf(ty, sizeof(ty), "CCSlice_%s", g_shadow_slices[si]);
        shadow_field_register_ex(ty, "base", "CCSlice");
        shadow_as_materialize_globs_for(ty);
    }
}

static const ShadowAsEmbed* shadow_as_lookup(const char* outer,
                                             const char* target) {
    int i;
    if (!outer || !target) return NULL;
    shadow_as_materialize_globs_for(outer);
    for (i = 0; i < g_shadow_nas; i++) {
        if (strcmp(g_shadow_as[i].outer, outer) == 0 &&
            strcmp(g_shadow_as[i].target, target) == 0)
            return &g_shadow_as[i];
    }
    return NULL;
}

/* Field type of an as: face (`as: tree` → RtxPieceTree). Used when the
 * struct field table missed the member (`struct Tag name`). */
static int shadow_as_field_target(const char* outer, const char* field,
                                  char* dst, size_t cap) {
    int i;
    char base[128];
    if (!outer || !field || !dst || !cap) return 0;
    dst[0] = 0;
    shadow_as_materialize_globs_for(outer);
    for (i = 0; i < g_shadow_nas; i++) {
        if (strcmp(g_shadow_as[i].outer, outer) == 0 &&
            strcmp(g_shadow_as[i].field, field) == 0 &&
            g_shadow_as[i].target[0] &&
            strchr(g_shadow_as[i].field, '.') == NULL) {
            snprintf(dst, cap, "%s", g_shadow_as[i].target);
            return 1;
        }
    }
    if (shadow_ty_restrict_base(outer, base, sizeof(base)) && base[0] &&
        strcmp(base, outer) != 0)
        return shadow_as_field_target(base, field, dst, cap);
    return 0;
}

/* Close transitive @as paths: Outer→Mid→CCFile becomes Outer / mid.file / CCFile. */
static void shadow_as_resolve_transitive(void) {
    int guard;
    for (guard = 0; guard < 8; guard++) {
        int i, j, added = 0;
        int n = g_shadow_nas;
        for (i = 0; i < n; i++) {
            for (j = 0; j < n; j++) {
                char path[128];
                if (strcmp(g_shadow_as[i].target, g_shadow_as[j].outer) != 0)
                    continue;
                if (strlen(g_shadow_as[i].field) + 1 +
                        strlen(g_shadow_as[j].field) + 1 >=
                    sizeof(path))
                    continue;
                snprintf(path, sizeof(path), "%s.%s", g_shadow_as[i].field,
                         g_shadow_as[j].field);
                if (shadow_as_lookup(g_shadow_as[i].outer,
                                     g_shadow_as[j].target))
                    continue;
                shadow_as_register(g_shadow_as[i].outer, path,
                                   g_shadow_as[j].target);
                added = 1;
            }
        }
        if (!added) break;
    }
}

static const ShadowFnParam* shadow_fnparam_lookup(const char* fn, int argi) {
    ShadowFnParamKey k;
    if (!fn) return NULL;
    shadow_meta_ar_ensure();
    if (!g_shadow_fnparams) return NULL;
    memset(&k, 0, sizeof(k));
    snprintf(k.fn, sizeof(k.fn), "%s", fn);
    k.argi = argi;
    return ShadowFnParamMap_get_ptr(g_shadow_fnparams, k);
}

static void shadow_fnparam_register(const char* fn, int argi, const char* base,
                                    int stars) {
    ShadowFnParamKey k;
    ShadowFnParam v;
    if (!fn || !fn[0] || !base || !base[0] || argi < 0) return;
    shadow_meta_ar_ensure();
    if (!g_shadow_fnparams) return;
    memset(&k, 0, sizeof(k));
    snprintf(k.fn, sizeof(k.fn), "%s", fn);
    k.argi = argi;
    /* TU prepass re-walks nested fns/headers; duplicates must not grow. */
    if (ShadowFnParamMap_get_ptr(g_shadow_fnparams, k)) return;
    memset(&v, 0, sizeof(v));
    snprintf(v.fn, sizeof(v.fn), "%s", fn);
    snprintf(v.base, sizeof(v.base), "%s", base);
    v.stars = stars;
    v.argi = argi;
    if (ShadowFnParamMap_insert(g_shadow_fnparams, k, v) != 0) {
        if (!g_shadow_fnparam_oom) {
            fprintf(stderr,
                    "error: fn param type table grow failed (OOM); typed "
                    "pointer casts would silently degrade\n");
            g_shadow_fnparam_oom = 1;
            g_shadow_ufcs_miss = 1; /* fail the emit product */
        }
    }
}

/* Header protos are not in the TU AST. One walk into the existing
 * ShadowFnParamMap (TU collect_binds already ran — insert-if-absent). */
static int shadow_fnparam_header_cb(const char* name, int argi, const char* ty,
                                    void* ctx) {
    char base[64];
    int stars = 0;
    (void)ctx;
    if (!name || !name[0] || !ty || !ty[0] || argi < 0) return 0;
    shadow_ty_base_stars(ty, base, sizeof(base), &stars);
    if (!base[0]) return 0;
    shadow_fnparam_register(name, argi, base, stars);
    return g_shadow_fnparam_oom ? -1 : 0;
}

static void shadow_fnparam_ingest_headers(void) {
    shadow_meta_ar_ensure();
    if (!g_shadow_fnparams) return;
    if (cc_included_cch_each_fn_param(shadow_fnparam_header_cb, NULL) != 0) {
        fprintf(stderr, "error: header fn param ingest failed\n");
        g_shadow_fnparam_oom = 1;
        g_shadow_ufcs_miss = 1;
    }
}


static const ShadowBind* shadow_bind_lookup(const char* name) {
    int i;
    if (!name) return NULL;
    for (i = g_shadow_nbinds - 1; i >= 0; i--) {
        if (g_shadow_binds[i] && strcmp(g_shadow_binds[i]->name, name) == 0)
            return g_shadow_binds[i];
    }
    return NULL;
}

static ShadowVariant* shadow_variant_for_recv(const char* recv) {
    const ShadowBind* b;
    char base[96];
    size_t tl;
    if (!recv || !recv[0]) return NULL;
    b = shadow_bind_lookup(recv);
    if (!b || !b->ty[0]) return NULL;
    /* `RedisValueKind k` resolves bare designators via the variant base. */
    tl = strlen(b->ty);
    if (tl > 4 && strcmp(b->ty + tl - 4, "Kind") == 0) {
        size_t nl = tl - 4;
        if (nl >= sizeof(base)) nl = sizeof(base) - 1;
        memcpy(base, b->ty, nl);
        base[nl] = 0;
        return shadow_variant_find(base);
    }
    return shadow_variant_find(b->ty);
}

static int shadow_recv_name_before(const char* expr, const char* dot,
                                   char* name, size_t cap) {
    const char* q = dot - 1;
    size_t ni = 0;
    char tmp[64];
    int dep;
    if (!expr || !dot || dot <= expr || !name || !cap) return 0;
    while (q >= expr && (*q == ' ' || *q == '\t')) q--;
    if (q < expr) return 0;
    if (*q == ']' || *q == ')') {
        dep = 0;
        while (q >= expr) {
            if (*q == ']' || *q == ')') dep++;
            else if ((*q == '[' || *q == '(') && dep > 0) dep--;
            else if (dep == 0 && shadow_is_id0(*q)) {
                /* Leave q on the last identifier char (walk may stop mid-id). */
                while (q + 1 < dot && shadow_is_id(q[1])) q++;
                break;
            }
            q--;
        }
    }
    while (q >= expr && shadow_is_id(*q) && ni + 1 < sizeof(tmp))
        tmp[ni++] = *q--;
    if (!ni) return 0;
    {
        size_t i;
        for (i = 0; i < ni && i + 1 < cap; i++)
            name[i] = tmp[ni - 1 - i];
        name[i < cap - 1 ? i : cap - 1] = 0;
    }
    return 1;
}

static ShadowVariant* shadow_variant_pick(const char* expr, const char* site,
                                          ShadowVariant* hint, const char* arm,
                                          size_t alen) {
    char ab[64], recv[64];
    ShadowVariant* vv;
    int a;
    if (hint) {
        for (a = 0; a < hint->narm; a++)
            if (strlen(hint->arms[a]) == alen &&
                memcmp(hint->arms[a], arm, alen) == 0)
                return hint;
    }
    if (site && shadow_recv_name_before(expr, site, recv, sizeof(recv))) {
        vv = shadow_variant_for_recv(recv);
        if (vv) {
            for (a = 0; a < vv->narm; a++)
                if (strlen(vv->arms[a]) == alen &&
                    memcmp(vv->arms[a], arm, alen) == 0)
                    return vv;
        }
        /* Receiver token present (`r.data`, `c->framed`): never guess by
         * global arm name — that rewrites ordinary struct fields whenever
         * any variant in the TU shares the member name (e.g. grammar `data`). */
        return hint;
    }
    if (alen + 1 <= sizeof(ab)) {
        memcpy(ab, arm, alen);
        ab[alen] = 0;
        vv = shadow_variant_find_by_arm(ab);
        if (vv) return vv;
    }
    return hint;
}

static int shadow_variant_arm_match(ShadowVariant* hint, const char* expr,
                                    const char* site, const char* arm,
                                    size_t alen, ShadowVariant** out_v,
                                    int* out_a) {
    ShadowVariant* vv = shadow_variant_pick(expr, site, hint, arm, alen);
    int a;
    if (!vv || !out_v || !out_a) return 0;
    for (a = 0; a < vv->narm; a++) {
        if (strlen(vv->arms[a]) == alen &&
            memcmp(vv->arms[a], arm, alen) == 0) {
            *out_v = vv;
            *out_a = a;
            return 1;
        }
    }
    return 0;
}

/* --- Variant frontend diagnostics (oracle-aligned with classic lower) --- */

static void shadow_variant_arm_list(const ShadowVariant* v, char* out,
                                   size_t out_sz) {
    size_t o = 0;
    int a;
    if (!out || !out_sz) return;
    out[0] = 0;
    if (!v) return;
    for (a = 0; a < v->narm; a++) {
        int m = snprintf(out + o, out_sz - o, "%s%s", a ? ", " : "",
                         v->arms[a]);
        if (m < 0 || (size_t)m >= out_sz - o) break;
        o += (size_t)m;
    }
}

/* Cheap did-you-mean: nearest arm by Levenshtein distance <= 2. */
static const char* shadow_variant_suggest_arm(const ShadowVariant* v,
                                              const char* s, size_t len) {
    const char* best = NULL;
    int best_d = 3;
    int a;
    char cand[64];
    if (!v || !s || len == 0 || len > 48) return NULL;
    memcpy(cand, s, len);
    cand[len] = 0;
    for (a = 0; a < v->narm; a++) {
        const char* t = v->arms[a];
        size_t tl = strlen(t);
        int d[64][64];
        size_t i, j;
        if (tl > 48) continue;
        for (i = 0; i <= len; i++) d[i][0] = (int)i;
        for (j = 0; j <= tl; j++) d[0][j] = (int)j;
        for (i = 1; i <= len; i++) {
            for (j = 1; j <= tl; j++) {
                int c = (cand[i - 1] == t[j - 1]) ? 0 : 1;
                int m = d[i - 1][j] + 1;
                if (d[i][j - 1] + 1 < m) m = d[i][j - 1] + 1;
                if (d[i - 1][j - 1] + c < m) m = d[i - 1][j - 1] + c;
                d[i][j] = m;
            }
        }
        if (d[len][tl] < best_d) {
            best_d = d[len][tl];
            best = t;
        }
    }
    return best;
}

/* Emit `path:line:col: error: …` (or `path:line: error:` when no_col).
 * Needle search starts at st->tok_off (never earlier) so comment-text hits
 * like the word "switch" in a file header cannot steal the column. */
static void shadow_variant_err_loc(ShadowCtx* ctx, AstNode* st, CEmit* out,
                                  const char* needle, int no_col,
                                  const char* msg) {
    const char* path = "<input>";
    char lfile[1024];
    int line = 1, col = 1;
    lfile[0] = 0;
    if (out) out->err = 1;
    if (ctx && ctx->cache && st && st->file_id) {
        FileTape* ft = tape_by_id(ctx->cache, st->file_id);
        if (ft && ft->bytes) {
            size_t off = st->tok_off;
            if (needle && needle[0] && strlen(needle) >= 2) {
                size_t nlen = strlen(needle);
                size_t i;
                size_t end = ft->len;
                if (st->tok_off + 1024 < end) end = st->tok_off + 1024;
                for (i = st->tok_off; i + nlen <= end; i++) {
                    if (memcmp(ft->bytes + i, needle, nlen) == 0) {
                        /* Prefer identifier needles at a word boundary. */
                        if (nlen >= 2 && shadow_is_id(needle[0]) && i > 0 &&
                            shadow_is_id(ft->bytes[i - 1]))
                            continue;
                        off = i;
                        break;
                    }
                }
            }
            offset_to_linecol(ft, off, &line, &col);
            tape_logical_at(ft, off, lfile, sizeof(lfile), &line);
            path = tape_diag_file(ft, off, lfile, sizeof(lfile));
        }
    }
    if (no_col)
        fprintf(stderr, "%s:%d: error: %s\n", path, line, msg);
    else
        fprintf(stderr, "%s:%d:%d: error: %s\n", path, line, col, msg);
}

static void shadow_variant_note_loc(ShadowCtx* ctx, AstNode* st,
                                   const char* needle, const char* msg) {
    const char* path = "<input>";
    char lfile[1024];
    int line = 1, col = 1;
    lfile[0] = 0;
    if (ctx && ctx->cache && st && st->file_id) {
        FileTape* ft = tape_by_id(ctx->cache, st->file_id);
        if (ft && ft->bytes) {
            size_t off = st->tok_off;
            if (needle && needle[0] && strlen(needle) >= 2) {
                size_t nlen = strlen(needle);
                size_t i;
                size_t end = ft->len;
                if (st->tok_off + 1024 < end) end = st->tok_off + 1024;
                for (i = st->tok_off; i + nlen <= end; i++) {
                    if (memcmp(ft->bytes + i, needle, nlen) == 0) {
                        off = i;
                        break;
                    }
                }
            }
            offset_to_linecol(ft, off, &line, &col);
            tape_logical_at(ft, off, lfile, sizeof(lfile), &line);
            path = tape_diag_file(ft, off, lfile, sizeof(lfile));
        }
    }
    fprintf(stderr, "%s:%d:%d: note: %s\n", path, line, col, msg);
}

static int shadow_variant_arm_index(const ShadowVariant* v, const char* arm,
                                   size_t alen) {
    int a;
    if (!v || !arm) return -1;
    for (a = 0; a < v->narm; a++)
        if (strlen(v->arms[a]) == alen && memcmp(v->arms[a], arm, alen) == 0)
            return a;
    return -1;
}

static int shadow_variant_is_bare_dot(const char* expr, const char* p) {
    const char* b;
    if (!expr || !p || *p != '.') return 0;
    b = p;
    while (b > expr && (b[-1] == ' ' || b[-1] == '\t')) b--;
    if (b > expr &&
        (shadow_is_id(b[-1]) || b[-1] == ')' || b[-1] == ']'))
        return 0;
    return 1;
}

/* Partner of `==` / `!=` resolves to a variant (via `.kind` / bind / NameKind). */
static ShadowVariant* shadow_variant_resolve_cmp_partner(const char* expr,
                                                        const char* op) {
    const char* a;
    const char* b;
    char recv[64];
    size_t n;
    ShadowVariant* v;
    if (!expr || !op || op < expr) return NULL;
    a = expr;
    b = op;
    while (a < b && (*a == ' ' || *a == '(' || *a == '\t')) a++;
    while (b > a && (b[-1] == ' ' || b[-1] == ')' || b[-1] == '\t')) b--;
    if (b <= a) return NULL;
    /* ends with `.kind` / `->kind` */
    if ((size_t)(b - a) >= 5 && memcmp(b - 4, "kind", 4) == 0) {
        const char* k = b - 4;
        while (k > a && (k[-1] == ' ' || k[-1] == '\t')) k--;
        if (k > a && k[-1] == '.') {
            if (shadow_recv_name_before(expr, k - 1, recv, sizeof(recv)))
                return shadow_variant_for_recv(recv);
        } else if (k > a + 1 && k[-1] == '>' && k[-2] == '-') {
            if (shadow_recv_name_before(expr, k - 2, recv, sizeof(recv)))
                return shadow_variant_for_recv(recv);
        }
        return NULL;
    }
    /* single identifier */
    n = 0;
    while (a + n < b && shadow_is_id(a[n])) n++;
    if (n && a + n == b && n < sizeof(recv)) {
        memcpy(recv, a, n);
        recv[n] = 0;
        v = shadow_variant_for_recv(recv);
        if (v) return v;
        /* NameKind */
        if (n > 4 && strcmp(recv + n - 4, "Kind") == 0) {
            char base[64];
            size_t bl = n - 4;
            if (bl >= sizeof(base)) bl = sizeof(base) - 1;
            memcpy(base, recv, bl);
            base[bl] = 0;
            return shadow_variant_find(base);
        }
    }
    return NULL;
}

/* Returns 1 if a diagnostic was emitted (caller must abort). */
static int shadow_variant_diag_bare_desig(ShadowCtx* ctx, AstNode* st,
                                         CEmit* out, const char* expr) {
    const char* p;
    if (!expr || !g_shadow_nvariants) return 0;
    p = expr;
    while (*p) {
        if (*p == '.' && shadow_variant_is_bare_dot(expr, p) &&
            ((p[1] >= 'a' && p[1] <= 'z') || (p[1] >= 'A' && p[1] <= 'Z') ||
             p[1] == '_')) {
            const char* arm = p + 1;
            const char* e = arm;
            const char* before;
            const char* after;
            char needle[72];
            char msg[320];
            size_t alen;
            ShadowVariant* v = NULL;
            int ai;
            while (*e && shadow_is_id(*e)) e++;
            alen = (size_t)(e - arm);
            if (!alen || alen >= 64) {
                p = e;
                continue;
            }
            before = p;
            while (before > expr &&
                   (before[-1] == ' ' || before[-1] == '\t'))
                before--;
            after = e;
            while (*after == ' ' || *after == '\t') after++;
            /* `.arm ==` / `.arm !=` or `== .arm` / `!= .arm` */
            if (before > expr + 1 &&
                ((before[-1] == '=' && before[-2] == '=') ||
                 (before[-1] == '=' && before[-2] == '!'))) {
                const char* op = before - 2;
                v = shadow_variant_resolve_cmp_partner(expr, op);
            } else if ((after[0] == '=' && after[1] == '=') ||
                       (after[0] == '!' && after[1] == '=')) {
                const char* rhs = after + 2;
                while (*rhs == ' ' || *rhs == '\t') rhs++;
                /* Partner on the right — rare; try bind/kind form. */
                {
                    char tmp[288];
                    size_t n = strlen(rhs);
                    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
                    memcpy(tmp, rhs, n);
                    tmp[n] = 0;
                    v = shadow_variant_resolve_cmp_partner(tmp, tmp + n);
                    if (!v) {
                        /* scan `Name_arm` / `.kind` on the right of == */
                        const char* q;
                        for (q = rhs; *q; q++) {
                            if ((q[0] == '=' && q[1] == '=') ||
                                (q[0] == '!' && q[1] == '='))
                                break;
                        }
                        (void)q;
                        {
                            char recv[64];
                            const char* id = rhs;
                            size_t nl = 0;
                            while (*id == '(' || *id == ' ') id++;
                            while (shadow_is_id(id[nl])) nl++;
                            if (nl && nl < sizeof(recv)) {
                                memcpy(recv, id, nl);
                                recv[nl] = 0;
                                v = shadow_variant_for_recv(recv);
                                if (!v && nl > 4 &&
                                    strcmp(recv + nl - 4, "Kind") == 0) {
                                    recv[nl - 4] = 0;
                                    v = shadow_variant_find(recv);
                                }
                            }
                            if (!v) {
                                const char* kd = strstr(rhs, ".kind");
                                if (!kd) kd = strstr(rhs, "->kind");
                                if (kd &&
                                    shadow_recv_name_before(rhs, kd, recv,
                                                            sizeof(recv)))
                                    v = shadow_variant_for_recv(recv);
                            }
                        }
                    }
                }
            } else {
                p = e;
                continue;
            }
            snprintf(needle, sizeof(needle), ".%.*s", (int)alen, arm);
            if (!v) {
                snprintf(msg, sizeof(msg),
                         "bare designator '.%.*s' cannot be resolved from "
                         "context (no variant type governs this "
                         "comparison/assignment)",
                         (int)alen, arm);
                shadow_variant_err_loc(ctx, st, out, needle, 0, msg);
                shadow_variant_note_loc(
                    ctx, st, needle,
                    "compare against a variant tag");
                return 1;
            }
            ai = shadow_variant_arm_index(v, arm, alen);
            if (ai < 0) {
                char arms[256];
                const char* sugg;
                shadow_variant_arm_list(v, arms, sizeof(arms));
                sugg = shadow_variant_suggest_arm(v, arm, alen);
                if (sugg)
                    snprintf(msg, sizeof(msg),
                             "variant '%s' has no arm '.%.*s' (arms: %s) — "
                             "did you mean '.%s'?",
                             v->name, (int)alen, arm, arms, sugg);
                else
                    snprintf(msg, sizeof(msg),
                             "variant '%s' has no arm '.%.*s' (arms: %s)",
                             v->name, (int)alen, arm, arms);
                shadow_variant_err_loc(ctx, st, out, needle, 0, msg);
                return 1;
            }
            p = e;
            continue;
        }
        p++;
    }
    return 0;
}

/* `.kind` / `->kind` write on a variant lvalue. */
static int shadow_variant_diag_kind_write(ShadowCtx* ctx, AstNode* st,
                                         CEmit* out, const char* lhs) {
    const char* k;
    const char* acc;
    char recv[64];
    char root[64];
    char msg[384];
    ShadowVariant* v;
    size_t rlen;
    int arrow = 0;
    if (!lhs) return 0;
    k = strstr(lhs, "->kind");
    if (k && (k[6] == 0 || !shadow_is_id(k[6]))) {
        acc = k;
        arrow = 1;
    } else {
        k = strstr(lhs, ".kind");
        if (!k || (k[5] && shadow_is_id(k[5]))) return 0;
        /* skip `.kind` that is part of something else; require end or space */
        if (k[5] != 0 && k[5] != ' ' && k[5] != '\t') return 0;
        acc = k;
    }
    if (!shadow_recv_name_before(lhs, acc, recv, sizeof(recv))) return 0;
    v = shadow_variant_for_recv(recv);
    if (!v) return 0;
    /* root spelling for the oracle message */
    {
        const char* rs = shadow_variant_recv_lbound(lhs, acc);
        rlen = (size_t)(acc - rs);
        while (rlen > 0 && (rs[rlen - 1] == ' ' || rs[rlen - 1] == '\t'))
            rlen--;
        if (rlen >= sizeof(root)) rlen = sizeof(root) - 1;
        memcpy(root, rs, rlen);
        root[rlen] = 0;
    }
    (void)arrow;
    snprintf(msg, sizeof(msg),
             "variant tag '.kind' is read-only — the tag changes only through "
             "construction or whole-variant assignment ('%s = (%s){ .arm = "
             "... };')",
             root[0] ? root : recv, v->name);
    shadow_variant_err_loc(ctx, st, out, ".kind", 0, msg);
    return 1;
}

/* Designated ctor / braced-assign diagnostics on `{ .arm = … }`. */
static int shadow_variant_diag_ctor(ShadowCtx* ctx, AstNode* st, CEmit* out,
                                   const char* vty, const char* expr,
                                   const char* lhs_for_braced) {
    ShadowVariant* v;
    const char* p;
    const char* brace;
    int arm_i = -1;
    char first_arm[64];
    first_arm[0] = 0;
    if (!expr) return 0;
    v = vty ? shadow_variant_find(vty) : NULL;
    brace = expr;
    while (*brace == ' ' || *brace == '\t') brace++;
    /* Strip outer `(Name){` compound form for scanning. Plain-struct
     * compound literals are host C — only bare `{ .arm = … }` braced
     * assigns are restricted to @variant. */
    int is_compound_lit = 0;
    if (*brace == '(') {
        const char* q = brace + 1;
        while (*q && *q != ')') q++;
        if (*q == ')') {
            q++;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '{') {
                brace = q;
                is_compound_lit = 1;
            }
        }
    }
    if (*brace != '{') {
        /* braced assign: rhs may be `{ … }` without type prefix */
        return 0;
    }
    p = brace + 1;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',') p++;
        if (*p == '}' || !*p) break;
        if (*p == '.') {
            const char* arm = p + 1;
            const char* e = arm;
            const char* eq;
            size_t alen;
            char needle[72];
            char msg[384];
            int ai;
            while (*e && shadow_is_id(*e)) e++;
            alen = (size_t)(e - arm);
            eq = e;
            while (*eq == ' ' || *eq == '\t') eq++;
            if (*eq != '=' || eq[1] == '=') {
                p = e;
                continue;
            }
            if (alen == 4 && memcmp(arm, "kind", 4) == 0) {
                p = eq + 1;
                continue;
            }
            if (alen == 1 && arm[0] == 'u') {
                p = eq + 1;
                continue;
            }
            snprintf(needle, sizeof(needle), ".%.*s", (int)alen, arm);
            if (!v) {
                /* Unique-arm / multi-owner / unresolved braced assign. */
                int owners = 0;
                int oi;
                ShadowVariant* uv = NULL;
                for (oi = 0; oi < g_shadow_nvariants; oi++) {
                    if (shadow_variant_arm_index(&g_shadow_variants[oi], arm,
                                                alen) >= 0) {
                        owners++;
                        uv = &g_shadow_variants[oi];
                    }
                }
                if (lhs_for_braced && lhs_for_braced[0] && owners == 0 &&
                    !is_compound_lit) {
                    char lhs_needle[96];
                    snprintf(msg, sizeof(msg),
                             "braced assignment 'lhs = { ... };' is only "
                             "defined for @variant types — '%s' does not "
                             "resolve to a variant and '.%.*s' is not an arm "
                             "of any declared variant (for a plain struct use "
                             "a compound literal: 'lhs = (T){ ... };')",
                             lhs_for_braced, (int)alen, arm);
                    /* `p = {` — avoid matching a lone `p` in comments. */
                    snprintf(lhs_needle, sizeof(lhs_needle), "%s = {",
                             lhs_for_braced);
                    shadow_variant_err_loc(ctx, st, out, lhs_needle, 0, msg);
                    return 1;
                }
                if (owners == 0) {
                    /* Plain-struct designated init / compound literal. */
                    p = eq + 1;
                    continue;
                }
                if (owners == 1) v = uv;
                else {
                    p = eq + 1;
                    continue;
                }
            }
            ai = shadow_variant_arm_index(v, arm, alen);
            if (ai < 0) {
                char arms[256];
                const char* sugg;
                shadow_variant_arm_list(v, arms, sizeof(arms));
                sugg = shadow_variant_suggest_arm(v, arm, alen);
                if (sugg)
                    snprintf(msg, sizeof(msg),
                             "variant '%s' has no arm '.%.*s' (arms: %s) — "
                             "did you mean '.%s'?",
                             v->name, (int)alen, arm, arms, sugg);
                else
                    snprintf(msg, sizeof(msg),
                             "variant '%s' has no arm '.%.*s' (arms: %s)",
                             v->name, (int)alen, arm, arms);
                shadow_variant_err_loc(ctx, st, out, needle, 0, msg);
                return 1;
            }
            if (arm_i >= 0) {
                snprintf(msg, sizeof(msg),
                         "variant '%s' initializer names two arms '.%s' and "
                         "'.%.*s' — a variant holds exactly one active arm",
                         v->name, first_arm, (int)alen, arm);
                shadow_variant_err_loc(ctx, st, out, needle, 0, msg);
                return 1;
            }
            arm_i = ai;
            snprintf(first_arm, sizeof(first_arm), "%.*s", (int)alen, arm);
            /* Payload type mismatch: `(Other){…}` vs arm type. */
            {
                const char* val = eq + 1;
                while (*val == ' ' || *val == '\t') val++;
                if (*val == '(' && v->tys[ai][0]) {
                    const char* tn = val + 1;
                    const char* te = tn;
                    char got[96];
                    size_t gl;
                    while (*te && *te != ')' && *te != ' ' && *te != '\t')
                        te++;
                    gl = (size_t)(te - tn);
                    if (gl && gl < sizeof(got) && *te == ')') {
                        memcpy(got, tn, gl);
                        got[gl] = 0;
                        if (strcmp(got, v->tys[ai]) != 0 &&
                            strcmp(got, "void") != 0) {
                            /* Skip if arm type is a pointer spelling of got. */
                            size_t want = strlen(v->tys[ai]);
                            int ptr_ok =
                                (want == gl + 1 &&
                                 memcmp(v->tys[ai], got, gl) == 0 &&
                                 v->tys[ai][gl] == '*');
                            if (!ptr_ok) {
                                snprintf(msg, sizeof(msg),
                                         "cannot convert 'struct %s' to",
                                         got);
                                shadow_variant_err_loc(ctx, st, out, NULL, 1,
                                                      msg);
                                return 1;
                            }
                        }
                    }
                }
            }
            p = eq + 1;
            continue;
        }
        /* skip nested groups */
        if (*p == '{' || *p == '(' || *p == '[') {
            char open = *p;
            char close = (open == '{') ? '}' : (open == '(') ? ')' : ']';
            int dep = 0;
            do {
                if (*p == open) dep++;
                else if (*p == close) dep--;
                p++;
            } while (*p && dep > 0);
            continue;
        }
        p++;
    }
    return 0;
}

/* Switch subject + designator cases: unknown arm / non-variant / exhaustiveness. */
static int shadow_variant_diag_switch(ShadowCtx* ctx, AstNode* st, CEmit* out,
                                     const char* subj, const char* body,
                                     ShadowVariant* v) {
    const char* p;
    unsigned covered = 0;
    int have_default = 0;
    int have_desig = 0;
    char msg[384];
    if (!body) return 0;
    p = body;
    while (*p) {
        if (strncmp(p, "default", 7) == 0 && !shadow_is_id(p[7] ? p[7] : 0) &&
            (p == body || !shadow_is_id(p[-1]))) {
            const char* q = p + 7;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == ':') have_default = 1;
            p = q;
            continue;
        }
        if (strncmp(p, "case", 4) == 0 && !shadow_is_id(p[4] ? p[4] : 0) &&
            (p == body || !shadow_is_id(p[-1]))) {
            const char* q = p + 4;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '.') {
                const char* arm = q + 1;
                const char* e = arm;
                char needle[72];
                size_t alen;
                int ai;
                have_desig = 1;
                while (*e && shadow_is_id(*e)) e++;
                /* Space-joined token spans can yield `case . str`. */
                while (*arm == ' ' || *arm == '\t') arm++;
                e = arm;
                while (*e && shadow_is_id(*e)) e++;
                alen = (size_t)(e - arm);
                if (!alen) {
                    p = (*q == '.') ? q + 1 : e;
                    continue;
                }
                snprintf(needle, sizeof(needle), ".%.*s", (int)alen, arm);
                if (!v) {
                    char subj_show[64];
                    size_t sl;
                    const char* s = subj ? subj : "?";
                    while (*s == '(' || *s == ' ') s++;
                    sl = 0;
                    while (s[sl] && shadow_is_id(s[sl]) &&
                           sl + 1 < sizeof(subj_show))
                        sl++;
                    memcpy(subj_show, s, sl);
                    subj_show[sl] = 0;
                    snprintf(msg, sizeof(msg),
                             "designator case label '.%.*s' requires a switch "
                             "on a @variant subject — '%s' does not resolve "
                             "to a variant",
                             (int)alen, arm,
                             subj_show[0] ? subj_show : "?");
                    shadow_variant_err_loc(ctx, st, out, needle, 0, msg);
                    return 1;
                }
                ai = shadow_variant_arm_index(v, arm, alen);
                if (ai < 0) {
                    char arms[256];
                    const char* sugg;
                    shadow_variant_arm_list(v, arms, sizeof(arms));
                    sugg = shadow_variant_suggest_arm(v, arm, alen);
                    if (sugg)
                        snprintf(msg, sizeof(msg),
                                 "variant '%s' has no arm '.%.*s' (arms: %s) — "
                                 "did you mean '.%s'?",
                                 v->name, (int)alen, arm, arms, sugg);
                    else
                        snprintf(msg, sizeof(msg),
                                 "variant '%s' has no arm '.%.*s' (arms: %s)",
                                 v->name, (int)alen, arm, arms);
                    shadow_variant_err_loc(ctx, st, out, needle, 0, msg);
                    return 1;
                }
                covered |= 1u << ai;
                p = e;
                continue;
            }
            /* tag constant Name_arm */
            if (v && shadow_is_id(*q)) {
                const char* e = q;
                size_t pfx = strlen(v->name);
                while (*e && shadow_is_id(*e)) e++;
                if ((size_t)(e - q) > pfx + 1 &&
                    memcmp(q, v->name, pfx) == 0 && q[pfx] == '_') {
                    int ai = shadow_variant_arm_index(
                        v, q + pfx + 1, (size_t)(e - (q + pfx + 1)));
                    if (ai >= 0) covered |= 1u << ai;
                }
                p = e;
                continue;
            }
        }
        p++;
    }
    if (v && have_desig && !have_default) {
        char missing[256];
        size_t mo = 0;
        int nmiss = 0;
        int a;
        missing[0] = 0;
        for (a = 0; a < v->narm; a++) {
            int m;
            if (covered & (1u << a)) continue;
            m = snprintf(missing + mo, sizeof(missing) - mo, "%s'.%s'",
                         nmiss ? ", " : "", v->arms[a]);
            if (m > 0 && (size_t)m < sizeof(missing) - mo) mo += (size_t)m;
            nmiss++;
        }
        if (nmiss > 0) {
            char sw_needle[96];
            snprintf(msg, sizeof(msg),
                     "switch on variant '%s' is not exhaustive: missing arm%s "
                     "%s (add the missing case%s or a 'default:' to opt out)",
                     v->name, nmiss > 1 ? "s" : "", missing,
                     nmiss > 1 ? "s" : "");
            /* Prefer `switch (` at the stmt — not the word in a comment. */
            snprintf(sw_needle, sizeof(sw_needle), "switch (");
            shadow_variant_err_loc(ctx, st, out, sw_needle, 0, msg);
            return 1;
        }
    }
    (void)have_desig;
    return 0;
}

/* Rewrite cc_ok/cc_err using a bound variable's declared CCResult_* type. */
static void shadow_rewrite_result_ctors_for_var(char* expr, size_t cap,
                                                const char* var) {
    const ShadowBind* b;
    if (!expr || !cap || !var || !var[0]) return;
    b = shadow_bind_lookup(var);
    if (b && b->ty[0] && strncmp(b->ty, "CCResult_", 9) == 0)
        shadow_rewrite_result_ctors(expr, cap, b->ty);
}

/* Simple identifier at the start of a UFCS receiver (strip * &). */
static const ShadowBind* shadow_bind_for_recv(const char* recv) {
    char name[64];
    const char* p = recv;
    size_t ni = 0;
    if (!p) return NULL;
    while (*p == '*' || *p == '&' || *p == ' ' || *p == '\t' || *p == '(') p++;
    while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
           (*p >= '0' && *p <= '9') || *p == '_') {
        if (ni + 1 >= sizeof(name)) break;
        name[ni++] = *p++;
    }
    if (!ni) return NULL;
    name[ni] = 0;
    return shadow_bind_lookup(name);
}

static int shadow_bind_ty_has(const ShadowBind* b, const char* needle) {
    return b && b->ty[0] && needle && strstr(b->ty, needle) != NULL;
}

/* Strip leading cv / trailing * / spaces from a bound type into dst. */
static void shadow_bind_base_ty(const ShadowBind* b, char* dst, size_t cap) {
    size_t n;
    const char* s;
    const char* resolved;
    if (!dst || !cap) return;
    dst[0] = 0;
    if (!b || !b->ty[0]) return;
    s = b->ty;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    for (;;) {
        if (strncmp(s, "const ", 6) == 0) {
            s += 6;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
            continue;
        }
        if (strncmp(s, "volatile ", 9) == 0) {
            s += 9;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
            continue;
        }
        break;
    }
    /* `struct Tag` / `union Tag` → Tag for @as / UFCS outer match. */
    if (strncmp(s, "struct ", 7) == 0) s += 7;
    else if (strncmp(s, "union ", 6) == 0) s += 6;
    while (*s == ' ' || *s == '\t') s++;
    snprintf(dst, cap, "%s", s);
    /* Drop trailing `*` / spaces so `Map_T*` and `Map_T` share family UFCS. */
    {
        size_t dn = strlen(dst);
        while (dn && (dst[dn - 1] == '*' || dst[dn - 1] == ' ' ||
                      dst[dn - 1] == '\t'))
            dst[--dn] = 0;
    }
    /* CharVec / CharVec* → CCVec_char so family UFCS (as_slice) resolves. */
    {
        char alias[128];
        size_t al = 0;
        while (dst[al] && dst[al] != '*' && dst[al] != ' ' &&
               al + 1 < sizeof(alias))
            al++;
        memcpy(alias, dst, al);
        alias[al] = 0;
        resolved = shadow_td_alias_resolve(alias);
        if (resolved && resolved[0] && strcmp(resolved, alias) != 0) {
            char base[128];
            size_t bl = 0;
            while (resolved[bl] && resolved[bl] != '*' && resolved[bl] != ' ' &&
                   bl + 1 < sizeof(base))
                bl++;
            memcpy(base, resolved, bl);
            base[bl] = 0;
            if (base[0]) snprintf(dst, cap, "%s", base);
        }
    }
    n = strlen(dst);
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '*' || dst[n - 1] == '\t'))
        dst[--n] = 0;
}

static void __attribute__((unused)) shadow_family_snake(const char* family, char* out, size_t cap) {
    size_t i;
    if (!family || !cap) return;
    for (i = 0; family[i] && i + 1 < cap; i++) {
        char c = family[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[i] = c;
    }
    out[i] = 0;
}

/* Prefer `tests/...` (MATCH goldens); else basename. */
static void shadow_fmt_site_path(const char* cur_file, char* file, size_t fcap) {
    const char* base = cur_file ? cur_file : "";
    const char* tests = strstr(base, "tests/");
    if (tests)
        base = tests;
    else {
        const char* slash = strrchr(base, '/');
        if (slash && slash[1]) base = slash + 1;
    }
    snprintf(file, fcap, "%s", base);
}

/* Resolve `#line` / masked CC_LN at byte offset via FileTape line index. */
static void shadow_tape_logical_at(const FileTape* ft, size_t off, char* file,
                                   size_t fcap, int* line_out) {
    char raw[256];
    raw[0] = 0;
    tape_logical_at(ft, off, raw, sizeof(raw), line_out);
    if (!file || !fcap) return;
    shadow_fmt_site_path(raw[0] ? raw : (ft && ft->path ? ft->path : ""), file,
                         fcap);
}

static void shadow_gfac_add(const char* family, int arity, const char* tpl,
                            int append, const char* origin_file,
                            int origin_line) {
    int i;
    if (!family || !tpl) return;
    if (g_shadow_ngfac >= SHADOW_GFAC_CAP) {
        shadow_table_full("gfac", SHADOW_GFAC_CAP, family);
        return;
    }
    for (i = 0; i < g_shadow_ngfac; i++) {
        if (strcmp(g_shadow_gfac[i].family, family) == 0) {
            if (append) {
                size_t cur = strlen(g_shadow_gfac[i].tpl);
                size_t add = strlen(tpl);
                if (cur + 1 + add < sizeof(g_shadow_gfac[i].tpl)) {
                    g_shadow_gfac[i].tpl[cur] = '\n';
                    memcpy(g_shadow_gfac[i].tpl + cur + 1, tpl, add + 1);
                }
            } else {
                snprintf(g_shadow_gfac[i].tpl, sizeof(g_shadow_gfac[i].tpl),
                         "%s", tpl);
                g_shadow_gfac[i].has_base = 1;
            }
            if (arity > 0) g_shadow_gfac[i].arity = arity;
            if (origin_file && origin_file[0] &&
                !g_shadow_gfac[i].origin_file[0]) {
                snprintf(g_shadow_gfac[i].origin_file,
                         sizeof(g_shadow_gfac[0].origin_file), "%s",
                         origin_file);
                g_shadow_gfac[i].origin_line = origin_line;
            }
            return;
        }
    }
    if (g_shadow_ngfac >= SHADOW_GFAC_CAP) {
        shadow_table_full("gfac", SHADOW_GFAC_CAP, family);
        return;
    }
    snprintf(g_shadow_gfac[g_shadow_ngfac].family,
             sizeof(g_shadow_gfac[0].family), "%s", family);
    snprintf(g_shadow_gfac[g_shadow_ngfac].tpl, sizeof(g_shadow_gfac[0].tpl),
             "%s", tpl);
    g_shadow_gfac[g_shadow_ngfac].arity = arity;
    g_shadow_gfac[g_shadow_ngfac].has_base = append ? 0 : 1;
    g_shadow_gfac[g_shadow_ngfac].origin_file[0] = 0;
    g_shadow_gfac[g_shadow_ngfac].origin_line = 0;
    if (origin_file && origin_file[0]) {
        snprintf(g_shadow_gfac[g_shadow_ngfac].origin_file,
                 sizeof(g_shadow_gfac[0].origin_file), "%s", origin_file);
        g_shadow_gfac[g_shadow_ngfac].origin_line = origin_line;
    }
    g_shadow_ngfac++;
}

static int shadow_vec_elem_typed_slice(const char* t) {
    return t && (!strcmp(t, "int") || !strcmp(t, "double") ||
                 !strcmp(t, "float") || !strcmp(t, "bool") ||
                 !strcmp(t, "long") || !strcmp(t, "long_long"));
}

static int shadow_ginst_after_vec(const char* family) {
    return family && (strcmp(family, "Map") == 0 ||
                      strcmp(family, "ArrayMap") == 0);
}

/* 0: CCSlice + user factories; 1: Vec (after slices); 2: Map / ArrayMap. */
static int shadow_ginst_phase(const char* family) {
    if (family && strcmp(family, "Vec") == 0) return 1;
    if (shadow_ginst_after_vec(family)) return 2;
    return 0;
}

static void shadow_ginst_mangled_name(const char* family, const char* compact,
                                     char* dst, size_t cap) {
    if (family && strcmp(family, "Vec") == 0)
        snprintf(dst, cap, "CCVec_%s", compact);
    else
        snprintf(dst, cap, "%s_%s", family ? family : "", compact);
}

static int shadow_vec_already(const char* compact) {
    return compact && compact[0] &&
           (strcmp(compact, "char") == 0 || strcmp(compact, "size_t") == 0);
}

/* Compact encodes T* as Tptr. Expand trailing ptr into * for the DECL. */
static void shadow_map_decode_ptr_arg(char* buf, size_t cap) {
    size_t vl;
    int stars = 0;
    char base[80];
    size_t o;
    if (!buf || !cap) return;
    snprintf(base, sizeof(base), "%s", buf);
    vl = strlen(base);
    while (vl >= 3 && strcmp(base + vl - 3, "ptr") == 0) {
        base[vl - 3] = 0;
        vl -= 3;
        stars++;
    }
    snprintf(buf, cap, "%s", base);
    o = strlen(buf);
    while (stars-- > 0 && o + 1 < cap)
        buf[o++] = '*';
    buf[o] = 0;
}

static int shadow_kv_compact_has_alias(const char* kv, const char* alias) {
    char k[64];
    char v[64];
    int klen;
    size_t pl;
    int side;
    if (!kv || !alias || !alias[0]) return 0;
    klen = shadow_kv_compact_key_len(kv);
    if (klen <= 0) return 0;
    if ((size_t)klen >= sizeof(k)) return 0;
    memcpy(k, kv, (size_t)klen);
    k[klen] = 0;
    snprintf(v, sizeof(v), "%s", kv + klen);
    if (v[0] == '_') memmove(v, v + 1, strlen(v));
    for (side = 0; side < 2; side++) {
        char* part = side == 0 ? k : v;
        pl = strlen(part);
        while (pl >= 3 && strcmp(part + pl - 3, "ptr") == 0) {
            part[pl - 3] = 0;
            pl -= 3;
        }
        if (pl && strcmp(part, alias) == 0) return 1;
    }
    return 0;
}

static int shadow_ginst_has(const char* family, const char* compact) {
    int i;
    char mangled[96];
    if (!family || !compact || !compact[0]) return 0;
    shadow_ginst_mangled_name(family, compact, mangled, sizeof(mangled));
    for (i = 0; i < g_shadow_nginst; i++) {
        if (strcmp(g_shadow_ginst[i].mangled, mangled) == 0) return 1;
    }
    return 0;
}

static void shadow_ginst_need_ex(const char* family, const char* compact,
                                 const char args[][64], int nargs) {
    int i, a;
    char mangled[96];
    if (!family || !compact || !compact[0]) return;
    if (strcmp(family, "Map") == 0) {
        if (shadow_map_already(compact)) return;
    }
    if (strcmp(family, "ArrayMap") == 0) {
        if (shadow_amap_already(compact)) return;
    }
    if (strcmp(family, "CCSlice") == 0) {
        if (shadow_slice_already(compact)) return;
        if (g_shadow_nslices >= SHADOW_SLICE_CAP) {
            shadow_table_full("slices", SHADOW_SLICE_CAP, compact);
            return;
        }
        for (i = 0; i < g_shadow_nslices; i++) {
            if (strcmp(g_shadow_slices[i], compact) == 0) break;
        }
        if (i == g_shadow_nslices) {
            snprintf(g_shadow_slices[g_shadow_nslices],
                     sizeof(g_shadow_slices[0]), "%s", compact);
            g_shadow_nslices++;
        }
        {
            char ty[96];
            snprintf(ty, sizeof(ty), "CCSlice_%s", compact);
            shadow_field_register_ex(ty, "base", "CCSlice");
            shadow_as_materialize_globs_for(ty);
        }
    }
    if (strcmp(family, "Vec") == 0) {
        if (shadow_vec_already(compact)) return;
        shadow_vec_need(compact);
        if (shadow_vec_elem_typed_slice(compact)) {
            if (args && nargs >= 1)
                shadow_ginst_need_ex("CCSlice", compact, args, 1);
            else
                shadow_ginst_need("CCSlice", compact);
        }
    }
    if (g_shadow_nginst >= SHADOW_GINST_CAP) {
        shadow_table_full("ginst", SHADOW_GINST_CAP, family);
        return;
    }
    shadow_ginst_mangled_name(family, compact, mangled, sizeof(mangled));
    for (i = 0; i < g_shadow_nginst; i++) {
        if (strcmp(g_shadow_ginst[i].mangled, mangled) == 0) {
            if (g_shadow_ginst[i].nargs == 0 && args && nargs > 0) {
                if (nargs > 8) nargs = 8;
                for (a = 0; a < nargs; a++)
                    snprintf(g_shadow_ginst[i].args[a],
                             sizeof(g_shadow_ginst[0].args[0]), "%s", args[a]);
                g_shadow_ginst[i].nargs = nargs;
            }
            return;
        }
    }
    snprintf(g_shadow_ginst[g_shadow_nginst].family,
             sizeof(g_shadow_ginst[0].family), "%s", family);
    snprintf(g_shadow_ginst[g_shadow_nginst].mangled,
             sizeof(g_shadow_ginst[0].mangled), "%s", mangled);
    snprintf(g_shadow_ginst[g_shadow_nginst].compact,
             sizeof(g_shadow_ginst[0].compact), "%s", compact);
    g_shadow_ginst[g_shadow_nginst].nargs = 0;
    if (args && nargs > 0) {
        if (nargs > 8) nargs = 8;
        for (a = 0; a < nargs; a++)
            snprintf(g_shadow_ginst[g_shadow_nginst].args[a],
                     sizeof(g_shadow_ginst[0].args[0]), "%s", args[a]);
        g_shadow_ginst[g_shadow_nginst].nargs = nargs;
    }
    g_shadow_nginst++;
}

static void shadow_ginst_need(const char* family, const char* compact) {
    shadow_ginst_need_ex(family, compact, NULL, 0);
}

/* Queue a factory instance from a type spelling (bind, typedef, param, field).
 * `Family_rest` in expression text is not an instance — that is a method or
 * an unrelated ident (`CCSlice_len`, `Map_len`, `ArrayMap_len`, `Note_len`). Instantiation is
 * `Name::[args]`, `T[:]` for slices, or a mangled type in a type position. */
static int shadow_ginst_match_family(const char* ident, char* fam_out,
                                    size_t fam_cap, char* compact_out,
                                    size_t compact_cap) {
    size_t best_fl = 0;
    const char* best = NULL;
    int fi;
    char fams[512];
    int nf;
    if (!ident || !ident[0] || !fam_out || !fam_cap || !compact_out ||
        !compact_cap)
        return 0;
    if (strncmp(ident, "CCVec_", 6) == 0 && ident[6]) {
        snprintf(fam_out, fam_cap, "Vec");
        snprintf(compact_out, compact_cap, "%s", ident + 6);
        return compact_out[0] ? 1 : 0;
    }
    for (fi = 0; fi < g_shadow_ngfac; fi++) {
        const char* fam = g_shadow_gfac[fi].family;
        size_t fl = strlen(fam);
        if (fl > best_fl && strncmp(ident, fam, fl) == 0 && ident[fl] == '_') {
            best = fam;
            best_fl = fl;
        }
    }
    nf = cc_emit_plan_generic_factory_names_csv(fams, sizeof(fams));
    if (nf > 0) {
        const char* s = fams;
        while (*s) {
            const char* e = s;
            size_t fl;
            while (*e && *e != ',') e++;
            fl = (size_t)(e - s);
            while (fl && (s[fl - 1] == ' ' || s[fl - 1] == '\t')) fl--;
            while (fl && (*s == ' ' || *s == '\t')) {
                s++;
                fl--;
            }
            if (fl > best_fl && strncmp(ident, s, fl) == 0 && ident[fl] == '_') {
                best = s;
                best_fl = fl;
            }
            if (*e == ',') {
                s = e + 1;
                while (*s == ' ') s++;
            } else
                break;
        }
    }
    if (!best || !best_fl || best_fl >= fam_cap) return 0;
    memcpy(fam_out, best, best_fl);
    fam_out[best_fl] = 0;
    snprintf(compact_out, compact_cap, "%s", ident + best_fl + 1);
    return compact_out[0] ? 1 : 0;
}

static void shadow_ginst_need_from_type(const char* ty) {
    const char* p = ty;
    char ident[160];
    char fam[96];
    char compact[96];
    size_t n = 0;
    if (!p) return;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "const ", 6) == 0) {
        p += 6;
        while (*p == ' ' || *p == '\t') p++;
    }
    if (strncmp(p, "struct ", 7) == 0) {
        p += 7;
        while (*p == ' ' || *p == '\t') p++;
    }
    while (*p && (shadow_is_ident_char(*p)) && n + 1 < sizeof(ident))
        ident[n++] = *p++;
    ident[n] = 0;
    if (!shadow_ginst_match_family(ident, fam, sizeof(fam), compact,
                                  sizeof(compact)))
        return;
    if (strcmp(fam, "CCSlice") == 0)
        shadow_slice_need(compact);
    else
        shadow_ginst_need(fam, compact);
}

/* Returns 1 on success, 0 on soft failure, -1 on arg OOB (sets *oob_arg). */
static int shadow_inst_generic_tpl(const char* tpl, const char* mangled,
                                   char args[][64], int nargs, char* out,
                                   size_t cap, int* oob_arg) {
    size_t o = 0;
    const char* p = tpl ? tpl : "";
    if (oob_arg) *oob_arg = -1;
    while (*p && o + 1 < cap) {
        if (strncmp(p, "${mangled}", 10) == 0) {
            size_t n = strlen(mangled);
            if (o + n >= cap) return 0;
            memcpy(out + o, mangled, n);
            o += n;
            p += 10;
            continue;
        }
        if (strncmp(p, "${arg(", 6) == 0) {
            int idx = 0;
            const char* q = p + 6;
            while (*q >= '0' && *q <= '9') idx = idx * 10 + (*q++ - '0');
            if (*q == ')' && q[1] == '}') {
                /* Text templates are 0-based; compiled factories are 1-based.
                 * Accept both: idx==nargs with nargs>0 is also OOB for 1-based. */
                if (idx < 0 || idx >= nargs) {
                    if (oob_arg) *oob_arg = idx;
                    return -1;
                }
                size_t n = strlen(args[idx]);
                if (o + n >= cap) return 0;
                memcpy(out + o, args[idx], n);
                o += n;
                p = q + 2;
                continue;
            }
        }
        /* Product factory sugar: ${type_args.items[N]} → arg N. */
        if (strncmp(p, "${type_args.items[", 18) == 0) {
            int idx = 0;
            const char* q = p + 18;
            while (*q >= '0' && *q <= '9') idx = idx * 10 + (*q++ - '0');
            if (*q == ']' && q[1] == '}') {
                if (idx < 0 || idx >= nargs) {
                    if (oob_arg) *oob_arg = idx;
                    return -1;
                }
                size_t n = strlen(args[idx]);
                if (o + n >= cap) return 0;
                memcpy(out + o, args[idx], n);
                o += n;
                p = q + 2;
                continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    return 1;
}

static void shadow_vec_need(const char* mangled_t) {
    int i;
    if (!mangled_t || !mangled_t[0]) return;
    if (g_shadow_nvecs >= SHADOW_VEC_CAP) {
        shadow_table_full("vecs", SHADOW_VEC_CAP, mangled_t);
        return;
    }
    for (i = 0; i < g_shadow_nvecs; i++) {
        if (strcmp(g_shadow_vecs[i], mangled_t) == 0) return;
    }
    snprintf(g_shadow_vecs[g_shadow_nvecs], sizeof(g_shadow_vecs[0]), "%s",
             mangled_t);
    g_shadow_nvecs++;
}

/* e from AST_CHAN_VAR / AST_TYPEDEF_CHAN:
 *   "" | "o" | "t:TOPO" | "o:TOPO" | "owned"  plus optional ";s" ";dn" ";do". */
static void shadow_chan_flags_from_e(const char* e, int* ordered, char* topo,
                                     size_t topo_cap, int* is_sync,
                                     int* bp_mode) {
    const char* semi;
    char base[64];
    size_t bl;
    if (ordered) *ordered = 0;
    if (topo && topo_cap) topo[0] = 0;
    if (is_sync) *is_sync = 0;
    if (bp_mode) *bp_mode = 0;
    if (!e || !e[0]) return;
    semi = strchr(e, ';');
    bl = semi ? (size_t)(semi - e) : strlen(e);
    if (bl >= sizeof(base)) bl = sizeof(base) - 1;
    memcpy(base, e, bl);
    base[bl] = 0;
    if (strcmp(base, "owned") == 0) {
        /* owned channels are not pair-registered. */
    } else if (base[0] == 'o') {
        if (ordered) *ordered = 1;
        if (base[1] == ':' && topo && topo_cap > 1)
            snprintf(topo, topo_cap, "%s", base + 2);
    } else if (base[0] == 't' && base[1] == ':' && topo && topo_cap > 1) {
        snprintf(topo, topo_cap, "%s", base + 2);
    } else if (strcmp(base, "ordered") == 0) {
        if (ordered) *ordered = 1;
    }
    if (semi) {
        const char* p = semi;
        while (*p) {
            if (*p == ';') p++;
            if (p[0] == 's' && (p[1] == 0 || p[1] == ';')) {
                if (is_sync) *is_sync = 1;
                p += 1;
            } else if (p[0] == 'd' && p[1] == 'n' &&
                       (p[2] == 0 || p[2] == ';')) {
                if (bp_mode) *bp_mode = 1;
                p += 2;
            } else if (p[0] == 'd' && p[1] == 'o' &&
                       (p[2] == 0 || p[2] == ';')) {
                if (bp_mode) *bp_mode = 2;
                p += 2;
            } else {
                while (*p && *p != ';') p++;
            }
        }
    }
}

static void shadow_chan_register_ex(const char* name, const char* cap, int ordered,
                                    const char* elem, const char* topo,
                                    int is_sync, int bp_mode) {
    int i;
    if (!name || !name[0]) return;
    if (g_shadow_nchans >= SHADOW_CHAN_CAP) {
        shadow_table_full("chans", SHADOW_CHAN_CAP, name);
        return;
    }
    for (i = 0; i < g_shadow_nchans; i++) {
        if (strcmp(g_shadow_chans[i].name, name) == 0) {
            /* Last wins — same name reused in later blocks (fifo smoke). */
            snprintf(g_shadow_chans[i].cap, sizeof(g_shadow_chans[i].cap), "%s",
                     cap ? cap : "1");
            g_shadow_chans[i].ordered = ordered ? 1 : 0;
            g_shadow_chans[i].is_sync = is_sync ? 1 : 0;
            g_shadow_chans[i].bp_mode = bp_mode;
            if (elem && elem[0])
                snprintf(g_shadow_chans[i].elem, sizeof(g_shadow_chans[i].elem),
                         "%s", elem);
            g_shadow_chans[i].topo[0] = 0;
            if (topo && topo[0])
                snprintf(g_shadow_chans[i].topo, sizeof(g_shadow_chans[i].topo),
                         "%s", topo);
            return;
        }
    }
    snprintf(g_shadow_chans[g_shadow_nchans].name,
             sizeof(g_shadow_chans[0].name), "%s", name);
    snprintf(g_shadow_chans[g_shadow_nchans].cap,
             sizeof(g_shadow_chans[0].cap), "%s", cap ? cap : "1");
    g_shadow_chans[g_shadow_nchans].elem[0] = 0;
    if (elem && elem[0])
        snprintf(g_shadow_chans[g_shadow_nchans].elem,
                 sizeof(g_shadow_chans[0].elem), "%s", elem);
    g_shadow_chans[g_shadow_nchans].topo[0] = 0;
    if (topo && topo[0])
        snprintf(g_shadow_chans[g_shadow_nchans].topo,
                 sizeof(g_shadow_chans[0].topo), "%s", topo);
    g_shadow_chans[g_shadow_nchans].ordered = ordered ? 1 : 0;
    g_shadow_chans[g_shadow_nchans].is_sync = is_sync ? 1 : 0;
    g_shadow_chans[g_shadow_nchans].bp_mode = bp_mode;
    g_shadow_nchans++;
}

static void __attribute__((unused)) shadow_chan_register(const char* name, const char* cap, int ordered) {
    shadow_chan_register_ex(name, cap, ordered, NULL, NULL, 0, 0);
}

static void shadow_chan_register_node(AstNode* n) {
    int ordered = 0;
    int is_sync = 0;
    int bp_mode = 0;
    char topo[16];
    const char* p;
    char name[64];
    size_t ni;
    if (!n || !n->a[0]) return;
    if (strcmp(n->c, "owned") == 0) return; /* create_owned, not pair table */
    shadow_chan_flags_from_e(n->e, &ordered, topo, sizeof(topo), &is_sync,
                             &bp_mode);
    /* `a` may be comma-separated multi-declarator names. */
    p = n->a;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        ni = 0;
        while (*p && *p != ',' && *p != ' ' && *p != '\t' &&
               ni + 1 < sizeof(name))
            name[ni++] = *p++;
        name[ni] = 0;
        if (name[0])
            shadow_chan_register_ex(name, n->b, ordered, n->d[0] ? n->d : NULL,
                                    topo[0] ? topo : NULL, is_sync, bp_mode);
    }
}

static const ShadowChanDecl* shadow_chan_find(const char* name) {
    int i;
    if (!name) return NULL;
    for (i = 0; i < g_shadow_nchans; i++) {
        if (strcmp(g_shadow_chans[i].name, name) == 0) return &g_shadow_chans[i];
    }
    return NULL;
}

/* Resolve AstNode → (path, line). Returns 0 if unknown.
 * Prefer `#line`/CC_LN-aware logical coords so .shcc sites keep user lines. */
static char g_shadow_site_lpath[256];
static int shadow_site_loc(TapeCache* cache, AstNode* st, const char** path_out,
                           int* line_out) {
    if (!cache || !st || !st->file_id || !path_out || !line_out) return 0;
    FileTape* ft = tape_by_id(cache, st->file_id);
    if (!ft || !ft->path || !ft->bytes) return 0;
    int line = 1;
    char lfile[256];
    lfile[0] = 0;
    shadow_tape_logical_at(ft, st->tok_off, lfile, sizeof(lfile), &line);
    if (lfile[0]) {
        snprintf(g_shadow_site_lpath, sizeof(g_shadow_site_lpath), "%s", lfile);
        *path_out = g_shadow_site_lpath;
    } else {
        *path_out = ft->path;
    }
    *line_out = line;
    return 1;
}

/* Escape path for a `#line` / string literal (\\ and \"). */
static void shadow_escape_path(char* dst, size_t cap, const char* path) {
    if (!dst || !cap) return;
    size_t o = 0;
    for (const char* p = path ? path : ""; *p && o + 2 < cap; p++) {
        if (*p == '\\' || *p == '"') {
            if (o + 3 >= cap) break;
            dst[o++] = '\\';
        }
        dst[o++] = *p;
    }
    dst[o] = 0;
}

/* draft_as §5: bind handler face — exact E, else unique @as path to F. */
static int shadow_as_path_to(const char* outer, const char* face, char* path,
                             size_t cap) {
    int i;
    int hits = 0;
    const char* hit_field = NULL;
    if (!outer || !face || !path || !cap) return 0;
    path[0] = 0;
    if (strcmp(outer, face) == 0) return 1; /* exact — empty path */
    shadow_as_materialize_globs_for(outer);
    for (i = 0; i < g_shadow_nas; i++) {
        if (strcmp(g_shadow_as[i].outer, outer) != 0) continue;
        if (strcmp(g_shadow_as[i].target, face) != 0) continue;
        hits++;
        hit_field = g_shadow_as[i].field;
    }
    if (hits == 1 && hit_field) {
        snprintf(path, cap, "%s", hit_field);
        return 1;
    }
    if (hits > 1) return -1; /* ambiguous */
    return 0;
}

/* Err face of CCResult_* from rfn table or common suffix. */
static int shadow_rname_err_face(const char* rname, char* out, size_t cap) {
    int i;
    size_t n;
    static const char* faces[] = {"CCIoError", "CCError", "CCNetError",
                                  "CCJsError", "CCPyError", NULL};
    if (!rname || !rname[0] || !out || !cap) return 0;
    out[0] = 0;
    for (i = 0; i < g_shadow_nrfns; i++) {
        if (strcmp(g_shadow_rfns[i].rname, rname) == 0 &&
            g_shadow_rfns[i].err[0]) {
            snprintf(out, cap, "%s", g_shadow_rfns[i].err);
            return 1;
        }
    }
    n = strlen(rname);
    for (i = 0; faces[i]; i++) {
        size_t fl = strlen(faces[i]);
        if (n > fl + 1 && rname[n - fl - 1] == '_' &&
            memcmp(rname + n - fl, faces[i], fl) == 0) {
            snprintf(out, cap, "%s", faces[i]);
            return 1;
        }
    }
    return 0;
}

/* Project cc_err(arg) into Result error face F via unique @as.
 * Returns 1 and writes projected expr; 0 leave arg as-is; -1 ambiguous. */
static int shadow_cc_err_as_project(const char* err_face, const char* arg,
                                    char* out, size_t cap) {
    int ai, nas = 0;
    char arms[768];
    size_t ao = 0;
    char trimmed[288];
    const char* a;
    size_t al;
    size_t i;
    int is_id;
    if (!err_face || !err_face[0] || !arg || !out || !cap) return 0;
    out[0] = 0;
    a = arg;
    while (*a == ' ' || *a == '\t' || *a == '\n' || *a == '\r') a++;
    al = strlen(a);
    while (al && (a[al - 1] == ' ' || a[al - 1] == '\t' || a[al - 1] == '\n' ||
                  a[al - 1] == '\r'))
        al--;
    if (!al || al >= sizeof(trimmed)) return 0;
    memcpy(trimmed, a, al);
    trimmed[al] = 0;
    for (ai = 0; ai < g_shadow_nas; ai++) {
        if (strcmp(g_shadow_as[ai].target, err_face) == 0) nas++;
    }
    if (nas == 0) return 0;
    /* Simple ident with a known bind: project `.path` when unique. */
    is_id = shadow_is_id0(trimmed[0]);
    for (i = 1; is_id && i < al; i++) {
        if (!shadow_is_id(trimmed[i])) is_id = 0;
    }
    if (is_id) {
        const ShadowBind* b = shadow_bind_lookup(trimmed);
        char path[128];
        int pr;
        if (b && b->ty[0] && strcmp(b->ty, err_face) != 0) {
            pr = shadow_as_path_to(b->ty, err_face, path, sizeof(path));
            if (pr < 0) {
                fprintf(stderr,
                        "error: cc_err: ambiguous as: path from '%s' to '%s'\n",
                        b->ty, err_face);
                return -1;
            }
            if (pr && path[0]) {
                snprintf(out, cap, "%s.%s", trimmed, path);
                return 1;
            }
            /* Unknown source type — fall through to _Generic. */
        }
    }
    /* Expression / unbound: stmt-expr + _Generic (offset-0 cast, same as
     * @errhandler binder projection). */
    ao = (size_t)snprintf(arms, sizeof(arms), "%s: __cc_ep", err_face);
    for (ai = 0; ai < g_shadow_nas && ao + 96 < sizeof(arms); ai++) {
        int n;
        if (strcmp(g_shadow_as[ai].target, err_face) != 0) continue;
        if (strchr(g_shadow_as[ai].field, '.'))
            n = snprintf(arms + ao, sizeof(arms) - ao, ", %s: (__cc_ep.%s)",
                         g_shadow_as[ai].outer, g_shadow_as[ai].field);
        else
            n = snprintf(arms + ao, sizeof(arms) - ao,
                         ", %s: (*(%s*)(void*)&__cc_ep)",
                         g_shadow_as[ai].outer, err_face);
        if (n < 0 || (size_t)n >= sizeof(arms) - ao) break;
        ao += (size_t)n;
    }
    snprintf(out, cap,
             "({ __typeof__(%s) __cc_ep = (%s); "
             "_Generic(__cc_ep, %s, default: __cc_ep); })",
             trimmed, trimmed, arms);
    return 1;
}

/* Bake file/line as string literals (production shape). `__LINE__` is an int
 * and breaks once `#line` is active if used as an int diag arg. */
static int shadow_emit_err_at_bind_tmp(CEmit* out, ShadowCtx* ctx,
                                       const char* indent, const char* bind,
                                       const char* site, const char* rtmp) {
    const char* path = "unknown";
    int line = 0;
    char esc[512];
    const char* eh_ty =
        (ctx && ctx->eh && ctx->eh->a[0]) ? ctx->eh->a : "CCError";
    const char* r = (rtmp && rtmp[0]) ? rtmp : "__r";
    char arms[768];
    size_t ao = 0;
    int ai;
    int nas = 0;
    (void)site;
    if (ctx && ctx->site)
        (void)shadow_site_loc(ctx->cache, ctx->site, &path, &line);
    if (!path || !path[0]) path = "unknown";
    shadow_escape_path(esc, sizeof(esc), path);
    /* Unique as: path from unwrap E to handler F (2-hop faces, not offset-0). */
    if (ctx && ctx->eh_proj[0]) {
        return cemit_fmt(out,
            "%scc_rt_diag_record_unwrap_site(\"%s\", \"%d\");\n"
            "%s%s %s = (%s).u.error.%s;\n",
            indent, esc, line, indent, eh_ty, bind, r, ctx->eh_proj);
    }
    /* draft_as §5: exact E, else unique @as face (offset-0 cast — all
     * _Generic arms are type-checked). Arms come from the @as registry. */
    arms[0] = 0;
    ao += (size_t)snprintf(arms + ao, sizeof(arms) - ao, "%s: (%s).u.error",
                           eh_ty, r);
    for (ai = 0; ai < g_shadow_nas && ao + 80 < sizeof(arms); ai++) {
        if (strcmp(g_shadow_as[ai].target, eh_ty) != 0) continue;
        if (strchr(g_shadow_as[ai].field, '.')) continue;
        ao += (size_t)snprintf(
            arms + ao, sizeof(arms) - ao,
            ", %s: (*(%s*)(void*)&(%s).u.error)", g_shadow_as[ai].outer,
            eh_ty, r);
        nas++;
    }
    if (nas > 0) {
        return cemit_fmt(out,
            "%scc_rt_diag_record_unwrap_site(\"%s\", \"%d\");\n"
            "%s%s %s = _Generic((%s).u.error, %s, default: (%s).u.error);\n",
            indent, esc, line, indent, eh_ty, bind, r, arms, r);
    }
    return cemit_fmt(out,
        "%scc_rt_diag_record_unwrap_site(\"%s\", \"%d\");\n"
        "%s%s %s = (%s).u.error;\n",
        indent, esc, line, indent, eh_ty, bind, r);
}

static int shadow_emit_err_at_bind(CEmit* out, ShadowCtx* ctx, const char* indent,
                                   const char* bind, const char* site) {
    return shadow_emit_err_at_bind_tmp(out, ctx, indent, bind, site, "__r");
}

/* Pointer/null unwrap: host `_Generic` `__cc_uw_err_at` (Result + null).
 * Result-only typed binds use shadow_emit_err_at_bind instead. */
static int shadow_emit_ptr_err_at_bind(CEmit* out, ShadowCtx* ctx,
                                       const char* indent, const char* bind,
                                       const char* site) {
    const char* path = "unknown";
    int line = 0;
    char esc[512];
    char site_esc[512];
    const char* eh_ty =
        (ctx && ctx->eh && ctx->eh->a[0]) ? ctx->eh->a : "CCError";
    if (ctx && ctx->site)
        (void)shadow_site_loc(ctx->cache, ctx->site, &path, &line);
    if (!path || !path[0]) path = "unknown";
    shadow_escape_path(esc, sizeof(esc), path);
    shadow_escape_path(site_esc, sizeof(site_esc),
                       site && site[0] ? site : "unwrap");
    /* Non-CCError handlers are Result-shaped — field bind + @as face. */
    if (strcmp(eh_ty, "CCError") != 0)
        return shadow_emit_err_at_bind(out, ctx, indent, bind, site);
    /* Infer bind type from the Result err arm (CCError / CCIoError / …). */
    return cemit_fmt(out,
        "%s__typeof__(__cc_uw_err_at(__r, \"%s\", \"%s\", \"%d\")) %s = "
        "__cc_uw_err_at(__r, \"%s\", \"%s\", \"%d\");\n",
        indent, site_esc, esc, line, bind, site_esc, esc, line);
}

/* `#line N "path"` when site differs from the last marker.
 * Indent with surrounding code (`#` is still the first non-whitespace).
 * Only whitespace may prefix `#line` — a non-space indent glues into the
 * directive (native self-emit once produced `de#line` from a bad indent). */
static int shadow_emit_line(CEmit* out, ShadowCtx* ctx, AstNode* st,
                            const char* indent) {
    if (!out || !ctx) return 1;
    if (g_shadow_no_line) return 1;
    const char* path = NULL;
    int line = 0;
    if (!shadow_site_loc(ctx->cache, st, &path, &line) || line <= 0) return 1;
    if (ctx->line_file_id == st->file_id && ctx->line_no == line) return 1;
    char esc[512];
    shadow_escape_path(esc, sizeof(esc), path);
    if (indent) {
        const char* p;
        for (p = indent; *p; p++) {
            if (*p != ' ' && *p != '\t') {
                indent = "";
                break;
            }
        }
    }
    if (!cemit_fmt(out, "%s#line %d \"%s\"\n", indent ? indent : "", line, esc))
        return 0;
    ctx->line_file_id = st->file_id;
    ctx->line_no = line;
    ctx->site = st;
    return 1;
}

/* Force a `#line` resync for the current site (multi-line synthetic blocks). */
static int shadow_resync_line(CEmit* out, ShadowCtx* ctx, const char* indent) {
    if (!ctx || !ctx->site) return 1;
    int saved_file = ctx->line_file_id;
    int saved_line = ctx->line_no;
    ctx->line_file_id = 0;
    ctx->line_no = 0;
    int ok = shadow_emit_line(out, ctx, ctx->site, indent);
    if (!ok) {
        ctx->line_file_id = saved_file;
        ctx->line_no = saved_line;
    }
    return ok;
}

/* Pin the next physical line to a real user site. Unlike shadow_emit_line,
 * always re-emits `#line` even when the site matches the last marker — so a
 * multi-line synthetic expansion can stay on one source line (cpp would
 * otherwise walk N, N+1, … past EOF). `st` NULL → ctx->site. */
static int shadow_pin_line(CEmit* out, ShadowCtx* ctx, AstNode* st,
                           const char* indent) {
    AstNode* site = st ? st : (ctx ? ctx->site : NULL);
    AstNode* saved;
    int ok;
    if (!out || !ctx || !site) return 1;
    saved = ctx->site;
    ctx->site = site;
    ctx->line_file_id = 0;
    ctx->line_no = 0;
    ok = shadow_emit_line(out, ctx, site, indent);
    if (!ok) ctx->site = saved;
    return ok;
}

/* Emit `block` (newline-terminated lines) with `#line` re-pinned to `st`
 * before every physical line — until the block's OWN ledger takes over:
 * a hoisted closure def carries correct `#line`/CC_LN anchors for the
 * copied user body (stamped by the inner stmt emitters), and re-pinning
 * past the first one re-attributes every body diagnostic to the literal's
 * line.  Pin the unanchored scaffolding prologue, then trust the embedded
 * ledger for the rest; a block with no anchors pins throughout. */
static int shadow_emit_pinned_block(CEmit* out, ShadowCtx* ctx, AstNode* st,
                                    const char* indent, const char* block) {
    const char* p;
    const char* line;
    int ledger_live = 0;
    if (!out || !block) return 0;
    p = block;
    while (*p) {
        line = p;
        while (*p && *p != '\n') p++;
        if (!ledger_live) {
            const char* q = line;
            while (q < p && (*q == ' ' || *q == '\t')) q++;
            if ((q + 5 <= p && strncmp(q, "#line", 5) == 0) ||
                (q + 8 <= p && strncmp(q, "/*CC_LN ", 8) == 0))
                ledger_live = 1;
            else if (!shadow_pin_line(out, ctx, st, indent))
                return 0;
        }
        if (*p == '\n') {
            if (!cemit_buf(out, line, (size_t)(p - line) + 1)) return 0;
            p++;
        } else if (!cemit_str(out, line)) {
            return 0;
        }
    }
    return 1;
}

/* Prefer indent of first real stmt; else 4 spaces. */
static void shadow_pick_body_indent(AstNode** kids, int nkids, char* dst,
                                    size_t cap) {
    for (int k = 0; k < nkids; k++) {
        AstNode* st = kids[k];
        if (!st) continue;
        if (st->kind == AST_ERRHANDLER || st->kind == AST_DEFER) continue;
        if (st->indent[0]) {
            snprintf(dst, cap, "%s", st->indent);
            return;
        }
    }
    snprintf(dst, cap, "    ");
}

/* Indent unit matching source style (tab or 2/4 spaces). */
static void shadow_indent_unit(const char* base, char* unit, size_t ucap) {
    if (!unit || !ucap) return;
    if (base) {
        for (const char* p = base; *p; p++) {
            if (*p == '\t') {
                snprintf(unit, ucap, "\t");
                return;
            }
        }
        size_t n = strlen(base);
        if (n > 0 && n % 2 == 0 && n % 4 != 0) {
            snprintf(unit, ucap, "  ");
            return;
        }
    }
    snprintf(unit, ucap, "    ");
}

/* base + levels of nest, preferring the statement's source indent. */
static void shadow_indent_nest(char* dst, size_t cap, const char* base, int levels) {
    if (!dst || !cap) return;
    snprintf(dst, cap, "%s", base ? base : "");
    char unit[8];
    shadow_indent_unit(base, unit, sizeof(unit));
    for (int i = 0; i < levels; i++) {
        size_t cur = strlen(dst);
        size_t ul = strlen(unit);
        if (cur + ul + 1 >= cap) break;
        memcpy(dst + cur, unit, ul + 1);
    }
}

/* Sticky post-include lead for first decl (file-header split). Never mutates
 * AstNode.lead_* — trivia attached at parse stays the source of truth. */
static size_t g_sticky_lead_off;
static size_t g_sticky_lead_len;
static int g_sticky_lead_fid;
static int g_sticky_lead_set;
/* 1 = file-header pass already printed items[0] lead (banner and/or sticky). */
static int g_file_header_lead_done;

/* Emit comments/blank lines from a tape span (does not mutate AST).
 * Skip `#…` preprocessor lines — those are re-emitted via pass_inc.
 * file_header=1: only the banner before the first `#…`; remainder goes to
 * g_sticky_lead_* for the first decl preamble. */
static int shadow_emit_lead_span(CEmit* out, TapeCache* cache, int file_id,
                                size_t lead_off, size_t lead_len,
                                int file_header) {
    if (!lead_len || !cache) return 1;
    FileTape* ft = tape_by_id(cache, file_id);
    if (!ft || !ft->bytes) return 1;
    size_t off = lead_off;
    size_t len = lead_len;
    if (off + len > ft->len) return 1;
    while (len > 0) {
        char c = ft->bytes[off + len - 1];
        if (c == ' ' || c == '\t') len--;
        else break;
    }
    if (len > 0 && ft->bytes[off] == '\n') {
        off++;
        len--;
    }
    if (!len) return 1;
    size_t i = 0;
    int seen_hash = 0;
    while (i < len) {
        size_t ls = i;
        while (i < len && ft->bytes[off + i] != '\n') i++;
        size_t le = i;
        int has_nl = (i < len);
        if (has_nl) i++;
        size_t j = ls;
        while (j < le && (ft->bytes[off + j] == ' ' || ft->bytes[off + j] == '\t'))
            j++;
        if (j < le && ft->bytes[off + j] == '#') {
            seen_hash = 1;
            continue;
        }
        /* Omit stage-2 skipped arms (shared tapes are not blanked). */
        if (pp_skip_span_overlaps(file_id, off + ls, le - ls)) continue;
        if (file_header && seen_hash) {
            g_sticky_lead_off = off + ls;
            g_sticky_lead_len = len - ls;
            g_sticky_lead_fid = file_id;
            g_sticky_lead_set = 1;
            g_file_header_lead_done = 1;
            return 1;
        }
        size_t n = (has_nl ? i : le) - ls;
        if (!cemit_reserve(out, n)) return 0;
        memcpy(out->buf + out->len, ft->bytes + off + ls, n);
        out->len += n;
        out->buf[out->len] = 0;
    }
    if (file_header) g_file_header_lead_done = 1;
    return 1;
}

static int shadow_emit_preamble_ex(AstNode* st, CEmit* out, TapeCache* cache,
                                   int file_header) {
    if (!st) return 1;
    return shadow_emit_lead_span(out, cache, st->file_id, st->lead_off,
                                 st->lead_len, file_header);
}

static int shadow_emit_preamble(AstNode* st, CEmit* out, TapeCache* cache) {
    /* Prefer sticky remainder from file-header split (first decl). */
    if (g_sticky_lead_set) {
        int ok = shadow_emit_lead_span(out, cache, g_sticky_lead_fid,
                                       g_sticky_lead_off, g_sticky_lead_len, 0);
        g_sticky_lead_set = 0;
        g_file_header_lead_done = 0;
        return ok;
    }
    if (g_file_header_lead_done) {
        /* Banner-only lead already printed; node trivia left intact. */
        g_file_header_lead_done = 0;
        return 1;
    }
    return shadow_emit_preamble_ex(st, out, cache, 0);
}

/* Prefer source line indent when present.
 * Explicit empty fallback means same-line continuation (e.g. `if (c) stmt`). */
static const char* shadow_stmt_indent(AstNode* st, const char* fallback) {
    if (fallback && !fallback[0]) return fallback;
    if (st && st->indent[0]) return st->indent;
    return fallback ? fallback : "";
}
