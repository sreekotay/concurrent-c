#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/*
 * Text-based @async lowering (state machine) used after all span-driven rewrites.
 *
 * Scope: very small + intentionally conservative.
 * - Lowers `@async void|int|intptr_t name(params) { ... }` into a poll-based Task<intptr_t>:
 *     CCTaskIntptr name(params)
 * - Supports these statement forms inside the function body:
 *   - `int x = expr;` / `intptr_t x = expr;`   (hoisted into frame as `intptr_t x`)
 *   - `await expr;`
 *   - `x = await expr;`  (x must be a hoisted local)
 *   - `return expr;`
 *   - `return await expr;`
 *
 * The goal is to unblock task-based auto-mixing + batching without relying on stub-AST spans.
 * Anything outside this subset is left as-is (no rewrite).
 */

static int cc__is_ident_start(char c) {
    return (c == '_') || isalpha((unsigned char)c);
}
static int cc__is_ident_char(char c) {
    return (c == '_') || isalnum((unsigned char)c);
}
static int cc__is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static size_t cc__skip_ws_and_comments_bounded(const char* b, size_t bl, size_t pos) {
    if (!b) return pos;
    for (;;) {
        while (pos < bl && cc__is_ws(b[pos])) pos++;
        if (pos + 1 < bl && b[pos] == '/' && b[pos + 1] == '/') {
            pos += 2;
            while (pos < bl && b[pos] != '\n') pos++;
            continue;
        }
        if (pos + 1 < bl && b[pos] == '/' && b[pos + 1] == '*') {
            pos += 2;
            while (pos + 1 < bl) {
                if (b[pos] == '*' && b[pos + 1] == '/') { pos += 2; break; }
                pos++;
            }
            continue;
        }
        return pos;
    }
}

static const char* cc__skip_ws_and_comments(const char* p) {
    if (!p) return p;
    for (;;) {
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (p[0] == '/' && p[1] == '/') {
            p += 2;
            while (*p && *p != '\n') p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p) {
                if (p[0] == '*' && p[1] == '/') { p += 2; break; }
                p++;
            }
            continue;
        }
        return p;
    }
}

static void cc__sb_reserve(char** io_s, size_t* io_len, size_t* io_cap, size_t extra) {
    if (!io_s || !io_len || !io_cap) return;
    size_t need = *io_len + extra + 1;
    if (*io_cap >= need) return;
    size_t nc = *io_cap ? *io_cap * 2 : 256;
    while (nc < need) nc *= 2;
    char* ns = (char*)realloc(*io_s, nc);
    if (!ns) return;
    *io_s = ns;
    *io_cap = nc;
}

static void cc__sb_append_n(char** io_s, size_t* io_len, size_t* io_cap, const char* p, size_t n) {
    if (!io_s || !io_len || !io_cap || !p) return;
    cc__sb_reserve(io_s, io_len, io_cap, n);
    if (!*io_s) return;
    memcpy(*io_s + *io_len, p, n);
    *io_len += n;
    (*io_s)[*io_len] = 0;
}

static void cc__sb_append_cstr(char** io_s, size_t* io_len, size_t* io_cap, const char* p) {
    if (!p) return;
    cc__sb_append_n(io_s, io_len, io_cap, p, strlen(p));
}

static void cc__sb_append_fmt(char** io_s, size_t* io_len, size_t* io_cap, const char* fmt, ...) {
    if (!io_s || !io_len || !io_cap || !fmt) return;
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n < sizeof(tmp)) {
        cc__sb_append_n(io_s, io_len, io_cap, tmp, (size_t)n);
        return;
    }
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) return;
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    cc__sb_append_n(io_s, io_len, io_cap, buf, (size_t)n);
    free(buf);
}

static char* cc__rewrite_idents(const char* s,
                                const char* const* names,
                                const char* const* repls,
                                int n) {
    if (!s) return NULL;
    if (n <= 0) return strdup(s);
    size_t sl = strlen(s);
    size_t out_cap = sl * 3 + 64;
    char* out = (char*)malloc(out_cap);
    if (!out) return NULL;
    size_t out_len = 0;

    for (size_t i = 0; i < sl; ) {
        if (cc__is_ident_start(s[i])) {
            size_t j = i + 1;
            while (j < sl && cc__is_ident_char(s[j])) j++;
            int did = 0;
            for (int k = 0; k < n; k++) {
                size_t nl = strlen(names[k]);
                if (nl == (j - i) && memcmp(s + i, names[k], nl) == 0) {
                    const char* r = repls[k];
                    size_t rl = strlen(r);
                    if (out_len + rl + 1 >= out_cap) {
                        out_cap = out_cap * 2 + rl + 64;
                        out = (char*)realloc(out, out_cap);
                        if (!out) return NULL;
                    }
                    memcpy(out + out_len, r, rl);
                    out_len += rl;
                    did = 1;
                    break;
                }
            }
            if (!did) {
                size_t tl = j - i;
                if (out_len + tl + 1 >= out_cap) {
                    out_cap = out_cap * 2 + tl + 64;
                    out = (char*)realloc(out, out_cap);
                    if (!out) return NULL;
                }
                memcpy(out + out_len, s + i, tl);
                out_len += tl;
            }
            i = j;
            continue;
        }
        if (out_len + 2 >= out_cap) {
            out_cap = out_cap * 2 + 64;
            out = (char*)realloc(out, out_cap);
            if (!out) return NULL;
        }
        out[out_len++] = s[i++];
    }
    out[out_len] = 0;
    return out;
}

typedef struct {
    size_t start;
    size_t end;
    int orig_nl;
    int ret_is_void;
    char name[128];
    char params[512];
    const char* body;
    size_t body_len;
} CCAsyncFnText;

typedef enum {
    CC_ASYNC_STMT_SEMI = 0,
    CC_ASYNC_STMT_IF = 1,
} CCAsyncStmtKind;

typedef struct {
    CCAsyncStmtKind kind;
    char* text;     /* for SEMI: statement text (no trailing ';') */
    char* cond;     /* for IF: condition expr text */
    char* then_body;/* for IF: contents inside then { ... } */
    char* else_body;/* for IF: contents inside else { ... } or NULL */
} CCAsyncStmt;

typedef struct {
    size_t off;  /* offset in output buffer */
    int value;   /* value to patch */
    int set;
} CCIntFixup6;

static size_t cc__sb_append_fixup6(char** io_s, size_t* io_len, size_t* io_cap) {
    /* Reserve exactly 6 chars; later patched with right-aligned decimal and spaces. */
    size_t off = *io_len;
    cc__sb_append_cstr(io_s, io_len, io_cap, "      ");
    return off;
}

static void cc__sb_patch_fixup6(char* s, size_t len, size_t off, int value) {
    if (!s || off + 6 > len) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%6d", value);
    memcpy(s + off, buf, 6);
}

