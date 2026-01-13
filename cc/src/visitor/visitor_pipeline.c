#include "visitor_pipeline.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "comptime/symbols.h"
#include "parser/parse.h"
#include "parser/tcc_bridge.h"
#include "preprocess/preprocess.h"
#include "util/io.h"
#include "visitor/async_ast.h"
#include "visitor/checker.h"
#include "visitor/pass_autoblock.h"
#include "visitor/pass_await_normalize.h"
#include "visitor/pass_closure_calls.h"
#include "visitor/pass_strip_markers.h"
#include "visitor/pass_ufcs.h"
#include "visitor/pass.h"
#include "visitor/visitor.h"
#include "visitor/walk.h"


static const char* cc__basename(const char* path);
static const char* cc__path_suffix2(const char* path);
static int cc__same_source_file(const char* a, const char* b);
static size_t cc__offset_of_line_1based(const char* s, size_t len, int line_no);
static size_t cc__offset_of_line_col_1based(const char* s, size_t len, int line_no, int col_no);
static int cc__node_file_matches_this_tu(const CCASTRoot* root,
                                        const CCVisitorCtx* ctx,
                                        const char* file);

static int cc__read_entire_file(const char* path, char** out_buf, size_t* out_len) {
    if (!path || !out_buf || !out_len) return 0;
    *out_buf = NULL;
    *out_len = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    *out_buf = buf;
    *out_len = n;
    return 1;
}

/* Helper: write temp C file for reparse */
static char* cc__write_temp_c_file(const char* buf, size_t len, const char* original_path) {
    if (!buf || !original_path) return NULL;
    char tmpl[] = "/tmp/cc_reparse_XXXXXX.c";
#ifdef __APPLE__
    int fd = mkstemps(tmpl, 2);
#else
    int fd = mkstemp(tmpl);
#endif
    if (fd < 0) return NULL;
    /* Minimal prelude so patched TCC can type-check rewritten intermediate code during the reparse step. */
    const char* prelude =
        "#define CC_PARSER_MODE 1\n"
        "#include <stdint.h>\n"
        "typedef intptr_t CCAbIntptr;\n";
    size_t pre_len = strlen(prelude);
    size_t off = 0;
    while (off < pre_len) {
        ssize_t n = write(fd, prelude + off, pre_len - off);
        if (n <= 0) { close(fd); unlink(tmpl); return NULL; }
        off += (size_t)n;
    }
    off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n <= 0) { close(fd); unlink(tmpl); return NULL; }
        off += (size_t)n;
    }
    close(fd);
    return strdup(tmpl);
}

/* Helper: reparse rewritten source to get updated stub AST */
static CCASTRoot* cc__reparse_after_rewrite(const char* rewritten_src, size_t rewritten_len,
                                           const char* input_path, CCSymbolTable* symbols,
                                           char** tmp_path_out) {
    char* tmp_path = cc__write_temp_c_file(rewritten_src, rewritten_len, input_path);
    if (!tmp_path) return NULL;

    char pp_path[128];
    int pp_err = cc_preprocess_file(tmp_path, pp_path, sizeof(pp_path));
    const char* use_path = (pp_err == 0) ? pp_path : tmp_path;

    CCASTRoot* root2 = cc_tcc_bridge_parse_to_ast(use_path, input_path, symbols);
    if (!root2) {
        unlink(tmp_path);
        free(tmp_path);
        if (pp_err == 0) unlink(pp_path);
        return NULL;
    }

    if (pp_err == 0) {
        root2->lowered_is_temp = 1;
    }

    *tmp_path_out = tmp_path;
    return root2;
}

