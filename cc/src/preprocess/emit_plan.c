/*
 * CCEmitPlan — unified splice anchors (track A2).
 */
#include "emit_plan.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include "comptime/executor.h"
#include "comptime/hook_compile.h"
#include "preprocess/emit_limits.h"
#include "preprocess/preprocess.h"
#include "preprocess/template_scan.h"
#include "util/path.h"
#include "util/text.h"

typedef struct CCEmitComptimeFragment {
    CCEmitAnchor anchor;
    size_t site_pos;
    char* text;
} CCEmitComptimeFragment;

static CCEmitComptimeFragment cc__comptime_frags[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS];
static size_t cc__comptime_frag_count = 0;

static void cc__exec_ranges_clear(void);
static void cc__generic_factories_clear(void);

/* Thin aliases onto the shared scanners in util/text.h.  Kept as file-local
 * names so the @comptime intrinsic collectors below read uniformly; the logic
 * has a single source of truth shared with symbols.c. */
#define cc__emit_match_kw(src, len, pos, kw)  cc_match_ident_kw((src), (len), (pos), (kw))
#define cc__emit_parse_c_string(src, len, pos, out, cap) \
    cc_parse_c_string_literal((src), (len), (pos), (out), (cap))

static int cc__emit_parse_ident(const char* src, size_t len, size_t* pos,
                                char* out, size_t cap) {
    size_t p = cc_skip_ws_len(src, len, *pos);
    size_t start;
    size_t n;
    if (p >= len || !cc_is_ident_start(src[p])) return 0;
    start = p++;
    while (p < len && cc_is_ident_char(src[p])) p++;
    n = p - start;
    if (n >= cap) n = cap - 1;
    memcpy(out, src + start, n);
    out[n] = '\0';
    *pos = p;
    return 1;
}

static int cc__emit_parse_anchor(const char* src, size_t len, size_t* pos, CCEmitAnchor* out) {
    size_t p = cc_skip_ws_len(src, len, *pos);
    if (p >= len) return 0;
    if (src[p] >= '0' && src[p] <= '9') {
        int v = 0;
        while (p < len && src[p] >= '0' && src[p] <= '9') {
            v = v * 10 + (src[p] - '0');
            p++;
        }
        if (v < 0 || v > (int)CC_EMIT_AT_COMPTIME_SITE) return 0;
        *out = (CCEmitAnchor)v;
        *pos = p;
        return 1;
    }
    if (cc__emit_match_kw(src, len, p, "CC_EMIT_AFTER_PRELUDE")) {
        *out = CC_EMIT_AFTER_PRELUDE;
        *pos = p + strlen("CC_EMIT_AFTER_PRELUDE");
        return 1;
    }
    if (cc__emit_match_kw(src, len, p, "CC_EMIT_BEFORE_FIRST_USE")) {
        *out = CC_EMIT_BEFORE_FIRST_USE;
        *pos = p + strlen("CC_EMIT_BEFORE_FIRST_USE");
        return 1;
    }
    if (cc__emit_match_kw(src, len, p, "CC_EMIT_AT_COMPTIME_SITE")) {
        *out = CC_EMIT_AT_COMPTIME_SITE;
        *pos = p + strlen("CC_EMIT_AT_COMPTIME_SITE");
        return 1;
    }
    return 0;
}

static int cc__emit_try_collect_cc_emit_cstr(const char* src, size_t len, size_t call_pos,
                                             size_t site_pos) {
    size_t p = call_pos + strlen("cc_emit_cstr");
    CCEmitAnchor anchor;
    char frag[4096];
    if (cc__comptime_frag_count >= CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS) return 0;
    p = cc_skip_ws_len(src, len, p);
    if (p >= len || src[p] != '(') return 0;
    p++;
    if (!cc__emit_parse_anchor(src, len, &p, &anchor)) return 0;
    p = cc_skip_ws_len(src, len, p);
    if (p >= len || src[p] != ',') return 0;
    p = cc_skip_ws_len(src, len, p + 1);
    if (!cc__emit_parse_c_string(src, len, &p, frag, sizeof(frag))) return 0;
    if (strlen(frag) + 1 >= sizeof(frag)) {
        fprintf(stderr, "error: cc_emit_cstr fragment exceeds %d bytes\n", CC_EMIT_FRAGMENT_MAX);
        return 0;
    }
    {
        CCEmitComptimeFragment* f = &cc__comptime_frags[cc__comptime_frag_count++];
        f->anchor = anchor;
        f->site_pos = site_pos;
        f->text = strdup(frag);
        if (!f->text) {
            cc__comptime_frag_count--;
            return 0;
        }
    }
    return 1;
}

/* cc_emit_format(anchor, "fmt", args...) — printf-style comptime emission.
 * Supports %s (string-literal arg), %d/%i (integer-literal arg) and %%.  The
 * substituted fragment is stored exactly like cc_emit_cstr, so it flows through
 * the same anchor splice.  On any malformed/unsupported conversion we bail
 * (return 0) and the call is simply not collected — matching cc_emit_cstr's
 * best-effort contract at this text-collection stage. */
static int cc__emit_try_collect_cc_emit_format(const char* src, size_t len, size_t call_pos,
                                               size_t site_pos) {
    size_t p = call_pos + strlen("cc_emit_format");
    CCEmitAnchor anchor;
    char fmt[4096];
    char frag[4096];
    if (cc__comptime_frag_count >= CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS) return 0;
    p = cc_skip_ws_len(src, len, p);
    if (p >= len || src[p] != '(') return 0;
    p++;
    if (!cc__emit_parse_anchor(src, len, &p, &anchor)) return 0;
    p = cc_skip_ws_len(src, len, p);
    if (p >= len || src[p] != ',') return 0;
    p = cc_skip_ws_len(src, len, p + 1);
    if (!cc__emit_parse_c_string(src, len, &p, fmt, sizeof(fmt))) return 0;

    size_t o = 0;
    for (size_t i = 0; fmt[i]; i++) {
        if (fmt[i] != '%') {
            if (o + 1 >= sizeof(frag)) {
                fprintf(stderr, "error: cc_emit_format fragment exceeds %d bytes\n",
                        CC_EMIT_FRAGMENT_MAX);
                return 0;
            }
            frag[o++] = fmt[i];
            continue;
        }
        char c = fmt[i + 1];
        if (c == '%') {
            if (o + 1 >= sizeof(frag)) return 0;
            frag[o++] = '%';
            i++;
            continue;
        }
        if (c != 's' && c != 'd' && c != 'i') return 0;  /* unsupported conv */
        /* pull the next argument: `, <arg>` */
        p = cc_skip_ws_len(src, len, p);
        if (p >= len || src[p] != ',') return 0;
        p = cc_skip_ws_len(src, len, p + 1);
        if (c == 's') {
            char arg[1024];
            if (!cc__emit_parse_c_string(src, len, &p, arg, sizeof(arg))) return 0;
            for (size_t k = 0; arg[k]; k++) {
                if (o + 1 >= sizeof(frag)) return 0;
                frag[o++] = arg[k];
            }
        } else {
            int neg = 0;
            if (p < len && (src[p] == '+' || src[p] == '-')) { neg = (src[p] == '-'); p++; }
            if (p >= len || src[p] < '0' || src[p] > '9') return 0;
            long v = 0;
            while (p < len && src[p] >= '0' && src[p] <= '9') { v = v * 10 + (src[p] - '0'); p++; }
            if (neg) v = -v;
            char num[32];
            int nl = snprintf(num, sizeof(num), "%ld", v);
            if (nl < 0) return 0;
            for (int k = 0; k < nl; k++) {
                if (o + 1 >= sizeof(frag)) return 0;
                frag[o++] = num[k];
            }
        }
        i++;  /* consumed the conversion char */
    }
    frag[o] = 0;

    {
        CCEmitComptimeFragment* f = &cc__comptime_frags[cc__comptime_frag_count++];
        f->anchor = anchor;
        f->site_pos = site_pos;
        f->text = strdup(frag);
        if (!f->text) { cc__comptime_frag_count--; return 0; }
    }
    return 1;
}

/* --- user generic factories (D6.0: declarative templates) --- */

typedef struct CCGenericTemplate {
    char* name;
    int   arity;
    char* tmpl;
} CCGenericTemplate;

static CCGenericTemplate cc__generic_templates[CC_EMIT_PLAN_MAX_GENERIC_TEMPLATES];
static size_t cc__generic_template_count = 0;

/* Mangled names whose definition has already been emitted this TU (dedup). */
static char* cc__generic_emitted[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS];
static size_t cc__generic_emitted_count = 0;

