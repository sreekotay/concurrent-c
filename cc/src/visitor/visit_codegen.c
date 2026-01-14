#include "visitor.h"
#include "visit_codegen.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>

#include "visitor/ufcs.h"
#include "visitor/pass_strip_markers.h"
#include "visitor/pass_await_normalize.h"
#include "visitor/pass_ufcs.h"
#include "visitor/pass_closure_calls.h"
#include "visitor/pass_autoblock.h"
#include "visitor/pass_arena_ast.h"
#include "visitor/pass_nursery_spawn_ast.h"
#include "visitor/pass_closure_literal_ast.h"
#include "visitor/pass_defer_syntax.h"
#include "visitor/pass_with_deadline_syntax.h"
#include "visitor/visitor_fileutil.h"
#include "visitor/text_span.h"
#include "parser/tcc_bridge.h"
#include "preprocess/preprocess.h"

#ifndef CC_TCC_EXT_AVAILABLE
#error "CC_TCC_EXT_AVAILABLE is required (patched TCC stub-AST required)."
#endif

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

static const char* cc__basename(const char* path) {
    if (!path) return NULL;
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

/* Return pointer to a stable suffix (last 2 path components) inside `path`.
   If `path` has fewer than 2 components, returns basename. */
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

/* UFCS span rewrite lives in pass_ufcs.c now (cc__rewrite_ufcs_spans_with_nodes). */


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

    /* For final codegen we read the original source and lower UFCS/@arena here.
       The preprocessor's temp file exists only to make TCC parsing succeed. */
    /* Read original source once; we may rewrite UFCS spans before @arena lowering. */
    char* src_all = NULL;
    size_t src_len = 0;
    if (ctx->input_path) {
#ifdef CC_TCC_EXT_AVAILABLE
        cc__read_entire_file(ctx->input_path, &src_all, &src_len);
#else
        cc__read_entire_file(ctx->input_path, &src_all, &src_len);
#endif
    }

    char* src_ufcs = src_all;
    size_t src_ufcs_len = src_len;

    /* Rewrite `with_deadline(expr) { ... }` (not valid C) into CCDeadline scope syntax
       using @defer, so the rest of the pipeline sees valid parseable text. */
    if (src_all && src_len) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_with_deadline_syntax(src_all, src_len, &rewritten, &rewritten_len) == 0 && rewritten) {
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }

    /* Produced by the closure-literal AST pass (emitted into the output TU). */
    char* closure_protos = NULL;
    size_t closure_protos_len = 0;
    char* closure_defs = NULL;
    size_t closure_defs_len = 0;

#ifdef CC_TCC_EXT_AVAILABLE
    if (src_ufcs && root && root->nodes && root->node_count > 0) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_ufcs_spans_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len)) {
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }
#endif

    /* Rewrite closure calls anywhere (including nested + multiline) using stub CALL nodes. */
#ifdef CC_TCC_EXT_AVAILABLE
    char* src_calls = NULL;
    size_t src_calls_len = 0;
    if (src_ufcs && root && root->nodes && root->node_count > 0) {
        if (cc__rewrite_all_closure_calls_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &src_calls, &src_calls_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = src_calls;
            src_ufcs_len = src_calls_len;
        }
    }
#endif

    /* Auto-blocking (first cut): inside @async functions, wrap statement-form calls to known
       non-@async/non-@noblock functions in cc_run_blocking_closure0(() => { ... }). */
#ifdef CC_TCC_EXT_AVAILABLE
    if (src_ufcs && root && root->nodes && root->node_count > 0 && ctx->symbols) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_autoblocking_calls_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }
#endif

    /* Normalize `await <expr>` used inside larger expressions into temp hoists so the
       text-based async state machine can lower it (AST-driven span rewrite). */
