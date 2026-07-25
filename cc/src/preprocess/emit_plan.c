/*
 * CCEmitPlan — unified splice anchors (track A2).
 */
#include "emit_plan.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>

#include <ccc/cc_arena.cch>

#include "comptime/executor.h"
#include "comptime/hook_compile.h"
#include "preprocess/emit_limits.h"
#include "preprocess/preprocess.h"
#include "preprocess/template_scan.h"
#include "util/path.h"
#include "util/text.h"

static int cc__diag_line_for_pos(size_t pos);

typedef struct CCEmitComptimeFragment {
    CCEmitAnchor anchor;
    size_t site_pos;
    int site_line;
    char* text;
    /* Emit provenance (edge-push #5): explicit origin for this fragment, set by
     * cc_emit_raw_at so a downstream C-compiler error in generated code maps to
     * the template/emit source.  NULL origin_file => derive from site_pos. */
    char* origin_file;
    int   origin_line;
} CCEmitComptimeFragment;

static CCEmitComptimeFragment cc__comptime_frags[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS];
static size_t cc__comptime_frag_count = 0;

static void cc__exec_ranges_clear(void);

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
        f->site_line = 1;
        for (size_t k = 0; k < site_pos && k < len; k++)
            if (src[k] == '\n') f->site_line++;
        f->origin_file = NULL;
        f->origin_line = 0;
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
        f->site_line = 1;
        for (size_t k = 0; k < site_pos && k < len; k++)
            if (src[k] == '\n') f->site_line++;
        f->origin_file = NULL;
        f->origin_line = 0;
        f->text = strdup(frag);
        if (!f->text) { cc__comptime_frag_count--; return 0; }
    }
    return 1;
}

/* --- unified generic registry (Option A) ---
 *
 * One registry holds every generic, built-in or user-defined, tagged by how it
 * produces C:
 *   - NATIVE_DECL : a compiler-native C fn that emits a container monomorph's
 *                   declaration from a CCTypeInstantiation (built-in Vec/Map;
 *                   consumed by the type-graph emission loop).
 *   - TEMPLATE    : a declarative `$0..$N` template expanded at the use site.
 *   - COMPILED    : a `@comptime` factory compiled to a dylib and invoked at
 *                   the use site.
 * The three historic registries (container-decl factories, generic templates,
 * compiled factories) are now one array keyed by (name, kind); the built-in
 * containers are simply NATIVE_DECL entries, so "Vec/Map are special" stops
 * being true at the dispatch level — they share the registry and the use-site
 * resolver with user `Name::[args]` factories. */
typedef enum CCGenericKind {
    CC_GENERIC_NATIVE_DECL = 0,
    CC_GENERIC_COMPILED    = 1,
} CCGenericKind;

#define CC_GENERIC_MAX_EXT 32
typedef struct CCGenericReg {
    char*         name;          /* owns: "Vec", "Map", "Pair", ...           */
    CCGenericKind kind;
    /* NATIVE_DECL */
    CCContainerDeclFactory decl_fn;
    /* COMPILED: base factory (defines the type) — last-wins, may be NULL while
       only extensions have registered so far (base requirement is enforced at
       the use site, so registration order across files is irrelevant). */
    char*         handler_name;  /* owns */
    const void*   fn_ptr;
    void*         owner;
    /* COMPILED: extension factories (CC_GENERIC_FACTORY_EXTEND), run after the
       base in registration order; each may emit an empty fragment to opt out. */
    char*         ext_handlers[CC_GENERIC_MAX_EXT];  /* owns */
    const void*   ext_fns[CC_GENERIC_MAX_EXT];
    void*         ext_owners[CC_GENERIC_MAX_EXT];
    size_t        ext_count;
} CCGenericReg;

#define CC_EMIT_PLAN_MAX_GENERICS 128
static CCGenericReg cc__generics[CC_EMIT_PLAN_MAX_GENERICS];
static size_t cc__generic_count = 0;
/* Set once the built-in NATIVE_DECL defaults (Vec/Map) have been seeded — or
 * suppressed by an explicit early container registration (preserves the prior
 * container registry's "first external register opts out of defaults" quirk). */
static int cc__generic_defaults_done = 0;

static CCGenericReg* cc__generic_find(const char* name, CCGenericKind kind) {
    if (!name) return NULL;
    for (size_t i = 0; i < cc__generic_count; i++)
        if (cc__generics[i].kind == kind && strcmp(cc__generics[i].name, name) == 0)
            return &cc__generics[i];
    return NULL;
}

static CCGenericReg* cc__generic_new(const char* name, CCGenericKind kind) {
    CCGenericReg* r;
    if (cc__generic_count >= CC_EMIT_PLAN_MAX_GENERICS) return NULL;
    r = &cc__generics[cc__generic_count];
    memset(r, 0, sizeof(*r));
    r->name = strdup(name);
    if (!r->name) return NULL;
    r->kind = kind;
    cc__generic_count++;
    return r;
}

static void cc__generic_free_entry(CCGenericReg* r) {
    free(r->name);
    free(r->handler_name);
    if (r->owner) cc_comptime_type_hook_owner_free(r->owner);
    for (size_t i = 0; i < r->ext_count; i++) {
        free(r->ext_handlers[i]);
        if (r->ext_owners[i]) cc_comptime_type_hook_owner_free(r->ext_owners[i]);
    }
    memset(r, 0, sizeof(*r));
}

/* Drop every entry of `kind`, compacting the array (NATIVE_DECL built-ins are
 * never removed, so per-TU clears of TEMPLATE/COMPILED leave them intact). */
