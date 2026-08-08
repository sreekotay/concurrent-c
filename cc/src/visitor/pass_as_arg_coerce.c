#include "pass_as_arg_coerce.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "preprocess/type_registry.h"
#include "util/path.h"
#include "util/text.h"
#include "visitor/pass_callee_occ.h"
#include "visitor/pass_common.h"

typedef CCNodeView NodeView;

static int cc__as_split_args(const char* txt, size_t args_s, size_t args_e,
                             size_t* starts, size_t* ends, int cap) {
    int argc = 0;
    size_t cur = args_s;
    size_t k = args_s;
    if (args_s >= args_e) return 0;
    while (k < args_e) {
        k = cc_find_char_top_level(txt, k, args_e, ',');
        if (k >= args_e) break;
        if (argc < cap) { starts[argc] = cur; ends[argc] = k; argc++; }
        cur = ++k;
    }
    if (argc < cap) { starts[argc] = cur; ends[argc] = args_e; argc++; }
    return argc;
}

static void cc__as_strip_param_name(char* ty) {
    size_t n, i, j;
    if (!ty) return;
    n = strlen(ty);
    while (n > 0 && (ty[n - 1] == ' ' || ty[n - 1] == '\t')) ty[--n] = '\0';
    while (n > 0 && ty[n - 1] == ']') {
        i = n;
        while (i > 0 && ty[i - 1] != '[') i--;
        if (i == 0) break;
        n = i - 1;
        ty[n] = '\0';
        while (n > 0 && (ty[n - 1] == ' ' || ty[n - 1] == '\t')) ty[--n] = '\0';
    }
    if (n == 0) return;
    i = n;
    while (i > 0 && (isalnum((unsigned char)ty[i - 1]) || ty[i - 1] == '_')) i--;
    if (i == 0 || i == n) return;
    j = i;
    while (j > 0 && (ty[j - 1] == ' ' || ty[j - 1] == '\t')) j--;
    if (j == 0) return;
    {
        char prev = ty[j - 1];
        if (!(isalnum((unsigned char)prev) || prev == '_' || prev == '*' ||
              prev == ']' || prev == '>'))
            return;
    }
    ty[j] = '\0';
}

/* Parse a named param base with 0 or 1 pointer levels (`T` / `T *`).
 * Returns 1 and sets *out_stars; 0 for anything else. */
static int cc__as_param_named_base(const char* spelling, char* out_base, size_t out_sz,
                                   int* out_stars) {
    char tmp[256];
    size_t n;
    char* p;
    size_t stars = 0;
    if (out_stars) *out_stars = 0;
    if (!spelling || !out_base || out_sz == 0) return 0;
    out_base[0] = '\0';
    n = strlen(spelling);
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, spelling, n);
    tmp[n] = '\0';
    cc__as_strip_param_name(tmp);
    p = tmp;
    while (*p == ' ' || *p == '\t') p++;
    for (;;) {
        if (strncmp(p, "const ", 6) == 0) { p += 6; continue; }
        if (strncmp(p, "volatile ", 9) == 0) { p += 9; continue; }
        if (strncmp(p, "restrict ", 9) == 0) { p += 9; continue; }
        if (strncmp(p, "struct ", 7) == 0) { p += 7; continue; }
        if (strncmp(p, "union ", 6) == 0) { p += 6; continue; }
        break;
    }
    if (!(isalpha((unsigned char)*p) || *p == '_')) return 0;
    {
        size_t bn = 0;
        while (p[bn] && (isalnum((unsigned char)p[bn]) || p[bn] == '_')) bn++;
        if (bn == 0 || bn + 1 > out_sz) return 0;
        memcpy(out_base, p, bn);
        out_base[bn] = '\0';
        p += bn;
    }
    while (*p == ' ' || *p == '\t') p++;
    while (*p == '*') { stars++; p++; while (*p == ' ' || *p == '\t') p++; }
    if (stars > 1 || *p != '\0' || !out_base[0]) return 0;
    if (out_stars) *out_stars = (int)stars;
    return 1;
}

