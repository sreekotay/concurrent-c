#include "pass_create.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccc/cc_slice.cch>
#include <ccc/cc_arena.cch>

#include "preprocess/type_registry.h"
#include "util/text.h"

typedef CCSlice (*CCTypeCreateHandler)(CCSlice type_name, CCSliceArray argv, CCSliceArray arg_types, CCArena* arena);

static void cc__create_offset_to_line_col(const char* src, size_t off, int* out_line, int* out_col) {
    int line = 1;
    int col = 1;
    for (size_t i = 0; src && i < off; i++) {
        if (src[i] == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
    }
    if (out_line) *out_line = line;
    if (out_col) *out_col = col;
}

static void cc__create_error(const char* file, int line, int col, const char* category, const char* message, const char* type_label) {
    fprintf(stderr, "%s:%d:%d: error: %s: %s",
            file ? file : "<input>", line > 0 ? line : 1, col > 0 ? col : 1, category, message);
    if (type_label && type_label[0]) fprintf(stderr, " `%s`", type_label);
    fprintf(stderr, "\n");
}

static int cc__create_match_kw(const char* src, size_t n, size_t pos, const char* kw) {
    size_t klen = strlen(kw);
    if (!src || !kw || pos + klen > n) return 0;
    if (memcmp(src + pos, kw, klen) != 0) return 0;
    if (pos > 0 && cc_is_ident_char(src[pos - 1])) return 0;
    if (pos + klen < n && cc_is_ident_char(src[pos + klen])) return 0;
    return 1;
}

static int cc__create_find_top_level_comma(const char* src, size_t start, size_t end, size_t* out_pos) {
    int par = 0, brk = 0, br = 0;
    int in_str = 0, in_chr = 0, in_lc = 0, in_bc = 0;
    for (size_t i = start; i < end; i++) {
        char c = src[i];
        char c2 = (i + 1 < end) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; } continue; }
        if (in_str) { if (c == '\\' && i + 1 < end) { i++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < end) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c == '(') par++;
        else if (c == ')') { if (par) par--; }
        else if (c == '[') brk++;
        else if (c == ']') { if (brk) brk--; }
        else if (c == '{') br++;
        else if (c == '}') { if (br) br--; }
        else if (c == ',' && par == 0 && brk == 0 && br == 0) {
            if (out_pos) *out_pos = i;
            return 1;
        }
    }
    return 0;
}

static CCArena cc__create_heap_arena(size_t bytes) {
    CCArena arena;
    memset(&arena, 0, sizeof(arena));
    if (bytes == 0) bytes = 1;
    arena.base = (uint8_t*)malloc(bytes);
    arena.capacity = arena.base ? bytes : 0;
    *(size_t*)&arena.offset = 0;
    arena.block_max = 1;
    return arena;
}

static void cc__create_heap_arena_free(CCArena* arena) {
    if (!arena) return;
    free(arena->base);
    arena->base = NULL;
    arena->capacity = 0;
}