static void cc__generic_remove_kind(CCGenericKind kind) {
    size_t w = 0;
    for (size_t i = 0; i < cc__generic_count; i++) {
        if (cc__generics[i].kind == kind) {
            cc__generic_free_entry(&cc__generics[i]);
        } else {
            if (w != i) cc__generics[w] = cc__generics[i];
            w++;
        }
    }
    cc__generic_count = w;
}

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
        f->site_line = 1;
        f->origin_file = NULL;
        f->origin_line = 0;
        f->text = d;
        cc__generic_emitted[cc__generic_emitted_count++] = m;
    }
    return 1;
}

/* Per-TU reset of the generic emit/invalid dedup tracking. */
static void cc__generic_dedup_clear(void) {
    for (size_t i = 0; i < cc__generic_emitted_count; i++) free(cc__generic_emitted[i]);
    cc__generic_emitted_count = 0;
    for (size_t i = 0; i < cc__generic_invalid_count; i++) free(cc__generic_invalid[i]);
    cc__generic_invalid_count = 0;
}

void cc_emit_plan_clear_generic_factory_registrations(void) {
    cc__generic_remove_kind(CC_GENERIC_COMPILED);
}

/* --- user generic factories (D6.1: compiled handlers) --- */

void cc_emit_plan_register_generic_factory(const char* name, const char* handler_name) {
    CCGenericReg* r;
    if (!name || !handler_name) return;
    r = cc__generic_find(name, CC_GENERIC_COMPILED);
    if (r) {
        /* An entry may already exist with handler_name == NULL when extensions
           registered before the base (cross-file order); fill in the base. */
        char* nh = strdup(handler_name);
        if (!nh) return;
        if (!r->handler_name || strcmp(r->handler_name, handler_name) != 0)
            r->fn_ptr = NULL;
        free(r->handler_name);
        r->handler_name = nh;
        return;
    }
    r = cc__generic_new(name, CC_GENERIC_COMPILED);
    if (!r) return;
    r->handler_name = strdup(handler_name);
    if (!r->handler_name) { cc__generic_free_entry(r); cc__generic_count--; }
}

/* Append an extension factory for `name`, creating the registry entry if the
 * base hasn't registered yet (the base requirement is checked at the use site,
 * so include/registration order across files does not matter). */
void cc_emit_plan_register_generic_factory_extend(const char* name, const char* handler_name) {
    CCGenericReg* r;
    char* h;
    if (!name || !handler_name) return;
    r = cc__generic_find(name, CC_GENERIC_COMPILED);
    if (!r) {
        r = cc__generic_new(name, CC_GENERIC_COMPILED);
        if (!r) return;
    }
    if (r->ext_count >= CC_GENERIC_MAX_EXT) {
        fprintf(stderr, "error: generic '%s' has too many CC_GENERIC_FACTORY_EXTEND "
                        "factories (max %d)\n", name, CC_GENERIC_MAX_EXT);
        return;
    }
    h = strdup(handler_name);
    if (!h) return;
    r->ext_handlers[r->ext_count++] = h;
}

/* True if `name` has a COMPILED registry entry (base and/or extensions). The
 * use-site gate uses this so an extend-only name is still recognized and gets
 * the explicit "extended but never defined" diagnostic from `ensure`. */
int cc_emit_plan_has_generic_factory(const char* name) {
    return cc__generic_find(name, CC_GENERIC_COMPILED) != NULL;
}

const void* cc_emit_plan_lookup_generic_factory(const char* name) {
    CCGenericReg* r = cc__generic_find(name, CC_GENERIC_COMPILED);
    return r ? r->fn_ptr : NULL;
}

