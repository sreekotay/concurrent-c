/*
 * cc/src/comptime/dylib_cache.c
 *
 * See dylib_cache.h for the rationale. This file implements the content-
 * addressed cache with an FNV-1a 64-bit key over (tu_src, compile_cmd).
 */

#include "dylib_cache.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------------- */
/* Local helpers (FNV-1a hash, mkdir -p). Kept static to avoid symbol clash  */
/* with the similarly-named helpers in cc_main.c.                            */
/* ------------------------------------------------------------------------- */

static uint64_t cc__dc_fnv1a64_update(uint64_t h, const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t cc__dc_fnv1a64_str(uint64_t h, const char* s) {
    if (!s) return h;
    return cc__dc_fnv1a64_update(h, s, strlen(s));
}

static int cc__dc_mkdir_one(const char* path) {
    if (!path || !path[0]) return -1;
    if (mkdir(path, 0777) == -1) {
        if (errno == EEXIST) return 0;
        return -1;
    }
    return 0;
}

static int cc__dc_mkdir_p(const char* path) {
    if (!path || !path[0]) return -1;
    char tmp[1024];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) return -1;
    memcpy(tmp, path, n + 1);
    while (n > 0 && tmp[n - 1] == '/') { tmp[n - 1] = '\0'; n--; }
    if (n == 0) return 0;
    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (cc__dc_mkdir_one(tmp) != 0) return -1;
            *p = '/';
        }
    }
    return cc__dc_mkdir_one(tmp);
}

