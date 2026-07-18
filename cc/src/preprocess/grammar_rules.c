/*
 * Builtin @grammar engine dispatch + the `rules` engine: a Rebol/Red
 * PARSE-inspired recognition dialect compiled to specialized C.
 *
 * v2 (this file): full dialect parse -> IR -> NAIVE but CORRECT emitter,
 * recognition (`cc_match` layer) plus span collection (`cc_collect` layer).
 * KEEP semantics: `keep term` logs the child's matched span [start,end) tagged
 * with the ENCLOSING RULE's index (exported as #define <Name>_KEEP_<rule>).
 * The log is coupled to the cursor: every backtracking construct restores
 * nlog alongside p, so keeps from failed branches are never observable. On a
 * successful full match, <Name>_collect replays the log through the caller's
 * callback (the C lowering of the CC closure surface in spec/cc_serdes.md
 * "Streaming Consumption"; a nonzero callback return stops and fails the
 * collect). Log-then-replay today; immediate flush at committed positions is
 * a planned optimization via the FIRST-set analysis, with no API change.
 * The emitted code is a set of mutually recursive matchers, one per rule:
 *
 *     static int <Name>__r_<rule>(<Name>__ctx* c, const unsigned char* s, size_t n, size_t* io);
 *     static int <Name>_match(const char* s, size_t n);               // recognition
 *     static int <Name>_collect(const char* s, size_t n, cb, env);    // keeps -> cb
 *
 * Dialect surface (v1):
 *     rule:    <alt>                       ; rule ends at next `name:` at depth 0
 *     alt:     seq { '|' seq }             ; PEG ordered choice, backtracks pos
 *     seq:     { term }
 *     term:    'some' term | 'any' term | 'opt' term      ; repetition (PEG greedy)
 *            | 'keep' term                                ; log matched span (v2)
 *            | 'collect' term                             ; transparent (entry generated always)
 *            | 'skip'                                     ; consume one byte
 *            | 'charset' '[' items ']'                    ; byte set (bitmap)
 *            | 'complement' charset-term                  ; inverted byte set
 *            | '"' bytes '"'                              ; literal
 *            | #'c'                                       ; char literal
 *            | '[' alt ']'                                ; group
 *            | ident                                      ; rule reference
 *     items:   { #'a' | #'a' - #'z' }                     ; chars and ranges
 *     ';'      line comment
 *
 * Semantics: PEG. Ordered choice restores the cursor on branch failure;
 * `any`/`some` are greedy with an empty-match guard (a child that succeeds
 * without consuming ends the loop, so `any [any ws]` cannot hang). No arena or
 * output yet — v1 is recognition-only, which is exactly the `cc_match` layer.
 * v4 perf analyses (both grammar-derived, semantics-preserving):
 *   - FIRST-set dispatch: alternations whose branches are non-nullable with
 *     pairwise-disjoint FIRST byte sets compile to a lookahead switch with the
 *     trial cascade deleted (identical to PEG order by disjointness). At most
 *     one large-FIRST branch becomes the default arm.
 *   - Charset-run scans: any/some over a plain charset emits a dedicated
 *     unfailing loop (no label scaffold); large sets additionally get a SWAR
 *     8-bytes/word skip when the stop set fits "bytes < T plus <= 2 specials"
 *     (safe over-approximation: the byte-exact loop stays authoritative).
 * Acceptance target: examples/serdes/json/json.h (golden).
 *
 * Design decisions (locked):
 *   - WHOLE BUFFER: end of input is plain match failure; no suspend/resume.
 *     Streaming falls out of the @async state-machine lowering later ("need
 *     more bytes" becomes an await point) without touching the grammar.
 *   - NO ARENA ROLLBACK: cursor rollback only. Under request-arena discipline
 *     (reset wholesale between parses) nodes allocated by a failed speculative
 *     branch are bounded waste until reset, never a correctness issue — a
 *     failed branch's output is simply never linked. Arena checkpointing is a
 *     later optimization for alternation-heavy grammars, not a semantic need.
 *
 * Left recursion is not detected in v1 (PEG discipline; it would recurse at
 * runtime). Unknown rule references fail at C-compile time with a clear
 * undefined-function error naming <Name>__r_<rule>.
 */
#include "preprocess/grammar_engine.h"
#include "util/text.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ IR ---- */

enum { RN_CHARSET, RN_LIT, RN_SEQ, RN_ALT, RN_SOME, RN_ANY, RN_OPT, RN_REF, RN_SKIP, RN_KEEP };

enum {
    R_MAX_NODES = 1024,
    R_MAX_KIDS  = 2048,
    R_MAX_RULES = 128,
    R_MAX_SETS  = 128,
    R_MAX_POOL  = 16384,
    R_NAME_MAX  = 64
};

typedef struct {
    int kind;
    int a;       /* SOME/ANY/OPT: child. CHARSET: set idx. LIT/REF: pool offset. */
    int b;       /* LIT/REF: byte length. SEQ/ALT: kids start. */
    int nkids;   /* SEQ/ALT: child count. */
    size_t at;   /* body offset, for diagnostics */
} RNode;

typedef struct {
    const char* body; size_t n; size_t pos;
    const char* file; int line0;
    const char* name;                     /* declared grammar Name */
    char err[512]; size_t err_at; int failed;

    RNode nodes[R_MAX_NODES]; int nnodes;
    int kids[R_MAX_KIDS]; int nkids;
    unsigned char sets[R_MAX_SETS][32]; int nsets;
    char pool[R_MAX_POOL]; int npool;
    struct { char name[R_NAME_MAX]; int node; size_t at; } rules[R_MAX_RULES];
    int nrules;
    char codecs[16][R_NAME_MAX]; int ncodecs;   /* keep/decode(fn) codec names */
} RG;

static int rg_line_at(const RG* g, size_t at) {
    int line = g->line0;
    for (size_t i = 0; i < at && i < g->n; i++)
        if (g->body[i] == '\n') line++;
    return line;
}

