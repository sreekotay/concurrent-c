#include "visitor.h"
#include "visit_codegen.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>
#include <dlfcn.h>

#include "visitor/ufcs.h"
#include "visitor/pass_strip_markers.h"
#include "visitor/pass_await_normalize.h"
#include "visitor/pass_ufcs.h"
#include "visitor/pass_closure_calls.h"
#include "visitor/pass_autoblock.h"
#include "visitor/pass_closure_literal_ast.h"
#include "visitor/pass_defer_syntax.h"
#include "visitor/pass_err_syntax.h"
#include "visitor/pass_result_unwrap.h"
#include "visitor/pass_channel_syntax.h"
#include "visitor/pass_create.h"
#include "visitor/pass_type_syntax.h"
#include "visitor/pass_match_syntax.h"
#include "visitor/pass_with_deadline_syntax.h"
#include "visitor/edit_buffer.h"
#include "visitor/visitor_fileutil.h"
#include "visitor/text_span.h"
#include "header/lower_header.h"
#include "parser/tcc_bridge.h"
#include "preprocess/preprocess.h"
#include "preprocess/type_registry.h"
#include "result_spec.h"
#include "util/path.h"
#include "util/text.h"

#ifndef CC_TCC_EXT_AVAILABLE
#error "CC_TCC_EXT_AVAILABLE is required (patched TCC stub-AST required)."
#endif

/* Local aliases for the shared helpers */
#define cc__sb_append_local cc_sb_append
#define cc__sb_append_cstr_local cc_sb_append_cstr
#define cc__is_ident_char_local2 cc_is_ident_char
#define cc__is_ident_start_local2 cc_is_ident_start
#define cc__skip_ws_local2 cc_skip_ws

#define cc__is_ident_char_local cc_is_ident_char

static char* cc__blank_comptime_blocks_preserve_layout(const char* src, size_t n);
static void cc__collect_ufcs_field_and_var_types(const char* src, size_t n);
static char* cc__rewrite_result_helper_calls_for_parser(const char* src, size_t n);
static int cc__is_parser_placeholder_type_codegen(const char* type_name);
typedef int (*CCCodegenEditCollectorFn)(const CCASTRoot* root,
                                        const CCVisitorCtx* ctx,
                                        CCEditBuffer* eb);
static int cc__apply_coarse_codegen_pass(const CCASTRoot* root,
                                         const CCVisitorCtx* ctx,
                                         char** src_io,
                                         size_t* len_io,
                                         const char* base_src,
                                         CCCodegenEditCollectorFn collect,
                                         int* out_changed);

static const char* cc__canonicalize_placeholder_family_type_codegen(const char* type_name,
                                                                    char* scratch,
                                                                    size_t scratch_cap);
static const char* cc__normalize_bool_spelling_codegen(const char* type_name,
                                                       char* scratch,
                                                       size_t scratch_cap);
static const char* cc__lookup_scoped_local_var_type_codegen(const char* src,
                                                            size_t limit,
                                                            const char* var_name,
                                                            char* out_type,
                                                            size_t out_type_sz);
static int cc__parse_decl_name_and_type_fallback_codegen(const char* stmt_start,
                                                         const char* stmt_end,
                                                         char* decl_name,
                                                         size_t decl_name_sz,
                                                         char* decl_type,
                                                         size_t decl_type_sz);
static int cc__parse_typedef_alias_stmt_codegen(const char* stmt_start,
                                                const char* stmt_end,
                                                char* alias_name,
                                                size_t alias_name_sz,
                                                char* alias_type,
                                                size_t alias_type_sz);
static const char* cc__lookup_enclosing_param_type_codegen(const char* src,
                                                           size_t limit,
                                                           const char* var_name,
                                                           char* out_type,
                                                           size_t out_type_sz);
static void cc__record_function_params_before_brace_codegen(CCTypeRegistry* reg,
                                                            const char* src,
                                                            size_t brace_pos);
static char* cc__rewrite_result_helper_family_to_visible_type(const char* src, size_t n);
static char* cc__rewrite_parser_generic_family_helpers_to_concrete(const char* src, size_t n);
static char* cc__rewrite_string_helper_family_to_visible_type(const char* src, size_t n);
static char* cc__rewrite_parser_placeholder_ufcs_lowers(const char* src, size_t n);
static void cc__emit_line_directive(FILE* out, int line, const char* path) {
    char rel[1024];
    const char* shown = (path && path[0]) ? cc_path_rel_to_repo(path, rel, sizeof(rel)) : "<input>";
    fprintf(out, "#line %d \"%s\"\n", line > 0 ? line : 1, shown);
}

static void cc__maybe_format_lowered_output(const char* out_path) {
    const char* format_flag = getenv("CC_FORMAT_LOWERED");
    const char* formatter = getenv("CC_CLANG_FORMAT");
    char cmd[4096];

    if (!out_path || !out_path[0] || !format_flag || format_flag[0] == '\0' || strcmp(format_flag, "0") == 0) {
        return;
    }
    if (!formatter || !formatter[0]) {
        formatter = "clang-format";
    }
    if (access(out_path, F_OK) != 0) {
        return;
    }
    snprintf(cmd, sizeof(cmd), "%s -i \"%s\" >/dev/null 2>&1", formatter, out_path);
    (void)system(cmd);
}

static void cc__debug_dump_reparse_source(const char* stage,
                                          const char* src,
                                          size_t src_len,
                                          const char* input_path) {
    const char* dir = getenv("CC_DEBUG_REPARSE_DUMP_DIR");
    char rel[1024];
    char safe[1024];
    char path[1536];
    FILE* f = NULL;
    size_t i = 0;
    const char* shown;
    if (!dir || !dir[0] || !stage || !stage[0] || !src) return;
    shown = cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel));
    for (; shown[i] && i + 1 < sizeof(safe); ++i) {
        char c = shown[i];
        safe[i] = (isalnum((unsigned char)c) || c == '_' || c == '-') ? c : '_';
    }
    safe[i] = '\0';
    snprintf(path, sizeof(path), "%s/%s_%s.c", dir, stage, safe[0] ? safe : "input");
    f = fopen(path, "wb");
    if (!f) return;
    fwrite(src, 1, src_len, f);
    fclose(f);
}

static int cc__count_lines_codegen(const char* src, size_t src_len) {
    int lines = 1;
    if (!src || src_len == 0) return 0;
    for (size_t i = 0; i < src_len; ++i) {
        if (src[i] == '\n') lines++;
    }
    return lines;
}

static char* cc__write_failed_reparse_dump(const char* stage,
                                           const char* src,
                                           size_t src_len,
                                           const char* input_path) {
    char tmpl[] = "/tmp/cc_reparse_fail_XXXXXX.c";
    char rel[1024];
    char header[2048];
    const char* shown = NULL;
    int fd = -1;
    int header_len = 0;
    size_t off = 0;
    if (!src || src_len == 0) return NULL;
    shown = cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel));
#ifdef __APPLE__
    fd = mkstemps(tmpl, 2);
#else
    fd = mkstemp(tmpl);
#endif
    if (fd < 0) return NULL;
    header_len = snprintf(header, sizeof(header),
                          "/* cc internal reparse failure\n"
                          " * stage: %s\n"
                          " * input: %s\n"
                          " */\n"
                          "#line 1 \"%s\"\n",
                          (stage && stage[0]) ? stage : "<unknown>",
                          shown ? shown : "<input>",
                          shown ? shown : "<input>");
    if (header_len <= 0 || (size_t)header_len >= sizeof(header)) {
        close(fd);
        unlink(tmpl);
        return NULL;
    }
    while (off < (size_t)header_len) {
        ssize_t n = write(fd, header + off, (size_t)header_len - off);
        if (n <= 0) {
            close(fd);
            unlink(tmpl);
            return NULL;
        }
        off += (size_t)n;
    }
    off = 0;
    while (off < src_len) {
        ssize_t n = write(fd, src + off, src_len - off);
        if (n <= 0) {
            close(fd);
            unlink(tmpl);
            return NULL;
        }
        off += (size_t)n;
    }
    close(fd);
    return strdup(tmpl);
}

static void cc__report_reparse_failure(const char* stage,
                                       const char* input_path,
                                       const char* transformed_src,
                                       size_t transformed_len,
                                       const char* prepared_src,
                                       size_t prepared_len) {
    char rel[1024];
    const char* shown = cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel));
    int transformed_lines = cc__count_lines_codegen(transformed_src, transformed_len);
    int prepared_lines = cc__count_lines_codegen(prepared_src, prepared_len);
    char* dump_path = cc__write_failed_reparse_dump(stage, transformed_src, transformed_len, input_path);
    fprintf(stderr, "cc: internal reparse failed during %s for %s\n",
            (stage && stage[0]) ? stage : "unknown stage",
            shown ? shown : "<input>");
    fprintf(stderr, "cc: parser diagnostics above refer to transformed compiler output, not raw user source\n");
    if (transformed_lines > 0) {
        if (prepared_lines > transformed_lines) {
            fprintf(stderr, "cc: transformed source is %d lines (%d lines after parser prelude/normalization)\n",
                    transformed_lines, prepared_lines);
        } else {
            fprintf(stderr, "cc: transformed source is %d lines\n", transformed_lines);
        }
    }
    if (dump_path) {
        fprintf(stderr, "cc: wrote transformed source dump to %s\n", dump_path);
        free(dump_path);
    }
}

static char* cc__neutralize_comments_for_reparse(const char* src, size_t n) {
    char* out = NULL;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    if (!src) return NULL;
    out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n);
    out[n] = '\0';
    for (size_t i = 0; i < n; i++) {
        char c = out[i];
        char c2 = (i + 1 < n) ? out[i + 1] : 0;
        if (in_lc) {
            if (c == '\n') in_lc = 0;
            else out[i] = ' ';
            continue;
        }
        if (in_bc) {
            if (c == '*' && c2 == '/') {
                out[i] = ' ';
                out[i + 1] = ' ';
                in_bc = 0;
                i++;
                continue;
            }
            if (c != '\n' && c != '\r' && c != '\t') out[i] = ' ';
            continue;
        }
        if (in_str) {
            if (c == '\\' && i + 1 < n) { i++; continue; }
            if (c == '"') in_str = 0;
            continue;
        }
        if (in_chr) {
            if (c == '\\' && i + 1 < n) { i++; continue; }
            if (c == '\'') in_chr = 0;
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c == '/' && c2 == '/') {
            out[i] = ' ';
            out[i + 1] = ' ';
            in_lc = 1;
            i++;
            continue;
        }
        if (c == '/' && c2 == '*') {
            out[i] = ' ';
            out[i + 1] = ' ';
            in_bc = 1;
            i++;
            continue;
        }
    }
    return out;
}

static int cc__apply_coarse_codegen_pass(const CCASTRoot* root,
                                         const CCVisitorCtx* ctx,
                                         char** src_io,
                                         size_t* len_io,
                                         const char* base_src,
                                         CCCodegenEditCollectorFn collect,
                                         int* out_changed) {
    CCEditBuffer eb;
    if (out_changed) *out_changed = 0;
    if (!root || !ctx || !src_io || !*src_io || !len_io || !collect) return 0;
    cc_edit_buffer_init(&eb, *src_io, *len_io);
    if (collect(root, ctx, &eb) < 0) {
        cc_edit_buffer_free(&eb);
        return -1;
    }
    if (eb.count > 0) {
        size_t new_len = 0;
        char* rewritten = cc_edit_buffer_apply(&eb, &new_len);
        if (rewritten) {
            if (*src_io != base_src) free(*src_io);
            *src_io = rewritten;
            *len_io = new_len;
            if (out_changed) *out_changed = 1;
        }
    }
    cc_edit_buffer_free(&eb);
    return 0;
}

static const char* cc__canonicalize_placeholder_family_type_codegen(const char* type_name,
                                                                    char* scratch,
                                                                    size_t scratch_cap) {
    if (!type_name || !scratch || scratch_cap == 0) return type_name;
    if (strncmp(type_name, "CCVec_", 6) == 0 || strncmp(type_name, "Map_", 4) == 0) {
        return type_name;
    }
    if (strncmp(type_name, "__CC_VEC(", 9) == 0) {
        const char* args = type_name + 9;
        const char* close = strrchr(args, ')');
        char mangled[128];
        if (!close || close <= args) return type_name;
        cc_result_spec_mangle_type(args, (size_t)(close - args), mangled, sizeof(mangled));
        if (!mangled[0]) return type_name;
        snprintf(scratch, scratch_cap, "CCVec_%s", mangled);
        return scratch;
    }
    if (strncmp(type_name, "__CC_MAP(", 9) == 0) {
        const char* args = type_name + 9;
        const char* close = strrchr(args, ')');
        const char* comma = NULL;
        int par = 0, br = 0, brc = 0, ang = 0;
        char mangled_k[128];
        char mangled_v[128];
        if (!close || close <= args) return type_name;
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
        if (!comma) return type_name;
        cc_result_spec_mangle_type(args, (size_t)(comma - args), mangled_k, sizeof(mangled_k));
        cc_result_spec_mangle_type(comma + 1, (size_t)(close - (comma + 1)), mangled_v, sizeof(mangled_v));
        if (!mangled_k[0] || !mangled_v[0]) return type_name;
        snprintf(scratch, scratch_cap, "Map_%s_%s", mangled_k, mangled_v);
        return scratch;
    }
    return type_name;
}

static const char* cc__normalize_bool_spelling_codegen(const char* type_name,
                                                       char* scratch,
                                                       size_t scratch_cap) {
    size_t out = 0;
    int changed = 0;
    if (!type_name || !scratch || scratch_cap == 0) return type_name;
    for (size_t i = 0; type_name[i] && out + 1 < scratch_cap; ) {
        if (strncmp(type_name + i, "_Bool", 5) == 0) {
            if (out + 4 >= scratch_cap) break;
            memcpy(scratch + out, "bool", 4);
            out += 4;
            i += 5;
            changed = 1;
            continue;
        }
        scratch[out++] = type_name[i++];
    }
    scratch[out] = '\0';
    return changed ? scratch : type_name;
}

static char* cc__rewrite_parser_placeholder_ufcs_lowers(const char* src, size_t n) {
    CCTypeRegistry* reg = cc_type_registry_get_global();
    char* out = NULL;
    size_t out_len = 0, out_cap = 0, last_emit = 0;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    int changed = 0;
    if (!src || n == 0) return NULL;

    for (size_t i = 0; i < n; ) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;

        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }

        if ((i == 0 || !cc_is_ident_char(src[i - 1])) &&
            i + 9 < n && memcmp(src + i, "CCString_", 9) == 0) {
            size_t method_start = i + 9;
            size_t method_end = method_start;
            const char* replacement = NULL;
            while (method_end < n && cc_is_ident_char(src[method_end])) method_end++;
            if (method_end < n && src[method_end] == '(') {
                size_t method_len = method_end - method_start;
                if (method_len == 6 && memcmp(src + method_start, "append", 6) == 0) replacement = "CCString_push";
                else if (method_len == 4 && memcmp(src + method_start, "push", 4) == 0) replacement = "CCString_push";
                else if (method_len == 8 && memcmp(src + method_start, "as_slice", 8) == 0) replacement = "CCString_as_slice";
                else if (method_len == 5 && memcmp(src + method_start, "clear", 5) == 0) replacement = "CCString_clear";
                else if (method_len == 4 && memcmp(src + method_start, "cstr", 4) == 0) replacement = "CCString_cstr";
                else if (method_len == 3 && memcmp(src + method_start, "len", 3) == 0) replacement = "CCString_len";
                else if (method_len == 3 && memcmp(src + method_start, "cap", 3) == 0) replacement = "CCString_cap";
                else if (method_len > 0) {
                    static _Thread_local char buf[128];
                    snprintf(buf, sizeof(buf), "CCString_%.*s", (int)method_len, src + method_start);
                    replacement = buf;
                }
                if (replacement) {
                    cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                    cc__sb_append_cstr_local(&out, &out_len, &out_cap, replacement);
                    last_emit = method_end;
                    i = method_end;
                    changed = 1;
                    continue;
                }
            }
        }

        if ((i == 0 || !cc_is_ident_char(src[i - 1])) &&
            (i + 17 < n && memcmp(src + i, "__cc_vec_generic_", 17) == 0)) {
            size_t prefix_len = 17;
            size_t method_start = i + prefix_len;
            size_t method_end = method_start;
            while (method_end < n && cc_is_ident_char(src[method_end])) method_end++;
            if (method_end < n && src[method_end] == '(') {
                size_t arg_start = method_end + 1;
                size_t p = arg_start;
                int par = 0, br = 0, brc = 0, in_s = 0, in_c = 0;
                size_t arg_end = arg_start;
                while (p < n) {
                    char d = src[p];
                    if (in_s) { if (d == '\\' && p + 1 < n) { p += 2; continue; } if (d == '"') in_s = 0; p++; continue; }
                    if (in_c) { if (d == '\\' && p + 1 < n) { p += 2; continue; } if (d == '\'') in_c = 0; p++; continue; }
                    if (d == '"') { in_s = 1; p++; continue; }
                    if (d == '\'') { in_c = 1; p++; continue; }
                    if (d == '(') par++;
                    else if (d == ')' && par == 0 && br == 0 && brc == 0) { arg_end = p; break; }
                    else if (d == ')' && par > 0) par--;
                    else if (d == '[') br++;
                    else if (d == ']' && br > 0) br--;
                    else if (d == '{') brc++;
                    else if (d == '}' && brc > 0) brc--;
                    else if (d == ',' && par == 0 && br == 0 && brc == 0) { arg_end = p; break; }
                    p++;
                }
                if (arg_end > arg_start && reg) {
                    char arg_expr[256];
                    char family_buf[256];
                    const char* recv_s = src + arg_start;
                    const char* recv_e = src + arg_end;
                    while (recv_s < recv_e && isspace((unsigned char)*recv_s)) recv_s++;
                    while (recv_e > recv_s && isspace((unsigned char)recv_e[-1])) recv_e--;
                    if ((size_t)(recv_e - recv_s) < sizeof(arg_expr)) {
                        memcpy(arg_expr, recv_s, (size_t)(recv_e - recv_s));
                        arg_expr[recv_e - recv_s] = '\0';
                        {
                            int recv_is_ptr = 0;
                            char root[64] = {0};
                            char field[64] = {0};
                            const char* q = arg_expr;
                            const char* root_ty = NULL;
                            const char* field_ty = NULL;
                            size_t rn = 0, fn = 0;
                            const char* type_name = cc_type_registry_resolve_receiver_expr_at(
                                reg, arg_expr, src, (size_t)(recv_s - src), &recv_is_ptr);
                            if (*q == '&') q++;
                            while (*q == ' ' || *q == '\t') q++;
                            while (q[rn] && (isalnum((unsigned char)q[rn]) || q[rn] == '_') && rn + 1 < sizeof(root)) rn++;
                            memcpy(root, q, rn);
                            root[rn] = '\0';
                            root_ty = root[0] ? cc_type_registry_lookup_var(reg, root) : NULL;
                            q += rn;
                            if (*q == '.') q++;
                            while (q[fn] && (isalnum((unsigned char)q[fn]) || q[fn] == '_') && fn + 1 < sizeof(field)) fn++;
                            memcpy(field, q, fn);
                            field[fn] = '\0';
                            field_ty = (root_ty && field[0]) ? cc_type_registry_lookup_field(reg, root_ty, field) : NULL;
                            if (!type_name && field_ty) type_name = field_ty;
                            const char* family_name = cc__canonicalize_placeholder_family_type_codegen(type_name, family_buf, sizeof(family_buf));
                            if (getenv("CC_DEBUG_PLACEHOLDER_UFCS")) {
                                fprintf(stderr, "placeholder vec recv='%s' type='%s' family='%s'\n",
                                        arg_expr,
                                        type_name ? type_name : "<null>",
                                        family_name ? family_name : "<null>");
                            }
                            if (family_name && strncmp(family_name, "CCVec_", 6) == 0) {
                                cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                                cc__sb_append_cstr_local(&out, &out_len, &out_cap, family_name);
                                cc__sb_append_cstr_local(&out, &out_len, &out_cap, "_");
                                cc__sb_append_local(&out, &out_len, &out_cap, src + method_start, method_end - method_start);
                                last_emit = method_end;
                                i = method_end;
                                changed = 1;
                                continue;
                            }
                        }
                    }
                }
            }
        }

        if ((i == 0 || !cc_is_ident_char(src[i - 1])) &&
            (i + 17 < n && memcmp(src + i, "__cc_map_generic_", 17) == 0)) {
            size_t prefix_len = 17;
            size_t method_start = i + prefix_len;
            size_t method_end = method_start;
            while (method_end < n && cc_is_ident_char(src[method_end])) method_end++;
            if (method_end < n && src[method_end] == '(') {
                size_t arg_start = method_end + 1;
                size_t p = arg_start;
                int par = 0, br = 0, brc = 0, in_s = 0, in_c = 0;
                size_t arg_end = arg_start;
                while (p < n) {
                    char d = src[p];
                    if (in_s) { if (d == '\\' && p + 1 < n) { p += 2; continue; } if (d == '"') in_s = 0; p++; continue; }
                    if (in_c) { if (d == '\\' && p + 1 < n) { p += 2; continue; } if (d == '\'') in_c = 0; p++; continue; }
                    if (d == '"') { in_s = 1; p++; continue; }
                    if (d == '\'') { in_c = 1; p++; continue; }
                    if (d == '(') par++;
                    else if (d == ')' && par == 0 && br == 0 && brc == 0) { arg_end = p; break; }
                    else if (d == ')' && par > 0) par--;
                    else if (d == '[') br++;
                    else if (d == ']' && br > 0) br--;
                    else if (d == '{') brc++;
                    else if (d == '}' && brc > 0) brc--;
                    else if (d == ',' && par == 0 && br == 0 && brc == 0) { arg_end = p; break; }
                    p++;
                }
                if (arg_end > arg_start && reg) {
                    char arg_expr[256];
                    char family_buf[256];
                    const char* recv_s = src + arg_start;
                    const char* recv_e = src + arg_end;
                    while (recv_s < recv_e && isspace((unsigned char)*recv_s)) recv_s++;
                    while (recv_e > recv_s && isspace((unsigned char)recv_e[-1])) recv_e--;
                    if ((size_t)(recv_e - recv_s) < sizeof(arg_expr)) {
                        memcpy(arg_expr, recv_s, (size_t)(recv_e - recv_s));
                        arg_expr[recv_e - recv_s] = '\0';
                        {
                            int recv_is_ptr = 0;
                            char root[64] = {0};
                            char field[64] = {0};
                            const char* q = arg_expr;
                            const char* root_ty = NULL;
                            const char* field_ty = NULL;
                            size_t rn = 0, fn = 0;
                            const char* type_name = cc_type_registry_resolve_receiver_expr_at(
                                reg, arg_expr, src, (size_t)(recv_s - src), &recv_is_ptr);
                            if (*q == '&') q++;
                            while (*q == ' ' || *q == '\t') q++;
                            while (q[rn] && (isalnum((unsigned char)q[rn]) || q[rn] == '_') && rn + 1 < sizeof(root)) rn++;
                            memcpy(root, q, rn);
                            root[rn] = '\0';
                            root_ty = root[0] ? cc_type_registry_lookup_var(reg, root) : NULL;
                            q += rn;
                            if (*q == '.') q++;
                            while (q[fn] && (isalnum((unsigned char)q[fn]) || q[fn] == '_') && fn + 1 < sizeof(field)) fn++;
                            memcpy(field, q, fn);
                            field[fn] = '\0';
                            field_ty = (root_ty && field[0]) ? cc_type_registry_lookup_field(reg, root_ty, field) : NULL;
                            if (!type_name && field_ty) type_name = field_ty;
                            const char* family_name = cc__canonicalize_placeholder_family_type_codegen(type_name, family_buf, sizeof(family_buf));
                            if (getenv("CC_DEBUG_PLACEHOLDER_UFCS")) {
                                fprintf(stderr, "placeholder map recv='%s' type='%s' family='%s'\n",
                                        arg_expr,
                                        type_name ? type_name : "<null>",
                                        family_name ? family_name : "<null>");
                            }
                            if (family_name && strncmp(family_name, "Map_", 4) == 0) {
                                cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                                cc__sb_append_cstr_local(&out, &out_len, &out_cap, family_name);
                                cc__sb_append_cstr_local(&out, &out_len, &out_cap, "_");
                                cc__sb_append_local(&out, &out_len, &out_cap, src + method_start, method_end - method_start);
                                last_emit = method_end;
                                i = method_end;
                                changed = 1;
                                continue;
                            }
                        }
                    }
                }
            }
        }

        if (src[i] == '.' || (i + 1 < n && src[i] == '-' && src[i + 1] == '>')) {
            size_t sep_len = (src[i] == '.') ? 1 : 2;
            size_t method_start = i + sep_len;
            size_t method_end = method_start;
            size_t recv_start = i;
            const char* lowered_method = NULL;
            while (method_start < n && isspace((unsigned char)src[method_start])) method_start++;
            while (method_end < n && cc_is_ident_char(src[method_end])) method_end++;
            if (method_end > method_start && method_end < n && src[method_end] == '(' && reg) {
                size_t recv_end = i;
                char recv_expr[256];
                char local_type_buf[256];
                char lowered[384];
                int recv_is_ptr = 0;
                const char* type_name;
                while (recv_start > 0 && isspace((unsigned char)src[recv_start - 1])) recv_start--;
                while (recv_start > 0) {
                    char prev = src[recv_start - 1];
                    if (cc_is_ident_char(prev) || prev == '&') {
                        recv_start--;
                        continue;
                    }
                    if (prev == '.') {
                        recv_start--;
                        while (recv_start > 0 && isspace((unsigned char)src[recv_start - 1])) recv_start--;
                        continue;
                    }
                    if (recv_start >= 2 && src[recv_start - 1] == '>' && src[recv_start - 2] == '-') {
                        recv_start -= 2;
                        while (recv_start > 0 && isspace((unsigned char)src[recv_start - 1])) recv_start--;
                        continue;
                    }
                    break;
                }
                while (recv_start < recv_end && isspace((unsigned char)src[recv_start])) recv_start++;
                while (recv_end > recv_start && isspace((unsigned char)src[recv_end - 1])) recv_end--;
                if (recv_end > recv_start && (recv_end - recv_start) < sizeof(recv_expr)) {
                    memcpy(recv_expr, src + recv_start, recv_end - recv_start);
                    recv_expr[recv_end - recv_start] = '\0';
                    type_name = cc__lookup_scoped_local_var_type_codegen(
                        src, recv_start, recv_expr, local_type_buf, sizeof(local_type_buf));
                    if (!type_name) {
                        type_name = cc_type_registry_resolve_receiver_expr_at(
                            reg, recv_expr, src, recv_start, &recv_is_ptr);
                    }
                    if (type_name && strncmp(type_name, "CCResult_", 9) == 0) {
                        char result_type_buf[256];
                        const char* result_type_name =
                            cc__normalize_bool_spelling_codegen(type_name, result_type_buf, sizeof(result_type_buf));
                        size_t method_len = method_end - method_start;
                        if (method_len == 5 && memcmp(src + method_start, "value", 5) == 0) lowered_method = "unwrap";
                        else if (method_len == 5 && memcmp(src + method_start, "error", 5) == 0) lowered_method = "error";
                        else if (method_len == 5 && memcmp(src + method_start, "is_ok", 5) == 0) lowered_method = "is_ok";
                        else if (method_len == 6 && memcmp(src + method_start, "is_err", 6) == 0) lowered_method = "is_err";
                        else if (method_len == 9 && memcmp(src + method_start, "unwrap_or", 9) == 0) lowered_method = "unwrap_or";
                        if (lowered_method) {
                            if (strcmp(lowered_method, "is_ok") == 0) {
                                snprintf(lowered, sizeof(lowered), "cc_is_ok(%s", recv_expr);
                            } else if (strcmp(lowered_method, "is_err") == 0) {
                                snprintf(lowered, sizeof(lowered), "cc_is_err(%s", recv_expr);
                            } else {
                                snprintf(lowered, sizeof(lowered), "%s_%s(%s",
                                         result_type_name, lowered_method, recv_expr);
                            }
                            cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, recv_start - last_emit);
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, lowered);
                            last_emit = method_end + 1;
                            i = method_end + 1;
                            changed = 1;
                            continue;
                        }
                    }
                }
            }
        }

        i++;
    }

    if (!changed) {
        free(out);
        return NULL;
    }
    if (last_emit < n) cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Helper: reparse source string to AST (in-memory). */
