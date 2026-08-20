/*
 * CCEmitPlan — unified splice anchors (track A2).
 */
#include "emit_plan.h"
#include "factory_abi.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>

#include <ccc/cc_arena.h>
/* Host tools (lower_headers_stage1) do not link runtime/string.c — that TU is
 * unity-included in concurrent_c.o for shadow_lower / ccc.  Take the same
 * static-inline CCString bodies the comptime executor uses. */
#define CC_COMPTIME 1
#include <ccc/std/string.h>
#undef CC_COMPTIME

#include "comptime/executor.h"
#include "comptime/hook_compile.h"
#include "preprocess/emit_limits.h"
#include "result_spec.h"
#include "preprocess/preprocess.h"
#include "preprocess/type_registry.h"
#include "preprocess/template_scan.h"
#include "util/path.h"
#include "util/text.h"
#include "util/text_scan.h"
#include "visitor/pass_type_syntax.h"

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
    size_t p = cc_skip_ws_and_comments(src, len, *pos);
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
    size_t p = cc_skip_ws_and_comments(src, len, *pos);
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
    p = cc_skip_ws_and_comments(src, len, p);
    if (p >= len || src[p] != '(') return 0;
    p++;
    if (!cc__emit_parse_anchor(src, len, &p, &anchor)) return 0;
    p = cc_skip_ws_and_comments(src, len, p);
    if (p >= len || src[p] != ',') return 0;
    p = cc_skip_ws_and_comments(src, len, p + 1);
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
    p = cc_skip_ws_and_comments(src, len, p);
    if (p >= len || src[p] != '(') return 0;
    p++;
    if (!cc__emit_parse_anchor(src, len, &p, &anchor)) return 0;
    p = cc_skip_ws_and_comments(src, len, p);
    if (p >= len || src[p] != ',') return 0;
    p = cc_skip_ws_and_comments(src, len, p + 1);
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
        p = cc_skip_ws_and_comments(src, len, p);
        if (p >= len || src[p] != ',') return 0;
        p = cc_skip_ws_and_comments(src, len, p + 1);
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
 *   - NATIVE_DECL : optional compiler-native C fn that emits a container
 *                   monomorph's declaration from a CCTypeInstantiation
 *                   (type-graph emission loop; not seeded for Vec/Map).
 *   - TEMPLATE    : a declarative `$0..$N` template expanded at the use site.
 *   - COMPILED    : a `@comptime` factory compiled to a dylib and invoked at
 *                   the use site.
 * One array keyed by (name, kind). Vec/Map/ArrayMap splice through
 * CC_GENERIC_FACTORY; NATIVE_DECL is only used when a library registers one. */
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
    /* Where the factory was declared, so a constraint it reports names its
     * own line instead of line 1.  The factory body runs compiled and
     * detached, so nothing else in the invoke path knows the source. */
    size_t        site_pos;
} CCGenericReg;

#define CC_EMIT_PLAN_MAX_GENERICS 128
static CCGenericReg cc__generics[CC_EMIT_PLAN_MAX_GENERICS];
static size_t cc__generic_count = 0;

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

/* --- factory-instance member sets (UFCS trust) ---
 *
 * A factory instance's methods exist only inside its emitted definition
 * fragment, which splices after the passes that answer "is this name
 * real".  Harvest the decl-shaped `<mangled>_<member>(` names from each
 * produced definition so the UFCS family oracle can trust composed
 * member spellings for factory instances, mirroring how macro families
 * derive their member sets from `##_` tokens. */
#define CC_GEN_INSTANCE_MAX 128
typedef struct CCGenericInstance {
    char* family;    /* owns: "Pair" */
    char* mangled;   /* owns: "Pair_int_double" */
    char* members;   /* owns: "make, head" (csv, derivation order) */
    char* def_text;  /* owns: the emitted definition (return-type reads) */
} CCGenericInstance;
static CCGenericInstance cc__gen_instances[CC_GEN_INSTANCE_MAX];
static size_t cc__gen_instance_count = 0;

static CCGenericInstance* cc__gen_instance_find(const char* mangled) {
    if (!mangled || !mangled[0]) return NULL;
    for (size_t i = 0; i < cc__gen_instance_count; i++)
        if (strcmp(cc__gen_instances[i].mangled, mangled) == 0)
            return &cc__gen_instances[i];
    return NULL;
}

static int cc__gen_members_csv_has(const char* csv, const char* member) {
    size_t ml = strlen(member);
    const char* p = csv;
    while (p && *p) {
        while (*p == ' ' || *p == ',') p++;
        if (strncmp(p, member, ml) == 0 && (p[ml] == 0 || p[ml] == ',')) return 1;
        p = strchr(p, ',');
    }
    return 0;
}

/* Append decl-shaped `<mangled>_<member>(` names in `def` to a csv. */
static char* cc__gen_instance_scan_members(const char* mangled, const char* def) {
    size_t n = strlen(def);
    size_t ml = strlen(mangled);
    size_t i = 0;
    char* csv = NULL;
    size_t csv_len = 0, csv_cap = 0;
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    while (i + ml + 1 < n) {
        size_t e, q, b;
        char member[128];
        size_t mlen2;
        if (cc_inert_scan_step(&scan, def, n, &i)) continue;
        if (def[i] != mangled[0]) { i++; continue; }
        if (i > 0 && cc_is_ident_char(def[i - 1])) { i++; continue; }
        if (memcmp(def + i, mangled, ml) != 0 || def[i + ml] != '_') { i++; continue; }
        e = i + ml + 1;
        while (e < n && cc_is_ident_char(def[e])) e++;
        mlen2 = e - (i + ml + 1);
        if (mlen2 == 0 || mlen2 >= sizeof(member)) { i = e; continue; }
        q = e;
        while (q < n && (def[q] == ' ' || def[q] == '\t')) q++;
        if (q >= n || def[q] != '(') { i = e; continue; }
        /* Decl-shaped: a return-type token (ident or `*`) precedes. */
        b = cc_rskip_ws_and_comments(def, i);
        if (b == 0 || !(cc_is_ident_char(def[b - 1]) || def[b - 1] == '*')) {
            i = e;
            continue;
        }
        memcpy(member, def + i + ml + 1, mlen2);
        member[mlen2] = 0;
        if (!csv || !cc__gen_members_csv_has(csv, member)) {
            if (csv_len) cc_sb_append_cstr(&csv, &csv_len, &csv_cap, ", ");
            cc_sb_append_cstr(&csv, &csv_len, &csv_cap, member);
        }
        i = e;
    }
    return csv;
}

void cc_emit_plan_note_generic_instance(const char* family, const char* mangled,
                                        const char* def_text) {
    CCGenericInstance* gi;
    if (!family || !mangled || !mangled[0] || !def_text) return;
    if (cc__gen_instance_find(mangled)) return;
    if (cc__gen_instance_count >= CC_GEN_INSTANCE_MAX) return;
    gi = &cc__gen_instances[cc__gen_instance_count];
    gi->family = strdup(family);
    gi->mangled = strdup(mangled);
    gi->def_text = strdup(def_text);
    gi->members = cc__gen_instance_scan_members(mangled, def_text);
    if (!gi->family || !gi->mangled || !gi->def_text) {
        free(gi->family);
        free(gi->mangled);
        free(gi->def_text);
        free(gi->members);
        memset(gi, 0, sizeof(*gi));
        return;
    }
    cc__gen_instance_count++;
}

int cc_emit_plan_generic_instance_known(const char* mangled) {
    return cc__gen_instance_find(mangled) != NULL;
}

int cc_emit_plan_generic_instance_has_member(const char* mangled, const char* member) {
    CCGenericInstance* gi = cc__gen_instance_find(mangled);
    if (!gi || !gi->members || !member || !member[0]) return 0;
    return cc__gen_members_csv_has(gi->members, member);
}

const char* cc_emit_plan_generic_instance_members_csv(const char* mangled) {
    CCGenericInstance* gi = cc__gen_instance_find(mangled);
    return (gi && gi->members) ? gi->members : NULL;
}

const char* cc_emit_plan_generic_instance_family(const char* mangled) {
    CCGenericInstance* gi = cc__gen_instance_find(mangled);
    return gi ? gi->family : NULL;
}

const char* cc_emit_plan_generic_instance_def_for_symbol(const char* fn_name) {
    size_t fl;
    if (!fn_name || !fn_name[0]) return NULL;
    fl = strlen(fn_name);
    for (size_t i = 0; i < cc__gen_instance_count; i++) {
        size_t il = strlen(cc__gen_instances[i].mangled);
        /* An instance member, `<mangled>_<member>`. */
        if (fl > il + 1 && fn_name[il] == '_' &&
            strncmp(fn_name, cc__gen_instances[i].mangled, il) == 0)
            return cc__gen_instances[i].def_text;
        /* The instance itself.  A factory that names an action rather than a
         * type emits one function called exactly `<mangled>` — its return type
         * lives in the same fragment and is just as unreadable from the TU. */
        if (fl == il && strcmp(fn_name, cc__gen_instances[i].mangled) == 0)
            return cc__gen_instances[i].def_text;
    }
    return NULL;
}

static void cc__gen_instances_clear(void) {
    for (size_t i = 0; i < cc__gen_instance_count; i++) {
        free(cc__gen_instances[i].family);
        free(cc__gen_instances[i].mangled);
        free(cc__gen_instances[i].members);
        free(cc__gen_instances[i].def_text);
        memset(&cc__gen_instances[i], 0, sizeof(cc__gen_instances[i]));
    }
    cc__gen_instance_count = 0;
}

