#ifndef CC_UTIL_PATH_H
#define CC_UTIL_PATH_H

#include <stddef.h>

/* Best-effort: return a path relative to the repository root.
   - Repo root is detected by walking up from `path` until `cc/src/cc_main.c` exists.
   - If `path` is not under the repo root (or root cannot be found), returns the basename.
   - The returned pointer is always `out` (null-terminated). */
const char* cc_path_rel_to_repo(const char* path, char* out, size_t out_cap);

#endif /* CC_UTIL_PATH_H */

