/*
 * @grammar(cli) argv product — parse the fenced declaration DSL and emit
 * typed struct + parse_args / print_usage / prepare. Called from the
 * harvested comptime `cli` engine, not as a seam builtin.
 *
 * Body lines (comments // ... stripped):
 *   name: flag SPELL... [attrs...]
 *   name: count SPELL... [attrs...]
 *   name: opt i64|string SPELL... [attrs...]
 *   name: rest string [attrs...]
 *   name: alias SPELL... -> target[=VALUE] [attrs...]
 *
 * Attrs (order-free): desc "..." | as NAME | attach | default LIT
 *   | alias SPELL=VALUE[, SPELL=VALUE...]   (on opt only; synthesizes alias rows)
 * LIT is digits for i64 or "quoted" for string. Omit attach => space-required.
 * Compat: attached/value_name/space/many positional string still accepted.
 *
 * SPELL is --long, -X, -digits (e.g. -11), or -LO..-HI digit range (e.g. -0..-9).
 */
#include "preprocess/grammar_engine.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CLI_MAX_FIELDS = 64,
    CLI_MAX_SPELLS = 16,
    CLI_MAX_INLINE_ALIASES = 8,
    CLI_MAX_NAME = 64,
    CLI_MAX_DESC = 256,
};

typedef enum {
    CK_FLAG = 1,
    CK_COUNT,
    CK_OPT_I64,
    CK_OPT_STRING,
    CK_MANY_STRING,
    CK_ALIAS,
} CliKind;

typedef struct {
    char name[CLI_MAX_NAME];
    CliKind kind;
    char longs[CLI_MAX_SPELLS][CLI_MAX_NAME];
    int nlongs;
    char shorts[CLI_MAX_SPELLS]; /* packed single chars */
    int nshorts;
    char short_aliases[CLI_MAX_SPELLS][CLI_MAX_NAME];
    int nshort_aliases;
    int attached;
    int require_space;
    char desc[CLI_MAX_DESC];
    char value_name[CLI_MAX_NAME];
    char maps_to[CLI_MAX_NAME];
    char maps_value[CLI_MAX_NAME];
    int has_maps_value;
    int is_help;
    char default_lit[CLI_MAX_DESC];
    int has_default;
    /* Inline `alias SPELL=VALUE` attrs — expanded to CK_ALIAS rows after emit. */
    char inline_alias_spells[CLI_MAX_INLINE_ALIASES][CLI_MAX_NAME];
    char inline_alias_values[CLI_MAX_INLINE_ALIASES][CLI_MAX_NAME];
    int ninline_aliases;
} CliField;

typedef struct {
    char name[CLI_MAX_NAME];
    CliField fields[CLI_MAX_FIELDS];
    int nfields;
    char err[512];
    int err_at;
    const char *body;
    size_t len;
    size_t p;
} CliG;

static void cli_fail(CliG *g, size_t at, const char *msg) {
    if (!g || g->err[0]) return;
    snprintf(g->err, sizeof(g->err), "%s", msg ? msg : "error");
    g->err_at = (int)at;
}

static void cli_skip_ws(CliG *g) {
    while (g->p < g->len) {
        char c = g->body[g->p];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            g->p++;
            continue;
        }
        if (c == '/' && g->p + 1 < g->len && g->body[g->p + 1] == '/') {
            g->p += 2;
            while (g->p < g->len && g->body[g->p] != '\n') g->p++;
            continue;
        }
        break;
    }
}

static int cli_ident(CliG *g, char *out, size_t out_sz) {
    size_t n = 0;
    cli_skip_ws(g);
    if (g->p >= g->len || !(isalpha((unsigned char)g->body[g->p]) || g->body[g->p] == '_'))
        return 0;
    while (g->p < g->len) {
        char c = g->body[g->p];
        if (!isalnum((unsigned char)c) && c != '_' && c != '-') break;
        if (n + 1 >= out_sz) {
            cli_fail(g, g->p, "identifier too long");
            return 0;
        }
        out[n++] = c;
        g->p++;
    }
    out[n] = '\0';
    return n > 0;
}

