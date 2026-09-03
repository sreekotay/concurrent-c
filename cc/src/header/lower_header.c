/*
 * lower_header.c - Header lowering implementation
 *
 * Transforms .cch files to .h files by rewriting CC type syntax
 * and emitting guarded type declarations.
 */

#include "lower_header.h"

#include "util/text.h"
#include "util/text_scan.h"
#include "preprocess/template_scan.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "preprocess/preprocess.h"
#include "preprocess/variant_lower.h"
#include "preprocess/unit_header.h"
#include "preprocess/type_graph.h"
#include "preprocess/emit_plan.h"
#include "util/io.h"
#include "util/result_fn_registry.h"
#include "util/text.h"
#include "visitor/pass_err_syntax.h"
#include "visitor/pass_result_unwrap.h"
#include "visitor/pass_type_syntax.h"
#include "visitor/visitor.h"

/* Mangle a type name for use in CCResult_T_E */
static void cc__mangle_type(const char* src, size_t len, char* out, size_t out_sz) {
    cc_result_spec_mangle_type(src, len, out, out_sz);
}

/* Scan back from position to find type start (delimited by ; { } , ( ) newline).
 * Skips over block comments so text inside them (including '*' continuations
 * that mangle to '_ptr_') doesn't get absorbed into the type name. */
