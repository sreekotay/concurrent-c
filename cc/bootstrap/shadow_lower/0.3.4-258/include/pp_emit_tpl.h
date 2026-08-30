/* Typed emit templates — the serdes write projection (C text out).
 *
 * All emit is hole-fill: literals memcpy'd, binds spliced. No vsnprintf over
 * payload. Named templates (shadow_tpl_emit / shadow_tpl_kv) are the house
 * style; cemit_fmt is a shim that rewrites %s/%d into ${0}/${1}/… templates.
 *
 * Hole forms:
 *   ${name}           — IDENT or EXPR bind (string splice)
 *   ${name@prefix}    — STMTS bind; each line prefixed with bind `prefix`
 *   ${line} / ${file} — special; filled from ShadowCtx / explicit binds
 *
 * Build-time: shadow_tpl_selfcheck() fails shadow_lower startup on hole/env
 * mismatch. Runtime: shadow_tpl_emit() appends filled text to CEmit.
 */
#pragma once
#include <stdarg.h>

#include "pp_emit_core.h"
enum {
    SHADOW_TPL_IDENT = 1,
    SHADOW_TPL_EXPR = 2,
    SHADOW_TPL_STMTS = 3,
    SHADOW_TPL_LINE = 4,
    SHADOW_TPL_FILE = 5
};

typedef struct {
    const char* name;
    int kind;
    const char* value;
} ShadowTplBind;

typedef struct {
    const char* id;   /* stable name for diagnostics */
    const char* text; /* template body */
    const ShadowTplBind* schema; /* kind+name only; value ignored */
    int nschema;
} ShadowTplDef;

static const ShadowTplBind* shadow_tpl_find(const ShadowTplBind* env, int n,
                                           const char* name, size_t nlen) {
    int i;
    if (!env || !name || !nlen) return NULL;
    for (i = 0; i < n; i++) {
        if (env[i].name && strlen(env[i].name) == nlen &&
            memcmp(env[i].name, name, nlen) == 0)
            return &env[i];
    }
    return NULL;
}

