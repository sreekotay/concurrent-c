#include "path.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

static int cc__file_exists(const char* p) {
    return (p && p[0] && access(p, F_OK) == 0);
}

static const char* cc__basename(const char* path) {
    if (!path) return NULL;
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

static void cc__dirname_inplace(char* path) {
    if (!path) return;
    size_t n = strlen(path);
    if (n == 0) return;
    while (n > 0 && path[n - 1] == '/') { path[n - 1] = 0; n--; }
    if (n == 0) return;
    char* slash = strrchr(path, '/');
    if (!slash) { path[0] = 0; return; }
    if (slash == path) { slash[1] = 0; return; } /* keep "/" */
    *slash = 0;
}

static int cc__starts_with_path(const char* p, const char* root) {
    if (!p || !root) return 0;
    size_t rn = strlen(root);
    if (rn == 0) return 0;
    if (strncmp(p, root, rn) != 0) return 0;
    return p[rn] == '/' || p[rn] == 0;
}

/* Walk up from `start_dir` looking for the repo marker. */
static int cc__walk_up_for_marker(const char* start_dir, char* out_root, size_t cap) {
    if (!start_dir || !start_dir[0] || !out_root || cap == 0) return 0;
    char dir[PATH_MAX];
    strncpy(dir, start_dir, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    for (int depth = 0; depth < 20; depth++) {
        char marker[PATH_MAX];
        snprintf(marker, sizeof(marker), "%s/cc/src/cc_main.c", dir);
        if (cc__file_exists(marker)) {
            strncpy(out_root, dir, cap - 1);
            out_root[cap - 1] = 0;
            return 1;
        }
        if (strcmp(dir, "/") == 0) break;
        cc__dirname_inplace(dir);
        if (!dir[0]) break;
    }
    return 0;
}

/* Best-effort path to the current executable.  Used so `ccc` can locate its
 * own repo (for header include paths) even when the input source lives
 * outside the tree.  Returns 1 on success, 0 otherwise. */
static int cc__self_exe_path(char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
#if defined(__APPLE__)
    char raw[PATH_MAX];
    uint32_t sz = (uint32_t)sizeof(raw);
    if (_NSGetExecutablePath(raw, &sz) != 0) return 0;
    char resolved[PATH_MAX];
    if (realpath(raw, resolved) == NULL) {
        strncpy(resolved, raw, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = 0;
    }
    strncpy(out, resolved, cap - 1);
    out[cap - 1] = 0;
    return out[0] ? 1 : 0;
#elif defined(__linux__)
    char resolved[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);
    if (n <= 0) return 0;
    resolved[n] = 0;
    strncpy(out, resolved, cap - 1);
    out[cap - 1] = 0;
    return 1;
#else
    return 0;
#endif
}

static int cc__find_repo_root_from(const char* any_path, char* out_root, size_t cap) {
    if (!out_root || cap == 0) return 0;
    out_root[0] = 0;

    /* 1. Walk up from `any_path` (the preferred source-local search). */
    if (any_path && any_path[0]) {
        char absbuf[PATH_MAX];
        absbuf[0] = 0;
        if (realpath(any_path, absbuf) == NULL) {
            strncpy(absbuf, any_path, sizeof(absbuf) - 1);
            absbuf[sizeof(absbuf) - 1] = 0;
        }
        char dir[PATH_MAX];
        strncpy(dir, absbuf, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = 0;
        cc__dirname_inplace(dir);
        if (dir[0] && cc__walk_up_for_marker(dir, out_root, cap)) return 1;
    }

    /* 2. Honor `$CC_REPO_ROOT` if it points at a real repo. */
    {
        const char* env = getenv("CC_REPO_ROOT");
        if (env && env[0]) {
            char marker[PATH_MAX];
            snprintf(marker, sizeof(marker), "%s/cc/src/cc_main.c", env);
            if (cc__file_exists(marker)) {
                strncpy(out_root, env, cap - 1);
                out_root[cap - 1] = 0;
                return 1;
            }
        }
    }

    /* 3. Last resort: locate repo by walking up from the `ccc` executable.
     *    The binary normally lives at `<repo>/cc/bin/ccc`, so its grandparent
     *    is the repo root.  This keeps out-of-tree compiles (e.g. `ccc run
     *    /tmp/foo.ccs`) working without any env-var setup. */
    {
        char exe[PATH_MAX];
        if (cc__self_exe_path(exe, sizeof(exe))) {
            cc__dirname_inplace(exe);
            if (exe[0] && cc__walk_up_for_marker(exe, out_root, cap)) return 1;
        }
    }

    return 0;
}

const char* cc_path_rel_to_repo(const char* path, char* out, size_t out_cap) {
    if (!out || out_cap == 0) return "";
    out[0] = 0;
    if (!path || !path[0]) {
        strncpy(out, "<input>", out_cap - 1);
        out[out_cap - 1] = 0;
        return out;
    }

    /* Cache the repo root after first successful detection. */
    static int g_root_init = 0;
    static char g_root[PATH_MAX];
    if (!g_root_init) {
        g_root[0] = 0;
        (void)cc__find_repo_root_from(path, g_root, sizeof(g_root));
        g_root_init = 1;
    }

    char absbuf[PATH_MAX];
    absbuf[0] = 0;
    if (realpath(path, absbuf) == NULL) {
        strncpy(absbuf, path, sizeof(absbuf) - 1);
        absbuf[sizeof(absbuf) - 1] = 0;
    }

    if (g_root[0] && cc__starts_with_path(absbuf, g_root)) {
        /* Use absolute path for #line directives so TCC error reporting works correctly
           regardless of the temp file's directory. */
        strncpy(out, absbuf, out_cap - 1);
        out[out_cap - 1] = 0;
        return out;
    }

    /* Not under repo root: use basename for readability. */
    const char* b = cc__basename(path);
    if (!b || !b[0]) b = path;
    strncpy(out, b, out_cap - 1);
    out[out_cap - 1] = 0;
    return out;
}

int cc_path_find_repo_root(const char* path, char* out, size_t out_cap) {
    return cc__find_repo_root_from(path, out, out_cap);
}