static size_t cc__scan_back_to_type_start(const char* s, size_t from) {
    size_t i = from;
    int in_block_comment = 0;
    while (i > 0) {
        if (in_block_comment) {
            if (i >= 2 && s[i - 2] == '/' && s[i - 1] == '*') {
                in_block_comment = 0;
                i -= 2;
                continue;
            }
            i--;
            continue;
        }
        if (i >= 2 && s[i - 2] == '*' && s[i - 1] == '/') {
            in_block_comment = 1;
            i -= 2;
            continue;
        }
        char p = s[i - 1];
        if (p == ';' || p == '{' || p == '}' || p == ',' || p == '(' || p == ')' || p == '\n') break;
        i--;
    }
    while (s[i] && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

static int cc__keyword_eq(const char* s, size_t len, const char* kw) {
    return strlen(kw) == len && strncmp(s, kw, len) == 0;
}

static int cc__is_leading_type_prefix_keyword(const char* s, size_t len) {
    return cc__keyword_eq(s, len, "static") ||
           cc__keyword_eq(s, len, "extern") ||
           cc__keyword_eq(s, len, "inline") ||
           cc__keyword_eq(s, len, "__inline") ||
           cc__keyword_eq(s, len, "__inline__") ||
           cc__keyword_eq(s, len, "register") ||
           cc__keyword_eq(s, len, "auto") ||
           cc__keyword_eq(s, len, "const") ||
           cc__keyword_eq(s, len, "volatile");
}

static size_t cc__skip_leading_type_prefix_keywords(const char* s, size_t start, size_t end) {
    size_t i = start;
    while (i < end) {
        size_t kw_start = i;
        size_t kw_end;
        while (kw_start < end && (s[kw_start] == ' ' || s[kw_start] == '\t')) kw_start++;
        kw_end = kw_start;
        while (kw_end < end && (isalnum((unsigned char)s[kw_end]) || s[kw_end] == '_')) kw_end++;
        if (kw_end == kw_start) break;
        if (!cc__is_leading_type_prefix_keyword(s + kw_start, kw_end - kw_start)) break;
        i = kw_end;
        while (i < end && (s[i] == ' ' || s[i] == '\t')) i++;
    }
    return i;
}

void cc_lower_state_init(CCLowerState* state) {
    if (state) {
        memset(state, 0, sizeof(*state));
        cc_result_spec_table_init(&state->result_specs);
    }
}

void cc_lower_state_add_result(CCLowerState* state,
                                const char* ok_type, size_t ok_len,
                                const char* err_type, size_t err_len,
                                const char* mangled_ok,
                                const char* mangled_err) {
    if (!state || !ok_type || !err_type || !mangled_ok || !mangled_err) return;
    if (cc_result_spec_is_core_builtin(mangled_ok, mangled_err) ||
        cc_result_spec_is_stdlib_predeclared(mangled_ok, mangled_err)) return;
    (void)cc_result_spec_table_add(&state->result_specs,
                                   ok_type, ok_len, err_type, err_len,
                                   mangled_ok, mangled_err);
}

char* cc_lower_state_emit_decls(const CCLowerState* state) {
    if (!state) return NULL;
    if (state->result_specs.count == 0) return NULL;

    char* out = NULL;
    size_t out_len = 0, out_cap = 0;

    /* Emit result type declarations */
    for (size_t i = 0; i < state->result_specs.count; i++) {
        const CCResultSpec* p = cc_result_spec_table_get(&state->result_specs, i);
        char decl[512];
        if (!p) continue;
        cc_result_spec_emit_decl(p, decl, sizeof(decl));
        cc_sb_append_cstr(&out, &out_len, &out_cap, decl);
    }
    
    return out;
}

/*
 * Rewrite T!>(E) syntax to CCResult_T_E and collect type pairs.
 */
/* First occurrence of `name` as a whole identifier in code — comments and
 * string literals do not count, so a box named only in prose is not mistaken
 * for a use. */
static const char* cc__find_ident_top_level(const char* s, size_t n, const char* name) {
    size_t nl = name ? strlen(name) : 0;
    size_t i = 0;
    CCInertScan scan;
    if (!s || !name || nl == 0) return NULL;
    cc_inert_scan_init(&scan, NULL);
    scan.at_line_start = 0;
    while (i < n) {
        if (cc_inert_scan_step(&scan, s, n, &i)) continue;
        if (i + nl <= n && memcmp(s + i, name, nl) == 0 &&
            (i == 0 || !cc_is_ident_char(s[i - 1])) &&
            (i + nl == n || !cc_is_ident_char(s[i + nl])))
            return s + i;
        i++;
    }
    return NULL;
}

/* Where a declaration may be spliced so that it precedes `use_pos`: the start
 * of the last line that began at brace depth 0.  A use inside a function body
 * or a struct body therefore anchors to the top-level construct containing it,
 * never to a point where a declaration would be ill-formed. */
static size_t cc__decl_insert_point(const char* s, size_t use_pos) {
    size_t i = 0, depth = 0, line_start = 0, anchor = 0;
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    scan.at_line_start = 0;
    while (i < use_pos) {
        size_t before = i;
        if (cc_inert_scan_step(&scan, s, use_pos, &i)) {
            /* A skipped run may span newlines; resync the line start. */
            for (size_t k = before; k < i && k < use_pos; k++)
                if (s[k] == '\n') { line_start = k + 1; if (depth == 0) anchor = line_start; }
            continue;
        }
        if (s[i] == '\n') {
            line_start = i + 1;
            if (depth == 0) anchor = line_start;
        } else if (s[i] == '{') depth++;
        else if (s[i] == '}') { if (depth) depth--; }
        i++;
    }
    (void)line_start;
    return anchor;
}

static int cc__header_vec_prebaked(const char* mangled) {
    return mangled &&
           (strcmp(mangled, "CCVec_char") == 0 ||
            strcmp(mangled, "CCVec_size_t") == 0);
}

/* Splice CC_VEC_DECL_ARENA for Vec::[T] uses so the lowered .h is host C
 * without waiting for a TU factory harvest. Each instance lands after its
 * element type (and before first use), not as one block at the file top.
 * Guest extract may have an empty type graph — also scan rewritten text. */
static int cc__header_vec_already(const char names[][96], size_t n,
                                  const char* mangled) {
    size_t i;
    if (!mangled) return 1;
    for (i = 0; i < n; i++)
        if (strcmp(names[i], mangled) == 0) return 1;
    return 0;
}

static int cc__header_vec_push(char names[][96], char elems[][64],
                               size_t *nitems, size_t cap,
                               const char* mangled, const char* type1) {
    if (!mangled || !type1 || !nitems || *nitems >= cap) return 0;
    if (cc__header_vec_prebaked(mangled)) return 0;
    if (cc__header_vec_already(names, *nitems, mangled)) return 0;
    snprintf(names[*nitems], 96, "%s", mangled);
    snprintf(elems[*nitems], 64, "%s", type1);
    (*nitems)++;
    return 1;
}

static void cc__header_scan_ccvec_types(const char* src, size_t n,
                                        char names[][96], char elems[][64],
                                        size_t *nitems, size_t cap) {
    size_t i = 0;
    if (!src) return;
    while (i + 6 < n) {
        size_t e, k, elen;
        char mangled[96];
        char elem[64];
        if ((i > 0 && cc_is_ident_char(src[i - 1])) ||
            memcmp(src + i, "CCVec_", 6) != 0) {
            i++;
            continue;
        }
        e = i + 6;
        while (e < n && cc_is_ident_char(src[e])) e++;
        k = e;
        while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
        /* `CCVec_T_init(` is a call, not a type use. */
        if (k < n && src[k] == '(') {
            i = e;
            continue;
        }
        elen = e - (i + 6);
        if (elen == 0 || elen >= sizeof(elem) || e - i >= sizeof(mangled)) {
            i = e;
            continue;
        }
        memcpy(mangled, src + i, e - i);
        mangled[e - i] = 0;
        memcpy(elem, src + i + 6, elen);
        elem[elen] = 0;
        cc__header_vec_push(names, elems, nitems, cap, mangled, elem);
        i = e;
    }
}

static char* cc__header_vec_decl_text(const char* type1, const char* mangled) {
    char guard[256];
    char* text = NULL;
    size_t tlen = 0, tcap = 0;
    if (!type1 || !mangled) return NULL;
    snprintf(guard, sizeof(guard), "CC_HEADER_VEC_%s", mangled);
    cc_sb_append_cstr(&text, &tlen, &tcap,
                      "/* --- CC auto-generated Vec instance --- */\n#ifndef ");
    cc_sb_append_cstr(&text, &tlen, &tcap, guard);
    cc_sb_append_cstr(&text, &tlen, &tcap, "\n#define ");
    cc_sb_append_cstr(&text, &tlen, &tcap, guard);
    cc_sb_append_cstr(&text, &tlen, &tcap, "\nCC_VEC_DECL_ARENA(");
    cc_sb_append_cstr(&text, &tlen, &tcap, type1);
    cc_sb_append_cstr(&text, &tlen, &tcap, ", ");
    cc_sb_append_cstr(&text, &tlen, &tcap, mangled);
    cc_sb_append_cstr(&text, &tlen, &tcap, ")\nstatic inline ");
    cc_sb_append_cstr(&text, &tlen, &tcap, mangled);
    cc_sb_append_cstr(&text, &tlen, &tcap, " ");
    cc_sb_append_cstr(&text, &tlen, &tcap, mangled);
    cc_sb_append_cstr(&text, &tlen, &tcap,
                      "_new(CCArena __a) {\n    return ");
    cc_sb_append_cstr(&text, &tlen, &tcap, mangled);
    cc_sb_append_cstr(&text, &tlen, &tcap, "_init(__a, 0);\n}\n#endif\n");
    return text;
}

static char* cc__splice_header_vec_decls(const char* src, size_t n) {
    typedef struct { size_t pos; char* text; } CCVecAt;
    CCTypeGraph* g = cc_type_graph_get_global();
    size_t nv = 0, i, nfound = 0, nitems = 0;
    char names[64][96];
    char elems[64][64];
    CCVecAt items[64];
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t last = 0;
    if (!src) return NULL;
    if (g) {
        nv = cc_type_graph_vec_count(g);
        for (i = 0; i < nv && nfound < 64; i++) {
            const CCTypeInstantiation* inst = cc_type_graph_get_vec(g, i);
            if (!inst || !inst->mangled_name || !inst->type1) continue;
            cc__header_vec_push(names, elems, &nfound, 64,
                                inst->mangled_name, inst->type1);
        }
    }
    cc__header_scan_ccvec_types(src, n, names, elems, &nfound, 64);
    for (i = 0; i < nfound && nitems < 64; i++) {
        const char* hit;
        size_t decl_end, pos;
        char* text;
        if (cc__header_vec_prebaked(names[i])) continue;
        decl_end = cc_emit_plan_type_decl_end_top_level(src, n, elems[i]);
        hit = cc__find_ident_top_level(src, n, names[i]);
        if (!hit) hit = (const char*)memmem(src, n, "__CC_VEC(", 9);
        pos = 0;
        if (hit)
            pos = cc__decl_insert_point(src, (size_t)(hit - src));
        if (decl_end > pos) pos = decl_end;
        if (pos == 0 && !hit) continue;
        text = cc__header_vec_decl_text(elems[i], names[i]);
        if (!text) continue;
        items[nitems].pos = pos;
        items[nitems].text = text;
        nitems++;
    }
    if (nitems == 0) return NULL;
    for (i = 1; i < nitems; i++) {
        CCVecAt tmp = items[i];
        size_t b = i;
        while (b > 0 && items[b - 1].pos > tmp.pos) {
            items[b] = items[b - 1];
            b--;
        }
        items[b] = tmp;
    }
    for (i = 0; i < nitems; i++) {
        if (items[i].pos > last)
            cc_sb_append(&out, &out_len, &out_cap, src + last, items[i].pos - last);
        if (items[i].text) {
            cc_sb_append_cstr(&out, &out_len, &out_cap, items[i].text);
            free(items[i].text);
        }
        last = items[i].pos;
    }
    if (last < n)
        cc_sb_append(&out, &out_len, &out_cap, src + last, n - last);
    return out;
}

static int cc__header_strip_result_suffix(char* name) {
    static const char* suf[] = {
        "_is_err", "_is_ok", "_unwrap_err", "_unwrap", "_value",
        "_error", "_DEFINED", NULL
    };
    size_t n;
    int i;
    if (!name) return 0;
    n = strlen(name);
    for (i = 0; suf[i]; i++) {
        size_t sl = strlen(suf[i]);
        if (n > sl && strcmp(name + n - sl, suf[i]) == 0) {
            name[n - sl] = 0;
            return 1;
        }
    }
    return 0;
}

static void cc__header_add_result_from_concrete(CCLowerState* state,
                                                const char* concrete) {
    char work[256];
    const char* rest;
    const char* us;
    char ok[128], err[128], mok[128], merr[128];
    size_t ol;
    if (!state || !concrete || strncmp(concrete, "CCResult_", 9) != 0) return;
    snprintf(work, sizeof(work), "%s", concrete);
    cc__header_strip_result_suffix(work);
    if (cc_result_spec_table_find_by_name(&state->result_specs, work)) return;
    rest = work + 9;
    us = strrchr(rest, '_');
    if (!us || us <= rest) return;
    ol = (size_t)(us - rest);
    if (ol >= sizeof(ok)) ol = sizeof(ok) - 1;
    memcpy(ok, rest, ol);
    ok[ol] = 0;
    snprintf(err, sizeof(err), "%s", us + 1);
    cc_result_spec_mangle_type(ok, strlen(ok), mok, sizeof(mok));
    cc_result_spec_mangle_type(err, strlen(err), merr, sizeof(merr));
    cc_lower_state_add_result(state, ok, strlen(ok), err, strlen(err), mok,
                              merr);
}

/* After unwrap rewrite, every CCResult_* the header names must have a
 * guarded spec in this file — not only T!>(E) on a declarator. */
static void cc__header_collect_ccresult_idents(const char* src, size_t n,
                                               CCLowerState* state) {
    size_t i = 0;
    CCInertScan scan;
    if (!src || !state || n == 0) return;
    cc_inert_scan_init(&scan, NULL);
    scan.at_line_start = 0;
    while (i < n) {
        if (cc_inert_scan_step(&scan, src, n, &i)) continue;
        if (i + 9 <= n && memcmp(src + i, "CCResult_", 9) == 0 &&
            (i == 0 || !cc_is_ident_char(src[i - 1]))) {
            size_t j = i + 9;
            char name[256];
            while (j < n && cc_is_ident_char(src[j])) j++;
            if (j - i >= sizeof(name)) {
                i++;
                continue;
            }
            memcpy(name, src + i, j - i);
            name[j - i] = 0;
            cc__header_add_result_from_concrete(state, name);
            i = j;
            continue;
        }
        i++;
    }
}

static int cc__header_has_op(const char* src, size_t n, const char* op) {
    size_t opl;
    size_t i = 0;
    CCInertScan scan;
    if (!src || !op || n < 2) return 0;
    opl = strlen(op);
    if (n < opl) return 0;
    cc_inert_scan_init(&scan, NULL);
    scan.at_line_start = 0;
    while (i + opl <= n) {
        if (cc_inert_scan_step(&scan, src, n, &i)) continue;
        if (memcmp(src + i, op, opl) == 0) return 1;
        i++;
    }
    return 0;
}

/* Registry rows whose callee appears in this header and that header
 * unwraps — the pair travels with the header even when rewrite uses
 * field accessors and never spells CCResult_*. */
static void cc__header_collect_unwrap_fn_pairs(const char* src, size_t n,
                                               CCLowerState* state) {
    size_t i, count;
    if (!src || !state || n == 0) return;
    if (!cc__header_has_op(src, n, "!>") && !cc__header_has_op(src, n, "?>"))
        return;
    count = cc_result_fn_registry_count();
    for (i = 0; i < count; i++) {
        const char* fn = cc_result_fn_registry_name_at(i);
        const char* rn = cc_result_fn_registry_result_type_at(i);
        if (!fn || !fn[0] || !rn || !rn[0]) continue;
        if (!cc__find_ident_top_level(src, n, fn)) continue;
        cc__header_add_result_from_concrete(state, rn);
    }
}

/* Same rule as preprocess `cc__bang_unwrap_is_stmt`: `!>(e) {` / `!>;`
 * is an unwrap, not a type-position `T !>(E)`. */
static int cc__header_bang_unwrap_is_stmt(const char* src, size_t n, size_t i) {
    size_t j, rp = 0;
    if (!src || i + 1 >= n || src[i] != '!' || src[i + 1] != '>') return 0;
    j = i + 2;
    while (j < n && (src[j] == ' ' || src[j] == '\t' ||
                     src[j] == '\n' || src[j] == '\r'))
        j++;
    if (j >= n || src[j] != '(') return 1;
    if (!cc_find_matching_paren(src, n, j, &rp)) return 1;
    j = cc_skip_ws_and_comments(src, n, rp + 1);
    return (j < n && (src[j] == '{' || src[j] == ';'));
}

static char* cc__lower_result_types(const char* src, size_t n, CCLowerState* state) {
    if (!src || n == 0) return NULL;
    
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0, last_emit = 0;
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    /* Headers are lowered before any CC construct can live in a directive
     * body; keeping '#' lines code-visible matches the old scanner. */
    scan.at_line_start = 0;

    while (i < n) {
        if (cc_inert_scan_step(&scan, src, n, &i)) continue;
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;

        /* Detect T!>(E) pattern. Statement unwrap (`expr !>(e) {`, `!>;`)
         * is rewritten later — do not treat the binder as an error type. */
        if (c == '!' && c2 == '>') {
            if (cc__header_bang_unwrap_is_stmt(src, n, i)) {
                i += 2;
                continue;
            }
            size_t sigil_pos = i;
            size_t j = i + 2;  /* skip '!>' */
            
            /* Skip whitespace */
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) j++;
            
            /* Must find '(' */
            if (j < n && src[j] == '(') {
                size_t lp = j;
                size_t rp = 0;
                if (cc_find_matching_paren(src, n, lp, &rp)) {
                    size_t err_start = lp + 1;
                    while (err_start < rp && (src[err_start] == ' ' || src[err_start] == '\t' ||
                                              src[err_start] == '\n' || src[err_start] == '\r')) err_start++;
                    size_t err_end = rp;
                    j = rp;
                    
                    /* Trim trailing whitespace from error type */
                    while (err_end > err_start && (src[err_end - 1] == ' ' || src[err_end - 1] == '\t' ||
                                                    src[err_end - 1] == '\n' || src[err_end - 1] == '\r')) {
                        err_end--;
                    }
                    
                    j++;  /* skip ')' */
                    
                    /* Scan back from '!>' to find the ok type */
                    size_t ty_end = sigil_pos;
                    while (ty_end > 0 && (src[ty_end - 1] == ' ' || src[ty_end - 1] == '\t')) ty_end--;
                    
                    size_t ty_start = cc__scan_back_to_type_start(src, ty_end);
                    
                    if (ty_start < ty_end && err_start < err_end) {
                        ty_start = cc__skip_leading_type_prefix_keywords(src, ty_start, ty_end);
                        size_t ty_len = ty_end - ty_start;
                        size_t err_len = err_end - err_start;
                        
                        char mangled_ok[256];
                        char mangled_err[256];
                        cc__mangle_type(src + ty_start, ty_len, mangled_ok, sizeof(mangled_ok));
                        cc__mangle_type(src + err_start, err_len, mangled_err, sizeof(mangled_err));
                        
                        if (mangled_ok[0] && mangled_err[0]) {
                            /* Collect this result type pair */
                            if (state) {
                                cc_lower_state_add_result(state,
                                    src + ty_start, ty_len,
                                    src + err_start, err_len,
                                    mangled_ok, mangled_err);
                            }
                            
                            /* Emit: everything up to type start, then CCResult_T_E */
                            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "CCResult_");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, mangled_ok);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "_");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, mangled_err);
                            last_emit = j;
                            i = j;
                            continue;
                        }
                    }
                }
            }
        }
        
        i++;
    }
    
    if (last_emit == 0) return NULL;  /* No rewrites needed */
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Blank CC_GENERIC_FACTORY / CC_GENERIC_FACTORY_EXTEND blocks in place (replace
 * every non-newline byte with a space, preserving line count) so the lowered
 * .h is clean host-C.  The factory bodies are harvested separately into the
 * including TU's comptime scope (see build_parse_input), where they register
 * and run; the .h only needs the *generated* monomorphs, which the use-site
 * splice supplies.  Newline preservation keeps the .h line-identical to the
 * .cch so cpp_expand's lowered->source #line remap stays exact. */