static CCASTRoot* cc__reparse_source_to_ast(const char* src, size_t src_len,
                                            const char* input_path, CCSymbolTable* symbols,
                                            const char* stage) {
    CCTypeRegistry* saved_reg = cc_type_registry_get_global();
    CCTypeRegistry* temp_reg = cc_type_registry_new();
    char* nursery_rewritten = cc_rewrite_nursery_create_destroy_proto(src, src_len, input_path);
    char* registered_create = NULL;
    char* family_rewritten = NULL;
    char* reparse_clean = NULL;
    CCASTRoot* root = NULL;
    if (temp_reg) cc_type_registry_set_global(temp_reg);
    const char* pp_in = nursery_rewritten ? nursery_rewritten : src;
    size_t pp_in_len = nursery_rewritten ? strlen(nursery_rewritten) : src_len;
    if (symbols) {
        registered_create = cc_rewrite_registered_type_create_destroy(pp_in, pp_in_len, input_path, symbols);
        if (registered_create == (char*)-1) {
            if (nursery_rewritten) free(nursery_rewritten);
            goto done;
        }
        if (registered_create) {
            pp_in = registered_create;
            pp_in_len = strlen(registered_create);
        }
    }
    /* Reparses can still see concrete family UFCS inside closure bodies before
       those regions are lowered by later passes. Normalize only the already-
       concrete family calls here so parser-mode TCC can build a stub AST. */
    family_rewritten = cc_rewrite_generic_family_ufcs_parser_safe(pp_in, pp_in_len);
    if (family_rewritten) {
        pp_in = family_rewritten;
        pp_in_len = strlen(family_rewritten);
    }
    reparse_clean = cc__neutralize_comments_for_reparse(pp_in, pp_in_len);
    if (reparse_clean) {
        pp_in = reparse_clean;
        pp_in_len = strlen(reparse_clean);
    }
    char* pp_buf = cc_preprocess_to_string_ex(pp_in, pp_in_len, input_path, 1);
    if (nursery_rewritten) free(nursery_rewritten);
    if (registered_create) free(registered_create);
    if (family_rewritten) free(family_rewritten);
    if (reparse_clean) free(reparse_clean);
    if (!pp_buf) goto done;
    {
        size_t st_len = 0;
        char* st = cc__rewrite_chan_send_task_text(NULL, pp_buf, strlen(pp_buf), &st_len);
        if (st) {
            free(pp_buf);
            pp_buf = st;
            (void)st_len;
        }
    }
    size_t pp_len = strlen(pp_buf);
    char* prep = cc__prepend_reparse_prelude(pp_buf, pp_len, &pp_len, input_path);
    free(pp_buf);
    if (!prep) goto done;
    {
        char* parser_helpers = cc__rewrite_result_helper_calls_for_parser(prep, pp_len);
        if (parser_helpers) {
            free(prep);
            prep = parser_helpers;
            pp_len = strlen(prep);
        }
    }
    char rel_path[1024];
    cc_path_rel_to_repo(input_path, rel_path, sizeof(rel_path));
    root = cc_tcc_bridge_parse_string_to_ast(prep, rel_path, input_path, symbols);
    if (!root) {
        cc__report_reparse_failure(stage, input_path, src, src_len, prep, pp_len);
    }
    free(prep);
done:
    if (temp_reg) {
        cc_type_registry_set_global(saved_reg);
        cc_type_registry_free(temp_reg);
    }
    return root;
}

/* AST-driven async lowering (implemented in `cc/src/visitor/async_ast.c`). */
int cc_async_rewrite_state_machine_ast(const CCASTRoot* root,
                                       const CCVisitorCtx* ctx,
                                       const char* in_src,
                                       size_t in_len,
                                       char** out_src,
                                       size_t* out_len);

/* Legacy closure scan/lowering helpers removed - now handled by AST-span passes. */

/* Strip CC decl markers so output is valid C. This is used regardless of whether
   TCC extensions are available, because the output C is compiled by the host compiler. */
/* cc__read_entire_file / cc__write_temp_c_file are implemented in visitor_fileutil.c */

/* UFCS span rewrite lives in pass_ufcs.c now (cc__rewrite_ufcs_spans_with_nodes). */

/* Helper to append to a string buffer */
static void cc__cg_sb_append(char** out, size_t* out_len, size_t* out_cap, const char* s, size_t len) {
    if (!s || len == 0) return;
    while (*out_len + len + 1 > *out_cap) {
        size_t new_cap = (*out_cap == 0) ? 256 : (*out_cap * 2);
        char* new_out = (char*)realloc(*out, new_cap);
        if (!new_out) return;
        *out = new_out;
        *out_cap = new_cap;
    }
    memcpy(*out + *out_len, s, len);
    *out_len += len;
    (*out)[*out_len] = 0;
}

static void cc__cg_sb_append_cstr(char** out, size_t* out_len, size_t* out_cap, const char* s) {
    if (s) cc__cg_sb_append(out, out_len, out_cap, s, strlen(s));
}

static char* cc__rewrite_result_helper_calls_for_parser(const char* src, size_t n) {
    if (!src || n == 0) return NULL;

    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    size_t last_emit = 0;
    while (i < n) {
        i = cc_skip_ws_and_comments(src, n, i);
        if (i >= n) break;
        if (src[i] == '"' || src[i] == '\'') {
            char q = src[i++];
            while (i < n) {
                if (src[i] == '\\' && i + 1 < n) {
                    i += 2;
                    continue;
                }
                if (src[i] == q) {
                    i++;
                    break;
                }
                i++;
            }
            continue;
        }
        if (!cc_is_ident_start(src[i])) {
            i++;
            continue;
        }

        size_t ident_start = i;
        while (i < n && cc_is_ident_char(src[i])) i++;
        size_t ident_end = i;
        if (ident_end - ident_start <= 9 || memcmp(src + ident_start, "CCResult_", 9) != 0) continue;

        size_t j = cc_skip_ws_and_comments(src, n, ident_end);
        if (j >= n || src[j] != '(') continue;

        size_t ident_len = ident_end - ident_start;
        const char* parser_method = NULL;
        size_t suffix_len = 0;
        if (ident_len > 10 && memcmp(src + ident_end - 10, "_unwrap_or", 10) == 0) {
            parser_method = "unwrap_or";
            suffix_len = 10;
        } else if (ident_len > 7 && memcmp(src + ident_end - 7, "_is_err", 7) == 0) {
            parser_method = "is_err";
            suffix_len = 7;
        } else if (ident_len > 6 && memcmp(src + ident_end - 6, "_is_ok", 6) == 0) {
            parser_method = "is_ok";
            suffix_len = 6;
        } else if (ident_len > 7 && memcmp(src + ident_end - 7, "_unwrap", 7) == 0) {
            parser_method = "unwrap";
            suffix_len = 7;
        } else if (ident_len > 6 && memcmp(src + ident_end - 6, "_error", 6) == 0) {
            parser_method = "error";
            suffix_len = 6;
        } else {
            continue;
        }

        size_t paren_end = 0;
        if (!cc_find_matching_paren(src, n, j, &paren_end)) continue;

        /* Skip helper declarations/definitions like:
           CCResult_T_E_error(CCResult_T_E r);
           We only want to rewrite expression call sites. */
        {
            size_t p = cc_skip_ws_and_comments(src, n, j + 1);
            if (p < paren_end && cc_is_ident_start(src[p])) {
                size_t first_tok_end = p + 1;
                while (first_tok_end < paren_end && cc_is_ident_char(src[first_tok_end])) first_tok_end++;
                p = cc_skip_ws_and_comments(src, n, first_tok_end);
                if (p < paren_end && (cc_is_ident_start(src[p]) || src[p] == '*')) {
                    i = paren_end + 1;
                    continue;
                }
            }
        }

        cc__cg_sb_append(&out, &out_len, &out_cap, src + last_emit, ident_start - last_emit);
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "__cc_parser_result_");
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, parser_method);
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "_");
        cc__cg_sb_append(&out, &out_len, &out_cap, src + ident_start, ident_len - suffix_len);
        last_emit = ident_end;
    }

    if (last_emit == 0) return NULL;
    if (last_emit < n) cc__cg_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

static int cc__find_matching_paren_codegen(const char* src, size_t len, size_t lpar, size_t* out_rpar) {
    int depth = 0, in_str = 0, in_chr = 0, in_lc = 0, in_bc = 0;
    for (size_t i = lpar; i < len; ++i) {
        char c = src[i];
        char c2 = (i + 1 < len) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; } continue; }
        if (in_str) { if (c == '\\' && c2) { i++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && c2) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c == '(') depth++;
        else if (c == ')') {
            depth--;
            if (depth == 0) {
                *out_rpar = i;
                return 1;
            }
        }
    }
    return 0;
}

static int cc__find_matching_brace_codegen(const char* src, size_t len, size_t lbrace, size_t* out_rbrace) {
    int depth = 0, in_str = 0, in_chr = 0, in_lc = 0, in_bc = 0;
    for (size_t i = lbrace; i < len; ++i) {
        char c = src[i];
        char c2 = (i + 1 < len) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; } continue; }
        if (in_str) { if (c == '\\' && c2) { i++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && c2) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) {
                *out_rbrace = i;
                return 1;
            }
        }
    }
    return 0;
}

static const char* cc__canonicalize_string_type_codegen(const char* type_name) {
    if (!type_name) return NULL;
    if (strcmp(type_name, "CCVec_char") == 0 || strcmp(type_name, "__CCVecGeneric") == 0) {
        return "CCString";
    }
    if (strcmp(type_name, "CCVec_char*") == 0 || strcmp(type_name, "__CCVecGeneric*") == 0) {
        return "CCString*";
    }
    return type_name;
}

static size_t cc__skip_ws_codegen(const char* src, size_t n, size_t i) {
    while (i < n && isspace((unsigned char)src[i])) i++;
    return i;
}

static int cc__match_keyword_codegen(const char* src, size_t n, size_t pos, const char* kw) {
    size_t klen = strlen(kw);
    if (pos + klen > n) return 0;
    if (memcmp(src + pos, kw, klen) != 0) return 0;
    if (pos > 0 && (isalnum((unsigned char)src[pos - 1]) || src[pos - 1] == '_')) return 0;
    if (pos + klen < n && (isalnum((unsigned char)src[pos + klen]) || src[pos + klen] == '_')) return 0;
    return 1;
}

static void cc__trim_range_codegen(const char* src, size_t* start, size_t* end) {
    while (*start < *end && isspace((unsigned char)src[*start])) (*start)++;
    while (*end > *start && isspace((unsigned char)src[*end - 1])) (*end)--;
}

static int cc__parse_string_literal_codegen(const char* src, size_t n, size_t* io_pos, char* out, size_t out_sz) {
    size_t i = *io_pos;
    size_t out_len = 0;
    if (i >= n || src[i] != '"') return 0;
    i++;
    while (i < n) {
        char c = src[i++];
        if (c == '"') {
            if (out_sz > 0) out[out_len < out_sz ? out_len : out_sz - 1] = '\0';
            *io_pos = i;
            return 1;
        }
        if (c == '\\' && i < n) {
            char esc = src[i++];
            c = esc;
        }
        if (out_len + 1 < out_sz) out[out_len] = c;
        out_len++;
    }
    return 0;
}

static int cc__parse_ident_codegen(const char* src, size_t n, size_t* io_pos, char* out, size_t out_sz) {
    size_t i = *io_pos;
    size_t len = 0;
    if (i >= n || !(isalpha((unsigned char)src[i]) || src[i] == '_')) return 0;
    while (i < n && (isalnum((unsigned char)src[i]) || src[i] == '_')) {
        if (len + 1 < out_sz) out[len] = src[i];
        len++;
        i++;
    }
    if (out_sz > 0) out[len < out_sz ? len : out_sz - 1] = '\0';
    *io_pos = i;
    return 1;
}

typedef struct {
    void* dl_handle;
    char obj_path[1024];
    char dylib_path[1024];
} CCComptimeDlModule;

/*
 * Named @comptime UFCS handlers currently execute through a small backend
 * boundary:
 *   1. extract a helper translation unit from the original source
 *   2. build a loadable helper module
 *   3. resolve the exported wrapper and store it as a callable registration
 *
 * On macOS we intentionally use the host C toolchain plus a temporary dylib
 * here rather than libtcc in-process relocation. That keeps the current bridge
 * working while the pure libtcc backend remains a separate follow-up.
 */

static void cc__dl_module_free(void* owner) {
    CCComptimeDlModule* module = (CCComptimeDlModule*)owner;
    if (!module) return;
    if (module->dl_handle) dlclose(module->dl_handle);
    if (module->obj_path[0]) unlink(module->obj_path);
    if (module->dylib_path[0]) unlink(module->dylib_path);
    free(module);
}

static void cc__dirname_codegen(const char* path, char* out, size_t out_sz) {
    size_t len = 0;
    const char* slash;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!path || !path[0]) return;
    slash = strrchr(path, '/');
    if (!slash) {
        strncpy(out, ".", out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    len = (size_t)(slash - path);
    if (len == 0) len = 1;
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static int cc__file_exists_codegen(const char* path) {
    return path && path[0] && access(path, F_OK) == 0;
}

static void cc__dirname_inplace_codegen(char* path) {
    size_t len;
    char* slash;
    if (!path) return;
    len = strlen(path);
    while (len > 0 && path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }
    if (len == 0) return;
    slash = strrchr(path, '/');
    if (!slash) {
        strcpy(path, ".");
        return;
    }
    if (slash == path) {
        path[1] = '\0';
        return;
    }
    *slash = '\0';
}

static int cc__find_repo_root_codegen(const char* input_path, char* out, size_t out_sz) {
    char dir[1024];
    if (!input_path || !out || out_sz == 0) return 0;
    cc__dirname_codegen(input_path, dir, sizeof(dir));
    for (int depth = 0; depth < 20 && dir[0]; ++depth) {
        char marker[1200];
        size_t len = strlen(dir);
        snprintf(marker, sizeof(marker), "%s/cc/src/cc_main.c", dir);
        if (cc__file_exists_codegen(marker)) {
            strncpy(out, dir, out_sz - 1);
            out[out_sz - 1] = '\0';
            return 1;
        }
        if (strcmp(dir, "/") == 0 || len == 0) break;
        cc__dirname_inplace_codegen(dir);
    }
    return 0;
}

static int cc__chunk_contains_at_codegen(const char* src, size_t start, size_t end) {
    if (!src || end <= start) return 0;
    for (size_t i = start; i < end; ++i) {
        if (src[i] == '@') return 1;
    }
    return 0;
}

static char* cc__build_comptime_tu_from_preprocessed(const char* src,
                                                     size_t n,
                                                     const char* repo_root,
                                                     const char* extra_defs,
                                                     const char* entry_name,
                                                     const char* callable_name,
                                                     int include_top_level_defs,
                                                     int source_is_include_expanded) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    int line_start = 1;
    if (!src || !entry_name || !callable_name) return NULL;
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "#ifndef __CC__\n");
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "#define __CC__ 1\n");
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "#endif\n");
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "#ifndef CC_PARSER_MODE\n");
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "#define CC_PARSER_MODE 1\n");
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "#endif\n");
    if (!source_is_include_expanded && repo_root && repo_root[0]) {
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "#include \"");
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, repo_root);
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "/out/include/ccc/cc_ufcs.h\"\n");
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "#include \"");
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, repo_root);
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "/out/include/ccc/std/string.h\"\n");
    }
    while (i < n) {
        if (line_start && src[i] == '#') {
            size_t line_end = i;
            while (line_end < n && src[line_end] != '\n') line_end++;
            if (strncmp(src + i, "#line", 5) == 0) {
                /* Keep line markers out of the temporary comptime TU. */
            } else {
                cc__cg_sb_append(&out, &out_len, &out_cap, src + i, line_end - i);
            }
            cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "\n");
            i = (line_end < n) ? line_end + 1 : line_end;
            line_start = 1;
            continue;
        }
        if (isspace((unsigned char)src[i])) {
            line_start = (src[i] == '\n');
            i++;
            continue;
        }
        if (!include_top_level_defs) {
            while (i < n && src[i] != '\n') i++;
            line_start = 1;
            continue;
        }
        {
            size_t start = i;
            size_t j = i;
            int depth = 0, in_str = 0, in_chr = 0, in_lc = 0, in_bc = 0;
            int saw_top_paren_close = 0;
            for (; j < n; ++j) {
                char c = src[j];
                char c2 = (j + 1 < n) ? src[j + 1] : 0;
                if (in_lc) { if (c == '\n') in_lc = 0; continue; }
                if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; j++; } continue; }
                if (in_str) { if (c == '\\' && c2) { j++; continue; } if (c == '"') in_str = 0; continue; }
                if (in_chr) { if (c == '\\' && c2) { j++; continue; } if (c == '\'') in_chr = 0; continue; }
                if (c == '/' && c2 == '/') { in_lc = 1; j++; continue; }
                if (c == '/' && c2 == '*') { in_bc = 1; j++; continue; }
                if (c == '"') { in_str = 1; continue; }
                if (c == '\'') { in_chr = 1; continue; }
                if (c == '(') { depth++; continue; }
                if (c == ')') {
                    if (depth > 0) depth--;
                    if (depth == 0) saw_top_paren_close = 1;
                    continue;
                }
                if (c == '{' && depth == 0) {
                    size_t body_r = 0;
                    if (!cc__find_matching_brace_codegen(src, n, j, &body_r)) {
                        free(out);
                        return NULL;
                    }
                    if (!saw_top_paren_close) {
                        j = body_r;
                        continue;
                    }
                    if (!cc__chunk_contains_at_codegen(src, start, body_r + 1)) {
                        cc__cg_sb_append(&out, &out_len, &out_cap, src + start, body_r + 1 - start);
                        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "\n");
                    }
                    j = body_r;
                    break;
                }
                if (c == ';' && depth == 0) {
                    if (!cc__chunk_contains_at_codegen(src, start, j + 1)) {
                        cc__cg_sb_append(&out, &out_len, &out_cap, src + start, j + 1 - start);
                        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "\n");
                    }
                    break;
                }
            }
            i = (j < n) ? (j + 1) : j;
            line_start = 1;
        }
    }
    if (extra_defs && extra_defs[0]) {
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "\n");
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, extra_defs);
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "\n");
    }
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "\nCCSlice ");
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, entry_name);
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap,
                          "(CCSlice recv_type, CCSlice method, CCSlice mode, CCSliceArray argv, CCSliceArray arg_types, CCArena *arena) {\n    return ");
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, callable_name);
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "(recv_type, method, mode, argv, arg_types, arena);\n}\n");
    return out;
}

static int cc__format_host_comptime_compile_cmd(char* out,
                                                size_t out_sz,
                                                const char* repo_root,
                                                const char* input_dir,
                                                const char* dylib_path,
                                                const char* source_path) {
    if (!out || out_sz == 0 || !dylib_path || !source_path) return -1;
#ifdef __APPLE__
    if (repo_root && repo_root[0]) {
        return snprintf(out, out_sz,
                        "cc -dynamiclib -undefined dynamic_lookup -I\"%s/cc/include\" -I\"%s/out/include\" -o \"%s\" \"%s\"",
                        repo_root, repo_root, dylib_path, source_path) >= (int)out_sz ? -1 : 0;
    }
    if (input_dir && input_dir[0]) {
        return snprintf(out, out_sz,
                        "cc -dynamiclib -undefined dynamic_lookup -I\"%s\" -o \"%s\" \"%s\"",
                        input_dir, dylib_path, source_path) >= (int)out_sz ? -1 : 0;
    }
    return snprintf(out, out_sz,
                    "cc -dynamiclib -undefined dynamic_lookup -o \"%s\" \"%s\"",
                    dylib_path, source_path) >= (int)out_sz ? -1 : 0;
#else
    if (repo_root && repo_root[0]) {
        return snprintf(out, out_sz,
                        "cc -shared -fPIC -I\"%s/cc/include\" -I\"%s/out/include\" -o \"%s\" \"%s\"",
                        repo_root, repo_root, dylib_path, source_path) >= (int)out_sz ? -1 : 0;
    }
    if (input_dir && input_dir[0]) {
        return snprintf(out, out_sz,
                        "cc -shared -fPIC -I\"%s\" -o \"%s\" \"%s\"",
                        input_dir, dylib_path, source_path) >= (int)out_sz ? -1 : 0;
    }
    return snprintf(out, out_sz,
                    "cc -shared -fPIC -o \"%s\" \"%s\"",
                    dylib_path, source_path) >= (int)out_sz ? -1 : 0;
#endif
}