/* Mangled names already reported as producing invalid C this TU (dedup across
 * the preprocess and codegen rewrite passes). */
static char* cc__generic_invalid[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS];
static size_t cc__generic_invalid_count = 0;

int cc_emit_plan_generic_invalid_report_once(const char* mangled) {
    if (!mangled) return 0;
    for (size_t i = 0; i < cc__generic_invalid_count; i++)
        if (strcmp(cc__generic_invalid[i], mangled) == 0) return 0;
    if (cc__generic_invalid_count >= CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS) return 1;
    {
        char* m = strdup(mangled);
        if (!m) return 1;
        cc__generic_invalid[cc__generic_invalid_count++] = m;
    }
    return 1;
}

void cc_emit_plan_register_generic_template(const char* name, int arity,
                                            const char* template_src) {
    char norm[CC_GENERIC_TEMPLATE_MAX];
    const char* stored = template_src;
    if (!name || !template_src) return;
    if (!cc_template_normalize_legacy_positional(template_src, norm, sizeof(norm))) {
        fprintf(stderr, "error: generic template '%s' exceeds %d bytes\n",
                name, CC_GENERIC_TEMPLATE_MAX);
        return;
    }
    stored = norm;
    for (size_t i = 0; i < cc__generic_template_count; i++) {
        if (strcmp(cc__generic_templates[i].name, name) == 0) {
            char* nt = strdup(stored);
            if (!nt) return;
            free(cc__generic_templates[i].tmpl);
            cc__generic_templates[i].tmpl = nt;
            cc__generic_templates[i].arity = arity;
            return;
        }
    }
    if (cc__generic_template_count >= CC_EMIT_PLAN_MAX_GENERIC_TEMPLATES) return;
    {
        CCGenericTemplate* t = &cc__generic_templates[cc__generic_template_count];
        t->name = strdup(name);
        t->tmpl = strdup(stored);
        t->arity = arity;
        if (!t->name || !t->tmpl) { free(t->name); free(t->tmpl); return; }
        cc__generic_template_count++;
    }
}

const char* cc_emit_plan_lookup_generic_template(const char* name, int* out_arity) {
    if (!name) return NULL;
    for (size_t i = 0; i < cc__generic_template_count; i++) {
        if (strcmp(cc__generic_templates[i].name, name) == 0) {
            if (out_arity) *out_arity = cc__generic_templates[i].arity;
            return cc__generic_templates[i].tmpl;
        }
    }
    return NULL;
}

int cc_emit_plan_generic_def_emit_once(const char* mangled, const char* def_text) {
    if (!mangled || !def_text) return 0;
    for (size_t i = 0; i < cc__generic_emitted_count; i++)
        if (strcmp(cc__generic_emitted[i], mangled) == 0) return 0;
    if (cc__comptime_frag_count >= CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS) return 0;
    if (cc__generic_emitted_count >= CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS) return 0;
    {
        char* m = strdup(mangled);
        char* d = strdup(def_text);
        if (!m || !d) { free(m); free(d); return 0; }
        CCEmitComptimeFragment* f = &cc__comptime_frags[cc__comptime_frag_count++];
        f->anchor = CC_EMIT_AFTER_PRELUDE;
        f->site_pos = 0;
        f->text = d;
        cc__generic_emitted[cc__generic_emitted_count++] = m;
    }
    return 1;
}

static void cc__generic_templates_clear(void) {
    for (size_t i = 0; i < cc__generic_template_count; i++) {
        free(cc__generic_templates[i].name);
        free(cc__generic_templates[i].tmpl);
    }
    cc__generic_template_count = 0;
    for (size_t i = 0; i < cc__generic_emitted_count; i++) free(cc__generic_emitted[i]);
    cc__generic_emitted_count = 0;
    for (size_t i = 0; i < cc__generic_invalid_count; i++) free(cc__generic_invalid[i]);
    cc__generic_invalid_count = 0;
}

void cc_emit_plan_clear_generic_factory_registrations(void) {
    cc__generic_factories_clear();
}

/* --- user generic factories (D6.1: compiled handlers) --- */

typedef struct CCGenericFactoryReg {
    char* name;
    char* handler_name;
    const void* fn_ptr;
    void* owner;
} CCGenericFactoryReg;

static CCGenericFactoryReg cc__generic_factories[CC_EMIT_PLAN_MAX_GENERIC_TEMPLATES];
static size_t cc__generic_factory_count = 0;

static void cc__generic_factories_clear(void) {
    for (size_t i = 0; i < cc__generic_factory_count; i++) {
        free(cc__generic_factories[i].name);
        free(cc__generic_factories[i].handler_name);
        if (cc__generic_factories[i].owner) {
            cc_comptime_type_hook_owner_free(cc__generic_factories[i].owner);
            cc__generic_factories[i].owner = NULL;
        }
    }
    cc__generic_factory_count = 0;
}

void cc_emit_plan_register_generic_factory(const char* name, const char* handler_name) {
    if (!name || !handler_name) return;
    for (size_t i = 0; i < cc__generic_factory_count; i++) {
        if (strcmp(cc__generic_factories[i].name, name) == 0) {
            char* nh = strdup(handler_name);
            if (!nh) return;
            if (strcmp(cc__generic_factories[i].handler_name, handler_name) != 0)
                cc__generic_factories[i].fn_ptr = NULL;
            free(cc__generic_factories[i].handler_name);
            cc__generic_factories[i].handler_name = nh;
            return;
        }
    }
    if (cc__generic_factory_count >= CC_EMIT_PLAN_MAX_GENERIC_TEMPLATES) return;
    {
        CCGenericFactoryReg* f = &cc__generic_factories[cc__generic_factory_count++];
        f->name = strdup(name);
        f->handler_name = strdup(handler_name);
        f->fn_ptr = NULL;
        f->owner = NULL;
        if (!f->name || !f->handler_name) {
            free(f->name);
            free(f->handler_name);
            cc__generic_factory_count--;
        }
    }
}

const void* cc_emit_plan_lookup_generic_factory(const char* name) {
    if (!name) return NULL;
    for (size_t i = 0; i < cc__generic_factory_count; i++) {
        if (strcmp(cc__generic_factories[i].name, name) == 0)
            return cc__generic_factories[i].fn_ptr;
    }
    return NULL;
}

const char* cc_emit_plan_lookup_generic_factory_handler(const char* name) {
    if (!name) return NULL;
    for (size_t i = 0; i < cc__generic_factory_count; i++) {
        if (strcmp(cc__generic_factories[i].name, name) == 0)
            return cc__generic_factories[i].handler_name;
    }
    return NULL;
}

/* ABI mirrors cc_slice.cch / cc_arena.cch for dylib factory calls. */
typedef struct CCFactorySlice {
    void*    ptr;
    size_t   len;
    uint64_t id;
    size_t   alen;
} CCFactorySlice;

typedef struct {
    CCFactorySlice* items;
    size_t          len;
} CCFactorySliceArray;

static CCFactorySlice cc__factory_slice_cstr(const char* s) {
    CCFactorySlice sl = {0};
    if (s) { sl.ptr = (void*)s; sl.len = strlen(s); }
    return sl;
}

typedef CCFactorySlice (*CCGenericFactoryFn)(CCFactorySlice, CCFactorySlice,
                                               CCFactorySliceArray, void*);

int cc_emit_plan_invoke_generic_factory(const char* name, const char* mangled,
                                        const char type_args[8][128], int nargs,
                                        char* def_out, size_t def_cap) {
    const void* fn = cc_emit_plan_lookup_generic_factory(name);
    CCFactorySliceArray args = {0};
    CCFactorySlice arg_slices[8];
    CCGenericFactoryFn call;
    CCFactorySlice result;
    if (!fn || !mangled || !def_out || def_cap == 0 || nargs <= 0 || nargs > 8) return 0;
    for (int i = 0; i < nargs; i++)
        arg_slices[i] = cc__factory_slice_cstr(type_args[i]);
    args.items = arg_slices;
    args.len = (size_t)nargs;
    call = (CCGenericFactoryFn)(uintptr_t)fn;
    result = call(cc__factory_slice_cstr(name),
                  cc__factory_slice_cstr(mangled),
                  args,
                  NULL);
    if (!result.ptr || result.len == 0) return 0;
    if (result.len >= def_cap) {
        fprintf(stderr,
                "error: compiled generic factory '%s' output (%zu bytes) exceeds %zu byte limit\n",
                name, result.len, def_cap);
        return 0;
    }
    memcpy(def_out, result.ptr, result.len);
    def_out[result.len] = '\0';
    return 1;
}