static int rg_fail(RG* g, size_t at, const char* msg) {
    if (!g->failed) {
        snprintf(g->err, sizeof(g->err), "@grammar(rules) %s: %s", g->name, msg);
        g->err_at = at;
        g->failed = 1;
    }
    return -1;
}

static int rg_node(RG* g, int kind, size_t at) {
    if (g->nnodes >= R_MAX_NODES) return rg_fail(g, at, "grammar too large (node limit)");
    RNode* nd = &g->nodes[g->nnodes];
    memset(nd, 0, sizeof(*nd));
    nd->kind = kind; nd->at = at;
    return g->nnodes++;
}

static int rg_pool_add(RG* g, const char* bytes, int len, size_t at) {
    if (g->npool + len + 1 > R_MAX_POOL) return rg_fail(g, at, "grammar too large (literal pool)");
    int off = g->npool;
    memcpy(g->pool + off, bytes, (size_t)len);
    g->pool[off + len] = '\0';
    g->npool += len + 1;
    return off;
}

/* ------------------------------------------------------------- lexing ---- */

static size_t rg_ws(RG* g, size_t p) {
    for (;;) {
        while (p < g->n && (g->body[p] == ' ' || g->body[p] == '\t' ||
                            g->body[p] == '\n' || g->body[p] == '\r'))
            p++;
        if (p < g->n && g->body[p] == ';') {
            while (p < g->n && g->body[p] != '\n') p++;
            continue;
        }
        return p;
    }
}

static int rg_kw(const RG* g, size_t p, const char* kw, size_t* after) {
    size_t k = strlen(kw);
    if (p + k > g->n || memcmp(g->body + p, kw, k) != 0) return 0;
    if (p + k < g->n && cc_is_ident_char(g->body[p + k])) return 0;
    if (after) *after = p + k;
    return 1;
}

static size_t rg_ident(const RG* g, size_t p, size_t* end) {
    if (p >= g->n || !cc_is_ident_start(g->body[p])) return 0;
    size_t e = p;
    while (e < g->n && cc_is_ident_char(g->body[e])) e++;
    *end = e;
    return 1;
}

/* One escaped-or-plain char inside a quoted form. Returns 1 and advances. */
static int rg_escchar(RG* g, size_t* io, unsigned char* out) {
    size_t p = *io;
    if (p >= g->n) return 0;
    unsigned char c = (unsigned char)g->body[p];
    if (c == '\\') {
        if (p + 1 >= g->n) return 0;
        unsigned char e = (unsigned char)g->body[p + 1];
        switch (e) {
        case 'n': *out = '\n'; break;
        case 't': *out = '\t'; break;
        case 'r': *out = '\r'; break;
        case '0': *out = '\0'; break;
        case '\\': case '\'': case '"': *out = e; break;
        default: return 0;
        }
        *io = p + 2;
        return 1;
    }
    *out = c;
    *io = p + 1;
    return 1;
}

/* #'c' (single char literal). */
static int rg_charlit(RG* g, size_t* io, unsigned char* out) {
    size_t p = *io;
    if (p + 2 >= g->n || g->body[p] != '#' || g->body[p + 1] != '\'') return 0;
    p += 2;
    if (!rg_escchar(g, &p, out)) { rg_fail(g, p, "bad character escape in #'...'"); return 0; }
    if (p >= g->n || g->body[p] != '\'') { rg_fail(g, p, "expected closing ' in #'...'"); return 0; }
    *io = p + 1;
    return 1;
}

/* ------------------------------------------------------------- parsing ---- */

static int rg_parse_alt(RG* g, size_t* io, int depth);

/* charset [ items ] — caller consumed the `charset` keyword. */
static int rg_parse_charset(RG* g, size_t* io, int complement) {
    size_t p = rg_ws(g, *io);
    size_t at = p;
    if (p >= g->n || g->body[p] != '[') return rg_fail(g, p, "expected '[' after charset");
    p = rg_ws(g, p + 1);
    if (g->nsets >= R_MAX_SETS) return rg_fail(g, p, "too many charsets");
    unsigned char* set = g->sets[g->nsets];
    memset(set, 0, 32);
    while (p < g->n && g->body[p] != ']') {
        unsigned char lo, hi;
        if (!rg_charlit(g, &p, &lo))
            return g->failed ? -1 : rg_fail(g, p, "expected #'c' item in charset");
        hi = lo;
        p = rg_ws(g, p);
        if (p < g->n && g->body[p] == '-') {
            p = rg_ws(g, p + 1);
            if (!rg_charlit(g, &p, &hi))
                return g->failed ? -1 : rg_fail(g, p, "expected #'c' after '-' in charset range");
            if (hi < lo) return rg_fail(g, p, "charset range is descending");
            p = rg_ws(g, p);
        }
        for (unsigned v = lo; v <= (unsigned)hi; v++) set[v >> 3] |= (unsigned char)(1u << (v & 7));
    }
    if (p >= g->n) return rg_fail(g, at, "unterminated charset [...]");
    p++;   /* ']' */
    if (complement)
        for (int i = 0; i < 32; i++) set[i] = (unsigned char)~set[i];
    int idx = g->nsets++;
    int nd = rg_node(g, RN_CHARSET, at);
    if (nd < 0) return -1;
    g->nodes[nd].a = idx;
    *io = p;
    return nd;
}