static CCSliceArray cc__create_build_arg_slices(CCArena* arena, const char* args_src) {
    CCSliceArray argv = {0};
    size_t count = 0, start = 0, n = 0, index = 0;
    int depth_paren = 0, depth_brack = 0, depth_brace = 0;
    int in_str = 0, in_chr = 0;
    CCSlice* items = NULL;
    if (!arena || !args_src || !args_src[0]) return argv;
    n = strlen(args_src);
    for (size_t i = 0; i <= n; ++i) {
        char c = (i < n) ? args_src[i] : ',';
        char c2 = (i + 1 < n) ? args_src[i + 1] : 0;
        if (in_str) { if (c == '\\' && c2) { i++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && c2) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c == '(') { depth_paren++; continue; }
        if (c == ')') { if (depth_paren > 0) depth_paren--; continue; }
        if (c == '[') { depth_brack++; continue; }
        if (c == ']') { if (depth_brack > 0) depth_brack--; continue; }
        if (c == '{') { depth_brace++; continue; }
        if (c == '}') { if (depth_brace > 0) depth_brace--; continue; }
        if (c == ',' && depth_paren == 0 && depth_brack == 0 && depth_brace == 0) {
            size_t s = start, e = i;
            while (s < e && isspace((unsigned char)args_src[s])) s++;
            while (e > s && isspace((unsigned char)args_src[e - 1])) e--;
            if (e > s) count++;
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
        if (in_str) { if (c == '\\' && c2) { i++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && c2) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c == '(') { depth_paren++; continue; }
        if (c == ')') { if (depth_paren > 0) depth_paren--; continue; }
        if (c == '[') { depth_brack++; continue; }
        if (c == ']') { if (depth_brack > 0) depth_brack--; continue; }
        if (c == '{') { depth_brace++; continue; }
        if (c == '}') { if (depth_brace > 0) depth_brace--; continue; }
        if (c == ',' && depth_paren == 0 && depth_brack == 0 && depth_brace == 0) {
            size_t s = start, e = i;
            while (s < e && isspace((unsigned char)args_src[s])) s++;
            while (e > s && isspace((unsigned char)args_src[e - 1])) e--;
            if (e > s) items[index++] = cc_slice_from_buffer((void*)(args_src + s), e - s);
            start = i + 1;
        }
    }
    argv.items = items;
    argv.len = index;
    return argv;
}

static CCSliceArray cc__create_build_arg_type_slices(CCArena* arena, CCSliceArray argv) {
    CCSliceArray arg_types = {0};
    CCSlice* items = NULL;
    CCTypeRegistry* reg = cc_type_registry_get_global();
    if (!arena || !argv.items || argv.len == 0) return arg_types;
    items = (CCSlice*)cc_arena_alloc(arena, argv.len * sizeof(CCSlice), _Alignof(CCSlice));
    if (!items) return arg_types;
    for (size_t i = 0; i < argv.len; ++i) {
        const char* type_name = NULL;
        char* buf = NULL;
        if (reg && argv.items[i].len > 0) {
            buf = (char*)cc_arena_alloc(arena, argv.items[i].len + 1, 1);
            if (buf) {
                memcpy(buf, argv.items[i].ptr, argv.items[i].len);
                buf[argv.items[i].len] = '\0';
                type_name = cc_type_registry_resolve_expr_type(reg, buf);
            }
        }
        items[i] = (type_name && type_name[0]) ? cc_slice_from_buffer((void*)type_name, strlen(type_name)) : cc_slice_empty();
    }
    arg_types.items = items;
    arg_types.len = argv.len;
    return arg_types;
}

static int cc__create_seed_registered_var_types(CCSymbolTable* symbols, const char* src, size_t n) {
    CCTypeRegistry* reg = cc_type_registry_get_global();
    if (!symbols || !reg || !src) return 0;
    for (size_t ti = 0; ti < cc_symbols_type_count(symbols); ++ti) {
        const char* type_name = cc_symbols_type_name(symbols, ti);
        const void* create_fn = NULL;
        size_t type_len = type_name ? strlen(type_name) : 0;
        int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
        if (!type_name || !type_len || strchr(type_name, '*')) continue;
        if (cc_symbols_lookup_type_create_callable(symbols, type_name, &create_fn) != 0 && create_fn == NULL) {
            const char* create_callee = NULL;
            if (cc_symbols_lookup_type_create_call(symbols, type_name, 1, &create_callee) != 0 &&
                cc_symbols_lookup_type_create_call(symbols, type_name, 2, &create_callee) != 0) {
                continue;
            }
        }
        for (size_t i = 0; i < n; ++i) {
            char c = src[i];
            char c2 = (i + 1 < n) ? src[i + 1] : 0;
            if (in_lc) { if (c == '\n') in_lc = 0; continue; }
            if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; } continue; }
            if (in_str) { if (c == '\\' && c2) { i++; continue; } if (c == '"') in_str = 0; continue; }
            if (in_chr) { if (c == '\\' && c2) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
            if (c == '/' && c2 == '/') { in_lc = 1; i++; continue; }
            if (c == '/' && c2 == '*') { in_bc = 1; i++; continue; }
            if (c == '"') { in_str = 1; continue; }
            if (c == '\'') { in_chr = 1; continue; }
            if (!cc__create_match_kw(src, n, i, type_name)) continue;
            {
                size_t p = cc_skip_ws_and_comments(src, n, i + type_len);
                while (p < n && src[p] == '*') p++;
                p = cc_skip_ws_and_comments(src, n, p);
                if (p < n && cc_is_ident_start(src[p])) {
                    char var_name[128];
                    size_t v = p;
                    size_t vn = 0;
                    while (v < n && cc_is_ident_char(src[v])) {
                        if (vn + 1 < sizeof(var_name)) var_name[vn] = src[v];
                        vn++;
                        v++;
                    }
                    var_name[vn < sizeof(var_name) ? vn : sizeof(var_name) - 1] = '\0';
                    v = cc_skip_ws_and_comments(src, n, v);
                    if (v < n && src[v] != '(') cc_type_registry_add_var(reg, var_name, type_name);
                }
            }
            i += type_len ? (type_len - 1) : 0;
        }
    }
    return 0;
}