static char* cc__build_lambda_handler_definition_codegen(const char* src,
                                                         size_t n,
                                                         size_t handler_s,
                                                         size_t handler_e,
                                                         const char* lambda_name) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t lpar = 0, rpar = 0, body_s = 0, body_e = handler_e;
    char params[6][64];
    const char* param_types[6] = { "CCSlice", "CCSlice", "CCSlice", "CCSliceArray", "CCSliceArray", "CCArena *" };
    int param_count = 0;
    if (!src || !lambda_name || handler_s >= handler_e || handler_e > n) return NULL;
    cc__trim_range_codegen(src, &handler_s, &handler_e);
    if (handler_s >= handler_e || src[handler_s] != '(') return NULL;
    lpar = handler_s;
    if (!cc__find_matching_paren_codegen(src, handler_e, lpar, &rpar)) return NULL;
    {
        size_t p = lpar + 1;
        while (p < rpar) {
            p = cc__skip_ws_codegen(src, rpar, p);
            if (p >= rpar) break;
            if (param_count >= 6 || !cc__parse_ident_codegen(src, rpar, &p, params[param_count], sizeof(params[param_count]))) {
                return NULL;
            }
            param_count++;
            p = cc__skip_ws_codegen(src, rpar, p);
            if (p < rpar) {
                if (src[p] != ',') return NULL;
                p++;
            }
        }
    }
    if (param_count != 6) return NULL;
    body_s = cc__skip_ws_codegen(src, handler_e, rpar + 1);
    if (body_s + 1 >= handler_e || src[body_s] != '=' || src[body_s + 1] != '>') return NULL;
    body_s = cc__skip_ws_codegen(src, handler_e, body_s + 2);
    cc__trim_range_codegen(src, &body_s, &body_e);
    if (body_s >= body_e) return NULL;
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "static CCSlice ");
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, lambda_name);
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "(");
    for (int i = 0; i < 6; ++i) {
        if (i) cc__cg_sb_append_cstr(&out, &out_len, &out_cap, ", ");
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, param_types[i]);
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, " ");
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, params[i]);
    }
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, ") ");
    if (src[body_s] == '{') {
        size_t body_r = 0;
        if (!cc__find_matching_brace_codegen(src, handler_e, body_s, &body_r)) {
            free(out);
            return NULL;
        }
        cc__cg_sb_append(&out, &out_len, &out_cap, src + body_s, body_r + 1 - body_s);
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "\n");
        return out;
    }
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "{ return ");
    cc__cg_sb_append(&out, &out_len, &out_cap, src + body_s, body_e - body_s);
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "; }\n");
    return out;
}

static int cc__compile_ufcs_handler_host_backend(const char* input_path,
                                                 const char* original_src,
                                                 size_t original_len,
                                                 const char* symbol_stem,
                                                 const char* callable_name,
                                                 const char* extra_defs,
                                                 int include_top_level_defs,
                                                 int source_is_include_expanded,
                                                 void** out_owner,
                                                 const void** out_fn_ptr) {
    char* blanked_src = NULL;
    char* pp_src = NULL;
    char* tu_src = NULL;
    char err_buf[1024] = {0};
    char entry_name[256];
    char input_dir[1024];
    char repo_root[1024];
    char tmp_base[] = "/tmp/cc_comptime_ufcs_XXXXXX";
    char compile_cmd[4096];
    CCComptimeDlModule* module = NULL;
    CCTypeRegistry* saved_reg = NULL;
    CCTypeRegistry* temp_reg = NULL;
    int rc = -1;
    int used_lowered_header = 0;
    if (!input_path || !original_src || !symbol_stem || !callable_name || !out_owner || !out_fn_ptr) return -1;
    *out_owner = NULL;
    *out_fn_ptr = NULL;
    saved_reg = cc_type_registry_get_global();
    temp_reg = cc_type_registry_new();
    if (temp_reg) cc_type_registry_set_global(temp_reg);
    repo_root[0] = '\0';
    (void)cc__find_repo_root_codegen(input_path, repo_root, sizeof(repo_root));
    blanked_src = cc__blank_comptime_blocks_preserve_layout(original_src, original_len);
    if (!blanked_src) goto done;
    {
        char* lowered_includes = cc_rewrite_local_cch_includes_to_lowered_headers(blanked_src, strlen(blanked_src), input_path);
        if (lowered_includes) {
            free(blanked_src);
            blanked_src = lowered_includes;
        }
    }
    if (input_path) {
        size_t path_len = strlen(input_path);
        if (path_len >= 4 && strcmp(input_path + path_len - 4, ".cch") == 0) {
            pp_src = cc_lower_header_string(blanked_src, strlen(blanked_src), input_path);
            used_lowered_header = (pp_src != NULL);
        }
    }
    if (!pp_src) {
        pp_src = cc_preprocess_to_string_ex(blanked_src, strlen(blanked_src), input_path, 1);
    }
    if (!pp_src) goto done;
    snprintf(entry_name, sizeof(entry_name), "__cc_comptime_ufcs_%s", symbol_stem);
    if (used_lowered_header) {
        size_t tu_len = 0, tu_cap = 0;
        cc__cg_sb_append_cstr(&tu_src, &tu_len, &tu_cap, "#ifndef __CC__\n");
        cc__cg_sb_append_cstr(&tu_src, &tu_len, &tu_cap, "#define __CC__ 1\n");
        cc__cg_sb_append_cstr(&tu_src, &tu_len, &tu_cap, "#endif\n");
        cc__cg_sb_append_cstr(&tu_src, &tu_len, &tu_cap, pp_src);
        cc__cg_sb_append_cstr(&tu_src, &tu_len, &tu_cap, "\n");
        if (extra_defs && extra_defs[0]) {
            cc__cg_sb_append_cstr(&tu_src, &tu_len, &tu_cap, extra_defs);
            cc__cg_sb_append_cstr(&tu_src, &tu_len, &tu_cap, "\n");
        }
        cc__cg_sb_append_cstr(&tu_src, &tu_len, &tu_cap, "\nCCSlice ");
        cc__cg_sb_append_cstr(&tu_src, &tu_len, &tu_cap, entry_name);
        cc__cg_sb_append_cstr(&tu_src, &tu_len, &tu_cap,
                              "(CCSlice recv_type, CCSlice method, CCSlice mode, CCSliceArray argv, CCSliceArray arg_types, CCArena *arena) {\n    return ");
        cc__cg_sb_append_cstr(&tu_src, &tu_len, &tu_cap, callable_name);
        cc__cg_sb_append_cstr(&tu_src, &tu_len, &tu_cap, "(recv_type, method, mode, argv, arg_types, arena);\n}\n");
    } else {
        tu_src = cc__build_comptime_tu_from_preprocessed(pp_src, strlen(pp_src),
                                                         repo_root[0] ? repo_root : NULL,
                                                         extra_defs, entry_name, callable_name,
                                                         include_top_level_defs,
                                                         source_is_include_expanded);
    }
    if (!tu_src) goto done;
    cc__dirname_codegen(input_path, input_dir, sizeof(input_dir));
    {
        int tmp_fd = mkstemp(tmp_base);
        if (tmp_fd < 0) goto done;
        close(tmp_fd);
    }
    unlink(tmp_base);
    module = (CCComptimeDlModule*)calloc(1, sizeof(*module));
    if (!module) goto done;
    snprintf(module->obj_path, sizeof(module->obj_path), "%s.c", tmp_base);
    snprintf(module->dylib_path, sizeof(module->dylib_path), "%s.dylib", tmp_base);
    {
        FILE* srcf = fopen(module->obj_path, "w");
        if (!srcf) {
            snprintf(err_buf, sizeof(err_buf), "failed to write comptime source");
            goto done;
        }
        fputs(tu_src, srcf);
        fclose(srcf);
    }
    if (cc__format_host_comptime_compile_cmd(compile_cmd, sizeof(compile_cmd),
                                             repo_root[0] ? repo_root : NULL,
                                             input_dir[0] ? input_dir : NULL,
                                             module->dylib_path,
                                             module->obj_path) != 0) {
        snprintf(err_buf, sizeof(err_buf), "failed to format host comptime compile command");
        goto done;
    }
    if (system(compile_cmd) != 0) {
        snprintf(err_buf, sizeof(err_buf), "host comptime compile failed");
        goto done;
    }
    module->dl_handle = dlopen(module->dylib_path, RTLD_NOW | RTLD_LOCAL);
    if (!module->dl_handle) {
        snprintf(err_buf, sizeof(err_buf), "dlopen failed: %s", dlerror() ? dlerror() : "unknown error");
        goto done;
    }
    *out_fn_ptr = dlsym(module->dl_handle, entry_name);
    if (!*out_fn_ptr) {
        snprintf(err_buf, sizeof(err_buf), "dlsym failed: %s", dlerror() ? dlerror() : "missing symbol");
        goto done;
    }
    *out_owner = module;
    module = NULL;
    rc = 0;
done:
    if (tu_src && getenv("CC_DEBUG_COMPTIME_UFCS_DUMP")) {
        FILE* dbg = fopen(getenv("CC_DEBUG_COMPTIME_UFCS_DUMP"), "w");
        if (dbg) {
            fputs(tu_src, dbg);
            fclose(dbg);
        }
    }
    if (rc != 0 && err_buf[0]) {
        fprintf(stderr, "%s: error: comptime UFCS compile failed for '%s': %s\n",
                input_path, symbol_stem, err_buf);
    }
    if (module) cc__dl_module_free(module);
    free(blanked_src);
    free(pp_src);
    free(tu_src);
    if (temp_reg) {
        cc_type_registry_set_global(saved_reg);
        cc_type_registry_free(temp_reg);
    }
    return rc;
}

static int cc__compile_named_ufcs_handler(const char* input_path,
                                          const char* original_src,
                                          size_t original_len,
                                          const char* handler_name,
                                          int source_is_include_expanded,
                                          void** out_owner,
                                          const void** out_fn_ptr) {
    return cc__compile_ufcs_handler_host_backend(input_path, original_src, original_len,
                                                 handler_name, handler_name, NULL, 1,
                                                 source_is_include_expanded,
                                                 out_owner, out_fn_ptr);
}

static int cc__compile_lambda_ufcs_handler(const char* input_path,
                                           const char* original_src,
                                           size_t original_len,
                                           size_t handler_s,
                                           size_t handler_e,
                                           int source_is_include_expanded,
                                           void** out_owner,
                                           const void** out_fn_ptr) {
    char lambda_name[128];
    char* lambda_def = NULL;
    int rc = -1;
    snprintf(lambda_name, sizeof(lambda_name), "__cc_ufcs_lambda_%zu", handler_s);
    lambda_def = cc__build_lambda_handler_definition_codegen(original_src, original_len,
                                                            handler_s, handler_e, lambda_name);
    if (!lambda_def) return -1;
    rc = cc__compile_ufcs_handler_host_backend(input_path, original_src, original_len,
                                               lambda_name, lambda_name, lambda_def, 0,
                                               source_is_include_expanded,
                                               out_owner, out_fn_ptr);
    free(lambda_def);
    return rc;
}

static char* cc__build_type_ufcs_lambda_definition(const char* expr_src,
                                                   size_t expr_len,
                                                   const char* lambda_name) {
    static const char* param_types[6] = {
        "CCSlice", "CCSlice", "CCSlice", "CCSliceArray", "CCSliceArray", "CCArena *"
    };
    char params[6][64];
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t lpar = 0, rpar = 0, p = 0;
    size_t body_s = 0, body_e = expr_len;
    int param_count = 0;
    if (!expr_src || !lambda_name || expr_len == 0) return NULL;
    p = cc__skip_ws_codegen(expr_src, expr_len, 0);
    if (p >= expr_len || expr_src[p] != '(') return NULL;
    lpar = p;
    if (!cc__find_matching_paren_codegen(expr_src, expr_len, lpar, &rpar)) return NULL;
    p = lpar + 1;
    while (p < rpar) {
        p = cc__skip_ws_codegen(expr_src, rpar, p);
        if (p >= rpar) break;
        if (param_count >= 6 || !cc__parse_ident_codegen(expr_src, rpar, &p, params[param_count], sizeof(params[param_count]))) {
            return NULL;
        }
        param_count++;
        p = cc__skip_ws_codegen(expr_src, rpar, p);
        if (p < rpar) {
            if (expr_src[p] != ',') return NULL;
            p++;
        }
    }
    if (param_count != 6) return NULL;
    body_s = cc__skip_ws_codegen(expr_src, expr_len, rpar + 1);
    if (body_s + 1 >= expr_len || expr_src[body_s] != '=' || expr_src[body_s + 1] != '>') return NULL;
    body_s = cc__skip_ws_codegen(expr_src, expr_len, body_s + 2);
    cc__trim_range_codegen(expr_src, &body_s, &body_e);
    if (body_s >= body_e) return NULL;

    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "static CCSlice ");
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, lambda_name);
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "(");
    for (int i = 0; i < 6; ++i) {
        if (i) cc__cg_sb_append_cstr(&out, &out_len, &out_cap, ", ");
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, param_types[i]);
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, " ");
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, params[i]);
    }
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, ") ");
    if (expr_src[body_s] == '{') {
        size_t body_r = 0;
        if (!cc__find_matching_brace_codegen(expr_src, expr_len, body_s, &body_r)) {
            free(out);
            return NULL;
        }
        cc__cg_sb_append(&out, &out_len, &out_cap, expr_src + body_s, body_r + 1 - body_s);
        cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "\n");
        return out;
    }
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "{ return ");
    cc__cg_sb_append(&out, &out_len, &out_cap, expr_src + body_s, body_e - body_s);
    cc__cg_sb_append_cstr(&out, &out_len, &out_cap, "; }\n");
    return out;
}

static int cc__compile_lambda_ufcs_handler_expr(const char* input_path,
                                                const char* original_src,
                                                size_t original_len,
                                                const char* expr_src,
                                                size_t expr_len,
                                                int source_is_include_expanded,
                                                void** out_owner,
                                                const void** out_fn_ptr) {
    char lambda_name[128];
    char* lambda_def = NULL;
    int rc = -1;
    if (!input_path || !original_src || !expr_src || expr_len == 0 || !out_owner || !out_fn_ptr) return -1;
    snprintf(lambda_name, sizeof(lambda_name), "__cc_ufcs_lambda_expr_%zu", expr_len);
    lambda_def = cc__build_type_ufcs_lambda_definition(expr_src, expr_len, lambda_name);
    if (!lambda_def) return -1;
    rc = cc__compile_ufcs_handler_host_backend(input_path, original_src, original_len,
                                               lambda_name, lambda_name, lambda_def, 0,
                                               source_is_include_expanded,
                                               out_owner, out_fn_ptr);
    free(lambda_def);
    return rc;
}

typedef struct {
    const char* registration_src;
    size_t registration_n;
    const char* compile_src;
    size_t compile_n;
    int compile_src_is_include_expanded;
} CCTypeUfcsCompileCtx;

static int cc__compile_type_ufcs_registration(CCSymbolTable* symbols,
                                              const char* registration_input_path,
                                              const char* logical_file,
                                              const char* type_name,
                                              const char* expr_src,
                                              size_t expr_len,
                                              void* user_ctx) {
    CCTypeUfcsCompileCtx* ctx = (CCTypeUfcsCompileCtx*)user_ctx;
    const char* handler_src = (ctx && ctx->compile_src) ? ctx->compile_src : expr_src;
    size_t handler_src_len = (ctx && ctx->compile_src) ? ctx->compile_n : expr_len;
    const char* handler_input_path = registration_input_path;
    int handler_src_is_include_expanded = ctx ? ctx->compile_src_is_include_expanded : 0;
    char handler[128];
    size_t p = 0;
    int handler_is_ident = 0;
    char* logical_src = NULL;
    size_t logical_len = 0;
    void* owner = NULL;
    const void* fn_ptr = NULL;

    if (!symbols || !type_name || !expr_src || expr_len == 0) return -1;
    if (logical_file && logical_file[0] && registration_input_path &&
        strcmp(logical_file, registration_input_path) != 0) {
        cc__read_entire_file(logical_file, &logical_src, &logical_len);
        if (logical_src && logical_len > 0) {
            handler_src = logical_src;
            handler_src_len = logical_len;
            handler_input_path = logical_file;
            handler_src_is_include_expanded = 0;
        }
    }

    if (cc__parse_ident_codegen(expr_src, expr_len, &p, handler, sizeof(handler)) && p == expr_len) {
        handler_is_ident = 1;
    }
    if (handler_is_ident) {
        if (cc__compile_named_ufcs_handler(handler_input_path,
                                           handler_src,
                                           handler_src_len,
                                           handler,
                                           handler_src_is_include_expanded,
                                           &owner,
                                           &fn_ptr) != 0) {
            free(logical_src);
            fprintf(stderr, "%s: error: unsupported comptime UFCS handler '%s'; expected a plain named function or non-capturing lambda compilable in the comptime subset\n",
                    registration_input_path ? registration_input_path : "<input>", handler);
            return -1;
        }
    } else {
        const char* lambda_src = (ctx && ctx->registration_src) ? ctx->registration_src : handler_src;
        size_t lambda_src_len = (ctx && ctx->registration_src) ? ctx->registration_n : handler_src_len;
        if (cc__compile_lambda_ufcs_handler_expr(registration_input_path,
                                                 lambda_src,
                                                 lambda_src_len,
                                                 expr_src,
                                                 expr_len,
                                                 handler_src_is_include_expanded,
                                                 &owner,
                                                 &fn_ptr) != 0) {
            free(logical_src);
            fprintf(stderr, "%s: error: unsupported comptime UFCS lambda; expected a non-capturing lambda compilable in the comptime subset\n",
                    registration_input_path ? registration_input_path : "<input>");
            return -1;
        }
    }
    free(logical_src);
    if (cc_symbols_set_type_ufcs_callable(symbols, type_name, fn_ptr, owner, cc__dl_module_free) != 0) {
        fprintf(stderr, "%s: error: failed to record callable UFCS registration for '%s'\n",
                registration_input_path ? registration_input_path : "<input>", type_name);
        cc__dl_module_free(owner);
        return -1;
    }
    return 0;
}

static int cc__collect_comptime_ufcs_registrations(CCSymbolTable* symbols,
                                                   const char* input_path,
                                                   const char* src,
                                                   size_t n,
                                                   const char* compile_src,
                                                   size_t compile_n,
                                                   int compile_src_is_include_expanded,
                                                   int headers_only) {
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0, line_start = 1;
    char logical_file[1024] = {0};
    if (!symbols || !src) return 0;
    for (size_t i = 0; i < n; ++i) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (line_start && c == '#') {
            size_t p = i + 1;
            while (p < n && isspace((unsigned char)src[p]) && src[p] != '\n') p++;
            while (p < n && isdigit((unsigned char)src[p])) p++;
            while (p < n && isspace((unsigned char)src[p]) && src[p] != '\n') p++;
            if (p < n && src[p] == '"') {
                size_t q = p + 1;
                size_t out = 0;
                while (q < n && src[q] && src[q] != '"' && out + 1 < sizeof(logical_file)) {
                    logical_file[out++] = src[q++];
                }
                logical_file[out] = '\0';
            }
            while (i < n && src[i] != '\n') i++;
            line_start = 1;
            continue;
        }
        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; } continue; }
        if (in_str) { if (c == '\\' && c2) { i++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && c2) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        line_start = (c == '\n');
        if (c != '@' || !cc__match_keyword_codegen(src, n, i + 1, "comptime")) continue;
        if (headers_only && logical_file[0] && input_path && strcmp(logical_file, input_path) == 0) continue;
        {
            size_t kw_end = i + 1 + strlen("comptime");
            size_t body_l = cc__skip_ws_codegen(src, n, kw_end);
            size_t body_r;
            if (body_l >= n || src[body_l] != '{') continue;
            if (!cc__find_matching_brace_codegen(src, n, body_l, &body_r)) continue;
            for (size_t j = body_l + 1; j < body_r; ++j) {
                if (!cc__match_keyword_codegen(src, body_r, j, "cc_ufcs_register")) continue;
                size_t lpar = cc__skip_ws_codegen(src, body_r, j + strlen("cc_ufcs_register"));
                size_t rpar, p;
                char pattern[128];
                char handler[128];
                size_t handler_s = 0, handler_e = 0;
                int handler_is_ident = 0;
                if (lpar >= body_r || src[lpar] != '(') continue;
                if (!cc__find_matching_paren_codegen(src, body_r, lpar, &rpar)) continue;
                p = cc__skip_ws_codegen(src, body_r, lpar + 1);
                if (!cc__parse_string_literal_codegen(src, body_r, &p, pattern, sizeof(pattern))) {
                    fprintf(stderr, "%s: error: unsupported @comptime cc_ufcs_register pattern form\n",
                            input_path ? input_path : "<input>");
                    return -1;
                }
                p = cc__skip_ws_codegen(src, body_r, p);
                if (p >= body_r || src[p] != ',') {
                    fprintf(stderr, "%s: error: malformed cc_ufcs_register(...) in @comptime block\n",
                            input_path ? input_path : "<input>");
                    return -1;
                }
                p = cc__skip_ws_codegen(src, body_r, p + 1);
                handler_s = p;
                if (cc__parse_ident_codegen(src, body_r, &p, handler, sizeof(handler))) {
                    handler_is_ident = 1;
                    handler_e = p;
                } else {
                    handler_e = rpar;
                }
                if (handler_is_ident) {
                    void* owner = NULL;
                    const void* fn_ptr = NULL;
                    const char* handler_src = compile_src ? compile_src : src;
                    size_t handler_src_len = compile_src ? compile_n : n;
                    const char* handler_input_path = input_path;
                    int handler_src_is_include_expanded = compile_src_is_include_expanded;
                    char* logical_src = NULL;
                    if (logical_file[0] && input_path && strcmp(logical_file, input_path) != 0) {
                        size_t logical_len = 0;
                        cc__read_entire_file(logical_file, &logical_src, &logical_len);
                        if (logical_src && logical_len > 0) {
                            handler_src = logical_src;
                            handler_src_len = logical_len;
                            handler_input_path = logical_file;
                            handler_src_is_include_expanded = 0;
                        }
                    }
                    if (cc__compile_named_ufcs_handler(handler_input_path,
                                                       handler_src,
                                                       handler_src_len,
                                                       handler,
                                                       handler_src_is_include_expanded,
                                                       &owner,
                                                       &fn_ptr) != 0) {
                        free(logical_src);
                        fprintf(stderr, "%s: error: unsupported comptime UFCS handler '%s'; expected a plain named function compilable in the comptime subset\n",
                                input_path ? input_path : "<input>", handler);
                        return -1;
                    }
                    free(logical_src);
                    if (cc_symbols_add_legacy_type_ufcs_callable(symbols, pattern, fn_ptr, owner, cc__dl_module_free) != 0) {
                        fprintf(stderr, "%s: error: failed to record callable UFCS registration for '%s'\n",
                                input_path ? input_path : "<input>", pattern);
                        cc__dl_module_free(owner);
                        return -1;
                    }
                } else {
                    void* owner = NULL;
                    const void* fn_ptr = NULL;
                    cc__trim_range_codegen(src, &handler_s, &handler_e);
                    if (cc__compile_lambda_ufcs_handler(input_path,
                                                        src,
                                                        n,
                                                        handler_s,
                                                        handler_e,
                                                        compile_src_is_include_expanded,
                                                        &owner,
                                                        &fn_ptr) != 0) {
                        fprintf(stderr, "%s: error: unsupported comptime UFCS lambda; expected a non-capturing lambda compilable in the comptime subset\n",
                                input_path ? input_path : "<input>");
                        return -1;
                    }
                    if (cc_symbols_add_legacy_type_ufcs_callable(symbols, pattern, fn_ptr, owner, cc__dl_module_free) != 0) {
                        fprintf(stderr, "%s: error: failed to record callable UFCS registration for '%s'\n",
                                input_path ? input_path : "<input>", pattern);
                        cc__dl_module_free(owner);
                        return -1;
                    }
                }
                j = rpar;
            }
            i = body_r;
        }
    }
    return 0;
}


