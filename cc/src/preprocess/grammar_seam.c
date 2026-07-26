/*
 * @grammar(engine) Name {SENT … SENT} — the capture-and-route seam.
 *
 * The compiler's ONLY grammar knowledge: recognize the declaration, capture the
 * fenced body verbatim (never tokenized), and rewrite the whole declaration into
 * a synthesized @comptime block that calls the named engine:
 *
 *     @comptime { engine(cc_slice_from_cstr("Name"),
 *                        cc_slice_from_buffer((void*)"<escaped bytes>", <len>),
 *                        "<file>", <line>); }
 *
 * Everything downstream is existing machinery: the engine is an ordinary
 * registered @comptime function (executor.c registry), the block runs under the
 * libtcc executor, and the engine emits C via cc_emit_raw/cc_emit_format at the
 * usual anchors.  See cc/docs/GRAMMAR_DSL_PROPOSAL.md.
 *
 * Fence rules (heredoc discipline):
 *   - '{' must be immediately followed by a non-whitespace sentinel token
 *     (chars up to the first whitespace; non-empty; must not contain '}').
 *   - The single whitespace char terminating the sentinel is consumed; if it is
 *     a newline the body starts on the next line.
 *   - The body is every byte up to the first occurrence of "<sentinel>}" and is
 *     captured verbatim — braces, quotes, backslashes, anything.
 *
 * The rewrite replaces the declaration with a single physical line plus enough
 * trailing newlines to keep the line count identical (downstream diagnostics
 * depend on stable line numbers).
 */
#include "preprocess/preprocess.h"
#include "preprocess/grammar_engine.h"
#include "preprocess/variant_lower.h"
#include "util/text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CC__GRAMMAR_KW "@grammar"
#define CC__GRAMMAR_SENT_MAX 64
#define CC__GRAMMAR_IDENT_MAX 256

/* Portable memmem (glibc's needs _GNU_SOURCE; keep the pass self-contained). */
static const char* cc__find_bytes(const char* hay, size_t hay_len,
                                  const char* needle, size_t needle_len) {
    if (!needle_len || hay_len < needle_len) return NULL;
    for (size_t i = 0; i + needle_len <= hay_len; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, needle_len) == 0)
            return hay + i;
    return NULL;
}

/* Resolve pos -> (file,line) honoring cpp-style line directives ("#line N" or
 * "# N \"file\"") that earlier passes may have inserted (mirrors executor.c's
 * cc__resolve_origin). */
static void cc__grammar_origin(const char* src, size_t at, const char* input_path,
                               char* out_file, size_t out_file_sz, int* out_line) {
    int line = 1;
    size_t i = 0;
    if (out_file_sz) out_file[0] = '\0';
    while (i < at) {
        if (src[i] == '\n') {
            line++;
            size_t j = i + 1;
            while (j < at && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j < at && src[j] == '#') {
                size_t k = j + 1;
                while (k < at && (src[k] == ' ' || src[k] == '\t')) k++;
                if (k + 4 < at && strncmp(src + k, "line", 4) == 0) k += 4;
                while (k < at && (src[k] == ' ' || src[k] == '\t')) k++;
                if (k < at && src[k] >= '0' && src[k] <= '9') {
                    int n = 0;
                    while (k < at && src[k] >= '0' && src[k] <= '9') n = n * 10 + (src[k++] - '0');
                    while (k < at && (src[k] == ' ' || src[k] == '\t')) k++;
                    if (k < at && src[k] == '"') {
                        size_t fs = ++k, o = 0;
                        while (k < at && src[k] != '"') k++;
                        for (size_t m = fs; m < k && o + 1 < out_file_sz; m++) out_file[o++] = src[m];
                        if (out_file_sz) out_file[o] = '\0';
                    }
                    /* The directive names the NEXT line's number. */
                    size_t e = j;
                    while (e < at && src[e] != '\n') e++;
                    if (e < at) { line = n; i = e; /* '\n' handled next loop */ continue; }
                }
            }
        }
        i++;
    }
    if (out_file_sz && !out_file[0] && input_path)
        snprintf(out_file, out_file_sz, "%s", input_path);
    *out_line = line;
}

static void cc__grammar_err(const char* input_path, const char* src, size_t at,
                            const char* msg) {
    char file[512]; int line = 0;
    cc__grammar_origin(src, at, input_path, file, sizeof(file), &line);
    fprintf(stderr, "%s:%d: error: %s\n", file[0] ? file : "<input>", line, msg);
}

/* Append `n` bytes as the inside of a C string literal.  Octal escapes are
 * self-terminating at 3 digits, so any byte can be followed by any byte. */
