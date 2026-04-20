/*
 * cc/src/comptime/dylib_cache.h
 *
 * Content-addressed cache for comptime helper dylibs.
 *
 * Several compile-time extension points in ccc (UFCS handler lowering,
 * @comptime type hooks) follow the same pattern:
 *
 *     1. Build a self-contained C translation unit `tu_src` as a string.
 *     2. Write it to a temp file.
 *     3. Shell out to `cc -dynamiclib ...` to produce a .dylib.
 *     4. dlopen the dylib and dlsym the entry symbol.
 *     5. On teardown, dlclose and unlink both files.
 *
 * This module hoists that sequence behind a content-addressed cache so that
 * identical `(tu_src, compile_cmd)` pairs compile clang exactly once per
 * machine, no matter how many times ccc is invoked.
 *
 * Correctness / staleness:
 *   - The cache key is a hash of `tu_src` itself (the fully preprocessed TU
 *     handed to clang) plus the exact compile command string. Any change in
 *     the prelude/runtime headers or in ccc's codegen that affects the
 *     emitted TU produces a different key; any change in compiler flags
 *     produces a different key. There is no path by which stale bytes
 *     survive a relevant change.
 *   - Atomic rename on install makes concurrent ccc invocations race-safe.
 *   - Users can disable the cache entirely with `CCC_NO_COMPTIME_CACHE=1`.
 */

#ifndef CC_COMPTIME_DYLIB_CACHE_H
#define CC_COMPTIME_DYLIB_CACHE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A loaded comptime helper dylib, owned by whoever called into the cache.
 *
 * When `keep_on_free` is non-zero, `obj_path` and `dylib_path` live in the
 * persistent content-addressed cache and must NOT be unlinked on teardown.
 * Otherwise they are scratch files created by the legacy per-invocation
 * fallback path and get unlinked.
 */
typedef struct CCComptimeDlModule {
    void* dl_handle;
    char obj_path[1024];
    char dylib_path[1024];
    int keep_on_free;
} CCComptimeDlModule;

/* dlclose and (conditionally) unlink. Safe to call on NULL. */
void cc_comptime_dl_module_free(void* owner);

/*
 * Callback type for formatting the clang compile command line.
 *
 * Implementations receive absolute paths for `dylib_path` and `source_path`
 * and must produce a `system()`-compatible command in `out`, returning 0 on
 * success and -1 on failure (e.g. buffer too small).
 *
 * The command is BOTH hashed (to key the cache) AND executed when a miss
 * occurs. Callbacks must therefore be deterministic given their inputs.
 */
typedef int (*CCComptimeCompileCmdFormatter)(char* out, size_t out_sz,
                                             const char* repo_root,
                                             const char* input_dir,
                                             const char* dylib_path,
                                             const char* source_path);

/*
 * Try to load a comptime helper dylib for the given TU from the persistent
 * cache, building it once if absent.
 *
 * Returns:
 *   - A newly allocated CCComptimeDlModule* on success. The caller owns it
 *     and must eventually call cc_comptime_dl_module_free(). dl_handle is
 *     already dlopen()'d; the caller is responsible for dlsym()ing its
 *     entry symbol(s). keep_on_free is set to 1.
 *   - NULL if caching is disabled (`CCC_NO_COMPTIME_CACHE=1`), if the
 *     cache root is not writable, or if the compile itself failed. The
 *     caller should fall back to the legacy per-invocation compile path.
 *
 * `repo_root` is used both for cache placement (cache lives at
 * `<repo_root>/out/ccc-cache/comptime/`) and as an input to the compile
 * command formatter. `input_dir` is passed through to the formatter and
 * does not affect the cache location.
 */
CCComptimeDlModule* cc_comptime_dylib_cache_load(
    const char* tu_src,
    CCComptimeCompileCmdFormatter format_cmd,
    const char* repo_root,
    const char* input_dir,
    char* err_buf, size_t err_sz);

#ifdef __cplusplus
}
#endif

#endif /* CC_COMPTIME_DYLIB_CACHE_H */
