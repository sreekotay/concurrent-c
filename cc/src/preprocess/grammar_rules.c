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
 * successful full match every form projects from ONE substrate — the TAPE:
 * the committed event stream reified as contiguous 16-byte pre-order nodes
 * (leaves from keep, codec fused at push; interiors from collect, span
 * back-patched at END). match = tape suppressed; parse = tape returned
 * (adjacency is the child link, span is the sibling link); collect = tape
 * folded through the closure; schema will specialize it away; format will
 * invert it. PEG rollback is tape truncation: two words (total, bdepth).
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

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ IR ---- */

enum { RN_CHARSET, RN_LIT, RN_SEQ, RN_ALT, RN_SOME, RN_ANY, RN_OPT, RN_REF, RN_SKIP, RN_KEEP, RN_COLLECT };

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
    int entry_idx, entry_set;   /* first rule declared at include-depth 0 */
    unsigned char rule_inc[R_MAX_RULES];   /* rule came from an include */
    int kids[R_MAX_KIDS]; int nkids;
    unsigned char sets[R_MAX_SETS][32]; int nsets;
    char pool[R_MAX_POOL]; int npool;
    struct { char name[R_NAME_MAX]; int node; size_t at; } rules[R_MAX_RULES];
    int nrules;
    char codecs[16][R_NAME_MAX]; int ncodecs;   /* keep/decode(fn) codec names */
    char codenc[16][R_NAME_MAX];                /* optional /encode(fn) inverses
                                                 * contract: size_t enc(p, n, dst, cap);
                                                 * returns exact encoded size ALWAYS;
                                                 * dst == NULL -> measure only;
                                                 * dst != NULL -> write at most cap
                                                 * bytes (need > cap = didn't fit) */
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
            /* optional inverse: keep/decode(dec)/encode(enc) — the write side */
            if (p < g->n && g->body[p] == '/') {
                p++;
                if (!rg_kw(g, p, "encode", &after))
                    return rg_fail(g, p, "expected /encode(fn) after keep/decode(...)");
                p = rg_ws(g, after);
                if (p >= g->n || g->body[p] != '(')
                    return rg_fail(g, p, "expected '(' after /encode");
                p = rg_ws(g, p + 1);
                if (!rg_ident(g, p, &ce) || ce - p >= R_NAME_MAX)
                    return rg_fail(g, p, "expected encoder function name");
                memcpy(g->codenc[codec - 1], g->body + p, ce - p);
                g->codenc[codec - 1][ce - p] = '\0';
                p = rg_ws(g, ce);
                if (p >= g->n || g->body[p] != ')')
                    return rg_fail(g, p, "expected ')' closing /encode(fn)");
                p++;
            }
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
        /* brackets its span with BEGIN/END log markers: nesting for the DOM
         * builder, inherited rollback safety from the log truncation. */
        p = after;
        int child = rg_parse_term(g, &p, depth);
        if (child < 0) return -1;
        int nd = rg_node(g, RN_COLLECT, at);
        if (nd < 0) return -1;
        g->nodes[nd].a = child;
        *io = p;
        return nd;
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

/* defined with the schema engine below; used by the emitters for readable,
 * dead-code-free lowered output (byte annotations, escaping, reachability) */
static const char* rw_chr(int b, char buf[16]);
static void rs_esc(char* dst, size_t dstsz, const unsigned char* src, int len);
static void rw_mark_rule(const RG* g, int r, unsigned char* mark);
static int rw_pool_needed(const RG* g);
static void rw_cs_desc(const unsigned char* set, char* out, size_t sz);
void cc__grammar_note_ufcs_type(const char* type_name);

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
        /* `include` is reserved at rule-body top level: it ends the rule and
         * starts an include directive (rule boundaries are `ident :`, so a
         * bare directive word would otherwise absorb into the body as a ref) */
        if (depth == 0) {
            size_t e2;
            if (rg_ident(g, p, &e2) && e2 - p == 7 &&
                memcmp(g->body + p, "include", 7) == 0) break;
        }
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

/* include support: bodies of earlier @grammar(rules) blocks in this file,
 * resolved through the per-file registry (defined with the schema engine). */
static const char* cc__rules_body_lookup(const char* name, size_t* len);

static int rg_parse_text(RG* g, int depth);

static int rg_parse(RG* g) {
    if (rg_parse_text(g, 0) != 0) return -1;
    if (g->nrules == 0) return rg_fail(g, 0, "no rules declared");
    /* The entry point is the first rule the INCLUDING grammar declares, even
     * when an include precedes it. Safe to reorder here: references are still
     * by name; resolution below assigns indices. */
    if (g->entry_set && g->entry_idx != 0) {
        unsigned char tmp[sizeof(g->rules[0])];
        memcpy(tmp, &g->rules[0], sizeof(g->rules[0]));
        memcpy(&g->rules[0], &g->rules[g->entry_idx], sizeof(g->rules[0]));
        memcpy(&g->rules[g->entry_idx], tmp, sizeof(g->rules[0]));
    }
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

static int rg_parse_text(RG* g, int depth) {
    size_t p = 0;
    for (;;) {
        p = rg_ws(g, p);
        if (p >= g->n) break;
        size_t e;
        if (!rg_ident(g, p, &e)) return rg_fail(g, p, "expected rule name");
        size_t q = rg_ws(g, e);
        /* `include Other` — splice a previously declared grammar's rules,
         * verbatim, at compile time. Composition without ceremony: the
         * emitted code is the same flat specialized C either way. */
        if (e - p == 7 && memcmp(g->body + p, "include", 7) == 0 &&
            (q >= g->n || g->body[q] != ':')) {
            if (depth >= 4) return rg_fail(g, p, "include nesting too deep");
            /* `include "path"` — a grammar FILE is a factory artifact: shared
             * dialect text loaded at compile time, path relative to the
             * including source. `include Name` splices a grammar declared
             * earlier in this file (registry). Both are pure text splices. */
            if (q < g->n && g->body[q] == '"') {
                size_t ps = ++q;
                while (q < g->n && g->body[q] != '"') q++;
                if (q >= g->n) return rg_fail(g, ps, "unterminated include path");
                char path[512];
                {
                    size_t pl = q - ps;
                    const char* base = g->file ? g->file : "";
                    const char* slash = strrchr(base, '/');
                    size_t bl = (slash && g->body[ps] != '/') ? (size_t)(slash - base) + 1 : 0;
                    if (bl + pl >= sizeof(path)) return rg_fail(g, ps, "include path too long");
                    memcpy(path, base, bl);
                    memcpy(path + bl, g->body + ps, pl);
                    path[bl + pl] = '\0';
                }
                q++;
                {
                    FILE* f = fopen(path, "rb");
                    if (!f) {
                        char msg[600];
                        snprintf(msg, sizeof(msg), "cannot open included grammar '%s'", path);
                        return rg_fail(g, p, msg);
                    }
                    fseek(f, 0, SEEK_END);
                    long fl = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    char* ftxt = (char*)malloc(fl > 0 ? (size_t)fl : 1);
                    size_t frd = ftxt ? fread(ftxt, 1, (size_t)fl, f) : 0;
                    fclose(f);
                    if (!ftxt) return rg_fail(g, p, "out of memory reading include");
                    const char* sb = g->body; size_t sn = g->n;
                    const char* sf = g->file;
                    g->body = ftxt; g->n = frd;
                    g->file = path;   /* nested file includes resolve relative to this file */
                    int rc = rg_parse_text(g, depth + 1);
                    g->body = sb; g->n = sn; g->file = sf;
                    free(ftxt);   /* names/literals were copied into the pool */
                    if (rc != 0) return rc;
                }
                p = q;
                continue;
            }
            size_t ie;
            if (!rg_ident(g, q, &ie)) return rg_fail(g, q, "expected grammar name or \"path\" after include");
            {
                char nm[64];
                size_t nl = ie - q < sizeof(nm) - 1 ? ie - q : sizeof(nm) - 1;
                memcpy(nm, g->body + q, nl); nm[nl] = '\0';
                size_t blen = 0;
                const char* btxt = cc__rules_body_lookup(nm, &blen);
                if (!btxt) {
                    char msg[160];
                    snprintf(msg, sizeof(msg), "include of unknown grammar '%s' "
                             "(must be a @grammar(rules) block earlier in this file)", nm);
                    return rg_fail(g, p, msg);
                }
                const char* sb = g->body; size_t sn = g->n;
                g->body = btxt; g->n = blen;
                int rc = rg_parse_text(g, depth + 1);
                g->body = sb; g->n = sn;
                if (rc != 0) return rc;
            }
            p = ie;
            continue;
        }
        if (q >= g->n || g->body[q] != ':') return rg_fail(g, p, "expected ':' after rule name");
        if (e - p >= R_NAME_MAX) return rg_fail(g, p, "rule name too long");
        if (g->nrules >= R_MAX_RULES) return rg_fail(g, p, "too many rules");
        /* Duplicates are SHADOWING when a factory is involved: a rule declared
         * in the block overrides one spliced by an include (that's how a
         * factory is specialized without forking its file), and among includes
         * the first wins (so diamond includes stay legal). Two block-level
         * definitions of one name remain an error. */
        int dup = -1;
        for (int i = 0; i < g->nrules; i++)
            if (strlen(g->rules[i].name) == e - p &&
                memcmp(g->rules[i].name, g->body + p, e - p) == 0) { dup = i; break; }
        if (dup >= 0 && depth == 0 && !g->rule_inc[dup])
            return rg_fail(g, p, "duplicate rule name");
        q++;
        int nd = rg_parse_alt(g, &q, 0);
        if (nd < 0) return -1;
        if (dup >= 0) {
            if (depth == 0) {              /* block definition overrides include;
                                            * overrides do NOT claim the entry point */
                g->rules[dup].node = nd;
                g->rules[dup].at = p;
                g->rule_inc[dup] = 0;
            }
            /* included duplicate of an existing rule: first wins, skip */
            p = q;
            continue;
        }
        memcpy(g->rules[g->nrules].name, g->body + p, e - p);
        g->rules[g->nrules].name[e - p] = '\0';
        g->rules[g->nrules].at = p;
        g->rules[g->nrules].node = nd;
        g->rule_inc[g->nrules] = (unsigned char)(depth > 0);
        if (depth == 0 && !g->entry_set) { g->entry_idx = g->nrules; g->entry_set = 1; }
        g->nrules++;
        p = q;
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
    case RN_KEEP: case RN_COLLECT: rf_node(g, F, x->a, set, nullable); break;
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
    case RN_KEEP: case RN_COLLECT: return 1;
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

/* Tiny-pure-rule inlining: a ref to a rule whose subtree has no keep/collect,
 * no non-alias refs, and few nodes (ws, digit runs) emits the body inline —
 * golden's shape, where whitespace skipping is a loop, not a call. */
static int rg_inline_size(const RG* g, int nd, int depth) {
    if (depth > 8) return 1000;
    const RNode* x = &g->nodes[nd];
    switch (x->kind) {
    case RN_COLLECT: return 1000;               /* containers stay functions */
    case RN_KEEP:
        if (x->b > 0) return 1000;              /* codec keeps pull in decode: stay calls */
        return 2 + rg_inline_size(g, x->a, depth + 1);
    case RN_REF: {
        int eff = rg_effective(g, nd);
        if (eff != nd) return 1;                /* charset/lit alias */
        {   /* recurse through refs whose own body is inline-sized */
            int t = rg_inline_size(g, g->rules[x->nkids].node, depth + 1);
            return t >= 1000 ? 1000 : t;
        }
    }
    case RN_SEQ: case RN_ALT: {
        int t = 1;
        for (int i = 0; i < x->nkids; i++) t += rg_inline_size(g, g->kids[x->b + i], depth + 1);
        return t;
    }
    case RN_SOME: case RN_ANY: case RN_OPT: return 1 + rg_inline_size(g, x->a, depth + 1);
    default: return 1;
    }
}

/* Pure-run purity: subtree consumes only via charsets/literals (incl. alias
 * refs and seq/rep/opt over those). Inside a codec keep, ALT branches that are
 * pure runs cannot introduce bytes the codec would transform; every other
 * branch sets the keep's dirty flag. This fuses the matcher's knowledge into
 * the keep: clean spans borrow inline with NO codec call and NO re-scan —
 * golden's has_escape, derived from the grammar. Contract: a keep/decode
 * codec's transform triggers must live in non-pure-run branches (true by
 * construction: a pure run is the identity on its bytes). Codecs are
 * invoked ONLY on dirty spans — they must decode, not re-probe (no
 * memchr for the trigger the matcher already recorded). */
static int rg_pure_run(const RG* g, int nd, int depth) {
    if (depth > 8) return 0;
    const RNode* x = &g->nodes[nd];
    switch (x->kind) {
    case RN_CHARSET: case RN_LIT: return 1;
    case RN_REF: {
        int eff = rg_effective(g, nd);
        return eff != nd;   /* charset/lit alias only */
    }
    case RN_SEQ: case RN_ALT:
        for (int i = 0; i < x->nkids; i++)
            if (!rg_pure_run(g, g->kids[x->b + i], depth + 1)) return 0;
        return 1;
    case RN_SOME: case RN_ANY: case RN_OPT:
        return rg_pure_run(g, x->a, depth + 1);
    default: return 0;
    }
}

/* Boundary-risk: derived restore (__unwind pops nodes with anchor >= resume
 * position) misattributes a node only when a push ANCHOR can COINCIDE with a
 * later resume boundary — i.e. zero required consumption between the anchor
 * and a subsequent kp restore site. Nodes pushed by the failed attempt itself
 * always anchor at/after the resume position and are meant to pop; the hazard
 * is exclusively a committed EARLIER node sitting exactly on the boundary.
 *
 * Grammar-level test, three ways an anchor reaches a boundary untouched:
 *   (a) a rule body is "open" — a push anchor can equal the rule's end
 *       position (zero consumption from anchor to exit); caller context is
 *       unknown, so an open rule is conservatively hazardous;
 *   (b) a collect BEGIN (anchor at open) can reach a kp restore site inside
 *       its own child with zero consumption ("zr": the site's sv would equal
 *       the still-open BEGIN's anchor, and the unwind would pop it);
 *   (c) within a SEQ, an open element is followed — across a nullable gap —
 *       by an element with a zero-consumption-reachable kp site.
 * Hazard anywhere => every kp site keeps the explicit 3-word snapshot.
 * No hazard => the cursor is the state, everywhere. (JSON: no hazard — every
 * anchor is followed by required consumption before any kp boundary.) */
typedef struct {
    unsigned char open_[R_MAX_RULES];   /* open() per rule, least fixpoint */
    unsigned char zr_[R_MAX_RULES];     /* zr() per rule, least fixpoint */
} RRisk;

static int rn_nullable(const RG* g, RFirst* F, int nd) {
    unsigned char set[32]; int nul = 0;
    memset(set, 0, 32);
    rf_node(g, F, nd, set, &nul);
    return nul;
}

/* open(n): can a push anchor inside n coincide with n's END position?
 * Existential over finite derivations => LEAST fixpoint: rule refs read the
 * current table (all 0 initially) and the driver iterates to convergence.
 * In-progress-conservative memoization would poison the table: member ends
 * with value, value is mid-computation, and a bogus 1 sticks forever. */
static int rg_open_node(const RG* g, RFirst* F, RRisk* R, int nd) {
    const RNode* x = &g->nodes[nd];
    switch (x->kind) {
    case RN_KEEP: case RN_COLLECT:
        /* anchor at entry; coincides with exit iff the body can match empty */
        return rn_nullable(g, F, x->a) || rg_open_node(g, F, R, x->a);
    case RN_REF: return R->open_[x->nkids];
    case RN_SOME: case RN_ANY: case RN_OPT: return rg_open_node(g, F, R, x->a);
    case RN_SEQ:
        for (int i = 0; i < x->nkids; i++) {
            if (!rg_open_node(g, F, R, g->kids[x->b + i])) continue;
            int tail_nul = 1;
            for (int j = i + 1; j < x->nkids; j++)
                if (!rn_nullable(g, F, g->kids[x->b + j])) { tail_nul = 0; break; }
            if (tail_nul) return 1;
        }
        return 0;
    case RN_ALT:
        for (int i = 0; i < x->nkids; i++)
            if (rg_open_node(g, F, R, g->kids[x->b + i])) return 1;
        return 0;
    default: return 0;   /* charset/lit/skip: no pushes */
    }
}

/* zr(n): is a kp restore site reachable at ZERO consumption from n's start?
 * Same least-fixpoint discipline as open(). */
static int rg_zr_node(const RG* g, RFirst* F, RKeeps* K, RRisk* R, int nd) {
    const RNode* x = &g->nodes[nd];
    switch (x->kind) {
    case RN_KEEP: case RN_COLLECT: return rg_zr_node(g, F, K, R, x->a);
    case RN_REF: return R->zr_[x->nkids];
    case RN_SOME: case RN_ANY: case RN_OPT:
        /* the construct takes sv at entry: a kp site AT this position */
        if (rk_node(g, K, x->a)) return 1;
        return rg_zr_node(g, F, K, R, x->a);
    case RN_ALT:
        /* cascades take sv at entry (dispatch ALTs don't, but conservative) */
        if (rk_node(g, K, nd)) return 1;
        for (int i = 0; i < x->nkids; i++)
            if (rg_zr_node(g, F, K, R, g->kids[x->b + i])) return 1;
        return 0;
    case RN_SEQ:
        for (int i = 0; i < x->nkids; i++) {
            if (rg_zr_node(g, F, K, R, g->kids[x->b + i])) return 1;
            if (!rn_nullable(g, F, g->kids[x->b + i])) return 0;  /* must consume */
        }
        return 0;
    default: return 0;
    }
}

static int rg_grammar_risk(const RG* g, RFirst* F, RKeeps* K) {
    RRisk* R = (RRisk*)calloc(1, sizeof(RRisk));
    if (!R) return 1;
    for (int changed = 1; changed; ) {
        changed = 0;
        for (int r = 0; r < g->nrules; r++) {
            int o = rg_open_node(g, F, R, g->rules[r].node);
            int z = rg_zr_node(g, F, K, R, g->rules[r].node);
            if (o && !R->open_[r]) { R->open_[r] = 1; changed = 1; }
            if (z && !R->zr_[r])   { R->zr_[r] = 1;   changed = 1; }
        }
    }
    int haz = 0;
    for (int r = 0; !haz && r < g->nrules; r++)                        /* (a) */
        if (R->open_[r]) haz = 1;
    for (int i = 0; !haz && i < g->nnodes; i++) {
        const RNode* x = &g->nodes[i];
        if (x->kind == RN_COLLECT) {                                    /* (b) */
            if (rg_zr_node(g, F, K, R, x->a)) haz = 1;
        } else if (x->kind == RN_SEQ) {                                 /* (c) */
            int pending = 0;
            for (int j = 0; j < x->nkids; j++) {
                int kid = g->kids[x->b + j];
                if (pending && rg_zr_node(g, F, K, R, kid)) { haz = 1; break; }
                if (rg_open_node(g, F, R, kid)) pending = 1;
                else if (!rn_nullable(g, F, kid)) pending = 0;
            }
        }
    }
    free(R);
    return haz;
}

/* ------------------------------------------------------------- emitter ---- */

/* last_pad / omit_lead_pad: absorb adjacent identical pads so authors can
 * write natural `value: [ws … ws]` + `member: [ws …]` without stacked skips.
 * last_pad = rule idx of a pad just satisfied; omit_lead_pad strips that pad
 * from the next SEQ (used when emitting __np rule variants). */
typedef struct {
    char** buf; size_t* len; size_t* cap; RFirst* F; RKeeps* K;
    int mode; int dk; int risk; int dom;
    int last_pad; int omit_lead_pad;
} EB;

static int rw_pad_rule(const RG* g, RFirst* F, RKeeps* K, int nd);

static void eb_fmt(EB* e, const char* fmt, ...) {
    char tmp[16384];
    char* big = NULL;
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    if ((size_t)n < sizeof(tmp)) {
        cc_sb_append(e->buf, e->len, e->cap, tmp, (size_t)n);
    } else {   /* never truncate emitted code */
        big = (char*)malloc((size_t)n + 1);
        if (big) {
            vsnprintf(big, (size_t)n + 1, fmt, ap2);
            cc_sb_append(e->buf, e->len, e->cap, big, (size_t)n);
            free(big);
        }
    }
    va_end(ap2);
}

/* Leading pad rule of `rule` (nullable keep-free ref at SEQ head), else -1. */
static int rg_leading_pad_rule(const RG* g, RFirst* F, RKeeps* K, int rule) {
    int body = g->rules[rule].node;
    while (g->nodes[body].kind == RN_COLLECT || g->nodes[body].kind == RN_KEEP)
        body = g->nodes[body].a;
    const RNode* x = &g->nodes[body];
    if (x->kind != RN_SEQ || x->nkids < 1) return -1;
    return rw_pad_rule(g, F, K, g->kids[x->b]);
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
        e->last_pad = -1;
        if (x->b == 1) {
            char cb[16];
            int b = (int)(unsigned char)g->pool[x->a];
            eb_fmt(e, "    if (!(p < n && s[p] == %d /*%s*/)) goto %s;\n    p++;\n",
                   b, rw_chr(b, cb), fail);
        } else {
            char lit[64];
            rs_esc(lit, sizeof lit, (const unsigned char*)g->pool + x->a, x->b);
            eb_fmt(e, "    if (!(p + %d <= n && memcmp(s + p, %s__pool + %d, %d) == 0)) goto %s;   /* \"%s\" */\n"
                      "    p += %d;\n",
                   x->b, g->name, x->a, x->b, fail, lit, x->b);
        }
        break;
    case RN_CHARSET:
        e->last_pad = -1;
        eb_fmt(e, "    if (!(p < n && (%s__cs[%d][((unsigned char)s[p]) >> 3] & (1u << (((unsigned char)s[p]) & 7u))))) goto %s;\n"
                  "    p++;\n",
               g->name, x->a, fail);
        break;
    case RN_SKIP:
        e->last_pad = -1;
        eb_fmt(e, "    if (!(p < n)) goto %s;\n    p++;\n", fail);
        break;
    case RN_REF: {
        int pad = rw_pad_rule(g, e->F, e->K, nd);
        if (pad >= 0) {
            /* Identical pad already satisfied at the call site — skip. */
            if (e->last_pad == pad) break;
            int eff = rg_effective(g, nd);
            if (eff != nd) rg_emit_node(g, e, eff, fail, lbl, rid);
            else {
                int body = g->rules[x->nkids].node;
                if (rg_inline_size(g, body, 0) <= 24)
                    rg_emit_node(g, e, body, fail, lbl, x->nkids);
                else if (!rk_rule(g, e->K, x->nkids))
                    eb_fmt(e, "    if (!%s__r_%s(s, n, &p)) goto %s;\n",
                           g->name, g->rules[x->nkids].name, fail);
                else if (e->mode == 1)
                    eb_fmt(e, "    if (!%s__b_%s(c, s, n, &p)) goto %s;\n",
                           g->name, g->rules[x->nkids].name, fail);
                else
                    eb_fmt(e, "    if (!%s__m_%s(s, n, &p)) goto %s;\n",
                           g->name, g->rules[x->nkids].name, fail);
            }
            e->last_pad = pad;
            break;
        }
        int eff = rg_effective(g, nd);
        if (eff != nd) { e->last_pad = -1; rg_emit_node(g, e, eff, fail, lbl, rid); break; }
        {
            int body = g->rules[x->nkids].node;
            int lp = rg_leading_pad_rule(g, e->F, e->K, x->nkids);
            int use_np = (lp >= 0 && e->last_pad == lp);
            if (rg_inline_size(g, body, 0) <= 24) {   /* small keep-free-or-raw-keep rules: inline per mode */
                /* the inlined subtree keeps the TARGET rule's identity: keep
                 * ids are "enclosing rule", which inlining must not rewrite */
                if (use_np) e->omit_lead_pad = lp;
                rg_emit_node(g, e, body, fail, lbl, x->nkids);
                e->omit_lead_pad = -1;
                e->last_pad = -1;
                break;
            }
            const char* suf = use_np ? "__np" : "";
            if (!rk_rule(g, e->K, x->nkids))   /* pure rule: ONE shared ctx-free variant */
                eb_fmt(e, "    if (!%s__r_%s%s(s, n, &p)) goto %s;\n",
                       g->name, g->rules[x->nkids].name, suf, fail);
            else if (e->mode == 1)
                eb_fmt(e, "    if (!%s__b_%s%s(c, s, n, &p)) goto %s;\n",
                       g->name, g->rules[x->nkids].name, suf, fail);
            else
                eb_fmt(e, "    if (!%s__m_%s%s(s, n, &p)) goto %s;\n",
                       g->name, g->rules[x->nkids].name, suf, fail);
            e->last_pad = -1;
            break;
        }
    }
    case RN_SEQ: {
        int i = 0;
        if (e->omit_lead_pad >= 0) {
            while (i < x->nkids &&
                   rw_pad_rule(g, e->F, e->K, g->kids[x->b + i]) == e->omit_lead_pad)
                i++;
            e->omit_lead_pad = -1;
        }
        for (; i < x->nkids; i++)
            rg_emit_node(g, e, g->kids[x->b + i], fail, lbl, rid);
        break;
    }
    case RN_KEEP: {
        if (!e->mode) { rg_emit_node(g, e, x->a, fail, lbl, rid); break; }
        int k = (*lbl)++;
        int saved_dk = e->dk;
        if (x->b > 0) {
            eb_fmt(e, "    { size_t ka%d = p; int dr%d = 0; (void)dr%d;\n", k, k, k);
            e->dk = k;
        } else {
            eb_fmt(e, "    { size_t ka%d = p;\n", k);
        }
        rg_emit_node(g, e, x->a, fail, lbl, rid);
        e->dk = saved_dk;
        if (e->mode == 2) {
            /* extract mode (schema tier): the keep captures into out-params —
             * no tape, no ctx. Backtracking is free: a retried branch simply
             * overwrites the locals; a failed rule reports nothing. */
            if (x->b > 0)
                eb_fmt(e, "    *xa = ka%d; *xb = p; *xdr = dr%d;\n    }\n", k, k);
            else
                eb_fmt(e, "    *xa = ka%d; *xb = p;\n    }\n", k);
            break;
        }
        /* Keep writes the RAW source span into u.bytes (unwind anchor).
         * Codec keeps: if dirty, decode NOW while the span is hot into the
         * mat_* stash; materialize installs without re-scanning. */
        eb_fmt(e, "    if (c->total == c->cap && !%s__tgrow(c)) goto %s;\n",
               g->name, fail);
        if (x->b == 0)
            eb_fmt(e, "    { %sNode* nd%d = &c->tape[c->total++];\n"
                      "      nd%d->meta = %du | (((unsigned long long)(p - ka%d)) << 10);\n"
                      "      nd%d->u.bytes = (const char*)(s + ka%d); }\n    }\n",
                   g->name, k, k, rid, k, k, k);
        else {
            eb_fmt(e, "    { size_t ti%d = c->total;\n"
                      "      %sNode* nd%d = &c->tape[c->total++];\n"
                      "      nd%d->meta = %du | (dr%d ? 0x200u : 0u) | (((unsigned long long)(p - ka%d)) << 10);\n"
                      "      nd%d->u.bytes = (const char*)(s + ka%d);\n"
                      "      if (dr%d) {\n"
                      "        if (!c->mat_ptr) {\n"
                      "          c->mat_ptr = (const char**)cc_arena_alloc_local(c->arena, c->cap * sizeof(char*), 8);\n"
                      "          c->mat_len = (size_t*)cc_arena_alloc_local(c->arena, c->cap * sizeof(size_t), 8);\n"
                      "          c->mat_cow = (unsigned char*)cc_arena_alloc_local(c->arena, c->cap, 1);\n"
                      "          if (!c->mat_ptr || !c->mat_len || !c->mat_cow) goto %s;\n"
                      "          memset(c->mat_ptr, 0, c->cap * sizeof(char*));\n"
                      "        }\n"
                      "        { CCSlice v%d;\n"
                      "          switch (%d) {\n",
                   k, g->name, k, k, rid, k, k, k, k, k, fail, k, x->b);
            /* single codec index on this keep — emit that case + default fail */
            eb_fmt(e, "          case %d: if (!%s((const char*)(s + ka%d), (size_t)(p - ka%d), &v%d, c->arena)) goto %s; break;\n"
                      "          default: goto %s;\n"
                      "          }\n"
                      "          c->mat_ptr[ti%d] = (const char*)v%d.ptr;\n"
                      "          c->mat_len[ti%d] = v%d.len;\n"
                      "          c->mat_cow[ti%d] = (unsigned char)cc_slice_is_unique(v%d);\n"
                      "        }\n"
                      "      }\n"
                      "    }\n    }\n",
                   x->b, g->codecs[x->b - 1], k, k, k, fail, fail,
                   k, k, k, k, k, k);
        }
        break;
    }
    case RN_COLLECT: {
        if (e->mode != 1) { rg_emit_node(g, e, x->a, fail, lbl, rid); break; }
        /* BEGIN/END markers bracket the child's span. A failing child unwinds
         * through tape truncation, removing the BEGIN — no special casing. */
        eb_fmt(e, "    { if (c->bdepth >= 512) goto %s;\n"
                  "        if (c->total == c->cap && !%s__tgrow(c)) goto %s;\n"
                  "        c->tape[c->total].meta = 0x100u | %du;\n"
                  "        c->tape[c->total].u.bytes = (const char*)(s + p);   /* source anchor */\n"
                  "%s"
                  "        c->bstack[c->bdepth++] = c->total++; }\n", fail, g->name, fail, rid,
               e->dom ? "        if (c->sb) c->vbase[c->bdepth] = c->vsp;\n" : "");
        rg_emit_node(g, e, x->a, fail, lbl, rid);
        if (e->dom)
            eb_fmt(e, "    { size_t bi = c->bstack[--c->bdepth];\n"
                      "        c->tape[bi].meta |= ((unsigned long long)(c->total - bi)) << 10;\n"
                      "        if (c->sb && !%s__shape_end(c, bi)) goto %s;\n"
                      "    }\n", g->name, fail);
        else
            eb_fmt(e, "    { size_t bi = c->bstack[--c->bdepth];\n"
                      "        c->tape[bi].meta |= ((unsigned long long)(c->total - bi)) << 10; }\n");
        e->last_pad = -1;
        break;
    }
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
                /* Single-branch dispatch: nothing retries at this level, so no
                 * save/restore — a failing branch propagates to the enclosing
                 * resume point (opt/any/cascade/entry), which owns recovery of
                 * both cursor and tape. */
                eb_fmt(e, "    {\n");
                eb_fmt(e, "    if (p >= n) goto Lb%d;\n", k);
                eb_fmt(e, "    switch (s[p]) {\n", k);
                for (int i = 0; i < x->nkids; i++) {
                    if (i == big) continue;
                    for (int bch = 0; bch < 256; bch++)
                        if (bf[i][bch >> 3] & (1u << (bch & 7)))
                            eb_fmt(e, "    case %d:\n", bch);
                    if (e->mode && e->dk >= 0 && !rg_pure_run(g, g->kids[x->b + i], 0))
                        eb_fmt(e, "    dr%d = 1;\n", e->dk);
                    {
                        char br[32];
                        snprintf(br, sizeof(br), "Lb%d", k);
                        rg_emit_node(g, e, g->kids[x->b + i], br, lbl, rid);
                    }
                    eb_fmt(e, "    break;\n");
                }
                if (big >= 0) {
                    eb_fmt(e, "    default:\n");
                    if (e->mode && e->dk >= 0 && !rg_pure_run(g, g->kids[x->b + big], 0))
                        eb_fmt(e, "    dr%d = 1;\n", e->dk);
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
                eb_fmt(e, "Lb%d: goto %s;\n", k, fail);
                eb_fmt(e, "Lok%d: ; }\n", k);
                break;
            }
        }
        /* fallback: PEG trial cascade */
        {
        int kp = e->mode == 1 ? rk_node(g, e->K, nd) : 0;
        int rk = kp ? e->risk : 0;
        if (kp && rk) eb_fmt(e, "    { size_t sv%d = p; size_t lt%d = c->total, ld%d = c->bdepth, lv%d = c->vsp;\n", k, k, k, k);
        else          eb_fmt(e, "    { size_t sv%d = p;\n", k);
        for (int i = 0; i < x->nkids; i++) {
            char br[32];
            int last = (i == x->nkids - 1);
            snprintf(br, sizeof(br), "La%d_%d", k, i);
            if (e->mode && e->dk >= 0 && !rg_pure_run(g, g->kids[x->b + i], 0))
                eb_fmt(e, "    dr%d = 1;\n", e->dk);
            rg_emit_node(g, e, g->kids[x->b + i], br, lbl, rid);
            eb_fmt(e, "    goto Lok%d;\n", k);
            if (kp && rk)      eb_fmt(e, "%s: p = sv%d; { c->total = lt%d; c->bdepth = ld%d; c->vsp = lv%d; }\n", br, k, k, k, k);
            else if (kp)       eb_fmt(e, "%s: p = sv%d; %s__unwind(c, s + p);\n", br, k, g->name);
            else               eb_fmt(e, "%s: p = sv%d;\n", br, k);
            if (last) eb_fmt(e, "    goto %s;\n", fail);
        }
        eb_fmt(e, "Lok%d: ; }\n", k);
        }
        break;
    }
    case RN_OPT: {
        int k = (*lbl)++;
        int kp = e->mode == 1 ? rk_node(g, e->K, x->a) : 0;
        int rk = kp ? e->risk : 0;
        char br[32];
        snprintf(br, sizeof(br), "Lo%d", k);
        if (kp && rk) eb_fmt(e, "    { size_t sv%d = p; size_t lt%d = c->total, ld%d = c->bdepth, lv%d = c->vsp;\n", k, k, k, k);
        else          eb_fmt(e, "    { size_t sv%d = p;\n", k);
        rg_emit_node(g, e, x->a, br, lbl, rid);
        if (kp && rk) eb_fmt(e, "    goto Lok%d;\n%s: p = sv%d; { c->total = lt%d; c->bdepth = ld%d; c->vsp = lv%d; }\nLok%d: ; }\n",
                             k, br, k, k, k, k, k);
        else if (kp)  eb_fmt(e, "    goto Lok%d;\n%s: p = sv%d; %s__unwind(c, s + p);\nLok%d: ; }\n",
                             k, br, k, g->name, k);
        else          eb_fmt(e, "    goto Lok%d;\n%s: p = sv%d;\nLok%d: ; }\n", k, br, k, k);
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
                    /* On a hit, advance to the first stop byte — don't restart
                     * the scalar walk at the word base (up to 7 double-touches). */
                    eb_fmt(e, "        if (m%d) { p += (size_t)(__builtin_ctzll(m%d) >> 3); break; }\n"
                              "        p += 8;\n    } }\n", k2, k2);
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
        int kp = e->mode == 1 ? rk_node(g, e->K, x->a) : 0;
        int rk = kp ? e->risk : 0;
        char br[32];
        snprintf(br, sizeof(br), "Ly%d", k);
        if (kp && rk) eb_fmt(e, "    { size_t sv%d; size_t lt%d = 0, ld%d = 0, lv%d = 0;\n"
                                "    for (;;) { sv%d = p; lt%d = c->total; ld%d = c->bdepth; lv%d = c->vsp;\n",
                             k, k, k, k, k, k, k, k);
        else          eb_fmt(e, "    { size_t sv%d;\n    for (;;) { sv%d = p;\n", k, k);
        rg_emit_node(g, e, x->a, br, lbl, rid);
        if (kp && rk) eb_fmt(e, "    if (p == sv%d) break;\n    }\n    goto Lok%d;\n%s: p = sv%d; { c->total = lt%d; c->bdepth = ld%d; c->vsp = lv%d; }\nLok%d: ; }\n",
                             k, k, br, k, k, k, k, k);
        else if (kp)  eb_fmt(e, "    if (p == sv%d) break;\n    }\n    goto Lok%d;\n%s: p = sv%d; %s__unwind(c, s + p);\nLok%d: ; }\n",
                             k, k, br, k, g->name, k);
        else          eb_fmt(e, "    if (p == sv%d) break;\n    }\n    goto Lok%d;\n%s: p = sv%d;\nLok%d: ; }\n",
                             k, k, br, k, k);
        break;
        }
    }
    }
}