static int cc__compile_generic_factory_reg(CCGenericFactoryReg* f, const char* input_path,
                                           char* err_buf, size_t err_sz) {
    static char entry_name[128];
    CCComptimeHookSpec spec = {0};
    const void* fn_ptr = NULL;
    void* owner = NULL;
    const char* def;
    if (!f) return -1;
    if (f->fn_ptr) return 0;
    def = cc_comptime_fn_registry_lookup_def(f->handler_name);
    if (!def || !def[0]) {
        if (err_buf && err_sz)
            snprintf(err_buf, err_sz, "factory handler '%s' not found in registry",
                     f->handler_name);
        return -1;
    }
    snprintf(entry_name, sizeof(entry_name), "__cc_gen_factory_%s", f->handler_name);
    spec.kind = CC_COMPTIME_TYPE_HOOK_GENERIC_FACTORY;
    spec.entry_name = entry_name;
    spec.handler_name = f->handler_name;
    if (cc_comptime_compile_type_hooks_tu_ex(input_path, def, &spec, 1, &owner, &fn_ptr,
                                             err_buf, err_sz) != 0)
        return -1;
    f->fn_ptr = fn_ptr;
    f->owner = owner;
    return 0;
}

int cc_emit_plan_ensure_generic_factory(const char* generic_name, const char* input_path,
                                        char* err_buf, size_t err_sz) {
    if (!generic_name) return -1;
    for (size_t i = 0; i < cc__generic_factory_count; i++) {
        if (strcmp(cc__generic_factories[i].name, generic_name) == 0)
            return cc__compile_generic_factory_reg(&cc__generic_factories[i], input_path,
                                                   err_buf, err_sz);
    }
    if (err_buf && err_sz)
        snprintf(err_buf, err_sz, "generic '%s' is not registered", generic_name);
    return -1;
}

int cc_emit_plan_compile_generic_factories(const char* src, size_t len,
                                           const char* input_path) {
    (void)src;
    (void)len;
    for (size_t i = 0; i < cc__generic_factory_count; i++) {
        if (cc__compile_generic_factory_reg(&cc__generic_factories[i], input_path, NULL, 0) != 0)
            return -1;
    }
    return 0;
}

/* cc_generic_template("Name", arity, "template...") — register a library
 * generic.  Template is one or more adjacent C string literals or a single
 * backtick template; ${name} expands to the mangled name and ${0}..${N} to
 * the type arguments at the use site.  Legacy $0/$1 are normalized at registration. */
static int cc__emit_scan_backtick_template(const char* src, size_t len, size_t* pos,
                                           char* out, size_t out_cap) {
    size_t p = cc_skip_ws_len(src, len, *pos);
    if (p >= len || src[p] != '`') return 0;
    size_t tick_e = 0;
    if (cc_scan_template_literal_end(src, len, p, &tick_e) != 0) return 0;
    {
        size_t start = p + 1;
        size_t n = tick_e - start;
        if (n >= out_cap) return 0;
        memcpy(out, src + start, n);
        out[n] = '\0';
        *pos = tick_e + 1;
        return 1;
    }
}

static int cc__emit_try_collect_cc_generic_template(const char* src, size_t len, size_t call_pos) {
    size_t p = call_pos + strlen("cc_generic_template");
    char name[128];
    char tmpl[CC_GENERIC_TEMPLATE_MAX];
    size_t tlen = 0;
    int arity = 0;
    p = cc_skip_ws_len(src, len, p);
    if (p >= len || src[p] != '(') return 0;
    p++;
    if (!cc__emit_parse_c_string(src, len, &p, name, sizeof(name))) return 0;
    p = cc_skip_ws_len(src, len, p);
    if (p >= len || src[p] != ',') return 0;
    p = cc_skip_ws_len(src, len, p + 1);
    if (p >= len || src[p] < '0' || src[p] > '9') return 0;
    while (p < len && src[p] >= '0' && src[p] <= '9') { arity = arity * 10 + (src[p] - '0'); p++; }
    p = cc_skip_ws_len(src, len, p);
    if (p >= len || src[p] != ',') return 0;
    p = cc_skip_ws_len(src, len, p + 1);
    if (cc__emit_scan_backtick_template(src, len, &p, tmpl, sizeof(tmpl))) {
        cc_emit_plan_register_generic_template(name, arity, tmpl);
        return 1;
    }
    {
        char piece[CC_EMIT_FRAGMENT_MAX];
        int got_any = 0;
        for (;;) {
            p = cc_skip_ws_len(src, len, p);
            if (p >= len || src[p] != '"') break;
            if (!cc__emit_parse_c_string(src, len, &p, piece, sizeof(piece))) break;
            {
                size_t pl = strlen(piece);
                if (tlen + pl >= sizeof(tmpl)) {
                    fprintf(stderr,
                            "error: generic template '%s' exceeds %d bytes\n",
                            name, CC_GENERIC_TEMPLATE_MAX);
                    return 0;
                }
                memcpy(tmpl + tlen, piece, pl);
                tlen += pl;
            }
            got_any = 1;
        }
        if (!got_any) return 0;
        tmpl[tlen] = 0;
    }
    cc_emit_plan_register_generic_template(name, arity, tmpl);
    return 1;
}

/* cc_generic_register("Name", handler_fn) — register a compiled factory. */
static int cc__emit_try_collect_cc_generic_register(const char* src, size_t len, size_t call_pos) {
    size_t p = call_pos + strlen("cc_generic_register");
    char name[128];
    char handler[128];
    p = cc_skip_ws_len(src, len, p);
    if (p >= len || src[p] != '(') return 0;
    p++;
    if (!cc__emit_parse_c_string(src, len, &p, name, sizeof(name))) return 0;
    p = cc_skip_ws_len(src, len, p);
    if (p >= len || src[p] != ',') return 0;
    p = cc_skip_ws_len(src, len, p + 1);
    if (!cc__emit_parse_ident(src, len, &p, handler, sizeof(handler))) return 0;
    cc_emit_plan_register_generic_factory(name, handler);
    return 1;
}

/* --- comptime explicit instantiation requests (track C1) --- */

typedef struct CCEmitComptimeInst {
    CCTypeGraphRequestKind kind;
    char a[128];   /* vec elem / map key / chan elem */
    char b[128];   /* map val (unused otherwise) */
} CCEmitComptimeInst;

static CCEmitComptimeInst cc__comptime_insts[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS];
static size_t cc__comptime_inst_count = 0;

void cc_emit_plan_clear_comptime_instantiations(void) {
    cc__comptime_inst_count = 0;
}

size_t cc_emit_plan_comptime_instantiation_count(void) {
    return cc__comptime_inst_count;
}

/* ---- Comptime intrinsic registry ----
 *
 * The compiler interprets a fixed set of builtin calls that appear inside
 * `@comptime { … }`.  Rather than open-coding an `if (match_kw …)` chain per
 * intrinsic, they are listed once in `cc__comptime_intrinsics`; the dispatcher
 * (`cc__emit_visit_dispatch`) walks each block body and routes a matched call
 * to its handler.  Each public collector simply selects the group(s) it cares
 * about via a mask.  This is the last text-matching layer before a real
 * `@comptime` evaluator — adding/replacing an intrinsic is a table edit.
 *
 * Note: `cc_type_register` / `cc_type_define` are *also* comptime intrinsics
 * but live in comptime/symbols.c, a different evaluation stage that drives
 * dylib compilation and propagates parse errors.  They share this module's
 * block recognizer and lexers (util/text.h) but not its static-buffer
 * collectors. */
enum { CC_CI_INSTANTIATE = 1u << 0, CC_CI_EMIT = 1u << 1 };

typedef struct CCComptimeIntrinsicDesc CCComptimeIntrinsicDesc;
typedef int (*CCComptimeIntrinsicFn)(const CCComptimeIntrinsicDesc* d,
                                     const char* src, size_t len,
                                     size_t pos, size_t splice_end);
struct CCComptimeIntrinsicDesc {
    const char*            name;    /* exact call keyword to match            */
    unsigned               group;   /* CC_CI_* bit (which collector wants it) */
    CCComptimeIntrinsicFn  collect; /* parse the matched call                 */
    CCTypeGraphRequestKind kind;    /* instantiate handler: request kind      */
    int                    n_args;  /* instantiate handler: 1 or 2 string args*/
};