static void cc__append_c_escaped(char** buf, size_t* len, size_t* cap,
                                 const char* s, size_t n) {
    char tmp[8];
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            tmp[0] = '\\'; tmp[1] = (char)c;
            cc_sb_append(buf, len, cap, tmp, 2);
        } else if (c >= 0x20 && c < 0x7F) {
            tmp[0] = (char)c;
            cc_sb_append(buf, len, cap, tmp, 1);
        } else {
            int m = snprintf(tmp, sizeof(tmp), "\\%03o", (unsigned)c);
            cc_sb_append(buf, len, cap, tmp, (size_t)(m > 0 ? m : 0));
        }
    }
}

/* Scan one @grammar declaration starting at `at` (src[at]=='@').  On success
 * fills the out-params and returns 1; returns 0 if this is not a @grammar
 * declaration; returns -1 on a malformed one (diagnostic already printed). */
static int cc__scan_grammar_decl(const char* src, size_t len, size_t at,
                                 const char* input_path,
                                 char* engine, size_t engine_sz,
                                 char* name, size_t name_sz,
                                 size_t* body_start, size_t* body_len,
                                 size_t* decl_end) {
    size_t p, q;
    if (!cc_match_ident_kw(src, len, at + 1, "grammar")) return 0;
    p = cc_skip_ws_and_comments(src, len, at + sizeof(CC__GRAMMAR_KW) - 1);
    if (p >= len || src[p] != '(') return 0;   /* not the decl form; leave alone */
    p = cc_skip_ws_and_comments(src, len, p + 1);
    q = p;
    while (q < len && cc_is_ident_char(src[q])) q++;
    if (q == p || q - p >= engine_sz) {
        cc__grammar_err(input_path, src, at, "@grammar: expected engine identifier in parentheses");
        return -1;
    }
    memcpy(engine, src + p, q - p); engine[q - p] = '\0';
    p = cc_skip_ws_and_comments(src, len, q);
    if (p >= len || src[p] != ')') {
        cc__grammar_err(input_path, src, at, "@grammar: expected ')' after engine name");
        return -1;
    }
    p = cc_skip_ws_and_comments(src, len, p + 1);
    q = p;
    if (q >= len || !cc_is_ident_start(src[q])) {
        cc__grammar_err(input_path, src, at, "@grammar: expected declared Name before body");
        return -1;
    }
    while (q < len && cc_is_ident_char(src[q])) q++;
    if (q - p >= name_sz) {
        cc__grammar_err(input_path, src, at, "@grammar: declared Name too long");
        return -1;
    }
    memcpy(name, src + p, q - p); name[q - p] = '\0';
    p = cc_skip_ws_and_comments(src, len, q);
    if (p >= len || src[p] != '{') {
        cc__grammar_err(input_path, src, at, "@grammar: expected '{' opening the fenced body");
        return -1;
    }
    /* Fence: sentinel immediately after '{' (no whitespace), up to first ws. */
    {
        size_t s0 = p + 1, s1 = s0;
        char sent[CC__GRAMMAR_SENT_MAX + 2];
        while (s1 < len && src[s1] != ' ' && src[s1] != '\t' &&
               src[s1] != '\n' && src[s1] != '\r')
            s1++;
        if (s1 == s0) {
            cc__grammar_err(input_path, src, p,
                "@grammar: fence sentinel required immediately after '{' (e.g. {~~~~ body ~~~~})");
            return -1;
        }
        if (s1 - s0 > CC__GRAMMAR_SENT_MAX) {
            cc__grammar_err(input_path, src, p, "@grammar: fence sentinel too long");
            return -1;
        }
        if (memchr(src + s0, '}', s1 - s0)) {
            cc__grammar_err(input_path, src, p, "@grammar: fence sentinel must not contain '}'");
            return -1;
        }
        if (s1 >= len) {
            cc__grammar_err(input_path, src, p, "@grammar: unterminated fenced body");
            return -1;
        }
        memcpy(sent, src + s0, s1 - s0);
        sent[s1 - s0] = '}';
        sent[s1 - s0 + 1] = '\0';
        /* Consume exactly the one whitespace char terminating the sentinel. */
        size_t b0 = s1 + 1;
        if (src[s1] == '\r' && b0 < len && src[b0] == '\n') b0++;   /* CRLF */
        const char* close = NULL;
        if (b0 < len)
            close = cc__find_bytes(src + b0, len - b0, sent, strlen(sent));
        if (!close) {
            cc__grammar_err(input_path, src, p, "@grammar: unterminated fenced body (closing sentinel not found)");
            return -1;
        }
        *body_start = b0;
        *body_len = (size_t)(close - src) - b0;
        *decl_end = (size_t)(close - src) + strlen(sent);   /* past "SENT}" */
    }
    return 1;
}