static int rg_parse_term(RG* g, size_t* io, int depth) {
    size_t p = rg_ws(g, *io);
    size_t at = p, after = 0;
    if (p >= g->n) return rg_fail(g, p, "unexpected end of grammar");

    if (g->body[p] == '[') {
        p++;
        int nd = rg_parse_alt(g, &p, depth + 1);
        if (nd < 0) return -1;
        p = rg_ws(g, p);
        if (p >= g->n || g->body[p] != ']') return rg_fail(g, at, "expected ']' closing group");
        *io = p + 1;
        return nd;
    }
    if (g->body[p] == '"') {
        unsigned char buf[512]; int blen = 0;
        p++;
        while (p < g->n && g->body[p] != '"') {
            unsigned char c;
            if (!rg_escchar(g, &p, &c)) return rg_fail(g, p, "bad escape in string literal");
            if (blen >= (int)sizeof(buf)) return rg_fail(g, at, "string literal too long");
            buf[blen++] = c;
        }
        if (p >= g->n) return rg_fail(g, at, "unterminated string literal");
        p++;
        if (blen == 0) return rg_fail(g, at, "empty string literal");
        int off = rg_pool_add(g, (const char*)buf, blen, at);
        if (off < 0) return -1;
        int nd = rg_node(g, RN_LIT, at);
        if (nd < 0) return -1;
        g->nodes[nd].a = off; g->nodes[nd].b = blen;
        *io = p;
        return nd;
    }
    if (g->body[p] == '#') {
        unsigned char c;
        if (!rg_charlit(g, &p, &c)) return g->failed ? -1 : rg_fail(g, p, "expected #'c'");
        int off = rg_pool_add(g, (const char*)&c, 1, at);
        if (off < 0) return -1;
        int nd = rg_node(g, RN_LIT, at);
        if (nd < 0) return -1;
        g->nodes[nd].a = off; g->nodes[nd].b = 1;
        *io = p;
        return nd;
    }
    if (rg_kw(g, p, "some", &after) || rg_kw(g, p, "any", &after) || rg_kw(g, p, "opt", &after)) {
        int kind = g->body[p] == 's' ? RN_SOME : (g->body[p] == 'a' ? RN_ANY : RN_OPT);
        p = after;
        int child = rg_parse_term(g, &p, depth);
        if (child < 0) return -1;
        int nd = rg_node(g, kind, at);
        if (nd < 0) return -1;
        g->nodes[nd].a = child;
        *io = p;
        return nd;
    }
    if (rg_kw(g, p, "keep", &after)) {
        int codec = 0;   /* 0 = raw borrow; k+1 = codec index k */
        p = after;
        if (p < g->n && g->body[p] == '/') {
            p++;
            if (!rg_kw(g, p, "decode", &after))
                return rg_fail(g, p, "expected keep/decode(codec)");
            p = rg_ws(g, after);
            if (p >= g->n || g->body[p] != '(')
                return rg_fail(g, p, "expected '(' after keep/decode");
            p = rg_ws(g, p + 1);
            size_t ce;
            if (!rg_ident(g, p, &ce) || ce - p >= R_NAME_MAX)
                return rg_fail(g, p, "expected codec function name");
            {
                char cn[R_NAME_MAX];
                memcpy(cn, g->body + p, ce - p); cn[ce - p] = '\0';
                int idx = -1;
                for (int i = 0; i < g->ncodecs; i++)
                    if (strcmp(g->codecs[i], cn) == 0) { idx = i; break; }
                if (idx < 0) {
                    if (g->ncodecs >= (int)(sizeof(g->codecs) / sizeof(g->codecs[0])))
                        return rg_fail(g, p, "too many distinct codecs");
                    idx = g->ncodecs++;
                    strcpy(g->codecs[idx], cn);
                }
                codec = idx + 1;
            }
            p = rg_ws(g, ce);
            if (p >= g->n || g->body[p] != ')')
                return rg_fail(g, p, "expected ')' closing keep/decode(codec)");
            p++;
        }
        int child = rg_parse_term(g, &p, depth);
        if (child < 0) return -1;
        int nd = rg_node(g, RN_KEEP, at);
        if (nd < 0) return -1;
        g->nodes[nd].a = child;
        g->nodes[nd].b = codec;
        *io = p;
        return nd;
    }
    if (rg_kw(g, p, "collect", &after)) {
        /* transparent: the collect entry point is generated unconditionally */
        p = after;
        int child = rg_parse_term(g, &p, depth);
        if (child < 0) return -1;
        *io = p;
        return child;
    }
    if (rg_kw(g, p, "skip", &after)) {
        int nd = rg_node(g, RN_SKIP, at);
        if (nd < 0) return -1;
        *io = after;
        return nd;
    }
    if (rg_kw(g, p, "charset", &after)) {
        p = after;
        int nd = rg_parse_charset(g, &p, 0);
        if (nd < 0) return -1;
        *io = p;
        return nd;
    }
    if (rg_kw(g, p, "complement", &after)) {
        p = rg_ws(g, after);
        if (!rg_kw(g, p, "charset", &after)) return rg_fail(g, p, "complement must be followed by charset");
        p = after;
        int nd = rg_parse_charset(g, &p, 1);
        if (nd < 0) return -1;
        *io = p;
        return nd;
    }
    if (rg_kw(g, p, "to", &after) || rg_kw(g, p, "thru", &after))
        return rg_fail(g, p, "to/thru not supported yet (v1)");

    {
        size_t e;
        if (rg_ident(g, p, &e)) {
            if (e - p >= R_NAME_MAX) return rg_fail(g, p, "identifier too long");
            int off = rg_pool_add(g, g->body + p, (int)(e - p), at);
            if (off < 0) return -1;
            int nd = rg_node(g, RN_REF, at);
            if (nd < 0) return -1;
            g->nodes[nd].a = off; g->nodes[nd].b = (int)(e - p);
            *io = e;
            return nd;
        }
    }
    return rg_fail(g, p, "unrecognized term");
}

/* Is `p` (already ws-skipped) sitting on the next rule header `ident :` ? */
static int rg_at_rule_header(const RG* g, size_t p) {
    size_t e;
    if (!rg_ident((RG*)g, p, &e)) return 0;
    size_t q = e;
    while (q < g->n && (g->body[q] == ' ' || g->body[q] == '\t')) q++;
    return q < g->n && g->body[q] == ':';
}