static char* cc__blank_comptime_blocks_preserve_layout(const char* src, size_t n) {
    char* out = NULL;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    if (!src) return NULL;
    out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n);
    out[n] = '\0';
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
        if (c != '@' || !cc__match_keyword_codegen(src, n, i + 1, "comptime")) continue;
        {
            size_t kw_end = i + 1 + strlen("comptime");
            size_t body_l = cc__skip_ws_codegen(src, n, kw_end);
            size_t body_r;
            if (body_l >= n || src[body_l] != '{') continue;
            if (!cc__find_matching_brace_codegen(src, n, body_l, &body_r)) continue;
            for (size_t k = i; k <= body_r; ++k) {
                if (out[k] != '\n') out[k] = ' ';
            }
            i = body_r;
        }
    }
    return out;
}

static void cc__register_ufcs_declared_vars_for_type(CCTypeRegistry* reg,
                                                     const char* type_name,
                                                     const char* src,
                                                     size_t n) {
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    size_t type_len = type_name ? strlen(type_name) : 0;
    if (!reg || !type_name || !type_len || !src) return;
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
        if (!cc__match_keyword_codegen(src, n, i, type_name)) continue;
        {
            size_t p = cc__skip_ws_codegen(src, n, i + type_len);
            while (p < n && src[p] == '*') p++;
            p = cc__skip_ws_codegen(src, n, p);
            if (p < n && (isalpha((unsigned char)src[p]) || src[p] == '_')) {
                char var_name[128];
                const char* final_type_name = type_name;
                size_t v = p;
                size_t vn = 0;
                while (v < n && (isalnum((unsigned char)src[v]) || src[v] == '_')) {
                    if (vn + 1 < sizeof(var_name)) var_name[vn] = src[v];
                    vn++;
                    v++;
                }
                var_name[vn < sizeof(var_name) ? vn : sizeof(var_name) - 1] = '\0';
                v = cc__skip_ws_codegen(src, n, v);
                if (v < n && src[v] == '(') continue;
                if ((strcmp(type_name, "CCChanTx") == 0 || strcmp(type_name, "CCChanRx") == 0) &&
                    v < n && src[v] == '=') {
                    size_t rhs = cc__skip_ws_codegen(src, n, v + 1);
                    if (rhs < n && (isalpha((unsigned char)src[rhs]) || src[rhs] == '_')) {
                        char rhs_name[128];
                        size_t rn = 0;
                        size_t r = rhs;
                        while (r < n && (isalnum((unsigned char)src[r]) || src[r] == '_')) {
                            if (rn + 1 < sizeof(rhs_name)) rhs_name[rn] = src[r];
                            rn++;
                            r++;
                        }
                        rhs_name[rn < sizeof(rhs_name) ? rn : sizeof(rhs_name) - 1] = '\0';
                        if (rhs_name[0]) {
                            const char* rhs_type_name = cc_type_registry_lookup_var(reg, rhs_name);
                            if (rhs_type_name &&
                                ((strcmp(type_name, "CCChanTx") == 0 && strncmp(rhs_type_name, "CCChanTx_", 9) == 0) ||
                                 (strcmp(type_name, "CCChanRx") == 0 && strncmp(rhs_type_name, "CCChanRx_", 9) == 0))) {
                                final_type_name = rhs_type_name;
                            }
                        }
                    }
                }
                cc_type_registry_add_var(reg, var_name, final_type_name);
            }
        }
        i += type_len ? (type_len - 1) : 0;
    }
}

static void cc__collect_registered_ufcs_var_types(CCSymbolTable* symbols, const char* src, size_t n) {
    CCTypeRegistry* reg = cc_type_registry_get_global();
    if (!symbols || !reg || !src) return;
    cc__collect_ufcs_field_and_var_types(src, n);
    for (size_t ti = 0; ti < cc_symbols_type_count(symbols); ++ti) {
        const char* type_name = cc_symbols_type_name(symbols, ti);
        if (!type_name) continue;
        /* Text fallback UFCS needs declared-variable types even for simple
           value-lowered hooks like `CCNursery* -> wait/free/cancel`, not just
           callable registrations. */
        cc__register_ufcs_declared_vars_for_type(reg, type_name, src, n);
    }
}

/* cc__parse_decl_name_and_type_codegen — now delegated to cc_parse_decl_name_and_type in util/text.h */

/* cc__is_non_decl_stmt_type_codegen — now cc_is_non_decl_stmt_type in util/text.h */

static int cc__parse_decl_name_and_type_fallback_codegen(const char* stmt_start,
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
    while (s < e && isspace((unsigned char)*s)) s++;
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

static int cc__parse_typedef_alias_stmt_codegen(const char* stmt_start,
                                                const char* stmt_end,
                                                char* alias_name,
                                                size_t alias_name_sz,
                                                char* alias_type,
                                                size_t alias_type_sz) {
    const char* s = stmt_start;
    const char* e = stmt_end;
    const char* alias_end;
    const char* alias_start;
    const char* type_start;
    const char* type_end;
    if (!stmt_start || !stmt_end || stmt_end <= stmt_start) return 0;
    if (!alias_name || alias_name_sz == 0 || !alias_type || alias_type_sz == 0) return 0;
    alias_name[0] = '\0';
    alias_type[0] = '\0';
    while (s < e && isspace((unsigned char)*s)) s++;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    if (e <= s || (size_t)(e - s) < 7 || memcmp(s, "typedef", 7) != 0) return 0;
    type_start = s + 7;
    while (type_start < e && isspace((unsigned char)*type_start)) type_start++;
    alias_end = e;
    while (alias_end > type_start && isspace((unsigned char)alias_end[-1])) alias_end--;
    alias_start = alias_end;
    while (alias_start > type_start && cc_is_ident_char(alias_start[-1])) alias_start--;
    if (alias_start == alias_end || !cc_is_ident_start(*alias_start)) return 0;
    type_end = alias_start;
    while (type_end > type_start && isspace((unsigned char)type_end[-1])) type_end--;
    if (type_end <= type_start) return 0;
    {
        size_t alias_len = (size_t)(alias_end - alias_start);
        size_t type_len = (size_t)(type_end - type_start);
        if (alias_len >= alias_name_sz) alias_len = alias_name_sz - 1;
        if (type_len >= alias_type_sz) type_len = alias_type_sz - 1;
        memcpy(alias_name, alias_start, alias_len);
        alias_name[alias_len] = '\0';
        memcpy(alias_type, type_start, type_len);
        alias_type[type_len] = '\0';
    }
    return 1;
}

static void cc__trim_type_span_codegen(const char** start, const char** end) {
    while (*start < *end && isspace((unsigned char)**start)) (*start)++;
    while (*end > *start && isspace((unsigned char)(*end)[-1])) (*end)--;
}

static void cc__normalize_decl_type_for_receiver_codegen(char* out, size_t out_sz, const char* type_name) {
    CCTypeRegistry* reg = cc_type_registry_get_global();
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
    type_name = cc__canonicalize_string_type_codegen(type_name);
    bang = strchr(type_name, '!');
    if (!bang || bang[1] == '=') {
        strncpy(out, type_name, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    ok_s = type_name;
    ok_e = bang;
    cc__trim_type_span_codegen(&ok_s, &ok_e);
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
        while (*err_e && (isalnum((unsigned char)*err_e) || *err_e == '_')) err_e++;
    }
    cc__trim_type_span_codegen(&err_s, &err_e);
    if (ok_e <= ok_s || err_e <= err_s) {
        strncpy(out, type_name, out_sz - 1);
        out[out_sz - 1] = '\0';
        if (reg) cc_type_registry_canonicalize_type_name(reg, out, out, out_sz);
        return;
    }
    cc_result_spec_mangle_type(ok_s, (size_t)(ok_e - ok_s), mangled_ok, sizeof(mangled_ok));
    cc_result_spec_mangle_type(err_s, (size_t)(err_e - err_s), mangled_err, sizeof(mangled_err));
    if (!mangled_ok[0] || !mangled_err[0]) {
        strncpy(out, type_name, out_sz - 1);
        out[out_sz - 1] = '\0';
        if (reg) cc_type_registry_canonicalize_type_name(reg, out, out, out_sz);
        return;
    }
    snprintf(out, out_sz, "CCResult_%s_%s", mangled_ok, mangled_err);
    if (reg) cc_type_registry_canonicalize_type_name(reg, out, out, out_sz);
}

static const char* cc__lookup_scoped_local_var_type_codegen(const char* src,
                                                            size_t limit,
                                                            const char* var_name,
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
    if (!src || !var_name || !var_name[0] || !out_type || out_type_sz == 0) return NULL;
    out_type[0] = '\0';
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
            cc_parse_decl_name_and_type(src + stmt_start, src + i,
                                                 decl_name, sizeof(decl_name),
                                                 decl_type, sizeof(decl_type));
            if (!decl_name[0] || strcmp(decl_name, var_name) != 0 || !decl_type[0]) {
                (void)cc__parse_decl_name_and_type_fallback_codegen(src + stmt_start, src + i,
                                                                    decl_name, sizeof(decl_name),
                                                                    decl_type, sizeof(decl_type));
            }
            if (decl_name[0] &&
                strcmp(decl_name, var_name) == 0 &&
                !cc_is_non_decl_stmt_type(decl_type) &&
                decl_count < MAX_DECLS) {
                decls[decl_count].scope_id = scope_stack[scope_depth - 1];
                cc__normalize_decl_type_for_receiver_codegen(decls[decl_count].type_name,
                                                             sizeof(decls[decl_count].type_name),
                                                             decl_type);
                decl_count++;
            }
            stmt_start = i + 1;
        }
        i++;
    }
    if (decl_count == 0) {
        return cc__lookup_enclosing_param_type_codegen(src, limit, var_name, out_type, out_type_sz);
    }
    strncpy(out_type, decls[decl_count - 1].type_name, out_type_sz - 1);
    out_type[out_type_sz - 1] = '\0';
    {
        const char* canon = cc__canonicalize_string_type_codegen(out_type);
        if (canon != out_type) {
            strncpy(out_type, canon, out_type_sz - 1);
            out_type[out_type_sz - 1] = '\0';
        }
    }
    return out_type;
}

static const char* cc__lookup_enclosing_param_type_codegen(const char* src,
                                                           size_t limit,
                                                           const char* var_name,
                                                           char* out_type,
                                                           size_t out_type_sz) {
    size_t i = 0;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    if (!src || !var_name || !var_name[0] || !out_type || out_type_sz == 0) return NULL;
    out_type[0] = '\0';
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
        if (c == '{') {
            size_t rpar = i;
            size_t lpar;
            size_t name_end;
            size_t name_start;
            char fn_name[64];
            size_t cursor;
            while (rpar > 0 && isspace((unsigned char)src[rpar - 1])) rpar--;
            if (rpar == 0 || src[rpar - 1] != ')') { i++; continue; }
            rpar--;
            lpar = rpar;
            {
                int depth = 1;
                while (lpar > 0) {
                    lpar--;
                    if (src[lpar] == ')') depth++;
                    else if (src[lpar] == '(') {
                        depth--;
                        if (depth == 0) break;
                    }
                }
                if (src[lpar] != '(') { i++; continue; }
            }
            name_end = lpar;
            while (name_end > 0 && isspace((unsigned char)src[name_end - 1])) name_end--;
            name_start = name_end;
            while (name_start > 0 &&
                   (isalnum((unsigned char)src[name_start - 1]) || src[name_start - 1] == '_')) {
                name_start--;
            }
            if (name_end <= name_start || name_end - name_start >= sizeof(fn_name)) { i++; continue; }
            memcpy(fn_name, src + name_start, name_end - name_start);
            fn_name[name_end - name_start] = '\0';
            if (strcmp(fn_name, "if") == 0 || strcmp(fn_name, "for") == 0 ||
                strcmp(fn_name, "while") == 0 || strcmp(fn_name, "switch") == 0) {
                i++;
                continue;
            }
            cursor = lpar + 1;
            while (cursor < rpar) {
                size_t param_s = cc__skip_ws_codegen(src, limit, cursor);
                size_t param_e = param_s;
                int par = 0, br = 0, brc = 0;
                char decl_name[128];
                char decl_type[256];
                if (param_s >= rpar) break;
                while (param_e < rpar) {
                    char pc = src[param_e];
                    if (pc == '(') par++;
                    else if (pc == ')' && par > 0) par--;
                    else if (pc == '[') br++;
                    else if (pc == ']' && br > 0) br--;
                    else if (pc == '{') brc++;
                    else if (pc == '}' && brc > 0) brc--;
                    else if (pc == ',' && par == 0 && br == 0 && brc == 0) break;
                    param_e++;
                }
                while (param_e > param_s && isspace((unsigned char)src[param_e - 1])) param_e--;
                if (param_e > param_s) {
                    cc_parse_decl_name_and_type(src + param_s, src + param_e,
                                                decl_name, sizeof(decl_name),
                                                decl_type, sizeof(decl_type));
                    if (!decl_name[0]) {
                        (void)cc__parse_decl_name_and_type_fallback_codegen(src + param_s, src + param_e,
                                                                            decl_name, sizeof(decl_name),
                                                                            decl_type, sizeof(decl_type));
                    }
                    if (decl_name[0] && strcmp(decl_name, var_name) == 0 &&
                        decl_type[0] && strcmp(decl_type, "void") != 0 &&
                        !cc_is_non_decl_stmt_type(decl_type)) {
                        cc__normalize_decl_type_for_receiver_codegen(out_type, out_type_sz, decl_type);
                        return out_type;
                    }
                }
                cursor = param_e;
                if (cursor < rpar && src[cursor] == ',') cursor++;
            }
        }
        i++;
    }
    return NULL;
}

static void cc__record_function_params_before_brace_codegen(CCTypeRegistry* reg,
                                                            const char* src,
                                                            size_t brace_pos) {
    size_t rpar;
    size_t lpar;
    size_t name_end;
    size_t name_start;
    char fn_name[64];
    size_t cursor;
    if (!reg || !src || brace_pos == 0) return;
    rpar = brace_pos;
    while (rpar > 0 && isspace((unsigned char)src[rpar - 1])) rpar--;
    if (rpar == 0 || src[rpar - 1] != ')') return;
    rpar--;
    lpar = rpar;
    {
        int depth = 1;
        while (lpar > 0) {
            lpar--;
            if (src[lpar] == ')') depth++;
            else if (src[lpar] == '(') {
                depth--;
                if (depth == 0) break;
            }
        }
        if (src[lpar] != '(') return;
    }
    name_end = lpar;
    while (name_end > 0 && isspace((unsigned char)src[name_end - 1])) name_end--;
    name_start = name_end;
    while (name_start > 0 &&
           (isalnum((unsigned char)src[name_start - 1]) || src[name_start - 1] == '_')) {
        name_start--;
    }
    if (name_end <= name_start || name_end - name_start >= sizeof(fn_name)) return;
    memcpy(fn_name, src + name_start, name_end - name_start);
    fn_name[name_end - name_start] = '\0';
    if (strcmp(fn_name, "if") == 0 || strcmp(fn_name, "for") == 0 ||
        strcmp(fn_name, "while") == 0 || strcmp(fn_name, "switch") == 0) {
        return;
    }
    cursor = lpar + 1;
    while (cursor < rpar) {
        size_t param_s = cc__skip_ws_codegen(src, brace_pos, cursor);
        size_t param_e = param_s;
        int par = 0, br = 0, brc = 0;
        char decl_name[128];
        char decl_type[256];
        if (param_s >= rpar) break;
        while (param_e < rpar) {
            char c = src[param_e];
            if (c == '(') par++;
            else if (c == ')' && par > 0) par--;
            else if (c == '[') br++;
            else if (c == ']' && br > 0) br--;
            else if (c == '{') brc++;
            else if (c == '}' && brc > 0) brc--;
            else if (c == ',' && par == 0 && br == 0 && brc == 0) break;
            param_e++;
        }
        while (param_e > param_s && isspace((unsigned char)src[param_e - 1])) param_e--;
        if (param_e > param_s) {
            cc_parse_decl_name_and_type(src + param_s, src + param_e,
                                        decl_name, sizeof(decl_name),
                                        decl_type, sizeof(decl_type));
            if (!decl_name[0]) {
                (void)cc__parse_decl_name_and_type_fallback_codegen(src + param_s, src + param_e,
                                                                    decl_name, sizeof(decl_name),
                                                                    decl_type, sizeof(decl_type));
            }
            if (decl_name[0] && decl_type[0] && strcmp(decl_type, "void") != 0 &&
                !cc_is_non_decl_stmt_type(decl_type)) {
                char canonical_type[256];
                if (cc_type_registry_canonicalize_type_name(reg, decl_type,
                                                            canonical_type, sizeof(canonical_type))) {
                    cc_type_registry_add_var(reg, decl_name, canonical_type);
                } else {
                    cc_type_registry_add_var(reg, decl_name, decl_type);
                }
            }
        }
        cursor = param_e;
        if (cursor < rpar && src[cursor] == ',') cursor++;
    }
}

static char* cc__rewrite_result_helper_family_to_visible_type(const char* src, size_t n) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    size_t last_emit = 0;
    int changed = 0;
    if (!src || n == 0) return NULL;
    while (i < n) {
        size_t ident_start;
        size_t ident_end;
        size_t ident_len;
        size_t suffix_len = 0;
        size_t paren_open;
        size_t paren_end = 0;
        const char* suffix = NULL;
        char helper_type[256];
        char actual_type[256];
        char arg_name[128];
        size_t arg_start;
        size_t arg_end;
        if (!cc_is_ident_start(src[i])) {
            i++;
            continue;
        }
        ident_start = i;
        while (i < n && cc_is_ident_char(src[i])) i++;
        ident_end = i;
        ident_len = ident_end - ident_start;
        if (ident_len <= 9 || memcmp(src + ident_start, "CCResult_", 9) != 0) continue;
        paren_open = cc_skip_ws_and_comments(src, n, ident_end);
        if (paren_open >= n || src[paren_open] != '(') continue;
        if (!cc_find_matching_paren(src, n, paren_open, &paren_end)) continue;
        if (ident_len > 10 && memcmp(src + ident_end - 10, "_unwrap_or", 10) == 0) {
            suffix = "_unwrap_or";
            suffix_len = 10;
        } else if (ident_len > 7 && memcmp(src + ident_end - 7, "_unwrap", 7) == 0) {
            suffix = "_unwrap";
            suffix_len = 7;
        } else if (ident_len > 6 && memcmp(src + ident_end - 6, "_error", 6) == 0) {
            suffix = "_error";
            suffix_len = 6;
        } else {
            continue;
        }
        if (ident_len - suffix_len >= sizeof(helper_type)) continue;
        memcpy(helper_type, src + ident_start, ident_len - suffix_len);
        helper_type[ident_len - suffix_len] = '\0';
        arg_start = cc_skip_ws_and_comments(src, n, paren_open + 1);
        arg_end = arg_start;
        if (suffix_len == 10) {
            int par = 0, br = 0, brc = 0;
            while (arg_end < paren_end) {
                char c = src[arg_end];
                if (c == '(') par++;
                else if (c == ')' && par > 0) par--;
                else if (c == '[') br++;
                else if (c == ']' && br > 0) br--;
                else if (c == '{') brc++;
                else if (c == '}' && brc > 0) brc--;
                else if (c == ',' && par == 0 && br == 0 && brc == 0) break;
                arg_end++;
            }
        } else {
            arg_end = paren_end;
        }
        while (arg_end > arg_start && isspace((unsigned char)src[arg_end - 1])) arg_end--;
        if (arg_end <= arg_start || (arg_end - arg_start) >= sizeof(arg_name)) continue;
        memcpy(arg_name, src + arg_start, arg_end - arg_start);
        arg_name[arg_end - arg_start] = '\0';
        if (!cc__lookup_scoped_local_var_type_codegen(src, ident_start, arg_name, actual_type, sizeof(actual_type))) {
            continue;
        }
        if (strncmp(actual_type, "CCResult_", 9) != 0 || strcmp(actual_type, helper_type) == 0) continue;
        cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, ident_start - last_emit);
        cc__sb_append_cstr_local(&out, &out_len, &out_cap, actual_type);
        cc__sb_append_cstr_local(&out, &out_len, &out_cap, suffix);
        last_emit = ident_end;
        changed = 1;
    }
    if (!changed) {
        free(out);
        return NULL;
    }
    if (last_emit < n) cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

static void cc__trim_expr_parens_codegen(const char** start, const char** end) {
    int changed = 1;
    if (!start || !end || !*start || !*end) return;
    cc__trim_type_span_codegen(start, end);
    while (changed && *start < *end) {
        const char* s = *start;
        const char* e = *end;
        int par = 0, br = 0, brc = 0;
        int changed_this_round = 0;
        if (*s != '(' || e <= s + 1 || e[-1] != ')') break;
        for (const char* p = s + 1; p < e - 1; ++p) {
            char c = *p;
            if (c == '(') par++;
            else if (c == ')' && par > 0) par--;
            else if (c == '[') br++;
            else if (c == ']' && br > 0) br--;
            else if (c == '{') brc++;
            else if (c == '}' && brc > 0) brc--;
            else if (c == ')' && par == 0 && br == 0 && brc == 0) {
                changed_this_round = -1;
                break;
            }
        }
        if (changed_this_round < 0 || par != 0 || br != 0 || brc != 0) break;
        *start = s + 1;
        *end = e - 1;
        cc__trim_type_span_codegen(start, end);
        changed = 1;
    }
}

