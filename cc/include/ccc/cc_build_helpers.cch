/*
 * Lightweight helpers for build-time scripts and comptime helpers.
 * Intended for future `build.cc` C code running inside `@comptime` to manipulate
 * paths, simple key/value data, and flag strings without pulling in heavy deps.
 */
#ifndef CC_BUILD_HELPERS_H
#define CC_BUILD_HELPERS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Join `base` + "/" + `rel` into `out` (null-terminated). */
int cc_build_join_paths(char* out, size_t cap, const char* base, const char* rel);

/* Convert a relative path into a normalized stem (slashes → '_', drop extension). */
int cc_build_make_stem(char* out, size_t cap, const char* rel_path);

/* Read the first `key=value` entry from a simple file and return the value. */
int cc_build_read_kv_pair(const char* path, const char* key, char* out, size_t cap);

/* Append `extra` to `base` with a single space, trimming whitespace. */
int cc_build_expand_flag(char* out, size_t cap, const char* base, const char* extra);

#ifdef __cplusplus
}
#endif

#endif // CC_BUILD_HELPERS_H
