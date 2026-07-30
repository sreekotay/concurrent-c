#include "visitor.h"
#include "visitor/pass_common.h"
#include "visit_codegen.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>
#include <dlfcn.h>
#include <time.h>
#include <limits.h>

/* Env-gated reparse instrumentation (CC_PROFILE_REPARSE=1).  Counts full
 * source->AST reparses and accumulates wall time so structural pass-count
 * reductions can be measured precisely, not just by noisy end-to-end timing. */
static int g_cc_reparse_count = 0;
static double g_cc_reparse_ms = 0.0;
static double cc__now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

#include "visitor/ufcs.h"
#include "visitor/pass_strip_markers.h"
#include "visitor/pass_await_normalize.h"
#include "visitor/pass_ufcs.h"
#include "preprocess/grammar_engine.h"
#include "visitor/pass_closure_calls.h"
#include "visitor/pass_autoblock.h"
#include "visitor/pass_slice_literal_coerce.h"
#include "visitor/pass_as_arg_coerce.h"
#include "visitor/pass_closure_literal_ast.h"
#include "visitor/pass_defer_syntax.h"
#include "visitor/pass_err_syntax.h"
#include "visitor/pass_result_unwrap.h"
#include "visitor/pass_unwrap_destroy.h"
#include "visitor/pass_channel_syntax.h"
#include "visitor/pass_create.h"
#include "visitor/pass_type_syntax.h"
#include "visitor/edit_buffer.h"
#include "visitor/visitor_fileutil.h"
#include "visitor/text_span.h"
#include "comptime/hook_compile.h"
#include "header/lower_header.h"
#include "parser/tcc_bridge.h"
#include "preprocess/cc_l2_rewriter.h"
#include "preprocess/preprocess.h"
#include "preprocess/cpp_expand.h"
#include "preprocess/type_registry.h"
#include "preprocess/emit_plan.h"
#include "preprocess/comptime_prepare.h"
#include "result_spec.h"
#include "util/path.h"
#include "util/text.h"
#include "util/text_scan.h"
#include "../diag/diag.h"
#include "../diag/source_map.h"

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

/* Emit `static const cc_type_info __cc_ti_<mangled>` + the
 * auto-registering `cc__ti_reg_<mangled>` constructor for one
 * codegen-emitted container instantiation (CCVec::[T], Map<K,V>).
 * The shape is identical for both, so this helper exists to
 * keep the two call sites in sync.
 *
 * Output layout is deliberately spacious — the goal is for the
 * lowered C to read like something a human wrote: a blank line
 * separates the struct from its registrar, and another blank
 * line above the whole block separates it from the preceding
 * `CC_VEC_DECL_ARENA` / `CC_MAP_DECL_ARENA` line.  A leading
 * source-attribution comment names the originating CC syntax.
 *
 * Returns 0 on success, -1 on snprintf truncation (which means
 * the mangled name is implausibly long — log it loudly so the
 * caller can investigate; the emission is silently dropped). */
static int cc__emit_container_cc_type_info(char** buf, size_t* len, size_t* cap,
                                           const char* mangled) {
    /* 4 KiB is plenty for any plausible mangled name (mangled
     * names are bounded by the user's identifier choices —
     * typical ≤ 60 chars, with 9 substitutions of length M each
     * the worst case is ≈ 9M + ~280 fixed bytes).  If this ever
     * fires in practice, switch to a malloc'd buffer here.   */
    char line[4096];
    int written = snprintf(line, sizeof(line),
        "\n"
        "/* cc_type_info for %s (emitted by visit_codegen) */\n"
        "static const cc_type_info __cc_ti_%s = {\n"
        "    .name      = \"%s\",\n"
        "    .mangled   = \"%s\",\n"
        "    .id        = 0,\n"
        "    .size      = (uint32_t)sizeof(%s),\n"
        "    .align     = (uint32_t)_Alignof(%s),\n"
        "    .kind      = (uint16_t)CC_TK_GENERIC_INST,\n"
        "    .nfields   = 0,\n"
        /* Containers are erasable (carryable through cc_dyn_vec)
         * but NOT POD/trivial-copy/trivial-drop — bitwise-copying
         * a vec header aliases the arena-owned data pointer. */
        "    .flags     = (uint16_t)CC_TF_ERASABLE,\n"
        "    ._reserved = 0,\n"
        "    .fields    = NULL,\n"
        "    .copy_fn   = NULL,\n"
        "    .drop_fn   = NULL,\n"
        "};\n"
        "\n"
        "__attribute__((constructor))\n"
        "static void cc__ti_reg_%s(void) {\n"
        "    cc_type_info_register(&__cc_ti_%s);\n"
        "}\n"
        "\n",
        mangled,
        mangled, mangled, mangled,
        mangled, mangled,
        mangled, mangled);
    if (written < 0 || (size_t)written >= sizeof(line)) {
        fprintf(stderr,
            "cc: warning: cc_type_info emission for '%s' truncated "
            "(written=%d, cap=%zu); registration skipped\n",
            mangled ? mangled : "<null>", written, sizeof(line));
        return -1;
    }
    cc__sb_append_cstr_local(buf, len, cap, line);
    return 0;
}

/* Format one Vec/Map/ArrayMap monomorph + its cc_type_info into *buf. */
static void cc__format_vec_container_decl(char** buf, size_t* len, size_t* cap,
                                          const CCTypeInstantiation* inst) {
    char line[512];
    char slice_name[256];
    const char* mangled_elem;
    if (!inst || !inst->type1 || !inst->mangled_name) return;
    mangled_elem = inst->mangled_name + 6; /* Skip "CCVec_" */
    if (strcmp(mangled_elem, "char") == 0) return;
    /* Elements with a declared slice instance get the typed `as_slice`
     * (returns CCSlice_<elem>); others keep the erased view. */
    if (cc_slice_spec_instance_for_elem(inst->type1, slice_name,
                                        sizeof(slice_name)) == 0)
        snprintf(line, sizeof(line), "CC_VEC_DECL_ARENA_TSLICE(%s, %s, %s)\n",
                 inst->type1, inst->mangled_name, slice_name);
    else
        snprintf(line, sizeof(line), "CC_VEC_DECL_ARENA(%s, %s)\n",
                 inst->type1, inst->mangled_name);
    cc__sb_append_cstr_local(buf, len, cap, line);
    cc__emit_container_cc_type_info(buf, len, cap, inst->mangled_name);
}

/* Seed Map/ArrayMap UFCS method tables from the type graph.  The emitted
 * `CC_*_MAP_DECL_UFCS` markers live in host-C output and are not present in
 * the comptime/canonical buffers that `cc_symbols_collect_type_registrations`
 * scans — so nested-only methods (e.g. live_bytes) would otherwise never
 * enter the table. */
static void cc__seed_map_ufcs_from_type_graph(CCSymbolTable* symbols) {
    CCTypeGraph* graph;
    CCTypeRegistry* reg;
    size_t n;
    size_t i;
    if (!symbols) return;
    graph = cc_type_graph_get_global();
    if (!graph) return;
    n = cc_type_graph_map_count(graph);
    for (i = 0; i < n; i++) {
        const CCTypeInstantiation* inst = cc_type_graph_get_map(graph, i);
        if (!inst || !inst->mangled_name || !inst->mangled_name[0]) continue;
        (void)cc_symbols_register_map_ufcs(symbols, inst->mangled_name);
    }
    /* Typedef aliases (NestedUfcsMap / RedisDbMap → ArrayMap_…) must key the
     * same callees; AST receivers often report the typedef name. */
    reg = cc_type_graph_active_registry(graph);
    if (!reg) return;
    n = cc_type_registry_alias_count(reg);
    for (i = 0; i < n; i++) {
        const char* alias = cc_type_registry_alias_name_at(reg, i);
        const char* target = cc_type_registry_alias_type_at(reg, i);
        char base[256];
        size_t blen;
        if (!alias || !target) continue;
        while (*target == ' ' || *target == '\t') target++;
        blen = strlen(target);
        while (blen > 0 && (target[blen - 1] == '*' || target[blen - 1] == ' ' ||
                            target[blen - 1] == '\t'))
            blen--;
        if (blen == 0 || blen >= sizeof(base)) continue;
        memcpy(base, target, blen);
        base[blen] = '\0';
        if (strncmp(base, "ArrayMap_", 9) != 0 && strncmp(base, "Map_", 4) != 0) continue;
        (void)cc_symbols_register_map_ufcs_alias(symbols, alias, base);
    }
}

static void cc__format_map_container_decl(char** buf, size_t* len, size_t* cap,
                                          const CCTypeInstantiation* inst) {
    char hash_buf[160];
    char eq_buf[160];
    const char* hash_fn;
    const char* eq_fn;
    char line[512];
    if (!inst || !inst->type1 || !inst->type2 || !inst->mangled_name) return;
    {
        int tu_static = -1;
        (void)cc_map_key_hasheq_ex(inst->type1, hash_buf, sizeof(hash_buf),
                                   eq_buf, sizeof(eq_buf), &tu_static);
        if (tu_static >= 0) {
            snprintf(line, sizeof(line), "%ssize_t %s(%s);\n%sint %s(%s, %s);\n",
                     tu_static ? "static " : "", hash_buf, inst->type1,
                     tu_static ? "static " : "", eq_buf, inst->type1,
                     inst->type1);
            cc__sb_append_cstr_local(buf, len, cap, line);
        }
    }
    hash_fn = hash_buf;
    eq_fn = eq_buf;
    if (strncmp(inst->mangled_name, "ArrayMap_", 9) == 0) {
        snprintf(line, sizeof(line), "CC_ARRAY_MAP_DECL(%s, %s, %s, %s, %s)\n",
                 inst->type1, inst->type2, inst->mangled_name, hash_fn, eq_fn);
        cc__sb_append_cstr_local(buf, len, cap, line);
        /* Seed Map-style UFCS method table (incl. live_bytes) for symbols. */
        snprintf(line, sizeof(line), "CC_ARRAY_MAP_DECL_UFCS(%s);\n", inst->mangled_name);
        cc__sb_append_cstr_local(buf, len, cap, line);
    } else {
        snprintf(line, sizeof(line), "CC_MAP_DECL_ARENA(%s, %s, %s, %s, %s)\n",
                 inst->type1, inst->type2, inst->mangled_name, hash_fn, eq_fn);
        cc__sb_append_cstr_local(buf, len, cap, line);
    }
    cc__emit_container_cc_type_info(buf, len, cap, inst->mangled_name);
}

typedef struct CCCtnrInsert {
    size_t pos;
    char* text;
    size_t text_len;
} CCCtnrInsert;

static void cc__ctnr_insert_append(CCCtnrInsert* inserts, size_t* n_ins, size_t max_ins,
                                   size_t pos, const char* text, size_t text_len) {
    size_t i;
    if (!inserts || !n_ins || !text || text_len == 0 || *n_ins >= max_ins) return;
    for (i = 0; i < *n_ins; i++) {
        if (inserts[i].pos == pos) {
            char* merged = (char*)realloc(inserts[i].text, inserts[i].text_len + text_len + 1);
            if (!merged) return;
            memcpy(merged + inserts[i].text_len, text, text_len);
            inserts[i].text_len += text_len;
            merged[inserts[i].text_len] = '\0';
            inserts[i].text = merged;
            return;
        }
    }
    inserts[*n_ins].pos = pos;
    inserts[*n_ins].text = (char*)malloc(text_len + 1);
    if (!inserts[*n_ins].text) return;
    memcpy(inserts[*n_ins].text, text, text_len);
    inserts[*n_ins].text[text_len] = '\0';
    inserts[*n_ins].text_len = text_len;
    (*n_ins)++;
}

static void cc__ctnr_insert_resync(const char* src, size_t pos,
                                   int* out_line, char* out_file, size_t out_file_sz) {
    int last_line_num = 1;
    char last_file[512] = {0};
    int lines_since = 0;
    size_t si = 0;
    if (out_line) *out_line = 1;
    if (out_file && out_file_sz) out_file[0] = '\0';
    if (!src) return;
    while (si < pos) {
        if (si + 5 < pos && src[si] == '#' && memcmp(src + si, "#line", 5) == 0) {
            size_t li = si + 5;
            int num = 0;
            while (li < pos && (src[li] == ' ' || src[li] == '\t')) li++;
            while (li < pos && src[li] >= '0' && src[li] <= '9') {
                num = num * 10 + (src[li] - '0');
                li++;
            }
            if (num > 0) {
                last_line_num = num;
                lines_since = 0;
                while (li < pos && (src[li] == ' ' || src[li] == '\t')) li++;
                if (li < pos && src[li] == '"') {
                    size_t fn = 0;
                    li++;
                    while (li < pos && src[li] != '"' && fn + 1 < sizeof(last_file)) {
                        last_file[fn++] = src[li++];
                    }
                    last_file[fn] = '\0';
                }
            }
        }
        if (src[si] == '\n') lines_since++;
        si++;
    }
    if (out_line) *out_line = last_line_num + lines_since;
    if (out_file && out_file_sz && last_file[0]) {
        snprintf(out_file, out_file_sz, "%s", last_file);
    }
}

static void cc__collect_ufcs_field_and_var_types(const char* src, size_t n);
static int cc__is_parser_placeholder_type_codegen(const char* type_name);
typedef enum CCPhase3Stage CCPhase3Stage;
static int cc__apply_batched_phase3_passes(const CCASTRoot* root,
                                           const CCVisitorCtx* ctx,
                                           char** src_io,
                                           size_t* len_io,
                                           const char* base_src,
                                           int* out_changed,
                                           CCPhase3Stage stage);


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

/* Result specs are collected during canonicalize; mirror them for host-.c emission. */
static void cc__mirror_canonical_result_specs(void) {
    CCResultSpecTable* canon = cc_result_spec_table_get_global();
    cc_result_spec_table_reset(&cc__cg_result_specs);
    if (!canon) {
        cc_result_spec_table_set_global(&cc__cg_result_specs);
        return;
    }
    for (size_t i = 0; i < canon->count; i++) {
        const CCResultSpec* s = cc_result_spec_table_get(canon, i);
        if (!s) continue;
        (void)cc_result_spec_table_add(&cc__cg_result_specs,
                                       s->ok_type, strlen(s->ok_type),
                                       s->err_type, strlen(s->err_type),
                                       s->mangled_ok, s->mangled_err);
    }
    cc_result_spec_table_set_global(&cc__cg_result_specs);
}

static char* cc__write_failed_reparse_dump(const char* stage,
                                           const char* src,
                                           size_t src_len,
                                           const char* input_path) {
    char tmpl[512];
    const char* tmpdir = getenv("TMPDIR");
    char rel[1024];
    char header[2048];
    const char* shown = NULL;
    int fd = -1;
    int header_len = 0;
    size_t off = 0;
    if (!src || src_len == 0) return NULL;
    shown = cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel));
    if (snprintf(tmpl, sizeof(tmpl), "%s/cc_reparse_fail_XXXXXX.c",
                 tmpdir && tmpdir[0] ? tmpdir : "/tmp") >= (int)sizeof(tmpl))
        return NULL;
    fd = mkstemps(tmpl, 2);
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
    fprintf(stderr, "cc: internal error: the compiler's intermediate form failed to re-parse during %s for %s\n",
            (stage && stage[0]) ? stage : "unknown stage",
            shown ? shown : "<input>");
    fprintf(stderr, "cc: this is a compiler bug — source that reaches this stage has already parsed once; please report it\n");
    fprintf(stderr, "cc: the parser diagnostic above maps to user coordinates via #line where possible, but the\n");
    fprintf(stderr, "cc: failing construct may be compiler-GENERATED text near that line rather than your code\n");
    if (stage && strcmp(stage, "final-UFCS input") == 0 && transformed_src &&
        (strstr(transformed_src, "__typeof__(cc_channel_send(") ||
         strstr(transformed_src, "__typeof__(cc_channel_recv("))) {
        fprintf(stderr,
                "cc: note: this looks like result-unwrapping a direct channel send/recv after async lowering\n");
        fprintf(stderr,
                "cc: note: a common cause is a channel operation inside an @async @noblock function; "
                "remove the function-level @noblock or mark that call site @blocking\n");
    }
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
    CCInertScan scan;
    if (!src) return NULL;
    out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n);
    out[n] = '\0';
    cc_inert_scan_init(&scan, NULL);
    /* Migrated to CCInertScan.  Trick: to decide whether a consumed
     * inert run was a comment (blank it) vs string/char/pp (leave it
     * verbatim), we snapshot `in_line_comment`/`in_block_comment`
     * BEFORE the step and check them AFTER.  A single step touches
     * at most one inert-region kind (e.g. `* /` closing a block
     * comment, then `"` opening a string, requires two separate
     * steps), so the prev||curr disjunction is unambiguous.
     *
     * Behavior tweak: pp directives (`#define`/`#line`/etc.) are now
     * skipped as inert instead of being treated as code.  Since the
     * fall-through behavior on code bytes here is "do nothing",
     * leaving pp bytes verbatim is the same outcome — no semantic
     * change. */
    int keep_marker = 0; /* current block comment is a /-*CC_CLO:N*-/ marker */
    for (size_t i = 0; i < n; ) {
        size_t before = i;
        int prev_lc = scan.in_line_comment;
        int prev_bc = scan.in_block_comment;
        if (cc_inert_scan_step(&scan, src, n, &i)) {
            int touched_comment = prev_lc || prev_bc ||
                                  scan.in_line_comment || scan.in_block_comment;
            /* Closure-ID markers survive into the reparse buffer: TCC's
             * lexer drops them like any comment, but preprocess_skip sees
             * them inside #if-skipped regions and records their IDs so the
             * closure pass can prune markers whose closures conditional
             * compilation discarded. */
            if (!prev_bc && scan.in_block_comment &&
                before + 9 <= n && memcmp(src + before, "/*CC_CLO:", 9) == 0)
                keep_marker = 1;
            if (touched_comment && !keep_marker) {
                for (size_t k = before; k < i; k++) {
                    char ch = src[k];
                    if (ch != '\n' && ch != '\r' && ch != '\t') out[k] = ' ';
                }
            }
            if (!scan.in_block_comment) keep_marker = 0;
            /* String/char/pp regions stay verbatim (already memcpy'd). */
            continue;
        }
        i++;
    }
    return out;
}

static int cc__cg_is_lifetime_marker_at(const char* src, size_t n, size_t p, size_t* marker_len) {
    if (marker_len) *marker_len = 0;
    if (!src || p >= n || src[p] != '@') return 0;
    if (p + 8 <= n && memcmp(src + p, "@destroy", 8) == 0 &&
        (p + 8 == n || !cc_is_ident_char(src[p + 8]))) {
        if (marker_len) *marker_len = 8;
        return 1;
    }
    if (p + 6 <= n && memcmp(src + p, "@defer", 6) == 0 &&
        (p + 6 == n || !cc_is_ident_char(src[p + 6]))) {
        if (marker_len) *marker_len = 6;
        return 1;
    }
    return 0;
}

static int cc__cg_mem_has(const char* hay, size_t hay_len, const char* needle) {
    size_t needle_len = needle ? strlen(needle) : 0;
    if (!hay || !needle || needle_len == 0 || hay_len < needle_len) return 0;
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) return 1;
    }
    return 0;
}

static int cc__cg_consume_postfix_cc_suffix_chain(const char* src, size_t n,
                                                  size_t first_suffix,
                                                  size_t* out_end,
                                                  int* out_has_lifetime) {
    size_t p = first_suffix;
    int consumed = 0;
    int has_lifetime = 0;
    if (out_end) *out_end = first_suffix;
    if (out_has_lifetime) *out_has_lifetime = 0;
    while (p < n) {
        size_t s = cc_skip_ws_and_comments(src, n, p);
        size_t marker_len = 0;
        if (s + 2 <= n && src[s] == '!' && src[s + 1] == '>') {
            int had_binder = 0;
            p = cc_skip_ws_and_comments(src, n, s + 2);
            if (p < n && src[p] == '(') {
                size_t rpar = 0;
                if (!cc_find_matching_paren(src, n, p, &rpar)) return 0;
                p = cc_skip_ws_and_comments(src, n, rpar + 1);
                had_binder = 1;
            }
            if (p < n && cc_is_ident_char(src[p])) {
                int is_control_stmt =
                    (p + 6 <= n && memcmp(src + p, "return", 6) == 0 &&
                     (p + 6 == n || !cc_is_ident_char(src[p + 6]))) ||
                    (p + 5 <= n && memcmp(src + p, "break", 5) == 0 &&
                     (p + 5 == n || !cc_is_ident_char(src[p + 5]))) ||
                    (p + 8 <= n && memcmp(src + p, "continue", 8) == 0 &&
                     (p + 8 == n || !cc_is_ident_char(src[p + 8]))) ||
                    (p + 4 <= n && memcmp(src + p, "goto", 4) == 0 &&
                     (p + 4 == n || !cc_is_ident_char(src[p + 4])));
                if (!is_control_stmt && had_binder) {
                    return 0;
                }
                if (is_control_stmt || !had_binder) {
                    while (p < n && src[p] != ';') p++;
                    consumed = 1;
                    continue;
                }
            } else if (had_binder && p < n && src[p] != '{') {
                return 0;
            }
            if (p < n && src[p] == '{') {
                size_t rbrace = 0;
                if (!cc_find_matching_brace(src, n, p, &rbrace)) return 0;
                p = rbrace + 1;
            }
            consumed = 1;
            continue;
        }
        if (s + 2 <= n && src[s] == '?' && src[s + 1] == '>') {
            p = cc_skip_ws_and_comments(src, n, s + 2);
            if (p < n && src[p] == '(') {
                size_t rpar = 0;
                if (!cc_find_matching_paren(src, n, p, &rpar)) return 0;
                p = cc_skip_ws_and_comments(src, n, rpar + 1);
            }
            while (p < n && src[p] != ';') {
                size_t nested_marker_len = 0;
                if (cc__cg_is_lifetime_marker_at(src, n, p, &nested_marker_len)) break;
                p++;
            }
            consumed = 1;
            continue;
        }
        if (cc__cg_is_lifetime_marker_at(src, n, s, &marker_len)) {
            p = cc_skip_ws_and_comments(src, n, s + marker_len);
            if (p < n && src[p] == '{') {
                size_t rbrace = 0;
                if (!cc_find_matching_brace(src, n, p, &rbrace)) return 0;
                p = rbrace + 1;
            }
            consumed = 1;
            has_lifetime = 1;
            continue;
        }
        break;
    }
    if (!consumed) return 0;
    if (out_end) *out_end = p;
    if (out_has_lifetime) *out_has_lifetime = has_lifetime;
    return 1;
}