/* Main visitor pipeline: orchestrates all lowering passes */
int cc_visit_pipeline(const CCASTRoot* root, CCVisitorCtx* ctx, const char* output_path) {
    if (!ctx || !ctx->symbols || !output_path) return EINVAL;
    const char* src_path = ctx->input_path ? ctx->input_path : "<cc_input>";
    FILE* out = fopen(output_path, "w");
    if (!out) return errno ? errno : -1;

    /* Read original source once; we may rewrite UFCS spans before @arena lowering. */
    char* src_all = NULL;
    size_t src_len = 0;
    if (ctx->input_path) {
        cc__read_entire_file(ctx->input_path, &src_all, &src_len);
    }

    char* src_ufcs = src_all;
    size_t src_ufcs_len = src_len;

    /* PASS 1: UFCS rewriting (collect spans from stub AST) */
    if (root && root->nodes && root->node_count > 0) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_ufcs_spans_with_nodes(root, ctx, src_all, src_len, &rewritten, &rewritten_len)) {
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }

    /* PASS 2: Closure call rewriting */
    if (root && root->nodes && root->node_count > 0) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_all_closure_calls_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }

    /* PASS 3: Auto-blocking (first cut) */
    if (root && root->nodes && root->node_count > 0 && ctx->symbols) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_autoblocking_calls_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }

    /* PASS 4: Normalize await <expr> so the async state machine can lower it */
    if (root && root->nodes && root->node_count > 0) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_await_exprs_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }

    /* PASS 5: AST-driven @async lowering (state machine). IMPORTANT: reparse after earlier rewrites */
    char* rewritten_async = NULL;
    size_t rewritten_async_len = 0;
    if (src_ufcs && ctx && ctx->symbols) {
        char* tmp_path = NULL;
        CCASTRoot* root2 = cc__reparse_after_rewrite(src_ufcs, src_ufcs_len, src_path, ctx->symbols, &tmp_path);
        if (!root2) {
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            fclose(out);
            return EINVAL;
        }

        int ar = cc_async_rewrite_state_machine_ast(root2, ctx, src_ufcs, src_ufcs_len, &rewritten_async, &rewritten_async_len);
        cc_tcc_bridge_free_ast(root2);

        if (tmp_path) {
            unlink(tmp_path);
            free(tmp_path);
        }

        if (ar < 0) {
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            fclose(out);
            return EINVAL;
        }
        if (ar > 0) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten_async;
            src_ufcs_len = rewritten_async_len;
        }
    }

    /* PASS 6: Strip @async/@noblock/@latency_sensitive markers */
    char* stripped = NULL;
    size_t stripped_len = 0;
    if (src_ufcs && cc__strip_cc_decl_markers(src_ufcs, src_ufcs_len, &stripped, &stripped_len)) {
        if (src_ufcs != src_all) free(src_ufcs);
        src_ufcs = stripped;
        src_ufcs_len = stripped_len;
    }

    /* NOTE: slice move/provenance checking is now handled by the stub-AST checker pass
       (`cc/src/visitor/checker.c`) before visitor lowering. */

    /* Emit CC headers and helpers */
    fprintf(out, "/* CC visitor: passthrough of lowered C (preprocess + TCC parse) */\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include \"cc_nursery.cch\"\n");
    fprintf(out, "#include \"cc_closure.cch\"\n");
    fprintf(out, "#include \"cc_slice.cch\"\n");
    fprintf(out, "#include \"cc_runtime.cch\"\n");
    fprintf(out, "#include \"std/task_intptr.cch\"\n");
    /* Helper alias: used for auto-blocking arg binding to avoid accidental hoisting of these temps. */
    fprintf(out, "typedef intptr_t CCAbIntptr;\n");
    /* Spawn thunks are emitted later (after parsing source) as static fns in this TU. */
    fprintf(out, "\n");
    fprintf(out, "/* --- CC spawn lowering helpers (best-effort) --- */\n");
    fprintf(out, "typedef struct { void (*fn)(void); } __cc_spawn_void_arg;\n");
    fprintf(out, "static void* __cc_spawn_thunk_void(void* p) {\n");
    fprintf(out, "  __cc_spawn_void_arg* a = (__cc_spawn_void_arg*)p;\n");
    fprintf(out, "  if (a && a->fn) a->fn();\n");
    fprintf(out, "  free(a);\n");
    fprintf(out, "  return NULL;\n");
    fprintf(out, "}\n");
    fprintf(out, "typedef struct { void (*fn)(int); int arg; } __cc_spawn_int_arg;\n");
    fprintf(out, "static void* __cc_spawn_thunk_int(void* p) {\n");
    fprintf(out, "  __cc_spawn_int_arg* a = (__cc_spawn_int_arg*)p;\n");
    fprintf(out, "  if (a && a->fn) a->fn(a->arg);\n");
    fprintf(out, "  free(a);\n");
    fprintf(out, "  return NULL;\n");
    fprintf(out, "}\n");
    fprintf(out, "/* --- end spawn helpers --- */\n\n");

    /* TODO: Extract the remaining closure/UFCS emission logic from visitor.c */

    /* Preserve diagnostics mapping to the original input where possible. */
    fprintf(out, "#line 1 \"%s\"\n", src_path);

    /* Write the final lowered source */
    if (src_ufcs) {
        fwrite(src_ufcs, 1, src_ufcs_len, out);
    }

    /* Cleanup */
    if (src_ufcs != src_all) free(src_ufcs);
    free(src_all);
    fclose(out);

    return 0;
}

/* Helper implementations (extracted from visitor.c) */
static const char* cc__basename(const char* path) {
    if (!path) return NULL;
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

static const char* cc__path_suffix2(const char* path) {
    if (!path) return NULL;
    const char* end = path + strlen(path);
    int seps = 0;
    for (const char* p = end; p > path; ) {
        p--;
        if (*p == '/' || *p == '\\') {
            seps++;
            if (seps == 2) return p + 1;
        }
    }
    return cc__basename(path);
}

static int cc__same_source_file(const char* a, const char* b) {
    if (!a || !b) return 0;
    if (strcmp(a, b) == 0) return 1;

    const char* a_base = cc__basename(a);
    const char* b_base = cc__basename(b);
    if (!a_base || !b_base || strcmp(a_base, b_base) != 0) return 0;

    /* Prefer 2-component suffix match (handles duplicate basenames across dirs). */
    const char* a_suf = cc__path_suffix2(a);
    const char* b_suf = cc__path_suffix2(b);
    if (a_suf && b_suf && strcmp(a_suf, b_suf) == 0) return 1;

    /* Fallback: basename-only match. */
    return 1;
}

static size_t cc__offset_of_line_1based(const char* s, size_t len, int line_no) {
    if (!s || line_no <= 1) return 0;
    int cur = 1;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') {
            cur++;
            if (cur == line_no) return i + 1;
        }
    }
    return len;
}

static size_t cc__offset_of_line_col_1based(const char* s, size_t len, int line_no, int col_no) {
    if (!s) return 0;
    if (line_no <= 1 && col_no <= 1) return 0;
    if (col_no <= 1) return cc__offset_of_line_1based(s, len, line_no);
    size_t loff = cc__offset_of_line_1based(s, len, line_no);
    size_t off = loff + (size_t)(col_no - 1);
    if (off > len) off = len;
    return off;
}

static int cc__node_file_matches_this_tu(const CCASTRoot* root,
                                        const CCVisitorCtx* ctx,
                                        const char* file) {
    if (!file || !ctx->input_path) return 0;
    if (cc__same_source_file(ctx->input_path, file)) return 1;
    if (root->lowered_path && cc__same_source_file(root->lowered_path, file)) return 1;
    return 0;
}