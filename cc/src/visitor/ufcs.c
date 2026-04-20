#include "ufcs.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccc/std/prelude.cch>
#include <ccc/cc_channel.cch>
#include <ccc/std/string.cch>

#include "comptime/symbols.h"
#include "preprocess/type_registry.h"
#include "result_spec.h"
#include "util/text.h"

// Thread-local context: set to 1 when rewriting UFCS inside `await`.
// Channel ops should emit task-returning variants when set.
static _Thread_local int g_ufcs_await_context = 0;

// Thread-local context: set to 1 when receiver's resolved type is a pointer.
// Used only by explicit type-driven channel dispatch.
static _Thread_local int g_ufcs_recv_type_is_ptr = 0;

// Thread-local context: receiver type name from TCC (e.g., "Point", "CCVec_int").
// When set, UFCS generates TypeName_method(&recv, ...) for struct types.
static _Thread_local const char* g_ufcs_recv_type = NULL;
static _Thread_local const char* g_ufcs_source_text = NULL;
static _Thread_local size_t g_ufcs_source_offset = 0;
static _Thread_local CCSymbolTable* g_ufcs_symbols = NULL;

#define CC_UFCS_EMIT_UNRESOLVED (-2)
/* C-first dispatch: the method name is an ordinary data member (non-closure)
   of the receiver's type, so the source expression is already a valid C
   function-pointer / field access call.  Leave the source span untouched
   and let TCC type-check the plain C form. */
#define CC_UFCS_EMIT_FIELD_WINS (-3)

void cc_ufcs_set_symbols(CCSymbolTable* symbols) {
    g_ufcs_symbols = symbols;
}

void cc_ufcs_set_source_context(const char* source_text, size_t source_offset) {
    g_ufcs_source_text = source_text;
    g_ufcs_source_offset = source_offset;
}

// Map a receiver+method to a desugared function call prefix.
// Returns number of bytes written to out (not including args), or -1 on failure.
static const char* skip_ws(const char* s) {
    while (s && *s && isspace((unsigned char)*s)) s++;
    return s;
}

static void trim_ws_in_place(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    size_t a = 0;
    while (a < n && isspace((unsigned char)s[a])) a++;
    size_t b = n;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    if (a > 0) memmove(s, s + a, b - a);
    s[b - a] = '\0';
}

static int is_ident_only(const char* s) {
    if (!s || !*s) return 0;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (const char* p = s; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_')) return 0;
    }
    return 1;
}

static int is_addr_of_ident(const char* s) {
    if (!s) return 0;
    s = skip_ws(s);
    if (*s != '&') return 0;
    s++;
    s = skip_ws(s);
    return is_ident_only(s);
}

static void cc__ufcs_trim_type_span(const char** start, const char** end) {
    while (*start < *end && isspace((unsigned char)**start)) (*start)++;
    while (*end > *start && isspace((unsigned char)(*end)[-1])) (*end)--;
}

static const char* cc__ufcs_canonicalize_string_type(const char* type_name) {
    if (!type_name) return NULL;
    if (strcmp(type_name, "CCVec_char") == 0 || strcmp(type_name, "__CCVecGeneric") == 0) {
        return "CCString";
    }
    if (strcmp(type_name, "CCVec_char*") == 0 || strcmp(type_name, "__CCVecGeneric*") == 0) {
        return "CCString*";
    }
    return type_name;
}

