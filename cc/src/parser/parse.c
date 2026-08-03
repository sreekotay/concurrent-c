#include "parse.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>

#include "build_parse_input.h"
#include "tcc_bridge.h"
#include "comptime/hook_compile.h"
#include "comptime/symbols.h"
#include "preprocess/preprocess.h"
#include "preprocess/script_entry.h"
#include "preprocess/type_graph.h"
#include "util/text.h"
#include "util/text_scan.h"
#include "visitor/pass_create.h"
#include "parser/check_py_own.h"
#include "visitor/pass_channel_syntax.h"
#include "visitor/pass_unwrap_destroy.h"
#include "util/path.h"

char* cc_blank_comptime_blocks_for_prep(const char* src, size_t n) {
    char* out;
    CCInertScan sc;
    if (!src) return NULL;
    out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n);
    out[n] = '\0';
    cc_inert_scan_init(&sc, NULL);
    for (size_t i = 0; i < n;) {
        if (cc_inert_scan_step(&sc, src, n, &i)) continue;
        if (src[i] != '@' || !cc_match_ident_kw(src, n, i + 1, "comptime")) { i++; continue; }
        {
            size_t kw_end = i + 1 + strlen("comptime");
            size_t body_l = cc_skip_ws_and_comments(src, n, kw_end);
            size_t body_r;
            if (body_l >= n) { i++; continue; }
            if (src[body_l] == '{') {
                if (!cc_find_matching_brace(src, n, body_l, &body_r)) { i++; continue; }
                for (size_t k = i; k <= body_r; ++k) {
                    if (out[k] != '\n') out[k] = ' ';
                }
                {
                    char marker[64];
                    int mlen = snprintf(marker, sizeof(marker),
                                        "enum{__ccs%zu=0};", body_l);
                    if (mlen > 0) {
                        size_t line_s = i;
                        while (line_s <= body_r) {
                            size_t line_e = line_s;
                            while (line_e <= body_r && out[line_e] != '\n') line_e++;
                            if (line_e - line_s >= (size_t)mlen) {
                                memcpy(out + line_s, marker, (size_t)mlen);
                                break;
                            }
                            line_s = line_e + 1;
                        }
                    }
                }
                i = body_r + 1;
                continue;
            }
            /* @comptime fn/const decl — blank through body or ';'. */
            {
                size_t p = body_l, lpar = 0, rpar = 0, end = 0;
                {
                    size_t lp = cc_find_char_top_level(src, p, n, '(');
                    if (lp < n) lpar = lp;
                }
                if (lpar) {
                    if (!cc_find_matching_paren(src, n, lpar, &rpar)) { i++; continue; }
                    p = cc_skip_ws_and_comments(src, n, rpar + 1);
                    if (p < n && src[p] == '{') {
                        if (!cc_find_matching_brace(src, n, p, &body_r)) { i++; continue; }
                        end = body_r;
                    } else {
                        end = cc_find_char_top_level(src, p, n, ';');
                        if (end >= n) { i++; continue; }
                    }
                } else {
                    end = cc_find_char_top_level(src, p, n, ';');
                    if (end >= n) { i++; continue; }
                }
                for (size_t k = i; k <= end; ++k) {
                    if (out[k] != '\n') out[k] = ' ';
                }
                i = end + 1;
            }
        }
    }
    return out;
}