static int rg_parse_seq(RG* g, size_t* io, int depth) {
    size_t p = *io;
    size_t at = rg_ws(g, p);
    int local[64]; int nlocal = 0;
    for (;;) {
        p = rg_ws(g, p);
        if (p >= g->n) break;
        if (g->body[p] == ']' || g->body[p] == '|') break;
        if (depth == 0 && rg_at_rule_header(g, p)) break;
        int nd = rg_parse_term(g, &p, depth);
        if (nd < 0) return -1;
        if (nlocal >= (int)(sizeof(local) / sizeof(local[0])))
            return rg_fail(g, p, "sequence too long");
        local[nlocal++] = nd;
    }
    if (nlocal == 0) return rg_fail(g, at, "empty sequence");
    if (nlocal == 1) { *io = p; return local[0]; }
    if (g->nkids + nlocal > R_MAX_KIDS) return rg_fail(g, at, "grammar too large (kid limit)");
    int nd = rg_node(g, RN_SEQ, at);
    if (nd < 0) return -1;
    g->nodes[nd].b = g->nkids;
    g->nodes[nd].nkids = nlocal;
    for (int i = 0; i < nlocal; i++) g->kids[g->nkids++] = local[i];
    *io = p;
    return nd;
}

static int rg_parse_alt(RG* g, size_t* io, int depth) {
    size_t p = *io;
    size_t at = rg_ws(g, p);
    int local[32]; int nlocal = 0;
    for (;;) {
        int nd = rg_parse_seq(g, &p, depth);
        if (nd < 0) return -1;
        if (nlocal >= (int)(sizeof(local) / sizeof(local[0])))
            return rg_fail(g, p, "too many alternatives");
        local[nlocal++] = nd;
        p = rg_ws(g, p);
        if (p < g->n && g->body[p] == '|') { p++; continue; }
        break;
    }
    if (nlocal == 1) { *io = p; return local[0]; }
    if (g->nkids + nlocal > R_MAX_KIDS) return rg_fail(g, at, "grammar too large (kid limit)");
    int nd = rg_node(g, RN_ALT, at);
    if (nd < 0) return -1;
    g->nodes[nd].b = g->nkids;
    g->nodes[nd].nkids = nlocal;
    for (int i = 0; i < nlocal; i++) g->kids[g->nkids++] = local[i];
    *io = p;
    return nd;
}

static int rg_parse(RG* g) {
    size_t p = 0;
    for (;;) {
        p = rg_ws(g, p);
        if (p >= g->n) break;
        size_t e;
        if (!rg_ident(g, p, &e)) return rg_fail(g, p, "expected rule name");
        size_t q = rg_ws(g, e);
        if (q >= g->n || g->body[q] != ':') return rg_fail(g, p, "expected ':' after rule name");
        if (e - p >= R_NAME_MAX) return rg_fail(g, p, "rule name too long");
        if (g->nrules >= R_MAX_RULES) return rg_fail(g, p, "too many rules");
        for (int i = 0; i < g->nrules; i++)
            if (strlen(g->rules[i].name) == e - p &&
                memcmp(g->rules[i].name, g->body + p, e - p) == 0)
                return rg_fail(g, p, "duplicate rule name");
        memcpy(g->rules[g->nrules].name, g->body + p, e - p);
        g->rules[g->nrules].name[e - p] = '\0';
        g->rules[g->nrules].at = p;
        q++;
        int nd = rg_parse_alt(g, &q, 0);
        if (nd < 0) return -1;
        g->rules[g->nrules].node = nd;
        g->nrules++;
        p = q;
    }
    if (g->nrules == 0) return rg_fail(g, 0, "no rules declared");
    /* Resolve references now so undefined names fail here, not in emitted C. */
    for (int i = 0; i < g->nnodes; i++) {
        if (g->nodes[i].kind != RN_REF) continue;
        const char* rn = g->pool + g->nodes[i].a;
        int found = -1;
        for (int r = 0; r < g->nrules; r++)
            if (strcmp(g->rules[r].name, rn) == 0) { found = r; break; }
        if (found < 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "reference to undefined rule '%s'", rn);
            return rg_fail(g, g->nodes[i].at, msg);
        }
        g->nodes[i].nkids = found;   /* resolved rule index */
    }
    return 0;
}

/* ------------------------------------------------------- FIRST analysis ---- */
/* FIRST(node) = set of bytes that can begin a match; nullable = can match
 * empty. Rule-level memo with an in-progress state that returns the
 * conservative answer (all bytes, nullable) on cycles — which simply disables
 * dispatch there. Used to compile disjoint alternations into first-byte
 * switch dispatch with NO backtracking: if the lookahead byte is outside a
 * branch's FIRST set the branch can only fail, and with pairwise-disjoint
 * branches at most one can match, so choosing by byte is exactly PEG order. */

typedef struct {
    unsigned char set[R_MAX_RULES][32];
    unsigned char nullable[R_MAX_RULES];
    unsigned char state[R_MAX_RULES];   /* 0 unvisited, 1 in progress, 2 done */
} RFirst;

static void rf_union(unsigned char* dst, const unsigned char* src) {
    for (int i = 0; i < 32; i++) dst[i] |= src[i];
}
static void rf_all(unsigned char* dst) { memset(dst, 0xFF, 32); }
static int rf_popcount(const unsigned char* set) {
    int n = 0;
    for (int i = 0; i < 256; i++) if (set[i >> 3] & (1u << (i & 7))) n++;
    return n;
}
static int rf_disjoint(const unsigned char* a, const unsigned char* b) {
    for (int i = 0; i < 32; i++) if (a[i] & b[i]) return 0;
    return 1;
}

static void rf_node(const RG* g, RFirst* F, int nd, unsigned char* set, int* nullable);

static void rf_rule(const RG* g, RFirst* F, int r, unsigned char* set, int* nullable) {
    if (F->state[r] == 2) { rf_union(set, F->set[r]); if (F->nullable[r]) *nullable = 1; return; }
    if (F->state[r] == 1) { rf_all(set); *nullable = 1; return; }   /* cycle: conservative */
    F->state[r] = 1;
    unsigned char tmp[32]; memset(tmp, 0, 32); int nul = 0;
    rf_node(g, F, g->rules[r].node, tmp, &nul);
    memcpy(F->set[r], tmp, 32);
    F->nullable[r] = (unsigned char)nul;
    F->state[r] = 2;
    rf_union(set, tmp); if (nul) *nullable = 1;
}

