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

static int cc__count_nl(const char* s, size_t a, size_t b) {
    int nl = 0;
    for (size_t i = a; i < b; i++) if (s[i] == '\n') nl++;
    return nl;
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

        /* Split body into top-level semicolon statements. */
        char* stmts[256] = {0};
        int stmt_n = 0;
        {
            const char* b = fn->body;
            size_t bl = fn->body_len;
            size_t s = 0;
            int par2 = 0, brk2 = 0, br2 = 0;
            int ins2 = 0; char q2 = 0;
            int in_lc = 0, in_bc = 0;
            for (size_t k = 0; k < bl && stmt_n < 256; k++) {
                char ch = b[k];
                char ch2 = (k + 1 < bl) ? b[k + 1] : 0;
                if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
                if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; k++; } continue; }
                if (ins2) { if (ch == '\\' && k + 1 < bl) { k++; continue; } if (ch == q2) ins2 = 0; continue; }
                if (ch == '/' && ch2 == '/') { in_lc = 1; k++; continue; }
                if (ch == '/' && ch2 == '*') { in_bc = 1; k++; continue; }
                if (ch == '"' || ch == '\'') { ins2 = 1; q2 = ch; continue; }
                if (ch == '(') par2++;
                else if (ch == ')') { if (par2) par2--; }
                else if (ch == '[') brk2++;
                else if (ch == ']') { if (brk2) brk2--; }
                else if (ch == '{') br2++;
                else if (ch == '}') { if (br2) br2--; }
                else if (ch == ';' && par2 == 0 && brk2 == 0 && br2 == 0) {
                    /* Exclude the ';' from the stored statement text. */
                    size_t e = k;
                    while (s < e && cc__is_ws(b[s])) s++;
                    while (e > s && cc__is_ws(b[e - 1])) e--;
                    if (e > s) {
                        size_t ln = e - s;
                        char* st = (char*)malloc(ln + 1);
                        if (st) { memcpy(st, b + s, ln); st[ln] = 0; stmts[stmt_n++] = st; }
                    }
                    s = k + 1;
                }
            }
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
            const char* st = stmts[si];
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

        /* Ensure body only uses supported await shapes; otherwise skip rewriting. */
        int ok = 1;
        for (int si = 0; si < stmt_n; si++) {
            const char* st = stmts[si];
            if (!st) continue;
            if (strstr(st, "await") == NULL) continue;
            const char* p = cc__skip_ws_and_comments(st);
            if (memcmp(p, "await", 5) == 0) continue;
            if (memcmp(p, "return await", 11) == 0) continue;
            if (strstr(p, "= await") != NULL) continue;
            ok = 0;
            break;
        }
        if (!ok) {
            for (int si = 0; si < stmt_n; si++) free(stmts[si]);
            for (int k = 0; k < local_n; k++) free(locals[k]);
            for (int k = 0; k < map_n; k++) free((void*)map_repls[k]);
            continue;
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
            const char* st = stmts[si];
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
            const char* st = stmts[si];
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

        cc__sb_append_fmt(&repl, &repl_len, &repl_cap,
                          "CCTaskIntptr %s%s{__cc_af%d_f*__f=(__cc_af%d_f*)calloc(1,sizeof(__cc_af%d_f));if(!__f){CCTaskIntptr __t;memset(&__t,0,sizeof(__t));return __t;}__f->__st=0;",
                          fn->name, fn->params, id, id, id);
        for (int k = 0; k < param_n; k++) {
            cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "__f->__p_%s=(intptr_t)(%s);", params[k], params[k]);
        }
        cc__sb_append_fmt(&repl, &repl_len, &repl_cap, "return cc_task_intptr_make_poll(__cc_af%d_poll,__f,__cc_af%d_drop);}", id, id);

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
        for (int si = 0; si < stmt_n; si++) free(stmts[si]);
        for (int k = 0; k < param_n; k++) free(params[k]);
        for (int k = 0; k < local_n; k++) free(locals[k]);
        for (int k = 0; k < map_n; k++) free((void*)map_repls[k]);
    }

    *out_src = cur;
    *out_len = cur_len;
    return 1;
}