static int rw_is_objlist(const RG* g, RFirst* F, RKeeps* K, int rule);

static char* rg_emit(const RG* g, int origin_line, int want_match, int want_build, int want_dom) {
    char* out = NULL; size_t len = 0, cap = 0;
    RFirst* F = (RFirst*)calloc(1, sizeof(RFirst));
    RKeeps* K = (RKeeps*)calloc(1, sizeof(RKeeps));
    EB e = { &out, &len, &cap, F, K, 0, -1, 0, 0, -1, -1 };
    int lbl = 0;
    if (!F || !K) { free(F); free(K); return NULL; }
    e.dom = want_dom;
    e.risk = rg_grammar_risk(g, F, K);

    /* the manifest: what this declaration lowered to, and how to call it —
     * the stable surface is the cc_* ops in <ccc/cc_grammar.cch> */
    eb_fmt(&e, "/* @grammar(rules) %s (line %d): %d rule(s). API:\n", g->name, origin_line, g->nrules);
    if (want_match)
        eb_fmt(&e, " *   cc_match(%s, s, n)                     -> %s_match\n", g->name, g->name);
    if (want_build) {
        eb_fmt(&e, " *   cc_parse(%s, s, n, arena)              -> %s_parse -> %sNode* tape\n",
               g->name, g->name, g->name);
        eb_fmt(&e, " *   cc_collect(%s, s, n, arena, cb, env)   -> %s_collect\n", g->name, g->name);
    if (want_dom)
        eb_fmt(&e, " *   cc_dom(%s, s, n, reg, arena, &out)      -> %s_dom (shaped: hidden-class DOM)\n",
               g->name, g->name);
        eb_fmt(&e, " *   nodes: %sNode_{id,is_list,len,first,next,count,slice}; ids: %s_KEEP_<rule>\n",
               g->name, g->name);
    }
    if (!want_match && !want_build)
        eb_fmt(&e, " *   (no tier referenced by this file -> none emitted; factory only)\n");
    eb_fmt(&e, " */\n");
    eb_fmt(&e, "typedef struct { int rule_count; } %s;\n", g->name);
    eb_fmt(&e, "static inline int %s_rule_count(void) { return %d; }\n", g->name, g->nrules);
    /* collect context: span log with cursor-coupled rollback (nlog restores
     * alongside p, so failed-branch keeps are never replayed). */
    /* THE TAPE — the one substrate every consumption form projects from.
     * The committed event stream is reified as contiguous 16-byte nodes in
     * pre-order: leaves from `keep`, interior nodes from `collect` (span
     * back-patched at END, O(1)). Dirty codec keeps decode at keep-commit into
     * mat_* (span still hot) while u.bytes stays the SOURCE anchor for
     * __unwind; materialize installs the stash. Adjacency is the child link
     * (first child = nd+1) and span is the sibling link (next = nd + span).
     *   meta = ruleid(8) | is_list(1<<8) | cow(1<<9) | byte_len(<<10)
     *   u    = source anchor during parse; decoded arena bytes after materialize
     * match = tape suppressed; collect = fold; parse = tape; schema specializes. */
    if (want_build) {
    eb_fmt(&e, "typedef struct { unsigned long long meta;\n"
               "    union { const char* bytes; } u; } %sNode;\n",
           g->name);
    /* the ctx only holds POINTERS to shape types, so the substrate stays
     * dependency-free: plain struct forward decls (always repeatable) —
     * full definitions are needed only when cc_dom is demanded, and a dom
     * caller necessarily includes <ccc/cc_shape.cch> for CCShapeReg. */
    eb_fmt(&e, "struct CCShapeB; struct CCShapeVal;\n");
    /* mat_* : eager keep/decode stash. Dirty keeps decode while the span is
     * still hot, but u.bytes stays the SOURCE anchor so __unwind works.
     * Materialize installs mat_ptr without re-scanning. */
    eb_fmt(&e, "typedef struct { %sNode* tape; size_t total, cap;\n"
               "    size_t bstack[512]; size_t bdepth; CCArena* arena;\n"
               "    const char** mat_ptr; size_t* mat_len; unsigned char* mat_cow;\n"
               "    /* shaped-DOM hook (armed by _dom; NULL for plain parse):\n"
               "     * completed containers hand their value up this stack —\n"
               "     * one push per container, anchored for derived restore */\n"
               "    struct CCShapeB* sb;\n"
               "    struct CCShapeVal* vstack; const unsigned char** vanchor;\n"
               "    size_t vsp, vcap;\n"
               "    size_t vbase[512]; } %s__ctx;\n",
           g->name, g->name);
    eb_fmt(&e, "static __attribute__((noinline)) int %s__tgrow(%s__ctx* c) {\n"
               "    size_t nc = c->cap * 2;\n"
               "    %sNode* nt = (%sNode*)cc_arena_realloc(c->arena, c->arena, c->tape,\n"
               "        c->cap * sizeof(%sNode), nc * sizeof(%sNode), _Alignof(%sNode));\n"
               "    if (!nt) return 0;\n"
               "    c->tape = nt;\n",
           g->name, g->name, g->name, g->name, g->name, g->name, g->name);
    if (g->ncodecs > 0) {
        eb_fmt(&e, "    if (c->mat_ptr) {\n"
                   "        const char** np = (const char**)cc_arena_realloc(c->arena, c->arena, c->mat_ptr,\n"
                   "            c->cap * sizeof(char*), nc * sizeof(char*), 8);\n"
                   "        size_t* nl = (size_t*)cc_arena_realloc(c->arena, c->arena, c->mat_len,\n"
                   "            c->cap * sizeof(size_t), nc * sizeof(size_t), 8);\n"
                   "        unsigned char* ncw = (unsigned char*)cc_arena_realloc(c->arena, c->arena, c->mat_cow,\n"
                   "            c->cap, nc, 1);\n"
                   "        if (!np || !nl || !ncw) return 0;\n"
                   "        memset(np + c->cap, 0, (nc - c->cap) * sizeof(char*));\n"
                   "        c->mat_ptr = np; c->mat_len = nl; c->mat_cow = ncw;\n"
                   "    }\n");
    }
    eb_fmt(&e, "    c->cap = nc; return 1;\n}\n");
    /* codec table: rule id -> codec index (0 = none). Lazy decode reads it.
     * Keep ids are the enclosing rule's index (inlining preserves this), so a
     * per-rule subtree scan resolves each rule's codec. Two DIFFERENT codecs
     * in one rule would collide — engine limitation, detectable, none here. */
    if (g->ncodecs > 0) {
        eb_fmt(&e, "static const short %s__codec_of[%d] = {", g->name, g->nrules);
        for (int r = 0; r < g->nrules; r++) {
            int cd = 0;
            int stack[R_MAX_NODES]; int sp = 0;
            stack[sp++] = g->rules[r].node;
            while (sp > 0) {
                const RNode* x = &g->nodes[stack[--sp]];
                if (x->kind == RN_KEEP) {
                    if (x->b > 0) cd = x->b;
                    stack[sp++] = x->a;
                } else if (x->kind == RN_COLLECT || x->kind == RN_SOME ||
                           x->kind == RN_ANY || x->kind == RN_OPT) {
                    stack[sp++] = x->a;
                } else if (x->kind == RN_SEQ || x->kind == RN_ALT) {
                    for (int i = 0; i < x->nkids; i++) stack[sp++] = g->kids[x->b + i];
                }
            }
            eb_fmt(&e, "%d,", cd);
        }
        eb_fmt(&e, "};\n");
    }
    /* Derived restore: the cursor is the ONLY live parse state. Every node's
     * u is its source anchor during the parse (leaves: raw span pointer;
     * BEGINs: position at open; spans live in meta), so failure recovery is
     * a cold backward pop of nodes born at/after the resume position. */
    eb_fmt(&e, "static __attribute__((noinline)) void %s__unwind(%s__ctx* c, const unsigned char* sv) {\n"
               "    while (c->total > 1 && (const unsigned char*)c->tape[c->total - 1].u.bytes >= sv)\n"
               "        c->total--;\n"
               "    while (c->bdepth > 0 && c->bstack[c->bdepth - 1] >= c->total) c->bdepth--;\n"
               "    if (c->vanchor)\n"
               "        while (c->vsp > 0 && c->vanchor[c->vsp - 1] >= sv) c->vsp--;\n"
               "}\n", g->name, g->name);
    if (want_dom)
        eb_fmt(&e, "static int %s__shape_end(%s__ctx* c, size_t bi);\n", g->name, g->name);
    }   /* want_build: tape substrate */
    /* keep ids: the enclosing rule's index, exported per rule containing keeps */
    for (int r = 0; r < g->nrules; r++) {
        /* reachable-keep scan (iterative stack over the rule's subtree) */
        int stack[R_MAX_NODES]; int sp = 0, haskeep = 0;
        stack[sp++] = g->rules[r].node;
        while (sp > 0) {
            const RNode* x = &g->nodes[stack[--sp]];
            if (x->kind == RN_KEEP || x->kind == RN_COLLECT) { haskeep = 1; break; }
            if (x->kind == RN_SEQ || x->kind == RN_ALT)
                for (int i = 0; i < x->nkids; i++) stack[sp++] = g->kids[x->b + i];
            else if (x->kind == RN_SOME || x->kind == RN_ANY || x->kind == RN_OPT)
                stack[sp++] = x->a;
            /* RN_REF: keeps inside other rules belong to those rules' ids */
        }
        if (haskeep)
            eb_fmt(&e, "#define %s_KEEP_%s %d\n", g->name, g->rules[r].name, r);
    }

    if (g->npool > 0 && (want_match || want_build) && rw_pool_needed(g)) {
        /* only multi-byte literals read the pool at runtime; otherwise it
         * carries nothing but compile-time name strings — emit nothing */
        eb_fmt(&e, "static const unsigned char %s__pool[%d] = {", g->name, g->npool);
        for (int i = 0; i < g->npool; i++) eb_fmt(&e, "%d,", (int)(unsigned char)g->pool[i]);
        eb_fmt(&e, "};\n");
    }
    if (g->nsets > 0 && (want_match || want_build)) {
        eb_fmt(&e, "static const unsigned char %s__cs[%d][32] = {\n", g->name, g->nsets);
        for (int s = 0; s < g->nsets; s++) {
            char desc[128];
            rw_cs_desc(g->sets[s], desc, sizeof desc);
            eb_fmt(&e, "  /* cs%d: %s */\n  {", s, desc);
            for (int i = 0; i < 32; i++) eb_fmt(&e, "%u,", (unsigned)g->sets[s][i]);
            eb_fmt(&e, "},\n");
        }
        eb_fmt(&e, "};\n");
    }
    /* Sharing is semantics-driven. Three classes per rule:
     *   skipped — every reference inlined (aliases, tiny pure bodies): no
     *             function emitted at all;
     *   pure    — KEEPS analysis proves no tape effect: ONE shared ctx-free
     *             __r_ variant, called by both tiers (truly shared code);
     *   sunk    — touches the tape: two specializations, __m_ (no ctx, no
     *             sink, no snapshots) and __b_ (unconditional sink), so no
     *             tier pays for the other's mode. */
    {
        unsigned char skip[R_MAX_RULES], pure[R_MAX_RULES], mark[R_MAX_RULES];
        memset(mark, 0, sizeof mark);
        rw_mark_rule(g, 0, mark);   /* only rules reachable-as-functions emit */
        for (int r = 0; r < g->nrules; r++) {
            int body = g->rules[r].node;
            int aliasable = g->nodes[body].kind == RN_CHARSET || g->nodes[body].kind == RN_LIT;
            skip[r] = (unsigned char)(r != 0 && (aliasable || rg_inline_size(g, body, 0) <= 24 || !mark[r]));
            pure[r] = (unsigned char)!rk_rule(g, e.K, r);
        }
        /* leading-pad rules get a __np sibling (body without the lead pad) so
         * call sites that just ran the same pad can skip the stacked re-walk */
        unsigned char has_np[R_MAX_RULES];
        memset(has_np, 0, sizeof has_np);
        for (int r = 0; r < g->nrules; r++) {
            if (skip[r]) continue;
            if (rg_leading_pad_rule(g, F, K, r) >= 0) has_np[r] = 1;
        }
        for (int r = 0; r < g->nrules; r++) {
            if (skip[r]) continue;
            if (pure[r]) {
                if (want_match || want_build) {
                    eb_fmt(&e, "static int %s__r_%s(const unsigned char* s, size_t n, size_t* io);\n",
                           g->name, g->rules[r].name);
                    if (has_np[r])
                        eb_fmt(&e, "static int %s__r_%s__np(const unsigned char* s, size_t n, size_t* io);\n",
                               g->name, g->rules[r].name);
                }
            } else {
                if (want_match) {
                    eb_fmt(&e, "static int %s__m_%s(const unsigned char* s, size_t n, size_t* io);\n",
                           g->name, g->rules[r].name);
                    if (has_np[r])
                        eb_fmt(&e, "static int %s__m_%s__np(const unsigned char* s, size_t n, size_t* io);\n",
                               g->name, g->rules[r].name);
                }
                if (want_build) {
                    eb_fmt(&e, "static int %s__b_%s(%s__ctx* c, const unsigned char* s, size_t n, size_t* io);\n",
                           g->name, g->rules[r].name, g->name);
                    if (has_np[r])
                        eb_fmt(&e, "static int %s__b_%s__np(%s__ctx* c, const unsigned char* s, size_t n, size_t* io);\n",
                               g->name, g->rules[r].name, g->name);
                }
            }
        }
        for (int mode = 0; mode <= 1; mode++) {
            e.mode = mode;
            for (int r = 0; r < g->nrules; r++) {
                if (skip[r]) continue;
                int lp = has_np[r] ? rg_leading_pad_rule(g, F, K, r) : -1;
                /* emit __np before the full rule so the wrapper can call it */
                for (int np = has_np[r] ? 1 : 0; np >= 0; np--) {
                    int is_np = has_np[r] && np == 1;
                    const char* suf = is_np ? "__np" : "";
                    if (pure[r]) {
                        if (!mode) continue;
                        if (!want_match && !want_build) continue;
                        e.mode = 0;
                        eb_fmt(&e, "static int %s__r_%s%s(const unsigned char* s, size_t n, size_t* io) {\n"
                                   "    size_t p = *io;\n    (void)s; (void)n;\n",
                               g->name, g->rules[r].name, suf);
                    } else if (mode) {
                        if (!want_build) continue;
                        eb_fmt(&e, "static int %s__b_%s%s(%s__ctx* c, const unsigned char* s, size_t n, size_t* io) {\n"
                                   "    size_t p = *io;\n    (void)c; (void)s; (void)n;\n",
                               g->name, g->rules[r].name, suf, g->name);
                    } else {
                        if (!want_match) continue;
                        eb_fmt(&e, "static int %s__m_%s%s(const unsigned char* s, size_t n, size_t* io) {\n"
                                   "    size_t p = *io;\n    (void)s; (void)n;\n",
                               g->name, g->rules[r].name, suf);
                    }
                    char fail[16];
                    snprintf(fail, sizeof(fail), "Lf%d", lbl++);
                    e.last_pad = -1;
                    e.omit_lead_pad = is_np ? lp : -1;
                    if (!is_np && lp >= 0) {
                        /* full rule: lead pad then __np (shared core) */
                        eb_fmt(&e, "    { size_t _pp = p;\n");
                        e.last_pad = -1;
                        /* emit the leading pad kid only */
                        {
                            int body = g->rules[r].node;
                            while (g->nodes[body].kind == RN_COLLECT ||
                                   g->nodes[body].kind == RN_KEEP)
                                body = g->nodes[body].a;
                            rg_emit_node(g, &e, g->kids[g->nodes[body].b], fail, &lbl, r);
                        }
                        if (pure[r] || !mode)
                            eb_fmt(&e, "    if (!%s__%s_%s__np(s, n, &p)) goto %s;\n",
                                   g->name, pure[r] ? "r" : "m", g->rules[r].name, fail);
                        else
                            eb_fmt(&e, "    if (!%s__b_%s__np(c, s, n, &p)) goto %s;\n",
                                   g->name, g->rules[r].name, fail);
                        eb_fmt(&e, "    (void)_pp; }\n");
                    } else {
                        rg_emit_node(g, &e, g->rules[r].node, fail, &lbl, r);
                    }
                    e.omit_lead_pad = -1;
                    e.last_pad = -1;
                    eb_fmt(&e, "    *io = p; return 1;\n%s:\n    return 0;\n}\n", fail);
                    e.mode = mode;
                    if (!has_np[r]) break;
                }
            }
        }
        /* entries reference rule 0 by its class */
        e.mode = 1;
        {
            const char* p0 = pure[0] ? "r" : "m";
            const char* b0 = pure[0] ? "r" : "b";
            if (want_match)
                eb_fmt(&e, "#define %s__ENTRY_M %s__%s_%s\n", g->name, g->name, p0, g->rules[0].name);
            if (want_build)
                eb_fmt(&e, "#define %s__ENTRY_B %s__%s_%s\n", g->name, g->name, b0, g->rules[0].name);
            eb_fmt(&e, "#define %s__ENTRY_PURE %d\n", g->name, pure[0] ? 1 : 0);
        }
    }

    if (want_build) {
    /* tape accessors: adjacency and span ARE the tree. Spans live in meta
     * (bits 10+) for interior nodes; u stays a byte pointer everywhere. */
    eb_fmt(&e, "static int %sNode_id(const %sNode* nd) { return (int)(nd->meta & 0xFFu); }\n",
           g->name, g->name);
    eb_fmt(&e, "static int %sNode_is_list(const %sNode* nd) { return (int)((nd->meta >> 8) & 1u); }\n",
           g->name, g->name);
    eb_fmt(&e, "static size_t %sNode_len(const %sNode* nd) { return (size_t)(nd->meta >> 10); }\n",
           g->name, g->name);
    eb_fmt(&e, "static %sNode* %sNode_first(%sNode* nd) {\n"
               "    return %sNode_is_list(nd) && (nd->meta >> 10) > 1 ? nd + 1 : 0; }\n",
           g->name, g->name, g->name, g->name);
    eb_fmt(&e, "static %sNode* %sNode_next(%sNode* nd, %sNode* parent) {\n"
               "    %sNode* nx = nd + (%sNode_is_list(nd) ? (size_t)(nd->meta >> 10) : 1);\n"
               "    return nx < parent + (size_t)(parent->meta >> 10) ? nx : 0; }\n",
           g->name, g->name, g->name, g->name, g->name, g->name);
    eb_fmt(&e, "static size_t %sNode_count(%sNode* nd) {\n"
               "    size_t k = 0;\n"
               "    for (%sNode* ch = %sNode_first(nd); ch; ch = %sNode_next(ch, nd)) k++;\n"
               "    return k; }\n",
           g->name, g->name, g->name, g->name, g->name);
    eb_fmt(&e, "static void %sNode_slice(const %sNode* nd, CCSlice* out) {\n"
               "    size_t l = (size_t)(nd->meta >> 10);\n"
               "    if ((nd->meta >> 9) & 1u)\n"
               "        *out = cc_slice_from_parts((void*)nd->u.bytes, l,\n"
               "                   cc_slice_make_id(3ULL, true, false, false), l);\n"
               "    else\n"
               "        *out = cc_slice_from_buffer((void*)nd->u.bytes, l);\n}\n",
           g->name, g->name);
    /* Lazy number accessor — borrow stays default; ffc runs on use.
     * Link a TU that defines FFC_IMPL (cc/runtime/arena_state.c). */
    eb_fmt(&e, "#include <ccc/vendor/ffc.h>\n"
               "static inline double %sNode_as_f64(const %sNode* nd) {\n"
               "    if ((nd->meta >> 8) & 1u) return 0.0;\n"
               "    { double v = 0.0; size_t l = (size_t)(nd->meta >> 10);\n"
               "      ffc_parse_options o; o.format = FFC_PRESET_JSON; o.decimal_point = '.';\n"
               "      ffc_result r = ffc_from_chars_double_options(\n"
               "          nd->u.bytes, nd->u.bytes + l, &v, o);\n"
               "      return r.outcome == FFC_OUTCOME_OK ? v : 0.0; }\n}\n",
           g->name, g->name);

    /* Entries — every form is a projection of the same run:
     *   match   = tape suppressed (NULL ctx)
     *   parse   = tape returned (root = tape[0], span covers the whole run)
     *   collect = parse + fold (leaves through the closure, warm sequential) */
    /* instance UFCS on tape nodes (`nd.first()`, `nd.next(parent)`, ...) */
    {
        char nn[80];
        snprintf(nn, sizeof nn, "%sNode", g->name);
        cc__grammar_note_ufcs_type(nn);
    }
    }   /* want_build: accessors */
    if (want_match)
    eb_fmt(&e, "static int %s_match(const char* s, size_t n) {\n"
               "    size_t p = 0;\n"
               "    if (!%s__ENTRY_M((const unsigned char*)s, n, &p)) return 0;\n"
               "    return p == n;\n}\n",
           g->name, g->name);
    if (want_build) {
    eb_fmt(&e, "static %sNode* %s_parse(const char* s, size_t n, CCArena* arena) {\n"
               "    %s__ctx c0;\n"
               "    size_t p = 0;\n"
               "    c0.cap = 1024;\n"
               "    c0.tape = (%sNode*)cc_arena_alloc_local(arena, c0.cap * sizeof(%sNode), _Alignof(%sNode));\n"
               "    if (!c0.tape) return 0;\n"
               "    c0.total = 1; c0.bdepth = 0; c0.arena = arena;\n"
               "    c0.mat_ptr = 0; c0.mat_len = 0; c0.mat_cow = 0;\n"
               "    c0.sb = 0; c0.vstack = 0; c0.vanchor = 0; c0.vsp = 0; c0.vcap = 0;\n"
               "    c0.tape[0].meta = 0x100u | 0xFFu;   /* root list */\n"
               "    c0.tape[0].u.bytes = s;             /* source anchor */\n"
               "#if %s__ENTRY_PURE\n"
               "    if (!%s__ENTRY_B((const unsigned char*)s, n, &p) || p != n) return 0;\n"
               "#else\n"
               "    if (!%s__ENTRY_B(&c0, (const unsigned char*)s, n, &p) || p != n) return 0;\n"
               "#endif\n"
               "    c0.tape[0].meta |= ((unsigned long long)c0.total) << 10;\n",
           g->name, g->name, g->name, g->name, g->name, g->name, g->name, g->name, g->name);
    if (g->ncodecs > 0) {
        /* Materialize: install eager mat_* stash (decoded at keep-commit while
         * hot). Fallback codec path covers leaves that somehow skipped stash.
         * Failed branches were unwound and never decode. */
        eb_fmt(&e, "    { size_t t;\n"
                   "    for (t = 1; t < c0.total; t++) {\n"
                   "        unsigned long long m = c0.tape[t].meta;\n"
                   "        if ((m & 0x300u) != 0x200u) continue;   /* dirty leaves only */\n"
                   "        { int id = (int)(m & 0xFFu);\n"
                   "          if (c0.mat_ptr && c0.mat_ptr[t]) {\n"
                   "            c0.tape[t].meta = (unsigned long long)id\n"
                   "                | (c0.mat_cow[t] ? 0x200u : 0u)\n"
                   "                | (((unsigned long long)c0.mat_len[t]) << 10);\n"
                   "            c0.tape[t].u.bytes = c0.mat_ptr[t];\n"
                   "            continue;\n"
                   "          }\n"
                   "          { CCSlice v;\n"
                   "            const char* kp = c0.tape[t].u.bytes;\n"
                   "            size_t kl = (size_t)(m >> 10);\n"
                   "            switch (%s__codec_of[id]) {\n", g->name);
        for (int ci = 0; ci < g->ncodecs; ci++)
            eb_fmt(&e, "            case %d: if (!%s(kp, kl, &v, arena)) return 0; break;\n",
                   ci + 1, g->codecs[ci]);
        eb_fmt(&e, "            default: continue;\n"
                   "            }\n"
                   "            c0.tape[t].meta = (unsigned long long)id\n"
                   "                | (cc_slice_is_unique(v) ? 0x200u : 0u)\n"
                   "                | (((unsigned long long)v.len) << 10);\n"
                   "            c0.tape[t].u.bytes = (const char*)v.ptr; } }\n"
                   "    } }\n");
    }
    eb_fmt(&e, "    return c0.tape;\n}\n");
    eb_fmt(&e, "static int %s_collect(const char* s, size_t n, CCArena* arena,\n"
               "        int (*cb)(void* env, int id, CCSlice v), void* env) {\n"
               "    %sNode* tape = %s_parse(s, n, arena);\n"
               "    if (!tape) return 0;\n"
               "    { size_t total = (size_t)(tape[0].meta >> 10);\n"
               "    for (size_t t = 1; t < total; t++) {\n"
               "        if ((tape[t].meta >> 8) & 1u) continue;   /* interior */\n"
               "        { CCSlice v; %sNode_slice(&tape[t], &v);\n"
               "          if (cb(env, (int)(tape[t].meta & 0xFFu), v)) return 0; }\n"
               "    } }\n"
               "    return 1;\n}\n",
           g->name, g->name, g->name, g->name);
    }   /* want_build: parse + collect entries */

    if (want_dom) {
        /* ---- shaped (hidden-class) DOM, CONSTRUCTED AS WE TAPE ----
         * Shaping happens at each container's END marker, bottom-up, while
         * its children are committed, contiguous, and cache-hot: leaves
         * shape inline (dirty spans decode HERE — a codec failure is an
         * ordinary branch failure); completed child containers hand their
         * value up the ctx value stack (one push per container, anchored
         * so derived restore unwinds them like keeps). No post-pass, no
         * recursive walker, each node visited once. Container rules are
         * classified by the SAME narrowing analysis the schema tier uses:
         * member lists build OBJECTS (keys -> the caller's persistent
         * CCShapeReg trie; instances carry 16 B value slots only), all
         * else ARRAYS; a container that doesn't pair (key,value) cleanly
         * falls back to an array — never wrong, just not key-addressable. */
        eb_fmt(&e, "static const unsigned char %s__dynobj[%d] = {", g->name, g->nrules);
        for (int r = 0; r < g->nrules; r++)
            eb_fmt(&e, "%s%d", r ? "," : "", rw_is_objlist(g, e.F, e.K, r) ? 1 : 0);
        eb_fmt(&e, "};\n");
        eb_fmt(&e, "static __attribute__((noinline)) int %s__vgrow(%s__ctx* c) {\n"
                   "    size_t nc = c->vcap * 2;\n"
                   "    CCShapeVal* nv = (CCShapeVal*)cc_arena_realloc(c->arena, c->arena, c->vstack,\n"
                   "        c->vcap * sizeof(CCShapeVal), nc * sizeof(CCShapeVal), 16);\n"
                   "    const unsigned char** na = (const unsigned char**)cc_arena_realloc(c->arena, c->arena,\n"
                   "        (void*)c->vanchor, c->vcap * sizeof(void*), nc * sizeof(void*), 16);\n"
                   "    if (!nv || !na) return 0;\n"
                   "    c->vstack = nv; c->vanchor = na; c->vcap = nc; return 1;\n}\n",
               g->name, g->name);
        eb_fmt(&e, "static int %s__shape_leaf(%s__ctx* c, const %sNode* ln, CCShapeVal* out) {\n"
                   "    unsigned long long m = ln->meta;\n"
                   "    int id = (int)(m & 0xFFu);\n",
               g->name, g->name, g->name);
        if (g->ncodecs > 0) {
            eb_fmt(&e, "    if (m & 0x200u) {   /* dirty: prefer keep-commit stash; else decode */\n"
                       "        size_t ti = (size_t)(ln - c->tape);\n"
                       "        if (c->mat_ptr && c->mat_ptr[ti]) {\n"
                       "            *out = cc_shape_leaf(id, c->mat_ptr[ti], c->mat_len[ti], c->mat_cow[ti]);\n"
                       "            return 1;\n"
                       "        }\n"
                       "        { CCSlice v;\n"
                       "        switch (%s__codec_of[id]) {\n", g->name);
            for (int ci = 0; ci < g->ncodecs; ci++)
                eb_fmt(&e, "        case %d: if (!%s((const char*)ln->u.bytes, (size_t)(m >> 10), &v, c->arena)) return 0; break;\n",
                       ci + 1, g->codecs[ci]);
            eb_fmt(&e, "        default: *out = cc_shape_leaf(id, ln->u.bytes, (size_t)(m >> 10), 0); return 1;\n"
                       "        }\n"
                       "        *out = cc_shape_leaf(id, (const char*)v.ptr, v.len, cc_slice_is_unique(v));\n"
                       "        return 1; }\n    }\n");
        }
        eb_fmt(&e, "    *out = cc_shape_leaf(id, ln->u.bytes, (size_t)(m >> 10), 0);\n"
                   "    return 1;\n}\n");
        eb_fmt(&e, "static int %s__shape_end(%s__ctx* c, size_t bi) {\n"
                   "    %sNode* nd = c->tape + bi;\n"
                   "    size_t span = c->total - bi;\n"
                   "    int id = (int)(nd->meta & 0xFFu);\n"
                   "    size_t vb = c->vbase[c->bdepth];\n"
                   "    size_t vidx = vb;\n"
                   "    CCShapeVal out;\n"
                   "    size_t cnt = 0;\n"
                   "    int pairs = id != 0xFF && id < %d && %s__dynobj[id];\n"
                   "    { size_t t = 1; int odd = 0;\n"
                   "      while (t < span) {\n"
                   "          unsigned long long cm = nd[t].meta;\n"
                   "          if (pairs && !odd && ((cm >> 8) & 1u)) pairs = 0;\n"
                   "          odd ^= 1; cnt++;\n"
                   "          t += ((cm >> 8) & 1u) ? (size_t)(cm >> 10) : 1;\n"
                   "      }\n"
                   "      if (odd) pairs = 0;   /* dangling key */\n"
                   "    }\n"
                   "    if (pairs) {\n"
                   "        CCShapeObjD od;\n"
                   "        if (!cc_shape_objd_begin(c->sb, &od, (unsigned)(cnt / 2))) return 0;\n"
                   "        { size_t t = 1;\n"
                   "          while (t < span) {\n"
                   "              const %sNode* kn = nd + t;\n"
                   "              t++;\n"
                   "              { unsigned long long vm = nd[t].meta;\n"
                   "                CCShapeVal* tgt = cc_shape_objd_slot(c->sb, &od,\n"
                   "                    kn->u.bytes, (unsigned)(kn->meta >> 10));\n"
                   "                if (!tgt) return 0;\n"
                   "                if (!((vm >> 8) & 1u)) {\n"
                   "                    if (!%s__shape_leaf(c, nd + t, tgt)) return 0;\n"
                   "                    t += 1;\n"
                   "                } else {\n"
                   "                    *tgt = c->vstack[vidx++];\n"
                   "                    t += (size_t)(vm >> 10);\n"
                   "                } }\n"
                   "          } }\n"
                   "        if (!cc_shape_objd_end(c->sb, &od, &out)) return 0;\n"
                   "    } else {\n"
                   "        CCShapeVal av;\n"
                   "        CCShapeVal* items = cc_shape_arrd(c->sb, cnt, &av);\n"
                   "        if (!items) return 0;\n"
                   "        { size_t t = 1, i = 0;\n"
                   "          while (t < span) {\n"
                   "              unsigned long long cm = nd[t].meta;\n"
                   "              if (!((cm >> 8) & 1u)) {\n"
                   "                  if (!%s__shape_leaf(c, nd + t, &items[i++])) return 0;\n"
                   "                  t += 1;\n"
                   "              } else {\n"
                   "                  items[i++] = c->vstack[vidx++];\n"
                   "                  t += (size_t)(cm >> 10);\n"
                   "              }\n"
                   "          } }\n"
                   "        out = av;\n"
                   "    }\n"
                   "    /* consume children values, push own (anchored at the open) */\n"
                   "    c->vsp = vb;\n"
                   "    if (c->vsp == c->vcap && !%s__vgrow(c)) return 0;\n"
                   "    c->vanchor[c->vsp] = (const unsigned char*)nd->u.bytes;\n"
                   "    c->vstack[c->vsp++] = out;\n"
                   "    return 1;\n}\n",
               g->name, g->name, g->name, g->nrules, g->name, g->name, g->name, g->name, g->name);
        eb_fmt(&e, "static __attribute__((unused)) int %s_dom(const char* s0, size_t n,\n"
                   "        CCShapeReg* reg, CCArena* arena, CCShapeVal* out) {\n"
                   "    const unsigned char* s = (const unsigned char*)s0;\n"
                   "    %s__ctx c0;\n"
                   "    size_t p = 0;\n"
                   "    CCShapeB b;\n"
                   "    c0.cap = 1024;\n"
                   "    c0.tape = (%sNode*)cc_arena_alloc_local(arena, c0.cap * sizeof(%sNode), _Alignof(%sNode));\n"
                   "    if (!c0.tape) return 0;\n"
                   "    c0.total = 1; c0.bdepth = 0; c0.arena = arena;\n"
                   "    c0.mat_ptr = 0; c0.mat_len = 0; c0.mat_cow = 0;\n"
                   "    c0.tape[0].meta = 0x100u | 0xFFu;\n"
                   "    c0.tape[0].u.bytes = (const char*)s;\n"
                   "    cc_shape_build(&b, reg, arena);\n"
                   "    c0.sb = &b;\n"
                   "    c0.vcap = 1024;\n"
                   "    c0.vstack = (CCShapeVal*)cc_arena_alloc_local(arena, c0.vcap * sizeof(CCShapeVal), 16);\n"
                   "    c0.vanchor = (const unsigned char**)cc_arena_alloc_local(arena, c0.vcap * sizeof(void*), 16);\n"
                   "    if (!c0.vstack || !c0.vanchor) return 0;\n"
                   "    c0.vsp = 0;\n"
                   "    c0.vbase[0] = 0;\n"
                   "#if %s__ENTRY_PURE\n"
                   "    if (!%s__ENTRY_B((const unsigned char*)s, n, &p) || p != n) return 0;\n"
                   "#else\n"
                   "    if (!%s__ENTRY_B(&c0, (const unsigned char*)s, n, &p) || p != n) return 0;\n"
                   "#endif\n"
                   "    c0.tape[0].meta |= ((unsigned long long)c0.total) << 10;\n"
                   "    c0.bdepth = 0;\n"
                   "    if (!%s__shape_end(&c0, 0)) return 0;\n"
                   "    *out = c0.vstack[0];\n"
                   "    if (cc_shape_kind(out) == CC_SHAPE_ARR && out->u.arr->n == 1)\n"
                   "        *out = out->u.arr->items[0];   /* single root: unwrap */\n"
                   "    return 1;\n}\n",
               g->name, g->name, g->name, g->name, g->name, g->name, g->name, g->name, g->name);
    }

    cc_sb_append(e.buf, e.len, e.cap, "", 1);
    if (out) out[len - 1] = '\0';
    free(F); free(K);
    return out;
}

