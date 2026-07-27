#include "errhandler_lookup.h"

#include <ctype.h>
#include <string.h>

#include "result_spec.h"
#include "util/result_fn_registry.h"
#include "util/text.h"
#include "util/text_scan.h"

void cc_errhandler_stack_init(CCErrHandlerStack* stk) {
    if (!stk) return;
    memset(stk, 0, sizeof(*stk));
}

static void cc__eh_trim(const char* s, size_t* a, size_t* b) {
    while (*a < *b && isspace((unsigned char)s[*a])) (*a)++;
    while (*b > *a && isspace((unsigned char)s[*b - 1])) (*b)--;
}

int cc_errhandler_split_param_decl(const char* decl,
                                   char* type_out, size_t type_sz,
                                   char* name_out, size_t name_sz) {
    size_t L;
    size_t start;
    size_t ta, tb;
    if (type_out && type_sz) type_out[0] = 0;
    if (name_out && name_sz) name_out[0] = 0;
    if (!decl) return 0;
    L = strlen(decl);
    while (L > 0 && isspace((unsigned char)decl[L - 1])) L--;
    start = L;
    while (start > 0 && cc_is_ident_char(decl[start - 1])) start--;
    if (L <= start) return 0;
    if (name_out && name_sz) {
        size_t nl = L - start;
        if (nl >= name_sz) return 0;
        memcpy(name_out, decl + start, nl);
        name_out[nl] = 0;
    }
    ta = 0;
    tb = start;
    cc__eh_trim(decl, &ta, &tb);
    if (tb <= ta) return 0;
    if (type_out && type_sz) {
        size_t tl = tb - ta;
        if (tl >= type_sz) return 0;
        memcpy(type_out, decl + ta, tl);
        type_out[tl] = 0;
    }
    return 1;
}

int cc_errhandler_types_equal(const char* a, const char* b) {
    size_t aa = 0, ab, ba = 0, bb;
    if (!a || !b) return 0;
    ab = strlen(a);
    bb = strlen(b);
    cc__eh_trim(a, &aa, &ab);
    cc__eh_trim(b, &ba, &bb);
    if (ab - aa != bb - ba) return 0;
    return memcmp(a + aa, b + ba, ab - aa) == 0;
}

const CCErrHandlerFrame* cc_errhandler_stack_find(const CCErrHandlerStack* stk,
                                                  const char* err_type) {
    int i;
    if (!stk || !err_type || !err_type[0]) return NULL;
    for (i = stk->n - 1; i >= 0; i--) {
        if (cc_errhandler_types_equal(stk->frames[i].param_type, err_type))
            return &stk->frames[i];
    }
    return NULL;
}

static int cc__eh_push(CCErrHandlerStack* stk, int depth,
                       const char* decl, size_t decl_len,
                       const char* body, size_t body_len,
                       size_t at_pos) {
    CCErrHandlerFrame* fr;
    if (!stk || stk->n >= CC_ERRHANDLER_STK_MAX) return -1;
    fr = &stk->frames[stk->n];
    memset(fr, 0, sizeof(*fr));
    fr->reg_depth = depth;
    fr->body = body;
    fr->body_len = body_len;
    fr->at_pos = at_pos;
    if (decl_len >= sizeof(fr->param_decl))
        decl_len = sizeof(fr->param_decl) - 1;
    if (decl_len) memcpy(fr->param_decl, decl, decl_len);
    fr->param_decl[decl_len] = 0;
    if (!cc_errhandler_split_param_decl(fr->param_decl,
                                        fr->param_type, sizeof(fr->param_type),
                                        fr->param_name, sizeof(fr->param_name))) {
        return -1;
    }
    stk->n++;
    return 0;
}

static void cc__eh_pop_depth(CCErrHandlerStack* stk, int depth) {
    while (stk && stk->n > 0 && stk->frames[stk->n - 1].reg_depth == depth)
        stk->n--;
}