/* True when spelling is a single-level pointer to a named type (`T *`). */
static int cc__as_param_is_named_ptr(const char* spelling, char* out_base, size_t out_sz) {
    int stars = 0;
    return cc__as_param_named_base(spelling, out_base, out_sz, &stars) && stars == 1;
}

/* The slice marker aliases are ABI-identical typedefs of CCSlice, and the
 * parser's typedef-name lookup may report any of them for a CCSlice param.
 * @as field types spell the base, so match against that. */
static void cc__as_normalize_slice_marker(char* base) {
    if (!base) return;
    if (strcmp(base, "CCSliceShared") == 0 || strcmp(base, "CCSliceUnique") == 0)
        strcpy(base, "CCSlice");
}

static int cc__as_lookup_param_bases(const CCASTRoot* root,
                                     const NodeView* n,
                                     const char* callee,
                                     char bases[][128],
                                     int* param_stars,
                                     int cap,
                                     int* out_paramc) {
    int paramc = 0;
    int k;
    if (!root || !n || !callee || !bases || !param_stars || !out_paramc || cap <= 0)
        return 0;
    *out_paramc = 0;
    for (k = 0; k < root->node_count; k++) {
        int p;
        if (n[k].kind != CC_AST_NODE_FUNC) continue;
        if (!n[k].aux_s1 || strcmp(n[k].aux_s1, callee) != 0) continue;
        for (p = 0; p < root->node_count && paramc < cap; p++) {
            if (n[p].parent != k || n[p].kind != CC_AST_NODE_PARAM) continue;
            param_stars[paramc] = -1;
            if (!cc__as_param_named_base(n[p].aux_s2, bases[paramc], 128,
                                         &param_stars[paramc])) {
                bases[paramc][0] = '\0';
                param_stars[paramc] = -1;
            }
            cc__as_normalize_slice_marker(bases[paramc]);
            paramc++;
        }
        if (paramc > 0) {
            *out_paramc = paramc;
            return 1;
        }
        break;
    }
    for (k = 0; k < root->node_count; k++) {
        const char* sig;
        const char* ps;
        const char* pe;
        size_t starts[16], ends[16];
        int argc;
        int ai;
        if (n[k].kind != CC_AST_NODE_DECL_ITEM) continue;
        if (!n[k].aux_s1 || !n[k].aux_s2) continue;
        if (strcmp(n[k].aux_s1, callee) != 0) continue;
        sig = n[k].aux_s2;
        if (!strchr(sig, '(')) continue;
        ps = strchr(sig, '(');
        pe = strrchr(sig, ')');
        if (!ps || !pe || pe <= ps) continue;
        ps++;
        while (ps < pe && (*ps == ' ' || *ps == '\t')) ps++;
        while (pe > ps && (pe[-1] == ' ' || pe[-1] == '\t')) pe--;
        if (pe == ps || (pe - ps == 4 && strncmp(ps, "void", 4) == 0)) {
            *out_paramc = 0;
            return 1;
        }
        argc = cc__as_split_args(sig, (size_t)(ps - sig), (size_t)(pe - sig),
                                 starts, ends, 16);
        for (ai = 0; ai < argc && ai < cap; ai++) {
            char tmp[256];
            size_t al = ends[ai] > starts[ai] ? ends[ai] - starts[ai] : 0;
            if (al >= sizeof(tmp)) al = sizeof(tmp) - 1;
            memcpy(tmp, sig + starts[ai], al);
            tmp[al] = '\0';
            param_stars[ai] = -1;
            if (!cc__as_param_named_base(tmp, bases[ai], 128, &param_stars[ai])) {
                bases[ai][0] = '\0';
                param_stars[ai] = -1;
            }
            cc__as_normalize_slice_marker(bases[ai]);
        }
        *out_paramc = argc < cap ? argc : cap;
        return 1;
    }
    return 0;
}