/* Returns NULL when the buffer has no @grammar declarations, (char*)-1 on a
 * malformed declaration (diagnostic already printed), else a fresh buffer. */
char* cc_rewrite_grammar_decls_text(const char* src, size_t n, const char* input_path) {
    char* out = NULL; size_t out_len = 0, out_cap = 0;
    size_t i = 0, copied = 0;
    int found = 0;

    if (!src) return NULL;
    /* Fast out for grammar-free streams BEFORE the registry reset: the seam
     * now runs in several pipelines per compile, and a no-op pass must not
     * wipe state (pending UFCS types, factories) a later phase consumes. */
    /* Grammar-free streams leave the schema-union queue alone: the seam
     * often runs twice per compile (prepare, then phase-1).  The second
     * pass may still see `@grammar` inside manifest comments even though
     * real decls are gone — clearing must wait until a real declaration
     * is processed (below), not a byte-scan hit. */
    if (!cc__find_bytes(src, n, CC__GRAMMAR_KW, sizeof(CC__GRAMMAR_KW) - 1))
        return NULL;
    while (i < n) {
        char c = src[i];
        /* Skip comments and string/char literals so "@grammar" inside them is inert. */
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) i++;
            i = i + 2 <= n ? i + 2 : n;
            continue;
        }
        if (c == '"' || c == '\'') {
            char qch = c; i++;
            while (i < n && src[i] != qch) { if (src[i] == '\\') i++; i++; }
            if (i < n) i++;
            continue;
        }
        if (c == '@' && cc_match_ident_kw(src, n, i + 1, "grammar")) {
            char engine[CC__GRAMMAR_IDENT_MAX], name[CC__GRAMMAR_IDENT_MAX];
            size_t body_start = 0, body_len = 0, decl_end = 0;
            int r = cc__scan_grammar_decl(src, n, i, input_path,
                                          engine, sizeof(engine), name, sizeof(name),
                                          &body_start, &body_len, &decl_end);
            if (r < 0) { free(out); return (char*)-1; }
            if (r == 1) {
                /* Reset on the FIRST real declaration, not on the keyword
                 * merely appearing in the stream: spliced output carries
                 * "@grammar" in manifest comments, and wiping the registry
                 * (pending UFCS types, schema names) on such a pass would
                 * strand state a later phase consumes. Cross-block state
                 * stays per-file: one reset ahead of the first decl. */
                if (!found) {
                    cc__grammar_registry_reset();
                    cc_variant_schema_pending_clear();
                }
                char file[512]; int line = 0;
                char head[1024];
                const char* ofile;
                cc__grammar_origin(src, i, input_path, file, sizeof(file), &line);
                ofile = file[0] ? file : (input_path ? input_path : "<input>");
                /* Copy everything before the declaration. */
                cc_sb_append(&out, &out_len, &out_cap, src + copied, i - copied);

                /* Builtin engine? Emit generated C natively and splice in place. */
                {
                    char gerr[512] = {0};
                    char* gen = cc_grammar_builtin_emit(engine, name, src + body_start,
                                                        body_len, ofile, line,
                                                        src, n,
                                                        gerr, sizeof(gerr));
                    if (gen) {
                        /* #line before/after so DSL body and following source
                         * keep faithful positions across the size change. */
                        int nl = 0;
                        for (size_t k = i; k < decl_end && k < n; k++)
                            if (src[k] == '\n') nl++;
                        snprintf(head, sizeof(head), "#line %d \"%s\"\n", line, ofile);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, head);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, gen);
                        if (out_len && out[out_len - 1] != '\n')
                            cc_sb_append(&out, &out_len, &out_cap, "\n", 1);
                        snprintf(head, sizeof(head), "#line %d \"%s\"\n", line + nl, ofile);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, head);
                        free(gen);
                        copied = decl_end;
                        i = decl_end;
                        found = 1;
                        continue;
                    }
                    if (gerr[0]) {   /* builtin engine parsed and rejected */
                        fprintf(stderr, "%s:%d: error: %s\n", ofile, line, gerr);
                        free(out);
                        return (char*)-1;
                    }
                    /* else: not a builtin — fall through to the comptime path */
                }

                /* User engine: synthesized @comptime call (one physical line). */
                snprintf(head, sizeof(head),
                         "@comptime { %s(cc_slice_from_cstr(\"%s\"), "
                         "cc_slice_from_buffer((void*)\"",
                         engine, name);
                cc_sb_append_cstr(&out, &out_len, &out_cap, head);
                cc__append_c_escaped(&out, &out_len, &out_cap, src + body_start, body_len);
                snprintf(head, sizeof(head), "\", %zu), \"%s\", %d); }", body_len, ofile, line);
                cc_sb_append_cstr(&out, &out_len, &out_cap, head);
                /* Preserve the physical line count of the replaced span. */
                for (size_t k = i; k < decl_end && k < n; k++)
                    if (src[k] == '\n') cc_sb_append(&out, &out_len, &out_cap, "\n", 1);
                copied = decl_end;
                i = decl_end;
                found = 1;
                continue;
            }
        }
        i++;
    }
    if (!found) { free(out); return NULL; }
    cc_sb_append(&out, &out_len, &out_cap, src + copied, n - copied);
    cc_sb_append(&out, &out_len, &out_cap, "", 1);   /* NUL */
    if (out) out[out_len - 1] = '\0';
    return out;
}

