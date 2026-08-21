#include "cparse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    CpTok *t;
    int n;
} TokList;

typedef struct {
    char *name;
    int is_func;
    char **params;
    int nparams;
    int variadic;
    CpTok *body;
    int nbody;
} Macro;

typedef struct {
    const char *src;
    int slen;
    Macro *macs;
    int nmac;
    int mcap;
    char *hide[64];
    int nhide;
    int parent[64];
    int taken[64];
    int saw_else[64];
    int ifn;
    int live;
    CpTok *out;
    int nout;
    int ocap;
    char *arena;
    size_t aused;
    size_t acap;
    char err[256];
    int depth;
    const char *file_dir;
} Ex;

static char *cp_strdup_err(const char *s) {
    size_t n = strlen(s);
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n + 1);
    return d;
}

static const char *tsp(const char *src, const CpTok *t, size_t *n) {
    if (t->ptr) {
        *n = t->len;
        return t->ptr;
    }
    *n = t->len;
    return src + t->offset;
}

static int teq(const char *src, const CpTok *t, const char *lit) {
    size_t n;
    const char *p = tsp(src, t, &n);
    size_t m = strlen(lit);
    return n == m && memcmp(p, lit, m) == 0;
}

static int at_bol(const char *src, size_t off) {
    size_t i = off;
    while (i > 0 && src[i - 1] != '\n') {
        char c = src[i - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\v' && c != '\f')
            return 0;
        i--;
    }
    return 1;
}

static int from_file(const char *src, const CpTok *t) {
    return src && t->ptr && t->ptr == src + t->offset;
}

static int hash_bol(const char *src, const CpTok *t) {
    return from_file(src, t) && t->kind == CP_TOK_PUNCT && t->len == 1 &&
           src[t->offset] == '#' && at_bol(src, t->offset);
}

static int line_end(const char *src, int slen, size_t off) {
    int e = (int)off;
    while (e < slen && src[e] != '\n') e++;
    return e;
}

static char *ex_str(Ex *ex, const char *s, size_t n) {
    char *d;
    if (ex->aused + n + 1 > ex->acap) {
        size_t nc = ex->acap ? ex->acap * 2 : 1024;
        char *nb;
        while (nc < ex->aused + n + 1) nc *= 2;
        nb = (char *)realloc(ex->arena, nc);
        if (!nb) return NULL;
        ex->arena = nb;
        ex->acap = nc;
    }
    d = ex->arena + ex->aused;
    memcpy(d, s, n);
    d[n] = 0;
    ex->aused += n + 1;
    return d;
}

static int emit(Ex *ex, CpTok t) {
    if (ex->nout >= ex->ocap) {
        int nc = ex->ocap ? ex->ocap * 2 : 64;
        CpTok *nb = (CpTok *)realloc(ex->out, (size_t)nc * sizeof(CpTok));
        if (!nb) return 0;
        ex->out = nb;
        ex->ocap = nc;
    }
    ex->out[ex->nout++] = t;
    return 1;
}

static int emit_copy(Ex *ex, const CpTok *t) {
    return emit(ex, *t);
}

static Macro *mac_find(Ex *ex, const char *name, size_t n) {
    int i;
    for (i = 0; i < ex->nmac; i++)
        if (strlen(ex->macs[i].name) == n &&
            memcmp(ex->macs[i].name, name, n) == 0)
            return &ex->macs[i];
    return NULL;
}

static int hidden(Ex *ex, const char *name, size_t n) {
    int i;
    for (i = 0; i < ex->nhide; i++)
        if (strlen(ex->hide[i]) == n && memcmp(ex->hide[i], name, n) == 0)
            return 1;
    return 0;
}

static void mac_clear(Macro *m) {
    int i;
    if (!m) return;
    free(m->name);
    for (i = 0; i < m->nparams; i++) free(m->params[i]);
    free(m->params);
    free(m->body);
    memset(m, 0, sizeof(*m));
}

static int mac_set(Ex *ex, Macro *src) {
    Macro *old = mac_find(ex, src->name, strlen(src->name));
    if (old) {
        mac_clear(old);
        *old = *src;
        return 1;
    }
    if (ex->nmac >= ex->mcap) {
        int nc = ex->mcap ? ex->mcap * 2 : 16;
        Macro *nb = (Macro *)realloc(ex->macs, (size_t)nc * sizeof(Macro));
        if (!nb) return 0;
        ex->macs = nb;
        ex->mcap = nc;
    }
    ex->macs[ex->nmac++] = *src;
    return 1;
}

static void mac_undef(Ex *ex, const char *name, size_t n) {
    Macro *m = mac_find(ex, name, n);
    int i;
    if (!m) return;
    i = (int)(m - ex->macs);
    mac_clear(m);
    if (i + 1 < ex->nmac) memmove(&ex->macs[i], &ex->macs[i + 1],
                                  (size_t)(ex->nmac - i - 1) * sizeof(Macro));
    ex->nmac--;
}

static int tok_defined(Ex *ex, const char *name, size_t n) {
    if ((n == 14 && memcmp(name, "__has_include", 14) == 0) ||
        (n == 14 && memcmp(name, "__has_feature", 14) == 0) ||
        (n == 14 && memcmp(name, "__has_builtin", 14) == 0))
        return 1;
    return mac_find(ex, name, n) != NULL;
}

static int expand_list(Ex *ex, const CpTok *in, int nin);

static int ident_is(const char *src, const CpTok *t, const char *lit) {
    return t->kind == CP_TOK_IDENT && teq(src, t, lit);
}

static int punct_is(const char *src, const CpTok *t, const char *lit) {
    return t->kind == CP_TOK_PUNCT && teq(src, t, lit);
}

static int collect_line(const char *src, int slen, const CpTok *in, int nin,
                        int i, int *line_hi) {
    *line_hi = line_end(src, slen, in[i].offset);
    while (i < nin && (int)in[i].offset < *line_hi) i++;
    return i;
}

static int param_index(const Macro *m, const char *src, const CpTok *t) {
    size_t n;
    const char *p;
    int i;
    if (t->kind != CP_TOK_IDENT) return -1;
    p = tsp(src, t, &n);
    if (m->variadic && n == 11 && memcmp(p, "__VA_ARGS__", 11) == 0)
        return m->nparams; /* va slot */
    for (i = 0; i < m->nparams; i++)
        if (strlen(m->params[i]) == n && memcmp(m->params[i], p, n) == 0)
            return i;
    return -1;
}

static int stringify(Ex *ex, const CpTok *args, int nargs, CpTok *out) {
    size_t cap = 32, n = 1;
    char *b = (char *)malloc(cap);
    int i;
    if (!b) return 0;
    b[0] = '"';
    for (i = 0; i < nargs; i++) {
        size_t tn;
        const char *p = tsp(ex->src, &args[i], &tn);
        size_t k;
        if (i && n + 1 >= cap) {
            cap *= 2;
            {
                char *nb = (char *)realloc(b, cap);
                if (!nb) {
                    free(b);
                    return 0;
                }
                b = nb;
            }
        }
        if (i) b[n++] = ' ';
        for (k = 0; k < tn; k++) {
            char c = p[k];
            if (n + 4 >= cap) {
                cap *= 2;
                {
                    char *nb = (char *)realloc(b, cap);
                    if (!nb) {
                        free(b);
                        return 0;
                    }
                    b = nb;
                }
            }
            if (c == '\\' || c == '"') b[n++] = '\\';
            b[n++] = c;
        }
    }
    if (n + 2 >= cap) {
        char *nb = (char *)realloc(b, n + 2);
        if (!nb) {
            free(b);
            return 0;
        }
        b = nb;
    }
    b[n++] = '"';
    b[n] = 0;
    {
        char *intern = ex_str(ex, b, n);
        free(b);
        if (!intern) return 0;
        out->kind = CP_TOK_STR;
        out->ptr = intern;
        out->len = n;
        out->offset = 0;
        out->file_id = 0;
    }
    return 1;
}

static int paste_pair(Ex *ex, const CpTok *left, const CpTok *right, CpTok *out) {
    size_t ln, rn;
    const char *lp = left ? tsp(ex->src, left, &ln) : "";
    const char *rp = right ? tsp(ex->src, right, &rn) : "";
    size_t n = (left ? ln : 0) + (right ? rn : 0);
    char *tmp = (char *)malloc(n + 1);
    CpTok *lexed = NULL;
    int nlex = 0;
    char *intern;
    if (!tmp) return 0;
    if (left) memcpy(tmp, lp, ln);
    if (right) memcpy(tmp + (left ? ln : 0), rp, rn);
    tmp[n] = 0;
    intern = ex_str(ex, tmp, n);
    free(tmp);
    if (!intern) return 0;
    if (n == 0) {
        out->kind = CP_TOK_IDENT;
        out->ptr = intern;
        out->len = 0;
        out->offset = 0;
        out->file_id = 0;
        return 1;
    }
    if (cparse_lex(intern, (int)n, &lexed, &nlex) == 0 && nlex == 1) {
        *out = lexed[0];
        out->ptr = intern;
        out->len = n;
        out->offset = 0;
        free(lexed);
        return 1;
    }
    free(lexed);
    out->kind = CP_TOK_IDENT;
    out->ptr = intern;
    out->len = n;
    out->offset = 0;
    out->file_id = 0;
    return 1;
}

static int expand_args(Ex *ex, const CpTok *a, int na, TokList *out) {
    CpTok *saved = ex->out;
    int sn = ex->nout, sc = ex->ocap;
    int ok;
    ex->out = NULL;
    ex->nout = 0;
    ex->ocap = 0;
    ok = expand_list(ex, a, na);
    out->t = ex->out;
    out->n = ex->nout;
    ex->out = saved;
    ex->nout = sn;
    ex->ocap = sc;
    return ok;
}

static int subst_body(Ex *ex, const Macro *m, TokList *args, int nargs,
                      TokList *out);

static int expand_macro(Ex *ex, const Macro *m, TokList *args, int nargs) {
    TokList subst = { NULL, 0 };
    char *nm;
    int ok;
    if (ex->nhide >= 64) {
        snprintf(ex->err, sizeof(ex->err), "macro hide stack overflow");
        return 0;
    }
    if (ex->depth++ > 256) {
        snprintf(ex->err, sizeof(ex->err), "macro expansion too deep");
        return 0;
    }
    if (!subst_body(ex, m, args, nargs, &subst)) {
        ex->depth--;
        return 0;
    }
    nm = m->name;
    ex->hide[ex->nhide++] = nm;
    ok = expand_list(ex, subst.t, subst.n);
    ex->nhide--;
    ex->depth--;
    free(subst.t);
    return ok;
}

static int subst_body(Ex *ex, const Macro *m, TokList *args, int nargs,
                      TokList *out) {
    CpTok *buf = NULL;
    int n = 0, cap = 0;
    int j;
    const char *src = ex->src;

    #define PUSH(T) do { \
        if (n >= cap) { \
            int nc = cap ? cap * 2 : 8; \
            CpTok *nb = (CpTok *)realloc(buf, (size_t)nc * sizeof(CpTok)); \
            if (!nb) { free(buf); return 0; } \
            buf = nb; cap = nc; \
        } \
        buf[n++] = (T); \
    } while (0)

    for (j = 0; j < m->nbody; j++) {
        int pi;
        if (punct_is(src, &m->body[j], "#") && j + 1 < m->nbody &&
            !punct_is(src, &m->body[j], "##")) {
            /* stringify: # must be a single #, not ## */
        }
        if (m->body[j].kind == CP_TOK_PUNCT && m->body[j].len == 1 &&
            teq(src, &m->body[j], "#") && j + 1 < m->nbody) {
            pi = param_index(m, src, &m->body[j + 1]);
            if (pi >= 0) {
                CpTok st;
                TokList *al = (pi == m->nparams) ? (nargs > m->nparams ? &args[m->nparams] : NULL)
                                                 : (pi < nargs ? &args[pi] : NULL);
                if (!stringify(ex, al ? al->t : NULL, al ? al->n : 0, &st)) {
                    free(buf);
                    return 0;
                }
                PUSH(st);
                j++;
                continue;
            }
        }
        if (j + 1 < m->nbody && punct_is(src, &m->body[j + 1], "##")) {
            CpTok left, right, pasted;
            int have_l = 0, have_r = 0;
            pi = param_index(m, src, &m->body[j]);
            if (pi >= 0) {
                TokList *al = (pi == m->nparams)
                                  ? (nargs > m->nparams ? &args[m->nparams] : NULL)
                                  : (pi < nargs ? &args[pi] : NULL);
                if (al && al->n > 0) {
                    int k;
                    for (k = 0; k + 1 < al->n; k++) PUSH(al->t[k]);
                    left = al->t[al->n - 1];
                    have_l = 1;
                }
            } else {
                left = m->body[j];
                have_l = 1;
            }
            if (j + 2 >= m->nbody) {
                snprintf(ex->err, sizeof(ex->err), "## missing right operand");
                free(buf);
                return 0;
            }
            pi = param_index(m, src, &m->body[j + 2]);
            if (pi >= 0) {
                TokList *al = (pi == m->nparams)
                                  ? (nargs > m->nparams ? &args[m->nparams] : NULL)
                                  : (pi < nargs ? &args[pi] : NULL);
                if (al && al->n > 0) {
                    right = al->t[0];
                    have_r = 1;
                    if (!paste_pair(ex, have_l ? &left : NULL, &right, &pasted)) {
                        free(buf);
                        return 0;
                    }
                    PUSH(pasted);
                    {
                        int k;
                        for (k = 1; k < al->n; k++) PUSH(al->t[k]);
                    }
                } else if (have_l) {
                    PUSH(left);
                }
            } else {
                right = m->body[j + 2];
                have_r = 1;
                if (!paste_pair(ex, have_l ? &left : NULL, have_r ? &right : NULL,
                                &pasted)) {
                    free(buf);
                    return 0;
                }
                PUSH(pasted);
            }
            j += 2;
            continue;
        }
        pi = param_index(m, src, &m->body[j]);
        if (pi >= 0) {
            TokList *al = (pi == m->nparams)
                              ? (nargs > m->nparams ? &args[m->nparams] : NULL)
                              : (pi < nargs ? &args[pi] : NULL);
            if (al && al->n) {
                TokList exp = { NULL, 0 };
                int k;
                if (!expand_args(ex, al->t, al->n, &exp)) {
                    free(buf);
                    return 0;
                }
                for (k = 0; k < exp.n; k++) PUSH(exp.t[k]);
                free(exp.t);
            }
            continue;
        }
        PUSH(m->body[j]);
    }
    #undef PUSH
    out->t = buf;
    out->n = n;
    return 1;
}

