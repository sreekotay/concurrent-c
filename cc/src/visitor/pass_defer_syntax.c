#include "pass_defer_syntax.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/path.h"
#include "util/text.h"
#include "util/text_scan.h"
#include "visitor/edit_buffer.h"
#include "visitor/pass_common.h"

typedef enum {
    DEFER_ALWAYS = 0,  /* @defer - always runs */
    DEFER_ON_ERR,      /* @defer(err) - only on error return */
    DEFER_ON_OK        /* @defer(ok) - only on success return */
} CCDeferCondition;

typedef struct {
    int line_no;
    char* stmt; /* includes trailing ';' */
    CCDeferCondition cond;
} CCDeferStmt;

typedef struct {
    int active;
    int cleanup_label_id;
    int has_top_level_defers;
    int has_top_level_conditional;
    int used_goto;
    int is_async;
    int is_void;
    int has_result_return;
    int open_line; /* ledger user line of the fn's `{` (0 = unknown) */
    char return_type[256];
    char ok_ctor[320];
    char err_ctor[320];
} CCDeferFunctionScope;

static int g_defer_cleanup_label_id = 0;

static void cc__free_defer_list(CCDeferStmt* xs, int n) {
    if (!xs) return;
    for (int i = 0; i < n; i++) free(xs[i].stmt);
    free(xs);
}

/* Local aliases for the shared helpers */
#define cc__is_ident_start cc_is_ident_start
#define cc__is_ident_char cc_is_ident_char
#define cc__append_n cc_sb_append
#define cc__append_str cc_sb_append_cstr
CC_DEFINE_SB_APPEND_FMT

static int cc__token_is(const char* s, size_t len, size_t i, const char* tok);

/* True if the token at `i` is not immediately preceded (past ASCII
 * whitespace) by a C member-access operator (`.` or `->`). Intended for
 * statement-position keywords like `cancel`, which must not match a method
 * call named `cancel` (e.g. `obj->chan.cancel()`). */
static int cc__not_member_access_prefix(const char* s, size_t i) {
    size_t k = i;
    while (k > 0 && (s[k - 1] == ' ' || s[k - 1] == '\t' ||
                     s[k - 1] == '\r' || s[k - 1] == '\n')) k--;
    if (k == 0) return 1;
    if (s[k - 1] == '.') return 0;
    if (k >= 2 && s[k - 2] == '-' && s[k - 1] == '>') return 0;
    return 1;
}

static void cc__append_newline_padding(char** out, size_t* out_len, size_t* out_cap,
                                       const char* src, size_t n) {
    if (!out || !out_len || !out_cap || !src) return;
    for (size_t i = 0; i < n; i++) {
        if (src[i] == '\n') {
            cc__append_n(out, out_len, out_cap, "\n", 1);
        }
    }
}

static size_t cc__current_line_indent_len(const char* out, size_t out_len) {
    size_t line_start = out_len;
    size_t i;
    if (!out || out_len == 0) return 0;
    while (line_start > 0 && out[line_start - 1] != '\n') line_start--;
    i = line_start;
    while (i < out_len && (out[i] == ' ' || out[i] == '\t')) i++;
    if (i != out_len) return 0;
    return out_len - line_start;
}

static size_t cc__source_line_indent_len(const char* src, size_t pos) {
    size_t line_start = pos;
    size_t indent = 0;
    if (!src) return 0;
    while (line_start > 0 && src[line_start - 1] != '\n') line_start--;
    while (line_start + indent < pos &&
           (src[line_start + indent] == ' ' || src[line_start + indent] == '\t')) {
        indent++;
    }
    return indent;
}

static size_t cc__suggest_statement_indent(const char* out, size_t out_len) {
    size_t line_end = out_len;
    size_t line_start = out_len;

    if (!out || out_len == 0) return 0;
    while (line_start > 0 && out[line_start - 1] != '\n') line_start--;
    while (line_start > 0) {
        size_t prev_end = line_start - 1;
        size_t prev_start = prev_end;
        size_t indent = 0;
        size_t trim_end;
        while (prev_start > 0 && out[prev_start - 1] != '\n') prev_start--;
        trim_end = prev_end;
        while (trim_end > prev_start && (out[trim_end - 1] == ' ' || out[trim_end - 1] == '\t' || out[trim_end - 1] == '\r')) trim_end--;
        if (trim_end > prev_start) {
            while (prev_start + indent < trim_end &&
                   (out[prev_start + indent] == ' ' || out[prev_start + indent] == '\t')) {
                indent++;
            }
            if (out[trim_end - 1] == '{') return indent + 4;
            return indent;
        }
        line_start = prev_start;
        line_end = prev_end;
        (void)line_end;
    }
    return 0;
}

static void cc__append_missing_indent_to(char** out, size_t* out_len, size_t* out_cap, size_t target) {
    static const char spaces[] = "                                ";
    size_t have = cc__current_line_indent_len(*out, *out_len);
    while (have < target) {
        size_t chunk = target - have;
        if (chunk > sizeof(spaces) - 1) chunk = sizeof(spaces) - 1;
        cc__append_n(out, out_len, out_cap, spaces, chunk);
        have += chunk;
    }
}

/* True when the return type is a plain arithmetic spelling for which the
 * cleanup epilogue's fallback can read `return 0;` instead of the generic
 * `return (__typeof__(__cc_retval_N)){0};` compound literal. */
