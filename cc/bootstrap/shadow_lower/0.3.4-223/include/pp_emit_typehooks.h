/* Shadow emit typehooks: harvest @typehooks / @as, compile .ufcs, resolve.
 * Requires pp_emit_core.cch (tables, binds, reset). Included from pp_emit.cch
 * after core and before ufcs. Not a standalone TU. */
#pragma once

#include "pp_tape.h"
typedef CCSlice (*ShadowUfcsRewriteFn)(CCSlice recv_type, CCSlice method,
                                       CCSlice mode, CCSliceArray argv,
                                       CCSliceArray arg_types, CCArena* arena);

static void shadow_ufcs_sb_append(char** o, size_t* n, size_t* c, const char* s,
                                 size_t ln) {
    if (!o || !s || !ln) return;
    if (*n + ln + 1 > *c) {
        size_t nc = *c ? *c * 2 : 4096;
        char* nb;
        while (nc < *n + ln + 1) nc *= 2;
        nb = (char*)realloc(*o, nc);
        if (!nb) return;
        *o = nb;
        *c = nc;
    }
    memcpy(*o + *n, s, ln);
    *n += ln;
    (*o)[*n] = 0;
}

static int shadow_ufcs_expr_is_ident(const char* expr, size_t n, char* out,
                                    size_t cap) {
    size_t i = 0, o = 0;
    if (!expr || !out || !cap) return 0;
    while (i < n && isspace((unsigned char)expr[i])) i++;
    if (i >= n || !cc_is_ident_start(expr[i])) return 0;
    while (i < n && cc_is_ident_char(expr[i])) {
        if (o + 1 < cap) out[o++] = expr[i];
        i++;
    }
    while (i < n && isspace((unsigned char)expr[i])) i++;
    out[o] = 0;
    return i == n && o > 0;
}

/* Definition of `name` (signature + body), or 0 if not a top-level fn. */
static int shadow_ufcs_extract_fn(const char* src, size_t n, const char* name,
                                 const char** out_s, const char** out_e) {
    size_t nlen;
    size_t i;
    if (!src || !name || !name[0] || !out_s || !out_e) return 0;
    nlen = strlen(name);
    for (i = 0; i + nlen < n; i++) {
        size_t p, rp, q, rb, s;
        int newlines;
        if (memcmp(src + i, name, nlen) != 0) continue;
        if (i > 0 && cc_is_ident_char(src[i - 1])) continue;
        if (i + nlen < n && cc_is_ident_char(src[i + nlen])) continue;
        if (i > 0 && (src[i - 1] == '.' || src[i - 1] == '>')) continue;
        p = i + nlen;
        while (p < n && isspace((unsigned char)src[p])) p++;
        if (p >= n || src[p] != '(') continue;
        if (!cc_find_matching_paren(src, n, p, &rp)) continue;
        q = rp + 1;
        while (q < n && isspace((unsigned char)src[q])) q++;
        if (q >= n || src[q] != '{') continue;
        if (!cc_find_matching_brace(src, n, q, &rb)) continue;
        s = i;
        newlines = 0;
        while (s > 0) {
            char c = src[s - 1];
            if (c == ';' || c == '}' || c == '{') break;
            if (c == '\n') {
                size_t t;
                newlines++;
                if (newlines > 3) break;
                t = s;
                while (t < i && (src[t] == ' ' || src[t] == '\t')) t++;
                if (t < i && src[t] == '#') break;
            }
            s--;
        }
        while (s < i && isspace((unsigned char)src[s])) s++;
        *out_s = src + s;
        *out_e = src + rb + 1;
        return 1;
    }
    return 0;
}

/* Includes + user typedefs + named handler. Lambda TUs are includes only. */
static char* shadow_ufcs_slim_src(const char* src, size_t n, const char* expr,
                                 size_t expr_len, size_t* out_n) {
    char handler[128];
    char* out = NULL;
    size_t on = 0, oc = 0;
    char* types;
    const char* prelude =
        "#include <ccc/std/prelude.h>\n"
        "#include <ccc/cc_ufcs.h>\n";
    if (!src || !n || !out_n) return NULL;
    *out_n = 0;
    shadow_ufcs_sb_append(&out, &on, &oc, prelude, strlen(prelude));
    types = cc_ct_extract_type_decls_prelude(src, n);
    if (types && types[0])
        shadow_ufcs_sb_append(&out, &on, &oc, types, strlen(types));
    free(types);
    if (shadow_ufcs_expr_is_ident(expr, expr_len, handler, sizeof(handler))) {
        const char* fs = NULL;
        const char* fe = NULL;
        if (!shadow_ufcs_extract_fn(src, n, handler, &fs, &fe) || !fs || !fe) {
            free(out);
            return NULL;
        }
        shadow_ufcs_sb_append(&out, &on, &oc, fs, (size_t)(fe - fs));
        shadow_ufcs_sb_append(&out, &on, &oc, "\n", 1);
    }
    *out_n = on;
    return out;
}