static int collect_call_args(const char *src, const CpTok *in, int nin, int *io,
                             TokList **out_args, int *out_n) {
    int i = *io;
    int depth = 0;
    int a0, narg = 0, acap = 0;
    TokList *args = NULL;
    if (i >= nin || !punct_is(src, &in[i], "(")) return 0;
    i++;
    a0 = i;
    depth = 1;
    if (i < nin && punct_is(src, &in[i], ")")) {
        *out_args = NULL;
        *out_n = 0;
        *io = i + 1;
        return 1;
    }
    while (i < nin && depth > 0) {
        if (punct_is(src, &in[i], "(")) depth++;
        else if (punct_is(src, &in[i], ")")) {
            depth--;
            if (depth == 0) {
                TokList one;
                one.t = (CpTok *)(void *)(in + a0);
                one.n = i - a0;
                if (narg >= acap) {
                    int nc = acap ? acap * 2 : 4;
                    TokList *nb = (TokList *)realloc(args, (size_t)nc * sizeof(TokList));
                    if (!nb) {
                        free(args);
                        return 0;
                    }
                    args = nb;
                    acap = nc;
                }
                args[narg++] = one;
                i++;
                break;
            }
        } else if (punct_is(src, &in[i], ",") && depth == 1) {
            TokList one;
            one.t = (CpTok *)(void *)(in + a0);
            one.n = i - a0;
            if (narg >= acap) {
                int nc = acap ? acap * 2 : 4;
                TokList *nb = (TokList *)realloc(args, (size_t)nc * sizeof(TokList));
                if (!nb) {
                    free(args);
                    return 0;
                }
                args = nb;
                acap = nc;
            }
            args[narg++] = one;
            i++;
            a0 = i;
            continue;
        }
        i++;
    }
    if (depth != 0) {
        free(args);
        return 0;
    }
    *out_args = args;
    *out_n = narg;
    *io = i;
    return 1;
}