static char* cc__strip_generic_factory_blocks_header(const char* src, size_t n) {
    static const char kw[] = "CC_GENERIC_FACTORY";
    static const char kw_ext[] = "CC_GENERIC_FACTORY_EXTEND";
    const size_t kwlen = sizeof(kw) - 1;
    const size_t kwlen_ext = sizeof(kw_ext) - 1;
    char* out = NULL;
    size_t i = 0;
    CCInertScan scan;
    int found = 0;
    if (!src || n == 0) return NULL;
    out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n);
    out[n] = '\0';
    cc_inert_scan_init(&scan, NULL);
    scan.at_line_start = 0; /* '#' lines stay code-visible, as before */
    while (i < n) {
        if (cc_inert_scan_step(&scan, src, n, &i)) continue;
        char c = src[i];
        size_t mlen = 0;
        if (c == 'C') {
            if (i + kwlen_ext <= n && memcmp(src + i, kw_ext, kwlen_ext) == 0 &&
                (i == 0 || !(isalnum((unsigned char)src[i - 1]) || src[i - 1] == '_')) &&
                (i + kwlen_ext >= n || !(isalnum((unsigned char)src[i + kwlen_ext]) || src[i + kwlen_ext] == '_')))
                mlen = kwlen_ext;
            else if (i + kwlen <= n && memcmp(src + i, kw, kwlen) == 0 &&
                     (i == 0 || !(isalnum((unsigned char)src[i - 1]) || src[i - 1] == '_')) &&
                     (i + kwlen >= n || !(isalnum((unsigned char)src[i + kwlen]) || src[i + kwlen] == '_')))
                mlen = kwlen;
        }
        if (mlen) {
            size_t p = i + mlen;
            while (p < n && isspace((unsigned char)src[p])) p++;
            if (p >= n || src[p] != '(') { i++; continue; }
            /* skip the (Name[, K]) parameter list */
            {
                size_t prp = 0;
                if (!cc_find_matching_paren(src, n, p, &prp)) { i++; continue; }
                p = prp + 1;
            }
            while (p < n && isspace((unsigned char)src[p])) p++;
            if (p >= n || src[p] != '{') { i++; continue; }
            size_t body_l = p, body_r = 0;
            if (!cc_find_matching_brace(src, n, body_l, &body_r) || body_r <= body_l) {
                free(out);
                return NULL;
            }
            for (size_t k = i; k <= body_r; ++k)
                if (out[k] != '\n') out[k] = ' ';
            found = 1;
            i = body_r + 1;
            continue;
        }
        i++;
    }
    if (!found) { free(out); return NULL; }
    return out;
}