static int cc__ret_type_plain_zero_ok(const char* t) {
    static const char* words[] = {
        "signed", "unsigned", "short", "long", "int", "char",
        "bool", "_Bool", "float", "double",
        "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t",
        "int8_t", "int16_t", "int32_t", "int64_t",
        "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    };
    int seen = 0;
    if (!t) return 0;
    while (*t) {
        while (*t == ' ' || *t == '\t') t++;
        if (!*t) break;
        const char* w = t;
        while (*t && *t != ' ' && *t != '\t') t++;
        size_t wl = (size_t)(t - w);
        int ok = 0;
        for (size_t k = 0; k < sizeof(words) / sizeof(words[0]); k++) {
            if (strlen(words[k]) == wl && memcmp(words[k], w, wl) == 0) { ok = 1; break; }
        }
        if (!ok) return 0;
        seen = 1;
    }
    return seen;
}

/* Emit return-through-cleanup without depending on `__cc_ret*` macros.
 * Parser-mode stubs define `__cc_ret` as `((void)(value))`; if those ever
 * leak into final codegen (or a local is named `__cc_ret`), a macro call
 * can drop the exit code. Inline set+goto matches the async path. */
static void cc__emit_ret_cleanup_begin(char** out, size_t* out_len, size_t* out_cap, int id) {
    cc_sb_append_fmt(out, out_len, out_cap, "__cc_retval_%d = (", id);
}

/* `close` is ");" for plain assign, "));" for cc_ok_into / cc_err_into.
 *
 * The three cleanup statements (`__cc_retval = (...)`, `__cc_ret_set = 1`,
 * `goto __cc_cleanup`) are the lowering of a SINGLE source `return` and are
 * emitted on ONE physical line so the whole blob maps to exactly the
 * return's source line via the CC_LN marker the caller stamps ahead of it.
 * Spreading them over three physical lines with no per-line ledger entry
 * inflated the buffer +2 lines per return; for a `return` near end-of-file
 * (e.g. the final `return 0;` in redis_idiomatic's `main`) that pushed the
 * trailing physical lines' #line mapping past the source's EOF. */
static void cc__emit_ret_cleanup_end(char** out, size_t* out_len, size_t* out_cap,
                                     int id, size_t indent, const char* close) {
    (void)indent;
    cc__append_str(out, out_len, out_cap, close ? close : ");");
    cc_sb_append_fmt(out, out_len, out_cap, " __cc_ret_set_%d = 1;", id);
    cc_sb_append_fmt(out, out_len, out_cap, " goto __cc_cleanup_%d;", id);
}

/* Stamp a masked CC_LN ledger marker ahead of a return-cleanup blob so the
 * blob maps to the return's true source line.  No-op unless the current
 * output line is whitespace-only (a marker mid-line is neither a valid
 * ledger entry nor valid C) and the ledger yielded a real user line and
 * path.  `indent` matches the blob's indent for readability only. */
static void cc__emit_ret_line_marker(char** out, size_t* out_len, size_t* out_cap,
                                     size_t indent, int user_line,
                                     const char* path, size_t path_len) {
    size_t k;
    if (user_line <= 0 || !path || path_len == 0 || path_len >= 1024) return;
    k = *out_len;
    while (k > 0 && (*out)[k - 1] != '\n') {
        char c = (*out)[k - 1];
        if (c != ' ' && c != '\t' && c != '\r') return; /* mid-line: skip */
        k--;
    }
    cc__append_missing_indent_to(out, out_len, out_cap, indent);
    cc_sb_append_fmt(out, out_len, out_cap, "/*CC_LN %d %.*s*/\n",
                     user_line, (int)path_len, path);
}

static void cc__emit_ret_ok_cleanup_begin(char** out, size_t* out_len, size_t* out_cap, int id) {
    cc_sb_append_fmt(out, out_len, out_cap, "cc_ok_into(__cc_retval_%d, (", id);
}

static void cc__emit_ret_err_cleanup_begin(char** out, size_t* out_len, size_t* out_cap, int id) {
    cc_sb_append_fmt(out, out_len, out_cap, "cc_err_into(__cc_retval_%d, (", id);
}

static int cc__count_top_level_semicolons(const char* s, size_t n) {
    int count = 0;
    int par = 0, brk = 0, br = 0;
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    size_t i = 0;
    while (i < n) {
        if (cc_inert_scan_step(&scan, s, n, &i)) continue;
        char ch = s[i];
        if (ch == '(') par++;
        else if (ch == ')' && par > 0) par--;
        else if (ch == '[') brk++;
        else if (ch == ']' && brk > 0) brk--;
        else if (ch == '{') br++;
        else if (ch == '}' && br > 0) br--;
        else if (ch == ';' && par == 0 && brk == 0 && br == 0) count++;
        i++;
    }
    return count;
}

static char* cc__normalize_defer_stmt(char* stmt) {
    size_t start, end, inner_start, inner_end;
    int top_level_semis;
    char* normalized;

    if (!stmt) return NULL;
    start = 0;
    end = strlen(stmt);
    while (start < end && isspace((unsigned char)stmt[start])) start++;
    while (end > start && isspace((unsigned char)stmt[end - 1])) end--;
    if (end <= start) return stmt;

    if (stmt[start] != '{' || end - start < 3 || stmt[end - 1] != ';' || stmt[end - 2] != '}') {
        return stmt;
    }

    inner_start = start + 1;
    inner_end = end - 2;
    while (inner_start < inner_end && isspace((unsigned char)stmt[inner_start])) inner_start++;
    while (inner_end > inner_start && isspace((unsigned char)stmt[inner_end - 1])) inner_end--;
    if (inner_end <= inner_start) return stmt;

    top_level_semis = cc__count_top_level_semicolons(stmt + inner_start, inner_end - inner_start);
    if (top_level_semis == 1) {
        size_t new_len = inner_end - inner_start;
        normalized = (char*)malloc(new_len + 1);
        if (!normalized) return stmt;
        memcpy(normalized, stmt + inner_start, new_len);
        normalized[new_len] = 0;
        free(stmt);
        return normalized;
    }

    /* Wrap multi-stmt body as `{ ... }` with breathing space — the
     * normalized form is what gets injected verbatim at every
     * function exit / block-exit point, so spacing here propagates
     * directly into the lowered C the user reads.  Pre-2026-05-27
     * this packed it as `{...}` and a 2-stmt defer rendered as the
     * notoriously cramped `{cc_nursery_wait(x); cc_nursery_free(x);}`. */
    size_t body_len = inner_end - inner_start;
    normalized = (char*)malloc(body_len + 5);
    if (!normalized) return stmt;
    normalized[0] = '{';
    normalized[1] = ' ';
    memcpy(normalized + 2, stmt + inner_start, body_len);
    normalized[2 + body_len] = ' ';
    normalized[3 + body_len] = '}';
    normalized[4 + body_len] = 0;
    free(stmt);
    return normalized;
}

static int cc__keyword_eq_range(const char* s, size_t n, const char* kw) {
    return strlen(kw) == n && strncmp(s, kw, n) == 0;
}

static int cc__is_control_keyword_range(const char* s, size_t n) {
    return cc__keyword_eq_range(s, n, "if") ||
           cc__keyword_eq_range(s, n, "for") ||
           cc__keyword_eq_range(s, n, "while") ||
           cc__keyword_eq_range(s, n, "switch");
}

static int cc__is_storage_keyword_range(const char* s, size_t n) {
    return cc__keyword_eq_range(s, n, "static") ||
           cc__keyword_eq_range(s, n, "extern") ||
           cc__keyword_eq_range(s, n, "inline") ||
           cc__keyword_eq_range(s, n, "__inline__") ||
           cc__keyword_eq_range(s, n, "_Noreturn");
}

/* Scan forward from 0 to `end` tracking block/line comments and string/char
 * literals.  Return the offset just after the last `;`, `{`, or `}` that is
 * NOT inside a comment or string literal.  Returns 0 if there is no such
 * terminator (i.e. the declaration is at top of file).  Critical for
 * cc__extract_function_return_type: a naive backward walk from the function
 * name happily lands on a `;` inside a long block-comment header,
 * which then makes the return-type extractor splice a huge chunk of source
 * into the function prologue.
 *
 * Hot path: defer rewrite builds a prefix table once (g_defer_last_term[end])
 * so each function brace is O(1) instead of O(end). */
static const size_t* g_defer_last_term = NULL;
static size_t g_defer_last_term_len = 0;

static size_t* cc__build_last_stmt_terminator_prefix(const char* s, size_t len) {
    size_t* pref;
    size_t last = 0;
    size_t i = 0;
    CCInertScan scan;
    if (!s) return NULL;
    pref = (size_t*)malloc((len + 1) * sizeof(size_t));
    if (!pref) return NULL;
    cc_inert_scan_init(&scan, NULL);
    while (i < len) {
        pref[i] = last;
        if (cc_inert_scan_step(&scan, s, len, &i)) continue;
        {
            char c = s[i];
            if (c == ';' || c == '{' || c == '}') last = i + 1;
        }
        i++;
    }
    pref[len] = last;
    return pref;
}

static size_t cc__last_stmt_terminator_before(const char* s, size_t end) {
    size_t last = 0;
    CCInertScan scan;
    size_t i = 0;
    if (!s || end == 0) return 0;
    if (g_defer_last_term && end <= g_defer_last_term_len)
        return g_defer_last_term[end];
    cc_inert_scan_init(&scan, NULL);
    while (i < end) {
        if (cc_inert_scan_step(&scan, s, end, &i)) continue;
        {
            char c = s[i];
            if (c == ';' || c == '{' || c == '}') last = i + 1;
        }
        i++;
    }
    return last;
}

static size_t cc__skip_decl_noise(const char* s, size_t pos, size_t end) {
    if (!s) return pos;
    for (;;) {
        while (pos < end && isspace((unsigned char)s[pos])) pos++;
        if (pos + 1 < end && s[pos] == '/' && s[pos + 1] == '*') {
            pos += 2;
            while (pos + 1 < end && !(s[pos] == '*' && s[pos + 1] == '/')) pos++;
            if (pos + 1 < end) pos += 2;
            continue;
        }
        if (pos + 1 < end && s[pos] == '/' && s[pos + 1] == '/') {
            pos += 2;
            while (pos < end && s[pos] != '\n') pos++;
            continue;
        }
        if (pos < end && s[pos] == '#') {
            while (pos < end && s[pos] != '\n') pos++;
            continue;
        }
        if (pos < end && s[pos] == '@') {
            size_t attr_start = pos + 1;
            size_t attr_end = attr_start;
            while (attr_end < end && cc__is_ident_char(s[attr_end])) attr_end++;
            if (attr_end > attr_start &&
                (cc__keyword_eq_range(s + attr_start, attr_end - attr_start, "async") ||
                 cc__keyword_eq_range(s + attr_start, attr_end - attr_start, "unsafe"))) {
                pos = attr_end;
                continue;
            }
        }
        break;
    }
    return pos;
}

static int cc__decl_has_async_attr(const char* s, size_t pos, size_t end) {
    if (!s) return 0;
    while (pos < end) {
        while (pos < end && isspace((unsigned char)s[pos])) pos++;
        if (pos + 1 < end && s[pos] == '/' && s[pos + 1] == '*') {
            pos += 2;
            while (pos + 1 < end && !(s[pos] == '*' && s[pos + 1] == '/')) pos++;
            if (pos + 1 < end) pos += 2;
            continue;
        }
        if (pos + 1 < end && s[pos] == '/' && s[pos + 1] == '/') {
            pos += 2;
            while (pos < end && s[pos] != '\n') pos++;
            continue;
        }
        if (pos < end && s[pos] == '#') {
            while (pos < end && s[pos] != '\n') pos++;
            continue;
        }
        if (pos < end && s[pos] == '@') {
            size_t attr_start = pos + 1;
            size_t attr_end = attr_start;
            while (attr_end < end && cc__is_ident_char(s[attr_end])) attr_end++;
            if (attr_end > attr_start &&
                cc__keyword_eq_range(s + attr_start, attr_end - attr_start, "async")) {
                return 1;
            }
            pos = attr_end;
            continue;
        }
        break;
    }
    return 0;
}

static int cc__find_matching_brace_text(const char* s, size_t len, size_t open_i, size_t* out_close_i) {
    /* Find the `}` that matches the `{` at `open_i`, skipping
     * inert content (comments, strings, char literals, pp
     * directives) via the shared `CCInertScan` helper. */
    if (!s || open_i >= len || s[open_i] != '{') return 0;
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    int depth = 1;
    size_t i = open_i + 1;
    while (i < len) {
        if (cc_inert_scan_step(&scan, s, len, &i)) continue;
        char ch = s[i];
        if (ch == '{') { depth++; i++; continue; }
        if (ch == '}') {
            depth--;
            if (depth == 0) {
                if (out_close_i) *out_close_i = i;
                return 1;
            }
            i++;
            continue;
        }
        i++;
    }
    return 0;
}

static int cc__scan_function_top_level_defer_info(const char* s, size_t len, size_t open_i,
                                                  int* out_has_defer, int* out_has_conditional) {
    size_t close_i = 0;
    int rel_depth = 1;
    CCInertScan scan;

    if (out_has_defer) *out_has_defer = 0;
    if (out_has_conditional) *out_has_conditional = 0;
    if (!s || open_i >= len || s[open_i] != '{') return 0;
    if (!cc__find_matching_brace_text(s, len, open_i, &close_i)) return 0;

    cc_inert_scan_init(&scan, NULL);
    scan.at_line_start = 0;  /* mid-buffer slice (inside function body) */
    size_t i = open_i + 1;
    while (i < close_i) {
        if (cc_inert_scan_step(&scan, s, close_i, &i)) continue;
        char ch = s[i];
        if (ch == '{') { rel_depth++; i++; continue; }
        if (ch == '}') { rel_depth--; i++; continue; }
        if (rel_depth == 1 && cc__token_is(s, len, i, "@defer")) {
            size_t j = i + 6;
            if (out_has_defer) *out_has_defer = 1;
            while (j < close_i && (s[j] == ' ' || s[j] == '\t')) j++;
            if (j < close_i && s[j] == '(') {
                if (out_has_conditional) *out_has_conditional = 1;
            }
        }
        i++;
    }
    return 1;
}

static int cc__extract_function_return_type(const char* s, size_t len, size_t brace_i,
                                            char* out_type, size_t out_type_sz,
                                            int* out_is_void, int* out_is_async) {
    size_t j, close_paren, open_paren, name_end, name_start, prefix_start, prefix_end, keep_start;
    int par = 1;

    if (out_is_void) *out_is_void = 0;
    if (out_is_async) *out_is_async = 0;
    if (!s || !out_type || out_type_sz == 0 || brace_i >= len) return 0;

    j = brace_i;
    while (j > 0 && isspace((unsigned char)s[j - 1])) j--;
    if (j == 0 || s[j - 1] != ')') return 0;
    close_paren = j - 1;
    open_paren = close_paren;
    while (open_paren > 0) {
        open_paren--;
        if (s[open_paren] == ')') par++;
        else if (s[open_paren] == '(') {
            par--;
            if (par == 0) break;
        }
    }
    if (par != 0) return 0;

    name_end = open_paren;
    while (name_end > 0 && isspace((unsigned char)s[name_end - 1])) name_end--;
    name_start = name_end;
    while (name_start > 0 && cc__is_ident_char(s[name_start - 1])) name_start--;
    if (name_start == name_end || !cc__is_ident_start(s[name_start])) return 0;
    if (cc__is_control_keyword_range(s + name_start, name_end - name_start)) return 0;

    prefix_start = cc__last_stmt_terminator_before(s, name_start);
    while (prefix_start < name_start && isspace((unsigned char)s[prefix_start])) prefix_start++;
    prefix_end = name_start;
    while (prefix_end > prefix_start && isspace((unsigned char)s[prefix_end - 1])) prefix_end--;
    if (prefix_end <= prefix_start) return 0;
    if (out_is_async) *out_is_async = cc__decl_has_async_attr(s, prefix_start, prefix_end);

    keep_start = cc__skip_decl_noise(s, prefix_start, prefix_end);
    for (;;) {
        size_t tok_start = keep_start, tok_end;
        while (tok_start < prefix_end && isspace((unsigned char)s[tok_start])) tok_start++;
        tok_end = tok_start;
        while (tok_end < prefix_end && cc__is_ident_char(s[tok_end])) tok_end++;
        if (tok_end == tok_start || !cc__is_storage_keyword_range(s + tok_start, tok_end - tok_start)) break;
        keep_start = tok_end;
        keep_start = cc__skip_decl_noise(s, keep_start, prefix_end);
    }
    while (keep_start < prefix_end && isspace((unsigned char)s[keep_start])) keep_start++;
    if (prefix_end <= keep_start) return 0;

    {
        size_t type_len = prefix_end - keep_start;
        if (type_len >= out_type_sz) type_len = out_type_sz - 1;
        memcpy(out_type, s + keep_start, type_len);
        out_type[type_len] = 0;
    }
    if (out_is_void && strcmp(out_type, "void") == 0) *out_is_void = 1;
    return 1;
}

/* Walk forward from `open_paren + 1` to find the matching `)`, treating
 * comments/strings/chars as inert.  Returns 1 with `*out_close` set to
 * the position of the matching `)`; 0 on unbalanced or trailing junk
 * (any non-whitespace byte after the matching `)`). */
static int cc__match_ctor_close_paren(const char* expr, size_t end,
                                      size_t open_paren, size_t* out_close) {
    int par = 1;
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    scan.at_line_start = 0;  /* mid-expression slice */
    size_t close_paren = open_paren + 1;
    while (close_paren < end) {
        if (cc_inert_scan_step(&scan, expr, end, &close_paren)) continue;
        char ch = expr[close_paren];
        if (ch == '(') par++;
        else if (ch == ')') {
            par--;
            if (par == 0) break;
        }
        close_paren++;
    }
    if (close_paren >= end || par != 0) return 0;
    for (size_t i = close_paren + 1; i < end; i++) {
        if (!isspace((unsigned char)expr[i])) return 0;
    }
    *out_close = close_paren;
    return 1;
}

static int cc__match_result_ctor_prefix_arg(const char* expr, size_t expr_len, const char* prefix,
                                            size_t* out_arg_start, size_t* out_arg_end) {
    size_t start = 0, end = expr_len, name_end;
    size_t open_paren, close_paren;
    size_t prefix_len;

    if (!expr || !prefix) return 0;
    prefix_len = strlen(prefix);
    while (start < end && isspace((unsigned char)expr[start])) start++;
    while (end > start && isspace((unsigned char)expr[end - 1])) end--;
    if (end <= start + prefix_len) return 0;
    if (strncmp(expr + start, prefix, prefix_len) != 0) return 0;

    name_end = start + prefix_len;
    while (name_end < end && cc__is_ident_char(expr[name_end])) name_end++;
    if (name_end == start + prefix_len) return 0;

    open_paren = name_end;
    while (open_paren < end && isspace((unsigned char)expr[open_paren])) open_paren++;
    if (open_paren >= end || expr[open_paren] != '(') return 0;

    if (!cc__match_ctor_close_paren(expr, end, open_paren, &close_paren)) return 0;
    if (out_arg_start) *out_arg_start = open_paren + 1;
    if (out_arg_end) *out_arg_end = close_paren;
    return 1;
}

static int cc__match_result_ctor_name_arg(const char* expr, size_t expr_len, const char* name,
                                          size_t* out_arg_start, size_t* out_arg_end) {
    size_t start = 0, end = expr_len, name_len;
    size_t open_paren, close_paren;

    if (!expr || !name) return 0;
    name_len = strlen(name);
    while (start < end && isspace((unsigned char)expr[start])) start++;
    while (end > start && isspace((unsigned char)expr[end - 1])) end--;
    if (end <= start + name_len) return 0;
    if (strncmp(expr + start, name, name_len) != 0) return 0;
    if (start + name_len < end && cc__is_ident_char(expr[start + name_len])) return 0;

    open_paren = start + name_len;
    while (open_paren < end && isspace((unsigned char)expr[open_paren])) open_paren++;
    if (open_paren >= end || expr[open_paren] != '(') return 0;

    if (!cc__match_ctor_close_paren(expr, end, open_paren, &close_paren)) return 0;
    if (out_arg_start) *out_arg_start = open_paren + 1;
    if (out_arg_end) *out_arg_end = close_paren;
    return 1;
}

static int cc__token_is(const char* s, size_t len, size_t i, const char* tok) {
    size_t tn = strlen(tok);
    if (i + tn > len) return 0;
    if (memcmp(s + i, tok, tn) != 0) return 0;
    if (i > 0 && cc__is_ident_char(s[i - 1])) return 0;
    if (i + tn < len && cc__is_ident_char(s[i + tn])) return 0;
    return 1;
}

static int cc__is_if_controlled_return(const char* s, size_t len, size_t ret_i) {
    (void)len;
    if (!s || ret_i == 0) return 0;
    /* Heuristic: detect `if (...) return ...;` without braces by looking backward for a ')'
       immediately before the `return` token (ignoring whitespace), and checking for `if`
       before the matching '('. */
    size_t j = ret_i;
    while (j > 0 && (s[j - 1] == ' ' || s[j - 1] == '\t' || s[j - 1] == '\r' || s[j - 1] == '\n')) j--;
    if (j == 0 || s[j - 1] != ')') return 0;

    int par = 0;
    size_t k = j - 1;
    while (k > 0) {
        char ch = s[k - 1];
        if (ch == ')') par++;
        else if (ch == '(') {
            if (par == 0) break;
            par--;
        }
        k--;
    }
    if (k == 0) return 0;

    size_t t = k - 1;
    while (t > 0 && (s[t - 1] == ' ' || s[t - 1] == '\t' || s[t - 1] == '\r' || s[t - 1] == '\n')) t--;
    if (t < 2) return 0;
    if (s[t - 2] != 'i' || s[t - 1] != 'f') return 0;
    if (t > 2 && cc__is_ident_char(s[t - 3])) return 0; /* word boundary */
    return 1;
}

/* True when `stmt_i` is the statement of `if (...)<stmt>` (no braces), so a
 * multi-statement defer rewrite must wrap the injection in `{ ... }`. */
static int cc__is_if_controlled_stmt(const char* s, size_t len, size_t stmt_i) {
    (void)len;
    if (!s || stmt_i == 0) return 0;
    size_t j = stmt_i;
    while (j > 0 && (s[j - 1] == ' ' || s[j - 1] == '\t' || s[j - 1] == '\r' || s[j - 1] == '\n')) j--;
    if (j == 0 || s[j - 1] != ')') return 0;

    int par = 0;
    size_t k = j - 1;
    while (k > 0) {
        char ch = s[k - 1];
        if (ch == ')') par++;
        else if (ch == '(') {
            if (par == 0) break;
            par--;
        }
        k--;
    }
    if (k == 0) return 0;

    size_t t = k - 1;
    while (t > 0 && (s[t - 1] == ' ' || s[t - 1] == '\t' || s[t - 1] == '\r' || s[t - 1] == '\n')) t--;
    if (t < 2) return 0;
    if (s[t - 2] != 'i' || s[t - 1] != 'f') return 0;
    if (t > 2 && cc__is_ident_char(s[t - 3])) return 0;
    return 1;
}

static int cc__scan_stmt_end_semicolon(const char* s, size_t len, size_t i, size_t* out_end_off) {
    int par = 0, brk = 0, br = 0;
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    scan.at_line_start = 0;  /* mid-buffer probe */
    while (i < len) {
        if (cc_inert_scan_step(&scan, s, len, &i)) continue;
        char ch = s[i];
        if (ch == '(') par++;
        else if (ch == ')') { if (par) par--; }
        else if (ch == '[') brk++;
        else if (ch == ']') { if (brk) brk--; }
        else if (ch == '{') br++;
        else if (ch == '}') { if (br) br--; }
        else if (ch == ';' && par == 0 && brk == 0 && br == 0) {
            if (out_end_off) *out_end_off = i + 1;
            return 1;
        }
        i++;
    }
    return 0;
}

static int cc__find_loop_body_open_after_control(const char* s, size_t len, size_t kw_i,
                                                 size_t* out_open_i) {
    size_t j, close_paren, body_i;
    if (!s || !out_open_i) return 0;
    j = kw_i;
    while (j < len && cc__is_ident_char(s[j])) j++;
    j = cc_skip_ws_and_comments(s, len, j);
    if (j >= len || s[j] != '(') return 0;
    if (!cc_find_matching_paren(s, len, j, &close_paren)) return 0;
    body_i = cc_skip_ws_and_comments(s, len, close_paren + 1);
    if (body_i < len && s[body_i] == '{') {
        *out_open_i = body_i;
        return 1;
    }
    return 0;
}

/* `do { ... } while (...);` — body `{` follows the keyword directly. */
static int cc__find_do_body_open(const char* s, size_t len, size_t kw_i, size_t* out_open_i) {
    size_t j, body_i;
    if (!s || !out_open_i) return 0;
    j = kw_i;
    while (j < len && cc__is_ident_char(s[j])) j++;
    body_i = cc_skip_ws_and_comments(s, len, j);
    if (body_i < len && s[body_i] == '{') {
        *out_open_i = body_i;
        return 1;
    }
    return 0;
}

static int cc__current_line_has_non_ws(const char* out, size_t out_len) {
    size_t k;
    if (!out) return 0;
    k = out_len;
    while (k > 0 && out[k - 1] != '\n') k--;
    for (size_t q = k; q < out_len; q++) {
        if (out[q] != ' ' && out[q] != '\t' && out[q] != '\r') return 1;
    }
    return 0;
}

static void cc__emit_always_defers_for_depth_range(char** out, size_t* out_len, size_t* out_cap,
                                                   const char* in_src, size_t stmt_i,
                                                   CCDeferStmt** defers, int* defer_counts,
                                                   int from_depth, int to_depth) {
    if (!out || !out_len || !out_cap || !defers || !defer_counts) return;
    if (from_depth < to_depth) return;
    int mid_line = cc__current_line_has_non_ws(*out, *out_len);
    if (mid_line) {
        for (int d = from_depth; d >= to_depth; d--) {
            int dd = (d < 0) ? 0 : (d >= 256 ? 255 : d);
            for (int k = defer_counts[dd] - 1; k >= 0; k--) {
                if (defers[dd][k].cond == DEFER_ALWAYS) {
                    cc__append_str(out, out_len, out_cap, defers[dd][k].stmt);
                    cc__append_str(out, out_len, out_cap, " ");
                }
            }
        }
    } else {
        size_t indent = cc__source_line_indent_len(in_src, stmt_i);
        for (int d = from_depth; d >= to_depth; d--) {
            int dd = (d < 0) ? 0 : (d >= 256 ? 255 : d);
            for (int k = defer_counts[dd] - 1; k >= 0; k--) {
                if (defers[dd][k].cond == DEFER_ALWAYS) {
                    cc__append_missing_indent_to(out, out_len, out_cap, indent);
                    cc__append_str(out, out_len, out_cap, defers[dd][k].stmt);
                    cc__append_n(out, out_len, out_cap, "\n", 1);
                }
            }
        }
        cc__append_missing_indent_to(out, out_len, out_cap, indent);
    }
}

static int cc__rewrite_defer_syntax_impl(const CCVisitorCtx* ctx,
                            const char* in_src,
                            size_t in_len,
                            char** out_src,
                            size_t* out_len,
                            int line_marks) {
    size_t* last_term_pref = NULL;
    if (!ctx || !in_src || !out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;

    /* Defer stacks by brace depth. */
    CCDeferStmt* defers[256];
    int defer_counts[256];
    int defer_caps[256];
    int return_just_emitted[256];
    for (int d = 0; d < 256; d++) {
        defers[d] = NULL;
        defer_counts[d] = 0;
        defer_caps[d] = 0;
        return_just_emitted[d] = 0;
    }

    char* out = NULL;
    size_t outl = 0, outc = 0;

    int depth = 0;
    int line_no = 1;
    last_term_pref = cc__build_last_stmt_terminator_prefix(in_src, in_len);
    g_defer_last_term = last_term_pref;
    g_defer_last_term_len = last_term_pref ? in_len : 0;
    /* Inert regions (comments / strings / char literals / preprocessor
     * directive bodies) are consumed by the shared CCInertScan.  The pp
     * coverage matters: without it, a benign
     * `#define UNUSED_DEFER(stmt) @defer stmt` would be mis-scanned, the
     * `@defer` keyword inside the macro body would be treated as a real
     * statement, and the user would see a confusing "malformed @defer
     * statement" error on the #define line.  See
     * `tests/inert_pp_directive_tokens_smoke.ccs` for the regression
     * guard. */
    CCInertScan sc;

    int changed = 0;
    CCDeferFunctionScope fn_scope;
    memset(&fn_scope, 0, sizeof(fn_scope));
    size_t pending_loop_body_open = (size_t)-1;
    int loop_body_depths[256];
    int loop_body_count = 0;
    /* Innermost switch/loop body depths for `break` (ordered by nesting). */
    size_t pending_breakable_body_open = (size_t)-1;
    int breakable_body_depths[256];
    int breakable_body_count = 0;

    cc_inert_scan_init(&sc, NULL);

    for (size_t i = 0; i < in_len;) {
        size_t span_a = i;
        int was_lit = sc.in_str || sc.in_chr;
        if (cc_inert_scan_step(&sc, in_src, in_len, &i)) {
            /* Inert content: copy through verbatim.  A `\` + newline
             * escape pair inside a string/char literal counts as escape
             * bytes, not a line break, for line_no. */
            for (size_t x = span_a; x < i; x++) {
                if (in_src[x] == '\n' &&
                    !(was_lit && x == span_a + 1 && in_src[span_a] == '\\'))
                    line_no++;
                cc__append_n(&out, &outl, &outc, &in_src[x], 1);
            }
            continue;
        }

        char ch = in_src[i];

        if (ch == '\n') line_no++;

        /* `cancel ...;` is reserved for named-defer cancellation and not
         * implemented yet: hard error. Do NOT misclassify a method call
         * named `cancel` (e.g. `chan.cancel()` / `obj->cancel()`) — that's
         * an ordinary user call and must pass through untouched. */
        if (cc__token_is(in_src, in_len, i, "cancel") &&
            cc__not_member_access_prefix(in_src, i)) {
            char rel[1024];
            const char* f = cc_path_rel_to_repo(ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
            cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                              "'cancel' is not implemented in defer lowering (use structured scopes instead)");
            for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);
            free(out);
            g_defer_last_term = NULL;
            g_defer_last_term_len = 0;
            free(last_term_pref);
            return -1;
        }

        if (cc__token_is(in_src, in_len, i, "for") ||
            cc__token_is(in_src, in_len, i, "while")) {
            size_t loop_open = 0;
            if (cc__find_loop_body_open_after_control(in_src, in_len, i, &loop_open)) {
                pending_loop_body_open = loop_open;
                pending_breakable_body_open = loop_open;
            }
        } else if (cc__token_is(in_src, in_len, i, "do")) {
            size_t do_open = 0;
            if (cc__find_do_body_open(in_src, in_len, i, &do_open)) {
                pending_loop_body_open = do_open;
                pending_breakable_body_open = do_open;
            }
        } else if (cc__token_is(in_src, in_len, i, "switch")) {
            size_t switch_open = 0;
            if (cc__find_loop_body_open_after_control(in_src, in_len, i, &switch_open)) {
                pending_breakable_body_open = switch_open;
            }
        }

        /* `return ...;` should execute all active defers (current scope and outers). */
        if (cc__token_is(in_src, in_len, i, "return")) {
            size_t stmt_end = 0;
            if (!cc__scan_stmt_end_semicolon(in_src, in_len, i, &stmt_end)) {
                char rel[1024];
                const char* f = cc_path_rel_to_repo(ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                                  "malformed 'return' while lowering @defer (expected ';')");
                for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);
                free(out);
                g_defer_last_term = NULL;
                g_defer_last_term_len = 0;
                free(last_term_pref);
                return -1;
            }
            int has_defers = 0;
            for (int d = depth; d >= 0; d--) {
                int dd = d;
                if (dd < 0) dd = 0;
                if (dd >= 256) dd = 255;
                if (defer_counts[dd] > 0) { has_defers = 1; break; }
            }

            if (!has_defers) {
                /* No active defers: keep return as-is. */
                cc__append_n(&out, &outl, &outc, in_src + i, stmt_end - i);
                /* Skip original text (keep line_no tracking correct via the main loop's '\n' handler). */
                for (size_t k = i; k < stmt_end; k++) {
                    if (in_src[k] == '\n') line_no++;
                }
                i = stmt_end;
                continue;
            }

            int is_if_ctl = cc__is_if_controlled_return(in_src, in_len, i);
            
            /* Check if we have any conditional defers */
            int has_conditional = 0;
            for (int d = depth; d >= 0 && !has_conditional; d--) {
                int dd = (d < 0) ? 0 : (d >= 256 ? 255 : d);
                for (int k = 0; k < defer_counts[dd]; k++) {
                    if (defers[dd][k].cond != DEFER_ALWAYS) {
                        has_conditional = 1;
                        break;
                    }
                }
            }
            
            /* Extract return expression (between 'return' and ';') */
            size_t ret_kw_end = i + 6; /* strlen("return") */
            while (ret_kw_end < stmt_end && (in_src[ret_kw_end] == ' ' || in_src[ret_kw_end] == '\t')) ret_kw_end++;
            size_t expr_start = ret_kw_end;
            size_t expr_end = stmt_end - 1; /* exclude ';' */
            while (expr_end > expr_start && (in_src[expr_end - 1] == ' ' || in_src[expr_end - 1] == '\t' || in_src[expr_end - 1] == '\n')) expr_end--;
            int has_expr = (expr_end > expr_start);

            /* User line (and governing path) of this `return`, from the
             * buffer's #line/CC_LN ledger — used to stamp a resync marker
             * ahead of the cleanup blob so its lowered form maps to the
             * return's true source line rather than to whatever physical
             * offset the accumulated expansion above it reached. */
            const char* ret_lp = NULL;
            size_t ret_lpl = 0;
            int ret_user_line = cc_user_line_for_offset(in_src, in_len, i, 1, &ret_lp, &ret_lpl);

            if (is_if_ctl) {
                cc__append_str(&out, &outl, &outc, "{\n");
            }
            
            {
                int use_fn_cleanup = fn_scope.active &&
                                     fn_scope.has_top_level_defers &&
                                     !fn_scope.has_top_level_conditional &&
                                     depth >= 1 &&
                                     defer_counts[1] > 0;
                int nested_has_conditional = 0;
                for (int d = depth; d >= 2 && !nested_has_conditional; d--) {
                    int dd = (d >= 256 ? 255 : d);
                    for (int k = 0; k < defer_counts[dd]; k++) {
                        if (defers[dd][k].cond != DEFER_ALWAYS) {
                            nested_has_conditional = 1;
                            break;
                        }
                    }
                }

                if (use_fn_cleanup) {
                    size_t synth_indent = cc__suggest_statement_indent(out, outl);
                    cc__emit_ret_line_marker(&out, &outl, &outc, synth_indent,
                                             ret_user_line, ret_lp, ret_lpl);
                    if (has_expr && nested_has_conditional) {
                        cc__append_str(&out, &outl, &outc, "{ __typeof__(");
                        cc__append_n(&out, &outl, &outc, in_src + expr_start, expr_end - expr_start);
                        cc__append_str(&out, &outl, &outc, ") __cc_ret = (");
                        cc__append_n(&out, &outl, &outc, in_src + expr_start, expr_end - expr_start);
                        cc__append_str(&out, &outl, &outc, "); int __cc_ret_err = !__cc_ret.ok;\n");
                        for (int d = depth; d >= 2; d--) {
                            int dd = (d >= 256 ? 255 : d);
                            for (int k = defer_counts[dd] - 1; k >= 0; k--) {
                                if (defers[dd][k].cond == DEFER_ALWAYS) {
                                    cc__append_str(&out, &outl, &outc, defers[dd][k].stmt);
                                    cc__append_n(&out, &outl, &outc, "\n", 1);
                                } else if (defers[dd][k].cond == DEFER_ON_ERR) {
                                    cc__append_str(&out, &outl, &outc, "if (__cc_ret_err) { ");
                                    cc__append_str(&out, &outl, &outc, defers[dd][k].stmt);
                                    cc__append_str(&out, &outl, &outc, " }\n");
                                } else if (defers[dd][k].cond == DEFER_ON_OK) {
                                    cc__append_str(&out, &outl, &outc, "if (!__cc_ret_err) { ");
                                    cc__append_str(&out, &outl, &outc, defers[dd][k].stmt);
                                    cc__append_str(&out, &outl, &outc, " }\n");
                                }
                            }
                        }
                        cc__append_missing_indent_to(&out, &outl, &outc, synth_indent);
                        /* Local temp is named `__cc_ret`; never emit the
                         * `__cc_ret(id, ...)` macro here (name clash + stub risk). */
                        cc_sb_append_fmt(&out, &outl, &outc, "__cc_retval_%d = __cc_ret;\n", fn_scope.cleanup_label_id);
                        cc__append_missing_indent_to(&out, &outl, &outc, synth_indent);
                        cc_sb_append_fmt(&out, &outl, &outc, "__cc_ret_set_%d = 1;\n", fn_scope.cleanup_label_id);
                        cc__append_missing_indent_to(&out, &outl, &outc, synth_indent);
                        cc_sb_append_fmt(&out, &outl, &outc, "goto __cc_cleanup_%d;\n}", fn_scope.cleanup_label_id);
                    } else {
                        size_t ok_arg_start = 0, ok_arg_end = 0;
                        size_t err_arg_start = 0, err_arg_end = 0;
                        int use_ret_ok = 0;
                        int use_ret_err = 0;
                        int use_ret_ok_short = 0;
                        int use_ret_err_short = 0;
                        for (int d = depth; d >= 2; d--) {
                            int dd = (d >= 256 ? 255 : d);
                            for (int k = defer_counts[dd] - 1; k >= 0; k--) {
                                cc__append_str(&out, &outl, &outc, defers[dd][k].stmt);
                                cc__append_n(&out, &outl, &outc, "\n", 1);
                            }
                        }
                        if (has_expr) {
                            use_ret_ok = cc__match_result_ctor_prefix_arg(in_src + expr_start, expr_end - expr_start,
                                                                          "cc_ok_", &ok_arg_start, &ok_arg_end);
                            if (!use_ret_ok) {
                                use_ret_ok_short = cc__match_result_ctor_name_arg(in_src + expr_start, expr_end - expr_start,
                                                                                  "cc_ok", &ok_arg_start, &ok_arg_end);
                                use_ret_ok = use_ret_ok_short;
                            }
                            if (!use_ret_ok) {
                                use_ret_err = cc__match_result_ctor_prefix_arg(in_src + expr_start, expr_end - expr_start,
                                                                               "cc_err_", &err_arg_start, &err_arg_end);
                                if (!use_ret_err) {
                                    use_ret_err_short = cc__match_result_ctor_name_arg(in_src + expr_start, expr_end - expr_start,
                                                                                       "cc_err", &err_arg_start, &err_arg_end);
                                    use_ret_err = use_ret_err_short;
                                }
                            }
                            cc__append_missing_indent_to(&out, &outl, &outc, synth_indent);
                            if (use_ret_ok) {
                                cc__emit_ret_ok_cleanup_begin(&out, &outl, &outc, fn_scope.cleanup_label_id);
                                cc__append_n(&out, &outl, &outc,
                                             in_src + expr_start + ok_arg_start,
                                             ok_arg_end - ok_arg_start);
                                cc__emit_ret_cleanup_end(&out, &outl, &outc,
                                                         fn_scope.cleanup_label_id, synth_indent, "));");
                            } else if (use_ret_err) {
                                size_t err_trim_start = err_arg_start;
                                size_t err_trim_end = err_arg_end;
                                while (err_trim_start < err_trim_end &&
                                       isspace((unsigned char)in_src[expr_start + err_trim_start])) err_trim_start++;
                                while (err_trim_end > err_trim_start &&
                                       isspace((unsigned char)in_src[expr_start + err_trim_end - 1])) err_trim_end--;
                                if (use_ret_err_short &&
                                    err_trim_end > err_trim_start + 6 &&
                                    memcmp(in_src + expr_start + err_trim_start, "CC_IO_", 6) == 0) {
                                    cc__emit_ret_err_cleanup_begin(&out, &outl, &outc, fn_scope.cleanup_label_id);
                                    cc__append_str(&out, &outl, &outc, "cc_io_error(");
                                    cc__append_n(&out, &outl, &outc,
                                                 in_src + expr_start + err_arg_start,
                                                 err_arg_end - err_arg_start);
                                    cc__append_str(&out, &outl, &outc, ")");
                                    cc__emit_ret_cleanup_end(&out, &outl, &outc,
                                                             fn_scope.cleanup_label_id, synth_indent, "));");
                                } else if (use_ret_err_short) {
                                    cc__emit_ret_cleanup_begin(&out, &outl, &outc, fn_scope.cleanup_label_id);
                                    cc__append_n(&out, &outl, &outc, in_src + expr_start, expr_end - expr_start);
                                    cc__emit_ret_cleanup_end(&out, &outl, &outc,
                                                             fn_scope.cleanup_label_id, synth_indent, ");");
                                } else {
                                    cc__emit_ret_err_cleanup_begin(&out, &outl, &outc, fn_scope.cleanup_label_id);
                                    cc__append_n(&out, &outl, &outc,
                                                 in_src + expr_start + err_arg_start,
                                                 err_arg_end - err_arg_start);
                                    cc__emit_ret_cleanup_end(&out, &outl, &outc,
                                                             fn_scope.cleanup_label_id, synth_indent, "));");
                                }
                            } else {
                                cc__emit_ret_cleanup_begin(&out, &outl, &outc, fn_scope.cleanup_label_id);
                                cc__append_n(&out, &outl, &outc, in_src + expr_start, expr_end - expr_start);
                                cc__emit_ret_cleanup_end(&out, &outl, &outc,
                                                         fn_scope.cleanup_label_id, synth_indent, ");");
                            }
                        } else {
                            cc__append_missing_indent_to(&out, &outl, &outc, synth_indent);
                            cc_sb_append_fmt(&out, &outl, &outc, "goto __cc_cleanup_%d;", fn_scope.cleanup_label_id);
                        }
                    }
                    fn_scope.used_goto = 1;
                } else if (has_conditional && has_expr) {
                    /* Emit: { __typeof__(expr) __cc_ret = (expr); bool __cc_ret_err = !__cc_ret.ok; ... } */
                    cc__append_str(&out, &outl, &outc, "{ __typeof__(");
                    cc__append_n(&out, &outl, &outc, in_src + expr_start, expr_end - expr_start);
                    cc__append_str(&out, &outl, &outc, ") __cc_ret = (");
                    cc__append_n(&out, &outl, &outc, in_src + expr_start, expr_end - expr_start);
                    cc__append_str(&out, &outl, &outc, "); int __cc_ret_err = !__cc_ret.ok;\n");
                    for (int d = depth; d >= 0; d--) {
                        int dd = (d < 0) ? 0 : (d >= 256 ? 255 : d);
                        for (int k = defer_counts[dd] - 1; k >= 0; k--) {
                            if (defers[dd][k].cond == DEFER_ALWAYS) {
                                cc__append_str(&out, &outl, &outc, defers[dd][k].stmt);
                                cc__append_n(&out, &outl, &outc, "\n", 1);
                            } else if (defers[dd][k].cond == DEFER_ON_ERR) {
                                cc__append_str(&out, &outl, &outc, "if (__cc_ret_err) { ");
                                cc__append_str(&out, &outl, &outc, defers[dd][k].stmt);
                                cc__append_str(&out, &outl, &outc, " }\n");
                            } else if (defers[dd][k].cond == DEFER_ON_OK) {
                                cc__append_str(&out, &outl, &outc, "if (!__cc_ret_err) { ");
                                cc__append_str(&out, &outl, &outc, defers[dd][k].stmt);
                                cc__append_str(&out, &outl, &outc, " }\n");
                            }
                        }
                    }
                    cc__append_str(&out, &outl, &outc, "return __cc_ret;\n}");
                } else {
                    /* Hand-crafted-C indent for the plain-return defer
                     * flush.  Pre-fix, this branch unconditionally
                     * appended `\n` after each defer body and dropped
                     * `return X;` at column 0 — readable as a machine
                     * dump, not as code.
                     *
                     * Two cases:
                     *
                     *   (a) The user's `return` is on its OWN line
                     *       (typical block style).  Output's current
                     *       line is whitespace-only when we arrive
                     *       here (we already emitted the user's
                     *       leading indent bytewise).  Emit each
                     *       defer body on its own line at the
                     *       source-line indent of `i`, then re-emit
                     *       the indent before `return X;`.  Result
                     *       reads like a hand-aligned cleanup block.
                     *
                     *   (b) The user's `return` is INLINE with prior
                     *       statements (`if (...) { ...; return 1; }`
                     *       on one source line).  Output's current
                     *       line has non-whitespace content.  Emit
                     *       defer bodies inline, separated by a
                     *       single space, no newlines — the
                     *       following `return X;` then flows on the
                     *       same line, mirroring the user's compact
                     *       layout. */
                    int mid_line = 0;
                    {
                        size_t k = outl;
                        while (k > 0 && out[k - 1] != '\n') k--;
                        for (size_t q = k; q < outl; q++) {
                            if (out[q] != ' ' && out[q] != '\t' && out[q] != '\r') {
                                mid_line = 1;
                                break;
                            }
                        }
                    }
                    if (mid_line) {
                        for (int d = depth; d >= 0; d--) {
                            int dd = (d < 0) ? 0 : (d >= 256 ? 255 : d);
                            for (int k = defer_counts[dd] - 1; k >= 0; k--) {
                                cc__append_str(&out, &outl, &outc, defers[dd][k].stmt);
                                cc__append_str(&out, &outl, &outc, " ");
                            }
                        }
                    } else {
                        size_t indent = cc__source_line_indent_len(in_src, i);
                        for (int d = depth; d >= 0; d--) {
                            int dd = (d < 0) ? 0 : (d >= 256 ? 255 : d);
                            for (int k = defer_counts[dd] - 1; k >= 0; k--) {
                                cc__append_missing_indent_to(&out, &outl, &outc, indent);
                                cc__append_str(&out, &outl, &outc, defers[dd][k].stmt);
                                cc__append_n(&out, &outl, &outc, "\n", 1);
                            }
                        }
                        cc__append_missing_indent_to(&out, &outl, &outc, indent);
                    }
                    cc__append_n(&out, &outl, &outc, in_src + i, stmt_end - i);
                }
            }
            
            if (is_if_ctl) {
                cc__append_str(&out, &outl, &outc, "\n");
                cc__append_missing_indent_to(&out, &outl, &outc, cc__source_line_indent_len(in_src, i));
                cc__append_str(&out, &outl, &outc, "}");
            } else {
                int dd = (depth < 0) ? 0 : (depth >= 256 ? 255 : depth);
                return_just_emitted[dd] = 1;
            }
            changed = 1;

            /* Skip original text (keep line_no tracking correct via the main loop's '\n' handler). */
            for (size_t k = i; k < stmt_end; k++) {
                if (in_src[k] == '\n') line_no++;
            }
            i = stmt_end;
            continue;
        }

        /* `continue;` exits scopes nested inside the nearest loop body before
         * jumping to the loop's next iteration.  Without this rewrite, defers
         * injected only at `}` are skipped by both plain C continue and the
         * async state-machine's lowered continue edge. */
        if (cc__token_is(in_src, in_len, i, "continue") && loop_body_count > 0) {
            size_t stmt_end = 0;
            if (!cc__scan_stmt_end_semicolon(in_src, in_len, i, &stmt_end)) {
                char rel[1024];
                const char* f = cc_path_rel_to_repo(ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                                  "malformed 'continue' while lowering @defer (expected ';')");
                for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);
                free(out);
                g_defer_last_term = NULL;
                g_defer_last_term_len = 0;
                free(last_term_pref);
                return -1;
            }

            int loop_body_depth = loop_body_depths[loop_body_count - 1];
            int has_defers = 0;
            int is_if_ctl = cc__is_if_controlled_stmt(in_src, in_len, i);
            for (int d = depth; d >= loop_body_depth; d--) {
                int dd = (d < 0) ? 0 : (d >= 256 ? 255 : d);
                for (int k = 0; k < defer_counts[dd]; k++) {
                    if (defers[dd][k].cond == DEFER_ALWAYS) {
                        has_defers = 1;
                        break;
                    }
                }
                if (has_defers) break;
            }

            if (has_defers) {
                if (is_if_ctl) {
                    cc__append_str(&out, &outl, &outc, "{\n");
                }
                cc__emit_always_defers_for_depth_range(&out, &outl, &outc, in_src, i,
                                                       defers, defer_counts,
                                                       depth, loop_body_depth);
                changed = 1;
            }
            cc__append_n(&out, &outl, &outc, in_src + i, stmt_end - i);
            if (has_defers && is_if_ctl) {
                cc__append_str(&out, &outl, &outc, "\n");
                cc__append_missing_indent_to(&out, &outl, &outc, cc__source_line_indent_len(in_src, i));
                cc__append_str(&out, &outl, &outc, "}");
            }

            for (size_t k = i; k < stmt_end; k++) {
                if (in_src[k] == '\n') line_no++;
            }
            i = stmt_end;
            continue;
        }

        /* `break;` exits scopes nested inside the nearest switch/loop body
         * before jumping past that construct.  Without this rewrite, defers
         * injected only at `}` are skipped (e.g. `@defer` in a switch case
         * followed by `break`). */
        if (cc__token_is(in_src, in_len, i, "break") && breakable_body_count > 0) {
            size_t stmt_end = 0;
            if (!cc__scan_stmt_end_semicolon(in_src, in_len, i, &stmt_end)) {
                char rel[1024];
                const char* f = cc_path_rel_to_repo(ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                                  "malformed 'break' while lowering @defer (expected ';')");
                for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);
                free(out);
                g_defer_last_term = NULL;
                g_defer_last_term_len = 0;
                free(last_term_pref);
                return -1;
            }

            int break_body_depth = breakable_body_depths[breakable_body_count - 1];
            int has_defers = 0;
            int is_if_ctl = cc__is_if_controlled_stmt(in_src, in_len, i);
            for (int d = depth; d >= break_body_depth; d--) {
                int dd = (d < 0) ? 0 : (d >= 256 ? 255 : d);
                for (int k = 0; k < defer_counts[dd]; k++) {
                    if (defers[dd][k].cond == DEFER_ALWAYS) {
                        has_defers = 1;
                        break;
                    }
                }
                if (has_defers) break;
            }

            if (has_defers) {
                if (is_if_ctl) {
                    cc__append_str(&out, &outl, &outc, "{\n");
                }
                cc__emit_always_defers_for_depth_range(&out, &outl, &outc, in_src, i,
                                                       defers, defer_counts,
                                                       depth, break_body_depth);
                changed = 1;
            }
            cc__append_n(&out, &outl, &outc, in_src + i, stmt_end - i);
            if (has_defers && is_if_ctl) {
                cc__append_str(&out, &outl, &outc, "\n");
                cc__append_missing_indent_to(&out, &outl, &outc, cc__source_line_indent_len(in_src, i));
                cc__append_str(&out, &outl, &outc, "}");
            }

            for (size_t k = i; k < stmt_end; k++) {
                if (in_src[k] == '\n') line_no++;
            }
            i = stmt_end;
            continue;
        }

        /* `@defer ...;` or `@defer(err) ...;` or `@defer(ok) ...;` */
        if (cc__token_is(in_src, in_len, i, "@defer")) {
            int defer_line = line_no;
            int defer_depth = depth;
            CCDeferCondition cond = DEFER_ALWAYS;

            size_t j = i + 6;
            while (j < in_len && (in_src[j] == ' ' || in_src[j] == '\t')) j++;

            /* Check for (err) or (ok) condition */
            if (j < in_len && in_src[j] == '(') {
                size_t paren_start = j;
                j++;
                while (j < in_len && (in_src[j] == ' ' || in_src[j] == '\t')) j++;
                if (j + 3 <= in_len && strncmp(in_src + j, "err", 3) == 0 && 
                    (j + 3 >= in_len || !cc__is_ident_char(in_src[j + 3]))) {
                    cond = DEFER_ON_ERR;
                    j += 3;
                } else if (j + 2 <= in_len && strncmp(in_src + j, "ok", 2) == 0 &&
                           (j + 2 >= in_len || !cc__is_ident_char(in_src[j + 2]))) {
                    cond = DEFER_ON_OK;
                    j += 2;
                }
                while (j < in_len && (in_src[j] == ' ' || in_src[j] == '\t')) j++;
                if (j < in_len && in_src[j] == ')') {
                    j++;
                } else {
                    /* Not a valid condition, reset */
                    j = paren_start;
                    cond = DEFER_ALWAYS;
                }
                while (j < in_len && (in_src[j] == ' ' || in_src[j] == '\t')) j++;
            }

            /* Optional name: identifier ':' */
            size_t name_start = j;
            if (j < in_len && cc__is_ident_start(in_src[j])) {
                j++;
                while (j < in_len && cc__is_ident_char(in_src[j])) j++;
                size_t save = j;
                while (j < in_len && (in_src[j] == ' ' || in_src[j] == '\t')) j++;
                if (j < in_len && in_src[j] == ':') {
                    j++;
                    while (j < in_len && (in_src[j] == ' ' || in_src[j] == '\t')) j++;
                } else {
                    /* not a name: rewind */
                    (void)name_start;
                    j = name_start;
                }
                (void)save;
            }

            size_t stmt_start = j;
            size_t stmt_end = 0;
            if (!cc__scan_stmt_end_semicolon(in_src, in_len, stmt_start, &stmt_end)) {
                char rel[1024];
                const char* f = cc_path_rel_to_repo(ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
                cc_pass_error_cat(f, defer_line, 1, CC_ERR_SYNTAX,
                                  "malformed '@defer' statement (expected ';' after deferred action)");
                for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);
                free(out);
                g_defer_last_term = NULL;
                g_defer_last_term_len = 0;
                free(last_term_pref);
                return -1;
            }

            size_t stmt_len = stmt_end - stmt_start;
            char* stmt = (char*)malloc(stmt_len + 1);
            if (!stmt) {
                for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);
                free(out);
                g_defer_last_term = NULL;
                g_defer_last_term_len = 0;
                free(last_term_pref);
                return -1;
            }
            memcpy(stmt, in_src + stmt_start, stmt_len);
            stmt[stmt_len] = 0;
            stmt = cc__normalize_defer_stmt(stmt);
            if (!stmt) {
                for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);
                free(out);
                g_defer_last_term = NULL;
                g_defer_last_term_len = 0;
                free(last_term_pref);
                return -1;
            }

            if (defer_depth < 0) defer_depth = 0;
            if (defer_depth >= 256) defer_depth = 255;

            if (defer_counts[defer_depth] + 1 > defer_caps[defer_depth]) {
                int nc = defer_caps[defer_depth] ? defer_caps[defer_depth] * 2 : 8;
                CCDeferStmt* nb = (CCDeferStmt*)realloc(defers[defer_depth], (size_t)nc * sizeof(CCDeferStmt));
                if (!nb) {
                    free(stmt);
                    for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);
                    free(out);
                    g_defer_last_term = NULL;
                    g_defer_last_term_len = 0;
                    free(last_term_pref);
                    return -1;
                }
                defers[defer_depth] = nb;
                defer_caps[defer_depth] = nc;
            }
            defers[defer_depth][defer_counts[defer_depth]++] = (CCDeferStmt){ .line_no = defer_line, .stmt = stmt, .cond = cond };

            cc__append_newline_padding(&out, &outl, &outc, in_src + i, stmt_end - i);
            changed = 1;

            /* Skip original text (keep line_no tracking correct via the main loop's '\n' handler). */
            for (size_t k = i; k < stmt_end; k++) {
                if (in_src[k] == '\n') line_no++;
            }
            i = stmt_end;
            continue;
        }

        if (ch == '}') {
            /* Emit defers for this scope before the brace.
               Only emit DEFER_ALWAYS defers at }; conditional defers only run at return. */
            int d = depth;
            if (d < 0) d = 0;
            if (d >= 256) d = 255;
            if (fn_scope.active && d == 1 && fn_scope.has_top_level_defers && !fn_scope.has_top_level_conditional) {
                /* Hand-crafted-C indent for the function-cleanup
                 * epilogue.  Pre-fix, the `__cc_cleanup_N:` label,
                 * each defer body, and the trailing `return`
                 * statements all landed at column 0 — readable as
                 * a goto-spaghetti dump rather than the structured
                 * cleanup the user wrote.  We preserve the source's
                 * function-body indent (typically 4 spaces) for the
                 * defer bodies and trailing returns; the label
                 * intentionally stays at column 0 (idiomatic for C
                 * labels — `__cc_cleanup_N:` at column 0 is what a
                 * human would write). */
                size_t fn_body_indent = cc__source_line_indent_len(in_src, i);
                if (fn_body_indent == 0) fn_body_indent = 4; /* belt + suspenders for funcs at col 0 */
                /* The epilogue is generated glue: anchor it at the fn's
                 * OPENING line so a function ending near EOF cannot map
                 * its cleanup lines past the source (the `}` is re-anchored
                 * to its own line after the epilogue below). */
                if (line_marks && fn_scope.open_line > 0) {
                    cc_sb_append_fmt(&out, &outl, &outc, "/*CC_LN %d %s*/\n",
                                     fn_scope.open_line,
                                     ctx->input_path ? ctx->input_path : "<cc_input>");
                }
                cc_sb_append_fmt(&out, &outl, &outc, "__cc_cleanup_%d:\n", fn_scope.cleanup_label_id);
                for (int k = defer_counts[1] - 1; k >= 0; k--) {
                    if (defers[1][k].cond == DEFER_ALWAYS) {
                        cc__append_missing_indent_to(&out, &outl, &outc, fn_body_indent);
                        cc__append_str(&out, &outl, &outc, defers[1][k].stmt);
                        if (defers[1][k].stmt[0] != 0) {
                            size_t sl = strlen(defers[1][k].stmt);
                            if (sl == 0 || defers[1][k].stmt[sl - 1] != '\n') cc__append_str(&out, &outl, &outc, "\n");
                        }
                    }
                    free(defers[1][k].stmt);
                    defers[1][k].stmt = NULL;
                }
                defer_counts[1] = 0;
                if (!fn_scope.is_void) {
                    cc__append_missing_indent_to(&out, &outl, &outc, fn_body_indent);
                    cc_sb_append_fmt(&out, &outl, &outc,
                                   "if (__cc_ret_set_%d) return __cc_retval_%d;\n",
                                   fn_scope.cleanup_label_id, fn_scope.cleanup_label_id);
                    cc__append_missing_indent_to(&out, &outl, &outc, fn_body_indent);
                    if (cc__ret_type_plain_zero_ok(fn_scope.return_type))
                        cc__append_str(&out, &outl, &outc, "return 0;\n");
                    else
                        cc_sb_append_fmt(&out, &outl, &outc,
                                       "return (__typeof__(__cc_retval_%d)){0};\n",
                                       fn_scope.cleanup_label_id);
                }
                /* Masked ledger resync: the epilogue added lines with no
                 * #line accounting, so everything below the function's `}`
                 * (including EOF-adjacent functions) would map past the
                 * source region — anchor the `}` back to its own user
                 * line.  Same ledger-aware accounting as the prologue. */
                if (line_marks) {
                    const char* lp = NULL;
                    size_t lpl = 0;
                    int rb_line = cc_user_line_for_offset(in_src, in_len, i, 1, &lp, &lpl);
                    if (lp && lpl > 0) {
                        cc_sb_append_fmt(&out, &outl, &outc, "/*CC_LN %d %.*s*/\n",
                                         rb_line, (int)lpl, lp);
                    } else {
                        cc_sb_append_fmt(&out, &outl, &outc, "/*CC_LN %d %s*/\n",
                                         rb_line, ctx->input_path ? ctx->input_path : "<cc_input>");
                    }
                }
                fn_scope.active = 0;
            } else if (defer_counts[d] > 0) {
                if (!return_just_emitted[d]) {
                    /* Preserve the trailing-`}` indent across the
                     * defer injection.  At this point `out` looks
                     * like `...prev_stmt;\n        ` and we are
                     * about to inject defer bodies + the `}` byte.
                     * If we just emit defer bodies + newline and
                     * then the `}`, the user's `}` lands at column
                     * 0 because we already consumed its leading
                     * whitespace by emitting it bytewise via the
                     * main loop.
                     *
                     * Fix: snapshot the current-line whitespace
                     * tail, pop it off `out`, emit defer bodies on
                     * fresh lines with the proper indent, then
                     * re-emit the snapshotted whitespace so the
                     * `}` lands at its original indent.  When the
                     * current line tail is NOT all whitespace
                     * (e.g. `} else {` on one line, no leading
                     * indent on the `}`), there's nothing to
                     * preserve and we fall through to the simple
                     * inline path.
                     *
                     * Net effect on lowered C: defer bodies are
                     * indented at the enclosing-block statement
                     * level, and the user's `}` keeps its original
                     * indent.  See pre-fix bug: cramped
                     * `{wait; free;}\n}` against column 0; post-fix
                     * reads as if hand-written. */
                    char ind_buf[64];
                    size_t ind_len = 0;
                    {
                        size_t k = outl;
                        while (k > 0 && out[k - 1] != '\n') k--;
                        size_t tail_start = k;
                        size_t tail_len = outl - tail_start;
                        int all_ws = 1;
                        for (size_t q = 0; q < tail_len; q++) {
                            char tc = out[tail_start + q];
                            if (tc != ' ' && tc != '\t') { all_ws = 0; break; }
                        }
                        if (all_ws && tail_len > 0 && tail_len < sizeof(ind_buf)) {
                            memcpy(ind_buf, out + tail_start, tail_len);
                            ind_len = tail_len;
                            outl = tail_start;
                        }
                    }
                    size_t indent = cc__suggest_statement_indent(out, outl);
                    for (int k = defer_counts[d] - 1; k >= 0; k--) {
                        if (defers[d][k].cond == DEFER_ALWAYS) {
                            cc__append_missing_indent_to(&out, &outl, &outc, indent);
                            cc__append_str(&out, &outl, &outc, defers[d][k].stmt);
                            if (defers[d][k].stmt[0] != 0) {
                                size_t sl = strlen(defers[d][k].stmt);
                                if (sl == 0 || defers[d][k].stmt[sl - 1] != '\n') cc__append_str(&out, &outl, &outc, "\n");
                            }
                        }
                    }
                    if (ind_len > 0) {
                        cc__append_n(&out, &outl, &outc, ind_buf, ind_len);
                    }
                }
                for (int k = defer_counts[d] - 1; k >= 0; k--) {
                    /* Note: conditional defers are NOT emitted at } - only at return.
                       We still free them to avoid leaks. */
                    free(defers[d][k].stmt);
                    defers[d][k].stmt = NULL;
                }
                defer_counts[d] = 0;
            }
            return_just_emitted[d] = 0;
            if (d == 1) {
                fn_scope.active = 0;
            }
            if (loop_body_count > 0 && loop_body_depths[loop_body_count - 1] == d) {
                loop_body_count--;
            }
            if (breakable_body_count > 0 && breakable_body_depths[breakable_body_count - 1] == d) {
                breakable_body_count--;
            }
            if (depth > 0) depth--;
            cc__append_n(&out, &outl, &outc, &ch, 1);
            i++;
            continue;
        }

        if (ch == '{') {
            if (depth == 0) {
                char return_type[256];
                int is_void = 0;
                int is_async = 0;
                int has_top_level_defer = 0;
                int has_top_level_conditional = 0;
                if (cc__extract_function_return_type(in_src, in_len, i, return_type, sizeof(return_type),
                                                     &is_void, &is_async) &&
                    cc__scan_function_top_level_defer_info(in_src, in_len, i, &has_top_level_defer, &has_top_level_conditional)) {
                    memset(&fn_scope, 0, sizeof(fn_scope));
                    fn_scope.active = 1;
                    fn_scope.cleanup_label_id = ++g_defer_cleanup_label_id;
                    fn_scope.has_top_level_defers = has_top_level_defer;
                    fn_scope.has_top_level_conditional = has_top_level_conditional;
                    fn_scope.is_async = is_async;
                    fn_scope.is_void = is_void;
                    strncpy(fn_scope.return_type, return_type, sizeof(fn_scope.return_type) - 1);
                    fn_scope.return_type[sizeof(fn_scope.return_type) - 1] = 0;
                    if (strncmp(fn_scope.return_type, "CCResult_", 9) == 0) {
                        fn_scope.has_result_return = 1;
                        snprintf(fn_scope.ok_ctor, sizeof(fn_scope.ok_ctor), "cc_ok_%s", fn_scope.return_type);
                        snprintf(fn_scope.err_ctor, sizeof(fn_scope.err_ctor), "cc_err_%s", fn_scope.return_type);
                    }
                }
            }
            depth++;
            int dd = (depth < 0) ? 0 : (depth >= 256 ? 255 : depth);
            return_just_emitted[dd] = 0;
            if (pending_loop_body_open == i) {
                if (loop_body_count < 256) loop_body_depths[loop_body_count++] = dd;
                pending_loop_body_open = (size_t)-1;
            }
            if (pending_breakable_body_open == i) {
                if (breakable_body_count < 256) breakable_body_depths[breakable_body_count++] = dd;
                pending_breakable_body_open = (size_t)-1;
            }
            cc__append_n(&out, &outl, &outc, &ch, 1);
            /* Prologue on its own lines (human-readable emitted C), with a
             * masked #line resync so diagnostics below keep exact user
             * coordinates (unmasked at write time; see visit_codegen.c
             * cc__unmask_line_directives).  When `line_marks` is off (the
             * buffer is GENERATED text — closure defs, body relowers —
             * where newline counts are not user lines), the prologue rides
             * the `{` line instead: line-neutral beats a wrong anchor. */
            if (fn_scope.active && depth == 1 && fn_scope.has_top_level_defers && !fn_scope.has_top_level_conditional) {
                int brace_line = 0;
                const char* mark_path = ctx->input_path ? ctx->input_path : "<cc_input>";
                int mark_path_len = (int)strlen(mark_path);
                if (line_marks) {
                    /* Ledger-aware user line of the `{` (NOT a raw newline
                     * count — upstream expansions like @grammar inflate the
                     * buffer, and their #line/CC_LN entries must be honored
                     * or this marker stamps a buffer-space line that can
                     * even point past the source's EOF). */
                    const char* lp = NULL;
                    size_t lpl = 0;
                    brace_line = cc_user_line_for_offset(in_src, in_len, i, 1, &lp, &lpl);
                    if (lp && lpl > 0) {
                        mark_path = lp;
                        mark_path_len = (int)lpl;
                    }
                    fn_scope.open_line = brace_line;
                }
                if (!fn_scope.is_void) {
                    if (line_marks) {
                        cc_sb_append_fmt(&out, &outl, &outc,
                                       "\n    %s __cc_retval_%d;\n    int __cc_ret_set_%d = 0;\n/*CC_LN %d %.*s*/\n",
                                       fn_scope.return_type, fn_scope.cleanup_label_id, fn_scope.cleanup_label_id,
                                       brace_line, mark_path_len, mark_path);
                    } else {
                        cc_sb_append_fmt(&out, &outl, &outc,
                                       " %s __cc_retval_%d; int __cc_ret_set_%d = 0;",
                                       fn_scope.return_type, fn_scope.cleanup_label_id, fn_scope.cleanup_label_id);
                    }
                } else {
                    if (line_marks) {
                        cc_sb_append_fmt(&out, &outl, &outc,
                                       "\n    int __cc_ret_set_%d = 0;\n/*CC_LN %d %.*s*/\n",
                                       fn_scope.cleanup_label_id,
                                       brace_line, mark_path_len, mark_path);
                    } else {
                        cc_sb_append_fmt(&out, &outl, &outc, " int __cc_ret_set_%d = 0;", fn_scope.cleanup_label_id);
                    }
                }
                changed = 1;
            }
            i++;
            continue;
        }

        if (!(ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')) {
            int dd = (depth < 0) ? 0 : (depth >= 256 ? 255 : depth);
            return_just_emitted[dd] = 0;
        }
        cc__append_n(&out, &outl, &outc, &ch, 1);
        i++;
    }

    for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);
    g_defer_last_term = NULL;
    g_defer_last_term_len = 0;
    free(last_term_pref);
    last_term_pref = NULL;

    if (!changed) {
        free(out);
        return 0;
    }

    *out_src = out;
    *out_len = outl;
    return 1;
}