static int cc__count_nl(const char* s, size_t a, size_t b) {
    int nl = 0;
    for (size_t i = a; i < b; i++) if (s[i] == '\n') nl++;
    return nl;
}

static void cc__line_col_for_offset(const char* s, size_t n, size_t off, int* out_line, int* out_col) {
    if (out_line) *out_line = 1;
    if (out_col) *out_col = 1;
    if (!s) return;
    if (off > n) off = n;
    int line = 1;
    int col = 1;
    for (size_t i = 0; i < off; i++) {
        if (s[i] == '\n') { line++; col = 1; }
        else col++;
    }
    if (out_line) *out_line = line;
    if (out_col) *out_col = col;
}

static int cc__is_kw_at(const char* b, size_t bl, size_t pos, const char* kw) {
    if (!b || !kw) return 0;
    size_t kl = strlen(kw);
    if (pos + kl > bl) return 0;
    if (memcmp(b + pos, kw, kl) != 0) return 0;
    char prev = (pos > 0) ? b[pos - 1] : 0;
    char next = (pos + kl < bl) ? b[pos + kl] : 0;
    if (cc__is_ident_char(prev)) return 0;
    if (cc__is_ident_char(next)) return 0;
    return 1;
}

static char* cc__dup_slice(const char* b, size_t s, size_t e) {
    if (!b || e <= s) return strdup("");
    size_t n = e - s;
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, b + s, n);
    out[n] = 0;
    return out;
}

static int cc__split_async_top_level_stmts(const char* b, size_t bl, CCAsyncStmt* out, int out_cap, int* out_n, int* out_has_trailing) {
    if (!b || !out || !out_n || out_cap <= 0) return 0;
    *out_n = 0;
    if (out_has_trailing) *out_has_trailing = 0;

    size_t pos = 0;
    while (pos < bl) {
        pos = cc__skip_ws_and_comments_bounded(b, bl, pos);
        if (pos >= bl) break;

        if (*out_n >= out_cap) return 0;

        /* if (...) { ... } [else { ... }] */
        if (cc__is_kw_at(b, bl, pos, "if")) {
            size_t p = pos + 2;
            p = cc__skip_ws_and_comments_bounded(b, bl, p);
            if (p >= bl || b[p] != '(') return 0;
            size_t cond_s = p + 1;
            int par = 1, brk = 0, br = 0;
            int ins = 0; char q = 0;
            int in_lc = 0, in_bc = 0;
            for (p = cond_s; p < bl; p++) {
                char ch = b[p];
                char ch2 = (p + 1 < bl) ? b[p + 1] : 0;
                if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
                if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; p++; } continue; }
                if (ins) { if (ch == '\\' && p + 1 < bl) { p++; continue; } if (ch == q) ins = 0; continue; }
                if (ch == '/' && ch2 == '/') { in_lc = 1; p++; continue; }
                if (ch == '/' && ch2 == '*') { in_bc = 1; p++; continue; }
                if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
                if (ch == '(') par++;
                else if (ch == ')') { par--; if (par == 0) break; }
                else if (ch == '[') brk++;
                else if (ch == ']') { if (brk) brk--; }
                else if (ch == '{') br++;
                else if (ch == '}') { if (br) br--; }
            }
            if (par != 0 || p >= bl) return 0;
            size_t cond_e = p;
            size_t after_rpar = p + 1;

            size_t tb = cc__skip_ws_and_comments_bounded(b, bl, after_rpar);
            if (tb >= bl || b[tb] != '{') return 0;
            size_t then_l = tb;
            int depth = 0;
            size_t q2 = then_l;
            for (; q2 < bl; q2++) {
                char ch = b[q2];
                char ch2 = (q2 + 1 < bl) ? b[q2 + 1] : 0;
                if (ch == '"' || ch == '\'') {
                    char quote = ch;
                    q2++;
                    while (q2 < bl) {
                        char c2 = b[q2];
                        if (c2 == '\\' && q2 + 1 < bl) { q2 += 2; continue; }
                        if (c2 == quote) break;
                        q2++;
                    }
                    continue;
                }
                if (ch == '/' && ch2 == '/') { q2 += 2; while (q2 < bl && b[q2] != '\n') q2++; continue; }
                if (ch == '/' && ch2 == '*') { q2 += 2; while (q2 + 1 < bl && !(b[q2] == '*' && b[q2 + 1] == '/')) q2++; if (q2 + 1 < bl) q2++; continue; }
                if (ch == '{') depth++;
                else if (ch == '}') { depth--; if (depth == 0) { q2++; break; } }
            }
            if (depth != 0) return 0;
            size_t then_r = q2;

            size_t eb = cc__skip_ws_and_comments_bounded(b, bl, then_r);
            char* else_body = NULL;
            size_t else_r = then_r;
            if (eb < bl && cc__is_kw_at(b, bl, eb, "else")) {
                size_t ep = eb + 4;
                ep = cc__skip_ws_and_comments_bounded(b, bl, ep);
                if (ep >= bl || b[ep] != '{') return 0;
                size_t else_l = ep;
                depth = 0;
                size_t q3 = else_l;
                for (; q3 < bl; q3++) {
                    char ch = b[q3];
                    char ch2 = (q3 + 1 < bl) ? b[q3 + 1] : 0;
                    if (ch == '"' || ch == '\'') {
                        char quote = ch;
                        q3++;
                        while (q3 < bl) {
                            char c2 = b[q3];
                            if (c2 == '\\' && q3 + 1 < bl) { q3 += 2; continue; }
                            if (c2 == quote) break;
                            q3++;
                        }
                        continue;
                    }
                    if (ch == '/' && ch2 == '/') { q3 += 2; while (q3 < bl && b[q3] != '\n') q3++; continue; }
                    if (ch == '/' && ch2 == '*') { q3 += 2; while (q3 + 1 < bl && !(b[q3] == '*' && b[q3 + 1] == '/')) q3++; if (q3 + 1 < bl) q3++; continue; }
                    if (ch == '{') depth++;
                    else if (ch == '}') { depth--; if (depth == 0) { q3++; break; } }
                }
                if (depth != 0) return 0;
                size_t else_end = q3;
                else_body = cc__dup_slice(b, else_l + 1, else_end - 1);
                if (!else_body) return 0;
                else_r = else_end;
            }

            char* cond = cc__dup_slice(b, cond_s, cond_e);
            char* then_body = cc__dup_slice(b, then_l + 1, then_r - 1);
            if (!cond || !then_body) { free(cond); free(then_body); free(else_body); return 0; }

            out[*out_n] = (CCAsyncStmt){
                .kind = CC_ASYNC_STMT_IF,
                .text = NULL,
                .cond = cond,
                .then_body = then_body,
                .else_body = else_body,
            };
            (*out_n)++;
            pos = else_r;
            continue;
        }

        /* semicolon statement */
        size_t s = pos;
        int par2 = 0, brk2 = 0, br2 = 0;
        int ins2 = 0; char q2 = 0;
        int in_lc2 = 0, in_bc2 = 0;
        size_t k = s;
        for (; k < bl; k++) {
            char ch = b[k];
            char ch2 = (k + 1 < bl) ? b[k + 1] : 0;
            if (in_lc2) { if (ch == '\n') in_lc2 = 0; continue; }
            if (in_bc2) { if (ch == '*' && ch2 == '/') { in_bc2 = 0; k++; } continue; }
            if (ins2) { if (ch == '\\' && k + 1 < bl) { k++; continue; } if (ch == q2) ins2 = 0; continue; }
            if (ch == '/' && ch2 == '/') { in_lc2 = 1; k++; continue; }
            if (ch == '/' && ch2 == '*') { in_bc2 = 1; k++; continue; }
            if (ch == '"' || ch == '\'') { ins2 = 1; q2 = ch; continue; }
            if (ch == '(') par2++;
            else if (ch == ')') { if (par2) par2--; }
            else if (ch == '[') brk2++;
            else if (ch == ']') { if (brk2) brk2--; }
            else if (ch == '{') br2++;
            else if (ch == '}') { if (br2) br2--; }
            else if (ch == ';' && par2 == 0 && brk2 == 0 && br2 == 0) break;
        }
        if (k >= bl || b[k] != ';') {
            if (out_has_trailing) *out_has_trailing = 1;
            return 1;
        }
        size_t e = k;
        while (s < e && cc__is_ws(b[s])) s++;
        while (e > s && cc__is_ws(b[e - 1])) e--;
        if (e > s) {
            char* st = cc__dup_slice(b, s, e);
            if (!st) return 0;
            out[*out_n] = (CCAsyncStmt){ .kind = CC_ASYNC_STMT_SEMI, .text = st, .cond = NULL, .then_body = NULL, .else_body = NULL };
            (*out_n)++;
        }
        pos = k + 1;
    }
    return 1;
}