static int ex_isdef(void *ctx, const char *name, size_t len) {
    return tok_defined((Ex *)ctx, name, len);
}

static int expand_from(Ex *ex, const CpTok *in, int nin, int *io);

static int is_has_kw(const char *src, const CpTok *t) {
    return ident_is(src, t, "__has_include") ||
           ident_is(src, t, "__has_feature") ||
           ident_is(src, t, "__has_builtin");
}

static int emit_protected(Ex *ex, const CpTok *in, int e, int *io) {
    int i = *io;
    int depth;
    if (!emit_copy(ex, &in[i])) return 0;
    i++;
    if (i < e && punct_is(ex->src, &in[i], "(")) {
        depth = 0;
        do {
            if (punct_is(ex->src, &in[i], "(")) depth++;
            else if (punct_is(ex->src, &in[i], ")")) depth--;
            if (!emit_copy(ex, &in[i])) return 0;
            i++;
        } while (i < e && depth > 0);
        if (depth != 0) {
            snprintf(ex->err, sizeof(ex->err), "unclosed ( in #if");
            return 0;
        }
    } else if (i < e && in[i].kind == CP_TOK_IDENT) {
        if (!emit_copy(ex, &in[i])) return 0;
        i++;
    }
    *io = i;
    return 1;
}

static int expand_if_operand(Ex *ex, const CpTok *in, int i, int e, TokList *out) {
    CpTok *saved = ex->out;
    int sn = ex->nout, sc = ex->ocap;
    int saved_live = ex->live;
    int ok = 1;
    ex->out = NULL;
    ex->nout = 0;
    ex->ocap = 0;
    /* #elif is walked while the current arm is dead; still expand the cond. */
    ex->live = 1;
    while (ok && i < e) {
        if (ident_is(ex->src, &in[i], "defined") || is_has_kw(ex->src, &in[i])) {
            ok = emit_protected(ex, in, e, &i);
            continue;
        }
        ok = expand_from(ex, in, e, &i);
    }
    out->t = ex->out;
    out->n = ex->nout;
    ex->out = saved;
    ex->nout = sn;
    ex->ocap = sc;
    ex->live = saved_live;
    return ok;
}

