#include "parse.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>

#include "tcc_bridge.h"
#include "comptime/hook_compile.h"
#include "comptime/symbols.h"
#include "preprocess/preprocess.h"
#include "util/text.h"
#include "visitor/pass_create.h"
#include "visitor/pass_channel_syntax.h"
#include "util/path.h"

static int cc__match_kw_parse(const char* src, size_t n, size_t pos, const char* kw) {
    size_t klen = strlen(kw);
    if (pos + klen > n) return 0;
    if (memcmp(src + pos, kw, klen) != 0) return 0;
    if (pos > 0 && (isalnum((unsigned char)src[pos - 1]) || src[pos - 1] == '_')) return 0;
    if (pos + klen < n && (isalnum((unsigned char)src[pos + klen]) || src[pos + klen] == '_')) return 0;
    return 1;
}

static size_t cc__skip_ws_parse(const char* src, size_t n, size_t i) {
    while (i < n && isspace((unsigned char)src[i])) i++;
    return i;
}

static int cc__find_matching_brace_parse(const char* src, size_t len, size_t lbrace, size_t* out_rbrace) {
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

static char* cc__blank_comptime_blocks_preserve_layout_parse(const char* src, size_t n) {
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
        if (c != '@' || !cc__match_kw_parse(src, n, i + 1, "comptime")) continue;
        {
            size_t kw_end = i + 1 + strlen("comptime");
            size_t body_l = cc__skip_ws_parse(src, n, kw_end);
            size_t body_r;
            if (body_l >= n || src[body_l] != '{') continue;
            if (!cc__find_matching_brace_parse(src, n, body_l, &body_r)) continue;
            for (size_t k = i; k <= body_r; ++k) {
                if (out[k] != '\n') out[k] = ' ';
            }
            i = body_r;
        }
    }
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

    /* Phase 1 -> 2: build canonical CC for comptime, then collect the current
       comptime-visible effects (today: type registrations) from that source
       before phase 3 lowers the main TU for TCC/host-C consumption. */
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
    free(reg_src);

    {
        char* lowered_includes = cc_rewrite_local_cch_includes_to_lowered_headers(file_buf, got, input_path);
        if (lowered_includes) {
            free(file_buf);
            file_buf = lowered_includes;
            got = strlen(file_buf);
        }
    }

    {
        char* lowered_system_includes = cc_rewrite_system_cch_includes_to_lowered_headers(file_buf, got);
        if (lowered_system_includes) {
            free(file_buf);
            file_buf = lowered_system_includes;
            got = strlen(file_buf);
        }
    }

    char* parse_buf = cc__blank_comptime_blocks_preserve_layout_parse(file_buf, got);
    if (parse_buf) {
        free(file_buf);
        file_buf = parse_buf;
    }

    char* nursery_proto = cc_rewrite_nursery_create_destroy_proto(file_buf, got, input_path);
    if (nursery_proto == (char*)-1) {
        free(file_buf);
        return -1;
    }
    if (nursery_proto) {
        free(file_buf);
        file_buf = nursery_proto;
        got = strlen(file_buf);
    }

    if (symbols) {
        char* registered_create = cc_rewrite_registered_type_create_destroy(file_buf, got, input_path, symbols);
        if (registered_create == (char*)-1) {
            free(file_buf);
            return -1;
        }
        if (registered_create) {
            free(file_buf);
            file_buf = registered_create;
            got = strlen(file_buf);
        }
    }

    /* Preprocess to string (no temp file) */
    char* pp_buf = cc_preprocess_to_string(file_buf, got, input_path);
    free(file_buf);
    if (!pp_buf) {
        return -1;
    }

    {
        size_t st_len = 0;
        char* st = cc__rewrite_chan_send_task_text(NULL, pp_buf, strlen(pp_buf), &st_len);
        if (st) {
            free(pp_buf);
            pp_buf = st;
            (void)st_len;
        }
    }

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
    CCASTRoot* root = cc_tcc_bridge_parse_string_to_ast(pp_buf, rel_path, input_path, symbols);
    free(pp_buf);
    if (root) {
        root->original_path = input_path;
        root->lowered_is_temp = 0;
        *out_root = root;
        return 0;
    }
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