static char* cc__neutralize_comments_preserve_layout_parse(const char* src, size_t n) {
    char* out;
    CCInertScan sc;
    if (!src) return NULL;
    out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n);
    out[n] = '\0';
    cc_inert_scan_init(&sc, NULL);
    for (size_t i = 0; i < n;) {
        size_t before = i;
        int was_lc = sc.in_line_comment;
        int was_bc = sc.in_block_comment;
        int enter_comment = 0;
        if (!was_lc && !was_bc && !sc.in_str && !sc.in_chr && !sc.in_pp &&
            src[i] == '/' && i + 1 < n) {
            if (src[i + 1] == '/') enter_comment = 1;
            else if (src[i + 1] == '*') enter_comment = 2;
        }
        if (cc_inert_scan_step(&sc, src, n, &i)) {
            if (was_lc || enter_comment == 1) {
                /* Line-comment bytes (opener included): blank all but the
                 * terminating newline. */
                for (size_t k = before; k < i; ++k)
                    if (out[k] != '\n') out[k] = ' ';
            } else if (was_bc || enter_comment == 2) {
                /* Block-comment bytes (opener/closer included): blank all
                 * but layout characters. */
                for (size_t k = before; k < i; ++k)
                    if (out[k] != '\n' && out[k] != '\r' && out[k] != '\t')
                        out[k] = ' ';
            }
            continue;
        }
        i++;
    }
    return out;
}

/* Was: rewrite `CCResult_*_{unwrap,error,is_err,...}(...)` → `__cc_parser_result_*`.
 * Disabled — stubs are only prototyped for TU-discovered result specs, so
 * header helpers (e.g. `CCResult_CCSlice_CCError_unwrap`) became undeclared
 * calls that TCC typed as `int`, and `CCSlice x = result !>;` failed with
 * "'{' expected (got ';')".  Real `CC_DECL_RESULT_SPEC` static inlines are
 * already in scope at every valid call site (same as reparse-diet).
 *
 * That error spelling is TCC's report for ANY struct declaration whose
 * initializer has the wrong type: it decides the initializer must be a
 * brace list and complains about the missing `{`, at the declaration's
 * line — or at logical EOF when the statement came from a text pass.  The
 * other known route here is `!>` on a Result-returning MACRO: the callee
 * name has no registry entry, the unwrap falls back to the roster
 * `_Generic`, and if the Result type is not among the arms the `default:`
 * arm hands back the whole struct.  Headers avoid this by declaring the
 * macro's shape as a real prototype above the `#define` (see
 * `cc_py_obj_map` in ccc/script/py.cch), which the registry scanner picks
 * up. */
static char* cc__rewrite_result_helper_calls_for_parser_parse(const char* src, size_t n) {
    (void)src;
    (void)n;
    return NULL;
}

typedef struct {
    const char* registration_src;
    size_t registration_len;
} CCParseTypeCreateCompileCtx;

static int cc__compile_type_create_registration(CCSymbolTable* symbols,
                                                const char* registration_input_path,
                                                const char* logical_file,
                                                const char* type_name,
                                                const char* expr_src,
                                                size_t expr_len,
                                                void* user_ctx) {
    CCParseTypeCreateCompileCtx* ctx = (CCParseTypeCreateCompileCtx*)user_ctx;
    void* owner = NULL;
    const void* fn_ptr = NULL;
    if (!symbols || !registration_input_path || !type_name || !expr_src || expr_len == 0 || !ctx) return -1;
    if (cc_comptime_compile_type_hook_callable(registration_input_path,
                                               logical_file,
                                               ctx->registration_src,
                                               ctx->registration_len,
                                               expr_src,
                                               expr_len,
                                               CC_COMPTIME_TYPE_HOOK_CREATE,
                                               &owner,
                                               &fn_ptr) != 0) {
        fprintf(stderr, "%s: error: failed to compile create hook for '%s'\n",
                registration_input_path, type_name);
        return -1;
    }
    if (cc_symbols_set_type_create_callable(symbols, type_name, fn_ptr, owner, cc_comptime_type_hook_owner_free) != 0) {
        fprintf(stderr, "%s: error: failed to record create hook for '%s'\n",
                registration_input_path, type_name);
        cc_comptime_type_hook_owner_free(owner);
        return -1;
    }
    return 0;
}