/* Reparse-input sanitizers blank regions newline-preservingly, then stamp a
   tiny placeholder so the result still parses. The stamp must never land on a
   '\n'/'\r': overwriting one shrinks the reparse input by a line, and every
   recorder line below runs low (this was the UFCS dead-band bug). Returns the
   first index in [lo, hi - need] where `need` consecutive bytes are
   newline-free, or (size_t)-1 when no such window exists. */
static size_t cc__cg_stamp_pos(const char* buf, size_t lo, size_t hi, size_t need) {
    if (hi < need) return (size_t)-1;
    for (size_t j = lo; j + need <= hi; j++) {
        int ok = 1;
        for (size_t k = j; k < j + need; k++) {
            if (buf[k] == '\n' || buf[k] == '\r') { ok = 0; break; }
        }
        if (ok) return j;
    }
    return (size_t)-1;
}

static char* cc__sanitize_statement_unwraps_for_reparse(const char* src, size_t n) {
    char* out = NULL;
    int changed = 0;
    CCInertScan scan;
    if (!src || n == 0) return NULL;
    cc_inert_scan_init(&scan, NULL);
    size_t i = 0;
    while (i + 1 < n) {
        if (cc_inert_scan_step(&scan, src, n, &i)) continue;
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (!(c == '!' && c2 == '>')) { i++; continue; }

        size_t suffix_end = 0;
        int has_lifetime = 0;
        if (!cc__cg_consume_postfix_cc_suffix_chain(src, n, i, &suffix_end, &has_lifetime)) { i++; continue; }

        size_t stmt_a = i;
        while (stmt_a > 0 && src[stmt_a - 1] != ';' && src[stmt_a - 1] != '{' &&
               src[stmt_a - 1] != '}') {
            stmt_a--;
        }
        size_t stmt_kw = stmt_a;
        while (stmt_kw < i && isspace((unsigned char)src[stmt_kw])) stmt_kw++;
        int is_return_stmt =
            stmt_kw + 6 <= i && memcmp(src + stmt_kw, "return", 6) == 0 &&
            (stmt_kw + 6 == i || !cc_is_ident_char(src[stmt_kw + 6]));
        int has_assign = 0;
        int looks_like_decl_init = 0;
        size_t eq = i;
        for (size_t k = stmt_a; k < i; k++) {
            if (src[k] == '=' && (k == 0 || src[k - 1] != '!') &&
                (k + 1 >= i || src[k + 1] != '=')) {
                has_assign = 1;
                eq = k;
                break;
            }
        }
        if (has_assign) {
            int ident_count = 0;
            for (size_t k = stmt_kw; k < eq;) {
                if (cc_is_ident_start(src[k])) {
                    size_t e = k + 1;
                    while (e < eq && cc_is_ident_char(src[e])) e++;
                    ident_count++;
                    k = e;
                    continue;
                }
                k++;
            }
            looks_like_decl_init = ident_count >= 2;
        }
        if (has_assign && !has_lifetime && !is_return_stmt) {
            if (!looks_like_decl_init) {
                i = (suffix_end > 0) ? suffix_end : i + 1;
                continue;
            }
        }

        if (!out) {
            out = (char*)malloc(n + 1);
            if (!out) return NULL;
            memcpy(out, src, n);
            out[n] = '\0';
        }

        if (is_return_stmt) {
            size_t r = stmt_kw + 6;
            for (size_t k = r; k < suffix_end; k++) {
                if (out[k] != '\n' && out[k] != '\r') out[k] = ' ';
            }
            if (r + 2 < suffix_end) {
                size_t j = cc__cg_stamp_pos(out, r + 1, suffix_end - 1, 1);
                if (j != (size_t)-1) out[j] = '0';
            }
        } else if (has_assign && (has_lifetime || looks_like_decl_init)) {
            for (size_t k = eq + 1; k < suffix_end; k++) {
                if (out[k] != '\n' && out[k] != '\r') out[k] = ' ';
            }
            if (looks_like_decl_init) {
                if (eq + 4 < suffix_end) {
                    size_t j = cc__cg_stamp_pos(out, eq + 2, suffix_end, 3);
                    if (j != (size_t)-1) {
                        out[j] = '{';
                        out[j + 1] = '0';
                        out[j + 2] = '}';
                    }
                }
            } else if (eq + 2 < suffix_end) {
                size_t j = cc__cg_stamp_pos(out, eq + 2, suffix_end, 1);
                if (j != (size_t)-1) out[j] = '0';
            }
        } else if (has_assign) {
            for (size_t k = i; k < suffix_end; k++) {
                if (out[k] != '\n' && out[k] != '\r') out[k] = ' ';
            }
        } else {
            for (size_t k = i; k < suffix_end; k++) {
                if (out[k] != '\n' && out[k] != '\r') out[k] = ' ';
            }
            out[i] = ';';
        }
        changed = 1;
        i = (suffix_end > 0) ? suffix_end : i + 1;
    }
    if (!changed) {
        free(out);
        return NULL;
    }
    return out;
}

static char* cc__sanitize_lifetime_markers_for_reparse(const char* src, size_t n) {
    char* out = NULL;
    int changed = 0;
    CCInertScan scan;
    if (!src || n == 0) return NULL;
    cc_inert_scan_init(&scan, NULL);
    for (size_t i = 0; i + 6 <= n;) {
        size_t marker_len = 0;
        /* Inert-region aware: "@destroy" inside a string literal or
         * comment must not blank the block that follows it. */
        if (cc_inert_scan_step(&scan, src, n, &i)) continue;
        if (memcmp(src + i, "@defer", 6) == 0) {
            marker_len = 6;
        } else if (i + 8 <= n && memcmp(src + i, "@destroy", 8) == 0) {
            marker_len = 8;
        } else {
            i++;
            continue;
        }
        if (i > 0 && cc_is_ident_char(src[i - 1])) { i++; continue; }
        if (i + marker_len < n && cc_is_ident_char(src[i + marker_len])) { i++; continue; }
        if (!out) {
            out = (char*)malloc(n + 1);
            if (!out) return NULL;
            memcpy(out, src, n);
            out[n] = '\0';
        }
        size_t erase_end = i + marker_len;
        size_t p = cc_skip_ws_and_comments(src, n, erase_end);
        if (p < n && src[p] == '(') {
            size_t rpar = 0;
            if (cc_find_matching_paren(src, n, p, &rpar)) {
                erase_end = rpar + 1;
                p = cc_skip_ws_and_comments(src, n, erase_end);
            }
        }
        if (p < n && src[p] == '{') {
            size_t rbrace = 0;
            if (cc_find_matching_brace(src, n, p, &rbrace)) {
                erase_end = rbrace + 1;
            }
        }
        for (size_t k = i; k < erase_end; k++) {
            if (out[k] != '\n' && out[k] != '\r') out[k] = ' ';
        }
        changed = 1;
        /* Advance past the marker keyword only; the erased body is
         * rescanned in src so the inert-scan state stays consistent. */
        i += marker_len;
    }
    if (!changed) {
        free(out);
        return NULL;
    }
    return out;
}

static char* cc__sanitize_generated_unwrap_handlers_for_reparse(const char* src, size_t n) {
    char* out = NULL;
    int changed = 0;
    if (!src || n == 0) return NULL;
    for (size_t i = 0; i + 18 < n; i++) {
        if (!(src[i] == 'i' && src[i + 1] == 'f' &&
              (i == 0 || !cc_is_ident_char(src[i - 1])) &&
              (i + 2 == n || !cc_is_ident_char(src[i + 2])))) {
            continue;
        }
        size_t p = cc_skip_ws_and_comments(src, n, i + 2);
        if (p >= n || src[p] != '(') continue;
        size_t cond_r = 0;
        if (!cc_find_matching_paren(src, n, p, &cond_r)) continue;
        size_t cond_len = cond_r > p ? cond_r - p - 1 : 0;
        if (cond_len == 0 ||
            !cc__cg_mem_has(src + p + 1, cond_len, "__cc_pu_") ||
            !(cc__cg_mem_has(src + p + 1, cond_len, "__cc_uw_is_err") ||
              cc__cg_mem_has(src + p + 1, cond_len, "_is_err"))) {
            continue;
        }
        size_t b = cc_skip_ws_and_comments(src, n, cond_r + 1);
        if (b >= n || src[b] != '{') continue;
        size_t rbrace = 0;
        if (!cc_find_matching_brace(src, n, b, &rbrace)) continue;
        if (!out) {
            out = (char*)malloc(n + 1);
            if (!out) return NULL;
            memcpy(out, src, n);
            out[n] = '\0';
        }
        for (size_t k = b + 1; k < rbrace; k++) {
            if (out[k] != '\n' && out[k] != '\r') out[k] = ' ';
        }
        /* Multi-line handlers open with "{\n"; a fixed stamp at b+1 would
           overwrite that newline, shrinking the reparse input by one line per
           handler and skewing every recorder line below it (the UFCS dead-band
           bug). Stamp in the first newline-free window instead; a blanked
           "{ }" parses fine if no window exists. */
        if (b + 9 < rbrace) {
            size_t j = cc__cg_stamp_pos(out, b + 1, rbrace, 9);
            if (j != (size_t)-1) memcpy(out + j, " (void)0;", 9);
        }
        changed = 1;
        i = rbrace;
    }
    if (!changed) {
        free(out);
        return NULL;
    }
    return out;
}

/* Every reparse-input sanitizer must be line-neutral: recorder lines from
   the reparse AST are matched back against the edit buffer by line number,
   so a single lost newline silently skews every node below it. Enforce the
   invariant at the call sites — a violation is a compiler bug and must fail
   the build, not mis-lower code. Returns `sanitized` on success; aborts the
   sanitize (returns NULL, caller keeps the original) never — parity failure
   is fatal. */
static char* cc__check_sanitize_line_parity(const char* orig, size_t orig_len,
                                            char* sanitized, const char* what) {
    if (!sanitized) return NULL;
    size_t a = 0, b = 0;
    for (size_t i = 0; i < orig_len; i++) a += (orig[i] == '\n');
    for (const char* p = sanitized; *p; p++) b += (*p == '\n');
    if (a != b) {
        fprintf(stderr,
                "cc: internal error: reparse sanitizer '%s' changed line count "
                "(%zu -> %zu newlines); recorder coordinates below the first "
                "divergence would silently skew\n",
                what, a, b);
        free(sanitized);
        exit(1);
    }
    return sanitized;
}

static int cc__cg_chan_recv_expr_char(char c) {
    return cc_is_ident_char(c) || c == '.' || c == '-' || c == '>' || c == ']' || c == ')';
}

static int cc__cg_late_channel_ufcs_callee(const char* src,
                                           size_t recv_a,
                                           size_t recv_b,
                                           const char* method,
                                           const char** out_fn) {
    char recv_expr[256];
    int recv_is_ptr = 0;
    CCUfcsChannelKind kind = CC_UFCS_CHANNEL_KIND_NONE;
    int recv_by_value = 0;
    const char* type_name = NULL;
    const char* fn = NULL;
    CCTypeRegistry* reg = cc_type_graph_active_registry(cc_type_graph_get_global());
    if (out_fn) *out_fn = NULL;
    if (!src || !method || !out_fn || !reg || recv_b <= recv_a) return 0;
    while (recv_a < recv_b && isspace((unsigned char)src[recv_a])) recv_a++;
    while (recv_b > recv_a && isspace((unsigned char)src[recv_b - 1])) recv_b--;
    if (recv_b <= recv_a || recv_b - recv_a >= sizeof(recv_expr)) return 0;
    memcpy(recv_expr, src + recv_a, recv_b - recv_a);
    recv_expr[recv_b - recv_a] = '\0';

    type_name = cc_type_registry_resolve_receiver_expr_at(
        reg, recv_expr, src, recv_a, &recv_is_ptr);
    if (!type_name || !type_name[0]) return 0;

    fn = cc_ufcs_channel_callee(type_name, method, 0, &kind, &recv_by_value);
    if (!fn || !fn[0]) return 0;

    /* Raw CCChan* send/recv need an element-size argument, which this late
     * textual cleanup cannot infer safely. Leave those for the AST UFCS path
     * (or for the C compiler to reject) instead of guessing. */
    if (kind == CC_UFCS_CHANNEL_KIND_RAW &&
        (strcmp(method, "send") == 0 || strcmp(method, "recv") == 0)) {
        return 0;
    }
    (void)recv_is_ptr;
    (void)recv_by_value;
    *out_fn = fn;
    return 1;
}

static char* cc__rewrite_channel_ufcs_text_late(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t ol = 0, oc = 0, last = 0;
    for (size_t i = 0; i + 6 < n; i++) {
        const char* fn = NULL;
        const char* method = NULL;
        size_t method_len = 0;
        if (memcmp(src + i, ".send(", 6) == 0) {
            method = "send";
            method_len = 5;
        } else if (memcmp(src + i, ".recv(", 6) == 0) {
            method = "recv";
            method_len = 5;
        } else if (i + 7 < n && memcmp(src + i, ".close(", 7) == 0) {
            method = "close";
            method_len = 6;
        } else if (i + 6 < n && memcmp(src + i, ".free(", 6) == 0) {
            method = "free";
            method_len = 5;
        } else {
            continue;
        }
        size_t recv_a = i;
        while (recv_a > 0 && cc__cg_chan_recv_expr_char(src[recv_a - 1])) recv_a--;
        if (recv_a == i) continue;
        size_t open = i + method_len;
        size_t close = 0;
        if (open >= n || src[open] != '(' || !cc_find_matching_paren(src, n, open, &close)) continue;
        if (!cc__cg_late_channel_ufcs_callee(src, recv_a, i, method, &fn)) continue;
        cc_sb_append(&out, &ol, &oc, src + last, recv_a - last);
        cc_sb_append_cstr(&out, &ol, &oc, fn);
        cc_sb_append_cstr(&out, &ol, &oc, "(");
        cc_sb_append(&out, &ol, &oc, src + recv_a, i - recv_a);
        if (close > open + 1) {
            cc_sb_append_cstr(&out, &ol, &oc, ", ");
            cc_sb_append(&out, &ol, &oc, src + open + 1, close - open - 1);
        }
        cc_sb_append_cstr(&out, &ol, &oc, ")");
        last = close + 1;
        i = close;
    }
    if (!out) return NULL;
    if (last < n) cc_sb_append(&out, &ol, &oc, src + last, n - last);
    cc_sb_append_cstr(&out, &ol, &oc, "");
    return out;
}

/* M2: collect UFCS + slice-literal coerce + closure_calls + autoblock +
 * await_normalize across staged applies. */
/* Phase-3 batching is split because UFCS is a *producer* of new call sites
 * that later Phase-3 passes need to see:
 *
 *   - Stage 1 (UFCS_ONLY): collects UFCS edits only.  After applying, the
 *     caller reparses so downstream passes see the lowered `method(obj)`
 *     call form in the AST.
 *
 *   - Stage 1.5 (SLICE_LIT_COERCE): wraps string literals at CCSlice /
 *     char[:0] parameters, and `@as` arg autocast (`Outer*`→`T*` via
 *     `&x.name`).  Own stage so arg-span edits do not overlap
 *     autoblock's whole-call replacements in the same EditBuffer.
 *
 *   - Stage 2 (POST_UFCS): batches closure_calls + autoblock +
 *     await_normalize.  These passes target disjoint constructs (closure
 *     calls / blocking calls / await expressions) and are AST-driven
 *     against the post-coerce reparse, so they compose into a single edit
 *     buffer apply and a single reparse.
 *
 * The single-stage variant (all collectors into one buffer) collided
 * for ~1% of programs because await's replacement of `await EXPR` covers
 * the same byte range as UFCS's replacement of `EXPR` when `EXPR` is a
 * UFCS call inside the await — a genuine semantic dependency that
 * sequential mode resolved via an interleaved reparse.  Staged
 * UFCS-first ordering preserves that while collapsing reparses.
 */
typedef enum CCPhase3Stage {
    CC_PHASE3_STAGE_UFCS_ONLY = 0,
    CC_PHASE3_STAGE_SLICE_LIT_COERCE = 1,
    CC_PHASE3_STAGE_POST_UFCS = 2,
} CCPhase3Stage;

