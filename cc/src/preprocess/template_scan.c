#include "template_scan.h"

#include "../util/text.h"
#include "../util/text_scan.h"
#include "../util/path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cc__tpl_is_tag_start(const char* src, size_t n, size_t pos) {
    return pos < n && (cc_is_ident_start(src[pos]) || src[pos] == '_');
}

static int cc__tpl_is_escaped_dollar(const char* src, size_t body_s, size_t dollar_pos) {
    if (!src || dollar_pos < body_s || src[dollar_pos] != '$') return 0;
    size_t k = dollar_pos;
    int bs = 0;
    while (k > body_s && src[k - 1] == '\\') {
        bs++;
        k--;
    }
    return (bs % 2) == 1;
}

static int cc__tpl_scan_interp_body(const char* src, size_t n, size_t brace_pos,
                                    size_t* body_end_out) {
    size_t i = brace_pos + 1;
    int brace_depth = 1;
    int paren_depth = 0;
    int bracket_depth = 0;
    int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
    if (!src || !body_end_out || brace_pos >= n || src[brace_pos] != '{') return -1;
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (in_line_comment) {
            if (c == '\n') in_line_comment = 0;
            i++;
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && c2 == '/') {
                in_block_comment = 0;
                i += 2;
                continue;
            }
            i++;
            continue;
        }
        if (in_str) {
            if (c == '\\' && i + 1 < n) {
                i += 2;
                continue;
            }
            if (c == '"') in_str = 0;
            i++;
            continue;
        }
        if (in_chr) {
            if (c == '\\' && i + 1 < n) {
                i += 2;
                continue;
            }
            if (c == '\'') in_chr = 0;
            i++;
            continue;
        }
        if (c == '/' && c2 == '/') {
            in_line_comment = 1;
            i += 2;
            continue;
        }
        if (c == '/' && c2 == '*') {
            in_block_comment = 1;
            i += 2;
            continue;
        }
        if (c == '"') {
            in_str = 1;
            i++;
            continue;
        }
        if (c == '\'') {
            in_chr = 1;
            i++;
            continue;
        }
        if (c == '{') {
            brace_depth++;
            i++;
            continue;
        }
        if (c == '}') {
            brace_depth--;
            if (brace_depth == 0 && paren_depth == 0 && bracket_depth == 0) {
                *body_end_out = i;
                return 0;
            }
            i++;
            continue;
        }
        if (c == '(') paren_depth++;
        else if (c == ')' && paren_depth > 0) paren_depth--;
        else if (c == '[') bracket_depth++;
        else if (c == ']' && bracket_depth > 0) bracket_depth--;
        i++;
    }
    return -1;
}

int cc_tpl_scan_literal(const char* src, size_t n, size_t tick_pos, size_t* tick_end_out) {
    size_t i = tick_pos + 1;
    if (!src || !tick_end_out || tick_pos >= n || src[tick_pos] != '`') return -1;
    while (i < n) {
        char c = src[i];
        if (c == '`') {
            *tick_end_out = i;
            return 0;
        }
        if (c == '$' && i + 1 < n) {
            if (i + 2 < n && src[i + 1] == '{' && src[i + 2] == '{' &&
                !cc__tpl_is_escaped_dollar(src, tick_pos + 1, i)) {
                /* ${{...}} verbatim span: skip raw to the first `}}` so its
                 * content (backticks included) can't terminate the literal */
                size_t p = i + 3;
                while (p + 1 < n && !(src[p] == '}' && src[p + 1] == '}')) p++;
                if (p + 1 >= n) return -1;
                i = p + 2;
                continue;
            }
            {
                size_t brace_pos = (size_t)-1;
                if (src[i + 1] == '{') {
                    brace_pos = i + 1;
                } else if (i + 2 < n && src[i + 1] == '~' &&
                           cc__tpl_is_tag_start(src, n, i + 2)) {
                    size_t t = i + 2;
                    while (t < n && cc_is_ident_char(src[t])) t++;
                    if (t < n && src[t] == '{') brace_pos = t;
                }
                if (brace_pos != (size_t)-1 &&
                    !cc__tpl_is_escaped_dollar(src, tick_pos + 1, i)) {
                    size_t body_end = 0;
                    if (cc__tpl_scan_interp_body(src, n, brace_pos, &body_end) != 0) return -1;
                    i = body_end + 1;
                    continue;
                }
            }
        }
        i++;
    }
    return -1;
}