static void cc__ufcs_normalize_decl_type(char* out, size_t out_sz, const char* type_name) {
    const char* bang;
    const char* ok_s;
    const char* ok_e;
    const char* err_s;
    const char* err_e;
    char mangled_ok[128];
    char mangled_err[128];
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!type_name || !type_name[0]) return;
    type_name = cc__ufcs_canonicalize_string_type(type_name);
    /* Channel sugar: `<elem> [~<cap> >]` / `<elem> [~<cap> <]` wraps the
     * element type into a CCChanTx_<mangled> / CCChanRx_<mangled> handle.
     * Detect the trailing bracketed span, mangle the element (which may
     * itself carry bang-result sugar), and wrap.  Mirrors the logic in
     * preprocess/type_registry.c so receiver-type inference matches the
     * registered typedef. */
    {
        const char* lb = strchr(type_name, '[');
        if (lb) {
            const char* rb = strchr(lb, ']');
            if (rb && memchr(lb, '~', (size_t)(rb - lb)) != NULL) {
                int is_tx = memchr(lb, '>', (size_t)(rb - lb)) != NULL;
                int is_rx = memchr(lb, '<', (size_t)(rb - lb)) != NULL;
                if (is_tx || is_rx) {
                    const char* elem_s = type_name;
                    const char* elem_e = lb;
                    char elem_buf[256];
                    char elem_norm[256];
                    size_t elem_len;
                    cc__ufcs_trim_type_span(&elem_s, &elem_e);
                    if (elem_e > elem_s) {
                        elem_len = (size_t)(elem_e - elem_s);
                        if (elem_len >= sizeof(elem_buf)) elem_len = sizeof(elem_buf) - 1;
                        memcpy(elem_buf, elem_s, elem_len);
                        elem_buf[elem_len] = '\0';
                        /* Recurse to resolve bang-result sugar on the element. */
                        cc__ufcs_normalize_decl_type(elem_norm, sizeof(elem_norm), elem_buf);
                        if (elem_norm[0]) {
                            char mangled_elem[128];
                            cc_result_spec_mangle_type(elem_norm, strlen(elem_norm),
                                                       mangled_elem, sizeof(mangled_elem));
                            if (mangled_elem[0]) {
                                snprintf(out, out_sz, "%s_%s",
                                         is_tx ? "CCChanTx" : "CCChanRx", mangled_elem);
                                return;
                            }
                        }
                    }
                }
            }
        }
    }
    bang = strchr(type_name, '!');
    if (!bang || bang[1] == '=') {
        strncpy(out, type_name, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    ok_s = type_name;
    ok_e = bang;
    cc__ufcs_trim_type_span(&ok_s, &ok_e);
    err_s = bang + 1;
    while (*err_s == ' ' || *err_s == '\t') err_s++;
    if (*err_s == '>') {
        err_s++;
        while (*err_s == ' ' || *err_s == '\t') err_s++;
        if (*err_s == '(') {
            err_s++;
            err_e = strchr(err_s, ')');
            if (!err_e) err_e = err_s + strlen(err_s);
        } else {
            err_e = err_s + strlen(err_s);
        }
    } else {
        err_e = err_s;
        while (*err_e && cc_is_ident_char(*err_e)) err_e++;
    }
    cc__ufcs_trim_type_span(&err_s, &err_e);
    if (ok_e <= ok_s || err_e <= err_s) {
        strncpy(out, type_name, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    cc_result_spec_mangle_type(ok_s, (size_t)(ok_e - ok_s), mangled_ok, sizeof(mangled_ok));
    cc_result_spec_mangle_type(err_s, (size_t)(err_e - err_s), mangled_err, sizeof(mangled_err));
    if (!mangled_ok[0] || !mangled_err[0]) {
        strncpy(out, type_name, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    snprintf(out, out_sz, "CCResult_%s_%s", mangled_ok, mangled_err);
}

/* cc__ufcs_parse_decl_name_and_type — now delegated to cc_parse_decl_name_and_type in util/text.h */

static int cc__ufcs_parse_decl_name_and_type_fallback(const char* stmt_start,
                                                      const char* stmt_end,
                                                      char* decl_name,
                                                      size_t decl_name_sz,
                                                      char* decl_type,
                                                      size_t decl_type_sz) {
    const char* s = stmt_start;
    const char* e = stmt_end;
    const char* scan_end;
    const char* name_end;
    const char* name_start;
    const char* type_end;
    int par = 0, br = 0, brc = 0, ang = 0;
    if (!stmt_start || !stmt_end || stmt_end <= stmt_start) return 0;
    if (!decl_name || decl_name_sz == 0 || !decl_type || decl_type_sz == 0) return 0;
    decl_name[0] = '\0';
    decl_type[0] = '\0';
    /* Skip leading whitespace and C-style comments so block comments between
     * the previous statement terminator and the type (e.g. a doc comment on
     * `bool !>(CCIoError) fill = ...;`) aren't absorbed into the type name. */
    for (;;) {
        while (s < e && isspace((unsigned char)*s)) s++;
        if (s + 1 < e && s[0] == '/' && s[1] == '/') {
            s += 2;
            while (s < e && *s != '\n') s++;
            continue;
        }
        if (s + 1 < e && s[0] == '/' && s[1] == '*') {
            s += 2;
            while (s + 1 < e && !(s[0] == '*' && s[1] == '/')) s++;
            if (s + 1 < e) s += 2;
            continue;
        }
        break;
    }
    while (e > s && isspace((unsigned char)e[-1])) e--;
    if (e <= s) return 0;
    scan_end = e;
    for (const char* p = s; p < e; ++p) {
        char c = *p;
        if (c == '(') par++;
        else if (c == ')' && par > 0) par--;
        else if (c == '[') br++;
        else if (c == ']' && br > 0) br--;
        else if (c == '{') brc++;
        else if (c == '}' && brc > 0) brc--;
        else if (c == '<') ang++;
        else if (c == '>' && ang > 0) ang--;
        else if (c == '=' && par == 0 && br == 0 && brc == 0 && ang == 0) {
            scan_end = p;
            break;
        }
    }
    while (scan_end > s && isspace((unsigned char)scan_end[-1])) scan_end--;
    name_end = scan_end;
    while (name_end > s && !cc_is_ident_char(name_end[-1])) name_end--;
    name_start = name_end;
    while (name_start > s && cc_is_ident_char(name_start[-1])) name_start--;
    if (name_start == name_end || !cc_is_ident_start(*name_start)) return 0;
    type_end = name_start;
    while (type_end > s && isspace((unsigned char)type_end[-1])) type_end--;
    if (type_end <= s) return 0;
    {
        size_t name_len = (size_t)(name_end - name_start);
        size_t type_len = (size_t)(type_end - s);
        if (name_len >= decl_name_sz) name_len = decl_name_sz - 1;
        if (type_len >= decl_type_sz) type_len = decl_type_sz - 1;
        memcpy(decl_name, name_start, name_len);
        decl_name[name_len] = '\0';
        memcpy(decl_type, s, type_len);
        decl_type[type_len] = '\0';
    }
    return 1;
}

static const char* cc__ufcs_lookup_scoped_local_var_type(const char* src,
                                                         size_t limit,
                                                         const char* recv_expr,
                                                         char* out_type,
                                                         size_t out_type_sz) {
    typedef struct {
        int scope_id;
        char type_name[256];
    } LocalDecl;
    enum { MAX_DECLS = 256, MAX_SCOPES = 256 };
    LocalDecl decls[MAX_DECLS];
    int decl_count = 0;
    int scope_stack[MAX_SCOPES];
    int scope_depth = 1;
    int next_scope_id = 1;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    int paren_depth = 0, bracket_depth = 0;
    size_t stmt_start = 0;
    size_t i = 0;
    const char* name = recv_expr;
    char root[128];
    if (!src || !recv_expr || !out_type || out_type_sz == 0) return NULL;
    out_type[0] = '\0';
    name = skip_ws(name);
    if (*name == '&') {
        name++;
        name = skip_ws(name);
    }
    if (!is_ident_only(name)) return NULL;
    if (strlen(name) >= sizeof(root)) return NULL;
    strcpy(root, name);
    scope_stack[0] = 0;
    while (i < limit) {
        char c = src[i];
        char c2 = (i + 1 < limit) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < limit) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < limit) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        if (c == '(') { paren_depth++; i++; continue; }
        if (c == ')') { if (paren_depth > 0) paren_depth--; i++; continue; }
        if (c == '[') { bracket_depth++; i++; continue; }
        if (c == ']') { if (bracket_depth > 0) bracket_depth--; i++; continue; }
        if (c == '{' && paren_depth == 0 && bracket_depth == 0) {
            if (scope_depth < MAX_SCOPES) scope_stack[scope_depth++] = next_scope_id++;
            stmt_start = i + 1;
            i++;
            continue;
        }
        if (c == '}' && paren_depth == 0 && bracket_depth == 0) {
            int closing_scope = (scope_depth > 1) ? scope_stack[scope_depth - 1] : 0;
            while (decl_count > 0 && decls[decl_count - 1].scope_id == closing_scope) decl_count--;
            if (scope_depth > 1) scope_depth--;
            stmt_start = i + 1;
            i++;
            continue;
        }
        if (c == ';' && paren_depth == 0 && bracket_depth == 0) {
            char decl_name[128];
            char decl_type[256];
            decl_name[0] = '\0';
            decl_type[0] = '\0';
            cc_parse_decl_name_and_type(src + stmt_start, src + i,
                                              decl_name, sizeof(decl_name),
                                              decl_type, sizeof(decl_type));
            if (!decl_name[0] || strcmp(decl_name, root) != 0 || !decl_type[0]) {
                (void)cc__ufcs_parse_decl_name_and_type_fallback(src + stmt_start, src + i,
                                                                 decl_name, sizeof(decl_name),
                                                                 decl_type, sizeof(decl_type));
            }
            if (decl_name[0] &&
                strcmp(decl_name, root) == 0 &&
                strcmp(decl_type, "return") != 0 &&
                strcmp(decl_type, "break") != 0 &&
                strcmp(decl_type, "continue") != 0 &&
                strcmp(decl_type, "goto") != 0 &&
                strcmp(decl_type, "case") != 0 &&
                strcmp(decl_type, "default") != 0 &&
                decl_count < MAX_DECLS) {
                decls[decl_count].scope_id = scope_stack[scope_depth - 1];
                cc__ufcs_normalize_decl_type(decls[decl_count].type_name,
                                             sizeof(decls[decl_count].type_name),
                                             decl_type);
                decl_count++;
            }
            stmt_start = i + 1;
        }
        i++;
    }
    if (decl_count == 0) return NULL;
    strncpy(out_type, decls[decl_count - 1].type_name, out_type_sz - 1);
    out_type[out_type_sz - 1] = '\0';
    {
        const char* canon = cc__ufcs_canonicalize_string_type(out_type);
        if (canon != out_type) {
            strncpy(out_type, canon, out_type_sz - 1);
            out_type[out_type_sz - 1] = '\0';
        }
    }
    return out_type;
}

static int cc__skip_balanced_suffix(const char** pp, char open, char close) {
    const char* p = *pp;
    int depth = 1;
    if (!p || *p != open) return 0;
    p++;
    while (*p && depth > 0) {
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p) {
                if (*p == '\\' && p[1]) { p += 2; continue; }
                if (*p == q) { p++; break; }
                p++;
            }
            continue;
        }
        if (*p == open) depth++;
        else if (*p == close) depth--;
        p++;
    }
    if (depth != 0) return 0;
    *pp = p;
    return 1;
}

static int cc__is_addressable_lvalue_expr(const char* s) {
    const char* p = skip_ws(s);
    if (!p) return 0;
    if (*p == '(' && p[1] == '*') {
        int depth = 1;
        const char* q = p + 1;
        while (*q && depth > 0) {
            if (*q == '(') depth++;
            else if (*q == ')') depth--;
            q++;
        }
        if (depth != 0) return 0;
        p = q;
        p = skip_ws(p);
        if (*p == '\0') return 1;
        if (*p == '.' || (*p == '-' && p[1] == '>')) {
            /* fall through to member chain scanning below */
        } else {
            return 0;
        }
    } else if (cc_is_ident_start(*p)) {
        while (cc_is_ident_char(*p)) p++;
    } else {
        return 0;
    }
    for (;;) {
        p = skip_ws(p);
        if (*p == '.') {
            p = skip_ws(p + 1);
            if (!cc_is_ident_start(*p)) return 0;
            while (cc_is_ident_char(*p)) p++;
            continue;
        }
        if (p[0] == '-' && p[1] == '>') {
            p = skip_ws(p + 2);
            if (!cc_is_ident_start(*p)) return 0;
            while (cc_is_ident_char(*p)) p++;
            continue;
        }
        if (*p == '[') {
            if (!cc__skip_balanced_suffix(&p, '[', ']')) return 0;
            continue;
        }
        break;
    }
    p = skip_ws(p);
    return *p == '\0';
}

static int cc__is_string_recv_type(const char* type_name) {
    return type_name &&
           (strcmp(type_name, "CCString") == 0 ||
            strcmp(type_name, "CCString*") == 0 ||
            strcmp(type_name, "CCVec_char") == 0 ||
            strcmp(type_name, "CCVec_char*") == 0 ||
            strcmp(type_name, "__CCVecGeneric") == 0 ||
            strcmp(type_name, "__CCVecGeneric*") == 0);
}

static int cc__is_slice_recv_type(const char* type_name) {
    return type_name &&
           (strcmp(type_name, "CCSlice") == 0 ||
            strcmp(type_name, "CCSlice*") == 0 ||
            strcmp(type_name, "CCSliceUnique") == 0 ||
            strcmp(type_name, "CCSliceUnique*") == 0);
}

static const char* cc__builtin_to_str_callee(const char* type_name) {
    if (!type_name || !type_name[0]) return NULL;
    if (strcmp(type_name, "char") == 0) return "char_to_str";
    if (strcmp(type_name, "signed char") == 0) return "signed_char_to_str";
    if (strcmp(type_name, "unsigned char") == 0) return "unsigned_char_to_str";
    if (strcmp(type_name, "short") == 0) return "short_to_str";
    if (strcmp(type_name, "unsigned short") == 0) return "unsigned_short_to_str";
    if (strcmp(type_name, "int") == 0) return "int_to_str";
    if (strcmp(type_name, "unsigned") == 0 || strcmp(type_name, "unsigned int") == 0) return "unsigned_to_str";
    if (strcmp(type_name, "long") == 0) return "long_to_str";
    if (strcmp(type_name, "unsigned long") == 0) return "unsigned_long_to_str";
    if (strcmp(type_name, "long long") == 0) return "long_long_to_str";
    if (strcmp(type_name, "unsigned long long") == 0) return "unsigned_long_long_to_str";
    if (strcmp(type_name, "int8_t") == 0) return "int8_t_to_str";
    if (strcmp(type_name, "uint8_t") == 0) return "uint8_t_to_str";
    if (strcmp(type_name, "int16_t") == 0) return "int16_t_to_str";
    if (strcmp(type_name, "uint16_t") == 0) return "uint16_t_to_str";
    if (strcmp(type_name, "int32_t") == 0) return "int32_t_to_str";
    if (strcmp(type_name, "uint32_t") == 0) return "uint32_t_to_str";
    if (strcmp(type_name, "int64_t") == 0) return "int64_t_to_str";
    if (strcmp(type_name, "uint64_t") == 0) return "uint64_t_to_str";
    if (strcmp(type_name, "intptr_t") == 0) return "intptr_t_to_str";
    if (strcmp(type_name, "uintptr_t") == 0) return "uintptr_t_to_str";
    if (strcmp(type_name, "float") == 0) return "float_to_str";
    if (strcmp(type_name, "double") == 0) return "double_to_str";
    if (strcmp(type_name, "_Bool") == 0 || strcmp(type_name, "bool") == 0) return "bool_to_str";
    return NULL;
}

static int cc__is_family_recv_type(const char* type_name) {
    return type_name &&
           (strncmp(type_name, "CCVec_", 6) == 0 ||
            strncmp(type_name, "Map_", 4) == 0 ||
            strncmp(type_name, "__CC_VEC(", 9) == 0 ||
            strncmp(type_name, "__CC_MAP(", 9) == 0 ||
            strncmp(type_name, "CCResult_", 9) == 0 ||
            strncmp(type_name, "CCOptional_", 11) == 0);
}

static const char* cc__canonicalize_parser_family_macro(const char* type_name,
                                                        char* scratch,
                                                        size_t scratch_cap) {
    const char* args = NULL;
    const char* close = NULL;
    const char* tail = NULL;
    int is_map = 0;
    char mangled_a[128];
    char mangled_b[128];
    if (!type_name || !scratch || scratch_cap == 0) return NULL;
    if (strncmp(type_name, "__CC_VEC(", 9) == 0) {
        args = type_name + 9;
        is_map = 0;
    } else if (strncmp(type_name, "__CC_MAP(", 9) == 0) {
        args = type_name + 9;
        is_map = 1;
    } else {
        return NULL;
    }
    close = strrchr(args, ')');
    if (!close || close <= args) return NULL;
    tail = close + 1;
    while (*tail == ' ' || *tail == '\t') tail++;
    if (!is_map) {
        cc_result_spec_mangle_type(args, (size_t)(close - args), mangled_a, sizeof(mangled_a));
        if (!mangled_a[0]) return NULL;
        snprintf(scratch, scratch_cap, "CCVec_%s", mangled_a);
        return scratch;
    }
    {
        const char* comma = NULL;
        int par = 0, br = 0, brc = 0, ang = 0;
        for (const char* p = args; p < close; ++p) {
            if (*p == '(') par++;
            else if (*p == ')' && par > 0) par--;
            else if (*p == '[') br++;
            else if (*p == ']' && br > 0) br--;
            else if (*p == '{') brc++;
            else if (*p == '}' && brc > 0) brc--;
            else if (*p == '<') ang++;
            else if (*p == '>' && ang > 0) ang--;
            else if (*p == ',' && par == 0 && br == 0 && brc == 0 && ang == 0) {
                comma = p;
                break;
            }
        }
        if (!comma) return NULL;
        cc_result_spec_mangle_type(args, (size_t)(comma - args), mangled_a, sizeof(mangled_a));
        cc_result_spec_mangle_type(comma + 1, (size_t)(close - (comma + 1)), mangled_b, sizeof(mangled_b));
        if (!mangled_a[0] || !mangled_b[0]) return NULL;
        snprintf(scratch, scratch_cap, "Map_%s_%s", mangled_a, mangled_b);
        return scratch;
    }
}

static const char* cc__canonicalize_family_recv_type(const char* type_name,
                                                     char* scratch,
                                                     size_t scratch_cap) {
    const char* normalized = NULL;
    char tmp[256];
    if (!type_name || !scratch || scratch_cap == 0) return type_name;
    normalized = cc__canonicalize_parser_family_macro(type_name, tmp, sizeof(tmp));
    if (normalized) type_name = normalized;
    if (!strstr(type_name, "_Bool")) {
        if (normalized) {
            snprintf(scratch, scratch_cap, "%s", type_name);
            return scratch;
        }
        return type_name;
    }

    size_t out = 0;
    for (size_t i = 0; type_name[i] && out + 1 < scratch_cap;) {
        if (strncmp(type_name + i, "_Bool", 5) == 0) {
            if (out + 4 >= scratch_cap) break;
            memcpy(scratch + out, "bool", 4);
            out += 4;
            i += 5;
            continue;
        }
        scratch[out++] = type_name[i++];
    }
    scratch[out] = '\0';
    return scratch;
}

static int cc__is_parser_result_recv_type(const char* type_name) {
    return type_name &&
           (strcmp(type_name, "__CCResultGeneric") == 0 ||
            strcmp(type_name, "__CCResultGeneric*") == 0);
}

static int cc__is_parser_vec_recv_type(const char* type_name) {
    return type_name &&
           (strcmp(type_name, "__CCVecGeneric") == 0 ||
            strcmp(type_name, "__CCVecGeneric*") == 0);
}

static int cc__is_parser_map_recv_type(const char* type_name) {
    return type_name &&
           (strcmp(type_name, "__CCMapGeneric") == 0 ||
            strcmp(type_name, "__CCMapGeneric*") == 0);
}

static int cc__is_channel_tx_recv_type(const char* type_name) {
    return type_name &&
           (strncmp(type_name, "CCChanTx_", 9) == 0 ||
            strcmp(type_name, "CCChanTx") == 0 ||
            strcmp(type_name, "CCChanTx*") == 0);
}

static int cc__is_channel_rx_recv_type(const char* type_name) {
    return type_name &&
           (strncmp(type_name, "CCChanRx_", 9) == 0 ||
            strcmp(type_name, "CCChanRx") == 0 ||
            strcmp(type_name, "CCChanRx*") == 0);
}

static int cc__is_raw_channel_recv_type(const char* type_name) {
    return type_name &&
           (strcmp(type_name, "CCChan") == 0 ||
            strcmp(type_name, "CCChan*") == 0);
}

static int cc__ufcs_rewrite_line_simple(const char* in, char* out, size_t out_cap);
typedef CCSlice (*CCUfcsCompiledCallable)(CCSlice recv_type, CCSlice method, CCSlice mode, CCSliceArray argv, CCSliceArray arg_types, CCArena *arena);

#define CC_UFCS_VALUE_TAG "__cc_ufcs_value__:"

typedef struct {
    const char* recv_type_name;
    char recv_type_base[256];
    char recv_family_type[256];
    const char* typed_chan_type;
    int recv_is_simple;
    int recv_is_addressable;
    int recv_is_ptr;
} CCUFCSDispatchCtx;

static int cc__recv_pass_direct(const CCUFCSDispatchCtx* ctx, bool recv_is_ptr) {
    return recv_is_ptr || !ctx || ctx->recv_is_ptr || !ctx->recv_is_addressable;
}

static int cc__type_name_has_ptr(const char* type_name) {
    return type_name && strchr(type_name, '*') != NULL;
}

static void cc__ufcs_copy_type_base(char* out, size_t out_sz, const char* type_name) {
    size_t len = 0;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!type_name) return;
    len = strlen(type_name);
    while (len > 0 && (type_name[len - 1] == ' ' || type_name[len - 1] == '\t')) len--;
    while (len > 0 && type_name[len - 1] == '*') len--;
    while (len > 0 && (type_name[len - 1] == ' ' || type_name[len - 1] == '\t')) len--;
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, type_name, len);
    out[len] = '\0';
}