static int eval_if_toks(Ex *ex, const CpTok *in, int i, int e) {
    TokList exp = { NULL, 0 };
    CpIfOpts opts;
    long long v = 0;
    int rc;
    if (i > e) return 0;
    if (!expand_if_operand(ex, in, i, e, &exp)) {
        free(exp.t);
        return -1;
    }
    memset(&opts, 0, sizeof(opts));
    opts.file_dir = ex->file_dir;
    rc = cparse_eval_if_toks_ex(ex->src, exp.t, exp.n, ex_isdef, ex, &v, ex->err,
                                (int)sizeof(ex->err), &opts);
    free(exp.t);
    if (rc != 0) return -1;
    return v != 0;
}

static int do_define(Ex *ex, const CpTok *in, int a, int b) {
    Macro m;
    int i = a;
    size_t nlen;
    const char *np;
    memset(&m, 0, sizeof(m));
    if (i >= b || in[i].kind != CP_TOK_IDENT) {
        snprintf(ex->err, sizeof(ex->err), "#define missing name");
        return 0;
    }
    np = tsp(ex->src, &in[i], &nlen);
    m.name = (char *)malloc(nlen + 1);
    if (!m.name) return 0;
    memcpy(m.name, np, nlen);
    m.name[nlen] = 0;
    i++;
    if (i < b && punct_is(ex->src, &in[i], "(") &&
        in[i].offset == in[i - 1].offset + in[i - 1].len) {
        m.is_func = 1;
        i++;
        while (i < b && !punct_is(ex->src, &in[i], ")")) {
            if (punct_is(ex->src, &in[i], "...")) {
                m.variadic = 1;
                i++;
                break;
            }
            if (in[i].kind != CP_TOK_IDENT) {
                snprintf(ex->err, sizeof(ex->err), "#define bad parameter");
                mac_clear(&m);
                return 0;
            }
            {
                size_t pn;
                const char *pp = tsp(ex->src, &in[i], &pn);
                char *ps = (char *)malloc(pn + 1);
                char **nb;
                if (!ps) {
                    mac_clear(&m);
                    return 0;
                }
                memcpy(ps, pp, pn);
                ps[pn] = 0;
                nb = (char **)realloc(m.params, (size_t)(m.nparams + 1) * sizeof(char *));
                if (!nb) {
                    free(ps);
                    mac_clear(&m);
                    return 0;
                }
                m.params = nb;
                m.params[m.nparams++] = ps;
            }
            i++;
            if (i < b && punct_is(ex->src, &in[i], ",")) i++;
        }
        if (i < b && punct_is(ex->src, &in[i], ")")) i++;
    }
    if (i < b) {
        m.nbody = b - i;
        m.body = (CpTok *)malloc((size_t)m.nbody * sizeof(CpTok));
        if (!m.body) {
            mac_clear(&m);
            return 0;
        }
        memcpy(m.body, in + i, (size_t)m.nbody * sizeof(CpTok));
    }
    return mac_set(ex, &m);
}