static int shadow_ufcs_on_register(CCSymbolTable* t,
                                  const char* registration_input_path,
                                  const char* logical_file,
                                  const char* type_name, const char* expr_src,
                                  size_t expr_len, void* user_ctx) {
    void* owner = NULL;
    const void* fn = NULL;
    char* slim = NULL;
    size_t slim_n = 0;
    int rc;
    (void)user_ctx;
    if (!t || !type_name || !expr_src || !expr_len) return -1;
    /* Catch-all folklore hook stays invent-side. Compiling `@typehooks on *`
     * would run before dyn-sink / @as and hard-fail compose-misses that those
     * paths still own. Specific header subjects (CCChanTx_*, user .cch) load. */
    if (strcmp(type_name, "*") == 0) return 0;
    if (!g_shadow_ufcs_compile_src || !g_shadow_ufcs_compile_len) return -1;
    slim = shadow_ufcs_slim_src(g_shadow_ufcs_compile_src,
                                g_shadow_ufcs_compile_len, expr_src, expr_len,
                                &slim_n);
    if (!slim || !slim_n) {
        fprintf(stderr,
                "%s: error: failed to isolate @typehooks .ufcs handler for '%s'\n",
                registration_input_path ? registration_input_path
                                        : g_shadow_ufcs_path,
                type_name);
        free(slim);
        return -1;
    }
    rc = cc_comptime_compile_type_hook_callable(
            registration_input_path ? registration_input_path
                                    : g_shadow_ufcs_path,
            logical_file, slim, slim_n, expr_src, expr_len,
            CC_COMPTIME_TYPE_HOOK_UFCS, &owner, &fn);
    free(slim);
    if (rc != 0) return -1;
    if (cc_symbols_set_type_ufcs_callable(t, type_name, fn, owner,
                                         cc_comptime_type_hook_owner_free) !=
        0) {
        if (owner) cc_comptime_type_hook_owner_free(owner);
        return -1;
    }
    return 0;
}

/* Drive create/destroy/sink/niche tables off CCSymbolTable (one harvest). */
static void shadow_dyn_sink_register(const char* ty, const char* callee,
                                     const char* wrap, int dest_aware,
                                     int returns_result);
static int shadow_text_fn_returns_result(const char* text, const char* name);
static void shadow_niche_register(const char* ty, unsigned size, unsigned align,
                                  unsigned off, unsigned width,
                                  unsigned long long sentinel);

static void shadow_ufcs_sync_hooks_from_syms(const char* probe_text) {
    size_t i, n;
    if (!g_shadow_ufcs_syms) return;
    n = cc_symbols_type_count(g_shadow_ufcs_syms);
    for (i = 0; i < n; i++) {
        const char* ty = cc_symbols_type_name(g_shadow_ufcs_syms, i);
        const char* create = NULL;
        const char* destroy = NULL;
        const char* pre = NULL;
        const char* dyn_cal = NULL;
        const char* dyn_wrap = NULL;
        unsigned ns = 0, na = 0, no = 0, nw = 0;
        unsigned long long nsen = 0;
        char needle[160];
        if (!ty || !ty[0]) continue;
        /* Only materialize facts whose register site is in this collect text
         * — re-syncing the whole table against another file's probe_text
         * corrupts dyn-sink returns_result. */
        if (probe_text) {
            snprintf(needle, sizeof(needle), "cc_type_register(\"%s\"", ty);
            if (!strstr(probe_text, needle)) continue;
        }
        if (cc_symbols_lookup_type_create_call(g_shadow_ufcs_syms, ty, 1,
                                               &create) == 0 &&
            create && create[0])
            shadow_create_hook_register_arity(ty, 1, create);
        if (cc_symbols_lookup_type_create_call(g_shadow_ufcs_syms, ty, 2,
                                               &create) == 0 &&
            create && create[0])
            shadow_create_hook_register_arity(ty, 2, create);
        if (!shadow_create_hook_for_arity(ty, 1) &&
            cc_symbols_lookup_type_create_call(g_shadow_ufcs_syms, ty, 0,
                                               &create) == 0 &&
            create && create[0])
            shadow_create_hook_register_arity(ty, 1, create);
        (void)cc_symbols_lookup_type_pre_destroy_call(g_shadow_ufcs_syms, ty,
                                                      &pre);
        (void)cc_symbols_lookup_type_destroy_call(g_shadow_ufcs_syms, ty,
                                                  &destroy);
        if ((pre && pre[0]) || (destroy && destroy[0]))
            shadow_destroy_hook_register(ty, pre, destroy);
        if (cc_symbols_lookup_type_ufcs_dynamic(g_shadow_ufcs_syms, ty, &dyn_cal,
                                                &dyn_wrap) == 0 &&
            dyn_cal && dyn_cal[0] && dyn_wrap && dyn_wrap[0]) {
            shadow_dyn_sink_register(
                ty, dyn_cal, dyn_wrap, 1,
                probe_text ? shadow_text_fn_returns_result(probe_text, dyn_cal)
                           : 0);
        }
        if (cc_symbols_lookup_type_niche(g_shadow_ufcs_syms, ty, &ns, &na, &no,
                                         &nw, &nsen) == 0 &&
            nw)
            shadow_niche_register(ty, ns, na, no, nw, nsen);
    }
}

/* Compile `.ufcs` only for project TU / local .cch. Stdlib headers redefine
 * types inside the hook TCC session if we compile their full text. */
static int shadow_ufcs_compile_for_path(const char* path) {
    if (!path || !path[0]) return 0;
    if (strstr(path, "/cc/include/ccc/")) return 0;
    if (strstr(path, "/out/include/ccc/")) return 0;
    if (strstr(path, "/include/ccc/")) return 0;
    return 1;
}

static int shadow_ufcs_mark_seen(const char* path) {
    int i;
    if (!path || !path[0]) return 0;
    for (i = 0; i < g_shadow_ufcs_nseen; i++) {
        if (strcmp(g_shadow_ufcs_seen[i], path) == 0) return 1;
    }
    if (g_shadow_ufcs_nseen >= SHADOW_UFCS_SEEN_CAP) return 0;
    snprintf(g_shadow_ufcs_seen[g_shadow_ufcs_nseen],
             sizeof(g_shadow_ufcs_seen[0]), "%s", path);
    g_shadow_ufcs_nseen++;
    return 0;
}