static int cc__same_nominal_type(const char* a, const char* b) {
    char abuf[256];
    char bbuf[256];
    size_t alen = 0, blen = 0;
    const char* ap = a;
    const char* bp = b;
    if (!a || !b) return 0;
    if (strncmp(ap, "struct ", 7) == 0) ap += 7;
    else if (strncmp(ap, "union ", 6) == 0) ap += 6;
    if (strncmp(bp, "struct ", 7) == 0) bp += 7;
    else if (strncmp(bp, "union ", 6) == 0) bp += 6;
    while (*ap && isspace((unsigned char)*ap)) ap++;
    while (*bp && isspace((unsigned char)*bp)) bp++;
    while (ap[alen] && ap[alen] != '*') alen++;
    while (bp[blen] && bp[blen] != '*') blen++;
    while (alen > 0 && isspace((unsigned char)ap[alen - 1])) alen--;
    while (blen > 0 && isspace((unsigned char)bp[blen - 1])) blen--;
    if (alen == 0 || blen == 0 || alen >= sizeof(abuf) || blen >= sizeof(bbuf)) return 0;
    memcpy(abuf, ap, alen);
    abuf[alen] = '\0';
    memcpy(bbuf, bp, blen);
    bbuf[blen] = '\0';
    return strcmp(abuf, bbuf) == 0;
}

static void cc__trim_slice_bounds(const char* src, size_t* start, size_t* end) {
    if (!src || !start || !end || *end < *start) return;
    while (*start < *end && isspace((unsigned char)src[*start])) (*start)++;
    while (*end > *start && isspace((unsigned char)src[*end - 1])) (*end)--;
}

static CCSliceArray cc__build_ufcs_arg_slices(CCArena* arena, const char* args_src) {
    CCSliceArray argv = {0};
    size_t count = 0;
    size_t start = 0;
    size_t n = 0;
    int depth_paren = 0, depth_brack = 0, depth_brace = 0;
    int in_str = 0, in_chr = 0;
    CCSlice* items = NULL;
    size_t index = 0;
    if (!arena || !args_src || !args_src[0]) return argv;
    n = strlen(args_src);
    for (size_t i = 0; i <= n; ++i) {
        char c = (i < n) ? args_src[i] : ',';
        char c2 = (i + 1 < n) ? args_src[i + 1] : 0;
        if (in_str) {
            if (c == '\\' && c2) { i++; continue; }
            if (c == '"') in_str = 0;
            continue;
        }
        if (in_chr) {
            if (c == '\\' && c2) { i++; continue; }
            if (c == '\'') in_chr = 0;
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c == '(') { depth_paren++; continue; }
        if (c == ')') { if (depth_paren > 0) depth_paren--; continue; }
        if (c == '[') { depth_brack++; continue; }
        if (c == ']') { if (depth_brack > 0) depth_brack--; continue; }
        if (c == '{') { depth_brace++; continue; }
        if (c == '}') { if (depth_brace > 0) depth_brace--; continue; }
        if (c == ',' && depth_paren == 0 && depth_brack == 0 && depth_brace == 0) {
            size_t item_s = start;
            size_t item_e = i;
            cc__trim_slice_bounds(args_src, &item_s, &item_e);
            if (item_e > item_s) count++;
            start = i + 1;
        }
    }
    if (count == 0) return argv;
    items = (CCSlice*)cc_arena_alloc(arena, count * sizeof(CCSlice), _Alignof(CCSlice));
    if (!items) return argv;
    start = 0;
    depth_paren = depth_brack = depth_brace = 0;
    in_str = in_chr = 0;
    for (size_t i = 0; i <= n; ++i) {
        char c = (i < n) ? args_src[i] : ',';
        char c2 = (i + 1 < n) ? args_src[i + 1] : 0;
        if (in_str) {
            if (c == '\\' && c2) { i++; continue; }
            if (c == '"') in_str = 0;
            continue;
        }
        if (in_chr) {
            if (c == '\\' && c2) { i++; continue; }
            if (c == '\'') in_chr = 0;
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c == '(') { depth_paren++; continue; }
        if (c == ')') { if (depth_paren > 0) depth_paren--; continue; }
        if (c == '[') { depth_brack++; continue; }
        if (c == ']') { if (depth_brack > 0) depth_brack--; continue; }
        if (c == '{') { depth_brace++; continue; }
        if (c == '}') { if (depth_brace > 0) depth_brace--; continue; }
        if (c == ',' && depth_paren == 0 && depth_brack == 0 && depth_brace == 0) {
            size_t item_s = start;
            size_t item_e = i;
            cc__trim_slice_bounds(args_src, &item_s, &item_e);
            if (item_e > item_s) {
                items[index++] = cc_slice_from_buffer((void*)(args_src + item_s), item_e - item_s);
            }
            start = i + 1;
        }
    }
    argv.items = items;
    argv.len = index;
    return argv;
}

static CCSliceArray cc__build_ufcs_arg_type_slices(CCArena* arena, const char* args_src) {
    CCSliceArray argv = cc__build_ufcs_arg_slices(arena, args_src);
    CCSlice* items = NULL;
    CCTypeRegistry* reg = cc_type_registry_get_global();
    if (!arena || !argv.items || argv.len == 0) {
        CCSliceArray empty = {0};
        return empty;
    }
    items = (CCSlice*)cc_arena_alloc(arena, argv.len * sizeof(CCSlice), _Alignof(CCSlice));
    if (!items) {
        CCSliceArray empty = {0};
        return empty;
    }
    for (size_t i = 0; i < argv.len; ++i) {
        const char* type_name = NULL;
        size_t len = 0;
        char* buf = NULL;
        if (reg && argv.items[i].ptr && argv.items[i].len > 0) {
            buf = (char*)cc_arena_alloc(arena, argv.items[i].len + 1, 1);
            if (buf) {
                int recv_is_ptr = 0;
                memcpy(buf, argv.items[i].ptr, argv.items[i].len);
                buf[argv.items[i].len] = '\0';
                if (g_ufcs_source_text) {
                    type_name = cc_type_registry_resolve_receiver_expr_at(
                        reg, buf, g_ufcs_source_text, g_ufcs_source_offset, &recv_is_ptr);
                }
                if (!type_name) {
                    type_name = cc_type_registry_resolve_expr_type(reg, buf);
                }
            }
        }
        len = type_name ? strlen(type_name) : 0;
        items[i] = (type_name && len > 0) ? cc_slice_from_buffer((void*)type_name, len) : cc_slice_empty();
    }
    argv.items = items;
    return argv;
}

static int cc__emit_registered_callable(char* out,
                                        size_t cap,
                                        const char* recv,
                                        const char* method,
                                        int recv_is_addressable,
                                        bool recv_is_ptr,
                                        const char* recv_type_name,
                                        const char* args_rewritten,
                                        bool has_args) {
    const void* fn_ptr = NULL;
    CCSliceArray argv = {0};
    CCSliceArray arg_types = {0};
    CCArena arena = cc_heap_arena(1024);
    CCUfcsCompiledCallable fn;
    CCSlice recv_type_slice;
    CCSlice method_slice;
    CCSlice mode_slice;
    CCSlice lowered;
    char lowered_name[256];
    size_t lowered_len = 0;
    int receiver_by_value = 0;
    /* `fn_ptr` can only come from a user `cc_type_register(".ufcs = ...")`
     * entry (including the @comptime registrations that headers install for
     * CCFile, CCArena, CCChanTx_ and CCChanRx_ families, etc).  If that hook returns an
     * empty slice for this method, the owner has explicitly rejected the
     * call: strict C-first says the hook is authoritative, so propagate
     * CC_UFCS_EMIT_UNRESOLVED instead of silently falling back to a
     * convention-based `Type_method(&recv, ...)` that may not exist. */
    if (!recv_type_name) return -1;
    if (g_ufcs_symbols) {
        if (cc_symbols_lookup_type_ufcs_callable(g_ufcs_symbols, recv_type_name, &fn_ptr) != 0 || !fn_ptr) {
            fn_ptr = NULL;
        }
    }
    if (!fn_ptr) {
        cc_heap_arena_free(&arena);
        return -1;
    }
    fn = (CCUfcsCompiledCallable)fn_ptr;
    if (has_args && args_rewritten && args_rewritten[0]) {
        argv = cc__build_ufcs_arg_slices(&arena, args_rewritten);
        arg_types = cc__build_ufcs_arg_type_slices(&arena, args_rewritten);
    }
    recv_type_slice = cc_slice_from_buffer((void*)recv_type_name, strlen(recv_type_name));
    method_slice = cc_slice_from_buffer((void*)method, strlen(method));
    mode_slice = cc_slice_from_buffer((void*)(g_ufcs_await_context ? "await" : "sync"),
                                      g_ufcs_await_context ? sizeof("await") - 1 : sizeof("sync") - 1);
    lowered = fn(recv_type_slice, method_slice, mode_slice, argv, arg_types, &arena);
    if (!lowered.ptr || lowered.len == 0) {
        cc_heap_arena_free(&arena);
        /* Registered hook said "not mine": strict C-first demands UNRESOLVED
         * so the AST pass raises a hard error. */
        return CC_UFCS_EMIT_UNRESOLVED;
    }
    if (lowered.len > sizeof(CC_UFCS_VALUE_TAG) - 1 &&
        memcmp(lowered.ptr, CC_UFCS_VALUE_TAG, sizeof(CC_UFCS_VALUE_TAG) - 1) == 0) {
        receiver_by_value = 1;
        lowered.ptr = (char*)lowered.ptr + (sizeof(CC_UFCS_VALUE_TAG) - 1);
        lowered.len -= (sizeof(CC_UFCS_VALUE_TAG) - 1);
    }
    lowered_len = lowered.len < sizeof(lowered_name) - 1 ? lowered.len : sizeof(lowered_name) - 1;
    memcpy(lowered_name, lowered.ptr, lowered_len);
    lowered_name[lowered_len] = '\0';
    cc_heap_arena_free(&arena);
    if (getenv("CC_DEBUG_COMPTIME_UFCS")) {
        fprintf(stderr, "CC_DEBUG_COMPTIME_UFCS: receiver type %s lowered %s -> %s\n",
                recv_type_name, method, lowered_name);
    }
    if (has_args) {
        if (receiver_by_value) {
            return snprintf(out, cap, "%s(%s, ", lowered_name, recv);
        }
        if (recv_is_ptr || !recv_is_addressable)
            return snprintf(out, cap, "%s(%s, ", lowered_name, recv);
        return snprintf(out, cap, "%s(&%s, ", lowered_name, recv);
    }
    if (receiver_by_value) return snprintf(out, cap, "%s(%s)", lowered_name, recv);
    if (recv_is_ptr || !recv_is_addressable)
        return snprintf(out, cap, "%s(%s)", lowered_name, recv);
    return snprintf(out, cap, "%s(&%s)", lowered_name, recv);
}