int cc_template_next_piece(const char* src, size_t n,
                           size_t body_s, size_t body_e,
                           size_t* pos, CCTemplatePiece* out) {
    size_t i;
    if (!src || !pos || !out || *pos >= body_e) return 0;
    memset(out, 0, sizeof(*out));
    i = *pos;
    while (i < body_e) {
        if (src[i] == '$' && i + 1 < body_e) {
            size_t tag_s = 0, tag_e = 0, brace_pos = 0;
            int tagged = 0;
            int ok = 0;
            if (i + 2 < body_e && src[i + 1] == '{' && src[i + 2] == '{' &&
                !cc__tpl_is_escaped_dollar(src, body_s, i)) {
                /* ${{...}} verbatim span: raw until the first `}}` */
                size_t p = i + 3;
                while (p + 1 < body_e && !(src[p] == '}' && src[p + 1] == '}')) p++;
                if (p + 1 >= body_e) return -3;
                out->kind = CC_TPL_PIECE_VERBATIM;
                out->lit_off = *pos;
                out->lit_len = i - *pos;
                out->expr_off = i + 3;
                out->expr_len = p - (i + 3);
                *pos = p + 2;
                return 1;
            }
            if (src[i + 1] == '{') {
                brace_pos = i + 1;
                ok = 1;
            } else if (i + 2 < body_e && src[i + 1] == '~' &&
                       cc__tpl_is_tag_start(src, body_e, i + 2)) {
                size_t t = i + 2;
                while (t < body_e && cc_is_ident_char(src[t])) t++;
                if (t < body_e && src[t] == '{') {
                    tagged = 1;
                    tag_s = i + 2;
                    tag_e = t;
                    brace_pos = t;
                    ok = 1;
                }
            }
            if (ok && cc__tpl_is_escaped_dollar(src, body_s, i)) ok = 0;
            if (ok) {
                size_t expr_end = 0;
                if (cc__tpl_scan_interp_body(src, n, brace_pos, &expr_end) != 0) return -1;
                out->kind = tagged ? CC_TPL_PIECE_TAGGED_SLOT : CC_TPL_PIECE_SLOT;
                out->lit_off = *pos;
                out->lit_len = i - *pos;
                out->tag_off = tag_s;
                out->tag_len = tag_e - tag_s;
                out->expr_off = brace_pos + 1;
                out->expr_len = expr_end - (brace_pos + 1);
                *pos = expr_end + 1;
                return 1;
            }
        }
        i++;
    }
    out->kind = CC_TPL_PIECE_LITERAL;
    out->lit_off = *pos;
    out->lit_len = body_e - *pos;
    *pos = body_e;
    return out->lit_len > 0 ? 1 : 0;
}

static int cc__tpl_emit_call_has_backtick(const char* src, size_t n, size_t start,
                                          size_t limit) {
    size_t p = start;
    int depth = 1;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    while (p < limit && depth > 0) {
        char c = src[p];
        char c2 = (p + 1 < n && p + 1 < limit) ? src[p + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; p++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; p += 2; continue; } p++; continue; }
        if (in_str) { if (c == '\\' && p + 1 < limit) { p += 2; continue; } if (c == '"') in_str = 0; p++; continue; }
        if (in_chr) { if (c == '\\' && p + 1 < limit) { p += 2; continue; } if (c == '\'') in_chr = 0; p++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; p += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; p += 2; continue; }
        if (c == '"') { in_str = 1; p++; continue; }
        if (c == '\'') { in_chr = 1; p++; continue; }
        if (c == '`') return 1;
        if (c == '(') depth++;
        else if (c == ')') depth--;
        p++;
    }
    return 0;
}

int cc_template_body_needs_emit_exec(const char* src, size_t n,
                                     size_t body_s, size_t body_e) {
    size_t i = body_s;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    if (!src || body_s >= body_e) return 0;
    while (i < body_e) {
        char c = src[i];
        char c2 = (i + 1 < n && i + 1 < body_e) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < body_e) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < body_e) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        if (c == '@' && i + 5 < body_e && memcmp(src + i, "@emit(", 6) == 0) {
            size_t arg = cc_skip_ws_and_comments(src, n, i + 6);
            if (arg < body_e && src[arg] == '`') return 1;
            if (cc__tpl_emit_call_has_backtick(src, n, i + 6, body_e)) return 1;
        }
        i++;
    }
    return 0;
}


/* --- closer-anchored dedent (see header) --- */

/* Logical position of `off`: honors `# <line> "<file>"` markers, so a
 * diagnostic in an include-expanded buffer names the user's file and
 * line rather than an offset into the expansion. */