/* Parse one `${...}` at *pp (points at '$'). Advances *pp past `}`. */
static int shadow_tpl_parse_hole(const char** pp, char* name, size_t ncap,
                                 char* prefix, size_t pcap, int* kind_hint) {
    const char* p;
    size_t ni = 0;
    if (!pp || !*pp || (*pp)[0] != '$' || (*pp)[1] != '{') return 0;
    p = *pp + 2;
    name[0] = 0;
    if (prefix && pcap) prefix[0] = 0;
    if (kind_hint) *kind_hint = 0;
    if (strncmp(p, "line}", 5) == 0) {
        snprintf(name, ncap, "line");
        if (kind_hint) *kind_hint = SHADOW_TPL_LINE;
        *pp = p + 5;
        return 1;
    }
    if (strncmp(p, "file}", 5) == 0) {
        snprintf(name, ncap, "file");
        if (kind_hint) *kind_hint = SHADOW_TPL_FILE;
        *pp = p + 5;
        return 1;
    }
    while (*p && *p != '}' && *p != '@') {
        if (!(((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_')))
            return 0;
        if (ni + 1 < ncap) name[ni++] = *p;
        p++;
    }
    name[ni] = 0;
    if (!ni) return 0;
    if (*p == '@') {
        size_t pi = 0;
        p++;
        if (kind_hint) *kind_hint = SHADOW_TPL_STMTS;
        while (*p && *p != '}') {
            if (!(((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || *p == '_')))
                return 0;
            if (prefix && pi + 1 < pcap) prefix[pi++] = *p;
            p++;
        }
        if (prefix && pcap) prefix[pi] = 0;
    }
    if (*p != '}') return 0;
    *pp = p + 1;
    return 1;
}

static int shadow_tpl_emit_stmts(CEmit* out, const char* body,
                                 const char* prefix) {
    const char* p;
    const char* line;
    if (!body) return 1;
    if (!prefix) prefix = "";
    p = body;
    while (*p) {
        line = p;
        while (*p && *p != '\n') p++;
        if (!cemit_str(out, prefix) ||
            !cemit_buf(out, line, (size_t)(p - line)))
            return 0;
        if (*p == '\n') {
            if (!cemit_str(out, "\n")) return 0;
            p++;
        }
    }
    return 1;
}

static int shadow_tpl_emit(CEmit* out, const char* tpl, const ShadowTplBind* env,
                           int nenv) {
    const char* p;
    if (!out || !tpl) return 0;
    p = tpl;
    while (*p) {
        if (p[0] == '$' && p[1] == '{') {
            char name[64], prefix[64];
            int hint = 0;
            const char* start = p;
            const ShadowTplBind* b;
            if (!shadow_tpl_parse_hole(&p, name, sizeof(name), prefix,
                                      sizeof(prefix), &hint)) {
                fprintf(stderr,
                        "error: emit template: malformed hole near '%.32s'\n",
                        start);
                out->err = 1;
                return 0;
            }
            if (hint == SHADOW_TPL_LINE || strcmp(name, "line") == 0) {
                b = shadow_tpl_find(env, nenv, "line", 4);
                if (!b || !b->value) {
                    fprintf(stderr,
                            "error: emit template: missing ${line} bind\n");
                    out->err = 1;
                    return 0;
                }
                if (!cemit_str(out, b->value)) return 0;
                continue;
            }
            if (hint == SHADOW_TPL_FILE || strcmp(name, "file") == 0) {
                b = shadow_tpl_find(env, nenv, "file", 4);
                if (!b || !b->value) {
                    fprintf(stderr,
                            "error: emit template: missing ${file} bind\n");
                    out->err = 1;
                    return 0;
                }
                if (!cemit_str(out, b->value)) return 0;
                continue;
            }
            b = shadow_tpl_find(env, nenv, name, strlen(name));
            if (!b) {
                fprintf(stderr,
                        "error: emit template: unbound hole '${%s}'\n", name);
                out->err = 1;
                return 0;
            }
            if (b->kind == SHADOW_TPL_STMTS || hint == SHADOW_TPL_STMTS ||
                prefix[0]) {
                const char* pref = "";
                if (b->kind != SHADOW_TPL_STMTS) {
                    fprintf(stderr,
                            "error: emit template: hole '${%s}' needs STMTS "
                            "bind (got kind %d)\n",
                            name, b->kind);
                    out->err = 1;
                    return 0;
                }
                if (prefix[0]) {
                    const ShadowTplBind* pb =
                        shadow_tpl_find(env, nenv, prefix, strlen(prefix));
                    if (!pb || !pb->value) {
                        fprintf(stderr,
                                "error: emit template: stmts prefix '${%s}' "
                                "missing\n",
                                prefix);
                        out->err = 1;
                        return 0;
                    }
                    pref = pb->value;
                }
                if (!shadow_tpl_emit_stmts(out, b->value ? b->value : "", pref))
                    return 0;
                continue;
            }
            if (b->kind != SHADOW_TPL_IDENT && b->kind != SHADOW_TPL_EXPR) {
                fprintf(stderr,
                        "error: emit template: hole '${%s}' needs IDENT/EXPR "
                        "(got kind %d)\n",
                        name, b->kind);
                out->err = 1;
                return 0;
            }
            if (!cemit_str(out, b->value ? b->value : "")) return 0;
            continue;
        }
        {
            const char* lit = p;
            while (*p && !(p[0] == '$' && p[1] == '{')) p++;
            if (!cemit_buf(out, lit, (size_t)(p - lit))) return 0;
        }
    }
    return 1;
}

/* Named-hole emit: ("indent", ind, "call", call, NULL). All values IDENT/EXPR. */
static int shadow_tpl_kv(CEmit* out, const char* tpl, ...) {
    ShadowTplBind binds[48];
    char names[48][32];
    int n = 0;
    va_list ap;
    if (!out || !tpl) return 0;
    va_start(ap, tpl);
    for (;;) {
        const char* name = va_arg(ap, const char*);
        const char* val;
        size_t nl;
        if (!name) break;
        if (n >= 48) {
            fprintf(stderr, "error: shadow_tpl_kv: too many binds\n");
            out->err = 1;
            va_end(ap);
            return 0;
        }
        val = va_arg(ap, const char*);
        nl = strlen(name);
        if (nl >= sizeof(names[n])) nl = sizeof(names[n]) - 1;
        memcpy(names[n], name, nl);
        names[n][nl] = 0;
        binds[n].name = names[n];
        binds[n].kind = SHADOW_TPL_EXPR;
        binds[n].value = val ? val : "";
        n++;
    }
    va_end(ap);
    return shadow_tpl_emit(out, tpl, binds, n);
}

/* Compatibility shim: former printf-shaped emit → ${0}/${1}/… templates.
 * No vsnprintf; payload args never re-enter a format parser. Prefer
 * shadow_tpl_emit / shadow_tpl_kv with named holes for new code. */
static int cemit_fmt(CEmit* e, const char* fmt, ...) {
    char tpl_stack[2048];
    char* tpl = tpl_stack;
    size_t tpl_cap = sizeof(tpl_stack);
    size_t tpl_len = 0;
    ShadowTplBind binds[48];
    char names[48][8];
    char numbufs[48][64];
    int nbind = 0;
    va_list ap;
    const char* p;
    int ok;
    if (!e || !fmt) return 0;
    va_start(ap, fmt);
    p = fmt;
    while (*p) {
        if (*p != '%') {
            const char* start = p;
            while (*p && *p != '%') p++;
            {
                size_t n = (size_t)(p - start);
                if (tpl_len + n + 1 > tpl_cap) {
                    size_t ncap = tpl_cap * 2;
                    char* nt;
                    while (ncap < tpl_len + n + 1) ncap *= 2;
                    nt = (char*)malloc(ncap);
                    if (!nt) {
                        e->err = 1;
                        va_end(ap);
                        if (tpl != tpl_stack) free(tpl);
                        return 0;
                    }
                    memcpy(nt, tpl, tpl_len);
                    if (tpl != tpl_stack) free(tpl);
                    tpl = nt;
                    tpl_cap = ncap;
                }
                memcpy(tpl + tpl_len, start, n);
                tpl_len += n;
            }
            continue;
        }
        p++;
        if (*p == '%') {
            if (tpl_len + 2 > tpl_cap) {
                /* rare; grow */
                size_t ncap = tpl_cap * 2;
                char* nt = (char*)malloc(ncap);
                if (!nt) {
                    e->err = 1;
                    va_end(ap);
                    if (tpl != tpl_stack) free(tpl);
                    return 0;
                }
                memcpy(nt, tpl, tpl_len);
                if (tpl != tpl_stack) free(tpl);
                tpl = nt;
                tpl_cap = ncap;
            }
            tpl[tpl_len++] = '%';
            p++;
            continue;
        }
        if (nbind >= 48) {
            fprintf(stderr, "error: cemit_fmt: too many holes\n");
            e->err = 1;
            va_end(ap);
            if (tpl != tpl_stack) free(tpl);
            return 0;
        }
        snprintf(names[nbind], sizeof(names[nbind]), "%d", nbind);
        binds[nbind].name = names[nbind];
        binds[nbind].kind = SHADOW_TPL_EXPR;
        if (*p == 's') {
            const char* s = va_arg(ap, const char*);
            binds[nbind].value = s ? s : "";
            p++;
        } else if (*p == 'd' || *p == 'i') {
            int v = va_arg(ap, int);
            snprintf(numbufs[nbind], sizeof(numbufs[nbind]), "%d", v);
            binds[nbind].value = numbufs[nbind];
            p++;
        } else if (*p == 'u') {
            unsigned v = va_arg(ap, unsigned);
            snprintf(numbufs[nbind], sizeof(numbufs[nbind]), "%u", v);
            binds[nbind].value = numbufs[nbind];
            p++;
        } else if (*p == 'l' && p[1] == 'l' && p[2] == 'u') {
            unsigned long long v = va_arg(ap, unsigned long long);
            snprintf(numbufs[nbind], sizeof(numbufs[nbind]), "%llu", v);
            binds[nbind].value = numbufs[nbind];
            p += 3;
        } else {
            fprintf(stderr,
                    "error: cemit_fmt: unsupported format near '%%%s'\n", p);
            e->err = 1;
            va_end(ap);
            if (tpl != tpl_stack) free(tpl);
            return 0;
        }
        {
            char hole[16];
            int hl = snprintf(hole, sizeof(hole), "${%d}", nbind);
            if (hl < 0 || tpl_len + (size_t)hl + 1 > tpl_cap) {
                size_t ncap = tpl_cap * 2;
                char* nt;
                while (ncap < tpl_len + 16) ncap *= 2;
                nt = (char*)malloc(ncap);
                if (!nt) {
                    e->err = 1;
                    va_end(ap);
                    if (tpl != tpl_stack) free(tpl);
                    return 0;
                }
                memcpy(nt, tpl, tpl_len);
                if (tpl != tpl_stack) free(tpl);
                tpl = nt;
                tpl_cap = ncap;
            }
            memcpy(tpl + tpl_len, hole, (size_t)hl);
            tpl_len += (size_t)hl;
        }
        nbind++;
    }
    va_end(ap);
    tpl[tpl_len] = 0;
    ok = shadow_tpl_emit(e, tpl, binds, nbind);
    if (tpl != tpl_stack) free(tpl);
    return ok;
}

/* Schema check: every hole in tpl has a schema entry with a compatible kind. */
static int shadow_tpl_check(const char* id, const char* tpl,
                            const ShadowTplBind* schema, int nschema) {
    const char* p;
    if (!tpl || !schema) return 0;
    p = tpl;
    while (*p) {
        if (p[0] == '$' && p[1] == '{') {
            char name[64], prefix[64];
            int hint = 0;
            const char* start = p;
            const ShadowTplBind* b;
            if (!shadow_tpl_parse_hole(&p, name, sizeof(name), prefix,
                                      sizeof(prefix), &hint)) {
                fprintf(stderr,
                        "error: emit template '%s': malformed hole near "
                        "'%.32s'\n",
                        id ? id : "?", start);
                return 0;
            }
            if (hint == SHADOW_TPL_LINE || strcmp(name, "line") == 0 ||
                hint == SHADOW_TPL_FILE || strcmp(name, "file") == 0)
                continue;
            b = shadow_tpl_find(schema, nschema, name, strlen(name));
            if (!b) {
                fprintf(stderr,
                        "error: emit template '%s': hole '${%s}' not in schema\n",
                        id ? id : "?", name);
                return 0;
            }
            if (b->kind == SHADOW_TPL_STMTS || hint == SHADOW_TPL_STMTS ||
                prefix[0]) {
                if (b->kind != SHADOW_TPL_STMTS) {
                    fprintf(stderr,
                            "error: emit template '%s': '${%s}' schema kind "
                            "must be STMTS\n",
                            id ? id : "?", name);
                    return 0;
                }
                if (prefix[0] &&
                    !shadow_tpl_find(schema, nschema, prefix, strlen(prefix))) {
                    fprintf(stderr,
                            "error: emit template '%s': stmts prefix '%s' not "
                            "in schema\n",
                            id ? id : "?", prefix);
                    return 0;
                }
            } else if (b->kind != SHADOW_TPL_IDENT &&
                       b->kind != SHADOW_TPL_EXPR) {
                fprintf(stderr,
                        "error: emit template '%s': '${%s}' schema kind must be "
                        "IDENT/EXPR\n",
                        id ? id : "?", name);
                return 0;
            }
            continue;
        }
        p++;
    }
    return 1;
}

/* --- Unwrap pilot templates ------------------------------------------------ */

static const char k_tpl_try_assign[] =
    "${indent}${ty_lhs} ${name};\n"
    "${indent}{\n"
    "${i1}__typeof__(${call}) __r = ${call};\n"
    "${i1}if (!__r.ok) {\n"
    "${err_body}"
    "${i1}}\n"
    "${i1}${name} = ${value_expr};\n"
    "${indent}}\n";

static const char k_tpl_try_call_open[] =
    "${indent}{\n"
    "${i1}__typeof__(${call}) __r = ${call};\n"
    "${i1}if (__cc_uw_is_err(__r)) {\n";

static const char k_tpl_try_call_close[] = "${i1}}\n"
                                          "${indent}}\n";

static const char k_tpl_bang_assign_open[] =
    "${indent}${ty_lhs} ${name};\n"
    "${indent}{\n"
    "${i1}__typeof__(${call}) __r = ${call};\n"
    "${i1}if (!__r.ok) {\n"
    "${i2}__typeof__(__r.u.error) ${bind} = __r.u.error;\n";

static const char k_tpl_bang_assign_close[] =
    "${i1}}\n"
    "${i1}${name} = __r.u.value;\n"
    "${indent}}\n";

static const ShadowTplBind k_schema_try_assign[] = {
    {"indent", SHADOW_TPL_IDENT, NULL},
    {"i1", SHADOW_TPL_IDENT, NULL},
    {"i2", SHADOW_TPL_IDENT, NULL},
    {"ty_lhs", SHADOW_TPL_IDENT, NULL},
    {"name", SHADOW_TPL_IDENT, NULL},
    {"call", SHADOW_TPL_EXPR, NULL},
    {"value_expr", SHADOW_TPL_EXPR, NULL},
    {"err_body", SHADOW_TPL_STMTS, NULL},
};

static const ShadowTplBind k_schema_try_call[] = {
    {"indent", SHADOW_TPL_IDENT, NULL},
    {"i1", SHADOW_TPL_IDENT, NULL},
    {"call", SHADOW_TPL_EXPR, NULL},
};

static const ShadowTplBind k_schema_bang_assign[] = {
    {"indent", SHADOW_TPL_IDENT, NULL},
    {"i1", SHADOW_TPL_IDENT, NULL},
    {"i2", SHADOW_TPL_IDENT, NULL},
    {"ty_lhs", SHADOW_TPL_IDENT, NULL},
    {"name", SHADOW_TPL_IDENT, NULL},
    {"call", SHADOW_TPL_EXPR, NULL},
    {"bind", SHADOW_TPL_IDENT, NULL},
};

/* --- Closure / Result / TU prelude (Phase 1 expand) ------------------------ */

static const char k_tpl_closure0_nocaps[] =
    "\n/* CC closure ${id} */\n"
    "static CCClosure0 cc_closure__N${id}_make(void) {\n"
    "    return cc_closure0_make(cc_closure__N${id}_entry, NULL, NULL);\n"
    "}\n"
    "static void* cc_closure__N${id}_entry(void* __p) {\n"
    "    (void)__p;\n";

static const char k_tpl_result_spec_value[] =
    "typedef struct { int ok; union { ${ok_ty} value; ${err_ty} error; } u; } "
    "${rname};\n"
    "static inline ${rname} cc_ok_${rname}(${ok_ty} v) {"
    " ${rname} r; r.ok = 1; r.u.value = v; return r; }\n";

static const char k_tpl_result_spec_void[] =
    "typedef struct { int ok; union { ${err_ty} error; } u; } ${rname};\n";

static const char k_tpl_tu_prelude_c[] =
    "/* generated by Concurrent-C (ccc / shadow_lower) */\n"
    "#include <stddef.h>\n"
    "#include <stdint.h>\n";

static const char k_tpl_tu_prelude_h[] =
    "/* generated by Concurrent-C (ccc / shadow_lower) — .cch → .h */\n"
    "#pragma once\n"
    "#include <stddef.h>\n";

static const ShadowTplBind k_schema_closure_id[] = {
    {"id", SHADOW_TPL_IDENT, NULL},
};

static const ShadowTplBind k_schema_result_spec[] = {
    {"rname", SHADOW_TPL_IDENT, NULL},
    {"ok_ty", SHADOW_TPL_IDENT, NULL},
    {"err_ty", SHADOW_TPL_IDENT, NULL},
};

static const ShadowTplBind k_schema_empty[] = {
    {"_", SHADOW_TPL_IDENT, NULL}, /* unused; no holes */
};

static int shadow_tpl_selfcheck(void) {
    if (!shadow_tpl_check("try_assign", k_tpl_try_assign, k_schema_try_assign,
                          (int)(sizeof(k_schema_try_assign) /
                                sizeof(k_schema_try_assign[0]))))
        return 0;
    if (!shadow_tpl_check("try_call_open", k_tpl_try_call_open, k_schema_try_call,
                          (int)(sizeof(k_schema_try_call) /
                                sizeof(k_schema_try_call[0]))))
        return 0;
    if (!shadow_tpl_check("try_call_close", k_tpl_try_call_close,
                          k_schema_try_call,
                          (int)(sizeof(k_schema_try_call) /
                                sizeof(k_schema_try_call[0]))))
        return 0;
    if (!shadow_tpl_check("bang_assign_open", k_tpl_bang_assign_open,
                          k_schema_bang_assign,
                          (int)(sizeof(k_schema_bang_assign) /
                                sizeof(k_schema_bang_assign[0]))))
        return 0;
    if (!shadow_tpl_check("bang_assign_close", k_tpl_bang_assign_close,
                          k_schema_bang_assign,
                          (int)(sizeof(k_schema_bang_assign) /
                                sizeof(k_schema_bang_assign[0]))))
        return 0;
    if (!shadow_tpl_check("closure0_nocaps", k_tpl_closure0_nocaps,
                          k_schema_closure_id,
                          (int)(sizeof(k_schema_closure_id) /
                                sizeof(k_schema_closure_id[0]))))
        return 0;
    if (!shadow_tpl_check("result_spec_value", k_tpl_result_spec_value,
                          k_schema_result_spec,
                          (int)(sizeof(k_schema_result_spec) /
                                sizeof(k_schema_result_spec[0]))))
        return 0;
    if (!shadow_tpl_check("result_spec_void", k_tpl_result_spec_void,
                          k_schema_result_spec,
                          (int)(sizeof(k_schema_result_spec) /
                                sizeof(k_schema_result_spec[0]))))
        return 0;
    if (!shadow_tpl_check("tu_prelude_c", k_tpl_tu_prelude_c, k_schema_empty,
                          0))
        return 0;
    if (!shadow_tpl_check("tu_prelude_h", k_tpl_tu_prelude_h, k_schema_empty, 0))
        return 0;
    return 1;
}
