#include "cccportable.h"

#include "preprocess/unit_header.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int cc__exists(const char* p) {
    struct stat st;
    return p && p[0] && stat(p, &st) == 0;
}

static int cc__is_dir(const char* p) {
    struct stat st;
    return p && p[0] && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int cc__mkdir_p(const char* path) {
    char buf[PATH_MAX];
    size_t n;
    size_t i;
    if (!path || !path[0]) return -1;
    n = strlen(path);
    if (n >= sizeof(buf)) return -1;
    memcpy(buf, path, n + 1);
    for (i = 1; i < n; i++) {
        if (buf[i] != '/') continue;
        buf[i] = '\0';
        if (buf[0] && mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
        buf[i] = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int cc__copy_file(const char* src, const char* dst) {
    FILE* in;
    FILE* out;
    char buf[64 * 1024];
    size_t n;
    if (!src || !dst) return -1;
    in = fopen(src, "rb");
    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }
    fclose(in);
    if (fclose(out) != 0) return -1;
    return 0;
}

static int cc__dir_empty(const char* path) {
    DIR* d = opendir(path);
    struct dirent* de;
    if (!d) return 0;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        closedir(d);
        return 0;
    }
    closedir(d);
    return 1;
}

static int cc__rm_rf(const char* path) {
    char cmd[PATH_MAX * 2];
    if (!path || !path[0]) return -1;
    if (strcmp(path, "/") == 0 || strcmp(path, ".") == 0 ||
        strcmp(path, "..") == 0)
        return -1;
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", path);
    return system(cmd) == 0 ? 0 : -1;
}

static int cc__ends_with(const char* s, const char* suf) {
    size_t n, m;
    if (!s || !suf) return 0;
    n = strlen(s);
    m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

static int cc__runtime_is_raw(const char* runtime_c) {
    if (!runtime_c) return 1;
    if (strstr(runtime_c, "/out/runtime/")) return 0;
    if (strstr(runtime_c, "/lib/ccc/runtime/")) return 0;
    if (strstr(runtime_c, "/cc/runtime/")) return 1;
    return 0;
}

static int cc__copy_h_tree(const char* src, const char* dst) {
    DIR* d;
    struct dirent* de;
    if (cc__mkdir_p(dst) != 0) return -1;
    d = opendir(src);
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        char from[PATH_MAX], to[PATH_MAX];
        struct stat st;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (snprintf(from, sizeof(from), "%s/%s", src, de->d_name) >=
                (int)sizeof(from) ||
            snprintf(to, sizeof(to), "%s/%s", dst, de->d_name) >=
                (int)sizeof(to)) {
            closedir(d);
            return -1;
        }
        if (stat(from, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (cc__copy_h_tree(from, to) != 0) {
                closedir(d);
                return -1;
            }
        } else if (S_ISREG(st.st_mode) && cc__ends_with(de->d_name, ".h")) {
            if (cc__copy_file(from, to) != 0) {
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    return 0;
}

static int cc__copy_tree(const char* src, const char* dst) {
    DIR* d;
    struct dirent* de;
    if (cc__mkdir_p(dst) != 0) return -1;
    d = opendir(src);
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        char from[PATH_MAX], to[PATH_MAX];
        struct stat st;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (snprintf(from, sizeof(from), "%s/%s", src, de->d_name) >=
                (int)sizeof(from) ||
            snprintf(to, sizeof(to), "%s/%s", dst, de->d_name) >=
                (int)sizeof(to)) {
            closedir(d);
            return -1;
        }
        if (lstat(from, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (cc__copy_tree(from, to) != 0) {
                closedir(d);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (cc__copy_file(from, to) != 0) {
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    return 0;
}

static void cc__dirname(const char* path, char* out, size_t cap) {
    const char* sl;
    size_t n;
    if (!path || !out || !cap) return;
    sl = strrchr(path, '/');
    if (!sl || sl == path) {
        snprintf(out, cap, sl == path ? "/" : ".");
        return;
    }
    n = (size_t)(sl - path);
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

static int cc__seterr(char* err, size_t cap, const char* msg) {
    if (err && cap) snprintf(err, cap, "%s", msg ? msg : "error");
    return -1;
}

int cc_portable_install(const char* dir,
                        const char* lowered_include,
                        const char* runtime_c,
                        const char* repo_root,
                        const char* version_line,
                        char* err, size_t err_cap) {
    char src_ccc[PATH_MAX];
    char src_rt[PATH_MAX];
    char dst_inc[PATH_MAX];
    char dst_rt[PATH_MAX];
    char stamp[PATH_MAX];
    char have_stamp[PATH_MAX];
    FILE* f;
    if (!dir || !dir[0])
        return cc__seterr(err, err_cap, "portable-install requires a directory");
    if (strcmp(dir, "/") == 0 || strcmp(dir, ".") == 0 || strcmp(dir, "..") == 0)
        return cc__seterr(err, err_cap, "portable-install: refuse that directory");
    if (!lowered_include || !lowered_include[0] || !runtime_c || !runtime_c[0])
        return cc__seterr(err, err_cap, "portable-install: toolchain paths unset");
    snprintf(src_ccc, sizeof(src_ccc), "%s/ccc", lowered_include);
    if (!cc__is_dir(src_ccc))
        return cc__seterr(err, err_cap,
                          "portable-install: lowered include/ccc missing");
    if (!cc__exists(runtime_c))
        return cc__seterr(err, err_cap,
                          "portable-install: runtime concurrent_c.c missing");
    if (cc__runtime_is_raw(runtime_c))
        return cc__seterr(err, err_cap,
                          "portable-install: refuse raw cc/runtime (need rewritten out/runtime or prefix lib/ccc/runtime)");
    cc__dirname(runtime_c, src_rt, sizeof(src_rt));
    if (cc__exists(dir) && !cc__is_dir(dir))
        return cc__seterr(err, err_cap, "portable-install: DIR is not a directory");
    snprintf(have_stamp, sizeof(have_stamp), "%s/CCCPORTABLE.txt", dir);
    if (cc__exists(dir) && !cc__dir_empty(dir) && !cc__exists(have_stamp))
        return cc__seterr(err, err_cap,
                          "portable-install: DIR is occupied and has no CCCPORTABLE.txt");
    if (cc__exists(dir)) {
        if (cc__rm_rf(dir) != 0)
            return cc__seterr(err, err_cap, "portable-install: cannot replace DIR");
    }
    if (cc__mkdir_p(dir) != 0)
        return cc__seterr(err, err_cap, "portable-install: cannot create DIR");
    snprintf(dst_inc, sizeof(dst_inc), "%s/include/ccc", dir);
    snprintf(dst_rt, sizeof(dst_rt), "%s/runtime", dir);
    if (cc__copy_h_tree(src_ccc, dst_inc) != 0)
        return cc__seterr(err, err_cap, "portable-install: copy include/ccc/*.h failed");
    if (cc__copy_tree(src_rt, dst_rt) != 0)
        return cc__seterr(err, err_cap, "portable-install: copy runtime failed");
    if (repo_root && repo_root[0]) {
        char lic[PATH_MAX], dstl[PATH_MAX];
        snprintf(lic, sizeof(lic), "%s/LICENSE-MIT", repo_root);
        snprintf(dstl, sizeof(dstl), "%s/LICENSE-MIT", dir);
        if (cc__exists(lic)) (void)cc__copy_file(lic, dstl);
        snprintf(lic, sizeof(lic), "%s/LICENSE-APACHE", repo_root);
        snprintf(dstl, sizeof(dstl), "%s/LICENSE-APACHE", dir);
        if (cc__exists(lic)) (void)cc__copy_file(lic, dstl);
    }
    snprintf(stamp, sizeof(stamp), "%s/CCCPORTABLE.txt", dir);
    f = fopen(stamp, "w");
    if (!f) return cc__seterr(err, err_cap, "portable-install: cannot write stamp");
    fprintf(f, "%s\n", version_line ? version_line : "ccc");
    fclose(f);
    return 0;
}

int cc_portable_check_tree(const char* dir, const char* version_line,
                           char* err, size_t err_cap) {
    char inc[PATH_MAX], rt[PATH_MAX], stamp[PATH_MAX];
    FILE* f;
    char line[256];
    if (!dir || !dir[0])
        return cc__seterr(err, err_cap, "--cccportable requires a directory");
    snprintf(inc, sizeof(inc), "%s/include/ccc", dir);
    snprintf(rt, sizeof(rt), "%s/runtime/concurrent_c.c", dir);
    if (!cc__is_dir(inc) || !cc__exists(rt))
        return cc__seterr(err, err_cap,
                          "--cccportable: missing include/ccc or runtime/concurrent_c.c");
    snprintf(stamp, sizeof(stamp), "%s/CCCPORTABLE.txt", dir);
    if (!cc__exists(stamp)) return 0;
    f = fopen(stamp, "r");
    if (!f) return cc__seterr(err, err_cap, "--cccportable: cannot read stamp");
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return cc__seterr(err, err_cap, "--cccportable: empty stamp");
    }
    fclose(f);
    {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
    }
    if (version_line && strcmp(line, version_line) != 0)
        return cc__seterr(err, err_cap,
                          "--cccportable: stamp does not match this ccc --version");
    return 0;
}

void cc_portable_print_cflags(const char* dir) {
    printf("-I%s/include\n", dir);
}

void cc_portable_print_libs(const char* dir) {
    printf("-DCC_ENABLE_ASYNC %s/runtime/concurrent_c.c -lpthread -lm\n", dir);
}

int cc_take_cccportable_flag(int argc, char** argv, int* i,
                             const char** dir_out, int* cli_set) {
    const char* a;
    if (!argv || !i || *i >= argc) return 0;
    a = argv[*i];
    if (strcmp(a, "--cccportable") == 0) {
        if (*i + 1 >= argc) {
            fprintf(stderr, "cc: --cccportable requires a directory\n");
            return -1;
        }
        *dir_out = argv[++(*i)];
        if (cli_set) *cli_set = 1;
        return 1;
    }
    if (strncmp(a, "--cccportable=", 14) == 0) {
        *dir_out = a + 14;
        if (cli_set) *cli_set = 1;
        return 1;
    }
    return 0;
}

static size_t cc__skip_ws(const char* s, size_t n, size_t i) {
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
        i++;
    return i;
}

static size_t cc__skip_ws_comments(const char* s, size_t n, size_t i) {
    for (;;) {
        i = cc__skip_ws(s, n, i);
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '/') {
            i += 2;
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            if (i + 1 < n) i += 2;
            continue;
        }
        return i;
    }
}

static int cc__match(const char* s, size_t n, size_t i, const char* lit) {
    size_t m = strlen(lit);
    return i + m <= n && memcmp(s + i, lit, m) == 0;
}

int cc_file_start_pragmas(const char* src, size_t n,
                          int* prelude_off, int* linenumbers_off,
                          int* per_tu,
                          char* err, size_t err_cap) {
    size_t i;
    if (prelude_off) *prelude_off = 0;
    if (linenumbers_off) *linenumbers_off = 0;
    if (per_tu) *per_tu = 0;
    if (!src) return 0;
    i = cc_unit_header_skip(src, n);
    i = cc__skip_ws_comments(src, n, i);
    while (i < n && src[i] == '#') {
        const char* q = src + i;
        size_t rest;
        int which = 0;
        if (!cc__match(src, n, i, "#pragma")) break;
        q += 7;
        while (*q == ' ' || *q == '\t') q++;
        rest = n - (size_t)(q - src);
        if (rest >= 10 && memcmp(q, "(@prelude)", 10) == 0) {
            which = 1;
            q += 10;
        } else if (rest >= 14 && memcmp(q, "(@linenumbers)", 14) == 0) {
            which = 2;
            q += 14;
        } else if (rest >= 9 && memcmp(q, "(@per_tu)", 9) == 0) {
            which = 3;
            q += 9;
        } else {
            break;
        }
        while (*q == ' ' || *q == '\t') q++;
        if (which == 3) {
            if (*q && *q != '\n' && *q != '\r' &&
                !(*q == '/' && q[1] == '/')) {
                return cc__seterr(err, err_cap,
                                  "#pragma(@per_tu) takes no operand");
            }
            if (per_tu) *per_tu = 1;
        } else {
            if (!(q[0] == 'o' && q[1] == 'f' && q[2] == 'f')) {
                return cc__seterr(err, err_cap,
                                  which == 1
                                      ? "#pragma(@prelude) takes 'off'"
                                      : "#pragma(@linenumbers) takes 'off'");
            }
            q += 3;
            if (*q && *q != '\n' && *q != '\r' && *q != ' ' && *q != '\t') {
                return cc__seterr(err, err_cap,
                                  which == 1
                                      ? "#pragma(@prelude) takes 'off'"
                                      : "#pragma(@linenumbers) takes 'off'");
            }
            while (*q == ' ' || *q == '\t') q++;
            if (*q && *q != '\n' && *q != '\r' &&
                !(*q == '/' && q[1] == '/')) {
                return cc__seterr(err, err_cap,
                                  which == 1
                                      ? "#pragma(@prelude) takes 'off'"
                                      : "#pragma(@linenumbers) takes 'off'");
            }
            if (which == 1 && prelude_off) *prelude_off = 1;
            if (which == 2 && linenumbers_off) *linenumbers_off = 1;
        }
        while (i < n && src[i] != '\n') i++;
        if (i < n) i++;
        i = cc__skip_ws_comments(src, n, i);
    }
    return 0;
}

int cc_emit_polish_c(const char* path, const char* version,
                     int prelude_off, int no_line) {
    FILE* f;
    char* src = NULL;
    long sz;
    char* out = NULL;
    size_t cap, len = 0;
    size_t i;
    const char* ver = version && version[0] ? version : "?";
    char banner[256];
    int nban;
    if (!path) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    src = (char*)malloc((size_t)sz + 1);
    if (!src) { fclose(f); return -1; }
    if (fread(src, 1, (size_t)sz, f) != (size_t)sz) {
        free(src);
        fclose(f);
        return -1;
    }
    fclose(f);
    src[sz] = '\0';
    cap = (size_t)sz + 128;
    out = (char*)malloc(cap);
    if (!out) { free(src); return -1; }
    nban = snprintf(banner, sizeof(banner),
                    "/* generated by Concurrent-C (ccc / shadow_lower) %s */\n",
                    ver);
    if (nban < 0) { free(src); free(out); return -1; }
    memcpy(out, banner, (size_t)nban);
    len = (size_t)nban;
    i = 0;
    if (src[0] == '/' && src[1] == '*') {
        i = 2;
        while (i + 1 < (size_t)sz && !(src[i] == '*' && src[i + 1] == '/')) i++;
        if (i + 1 < (size_t)sz) i += 2;
        if (i < (size_t)sz && src[i] == '\n') i++;
    }
    if (prelude_off) {
        int stripped = 1;
        while (stripped) {
            stripped = 0;
            if (i + 20 <= (size_t)sz &&
                memcmp(src + i, "#include <stddef.h>\n", 20) == 0) {
                i += 20;
                stripped = 1;
            } else if (i + 20 <= (size_t)sz &&
                       memcmp(src + i, "#include <stdint.h>\n", 20) == 0) {
                i += 20;
                stripped = 1;
            } else if (i + 20 <= (size_t)sz &&
                       memcmp(src + i, "#include <stdlib.h>\n", 20) == 0) {
                i += 20;
                stripped = 1;
            }
        }
        if (i < (size_t)sz && src[i] == '\n') i++;
    }
    while (i < (size_t)sz) {
        size_t line_a = i;
        size_t line_b;
        int drop = 0;
        while (i < (size_t)sz && src[i] != '\n') i++;
        line_b = i;
        if (i < (size_t)sz) i++;
        if (no_line) {
            const char* p = src + line_a;
            while (p < src + line_b && (*p == ' ' || *p == '\t')) p++;
            if (p + 5 <= src + line_b && memcmp(p, "#line", 5) == 0)
                drop = 1;
            if (p + 8 <= src + line_b && memcmp(p, "/*CC_LN ", 8) == 0)
                drop = 1;
        }
        if (drop) continue;
        {
            size_t n = (i > line_a) ? (i - line_a) : 0;
            if (len + n + 1 > cap) {
                char* nbuf;
                cap = (len + n + 1) * 2;
                nbuf = (char*)realloc(out, cap);
                if (!nbuf) { free(src); free(out); return -1; }
                out = nbuf;
            }
            memcpy(out + len, src + line_a, n);
            len += n;
        }
    }
    free(src);
    f = fopen(path, "wb");
    if (!f) { free(out); return -1; }
    if (fwrite(out, 1, len, f) != len) { fclose(f); free(out); return -1; }
    fclose(f);
    free(out);
    return 0;
}

int cc_path_under_dir(const char* path, const char* dir) {
    char ap[PATH_MAX], ad[PATH_MAX];
    size_t nd;
    if (!path || !dir || !path[0] || !dir[0]) return 0;
    if (!realpath(path, ap) || !realpath(dir, ad)) return 0;
    nd = strlen(ad);
    if (strncmp(ap, ad, nd) != 0) return 0;
    return ap[nd] == '\0' || ap[nd] == '/';
}