static int cli_string(CliG *g, char *out, size_t out_sz) {
    size_t n = 0;
    cli_skip_ws(g);
    if (g->p >= g->len || g->body[g->p] != '"') return 0;
    g->p++;
    while (g->p < g->len && g->body[g->p] != '"') {
        if (n + 1 >= out_sz) {
            cli_fail(g, g->p, "string too long");
            return 0;
        }
        out[n++] = g->body[g->p++];
    }
    if (g->p >= g->len || g->body[g->p] != '"') {
        cli_fail(g, g->p, "unterminated string");
        return 0;
    }
    g->p++;
    out[n] = '\0';
    return 1;
}

static int cli_parse_neg_digits(const char *s, int *out) {
    size_t i;
    int v = 0;
    if (!s || s[0] != '-' || !s[1] || out == NULL) return 0;
    for (i = 1; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
        if (v > 99) return 0;
    }
    *out = v;
    return i > 1;
}

static int cli_add_spell(CliG *g, CliField *f, const char *spell) {
    size_t n;
    const char *dots;
    if (!spell || spell[0] != '-') {
        cli_fail(g, g->p, "spelling must start with -");
        return 0;
    }
    /* Digit range: -LO..-HI (inclusive), e.g. -0..-9 */
    dots = strstr(spell, "..");
    if (dots && dots > spell) {
        char left[CLI_MAX_NAME];
        char right[CLI_MAX_NAME];
        size_t left_n = (size_t)(dots - spell);
        int lo = 0, hi = 0, d;
        if (left_n == 0 || left_n >= sizeof(left)) {
            cli_fail(g, g->p, "bad digit range spelling");
            return 0;
        }
        memcpy(left, spell, left_n);
        left[left_n] = '\0';
        snprintf(right, sizeof(right), "%s", dots + 2);
        if (!cli_parse_neg_digits(left, &lo) || !cli_parse_neg_digits(right, &hi)) {
            cli_fail(g, g->p, "digit range must be -LO..-HI");
            return 0;
        }
        if (lo > hi) {
            cli_fail(g, g->p, "digit range LO must be <= HI");
            return 0;
        }
        for (d = lo; d <= hi; d++) {
            char one[16];
            snprintf(one, sizeof(one), "-%d", d);
            if (!cli_add_spell(g, f, one)) return 0;
        }
        return 1;
    }
    if (spell[1] == '-') {
        const char *long_name = spell + 2;
        if (!long_name[0] || f->nlongs >= CLI_MAX_SPELLS) {
            cli_fail(g, g->p, "bad or too many long spellings");
            return 0;
        }
        snprintf(f->longs[f->nlongs++], CLI_MAX_NAME, "%s", long_name);
        return 1;
    }
    n = strlen(spell + 1);
    if (n == 0) {
        cli_fail(g, g->p, "empty short spelling");
        return 0;
    }
    if (n == 1) {
        if (f->nshorts >= CLI_MAX_SPELLS) {
            cli_fail(g, g->p, "too many short spellings");
            return 0;
        }
        f->shorts[f->nshorts++] = spell[1];
        f->shorts[f->nshorts] = '\0';
        return 1;
    }
    /* multi-char short alias (e.g. -11) */
    {
        size_t i;
        int all_digit = 1;
        for (i = 0; i < n; i++) {
            if (spell[1 + i] < '0' || spell[1 + i] > '9') all_digit = 0;
        }
        if (!all_digit) {
            cli_fail(g, g->p, "multi-char short must be digits (e.g. -11)");
            return 0;
        }
        if (f->nshort_aliases >= CLI_MAX_SPELLS) {
            cli_fail(g, g->p, "too many short aliases");
            return 0;
        }
        snprintf(f->short_aliases[f->nshort_aliases++], CLI_MAX_NAME, "%s", spell + 1);
        return 1;
    }
}

