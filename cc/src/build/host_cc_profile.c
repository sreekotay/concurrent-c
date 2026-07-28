#include "build/host_cc_profile.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <limits.h>
#else
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#endif

static int cc__hp_mkdir_p(const char* path) {
    char tmp[PATH_MAX];
    size_t n;
    if (!path || !path[0]) return -1;
    strncpy(tmp, path, sizeof(tmp));
    tmp[sizeof(tmp) - 1] = '\0';
    n = strlen(tmp);
    while (n > 0 && tmp[n - 1] == '/') {
        tmp[--n] = '\0';
    }
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) == -1 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) == -1 && errno != EEXIST) return -1;
    return 0;
}

static int cc__hp_ends_with(const char* s, const char* suf) {
    size_t n, m;
    if (!s || !suf) return 0;
    n = strlen(s);
    m = strlen(suf);
    return n >= m && memcmp(s + n - m, suf, m) == 0;
}

static int cc__hp_is_tcc_name(const char* cc_bin) {
    if (!cc_bin) return 0;
    if (cc__hp_ends_with(cc_bin, "tcc")) return 1;
    if (cc__hp_ends_with(cc_bin, "tcc.exe")) return 1;
    if (strstr(cc_bin, "/tcc") || strstr(cc_bin, "\\tcc")) return 1;
    return 0;
}

static const char* cc__hp_tcc_lib_dir(const char* cc_bin,
                                      const char* repo_root,
                                      char* buf,
                                      size_t cap) {
    const char* cands[8];
    size_t nc = 0;
    char from_bin[PATH_MAX];
    const char* env = getenv("CC_TCC_LIB_PATH");
    if (env && env[0]) cands[nc++] = env;
#ifdef CC_TCC_LIB_DIR
    cands[nc++] = CC_TCC_LIB_DIR;
#endif
    if (cc_bin && cc_bin[0]) {
        size_t n = strlen(cc_bin);
        if (n + 1 < sizeof(from_bin)) {
            memcpy(from_bin, cc_bin, n + 1);
            char* slash = strrchr(from_bin, '/');
            if (slash && slash != from_bin) {
                *slash = '\0';
                cands[nc++] = from_bin;
            }
        }
    }
    if (repo_root && repo_root[0]) {
        static char abs_cand[PATH_MAX];
        if (snprintf(abs_cand, sizeof(abs_cand), "%s/third_party/tcc", repo_root)
            < (int)sizeof(abs_cand))
            cands[nc++] = abs_cand;
    }
    cands[nc++] = "third_party/tcc";
    cands[nc++] = "../third_party/tcc";
    for (size_t i = 0; i < nc; i++) {
        char probe[PATH_MAX];
        if (!cands[i] || !cands[i][0]) continue;
        if (snprintf(probe, sizeof(probe), "%s/include/stdbool.h", cands[i])
            >= (int)sizeof(probe))
            continue;
        if (access(probe, R_OK) == 0) {
            if (snprintf(buf, cap, "%s", cands[i]) < (int)cap) return buf;
        }
    }
    return NULL;
}