static void rf_node(const RG* g, RFirst* F, int nd, unsigned char* set, int* nullable) {
    const RNode* x = &g->nodes[nd];
    switch (x->kind) {
    case RN_LIT: {
        unsigned char b = (unsigned char)g->pool[x->a];
        set[b >> 3] |= (unsigned char)(1u << (b & 7));
        break;
    }
    case RN_CHARSET: rf_union(set, g->sets[x->a]); break;
    case RN_SKIP: rf_all(set); break;
    case RN_REF: rf_rule(g, F, x->nkids, set, nullable); break;
    case RN_KEEP: rf_node(g, F, x->a, set, nullable); break;
    case RN_OPT: case RN_ANY: {
        int n2 = 0; rf_node(g, F, x->a, set, &n2);
        *nullable = 1;
        break;
    }
    case RN_SOME: rf_node(g, F, x->a, set, nullable); break;
    case RN_SEQ: {
        int i;
        for (i = 0; i < x->nkids; i++) {
            int n2 = 0;
            rf_node(g, F, g->kids[x->b + i], set, &n2);
            if (!n2) break;             /* child can't match empty: stop */
        }
        if (i == x->nkids) *nullable = 1;   /* every child nullable */
        break;
    }
    case RN_ALT: {
        int any_nul = 0;
        for (int i = 0; i < x->nkids; i++) {
            int n2 = 0;
            rf_node(g, F, g->kids[x->b + i], set, &n2);
            if (n2) any_nul = 1;
        }
        if (any_nul) *nullable = 1;
        break;
    }
    }
}

/* Charset-run scan specialization: `any`/`some` over a plain charset is an
 * unfailing greedy run, so it emits as a dedicated loop with no label
 * scaffold. For big sets (complement-style content scans) we additionally
 * emit a SWAR 8-bytes/word skip when the STOP set (complement) fits
 * "every byte < T, plus at most two specific bytes" — an OVER-approximation
 * is safe because the word loop only fast-skips; the byte-exact bitmap loop
 * that follows remains authoritative. This is exactly the golden json.h
 * string-scan shape, derived from the grammar. */
static int rf_swar_stop(const unsigned char* set, int* out_T, int* out_b1, int* out_b2) {
    unsigned char stop[32];
    for (int i = 0; i < 32; i++) stop[i] = (unsigned char)~set[i];
    /* choose smallest T (<= 0x40) covering the low-byte cluster of stops */
    for (int T = 0; T <= 0x40; T++) {
        int extra[3]; int nx = 0;
        for (int b = T; b < 256 && nx <= 2; b++)
            if (stop[b >> 3] & (1u << (b & 7))) { if (nx < 3) extra[nx] = b; nx++; }
        if (nx <= 2) {
            *out_T = T;
            *out_b1 = nx > 0 ? extra[0] : -1;
            *out_b2 = nx > 1 ? extra[1] : -1;
            return 1;
        }
    }
    return 0;
}

/* KEEPS(subtree): can this node's subtree (transitively through rule refs)
 * log a keep? Constructs that provably cannot keep skip the log save/restore
 * entirely — the ctx traffic vanishes from pure-scanning hot loops (ws, digit
 * runs, string content) for BOTH the match and collect entries. Conservative
 * on cycles. */
typedef struct {
    unsigned char state[R_MAX_RULES];   /* 0 unvisited, 1 in progress, 2 done */
    unsigned char keeps[R_MAX_RULES];
} RKeeps;

static int rk_node(const RG* g, RKeeps* K, int nd);

static int rk_rule(const RG* g, RKeeps* K, int r) {
    if (K->state[r] == 2) return K->keeps[r];
    if (K->state[r] == 1) return 1;    /* cycle: conservative */
    K->state[r] = 1;
    int k = rk_node(g, K, g->rules[r].node);
    K->keeps[r] = (unsigned char)k;
    K->state[r] = 2;
    return k;
}

static int rk_node(const RG* g, RKeeps* K, int nd) {
    const RNode* x = &g->nodes[nd];
    switch (x->kind) {
    case RN_KEEP: return 1;
    case RN_REF: return rk_rule(g, K, x->nkids);
    case RN_SOME: case RN_ANY: case RN_OPT: return rk_node(g, K, x->a);
    case RN_SEQ: case RN_ALT:
        for (int i = 0; i < x->nkids; i++)
            if (rk_node(g, K, g->kids[x->b + i])) return 1;
        return 0;
    default: return 0;
    }
}

/* Reference resolution: named rules are the dialect's readability device
 * (`digit`, `strchar`), but a ref to a rule whose whole body is one charset or
 * literal must not cost a function call per element. rg_effective() sees
 * through such refs (with a hop guard), so run specialization and inline
 * emission treat `some digit` exactly like `some charset [...]`. */
static int rg_effective(const RG* g, int nd) {
    int hops = 0;
    while (g->nodes[nd].kind == RN_REF && hops++ < 8) {
        int body = g->rules[g->nodes[nd].nkids].node;
        int k = g->nodes[body].kind;
        if (k == RN_CHARSET || k == RN_LIT) nd = body;
        else break;
    }
    return nd;
}

/* ------------------------------------------------------------- emitter ---- */

typedef struct { char** buf; size_t* len; size_t* cap; RFirst* F; RKeeps* K; } EB;

static void eb_fmt(EB* e, const char* fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) cc_sb_append(e->buf, e->len, e->cap, tmp, (size_t)(n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1));
}

/* Emit statements matching node `nd`; on mismatch: `goto <fail>;` with the
 * cursor restored by the construct that owns <fail>. `p` is the cursor var.
 * `c` is the collect context (NULL under <Name>_match): keeps append spans to
 * its log, and every backtracking construct restores the log length alongside
 * the cursor — so a failed branch's keeps are never observable. `rid` is the
 * enclosing rule index (the keep id). */
