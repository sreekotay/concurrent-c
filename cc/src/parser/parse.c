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
#include "util/text.h"
#include "visitor/pass_create.h"
#include "visitor/pass_channel_syntax.h"
#include "visitor/pass_unwrap_destroy.h"
#include "util/path.h"

char* cc_blank_comptime_blocks_for_prep(const char* src, size_t n) {
    char* out;
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
        if (c != '@' || !cc_match_ident_kw(src, n, i + 1, "comptime")) continue;
        {
            size_t kw_end = i + 1 + strlen("comptime");
            size_t body_l = cc_skip_ws_len(src, n, kw_end);
            size_t body_r;
            if (body_l >= n) continue;
            if (src[body_l] == '{') {
                if (!cc_find_matching_brace(src, n, body_l, &body_r)) continue;
                for (size_t k = i; k <= body_r; ++k) {
                    if (out[k] != '\n') out[k] = ' ';
                }
                i = body_r;
                continue;
            }
            /* @comptime fn/const decl — blank through body or ';'. */
            {
                size_t p = body_l, lpar = 0, rpar = 0, end = 0;
                for (; p < n; p++) {
                    if (src[p] == '(') { lpar = p; break; }
                }
                if (lpar) {
                    if (!cc_find_matching_paren(src, n, lpar, &rpar)) continue;
                    p = cc_skip_ws_len(src, n, rpar + 1);
                    if (p < n && src[p] == '{') {
                        if (!cc_find_matching_brace(src, n, p, &body_r)) continue;
                        end = body_r;
                    } else {
                        for (; p < n; p++) {
                            if (src[p] == ';') { end = p; break; }
                        }
                        if (!end) continue;
                    }
                } else {
                    for (; p < n; p++) {
                        if (src[p] == ';') { end = p; break; }
                    }
                    if (!end) continue;
                }
                for (size_t k = i; k <= end; ++k) {
                    if (out[k] != '\n') out[k] = ' ';
                }
                i = end;
            }
        }
    }
    return out;
}

static char* cc__neutralize_comments_preserve_layout_parse(const char* src, size_t n) {
    char* out;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    if (!src) return NULL;
    out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n);
    out[n] = '\0';
    for (size_t i = 0; i < n; ++i) {
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
                ++i;
                continue;
            }
            if (c != '\n' && c != '\r' && c != '\t') out[i] = ' ';
            continue;
        }
        if (in_str) {
            if (c == '\\' && i + 1 < n) { ++i; continue; }
            if (c == '"') in_str = 0;
            continue;
        }
        if (in_chr) {
            if (c == '\\' && i + 1 < n) { ++i; continue; }
            if (c == '\'') in_chr = 0;
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c == '/' && c2 == '/') {
            out[i] = ' ';
            out[i + 1] = ' ';
            in_lc = 1;
            ++i;
            continue;
        }
        if (c == '/' && c2 == '*') {
            out[i] = ' ';
            out[i + 1] = ' ';
            in_bc = 1;
            ++i;
            continue;
        }
    }
    return out;
}

static char* cc__rewrite_result_helper_calls_for_parser_parse(const char* src, size_t n) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    size_t last_emit = 0;

    if (!src || n == 0) return NULL;
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
        size_t ident_len = ident_end - ident_start;
        const char* parser_method = NULL;
        size_t suffix_len = 0;
        size_t j;
        size_t paren_end = 0;

        if (ident_len <= 9 || memcmp(src + ident_start, "CCResult_", 9) != 0) continue;

        j = cc_skip_ws_and_comments(src, n, ident_end);
        if (j >= n || src[j] != '(') continue;

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

        if (!cc_find_matching_paren(src, n, j, &paren_end)) continue;

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

        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, ident_start - last_emit);
        cc_sb_append_cstr(&out, &out_len, &out_cap, "__cc_parser_result_");
        cc_sb_append_cstr(&out, &out_len, &out_cap, parser_method);
        cc_sb_append_cstr(&out, &out_len, &out_cap, "_");
        cc_sb_append(&out, &out_len, &out_cap, src + ident_start, ident_len - suffix_len);
        last_emit = ident_end;
    }

    if (last_emit == 0) return NULL;
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
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