#ifdef CC_TCC_EXT_AVAILABLE
    if (src_ufcs && root && root->nodes && root->node_count > 0) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_await_exprs_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
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

    /* Reparse the current TU source to get an up-to-date stub-AST for statement-level lowering
       (@arena/@nursery/spawn). These rewrites run before marker stripping to keep spans stable. */
    if (src_ufcs && ctx && ctx->symbols) {
        char* tmp_path = cc__write_temp_c_file(src_ufcs, src_ufcs_len, ctx->input_path);
        char pp_path[128];
        int pp_err = tmp_path ? cc_preprocess_file(tmp_path, pp_path, sizeof(pp_path)) : EINVAL;
        const char* use_path = (pp_err == 0) ? pp_path : tmp_path;
        CCASTRoot* root3 = use_path ? cc_tcc_bridge_parse_to_ast(use_path, ctx->input_path, ctx->symbols) : NULL;
        if (pp_err == 0 && !(getenv("CC_KEEP_REPARSE"))) unlink(pp_path);
        if (tmp_path) {
            if (!getenv("CC_KEEP_REPARSE")) unlink(tmp_path);
            free(tmp_path);
        }
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

        /* Reparse after closure rewrite so spawn/nursery/arena spans are correct. */
        char* tmp2 = cc__write_temp_c_file(src_ufcs, src_ufcs_len, ctx->input_path);
        char pp2[128];
        int pp2_err = tmp2 ? cc_preprocess_file(tmp2, pp2, sizeof(pp2)) : EINVAL;
        const char* use2 = (pp2_err == 0) ? pp2 : tmp2;
        CCASTRoot* root4 = use2 ? cc_tcc_bridge_parse_to_ast(use2, ctx->input_path, ctx->symbols) : NULL;
        if (pp2_err == 0 && !(getenv("CC_KEEP_REPARSE"))) unlink(pp2);
        if (tmp2) {
            if (!getenv("CC_KEEP_REPARSE")) unlink(tmp2);
            free(tmp2);
        }
        if (!root4) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }

        /* 2) spawn(...) -> cc_nursery_spawn* (hard error if outside nursery). */
        {
            char* rewritten = NULL;
            size_t rewritten_len = 0;
            int r = cc__rewrite_spawn_stmts_with_nodes(root4, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
            if (r < 0) {
                cc_tcc_bridge_free_ast(root4);
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
        cc_tcc_bridge_free_ast(root4);

        /* Reparse after spawn rewrite so nursery/arena end braces are correct. */
        char* tmp3 = cc__write_temp_c_file(src_ufcs, src_ufcs_len, ctx->input_path);
        char pp3[128];
        int pp3_err = tmp3 ? cc_preprocess_file(tmp3, pp3, sizeof(pp3)) : EINVAL;
        const char* use3 = (pp3_err == 0) ? pp3 : tmp3;
        CCASTRoot* root5 = use3 ? cc_tcc_bridge_parse_to_ast(use3, ctx->input_path, ctx->symbols) : NULL;
        if (pp3_err == 0 && !(getenv("CC_KEEP_REPARSE"))) unlink(pp3);
        if (tmp3) {
            if (!getenv("CC_KEEP_REPARSE")) unlink(tmp3);
            free(tmp3);
        }
        if (!root5) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }

        /* 3) @nursery { ... } -> CCNursery create/wait/free */
        {
            char* rewritten = NULL;
            size_t rewritten_len = 0;
            int r = cc__rewrite_nursery_blocks_with_nodes(root5, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
            if (r < 0) {
                cc_tcc_bridge_free_ast(root5);
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

        /* 4) @arena(...) { ... } -> CCArena prologue/epilogue */
        {
            char* rewritten = NULL;
            size_t rewritten_len = 0;
            int r = cc__rewrite_arena_blocks_with_nodes(root5, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
            if (r > 0) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = rewritten_len;
            }
        }
        cc_tcc_bridge_free_ast(root5);
    }

    /* Lower @defer (and hard-error on cancel) using a syntax-driven pass.
       IMPORTANT: this must run BEFORE async lowering so `@defer` can be made suspend-safe. */
    if (src_ufcs) {
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
       IMPORTANT: run AFTER CC statement-level lowering so @nursery/@arena/spawn/closures are real C. */
    if (src_ufcs && ctx && ctx->symbols) {
        char* tmp_path = cc__write_temp_c_file(src_ufcs, src_ufcs_len, ctx->input_path);
        char pp_path[128];
        int pp_err = tmp_path ? cc_preprocess_file(tmp_path, pp_path, sizeof(pp_path)) : EINVAL;
        const char* use_path = (pp_err == 0) ? pp_path : tmp_path;
        if (getenv("CC_DEBUG_REPARSE")) {
            fprintf(stderr, "CC: reparse: tmp=%s pp=%s pp_err=%d use=%s\n",
                    tmp_path ? tmp_path : "<null>",
                    (pp_err == 0) ? pp_path : "<n/a>",
                    pp_err,
                    use_path ? use_path : "<null>");
        }
        CCASTRoot* root2 = use_path ? cc_tcc_bridge_parse_to_ast(use_path, ctx->input_path, ctx->symbols) : NULL;
        if (!root2) {
            if (tmp_path) {
                if (!getenv("CC_KEEP_REPARSE")) unlink(tmp_path);
                free(tmp_path);
            }
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }
        if (pp_err == 0) root2->lowered_is_temp = 1;
        if (getenv("CC_DEBUG_REPARSE")) {
            fprintf(stderr, "CC: reparse: stub ast node_count=%d\n", root2->node_count);
        }

        char* rewritten = NULL;
        size_t rewritten_len = 0;
        int ar = cc_async_rewrite_state_machine_ast(root2, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
        cc_tcc_bridge_free_ast(root2);
        if (tmp_path) {
            if (!getenv("CC_KEEP_REPARSE")) unlink(tmp_path);
            free(tmp_path);
        }
        if (pp_err == 0 && !(getenv("CC_KEEP_REPARSE"))) {
            unlink(pp_path);
        }
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

    /* Captures are lowered via __cc_closure_make_N factories. */
    if (closure_protos && closure_protos_len > 0) {
        fputs("/* --- CC closure forward decls --- */\n", out);
        fwrite(closure_protos, 1, closure_protos_len, out);
        fputs("/* --- end closure forward decls --- */\n\n", out);
    }

    /* Preserve diagnostics mapping to the original input where possible. */
    fprintf(out, "#line 1 \"%s\"\n", src_path);

    if (src_ufcs) {
        fwrite(src_ufcs, 1, src_ufcs_len, out);
        if (src_ufcs_len == 0 || src_ufcs[src_ufcs_len - 1] != '\n') fputc('\n', out);

        free(closure_protos);
        if (closure_defs && closure_defs_len > 0) {
            /* Emit closure definitions at end-of-file so global names are in scope. */
            fputs("\n#line 1 \"<cc_generated_closures>\"\n", out);
            fwrite(closure_defs, 1, closure_defs_len, out);
        }
        free(closure_defs);
        if (src_ufcs != src_all) free(src_ufcs);
        free(src_all);
    } else {
        // Fallback stub when input is unavailable.
        fprintf(out,
                "#include \"std/prelude.cch\"\n"
                "int main(void) {\n"
                "  CCArena a = cc_heap_arena(kilobytes(1));\n"
                "  CCString s = cc_string_new(&a, 0);\n"
                "  cc_string_append_cstr(&a, &s, \"Hello, \");\n"
                "  cc_string_append_cstr(&a, &s, \"Concurrent-C via UFCS!\\n\");\n"
                "  cc_std_out_write(cc_string_as_slice(&s));\n"
                "  return 0;\n"
                "}\n");
    }

    fclose(out);
    return 0;
}