/* Emitted matchers take const unsigned char*; LIT bytes compare via pool
 * (unsigned) so charset/memcmp semantics are byte-exact for non-ASCII. */

/* ==================================================================== */
/* @grammar(schema) — the typed tier: parse direct-to-struct.            */
/*                                                                       */
/* A schema block `use`s a rules grammar declared earlier in the file    */
/* and describes a product shape over it:                                */
/*                                                                       */
/*   use Json                                                            */
/*   fields: open #'{' close #'}' sep #',' kv #':'                       */
/*           key Json.string pad Json.ws else Json.value                 */
/*   items:  open #'[' close #']' sep #',' pad Json.ws                   */
/*   Json.ws                                                             */
/*   fields [ "id"   id:   int Json.number                               */
/*            "text" text: Json.string                                   */
/*            "user" fields [ "screen_name" name: Json.string ] ]        */
/*   Json.ws                                                             */
/*                                                                       */
/* Emits `typedef struct Name {...} Name` + `Name_parse(s,n,arena,out)`. */
/* Bound leaves reuse the rules engine in EXTRACT mode (the keep's span  */
/* captured into locals — no tape); unknown members dispatch through a   */
/* key-length switch to the `else` rule's MATCH-tier skip. Nested        */
/* `fields` bind into the same struct (fields live at event sites);     */
/* `items Schema` produces an arena array of a previously declared      */
/* schema. Matchers for a used rules grammar are emitted once per file   */
/* under the `<Rules>__s` prefix and shared by every schema.             */
/* ==================================================================== */

enum { SK_LIT, SK_RULE, SK_BIND_SLICE, SK_BIND_INT, SK_BIND_FLOAT, SK_BIND_ITEMS, SK_BIND_BYTES, SK_FIELDS,
       SK_UNION,            /* one of [ variant [ product-terms ] ... ] — tagged union */
       SK_NARROW_MEMBERS,   /* G.rule [ "k" term ... ] — narrow a member-list rule */
       SK_NARROW_LIST };    /* f: G.rule of Schema    — narrow a list rule to an array */

/* Derived structural parameters of a narrowed rule. Narrowing is COMPOSITION:
 * the schema names a rules-grammar rule and the engine decomposes its shape —
 * open/sep/close delimiters, pad rules, the member's key/kv/value structure —
 * instead of the schema re-describing them in directives. The grammar stays
 * the single source of structural truth; the schema only selects bindings. */
typedef struct {
    int open_b, sep_b, close_b;        /* single-byte delimiters */
    int lpad[2]; int nlpad;            /* pads after open */
    int tpad[2]; int ntpad;            /* pads before close */
    int elem;                          /* element rule (member / value) */
    int mpad_a[2]; int nmpad_a;        /* member: pads before key */
    int key_rule;                      /* member: keep-bearing key rule */
    int mpad_b[2]; int nmpad_b;        /* member: pads before kv */
    int kv_b;                          /* member: kv delimiter */
    int val_rule;                      /* member: default value rule (the skip) */
    int vpad_a[2]; int nvpad_a;        /* value: pads before its core */
    int vpad_b[2]; int nvpad_b;        /* value: pads after its core */
} RNarrow;
enum { S_MAX_TERMS = 96, S_MAX_KEYS = 64, S_NAME = 64, S_MAX_BODY = 32 };

/* variant field prefix ("u.<name>." while emitting a union variant's terms;
 * "" otherwise) and the schema name (kind constants) — engine emission is
 * single-threaded per splice */
static char cc__fpfx[72];
static const char* cc__sname;

typedef struct {
    int kind;
    unsigned char lit[24]; int litlen;   /* SK_LIT */
    char rname[S_NAME]; int rule;        /* SK_RULE / SK_BIND_SLICE / SK_BIND_INT / SK_BIND_FLOAT */
    char field[S_NAME];                  /* SK_BIND_* */
    char etype[S_NAME];                  /* SK_BIND_ITEMS: element schema type */
    int cap;                             /* counted items: comptime capacity ->
                                          * INLINE array in the struct, no alloc;
                                          * over-cap is a parse error (0 = none) */
    char cfield[S_NAME];                 /* count-driven items / bytes: an earlier
                                            `int` field naming the count/length */
    RNarrow nw;                          /* SK_NARROW_*: derived structure */
    int kidx[24]; int k_cnt;             /* SK_FIELDS / SK_NARROW_MEMBERS: entries (indices into keys[];
                                            nested fields interleave the pool, so an
                                            explicit list, not a contiguous range) */
} STerm;

typedef struct { char key[S_NAME]; int term; } SKey;

typedef struct {
    const char* b; size_t n, p;
    int line0;
    char err[256]; size_t err_at;

    char usename[S_NAME];
    char usepath[256];                   /* use "path" as Name: file-backed factory */
    const char* rtext; size_t rlen;      /* inline rules [ ... ] section (verbatim) */
    STerm terms[S_MAX_TERMS]; int nterms;
    SKey keys[S_MAX_KEYS]; int nkeys;
    int body[S_MAX_BODY]; int nbody;

    int fo, fc, fs, fkv;                          /* fields: open/close/sep/kv */
    char fkey[S_NAME], fpad[S_NAME], felse[S_NAME];
    int io_, ic_, is_;                            /* items: open/close/sep */
    char ipad[S_NAME];
    int rfkey, rfpad, rfelse, ripad;              /* resolved rule idx, -1 unset */

    /* tagged union (`one of`): variants are product-term runs; dispatch is
     * the first byte of each variant's leading literal (one optional
     * non-literal DEFAULT arm — e.g. redis inline commands) */
    struct { char name[S_NAME]; int t[16]; int nt; int db; } uv[24];
    int nuv;
    int uterm;                           /* the SK_UNION term idx, -1 none */
    signed char vof[S_MAX_TERMS];        /* term idx -> variant idx (-1 top) */
    char fpfx[72];                       /* "u.<variant>." while emitting variant terms */

    int bindbit[S_MAX_TERMS];            /* term idx -> presence bit (-1 none) */
    int nbinds;                          /* fields in declaration order */
    int presence;                        /* conditional schema: emit cc__set bitmap */
} SS;

static int ss_fail(SS* s, size_t at, const char* msg) {
    if (!s->err[0]) { snprintf(s->err, sizeof(s->err), "%s", msg); s->err_at = at; }
    return -1;
}

static int ss_line_at(const SS* s, size_t at) {
    int line = s->line0;
    for (size_t i = 0; i < at && i < s->n; i++) if (s->b[i] == '\n') line++;
    return line;
}

static void ss_ws(SS* s) {
    for (;;) {
        while (s->p < s->n && (s->b[s->p] == ' ' || s->b[s->p] == '\t' ||
                               s->b[s->p] == '\r' || s->b[s->p] == '\n')) s->p++;
        if (s->p < s->n && s->b[s->p] == ';') {
            while (s->p < s->n && s->b[s->p] != '\n') s->p++;
            continue;
        }
        break;
    }
}