static int handle_dir(Ex *ex, const CpTok *in, int nin, int *io) {
    int i = *io;
    int line_hi, e, kw;
    const char *src = ex->src;
    e = collect_line(src, ex->slen, in, nin, i, &line_hi);
    kw = i + 1;
    if (kw >= e || in[kw].kind != CP_TOK_IDENT) {
        snprintf(ex->err, sizeof(ex->err), "bad directive");
        return 0;
    }
    if (ident_is(src, &in[kw], "ifdef") || ident_is(src, &in[kw], "ifndef")) {
        int want = ident_is(src, &in[kw], "ifdef");
        int d = 0;
        if (kw + 1 < e && in[kw + 1].kind == CP_TOK_IDENT) {
            size_t n;
            const char *p = tsp(src, &in[kw + 1], &n);
            d = tok_defined(ex, p, n);
        }
        {
            int cond = want ? d : !d;
            if (ex->ifn >= 64) {
                snprintf(ex->err, sizeof(ex->err), "#if too deep");
                return 0;
            }
            ex->parent[ex->ifn] = ex->live;
            ex->taken[ex->ifn] = ex->live && cond;
            ex->saw_else[ex->ifn] = 0;
            ex->live = ex->taken[ex->ifn];
            ex->ifn++;
        }
        *io = e;
        return 1;
    }
    if (ident_is(src, &in[kw], "if")) {
        int cond = eval_if_toks(ex, in, kw + 1, e);
        if (cond < 0) return 0;
        if (ex->ifn >= 64) {
            snprintf(ex->err, sizeof(ex->err), "#if too deep");
            return 0;
        }
        ex->parent[ex->ifn] = ex->live;
        ex->taken[ex->ifn] = ex->live && cond;
        ex->saw_else[ex->ifn] = 0;
        ex->live = ex->taken[ex->ifn];
        ex->ifn++;
        *io = e;
        return 1;
    }
    if (ident_is(src, &in[kw], "else")) {
        if (ex->ifn <= 0) {
            snprintf(ex->err, sizeof(ex->err), "#else without #if");
            return 0;
        }
        if (ex->saw_else[ex->ifn - 1]) {
            snprintf(ex->err, sizeof(ex->err), "duplicate #else");
            return 0;
        }
        ex->saw_else[ex->ifn - 1] = 1;
        ex->live = ex->parent[ex->ifn - 1] && !ex->taken[ex->ifn - 1];
        *io = e;
        return 1;
    }
    if (ident_is(src, &in[kw], "endif")) {
        if (ex->ifn <= 0) {
            snprintf(ex->err, sizeof(ex->err), "#endif without #if");
            return 0;
        }
        ex->ifn--;
        ex->live = ex->parent[ex->ifn];
        *io = e;
        return 1;
    }
    if (ident_is(src, &in[kw], "elif")) {
        int cond;
        if (ex->ifn <= 0) {
            snprintf(ex->err, sizeof(ex->err), "#elif without #if");
            return 0;
        }
        if (ex->saw_else[ex->ifn - 1]) {
            snprintf(ex->err, sizeof(ex->err), "#elif after #else");
            return 0;
        }
        cond = eval_if_toks(ex, in, kw + 1, e);
        if (cond < 0) return 0;
        if (ex->taken[ex->ifn - 1]) {
            ex->live = 0;
        } else {
            int take = ex->parent[ex->ifn - 1] && cond;
            ex->taken[ex->ifn - 1] = take;
            ex->live = take;
        }
        *io = e;
        return 1;
    }
    if (ident_is(src, &in[kw], "define")) {
        if (ex->live && !do_define(ex, in, kw + 1, e)) return 0;
        *io = e;
        return 1;
    }
    if (ident_is(src, &in[kw], "undef")) {
        if (ex->live && kw + 1 < e && in[kw + 1].kind == CP_TOK_IDENT) {
            size_t n;
            const char *p = tsp(src, &in[kw + 1], &n);
            mac_undef(ex, p, n);
        }
        *io = e;
        return 1;
    }
    /* #error / #pragma / #line: drop in expand */
    *io = e;
    (void)i;
    return 1;
}