char* cc_rewrite_registered_type_create_destroy(const char* src,
                                                size_t n,
                                                const char* input_path,
                                                CCSymbolTable* symbols) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t last_emit = 0;
    int changed = 0;
    if (!src || n == 0 || !symbols) return NULL;
    cc__create_seed_registered_var_types(symbols, src, n);
    for (size_t i = 0; i < n; ) {
        size_t lpar = 0, rpar = 0, p = 0, eq = 0, name_end = 0, stmt_s = 0;
        size_t name_s = 0, name_e = 0, declared_type_len = 0;
        char declared_type[256];
        const char* registered_create_callee = NULL;
        const void* registered_create_callable = NULL;
        const char* registered_pre_destroy_callee = NULL;
        const char* registered_destroy_callee = NULL;
        size_t args_s = 0, args_e = 0, comma = 0;
        int has_second_arg = 0;
        size_t after_create = 0;
        size_t destroy_body_s = 0, destroy_body_e = 0, semi = 0;
        size_t arg0_s = 0, arg0_e = 0, arg1_s = 0, arg1_e = 0;
        enum { CC_CREATE_OWN_NONE = 0, CC_CREATE_OWN_DESTROY = 1, CC_CREATE_OWN_DETACH = 2 } ownership = CC_CREATE_OWN_NONE;
        if (src[i] != '@' || i + 7 > n || memcmp(src + i, "@create", 7) != 0 || (i + 7 < n && cc_is_ident_char(src[i + 7]))) {
            i++;
            continue;
        }
        lpar = cc_skip_ws_and_comments(src, n, i + 7);
        if (lpar >= n || src[lpar] != '(' || !cc_find_matching_paren(src, n, lpar, &rpar)) {
            i++;
            continue;
        }
        p = i;
        while (p > 0 && isspace((unsigned char)src[p - 1])) p--;
        if (p == 0 || src[p - 1] != '=') {
            i++;
            continue;
        }
        eq = p - 1;
        p = eq;
        while (p > 0 && isspace((unsigned char)src[p - 1])) p--;
        name_end = p;
        while (p > 0 && cc_is_ident_char(src[p - 1])) p--;
        if (p >= name_end || !cc_is_ident_start(src[p])) {
            i++;
            continue;
        }
        name_s = p;
        name_e = name_end;
        stmt_s = name_s;
        while (stmt_s > 0 && src[stmt_s - 1] != ';' && src[stmt_s - 1] != '{' && src[stmt_s - 1] != '}') stmt_s--;
        stmt_s = cc_skip_ws_and_comments(src, n, stmt_s);
        declared_type_len = name_s - stmt_s;
        while (declared_type_len > 0 &&
               (src[stmt_s + declared_type_len - 1] == ' ' || src[stmt_s + declared_type_len - 1] == '\t')) {
            declared_type_len--;
        }
        if (declared_type_len == 0) {
            i++;
            continue;
        }
        if (declared_type_len >= sizeof(declared_type)) declared_type_len = sizeof(declared_type) - 1;
        memcpy(declared_type, src + stmt_s, declared_type_len);
        declared_type[declared_type_len] = '\0';
        if (cc_symbols_lookup_type_create_call(symbols, declared_type, 1, &registered_create_callee) != 0 &&
            cc_symbols_lookup_type_create_call(symbols, declared_type, 2, &registered_create_callee) != 0 &&
            cc_symbols_lookup_type_create_callable(symbols, declared_type, &registered_create_callable) != 0) {
            i++;
            continue;
        }
        args_s = lpar + 1;
        args_e = rpar;
        has_second_arg = cc__create_find_top_level_comma(src, args_s, args_e, &comma);
        if (has_second_arg) {
            size_t extra = 0;
            if (cc__create_find_top_level_comma(src, comma + 1, args_e, &extra)) {
                int line = 1, col = 1;
                cc__create_offset_to_line_col(src, i, &line, &col);
                cc__create_error(input_path, line, col, "syntax",
                                 "registered '@create' currently supports at most 2 arguments for", declared_type);
                free(out);
                return (char*)-1;
            }
        }
        if (!registered_create_callable &&
            cc_symbols_lookup_type_create_call(symbols, declared_type, has_second_arg ? 2 : 1, &registered_create_callee) != 0) {
            int line = 1, col = 1;
            cc__create_offset_to_line_col(src, i, &line, &col);
            fprintf(stderr, "%s:%d:%d: error: type: `%s` does not register an @create overload for %d argument%s\n",
                    input_path ? input_path : "<input>", line, col,
                    declared_type, has_second_arg ? 2 : 1, has_second_arg ? "s" : "");
            free(out);
            return (char*)-1;
        }
        after_create = cc_skip_ws_and_comments(src, n, rpar + 1);
        if (after_create + 8 <= n && memcmp(src + after_create, "@destroy", 8) == 0 &&
            (after_create + 8 >= n || !cc_is_ident_char(src[after_create + 8]))) {
            ownership = CC_CREATE_OWN_DESTROY;
        } else if (after_create + 7 <= n && memcmp(src + after_create, "@detach", 7) == 0 &&
                   (after_create + 7 >= n || !cc_is_ident_char(src[after_create + 7]))) {
            ownership = CC_CREATE_OWN_DETACH;
        } else if (cc_symbols_lookup_type_destroy_call(symbols, declared_type, &registered_destroy_callee) == 0) {
            int line = 1, col = 1;
            cc__create_offset_to_line_col(src, i, &line, &col);
            fprintf(stderr, "%s:%d:%d: error: type: `%s` created with '@create' requires explicit ownership: use '@destroy' or '@detach'\n",
                    input_path ? input_path : "<input>", line, col, declared_type);
            free(out);
            return (char*)-1;
        }
        if (ownership == CC_CREATE_OWN_DESTROY) {
            (void)cc_symbols_lookup_type_pre_destroy_call(symbols, declared_type, &registered_pre_destroy_callee);
        }
        if (ownership == CC_CREATE_OWN_DESTROY && !registered_destroy_callee) {
            (void)cc_symbols_lookup_type_destroy_call(symbols, declared_type, &registered_destroy_callee);
        }
        if (ownership == CC_CREATE_OWN_DESTROY) {
            size_t after_destroy = cc_skip_ws_and_comments(src, n, after_create + 8);
            if (after_destroy < n && src[after_destroy] == '{') {
                destroy_body_s = after_destroy;
                if (!cc_find_matching_brace(src, n, destroy_body_s, &destroy_body_e)) {
                    int line = 1, col = 1;
                    cc__create_offset_to_line_col(src, destroy_body_s, &line, &col);
                    cc__create_error(input_path, line, col, "syntax", "malformed '@destroy { ... }' block for", declared_type);
                    free(out);
                    return (char*)-1;
                }
                semi = cc_skip_ws_and_comments(src, n, destroy_body_e + 1);
            } else {
                semi = after_destroy;
            }
            if (semi >= n || src[semi] != ';') {
                int line = 1, col = 1;
                cc__create_offset_to_line_col(src, semi < n ? semi : n, &line, &col);
                cc__create_error(input_path, line, col, "syntax", "expected ';' after '@destroy' declaration for", declared_type);
                free(out);
                return (char*)-1;
            }
        } else {
            size_t after_detach = cc_skip_ws_and_comments(src, n, after_create + 7);
            if (after_detach < n && src[after_detach] == '{') {
                int line = 1, col = 1;
                cc__create_offset_to_line_col(src, after_detach, &line, &col);
                cc__create_error(input_path, line, col, "syntax", "'@detach' does not take a cleanup body for", declared_type);
                free(out);
                return (char*)-1;
            }
            semi = after_detach;
            if (semi >= n || src[semi] != ';') {
                int line = 1, col = 1;
                cc__create_offset_to_line_col(src, semi < n ? semi : n, &line, &col);
                cc__create_error(input_path, line, col, "syntax", "expected ';' after '@detach' declaration for", declared_type);
                free(out);
                return (char*)-1;
            }
        }
        arg0_s = args_s;
        arg0_e = has_second_arg ? comma : args_e;
        while (arg0_s < arg0_e && isspace((unsigned char)src[arg0_s])) arg0_s++;
        while (arg0_e > arg0_s && isspace((unsigned char)src[arg0_e - 1])) arg0_e--;
        if (has_second_arg) {
            arg1_s = comma + 1;
            arg1_e = args_e;
            while (arg1_s < arg1_e && isspace((unsigned char)src[arg1_s])) arg1_s++;
            while (arg1_e > arg1_s && isspace((unsigned char)src[arg1_e - 1])) arg1_e--;
        }
        if (registered_create_callable) {
            char args_buf[1024];
            size_t args_len = args_e - args_s;
            CCArena create_arena = cc__create_heap_arena(1024);
            CCSliceArray create_args = {0};
            CCSliceArray create_arg_types = {0};
            CCSlice type_name_slice = cc_slice_from_buffer((void*)declared_type, strlen(declared_type));
            CCSlice lowered_create = cc_slice_empty();
            static _Thread_local char create_callee_buf[256];
            if (args_len >= sizeof(args_buf)) args_len = sizeof(args_buf) - 1;
            memcpy(args_buf, src + args_s, args_len);
            args_buf[args_len] = '\0';
            create_args = cc__create_build_arg_slices(&create_arena, args_buf);
            create_arg_types = cc__create_build_arg_type_slices(&create_arena, create_args);
            lowered_create = ((CCTypeCreateHandler)registered_create_callable)(type_name_slice, create_args, create_arg_types, &create_arena);
            if (!lowered_create.ptr || lowered_create.len == 0 || lowered_create.len >= sizeof(create_callee_buf)) {
                int line = 1, col = 1;
                cc__create_heap_arena_free(&create_arena);
                cc__create_offset_to_line_col(src, i, &line, &col);
                fprintf(stderr, "%s:%d:%d: error: type: `%s` create hook did not return a valid callee for %d argument%s\n",
                        input_path ? input_path : "<input>", line, col,
                        declared_type, has_second_arg ? 2 : 1, has_second_arg ? "s" : "");
                free(out);
                return (char*)-1;
            }
            memcpy(create_callee_buf, lowered_create.ptr, lowered_create.len);
            create_callee_buf[lowered_create.len] = '\0';
            registered_create_callee = create_callee_buf;
            cc__create_heap_arena_free(&create_arena);
        }
        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
        cc_sb_append_cstr(&out, &out_len, &out_cap, registered_create_callee);
        cc_sb_append_cstr(&out, &out_len, &out_cap, "(");
        cc_sb_append(&out, &out_len, &out_cap, src + arg0_s, arg0_e - arg0_s);
        if (has_second_arg) {
            cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
            cc_sb_append(&out, &out_len, &out_cap, src + arg1_s, arg1_e - arg1_s);
        }
        {
            int need_defer = (ownership == CC_CREATE_OWN_DESTROY &&
                (registered_pre_destroy_callee || destroy_body_s || registered_destroy_callee));
            cc_sb_append_cstr(&out, &out_len, &out_cap, need_defer ? "); " : ");\n");
        }
        if (ownership == CC_CREATE_OWN_DESTROY &&
            (registered_pre_destroy_callee || destroy_body_s || registered_destroy_callee)) {
            int has_pre_destroy = (registered_pre_destroy_callee != NULL);
            int has_custom_body = (destroy_body_s && destroy_body_e > destroy_body_s);
            int has_destroy_callee = (registered_destroy_callee != NULL);
            if (!has_pre_destroy && !has_custom_body && has_destroy_callee) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "@defer ");
                cc_sb_append_cstr(&out, &out_len, &out_cap, registered_destroy_callee);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "(");
                if (strchr(declared_type, '*')) {
                    cc_sb_append(&out, &out_len, &out_cap, src + name_s, name_e - name_s);
                } else {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "&");
                    cc_sb_append(&out, &out_len, &out_cap, src + name_s, name_e - name_s);
                }
                cc_sb_append_cstr(&out, &out_len, &out_cap, ");\n");
            } else {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "@defer { ");
                if (registered_pre_destroy_callee) {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, registered_pre_destroy_callee);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "(");
                    if (strchr(declared_type, '*')) {
                        cc_sb_append(&out, &out_len, &out_cap, src + name_s, name_e - name_s);
                    } else {
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "&");
                        cc_sb_append(&out, &out_len, &out_cap, src + name_s, name_e - name_s);
                    }
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "); ");
                }
                if (destroy_body_s && destroy_body_e > destroy_body_s) {
                    cc_sb_append(&out, &out_len, &out_cap, src + destroy_body_s, destroy_body_e - destroy_body_s + 1);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, " ");
                }
                if (registered_destroy_callee) {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, registered_destroy_callee);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "(");
                    if (strchr(declared_type, '*')) {
                        cc_sb_append(&out, &out_len, &out_cap, src + name_s, name_e - name_s);
                    } else {
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "&");
                        cc_sb_append(&out, &out_len, &out_cap, src + name_s, name_e - name_s);
                    }
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "); ");
                }
                cc_sb_append_cstr(&out, &out_len, &out_cap, "};\n");
            }
        }
        last_emit = semi + 1;
        if (last_emit < n && src[last_emit] == '\n') last_emit++;
        i = last_emit;
        changed = 1;
    }
    if (!changed) {
        free(out);
        return NULL;
    }
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    {
        char* nested = cc_rewrite_registered_type_create_destroy(out, out_len, input_path, symbols);
        if (nested == (char*)-1) {
            free(out);
            return (char*)-1;
        }
        if (nested) {
            free(out);
            return nested;
        }
    }
    return out;
}
