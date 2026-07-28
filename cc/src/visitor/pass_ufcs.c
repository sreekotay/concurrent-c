/* Extracted from the working implementation in `cc/src/visitor/visitor.c`.
   Goal: keep semantics identical while shrinking visitor.c over time. */

#include "pass_ufcs.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "preprocess/preprocess.h"
#include "preprocess/type_registry.h"
#include "util/path.h"
#include "util/text.h"
#include "util/text_scan.h"
#include "visitor/pass_common.h"
#include "visitor/ufcs.h"

struct CC__UFCSSpan {
    size_t start; /* inclusive */
    size_t end;   /* exclusive */
};

static size_t cc__scan_receiver_start_left(const char* s, size_t range_start, size_t sep_pos);
static int cc__is_ident_start_char(char c);
static int cc__is_ident_char_char(char c);
static size_t cc__scan_member_chain_start_left(const char* s, size_t range_start, size_t sep_pos);
static int cc__find_ufcs_span_in_range(const char* s,
                                       size_t range_start,
                                       size_t range_end,
                                       const char* method,
                                       int occurrence_1based,
                                       struct CC__UFCSSpan* out_span);
static size_t cc__ufcs_extend_chain_end(const char* s, size_t len, size_t end);
static void cc__ufcs_extract_receiver_expr(const char* expr, char* out, size_t out_cap);

static void cc__trim_span(const char** start, const char** end) {
    if (!start || !end || !*start || !*end) return;
    while (*start < *end && isspace((unsigned char)**start)) (*start)++;
    while (*end > *start && isspace((unsigned char)*((*end) - 1))) (*end)--;
}

static void cc__ufcs_extract_receiver_expr(const char* expr, char* out, size_t out_cap) {
    const char* start = expr;
    const char* end = expr ? expr + strlen(expr) : NULL;
    int par = 0, br = 0, brc = 0;
    if (!out || out_cap == 0) return;
    out[0] = '\0';
    if (!expr) return;
    for (const char* p = expr; *p; p++) {
        char c = *p;
        if (c == '"' || c == '\'') {
            char q = c;
            p++;
            while (*p) {
                if (*p == '\\' && p[1]) { p++; continue; }
                if (*p == q) break;
                p++;
            }
            continue;
        }
        if (c == '(') { par++; continue; }
        if (c == ')' && par > 0) { par--; continue; }
        if (c == '[') { br++; continue; }
        if (c == ']' && br > 0) { br--; continue; }
        if (c == '{') { brc++; continue; }
        if (c == '}' && brc > 0) { brc--; continue; }
        if (par || br || brc) continue;
        if (c == '.' || (c == '-' && p[1] == '>')) {
            const char* sep_end = p + ((c == '.') ? 1 : 2);
            const char* method = sep_end;
            while (*method && isspace((unsigned char)*method)) method++;
            if (!cc__is_ident_start_char(*method)) continue;
            while (cc__is_ident_char_char(*method)) method++;
            while (*method && isspace((unsigned char)*method)) method++;
            if (*method != '(') continue;
            end = p;
            break;
        }
    }
    cc__trim_span(&start, &end);
    if (end <= start) return;
    {
        size_t n = (size_t)(end - start);
        if (n >= out_cap) n = out_cap - 1;
        memcpy(out, start, n);
        out[n] = '\0';
    }
}