static int expand_from(Ex *ex, const CpTok *in, int nin, int *io);

static int expand_list(Ex *ex, const CpTok *in, int nin) {
    int i = 0;
    while (i < nin) {
        if (!expand_from(ex, in, nin, &i)) return 0;
    }
    return 1;
}

static int expand_from(Ex *ex, const CpTok *in, int nin, int *io) {
    int i = *io;
    const CpTok *t;
    size_t n;
    const char *p;
    Macro *m;
    if (i >= nin) return 1;
    t = &in[i];
    if (hash_bol(ex->src, t)) {
        return handle_dir(ex, in, nin, io);
    }
    /* replacement tokens may copy a file-offset '#'; only raw stream dirs */
    if (!ex->live) {
        *io = i + 1;
        return 1;
    }
    if (t->kind != CP_TOK_IDENT) {
        if (!emit_copy(ex, t)) return 0;
        *io = i + 1;
        return 1;
    }
    p = tsp(ex->src, t, &n);
    if (hidden(ex, p, n) || !(m = mac_find(ex, p, n))) {
        if (!emit_copy(ex, t)) return 0;
        *io = i + 1;
        return 1;
    }
    if (m->is_func) {
        TokList *args = NULL;
        int nargs = 0;
        int j = i + 1;
        if (j >= nin || !punct_is(ex->src, &in[j], "(")) {
            if (!emit_copy(ex, t)) return 0;
            *io = i + 1;
            return 1;
        }
        if (!collect_call_args(ex->src, in, nin, &j, &args, &nargs)) {
            snprintf(ex->err, sizeof(ex->err), "unclosed macro call");
            return 0;
        }
        {
            int need = m->nparams + (m->variadic ? 1 : 0);
            TokList *use = args;
            int un = nargs;
            /* FOO() with FOO(x): one empty arg already if we saw () — handled
             * as narg=0. Promote to one empty arg when a single param. */
            TokList empty;
            TokList one_empty[1];
            empty.t = NULL;
            empty.n = 0;
            if (!m->variadic && m->nparams == 1 && nargs == 0) {
                one_empty[0] = empty;
                use = one_empty;
                un = 1;
            }
            if (!m->variadic && un != m->nparams) {
                snprintf(ex->err, sizeof(ex->err),
                         "macro %s argument count", m->name);
                free(args);
                return 0;
            }
            if (m->variadic && un < m->nparams) {
                snprintf(ex->err, sizeof(ex->err),
                         "macro %s argument count", m->name);
                free(args);
                return 0;
            }
            /* pack leftover into va slot: args[nparams] should be the rest
             * joined — collect_call_args already split. For subst, va is
             * args from nparams onward as one list with commas. */
            if (m->variadic) {
                TokList packed[16];
                TokList va;
                CpTok *vat = NULL;
                int vn = 0, k, pidx;
                int npack = m->nparams;
                for (k = 0; k < m->nparams && k < un; k++) packed[k] = use[k];
                va.t = NULL;
                va.n = 0;
                if (un > m->nparams) {
                    /* rebuild tokens from first va arg through last, including commas */
                    /* args are slices of `in`; reconstruct from first va start to last end */
                    int a0 = (int)(use[m->nparams].t - in);
                    int a1 = (int)(use[un - 1].t - in) + use[un - 1].n;
                    va.t = (CpTok *)(void *)(in + a0);
                    va.n = a1 - a0;
                    (void)vat;
                    (void)vn;
                    (void)pidx;
                }
                for (k = 0; k < npack; k++) packed[k] = use[k];
                packed[m->nparams] = va;
                if (!expand_macro(ex, m, packed, m->nparams + 1)) {
                    free(args);
                    return 0;
                }
            } else {
                if (!expand_macro(ex, m, use, un)) {
                    free(args);
                    return 0;
                }
            }
            (void)need;
        }
        free(args);
        *io = j;
        return 1;
    }
    if (!expand_macro(ex, m, NULL, 0)) return 0;
    *io = i + 1;
    return 1;
}