static int cc__apply_batched_phase3_passes(const CCASTRoot* root,
                                           const CCVisitorCtx* ctx,
                                           char** src_io,
                                           size_t* len_io,
                                           const char* base_src,
                                           int* out_changed,
                                           CCPhase3Stage stage) {
    CCEditBuffer eb;
    if (out_changed) *out_changed = 0;
    if (!root || !ctx || !src_io || !*src_io || !len_io) return 0;

    cc_edit_buffer_init(&eb, *src_io, *len_io);
    int rc = 0;
    if (stage == CC_PHASE3_STAGE_UFCS_ONLY) {
        if (cc__collect_ufcs_edits(root, ctx, &eb) < 0) rc = -1;
    } else if (stage == CC_PHASE3_STAGE_SLICE_LIT_COERCE) {
        if (cc__collect_slice_literal_coerce_edits(root, ctx, &eb) < 0) rc = -1;
        if (rc == 0 && cc__collect_as_arg_coerce_edits(root, ctx, &eb) < 0) rc = -1;
    } else {
        if (cc__collect_closure_calls_edits(root, ctx, &eb) < 0 ||
            cc__collect_autoblocking_edits(root, ctx, &eb) < 0 ||
            cc__collect_await_normalize_edits(root, ctx, &eb) < 0) {
            rc = -1;
        }
    }
    if (rc < 0) {
        cc_edit_buffer_free(&eb);
        return -1;
    }
    /* LINE-LEDGER SWEEP: any collected edit whose replacement changes the
     * physical line count (autoblock's multi-line lowering is the main
     * producer) would silently shift every user coordinate stamped below
     * it by a later pass (defer prologue markers, closure-def #line
     * anchors, async machine markers) — redis_idiomatic.ccs drifted +25
     * lines by main().  Drop a masked resync marker on the line AFTER each
     * non-neutral edit so ledger-aware accounting (cc_user_line_for_offset)
     * stays exact.  The marker is inert for passes and becomes a real
     * `#line` at write time. */
    {
        const char* s = *src_io;
        size_t n = *len_io;
        int base_count = eb.count; /* edits appended below must not be re-swept */
        for (int ei = 0; ei < base_count; ei++) {
            const CCEdit* ed = &eb.edits[ei];
            size_t span_nl = 0, repl_nl = 0;
            if (ed->end_off > n || ed->start_off > ed->end_off) continue;
            for (size_t b = ed->start_off; b < ed->end_off; b++)
                if (s[b] == '\n') span_nl++;
            for (const char* r = ed->replacement; r && *r; r++)
                if (*r == '\n') repl_nl++;
            if (span_nl == repl_nl) continue;
            /* Insertion point: start of the line after the edited span. */
            size_t nl = ed->end_off;
            while (nl < n && s[nl] != '\n') nl++;
            size_t ins_at = (nl < n) ? nl + 1 : n;
            /* Skip if that point sits inside (or splits) another edit. */
            int clashes = 0;
            for (int ej = 0; ej < base_count && !clashes; ej++) {
                if (ej == ei) continue;
                if (eb.edits[ej].start_off < ins_at && ins_at < eb.edits[ej].end_off) clashes = 1;
            }
            if (clashes) continue;
            /* One marker per insertion point (several edits can end on the
             * same source line). */
            for (int ej = base_count; ej < eb.count && !clashes; ej++) {
                if (eb.edits[ej].start_off == ins_at) clashes = 1;
            }
            if (clashes) continue;
            const char* lp = NULL;
            size_t lpl = 0;
            int user_line = cc_user_line_for_offset(s, n, nl, 1, &lp, &lpl);
            if (nl < n) user_line++; /* marker governs the NEXT line */
            char mark[1152];
            int mn;
            if (lp && lpl > 0 && lpl < 1024) {
                mn = snprintf(mark, sizeof(mark), "/*CC_LN %d %.*s*/\n", user_line, (int)lpl, lp);
            } else {
                mn = snprintf(mark, sizeof(mark), "/*CC_LN %d %s*/\n", user_line,
                              ctx->input_path ? ctx->input_path : "<cc_input>");
            }
            if (mn <= 0 || (size_t)mn >= sizeof(mark)) continue;
            if (cc_edit_buffer_add(&eb, ins_at, ins_at, mark, 0, "line_ledger_resync") < 0) {
                cc_edit_buffer_free(&eb);
                return -1;
            }
        }
    }
    if (eb.count > 0) {
        size_t new_len = 0;
        char* rewritten = cc_edit_buffer_apply(&eb, &new_len);
        if (rewritten) {
            if (*src_io != base_src) free(*src_io);
            *src_io = rewritten;
            *len_io = new_len;
            if (out_changed) *out_changed = 1;
            if (cc_debug_enabled("LOWER")) {
                cc_debug_log("lower", "phase3 batched stage %d apply: %d edits",
                             (int)stage, eb.count);
            }
            cc_diag_maybe_dump_lowered(stage == CC_PHASE3_STAGE_UFCS_ONLY
                                           ? "phase3-stage1-ufcs"
                                           : stage == CC_PHASE3_STAGE_SLICE_LIT_COERCE
                                                 ? "phase3-stage1_5-slice-lit-coerce"
                                                 : "phase3-stage2-post-ufcs",
                                       rewritten, new_len);
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
    if (strncmp(type_name, "__CC_ARRAY_MAP(", 15) == 0 ||
        strncmp(type_name, "__CC_MAP(", 9) == 0) {
        int is_array = strncmp(type_name, "__CC_ARRAY_MAP(", 15) == 0;
        const char* args = type_name + (is_array ? 15 : 9);
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
        snprintf(scratch, scratch_cap, "%s_%s_%s", is_array ? "ArrayMap" : "Map",
                 mangled_k, mangled_v);
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
    int changed = 0;
    CCInertScan scan;
    if (!src || n == 0) return NULL;
    cc_inert_scan_init(&scan, NULL);

    for (size_t i = 0; i < n; ) {
        if (cc_inert_scan_step(&scan, src, n, &i)) continue;

        if ((i == 0 || !cc_is_ident_char(src[i - 1])) &&
            i + 10 < n && memcmp(src + i, "cc_string_", 10) == 0) {
            size_t method_start = i + 10;
            size_t method_end = method_start;
            const char* replacement = NULL;
            while (method_end < n && cc_is_ident_char(src[method_end])) method_end++;
            if (method_end < n && src[method_end] == '(') {
                size_t method_len = method_end - method_start;
                if (method_len == 6 && memcmp(src + method_start, "append", 6) == 0) replacement = "cc_string_push";
                else if (method_len == 4 && memcmp(src + method_start, "push", 4) == 0) replacement = "cc_string_push";
                else if (method_len == 8 && memcmp(src + method_start, "as_slice", 8) == 0) replacement = "cc_string_as_slice";
                else if (method_len == 5 && memcmp(src + method_start, "clear", 5) == 0) replacement = "cc_string_clear";
                else if (method_len == 4 && memcmp(src + method_start, "cstr", 4) == 0) replacement = "cc_string_cstr";
                else if (method_len == 3 && memcmp(src + method_start, "len", 3) == 0) replacement = "cc_string_len";
                else if (method_len == 3 && memcmp(src + method_start, "cap", 3) == 0) replacement = "cc_string_cap";
                else if (method_len > 0) {
                    static _Thread_local char buf[128];
                    snprintf(buf, sizeof(buf), "cc_string_%.*s", (int)method_len, src + method_start);
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

static CCASTRoot* cc__reparse_source_to_ast_ex(const char* src, size_t src_len,
                                               const char* input_path, CCSymbolTable* symbols,
                                               const char* stage);

static CCASTRoot* cc__reparse_source_to_ast_ctx(const CCVisitorCtx* ctx,
                                                const char* src, size_t src_len,
                                                const char* stage) {
    return cc__reparse_source_to_ast_ex(src, src_len,
                                        ctx ? ctx->input_path : NULL,
                                        ctx ? ctx->symbols : NULL,
                                        stage);
}

/* Conservative detector for member-call surface syntax (`<expr>.method(` or
 * `<expr>->method(`).  UFCS lowering only ever rewrites that shape, so when a
 * buffer contains no such candidate the corresponding UFCS reparse+collect is
 * guaranteed to be a no-op and can be skipped.  Intentionally over-eager
 * (string/comment contents are not excluded): a false positive merely keeps
 * the reparse, a false negative would drop a real lowering — so the scan errs
 * toward keeping the reparse. */
static int cc__has_member_call_candidate(const char* s, size_t n) {
    if (!s) return 0;
    for (size_t i = 0; i + 1 < n; i++) {
        size_t j;
        if (s[i] == '.') {
            j = i + 1;
        } else if (s[i] == '-' && s[i + 1] == '>') {
            j = i + 2;
        } else {
            continue;
        }
        if (j >= n || !cc_is_ident_start(s[j])) continue;
        while (j < n && cc_is_ident_char(s[j])) j++;
        while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
        if (j < n && s[j] == '(') return 1;
    }
    return 0;
}

/* Conservative: phase-3 `@as` arg coerce when the emit buffer declares a local
 * embed (comment-marker form) or names a registry outer that has `@as`. */
static int cc__may_need_as_arg_coerce(const char* s, size_t n) {
    return cc_type_registry_may_need_as_arg_coerce(cc_type_registry_get_global(), s, n);
}

/* Conservative: phase-3 slice-literal coerce when the buffer may pass a string
 * literal to a by-value slice parameter (stdlib or same-TU). False positives
 * only keep a reparse; false negatives drop user-TU wraps. */
static int cc__may_need_slice_lit_coerce(const char* s, size_t n) {
    size_t i;
    int saw_lit = 0;
    if (!s || n == 0) return 0;
    for (i = 0; i < n; i++) {
        if (s[i] == '"') {
            saw_lit = 1;
            break;
        }
    }
    if (!saw_lit) return 0;
    if (cc_contains_token_top_level(s, n, "CCSlice") ||
        cc_contains_token_top_level(s, n, "CCSliceShared") ||
        cc_contains_token_top_level(s, n, "CCSliceUnique"))
        return 1;
    for (i = 0; i + 6 < n; i++) {
        if (memcmp(s + i, "char[:", 6) == 0) return 1;
    }
    /* Stdlib faces that take CCSlice even when the spelling is only in headers. */
    if (cc_contains_token_top_level(s, n, "cc_path_exists") ||
        cc_contains_token_top_level(s, n, "cc_path_is_dir") ||
        cc_contains_token_top_level(s, n, "cc_path_is_file") ||
        cc_contains_token_top_level(s, n, "cc_file_open") ||
        cc_contains_token_top_level(s, n, "cc_dir_open") ||
        cc_contains_token_top_level(s, n, "cc_glob") ||
        cc_contains_token_top_level(s, n, "cc_command") ||
        cc_contains_token_top_level(s, n, "cc_path_join") ||
        cc_contains_token_top_level(s, n, "cc_script_path_join") ||
        cc_contains_token_top_level(s, n, "cc_file_read_path") ||
        cc_contains_token_top_level(s, n, "cc_file_write_path") ||
        cc_contains_token_top_level(s, n, "cc_sh_run"))
        return 1;
    return 0;
}

static CCASTRoot* cc__reparse_source_to_ast_ex(const char* src, size_t src_len,
                                               const char* input_path, CCSymbolTable* symbols,
                                               const char* stage) {
    CCTypeRegistryScope reg_scope;
    int reg_pushed = 0;
    char* reparse_clean = NULL;
    char* prep = NULL;
    char* reparse_surface_sanitized = NULL;
    CCASTRoot* root = NULL;
    const char* pp_in = src;
    size_t pp_in_len = src_len;
    int profile_reparse = (getenv("CC_PROFILE_REPARSE") != NULL);
    double reparse_t0 = profile_reparse ? cc__now_ms() : 0.0;
    if (profile_reparse) g_cc_reparse_count++;
    /* REPARSE DIET tracker: the sanitizers and comment neutralization are
     * length-preserving in-place blanking (coordinate-SAFE) and the prelude
     * is a pure prepend (constant shift).  Only the splicing rewriters can
     * break the parse-offset ↔ source-offset correspondence; record the
     * first one that actually fires so the root can advertise an EXACT
     * shift when none did (root->parse_src_shift). */
    const char* diet_broken_by = NULL;
    long diet_shift = -1;
    long diet_valid_from = 0;   /* src offset after the last splice anchor */
    /* (Reparse diet: the parser-safe family-UFCS rewrite used to run here
     * for concrete family UFCS reintroduced by closure/async edits — but
     * parser mode tolerates those forms.  Probed corpus-green without it,
     * including the two files that actually fired it; deleted.) */
    reparse_clean = cc__check_sanitize_line_parity(
        pp_in, pp_in_len,
        cc__neutralize_comments_for_reparse(pp_in, pp_in_len),
        "neutralize_comments");
    if (reparse_clean) {
        pp_in = reparse_clean;
        pp_in_len = strlen(reparse_clean);
    }
    {
        char* safe_unwrap = cc__check_sanitize_line_parity(
            pp_in, pp_in_len,
            cc__sanitize_statement_unwraps_for_reparse(pp_in, pp_in_len),
            "statement_unwraps");
        if (safe_unwrap) {
            reparse_surface_sanitized = safe_unwrap;
            pp_in = reparse_surface_sanitized;
            pp_in_len = strlen(reparse_surface_sanitized);
        }
        if (strstr(pp_in, "@defer") || strstr(pp_in, "@destroy")) {
            char* lifetime_safe = cc__check_sanitize_line_parity(
                pp_in, pp_in_len,
                cc__sanitize_lifetime_markers_for_reparse(pp_in, pp_in_len),
                "lifetime_markers");
            if (lifetime_safe) {
                free(reparse_surface_sanitized);
                reparse_surface_sanitized = lifetime_safe;
                pp_in = reparse_surface_sanitized;
                pp_in_len = strlen(reparse_surface_sanitized);
            }
        }
        /* `__attribute__((constructor(N)))`: the pinned TCC has no priority
         * support, and whether it ever SEES the attribute is decided by the
         * prelude's libc headers — glibc #define-erases __attribute__ for
         * non-GCC compilers (Linux reparses passed by accident), the Apple
         * SDK does not (macOS reparses died with "')' expected (got '(')").
         * Blank the priority arg IN PLACE (length-preserving → the exact
         * offset invariant is untouched) so tolerance is ours, not libc's. */
        if (strstr(pp_in, "constructor(") || strstr(pp_in, "destructor(")) {
            char* ctor_safe = (char*)malloc(pp_in_len + 1);
            if (ctor_safe) {
                memcpy(ctor_safe, pp_in, pp_in_len + 1);
                if (cc_l2_blank_ctor_priority_inplace(ctor_safe, pp_in_len) > 0) {
                    free(reparse_surface_sanitized);
                    reparse_surface_sanitized = ctor_safe;
                    pp_in = reparse_surface_sanitized;
                } else {
                    free(ctor_safe);
                }
            }
        }
    }
    /* Install the symbol table on the unwrap-destroy pass's ambient slot
     * for the duration of this reparse preprocess.  The preprocess chain
     * invokes `cc__rewrite_unwrap_destroy_suffix` internally to rewrite
     * bodyless `!> @destroy;` on user types, and that lookup needs the
     * symtab hooks from `@comptime cc_type_register(...)` — otherwise the
     * pass diagnoses the type as having "no registered destructor" and
     * aborts the reparse even though the outer codegen pipeline already
     * handled the same site successfully.  Clear on return to avoid
     * leaking a stale table into later unrelated calls. */
    cc_unwrap_destroy_set_symbols(symbols);
    const char* strict_env = getenv("CC_STRICT_RESULT_UNWRAP");
    int strict_had_env = strict_env != NULL;
    char* strict_saved = strict_env ? strdup(strict_env) : NULL;
    /* Strict mode is on by default, so suppressing it for the reparse means
     * setting the opt-out explicitly — unsetting would now enable it. */
    setenv("CC_STRICT_RESULT_UNWRAP", "0", 1);
    char* pp_buf = cc_preprocess_emit_splice(pp_in, pp_in_len, input_path, 1);
    if (pp_buf && !diet_broken_by) {
        /* The splice inserts declaration blocks at known anchors and copies
         * user text verbatim between them, so text after the last anchor
         * keeps an exact constant-shift mapping.  VERIFY the accounting by
         * comparing the tail bytes — an accounting bug must degrade to the
         * line-keyed fallback, never silently corrupt offsets. */
        size_t sp_anchor = 0;
        long sp_delta = 0;
        int sp_rw = 0;
        cc_pp_get_splice_coord_info(&sp_anchor, &sp_delta, &sp_rw);
        size_t ob = strlen(pp_buf);
        if (sp_rw) {
            diet_broken_by = "include_rewrite";
        } else if (sp_anchor > pp_in_len ||
                   (long)ob != (long)pp_in_len + sp_delta ||
                   memcmp(pp_in + sp_anchor,
                          pp_buf + (long)sp_anchor + sp_delta,
                          pp_in_len - sp_anchor) != 0) {
            diet_broken_by = "emit_splice_accounting";
        } else {
            if ((long)sp_anchor > diet_valid_from) diet_valid_from = (long)sp_anchor;
            diet_shift = sp_delta; /* prelude shift added below */
        }
    }
    if (strict_had_env) {
        setenv("CC_STRICT_RESULT_UNWRAP", strict_saved ? strict_saved : "", 1);
    } else {
        unsetenv("CC_STRICT_RESULT_UNWRAP");
    }
    free(strict_saved);
    cc_unwrap_destroy_set_symbols(NULL);
    if (reparse_clean) free(reparse_clean);
    if (reparse_surface_sanitized) free(reparse_surface_sanitized);
    if (!pp_buf) goto done;
    /* (Reparse diet: the chan_send_task text rewrite used to run here on
     * every reparse.  The parse only needs TCC-parseable text and the
     * prelude declares cc_channel_send_task — probed corpus-green without
     * it, so it's gone.  The initial-parse path keeps its own call.) */
    if (strstr(pp_buf, "@defer") || strstr(pp_buf, "@destroy")) {
        char* lifetime_safe = cc__check_sanitize_line_parity(
            pp_buf, strlen(pp_buf),
            cc__sanitize_lifetime_markers_for_reparse(pp_buf, strlen(pp_buf)),
            "lifetime_markers(post-splice)");
        if (lifetime_safe) {
            free(pp_buf);
            pp_buf = lifetime_safe;
        }
    }
    if (strstr(pp_buf, "__cc_uw_is_err") || strstr(pp_buf, "_is_err")) {
        char* unwrap_safe = cc__check_sanitize_line_parity(
            pp_buf, strlen(pp_buf),
            cc__sanitize_generated_unwrap_handlers_for_reparse(pp_buf, strlen(pp_buf)),
            "unwrap_handlers(post-splice)");
        if (unwrap_safe) {
            free(pp_buf);
            pp_buf = unwrap_safe;
        }
    }
    size_t pp_len = strlen(pp_buf);
    {
        size_t body_len = pp_len;
        char* prep0 = cc__prepend_reparse_prelude(pp_buf, pp_len, &pp_len, input_path);
        free(pp_buf);
        pp_buf = NULL;
        if (!prep0) goto done;
        prep = prep0;
        /* pure prepend: parse offset = source offset + shift */
        diet_shift = (diet_shift < 0 ? 0 : diet_shift) + (long)(pp_len - body_len);
    }
    /* (Reparse diet: the result-helper call rewrite
     * (CCResult_X_Y_method(...) -> __cc_parser_result_...) used to run on
     * every reparse and broke offset coordinates in ~every result-using TU.
     * Probed corpus-green without it — TCC's parse tolerates the direct
     * helper names — so it's gone from the reparse path.) */
    if (strstr(prep, "__cc_uw_is_err") || strstr(prep, "_is_err")) {
        char* unwrap_safe = cc__check_sanitize_line_parity(
            prep, pp_len,
            cc__sanitize_generated_unwrap_handlers_for_reparse(prep, pp_len),
            "unwrap_handlers(prepared)");
        if (unwrap_safe) {
            free(prep);
            prep = unwrap_safe;
            pp_len = strlen(prep);
        }
    }
    /* Note: an earlier prototype gated reparse-side pre-expand behind a
     * separate `CC_PRE_EXPAND_REPARSE` env knob.  That path is removed:
     * CPP-expanding the FINAL reparse buffer changes AST line/offset
     * coordinates relative to `src_ufcs` (which is NOT re-expanded by the
     * outer visitor), causing async_ast, UFCS, and other AST walkers to
     * walk past their intended targets.  Proper macro-in-reparse support
     * needs the full M1 swap (visitor consumes the pre-expand buffer
     * end-to-end) — see PASS_INVENTORY.md and COMPILER_CLEANUP_STATUS.md.
     * Until then, reparses use the unexpanded prelude+sanitize chain.
     *
     * (Pre-expanding here was also measured to be a net wall-clock loss:
     * cc_cpp_expand performs the same header preprocessing the TCC parse
     * would, plus an extra re-lower pass over the expanded buffer.) */

    /* (Reparse diet: the L2 prelude rewriter used to run here on every
     * reparse "because TCC would reject the same idioms it rejects on the
     * initial parse" — but the reparse runs in parser mode, which tolerates
     * them.  Probed corpus-green without it, including the l2_rewriter_*
     * tests that exercise those idioms; deleted.) */

    char rel_path[1024];
    cc_path_rel_to_repo(input_path, rel_path, sizeof(rel_path));
    if (cc_debug_enabled("REPARSE")) {
        cc_debug_log("reparse", "stage=%s path=%s", stage ? stage : "?", input_path ? input_path : "?");
    }
    reg_pushed = cc_type_registry_scope_push(&reg_scope);
    cc__debug_dump_reparse_source("reparse_prepared", prep, pp_len, input_path);
    root = cc_tcc_bridge_parse_string_to_ast(prep, rel_path, input_path, symbols);
    if (getenv("CC_DEBUG_REPARSE_DIET")) {
        fprintf(stderr, "CC_REPARSE_DIET: stage=%s %s%s shift=%ld valid_from=%ld path=%s\n",
                stage ? stage : "?",
                diet_broken_by ? "BROKEN by " : "EXACT",
                diet_broken_by ? diet_broken_by : "",
                diet_broken_by ? -1L : diet_shift,
                diet_broken_by ? -1L : diet_valid_from,
                input_path ? input_path : "?");
    }
    if (!root) {
        cc__report_reparse_failure(stage, input_path, src, src_len, prep, pp_len);
        free(prep);
    } else if (!root->parse_buffer) {
        /* Retain the EXACT text TCC lexed: stub-node off_start/off_end
         * address this buffer, and consumers (offset self-checks, span
         * migrations) must slice the same bytes the lexer saw.  Ownership
         * transfers to the root (freed in cc_tcc_bridge_free_ast). */
        root->parse_buffer = prep;
        root->parse_buffer_len = pp_len;
        root->parse_src_shift = diet_broken_by ? -1 : diet_shift;
        root->parse_src_valid_from = diet_broken_by ? 0 : diet_valid_from;
    } else {
        free(prep);
    }
done:
    if (reg_pushed) cc_type_registry_scope_pop(&reg_scope);
    if (profile_reparse) {
        double dt = cc__now_ms() - reparse_t0;
        g_cc_reparse_ms += dt;
        fprintf(stderr, "CC_PROFILE_REPARSE: #%d %-40s %.2f ms (cumulative %.2f ms)\n",
                g_cc_reparse_count, stage ? stage : "<reparse>", dt, g_cc_reparse_ms);
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

/* UFCS span rewrite lives in pass_ufcs.c now (cc__collect_ufcs_edits). */

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


static int cc__find_matching_paren_codegen(const char* src, size_t len, size_t lpar, size_t* out_rpar) {
    /* Find the `)` that matches the `(` at `lpar`, skipping inert
     * content via the shared `CCInertScan` helper. */
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    int depth = 0;
    size_t i = lpar;
    while (i < len) {
        if (cc_inert_scan_step(&scan, src, len, &i)) continue;
        char c = src[i];
        if (c == '(') depth++;
        else if (c == ')') {
            depth--;
            if (depth == 0) {
                *out_rpar = i;
                return 1;
            }
        }
        i++;
    }
    return 0;
}

static int cc__find_matching_brace_codegen(const char* src, size_t len, size_t lbrace, size_t* out_rbrace) {
    /* Find the `}` that matches the `{` at `lbrace`, skipping inert
     * content via the shared `CCInertScan` helper. */
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    int depth = 0;
    size_t i = lbrace;
    while (i < len) {
        if (cc_inert_scan_step(&scan, src, len, &i)) continue;
        char c = src[i];
        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) {
                *out_rbrace = i;
                return 1;
            }
        }
        i++;
    }
    return 0;
}

static const char* cc__canonicalize_type_alias_codegen(const char* type_name) {
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

/*
 * Phase-2 batched UFCS / @comptime hook compilation.
 *
 * Registrations discovered from both `cc_type_register({.ufcs = ...})` and
 * the legacy `cc_ufcs_register(...)` calls inside @comptime blocks are
 * collected into a single pending list, bucketed by the on-disk source that
 * defines the handler body (usually the main TU, but headers also contribute
 * their own bucket), then each bucket is compiled into a single dylib with
 * one exported wrapper per registration.  The produced dylib is owned by a
 * refcounted handle in cc/src/comptime/hook_compile.c; each registered fn_ptr
 * retains one reference.  The dylib closes once every registration drops its
 * reference.
 */

typedef struct {
    int    is_legacy;         /* 0 = set_type_ufcs_callable, 1 = add_legacy_type_ufcs_callable */
    char*  pattern;           /* owned: pattern or type_name */
    int    handler_is_ident;  /* 1 → use handler_name; 0 → emit lambda_src */
    char*  handler_name;      /* owned ident, or NULL */
    char*  lambda_src;        /* owned source span (for lambda mode), or NULL */
    size_t lambda_len;
    char*  bucket_path;       /* owned canonical path: input_path or logical_file */
    char   entry_name[192];   /* generated unique exported symbol name */
} CCUfcsPendingReg;

typedef struct {
    CCUfcsPendingReg* items;
    size_t            count;
    size_t            capacity;
} CCUfcsPendingList;

typedef struct {
    CCUfcsPendingList* pending;
    const char*        default_path;
    const char*        default_src;
    size_t             default_src_len;
} CCUfcsBatchCtx;

static int cc__ufcs_pending_append(CCUfcsPendingList* list, CCUfcsPendingReg item) {
    if (!list) return -1;
    if (list->count + 1 > list->capacity) {
        size_t new_cap = list->capacity ? list->capacity * 2 : 8;
        CCUfcsPendingReg* nv = (CCUfcsPendingReg*)realloc(list->items, new_cap * sizeof(*nv));
        if (!nv) return -1;
        list->items = nv;
        list->capacity = new_cap;
    }
    list->items[list->count++] = item;
    return 0;
}

static void cc__ufcs_pending_free(CCUfcsPendingList* list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; ++i) {
        CCUfcsPendingReg* r = &list->items[i];
        free(r->pattern);
        free(r->handler_name);
        free(r->lambda_src);
        free(r->bucket_path);
    }
    free(list->items);
    list->items = NULL;
    list->count = list->capacity = 0;
}

static char* cc__strndup_local(const char* s, size_t n) {
    char* out;
    if (!s) return NULL;
    out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/*
 * UFCS / @comptime hook batch compile.
 *
 * One pass groups pending registrations by their bucket_path (the on-disk
 * source that defines the handler body) and compiles each bucket into a
 * single dylib via `cc_comptime_compile_type_hooks`.  Each registered
 * function pointer retains one reference to the returned owner; the batch
 * ref is released once all registrations have been recorded.
 */

static int cc__ufcs_compile_bucket(CCSymbolTable* symbols,
                                   const char* reg_input_path,
                                   const char* bucket_path,
                                   const char* bucket_src,
                                   size_t bucket_src_len,
                                   CCUfcsPendingReg** items,
                                   size_t n_items) {
    CCComptimeHookSpec* specs = NULL;
    const void** fn_ptrs = NULL;
    void* owner = NULL;
    int rc = -1;

    if (!symbols || !bucket_src || n_items == 0) return -1;

    specs = (CCComptimeHookSpec*)calloc(n_items, sizeof(*specs));
    fn_ptrs = (const void**)calloc(n_items, sizeof(*fn_ptrs));
    if (!specs || !fn_ptrs) goto done;

    for (size_t i = 0; i < n_items; ++i) {
        CCUfcsPendingReg* r = items[i];
        /* Entry name must be stable across processes so the content-addressed
           dylib cache hits for unchanged TUs.  Derive it purely from inputs
           the user can see (handler/lambda spans + position in the bucket),
           not from heap addresses. */
        const char* hkey  = r->handler_is_ident ? r->handler_name : "lam";
        size_t      hsize = r->handler_is_ident ? strlen(r->handler_name)
                                                : r->lambda_len;
        snprintf(r->entry_name, sizeof(r->entry_name),
                 "__cc_ufcs_hook_%s_%zu_%zu", hkey, hsize, i);
        specs[i].kind = CC_COMPTIME_TYPE_HOOK_UFCS;
        specs[i].entry_name = r->entry_name;
        if (r->handler_is_ident) {
            specs[i].handler_name = r->handler_name;
        } else {
            specs[i].lambda_src = r->lambda_src;
            specs[i].lambda_len = r->lambda_len;
        }
    }

    if (cc_comptime_compile_type_hooks(bucket_path ? bucket_path : reg_input_path,
                                       bucket_path,
                                       bucket_src, bucket_src_len,
                                       specs, n_items,
                                       &owner, fn_ptrs) != 0) {
        fprintf(stderr, "%s: error: failed to compile @comptime UFCS hook batch (bucket=%s, %zu handler%s)\n",
                reg_input_path ? reg_input_path : "<input>",
                bucket_path ? bucket_path : "<none>",
                n_items, n_items == 1 ? "" : "s");
        goto done;
    }

    for (size_t i = 0; i < n_items; ++i) {
        CCUfcsPendingReg* r = items[i];
        const void* fn = fn_ptrs[i];
        int reg_rc;
        cc_comptime_type_hook_owner_retain(owner);
        if (r->is_legacy) {
            reg_rc = cc_symbols_add_legacy_type_ufcs_callable(symbols, r->pattern, fn, owner,
                                                              cc_comptime_type_hook_owner_free);
        } else {
            reg_rc = cc_symbols_set_type_ufcs_callable(symbols, r->pattern, fn, owner,
                                                       cc_comptime_type_hook_owner_free);
        }
        if (reg_rc != 0) {
            fprintf(stderr, "%s: error: failed to record callable UFCS registration for '%s'\n",
                    reg_input_path ? reg_input_path : "<input>", r->pattern);
            cc_comptime_type_hook_owner_free(owner);  /* undo retain */
            goto done;
        }
    }
    rc = 0;

done:
    if (owner) cc_comptime_type_hook_owner_free(owner);  /* drop batch's creator ref */
    free(specs);
    free(fn_ptrs);
    return rc;
}

static int cc__ufcs_pending_compile_and_register(CCSymbolTable* symbols,
                                                 const char* reg_input_path,
                                                 const char* default_src,
                                                 size_t default_src_len,
                                                 CCUfcsPendingList* list) {
    int rc = 0;
    char* loaded_src = NULL;  /* reused across buckets that read from disk */
    size_t loaded_src_len = 0;
    char* loaded_path = NULL;

    if (!list || list->count == 0) return 0;

    /* Simple n² bucketing by bucket_path strcmp — handler counts are tiny. */
    char** visited = (char**)calloc(list->count, sizeof(char*));
    if (!visited) return -1;
    size_t visited_n = 0;

    CCUfcsPendingReg** bucket = (CCUfcsPendingReg**)calloc(list->count, sizeof(*bucket));
    if (!bucket) { free(visited); return -1; }

    for (size_t i = 0; i < list->count; ++i) {
        const char* key = list->items[i].bucket_path;
        int already = 0;
        for (size_t k = 0; k < visited_n; ++k) {
            const char* v = visited[k];
            if ((!v && !key) || (v && key && strcmp(v, key) == 0)) { already = 1; break; }
        }
        if (already) continue;
        visited[visited_n++] = list->items[i].bucket_path;

        size_t bn = 0;
        for (size_t j = 0; j < list->count; ++j) {
            const char* bk = list->items[j].bucket_path;
            if ((!bk && !key) || (bk && key && strcmp(bk, key) == 0)) {
                bucket[bn++] = &list->items[j];
            }
        }
        if (bn == 0) continue;

        const char* bsrc = default_src;
        size_t blen = default_src_len;
        if (key && reg_input_path && strcmp(key, reg_input_path) != 0) {
            /* Handler body lives in a different on-disk file (typically a
               header that registered the hook) — read it once and reuse. */
            if (!loaded_path || strcmp(loaded_path, key) != 0) {
                free(loaded_src);
                loaded_src = NULL;
                loaded_src_len = 0;
                cc__read_entire_file(key, &loaded_src, &loaded_src_len);
                free(loaded_path);
                loaded_path = loaded_src ? strdup(key) : NULL;
            }
            if (loaded_src && loaded_src_len > 0) {
                bsrc = loaded_src;
                blen = loaded_src_len;
            }
        }

        if (cc__ufcs_compile_bucket(symbols, reg_input_path, key, bsrc, blen, bucket, bn) != 0) {
            rc = -1;
            break;
        }
    }

    free(visited);
    free(bucket);
    free(loaded_src);
    free(loaded_path);
    return rc;
}

static int cc__collect_type_ufcs_registration(CCSymbolTable* symbols,
                                              const char* registration_input_path,
                                              const char* logical_file,
                                              const char* type_name,
                                              const char* expr_src,
                                              size_t expr_len,
                                              void* user_ctx) {
    CCUfcsBatchCtx* ctx = (CCUfcsBatchCtx*)user_ctx;
    CCUfcsPendingReg r = {0};
    char ident[128];
    size_t p = 0;
    (void)symbols;  /* we only collect here; registration happens at end-of-pass */

    if (!ctx || !type_name || !expr_src || expr_len == 0) return -1;

    r.is_legacy = 0;
    r.pattern = strdup(type_name);
    if (!r.pattern) return -1;

    if (cc__parse_ident_codegen(expr_src, expr_len, &p, ident, sizeof(ident)) && p == expr_len) {
        r.handler_is_ident = 1;
        r.handler_name = strdup(ident);
        if (!r.handler_name) { free(r.pattern); return -1; }
    } else {
        r.handler_is_ident = 0;
        r.lambda_src = cc__strndup_local(expr_src, expr_len);
        r.lambda_len = expr_len;
        if (!r.lambda_src) { free(r.pattern); return -1; }
    }

    if (logical_file && logical_file[0] && registration_input_path &&
        strcmp(logical_file, registration_input_path) != 0) {
        r.bucket_path = strdup(logical_file);
    } else {
        r.bucket_path = strdup(registration_input_path ? registration_input_path : "");
    }
    if (!r.bucket_path) {
        free(r.pattern); free(r.handler_name); free(r.lambda_src);
        return -1;
    }

    if (cc__ufcs_pending_append(ctx->pending, r) != 0) {
        free(r.pattern); free(r.handler_name); free(r.lambda_src); free(r.bucket_path);
        return -1;
    }
    return 0;
}

static int cc__collect_legacy_ufcs_registrations(CCUfcsPendingList* pending,
                                                 const char* input_path,
                                                 const char* src,
                                                 size_t n) {
    /* CCInertScan replaces inline in_lc/in_bc/in_str/in_chr/line_start
     * tracking AND the inline `# N "file"` parser.  `scan.current_file`
     * holds the most-recent `#line` filename — the exact data the legacy
     * `logical_file` buffer captured, plus support for the `#line N "..."`
     * keyword form.  Full-path compare against `input_path` (below)
     * preserves the legacy bucket-path semantics; we deliberately do NOT
     * use `scan.in_user_file` here because that is a basename match and
     * the legacy code distinguished by full path. */
    CCInertScan scan;
    if (!pending || !src) return 0;
    cc_inert_scan_init(&scan, input_path);
    for (size_t i = 0; i < n; ) {
        if (cc_inert_scan_step(&scan, src, n, &i)) continue;
        char c = src[i];
        if (c != '@' || !cc__match_keyword_codegen(src, n, i + 1, "comptime")) { i++; continue; }
        {
            size_t kw_end = i + 1 + strlen("comptime");
            size_t body_l = cc__skip_ws_codegen(src, n, kw_end);
            size_t body_r;
            if (body_l >= n || src[body_l] != '{') { i++; continue; }
            if (!cc__find_matching_brace_codegen(src, n, body_l, &body_r)) { i++; continue; }
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

                CCUfcsPendingReg r = {0};
                r.is_legacy = 1;
                r.pattern = strdup(pattern);
                if (!r.pattern) return -1;
                if (handler_is_ident) {
                    r.handler_is_ident = 1;
                    r.handler_name = strdup(handler);
                    if (!r.handler_name) { free(r.pattern); return -1; }
                    if (scan.current_file[0] && input_path &&
                        strcmp(scan.current_file, input_path) != 0) {
                        r.bucket_path = strdup(scan.current_file);
                    } else {
                        r.bucket_path = strdup(input_path ? input_path : "");
                    }
                } else {
                    cc__trim_range_codegen(src, &handler_s, &handler_e);
                    r.handler_is_ident = 0;
                    r.lambda_src = cc__strndup_local(src + handler_s, handler_e - handler_s);
                    r.lambda_len = handler_e - handler_s;
                    if (!r.lambda_src) { free(r.pattern); return -1; }
                    r.bucket_path = strdup(input_path ? input_path : "");
                }
                if (!r.bucket_path) {
                    free(r.pattern); free(r.handler_name); free(r.lambda_src);
                    return -1;
                }
                if (cc__ufcs_pending_append(pending, r) != 0) {
                    free(r.pattern); free(r.handler_name); free(r.lambda_src); free(r.bucket_path);
                    return -1;
                }
                j = rpar;
            }
            /* Jump past the matched `@comptime { ... }` body.
             *
             * Stale-scanner-state audit (Batch G watch-out):
             *   - in_str/in_chr/in_line_comment/in_block_comment: all 0
             *     pre-body, all 0 post-body — `cc__find_matching_brace_codegen`
             *     is comment- and string-aware so the body is balanced.
             *   - in_pp: 0 (we entered the body via `@`, not `#`).
             *   - current_file / in_user_file: stable.  `#line` directives
             *     do not appear inside user-written `@comptime` blocks —
             *     the preprocessor emits them around includes, not inside
             *     hand-written user braces.  This matches the legacy
             *     behaviour, which never re-scanned the body for `#line`s
             *     either.
             *   - at_line_start: will self-correct on the next step.
             * No re-init required.
             */
            i = body_r + 1;
        }
    }
    return 0;
}




/* Cosmetic: drop spaces/tabs that sit immediately before each '\n' (and at
 * EOF).  Blanking `@comptime` constructs to spaces (to keep byte offsets and
 * line numbers stable for earlier passes) leaves long all-blank runs in the
 * emitted C.  Stripping trailing whitespace removes that noise.  Crucially it
 * preserves the line *count* (only intra-line trailing blanks are removed), so
 * `#line` directives in the output stay exact.  Must run only on the final
 * source, after all span-sensitive reparses.  Compacts in place (w <= r). */
static void cc__strip_trailing_ws_in_place(char* s, size_t* io_len) {
    size_t n, w = 0, dst_line_start = 0;
    if (!s || !io_len) return;
    n = *io_len;
    for (size_t r = 0; r < n; r++) {
        char c = s[r];
        if (c == '\n') {
            while (w > dst_line_start && (s[w - 1] == ' ' || s[w - 1] == '\t')) w--;
            s[w++] = '\n';
            dst_line_start = w;
        } else {
            s[w++] = c;
        }
    }
    while (w > dst_line_start && (s[w - 1] == ' ' || s[w - 1] == '\t')) w--;
    s[w] = '\0';
    *io_len = w;
}

/* True when [s, s+n) is a `#line` preprocessor directive (leading blanks ok). */
static int cc__line_is_line_directive(const char* s, size_t n) {
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    return i + 5 <= n && memcmp(s + i, "#line", 5) == 0 &&
           (i + 5 == n || s[i + 5] == ' ' || s[i + 5] == '\t');
}

/* mark[i]=1 iff s[i] starts a live (non-comment/string) #line line.
 * Comment-blind text walkers must consult this instead of scanning raw
 * lines — a #line inside a block comment must not be renumbered/deleted
 * (and must not swallow the comment closer on that line). */
static unsigned char* cc__mark_live_line_directives(const char* s, size_t n) {
    unsigned char* mark;
    CCInertScan scan;
    size_t i;
    if (!s) return NULL;
    mark = (unsigned char*)calloc(n ? n : 1, 1);
    if (!mark) return NULL;
    cc_inert_scan_init(&scan, NULL);
    for (i = 0; i < n; ) {
        if (scan.at_line_start &&
            !scan.in_block_comment && !scan.in_line_comment &&
            !scan.in_str && !scan.in_chr && !scan.in_pp) {
            size_t e = i;
            while (e < n && s[e] != '\n') e++;
            if (cc__line_is_line_directive(s + i, e - i)) mark[i] = 1;
        }
        if (cc_inert_scan_step(&scan, s, n, &i)) continue;
        i++;
    }
    return mark;
}

/* Cosmetic: when a `#line` directive is immediately followed by another
 * `#line` directive, only the last of the run affects the mapping — drop the
 * earlier ones.  Pass rewinds leave these back-to-back ping-pong pairs in the
 * lowered body.  Removes whole lines, so it must run only on the final
 * buffer, immediately before the write.  Compacts in place (w <= r). */
static void cc__coalesce_adjacent_line_directives(char* s, size_t* io_len) {
    size_t n, r = 0, w = 0;
    unsigned char* live;
    if (!s || !io_len) return;
    n = *io_len;
    live = cc__mark_live_line_directives(s, n);
    if (!live) return; /* OOM: leave buffer unchanged */
    while (r < n) {
        size_t e = r;
        while (e < n && s[e] != '\n') e++;
        int drop = 0;
        if (e < n && live[r]) {
            size_t nr = e + 1;
            if (nr < n && live[nr]) drop = 1;
        }
        if (!drop) {
            size_t m = (e < n ? e + 1 : e) - r;
            if (w != r) memmove(s + w, s + r, m);
            w += m;
        }
        r = (e < n) ? e + 1 : e;
    }
    free(live);
    s[w] = '\0';
    *io_len = w;
}

/* Parse a numbered `#line N ...` line: returns N (> 0) and the [num_a,
 * num_b) byte range of the digits, or 0 when not a numbered directive. */
static long cc__line_directive_number(const char* s, size_t n,
                                      size_t* num_a, size_t* num_b) {
    size_t i = 0;
    long v = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i + 5 > n || memcmp(s + i, "#line", 5) != 0) return 0;
    i += 5;
    if (i < n && s[i] != ' ' && s[i] != '\t') return 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= n || s[i] < '0' || s[i] > '9') return 0;
    *num_a = i;
    while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
    *num_b = i;
    return v;
}

/* Cosmetic: blank-line runs around `#line` directives.  Passes blank whole
 * constructs (doc comments, @comptime bodies) in place to keep physical and
 * user lines aligned, which leaves runs of empty lines in the lowered
 * output.  Two mapping-exact rewrites clean them up:
 *
 *   #line N  +  k>=2 blank lines  +  code       =>  #line N+k  +  code
 *   #line .. +  blanks            +  #line ..   =>  (second directive only)
 *   k>=2 blank lines              +  #line ..   =>  1 blank  +  #line ..
 *
 * The first advances the anchor instead of padding to it; the others rely
 * on the following directive re-anchoring.  Removes whole lines, so it
 * must run only on the final buffer (after trailing-ws strip, so blank
 * means empty).  Renumber stages the rewritten directive in a stack
 * buffer before writing — when the new number gains a digit and no
 * write-slack has accumulated yet, an in-place digit memcpy would clobber
 * the space before the filename (`#line 122"file"`), which host CCs accept
 * silently and defeat diagnostic/__LINE__ fidelity below the anchor. */
static void cc__compact_blank_runs_at_line_directives(char* s, size_t* io_len) {
    size_t n, r = 0, w = 0;
    unsigned char* live;
    if (!s || !io_len) return;
    n = *io_len;
    live = cc__mark_live_line_directives(s, n);
    if (!live) return; /* OOM: leave buffer unchanged */
    while (r < n) {
        size_t e = r;
        while (e < n && s[e] != '\n') e++;
        /* Measure the blank run starting at line `p` (possibly empty run),
         * leaving `p` at the first non-blank line start and `k` its size. */
        size_t k = 0, p = (e < n) ? e + 1 : n;
        int line_is_blank = (e == r);
        if (line_is_blank) { k = 1; p = (e < n) ? e + 1 : n; }
        {
            size_t q = p;
            while (q < n && s[q] == '\n') { k++; q++; }
            p = q;
        }
        size_t pe = p;
        while (pe < n && s[pe] != '\n') pe++;
        int next_is_dir = (p < n) && live[p];

        if (!line_is_blank) {
            size_t na = 0, nb = 0;
            long ln = live[r] ? cc__line_directive_number(s + r, e - r, &na, &nb) : 0;
            if (ln > 0 && k > 0 && p < n) {
                if (next_is_dir) {
                    /* Directive + blanks + directive: drop this one. */
                    r = p;
                    continue;
                }
                if (k >= 2) {
                    /* Renumber past the dropped blanks — stage first. */
                    char num[24];
                    char staged[PATH_MAX + 64];
                    int nd = snprintf(num, sizeof(num), "%ld", ln + (long)k);
                    size_t prefix = na;
                    size_t tail = e - (r + nb);
                    size_t need = prefix + (size_t)nd + tail + 1; /* + '\n' */
                    if (nd > 0 && need <= sizeof(staged)) {
                        memcpy(staged, s + r, prefix);
                        memcpy(staged + prefix, num, (size_t)nd);
                        memcpy(staged + prefix + (size_t)nd, s + r + nb, tail);
                        staged[prefix + (size_t)nd + tail] = '\n';
                        memmove(s + w, staged, need);
                        w += need;
                        r = p;
                        continue;
                    }
                    /* Path too long for stack stage: skip renumber, keep line. */
                }
            }
            /* Ordinary line: copy through. */
            size_t m = (e < n ? e + 1 : e) - r;
            if (w != r) memmove(s + w, s + r, m);
            w += m;
            r = (e < n) ? e + 1 : e;
            continue;
        }
        /* Blank run: collapse to one blank when a directive re-anchors. */
        if (next_is_dir && k >= 2) {
            s[w++] = '\n';
            r = p;
            continue;
        }
        while (k-- > 0) s[w++] = '\n';
        r = p;
    }
    free(live);
    s[w] = '\0';
    *io_len = w;
}

/* Formerly collapsed `__typeof__(EXPR) name = (EXPR)` → `__auto_type name = (EXPR)`
 * on the host buffer. Disabled: vendored TCC rejects `__auto_type`, and emit-c /
 * host-compile can be separate steps. Keep the `__typeof__` spelling. */
static void cc__collapse_typeof_dup_decls(char* s, size_t* io_len) {
    (void)s;
    (void)io_len;
}

static void cc__register_ufcs_declared_vars_for_type(CCTypeRegistry* reg,
                                                     const char* type_name,
                                                     const char* src,
                                                     size_t n) {
    size_t type_len = type_name ? strlen(type_name) : 0;
    CCInertScan scan;
    if (!reg || !type_name || !type_len || !src) return;
    cc_inert_scan_init(&scan, NULL);
    for (size_t i = 0; i < n; ) {
        if (cc_inert_scan_step(&scan, src, n, &i)) continue;
        if (!cc__match_keyword_codegen(src, n, i, type_name)) { i++; continue; }
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
                /* Original used `continue;` here when `src[v] == '('`
                 * (function-call shape, not a decl).  In the new while
                 * loop, `continue` would skip the `i += type_len`
                 * advance and re-test the same byte forever.  Invert to
                 * a guard so we still fall through to the advance. */
                if (!(v < n && src[v] == '(')) {
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
        }
        i += type_len ? type_len : 1;
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
        const char* before;
        /* Reject call arguments (`fn(&name)` / `fn(name)`): last ident is not
         * a declarator.  Mirror primary `cc_parse_decl_name_and_type`. */
        before = name_start;
        while (before > s && isspace((unsigned char)before[-1])) before--;
        if (before > s && (before[-1] == '&' || before[-1] == '(' || before[-1] == ',')) {
            return 0;
        }
        if (name_len >= decl_name_sz) name_len = decl_name_sz - 1;
        if (type_len >= decl_type_sz) type_len = decl_type_sz - 1;
        memcpy(decl_name, name_start, name_len);
        decl_name[name_len] = '\0';
        memcpy(decl_type, s, type_len);
        decl_type[type_len] = '\0';
        if (cc_is_non_decl_stmt_type(decl_type)) {
            decl_name[0] = '\0';
            decl_type[0] = '\0';
            return 0;
        }
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
    type_name = cc__canonicalize_type_alias_codegen(type_name);
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
    int paren_depth = 0, bracket_depth = 0;
    size_t stmt_start = 0;
    size_t i = 0;
    CCInertScan scan;
    if (!src || !var_name || !var_name[0] || !out_type || out_type_sz == 0) return NULL;
    out_type[0] = '\0';
    scope_stack[0] = 0;
    cc_inert_scan_init(&scan, NULL);
    scan.at_line_start = 0;  /* src is a mid-buffer slice */
    while (i < limit) {
        if (cc_inert_scan_step(&scan, src, limit, &i)) continue;
        char c = src[i];
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
        const char* canon = cc__canonicalize_type_alias_codegen(out_type);
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
    CCInertScan scan;
    if (!src || !var_name || !var_name[0] || !out_type || out_type_sz == 0) return NULL;
    out_type[0] = '\0';
    cc_inert_scan_init(&scan, NULL);
    scan.at_line_start = 0;  /* src is a mid-buffer slice */
    while (i < limit) {
        if (cc_inert_scan_step(&scan, src, limit, &i)) continue;
        char c = src[i];
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
    type_name = cc__canonicalize_type_alias_codegen(type_name);
    if (strcmp(family, "cc_string_from") == 0) {
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
        if (strcmp(type_name, "char") == 0) return "cc_string_push_char";
        if (strcmp(type_name, "signed char") == 0) return "cc_string_push_int";
        if (strcmp(type_name, "unsigned char") == 0) return "cc_string_push_uint";
        if (strcmp(type_name, "short") == 0) return "cc_string_push_int";
        if (strcmp(type_name, "unsigned short") == 0) return "cc_string_push_uint";
        if (strcmp(type_name, "int") == 0) return "cc_string_push_int";
        if (strcmp(type_name, "unsigned") == 0) return "cc_string_push_uint";
        if (strcmp(type_name, "long") == 0) return "cc_string_push_int";
        if (strcmp(type_name, "unsigned long") == 0) return "cc_string_push_uint";
        if (strcmp(type_name, "long long") == 0) return "cc_string_push_int";
        if (strcmp(type_name, "unsigned long long") == 0) return "cc_string_push_uint";
        if (strcmp(type_name, "int8_t") == 0) return "cc_string_push_int";
        if (strcmp(type_name, "uint8_t") == 0) return "cc_string_push_uint";
        if (strcmp(type_name, "int16_t") == 0) return "cc_string_push_int";
        if (strcmp(type_name, "uint16_t") == 0) return "cc_string_push_uint";
        if (strcmp(type_name, "int32_t") == 0) return "cc_string_push_int";
        if (strcmp(type_name, "uint32_t") == 0) return "cc_string_push_uint";
        if (strcmp(type_name, "int64_t") == 0) return "cc_string_push_int";
        if (strcmp(type_name, "uint64_t") == 0) return "cc_string_push_uint";
        if (strcmp(type_name, "intptr_t") == 0) return "cc_string_push_int";
        if (strcmp(type_name, "uintptr_t") == 0) return "cc_string_push_uint";
        if (strcmp(type_name, "size_t") == 0) return "cc_string_push_uint";
        if (strcmp(type_name, "float") == 0) return "cc_string_push_f32";
        if (strcmp(type_name, "double") == 0) return "cc_string_push_f64";
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
        if (invalid) return NULL;
        if (has_operator && has_digit) {
            strncpy(type_buf, has_float_marker ? "double" : "int", type_buf_sz - 1);
            type_buf[type_buf_sz - 1] = '\0';
            return cc__string_helper_for_type_codegen(family, type_buf);
        }
    }
    {
        /* Pure numeric token with optional type suffix (42ull, 1.0f). */
        char suffix[8];
        size_t suf_len = 0;
        const char* p = e;
        const char* num_end;
        int has_digit = 0;
        while (p > s && isalpha((unsigned char)p[-1]) && suf_len + 1 < sizeof(suffix)) {
            suffix[suf_len++] = (char)tolower((unsigned char)p[-1]);
            p--;
        }
        suffix[suf_len] = '\0';
        num_end = p;
        for (p = s; p < num_end; ++p) {
            char c = *p;
            if (isdigit((unsigned char)c)) {
                has_digit = 1;
                continue;
            }
            if (c == '.' || c == '+' || c == '-' || c == 'e' || c == 'E') continue;
            return NULL;
        }
        if (!has_digit) return NULL;
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
        } else if (strchr(suffix, 'f')) {
            strncpy(type_buf, "float", type_buf_sz - 1);
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
        if (family_len == 14 && memcmp(src + ident_start, "cc_string_from", 14) == 0) {
            family = "cc_string_from";
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

/* [Removed Phase 3.3] cc__canonicalize_parser_family_macro_codegen and
   cc__rewrite_parser_generic_family_helpers_to_concrete: the end-of-
   pipeline placeholder rewriter used to scan emitted source for
   `__cc_vec_generic_*` / `__cc_map_generic_*` idents and substitute
   concrete `CCVec_<T>` / `Map_<K>_<V>` callees.  The AST UFCS pass now
   performs the same canonicalization up front (see
   cc__ufcs_canonicalize_family_macro in visitor/ufcs.c), so no
   placeholder form is ever emitted and this post-sweep is dead code. */

static void cc__collect_ufcs_field_and_var_types(const char* src, size_t n) {
    CCTypeRegistry* reg = cc_type_registry_get_global();
    size_t i = 0;
    CCInertScan scan;
    if (!reg || !src) return;
    cc_inert_scan_init(&scan, NULL);
    while (i < n) {
        if (cc_inert_scan_step(&scan, src, n, &i)) continue;
        char c = src[i];
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
                            /* Top-level ';' only (skip comments/strings/nests).
                             * Naive memchr poisons the next field when a
                             * trailing block comment contains a semicolon. */
                            while (stmt < body_end) {
                                size_t stmt_off = (size_t)(stmt - src);
                                size_t end_off = (size_t)(body_end - src);
                                size_t semi_off = cc_find_char_top_level(src, stmt_off, end_off, ';');
                                if (semi_off >= end_off) break;
                                const char* semi = src + semi_off;
                                char field_name[128];
                                char field_type[256];
                                int field_is_as = 0;
                                cc_parse_decl_name_and_type_ex(stmt, semi, field_name, sizeof(field_name),
                                                               field_type, sizeof(field_type), &field_is_as);
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
                                        (void)cc_type_registry_add_field_ex(reg, struct_name, field_name,
                                                                            canonical_field_type, field_is_as);
                                    } else {
                                        (void)cc_type_registry_add_field_ex(reg, struct_name, field_name,
                                                                            field_type, field_is_as);
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
            } else if (type_end - type_start == sizeof("__CC_ARRAY_MAP") - 1 &&
                       memcmp(src + type_start, "__CC_ARRAY_MAP",
                              sizeof("__CC_ARRAY_MAP") - 1) == 0) {
                size_t macro_l = cc__skip_ws_codegen(src, n, type_end);
                size_t macro_r = 0;
                if (macro_l < n && src[macro_l] == '(' &&
                    cc__find_matching_paren_codegen(src, n, macro_l, &macro_r)) {
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

/* Re-lower generated closure definitions (defer syntax, chan send_task,
 * parser-placeholder UFCS lowers).  Closure bodies are synthesized AFTER the
 * main buffer's lowering passes ran, so the same three rewrites must run
 * again on the side buffer.  Shared by the merge-into-src_ufcs path and the
 * end-of-file emission path — they carried verbatim copies of this block. */
static void cc__relower_closure_defs(CCVisitorCtx* ctx, char** defs, size_t* defs_len) {
    if (!defs || !*defs || !*defs_len) return;
    if (cc_contains_token_top_level(*defs, *defs_len, "@defer") ||
        cc_contains_token_top_level(*defs, *defs_len, "cancel")) {
        char* lowered = NULL;
        size_t lowered_len = 0;
        if (cc__rewrite_defer_syntax(ctx, *defs, *defs_len, &lowered, &lowered_len) > 0) {
            free(*defs);
            *defs = lowered;
            *defs_len = lowered_len;
        }
    }
    if (cc_contains_token_top_level(*defs, *defs_len, "send_task") ||
        cc_contains_token_top_level(*defs, *defs_len, "cc_channel_send_task")) {
        size_t rewritten_len = 0;
        char* rewritten = cc__rewrite_chan_send_task_text(ctx, *defs, *defs_len, &rewritten_len);
        if (rewritten) {
            free(*defs);
            *defs = rewritten;
            *defs_len = rewritten_len;
        }
    }
    {
        char* rewritten = NULL;
        CCTypeRegistryScope reg_scope;
        int reg_pushed = cc_type_registry_scope_push(&reg_scope);
        if (reg_pushed && ctx && ctx->symbols) {
            cc__collect_registered_ufcs_var_types(ctx->symbols, *defs, *defs_len);
        }
        rewritten = cc__rewrite_parser_placeholder_ufcs_lowers(*defs, *defs_len);
        if (reg_pushed) cc_type_registry_scope_pop(&reg_scope);
        if (rewritten) {
            free(*defs);
            *defs = rewritten;
            *defs_len = strlen(rewritten);
        }
    }
}

/* Copy `buf` dropping every `#line` directive line (whitespace-tolerant).
 * Returns a malloc'd buffer (NULL on OOM) and its length via out_len. */
/* #line directives inside pass-visible buffers break physical-line
 * addressing (the final UFCS sweep needs logical == physical), but the
 * EMITTED C needs them for user-line diagnostics inside hoisted closure
 * bodies.  Resolution: MASK directives to same-line inert comments while
 * passes run (`#line 12 "f.ccs"` -> `/;*CC_LN 12 f.ccs*;/` sans the
 * semicolons), then UNMASK at write time.  Line counts are preserved in
 * both directions, so the mask is coordinate-neutral.  (The old approach
 * DELETED the directive lines — user diagnostics inside closures then
 * pointed at a synthetic "<cc-closures>" file; oracle test:
 * diag_oracle_closure_fail.) */
static char* cc__mask_line_directives(const char* buf, size_t len, size_t* out_len) {
    char* out = (char*)malloc(len + len / 2 + 64);
    size_t cap = len + len / 2 + 64;
    if (!out) { *out_len = 0; return NULL; }
    size_t di = 0;
    for (size_t i = 0; i < len;) {
        size_t ln_end = i;
        while (ln_end < len && buf[ln_end] != '\n') ln_end++;
        size_t ss = i;
        while (ss < ln_end && (buf[ss] == ' ' || buf[ss] == '\t')) ss++;
        int is_line_directive = 0;
        size_t num_start = 0, num_end = 0, path_start = 0, path_end = 0;
        if (ss < ln_end && buf[ss] == '#') {
            size_t ps = ss + 1;
            while (ps < ln_end && (buf[ps] == ' ' || buf[ps] == '\t')) ps++;
            if (ps + 4 <= ln_end && memcmp(buf + ps, "line", 4) == 0 &&
                (ps + 4 == ln_end || buf[ps + 4] == ' ' || buf[ps + 4] == '\t')) {
                size_t q = ps + 4;
                while (q < ln_end && (buf[q] == ' ' || buf[q] == '\t')) q++;
                num_start = q;
                while (q < ln_end && buf[q] >= '0' && buf[q] <= '9') q++;
                num_end = q;
                while (q < ln_end && (buf[q] == ' ' || buf[q] == '\t')) q++;
                if (q < ln_end && buf[q] == '"') {
                    path_start = q + 1;
                    size_t e = ln_end;
                    while (e > path_start && buf[e - 1] != '"') e--;
                    path_end = (e > path_start) ? e - 1 : path_start;
                }
                if (num_end > num_start) is_line_directive = 1;
            }
        }
        size_t need = (ln_end - i) + 32 + (path_end - path_start);
        if (di + need + 2 > cap) {
            cap = (di + need + 2) * 2;
            char* g = (char*)realloc(out, cap);
            if (!g) { free(out); *out_len = 0; return NULL; }
            out = g;
        }
        if (is_line_directive) {
            di += (size_t)snprintf(out + di, cap - di, "/*CC_LN %.*s %.*s*/",
                                   (int)(num_end - num_start), buf + num_start,
                                   (int)(path_end - path_start), buf + path_start);
            if (ln_end < len) out[di++] = '\n';
            i = (ln_end < len) ? ln_end + 1 : ln_end;
        } else {
            size_t n = (ln_end < len ? ln_end + 1 : ln_end) - i;
            memcpy(out + di, buf + i, n);
            di += n;
            i += n;
        }
    }
    out[di] = '\0';
    *out_len = di;
    return out;
}

/* Inverse of cc__mask_line_directives: `/;*CC_LN N PATH*;/` lines (sans
 * semicolons) become `#line N "PATH"`.  Returns NULL when no marker was
 * found (caller keeps the original buffer). */
static char* cc__unmask_line_directives(const char* buf, size_t len, size_t* out_len) {
    if (!strstr(buf, "/*CC_LN ")) return NULL; /* buf is NUL-terminated */
    size_t cap = len + 256;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    size_t di = 0;
    int any = 0;
    for (size_t i = 0; i < len;) {
        size_t ln_end = i;
        while (ln_end < len && buf[ln_end] != '\n') ln_end++;
        size_t ss = i;
        while (ss < ln_end && (buf[ss] == ' ' || buf[ss] == '\t')) ss++;
        int is_marker = 0;
        size_t num_start = 0, num_end = 0, path_start = 0, path_end = 0;
        if (ss + 8 <= ln_end && memcmp(buf + ss, "/*CC_LN ", 8) == 0 &&
            ln_end >= 2 && buf[ln_end - 2] == '*' && buf[ln_end - 1] == '/') {
            size_t q = ss + 8;
            num_start = q;
            while (q < ln_end && buf[q] >= '0' && buf[q] <= '9') q++;
            num_end = q;
            if (num_end > num_start && q < ln_end && buf[q] == ' ') {
                path_start = q + 1;
                path_end = ln_end - 2;
                is_marker = 1;
            } else if (num_end > num_start && q == ln_end - 2) {
                path_start = path_end = 0;
                is_marker = 1;
            }
        }
        size_t need = (ln_end - i) + 32;
        if (di + need + 2 > cap) {
            cap = (di + need + 2) * 2;
            char* g = (char*)realloc(out, cap);
            if (!g) { free(out); return NULL; }
            out = g;
        }
        if (is_marker) {
            if (path_end > path_start) {
                di += (size_t)snprintf(out + di, cap - di, "#line %.*s \"%.*s\"",
                                       (int)(num_end - num_start), buf + num_start,
                                       (int)(path_end - path_start), buf + path_start);
            } else {
                di += (size_t)snprintf(out + di, cap - di, "#line %.*s",
                                       (int)(num_end - num_start), buf + num_start);
            }
            if (ln_end < len) out[di++] = '\n';
            i = (ln_end < len) ? ln_end + 1 : ln_end;
            any = 1;
        } else {
            size_t n = (ln_end < len ? ln_end + 1 : ln_end) - i;
            memcpy(out + di, buf + i, n);
            di += n;
            i += n;
        }
    }
    if (!any) { free(out); return NULL; }
    out[di] = '\0';
    *out_len = di;
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
            const CCNodeView* n = (const CCNodeView*)root->nodes;
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
       succeed. */
    /* Canonical buffer from initial parse (cc_build_parse_input). */
    char* src_all = NULL;
    char* src_raw = NULL;
    char* src_regs = NULL;
    int src_regs_owned = 0;       /* 0 when borrowing root->comptime_buffer */
    int need_phase3_ast = 0;
    char* src_ufcs = NULL;        /* aliases src_all until a rewrite fires */
    size_t src_ufcs_len = 0;
    char* closure_protos = NULL;  /* closure-literal pass output */
    size_t closure_protos_len = 0;
    char* closure_defs = NULL;
    size_t closure_defs_len = 0;
    int need_container_decls = 0;
    size_t src_len = 0;
    size_t src_raw_len = 0;
    if (!root || !root->codegen_buffer || !root->codegen_buffer_len) {
        fclose(out);
        return EINVAL;
    }
    src_all = strdup(root->codegen_buffer);
    if (!src_all) {
        fclose(out);
        return ENOMEM;
    }
    src_len = root->codegen_buffer_len;
    if (ctx->input_path) {
        cc__read_entire_file(ctx->input_path, &src_raw, &src_raw_len);
    }
    if (src_all && src_len && ctx && ctx->symbols) {
        /* Prefer the authoritative buffer stashed at parse time. */
        if (root->comptime_buffer && root->comptime_buffer_len) {
            src_regs = root->comptime_buffer;
        } else {
            src_regs = cc_preprocess_comptime_source(ctx->input_path);
            src_regs_owned = src_regs != NULL;
        }
    }

    src_ufcs = src_all;
    src_ufcs_len = src_len;
    const char* reg_src = src_regs ? src_regs : src_all;
    size_t reg_src_len = src_regs
        ? (src_regs_owned ? strlen(src_regs) : root->comptime_buffer_len)
        : src_len;
    const char* hook_src = src_raw ? src_raw : src_all;
    size_t hook_src_len = src_raw ? src_raw_len : src_len;

    /* Phase 2 currently means: collect the comptime-visible effects from the
       canonical CC source (type registrations, UFCS handlers, etc.). After
       that, phase 3 blanks source-local @comptime blocks out of the emitted TU
       while preserving layout so earlier AST spans remain valid. The original
       source still drives named/lambda handler compilation where exact local
       context matters; the comptime discovery input itself should come from one
       authoritative source. */
    if (reg_src && reg_src_len && ctx && ctx->symbols) {
        CCUfcsPendingList pending = {0};
        CCUfcsBatchCtx batch_ctx = {
            .pending = &pending,
            .default_path = ctx->input_path,
            .default_src = hook_src,
            .default_src_len = hook_src_len,
        };
        if (cc_symbols_collect_type_registrations_ex(ctx->symbols,
                                                     ctx->input_path,
                                                     reg_src,
                                                     reg_src_len,
                                                     NULL,
                                                     NULL,
                                                     cc__collect_type_ufcs_registration,
                                                     &batch_ctx) != 0) {
            cc__ufcs_pending_free(&pending);
            goto fail;
        }
        /* Type-graph maps are known by now (preprocess registered them);
         * seed method tables before any UFCS rewrite. */
        cc__seed_map_ufcs_from_type_graph(ctx->symbols);
        if (cc__collect_legacy_ufcs_registrations(&pending, ctx->input_path,
                                                  reg_src, reg_src_len) != 0) {
            cc__ufcs_pending_free(&pending);
            goto fail;
        }
        int reg_rc = cc__ufcs_pending_compile_and_register(ctx->symbols, ctx->input_path,
                                                           hook_src, hook_src_len, &pending);
        cc__ufcs_pending_free(&pending);
        if (reg_rc != 0) {
            goto fail;
        }
        /* Generated grammar types (NameReader, NameNode): the @grammar
         * engines noted them during the prepare-phase splice; register each
         * with the NATIVE Type_method hook so instance UFCS (`r.next(&out)`,
         * `nd.first()`) lowers with no user-written registration. */
        for (int gi = 0; gi < cc_grammar_pending_ufcs_type_count(); gi++) {
            const char* tn = cc_grammar_pending_ufcs_type(gi);
            char tnp[96];
            if (!tn) continue;
            (void)cc_symbols_set_type_ufcs_callable(ctx->symbols, tn,
                    cc_ufcs_grammar_type_method_native_ptr(), NULL, NULL);
            snprintf(tnp, sizeof tnp, "%s*", tn);
            (void)cc_symbols_set_type_ufcs_callable(ctx->symbols, tnp,
                    cc_ufcs_grammar_type_method_native_ptr(), NULL, NULL);
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
    }
    if (src_regs_owned) free(src_regs);
    src_regs = NULL;

    /* Replay explicit @comptime cc_instantiate_* requests (track C1) into the
     * global registry so forced monomorphs (never spelled as CCVec::[T] in
     * source) still get CC_*_DECL_ARENA emitted in the final .c. */
    cc_emit_plan_apply_comptime_instantiations(cc_type_graph_get_global());

    /* src_raw's last use is the comptime hook phase above.  Free it HERE,
     * not at the function's tail: this function has ~30 early returns with
     * hand-copied cleanup blocks, and the copies had already diverged —
     * every error return below this point leaked src_raw (audit finding).
     * Ending the lifetime at end-of-use makes the later paths structurally
     * unable to leak it. */
    free(src_raw);
    src_raw = NULL;

    /* Produced by the closure-literal AST pass (emitted into the output TU). */

    /* Phase 3 currently uses coarse whole-file rewrites for several AST-driven
       passes. Run them sequentially with reparsing between changed passes so
       whole-file snapshots do not collide in the shared edit buffer. */
#ifdef CC_TCC_EXT_AVAILABLE
    /* Phase-3 AST sync reparse is only useful when a stage will walk the AST.
     * Skip it (and the staged collectors) when the emit-ready buffer has no
     * UFCS/async/closure/call-site-mode surface — same gating pattern as the
     * later statement/async reparses. Initial AST spans are not used for edits
     * in that case. */
    need_phase3_ast = src_ufcs && src_ufcs_len && ctx && ctx->symbols &&
        (cc__has_member_call_candidate(src_ufcs, src_ufcs_len) ||
         cc_contains_token_top_level(src_ufcs, src_ufcs_len, "=>") ||
         cc_contains_token_top_level(src_ufcs, src_ufcs_len, "@async") ||
         cc_contains_token_top_level(src_ufcs, src_ufcs_len, "await") ||
         cc_contains_token_top_level(src_ufcs, src_ufcs_len, "@blocking") ||
         cc_contains_token_top_level(src_ufcs, src_ufcs_len, "@noblock") ||
         cc_contains_token_top_level(src_ufcs, src_ufcs_len, "@nonblocking") ||
         /* Slice-literal coerce (stage 1.5): stdlib and same-TU CCSlice /
          * char[:] callees with bare string literals. */
         cc__may_need_slice_lit_coerce(src_ufcs, src_ufcs_len) ||
         /* `@as` arg coerce (same stage): Outer* / &Outer where T* expected. */
         cc__may_need_as_arg_coerce(src_ufcs, src_ufcs_len) ||
         /* Dump-dir selftests (scripts/test_reparse_sanitize.sh) assert on
          * reparse_prepared_* output; keep one entry reparse when requested. */
         getenv("CC_DEBUG_REPARSE_DUMP_DIR") != NULL);
    if (src_ufcs && root && root->nodes && root->node_count > 0 && need_phase3_ast) {
        const CCASTRoot* phase3_root = root;
        CCASTRoot* phase3_owned_root = NULL;
        int phase3_changed = 0;
        /* Initial AST was built from CPP-expanded parse_view; src_ufcs is the
         * emit-ready buffer.  Reparse once so phase-3 AST walks match the text
         * UFCS and downstream coarse passes will edit. */
        cc__debug_dump_reparse_source("phase3_entry", src_ufcs, src_ufcs_len, ctx->input_path);
        phase3_owned_root = cc__reparse_source_to_ast_ctx(ctx, src_ufcs, src_ufcs_len,
                                                          "phase3 entry (emit-ready AST sync)");
        if (!phase3_owned_root) {
            goto fail;
        }
        phase3_root = phase3_owned_root;
        cc__collect_registered_ufcs_var_types(ctx->symbols, src_ufcs, src_ufcs_len);
        /* Phase 3 lowering — two-stage batched pipeline.
         *
         * Stage 0 (text): if any `@blocking` / `@noblock` / `@nonblocking`
         * call-site or block markers are present, rewrite them in-buffer first so the stage-1 AST is
         * parsed against the marker-resolved form.  This is a pure text
         * transform and does not perturb byte offsets at the granularity
         * the AST passes care about.
         *
         * Stage 1 (UFCS): collects only `cc__collect_ufcs_edits` into the
         * edit buffer and applies them.  UFCS lowers `obj.method(...)` →
         * `method(obj, ...)`, *producing* new conventional call sites.
         * Later collectors are AST-driven against CALL nodes whose
         * `is_ufcs` flag has been cleared by lowering, so they need to see
         * the post-UFCS AST — hence the mandatory reparse barrier here.
         *
         * Stage 1.5 (slice literal coerce): wraps string literals at
         * CCSlice / char[:0] parameters.  Separate from Stage 2 so
         * arg-span edits never overlap autoblock whole-call replacements.
         *
         * Stage 2 (post-UFCS): collects closure_calls + autoblock +
         * await_normalize into a single edit buffer and applies them in
         * one shot.  These three passes target disjoint constructs
         * (closure-typed CALL nodes / blocking CALL nodes inside @async /
         * await expressions) and emit non-overlapping per-span edits, so
         * one collect+apply+reparse cycle covers all three.
         *
         * Net reparses in Phase 3: up to 3 (one per stage when each
         * produces changes; fewer when a stage emits no edits).  See
         * PIPELINE.md.
         */
        if (src_ufcs &&
            (cc_contains_token_top_level(src_ufcs, src_ufcs_len, "@blocking") ||
             cc_contains_token_top_level(src_ufcs, src_ufcs_len, "@noblock") ||
             cc_contains_token_top_level(src_ufcs, src_ufcs_len, "@nonblocking"))) {
            /* @blocking/@noblock/@nonblocking are lowered in canonicalize; phase-3 AST
             * passes may still see markers if a prior edit reintroduced them. */
            char* cs = cc__rewrite_at_call_site_mode(src_ufcs, src_ufcs_len);
            if (cs) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = cs;
                src_ufcs_len = strlen(cs);
            }
        }
        phase3_changed = 0;
        if (cc__apply_batched_phase3_passes(phase3_root, ctx, &src_ufcs, &src_ufcs_len,
                                            src_all, &phase3_changed,
                                            CC_PHASE3_STAGE_UFCS_ONLY) < 0) {
            if (phase3_owned_root) cc_tcc_bridge_free_ast(phase3_owned_root);
            goto fail;
        }
        if (phase3_changed) {
            if (phase3_owned_root) cc_tcc_bridge_free_ast(phase3_owned_root);
            phase3_owned_root = cc__reparse_source_to_ast_ctx(ctx, src_ufcs, src_ufcs_len,
                                                              "phase3 stage 1 (UFCS) reparse");
            if (!phase3_owned_root) {
                goto fail;
            }
            phase3_root = phase3_owned_root;
        }
        phase3_changed = 0;
        if (cc__apply_batched_phase3_passes(phase3_root, ctx, &src_ufcs, &src_ufcs_len,
                                            src_all, &phase3_changed,
                                            CC_PHASE3_STAGE_SLICE_LIT_COERCE) < 0) {
            if (phase3_owned_root) cc_tcc_bridge_free_ast(phase3_owned_root);
            goto fail;
        }
        if (phase3_changed) {
            if (phase3_owned_root) cc_tcc_bridge_free_ast(phase3_owned_root);
            phase3_owned_root = cc__reparse_source_to_ast_ctx(
                ctx, src_ufcs, src_ufcs_len, "phase3 stage 1.5 (slice lit coerce) reparse");
            if (!phase3_owned_root) {
                goto fail;
            }
            phase3_root = phase3_owned_root;
        }
        phase3_changed = 0;
        if (cc__apply_batched_phase3_passes(phase3_root, ctx, &src_ufcs, &src_ufcs_len,
                                            src_all, &phase3_changed,
                                            CC_PHASE3_STAGE_POST_UFCS) < 0) {
            if (phase3_owned_root) cc_tcc_bridge_free_ast(phase3_owned_root);
            goto fail;
        }
        if (phase3_changed) {
            if (phase3_owned_root) cc_tcc_bridge_free_ast(phase3_owned_root);
            phase3_owned_root = cc__reparse_source_to_ast_ctx(ctx, src_ufcs, src_ufcs_len,
                                                              "phase3 stage 2 (post-UFCS) reparse");
            if (!phase3_owned_root) {
                goto fail;
            }
            phase3_root = phase3_owned_root;
        } else if (phase3_owned_root) {
            cc_tcc_bridge_free_ast(phase3_owned_root);
            phase3_owned_root = NULL;
        }

        /* Debug output for await rewrite */
        if (getenv("CC_DEBUG_AWAIT_REWRITE") && src_ufcs) {
            const char* needle = "@async int f";
            size_t np = cc_find_substr_top_level(src_ufcs, 0, src_ufcs_len, needle, strlen(needle));
            if (np >= src_ufcs_len) np = cc_find_substr_top_level(src_ufcs, 0, src_ufcs_len, "@async", 6);
            if (np < src_ufcs_len) {
                fprintf(stderr, "CC_DEBUG_AWAIT_REWRITE: ---- snippet ----\n");
                size_t take = 800;
                if (np + take > src_ufcs_len) take = src_ufcs_len - np;
                fwrite(src_ufcs + np, 1, take, stderr);
                fprintf(stderr, "\nCC_DEBUG_AWAIT_REWRITE: ---- end ----\n");
            }
        }
    }
#endif

    /* send_task lowering before statement/async reparses so AST spans match text. */
    if (src_ufcs && ctx &&
        (cc_contains_token_top_level(src_ufcs, src_ufcs_len, "send_task") ||
         cc_contains_token_top_level(src_ufcs, src_ufcs_len, "cc_channel_send_task"))) {
        size_t st_len = 0;
        char* st = cc__rewrite_chan_send_task_text(ctx, src_ufcs, src_ufcs_len, &st_len);
        if (st) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = st;
            src_ufcs_len = st_len;
        }
    }

    /* Normalise the source so that the stage1 reparse (and the closure-lift
       pass that uses it) always operates on fully-lowered, position-stable
       text.  Both rewrites mutate potentially multi-line constructs into
       single-expression forms; doing them here ensures the AST node spans
       recorded by cc__rewrite_closure_literals_with_nodes remain in sync
       with the text buffer throughout the rest of the pipeline.
       Previously these ran AFTER closure lifting, which caused character
       positions to shift under already-computed AST offsets — producing
       truncated or misassigned closure bodies whenever the `!>` handler or
       `@destroy` body spanned more than one line. */

    /* 1. @destroy → @defer.  Track whether the rewrite fired; if it did,
          the source now contains a synthesized @defer block whose text
          displaces subsequent character positions — we must also lower
          !> / ?> before the AST reparse so the closure-lift pass sees a
          stable, already-normalised source. */
    int ud_fired = 0;
    if (src_ufcs && src_ufcs_len &&
        cc_contains_token_top_level(src_ufcs, src_ufcs_len, "@destroy") &&
        (cc_contains_token_top_level(src_ufcs, src_ufcs_len, "!>") ||
         cc_contains_token_top_level(src_ufcs, src_ufcs_len, "?>"))) {
        char* ud_out = NULL;
        size_t ud_out_len = 0;
        /* Hand the pass our symbol table so bodyless `@destroy;` on
         * user-declared types with `@comptime cc_type_register(...)`
         * destroy hooks resolves correctly.  Cleared after the call to
         * avoid leaking the table into unrelated later invocations. */
        cc_unwrap_destroy_set_symbols(ctx ? ctx->symbols : NULL);
        int ud_r = cc__rewrite_unwrap_destroy_suffix(
            src_ufcs, src_ufcs_len,
            ctx && ctx->input_path ? ctx->input_path : NULL,
            &ud_out, &ud_out_len);
        cc_unwrap_destroy_set_symbols(NULL);
        if (ud_r < 0) {
            goto fail;
        }
        if (ud_r > 0 && ud_out) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = ud_out;
            src_ufcs_len = ud_out_len;
            ud_fired = 1;
        }
    }
    /* 2. !> / ?> → inline expression.  Only needed here when step 1
          fired: the @defer insertion can shift char positions, so both
          passes must run together before the closure-AST reparse.  When
          step 1 did NOT fire there is no position disruption and the late
          text-pass call (after closure lifting) is sufficient — running it
          early in that case risks misrewriting the `T !>(E) name = expr`
          declaration form before the pointer-fn registry is populated. */
    if (ud_fired && ctx &&
        src_ufcs && src_ufcs_len &&
        (cc_contains_token_top_level(src_ufcs, src_ufcs_len, "!>") ||
         cc_contains_token_top_level(src_ufcs, src_ufcs_len, "?>"))) {
        char* ru_out = NULL;
        size_t ru_out_len = 0;
        if (cc__rewrite_result_unwrap(ctx, src_ufcs, src_ufcs_len, &ru_out, &ru_out_len) > 0 && ru_out) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = ru_out;
            src_ufcs_len = ru_out_len;
        }
    }

    /* Reparse the current TU source to get an up-to-date stub-AST for statement-level lowering.
       These rewrites run before marker stripping to keep spans stable.

       When both `=>` (closure literals) and `@async`/`await` are present, reuse
       one stub-AST for async lowering if closure lifting was a no-op AND @defer
       did not mutate the source in between (otherwise offsets/types are stale).

       Pre-check: closure lift is a no-op without `=>`. Gating the reparse on
       `=>` skips an unconditional reparse for ~70% of smoke TUs (2026-05-28). */
    {
        int have_closure_token = src_ufcs && src_ufcs_len && ctx && ctx->symbols &&
            cc_contains_token_top_level(src_ufcs, src_ufcs_len, "=>");
        int have_async_token = src_ufcs && ctx && ctx->symbols &&
            (cc_contains_token_top_level(src_ufcs, src_ufcs_len, "@async") ||
             cc_contains_token_top_level(src_ufcs, src_ufcs_len, "await"));
        CCASTRoot* stmt_async_root = NULL;

        if (have_closure_token) {
            cc__debug_dump_reparse_source("stage1_pre_stmt", src_ufcs, src_ufcs_len, ctx->input_path);
            stmt_async_root = cc__reparse_source_to_ast_ctx(ctx, src_ufcs, src_ufcs_len,
                                                            "statement-lowering input");
            if (!stmt_async_root) {
                goto fail;
            }

            char* rewritten = NULL;
            size_t rewritten_len = 0;
            char* protos = NULL;
            size_t protos_len = 0;
            char* defs = NULL;
            size_t defs_len = 0;
            int r = cc__rewrite_closure_literals_with_nodes(stmt_async_root, ctx, src_ufcs, src_ufcs_len,
                                                            &rewritten, &rewritten_len,
                                                            &protos, &protos_len,
                                                            &defs, &defs_len);
            if (r < 0) {
                cc_tcc_bridge_free_ast(stmt_async_root);
                goto fail;
            }
            if (r > 0) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = rewritten_len;
                cc_tcc_bridge_free_ast(stmt_async_root);
                stmt_async_root = NULL;
            } else {
                free(rewritten);
            }
            if (protos) { free(closure_protos); closure_protos = protos; closure_protos_len = protos_len; }
            if (defs) { free(closure_defs); closure_defs = defs; closure_defs_len = defs_len; }
        }

        /* Lower @defer (and hard-error on cancel) using a syntax-driven pass.
           IMPORTANT: this must run BEFORE async lowering so `@defer` can be made suspend-safe. */
        if (src_ufcs && (cc_contains_token_top_level(src_ufcs, src_ufcs_len, "@defer") ||
                         cc_contains_token_top_level(src_ufcs, src_ufcs_len, "cancel"))) {
            char* rewritten = NULL;
            size_t rewritten_len = 0;
            int r = cc__rewrite_defer_syntax_marked(ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
            if (r < 0) {
                if (stmt_async_root) cc_tcc_bridge_free_ast(stmt_async_root);
                goto fail;
            }
            if (r > 0) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = rewritten_len;
                if (stmt_async_root) {
                    cc_tcc_bridge_free_ast(stmt_async_root);
                    stmt_async_root = NULL;
                }
            }
        }

        /* AST-driven @async lowering (state machine).
           IMPORTANT: run after statement-level lowering so closure rewrites are already reflected. */
        if (have_async_token) {
            if (!stmt_async_root) {
                cc__debug_dump_reparse_source("stage2_pre_async", src_ufcs, src_ufcs_len, ctx->input_path);
                stmt_async_root = cc__reparse_source_to_ast_ctx(ctx, src_ufcs, src_ufcs_len,
                                                                "async-lowering input");
            }
            if (getenv("CC_DEBUG_REPARSE")) {
                fprintf(stderr, "CC: reparse: stub ast node_count=%d\n",
                        stmt_async_root ? stmt_async_root->node_count : -1);
            }
            if (!stmt_async_root) {
                goto fail;
            }

            char* rewritten = NULL;
            size_t rewritten_len = 0;
            int ar = cc_async_rewrite_state_machine_ast(stmt_async_root, ctx, src_ufcs, src_ufcs_len,
                                                        &rewritten, &rewritten_len);
            cc_tcc_bridge_free_ast(stmt_async_root);
            if (ar < 0) {
                goto fail;
            }
            if (ar > 0) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = rewritten_len;
            }
        } else if (stmt_async_root) {
            cc_tcc_bridge_free_ast(stmt_async_root);
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

    /* Last-chance UFCS normalization before writing C. Earlier AST/text passes
       can rewrite large source regions from parser snapshots and leave a
       member-call spelling behind. Do not let `recv.method(...)` survive to
       the host compiler when the receiver type is one of the registered
       parser-safe UFCS families. */
    if (src_ufcs && ctx) {
        char* rew = cc_rewrite_generic_family_ufcs_parser_safe(src_ufcs, src_ufcs_len);
        if (rew) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rew;
            src_ufcs_len = strlen(src_ufcs);
        }
    }

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

    /* Container monomorph macros are spliced later (after UFCS/closures) using
     * cc_emit_plan_build_container_schedule so decls land after payload typedefs
     * (e.g. ArrayMap::[K, Entry] when Entry follows a helper function). */
    {
        CCTypeRegistry* reg = cc_type_registry_get_global();
        if (reg) {
            size_t n_vec = cc_type_registry_vec_count(reg);
            size_t n_map = cc_type_registry_map_count(reg);
            need_container_decls = (n_map > 0);
            if (!need_container_decls) {
                for (size_t i = 0; i < n_vec; i++) {
                    const CCTypeInstantiation* inst = cc_type_registry_get_vec(reg, i);
                    if (!inst || !inst->mangled_name) continue;
                    if (strcmp(inst->mangled_name, "CCVec_char") != 0) {
                        need_container_decls = 1;
                        break;
                    }
                }
            }
            if (need_container_decls) {
                fprintf(out, "/* --- CC generic container declarations --- */\n");
                fprintf(out, "#include <ccc/std/vec.h>\n");
                fprintf(out, "#include <ccc/std/map.h>\n");
                fprintf(out, "#include <ccc/std/array_map.h>\n");
                fprintf(out, "#include <ccc/cc_channel.h>\n");
                /* Pulled in so the per-T `cc_type_info` symbol emissions
                 * below (the "container type info" block) compile.  See
                 * COMPILER_CLEANUP_STATUS.md milestone #4a / Commit 3a. */
                fprintf(out, "#include <ccc/cc_type.cch>\n");
                fprintf(out, "/* --- end container declarations (macros inserted after typedefs) --- */\n\n");
            }
        }
    }

    /* Result type declarations are emitted later from specs collected during canonicalize. */

    /* Preserve diagnostics mapping to the original input (repo-relative for readability). */
    {
        cc__emit_line_directive(out, 1, src_path);
    }

    if (src_ufcs) {
        cc__mirror_canonical_result_specs();
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
        /* Some later lowering stages can still synthesize raw `@defer ...;`
           forms. Normalize them again before the final UFCS reparse so
           expressions like `@defer arena.free();` don't reach strict UFCS
           dispatch with `@defer arena` as the apparent receiver. */
        if (cc_contains_token_top_level(src_ufcs, src_ufcs_len, "@defer") ||
            cc_contains_token_top_level(src_ufcs, src_ufcs_len, "cancel")) {
            char* rewritten = NULL;
            size_t rewritten_len = 0;
            int r = cc__rewrite_defer_syntax_marked(ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
            if (r < 0) {
                goto fail;
            }
            if (r > 0) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = rewritten_len;
            }
        }
        /* Splice generated closure definitions into src_ufcs BEFORE the final
         * UFCS sweep so the AST UFCS pass sees the lifted closure bodies as
         * part of the translation unit.  Historically closure_defs was
         * emitted as a side buffer and never went through AST UFCS, which
         * forced an always-on text fallback to clean up cases like
         *   chan.send_task(() => [blk] { blk->arena.free(); })
         * By merging here we make the AST UFCS pass authoritative. */
        if (closure_defs && closure_defs_len > 0) {
            cc__relower_closure_defs(ctx, &closure_defs, &closure_defs_len);

            /* The AST UFCS pass addresses rewrite targets by PHYSICAL line
             * number in the final src_ufcs buffer.  closure_defs contains
             * `#line <userN> "pigz.ccs"` directives (so user-level diagnostics
             * still point to the source `() => { ... }` expression) which, if
             * left intact, cause TCC to report UFCS calls at user lines
             * (e.g. 210) that already correspond to unrelated physical text
             * earlier in src_ufcs.  Strip those remapping directives so the
             * closure section's TCC line numbers match physical line numbers
             * in the merged buffer.  User-line diagnostics for code inside
             * lifted closure bodies are handled at spawn-time by the closure
             * literal pass anchoring the top-level `#line` before the call. */
            size_t closure_defs_stripped_len = 0;
            char* closure_defs_stripped = cc__mask_line_directives(
                closure_defs, closure_defs_len, &closure_defs_stripped_len);
            const char* hdr_protos_open  = "\n/* --- CC closure declarations --- */\n";
            const char* hdr_protos_close = "/* --- end closure declarations --- */\n";
            const char* hdr_defs_open    = "\n/* --- CC generated closures --- */\n";
            int has_protos = (closure_protos && closure_protos_len > 0);
            const char* defs_buf = closure_defs_stripped ? closure_defs_stripped : closure_defs;
            size_t defs_buf_len  = closure_defs_stripped ? closure_defs_stripped_len : closure_defs_len;
            /* M-closure refactor: place file-scope forward decls
             * (`closure_protos`) BEFORE the first function definition,
             * not at the very end.  The closure pass emits *only*
             * file-scope forward decls into `closure_protos` (the legacy
             * in-source walker was deleted 2026-05-28), so the only
             * declaration the call site sees comes from this block.
             * If we leave it appended at end (after `main()`), the call
             * site inside `main()` has no visible declaration of
             * `__cc_closure_make_N` and TCC fails to parse.
             *
             * Critically, "before first function definition" — not "after
             * last #include" — is the correct insertion point: closure
             * capture types may be user `typedef`s declared between the
             * last `#include` and the first function (see
             * tests/redis_owner_reply_try_send_hol_smoke.ccs which
             * declares `HolReqTx` / `HolReqRx` after #includes but before
             * `static void hol_owner_blocking_policy(HolReqRx ...)`).
             * Inserting after `#include`s would put protos that reference
             * `HolReqRx` before its typedef.
             *
             * Definitions (`closure_defs`) continue to be appended at the
             * end of the buffer: they reference user types via `__typeof__`
             * that are only available in the user code's scope, and the
             * #line directive below keeps user-line diagnostics anchored
             * for code inside lifted closure bodies. */
            size_t protos_insert_off = has_protos
                ? cc_find_first_func_def_offset(src_ufcs, src_ufcs_len)
                : 0;
            if (has_protos && protos_insert_off >= src_ufcs_len) {
                /* No top-level function definition (header-only-style TU).
                 * Fall back to after-#includes; correct for files with no
                 * user typedefs between includes and the closure call site. */
                protos_insert_off = cc_find_protos_insertion_point(src_ufcs, src_ufcs_len);
            }
            /* Resync marker after the protos block: the block inserts lines
             * with no #line accounting, shifting every host-compiler
             * diagnostic below it (oracle: diag_oracle_closure_fail).  The
             * marker is masked (see cc__mask_line_directives) so passes
             * still see a directive-free buffer; the write-time unmask
             * turns it into a real `#line <K> "<input>"`. */
            char protos_resync[4160];
            protos_resync[0] = '\0';
            if (has_protos) {
                /* Ledger-aware user line (honors #line/CC_LN entries above
                 * the insertion point; a raw newline count runs high below
                 * upstream expansions like @grammar). */
                const char* lp = NULL;
                size_t lpl = 0;
                int user_line = cc_user_line_for_offset(src_ufcs, src_ufcs_len,
                                                        protos_insert_off, 1, &lp, &lpl);
                if (lp && lpl > 0 && lpl < 1024) {
                    snprintf(protos_resync, sizeof(protos_resync),
                             "/*CC_LN %d %.*s*/\n", user_line, (int)lpl, lp);
                } else {
                    snprintf(protos_resync, sizeof(protos_resync),
                             "/*CC_LN %d %s*/\n", user_line, src_path);
                }
            }
            size_t add = strlen(hdr_defs_open) + defs_buf_len + 80 + 1;
            if (has_protos) add += strlen(hdr_protos_open) + closure_protos_len +
                                   strlen(hdr_protos_close) + strlen(protos_resync);
            char* merged = (char*)malloc(src_ufcs_len + add + 1);
            if (merged) {
                size_t pos = 0;
                /* [src_ufcs[0..insert_off)] */
                if (has_protos) {
                    memcpy(merged + pos, src_ufcs, protos_insert_off);
                    pos += protos_insert_off;
                    size_t l = strlen(hdr_protos_open);
                    memcpy(merged + pos, hdr_protos_open, l); pos += l;
                    memcpy(merged + pos, closure_protos, closure_protos_len); pos += closure_protos_len;
                    l = strlen(hdr_protos_close);
                    memcpy(merged + pos, hdr_protos_close, l); pos += l;
                    l = strlen(protos_resync);
                    memcpy(merged + pos, protos_resync, l); pos += l;
                    /* [src_ufcs[insert_off..end)] */
                    memcpy(merged + pos, src_ufcs + protos_insert_off,
                           src_ufcs_len - protos_insert_off);
                    pos += src_ufcs_len - protos_insert_off;
                } else {
                    memcpy(merged + pos, src_ufcs, src_ufcs_len); pos += src_ufcs_len;
                }
                /* Reset TCC's logical-line counter so physical-line == reported-
                 * line throughout the closure section.  The reset goes BEFORE
                 * the section header comment: those header lines are generated
                 * glue, and leaving them under the preceding user-file mapping
                 * would run the mapped coordinate past the source's EOF. */
                {
                    if (pos > 0 && merged[pos - 1] != '\n') merged[pos++] = '\n';
                    int phys_line = 1;
                    for (size_t k = 0; k < pos; k++) if (merged[k] == '\n') phys_line++;
                    int n = snprintf(merged + pos, 64, "#line %d \"<cc-closures>\"\n", phys_line + 1);
                    if (n > 0) pos += (size_t)n;
                }
                {
                    size_t l = strlen(hdr_defs_open);
                    memcpy(merged + pos, hdr_defs_open, l); pos += l;
                }
                memcpy(merged + pos, defs_buf, defs_buf_len); pos += defs_buf_len;
                merged[pos++] = '\n';
                merged[pos] = '\0';
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = merged;
                src_ufcs_len = pos;
                /* Ownership of both closure_protos and closure_defs transferred
                 * into src_ufcs; zero them so the legacy end-of-TU emission
                 * paths become no-ops. */
                free(closure_protos); closure_protos = NULL; closure_protos_len = 0;
                free(closure_defs);   closure_defs   = NULL; closure_defs_len   = 0;
            }
            if (closure_defs_stripped) free(closure_defs_stripped);
        }

        {
            char* rew_chan = cc__rewrite_channel_ufcs_text_late(src_ufcs, src_ufcs_len);
            if (rew_chan) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_chan;
                src_ufcs_len = strlen(src_ufcs);
            }
        }

        /* Final UFCS sweep: statement-level rewrites can synthesize new
           method-call surface syntax (defer/spawn/nursery/async).  Skip the
           reparse entirely when no member-call surface remains — the collect
           step would produce zero edits but the reparse still costs a full
           TCC header parse (~10ms). */
        if (ctx && ctx->symbols && cc__has_member_call_candidate(src_ufcs, src_ufcs_len)) {
            CCTypeRegistry* saved_reg = cc_type_registry_get_global();
            CCTypeRegistry* temp_reg = cc_type_registry_new();
            cc__debug_dump_reparse_source("stage3_pre_final_ufcs", src_ufcs, src_ufcs_len, ctx->input_path);
            CCASTRoot* final_ufcs_root = cc__reparse_source_to_ast_ctx(ctx, src_ufcs, src_ufcs_len,
                                                                       "final-UFCS input");
            if (!final_ufcs_root) {
                if (temp_reg) cc_type_registry_free(temp_reg);
                goto fail;
            }

            if (temp_reg) cc_type_registry_set_global(temp_reg);
            cc__collect_registered_ufcs_var_types(ctx->symbols, src_ufcs, src_ufcs_len);
            cc__seed_map_ufcs_from_type_graph(ctx->symbols);
            CCEditBuffer eb;
            cc_edit_buffer_init(&eb, src_ufcs, src_ufcs_len);
            if (cc__collect_ufcs_edits(final_ufcs_root, ctx, &eb) < 0) {
                cc_tcc_bridge_free_ast(final_ufcs_root);
                if (temp_reg) {
                    cc_type_registry_set_global(saved_reg);
                    cc_type_registry_free(temp_reg);
                }
                goto fail;
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

        /* Lower @err / @errhandler / <? / =<! ... @err for host C emission
           after the last TCC-backed UFCS reparse.  This keeps synthetic
           unwrap blocks out of reparse inputs while still ensuring the final
           emitted C has no result/err surface syntax. */
        if (src_ufcs && src_ufcs_len &&
            (cc_contains_token_top_level(src_ufcs, src_ufcs_len, "?>") ||
             cc_contains_token_top_level(src_ufcs, src_ufcs_len, "!>"))) {
            char* ru_out = NULL;
            size_t ru_out_len = 0;
            int ru_r = cc__rewrite_result_unwrap(ctx, src_ufcs, src_ufcs_len, &ru_out, &ru_out_len);
            if (ru_r < 0) {
                goto fail;
            }
            if (ru_r > 0 && ru_out) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = ru_out;
                src_ufcs_len = ru_out_len;
            }
        }
        if (src_ufcs && src_ufcs_len &&
            (cc_contains_token_top_level(src_ufcs, src_ufcs_len, "@errhandler") ||
             cc_contains_token_top_level(src_ufcs, src_ufcs_len, "@err") ||
             cc_contains_token_top_level(src_ufcs, src_ufcs_len, "=<!") ||
             cc_contains_token_top_level(src_ufcs, src_ufcs_len, "<?"))) {
            char* err_out = NULL;
            size_t err_out_len = 0;
            int err_r = cc__rewrite_err_syntax(ctx, src_ufcs, src_ufcs_len, &err_out, &err_out_len);
            if (err_r < 0) {
                goto fail;
            }
            if (err_r > 0 && err_out) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = err_out;
                src_ufcs_len = err_out_len;
            }
        }
        if (src_ufcs && ctx) {
            char* rew = cc_rewrite_generic_family_ufcs_parser_safe(src_ufcs, src_ufcs_len);
            if (rew) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew;
                src_ufcs_len = strlen(src_ufcs);
            }
        }
        {
            char* rew_chan = cc__rewrite_channel_ufcs_text_late(src_ufcs, src_ufcs_len);
            if (rew_chan) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_chan;
                src_ufcs_len = strlen(src_ufcs);
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
        /* End-of-pipeline placeholder sweep removed (Phase 3.3): the AST
           UFCS dispatch now canonicalizes `__CC_VEC(T)` / `__CC_MAP(K,V)`
           receiver types to `CCVec_<T>` / `Map_<K>_<V>` directly (see
           cc__ufcs_canonicalize_family_macro in visitor/ufcs.c), so
           `__cc_vec_generic_*` / `__cc_map_generic_*` placeholders are
           no longer emitted into src_ufcs. */
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
           Find the first CCResult_ usage and insert before that line (at file scope).

           Also: if the unified unwrap primitives appear in the lowered source
           (meaning a `!>` / `?>` / `@err` lowered to `__cc_uw_*`), seed the
           TU spec table with the stdlib-predeclared result specs so the
           `_Generic` enumeration below covers result types introduced via
           UFCS-expanded stdlib macros (e.g. `tx.send(x)` expanding to a
           `CCResult_bool_CCIoError`-valued stmt-expr).  Without this seeding
           the parser-mode default arm wins and binder types collapse to
           `__CCGenericError`.  See docs/known-bugs/redis_idiomatic_async.md
           [F8]. */
        int uses_uw_primitives =
            (cc_find_ident_top_level(src_ufcs, 0, src_ufcs_len,
                                     "__cc_uw_is_err", 14) < src_ufcs_len) ||
            (cc_find_ident_top_level(src_ufcs, 0, src_ufcs_len,
                                     "__cc_uw_value", 13) < src_ufcs_len) ||
            (cc_find_ident_top_level(src_ufcs, 0, src_ufcs_len,
                                     "__cc_uw_err_at", 14) < src_ufcs_len);
        if (uses_uw_primitives) {
            int pi;
            if (getenv("CC_DEBUG_UW_SEED")) fprintf(stderr, "uw_seed: specs_before=%zu\n", cc__cg_result_specs.count);
            for (pi = 0; ; pi++) {
                const CCStdlibPredeclaredResult* pre =
                    cc_result_spec_lookup_stdlib_predeclared_by_index(pi);
                if (!pre) break;
                if (cc_result_spec_table_find_by_name(&cc__cg_result_specs,
                                                      pre->concrete_name)) continue;
                /* Only seed a stdlib predeclared spec when the TU actually
                 * references either the `CCResult_T_E` concrete type or its
                 * ok-type / err-type.  Otherwise the `_Generic` arm below
                 * would name a typedef whose defining header `ccc/std/X.cch`
                 * is not included by this TU, and the real-compile reparse
                 * would fail with `unknown type name 'CCResult_...'`. */
                size_t cname_len = strlen(pre->concrete_name);
                int referenced =
                    (cc_find_ident_top_level(src_ufcs, 0, src_ufcs_len,
                                              pre->concrete_name, cname_len) < src_ufcs_len);
                if (!referenced && pre->ok_type) {
                    size_t okt_len = strlen(pre->ok_type);
                    /* Strip pointer/array suffixes so we look up the bare
                     * type name (e.g. `CCDirIter*` -> `CCDirIter`). */
                    while (okt_len > 0 && (pre->ok_type[okt_len - 1] == '*' ||
                                            pre->ok_type[okt_len - 1] == ' ' ||
                                            pre->ok_type[okt_len - 1] == '\t')) {
                        okt_len--;
                    }
                    if (okt_len > 0 &&
                        cc_find_ident_top_level(src_ufcs, 0, src_ufcs_len,
                                                 pre->ok_type, okt_len) < src_ufcs_len) {
                        referenced = 1;
                    }
                }
                if (!referenced) {
                    if (getenv("CC_DEBUG_UW_SEED"))
                        fprintf(stderr, "uw_seed: skipped %s (not referenced)\n", pre->concrete_name);
                    continue;
                }
                (void)cc_result_spec_table_add(&cc__cg_result_specs,
                                               pre->ok_type, strlen(pre->ok_type),
                                               pre->err_type, strlen(pre->err_type),
                                               pre->mangled_ok, pre->mangled_err);
                if (getenv("CC_DEBUG_UW_SEED")) fprintf(stderr, "uw_seed: added %s\n", pre->concrete_name);
            }
            if (getenv("CC_DEBUG_UW_SEED")) fprintf(stderr, "uw_seed: specs_after=%zu\n", cc__cg_result_specs.count);
        } else if (getenv("CC_DEBUG_UW_SEED")) {
            fprintf(stderr, "uw_seed: no uw primitives detected\n");
        }
        if (cc__cg_result_specs.count > 0) {
            /* Find first usage of any CCResult_T_E type in the source.
             * Comment/string-aware so a type name that appears only in
             * a doc comment (e.g. `// example: CCResult_int_CCError f`)
             * doesn't anchor the typedef insertion point to the comment. */
            size_t earliest_pos = src_ufcs_len;
            for (size_t ri = 0; ri < cc__cg_result_specs.count; ri++) {
                const CCResultSpec* spec = cc_result_spec_table_get(&cc__cg_result_specs, ri);
                const char* pattern = spec ? spec->concrete_name : NULL;
                if (!pattern || !pattern[0]) continue;
                size_t pos = cc_find_ident_top_level(src_ufcs, 0, src_ufcs_len,
                                                    pattern, strlen(pattern));
                if (pos < src_ufcs_len && pos < earliest_pos) {
                    earliest_pos = pos;
                }
            }

            /* Fallback insertion point: if no `CCResult_T_E` name appears
             * literally in the source (e.g. the only Result usage is via
             * UFCS-expanded stdlib macros), insert the `_Generic`
             * enumeration just after the last top-level `#include` so the
             * stdlib struct definitions are visible.  Without this
             * fallback the `#undef __cc_uw_*` block is silently skipped
             * and the parser-mode default arm wins at TCC compile. */
            if (earliest_pos >= src_ufcs_len && uses_uw_primitives) {
                size_t last_include_end = 0;
                size_t k = 0;
                int in_lc = 0, in_bc = 0, in_s = 0, in_c = 0;
                while (k < src_ufcs_len) {
                    char ch = src_ufcs[k];
                    char ch2 = (k + 1 < src_ufcs_len) ? src_ufcs[k + 1] : 0;
                    if (in_lc) { if (ch == '\n') in_lc = 0; k++; continue; }
                    if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; k += 2; continue; } k++; continue; }
                    if (in_s)  { if (ch == '\\' && k + 1 < src_ufcs_len) { k += 2; continue; } if (ch == '"') in_s = 0; k++; continue; }
                    if (in_c)  { if (ch == '\\' && k + 1 < src_ufcs_len) { k += 2; continue; } if (ch == '\'') in_c = 0; k++; continue; }
                    if (ch == '/' && ch2 == '/') { in_lc = 1; k += 2; continue; }
                    if (ch == '/' && ch2 == '*') { in_bc = 1; k += 2; continue; }
                    if (ch == '"')  { in_s = 1; k++; continue; }
                    if (ch == '\'') { in_c = 1; k++; continue; }
                    if (ch == '#') {
                        size_t p = k + 1;
                        while (p < src_ufcs_len && (src_ufcs[p] == ' ' || src_ufcs[p] == '\t')) p++;
                        if (p + 7 <= src_ufcs_len && memcmp(src_ufcs + p, "include", 7) == 0) {
                            size_t eol = p + 7;
                            while (eol < src_ufcs_len && src_ufcs[eol] != '\n') eol++;
                            if (eol < src_ufcs_len) eol++;
                            last_include_end = eol;
                            k = eol;
                            continue;
                        }
                    }
                    k++;
                }
                if (last_include_end > 0 && last_include_end < src_ufcs_len) {
                    earliest_pos = last_include_end;
                }
            }

            if (earliest_pos < src_ufcs_len) {
                /* Back up to start of line */
                while (earliest_pos > 0 && src_ufcs[earliest_pos - 1] != '\n') {
                    earliest_pos--;
                }
                unsigned char delayed_result_specs[512] = {0};
                size_t delayed_result_pos[512];
                for (size_t ri = 0; ri < sizeof(delayed_result_pos) / sizeof(delayed_result_pos[0]); ri++) {
                    delayed_result_pos[ri] = src_ufcs_len + 1;
                }
                for (size_t ri = 0; ri < cc__cg_result_specs.count && ri < sizeof(delayed_result_specs); ri++) {
                    const CCResultSpec* spec = cc_result_spec_table_get(&cc__cg_result_specs, ri);
                    size_t ok_decl_end = spec ? cc_emit_plan_type_decl_end_top_level(src_ufcs, src_ufcs_len, spec->ok_type) : 0;
                    size_t err_decl_end = spec ? cc_emit_plan_type_decl_end_top_level(src_ufcs, src_ufcs_len, spec->err_type) : 0;
                    size_t decl_end = ok_decl_end > err_decl_end ? ok_decl_end : err_decl_end;
                    if (!spec || decl_end <= earliest_pos) continue;
                    size_t first_use = cc_find_ident_top_level(src_ufcs, decl_end, src_ufcs_len,
                                                               spec->concrete_name,
                                                               strlen(spec->concrete_name));
                    delayed_result_specs[ri] = 1;
                    if (first_use < src_ufcs_len) {
                        delayed_result_pos[ri] = cc_emit_plan_line_start_before(src_ufcs, first_use);
                    } else {
                        delayed_result_pos[ri] = decl_end;
                    }
                }
                
                /* Build declaration string */
                char* decls = NULL;
                size_t decls_len = 0, decls_cap = 0;
                cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap,
                    "/* --- CC result type declarations (auto-generated) --- */\n");
                /* No longer gated on `CC_PARSER_MODE`.  The typed struct and
                 * enumerated `_Generic` arms below must be active in the
                 * *final* lowered compile so `__cc_uw_value(r)` returns the
                 * declared `OkType` rather than `intptr_t` — that `intptr_t`
                 * collapse from the old `__CCResultGeneric` arm was the root
                 * of the "have 'long' and 'struct T'" type-mismatch hits on
                 * struct payloads in the `?>(e)` ternary lowering (see
                 * docs/known-bugs/redis_idiomatic_async.md, follow-up
                 * "parser-mode result-type collapse"). */
                for (size_t ri = 0; ri < cc__cg_result_specs.count; ri++) {
                    const CCResultSpec* spec = cc_result_spec_table_get(&cc__cg_result_specs, ri);
                    char line[512];
                    if (ri < sizeof(delayed_result_specs) && delayed_result_specs[ri]) continue;
                    if (!spec) continue;
                    /* Stdlib-predeclared types already have `CC_DECL_RESULT_SPEC`
                     * expanded by a header (unguarded), so re-emitting would
                     * duplicate `_unwrap`/`_unwrap_err`/etc. static-inline
                     * definitions.  We still keep these specs in
                     * cc__cg_result_specs so the `_Generic` enumeration below
                     * picks them up — we just skip the struct decl here. */
                    if (cc_result_spec_is_stdlib_predeclared_name(spec->concrete_name)) continue;
                    cc_result_spec_emit_decl(spec, line, sizeof(line));
                    cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap, line);
                }

                /* Unified unwrap primitives for real compile mode.  The
                 * only per-TU information is the roster of concrete Result
                 * types, so emit it once as an X-macro list (split into
                 * value-ok and void-ok, whose `value` arms differ) and
                 * derive the three `__cc_uw_*` `_Generic` dispatchers from
                 * it; the `default:` arm covers raw-pointer LHSs.  These
                 * mirror the parser-mode definitions in cc_result.cch so
                 * the lowering pass can emit the same call shape for both
                 * Result-struct and pointer LHSs without scanning source
                 * text to guess the LHS type.  Every `err_at` arm
                 * side-effects on R3's unwrap chain (comma operator)
                 * before extracting the error — same shape as the baseline
                 * macro in cc_result.cch, so the record() call fires for
                 * Result-struct LHSs too, not just the raw-pointer
                 * default. */
                cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap,
                    "/* Unified unwrap dispatch (real mode).  This TU's Result types as an\n"
                    " * X-macro roster; the __cc_uw_* dispatchers derive their _Generic arms\n"
                    " * from it (default: arm = raw-pointer LHS). */\n");
                cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap,
                    "#undef __CC_UW_RESULT_TYPES\n"
                    "#define __CC_UW_RESULT_TYPES(X, ...)");
                for (size_t ri = 0; ri < cc__cg_result_specs.count; ri++) {
                    const CCResultSpec* spec = cc_result_spec_table_get(&cc__cg_result_specs, ri);
                    char line[320];
                    if (ri < sizeof(delayed_result_specs) && delayed_result_specs[ri]) continue;
                    if (!spec || strcmp(spec->ok_type, "void") == 0) continue;
                    snprintf(line, sizeof(line), " \\\n    X(%s, __VA_ARGS__)", spec->concrete_name);
                    cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap, line);
                }
                cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap,
                    "\n#undef __CC_UW_RESULT_TYPES_VOIDOK\n"
                    "#define __CC_UW_RESULT_TYPES_VOIDOK(X, ...)");
                for (size_t ri = 0; ri < cc__cg_result_specs.count; ri++) {
                    const CCResultSpec* spec = cc_result_spec_table_get(&cc__cg_result_specs, ri);
                    char line[320];
                    if (ri < sizeof(delayed_result_specs) && delayed_result_specs[ri]) continue;
                    if (!spec || strcmp(spec->ok_type, "void") != 0) continue;
                    snprintf(line, sizeof(line), " \\\n    X(%s, __VA_ARGS__)", spec->concrete_name);
                    cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap, line);
                }
                cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap,
                    "\n"
                    "#undef __cc_uw_arm_is_err\n"
                    "#define __cc_uw_arm_is_err(T, x) T: (!((T*)(void*)&(x))->ok),\n"
                    "#undef __cc_uw_arm_value\n"
                    "#define __cc_uw_arm_value(T, x) T: ((T*)(void*)&(x))->u.value,\n"
                    "#undef __cc_uw_arm_value_voidok\n"
                    "#define __cc_uw_arm_value_voidok(T, x) T: ((void)0),\n"
                    "#undef __cc_uw_arm_err_at\n"
                    "#define __cc_uw_arm_err_at(T, x, f, l) T: (cc_rt_diag_record_unwrap_site(f, l), ((T*)(void*)&(x))->u.error),\n"
                    "#undef __cc_uw_is_err\n"
                    "#define __cc_uw_is_err(__x__) _Generic((__x__), \\\n"
                    "    __CC_UW_RESULT_TYPES(__cc_uw_arm_is_err, __x__) \\\n"
                    "    __CC_UW_RESULT_TYPES_VOIDOK(__cc_uw_arm_is_err, __x__) \\\n"
                    "    default: (*(void* const*)(void*)&(__x__) == (void*)0))\n"
                    "#undef __cc_uw_value\n"
                    "#define __cc_uw_value(__x__) _Generic((__x__), \\\n"
                    "    __CC_UW_RESULT_TYPES(__cc_uw_arm_value, __x__) \\\n"
                    "    __CC_UW_RESULT_TYPES_VOIDOK(__cc_uw_arm_value_voidok, __x__) \\\n"
                    "    default: (__x__))\n"
                    "#undef __cc_uw_err_at\n"
                    "#define __cc_uw_err_at(__x__, __e__, __f__, __l__) _Generic((__x__), \\\n"
                    "    __CC_UW_RESULT_TYPES(__cc_uw_arm_err_at, __x__, __f__, __l__) \\\n"
                    "    __CC_UW_RESULT_TYPES_VOIDOK(__cc_uw_arm_err_at, __x__, __f__, __l__) \\\n"
                    "    default: (cc_rt_diag_record_unwrap_site(__f__, __l__), __cc_err_null_at(__e__, __f__, __l__)))\n");

                cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap,
                    "/* --- end result type declarations --- */\n\n");
                
                /* Build new source: prefix + decls + suffix */
                char* new_src = NULL;
                size_t new_len = 0, new_cap = 0;
                cc__sb_append_local(&new_src, &new_len, &new_cap, src_ufcs, earliest_pos);
                cc__sb_append_local(&new_src, &new_len, &new_cap, decls, decls_len);
                {
                    size_t delayed_cursor = earliest_pos;
                    for (;;) {
                        size_t next_pos = src_ufcs_len + 1;
                        for (size_t ri = 0; ri < cc__cg_result_specs.count && ri < sizeof(delayed_result_specs); ri++) {
                            if (!delayed_result_specs[ri]) continue;
                            if (delayed_result_pos[ri] > delayed_cursor && delayed_result_pos[ri] < next_pos) {
                                next_pos = delayed_result_pos[ri];
                            }
                        }
                        if (next_pos > src_ufcs_len) break;

                        cc__sb_append_local(&new_src, &new_len, &new_cap,
                                            src_ufcs + delayed_cursor, next_pos - delayed_cursor);
                        cc__sb_append_cstr_local(&new_src, &new_len, &new_cap,
                            "/* --- CC delayed result type declarations (after local typedefs) --- */\n");
                        for (size_t ri = 0; ri < cc__cg_result_specs.count && ri < sizeof(delayed_result_specs); ri++) {
                            const CCResultSpec* spec = cc_result_spec_table_get(&cc__cg_result_specs, ri);
                            char line[512];
                            if (!delayed_result_specs[ri] || delayed_result_pos[ri] != next_pos || !spec) continue;
                            if (cc_result_spec_is_stdlib_predeclared_name(spec->concrete_name)) continue;
                            cc_result_spec_emit_decl(spec, line, sizeof(line));
                            cc__sb_append_cstr_local(&new_src, &new_len, &new_cap, line);
                        }
                        cc__sb_append_cstr_local(&new_src, &new_len, &new_cap,
                            "/* --- end delayed result type declarations --- */\n\n");
                        delayed_cursor = next_pos;
                    }
                    cc__sb_append_local(&new_src, &new_len, &new_cap,
                                        src_ufcs + delayed_cursor, src_ufcs_len - delayed_cursor);
                }
                
                free(decls);
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = new_src;
                src_ufcs_len = new_len;
            }
        }

        {
            char* rewritten = NULL;
            CCTypeRegistryScope reg_scope;
            int reg_pushed = cc_type_registry_scope_push(&reg_scope);
            if (reg_pushed && ctx && ctx->symbols) {
                cc__collect_registered_ufcs_var_types(ctx->symbols, src_ufcs, src_ufcs_len);
            }
            rewritten = cc__rewrite_parser_placeholder_ufcs_lowers(src_ufcs, src_ufcs_len);
            if (reg_pushed) cc_type_registry_scope_pop(&reg_scope);
            if (rewritten) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = strlen(rewritten);
            }
        }
        
        /* Splice Vec/Map/ArrayMap monomorphs and typed-slice instances
         * using the emit-plan schedule so decls whose payload types are
         * declared after the early anchor (e.g. after a helper fn /
         * closure protos) land after those typedefs. */
        if (need_container_decls || cc_slice_spec_count() > 0) {
            CCEmitPlanContainerSchedule sched;
            CCTypeGraph* graph = cc_type_graph_get_global();
            CCCtnrInsert inserts[CC_EMIT_PLAN_MAX_DELAYED];
            size_t n_ins = 0;
            size_t i;
            memset(inserts, 0, sizeof(inserts));
            cc_emit_plan_build_container_schedule(src_ufcs, src_ufcs_len, graph, &sched);
            for (i = 0; i < sched.n_vec && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
                const CCTypeInstantiation* inst = cc_type_graph_get_vec(graph, i);
                char* piece = NULL;
                size_t piece_len = 0, piece_cap = 0;
                size_t pos;
                if (!inst || !inst->mangled_name) continue;
                if (strcmp(inst->mangled_name + 6, "char") == 0) continue;
                cc__format_vec_container_decl(&piece, &piece_len, &piece_cap, inst);
                if (!piece || piece_len == 0) { free(piece); continue; }
                pos = sched.vec_delayed[i] ? sched.vec_pos[i] : sched.anchor_pos;
                cc__ctnr_insert_append(inserts, &n_ins, CC_EMIT_PLAN_MAX_DELAYED,
                                       pos, piece, piece_len);
                free(piece);
            }
            for (i = 0; i < sched.n_map && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
                const CCTypeInstantiation* inst = cc_type_graph_get_map(graph, i);
                char* piece = NULL;
                size_t piece_len = 0, piece_cap = 0;
                size_t pos;
                if (!inst || !inst->mangled_name) continue;
                cc__format_map_container_decl(&piece, &piece_len, &piece_cap, inst);
                if (!piece || piece_len == 0) { free(piece); continue; }
                pos = sched.map_delayed[i] ? sched.map_pos[i] : sched.anchor_pos;
                cc__ctnr_insert_append(inserts, &n_ins, CC_EMIT_PLAN_MAX_DELAYED,
                                       pos, piece, piece_len);
                free(piece);
            }
            /* Typed slice instances the source names but no declaration
             * covers: `Pt[:]` auto-instantiates its CC_DECL_SLICE_SPEC
             * exactly like a Vec/Map monomorph. The expansion carries a
             * `#line` to its first use site so an error inside it (e.g.
             * undeclared element type) points at the line that named the
             * instance; the splice loop's resync restores mapping after. */
            for (i = 0; i < sched.n_slice && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
                const char* nm = NULL;
                const char* el = NULL;
                char piece[1024];
                int wrote;
                size_t pos;
                size_t fu;
                int fu_line = 0;
                char fu_file[512] = {0};
                if (!sched.slice_emit[i]) continue;
                if (cc_slice_spec_get(i, &nm, &el) != 0) continue;
                fu = cc_emit_plan_find_ident_any(src_ufcs, src_ufcs_len, nm);
                if (fu < src_ufcs_len)
                    cc__ctnr_insert_resync(src_ufcs, fu, &fu_line, fu_file,
                                           sizeof(fu_file));
                if (fu_line > 0 && fu_file[0])
                    wrote = snprintf(piece, sizeof(piece),
                                     "#line %d \"%s\"\nCC_DECL_SLICE_SPEC(%s, %s)\n",
                                     fu_line, fu_file, nm, el);
                else
                    wrote = snprintf(piece, sizeof(piece),
                                     "CC_DECL_SLICE_SPEC(%s, %s)\n", nm, el);
                if (wrote <= 0 || (size_t)wrote >= sizeof(piece)) continue;
                pos = sched.slice_delayed[i] ? sched.slice_pos[i] : sched.anchor_pos;
                cc__ctnr_insert_append(inserts, &n_ins, CC_EMIT_PLAN_MAX_DELAYED,
                                       pos, piece, (size_t)wrote);
            }
            /* Highest offset first so earlier splice points stay stable. */
            for (i = 0; i + 1 < n_ins; i++) {
                size_t j;
                for (j = i + 1; j < n_ins; j++) {
                    if (inserts[j].pos > inserts[i].pos) {
                        CCCtnrInsert tmp = inserts[i];
                        inserts[i] = inserts[j];
                        inserts[j] = tmp;
                    }
                }
            }
            for (i = 0; i < n_ins; i++) {
                char* new_src = NULL;
                size_t new_len = 0, new_cap = 0;
                int resync_line = 0;
                char resync_file[512] = {0};
                size_t pos = inserts[i].pos;
                if (pos > src_ufcs_len) pos = src_ufcs_len;
                cc__ctnr_insert_resync(src_ufcs, pos, &resync_line, resync_file, sizeof(resync_file));
                cc__sb_append_local(&new_src, &new_len, &new_cap, src_ufcs, pos);
                cc__sb_append_cstr_local(&new_src, &new_len, &new_cap,
                    "/* --- CC container type macros (auto-positioned after typedefs) --- */\n");
                cc__sb_append_local(&new_src, &new_len, &new_cap,
                                    inserts[i].text, inserts[i].text_len);
                cc__sb_append_cstr_local(&new_src, &new_len, &new_cap,
                    "/* --- end container type macros --- */\n");
                if (resync_line > 0 && resync_file[0]) {
                    char line_dir[640];
                    snprintf(line_dir, sizeof(line_dir), "#line %d \"%s\"\n",
                             resync_line, resync_file);
                    cc__sb_append_cstr_local(&new_src, &new_len, &new_cap, line_dir);
                }
                cc__sb_append_local(&new_src, &new_len, &new_cap,
                                    src_ufcs + pos, src_ufcs_len - pos);
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = new_src;
                src_ufcs_len = new_len;
                free(inserts[i].text);
                inserts[i].text = NULL;
            }
        }

        if (cc_emit_plan_splice_comptime_fragments(&src_ufcs, &src_ufcs_len, ctx->input_path) != 0) {
            goto fail;
        }

        /* WRITE-TIME UNMASK: restore `#line` directives from the masked
         * `CC_LN` markers (closure-defs anchors + protos-block resync) —
         * passes needed a directive-free buffer, diagnostics need the
         * directives.  Line counts are unchanged by construction. */
        {
            size_t um_len = 0;
            char* um = cc__unmask_line_directives(src_ufcs, src_ufcs_len, &um_len);
            if (um) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = um;
                src_ufcs_len = um_len;
            }
        }

        /* Final cosmetic pass: src_ufcs is now fully lowered + spliced and is
           only consumed by the write below, so trimming trailing whitespace
           (line-count preserving) is safe and removes the blank runs left by
           @comptime blanking. */
        cc__strip_trailing_ws_in_place(src_ufcs, &src_ufcs_len);

        /* Persist post-UFCS local .cch bodies back to out/include/*.h and
         * collapse splice markers to #include so the host .c does not
         * duplicate definitions. */
        if (cc_writeback_local_lowered_headers_from_codegen(&src_ufcs, &src_ufcs_len) != 0) {
            goto fail;
        }

        /* Final cosmetics — host-compiler buffer only: collapse the doubled
         * `__typeof__(EXPR) tmp = (EXPR)` spelling (line-count preserving),
         * compact blank runs against their `#line` anchors, then drop
         * `#line` ping-pong pairs (removes lines). */
        cc__collapse_typeof_dup_decls(src_ufcs, &src_ufcs_len);
        cc__compact_blank_runs_at_line_directives(src_ufcs, &src_ufcs_len);
        cc__coalesce_adjacent_line_directives(src_ufcs, &src_ufcs_len);
        /* Host TCC rejects `constructor(N)` priority args; blank them in the
         * emit buffer (clang accepts bare `constructor` equally). Parse-time
         * L2 already does this for libtcc; the written .c must match. */
        if (strstr(src_ufcs, "constructor(") || strstr(src_ufcs, "destructor(")) {
            (void)cc_l2_blank_ctor_priority_inplace(src_ufcs, src_ufcs_len);
        }

        fwrite(src_ufcs, 1, src_ufcs_len, out);
        if (src_ufcs_len == 0 || src_ufcs[src_ufcs_len - 1] != '\n') fputc('\n', out);

        if (closure_defs && closure_defs_len > 0) {
            /* Re-lower once more: the merge path above may be skipped
             * (no-merge TUs) and spawn closures can carry @defer. */
            cc__relower_closure_defs(ctx, &closure_defs, &closure_defs_len);
            cc__collapse_typeof_dup_decls(closure_defs, &closure_defs_len);

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
                "  CCString s = cc_string_new();\n"
                "  cc_string_push(&s, cc_slice_from_buffer(\"Hello, \", sizeof(\"Hello, \") - 1), &a);\n"
                "  cc_string_push(&s, cc_slice_from_buffer(\"Concurrent-C via UFCS!\\n\", sizeof(\"Concurrent-C via UFCS!\\n\") - 1), &a);\n"
                "  cc_std_out_write(cc_string_as_slice(&s));\n"
                "  return 0;\n"
                "}\n");
    }

    fclose(out);
    cc__maybe_format_lowered_output(output_path);
    return 0;
fail:
    /* Unified error exit: every owned pointer is declared at the top,
     * NULL-initialized, and NULLed after mid-function frees, so this
     * single block replaces ~20 hand-copied cleanup runs that had
     * already diverged (audit: several leaked src_all, one leaked the
     * FILE*).  free(NULL) is a no-op by contract. */
    fclose(out);
    if (src_ufcs && src_ufcs != src_all) free(src_ufcs);
    free(src_all);
    free(src_raw);
    if (src_regs_owned) free(src_regs);
    free(closure_protos);
    free(closure_defs);
    return EINVAL;
}