static int cc__ci_collect_instantiate(const CCComptimeIntrinsicDesc* d,
                                      const char* src, size_t len,
                                      size_t pos, size_t splice_end) {
    size_t p = pos + strlen(d->name);
    CCEmitComptimeInst inst;
    (void)splice_end;
    if (cc__comptime_inst_count >= CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS) return 0;
    memset(&inst, 0, sizeof(inst));
    inst.kind = d->kind;
    p = cc_skip_ws_len(src, len, p);
    if (p >= len || src[p] != '(') return 0;
    p = cc_skip_ws_len(src, len, p + 1);
    if (!cc__emit_parse_c_string(src, len, &p, inst.a, sizeof(inst.a))) return 0;
    if (d->n_args == 2) {
        p = cc_skip_ws_len(src, len, p);
        if (p >= len || src[p] != ',') return 0;
        p = cc_skip_ws_len(src, len, p + 1);
        if (!cc__emit_parse_c_string(src, len, &p, inst.b, sizeof(inst.b))) return 0;
    }
    cc__comptime_insts[cc__comptime_inst_count++] = inst;
    return 1;
}

static int cc__ci_collect_emit(const CCComptimeIntrinsicDesc* d,
                               const char* src, size_t len,
                               size_t pos, size_t splice_end) {
    (void)d;
    return cc__emit_try_collect_cc_emit_cstr(src, len, pos, splice_end);
}

static int cc__ci_collect_emit_format(const CCComptimeIntrinsicDesc* d,
                                      const char* src, size_t len,
                                      size_t pos, size_t splice_end) {
    (void)d;
    return cc__emit_try_collect_cc_emit_format(src, len, pos, splice_end);
}

static int cc__ci_collect_generic_template(const CCComptimeIntrinsicDesc* d,
                                           const char* src, size_t len,
                                           size_t pos, size_t splice_end) {
    (void)d; (void)splice_end;
    return cc__emit_try_collect_cc_generic_template(src, len, pos);
}

static int cc__ci_collect_generic_register(const CCComptimeIntrinsicDesc* d,
                                           const char* src, size_t len,
                                           size_t pos, size_t splice_end) {
    (void)d; (void)splice_end;
    return cc__emit_try_collect_cc_generic_register(src, len, pos);
}

static const CCComptimeIntrinsicDesc cc__comptime_intrinsics[] = {
    { "cc_instantiate_vec",  CC_CI_INSTANTIATE, cc__ci_collect_instantiate,  CC_GRAPH_REQUEST_VEC,  1 },
    { "cc_instantiate_map",  CC_CI_INSTANTIATE, cc__ci_collect_instantiate,  CC_GRAPH_REQUEST_MAP,  2 },
    { "cc_instantiate_chan", CC_CI_INSTANTIATE, cc__ci_collect_instantiate,  CC_GRAPH_REQUEST_CHAN, 1 },
    /* cc_emit_format before cc_emit_cstr is irrelevant (whole-ident match), but
     * keep the more specific name listed so the table reads clearly. */
    { "cc_emit_format",      CC_CI_EMIT,        cc__ci_collect_emit_format,  (CCTypeGraphRequestKind)0, 0 },
    { "cc_emit_cstr",        CC_CI_EMIT,        cc__ci_collect_emit,         (CCTypeGraphRequestKind)0, 0 },
    /* generic-factory registration: collected in the EMIT pass; records a
     * template (side effect), emits no fragment of its own. */
    { "cc_generic_template", CC_CI_EMIT,        cc__ci_collect_generic_template, (CCTypeGraphRequestKind)0, 0 },
    { "cc_generic_register", CC_CI_EMIT,        cc__ci_collect_generic_register, (CCTypeGraphRequestKind)0, 0 },
};

static const unsigned cc__ci_mask_instantiate = CC_CI_INSTANTIATE;
static const unsigned cc__ci_mask_emit        = CC_CI_EMIT;

/* Single enumerator over top-level `@comptime { ... }` blocks.  The body of
 * each block is handed to a visitor; the table-driven `cc__emit_visit_dispatch`
 * is the only visitor needed — it routes recognized intrinsics from
 * `cc__comptime_intrinsics`.  Block bounds come from the shared
 * `cc_match_comptime_block` recognizer (util/text.h), the same one symbols.c
 * uses.  See COMPTIME_INSTANTIATION_SEAM.md §1b. */
typedef void (*CCEmitComptimeBlockVisitor)(const char* src, size_t len,
                                           size_t body_l, size_t body_r, void* ctx);

static void cc__emit_for_each_comptime_block(const char* src, size_t len,
                                             CCEmitComptimeBlockVisitor visit, void* ctx) {
    size_t i = 0;
    if (!src || len == 0 || !visit) return;
    while (i < len) {
        size_t body_l = 0, body_r = 0;
        if (!cc_match_comptime_block(src, len, i, &body_l, &body_r)) {
            i++;
            continue;
        }
        visit(src, len, body_l, body_r, ctx);
        i = body_r + 1;
    }
}

/* Per-block dispatcher: scan the body once and route each recognized intrinsic
 * call (whose group is enabled in *ctx mask) to its registry handler. */
static void cc__emit_visit_dispatch(const char* src, size_t len,
                                    size_t body_l, size_t body_r, void* ctx) {
    unsigned mask = ctx ? *(const unsigned*)ctx : 0u;
    const size_t n_intr = sizeof(cc__comptime_intrinsics) / sizeof(cc__comptime_intrinsics[0]);
    for (size_t j = body_l + 1; j < body_r; j++) {
        for (size_t k = 0; k < n_intr; k++) {
            const CCComptimeIntrinsicDesc* d = &cc__comptime_intrinsics[k];
            if (!(d->group & mask)) continue;
            if (!cc__emit_match_kw(src, len, j, d->name)) continue;
            d->collect(d, src, len, j, body_r + 1);
            j += strlen(d->name) - 1; /* loop's ++ steps past the last char */
            break;
        }
    }
}

void cc_emit_plan_collect_comptime_instantiations(const char* src, size_t len) {
    cc__emit_for_each_comptime_block(src, len, cc__emit_visit_dispatch,
                                     (void*)&cc__ci_mask_instantiate);
}

void cc_emit_plan_apply_comptime_instantiations(CCTypeGraph* graph) {
    /* graph may be NULL: cc_type_graph_request_* falls back to the global
     * registry (see cc_type_graph_active_registry), which is what the
     * final-compile path in visit_codegen.c reads. */
    for (size_t i = 0; i < cc__comptime_inst_count; i++) {
        const CCEmitComptimeInst* inst = &cc__comptime_insts[i];
        char mangled[256];
        switch (inst->kind) {
        case CC_GRAPH_REQUEST_VEC:
            if (!inst->a[0]) break;
            snprintf(mangled, sizeof(mangled), "CCVec_%s", inst->a);
            cc_type_graph_request_vec(graph, inst->a, mangled);
            break;
        case CC_GRAPH_REQUEST_MAP:
            if (!inst->a[0] || !inst->b[0]) break;
            snprintf(mangled, sizeof(mangled), "Map_%s_%s", inst->a, inst->b);
            cc_type_graph_request_map(graph, inst->a, inst->b, mangled);
            break;
        case CC_GRAPH_REQUEST_CHAN:
            if (!inst->a[0]) break;
            snprintf(mangled, sizeof(mangled), "CCChan_%s", inst->a);
            cc_type_graph_request_channel(graph, inst->a, mangled);
            break;
        default:
            break;
        }
    }
}

void cc_emit_plan_clear_comptime_fragments(void) {
    for (size_t i = 0; i < cc__comptime_frag_count; i++) {
        free(cc__comptime_frags[i].text);
        cc__comptime_frags[i].text = NULL;
    }
    cc__comptime_frag_count = 0;
    cc__generic_templates_clear();
    cc__exec_ranges_clear();
}

size_t cc_emit_plan_comptime_fragment_count(void) {
    return cc__comptime_frag_count;
}

/* --- comptime executor host API (Stage 0) --- */

static size_t cc__host_site_pos = 0;

typedef struct { size_t body_l; size_t body_r; } CCExecRange;
static CCExecRange cc__exec_ranges[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS];
static size_t cc__exec_range_count = 0;

static void cc__exec_ranges_clear(void) {
    cc__exec_range_count = 0;
}

static int cc__exec_range_contains(size_t body_l, size_t body_r) {
    for (size_t i = 0; i < cc__exec_range_count; i++)
        if (cc__exec_ranges[i].body_l == body_l && cc__exec_ranges[i].body_r == body_r)
            return 1;
    return 0;
}

static void cc__exec_range_mark(size_t body_l, size_t body_r) {
    if (cc__exec_range_count >= CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS) return;
    cc__exec_ranges[cc__exec_range_count].body_l = body_l;
    cc__exec_ranges[cc__exec_range_count].body_r = body_r;
    cc__exec_range_count++;
}

void cc_emit_plan_host_ctx_begin(size_t site_pos) {
    cc__host_site_pos = site_pos;
}

