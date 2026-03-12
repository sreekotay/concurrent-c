#ifndef CC_PREPROCESS_H
#define CC_PREPROCESS_H

#include <stddef.h>

// Preprocess a CC source file, rewriting CC syntax (e.g., @arena, UFCS) into
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

// Simple preprocessing for cccn: only adds #line directive, no CC syntax rewrites.
// All CC syntax (try, await, closures, etc.) is handled by TCC hooks and AST passes.
// Returns malloc'd string on success, NULL on error. Caller must free().
char* cc_preprocess_simple(const char* input, size_t input_len, const char* input_path);

// Rewrite @link("lib") directives to marker comments for linker extraction.
// Returns newly allocated string, or NULL if no rewrites needed.
char* cc__rewrite_link_directives(const char* src, size_t n);

// Rewrite generic container syntax:
//   Vec<T> -> Vec_T, Map<K,V> -> Map_K_V
//   vec_new<T>(&arena) -> Vec_T_init(&arena, CC_VEC_INITIAL_CAP)
//   map_new<K,V>(&arena) -> Map_K_V_init(&arena)
// Returns newly allocated string with rewrites, or NULL if no changes.
char* cc_rewrite_generic_containers(const char* src, size_t n, const char* input_path);

// Rewrite synthetic stdio receivers:
//   cc_std_out.write(x) / cc_std_err.write(x)
// These are not ordinary typed UFCS receivers, so they still lower through a
// small dedicated text pass in final codegen.
char* cc_rewrite_synthetic_std_io_receivers(const char* src, size_t n);

// Rewrite CCFile UFCS for parser/generated-code survival:
//   file.read(...) / file.write(...) / file.sync() / file.close()
// Final source should still prefer the AST-aware UFCS pass; this helper is for
// places where generated code introduces raw file UFCS after the main AST pass.
char* cc_rewrite_file_ufcs_survival(const char* src, size_t n);

// Rewrite CCResult_* UFCS for generated-code survival:
//   r.value() / r.error() / r.unwrap() / r.unwrap_err() / r.is_ok() / ...
// This is currently needed for generated closure bodies that bypass the main
// AST-aware UFCS sweep.
char* cc_rewrite_result_ufcs_survival(const char* src, size_t n);

// Rewrite concrete generic/container UFCS that survives into later parsing:
//   v.get(i) / v.pop() / m.get(k) / c.items.get(i) -> Vec_T_get(...) / Map_K_V_get(...)
// Used as a parser/codegen survival pass when concrete family receiver types are
// already visible in source text.
char* cc_rewrite_generic_family_ufcs_survival(const char* src, size_t n);

// Rewrite parser-style generic family types in final lowered source to concrete
// family calls:
//   __CC_VEC(int) v; v.get(1) -> Vec_int_get(&v, 1)
//   __CC_MAP(int,int)* m; m.get(1) -> Map_int_int_get(m, 1)
char* cc_rewrite_generic_family_ufcs_concrete(const char* src, size_t n);

#endif // CC_PREPROCESS_H