static int ss_ident(SS* s, char* out, size_t sz) {
    ss_ws(s);
    size_t q = s->p, o = 0;
    if (!(q < s->n && (isalpha((unsigned char)s->b[q]) || s->b[q] == '_'))) return 0;
    while (q < s->n && (isalnum((unsigned char)s->b[q]) || s->b[q] == '_')) {
        if (o + 1 < sz) out[o++] = s->b[q];
        q++;
    }
    out[o] = '\0'; s->p = q;
    return 1;
}

static int ss_peek(SS* s) { ss_ws(s); return s->p < s->n ? (unsigned char)s->b[s->p] : -1; }

static int ss_escbyte(SS* s, unsigned char* out) {
    if (s->p >= s->n) return 0;
    char c = s->b[s->p++];
    if (c != '\\') { *out = (unsigned char)c; return 1; }
    if (s->p >= s->n) return 0;
    char e = s->b[s->p++];
    switch (e) {
    case 'n': *out = '\n'; return 1;  case 't': *out = '\t'; return 1;
    case 'r': *out = '\r'; return 1;  case '0': *out = '\0'; return 1;
    case '\\': case '\'': case '"': *out = (unsigned char)e; return 1;
    default: return 0;
    }
}

static int ss_charlit(SS* s, unsigned char* out) {   /* at "#'" */
    if (!(s->p + 1 < s->n && s->b[s->p] == '#' && s->b[s->p + 1] == '\'')) return 0;
    s->p += 2;
    if (!ss_escbyte(s, out)) return 0;
    if (!(s->p < s->n && s->b[s->p] == '\'')) return 0;
    s->p++;
    return 1;
}

static int ss_string(SS* s, unsigned char* out, int cap, int* outlen) {   /* at '"' */
    if (!(s->p < s->n && s->b[s->p] == '"')) return 0;
    s->p++;
    int o = 0;
    while (s->p < s->n && s->b[s->p] != '"') {
        unsigned char c;
        if (!ss_escbyte(s, &c)) return 0;
        if (o >= cap) return 0;
        out[o++] = c;
    }
    if (s->p >= s->n) return 0;
    s->p++;
    *outlen = o;
    return 1;
}

static int ss_ruleref_tail(SS* s, char* rname) {   /* after Usename, at '.' */
    if (!(s->p < s->n && s->b[s->p] == '.')) return ss_fail(s, s->p, "expected '.' in rules reference");
    s->p++;
    char rn[S_NAME];
    if (!ss_ident(s, rn, sizeof rn)) return ss_fail(s, s->p, "expected rule name after '.'");
    memcpy(rname, rn, sizeof rn);
    return 0;
}

static int ss_ruleref(SS* s, char* rname, const char* what) {
    char un[S_NAME];
    if (!ss_ident(s, un, sizeof un)) return ss_fail(s, s->p, what);
    if (s->usename[0]) {
        if (strcmp(un, s->usename) != 0)
            return ss_fail(s, s->p, "this schema composes with `use` (shared factory): "
                                    "qualify rule references with the use name; bare names "
                                    "are for schemas with an inline rules [...] section (private copy)");
        return ss_ruleref_tail(s, rname);
    }
    /* self-contained schema (inline rules): a bare name IS the rule */
    snprintf(rname, S_NAME, "%s", un);
    return 0;
}

/* verbatim capture of an inline `rules [ ... ]` section: bracket depth with
 * lexical awareness (comments, "strings", #'c' literals may contain brackets) */
static int ss_rules_section(SS* s) {   /* at '[' */
    s->p++;
    size_t start = s->p;
    int depth = 1;
    while (s->p < s->n) {
        char c = s->b[s->p];
        if (c == ';') { while (s->p < s->n && s->b[s->p] != '\n') s->p++; continue; }
        if (c == '"') {
            s->p++;
            while (s->p < s->n && s->b[s->p] != '"') {
                if (s->b[s->p] == '\\') s->p++;
                s->p++;
            }
            if (s->p >= s->n) return ss_fail(s, start, "unterminated string in rules [...]");
            s->p++;
            continue;
        }
        if (c == '#' && s->p + 1 < s->n && s->b[s->p + 1] == '\'') {
            s->p += 2;
            if (s->p < s->n && s->b[s->p] == '\\') s->p++;
            if (s->p < s->n) s->p++;
            if (!(s->p < s->n && s->b[s->p] == '\'')) return ss_fail(s, start, "bad #'c' in rules [...]");
            s->p++;
            continue;
        }
        if (c == '[') depth++;
        else if (c == ']') { depth--; if (depth == 0) break; }
        s->p++;
    }
    if (depth != 0) return ss_fail(s, start, "unterminated rules [...] section");
    s->rtext = s->b + start;
    s->rlen = s->p - start;
    s->p++;
    return 0;
}

static int ss_term(SS* s, int* out_term);

static int ss_fields_body(SS* s, int self) {
    ss_ws(s);
    if (!(s->p < s->n && s->b[s->p] == '[')) return ss_fail(s, s->p, "expected '[' after fields");
    s->p++;
    for (;;) {
        int c = ss_peek(s);
        if (c == ']') { s->p++; break; }
        if (c != '"') return ss_fail(s, s->p, "expected \"key\" or ']' in fields [...]");
        if (s->nkeys >= S_MAX_KEYS) return ss_fail(s, s->p, "too many keys in fields [...]");
        if (s->terms[self].k_cnt >= (int)(sizeof(s->terms[self].kidx) / sizeof(int)))
            return ss_fail(s, s->p, "too many entries in one fields [...]");
        unsigned char kb[S_NAME]; int kl = 0;
        if (!ss_string(s, kb, S_NAME - 1, &kl)) return ss_fail(s, s->p, "bad key string");
        int ki = s->nkeys++;
        memcpy(s->keys[ki].key, kb, (size_t)kl); s->keys[ki].key[kl] = '\0';
        int ti;
        if (ss_term(s, &ti)) return -1;   /* may append nested keys in between */
        s->keys[ki].term = ti;
        s->terms[self].kidx[s->terms[self].k_cnt++] = ki;
    }
    return 0;
}

static int ss_term(SS* s, int* out_term) {
    if (s->nterms >= S_MAX_TERMS) return ss_fail(s, s->p, "schema too large (term limit)");
    int c = ss_peek(s);
    STerm* t = &s->terms[s->nterms];
    memset(t, 0, sizeof *t);
    t->rule = -1;
    if (c == '"') {
        int ll = 0;
        if (!ss_string(s, t->lit, (int)sizeof(t->lit), &ll)) return ss_fail(s, s->p, "bad string literal");
        if (ll == 0) return ss_fail(s, s->p, "empty literal");
        t->litlen = ll; t->kind = SK_LIT;
        *out_term = s->nterms++;
        return 0;
    }
    if (c == '#') {
        unsigned char b;
        if (!ss_charlit(s, &b)) return ss_fail(s, s->p, "bad #'c' literal");
        t->lit[0] = b; t->litlen = 1; t->kind = SK_LIT;
        *out_term = s->nterms++;
        return 0;
    }
    char id[S_NAME];
    if (!ss_ident(s, id, sizeof id)) return ss_fail(s, s->p, "expected term");
    if (strcmp(id, "one") == 0) {
        size_t save1 = s->p;
        char kw[S_NAME];
        if (ss_ident(s, kw, sizeof kw) && strcmp(kw, "of") == 0) {
            ss_ws(s);
            if (!(s->p < s->n && s->b[s->p] == '[')) return ss_fail(s, s->p, "expected '[' after one of");
            s->p++;
            if (s->uterm >= 0) return ss_fail(s, s->p, "one union per schema");
            t->kind = SK_UNION;
            int self = s->nterms++;
            for (;;) {
                ss_ws(s);
                if (s->p < s->n && s->b[s->p] == ']') { s->p++; break; }
                if (s->nuv >= 24) return ss_fail(s, s->p, "too many variants");
                char vn[S_NAME];
                if (!ss_ident(s, vn, sizeof vn)) return ss_fail(s, s->p, "expected variant name");
                snprintf(s->uv[s->nuv].name, S_NAME, "%s", vn);
                s->uv[s->nuv].nt = 0;
                ss_ws(s);
                if (!(s->p < s->n && s->b[s->p] == '[')) return ss_fail(s, s->p, "expected '[' after variant name");
                s->p++;
                for (;;) {
                    ss_ws(s);
                    if (s->p < s->n && s->b[s->p] == ']') { s->p++; break; }
                    if (s->uv[s->nuv].nt >= 16) return ss_fail(s, s->p, "variant too large");
                    int ti;
                    if (ss_term(s, &ti)) return -1;
                    s->vof[ti] = (signed char)s->nuv;
                    s->uv[s->nuv].t[s->uv[s->nuv].nt++] = ti;
                }
                if (s->uv[s->nuv].nt == 0) return ss_fail(s, s->p, "empty variant");
                s->nuv++;
            }
            if (s->nuv < 2) return ss_fail(s, s->p, "one of needs at least two variants");
            s->uterm = self;
            *out_term = self;
            return 0;
        }
        s->p = save1;
    }
    if (strcmp(id, "fields") == 0) {
        t->kind = SK_FIELDS;
        int self = s->nterms++;          /* reserve BEFORE recursing */
        if (ss_fields_body(s, self)) return -1;
        *out_term = self;
        return 0;
    }
    if (s->p < s->n && s->b[s->p] == '.') {
        if (!s->usename[0])
            return ss_fail(s, s->p, "this schema has no `use` — its rules are inline (private): "
                                    "reference rules bare, or add `use <Grammar>` / `use \"path\" as <Name>`");
        if (strcmp(id, s->usename) != 0)
            return ss_fail(s, s->p, "qualified rule reference does not match this schema's `use` name");
        if (ss_ruleref_tail(s, t->rname)) return -1;
        ss_ws(s);
        if (s->p < s->n && s->b[s->p] == '[') {
            /* G.rule [ ... ] — narrow the rule's member-list structure */
            t->kind = SK_NARROW_MEMBERS;
            int self = s->nterms++;
            if (ss_fields_body(s, self)) return -1;
            *out_term = self;
            return 0;
        }
        t->kind = SK_RULE;
        *out_term = s->nterms++;
        return 0;
    }
    ss_ws(s);
    if (!(s->p < s->n && s->b[s->p] == ':')) {
        if (!s->usename[0]) {
            /* self-contained schema: a bare ident is an inline rule ref,
             * optionally narrowed with a [ ... ] block */
            snprintf(t->rname, sizeof(t->rname), "%s", id);
            if (s->p < s->n && s->b[s->p] == '[') {
                t->kind = SK_NARROW_MEMBERS;
                int self = s->nterms++;
                if (ss_fields_body(s, self)) return -1;
                *out_term = self;
                return 0;
            }
            t->kind = SK_RULE;
            *out_term = s->nterms++;
            return 0;
        }
        return ss_fail(s, s->p, "expected '.', ':' or fields [...] after identifier");
    }
    s->p++;
    snprintf(t->field, sizeof(t->field), "%s", id);
    char v[S_NAME];
    if (!ss_ident(s, v, sizeof v)) return ss_fail(s, s->p, "expected binding after ':'");
    if (strcmp(v, "int") == 0) {
        if (ss_ruleref(s, t->rname, "expected rules reference after int")) return -1;
        t->kind = SK_BIND_INT;
    } else if (strcmp(v, "float") == 0 || strcmp(v, "double") == 0) {
        /* Lazy borrow remains the number default; float/double binds parse at
         * the keep site via ffc (JSON number grammar). */
        if (ss_ruleref(s, t->rname, "expected rules reference after float")) return -1;
        t->kind = SK_BIND_FLOAT;
    } else if (strcmp(v, "bytes") == 0) {
        /* count-driven raw read: exactly <field> bytes, zero-copy borrow.
         * This is the length-prefix primitive (RESP bulk strings, TLV). */
        char cn[S_NAME];
        if (!ss_ident(s, cn, sizeof cn)) return ss_fail(s, s->p, "expected length field after bytes");
        snprintf(t->cfield, sizeof(t->cfield), "%s", cn);
        t->kind = SK_BIND_BYTES;
    } else if (strcmp(v, "items") == 0) {
        char en[S_NAME];
        if (!ss_ident(s, en, sizeof en)) return ss_fail(s, s->p, "expected schema name after items");
        snprintf(t->etype, sizeof(t->etype), "%s", en);
        t->kind = SK_BIND_ITEMS;
        /* optional trailing count field => count-driven repetition (no
         * open/close/sep). Lookahead: a bare ident that is not the start of
         * the next term (ruleref `X.`, binding `x:`, or `fields [`). */
        {
            size_t save = s->p;
            char cn[S_NAME];
            if (ss_ident(s, cn, sizeof cn)) {
                ss_ws(s);
                int next_is_term = (s->p < s->n && (s->b[s->p] == '.' || s->b[s->p] == ':')) ||
                                   strcmp(cn, "fields") == 0 || strcmp(cn, "cap") == 0;
                if (next_is_term) s->p = save;
                else snprintf(t->cfield, sizeof(t->cfield), "%s", cn);
            }
        }
        /* `cap N`: comptime capacity — the field becomes an inline array,
         * the parse rejects larger counts (a protocol policy, not a tuning
         * knob), and the hot path allocates NOTHING */
        {
            size_t save = s->p;
            char kw[S_NAME];
            if (ss_ident(s, kw, sizeof kw)) {
                if (strcmp(kw, "cap") == 0) {
                    if (!t->cfield[0])
                        return ss_fail(s, s->p, "cap requires a count field (items Elem count cap N)");
                    ss_ws(s);
                    long v = 0; int nd = 0;
                    while (s->p < s->n && s->b[s->p] >= '0' && s->b[s->p] <= '9') {
                        v = v * 10 + (s->b[s->p++] - '0'); nd++;
                        if (v > 65536) return ss_fail(s, s->p, "cap too large (max 65536)");
                    }
                    if (!nd || v < 1) return ss_fail(s, s->p, "expected capacity after cap");
                    t->cap = (int)v;
                } else {
                    s->p = save;
                }
            }
        }
    } else if ((s->usename[0] && strcmp(v, s->usename) == 0) || !s->usename[0]) {
        if (s->usename[0]) {
            if (ss_ruleref_tail(s, t->rname)) return -1;
        } else {
            snprintf(t->rname, sizeof(t->rname), "%s", v);   /* bare inline rule */
        }
        t->kind = SK_BIND_SLICE;
        /* `f: G.rule of Schema` — narrow a list rule: elements parse as the
         * schema, the array shape (delimiters, pads) derives from the rule */
        {
            size_t save = s->p;
            char kw[S_NAME];
            if (ss_ident(s, kw, sizeof kw)) {
                if (strcmp(kw, "of") == 0) {
                    char en[S_NAME];
                    if (!ss_ident(s, en, sizeof en)) return ss_fail(s, s->p, "expected schema name after of");
                    snprintf(t->etype, sizeof(t->etype), "%s", en);
                    t->kind = SK_NARROW_LIST;
                } else {
                    s->p = save;
                }
            }
        }
    } else {
        return ss_fail(s, s->p, s->usename[0]
            ? "binding must be `int Use.rule`, `Use.rule`, or `items Schema` — this "
              "schema composes with `use` (shared): qualify rule references with the use name"
            : "binding must be `int rule`, `rule`, `bytes lenfield`, or `items Schema`");
    }
    *out_term = s->nterms++;
    return 0;
}

static int ss_is_param(const char* id) {
    return !strcmp(id, "open") || !strcmp(id, "close") || !strcmp(id, "sep") ||
           !strcmp(id, "kv") || !strcmp(id, "key") || !strcmp(id, "pad") ||
           !strcmp(id, "else");
}

static int ss_params(SS* s, int items) {
    for (;;) {
        size_t save = s->p;
        char id[S_NAME];
        if (!ss_ident(s, id, sizeof id)) return 0;
        if (!ss_is_param(id)) { s->p = save; return 0; }
        if (!strcmp(id, "open") || !strcmp(id, "close") || !strcmp(id, "sep") || !strcmp(id, "kv")) {
            ss_ws(s);
            unsigned char b;
            if (!ss_charlit(s, &b)) return ss_fail(s, s->p, "directive expects a #'c' literal");
            int* dst = items
                ? (!strcmp(id, "open") ? &s->io_ : !strcmp(id, "close") ? &s->ic_ :
                   !strcmp(id, "sep") ? &s->is_ : NULL)
                : (!strcmp(id, "open") ? &s->fo : !strcmp(id, "close") ? &s->fc :
                   !strcmp(id, "sep") ? &s->fs : &s->fkv);
            if (!dst) return ss_fail(s, s->p, "kv is not a valid items parameter");
            *dst = (int)b;
        } else {
            char* dst = items
                ? (!strcmp(id, "pad") ? s->ipad : NULL)
                : (!strcmp(id, "key") ? s->fkey : !strcmp(id, "pad") ? s->fpad :
                   !strcmp(id, "else") ? s->felse : NULL);
            if (!dst) return ss_fail(s, s->p, "key/else are not valid items parameters");
            if (ss_ruleref(s, dst, "directive expects Use.rule")) return -1;
        }
    }
}

static int ss_parse(SS* s) {
    s->fo = s->fc = s->fs = s->fkv = -1;
    s->io_ = s->ic_ = s->is_ = -1;
    s->rfkey = s->rfpad = s->rfelse = s->ripad = -1;
    /* `use <Grammar>` composes with a shared grammar; an inline `rules [...]`
     * section makes the schema self-contained. One or the other (v1). */
    {
        size_t save = s->p;
        char id[S_NAME];
        if (ss_ident(s, id, sizeof id) && strcmp(id, "use") == 0) {
            if (ss_peek(s) == '"') {
                /* use "path.rules" as Name — file-backed shared factory */
                s->p++;
                size_t ps = s->p;
                while (s->p < s->n && s->b[s->p] != '"') s->p++;
                if (s->p >= s->n) return ss_fail(s, ps, "unterminated use path");
                size_t pl = s->p - ps;
                if (pl >= sizeof(s->usepath)) return ss_fail(s, ps, "use path too long");
                memcpy(s->usepath, s->b + ps, pl); s->usepath[pl] = '\0';
                s->p++;
                if (!ss_ident(s, id, sizeof id) || strcmp(id, "as") != 0)
                    return ss_fail(s, s->p, "expected `as <Name>` after use \"path\"");
                if (!ss_ident(s, s->usename, sizeof s->usename))
                    return ss_fail(s, s->p, "expected namespace name after as");
            } else if (!ss_ident(s, s->usename, sizeof s->usename)) {
                return ss_fail(s, s->p, "expected rules grammar name or \"path\" after use");
            }
        } else {
            s->p = save;
        }
    }
    for (;;) {
        ss_ws(s);
        if (s->p >= s->n) break;
        size_t save = s->p;
        char kw[S_NAME];
        if (ss_ident(s, kw, sizeof kw)) {
            ss_ws(s);
            if (!strcmp(kw, "rules") && s->p < s->n && s->b[s->p] == '[') {
                if (s->rtext) return ss_fail(s, save, "duplicate rules [...] section");
                if (s->usename[0]) return ss_fail(s, save, "schema has `use` — inline rules [...] not allowed (v1: one or the other)");
                if (ss_rules_section(s)) return -1;
                continue;
            }
            int isdir = (s->p < s->n && s->b[s->p] == ':' &&
                         (!strcmp(kw, "fields") || !strcmp(kw, "items")));
            if (isdir) {
                s->p++;
                if (ss_params(s, kw[0] == 'i')) return -1;
                continue;
            }
        }
        s->p = save;
        if (s->nbody >= S_MAX_BODY) return ss_fail(s, s->p, "schema body too large");
        int ti;
        if (ss_term(s, &ti)) return -1;
        s->body[s->nbody++] = ti;
    }
    if (s->nbody == 0) return ss_fail(s, 0, "schema has no body");
    if (!s->usename[0] && !s->rtext)
        return ss_fail(s, 0, "schema needs `use <Grammar>` or an inline rules [...] section");
    return 0;
}

/* ---- registry: rules bodies + schema names, per input file ---- */

typedef struct {
    char name[S_NAME]; char pfx[S_NAME + 24];
    char path[512];                  /* nonempty for file-backed factories */
    char* body; size_t blen;
    int matchers_done;
    unsigned char x_done[R_MAX_RULES];
} SRulesReg;