static void rg_emit_node(const RG* g, EB* e, int nd, const char* fail, int* lbl, int rid) {
    const RNode* x = &g->nodes[nd];
    switch (x->kind) {
    case RN_LIT:
        if (x->b == 1) {
            eb_fmt(e, "    if (!(p < n && s[p] == %d)) goto %s;\n    p++;\n",
                   (int)(unsigned char)g->pool[x->a], fail);
        } else {
            eb_fmt(e, "    if (!(p + %d <= n && memcmp(s + p, %s__pool + %d, %d) == 0)) goto %s;\n"
                      "    p += %d;\n",
                   x->b, g->name, x->a, x->b, fail, x->b);
        }
        break;
    case RN_CHARSET:
        eb_fmt(e, "    if (!(p < n && (%s__cs[%d][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u))))) goto %s;\n"
                  "    p++;\n",
               g->name, x->a, fail);
        break;
    case RN_SKIP:
        eb_fmt(e, "    if (!(p < n)) goto %s;\n    p++;\n", fail);
        break;
    case RN_REF: {
        int eff = rg_effective(g, nd);
        if (eff != nd) { rg_emit_node(g, e, eff, fail, lbl, rid); break; }
        eb_fmt(e, "    if (!%s__r_%s(c, s, n, &p)) goto %s;\n",
               g->name, g->rules[x->nkids].name, fail);
        break;
    }
    case RN_KEEP: {
        int k = (*lbl)++;
        eb_fmt(e, "    { size_t ka%d = p;\n", k);
        rg_emit_node(g, e, x->a, fail, lbl, rid);
        eb_fmt(e, "    if (c && !%s__push(c, %d, %d, ka%d, p)) goto %s;\n    }\n",
               g->name, rid, x->b, k, fail);
        break;
    }
    case RN_SEQ:
        for (int i = 0; i < x->nkids; i++)
            rg_emit_node(g, e, g->kids[x->b + i], fail, lbl, rid);
        break;
    case RN_ALT: {
        int k = (*lbl)++;
        /* FIRST-set dispatch: if every branch is non-nullable and the branch
         * FIRST sets are pairwise disjoint, choose by lookahead byte — exactly
         * PEG order, with the trial cascade deleted. Branches with small FIRST
         * sets become switch cases; at most one large-set branch becomes the
         * default arm (its own first test re-verifies membership). */
        {
            unsigned char bf[32][32]; int bnul[32]; int ok = x->nkids <= 32;
            for (int i = 0; ok && i < x->nkids; i++) {
                memset(bf[i], 0, 32); bnul[i] = 0;
                rf_node(g, e->F, g->kids[x->b + i], bf[i], &bnul[i]);
                if (bnul[i]) ok = 0;
            }
            for (int i = 0; ok && i < x->nkids; i++)
                for (int j = i + 1; ok && j < x->nkids; j++)
                    if (!rf_disjoint(bf[i], bf[j])) ok = 0;
            int big = -1;
            for (int i = 0; ok && i < x->nkids; i++)
                if (rf_popcount(bf[i]) > 32) { if (big >= 0) ok = 0; else big = i; }
            if (ok) {
                int kp = rk_node(g, e->K, nd);
                if (kp) eb_fmt(e, "    { size_t sv%d = p; size_t lv%d = c ? c->nlog : 0;\n", k, k);
                else    eb_fmt(e, "    { size_t sv%d = p;\n", k);
                eb_fmt(e, "    if (p >= n) goto Lb%d;\n", k);
                eb_fmt(e, "    switch (s[p]) {\n", k);
                for (int i = 0; i < x->nkids; i++) {
                    if (i == big) continue;
                    for (int bch = 0; bch < 256; bch++)
                        if (bf[i][bch >> 3] & (1u << (bch & 7)))
                            eb_fmt(e, "    case %d:\n", bch);
                    {
                        char br[32];
                        snprintf(br, sizeof(br), "Lb%d", k);
                        rg_emit_node(g, e, g->kids[x->b + i], br, lbl, rid);
                    }
                    eb_fmt(e, "    break;\n");
                }
                if (big >= 0) {
                    eb_fmt(e, "    default:\n");
                    {
                        char br[32];
                        snprintf(br, sizeof(br), "Lb%d", k);
                        rg_emit_node(g, e, g->kids[x->b + big], br, lbl, rid);
                    }
                    eb_fmt(e, "    break;\n");
                } else {
                    eb_fmt(e, "    default: goto Lb%d;\n", k);
                }
                eb_fmt(e, "    }\n    goto Lok%d;\n", k);
                if (kp) eb_fmt(e, "Lb%d: p = sv%d; if (c) c->nlog = lv%d; goto %s;\n", k, k, k, fail);
                else    eb_fmt(e, "Lb%d: p = sv%d; goto %s;\n", k, k, fail);
                eb_fmt(e, "Lok%d: ; }\n", k);
                break;
            }
        }
        /* fallback: PEG trial cascade */
        {
        int kp = rk_node(g, e->K, nd);
        if (kp) eb_fmt(e, "    { size_t sv%d = p; size_t lv%d = c ? c->nlog : 0;\n", k, k);
        else    eb_fmt(e, "    { size_t sv%d = p;\n", k);
        for (int i = 0; i < x->nkids; i++) {
            char br[32];
            int last = (i == x->nkids - 1);
            snprintf(br, sizeof(br), "La%d_%d", k, i);
            rg_emit_node(g, e, g->kids[x->b + i], br, lbl, rid);
            eb_fmt(e, "    goto Lok%d;\n", k);
            if (kp) eb_fmt(e, "%s: p = sv%d; if (c) c->nlog = lv%d;\n", br, k, k);
            else    eb_fmt(e, "%s: p = sv%d;\n", br, k);
            if (last) eb_fmt(e, "    goto %s;\n", fail);
        }
        eb_fmt(e, "Lok%d: ; }\n", k);
        }
        break;
    }
    case RN_OPT: {
        int k = (*lbl)++;
        int kp = rk_node(g, e->K, x->a);
        char br[32];
        snprintf(br, sizeof(br), "Lo%d", k);
        if (kp) eb_fmt(e, "    { size_t sv%d = p; size_t lv%d = c ? c->nlog : 0;\n", k, k);
        else    eb_fmt(e, "    { size_t sv%d = p;\n", k);
        rg_emit_node(g, e, x->a, br, lbl, rid);
        if (kp) eb_fmt(e, "    goto Lok%d;\n%s: p = sv%d; if (c) c->nlog = lv%d;\nLok%d: ; }\n",
                       k, br, k, k, k);
        else    eb_fmt(e, "    goto Lok%d;\n%s: p = sv%d;\nLok%d: ; }\n", k, br, k, k);
        break;
    }
    case RN_ANY:
    case RN_SOME: {
        int eff = rg_effective(g, x->a);
        if (g->nodes[eff].kind == RN_CHARSET) {
            int cs = g->nodes[eff].a;
            if (x->kind == RN_SOME) {   /* first element is required */
                eb_fmt(e, "    if (!(p < n && (%s__cs[%d][s[p] >> 3] & (1u << (s[p] & 7u))))) goto %s;\n"
                          "    p++;\n", g->name, cs, fail);
            }
            {
                int T, b1, b2;
                if (rf_popcount(g->sets[cs]) >= 64 && rf_swar_stop(g->sets[cs], &T, &b1, &b2)) {
                    int k2 = (*lbl)++;
                    eb_fmt(e, "    { const unsigned long long L%d = 0x0101010101010101ULL, H%d = 0x8080808080808080ULL;\n",
                           k2, k2);
                    eb_fmt(e, "    while (p + 8 <= n) { unsigned long long w%d; memcpy(&w%d, s + p, 8);\n", k2, k2);
                    eb_fmt(e, "        unsigned long long m%d = 0;\n", k2);
                    if (T > 0)
                        eb_fmt(e, "        m%d |= (w%d - L%d * %d) & ~w%d & H%d;\n", k2, k2, k2, T, k2, k2);
                    if (b1 >= 0)
                        eb_fmt(e, "        { unsigned long long x = w%d ^ (L%d * %d); m%d |= (x - L%d) & ~x & H%d; }\n",
                               k2, k2, b1, k2, k2, k2);
                    if (b2 >= 0)
                        eb_fmt(e, "        { unsigned long long x = w%d ^ (L%d * %d); m%d |= (x - L%d) & ~x & H%d; }\n",
                               k2, k2, b2, k2, k2, k2);
                    eb_fmt(e, "        if (m%d) break;\n        p += 8;\n    } }\n", k2);
                }
            }
            eb_fmt(e, "    while (p < n && (%s__cs[%d][s[p] >> 3] & (1u << (s[p] & 7u)))) p++;\n",
                   g->name, cs);
            break;
        }
        if (x->kind == RN_SOME) {
            /* generic some: child once (required), then greedy any */
            rg_emit_node(g, e, x->a, fail, lbl, rid);
        }
        {
        int k = (*lbl)++;
        int kp = rk_node(g, e->K, x->a);
        char br[32];
        snprintf(br, sizeof(br), "Ly%d", k);
        if (kp) eb_fmt(e, "    { size_t sv%d; size_t lv%d;\n    for (;;) { sv%d = p; lv%d = c ? c->nlog : 0;\n",
                       k, k, k, k);
        else    eb_fmt(e, "    { size_t sv%d;\n    for (;;) { sv%d = p;\n", k, k);
        rg_emit_node(g, e, x->a, br, lbl, rid);
        if (kp) eb_fmt(e, "    if (p == sv%d) break;\n    }\n    goto Lok%d;\n%s: p = sv%d; if (c) c->nlog = lv%d;\nLok%d: ; }\n",
                       k, k, br, k, k, k);
        else    eb_fmt(e, "    if (p == sv%d) break;\n    }\n    goto Lok%d;\n%s: p = sv%d;\nLok%d: ; }\n",
                       k, k, br, k, k);
        break;
        }
    }
    }
}