const char* cc_emit_plan_lookup_generic_factory_handler(const char* name) {
    CCGenericReg* r = cc__generic_find(name, CC_GENERIC_COMPILED);
    return r ? r->handler_name : NULL;
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

/* Run one factory fn into its own scratch arena and append the returned
 * fragment to def_out at *io_total (newline-separated).  `require_nonempty`
 * distinguishes the base (must define the type) from extensions (an empty
 * return opts out — conditional specialization).  Returns 0 on hard failure
 * (required-but-empty, or buffer overflow); 1 otherwise. */
static int cc__invoke_one_factory(const void* fn, const char* name, const char* mangled,
                                  CCFactorySliceArray args, char* def_out, size_t def_cap,
                                  size_t* io_total, int require_nonempty, const char* who) {
    CCGenericFactoryFn call = (CCGenericFactoryFn)(uintptr_t)fn;
    CCFactorySlice result;
    if (!fn) return 0;
    /* The factory builds its @emit output into this scratch arena (stack-first,
       heap-spill); the returned slice points into it and is copied into def_out
       before we free it.  The factory's `arena` param is load-bearing. */
    {
        CC_ARENA_STACK(factory_arena, CC_EMIT_TPL_BUF_SIZE);
        result = call(cc__factory_slice_cstr(name),
                      cc__factory_slice_cstr(mangled),
                      args,
                      &factory_arena);
        if (!result.ptr || result.len == 0) {
            cc_arena_free(&factory_arena);
            return require_nonempty ? 0 : 1;
        }
        {
            size_t sep = (*io_total > 0) ? 1 : 0;
            if (*io_total + sep + result.len >= def_cap) {
                fprintf(stderr,
                        "error: compiled generic factory '%s' output exceeds %zu byte limit\n",
                        who, def_cap);
                cc_arena_free(&factory_arena);
                return 0;
            }
            if (sep) def_out[(*io_total)++] = '\n';
            memcpy(def_out + *io_total, result.ptr, result.len);
            *io_total += result.len;
            def_out[*io_total] = '\0';
        }
        cc_arena_free(&factory_arena);
    }
    return 1;
}

int cc_emit_plan_invoke_generic_factory(const char* name, const char* mangled,
                                        const char type_args[8][128], int nargs,
                                        char* def_out, size_t def_cap) {
    CCGenericReg* r = cc__generic_find(name, CC_GENERIC_COMPILED);
    CCFactorySliceArray args = {0};
    CCFactorySlice arg_slices[8];
    size_t total = 0;
    if (!r || !mangled || !def_out || def_cap == 0 || nargs <= 0 || nargs > 8) return 0;
    if (!r->handler_name || !r->fn_ptr) return 0;  /* base required and compiled */
    for (int i = 0; i < nargs; i++)
        arg_slices[i] = cc__factory_slice_cstr(type_args[i]);
    args.items = arg_slices;
    args.len = (size_t)nargs;
    def_out[0] = '\0';
    /* Base first (defines the type / `${mangled}`), then extensions in
       registration order — so extensions can reference the base's symbols. */
    if (!cc__invoke_one_factory(r->fn_ptr, name, mangled, args,
                                def_out, def_cap, &total, 1, name))
        return 0;
    for (size_t e = 0; e < r->ext_count; e++) {
        if (!cc__invoke_one_factory(r->ext_fns[e], name, mangled, args,
                                    def_out, def_cap, &total, 0, r->ext_handlers[e]))
            return 0;
    }
    return 1;
}

CCGenProduceStatus cc_emit_plan_produce_generic_def(
    const char* gname, const char* mangled, const char orig_args[8][128], int nargs,
    const char* reflect_src, size_t reflect_len, const char* input_path,
    char* def_out, size_t def_cap, char* err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    cc_emit_plan_set_reflect_source(reflect_src, reflect_len);
    if (cc_emit_plan_ensure_generic_factory(gname, input_path, err, err_cap) != 0)
        return CC_GEN_PRODUCE_ENSURE_FAILED;
    if (!cc_emit_plan_invoke_generic_factory(gname, mangled, orig_args, nargs,
                                             def_out, def_cap))
        return CC_GEN_PRODUCE_INVOKE_FAILED;
    return CC_GEN_PRODUCE_OK;
}

/* Compile a single factory handler (base or extension) to an in-process fn ptr.
 * Idempotent: returns immediately when *fn_out is already set. */
static int cc__compile_one_factory_handler(const char* handler_name, const char* input_path,
                                           const void** fn_out, void** owner_out,
                                           char* err_buf, size_t err_sz) {
    static char entry_name[256];
    CCComptimeHookSpec spec = {0};
    const void* fn_ptr = NULL;
    void* owner = NULL;
    const char* def;
    if (*fn_out) return 0;
    def = cc_comptime_fn_registry_lookup_def(handler_name);
    if (!def || !def[0]) {
        if (err_buf && err_sz)
            snprintf(err_buf, err_sz, "factory handler '%s' not found in registry",
                     handler_name);
        return -1;
    }
    snprintf(entry_name, sizeof(entry_name), "__cc_gen_factory_%s", handler_name);
    spec.kind = CC_COMPTIME_TYPE_HOOK_GENERIC_FACTORY;
    spec.entry_name = entry_name;
    spec.handler_name = handler_name;
    if (cc_comptime_compile_type_hooks_tu_ex(input_path, def, &spec, 1, &owner, &fn_ptr,
                                             err_buf, err_sz) != 0)
        return -1;
    *fn_out = fn_ptr;
    *owner_out = owner;
    return 0;
}

/* Compile a generic's base factory (when present) plus every extension.  The
 * base-required check lives in `cc_emit_plan_ensure_generic_factory` (use site),
 * so this is a no-op-friendly compile pass that the eager builder can call on
 * extend-only entries without erroring. */
static int cc__compile_generic_factory_reg(CCGenericReg* f, const char* input_path,
                                           char* err_buf, size_t err_sz) {
    if (!f) return -1;
    if (f->handler_name &&
        cc__compile_one_factory_handler(f->handler_name, input_path,
                                        &f->fn_ptr, &f->owner, err_buf, err_sz) != 0)
        return -1;
    for (size_t i = 0; i < f->ext_count; i++)
        if (cc__compile_one_factory_handler(f->ext_handlers[i], input_path,
                                            &f->ext_fns[i], &f->ext_owners[i],
                                            err_buf, err_sz) != 0)
            return -1;
    return 0;
}

int cc_emit_plan_ensure_generic_factory(const char* generic_name, const char* input_path,
                                        char* err_buf, size_t err_sz) {
    CCGenericReg* r;
    if (!generic_name) return -1;
    r = cc__generic_find(generic_name, CC_GENERIC_COMPILED);
    if (!r) {
        if (err_buf && err_sz)
            snprintf(err_buf, err_sz, "generic '%s' is not registered", generic_name);
        return -1;
    }
    if (!r->handler_name) {
        /* Only CC_GENERIC_FACTORY_EXTEND seen for this name — nothing defines
           the type itself. */
        if (err_buf && err_sz)
            snprintf(err_buf, err_sz,
                     "generic '%s' is extended but never defined "
                     "(needs CC_GENERIC_FACTORY(%s) or cc_generic_register)",
                     generic_name, generic_name);
        return -1;
    }
    return cc__compile_generic_factory_reg(r, input_path, err_buf, err_sz);
}

int cc_emit_plan_compile_generic_factories(const char* src, size_t len,
                                           const char* input_path) {
    (void)src;
    (void)len;
    for (size_t i = 0; i < cc__generic_count; i++) {
        if (cc__generics[i].kind != CC_GENERIC_COMPILED) continue;
        if (cc__compile_generic_factory_reg(&cc__generics[i], input_path, NULL, 0) != 0)
            return -1;
    }
    return 0;
}

/* cc_generic_register("Name", handler_fn) — register a compiled factory.
 * This is the single generic-factory registration path; the
 * `CC_GENERIC_FACTORY(Name) { ... }` sugar lowers to it (see preprocess.c). */
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

/* cc_generic_register_extend("Name", handler_fn) — append an extension factory.
 * The `CC_GENERIC_FACTORY_EXTEND(Name) { ... }` sugar lowers to it. */
static int cc__emit_try_collect_cc_generic_register_extend(const char* src, size_t len, size_t call_pos) {
    size_t p = call_pos + strlen("cc_generic_register_extend");
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
    cc_emit_plan_register_generic_factory_extend(name, handler);
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

static int cc__ci_collect_generic_register(const CCComptimeIntrinsicDesc* d,
                                           const char* src, size_t len,
                                           size_t pos, size_t splice_end) {
    (void)d; (void)splice_end;
    return cc__emit_try_collect_cc_generic_register(src, len, pos);
}

static int cc__ci_collect_generic_register_extend(const CCComptimeIntrinsicDesc* d,
                                                  const char* src, size_t len,
                                                  size_t pos, size_t splice_end) {
    (void)d; (void)splice_end;
    return cc__emit_try_collect_cc_generic_register_extend(src, len, pos);
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
     * factory binding (side effect), emits no fragment of its own. */
    { "cc_generic_register", CC_CI_EMIT,        cc__ci_collect_generic_register, (CCTypeGraphRequestKind)0, 0 },
    { "cc_generic_register_extend", CC_CI_EMIT, cc__ci_collect_generic_register_extend, (CCTypeGraphRequestKind)0, 0 },
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
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    if (!src || len == 0 || !visit) return;
    /* Skip comments and string/char literals: `@comptime` appearing in prose
     * (e.g. "a @comptime block") or inside a string is not a real block. */
    while (i < len) {
        char c = src[i];
        char c2 = (i + 1 < len) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\') { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\') { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        if (c == '@') {
            size_t body_l = 0, body_r = 0;
            if (cc_match_comptime_block(src, len, i, &body_l, &body_r)) {
                visit(src, len, body_l, body_r, ctx);
                i = body_r + 1;
                continue;
            }
        }
        i++;
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
        free(cc__comptime_frags[i].origin_file);
        cc__comptime_frags[i].origin_file = NULL;
        cc__comptime_frags[i].origin_line = 0;
    }
    cc__comptime_frag_count = 0;
    cc__generic_dedup_clear();
    cc__exec_ranges_clear();
}

size_t cc_emit_plan_comptime_fragment_count(void) {
    return cc__comptime_frag_count;
}

/* --- comptime executor host API (Stage 0) --- */

static size_t cc__host_site_pos = 0;
/* Input path for comptime diagnostics (cc_emit_error/warning); set when the
 * exec pass begins so host verbs can attribute file:line. */
static const char* cc__diag_input_path = NULL;

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

static void cc__host_emit_raw_impl(int anchor, const char* ptr, size_t len,
                                   const char* origin_file, int origin_line) {
    if (!ptr || len == 0) return;
    /* Coalesce consecutive emits at the same anchor/site so multi-call loops
     * (e.g. CRC table generation) splice as one block, not N inserts at the
     * same prelude offset.  Only default-origin fragments coalesce; an explicit
     * provenance (cc_emit_raw_at) keeps each fragment's #line distinct. */
    if (cc__comptime_frag_count > 0) {
        CCEmitComptimeFragment* last = &cc__comptime_frags[cc__comptime_frag_count - 1];
        if (last->text && last->anchor == (CCEmitAnchor)anchor &&
            last->site_pos == cc__host_site_pos &&
            last->origin_file == NULL && origin_file == NULL) {
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
    f->site_line = cc__diag_line_for_pos(cc__host_site_pos);
    f->text = dup;
    f->origin_line = origin_line;
    f->origin_file = (origin_file && origin_file[0]) ? strdup(origin_file) : NULL;
}

void cc_emit_plan_host_emit_raw(int anchor, const char* ptr, size_t len) {
    cc__host_emit_raw_impl(anchor, ptr, len, NULL, 0);
}

/* Emit provenance (edge-push #5): emit with an explicit origin so a downstream
 * C-compiler diagnostic in the generated fragment maps back to the template /
 * emit source the library author actually wrote, not the splice location. */
void cc_emit_plan_host_emit_raw_at(int anchor, const char* file, int line,
                                   const char* ptr, size_t len) {
    cc__host_emit_raw_impl(anchor, ptr, len, file, line);
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

/* --- enum reflection host verbs (edge-push #1) ---
 * Bytes-only across the user-space waist: enumerator names copy out as bytes,
 * values cross as a scalar.  Backed by an on-demand scan of the current source
 * buffer, exactly like the struct-field reflection above; shared by the libtcc
 * executor and compiled factory dylibs. */
int cc_reflect_enum_count(const char* enum_name) {
    CCCtEnumMember* m = NULL;
    size_t nm = 0;
    if (!cc_ct_reflect_enum_members(cc__reflect_src, cc__reflect_src_len,
                                    enum_name, &m, &nm))
        return -1;
    cc_ct_free_enum_members(m, nm);
    return (int)nm;
}

int cc_reflect_enum_name(const char* enum_name, int idx, char* buf, int buf_sz) {
    if (buf && buf_sz > 0) buf[0] = '\0';
    CCCtEnumMember* m = NULL;
    size_t nm = 0;
    if (!cc_ct_reflect_enum_members(cc__reflect_src, cc__reflect_src_len,
                                    enum_name, &m, &nm))
        return -1;
    int rc = -1;
    if (idx >= 0 && (size_t)idx < nm)
        rc = cc__rfl_emit(m[idx].name, buf, buf_sz);
    cc_ct_free_enum_members(m, nm);
    return rc;
}

int cc_reflect_enum_value(const char* enum_name, int idx, long long* out) {
    if (out) *out = 0;
    CCCtEnumMember* m = NULL;
    size_t nm = 0;
    if (!cc_ct_reflect_enum_members(cc__reflect_src, cc__reflect_src_len,
                                    enum_name, &m, &nm))
        return -1;
    int rc = -1;
    if (idx >= 0 && (size_t)idx < nm) {
        if (out) *out = m[idx].value;
        rc = 0;
    }
    cc_ct_free_enum_members(m, nm);
    return rc;
}

/* Type-kind classifier host verb (edge-push #2): returns a CC_REFLECT_KIND_*
 * code for a type spelling, scanning the current source buffer. */
int cc_reflect_kind(const char* type_name) {
    return cc_ct_reflect_type_kind(cc__reflect_src, cc__reflect_src_len, type_name);
}

/* Canonical generic mangling host verb (naming/composition): the exact mangled
 * name the compiler uses for base::[args...], so library generics compose. */
int cc_canonical_name(const char* base, const char** args, int nargs,
                      char* out, int out_sz) {
    if (out_sz <= 0) return -1;
    return cc_ct_canonical_name(base, (const char* const*)args, nargs,
                                out, (size_t)out_sz);
}

/* Tag-filtered reflection host verbs (edge-push #3): count and name the
 * functions carrying a `@tag:NAME` marker, so a @comptime block can build a
 * registry/dispatch table.  Re-scans per call (mirrors the enum verbs). */
int cc_reflect_tagged_count(const char* tag) {
    char** names = NULL;
    size_t n = 0;
    if (!cc_ct_reflect_tagged_fns(cc__reflect_src, cc__reflect_src_len, tag, &names, &n))
        return -1;
    cc_ct_free_tagged_fns(names, n);
    return (int)n;
}

int cc_reflect_tagged_name(const char* tag, int idx, char* buf, int buf_sz) {
    if (!buf || buf_sz <= 0) return -1;
    buf[0] = 0;
    char** names = NULL;
    size_t n = 0;
    if (!cc_ct_reflect_tagged_fns(cc__reflect_src, cc__reflect_src_len, tag, &names, &n))
        return -1;
    int rc = -1;
    if (idx >= 0 && (size_t)idx < n) {
        snprintf(buf, (size_t)buf_sz, "%s", names[idx]);
        rc = 0;
    }
    cc_ct_free_tagged_fns(names, n);
    return rc;
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
    /* Custom diagnostics and raw/at-origin emits are runtime side effects, not
     * statically collectible emit text, so such a block must run through the
     * executor.  ("cc_emit_raw" also covers "cc_emit_raw_at".) */
    if (body_r > body_l + 1 &&
        (cc__span_contains(src + body_l + 1, body_r - body_l - 1, "cc_emit_error") ||
         cc__span_contains(src + body_l + 1, body_r - body_l - 1, "cc_emit_warning") ||
         cc__span_contains(src + body_l + 1, body_r - body_l - 1, "cc_emit_raw") ||
         cc__span_contains(src + body_l + 1, body_r - body_l - 1, "cc_canonical_name")))
        return 1;
    for (size_t j = body_l + 1; j + 2 < body_r; j++) {
        if (cc_match_ident_kw(src, body_r, j, "for")) return 1;
        if (cc_match_ident_kw(src, body_r, j, "while")) return 1;
        if (cc_match_ident_kw(src, body_r, j, "do")) return 1;
    }
    return 0;
}

static int cc__exec_failed = 0;

/* --- custom domain diagnostics (edge-push #4) ---
 * cc_emit_error/cc_emit_warning are the dual of cc_emit_raw: instead of
 * emitting C, a @comptime block (or compiled factory dylib) raises a compiler
 * diagnostic for a constraint it checks itself.  Attributed to the source line
 * of the enclosing @comptime block via cc__host_site_pos (block-level for now;
 * finer call-site attribution is the emit-provenance milestone).  An error
 * marks the exec pass failed so the build stops, exactly like a built-in
 * diagnostic — collisions and violations are loud, never silent.  Real global
 * symbols so both the libtcc executor and dynamic_lookup dylibs resolve them. */
static int cc__diag_line_for_pos(size_t pos) {
    int line = 1;
    if (!cc__reflect_src) return line;
    size_t lim = pos < cc__reflect_src_len ? pos : cc__reflect_src_len;
    for (size_t i = 0; i < lim; i++)
        if (cc__reflect_src[i] == '\n') line++;
    return line;
}

void cc_emit_error(const char* msg) {
    fprintf(stderr, "%s:%d: error: %s\n",
            cc__diag_input_path ? cc__diag_input_path : "<input>",
            cc__diag_line_for_pos(cc__host_site_pos),
            msg && msg[0] ? msg : "comptime error");
    cc__exec_failed = 1;
}

void cc_emit_warning(const char* msg) {
    fprintf(stderr, "%s:%d: warning: %s\n",
            cc__diag_input_path ? cc__diag_input_path : "<input>",
            cc__diag_line_for_pos(cc__host_site_pos),
            msg && msg[0] ? msg : "comptime warning");
}

/* Explicit-origin diagnostics (edge-push #5): the author supplies the file:line
 * (e.g. a reflected member's location or a template-literal origin) so the
 * diagnostic points exactly where the constraint is, not just at the block. */
void cc_emit_error_at(const char* file, int line, const char* msg) {
    fprintf(stderr, "%s:%d: error: %s\n",
            file && file[0] ? file : (cc__diag_input_path ? cc__diag_input_path : "<input>"),
            line > 0 ? line : cc__diag_line_for_pos(cc__host_site_pos),
            msg && msg[0] ? msg : "comptime error");
    cc__exec_failed = 1;
}

void cc_emit_warning_at(const char* file, int line, const char* msg) {
    fprintf(stderr, "%s:%d: warning: %s\n",
            file && file[0] ? file : (cc__diag_input_path ? cc__diag_input_path : "<input>"),
            line > 0 ? line : cc__diag_line_for_pos(cc__host_site_pos),
            msg && msg[0] ? msg : "comptime warning");
}

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
    cc__diag_input_path = input_path;
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

static const char* cc__emit_basename(const char* path) {
    const char* slash;
    if (!path) return "";
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int cc__emit_same_file(const char* a, const char* b) {
    return a && b && (strcmp(a, b) == 0 ||
           strcmp(cc__emit_basename(a), cc__emit_basename(b)) == 0);
}

static size_t cc__emit_find_logical_line(const char* src, size_t len,
                                         const char* input_path, int target_line,
                                         size_t fallback) {
    size_t pos = 0;
    int logical = 1;
    char current[PATH_MAX];
    snprintf(current, sizeof(current), "%s", input_path ? input_path : "");
    while (pos < len) {
        size_t end = pos;
        while (end < len && src[end] != '\n') end++;
        if (cc__emit_same_file(current, input_path) && logical == target_line)
            return pos;
        {
            char line[PATH_MAX + 64], file[4096];
            int nline = 0;
            size_t ll = end - pos;
            if (ll >= sizeof(line)) ll = sizeof(line) - 1;
            memcpy(line, src + pos, ll);
            line[ll] = '\0';
            file[0] = '\0';
            if (sscanf(line, " #line %d \"%4095[^\"]\"", &nline, file) == 2 ||
                sscanf(line, "#line %d \"%4095[^\"]\"", &nline, file) == 2 ||
                sscanf(line, " #%d \"%4095[^\"]\"", &nline, file) == 2 ||
                sscanf(line, "#%d \"%4095[^\"]\"", &nline, file) == 2) {
                logical = nline;
                snprintf(current, sizeof(current), "%s", file);
            } else {
                logical++;
            }
        }
        pos = end < len ? end + 1 : end;
    }
    return fallback < len ? fallback : len;
}

static size_t cc__emit_resolve_anchor_pos(CCEmitAnchor anchor, size_t site_pos,
                                          int site_line, const char* src, size_t len,
                                          const char* input_path,
                                          size_t insert_pos, size_t container_pos) {
    switch (anchor) {
    case CC_EMIT_AT_COMPTIME_SITE: {
        char marker[64];
        snprintf(marker, sizeof(marker), "enum{__ccs%zu=0};", site_pos);
        const char* hit = src ? strstr(src, marker) : NULL;
        if (hit) {
            size_t pos = (size_t)(hit - src);
            while (pos > 0 && src[pos - 1] != '\n') pos--;
            return pos;
        }
        return cc__emit_find_logical_line(src, len, input_path, site_line, site_pos);
    }
    case CC_EMIT_BEFORE_FIRST_USE:
        return insert_pos;
    case CC_EMIT_AFTER_PRELUDE:
    default:
        return container_pos > 0 && container_pos < insert_pos ? container_pos : insert_pos;
    }
}

void cc_emit_plan_build_comptime_schedule(const char* src, size_t len,
                                          const char* input_path,
                                          size_t insert_pos, size_t container_pos,
                                          CCEmitPlanComptimeSchedule* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < cc__comptime_frag_count &&
                       out->n < CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS; i++) {
        out->pos[out->n] = cc__emit_resolve_anchor_pos(cc__comptime_frags[i].anchor,
                                                         cc__comptime_frags[i].site_pos,
                                                         cc__comptime_frags[i].site_line,
                                                         src, len, input_path,
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

/* Dup-emit-name detector (naming/composition): find top-level function
 * *definition* names (`NAME(...) {`) in a generated fragment.  Functions are
 * the external link symbols — C's one genuinely-silent collision footgun — so
 * we surface duplicates loudly with both origins instead of letting them slip
 * through.  Returns count; names truncated to 127 bytes. */
static int cc__scan_fn_def_names(const char* text, char names[][128], int cap) {
    if (!text) return 0;
    int cnt = 0;
    size_t n = strlen(text), i = 0;
    int brace_depth = 0;
    while (i < n && cnt < cap) {
        char c = text[i];
        if (c == '/' && i + 1 < n && text[i + 1] == '/') {
            while (i < n && text[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(text[i] == '*' && text[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = text[i++];
            while (i < n && text[i] != q) { if (text[i] == '\\') i++; i++; }
            if (i < n) i++;
            continue;
        }
        if (c == '{') { brace_depth++; i++; continue; }
        if (c == '}') { if (brace_depth > 0) brace_depth--; i++; continue; }
        if (brace_depth == 0 && cc_is_ident_start(c) &&
            (i == 0 || !cc_is_ident_char(text[i - 1]))) {
            size_t s = i;
            while (i < n && cc_is_ident_char(text[i])) i++;
            size_t e = i;
            size_t nl = e - s;
            /* Skip control keywords that can precede '(' (defensive; they live
             * at brace_depth>0 normally). */
            int is_kw = (nl == 2 && memcmp(text + s, "if", 2) == 0) ||
                        (nl == 3 && memcmp(text + s, "for", 3) == 0) ||
                        (nl == 5 && memcmp(text + s, "while", 5) == 0) ||
                        (nl == 6 && memcmp(text + s, "switch", 6) == 0) ||
                        (nl == 6 && memcmp(text + s, "return", 6) == 0) ||
                        (nl == 6 && memcmp(text + s, "sizeof", 6) == 0);
            size_t j = cc_skip_ws_and_comments(text, n, i);
            if (!is_kw && j < n && text[j] == '(') {
                int pd = 0; size_t k = j;
                for (; k < n; k++) {
                    if (text[k] == '(') pd++;
                    else if (text[k] == ')') { pd--; if (pd == 0) { k++; break; } }
                }
                size_t m = cc_skip_ws_and_comments(text, n, k);
                if (m < n && text[m] == '{' && nl > 0 && nl < 128) {
                    memcpy(names[cnt], text + s, nl);
                    names[cnt][nl] = 0;
                    cnt++;
                }
            }
            continue;
        }
        i++;
    }
    return cnt;
}

/* Dup-emit-name detector (naming/composition): warn (loud, never silent) when
 * two collected comptime fragments define the same top-level function symbol,
 * naming both origins.  Path-independent: scans all collected fragments, so it
 * works from both the preprocess fprint path and the codegen splice path.
 * `src`/`len` is the line-aligned body buffer used to derive @comptime origins. */
void cc_emit_plan_warn_duplicate_symbols(const char* src, size_t len,
                                         const char* input_path) {
    if (cc__comptime_frag_count == 0) return;
    char relbuf[1024];
    const char* rel_input = cc_path_rel_to_repo(input_path ? input_path : "<input>",
                                                relbuf, sizeof(relbuf));
    static char seen_name[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS][128];
    static char seen_orig[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS][288];
    size_t seen = 0;
    for (size_t fi = 0; fi < cc__comptime_frag_count; fi++) {
        const CCEmitComptimeFragment* f = &cc__comptime_frags[fi];
        if (!f->text) continue;
        const char* of = (f->origin_file && f->origin_file[0]) ? f->origin_file : rel_input;
        int oline;
        if (f->origin_file && f->origin_file[0]) {
            oline = f->origin_line > 0 ? f->origin_line : 1;
        } else {
            oline = 1;
            for (size_t k = 0; k < f->site_pos && src && k < len; k++)
                if (src[k] == '\n') oline++;
        }
        char origin[288];
        snprintf(origin, sizeof(origin), "%s:%d", of, oline);
        char names[16][128];
        int nc = cc__scan_fn_def_names(f->text, names, 16);
        for (int ni = 0; ni < nc; ni++) {
            size_t hit = (size_t)-1;
            for (size_t q = 0; q < seen; q++)
                if (strcmp(seen_name[q], names[ni]) == 0) { hit = q; break; }
            if (hit != (size_t)-1) {
                if (strcmp(seen_orig[hit], origin) != 0)
                    fprintf(stderr,
                        "%s: warning: duplicate comptime-emitted symbol '%s' "
                        "(emitted at %s and %s)\n",
                        input_path ? input_path : "<input>",
                        names[ni], seen_orig[hit], origin);
            } else if (seen < CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS) {
                snprintf(seen_name[seen], sizeof(seen_name[seen]), "%s", names[ni]);
                snprintf(seen_orig[seen], sizeof(seen_orig[seen]), "%s", origin);
                seen++;
            }
        }
    }
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
    cc_emit_plan_build_comptime_schedule(*src, *len, input_path,
                                          insert_pos, container_pos, &sched);
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
    /* Emit provenance (edge-push #5): wrap each spliced fragment with #line
     * directives so a downstream C-compiler error inside generated code maps to
     * the template/emit origin, and code after the splice resumes correct
     * attribution.  Compute all line numbers up front against the untouched
     * (line-aligned) body buffer, since the splice loop mutates *src. */
    char relbuf[1024];
    const char* rel_input = cc_path_rel_to_repo(input_path ? input_path : "<input>",
                                                relbuf, sizeof(relbuf));
    static int restore_lines[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS];
    static int origin_lines[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS];
    for (size_t si = 0; si < sched.n; si++) {
        const CCEmitComptimeFragment* f = &cc__comptime_frags[sched.frag_index[si]];
        int rl = 1;
        for (size_t k = 0; k < sched.pos[si] && k < *len; k++)
            if ((*src)[k] == '\n') rl++;
        restore_lines[si] = rl;
        if (f->origin_file && f->origin_file[0]) {
            origin_lines[si] = f->origin_line > 0 ? f->origin_line : 1;
        } else {
            int ol = 1;
            for (size_t k = 0; k < f->site_pos && k < *len; k++)
                if ((*src)[k] == '\n') ol++;
            origin_lines[si] = ol;
        }
    }
    cc_emit_plan_warn_duplicate_symbols(*src, *len, input_path);
    for (size_t oi = 0; oi < sched.n; oi++) {
        size_t si = order[oi];
        size_t frag_i = sched.frag_index[si];
        const CCEmitComptimeFragment* f = &cc__comptime_frags[frag_i];
        const char* origin_file = (f->origin_file && f->origin_file[0])
                                  ? f->origin_file : rel_input;
        char block[CC_EMIT_SPLICE_BLOCK_MAX];
        int n = snprintf(block, sizeof(block),
                         "\n#line %d \"%s\"\n%s%s#line %d \"%s\"\n",
                         origin_lines[si], origin_file,
                         f->text ? f->text : "",
                         (f->text && f->text[0] && f->text[strlen(f->text) - 1] == '\n') ? "" : "\n",
                         restore_lines[si], rel_input);
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
                    if (declares_type) {
                        size_t after_name = e;
                        while (after_name < end &&
                               (src[after_name] == ' ' || src[after_name] == '\t' ||
                                src[after_name] == '\r' || src[after_name] == '\n')) {
                            after_name++;
                        }
                        /* Autoblock argument captures are ordinary variables
                         * (`struct T* __cc_ab_*`), not definitions of T.
                         * Do not delay Result<T,E> declarations to them. */
                        if (after_name < end && src[after_name] == '*' &&
                            cc_find_substr_top_level(src, after_name, end,
                                                     "__cc_ab_", 8) < end) {
                            declares_type = 0;
                        }
                    }
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
        /* Check __CC_ARRAY_MAP before __CC_MAP: the names share no prefix, but
         * keeping the longer family first documents the intended order next to
         * Map_/ArrayMap_ below. */
        if ((si + 14 <= block_end && memcmp(src + si, "__CC_ARRAY_MAP", 14) == 0) ||
            memcmp(src + si, "__CC_MAP", 8) == 0 ||
            memcmp(src + si, "__CC_VEC", 8) == 0) {
            refs_container = 1;
        } else if ((si + 9 <= block_end && memcmp(src + si, "ArrayMap_", 9) == 0) ||
                   (si + 4 < block_end && memcmp(src + si, "Map_", 4) == 0 &&
                    (si == 0 || !cc_is_ident_char(src[si - 1]))) ||
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
        if (need_map) {
            fprintf(out, "#include <ccc/std/map.h>\n");
            fprintf(out, "#include <ccc/std/array_map.h>\n");
        }
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

/* --- D6.4 / Option A: container declaration factories ------------------- *
 *
 * Container monomorph declarations (Vec/Map/...) used to be emitted by two
 * hardcoded functions.  They are now NATIVE_DECL entries in the unified generic
 * registry (above): the built-in Vec/Map emitters below are simply the
 * default-seeded entries, so a library can register an additional container
 * kind (or override a built-in) through the same seam — and built-in containers
 * share one registry with user `Name::[args]` factories.
 * `cc_emit_plan_fprint_vec_decl` / `_map_decl` are thin dispatchers that look
 * the kind up and call the registered factory (the built-in unless overridden).
 */

static void cc__builtin_vec_decl(FILE* out, const CCTypeInstantiation* inst) {
    if (!out || !inst || !inst->type1 || !inst->mangled_name) return;
    const char* mangled_elem = inst->mangled_name + 6;
    if (strcmp(mangled_elem, "char") == 0) return;
    fprintf(out, "CC_VEC_DECL_ARENA(%s, %s)\n", inst->type1, inst->mangled_name);
}

/* Map key-type -> (hash, eq) selection, as data rather than control flow.
 * Priority order, first match wins; `substr` chooses substring vs exact match;
 * an unmatched key falls back to the i32 pair.  This is the built-in (closed)
 * set of primitive/slice key kinds — keeping it as a table makes the set
 * explicit and is the natural place a future registrable seam would prepend
 * library-supplied key hashers (see COMPTIME_INSTANTIATION_SEAM.md). */
typedef struct CCMapKeyHashEq {
    const char* pat;
    int         substr;   /* 1: strstr match; 0: exact strcmp */
    const char* hash_fn;
    const char* eq_fn;
} CCMapKeyHashEq;

static const CCMapKeyHashEq cc__map_key_hasheq[] = {
    { "int",          0, "cc_map_hash_i32",         "cc_map_eq_i32"         },
    { "CCSliceHdr",   0, "cc_map_hash_slice_hdr",   "cc_map_eq_slice_hdr"   },
    { "CCStringRef",  0, "cc_map_hash_string_ref",  "cc_map_eq_string_ref"  },
    { "64",           1, "cc_map_hash_u64",         "cc_map_eq_u64"         },
    { "slice",        1, "cc_map_hash_slice",       "cc_map_eq_slice"       },
    { "Slice",        1, "cc_map_hash_slice",       "cc_map_eq_slice"       },
};

static void cc__map_select_hasheq(const char* key_type,
                                  const char** out_hash, const char** out_eq) {
    *out_hash = "cc_map_hash_i32";
    *out_eq   = "cc_map_eq_i32";
    for (size_t i = 0; i < sizeof(cc__map_key_hasheq) / sizeof(cc__map_key_hasheq[0]); i++) {
        const CCMapKeyHashEq* e = &cc__map_key_hasheq[i];
        int hit = e->substr ? (strstr(key_type, e->pat) != NULL)
                            : (strcmp(key_type, e->pat) == 0);
        if (hit) { *out_hash = e->hash_fn; *out_eq = e->eq_fn; return; }
    }
}

static void cc__builtin_map_decl(FILE* out, const CCTypeInstantiation* inst) {
    const char* hash_fn;
    const char* eq_fn;
    if (!out || !inst || !inst->type1 || !inst->type2 || !inst->mangled_name) return;
    cc__map_select_hasheq(inst->type1, &hash_fn, &eq_fn);
    if (strncmp(inst->mangled_name, "ArrayMap_", 9) == 0) {
        fprintf(out, "CC_ARRAY_MAP_DECL(%s, %s, %s, %s, %s)\n",
                inst->type1, inst->type2, inst->mangled_name, hash_fn, eq_fn);
        return;
    }
    fprintf(out, "CC_MAP_DECL_ARENA(%s, %s, %s, %s, %s)\n",
            inst->type1, inst->type2, inst->mangled_name, hash_fn, eq_fn);
}

static void cc__container_factories_ensure_defaults(void) {
    if (cc__generic_defaults_done) return;
    cc__generic_defaults_done = 1;
    cc_emit_plan_register_container_factory("Vec", cc__builtin_vec_decl);
    cc_emit_plan_register_container_factory("Map", cc__builtin_map_decl);
}

void cc_emit_plan_register_container_factory(const char* kind, CCContainerDeclFactory fn) {
    CCGenericReg* r;
    if (!kind || !fn) return;
    /* First external registration suppresses the default seeding (preserves the
     * historic container registry's opt-out quirk) and avoids re-entering
     * ensure_defaults() while it registers the built-ins. */
    if (!cc__generic_defaults_done) cc__generic_defaults_done = 1;
    r = cc__generic_find(kind, CC_GENERIC_NATIVE_DECL);
    if (r) { r->decl_fn = fn; return; }  /* last registration wins */
    r = cc__generic_new(kind, CC_GENERIC_NATIVE_DECL);
    if (r) r->decl_fn = fn;
}

CCContainerDeclFactory cc_emit_plan_lookup_container_factory(const char* kind) {
    CCGenericReg* r;
    if (!kind) return NULL;
    cc__container_factories_ensure_defaults();
    r = cc__generic_find(kind, CC_GENERIC_NATIVE_DECL);
    return r ? r->decl_fn : NULL;
}

void cc_emit_plan_fprint_vec_decl(FILE* out, const CCTypeInstantiation* inst) {
    CCContainerDeclFactory fn = cc_emit_plan_lookup_container_factory("Vec");
    if (fn) fn(out, inst);
}

void cc_emit_plan_fprint_map_decl(FILE* out, const CCTypeInstantiation* inst) {
    CCContainerDeclFactory fn = cc_emit_plan_lookup_container_factory("Map");
    if (fn) fn(out, inst);
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
