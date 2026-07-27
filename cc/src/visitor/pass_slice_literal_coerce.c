#include "pass_slice_literal_coerce.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/text.h"
#include "visitor/pass_common.h"

typedef CCNodeView NodeView;

static int cc__slc_arg_is_string_literal(const char* s, size_t a, size_t b) {
    if (!s || a >= b) return 0;
    size_t i = a;
    int segments = 0;
    for (;;) {
        i = cc_skip_ws_and_comments(s, b, i);
        if (i >= b) break;
        if (i + 2 < b && s[i] == 'u' && s[i + 1] == '8' && s[i + 2] == '"') i += 2;
        else if (i + 1 < b && (s[i] == 'L' || s[i] == 'u' || s[i] == 'U') && s[i + 1] == '"') i += 1;
        if (i >= b || s[i] != '"') return 0;
        i++;
        while (i < b && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < b) { i += 2; continue; }
            i++;
        }
        if (i >= b) return 0;
        i++;
        segments++;
    }
    return segments > 0;
}

static int cc__slc_split_args(const char* txt, size_t args_s, size_t args_e,
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

/* True when `ty` is a by-value slice (CCSlice / char[:0]), not a pointer. */
static int cc__slc_type_is_slice_value(const char* ty) {
    char buf[256];
    size_t n = 0;
    size_t i = 0;
    const char* p;
    if (!ty) return 0;
    while (ty[i] && n + 1 < sizeof(buf)) {
        char c = ty[i++];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (n > 0 && buf[n - 1] != ' ') buf[n++] = ' ';
            continue;
        }
        buf[n++] = c;
    }
    while (n > 0 && buf[n - 1] == ' ') n--;
    buf[n] = '\0';
    p = buf;
    while (*p == ' ') p++;
    /* Strip leading cv / storage noise words. */
    for (;;) {
        if (strncmp(p, "const ", 6) == 0) { p += 6; continue; }
        if (strncmp(p, "volatile ", 9) == 0) { p += 9; continue; }
        if (strncmp(p, "restrict ", 9) == 0) { p += 9; continue; }
        break;
    }
    /* CCSlice and semantic markers (CCSliceShared / CCSliceUnique) are
     * ABI-identical by-value slice params. Reject pointer forms.
     * Accept surface `char[:]` / `char[:0]` and `struct CCSlice` spellings
     * from type_to_str. */
    if (strncmp(p, "struct ", 7) == 0) p += 7;
    if (strncmp(p, "CCSliceShared", 13) == 0) {
        p += 13;
    } else if (strncmp(p, "CCSliceUnique", 13) == 0) {
        p += 13;
    } else if (strncmp(p, "CCSlice", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "char[:0]", 8) == 0) {
        p += 8;
    } else if (strncmp(p, "char[:]", 7) == 0) {
        p += 7;
    } else {
        return 0;
    }
    while (*p == ' ') p++;
    return *p == '\0';
}

static int cc__slc_arg_already_wrapped(const char* s, size_t a, size_t b) {
    static const char* wraps[] = {
        "const_char_to_slice",
        "char_to_slice",
        "cc_slice_from_cstr",
        "unsigned_char_to_slice",
        "signed_char_to_slice",
        "const_unsigned_char_to_slice",
        "const_signed_char_to_slice",
    };
    size_t i = cc_skip_ws_and_comments(s, b, a);
    size_t k;
    if (i >= b) return 0;
    for (k = 0; k < sizeof(wraps) / sizeof(wraps[0]); k++) {
        size_t wn = strlen(wraps[k]);
        if (i + wn <= b && memcmp(s + i, wraps[k], wn) == 0) {
            size_t j = i + wn;
            j = cc_skip_ws_and_comments(s, b, j);
            if (j < b && s[j] == '(') return 1;
        }
    }
    return 0;
}