static int cli_parse_spells(CliG *g, CliField *f) {
    for (;;) {
        char spell[CLI_MAX_NAME];
        size_t n = 0;
        cli_skip_ws(g);
        if (g->p >= g->len) break;
        if (g->body[g->p] != '-') break;
        /* `->` is the alias arrow, not a spelling */
        if (g->p + 1 < g->len && g->body[g->p + 1] == '>') break;
        while (g->p < g->len) {
            char c = g->body[g->p];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',') break;
            if (n + 1 >= sizeof(spell)) {
                cli_fail(g, g->p, "spelling too long");
                return 0;
            }
            spell[n++] = c;
            g->p++;
        }
        spell[n] = '\0';
        if (!cli_add_spell(g, f, spell)) return 0;
        cli_skip_ws(g);
        if (g->p < g->len && g->body[g->p] == ',') {
            g->p++;
            continue;
        }
    }
    return 1;
}

static int cli_parse_field(CliG *g) {
    CliField *f;
    char kind[32];
    char typ[32];
    if (g->nfields >= CLI_MAX_FIELDS) {
        cli_fail(g, g->p, "too many fields");
        return 0;
    }
    f = &g->fields[g->nfields];
    memset(f, 0, sizeof(*f));
    if (!cli_ident(g, f->name, sizeof(f->name))) {
        cli_fail(g, g->p, "expected field name");
        return 0;
    }
    cli_skip_ws(g);
    if (g->p >= g->len || g->body[g->p] != ':') {
        cli_fail(g, g->p, "expected ':' after field name");
        return 0;
    }
    g->p++;
    if (!cli_ident(g, kind, sizeof(kind))) {
        cli_fail(g, g->p, "expected field kind");
        return 0;
    }
    if (strcmp(kind, "flag") == 0) {
        f->kind = CK_FLAG;
        if (!cli_parse_spells(g, f)) return 0;
    } else if (strcmp(kind, "count") == 0) {
        f->kind = CK_COUNT;
        if (!cli_parse_spells(g, f)) return 0;
    } else if (strcmp(kind, "opt") == 0) {
        if (!cli_ident(g, typ, sizeof(typ))) {
            cli_fail(g, g->p, "expected opt type");
            return 0;
        }
        if (strcmp(typ, "i64") == 0) f->kind = CK_OPT_I64;
        else if (strcmp(typ, "string") == 0) f->kind = CK_OPT_STRING;
        else {
            cli_fail(g, g->p, "opt type must be i64 or string");
            return 0;
        }
        if (!cli_parse_spells(g, f)) return 0;
    } else if (strcmp(kind, "rest") == 0) {
        if (!cli_ident(g, typ, sizeof(typ)) || strcmp(typ, "string") != 0) {
            cli_fail(g, g->p, "expected 'string' after rest");
            return 0;
        }
        f->kind = CK_MANY_STRING;
    } else if (strcmp(kind, "many") == 0) {
        /* Compat: many positional string */
        char pos[32];
        if (!cli_ident(g, pos, sizeof(pos)) || strcmp(pos, "positional") != 0) {
            cli_fail(g, g->p, "expected 'positional' after many (or use: rest string)");
            return 0;
        }
        if (!cli_ident(g, typ, sizeof(typ)) || strcmp(typ, "string") != 0) {
            cli_fail(g, g->p, "expected 'string' after positional");
            return 0;
        }
        f->kind = CK_MANY_STRING;
    } else if (strcmp(kind, "alias") == 0) {
        char arrow[8];
        char target[CLI_MAX_NAME];
        f->kind = CK_ALIAS;
        if (!cli_parse_spells(g, f)) return 0;
        cli_skip_ws(g);
        if (g->p + 1 >= g->len || g->body[g->p] != '-' || g->body[g->p + 1] != '>') {
            cli_fail(g, g->p, "alias needs '-> target[=value]'");
            return 0;
        }
        g->p += 2;
        if (!cli_ident(g, target, sizeof(target))) {
            cli_fail(g, g->p, "alias target name");
            return 0;
        }
        snprintf(f->maps_to, sizeof(f->maps_to), "%s", target);
        cli_skip_ws(g);
        if (g->p < g->len && g->body[g->p] == '=') {
            g->p++;
            cli_skip_ws(g);
            if (!cli_ident(g, f->maps_value, sizeof(f->maps_value))) {
                /* allow numeric maps value as ident-like digits */
                size_t n = 0;
                while (g->p < g->len && isdigit((unsigned char)g->body[g->p])) {
                    if (n + 1 >= sizeof(f->maps_value)) break;
                    f->maps_value[n++] = g->body[g->p++];
                }
                f->maps_value[n] = '\0';
                if (n == 0) {
                    cli_fail(g, g->p, "alias value after =");
                    return 0;
                }
            }
            f->has_maps_value = 1;
        }
        (void)arrow;
    } else {
        cli_fail(g, g->p, "unknown field kind (flag|count|opt|rest|alias)");
        return 0;
    }

    for (;;) {
        char attr[32];
        size_t save;
        cli_skip_ws(g);
        if (g->p >= g->len) break;
        if (g->body[g->p] == '\n') break;
        save = g->p;
        if (!cli_ident(g, attr, sizeof(attr))) break;
        if (strcmp(attr, "attach") == 0 || strcmp(attr, "attached") == 0) {
            f->attached = 1;
        } else if (strcmp(attr, "space") == 0) {
            /* default policy; accepted as no-op */
            f->require_space = 1;
        } else if (strcmp(attr, "desc") == 0) {
            if (!cli_string(g, f->desc, sizeof(f->desc))) {
                cli_fail(g, g->p, "desc expects \"...\"");
                return 0;
            }
        } else if (strcmp(attr, "as") == 0 || strcmp(attr, "value_name") == 0) {
            if (!cli_ident(g, f->value_name, sizeof(f->value_name))) {
                cli_fail(g, g->p, "as expects ident");
                return 0;
            }
        } else if (strcmp(attr, "default") == 0) {
            cli_skip_ws(g);
            if (f->kind == CK_OPT_STRING) {
                if (!cli_string(g, f->default_lit, sizeof(f->default_lit))) {
                    cli_fail(g, g->p, "default expects \"...\" for opt string");
                    return 0;
                }
                f->has_default = 1;
            } else if (f->kind == CK_OPT_I64) {
                size_t n = 0;
                if (g->p < g->len && g->body[g->p] == '-') {
                    f->default_lit[n++] = g->body[g->p++];
                }
                while (g->p < g->len && isdigit((unsigned char)g->body[g->p])) {
                    if (n + 1 >= sizeof(f->default_lit)) break;
                    f->default_lit[n++] = g->body[g->p++];
                }
                f->default_lit[n] = '\0';
                if (n == 0 || (n == 1 && f->default_lit[0] == '-')) {
                    cli_fail(g, g->p, "default expects integer for opt i64");
                    return 0;
                }
                f->has_default = 1;
            } else {
                cli_fail(g, g->p, "default only valid on opt i64|string");
                return 0;
            }
        } else if (strcmp(attr, "alias") == 0) {
            /* Inline aliases on opt: alias --fast=1, --best=9 */
            if (f->kind != CK_OPT_I64 && f->kind != CK_OPT_STRING) {
                cli_fail(g, g->p, "inline alias only valid on opt");
                return 0;
            }
            for (;;) {
                char tok[CLI_MAX_NAME];
                char *eq;
                size_t tn = 0;
                cli_skip_ws(g);
                if (g->p >= g->len || g->body[g->p] != '-') break;
                while (g->p < g->len) {
                    char c = g->body[g->p];
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',') break;
                    if (tn + 1 >= sizeof(tok)) {
                        cli_fail(g, g->p, "inline alias spelling too long");
                        return 0;
                    }
                    tok[tn++] = c;
                    g->p++;
                }
                tok[tn] = '\0';
                eq = strchr(tok, '=');
                if (!eq || eq == tok || !eq[1]) {
                    cli_fail(g, g->p, "inline alias expects SPELL=VALUE");
                    return 0;
                }
                *eq = '\0';
                if (f->ninline_aliases >= CLI_MAX_INLINE_ALIASES) {
                    cli_fail(g, g->p, "too many inline aliases");
                    return 0;
                }
                snprintf(f->inline_alias_spells[f->ninline_aliases], CLI_MAX_NAME, "%s",
                         tok);
                snprintf(f->inline_alias_values[f->ninline_aliases], CLI_MAX_NAME, "%s",
                         eq + 1);
                f->ninline_aliases++;
                cli_skip_ws(g);
                if (g->p < g->len && g->body[g->p] == ',') {
                    g->p++;
                    continue;
                }
                break;
            }
            if (f->ninline_aliases == 0) {
                cli_fail(g, g->p, "alias expects SPELL=VALUE");
                return 0;
            }
        } else {
            g->p = save;
            break;
        }
    }
    /* default long name = field name when none given (non-positional, non-alias ok) */
    if (f->kind != CK_MANY_STRING && f->kind != CK_ALIAS && f->nlongs == 0) {
        snprintf(f->longs[f->nlongs++], CLI_MAX_NAME, "%s", f->name);
    }
    if (f->kind == CK_ALIAS && f->nlongs == 0 && f->nshorts == 0
        && f->nshort_aliases == 0) {
        cli_fail(g, g->p, "alias needs a spelling");
        return 0;
    }
    /* Convention: flag field named `help` drives prepare → Ok(false). */
    if (f->kind == CK_FLAG && strcmp(f->name, "help") == 0) f->is_help = 1;
    {
        int nin = f->ninline_aliases;
        char owner[CLI_MAX_NAME];
        char spells[CLI_MAX_INLINE_ALIASES][CLI_MAX_NAME];
        char values[CLI_MAX_INLINE_ALIASES][CLI_MAX_NAME];
        int k;
        snprintf(owner, sizeof(owner), "%s", f->name);
        for (k = 0; k < nin; k++) {
            snprintf(spells[k], sizeof(spells[k]), "%s", f->inline_alias_spells[k]);
            snprintf(values[k], sizeof(values[k]), "%s", f->inline_alias_values[k]);
        }
        g->nfields++;
        for (k = 0; k < nin; k++) {
            CliField *a;
            if (g->nfields >= CLI_MAX_FIELDS) {
                cli_fail(g, g->p, "too many fields");
                return 0;
            }
            a = &g->fields[g->nfields];
            memset(a, 0, sizeof(*a));
            a->kind = CK_ALIAS;
            snprintf(a->maps_to, sizeof(a->maps_to), "%s", owner);
            snprintf(a->maps_value, sizeof(a->maps_value), "%s", values[k]);
            a->has_maps_value = 1;
            if (!cli_add_spell(g, a, spells[k])) return 0;
            if (a->nlongs > 0) {
                snprintf(a->name, sizeof(a->name), "%s", a->longs[0]);
            } else if (a->nshorts > 0) {
                snprintf(a->name, sizeof(a->name), "%c", a->shorts[0]);
            } else if (a->nshort_aliases > 0) {
                snprintf(a->name, sizeof(a->name), "%s", a->short_aliases[0]);
            } else {
                cli_fail(g, g->p, "inline alias needs a spelling");
                return 0;
            }
            g->nfields++;
        }
    }
    return 1;
}