/* Accumulate typehooks from one source into g_shadow_ufcs_syms (do not free).
 * `compile_ufcs`: compile `.ufcs` callables (project TU / local .cch only).
 * Stdlib headers still feed create/destroy/sink/niche via collect; compiling
 * their handlers against the full `.cch` redefines types inside TCC. */
static void shadow_ufcs_hooks_load(const char* path, const char* collect_src,
                                  size_t collect_n, const char* compile_src,
                                  size_t compile_n, int compile_ufcs) {
    if (!path || !collect_src || !compile_src) return;
    if (shadow_ufcs_mark_seen(path)) return;
    if (!g_shadow_ufcs_syms) {
        g_shadow_ufcs_syms = cc_symbols_new();
        if (!g_shadow_ufcs_syms) {
            fprintf(stderr,
                    "%s: error: typehooks symbol table alloc failed; refusing "
                    "silent UFCS invent\n",
                    path);
            g_shadow_ufcs_miss = 1;
            return;
        }
    }
    snprintf(g_shadow_ufcs_path, sizeof(g_shadow_ufcs_path), "%s", path);
    g_shadow_ufcs_compile_src = compile_src;
    g_shadow_ufcs_compile_len = compile_n;
    if (cc_symbols_collect_type_registrations_ex(
            g_shadow_ufcs_syms, path, collect_src, collect_n, NULL, NULL,
            compile_ufcs ? shadow_ufcs_on_register : NULL, NULL) != 0) {
        fprintf(stderr, "%s: error: failed to compile @typehooks .ufcs hook\n",
                path);
        g_shadow_ufcs_miss = 1;
    }
    shadow_ufcs_sync_hooks_from_syms(collect_src);
    g_shadow_ufcs_compile_src = NULL;
    g_shadow_ufcs_compile_len = 0;
}

static void shadow_ufcs_hooks_collect_text(const char* path, const char* text,
                                          size_t n, int compile_ufcs);

/* Paths spliced into stage1 (impl_cch_begin / local_cch_begin marks).
 * Impl-grade headers never enter lowered_local; collect follows the marks. */
static void shadow_ufcs_hooks_collect_spliced_marks(const char* text) {
    static const char* marks[2] = {
        "/*cc:impl_cch_begin:",
        "/*cc:local_cch_begin:",
    };
    int mi;
    if (!text) return;
    for (mi = 0; mi < 2; mi++) {
        const char* p = text;
        size_t ml = strlen(marks[mi]);
        while ((p = strstr(p, marks[mi])) != NULL) {
            const char* s = p + ml;
            const char* e = strstr(s, "*/");
            char path[512];
            char* src = NULL;
            size_t n = 0;
            size_t pl;
            p = e ? e + 2 : p + ml;
            if (!e || e <= s) continue;
            pl = (size_t)(e - s);
            if (pl == 0 || pl >= sizeof(path)) continue;
            memcpy(path, s, pl);
            path[pl] = 0;
            if (!read_file(path, &src, &n) || !src) continue;
            shadow_ufcs_hooks_collect_text(path, src, n,
                                           shadow_ufcs_compile_for_path(path));
            free(src);
        }
    }
}

/* Rewrite @typehooks → cc_type_register, then collect (+ optional .ufcs). */
static void shadow_ufcs_hooks_collect_text(const char* path, const char* text,
                                          size_t n, int compile_ufcs) {
    char* rw = NULL;
    if (!path || !text) return;
    rw = cc_rewrite_typehooks_to_register(text, n);
    {
        const char* src = rw ? rw : text;
        size_t sn = rw ? strlen(rw) : n;
        shadow_ufcs_hooks_load(path, src, sn, src, sn, compile_ufcs);
    }
    free(rw);
}

enum { SHADOW_UFCS_HOOK_ARG_MAX = 8 };

static int shadow_ufcs_split_args(const char* a, const char** starts, size_t* lens,
                                 int max) {
    int n = 0;
    const char* p;
    int depth = 0, in_str = 0, in_chr = 0;
    if (!a || !starts || !lens || max <= 0) return 0;
    p = a;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return 0;
    starts[0] = p;
    for (; *p; p++) {
        if (in_str) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == '"') in_str = 0;
            continue;
        }
        if (in_chr) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == '\'') in_chr = 0;
            continue;
        }
        if (*p == '"') { in_str = 1; continue; }
        if (*p == '\'') { in_chr = 1; continue; }
        if (*p == '(' || *p == '[' || *p == '{') { depth++; continue; }
        if (*p == ')' || *p == ']' || *p == '}') {
            if (depth) depth--;
            continue;
        }
        if (*p == ',' && depth == 0) {
            const char* e = p;
            while (e > starts[n] && (e[-1] == ' ' || e[-1] == '\t')) e--;
            lens[n] = (size_t)(e - starts[n]);
            n++;
            /* Another arg follows this comma — refuse silent truncate. */
            if (n >= max) return -1;
            p++;
            while (*p == ' ' || *p == '\t') p++;
            starts[n] = p;
            p--;
        }
    }
    {
        const char* e = p;
        while (e > starts[n] && (e[-1] == ' ' || e[-1] == '\t')) e--;
        if (e > starts[n]) {
            lens[n] = (size_t)(e - starts[n]);
            n++;
        }
    }
    return n;
}