static const char* cc__string_helper_for_type_codegen(const char* family, const char* type_name) {
    if (!family || !type_name || !type_name[0]) return NULL;
    type_name = cc__canonicalize_string_type_codegen(type_name);
    if (strcmp(family, "CCString_from") == 0) {
        if (strcmp(type_name, "char") == 0) return "char_to_str";
        if (strcmp(type_name, "signed char") == 0) return "signed_char_to_str";
        if (strcmp(type_name, "unsigned char") == 0) return "unsigned_char_to_str";
        if (strcmp(type_name, "short") == 0) return "short_to_str";
        if (strcmp(type_name, "unsigned short") == 0) return "unsigned_short_to_str";
        if (strcmp(type_name, "int") == 0) return "int_to_str";
        if (strcmp(type_name, "unsigned") == 0) return "unsigned_to_str";
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
        if (strcmp(type_name, "size_t") == 0) return "uintptr_t_to_str";
        if (strcmp(type_name, "float") == 0) return "float_to_str";
        if (strcmp(type_name, "double") == 0) return "double_to_str";
        if (strcmp(type_name, "bool") == 0) return "bool_to_str";
        return NULL;
    }
    if (strcmp(family, "cc__string_slot_arg") == 0) {
        if (strcmp(type_name, "char") == 0) return "cc__string_slot_from_char";
        if (strcmp(type_name, "signed char") == 0) return "cc__string_slot_from_signed_char";
        if (strcmp(type_name, "unsigned char") == 0) return "cc__string_slot_from_unsigned_char";
        if (strcmp(type_name, "short") == 0) return "cc__string_slot_from_short";
        if (strcmp(type_name, "unsigned short") == 0) return "cc__string_slot_from_unsigned_short";
        if (strcmp(type_name, "int") == 0) return "cc__string_slot_from_int";
        if (strcmp(type_name, "unsigned") == 0) return "cc__string_slot_from_unsigned";
        if (strcmp(type_name, "long") == 0) return "cc__string_slot_from_long";
        if (strcmp(type_name, "unsigned long") == 0) return "cc__string_slot_from_unsigned_long";
        if (strcmp(type_name, "long long") == 0) return "cc__string_slot_from_long_long";
        if (strcmp(type_name, "unsigned long long") == 0) return "cc__string_slot_from_unsigned_long_long";
        if (strcmp(type_name, "int8_t") == 0) return "cc__string_slot_from_int8_t";
        if (strcmp(type_name, "uint8_t") == 0) return "cc__string_slot_from_uint8_t";
        if (strcmp(type_name, "int16_t") == 0) return "cc__string_slot_from_int16_t";
        if (strcmp(type_name, "uint16_t") == 0) return "cc__string_slot_from_uint16_t";
        if (strcmp(type_name, "int32_t") == 0) return "cc__string_slot_from_int32_t";
        if (strcmp(type_name, "uint32_t") == 0) return "cc__string_slot_from_uint32_t";
        if (strcmp(type_name, "int64_t") == 0) return "cc__string_slot_from_int64_t";
        if (strcmp(type_name, "uint64_t") == 0) return "cc__string_slot_from_uint64_t";
        if (strcmp(type_name, "intptr_t") == 0) return "cc__string_slot_from_intptr_t";
        if (strcmp(type_name, "uintptr_t") == 0) return "cc__string_slot_from_uintptr_t";
        if (strcmp(type_name, "size_t") == 0) return "cc__string_slot_from_uintptr_t";
        if (strcmp(type_name, "float") == 0) return "cc__string_slot_from_float";
        if (strcmp(type_name, "double") == 0) return "cc__string_slot_from_double";
        if (strcmp(type_name, "bool") == 0) return "cc__string_slot_from_bool";
        return NULL;
    }
    if (strcmp(family, "cc__string_slot_push") == 0) {
        if (strcmp(type_name, "char") == 0) return "cc__string_slot_push_from_char";
        if (strcmp(type_name, "signed char") == 0) return "cc__string_slot_push_from_signed_char";
        if (strcmp(type_name, "unsigned char") == 0) return "cc__string_slot_push_from_unsigned_char";
        if (strcmp(type_name, "short") == 0) return "cc__string_slot_push_from_short";
        if (strcmp(type_name, "unsigned short") == 0) return "cc__string_slot_push_from_unsigned_short";
        if (strcmp(type_name, "int") == 0) return "cc__string_slot_push_from_int";
        if (strcmp(type_name, "unsigned") == 0) return "cc__string_slot_push_from_unsigned";
        if (strcmp(type_name, "long") == 0) return "cc__string_slot_push_from_long";
        if (strcmp(type_name, "unsigned long") == 0) return "cc__string_slot_push_from_unsigned_long";
        if (strcmp(type_name, "long long") == 0) return "cc__string_slot_push_from_long_long";
        if (strcmp(type_name, "unsigned long long") == 0) return "cc__string_slot_push_from_unsigned_long_long";
        if (strcmp(type_name, "int8_t") == 0) return "cc__string_slot_push_from_int8_t";
        if (strcmp(type_name, "uint8_t") == 0) return "cc__string_slot_push_from_uint8_t";
        if (strcmp(type_name, "int16_t") == 0) return "cc__string_slot_push_from_int16_t";
        if (strcmp(type_name, "uint16_t") == 0) return "cc__string_slot_push_from_uint16_t";
        if (strcmp(type_name, "int32_t") == 0) return "cc__string_slot_push_from_int32_t";
        if (strcmp(type_name, "uint32_t") == 0) return "cc__string_slot_push_from_uint32_t";
        if (strcmp(type_name, "int64_t") == 0) return "cc__string_slot_push_from_int64_t";
        if (strcmp(type_name, "uint64_t") == 0) return "cc__string_slot_push_from_uint64_t";
        if (strcmp(type_name, "intptr_t") == 0) return "cc__string_slot_push_from_intptr_t";
        if (strcmp(type_name, "uintptr_t") == 0) return "cc__string_slot_push_from_uintptr_t";
        if (strcmp(type_name, "size_t") == 0) return "cc__string_slot_push_from_size_t";
        if (strcmp(type_name, "float") == 0) return "cc__string_slot_push_from_float";
        if (strcmp(type_name, "double") == 0) return "cc__string_slot_push_from_double";
        if (strcmp(type_name, "bool") == 0) return "cc__string_slot_push_from_bool";
        return NULL;
    }
    return NULL;
}

static int cc__is_numeric_expr_type_codegen(const char* type_name) {
    if (!type_name || !type_name[0]) return 0;
    return strcmp(type_name, "char") == 0 ||
           strcmp(type_name, "signed char") == 0 ||
           strcmp(type_name, "unsigned char") == 0 ||
           strcmp(type_name, "short") == 0 ||
           strcmp(type_name, "unsigned short") == 0 ||
           strcmp(type_name, "int") == 0 ||
           strcmp(type_name, "unsigned") == 0 ||
           strcmp(type_name, "long") == 0 ||
           strcmp(type_name, "unsigned long") == 0 ||
           strcmp(type_name, "long long") == 0 ||
           strcmp(type_name, "unsigned long long") == 0 ||
           strcmp(type_name, "int8_t") == 0 ||
           strcmp(type_name, "uint8_t") == 0 ||
           strcmp(type_name, "int16_t") == 0 ||
           strcmp(type_name, "uint16_t") == 0 ||
           strcmp(type_name, "int32_t") == 0 ||
           strcmp(type_name, "uint32_t") == 0 ||
           strcmp(type_name, "int64_t") == 0 ||
           strcmp(type_name, "uint64_t") == 0 ||
           strcmp(type_name, "intptr_t") == 0 ||
           strcmp(type_name, "uintptr_t") == 0 ||
           strcmp(type_name, "size_t") == 0 ||
           strcmp(type_name, "float") == 0 ||
           strcmp(type_name, "double") == 0 ||
           strcmp(type_name, "bool") == 0;
}

static int cc__copy_type_name_codegen(char* out, size_t out_sz, const char* type_name) {
    size_t len = 0;
    if (!out || out_sz == 0 || !type_name || !type_name[0]) return 0;
    len = strlen(type_name);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, type_name, len);
    out[len] = '\0';
    return 1;
}

static const char* cc__promote_numeric_expr_type_codegen(const char* lhs,
                                                         const char* rhs) {
    if (!lhs || !rhs) return NULL;
    if (strcmp(lhs, "double") == 0 || strcmp(rhs, "double") == 0) return "double";
    if (strcmp(lhs, "float") == 0 || strcmp(rhs, "float") == 0) return "float";
    if (strcmp(lhs, "uintptr_t") == 0 || strcmp(rhs, "uintptr_t") == 0) return "uintptr_t";
    if (strcmp(lhs, "intptr_t") == 0 || strcmp(rhs, "intptr_t") == 0) return "intptr_t";
    if (strcmp(lhs, "size_t") == 0 || strcmp(rhs, "size_t") == 0) return "size_t";
    if (strcmp(lhs, "uint64_t") == 0 || strcmp(rhs, "uint64_t") == 0) return "uint64_t";
    if (strcmp(lhs, "int64_t") == 0 || strcmp(rhs, "int64_t") == 0) return "int64_t";
    if (strcmp(lhs, "unsigned long long") == 0 || strcmp(rhs, "unsigned long long") == 0) return "unsigned long long";
    if (strcmp(lhs, "long long") == 0 || strcmp(rhs, "long long") == 0) return "long long";
    if (strcmp(lhs, "unsigned long") == 0 || strcmp(rhs, "unsigned long") == 0) return "unsigned long";
    if (strcmp(lhs, "long") == 0 || strcmp(rhs, "long") == 0) return "long";
    if (strcmp(lhs, "uint32_t") == 0 || strcmp(rhs, "uint32_t") == 0) return "uint32_t";
    if (strcmp(lhs, "int32_t") == 0 || strcmp(rhs, "int32_t") == 0) return "int32_t";
    if (strcmp(lhs, "unsigned") == 0 || strcmp(rhs, "unsigned") == 0) return "unsigned";
    if (strcmp(lhs, "int") == 0 || strcmp(rhs, "int") == 0) return "int";
    if (strcmp(lhs, "uint16_t") == 0 || strcmp(rhs, "uint16_t") == 0) return "uint16_t";
    if (strcmp(lhs, "int16_t") == 0 || strcmp(rhs, "int16_t") == 0) return "int16_t";
    if (strcmp(lhs, "uint8_t") == 0 || strcmp(rhs, "uint8_t") == 0) return "uint8_t";
    if (strcmp(lhs, "int8_t") == 0 || strcmp(rhs, "int8_t") == 0) return "int8_t";
    if (strcmp(lhs, "unsigned short") == 0 || strcmp(rhs, "unsigned short") == 0) return "unsigned short";
    if (strcmp(lhs, "short") == 0 || strcmp(rhs, "short") == 0) return "short";
    if (strcmp(lhs, "unsigned char") == 0 || strcmp(rhs, "unsigned char") == 0) return "unsigned char";
    if (strcmp(lhs, "signed char") == 0 || strcmp(rhs, "signed char") == 0) return "signed char";
    if (strcmp(lhs, "char") == 0 || strcmp(rhs, "char") == 0) return "char";
    if (strcmp(lhs, "bool") == 0 && strcmp(rhs, "bool") == 0) return "bool";
    return NULL;
}

static int cc__find_top_level_binary_op_codegen(const char* expr,
                                                size_t len,
                                                const char* ops,
                                                size_t* op_idx) {
    int par = 0, br = 0, brc = 0;
    if (!expr || !ops || !op_idx) return 0;
    for (size_t i = len; i > 0; --i) {
        char c = expr[i - 1];
        if (c == ')') par++;
        else if (c == '(' && par > 0) par--;
        else if (c == ']') br++;
        else if (c == '[' && br > 0) br--;
        else if (c == '}') brc++;
        else if (c == '{' && brc > 0) brc--;
        if (par != 0 || br != 0 || brc != 0) continue;
        if (!strchr(ops, c)) continue;
        if ((c == '+' || c == '-') &&
            (i == 1 ||
             strchr("([{,?:=+-*/%&|^!~<>", expr[i - 2]) != NULL ||
             (c == '-' && i < len && expr[i] == '>'))) {
            continue;
        }
        *op_idx = i - 1;
        return 1;
    }
    return 0;
}

static int cc__resolve_expr_type_codegen(const char* src,
                                         size_t use_offset,
                                         CCTypeRegistry* reg,
                                         const char* expr,
                                         char* out_type,
                                         size_t out_type_sz) {
    const char* resolved = NULL;
    const char* start = expr;
    const char* end = expr ? expr + strlen(expr) : NULL;
    char trimmed[256];
    char lhs_expr[256];
    char rhs_expr[256];
    char lhs_type[128];
    char rhs_type[128];
    size_t len;
    size_t op_idx = 0;
    const char* promoted = NULL;

    if (!expr || !out_type || out_type_sz == 0) return 0;
    out_type[0] = '\0';
    cc__trim_expr_parens_codegen(&start, &end);
    if (!start || !end || end <= start) return 0;
    len = (size_t)(end - start);
    if (len >= sizeof(trimmed)) return 0;
    memcpy(trimmed, start, len);
    trimmed[len] = '\0';

    if (reg) {
        resolved = cc_type_registry_resolve_receiver_expr_at(reg, trimmed, src, use_offset, NULL);
        if (resolved && resolved[0]) return cc__copy_type_name_codegen(out_type, out_type_sz, resolved);
        resolved = cc_type_registry_resolve_expr_type(reg, trimmed);
        if (resolved && resolved[0]) return cc__copy_type_name_codegen(out_type, out_type_sz, resolved);
    }
    if (cc__lookup_scoped_local_var_type_codegen(src, use_offset, trimmed, out_type, out_type_sz)) {
        return 1;
    }

    if (cc__find_top_level_binary_op_codegen(trimmed, len, "+-", &op_idx) ||
        cc__find_top_level_binary_op_codegen(trimmed, len, "*/%", &op_idx) ||
        cc__find_top_level_binary_op_codegen(trimmed, len, "&|^", &op_idx)) {
        if (op_idx == 0 || op_idx + 1 >= len) return 0;
        if (op_idx >= sizeof(lhs_expr) || len - op_idx - 1 >= sizeof(rhs_expr)) return 0;
        memcpy(lhs_expr, trimmed, op_idx);
        lhs_expr[op_idx] = '\0';
        memcpy(rhs_expr, trimmed + op_idx + 1, len - op_idx - 1);
        rhs_expr[len - op_idx - 1] = '\0';
        if (!cc__resolve_expr_type_codegen(src, use_offset, reg, lhs_expr, lhs_type, sizeof(lhs_type)) ||
            !cc__resolve_expr_type_codegen(src, use_offset, reg, rhs_expr, rhs_type, sizeof(rhs_type))) {
            return 0;
        }
        if (!cc__is_numeric_expr_type_codegen(lhs_type) || !cc__is_numeric_expr_type_codegen(rhs_type)) {
            return 0;
        }
        promoted = cc__promote_numeric_expr_type_codegen(lhs_type, rhs_type);
        return promoted ? cc__copy_type_name_codegen(out_type, out_type_sz, promoted) : 0;
    }

    return 0;
}

static const char* cc__string_helper_for_literal_codegen(const char* family,
                                                         const char* expr,
                                                         char* type_buf,
                                                         size_t type_buf_sz) {
    const char* s = expr;
    const char* e = expr ? expr + strlen(expr) : NULL;
    size_t len;
    if (!expr || !type_buf || type_buf_sz == 0) return NULL;
    type_buf[0] = '\0';
    cc__trim_expr_parens_codegen(&s, &e);
    if (!s || !e || e <= s) return NULL;
    len = (size_t)(e - s);
    if ((len == 4 && memcmp(s, "true", 4) == 0) ||
        (len == 5 && memcmp(s, "false", 5) == 0)) {
        strncpy(type_buf, "bool", type_buf_sz - 1);
        type_buf[type_buf_sz - 1] = '\0';
        return cc__string_helper_for_type_codegen(family, type_buf);
    }
    if (*s == '"' || *s == '\'') return NULL;
    {
        int has_dot = 0, has_exp = 0;
        const char* t = s;
        while (t < e && (*t == '+' || *t == '-')) t++;
        if (t >= e || !(isdigit((unsigned char)*t) || *t == '.')) return NULL;
        for (const char* p = t; p < e; ++p) {
            if (*p == '.') has_dot = 1;
            else if (*p == 'e' || *p == 'E') has_exp = 1;
        }
        if (has_dot || has_exp) {
            if (e > t && (e[-1] == 'f' || e[-1] == 'F')) {
                strncpy(type_buf, "float", type_buf_sz - 1);
            } else {
                strncpy(type_buf, "double", type_buf_sz - 1);
            }
            type_buf[type_buf_sz - 1] = '\0';
            return cc__string_helper_for_type_codegen(family, type_buf);
        }
    }
    {
        int has_digit = 0;
        int has_float_marker = 0;
        int has_operator = 0;
        int invalid = 0;
        for (const char* p = s; p < e; ++p) {
            char c = *p;
            if (isdigit((unsigned char)c)) { has_digit = 1; continue; }
            if (isalpha((unsigned char)c) || c == '_') {
                if (c == 'e' || c == 'E' || c == 'f' || c == 'F') has_float_marker = 1;
                continue;
            }
            if (isspace((unsigned char)c) || c == '(' || c == ')') continue;
            if (strchr("+-*/%&|^<>", c)) { has_operator = 1; continue; }
            invalid = 1;
            break;
        }
        if (!invalid && has_operator && has_digit) {
            strncpy(type_buf, has_float_marker ? "double" : "int", type_buf_sz - 1);
            type_buf[type_buf_sz - 1] = '\0';
            return cc__string_helper_for_type_codegen(family, type_buf);
        }
    }
    {
        char suffix[8];
        size_t suf_len = 0;
        const char* p = e;
        while (p > s && isalpha((unsigned char)p[-1]) && suf_len + 1 < sizeof(suffix)) {
            suffix[suf_len++] = (char)tolower((unsigned char)p[-1]);
            p--;
        }
        suffix[suf_len] = '\0';
        if (strstr(suffix, "ull") || (strchr(suffix, 'u') && strstr(suffix, "ll"))) {
            strncpy(type_buf, "unsigned long long", type_buf_sz - 1);
        } else if (strstr(suffix, "ll")) {
            strncpy(type_buf, "long long", type_buf_sz - 1);
        } else if (strchr(suffix, 'u') && strchr(suffix, 'l')) {
            strncpy(type_buf, "unsigned long", type_buf_sz - 1);
        } else if (strchr(suffix, 'u')) {
            strncpy(type_buf, "unsigned", type_buf_sz - 1);
        } else if (strchr(suffix, 'l')) {
            strncpy(type_buf, "long", type_buf_sz - 1);
        } else {
            strncpy(type_buf, "int", type_buf_sz - 1);
        }
        type_buf[type_buf_sz - 1] = '\0';
        return cc__string_helper_for_type_codegen(family, type_buf);
    }
}

static char* cc__rewrite_string_helper_family_to_visible_type(const char* src, size_t n) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0, last_emit = 0;
    int changed = 0;
    CCTypeRegistry* saved_reg = NULL;
    CCTypeRegistry* temp_reg = NULL;
    if (!src || n == 0) return NULL;
    temp_reg = cc_type_registry_new();
    if (temp_reg) {
        saved_reg = cc_type_registry_get_global();
        cc_type_registry_set_global(temp_reg);
        cc__collect_ufcs_field_and_var_types(src, n);
        cc_type_registry_set_global(saved_reg);
    }
    while (i < n) {
        size_t ident_start = i;
        size_t ident_end;
        size_t paren_open;
        size_t paren_end = 0;
        const char* family = NULL;
        size_t family_len = 0;
        size_t arg_s[3] = {0}, arg_e[3] = {0};
        int arg_count = 0;
        char expr_buf[256];
        char actual_type[256];
        char literal_type[64];
        const char* helper = NULL;
        if (!cc_is_ident_start(src[i])) {
            i++;
            continue;
        }
        while (i < n && cc_is_ident_char(src[i])) i++;
        ident_end = i;
        family_len = ident_end - ident_start;
        if (family_len == 13 && memcmp(src + ident_start, "CCString_from", 13) == 0) {
            family = "CCString_from";
        } else if (family_len == 19 && memcmp(src + ident_start, "cc__string_slot_arg", 19) == 0) {
            family = "cc__string_slot_arg";
        } else if (family_len == 20 && memcmp(src + ident_start, "cc__string_slot_push", 20) == 0) {
            family = "cc__string_slot_push";
        } else {
            continue;
        }
        paren_open = cc_skip_ws_and_comments(src, n, ident_end);
        if (paren_open >= n || src[paren_open] != '(') continue;
        if (!cc_find_matching_paren(src, n, paren_open, &paren_end)) continue;
        {
            size_t cursor = paren_open + 1;
            int par = 0, br = 0, brc = 0;
            while (cursor < paren_end && arg_count < 3) {
                arg_s[arg_count] = cc_skip_ws_and_comments(src, n, cursor);
                cursor = arg_s[arg_count];
                while (cursor < paren_end) {
                    char c = src[cursor];
                    if (c == '(') par++;
                    else if (c == ')' && par > 0) par--;
                    else if (c == '[') br++;
                    else if (c == ']' && br > 0) br--;
                    else if (c == '{') brc++;
                    else if (c == '}' && brc > 0) brc--;
                    else if (c == ',' && par == 0 && br == 0 && brc == 0) break;
                    cursor++;
                }
                arg_e[arg_count] = cursor;
                while (arg_e[arg_count] > arg_s[arg_count] &&
                       isspace((unsigned char)src[arg_e[arg_count] - 1])) arg_e[arg_count]--;
                arg_count++;
                if (cursor < paren_end && src[cursor] == ',') cursor++;
            }
        }
        {
            int value_idx = (family && strcmp(family, "cc__string_slot_push") == 0) ? 1 : 0;
            size_t start = arg_s[value_idx];
            size_t end = arg_e[value_idx];
            if (arg_count <= value_idx || end <= start || end - start >= sizeof(expr_buf)) continue;
            memcpy(expr_buf, src + start, end - start);
            expr_buf[end - start] = '\0';
            if (cc__resolve_expr_type_codegen(src, ident_start, temp_reg, expr_buf,
                                              actual_type, sizeof(actual_type))) {
                helper = cc__string_helper_for_type_codegen(family, actual_type);
            }
            if (!helper) helper = cc__string_helper_for_literal_codegen(family, expr_buf, literal_type, sizeof(literal_type));
        }
        if (!helper) continue;
        cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, ident_start - last_emit);
        cc__sb_append_cstr_local(&out, &out_len, &out_cap, helper);
        last_emit = ident_end;
        changed = 1;
    }
    if (temp_reg) cc_type_registry_free(temp_reg);
    if (!changed) {
        free(out);
        return NULL;
    }
    if (last_emit < n) cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

static const char* cc__canonicalize_parser_family_macro_codegen(const char* type_name,
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
    } else if (strncmp(type_name, "CCVec_", 6) == 0 || strncmp(type_name, "Map_", 4) == 0) {
        strncpy(scratch, type_name, scratch_cap - 1);
        scratch[scratch_cap - 1] = '\0';
        return scratch;
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

static char* cc__rewrite_parser_generic_family_helpers_to_concrete(const char* src, size_t n) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    size_t last_emit = 0;
    int changed = 0;
    CCTypeRegistry* saved_reg = NULL;
    CCTypeRegistry* temp_reg = NULL;
    if (!src || n == 0) return NULL;
    temp_reg = cc_type_registry_new();
    if (temp_reg) {
        saved_reg = cc_type_registry_get_global();
        cc_type_registry_set_global(temp_reg);
        cc__collect_ufcs_field_and_var_types(src, n);
        cc_type_registry_set_global(saved_reg);
    }
    while (i < n) {
        size_t ident_start = i;
        size_t ident_end;
        size_t paren_open;
        size_t paren_end = 0;
        size_t prefix_len = 0;
        const char* method = NULL;
        char recv_expr[256];
        char concrete_type[256];
        size_t arg_start;
        size_t arg_end;
        if (!cc_is_ident_start(src[i])) {
            i++;
            continue;
        }
        while (i < n && cc_is_ident_char(src[i])) i++;
        ident_end = i;
        if (ident_end - ident_start > 17 &&
            memcmp(src + ident_start, "__cc_vec_generic_", 17) == 0) {
            prefix_len = 17;
        } else if (ident_end - ident_start > 17 &&
                   memcmp(src + ident_start, "__cc_map_generic_", 17) == 0) {
            prefix_len = 17;
        } else {
            continue;
        }
        method = src + ident_start + prefix_len;
        if (method >= src + ident_end) continue;
        paren_open = cc_skip_ws_and_comments(src, n, ident_end);
        if (paren_open >= n || src[paren_open] != '(') continue;
        if (!cc_find_matching_paren(src, n, paren_open, &paren_end)) continue;
        arg_start = cc_skip_ws_and_comments(src, n, paren_open + 1);
        arg_end = arg_start;
        {
            int par = 0, br = 0, brc = 0;
            while (arg_end < paren_end) {
                char c = src[arg_end];
                if (c == '(') par++;
                else if (c == ')' && par > 0) par--;
                else if (c == '[') br++;
                else if (c == ']' && br > 0) br--;
                else if (c == '{') brc++;
                else if (c == '}' && brc > 0) brc--;
                else if (c == ',' && par == 0 && br == 0 && brc == 0) break;
                arg_end++;
            }
        }
        while (arg_end > arg_start && isspace((unsigned char)src[arg_end - 1])) arg_end--;
        if (arg_end <= arg_start || (arg_end - arg_start) >= sizeof(recv_expr)) continue;
        memcpy(recv_expr, src + arg_start, arg_end - arg_start);
        recv_expr[arg_end - arg_start] = '\0';
        {
            const char* actual_type = temp_reg
                ? cc_type_registry_resolve_receiver_expr_at(
                      temp_reg, recv_expr, src, ident_start, NULL)
                : NULL;
            const char* concrete = cc__canonicalize_parser_family_macro_codegen(
                actual_type, concrete_type, sizeof(concrete_type));
            if (!concrete || !concrete[0]) continue;
            cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, ident_start - last_emit);
            cc__sb_append_cstr_local(&out, &out_len, &out_cap, concrete);
            cc__sb_append_cstr_local(&out, &out_len, &out_cap, "_");
            cc__sb_append_local(&out, &out_len, &out_cap, method, (size_t)((src + ident_end) - method));
            last_emit = ident_end;
            changed = 1;
        }
    }
    if (temp_reg) cc_type_registry_free(temp_reg);
    if (!changed) {
        free(out);
        return NULL;
    }
    if (last_emit < n) cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

