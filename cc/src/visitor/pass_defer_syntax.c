#include "pass_defer_syntax.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/path.h"
#include "util/text.h"
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

static void cc__ensure_line_start(char** out, size_t* out_len, size_t* out_cap) {
    if (!out || !out_len || !out_cap) return;
    if (*out_len == 0) return;
    char last = (*out)[*out_len - 1];
    if (last != '\n') cc__append_n(out, out_len, out_cap, "\n", 1);
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

static int cc__count_top_level_semicolons(const char* s, size_t n) {
    int count = 0;
    int par = 0, brk = 0, br = 0;
    int in_str = 0;
    char qch = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;

    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (in_line_comment) {
            if (ch == '\n') in_line_comment = 0;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && i + 1 < n && s[i + 1] == '/') { in_block_comment = 0; i++; }
            continue;
        }
        if (in_str) {
            if (ch == '\\' && i + 1 < n) { i++; continue; }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '/' && i + 1 < n && s[i + 1] == '/') { in_line_comment = 1; i++; continue; }
        if (ch == '/' && i + 1 < n && s[i + 1] == '*') { in_block_comment = 1; i++; continue; }
        if (ch == '"' || ch == '\'') { in_str = 1; qch = ch; continue; }
        if (ch == '(') par++;
        else if (ch == ')' && par > 0) par--;
        else if (ch == '[') brk++;
        else if (ch == ']' && brk > 0) brk--;
        else if (ch == '{') br++;
        else if (ch == '}' && br > 0) br--;
        else if (ch == ';' && par == 0 && brk == 0 && br == 0) count++;
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

    normalized = (char*)malloc((inner_end - inner_start) + 3);
    if (!normalized) return stmt;
    normalized[0] = '{';
    memcpy(normalized + 1, stmt + inner_start, inner_end - inner_start);
    normalized[1 + (inner_end - inner_start)] = '}';
    normalized[2 + (inner_end - inner_start)] = 0;
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
 * into the function prologue.  Scan is O(end) per call. */
static size_t cc__last_stmt_terminator_before(const char* s, size_t end) {
    size_t last = 0;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    if (!s || end == 0) return 0;
    for (size_t i = 0; i < end; ++i) {
        char c = s[i];
        char c2 = (i + 1 < end) ? s[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; } continue; }
        if (in_str) { if (c == '\\' && c2) { i++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && c2) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c == ';' || c == '{' || c == '}') last = i + 1;
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
    int depth = 1;
    int in_str = 0;
    char qch = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;

    if (!s || open_i >= len || s[open_i] != '{') return 0;
    for (size_t i = open_i + 1; i < len; i++) {
        char ch = s[i];
        if (in_line_comment) {
            if (ch == '\n') in_line_comment = 0;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && i + 1 < len && s[i + 1] == '/') { in_block_comment = 0; i++; }
            continue;
        }
        if (in_str) {
            if (ch == '\\' && i + 1 < len) { i++; continue; }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '/' && i + 1 < len && s[i + 1] == '/') { in_line_comment = 1; i++; continue; }
        if (ch == '/' && i + 1 < len && s[i + 1] == '*') { in_block_comment = 1; i++; continue; }
        if (ch == '"' || ch == '\'') { in_str = 1; qch = ch; continue; }
        if (ch == '{') depth++;
        else if (ch == '}') {
            depth--;
            if (depth == 0) {
                if (out_close_i) *out_close_i = i;
                return 1;
            }
        }
    }
    return 0;
}

