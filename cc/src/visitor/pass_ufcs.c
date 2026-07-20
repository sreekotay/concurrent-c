/* Extracted from the working implementation in `cc/src/visitor/visitor.c`.
   Goal: keep semantics identical while shrinking visitor.c over time. */

#include "pass_ufcs.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "preprocess/preprocess.h"
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
static int cc__rewrite_ufcs_text_fallback(const CCVisitorCtx* ctx,
                                          const char* in_src,
                                          size_t in_len,
                                          char** out_src,
                                          size_t* out_len);
static int cc__line_has_await_keyword(const char* line);
static int cc__line_starts_with_decl_kw(const char* line);
static size_t cc__count_lines_to_offset(const char* s, size_t n, size_t off);

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

/* Cheap line-level detector for the `await` keyword.  Used by the text fallback
 * to mirror what the AST path discovers via parent traversal: if a line
 * contains a real `await ` token (not part of an identifier, string, or
 * comment) before the UFCS separator, we treat the call as await-context so
 * channel ops lower to the task-returning variants. */
static int cc__line_has_await_keyword(const char* line) {
    if (!line) return 0;
    size_t n = strlen(line);
    CCInertScan s;
    cc_inert_scan_init(&s, NULL);
    /* `line` is a single in-buffer line slice; not the start of a file, so a
     * leading `#` here would be code (preprocessor directives never reach this
     * fallback path).  Disable the `#` heuristic to preserve byte-equivalent
     * behavior with the previous hand-rolled scanner. */
    s.at_line_start = 0;
    for (size_t i = 0; i < n; ) {
        if (cc_inert_scan_step(&s, line, n, &i)) continue;
        if (line[i] != 'a' || i + 5 > n) { i++; continue; }
        if (memcmp(line + i, "await", 5) != 0) { i++; continue; }
        /* Word boundary: preceding char (if any) and following char must not
         * be identifier chars. */
        if (i > 0 && cc__is_ident_char_char(line[i - 1])) { i++; continue; }
        char nx = line[i + 5];
        if (cc__is_ident_char_char(nx)) { i++; continue; }
        return 1;
    }
    return 0;
}

/* Detect lines that look like a declaration introducer (typedef, struct/union
 * forward decl, etc).  These never carry executable UFCS, and producing a
 * spurious "unresolved UFCS" error from the fallback for such a line would be
 * a regression vs. the AST path. */
static int cc__line_starts_with_decl_kw(const char* line) {
    if (!line) return 0;
    while (*line == ' ' || *line == '\t') line++;
    static const char* kws[] = { "typedef", "struct", "union", "enum", "extern", "static", "#" };
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
        size_t kl = strlen(kws[i]);
        if (strncmp(line, kws[i], kl) == 0 &&
            (line[kl] == '\0' || line[kl] == ' ' || line[kl] == '\t' || line[kl] == '(')) {
            return 1;
        }
    }
    return 0;
}

static size_t cc__count_lines_to_offset(const char* s, size_t n, size_t off) {
    size_t lines = 1;
    size_t end = off < n ? off : n;
    for (size_t i = 0; i < end; i++) {
        if (s[i] == '\n') lines++;
    }
    return lines;
}