void cc_emit_plan_host_ctx_end(void) {
    cc__host_site_pos = 0;
}

void cc_emit_plan_host_emit_raw(int anchor, const char* ptr, size_t len) {
    if (!ptr || len == 0) return;
    /* Coalesce consecutive emits at the same anchor/site so multi-call loops
     * (e.g. CRC table generation) splice as one block, not N inserts at the
     * same prelude offset. */
    if (cc__comptime_frag_count > 0) {
        CCEmitComptimeFragment* last = &cc__comptime_frags[cc__comptime_frag_count - 1];
        if (last->text && last->anchor == (CCEmitAnchor)anchor &&
            last->site_pos == cc__host_site_pos) {
            size_t old_len = strlen(last->text);
            char* nv = (char*)realloc(last->text, old_len + len + 1);
            if (nv) {
                memcpy(nv + old_len, ptr, len);
                nv[old_len + len] = '\0';
                last->text = nv;
                return;
            }
        }
    }
    if (cc__comptime_frag_count >= CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS) return;
    char* dup = (char*)malloc(len + 1);
    if (!dup) return;
    memcpy(dup, ptr, len);
    dup[len] = '\0';
    CCEmitComptimeFragment* f = &cc__comptime_frags[cc__comptime_frag_count++];
    f->anchor = (CCEmitAnchor)anchor;
    f->site_pos = cc__host_site_pos;
    f->text = dup;
}

static void cc__host_add_inst(CCTypeGraphRequestKind kind, const char* a, const char* b) {
    if (cc__comptime_inst_count >= CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS) return;
    CCEmitComptimeInst* inst = &cc__comptime_insts[cc__comptime_inst_count++];
    memset(inst, 0, sizeof(*inst));
    inst->kind = kind;
    if (a) snprintf(inst->a, sizeof(inst->a), "%s", a);
    if (b) snprintf(inst->b, sizeof(inst->b), "%s", b);
}

void cc_emit_plan_host_instantiate_vec(const char* elem) {
    if (elem) cc__host_add_inst(CC_GRAPH_REQUEST_VEC, elem, NULL);
}

void cc_emit_plan_host_instantiate_map(const char* key, const char* val) {
    if (key && val) cc__host_add_inst(CC_GRAPH_REQUEST_MAP, key, val);
}

void cc_emit_plan_host_instantiate_result(const char* ok, const char* err) {
    (void)ok; (void)err;
    /* Result monomorph collection deferred; stub for ABI completeness. */
}

void cc_emit_plan_host_instantiate_chan(const char* elem) {
    if (elem) cc__host_add_inst(CC_GRAPH_REQUEST_CHAN, elem, NULL);
}

const void* cc_emit_plan_host_type_of(const char* name) {
    (void)name;
    return NULL;
}

/* --- comptime reflection host API (D6.3) --- */

static const char* cc__reflect_src = NULL;
static size_t cc__reflect_src_len = 0;

void cc_emit_plan_set_reflect_source(const char* src, size_t len) {
    cc__reflect_src = src;
    cc__reflect_src_len = len;
}

/* Copy `s` into out (NUL-terminated, truncated to out_sz). Returns bytes written. */
static int cc__rfl_emit(const char* s, char* out, int out_sz) {
    int wlen = (int)strlen(s);
    if (!out || out_sz <= 0) return wlen;
    int cap = wlen < out_sz - 1 ? wlen : out_sz - 1;
    memcpy(out, s, (size_t)cap);
    out[cap] = '\0';
    return cap;
}

int cc_reflect_field_count(const char* type_name) {
    CCCtField* fields = NULL;
    size_t nf = 0;
    if (!cc_ct_reflect_struct_fields(cc__reflect_src, cc__reflect_src_len,
                                     type_name, &fields, &nf))
        return -1;
    cc_ct_free_fields(fields, nf);
    return (int)nf;
}

static int cc__reflect_field_member(const char* type_name, int idx, int want_type,
                                    char* buf, int buf_sz) {
    if (buf && buf_sz > 0) buf[0] = '\0';
    CCCtField* fields = NULL;
    size_t nf = 0;
    if (!cc_ct_reflect_struct_fields(cc__reflect_src, cc__reflect_src_len,
                                     type_name, &fields, &nf))
        return -1;
    int rc = -1;
    if (idx >= 0 && (size_t)idx < nf)
        rc = cc__rfl_emit(want_type ? fields[idx].type : fields[idx].name, buf, buf_sz);
    cc_ct_free_fields(fields, nf);
    return rc;
}

int cc_reflect_field_name(const char* type_name, int idx, char* buf, int buf_sz) {
    return cc__reflect_field_member(type_name, idx, 0, buf, buf_sz);
}

int cc_reflect_field_type(const char* type_name, int idx, char* buf, int buf_sz) {
    return cc__reflect_field_member(type_name, idx, 1, buf, buf_sz);
}

static int cc__block_has_comptime_fn_call(const char* src, size_t body_l, size_t body_r) {
    for (size_t j = body_l + 1; j < body_r; j++) {
        if (!cc_is_ident_start(src[j])) continue;
        if (j > body_l + 1 && cc_is_ident_char(src[j - 1])) continue;
        {
            size_t id_e = j;
            char name[CC_COMPTIME_FN_NAME_MAX];
            size_t nlen;
            while (id_e < body_r && cc_is_ident_char(src[id_e])) id_e++;
            if (id_e >= body_r || src[id_e] != '(') continue;
            nlen = id_e - j;
            if (nlen >= sizeof(name)) continue;
            memcpy(name, src + j, nlen);
            name[nlen] = '\0';
            if (cc_comptime_fn_is_registered(name)) return 1;
        }
    }
    return 0;
}

static int cc__span_contains(const char* hay, size_t hay_len, const char* needle) {
    size_t nlen = needle ? strlen(needle) : 0;
    if (!hay || nlen == 0 || hay_len < nlen) return 0;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) return 1;
    }
    return 0;
}

static int cc__block_needs_executor(const char* src, size_t body_l, size_t body_r) {
    if (cc__block_has_comptime_fn_call(src, body_l, body_r)) return 1;
    if (body_r > body_l + 1 &&
        cc__span_contains(src + body_l + 1, body_r - body_l - 1, "cc_emit_tpl_"))
        return 1;
    for (size_t j = body_l + 1; j + 2 < body_r; j++) {
        if (cc_match_ident_kw(src, body_r, j, "for")) return 1;
        if (cc_match_ident_kw(src, body_r, j, "while")) return 1;
        if (cc_match_ident_kw(src, body_r, j, "do")) return 1;
    }
    return 0;
}

static int cc__exec_failed = 0;

static void cc__exec_visit_block(const char* src, size_t len,
                                 size_t body_l, size_t body_r, void* ctx) {
    const char* input_path = (const char*)ctx;
    if (!cc__block_needs_executor(src, body_l, body_r)) return;
    if (cc__exec_range_contains(body_l, body_r)) return;

    CCComptimeExecOpts opts = {0};
    opts.input_path = input_path;
    opts.site_pos = body_l;
    char err[512];
    if (cc_comptime_exec_block_body(src + body_l + 1, body_r - body_l - 1,
                                    &opts, err, sizeof(err)) != 0) {
        fprintf(stderr, "%s: error: @comptime block execution failed: %s\n",
                input_path ? input_path : "<input>", err[0] ? err : "unknown");
        cc__exec_failed = 1;
    }
    cc__exec_range_mark(body_l, body_r);
}

int cc_emit_plan_exec_comptime_blocks(const char* src, size_t len, const char* input_path) {
    cc__exec_failed = 0;
    cc_emit_plan_set_reflect_source(src, len);
    if (cc_comptime_fn_registry_scan(src, len) < 0) {
        const char* err = cc_comptime_fn_registry_scan_error();
        fprintf(stderr, "%s: error: %s\n",
                input_path ? input_path : "<input>",
                err ? err : "@comptime function registry scan failed");
        cc__exec_failed = 1;
    }
    cc__emit_for_each_comptime_block(src, len, cc__exec_visit_block, (void*)input_path);
    return cc__exec_failed ? -1 : 0;
}

static void cc__emit_visit_dispatch_skip_exec(const char* src, size_t len,
                                              size_t body_l, size_t body_r, void* ctx) {
    if (cc__exec_range_contains(body_l, body_r)) return;
    cc__emit_visit_dispatch(src, len, body_l, body_r, ctx);
}

void cc_emit_plan_collect_comptime_emits(const char* src, size_t len) {
    cc__emit_for_each_comptime_block(src, len, cc__emit_visit_dispatch_skip_exec,
                                     (void*)&cc__ci_mask_emit);
}