/* --- snake-name family lookup (free-name constructor grid) ---
 *
 * `<snake(Family)>_<member>::[targs](args)` lowers to
 * `<Family>_<mangled targs>_<member>(args)` for any registered factory
 * family — the same grid as `vec_new::[T]` / `map_new::[K, V]`.
 * snake(Family): '_' before an uppercase that follows a lowercase, then
 * tolower (Pair -> pair, LruCache -> lru_cache). */
void cc_emit_plan_snake_name(const char* name, char* out, size_t cap) {
    size_t o = 0;
    if (!out || cap == 0) return;
    for (size_t i = 0; name && name[i]; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') {
            if (i > 0 && name[i - 1] >= 'a' && name[i - 1] <= 'z' && o + 1 < cap)
                out[o++] = '_';
            c = (char)(c - 'A' + 'a');
        }
        if (o + 1 >= cap) break;
        out[o++] = c;
    }
    out[o] = 0;
}

int cc_emit_plan_generic_factory_for_snake_call(const char* ident,
                                                char* family_out, size_t family_cap,
                                                size_t* member_off_out) {
    size_t best_snake_len = 0;
    const char* best_family = NULL;
    if (!ident || !ident[0]) return 0;
    for (size_t i = 0; i < cc__generic_count; i++) {
        char snake[160];
        size_t sl;
        if (cc__generics[i].kind != CC_GENERIC_COMPILED) continue;
        cc_emit_plan_snake_name(cc__generics[i].name, snake, sizeof(snake));
        sl = strlen(snake);
        if (sl == 0 || strncmp(ident, snake, sl) != 0) continue;
        if (ident[sl] != '_' || !ident[sl + 1]) continue;
        if (sl > best_snake_len) {
            best_snake_len = sl;
            best_family = cc__generics[i].name;
        }
    }
    if (!best_family) return 0;
    if (family_out && family_cap)
        snprintf(family_out, family_cap, "%s", best_family);
    if (member_off_out) *member_off_out = best_snake_len + 1;
    return 1;
}

int cc_emit_plan_generic_factory_names_csv(char* out, size_t cap) {
    int count = 0;
    size_t o = 0;
    if (!out || cap == 0) return 0;
    out[0] = 0;
    for (size_t i = 0; i < cc__generic_count; i++) {
        size_t nl;
        if (cc__generics[i].kind != CC_GENERIC_COMPILED) continue;
        nl = strlen(cc__generics[i].name);
        if (o + nl + 3 >= cap) break;
        if (count) { out[o++] = ','; out[o++] = ' '; }
        memcpy(out + o, cc__generics[i].name, nl);
        o += nl;
        out[o] = 0;
        count++;
    }
    return count;
}

/* Per-TU reset of the generic emit/invalid dedup tracking. */
static void cc__generic_dedup_clear(void) {
    for (size_t i = 0; i < cc__generic_emitted_count; i++) free(cc__generic_emitted[i]);
    cc__generic_emitted_count = 0;
    for (size_t i = 0; i < cc__generic_invalid_count; i++) free(cc__generic_invalid[i]);
    cc__generic_invalid_count = 0;
    cc__gen_instances_clear();
}

void cc_emit_plan_clear_generic_factory_registrations(void) {
    cc__generic_remove_kind(CC_GENERIC_COMPILED);
}

/* --- user generic factories (D6.1: compiled handlers) --- */

void cc_emit_plan_register_generic_factory(const char* name, const char* handler_name,
                                          size_t site_pos) {
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
    if (r) r->site_pos = site_pos;
    if (!r->handler_name) { cc__generic_free_entry(r); cc__generic_count--; }
}

/* Append an extension factory for `name`, creating the registry entry if the
 * base hasn't registered yet (the base requirement is checked at the use site,
 * so include/registration order across files does not matter). */