/* load `relpath` relative to `base_file`'s directory (absolute passes through) */
static char* cc__load_rel(const char* base_file, const char* relpath,
                          char* pathbuf, size_t pathbuf_sz, size_t* out_len) {
    const char* base = base_file ? base_file : "";
    const char* slash = strrchr(base, '/');
    size_t bl = (slash && relpath[0] != '/') ? (size_t)(slash - base) + 1 : 0;
    size_t pl = strlen(relpath);
    if (bl + pl >= pathbuf_sz) return NULL;
    memcpy(pathbuf, base, bl);
    memcpy(pathbuf + bl, relpath, pl);
    pathbuf[bl + pl] = '\0';
    FILE* f = fopen(pathbuf, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long fl = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* txt = (char*)malloc(fl > 0 ? (size_t)fl + 1 : 1);
    if (!txt) { fclose(f); return NULL; }
    size_t rd = fread(txt, 1, (size_t)fl, f);
    fclose(f);
    txt[rd] = '\0';
    *out_len = rd;
    return txt;
}

static SRulesReg cc__rules_reg[8]; static int cc__rules_nreg;
static char cc__schema_reg[32][S_NAME]; static int cc__schema_nreg;

/* generated types needing instance-UFCS dispatch (Readers, Nodes); the
 * codegen phase registers each with the native Type_method hook */
static char cc__ufcs_types[64][80]; static int cc__ufcs_ntypes;

void cc__grammar_note_ufcs_type(const char* type_name) {
    for (int i = 0; i < cc__ufcs_ntypes; i++)
        if (strcmp(cc__ufcs_types[i], type_name) == 0) return;
    if (cc__ufcs_ntypes >= (int)(sizeof(cc__ufcs_types) / sizeof(cc__ufcs_types[0]))) return;
    snprintf(cc__ufcs_types[cc__ufcs_ntypes++], sizeof(cc__ufcs_types[0]), "%s", type_name);
}

int cc_grammar_pending_ufcs_type_count(void) { return cc__ufcs_ntypes; }
const char* cc_grammar_pending_ufcs_type(int i) {
    return (i >= 0 && i < cc__ufcs_ntypes) ? cc__ufcs_types[i] : NULL;
}

void cc__grammar_registry_reset(void) {
    for (int i = 0; i < cc__rules_nreg; i++) free(cc__rules_reg[i].body);
    memset(cc__rules_reg, 0, sizeof cc__rules_reg);
    cc__rules_nreg = 0; cc__schema_nreg = 0; cc__ufcs_ntypes = 0;
}

static void cc__register_rules(const char* name, const char* body, size_t blen) {
    if (cc__rules_nreg >= (int)(sizeof(cc__rules_reg) / sizeof(cc__rules_reg[0]))) return;
    SRulesReg* r = &cc__rules_reg[cc__rules_nreg];
    char* copy = (char*)malloc(blen + 1);
    if (!copy) return;
    memcpy(copy, body, blen); copy[blen] = '\0';
    snprintf(r->name, sizeof(r->name), "%s", name);
    snprintf(r->pfx, sizeof(r->pfx), "%s__s", name);
    r->body = copy; r->blen = blen; r->matchers_done = 0;
    memset(r->x_done, 0, sizeof(r->x_done));
    cc__rules_nreg++;
}

static SRulesReg* cc__find_rules(const char* name) {
    for (int i = 0; i < cc__rules_nreg; i++)
        if (strcmp(cc__rules_reg[i].name, name) == 0) return &cc__rules_reg[i];
    return NULL;
}

static const char* cc__rules_body_lookup(const char* name, size_t* len) {
    SRulesReg* r = cc__find_rules(name);
    if (!r) return NULL;
    *len = r->blen;
    return r->body;
}

/* file-backed factory: `use "path" as Name`. Keyed by RESOLVED path so every
 * schema using the same file shares one emitted matcher set (the prefix comes
 * from the first use's alias; later uses may alias differently). */
static SRulesReg* cc__use_rules_file(const char* base_file, const char* relpath,
                                     const char* alias) {
    char full[512];
    size_t blen = 0;
    char* body = cc__load_rel(base_file, relpath, full, sizeof(full), &blen);
    if (!body) return NULL;
    for (int i = 0; i < cc__rules_nreg; i++) {
        if (strcmp(cc__rules_reg[i].path, full) == 0) {
            free(body);
            return &cc__rules_reg[i];
        }
    }
    if (cc__rules_nreg >= (int)(sizeof(cc__rules_reg) / sizeof(cc__rules_reg[0]))) {
        free(body);
        return NULL;
    }
    SRulesReg* r = &cc__rules_reg[cc__rules_nreg++];
    snprintf(r->name, sizeof(r->name), "%s", alias);
    /* prefix carries the registry slot so a file-backed factory can never
     * collide with a same-named block factory (or another alias) */
    snprintf(r->pfx, sizeof(r->pfx), "%s__s%d", alias, cc__rules_nreg - 1);
    snprintf(r->path, sizeof(r->path), "%s", full);
    r->body = body; r->blen = blen; r->matchers_done = 0;
    memset(r->x_done, 0, sizeof(r->x_done));
    return r;
}

static int cc__schema_known(const char* name) {
    for (int i = 0; i < cc__schema_nreg; i++)
        if (strcmp(cc__schema_reg[i], name) == 0) return 1;
    return 0;
}

/* ---- schema emission ---- */

static int rs_rule_by_name(const RG* g, const char* name) {
    for (int r = 0; r < g->nrules; r++)
        if (strcmp(g->rules[r].name, name) == 0) return r;
    return -1;
}

static int rs_rule_codec(const RG* g, int r) {   /* 0 = none, else codec idx */
    int stack[R_MAX_NODES]; int sp = 0, cd = 0;
    stack[sp++] = g->rules[r].node;
    while (sp > 0) {
        const RNode* x = &g->nodes[stack[--sp]];
        if (x->kind == RN_KEEP) { if (x->b > 0) cd = x->b; stack[sp++] = x->a; }
        else if (x->kind == RN_COLLECT || x->kind == RN_SOME || x->kind == RN_ANY || x->kind == RN_OPT)
            stack[sp++] = x->a;
        else if (x->kind == RN_SEQ || x->kind == RN_ALT)
            for (int i = 0; i < x->nkids; i++) stack[sp++] = g->kids[x->b + i];
    }
    return cd;
}

static int rs_rule_has_keep(const RG* g, int r) {
    int stack[R_MAX_NODES]; int sp = 0;
    stack[sp++] = g->rules[r].node;
    while (sp > 0) {
        const RNode* x = &g->nodes[stack[--sp]];
        if (x->kind == RN_KEEP) return 1;
        if (x->kind == RN_COLLECT || x->kind == RN_SOME || x->kind == RN_ANY || x->kind == RN_OPT)
            stack[sp++] = x->a;
        else if (x->kind == RN_SEQ || x->kind == RN_ALT)
            for (int i = 0; i < x->nkids; i++) stack[sp++] = g->kids[x->b + i];
    }
    return 0;
}

static const char* rs_class(const RG* g, RKeeps* K, int r) {
    return rk_rule(g, (RKeeps*)K, r) ? "m" : "r";
}

/* ---- lowered-code quality helpers ---- */

/* reachability: which rules are needed AS FUNCTIONS, mirroring the emitter's
 * inline-vs-call decision exactly. Inlined bodies are walked (their inner
 * refs may still call); called rules are marked and walked. */
static void rw_mark_node(const RG* g, int nd, unsigned char* mark);

static void rw_mark_rule(const RG* g, int r, unsigned char* mark) {
    if (mark[r]) return;
    mark[r] = 1;
    rw_mark_node(g, g->rules[r].node, mark);
}

static void rw_mark_node(const RG* g, int nd, unsigned char* mark) {
    const RNode* x = &g->nodes[nd];
    switch (x->kind) {
    case RN_REF: {
        int eff = rg_effective(g, nd);
        if (eff != nd) return;                       /* charset/lit alias: inlined */
        {
            int body = g->rules[x->nkids].node;
            if (rg_inline_size(g, body, 0) <= 24) {  /* inlined: walk, don't mark */
                if (!mark[x->nkids]) rw_mark_node(g, body, mark);
                return;
            }
        }
        rw_mark_rule(g, x->nkids, mark);             /* called as a function */
        return;
    }
    case RN_KEEP: case RN_COLLECT: case RN_SOME: case RN_ANY: case RN_OPT:
        rw_mark_node(g, x->a, mark);
        return;
    case RN_SEQ: case RN_ALT:
        for (int i = 0; i < x->nkids; i++) rw_mark_node(g, g->kids[x->b + i], mark);
        return;
    default: return;
    }
}

/* does any literal actually need the byte pool at runtime? (single-byte lits
 * compare inline; the pool otherwise carries only compile-time name strings) */
static int rw_pool_needed(const RG* g) {
    for (int i = 0; i < g->nnodes; i++)
        if (g->nodes[i].kind == RN_LIT && g->nodes[i].b > 1) return 1;
    return 0;
}

/* printable-byte annotation for emitted compares: 61 -> "'='". '*' and '/'
 * stay hex so a comment can never contain a comment terminator. */
static const char* rw_chr(int b, char buf[16]) {
    if (b >= 0x21 && b <= 0x7e && b != '\'' && b != '\\' && b != '*' && b != '/')
        snprintf(buf, 16, "'%c'", (char)b);
    else if (b == ' ')  snprintf(buf, 16, "' '");
    else if (b == '\n') snprintf(buf, 16, "nl");
    else if (b == '\r') snprintf(buf, 16, "cr");
    else if (b == '\t') snprintf(buf, 16, "tab");
    else                snprintf(buf, 16, "0x%02X", b & 0xFF);
    return buf;
}

/* compact human description of a charset row, for the emitted table */
static void rw_cs_desc(const unsigned char* set, char* out, size_t sz) {
    size_t o = 0;
    int c = 0;
    out[0] = '\0';
    while (c < 256 && o + 16 < sz) {
        if (!((set[c >> 3] >> (c & 7)) & 1)) { c++; continue; }
        int d = c;
        while (d + 1 < 256 && ((set[(d + 1) >> 3] >> ((d + 1) & 7)) & 1)) d++;
        char a[16], b[16];
        if (d > c)
            o += (size_t)snprintf(out + o, sz - o, "%s%s-%s", o ? " " : "",
                                  rw_chr(c, a), rw_chr(d, b));
        else
            o += (size_t)snprintf(out + o, sz - o, "%s%s", o ? " " : "", rw_chr(c, a));
        c = d + 1;
    }
}

/* ---- narrowing: decompose a rule's structure instead of re-describing it ---- */

static int rw_unwrap(const RG* g, int nd) {   /* look through collect/keep wrappers */
    while (g->nodes[nd].kind == RN_COLLECT || g->nodes[nd].kind == RN_KEEP)
        nd = g->nodes[nd].a;
    return nd;
}

static int rw_lit1(const RG* g, int nd) {     /* single-byte literal -> byte, else -1 */
    const RNode* x = &g->nodes[nd];
    if (x->kind == RN_LIT && x->b == 1) return (int)(unsigned char)g->pool[x->a];
    return -1;
}

static int rw_pad_rule(const RG* g, RFirst* F, RKeeps* K, int nd) {
    /* a pad is a nullable keep-free rule reference (e.g. ws) */
    const RNode* x = &g->nodes[nd];
    if (x->kind != RN_REF) return -1;
    if (rk_rule((RG*)g, K, x->nkids)) return -1;
    if (!rn_nullable(g, F, nd)) return -1;
    return x->nkids;
}

/* delimited list: SEQ( LIT1 pad* OPT(SEQ(REF e, ANY(SEQ(LIT1, REF e)))) pad* LIT1 ) */
static int rw_match_list(const RG* g, RFirst* F, RKeeps* K, int rule, RNarrow* o) {
    memset(o, 0, sizeof *o);
    int body = rw_unwrap(g, g->rules[rule].node);
    const RNode* x = &g->nodes[body];
    if (x->kind != RN_SEQ || x->nkids < 3) return 0;
    int nk = x->nkids, i = 0;
    o->open_b = rw_lit1(g, g->kids[x->b + i]); if (o->open_b < 0) return 0;
    i++;
    while (i < nk) {
        int pr = rw_pad_rule(g, F, K, g->kids[x->b + i]);
        if (pr < 0) break;
        if (o->nlpad < 2) o->lpad[o->nlpad++] = pr;
        i++;
    }
    if (i >= nk) return 0;
    {
        const RNode* op = &g->nodes[g->kids[x->b + i]];
        if (op->kind != RN_OPT) return 0;
        const RNode* isq = &g->nodes[op->a];
        if (isq->kind != RN_SEQ || isq->nkids != 2) return 0;
        int k0 = g->kids[isq->b], k1 = g->kids[isq->b + 1];
        if (g->nodes[k0].kind != RN_REF) return 0;
        const RNode* an = &g->nodes[k1];
        if (an->kind != RN_ANY) return 0;
        const RNode* asq = &g->nodes[an->a];
        if (asq->kind != RN_SEQ || asq->nkids != 2) return 0;
        o->sep_b = rw_lit1(g, g->kids[asq->b]); if (o->sep_b < 0) return 0;
        if (g->nodes[g->kids[asq->b + 1]].kind != RN_REF) return 0;
        if (g->nodes[k0].nkids != g->nodes[g->kids[asq->b + 1]].nkids) return 0;
        o->elem = g->nodes[k0].nkids;
        i++;
    }
    while (i < nk - 1) {
        int pr = rw_pad_rule(g, F, K, g->kids[x->b + i]);
        if (pr < 0) return 0;
        if (o->ntpad < 2) o->tpad[o->ntpad++] = pr;
        i++;
    }
    o->close_b = rw_lit1(g, g->kids[x->b + (nk - 1)]); if (o->close_b < 0) return 0;
    return 1;
}

/* member: SEQ( pad* REF(key, keep-bearing) pad* LIT1 REF(value) ) */
static int rw_match_member(const RG* g, RFirst* F, RKeeps* K, RNarrow* o) {
    int body = rw_unwrap(g, g->rules[o->elem].node);
    const RNode* x = &g->nodes[body];
    if (x->kind != RN_SEQ || x->nkids < 3) return 0;
    int nk = x->nkids, i = 0;
    while (i < nk) {
        int pr = rw_pad_rule(g, F, K, g->kids[x->b + i]);
        if (pr < 0) break;
        if (o->nmpad_a < 2) o->mpad_a[o->nmpad_a++] = pr;
        i++;
    }
    if (i >= nk) return 0;
    if (g->nodes[g->kids[x->b + i]].kind != RN_REF) return 0;
    o->key_rule = g->nodes[g->kids[x->b + i]].nkids;
    if (!rs_rule_has_keep(g, o->key_rule)) return 0;
    i++;
    while (i < nk) {
        int pr = rw_pad_rule(g, F, K, g->kids[x->b + i]);
        if (pr < 0) break;
        if (o->nmpad_b < 2) o->mpad_b[o->nmpad_b++] = pr;
        i++;
    }
    if (i + 2 != nk) return 0;
    o->kv_b = rw_lit1(g, g->kids[x->b + i]); if (o->kv_b < 0) return 0;
    if (g->nodes[g->kids[x->b + (nk - 1)]].kind != RN_REF) return 0;
    o->val_rule = g->nodes[g->kids[x->b + (nk - 1)]].nkids;
    return 1;
}

/* value: SEQ( pad* core... pad* ) — pads around the core, for bound paths
 * (bound terms replace the core; the pads still belong to the grammar) */
/* dom classification: does this rule narrow to a member list (object)? */
static int rw_is_objlist(const RG* g, RFirst* F, RKeeps* K, int rule) {
    RNarrow nw;
    return rw_match_list(g, F, K, rule, &nw) && rw_match_member(g, F, K, &nw);
}

static void rw_match_value(const RG* g, RFirst* F, RKeeps* K, int vrule, RNarrow* o) {
    int body = rw_unwrap(g, g->rules[vrule].node);
    const RNode* x = &g->nodes[body];
    if (x->kind != RN_SEQ) return;   /* no pads */
    int nk = x->nkids, lo = 0, hi = nk;
    while (lo < hi) {
        int pr = rw_pad_rule(g, F, K, g->kids[x->b + lo]);
        if (pr < 0) break;
        if (o->nvpad_a < 2) o->vpad_a[o->nvpad_a++] = pr;
        lo++;
    }
    while (hi > lo + 1) {
        int pr = rw_pad_rule(g, F, K, g->kids[x->b + (hi - 1)]);
        if (pr < 0) break;
        hi--;
    }
    for (int j = hi; j < nk; j++) {
        int pr = rw_pad_rule(g, F, K, g->kids[x->b + j]);
        if (pr >= 0 && o->nvpad_b < 2) o->vpad_b[o->nvpad_b++] = pr;
    }
}

static void rs_esc(char* dst, size_t dstsz, const unsigned char* src, int len) {
    size_t o = 0;
    for (int i = 0; i < len && o + 5 < dstsz; i++) {
        unsigned char c = src[i];
        if (isalnum(c) || c == '_' || c == ' ' || c == '-')
            dst[o++] = (char)c;
        else
            o += (size_t)snprintf(dst + o, dstsz - o, "\\%03o", c);
    }
    dst[o] = '\0';
}

static void rs_emit_matchers(RG* g, EB* e, int* lbl, const unsigned char* mark) {
    /* mark == NULL: a SHARED factory (`use`) — later schemas in this file may
     * need any rule, so all are emitted, tagged unused-ok. mark != NULL: a
     * private inline grammar — only reachable-as-function rules are emitted. */
    const char* attr = mark ? "" : "__attribute__((unused)) ";
    if (g->npool > 0 && rw_pool_needed(g)) {
        eb_fmt(e, "static const unsigned char %s__pool[%d] = {", g->name, g->npool);
        for (int i = 0; i < g->npool; i++) eb_fmt(e, "%d,", (int)(unsigned char)g->pool[i]);
        eb_fmt(e, "};\n");
    }
    if (g->nsets > 0) {
        eb_fmt(e, "static const unsigned char %s__cs[%d][32] = {\n", g->name, g->nsets);
        for (int s = 0; s < g->nsets; s++) {
            char desc[128];
            rw_cs_desc(g->sets[s], desc, sizeof desc);
            eb_fmt(e, "  /* cs%d: %s */\n  {", s, desc);
            for (int i = 0; i < 32; i++) eb_fmt(e, "%u,", (unsigned)g->sets[s][i]);
            eb_fmt(e, "},\n");
        }
        eb_fmt(e, "};\n");
    }
    unsigned char has_np[R_MAX_RULES];
    memset(has_np, 0, sizeof has_np);
    for (int r = 0; r < g->nrules; r++) {
        if (mark && !mark[r]) continue;
        if (rg_leading_pad_rule(g, e->F, e->K, r) >= 0) has_np[r] = 1;
    }
    for (int r = 0; r < g->nrules; r++) {
        if (mark && !mark[r]) continue;
        eb_fmt(e, "static %sint %s__%s_%s(const unsigned char* s, size_t n, size_t* io);\n",
               attr, g->name, rs_class(g, e->K, r), g->rules[r].name);
        if (has_np[r])
            eb_fmt(e, "static %sint %s__%s_%s__np(const unsigned char* s, size_t n, size_t* io);\n",
                   attr, g->name, rs_class(g, e->K, r), g->rules[r].name);
    }
    e->mode = 0;
    for (int r = 0; r < g->nrules; r++) {
        if (mark && !mark[r]) continue;
        int lp = has_np[r] ? rg_leading_pad_rule(g, e->F, e->K, r) : -1;
        for (int vi = 0; vi < (has_np[r] ? 2 : 1); vi++) {
            int is_np = has_np[r] && vi == 0;
            const char* suf = is_np ? "__np" : "";
            eb_fmt(e, "static %sint %s__%s_%s%s(const unsigned char* s, size_t n, size_t* io) {\n"
                      "    size_t p = *io;\n    (void)s; (void)n;\n",
                   attr, g->name, rs_class(g, e->K, r), g->rules[r].name, suf);
            char fail[16];
            snprintf(fail, sizeof(fail), "Lf%d", (*lbl)++);
            e->last_pad = -1;
            e->omit_lead_pad = is_np ? lp : -1;
            if (!is_np && lp >= 0) {
                int body = g->rules[r].node;
                while (g->nodes[body].kind == RN_COLLECT || g->nodes[body].kind == RN_KEEP)
                    body = g->nodes[body].a;
                rg_emit_node(g, e, g->kids[g->nodes[body].b], fail, lbl, r);
                eb_fmt(e, "    if (!%s__%s_%s__np(s, n, &p)) goto %s;\n",
                       g->name, rs_class(g, e->K, r), g->rules[r].name, fail);
            } else {
                rg_emit_node(g, e, g->rules[r].node, fail, lbl, r);
            }
            e->omit_lead_pad = -1;
            e->last_pad = -1;
            eb_fmt(e, "    *io = p; return 1;\n%s:\n    return 0;\n}\n", fail);
        }
    }
}

static void rs_emit_x(RG* g, EB* e, int* lbl, int r) {
    e->mode = 2;
    eb_fmt(e, "static int %s__x_%s(const unsigned char* s, size_t n, size_t* io,\n"
              "        size_t* xa, size_t* xb, int* xdr) {\n"
              "    size_t p = *io;\n    (void)s; (void)n;\n    *xdr = 0;\n",
           g->name, g->rules[r].name);
    char fail[16];
    snprintf(fail, sizeof(fail), "Lf%d", (*lbl)++);
    rg_emit_node(g, e, g->rules[r].node, fail, lbl, r);
    eb_fmt(e, "    *io = p; return 1;\n%s:\n    return 0;\n}\n", fail);
    e->mode = 0;
}

static void rs_emit_pad(RG* g, EB* e, int prule, const char* fail) {
    if (prule < 0) return;
    eb_fmt(e, "    if (!%s__%s_%s(s, n, &p)) goto %s;\n",
           g->name, rs_class(g, e->K, prule), g->rules[prule].name, fail);
}

/* fusable int run: `keep [opt(charset=={'-'}) some(charset=contiguous range
 * within '0'..'9')]` (refs resolved) — the shape every length/count field
 * takes. Qualifying binds emit ONE accumulate-while-scan loop: no second
 * conversion pass, no charset bitmap loads (a range compare beats a table
 * load for digits), no opt scaffold. The hand-parser inner loop, derived. */
static int rs_int_run_shape(const RG* g, int rule, int* out_neg) {
    int body = g->rules[rule].node;
    if (g->nodes[body].kind != RN_KEEP || g->nodes[body].b != 0) return 0;
    int ch = rg_effective(g, g->nodes[body].a);
    int neg = 0, digits;
    if (g->nodes[ch].kind == RN_SEQ) {
        const RNode* x = &g->nodes[ch];
        if (x->nkids != 2) return 0;
        int k0 = rg_effective(g, g->kids[x->b]);
        if (g->nodes[k0].kind != RN_OPT) return 0;
        int c0 = rg_effective(g, g->nodes[k0].a);
        if (g->nodes[c0].kind == RN_LIT) {
            /* `opt #'-'` — the common spelling */
            if (g->nodes[c0].b != 1 || g->pool[g->nodes[c0].a] != '-') return 0;
        } else if (g->nodes[c0].kind == RN_CHARSET) {
            const unsigned char* set = g->sets[g->nodes[c0].a];
            for (int b = 0; b < 256; b++)
                if ((set[b >> 3] & (1u << (b & 7))) && b != '-') return 0;
            if (!(set['-' >> 3] & (1u << ('-' & 7)))) return 0;
        } else {
            return 0;
        }
        neg = 1;
        digits = rg_effective(g, g->kids[x->b + 1]);
    } else {
        digits = ch;
    }
    if (g->nodes[digits].kind != RN_SOME) return 0;
    {
        int dc = rg_effective(g, g->nodes[digits].a);
        if (g->nodes[dc].kind != RN_CHARSET) return 0;
        const unsigned char* set = g->sets[g->nodes[dc].a];
        int lo = -1, hi = -1;
        for (int b = 0; b < 256; b++) {
            if (set[b >> 3] & (1u << (b & 7))) {
                if (b < '0' || b > '9') return 0;
                if (lo < 0) lo = b;
                else if (b != hi + 1) return 0;
                hi = b;
            }
        }
        if (lo != '0' || hi != '9') return 0;   /* exact decimal range only */
    }
    *out_neg = neg;
    return 1;
}

static void rs_emit_bind_value(SS* ss, RG* g, EB* e, int* lbl, const STerm* t, const char* fail) {
    (void)ss;
    int k = (*lbl)++;
    if (t->kind == SK_BIND_INT) {
        int neg;
        if (rs_int_run_shape(g, t->rule, &neg)) {
            eb_fmt(e, "    { long long v%d = 0; size_t d%d;\n", k, k);
            if (neg)
                eb_fmt(e, "      int ng%d = 0;\n"
                          "      if (p < n && s[p] == '-') { ng%d = 1; p++; }\n", k, k);
            eb_fmt(e, "      d%d = p;\n"
                      "      for (; p < n && s[p] >= '0' && s[p] <= '9'; p++)\n"
                      "          v%d = v%d * 10 + (s[p] - '0');\n"
                      "      if (p == d%d) { if (p >= n) *cc_inc = 1; goto %s; }\n", k, k, k, k, fail);
            if (neg)
                eb_fmt(e, "      out->%s%s = ng%d ? -v%d : v%d; }\n", cc__fpfx, t->field, k, k, k);
            else
                eb_fmt(e, "      out->%s%s = v%d; }\n", cc__fpfx, t->field, k);
            return;
        }
    }
    /* small top-level-keep rules extract INLINE at the bind site — the
     * pointer indirection folds to registers and the call disappears. The
     * top-level-keep gate guarantees the capture fires on every success
     * path, so the locals are always written. Bigger rules stay calls. */
    int body = g->rules[t->rule].node;
    if (g->nodes[body].kind == RN_KEEP && rg_inline_size(g, g->nodes[body].a, 0) <= 16) {
        eb_fmt(e, "    { size_t xa%d, xb%d; int xd%d = 0;\n"
                  "      { size_t* xa = &xa%d; size_t* xb = &xb%d; int* xdr = &xd%d;\n"
                  "        (void)xa; (void)xb; (void)xdr;\n",
               k, k, k, k, k, k);
        e->mode = 2;
        rg_emit_node(g, e, body, fail, lbl, t->rule);
        e->mode = 0;
        eb_fmt(e, "      }\n");
    } else {
        eb_fmt(e, "    { size_t xa%d, xb%d; int xd%d;\n"
                  "      if (!%s__x_%s(s, n, &p, &xa%d, &xb%d, &xd%d)) goto %s;\n",
               k, k, k, g->name, g->rules[t->rule].name, k, k, k, fail);
    }
    if (t->kind == SK_BIND_INT) {
        /* inline accumulation over the captured span — the matcher already
         * validated the digits; strtoll would copy and re-scan them (and is
         * a strtoll prefix-parse either way: stops at the first non-digit,
         * so `int` over a float span binds its integer part). */
        eb_fmt(e, "      (void)xd%d;\n"
                  "      { long long v%d = 0; size_t q%d = xa%d; int ng%d = 0;\n"
                  "        if (q%d < xb%d && s[q%d] == '-') { ng%d = 1; q%d++; }\n"
                  "        for (; q%d < xb%d && s[q%d] >= '0' && s[q%d] <= '9'; q%d++)\n"
                  "            v%d = v%d * 10 + (s[q%d] - '0');\n"
                  "        out->%s%s = ng%d ? -v%d : v%d; } }\n",
               k, k, k, k, k, k, k, k, k, k, k, k, k, k, k, k, k, k, cc__fpfx, t->field, k, k, k);
        return;
    }
    if (t->kind == SK_BIND_FLOAT) {
        /* Number text was borrowed by the keep; ffc parses JSON doubles here. */
        eb_fmt(e, "      (void)xd%d;\n"
                  "      { ffc_parse_options o%d; o%d.format = FFC_PRESET_JSON; o%d.decimal_point = '.';\n"
                  "        double v%d = 0.0;\n"
                  "        ffc_result r%d = ffc_from_chars_double_options(\n"
                  "            (const char*)(s + xa%d), (const char*)(s + xb%d), &v%d, o%d);\n"
                  "        if (r%d.outcome != FFC_OUTCOME_OK) goto %s;\n"
                  "        out->%s%s = v%d; } }\n",
               k, k, k, k, k, k, k, k, k, k, k, fail, cc__fpfx, t->field, k);
        return;
    }
    int cd = rs_rule_codec(g, t->rule);
    if (cd > 0) {
        /* clean spans borrow raw; dirty spans decode through the rule's codec
         * (same provenance contract as the collect/DOM tiers) */
        eb_fmt(e, "      if (!xd%d) out->%s%s = cc_slice_from_buffer((void*)(s + xa%d), xb%d - xa%d);\n"
                  "      else if (!%s((const char*)(s + xa%d), xb%d - xa%d, &out->%s%s, arena)) goto %s;\n"
                  "    }\n",
               k, cc__fpfx, t->field, k, k, k, g->codecs[cd - 1], k, k, k, cc__fpfx, t->field, fail);
    } else {
        eb_fmt(e, "      (void)xd%d;\n"
                  "      out->%s%s = cc_slice_from_buffer((void*)(s + xa%d), xb%d - xa%d);\n    }\n",
               k, cc__fpfx, t->field, k, k, k);
    }
}

static void rs_emit_term(SS* ss, RG* g, EB* e, int* lbl, int ti, const char* fail);

/* ---- format: the schema INVERTED (write side of SERDES) ----
 * A product schema is a byte template: literals emit verbatim, slice/bytes
 * binds emit their bytes, int binds emit decimal — and a length/count field
 * consumed by a later `bytes`/counted-`items` term is DERIVED from the data
 * (data.len / items_n), so output is correct by construction. Matcher terms
 * (pads) emit nothing: format is canonical. Member-list combinators need
 * codec inversion (escape ENCODING) and are not formatable yet. */

static int rs_derived_from(const SS* ss, int ti) {
    /* is terms[ti] (an int bind) the length/count source of a LATER term?
     * returns that term's index, or -1 */
    for (int j = ti + 1; j < ss->nterms; j++) {
        const STerm* t = &ss->terms[j];
        if (ss->vof[j] != ss->vof[ti]) continue;       /* stay in-variant */
        if ((t->kind == SK_BIND_BYTES ||
             (t->kind == SK_BIND_ITEMS && t->cfield[0])) &&
            strcmp(t->cfield, ss->terms[ti].field) == 0)
            return j;
    }
    return -1;
}

/* writable leaf shape: a bound rule whose body is (a seq of) literal prefix,
 * ONE keep, literal suffix — pads skipped (canonical). The quotes around a
 * JSON string are the rule's OWN literals; the write side derives them. */
typedef struct { int pre[4]; int npre; int post[4]; int npost; int codec; int ok; } RWLeaf;

static void rw_leaf_shape(const RG* g, int rule, RWLeaf* o) {
    memset(o, 0, sizeof *o);
    int body = g->rules[rule].node;
    while (g->nodes[body].kind == RN_COLLECT) body = g->nodes[body].a;
    if (g->nodes[body].kind == RN_KEEP) {
        o->codec = g->nodes[body].b;
        o->ok = 1;
        return;
    }
    if (g->nodes[body].kind != RN_SEQ) return;
    const RNode* x = &g->nodes[body];
    int seen_keep = 0;
    for (int i = 0; i < x->nkids; i++) {
        int kid = g->kids[x->b + i];
        const RNode* kn = &g->nodes[kid];
        if (kn->kind == RN_KEEP) {
            if (seen_keep) return;
            seen_keep = 1;
            o->codec = kn->b;
            continue;
        }
        if (kn->kind == RN_LIT) {
            if (!seen_keep) { if (o->npre < 4) o->pre[o->npre++] = kid; else return; }
            else            { if (o->npost < 4) o->post[o->npost++] = kid; else return; }
            continue;
        }
        if (kn->kind == RN_REF) {   /* pad refs skip on write (canonical) */
            const RNode* rn2 = kn;
            (void)rn2;
            continue;
        }
        return;
    }
    o->ok = seen_keep;
}

static int rs_formatable_term(const SS* ss, const RG* g, int ti);

static int rs_formatable_entries(const SS* ss, const RG* g, const STerm* t) {
    for (int i = 0; i < t->k_cnt; i++)
        if (!rs_formatable_term(ss, g, ss->keys[t->kidx[i]].term)) return 0;
    return 1;
}

static int rs_formatable_term(const SS* ss, const RG* g, int ti) {
    const STerm* t = &ss->terms[ti];
    switch (t->kind) {
    case SK_LIT: case SK_RULE: case SK_BIND_INT: case SK_BIND_FLOAT: case SK_BIND_BYTES:
        return 1;
    case SK_BIND_SLICE: {
        RWLeaf lf;
        rw_leaf_shape(g, t->rule, &lf);
        /* a decode codec means wire form != memory form: writing raw bytes
         * would be wrong the moment a payload needs escaping — formatable
         * only when the codec declares its /encode inverse */
        return lf.ok && (lf.codec == 0 || g->codenc[lf.codec - 1][0]);
    }
    case SK_BIND_ITEMS:
        return t->cfield[0] != 0;
    case SK_NARROW_MEMBERS:
        return rs_formatable_entries(ss, g, t);
    case SK_NARROW_LIST:
        return 1;
    default:
        return 0;   /* SK_FIELDS: directive combinators need their own emit rules */
    }
}

static int rs_formatable(const SS* ss, const RG* g) {
    for (int i = 0; i < ss->nbody; i++)
        if (!rs_formatable_term(ss, g, ss->body[i])) return 0;
    return 1;
}

/* ---- the write side: three projections of one template walk ----
 *   __wmeasure (md 0): exact output size, no stores — literals (including
 *       the whole structural skeleton of narrowed schemas) fold to ONE
 *       compile-time constant; ints count digits by compare chain (no
 *       divisions); encoders are asked with dst == NULL.
 *   __wput     (md 1): unchecked writer, space already proven — the to_str
 *       back end: measure once, allocate exactly, put.
 *   __wchk     (md 2): SINGLE-PASS checked writer — Name_write. Constant
 *       runs check once per batch, encoders get the real remaining cap and
 *       report exact need (snprintf-style), so encoder-heavy formats pay
 *       one encoding pass, not measure+put's two. */
enum { RW_MEASURE, RW_PUT, RW_CHK };

typedef struct {
    long cfix;                    /* measure: compile-time constant bytes */
    unsigned char lit[224];       /* put/chk: pending constant byte run */
    int nlit;
} WAcc;

static void rw_wacc_flush_put(EB* e, WAcc* w, int md) {
    if (w->nlit == 0) return;
    if (md == RW_CHK) eb_fmt(e, "    if (cap - o < %d) return 0;\n", w->nlit);
    if (w->nlit <= 4) {
        for (int i = 0; i < w->nlit; i++) {
            char cb[16];
            eb_fmt(e, "    dst[o++] = (char)%d; /*%s*/\n",
                   (int)w->lit[i], rw_chr((int)w->lit[i], cb));
        }
    } else {
        char esc[4 * 224 + 8];
        rs_esc(esc, sizeof esc, w->lit, w->nlit);
        eb_fmt(e, "    memcpy(dst + o, \"%s\", %d); o += %d;\n", esc, w->nlit, w->nlit);
    }
    w->nlit = 0;
}

static void rw_wacc_bytes(EB* e, WAcc* w, int md, const unsigned char* p, int n) {
    if (md == RW_MEASURE) { w->cfix += n; return; }
    for (int i = 0; i < n; i++) {
        if (w->nlit == (int)sizeof(w->lit)) rw_wacc_flush_put(e, w, md);
        w->lit[w->nlit++] = p[i];
    }
}

static void rw_wacc_lit_node(const RG* g, EB* e, WAcc* w, int md, int nd) {
    rw_wacc_bytes(e, w, md, (const unsigned char*)g->pool + g->nodes[nd].a, g->nodes[nd].b);
}

/* compare-chain digit count into `dest` — no divisions on the 1-6 digit
 * values wire formats actually carry; full 20-digit chain so it's total */
static void rw_emit_digits_expr(EB* e, int k, const char* dest) {
    unsigned long long p = 10;
    eb_fmt(e, "      %s ", dest);
    for (int d = 1; d <= 19; d++) {
        eb_fmt(e, "u%d < %lluULL ? %d\n         : ", k, p, d);
        if (d < 19) p *= 10;
    }
    eb_fmt(e, "20;\n");
}

static void rw_emit_wint(EB* e, WAcc* w, int* lbl, int md, const char* expr, int is_unsigned) {
    int k = (*lbl)++;
    char dest[32];
    if (md != RW_MEASURE) rw_wacc_flush_put(e, w, md);
    if (md == RW_PUT && is_unsigned) {
        eb_fmt(e, "    { unsigned long long u%d = (unsigned long long)(%s);\n"
                  "      char nb%d[21]; int nl%d = 0;\n"
                  "      do { nb%d[nl%d++] = (char)('0' + (u%d %% 10)); u%d /= 10; } while (u%d);\n"
                  "      while (nl%d) dst[o++] = nb%d[--nl%d]; }\n",
               k, expr, k, k, k, k, k, k, k, k, k, k);
    } else if (md == RW_PUT) {
        eb_fmt(e, "    { long long x%d = (long long)(%s);\n"
                  "      char nb%d[21]; int nl%d = 0;\n"
                  "      unsigned long long u%d = x%d < 0 ? (unsigned long long)-x%d : (unsigned long long)x%d;\n"
                  "      if (x%d < 0) dst[o++] = '-';\n"
                  "      do { nb%d[nl%d++] = (char)('0' + (u%d %% 10)); u%d /= 10; } while (u%d);\n"
                  "      while (nl%d) dst[o++] = nb%d[--nl%d]; }\n",
               k, expr, k, k, k, k, k, k, k, k, k, k, k, k, k, k, k);
    } else if (md == RW_CHK && is_unsigned) {
        eb_fmt(e, "    { unsigned long long u%d = (unsigned long long)(%s);\n"
                  "      size_t nd%d;\n", k, expr, k);
        snprintf(dest, sizeof dest, "nd%d =", k);
        rw_emit_digits_expr(e, k, dest);
        eb_fmt(e, "      if (cap - o < nd%d) return 0;\n"
                  "      o += nd%d;\n"
                  "      { size_t q%d = o;\n"
                  "        do { dst[--q%d] = (char)('0' + (u%d %% 10)); u%d /= 10; } while (u%d); } }\n",
               k, k, k, k, k, k, k);
    } else if (md == RW_CHK) {
        eb_fmt(e, "    { long long x%d = (long long)(%s);\n"
                  "      unsigned long long u%d = x%d < 0 ? (unsigned long long)-x%d : (unsigned long long)x%d;\n"
                  "      size_t nd%d;\n", k, expr, k, k, k, k, k);
        snprintf(dest, sizeof dest, "nd%d =", k);
        rw_emit_digits_expr(e, k, dest);
        eb_fmt(e, "      if (x%d < 0) nd%d++;\n"
                  "      if (cap - o < nd%d) return 0;\n"
                  "      o += nd%d;\n"
                  "      { size_t q%d = o;\n"
                  "        do { dst[--q%d] = (char)('0' + (u%d %% 10)); u%d /= 10; } while (u%d);\n"
                  "        if (x%d < 0) dst[--q%d] = '-'; } }\n",
               k, k, k, k, k, k, k, k, k, k, k);
    } else if (is_unsigned) {
        eb_fmt(e, "    { unsigned long long u%d = (unsigned long long)(%s);\n", k, expr);
        rw_emit_digits_expr(e, k, "o +=");
        eb_fmt(e, "    }\n");
    } else {
        eb_fmt(e, "    { long long x%d = (long long)(%s);\n"
                  "      unsigned long long u%d = x%d < 0 ? (unsigned long long)-x%d : (unsigned long long)x%d;\n"
                  "      if (x%d < 0) o++;\n",
               k, expr, k, k, k, k, k);
        rw_emit_digits_expr(e, k, "o +=");
        eb_fmt(e, "    }\n");
    }
}

/* slice-shaped payload through a leaf rule: pre-lits, bytes (encoded when the
 * rule's codec has an /encode inverse), post-lits */
static void rw_emit_wleaf(const RG* g, EB* e, WAcc* w, int* lbl, int md,
                          const RWLeaf* lf, const char* pexpr, const char* lexpr) {
    for (int i = 0; i < lf->npre; i++) rw_wacc_lit_node(g, e, w, md, lf->pre[i]);
    const char* enc = (lf->codec > 0 && g->codenc[lf->codec - 1][0])
                          ? g->codenc[lf->codec - 1] : NULL;
    if (md != RW_MEASURE) rw_wacc_flush_put(e, w, md);
    if (enc && md == RW_PUT) {
        eb_fmt(e, "    o += %s((const char*)%s, %s, dst + o, (size_t)-1);\n", enc, pexpr, lexpr);
    } else if (enc && md == RW_CHK) {
        int k = (*lbl)++;
        eb_fmt(e, "    { size_t en%d = %s((const char*)%s, %s, dst + o, cap - o);\n"
                  "      if (en%d > cap - o) return 0;\n"
                  "      o += en%d; }\n",
               k, enc, pexpr, lexpr, k, k);
    } else if (enc) {
        eb_fmt(e, "    o += %s((const char*)%s, %s, 0, 0);\n", enc, pexpr, lexpr);
    } else if (md == RW_PUT) {
        eb_fmt(e, "    memcpy(dst + o, %s, %s); o += %s;\n", pexpr, lexpr, lexpr);
    } else if (md == RW_CHK) {
        eb_fmt(e, "    if (cap - o < %s) return 0;\n"
                  "    memcpy(dst + o, %s, %s); o += %s;\n", lexpr, pexpr, lexpr, lexpr);
    } else {
        eb_fmt(e, "    o += %s;\n", lexpr);
    }
    for (int i = 0; i < lf->npost; i++) rw_wacc_lit_node(g, e, w, md, lf->post[i]);
}

static void rw_emit_wterm(SS* ss, const RG* g, EB* e, WAcc* w, int* lbl, int md, int ti);

static void rw_emit_wvalue(SS* ss, const RG* g, EB* e, WAcc* w, int* lbl, int md, int ti) {
    const STerm* t = &ss->terms[ti];
    char pe[192], le[192];
    switch (t->kind) {
    case SK_BIND_INT: {
        RWLeaf lf;
        rw_leaf_shape(g, t->rule, &lf);
        for (int i = 0; i < lf.npre; i++) rw_wacc_lit_node(g, e, w, md, lf.pre[i]);
        snprintf(pe, sizeof pe, "v->%s", t->field);
        rw_emit_wint(e, w, lbl, md, pe, 0);
        for (int i = 0; i < lf.npost; i++) rw_wacc_lit_node(g, e, w, md, lf.post[i]);
        break;
    }
    case SK_BIND_FLOAT: {
        RWLeaf lf;
        rw_leaf_shape(g, t->rule, &lf);
        for (int i = 0; i < lf.npre; i++) rw_wacc_lit_node(g, e, w, md, lf.pre[i]);
        {
            int k = (*lbl)++;
            if (md != RW_MEASURE) rw_wacc_flush_put(e, w, md);
            if (md == RW_PUT)
                eb_fmt(e, "    { char tb%d[64]; int n%d = snprintf(tb%d, sizeof tb%d, \"%%.17g\", v->%s);\n"
                          "      memcpy(dst + o, tb%d, (size_t)n%d); o += (size_t)n%d; }\n",
                       k, k, k, k, t->field, k, k, k);
            else if (md == RW_CHK)
                eb_fmt(e, "    { char tb%d[64]; int n%d = snprintf(tb%d, sizeof tb%d, \"%%.17g\", v->%s);\n"
                          "      if (n%d < 0 || cap - o < (size_t)n%d) return 0;\n"
                          "      memcpy(dst + o, tb%d, (size_t)n%d); o += (size_t)n%d; }\n",
                       k, k, k, k, t->field, k, k, k, k, k);
            else
                eb_fmt(e, "    { char tb%d[64]; o += (size_t)snprintf(tb%d, sizeof tb%d, \"%%.17g\", v->%s); }\n",
                       k, k, k, t->field);
        }
        for (int i = 0; i < lf.npost; i++) rw_wacc_lit_node(g, e, w, md, lf.post[i]);
        break;
    }
    case SK_BIND_SLICE: {
        RWLeaf lf;
        rw_leaf_shape(g, t->rule, &lf);
        snprintf(pe, sizeof pe, "v->%s.ptr", t->field);
        snprintf(le, sizeof le, "v->%s.len", t->field);
        rw_emit_wleaf(g, e, w, lbl, md, &lf, pe, le);
        break;
    }
    default:
        rw_emit_wterm(ss, g, e, w, lbl, md, ti);
        break;
    }
}

static void rw_emit_wterm(SS* ss, const RG* g, EB* e, WAcc* w, int* lbl, int md, int ti) {
    const STerm* t = &ss->terms[ti];
    char pe[192], le[192];
    switch (t->kind) {
    case SK_LIT:
        rw_wacc_bytes(e, w, md, t->lit, t->litlen);
        break;
    case SK_RULE:
        break;   /* pads/matchers: canonical output emits nothing */
    case SK_BIND_INT: {
        int dj = rs_derived_from(ss, ti);
        if (dj >= 0 && ss->terms[dj].kind == SK_BIND_BYTES)
            snprintf(pe, sizeof pe, "v->%s.len", ss->terms[dj].field);
        else if (dj >= 0)
            snprintf(pe, sizeof pe, "v->%s_n", ss->terms[dj].field);
        else
            snprintf(pe, sizeof pe, "v->%s", t->field);
        rw_emit_wint(e, w, lbl, md, pe, dj >= 0);
        break;
    }
    case SK_BIND_FLOAT: {
        int k = (*lbl)++;
        if (md != RW_MEASURE) rw_wacc_flush_put(e, w, md);
        if (md == RW_PUT)
            eb_fmt(e, "    { char tb%d[64]; int n%d = snprintf(tb%d, sizeof tb%d, \"%%.17g\", v->%s);\n"
                      "      memcpy(dst + o, tb%d, (size_t)n%d); o += (size_t)n%d; }\n",
                   k, k, k, k, t->field, k, k, k);
        else if (md == RW_CHK)
            eb_fmt(e, "    { char tb%d[64]; int n%d = snprintf(tb%d, sizeof tb%d, \"%%.17g\", v->%s);\n"
                      "      if (n%d < 0 || cap - o < (size_t)n%d) return 0;\n"
                      "      memcpy(dst + o, tb%d, (size_t)n%d); o += (size_t)n%d; }\n",
                   k, k, k, k, t->field, k, k, k, k, k);
        else
            eb_fmt(e, "    { char tb%d[64]; o += (size_t)snprintf(tb%d, sizeof tb%d, \"%%.17g\", v->%s); }\n",
                   k, k, k, t->field);
        break;
    }
    case SK_BIND_SLICE: {
        RWLeaf lf;
        rw_leaf_shape(g, t->rule, &lf);
        snprintf(pe, sizeof pe, "v->%s.ptr", t->field);
        snprintf(le, sizeof le, "v->%s.len", t->field);
        rw_emit_wleaf(g, e, w, lbl, md, &lf, pe, le);
        break;
    }
    case SK_BIND_BYTES:
        if (md == RW_MEASURE) {
            eb_fmt(e, "    o += v->%s.len;\n", t->field);
        } else {
            rw_wacc_flush_put(e, w, md);
            if (md == RW_CHK)
                eb_fmt(e, "    if (cap - o < v->%s.len) return 0;\n", t->field);
            eb_fmt(e, "    memcpy(dst + o, v->%s.ptr, v->%s.len); o += v->%s.len;\n",
                   t->field, t->field, t->field);
        }
        break;
    case SK_BIND_ITEMS: {   /* count-driven */
        int k = (*lbl)++;
        if (md != RW_MEASURE) rw_wacc_flush_put(e, w, md);
        eb_fmt(e, "    { size_t i%d;\n    for (i%d = 0; i%d < v->%s_n; i%d++)\n",
               k, k, k, t->field, k);
        if (md == RW_PUT)
            eb_fmt(e, "        o += %s__wput(&v->%s[i%d], dst + o);\n", t->etype, t->field, k);
        else if (md == RW_CHK)
            eb_fmt(e, "        if (!%s__wchk(&v->%s[i%d], dst, cap, &o)) return 0;\n",
                   t->etype, t->field, k);
        else
            eb_fmt(e, "        o += %s__wmeasure(&v->%s[i%d]);\n", t->etype, t->field, k);
        eb_fmt(e, "    }\n");
        break;
    }
    case SK_NARROW_MEMBERS: {
        const RNarrow* nw = &t->nw;
        unsigned char b;
        b = (unsigned char)nw->open_b;
        rw_wacc_bytes(e, w, md, &b, 1);
        for (int i = 0; i < t->k_cnt; i++) {
            const SKey* ke = &ss->keys[t->kidx[i]];
            if (i > 0) { b = (unsigned char)nw->sep_b; rw_wacc_bytes(e, w, md, &b, 1); }
            {   /* the key through its own leaf shape (quotes from the rule) */
                RWLeaf kl;
                rw_leaf_shape(g, nw->key_rule, &kl);
                for (int j = 0; j < kl.npre; j++) rw_wacc_lit_node(g, e, w, md, kl.pre[j]);
                rw_wacc_bytes(e, w, md, (const unsigned char*)ke->key, (int)strlen(ke->key));
                for (int j = 0; j < kl.npost; j++) rw_wacc_lit_node(g, e, w, md, kl.post[j]);
            }
            b = (unsigned char)nw->kv_b;
            rw_wacc_bytes(e, w, md, &b, 1);
            rw_emit_wvalue(ss, g, e, w, lbl, md, ke->term);
        }
        b = (unsigned char)nw->close_b;
        rw_wacc_bytes(e, w, md, &b, 1);
        break;
    }
    case SK_NARROW_LIST: {
        const RNarrow* nw = &t->nw;
        int k = (*lbl)++;
        unsigned char b = (unsigned char)nw->open_b;
        rw_wacc_bytes(e, w, md, &b, 1);
        if (md != RW_MEASURE) rw_wacc_flush_put(e, w, md);
        eb_fmt(e, "    { size_t i%d;\n    for (i%d = 0; i%d < v->%s_n; i%d++) {\n",
               k, k, k, t->field, k);
        if (md == RW_PUT)
            eb_fmt(e, "        if (i%d) dst[o++] = (char)%d;\n"
                      "        o += %s__wput(&v->%s[i%d], dst + o);\n",
                   k, nw->sep_b, t->etype, t->field, k);
        else if (md == RW_CHK)
            eb_fmt(e, "        if (i%d) { if (cap - o < 1) return 0; dst[o++] = (char)%d; }\n"
                      "        if (!%s__wchk(&v->%s[i%d], dst, cap, &o)) return 0;\n",
                   k, nw->sep_b, t->etype, t->field, k);
        else
            eb_fmt(e, "        if (i%d) o++;\n"
                      "        o += %s__wmeasure(&v->%s[i%d]);\n",
                   k, t->etype, t->field, k);
        eb_fmt(e, "    }\n    }\n");
        b = (unsigned char)nw->close_b;
        rw_wacc_bytes(e, w, md, &b, 1);
        break;
    }
    default:
        break;
    }
}

/* any method-shaped reference at all (`X_<m>`, `r.<m>(`)? An element schema's
 * writer is demanded by a LATER schema's generated code, and a value-receiver
 * UFCS call (`cmd.to_str(&a)`) cannot be associated with its type by a
 * per-name scan — so formatable schemas also emit under this loose
 * file-level demand, tagged unused-ok. */
static int rs_any_method_demand(const char* src, size_t n, const char* m) {
    size_t ml = strlen(m);
    for (size_t i = 1; i + ml <= n; i++) {
        if (src[i] != m[0] || memcmp(src + i, m, ml) != 0) continue;
        if (src[i - 1] != '_' && src[i - 1] != '.') continue;
        if (i + ml < n && (isalnum((unsigned char)src[i + ml]) || src[i + ml] == '_')) continue;
        return 1;
    }
    return 0;
}

/* ---- reflection face: keys separated from values ----
 * The schema's key->field knowledge, KEPT after parse instead of consumed
 * by it: a static per-TYPE table (name, kind, offset — the comptime hidden
 * class) plus a compiled get() whose dispatch is the same length-switch +
 * memcmp the parser uses. Instances stay packed C; the table is shared by
 * all of them. This is the comptime rung of the shapes ladder (see
 * examples/serdes/json/json_shape.h for the runtime rungs). */
static void rs_collect_binds(const SS* ss, int ti, int* order, int* cnt);

static const char* rs_gk(const STerm* t) {
    switch (t->kind) {
    case SK_BIND_INT:   return "CC_GK_INT";
    case SK_BIND_FLOAT: return "CC_GK_FLOAT";
    case SK_BIND_BYTES: return "CC_GK_BYTES";
    case SK_BIND_ITEMS: case SK_NARROW_LIST: return "CC_GK_ITEMS";
    default:            return "CC_GK_SLICE";
    }
}

static void rs_emit_get(SS* ss, EB* e, const char* name) {
    int order[S_MAX_TERMS]; int cnt = 0;
    for (int i = 0; i < ss->nbody; i++) rs_collect_binds(ss, ss->body[i], order, &cnt);
    eb_fmt(e, "static const CCGramField %s__fields[%d] = {\n", name, cnt > 0 ? cnt : 1);
    for (int i = 0; i < cnt; i++) {
        const STerm* t = &ss->terms[order[i]];
        int is_items = t->kind == SK_BIND_ITEMS || t->kind == SK_NARROW_LIST;
        if (is_items)
            eb_fmt(e, "    { \"%s\", %s, (unsigned)offsetof(%s, %s), "
                      "(unsigned)offsetof(%s, %s_n), \"%s\" },\n",
                   t->field, rs_gk(t), name, t->field, name, t->field, t->etype);
        else
            eb_fmt(e, "    { \"%s\", %s, (unsigned)offsetof(%s, %s), 0, 0 },\n",
                   t->field, rs_gk(t), name, t->field);
    }
    if (cnt == 0) eb_fmt(e, "    { 0, 0, 0, 0, 0 },\n");
    eb_fmt(e, "};\n");
    eb_fmt(e, "static __attribute__((unused)) const CCGramField* %s_field(const char* k, size_t kl) {\n"
              "    switch (kl) {\n", name);
    {
        unsigned char done[S_MAX_TERMS] = {0};
        for (int i = 0; i < cnt; i++) {
            if (done[i]) continue;
            size_t L = strlen(ss->terms[order[i]].field);
            eb_fmt(e, "    case %d:\n", (int)L);
            for (int j = i; j < cnt; j++) {
                const STerm* t = &ss->terms[order[j]];
                if (done[j] || strlen(t->field) != L) continue;
                done[j] = 1;
                eb_fmt(e, "        if (memcmp(k, \"%s\", %d) == 0) return &%s__fields[%d];\n",
                       t->field, (int)L, name, j);
            }
            eb_fmt(e, "        break;\n");
        }
    }
    eb_fmt(e, "    }\n    return 0;\n}\n");
    /* compiled get: dispatch straight to member reads — the field table is
     * the reflective face, this is the fast one */
    eb_fmt(e, "static __attribute__((unused)) int %s_get(const %s* v, const char* k, CCGramValue* out) {\n"
              "    size_t kl = strlen(k);\n    (void)v;\n"
              "    switch (kl) {\n", name, name);
    {
        unsigned char done[S_MAX_TERMS] = {0};
        for (int i = 0; i < cnt; i++) {
            if (done[i]) continue;
            size_t L = strlen(ss->terms[order[i]].field);
            eb_fmt(e, "    case %d:\n", (int)L);
            for (int j = i; j < cnt; j++) {
                const STerm* t = &ss->terms[order[j]];
                if (done[j] || strlen(t->field) != L) continue;
                done[j] = 1;
                eb_fmt(e, "        if (memcmp(k, \"%s\", %d) == 0) {\n"
                          "            out->kind = %s;\n"
                          "            out->field = &%s__fields[%d];\n",
                       t->field, (int)L, rs_gk(t), name, j);
                if (ss->presence)
                    eb_fmt(e, "            out->present = (int)((v->cc__set >> %d) & 1);\n", j);
                else
                    eb_fmt(e, "            out->present = 1;\n");
                if (t->kind == SK_BIND_INT)
                    eb_fmt(e, "            out->i = v->%s;\n", t->field);
                else if (t->kind == SK_BIND_FLOAT)
                    eb_fmt(e, "            out->f = v->%s;\n", t->field);
                else if (t->kind == SK_BIND_ITEMS || t->kind == SK_NARROW_LIST)
                    eb_fmt(e, "            out->items = v->%s; out->items_n = v->%s_n;\n",
                           t->field, t->field);
                else
                    eb_fmt(e, "            out->s = v->%s;\n", t->field);
                eb_fmt(e, "            return 1;\n        }\n");
            }
            eb_fmt(e, "        break;\n");
        }
    }
    eb_fmt(e, "    }\n    return 0;\n}\n");
}

static void rs_emit_write(SS* ss, const RG* g, EB* e, int* lbl, const char* name) {
    for (int md = RW_MEASURE; md <= RW_CHK; md++) {
        WAcc w;
        memset(&w, 0, sizeof w);
        if (md == RW_PUT)
            eb_fmt(e, "static __attribute__((unused)) size_t %s__wput(const %s* v, char* dst) {\n"
                      "    size_t o = 0;\n    (void)v;\n", name, name);
        else if (md == RW_CHK)
            eb_fmt(e, "static __attribute__((unused)) int %s__wchk(const %s* v, char* dst, size_t cap, size_t* io) {\n"
                      "    size_t o = *io;\n    (void)v;\n", name, name);
        else
            eb_fmt(e, "static __attribute__((unused)) size_t %s__wmeasure(const %s* v) {\n"
                      "    size_t o = 0;\n    (void)v;\n", name, name);
        for (int i = 0; i < ss->nbody; i++)
            rw_emit_wterm(ss, g, e, &w, lbl, md, ss->body[i]);
        if (md != RW_MEASURE) rw_wacc_flush_put(e, &w, md);
        else if (w.cfix) eb_fmt(e, "    o += %ld;   /* structural skeleton */\n", w.cfix);
        if (md == RW_CHK) eb_fmt(e, "    *io = o;\n    return 1;\n}\n");
        else eb_fmt(e, "    return o;\n}\n");
    }
    eb_fmt(e, "static __attribute__((unused)) size_t %s_write(const %s* v, char* dst, size_t cap) {\n"
              "    size_t o = 0;\n"
              "    if (!%s__wchk(v, dst, cap, &o)) return 0;\n"
              "    return o;\n}\n",
           name, name, name);
}

static void rw_emit_pads(RG* g, EB* e, const int* pads, int npads, const char* fail) {
    for (int i = 0; i < npads; i++) rs_emit_pad(g, e, pads[i], fail);
}

/* narrowed member list: every delimiter, pad, and the unknown-member skip
 * come from the decomposed rule — the schema contributed only the bindings */
static void rs_emit_narrow_members(SS* ss, RG* g, EB* e, int* lbl, const STerm* t, const char* fail) {
    const RNarrow* w = &t->nw;
    int k = (*lbl)++;
    eb_fmt(e, "    if (!(p < n && s[p] == %d)) goto %s;\n    p++;\n", w->open_b, fail);
    rw_emit_pads(g, e, w->lpad, w->nlpad, fail);
    eb_fmt(e, "    if (p < n && s[p] != %d) {\n    for (;;) {\n", w->close_b);
    rw_emit_pads(g, e, w->mpad_a, w->nmpad_a, fail);
    eb_fmt(e, "    { size_t xa%d, xb%d; int xd%d;\n"
              "      if (!%s__x_%s(s, n, &p, &xa%d, &xb%d, &xd%d)) goto %s;\n"
              "      (void)xd%d;\n",
           k, k, k, g->name, g->rules[w->key_rule].name, k, k, k, fail, k);
    rw_emit_pads(g, e, w->mpad_b, w->nmpad_b, fail);
    eb_fmt(e, "      if (!(p < n && s[p] == %d)) goto %s;\n      p++;\n", w->kv_b, fail);
    eb_fmt(e, "      switch (xb%d - xa%d) {\n", k, k);
    {
        unsigned char done[S_MAX_KEYS] = {0};
        for (int i = 0; i < t->k_cnt; i++) {
            if (done[i]) continue;
            const SKey* ki = &ss->keys[t->kidx[i]];
            size_t L = strlen(ki->key);
            eb_fmt(e, "      case %d:\n", (int)L);
            for (int j = i; j < t->k_cnt; j++) {
                const SKey* kj = &ss->keys[t->kidx[j]];
                if (done[j] || strlen(kj->key) != L) continue;
                done[j] = 1;
                char esc[4 * S_NAME];
                rs_esc(esc, sizeof esc, (const unsigned char*)kj->key, (int)L);
                eb_fmt(e, "        if (memcmp(s + xa%d, \"%s\", %d) == 0) {\n", k, esc, (int)L);
                rw_emit_pads(g, e, w->vpad_a, w->nvpad_a, fail);
                rs_emit_term(ss, g, e, lbl, kj->term, fail);
                rw_emit_pads(g, e, w->vpad_b, w->nvpad_b, fail);
                eb_fmt(e, "          break; }\n");
            }
            eb_fmt(e, "        goto Ld%d;\n", k);
        }
    }
    eb_fmt(e, "      default: goto Ld%d;\n      }\n      goto Ln%d;\n", k, k);
    eb_fmt(e, "Ld%d:\n", k);
    eb_fmt(e, "      if (!%s__%s_%s(s, n, &p)) goto %s;\n",
           g->name, rs_class(g, e->K, w->val_rule), g->rules[w->val_rule].name, fail);
    eb_fmt(e, "Ln%d: ;\n    }\n", k);
    eb_fmt(e, "    if (p < n && s[p] == %d) { p++; continue; }\n    break;\n    }\n    }\n", w->sep_b);
    rw_emit_pads(g, e, w->tpad, w->ntpad, fail);
    eb_fmt(e, "    if (!(p < n && s[p] == %d)) goto %s;\n    p++;\n", w->close_b, fail);
}

/* narrowed list: array shape from the rule, elements parsed as the schema
 * (placed at the element rule's core, inside its own pads) */
static void rs_emit_narrow_list(RG* g, EB* e, int* lbl, const STerm* t, const char* fail) {
    const RNarrow* w = &t->nw;
    int k = (*lbl)++;
    const char* T = t->etype;
    eb_fmt(e, "    { size_t cap%d = 8, cnt%d = 0;\n"
              "    %s* v%d = (%s*)cc_arena_alloc_local(arena, cap%d * sizeof(%s), _Alignof(%s));\n"
              "    if (!v%d) goto %s;\n",
           k, k, T, k, T, k, T, T, k, fail);
    eb_fmt(e, "    if (!(p < n && s[p] == %d)) goto %s;\n    p++;\n", w->open_b, fail);
    rw_emit_pads(g, e, w->lpad, w->nlpad, fail);
    eb_fmt(e, "    if (p < n && s[p] != %d) {\n    for (;;) {\n", w->close_b);
    eb_fmt(e, "    if (cnt%d == cap%d) {\n"
              "        %s* nv%d = (%s*)cc_arena_realloc(arena, arena, v%d,\n"
              "            cap%d * sizeof(%s), cap%d * 2 * sizeof(%s), _Alignof(%s));\n"
              "        if (!nv%d) goto %s;\n        v%d = nv%d; cap%d *= 2;\n    }\n",
           k, k, T, k, T, k, k, T, k, T, T, k, fail, k, k, k);
    rw_emit_pads(g, e, w->vpad_a, w->nvpad_a, fail);
    eb_fmt(e, "    if (!%s__fill(s, n, &p, arena, &v%d[cnt%d], cc_inc, cc_depth + 1)) goto %s;\n    cnt%d++;\n",
           T, k, k, fail, k);
    rw_emit_pads(g, e, w->vpad_b, w->nvpad_b, fail);
    eb_fmt(e, "    if (p < n && s[p] == %d) { p++; continue; }\n    break;\n    }\n    }\n", w->sep_b);
    rw_emit_pads(g, e, w->tpad, w->ntpad, fail);
    eb_fmt(e, "    if (!(p < n && s[p] == %d)) goto %s;\n    p++;\n", w->close_b, fail);
    eb_fmt(e, "    out->%s%s = v%d; out->%s%s_n = cnt%d; }\n", cc__fpfx, t->field, k, cc__fpfx, t->field, k);
}

static void rs_emit_fields(SS* ss, RG* g, EB* e, int* lbl, const STerm* t, const char* fail) {
    int k = (*lbl)++;
    eb_fmt(e, "    if (!(p < n && s[p] == %d)) goto %s;\n    p++;\n", ss->fo, fail);
    rs_emit_pad(g, e, ss->rfpad, fail);
    eb_fmt(e, "    if (p < n && s[p] != %d) {\n    for (;;) {\n", ss->fc);
    eb_fmt(e, "    { size_t xa%d, xb%d; int xd%d;\n"
              "      if (!%s__x_%s(s, n, &p, &xa%d, &xb%d, &xd%d)) goto %s;\n"
              "      (void)xd%d;\n",
           k, k, k, g->name, g->rules[ss->rfkey].name, k, k, k, fail, k);
    rs_emit_pad(g, e, ss->rfpad, fail);
    eb_fmt(e, "      if (!(p < n && s[p] == %d)) goto %s;\n      p++;\n", ss->fkv, fail);
    rs_emit_pad(g, e, ss->rfpad, fail);
    /* key dispatch: switch on length, memcmp chain within a length class */
    eb_fmt(e, "      switch (xb%d - xa%d) {\n", k, k);
    {
        unsigned char done[S_MAX_KEYS] = {0};
        for (int i = 0; i < t->k_cnt; i++) {
            if (done[i]) continue;
            const SKey* ki = &ss->keys[t->kidx[i]];
            size_t L = strlen(ki->key);
            eb_fmt(e, "      case %d:\n", (int)L);
            for (int j = i; j < t->k_cnt; j++) {
                const SKey* kj = &ss->keys[t->kidx[j]];
                if (done[j] || strlen(kj->key) != L) continue;
                done[j] = 1;
                char esc[4 * S_NAME];
                rs_esc(esc, sizeof esc, (const unsigned char*)kj->key, (int)L);
                eb_fmt(e, "        if (memcmp(s + xa%d, \"%s\", %d) == 0) {\n", k, esc, (int)L);
                rs_emit_term(ss, g, e, lbl, kj->term, fail);
                eb_fmt(e, "          break; }\n");
            }
            eb_fmt(e, "        goto Ld%d;\n", k);
        }
    }
    eb_fmt(e, "      default: goto Ld%d;\n      }\n      goto Ln%d;\n", k, k);
    eb_fmt(e, "Ld%d:\n", k);
    if (ss->rfelse >= 0)
        eb_fmt(e, "      if (!%s__%s_%s(s, n, &p)) goto %s;\n",
               g->name, rs_class(g, e->K, ss->rfelse), g->rules[ss->rfelse].name, fail);
    else
        eb_fmt(e, "      goto %s;   /* unknown member and no else rule */\n", fail);
    eb_fmt(e, "Ln%d: ;\n    }\n", k);
    rs_emit_pad(g, e, ss->rfpad, fail);
    eb_fmt(e, "    if (p < n && s[p] == %d) { p++;\n", ss->fs);
    rs_emit_pad(g, e, ss->rfpad, fail);
    eb_fmt(e, "    continue; }\n    break;\n    }\n    }\n");
    eb_fmt(e, "    if (!(p < n && s[p] == %d)) goto %s;\n    p++;\n", ss->fc, fail);
}

static void rs_emit_bytes(EB* e, int* lbl, const STerm* t, const char* fail) {
    int k = (*lbl)++;
    /* exactly out-><cfield> bytes, borrowed — the length was parsed, so the
     * payload is opaque: \r\n, NUL, anything. p <= n always, so n - p is safe. */
    eb_fmt(e, "    { long long L%d = out->%s%s;\n"
              "      if (L%d < 0) goto %s;\n"
              "      if ((unsigned long long)L%d > (unsigned long long)(n - p)) { *cc_inc = 1; goto %s; }\n"
              "      out->%s%s = cc_slice_from_buffer((void*)(s + p), (size_t)L%d);\n"
              "      p += (size_t)L%d; }\n",
           k, cc__fpfx, t->cfield, k, fail, k, fail, cc__fpfx, t->field, k, k);
}

static void rs_emit_items_counted(EB* e, int* lbl, const STerm* t, const char* fail) {
    int k = (*lbl)++;
    const char* T = t->etype;
    if (t->cap > 0) {
        /* comptime cap: elements fill the struct's INLINE array — no
         * allocation at all; a larger count is a protocol error */
        eb_fmt(e, "    { long long C%d = out->%s%s;\n"
                  "      if (C%d < 0 || C%d > %d) goto %s;\n"
                  "      if ((unsigned long long)C%d > n - p) { *cc_inc = 1; goto %s; }\n"
                  "      for (long long i%d = 0; i%d < C%d; i%d++)\n"
                  "          if (!%s__fill(s, n, &p, arena, &out->%s%s[i%d], cc_inc, cc_depth + 1)) goto %s;\n"
                  "      out->%s%s_n = (size_t)C%d; }\n",
               k, cc__fpfx, t->cfield, k, k, t->cap, fail, k, fail,
               k, k, k, k, T, cc__fpfx, t->field, k, fail, cc__fpfx, t->field, k);
        return;
    }
    /* the count is data: exact-size allocation, no realloc, no delimiters */
    eb_fmt(e, "    { long long C%d = out->%s%s;\n"
              "      if (C%d < 0) goto %s;\n"
              "      if ((unsigned long long)C%d > n - p) { *cc_inc = 1; goto %s; }\n"
              "      size_t cap%d = C%d > 0 ? (size_t)C%d : 1;\n"
              "      %s* v%d = (%s*)cc_arena_alloc_local(arena, cap%d * sizeof(%s), _Alignof(%s));\n"
              "      if (!v%d) goto %s;\n"
              "      for (long long i%d = 0; i%d < C%d; i%d++)\n"
              "          if (!%s__fill(s, n, &p, arena, &v%d[i%d], cc_inc, cc_depth + 1)) goto %s;\n"
              "      out->%s%s = v%d; out->%s%s_n = (size_t)C%d; }\n",
           k, cc__fpfx, t->cfield, k, fail, k, fail, k, k, k, T, k, T, k, T, T, k, fail,
           k, k, k, k, T, k, k, fail, cc__fpfx, t->field, k, cc__fpfx, t->field, k);
}

static void rs_emit_items(SS* ss, RG* g, EB* e, int* lbl, const STerm* t, const char* fail) {
    int k = (*lbl)++;
    const char* T = t->etype;
    eb_fmt(e, "    { size_t cap%d = 8, cnt%d = 0;\n"
              "    %s* v%d = (%s*)cc_arena_alloc_local(arena, cap%d * sizeof(%s), _Alignof(%s));\n"
              "    if (!v%d) goto %s;\n",
           k, k, T, k, T, k, T, T, k, fail);
    eb_fmt(e, "    if (!(p < n && s[p] == %d)) goto %s;\n    p++;\n", ss->io_, fail);
    rs_emit_pad(g, e, ss->ripad, fail);
    eb_fmt(e, "    if (p < n && s[p] != %d) {\n    for (;;) {\n", ss->ic_);
    eb_fmt(e, "    if (cnt%d == cap%d) {\n"
              "        %s* nv%d = (%s*)cc_arena_realloc(arena, arena, v%d,\n"
              "            cap%d * sizeof(%s), cap%d * 2 * sizeof(%s), _Alignof(%s));\n"
              "        if (!nv%d) goto %s;\n        v%d = nv%d; cap%d *= 2;\n    }\n",
           k, k, T, k, T, k, k, T, k, T, T, k, fail, k, k, k);
    eb_fmt(e, "    if (!%s__fill(s, n, &p, arena, &v%d[cnt%d], cc_inc, cc_depth + 1)) goto %s;\n    cnt%d++;\n",
           T, k, k, fail, k);
    rs_emit_pad(g, e, ss->ripad, fail);
    eb_fmt(e, "    if (p < n && s[p] == %d) { p++;\n", ss->is_);
    rs_emit_pad(g, e, ss->ripad, fail);
    eb_fmt(e, "    continue; }\n    break;\n    }\n    }\n");
    eb_fmt(e, "    if (!(p < n && s[p] == %d)) goto %s;\n    p++;\n", ss->ic_, fail);
    eb_fmt(e, "    out->%s%s = v%d; out->%s%s_n = cnt%d; }\n", cc__fpfx, t->field, k, cc__fpfx, t->field, k);
}

static void rs_emit_presence(const SS* ss, EB* e, int ti) {
    if (ss->presence && ss->bindbit[ti] >= 0)
        eb_fmt(e, "    out->cc__set |= 1ULL << %d;\n", ss->bindbit[ti]);
}

static void rs_emit_term(SS* ss, RG* g, EB* e, int* lbl, int ti, const char* fail) {
    const STerm* t = &ss->terms[ti];
    switch (t->kind) {
    case SK_LIT:
        /* bounds-driven failure = INCOMPLETE (the frame ran past the bytes
         * given); content-driven = malformed. Exactly decidable here. */
        if (t->litlen == 1) {
            char cb[16];
            eb_fmt(e, "    if (p >= n) { *cc_inc = 1; goto %s; }\n"
                      "    if (s[p] != %d /*%s*/) goto %s;\n    p++;\n",
                   fail, (int)t->lit[0], rw_chr((int)t->lit[0], cb), fail);
        } else {
            char esc[128];
            rs_esc(esc, sizeof esc, t->lit, t->litlen);
            eb_fmt(e, "    if (p + %d > n) { *cc_inc = 1; goto %s; }\n"
                      "    if (memcmp(s + p, \"%s\", %d) != 0) goto %s;\n"
                      "    p += %d;\n",
                   t->litlen, fail, esc, t->litlen, fail, t->litlen);
        }
        break;
    case SK_RULE:
        eb_fmt(e, "    if (!%s__%s_%s(s, n, &p)) goto %s;\n",
               g->name, rs_class(g, e->K, t->rule), g->rules[t->rule].name, fail);
        break;
    case SK_BIND_SLICE:
    case SK_BIND_INT:
    case SK_BIND_FLOAT:
        rs_emit_bind_value(ss, g, e, lbl, t, fail);
        rs_emit_presence(ss, e, ti);
        break;
    case SK_BIND_ITEMS:
        if (t->cfield[0]) rs_emit_items_counted(e, lbl, t, fail);
        else rs_emit_items(ss, g, e, lbl, t, fail);
        rs_emit_presence(ss, e, ti);
        break;
    case SK_BIND_BYTES:
        rs_emit_bytes(e, lbl, t, fail);
        rs_emit_presence(ss, e, ti);
        break;
    case SK_FIELDS:
        rs_emit_fields(ss, g, e, lbl, t, fail);
        break;
    case SK_NARROW_MEMBERS:
        rs_emit_narrow_members(ss, g, e, lbl, t, fail);
        break;
    case SK_NARROW_LIST:
        rs_emit_narrow_list(g, e, lbl, t, fail);
        rs_emit_presence(ss, e, ti);
        break;
    case SK_UNION: {
        /* first-byte dispatch; the default arm (if any) takes everything
         * else. Recursion is bounded: self-referential variants ride the
         * items depth counter. */
        int defv = -1;
        eb_fmt(e, "    if (p >= n) { *cc_inc = 1; goto %s; }\n", fail);
        eb_fmt(e, "    if (cc_depth > 128) goto %s;\n", fail);
        eb_fmt(e, "    switch (s[p]) {\n");
        for (int vi = 0; vi < ss->nuv; vi++) {
            if (ss->uv[vi].db < 0) { defv = vi; continue; }
            char cb[16];
            eb_fmt(e, "    case %d: /*%s*/ {\n        out->kind = %s_%s;\n",
                   ss->uv[vi].db, rw_chr(ss->uv[vi].db, cb), cc__sname, ss->uv[vi].name);
            snprintf(cc__fpfx, sizeof cc__fpfx, "u.%s.", ss->uv[vi].name);
            for (int j = 0; j < ss->uv[vi].nt; j++)
                rs_emit_term(ss, g, e, lbl, ss->uv[vi].t[j], fail);
            cc__fpfx[0] = '\0';
            eb_fmt(e, "        break; }\n");
        }
        if (defv >= 0) {
            eb_fmt(e, "    default: {\n        out->kind = %s_%s;\n",
                   cc__sname, ss->uv[defv].name);
            snprintf(cc__fpfx, sizeof cc__fpfx, "u.%s.", ss->uv[defv].name);
            for (int j = 0; j < ss->uv[defv].nt; j++)
                rs_emit_term(ss, g, e, lbl, ss->uv[defv].t[j], fail);
            cc__fpfx[0] = '\0';
            eb_fmt(e, "        break; }\n");
        } else {
            eb_fmt(e, "    default: goto %s;\n", fail);
        }
        eb_fmt(e, "    }\n");
        break;
    }
    }
}

static int rs_find_int_field(const SS* ss, int before, const char* nm) {
    for (int i = 0; i < before; i++) {
        if (ss->vof[i] != ss->vof[before]) continue;   /* stay in-variant */
        if (ss->terms[i].kind == SK_BIND_INT && strcmp(ss->terms[i].field, nm) == 0) return i;
    }
    return -1;
}

static void rs_collect_binds(const SS* ss, int ti, int* order, int* cnt) {
    const STerm* t = &ss->terms[ti];
    if (t->kind == SK_BIND_SLICE || t->kind == SK_BIND_INT || t->kind == SK_BIND_FLOAT ||
        t->kind == SK_BIND_ITEMS || t->kind == SK_BIND_BYTES ||
        t->kind == SK_NARROW_LIST) {
        order[(*cnt)++] = ti;
    } else if (t->kind == SK_FIELDS || t->kind == SK_NARROW_MEMBERS) {
        for (int i = 0; i < t->k_cnt; i++)
            rs_collect_binds(ss, ss->keys[t->kidx[i]].term, order, cnt);
    }
    /* SK_UNION: variant fields live inside the union, not as flat binds */
}

static int rs_has_token(const char* src, size_t n, const char* name, const char* suffix);
static int rs_has_op(const char* src, size_t n, const char* op, const char* name);
static int rs_has_dot(const char* src, size_t n, const char* name, const char* method);

static char* cc__schema_emit(const char* name, const char* body, size_t body_len,
                             const char* file, int line,
                             const char* src, size_t src_len,
                             char* err, size_t err_sz) {
    (void)src; (void)src_len;
    SS* ss = (SS*)calloc(1, sizeof(SS));
    RG* g = NULL; RFirst* F = NULL; RKeeps* K = NULL;
    char* out = NULL; size_t len = 0, cap = 0;
    if (!ss) { snprintf(err, err_sz, "@grammar(schema): out of memory"); return NULL; }
    ss->b = body; ss->n = body_len; ss->line0 = line;
    ss->uterm = -1;
    memset(ss->vof, -1, sizeof ss->vof);

    if (ss_parse(ss)) {
        snprintf(err, err_sz, "@grammar(schema) %s: %s (at line %d)",
                 name, ss->err, ss_line_at(ss, ss->err_at));
        goto done;
    }
    SRulesReg* reg = NULL;
    if (ss->usepath[0]) {
        reg = cc__use_rules_file(file, ss->usepath, ss->usename);
        if (!reg) {
            snprintf(err, err_sz, "@grammar(schema) %s: cannot load factory \"%s\" "
                     "(path is relative to this source file)", name, ss->usepath);
            goto done;
        }
    } else if (ss->usename[0]) {
        reg = cc__find_rules(ss->usename);
        if (!reg) {
            snprintf(err, err_sz, "@grammar(schema) %s: unknown rules grammar '%s' "
                     "(a @grammar(rules) %s block must appear earlier in this file, "
                     "or use \"path.rules\" as %s for a file-backed factory)",
                     name, ss->usename, ss->usename, ss->usename);
            goto done;
        }
    }
    g = (RG*)calloc(1, sizeof(RG));
    F = (RFirst*)calloc(1, sizeof(RFirst));
    K = (RKeeps*)calloc(1, sizeof(RKeeps));
    if (!g || !F || !K) { snprintf(err, err_sz, "@grammar(schema): out of memory"); goto done; }
    if (reg) {   /* composed: shared matchers under the <Rules>__s prefix */
        g->body = reg->body; g->n = reg->blen; g->name = reg->pfx;
    } else {     /* self-contained: inline rules, private matchers */
        g->body = ss->rtext; g->n = ss->rlen; g->name = name;
    }
    g->file = file; g->line0 = line;
    if (rg_parse(g) != 0) {
        snprintf(err, err_sz, "@grammar(schema) %s: %s grammar failed to parse: %s",
                 name, reg ? "used" : "inline", g->err);
        goto done;
    }
    /* resolve rule references */
    {
        struct { char* nm; int* dst; } dir[4] = {
            { ss->fkey, &ss->rfkey }, { ss->fpad, &ss->rfpad },
            { ss->felse, &ss->rfelse }, { ss->ipad, &ss->ripad },
        };
        for (int i = 0; i < 4; i++) {
            if (!dir[i].nm[0]) continue;
            int r = rs_rule_by_name(g, dir[i].nm);
            if (r < 0) {
                snprintf(err, err_sz, "@grammar(schema) %s: unknown rule '%s.%s'",
                         name, ss->usename, dir[i].nm);
                goto done;
            }
            *dir[i].dst = r;
        }
        for (int ti = 0; ti < ss->nterms; ti++) {
            STerm* t = &ss->terms[ti];
            if (t->kind != SK_RULE && t->kind != SK_BIND_SLICE && t->kind != SK_BIND_INT &&
                t->kind != SK_BIND_FLOAT &&
                t->kind != SK_NARROW_MEMBERS && t->kind != SK_NARROW_LIST) continue;
            t->rule = rs_rule_by_name(g, t->rname);
            if (t->rule < 0) {
                snprintf(err, err_sz, "@grammar(schema) %s: unknown rule '%s.%s'",
                         name, ss->usename, t->rname);
                goto done;
            }
            if ((t->kind == SK_BIND_SLICE || t->kind == SK_BIND_INT || t->kind == SK_BIND_FLOAT) &&
                !rs_rule_has_keep(g, t->rule)) {
                snprintf(err, err_sz, "@grammar(schema) %s: rule '%s.%s' has no keep to extract",
                         name, ss->usename, t->rname);
                goto done;
            }
            if (t->kind == SK_NARROW_MEMBERS) {
                if (!rw_match_list(g, F, K, t->rule, &t->nw) ||
                    !rw_match_member(g, F, K, &t->nw)) {
                    snprintf(err, err_sz, "@grammar(schema) %s: rule '%s.%s' does not "
                             "narrow to a member list (need: open pad* opt[member "
                             "any[sep member]] pad* close; member: pad* key pad* kv value)",
                             name, ss->usename, t->rname);
                    goto done;
                }
                rw_match_value(g, F, K, t->nw.val_rule, &t->nw);
            }
            if (t->kind == SK_NARROW_LIST) {
                if (!rw_match_list(g, F, K, t->rule, &t->nw)) {
                    snprintf(err, err_sz, "@grammar(schema) %s: rule '%s.%s' does not "
                             "narrow to a list (need: open pad* opt[elem any[sep elem]] pad* close)",
                             name, ss->usename, t->rname);
                    goto done;
                }
                rw_match_value(g, F, K, t->nw.elem, &t->nw);
                if (!cc__schema_known(t->etype)) {
                    snprintf(err, err_sz, "@grammar(schema) %s: unknown schema '%s' "
                             "(must be declared earlier in this file)", name, t->etype);
                    goto done;
                }
            }
            if (t->kind == SK_BIND_ITEMS && strcmp(t->etype, name) != 0 &&
                !cc__schema_known(t->etype)) {
                snprintf(err, err_sz, "@grammar(schema) %s: unknown schema '%s' "
                         "(must be declared earlier in this file)", name, t->etype);
                goto done;
            }
        }
        for (int ti = 0; ti < ss->nterms; ti++) {
            STerm* t = &ss->terms[ti];
            if (t->kind == SK_BIND_ITEMS && strcmp(t->etype, name) != 0 &&
                !cc__schema_known(t->etype)) {
                snprintf(err, err_sz, "@grammar(schema) %s: unknown schema '%s' "
                         "(must be declared earlier in this file)", name, t->etype);
                goto done;
            }
            if (t->kind == SK_FIELDS &&
                (ss->fo < 0 || ss->fc < 0 || ss->fs < 0 || ss->fkv < 0 || ss->rfkey < 0)) {
                snprintf(err, err_sz, "@grammar(schema) %s: fields [...] requires a "
                         "`fields:` directive with open/close/sep/kv/key", name);
                goto done;
            }
            if (t->kind == SK_BIND_ITEMS && !t->cfield[0] &&
                (ss->io_ < 0 || ss->ic_ < 0 || ss->is_ < 0)) {
                snprintf(err, err_sz, "@grammar(schema) %s: items requires an "
                         "`items:` directive with open/close/sep (or a count field)", name);
                goto done;
            }
            if ((t->kind == SK_BIND_BYTES || (t->kind == SK_BIND_ITEMS && t->cfield[0])) &&
                rs_find_int_field(ss, ti, t->cfield) < 0) {
                snprintf(err, err_sz, "@grammar(schema) %s: '%s' must name an earlier "
                         "`int` field of this schema", name, t->cfield);
                goto done;
            }
        }
        if (ss->rfkey >= 0 && !rs_rule_has_keep(g, ss->rfkey)) {
            snprintf(err, err_sz, "@grammar(schema) %s: key rule '%s.%s' has no keep",
                     name, ss->usename, ss->fkey);
            goto done;
        }
        if (ss->uterm >= 0) {
            if (ss->nbody != 1 || ss->body[0] != ss->uterm) {
                snprintf(err, err_sz, "@grammar(schema) %s: `one of` must be the "
                         "entire schema body", name);
                goto done;
            }
            /* dispatch: each variant's leading literal's first byte; at most
             * one non-literal DEFAULT arm; bytes must be distinct */
            {
                int ndef = 0;
                for (int i = 0; i < ss->nuv; i++) {
                    const STerm* ft = &ss->terms[ss->uv[i].t[0]];
                    ss->uv[i].db = (ft->kind == SK_LIT && ft->litlen >= 1)
                                       ? (int)ft->lit[0] : -1;
                    if (ss->uv[i].db < 0 && ++ndef > 1) {
                        snprintf(err, err_sz, "@grammar(schema) %s: at most one "
                                 "variant may lack a leading literal (the default arm)",
                                 name);
                        goto done;
                    }
                    for (int j = 0; j < i; j++)
                        if (ss->uv[j].db >= 0 && ss->uv[j].db == ss->uv[i].db) {
                            snprintf(err, err_sz, "@grammar(schema) %s: variants '%s' "
                                     "and '%s' dispatch on the same first byte",
                                     name, ss->uv[j].name, ss->uv[i].name);
                            goto done;
                        }
                }
            }
            /* recursion: self items must use the arena form */
            for (int ti = 0; ti < ss->nterms; ti++) {
                const STerm* t = &ss->terms[ti];
                if (t->kind == SK_BIND_ITEMS && strcmp(t->etype, name) == 0 && t->cap > 0) {
                    snprintf(err, err_sz, "@grammar(schema) %s: a recursive variant "
                             "cannot use cap (an inline array of yourself has "
                             "infinite size) — use the arena items form", name);
                    goto done;
                }
            }
        }
    }

    cc__sname = name;
    cc__fpfx[0] = '\0';
    {
        EB e = { &out, &len, &cap, F, K, 0, -1, 0, 0, -1, -1 };
        int lbl = 0;
        unsigned char local_xdone[R_MAX_RULES];
        unsigned char* xdone = reg ? reg->x_done : local_xdone;
        memset(local_xdone, 0, sizeof local_xdone);
        eb_fmt(&e, "/* @grammar(schema) %s (line %d): %s%s. API:\n"
                   " *   typedef struct %s (+ %sReader cursor)\n"
                   " *   cc_parse(%s, s, n, arena, &out)   / %s.parse(...)  -> %s_parse\n"
                   " *   cc_read(%s, s, n, &pos, arena, &out) / %s.read(...) -> %s_read\n"
                   " *   cc_reader(%s, s, n, arena) / %s.reader(...)        -> %s_reader\n"
                   " *   cc_next / cc_at_end / r.next(&out) / r.at_end()    -> %sReader_next/_at_end\n"
                   " *   cc_get(T, &v, \"field\", &out) / v.get(...)          -> T_get + T__fields table\n"
                   "%s */\n",
               name, line, reg ? "use " : "inline rules", reg ? ss->usename : "",
               name, name, name, name, name, name, name, name, name, name, name, name,
               rs_formatable(ss, g)
                   ? " *   cc_write(T, &v, dst, cap) / T.write(...)           -> T_write (format: derived lengths)\n"
                   : "");
        if (!reg || !reg->matchers_done) {
            /* private grammars emit only what this schema can reach; rules
             * the schema CALLS by name (pads, else, bare terms) are roots
             * even when small, extract bodies contribute their inner refs */
            unsigned char mark[R_MAX_RULES];
            memset(mark, 0, sizeof mark);
            if (ss->rfpad >= 0) rw_mark_rule(g, ss->rfpad, mark);
            if (ss->rfelse >= 0) rw_mark_rule(g, ss->rfelse, mark);
            if (ss->ripad >= 0) rw_mark_rule(g, ss->ripad, mark);
            if (ss->rfkey >= 0) rw_mark_node(g, g->rules[ss->rfkey].node, mark);
            for (int ti = 0; ti < ss->nterms; ti++) {
                const STerm* t = &ss->terms[ti];
                if (t->kind == SK_RULE) rw_mark_rule(g, t->rule, mark);
                else if (t->kind == SK_BIND_SLICE || t->kind == SK_BIND_INT || t->kind == SK_BIND_FLOAT)
                    rw_mark_node(g, g->rules[t->rule].node, mark);
                else if (t->kind == SK_NARROW_MEMBERS) {
                    const RNarrow* w = &t->nw;
                    rw_mark_rule(g, w->val_rule, mark);
                    rw_mark_node(g, g->rules[w->key_rule].node, mark);
                    for (int i = 0; i < w->nlpad; i++) rw_mark_rule(g, w->lpad[i], mark);
                    for (int i = 0; i < w->ntpad; i++) rw_mark_rule(g, w->tpad[i], mark);
                    for (int i = 0; i < w->nmpad_a; i++) rw_mark_rule(g, w->mpad_a[i], mark);
                    for (int i = 0; i < w->nmpad_b; i++) rw_mark_rule(g, w->mpad_b[i], mark);
                    for (int i = 0; i < w->nvpad_a; i++) rw_mark_rule(g, w->vpad_a[i], mark);
                    for (int i = 0; i < w->nvpad_b; i++) rw_mark_rule(g, w->vpad_b[i], mark);
                } else if (t->kind == SK_NARROW_LIST) {
                    const RNarrow* w = &t->nw;
                    for (int i = 0; i < w->nlpad; i++) rw_mark_rule(g, w->lpad[i], mark);
                    for (int i = 0; i < w->ntpad; i++) rw_mark_rule(g, w->tpad[i], mark);
                    for (int i = 0; i < w->nvpad_a; i++) rw_mark_rule(g, w->vpad_a[i], mark);
                    for (int i = 0; i < w->nvpad_b; i++) rw_mark_rule(g, w->vpad_b[i], mark);
                }
            }
            rs_emit_matchers(g, &e, &lbl, reg ? NULL : mark);
            if (reg) reg->matchers_done = 1;
        }
        /* extract variants needed by this schema (shared across schemas) */
        {
            for (int ti = -1; ti < ss->nterms; ti++) {
                int r = -1, as_key = 0;
                if (ti < 0) { r = ss->rfkey; as_key = 1; }
                else if (ss->terms[ti].kind == SK_BIND_SLICE || ss->terms[ti].kind == SK_BIND_INT ||
                         ss->terms[ti].kind == SK_BIND_FLOAT)
                    r = ss->terms[ti].rule;
                else if (ss->terms[ti].kind == SK_NARROW_MEMBERS) {
                    r = ss->terms[ti].nw.key_rule; as_key = 1;
                }
                if (r < 0 || xdone[r]) continue;
                if (!as_key) {   /* bind sites inline small top-level-keep rules — no call, no fn */
                    int body = g->rules[r].node;
                    if (g->nodes[body].kind == RN_KEEP &&
                        rg_inline_size(g, g->nodes[body].a, 0) <= 16) continue;
                }
                rs_emit_x(g, &e, &lbl, r);
                xdone[r] = 1;
            }
        }
        /* the struct: fields in declaration order, at their event sites */
        {
            int order[S_MAX_TERMS]; int cnt = 0;
            for (int i = 0; i < ss->nbody; i++) rs_collect_binds(ss, ss->body[i], order, &cnt);
            for (int i = 0; i < ss->nterms; i++) ss->bindbit[i] = -1;
            ss->nbinds = cnt;
            /* presence tracking only where absence is possible: a dispatching
             * schema (fields/narrowed members) may leave fields unassigned; a
             * product schema assigns every field on any successful parse */
            {
                int conditional = 0;
                for (int ti = 0; ti < ss->nterms && !conditional; ti++)
                    if (ss->terms[ti].kind == SK_FIELDS ||
                        ss->terms[ti].kind == SK_NARROW_MEMBERS) conditional = 1;
                ss->presence = conditional && cnt > 0 && cnt <= 64;
            }
            for (int i = 0; i < cnt; i++) ss->bindbit[order[i]] = i;
            if (ss->uterm >= 0) {
                eb_fmt(&e, "typedef enum { ");
                for (int vi = 0; vi < ss->nuv; vi++)
                    eb_fmt(&e, "%s%s_%s", vi ? ", " : "", name, ss->uv[vi].name);
                eb_fmt(&e, " } %sKind;\n", name);
            }
            {
                int need_ffc = 0;
                for (int i = 0; i < cnt && !need_ffc; i++)
                    if (ss->terms[order[i]].kind == SK_BIND_FLOAT) need_ffc = 1;
                if (need_ffc) eb_fmt(&e, "#include <ccc/vendor/ffc.h>\n");
            }
            eb_fmt(&e, "typedef struct %s {\n", name);
            for (int i = 0; i < cnt; i++) {
                const STerm* t = &ss->terms[order[i]];
                if (t->kind == SK_BIND_INT)
                    eb_fmt(&e, "    long long %s;\n", t->field);
                else if (t->kind == SK_BIND_FLOAT)
                    eb_fmt(&e, "    double %s;\n", t->field);
                else if (t->kind == SK_BIND_SLICE || t->kind == SK_BIND_BYTES)
                    eb_fmt(&e, "    CCSlice %s;\n", t->field);
                else if (t->cap > 0)
                    eb_fmt(&e, "    %s %s[%d]; size_t %s_n;\n", t->etype, t->field, t->cap, t->field);
                else
                    eb_fmt(&e, "    %s* %s; size_t %s_n;\n", t->etype, t->field, t->field);
            }
            if (ss->presence)
                eb_fmt(&e, "    unsigned long long cc__set;   /* presence bitmap, bit i = field i */\n");
            if (ss->uterm >= 0) {
                eb_fmt(&e, "    %sKind kind;\n    union {\n", name);
                for (int vi = 0; vi < ss->nuv; vi++) {
                    eb_fmt(&e, "        struct {\n");
                    for (int j = 0; j < ss->uv[vi].nt; j++) {
                        const STerm* t = &ss->terms[ss->uv[vi].t[j]];
                        if (t->kind == SK_BIND_INT)
                            eb_fmt(&e, "            long long %s;\n", t->field);
                        else if (t->kind == SK_BIND_FLOAT)
                            eb_fmt(&e, "            double %s;\n", t->field);
                        else if (t->kind == SK_BIND_SLICE || t->kind == SK_BIND_BYTES)
                            eb_fmt(&e, "            CCSlice %s;\n", t->field);
                        else if (t->kind == SK_BIND_ITEMS && t->cap > 0)
                            eb_fmt(&e, "            %s %s[%d]; size_t %s_n;\n",
                                   t->etype, t->field, t->cap, t->field);
                        else if (t->kind == SK_BIND_ITEMS)
                            eb_fmt(&e, "            struct %s* %s; size_t %s_n;\n",
                                   t->etype, t->field, t->field);
                    }
                    eb_fmt(&e, "        } %s;\n", ss->uv[vi].name);
                }
                eb_fmt(&e, "    } u;\n");
            }
            if (cnt == 0 && ss->uterm < 0) eb_fmt(&e, "    char cc__empty;\n");
            eb_fmt(&e, "} %s;\n", name);
        }
        eb_fmt(&e, "static int %s__fill(const unsigned char* s, size_t n, size_t* io,\n"
                   "        CCArena* arena, %s* out, int* cc_inc, int cc_depth) {\n"
                   "    (void)cc_inc; (void)cc_depth;\n"
                   "    size_t p = *io;\n    (void)arena;\n", name, name);
        /* zero the struct only when some bind is conditional (inside a
         * fields dispatch): a body whose binds are all unconditional terms
         * assigns every member on the success path — missing-member zeroing
         * is a fields-combinator semantic, not a product-schema one. */
        {
            int conditional = 0;
            for (int ti = 0; ti < ss->nterms && !conditional; ti++)
                if (ss->terms[ti].kind == SK_FIELDS ||
                    ss->terms[ti].kind == SK_NARROW_MEMBERS) conditional = 1;
            if (conditional)
                eb_fmt(&e, "    memset(out, 0, sizeof *out);\n");
        }
        {
            char fail[16];
            snprintf(fail, sizeof(fail), "Lz%d", lbl++);
            for (int i = 0; i < ss->nbody; i++)
                rs_emit_term(ss, g, &e, &lbl, ss->body[i], fail);
            eb_fmt(&e, "    *io = p;\n    return 1;\n%s:\n    return 0;\n}\n", fail);
        }
        eb_fmt(&e, "static int %s_parse(const char* s0, size_t n, CCArena* arena, %s* out) {\n"
                   "    const unsigned char* s = (const unsigned char*)s0;\n"
                   "    size_t p = 0;\n"
                   "    { int cc_i0 = 0;\n"
                   "    if (!%s__fill(s, n, &p, arena, out, &cc_i0, 0)) return 0; }\n"
                   "    return p == n;\n}\n", name, name, name);
        /* incremental entry: parse ONE value at *pos, advance it — pipelines
         * and framed streams call this in a loop instead of requiring p == n */
        /* the streaming face, in the house Result idiom — the SAME contract
         * as channel recv:
         *   Ok(true)                 frame parsed; pos advanced
         *   Ok(false)                clean end-of-input AT a frame boundary
         *                            (pos == n): graceful close
         *   Err(CC_ERR_WOULD_BLOCK)  partial frame — need more bytes; pos
         *                            unmoved; refill and retry. Source
         *                            closed too? caller escalates.
         *   Err(CC_ERR_PARSE)        malformed, evidence in hand
         * Bounds-vs-content is decided exactly at product terms (literals,
         * fused ints, bytes, counted items); matcher-tier failures report
         * Err(PARSE) conservatively. */
        eb_fmt(&e, "static bool !>(CCError) %s_try_read(const char* s0, size_t n,\n"
                   "        size_t* pos, CCArena* arena, %s* out) {\n"
                   "    size_t p = *pos;\n"
                   "    int cc_i0 = 0;\n"
                   "    if (p >= n) return cc_ok(false);   /* clean end at frame boundary */\n"
                   "    if (%s__fill((const unsigned char*)s0, n, &p, arena, out, &cc_i0, 0)) {\n"
                   "        *pos = p;\n"
                   "        return cc_ok(true);\n"
                   "    }\n"
                   "    if (cc_i0) return cc_err(CC_ERROR(CC_ERR_WOULD_BLOCK, \"%s: incomplete frame\"));\n"
                   "    return cc_err(CC_ERROR(CC_ERR_PARSE, \"%s: malformed input\"));\n"
                   "}\n",
               name, name, name, name, name);
        eb_fmt(&e, "static int %s_read(const char* s0, size_t n, size_t* pos, CCArena* arena, %s* out) {\n"
                   "    int cc_i0 = 0;\n"
                   "    return %s__fill((const unsigned char*)s0, n, pos, arena, out, &cc_i0, 0);\n}\n",
               name, name, name);
        /* the runtime instance: a Reader is a CURSOR over resources (buffer,
         * position, arena) — all behavior was specialized at compile time.
         * _next returns 0 at clean end-of-input AND on parse error; the two
         * are distinguished by _at_end (error leaves the cursor mid-input). */
        eb_fmt(&e, "typedef struct %sReader {\n"
                   "    const char* s; size_t n; size_t pos; CCArena* arena;\n"
                   "} %sReader;\n", name, name);
        eb_fmt(&e, "static %sReader %s_reader(const char* s, size_t n, CCArena* arena) {\n"
                   "    %sReader r; r.s = s; r.n = n; r.pos = 0; r.arena = arena; return r;\n}\n",
               name, name, name);
        /* methods are named after the RECEIVER type so instance UFCS
         * (`r.next(&out)`, `r.at_end()`) resolves by convention */
        eb_fmt(&e, "static int %sReader_next(%sReader* r, %s* out) {\n"
                   "    if (r->pos >= r->n) return 0;\n"
                   "    int cc_i0 = 0;\n"
                   "    return %s__fill((const unsigned char*)r->s, r->n, &r->pos, r->arena, out, &cc_i0, 0);\n}\n",
               name, name, name, name);
        eb_fmt(&e, "static int %sReader_at_end(const %sReader* r) { return r->pos == r->n; }\n",
               name, name);
        /* instance UFCS (`r.next(&out)`, `r.at_end()`, `v.to_str(&a)`): the
         * engine registers the schema and Reader types natively — users
         * never write a registration */
        cc__grammar_note_ufcs_type(name);
        {
            char rn[S_NAME + 8];
            snprintf(rn, sizeof rn, "%sReader", name);
            cc__grammar_note_ufcs_type(rn);
        }
        /* reflection face (field table + compiled get), on demand */
        {
            int want_get = rs_has_token(src, src_len, name, "_get") ||
                           rs_has_token(src, src_len, name, "_field") ||
                           rs_has_token(src, src_len, name, "__fields") ||
                           rs_has_op(src, src_len, "cc_get", name) ||
                           rs_has_op(src, src_len, "cc_field", name) ||
                           rs_has_dot(src, src_len, name, "get") ||
                           rs_any_method_demand(src, src_len, "get");
            if (want_get) rs_emit_get(ss, &e, name);
        }
        /* the WRITE projection (schema inverted), on demand like every tier */
        {
            int fmtable = rs_formatable(ss, g);
            int want_write = rs_has_token(src, src_len, name, "_write") ||
                             rs_has_op(src, src_len, "cc_write", name) ||
                             rs_has_dot(src, src_len, name, "write");
            int want_tostr = rs_has_token(src, src_len, name, "_to_str") ||
                             rs_has_op(src, src_len, "cc_format", name) ||
                             rs_has_dot(src, src_len, name, "to_str") ||
                             (fmtable && rs_any_method_demand(src, src_len, "to_str"));
            if (!want_write && fmtable &&
                (want_tostr || rs_any_method_demand(src, src_len, "write")))
                want_write = 1;   /* element writers / to_str substrate */
            if ((want_write || want_tostr) && !fmtable) {
                snprintf(err, err_sz, "@grammar(schema) %s: %s_write/_to_str is referenced "
                         "but the schema is not formatable yet (directive `fields [...]` has "
                         "no grammar rule to invert — narrow a rule instead: `G.rule [...]`)",
                         name, name);
                free(out);
                out = NULL;
                goto done;
            }
            if (want_write)
                rs_emit_write(ss, g, &e, &lbl, name);
            if (want_tostr) {
                /* CCString face: composes with @string templates (a CCString
                 * slot arm exists in the _Generic) and the language's
                 * `x.to_str(arena)` idiom. Measure once, allocate exactly,
                 * write unchecked — no retry loop, no transient waste. */
                eb_fmt(&e, "static __attribute__((unused)) CCString %s_to_str(const %s* v, CCArena* arena) {\n"
                           "    CCString s = cc_string_new();\n"
                           "    size_t need = %s__wmeasure(v);\n"
                           "    char* buf = (char*)cc_arena_alloc_local(arena, need ? need : 1, 1);\n"
                           "    if (!buf) return s;\n"
                           "    %s__wput(v, buf);\n"
                           "    cc_string_push_buffer(&s, buf, (uint32_t)need, arena);\n"
                           "    return s;\n"
                           "}\n", name, name, name, name);
            }
        }
        cc_sb_append(e.buf, e.len, e.cap, "", 1);
        if (out) out[len - 1] = '\0';
    }
    if (cc__schema_nreg < (int)(sizeof(cc__schema_reg) / sizeof(cc__schema_reg[0])))
        snprintf(cc__schema_reg[cc__schema_nreg++], S_NAME, "%s", name);

done:
    free(ss); free(g); free(F); free(K);
    return out;
}

/* bounded whole-token search: does `<name><suffix>` appear in src as a full
 * identifier? Over-approximates (comments/strings count) — the safe side. */
static int rs_has_token(const char* src, size_t n, const char* name, const char* suffix) {
    char tok[160];
    int tl = snprintf(tok, sizeof(tok), "%s%s", name, suffix);
    if (tl <= 0 || (size_t)tl >= sizeof(tok) || (size_t)tl > n) return 0;
    for (size_t i = 0; i + (size_t)tl <= n; i++) {
        if (src[i] != tok[0] || memcmp(src + i, tok, (size_t)tl) != 0) continue;
        int okl = (i == 0) || !(isalnum((unsigned char)src[i - 1]) || src[i - 1] == '_');
        int okr = (i + (size_t)tl == n) ||
                  !(isalnum((unsigned char)src[i + (size_t)tl]) || src[i + (size_t)tl] == '_');
        if (okl && okr) return 1;
    }
    return 0;
}

/* op-form reference: `cc_match ( Name` etc — the <ccc/cc_grammar.cch> surface */
static int rs_has_op(const char* src, size_t n, const char* op, const char* name) {
    size_t ol = strlen(op), nl = strlen(name);
    for (size_t i = 0; i + ol < n; i++) {
        if (src[i] != op[0] || memcmp(src + i, op, ol) != 0) continue;
        if (i > 0 && (isalnum((unsigned char)src[i - 1]) || src[i - 1] == '_')) continue;
        size_t j = i + ol;
        while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
        if (j >= n || src[j] != '(') continue;
        j++;
        while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
        if (j + nl > n || memcmp(src + j, name, nl) != 0) continue;
        if (j + nl < n && (isalnum((unsigned char)src[j + nl]) || src[j + nl] == '_')) continue;
        return 1;
    }
    return 0;
}

/* UFCS type-scoped reference: `Name . method (` */
static int rs_has_dot(const char* src, size_t n, const char* name, const char* method) {
    size_t nl = strlen(name), ml = strlen(method);
    for (size_t i = 0; i + nl < n; i++) {
        if (src[i] != name[0] || memcmp(src + i, name, nl) != 0) continue;
        if (i > 0 && (isalnum((unsigned char)src[i - 1]) || src[i - 1] == '_')) continue;
        size_t j = i + nl;
        while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
        if (j >= n || src[j] != '.') continue;
        j++;
        while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
        if (j + ml > n || memcmp(src + j, method, ml) != 0) continue;
        size_t k = j + ml;
        while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
        if (k < n && src[k] == '(') return 1;
    }
    return 0;
}

static char* cc__rules_emit(const char* name, const char* body, size_t body_len,
                            const char* file, int line,
                            const char* src, size_t src_len,
                            char* err, size_t err_sz) {
    RG* g = (RG*)calloc(1, sizeof(RG));
    if (!g) { snprintf(err, err_sz, "@grammar(rules): out of memory"); return NULL; }
    g->body = body; g->n = body_len; g->file = file; g->line0 = line; g->name = name;

    char* out = NULL;
    if (rg_parse(g) == 0) {
        /* demand analysis: the block is a FACTORY — a tier is stamped out
         * only if this file references its entry points. Unused projections
         * cost nothing, not even compile time. (Schemas that `use` this
         * grammar emit their own shared copy under <Name>__s — independent.) */
        int want_match = rs_has_token(src, src_len, name, "_match") ||
                         rs_has_op(src, src_len, "cc_match", name) ||
                         rs_has_dot(src, src_len, name, "match");
        int want_build = rs_has_token(src, src_len, name, "_parse") ||
                         rs_has_token(src, src_len, name, "_collect") ||
                         rs_has_token(src, src_len, name, "Node") ||
                         rs_has_op(src, src_len, "cc_parse", name) ||
                         rs_has_op(src, src_len, "cc_collect", name) ||
                         rs_has_dot(src, src_len, name, "parse") ||
                         rs_has_dot(src, src_len, name, "collect");
        int want_dom = rs_has_token(src, src_len, name, "_dom") ||
                       rs_has_op(src, src_len, "cc_dom", name) ||
                       rs_has_dot(src, src_len, name, "dom");
        if (want_dom) want_build = 1;   /* the tape is the dom's substrate */
        out = rg_emit(g, line, want_match, want_build, want_dom);
        if (out) cc__register_rules(name, body, body_len);   /* schemas may `use` it */
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
                              const char* src, size_t src_len,
                              char* err, size_t err_sz) {
    if (err && err_sz) err[0] = '\0';
    if (engine && strcmp(engine, "rules") == 0)
        return cc__rules_emit(name, body, body_len, file, line, src, src_len, err, err_sz);
    if (engine && strcmp(engine, "schema") == 0)
        return cc__schema_emit(name, body, body_len, file, line, src, src_len, err, err_sz);
    /* not a builtin: signal fall-through to the comptime-fn path */
    return NULL;
}