/* NEW: Collect @defer edits into EditBuffer without applying.
   NOTE: Due to the complexity of defer semantics (scope tracking, multiple injection points),
   this function uses the existing rewrite function and adds a single whole-file edit.
   Returns number of edits added (>= 0), or -1 on error. */
int cc__rewrite_defer_syntax(const CCVisitorCtx* ctx,
                            const char* in_src,
                            size_t in_len,
                            char** out_src,
                            size_t* out_len) {
    return cc__rewrite_defer_syntax_impl(ctx, in_src, in_len, out_src, out_len, 0);
}

/* Variant for the MAIN source buffer (1:1 with user lines): the defer
 * prologue goes on its own lines followed by a masked #line resync, so
 * emitted C stays human-shaped AND diagnostics stay exact. */
int cc__rewrite_defer_syntax_marked(const CCVisitorCtx* ctx,
                                    const char* in_src,
                                    size_t in_len,
                                    char** out_src,
                                    size_t* out_len) {
    return cc__rewrite_defer_syntax_impl(ctx, in_src, in_len, out_src, out_len, 1);
}

int cc__collect_defer_edits(const CCVisitorCtx* ctx, CCEditBuffer* eb) {
    if (!ctx || !eb || !eb->src) return 0;

    char* rewritten = NULL;
    size_t rewritten_len = 0;
    int r = cc__rewrite_defer_syntax(ctx, eb->src, eb->src_len, &rewritten, &rewritten_len);
    
    if (r < 0) {
        /* Error already printed */
        return -1;
    }
    if (r == 0 || !rewritten) {
        /* No changes */
        return 0;
    }    /* Add a whole-file replacement edit */
    int edits_added = 0;
    if (cc_edit_buffer_add(eb, 0, eb->src_len, rewritten, 40, "defer") == 0) {
        edits_added = 1;
    }
    free(rewritten);
    return edits_added;
}