static void cc__collect_ufcs_field_and_var_types(const char* src, size_t n) {
    CCTypeRegistry* reg = cc_type_registry_get_global();
    size_t i = 0;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    if (!reg || !src) return;
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        if (c == '{') {
            cc__record_function_params_before_brace_codegen(reg, src, i);
        }

        if (i + 6 <= n && memcmp(src + i, "CCChan", 6) == 0 &&
            (i == 0 || !(isalnum((unsigned char)src[i - 1]) || src[i - 1] == '_')) &&
            (i + 6 == n || !(isalnum((unsigned char)src[i + 6]) || src[i + 6] == '_'))) {
            size_t p = i + 6;
            int is_ptr = 0;
            while (p < n && (src[p] == ' ' || src[p] == '\t')) p++;
            if (p < n && src[p] == '*') {
                is_ptr = 1;
                p++;
                while (p < n && (src[p] == ' ' || src[p] == '\t')) p++;
            }
            if (p < n && (isalpha((unsigned char)src[p]) || src[p] == '_')) {
                char var_name[128];
                size_t vn = 0;
                size_t v = p;
                while (v < n && (isalnum((unsigned char)src[v]) || src[v] == '_')) {
                    if (vn + 1 < sizeof(var_name)) var_name[vn] = src[v];
                    vn++;
                    v++;
                }
                var_name[vn < sizeof(var_name) ? vn : sizeof(var_name) - 1] = '\0';
                v = cc__skip_ws_codegen(src, n, v);
                if (v < n && src[v] != '(') {
                    cc_type_registry_add_var(reg, var_name, is_ptr ? "CCChan*" : "CCChan");
                }
                i = v;
                continue;
            }
        }

        if (i + 7 < n && memcmp(src + i, "typedef", 7) == 0 && !isalnum((unsigned char)src[i + 7]) && src[i + 7] != '_') {
            size_t semi = i;
            while (semi < n && src[semi] != ';') semi++;
            if (semi < n) {
                char alias_name[128];
                char alias_type[256];
                char canonical_alias_type[256];
                if (cc__parse_typedef_alias_stmt_codegen(src + i, src + semi,
                                                         alias_name, sizeof(alias_name),
                                                         alias_type, sizeof(alias_type)) &&
                    alias_name[0]) {
                    if (cc_type_registry_canonicalize_type_name(reg, alias_type,
                                                                canonical_alias_type,
                                                                sizeof(canonical_alias_type))) {
                        cc_type_registry_add_alias(reg, alias_name, canonical_alias_type);
                    } else {
                        cc_type_registry_add_alias(reg, alias_name, alias_type);
                    }
                }
            }
            size_t j = cc__skip_ws_codegen(src, n, i + 7);
            if (j + 6 < n && memcmp(src + j, "struct", 6) == 0 && !isalnum((unsigned char)src[j + 6]) && src[j + 6] != '_') {
                size_t body_l = cc__skip_ws_codegen(src, n, j + 6);
                /* Skip an optional struct tag identifier before the `{` so
                 * tagged typedefs (e.g. `typedef struct Foo { ... } Foo;`)
                 * register their fields alongside the anonymous form. */
                if (body_l < n && (isalpha((unsigned char)src[body_l]) || src[body_l] == '_')) {
                    size_t tag_end = body_l;
                    while (tag_end < n && (isalnum((unsigned char)src[tag_end]) || src[tag_end] == '_')) tag_end++;
                    body_l = cc__skip_ws_codegen(src, n, tag_end);
                }
                size_t body_r = 0;
                if (body_l < n && src[body_l] == '{' && cc__find_matching_brace_codegen(src, n, body_l, &body_r)) {
                    size_t name_pos = cc__skip_ws_codegen(src, n, body_r + 1);
                    if (name_pos < n && (isalpha((unsigned char)src[name_pos]) || src[name_pos] == '_')) {
                        char struct_name[128];
                        size_t sn = 0;
                        size_t p = name_pos;
                        while (p < n && (isalnum((unsigned char)src[p]) || src[p] == '_')) {
                            if (sn + 1 < sizeof(struct_name)) struct_name[sn] = src[p];
                            sn++;
                            p++;
                        }
                        struct_name[sn < sizeof(struct_name) ? sn : sizeof(struct_name) - 1] = '\0';
                        {
                            const char* body = src + body_l + 1;
                            const char* body_end = src + body_r;
                            const char* stmt = body;
                            while (stmt < body_end) {
                                const char* semi = memchr(stmt, ';', (size_t)(body_end - stmt));
                                if (!semi) break;
                                char field_name[128];
                                char field_type[256];
                                cc_parse_decl_name_and_type(stmt, semi, field_name, sizeof(field_name),
                                                                     field_type, sizeof(field_type));
                                if (!field_name[0]) {
                                    (void)cc__parse_decl_name_and_type_fallback_codegen(stmt, semi,
                                                                                        field_name, sizeof(field_name),
                                                                                        field_type, sizeof(field_type));
                                }
                                if (field_name[0] && field_type[0]) {
                                    char canonical_field_type[256];
                                    if (cc_type_registry_canonicalize_type_name(reg, field_type,
                                                                                canonical_field_type,
                                                                                sizeof(canonical_field_type))) {
                                        cc_type_registry_add_field(reg, struct_name, field_name, canonical_field_type);
                                    } else {
                                        cc_type_registry_add_field(reg, struct_name, field_name, field_type);
                                    }
                                }
                                stmt = semi + 1;
                            }
                        }
                    }
                }
            }
        }
        if ((isalpha((unsigned char)c) || c == '_') &&
            !(i > 0 && (isalnum((unsigned char)src[i - 1]) || src[i - 1] == '_' || src[i - 1] == '@'))) {
            size_t type_start = i;
            size_t type_end;
            size_t j;
            while (i < n && (isalnum((unsigned char)src[i]) || src[i] == '_')) i++;
            type_end = i;
            if ((type_end - type_start == 6 && memcmp(src + type_start, "struct", 6) == 0) ||
                (type_end - type_start == 5 && memcmp(src + type_start, "union", 5) == 0)) {
                size_t tag = cc__skip_ws_codegen(src, n, type_end);
                if (tag < n && (isalpha((unsigned char)src[tag]) || src[tag] == '_')) {
                    size_t tag_end = tag;
                    while (tag_end < n && (isalnum((unsigned char)src[tag_end]) || src[tag_end] == '_')) tag_end++;
                    type_end = tag_end;
                    i = tag_end;
                }
            }
            if (type_end - type_start == sizeof("__CC_VEC") - 1 &&
                memcmp(src + type_start, "__CC_VEC", sizeof("__CC_VEC") - 1) == 0) {
                size_t macro_l = cc__skip_ws_codegen(src, n, type_end);
                size_t macro_r = 0;
                if (macro_l < n && src[macro_l] == '(' && cc__find_matching_paren_codegen(src, n, macro_l, &macro_r)) {
                    type_end = macro_r + 1;
                }
            } else if (type_end - type_start == sizeof("__CC_MAP") - 1 &&
                       memcmp(src + type_start, "__CC_MAP", sizeof("__CC_MAP") - 1) == 0) {
                size_t macro_l = cc__skip_ws_codegen(src, n, type_end);
                size_t macro_r = 0;
                if (macro_l < n && src[macro_l] == '(' && cc__find_matching_paren_codegen(src, n, macro_l, &macro_r)) {
                    type_end = macro_r + 1;
                }
            }
            j = cc__skip_ws_codegen(src, n, type_end);
            while (j < n && src[j] == '*') {
                j++;
                j = cc__skip_ws_codegen(src, n, j);
            }
            if (j < n && (isalpha((unsigned char)src[j]) || src[j] == '_')) {
                size_t after_name;
                size_t var_start = j;
                char type_name[256];
                char var_name[128];
                size_t tn;
                size_t vn;
                while (j < n && (isalnum((unsigned char)src[j]) || src[j] == '_')) j++;
                after_name = cc__skip_ws_codegen(src, n, j);
                if (after_name < n && src[after_name] != '(') {
                    tn = type_end - type_start;
                    vn = j - var_start;
                    if (tn >= sizeof(type_name)) tn = sizeof(type_name) - 1;
                    if (vn >= sizeof(var_name)) vn = sizeof(var_name) - 1;
                    memcpy(type_name, src + type_start, tn);
                    type_name[tn] = '\0';
                    memcpy(var_name, src + var_start, vn);
                    var_name[vn] = '\0';
                    {
                        size_t k = cc__skip_ws_codegen(src, n, type_end);
                        while (k < var_start && (src[k] == '*' || src[k] == ' ' || src[k] == '\t')) {
                            if (src[k] == '*') {
                                strncat(type_name, "*", sizeof(type_name) - strlen(type_name) - 1);
                            }
                            k++;
                        }
                    }
                    {
                        const char* existing = cc_type_registry_lookup_var(reg, var_name);
                        if (!existing || cc__is_parser_placeholder_type_codegen(existing)) {
                            char canonical_type_name[256];
                            if (cc_type_registry_canonicalize_type_name(reg, type_name,
                                                                        canonical_type_name,
                                                                        sizeof(canonical_type_name))) {
                                cc_type_registry_add_var(reg, var_name, canonical_type_name);
                            } else {
                                cc_type_registry_add_var(reg, var_name, type_name);
                            }
                        }
                    }
                }
            }
            continue;
        }
        i++;
    }
}

static int cc__is_parser_placeholder_type_codegen(const char* type_name) {
    return type_name &&
           (strcmp(type_name, "__CCVecGeneric") == 0 ||
            strcmp(type_name, "__CCVecGeneric*") == 0 ||
            strcmp(type_name, "__CCMapGeneric") == 0 ||
            strcmp(type_name, "__CCMapGeneric*") == 0 ||
            strcmp(type_name, "__CCResultGeneric") == 0 ||
            strcmp(type_name, "__CCResultGeneric*") == 0);
}

/* Rewrite `if @try (T x = expr) { ... } else { ... }` into expanded form:
   { __typeof__(expr) __cc_try_bind = (expr);
     if (__cc_try_bind.ok) { T x = __cc_try_bind.u.value; ... }
     else { ... } }
*/
static char* cc__rewrite_if_try_syntax(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0, last_emit = 0;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    
    while (i < n) {
        char c = src[i], c2 = (i+1 < n) ? src[i+1] : 0;
        /* Skip comments and strings */
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i+1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i+1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Look for `if @try (` */
        if (c == 'i' && c2 == 'f') {
            int ws = (i == 0) || !cc_is_ident_char(src[i-1]);
            int we = (i+2 >= n) || !cc_is_ident_char(src[i+2]);
            if (ws && we) {
                size_t if_start = i, j = i + 2;
                /* Skip whitespace after 'if' */
                while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
                /* Check for '@try' */
                if (j+4 <= n && src[j] == '@' && src[j+1] == 't' && src[j+2] == 'r' && src[j+3] == 'y' &&
                    (j+4 >= n || !cc_is_ident_char(src[j+4]))) {
                    size_t after_try = j + 4;
                    while (after_try < n && (src[after_try] == ' ' || src[after_try] == '\t' || src[after_try] == '\n')) after_try++;
                    /* Expect '(' */
                    if (after_try < n && src[after_try] == '(') {
                        size_t cond_start = after_try + 1;
                        /* Find matching ')' */
                        size_t cond_end = cond_start;
                        int paren = 1, in_s = 0, in_c = 0;
                        while (cond_end < n && paren > 0) {
                            char ec = src[cond_end];
                            if (in_s) { if (ec == '\\' && cond_end+1 < n) cond_end++; else if (ec == '"') in_s = 0; cond_end++; continue; }
                            if (in_c) { if (ec == '\\' && cond_end+1 < n) cond_end++; else if (ec == '\'') in_c = 0; cond_end++; continue; }
                            if (ec == '"') { in_s = 1; cond_end++; continue; }
                            if (ec == '\'') { in_c = 1; cond_end++; continue; }
                            if (ec == '(') paren++;
                            else if (ec == ')') { paren--; if (paren == 0) break; }
                            cond_end++;
                        }
                        if (paren != 0) { i++; continue; }
                        
                        /* Parse T x = expr from cond_start to cond_end */
                        size_t eq = cond_start;
                        while (eq < cond_end && src[eq] != '=') eq++;
                        if (eq >= cond_end) { i++; continue; }
                        
                        /* Type and var before '=' */
                        size_t tv_end = eq;
                        while (tv_end > cond_start && (src[tv_end-1] == ' ' || src[tv_end-1] == '\t')) tv_end--;
                        size_t var_end = tv_end, var_start = var_end;
                        while (var_start > cond_start && cc_is_ident_char(src[var_start-1])) var_start--;
                        if (var_start >= var_end) { i++; continue; }
                        
                        size_t type_end = var_start;
                        while (type_end > cond_start && (src[type_end-1] == ' ' || src[type_end-1] == '\t')) type_end--;
                        size_t type_start = cond_start;
                        while (type_start < type_end && (src[type_start] == ' ' || src[type_start] == '\t')) type_start++;
                        if (type_start >= type_end) { i++; continue; }
                        
                        /* Expr after '=' */
                        size_t expr_start = eq + 1;
                        while (expr_start < cond_end && (src[expr_start] == ' ' || src[expr_start] == '\t')) expr_start++;
                        size_t expr_end = cond_end;
                        while (expr_end > expr_start && (src[expr_end-1] == ' ' || src[expr_end-1] == '\t')) expr_end--;
                        if (expr_start >= expr_end) { i++; continue; }
                        
                        /* Find then-block */
                        size_t k = cond_end + 1; /* skip ')' */
                        while (k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\n')) k++;
                        if (k >= n || src[k] != '{') { i++; continue; }
                        
                        size_t then_start = k;
                        int brace = 1; k++; in_s = 0; in_c = 0;
                        while (k < n && brace > 0) {
                            char ec = src[k];
                            if (in_s) { if (ec == '\\' && k+1 < n) k++; else if (ec == '"') in_s = 0; k++; continue; }
                            if (in_c) { if (ec == '\\' && k+1 < n) k++; else if (ec == '\'') in_c = 0; k++; continue; }
                            if (ec == '"') { in_s = 1; k++; continue; }
                            if (ec == '\'') { in_c = 1; k++; continue; }
                            if (ec == '{') brace++; else if (ec == '}') brace--;
                            k++;
                        }
                        size_t then_end = k;
                        
                        /* Check for else */
                        size_t else_start = 0, else_end = 0, m = k;
                        while (m < n && (src[m] == ' ' || src[m] == '\t' || src[m] == '\n')) m++;
                        if (m+4 <= n && src[m] == 'e' && src[m+1] == 'l' && src[m+2] == 's' && src[m+3] == 'e' &&
                            (m+4 >= n || !cc_is_ident_char(src[m+4]))) {
                            m += 4;
                            while (m < n && (src[m] == ' ' || src[m] == '\t' || src[m] == '\n')) m++;
                            if (m < n && src[m] == '{') {
                                else_start = m; brace = 1; m++; in_s = 0; in_c = 0;
                                while (m < n && brace > 0) {
                                    char ec = src[m];
                                    if (in_s) { if (ec == '\\' && m+1 < n) m++; else if (ec == '"') in_s = 0; m++; continue; }
                                    if (in_c) { if (ec == '\\' && m+1 < n) m++; else if (ec == '\'') in_c = 0; m++; continue; }
                                    if (ec == '"') { in_s = 1; m++; continue; }
                                    if (ec == '\'') { in_c = 1; m++; continue; }
                                    if (ec == '{') brace++; else if (ec == '}') brace--;
                                    m++;
                                }
                                else_end = m;
                            }
                        }
                        
                        /* Emit expansion */
                        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, if_start - last_emit);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "{ __typeof__(");
                        cc_sb_append(&out, &out_len, &out_cap, src + expr_start, expr_end - expr_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, ") __cc_try_bind = (");
                        cc_sb_append(&out, &out_len, &out_cap, src + expr_start, expr_end - expr_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "); if (__cc_try_bind.ok) { ");
                        cc_sb_append(&out, &out_len, &out_cap, src + type_start, type_end - type_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, " ");
                        cc_sb_append(&out, &out_len, &out_cap, src + var_start, var_end - var_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, " = __cc_try_bind.u.value; ");
                        cc_sb_append(&out, &out_len, &out_cap, src + then_start + 1, then_end - then_start - 2);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, " }");
                        if (else_end > else_start) {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, " else ");
                            cc_sb_append(&out, &out_len, &out_cap, src + else_start, else_end - else_start);
                        }
                        cc_sb_append_cstr(&out, &out_len, &out_cap, " }");
                        
                        last_emit = (else_end > 0) ? else_end : then_end;
                        i = last_emit;
                        continue;
                    }
                }
            }
        }
        i++;
    }
    if (last_emit == 0) return NULL;
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

int cc_visit_codegen(const CCASTRoot* root, CCVisitorCtx* ctx, const char* output_path) {
    if (!ctx || !ctx->symbols || !output_path) return EINVAL;
    const char* src_path = ctx->input_path ? ctx->input_path : "<cc_input>";
    FILE* out = fopen(output_path, "w");
    if (!out) return errno ? errno : -1;

    /* Optional: dump TCC stub nodes for debugging wiring. */
    if (root && root->nodes && root->node_count > 0) {
        const char* dump = getenv("CC_DUMP_TCC_STUB_AST");
        if (dump && dump[0] == '1') {
            typedef struct {
                int kind;
                int parent;
                const char* file;
                int line_start;
                int line_end;
                int col_start;
                int col_end;
                int aux1;
                int aux2;
                const char* aux_s1;
                const char* aux_s2;
            } CCASTStubNodeView;
            const CCASTStubNodeView* n = (const CCASTStubNodeView*)root->nodes;
            fprintf(stderr, "[cc] stub ast nodes: %d\n", root->node_count);
            int max_dump = root->node_count;
            if (max_dump > 4000) max_dump = 4000;
            for (int i = 0; i < max_dump; i++) {
                fprintf(stderr,
                        "  [%d] kind=%d parent=%d file=%s lines=%d..%d cols=%d..%d aux1=%d aux2=%d aux_s1=%s aux_s2=%s\n",
                        i,
                        n[i].kind,
                        n[i].parent,
                        n[i].file ? n[i].file : "<null>",
                        n[i].line_start,
                        n[i].line_end,
                        n[i].col_start,
                        n[i].col_end,
                        n[i].aux1,
                        n[i].aux2,
                        n[i].aux_s1 ? n[i].aux_s1 : "<null>",
                        n[i].aux_s2 ? n[i].aux_s2 : "<null>");
            }
            if (max_dump != root->node_count)
                fprintf(stderr, "  ... truncated (%d total)\n", root->node_count);
        }
    }

    /* The long-term pipeline is:
         phase 1: canonical CC for comptime
         phase 2: execute/evaluate comptime against that canonical CC
         phase 3: lower the post-comptime TU to host C
       Today phase 2 is still implemented as registration/handler collection,
       but keep the comments and call structure aligned with that broader model.
       For final codegen we still read the original source and lower UFCS plus
       the remaining AST/text passes that operate on original spans
       here; the preprocessor's temp file exists only to make TCC parsing
       succeed. Note: text-based rewrites like `if @try` run on original source
       early in this function. */
    /* Read original source once; later passes still rewrite against original spans. */
    char* src_all = NULL;
    char* src_regs = NULL;
    size_t src_len = 0;
    if (ctx->input_path) {
        cc__read_entire_file(ctx->input_path, &src_all, &src_len);
    }
    if (src_all && src_len && ctx && ctx->symbols) {
        src_regs = cc_preprocess_comptime_source(ctx->input_path);
    }

    char* src_ufcs = src_all;
    size_t src_ufcs_len = src_len;
    const char* reg_src = src_regs ? src_regs : src_all;
    size_t reg_src_len = src_regs ? strlen(src_regs) : src_len;

    /* Phase 2 currently means: collect the comptime-visible effects from the
       canonical CC source (type registrations, UFCS handlers, etc.). After
       that, phase 3 blanks source-local @comptime blocks out of the emitted TU
       while preserving layout so earlier AST spans remain valid. The original
       source still drives named/lambda handler compilation where exact local
       context matters; the comptime discovery input itself should come from one
       authoritative source. */
    if (reg_src && reg_src_len && ctx && ctx->symbols) {
        CCTypeUfcsCompileCtx type_ufcs_ctx = {
            .registration_src = reg_src,
            .registration_n = reg_src_len,
            .compile_src = src_all,
            .compile_n = src_len,
            .compile_src_is_include_expanded = 0,
        };
        if (cc_symbols_collect_type_registrations_ex(ctx->symbols,
                                                     ctx->input_path,
                                                     reg_src,
                                                     reg_src_len,
                                                     NULL,
                                                     NULL,
                                                     cc__compile_type_ufcs_registration,
                                                     &type_ufcs_ctx) != 0) {
            fclose(out);
            free(src_regs);
            free(src_all);
            return EINVAL;
        }
        if (cc__collect_comptime_ufcs_registrations(ctx->symbols, ctx->input_path,
                                                    reg_src, reg_src_len,
                                                    src_all, src_len,
                                                    0,
                                                    0) != 0) {
            fclose(out);
            free(src_regs);
            free(src_all);
            return EINVAL;
        }
        if (getenv("CC_DEBUG_COMPTIME_UFCS")) {
            fprintf(stderr, "CC_DEBUG_COMPTIME_UFCS: collected %zu type-pattern registration(s)\n",
                    cc_symbols_type_count(ctx->symbols));
            for (size_t ti = 0; ti < cc_symbols_type_count(ctx->symbols); ++ti) {
                const char* pat = cc_symbols_type_name(ctx->symbols, ti);
                const void* fn_ptr = NULL;
                if (!pat) continue;
                if (cc_symbols_lookup_type_ufcs_callable(ctx->symbols, pat, &fn_ptr) != 0 || !fn_ptr) continue;
                fprintf(stderr, "  pattern[%zu] = %s\n", ti, pat);
            }
        }
        char* blanked = cc__blank_comptime_blocks_preserve_layout(src_ufcs, src_ufcs_len);
        if (blanked) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = blanked;
            src_ufcs_len = src_len;
        }
    }
    free(src_regs);
    src_regs = NULL;

    if (src_ufcs && src_ufcs_len) {
        char* lowered_includes = cc_rewrite_local_cch_includes_to_lowered_headers(src_ufcs, src_ufcs_len, ctx->input_path);
        if (lowered_includes) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = lowered_includes;
            src_ufcs_len = strlen(lowered_includes);
        }
    }
    if (src_ufcs && src_ufcs_len) {
        char* lowered_system_includes = cc_rewrite_system_cch_includes_to_lowered_headers(src_ufcs, src_ufcs_len);
        if (lowered_system_includes) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = lowered_system_includes;
            src_ufcs_len = strlen(lowered_system_includes);
        }
    }

    if (src_ufcs && src_ufcs_len) {
        char* rewritten = cc_rewrite_nursery_create_destroy_proto(src_ufcs, src_ufcs_len, ctx->input_path);
        if (rewritten == (char*)-1) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            return EINVAL;
        }
        if (rewritten) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = strlen(rewritten);
        }
    }

    if (src_ufcs && src_ufcs_len && ctx->symbols) {
        char* rewritten = cc_rewrite_registered_type_create_destroy(src_ufcs, src_ufcs_len, ctx->input_path, ctx->symbols);
        if (rewritten == (char*)-1) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            return EINVAL;
        }
        if (rewritten) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = strlen(rewritten);
        }
    }

    /* Rewrite `if @try (T x = expr) { ... }` into expanded form */
    if (src_ufcs && src_ufcs_len) {
        char* rewritten = cc__rewrite_if_try_syntax(src_ufcs, src_ufcs_len);
        if (rewritten) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = strlen(rewritten);
        }
    }

    /* Rewrite generic container syntax: CCVec<T> -> CCVec_T, cc_vec_new<T>() -> CCVec_T_init() */
    if (src_ufcs && src_ufcs_len) {
        char* rewritten = cc_rewrite_generic_containers(src_ufcs, src_ufcs_len, ctx->input_path);
        if (rewritten) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = strlen(rewritten);
        }
    }

    /* Rewrite `with_deadline(expr) { ... }` (not valid C) into CCDeadline scope syntax
       using @defer, so the rest of the pipeline sees valid parseable text. */
    if (src_ufcs && src_ufcs_len) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_with_deadline_syntax(src_ufcs, src_ufcs_len, &rewritten, &rewritten_len) == 0 && rewritten) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }

    /* Rewrite `@match { ... }` into valid C before any node-based rewrites. */
    if (src_ufcs && src_ufcs_len) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        int r = cc__rewrite_match_syntax(ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
        if (r < 0) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            return EINVAL;
        }
        if (r > 0 && rewritten) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }

    /* Produced by the closure-literal AST pass (emitted into the output TU). */
    char* closure_protos = NULL;
    size_t closure_protos_len = 0;
    char* closure_defs = NULL;
    size_t closure_defs_len = 0;

    /* Phase 3 currently uses coarse whole-file rewrites for several AST-driven
       passes. Run them sequentially with reparsing between changed passes so
       whole-file snapshots do not collide in the shared edit buffer. */