static int cc__as_name_is_declarator(const char* s, size_t len, size_t name_s,
                                     size_t callee_n) {
    size_t after, lparen, rparen, k, i, ns, nl;
    if (!s || name_s >= len) return 0;
    after = cc_skip_ws_and_comments(s, len, name_s + callee_n);
    if (after >= len || s[after] != '(') return 0;
    lparen = after;
    if (!cc_find_matching_paren(s, len, lparen, &rparen)) return 0;
    k = cc_skip_ws_and_comments(s, len, rparen + 1);
    if (k < len && s[k] == '{') return 1;
    i = name_s;
    while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t' || s[i - 1] == '\n' ||
                     s[i - 1] == '\r'))
        i--;
    if (i == 0) return 0;
    if (s[i - 1] == '*') return 1;
    if (!cc_is_ident_char(s[i - 1])) return 0;
    nl = 0;
    while (i > 0 && cc_is_ident_char(s[i - 1])) { i--; nl++; }
    ns = i;
    if (nl == 6 && memcmp(s + ns, "return", 6) == 0) return 0;
    if (nl == 6 && memcmp(s + ns, "sizeof", 6) == 0) return 0;
    return 1;
}

/* Parse arg as optional `&` + identifier (no member/call). Sets *out_has_amp. */
static int cc__as_arg_is_simple_addr(const char* s, size_t a, size_t b,
                                     char* out_ident, size_t ident_sz,
                                     int* out_has_amp) {
    size_t i = cc_skip_ws_and_comments(s, b, a);
    size_t e = b;
    size_t n = 0;
    if (out_has_amp) *out_has_amp = 0;
    if (!out_ident || ident_sz == 0) return 0;
    out_ident[0] = '\0';
    while (e > i && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' ||
                     s[e - 1] == '\r'))
        e--;
    if (i >= e) return 0;
    if (s[i] == '&') {
        if (out_has_amp) *out_has_amp = 1;
        i = cc_skip_ws_and_comments(s, e, i + 1);
    }
    if (i >= e || !(isalpha((unsigned char)s[i]) || s[i] == '_')) return 0;
    while (i < e && (isalnum((unsigned char)s[i]) || s[i] == '_')) {
        if (n + 1 < ident_sz) out_ident[n++] = s[i];
        i++;
    }
    out_ident[n] = '\0';
    i = cc_skip_ws_and_comments(s, e, i);
    return n > 0 && i == e;
}

/* Detect `(T*)&x` / `(T*)x` — explicit cast; do not rewrite. */
static int cc__as_arg_is_explicit_cast(const char* s, size_t a, size_t b,
                                       char* out_cast_base, size_t base_sz,
                                       char* out_ident, size_t ident_sz,
                                       int* out_has_amp) {
    size_t i = cc_skip_ws_and_comments(s, b, a);
    size_t e = b;
    size_t rp;
    char tmp[256];
    if (out_has_amp) *out_has_amp = 0;
    if (out_cast_base) out_cast_base[0] = '\0';
    if (out_ident) out_ident[0] = '\0';
    while (e > i && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' ||
                     s[e - 1] == '\r'))
        e--;
    if (i >= e || s[i] != '(') return 0;
    if (!cc_find_matching_paren(s, e, i, &rp)) return 0;
    /* Capture inside parens as cast type spelling. */
    {
        size_t cs = i + 1;
        size_t ce = rp;
        while (cs < ce && (s[cs] == ' ' || s[cs] == '\t')) cs++;
        while (ce > cs && (s[ce - 1] == ' ' || s[ce - 1] == '\t')) ce--;
        if (ce <= cs || ce - cs >= sizeof(tmp)) return 0;
        memcpy(tmp, s + cs, ce - cs);
        tmp[ce - cs] = '\0';
    }
    if (!cc__as_param_is_named_ptr(tmp, out_cast_base ? out_cast_base : tmp, base_sz ? base_sz : sizeof(tmp)))
        return 0;
    i = cc_skip_ws_and_comments(s, e, rp + 1);
    return cc__as_arg_is_simple_addr(s, i, e, out_ident, ident_sz, out_has_amp);
}