static size_t cc__emit_resolve_anchor_pos(CCEmitAnchor anchor, size_t site_pos,
                                          size_t insert_pos, size_t container_pos) {
    switch (anchor) {
    case CC_EMIT_AT_COMPTIME_SITE:
        return site_pos;
    case CC_EMIT_BEFORE_FIRST_USE:
        return insert_pos;
    case CC_EMIT_AFTER_PRELUDE:
    default:
        return container_pos > 0 && container_pos < insert_pos ? container_pos : insert_pos;
    }
}

void cc_emit_plan_build_comptime_schedule(const char* src, size_t len,
                                          size_t insert_pos, size_t container_pos,
                                          CCEmitPlanComptimeSchedule* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    (void)src;
    (void)len;
    for (size_t i = 0; i < cc__comptime_frag_count &&
                       out->n < CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS; i++) {
        out->pos[out->n] = cc__emit_resolve_anchor_pos(cc__comptime_frags[i].anchor,
                                                         cc__comptime_frags[i].site_pos,
                                                         insert_pos, container_pos);
        out->frag_index[out->n] = i;
        out->n++;
    }
}

void cc_emit_plan_fprint_comptime_fragment(FILE* out, size_t frag_index) {
    if (!out || frag_index >= cc__comptime_frag_count) return;
    const CCEmitComptimeFragment* f = &cc__comptime_frags[frag_index];
    if (!f->text || !f->text[0]) return;
    fprintf(out, "/* --- comptime cc_emit_cstr --- */\n%s", f->text);
    if (f->text[strlen(f->text) - 1] != '\n') fputc('\n', out);
}

static int cc__emit_splice_at(char** src, size_t* len, size_t pos, const char* insert,
                              const char* input_path) {
    size_t ins_len;
    char* nb;
    size_t old_len;
    if (!src || !len || !insert) return -1;
    old_len = *len;
    if (pos > old_len) pos = old_len;
    ins_len = strlen(insert);
    nb = (char*)malloc(old_len + ins_len + 256);
    if (!nb) return -1;
    memcpy(nb, *src, pos);
    memcpy(nb + pos, insert, ins_len);
    size_t tail = old_len - pos;
    memcpy(nb + pos + ins_len, *src + pos, tail);
    nb[pos + ins_len + tail] = '\0';
    free(*src);
    *src = nb;
    *len = pos + ins_len + tail;
    (void)input_path;
    return 0;
}

int cc_emit_plan_splice_comptime_fragments(char** src, size_t* len, const char* input_path) {
    size_t insert_pos;
    size_t container_pos;
    CCEmitPlanComptimeSchedule sched;
    size_t order[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS];
    if (!src || !*src || !len) return 0;
    if (cc__comptime_frag_count == 0) return 0;
    insert_pos = cc_emit_plan_compute_prelude_insert_pos(*src, *len);
    container_pos = cc_emit_plan_compute_container_anchor(*src, *len);
    cc_emit_plan_build_comptime_schedule(*src, *len, insert_pos, container_pos, &sched);
    for (size_t i = 0; i < sched.n; i++) order[i] = i;
    for (size_t i = 0; i + 1 < sched.n; i++) {
        for (size_t j = i + 1; j < sched.n; j++) {
            if (sched.pos[order[j]] > sched.pos[order[i]]) {
                size_t t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }
    for (size_t oi = 0; oi < sched.n; oi++) {
        size_t si = order[oi];
        size_t frag_i = sched.frag_index[si];
        const CCEmitComptimeFragment* f = &cc__comptime_frags[frag_i];
        char block[CC_EMIT_SPLICE_BLOCK_MAX];
        int n = snprintf(block, sizeof(block),
                         "\n/* --- comptime cc_emit_cstr --- */\n%s%s",
                         f->text ? f->text : "",
                         (f->text && f->text[0] && f->text[strlen(f->text) - 1] == '\n') ? "" : "\n");
        if (n <= 0 || (size_t)n >= sizeof(block)) {
            fprintf(stderr,
                    "%s: error: comptime emit splice block exceeds %d bytes\n",
                    input_path ? input_path : "<input>", CC_EMIT_SPLICE_BLOCK_MAX);
            return -1;
        }
        if (cc__emit_splice_at(src, len, sched.pos[si], block, input_path) != 0) return -1;
    }
    return 0;
}

size_t cc_emit_plan_line_start_before(const char* src, size_t pos) {
    if (!src) return 0;
    while (pos > 0 && src[pos - 1] != '\n') pos--;
    return pos;
}

size_t cc_emit_plan_find_ident_top_level(const char* src, size_t start, size_t len,
                                         const char* ident) {
    size_t ident_len = ident ? strlen(ident) : 0;
    size_t pos = start;
    if (!src || !ident || ident_len == 0 || start >= len) return len;
    while (pos + ident_len <= len) {
        pos = cc_find_substr_top_level(src, pos, len, ident, ident_len);
        if (pos >= len) return len;
        int left_ok = (pos == 0) || !cc_is_ident_char(src[pos - 1]);
        int right_ok = (pos + ident_len >= len) || !cc_is_ident_char(src[pos + ident_len]);
        if (left_ok && right_ok) return pos;
        pos++;
    }
    return len;
}

size_t cc_emit_plan_type_decl_end_top_level(const char* src, size_t len,
                                            const char* type_name) {
    size_t p = 0;
    if (!src || !type_name || !type_name[0]) return 0;
    if (strcmp(type_name, "void") == 0 ||
        strcmp(type_name, "bool") == 0 ||
        strcmp(type_name, "char") == 0 ||
        strcmp(type_name, "short") == 0 ||
        strcmp(type_name, "int") == 0 ||
        strcmp(type_name, "long") == 0 ||
        strcmp(type_name, "float") == 0 ||
        strcmp(type_name, "double") == 0 ||
        strcmp(type_name, "size_t") == 0 ||
        strcmp(type_name, "ssize_t") == 0 ||
        strcmp(type_name, "CCError") == 0) {
        return 0;
    }
    while (p < len) {
        size_t line_start = p;
        size_t line_end = line_start;
        while (line_end < len && src[line_end] != '\n') line_end++;
        size_t s = line_start;
        while (s < line_end && (src[s] == ' ' || src[s] == '\t' || src[s] == '\r')) s++;
        int is_type_decl =
            (s + 7 <= line_end && memcmp(src + s, "typedef", 7) == 0 && !cc_is_ident_char(src[s + 7])) ||
            (s + 6 <= line_end && memcmp(src + s, "struct", 6) == 0 && !cc_is_ident_char(src[s + 6])) ||
            (s + 5 <= line_end && memcmp(src + s, "union", 5) == 0 && !cc_is_ident_char(src[s + 5])) ||
            (s + 4 <= line_end && memcmp(src + s, "enum", 4) == 0 && !cc_is_ident_char(src[s + 4]));
        if (!is_type_decl) {
            p = (line_end < len) ? line_end + 1 : line_end;
            continue;
        }

        size_t q = s;
        int brace_depth = 0;
        int in_str = 0, in_chr = 0, in_lc = 0, in_bc = 0;
        for (; q < len; q++) {
            char c = src[q];
            char c2 = (q + 1 < len) ? src[q + 1] : 0;
            if (in_lc) { if (c == '\n') in_lc = 0; continue; }
            if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; q++; } continue; }
            if (in_str) { if (c == '\\' && c2) { q++; continue; } if (c == '"') in_str = 0; continue; }
            if (in_chr) { if (c == '\\' && c2) { q++; continue; } if (c == '\'') in_chr = 0; continue; }
            if (c == '/' && c2 == '/') { in_lc = 1; q++; continue; }
            if (c == '/' && c2 == '*') { in_bc = 1; q++; continue; }
            if (c == '"') { in_str = 1; continue; }
            if (c == '\'') { in_chr = 1; continue; }
            if (c == '{') { brace_depth++; continue; }
            if (c == '}') { if (brace_depth > 0) brace_depth--; continue; }
            if (c == ';' && brace_depth == 0) {
                size_t end = q + 1;
                int declares_type = 0;
                if (s + 7 <= line_end && memcmp(src + s, "typedef", 7) == 0 && !cc_is_ident_char(src[s + 7])) {
                    size_t e = q;
                    while (e > s && (src[e - 1] == ' ' || src[e - 1] == '\t' || src[e - 1] == '\r' || src[e - 1] == '\n')) e--;
                    size_t b = e;
                    while (b > s && cc_is_ident_char(src[b - 1])) b--;
                    size_t type_len = strlen(type_name);
                    declares_type = (e > b && e - b == type_len && memcmp(src + b, type_name, type_len) == 0);
                } else {
                    size_t kw_len =
                        (s + 6 <= line_end && memcmp(src + s, "struct", 6) == 0 && !cc_is_ident_char(src[s + 6])) ? 6 :
                        (s + 5 <= line_end && memcmp(src + s, "union", 5) == 0 && !cc_is_ident_char(src[s + 5])) ? 5 :
                        (s + 4 <= line_end && memcmp(src + s, "enum", 4) == 0 && !cc_is_ident_char(src[s + 4])) ? 4 : 0;
                    size_t b = s + kw_len;
                    while (b < end && (src[b] == ' ' || src[b] == '\t' || src[b] == '\r' || src[b] == '\n')) b++;
                    size_t e = b;
                    while (e < end && cc_is_ident_char(src[e])) e++;
                    size_t type_len = strlen(type_name);
                    declares_type = (e > b && e - b == type_len && memcmp(src + b, type_name, type_len) == 0);
                }
                if (declares_type) {
                    if (end < len && src[end] == '\n') end++;
                    return end;
                }
                p = (end < len && src[end] == '\n') ? end + 1 : end;
                break;
            }
        }
        if (q >= len) return 0;
    }
    return 0;
}

