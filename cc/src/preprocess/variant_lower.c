/*
 * variant_lower.c — `@variant` first-class tagged unions.
 *
 * Contract: spec/draft_variants.md (v4).  Two phase-1 canonical text passes:
 *
 *  1. cc_rewrite_variant_decls_text
 *     `@variant Name { arm: Type; ... };` (file scope) lowers to the schema
 *     one-of shape (normative, §3):
 *         typedef enum { Name_a, Name_b } NameKind;
 *         typedef struct Name { NameKind kind; union { Ta a; Tb b; } u; } Name;
 *     plus, when any arm type has a registered destructor
 *     (`@comptime cc_type_register(...)` hooks, resolved through the ambient
 *     unwrap-destroy symbol table), a `Name__cc_drop(Name*)` helper that
 *     drops the ACTIVE arm.  The pass populates a per-thread registry the
 *     uses pass consults.
 *
 *  2. cc_rewrite_variant_uses_text — the trapped consumption dialect (§4-§6):
 *     - designated-init construction with tag auto-fill (decl init +
 *       compound literal), two-arms / unknown-arm / void-arm checking;
 *     - braced ASSIGNMENT `lhs = { .arm = e };` (C syntax hole) resolved by
 *       the LHS type;
 *     - bare designators in comparison/assignment position
 *       (`if (v.kind == .num)`, `NameKind k = .num;`);
 *     - subject-switch `switch (v)` / `switch (p)` → switch on the tag,
 *       designator case labels `case .arm:` → tag constants, and
 *       compile-time exhaustiveness (missing arms named; `default:` opts out);
 *     - protected projection: `v.arm` / `p->arm` legal only under a
 *       dominating kind check (enclosing `case Name_arm:` / directly
 *       enclosing `if (v.kind == Name_arm)`), or with a `!>` handler /
 *       `?>` fallback (lowered to the same statement-expression shapes the
 *       Result unwrap machinery emits);
 *     - `.kind` is a READ-ONLY pseudo-member (writes are compile errors);
 *     - transition drop: whole-variant assignment drops the old active arm
 *       first when the variant is destructor-bearing;
 *     - scope-exit drop: destructor-bearing variant locals get an injected
 *       `@defer { Name__cc_drop(&v); };` (rides the existing defer pass).
 *
 * All rewrites are physical-line-neutral (replacement text re-embeds the
 * original sub-expressions verbatim and pads any removed newlines), so no
 * masked resyncs are needed and the reparse parity guard stays quiet.
 *
 * v1 scope notes (deliberate):
 *  - Subject/base resolution is a backward declaration scan for SIMPLE
 *    lvalues (ident, *ident, (ident), ident[...]); struct-field bases are
 *    not resolved (projections through them are left alone and fail loudly
 *    in the C compile).
 *  - Domination is syntactic (spec §11.5): nearest enclosing case label of
 *    a variant switch on the same root, or a directly enclosing
 *    `if (root.kind == Tag)` guard block.
 */
#include "preprocess/variant_lower.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "comptime/symbols.h"
#include "util/text.h"
#include "util/text_scan.h"
#include "visitor/pass_unwrap_destroy.h"

CC_DEFINE_SB_APPEND_FMT

/* ==================================================================== */
/* Registry                                                              */
/* ==================================================================== */

enum {
    CC_VA_MAX_VARIANTS = 64,
    CC_VA_MAX_ARMS = 32,
    CC_VA_MAX_SWITCHES = 256,
    CC_VA_MAX_LABELS = 512,
    CC_VA_MAX_GUARDS = 256
};

typedef struct {
    char name[64];
    char type[160];   /* trimmed arm type text (newlines collapsed) */
    int is_void;
    char destroy[128];          /* registered destroy callee ("" = none) */
    int destroy_takes_value;    /* arm type is a pointer: pass u.arm, not &u.arm */
    int decl_line;
    /* Packed-layout facts (spec §11).  Resolved only for @variant(packed). */
    int sized;                  /* size/align known at compile time */
    unsigned size, align;
    int has_niche;              /* this arm's type donates a niche */
    unsigned niche_off, niche_width;
    unsigned long long niche_sentinel;
} CCVaArm;

typedef struct {
    char name[96];
    char kindname[104];
    CCVaArm arms[CC_VA_MAX_ARMS];
    int narms;
    int has_drop;
    /* Packing plan (spec §11).  is_packed=1 for @variant(packed); when the
     * plan proves a lossless niche, donor_arm is the niche-donating arm and
     * (niche_off, niche_width, niche_sentinel) is the discriminant location:
     * niche == sentinel decodes to the (single) non-donor arm, else donor. */
    int is_packed;
    unsigned packed_size;
    unsigned packed_align;      /* max arm alignment: the byte block is aligned
                                 * to it so offset-0 typed lvalue access
                                 * (&v->arm, spec §11) is well-aligned */
    int donor_arm;
    unsigned niche_off, niche_width;
    unsigned long long niche_sentinel;
    /* Schema `one of`: same kind+u layout and projection rules as @variant.
     * Non-arm members (product-level binds such as a field discriminant) pass
     * through to C instead of being diagnosed as unknown arms. */
    int is_schema;
} CCVaDef;

static _Thread_local CCVaDef g_va[CC_VA_MAX_VARIANTS];
static _Thread_local int g_va_n = 0;
static _Thread_local int g_va_tmp_id = 0;

/* Queued by grammar emit; committed at the end of the variant decls pass so
 * a decls-pass registry reset cannot wipe schema registrations. */
static _Thread_local CCVaDef g_schema_pending[CC_VA_MAX_VARIANTS];
static _Thread_local int g_schema_pending_n = 0;

size_t cc_variant_registry_count(void) { return (size_t)g_va_n; }

static int cc__va_find(const char* s, size_t len); /* fwd: used by schema commit */

void cc_variant_schema_pending_clear(void) { g_schema_pending_n = 0; }

int cc_variant_schema_pending_count(void) { return g_schema_pending_n; }

const char* cc_variant_schema_pending_name(int i) {
    if (i < 0 || i >= g_schema_pending_n) return "";
    return g_schema_pending[i].name;
}

int cc_variant_schema_pending_narms(int i) {
    if (i < 0 || i >= g_schema_pending_n) return 0;
    return g_schema_pending[i].narms;
}

const char* cc_variant_schema_pending_arm(int i, int a) {
    if (i < 0 || i >= g_schema_pending_n || a < 0 || a >= g_schema_pending[i].narms)
        return "";
    return g_schema_pending[i].arms[a].name;
}

int cc_variant_schema_pending_arm_is_void(int i, int a) {
    if (i < 0 || i >= g_schema_pending_n || a < 0 || a >= g_schema_pending[i].narms)
        return 0;
    return g_schema_pending[i].arms[a].is_void;
}

int cc_variant_schema_pending_add(const char* name, int narms,
                                  const char* const* arm_names,
                                  const int* is_void) {
    if (!name || !name[0] || narms <= 0 || narms > CC_VA_MAX_ARMS || !arm_names)
        return -1;
    if (g_schema_pending_n >= CC_VA_MAX_VARIANTS) return -1;
    for (int i = 0; i < g_schema_pending_n; i++) {
        if (strcmp(g_schema_pending[i].name, name) == 0) return 0; /* idempotent */
    }
    CCVaDef* d = &g_schema_pending[g_schema_pending_n];
    memset(d, 0, sizeof(*d));
    snprintf(d->name, sizeof(d->name), "%s", name);
    snprintf(d->kindname, sizeof(d->kindname), "%sKind", name);
    d->narms = narms;
    d->is_schema = 1;
    for (int a = 0; a < narms; a++) {
        if (!arm_names[a] || !arm_names[a][0]) return -1;
        snprintf(d->arms[a].name, sizeof(d->arms[a].name), "%s", arm_names[a]);
        d->arms[a].is_void = is_void ? (is_void[a] != 0) : 0;
        /* Payload type text is unused for schema arms: construction pastes
         * the designated value into `.u.arm`, and C typechecks the struct. */
        snprintf(d->arms[a].type, sizeof(d->arms[a].type), "/*schema*/");
    }
    g_schema_pending_n++;
    return 0;
}

static void cc__va_commit_schema_pending(void) {
    for (int i = 0; i < g_schema_pending_n; i++) {
        const CCVaDef* src = &g_schema_pending[i];
        if (cc__va_find(src->name, strlen(src->name)) >= 0) continue;
        if (g_va_n >= CC_VA_MAX_VARIANTS) break;
        g_va[g_va_n++] = *src;
    }
    g_schema_pending_n = 0;
}

static const char* cc__va_surface(const CCVaDef* def) {
    return def && def->is_schema ? "schema union" : "variant";
}

/* Schema fill/write helpers (`Name__fill`, `Name__wchk`, …) must keep raw
 * `.kind` / `.u` stores.  Collect their function-body spans so the ban and
 * kind-write checks skip generated code. */
enum { CC_VA_MAX_CG_SPANS = 256 };
typedef struct { size_t a, b; } CCVaCgSpan;

static int cc__va_collect_codegen_spans(const char* s, size_t n,
                                        CCVaCgSpan* out, int cap) {
    static const char* const sfx[] = {
        "__fill", "__wput", "__wchk", "__wmeasure"
    };
    int cnt = 0;
    CCInertScan sc;
    cc_inert_scan_init(&sc, NULL);
    size_t i = 0;
    while (i < n && cnt < cap) {
        if (cc_inert_scan_step(&sc, s, n, &i)) continue;
        if (!cc_is_ident_start(s[i])) { i++; continue; }
        size_t a = i;
        while (i < n && cc_is_ident_char(s[i])) i++;
        size_t len = i - a;
        int hit = 0;
        for (size_t k = 0; k < sizeof(sfx) / sizeof(sfx[0]); k++) {
            size_t sl = strlen(sfx[k]);
            if (len > sl && memcmp(s + a + len - sl, sfx[k], sl) == 0) {
                hit = 1;
                break;
            }
        }
        if (!hit) continue;
        size_t p = cc_skip_ws_and_comments(s, n, i);
        if (p >= n || s[p] != '(') continue;
        size_t rp = 0;
        if (!cc_find_matching_paren(s, n, p, &rp)) continue;
        size_t lb = cc_skip_ws_and_comments(s, n, rp + 1);
        if (lb >= n || s[lb] != '{') continue;
        size_t rb = 0;
        if (!cc_find_matching_brace(s, n, lb, &rb)) continue;
        out[cnt].a = lb;
        out[cnt].b = rb + 1;
        cnt++;
        i = rb + 1;
    }
    return cnt;
}

static int cc__va_in_cg(size_t pos, const CCVaCgSpan* sp, int nsp) {
    for (int i = 0; i < nsp; i++)
        if (pos >= sp[i].a && pos < sp[i].b) return 1;
    return 0;
}

/* Is variant vi a proven-packed variant (opaque byte layout, §11)? */
static int cc__va_is_packed(int vi) {
    return vi >= 0 && vi < g_va_n && g_va[vi].is_packed && g_va[vi].donor_arm >= 0;
}

static int cc__va_find(const char* s, size_t len) {
    for (int i = 0; i < g_va_n; i++) {
        if (strlen(g_va[i].name) == len && memcmp(g_va[i].name, s, len) == 0) return i;
    }
    return -1;
}

static int cc__va_find_kind(const char* s, size_t len) {
    for (int i = 0; i < g_va_n; i++) {
        if (strlen(g_va[i].kindname) == len && memcmp(g_va[i].kindname, s, len) == 0) return i;
    }
    return -1;
}

static int cc__va_arm_index(int vi, const char* s, size_t len) {
    if (vi < 0 || vi >= g_va_n) return -1;
    for (int a = 0; a < g_va[vi].narms; a++) {
        if (strlen(g_va[vi].arms[a].name) == len &&
            memcmp(g_va[vi].arms[a].name, s, len) == 0) return a;
    }
    return -1;
}

/* Does ANY variant declare an arm with this name?  Returns the number of
 * distinct variants that do; *out_vi is the last one found. */
static int cc__va_variants_with_arm(const char* s, size_t len, int* out_vi) {
    int cnt = 0;
    for (int i = 0; i < g_va_n; i++) {
        if (cc__va_arm_index(i, s, len) >= 0) {
            cnt++;
            if (out_vi) *out_vi = i;
        }
    }
    return cnt;
}

/* Comma-joined arm list for diagnostics. */
static void cc__va_arm_list(int vi, char* out, size_t out_sz) {
    size_t o = 0;
    out[0] = '\0';
    if (vi < 0 || vi >= g_va_n) return;
    for (int a = 0; a < g_va[vi].narms; a++) {
        int m = snprintf(out + o, out_sz - o, "%s%s", a ? ", " : "", g_va[vi].arms[a].name);
        if (m < 0 || (size_t)m >= out_sz - o) break;
        o += (size_t)m;
    }
}

/* Cheap did-you-mean: nearest arm by Levenshtein distance <= 2. */
static const char* cc__va_suggest_arm(int vi, const char* s, size_t len) {
    if (vi < 0 || vi >= g_va_n || len == 0 || len > 48) return NULL;
    const char* best = NULL;
    int best_d = 3;
    char cand[64];
    memcpy(cand, s, len);
    cand[len] = '\0';
    for (int a = 0; a < g_va[vi].narms; a++) {
        const char* t = g_va[vi].arms[a].name;
        size_t tl = strlen(t);
        /* Tiny DP; both strings are short. */
        int d[64][64];
        if (tl > 48) continue;
        for (size_t i = 0; i <= len; i++) d[i][0] = (int)i;
        for (size_t j = 0; j <= tl; j++) d[0][j] = (int)j;
        for (size_t i = 1; i <= len; i++) {
            for (size_t j = 1; j <= tl; j++) {
                int c = (cand[i - 1] == t[j - 1]) ? 0 : 1;
                int m = d[i - 1][j] + 1;
                if (d[i][j - 1] + 1 < m) m = d[i][j - 1] + 1;
                if (d[i - 1][j - 1] + c < m) m = d[i - 1][j - 1] + c;
                d[i][j] = m;
            }
        }
        if (d[len][tl] < best_d) { best_d = d[len][tl]; best = t; }
    }
    return best;
}

/* ==================================================================== */
/* Diagnostics                                                           */
/* ==================================================================== */

static void cc__va_diag_v(const char* src, size_t n, const char* path, size_t off,
                          const char* sev, const char* fmt, va_list ap) {
    const char* lp = NULL;
    size_t lpl = 0;
    int line = cc_user_line_for_offset(src, n, off, 1, &lp, &lpl);
    int col = 1;
    {
        size_t ls = off;
        while (ls > 0 && src[ls - 1] != '\n') ls--;
        col = (int)(off - ls) + 1;
    }
    char file[512];
    if (lp && lpl > 0 && lpl < sizeof(file)) {
        memcpy(file, lp, lpl);
        file[lpl] = '\0';
    } else {
        snprintf(file, sizeof(file), "%s", (path && path[0]) ? path : "<input>");
    }
    fprintf(stderr, "%s:%d:%d: %s: ", file, line, col, sev);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
}

static void cc__va_err(const char* src, size_t n, const char* path, size_t off,
                       const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    cc__va_diag_v(src, n, path, off, "error", fmt, ap);
    va_end(ap);
}

static void cc__va_note(const char* src, size_t n, const char* path, size_t off,
                        const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    cc__va_diag_v(src, n, path, off, "note", fmt, ap);
    va_end(ap);
}

/* ==================================================================== */
/* Edit list                                                             */
/* ==================================================================== */

typedef struct {
    size_t a, b;   /* replace src[a..b) */
    char* text;    /* owned */
} CCVaEdit;

typedef struct {
    CCVaEdit* v;
    size_t n, cap;
} CCVaEdits;

static int cc__va_edit_add(CCVaEdits* e, size_t a, size_t b, char* text_owned) {
    if (!text_owned) return -1;
    if (e->n == e->cap) {
        size_t nc = e->cap ? e->cap * 2 : 16;
        CCVaEdit* nv = (CCVaEdit*)realloc(e->v, nc * sizeof(CCVaEdit));
        if (!nv) { free(text_owned); return -1; }
        e->v = nv;
        e->cap = nc;
    }
    e->v[e->n].a = a;
    e->v[e->n].b = b;
    e->v[e->n].text = text_owned;
    e->n++;
    return 0;
}

static void cc__va_edits_free(CCVaEdits* e) {
    for (size_t i = 0; i < e->n; i++) free(e->v[i].text);
    free(e->v);
    e->v = NULL;
    e->n = e->cap = 0;
}

static int cc__va_edit_cmp(const void* pa, const void* pb) {
    const CCVaEdit* a = (const CCVaEdit*)pa;
    const CCVaEdit* b = (const CCVaEdit*)pb;
    if (a->a < b->a) return -1;
    if (a->a > b->a) return 1;
    return 0;
}

/* Apply edits (sorted; overlap is an internal error → NULL, FAIL LOUDLY). */
static char* cc__va_edits_apply(const char* src, size_t n, CCVaEdits* e) {
    char* out = NULL;
    size_t ol = 0, oc = 0, cur = 0;
    qsort(e->v, e->n, sizeof(CCVaEdit), cc__va_edit_cmp);
    for (size_t i = 0; i < e->n; i++) {
        size_t a = e->v[i].a, b = e->v[i].b;
        if (a < cur || a > n || b > n || b < a) {
            fprintf(stderr, "cc: internal error: @variant rewrite produced overlapping edits\n");
            free(out);
            return NULL;
        }
        if (a > cur) cc_sb_append(&out, &ol, &oc, src + cur, a - cur);
        cc_sb_append_cstr(&out, &ol, &oc, e->v[i].text);
        cur = b;
    }
    {
        size_t rem = (n > cur) ? (n - cur) : 0;
        if (rem > 0 && rem < ((size_t)-1) / 2) cc_sb_append(&out, &ol, &oc, src + cur, rem);
    }
    if (!out) out = strdup(src);
    return out;
}

static size_t cc__va_count_nl(const char* s, size_t a, size_t b) {
    size_t c = 0;
    for (size_t i = a; i < b; i++)
        if (s[i] == '\n') c++;
    return c;
}

/* Pad replacement with the newlines the original span had beyond what the
 * replacement carries (line-count discipline).  Replacement carrying MORE
 * newlines than the original span is an internal bug → loud NULL. */
static char* cc__va_pad_repl(const char* src, size_t a, size_t b,
                             char* repl, size_t* io_len, size_t* io_cap) {
    size_t orig_nl = cc__va_count_nl(src, a, b);
    size_t repl_nl = repl ? cc__va_count_nl(repl, 0, *io_len) : 0;
    if (repl_nl > orig_nl) {
        fprintf(stderr, "cc: internal error: @variant rewrite would grow the line count\n");
        free(repl);
        return NULL;
    }
    for (size_t i = repl_nl; i < orig_nl; i++)
        cc_sb_append(&repl, io_len, io_cap, "\n", 1);
    return repl;
}

/* ==================================================================== */
/* Small text helpers                                                    */
/* ==================================================================== */