static int cc__as_arg_already_member(const char* s, size_t a, size_t b,
                                     const char* field) {
    size_t i = cc_skip_ws_and_comments(s, b, a);
    size_t e = b;
    size_t fl;
    if (!field || !field[0]) return 0;
    fl = strlen(field);
    while (e > i && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' ||
                     s[e - 1] == '\r'))
        e--;
    if (i < e && s[i] == '&') i = cc_skip_ws_and_comments(s, e, i + 1);
    /* Skip optional outer parens around recv.field */
    while (i < e && s[i] == '(') {
        size_t rp = 0;
        if (!cc_find_matching_paren(s, e, i, &rp) || rp + 1 != e) break;
        i = cc_skip_ws_and_comments(s, rp, i + 1);
        e = rp;
        while (e > i && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
    }
    if (i >= e || !(isalpha((unsigned char)s[i]) || s[i] == '_')) return 0;
    while (i < e && (isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
    i = cc_skip_ws_and_comments(s, e, i);
    if (i + 1 < e && s[i] == '-' && s[i + 1] == '>') i += 2;
    else if (i < e && s[i] == '.') i++;
    else return 0;
    i = cc_skip_ws_and_comments(s, e, i);
    return i + fl == e && memcmp(s + i, field, fl) == 0;
}

static size_t cc__as_find_callee_span(const CCASTRoot* root, const NodeView* n,
                                      int call_idx, const char* in_src, size_t in_len,
                                      const CCVisitorCtx* ctx, const char* callee,
                                      const CCCalleeOccIndex* occ_idx) {
    size_t callee_n = strlen(callee);
    size_t call_s = cc_pass_node_exact_off(root, n[call_idx].off_start, in_len);
    size_t call_e = cc_pass_node_exact_end_off(root, n[call_idx].off_end, in_len);
    size_t name_s = (size_t)-1;
    (void)ctx;
    if (call_s != (size_t)-1 && call_e != (size_t)-1 && call_e > call_s) {
        if (call_s + callee_n <= in_len && memcmp(in_src + call_s, callee, callee_n) == 0 &&
            !cc__as_name_is_declarator(in_src, in_len, call_s, callee_n)) {
            return call_s;
        }
        if (occ_idx) {
            name_s = cc_callee_occ_index_first_in_range(occ_idx, callee, call_s, call_e);
            if (name_s != (size_t)-1) return name_s;
        }
    }
    {
        int want_occ = 1;
        int k;
        for (k = 0; k < call_idx; k++) {
            if (n[k].kind != CC_AST_NODE_CALL) continue;
            if ((n[k].aux2 & 4) != 0) continue;
            if (!n[k].aux_s1 || strcmp(n[k].aux_s1, callee) != 0) continue;
            if (!cc_pass_node_in_tu(root, ctx, n[k].file)) continue;
            if (n[k].line_start < n[call_idx].line_start ||
                (n[k].line_start == n[call_idx].line_start &&
                 n[k].col_start <= n[call_idx].col_start))
                want_occ++;
        }
        if (occ_idx)
            name_s = cc_callee_occ_index_nth(occ_idx, callee, want_occ);
    }
    return name_s;
}

int cc__collect_as_arg_coerce_edits(const CCASTRoot* root,
                                    const CCVisitorCtx* ctx,
                                    CCEditBuffer* eb) {
    const NodeView* n;
    const char* in_src;
    size_t in_len;
    CCTypeRegistry* reg;
    CCCalleeOccIndex* occ_idx = NULL;
    int edits = 0;
    int i;
    int dbg = getenv("CC_DEBUG_AS_ARG_COERCE") != NULL;

    if (!root || !ctx || !eb || !eb->src) return 0;
    if (!root->nodes || root->node_count <= 0) return 0;
    reg = cc_type_registry_get_global();
    if (!reg) return 0;

    in_src = eb->src;
    in_len = eb->src_len;
    n = (const NodeView*)root->nodes;
    occ_idx = cc_callee_occ_index_build(in_src, in_len, cc__as_name_is_declarator);

    for (i = 0; i < root->node_count; i++) {
        const char* callee;
        size_t name_s, after, lparen, rparen;
        size_t starts[16], ends[16];
        char bases[16][128];
        int param_stars[16];
        int argc;
        int paramc = 0;
        int have_params;
        int ai;
        size_t callee_n;

        if (n[i].kind != CC_AST_NODE_CALL) continue;
        if ((n[i].aux2 & 4) != 0) continue; /* still UFCS form */
        callee = n[i].aux_s1;
        if (!callee) continue;
        if (!cc_pass_node_in_tu(root, ctx, n[i].file)) continue;
        callee_n = strlen(callee);
        name_s = cc__as_find_callee_span(root, n, i, in_src, in_len, ctx, callee, occ_idx);
        if (name_s == (size_t)-1) continue;
        after = cc_skip_ws_and_comments(in_src, in_len, name_s + callee_n);
        if (after >= in_len || in_src[after] != '(') continue;
        lparen = after;
        if (!cc_find_matching_paren(in_src, in_len, lparen, &rparen)) continue;
        argc = cc__as_split_args(in_src, lparen + 1, rparen, starts, ends, 16);
        if (argc <= 0) continue;
        memset(param_stars, -1, sizeof(param_stars));
        memset(bases, 0, sizeof(bases));
        have_params = cc__as_lookup_param_bases(root, n, callee, bases, param_stars, 16, &paramc);
        if (dbg)
            fprintf(stderr, "as_arg_coerce: call %s argc=%d have=%d paramc=%d star0=%d base0=%s\n",
                    callee, argc, have_params, paramc,
                    paramc > 0 ? param_stars[0] : -9, paramc > 0 ? bases[0] : "-");
        if (!have_params || paramc <= 0) continue;

        for (ai = 0; ai < argc && ai < paramc; ai++) {
            size_t a = starts[ai];
            size_t e = ends[ai];
            size_t trim_a, trim_e;
            char ident[128];
            char cast_base[128];
            const char* field_name = NULL;
            const char* outer_ty = NULL;
            int has_amp = 0;
            int outer_is_ptr = 0;
            int match;
            char repl[256];
            char rel[1024];
            const char* file;
            int line, col;

            if (param_stars[ai] < 0 || !bases[ai][0]) continue;
            trim_a = cc_skip_ws_and_comments(in_src, e, a);
            trim_e = e;
            while (trim_e > trim_a &&
                   (in_src[trim_e - 1] == ' ' || in_src[trim_e - 1] == '\t' ||
                    in_src[trim_e - 1] == '\n' || in_src[trim_e - 1] == '\r'))
                trim_e--;
            if (trim_e <= trim_a) continue;

            line = n[i].line_start > 0 ? n[i].line_start : 1;
            col = n[i].col_start > 0 ? n[i].col_start : 1;
            file = cc_path_rel_to_repo(ctx->input_path ? ctx->input_path : "<input>",
                                      rel, sizeof(rel));

            /* Explicit cast: never rewrite; warn when @as path is not offset-0. */
            if (param_stars[ai] == 1 &&
                cc__as_arg_is_explicit_cast(in_src, trim_a, trim_e, cast_base, sizeof(cast_base),
                                            ident, sizeof(ident), &has_amp)) {
                char path[256];
                outer_ty = cc_type_registry_resolve_receiver_expr_at(
                    reg, ident, in_src, trim_a, &outer_is_ptr);
                if (!outer_ty && has_amp) {
                    outer_ty = cc_type_registry_lookup_var(reg, ident);
                    outer_is_ptr = 0;
                }
                if (outer_ty && strcmp(cast_base, bases[ai]) == 0) {
                    match = cc_type_registry_as_path_for_type(reg, outer_ty, bases[ai],
                                                             path, sizeof(path));
                    if (match == 0 && path[0]) {
                        const char* first = path;
                        const char* dot = strchr(path, '.');
                        char first_field[128];
                        size_t fl = dot ? (size_t)(dot - path) : strlen(path);
                        if (fl >= sizeof(first_field)) fl = sizeof(first_field) - 1;
                        memcpy(first_field, first, fl);
                        first_field[fl] = '\0';
                        if (dot || !cc_type_registry_field_is_first(reg, outer_ty, first_field)) {
                            cc_pass_warning(file, line, col,
                                            "explicit cast to '%s *' from '%s' skips @as path "
                                            "'%s'; use &%s.%s instead",
                                            bases[ai], outer_ty, path, ident, path);
                        }
                    }
                }
                continue;
            }

            if (!cc__as_arg_is_simple_addr(in_src, trim_a, trim_e, ident, sizeof(ident),
                                           &has_amp))
                continue;

            /* Resolve Outer / Outer*. `&w` → Outer value, amp marks address-of. */
            if (has_amp) {
                outer_ty = cc_type_registry_lookup_var(reg, ident);
                outer_is_ptr = 0;
            } else {
                outer_ty = cc_type_registry_resolve_receiver_expr_at(
                    reg, ident, in_src, trim_a, &outer_is_ptr);
                if (!outer_ty) {
                    outer_ty = cc_type_registry_lookup_var(reg, ident);
                    /* pointer var: type often stored as Outer* */
                    if (outer_ty) {
                        size_t ol = strlen(outer_ty);
                        outer_is_ptr = (ol > 0 && outer_ty[ol - 1] == '*');
                    }
                }
            }
            if (!outer_ty || !outer_ty[0]) continue;

            {
                char path[256];
                match = cc_type_registry_as_path_for_type(reg, outer_ty, bases[ai],
                                                         path, sizeof(path));
                if (match == -3) {
                    cc_pass_error_cat(file, line, col, CC_ERR_TYPE,
                                      "cyclic @as embed graph while coercing '%s' to '%s *'",
                                      outer_ty, bases[ai]);
                    cc_callee_occ_index_free(occ_idx);
                    return -1;
                }
                if (match == -2) {
                    cc_pass_error_cat(file, line, col, CC_ERR_TYPE,
                                      "ambiguous @as arg coerce: multiple '%s' embeds on '%s'",
                                      bases[ai], outer_ty);
                    cc_callee_occ_index_free(occ_idx);
                    return -1;
                }
                if (match != 0 || !path[0]) continue;
                field_name = path; /* dotted path, e.g. mid.file */
                if (cc__as_arg_already_member(in_src, trim_a, trim_e, field_name)) continue;

                if (param_stars[ai] == 0) {
                    /* By-value param: `xs` (Outer value) → `xs.base`.
                     * `&xs` / pointer outers stay as-written — the type
                     * error they produce is the honest one. Typed slice
                     * instances erase SCALED: the byte world must receive
                     * byte-measured len/alen, and the template's bytes()
                     * knows sizeof(T). */
                    const char* ts_prefix = NULL;
                    if (has_amp || outer_is_ptr) continue;
                    if (strcmp(bases[ai], "CCSlice") == 0 &&
                        cc_slice_spec_lookup(outer_ty, &ts_prefix, NULL) == 0 &&
                        ts_prefix) {
                        snprintf(repl, sizeof(repl), "%s_bytes(&%s)", ts_prefix, ident);
                    } else {
                        snprintf(repl, sizeof(repl), "%s.%s", ident, field_name);
                    }
                } else {
                    /* Value Outer must be addressed (`&w`); pointer Outer uses `p`. */
                    if (!has_amp && !outer_is_ptr) {
                        cc_pass_error_cat(file, line, col, CC_ERR_TYPE,
                                          "cannot coerce by-value '%s' to '%s *'; pass &%s "
                                          "(lowers via @as to &%s.%s)",
                                          outer_ty, bases[ai], ident, ident, field_name);
                        cc_callee_occ_index_free(occ_idx);
                        return -1;
                    }

                    if (has_amp || !outer_is_ptr) {
                        /* `&w` → `&w.mid.file` */
                        snprintf(repl, sizeof(repl), "&%s.%s", ident, field_name);
                    } else {
                        /* `p` (Outer*) → `&p->mid.file` */
                        snprintf(repl, sizeof(repl), "&%s->%s", ident, field_name);
                    }
                }
            }

            if (dbg) {
                fprintf(stderr, "as_arg_coerce: %s arg%d '%.*s' -> '%s'\n",
                        callee, ai, (int)(trim_e - trim_a), in_src + trim_a, repl);
            }
            if (cc_edit_buffer_add(eb, trim_a, trim_e, repl, 0, "as_arg_coerce") < 0) {
                cc_callee_occ_index_free(occ_idx);
                return -1;
            }
            edits++;
        }
    }
    cc_callee_occ_index_free(occ_idx);
    return edits;
}