int cc_errhandler_stack_build_at(const char* s, size_t n, size_t pos,
                                 CCErrHandlerStack* out) {
    CCInertScan scan;
    int depth = 0;
    size_t end;
    size_t i = 0;
    if (!out) return -1;
    cc_errhandler_stack_init(out);
    if (!s) return 0;
    end = (pos <= n) ? pos : n;
    cc_inert_scan_init(&scan, NULL);
    while (i < end) {
        char ch;
        if (cc_inert_scan_step(&scan, s, n, &i)) continue;
        ch = s[i];
        if (ch == '@' && i + 11 <= n && memcmp(s + i, "@errhandler", 11) == 0 &&
            (i + 11 >= n || !cc_is_ident_char(s[i + 11])) &&
            (i == 0 || !cc_is_ident_char(s[i - 1]))) {
            size_t at = i;
            size_t j = i + 11;
            size_t rpar = 0;
            size_t rbrace = 0;
            size_t decl_a, decl_b;
            while (j < n && isspace((unsigned char)s[j])) j++;
            if (j >= n || s[j] != '(') {
                i++;
                continue;
            }
            if (!cc_find_matching_paren(s, n, j, &rpar)) {
                i++;
                continue;
            }
            decl_a = j + 1;
            decl_b = rpar;
            cc__eh_trim(s, &decl_a, &decl_b);
            j = rpar + 1;
            while (j < n && isspace((unsigned char)s[j])) j++;
            if (j >= n || s[j] != '{') {
                i++;
                continue;
            }
            if (!cc_find_matching_brace(s, n, j, &rbrace)) {
                i++;
                continue;
            }
            if (cc__eh_push(out, depth, s + decl_a, decl_b - decl_a,
                            s + j + 1, rbrace - (j + 1), at) != 0) {
                return -1;
            }
            i = rbrace + 1;
            continue;
        }
        if (ch == '{') {
            depth++;
            i++;
            continue;
        }
        if (ch == '}') {
            cc__eh_pop_depth(out, depth);
            if (depth > 0) depth--;
            i++;
            continue;
        }
        i++;
    }
    return 0;
}

static int cc__eh_err_type_from_result_name(const char* result_name,
                                            char* out, size_t out_sz) {
    const char* p;
    const char* last_us;
    size_t len;
    const CCStdlibPredeclaredResult* pre;
    if (!result_name || !out || out_sz == 0) return 0;
    out[0] = 0;
    if (strncmp(result_name, "CCResult_", 9) != 0) return 0;
    pre = cc_result_spec_lookup_stdlib_predeclared(result_name);
    if (pre && pre->err_type) {
        len = strlen(pre->err_type);
        if (len + 1 > out_sz) return 0;
        memcpy(out, pre->err_type, len);
        out[len] = 0;
        return 1;
    }
    p = result_name + 9;
    last_us = strrchr(p, '_');
    if (!last_us || last_us[1] == 0) return 0;
    len = strlen(last_us + 1);
    if (len + 1 > out_sz) return 0;
    memcpy(out, last_us + 1, len);
    out[len] = 0;
    return 1;
}

static int cc__eh_extract_plain_callee(const char* s, size_t a, size_t b,
                                       char* out, size_t out_sz) {
    size_t name_a, name_b, k, name_len;
    if (!out || out_sz == 0) return 0;
    out[0] = 0;
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    if (a >= b || s[b - 1] != ')') return 0;
    if (!cc_is_ident_start(s[a])) return 0;
    name_a = a;
    name_b = a;
    while (name_b < b && cc_is_ident_char(s[name_b])) name_b++;
    if (name_b == name_a) return 0;
    k = name_b;
    while (k < b && isspace((unsigned char)s[k])) k++;
    if (k >= b || s[k] != '(') return 0;
    name_len = name_b - name_a;
    if (name_len + 1 > out_sz) return 0;
    memcpy(out, s + name_a, name_len);
    out[name_len] = 0;
    return 1;
}

