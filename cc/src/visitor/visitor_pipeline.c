#include "visitor_pipeline.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "comptime/symbols.h"
#include "parser/parse.h"
#include "parser/tcc_bridge.h"
#include "preprocess/preprocess.h"
#include "preprocess/type_registry.h"
#include "util/io.h"
#include "util/path.h"
#include "visitor/async_ast.h"
#include "visitor/checker.h"
#include "visitor/pass_autoblock.h"
#include "visitor/pass_await_normalize.h"
#include "visitor/pass_closure_calls.h"
#include "visitor/pass_strip_markers.h"
#include "visitor/pass_ufcs.h"
#include "visitor/pass.h"
#include "visitor/text_span.h"
#include "visitor/visitor.h"
#include "visitor/visitor_fileutil.h"
#include "visitor/walk.h"


/* Helper: reparse rewritten source to get updated stub AST (file-based for correctness) */
static CCASTRoot* cc__reparse_after_rewrite(const char* rewritten_src, size_t rewritten_len,
                                           const char* input_path, CCSymbolTable* symbols,
                                           char** tmp_path_out) {
    /* Use file-based path for reparse - the prelude and path handling is complex */
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
        if (!getenv("CC_KEEP_REPARSE")) unlink(pp_path);
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
    if (src_ufcs) {
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

    /* PASS 7: Rewrite @link("lib") to marker comments for linker extraction */
    char* link_rewritten = cc__rewrite_link_directives(src_ufcs, src_ufcs_len);
    if (link_rewritten) {
        if (src_ufcs != src_all) free(src_ufcs);
        src_ufcs = link_rewritten;
        src_ufcs_len = strlen(link_rewritten);
    }

    /* NOTE: slice move/provenance checking is now handled by the stub-AST checker pass
       (`cc/src/visitor/checker.c`) before visitor lowering. */

    /* Emit CC headers and helpers */
    fprintf(out, "/* CC visitor: passthrough of lowered C (preprocess + TCC parse) */\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include <ccc/cc_nursery.cch>\n");
    fprintf(out, "#include <ccc/cc_closure.cch>\n");
    fprintf(out, "#include <ccc/cc_slice.cch>\n");
    fprintf(out, "#include <ccc/cc_runtime.cch>\n");
    fprintf(out, "#include <ccc/std/task_intptr.cch>\n");
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

    /* Note: most CC lowering is now handled via dedicated passes; this prelude exists only to keep reparses parseable. */

    /* Emit container type declarations from type registry */
    {
        CCTypeRegistry* reg = cc_type_registry_get_global();
        if (reg) {
            size_t n_opt = cc_type_registry_optional_count(reg);
            size_t n_vec = cc_type_registry_vec_count(reg);
            size_t n_map = cc_type_registry_map_count(reg);
            
            if (n_opt > 0 || n_vec > 0 || n_map > 0) {
                fprintf(out, "/* --- CC generic container declarations --- */\n");
                fprintf(out, "#include <ccc/std/vec.cch>\n");
                fprintf(out, "#include <ccc/std/map.cch>\n");
                /* Vec/Map/Optional declarations must be skipped in parser mode where they're
                   already typedef'd to generic placeholders in the headers */
                fprintf(out, "#ifndef CC_PARSER_MODE\n");
                
                /* Emit optional type declarations */
                for (size_t i = 0; i < n_opt; i++) {
                    const CCTypeInstantiation* inst = cc_type_registry_get_optional(reg, i);
                    if (inst && inst->type1 && inst->mangled_name) {
                        fprintf(out, "CC_DECL_OPTIONAL(%s, %s)\n", inst->mangled_name, inst->type1);
                    }
                }
                
                /* Emit Vec declarations */
                for (size_t i = 0; i < n_vec; i++) {
                    const CCTypeInstantiation* inst = cc_type_registry_get_vec(reg, i);
                    if (inst && inst->type1 && inst->mangled_name) {
                        /* Extract mangled element name from Vec_xxx */
                        const char* mangled_elem = inst->mangled_name + 4; /* Skip "Vec_" */
                        
                        /* Skip Vec_char - it's predeclared in string.cch */
                        if (strcmp(mangled_elem, "char") == 0) {
                            continue;
                        }
                        
                        /* Check if type is complex (pointer, struct) - needs FULL macro */
                        int is_complex = (strchr(inst->type1, '*') != NULL || 
                                          strncmp(inst->type1, "struct ", 7) == 0 ||
                                          strncmp(inst->type1, "union ", 6) == 0);
                        if (is_complex) {
                            int opt_predeclared = (strcmp(mangled_elem, "charptr") == 0 ||
                                                   strcmp(mangled_elem, "intptr") == 0 ||
                                                   strcmp(mangled_elem, "voidptr") == 0);
                            if (!opt_predeclared) {
                                fprintf(out, "CC_DECL_OPTIONAL(CCOptional_%s, %s)\n", mangled_elem, inst->type1);
                            }
                            fprintf(out, "CC_VEC_DECL_ARENA_FULL(%s, %s, CCOptional_%s)\n", 
                                    inst->type1, inst->mangled_name, mangled_elem);
                        } else {
                            fprintf(out, "CC_VEC_DECL_ARENA(%s, %s)\n", inst->type1, inst->mangled_name);
                        }
                    }
                }
                
                /* Emit Map declarations (using default hash functions for known types) */
                for (size_t i = 0; i < n_map; i++) {
                    const CCTypeInstantiation* inst = cc_type_registry_get_map(reg, i);
                    if (inst && inst->type1 && inst->type2 && inst->mangled_name) {
                        /* Determine hash/eq functions based on key type */
                        const char* hash_fn = "cc_kh_hash_i32";
                        const char* eq_fn = "cc_kh_eq_i32";
                        if (strcmp(inst->type1, "int") == 0) {
                            hash_fn = "cc_kh_hash_i32"; eq_fn = "cc_kh_eq_i32";
                        } else if (strstr(inst->type1, "64") != NULL) {
                            hash_fn = "cc_kh_hash_u64"; eq_fn = "cc_kh_eq_u64";
                        } else if (strstr(inst->type1, "slice") != NULL || strcmp(inst->type1, "charslice") == 0) {
                            hash_fn = "cc_kh_hash_slice"; eq_fn = "cc_kh_eq_slice";
                        }
                        fprintf(out, "CC_MAP_DECL_ARENA(%s, %s, %s, %s, %s)\n", 
                                inst->type1, inst->type2, inst->mangled_name, hash_fn, eq_fn);
                    }
                }
                
                fprintf(out, "#endif /* !CC_PARSER_MODE */\n");
                fprintf(out, "/* --- end container declarations --- */\n\n");
            }
        }
    }

    /* Preserve diagnostics mapping to the original input (repo-relative for readability). */
    {
        char rel[1024];
        fprintf(out, "#line 1 \"%s\"\n", cc_path_rel_to_repo(src_path, rel, sizeof(rel)));
    }

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

/* (cleanup) cc__node_file_matches_this_tu was used by earlier versions of the pipeline; removed as unused. */