static char* cc__strip_comptime_blocks_header(const char* src, size_t n) {
    char* out = NULL;
    size_t i = 0;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0, in_tpl = 0;
    if (!src || n == 0) return NULL;
    out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n);
    out[n] = '\0';
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && c2) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && c2) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        /* A backtick template is one lexical region: its prose is not code, so
         * an apostrophe in it must not open a char literal that never closes
         * and swallows the rest of the header. */
        if (in_tpl) { if (c == '\\' && c2) { i += 2; continue; } if (c == '`') in_tpl = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '`') { in_tpl = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        if (c == '@' && i + strlen("@comptime") < n && strncmp(src + i, "@comptime", strlen("@comptime")) == 0) {
            size_t p = i + strlen("@comptime");
            size_t body_l = 0, body_r = 0;
            int depth = 0, ls = 0, lc = 0, st = 0, ch = 0, tp = 0;
            while (p < n && isspace((unsigned char)src[p])) p++;
            if (p >= n) { i++; continue; }
            if (src[p] != '{') {
                size_t lparen = 0, rparen = 0;
                if (cc_match_ident_kw(src, n, p, "if") ||
                    cc_match_ident_kw(src, n, p, "for")) {
                    i++;
                    continue;
                }
                for (size_t q = p; q < n; q++) {
                    if (src[q] == ';' || src[q] == '{') break;
                    if (src[q] == '(') { lparen = q; break; }
                }
                if (!lparen || !cc_find_matching_paren(src, n, lparen, &rparen)) {
                    i++;
                    continue;
                }
                p = rparen + 1;
                while (p < n && isspace((unsigned char)src[p])) p++;
                if (p >= n || src[p] != '{') { i++; continue; }
            }
            body_l = p;
            for (p = body_l; p < n; ++p) {
                char d = src[p];
                char d2 = (p + 1 < n) ? src[p + 1] : 0;
                if (lc) { if (d == '\n') lc = 0; continue; }
                if (ls) { if (d == '*' && d2 == '/') { ls = 0; p++; } continue; }
                if (st) { if (d == '\\' && d2) { p++; continue; } if (d == '"') st = 0; continue; }
                if (ch) { if (d == '\\' && d2) { p++; continue; } if (d == '\'') ch = 0; continue; }
                /* Templates hold the factory's emitted C, braces and all — the
                 * body's brace depth must not count them. */
                if (tp) { if (d == '\\' && d2) { p++; continue; } if (d == '`') tp = 0; continue; }
                if (d == '/' && d2 == '/') { lc = 1; p++; continue; }
                if (d == '/' && d2 == '*') { ls = 1; p++; continue; }
                if (d == '"') { st = 1; continue; }
                if (d == '`') { tp = 1; continue; }
                if (d == '\'') { ch = 1; continue; }
                if (d == '{') { depth++; continue; }
                if (d == '}') {
                    if (depth == 0) break;
                    depth--;
                    if (depth == 0) { body_r = p; break; }
                }
            }
            if (body_r <= body_l) {
                /* Returning NULL leaves EVERY `@comptime` block in the lowered
                 * `.h`, so one unbalanced block far away surfaces as
                 * `declaration expected` in generated C with nothing naming
                 * this position.  Say where it is. */
                size_t line = 1;
                for (size_t k = 0; k < i; k++) if (src[k] == '\n') line++;
                fprintf(stderr,
                        "cc: error: unterminated '@comptime' block at line %zu "
                        "of this header; no @comptime block in it can be "
                        "stripped\n", line);
                free(out);
                return NULL;
            }
            for (size_t k = i; k <= body_r; ++k) {
                if (out[k] != '\n') out[k] = ' ';
            }
            i = body_r + 1;
            continue;
        }
        i++;
    }
    return out;
}

