#ifndef CC_UTIL_PATH_H
#define CC_UTIL_PATH_H

#include <stddef.h>

/* Best-effort: return a path relative to the repository root.
   - Repo root is detected by walking up from `path` until `cc/src/cc_main.c` exists.
   - If `path` is not under the repo root (or root cannot be found), returns the basename.
   - The returned pointer is always `out` (null-terminated). */
const char* cc_path_rel_to_repo(const char* path, char* out, size_t out_cap);
int cc_path_find_repo_root(const char* path, char* out, size_t out_cap);

/* Resolve an angle-include path like `ccc/script/js.cch` to an absolute
 * readable file.  Searches CC_INCLUDE_PATH (set by ccc for both checkout
 * `cc/include` and prefix `include`), then `$repo/cc/include`, then
 * `$CC_HOME/include`.  Returns 1 and fills `out` on success. */
int cc_path_resolve_system_cch(const char* rel, char* out, size_t out_cap);

#endif /* CC_UTIL_PATH_H */