static size_t cc__tpl_line_of(const char* src, size_t n, size_t off,
                              char* file_out, size_t file_cap) {
    size_t line = 1, k = 0;
    if (file_out && file_cap) file_out[0] = '\0';
    while (k < off) {
        size_t ls = k, le = k;
        while (le < n && src[le] != '\n') le++;
        {
            size_t q = ls;
            while (q < le && (src[q] == ' ' || src[q] == '\t')) q++;
            if (q < le && src[q] == '#') {
                size_t d = q + 1;
                while (d < le && (src[d] == ' ' || src[d] == '\t')) d++;
                if (d < le && src[d] >= '0' && src[d] <= '9') {
                    size_t v = 0;
                    while (d < le && src[d] >= '0' && src[d] <= '9')
                        v = v * 10 + (size_t)(src[d++] - '0');
                    while (d < le && (src[d] == ' ' || src[d] == '\t')) d++;
                    if (d < le && src[d] == '"' && file_out) {
                        size_t f = d + 1, m = 0;
                        while (f < le && src[f] != '"' && m + 1 < file_cap)
                            file_out[m++] = src[f++];
                        file_out[m] = '\0';
                    }
                    /* The marker names the NEXT line's number. */
                    line = v;
                    k = le + 1;
                    continue;
                }
            }
        }
        if (le >= off) break;
        line++;
        k = le + 1;
    }
    return line;
}

char* cc_tpl_dedent_text(const char* src, size_t n, const char* input_path,
                         size_t* out_len) {
    char* out = NULL;
    size_t olen = 0, ocap = 0;
    size_t i = 0, last_emit = 0;
    int changed = 0;
    CCInertScan sc;
    if (!src || n == 0 || !memchr(src, '`', n)) return NULL;
    cc_inert_scan_init(&sc, NULL);
    while (i < n) {
        if (cc_inert_scan_step(&sc, src, n, &i)) continue;
        if (src[i] != '`') { i++; continue; }
        {
            size_t tick_s = i, tick_e = 0;
            size_t ls, mlen, open_nl, p;
            int alone = 1;
            if (cc_tpl_scan_literal(src, n, tick_s, &tick_e) != 0) {
                /* Unterminated: leave it for the pass that will diagnose. */
                i++;
                continue;
            }
            ls = tick_e;
            while (ls > 0 && src[ls - 1] != '\n') ls--;
            for (size_t k = ls; k < tick_e; k++)
                if (src[k] != ' ' && src[k] != '\t') { alone = 0; break; }
            mlen = tick_e - ls;
            /* No dedent when the closer shares its line with content, when
             * the margin is empty (every pre-rule template), or when the
             * whole template sits on one line. */
            if (!alone || mlen == 0 || ls <= tick_s) { i = tick_e + 1; continue; }
            open_nl = tick_s + 1;
            while (open_nl < tick_e && src[open_nl] != '\n') open_nl++;
            if (open_nl >= tick_e) { i = tick_e + 1; continue; }
            /* Emit through the opening line (its remainder is not margined:
             * it starts mid-line, after the tick). */
            cc_sb_append(&out, &olen, &ocap, src + last_emit, open_nl + 1 - last_emit);
            p = open_nl + 1;
            while (p < tick_e) {
                size_t le = p, q = p;
                if (p == ls) { p = tick_e; break; } /* the closer's margin: drop it */
                while (le < tick_e && src[le] != '\n') le++;
                while (q < le && (src[q] == ' ' || src[q] == '\t')) q++;
                if (q == le) {
                    /* Blank line: nothing to dedent, bytes pass through. */
                    cc_sb_append(&out, &olen, &ocap, src + p, le + 1 - p);
                } else if (le - p >= mlen && memcmp(src + p, src + ls, mlen) == 0) {
                    cc_sb_append(&out, &olen, &ocap, src + p + mlen, le + 1 - (p + mlen));
                } else {
                    char rel[1024];
                    char mfile[512];
                    size_t mline = cc__tpl_line_of(src, n, p, mfile, sizeof(mfile));
                    fprintf(stderr,
                            "%s:%zu:1: error: template: line indented less than "
                            "the closing backtick's margin (the closer's "
                            "indentation is the margin stripped from every "
                            "line; indent the line to at least match it)\n",
                            mfile[0] ? mfile
                                     : (input_path
                                            ? cc_path_rel_to_repo(input_path, rel, sizeof(rel))
                                            : "<input>"),
                            mline);
                    free(out);
                    return (char*)-1;
                }
                p = le + 1;
            }
            cc_sb_append(&out, &olen, &ocap, "`", 1);
            changed = 1;
            last_emit = tick_e + 1;
            i = tick_e + 1;
        }
    }
    if (!changed) { free(out); return NULL; }
    cc_sb_append(&out, &olen, &ocap, src + last_emit, n - last_emit);
    if (out_len) *out_len = olen;
    return out;
}