static int cc__scan_function_top_level_defer_info(const char* s, size_t len, size_t open_i,
                                                  int* out_has_defer, int* out_has_conditional) {
    size_t close_i = 0;
    int rel_depth = 1;
    int in_str = 0;
    char qch = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;

    if (out_has_defer) *out_has_defer = 0;
    if (out_has_conditional) *out_has_conditional = 0;
    if (!s || open_i >= len || s[open_i] != '{') return 0;
    if (!cc__find_matching_brace_text(s, len, open_i, &close_i)) return 0;

    for (size_t i = open_i + 1; i < close_i; i++) {
        char ch = s[i];
        if (in_line_comment) {
            if (ch == '\n') in_line_comment = 0;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && i + 1 < len && s[i + 1] == '/') { in_block_comment = 0; i++; }
            continue;
        }
        if (in_str) {
            if (ch == '\\' && i + 1 < len) { i++; continue; }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '/' && i + 1 < len && s[i + 1] == '/') { in_line_comment = 1; i++; continue; }
        if (ch == '/' && i + 1 < len && s[i + 1] == '*') { in_block_comment = 1; i++; continue; }
        if (ch == '"' || ch == '\'') { in_str = 1; qch = ch; continue; }
        if (ch == '{') { rel_depth++; continue; }
        if (ch == '}') { rel_depth--; continue; }
        if (rel_depth == 1 && cc__token_is(s, len, i, "@defer")) {
            size_t j = i + 6;
            if (out_has_defer) *out_has_defer = 1;
            while (j < close_i && (s[j] == ' ' || s[j] == '\t')) j++;
            if (j < close_i && s[j] == '(') {
                if (out_has_conditional) *out_has_conditional = 1;
            }
        }
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

static int cc__match_result_ctor_prefix_arg(const char* expr, size_t expr_len, const char* prefix,
                                            size_t* out_arg_start, size_t* out_arg_end) {
    size_t start = 0, end = expr_len, name_end;
    size_t open_paren, close_paren;
    size_t prefix_len;
    int par = 1;
    int in_str = 0;
    int in_chr = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;

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

    close_paren = open_paren + 1;
    while (close_paren < end) {
        char ch = expr[close_paren];
        char ch2 = (close_paren + 1 < end) ? expr[close_paren + 1] : 0;
        if (in_line_comment) {
            if (ch == '\n') in_line_comment = 0;
            close_paren++;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && ch2 == '/') { in_block_comment = 0; close_paren += 2; continue; }
            close_paren++;
            continue;
        }
        if (in_str) {
            if (ch == '\\' && close_paren + 1 < end) { close_paren += 2; continue; }
            if (ch == '"') in_str = 0;
            close_paren++;
            continue;
        }
        if (in_chr) {
            if (ch == '\\' && close_paren + 1 < end) { close_paren += 2; continue; }
            if (ch == '\'') in_chr = 0;
            close_paren++;
            continue;
        }
        if (ch == '/' && ch2 == '/') { in_line_comment = 1; close_paren += 2; continue; }
        if (ch == '/' && ch2 == '*') { in_block_comment = 1; close_paren += 2; continue; }
        if (ch == '"') { in_str = 1; close_paren++; continue; }
        if (ch == '\'') { in_chr = 1; close_paren++; continue; }
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
    if (out_arg_start) *out_arg_start = open_paren + 1;
    if (out_arg_end) *out_arg_end = close_paren;
    return 1;
}

static int cc__match_result_ctor_name_arg(const char* expr, size_t expr_len, const char* name,
                                          size_t* out_arg_start, size_t* out_arg_end) {
    size_t start = 0, end = expr_len, name_len;
    size_t open_paren, close_paren;
    int par = 1;
    int in_str = 0;
    int in_chr = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;

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

    close_paren = open_paren + 1;
    while (close_paren < end) {
        char ch = expr[close_paren];
        char ch2 = (close_paren + 1 < end) ? expr[close_paren + 1] : 0;
        if (in_line_comment) {
            if (ch == '\n') in_line_comment = 0;
            close_paren++;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && ch2 == '/') { in_block_comment = 0; close_paren += 2; continue; }
            close_paren++;
            continue;
        }
        if (in_str) {
            if (ch == '\\' && close_paren + 1 < end) { close_paren += 2; continue; }
            if (ch == '"') in_str = 0;
            close_paren++;
            continue;
        }
        if (in_chr) {
            if (ch == '\\' && close_paren + 1 < end) { close_paren += 2; continue; }
            if (ch == '\'') in_chr = 0;
            close_paren++;
            continue;
        }
        if (ch == '/' && ch2 == '/') { in_line_comment = 1; close_paren += 2; continue; }
        if (ch == '/' && ch2 == '*') { in_block_comment = 1; close_paren += 2; continue; }
        if (ch == '"') { in_str = 1; close_paren++; continue; }
        if (ch == '\'') { in_chr = 1; close_paren++; continue; }
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

static int cc__scan_stmt_end_semicolon(const char* s, size_t len, size_t i, size_t* out_end_off) {
    int par = 0, brk = 0, br = 0;
    int in_str = 0;
    char qch = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    for (; i < len; i++) {
        char ch = s[i];
        if (in_line_comment) {
            if (ch == '\n') in_line_comment = 0;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && i + 1 < len && s[i + 1] == '/') { in_block_comment = 0; i++; }
            continue;
        }
        if (in_str) {
            if (ch == '\\' && i + 1 < len) { i++; continue; }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '/' && i + 1 < len && s[i + 1] == '/') { in_line_comment = 1; i++; continue; }
        if (ch == '/' && i + 1 < len && s[i + 1] == '*') { in_block_comment = 1; i++; continue; }
        if (ch == '"' || ch == '\'') { in_str = 1; qch = ch; continue; }

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
    }
    return 0;
}

int cc__rewrite_defer_syntax(const CCVisitorCtx* ctx,
                            const char* in_src,
                            size_t in_len,
                            char** out_src,
                            size_t* out_len) {
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
    int in_str = 0;
    char qch = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;

    int changed = 0;
    CCDeferFunctionScope fn_scope;
    memset(&fn_scope, 0, sizeof(fn_scope));

    for (size_t i = 0; i < in_len; i++) {
        char ch = in_src[i];

        if (ch == '\n') line_no++;

        if (in_line_comment) {
            cc__append_n(&out, &outl, &outc, &ch, 1);
            if (ch == '\n') in_line_comment = 0;
            continue;
        }
        if (in_block_comment) {
            cc__append_n(&out, &outl, &outc, &ch, 1);
            if (ch == '*' && i + 1 < in_len && in_src[i + 1] == '/') {
                cc__append_n(&out, &outl, &outc, &in_src[i + 1], 1);
                i++;
                in_block_comment = 0;
            }
            continue;
        }
        if (in_str) {
            cc__append_n(&out, &outl, &outc, &ch, 1);
            if (ch == '\\' && i + 1 < in_len) {
                cc__append_n(&out, &outl, &outc, &in_src[i + 1], 1);
                i++;
                continue;
            }
            if (ch == qch) in_str = 0;
            continue;
        }

        if (ch == '/' && i + 1 < in_len && in_src[i + 1] == '/') {
            cc__append_n(&out, &outl, &outc, &in_src[i], 2);
            i++;
            in_line_comment = 1;
            continue;
        }
        if (ch == '/' && i + 1 < in_len && in_src[i + 1] == '*') {
            cc__append_n(&out, &outl, &outc, &in_src[i], 2);
            i++;
            in_block_comment = 1;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            cc__append_n(&out, &outl, &outc, &ch, 1);
            in_str = 1;
            qch = ch;
            continue;
        }

        /* `cancel ...;` is not implemented: hard error. */
        if (cc__token_is(in_src, in_len, i, "cancel")) {
            char rel[1024];
            const char* f = cc_path_rel_to_repo(ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel));
            cc_pass_error_cat(f, line_no, 1, CC_ERR_SYNTAX,
                              "'cancel' is not implemented in defer lowering (use structured scopes instead)");
            for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);
            free(out);
            return -1;
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
                i = stmt_end - 1;
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
                        if (fn_scope.is_async) {
                            cc_sb_append_fmt(&out, &outl, &outc, "__cc_retval_%d = __cc_ret;\n", fn_scope.cleanup_label_id);
                            cc__append_missing_indent_to(&out, &outl, &outc, synth_indent);
                            cc_sb_append_fmt(&out, &outl, &outc, "__cc_ret_set_%d = 1;\n", fn_scope.cleanup_label_id);
                            cc__append_missing_indent_to(&out, &outl, &outc, synth_indent);
                            cc_sb_append_fmt(&out, &outl, &outc, "goto __cc_cleanup_%d;\n}", fn_scope.cleanup_label_id);
                        } else {
                            cc_sb_append_fmt(&out, &outl, &outc, "__cc_ret(%d, __cc_ret);\n}", fn_scope.cleanup_label_id);
                        }
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
                            if (fn_scope.is_async) {
                                cc_sb_append_fmt(&out, &outl, &outc, "__cc_retval_%d = (", fn_scope.cleanup_label_id);
                                cc__append_n(&out, &outl, &outc, in_src + expr_start, expr_end - expr_start);
                                cc__append_str(&out, &outl, &outc, ");\n");
                                cc__append_missing_indent_to(&out, &outl, &outc, synth_indent);
                                cc_sb_append_fmt(&out, &outl, &outc, "__cc_ret_set_%d = 1;\n", fn_scope.cleanup_label_id);
                                cc__append_missing_indent_to(&out, &outl, &outc, synth_indent);
                                cc_sb_append_fmt(&out, &outl, &outc, "goto __cc_cleanup_%d;", fn_scope.cleanup_label_id);
                            } else if (use_ret_ok) {
                                cc_sb_append_fmt(&out, &outl, &outc, "__cc_ret_ok(%d, ", fn_scope.cleanup_label_id);
                                cc__append_n(&out, &outl, &outc,
                                             in_src + expr_start + ok_arg_start,
                                             ok_arg_end - ok_arg_start);
                                cc__append_str(&out, &outl, &outc, ");");
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
                                    cc_sb_append_fmt(&out, &outl, &outc, "__cc_ret_err(%d, cc_io_error(", fn_scope.cleanup_label_id);
                                    cc__append_n(&out, &outl, &outc,
                                                 in_src + expr_start + err_arg_start,
                                                 err_arg_end - err_arg_start);
                                    cc__append_str(&out, &outl, &outc, "));");
                                } else {
                                    if (use_ret_err_short) {
                                        cc_sb_append_fmt(&out, &outl, &outc, "__cc_ret(%d, ", fn_scope.cleanup_label_id);
                                        cc__append_n(&out, &outl, &outc, in_src + expr_start, expr_end - expr_start);
                                        cc__append_str(&out, &outl, &outc, ");");
                                    } else {
                                        cc_sb_append_fmt(&out, &outl, &outc, "__cc_ret_err(%d, ", fn_scope.cleanup_label_id);
                                        cc__append_n(&out, &outl, &outc,
                                                     in_src + expr_start + err_arg_start,
                                                     err_arg_end - err_arg_start);
                                        cc__append_str(&out, &outl, &outc, ");");
                                    }
                                }
                            } else {
                                cc_sb_append_fmt(&out, &outl, &outc, "__cc_ret(%d, ", fn_scope.cleanup_label_id);
                                cc__append_n(&out, &outl, &outc, in_src + expr_start, expr_end - expr_start);
                                cc__append_str(&out, &outl, &outc, ");");
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
                    for (int d = depth; d >= 0; d--) {
                        int dd = (d < 0) ? 0 : (d >= 256 ? 255 : d);
                        for (int k = defer_counts[dd] - 1; k >= 0; k--) {
                            cc__append_str(&out, &outl, &outc, defers[dd][k].stmt);
                            cc__append_n(&out, &outl, &outc, "\n", 1);
                        }
                    }
                    cc__ensure_line_start(&out, &outl, &outc);
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
            i = stmt_end - 1;
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
                return -1;
            }

            size_t stmt_len = stmt_end - stmt_start;
            char* stmt = (char*)malloc(stmt_len + 1);
            if (!stmt) {
                for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);
                free(out);
                return -1;
            }
            memcpy(stmt, in_src + stmt_start, stmt_len);
            stmt[stmt_len] = 0;
            stmt = cc__normalize_defer_stmt(stmt);
            if (!stmt) {
                for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);
                free(out);
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
            i = stmt_end - 1;
            continue;
        }

        if (ch == '}') {
            /* Emit defers for this scope before the brace.
               Only emit DEFER_ALWAYS defers at }; conditional defers only run at return. */
            int d = depth;
            if (d < 0) d = 0;
            if (d >= 256) d = 255;
            if (fn_scope.active && d == 1 && fn_scope.has_top_level_defers && !fn_scope.has_top_level_conditional) {
                cc_sb_append_fmt(&out, &outl, &outc, "__cc_cleanup_%d:\n", fn_scope.cleanup_label_id);
                for (int k = defer_counts[1] - 1; k >= 0; k--) {
                    if (defers[1][k].cond == DEFER_ALWAYS) {
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
                    cc_sb_append_fmt(&out, &outl, &outc,
                                   "if (__cc_ret_set_%d) return __cc_retval_%d;\n",
                                   fn_scope.cleanup_label_id, fn_scope.cleanup_label_id);
                    cc_sb_append_fmt(&out, &outl, &outc,
                                   "return (__typeof__(__cc_retval_%d)){0};\n",
                                   fn_scope.cleanup_label_id);
                }
                fn_scope.active = 0;
            } else if (defer_counts[d] > 0) {
                if (!return_just_emitted[d]) {
                    for (int k = defer_counts[d] - 1; k >= 0; k--) {
                        if (defers[d][k].cond == DEFER_ALWAYS) {
                            cc__append_str(&out, &outl, &outc, defers[d][k].stmt);
                            if (defers[d][k].stmt[0] != 0) {
                                size_t sl = strlen(defers[d][k].stmt);
                                if (sl == 0 || defers[d][k].stmt[sl - 1] != '\n') cc__append_str(&out, &outl, &outc, "\n");
                            }
                        }
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
            if (depth > 0) depth--;
            cc__append_n(&out, &outl, &outc, &ch, 1);
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
            cc__append_n(&out, &outl, &outc, &ch, 1);
            if (fn_scope.active && depth == 1 && fn_scope.has_top_level_defers && !fn_scope.has_top_level_conditional && !fn_scope.is_void) {
                cc_sb_append_fmt(&out, &outl, &outc,
                               "\n    %s __cc_retval_%d;\n    int __cc_ret_set_%d = 0;\n",
                               fn_scope.return_type, fn_scope.cleanup_label_id, fn_scope.cleanup_label_id);
                changed = 1;
            } else if (fn_scope.active && depth == 1 && fn_scope.has_top_level_defers && !fn_scope.has_top_level_conditional) {
                cc_sb_append_fmt(&out, &outl, &outc, "\n    int __cc_ret_set_%d = 0;\n", fn_scope.cleanup_label_id);
                changed = 1;
            }
            continue;
        }

        if (!(ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')) {
            int dd = (depth < 0) ? 0 : (depth >= 256 ? 255 : depth);
            return_just_emitted[dd] = 0;
        }
        cc__append_n(&out, &outl, &outc, &ch, 1);
    }

    for (int d = 0; d < 256; d++) cc__free_defer_list(defers[d], defer_counts[d]);

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