#ifdef CC_TCC_EXT_AVAILABLE
    if (src_ufcs && root && root->nodes && root->node_count > 0 && ctx->symbols) {
        const CCASTRoot* phase3_root = root;
        CCASTRoot* phase3_owned_root = NULL;
        int phase3_changed = 0;
        cc__collect_registered_ufcs_var_types(ctx->symbols, src_ufcs, src_ufcs_len);
        if (cc__apply_coarse_codegen_pass(phase3_root, ctx, &src_ufcs, &src_ufcs_len,
                                          src_all, cc__collect_ufcs_edits, &phase3_changed) < 0) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }
        if (phase3_changed) {
            phase3_owned_root = cc__reparse_source_to_ast(src_ufcs, src_ufcs_len, ctx->input_path, ctx->symbols,
                                                          "phase3 after coarse UFCS rewrite");
            if (!phase3_owned_root) {
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(src_all);
                free(closure_protos);
                free(closure_defs);
                return EINVAL;
            }
            phase3_root = phase3_owned_root;
        }
        if (cc__apply_coarse_codegen_pass(phase3_root, ctx, &src_ufcs, &src_ufcs_len,
                                          src_all, cc__collect_closure_calls_edits, &phase3_changed) < 0) {
            if (phase3_owned_root) cc_tcc_bridge_free_ast(phase3_owned_root);
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }
        if (phase3_changed) {
            if (getenv("CC_DEBUG_POST_AUTOBLOCK_DUMP") && src_ufcs) {
                const char* dump_path = getenv("CC_DEBUG_POST_AUTOBLOCK_DUMP");
                FILE* df = dump_path ? fopen(dump_path, "w") : NULL;
                if (df) {
                    fwrite(src_ufcs, 1, src_ufcs_len, df);
                    fclose(df);
                }
            }
            if (phase3_owned_root) cc_tcc_bridge_free_ast(phase3_owned_root);
            phase3_owned_root = cc__reparse_source_to_ast(src_ufcs, src_ufcs_len, ctx->input_path, ctx->symbols,
                                                          "phase3 after closure-call rewrite");
            if (!phase3_owned_root) {
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(src_all);
                free(closure_protos);
                free(closure_defs);
                return EINVAL;
            }
            phase3_root = phase3_owned_root;
        }
        if (cc__apply_coarse_codegen_pass(phase3_root, ctx, &src_ufcs, &src_ufcs_len,
                                          src_all, cc__collect_autoblocking_edits, &phase3_changed) < 0) {
            if (phase3_owned_root) cc_tcc_bridge_free_ast(phase3_owned_root);
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }
        if (phase3_changed) {
            if (getenv("CC_DEBUG_POST_AUTOBLOCK_DUMP") && src_ufcs) {
                const char* dump_path = getenv("CC_DEBUG_POST_AUTOBLOCK_DUMP");
                FILE* df = dump_path ? fopen(dump_path, "w") : NULL;
                if (df) {
                    fwrite(src_ufcs, 1, src_ufcs_len, df);
                    fclose(df);
                }
            }
            if (phase3_owned_root) cc_tcc_bridge_free_ast(phase3_owned_root);
            phase3_owned_root = cc__reparse_source_to_ast(src_ufcs, src_ufcs_len, ctx->input_path, ctx->symbols,
                                                          "phase3 after autoblock rewrite");
            if (!phase3_owned_root) {
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(src_all);
                free(closure_protos);
                free(closure_defs);
                return EINVAL;
            }
            phase3_root = phase3_owned_root;
        }
        if (cc__apply_coarse_codegen_pass(phase3_root, ctx, &src_ufcs, &src_ufcs_len,
                                          src_all, cc__collect_await_normalize_edits, &phase3_changed) < 0) {
            if (phase3_owned_root) cc_tcc_bridge_free_ast(phase3_owned_root);
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }
        if (phase3_owned_root) cc_tcc_bridge_free_ast(phase3_owned_root);

        /* Debug output for await rewrite */
        if (getenv("CC_DEBUG_AWAIT_REWRITE") && src_ufcs) {
            const char* needle = "@async int f";
            const char* p = strstr(src_ufcs, needle);
            if (!p) p = strstr(src_ufcs, "@async");
            if (p) {
                fprintf(stderr, "CC_DEBUG_AWAIT_REWRITE: ---- snippet ----\n");
                size_t off = (size_t)(p - src_ufcs);
                size_t take = 800;
                if (off + take > src_ufcs_len) take = src_ufcs_len - off;
                fwrite(p, 1, take, stderr);
                fprintf(stderr, "\nCC_DEBUG_AWAIT_REWRITE: ---- end ----\n");
            }
        }
    }