static int install_predef(Ex *ex, const CpEnv *predef) {
    int i;
    if (!predef) return 1;
    for (i = 0; i < predef->n; i++) {
        Macro m;
        const char *body;
        memset(&m, 0, sizeof(m));
        m.name = (char *)malloc(strlen(predef->names[i]) + 1);
        if (!m.name) return 0;
        memcpy(m.name, predef->names[i], strlen(predef->names[i]) + 1);
        body = (predef->bodies && predef->bodies[i]) ? predef->bodies[i] : "1";
        if (predef->is_func && predef->is_func[i]) {
            size_t nlen = strlen(predef->names[i]);
            size_t blen = strlen(body);
            char *tmp = (char *)malloc(nlen + blen + 1);
            char *intern;
            CpTok *lt = NULL;
            int nlt = 0;
            if (!tmp) {
                free(m.name);
                return 0;
            }
            memcpy(tmp, predef->names[i], nlen);
            memcpy(tmp + nlen, body, blen + 1);
            intern = ex_str(ex, tmp, nlen + blen);
            free(tmp);
            free(m.name);
            if (!intern) return 0;
            if (cparse_lex(intern, (int)(nlen + blen), &lt, &nlt) != 0) {
                free(lt);
                return 0;
            }
            if (!do_define(ex, lt, 0, nlt)) {
                free(lt);
                return 0;
            }
            free(lt);
            continue;
        }
        if (body[0]) {
            CpTok *lt = NULL;
            int nlt = 0;
            if (cparse_lex(body, (int)strlen(body), &lt, &nlt) != 0) {
                free(m.name);
                free(lt);
                return 0;
            }
            m.body = lt;
            m.nbody = nlt;
        }
        if (!mac_set(ex, &m)) return 0;
    }
    return 1;
}