/* Canonicalize parser-family stub macro names like `__CC_VEC(int)` /
 * `__CC_MAP(K,V)` into their concrete instantiations (`CCVec_int`,
 * `Map_K_V`).  Historically the AST UFCS pass emitted `__cc_vec_generic_*`
 * placeholders for these receivers and a separate end-of-pipeline
 * rewriter walked the emitted source to substitute the real type.
 * Running the same mangling here lets dispatch emit the concrete
 * Vec/Map callee directly and retires the placeholder pass. */
static const char* cc__ufcs_canonicalize_family_macro(const char* type_name,
                                                      char* scratch,
                                                      size_t scratch_cap) {
    const char* args = NULL;
    const char* close = NULL;
    int is_map = 0;
    char mangled_a[128];
    char mangled_b[128];
    if (!type_name || !scratch || scratch_cap == 0) return NULL;
    if (strncmp(type_name, "__CC_VEC(", 9) == 0) {
        args = type_name + 9;
    } else if (strncmp(type_name, "__CC_MAP(", 9) == 0) {
        args = type_name + 9;
        is_map = 1;
    } else {
        return NULL;
    }
    close = strrchr(args, ')');
    if (!close || close <= args) return NULL;
    if (!is_map) {
        cc_result_spec_mangle_type(args, (size_t)(close - args), mangled_a, sizeof(mangled_a));
        if (!mangled_a[0]) return NULL;
        snprintf(scratch, scratch_cap, "CCVec_%s", mangled_a);
        return scratch;
    }
    {
        const char* comma = NULL;
        int par = 0, br = 0, brc = 0, ang = 0;
        for (const char* p = args; p < close; ++p) {
            if (*p == '(') par++;
            else if (*p == ')' && par > 0) par--;
            else if (*p == '[') br++;
            else if (*p == ']' && br > 0) br--;
            else if (*p == '{') brc++;
            else if (*p == '}' && brc > 0) brc--;
            else if (*p == '<') ang++;
            else if (*p == '>' && ang > 0) ang--;
            else if (*p == ',' && par == 0 && br == 0 && brc == 0 && ang == 0) {
                comma = p;
                break;
            }
        }
        if (!comma) return NULL;
        cc_result_spec_mangle_type(args, (size_t)(comma - args), mangled_a, sizeof(mangled_a));
        cc_result_spec_mangle_type(comma + 1, (size_t)(close - (comma + 1)), mangled_b, sizeof(mangled_b));
        if (!mangled_a[0] || !mangled_b[0]) return NULL;
        snprintf(scratch, scratch_cap, "Map_%s_%s", mangled_a, mangled_b);
        return scratch;
    }
}

static void cc__resolve_dispatch_ctx(CCUFCSDispatchCtx* ctx, const char* recv) {
    CCTypeRegistry* reg = cc_type_registry_get_global();
    const char* reg_type_name = NULL;
    const char* alias_target = NULL;
    char local_type_buf[256];
    char family_canon_buf[256];
    int reg_recv_is_ptr = 0;
    int recv_is_member_chain = 0;
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->recv_is_simple = is_ident_only(recv) || is_addr_of_ident(recv);
    ctx->recv_is_addressable = ctx->recv_is_simple || cc__is_addressable_lvalue_expr(recv);
    ctx->recv_is_ptr = 0;
    ctx->recv_type_name = g_ufcs_recv_type;
    recv_is_member_chain = recv && (strstr(recv, ".") != NULL || strstr(recv, "->") != NULL);
    if (reg) {
        if (g_ufcs_source_text && (is_ident_only(recv) || is_addr_of_ident(recv))) {
            const char* local_type_name = cc__ufcs_lookup_scoped_local_var_type(
                g_ufcs_source_text, g_ufcs_source_offset, recv, local_type_buf, sizeof(local_type_buf));
            if (local_type_name &&
                (strncmp(local_type_name, "CCResult_", 9) == 0 ||
                 strncmp(local_type_name, "CCOptional_", 11) == 0)) {
                reg_type_name = local_type_name;
            }
        }
        if (!reg_type_name && g_ufcs_source_text) {
            reg_type_name = cc_type_registry_resolve_receiver_expr_at(
                reg, recv, g_ufcs_source_text, g_ufcs_source_offset, &reg_recv_is_ptr);
        } else if (!reg_type_name) {
            reg_type_name = cc_type_registry_resolve_receiver_expr(reg, recv, &reg_recv_is_ptr);
        }
        if ((recv_is_member_chain && reg_type_name && reg_type_name[0]) ||
            !ctx->recv_type_name ||
            (reg_type_name &&
             (strncmp(reg_type_name, "CCChanTx_", 9) == 0 ||
              strncmp(reg_type_name, "CCChanRx_", 9) == 0)) ||
            ((cc__is_parser_vec_recv_type(ctx->recv_type_name) ||
              cc__is_parser_map_recv_type(ctx->recv_type_name) ||
              cc__is_parser_result_recv_type(ctx->recv_type_name) ||
              strcmp(ctx->recv_type_name, "CCChanTx") == 0 ||
              strcmp(ctx->recv_type_name, "CCChanRx") == 0) &&
             reg_type_name && reg_type_name[0])) {
            ctx->recv_type_name = reg_type_name;
        }
        /* If the (possibly-overridden) receiver type is a parser-family
         * macro stub (`__CC_VEC(int)`, `__CC_MAP(K,V)`), canonicalize to
         * the concrete `CCVec_<T>` / `Map_<K>_<V>` name up front so the
         * type-driven dispatch below emits a real callee instead of the
         * `__cc_vec_generic_*` / `__cc_map_generic_*` placeholder that
         * used to be cleaned up by a separate end-of-pipeline rewriter. */
        if (ctx->recv_type_name &&
            (strncmp(ctx->recv_type_name, "__CC_VEC(", 9) == 0 ||
             strncmp(ctx->recv_type_name, "__CC_MAP(", 9) == 0)) {
            const char* canon = cc__ufcs_canonicalize_family_macro(
                ctx->recv_type_name, family_canon_buf, sizeof(family_canon_buf));
            if (canon && canon[0]) ctx->recv_type_name = canon;
        }
        if (reg_type_name && reg_type_name[0]) {
            if (reg_recv_is_ptr &&
                ctx->recv_type_name && ctx->recv_type_name[0] &&
                (strncmp(reg_type_name, "struct ", 7) == 0 ||
                 strncmp(reg_type_name, "union ", 6) == 0) &&
                !cc__type_name_has_ptr(ctx->recv_type_name) &&
                cc__same_nominal_type(reg_type_name, ctx->recv_type_name)) {
                ctx->recv_is_ptr = 0;
            } else {
                ctx->recv_is_ptr = reg_recv_is_ptr;
            }
        }
    }
    if (ctx->recv_type_name &&
        (strncmp(ctx->recv_type_name, "CCChanTx_", 9) == 0 ||
         strncmp(ctx->recv_type_name, "CCChanRx_", 9) == 0)) {
        ctx->typed_chan_type = ctx->recv_type_name;
    } else if (reg_type_name &&
               (strncmp(reg_type_name, "CCChanTx_", 9) == 0 ||
                strncmp(reg_type_name, "CCChanRx_", 9) == 0)) {
        ctx->typed_chan_type = reg_type_name;
    }
    cc__ufcs_copy_type_base(ctx->recv_type_base, sizeof(ctx->recv_type_base), ctx->recv_type_name);
    if (ctx->recv_type_base[0]) {
        alias_target = reg ? cc_type_registry_lookup_alias(reg, ctx->recv_type_base) : NULL;
        if (alias_target && *alias_target) {
            cc__ufcs_copy_type_base(ctx->recv_family_type, sizeof(ctx->recv_family_type), alias_target);
        } else {
            snprintf(ctx->recv_family_type, sizeof(ctx->recv_family_type), "%s", ctx->recv_type_base);
        }
    } else if (ctx->recv_type_name && ctx->recv_type_name[0]) {
        cc__ufcs_copy_type_base(ctx->recv_family_type, sizeof(ctx->recv_family_type), ctx->recv_type_name);
    }
}