void cc_emit_plan_register_generic_factory_extend(const char* name, const char* handler_name,
                                                 size_t site_pos) {
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
    if (r) r->site_pos = site_pos;
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

/* The slice ABI mirror lives in factory_abi.h (shared with the loader,
 * which verifies it against the comptime side's sizeof(CCSlice) probe
 * before any factory runs).  It must stay layout-identical to CCSlice
 * ({ptr,len,id}): the stale trailing `alen` made type_args.items[i]
 * for i>=1 read the previous slot's padding as the next slice's ptr. */
static CCFactorySlice cc__factory_slice_cstr(const char* s) {
    CCFactorySlice sl = {0};
    if (s) { sl.ptr = (void*)s; sl.len = strlen(s); }
    return sl;
}

/* Run one factory fn into its own scratch arena and append the returned
 * fragment to `def` via CCString (newline-separated).  `require_nonempty`
 * distinguishes the base (must define the type) from extensions (empty opts
 * out). */
static int cc__invoke_one_factory(const void* fn, const char* name, const char* mangled,
                                  CCFactorySliceArray args, CCString* def, CCArena* out_ar,
                                  int require_nonempty, const char* who) {
    CCGenericFactoryFn call = (CCGenericFactoryFn)(uintptr_t)fn;
    CCFactorySlice result;
    if (!fn || !def || !out_ar) return 0;
    /* Heap-rooted scratch: large modules (py_module with hundreds of methods)
     * exceed a fixed stack root; overflow slabs grow as needed. */
    {
        CCArena factory_arena = cc_arena_heap(CC_EMIT_TPL_BUF_SIZE);
        if (!factory_arena.base) {
            fprintf(stderr,
                    "error: compiled generic factory '%s' scratch arena OOM\n",
                    who);
            return 0;
        }
        result = call(cc__factory_slice_cstr(name),
                      cc__factory_slice_cstr(mangled),
                      args,
                      &factory_arena);
        if (!result.ptr || result.len == 0) {
            cc_arena_free(&factory_arena);
            return require_nonempty ? 0 : 1;
        }
        if (result.len > (size_t)UINT32_MAX) {
            fprintf(stderr,
                    "error: compiled generic factory '%s' fragment too large\n",
                    who);
            cc_arena_free(&factory_arena);
            return 0;
        }
        if (cc_string_len(def) > 0 &&
            !cc_string_push_buffer(def, "\n", 1u, out_ar)) {
            fprintf(stderr,
                    "error: compiled generic factory '%s' output string OOM\n",
                    who);
            cc_arena_free(&factory_arena);
            return 0;
        }
        if (!cc_string_push_buffer(def, (const char*)result.ptr,
                                   (uint32_t)result.len, out_ar)) {
            fprintf(stderr,
                    "error: compiled generic factory '%s' output string OOM\n",
                    who);
            cc_arena_free(&factory_arena);
            return 0;
        }
        cc_arena_free(&factory_arena);
    }
    return 1;
}

int cc_emit_plan_invoke_generic_factory(const char* name, const char* mangled,
                                        const char type_args[8][128], int nargs,
                                        CCArena* out_ar, char** out_def) {
    CCGenericReg* r = cc__generic_find(name, CC_GENERIC_COMPILED);
    CCFactorySliceArray args = {0};
    CCFactorySlice arg_slices[8];
    CCString def = cc_string_new();
    const char* cstr;
    if (!r || !mangled || !out_ar || !out_def || nargs <= 0 || nargs > 8) return 0;
    if (!r->handler_name || !r->fn_ptr) return 0;  /* base required and compiled */
    *out_def = NULL;
    for (int i = 0; i < nargs; i++)
        arg_slices[i] = cc__factory_slice_cstr(type_args[i]);
    args.items = arg_slices;
    args.len = (size_t)nargs;
    /* A factory that raises cc_emit_error should name ITS line, not line 1:
       the body runs compiled and detached, so the host position has to be
       supplied here from what registration recorded. */
    cc_emit_plan_host_ctx_begin(r->site_pos);
    /* Base first (defines the type / `${mangled}`), then extensions in
       registration order — so extensions can reference the base's symbols. */
    if (!cc__invoke_one_factory(r->fn_ptr, name, mangled, args, &def, out_ar, 1,
                                name)) {
        cc_emit_plan_host_ctx_end();
        return 0;
    }
    for (size_t e = 0; e < r->ext_count; e++) {
        if (!cc__invoke_one_factory(r->ext_fns[e], name, mangled, args, &def,
                                    out_ar, 0, r->ext_handlers[e])) {
            cc_emit_plan_host_ctx_end();
            return 0;
        }
    }
    cc_emit_plan_host_ctx_end();
    if (cc_string_failed(&def)) return 0;
    /* Assembled factory text is host C; lower Result sugar here (not at
     * @emit literal-piece time — ${} can split mid-signature). */
    cstr = cc_string_cstr(&def, out_ar);
    if (!cstr) {
        /* Empty is only reachable if base returned empty — already failed. */
        char* empty = (char*)cc_arena_alloc(out_ar, 1, 1);
        if (!empty) return 0;
        empty[0] = '\0';
        *out_def = empty;
        return 1;
    }
    {
        size_t total = cc_string_len(&def);
        char* rw = cc_emit_rewrite_result_sugar(cstr, total);
        if (rw) {
            size_t n = strlen(rw);
            char* nb = (char*)cc_arena_alloc(out_ar, n + 1, 1);
            if (!nb) {
                fprintf(stderr,
                        "error: result-sugar rewrite of generic factory '%s' "
                        "arena OOM\n",
                        name);
                free(rw);
                return 0;
            }
            memcpy(nb, rw, n + 1);
            free(rw);
            *out_def = nb;
            return 1;
        }
    }
    *out_def = (char*)cstr;
    return 1;
}

CCGenProduceStatus cc_emit_plan_produce_generic_def(
    const char* gname, const char* mangled, const char orig_args[8][128], int nargs,
    const char* reflect_src, size_t reflect_len, const char* input_path,
    CCArena* out_ar, char** out_def, char* err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (out_def) *out_def = NULL;
    cc_emit_plan_set_reflect_source(reflect_src, reflect_len);
    if (cc_emit_plan_ensure_generic_factory(gname, input_path, err, err_cap) != 0)
        return CC_GEN_PRODUCE_ENSURE_FAILED;
    if (!cc_emit_plan_invoke_generic_factory(gname, mangled, orig_args, nargs,
                                             out_ar, out_def))
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
    p = cc_skip_ws_and_comments(src, len, p);
    if (p >= len || src[p] != '(') return 0;
    p++;
    if (!cc__emit_parse_c_string(src, len, &p, name, sizeof(name))) return 0;
    p = cc_skip_ws_and_comments(src, len, p);
    if (p >= len || src[p] != ',') return 0;
    p = cc_skip_ws_and_comments(src, len, p + 1);
    if (!cc__emit_parse_ident(src, len, &p, handler, sizeof(handler))) return 0;
    cc_emit_plan_register_generic_factory(name, handler, call_pos);
    return 1;
}

/* cc_generic_register_extend("Name", handler_fn) — append an extension factory.
 * The `CC_GENERIC_FACTORY_EXTEND(Name) { ... }` sugar lowers to it. */
static int cc__emit_try_collect_cc_generic_register_extend(const char* src, size_t len, size_t call_pos) {
    size_t p = call_pos + strlen("cc_generic_register_extend");
    char name[128];
    char handler[128];
    p = cc_skip_ws_and_comments(src, len, p);
    if (p >= len || src[p] != '(') return 0;
    p++;
    if (!cc__emit_parse_c_string(src, len, &p, name, sizeof(name))) return 0;
    p = cc_skip_ws_and_comments(src, len, p);
    if (p >= len || src[p] != ',') return 0;
    p = cc_skip_ws_and_comments(src, len, p + 1);
    if (!cc__emit_parse_ident(src, len, &p, handler, sizeof(handler))) return 0;
    cc_emit_plan_register_generic_factory_extend(name, handler, call_pos);
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
    p = cc_skip_ws_and_comments(src, len, p);
    if (p >= len || src[p] != '(') return 0;
    p = cc_skip_ws_and_comments(src, len, p + 1);
    if (!cc__emit_parse_c_string(src, len, &p, inst.a, sizeof(inst.a))) return 0;
    if (d->n_args == 2) {
        p = cc_skip_ws_and_comments(src, len, p);
        if (p >= len || src[p] != ',') return 0;
        p = cc_skip_ws_and_comments(src, len, p + 1);
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

const char* cc_emit_plan_comptime_fragment_text(size_t frag_index) {
    if (frag_index >= cc__comptime_frag_count) return NULL;
    return cc__comptime_frags[frag_index].text;
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

static char* cc__frag_text_result_sugar(char* text) {
    char* rw;
    if (!text || !text[0]) return text;
    rw = cc_emit_rewrite_result_sugar(text, strlen(text));
    if (!rw) return text;
    free(text);
    return rw;
}

static void cc__host_emit_raw_impl(int anchor, const char* ptr, size_t len,
                                   const char* origin_file, int origin_line) {
    if (!ptr || len == 0) return;
    /* Coalesce consecutive emits at the same anchor/site so multi-call loops
     * splice as one block.  Splicing N fragments at the same AFTER_PRELUDE
     * offset prepends, which reverses split constructs (`enum { N }; static
     * T table[] = {` in one @emit, entries in the next).  Provenanced
     * @emit used to skip this and keep per-fragment #line wrappers; stamp
     * inner #line on the appended chunk instead. */
    if (cc__comptime_frag_count > 0) {
        CCEmitComptimeFragment* last = &cc__comptime_frags[cc__comptime_frag_count - 1];
        if (last->text && last->anchor == (CCEmitAnchor)anchor &&
            last->site_pos == cc__host_site_pos) {
            char prefix[PATH_MAX + 64];
            size_t prefix_len = 0;
            prefix[0] = '\0';
            if (origin_file && origin_file[0]) {
                int pn = snprintf(prefix, sizeof(prefix), "#line %d \"%s\"\n",
                                  origin_line > 0 ? origin_line : 1,
                                  origin_file);
                if (pn > 0 && (size_t)pn < sizeof(prefix))
                    prefix_len = (size_t)pn;
            }
            size_t old_len = strlen(last->text);
            size_t nl = (old_len > 0 && last->text[old_len - 1] != '\n') ? 1 : 0;
            char* nv = (char*)realloc(last->text,
                                      old_len + nl + prefix_len + len + 1);
            if (nv) {
                size_t o = old_len;
                if (nl) nv[o++] = '\n';
                if (prefix_len) {
                    memcpy(nv + o, prefix, prefix_len);
                    o += prefix_len;
                }
                memcpy(nv + o, ptr, len);
                nv[o + len] = '\0';
                /* Rewrite the coalesced blob so multi-fn rname tracking sees
                 * the full fragment (idempotent on already-mangled text). */
                last->text = cc__frag_text_result_sugar(nv);
                return;
            }
        }
    }
    if (cc__comptime_frag_count >= CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS) return;
    char* dup = (char*)malloc(len + 1);
    if (!dup) return;
    memcpy(dup, ptr, len);
    dup[len] = '\0';
    dup = cc__frag_text_result_sugar(dup);
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

/* Method reflection index: scan the TU once per type, then answer
 * cc_reflect_method_* from the retained rows.  Re-running cc__ct_load_methods
 * per name/params/ret/err call is O(N²) for factories that walk every method
 * (py_module / js_module).  Open method families still require a source scan
 * to discover decls — that scan is the miss path, not every lookup. */
typedef struct CCCtMethodRegEntry {
    char* type_name;
    CCCtField* ms;
    size_t nm;
} CCCtMethodRegEntry;

static CCCtMethodRegEntry* cc__method_reg = NULL;
static size_t cc__method_reg_n = 0;
static size_t cc__method_reg_cap = 0;
static const char* cc__method_reg_src = NULL;
static size_t cc__method_reg_src_len = 0;

static void cc__rm_cache_clear(void) {
    size_t i;
    for (i = 0; i < cc__method_reg_n; i++) {
        free(cc__method_reg[i].type_name);
        cc_ct_free_fields(cc__method_reg[i].ms, cc__method_reg[i].nm);
    }
    free(cc__method_reg);
    cc__method_reg = NULL;
    cc__method_reg_n = 0;
    cc__method_reg_cap = 0;
    cc__method_reg_src = NULL;
    cc__method_reg_src_len = 0;
}

static CCCtMethodRegEntry* cc__rm_find(const char* type_name) {
    size_t i;
    if (!type_name) return NULL;
    for (i = 0; i < cc__method_reg_n; i++) {
        if (cc__method_reg[i].type_name &&
            strcmp(cc__method_reg[i].type_name, type_name) == 0)
            return &cc__method_reg[i];
    }
    return NULL;
}

/* Load-or-hit: one source scan per type for the current reflect buffer. */
static int cc__rm_methods(const char* type_name, CCCtField** out, size_t* out_n) {
    CCCtMethodRegEntry* e;
    CCCtField* ms = NULL;
    size_t nm = 0;
    if (out) *out = NULL;
    if (out_n) *out_n = 0;
    if (!type_name || !type_name[0] || !out || !out_n) return 0;
    if (cc__method_reg_src != cc__reflect_src ||
        cc__method_reg_src_len != cc__reflect_src_len) {
        cc__rm_cache_clear();
        cc__method_reg_src = cc__reflect_src;
        cc__method_reg_src_len = cc__reflect_src_len;
    }
    e = cc__rm_find(type_name);
    if (e) {
        *out = e->ms;
        *out_n = e->nm;
        return 1;
    }
    if (!cc_ct_reflect_type_methods(cc__reflect_src, cc__reflect_src_len,
                                    type_name, &ms, &nm))
        return 0;
    if (cc__method_reg_n >= cc__method_reg_cap) {
        size_t ncap = cc__method_reg_cap ? cc__method_reg_cap * 2 : 8;
        CCCtMethodRegEntry* nb = (CCCtMethodRegEntry*)realloc(
            cc__method_reg, ncap * sizeof(*nb));
        if (!nb) {
            cc_ct_free_fields(ms, nm);
            return 0;
        }
        cc__method_reg = nb;
        cc__method_reg_cap = ncap;
    }
    e = &cc__method_reg[cc__method_reg_n++];
    memset(e, 0, sizeof(*e));
    e->type_name = strdup(type_name);
    e->ms = ms;
    e->nm = nm;
    if (!e->type_name) {
        cc_ct_free_fields(ms, nm);
        cc__method_reg_n--;
        return 0;
    }
    *out = e->ms;
    *out_n = e->nm;
    return 1;
}

/* In-memory field graph from lower-then-TCC type pass (emit `__cc_rf_*`). */
typedef struct CCCtFieldRegEntry {
    char* type_name;
    char** names;
    char** types;
    int* is_as;
    int n;
} CCCtFieldRegEntry;

static CCCtFieldRegEntry* cc__field_reg = NULL;
static size_t cc__field_reg_n = 0;
static size_t cc__field_reg_cap = 0;
static char* cc__field_reg_lowered_c = NULL;
static int cc__field_reg_type_pass_skipped = 0;
static int cc__field_reg_type_pass_failed = 0;

void cc_ct_field_reg_clear(void) {
    size_t i, j;
    for (i = 0; i < cc__field_reg_n; i++) {
        CCCtFieldRegEntry* e = &cc__field_reg[i];
        for (j = 0; j < (size_t)e->n; j++) {
            free(e->names[j]);
            free(e->types[j]);
        }
        free(e->names);
        free(e->types);
        free(e->is_as);
        free(e->type_name);
    }
    free(cc__field_reg);
    cc__field_reg = NULL;
    cc__field_reg_n = 0;
    cc__field_reg_cap = 0;
    free(cc__field_reg_lowered_c);
    cc__field_reg_lowered_c = NULL;
    cc__field_reg_type_pass_skipped = 0;
    cc__field_reg_type_pass_failed = 0;
}

void cc_ct_field_reg_set_type_pass_skipped(int skipped) {
    cc__field_reg_type_pass_skipped = skipped ? 1 : 0;
}

int cc_ct_field_reg_type_pass_skipped(void) {
    return cc__field_reg_type_pass_skipped;
}

void cc_ct_field_reg_set_type_pass_failed(int failed) {
    cc__field_reg_type_pass_failed = failed ? 1 : 0;
}

int cc_ct_field_reg_type_pass_failed(void) {
    return cc__field_reg_type_pass_failed;
}

/* Empty registry after skip/fail is not "unknown type" / unsupported forms. */
static int cc__field_reg_refuse_if_skipped(const char* api) {
    if (cc__field_reg_type_pass_failed) {
        fprintf(stderr,
                "error: %s: type-pass failed (parse/emit after blanking "
                "@comptime) — refusing silent empty field registry\n",
                api ? api : "cc_reflect_field_*");
        return 1;
    }
    if (!cc__field_reg_type_pass_skipped) return 0;
    fprintf(stderr,
            "error: %s: type-pass was skipped (no type_of/cc_reflect_field_ "
            "in the harvested TU) but comptime asked the field registry — "
            "refusing silent empty lookup\n",
            api ? api : "cc_reflect_field_*");
    return 1;
}

void cc_ct_field_reg_set_lowered_c(char* owned_c) {
    /* Keep typedefs/structs only — drop functions/main from the type-pass TU. */
    char* slim = NULL;
    if (owned_c && owned_c[0])
        slim = cc_ct_extract_type_decls_prelude(owned_c, strlen(owned_c));
    free(owned_c);
    free(cc__field_reg_lowered_c);
    cc__field_reg_lowered_c = slim;
}

const char* cc_ct_field_reg_lowered_c(void) {
    return cc__field_reg_lowered_c;
}

/* Rebuild `__cc_rf_*` tables from the in-memory registry (plain C). */
static void cc__field_reg_append_rf_tables(char** out, size_t* out_len,
                                           size_t* out_cap) {
    size_t i;
    int j;
    for (i = 0; i < cc__field_reg_n; i++) {
        CCCtFieldRegEntry* e = &cc__field_reg[i];
        char hdr[256];
        if (!e->type_name || !e->type_name[0]) continue;
        /* Attr before `static`: Apple Clang 17 -Wunused-const-variable. */
        snprintf(hdr, sizeof(hdr),
                 "/* CC reflect fields:%s */\n"
                 "__attribute__((unused)) static const struct { const char* name; "
                 "const char* type; int is_as; } __cc_rf_%s[] = {\n",
                 e->type_name, e->type_name);
        cc_sb_append_cstr(out, out_len, out_cap, hdr);
        for (j = 0; j < e->n; j++) {
            char row[384];
            snprintf(row, sizeof(row), "    { \"%s\", \"%s\", %d },\n",
                     e->names[j] ? e->names[j] : "",
                     e->types[j] ? e->types[j] : "", e->is_as[j] ? 1 : 0);
            cc_sb_append_cstr(out, out_len, out_cap, row);
        }
        snprintf(hdr, sizeof(hdr),
                 "};\n__attribute__((unused)) static const int __cc_rf_%s_n = %d;\n\n",
                 e->type_name, e->n);
        cc_sb_append_cstr(out, out_len, out_cap, hdr);
    }
}

char* cc_ct_field_reg_slim_prelude(void) {
    /* TCC sessions already carry user typedefs via the emit-tpl prelude /
     * fndefs / body — re-injecting type-pass typedefs redefines structs
     * (static_map, header harvest). Only `__cc_rf_*` tables are unique. */
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    cc__field_reg_append_rf_tables(&out, &out_len, &out_cap);
    return out;
}

static CCCtFieldRegEntry* cc__field_reg_find(const char* type_name) {
    size_t i;
    if (!type_name || !type_name[0]) return NULL;
    for (i = 0; i < cc__field_reg_n; i++) {
        if (strcmp(cc__field_reg[i].type_name, type_name) == 0)
            return &cc__field_reg[i];
    }
    return NULL;
}

int cc_ct_field_reg_has(const char* type_name) {
    return cc__field_reg_find(type_name) != NULL;
}

int cc_ct_field_reg_put(const char* type_name, const char* const* names,
                        const char* const* types, const int* is_as, int n) {
    CCCtFieldRegEntry* e;
    int i;
    if (!type_name || !type_name[0] || n < 0) return -1;
    if (n > 0 && (!names || !types || !is_as)) return -1;
    e = cc__field_reg_find(type_name);
    if (e) {
        for (i = 0; i < e->n; i++) {
            free(e->names[i]);
            free(e->types[i]);
        }
        free(e->names);
        free(e->types);
        free(e->is_as);
        e->names = NULL;
        e->types = NULL;
        e->is_as = NULL;
        e->n = 0;
    } else {
        if (cc__field_reg_n >= cc__field_reg_cap) {
            size_t nc = cc__field_reg_cap ? cc__field_reg_cap * 2 : 32;
            CCCtFieldRegEntry* nb = (CCCtFieldRegEntry*)realloc(
                cc__field_reg, nc * sizeof(CCCtFieldRegEntry));
            if (!nb) return -1;
            cc__field_reg = nb;
            cc__field_reg_cap = nc;
        }
        e = &cc__field_reg[cc__field_reg_n++];
        memset(e, 0, sizeof(*e));
        e->type_name = strdup(type_name);
        if (!e->type_name) {
            cc__field_reg_n--;
            return -1;
        }
    }
    if (n == 0) return 0;
    e->names = (char**)calloc((size_t)n, sizeof(char*));
    e->types = (char**)calloc((size_t)n, sizeof(char*));
    e->is_as = (int*)calloc((size_t)n, sizeof(int));
    if (!e->names || !e->types || !e->is_as) {
        free(e->names);
        free(e->types);
        free(e->is_as);
        e->names = NULL;
        e->types = NULL;
        e->is_as = NULL;
        return -1;
    }
    for (i = 0; i < n; i++) {
        e->names[i] = strdup(names[i] ? names[i] : "");
        e->types[i] = strdup(types[i] ? types[i] : "");
        e->is_as[i] = is_as[i] ? 1 : 0;
        if (!e->names[i] || !e->types[i]) return -1;
    }
    e->n = n;
    return 0;
}

void cc_emit_plan_set_reflect_source(const char* src, size_t len) {
    cc__rm_cache_clear();
    cc__reflect_src = src;
    cc__reflect_src_len = len;
}

/* The declarations a generated fragment may depend on: the file's own type
 * definitions, plus a stand-in for every Result box collected so far.
 *
 * The boxes are stand-ins rather than the real `CC_DECL_RESULT_SPEC` expansion
 * because the consumer is a syntax check compiled without the prelude that
 * defines that macro.  The shape is the documented lowering — tag plus a union
 * of value and error — so member access through `cc_is_ok` / `cc_value` parses
 * exactly as it will in the merged TU.
 *
 * Caller frees; NULL when there is nothing to say. */
char* cc_emit_plan_reflect_type_prelude(void) {
    char* types = NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    const CCResultSpecTable* tbl = cc_result_spec_table_get_global();
    /* Prefer plain-C typedefs from the type pass over CC-source extract. */
    if (cc__field_reg_lowered_c && cc__field_reg_lowered_c[0]) {
        cc_sb_append_cstr(&out, &out_len, &out_cap, cc__field_reg_lowered_c);
    } else if (cc__reflect_src && cc__reflect_src_len > 0) {
        types = cc_ct_extract_type_decls_prelude(cc__reflect_src, cc__reflect_src_len);
        if (types && types[0]) cc_sb_append_cstr(&out, &out_len, &out_cap, types);
        free(types);
    }
    if (tbl) {
        for (size_t i = 0; i < tbl->count; i++) {
            const CCResultSpec* sp = cc_result_spec_table_get(tbl, i);
            char line[1024];
            if (!sp || !sp->concrete_name[0]) continue;
            if (strcmp(sp->ok_type, "void") == 0)
                snprintf(line, sizeof(line),
                         "typedef struct { int ok; union { %s error; } u; } %s;\n",
                         sp->err_type, sp->concrete_name);
            else
                snprintf(line, sizeof(line),
                         "typedef struct { int ok; union { %s value; %s error; } u; } %s;\n",
                         sp->ok_type, sp->err_type, sp->concrete_name);
            cc_sb_append_cstr(&out, &out_len, &out_cap, line);
        }
    }
    return out;
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
    CCCtFieldRegEntry* e = cc__field_reg_find(type_name);
    CCCtField* fields = NULL;
    size_t nf = 0;
    /* Concurrent-C reflect owns count/name/type when reflect source is set:
     * whitelist type-pass tables are incomplete (arrays, multi-declarators,
     * unmodelable forms). Do not fall back to registry after a CC refuse —
     * that would turn all-or-nothing -1 into a partial hit. */
    if (cc__reflect_src && cc__reflect_src_len > 0) {
        if (!cc_ct_reflect_struct_fields(cc__reflect_src, cc__reflect_src_len,
                                         type_name, &fields, &nf))
            return -1;
        cc_ct_free_fields(fields, nf);
        return (int)nf;
    }
    if (cc__field_reg_refuse_if_skipped("cc_reflect_field_count")) return -1;
    if (e) return e->n;
    return -1;
}

static int cc__reflect_field_member(const char* type_name, int idx, int want_type,
                                    char* buf, int buf_sz) {
    CCCtFieldRegEntry* e;
    CCCtField* fields = NULL;
    size_t nf = 0;
    int rc = -1;
    if (buf && buf_sz > 0) buf[0] = '\0';
    if (cc__reflect_src && cc__reflect_src_len > 0) {
        if (!cc_ct_reflect_struct_fields(cc__reflect_src, cc__reflect_src_len,
                                         type_name, &fields, &nf))
            return -1;
        if (idx >= 0 && (size_t)idx < nf)
            rc = cc__rfl_emit(want_type ? fields[idx].type : fields[idx].name,
                              buf, buf_sz);
        cc_ct_free_fields(fields, nf);
        return rc;
    }
    if (cc__field_reg_refuse_if_skipped(want_type ? "cc_reflect_field_type"
                                                  : "cc_reflect_field_name"))
        return -1;
    e = cc__field_reg_find(type_name);
    if (!e || idx < 0 || idx >= e->n) return -1;
    return cc__rfl_emit(want_type ? e->types[idx] : e->names[idx], buf, buf_sz);
}

int cc_reflect_field_name(const char* type_name, int idx, char* buf, int buf_sz) {
    return cc__reflect_field_member(type_name, idx, 0, buf, buf_sz);
}

int cc_reflect_field_type(const char* type_name, int idx, char* buf, int buf_sz) {
    return cc__reflect_field_member(type_name, idx, 1, buf, buf_sz);
}

int cc_reflect_field_is_as(const char* type_name, int idx) {
    CCCtFieldRegEntry* e = cc__field_reg_find(type_name);
    CCCtField* fields = NULL;
    size_t nf = 0;
    int rc = -1;
    if (e) {
        if (idx < 0 || idx >= e->n) return -1;
        return e->is_as[idx] ? 1 : 0;
    }
    if (cc__reflect_src && cc__reflect_src_len > 0) {
        if (!cc_ct_reflect_struct_fields(cc__reflect_src, cc__reflect_src_len,
                                         type_name, &fields, &nf))
            return -1;
        if (idx >= 0 && (size_t)idx < nf) rc = fields[idx].is_as ? 1 : 0;
        cc_ct_free_fields(fields, nf);
        return rc;
    }
    if (cc__field_reg_refuse_if_skipped("cc_reflect_field_is_as")) return -1;
    return -1;
}

/* --- method reflection host verbs ---
 * The value-level face of `type_of(T).methods`, so a compiled factory can
 * enumerate what the `@comptime for` surface enumerates.  Same bytes-only waist
 * as the field verbs: names and spellings copy out, nothing crosses by
 * pointer. */
int cc_reflect_method_count(const char* type_name) {
    CCCtField* ms = NULL;
    size_t nm = 0;
    if (!cc__rm_methods(type_name, &ms, &nm)) return -1;
    return (int)nm;
}

/* Which spelling to copy out of one method entry. */
enum { CC__RM_NAME = 0, CC__RM_MEMBER, CC__RM_PARAMS, CC__RM_ARGS, CC__RM_RET, CC__RM_ERR };

/* Drop the types from a parameter list, leaving `(a, b)` — the call form,
 * whose every name the generated body already declared. */
static int cc__rm_args_of(const char* params, char* buf, int buf_sz) {
    CCCtField* ps = NULL;
    size_t pn = 0, len;
    char* acc;
    int rc;
    if (!params || !*params) return cc__rfl_emit("()", buf, buf_sz);
    if (!cc_ct_reflect_param_list(params, &ps, &pn)) return -1;
    len = 3;
    for (size_t k = 0; k < pn; k++) len += strlen(ps[k].name) + 2;
    acc = (char*)malloc(len);
    if (!acc) { cc_ct_free_fields(ps, pn); return -1; }
    acc[0] = '('; acc[1] = '\0';
    for (size_t k = 0; k < pn; k++) {
        if (k) strcat(acc, ", ");
        strcat(acc, ps[k].name);
    }
    strcat(acc, ")");
    cc_ct_free_fields(ps, pn);
    rc = cc__rfl_emit(acc, buf, buf_sz);
    free(acc);
    return rc;
}

static int cc__reflect_method_member(const char* type_name, int idx, int want,
                                     char* buf, int buf_sz) {
    CCCtField* ms = NULL;
    size_t nm = 0;
    int rc = -1;
    if (buf && buf_sz > 0) buf[0] = '\0';
    if (!cc__rm_methods(type_name, &ms, &nm)) return -1;
    if (idx >= 0 && (size_t)idx < nm) {
        const CCCtField* m = &ms[idx];
        const char* bang = m->type ? strstr(m->type, "!>") : NULL;
        switch (want) {
        case CC__RM_NAME:   rc = cc__rfl_emit(m->name, buf, buf_sz); break;
        case CC__RM_MEMBER: rc = cc__rfl_emit(m->member ? m->member : m->name, buf, buf_sz); break;
        case CC__RM_PARAMS: rc = cc__rfl_emit(m->params ? m->params : "(void)", buf, buf_sz); break;
        case CC__RM_ARGS:   rc = cc__rm_args_of(m->params, buf, buf_sz); break;
        case CC__RM_RET: {
            /* The ok half: everything before `!>`, trailing blanks trimmed.
             * Slice sugar (`double[:]`) rewrites to its lowered instance
             * name — a factory splices this into host C. */
            size_t n = bang ? (size_t)(bang - m->type) : strlen(m->type ? m->type : "");
            char* ok;
            while (n > 0 && (m->type[n - 1] == ' ' || m->type[n - 1] == '\t')) n--;
            ok = (char*)malloc(n + 1);
            if (ok) {
                char* rw;
                memcpy(ok, m->type, n); ok[n] = '\0';
                rw = cc_ct_slice_sugar_rewrite(ok);
                rc = cc__rfl_emit(rw ? rw : ok, buf, buf_sz);
                free(rw);
                free(ok);
            }
            break;
        }
        case CC__RM_ERR: {
            /* Empty for an infallible method, which is how `fallible` reads. */
            const char* lp = bang ? strchr(bang, '(') : NULL;
            const char* rp = lp ? strrchr(lp, ')') : NULL;
            if (lp && rp && rp > lp + 1) {
                const char* a = lp + 1;
                size_t n;
                char* er;
                while (a < rp && (*a == ' ' || *a == '\t')) a++;
                n = (size_t)(rp - a);
                while (n > 0 && (a[n - 1] == ' ' || a[n - 1] == '\t')) n--;
                er = (char*)malloc(n + 1);
                if (er) {
                    memcpy(er, a, n); er[n] = '\0';
                    rc = cc__rfl_emit(er, buf, buf_sz);
                    free(er);
                }
            } else {
                rc = cc__rfl_emit("", buf, buf_sz);
            }
            break;
        }
        default: break;
        }
    }
    return rc;
}

/* The concrete name of the Result box for `ok !>(err)`.
 *
 * A generated forward declaration cannot use `__typeof__` — there is no
 * expression yet — so it has to spell the box.  Spelling it by hand would
 * re-derive canonicalization at a second site, which is how two spellings
 * drift apart; this returns the canonicalizer's own answer. */
int cc_result_box_name(const char* ok_type, const char* err_type,
                       char* buf, int buf_sz) {
    char mok[256], merr[256], name[512];
    if (buf && buf_sz > 0) buf[0] = '\0';
    if (!ok_type || !err_type || !*ok_type || !*err_type) return -1;
    cc_result_spec_mangle_type(ok_type, strlen(ok_type), mok, sizeof(mok));
    cc_result_spec_mangle_type(err_type, strlen(err_type), merr, sizeof(merr));
    if (!mok[0] || !merr[0]) return -1;
    cc_result_spec_format_name(mok, merr, name, sizeof(name));
    return cc__rfl_emit(name, buf, buf_sz);
}

int cc_reflect_method_name(const char* type_name, int idx, char* buf, int buf_sz) {
    return cc__reflect_method_member(type_name, idx, CC__RM_NAME, buf, buf_sz);
}
/* Parameter reflection over a parenthesized list, so a factory can walk a
 * method's parameters without CCCtField crossing the waist. */
int cc_reflect_param_count(const char* params) {
    CCCtField* ps = NULL;
    size_t pn = 0;
    if (!params || !cc_ct_reflect_param_list(params, &ps, &pn)) return -1;
    cc_ct_free_fields(ps, pn);
    return (int)pn;
}

static int cc__reflect_param_member(const char* params, int idx, int want_type,
                                    char* buf, int buf_sz) {
    CCCtField* ps = NULL;
    size_t pn = 0;
    int rc = -1;
    if (buf && buf_sz > 0) buf[0] = '\0';
    if (!params || !cc_ct_reflect_param_list(params, &ps, &pn)) return -1;
    if (idx >= 0 && (size_t)idx < pn)
        rc = cc__rfl_emit(want_type ? ps[idx].type : ps[idx].name, buf, buf_sz);
    cc_ct_free_fields(ps, pn);
    return rc;
}

int cc_reflect_param_name(const char* params, int idx, char* buf, int buf_sz) {
    return cc__reflect_param_member(params, idx, 0, buf, buf_sz);
}
int cc_reflect_param_type(const char* params, int idx, char* buf, int buf_sz) {
    return cc__reflect_param_member(params, idx, 1, buf, buf_sz);
}

int cc_reflect_param_default(const char* params, int idx, char* buf, int buf_sz) {
    return cc_ct_reflect_param_default(params, idx, buf, buf_sz);
}

int cc_reflect_params_c_abi(const char* params, char* buf, int buf_sz) {
    CCCtField* ps = NULL;
    size_t pn = 0, len;
    char* acc;
    int rc;
    if (!params || !*params) return cc__rfl_emit("()", buf, buf_sz);
    if (!cc_ct_reflect_param_list(params, &ps, &pn)) return -1;
    if (pn == 0) {
        cc_ct_free_fields(ps, pn);
        return cc__rfl_emit("(void)", buf, buf_sz);
    }
    len = 3;
    for (size_t k = 0; k < pn; k++)
        len += strlen(ps[k].type) + 1 + strlen(ps[k].name) + 2;
    acc = (char*)malloc(len);
    if (!acc) { cc_ct_free_fields(ps, pn); return -1; }
    acc[0] = '('; acc[1] = '\0';
    for (size_t k = 0; k < pn; k++) {
        if (k) strcat(acc, ", ");
        strcat(acc, ps[k].type);
        strcat(acc, " ");
        strcat(acc, ps[k].name);
    }
    strcat(acc, ")");
    cc_ct_free_fields(ps, pn);
    rc = cc__rfl_emit(acc, buf, buf_sz);
    free(acc);
    return rc;
}

int cc_reflect_method_member(const char* type_name, int idx, char* buf, int buf_sz) {
    return cc__reflect_method_member(type_name, idx, CC__RM_MEMBER, buf, buf_sz);
}
int cc_reflect_method_params(const char* type_name, int idx, char* buf, int buf_sz) {
    return cc__reflect_method_member(type_name, idx, CC__RM_PARAMS, buf, buf_sz);
}
int cc_reflect_method_args(const char* type_name, int idx, char* buf, int buf_sz) {
    return cc__reflect_method_member(type_name, idx, CC__RM_ARGS, buf, buf_sz);
}
int cc_reflect_method_ret(const char* type_name, int idx, char* buf, int buf_sz) {
    return cc__reflect_method_member(type_name, idx, CC__RM_RET, buf, buf_sz);
}
int cc_reflect_method_err(const char* type_name, int idx, char* buf, int buf_sz) {
    return cc__reflect_method_member(type_name, idx, CC__RM_ERR, buf, buf_sz);
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
        if (cc_match_ident_kw(src, body_r, j, "switch")) return 1;
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
    const char* lp = NULL;
    size_t lpl = 0;
    if (!cc__reflect_src) return 1;
    return cc_user_line_for_offset(cc__reflect_src, cc__reflect_src_len, pos, 1, &lp, &lpl);
}

/* Resolve (file,line) for a comptime diagnostic site, honoring #line/CC_LN
 * ledger entries so spliced headers blame the .cch, not the including TU. */
static void cc__diag_origin_for_pos(size_t pos, char* file_out, size_t file_cap, int* line_out) {
    const char* lp = NULL;
    size_t lpl = 0;
    int line = 1;
    if (file_out && file_cap) file_out[0] = '\0';
    if (cc__reflect_src) {
        line = cc_user_line_for_offset(cc__reflect_src, cc__reflect_src_len, pos, 1, &lp, &lpl);
        if (lp && lpl > 0 && file_out && file_cap > 1) {
            size_t n = lpl < file_cap - 1 ? lpl : file_cap - 1;
            memcpy(file_out, lp, n);
            file_out[n] = '\0';
        }
    }
    if (line_out) *line_out = line;
}

void cc_emit_error(const char* msg) {
    char file[PATH_MAX];
    int line = 1;
    const char* f;
    cc__diag_origin_for_pos(cc__host_site_pos, file, sizeof(file), &line);
    f = file[0] ? file : (cc__diag_input_path ? cc__diag_input_path : "<input>");
    fprintf(stderr, "%s:%d: error: %s\n", f, line,
            msg && msg[0] ? msg : "comptime error");
    cc__exec_failed = 1;
}

void cc_emit_warning(const char* msg) {
    char file[PATH_MAX];
    int line = 1;
    const char* f;
    cc__diag_origin_for_pos(cc__host_site_pos, file, sizeof(file), &line);
    f = file[0] ? file : (cc__diag_input_path ? cc__diag_input_path : "<input>");
    fprintf(stderr, "%s:%d: warning: %s\n", f, line,
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
    (void)len;
    if (!cc__block_needs_executor(src, body_l, body_r)) return;
    if (cc__exec_range_contains(body_l, body_r)) return;

    CCComptimeExecOpts opts = {0};
    char ofile[PATH_MAX];
    char rel[1024];
    int oline = 0;
    const char* diag_file;
    /* Honor `#line` in the prepared buffer (unit_native copies start with
     * `#line 2 "user.ccs"`).  libtcc must name that file, not the cache path. */
    cc__diag_origin_for_pos((body_l + 1 <= body_r) ? body_l + 1 : body_l,
                            ofile, sizeof(ofile), &oline);
    diag_file = ofile[0] ? ofile : (input_path ? input_path : "<input>");
    diag_file = cc_path_rel_to_repo(diag_file, rel, sizeof(rel));
    opts.input_path = diag_file;
    opts.site_pos = body_l;
    opts.site_line = oline > 0 ? oline : 0;
    char err[512];
    if (cc_comptime_exec_block_body(src + body_l + 1, body_r - body_l - 1,
                                    &opts, err, sizeof(err)) != 0) {
        /* libtcc messages (with `#line`) already look like
         * `file:line: error: ...` — pass them through.  Generic failures
         * still get a path:line wrapper at the @comptime site. */
        if (err[0] && strstr(err, "error:")) {
            fprintf(stderr, "%s\n", err);
        } else {
            int line = opts.site_line > 0 ? opts.site_line
                                          : cc__diag_line_for_pos(body_l);
            fprintf(stderr, "%s:%d: error: @comptime block execution failed: %s\n",
                    diag_file, line,
                    err[0] ? err : "unknown");
        }
        cc__exec_failed = 1;
    }
    cc__exec_range_mark(body_l, body_r);
}

/* Did a comptime body raise cc_emit_error since the last clear?  The
   generic-factory path consults this: a factory that reports a
   constraint violation (an out-of-range arg(i), say) must fail the
   build even when its fragment still parses as C. */
int cc_emit_plan_take_exec_error(void) {
    int had = cc__exec_failed;
    cc__exec_failed = 0;
    return had;
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
        size_t pos;
        size_t k;
        int in_block = 0;
        snprintf(marker, sizeof(marker), "enum{__ccs%zu=0};", site_pos);
        const char* hit = src ? strstr(src, marker) : NULL;
        /* Serdes stage1 markers use un-harvested body_l; exec fragments record
         * harvested site_pos. Fall back to the nearest `__ccs<digits>` anchor. */
        if (!hit && src) {
            size_t logic_pos =
                (site_line > 0)
                    ? cc__emit_find_logical_line(src, len, input_path, site_line,
                                                 0)
                    : 0;
            const char* p = src;
            const char* best = NULL;
            while ((p = strstr(p, "enum{__ccs")) != NULL) {
                const char* q = p + 10;
                if (*q < '0' || *q > '9') {
                    p++;
                    continue;
                }
                while (*q >= '0' && *q <= '9') q++;
                if (strncmp(q, "=0};", 4) != 0) {
                    p++;
                    continue;
                }
                if ((size_t)(p - src) >= logic_pos) {
                    best = p;
                    break;
                }
                if (!best) best = p;
                p = q;
            }
            hit = best;
        }
        if (hit) {
            pos = (size_t)(hit - src);
            while (pos > 0 && src[pos - 1] != '\n') pos--;
        } else {
            /* No marker in this buffer. Legacy keeps harvested header
             * `@comptime` markers via parse-input append; serdes emits from a
             * stage1 buffer that never saw that append, so header sites
             * (static_map in .cch) have nothing to aim at. Searching
             * `site_line` against the TU path is wrong when the line came from
             * a `#line` in a harvested header — e.g. pp_stage2.cch:41 colliding
             * with shadow_lower.ccs:41 and landing *before* umbrella includes
             * that declare PpDirSpec. Match legacy harvest-append: EOF. */
            (void)site_line;
            (void)site_pos;
            (void)input_path;
            pos = len;
        }
        /* Marker hit can still land inside a preceding block-comment lead
         * (serdes layout). Splice after the closer so wrappers are live host C,
         * not comment text. */
        if (!src || !len) return pos;
        for (k = 0; k < pos && k < len; k++) {
            if (!in_block && src[k] == '/' && k + 1 < len && src[k + 1] == '*') {
                in_block = 1;
                k++;
            } else if (in_block && src[k] == '*' && k + 1 < len &&
                       src[k + 1] == '/') {
                in_block = 0;
                k++;
            } else if (!in_block && src[k] == '/' && k + 1 < len &&
                       src[k + 1] == '/') {
                while (k < len && src[k] != '\n') k++;
            } else if (!in_block && (src[k] == '"' || src[k] == '\'')) {
                char q = src[k++];
                while (k < len && src[k] != q) {
                    if (src[k] == '\\' && k + 1 < len) k += 2;
                    else k++;
                }
            }
        }
        if (in_block) {
            while (pos + 1 < len && !(src[pos] == '*' && src[pos + 1] == '/'))
                pos++;
            if (pos + 1 < len) pos += 2;
            while (pos < len &&
                   (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\r'))
                pos++;
            if (pos < len && src[pos] == '\n') pos++;
        }
        return pos;
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
    (void)src; (void)len;
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
            oline = f->site_line > 0 ? f->site_line : 1;
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
     * (line-aligned) body buffer, since the splice loop mutates *src.
     *
     * Use the #line/CC_LN ledger — never raw physical newline counts.  The
     * buffer is inflated by spliced headers / grammar, so a phys count past
     * the insert site lands past the TU's EOF (caught by test_async_line_map). */
    char relbuf[1024];
    const char* rel_input = cc_path_rel_to_repo(input_path ? input_path : "<input>",
                                                relbuf, sizeof(relbuf));
    static int restore_lines[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS];
    static int origin_lines[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS];
    static char restore_files[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS][PATH_MAX];
    static char origin_files[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS][PATH_MAX];
    for (size_t si = 0; si < sched.n; si++) {
        const CCEmitComptimeFragment* f = &cc__comptime_frags[sched.frag_index[si]];
        const char* rp = NULL;
        size_t rpl = 0;
        int rl = cc_user_line_for_offset(*src, *len, sched.pos[si], 1, &rp, &rpl);
        restore_lines[si] = rl > 0 ? rl : 1;
        if (rp && rpl > 0 && rpl < sizeof(restore_files[si])) {
            memcpy(restore_files[si], rp, rpl);
            restore_files[si][rpl] = '\0';
        } else {
            snprintf(restore_files[si], sizeof(restore_files[si]), "%s", rel_input);
        }
        if (f->origin_file && f->origin_file[0]) {
            origin_lines[si] = f->origin_line > 0 ? f->origin_line : 1;
            snprintf(origin_files[si], sizeof(origin_files[si]), "%s", f->origin_file);
        } else {
            /* site_line was recorded via the ledger (cc__diag_line_for_pos);
             * never recount newlines to a stale site_pos across rewrites. */
            origin_lines[si] = f->site_line > 0 ? f->site_line : restore_lines[si];
            snprintf(origin_files[si], sizeof(origin_files[si]), "%s", restore_files[si]);
        }
    }
    cc_emit_plan_warn_duplicate_symbols(*src, *len, input_path);
    for (size_t oi = 0; oi < sched.n; oi++) {
        size_t si = order[oi];
        size_t frag_i = sched.frag_index[si];
        const CCEmitComptimeFragment* f = &cc__comptime_frags[frag_i];
        const char* origin_file = origin_files[si];
        /* No leading blank before `#line origin` — it would inherit the
         * previous directive and can map past the TU's EOF. */
        size_t pos = sched.pos[si];
        if (pos > 0 && pos <= *len && (*src)[pos - 1] != '\n') {
            if (cc__emit_splice_at(src, len, pos, "\n", input_path) != 0) return -1;
            pos += 1;
        }
        const char* text = f->text ? f->text : "";
        const char* extra_nl =
            (text[0] && text[strlen(text) - 1] == '\n') ? "" : "\n";
        int n = snprintf(NULL, 0,
                         "#line %d \"%s\"\n%s%s#line %d \"%s\"\n",
                         origin_lines[si], origin_file,
                         text, extra_nl,
                         restore_lines[si], restore_files[si]);
        if (n <= 0) {
            fprintf(stderr,
                    "%s: error: failed to format comptime emit splice block\n",
                    input_path ? input_path : "<input>");
            return -1;
        }
        char stack_block[CC_EMIT_SPLICE_BLOCK_MAX];
        char* block = stack_block;
        if ((size_t)n >= sizeof(stack_block)) {
            block = (char*)malloc((size_t)n + 1);
            if (!block) {
                fprintf(stderr,
                        "%s: error: out of memory formatting comptime splice "
                        "(%d bytes)\n",
                        input_path ? input_path : "<input>", n);
                return -1;
            }
        }
        (void)snprintf(block, (size_t)n + 1,
                       "#line %d \"%s\"\n%s%s#line %d \"%s\"\n",
                       origin_lines[si], origin_file,
                       text, extra_nl,
                       restore_lines[si], restore_files[si]);
        int sp = cc__emit_splice_at(src, len, pos, block, input_path);
        if (block != stack_block) free(block);
        if (sp != 0) return -1;
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

/* First identifier-boundary occurrence of `ident` at ANY scope,
 * comment/string-aware. Returns len when absent. Used to attribute a
 * spliced instance declaration to its first use site (`#line`), so an
 * error inside the expansion (e.g. an undeclared element type) points
 * at the line that named the instance. */
size_t cc_emit_plan_find_ident_any(const char* src, size_t len, const char* ident) {
    size_t ident_len = ident ? strlen(ident) : 0;
    size_t i = 0;
    CCInertScan scan;
    if (!src || !ident_len) return len;
    cc_inert_scan_init(&scan, NULL);
    while (i + ident_len <= len) {
        if (cc_inert_scan_step(&scan, src, len, &i)) continue;
        if (src[i] != ident[0]) { i++; continue; }
        if (i > 0 && cc_is_ident_char(src[i - 1])) { i++; continue; }
        if (memcmp(src + i, ident, ident_len) == 0 &&
            (i + ident_len >= len || !cc_is_ident_char(src[i + ident_len])))
            return i;
        i++;
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
            memcmp(src + si, "__CC_VEC", 8) == 0 ||
            (si + 11 <= block_end && memcmp(src + si, "ArrayMap::[", 11) == 0) ||
            (si + 6 <= block_end && memcmp(src + si, "Map::[", 6) == 0) ||
            (si + 6 <= block_end && memcmp(src + si, "Vec::[", 6) == 0)) {
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
    /* Directives are skipped one line at a time, which walks straight INTO a
     * `#ifndef X` / `#define X 1` guard and stops at the first ordinary line
     * inside it.  Inside CC's own generated `#ifndef CCResult_..._DEFINED`
     * blocks that is fatal: the guard is normally already defined by a header,
     * so the fragment is emitted, looks spliced, and is compiled away —
     * surfacing as an implicit declaration at the use site with nothing
     * pointing back here.
     *
     * Only those blocks are skipped.  A hand-written conditional is a
     * legitimate anchor: a header that declares Result boxes under guards
     * needs the fragment to land among them. */
    size_t cond_depth = 0;
    size_t gen_guard_depth = 0;
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
            size_t d = p + 1;
            while (d < line_end && (src[d] == ' ' || src[d] == '\t')) d++;
            if ((d + 2 <= line_end && memcmp(src + d, "if", 2) == 0)) {
                cond_depth++;                 /* #if / #ifdef / #ifndef */
                if (!gen_guard_depth &&
                    d + 7 <= line_end && memcmp(src + d, "ifndef", 6) == 0) {
                    size_t g = d + 6;
                    while (g < line_end && (src[g] == ' ' || src[g] == '\t')) g++;
                    if (g + 9 <= line_end && memcmp(src + g, "CCResult_", 9) == 0)
                        gen_guard_depth = cond_depth;
                }
            } else if (d + 5 <= line_end && memcmp(src + d, "endif", 5) == 0 &&
                       cond_depth > 0) {
                if (gen_guard_depth == cond_depth) gen_guard_depth = 0;
                cond_depth--;
            }
            /* A directive continued with `\` owns the lines that follow it.
             * Stopping inside a multi-line `#define` body splices the fragment
             * into a macro definition, where it is not code at all. */
            for (;;) {
                size_t e = line_end;
                while (e > line_start && (src[e - 1] == '\r' || src[e - 1] == ' ' ||
                                          src[e - 1] == '\t')) e--;
                if (e == line_start || src[e - 1] != '\\' || line_end >= len) break;
                line_start = line_end + 1;
                line_end = line_start;
                while (line_end < len && src[line_end] != '\n') line_end++;
            }
            insert_pos = (line_end < len) ? line_end + 1 : line_end;
            continue;
        }
        if (gen_guard_depth > 0) {
            /* Inside a generated Result guard: keep going, never anchor. */
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
            /* Skip whole #if/#ifdef/#ifndef … #endif regions when they do not
             * host container typedefs.  A one-line skip used to land the
             * container anchor between `#define FOO_DEFINED 1` and the body of
             * an already-satisfied `#ifndef`, so ArrayMap monomorphs for
             * builtin payloads (e.g. int64_t) nested inside a dead branch and
             * never compiled.  Skipping the whole region fixes that — but
             * include-guarded impl .cch that wraps `typedef ArrayMap::…`
             * must still be entered: otherwise the monomorph lands after
             * `#endif` and the typedef sees an unknown type. */
            size_t kw = p + 1;
            while (kw < line_end && (src[kw] == ' ' || src[kw] == '\t')) kw++;
            int is_if = 0;
            if (kw + 2 <= line_end && memcmp(src + kw, "if", 2) == 0 &&
                (kw + 2 == line_end || !cc_is_ident_char(src[kw + 2])))
                is_if = 1;
            else if (kw + 5 <= line_end && memcmp(src + kw, "ifdef", 5) == 0 &&
                     (kw + 5 == line_end || !cc_is_ident_char(src[kw + 5])))
                is_if = 1;
            else if (kw + 6 <= line_end && memcmp(src + kw, "ifndef", 6) == 0 &&
                     (kw + 6 == line_end || !cc_is_ident_char(src[kw + 6])))
                is_if = 1;
            if (is_if) {
                int depth = 1;
                int region_refs_container = 0;
                /* Factory product splices wrap the real decl in
                 * `#ifdef CC_FRAGMENT_VALIDATE` / `#else`. Product TUs never
                 * define that macro; AFTER_PRELUDE must not land in the stub
                 * arm (TRIPLE_HAS / cc_emit_format would vanish). */
                {
                    size_t op = kw;
                    if (kw + 5 <= line_end && memcmp(src + kw, "ifdef", 5) == 0)
                        op = kw + 5;
                    else if (kw + 6 <= line_end && memcmp(src + kw, "ifndef", 6) == 0)
                        op = kw + 6;
                    else
                        op = 0;
                    if (op) {
                        while (op < line_end && (src[op] == ' ' || src[op] == '\t'))
                            op++;
                        if (op + 20 <= line_end &&
                            memcmp(src + op, "CC_FRAGMENT_VALIDATE", 20) == 0 &&
                            (op + 20 == line_end || !cc_is_ident_char(src[op + 20]))) {
                            /* Insert before the stub/real-decl wrapper, not
                             * inside the VALIDATE arm. */
                            return line_start;
                        }
                    }
                }
                size_t q = (line_end < len) ? line_end + 1 : line_end;
                while (q < len && depth > 0) {
                    size_t ls = q, le = q;
                    while (le < len && src[le] != '\n') le++;
                    size_t r = ls;
                    while (r < le && (src[r] == ' ' || src[r] == '\t' || src[r] == '\r')) r++;
                    if (r < le && src[r] == '#') {
                        size_t k = r + 1;
                        while (k < le && (src[k] == ' ' || src[k] == '\t')) k++;
                        if (k + 2 <= le && memcmp(src + k, "if", 2) == 0 &&
                            (k + 2 == le || !cc_is_ident_char(src[k + 2])))
                            depth++;
                        else if (k + 5 <= le && memcmp(src + k, "ifdef", 5) == 0 &&
                                 (k + 5 == le || !cc_is_ident_char(src[k + 5])))
                            depth++;
                        else if (k + 6 <= le && memcmp(src + k, "ifndef", 6) == 0 &&
                                 (k + 6 == le || !cc_is_ident_char(src[k + 6])))
                            depth++;
                        else if (k + 5 <= le && memcmp(src + k, "endif", 5) == 0 &&
                                 (k + 5 == le || !cc_is_ident_char(src[k + 5])))
                            depth--;
                    } else if (depth == 1 && !region_refs_container && r < le &&
                               ((r + 7 <= le && memcmp(src + r, "typedef", 7) == 0 &&
                                 !cc_is_ident_char(src[r + 7])) ||
                                (r + 6 <= le && memcmp(src + r, "struct", 6) == 0 &&
                                 !cc_is_ident_char(src[r + 6])) ||
                                (r + 5 <= le && memcmp(src + r, "union", 5) == 0 &&
                                 !cc_is_ident_char(src[r + 5])) ||
                                (r + 4 <= le && memcmp(src + r, "enum", 4) == 0 &&
                                 !cc_is_ident_char(src[r + 4])))) {
                        int is_typedef_block =
                            (r + 7 <= le && memcmp(src + r, "typedef", 7) == 0 &&
                             !cc_is_ident_char(src[r + 7]));
                        size_t block_start = r;
                        size_t b = r;
                        int brace_depth = 0;
                        size_t block_end = len;
                        while (b < len) {
                            char c = src[b];
                            if (c == '{') brace_depth++;
                            else if (c == '}') { if (brace_depth > 0) brace_depth--; }
                            else if (c == ';' && brace_depth == 0) {
                                b++;
                                if (b < len && src[b] == '\n') b++;
                                block_end = b;
                                break;
                            }
                            b++;
                        }
                        if (b >= len) block_end = len;
                        if (cc__emit_plan_block_references_container(
                                src, block_start, block_end, is_typedef_block))
                            region_refs_container = 1;
                    }
                    q = (le < len) ? le + 1 : le;
                }
                if (!region_refs_container) {
                    pos = q;
                    continue;
                }
                /* Include-guard (or live conditional) hosting a container
                 * typedef: fall through to the one-line `#` skip so the
                 * normal typedef walk can place the anchor before first use. */
            }
            /* Also consume backslash-continued physical lines so a
             * multi-line `#define … _Generic( \\` (result unwrap arms)
             * cannot host the container insert mid-macro. */
            pos = (line_end < len) ? line_end + 1 : line_end;
            while (line_end > line_start && src[line_end - 1] == '\\' && pos < len) {
                line_start = pos;
                line_end = line_start;
                while (line_end < len && src[line_end] != '\n') line_end++;
                pos = (line_end < len) ? line_end + 1 : line_end;
            }
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
    out->anchor_pos = cc_emit_plan_compute_container_anchor(src, len);
    out->n_vec = graph ? cc_type_graph_vec_count(graph) : 0;
    out->n_map = graph ? cc_type_graph_map_count(graph) : 0;
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
    /* Typed slice instances auto-instantiate: any registered spec whose
     * element is not a prebaked scalar and that no hand-written
     * declaration covers gets a spliced CC_DECL_SLICE_SPEC, positioned
     * after the element's typedef like the Vec/Map monomorphs. */
    out->n_slice = cc_slice_spec_count();
    for (size_t i = 0; i < out->n_slice && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
        const char* nm = NULL;
        const char* el = NULL;
        if (cc_slice_spec_get(i, &nm, &el) != 0) continue;
        out->slice_pos[i] = len + 1;
        if (!cc_slice_spec_tu_needs_decl(src, len, nm, el)) continue;
        out->slice_emit[i] = 1;
        {
            size_t pos = cc_emit_plan_compute_before_first_use(src, len, out->anchor_pos,
                                                               el, nm);
            if (pos > out->anchor_pos) {
                out->slice_delayed[i] = 1;
                out->slice_pos[i] = pos;
            }
        }
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
 * NATIVE_DECL is an optional registry slot a library may fill with
 * `cc_emit_plan_register_container_factory`. Vec/Map/ArrayMap splice through
 * `CC_GENERIC_FACTORY` in the stdlib headers; the compiler does not seed
 * native DECL emitters. A TU that spells `Vec::[T]` without that factory
 * fails at the use site.
 * `cc_emit_plan_fprint_vec_decl` / `_map_decl` dispatch to a registered
 * NATIVE_DECL if one exists; otherwise they diagnose.
 */

void cc_emit_plan_register_container_factory(const char* kind, CCContainerDeclFactory fn) {
    CCGenericReg* r;
    if (!kind || !fn) return;
    r = cc__generic_find(kind, CC_GENERIC_NATIVE_DECL);
    if (r) { r->decl_fn = fn; return; }  /* last registration wins */
    r = cc__generic_new(kind, CC_GENERIC_NATIVE_DECL);
    if (r) r->decl_fn = fn;
}

CCContainerDeclFactory cc_emit_plan_lookup_container_factory(const char* kind) {
    CCGenericReg* r;
    if (!kind) return NULL;
    r = cc__generic_find(kind, CC_GENERIC_NATIVE_DECL);
    return r ? r->decl_fn : NULL;
}

void cc_emit_plan_fprint_vec_decl(FILE* out, const CCTypeInstantiation* inst) {
    CCContainerDeclFactory fn = cc_emit_plan_lookup_container_factory("Vec");
    if (fn) { fn(out, inst); return; }
    fprintf(stderr,
            "error: Vec instantiation requires CC_GENERIC_FACTORY(Vec); "
            "include <ccc/std/vec.cch> (no compiler-native Vec fallback)\n");
    (void)out; (void)inst;
}

void cc_emit_plan_fprint_map_decl(FILE* out, const CCTypeInstantiation* inst) {
    CCContainerDeclFactory fn = cc_emit_plan_lookup_container_factory("Map");
    if (fn) { fn(out, inst); return; }
    fprintf(stderr,
            "error: Map instantiation requires CC_GENERIC_FACTORY(Map); "
            "include <ccc/std/map_forward.cch> (no compiler-native Map fallback)\n");
    (void)out; (void)inst;
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
    char pathbuf[PATH_MAX];
    const char* lp = NULL;
    size_t lpl = 0;
    int resume_line;
    const char* file;
    if (!out || !src) return;
    /* Bound the scan to `offset`: callers may pass a non-NUL-terminated span. */
    resume_line = cc_user_line_for_offset(src, offset, offset, 1, &lp, &lpl);
    if (resume_line <= 0) resume_line = 1;
    if (lp && lpl > 0 && lpl < sizeof(pathbuf)) {
        memcpy(pathbuf, lp, lpl);
        pathbuf[lpl] = '\0';
        file = pathbuf;
    } else {
        file = cc_path_rel_to_repo(input_path ? input_path : "<string>", rel, sizeof(rel));
    }
    fprintf(out, "#line %d \"%s\"\n", resume_line, file);
}