/* Hardcode fallback when FUNC/PARAM nodes are absent from the reparse AST. */
static int cc__slc_known_slice_arg(const char* callee, int arg_index) {
    if (!callee || arg_index < 0) return 0;
    if (strcmp(callee, "cc_path_exists") == 0) return arg_index == 0;
    if (strcmp(callee, "cc_path_is_dir") == 0) return arg_index == 0;
    if (strcmp(callee, "cc_path_is_file") == 0) return arg_index == 0;
    if (strcmp(callee, "cc_file_open") == 0) return arg_index == 1;
    if (strcmp(callee, "cc_file_open_async") == 0) return arg_index == 2;
    if (strcmp(callee, "cc_file_open_async_deadline") == 0) return arg_index == 2;
    if (strcmp(callee, "cc_dir_open") == 0) return arg_index == 1;
    if (strcmp(callee, "cc_glob") == 0) return arg_index == 0;
    if (strcmp(callee, "cc_command") == 0) return arg_index == 1;
    if (strcmp(callee, "cc_command_new") == 0) return arg_index == 1;
    if (strcmp(callee, "cc_path_join") == 0) return arg_index == 1 || arg_index == 2;
    if (strcmp(callee, "cc_script_path_join") == 0) return arg_index == 0 || arg_index == 1;
    if (strcmp(callee, "cc_file_read_path") == 0) return arg_index == 0;
    if (strcmp(callee, "cc_file_write_path") == 0) return arg_index == 0;
    if (strcmp(callee, "cc_sh_run") == 0) return arg_index == 0;
    if (strcmp(callee, "cc_cwd") == 0) return 0;
    return 0;
}

static int cc__slc_skip_callee(const char* callee) {
    if (!callee) return 1;
    if (strcmp(callee, "const_char_to_slice") == 0) return 1;
    if (strcmp(callee, "char_to_slice") == 0) return 1;
    if (strcmp(callee, "cc_slice_from_cstr") == 0) return 1;
    if (strcmp(callee, "unsigned_char_to_slice") == 0) return 1;
    if (strcmp(callee, "signed_char_to_slice") == 0) return 1;
    if (strcmp(callee, "const_unsigned_char_to_slice") == 0) return 1;
    if (strcmp(callee, "const_signed_char_to_slice") == 0) return 1;
    return 0;
}

/* True when `name(` at `name_s` is a function declarator (definition or
 * prototype), not a call. Looks left: a preceding type/storage token means
 * declarator; `return`/`sizeof`/… and expression punctuation mean call.
 * Also: `) {` after the param list is always a definition. */
static int cc__slc_name_is_declarator(const char* s, size_t len, size_t name_s,
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
    if (!cc_is_ident_char(s[i - 1])) return 0; /* `=` `,` `(` `{` `;` `}` → call */
    nl = 0;
    while (i > 0 && cc_is_ident_char(s[i - 1])) {
        i--;
        nl++;
    }
    ns = i;
    /* Statement keywords before a call, not a return type. */
    if (nl == 6 && memcmp(s + ns, "return", 6) == 0) return 0;
    if (nl == 6 && memcmp(s + ns, "sizeof", 6) == 0) return 0;
    if (nl == 5 && memcmp(s + ns, "throw", 5) == 0) return 0;
    if (nl == 4 && memcmp(s + ns, "else", 4) == 0) return 0;
    if (nl == 2 && memcmp(s + ns, "if", 2) == 0) return 0;
    if (nl == 3 && memcmp(s + ns, "for", 3) == 0) return 0;
    if (nl == 5 && memcmp(s + ns, "while", 5) == 0) return 0;
    if (nl == 6 && memcmp(s + ns, "switch", 6) == 0) return 0;
    if (nl == 4 && memcmp(s + ns, "case", 4) == 0) return 0;
    if (nl == 2 && memcmp(s + ns, "do", 2) == 0) return 0;
    return 1; /* e.g. `void takes(` / `static int foo(` / `CCSlice bar(` */
}

/* Fill param_is_slice[0..out_paramc) from FUNC/PARAM or DECL_ITEM; returns 1 if
 * any param info was found. */