static void shadow_ufcs_infer_one_type(const char* arg, size_t n, char* out,
                                      size_t cap) {
    size_t i = 0, e = n;
    if (!arg || !out || !cap) return;
    out[0] = 0;
    while (i < e && (arg[i] == ' ' || arg[i] == '\t')) i++;
    while (e > i && (arg[e - 1] == ' ' || arg[e - 1] == '\t')) e--;
    if (i >= e) return;
    if (arg[i] == '"') {
        snprintf(out, cap, "const char*");
        return;
    }
    {
        size_t k = i;
        if (arg[k] == '-' || arg[k] == '+') k++;
        if (k < e && arg[k] >= '0' && arg[k] <= '9') {
            int all = 1;
            for (; k < e; k++) {
                if (arg[k] < '0' || arg[k] > '9') {
                    all = 0;
                    break;
                }
            }
            if (all) {
                snprintf(out, cap, "int");
                return;
            }
        }
    }
    if (cc_is_ident_start(arg[i])) {
        char id[96];
        size_t o = 0, k = i;
        const ShadowBind* b;
        while (k < e && cc_is_ident_char(arg[k]) && o + 1 < sizeof(id))
            id[o++] = arg[k++];
        id[o] = 0;
        if (k == e) {
            b = shadow_bind_lookup(id);
            if (b && b->ty[0]) snprintf(out, cap, "%s", b->ty);
        }
    }
}

/* Invoke the longest-matching `.ufcs` hook. 1 = use `out` as callee.
 * 0 = no hook / pass-through. Empty return is a hard reject.
 * `cc_ufcs_emit_value[_cstr]` tags a by-value receiver (never `&recv`). */
static int shadow_ufcs_hook_resolve(const char* ty, const char* method,
                                   const char* args, const char* view_mode,
                                   char* out, size_t cap, int* out_by_value) {
    const void* fn = NULL;
    ShadowUfcsRewriteFn rewrite;
    CCSlice recv, meth, mode, result;
    CCSliceArray argv;
    CCSliceArray arg_types;
    char base[96];
    size_t n;
    const char* pass = "__cc_ufcs_pass__";
    const char* vtag = "__cc_ufcs_value__:";
    size_t vtag_n = sizeof("__cc_ufcs_value__:") - 1;
    if (out_by_value) *out_by_value = 0;
    if (!g_shadow_ufcs_syms || !ty || !ty[0] || !method || !method[0] || !out ||
        !cap)
        return 0;
    snprintf(base, sizeof(base), "%s", ty);
    n = strlen(base);
    while (n && (base[n - 1] == '*' || base[n - 1] == ' ')) base[--n] = 0;
    if (!base[0]) return 0;
    if (cc_symbols_lookup_type_ufcs_callable(g_shadow_ufcs_syms, base, &fn) !=
            0 ||
        !fn)
        return 0;
    rewrite = (ShadowUfcsRewriteFn)fn;
    recv = cc_slice_from_static((void*)base, strlen(base));
    meth = cc_slice_from_static((void*)method, strlen(method));
    /* Named `@typeview Mode on T` — `view_mode` is that Mode. Unnamed
     * views pass empty (the `.ufcs` parameter is not a call-site invent). */
    if (view_mode && view_mode[0])
        mode = cc_slice_from_static((void*)view_mode, strlen(view_mode));
    else
        mode = cc_slice_from_static((void*)"", 0);
    {
        const char* starts[SHADOW_UFCS_HOOK_ARG_MAX];
        size_t lens[SHADOW_UFCS_HOOK_ARG_MAX];
        CCSlice argv_store[SHADOW_UFCS_HOOK_ARG_MAX];
        CCSlice type_store[SHADOW_UFCS_HOOK_ARG_MAX];
        char type_bufs[SHADOW_UFCS_HOOK_ARG_MAX][80];
        int na = shadow_ufcs_split_args(args, starts, lens, SHADOW_UFCS_HOOK_ARG_MAX);
        int ai;
        if (na < 0) {
            fprintf(stderr,
                    "error: type: .ufcs hook for '%s.%s' exceeds %d args; "
                    "refusing silent truncate\n",
                    base, method, SHADOW_UFCS_HOOK_ARG_MAX);
            g_shadow_ufcs_miss = 1;
            return 0;
        }
        for (ai = 0; ai < na; ai++) {
            argv_store[ai] = cc_slice_from_static((void*)starts[ai], lens[ai]);
            shadow_ufcs_infer_one_type(starts[ai], lens[ai], type_bufs[ai],
                                       sizeof(type_bufs[ai]));
            type_store[ai] = cc_slice_from_static((void*)type_bufs[ai],
                                                 strlen(type_bufs[ai]));
        }
        argv.items = na ? argv_store : NULL;
        argv.len = (size_t)na;
        arg_types.items = na ? type_store : NULL;
        arg_types.len = (size_t)na;
        shadow_meta_ar_ensure();
        result = rewrite(recv, meth, mode, argv, arg_types, &g_shadow_meta_ar);
    }
    if (result.ptr && result.len == sizeof("__cc_ufcs_pass__") - 1 &&
        memcmp(result.ptr, pass, result.len) == 0)
        return 0;
    if (!result.ptr || result.len == 0) {
        fprintf(stderr,
                "error: type: .ufcs hook rejected method '%s' for '%s'\n",
                method, base);
        g_shadow_ufcs_miss = 1;
        return 0;
    }
    if (result.len >= vtag_n && memcmp(result.ptr, vtag, vtag_n) == 0) {
        result.ptr = (char*)result.ptr + vtag_n;
        result.len -= vtag_n;
        if (out_by_value) *out_by_value = 1;
    }
    if (!result.ptr || result.len == 0) {
        fprintf(stderr,
                "error: type: .ufcs hook rejected method '%s' for '%s'\n",
                method, base);
        g_shadow_ufcs_miss = 1;
        return 0;
    }
    if (result.len + 1 > cap) {
        fprintf(stderr,
                "error: type: .ufcs hook rewrite for '%s.%s' overflowed "
                "callee buffer (%zu); refusing silent invent\n",
                base, method, cap);
        g_shadow_ufcs_miss = 1;
        return 0;
    }
    memcpy(out, result.ptr, result.len);
    out[result.len] = 0;
    return 1;
}