#endif

    /* Text-based rewrites that must happen BEFORE creating root3 AST, so AST positions
       match the transformed source. These are text-based and don't need AST. */
    if (src_ufcs && ctx) {
        /* Lower `cc_channel_pair(&tx, &rx);` BEFORE channel type rewrite (it needs `[~]` patterns). */
        {
            size_t rp_len = 0;
            char* rp = cc__rewrite_channel_pair_calls_text(ctx, src_ufcs, src_ufcs_len, &rp_len);
            if (!rp) {
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(src_all);
                return EINVAL;
            }
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rp;
            src_ufcs_len = rp_len;
        }

        /* Rewrite cc_channel_send_task(ch, closure) BEFORE channel type rewrite.
           This wraps the closure body to store result in fiber-local storage. */
        if (strstr(src_ufcs, "send_task") != NULL || strstr(src_ufcs, "cc_channel_send_task") != NULL) {
            size_t st_len = 0;
            char* st = cc__rewrite_chan_send_task_text(ctx, src_ufcs, src_ufcs_len, &st_len);
            if (st) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = st;
                src_ufcs_len = st_len;
            }
        }

        /* Rewrite channel handle types (including owned channels) BEFORE creating AST.
           This transforms `int[~4 >]` -> `CCChanTx`, and `T[~N owned {...}]` -> `cc_chan_create_owned(...)`.
           T[~N ordered <] becomes CCChanRx (ordered is a flag on the channel).
           Must happen before AST creation so closure positions are correct. */
        {
            char* rew = cc__rewrite_chan_handle_types_text(ctx, src_ufcs, src_ufcs_len);
            if (rew) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew;
                src_ufcs_len = strlen(src_ufcs);
            }
        }
    }

    /* Internal reparses need parser-safe container receiver types too. When
       src_ufcs still carries raw `Vec<T>` / `Map<K,V>` syntax, direct field UFCS
       like `c.items.push(...)` can survive the initial parse but fail later
       during these in-memory reparses. Canonicalize containers here so the
       subsequent reparses see the same receiver shapes as the main parser. */
    if (src_ufcs && ctx && ctx->input_path) {
        char* rew = cc_rewrite_generic_containers(src_ufcs, src_ufcs_len, ctx->input_path);
        if (rew) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rew;
            src_ufcs_len = strlen(src_ufcs);
        }
    }
    /* Some concrete UFCS families are still parser-fragile in nested expression
       contexts (for example Vec methods inside printf args, or CCFile/CCString
       methods that TCC records inconsistently). Normalize those known-safe
       receiver families before the reparse-driven AST passes so later stages see
       stable lowered calls instead of relying on span matching. */
    if (src_ufcs && ctx) {
        char* rew = cc_rewrite_generic_family_ufcs_parser_safe(src_ufcs, src_ufcs_len);
        if (rew) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rew;
            src_ufcs_len = strlen(src_ufcs);
        }
    }

    /* Reparse the current TU source to get an up-to-date stub-AST for statement-level lowering.
       These rewrites run before marker stripping to keep spans stable. */
    if (src_ufcs && ctx && ctx->symbols) {
        cc__debug_dump_reparse_source("stage1_pre_stmt", src_ufcs, src_ufcs_len, ctx->input_path);
        CCASTRoot* root3 = cc__reparse_source_to_ast(src_ufcs, src_ufcs_len, ctx->input_path, ctx->symbols,
                                                     "statement-lowering input");
        if (!root3) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            return EINVAL;
        }

        /* 1) closure literals -> __cc_closure_make_N(...) + generated closure defs */
        {
            char* rewritten = NULL;
            size_t rewritten_len = 0;
            char* protos = NULL;
            size_t protos_len = 0;
            char* defs = NULL;
            size_t defs_len = 0;
            int r = cc__rewrite_closure_literals_with_nodes(root3, ctx, src_ufcs, src_ufcs_len,
                                                           &rewritten, &rewritten_len,
                                                           &protos, &protos_len,
                                                           &defs, &defs_len);
            if (r < 0) {
                cc_tcc_bridge_free_ast(root3);
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(src_all);
                return EINVAL;
            }
            if (r > 0) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = rewritten_len;
            } else {
                free(rewritten);
            }
            if (protos) { free(closure_protos); closure_protos = protos; closure_protos_len = protos_len; }
            if (defs) { free(closure_defs); closure_defs = defs; closure_defs_len = defs_len; }
        }
        cc_tcc_bridge_free_ast(root3);
    }

    /* Lower @defer (and hard-error on cancel) using a syntax-driven pass.
       IMPORTANT: this must run BEFORE async lowering so `@defer` can be made suspend-safe. */
    if (src_ufcs && (strstr(src_ufcs, "@defer") != NULL || strstr(src_ufcs, "cancel") != NULL)) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        int r = cc__rewrite_defer_syntax(ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
        if (r < 0) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }
        if (r > 0) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }

    /* AST-driven @async lowering (state machine).
       IMPORTANT: run after statement-level lowering so closure rewrites are already reflected. */
    if (src_ufcs && ctx && ctx->symbols &&
        (strstr(src_ufcs, "@async") != NULL || strstr(src_ufcs, "await") != NULL)) {
        cc__debug_dump_reparse_source("stage2_pre_async", src_ufcs, src_ufcs_len, ctx->input_path);
        CCASTRoot* root2 = cc__reparse_source_to_ast(src_ufcs, src_ufcs_len, ctx->input_path, ctx->symbols,
                                                     "async-lowering input");
        if (getenv("CC_DEBUG_REPARSE")) {
            fprintf(stderr, "CC: reparse: stub ast node_count=%d\n", root2 ? root2->node_count : -1);
        }
        if (!root2) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }

        char* rewritten = NULL;
        size_t rewritten_len = 0;
        int ar = cc_async_rewrite_state_machine_ast(root2, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
        cc_tcc_bridge_free_ast(root2);
        if (ar < 0) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }
        if (ar > 0) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }

    /* Strip CC decl markers so output is valid C (run after async lowering so it can see `@async`). */
    if (src_ufcs) {
        char* stripped = NULL;
        size_t stripped_len = 0;
        if (cc__strip_cc_decl_markers(src_ufcs, src_ufcs_len, &stripped, &stripped_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = stripped;
            src_ufcs_len = stripped_len;
        }
    }

    /* NOTE: slice move/provenance checking is now handled by the stub-AST checker pass
       (`cc/src/visitor/checker.c`) before visitor lowering. */

    fprintf(out, "/* CC lowered C output */\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <stdint.h>\n");
    /* Include lowered headers (.h) - these are generated from .cch files
       with CC type syntax transformed to valid C. Lowered headers live in
       out/include/ which should be added to include path before cc/include/. */
    fprintf(out, "#include <ccc/cc_nursery.h>\n");
    fprintf(out, "#include <ccc/cc_closure.h>\n");
    fprintf(out, "#include <ccc/cc_slice.h>\n");
    fprintf(out, "#include <ccc/cc_runtime.h>\n");
    fprintf(out, "#include <ccc/std/io.h>\n");  /* CCFile for closure captures */
    fprintf(out, "#include <ccc/std/task.h>\n");
    /* Helper alias: used for auto-blocking arg binding to avoid accidental hoisting of these temps. */
    fprintf(out, "typedef intptr_t CCAbIntptr;\n");
    
    /* TSan macros and spawn helpers */
    fprintf(out, "#include <ccc/cc_closure_helper.h>\n\n");

    /* Build container type declarations from type registry (populated by generic rewriting).
       These are buffered and inserted into the source AFTER user type definitions
       so that user-defined types (e.g. Entry in Map<K, Entry*>) are visible. */
    char* container_decl_buf = NULL;
    size_t container_decl_len = 0, container_decl_cap = 0;
    {
        CCTypeRegistry* reg = cc_type_registry_get_global();
        if (reg) {
            size_t n_vec = cc_type_registry_vec_count(reg);
            size_t n_map = cc_type_registry_map_count(reg);
            int emit_container_decls = (n_map > 0);
            if (!emit_container_decls) {
                for (size_t i = 0; i < n_vec; i++) {
                    const CCTypeInstantiation* inst = cc_type_registry_get_vec(reg, i);
                    if (!inst || !inst->mangled_name) continue;
                    if (strcmp(inst->mangled_name, "CCVec_char") != 0) {
                        emit_container_decls = 1;
                        break;
                    }
                }
            }

            if (emit_container_decls) {
                fprintf(out, "/* --- CC generic container declarations --- */\n");
                fprintf(out, "#include <ccc/std/vec.h>\n");
                fprintf(out, "#include <ccc/std/map.h>\n");
                fprintf(out, "#include <ccc/cc_channel.h>\n");
                fprintf(out, "/* --- end container declarations (macros inserted after typedefs) --- */\n\n");

                cc__sb_append_cstr_local(&container_decl_buf, &container_decl_len, &container_decl_cap,
                    "/* --- CC container type macros (auto-positioned after typedefs) --- */\n");
                cc__sb_append_cstr_local(&container_decl_buf, &container_decl_len, &container_decl_cap,
                    "#ifndef CC_PARSER_MODE\n");
                
                /* Emit Vec declarations */
                for (size_t i = 0; i < n_vec; i++) {
                    const CCTypeInstantiation* inst = cc_type_registry_get_vec(reg, i);
                    if (inst && inst->type1 && inst->mangled_name) {
                        const char* mangled_elem = inst->mangled_name + 6; /* Skip "CCVec_" */
                        
                        if (strcmp(mangled_elem, "char") == 0) {
                            continue;
                        }
                        
                        int opt_predeclared = (strcmp(mangled_elem, "int") == 0 ||
                                               strcmp(mangled_elem, "bool") == 0 ||
                                               strcmp(mangled_elem, "char") == 0 ||
                                               strcmp(mangled_elem, "size_t") == 0 ||
                                               strcmp(mangled_elem, "voidptr") == 0 ||
                                               strcmp(mangled_elem, "charptr") == 0 ||
                                               strcmp(mangled_elem, "long") == 0 ||
                                               strcmp(mangled_elem, "short") == 0 ||
                                               strcmp(mangled_elem, "float") == 0 ||
                                               strcmp(mangled_elem, "double") == 0 ||
                                               strcmp(mangled_elem, "void") == 0 ||
                                               strcmp(mangled_elem, "CCSlice") == 0 ||
                                               strcmp(mangled_elem, "CCSliceUnique") == 0);
                        int is_complex = (!opt_predeclared ||
                                          strchr(inst->type1, '*') != NULL || 
                                          strncmp(inst->type1, "struct ", 7) == 0 ||
                                          strncmp(inst->type1, "union ", 6) == 0);
                        if (is_complex) {
                            if (!opt_predeclared) {
                                char line[512];
                                snprintf(line, sizeof(line), "CC_DECL_OPTIONAL(CCOptional_%s, %s)\n", mangled_elem, inst->type1);
                                cc__sb_append_cstr_local(&container_decl_buf, &container_decl_len, &container_decl_cap, line);
                            }
                            {
                                char line[512];
                                snprintf(line, sizeof(line), "CC_VEC_DECL_ARENA_FULL(%s, %s, CCOptional_%s)\n", 
                                        inst->type1, inst->mangled_name, mangled_elem);
                                cc__sb_append_cstr_local(&container_decl_buf, &container_decl_len, &container_decl_cap, line);
                            }
                        } else {
                            char line[512];
                            snprintf(line, sizeof(line), "CC_VEC_DECL_ARENA(%s, %s)\n", inst->type1, inst->mangled_name);
                            cc__sb_append_cstr_local(&container_decl_buf, &container_decl_len, &container_decl_cap, line);
                        }
                    }
                }
                
                /* Emit Map declarations (with CC_DECL_OPTIONAL for value type) */
                for (size_t i = 0; i < n_map; i++) {
                    const CCTypeInstantiation* inst = cc_type_registry_get_map(reg, i);
                    if (inst && inst->type1 && inst->type2 && inst->mangled_name) {
                        const char* hash_fn = "cc_kh_hash_i32";
                        const char* eq_fn = "cc_kh_eq_i32";
                        if (strcmp(inst->type1, "int") == 0) {
                            hash_fn = "cc_kh_hash_i32"; eq_fn = "cc_kh_eq_i32";
                        } else if (strstr(inst->type1, "64") != NULL) {
                            hash_fn = "cc_kh_hash_u64"; eq_fn = "cc_kh_eq_u64";
                        } else if (strstr(inst->type1, "slice") != NULL || strstr(inst->type1, "Slice") != NULL || strcmp(inst->type1, "charslice") == 0) {
                            hash_fn = "cc_kh_hash_slice"; eq_fn = "cc_kh_eq_slice";
                        }
                        char mangled_val[128];
                        cc_result_spec_mangle_type(inst->type2, strlen(inst->type2), mangled_val, sizeof(mangled_val));
                        {
                            int opt_predeclared = (strcmp(mangled_val, "charptr") == 0 ||
                                                   strcmp(mangled_val, "intptr") == 0 ||
                                                   strcmp(mangled_val, "voidptr") == 0 ||
                                                   strcmp(mangled_val, "int") == 0 ||
                                                   strcmp(mangled_val, "char") == 0);
                            if (!opt_predeclared) {
                                char line[512];
                                snprintf(line, sizeof(line), "CC_DECL_OPTIONAL(CCOptional_%s, %s)\n", mangled_val, inst->type2);
                                cc__sb_append_cstr_local(&container_decl_buf, &container_decl_len, &container_decl_cap, line);
                            }
                        }
                        {
                            char line[512];
                            snprintf(line, sizeof(line), "CC_MAP_DECL_ARENA_FULL(%s, %s, %s, CCOptional_%s, %s, %s)\n", 
                                    inst->type1, inst->type2, inst->mangled_name, mangled_val, hash_fn, eq_fn);
                            cc__sb_append_cstr_local(&container_decl_buf, &container_decl_len, &container_decl_cap, line);
                        }
                    }
                }

                cc__sb_append_cstr_local(&container_decl_buf, &container_decl_len, &container_decl_cap,
                    "#endif /* !CC_PARSER_MODE */\n");
                cc__sb_append_cstr_local(&container_decl_buf, &container_decl_len, &container_decl_cap,
                    "/* --- end container type macros --- */\n");
            }
        }
    }

    /* Result type declarations are emitted later, after they're collected during
       cc__rewrite_result_types_text processing. */

    /* Preserve diagnostics mapping to the original input (repo-relative for readability). */
    {
        cc__emit_line_directive(out, 1, src_path);
    }

    if (src_ufcs) {
        /* Lower `cc_channel_pair(&tx, &rx);` into `cc_channel_pair_create(...)` */
        {
            size_t rp_len = 0;
            char* rp = cc__rewrite_channel_pair_calls_text(ctx, src_ufcs, src_ufcs_len, &rp_len);
            if (!rp) {
                fclose(out);
                return EINVAL;
            }
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rp;
            src_ufcs_len = rp_len;
        }
        /* Rewrite cc_channel_send_task(ch, closure) */
        if (strstr(src_ufcs, "send_task") != NULL || strstr(src_ufcs, "cc_channel_send_task") != NULL) {
            size_t st_len = 0;
            char* st = cc__rewrite_chan_send_task_text(ctx, src_ufcs, src_ufcs_len, &st_len);
            if (st) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = st;
                src_ufcs_len = st_len;
            }
        }
        /* Final safety: ensure invalid surface syntax like `T[~ ... >]` does not reach the C compiler. */
        {
            char* rew_string = cc_rewrite_string_templates_text(src_ufcs, src_ufcs_len, ctx->input_path);
            if (rew_string == (char*)-1) {
                fclose(out);
                return EINVAL;
            }
            if (rew_string) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_string;
                src_ufcs_len = strlen(src_ufcs);
            }
        }
        {
            char* rew_slice = cc__rewrite_slice_types_text(ctx, src_ufcs, src_ufcs_len);
            if (!rew_slice) {
                fclose(out);
                return EINVAL;
            }
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rew_slice;
            src_ufcs_len = strlen(src_ufcs);
        }
        {
            char* rew = cc__rewrite_chan_handle_types_text(ctx, src_ufcs, src_ufcs_len);
            if (!rew) {
            fclose(out);
                return EINVAL;
            }
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rew;
            src_ufcs_len = strlen(src_ufcs);
        }
        /* Reset type registries once before the type-rewriting passes so they
           accumulate correctly across the optional and result scans below.
           Previously each scan function reset on entry, losing types collected
           by earlier calls in the same compilation unit. */
        cc__cg_reset_type_registries();
        cc_result_spec_table_set_global(&cc__cg_result_specs);
        /* Rewrite T? -> CCOptional_T */
        {
            if (getenv("CC_DEBUG_OPTIONAL")) fprintf(stderr, "CC: DEBUG: calling cc__rewrite_optional_types_text, len=%zu\n", src_ufcs_len);
            char* rew_opt = cc__rewrite_optional_types_text(ctx, src_ufcs, src_ufcs_len);
            if (getenv("CC_DEBUG_OPTIONAL")) fprintf(stderr, "CC: DEBUG: rew_opt=%p\n", (void*)rew_opt);
            if (rew_opt) {
            if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_opt;
                src_ufcs_len = strlen(src_ufcs);
                if (getenv("CC_DEBUG_OPTIONAL")) fprintf(stderr, "CC: DEBUG: new len=%zu\n", src_ufcs_len);
            }
        }
        /* Rewrite T!>(E) -> CCResult_T_E and collect result type pairs */
        {
            char* rew_res = cc__rewrite_result_types_text(ctx, src_ufcs, src_ufcs_len);
            if (rew_res) {
            if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_res;
                src_ufcs_len = strlen(src_ufcs);
            }
            
            /* Result type declarations are now emitted at file scope in the header section,
               so no need to splice them into source here. */
        }
        /* Result field sugar:
           `res.value` / `res.error` -> `res.u.value` / `res.u.error`
           while keeping the compact union ABI in generated C. */
        {
            char* rew_res_fields = cc__rewrite_result_field_sugar_text(ctx, src_ufcs, src_ufcs_len);
            if (rew_res_fields) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_res_fields;
                src_ufcs_len = strlen(src_ufcs);
            }
        }
        /* Insert optional type declarations for custom types.
           Each CC_DECL_OPTIONAL is inserted right before the first use of that specific
           optional type, to ensure the underlying type is defined by then. */
        if (cc__cg_optional_type_count > 0) {
            /* Sort types by their first usage position (descending) so we can insert
               from end to start without invalidating positions */
            size_t* type_positions = (size_t*)malloc(cc__cg_optional_type_count * sizeof(size_t));
            size_t* sorted_indices_buf = (size_t*)malloc(cc__cg_optional_type_count * sizeof(size_t));
            if (!type_positions || !sorted_indices_buf) {
                free(type_positions);
                free(sorted_indices_buf);
                goto skip_optional_decls;
            }
            for (size_t oi = 0; oi < cc__cg_optional_type_count; oi++) {
                CCCodegenOptionalType* p = &cc__cg_optional_types[oi];
                char pattern1[256], pattern2[256];
                snprintf(pattern1, sizeof(pattern1), "CCOptional_%s", p->mangled_type);
                snprintf(pattern2, sizeof(pattern2), "__CC_OPTIONAL(%s)", p->mangled_type);
                const char* found1 = strstr(src_ufcs, pattern1);
                const char* found2 = strstr(src_ufcs, pattern2);
                size_t pos = src_ufcs_len;
                if (found1 && (size_t)(found1 - src_ufcs) < pos) {
                    pos = (size_t)(found1 - src_ufcs);
                }
                if (found2 && (size_t)(found2 - src_ufcs) < pos) {
                    pos = (size_t)(found2 - src_ufcs);
                }
                /* Back up to start of line/function */
                if (pos < src_ufcs_len) {
                    size_t search_pos = pos;
                    while (search_pos > 0) {
                        size_t line_start = search_pos;
                        while (line_start > 0 && src_ufcs[line_start - 1] != '\n') line_start--;
                        const char* line = src_ufcs + line_start;
                        while (*line == ' ' || *line == '\t') line++;
                        if ((strncmp(line, "int ", 4) == 0 || strncmp(line, "void ", 5) == 0 ||
                             strncmp(line, "static ", 7) == 0 || strncmp(line, "CCOptional_", 11) == 0 ||
                             strncmp(line, "__CC_OPTIONAL", 13) == 0 || strncmp(line, "typedef ", 8) == 0) &&
                            strchr(line, '(') != NULL) {
                            pos = line_start;
                            break;
                        }
                        if (line_start == 0) break;
                        search_pos = line_start - 1;
                    }
                }
                type_positions[oi] = pos;
            }
            
            /* Sort indices by position descending (bubble sort is fine for small N) */
            size_t* sorted_indices = sorted_indices_buf;
            for (size_t i = 0; i < cc__cg_optional_type_count; i++) sorted_indices[i] = i;
            for (size_t i = 0; i < cc__cg_optional_type_count; i++) {
                for (size_t j = i + 1; j < cc__cg_optional_type_count; j++) {
                    if (type_positions[sorted_indices[j]] > type_positions[sorted_indices[i]]) {
                        size_t tmp = sorted_indices[i];
                        sorted_indices[i] = sorted_indices[j];
                        sorted_indices[j] = tmp;
                    }
                }
            }
            
            /* Insert each declaration at its position (from end to start) */
            for (size_t si = 0; si < cc__cg_optional_type_count; si++) {
                size_t oi = sorted_indices[si];
                size_t insert_offset = type_positions[oi];
                if (insert_offset >= src_ufcs_len) continue;
                
                CCCodegenOptionalType* p = &cc__cg_optional_types[oi];
                char decl[512];
                snprintf(decl, sizeof(decl), 
                    "/* CC optional for %s */\nCC_DECL_OPTIONAL(CCOptional_%s, %s)\n",
                    p->raw_type, p->mangled_type, p->raw_type);
                
                /* Build new source: prefix + decl + suffix */
                char* new_src = NULL;
                size_t new_len = 0, new_cap = 0;
                cc__sb_append_local(&new_src, &new_len, &new_cap, src_ufcs, insert_offset);
                cc__sb_append_cstr_local(&new_src, &new_len, &new_cap, decl);
                cc__sb_append_local(&new_src, &new_len, &new_cap, 
                                    src_ufcs + insert_offset, src_ufcs_len - insert_offset);
                
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = new_src;
                src_ufcs_len = new_len;
            }
            free(type_positions);
            free(sorted_indices_buf);
        }
        skip_optional_decls:;
        /* Rewrite cc_ok(v) -> cc_ok_CCResult_T_E(v) based on enclosing function return type */
        {
            char* rew_infer = cc__rewrite_inferred_result_constructors(src_ufcs, src_ufcs_len);
            if (rew_infer) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_infer;
                src_ufcs_len = strlen(src_ufcs);
            }
        }
        /* Lower @err / @errhandler / <? / =<! ... @err for host C emission (parse already
           preprocesses these; src_ufcs is still the on-disk-shaped TU until here). */
        if (src_ufcs && src_ufcs_len &&
            (strstr(src_ufcs, "?>") != NULL || strstr(src_ufcs, "!>") != NULL)) {
            char* ru_out = NULL;
            size_t ru_out_len = 0;
            int ru_r = cc__rewrite_result_unwrap(ctx, src_ufcs, src_ufcs_len, &ru_out, &ru_out_len);
            if (ru_r < 0) {
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(closure_protos);
                free(closure_defs);
                return EINVAL;
            }
            if (ru_r > 0 && ru_out) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = ru_out;
                src_ufcs_len = ru_out_len;
            }
        }
        if (src_ufcs && src_ufcs_len &&
            (strstr(src_ufcs, "@errhandler") != NULL || strstr(src_ufcs, "@err") != NULL ||
             strstr(src_ufcs, "=<!") != NULL || strstr(src_ufcs, "<?") != NULL)) {
            char* err_out = NULL;
            size_t err_out_len = 0;
            int err_r = cc__rewrite_err_syntax(ctx, src_ufcs, src_ufcs_len, &err_out, &err_out_len);
            if (err_r < 0) {
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(closure_protos);
                free(closure_defs);
                return EINVAL;
            }
            if (err_r > 0 && err_out) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = err_out;
                src_ufcs_len = err_out_len;
            }
        }
        /* Rewrite try expr -> cc_try(expr) */
        {
            char* rew_try = cc__rewrite_try_exprs_text(ctx, src_ufcs, src_ufcs_len);
            if (rew_try) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_try;
                src_ufcs_len = strlen(src_ufcs);
            }
        }
        /* Rewrite *opt -> cc_unwrap_opt(opt) for optional variables */
        {
            char* rew_unwrap = cc__rewrite_optional_unwrap_text(ctx, src_ufcs, src_ufcs_len);
            if (rew_unwrap) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_unwrap;
                src_ufcs_len = strlen(src_ufcs);
            }
        }
        /* Keep generic-family UFCS in source form here. The final AST-aware UFCS
           pass has better receiver type information, while the older parser-safe
           textual rewrite can leak parser-generic names like __cc_vec_generic_* into
           emitted host C. */
        {
            char* rew_channel = cc_rewrite_channel_ufcs_concrete(src_ufcs, src_ufcs_len);
            if (rew_channel) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_channel;
                src_ufcs_len = strlen(src_ufcs);
            }
        }
        {
            size_t st_len = 0;
            char* st = cc__rewrite_chan_send_task_text(ctx, src_ufcs, src_ufcs_len, &st_len);
            if (st) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = st;
                src_ufcs_len = st_len;
            }
        }
        /* Some later lowering stages can still synthesize raw `@defer ...;`
           forms. Normalize them again before the final UFCS reparse so
           expressions like `@defer arena.free();` don't reach strict UFCS
           dispatch with `@defer arena` as the apparent receiver. */
        if (strstr(src_ufcs, "@defer") != NULL || strstr(src_ufcs, "cancel") != NULL) {
            char* rewritten = NULL;
            size_t rewritten_len = 0;
            int r = cc__rewrite_defer_syntax(ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
            if (r < 0) {
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(closure_protos);
                free(closure_defs);
                return EINVAL;
            }
            if (r > 0) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = rewritten_len;
            }
        }
        /* Final UFCS sweep: earlier statement/syntax rewrites can synthesize new
           method-call surface syntax (notably via @defer / spawn / nursery
           lowering). Reparse the current source and lower any remaining UFCS
           spans before emitting C. */
        if (ctx && ctx->symbols) {
            CCTypeRegistry* saved_reg = cc_type_registry_get_global();
            CCTypeRegistry* temp_reg = cc_type_registry_new();
            cc__debug_dump_reparse_source("stage3_pre_final_ufcs", src_ufcs, src_ufcs_len, ctx->input_path);
            CCASTRoot* final_ufcs_root = cc__reparse_source_to_ast(src_ufcs, src_ufcs_len, ctx->input_path, ctx->symbols,
                                                                   "final-UFCS input");
            if (!final_ufcs_root) {
                if (temp_reg) cc_type_registry_free(temp_reg);
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(closure_protos);
                free(closure_defs);
                return EINVAL;
            }

            if (temp_reg) cc_type_registry_set_global(temp_reg);
            cc__collect_registered_ufcs_var_types(ctx->symbols, src_ufcs, src_ufcs_len);
            CCEditBuffer eb;
            cc_edit_buffer_init(&eb, src_ufcs, src_ufcs_len);
            if (cc__collect_ufcs_edits(final_ufcs_root, ctx, &eb) < 0) {
                cc_tcc_bridge_free_ast(final_ufcs_root);
                if (temp_reg) {
                    cc_type_registry_set_global(saved_reg);
                    cc_type_registry_free(temp_reg);
                }
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(src_all);
                free(closure_protos);
                free(closure_defs);
                return EINVAL;
            }
            if (eb.count > 0) {
                size_t new_len = 0;
                char* rewritten = cc_edit_buffer_apply(&eb, &new_len);
                if (rewritten) {
                    if (src_ufcs != src_all) free(src_ufcs);
                    src_ufcs = rewritten;
                    src_ufcs_len = new_len;
                }
            }
            cc_edit_buffer_free(&eb);
            cc_tcc_bridge_free_ast(final_ufcs_root);
            if (temp_reg) {
                cc_type_registry_set_global(saved_reg);
                cc_type_registry_free(temp_reg);
            }
        }

        /* Final concrete-family sweep: pointer-backed family types like Map_* can
           still survive the AST UFCS pass in raw surface form (for example
           `m.insert(...)` where `m` is already `Map_K_V*`). Normalize any
           remaining concrete family UFCS before emitting host C. */
        {
            char* rewritten = cc_rewrite_generic_family_ufcs_concrete(src_ufcs, src_ufcs_len);
            if (rewritten) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = strlen(rewritten);
            }
        }
        {
            char* rewritten = cc__rewrite_result_helper_family_to_visible_type(src_ufcs, src_ufcs_len);
            if (rewritten) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = strlen(rewritten);
            }
        }
        {
            char* rewritten = cc__rewrite_parser_generic_family_helpers_to_concrete(src_ufcs, src_ufcs_len);
            if (rewritten) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = strlen(rewritten);
            }
        }
        {
            char* rewritten = cc__rewrite_string_helper_family_to_visible_type(src_ufcs, src_ufcs_len);
            if (rewritten) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = strlen(rewritten);
            }
        }

        /* Insert result type declarations INTO the source at the right position.
           They must come AFTER custom type definitions but BEFORE functions that use them.
           Find the first CCResult_ usage and insert before that line (at file scope). */
        if (cc__cg_result_specs.count > 0) {
            /* Find first usage of any CCResult_T_E type in the source */
            size_t earliest_pos = src_ufcs_len;
            for (size_t ri = 0; ri < cc__cg_result_specs.count; ri++) {
                const CCResultSpec* spec = cc_result_spec_table_get(&cc__cg_result_specs, ri);
                const char* pattern = spec ? spec->concrete_name : NULL;
                const char* found = strstr(src_ufcs, pattern);
                if (found && (size_t)(found - src_ufcs) < earliest_pos) {
                    earliest_pos = (size_t)(found - src_ufcs);
                }
            }
            
            if (earliest_pos < src_ufcs_len) {
                /* Back up to start of line */
                while (earliest_pos > 0 && src_ufcs[earliest_pos - 1] != '\n') {
                    earliest_pos--;
                }
                
                /* Build declaration string */
                char* decls = NULL;
                size_t decls_len = 0, decls_cap = 0;
                cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap,
                    "/* --- CC result type declarations (auto-generated) --- */\n");
                cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap,
                    "#ifndef CC_PARSER_MODE\n");
                for (size_t ri = 0; ri < cc__cg_result_specs.count; ri++) {
                    const CCResultSpec* spec = cc_result_spec_table_get(&cc__cg_result_specs, ri);
                    char line[512];
                    if (!spec) continue;
                    cc_result_spec_emit_decl(spec, line, sizeof(line));
                    cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap, line);
                }
                cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap,
                    "#endif /* !CC_PARSER_MODE */\n");
                cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap,
                    "/* --- end result type declarations --- */\n\n");
                
                /* Build new source: prefix + decls + suffix */
                char* new_src = NULL;
                size_t new_len = 0, new_cap = 0;
                cc__sb_append_local(&new_src, &new_len, &new_cap, src_ufcs, earliest_pos);
                cc__sb_append_local(&new_src, &new_len, &new_cap, decls, decls_len);
                cc__sb_append_local(&new_src, &new_len, &new_cap,
                                    src_ufcs + earliest_pos, src_ufcs_len - earliest_pos);
                
                free(decls);
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = new_src;
                src_ufcs_len = new_len;
            }
        }

        {
            char* rewritten = NULL;
            CCTypeRegistry* saved_reg = cc_type_registry_get_global();
            CCTypeRegistry* temp_reg = cc_type_registry_new();
            if (temp_reg) {
                cc_type_registry_set_global(temp_reg);
                if (ctx && ctx->symbols) {
                    cc__collect_registered_ufcs_var_types(ctx->symbols, src_ufcs, src_ufcs_len);
                }
            }
            rewritten = cc__rewrite_parser_placeholder_ufcs_lowers(src_ufcs, src_ufcs_len);
            if (temp_reg) {
                cc_type_registry_set_global(saved_reg);
                cc_type_registry_free(temp_reg);
            }
            if (rewritten) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = strlen(rewritten);
            }
        }
        
        /* Insert buffered container declarations after typedefs/structs but BEFORE
           any struct that references a container type (e.g. __CC_MAP, Map_). */
        if (container_decl_buf && container_decl_len > 0) {
            size_t ctnr_pos = 0;
            while (ctnr_pos < src_ufcs_len) {
                size_t line_start = ctnr_pos;
                size_t line_end = line_start;
                while (line_end < src_ufcs_len && src_ufcs[line_end] != '\n') line_end++;
                size_t p = line_start;
                while (p < line_end && (src_ufcs[p] == ' ' || src_ufcs[p] == '\t' || src_ufcs[p] == '\r')) p++;
                /* Skip blank lines */
                if (p == line_end) {
                    ctnr_pos = (line_end < src_ufcs_len) ? line_end + 1 : line_end;
                    continue;
                }
                /* Skip block comments */
                if (p + 1 < src_ufcs_len && src_ufcs[p] == '/' && src_ufcs[p + 1] == '*') {
                    size_t end = p + 2;
                    while (end + 1 < src_ufcs_len && !(src_ufcs[end] == '*' && src_ufcs[end + 1] == '/')) end++;
                    ctnr_pos = (end + 1 < src_ufcs_len) ? end + 2 : src_ufcs_len;
                    if (ctnr_pos < src_ufcs_len && src_ufcs[ctnr_pos] == '\n') ctnr_pos++;
                    continue;
                }
                /* Skip line comments */
                if (p + 1 < line_end && src_ufcs[p] == '/' && src_ufcs[p + 1] == '/') {
                    ctnr_pos = (line_end < src_ufcs_len) ? line_end + 1 : line_end;
                    continue;
                }
                /* Skip preprocessor directives */
                if (src_ufcs[p] == '#') {
                    ctnr_pos = (line_end < src_ufcs_len) ? line_end + 1 : line_end;
                    continue;
                }
                /* Skip typedef, struct, union, enum blocks — but stop if the block
                   references a container type (__CC_MAP, __CC_VEC, Map_, CCVec_) since
                   the container macros must be emitted before that struct. */
                if ((p + 7 <= line_end && memcmp(src_ufcs + p, "typedef", 7) == 0 && !cc_is_ident_char(src_ufcs[p + 7])) ||
                    (p + 6 <= line_end && memcmp(src_ufcs + p, "struct", 6) == 0 && !cc_is_ident_char(src_ufcs[p + 6])) ||
                    (p + 5 <= line_end && memcmp(src_ufcs + p, "union", 5) == 0 && !cc_is_ident_char(src_ufcs[p + 5])) ||
                    (p + 4 <= line_end && memcmp(src_ufcs + p, "enum", 4) == 0 && !cc_is_ident_char(src_ufcs[p + 4]))) {
                    int is_typedef_block =
                        (p + 7 <= line_end && memcmp(src_ufcs + p, "typedef", 7) == 0 && !cc_is_ident_char(src_ufcs[p + 7]));
                    size_t block_start = p;
                    size_t q = p;
                    int brace_depth = 0;
                    size_t block_end = src_ufcs_len;
                    while (q < src_ufcs_len) {
                        char c = src_ufcs[q];
                        if (c == '{') brace_depth++;
                        else if (c == '}') { brace_depth--; if (brace_depth < 0) brace_depth = 0; }
                        else if (c == ';' && brace_depth == 0) {
                            q++;
                            if (q < src_ufcs_len && src_ufcs[q] == '\n') q++;
                            block_end = q;
                            break;
                        }
                        q++;
                    }
                    if (q >= src_ufcs_len) block_end = src_ufcs_len;
                    /* Check if this block references container types */
                    size_t block_sz = block_end - block_start;
                    int refs_container = 0;
                    int typedef_uses_only_predeclared_vec_char = 0;
                    for (size_t si = block_start; si + 7 < block_end && !refs_container; si++) {
                        if (memcmp(src_ufcs + si, "__CC_MAP", 8) == 0 ||
                            memcmp(src_ufcs + si, "__CC_VEC", 8) == 0) {
                            refs_container = 1;
                        } else if ((si + 4 < block_end && memcmp(src_ufcs + si, "Map_", 4) == 0) ||
                                   (si + 6 < block_end && memcmp(src_ufcs + si, "CCVec_", 6) == 0)) {
                            refs_container = 1;
                        }
                    }
                    if (is_typedef_block && refs_container) {
                        for (size_t si = block_start; si + 14 <= block_end; si++) {
                            if (memcmp(src_ufcs + si, "__CC_VEC(char)", 14) == 0) {
                                typedef_uses_only_predeclared_vec_char = 1;
                                break;
                            }
                        }
                    }
                    (void)block_sz;
                    if (refs_container && !is_typedef_block) {
                        ctnr_pos = line_start;
                        break;
                    }
                    if (refs_container && is_typedef_block && !typedef_uses_only_predeclared_vec_char) {
                        ctnr_pos = line_start;
                        break;
                    }
                    ctnr_pos = block_end;
                    continue;
                }
                break;
            }
            /* Splice container declarations into src_ufcs at ctnr_pos,
               then re-sync line numbers with a #line directive. */
            {
                char* new_src = NULL;
                size_t new_len = 0, new_cap = 0;

                int resync_line = 0;
                char resync_file[512] = {0};
                {
                    int last_line_num = 1;
                    char last_file[512] = {0};
                    int lines_since = 0;
                    const char* s = src_ufcs;
                    size_t si = 0;
                    while (si < ctnr_pos) {
                        if (si + 5 < ctnr_pos && s[si] == '#' && memcmp(s + si, "#line", 5) == 0) {
                            size_t li = si + 5;
                            while (li < ctnr_pos && (s[li] == ' ' || s[li] == '\t')) li++;
                            int num = 0;
                            while (li < ctnr_pos && s[li] >= '0' && s[li] <= '9') {
                                num = num * 10 + (s[li] - '0');
                                li++;
                            }
                            if (num > 0) {
                                last_line_num = num;
                                lines_since = 0;
                                while (li < ctnr_pos && (s[li] == ' ' || s[li] == '\t')) li++;
                                if (li < ctnr_pos && s[li] == '"') {
                                    li++;
                                    size_t fn = 0;
                                    while (li < ctnr_pos && s[li] != '"' && fn + 1 < sizeof(last_file)) {
                                        last_file[fn++] = s[li++];
                                    }
                                    last_file[fn] = '\0';
                                }
                            }
                        }
                        if (s[si] == '\n') lines_since++;
                        si++;
                    }
                    resync_line = last_line_num + lines_since;
                    if (last_file[0]) {
                        memcpy(resync_file, last_file, sizeof(resync_file));
                    }
                }

                cc__sb_append_local(&new_src, &new_len, &new_cap, src_ufcs, ctnr_pos);
                cc__sb_append_local(&new_src, &new_len, &new_cap, container_decl_buf, container_decl_len);
                cc__sb_append_cstr_local(&new_src, &new_len, &new_cap, "\n");
                if (resync_line > 0 && resync_file[0]) {
                    char line_dir[640];
                    snprintf(line_dir, sizeof(line_dir), "#line %d \"%s\"\n", resync_line, resync_file);
                    cc__sb_append_cstr_local(&new_src, &new_len, &new_cap, line_dir);
                }
                cc__sb_append_local(&new_src, &new_len, &new_cap,
                                    src_ufcs + ctnr_pos, src_ufcs_len - ctnr_pos);
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = new_src;
                src_ufcs_len = new_len;
            }
        }
        free(container_decl_buf);
        container_decl_buf = NULL;
        
        fwrite(src_ufcs, 1, src_ufcs_len, out);
        if (src_ufcs_len == 0 || src_ufcs[src_ufcs_len - 1] != '\n') fputc('\n', out);

        if (closure_defs && closure_defs_len > 0) {
            /* Run @defer lowering on closure definitions too (handles @defer inside spawn closures) */
            if (strstr(closure_defs, "@defer") != NULL || strstr(closure_defs, "cancel") != NULL) {
                char* closure_defs_lowered = NULL;
                size_t closure_defs_lowered_len = 0;
                if (cc__rewrite_defer_syntax(ctx, closure_defs, closure_defs_len,
                                             &closure_defs_lowered, &closure_defs_lowered_len) > 0) {
                    free(closure_defs);
                    closure_defs = closure_defs_lowered;
                    closure_defs_len = closure_defs_lowered_len;
                }
            }

            /* Same rationale as the main source path above: leave family UFCS for the
               final AST-aware rewrite instead of baking parser-safe generic names into
               emitted closure definitions. */
            {
                char* rewritten = cc_rewrite_channel_ufcs_concrete(closure_defs, closure_defs_len);
                if (rewritten) {
                    free(closure_defs);
                    closure_defs = rewritten;
                    closure_defs_len = strlen(rewritten);
                }
            }
            if (strstr(closure_defs, "send_task") != NULL || strstr(closure_defs, "cc_channel_send_task") != NULL) {
                size_t rewritten_len = 0;
                char* rewritten = cc__rewrite_chan_send_task_text(ctx, closure_defs, closure_defs_len, &rewritten_len);
                if (rewritten) {
                    free(closure_defs);
                    closure_defs = rewritten;
                    closure_defs_len = rewritten_len;
                }
            }
            {
                char* rewritten = NULL;
                CCTypeRegistry* saved_reg = cc_type_registry_get_global();
                CCTypeRegistry* temp_reg = cc_type_registry_new();
                if (temp_reg) {
                    cc_type_registry_set_global(temp_reg);
                    if (ctx && ctx->symbols) {
                        cc__collect_registered_ufcs_var_types(ctx->symbols, closure_defs, closure_defs_len);
                    }
                }
                rewritten = cc__rewrite_parser_placeholder_ufcs_lowers(closure_defs, closure_defs_len);
                if (temp_reg) {
                    cc_type_registry_set_global(saved_reg);
                    cc_type_registry_free(temp_reg);
                }
                if (rewritten) {
                    free(closure_defs);
                    closure_defs = rewritten;
                    closure_defs_len = strlen(rewritten);
                }
            }
            
            /* Emit closure declarations/definitions at end-of-file so all user
               types are already in scope and exact signatures are valid. */
            if (closure_protos && closure_protos_len > 0) {
                fputs("\n/* --- CC closure declarations --- */\n", out);
                fwrite(closure_protos, 1, closure_protos_len, out);
                fputs("/* --- end closure declarations --- */\n", out);
            }
            fputs("\n/* --- CC generated closures --- */\n", out);
            fwrite(closure_defs, 1, closure_defs_len, out);
        }
        free(closure_protos);
        free(closure_defs);
        if (src_ufcs != src_all) free(src_ufcs);
        free(src_all);
    } else {
        // Fallback stub when input is unavailable.
        fprintf(out,
                "#include \"std/prelude.h\"\n"
                "int main(void) {\n"
                "  CCArena a = cc_heap_arena(kilobytes(1));\n"
                "  CCString s = CCString_new(&a);\n"
                "  CCString_push(&s, cc_slice_from_buffer(\"Hello, \", sizeof(\"Hello, \") - 1));\n"
                "  CCString_push(&s, cc_slice_from_buffer(\"Concurrent-C via UFCS!\\n\", sizeof(\"Concurrent-C via UFCS!\\n\") - 1));\n"
                "  cc_std_out_write(CCString_as_slice(&s));\n"
                "  return 0;\n"
                "}\n");
    }

    fclose(out);
    cc__maybe_format_lowered_output(output_path);
    return 0;
}