static int cc__emit_plan_block_references_container(const char* src, size_t block_start,
                                                    size_t block_end, int is_typedef_block) {
    int refs_container = 0;
    int typedef_uses_only_predeclared_vec_char = 0;
    if (!src || block_end <= block_start) return 0;
    for (size_t si = block_start; si + 7 < block_end && !refs_container; si++) {
        if (memcmp(src + si, "__CC_MAP", 8) == 0 ||
            memcmp(src + si, "__CC_VEC", 8) == 0) {
            refs_container = 1;
        } else if ((si + 4 < block_end && memcmp(src + si, "Map_", 4) == 0) ||
                   (si + 6 < block_end && memcmp(src + si, "CCVec_", 6) == 0)) {
            refs_container = 1;
        }
    }
    if (is_typedef_block && refs_container) {
        for (size_t si = block_start; si + 14 <= block_end; si++) {
            if (memcmp(src + si, "__CC_VEC(char)", 14) == 0) {
                typedef_uses_only_predeclared_vec_char = 1;
                break;
            }
        }
    }
    if (!refs_container) return 0;
    if (!is_typedef_block) return 1;
    return !typedef_uses_only_predeclared_vec_char;
}

size_t cc_emit_plan_compute_prelude_insert_pos(const char* src, size_t len) {
    size_t insert_pos = 0;
    if (!src || len == 0) return 0;
    while (insert_pos < len) {
        size_t line_start = insert_pos;
        size_t line_end = line_start;
        while (line_end < len && src[line_end] != '\n') line_end++;
        size_t p = line_start;
        while (p < line_end && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r')) p++;
        if (p + 1 < len && p + 1 < line_end && src[p] == '/' && src[p + 1] == '*') {
            size_t end = p + 2;
            while (end + 1 < len && !(src[end] == '*' && src[end + 1] == '/')) end++;
            insert_pos = (end + 1 < len) ? end + 2 : len;
            if (insert_pos < len && src[insert_pos] == '\n') insert_pos++;
            continue;
        }
        if (p + 9 <= line_end && memcmp(src + p, "#include ", 9) == 0) {
            insert_pos = (line_end < len) ? line_end + 1 : line_end;
            continue;
        }
        if (p < line_end && src[p] == '#') {
            insert_pos = (line_end < len) ? line_end + 1 : line_end;
            continue;
        }
        if (p == line_end || (p + 1 < line_end && src[p] == '/' && src[p + 1] == '/')) {
            insert_pos = (line_end < len) ? line_end + 1 : line_end;
            continue;
        }
        if ((p + 7 <= line_end && memcmp(src + p, "typedef", 7) == 0 && !cc_is_ident_char(src[p + 7])) ||
            (p + 6 <= line_end && memcmp(src + p, "struct", 6) == 0 && !cc_is_ident_char(src[p + 6])) ||
            (p + 5 <= line_end && memcmp(src + p, "union", 5) == 0 && !cc_is_ident_char(src[p + 5])) ||
            (p + 4 <= line_end && memcmp(src + p, "enum", 4) == 0 && !cc_is_ident_char(src[p + 4]))) {
            size_t q = p;
            int brace_depth = 0;
            int in_str = 0, in_chr = 0, in_lc = 0, in_bc = 0;
            for (; q < len; q++) {
                char c = src[q];
                char c2 = (q + 1 < len) ? src[q + 1] : 0;
                if (in_lc) { if (c == '\n') in_lc = 0; continue; }
                if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; q++; } continue; }
                if (in_str) { if (c == '\\' && c2) { q++; continue; } if (c == '"') in_str = 0; continue; }
                if (in_chr) { if (c == '\\' && c2) { q++; continue; } if (c == '\'') in_chr = 0; continue; }
                if (c == '/' && c2 == '/') { in_lc = 1; q++; continue; }
                if (c == '/' && c2 == '*') { in_bc = 1; q++; continue; }
                if (c == '"') { in_str = 1; continue; }
                if (c == '\'') { in_chr = 1; continue; }
                if (c == '{') { brace_depth++; continue; }
                if (c == '}') { if (brace_depth > 0) brace_depth--; continue; }
                if (c == ';' && brace_depth == 0) {
                    q++;
                    if (q < len && src[q] == '\n') q++;
                    insert_pos = q;
                    break;
                }
            }
            if (q >= len) insert_pos = len;
            continue;
        }
        break;
    }
    return insert_pos;
}

size_t cc_emit_plan_compute_container_anchor(const char* src, size_t len) {
    size_t pos = 0;
    if (!src || len == 0) return 0;
    while (pos < len) {
        size_t line_start = pos;
        size_t line_end = line_start;
        while (line_end < len && src[line_end] != '\n') line_end++;
        size_t p = line_start;
        while (p < line_end && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r')) p++;
        if (p == line_end) {
            pos = (line_end < len) ? line_end + 1 : line_end;
            continue;
        }
        if (p + 1 < len && src[p] == '/' && src[p + 1] == '*') {
            size_t end = p + 2;
            while (end + 1 < len && !(src[end] == '*' && src[end + 1] == '/')) end++;
            pos = (end + 1 < len) ? end + 2 : len;
            if (pos < len && src[pos] == '\n') pos++;
            continue;
        }
        if (p + 1 < line_end && src[p] == '/' && src[p + 1] == '/') {
            pos = (line_end < len) ? line_end + 1 : line_end;
            continue;
        }
        if (src[p] == '#') {
            pos = (line_end < len) ? line_end + 1 : line_end;
            continue;
        }
        if ((p + 7 <= line_end && memcmp(src + p, "typedef", 7) == 0 && !cc_is_ident_char(src[p + 7])) ||
            (p + 6 <= line_end && memcmp(src + p, "struct", 6) == 0 && !cc_is_ident_char(src[p + 6])) ||
            (p + 5 <= line_end && memcmp(src + p, "union", 5) == 0 && !cc_is_ident_char(src[p + 5])) ||
            (p + 4 <= line_end && memcmp(src + p, "enum", 4) == 0 && !cc_is_ident_char(src[p + 4]))) {
            int is_typedef_block =
                (p + 7 <= line_end && memcmp(src + p, "typedef", 7) == 0 && !cc_is_ident_char(src[p + 7]));
            size_t block_start = p;
            size_t q = p;
            int brace_depth = 0;
            size_t block_end = len;
            while (q < len) {
                char c = src[q];
                if (c == '{') brace_depth++;
                else if (c == '}') { if (brace_depth > 0) brace_depth--; }
                else if (c == ';' && brace_depth == 0) {
                    q++;
                    if (q < len && src[q] == '\n') q++;
                    block_end = q;
                    break;
                }
                q++;
            }
            if (q >= len) block_end = len;
            if (cc__emit_plan_block_references_container(src, block_start, block_end, is_typedef_block)) {
                return line_start;
            }
            pos = block_end;
            continue;
        }
        break;
    }
    return pos;
}

size_t cc_emit_plan_compute_before_first_use(const char* src, size_t len, size_t anchor_pos,
                                             const char* payload_type, const char* mangled_name) {
    size_t decl_end = cc_emit_plan_type_decl_end_top_level(src, len, payload_type);
    if (decl_end <= anchor_pos) return anchor_pos;
    if (mangled_name && mangled_name[0]) {
        size_t first_use = cc_emit_plan_find_ident_top_level(src, decl_end, len, mangled_name);
        if (first_use < len) {
            return cc_emit_plan_line_start_before(src, first_use);
        }
    }
    return decl_end;
}

void cc_emit_plan_build_container_schedule(const char* src, size_t len, CCTypeGraph* graph,
                                           CCEmitPlanContainerSchedule* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
        out->vec_pos[i] = len + 1;
        out->map_pos[i] = len + 1;
    }
    if (!graph) return;
    out->anchor_pos = cc_emit_plan_compute_container_anchor(src, len);
    out->n_vec = cc_type_graph_vec_count(graph);
    out->n_map = cc_type_graph_map_count(graph);
    for (size_t i = 0; i < out->n_vec && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
        const CCTypeInstantiation* inst = cc_type_graph_get_vec(graph, i);
        if (!inst || !inst->type1 || !inst->mangled_name) continue;
        const char* mangled_elem = inst->mangled_name + 6;
        if (strcmp(mangled_elem, "char") == 0) continue;
        size_t pos = cc_emit_plan_compute_before_first_use(src, len, out->anchor_pos,
                                                           inst->type1, inst->mangled_name);
        if (pos > out->anchor_pos) {
            out->vec_delayed[i] = 1;
            out->vec_pos[i] = pos;
        }
    }
    for (size_t i = 0; i < out->n_map && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
        const CCTypeInstantiation* inst = cc_type_graph_get_map(graph, i);
        if (!inst || !inst->type1 || !inst->type2 || !inst->mangled_name) continue;
        size_t k_end = cc_emit_plan_type_decl_end_top_level(src, len, inst->type1);
        size_t v_end = cc_emit_plan_type_decl_end_top_level(src, len, inst->type2);
        size_t decl_end = k_end > v_end ? k_end : v_end;
        if (decl_end <= out->anchor_pos) continue;
        size_t pos = cc_emit_plan_compute_before_first_use(src, len, out->anchor_pos,
                                                           inst->type1, inst->mangled_name);
        size_t pos2 = cc_emit_plan_compute_before_first_use(src, len, out->anchor_pos,
                                                            inst->type2, inst->mangled_name);
        if (pos2 > pos) pos = pos2;
        out->map_delayed[i] = 1;
        out->map_pos[i] = pos;
    }
}