int cc_async_rewrite_state_machine_text(const char* in_src,
                                        size_t in_len,
                                        char** out_src,
                                        size_t* out_len) {
    if (!in_src || !out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;

    CCAsyncFnText fns[128];
    int fn_n = 0;

    /* Find @async definitions. */
    for (size_t i = 0; i + 6 < in_len && fn_n < (int)(sizeof(fns) / sizeof(fns[0])); ) {
        if (in_src[i] != '@') { i++; continue; }
        size_t j = i + 1;
        while (j < in_len && (in_src[j] == ' ' || in_src[j] == '\t')) j++;
        if (j + 5 > in_len || memcmp(in_src + j, "async", 5) != 0) { i++; continue; }
        size_t p = j + 5;
        if (p < in_len && cc__is_ident_char(in_src[p])) { i++; continue; }
        while (p < in_len && cc__is_ws(in_src[p])) p++;

        int ret_is_void = 0;
        if (p + 4 <= in_len && memcmp(in_src + p, "void", 4) == 0) { ret_is_void = 1; p += 4; }
        else if (p + 8 <= in_len && memcmp(in_src + p, "intptr_t", 8) == 0) { ret_is_void = 0; p += 8; }
        else if (p + 3 <= in_len && memcmp(in_src + p, "int", 3) == 0) { ret_is_void = 0; p += 3; }
        else { i++; continue; }

        while (p < in_len && (in_src[p] == ' ' || in_src[p] == '\t')) p++;
        if (p >= in_len || !cc__is_ident_start(in_src[p])) { i++; continue; }
        size_t ns = p++;
        while (p < in_len && cc__is_ident_char(in_src[p])) p++;
        size_t nn = p - ns;
        if (nn == 0 || nn >= sizeof(fns[0].name)) { i++; continue; }
        while (p < in_len && (in_src[p] == ' ' || in_src[p] == '\t')) p++;
        if (p >= in_len || in_src[p] != '(') { i++; continue; }

        size_t ps = p;
        int par = 0;
        for (; p < in_len; p++) {
            char ch = in_src[p];
            if (ch == '(') par++;
            else if (ch == ')') { par--; if (par == 0) { p++; break; } }
        }
        if (par != 0) { i++; continue; }
        size_t pe = p;
        size_t params_len = pe - ps;
        if (params_len >= sizeof(fns[0].params)) { i++; continue; }
        while (p < in_len && cc__is_ws(in_src[p])) p++;
        if (p >= in_len || in_src[p] != '{') { i++; continue; } /* definition only */

        size_t body_lbrace = p;
        int depth = 0;
        size_t q = body_lbrace;
        for (; q < in_len; q++) {
            char ch = in_src[q];
            if (ch == '"' || ch == '\'') {
                char quote = ch;
                q++;
                while (q < in_len) {
                    char c2 = in_src[q];
                    if (c2 == '\\' && q + 1 < in_len) { q += 2; continue; }
                    if (c2 == quote) break;
                    q++;
                }
                continue;
            }
            if (ch == '{') depth++;
            else if (ch == '}') { depth--; if (depth == 0) { q++; break; } }
        }
        if (depth != 0) { i++; continue; }
        size_t end = q;
        while (end < in_len && in_src[end] != '\n') end++;
        if (end < in_len) end++;

        CCAsyncFnText fn;
        memset(&fn, 0, sizeof(fn));
        fn.start = i;
        fn.end = end;
        fn.orig_nl = cc__count_nl(in_src, fn.start, fn.end);
        fn.ret_is_void = ret_is_void;
        memcpy(fn.name, in_src + ns, nn);
        fn.name[nn] = 0;
        memcpy(fn.params, in_src + ps, params_len);
        fn.params[params_len] = 0;
        fn.body = in_src + body_lbrace + 1;
        fn.body_len = (size_t)((in_src + (q - 1)) - fn.body);
        fns[fn_n++] = fn;

        i = end;
    }

    if (fn_n == 0) return 0;

    char* cur = (char*)malloc(in_len + 1);
    if (!cur) return 0;
    memcpy(cur, in_src, in_len);
    cur[in_len] = 0;
    size_t cur_len = in_len;

    static int g_id = 50000;
    for (int fi = fn_n - 1; fi >= 0; fi--) {
        CCAsyncFnText* fn = &fns[fi];
        int id = g_id++;

        /* Split body into top-level statements: semicolon statements + conservative if/else blocks. */
        CCAsyncStmt stmts[256];
        memset(stmts, 0, sizeof(stmts));
        int stmt_n = 0;
        int body_has_trailing_nonsemi = 0;
        if (!cc__split_async_top_level_stmts(fn->body, fn->body_len, stmts, 256, &stmt_n, &body_has_trailing_nonsemi)) {
            body_has_trailing_nonsemi = 1;
        }

        /* Collect parameter names (best-effort: last identifier in each comma-separated item). */
        char* params[32] = {0};
        int param_n = 0;
        {
            const char* p = fn->params;
            size_t pl = strlen(fn->params);
            if (pl >= 2 && p[0] == '(') { p++; pl--; }
            if (pl >= 1 && p[pl - 1] == ')') pl--;
            const char* endp = p + pl;
            const char* curp = p;
            while (curp < endp && param_n < 32) {
                while (curp < endp && cc__is_ws(*curp)) curp++;
                const char* item_s = curp;
                int par3 = 0, brk3 = 0;
                int ins3 = 0; char q3 = 0;
                while (curp < endp) {
                    char ch = *curp;
                    if (ins3) {
                        if (ch == '\\' && curp + 1 < endp) { curp += 2; continue; }
                        if (ch == q3) ins3 = 0;
                        curp++;
                        continue;
                    }
                    if (ch == '"' || ch == '\'') { ins3 = 1; q3 = ch; curp++; continue; }
                    if (ch == '(') par3++;
                    else if (ch == ')') { if (par3) par3--; }
                    else if (ch == '[') brk3++;
                    else if (ch == ']') { if (brk3) brk3--; }
                    else if (ch == ',' && par3 == 0 && brk3 == 0) break;
                    curp++;
                }
                const char* item_e = curp;
                if (curp < endp && *curp == ',') curp++;
                while (item_e > item_s && cc__is_ws(item_e[-1])) item_e--;
                if (item_e <= item_s) continue;
                if ((item_e - item_s) == 4 && memcmp(item_s, "void", 4) == 0) continue;

                const char* e = item_e;
                while (e > item_s && !cc__is_ident_char(e[-1])) e--;
                const char* e2 = e;
                while (e > item_s && cc__is_ident_char(e[-1])) e--;
                if (e2 > e && cc__is_ident_start(*e)) {
                    size_t ln = (size_t)(e2 - e);
                    char* nm = (char*)malloc(ln + 1);
                    if (nm) { memcpy(nm, e, ln); nm[ln] = 0; params[param_n++] = nm; }
                }
            }
        }

        /* Collect locals declared with int/intptr_t. */
        char* locals[64] = {0};
        int local_n = 0;
        for (int si = 0; si < stmt_n && local_n < 64; si++) {
            if (stmts[si].kind != CC_ASYNC_STMT_SEMI) continue;
            const char* st = stmts[si].text;
            if (!st) continue;
            const char* p = cc__skip_ws_and_comments(st);
            if (memcmp(p, "int ", 4) != 0 && memcmp(p, "intptr_t ", 9) != 0) continue;
            p += (memcmp(p, "int ", 4) == 0) ? 4 : 9;
            while (*p == ' ' || *p == '\t') p++;
            if (!cc__is_ident_start(*p)) continue;
            const char* ns = p++;
            while (cc__is_ident_char(*p)) p++;
            size_t nn = (size_t)(p - ns);
            if (nn == 0 || nn >= 128) continue;
            char* nm = (char*)malloc(nn + 1);
            if (!nm) continue;
            memcpy(nm, ns, nn);
            nm[nn] = 0;
            locals[local_n++] = nm;
        }

        /* Identifier map for locals + params. */
        const char* map_names[96];
        const char* map_repls[96];
        int map_n = 0;
        for (int k = 0; k < local_n && map_n < 96; k++) {
            map_names[map_n] = locals[k];
            char* r = (char*)malloc(strlen(locals[k]) + 8);
            if (!r) continue;
            sprintf(r, "__f->%s", locals[k]);
            map_repls[map_n++] = r;
        }
        for (int k = 0; k < param_n && map_n < 96; k++) {
            map_names[map_n] = params[k];
            char* r = (char*)malloc(strlen(params[k]) + 16);
            if (!r) continue;
            sprintf(r, "__f->__p_%s", params[k]);
            map_repls[map_n++] = r;
        }

        /* Ensure body only uses supported await shapes, and no trailing non-; top-level statements. */
        int ok = body_has_trailing_nonsemi ? 0 : 1;
        for (int si = 0; si < stmt_n; si++) {
            if (stmts[si].kind == CC_ASYNC_STMT_SEMI) {
                const char* st = stmts[si].text;
            if (!st) continue;
            if (strstr(st, "await") == NULL) continue;
                const char* p = cc__skip_ws_and_comments(st);
            if (memcmp(p, "await", 5) == 0) continue;
            if (memcmp(p, "return await", 11) == 0) continue;
            if (strstr(p, "= await") != NULL) continue;
            ok = 0;
            break;
            } else if (stmts[si].kind == CC_ASYNC_STMT_IF) {
                /* Conservative: no nested control flow; no decls inside branches; await only in supported semicolon forms.
                   NOTE: return IS allowed inside branches now (handled by lowering). */
                const char* cb = stmts[si].cond ? stmts[si].cond : "";
                if (strstr(cb, "await") != NULL) { ok = 0; break; } /* no await in condition */

                CCAsyncStmt then_st[256];
                int tn = 0, ttrail = 0;
                memset(then_st, 0, sizeof(then_st));
                if (!cc__split_async_top_level_stmts(stmts[si].then_body, strlen(stmts[si].then_body), then_st, 256, &tn, &ttrail) || ttrail) {
                    ok = 0;
                    break;
                }
                for (int ti = 0; ti < tn; ti++) {
                    if (then_st[ti].kind != CC_ASYNC_STMT_SEMI) { ok = 0; break; }
                    const char* p = cc__skip_ws_and_comments(then_st[ti].text);
                    if (memcmp(p, "int ", 4) == 0 || memcmp(p, "intptr_t ", 9) == 0) { ok = 0; break; }
                    if (strstr(p, "await") == NULL) continue;
                    if (memcmp(p, "await", 5) == 0) continue;
                    if (memcmp(p, "return await", 11) == 0) continue;
                    if (strstr(p, "= await") != NULL) continue;
                    if (memcmp(p, "return", 6) == 0) continue;
                    ok = 0;
                    break;
                }
                for (int ti = 0; ti < tn; ti++) free(then_st[ti].text), free(then_st[ti].cond), free(then_st[ti].then_body), free(then_st[ti].else_body);
                if (!ok) break;

                if (stmts[si].else_body) {
                    CCAsyncStmt else_st[256];
                    int en = 0, etrail = 0;
                    memset(else_st, 0, sizeof(else_st));
                    if (!cc__split_async_top_level_stmts(stmts[si].else_body, strlen(stmts[si].else_body), else_st, 256, &en, &etrail) || etrail) {
                        ok = 0;
                        break;
                    }
                    for (int ei = 0; ei < en; ei++) {
                        if (else_st[ei].kind != CC_ASYNC_STMT_SEMI) { ok = 0; break; }
                        const char* p = cc__skip_ws_and_comments(else_st[ei].text);
                        if (memcmp(p, "int ", 4) == 0 || memcmp(p, "intptr_t ", 9) == 0) { ok = 0; break; }
                        if (strstr(p, "await") == NULL) continue;
                        if (memcmp(p, "await", 5) == 0) continue;
                        if (memcmp(p, "return await", 11) == 0) continue;
                        if (strstr(p, "= await") != NULL) continue;
                        if (memcmp(p, "return", 6) == 0) continue;
                        ok = 0;
                        break;
                    }
                    for (int ei = 0; ei < en; ei++) free(else_st[ei].text), free(else_st[ei].cond), free(else_st[ei].then_body), free(else_st[ei].else_body);
                    if (!ok) break;
                } else {
                    ok = 0; /* still require else for now */
                    break;
                }
            }
        }
        if (!ok) {
            int line = 1, col = 1;
            cc__line_col_for_offset(in_src, in_len, fn->start, &line, &col);
            fprintf(stderr,
                    "CC:%d:%d: error: CC: @async lowering is not implemented for function '%s' yet (unsupported body)\n",
                    line, col, fn->name);
            for (int si = 0; si < stmt_n; si++) { free(stmts[si].text); free(stmts[si].cond); free(stmts[si].then_body); free(stmts[si].else_body); }
            for (int k = 0; k < local_n; k++) free(locals[k]);
            for (int k = 0; k < map_n; k++) free((void*)map_repls[k]);
            free(cur);
            return -1;
        }

        char* repl = NULL;
        size_t repl_len = 0, repl_cap = 0;

        cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "typedef struct{int __st; intptr_t __r;");
        for (int k = 0; k < local_n; k++) cc__sb_append_fmt(&repl, &repl_len, &repl_cap, " intptr_t %s;", locals[k]);
        for (int k = 0; k < param_n; k++) cc__sb_append_fmt(&repl, &repl_len, &repl_cap, " intptr_t __p_%s;", params[k]);
        for (int k = 0; k < 16; k++) cc__sb_append_fmt(&repl, &repl_len, &repl_cap, " CCTaskIntptr __t%d;", k);
        cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "}__cc_af%d_f;", id);

        cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                          "static CCFutureStatus __cc_af%d_poll(void*__p,intptr_t*__o,int*__e){__cc_af%d_f*__f=(__cc_af%d_f*)__p;if(!__f)return CC_FUTURE_ERR;switch(__f->__st){case 0:",
                          id, id, id);

        /* init locals */
        for (int si = 0; si < stmt_n; si++) {
            if (stmts[si].kind != CC_ASYNC_STMT_SEMI) continue;
            const char* st = stmts[si].text;
            if (!st) continue;
            const char* p = cc__skip_ws_and_comments(st);
            int is_decl = 0;
            if (memcmp(p, "int ", 4) == 0) { is_decl = 1; p += 4; }
            else if (memcmp(p, "intptr_t ", 9) == 0) { is_decl = 1; p += 9; }
            if (!is_decl) continue;
            while (*p == ' ' || *p == '\t') p++;
            const char* ns = p++;
            while (cc__is_ident_char(*p)) p++;
            size_t nn = (size_t)(p - ns);
            if (nn == 0 || nn >= 128) continue;
            char nm[128];
            memcpy(nm, ns, nn);
            nm[nn] = 0;
            while (*p == ' ' || *p == '\t') p++;
            const char* init = "0";
            if (*p == '=') { p++; while (*p == ' ' || *p == '\t') p++; init = p; }
            char* init2 = cc__rewrite_idents(init, map_names, map_repls, map_n);
            if (init2) {
                cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "__f->%s=(intptr_t)(%s);\n", nm, init2);
                free(init2);
            }
        }
        cc__sb_append_cstr(&repl, &repl_len, &repl_cap, "__f->__st=1;/*fall*/case 1:{\n");

        int state = 1;
        int task_idx = 0;
        int finished = 0;
        for (int si = 0; si < stmt_n && !finished; si++) {
            if (stmts[si].kind == CC_ASYNC_STMT_IF) {
                /* Conservative if/else lowering: else is required; no returns inside branches (validated earlier).
                   Uses patchable state numbers so awaits inside branches can allocate fresh cases without collision. */
                char* cond2 = cc__rewrite_idents(stmts[si].cond ? stmts[si].cond : "0", map_names, map_repls, map_n);
                if (!cond2) continue;

                int then_state = state + 1;
                int else_state = 0;
                int after_state = 0;

                cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                                  "int __cc_if_l%d=(%s);__f->__st=__cc_if_l%d?",
                                  state, cond2, state);
                free(cond2);
                size_t then_patch_off = cc__sb_append_fixup6(&repl, &repl_len, &repl_cap);
                cc__sb_append_cstr(&repl, &repl_len, &repl_cap, ":");
                size_t else_patch_off = cc__sb_append_fixup6(&repl, &repl_len, &repl_cap);
                cc__sb_append_cstr(&repl, &repl_len, &repl_cap, ";return CC_FUTURE_PENDING;}");

                /* Patch then target immediately (it's just the next state). */
                {
                    cc__sb_patch_fixup6(repl, repl_len, then_patch_off, then_state);
                }

                cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "case %d:{\n", then_state);
                state = then_state;

                /* Then branch statements */
                CCAsyncStmt then_st[256];
                int tn = 0, ttrail = 0;
                memset(then_st, 0, sizeof(then_st));
                (void)cc__split_async_top_level_stmts(stmts[si].then_body, strlen(stmts[si].then_body), then_st, 256, &tn, &ttrail);
                int then_closed = 0;
                for (int ti = 0; ti < tn && !finished; ti++) {
                    const char* p = cc__skip_ws_and_comments(then_st[ti].text);
                    if (memcmp(p, "return", 6) == 0) {
                        const char* rp = p + 6;
                        while (*rp == ' ' || *rp == '\t') rp++;
                        if (fn->ret_is_void && *rp == 0) {
                            cc__sb_append_cstr(&repl, &repl_len, &repl_cap, "__f->__r=0;__f->__st=999;return CC_FUTURE_PENDING;}\n");
                            /* Ensure we stop generating the rest of this branch. */
                            then_closed = 1;
                            break;
                        }
                        if (memcmp(rp, "await", 5) == 0 && task_idx < 16) {
                            rp += 5;
                            while (*rp == ' ' || *rp == '\t') rp++;
                            char* ex = cc__rewrite_idents(rp, map_names, map_repls, map_n);
                            if (ex) {
                                cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "__f->__t%d=(%s);__f->__st=%d;return CC_FUTURE_PENDING;}\n",
                                                  task_idx, ex, state + 1);
                                free(ex);
                                cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                                                  "case %d:{intptr_t __v=0;int __err=0;CCFutureStatus __st=cc_task_intptr_poll(&__f->__t%d,&__v,&__err);if(__st==CC_FUTURE_PENDING)return CC_FUTURE_PENDING;cc_task_intptr_free(&__f->__t%d);__f->__r=__v;__f->__st=999;return CC_FUTURE_PENDING;}\n",
                                                  state + 1, task_idx, task_idx);
                                state += 1;
                                task_idx++;
                                then_closed = 1;
                                break;
                            }
                        }
                        {
                            char* ex = cc__rewrite_idents(rp, map_names, map_repls, map_n);
                            if (ex) {
                                cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "__f->__r=(intptr_t)(%s);__f->__st=999;return CC_FUTURE_PENDING;}\n", ex);
                                free(ex);
                                then_closed = 1;
                                break;
                            }
                        }
                    }
                    const char* eq = strchr(p, '=');
                    if (eq && task_idx < 16) {
                        const char* rhs = eq + 1;
                        while (*rhs == ' ' || *rhs == '\t') rhs++;
                        if (memcmp(rhs, "await", 5) == 0) {
                            rhs += 5;
                            while (*rhs == ' ' || *rhs == '\t') rhs++;
                            const char* lhs_e = eq;
                            while (lhs_e > p && (lhs_e[-1] == ' ' || lhs_e[-1] == '\t')) lhs_e--;
                            const char* lhs_s = lhs_e;
                            while (lhs_s > p && cc__is_ident_char(lhs_s[-1])) lhs_s--;
                            size_t ln = (size_t)(lhs_e - lhs_s);
                            if (ln > 0 && ln < 128) {
                                char lhs[128];
                                memcpy(lhs, lhs_s, ln);
                                lhs[ln] = 0;
                                char* lhs2 = cc__rewrite_idents(lhs, map_names, map_repls, map_n);
                                char* ex = cc__rewrite_idents(rhs, map_names, map_repls, map_n);
                                if (lhs2 && ex) {
                                    cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "__f->__t%d=(%s);__f->__st=%d;return CC_FUTURE_PENDING;",
                                                      task_idx, ex, state + 1);
                                    cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                                                      "}case %d:{intptr_t __v=0;int __err=0;CCFutureStatus __st=cc_task_intptr_poll(&__f->__t%d,&__v,&__err);if(__st==CC_FUTURE_PENDING)return CC_FUTURE_PENDING;cc_task_intptr_free(&__f->__t%d);%s=(intptr_t)__v;__f->__st=%d;/*fall*/}case %d:{",
                                                      state + 1, task_idx, task_idx, lhs2, state + 2, state + 2);
                                    state += 2;
                                    task_idx++;
                                }
                                free(lhs2);
                                free(ex);
                                continue;
                            }
                        }
                    }
                    if (memcmp(p, "await", 5) == 0 && task_idx < 16) {
                        p += 5;
                        while (*p == ' ' || *p == '\t') p++;
                        char* ex = cc__rewrite_idents(p, map_names, map_repls, map_n);
                        if (!ex) continue;
                        cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "__f->__t%d=(%s);__f->__st=%d;return CC_FUTURE_PENDING;",
                                          task_idx, ex, state + 1);
                        free(ex);
                        cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                                          "}case %d:{intptr_t __v=0;int __err=0;CCFutureStatus __st=cc_task_intptr_poll(&__f->__t%d,&__v,&__err);if(__st==CC_FUTURE_PENDING)return CC_FUTURE_PENDING;cc_task_intptr_free(&__f->__t%d);(void)__v;__f->__st=%d;/*fall*/}case %d:{",
                                          state + 1, task_idx, task_idx, state + 2, state + 2);
                        state += 2;
                        task_idx++;
                        continue;
                    }
                    {
                        char* st2 = cc__rewrite_idents(p, map_names, map_repls, map_n);
                        if (st2) {
                            cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "%s;\n", st2);
                            free(st2);
                        }
                    }
                }
                for (int ti = 0; ti < tn; ti++) { free(then_st[ti].text); free(then_st[ti].cond); free(then_st[ti].then_body); free(then_st[ti].else_body); }

                /* Then branch tail: jump to after (patched later). */
                size_t after_patch_off1 = 0;
                if (!then_closed) {
                    cc__sb_append_cstr(&repl, &repl_len, &repl_cap, "__f->__st=");
                    after_patch_off1 = cc__sb_append_fixup6(&repl, &repl_len, &repl_cap);
                    cc__sb_append_cstr(&repl, &repl_len, &repl_cap, ";return CC_FUTURE_PENDING;}");
                }

                /* Else branch begins at the next free state. Patch else target now. */
                else_state = state + 1;
                {
                    cc__sb_patch_fixup6(repl, repl_len, else_patch_off, else_state);
                }

                cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "case %d:{\n", else_state);
                state = else_state;

                /* Else branch statements */
                CCAsyncStmt else_st[256];
                int en = 0, etrail = 0;
                memset(else_st, 0, sizeof(else_st));
                (void)cc__split_async_top_level_stmts(stmts[si].else_body, strlen(stmts[si].else_body), else_st, 256, &en, &etrail);
                int else_closed = 0;
                for (int ei = 0; ei < en && !finished; ei++) {
                    const char* p = cc__skip_ws_and_comments(else_st[ei].text);
                    if (memcmp(p, "return", 6) == 0) {
                        const char* rp = p + 6;
                        while (*rp == ' ' || *rp == '\t') rp++;
                        if (fn->ret_is_void && *rp == 0) {
                            cc__sb_append_cstr(&repl, &repl_len, &repl_cap, "__f->__r=0;__f->__st=999;return CC_FUTURE_PENDING;}\n");
                            else_closed = 1;
                            break;
                        }
                        if (memcmp(rp, "await", 5) == 0 && task_idx < 16) {
                            rp += 5;
                            while (*rp == ' ' || *rp == '\t') rp++;
                            char* ex = cc__rewrite_idents(rp, map_names, map_repls, map_n);
                            if (ex) {
                                cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "__f->__t%d=(%s);__f->__st=%d;return CC_FUTURE_PENDING;}\n",
                                                  task_idx, ex, state + 1);
                                free(ex);
                                cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                                                  "case %d:{intptr_t __v=0;int __err=0;CCFutureStatus __st=cc_task_intptr_poll(&__f->__t%d,&__v,&__err);if(__st==CC_FUTURE_PENDING)return CC_FUTURE_PENDING;cc_task_intptr_free(&__f->__t%d);__f->__r=__v;__f->__st=999;return CC_FUTURE_PENDING;}\n",
                                                  state + 1, task_idx, task_idx);
                                state += 1;
                                task_idx++;
                                else_closed = 1;
                                break;
                            }
                        }
                        {
                            char* ex = cc__rewrite_idents(rp, map_names, map_repls, map_n);
                            if (ex) {
                                cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "__f->__r=(intptr_t)(%s);__f->__st=999;return CC_FUTURE_PENDING;}\n", ex);
                                free(ex);
                                else_closed = 1;
                                break;
                            }
                        }
                    }
                    const char* eq = strchr(p, '=');
                    if (eq && task_idx < 16) {
                        const char* rhs = eq + 1;
                        while (*rhs == ' ' || *rhs == '\t') rhs++;
                        if (memcmp(rhs, "await", 5) == 0) {
                            rhs += 5;
                            while (*rhs == ' ' || *rhs == '\t') rhs++;
                            const char* lhs_e = eq;
                            while (lhs_e > p && (lhs_e[-1] == ' ' || lhs_e[-1] == '\t')) lhs_e--;
                            const char* lhs_s = lhs_e;
                            while (lhs_s > p && cc__is_ident_char(lhs_s[-1])) lhs_s--;
                            size_t ln = (size_t)(lhs_e - lhs_s);
                            if (ln > 0 && ln < 128) {
                                char lhs[128];
                                memcpy(lhs, lhs_s, ln);
                                lhs[ln] = 0;
                                char* lhs2 = cc__rewrite_idents(lhs, map_names, map_repls, map_n);
                                char* ex = cc__rewrite_idents(rhs, map_names, map_repls, map_n);
                                if (lhs2 && ex) {
                                    cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "__f->__t%d=(%s);__f->__st=%d;return CC_FUTURE_PENDING;",
                                                      task_idx, ex, state + 1);
                                    cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                                                      "}case %d:{intptr_t __v=0;int __err=0;CCFutureStatus __st=cc_task_intptr_poll(&__f->__t%d,&__v,&__err);if(__st==CC_FUTURE_PENDING)return CC_FUTURE_PENDING;cc_task_intptr_free(&__f->__t%d);%s=(intptr_t)__v;__f->__st=%d;/*fall*/}case %d:{",
                                                      state + 1, task_idx, task_idx, lhs2, state + 2, state + 2);
                                    state += 2;
                                    task_idx++;
                                }
                                free(lhs2);
                                free(ex);
                                continue;
                            }
                        }
                    }
                    if (memcmp(p, "await", 5) == 0 && task_idx < 16) {
                        p += 5;
                        while (*p == ' ' || *p == '\t') p++;
                        char* ex = cc__rewrite_idents(p, map_names, map_repls, map_n);
                        if (!ex) continue;
                        cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "__f->__t%d=(%s);__f->__st=%d;return CC_FUTURE_PENDING;",
                                          task_idx, ex, state + 1);
                        free(ex);
                        cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                                          "}case %d:{intptr_t __v=0;int __err=0;CCFutureStatus __st=cc_task_intptr_poll(&__f->__t%d,&__v,&__err);if(__st==CC_FUTURE_PENDING)return CC_FUTURE_PENDING;cc_task_intptr_free(&__f->__t%d);(void)__v;__f->__st=%d;/*fall*/}case %d:{",
                                          state + 1, task_idx, task_idx, state + 2, state + 2);
                        state += 2;
                        task_idx++;
                        continue;
                    }
                    {
                        char* st2 = cc__rewrite_idents(p, map_names, map_repls, map_n);
                        if (st2) {
                            cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "%s;\n", st2);
                            free(st2);
                        }
                    }
                }
                for (int ei = 0; ei < en; ei++) { free(else_st[ei].text); free(else_st[ei].cond); free(else_st[ei].then_body); free(else_st[ei].else_body); }

                /* Else branch tail: jump to after (patched later). */
                size_t after_patch_off2 = 0;
                if (!else_closed) {
                    cc__sb_append_cstr(&repl, &repl_len, &repl_cap, "__f->__st=");
                    after_patch_off2 = cc__sb_append_fixup6(&repl, &repl_len, &repl_cap);
                    cc__sb_append_cstr(&repl, &repl_len, &repl_cap, ";return CC_FUTURE_PENDING;}");
                }

                after_state = state + 1;
                if (!then_closed) cc__sb_patch_fixup6(repl, repl_len, after_patch_off1, after_state);
                if (!else_closed) cc__sb_patch_fixup6(repl, repl_len, after_patch_off2, after_state);

                cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "case %d:{\n", after_state);
                state = after_state;
                continue;
            }

            if (stmts[si].kind != CC_ASYNC_STMT_SEMI) continue;
            const char* st = stmts[si].text;
            if (!st) continue;
            const char* p = cc__skip_ws_and_comments(st);
            if (memcmp(p, "int ", 4) == 0 || memcmp(p, "intptr_t ", 9) == 0) continue;

            if (memcmp(p, "return", 6) == 0) {
                p += 6;
                while (*p == ' ' || *p == '\t') p++;
                if (fn->ret_is_void && *p == 0) {
                    cc__sb_append_cstr(&repl, &repl_len, &repl_cap, "__f->__r=0;__f->__st=999;/*fall*/");
                    finished = 1;
                    break;
                }
                if (memcmp(p, "await", 5) == 0 && task_idx < 16) {
                    p += 5;
                    while (*p == ' ' || *p == '\t') p++;
                    char* ex = cc__rewrite_idents(p, map_names, map_repls, map_n);
                    if (!ex) continue;
                    cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "__f->__t%d=(%s);__f->__st=%d;return CC_FUTURE_PENDING;",
                                      task_idx, ex, state + 1);
                    free(ex);
                    cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                                      "}case %d:{intptr_t __v=0;int __err=0;CCFutureStatus __st=cc_task_intptr_poll(&__f->__t%d,&__v,&__err);if(__st==CC_FUTURE_PENDING)return CC_FUTURE_PENDING;cc_task_intptr_free(&__f->__t%d);(void)__e;__f->__r=__v;__f->__st=999;/*fall*/}case 999:{if(__o)*__o=__f->__r;return CC_FUTURE_READY;}}",
                                      state + 1, task_idx, task_idx);
                    finished = 1;
                    break;
                }
                char* ex = cc__rewrite_idents(p, map_names, map_repls, map_n);
                if (ex) {
                    cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "__f->__r=(intptr_t)(%s);__f->__st=999;/*fall*/}case 999:{if(__o)*__o=__f->__r;return CC_FUTURE_READY;}}", ex);
                    free(ex);
                    finished = 1;
                    break;
                }
            }

            /* x = await expr; */
            const char* eq = strchr(p, '=');
            if (eq && task_idx < 16) {
                const char* rhs = eq + 1;
                while (*rhs == ' ' || *rhs == '\t') rhs++;
                if (memcmp(rhs, "await", 5) == 0) {
                    rhs += 5;
                    while (*rhs == ' ' || *rhs == '\t') rhs++;
                    const char* lhs_e = eq;
                    while (lhs_e > p && (lhs_e[-1] == ' ' || lhs_e[-1] == '\t')) lhs_e--;
                    const char* lhs_s = lhs_e;
                    while (lhs_s > p && cc__is_ident_char(lhs_s[-1])) lhs_s--;
                    size_t ln = (size_t)(lhs_e - lhs_s);
                    if (ln > 0 && ln < 128) {
                        char lhs[128];
                        memcpy(lhs, lhs_s, ln);
                        lhs[ln] = 0;
                        char* lhs2 = cc__rewrite_idents(lhs, map_names, map_repls, map_n);
                        char* ex = cc__rewrite_idents(rhs, map_names, map_repls, map_n);
                        if (lhs2 && ex) {
                            cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "__f->__t%d=(%s);__f->__st=%d;return CC_FUTURE_PENDING;",
                                              task_idx, ex, state + 1);
                            cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                                              "}case %d:{intptr_t __v=0;int __err=0;CCFutureStatus __st=cc_task_intptr_poll(&__f->__t%d,&__v,&__err);if(__st==CC_FUTURE_PENDING)return CC_FUTURE_PENDING;cc_task_intptr_free(&__f->__t%d);%s=(intptr_t)__v;__f->__st=%d;/*fall*/}case %d:{",
                                              state + 1, task_idx, task_idx, lhs2, state + 2, state + 2);
                            state += 2;
                            task_idx++;
                        }
                        free(lhs2);
                        free(ex);
                        continue;
                    }
                }
            }

            /* await expr; */
            if (memcmp(p, "await", 5) == 0 && task_idx < 16) {
                p += 5;
                while (*p == ' ' || *p == '\t') p++;
                char* ex = cc__rewrite_idents(p, map_names, map_repls, map_n);
                if (!ex) continue;
                cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "__f->__t%d=(%s);__f->__st=%d;return CC_FUTURE_PENDING;",
                                  task_idx, ex, state + 1);
                free(ex);
                cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                                  "}case %d:{intptr_t __v=0;int __err=0;CCFutureStatus __st=cc_task_intptr_poll(&__f->__t%d,&__v,&__err);if(__st==CC_FUTURE_PENDING)return CC_FUTURE_PENDING;cc_task_intptr_free(&__f->__t%d);(void)__v;__f->__st=%d;/*fall*/}case %d:{",
                                  state + 1, task_idx, task_idx, state + 2, state + 2);
                state += 2;
                task_idx++;
                continue;
            }

            /* Plain statement. */
            {
                char* st2 = cc__rewrite_idents(p, map_names, map_repls, map_n);
                if (st2) {
                    cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "%s;\n", st2);
                    free(st2);
                }
            }
        }

        if (!finished) {
            /* implicit return */
            cc__sb_append_cstr(&repl, &repl_len, &repl_cap, "__f->__r=0;__f->__st=999;/*fall*/}case 999:{if(__o)*__o=__f->__r;return CC_FUTURE_READY;}}");
        }

        /* After the switch, fall back to ERR (should be unreachable in supported subsets). */
        cc__sb_append_cstr(&repl, &repl_len, &repl_cap, "return CC_FUTURE_ERR;}");

        cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                          "static void __cc_af%d_drop(void*__p){__cc_af%d_f*__f=(__cc_af%d_f*)__p;if(!__f)return;",
                          id, id, id);
        for (int k = 0; k < 16; k++) cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "cc_task_intptr_free(&__f->__t%d);", k);
        cc__sb_append_cstr(&repl, &repl_len, &repl_cap, "free(__f);}");

        /* Wait hook: block on any outstanding future task slot (best-effort) to avoid spin in block_on. */
        cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                          "static int __cc_af%d_wait(void*__p){__cc_af%d_f*__f=(__cc_af%d_f*)__p;if(!__f)return 0;"
                          "for(int __i=0;__i<16;__i++){CCTaskIntptr*__t=NULL;switch(__i){"
                          "case 0:__t=&__f->__t0;break;case 1:__t=&__f->__t1;break;case 2:__t=&__f->__t2;break;case 3:__t=&__f->__t3;break;"
                          "case 4:__t=&__f->__t4;break;case 5:__t=&__f->__t5;break;case 6:__t=&__f->__t6;break;case 7:__t=&__f->__t7;break;"
                          "case 8:__t=&__f->__t8;break;case 9:__t=&__f->__t9;break;case 10:__t=&__f->__t10;break;case 11:__t=&__f->__t11;break;"
                          "case 12:__t=&__f->__t12;break;case 13:__t=&__f->__t13;break;case 14:__t=&__f->__t14;break;case 15:__t=&__f->__t15;break;}"
                          "if(__t && __t->kind==CC_TASK_INTPTR_KIND_FUTURE && __t->future.fut.handle.done){int __err=0;(void)cc_future_wait_peek_err(&__t->future.fut,&__err);return 0;}"
                          "}return 0;}",
                          id, id, id);

        cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                          "CCTaskIntptr %s%s{__cc_af%d_f*__f=(__cc_af%d_f*)calloc(1,sizeof(__cc_af%d_f));if(!__f){CCTaskIntptr __t;memset(&__t,0,sizeof(__t));return __t;}__f->__st=0;",
                          fn->name, fn->params, id, id, id);
        for (int k = 0; k < param_n; k++) {
            cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "__f->__p_%s=(intptr_t)(%s);", params[k], params[k]);
        }
        cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "return cc_task_intptr_make_poll_ex(__cc_af%d_poll,__cc_af%d_wait,__f,__cc_af%d_drop);}", id, id, id);

        /* Pad newlines to keep line counts stable-ish. */
        int repl_nl = 0;
        for (size_t k = 0; k < repl_len; k++) if (repl[k] == '\n') repl_nl++;
        if (repl_nl < fn->orig_nl) {
            int need = fn->orig_nl - repl_nl;
            for (int k = 0; k < need; k++) cc__sb_append_cstr(&repl, &repl_len, &repl_cap, "\n");
        }

        size_t new_len = cur_len - (fn->end - fn->start) + repl_len;
        char* next = (char*)malloc(new_len + 1);
        if (next) {
            memcpy(next, cur, fn->start);
            memcpy(next + fn->start, repl, repl_len);
            memcpy(next + fn->start + repl_len, cur + fn->end, cur_len - fn->end);
            next[new_len] = 0;
            free(cur);
            cur = next;
            cur_len = new_len;
        }

        free(repl);
        for (int si = 0; si < stmt_n; si++) { free(stmts[si].text); free(stmts[si].cond); free(stmts[si].then_body); free(stmts[si].else_body); }
        for (int k = 0; k < param_n; k++) free(params[k]);
        for (int k = 0; k < local_n; k++) free(locals[k]);
        for (int k = 0; k < map_n; k++) free((void*)map_repls[k]);
    }

    *out_src = cur;
    *out_len = cur_len;
    return 1;
}