static const char* const cc__va_c_keywords[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "inline", "int", "long", "register", "restrict", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
    "unsigned", "void", "volatile", "while", "_Alignas", "_Alignof",
    "_Atomic", "_Bool", "_Complex", "_Generic", "_Imaginary", "_Noreturn",
    "_Static_assert", "_Thread_local", NULL
};

static int cc__va_is_c_keyword(const char* s, size_t len) {
    for (int i = 0; cc__va_c_keywords[i]; i++) {
        if (strlen(cc__va_c_keywords[i]) == len &&
            memcmp(cc__va_c_keywords[i], s, len) == 0) return 1;
    }
    return 0;
}

/* Backward skip over whitespace and comments — the short gaps callers
 * cross (`= {`, `) {`, `Type name`) all admit a comment. */
static size_t cc__va_back_ws(const char* s, size_t i) {
    return cc_rskip_ws_and_comments(s, i);
}

/* Backward over a balanced (), [] group whose CLOSER is at s[i-1].
 * Returns index of the opener, or 0 on failure. */
static size_t cc__va_back_group(const char* s, size_t i, char open, char close) {
    int depth = 0;
    while (i > 0) {
        i--;
        char c = s[i];
        if (c == close) depth++;
        else if (c == open) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return 0;
}

/* Append `s[a..b)` with newlines/CR/tabs collapsed to single spaces. */
static void cc__va_append_flat(char** out, size_t* ol, size_t* oc,
                               const char* s, size_t a, size_t b) {
    for (size_t i = a; i < b; i++) {
        char c = s[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        cc_sb_append(out, ol, oc, &c, 1);
    }
}

/* Every consumer inspects this span's ends with raw byte tests (is it an
 * ident start, does it end in `kind`, is the first byte `.`), so both edges
 * must land on code or the classification silently changes. */
static void cc__va_trim(const char* s, size_t* a, size_t* b) {
    *a = cc_skip_ws_and_comments(s, *b, *a);
    *b = cc_rskip_ws_and_comments(s, *b);
    if (*b < *a) *b = *a;
}

/* Forward find of the next top-level ';' from `from` (depth-aware, inert-safe).
 * Returns 1 with *out set, 0 when the scan leaves the statement. */
static int cc__va_find_semi(const char* s, size_t n, size_t from, size_t* out) {
    int par = 0, brk = 0, br = 0;
    CCInertScan sc;
    cc_inert_scan_init(&sc, NULL);
    sc.at_line_start = 0;
    size_t i = from;
    while (i < n) {
        if (cc_inert_scan_step(&sc, s, n, &i)) continue;
        char c = s[i];
        if (c == '(') par++;
        else if (c == ')') { if (par == 0) return 0; par--; }
        else if (c == '[') brk++;
        else if (c == ']') { if (brk == 0) return 0; brk--; }
        else if (c == '{') br++;
        else if (c == '}') { if (br == 0) return 0; br--; }
        else if (c == ';' && par == 0 && brk == 0 && br == 0) {
            *out = i;
            return 1;
        }
        i++;
    }
    return 0;
}

/* Forward scan for the end of an expression operand starting at `from`
 * (used for `?>` fallbacks and designator-first comparison partners).
 * Stops at a top-level `; , ) ] } ? : && ||` or a `?>`/`!>` sigil. */
static size_t cc__va_expr_end(const char* s, size_t n, size_t from) {
    int par = 0, brk = 0, br = 0;
    CCInertScan sc;
    cc_inert_scan_init(&sc, NULL);
    sc.at_line_start = 0;
    size_t i = from;
    while (i < n) {
        if (cc_inert_scan_step(&sc, s, n, &i)) continue;
        char c = s[i];
        char c2 = (i + 1 < n) ? s[i + 1] : 0;
        if (c == '(') { par++; i++; continue; }
        if (c == '[') { brk++; i++; continue; }
        if (c == '{') { br++; i++; continue; }
        if (c == ')') { if (par == 0) return i; par--; i++; continue; }
        if (c == ']') { if (brk == 0) return i; brk--; i++; continue; }
        if (c == '}') { if (br == 0) return i; br--; i++; continue; }
        if (par || brk || br) { i++; continue; }
        if (c == ';' || c == ',' || c == '?' || c == ':') return i;
        if (c == '&' && c2 == '&') return i;
        if (c == '|' && c2 == '|') return i;
        if (c == '!' && c2 == '>') return i;
        i++;
    }
    return n;
}

/* ==================================================================== */
/* Root-identifier type resolution                                       */
/* ==================================================================== */

enum { CC_VA_FORM_VALUE = 0, CC_VA_FORM_PTR = 1, CC_VA_FORM_KIND = 2 };

/* Per-buffer, per-name occurrence lists for cc__va_resolve_root. Each call
 * used to re-scan src[0..use_pos) for `id`; redis-sized TUs hammer this.
 * Occurrence positions are buffer-absolute (independent of use_pos); the
 * declarator-shape test still uses use_pos, so a full-buffer snapshot of
 * resolve results would be incorrect. */
typedef struct {
    char* id;
    size_t idl;
    size_t* hits;
    size_t n_hits;
    size_t hits_cap;
} CCVaIdHits;

static const char* g_va_hits_s = NULL;
static size_t g_va_hits_n = 0;
static CCVaIdHits* g_va_hits = NULL;
static size_t g_va_hits_count = 0;
static size_t g_va_hits_cap = 0;

static void cc__va_hits_reset(void) {
    size_t i;
    for (i = 0; i < g_va_hits_count; i++) {
        free(g_va_hits[i].id);
        free(g_va_hits[i].hits);
        g_va_hits[i].id = NULL;
        g_va_hits[i].hits = NULL;
        g_va_hits[i].n_hits = g_va_hits[i].hits_cap = 0;
    }
    g_va_hits_count = 0;
    g_va_hits_s = NULL;
    g_va_hits_n = 0;
}

/* Grow / locate a per-name hit bucket. Returns NULL on OOM. */
static CCVaIdHits* cc__va_hits_bucket(const char* id, size_t idl) {
    size_t i;
    CCVaIdHits* slot;
    for (i = 0; i < g_va_hits_count; i++) {
        if (g_va_hits[i].idl == idl &&
            memcmp(g_va_hits[i].id, id, idl) == 0)
            return &g_va_hits[i];
    }
    if (g_va_hits_count == g_va_hits_cap) {
        size_t cap = g_va_hits_cap ? g_va_hits_cap * 2 : 64;
        CCVaIdHits* nv = (CCVaIdHits*)realloc(g_va_hits, cap * sizeof(*nv));
        if (!nv) return NULL;
        g_va_hits = nv;
        g_va_hits_cap = cap;
    }
    slot = &g_va_hits[g_va_hits_count];
    memset(slot, 0, sizeof(*slot));
    slot->id = (char*)malloc(idl + 1);
    if (!slot->id) return NULL;
    memcpy(slot->id, id, idl);
    slot->id[idl] = 0;
    slot->idl = idl;
    g_va_hits_count++;
    return slot;
}

static int cc__va_hits_push(CCVaIdHits* slot, size_t off) {
    size_t* hv;
    if (!slot) return -1;
    if (slot->n_hits == slot->hits_cap) {
        size_t cap = slot->hits_cap ? slot->hits_cap * 2 : 8;
        hv = (size_t*)realloc(slot->hits, cap * sizeof(*hv));
        if (!hv) return -1;
        slot->hits = hv;
        slot->hits_cap = cap;
    }
    slot->hits[slot->n_hits++] = off;
    return 0;
}

/* One pass over the buffer — same inert rules as cc_find_ident_top_level —
 * indexing every word-bounded ident.  Turns O(unique_ids · n) resolve work
 * into O(n) per buffer version (redis-sized TUs). */
static int cc__va_hits_index_all(const char* s, size_t n) {
    int in_lc = 0, in_bc = 0, ins = 0;
    char q = 0;
    size_t p = 0;
    while (p < n) {
        char c = s[p];
        char c2 = (p + 1 < n) ? s[p + 1] : 0;
        if (in_lc) {
            if (c == '\n') in_lc = 0;
            p++;
            continue;
        }
        if (in_bc) {
            if (c == '*' && c2 == '/') {
                in_bc = 0;
                p += 2;
            } else {
                p++;
            }
            continue;
        }
        if (ins) {
            if (c == '\\' && p + 1 < n) {
                p += 2;
                continue;
            }
            if (c == q) ins = 0;
            p++;
            continue;
        }
        if (c == '/' && c2 == '/') {
            in_lc = 1;
            p += 2;
            continue;
        }
        if (c == '/' && c2 == '*') {
            in_bc = 1;
            p += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            ins = 1;
            q = c;
            p++;
            continue;
        }
        if (cc_is_ident_start(c)) {
            size_t start = p++;
            CCVaIdHits* slot;
            while (p < n && cc_is_ident_char(s[p])) p++;
            slot = cc__va_hits_bucket(s + start, p - start);
            if (!slot || cc__va_hits_push(slot, start) != 0) return -1;
            continue;
        }
        p++;
    }
    return 0;
}

static const CCVaIdHits* cc__va_hits_for(const char* s, size_t n,
                                         const char* id, size_t idl) {
    size_t i;
    CCVaIdHits* slot;
    if (!s || !id || idl == 0) return NULL;
    if (s != g_va_hits_s || n != g_va_hits_n) {
        cc__va_hits_reset();
        g_va_hits_s = s;
        g_va_hits_n = n;
        /* Full-buffer index; on OOM leave empty so resolve falls back. */
        if (cc__va_hits_index_all(s, n) != 0) {
            cc__va_hits_reset();
            g_va_hits_s = s;
            g_va_hits_n = n;
            return NULL;
        }
    }
    for (i = 0; i < g_va_hits_count; i++) {
        if (g_va_hits[i].idl == idl &&
            memcmp(g_va_hits[i].id, id, idl) == 0)
            return &g_va_hits[i];
    }
    /* Present-but-never-seen id: empty bucket (same as a full miss scan). */
    slot = cc__va_hits_bucket(id, idl);
    return slot;
}

/* Resolve the declared type of identifier `id` by scanning declarations /
 * parameters textually in src[0..use_pos).  Recognizes
 *   Variant id | Variant* id | Variant *id | VariantKind id
 * The LAST match before the use wins.  Returns variant index or -1. */
static int cc__va_resolve_root_x(const char* s, size_t n, size_t use_pos,
                                 const char* id, size_t idl, int* out_form,
                                 int* out_other) {
    /* Nearest declarator wins.  A later non-variant `T* out` must shadow an
     * earlier schema/variant `V* out` (generated fill helpers use `out`). */
    int found = -1, form = CC_VA_FORM_VALUE;
    int other = 0;
    const CCVaIdHits* hits;
    size_t hi;
    if (use_pos > n) use_pos = n;
    hits = cc__va_hits_for(s, n, id, idl);
    if (!hits) {
        /* Allocation failure — fall back to the original scan. */
        size_t from = 0;
        while (from < use_pos) {
            size_t h = cc_find_ident_top_level(s, from, use_pos, id, idl);
            if (h >= use_pos) break;
            from = h + idl;
            {
                size_t f = h + idl;
                while (f < use_pos && isspace((unsigned char)s[f])) f++;
                if (f < use_pos && !(s[f] == ',' || s[f] == ';' || s[f] == '=' ||
                                     s[f] == ')' || s[f] == '[')) continue;
            }
            size_t k = cc__va_back_ws(s, h);
            int stars = 0;
            while (k > 0 && s[k - 1] == '*') {
                stars++;
                k--;
                k = cc__va_back_ws(s, k);
            }
            if (k == 0 || !cc_is_ident_char(s[k - 1])) continue;
            size_t te = k, ts = k;
            while (ts > 0 && cc_is_ident_char(s[ts - 1])) ts--;
            {
                size_t q = cc__va_back_ws(s, ts);
                if (q > 0 && (s[q - 1] == '.' ||
                              (s[q - 1] == '>' && q > 1 && s[q - 2] == '-'))) continue;
            }
            {
                int vi = cc__va_find(s + ts, te - ts);
                if (vi >= 0) {
                    found = vi;
                    other = 0;
                    form = stars > 0 ? CC_VA_FORM_PTR : CC_VA_FORM_VALUE;
                    continue;
                }
                if (stars == 0) {
                    vi = cc__va_find_kind(s + ts, te - ts);
                    if (vi >= 0) {
                        found = vi;
                        other = 0;
                        form = CC_VA_FORM_KIND;
                        continue;
                    }
                }
                found = -1;
                other = 1;
                form = CC_VA_FORM_VALUE;
            }
        }
        if (out_other) *out_other = other;
        if (found >= 0 && out_form) *out_form = form;
        return found;
    }
    for (hi = 0; hi < hits->n_hits; hi++) {
        size_t h = hits->hits[hi];
        if (h >= use_pos) break;
        /* Following char must look like a declarator continuation. */
        {
            size_t f = h + idl;
            while (f < use_pos && isspace((unsigned char)s[f])) f++;
            if (f < use_pos && !(s[f] == ',' || s[f] == ';' || s[f] == '=' ||
                                 s[f] == ')' || s[f] == '[')) continue;
        }
        /* Preceding: optional stars, then a type identifier. */
        size_t k = cc__va_back_ws(s, h);
        int stars = 0;
        while (k > 0 && s[k - 1] == '*') {
            stars++;
            k--;
            k = cc__va_back_ws(s, k);
        }
        if (k == 0 || !cc_is_ident_char(s[k - 1])) continue;
        size_t te = k, ts = k;
        while (ts > 0 && cc_is_ident_char(s[ts - 1])) ts--;
        /* The type token itself must not be a member access tail. */
        {
            size_t q = cc__va_back_ws(s, ts);
            if (q > 0 && (s[q - 1] == '.' ||
                          (s[q - 1] == '>' && q > 1 && s[q - 2] == '-'))) continue;
        }
        {
            int vi = cc__va_find(s + ts, te - ts);
            if (vi >= 0) {
                found = vi;
                other = 0;
                form = stars > 0 ? CC_VA_FORM_PTR : CC_VA_FORM_VALUE;
                continue;
            }
            if (stars == 0) {
                vi = cc__va_find_kind(s + ts, te - ts);
                if (vi >= 0) {
                    found = vi;
                    other = 0;
                    form = CC_VA_FORM_KIND;
                    continue;
                }
            }
            /* Nearest decl is some other type — clear any earlier variant hit. */
            found = -1;
            other = 1;
            form = CC_VA_FORM_VALUE;
        }
    }
    if (out_other) *out_other = other;
    if (found >= 0 && out_form) *out_form = form;
    return found;
}

static int cc__va_resolve_root(const char* s, size_t n, size_t use_pos,
                               const char* id, size_t idl, int* out_form) {
    return cc__va_resolve_root_x(s, n, use_pos, id, idl, out_form, NULL);
}

static int cc__va_ident_has_nonvariant_decl(const char* s, size_t n, size_t use_pos,
                                           const char* id, size_t idl) {
    int other = 0;
    (void)cc__va_resolve_root_x(s, n, use_pos, id, idl, NULL, &other);
    return other;
}

/* Extract the base expression that a member accessor at `acc` (position of
 * '.' or '-' of '->') applies to.  Fills [ba..acc) as the base span, the
 * root identifier span, the number of member hops in the chain, and whether
 * the base ultimately derefs a pointer.  Returns 1 on success. */
static int cc__va_member_base(const char* s, size_t acc,
                              size_t* out_ba, size_t* out_ra, size_t* out_rb,
                              int* out_hops) {
    size_t i = cc__va_back_ws(s, acc);
    int hops = 0;
    size_t ra = 0, rb = 0;
    if (i == 0) return 0;
    for (;;) {
        char c = s[i - 1];
        if (c == ']') {
            size_t o = cc__va_back_group(s, i, '[', ']');
            if (o == 0 && (i < 2 || s[0] != '[')) return 0;
            i = o;
            continue;
        }
        if (c == ')') {
            size_t o = cc__va_back_group(s, i, '(', ')');
            /* A call `f(...).m` is not a resolvable base; a parenthesized
             * lvalue `(v)` / `(*p)` is.  Distinguish by what precedes '('. */
            size_t q = cc__va_back_ws(s, o);
            if (q > 0 && cc_is_ident_char(s[q - 1])) return 0; /* call */
            /* Look inside the parens for `*ident` / `ident`. */
            size_t ia = o + 1, ib = i - 1;
            cc__va_trim(s, &ia, &ib);
            while (ia < ib && s[ia] == '*') { ia++; }
            while (ia < ib && isspace((unsigned char)s[ia])) ia++;
            if (ia >= ib || !cc_is_ident_start(s[ia])) return 0;
            {
                size_t e = ia;
                while (e < ib && cc_is_ident_char(s[e])) e++;
                if (e != ib) return 0;
                ra = ia;
                rb = ib;
            }
            i = o;
            break;
        }
        if (cc_is_ident_char(c)) {
            size_t e = i, a = i;
            while (a > 0 && cc_is_ident_char(s[a - 1])) a--;
            ra = a;
            rb = e;
            i = a;
            /* Chain hop behind this ident? */
            {
                size_t q = cc__va_back_ws(s, a);
                if (q > 0 && s[q - 1] == '.') {
                    /* Could be a designator dot (preceded by '{' or ','):
                     * then there is no base at all. */
                    size_t q2 = cc__va_back_ws(s, q - 1);
                    if (q2 == 0) return 0;
                    char pc = s[q2 - 1];
                    if (pc == '{' || pc == ',') return 0;
                    hops++;
                    i = q - 1;
                    continue;
                }
                if (q > 1 && s[q - 1] == '>' && s[q - 2] == '-') {
                    hops++;
                    i = q - 2;
                    continue;
                }
            }
            break;
        }
        if (c == '*') {
            /* Leading deref: `*p . m` (rare without parens) — accept. */
            i--;
            continue;
        }
        return 0;
    }
    if (rb == 0) return 0;
    if (out_ba) *out_ba = i;
    if (out_ra) *out_ra = ra;
    if (out_rb) *out_rb = rb;
    if (out_hops) *out_hops = hops;
    return 1;
}

/* ==================================================================== */
/* Pass 1: declarations                                                  */
/* ==================================================================== */

static int cc__va_def_has_arm(const CCVaDef* def, const char* s, size_t len) {
    for (int d = 0; d < def->narms; d++) {
        if (strlen(def->arms[d].name) == len &&
            memcmp(def->arms[d].name, s, len) == 0) return 1;
    }
    return 0;
}

static void cc__va_line_of(const char* s, size_t off, int* out_line) {
    int line = 1;
    for (size_t i = 0; i < off; i++)
        if (s[i] == '\n') line++;
    *out_line = line;
}

/* ==================================================================== */
/* Packing engine (spec §11): niche resolution + lossless packing proof   */
/* ==================================================================== */

/* Normalize an arm type into a lookup key: collapse whitespace, drop
 * `const`/`volatile`/`struct`/`enum`/`union` qualifier tokens.  Returns the
 * trailing-`*` count (pointer depth) via *out_ptr. */
static void cc__va_type_key(const char* type, char* key, size_t keysz, int* out_ptr) {
    char toks[8][40];
    int ntok = 0;
    int ptr = 0;
    size_t i = 0, tl = strlen(type);
    while (i < tl) {
        while (i < tl && (isspace((unsigned char)type[i]))) i++;
        if (i >= tl) break;
        if (type[i] == '*') { ptr++; i++; continue; }
        size_t a = i;
        while (i < tl && (isalnum((unsigned char)type[i]) || type[i] == '_')) i++;
        if (i == a) { i++; continue; } /* skip stray punctuation */
        size_t l = i - a;
        if ((l == 5 && memcmp(type + a, "const", 5) == 0) ||
            (l == 8 && memcmp(type + a, "volatile", 8) == 0) ||
            (l == 6 && memcmp(type + a, "struct", 6) == 0) ||
            (l == 4 && memcmp(type + a, "enum", 4) == 0) ||
            (l == 5 && memcmp(type + a, "union", 5) == 0)) continue;
        if (ntok < 8) {
            if (l >= sizeof(toks[0])) l = sizeof(toks[0]) - 1;
            memcpy(toks[ntok], type + a, l);
            toks[ntok][l] = '\0';
            ntok++;
        }
    }
    size_t o = 0;
    key[0] = '\0';
    for (int k = 0; k < ntok; k++) {
        int m = snprintf(key + o, keysz - o, "%s%s", k ? " " : "", toks[k]);
        if (m < 0 || (size_t)m >= keysz - o) break;
        o += (size_t)m;
    }
    if (out_ptr) *out_ptr = ptr;
}

/* Primitive size/align table for the host ABI the compiler is running on.
 * Pointer-width integers track sizeof(void*) so ILP32 packing proofs stay
 * lossless (spec §11); fixed-width and C floating types are absolute. */
static int cc__va_prim_size(const char* key, unsigned* sz, unsigned* al) {
    const unsigned psz = (unsigned)sizeof(void*);
    struct { const char* name; unsigned size; } tab[] = {
        { "char", 1 }, { "signed char", 1 }, { "unsigned char", 1 },
        { "int8_t", 1 }, { "uint8_t", 1 }, { "_Bool", 1 }, { "bool", 1 },
        { "short", 2 }, { "short int", 2 }, { "unsigned short", 2 },
        { "int16_t", 2 }, { "uint16_t", 2 },
        { "int", 4 }, { "signed", 4 }, { "signed int", 4 }, { "unsigned", 4 },
        { "unsigned int", 4 }, { "int32_t", 4 }, { "uint32_t", 4 }, { "float", 4 },
        { "long", psz }, { "unsigned long", psz }, { "long int", psz },
        { "long long", 8 }, { "unsigned long long", 8 }, { "long long int", 8 },
        { "int64_t", 8 }, { "uint64_t", 8 }, { "double", 8 },
        { "size_t", psz }, { "ssize_t", psz }, { "ptrdiff_t", psz },
        { "intptr_t", psz }, { "uintptr_t", psz },
    };
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (strcmp(tab[i].name, key) == 0) {
            *sz = tab[i].size;
            *al = tab[i].size; /* natural alignment for these scalar types */
            return 1;
        }
    }
    return 0;
}

/* Resolve an arm's compile-time layout facts (size/align + donated niche).
 * Returns 1 when the size is known, 0 when it cannot be proven. */
static int cc__va_arm_layout(CCSymbolTable* sym, CCVaArm* arm) {
    arm->sized = 0;
    arm->has_niche = 0;
    if (arm->is_void) { arm->size = 0; arm->align = 1; arm->sized = 1; return 1; }
    char key[160];
    int ptr = 0;
    cc__va_type_key(arm->type, key, sizeof(key), &ptr);
    if (ptr > 0) {
        /* Inferred niche: a raw pointer arm donates the null pattern
         * (offset 0, width = pointer size, sentinel 0). */
        const unsigned psz = (unsigned)sizeof(void*);
        arm->size = psz;
        arm->align = psz;
        arm->sized = 1;
        arm->has_niche = 1;
        arm->niche_off = 0;
        arm->niche_width = psz;
        arm->niche_sentinel = 0;
        return 1;
    }
    unsigned sz = 0, al = 0;
    if (cc__va_prim_size(key, &sz, &al)) {
        arm->size = sz;
        arm->align = al;
        arm->sized = 1;
        return 1;
    }
    if (sym) {
        unsigned nsz = 0, nal = 0, noff = 0, nw = 0;
        unsigned long long nsent = 0;
        if (cc_symbols_lookup_type_niche(sym, key, &nsz, &nal, &noff, &nw, &nsent) == 0) {
            arm->size = nsz;
            arm->align = nal ? nal : 1;
            arm->sized = 1;
            arm->has_niche = 1;
            arm->niche_off = noff;
            arm->niche_width = nw;
            arm->niche_sentinel = nsent;
            return 1;
        }
    }
    return 0;
}

/* Prove a lossless niche packing for def, or leave donor_arm = -1.
 * Prints a diagnostic and returns -1 on an unpackable declaration; returns 0
 * on success (donor_arm set) or when packing is not requested. */
static int cc__va_plan_packing(CCSymbolTable* sym, CCVaDef* def,
                               const char* src, size_t n, const char* path, size_t at) {
    def->donor_arm = -1;
    if (!def->is_packed) return 0;

    /* All arm sizes must be known at compile time. */
    unsigned maxsz = 0, maxal = 1;
    for (int a = 0; a < def->narms; a++) {
        if (!cc__va_arm_layout(sym, &def->arms[a])) {
            cc__va_err(src, n, path, at,
                       "@variant(packed) '%s': cannot compute the size of arm '%s' (type '%s') — packed layout requires every arm's size at compile time; give the type a declared niche via cc_type_register(\".niche = cc_type_niche(...)\") or drop '(packed)'",
                       def->name, def->arms[a].name, def->arms[a].type);
            return -1;
        }
        if (def->arms[a].size > maxsz) maxsz = def->arms[a].size;
        if (def->arms[a].align > maxal) maxal = def->arms[a].align;
    }
    def->packed_size = maxsz ? maxsz : 1;
    def->packed_align = maxal;

    /* A single niche sentinel distinguishes exactly two groups (donor vs the
     * one non-donor arm), so niche packing needs at most two arms. */
    if (def->narms > 2) {
        cc__va_err(src, n, path, at,
                   "@variant(packed) '%s': niche packing distinguishes only two arms (a niche donor and one other), but '%s' has %d arms — a single sentinel cannot tag them all",
                   def->name, def->name, def->narms);
        return -1;
    }
    if (def->narms == 1) {
        /* Degenerate: one arm, tag is constant.  Donor = arm 0 (no niche
         * needed to decode; kind is always arm 0). */
        def->donor_arm = 0;
        def->niche_off = 0;
        def->niche_width = 0;
        def->niche_sentinel = 0;
        return 0;
    }

    /* Two arms: pick a donor D whose niche region is free in the other arm O
     * (O's payload [0, size(O)) must not overlap [off, off+width)). */
    for (int d = 0; d < 2; d++) {
        int o = 1 - d;
        CCVaArm* D = &def->arms[d];
        CCVaArm* O = &def->arms[o];
        if (!D->has_niche) continue;
        if ((unsigned)O->size <= D->niche_off) {
            def->donor_arm = d;
            def->niche_off = D->niche_off;
            def->niche_width = D->niche_width;
            def->niche_sentinel = D->niche_sentinel;
            return 0;
        }
    }

    /* No donor: name both arms and the bytes they contend for. */
    cc__va_err(src, n, path, at,
               "@variant(packed) '%s' cannot be niche-packed: arm '%s' (%s, %u bytes) and arm '%s' (%s, %u bytes) both require the low bytes and neither donates a free niche the other can leave untouched — remove '(packed)' or give one arm's type a declared niche above its payload",
               def->name,
               def->arms[0].name, def->arms[0].type, def->arms[0].size,
               def->arms[1].name, def->arms[1].type, def->arms[1].size);
    return -1;
}

static int cc__va_is_packed_def(const CCVaDef* def) {
    return def->is_packed && def->donor_arm >= 0;
}

/* Fixed-width unsigned type spelling for a niche of `width` bytes. */
static const char* cc__va_niche_uint(unsigned width) {
    switch (width) {
        case 1: return "uint8_t";
        case 2: return "uint16_t";
        case 4: return "uint32_t";
        default: return "uint64_t";
    }
}

/* Emit the packed lowering (opaque byte struct + encode/decode accessors +
 * drop, spec §11).  Returns a newline-padded malloc'd replacement for
 * src[at..after+1], or NULL on failure. */
static char* cc__va_emit_packed_lowering(CCVaDef* def, const char* src,
                                         size_t at, size_t after, CCSymbolTable* sym) {
    const char* v = def->name;
    char* out = NULL;
    size_t ol = 0, oc = 0;
    int d = def->donor_arm;
    const char* nut = cc__va_niche_uint(def->niche_width);

    /* enum */
    cc_sb_append_fmt(&out, &ol, &oc, "typedef enum { ");
    for (int a = 0; a < def->narms; a++)
        cc_sb_append_fmt(&out, &ol, &oc, "%s%s_%s", a ? ", " : "", v, def->arms[a].name);
    cc_sb_append_fmt(&out, &ol, &oc, " } %sKind;", v);
    /* opaque byte struct.  The block is aligned to the widest arm so that a
     * projection can hand out a real, well-aligned pointer to the arm payload
     * stored at offset 0 (`&v->arm`, spec §11). */
    cc_sb_append_fmt(&out, &ol, &oc,
                     " typedef struct %s { _Alignas(%u) unsigned char __cc_p[%u]; } %s;",
                     v, def->packed_align ? def->packed_align : 1, def->packed_size, v);
    /* Per-arm overlay structs.  The arm payload lives at byte 0, so an overlay
     * whose sole field is the arm type reinterprets the block as that arm.  A
     * dominated projection lowers to `((Name__cc_ov_arm*)base)->arm`: an LVALUE
     * at offset 0 that reads, takes its address (`&v->arm`), mutates in place
     * (`v->arm.field = x`) and — because the receiver ends in a plain member —
     * resolves under UFCS (`v->arm.method()`) exactly like the unpacked union
     * member it stands in for. */
    for (int a = 0; a < def->narms; a++) {
        if (def->arms[a].is_void) continue;
        cc_sb_append_fmt(&out, &ol, &oc,
                         " typedef struct { %s %s; } %s__cc_ov_%s;",
                         def->arms[a].type, def->arms[a].name, v, def->arms[a].name);
    }
    /* static asserts pinning size/niche assumptions */
    for (int a = 0; a < def->narms; a++) {
        if (def->arms[a].is_void) continue;
        cc_sb_append_fmt(&out, &ol, &oc,
                         " _Static_assert(sizeof(%s) <= %u, \"packed variant '%s' arm '%s' does not fit its packed footprint\");",
                         def->arms[a].type, def->packed_size, v, def->arms[a].name);
    }
    if (def->narms == 2 && def->arms[d].has_niche) {
        cc_sb_append_fmt(&out, &ol, &oc,
                         " _Static_assert(%u + %u <= %u, \"packed variant '%s' niche region overflows its footprint\");",
                         def->niche_off, def->niche_width, def->packed_size, v);
    }
    /* decode: kind */
    cc_sb_append_fmt(&out, &ol, &oc,
                     " static inline %sKind %s__cc_kind(const %s* __v) {", v, v, v);
    if (def->narms == 1) {
        cc_sb_append_fmt(&out, &ol, &oc, " (void)__v; return %s_%s; }", v, def->arms[0].name);
    } else {
        int o = 1 - d;
        cc_sb_append_fmt(&out, &ol, &oc,
                         " %s __n; cc__bytes_copy(&__n, __v->__cc_p + %u, %u);"
                         " return __n == (%s)%lluULL ? %s_%s : %s_%s; }",
                         nut, def->niche_off, def->niche_width,
                         nut, def->niche_sentinel,
                         v, def->arms[o].name, v, def->arms[d].name);
    }
    /* per-arm getters (payload arms) + setters */
    for (int a = 0; a < def->narms; a++) {
        CCVaArm* arm = &def->arms[a];
        if (!arm->is_void) {
            cc_sb_append_fmt(&out, &ol, &oc,
                             " static inline %s %s__cc_get_%s(const %s* __v) {"
                             " %s __x; cc__bytes_copy(&__x, __v->__cc_p, sizeof(%s)); return __x; }",
                             arm->type, v, arm->name, v, arm->type, arm->type);
        }
        /* setter */
        if (arm->is_void) {
            cc_sb_append_fmt(&out, &ol, &oc,
                             " static inline %s %s__cc_set_%s(void) {"
                             " %s __r; cc__bytes_zero(__r.__cc_p, sizeof(%s));",
                             v, v, arm->name, v, v);
        } else {
            cc_sb_append_fmt(&out, &ol, &oc,
                             " static inline %s %s__cc_set_%s(%s __x) {"
                             " %s __r; cc__bytes_zero(__r.__cc_p, sizeof(%s));"
                             " cc__bytes_copy(__r.__cc_p, &__x, sizeof(%s));",
                             v, v, arm->name, arm->type, v, v, arm->type);
        }
        if (a != d && def->narms == 2) {
            /* non-donor arm stamps the niche sentinel */
            cc_sb_append_fmt(&out, &ol, &oc,
                             " %s __s = (%s)%lluULL; cc__bytes_copy(__r.__cc_p + %u, &__s, %u);",
                             nut, nut, def->niche_sentinel, def->niche_off, def->niche_width);
        }
        cc_sb_append_fmt(&out, &ol, &oc, " return __r; }");
    }
    /* drop */
    if (def->has_drop) {
        cc_sb_append_fmt(&out, &ol, &oc,
                         " static inline void %s__cc_drop(%s* __v) { switch (%s__cc_kind(__v)) {",
                         v, v, v);
        for (int a = 0; a < def->narms; a++) {
            CCVaArm* arm = &def->arms[a];
            if (!arm->destroy[0]) continue;
            if (arm->destroy_takes_value)
                cc_sb_append_fmt(&out, &ol, &oc,
                                 " case %s_%s: { %s __t = %s__cc_get_%s(__v); %s(__t); } break;",
                                 v, arm->name, arm->type, v, arm->name, arm->destroy);
            else
                cc_sb_append_fmt(&out, &ol, &oc,
                                 " case %s_%s: { %s __t = %s__cc_get_%s(__v); %s(&__t); } break;",
                                 v, arm->name, arm->type, v, arm->name, arm->destroy);
        }
        cc_sb_append_fmt(&out, &ol, &oc, " default: break; } }");
        if (sym) {
            char drop_name[160];
            snprintf(drop_name, sizeof(drop_name), "%s__cc_drop", v);
            (void)cc_symbols_set_type_destroy_call(sym, v, drop_name);
        }
    }
    return cc__va_pad_repl(src, at, after + 1, out, &ol, &oc);
}

char* cc_rewrite_variant_decls_text(const char* src, size_t n, const char* input_path) {
    g_va_n = 0;           /* per-TU registry: reset unconditionally */
    g_va_tmp_id = 0;
    if (!src || n == 0) {
        cc__va_commit_schema_pending();
        return NULL;
    }
    int has_variant = cc_contains_token_top_level(src, n, "@variant");
    if (!has_variant) {
        /* Schema-only TUs still need registry membership for projection. */
        cc__va_commit_schema_pending();
        return NULL;
    }

    CCVaEdits edits = {0};
    int nerr = 0;
    int depth = 0;
    CCInertScan sc;
    cc_inert_scan_init(&sc, NULL);
    CCSymbolTable* sym = cc_unwrap_destroy_get_symbols();

    size_t i = 0;
    while (i < n) {
        if (cc_inert_scan_step(&sc, src, n, &i)) continue;
        char c = src[i];
        if (c == '{') { depth++; i++; continue; }
        if (c == '}') { if (depth > 0) depth--; i++; continue; }
        if (!(c == '@' && cc_match_ident_kw(src, n, i + 1, "variant"))) { i++; continue; }

        size_t at = i;
        size_t p = cc_skip_ws_and_comments(src, n, i + 1 + 7);
        int packed = 0;
        if (p < n && src[p] == '(') {
            size_t q = cc_skip_ws_and_comments(src, n, p + 1);
            if (cc_match_ident_kw(src, n, q, "packed")) {
                packed = 1;
                q = cc_skip_ws_and_comments(src, n, q + 6);
            } else {
                cc__va_err(src, n, input_path, p,
                           "@variant: unknown qualifier in '@variant(...)' (only 'packed' is defined)");
                nerr++;
                i = p + 1;
                continue;
            }
            if (q >= n || src[q] != ')') {
                cc__va_err(src, n, input_path, q < n ? q : n - 1,
                           "@variant: expected ')' after '@variant(packed'");
                nerr++;
                i = p + 1;
                continue;
            }
            p = cc_skip_ws_and_comments(src, n, q + 1);
        }
        /* Name */
        if (p >= n || !cc_is_ident_start(src[p])) {
            cc__va_err(src, n, input_path, at,
                       "@variant: expected variant name identifier after '@variant'");
            nerr++;
            i = at + 8;
            continue;
        }
        size_t name_a = p, name_b = p;
        while (name_b < n && cc_is_ident_char(src[name_b])) name_b++;
        char vname[96];
        {
            size_t l = name_b - name_a;
            if (l >= sizeof(vname)) l = sizeof(vname) - 1;
            memcpy(vname, src + name_a, l);
            vname[l] = '\0';
        }
        if (depth != 0) {
            cc__va_err(src, n, input_path, at,
                       "@variant '%s' must be declared at file scope", vname);
            nerr++;
            i = name_b;
            continue;
        }
        if (cc__va_is_c_keyword(src + name_a, name_b - name_a)) {
            cc__va_err(src, n, input_path, name_a,
                       "@variant: variant name '%s' is a C keyword", vname);
            nerr++;
            i = name_b;
            continue;
        }
        p = cc_skip_ws_and_comments(src, n, name_b);
        if (p >= n || src[p] != '{') {
            cc__va_err(src, n, input_path, p < n ? p : at,
                       "@variant '%s': expected '{' opening the arm list", vname);
            nerr++;
            i = name_b;
            continue;
        }
        size_t body_l = p, body_r = 0;
        if (!cc_find_matching_brace(src, n, body_l, &body_r)) {
            cc__va_err(src, n, input_path, body_l,
                       "@variant '%s': unterminated arm list (no matching '}')", vname);
            nerr++;
            i = body_l + 1;
            continue;
        }
        /* Parse arms */
        if (g_va_n >= CC_VA_MAX_VARIANTS) {
            cc__va_err(src, n, input_path, at,
                       "@variant '%s': too many variants in one translation unit (max %d)",
                       vname, CC_VA_MAX_VARIANTS);
            nerr++;
            i = body_r + 1;
            continue;
        }
        CCVaDef* def = &g_va[g_va_n];
        memset(def, 0, sizeof(*def));
        def->is_packed = packed;
        def->donor_arm = -1;
        snprintf(def->name, sizeof(def->name), "%s", vname);
        snprintf(def->kindname, sizeof(def->kindname), "%sKind", vname);
        int arm_errs = 0;
        size_t q = body_l + 1;
        for (;;) {
            q = cc_skip_ws_and_comments(src, n, q);
            if (q >= body_r) break;
            if (!cc_is_ident_start(src[q])) {
                cc__va_err(src, n, input_path, q,
                           "@variant '%s': expected arm name identifier", vname);
                arm_errs++;
                break;
            }
            size_t aa = q, ab = q;
            while (ab < body_r && cc_is_ident_char(src[ab])) ab++;
            size_t colon = cc_skip_ws_and_comments(src, n, ab);
            if (colon >= body_r || src[colon] != ':') {
                cc__va_err(src, n, input_path, aa,
                           "@variant '%s': expected ':' after arm name '%.*s'",
                           vname, (int)(ab - aa), src + aa);
                arm_errs++;
                break;
            }
            size_t ty_a = cc_skip_ws_and_comments(src, n, colon + 1);
            size_t semi = ty_a;
            {
                int par = 0, brk = 0;
                while (semi < body_r) {
                    char tc = src[semi];
                    if (tc == '(') par++;
                    else if (tc == ')') { if (par) par--; }
                    else if (tc == '[') brk++;
                    else if (tc == ']') { if (brk) brk--; }
                    else if (tc == ';' && par == 0 && brk == 0) break;
                    semi++;
                }
            }
            if (semi >= body_r) {
                cc__va_err(src, n, input_path, aa,
                           "@variant '%s': expected ';' after the type of arm '%.*s'",
                           vname, (int)(ab - aa), src + aa);
                arm_errs++;
                break;
            }
            size_t ty_b = semi;
            cc__va_trim(src, &ty_a, &ty_b);
            /* Validate arm name */
            if (cc__va_is_c_keyword(src + aa, ab - aa)) {
                cc__va_err(src, n, input_path, aa,
                           "@variant '%s': arm name '%.*s' is a C keyword (arm names become enum and union member names) — pick another spelling (e.g. 'num' instead of 'int')",
                           vname, (int)(ab - aa), src + aa);
                arm_errs++;
            } else if ((ab - aa == 4 && memcmp(src + aa, "kind", 4) == 0) ||
                       (ab - aa == 1 && src[aa] == 'u')) {
                cc__va_err(src, n, input_path, aa,
                           "@variant '%s': arm name '%.*s' is reserved (it collides with the lowered '%.*s' member)",
                           vname, (int)(ab - aa), src + aa, (int)(ab - aa), src + aa);
                arm_errs++;
            } else if (cc__va_def_has_arm(def, src + aa, ab - aa)) {
                int first_line = 0;
                for (int d = 0; d < def->narms; d++) {
                    if (strlen(def->arms[d].name) == ab - aa &&
                        memcmp(def->arms[d].name, src + aa, ab - aa) == 0) {
                        first_line = def->arms[d].decl_line;
                        break;
                    }
                }
                cc__va_err(src, n, input_path, aa,
                           "@variant '%s': duplicate arm name '%.*s' (first declared at line %d)",
                           vname, (int)(ab - aa), src + aa, first_line);
                arm_errs++;
            } else if (ty_b <= ty_a) {
                cc__va_err(src, n, input_path, aa,
                           "@variant '%s': arm '%.*s' is missing a type before ';'",
                           vname, (int)(ab - aa), src + aa);
                arm_errs++;
            } else if (def->narms >= CC_VA_MAX_ARMS) {
                cc__va_err(src, n, input_path, aa,
                           "@variant '%s': too many arms (max %d)", vname, CC_VA_MAX_ARMS);
                arm_errs++;
            } else {
                CCVaArm* arm = &def->arms[def->narms];
                memset(arm, 0, sizeof(*arm));
                {
                    size_t l = ab - aa;
                    if (l >= sizeof(arm->name)) l = sizeof(arm->name) - 1;
                    memcpy(arm->name, src + aa, l);
                    arm->name[l] = '\0';
                }
                {
                    char* t = NULL;
                    size_t tl = 0, tc2 = 0;
                    cc__va_append_flat(&t, &tl, &tc2, src, ty_a, ty_b);
                    snprintf(arm->type, sizeof(arm->type), "%s", t ? t : "");
                    free(t);
                }
                cc__va_line_of(src, aa, &arm->decl_line);
                arm->is_void = (strcmp(arm->type, "void") == 0);
                /* Recursive by-value check: arm type names this variant as a
                 * bare token with no pointer/slice indirection. */
                if (!arm->is_void &&
                    cc_range_contains_token(arm->type, strlen(arm->type), vname) &&
                    !strchr(arm->type, '*') && !strstr(arm->type, "[:")) {
                    cc__va_err(src, n, input_path, ty_a,
                               "@variant '%s': arm '%s' contains '%s' by value — recursive arms must go through a pointer or slice (e.g. '%s*' or '%s[:]')",
                               vname, arm->name, vname, vname, vname);
                    arm_errs++;
                } else {
                    /* Destructor lookup via registered type hooks. */
                    if (!arm->is_void && sym) {
                        char key[160];
                        size_t o = 0;
                        for (const char* t = arm->type; *t && o + 1 < sizeof(key); t++)
                            if (!isspace((unsigned char)*t)) key[o++] = *t;
                        key[o] = '\0';
                        const char* callee = NULL;
                        if (cc_symbols_lookup_type_destroy_call(sym, key, &callee) == 0 && callee) {
                            snprintf(arm->destroy, sizeof(arm->destroy), "%s", callee);
                            arm->destroy_takes_value = (o > 0 && key[o - 1] == '*');
                            def->has_drop = 1;
                        }
                    }
                    def->narms++;
                }
            }
            q = semi + 1;
        }
        if (arm_errs) {
            nerr += arm_errs;
            i = body_r + 1;
            continue;
        }
        if (def->narms == 0) {
            cc__va_err(src, n, input_path, at, "@variant '%s': needs at least one arm", vname);
            nerr++;
            i = body_r + 1;
            continue;
        }
        size_t after = cc_skip_ws_and_comments(src, n, body_r + 1);
        if (after >= n || src[after] != ';') {
            cc__va_err(src, n, input_path, body_r,
                       "@variant '%s': expected ';' after the closing '}'", vname);
            nerr++;
            i = body_r + 1;
            continue;
        }

        /* Packed variants: prove a lossless niche packing (or refuse). */
        if (cc__va_plan_packing(sym, def, src, n, input_path, at) < 0) {
            nerr++;
            i = after + 1;
            continue;
        }

        /* Compose the lowering (single physical line + newline padding). */
        if (cc__va_is_packed_def(def)) {
            char* out = cc__va_emit_packed_lowering(def, src, at, after, sym);
            if (!out || cc__va_edit_add(&edits, at, after + 1, out) != 0) nerr++;
            g_va_n++;
            i = after + 1;
            continue;
        }
        {
            char* out = NULL;
            size_t ol = 0, oc = 0;
            cc_sb_append_fmt(&out, &ol, &oc, "typedef enum { ");
            for (int a = 0; a < def->narms; a++)
                cc_sb_append_fmt(&out, &ol, &oc, "%s%s_%s", a ? ", " : "", vname,
                                 def->arms[a].name);
            cc_sb_append_fmt(&out, &ol, &oc, " } %sKind; typedef struct %s { %sKind kind;",
                             vname, vname, vname);
            {
                int have_payload = 0;
                for (int a = 0; a < def->narms; a++)
                    if (!def->arms[a].is_void) have_payload = 1;
                if (have_payload) {
                    cc_sb_append_fmt(&out, &ol, &oc, " union {");
                    for (int a = 0; a < def->narms; a++) {
                        if (def->arms[a].is_void) continue;
                        cc_sb_append_fmt(&out, &ol, &oc, " %s %s;", def->arms[a].type,
                                         def->arms[a].name);
                    }
                    cc_sb_append_fmt(&out, &ol, &oc, " } u;");
                }
            }
            cc_sb_append_fmt(&out, &ol, &oc, " } %s;", vname);
            if (def->has_drop) {
                cc_sb_append_fmt(&out, &ol, &oc,
                                 " static inline void %s__cc_drop(%s* __cc_vp) { switch (__cc_vp->kind) {",
                                 vname, vname);
                for (int a = 0; a < def->narms; a++) {
                    if (!def->arms[a].destroy[0]) continue;
                    if (def->arms[a].destroy_takes_value)
                        cc_sb_append_fmt(&out, &ol, &oc, " case %s_%s: %s(__cc_vp->u.%s); break;",
                                         vname, def->arms[a].name, def->arms[a].destroy,
                                         def->arms[a].name);
                    else
                        cc_sb_append_fmt(&out, &ol, &oc, " case %s_%s: %s(&__cc_vp->u.%s); break;",
                                         vname, def->arms[a].name, def->arms[a].destroy,
                                         def->arms[a].name);
                }
                cc_sb_append_fmt(&out, &ol, &oc, " default: break; } }");
                /* Make the whole variant a registered-destructor type so the
                 * existing @destroy / drop machinery composes (incl. variants
                 * nested as arms of other variants). */
                if (sym) {
                    char drop_name[160];
                    snprintf(drop_name, sizeof(drop_name), "%s__cc_drop", vname);
                    (void)cc_symbols_set_type_destroy_call(sym, vname, drop_name);
                }
            }
            out = cc__va_pad_repl(src, at, after + 1, out, &ol, &oc);
            if (!out || cc__va_edit_add(&edits, at, after + 1, out) != 0) {
                if (!out) nerr++;
                else nerr++;
            }
        }
        g_va_n++;
        i = after + 1;
    }

    if (nerr) {
        cc__va_edits_free(&edits);
        cc__va_commit_schema_pending(); /* still expose schemas for uses diagnostics */
        return (char*)-1;
    }
    cc__va_commit_schema_pending();
    if (edits.n == 0) {
        cc__va_edits_free(&edits);
        return NULL;
    }
    char* out = cc__va_edits_apply(src, n, &edits);
    cc__va_edits_free(&edits);
    return out ? out : (char*)-1;
}

/* ==================================================================== */
/* Uses pass: shared switch/label scanning                               */
/* ==================================================================== */

typedef struct {
    size_t kw;              /* position of `switch` */
    size_t subj_a, subj_b;  /* inside the parens, trimmed */
    size_t body_a, body_b;  /* inside the braces: [lb+1, rb) */
    int vi;                 /* variant index, -1 when not a variant switch */
    char root[128];         /* root identifier of the subject */
    int has_designator_labels;
} CCVaSwitchInfo;

typedef struct {
    size_t pos;      /* position of `case` / `default` keyword */
    size_t lab_a, lab_b; /* label text span (case only) */
    int is_default;
    int arm;         /* arm index when the label is a known tag, else -1 */
} CCVaLabelInfo;

/* Collect every switch statement (variant or not). Returns count. */
static int cc__va_collect_switches(const char* s, size_t n, CCVaSwitchInfo* out, int max) {
    int cnt = 0;
    CCInertScan sc;
    cc_inert_scan_init(&sc, NULL);
    size_t i = 0;
    while (i < n) {
        if (cc_inert_scan_step(&sc, s, n, &i)) continue;
        if (!(s[i] == 's' && cc_match_ident_kw(s, n, i, "switch"))) { i++; continue; }
        size_t kw = i;
        size_t p = cc_skip_ws_and_comments(s, n, i + 6);
        if (p >= n || s[p] != '(') { i += 6; continue; }
        size_t rp = 0;
        if (!cc_find_matching_paren(s, n, p, &rp)) { i += 6; continue; }
        size_t lb = cc_skip_ws_and_comments(s, n, rp + 1);
        if (lb >= n || s[lb] != '{') { i = rp + 1; continue; }
        size_t rb = 0;
        if (!cc_find_matching_brace(s, n, lb, &rb)) { i = rp + 1; continue; }
        if (cnt < max) {
            CCVaSwitchInfo* w = &out[cnt];
            memset(w, 0, sizeof(*w));
            w->kw = kw;
            w->subj_a = p + 1;
            w->subj_b = rp;
            cc__va_trim(s, &w->subj_a, &w->subj_b);
            w->body_a = lb + 1;
            w->body_b = rb;
            w->vi = -1;
            cnt++;
        }
        i = lb + 1;
    }
    return cnt;
}

/* Is position p inside the body of a switch nested strictly within sw? */
static int cc__va_in_nested_switch(const CCVaSwitchInfo* all, int cnt, int self, size_t p) {
    for (int j = 0; j < cnt; j++) {
        if (j == self) continue;
        if (all[j].body_a > all[self].body_a && all[j].body_b < all[self].body_b &&
            p >= all[j].body_a && p < all[j].body_b) return 1;
    }
    return 0;
}

/* Collect case/default labels belonging to switch `self` (skips labels of
 * nested switches).  For `case`, the label span [lab_a, lab_b) runs to the
 * matching ':' (?: aware).  Returns label count. */
static int cc__va_collect_labels(const char* s, const CCVaSwitchInfo* all, int cnt, int self,
                                 CCVaLabelInfo* out, int max) {
    int lcnt = 0;
    const CCVaSwitchInfo* w = &all[self];
    CCInertScan sc;
    cc_inert_scan_init(&sc, NULL);
    sc.at_line_start = 0;
    size_t i = w->body_a;
    while (i < w->body_b) {
        if (cc_inert_scan_step(&sc, s, w->body_b, &i)) continue;
        char c = s[i];
        if (c != 'c' && c != 'd') { i++; continue; }
        if (cc_match_ident_kw(s, w->body_b, i, "default")) {
            if (!cc__va_in_nested_switch(all, cnt, self, i) && lcnt < max) {
                out[lcnt].pos = i;
                out[lcnt].lab_a = out[lcnt].lab_b = 0;
                out[lcnt].is_default = 1;
                out[lcnt].arm = -1;
                lcnt++;
            }
            i += 7;
            continue;
        }
        if (!cc_match_ident_kw(s, w->body_b, i, "case")) { i++; continue; }
        if (cc__va_in_nested_switch(all, cnt, self, i)) { i += 4; continue; }
        size_t la = cc_skip_ws_and_comments(s, w->body_b, i + 4);
        /* Find the label-terminating ':' (skip ?:, brackets, strings). */
        size_t j = la;
        int pend_q = 0, par = 0, brk = 0;
        CCInertScan sc2;
        cc_inert_scan_init(&sc2, NULL);
        sc2.at_line_start = 0;
        size_t colon = 0;
        while (j < w->body_b) {
            if (cc_inert_scan_step(&sc2, s, w->body_b, &j)) continue;
            char cj = s[j];
            if (cj == '(') par++;
            else if (cj == ')') { if (par) par--; }
            else if (cj == '[') brk++;
            else if (cj == ']') { if (brk) brk--; }
            else if (cj == '?' && !par && !brk) pend_q++;
            else if (cj == ':' && !par && !brk) {
                if (pend_q) pend_q--;
                else { colon = j; break; }
            }
            j++;
        }
        if (!colon) { i += 4; continue; }
        if (lcnt < max) {
            size_t lb2 = colon;
            size_t la2 = la;
            cc__va_trim(s, &la2, &lb2);
            out[lcnt].pos = i;
            out[lcnt].lab_a = la2;
            out[lcnt].lab_b = lb2;
            out[lcnt].is_default = 0;
            out[lcnt].arm = -1;
            lcnt++;
        }
        i = colon + 1;
    }
    return lcnt;
}

/* Classify a switch subject against the registry.  Fills w->vi / w->root and
 * returns:
 *   0 — not a variant switch (leave alone)
 *   1 — variant switch, subject already tag-valued (`x.kind`, `p->kind`,
 *       or a NameKind-typed variable): no subject rewrite needed
 *   2 — variant switch on a VALUE subject: rewrite to `(subj).kind`
 *   3 — variant switch on a POINTER subject: rewrite to `(subj)->kind`
 */
static int cc__va_classify_switch_subject(const char* s, size_t n, CCVaSwitchInfo* w) {
    size_t a = w->subj_a, b = w->subj_b;
    cc__va_trim(s, &a, &b);
    if (b <= a) return 0;
    /* Strip one layer of redundant parens. */
    while (b - a >= 2 && s[a] == '(' ) {
        size_t rp = 0;
        if (cc_find_matching_paren(s, n, a, &rp) && rp == b - 1) {
            a++;
            b--;
            cc__va_trim(s, &a, &b);
        } else break;
    }
    /* `X.kind` / `X->kind` suffix? */
    if (b - a > 5 && memcmp(s + b - 4, "kind", 4) == 0 &&
        !(b - 4 > a && cc_is_ident_char(s[b - 5]) )) {
        /* preceded by '.' or '->' */
        size_t k = b - 4;
        size_t q = cc__va_back_ws(s, k);
        if (q > a && s[q - 1] == '.') {
            size_t ba, ra, rb;
            int hops = 0;
            if (cc__va_member_base(s, q - 1, &ba, &ra, &rb, &hops) && hops == 0) {
                int form = 0;
                int vi = cc__va_resolve_root(s, n, w->kw, s + ra, rb - ra, &form);
                if (vi >= 0 && form != CC_VA_FORM_KIND) {
                    w->vi = vi;
                    snprintf(w->root, sizeof(w->root), "%.*s", (int)(rb - ra), s + ra);
                    return 1;
                }
            }
        } else if (q > a + 1 && s[q - 1] == '>' && s[q - 2] == '-') {
            size_t ba, ra, rb;
            int hops = 0;
            if (cc__va_member_base(s, q - 2, &ba, &ra, &rb, &hops) && hops == 0) {
                int form = 0;
                int vi = cc__va_resolve_root(s, n, w->kw, s + ra, rb - ra, &form);
                if (vi >= 0 && form != CC_VA_FORM_KIND) {
                    w->vi = vi;
                    snprintf(w->root, sizeof(w->root), "%.*s", (int)(rb - ra), s + ra);
                    return 1;
                }
            }
        }
    }
    /* Bare subject: ident / *ident. */
    {
        size_t p = a;
        int deref = 0;
        while (p < b && s[p] == '*') { deref++; p++; }
        while (p < b && isspace((unsigned char)s[p])) p++;
        if (p < b && cc_is_ident_start(s[p])) {
            size_t e = p;
            while (e < b && cc_is_ident_char(s[e])) e++;
            size_t e2 = e;
            while (e2 < b && isspace((unsigned char)s[e2])) e2++;
            if (e2 == b) { /* whole subject is (derefed) ident */
                int form = 0;
                int vi = cc__va_resolve_root(s, n, w->kw, s + p, e - p, &form);
                if (vi >= 0) {
                    w->vi = vi;
                    snprintf(w->root, sizeof(w->root), "%.*s", (int)(e - p), s + p);
                    if (form == CC_VA_FORM_KIND) return 1;
                    if (form == CC_VA_FORM_PTR && deref == 0) return 3;
                    return 2; /* value, or pointer already deref'd */
                }
            }
        }
    }
    return 0;
}

/* ==================================================================== */
/* Uses step: constructions                                              */
/* ==================================================================== */

/* Rewrite the designator list inside braces [lb..rb] for variant vi.
 * `prefix` text goes before the '{' in the replacement (e.g. "(Name)").
 * On error prints diagnostics and returns -1; on "leave alone" returns 0
 * with *out NULL; on success returns 0 with *out = replacement for
 * src[span_a..rb+1). */
static int cc__va_rewrite_ctor_braces(const char* s, size_t n, const char* path,
                                      int vi, size_t span_a, size_t lb, size_t rb,
                                      const char* prefix, char** out) {
    *out = NULL;
    const CCVaDef* def = &g_va[vi];
    /* Parse designator items at top level. */
    size_t items_a[8], items_b[8];
    int nitems = 0;
    {
        size_t i = lb + 1;
        size_t item_start = i;
        int par = 0, brk = 0, br = 0;
        CCInertScan sc;
        cc_inert_scan_init(&sc, NULL);
        sc.at_line_start = 0;
        while (i <= rb) {
            if (i == rb) {
                if (nitems < 8) { items_a[nitems] = item_start; items_b[nitems] = i; nitems++; }
                break;
            }
            if (cc_inert_scan_step(&sc, s, rb, &i)) continue;
            char c = s[i];
            if (c == '(') par++;
            else if (c == ')') { if (par) par--; }
            else if (c == '[') brk++;
            else if (c == ']') { if (brk) brk--; }
            else if (c == '{') br++;
            else if (c == '}') { if (br) br--; }
            else if (c == ',' && !par && !brk && !br) {
                if (nitems < 8) { items_a[nitems] = item_start; items_b[nitems] = i; nitems++; }
                item_start = i + 1;
            }
            i++;
        }
    }
    /* Classify items. */
    int arm_item = -1, arm_idx = -1;
    size_t arm_val_a = 0, arm_val_b = 0;
    size_t arm_name_pos = 0;
    for (int it = 0; it < nitems; it++) {
        size_t a = items_a[it], b = items_b[it];
        cc__va_trim(s, &a, &b);
        if (a >= b) continue; /* empty (trailing comma) */
        if (s[a] != '.') return 0; /* positional init — leave alone */
        size_t ia = a + 1, ib = ia;
        while (ib < b && cc_is_ident_char(s[ib])) ib++;
        if (ib == ia) return 0;
        /* raw interop escape: `.kind` / `.u...` designators — leave alone */
        if ((ib - ia == 4 && memcmp(s + ia, "kind", 4) == 0) ||
            (ib - ia == 1 && s[ia] == 'u')) return 0;
        int ai = cc__va_arm_index(vi, s + ia, ib - ia);
        if (ai < 0) {
            char arms[512];
            cc__va_arm_list(vi, arms, sizeof(arms));
            const char* sugg = cc__va_suggest_arm(vi, s + ia, ib - ia);
            if (sugg)
                cc__va_err(s, n, path, a,
                           "variant '%s' has no arm '.%.*s' (arms: %s) — did you mean '.%s'?",
                           def->name, (int)(ib - ia), s + ia, arms, sugg);
            else
                cc__va_err(s, n, path, a,
                           "variant '%s' has no arm '.%.*s' (arms: %s)",
                           def->name, (int)(ib - ia), s + ia, arms);
            return -1;
        }
        if (arm_item >= 0) {
            cc__va_err(s, n, path, a,
                       "variant '%s' initializer names two arms '.%s' and '.%s' — a variant holds exactly one active arm",
                       def->name, def->arms[arm_idx].name, def->arms[ai].name);
            return -1;
        }
        arm_item = it;
        arm_idx = ai;
        arm_name_pos = a;
        /* value = after '=' */
        {
            size_t eq = ib;
            while (eq < b && isspace((unsigned char)s[eq])) eq++;
            if (eq >= b || s[eq] != '=') {
                cc__va_err(s, n, path, a,
                           "variant '%s' designator '.%s' needs '= value'",
                           def->name, def->arms[ai].name);
                return -1;
            }
            arm_val_a = eq + 1;
            arm_val_b = b;
            cc__va_trim(s, &arm_val_a, &arm_val_b);
        }
    }
    if (arm_item < 0) return 0; /* no designators — leave alone */
    const CCVaArm* arm = &def->arms[arm_idx];
    char* out_sb = NULL;
    size_t ol = 0, oc = 0;
    if (arm->is_void) {
        /* value must be `{}` (or empty) */
        int ok = (arm_val_b == arm_val_a);
        if (!ok && s[arm_val_a] == '{') {
            size_t inner_a = arm_val_a + 1, inner_b = arm_val_b > arm_val_a ? arm_val_b - 1 : arm_val_a;
            if (arm_val_b - arm_val_a >= 2 && s[arm_val_b - 1] == '}') {
                cc__va_trim(s, &inner_a, &inner_b);
                ok = (inner_b <= inner_a);
            }
        }
        if (!ok) {
            cc__va_err(s, n, path, arm_name_pos,
                       "arm '%s' of variant '%s' is void — construct it with '{ .%s = {} }'",
                       arm->name, def->name, arm->name);
            return -1;
        }
        if (cc__va_is_packed_def(def))
            cc_sb_append_fmt(&out_sb, &ol, &oc, "%s%s__cc_set_%s()",
                             prefix ? prefix : "", def->name, arm->name);
        else
            cc_sb_append_fmt(&out_sb, &ol, &oc, "%s{ .kind = %s_%s }",
                             prefix ? prefix : "", def->name, arm->name);
    } else if (cc__va_is_packed_def(def)) {
        cc_sb_append_fmt(&out_sb, &ol, &oc, "%s%s__cc_set_%s(",
                         prefix ? prefix : "", def->name, arm->name);
        cc_sb_append(&out_sb, &ol, &oc, s + arm_val_a, arm_val_b - arm_val_a);
        cc_sb_append_cstr(&out_sb, &ol, &oc, ")");
    } else {
        cc_sb_append_fmt(&out_sb, &ol, &oc, "%s{ .kind = %s_%s, .u.%s = ",
                         prefix ? prefix : "", def->name, arm->name, arm->name);
        cc_sb_append(&out_sb, &ol, &oc, s + arm_val_a, arm_val_b - arm_val_a);
        cc_sb_append_cstr(&out_sb, &ol, &oc, " }");
    }
    out_sb = cc__va_pad_repl(s, span_a, rb + 1, out_sb, &ol, &oc);
    if (!out_sb) return -1;
    *out = out_sb;
    return 0;
}

/* Analyze the LHS text of `LHS = { ... };` for the braced-assignment trap.
 * Returns the variant index, or -1 (with *out_unresolved set) when the LHS
 * does not resolve. */
static int cc__va_resolve_lvalue(const char* s, size_t n, size_t lhs_a, size_t lhs_b,
                                 int* out_ok_form) {
    size_t a = lhs_a, b = lhs_b;
    cc__va_trim(s, &a, &b);
    if (b <= a) return -1;
    while (b - a >= 2 && s[a] == '(') {
        size_t rp = 0;
        if (cc_find_matching_paren(s, n, a, &rp) && rp == b - 1) {
            a++;
            b--;
            cc__va_trim(s, &a, &b);
        } else break;
    }
    int deref = 0;
    while (a < b && s[a] == '*') { deref++; a++; while (a < b && isspace((unsigned char)s[a])) a++; }
    if (a >= b || !cc_is_ident_start(s[a])) return -1;
    size_t ra = a, re = a;
    while (re < b && cc_is_ident_char(s[re])) re++;
    /* rest must be empty or a bracket group `[...]` */
    size_t rest = re;
    int indexed = 0;
    while (rest < b && isspace((unsigned char)s[rest])) rest++;
    if (rest < b) {
        if (s[rest] != '[') return -1;
        size_t rbk = 0;
        if (!cc_find_matching_bracket(s, n, rest, &rbk) || rbk > b - 1) return -1;
        indexed = 1;
        rest = rbk + 1;
        while (rest < b && isspace((unsigned char)s[rest])) rest++;
        if (rest < b) return -1;
    }
    int form = 0;
    int vi = cc__va_resolve_root(s, n, lhs_a, s + ra, re - ra, &form);
    if (vi < 0) return -1;
    if (form == CC_VA_FORM_KIND) return -1;
    /* A pointer must be deref'd or indexed to be a variant lvalue. */
    if (form == CC_VA_FORM_PTR && !deref && !indexed) return -1;
    if (out_ok_form) *out_ok_form = form;
    return vi;
}

static int cc__va_step_construct(const char* s, size_t n, const char* path, CCVaEdits* ed) {
    int nerr = 0;
    CCInertScan sc;
    cc_inert_scan_init(&sc, NULL);
    size_t last_boundary = 0; /* position just after last `;` `{` `}` `:` or `)`-at-depth0 */
    int par = 0;
    size_t i = 0;
    while (i < n) {
        if (cc_inert_scan_step(&sc, s, n, &i)) continue;
        char c = s[i];
        if (c == '(') { par++; i++; continue; }
        if (c == ')') { if (par) par--; if (par == 0) last_boundary = i + 1; i++; continue; }
        if (c == ';' || c == '}' || c == ':') { if (par == 0) last_boundary = i + 1; i++; continue; }
        if (c == '{') {
            /* Is this an aggregate-open that begins a designated variant
             * construction?  Peek: first non-ws char is '.', then ident. */
            size_t p = cc_skip_ws_and_comments(s, n, i + 1);
            if (par == 0) { /* update boundary AFTER we grab it below */ }
            if (!(p < n && s[p] == '.' && p + 1 < n && cc_is_ident_start(s[p + 1]))) {
                if (par == 0) last_boundary = i + 1;
                i++;
                continue;
            }
            size_t rb = 0;
            if (!cc_find_matching_brace(s, n, i, &rb)) {
                if (par == 0) last_boundary = i + 1;
                i++;
                continue;
            }
            /* Grab the first designator ident (for LHS-less resolution). */
            size_t d_ia = p + 1, d_ib = d_ia;
            while (d_ib < n && cc_is_ident_char(s[d_ib])) d_ib++;

            /* Context: what precedes the '{'? */
            size_t q = cc__va_back_ws(s, i);
            /* (a) compound literal `(Name){` */
            if (q > 0 && s[q - 1] == ')') {
                size_t lp = cc__va_back_group(s, q, '(', ')');
                size_t ta = lp + 1, tb = q - 1;
                cc__va_trim(s, &ta, &tb);
                if (tb > ta && cc_is_ident_start(s[ta])) {
                    size_t te = ta;
                    while (te < tb && cc_is_ident_char(s[te])) te++;
                    if (te == tb) {
                        int vi = cc__va_find(s + ta, tb - ta);
                        if (vi >= 0) {
                            char* repl = NULL;
                            if (cc__va_rewrite_ctor_braces(s, n, path, vi, i, i, rb, "", &repl) < 0) {
                                nerr++;
                            } else if (repl) {
                                if (cc__va_edit_add(ed, i, rb + 1, repl) != 0) nerr++;
                            }
                            i = rb + 1;
                            continue;
                        }
                    }
                }
                /* Non-variant compound literal: walk inside so a nested
                 * `{ .arm = }` payload (e.g. `(Hold){ .cell = { .num = 3 } }`)
                 * is still rewritten. */
                if (par == 0) last_boundary = i + 1;
                i++;
                continue;
            }
            /* (b)/(c): `... = {` */
            if (q > 0 && s[q - 1] == '=' &&
                !(q > 1 && (s[q - 2] == '=' || s[q - 2] == '!' || s[q - 2] == '<' ||
                            s[q - 2] == '>' || s[q - 2] == '+' || s[q - 2] == '-' ||
                            s[q - 2] == '*' || s[q - 2] == '/' || s[q - 2] == '%' ||
                            s[q - 2] == '&' || s[q - 2] == '|' || s[q - 2] == '^'))) {
                size_t eq = q - 1;
                size_t stmt_a = cc_skip_ws_and_comments(s, eq, last_boundary);
                /* Skip leading `else` / `do` keywords. */
                for (;;) {
                    if (cc_match_ident_kw(s, n, stmt_a, "else")) { stmt_a = cc_skip_ws_and_comments(s, n, stmt_a + 4); continue; }
                    if (cc_match_ident_kw(s, n, stmt_a, "do")) { stmt_a = cc_skip_ws_and_comments(s, n, stmt_a + 2); continue; }
                    break;
                }
                /* Decl-init?  Tokens before '=' of shape [quals]* Name [*]* ident */
                int is_decl = 0, decl_vi = -1;
                {
                    size_t t = stmt_a;
                    int ntoks = 0;
                    int saw_star = 0;
                    int vi_tok = -1;
                    while (t < eq) {
                        if (isspace((unsigned char)s[t])) { t++; continue; }
                        {
                            size_t t2 = cc_skip_ws_and_comments(s, eq, t);
                            if (t2 != t) { t = t2; continue; }
                        }
                        if (s[t] == '*') { saw_star = 1; t++; continue; }
                        if (!cc_is_ident_start(s[t])) { ntoks = -1; break; }
                        size_t e = t;
                        while (e < eq && cc_is_ident_char(s[e])) e++;
                        if (vi_tok < 0) {
                            int cand = cc__va_find(s + t, e - t);
                            if (cand >= 0 && ntoks <= 3) vi_tok = cand;
                        }
                        ntoks++;
                        t = e;
                    }
                    if (ntoks >= 2 && vi_tok >= 0 && !saw_star) {
                        is_decl = 1;
                        decl_vi = vi_tok;
                    } else if (ntoks >= 2 && vi_tok >= 0 && saw_star) {
                        is_decl = 1; /* pointer decl with braces: leave to C */
                        decl_vi = -1;
                    } else if (ntoks >= 2 && vi_tok < 0) {
                        /* some other declaration (two tokens) — leave alone */
                        is_decl = 1;
                        decl_vi = -1;
                    }
                }
                if (is_decl) {
                    if (decl_vi >= 0) {
                        char* repl = NULL;
                        if (cc__va_rewrite_ctor_braces(s, n, path, decl_vi, i, i, rb, "", &repl) < 0) {
                            nerr++;
                        } else if (repl) {
                            if (cc__va_edit_add(ed, i, rb + 1, repl) != 0) nerr++;
                        }
                        i = rb + 1;
                        continue;
                    }
                    /* `Hold h = { .cell = { .num = 3 } }`: leave the outer
                     * braces for C, walk inside for the nested variant. */
                    if (par == 0) last_boundary = i + 1;
                    i++;
                    continue;
                }
                /* Braced ASSIGNMENT (statement level only: '=' outside parens). */
                if (par == 0) {
                    int form = 0;
                    size_t lhs_a = stmt_a, lhs_b = eq;
                    cc__va_trim(s, &lhs_a, &lhs_b);
                    int lhs_show = (int)(lhs_b - lhs_a) > 64 ? 64 : (int)(lhs_b - lhs_a);
                    int vi = cc__va_resolve_lvalue(s, n, stmt_a, eq, &form);
                    if (vi < 0) {
                        /* Unique-arm guess is only for unresolved LHS
                         * (field paths, missing decls). A simple ident
                         * declared as a non-variant must not be rewritten
                         * just because some variant in the TU has that arm
                         * (`Field f; f = { .buf = ... }` vs `@variant V { buf: }`). */
                        int skip_guess = 0;
                        if (lhs_b > lhs_a && cc_is_ident_start(s[lhs_a])) {
                            size_t e = lhs_a;
                            while (e < lhs_b && cc_is_ident_char(s[e])) e++;
                            size_t rest = e;
                            while (rest < lhs_b && isspace((unsigned char)s[rest])) rest++;
                            if (rest == lhs_b &&
                                cc__va_ident_has_nonvariant_decl(s, n, stmt_a,
                                                                 s + lhs_a, e - lhs_a))
                                skip_guess = 1;
                        }
                        if (skip_guess) {
                            cc__va_err(s, n, path, stmt_a,
                                       "braced assignment 'lhs = { ... };' is only defined for @variant types — '%.*s' is not a variant (for a plain struct use a compound literal: 'lhs = (T){ ... };')",
                                       lhs_show, s + lhs_a);
                            nerr++;
                            i = rb + 1;
                            continue;
                        }
                        /* fall back: unique-arm resolution */
                        int uvi = -1;
                        int owners = cc__va_variants_with_arm(s + d_ia, d_ib - d_ia, &uvi);
                        if (owners == 1) {
                            vi = uvi;
                        } else if (owners > 1) {
                            cc__va_err(s, n, path, stmt_a,
                                       "cannot resolve the variant type of braced assignment target '%.*s' (arm '.%.*s' belongs to more than one variant) — annotate the value: 'lhs = (VariantName){ .%.*s = ... };'",
                                       lhs_show, s + lhs_a,
                                       (int)(d_ib - d_ia), s + d_ia,
                                       (int)(d_ib - d_ia), s + d_ia);
                            nerr++;
                            i = rb + 1;
                            continue;
                        } else {
                            cc__va_err(s, n, path, stmt_a,
                                       "braced assignment 'lhs = { ... };' is only defined for @variant types — '%.*s' does not resolve to a variant and '.%.*s' is not an arm of any declared variant (for a plain struct use a compound literal: 'lhs = (T){ ... };')",
                                       lhs_show, s + lhs_a,
                                       (int)(d_ib - d_ia), s + d_ia);
                            nerr++;
                            i = rb + 1;
                            continue;
                        }
                    }
                    {
                        char prefix[128];
                        snprintf(prefix, sizeof(prefix), "(%s)", g_va[vi].name);
                        char* repl = NULL;
                        if (cc__va_rewrite_ctor_braces(s, n, path, vi, i, i, rb, prefix, &repl) < 0) {
                            nerr++;
                        } else if (repl) {
                            if (cc__va_edit_add(ed, i, rb + 1, repl) != 0) nerr++;
                        } else {
                            /* Raw designators (.kind/.u) or positional init:
                             * keep the braces verbatim, just supply the
                             * compound-literal type so the assignment parses. */
                            char* raw = NULL;
                            size_t rl = 0, rc2 = 0;
                            cc_sb_append_cstr(&raw, &rl, &rc2, prefix);
                            cc_sb_append(&raw, &rl, &rc2, s + i, rb + 1 - i);
                            if (!raw || cc__va_edit_add(ed, i, rb + 1, raw) != 0) nerr++;
                        }
                    }
                    i = rb + 1;
                    continue;
                }
            }
            /* Nested/other context: leave alone. */
            i = rb + 1;
            continue;
        }
        i++;
    }
    return nerr ? -1 : 0;
}

/* ==================================================================== */
/* Uses step: bare designators in comparison / assignment position       */
/* ==================================================================== */

/* Resolve the "partner" expression span to a variant index (its type must
 * be tag-valued: `X.kind` / `X->kind`, or a NameKind-typed identifier, or a
 * NameKind type name in a decl target). Returns vi or -1. */
static int cc__va_resolve_partner(const char* s, size_t n, size_t a, size_t b) {
    a = cc_skip_ws_and_comments(s, b, a);   /* leading comments are inert */
    cc__va_trim(s, &a, &b);
    if (b <= a) return -1;
    while (b - a >= 2 && s[a] == '(') {
        size_t rp = 0;
        if (cc_find_matching_paren(s, n, a, &rp) && rp == b - 1) {
            a++;
            b--;
            cc__va_trim(s, &a, &b);
        } else break;
    }
    if (b <= a) return -1;
    /* ends with `.kind` / `->kind`? */
    if (b - a >= 5 && memcmp(s + b - 4, "kind", 4) == 0) {
        size_t k = cc__va_back_ws(s, b - 4);
        if (k > a && s[k - 1] == '.') {
            size_t ba, ra, rb2;
            int hops = 0;
            if (cc__va_member_base(s, k - 1, &ba, &ra, &rb2, &hops)) {
                int form = 0;
                int vi = cc__va_resolve_root(s, n, a, s + ra, rb2 - ra, &form);
                if (vi >= 0 && hops == 0) return vi;
            }
            return -1;
        }
        if (k > a + 1 && s[k - 1] == '>' && s[k - 2] == '-') {
            size_t ba, ra, rb2;
            int hops = 0;
            if (cc__va_member_base(s, k - 2, &ba, &ra, &rb2, &hops)) {
                int form = 0;
                int vi = cc__va_resolve_root(s, n, a, s + ra, rb2 - ra, &form);
                if (vi >= 0 && hops == 0) return vi;
            }
            return -1;
        }
    }
    /* single identifier of NameKind type, or the NameKind type name itself
     * (decl target `NameKind k`) */
    if (cc_is_ident_start(s[a])) {
        size_t e = a;
        while (e < b && cc_is_ident_char(s[e])) e++;
        if (e == b) {
            int vi = cc__va_find_kind(s + a, e - a);
            if (vi >= 0) return vi;
            int form = 0;
            vi = cc__va_resolve_root(s, n, a, s + a, e - a, &form);
            if (vi >= 0 && form == CC_VA_FORM_KIND) return vi;
            return -1;
        }
        /* decl target: `... NameKind k` — last two tokens */
        {
            /* find last ident token */
            size_t t = b;
            while (t > a && !cc_is_ident_char(s[t - 1])) t--;
            size_t le = t, la = t;
            while (la > a && cc_is_ident_char(s[la - 1])) la--;
            if (le > la) {
                size_t t2 = cc__va_back_ws(s, la);
                size_t pe = t2, pa = t2;
                while (pa > a && cc_is_ident_char(s[pa - 1])) pa--;
                if (pe > pa) {
                    int vi = cc__va_find_kind(s + pa, pe - pa);
                    if (vi >= 0) return vi;
                }
                /* assignment target `k` where k: NameKind */
                int form = 0;
                int vi2 = cc__va_resolve_root(s, n, a, s + la, le - la, &form);
                if (vi2 >= 0 && form == CC_VA_FORM_KIND && le == b) return vi2;
            }
        }
    }
    return -1;
}

static int cc__va_step_bare_designators(const char* s, size_t n, const char* path, CCVaEdits* ed) {
    int nerr = 0;
    CCInertScan sc;
    cc_inert_scan_init(&sc, NULL);
    size_t last_boundary = 0;
    int par_depth = 0;
    size_t i = 0;
    while (i < n) {
        if (cc_inert_scan_step(&sc, s, n, &i)) continue;
        char c = s[i];
        if (c == '(') { par_depth++; i++; continue; }
        if (c == ')') { if (par_depth) par_depth--; i++; continue; }
        if (c == ';' || c == '{' || c == '}' || c == ':') { last_boundary = i + 1; i++; continue; }
        if (c != '.') { i++; continue; }
        if (i + 1 >= n || !cc_is_ident_start(s[i + 1])) { i++; continue; }
        /* member access? (prev is ident/')'/']') → not bare */
        {
            size_t q = cc__va_back_ws(s, i);
            if (q > 0 && (cc_is_ident_char(s[q - 1]) || s[q - 1] == ')' || s[q - 1] == ']')) {
                i++;
                continue;
            }
        }
        size_t ia = i + 1, ib = ia;
        while (ib < n && cc_is_ident_char(s[ib])) ib++;
        /* Trigger shapes */
        size_t q = cc__va_back_ws(s, i);
        int after_eqeq = (q >= 2 && s[q - 1] == '=' && (s[q - 2] == '=' || s[q - 2] == '!'));
        int after_assign = 0;
        size_t eq_pos = 0;
        if (!after_eqeq && q >= 1 && s[q - 1] == '=' &&
            !(q >= 2 && (s[q - 2] == '<' || s[q - 2] == '>' || s[q - 2] == '+' ||
                         s[q - 2] == '-' || s[q - 2] == '*' || s[q - 2] == '/' ||
                         s[q - 2] == '%' || s[q - 2] == '&' || s[q - 2] == '|' ||
                         s[q - 2] == '^' || s[q - 2] == '!' || s[q - 2] == '='))) {
            after_assign = 1;
            eq_pos = q - 1;
        }
        int before_cmp = 0;
        {
            size_t f = cc_skip_ws_and_comments(s, n, ib);
            if (f + 1 < n && (s[f] == '=' || s[f] == '!') && s[f + 1] == '=') before_cmp = 1;
        }
        if (!after_eqeq && !after_assign && !before_cmp) { i = ib; continue; }

        int vi = -1;
        if (after_eqeq) {
            /* partner is on the LEFT of the == / != */
            size_t op = q - 2;
            size_t pa = last_boundary;
            /* partner span: from the last boundary/logical-op to op */
            {
                size_t t = op;
                int par2 = 0, brk2 = 0;
                while (t > pa) {
                    char pc = s[t - 1];
                    if (pc == ')') { size_t o = cc__va_back_group(s, t, '(', ')'); if (o == 0) break; t = o; continue; }
                    if (pc == ']') { size_t o = cc__va_back_group(s, t, '[', ']'); if (o == 0) break; t = o; continue; }
                    if (pc == '(' || pc == '[' || pc == '{' || pc == ',' || pc == ';' ||
                        pc == '?' || pc == ':') break;
                    if (pc == '&' && t > pa + 1 && s[t - 2] == '&') break;
                    if (pc == '|' && t > pa + 1 && s[t - 2] == '|') break;
                    /* '->' is a member accessor, not an operator boundary:
                       keep walking so `p->kind` is captured whole (a bare '>'
                       comparison still terminates the partner). */
                    if (pc == '>' && t > pa + 1 && s[t - 2] == '-') { t--; continue; }
                    if (pc == '=' || pc == '<' || pc == '>' || pc == '!') break;
                    (void)par2; (void)brk2;
                    t--;
                }
                vi = cc__va_resolve_partner(s, n, t, op);
            }
        } else if (after_assign) {
            vi = cc__va_resolve_partner(s, n, last_boundary, eq_pos);
        } else { /* before_cmp */
            size_t f = cc_skip_ws_and_comments(s, n, ib);
            size_t pb = cc__va_expr_end(s, n, f + 2);
            vi = cc__va_resolve_partner(s, n, f + 2, pb);
        }
        if (vi < 0) {
            cc__va_err(s, n, path, i,
                       "bare designator '.%.*s' cannot be resolved from context (no variant type governs this comparison/assignment)",
                       (int)(ib - ia), s + ia);
            cc__va_note(s, n, path, i,
                        "compare against a variant tag ('v.kind == .%.*s') or a NameKind-typed value, or spell the tag explicitly ('<Variant>_%.*s')",
                        (int)(ib - ia), s + ia, (int)(ib - ia), s + ia);
            nerr++;
            i = ib;
            continue;
        }
        {
            int ai = cc__va_arm_index(vi, s + ia, ib - ia);
            if (ai < 0) {
                char arms[512];
                cc__va_arm_list(vi, arms, sizeof(arms));
                const char* sugg = cc__va_suggest_arm(vi, s + ia, ib - ia);
                if (sugg)
                    cc__va_err(s, n, path, i,
                               "variant '%s' has no arm '.%.*s' (arms: %s) — did you mean '.%s'?",
                               g_va[vi].name, (int)(ib - ia), s + ia, arms, sugg);
                else
                    cc__va_err(s, n, path, i,
                               "variant '%s' has no arm '.%.*s' (arms: %s)",
                               g_va[vi].name, (int)(ib - ia), s + ia, arms);
                nerr++;
                i = ib;
                continue;
            }
            char* repl = NULL;
            size_t rl = 0, rc = 0;
            cc_sb_append_fmt(&repl, &rl, &rc, "%s_%s", g_va[vi].name, g_va[vi].arms[ai].name);
            if (!repl || cc__va_edit_add(ed, i, ib, repl) != 0) nerr++;
        }
        i = ib;
    }
    return nerr ? -1 : 0;
}

/* ==================================================================== */
/* Uses step: switches (subject rewrite, case labels, exhaustiveness)    */
/* ==================================================================== */

static int cc__va_step_switches(const char* s, size_t n, const char* path, CCVaEdits* ed) {
    int nerr = 0;
    CCVaSwitchInfo* sw = (CCVaSwitchInfo*)calloc(CC_VA_MAX_SWITCHES, sizeof(CCVaSwitchInfo));
    CCVaLabelInfo* labels = (CCVaLabelInfo*)calloc(CC_VA_MAX_LABELS, sizeof(CCVaLabelInfo));
    if (!sw || !labels) { free(sw); free(labels); return -1; }
    int cnt = cc__va_collect_switches(s, n, sw, CC_VA_MAX_SWITCHES);

    for (int w = 0; w < cnt; w++) {
        int cls = cc__va_classify_switch_subject(s, n, &sw[w]);
        int lcnt = cc__va_collect_labels(s, sw, cnt, w, labels, CC_VA_MAX_LABELS);
        /* Any designator labels? */
        int have_desig = 0;
        for (int l = 0; l < lcnt; l++) {
            if (!labels[l].is_default && labels[l].lab_b > labels[l].lab_a &&
                s[labels[l].lab_a] == '.') have_desig = 1;
        }
        if (cls == 0) {
            if (have_desig) {
                for (int l = 0; l < lcnt; l++) {
                    if (labels[l].is_default || labels[l].lab_b <= labels[l].lab_a ||
                        s[labels[l].lab_a] != '.') continue;
                    size_t da = labels[l].lab_a + 1, db = da;
                    while (db < labels[l].lab_b && cc_is_ident_char(s[db])) db++;
                    cc__va_err(s, n, path, labels[l].lab_a,
                               "designator case label '.%.*s' requires a switch on a @variant subject — '%.*s' does not resolve to a variant",
                               (int)(db - da), s + da,
                               (int)(sw[w].subj_b - sw[w].subj_a), s + sw[w].subj_a);
                    nerr++;
                }
            }
            continue;
        }
        int vi = sw[w].vi;
        const CCVaDef* def = &g_va[vi];
        /* Subject rewrite */
        if (cls == 2 || cls == 3) {
            char* repl = NULL;
            size_t rl = 0, rc = 0;
            cc_sb_append_cstr(&repl, &rl, &rc, "(");
            cc__va_append_flat(&repl, &rl, &rc, s, sw[w].subj_a, sw[w].subj_b);
            cc_sb_append_cstr(&repl, &rl, &rc, cls == 3 ? ")->kind" : ").kind");
            repl = cc__va_pad_repl(s, sw[w].subj_a, sw[w].subj_b, repl, &rl, &rc);
            if (!repl || cc__va_edit_add(ed, sw[w].subj_a, sw[w].subj_b, repl) != 0) nerr++;
        }
        /* Labels */
        unsigned long long covered = 0;
        int have_default = 0;
        for (int l = 0; l < lcnt; l++) {
            if (labels[l].is_default) { have_default = 1; continue; }
            size_t la = labels[l].lab_a, lb = labels[l].lab_b;
            if (lb <= la) continue;
            if (s[la] == '.') {
                size_t da = la + 1, db = da;
                while (db < lb && cc_is_ident_char(s[db])) db++;
                int ai = cc__va_arm_index(vi, s + da, db - da);
                if (ai < 0) {
                    char arms[512];
                    cc__va_arm_list(vi, arms, sizeof(arms));
                    const char* sugg = cc__va_suggest_arm(vi, s + da, db - da);
                    if (sugg)
                        cc__va_err(s, n, path, la,
                                   "variant '%s' has no arm '.%.*s' (arms: %s) — did you mean '.%s'?",
                                   def->name, (int)(db - da), s + da, arms, sugg);
                    else
                        cc__va_err(s, n, path, la,
                                   "variant '%s' has no arm '.%.*s' (arms: %s)",
                                   def->name, (int)(db - da), s + da, arms);
                    nerr++;
                    continue;
                }
                covered |= 1ull << ai;
                {
                    char* repl = NULL;
                    size_t rl = 0, rc = 0;
                    cc_sb_append_fmt(&repl, &rl, &rc, "%s_%s", def->name, def->arms[ai].name);
                    if (!repl || cc__va_edit_add(ed, la, db, repl) != 0) nerr++;
                }
                continue;
            }
            /* tag constant `Name_arm`? */
            {
                size_t da = la, db = la;
                while (db < lb && cc_is_ident_char(s[db])) db++;
                int matched = 0;
                if (db == lb) {
                    size_t pfx = strlen(def->name);
                    if (db - da > pfx + 1 && memcmp(s + da, def->name, pfx) == 0 &&
                        s[da + pfx] == '_') {
                        int ai = cc__va_arm_index(vi, s + da + pfx + 1, db - (da + pfx + 1));
                        if (ai >= 0) {
                            covered |= 1ull << ai;
                            matched = 1;
                        }
                    }
                }
                if (!matched) {
                    cc__va_err(s, n, path, la,
                               "case label on a switch over variant '%s' must be a designator ('case .arm:') or a tag constant ('case %s_arm:') — got '%.*s'",
                               def->name, def->name, (int)(lb - la), s + la);
                    nerr++;
                }
            }
        }
        /* Exhaustiveness */
        if (!have_default) {
            char missing[512];
            size_t mo = 0;
            int nmiss = 0;
            missing[0] = '\0';
            for (int a = 0; a < def->narms; a++) {
                if (covered & (1ull << a)) continue;
                int m = snprintf(missing + mo, sizeof(missing) - mo, "%s'.%s'",
                                 nmiss ? ", " : "", def->arms[a].name);
                if (m > 0 && (size_t)m < sizeof(missing) - mo) mo += (size_t)m;
                nmiss++;
            }
            if (nmiss > 0) {
                cc__va_err(s, n, path, sw[w].kw,
                           "switch on variant '%s' is not exhaustive: missing arm%s %s (add the missing case%s or a 'default:' to opt out)",
                           def->name, nmiss > 1 ? "s" : "", missing, nmiss > 1 ? "s" : "");
                nerr++;
            }
        }
    }
    free(sw);
    free(labels);
    return nerr ? -1 : 0;
}

/* ==================================================================== */
/* Uses step: projections (+ .kind read-only, unknown members)           */
/* ==================================================================== */

typedef struct {
    int vi;
    int arm;
    char root[128];
    size_t a, b; /* dominated region */
} CCVaGuard;

/* Collect `if (root.kind == Name_arm)` / `if (root.kind != Name_arm)` style
 * guards from the CURRENT buffer (bare designators/switch labels already
 * rewritten to tag constants).  Each guard yields up to two dominated
 * regions:
 *   ==  : then-region dominates the named arm; else-region dominates the
 *         COMPLEMENT arm when the variant has exactly two arms.
 *   !=  : then-region dominates the complement (two-arm variants only);
 *         else-region dominates the named arm.
 * The complement rule is what legalizes the spec §5a/§10 idiom of touching
 * the other arm inside a projection's `!>` handler / `?>` fallback (both
 * lower to exactly these if/else shapes). */
static void cc__va_guard_push(CCVaGuard* out, int max, int* cnt,
                              int vi, int arm, const char* s, size_t ra, size_t rb,
                              size_t a, size_t b) {
    if (arm < 0 || *cnt >= max) return;
    out[*cnt].vi = vi;
    out[*cnt].arm = arm;
    snprintf(out[*cnt].root, sizeof(out[*cnt].root), "%.*s", (int)(rb - ra), s + ra);
    out[*cnt].a = a;
    out[*cnt].b = b;
    (*cnt)++;
}

static int cc__va_complement_arm(int vi, int arm) {
    if (vi < 0 || vi >= g_va_n || g_va[vi].narms != 2) return -1;
    return arm == 0 ? 1 : 0;
}

static int cc__va_collect_guards(const char* s, size_t n, CCVaGuard* out, int max) {
    int cnt = 0;
    CCInertScan sc;
    cc_inert_scan_init(&sc, NULL);
    size_t i = 0;
    while (i < n) {
        if (cc_inert_scan_step(&sc, s, n, &i)) continue;
        if (!(s[i] == 'i' && cc_match_ident_kw(s, n, i, "if"))) { i++; continue; }
        size_t p = cc_skip_ws_and_comments(s, n, i + 2);
        if (p >= n || s[p] != '(') { i += 2; continue; }
        size_t rp = 0;
        if (!cc_find_matching_paren(s, n, p, &rp)) { i += 2; continue; }
        size_t ca = p + 1, cb = rp;
        cc__va_trim(s, &ca, &cb);
        while (cb - ca >= 2 && s[ca] == '(') {
            size_t rp2 = 0;
            if (cc_find_matching_paren(s, n, ca, &rp2) && rp2 == cb - 1) {
                ca++;
                cb--;
                cc__va_trim(s, &ca, &cb);
            } else break;
        }
        /* find top-level `==` / `!=` */
        size_t eq = cb;
        int negated = 0;
        {
            int par = 0, brk = 0;
            for (size_t t = ca; t + 1 < cb; t++) {
                char tc = s[t];
                if (tc == '(') par++;
                else if (tc == ')') { if (par) par--; }
                else if (tc == '[') brk++;
                else if (tc == ']') { if (brk) brk--; }
                else if (!par && !brk && tc == '=' && s[t + 1] == '=' &&
                         (t == ca || (s[t - 1] != '!' && s[t - 1] != '<' && s[t - 1] != '>' && s[t - 1] != '='))) {
                    eq = t;
                    break;
                } else if (!par && !brk && tc == '!' && s[t + 1] == '=') {
                    eq = t;
                    negated = 1;
                    break;
                } else if (!par && !brk && (tc == '&' || tc == '|') && s[t + 1] == tc) {
                    eq = cb; /* compound condition: v1 domination requires a lone check */
                    break;
                }
            }
        }
        if (eq >= cb) { i = rp + 1; continue; }
        size_t l_a = ca, l_b = eq, r_a = eq + 2, r_b = cb;
        cc__va_trim(s, &l_a, &l_b);
        cc__va_trim(s, &r_a, &r_b);
        /* one side is a tag constant Name_arm, other ends in .kind/->kind */
        int vi = -1, arm = -1;
        size_t kside_a = 0, kside_b = 0;
        for (int side = 0; side < 2 && vi < 0; side++) {
            size_t ta = side ? r_a : l_a, tb = side ? r_b : l_b;
            if (tb <= ta || !cc_is_ident_start(s[ta])) continue;
            size_t te = ta;
            while (te < tb && cc_is_ident_char(s[te])) te++;
            if (te != tb) continue;
            for (int v = 0; v < g_va_n && vi < 0; v++) {
                size_t pfx = strlen(g_va[v].name);
                if (tb - ta > pfx + 1 && memcmp(s + ta, g_va[v].name, pfx) == 0 &&
                    s[ta + pfx] == '_') {
                    int ai = cc__va_arm_index(v, s + ta + pfx + 1, tb - (ta + pfx + 1));
                    if (ai >= 0) {
                        vi = v;
                        arm = ai;
                        kside_a = side ? l_a : r_a;
                        kside_b = side ? l_b : r_b;
                    }
                }
            }
        }
        if (vi < 0) { i = rp + 1; continue; }
        /* kind side must end with .kind / ->kind and root-resolve */
        {
            size_t a = kside_a, b = kside_b;
            cc__va_trim(s, &a, &b);
            if (!(b - a >= 5 && memcmp(s + b - 4, "kind", 4) == 0)) { i = rp + 1; continue; }
            size_t k = cc__va_back_ws(s, b - 4);
            size_t accpos;
            if (k > a && s[k - 1] == '.') accpos = k - 1;
            else if (k > a + 1 && s[k - 1] == '>' && s[k - 2] == '-') accpos = k - 2;
            else { i = rp + 1; continue; }
            size_t ba, ra, rb2;
            int hops = 0;
            if (!cc__va_member_base(s, accpos, &ba, &ra, &rb2, &hops) || hops != 0) {
                i = rp + 1;
                continue;
            }
            /* then-region: the guarded statement/block after ')' */
            size_t g_a = cc_skip_ws_and_comments(s, n, rp + 1);
            size_t g_b = g_a;
            if (g_a < n && s[g_a] == '{') {
                size_t grb = 0;
                if (!cc_find_matching_brace(s, n, g_a, &grb)) { i = rp + 1; continue; }
                g_b = grb;
            } else {
                if (!cc__va_find_semi(s, n, g_a, &g_b)) { i = rp + 1; continue; }
            }
            {
                int comp = cc__va_complement_arm(vi, arm);
                cc__va_guard_push(out, max, &cnt, vi, negated ? comp : arm,
                                  s, ra, rb2, g_a, g_b);
                /* else-region (when present) */
                size_t e_kw = cc_skip_ws_and_comments(s, n, g_b + 1);
                if (cc_match_ident_kw(s, n, e_kw, "else")) {
                    size_t e_a = cc_skip_ws_and_comments(s, n, e_kw + 4);
                    size_t e_b = e_a;
                    int e_ok = 0;
                    if (e_a < n && s[e_a] == '{') {
                        size_t erb = 0;
                        if (cc_find_matching_brace(s, n, e_a, &erb)) {
                            e_b = erb;
                            e_ok = 1;
                        }
                    } else if (cc__va_find_semi(s, n, e_a, &e_b)) {
                        e_ok = 1;
                    }
                    if (e_ok)
                        cc__va_guard_push(out, max, &cnt, vi, negated ? arm : comp,
                                          s, ra, rb2, e_a, e_b);
                }
            }
        }
        i = rp + 1;
    }
    return cnt;
}

static int cc__va_step_projections(const char* s, size_t n, const char* path, CCVaEdits* ed) {
    int nerr = 0;

    /* Domination context. */
    CCVaSwitchInfo* sw = (CCVaSwitchInfo*)calloc(CC_VA_MAX_SWITCHES, sizeof(CCVaSwitchInfo));
    CCVaGuard* guards = (CCVaGuard*)calloc(CC_VA_MAX_GUARDS, sizeof(CCVaGuard));
    CCVaLabelInfo* labels = (CCVaLabelInfo*)calloc(CC_VA_MAX_LABELS, sizeof(CCVaLabelInfo));
    if (!sw || !guards || !labels) { free(sw); free(guards); free(labels); return -1; }
    int swcnt = cc__va_collect_switches(s, n, sw, CC_VA_MAX_SWITCHES);
    for (int w = 0; w < swcnt; w++)
        (void)cc__va_classify_switch_subject(s, n, &sw[w]);
    int gcnt = cc__va_collect_guards(s, n, guards, CC_VA_MAX_GUARDS);
    CCVaCgSpan cg[CC_VA_MAX_CG_SPANS];
    int ncg = cc__va_collect_codegen_spans(s, n, cg, CC_VA_MAX_CG_SPANS);

    CCInertScan sc;
    cc_inert_scan_init(&sc, NULL);
    size_t i = 0;
    while (i < n) {
        if (cc_inert_scan_step(&sc, s, n, &i)) continue;
        char c = s[i];
        size_t acc = 0;   /* position of '.' or '-' */
        size_t mem_a = 0; /* member ident */
        int is_arrow = 0;
        if (c == '.' ) {
            /* member access only: prev is ident char / ')' / ']' */
            size_t q = cc__va_back_ws(s, i);
            if (!(q > 0 && (cc_is_ident_char(s[q - 1]) || s[q - 1] == ')' || s[q - 1] == ']'))) {
                i++;
                continue;
            }
            if (i + 1 >= n || !cc_is_ident_start(s[i + 1])) { i++; continue; }
            acc = i;
            mem_a = i + 1;
        } else if (c == '-' && i + 1 < n && s[i + 1] == '>') {
            if (i + 2 >= n || !cc_is_ident_start(s[i + 2])) { i += 2; continue; }
            acc = i;
            is_arrow = 1;
            mem_a = i + 2;
        } else {
            i++;
            continue;
        }
        size_t mem_b = mem_a;
        while (mem_b < n && cc_is_ident_char(s[mem_b])) mem_b++;

        /* base + root */
        size_t ba = 0, ra = 0, rb = 0;
        int hops = 0;
        if (!cc__va_member_base(s, acc, &ba, &ra, &rb, &hops) || hops != 0) {
            i = mem_b;
            continue;
        }
        int form = 0;
        int vi = cc__va_resolve_root(s, n, ba, s + ra, rb - ra, &form);
        if (vi < 0 || form == CC_VA_FORM_KIND) { i = mem_b; continue; }
        const CCVaDef* def = &g_va[vi];
        size_t mlen = mem_b - mem_a;

        /* Compiler-internal members of the emitted lowering (e.g. the packed
         * byte store `__v->__cc_p`) are not user projections — skip them so
         * the decls-pass output re-scanned here is inert. */
        if (mlen >= 4 && memcmp(s + mem_a, "__cc", 4) == 0) { i = mem_b; continue; }

        /* `.u` raw interop: skip entirely (and skip the whole .u.x chain).
         * For packed variants, user-written raw `.u` was already diagnosed by
         * the earlier packed-raw-access step; any `.u` reaching here is a
         * compiler-generated projection lowered for later materialization. */
        if (mlen == 1 && s[mem_a] == 'u') { i = mem_b; continue; }

        /* `.kind` pseudo-member: read ok, write = error (except schema codegen). */
        if (mlen == 4 && memcmp(s + mem_a, "kind", 4) == 0) {
            size_t f = cc_skip_ws_and_comments(s, n, mem_b);
            int is_write = 0;
            if (f < n) {
                if (s[f] == '=' && (f + 1 >= n || s[f + 1] != '=')) is_write = 1;
                else if (f + 1 < n && s[f + 1] == '=' &&
                         (s[f] == '+' || s[f] == '-' || s[f] == '*' || s[f] == '/' ||
                          s[f] == '%' || s[f] == '&' || s[f] == '|' || s[f] == '^')) is_write = 1;
                else if (f + 2 < n && ((s[f] == '<' && s[f + 1] == '<') ||
                                       (s[f] == '>' && s[f + 1] == '>')) && s[f + 2] == '=') is_write = 1;
                else if (f + 1 < n && ((s[f] == '+' && s[f + 1] == '+') ||
                                       (s[f] == '-' && s[f + 1] == '-'))) is_write = 1;
            }
            if (is_write && !cc__va_in_cg(acc, cg, ncg)) {
                cc__va_err(s, n, path, acc,
                           "%s tag '.kind' is read-only — the tag changes only through construction or whole-variant assignment ('%.*s = (%s){ .arm = ... };')",
                           cc__va_surface(def), (int)(rb - ra), s + ra, def->name);
                nerr++;
            }
            i = mem_b;
            continue;
        }

        int ai = cc__va_arm_index(vi, s + mem_a, mlen);
        if (ai < 0) {
            /* Method call → UFCS territory, leave alone. */
            size_t f = cc_skip_ws_and_comments(s, n, mem_b);
            if (f < n && s[f] == '(') { i = mem_b; continue; }
            /* Schema product-level fields (e.g. field-mode discriminant) are
             * ordinary struct members — leave them for the C compile. */
            if (def->is_schema) { i = mem_b; continue; }
            char arms[512];
            cc__va_arm_list(vi, arms, sizeof(arms));
            const char* sugg = cc__va_suggest_arm(vi, s + mem_a, mlen);
            if (sugg)
                cc__va_err(s, n, path, acc,
                           "variant '%s' has no arm '.%.*s' (arms: %s) — did you mean '.%s'?",
                           def->name, (int)mlen, s + mem_a, arms, sugg);
            else
                cc__va_err(s, n, path, acc,
                           "variant '%s' has no arm '.%.*s' (arms: %s)",
                           def->name, (int)mlen, s + mem_a, arms);
            nerr++;
            i = mem_b;
            continue;
        }
        if (def->arms[ai].is_void) {
            cc__va_err(s, n, path, acc,
                       "arm '%s' of %s '%s' is void — it has no payload to project; compare '%.*s%skind == %s_%s' or use 'case .%s:' instead",
                       def->arms[ai].name, cc__va_surface(def), def->name,
                       (int)(rb - ra), s + ra, is_arrow ? "->" : ".",
                       def->name, def->arms[ai].name, def->arms[ai].name);
            nerr++;
            i = mem_b;
            continue;
        }

        const char* accs = is_arrow ? "->" : ".";

        /* Suffix: `!>` handler or `?>` fallback. */
        size_t f = cc_skip_ws_and_comments(s, n, mem_b);
        if (f + 1 < n && s[f] == '!' && s[f + 1] == '>') {
            size_t h = cc_skip_ws_and_comments(s, n, f + 2);
            if (h < n && s[h] == '(') {
                cc__va_err(s, n, path, f,
                           "variant projection '!>' takes a bare handler block ('%.*s%s%s !> { ... }') — there is no error value to bind",
                           (int)(rb - ra), s + ra, accs, def->arms[ai].name);
                nerr++;
                i = mem_b;
                continue;
            }
            if (h >= n || s[h] != '{') {
                cc__va_err(s, n, path, f,
                           "variant projection '!>' requires a handler block: '%.*s%s%s !> { ...diverge... }'",
                           (int)(rb - ra), s + ra, accs, def->arms[ai].name);
                nerr++;
                i = mem_b;
                continue;
            }
            size_t hrb = 0;
            if (!cc_find_matching_brace(s, n, h, &hrb)) {
                cc__va_err(s, n, path, h,
                           "variant projection '!>': unterminated handler block");
                nerr++;
                i = mem_b;
                continue;
            }
            char* repl = NULL;
            size_t rl = 0, rc = 0;
            cc_sb_append_cstr(&repl, &rl, &rc, "({ if ((");
            cc__va_append_flat(&repl, &rl, &rc, s, ba, acc);
            cc_sb_append_fmt(&repl, &rl, &rc, ")%skind != %s_%s) ", accs, def->name,
                             def->arms[ai].name);
            cc_sb_append(&repl, &rl, &rc, s + h, hrb + 1 - h);
            cc_sb_append_cstr(&repl, &rl, &rc, " (");
            cc__va_append_flat(&repl, &rl, &rc, s, ba, acc);
            cc_sb_append_fmt(&repl, &rl, &rc, ")%su.%s; })", accs, def->arms[ai].name);
            repl = cc__va_pad_repl(s, ba, hrb + 1, repl, &rl, &rc);
            if (!repl || cc__va_edit_add(ed, ba, hrb + 1, repl) != 0) nerr++;
            i = hrb + 1;
            continue;
        }
        if (f + 1 < n && s[f] == '?' && s[f + 1] == '>') {
            size_t fb_a = cc_skip_ws_and_comments(s, n, f + 2);
            size_t fb_b = cc__va_expr_end(s, n, fb_a);
            size_t fb_a2 = fb_a, fb_b2 = fb_b;
            cc__va_trim(s, &fb_a2, &fb_b2);
            if (fb_b2 <= fb_a2) {
                cc__va_err(s, n, path, f,
                           "variant projection '?>' requires a fallback expression: '%.*s%s%s ?> fallback'",
                           (int)(rb - ra), s + ra, accs, def->arms[ai].name);
                nerr++;
                i = mem_b;
                continue;
            }
            /* Lower to an if/else statement expression (not a ternary): the
             * else-branch is then a recognizable complement-domination
             * region, which legalizes other-arm projections inside the
             * fallback on the next fixpoint iteration (spec §10 idiom). */
            int id = ++g_va_tmp_id;
            char* repl = NULL;
            size_t rl = 0, rc = 0;
            cc_sb_append_fmt(&repl, &rl, &rc, "({ __typeof__((");
            cc__va_append_flat(&repl, &rl, &rc, s, ba, acc);
            cc_sb_append_fmt(&repl, &rl, &rc, ")%su.%s) __cc_pj%d; if ((", accs,
                             def->arms[ai].name, id);
            cc__va_append_flat(&repl, &rl, &rc, s, ba, acc);
            cc_sb_append_fmt(&repl, &rl, &rc, ")%skind == %s_%s) { __cc_pj%d = (", accs,
                             def->name, def->arms[ai].name, id);
            cc__va_append_flat(&repl, &rl, &rc, s, ba, acc);
            cc_sb_append_fmt(&repl, &rl, &rc, ")%su.%s; } else { __cc_pj%d = (", accs,
                             def->arms[ai].name, id);
            cc_sb_append(&repl, &rl, &rc, s + fb_a2, fb_b2 - fb_a2);
            cc_sb_append_fmt(&repl, &rl, &rc, "); } __cc_pj%d; })", id);
            repl = cc__va_pad_repl(s, ba, fb_b, repl, &rl, &rc);
            if (!repl || cc__va_edit_add(ed, ba, fb_b, repl) != 0) nerr++;
            i = fb_b;
            continue;
        }

        /* Domination. */
        int dominated = 0;
        for (int w = 0; w < swcnt && !dominated; w++) {
            if (sw[w].vi != vi) continue;
            if (!(acc >= sw[w].body_a && acc < sw[w].body_b)) continue;
            if (strlen(sw[w].root) != rb - ra ||
                memcmp(sw[w].root, s + ra, rb - ra) != 0) continue;
            int lcnt = cc__va_collect_labels(s, sw, swcnt, w, labels, CC_VA_MAX_LABELS);
            /* nearest label before acc */
            int nearest = -1;
            for (int l = 0; l < lcnt; l++) {
                if (labels[l].pos >= acc) break;
                nearest = l;
            }
            if (nearest < 0 || labels[nearest].is_default) continue;
            size_t la = labels[nearest].lab_a, lb = labels[nearest].lab_b;
            size_t pfx = strlen(def->name);
            if (lb - la > pfx + 1 && memcmp(s + la, def->name, pfx) == 0 && s[la + pfx] == '_') {
                int lai = cc__va_arm_index(vi, s + la + pfx + 1, lb - (la + pfx + 1));
                if (lai == ai) dominated = 1;
            }
        }
        for (int g = 0; g < gcnt && !dominated; g++) {
            if (guards[g].vi != vi || guards[g].arm != ai) continue;
            if (!(acc >= guards[g].a && acc < guards[g].b)) continue;
            if (strlen(guards[g].root) != rb - ra ||
                memcmp(guards[g].root, s + ra, rb - ra) != 0) continue;
            dominated = 1;
        }

        if (!dominated) {
            cc__va_err(s, n, path, acc,
                       "projection of arm '%s' is not dominated by a kind check and has no !> handler",
                       def->arms[ai].name);
            cc__va_note(s, n, path, acc,
                        "protect it: 'if (%.*s%skind == %s_%s) { ... }' or 'case .%s:' in a switch on '%.*s', or handle the other arms: '%.*s%s%s !> { ...diverge... }' / '%.*s%s%s ?> fallback'",
                        (int)(rb - ra), s + ra, accs, def->name, def->arms[ai].name,
                        def->arms[ai].name, (int)(rb - ra), s + ra,
                        (int)(rb - ra), s + ra, accs, def->arms[ai].name,
                        (int)(rb - ra), s + ra, accs, def->arms[ai].name);
            nerr++;
            i = mem_b;
            continue;
        }
        /* Dominated: `.arm` → `.u.arm`.  On the unpacked layout this is a real
         * union member (already an lvalue).  On the packed layout the later
         * materialize step rewrites `.u.arm` to either a value-returning getter
         * (rvalue positions) or a dereferenced-cast lvalue (address-of /
         * assignment positions), spec §11. */
        {
            char* repl = NULL;
            size_t rl = 0, rc = 0;
            cc_sb_append_fmt(&repl, &rl, &rc, "%su.%s", accs, def->arms[ai].name);
            if (!repl || cc__va_edit_add(ed, acc, mem_b, repl) != 0) nerr++;
        }
        i = mem_b;
    }
    free(sw);
    free(guards);
    free(labels);
    return nerr ? -1 : 0;
}

/* ==================================================================== */
/* Uses step: transition drop on whole-variant assignment                */
/* ==================================================================== */

static int cc__va_step_transitions(const char* s, size_t n, const char* path, CCVaEdits* ed) {
    (void)path;
    int nerr = 0;
    CCInertScan sc;
    cc_inert_scan_init(&sc, NULL);
    size_t last_boundary = 0;
    int par = 0, brace_depth = 0;
    size_t i = 0;
    while (i < n) {
        if (cc_inert_scan_step(&sc, s, n, &i)) continue;
        char c = s[i];
        if (c == '(') { par++; i++; continue; }
        if (c == ')') { if (par) par--; if (!par) last_boundary = i + 1; i++; continue; }
        if (c == '{') { brace_depth++; last_boundary = i + 1; i++; continue; }
        if (c == '}') { if (brace_depth) brace_depth--; last_boundary = i + 1; i++; continue; }
        if (c == ';' || c == ':') { if (!par) last_boundary = i + 1; i++; continue; }
        if (c != '=' || par != 0 || brace_depth == 0) { i++; continue; }
        /* simple assignment only */
        if (i + 1 < n && s[i + 1] == '=') { i += 2; continue; }
        if (i > 0 && (s[i - 1] == '=' || s[i - 1] == '!' || s[i - 1] == '<' ||
                      s[i - 1] == '>' || s[i - 1] == '+' || s[i - 1] == '-' ||
                      s[i - 1] == '*' || s[i - 1] == '/' || s[i - 1] == '%' ||
                      s[i - 1] == '&' || s[i - 1] == '|' || s[i - 1] == '^')) { i++; continue; }
        size_t stmt_a = last_boundary;
        while (stmt_a < i && isspace((unsigned char)s[stmt_a])) stmt_a++;
        for (;;) {
            if (cc_match_ident_kw(s, n, stmt_a, "else")) { stmt_a = cc_skip_ws_and_comments(s, n, stmt_a + 4); continue; }
            if (cc_match_ident_kw(s, n, stmt_a, "do")) { stmt_a = cc_skip_ws_and_comments(s, n, stmt_a + 2); continue; }
            break;
        }
        int form = 0;
        int vi = cc__va_resolve_lvalue(s, n, stmt_a, i, &form);
        if (vi < 0 || !g_va[vi].has_drop) { i++; continue; }
        size_t semi = 0;
        if (!cc__va_find_semi(s, n, i + 1, &semi)) { i++; continue; }
        size_t rhs_a = i + 1, rhs_b = semi;
        cc__va_trim(s, &rhs_a, &rhs_b);
        if (rhs_b <= rhs_a) { i++; continue; }
        size_t lhs_a = stmt_a, lhs_b = i;
        cc__va_trim(s, &lhs_a, &lhs_b);
        {
            int id = ++g_va_tmp_id;
            char* repl = NULL;
            size_t rl = 0, rc = 0;
            cc_sb_append_fmt(&repl, &rl, &rc, "{ %s __cc_vt%d = ", g_va[vi].name, id);
            cc_sb_append(&repl, &rl, &rc, s + rhs_a, rhs_b - rhs_a);
            cc_sb_append_fmt(&repl, &rl, &rc, "; %s__cc_drop(&(", g_va[vi].name);
            cc__va_append_flat(&repl, &rl, &rc, s, lhs_a, lhs_b);
            cc_sb_append_cstr(&repl, &rl, &rc, ")); (");
            cc__va_append_flat(&repl, &rl, &rc, s, lhs_a, lhs_b);
            cc_sb_append_fmt(&repl, &rl, &rc, ") = __cc_vt%d; }", id);
            repl = cc__va_pad_repl(s, stmt_a, semi + 1, repl, &rl, &rc);
            if (!repl || cc__va_edit_add(ed, stmt_a, semi + 1, repl) != 0) nerr++;
        }
        i = semi + 1;
        last_boundary = i;
    }
    return nerr ? -1 : 0;
}

/* ==================================================================== */
/* Uses step: scope-exit drop for destructor-bearing variant locals      */
/* ==================================================================== */

/* Does `return NAME;` / `return cc_move(NAME);` appear after `from`?
 * (Conservative guard: such a local is returned by value; the injected
 * scope-exit drop would run after the copy and free the escaping payload.) */
static int cc__va_returned_by_value(const char* s, size_t n, size_t from,
                                    const char* name, size_t namelen) {
    CCInertScan sc;
    cc_inert_scan_init(&sc, NULL);
    size_t i = from;
    while (i < n) {
        if (cc_inert_scan_step(&sc, s, n, &i)) continue;
        if (!(s[i] == 'r' && cc_match_ident_kw(s, n, i, "return"))) { i++; continue; }
        size_t p = cc_skip_ws_and_comments(s, n, i + 6);
        if (cc_match_ident_kw(s, n, p, "cc_move")) {
            p = cc_skip_ws_and_comments(s, n, p + 7);
            if (p < n && s[p] == '(') {
                p = cc_skip_ws_and_comments(s, n, p + 1);
                if (p + namelen <= n && memcmp(s + p, name, namelen) == 0 &&
                    (p + namelen == n || !cc_is_ident_char(s[p + namelen]))) return 1;
            }
        } else if (p + namelen <= n && memcmp(s + p, name, namelen) == 0 &&
                   (p + namelen == n || !cc_is_ident_char(s[p + namelen]))) {
            size_t e = cc_skip_ws_and_comments(s, n, p + namelen);
            if (e < n && s[e] == ';') return 1;
        }
        i += 6;
    }
    return 0;
}

static int cc__va_step_defers(const char* s, size_t n, const char* path, CCVaEdits* ed) {
    (void)path;
    int nerr = 0;
    CCInertScan sc;
    cc_inert_scan_init(&sc, NULL);
    /* Executable-scope tracking: a '{' opens an exec scope when it follows
     * ')' ';' '{' '}' ':' or the keywords else/do, AND its parent is exec
     * (file scope counts as exec parent only for `) {` function bodies). */
    int depth = 0;
    unsigned char exec_stack[256];
    char prev_code = 0;
    size_t prev_ident_a = 0, prev_ident_b = 0;
    size_t last_boundary = 0;
    int par = 0;
    size_t i = 0;
    while (i < n) {
        if (cc_inert_scan_step(&sc, s, n, &i)) continue;
        char c = s[i];
        if (c == '(') { par++; prev_code = c; i++; continue; }
        if (c == ')') { if (par) par--; prev_code = c; if (!par) last_boundary = i + 1; i++; continue; }
        if (c == '{') {
            int parent_exec = depth == 0 ? 1 : (depth <= 256 ? exec_stack[depth - 1] : 0);
            int opener_ok = 0;
            if (depth == 0) {
                opener_ok = (prev_code == ')');
            } else if (prev_code == ')' || prev_code == ';' || prev_code == '{' ||
                       prev_code == '}' || prev_code == ':') {
                opener_ok = 1;
            } else if (prev_ident_b > prev_ident_a) {
                size_t l = prev_ident_b - prev_ident_a;
                if ((l == 4 && memcmp(s + prev_ident_a, "else", 4) == 0) ||
                    (l == 2 && memcmp(s + prev_ident_a, "do", 2) == 0)) opener_ok = 1;
            }
            if (depth < 256) exec_stack[depth] = (unsigned char)(parent_exec && opener_ok);
            depth++;
            prev_code = c;
            last_boundary = i + 1;
            i++;
            continue;
        }
        if (c == '}') { if (depth) depth--; prev_code = c; last_boundary = i + 1; i++; continue; }
        if (c == ';' || c == ':') { prev_code = c; if (!par) last_boundary = i + 1; i++; continue; }
        if (cc_is_ident_start(c)) {
            size_t a = i, b = i;
            while (b < n && cc_is_ident_char(s[b])) b++;
            /* Candidate declaration statement start? */
            int in_exec = depth > 0 && depth <= 256 && exec_stack[depth - 1];
            size_t stmt_a = last_boundary;
            while (stmt_a < a && isspace((unsigned char)s[stmt_a])) stmt_a++;
            if (in_exec && par == 0 && stmt_a == a) {
                int vi = cc__va_find(s + a, b - a);
                if (vi >= 0 && g_va[vi].has_drop) {
                    /* Next token must be a declarator ident (value decl). */
                    size_t p = cc_skip_ws_and_comments(s, n, b);
                    if (p < n && cc_is_ident_start(s[p])) {
                        size_t da = p, db = p;
                        while (db < n && cc_is_ident_char(s[db])) db++;
                        size_t f = cc_skip_ws_and_comments(s, n, db);
                        /* Skip prototypes/calls/arrays; take `=` or `;`. */
                        /* The transition step (cc__va_step_transitions) emits a
                           move-through temp `__cc_vtN` that is consumed by the
                           immediately following `(lhs) = __cc_vtN;`.  Its bytes
                           alias the LHS payload after that copy, so a scope-exit
                           drop here would free the LHS's live arm (double-free).
                           Never inject a drop for it. */
                        int is_transition_temp =
                            (db - da) >= 7 && memcmp(s + da, "__cc_vt", 7) == 0;
                        if (f < n && (s[f] == '=' || s[f] == ';') && !is_transition_temp) {
                            size_t semi = 0;
                            if (cc__va_find_semi(s, n, db, &semi) &&
                                !cc__va_returned_by_value(s, n, semi + 1, s + da, db - da)) {
                                char* repl = NULL;
                                size_t rl = 0, rc = 0;
                                cc_sb_append_fmt(&repl, &rl, &rc,
                                                 "; @defer { %s__cc_drop(&%.*s); };",
                                                 g_va[vi].name, (int)(db - da), s + da);
                                if (!repl || cc__va_edit_add(ed, semi, semi + 1, repl) != 0) nerr++;
                            }
                        }
                    }
                }
            }
            prev_ident_a = a;
            prev_ident_b = b;
            prev_code = 'A';
            i = b;
            continue;
        }
        if (!isspace((unsigned char)c)) prev_code = c;
        i++;
        continue;
    }
    return nerr ? -1 : 0;
}

/* ==================================================================== */
/* Uses step: packed raw-access guard (spec §11)                         */
/* ==================================================================== */

/* Ban user-written raw `.u` on every registered tagged union ( @variant and
 * schema `one of`) BEFORE the projection step lowers protected arm reads to
 * `.u.arm` (so compiler-generated `.u` never collides with this diagnosis).
 * Schema fill/write helpers are exempt via codegen body spans.  `.kind`
 * writes are diagnosed by the projection step. */
static int cc__va_step_packed_raw_access(const char* s, size_t n, const char* path, CCVaEdits* ed) {
    (void)ed;
    int nerr = 0;
    if (g_va_n <= 0) return 0;
    CCVaCgSpan cg[CC_VA_MAX_CG_SPANS];
    int ncg = cc__va_collect_codegen_spans(s, n, cg, CC_VA_MAX_CG_SPANS);

    CCInertScan sc;
    cc_inert_scan_init(&sc, NULL);
    size_t i = 0;
    while (i < n) {
        if (cc_inert_scan_step(&sc, s, n, &i)) continue;
        char c = s[i];
        size_t acc, mem_a;
        int is_arrow = 0;
        if (c == '.') {
            size_t q = cc__va_back_ws(s, i);
            if (!(q > 0 && (cc_is_ident_char(s[q - 1]) || s[q - 1] == ')' || s[q - 1] == ']'))) { i++; continue; }
            if (i + 1 >= n || !cc_is_ident_start(s[i + 1])) { i++; continue; }
            acc = i; mem_a = i + 1;
        } else if (c == '-' && i + 1 < n && s[i + 1] == '>') {
            if (i + 2 >= n || !cc_is_ident_start(s[i + 2])) { i += 2; continue; }
            acc = i; is_arrow = 1; mem_a = i + 2;
        } else { i++; continue; }
        size_t mem_b = mem_a;
        while (mem_b < n && cc_is_ident_char(s[mem_b])) mem_b++;
        if (!(mem_b - mem_a == 1 && s[mem_a] == 'u')) { i = mem_b; continue; }
        size_t ba, ra, rb;
        int hops = 0;
        if (!cc__va_member_base(s, acc, &ba, &ra, &rb, &hops) || hops != 0) { i = mem_b; continue; }
        int form = 0;
        int vi = cc__va_resolve_root(s, n, ba, s + ra, rb - ra, &form);
        if (vi < 0 || form == CC_VA_FORM_KIND) { i = mem_b; continue; }
        /* Compiler-emitted drop/accessors use `__cc_*` temps (`__cc_vp->u.arm`);
         * those are not user raw reach-in. */
        if (rb > ra + 4 && strncmp(s + ra, "__cc_", 5) == 0) { i = mem_b; continue; }
        if (cc__va_in_cg(acc, cg, ncg)) { i = mem_b; continue; }
        if (cc__va_is_packed(vi)) {
            cc__va_err(s, n, path, acc,
                       "variant '%s' is @variant(packed): it has no exposed '.u' union — project an arm ('%.*s%s<arm>', protected by a kind check / '!>' / '?>') instead of reaching into the raw representation",
                       g_va[vi].name, (int)(rb - ra), s + ra, is_arrow ? "->" : ".");
        } else {
            cc__va_err(s, n, path, acc,
                       "cannot reach into %s '%s' via '.u' — project an arm ('%.*s%s<arm>', protected by a kind check / '!>' / '?>') or use '@unsafe'",
                       cc__va_surface(&g_va[vi]), g_va[vi].name,
                       (int)(rb - ra), s + ra, is_arrow ? "->" : ".");
        }
        nerr++;
        i = mem_b;
    }
    return nerr ? -1 : 0;
}

/* ==================================================================== */
/* Uses step: packed materialization (spec §11)                          */
/* ==================================================================== */

/* Final pass for @variant(packed): the earlier steps produce the canonical
 * layout-agnostic forms (`v.kind`, `v.u.arm`, tag constants) that the honest
 * unpacked struct exposes as members.  A packed variant is an opaque byte
 * blob with no such members, so those forms are rewritten:
 *   (base).kind / (base)->kind   -> Name__cc_kind(&(base) / base)
 *   (base).u.arm / (base)->u.arm -> ((Name__cc_ov_arm*)&(base) / base)->arm
 * The overlay-cast form is an LVALUE at offset 0 whose receiver ends in a
 * plain member, so a single spelling covers reads, address-of (`&v->arm`),
 * in-place assignment (`v->arm.field = x`) and UFCS method calls
 * (`v->arm.method()`) — everything the unpacked union member supported
 * (spec §11).  Only bases that resolve to a PROVEN-packed variant are
 * touched; unpacked variants keep their real members.  Runs once, last, and
 * emits no new `.kind`/`.u` text, so a single pass suffices. */
static int cc__va_step_packed_materialize(const char* s, size_t n, const char* path, CCVaEdits* ed) {
    (void)path;
    int nerr = 0;
    /* Only run if some packed variant exists. */
    int any_packed = 0;
    for (int v = 0; v < g_va_n; v++) if (cc__va_is_packed(v)) any_packed = 1;
    if (!any_packed) return 0;

    CCInertScan sc;
    cc_inert_scan_init(&sc, NULL);
    size_t i = 0;
    while (i < n) {
        if (cc_inert_scan_step(&sc, s, n, &i)) continue;
        char c = s[i];
        size_t acc, mem_a;
        int is_arrow = 0;
        if (c == '.') {
            size_t q = cc__va_back_ws(s, i);
            if (!(q > 0 && (cc_is_ident_char(s[q - 1]) || s[q - 1] == ')' || s[q - 1] == ']'))) { i++; continue; }
            if (i + 1 >= n || !cc_is_ident_start(s[i + 1])) { i++; continue; }
            acc = i; mem_a = i + 1;
        } else if (c == '-' && i + 1 < n && s[i + 1] == '>') {
            if (i + 2 >= n || !cc_is_ident_start(s[i + 2])) { i += 2; continue; }
            acc = i; is_arrow = 1; mem_a = i + 2;
        } else { i++; continue; }
        size_t mem_b = mem_a;
        while (mem_b < n && cc_is_ident_char(s[mem_b])) mem_b++;
        size_t mlen = mem_b - mem_a;
        int is_kind = (mlen == 4 && memcmp(s + mem_a, "kind", 4) == 0);
        int is_u = (mlen == 1 && s[mem_a] == 'u');
        if (!is_kind && !is_u) { i = mem_b; continue; }

        size_t ba, ra, rb;
        int hops = 0;
        if (!cc__va_member_base(s, acc, &ba, &ra, &rb, &hops) || hops != 0) { i = mem_b; continue; }
        int form = 0;
        int vi = cc__va_resolve_root(s, n, ba, s + ra, rb - ra, &form);
        if (vi < 0 || form == CC_VA_FORM_KIND || !cc__va_is_packed(vi)) { i = mem_b; continue; }
        const CCVaDef* def = &g_va[vi];

        if (is_kind) {
            /* Skip an lvalue write (already diagnosed by the projections step). */
            size_t f = cc_skip_ws_and_comments(s, n, mem_b);
            if (f < n && s[f] == '=' && (f + 1 >= n || s[f + 1] != '=')) { i = mem_b; continue; }
            char* repl = NULL;
            size_t rl = 0, rc = 0;
            cc_sb_append_fmt(&repl, &rl, &rc, "%s__cc_kind(", def->name);
            if (!is_arrow) cc_sb_append_cstr(&repl, &rl, &rc, "&(");
            cc__va_append_flat(&repl, &rl, &rc, s, ba, acc);
            cc_sb_append_cstr(&repl, &rl, &rc, is_arrow ? ")" : "))");
            if (!repl || cc__va_edit_add(ed, ba, mem_b, repl) != 0) nerr++;
            i = mem_b;
            continue;
        }
        /* is_u: expect `.arm` after `u`. */
        {
            size_t f = cc_skip_ws_and_comments(s, n, mem_b);
            if (f >= n || s[f] != '.') { i = mem_b; continue; }
            size_t aa = cc_skip_ws_and_comments(s, n, f + 1);
            size_t ab = aa;
            while (ab < n && cc_is_ident_char(s[ab])) ab++;
            if (ab == aa) { i = mem_b; continue; }
            int ai = cc__va_arm_index(vi, s + aa, ab - aa);
            if (ai < 0) { i = ab; continue; }
            /* Overlay-cast lvalue: `((Name__cc_ov_arm*)PTR)->arm`, where PTR is
             * the base (already a pointer under `->`, address-taken under `.`).
             * One form serves reads, address-of, in-place assignment and UFCS
             * method calls — the payload sits at offset 0 and the block is
             * aligned to the widest arm, so the reinterpret is well-defined. */
            char* repl = NULL;
            size_t rl = 0, rc = 0;
            cc_sb_append_fmt(&repl, &rl, &rc, "((%s__cc_ov_%s*)%s", def->name,
                             def->arms[ai].name, is_arrow ? "(" : "&(");
            cc__va_append_flat(&repl, &rl, &rc, s, ba, acc);
            cc_sb_append_fmt(&repl, &rl, &rc, "))->%s", def->arms[ai].name);
            if (!repl || cc__va_edit_add(ed, ba, ab, repl) != 0) nerr++;
            i = ab;
            continue;
        }
    }
    return nerr ? -1 : 0;
}

/* ==================================================================== */
/* Uses driver                                                           */
/* ==================================================================== */

typedef int (*CCVaStepFn)(const char*, size_t, const char*, CCVaEdits*);

char* cc_rewrite_variant_uses_text(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0 || g_va_n == 0) return NULL;
    /* The projections step iterates to a fixpoint: a `!>` handler / `?>`
     * fallback rewrite embeds its inner text verbatim and skips it in that
     * iteration; the next iteration sees the lowered if/else guards and
     * resolves (or rejects) the inner projections.  Nesting depth bounds
     * the iteration count; 16 is far beyond any sane nesting. */
    static const struct {
        CCVaStepFn fn;
        int iterate;
    } steps[] = {
        { cc__va_step_construct, 0 },
        { cc__va_step_bare_designators, 0 },
        { cc__va_step_switches, 0 },
        { cc__va_step_packed_raw_access, 0 },
        { cc__va_step_projections, 1 },
        { cc__va_step_transitions, 0 },
        { cc__va_step_defers, 0 },
        { cc__va_step_packed_materialize, 0 },
    };
    char* owned = NULL;
    const char* cur = src;
    size_t cn = n;
    cc__va_hits_reset();
    for (size_t st = 0; st < sizeof(steps) / sizeof(steps[0]); st++) {
        int rounds = steps[st].iterate ? 16 : 1;
        for (int r = 0; r < rounds; r++) {
            CCVaEdits ed = {0};
            int rc = steps[st].fn(cur, cn, input_path, &ed);
            if (rc < 0) {
                cc__va_edits_free(&ed);
                free(owned);
                cc__va_hits_reset();
                return (char*)-1;
            }
            if (!ed.n) {
                cc__va_edits_free(&ed);
                break;
            }
            char* nb = cc__va_edits_apply(cur, cn, &ed);
            cc__va_edits_free(&ed);
            if (!nb) {
                free(owned);
                cc__va_hits_reset();
                return (char*)-1;
            }
            free(owned);
            owned = nb;
            cur = owned;
            cn = strlen(owned);
            /* Edits change absolute positions — drop occurrence lists. */
            cc__va_hits_reset();
        }
    }
    cc__va_hits_reset();
    return owned;
}