/* Writable? Simple heuristic: a probe file in the directory. */
static int cc__dc_dir_writable(const char* dir) {
    char probe[1024];
    if (!dir || !dir[0]) return 0;
    if (snprintf(probe, sizeof(probe), "%s/.cache_probe_%ld",
                 dir, (long)getpid()) >= (int)sizeof(probe)) return 0;
    int fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return 0;
    close(fd);
    unlink(probe);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

void cc_comptime_dl_module_free(void* owner) {
    CCComptimeDlModule* module = (CCComptimeDlModule*)owner;
    if (!module) return;
    if (module->dl_handle) dlclose(module->dl_handle);
    if (!module->keep_on_free) {
        if (module->obj_path[0]) unlink(module->obj_path);
        if (module->dylib_path[0]) unlink(module->dylib_path);
    }
    free(module);
}

CCComptimeDlModule* cc_comptime_dylib_cache_load(
    const char* tu_src,
    CCComptimeCompileCmdFormatter format_cmd,
    const char* repo_root,
    const char* input_dir,
    char* err_buf, size_t err_sz) {

    if (!tu_src || !format_cmd) return NULL;

    /* Env opt-out. */
    const char* opt_out = getenv("CCC_NO_COMPTIME_CACHE");
    if (opt_out && opt_out[0] && opt_out[0] != '0') return NULL;

    /* Determine the cache root. Prefer `<repo_root>/out/ccc-cache/comptime`
     * so that a `make clean` naturally drops the cache; fall back to a
     * per-user tmp directory if no repo root is known. */
    char cache_dir[1024];
    if (repo_root && repo_root[0]) {
        if (snprintf(cache_dir, sizeof(cache_dir),
                     "%s/out/ccc-cache/comptime", repo_root) >= (int)sizeof(cache_dir)) {
            return NULL;
        }
    } else {
        if (snprintf(cache_dir, sizeof(cache_dir),
                     "/tmp/ccc-cache-comptime-%ld", (long)getuid()) >= (int)sizeof(cache_dir)) {
            return NULL;
        }
    }
    if (cc__dc_mkdir_p(cache_dir) != 0) {
        if (err_buf && err_sz) {
            snprintf(err_buf, err_sz, "cache: mkdir -p %s failed: %s",
                     cache_dir, strerror(errno));
        }
        return NULL;
    }
    if (!cc__dc_dir_writable(cache_dir)) {
        if (err_buf && err_sz) {
            snprintf(err_buf, err_sz, "cache: %s not writable", cache_dir);
        }
        return NULL;
    }

    /* Produce a "canonical" compile command for hashing. We format it with
     * placeholder paths so the hash doesn't drift due to the real (cache-
     * internal) paths we'll substitute later. The placeholders still cover
     * all -I/flag content, so any flag change invalidates the entry. */
    char canonical_cmd[4096];
    if (format_cmd(canonical_cmd, sizeof(canonical_cmd),
                   repo_root, input_dir,
                   "__CCC_CACHE_DYLIB__",
                   "__CCC_CACHE_SRC__") != 0) {
        if (err_buf && err_sz) {
            snprintf(err_buf, err_sz, "cache: failed to format canonical compile cmd");
        }
        return NULL;
    }

    /* Compute the content-addressed key. */
    uint64_t h = 1469598103934665603ULL;
    h = cc__dc_fnv1a64_str(h, tu_src);
    h = cc__dc_fnv1a64_update(h, "\0", 1);
    h = cc__dc_fnv1a64_str(h, canonical_cmd);

    char hash_hex[17];
    snprintf(hash_hex, sizeof(hash_hex), "%016llx", (unsigned long long)h);

    char dylib_path[1024];
    char src_path[1024];
    if (snprintf(dylib_path, sizeof(dylib_path), "%s/%s.dylib",
                 cache_dir, hash_hex) >= (int)sizeof(dylib_path)) return NULL;
    if (snprintf(src_path, sizeof(src_path), "%s/%s.c",
                 cache_dir, hash_hex) >= (int)sizeof(src_path)) return NULL;

    /* Fast path: cache hit. Validated by presence + non-empty size + a
     * successful dlopen. If dlopen fails (e.g. corrupt file from a past
     * crash mid-rename), fall through and rebuild. */
    struct stat st;
    if (stat(dylib_path, &st) == 0 && st.st_size > 0) {
        void* handle = dlopen(dylib_path, RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            CCComptimeDlModule* module = (CCComptimeDlModule*)calloc(1, sizeof(*module));
            if (!module) {
                dlclose(handle);
                return NULL;
            }
            module->dl_handle = handle;
            strncpy(module->obj_path, src_path, sizeof(module->obj_path) - 1);
            strncpy(module->dylib_path, dylib_path, sizeof(module->dylib_path) - 1);
            module->keep_on_free = 1;
            return module;
        }
        /* Corrupt entry: remove and rebuild. */
        unlink(dylib_path);
    }

    /* Cache miss: write source, compile to a tmp dylib, atomically rename. */
    {
        FILE* f = fopen(src_path, "w");
        if (!f) {
            if (err_buf && err_sz) {
                snprintf(err_buf, err_sz, "cache: cannot write %s: %s",
                         src_path, strerror(errno));
            }
            return NULL;
        }
        fputs(tu_src, f);
        fclose(f);
    }

    char tmp_dylib[1024];
    if (snprintf(tmp_dylib, sizeof(tmp_dylib), "%s.tmp.%ld",
                 dylib_path, (long)getpid()) >= (int)sizeof(tmp_dylib)) {
        return NULL;
    }

    char real_cmd[4096];
    if (format_cmd(real_cmd, sizeof(real_cmd),
                   repo_root, input_dir,
                   tmp_dylib, src_path) != 0) {
        if (err_buf && err_sz) {
            snprintf(err_buf, err_sz, "cache: failed to format compile cmd");
        }
        return NULL;
    }
    if (system(real_cmd) != 0) {
        unlink(tmp_dylib);
        if (err_buf && err_sz) {
            snprintf(err_buf, err_sz, "cache: compile failed");
        }
        /* Leave src_path behind so the user can inspect it on failure. */
        return NULL;
    }

    /* rename() is atomic within a filesystem; if a racing ccc process won,
     * it's already at dylib_path with equivalent content and we just drop
     * our tmp dylib. */
    if (rename(tmp_dylib, dylib_path) != 0) {
        unlink(tmp_dylib);
        if (stat(dylib_path, &st) != 0 || st.st_size == 0) {
            if (err_buf && err_sz) {
                snprintf(err_buf, err_sz, "cache: rename failed: %s", strerror(errno));
            }
            return NULL;
        }
    }

    void* handle = dlopen(dylib_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        if (err_buf && err_sz) {
            snprintf(err_buf, err_sz, "cache: dlopen failed: %s",
                     dlerror() ? dlerror() : "unknown");
        }
        return NULL;
    }

    CCComptimeDlModule* module = (CCComptimeDlModule*)calloc(1, sizeof(*module));
    if (!module) {
        dlclose(handle);
        return NULL;
    }
    module->dl_handle = handle;
    strncpy(module->obj_path, src_path, sizeof(module->obj_path) - 1);
    strncpy(module->dylib_path, dylib_path, sizeof(module->dylib_path) - 1);
    module->keep_on_free = 1;
    return module;
}