static int cli_parse_body(CliG *g) {
    while (g->p < g->len) {
        cli_skip_ws(g);
        if (g->p >= g->len) break;
        if (!cli_parse_field(g)) return 0;
    }
    if (g->nfields == 0) {
        cli_fail(g, 0, "cli grammar has no fields");
        return 0;
    }
    return 1;
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} CliBuf;

static int cli_buf_grow(CliBuf *b, size_t need) {
    size_t ncap;
    char *nd;
    if (need <= b->cap) return 1;
    ncap = b->cap ? b->cap : 4096;
    while (ncap < need) ncap *= 2;
    nd = (char *)realloc(b->data, ncap);
    if (!nd) return 0;
    b->data = nd;
    b->cap = ncap;
    return 1;
}

static int cli_buf_printf(CliBuf *b, const char *fmt, ...) {
    va_list ap;
    int n;
    for (;;) {
        size_t avail = b->cap - b->len;
        va_start(ap, fmt);
        n = vsnprintf(b->data + b->len, avail, fmt, ap);
        va_end(ap);
        if (n < 0) return 0;
        if ((size_t)n + 1 <= avail) {
            b->len += (size_t)n;
            return 1;
        }
        if (!cli_buf_grow(b, b->len + (size_t)n + 1)) return 0;
    }
}