void cc_emit_plan_build_result_delays(const char* src, size_t len,
                                      const CCResultSpecTable* specs,
                                      size_t prelude_insert_pos,
                                      CCEmitPlanResultDelay* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
        out->pos[i] = len + 1;
    }
    if (!specs) return;
    for (size_t i = 0; i < specs->count && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
        const CCResultSpec* spec = cc_result_spec_table_get(specs, i);
        size_t ok_decl_end = spec ? cc_emit_plan_type_decl_end_top_level(src, len, spec->ok_type) : 0;
        size_t err_decl_end = spec ? cc_emit_plan_type_decl_end_top_level(src, len, spec->err_type) : 0;
        size_t decl_end = ok_decl_end > err_decl_end ? ok_decl_end : err_decl_end;
        if (!spec || decl_end <= prelude_insert_pos) continue;

        char concrete[256];
        cc_result_spec_format_name(spec->mangled_ok, spec->mangled_err,
                                   concrete, sizeof(concrete));
        size_t first_use = cc_emit_plan_find_ident_top_level(src, decl_end, len, concrete);
        out->delayed[i] = 1;
        if (first_use < len) {
            out->pos[i] = cc_emit_plan_line_start_before(src, first_use);
        } else {
            out->pos[i] = decl_end;
        }
    }
}

void cc_emit_plan_fprint_container_prelude(FILE* out, int use_cch,
                                           int need_vec, int need_map, int need_chan) {
    if (!out) return;
    fprintf(out, "/* --- CC generic container declarations --- */\n");
    fprintf(out, "#ifdef CC_PARSER_MODE\n");
    fprintf(out, "#undef CC_PARSER_MODE\n");
    fprintf(out, "#define __CC_RESTORE_PARSER_MODE_AFTER_CONTAINERS 1\n");
    fprintf(out, "#endif\n");
    if (use_cch) {
        /* TCC parse path: map_forward (from prelude) already froze the parser
         * stub CC_MAP_DECL_ARENA — do not include map_impl (cc_containers). */
        if (need_vec) fprintf(out, "#include <ccc/std/vec.cch>\n");
        if (need_chan) fprintf(out, "#include <ccc/cc_channel.cch>\n");
    } else {
        if (need_vec) fprintf(out, "#include <ccc/std/vec.h>\n");
        if (need_map) fprintf(out, "#include <ccc/std/map.h>\n");
        if (need_chan) fprintf(out, "#include <ccc/cc_channel.h>\n");
    }
}

void cc_emit_plan_fprint_container_epilogue(FILE* out) {
    if (!out) return;
    fprintf(out, "/* --- end container declarations (post-prelude) --- */\n");
    fprintf(out, "#ifdef __CC_RESTORE_PARSER_MODE_AFTER_CONTAINERS\n");
    fprintf(out, "#undef __CC_RESTORE_PARSER_MODE_AFTER_CONTAINERS\n");
    fprintf(out, "#ifndef CC_PARSER_MODE\n");
    fprintf(out, "#define CC_PARSER_MODE 1\n");
    fprintf(out, "#endif\n");
    fprintf(out, "#endif\n\n");
}

void cc_emit_plan_fprint_vec_decl(FILE* out, const CCTypeInstantiation* inst) {
    if (!out || !inst || !inst->type1 || !inst->mangled_name) return;
    const char* mangled_elem = inst->mangled_name + 6;
    if (strcmp(mangled_elem, "char") == 0) return;
    fprintf(out, "CC_VEC_DECL_ARENA(%s, %s)\n", inst->type1, inst->mangled_name);
}

void cc_emit_plan_fprint_map_decl(FILE* out, const CCTypeInstantiation* inst) {
    if (!out || !inst || !inst->type1 || !inst->type2 || !inst->mangled_name) return;
    const char* hash_fn = "cc_map_hash_i32";
    const char* eq_fn = "cc_map_eq_i32";
    if (strcmp(inst->type1, "int") == 0) {
        hash_fn = "cc_map_hash_i32"; eq_fn = "cc_map_eq_i32";
    } else if (strcmp(inst->type1, "CCSliceHdr") == 0) {
        hash_fn = "cc_map_hash_slice_hdr"; eq_fn = "cc_map_eq_slice_hdr";
    } else if (strstr(inst->type1, "64") != NULL) {
        hash_fn = "cc_map_hash_u64"; eq_fn = "cc_map_eq_u64";
    } else if (strstr(inst->type1, "slice") != NULL || strstr(inst->type1, "Slice") != NULL ||
               strcmp(inst->type1, "charslice") == 0) {
        hash_fn = "cc_map_hash_slice"; eq_fn = "cc_map_eq_slice";
    }
    fprintf(out, "CC_MAP_DECL_ARENA(%s, %s, %s, %s, %s)\n",
            inst->type1, inst->type2, inst->mangled_name, hash_fn, eq_fn);
}

int cc_emit_plan_format_result_arm(char* out, size_t out_sz,
                                   const char* concrete,
                                   CCResultArmKind kind,
                                   int ok_is_void, int with_diag) {
    if (!out || out_sz == 0 || !concrete) return -1;
    switch (kind) {
    case CC_RESULT_ARM_IS_ERR:
        return snprintf(out, out_sz,
            "    %s: (!((%s*)(void*)&(__x__))->ok), \\\n",
            concrete, concrete);
    case CC_RESULT_ARM_VALUE:
        if (ok_is_void) {
            return snprintf(out, out_sz, "    %s: ((void)0), \\\n", concrete);
        }
        return snprintf(out, out_sz,
            "    %s: ((%s*)(void*)&(__x__))->u.value, \\\n",
            concrete, concrete);
    case CC_RESULT_ARM_ERR:
        if (with_diag) {
            return snprintf(out, out_sz,
                "    %s: (cc_rt_diag_record_unwrap_site(__f__, __l__), "
                "((%s*)(void*)&(__x__))->u.error), \\\n",
                concrete, concrete);
        }
        return snprintf(out, out_sz,
            "    %s: ((%s*)(void*)&(__x__))->u.error, \\\n",
            concrete, concrete);
    default:
        return -1;
    }
}

void cc_emit_plan_fprint_line_directive(FILE* out, const char* src, size_t offset,
                                        const char* input_path) {
    char rel[1024];
    int resume_line = 1;
    if (!out || !src) return;
    for (size_t i = 0; i < offset; i++) {
        if (src[i] == '\n') resume_line++;
    }
    fprintf(out, "#line %d \"%s\"\n", resume_line,
            cc_path_rel_to_repo(input_path ? input_path : "<string>", rel, sizeof(rel)));
}