/* Strip a trailing C declarator name / array brackets from a param spelling
 * so `CCSlice s` / `char[:0] buf` / `char *argv[]` reduce to the type. */
static void cc__slc_strip_param_name(char* ty) {
    size_t n, i, j;
    if (!ty) return;
    n = strlen(ty);
    while (n > 0 && (ty[n - 1] == ' ' || ty[n - 1] == '\t')) ty[--n] = '\0';
    /* Drop trailing [] dimensions (possibly empty). */
    while (n > 0 && ty[n - 1] == ']') {
        i = n;
        while (i > 0 && ty[i - 1] != '[') i--;
        if (i == 0) break;
        n = i - 1;
        ty[n] = '\0';
        while (n > 0 && (ty[n - 1] == ' ' || ty[n - 1] == '\t')) ty[--n] = '\0';
    }
    /* If the last token is an identifier and something type-like precedes it,
     * drop the identifier (parameter name). Keep bare type idents like
     * `CCSlice`. */
    if (n == 0) return;
    i = n;
    while (i > 0 && (isalnum((unsigned char)ty[i - 1]) || ty[i - 1] == '_')) i--;
    if (i == 0 || i == n) return; /* whole string is one ident, or no ident */
    j = i;
    while (j > 0 && (ty[j - 1] == ' ' || ty[j - 1] == '\t')) j--;
    if (j == 0) return;
    /* Preceding must look like a type end: ident, *, ], or > (templates). */
    {
        char prev = ty[j - 1];
        if (!(isalnum((unsigned char)prev) || prev == '_' || prev == '*' ||
              prev == ']' || prev == '>'))
            return;
    }
    ty[j] = '\0';
}

static int cc__slc_param_spelling_is_slice(const char* spelling) {
    char tmp[256];
    size_t n;
    if (!spelling) return 0;
    n = strlen(spelling);
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, spelling, n);
    tmp[n] = '\0';
    cc__slc_strip_param_name(tmp);
    return cc__slc_type_is_slice_value(tmp);
}

/* Fill param_is_slice[0..out_paramc) from FUNC/PARAM or DECL_ITEM; returns 1 if
 * any param info was found. */
static int cc__slc_lookup_param_slices(const CCASTRoot* root,
                                       const NodeView* n,
                                       const char* callee,
                                       int* param_is_slice,
                                       int cap,
                                       int* out_paramc) {
    int paramc = 0;
    int k;
    int dbg = getenv("CC_DEBUG_SLICE_LIT_COERCE") != NULL;
    if (!root || !n || !callee || !param_is_slice || !out_paramc || cap <= 0) return 0;
    *out_paramc = 0;
    for (k = 0; k < root->node_count; k++) {
        int p;
        if (n[k].kind != CC_AST_NODE_FUNC) continue;
        if (!n[k].aux_s1 || strcmp(n[k].aux_s1, callee) != 0) continue;
        for (p = 0; p < root->node_count && paramc < cap; p++) {
            if (n[p].parent != k || n[p].kind != CC_AST_NODE_PARAM) continue;
            if (dbg) {
                fprintf(stderr, "slice_lit_coerce: FUNC %s param%d ty='%s'\n",
                        callee, paramc, n[p].aux_s2 ? n[p].aux_s2 : "(null)");
            }
            param_is_slice[paramc++] = cc__slc_param_spelling_is_slice(n[p].aux_s2);
        }
        if (paramc > 0) {
            *out_paramc = paramc;
            return 1;
        }
        /* FUNC with no PARAM children — fall through to DECL_ITEM. */
        break;
    }
    /* DECL_ITEM signature text fallback. */
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
        argc = cc__slc_split_args(sig, (size_t)(ps - sig), (size_t)(pe - sig),
                                  starts, ends, 16);
        for (ai = 0; ai < argc && ai < cap; ai++) {
            char tmp[256];
            size_t al = ends[ai] > starts[ai] ? ends[ai] - starts[ai] : 0;
            if (al >= sizeof(tmp)) al = sizeof(tmp) - 1;
            memcpy(tmp, sig + starts[ai], al);
            tmp[al] = '\0';
            if (dbg) {
                fprintf(stderr, "slice_lit_coerce: DECL %s param%d ty='%s'\n",
                        callee, ai, tmp);
            }
            param_is_slice[ai] = cc__slc_param_spelling_is_slice(tmp);
        }
        *out_paramc = argc < cap ? argc : cap;
        return 1;
    }
    return 0;
}