static const char *cli_kind_c(CliKind k) {
    switch (k) {
    case CK_FLAG: return "CC_CLI_FLAG";
    case CK_COUNT: return "CC_CLI_COUNT";
    case CK_OPT_I64: return "CC_CLI_OPT_I64";
    case CK_OPT_STRING: return "CC_CLI_OPT_STRING";
    case CK_MANY_STRING: return "CC_CLI_MANY_STRING";
    case CK_ALIAS: return "CC_CLI_ALIAS";
    }
    return "CC_CLI_FLAG";
}

static char *cc__cli_grammar_emit(const char *name,
                                  const char *body, size_t body_len,
                                  const char *file, int line,
                                  char *err, size_t err_sz) {
    CliG g;
    CliBuf buf = {0};
    int i, j;
    memset(&g, 0, sizeof(g));
    snprintf(g.name, sizeof(g.name), "%s", name ? name : "Opts");
    g.body = body ? body : "";
    g.len = body_len;
    if (!cli_parse_body(&g)) {
        if (err && err_sz) {
            snprintf(err, err_sz, "@grammar(cli) %s (%s:%d): %s",
                     g.name, file ? file : "?", line, g.err);
        }
        return NULL;
    }
    if (!cli_buf_grow(&buf, 4096)) {
        if (err && err_sz) snprintf(err, err_sz, "oom");
        return NULL;
    }
    buf.data[0] = '\0';

    if (!cli_buf_printf(&buf,
                        "/* @grammar(cli) %s — parse_args / print_usage */\n"
                        "#include <stddef.h>\n"
                        "#include <stdint.h>\n"
                        "#include <string.h>\n"
                        "typedef struct %s {\n",
                        g.name, g.name))
        goto oom;

    for (i = 0; i < g.nfields; i++) {
        CliField *f = &g.fields[i];
        if (f->kind == CK_ALIAS) continue;
        switch (f->kind) {
        case CK_FLAG:
            if (!cli_buf_printf(&buf, "    bool %s;\n", f->name)) goto oom;
            break;
        case CK_COUNT:
            if (!cli_buf_printf(&buf, "    int64_t %s;\n", f->name)) goto oom;
            break;
        case CK_OPT_I64:
            if (!cli_buf_printf(&buf,
                                "    int64_t %s;\n"
                                "    bool %s_present;\n",
                                f->name, f->name))
                goto oom;
            break;
        case CK_OPT_STRING:
            /* Post-lower splice: char[:0] is already CCSlice in host C. */
            if (!cli_buf_printf(&buf,
                                "    CCSlice %s;\n"
                                "    bool %s_present;\n",
                                f->name, f->name))
                goto oom;
            break;
        case CK_MANY_STRING:
            if (!cli_buf_printf(&buf,
                                "    CCSlice *%s;\n"
                                "    size_t %s_len;\n",
                                f->name, f->name))
                goto oom;
            break;
        default:
            break;
        }
    }
    if (!cli_buf_printf(&buf, "} %s;\n\n", g.name)) goto oom;

    /* spelling tables */
    for (i = 0; i < g.nfields; i++) {
        CliField *f = &g.fields[i];
        if (f->nlongs > 0) {
            if (!cli_buf_printf(&buf, "static const char *const %s__cli_long_%s[] = {",
                                g.name, f->name))
                goto oom;
            for (j = 0; j < f->nlongs; j++) {
                if (!cli_buf_printf(&buf, "\"%s\", ", f->longs[j])) goto oom;
            }
            if (!cli_buf_printf(&buf, "NULL};\n")) goto oom;
        }
        if (f->nshort_aliases > 0) {
            if (!cli_buf_printf(&buf,
                                "static const char *const %s__cli_salias_%s[] = {",
                                g.name, f->name))
                goto oom;
            for (j = 0; j < f->nshort_aliases; j++) {
                if (!cli_buf_printf(&buf, "\"%s\", ", f->short_aliases[j])) goto oom;
            }
            if (!cli_buf_printf(&buf, "NULL};\n")) goto oom;
        }
    }

    if (!cli_buf_printf(&buf, "static const CCCliField %s__cli_fields[] = {\n",
                        g.name))
        goto oom;
    for (i = 0; i < g.nfields; i++) {
        CliField *f = &g.fields[i];
        const char *longs_expr = "NULL";
        const char *salias_expr = "NULL";
        char longs_buf[128];
        char salias_buf[128];
        char shorts_lit[CLI_MAX_SPELLS + 1];
        if (f->nlongs > 0) {
            snprintf(longs_buf, sizeof(longs_buf), "%s__cli_long_%s", g.name, f->name);
            longs_expr = longs_buf;
        }
        if (f->nshort_aliases > 0) {
            snprintf(salias_buf, sizeof(salias_buf), "%s__cli_salias_%s", g.name,
                     f->name);
            salias_expr = salias_buf;
        }
        memcpy(shorts_lit, f->shorts, (size_t)f->nshorts);
        shorts_lit[f->nshorts] = '\0';

        if (f->kind == CK_ALIAS) {
            char desc_lit[CLI_MAX_DESC + 4] = "NULL";
            char maps_lit[CLI_MAX_NAME + 4] = "NULL";
            if (f->desc[0]) snprintf(desc_lit, sizeof(desc_lit), "\"%s\"", f->desc);
            if (f->has_maps_value)
                snprintf(maps_lit, sizeof(maps_lit), "\"%s\"", f->maps_value);
            if (!cli_buf_printf(&buf,
                                "    {.name=\"%s\", .kind=%s, .longs=%s, .shorts=\"%s\", "
                                ".short_aliases=%s, .desc=%s, .default_cstr=NULL, "
                                ".is_help=false, .maps_to=\"%s\", .maps_value=%s},\n",
                                f->name, cli_kind_c(f->kind), longs_expr, shorts_lit,
                                salias_expr, desc_lit, f->maps_to, maps_lit))
                goto oom;
            continue;
        }

        {
            char desc_lit[CLI_MAX_DESC + 4] = "NULL";
            char vname_lit[CLI_MAX_NAME + 4] = "NULL";
            char def_lit[CLI_MAX_DESC + 4] = "NULL";
            if (f->desc[0]) snprintf(desc_lit, sizeof(desc_lit), "\"%s\"", f->desc);
            if (f->value_name[0])
                snprintf(vname_lit, sizeof(vname_lit), "\"%s\"", f->value_name);
            if (f->has_default)
                snprintf(def_lit, sizeof(def_lit), "\"%s\"", f->default_lit);
            if (!cli_buf_printf(&buf,
                                "    {.name=\"%s\", .kind=%s, .longs=%s, .shorts=\"%s\", "
                                ".short_aliases=%s, .allow_attached=%s, .require_space=%s, "
                                ".desc=%s, .value_name=%s, .default_cstr=%s, .is_help=%s, "
                                ".off=(unsigned)offsetof(%s,%s)",
                                f->name, cli_kind_c(f->kind), longs_expr, shorts_lit,
                                salias_expr,
                                f->attached ? "true" : "false",
                                f->require_space ? "true" : "false",
                                desc_lit, vname_lit, def_lit,
                                f->is_help ? "true" : "false",
                                g.name, f->name))
                goto oom;
        }

        if (f->kind == CK_OPT_I64 || f->kind == CK_OPT_STRING) {
            if (!cli_buf_printf(&buf, ", .off_present=(unsigned)offsetof(%s,%s_present)",
                                g.name, f->name))
                goto oom;
        }
        if (f->kind == CK_MANY_STRING) {
            if (!cli_buf_printf(&buf, ", .off_len=(unsigned)offsetof(%s,%s_len)",
                                g.name, f->name))
                goto oom;
        }
        if (!cli_buf_printf(&buf, "},\n")) goto oom;
    }
    if (!cli_buf_printf(&buf, "};\n"
                              "enum { %s__cli_field_count = "
                              "(unsigned)(sizeof(%s__cli_fields)/sizeof(%s__cli_fields[0])) "
                              "};\n\n",
                        g.name, g.name, g.name))
        goto oom;

    if (!cli_buf_printf(&buf,
                        "static inline int %s_parse_args(int argc, char **argv, "
                        "CCArena *arena, %s *out) {\n"
                        "    CCCliFillResult r;\n"
                        "    if (!out) return 0;\n"
                        "    memset(out, 0, sizeof(*out));\n"
                        "    cc_cli_apply_defaults(%s__cli_fields, %s__cli_field_count, out);\n"
                        "    r = cc_cli_fill(%s__cli_fields, %s__cli_field_count, "
                        "argc, argv, arena, out);\n"
                        "    return r.ok ? 1 : 0;\n"
                        "}\n"
                        "static inline void %s_print_usage(FILE *out, const char *prog) {\n"
                        "    cc_cli_print_fields(out, cc_cli_prog_basename(prog), "
                        "%s__cli_fields, %s__cli_field_count);\n"
                        "}\n"
                        "static inline CCResult_bool_CCError %s_prepare(\n"
                        "        int argc, char **argv, CCArena *arena, %s *out, "
                        "FILE *usage_out) {\n"
                        "    if (!out) {\n"
                        "        return cc_err_CCResult_bool_CCError(\n"
                        "            CC_ERROR(CC_ERR_INVALID_ARG, \"%s_prepare: null out\"));\n"
                        "    }\n"
                        "    memset(out, 0, sizeof(*out));\n"
                        "    return cc_cli_prepare_fields(%s__cli_fields, "
                        "%s__cli_field_count, argc, argv, arena, out, usage_out);\n"
                        "}\n",
                        g.name, g.name, g.name, g.name, g.name, g.name,
                        g.name, g.name, g.name, g.name, g.name, g.name, g.name,
                        g.name))
        goto oom;

    return buf.data;
oom:
    free(buf.data);
    if (err && err_sz) snprintf(err, err_sz, "@grammar(cli) %s: emit oom", g.name);
    return NULL;
}

/* Exported for grammar_rules builtin table */
char *cc_grammar_cli_emit(const char *name,
                          const char *body, size_t body_len,
                          const char *file, int line,
                          const char *src, size_t src_len,
                          char *err, size_t err_sz) {
    (void)src;
    (void)src_len;
    return cc__cli_grammar_emit(name, body, body_len, file, line, err, err_sz);
}
