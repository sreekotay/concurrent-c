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

// Rewrite UFCS method calls on container variables:
//   v.push(x) -> Vec_int_push(&v, x)
//   m.get(k)  -> Map_K_V_get(&m, k)
// Relies on type registry being populated by cc_rewrite_generic_containers.
// Returns newly allocated string with rewrites, or NULL if no changes.
char* cc_rewrite_ufcs_container_calls(const char* src, size_t n, const char* input_path);

// Rewrite std_out.write()/std_err.write() UFCS patterns to cc_std_out_write() etc.
// These are synthetic receivers that don't exist as real variables.
// Returns newly allocated string with rewrites, or NULL if no changes.
char* cc_rewrite_std_io_ufcs(const char* src, size_t n);

#endif // CC_PREPROCESS_H

