#include "cparse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void dirname_of(const char *path, char *out, size_t cap) {
    const char *s;
    size_t n;
    if (!path || !out || cap == 0) return;
    s = strrchr(path, '/');
    if (!s) {
        snprintf(out, cap, ".");
        return;
    }
    if (s == path) {
        snprintf(out, cap, "/");
        return;
    }
    n = (size_t)(s - path);
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = 0;
}

static void usage(void) {
    fprintf(stderr,
            "usage: cparse-dump [-Dname] [--preserve] [--evaluate] [--tokens] [--expand] [--fields] file\n");
}

static char *read_all(const char *path, int *out_len) {
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    size_t cap = 0, n = 0;
    int c;
    if (!f) return NULL;
    while ((c = fgetc(f)) != EOF) {
        if (n + 1 >= cap) {
            size_t nc = cap ? cap * 2 : 4096;
            char *nb = (char *)realloc(buf, nc);
            if (!nb) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
            cap = nc;
        }
        buf[n++] = (char)c;
    }
    fclose(f);
    if (!buf) {
        buf = (char *)malloc(1);
        if (!buf) return NULL;
        buf[0] = 0;
        *out_len = 0;
        return buf;
    }
    buf[n] = 0;
    *out_len = (int)n;
    return buf;
}

int main(int argc, char **argv) {
    CpParse p;
    CpEnv env;
    const char *path = NULL;
    int want_p = 0, want_e = 0, want_t = 0, want_x = 0, want_f = 0;
    int i;
    char *text = NULL;
    size_t n = 0;

    cpenv_init(&env);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--preserve") == 0) {
            want_p = 1;
        } else if (strcmp(argv[i], "--evaluate") == 0) {
            want_e = 1;
        } else if (strcmp(argv[i], "--tokens") == 0) {
            want_t = 1;
        } else if (strcmp(argv[i], "--expand") == 0) {
            want_x = 1;
        } else if (strcmp(argv[i], "--fields") == 0) {
            want_f = 1;
        } else if (strncmp(argv[i], "-D", 2) == 0) {
            const char *name = argv[i] + 2;
            char tmp[128];
            char *eq;
            if (*name == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "cparse-dump: -D needs a name\n");
                    cpenv_free(&env);
                    return 2;
                }
                name = argv[++i];
            }
            strncpy(tmp, name, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = 0;
            eq = strchr(tmp, '=');
            if (eq) {
                *eq = 0;
                if (!cpenv_define_body(&env, tmp, eq + 1)) {
                    fprintf(stderr, "cparse-dump: oom\n");
                    cpenv_free(&env);
                    return 2;
                }
            } else if (!cpenv_define(&env, tmp)) {
                fprintf(stderr, "cparse-dump: oom\n");
                cpenv_free(&env);
                return 2;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "cparse-dump: unknown option %s\n", argv[i]);
            usage();
            cpenv_free(&env);
            return 2;
        } else if (!path) {
            path = argv[i];
        } else {
            usage();
            cpenv_free(&env);
            return 2;
        }
    }
    if (!path) {
        usage();
        cpenv_free(&env);
        return 2;
    }
    {
        static char file_dir[1024];
        dirname_of(path, file_dir, sizeof(file_dir));
        env.file_dir = file_dir;
    }
    if (!want_p && !want_e && !want_t && !want_x && !want_f) {
        want_p = 1;
        want_e = 1;
    }

    if (want_x) {
        char *raw;
        int rlen = 0;
        char *bytes = NULL;
        int blen = 0;
        CpTok *toks = NULL, *xtoks = NULL;
        int ntoks = 0, nxt = 0;
        char *arena = NULL, *xerr = NULL;
        raw = read_all(path, &rlen);
        if (!raw) {
            fprintf(stderr, "cparse-dump: cannot read %s\n", path);
            cpenv_free(&env);
            return 1;
        }
        if (cparse_lex_bytes(raw, rlen, &bytes, &blen, &toks, &ntoks) != 0) {
            fprintf(stderr, "cparse-dump: lex failed\n");
            free(raw);
            cpenv_free(&env);
            return 1;
        }
        if (cparse_expand(bytes, blen, toks, ntoks, &env, &xtoks, &nxt, &arena,
                          &xerr, env.file_dir) != 0) {
            fprintf(stderr, "cparse-dump: expand: %s\n",
                    xerr ? xerr : "failed");
            free(xerr);
            free(toks);
            free(bytes);
            free(raw);
            cpenv_free(&env);
            return 1;
        }
        if (cparse_dump_tokens(bytes, xtoks, nxt, &text, &n) != 0) {
            fprintf(stderr, "cparse-dump: expand dump failed\n");
            free(xtoks);
            free(arena);
            free(toks);
            free(bytes);
            free(raw);
            cpenv_free(&env);
            return 1;
        }
        if (want_p || want_e || want_t) fputs("=== expand ===\n", stdout);
        fwrite(text, 1, n, stdout);
        free(text);
        text = NULL;
        free(xtoks);
        free(arena);
        free(toks);
        free(bytes);
        free(raw);
        if (!want_p && !want_e && !want_t && !want_f) {
            cpenv_free(&env);
            return 0;
        }
    }

    if (want_t) {
        char *raw;
        int rlen = 0;
        char *bytes = NULL;
        int blen = 0;
        CpTok *toks = NULL;
        int ntoks = 0;
        raw = read_all(path, &rlen);
        if (!raw) {
            fprintf(stderr, "cparse-dump: cannot read %s\n", path);
            cpenv_free(&env);
            return 1;
        }
        if (cparse_lex_bytes(raw, rlen, &bytes, &blen, &toks, &ntoks) != 0) {
            fprintf(stderr, "cparse-dump: lex failed\n");
            free(raw);
            cpenv_free(&env);
            return 1;
        }
        if (cparse_dump_tokens(bytes, toks, ntoks, &text, &n) != 0) {
            fprintf(stderr, "cparse-dump: token dump failed\n");
            free(toks);
            free(bytes);
            free(raw);
            cpenv_free(&env);
            return 1;
        }
        if (want_p || want_e || want_f) fputs("=== tokens ===\n", stdout);
        fwrite(text, 1, n, stdout);
        free(text);
        text = NULL;
        free(toks);
        free(bytes);
        free(raw);
        if (!want_p && !want_e && !want_f) {
            cpenv_free(&env);
            return 0;
        }
    }

    if (want_f) {
        char *raw;
        int rlen = 0;
        char *bytes = NULL;
        int blen = 0;
        CpTok *toks = NULL;
        int ntoks = 0;
        int lo = -1, hi = -1, ti, nflat = 0;
        CpFlat flat[64];
        char ferr[192];
        raw = read_all(path, &rlen);
        if (!raw) {
            fprintf(stderr, "cparse-dump: cannot read %s\n", path);
            cpenv_free(&env);
            return 1;
        }
        if (cparse_lex_bytes(raw, rlen, &bytes, &blen, &toks, &ntoks) != 0) {
            fprintf(stderr, "cparse-dump: lex failed\n");
            free(raw);
            cpenv_free(&env);
            return 1;
        }
        {
            int depth = 0;
            for (ti = 0; ti < ntoks; ti++) {
                const char *sp =
                    toks[ti].ptr ? toks[ti].ptr : bytes + toks[ti].offset;
                if (toks[ti].kind != CP_TOK_PUNCT || toks[ti].len != 1) continue;
                if (sp[0] == '{') {
                    if (lo < 0) lo = ti + 1;
                    else depth++;
                } else if (sp[0] == '}' && lo >= 0) {
                    if (depth == 0) {
                        hi = ti;
                        break;
                    }
                    depth--;
                }
            }
        }
        if (lo < 0 || hi < lo) {
            fprintf(stderr, "cparse-dump: no struct field span\n");
            free(toks);
            free(bytes);
            free(raw);
            cpenv_free(&env);
            return 1;
        }
        ferr[0] = 0;
        if (cparse_flat_fields(bytes, blen, toks + lo, hi - lo, flat, 64,
                               &nflat, ferr, (int)sizeof(ferr)) != 0) {
            fprintf(stderr, "cparse-dump: fields: %s\n",
                    ferr[0] ? ferr : "failed");
            free(toks);
            free(bytes);
            free(raw);
            cpenv_free(&env);
            return 1;
        }
        if (want_p || want_e || want_t) fputs("=== fields ===\n", stdout);
        for (ti = 0; ti < nflat; ti++) {
            const char *k = (flat[ti].kind == CP_FLAT_PPDIR) ? "ppdir"
                             : (flat[ti].kind == CP_FLAT_FUNC) ? "func"
                                                              : "field";
            printf("%s %s %s\n", k, flat[ti].name, flat[ti].text);
        }
        free(toks);
        free(bytes);
        free(raw);
        if (!want_p && !want_e) {
            cpenv_free(&env);
            return 0;
        }
    }

    memset(&p, 0, sizeof(p));
    if (cparse_file(path, &p) != 0) {
        fprintf(stderr, "%s:%d:%d: %s\n", path,
                p.err.line ? p.err.line : 1, p.err.col ? p.err.col : 1,
                p.err.msg ? p.err.msg : "parse failed");
        cparse_free(&p);
        cpenv_free(&env);
        return 1;
    }

    if (want_p) {
        if (cparse_dump_preserve(p.root, &text, &n) != 0) {
            fprintf(stderr, "cparse-dump: preserve dump failed\n");
            cparse_free(&p);
            cpenv_free(&env);
            return 1;
        }
        if (want_e || want_t) fputs("=== preserve ===\n", stdout);
        fwrite(text, 1, n, stdout);
        if (n && text[n - 1] != '\n') fputc('\n', stdout);
        free(text);
        text = NULL;
    }

    if (want_e) {
        cparse_evaluate(p.root, &env, 1);
        if (cparse_dump_evaluate(p.root, &text, &n) != 0) {
            fprintf(stderr, "cparse-dump: evaluate dump failed\n");
            cparse_free(&p);
            cpenv_free(&env);
            return 1;
        }
        if (want_p || want_t) fputs("=== evaluate ===\n", stdout);
        fwrite(text, 1, n, stdout);
        free(text);
    }

    cparse_free(&p);
    cpenv_free(&env);
    return 0;
}