static int cc__emit_type_driven_dispatch(char* out,
                                         size_t cap,
                                         const char* recv,
                                         const char* method,
                                         bool recv_is_ptr,
                                         const char* args_rewritten,
                                         bool has_args,
                                         const CCUFCSDispatchCtx* ctx) {
    const char* family_recv_type = (ctx && ctx->recv_family_type[0]) ? ctx->recv_family_type : (ctx ? ctx->recv_type_name : NULL);
    const char* surface_recv_type = (ctx && ctx->recv_type_base[0]) ? ctx->recv_type_base : (ctx ? ctx->recv_type_name : NULL);
    CCUfcsChannelKind channel_kind = CC_UFCS_CHANNEL_KIND_NONE;
    int channel_recv_by_value = 0;
    const char* channel_callee = NULL;
    if (!ctx) return -1;
    if (g_ufcs_symbols && ctx->recv_type_name) {
        const char* registered_value = NULL;
        if (cc_symbols_lookup_type_ufcs_value(g_ufcs_symbols, ctx->recv_type_name, method, &registered_value) == 0 &&
            registered_value && registered_value[0]) {
            if (has_args) {
                if (cc__recv_pass_direct(ctx, recv_is_ptr))
                    return snprintf(out, cap, "%s(%s, ", registered_value, recv);
                return snprintf(out, cap, "%s(&%s, ", registered_value, recv);
            }
            if (cc__recv_pass_direct(ctx, recv_is_ptr))
                return snprintf(out, cap, "%s(%s)", registered_value, recv);
            return snprintf(out, cap, "%s(&%s)", registered_value, recv);
        }
    }
    if (ctx->recv_type_name) {
        int callable_len = cc__emit_registered_callable(out, cap, recv, method, ctx->recv_is_addressable,
                                                        recv_is_ptr, ctx->recv_type_name, args_rewritten, has_args);
        if (callable_len >= 0) return callable_len;
        /* Registered @comptime UFCS hook explicitly rejected this method.
         * The owner is authoritative: short-circuit the rest of the
         * dispatch chain so we don't silently synthesize a default
         * `Type_method(&recv, ...)` that the host C compiler would then
         * reject with a confusing "undeclared function" error. */
        if (callable_len == CC_UFCS_EMIT_UNRESOLVED) return CC_UFCS_EMIT_UNRESOLVED;
    }
    channel_callee = cc_ufcs_channel_callee(ctx->recv_type_name, method, g_ufcs_await_context,
                                            &channel_kind, &channel_recv_by_value);
    if (channel_callee &&
        (channel_kind == CC_UFCS_CHANNEL_KIND_RAW ||
         ((channel_kind == CC_UFCS_CHANNEL_KIND_TX || channel_kind == CC_UFCS_CHANNEL_KIND_RX) &&
          (!ctx->typed_chan_type || !ctx->typed_chan_type[0])))) {
        if (has_args && args_rewritten &&
            (strcmp(method, "recv") == 0 || strcmp(method, "try_recv") == 0)) {
            const char* raw_fn = (strcmp(method, "recv") == 0)
                ? (g_ufcs_await_context ? "cc_channel_raw_recv_task" : "cc_channel_raw_recv")
                : "cc_channel_raw_try_recv";
            if (g_ufcs_await_context) {
                return snprintf(out, cap, "%s(%s, %s, sizeof(*%s))",
                                raw_fn, recv, args_rewritten, args_rewritten);
            }
            return snprintf(out, cap, "cc_chan_result_from_errno(%s(%s, %s, sizeof(*%s)))",
                            raw_fn, recv, args_rewritten, args_rewritten);
        }
        if (has_args) {
            if (channel_recv_by_value || cc__recv_pass_direct(ctx, recv_is_ptr))
                return snprintf(out, cap, "%s(%s, ", channel_callee, recv);
            return snprintf(out, cap, "%s(&%s, ", channel_callee, recv);
        }
        if (channel_recv_by_value || cc__recv_pass_direct(ctx, recv_is_ptr))
            return snprintf(out, cap, "%s(%s)", channel_callee, recv);
        return snprintf(out, cap, "%s(&%s)", channel_callee, recv);
    }
    if (ctx->typed_chan_type && ctx->typed_chan_type[0]) {
        if (strcmp(method, "send") == 0 || strcmp(method, "try_send") == 0 ||
            strcmp(method, "send_task") == 0 || strcmp(method, "send_task_hybrid") == 0 ||
            strcmp(method, "recv") == 0 ||
            strcmp(method, "try_recv") == 0 || strcmp(method, "close") == 0 ||
            strcmp(method, "cancel") == 0 ||
            strcmp(method, "free") == 0) {
            if (has_args) {
                return snprintf(out, cap, "%s_%s(%s, ", ctx->typed_chan_type, method, recv);
            }
            return snprintf(out, cap, "%s_%s(%s)", ctx->typed_chan_type, method, recv);
        }
    }
    if (cc__is_parser_vec_recv_type(family_recv_type)) {
        if (strcmp(method, "push") == 0 || strcmp(method, "get") == 0 ||
            strcmp(method, "get_ptr") == 0 || strcmp(method, "pop") == 0 ||
            strcmp(method, "at_grow") == 0 || strcmp(method, "push_ptr") == 0 ||
            strcmp(method, "reserve") == 0 || strcmp(method, "clear") == 0 ||
            strcmp(method, "len") == 0 || strcmp(method, "cap") == 0 ||
            strcmp(method, "begin") == 0 || strcmp(method, "end") == 0 ||
            strcmp(method, "data") == 0) {
            if (has_args) {
                if (cc__recv_pass_direct(ctx, recv_is_ptr))
                    return snprintf(out, cap, "__cc_vec_generic_%s(%s, ", method, recv);
                return snprintf(out, cap, "__cc_vec_generic_%s(&%s, ", method, recv);
            }
            if (cc__recv_pass_direct(ctx, recv_is_ptr))
                return snprintf(out, cap, "__cc_vec_generic_%s(%s)", method, recv);
            return snprintf(out, cap, "__cc_vec_generic_%s(&%s)", method, recv);
        }
    }
    if (cc__is_parser_map_recv_type(family_recv_type)) {
        if (strcmp(method, "insert") == 0 || strcmp(method, "put") == 0 ||
            strcmp(method, "get") == 0 || strcmp(method, "get_ptr") == 0 ||
            strcmp(method, "remove") == 0 || strcmp(method, "del") == 0 ||
            strcmp(method, "clear") == 0 || strcmp(method, "destroy") == 0 ||
            strcmp(method, "len") == 0) {
            if (has_args) {
                if (cc__recv_pass_direct(ctx, recv_is_ptr))
                    return snprintf(out, cap, "__cc_map_generic_%s(%s, ", method, recv);
                return snprintf(out, cap, "__cc_map_generic_%s(&%s, ", method, recv);
            }
            if (cc__recv_pass_direct(ctx, recv_is_ptr))
                return snprintf(out, cap, "__cc_map_generic_%s(%s)", method, recv);
            return snprintf(out, cap, "__cc_map_generic_%s(&%s)", method, recv);
        }
    }
    if (cc__is_parser_result_recv_type(family_recv_type)) {
        if (!has_args && (strcmp(method, "value") == 0 || strcmp(method, "unwrap") == 0)) {
            return snprintf(out, cap, "cc_value(%s)", recv);
        }
        if (!has_args && (strcmp(method, "error") == 0 || strcmp(method, "unwrap_err") == 0)) {
            return snprintf(out, cap, "cc_error(%s)", recv);
        }
        if (!has_args && strcmp(method, "is_ok") == 0) {
            return snprintf(out, cap, "cc_is_ok(%s)", recv);
        }
        if (!has_args && strcmp(method, "is_err") == 0) {
            return snprintf(out, cap, "cc_is_err(%s)", recv);
        }
    }
    if (cc__is_family_recv_type(family_recv_type)) {
        char family_type_buf[256];
        const char* family_type_name =
            cc__canonicalize_family_recv_type(family_recv_type, family_type_buf, sizeof(family_type_buf));
        int by_value = (strncmp(family_type_name, "CCResult_", 9) == 0 ||
                        strncmp(family_type_name, "CCOptional_", 11) == 0);
        const char* family_method = method;
        if (strncmp(family_type_name, "CCResult_", 9) == 0) {
            if (!has_args && strcmp(method, "value") == 0) {
                return snprintf(out, cap, "%s_unwrap(%s)", family_type_name, recv);
            }
            if (!has_args && strcmp(method, "error") == 0) {
                return snprintf(out, cap, "%s_error(%s)", family_type_name, recv);
            }
            if (!has_args && strcmp(method, "is_ok") == 0) {
                return snprintf(out, cap, "cc_is_ok(%s)", recv);
            }
            if (!has_args && strcmp(method, "is_err") == 0) {
                return snprintf(out, cap, "cc_is_err(%s)", recv);
            }
            if (has_args && strcmp(method, "unwrap_or") == 0) {
                return snprintf(out, cap, "%s_unwrap_or(%s, ", family_type_name, recv);
            }
            if (!(strcmp(method, "value") == 0 ||
                  strcmp(method, "error") == 0 ||
                  strcmp(method, "is_ok") == 0 ||
                  strcmp(method, "is_err") == 0 ||
                  strcmp(method, "unwrap_or") == 0)) {
                return CC_UFCS_EMIT_UNRESOLVED;
            }
        }
        if (has_args) {
            if (cc__recv_pass_direct(ctx, recv_is_ptr) || by_value)
                return snprintf(out, cap, "%s_%s(%s, ", family_type_name, family_method, recv);
            return snprintf(out, cap, "%s_%s(&%s, ", family_type_name, family_method, recv);
        }
        if (cc__recv_pass_direct(ctx, recv_is_ptr) || by_value)
            return snprintf(out, cap, "%s_%s(%s)", family_type_name, family_method, recv);
        return snprintf(out, cap, "%s_%s(&%s)", family_type_name, family_method, recv);
    }
    if (surface_recv_type && surface_recv_type[0] &&
        !cc__is_string_recv_type(family_recv_type) &&
        !cc__is_slice_recv_type(family_recv_type) &&
        !cc__is_raw_channel_recv_type(family_recv_type) &&
        strcmp(family_recv_type, "CCChanTx") != 0 &&
        strcmp(family_recv_type, "CCChanTx*") != 0 &&
        strcmp(family_recv_type, "CCChanRx") != 0 &&
        strcmp(family_recv_type, "CCChanRx*") != 0 &&
        strncmp(family_recv_type, "CCChanTx_", 9) != 0 &&
        strncmp(family_recv_type, "CCChanRx_", 9) != 0) {
        /* Convention-based default: emit `<surface_recv_type>_<method>(&recv, ...)`.
         * Validate `surface_recv_type` looks like a real C identifier prefix before
         * using it as part of a callee name — the stub type-resolver can return
         * junk for receivers whose declarations use CC extensions (e.g. the owned
         * channel declaration `CCArena*[~N owned { ... }] arena_pool;` lets
         * `}]` or `]` leak through as the "surface type", which would otherwise
         * produce nonsense like `]_free(&arena_pool)`).  Bail out to UNRESOLVED
         * so the caller leaves the original source span untouched, and trust any
         * earlier pass (preprocess text / AST) that already lowered this call. */
        {
            unsigned char c0 = (unsigned char)surface_recv_type[0];
            int first_ok = (c0 == '_' || (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z'));
            int all_ok = first_ok;
            if (first_ok) {
                for (const char* p = surface_recv_type + 1; *p; p++) {
                    unsigned char cc = (unsigned char)*p;
                    int ok = (cc == '_' || cc == '*' ||
                              (cc >= '0' && cc <= '9') ||
                              (cc >= 'A' && cc <= 'Z') ||
                              (cc >= 'a' && cc <= 'z'));
                    if (!ok) { all_ok = 0; break; }
                }
            }
            if (!all_ok) return CC_UFCS_EMIT_UNRESOLVED;
        }
        if (has_args) {
            if (cc__recv_pass_direct(ctx, recv_is_ptr))
                return snprintf(out, cap, "%s_%s(%s, ", surface_recv_type, method, recv);
            return snprintf(out, cap, "%s_%s(&%s, ", surface_recv_type, method, recv);
        }
        if (cc__recv_pass_direct(ctx, recv_is_ptr))
            return snprintf(out, cap, "%s_%s(%s)", surface_recv_type, method, recv);
        return snprintf(out, cap, "%s_%s(&%s)", surface_recv_type, method, recv);
    }
    return -1;
}

/* UFCS-vs-closure-field dispatch.
 *
 * If `method` is actually a CCClosureN field on the receiver's type, emit
 * `cc_closureN_call(recv->method, <args>)` instead of falling through to
 * free-function dispatch (`Type_method(recv, ...)`).  This is what lets
 *
 *     w->destroy();
 *
 * invoke the closure stored in the `destroy` field of `*w`, rather than
 * looking up a non-existent `Widget_destroy` free function.  It is the
 * "field shadows method" rule every OO-ish language with both has to pick.
 *
 * Gating: we only redirect when cc_type_registry_lookup_field returns a
 * field type that is exactly one of CCClosure0/1/2 (pointer-to-closure or
 * other wrappers explicitly don't qualify).  When the method name is not a
 * field of the receiver, or the field is not a closure type, we return
 * CC_UFCS_EMIT_UNRESOLVED so the rest of emit_desugared_call runs normally.
 */
static int cc__emit_closure_field_call(char* out,
                                       size_t cap,
                                       const char* recv,
                                       const char* method,
                                       bool recv_is_ptr,
                                       const char* args_rewritten,
                                       bool has_args,
                                       const CCUFCSDispatchCtx* ctx) {
    if (!ctx) return CC_UFCS_EMIT_UNRESOLVED;
    const char* type_for_lookup = NULL;
    if (ctx->recv_type_base[0]) type_for_lookup = ctx->recv_type_base;
    else if (ctx->recv_type_name) type_for_lookup = ctx->recv_type_name;
    if (!type_for_lookup || !type_for_lookup[0]) return CC_UFCS_EMIT_UNRESOLVED;
    /* Strip leading "struct "/"union " so we match the tag recorded by the
     * preprocess pass (which keys fields on the typedef name / tag alone). */
    if (strncmp(type_for_lookup, "struct ", 7) == 0) type_for_lookup += 7;
    else if (strncmp(type_for_lookup, "union ", 6) == 0) type_for_lookup += 6;
    while (*type_for_lookup == ' ' || *type_for_lookup == '\t') type_for_lookup++;
    if (!*type_for_lookup) return CC_UFCS_EMIT_UNRESOLVED;

    CCTypeRegistry* reg = cc_type_registry_get_global();
    if (!reg) return CC_UFCS_EMIT_UNRESOLVED;
    const char* field_type = cc_type_registry_lookup_field(reg, type_for_lookup, method);
    if (!field_type) return CC_UFCS_EMIT_UNRESOLVED;

    /* Trim leading whitespace on the field type. */
    while (*field_type == ' ' || *field_type == '\t') field_type++;

    int arity;
    const char* fn_name;
    if (strcmp(field_type, "CCClosure0") == 0) {
        arity = 0;
        fn_name = "cc_closure0_call";
    } else if (strcmp(field_type, "CCClosure1") == 0) {
        arity = 1;
        fn_name = "cc_closure1_call";
    } else if (strcmp(field_type, "CCClosure2") == 0) {
        arity = 2;
        fn_name = "cc_closure2_call";
    } else {
        /* C-first dispatch: a data/function-pointer member with this name
         * exists on the receiver type.  Normally the field shadows any
         * free UFCS function of the same name and we signal FIELD_WINS
         * so the caller leaves `recv.method(args)` untouched.
         *
         * Opt-out: if the receiver type has a registered UFCS hook
         * (cc_type_register(".ufcs = ...")), treat that as the user
         * explicitly taking ownership of method dispatch.  This covers
         * the stdlib + user pattern where a struct body carries a
         * CC_PARSER_MODE-only scaffold field like `int (*ping)();` to
         * keep CCC's parser-mode TCC happy — those scaffold fields do
         * not exist in host-C and must not shadow the real UFCS hook. */
        const void* fn_ptr = NULL;
        if (g_ufcs_symbols &&
            cc_symbols_lookup_type_ufcs_callable(g_ufcs_symbols, type_for_lookup, &fn_ptr) == 0 &&
            fn_ptr) {
            return CC_UFCS_EMIT_UNRESOLVED;
        }
        return CC_UFCS_EMIT_FIELD_WINS;
    }

    /* Build the closure-field access expression.  If the user wrote `->`,
     * or type resolution says the receiver is a pointer, emit pointer
     * dereference; otherwise value-access with `.`.
     *
     * Parenthesize the receiver so complex expressions like `get()->destroy`
     * still compose correctly; the existing UFCS path already trusts the
     * rewriter to supply well-formed receivers at this point. */
    char access[512];
    int use_arrow = recv_is_ptr || (ctx && ctx->recv_is_ptr);
    int an = snprintf(access, sizeof(access),
                      use_arrow ? "(%s)->%s" : "(%s).%s", recv, method);
    if (an < 0 || (size_t)an >= sizeof(access)) return -1;

    if (arity == 0) {
        if (has_args) return CC_UFCS_EMIT_UNRESOLVED; /* signature mismatch */
        return snprintf(out, cap, "%s(%s)", fn_name, access);
    }

    const char* args = (has_args && args_rewritten) ? args_rewritten : "";
    if (arity == 1) {
        return snprintf(out, cap, "%s(%s, (intptr_t)(%s))", fn_name, access, args);
    }

    /* arity == 2: split args on the top-level comma (respecting strings and
     * nested ()/[]/{}).  If we can't find a balanced split, punt and let
     * UFCS's default dispatch handle the diagnostic. */
    const char* comma = NULL;
    {
        int par = 0, brk = 0, brc = 0;
        int in_str = 0; char q = 0;
        for (const char* p = args; *p; p++) {
            char ch = *p;
            if (in_str) {
                if (ch == '\\' && p[1]) { p++; continue; }
                if (ch == q) in_str = 0;
                continue;
            }
            if (ch == '"' || ch == '\'') { in_str = 1; q = ch; continue; }
            if (ch == '(') par++;
            else if (ch == ')') { if (par) par--; }
            else if (ch == '[') brk++;
            else if (ch == ']') { if (brk) brk--; }
            else if (ch == '{') brc++;
            else if (ch == '}') { if (brc) brc--; }
            else if (ch == ',' && par == 0 && brk == 0 && brc == 0) { comma = p; break; }
        }
    }
    if (!comma) return CC_UFCS_EMIT_UNRESOLVED;

    size_t a_len = (size_t)(comma - args);
    while (a_len > 0 && isspace((unsigned char)args[a_len - 1])) a_len--;
    const char* b = comma + 1;
    while (*b && isspace((unsigned char)*b)) b++;
    return snprintf(out, cap, "%s(%s, (intptr_t)(%.*s), (intptr_t)(%s))",
                    fn_name, access, (int)a_len, args, b);
}

static int emit_desugared_call(char* out,
                               size_t cap,
                               const char* recv,
                               const char* method,
                               bool recv_is_ptr,
                               const char* args_rewritten,
                               bool has_args) {
    CCUFCSDispatchCtx ctx;
    int dispatch_n;
    if (!out || cap == 0 || !recv || !method) return -1;
    cc__resolve_dispatch_ctx(&ctx, recv);
    /* C-first dispatch: if `method` is a data member of the receiver type,
     * the source `recv.method(args)` is already a valid C call (field
     * shadows any same-named free function).  Closure-typed fields get a
     * dedicated `cc_closureN_call` lowering; all other fields get the
     * FIELD_WINS sentinel so the caller can leave the span untouched.
     * Only actual UNRESOLVED / snprintf error (-1) should fall through to
     * normal free-function dispatch. */
    {
        int n = cc__emit_closure_field_call(out, cap, recv, method, recv_is_ptr,
                                            args_rewritten, has_args, &ctx);
        if (n >= 0) return n;
        if (n == CC_UFCS_EMIT_FIELD_WINS) return CC_UFCS_EMIT_FIELD_WINS;
        /* n == CC_UFCS_EMIT_UNRESOLVED or snprintf error: fall through. */
    }
    if ((strcmp(recv, "std_out") == 0 || strcmp(recv, "cc_std_out") == 0 ||
         strcmp(recv, "std_err") == 0 || strcmp(recv, "cc_std_err") == 0) &&
        strcmp(method, "write") == 0) {
        const char* callee = (strcmp(recv, "std_out") == 0 || strcmp(recv, "cc_std_out") == 0)
                                 ? "cc_std_out_write_auto"
                                 : "cc_std_err_write_auto";
        if (!has_args || !args_rewritten) return snprintf(out, cap, "%s(", callee);
        return snprintf(out, cap, "%s(%s)", callee, args_rewritten);
    }
    if (strcmp(method, "to_str") == 0) {
        const char* callee = cc__builtin_to_str_callee(ctx.recv_type_name);
        if (callee) {
            if (!has_args || !args_rewritten) {
                return recv_is_ptr ? snprintf(out, cap, "%s(*%s)", callee, recv)
                                   : snprintf(out, cap, "%s(%s)", callee, recv);
            }
            return recv_is_ptr ? snprintf(out, cap, "%s(*%s, %s)", callee, recv, args_rewritten)
                               : snprintf(out, cap, "%s(%s, %s)", callee, recv, args_rewritten);
        }
    }
    if (ctx.recv_type_name &&
        (strcmp(ctx.recv_type_name, "CCNursery") == 0 || strcmp(ctx.recv_type_name, "CCNursery*") == 0) &&
        strcmp(method, "close_on") == 0) {
        if (!has_args || !args_rewritten) return snprintf(out, cap, "cc_nursery_add_closing_tx(");
        if (recv_is_ptr || !ctx.recv_is_addressable) {
            return snprintf(out, cap, "cc_nursery_add_closing_tx(%s, %s)", recv, args_rewritten);
        }
        return snprintf(out, cap, "cc_nursery_add_closing_tx(&%s, %s)", recv, args_rewritten);
    }
    /* Special cases for stdlib convenience (String methods).
       
       IMPORTANT: These String-specific handlers run BEFORE the type-qualified
       dispatch below (g_ufcs_recv_type check at ~line 305). This means "push"
       on a String will be handled here, not via Type_push().
       
       Raw UFCS now survives parsing through the patched TCC tolerance path, and
       final emitted C comes from this file's AST-aware lowering. Keep
       String-specific dispatch narrow so it does not steal other families'
       methods. */
    if ((cc__is_string_recv_type(ctx.recv_type_name) || cc__is_string_recv_type(ctx.recv_family_type)) &&
        strcmp(method, "as_slice") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCString_as_slice(%s)", recv)
                           : snprintf(out, cap, "CCString_as_slice(&%s)", recv);
    }
    if ((cc__is_string_recv_type(ctx.recv_type_name) || cc__is_string_recv_type(ctx.recv_family_type)) &&
        (strcmp(method, "append") == 0 || strcmp(method, "push") == 0)) {
        if (!has_args || !args_rewritten) {
            return recv_is_ptr ? snprintf(out, cap, "CCString_push(%s, cc_slice_empty())", recv)
                               : snprintf(out, cap, "CCString_push(&%s, cc_slice_empty())", recv);
        }
        return recv_is_ptr ? snprintf(out, cap, "CCString_push(%s, %s)", recv, args_rewritten)
                           : snprintf(out, cap, "CCString_push(&%s, %s)", recv, args_rewritten);
    }
    if ((cc__is_string_recv_type(ctx.recv_type_name) || cc__is_string_recv_type(ctx.recv_family_type)) &&
        strcmp(method, "push_char") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCString_push_char(%s, ", recv)
                           : snprintf(out, cap, "CCString_push_char(&%s, ", recv);
    }
    if ((cc__is_string_recv_type(ctx.recv_type_name) || cc__is_string_recv_type(ctx.recv_family_type)) &&
        strcmp(method, "push_int") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCString_push_int(%s, ", recv)
                           : snprintf(out, cap, "CCString_push_int(&%s, ", recv);
    }
    if ((cc__is_string_recv_type(ctx.recv_type_name) || cc__is_string_recv_type(ctx.recv_family_type)) &&
        strcmp(method, "push_uint") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCString_push_uint(%s, ", recv)
                           : snprintf(out, cap, "CCString_push_uint(&%s, ", recv);
    }
    if ((cc__is_string_recv_type(ctx.recv_type_name) || cc__is_string_recv_type(ctx.recv_family_type)) &&
        strcmp(method, "push_float") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCString_push_float(%s, ", recv)
                           : snprintf(out, cap, "CCString_push_float(&%s, ", recv);
    }
    if ((cc__is_string_recv_type(ctx.recv_type_name) || cc__is_string_recv_type(ctx.recv_family_type)) &&
        strcmp(method, "clear") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCString_clear(%s)", recv)
                           : snprintf(out, cap, "CCString_clear(&%s)", recv);
    }
    if ((cc__is_string_recv_type(ctx.recv_type_name) || cc__is_string_recv_type(ctx.recv_family_type)) &&
        strcmp(method, "len") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCString_len(%s)", recv)
                           : snprintf(out, cap, "CCString_len(&%s)", recv);
    }
    if ((cc__is_string_recv_type(ctx.recv_type_name) || cc__is_string_recv_type(ctx.recv_family_type)) &&
        strcmp(method, "cap") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCString_cap(%s)", recv)
                           : snprintf(out, cap, "CCString_cap(&%s)", recv);
    }
    if ((cc__is_string_recv_type(ctx.recv_type_name) || cc__is_string_recv_type(ctx.recv_family_type)) &&
        strcmp(method, "cstr") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCString_cstr(%s)", recv)
                           : snprintf(out, cap, "CCString_cstr(&%s)", recv);
    }
    dispatch_n = cc__emit_type_driven_dispatch(out, cap, recv, method, recv_is_ptr, args_rewritten, has_args, &ctx);
    if (dispatch_n >= 0) return dispatch_n;
    /* Registered-hook-rejected (CC_UFCS_EMIT_UNRESOLVED) must not be
     * overridden by the CCSlice convenience fallbacks below: the owner
     * already said no.  Only -1 (no hook / snprintf error) falls through. */
    if (dispatch_n == CC_UFCS_EMIT_UNRESOLVED) return CC_UFCS_EMIT_UNRESOLVED;
    
    /* Slice UFCS methods dispatch to CCSlice_* (pointer-receiver). */
    if (cc__is_slice_recv_type(ctx.recv_type_name) && strcmp(method, "len") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_len(%s)", recv)
                           : snprintf(out, cap, "CCSlice_len(&%s)", recv);
    }
    if (cc__is_slice_recv_type(ctx.recv_type_name) && strcmp(method, "trim") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_trim(%s)", recv)
                           : snprintf(out, cap, "CCSlice_trim(&%s)", recv);
    }
    if (cc__is_slice_recv_type(ctx.recv_type_name) && strcmp(method, "trim_left") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_trim_left(%s)", recv)
                           : snprintf(out, cap, "CCSlice_trim_left(&%s)", recv);
    }
    if (cc__is_slice_recv_type(ctx.recv_type_name) && strcmp(method, "trim_right") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_trim_right(%s)", recv)
                           : snprintf(out, cap, "CCSlice_trim_right(&%s)", recv);
    }
    if (cc__is_slice_recv_type(ctx.recv_type_name) && strcmp(method, "is_empty") == 0) {
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_is_empty(%s)", recv)
                           : snprintf(out, cap, "CCSlice_is_empty(&%s)", recv);
    }
    if (cc__is_slice_recv_type(ctx.recv_type_name) && strcmp(method, "at") == 0) {
        if (!has_args || !args_rewritten) {
            return recv_is_ptr ? snprintf(out, cap, "CCSlice_at(%s, 0)", recv)
                               : snprintf(out, cap, "CCSlice_at(&%s, 0)", recv);
        }
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_at(%s, %s)", recv, args_rewritten)
                           : snprintf(out, cap, "CCSlice_at(&%s, %s)", recv, args_rewritten);
    }
    if (cc__is_slice_recv_type(ctx.recv_type_name) && strcmp(method, "sub") == 0) {
        if (!has_args || !args_rewritten) {
            return recv_is_ptr ? snprintf(out, cap, "CCSlice_sub(%s, 0, 0)", recv)
                               : snprintf(out, cap, "CCSlice_sub(&%s, 0, 0)", recv);
        }
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_sub(%s, %s)", recv, args_rewritten)
                           : snprintf(out, cap, "CCSlice_sub(&%s, %s)", recv, args_rewritten);
    }
    if (cc__is_slice_recv_type(ctx.recv_type_name) && strcmp(method, "starts_with") == 0) {
        if (!has_args || !args_rewritten) {
            return recv_is_ptr ? snprintf(out, cap, "CCSlice_starts_with(%s, (CCSlice){0})", recv)
                               : snprintf(out, cap, "CCSlice_starts_with(&%s, (CCSlice){0})", recv);
        }
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_starts_with(%s, %s)", recv, args_rewritten)
                           : snprintf(out, cap, "CCSlice_starts_with(&%s, %s)", recv, args_rewritten);
    }
    if (cc__is_slice_recv_type(ctx.recv_type_name) && strcmp(method, "ends_with") == 0) {
        if (!has_args || !args_rewritten) {
            return recv_is_ptr ? snprintf(out, cap, "CCSlice_ends_with(%s, (CCSlice){0})", recv)
                               : snprintf(out, cap, "CCSlice_ends_with(&%s, (CCSlice){0})", recv);
        }
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_ends_with(%s, %s)", recv, args_rewritten)
                           : snprintf(out, cap, "CCSlice_ends_with(&%s, %s)", recv, args_rewritten);
    }
    if (cc__is_slice_recv_type(ctx.recv_type_name) && strcmp(method, "eq") == 0) {
        if (!has_args || !args_rewritten) {
            return recv_is_ptr ? snprintf(out, cap, "CCSlice_eq(%s, (CCSlice){0})", recv)
                               : snprintf(out, cap, "CCSlice_eq(&%s, (CCSlice){0})", recv);
        }
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_eq(%s, %s)", recv, args_rewritten)
                           : snprintf(out, cap, "CCSlice_eq(&%s, %s)", recv, args_rewritten);
    }
    if (cc__is_slice_recv_type(ctx.recv_type_name) && strcmp(method, "eq_cstr") == 0) {
        if (!has_args || !args_rewritten) {
            return recv_is_ptr ? snprintf(out, cap, "CCSlice_eq_cstr(%s, \"\")", recv)
                               : snprintf(out, cap, "CCSlice_eq_cstr(&%s, \"\")", recv);
        }
        return recv_is_ptr ? snprintf(out, cap, "CCSlice_eq_cstr(%s, %s)", recv, args_rewritten)
                           : snprintf(out, cap, "CCSlice_eq_cstr(&%s, %s)", recv, args_rewritten);
    }
    return CC_UFCS_EMIT_UNRESOLVED;
}

static int emit_full_call(char* out,
                          size_t cap,
                          const char* recv,
                          const char* method,
                          bool recv_is_ptr,
                          const char* args_rewritten,
                          bool has_args) {
    char tmp[1024];
    int n = emit_desugared_call(tmp, sizeof(tmp), recv, method, recv_is_ptr, args_rewritten, has_args);
    if (n == CC_UFCS_EMIT_UNRESOLVED) return CC_UFCS_EMIT_UNRESOLVED;
    if (n == CC_UFCS_EMIT_FIELD_WINS) return CC_UFCS_EMIT_FIELD_WINS;
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;
    if (n > 0 && tmp[n - 1] == ')') {
        return snprintf(out, cap, "%s", tmp);
    }
    if (has_args && args_rewritten) {
        return snprintf(out, cap, "%s%s)", tmp, args_rewritten);
    }
    return snprintf(out, cap, "%s)", tmp);
}

struct CCUFCSSegment {
    char method[64];
    char* args;
    bool recv_is_ptr;
};

static void cc__free_ufcs_segments(struct CCUFCSSegment* segs, int seg_count) {
    if (!segs) return;
    for (int i = 0; i < seg_count; i++) {
        free(segs[i].args);
        segs[i].args = NULL;
    }
}

static char* cc__rewrite_nested_ufcs_args(const char* args_src) {
    size_t arg_len = 0;
    size_t out_cap = 0;
    char* out = NULL;
    if (!args_src) return NULL;
    arg_len = strlen(args_src);
    out_cap = arg_len * 4 + 256;
    if (out_cap < 512) out_cap = 512;
    out = (char*)malloc(out_cap);
    if (!out) return NULL;
    if (cc_ufcs_rewrite_line(args_src, out, out_cap) != 0) {
        memcpy(out, args_src, arg_len + 1);
    }
    return out;
}

static int cc__parse_ufcs_chain(const char* in,
                                char* recv,
                                size_t recv_cap,
                                struct CCUFCSSegment* segs,
                                int* seg_count) {
    if (!in || !recv || !segs || !seg_count || recv_cap == 0) return 0;
    *seg_count = 0;

    const char* s = in;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return 0;

    int par = 0, br = 0, brc = 0;
    const char* sep = NULL;
    bool sep_is_ptr = false;
    for (const char* p = s; *p; p++) {
        char c = *p;
        if (c == '"' || c == '\'') {
            char q = c;
            p++;
            while (*p) {
                if (*p == '\\' && p[1]) { p++; continue; }
                if (*p == q) break;
                p++;
            }
            continue;
        }
        if (c == '(') { par++; continue; }
        if (c == ')') { if (par > 0) par--; continue; }
        if (c == '[') { br++; continue; }
        if (c == ']') { if (br > 0) br--; continue; }
        if (c == '{') { brc++; continue; }
        if (c == '}') { if (brc > 0) brc--; continue; }
        if (par || br || brc) continue;
        if (c == '.' || (c == '-' && p[1] == '>')) {
            bool cand_is_ptr = (c == '-');
            const char* m = p + (cand_is_ptr ? 2 : 1);
            while (*m && isspace((unsigned char)*m)) m++;
            if (cc_is_ident_char(*m)) {
                const char* me = m;
                while (cc_is_ident_char(*me)) me++;
                const char* after = me;
                while (*after && isspace((unsigned char)*after)) after++;
                if (*after == '(') {
                    sep = p;
                    sep_is_ptr = cand_is_ptr;
                    break;
                }
            }
            if (cand_is_ptr) p++;
            continue;
        }
    }
    if (!sep) return 0;

    const char* r_end = sep;
    while (r_end > s && isspace((unsigned char)r_end[-1])) r_end--;
    size_t recv_len = (size_t)(r_end - s);
    if (recv_len == 0 || recv_len >= recv_cap) return 0;
    memcpy(recv, s, recv_len);
    recv[recv_len] = '\0';
    trim_ws_in_place(recv);

    const char* p = sep + (sep_is_ptr ? 2 : 1);
    for (;;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!cc_is_ident_char(*p)) {
            cc__free_ufcs_segments(segs, *seg_count);
            *seg_count = 0;
            return 0;
        }
        const char* m_start = p;
        while (cc_is_ident_char(*p)) p++;
        size_t m_len = (size_t)(p - m_start);
        if (m_len == 0 || m_len >= sizeof(segs[0].method)) {
            cc__free_ufcs_segments(segs, *seg_count);
            *seg_count = 0;
            return 0;
        }

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '(') {
            cc__free_ufcs_segments(segs, *seg_count);
            *seg_count = 0;
            return 0;
        }

        const char* args_start = p + 1;
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            else if (*p == '"' || *p == '\'') {
                char q = *p++;
                while (*p) {
                    if (*p == '\\' && p[1]) { p += 2; continue; }
                    if (*p == q) { p++; break; }
                    p++;
                }
                continue;
            }
            if (depth > 0) p++;
        }
        if (depth != 0) {
            cc__free_ufcs_segments(segs, *seg_count);
            *seg_count = 0;
            return 0;
        }
        const char* args_end = p;

        if (*seg_count >= 8) {
            cc__free_ufcs_segments(segs, *seg_count);
            *seg_count = 0;
            return 0;
        }
        struct CCUFCSSegment* seg = &segs[(*seg_count)++];
        memcpy(seg->method, m_start, m_len);
        seg->method[m_len] = '\0';
        seg->recv_is_ptr = sep_is_ptr;
        seg->args = NULL;

        size_t args_len = (size_t)(args_end - args_start);
        seg->args = (char*)malloc(args_len + 1);
        if (!seg->args) {
            cc__free_ufcs_segments(segs, *seg_count);
            *seg_count = 0;
            return 0;
        }
        memcpy(seg->args, args_start, args_len);
        seg->args[args_len] = '\0';

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '.' || (*p == '-' && p[1] == '>')) {
            sep_is_ptr = (*p == '-');
            p += sep_is_ptr ? 2 : 1;
            continue;
        }
        break;
    }

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '\0') return 1;
    cc__free_ufcs_segments(segs, *seg_count);
    *seg_count = 0;
    return 0;
}

static int cc__rewrite_ufcs_chain(const char* in, char* out, size_t out_cap) {
    char recv[256];
    struct CCUFCSSegment segs[8] = {0};
    int seg_count = 0;
    if (!cc__parse_ufcs_chain(in, recv, sizeof(recv), segs, &seg_count)) return 1;
    if (seg_count <= 0) return 1;
    const char* recv_expr = recv;
    const int recv_needs_tmp = (!segs[0].recv_is_ptr && !is_ident_only(recv) && !is_addr_of_ident(recv));
    const int needs_temps = (seg_count > 1) || recv_needs_tmp;

    if (!needs_temps) {
        char* rewritten_args = cc__rewrite_nested_ufcs_args(segs[0].args ? segs[0].args : "");
        size_t args_len = rewritten_args ? strlen(rewritten_args) : 0;
        bool has_args = args_len > 0;
        int n = emit_full_call(out, out_cap, recv_expr, segs[0].method, segs[0].recv_is_ptr,
                               rewritten_args ? rewritten_args : "", has_args);
        free(rewritten_args);
        cc__free_ufcs_segments(segs, seg_count);
        if (n == CC_UFCS_EMIT_UNRESOLVED) return CC_UFCS_REWRITE_UNRESOLVED;
        if (n == CC_UFCS_EMIT_FIELD_WINS) return CC_UFCS_REWRITE_NO_MATCH;
        return (n < 0 || (size_t)n >= out_cap) ? CC_UFCS_REWRITE_ERROR : CC_UFCS_REWRITE_OK;
    }

    char* o = out;
    size_t cap = out_cap;
    int n = snprintf(o, cap, "({ ");
    if (n < 0 || (size_t)n >= cap) return -1;
    o += n; cap -= (size_t)n;
    if (recv_needs_tmp) {
        n = snprintf(o, cap, "__typeof__(%s) __cc_ufcs_recv = %s; ", recv, recv);
        if (n < 0 || (size_t)n >= cap) return -1;
        o += n; cap -= (size_t)n;
        recv_expr = "__cc_ufcs_recv";
    }

    for (int i = 0; i < seg_count; i++) {
        char* rewritten_args = cc__rewrite_nested_ufcs_args(segs[i].args ? segs[i].args : "");
        size_t args_len = rewritten_args ? strlen(rewritten_args) : 0;
        bool has_args = args_len > 0;

        char call[1024];
        const char* recv_for_call = recv_expr;
        char tmp_name[32];
        if (i > 0) {
            snprintf(tmp_name, sizeof(tmp_name), "__cc_ufcs_tmp%d", i);
            recv_for_call = tmp_name;
        }
        int cn = emit_full_call(call, sizeof(call), recv_for_call, segs[i].method,
                                segs[i].recv_is_ptr, rewritten_args ? rewritten_args : "", has_args);
        free(rewritten_args);
        if (cn == CC_UFCS_EMIT_UNRESOLVED) {
            cc__free_ufcs_segments(segs, seg_count);
            return CC_UFCS_REWRITE_UNRESOLVED;
        }
        if (cn == CC_UFCS_EMIT_FIELD_WINS) {
            /* In a chain, a mid-segment field-wins hit would require splicing
               the chain around the plain-C member call.  Treat as "leave
               chain alone" for now so the line rewriter falls through to the
               simple path and never half-rewrites a chain. */
            cc__free_ufcs_segments(segs, seg_count);
            return CC_UFCS_REWRITE_NO_MATCH;
        }
        if (cn < 0 || (size_t)cn >= sizeof(call)) {
            cc__free_ufcs_segments(segs, seg_count);
            return -1;
        }

        if (i < seg_count - 1) {
            n = snprintf(o, cap, "__typeof__(%s) __cc_ufcs_tmp%d = %s; ", call, i + 1, call);
        } else {
            n = snprintf(o, cap, "%s; ", call);
        }
        if (n < 0 || (size_t)n >= cap) return -1;
        o += n; cap -= (size_t)n;
    }

    n = snprintf(o, cap, "})");
    if (n < 0 || (size_t)n >= cap) {
        cc__free_ufcs_segments(segs, seg_count);
        return -1;
    }
    o += n; cap -= (size_t)n;
    cc__free_ufcs_segments(segs, seg_count);
    return CC_UFCS_REWRITE_OK;
}

static const char* cc__recv_chain_start(const char* line_start, const char* recv_end) {
    const char* seg_start = recv_end + 1;
    while (seg_start > line_start && cc_is_ident_char(*(seg_start - 1))) seg_start--;
    if (seg_start > recv_end || !cc_is_ident_start(*seg_start)) return NULL;
    for (;;) {
        const char* q = seg_start;
        while (q > line_start && isspace((unsigned char)q[-1])) q--;
        if (q > line_start && q[-1] == '.') {
            q--;
        } else if (q > line_start + 1 && q[-2] == '-' && q[-1] == '>') {
            q -= 2;
        } else {
            break;
        }
        while (q > line_start && isspace((unsigned char)q[-1])) q--;
        if (q > line_start && q[-1] == ')') {
            int depth = 1;
            const char* pp = q - 1;
            while (pp > line_start && depth > 0) {
                pp--;
                if (*pp == ')') depth++;
                else if (*pp == '(') depth--;
            }
            if (depth == 0) {
                seg_start = pp;
                continue;
            }
            break;
        }
        if (q > line_start && q[-1] == ']') {
            int depth = 1;
            const char* pp = q - 1;
            while (pp > line_start && depth > 0) {
                pp--;
                if (*pp == ']') depth++;
                else if (*pp == '[') depth--;
            }
            if (depth == 0) {
                seg_start = pp;
                continue;
            }
            break;
        }
        if (q <= line_start || !cc_is_ident_char(q[-1])) return seg_start;
        seg_start = q;
        while (seg_start > line_start && cc_is_ident_char(*(seg_start - 1))) seg_start--;
        if (!cc_is_ident_start(*seg_start)) return NULL;
    }
    return seg_start;
}

static int cc__ufcs_rewrite_line_simple(const char* in, char* out, size_t out_cap) {
    if (!in || !out || out_cap == 0) return -1;
    const char* p = in;
    char* o = out;
    size_t cap = out_cap;

    while (*p && cap > 1) {
        const char* sep = NULL;
        bool recv_is_ptr = false;
        const char* scan = p;
        while (*scan) {
            bool cand_is_ptr = false;
            if (*scan == '.') {
                sep = scan;
                cand_is_ptr = false;
            } else if (scan[0] == '-' && scan[1] == '>') {
                sep = scan;
                cand_is_ptr = true;
            } else {
                scan++;
                continue;
            }

            const char* m_start = sep + (cand_is_ptr ? 2 : 1);
            while (*m_start && isspace((unsigned char)*m_start)) m_start++;
            if (!cc_is_ident_char(*m_start)) {
                scan = sep + (cand_is_ptr ? 2 : 1);
                sep = NULL;
                continue;
            }
            const char* m_end = m_start;
            while (cc_is_ident_char(*m_end)) m_end++;
            const char* paren = m_end;
            while (*paren && isspace((unsigned char)*paren)) paren++;
            if (*paren == '(') {
                recv_is_ptr = cand_is_ptr;
                break;
            }
            scan = m_end;
            sep = NULL;
        }
        if (!sep) break;
        // Identify receiver
        const char* r_end = sep - 1;
        while (r_end >= p && isspace((unsigned char)*r_end)) r_end--;
        if (r_end < p || !cc_is_ident_char(*r_end)) {
            size_t chunk = (size_t)((sep + (recv_is_ptr ? 2 : 1)) - p);
            if (chunk >= cap) chunk = cap - 1;
            memcpy(o, p, chunk); o += chunk; cap -= chunk;
            p = sep + (recv_is_ptr ? 2 : 1);
            continue;
        }
        const char* r_start = cc__recv_chain_start(p, r_end);
        if (!r_start) {
            size_t chunk = (size_t)((sep + (recv_is_ptr ? 2 : 1)) - p);
            if (chunk >= cap) chunk = cap - 1;
            memcpy(o, p, chunk); o += chunk; cap -= chunk;
            p = sep + (recv_is_ptr ? 2 : 1);
            continue;
        }
        if (r_start > p) {
            const char* pre = r_start - 1;
            while (pre >= p && isspace((unsigned char)*pre)) pre--;
            if (pre >= p && (*pre == '.' || (*pre == '>' && pre > p && *(pre-1) == '-'))) {
                size_t chunk = (size_t)((sep + (recv_is_ptr ? 2 : 1)) - p);
                if (chunk >= cap) chunk = cap - 1;
                memcpy(o, p, chunk); o += chunk; cap -= chunk;
                p = sep + (recv_is_ptr ? 2 : 1);
                continue;
            }
        }
        size_t recv_len = (size_t)(r_end - r_start + 1);

        // Identify method
        const char* m_start = sep + (recv_is_ptr ? 2 : 1);
        while (*m_start && isspace((unsigned char)*m_start)) m_start++;
        if (!cc_is_ident_char(*m_start)) {
            size_t chunk = (size_t)((sep + (recv_is_ptr ? 2 : 1)) - p);
            if (chunk >= cap) chunk = cap - 1;
            memcpy(o, p, chunk); o += chunk; cap -= chunk;
            p = sep + (recv_is_ptr ? 2 : 1);
            continue;
        }
        const char* m_end = m_start;
        while (cc_is_ident_char(*m_end)) m_end++;
        size_t method_len = (size_t)(m_end - m_start);

        // Next non-space after method must be '('
        const char* paren = m_end;
        while (*paren && isspace((unsigned char)*paren)) paren++;
        if (*paren != '(') {
            size_t chunk = (size_t)((sep + (recv_is_ptr ? 2 : 1)) - p);
            if (chunk >= cap) chunk = cap - 1;
            memcpy(o, p, chunk); o += chunk; cap -= chunk;
            p = sep + (recv_is_ptr ? 2 : 1);
            continue;
        }

        // Extract args (balanced parentheses)
        int depth = 1;
        const char* args_start = paren + 1;
        const char* args_end = args_start;
        while (*args_end && depth > 0) {
            if (*args_end == '(') depth++;
            else if (*args_end == ')') depth--;
            else if (*args_end == '"' || *args_end == '\'') {
                char q = *args_end++;
                while (*args_end) {
                    if (*args_end == '\\' && args_end[1]) { args_end += 2; continue; }
                    if (*args_end == q) { args_end++; break; }
                    args_end++;
                }
                continue;
            }
            if (depth > 0) args_end++;
        }
        if (depth != 0) {
            size_t chunk = (size_t)((sep + (recv_is_ptr ? 2 : 1)) - p);
            if (chunk >= cap) chunk = cap - 1;
            memcpy(o, p, chunk); o += chunk; cap -= chunk;
            p = sep + (recv_is_ptr ? 2 : 1);
            continue;
        }
        // Emit prefix before receiver
        size_t prefix_len = (size_t)(r_start - p);
        if (prefix_len >= cap) prefix_len = cap - 1;
        memcpy(o, p, prefix_len); o += prefix_len; cap -= prefix_len;

        // Build receiver/method strings
        char recv[256] = {0};
        char method[64] = {0};
        if (recv_len >= sizeof(recv) || method_len >= sizeof(method)) {
            // Too long; fall back to copying original segment.
            size_t seg_len = (size_t)(args_end - p + 1);
            if (seg_len >= cap) seg_len = cap - 1;
            memcpy(o, p, seg_len); o += seg_len; cap -= seg_len;
            p = args_end + 1;
            continue;
        }
        memcpy(recv, r_start, recv_len); recv[recv_len] = '\0';
        memcpy(method, m_start, method_len); method[method_len] = '\0';

        // Rewrite arguments recursively to catch nested UFCS.
        size_t arg_len = (size_t)(args_end - args_start);
        char* inner = (char*)malloc(arg_len + 1);
        char* rewritten_args = NULL;
        if (!inner) return -1;
        memcpy(inner, args_start, arg_len);
        inner[arg_len] = '\0';
        rewritten_args = cc__rewrite_nested_ufcs_args(inner);
        if (!rewritten_args) {
            rewritten_args = inner;
            inner = NULL;
        }

        size_t args_out_len = strlen(rewritten_args);
        bool has_args = args_out_len > 0;

        // Emit desugared call
        int n = emit_desugared_call(o, cap, recv, method, recv_is_ptr, rewritten_args, has_args);
        if (n == CC_UFCS_EMIT_UNRESOLVED) {
            free(rewritten_args);
            free(inner);
            return CC_UFCS_REWRITE_UNRESOLVED;
        }
        if (n == CC_UFCS_EMIT_FIELD_WINS) {
            free(rewritten_args);
            free(inner);
            return CC_UFCS_REWRITE_NO_MATCH;
        }
        if (n < 0 || (size_t)n >= cap) return -1;
        o += n; cap -= (size_t)n;
        /* If we emitted a full call for std_out/std_err.write overload, we don't
           append args/closing paren. Detect that by checking for a trailing ')'. */
        if (o > out && *(o - 1) == ')') {
            /* already complete */
        } else {
            size_t copy_len = args_out_len;
            if (copy_len >= cap) copy_len = cap - 1;
            memcpy(o, rewritten_args, copy_len); o += copy_len; cap -= copy_len;
            if (cap > 1 && has_args) { *o++ = ')'; cap--; }
        }
        free(rewritten_args);
        free(inner);

        p = args_end + 1;
    }

    // Copy any remaining tail
    while (*p && cap > 1) { *o++ = *p++; cap--; }
    *o = '\0';
    return CC_UFCS_REWRITE_OK;
}

// Rewrite one line, handling nested method calls.
int cc_ufcs_rewrite_line(const char* in, char* out, size_t out_cap) {
    int rc = cc__rewrite_ufcs_chain(in, out, out_cap);
    if (rc != CC_UFCS_REWRITE_NO_MATCH) return rc;
    return cc__ufcs_rewrite_line_simple(in, out, out_cap);
}

// Rewrite UFCS with await context: channel ops emit task-returning variants.
int cc_ufcs_rewrite_line_await(const char* in, char* out, size_t out_cap, int is_await) {
    g_ufcs_await_context = is_await;
    int rc = cc_ufcs_rewrite_line(in, out, out_cap);
    g_ufcs_await_context = 0;
    return rc;
}

// Extended rewrite with type info from TCC.
int cc_ufcs_rewrite_line_ex(const char* in, char* out, size_t out_cap, int is_await, int recv_type_is_ptr) {
    g_ufcs_await_context = is_await;
    g_ufcs_recv_type_is_ptr = recv_type_is_ptr;
    g_ufcs_recv_type = NULL;
    int rc = cc_ufcs_rewrite_line(in, out, out_cap);
    g_ufcs_await_context = 0;
    g_ufcs_recv_type_is_ptr = 0;
    return rc;
}

// Full UFCS rewrite with receiver type from TCC.
int cc_ufcs_rewrite_line_full(const char* in, char* out, size_t out_cap, 
                              int is_await, int recv_type_is_ptr, const char* recv_type) {
    g_ufcs_await_context = is_await;
    g_ufcs_recv_type_is_ptr = recv_type_is_ptr;
    g_ufcs_recv_type = recv_type;
    int rc = cc_ufcs_rewrite_line(in, out, out_cap);
    g_ufcs_await_context = 0;
    g_ufcs_recv_type_is_ptr = 0;
    g_ufcs_recv_type = NULL;
    return rc;
}