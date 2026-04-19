#ifndef CC_PREPROCESS_H
#define CC_PREPROCESS_H

#include <stddef.h>

typedef struct CCSymbolTable CCSymbolTable;

// Preprocess a CC source file, rewriting CC syntax (e.g., UFCS) into
// plain C that TCC can parse. Writes to a temporary file path (returned via
// out_path), nul-terminated. Returns 0 on success; caller must unlink the
// temp file when done.
int cc_preprocess_file(const char* input_path, char* out_path, size_t out_path_sz);

// Preprocess source string to output string (no temp files).
// Returns malloc'd string on success, NULL on error. Caller must free().
char* cc_preprocess_to_string(const char* input, size_t input_len, const char* input_path);

// Same as cc_preprocess_to_string but with option to skip validation checks.
// Use skip_checks=1 for reparse passes where checks already ran on original source.
char* cc_preprocess_to_string_ex(const char* input, size_t input_len, const char* input_path, int skip_checks);

// Expand a source file through the host C preprocessor so local and stdlib
// headers appear in one include-expanded stream with line markers preserved.
// Returns malloc'd string on success, NULL on error. Caller must free().
char* cc_preprocess_include_expanded(const char* input_path);

// Phase 1: build the canonical CC source buffer consumed by comptime.
//
// This is intentionally NOT the final lowered-C form. Callers should treat the
// returned buffer as the normalized CC program that phase 2 ("execute
// comptime") will inspect/evaluate before any host-C-facing lowering happens.
//
// Current implementation: include-expanded CC source with line markers
// preserved. This is a transitional substrate; the abstraction exists so we
// can later replace it with a richer canonical preprocessed-CC form without
// changing parser/codegen callers.
//
// Returns malloc'd string on success, NULL on error. Caller must free().
char* cc_preprocess_comptime_source(const char* input_path);

// Rewrite quoted local .cch includes to stable lowered .h files under
// out/include/ so parser and final host C compilation do not see raw project
// headers with CC-only syntax such as @comptime blocks. Returns malloc'd
// string on success or NULL when no rewrite was needed. Caller must free().
char* cc_rewrite_local_cch_includes_to_lowered_headers(const char* src,
                                                       size_t input_len,
                                                       const char* input_path);
char* cc_rewrite_system_cch_includes_to_lowered_headers(const char* src,
                                                        size_t input_len);

// Shared header-safe type-syntax lowering used by both preprocessing and
// `.cch -> .h` lowering. Rewrites syntax that must not leak into plain C
// headers, such as slice, typed channel handles, and generic container types.
char* cc_rewrite_header_type_syntax_shared(const char* src,
                                           size_t input_len,
                                           const char* input_path);

// Simple preprocessing for the experimental AST/codegen path: only adds #line directive, no CC syntax rewrites.
// All CC syntax (try, await, closures, etc.) is handled by TCC hooks and AST passes.
// Returns malloc'd string on success, NULL on error. Caller must free().
char* cc_preprocess_simple(const char* input, size_t input_len, const char* input_path);

// Rewrite `@slice(...)`, `@string(...)`, and backtick template literals in a
// source fragment used by later text-based lowering/codegen passes.
char* cc_rewrite_string_templates_text(const char* src, size_t n, const char* input_path);

// Rewrite @link("lib") directives to marker comments for linker extraction.
// Returns newly allocated string, or NULL if no rewrites needed.
char* cc__rewrite_link_directives(const char* src, size_t n);

// Rewrite generic container syntax:
//   CCVec<T> -> CCVec_T, Map<K,V> -> Map_K_V
//   cc_vec_new<T>(&arena) -> CCVec_T_init(&arena, CC_VEC_INITIAL_CAP)
//   map_new<K,V>(&arena) -> Map_K_V_init(&arena)
// Returns newly allocated string with rewrites, or NULL if no changes.
char* cc_rewrite_generic_containers(const char* src, size_t n, const char* input_path);

// Parser-only text rewrite for concrete generic family UFCS. The AST UFCS
// pass is authoritative for all family UFCS; this rewriter remains as a
// narrow parser-survival aid so the stub-AST parser sees lowered receiver
// forms for fragile nested contexts (Vec methods inside printf args,
// CCCommand/CCFile calls recorded inconsistently by TCC, etc.).
char* cc_rewrite_generic_family_ufcs_parser_safe(const char* src, size_t n);

// [Removed] cc_rewrite_channel_ufcs_concrete: channel UFCS is now handled by
// the AST UFCS pass via the CCChanTx/CCChanRx registered hooks.  The AST
// dispatch also retains a small cc_ufcs_channel_callee helper for the raw
// CCChan/untyped-alias cases that still need sizeof(*out_ptr) insertion.

// Prototype rewrite for builtin nursery declarations:
//   CCNursery* n = @create(parent, closure) @destroy;
//   CCNursery* n = @create(parent, closure) @destroy { ... };
// Returns newly allocated string on change, NULL on no-op, (char*)-1 on error.
char* cc_rewrite_nursery_create_destroy_proto(const char* src, size_t n, const char* input_path);
char* cc_rewrite_nursery_create_destroy_proto_ex(const char* src,
                                                 size_t n,
                                                 const char* input_path,
                                                 CCSymbolTable* symbols);

#endif // CC_PREPROCESS_H