int cc__collect_slice_literal_coerce_edits(const CCASTRoot* root,
                                           const CCVisitorCtx* ctx,
                                           CCEditBuffer* eb) {
    const NodeView* n;
    const char* in_src;
    size_t in_len;
    int edits = 0;
    int i;
    int dbg = getenv("CC_DEBUG_SLICE_LIT_COERCE") != NULL;
    int call_nodes = 0;

    if (!root || !ctx || !eb || !eb->src) return 0;
    if (!root->nodes || root->node_count <= 0) return 0;

    in_src = eb->src;
    in_len = eb->src_len;
    n = (const NodeView*)root->nodes;
    if (dbg) {
        fprintf(stderr, "slice_lit_coerce: nodes=%d tu=%s\n",
                root->node_count, ctx->input_path ? ctx->input_path : "?");
    }

    for (i = 0; i < root->node_count; i++) {
        const char* callee;
        size_t call_s, call_e;
        size_t name_s, after, lparen, rparen;
        size_t starts[16], ends[16];
        int argc;
        int param_is_slice[16];
        int paramc = 0;
        int have_params;
        int ai;
        size_t callee_n;

        if (n[i].kind != CC_AST_NODE_CALL) continue;
        call_nodes++;
        if ((n[i].aux2 & 4) != 0) continue; /* still UFCS form */
        callee = n[i].aux_s1;
        if (!callee || cc__slc_skip_callee(callee)) continue;
        if (!cc_pass_node_in_tu(root, ctx, n[i].file)) continue;
        callee_n = strlen(callee);
        call_s = cc_pass_node_exact_off(root, n[i].off_start, in_len);
        call_e = cc_pass_node_exact_end_off(root, n[i].off_end, in_len);
        name_s = (size_t)-1;
        if (call_s != (size_t)-1 && call_e != (size_t)-1 && call_e > call_s) {
            if (call_s + callee_n <= in_len && memcmp(in_src + call_s, callee, callee_n) == 0 &&
                !cc__slc_name_is_declarator(in_src, in_len, call_s, callee_n)) {
                name_s = call_s;
            } else {
                size_t j;
                for (j = call_s; j + callee_n <= call_e; j++) {
                    if (memcmp(in_src + j, callee, callee_n) != 0) continue;
                    if (j > 0 && (isalnum((unsigned char)in_src[j - 1]) || in_src[j - 1] == '_'))
                        continue;
                    if (j + callee_n < in_len &&
                        (isalnum((unsigned char)in_src[j + callee_n]) || in_src[j + callee_n] == '_'))
                        continue;
                    if (cc__slc_name_is_declarator(in_src, in_len, j, callee_n)) continue;
                    name_s = j;
                    break;
                }
            }
        }
        /* #line makes physical line_start unreliable; scan the whole buffer
         * for this callee occurrence when exact offs miss. Skip declarators
         * so same-TU definitions are not mistaken for their call sites. */
        if (name_s == (size_t)-1) {
            size_t j;
            int occ = 0;
            int want_occ = 1;
            for (int k = 0; k < i; k++) {
                if (n[k].kind != CC_AST_NODE_CALL) continue;
                if ((n[k].aux2 & 4) != 0) continue;
                if (!n[k].aux_s1 || strcmp(n[k].aux_s1, callee) != 0) continue;
                if (!cc_pass_node_in_tu(root, ctx, n[k].file)) continue;
                if (n[k].line_start < n[i].line_start ||
                    (n[k].line_start == n[i].line_start && n[k].col_start <= n[i].col_start))
                    want_occ++;
            }
            for (j = 0; j + callee_n <= in_len; j++) {
                size_t after;
                if (memcmp(in_src + j, callee, callee_n) != 0) continue;
                if (j > 0 && (isalnum((unsigned char)in_src[j - 1]) || in_src[j - 1] == '_'))
                    continue;
                if (j + callee_n < in_len &&
                    (isalnum((unsigned char)in_src[j + callee_n]) || in_src[j + callee_n] == '_'))
                    continue;
                after = cc_skip_ws_and_comments(in_src, in_len, j + callee_n);
                if (after >= in_len || in_src[after] != '(') continue;
                if (cc__slc_name_is_declarator(in_src, in_len, j, callee_n)) continue;
                occ++;
                if (occ == want_occ) {
                    name_s = j;
                    break;
                }
            }
        }
        if (name_s == (size_t)-1) {
            if (dbg) {
                fprintf(stderr, "slice_lit_coerce: no callee span for %s line=%d "
                                "off=%ld..%ld shift=%ld\n",
                        callee, n[i].line_start, (long)n[i].off_start, (long)n[i].off_end,
                        (long)root->parse_src_shift);
            }
            continue;
        }
        if (dbg) {
            fprintf(stderr, "slice_lit_coerce: consider callee=%s line=%d name_s=%zu\n",
                    callee, n[i].line_start, name_s);
        }

        after = name_s + callee_n;
        after = cc_skip_ws_and_comments(in_src, in_len, after);
        if (after >= in_len || in_src[after] != '(') continue;
        lparen = after;
        if (!cc_find_matching_paren(in_src, in_len, lparen, &rparen)) continue;

        argc = cc__slc_split_args(in_src, lparen + 1, rparen, starts, ends, 16);
        if (argc <= 0) continue;

        memset(param_is_slice, 0, sizeof(param_is_slice));
        have_params = cc__slc_lookup_param_slices(root, n, callee, param_is_slice, 16, &paramc);

        for (ai = 0; ai < argc; ai++) {
            size_t a = starts[ai];
            size_t e = ends[ai];
            size_t trim_a, trim_e;
            int want_slice;
            char* repl;
            size_t arg_n;
            int need;

            trim_a = cc_skip_ws_and_comments(in_src, e, a);
            trim_e = e;
            while (trim_e > trim_a &&
                   (in_src[trim_e - 1] == ' ' || in_src[trim_e - 1] == '\t' ||
                    in_src[trim_e - 1] == '\n' || in_src[trim_e - 1] == '\r'))
                trim_e--;
            if (trim_e <= trim_a) continue;
            if (!cc__slc_arg_is_string_literal(in_src, trim_a, trim_e)) continue;
            if (cc__slc_arg_already_wrapped(in_src, trim_a, trim_e)) continue;

            want_slice = 0;
            if (have_params && ai < paramc) want_slice = param_is_slice[ai];
            if (!want_slice) want_slice = cc__slc_known_slice_arg(callee, ai);
            if (!want_slice) continue;

            arg_n = trim_e - trim_a;
            need = (int)(sizeof("const_char_to_slice()") + arg_n);
            repl = (char*)malloc((size_t)need);
            if (!repl) return -1;
            memcpy(repl, "const_char_to_slice(", 20);
            memcpy(repl + 20, in_src + trim_a, arg_n);
            repl[20 + arg_n] = ')';
            repl[20 + arg_n + 1] = '\0';
            if (cc_edit_buffer_add(eb, trim_a, trim_e, repl, 0, "slice_literal_coerce") < 0) {
                free(repl);
                return -1;
            }
            free(repl);
            edits++;
            if (dbg) {
                fprintf(stderr, "slice_lit_coerce: wrap %s arg%d\n", callee, ai);
            }
        }
    }
    if (dbg) {
        fprintf(stderr, "slice_lit_coerce: call_nodes=%d edits=%d\n", call_nodes, edits);
    }
    return edits;
}