int cc_errhandler_resolve_call_err_type(const char* s, size_t n,
                                        size_t call_a, size_t call_b,
                                        char* out, size_t out_sz) {
    char callee[128];
    char result_type[256];
    size_t callee_len;
    size_t pos;
    if (!out || out_sz == 0) return 0;
    out[0] = 0;
    if (!s || call_a >= call_b) return 0;
    if (!cc__eh_extract_plain_callee(s, call_a, call_b, callee, sizeof(callee)))
        return 0;
    callee_len = strlen(callee);
    if (cc_result_fn_registry_get_err_type(callee, callee_len, out, out_sz))
        return 1;
    if (cc_result_fn_registry_get_result_type(callee, callee_len,
                                              result_type, sizeof(result_type)) &&
        cc__eh_err_type_from_result_name(result_type, out, out_sz))
        return 1;
    pos = 0;
    while (pos < n) {
        size_t hit = cc_find_ident_top_level(s, pos, n, callee, callee_len);
        size_t after, p, end, len;
        if (hit >= n) break;
        after = hit + callee_len;
        while (after < n && isspace((unsigned char)s[after])) after++;
        if (after < n && s[after] == '(') {
            p = hit;
            while (p > 0 && isspace((unsigned char)s[p - 1])) p--;
            end = p;
            while (p > 0 && cc_is_ident_char(s[p - 1])) p--;
            len = end - p;
            if (len > 9 && memcmp(s + p, "CCResult_", 9) == 0 &&
                len + 1 <= sizeof(result_type)) {
                memcpy(result_type, s + p, len);
                result_type[len] = 0;
                if (cc__eh_err_type_from_result_name(result_type, out, out_sz))
                    return 1;
            }
        }
        pos = hit + callee_len;
    }
    return 0;
}

int cc_errhandler_find_for_call(const char* s, size_t n, size_t pos,
                                size_t call_a, size_t call_b,
                                char* out_decl, size_t out_decl_sz,
                                size_t* out_decl_len,
                                const char** out_body, size_t* out_body_len,
                                size_t* out_decl_pos,
                                char* out_err_type, size_t out_err_type_sz,
                                int* out_have_handlers) {
    CCErrHandlerStack stk;
    const CCErrHandlerFrame* fr;
    char err_type[128];
    if (out_have_handlers) *out_have_handlers = 0;
    if (out_decl_len) *out_decl_len = 0;
    if (out_body) *out_body = NULL;
    if (out_body_len) *out_body_len = 0;
    if (out_decl_pos) *out_decl_pos = 0;
    if (out_err_type && out_err_type_sz) out_err_type[0] = 0;
    if (out_decl && out_decl_sz) out_decl[0] = 0;
    if (cc_errhandler_stack_build_at(s, n, pos, &stk) != 0) return 0;
    if (out_have_handlers) *out_have_handlers = (stk.n > 0);
    err_type[0] = 0;
    if (!cc_errhandler_resolve_call_err_type(s, n, call_a, call_b,
                                             err_type, sizeof(err_type))) {
        /* Pointer / untyped unwrap binders lower as ambient CCError
         * (`__cc_uw_err_at` default arm). Match that for dispatch. */
        memcpy(err_type, "CCError", sizeof("CCError"));
    }
    if (out_err_type && out_err_type_sz) {
        size_t el = strlen(err_type);
        if (el >= out_err_type_sz) el = out_err_type_sz - 1;
        memcpy(out_err_type, err_type, el);
        out_err_type[el] = 0;
    }
    fr = cc_errhandler_stack_find(&stk, err_type);
    if (!fr) return 0;
    if (out_decl && out_decl_sz) {
        size_t dl = strlen(fr->param_decl);
        if (dl >= out_decl_sz) dl = out_decl_sz - 1;
        memcpy(out_decl, fr->param_decl, dl);
        out_decl[dl] = 0;
        if (out_decl_len) *out_decl_len = dl;
    }
    if (out_body) *out_body = fr->body;
    if (out_body_len) *out_body_len = fr->body_len;
    if (out_decl_pos) *out_decl_pos = fr->at_pos;
    return 1;
}