typedef struct {
    char outer[64];
    char name[64];
    char ty[64];
} ShadowAsScanFld;

static int shadow_as_scan_fld_push(ShadowAsScanFld** v, int* n, int* cap,
                                  const char* outer, const char* name,
                                  const char* ty) {
    ShadowAsScanFld* nv;
    int nc;
    if (!v || !n || !cap || !name || !name[0] || !ty) return 0;
    if (*n >= *cap) {
        nc = *cap ? *cap * 2 : 8;
        nv = (ShadowAsScanFld*)realloc(*v, (size_t)nc * sizeof(ShadowAsScanFld));
        if (!nv) {
            fprintf(stderr,
                    "error: type: field harvest grow failed on '%s.%s'\n",
                    outer && outer[0] ? outer : "(anonymous)", name);
            g_shadow_ufcs_miss = 1;
            return 0;
        }
        *v = nv;
        *cap = nc;
    }
    snprintf((*v)[*n].outer, sizeof((*v)[0].outer), "%s", outer ? outer : "");
    snprintf((*v)[*n].name, sizeof((*v)[0].name), "%s", name);
    snprintf((*v)[*n].ty, sizeof((*v)[0].ty), "%s", ty);
    (*n)++;
    return 1;
}

/* Scan header text for `Type field @as;` / `@typeview on T { as:… }`.
 * create/destroy/sink/niche/.ufcs come from CCSymbolTable via
 * shadow_ufcs_hooks_collect_* (one harvest — no parallel strstr). */