static char* rg_emit(const RG* g, int origin_line) {
    char* out = NULL; size_t len = 0, cap = 0;
    RFirst* F = (RFirst*)calloc(1, sizeof(RFirst));
    RKeeps* K = (RKeeps*)calloc(1, sizeof(RKeeps));
    EB e = { &out, &len, &cap, F, K };
    int lbl = 0;
    if (!F || !K) { free(F); free(K); return NULL; }

    eb_fmt(&e, "/* generated by @grammar(rules) %s (line %d): %d rule(s), match + collect (v2) */\n",
           g->name, origin_line, g->nrules);
    eb_fmt(&e, "typedef struct { int rule_count; } %s;\n", g->name);
    eb_fmt(&e, "static inline int %s_rule_count(void) { return %d; }\n", g->name, g->nrules);
    /* collect context: span log with cursor-coupled rollback (nlog restores
     * alongside p, so failed-branch keeps are never replayed). */
    eb_fmt(&e, "typedef struct { struct { int id; int codec; size_t a, b; }* log; size_t nlog, cap; } %s__ctx;\n",
           g->name);
    eb_fmt(&e, "static int %s__push(%s__ctx* c, int id, int codec, size_t a, size_t b) {\n"
               "    if (c->nlog == c->cap) {\n"
               "        size_t nc = c->cap ? c->cap * 2 : 64;\n"
               "        void* nl = realloc(c->log, nc * sizeof(*c->log));\n"
               "        if (!nl) return 0;\n"
               "        c->log = nl; c->cap = nc;\n"
               "    }\n"
               "    c->log[c->nlog].id = id; c->log[c->nlog].codec = codec;\n"
               "    c->log[c->nlog].a = a; c->log[c->nlog].b = b;\n"
               "    c->nlog++; return 1;\n}\n", g->name, g->name);
    /* keep ids: the enclosing rule's index, exported per rule containing keeps */
    for (int r = 0; r < g->nrules; r++) {
        /* reachable-keep scan (iterative stack over the rule's subtree) */
        int stack[R_MAX_NODES]; int sp = 0, haskeep = 0;
        stack[sp++] = g->rules[r].node;
        while (sp > 0) {
            const RNode* x = &g->nodes[stack[--sp]];
            if (x->kind == RN_KEEP) { haskeep = 1; break; }
            if (x->kind == RN_SEQ || x->kind == RN_ALT)
                for (int i = 0; i < x->nkids; i++) stack[sp++] = g->kids[x->b + i];
            else if (x->kind == RN_SOME || x->kind == RN_ANY || x->kind == RN_OPT)
                stack[sp++] = x->a;
            /* RN_REF: keeps inside other rules belong to those rules' ids */
        }
        if (haskeep)
            eb_fmt(&e, "#define %s_KEEP_%s %d\n", g->name, g->rules[r].name, r);
    }

    if (g->npool > 0) {
        eb_fmt(&e, "static const unsigned char %s__pool[%d] = {", g->name, g->npool);
        for (int i = 0; i < g->npool; i++) eb_fmt(&e, "%d,", (int)(unsigned char)g->pool[i]);
        eb_fmt(&e, "};\n");
    }
    if (g->nsets > 0) {
        eb_fmt(&e, "static const unsigned char %s__cs[%d][32] = {\n", g->name, g->nsets);
        for (int s = 0; s < g->nsets; s++) {
            eb_fmt(&e, "  {");
            for (int i = 0; i < 32; i++) eb_fmt(&e, "%u,", (unsigned)g->sets[s][i]);
            eb_fmt(&e, "},\n");
        }
        eb_fmt(&e, "};\n");
    }
    for (int r = 0; r < g->nrules; r++)
        eb_fmt(&e, "static int %s__r_%s(%s__ctx* c, const unsigned char* s, size_t n, size_t* io);\n",
               g->name, g->rules[r].name, g->name);

    for (int r = 0; r < g->nrules; r++) {
        eb_fmt(&e, "static int %s__r_%s(%s__ctx* c, const unsigned char* s, size_t n, size_t* io) {\n"
                   "    size_t p = *io;\n    (void)c; (void)s; (void)n;\n",
               g->name, g->rules[r].name, g->name);
        char fail[16];
        snprintf(fail, sizeof(fail), "Lf%d", lbl++);
        rg_emit_node(g, &e, g->rules[r].node, fail, &lbl, r);
        eb_fmt(&e, "    *io = p; return 1;\n%s:\n    return 0;\n}\n", fail);
    }

    /* Entries (first declared rule, full-input):
     *   <Name>_match(s, n)                       recognition only
     *   <Name>_collect(s, n, cb, env)            keeps replayed through cb on success;
     *                                            cb nonzero return stops (returns 0). */
    eb_fmt(&e, "static int %s_match(const char* s, size_t n) {\n"
               "    size_t p = 0;\n"
               "    if (!%s__r_%s(0, (const unsigned char*)s, n, &p)) return 0;\n"
               "    return p == n;\n}\n",
           g->name, g->name, g->rules[0].name);
    eb_fmt(&e, "static int %s_collect(const char* s, size_t n, CCArena* arena,\n"
               "        int (*cb)(void* env, int id, CCSlice v), void* env) {\n"
               "    %s__ctx c0; c0.log = 0; c0.nlog = 0; c0.cap = 0;\n"
               "    size_t p = 0;\n"
               "    int ok = %s__r_%s(&c0, (const unsigned char*)s, n, &p) && p == n;\n"
               "    (void)arena;\n"
               "    if (ok && cb) {\n"
               "        for (size_t i = 0; i < c0.nlog; i++) {\n"
               "            const char* kp = s + c0.log[i].a;\n"
               "            size_t kl = c0.log[i].b - c0.log[i].a;\n"
               "            CCSlice v;\n"
               "            switch (c0.log[i].codec) {\n",
           g->name, g->name, g->name, g->rules[0].name);
    /* codec contract: int codec(const char* p, size_t n, CCSlice* out, CCArena* arena)
     * — returns 0 on decode failure (fails the collect); writes *out. Out-param
     * keeps unique (materialized) slices inside CC's move-only rules. */
    for (int ci = 0; ci < g->ncodecs; ci++)
        eb_fmt(&e, "            case %d: if (!%s(kp, kl, &v, arena)) { ok = 0; goto Ldone; } break;\n",
               ci + 1, g->codecs[ci]);
    eb_fmt(&e, "            default: v = cc_slice_from_buffer((void*)kp, kl); break;\n"
               "            }\n"
               "            if (cb(env, c0.log[i].id, v)) { ok = 0; break; }\n"
               "        }\n"
               "    }\n"
               "Ldone:\n"
               "    free(c0.log);\n"
               "    return ok;\n}\n");

    cc_sb_append(e.buf, e.len, e.cap, "", 1);
    if (out) out[len - 1] = '\0';
    free(F); free(K);
    return out;
}