static int cc__is_ident_start_char(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static int cc__is_ident_char_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static size_t cc__scan_member_chain_start_left(const char* s, size_t range_start, size_t sep_pos) {
    size_t r_end = sep_pos;
    size_t seg_start;
    size_t q;
    if (!s || sep_pos <= range_start) return sep_pos;
    while (r_end > range_start && isspace((unsigned char)s[r_end - 1])) r_end--;
    if (r_end == range_start) return sep_pos;

    /* Initial segment may be CallExpr / paren primary: `foo(args).m` or `(e).m`.
     * Without this, `make_bool(true).is_err()` fails the ident-only gate below
     * and the span collapses (rewrite no-ops; host C sees `.is_err`). */
    if (r_end > range_start && s[r_end - 1] == ')') {
        int depth = 1;
        size_t pp = r_end - 1;
        while (pp > range_start && depth > 0) {
            pp--;
            if (s[pp] == ')') depth++;
            else if (s[pp] == '(') depth--;
        }
        if (depth != 0) return sep_pos;
        seg_start = pp;
        q = pp;
        while (q > range_start && isspace((unsigned char)s[q - 1])) q--;
        if (q > range_start && cc__is_ident_char_char(s[q - 1])) {
            seg_start = q;
            while (seg_start > range_start &&
                   cc__is_ident_char_char(s[seg_start - 1])) seg_start--;
            if (!cc__is_ident_start_char(s[seg_start])) return sep_pos;
        }
    } else {
        seg_start = r_end;
        while (seg_start > range_start && cc__is_ident_char_char(s[seg_start - 1])) seg_start--;
        if (seg_start == r_end || !cc__is_ident_start_char(s[seg_start])) return sep_pos;
    }

    for (;;) {
        q = seg_start;
        while (q > range_start && isspace((unsigned char)s[q - 1])) q--;
        if (q > range_start && s[q - 1] == '.') {
            q--;
        } else if (q > range_start + 1 && s[q - 2] == '-' && s[q - 1] == '>') {
            q -= 2;
        } else {
            break;
        }
        while (q > range_start && isspace((unsigned char)s[q - 1])) q--;
        /* Left operand of `.`/`->` is a postfix expr.  Consume trailing
         * subscripts (`shards[i]`, `a[i][j]`), then at most one `(...)`
         * (call or paren primary), then the base ident when present.
         * Do not keep chewing `(...)` groups — that would swallow C casts
         * like `(int64_t)(p)->field.method()`.  Stopping at `[` would
         * truncate `db->shards[i].field.method()` to `[i].field.method()`
         * and mis-emit it as a closure capture. */
        while (q > range_start && s[q - 1] == ']') {
            int depth = 1;
            size_t pp = q - 1;
            while (pp > range_start && depth > 0) {
                pp--;
                if (s[pp] == ']') depth++;
                else if (s[pp] == '[') depth--;
            }
            if (depth != 0) return sep_pos;
            seg_start = pp;
            q = pp;
            while (q > range_start && isspace((unsigned char)s[q - 1])) q--;
        }
        if (q > range_start && s[q - 1] == ')') {
            int depth = 1;
            size_t pp = q - 1;
            while (pp > range_start && depth > 0) {
                pp--;
                if (s[pp] == ')') depth++;
                else if (s[pp] == '(') depth--;
            }
            if (depth != 0) return sep_pos;
            seg_start = pp;
            q = pp;
            while (q > range_start && isspace((unsigned char)s[q - 1])) q--;
        }
        if (q > range_start && cc__is_ident_char_char(s[q - 1])) {
            seg_start = q;
            while (seg_start > range_start && cc__is_ident_char_char(s[seg_start - 1])) seg_start--;
            if (!cc__is_ident_start_char(s[seg_start])) return sep_pos;
            continue;
        }
        /* Parenthesized primary with no leading ident, e.g. `(e).m`. */
        break;
    }

    return seg_start;
}


static size_t cc__scan_receiver_start_left(const char* s, size_t range_start, size_t sep_pos) {
    if (!s || sep_pos <= range_start) return range_start;
    {
        size_t chain_start = cc__scan_member_chain_start_left(s, range_start, sep_pos);
        if (chain_start < sep_pos) return chain_start;
    }
    size_t r_end = sep_pos;
    while (r_end > range_start && isspace((unsigned char)s[r_end - 1])) r_end--;
    if (r_end == range_start) return range_start;

    int par = 0, br = 0, brc = 0;
    size_t r = r_end;
    while (r > range_start) {
        char c = s[r - 1];
        if (c == ')') { par++; r--; continue; }
        if (c == ']') { br++; r--; continue; }
        if (c == '}') { brc++; r--; continue; }
        if (c == '(' && par > 0) { par--; r--; continue; }
        if (c == '[' && br > 0) { br--; r--; continue; }
        if (c == '{' && brc > 0) { brc--; r--; continue; }
        if (par || br || brc) { r--; continue; }
        /* `->` is member-access — consume both chars so the receiver chain
           extends past it (see cc__find_ufcs_span_in_range for details). */
        if (c == '>' && r >= range_start + 2 && s[r - 2] == '-') {
            r -= 2;
            continue;
        }
        if (c == ',' || c == ';' || c == '=' || c == '\n' ||
            c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
            c == '&' || c == '|' || c == '^' || c == '!' || c == '~' ||
            c == '<' || c == '>' || c == '?' || c == ':' ) {
            break;
        }
        r--;
    }
    while (r < r_end && isspace((unsigned char)s[r])) r++;
    return r;
}

static int cc__find_ufcs_span_in_range(const char* s,
                                       size_t range_start,
                                       size_t range_end,
                                       const char* method,
                                       int occurrence_1based,
                                       struct CC__UFCSSpan* out_span) {
    if (!s || !method || !out_span) return 0;
    const size_t method_len = strlen(method);
    if (method_len == 0) return 0;
    if (occurrence_1based <= 0) occurrence_1based = 1;
    int seen = 0;

    /* Find ".method" or "->method" followed by optional whitespace then '(' */
    for (size_t i = range_start; i + method_len + 2 < range_end; i++) {
        int is_arrow = 0;
        size_t sep_pos = 0;
        if (s[i] == '.' ) { is_arrow = 0; sep_pos = i; }
        else if (s[i] == '-' && i + 1 < range_end && s[i + 1] == '>') { is_arrow = 1; sep_pos = i; }
        else continue;

        size_t mpos = sep_pos + (is_arrow ? 2 : 1);
        while (mpos < range_end && isspace((unsigned char)s[mpos])) mpos++;
        if (mpos + method_len >= range_end) continue;
        if (memcmp(s + mpos, method, method_len) != 0) continue;

        size_t after = mpos + method_len;
        while (after < range_end && isspace((unsigned char)s[after])) after++;
        if (after >= range_end || s[after] != '(') continue;

        /* Match Nth occurrence. */
        seen++;
        if (seen != occurrence_1based) continue;

        /* Receiver: allow non-trivial expressions like (foo()).bar, arr[i].m, (*p).m.
           Find the start by scanning left with bracket balancing until a delimiter. */
        size_t r_end = sep_pos;
        while (r_end > range_start && isspace((unsigned char)s[r_end - 1])) r_end--;
        if (r_end == range_start) continue;

        int par = 0, br = 0, brc = 0;
        size_t r = r_end;
        while (r > range_start) {
            char c = s[r - 1];
            if (c == ')') { par++; r--; continue; }
            if (c == ']') { br++; r--; continue; }
            if (c == '}') { brc++; r--; continue; }
            if (c == '(' && par > 0) { par--; r--; continue; }
            if (c == '[' && br > 0) { br--; r--; continue; }
            if (c == '{' && brc > 0) { brc--; r--; continue; }
            if (par || br || brc) { r--; continue; }

            /* `->` is member-access, not a delimiter: it binds the identifier
               on its left into the receiver chain (e.g. `obj->field.method()`).
               Consume both chars and continue scanning left.  Without this,
               `-`/`>` would break the scan and receiver would be truncated to
               just the trailing segment. */
            if (c == '>' && r >= range_start + 2 && s[r - 2] == '-') {
                r -= 2;
                continue;
            }

            /* At top-level: stop on likely expression delimiters. */
            if (c == ',' || c == ';' || c == '=' || c == '\n' ||
                c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
                c == '&' || c == '|' || c == '^' || c == '!' || c == '~' ||
                c == '<' || c == '>' || c == '?' || c == ':' ) {
                break;
            }
            /* Otherwise keep consuming (identifiers, dots, brackets, parens, spaces). */
            r--;
        }
        /* Trim any leading whitespace included in the backward scan. */
        while (r < r_end && isspace((unsigned char)s[r])) r++;
        if (r >= r_end) continue;

        /* Find matching ')' for the call, skipping strings/chars. */
        size_t p = after;
        int depth = 0;
        while (p < range_end) {
            char c = s[p++];
            if (c == '(') depth++;
            else if (c == ')') {
                depth--;
                if (depth == 0) {
                    out_span->start = r;
                    out_span->end = p;
                    return 1;
                }
            } else if (c == '"' || c == '\'') {
                char q = c;
                while (p < range_end) {
                    char d = s[p++];
                    if (d == '\\' && p < range_end) { p++; continue; }
                    if (d == q) break;
                }
            }
        }
        return 0;
    }
    return 0;
}

static size_t cc__ufcs_extend_chain_end(const char* s, size_t len, size_t end) {
    if (!s || end >= len) return end;
    size_t p = end;
    for (;;) {
        while (p < len && isspace((unsigned char)s[p])) p++;
        if (p >= len) break;
        if (s[p] == '.') {
            p++;
        } else if (p + 1 < len && s[p] == '-' && s[p + 1] == '>') {
            p += 2;
        } else {
            break;
        }

        while (p < len && isspace((unsigned char)s[p])) p++;
        if (p >= len || (!isalpha((unsigned char)s[p]) && s[p] != '_')) break;
        while (p < len && (isalnum((unsigned char)s[p]) || s[p] == '_')) p++;
        while (p < len && isspace((unsigned char)s[p])) p++;
        if (p >= len || s[p] != '(') break;

        /* Scan to matching ')' of this call. */
        int depth = 0;
        while (p < len) {
            char c = s[p++];
            if (c == '(') depth++;
            else if (c == ')') {
                depth--;
                if (depth == 0) break;
            } else if (c == '"' || c == '\'') {
                char q = c;
                while (p < len) {
                    char d = s[p++];
                    if (d == '\\' && p < len) { p++; continue; }
                    if (d == q) break;
                }
            }
        }
        end = p;
    }
    return end;
}

/* Path helpers are now in pass_common.h */

/* Per-span edit collector.  Walks UFCS-flagged CALL nodes in TCC's stub AST,
 * resolves each call's source span against `eb->src`, runs
 * `cc_ufcs_rewrite_line_full()` on the extracted text, and pushes a
 * `[span.start, span.end) → lowered_text` edit per non-subsumed call.
 *
 * (A legacy wholesale rewriter that mutated the buffer in place between
 * nodes existed until the offset cleanup; it had no callers and is gone.)
 *
 * This per-span variant resolves every span against the ORIGINAL `eb->src`
 * (offsets stay consistent because the source never mutates) and uses the
 * same `done[]` containment check the wholesale path used to suppress
 * subsumed nested chain segments.  The result: each UFCS call site
 * produces exactly one edit, surgical and non-overlapping, so other
 * collectors can run in the same edit buffer without collision.
 *
 * (The CC_UFCS_TEXT_FALLBACK whole-buffer diagnostic path is deleted —
 * the AST resolver is the only path.)
 */
/* AST nodes speak LOGICAL coordinates (TCC honors #line directives), but the
 * buffer being edited may contain large spliced regions (e.g. @grammar
 * lowerings) that change physical line counts and bracket themselves with
 * #line — so one logical (file,line) can materialize at SEVERAL physical
 * offsets.  The resolver enumerates every candidate and lets byte
 * verification arbitrate (see cc__span_at_candidate); the old scheme —
 * trust the last logical match, bolt on a raw-physical retry when the span
 * finder disagreed, then accept an unverified "lax" span — is gone. */
/* Strict member verification — the resolver's arbiter.  True iff sep_pos
 * holds `.`/`->`, then ws*, the method identifier as a whole token, ws*,
 * `(`.  A candidate that cannot produce these bytes is not the node's
 * line, no matter what the coordinates claim. */
static int cc__verify_member_at(const char* s, size_t n, size_t sep_pos,
                                const char* method) {
    size_t p = sep_pos, ml;
    if (!s || !method) return 0;
    ml = strlen(method);
    if (ml == 0 || p >= n) return 0;
    if (s[p] == '.') p++;
    else if (p + 1 < n && s[p] == '-' && s[p + 1] == '>') p += 2;
    else return 0;
    while (p < n && isspace((unsigned char)s[p])) p++;
    if (p + ml > n || memcmp(s + p, method, ml) != 0) return 0;
    p += ml;
    if (p < n && cc__is_ident_char_char(s[p])) return 0;
    while (p < n && isspace((unsigned char)s[p])) p++;
    return p < n && s[p] == '(';
}

/* Probe one candidate line offset for the node's member construct and
 * build the rewrite span there.  Two probes, strictest first:
 *
 *   1. column-anchored: the recorder's separator column applied RELATIVE
 *      TO THIS CANDIDATE's line start (the old resolver anchored columns
 *      through a global physical mapping, which only agreed with the
 *      logical mapping when nothing above the node had spliced lines).
 *      The recorder can report a column past the separator — the method
 *      ident, the call's rparen, or a macro invocation's tail — so on a
 *      miss we back-scan one token and re-verify.  Either way the member
 *      bytes must verify at the resolved separator.
 *
 *   2. the same occurrence-scan over [line_start .. line_end] the old
 *      resolver used as its range fallback, bounded to this candidate.
 *
 * Returns 1 with *out set only when the construct is verifiably present. */
static int cc__span_at_candidate(const char* s, size_t n, size_t ls_off,
                                 int ls, int le, int col_start, int col_end,
                                 const char* method, int occurrence_1based,
                                 struct CC__UFCSSpan* out) {
    if (!s || !out || ls_off >= n) return 0;
    if (le < ls) le = ls;
    /* Start of line le, counted from this candidate: a single construct
     * never straddles a splice boundary, so its logical lines advance
     * with physical lines inside the candidate's region. */
    size_t le_off = ls_off;
    for (int k = ls; k < le; k++) {
        while (le_off < n && s[le_off] != '\n') le_off++;
        if (le_off >= n) return 0;
        le_off++;
    }
    size_t region_end = le_off;
    while (region_end < n && s[region_end] != '\n') region_end++;
    if (region_end < n) region_end++; /* == start of line le+1, old `re` */

    if (col_start > 0 && col_end > 0) {
        size_t sep_pos = ls_off + (size_t)(col_start - 1);
        size_t end_pos = le_off + (size_t)(col_end - 1);
        if (end_pos > region_end) end_pos = region_end;
        if (sep_pos < region_end && end_pos > sep_pos) {
            size_t sep = (size_t)-1;
            if (cc__verify_member_at(s, n, sep_pos, method)) {
                sep = sep_pos;
            } else {
                size_t p = sep_pos;
                while (p > ls_off && isspace((unsigned char)s[p - 1])) p--;
                while (p > ls_off && cc__is_ident_char_char(s[p - 1])) p--;
                while (p > ls_off && isspace((unsigned char)s[p - 1])) p--;
                if (p > ls_off && s[p - 1] == '.' &&
                    cc__verify_member_at(s, n, p - 1, method)) {
                    sep = p - 1;
                } else if (p > ls_off + 1 && s[p - 2] == '-' && s[p - 1] == '>' &&
                           cc__verify_member_at(s, n, p - 2, method)) {
                    sep = p - 2;
                }
            }
            if (sep != (size_t)-1) {
                out->start = cc__scan_receiver_start_left(s, ls_off, sep);
                out->end = end_pos;
                if (out->start < out->end) return 1;
            }
        }
    }
    return cc__find_ufcs_span_in_range(s, ls_off, region_end, method,
                                       occurrence_1based, out);
}

int cc__collect_ufcs_edits(const CCASTRoot* root,
                           const CCVisitorCtx* ctx,
                           CCEditBuffer* eb) {
    if (!root || !ctx || !ctx->input_path || !eb || !eb->src) return 0;
    if (!root->nodes || root->node_count <= 0) return 0;

    cc_ufcs_set_symbols(ctx->symbols);

    const char* in_src = eb->src;
    size_t in_len = eb->src_len;

    typedef CCNodeView NodeView;
    const NodeView* n = (const NodeView*)root->nodes;

    /* Collect UFCS-flagged CALL nodes.  Mirrors the rewriter exactly. */
    struct UFCSNode {
        int line_start;
        int line_end;
        int col_start;
        int col_end;
        const char* method;
        const char* recv_type;
        int occurrence_1based;
        int is_under_await;
        int recv_type_is_ptr;
        const char* file;
        long off_start;   /* member-separator offset in the PARSE buffer */
    };
    struct UFCSNode* nodes = NULL;
    int node_count = 0;
    int node_cap = 0;

    for (int i = 0; i < root->node_count; i++) {
        if (n[i].kind != 5) continue;          /* CALL */
        if ((n[i].aux2 & 4) == 0) continue;    /* UFCS marker */
        if (!n[i].aux_s1) continue;
        int ls = n[i].line_start;
        int le = n[i].line_end;
        if (ls <= 0) continue;
        if (le < ls) le = ls;
        /* OFFSET SELF-CHECK (fail loudly): a UFCS CALL node's off_start is
         * the member-token byte offset in root->parse_buffer — the method
         * name must sit exactly there.  This validates the offset substrate
         * against the whole corpus before any pass RELIES on offsets; a
         * mismatch means the lexer-offset capture regressed.  Warns by
         * default; CC_STRICT_OFFSETS=1 makes it fatal (CI mode). */
        if (root->parse_buffer && n[i].off_start >= 0 &&
            (size_t)n[i].off_start < root->parse_buffer_len) {
            const char* at = root->parse_buffer + n[i].off_start;
            const char* end = root->parse_buffer + root->parse_buffer_len;
            const char* q = at;
            /* the offset anchors the MEMBER CONSTRUCT: `.name` / `->name`
             * (separator first), bare `name` on the fallback path */
            if (q < end && *q == '.') q++;
            else if (q + 1 < end && q[0] == '-' && q[1] == '>') q += 2;
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            size_t rem = (size_t)(end - q);
            size_t ml = strlen(n[i].aux_s1);
            if (ml > rem || strncmp(q, n[i].aux_s1, ml) != 0) {
                fprintf(stderr,
                        "cc: OFFSET SELF-CHECK FAILED: ufcs node %d method '%s' "
                        "not at parse-buffer off %ld (found '%.24s') [%s:%d-%d]\n",
                        i, n[i].aux_s1, n[i].off_start,
                        (size_t)n[i].off_start < root->parse_buffer_len ? at : "",
                        n[i].file ? n[i].file : "?", ls, le);
                if (getenv("CC_STRICT_OFFSETS")) {
                    free(nodes);
                    cc_ufcs_set_symbols(NULL);
                    return -1;
                }
            }
        }
        if (node_count == node_cap) {
            int new_cap = node_cap ? node_cap * 2 : 32;
            struct UFCSNode* nn = (struct UFCSNode*)realloc(nodes, (size_t)new_cap * sizeof(*nn));
            if (!nn) { free(nodes); cc_ufcs_set_symbols(NULL); return 0; }
            nodes = nn;
            node_cap = new_cap;
        }
        int occ = (n[i].aux2 >> 8) & 0x00ffffff;
        if (occ <= 0) occ = 1;
        /* Dedupe compiler-duplicated expressions before probing: template
         * lowering (@string interpolation) duplicates interpolated exprs in
         * the parse buffer, so the recorder emits two nodes for ONE source
         * construct — identical (file, line, col, method).  The recorder
         * assigns both the same occ; keep only the first here so phantom
         * twins never reach the span probes (and can't mask a real no-span
         * failure).  Genuine same-method calls on one line differ in col —
         * and always in occ, so occ is part of the key: distinct calls with
         * a degenerate (uncaptured) col must never merge. */
        {
            int dup = 0;
            for (int k = 0; k < node_count; k++) {
                if (nodes[k].line_start != ls) continue;
                if (nodes[k].col_start != n[i].col_start) continue;
                if (nodes[k].occurrence_1based != occ) continue;
                if (strcmp(nodes[k].method, n[i].aux_s1) != 0) continue;
                if (!((nodes[k].file == n[i].file) ||
                      (nodes[k].file && n[i].file &&
                       strcmp(nodes[k].file, n[i].file) == 0))) continue;
                dup = 1;
                break;
            }
            if (dup) {
                if (getenv("CC_DEBUG_UFCS_NODES"))
                    fprintf(stderr, "[cc:ufcs-dedup] phantom twin dropped "
                            "file=%s line=%d col=%d method=%s occ=%d\n",
                            n[i].file ? n[i].file : "?", ls, n[i].col_start,
                            n[i].aux_s1, occ);
                continue;
            }
        }
        int recv_type_is_ptr = (n[i].aux2 & 2) ? 1 : 0;
        int under_await = 0;
        for (int p = n[i].parent; p >= 0 && p < root->node_count; p = n[p].parent) {
            if (n[p].kind == 6) { under_await = 1; break; }
        }
        nodes[node_count++] = (struct UFCSNode){
            .line_start = ls,
            .line_end = le,
            .col_start = n[i].col_start,
            .col_end = n[i].col_end,
            .method = n[i].aux_s1,
            .recv_type = n[i].aux_s2,
            .occurrence_1based = occ,
            .is_under_await = under_await,
            .recv_type_is_ptr = recv_type_is_ptr,
            .file = n[i].file,
            .off_start = n[i].off_start,
        };
        if (getenv("CC_DEBUG_UFCS_NODES")) {
            fprintf(stderr, "[cc:ufcs-node2] file=%s line=%d..%d col=%d method=%s recv=%s occ=%d off=%ld\n",
                    n[i].file ? n[i].file : "?", ls, le, n[i].col_start,
                    n[i].aux_s1 ? n[i].aux_s1 : "?",
                    n[i].aux_s2 ? n[i].aux_s2 : "?", occ, (long)n[i].off_start);
        }
    }

    if (node_count == 0) {
        free(nodes);
        cc_ufcs_set_symbols(NULL);
        return 0;
    }

    /* Bottom-up sort: process later-source nodes first so the `done[]`
     * containment check catches nested chain segments (inner `.m1()` calls
     * that get absorbed by the outer chain's receiver scan).  Order doesn't
     * affect offset validity (we never mutate in_src), but matches the
     * original rewriter's iteration order so containment relationships
     * resolve identically. */
    for (int i = 0; i < node_count; i++) {
        for (int j = i + 1; j < node_count; j++) {
            int swap = 0;
            if (nodes[j].line_start > nodes[i].line_start) swap = 1;
            else if (nodes[j].line_start == nodes[i].line_start) {
                if (nodes[j].col_start > 0 && nodes[i].col_start > 0) {
                    if (nodes[j].col_start > nodes[i].col_start) swap = 1;
                    else if (nodes[j].col_start == nodes[i].col_start &&
                             nodes[j].col_end > nodes[i].col_end) swap = 1;
                } else if (nodes[j].line_end > nodes[i].line_end) {
                    swap = 1;
                }
            }
            if (swap) {
                struct UFCSNode tmp = nodes[i];
                nodes[i] = nodes[j];
                nodes[j] = tmp;
            }
        }
    }

    struct CC__UFCSSpan* done = NULL;
    int done_count = 0;
    int done_cap = 0;

    int edits_added = 0;
    int err = 0;

    for (int i = 0; i < node_count && !err; i++) {
        int ls = nodes[i].line_start;
        int le = nodes[i].line_end;
        if (ls <= 0) continue;
        if (le < ls) le = ls;
        /* Offset-era resolver: VERIFIED CANDIDATES.  Enumerate every
         * physical offset whose logical (file,line) matches the node,
         * plus the raw physical line (the old "physical retry", now just
         * another candidate).  Probe each for the node's actual member
         * bytes — the LAST verified candidate wins, because a spliced
         * region's #line brackets re-establish user lines AFTER the
         * splice, so later candidates are the re-established user text.
         * No span without byte verification: the coordinates propose,
         * the bytes dispose. */
        struct CC__UFCSSpan sp;
        int have_span = 0;
        /* EXACT-FIRST: when this root's reparse carried the coordinate
         * invariant (parse_src_shift — 100% of corpus reparses after the
         * diet), the member separator's position in the edit buffer is
         * just off_start - shift.  The member bytes are still verified —
         * arithmetic alone is never trusted; a miss falls through to the
         * candidate resolver. */
        {
            size_t so = cc_pass_node_exact_off(root, nodes[i].off_start, in_len);
            if (so != (size_t)-1 &&
                cc__verify_member_at(in_src, in_len, so, nodes[i].method)) {
                size_t sep = so;
                size_t ln_lo = sep;
                while (ln_lo > 0 && in_src[ln_lo - 1] != '\n') ln_lo--;
                size_t mp = sep + (in_src[sep] == '.' ? 1 : 2);
                while (mp < in_len && isspace((unsigned char)in_src[mp])) mp++;
                mp += strlen(nodes[i].method);
                while (mp < in_len && isspace((unsigned char)in_src[mp])) mp++;
                size_t rp = 0;
                if (mp < in_len && in_src[mp] == '(' &&
                    cc_find_matching_paren(in_src, in_len, mp, &rp)) {
                    sp.start = cc__scan_receiver_start_left(in_src, ln_lo, sep);
                    sp.end = rp + 1;
                    if (sp.start < sp.end) have_span = 1;
                }
            }
        }
        if (!have_span) {
            size_t cand[32];
            int ncand = cc_span_logical_line_candidates(in_src, in_len, nodes[i].file, ls,
                                                        cand, 32);
            for (int c = 0; c < ncand; c++) {
                struct CC__UFCSSpan t;
                if (cc__span_at_candidate(in_src, in_len, cand[c], ls, le,
                                          nodes[i].col_start, nodes[i].col_end,
                                          nodes[i].method, nodes[i].occurrence_1based, &t)) {
                    sp = t;
                    have_span = 1;
                }
            }
            if (!have_span) {
                size_t phys = cc__offset_of_line_1based(in_src, in_len, ls);
                int dup = 0;
                for (int c = 0; c < ncand; c++) {
                    if (cand[c] == phys) { dup = 1; break; }
                }
                if (!dup && phys < in_len &&
                    cc__span_at_candidate(in_src, in_len, phys, ls, le,
                                          nodes[i].col_start, nodes[i].col_end,
                                          nodes[i].method, nodes[i].occurrence_1based, &sp)) {
                    have_span = 1;
                }
            }
        }
        if (!have_span) {
            /* No probe could locate this node in the edit buffer.  This is
             * NOT always a bug: template lowering (`@string` interpolation)
             * duplicates expressions in the parse buffer, producing phantom
             * higher-occ nodes with no source-line counterpart.  It was,
             * however, also the only symptom of the sanitizer line-eating
             * skew (the redis UFCS dead band) — that class is now caught
             * structurally by cc__check_sanitize_line_parity(), so a debug
             * trace suffices here. */
            if (getenv("CC_DEBUG_UFCS_NODES"))
                fprintf(stderr, "[cc:ufcs-skip] no-span method=%s line=%d occ=%d\n",
                        nodes[i].method, ls, nodes[i].occurrence_1based);
            continue;
        }
        if (sp.end > in_len || sp.start >= sp.end) continue;

        sp.end = cc__ufcs_extend_chain_end(in_src, in_len, sp.end);

        int covered = 0;
        for (int k = 0; k < done_count; k++) {
            if (sp.start >= done[k].start && sp.end <= done[k].end) {
                covered = 1;
                break;
            }
        }
        if (covered) {
            if (getenv("CC_DEBUG_UFCS_NODES"))
                fprintf(stderr, "[cc:ufcs-skip] covered method=%s line=%d span=%zu..%zu\n",
                        nodes[i].method, ls, sp.start, sp.end);
            continue;
        }

        size_t expr_len = sp.end - sp.start;
        size_t out_cap = expr_len * 2 + 256;
        char* out_buf = (char*)malloc(out_cap);
        if (!out_buf) continue;
        char* expr = (char*)malloc(expr_len + 1);
        if (!expr) { free(out_buf); continue; }
        memcpy(expr, in_src + sp.start, expr_len);
        expr[expr_len] = '\0';
        {
            const char* p = expr;
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            size_t p_len = strlen(p);
            int defers = cc_find_substr_top_level(p, 0, p_len, "@defer", 6) < p_len;
            if ((strncmp(p, "@defer", 6) == 0 &&
                 (p[6] == '\0' || p[6] == ' ' || p[6] == '\t' || p[6] == '\n' || p[6] == '\r' || p[6] == '(')) ||
                defers) {
                free(expr);
                free(out_buf);
                continue;
            }
        }
        cc_ufcs_set_source_context(in_src, sp.start);
        int rewrite_rc = cc_ufcs_rewrite_line_full(expr, out_buf, out_cap, nodes[i].is_under_await,
                                                   nodes[i].recv_type_is_ptr, nodes[i].recv_type);
        cc_ufcs_set_source_context(NULL, 0);

        if (rewrite_rc == CC_UFCS_REWRITE_UNRESOLVED ||
            rewrite_rc == CC_UFCS_REWRITE_AS_AMBIGUOUS ||
            rewrite_rc == CC_UFCS_REWRITE_AS_CYCLE) {
            char rel[1024];
            char file_buf[1024];
            char recv_expr[256];
            const char* lp = NULL;
            size_t lpl = 0;
            int user_line = cc_user_line_for_offset(in_src, in_len, sp.start, 1, &lp, &lpl);
            const char* raw_file = NULL;
            /* Prefer the buffer ledger (#line / CC_LN), then the AST node's
             * logical file from TCC, then the including TU.  Never blame the
             * TU for a call that lives in a spliced .cch. */
            if (lp && lpl > 0 && lpl < sizeof(file_buf)) {
                memcpy(file_buf, lp, lpl);
                file_buf[lpl] = '\0';
                raw_file = file_buf;
            } else if (nodes[i].file && nodes[i].file[0]) {
                raw_file = nodes[i].file;
                user_line = nodes[i].line_start;
            } else {
                raw_file = ctx->input_path ? ctx->input_path : "<input>";
                user_line = nodes[i].line_start;
            }
            const char* file = cc_path_rel_to_repo(raw_file, rel, sizeof(rel));
            int col = nodes[i].col_start > 0 ? nodes[i].col_start : 1;
            cc__ufcs_extract_receiver_expr(expr, recv_expr, sizeof(recv_expr));
            if (rewrite_rc == CC_UFCS_REWRITE_AS_AMBIGUOUS) {
                CCTypeRegistry* reg = cc_type_registry_get_global();
                char base[256];
                size_t bl = 0;
                const char* rt = nodes[i].recv_type ? nodes[i].recv_type : "";
                while (rt[bl] && rt[bl] != '*' && rt[bl] != ' ' && bl + 1 < sizeof(base)) {
                    base[bl] = rt[bl];
                    bl++;
                }
                base[bl] = '\0';
                cc_pass_error_cat(file, user_line, col, CC_ERR_TYPE,
                                  "ambiguous UFCS method '%s': multiple @as fields on '%s' provide it",
                                  nodes[i].method ? nodes[i].method : "<unknown>",
                                  base[0] ? base : "<unknown>");
                if (reg && base[0]) {
                    size_t nas = cc_type_registry_as_field_count(reg, base);
                    size_t ai;
                    for (ai = 0; ai < nas; ai++) {
                        const char* fn = NULL;
                        const char* ft = NULL;
                        if (cc_type_registry_as_field_at(reg, base, ai, &fn, &ft) != 0)
                            continue;
                        cc_pass_note(file, user_line, col,
                                     "  candidate @as field: %s %s",
                                     ft ? ft : "?", fn ? fn : "?");
                    }
                }
            } else if (rewrite_rc == CC_UFCS_REWRITE_AS_CYCLE) {
                cc_pass_error_cat(file, user_line, col, CC_ERR_TYPE,
                                  "cyclic @as embed graph while resolving UFCS method '%s' on '%s'",
                                  nodes[i].method ? nodes[i].method : "<unknown>",
                                  nodes[i].recv_type && nodes[i].recv_type[0]
                                      ? nodes[i].recv_type : "<unknown>");
                cc_pass_note(file, user_line, col,
                             "each @as edge must form a DAG; break the cycle or drop one embed");
            } else if (nodes[i].recv_type && nodes[i].recv_type[0]) {
                cc_pass_error_cat(file, user_line, col, CC_ERR_TYPE,
                                  "no UFCS method '%s' for receiver type '%s'",
                                  nodes[i].method ? nodes[i].method : "<unknown>",
                                  nodes[i].recv_type);
            } else {
                cc_pass_error_cat(file, user_line, col, CC_ERR_TYPE,
                                  "cannot resolve UFCS method '%s' because the receiver type is unknown",
                                  nodes[i].method ? nodes[i].method : "<unknown>");
            }
            if (recv_expr[0]) {
                cc_pass_note(file, user_line, col, "receiver expression: %s", recv_expr);
            }
            cc_pass_note(file, user_line, col, "offending call: %s", expr);
            /* Ambiguity / cycle already name the @as graph problem; skip the
             * "retry also failed" field dump used for plain unresolved. */
            if (rewrite_rc != CC_UFCS_REWRITE_AS_AMBIGUOUS &&
                rewrite_rc != CC_UFCS_REWRITE_AS_CYCLE &&
                nodes[i].recv_type && nodes[i].recv_type[0]) {
                CCTypeRegistry* reg = cc_type_registry_get_global();
                char base[256];
                size_t bl = 0;
                const char* rt = nodes[i].recv_type;
                while (rt[bl] && rt[bl] != '*' && rt[bl] != ' ' && bl + 1 < sizeof(base)) {
                    base[bl] = rt[bl];
                    bl++;
                }
                base[bl] = '\0';
                if (reg && base[0] && cc_type_registry_has_as_field(reg, base)) {
                    size_t nas = cc_type_registry_as_field_count(reg, base);
                    size_t ai;
                    cc_pass_note(file, user_line, col,
                                 "@as retry also failed on '%s' (%zu embed%s); "
                                 "no matching method on the outer type or its @as fields",
                                 base, nas, nas == 1 ? "" : "s");
                    for (ai = 0; ai < nas; ai++) {
                        const char* fn = NULL;
                        const char* ft = NULL;
                        if (cc_type_registry_as_field_at(reg, base, ai, &fn, &ft) != 0)
                            continue;
                        cc_pass_note(file, user_line, col,
                                     "  @as field: %s %s  (try %s.%s.%s(...))",
                                     ft ? ft : "?", fn ? fn : "?",
                                     recv_expr[0] ? recv_expr : "<recv>",
                                     fn ? fn : "?",
                                     nodes[i].method ? nodes[i].method : "?");
                    }
                } else {
                    cc_pass_note(file, user_line, col,
                                 "hint: UFCS dispatch is strict; register an exact or wildcard "
                                 "owner, or call the lowered function explicitly");
                }
            } else if (rewrite_rc != CC_UFCS_REWRITE_AS_AMBIGUOUS &&
                       rewrite_rc != CC_UFCS_REWRITE_AS_CYCLE) {
                cc_pass_note(file, user_line, col,
                             "hint: UFCS dispatch is strict; register an exact or wildcard owner, "
                             "or call the lowered function explicitly");
            }
            err = -1;
        } else if (rewrite_rc != CC_UFCS_REWRITE_OK) {
            if (getenv("CC_DEBUG_UFCS_NODES"))
                fprintf(stderr, "[cc:ufcs-skip] rewrite-rc=%d method=%s line=%d expr=%.60s\n",
                        rewrite_rc, nodes[i].method, ls, expr);
        } else if (rewrite_rc == CC_UFCS_REWRITE_OK) {
            if (cc_edit_buffer_add(eb, sp.start, sp.end, out_buf, 100, "ufcs") == 0) {
                edits_added++;
                if (done_count == done_cap) {
                    int new_cap = done_cap ? done_cap * 2 : 16;
                    struct CC__UFCSSpan* nd = (struct CC__UFCSSpan*)realloc(done, (size_t)new_cap * sizeof(*nd));
                    if (nd) { done = nd; done_cap = new_cap; }
                }
                if (done && done_count < done_cap) done[done_count++] = sp;
            }
        }
        free(expr);
        free(out_buf);
    }

    free(nodes);
    free(done);
    cc_ufcs_set_symbols(NULL);
    if (err < 0) return -1;
    return edits_added;
}