static void shadow_as_scan_header_text(const char* text) {
    const char* p;
    char outer[96];
    int in_typedef_struct = 0;
    /* Pending @as inside anonymous `typedef struct { … } Alias;` */
    char pend_field[8][64];
    char pend_target[8][64];
    int npend = 0;
    /* Current struct only. Grows. Reset at each `typedef struct` so a
     * later anonymous alias (RtxDoc) is not a leftover of earlier types. */
    ShadowAsScanFld* flds = NULL;
    int nfld = 0;
    int fld_cap = 0;
    /* `@typeview on Base { … }` body capture (may span lines). */
    char tv_base[96];
    int tv_depth = 0;
    if (!text) return;
    outer[0] = 0;
    tv_base[0] = 0;
    p = text;
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[512];
        const char* as;
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;
        /* Skip line comments. */
        {
            char* cmt = strstr(line, "//");
            if (cmt) *cmt = 0;
        }
        if (strstr(line, "typedef struct") ||
            (strstr(line, "struct ") && strchr(line, '{'))) {
            const char* s = strstr(line, "struct");
            if (s) {
                s += 6;
                while (*s == ' ' || *s == '\t') s++;
                if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
                    *s == '_') {
                    size_t n = 0;
                    while (shadow_is_ident_char(s[n]) && n + 1 < sizeof(outer))
                        n++;
                    memcpy(outer, s, n);
                    outer[n] = 0;
                    in_typedef_struct = 1;
                    npend = 0;
                    nfld = 0;
                } else if (*s == '{') {
                    outer[0] = 0;
                    in_typedef_struct = 1;
                    npend = 0;
                    nfld = 0;
                }
            }
        }
        {
            const char* cl = line;
            while (*cl == ' ' || *cl == '\t') cl++;
            if (in_typedef_struct && *cl == '}') {
            const char* a = cl + 1;
            int pi;
            while (*a == ' ' || *a == '\t') a++;
            if (((*a >= 'A' && *a <= 'Z') || (*a >= 'a' && *a <= 'z') ||
                 *a == '_')) {
                size_t n = 0;
                char alias[96];
                while (shadow_is_ident_char(a[n]) && n + 1 < sizeof(alias)) n++;
                memcpy(alias, a, n);
                alias[n] = 0;
                if (alias[0]) {
                    /* Retarget pending fields recorded under empty outer.
                     * Also install them on the alias — `@typeview` in a
                     * later file uses field_ty_of, not this scan's fld_*. */
                    int fi;
                    for (fi = 0; fi < nfld; fi++) {
                        int same = !flds[fi].outer[0] ||
                                   (outer[0] &&
                                    strcmp(flds[fi].outer, outer) == 0);
                        if (same) {
                            if (!flds[fi].outer[0])
                                snprintf(flds[fi].outer, sizeof(flds[fi].outer),
                                         "%s", alias);
                            shadow_field_register_ex(alias, flds[fi].name,
                                                     flds[fi].ty);
                        }
                    }
                    snprintf(outer, sizeof(outer), "%s", alias);
                }
            }
            for (pi = 0; pi < npend && outer[0]; pi++)
                shadow_as_register(outer, pend_field[pi], pend_target[pi]);
            npend = 0;
            in_typedef_struct = 0;
            }
        }
        /* Record ordinary fields for later typeview as: lookup.
         * `struct Tag name` and `Tag *name` are fields; register even
         * when the local as: table is full. */
        if (in_typedef_struct) {
            const char* t0 = line;
            char fty[64];
            char fname[64];
            size_t ti = 0, fi = 0;
            const char* mid;
            int stars = 0;
            while (*t0 == ' ' || *t0 == '\t') t0++;
            for (;;) {
                if (strncmp(t0, "const ", 6) == 0) t0 += 6;
                else if (strncmp(t0, "volatile ", 9) == 0) t0 += 9;
                else if (strncmp(t0, "struct ", 7) == 0) t0 += 7;
                else if (strncmp(t0, "union ", 6) == 0) t0 += 6;
                else if (strncmp(t0, "enum ", 5) == 0) t0 += 5;
                else break;
                while (*t0 == ' ' || *t0 == '\t') t0++;
            }
            while (shadow_is_ident_char(t0[ti]) && ti + 1 < sizeof(fty)) {
                fty[ti] = t0[ti];
                ti++;
            }
            fty[ti] = 0;
            mid = t0 + ti;
            while (*mid == ' ' || *mid == '\t') mid++;
            while (*mid == '*') {
                stars++;
                mid++;
                while (*mid == ' ' || *mid == '\t') mid++;
            }
            if (fty[0] && *mid != '(') {
                while (shadow_is_ident_char(mid[fi]) && fi + 1 < sizeof(fname)) {
                    fname[fi] = mid[fi];
                    fi++;
                }
                fname[fi] = 0;
                    if (fname[0] && strcmp(fname, fty) != 0) {
                    while (stars > 0 && ti + 1 < sizeof(fty)) {
                        fty[ti++] = '*';
                        fty[ti] = 0;
                        stars--;
                    }
                    if (outer[0])
                        shadow_field_register_ex(outer, fname, fty);
                    (void)shadow_as_scan_fld_push(&flds, &nfld, &fld_cap, outer,
                                                  fname, fty);
                }
            }
        }
        /* Type field @as; — also host-C comment-form slash-star @as star-slash. */
        as = strstr(line, "/*@as*/");
        if (as && in_typedef_struct && as > line) {
            const char* q = as;
            char field[64];
            char target[64];
            size_t fi = 0, ti = 0;
            const char* t0;
            while (q > line && (q[-1] == ' ' || q[-1] == '\t')) q--;
            while (q > line && shadow_is_ident_char(q[-1])) q--;
            while (shadow_is_ident_char(q[fi]) && fi + 1 < sizeof(field)) {
                field[fi] = q[fi];
                fi++;
            }
            field[fi] = 0;
            t0 = line;
            while (*t0 == ' ' || *t0 == '\t') t0++;
            /* Skip storage / qualifiers / `struct Tag`. */
            for (;;) {
                if (strncmp(t0, "const ", 6) == 0) t0 += 6;
                else if (strncmp(t0, "volatile ", 9) == 0) t0 += 9;
                else if (strncmp(t0, "struct ", 7) == 0) t0 += 7;
                else if (strncmp(t0, "union ", 6) == 0) t0 += 6;
                else if (strncmp(t0, "enum ", 5) == 0) t0 += 5;
                else break;
                while (*t0 == ' ' || *t0 == '\t') t0++;
            }
            while (shadow_is_ident_char(t0[ti]) && ti + 1 < sizeof(target)) {
                target[ti] = t0[ti];
                ti++;
            }
            target[ti] = 0;
            /* Reject pointer @as: `Type *name @as`. */
            {
                const char* mid = t0 + ti;
                while (*mid == ' ' || *mid == '\t') mid++;
                if (*mid == '*') {
                    /* skip */
                } else if (field[0] && target[0] &&
                           strcmp(field, target) != 0) {
                    if (outer[0])
                        shadow_as_register(outer, field, target);
                    else if (npend < 8) {
                        snprintf(pend_field[npend], sizeof(pend_field[0]),
                                 "%s", field);
                        snprintf(pend_target[npend], sizeof(pend_target[0]),
                                 "%s", target);
                        npend++;
                    }
                }
            }
        }
        /* `@typeview on Base` — collect as: faces. */
        {
            const char* kw = strstr(line, "@typeview");
            if (kw && strstr(kw, " on ")) {
                const char* on = strstr(kw, " on ");
                const char* b;
                size_t bn = 0;
                on += 4;
                while (*on == ' ' || *on == '\t') on++;
                b = on;
                while (shadow_is_ident_char(b[bn]) && bn + 1 < sizeof(tv_base))
                    bn++;
                /* Trailing glob `*` (`CCSlice_*`). */
                if (b[bn] == '*' && bn + 1 < sizeof(tv_base)) bn++;
                if (bn > 0) {
                    memcpy(tv_base, b, bn);
                    tv_base[bn] = 0;
                    tv_depth = 0;
                    if (strchr(line, '{')) tv_depth = 1;
                }
            }
        }
        if (tv_base[0] && tv_depth > 0) {
            const char* q = line;
            while ((q = strstr(q, "as:")) != NULL) {
                q += 3;
                while (*q) {
                    char fname[64];
                    size_t fi = 0;
                    int i;
                    while (*q == ' ' || *q == '\t' || *q == ',') q++;
                    if (!*q || *q == ';' || *q == '}' || *q == '\n') break;
                    if (*q == 'r' && q[1] == ':') break;
                    if (*q == 'w' && q[1] == ':') break;
                    if (*q == 'r' && q[1] == 'w' && q[2] == ':') break;
                    while (shadow_is_ident_char(q[fi]) && fi + 1 < sizeof(fname)) {
                        fname[fi] = q[fi];
                        fi++;
                    }
                    if (!fi) break;
                    fname[fi] = 0;
                    q += fi;
                    if (shadow_ty_is_glob(tv_base))
                        shadow_as_glob_register(tv_base, fname);
                    {
                        int matched = 0;
                        char fty[64];
                        for (i = 0; i < nfld; i++) {
                            int match = 0;
                            if (shadow_ty_is_glob(tv_base))
                                match = shadow_restrict_pattern_matches(
                                            tv_base, flds[i].outer) &&
                                        strcmp(flds[i].name, fname) == 0;
                            else
                                match = strcmp(flds[i].outer, tv_base) == 0 &&
                                        strcmp(flds[i].name, fname) == 0;
                            if (match) {
                                shadow_as_register(flds[i].outer, fname,
                                                   flds[i].ty);
                                matched = 1;
                                if (shadow_ty_is_glob(tv_base)) {
                                    int gi;
                                    for (gi = 0; gi < g_shadow_nas_globs; gi++) {
                                        if (strcmp(g_shadow_as_globs[gi].pat,
                                                   tv_base) == 0 &&
                                            strcmp(g_shadow_as_globs[gi].field,
                                                   fname) == 0)
                                            g_shadow_as_globs[gi].hits++;
                                    }
                                }
                            }
                        }
                        if (!matched && !shadow_ty_is_glob(tv_base) &&
                            shadow_field_ty_of(tv_base, fname, fty,
                                              sizeof(fty)))
                            shadow_as_register(tv_base, fname, fty);
                    }
                    while (*q == ' ' || *q == '\t') q++;
                    if (*q == ',') {
                        q++;
                        continue;
                    }
                    break;
                }
            }
            {
                const char* c;
                for (c = line; *c; c++) {
                    if (*c == '{') tv_depth++;
                    else if (*c == '}') {
                        tv_depth--;
                        if (tv_depth <= 0) {
                            tv_base[0] = 0;
                            tv_depth = 0;
                            break;
                        }
                    }
                }
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    free(flds);
}

static int shadow_as_project_header_read(const char* rel, const char* from_dir,
                                          char** out, size_t* out_len,
                                          char* opened, size_t ocap) {
    char cand[512];
    if (!rel || !rel[0] || !out || !out_len) return 0;
    *out = NULL;
    *out_len = 0;
    if (opened && ocap) opened[0] = 0;
    if (!shadow_project_face_path(rel, from_dir, cand, sizeof(cand)))
        return 0;
    if (opened && ocap) snprintf(opened, ocap, "%s", cand);
    return read_file(cand, out, out_len) && *out != NULL;
}

static void shadow_as_scan_nested_incs(const char* text, int depth,
                                      const char* from_dir);

static void shadow_as_scan_opened(const char* rel, const char* opened_path,
                                 char* text, size_t tlen, int compile,
                                 int nest, const char* from_dir) {
    char ndir[512];
    if (!text) return;
    shadow_as_scan_header_text(text);
    shadow_ufcs_hooks_collect_text(rel, text, tlen, compile);
    if (nest <= 0) return;
    ndir[0] = 0;
    if (opened_path && opened_path[0])
        shadow_dir_of(opened_path, ndir, sizeof(ndir));
    shadow_as_scan_nested_incs(text, nest, ndir[0] ? ndir : from_dir);
}

static int shadow_as_scan_rel(const char* rel, const char* from_dir,
                              int compile, int nest) {
    char* text = NULL;
    size_t tlen = 0;
    char opened[512];
    const char* fam = rel;
    if (!rel || !rel[0]) return 0;
    opened[0] = 0;
    if (strncmp(rel, "ccc/", 4) == 0) fam = rel + 4;
    if (shadow_family_header_read(fam, &text, &tlen) && text) {
        shadow_as_scan_opened(rel, NULL, text, tlen, compile, nest, from_dir);
        free(text);
        return 1;
    }
    if (shadow_as_project_header_read(rel, from_dir, &text, &tlen, opened,
                                     sizeof(opened)) &&
        text) {
        shadow_as_scan_opened(rel, opened, text, tlen, compile, nest,
                             from_dir);
        free(text);
        return 1;
    }
    return 0;
}

/* Follow #include <ccc/…> / "…" — stdlib via family read, project
 * faces beside the including file (driver-rewritten .h included). */
static void shadow_as_scan_nested_incs(const char* text, int depth,
                                      const char* from_dir) {
    const char* p;
    if (!text || depth <= 0) return;
    p = text;
    while ((p = strstr(p, "#include")) != NULL) {
        const char* lt;
        const char* gt;
        char rel[256];
        size_t n;
        p += 8;
        lt = strchr(p, '<');
        gt = lt ? strchr(lt, '>') : NULL;
        if (!lt || !gt || gt <= lt + 1) {
            lt = strchr(p, '"');
            gt = lt ? strchr(lt + 1, '"') : NULL;
            if (!lt || !gt || gt <= lt + 1) continue;
            lt++;
        } else
            lt++;
        n = (size_t)(gt - lt);
        if (n >= sizeof(rel)) n = sizeof(rel) - 1;
        memcpy(rel, lt, n);
        rel[n] = 0;
        /* Prefer .cch facts; strip generated .h suffix.
         * Do not spell ".h" in a string literal — header lowerer rewrites
         * those to ".h" (same constraint as pp_stage2 umbrella check). */
        if (n > 2 && strcmp(rel + n - 2, ".h") == 0) {
            size_t rl;
            rel[n - 2] = 0;
            rl = strlen(rel);
            if (rl + 4 < sizeof(rel)) {
                rel[rl] = '.';
                rel[rl + 1] = 'c';
                rel[rl + 2] = 'c';
                rel[rl + 3] = 'h';
                rel[rl + 4] = 0;
            }
        }
        shadow_as_scan_rel(rel, from_dir, 0, depth - 1);
        p = gt + 1;
    }
}

/* Resolve #include <ccc/…> / "…" from pass_inc lines and scan for @as/hooks. */
static void shadow_as_scan_pass_inc(char pass_inc[][256], int npass_inc,
                                   const char* from_dir) {
    int i;
    for (i = 0; i < npass_inc; i++) {
        const char* line = pass_inc[i];
        const char* lt;
        const char* gt;
        char rel[256];
        size_t n;
        if (!line) continue;
        lt = strchr(line, '<');
        gt = lt ? strchr(lt, '>') : NULL;
        if (!lt || !gt || gt <= lt + 1) {
            lt = strchr(line, '"');
            gt = lt ? strchr(lt + 1, '"') : NULL;
            if (!lt || !gt || gt <= lt + 1) continue;
            lt++;
        } else
            lt++;
        n = (size_t)(gt - lt);
        if (n >= sizeof(rel)) n = sizeof(rel) - 1;
        memcpy(rel, lt, n);
        rel[n] = 0;
        /* Same no-".h"-literal rule as nested scan above. */
        if (n > 2 && strcmp(rel + n - 2, ".h") == 0) {
            size_t rl;
            rel[n - 2] = 0;
            rl = strlen(rel);
            if (rl + 4 < sizeof(rel)) {
                rel[rl] = '.';
                rel[rl + 1] = 'c';
                rel[rl + 2] = 'c';
                rel[rl + 3] = 'h';
                rel[rl + 4] = 0;
            }
        }
        shadow_as_scan_rel(rel, from_dir, 0, 2);
    }
}

/* Scan warmed tapes (TU + includes) for @as / typeview.
 * Collect typehooks from the original path — stage1 blanks @comptime. */
static void shadow_as_scan_tapes(TapeCache* cache) {
    int i;
    if (!cache) return;
    for (i = 0; i < cache->n; i++) {
        FileTape* ft = cache->items[i];
        char* text = NULL;
        size_t tlen = 0;
        if (!ft) continue;
        if (ft->bytes && ft->len > 0) {
            char ndir[512];
            ndir[0] = 0;
            shadow_as_scan_header_text(ft->bytes);
            shadow_ufcs_hooks_collect_spliced_marks(ft->bytes);
            if (ft->path && ft->path[0])
                shadow_dir_of(ft->path, ndir, sizeof(ndir));
            shadow_as_scan_nested_incs(ft->bytes, 2,
                                       ndir[0] ? ndir : NULL);
        }
        if (!ft->path || !ft->path[0]) continue;
        if (!read_file(ft->path, &text, &tlen) || !text) continue;
        shadow_ufcs_hooks_collect_text(ft->path, text, tlen,
                                       shadow_ufcs_compile_for_path(ft->path));
        free(text);
    }
}

/* System `.cch` registered when a splice rewrites `<ccc/….h>` → `.h`
 * (those lines sit under `#ifndef` and never become pass_inc). */
static void shadow_as_scan_included_cch(void) {
    size_t i, n = cc_included_cch_source_count();
    for (i = 0; i < n; i++) {
        const char* path = cc_included_cch_source_path(i);
        char* text = NULL;
        size_t tlen = 0;
        if (!path || !path[0]) continue;
        if (!read_file(path, &text, &tlen) || !text) continue;
        shadow_as_scan_header_text(text);
        shadow_ufcs_hooks_collect_text(path, text, tlen, 0);
        free(text);
    }
}

/* Quoted project `.cch` lowers to `.h` (typeview/typehooks stripped), same
 * as stdlib.  Recover those facts from the original `.cch` the lowerer
 * registered — `shadow_family_header_read` only sees `cc/include/ccc/`. */
static void shadow_as_scan_lowered_local_cch(void) {
    size_t i, n = cc_lowered_local_header_count();
    for (i = 0; i < n; i++) {
        const char* path = cc_lowered_local_header_source_path(i);
        char* text = NULL;
        size_t tlen = 0;
        if (!path || !path[0]) continue;
        if (!read_file(path, &text, &tlen) || !text) continue;
        shadow_as_scan_header_text(text);
        shadow_ufcs_hooks_collect_text(path, text, tlen, 1);
        free(text);
    }
}

/* Original TU source (pre-blank): stage1 blanks @comptime on tapes.
 * Follow quoted includes — after driver rewrite those are .h passthrough. */
static void shadow_as_scan_src_path(const char* path) {
    char* text = NULL;
    size_t n = 0;
    char from_dir[512];
    if (!path || !path[0]) return;
    if (!read_file(path, &text, &n) || !text) return;
    shadow_as_scan_header_text(text);
    shadow_ufcs_hooks_collect_text(path, text, n, 1);
    from_dir[0] = 0;
    shadow_fill_quote_dir(path, from_dir, sizeof(from_dir));
    shadow_as_scan_nested_incs(text, 2, from_dir[0] ? from_dir : NULL);
    free(text);
}