/* Emitted matchers take const unsigned char*; LIT bytes compare via pool
 * (unsigned) so charset/memcmp semantics are byte-exact for non-ASCII. */

static char* cc__rules_emit(const char* name, const char* body, size_t body_len,
                            const char* file, int line, char* err, size_t err_sz) {
    RG* g = (RG*)calloc(1, sizeof(RG));
    if (!g) { snprintf(err, err_sz, "@grammar(rules): out of memory"); return NULL; }
    g->body = body; g->n = body_len; g->file = file; g->line0 = line; g->name = name;

    char* out = NULL;
    if (rg_parse(g) == 0) {
        out = rg_emit(g, line);
    } else {
        snprintf(err, err_sz, "%s (at line %d)", g->err, rg_line_at(g, g->err_at));
    }
    free(g);
    return out;
}

/* --- builtin engine table --- */
char* cc_grammar_builtin_emit(const char* engine,
                              const char* name,
                              const char* body, size_t body_len,
                              const char* file, int line,
                              char* err, size_t err_sz) {
    if (err && err_sz) err[0] = '\0';
    if (engine && strcmp(engine, "rules") == 0)
        return cc__rules_emit(name, body, body_len, file, line, err, err_sz);
    /* not a builtin: signal fall-through to the comptime-fn path */
    return NULL;
}