/* ---------------------------------------------------------------------- */
/* Type-scoped calls: `Type.method(args)` -> `Type_method(args)`.          */
/*                                                                         */
/* A type name in expression position is a C syntax error, so this cannot  */
/* wait for the UFCS pass (the broken parse would take every rewrite down  */
/* with it). Textual, pre-parse, and deliberately narrow:                  */
/*   - receiver is a bare PascalCase identifier NOT preceded by an         */
/*     expression tail (ident char, ')', ']', '.', '->')                   */
/*   - method is a lowercase identifier followed by '('                    */
/*   - the lowered name `Type_method` is visibly used/declared in THIS     */
/*     file's text (grammar splices qualify — the seam ran first), so a    */
/*     miss stays untouched and fails loudly in the C parse.               */
/* Strings, char literals, and comments are skipped.                       */

static int cc__tsc_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int cc__tsc_name_used(const char* src, size_t n, const char* name, size_t nl) {
    for (size_t i = 0; i + nl <= n; i++) {
        if (src[i] != name[0] || memcmp(src + i, name, nl) != 0) continue;
        if (i > 0 && cc__tsc_ident_char(src[i - 1])) continue;
        size_t j = i + nl;
        if (j < n && cc__tsc_ident_char(src[j])) continue;
        while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
        if (j < n && src[j] == '(') return 1;
    }
    return 0;
}

char* cc_rewrite_type_scoped_calls_text(const char* src, size_t n) {
    char* out = NULL; size_t out_len = 0, out_cap = 0;
    size_t i = 0, copied = 0;
    int found = 0;

    if (!src) return NULL;
    while (i < n) {
        char c = src[i];
        if (c == '"' || c == '\'') {                      /* string/char literal */
            char q = c; i++;
            while (i < n) {
                if (src[i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (src[i] == q) { i++; break; }
                i++;
            }
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') { /* line comment */
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') { /* block comment */
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) i++;
            i = i + 2 < n ? i + 2 : n;
            continue;
        }
        if (!(c >= 'A' && c <= 'Z') || (i > 0 && cc__tsc_ident_char(src[i - 1]))) {
            i++;
            continue;
        }
        /* previous non-ws must not be an expression tail */
        {
            size_t p = i;
            while (p > 0 && (src[p - 1] == ' ' || src[p - 1] == '\t')) p--;
            if (p > 0) {
                char pc = src[p - 1];
                if (cc__tsc_ident_char(pc) || pc == ')' || pc == ']' || pc == '.' ||
                    (pc == '>' && p > 1 && src[p - 2] == '-')) { i++; continue; }
            }
        }
        size_t ts = i, te = i;
        while (te < n && cc__tsc_ident_char(src[te])) te++;
        size_t j = te;
        while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
        if (!(j < n && src[j] == '.')) { i = te; continue; }
        j++;
        while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
        if (!(j < n && src[j] >= 'a' && src[j] <= 'z')) { i = te; continue; }
        size_t ms = j, me = j;
        while (me < n && cc__tsc_ident_char(src[me])) me++;
        j = me;
        while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
        if (!(j < n && src[j] == '(')) { i = te; continue; }
        {
            char fname[192];
            size_t tl = te - ts, ml = me - ms;
            if (tl + ml + 2 >= sizeof(fname)) { i = te; continue; }
            memcpy(fname, src + ts, tl);
            fname[tl] = '_';
            memcpy(fname + tl + 1, src + ms, ml);
            fname[tl + 1 + ml] = '\0';
            if (!cc__tsc_name_used(src, n, fname, tl + 1 + ml)) { i = te; continue; }
            cc_sb_append(&out, &out_len, &out_cap, src + copied, ts - copied);
            cc_sb_append(&out, &out_len, &out_cap, fname, tl + 1 + ml);
            copied = me;   /* keep everything from '(' on (incl. ws before it) */
            found = 1;
            i = me;
        }
    }
    if (!found) { free(out); return NULL; }
    cc_sb_append(&out, &out_len, &out_cap, src + copied, n - copied);
    cc_sb_append(&out, &out_len, &out_cap, "", 1);
    if (out) out[out_len - 1] = '\0';
    return out;
}
