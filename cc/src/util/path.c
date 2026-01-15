#include "path.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static int cc__find_repo_root_from(const char* any_path, char* out_root, size_t cap) {
    if (!any_path || !out_root || cap == 0) return 0;
    out_root[0] = 0;

    char absbuf[PATH_MAX];
    absbuf[0] = 0;
    if (realpath(any_path, absbuf) == NULL) {
        /* If realpath fails (should be rare), just work with the provided string. */
        strncpy(absbuf, any_path, sizeof(absbuf) - 1);
        absbuf[sizeof(absbuf) - 1] = 0;
    }

    char dir[PATH_MAX];
    strncpy(dir, absbuf, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    cc__dirname_inplace(dir);
    if (!dir[0]) return 0;

    for (int depth = 0; depth < 20; depth++) {
        char marker[PATH_MAX];
        snprintf(marker, sizeof(marker), "%s/cc/src/cc_main.c", dir);
        if (cc__file_exists(marker)) {
            strncpy(out_root, dir, cap - 1);
            out_root[cap - 1] = 0;
            return 1;
        }
        /* ascend */
        if (strcmp(dir, "/") == 0) break;
        cc__dirname_inplace(dir);
        if (!dir[0]) break;
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
        size_t rn = strlen(g_root);
        const char* rel = absbuf + rn;
        if (*rel == '/') rel++;
        if (!*rel) rel = ".";
        strncpy(out, rel, out_cap - 1);
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