int cc_parse_to_ast(const char* input_path, CCSymbolTable* symbols, CCASTRoot** out_root) {
    if (!input_path || !out_root) {
        return EINVAL;
    }
#ifdef CC_TCC_EXT_AVAILABLE
    /* Read input file into memory */
    FILE* f = fopen(input_path, "r");
    if (!f) return errno ? errno : EIO;
    fseek(f, 0, SEEK_END);
    long file_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_len < 0 || file_len > (1 << 22)) { /* 4MB cap */
        fclose(f);
        return ENOMEM;
    }
    char* file_buf = (char*)malloc((size_t)file_len + 1);
    if (!file_buf) {
        fclose(f);
        return ENOMEM;
    }
    size_t got = fread(file_buf, 1, (size_t)file_len, f);
    file_buf[got] = '\0';
    fclose(f);

    /* .shcc: auto-prelude, default @errhandler, synthetic main. */
    {
        size_t script_len = 0;
        char* script = cc_script_rewrite_source(input_path, file_buf, got, &script_len);
        if (script) {
            free(file_buf);
            file_buf = script;
            got = script_len;
        }
    }

    /* Ownership check on the raw user file: headers are not in view, so
     * py.cch's own CCPyObj plumbing never trips it. */
    if (cc_check_py_own(file_buf, got, input_path) != 0) {
        free(file_buf);
        return -1;
    }

    /* Phase 1 -> 2: include-expand once for comptime discovery, collect type
       registrations, then stash the buffer on the AST root so visit_codegen
       reuses it for UFCS (no second cc -E / phase-1 over the prelude). */
    char* reg_src = cc_preprocess_comptime_source(input_path);
    const char* use_src = reg_src ? reg_src : file_buf;
    size_t use_len = reg_src ? strlen(reg_src) : got;
    if (symbols) {
        CCParseTypeCreateCompileCtx create_ctx = {
            .registration_src = use_src,
            .registration_len = use_len,
        };
        if (cc_symbols_collect_type_registrations_ex(symbols,
                                                     input_path,
                                                     use_src,
                                                     use_len,
                                                     cc__compile_type_create_registration,
                                                     &create_ctx,
                                                     NULL,
                                                     NULL) != 0) {
            free(reg_src);
            free(file_buf);
            return -1;
        }
    }

    CCBuildParseInput prep = {0};
    if (cc_build_parse_input(file_buf, got, input_path, symbols, 0, &prep) != 0) {
        free(reg_src);
        free(file_buf);
        return -1;
    }
    free(file_buf);
    /* Preprocess registered Map/ArrayMap instantiations on the type graph;
     * seed UFCS method tables now — emitted DECL_UFCS markers are not in the
     * comptime buffer that collect_type_registrations already scanned. */
    if (symbols) {
        CCTypeGraph* graph = cc_type_graph_get_global();
        size_t n_map = graph ? cc_type_graph_map_count(graph) : 0;
        for (size_t mi = 0; mi < n_map; mi++) {
            const CCTypeInstantiation* inst = cc_type_graph_get_map(graph, mi);
            if (inst && inst->mangled_name && inst->mangled_name[0])
                (void)cc_symbols_register_map_ufcs(symbols, inst->mangled_name);
        }
    }
    char* pp_buf = prep.buffer;
    {
        size_t st_len = 0;
        char* st = cc__rewrite_chan_send_task_text(NULL, pp_buf, prep.len, &st_len);
        if (st) {
            free(pp_buf);
            pp_buf = st;
            prep.buffer = st;
            prep.len = st_len;
        }
    }
    {
        char* parser_helpers = cc__rewrite_result_helper_calls_for_parser_parse(pp_buf, strlen(pp_buf));
        if (parser_helpers) {
            free(pp_buf);
            pp_buf = parser_helpers;
            prep.buffer = parser_helpers;
            prep.len = strlen(parser_helpers);
        }
    }
    (void)prep.source_map; /* threaded through codegen in later milestones */

    /* Debug: dump preprocessed output if requested */
    if (getenv("CC_DUMP_LOWERED")) {
        const char* dump_path = getenv("CC_DUMP_LOWERED");
        FILE* df = fopen(dump_path, "w");
        if (df) {
            fputs(pp_buf, df);
            fclose(df);
            fprintf(stderr, "cc: dumped lowered source to %s\n", dump_path);
        }
    }

    /* Parse from string (no temp file).
       Use same relative path for virtual filename that #line directive uses. */
    char rel_path[1024];
    cc_path_rel_to_repo(input_path, rel_path, sizeof(rel_path));
    char* parse_input = cc__neutralize_comments_preserve_layout_parse(pp_buf, strlen(pp_buf));
    if (getenv("CC_DEBUG_MAIN_PARSE_DUMP")) {
        FILE* df = fopen(getenv("CC_DEBUG_MAIN_PARSE_DUMP"), "w");
        if (df) {
            const char* b = parse_input ? parse_input : pp_buf;
            fwrite(b, 1, strlen(b), df);
            fclose(df);
        }
    }
    CCASTRoot* root = cc_tcc_bridge_parse_string_to_ast(parse_input ? parse_input : pp_buf, rel_path, input_path, symbols);
    free(parse_input);
    /* M7.C3 / M1-lite: when pre-expand is on, hand the post-pre-expand
     * buffer (with macros resolved and CC type syntax re-lowered) to the
     * visitor via root->parse_buffer.  Visitor span passes that scan the
     * raw user source can fall back to this buffer when their pattern
     * doesn't match in src_all (e.g. `cc__find_chan_decl_before` for
     * macro-generated chan handles). The buffer ownership is transferred
     * to root; `cc_tcc_bridge_free_ast` frees it. */
    {
        /* Pre-expand is unconditional now (the CC_PRE_EXPAND opt-out was
         * collapsed 2026-05-29; see build_parse_input.c). */
        if (root && pp_buf) {
            root->parse_buffer = pp_buf;       /* take ownership */
            root->parse_buffer_len = prep.len;
            prep.buffer = NULL;                /* don't double-free */
            prep.len = 0;
            if (prep.buffer_pre_relower) {
                root->parse_buffer_pre_relower = prep.buffer_pre_relower;
                root->parse_buffer_pre_relower_len = prep.buffer_pre_relower_len;
                prep.buffer_pre_relower = NULL;
                prep.buffer_pre_relower_len = 0;
            }
            if (prep.buffer_codegen) {
                root->codegen_buffer = prep.buffer_codegen;
                root->codegen_buffer_len = prep.buffer_codegen_len;
                prep.buffer_codegen = NULL;
                prep.buffer_codegen_len = 0;
            }
        }
    }
    cc_build_parse_input_free(&prep);
    if (root) {
        root->original_path = input_path;
        root->lowered_is_temp = 0;
        root->comptime_buffer = reg_src;
        root->comptime_buffer_len = reg_src ? use_len : 0;
        reg_src = NULL;
        *out_root = root;
        return 0;
    }
    free(reg_src);
    return -1;
#endif
    // Fallback: dummy AST so the pipeline keeps running when hooks are absent.
    (void)symbols;
    *out_root = (CCASTRoot*)malloc(sizeof(CCASTRoot));
    if (!*out_root) return ENOMEM;
    (*out_root)->original_path = input_path;
    (*out_root)->lowered_path = NULL;
    (*out_root)->lowered_is_temp = 0;
    (*out_root)->tcc_root = NULL;
    (*out_root)->nodes = NULL;
    (*out_root)->node_count = 0;
    return 0;
}

void cc_free_ast(CCASTRoot* root) {
    if (!root) return;
#ifdef CC_TCC_EXT_AVAILABLE
    if (root->tcc_root) {
        cc_tcc_bridge_free_ast(root);
        return;
    }
#endif
    if (root->lowered_path) {
        if (root->lowered_is_temp && !getenv("CC_KEEP_PP")) unlink(root->lowered_path);
        free(root->lowered_path);
    }
    free(root);
}