int cparse_eval_if_expr(const char *expr, const CpEnv *env, long long *out,
                        char *err, int errcap) {
    Ex ex;
    CpTok *toks = NULL;
    int nt = 0;
    TokList exp = { NULL, 0 };
    CpIfOpts opts;
    long long v = 0;
    int rc;
    if (!expr || !out) return -1;
    if (cparse_lex(expr, (int)strlen(expr), &toks, &nt) != 0) {
        if (err && errcap) snprintf(err, (size_t)errcap, "#if lex failed");
        free(toks);
        return -1;
    }
    memset(&ex, 0, sizeof(ex));
    ex.src = expr;
    ex.slen = (int)strlen(expr);
    ex.live = 1;
    if (env) ex.file_dir = env->file_dir;
    if (!install_predef(&ex, env)) {
        if (err && errcap) snprintf(err, (size_t)errcap, "oom");
        free(toks);
        return -1;
    }
    if (!expand_if_operand(&ex, toks, 0, nt, &exp)) {
        if (err && errcap)
            snprintf(err, (size_t)errcap, "%s",
                     ex.err[0] ? ex.err : "expand #if failed");
        free(exp.t);
        free(toks);
        {
            int i;
            for (i = 0; i < ex.nmac; i++) mac_clear(&ex.macs[i]);
        }
        free(ex.macs);
        free(ex.arena);
        return -1;
    }
    free(toks);
    if (exp.n == 0) {
        *out = 0;
        rc = 0;
    } else {
        memset(&opts, 0, sizeof(opts));
        if (env) opts.file_dir = env->file_dir;
        rc = cparse_eval_if_toks_ex(expr, exp.t, exp.n, ex_isdef, &ex, &v,
                                    err, errcap, &opts);
        *out = v;
    }
    free(exp.t);
    {
        int i;
        for (i = 0; i < ex.nmac; i++) mac_clear(&ex.macs[i]);
    }
    free(ex.macs);
    free(ex.arena);
    return rc;
}

int cparse_expand(const char *src, int len, const CpTok *in, int nin,
                  const CpEnv *predef, CpTok **out, int *nout, char **arena,
                  char **err, const char *file_dir) {
    Ex ex;
    int i, ok;
    memset(&ex, 0, sizeof(ex));
    ex.src = src;
    ex.slen = len < 0 ? (int)strlen(src) : len;
    ex.live = 1;
    ex.file_dir = file_dir;
    *out = NULL;
    *nout = 0;
    *arena = NULL;
    if (err) *err = NULL;
    if (!install_predef(&ex, predef)) {
        if (err) *err = cp_strdup_err("oom");
        return -1;
    }
    i = 0;
    ok = 1;
    while (i < nin && ok) ok = expand_from(&ex, in, nin, &i);
    if (!ok || ex.ifn != 0) {
        if (ex.ifn != 0 && !ex.err[0])
            snprintf(ex.err, sizeof(ex.err), "unclosed #if");
        if (err) *err = ex.err[0] ? cp_strdup_err(ex.err) : cp_strdup_err("expand failed");
        free(ex.out);
        for (i = 0; i < ex.nmac; i++) mac_clear(&ex.macs[i]);
        free(ex.macs);
        free(ex.arena);
        return -1;
    }
    *out = ex.out;
    *nout = ex.nout;
    *arena = ex.arena;
    for (i = 0; i < ex.nmac; i++) mac_clear(&ex.macs[i]);
    free(ex.macs);
    return 0;
}