static char* cc__rewrite_header_includes(const char* src, size_t n) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t last_emit = 0;
    size_t i = 0;
    if (!src || n == 0) return NULL;

    while (i < n) {
        if (i + 5 <= n && strncmp(src + i, ".cch\"", 5) == 0) {
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
            cc_sb_append_cstr(&out, &out_len, &out_cap, ".h\"");
            i += 5;
            last_emit = i;
            continue;
        }
        if (i + 5 <= n && strncmp(src + i, ".cch>", 5) == 0) {
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
            cc_sb_append_cstr(&out, &out_len, &out_cap, ".h>");
            i += 5;
            last_emit = i;
            continue;
        }
        i++;
    }

    if (last_emit == 0) return NULL;
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

char* cc_lower_header_string(const char* input, size_t input_len, const char* input_path) {
    
    if (!input || input_len == 0) return NULL;
    
    CCLowerState state;
    cc_lower_state_init(&state);
    
    /* Current processing buffer */
    const char* cur = input;
    size_t cur_len = input_len;
    {
        size_t skip = cc_unit_header_skip(input, input_len);
        if (skip > 0 && skip <= input_len) {
            cur = input + skip;
            cur_len = input_len - skip;
        }
    }
    char* buf_ded = NULL;
    char* buf0 = NULL;
    char* buf_fac = NULL;
    char* buf_inc = NULL;
    char* buf_as = NULL;
    char* buf_types = NULL;
    char* buf_va = NULL;
    char* buf_vec = NULL;
    char* buf_result_ctors = NULL;
    char* buf2 = NULL;
    char* buf_result_fields = NULL;
    char* buf_unwrap = NULL;
    char* buf_errsyn = NULL;
    
    /* Pass -1: closer-anchored template dedent, so a header's @emit
       templates carry the same bytes here as in the comptime and main
       pipelines (all three dedent before reading a template body). */
    {
        size_t dlen = 0;
        buf_ded = cc_tpl_dedent_text(cur, cur_len, input_path, &dlen);
        if (buf_ded == (char*)-1) return NULL; /* diagnosed */
        if (buf_ded) { cur = buf_ded; cur_len = dlen; }
    }

    /* Pass 0: Strip raw @comptime blocks and function definitions so lowered
       headers are valid C. Functions and `@comptime { }` blocks are harvested
       into including CC TUs (see cc_harvest_header_comptime_functions /
       cc_harvest_local_header_comptime_blocks). */
    buf0 = cc__strip_comptime_blocks_header(cur, cur_len);
    if (buf0) {
        cur = buf0;
        cur_len = strlen(buf0);
    }

    /* Pass 0a: Blank CC_GENERIC_FACTORY / _EXTEND blocks.  They are harvested
       into the including TU's comptime scope (build_parse_input); the lowered
       .h must stay clean host-C, the use-site splice provides the monomorphs. */
    buf_fac = cc__strip_generic_factory_blocks_header(cur, cur_len);
    if (buf_fac) {
        cur = buf_fac;
        cur_len = strlen(buf_fac);
    }

    /* Pass 0b: Rewrite .cch includes to .h so lowered headers are self-contained C headers. */
    buf_inc = cc__rewrite_header_includes(cur, cur_len);
    if (buf_inc) {
        cur = buf_inc;
        cur_len = strlen(buf_inc);
    }

    /* Pass 0b2: erase `@as` so lowered .h is plain host C (attribute lives
     * in the .cch / Concurrent-C AST only). */
    buf_as = cc_rewrite_as_attr_to_comment(cur, cur_len);
    if (buf_as) {
        cur = buf_as;
        cur_len = strlen(buf_as);
    }

    /* Pass 0b3: erase `@typeview` blocks (and rewrite
     * `@typeview(Mode) Base` sugar) — same Concurrent-C-only facts. */
    {
        char* buf_tv = cc_strip_typeview_blocks(cur, cur_len);
        if (buf_tv) {
            if (buf_as && cur == buf_as) free(buf_as);
            buf_as = buf_tv; /* reuse free slot at end */
            cur = buf_tv;
            cur_len = strlen(buf_tv);
        }
    }

    /* Pass 0b4: erase `@typehooks on T { … };` (→ register at Concurrent-C
     * scan time; host `.h` must stay plain C). */
    {
        char* buf_th = cc_strip_typehooks_blocks(cur, cur_len);
        if (buf_th) {
            if (buf_as && cur == buf_as) free(buf_as);
            buf_as = buf_th;
            cur = buf_th;
            cur_len = strlen(buf_th);
        }
    }

    /* Pass 0b5: erase `@destroy` / `@detach` in `#define` bodies (and any
     * other leftover) so the host `.h` stays plain C. Concurrent-C rewrites
     * `cc_arena_stack` use sites to a real `@destroy` decl. */
    {
        char* buf_life = cc_strip_at_destroy_detach(cur, cur_len);
        if (buf_life) {
            if (buf_as && cur == buf_as) free(buf_as);
            buf_as = buf_life;
            cur = buf_life;
            cur_len = strlen(buf_life);
        }
    }

    /* Pass 0c: Share header-safe type-syntax lowering with preprocess.c so
       constructs like `char[:]` and generic container types lower the same
       way they do in the main compiler pipeline. */
    buf_types = cc_rewrite_header_type_syntax_shared(cur, cur_len, input_path);
    if (buf_types) {
        cur = buf_types;
        cur_len = strlen(buf_types);
    }

    /* Pass 0c2: `@variant Name { … };` → enum+union C so a guest `.h`
     * does not leave the sigil (impl-grade splice is the other path). */
    buf_va = cc_rewrite_variant_decls_text(cur, cur_len, input_path);
    if (buf_va == (char*)-1) {
        free(buf_ded);
        free(buf0);
        free(buf_fac);
        free(buf_inc);
        free(buf_as);
        free(buf_types);
        cc_result_spec_table_free(&state.result_specs);
        return NULL;
    }
    if (buf_va) {
        cur = buf_va;
        cur_len = strlen(buf_va);
    }

    buf_vec = cc__splice_header_vec_decls(cur, cur_len);
    if (buf_vec) {
        cur = buf_vec;
        cur_len = strlen(buf_vec);
    }

    /* Pass 1: Rewrite T!>(E) -> CCResult_T_E */
    buf2 = cc__lower_result_types(cur, cur_len, &state);
    if (buf2) {
        cur = buf2;
        cur_len = strlen(buf2);
    }

    /* Pass 1a: Once result types are concrete, infer bare constructors using
       the same shared source-lowering pass used for normal source files. */
    buf_result_ctors = cc__rewrite_inferred_result_constructors(cur, cur_len);
    if (buf_result_ctors) {
        cur = buf_result_ctors;
        cur_len = strlen(buf_result_ctors);
    }

    /* Pass 1b: Lower result field sugar using the same shared rewrite as
       source files so `res.value` / `res.error` survive in header bodies. */
    buf_result_fields = cc__rewrite_result_field_sugar_text(NULL, cur, cur_len);
    if (buf_result_fields) {
        cur = buf_result_fields;
        cur_len = strlen(buf_result_fields);
    }

    /* Pass 1c: `!>` / `?>` in a header is a use. Rewrite it here (typed
     * `CCResult_*_is_err` when the callee is known, otherwise `cc_is_err`
     * field access — never TU-level `__cc_uw_*`) and collect every Result
     * pair so the spec is spliced before the first mention. */
    {
        CCVisitorCtx vctx;
        memset(&vctx, 0, sizeof(vctx));
        vctx.input_path = input_path;
        cc_result_fn_registry_scan_source(cur, cur_len);
        cc__header_collect_unwrap_fn_pairs(cur, cur_len, &state);
        if (cc__header_has_op(cur, cur_len, "!>") ||
            cc__header_has_op(cur, cur_len, "?>")) {
            char* ru_out = NULL;
            size_t ru_len = 0;
            int ru = cc__rewrite_result_unwrap_header(&vctx, cur, cur_len,
                                                      &ru_out, &ru_len);
            if (ru < 0) {
                free(ru_out);
                free(buf_ded);
                free(buf0);
                free(buf_fac);
                free(buf_inc);
                free(buf_as);
                free(buf_types);
                free(buf_va);
                free(buf_vec);
                free(buf_result_ctors);
                free(buf2);
                free(buf_result_fields);
                cc_result_spec_table_free(&state.result_specs);
                return NULL;
            }
            if (ru > 0 && ru_out) {
                buf_unwrap = ru_out;
                cur = ru_out;
                cur_len = ru_len;
            } else {
                free(ru_out);
            }
        }
        if (cc_contains_token_top_level(cur, cur_len, "@errhandler") ||
            cc_contains_token_top_level(cur, cur_len, "@err")) {
            char* es_out = NULL;
            size_t es_len = 0;
            int es = cc__rewrite_err_syntax(&vctx, cur, cur_len, &es_out,
                                            &es_len);
            if (es < 0) {
                free(es_out);
                free(buf_ded);
                free(buf0);
                free(buf_fac);
                free(buf_inc);
                free(buf_as);
                free(buf_types);
                free(buf_va);
                free(buf_vec);
                free(buf_result_ctors);
                free(buf2);
                free(buf_result_fields);
                free(buf_unwrap);
                cc_result_spec_table_free(&state.result_specs);
                return NULL;
            }
            if (es > 0 && es_out) {
                buf_errsyn = es_out;
                cur = es_out;
                cur_len = es_len;
            } else {
                free(es_out);
            }
        }
        cc__header_collect_ccresult_idents(cur, cur_len, &state);
    }

    /* (retired) Pass 2 used to rewrite T? -> CCOptional_T. */

    /* Build final output */
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    /* Each collected Result box is declared just before its own first use.
     *
     * Declaring them all at the end (before the closing include guard) is only
     * enough when the box already exists — which is why a header returning
     * `int !>(E)` worked and `long long !>(E)` did not: the prelude predeclares
     * the single-word instances, so the late declaration was dead code and
     * nobody noticed it was late.  A newly minted box has to precede the
     * function whose return type names it.
     *
     * Per-use placement rather than one block at the top, because an ok type
     * may be declared inside this header: the first use necessarily follows
     * that declaration, so anchoring to the use is correct in both directions. */
    if (state.result_specs.count > 0) {
        typedef struct { size_t pos; char decl[512]; } CCDeclAt;
        CCDeclAt* items = (CCDeclAt*)calloc(state.result_specs.count, sizeof(CCDeclAt));
        size_t nitems = 0;
        if (items) {
            for (size_t i = 0; i < state.result_specs.count; i++) {
                const CCResultSpec* sp = cc_result_spec_table_get(&state.result_specs, i);
                const char* hit;
                if (!sp) continue;
                hit = cc__find_ident_top_level(cur, cur_len, sp->concrete_name);
                if (!hit) continue;
                items[nitems].pos = cc__decl_insert_point(cur, (size_t)(hit - cur));
                cc_result_spec_emit_decl(sp, items[nitems].decl, sizeof(items[nitems].decl));
                nitems++;
            }
            /* Ascending by insertion point so one forward pass can splice. */
            for (size_t a = 1; a < nitems; a++) {
                CCDeclAt tmp = items[a];
                size_t b = a;
                while (b > 0 && items[b - 1].pos > tmp.pos) { items[b] = items[b - 1]; b--; }
                items[b] = tmp;
            }
        }
        if (nitems > 0) {
            size_t last = 0;
            for (size_t a = 0; a < nitems; a++) {
                cc_sb_append(&out, &out_len, &out_cap, cur + last, items[a].pos - last);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "/* --- CC auto-generated type declaration --- */\n");
                cc_sb_append_cstr(&out, &out_len, &out_cap, items[a].decl);
                last = items[a].pos;
            }
            cc_sb_append(&out, &out_len, &out_cap, cur + last, cur_len - last);
        } else if (cur != input) {
            out = strdup(cur);
            out_len = cur_len;
        }
        free(items);
    } else if (cur != input) {
        /* No declarations to add, but we did rewrites */
        out = strdup(cur);
        out_len = cur_len;
    }
    
    /* Cleanup */
    free(buf_ded);
    free(buf0);
    free(buf_fac);
    free(buf_inc);
    free(buf_as);
    free(buf_types);
    free(buf_va);
    free(buf_vec);
    free(buf_result_ctors);
    free(buf2);
    free(buf_result_fields);
    free(buf_unwrap);
    free(buf_errsyn);
    cc_result_spec_table_free(&state.result_specs);
    
    return out;
}

int cc_lower_header(const char* cch_path, const char* h_path) {
    if (!cch_path || !h_path) return EINVAL;
    
    /* Read input file */
    FILE* in = fopen(cch_path, "r");
    if (!in) return errno ? errno : -1;
    
    fseek(in, 0, SEEK_END);
    long len = ftell(in);
    fseek(in, 0, SEEK_SET);
    
    if (len <= 0) {
        fclose(in);
        return EINVAL;
    }
    
    char* input = (char*)malloc(len + 1);
    if (!input) {
        fclose(in);
        return ENOMEM;
    }
    
    size_t read_len = fread(input, 1, len, in);
    fclose(in);
    input[read_len] = '\0';
    
    /* Lower the content */
    char* output = cc_lower_header_string(input, read_len, cch_path);
    
    /* If no changes, copy input directly */
    const char* to_write = output ? output : input;
    size_t to_write_len = output ? strlen(output) : read_len;
    
    int err = cc_write_file_if_changed(h_path, to_write, to_write_len);
    free(input);
    free(output);
    return err;
}