static int cc__rewrite_ufcs_text_fallback(const CCVisitorCtx* ctx,
                                          const char* in_src,
                                          size_t in_len,
                                          char** out_src,
                                          size_t* out_len) {
    if (!in_src || !out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;

    size_t out_cap = in_len + 1;
    char* out = (char*)malloc(out_cap);
    if (!out) return -1;
    size_t out_n = 0;
    int changed = 0;

    for (size_t line_start = 0; line_start < in_len; ) {
        size_t line_end = line_start;
        while (line_end < in_len && in_src[line_end] != '\n') line_end++;
        size_t line_len = line_end - line_start;
        int has_nl = (line_end < in_len && in_src[line_end] == '\n') ? 1 : 0;

        char* line = (char*)malloc(line_len + 1);
        if (!line) {
            free(out);
            return -1;
        }
        memcpy(line, in_src + line_start, line_len);
        line[line_len] = '\0';

        const char* emit = line;
        char* rewritten = NULL;
        const char* trim = line;
        while (*trim == ' ' || *trim == '\t' || *trim == '\r') trim++;
        if (*trim != '#') {
            size_t rew_cap = line_len * 2 + 256;
            rewritten = (char*)malloc(rew_cap);
            if (!rewritten) {
                free(line);
                free(out);
                return -1;
            }
            int is_under_await = cc__line_has_await_keyword(line);
            cc_ufcs_set_source_context(in_src, line_start);
            /* Use the full rewrite entry point so await-context lowers channel
             * ops to the task-returning variants, matching the AST path.  We
             * pass recv_type_is_ptr=0 / recv_type=NULL because the text path
             * has no AST hint; the rewriter falls back to the type registry
             * (which is now seeded with pointer-ness for Map<K,V>). */
            int rc = cc_ufcs_rewrite_line_full(line, rewritten, rew_cap,
                                               is_under_await,
                                               /*recv_type_is_ptr=*/0,
                                               /*recv_type=*/NULL);
            cc_ufcs_set_source_context(NULL, 0);
            if (rc == CC_UFCS_REWRITE_UNRESOLVED && !cc__line_starts_with_decl_kw(line)) {
                /* Mirror the AST path's hard-error behavior so a strictly
                 * unresolved UFCS in fallback territory (typically a closure
                 * body or macro argument the AST couldn't see into) does not
                 * silently slip through to the host C compiler with a bogus
                 * `recv.method(args)` lowering. */
                char rel[1024];
                const char* file = "<input>";
                if (ctx && ctx->input_path) {
                    file = cc_path_rel_to_repo(ctx->input_path, rel, sizeof(rel));
                }
                size_t lineno = cc__count_lines_to_offset(in_src, in_len, line_start);
                cc_pass_error_cat(file, (int)lineno, 1, CC_ERR_TYPE,
                                  "cannot resolve UFCS call (text fallback)");
                cc_pass_note(file, (int)lineno, 1, "offending line: %s", line);
                cc_pass_note(file, (int)lineno, 1,
                             "hint: register an exact or wildcard owner via "
                             "cc_type_register, or call the lowered function explicitly");
                free(rewritten);
                free(line);
                free(out);
                return -1;
            }
            if (rc == CC_UFCS_REWRITE_OK && strcmp(rewritten, line) != 0) {
                if (getenv("CC_DEBUG_UFCS_FALLBACK")) {
                    fprintf(stderr, "[ufcs-fb] BEFORE: %s\n[ufcs-fb] AFTER:  %s\n", line, rewritten);
                }
                emit = rewritten;
                changed = 1;
            }
        }

        size_t emit_len = strlen(emit);
        if (out_n + emit_len + (size_t)has_nl + 1 > out_cap) {
            size_t new_cap = (out_cap * 2 > out_n + emit_len + (size_t)has_nl + 1)
                           ? out_cap * 2
                           : out_n + emit_len + (size_t)has_nl + 1;
            char* next = (char*)realloc(out, new_cap);
            if (!next) {
                free(rewritten);
                free(line);
                free(out);
                return -1;
            }
            out = next;
            out_cap = new_cap;
        }
        memcpy(out + out_n, emit, emit_len);
        out_n += emit_len;
        if (has_nl) out[out_n++] = '\n';

        free(rewritten);
        free(line);
        line_start = line_end + (size_t)has_nl;
    }

    out[out_n] = '\0';
    if (!changed) {
        free(out);
        return 0;
    }
    *out_src = out;
    *out_len = out_n;
    return 1;
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

    seg_start = r_end;
    while (seg_start > range_start && cc__is_ident_char_char(s[seg_start - 1])) seg_start--;
    if (seg_start == r_end || !cc__is_ident_start_char(s[seg_start])) return sep_pos;

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
        if (q > range_start && s[q - 1] == ')') {
            int depth = 1;
            size_t pp = q - 1;
            while (pp > range_start && depth > 0) {
                pp--;
                if (s[pp] == ')') depth++;
                else if (s[pp] == '(') depth--;
            }
            if (depth == 0) {
                seg_start = pp;
                continue;
            }
            break;
        }
        if (q > range_start && s[q - 1] == ']') {
            int depth = 1;
            size_t pp = q - 1;
            while (pp > range_start && depth > 0) {
                pp--;
                if (s[pp] == ']') depth++;
                else if (s[pp] == '[') depth--;
            }
            if (depth == 0) {
                seg_start = pp;
                continue;
            }
            break;
        }
        if (q <= range_start || !cc__is_ident_char_char(s[q - 1])) break;
        seg_start = q;
        while (seg_start > range_start && cc__is_ident_char_char(s[seg_start - 1])) seg_start--;
        if (!cc__is_ident_start_char(s[seg_start])) return sep_pos;
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
 * `CC_UFCS_TEXT_FALLBACK=1` (debug only, off by default) preserves the
 * legacy wholesale fallback path — emits a single whole-buffer edit.
 * This is intentionally compatible only with the non-batched pipeline;
 * it exists purely as a diagnostic for AST-resolver regressions.
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
    if (!root->nodes || root->node_count <= 0) {
        /* Even with no AST nodes the text-only fallback may want to run. */
        if (!getenv("CC_UFCS_TEXT_FALLBACK")) return 0;
    }

    cc_ufcs_set_symbols(ctx->symbols);

    const char* in_src = eb->src;
    size_t in_len = eb->src_len;

    /* CC_UFCS_TEXT_FALLBACK debug path: run the legacy whole-buffer text
     * rewriter and emit a single coarse edit.  Mutually exclusive with
     * batched mode (would collide); intended only for AST-resolver
     * debugging where bypassing the AST path is the whole point. */
    if (getenv("CC_UFCS_TEXT_FALLBACK")) {
        char* fallback = NULL;
        size_t fallback_len = 0;
        int fr = cc__rewrite_ufcs_text_fallback(ctx, in_src, in_len, &fallback, &fallback_len);
        cc_ufcs_set_symbols(NULL);
        if (fr < 0) { free(fallback); return -1; }
        if (fr <= 0 || !fallback) { free(fallback); return 0; }
        if (fallback_len == in_len && memcmp(fallback, in_src, in_len) == 0) {
            free(fallback);
            return 0;
        }
        int rc = cc_edit_buffer_add(eb, 0, in_len, fallback, 100, "ufcs");
        free(fallback);
        return rc == 0 ? 1 : -1;
    }

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
        };
        if (getenv("CC_DEBUG_UFCS_NODES")) {
            fprintf(stderr, "[cc:ufcs-node2] file=%s line=%d..%d method=%s recv=%s occ=%d\n",
                    n[i].file ? n[i].file : "?", ls, le,
                    n[i].aux_s1 ? n[i].aux_s1 : "?",
                    n[i].aux_s2 ? n[i].aux_s2 : "?", occ);
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
        {
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
        if (!have_span) continue;
        if (sp.end > in_len || sp.start >= sp.end) continue;

        sp.end = cc__ufcs_extend_chain_end(in_src, in_len, sp.end);

        int covered = 0;
        for (int k = 0; k < done_count; k++) {
            if (sp.start >= done[k].start && sp.end <= done[k].end) {
                covered = 1;
                break;
            }
        }
        if (covered) continue;

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

        if (rewrite_rc == CC_UFCS_REWRITE_UNRESOLVED) {
            char rel[1024];
            char recv_expr[256];
            const char* file = cc_path_rel_to_repo(ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
            int col = nodes[i].col_start > 0 ? nodes[i].col_start : 1;
            cc__ufcs_extract_receiver_expr(expr, recv_expr, sizeof(recv_expr));
            if (nodes[i].recv_type && nodes[i].recv_type[0]) {
                cc_pass_error_cat(file, nodes[i].line_start, col, CC_ERR_TYPE,
                                  "no UFCS method '%s' for receiver type '%s'",
                                  nodes[i].method ? nodes[i].method : "<unknown>",
                                  nodes[i].recv_type);
            } else {
                cc_pass_error_cat(file, nodes[i].line_start, col, CC_ERR_TYPE,
                                  "cannot resolve UFCS method '%s' because the receiver type is unknown",
                                  nodes[i].method ? nodes[i].method : "<unknown>");
            }
            if (recv_expr[0]) {
                cc_pass_note(file, nodes[i].line_start, col, "receiver expression: %s", recv_expr);
            }
            cc_pass_note(file, nodes[i].line_start, col, "offending call: %s", expr);
            cc_pass_note(file, nodes[i].line_start, col,
                         "hint: UFCS dispatch is strict; register an exact or wildcard owner, or call the lowered function explicitly");
            err = -1;
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