static uint64_t cc__hp_fnv1a64(const void* data, size_t n, uint64_t h) {
    const unsigned char* p = (const unsigned char*)data;
    size_t i;
    if (!h) h = 14695981039346656037ull;
    for (i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* Resolve a host CC to an executable path. Bare names like "cc" must not use
 * realpath() alone — in this repo that resolves to the `cc/` directory. */
static int cc__hp_resolve_cc(const char* cc_bin, char* out, size_t cap) {
    char trial[PATH_MAX];
    struct stat st;
    if (!cc_bin || !cc_bin[0] || !out || !cap) return -1;
    out[0] = '\0';
    if (strchr(cc_bin, '/')) {
        if (realpath(cc_bin, trial) && stat(trial, &st) == 0 && S_ISREG(st.st_mode)
            && access(trial, X_OK) == 0) {
            strncpy(out, trial, cap);
            out[cap - 1] = '\0';
            return 0;
        }
        strncpy(out, cc_bin, cap);
        out[cap - 1] = '\0';
        return 0;
    }
    /* Search PATH for an executable file (not a directory). */
    {
        const char* path = getenv("PATH");
        char pathbuf[4096];
        char* save = NULL;
        char* dir;
        if (!path || !path[0]) path = "/usr/bin:/bin";
        strncpy(pathbuf, path, sizeof(pathbuf) - 1);
        pathbuf[sizeof(pathbuf) - 1] = '\0';
        for (dir = strtok_r(pathbuf, ":", &save); dir;
             dir = strtok_r(NULL, ":", &save)) {
            if (snprintf(trial, sizeof(trial), "%s/%s", dir, cc_bin) >= (int)sizeof(trial))
                continue;
            if (stat(trial, &st) == 0 && S_ISREG(st.st_mode) && access(trial, X_OK) == 0) {
                if (realpath(trial, out)) return 0;
                strncpy(out, trial, cap);
                out[cap - 1] = '\0';
                return 0;
            }
        }
    }
    strncpy(out, cc_bin, cap);
    out[cap - 1] = '\0';
    return 0;
}

static int cc__hp_fingerprint(const char* cc_bin, char* hex_out, size_t hex_cap,
                              char* resolved_out, size_t resolved_cap) {
    char resolved[PATH_MAX];
    struct stat st;
    uint64_t h;
    if (!cc_bin || !cc_bin[0] || !hex_out || hex_cap < 17) return -1;
    if (cc__hp_resolve_cc(cc_bin, resolved, sizeof(resolved)) != 0) return -1;
    if (resolved_out && resolved_cap) {
        strncpy(resolved_out, resolved, resolved_cap);
        resolved_out[resolved_cap - 1] = '\0';
    }
    memset(&st, 0, sizeof(st));
    (void)stat(resolved, &st);
    h = cc__hp_fnv1a64(resolved, strlen(resolved), 0);
    h = cc__hp_fnv1a64(&st.st_mtime, sizeof(st.st_mtime), h);
    h = cc__hp_fnv1a64(&st.st_size, sizeof(st.st_size), h);
    {
        int schema = CC_HOST_PROFILE_SCHEMA;
        h = cc__hp_fnv1a64(&schema, sizeof(schema), h);
    }
    snprintf(hex_out, hex_cap, "%016llx", (unsigned long long)h);
    return 0;
}

static int cc__hp_write_src(const char* path, const char* src) {
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    if (fputs(src, f) < 0) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

/* Compile `src_path` to a throwaway .o with `cc_bin` + `flags`. 0 = success. */
static int cc__hp_try_compile(const char* cc_bin, const char* flags,
                              const char* src_path, const char* obj_path) {
    char cmd[4096];
    int rc;
    snprintf(cmd, sizeof(cmd), "%s%s -c %s -o %s >/dev/null 2>&1",
             cc_bin, flags ? flags : "", src_path, obj_path);
    rc = system(cmd);
    return rc == 0 ? 0 : -1;
}

static int cc__hp_load(const char* path, CCHostCcProfile* out) {
    FILE* f;
    char line[1280];
    int schema = -1;
    if (!path || !out) return -1;
    memset(out, 0, sizeof(*out));
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strncmp(line, "schema=", 7) == 0) schema = atoi(line + 7);
        else if (strncmp(line, "cc=", 3) == 0) {
            strncpy(out->cc_path, line + 3, sizeof(out->cc_path) - 1);
        } else if (strncmp(line, "flags=", 6) == 0) {
            strncpy(out->flags, line + 6, sizeof(out->flags) - 1);
        } else if (strncmp(line, "is_tcc=", 7) == 0) out->is_tcc = atoi(line + 7);
        else if (strncmp(line, "no_liblfds=", 11) == 0) out->no_liblfds = atoi(line + 11);
        else if (strncmp(line, "no_xjb_float=", 13) == 0)
            out->no_xjb_float = atoi(line + 13);
        else if (strncmp(line, "ok=", 3) == 0) out->ok = atoi(line + 3);
    }
    fclose(f);
    if (schema != CC_HOST_PROFILE_SCHEMA || !out->ok) return -1;
    return 0;
}

static int cc__hp_save(const char* path, const CCHostCcProfile* p) {
    FILE* f;
    char tmp[PATH_MAX];
    if (!path || !p) return -1;
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "schema=%d\n", CC_HOST_PROFILE_SCHEMA);
    fprintf(f, "cc=%s\n", p->cc_path);
    fprintf(f, "flags=%s\n", p->flags);
    fprintf(f, "is_tcc=%d\n", p->is_tcc);
    fprintf(f, "no_liblfds=%d\n", p->no_liblfds);
    fprintf(f, "no_xjb_float=%d\n", p->no_xjb_float);
    fprintf(f, "ok=%d\n", p->ok);
    fclose(f);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int cc__hp_probe(const char* cc_bin,
                        const char* repo_root,
                        const char* work_dir,
                        CCHostCcProfile* out) {
    char tcc_dir[PATH_MAX];
    char src_basic[PATH_MAX], src_c11[PATH_MAX], src_atomic[PATH_MAX];
    char obj[PATH_MAX];
    char flags[512];
    const char* libdir;
    int need_std = 0;
    int need_B = 0;

    memset(out, 0, sizeof(*out));
    strncpy(out->cc_path, cc_bin, sizeof(out->cc_path) - 1);
    out->is_tcc = cc__hp_is_tcc_name(cc_bin);

    snprintf(src_basic, sizeof(src_basic), "%s/hp_basic.c", work_dir);
    snprintf(src_c11, sizeof(src_c11), "%s/hp_c11.c", work_dir);
    snprintf(src_atomic, sizeof(src_atomic), "%s/hp_atomic.c", work_dir);
    snprintf(obj, sizeof(obj), "%s/hp_probe.o", work_dir);

    if (cc__hp_write_src(src_basic,
                         "#include <stdbool.h>\nint main(void){return 0;}\n")
        != 0)
        return -1;
    if (cc__hp_write_src(src_c11,
                         "#include <stddef.h>\n"
                         "int main(void){return (int)sizeof(max_align_t);}\n")
        != 0)
        return -1;
    if (cc__hp_write_src(src_atomic,
                         "#include <stdint.h>\n"
                         "#include <stdatomic.h>\n"
                         "int main(void){\n"
                         "  _Atomic uint64_t x = 0;\n"
                         "  uint64_t e = 0;\n"
                         "  return atomic_compare_exchange_strong(&x,&e,1) ? 0 : 1;\n"
                         "}\n")
        != 0)
        return -1;

    flags[0] = '\0';
    if (cc__hp_try_compile(cc_bin, flags, src_basic, obj) != 0) {
        need_B = 1;
        need_std = 1;
    } else if (cc__hp_try_compile(cc_bin, flags, src_c11, obj) != 0) {
        need_std = 1;
    }

    if (need_std) strncat(flags, " -std=c11", sizeof(flags) - strlen(flags) - 1);
    if (need_B || out->is_tcc) {
        libdir = cc__hp_tcc_lib_dir(cc_bin, repo_root, tcc_dir, sizeof(tcc_dir));
        if (libdir) {
            char bflag[PATH_MAX + 4];
            snprintf(bflag, sizeof(bflag), " -B%s", libdir);
            strncat(flags, bflag, sizeof(flags) - strlen(flags) - 1);
            need_B = 1;
        }
    }

    /* Re-validate with discovered flags. */
    if (cc__hp_try_compile(cc_bin, flags, src_basic, obj) != 0) {
        fprintf(stderr,
                "cc: host CC probe failed for '%s' (cannot compile with"
                " stdbool.h). flags='%s'\n",
                cc_bin, flags);
        return -1;
    }
    if (cc__hp_try_compile(cc_bin, flags, src_c11, obj) != 0) {
        fprintf(stderr,
                "cc: host CC probe failed for '%s' (need C11 max_align_t)."
                " flags='%s'\n",
                cc_bin, flags);
        return -1;
    }

    /* Atomics: if C11 stdatomic CAS fails, treat as no-liblfds / weak host. */
    if (cc__hp_try_compile(cc_bin, flags, src_atomic, obj) != 0) {
        out->no_liblfds = 1;
    } else if (out->is_tcc) {
        /* TCC has stdatomic but no liblfds PAL. */
        out->no_liblfds = 1;
        out->no_xjb_float = 1;
    }

    if (need_B || need_std) out->is_tcc = out->is_tcc || need_B;

    strncpy(out->flags, flags, sizeof(out->flags) - 1);
    out->ok = 1;

    unlink(src_basic);
    unlink(src_c11);
    unlink(src_atomic);
    unlink(obj);
    return 0;
}

int cc_host_cc_profile_ensure(const char* cc_bin,
                              const char* cache_root,
                              const char* repo_root,
                              CCHostCcProfile* out) {
    static CCHostCcProfile cached;
    static char cached_bin[1024];
    static int have_cached = 0;

    char hex[32];
    char resolved[PATH_MAX];
    char host_dir[PATH_MAX];
    char path[PATH_MAX];
    char work[PATH_MAX];
    const char* reprobe;

    if (!cc_bin || !cc_bin[0] || !out) return -1;

    if (have_cached && strcmp(cached_bin, cc_bin) == 0) {
        *out = cached;
        return 0;
    }

    if (cc__hp_fingerprint(cc_bin, hex, sizeof(hex), resolved, sizeof(resolved)) != 0)
        return -1;

    if (!cache_root || !cache_root[0]) cache_root = "out/.cc-build";
    snprintf(host_dir, sizeof(host_dir), "%s/host", cache_root);
    snprintf(path, sizeof(path), "%s/%s.profile", host_dir, hex);
    snprintf(work, sizeof(work), "%s/probe-%d", host_dir, (int)getpid());

    reprobe = getenv("CC_REPROBE_HOST");
    if (!(reprobe && reprobe[0] && reprobe[0] != '0')) {
        if (cc__hp_load(path, out) == 0 && strcmp(out->cc_path, resolved) == 0) {
            strncpy(cached_bin, cc_bin, sizeof(cached_bin) - 1);
            cached = *out;
            have_cached = 1;
            return 0;
        }
    }

    if (cc__hp_mkdir_p(host_dir) != 0) return -1;
    if (cc__hp_mkdir_p(work) != 0) return -1;

    if (cc__hp_probe(resolved, repo_root, work, out) != 0) {
        rmdir(work);
        return -1;
    }
    strncpy(out->cc_path, resolved, sizeof(out->cc_path) - 1);
    (void)cc__hp_save(path, out);
    rmdir(work);

    strncpy(cached_bin, cc_bin, sizeof(cached_bin) - 1);
    cached = *out;
    have_cached = 1;
    return 0;
}

void cc_host_cc_profile_append_flags(const CCHostCcProfile* p,
                                     char* cmd,
                                     size_t cmd_cap) {
    if (!p || !cmd || !cmd_cap || !p->flags[0]) return;
    strncat(cmd, p->flags, cmd_cap - strlen(cmd) - 1);
}

int cc_host_cc_fingerprint(const char* cc_bin, char* hex_out, size_t hex_cap) {
    return cc__hp_fingerprint(cc_bin, hex_out, hex_cap, NULL, 0);
}

int cc_host_cc_obj_root(const char* cc_bin,
                        const char* cache_root,
                        char* out,
                        size_t out_cap) {
    char hex[32];
    if (!out || out_cap == 0) return -1;
    if (cc__hp_fingerprint(cc_bin, hex, sizeof(hex), NULL, 0) != 0) return -1;
    if (!cache_root || !cache_root[0]) cache_root = "out/.cc-build";
    if (snprintf(out, out_cap, "%s/host/%s", cache_root, hex) >= (int)out_cap) return -1;
    return 0;
}
