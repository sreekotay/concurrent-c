#include "preprocess.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>

#include <ccc/cc_slice.cch>

#include "header/lower_header.h"
#include "comptime/symbols.h"
#include "preprocess/type_registry.h"
#include "result_spec.h"
#include "util/path.h"
#include "util/result_fn_registry.h"
#include "util/text.h"
#include "visitor/ufcs.h"
#include "visitor/visitor.h"
#include "visitor/pass_err_syntax.h"
#include "visitor/pass_result_unwrap.h"
#include "visitor/pass_unwrap_destroy.h"

/* ========================================================================== */
/* Diagnostic helpers (gcc/clang compatible format)                           */
/* Format: file:line:col: error: category: message                            */
/* Categories: syntax, channel, type, async, closure, slice                   */
/* ========================================================================== */

static void cc_pp_error_cat(const char* file, int line, int col, 
                            const char* category, const char* fmt, ...) {
    const char* f = file ? file : "<input>";
    int l = (line > 0) ? line : 1;
    int c = (col > 0) ? col : 1;
    fprintf(stderr, "%s:%d:%d: error: %s: ", f, l, c, category);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

/* ========================================================================== */
/* Scanner helper for skipping comments and strings                           */
/* Reduces code duplication across cc__rewrite_* functions                    */
/* ========================================================================== */

typedef struct {
    int in_line_comment;
    int in_block_comment;
    int in_str;
    int in_chr;
    int line;  /* Current line (1-based), updated if track_pos is true */
    int col;   /* Current column (1-based), updated if track_pos is true */
} CCScannerState;

/* Initialize scanner state */
static void cc_scanner_init(CCScannerState* s) {
    s->in_line_comment = 0;
    s->in_block_comment = 0;
    s->in_str = 0;
    s->in_chr = 0;
    s->line = 1;
    s->col = 1;
}

/* Process current character and advance past non-code (comments, strings).
 * Returns 1 if we're in a comment/string (caller should continue to next char).
 * Returns 0 if we're at actual code (caller should process the character).
 * Updates *pos to skip multi-char sequences.
 * Updates s->line and s->col to track position.
 */
static int cc_scanner_skip_non_code(CCScannerState* s, const char* src, size_t n, size_t* pos) {
    size_t i = *pos;
    if (i >= n) return 0;
    
    char c = src[i];
    char c2 = (i + 1 < n) ? src[i + 1] : 0;
    
    /* Track newlines for position */
    if (c == '\n') { s->line++; s->col = 1; } else { s->col++; }
    
    /* Inside line comment */
    if (s->in_line_comment) {
        if (c == '\n') s->in_line_comment = 0;
        (*pos)++;
        return 1;
    }
    
    /* Inside block comment */
    if (s->in_block_comment) {
        if (c == '*' && c2 == '/') {
            s->in_block_comment = 0;
            *pos += 2;
            s->col++;  /* Account for '/' */
        } else {
            (*pos)++;
        }
        return 1;
    }
    
    /* Inside string literal */
    if (s->in_str) {
        if (c == '\\' && i + 1 < n) {
            *pos += 2;  /* Skip escape sequence */
            s->col++;   /* Account for escaped char */
        } else {
            if (c == '"') s->in_str = 0;
            (*pos)++;
        }
        return 1;
    }
    
    /* Inside char literal */
    if (s->in_chr) {
        if (c == '\\' && i + 1 < n) {
            *pos += 2;  /* Skip escape sequence */
            s->col++;   /* Account for escaped char */
        } else {
            if (c == '\'') s->in_chr = 0;
            (*pos)++;
        }
        return 1;
    }
    
    /* Check for start of comment/string */
    if (c == '/' && c2 == '/') { s->in_line_comment = 1; *pos += 2; s->col++; return 1; }
    if (c == '/' && c2 == '*') { s->in_block_comment = 1; *pos += 2; s->col++; return 1; }
    if (c == '"') { s->in_str = 1; (*pos)++; return 1; }
    if (c == '\'') { s->in_chr = 1; (*pos)++; return 1; }
    
    /* At actual code - undo the col++ since caller will handle this char */
    if (c != '\n') s->col--;
    
    return 0;
}

/* ========================================================================== */
/* End scanner helper                                                         */
/* ========================================================================== */

/* ========================================================================== */
/* Pass chain helper - tracks allocations for cleanup                         */
/* ========================================================================== */

#define CC_PASS_CHAIN_MAX 32

typedef struct {
    const char* src;      /* Current source buffer */
    size_t len;           /* Current source length */
    char* allocs[CC_PASS_CHAIN_MAX];  /* Tracked allocations */
    int n_allocs;         /* Number of tracked allocations */
} CCPassChain;

static int cc__apply_phase1_canonical_passes(CCPassChain* chain,
                                             const char* input_path);
static int cc__apply_phase3_host_lowering_passes(CCPassChain* chain,
                                                 const char* input_path);
char* cc__rewrite_at_await(const char* src, size_t n); /* defined later */

/* Initialize chain with source buffer (buffer is NOT owned by chain) */
static inline void cc_pass_chain_init(CCPassChain* c, const char* src, size_t len) {
    c->src = src;
    c->len = len;
    c->n_allocs = 0;
}

/* Apply a pass result to the chain.
 * - If result is NULL: no change, chain continues with current buffer
 * - If result is (char*)-1: error, returns -1 (caller should cleanup)
 * - Otherwise: result becomes new source, tracked for later cleanup
 * Returns 0 on success, -1 on error.
 */
static inline int cc_pass_chain_apply(CCPassChain* c, char* result) {
    if (result == (char*)-1) return -1;  /* Error sentinel */
    if (result) {
        if (c->n_allocs < CC_PASS_CHAIN_MAX) {
            c->allocs[c->n_allocs++] = result;
        }
        c->src = result;
        c->len = strlen(result);
    }
    return 0;
}

/* Free all tracked allocations */
static inline void cc_pass_chain_free(CCPassChain* c) {
    for (int i = 0; i < c->n_allocs; i++) {
        free(c->allocs[i]);
    }
    c->n_allocs = 0;
}

/* Convenience macros for common patterns */
#define CC_CHAIN(c, call) \
    do { if (cc_pass_chain_apply(&(c), call) < 0) goto chain_cleanup; } while(0)

/* ========================================================================== */
/* End pass chain helper                                                      */
/* ========================================================================== */

static int cc__pp_is_ident_char(char ch) {
    return isalnum((unsigned char)ch) || ch == '_';
}

static void cc__pp_offset_to_line_col(const char* src, size_t off, int* out_line, int* out_col) {
    int line = 1;
    int col = 1;
    for (size_t i = 0; src && i < off; i++) {
        if (src[i] == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
    }
    if (out_line) *out_line = line;
    if (out_col) *out_col = col;
}

static int cc__pp_find_top_level_equal(const char* src, size_t start, size_t end, size_t* out_pos) {
    int par = 0, brk = 0, br = 0;
    int in_str = 0, in_chr = 0, in_lc = 0, in_bc = 0;
    for (size_t i = start; i < end; i++) {
        char c = src[i];
        char c2 = (i + 1 < end) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; } continue; }
        if (in_str) { if (c == '\\' && i + 1 < end) { i++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < end) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c == '(') par++;
        else if (c == ')') { if (par) par--; }
        else if (c == '[') brk++;
        else if (c == ']') { if (brk) brk--; }
        else if (c == '{') br++;
        else if (c == '}') { if (br) br--; }
        else if (c == '=' && par == 0 && brk == 0 && br == 0) {
            if (out_pos) *out_pos = i;
            return 1;
        }
    }
    return 0;
}

static int cc__pp_builtin_destroy_info(const char* declared_type,
                                       const char** out_pre_callee,
                                       const char** out_callee,
                                       int* out_pass_address) {
    int saw_nursery = 0;
    int saw_arena = 0;
    int saw_chan = 0;
    int saw_star = 0;
    size_t i = 0;
    if (out_pre_callee) *out_pre_callee = NULL;
    if (out_callee) *out_callee = NULL;
    if (out_pass_address) *out_pass_address = 0;
    if (!declared_type) return 0;
    while (declared_type[i]) {
        if (declared_type[i] == '*') {
            saw_star = 1;
            i++;
            continue;
        }
        if (isalpha((unsigned char)declared_type[i]) || declared_type[i] == '_') {
            size_t start = i;
            while (declared_type[i] &&
                   (isalnum((unsigned char)declared_type[i]) || declared_type[i] == '_')) {
                i++;
            }
            size_t len = i - start;
            if (len == sizeof("CCArena") - 1 &&
                memcmp(declared_type + start, "CCArena", len) == 0) {
                saw_arena = 1;
            } else if (len == sizeof("CCNursery") - 1 &&
                       memcmp(declared_type + start, "CCNursery", len) == 0) {
                saw_nursery = 1;
            } else if (len == sizeof("CCChan") - 1 &&
                       memcmp(declared_type + start, "CCChan", len) == 0) {
                saw_chan = 1;
            }
            continue;
        }
        i++;
    }
    if (saw_arena && !saw_nursery && !saw_chan && !saw_star) {
        if (out_callee) *out_callee = "cc_arena_destroy";
        if (out_pass_address) *out_pass_address = 1;
        return 1;
    }
    if (saw_nursery && saw_star && !saw_arena && !saw_chan) {
        /* Nurseries must wait for outstanding tasks before the handle is
         * freed, so the full lifecycle is wait -> user body -> free.  This
         * mirrors the hook sequence registered for the `@create(CCNursery)
         * @destroy { body }` primary path in pass_create.c. */
        if (out_pre_callee) *out_pre_callee = "cc_nursery_wait";
        if (out_callee) *out_callee = "cc_nursery_free";
        if (out_pass_address) *out_pass_address = 0;
        return 1;
    }
    if (saw_chan && saw_star && !saw_nursery && !saw_arena) {
        /* Channels auto-free on scope exit via `cc_channel_free`, which is a
         * _Generic macro that dispatches to cc_chan_free for `CCChan*`. */
        if (out_callee) *out_callee = "cc_channel_free";
        if (out_pass_address) *out_pass_address = 0;
        return 1;
    }
    return 0;
}

static char* cc__rewrite_builtin_owned_decl_annotations(const char* src, size_t n, const char* input_path) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t last_emit = 0;
    int changed = 0;
    CCScannerState scanner;
    cc_scanner_init(&scanner);
    if (!src || n == 0) return NULL;

    for (size_t i = 0; i < n; ) {
        size_t token_len = 0;
        int is_destroy = 0;
        size_t stmt_s = 0, eq = 0, p = 0, name_s = 0, name_e = 0;
        size_t rhs_s = 0, rhs_e = 0, semi = 0;
        size_t destroy_body_s = 0, destroy_body_e = 0;
        char declared_type[256];
        const char* destroy_pre_callee = NULL;
        const char* destroy_callee = NULL;
        int pass_address = 0;

        if (cc_scanner_skip_non_code(&scanner, src, n, &i)) continue;
        if (src[i] != '@') {
            i++;
            continue;
        }
        if (i + 8 <= n && memcmp(src + i, "@destroy", 8) == 0 &&
            (i + 8 >= n || !cc__pp_is_ident_char(src[i + 8]))) {
            token_len = 8;
            is_destroy = 1;
        } else if (i + 7 <= n && memcmp(src + i, "@detach", 7) == 0 &&
                   (i + 7 >= n || !cc__pp_is_ident_char(src[i + 7]))) {
            token_len = 7;
            is_destroy = 0;
        } else {
            i++;
            continue;
        }

        stmt_s = i;
        while (stmt_s > 0 &&
               src[stmt_s - 1] != ';' &&
               src[stmt_s - 1] != '{' &&
               src[stmt_s - 1] != '}') {
            stmt_s--;
        }
        stmt_s = cc_skip_ws_and_comments(src, n, stmt_s);
        if (!cc__pp_find_top_level_equal(src, stmt_s, i, &eq)) {
            i++;
            continue;
        }

        p = eq;
        while (p > stmt_s && isspace((unsigned char)src[p - 1])) p--;
        name_e = p;
        while (p > stmt_s && cc__pp_is_ident_char(src[p - 1])) p--;
        name_s = p;
        if (name_s >= name_e || !(isalpha((unsigned char)src[name_s]) || src[name_s] == '_')) {
            i++;
            continue;
        }

        {
            size_t declared_type_len = name_s - stmt_s;
            while (declared_type_len > 0 &&
                   (src[stmt_s + declared_type_len - 1] == ' ' ||
                    src[stmt_s + declared_type_len - 1] == '\t')) {
                declared_type_len--;
            }
            if (declared_type_len == 0) {
                i++;
                continue;
            }
            if (declared_type_len >= sizeof(declared_type)) {
                declared_type_len = sizeof(declared_type) - 1;
            }
            memcpy(declared_type, src + stmt_s, declared_type_len);
            declared_type[declared_type_len] = '\0';
        }

        if (!cc__pp_builtin_destroy_info(declared_type, &destroy_pre_callee, &destroy_callee, &pass_address)) {
            i++;
            continue;
        }

        rhs_s = cc_skip_ws_and_comments(src, n, eq + 1);
        rhs_e = i;
        while (rhs_e > rhs_s && isspace((unsigned char)src[rhs_e - 1])) rhs_e--;
        if (rhs_s >= rhs_e) {
            i++;
            continue;
        }
        if (rhs_e >= rhs_s + 7 && memcmp(src + rhs_s, "@create(", 8) == 0) {
            i++;
            continue;
        }

        if (is_destroy) {
            size_t after_destroy = cc_skip_ws_and_comments(src, n, i + token_len);
            if (after_destroy < n && src[after_destroy] == '{') {
                destroy_body_s = after_destroy;
                if (!cc_find_matching_brace(src, n, destroy_body_s, &destroy_body_e)) {
                    int line = 1, col = 1;
                    cc__pp_offset_to_line_col(src, destroy_body_s, &line, &col);
                    cc_pp_error_cat(input_path, line, col, "syntax",
                                    "malformed '@destroy { ... }' block");
                    free(out);
                    return (char*)-1;
                }
                semi = cc_skip_ws_and_comments(src, n, destroy_body_e + 1);
            } else {
                semi = after_destroy;
            }
            if (semi >= n || src[semi] != ';') {
                int line = 1, col = 1;
                cc__pp_offset_to_line_col(src, semi < n ? semi : n, &line, &col);
                cc_pp_error_cat(input_path, line, col, "syntax",
                                "expected ';' after '@destroy' declaration");
                free(out);
                return (char*)-1;
            }
        } else {
            size_t after_detach = cc_skip_ws_and_comments(src, n, i + token_len);
            if (after_detach < n && src[after_detach] == '{') {
                int line = 1, col = 1;
                cc__pp_offset_to_line_col(src, after_detach, &line, &col);
                cc_pp_error_cat(input_path, line, col, "syntax",
                                "'@detach' does not take a cleanup body; use '@destroy { ... }' for custom teardown");
                free(out);
                return (char*)-1;
            }
            semi = after_detach;
            if (semi >= n || src[semi] != ';') {
                int line = 1, col = 1;
                cc__pp_offset_to_line_col(src, semi < n ? semi : n, &line, &col);
                cc_pp_error_cat(input_path, line, col, "syntax",
                                "expected ';' after '@detach' declaration");
                free(out);
                return (char*)-1;
            }
        }

        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
        if (is_destroy && destroy_callee) {
            int has_custom_body = (destroy_body_s && destroy_body_e > destroy_body_s);
            cc_sb_append_cstr(&out, &out_len, &out_cap, "; ");
            if (!has_custom_body) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "@defer { ");
                if (destroy_pre_callee) {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, destroy_pre_callee);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "(");
                    if (pass_address) {
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "&");
                    }
                    cc_sb_append(&out, &out_len, &out_cap, src + name_s, name_e - name_s);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "); ");
                }
                cc_sb_append_cstr(&out, &out_len, &out_cap, destroy_callee);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "(");
                if (pass_address) {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "&");
                }
                cc_sb_append(&out, &out_len, &out_cap, src + name_s, name_e - name_s);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "); };\n");
            } else {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "@defer { ");
                if (destroy_pre_callee) {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, destroy_pre_callee);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "(");
                    if (pass_address) {
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "&");
                    }
                    cc_sb_append(&out, &out_len, &out_cap, src + name_s, name_e - name_s);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "); ");
                }
                cc_sb_append(&out, &out_len, &out_cap, src + destroy_body_s, destroy_body_e - destroy_body_s + 1);
                cc_sb_append_cstr(&out, &out_len, &out_cap, " ");
                cc_sb_append_cstr(&out, &out_len, &out_cap, destroy_callee);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "(");
                if (pass_address) {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "&");
                }
                cc_sb_append(&out, &out_len, &out_cap, src + name_s, name_e - name_s);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "); };\n");
            }
        } else {
            cc_sb_append_cstr(&out, &out_len, &out_cap, ";\n");
        }

        last_emit = semi + 1;
        if (last_emit < n && src[last_emit] == '\n') last_emit++;
        i = last_emit;
        changed = 1;
    }

    if (!changed) {
        free(out);
        return NULL;
    }
    if (last_emit < n) {
        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    }
    return out;
}

/* Legacy compatibility wrapper. Builtin arena/nursery declaration lowering has
   moved to registered type-owned `@create` hooks; keep only the expression-form
   ownership annotation rewrite here (`Type x = expr @destroy/@detach;`). */
char* cc_rewrite_nursery_create_destroy_proto_ex(const char* src, size_t n, const char* input_path, CCSymbolTable* symbols) {
    (void)symbols;
    return cc__rewrite_builtin_owned_decl_annotations(src, n, input_path);
}

char* cc_rewrite_nursery_create_destroy_proto(const char* src, size_t n, const char* input_path) {
    return cc_rewrite_nursery_create_destroy_proto_ex(src, n, input_path, NULL);
}

/* Rewrite `@match { case <hdr>: <body> ... }` into valid C using cc_chan_match_select.
   This is intentionally text-based: the construct is not valid C, so TCC must see rewritten code.

   Supported (initial subset):
     - `case <rx>.recv(<out_ptr>): { ... }`
     - `case <tx>.send(<value_expr>): { ... }`
     - `case is_cancelled(): { ... }`  (routes immediately if cancelled)

   Notes:
     - In @async code, the emitted `cc_chan_match_select(...)` call will be auto-wrapped by the
       autoblock pass into an `await cc_run_blocking_task_intptr(...)`, making it cooperative. */
static char* cc__rewrite_match_syntax(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return NULL;

    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    size_t last_emit = 0;

    CCScannerState scanner;
    cc_scanner_init(&scanner);
    int line = 1;
    int col = 1;
    unsigned long counter = 0;

    while (i < n) {
        char c = src[i];
        if (c == '\n') { line++; col = 1; }

        /* Skip comments and strings using helper */
        size_t old_i = i;
        if (cc_scanner_skip_non_code(&scanner, src, n, &i)) {
            col += (int)(i - old_i);
            continue;
        }

        /* Look for "@match" */
        if (c == '@') {
            size_t j = i + 1;
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\r' || src[j] == '\n')) j++;
            const char* kw = "match";
            if (j + 5 <= n && memcmp(src + j, kw, 5) == 0) {
                char after = (j + 5 < n) ? src[j + 5] : 0;
                if (!after || !cc_is_ident_char(after)) {
                    size_t k = j + 5;
                    while (k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\r' || src[k] == '\n')) k++;
                    if (k >= n || src[k] != '{') { i++; col++; continue; }

                    /* Find matching '}' for @match block. */
                    size_t body_s = k; /* points at '{' */
                    int br = 1;
                    int in_s2 = 0, in_lc2 = 0, in_bc2 = 0;
                    char q2 = 0;
                    size_t m = body_s + 1;
                    for (; m < n; m++) {
                        char ch = src[m];
                        char ch2 = (m + 1 < n) ? src[m + 1] : 0;
                        if (in_lc2) { if (ch == '\n') in_lc2 = 0; continue; }
                        if (in_bc2) { if (ch == '*' && ch2 == '/') { in_bc2 = 0; m++; } continue; }
                        if (in_s2) { if (ch == '\\' && m + 1 < n) { m++; continue; } if (ch == q2) in_s2 = 0; continue; }
                        if (ch == '/' && ch2 == '/') { in_lc2 = 1; m++; continue; }
                        if (ch == '/' && ch2 == '*') { in_bc2 = 1; m++; continue; }
                        if (ch == '"' || ch == '\'') { in_s2 = 1; q2 = ch; continue; }
                        if (ch == '{') br++;
                        else if (ch == '}') { br--; if (br == 0) break; }
                    }
                    if (m >= n || br != 0) {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                    line, col, "syntax", "unterminated @match block");
                        free(out);
                        return NULL;
                    }
                    size_t body_e = m + 1; /* after '}' */

                    /* Parse cases inside body_s..body_e. */
                    typedef struct {
                        int kind; /* 0=send, 1=recv, 2=cancel */
                        char ch_expr[160];
                        char arg_expr[240];
                        size_t body_off_s;
                        size_t body_off_e;
                    } MatchCase;
                    MatchCase cases[32];
                    int case_n = 0;
                    int cancel_idx = -1;

                    size_t p = body_s + 1;
                    while (p < body_e) {
                        /* skip whitespace */
                        while (p < body_e && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) p++;
                        if (p + 4 >= body_e) break;
                        if (memcmp(src + p, "case", 4) != 0 || (p > 0 && cc_is_ident_char(src[p - 1])) || (p + 4 < n && cc_is_ident_char(src[p + 4]))) {
                            p++;
                            continue;
                        }
                        p += 4;
                        while (p < body_e && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) p++;

                        /* header: until ':' at depth 0 */
                        size_t hdr_s = p;
                        int par = 0, brk2 = 0, br2 = 0;
                        int ins = 0; char qq = 0;
                        size_t hdr_e = (size_t)-1;
                        for (size_t q = p; q < body_e; q++) {
                            char ch = src[q];
                            char ch2 = (q + 1 < body_e) ? src[q + 1] : 0;
                            if (ins) { if (ch == '\\' && q + 1 < body_e) { q++; continue; } if (ch == qq) ins = 0; continue; }
                            if (ch == '"' || ch == '\'') { ins = 1; qq = ch; continue; }
                            if (ch == '/' && ch2 == '/') { while (q < body_e && src[q] != '\n') q++; continue; }
                            if (ch == '/' && ch2 == '*') { q += 2; while (q + 1 < body_e && !(src[q] == '*' && src[q + 1] == '/')) q++; q++; continue; }
                            if (ch == '(') par++;
                            else if (ch == ')') { if (par) par--; }
                            else if (ch == '[') brk2++;
                            else if (ch == ']') { if (brk2) brk2--; }
                            else if (ch == '{') br2++;
                            else if (ch == '}') { if (br2) br2--; }
                            else if (ch == ':' && par == 0 && brk2 == 0 && br2 == 0) { hdr_e = q; break; }
                        }
                        if (hdr_e == (size_t)-1) break;

                        /* body: after ':' */
                        p = hdr_e + 1;
                        while (p < body_e && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) p++;
                        if (p >= body_e) break;
                        size_t cb_s = p;
                        size_t cb_e = (size_t)-1;
                        if (src[p] == '{') {
                            /* balanced block */
                            int brr = 1;
                            int ins2 = 0; char qq2 = 0;
                            int lc = 0, bc = 0;
                            for (size_t q = p + 1; q < body_e; q++) {
                                char ch = src[q];
                                char ch2 = (q + 1 < body_e) ? src[q + 1] : 0;
                                if (lc) { if (ch == '\n') lc = 0; continue; }
                                if (bc) { if (ch == '*' && ch2 == '/') { bc = 0; q++; } continue; }
                                if (ins2) { if (ch == '\\' && q + 1 < body_e) { q++; continue; } if (ch == qq2) ins2 = 0; continue; }
                                if (ch == '/' && ch2 == '/') { lc = 1; q++; continue; }
                                if (ch == '/' && ch2 == '*') { bc = 1; q++; continue; }
                                if (ch == '"' || ch == '\'') { ins2 = 1; qq2 = ch; continue; }
                                if (ch == '{') brr++;
                                else if (ch == '}') { brr--; if (brr == 0) { cb_e = q + 1; break; } }
                            }
                        } else {
                            /* single statement until ';' */
                            int par3 = 0, brk3 = 0, br3 = 0;
                            int ins3 = 0; char qq3 = 0;
                            for (size_t q = p; q < body_e; q++) {
                                char ch = src[q];
                                if (ins3) { if (ch == '\\' && q + 1 < body_e) { q++; continue; } if (ch == qq3) ins3 = 0; continue; }
                                if (ch == '"' || ch == '\'') { ins3 = 1; qq3 = ch; continue; }
                                if (ch == '(') par3++;
                                else if (ch == ')') { if (par3) par3--; }
                                else if (ch == '[') brk3++;
                                else if (ch == ']') { if (brk3) brk3--; }
                                else if (ch == '{') br3++;
                                else if (ch == '}') { if (br3) br3--; }
                                else if (ch == ';' && par3 == 0 && brk3 == 0 && br3 == 0) { cb_e = q + 1; break; }
                            }
                        }
                        if (cb_e == (size_t)-1) break;

                        if (case_n >= (int)(sizeof(cases)/sizeof(cases[0]))) break;
                        memset(&cases[case_n], 0, sizeof(cases[case_n]));
                        cases[case_n].body_off_s = cb_s;
                        cases[case_n].body_off_e = cb_e;

                        /* interpret header */
                        {
                            /* trim hdr */
                            while (hdr_s < hdr_e && (src[hdr_s] == ' ' || src[hdr_s] == '\t')) hdr_s++;
                            while (hdr_e > hdr_s && (src[hdr_e - 1] == ' ' || src[hdr_e - 1] == '\t')) hdr_e--;
                            size_t hl = hdr_e > hdr_s ? (hdr_e - hdr_s) : 0;
                            if (hl >= 380) hl = 379;
                            char hdr[380];
                            memcpy(hdr, src + hdr_s, hl);
                            hdr[hl] = 0;

                            if (strncmp(hdr, "is_cancelled()", 14) == 0) {
                                cases[case_n].kind = 2;
                                cancel_idx = case_n;
                            } else {
                                /* find ".recv(" or ".send(" — comment/string-aware,
                                 * so a case header that starts with a block comment
                                 * like `/* recv * / chan.recv(&v)` isn't mis-routed
                                 * by the comment's bytes. */
                                size_t hdr_len_s = strlen(hdr);
                                size_t recv_p = cc_find_substr_top_level(hdr, 0, hdr_len_s, ".recv", 5);
                                size_t send_p = cc_find_substr_top_level(hdr, 0, hdr_len_s, ".send", 5);
                                const char* recv = (recv_p < hdr_len_s) ? (hdr + recv_p) : NULL;
                                const char* send = (send_p < hdr_len_s) ? (hdr + send_p) : NULL;
                                const char* dot = recv ? recv : send;
                                int is_recv = (recv != NULL);
                                int is_send = (send != NULL);
                                if (!dot || (!is_recv && !is_send)) {
                                    char rel[1024];
                                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                                line, col, "syntax", "@match case header must be <chan>.recv(ptr) or <chan>.send(value) or is_cancelled()");
                                    free(out);
                                    return NULL;
                                }
                                /* channel expr */
                                size_t cn = (size_t)(dot - hdr);
                                while (cn > 0 && (hdr[cn - 1] == ' ' || hdr[cn - 1] == '\t')) cn--;
                                if (cn >= sizeof(cases[case_n].ch_expr)) cn = sizeof(cases[case_n].ch_expr) - 1;
                                memcpy(cases[case_n].ch_expr, hdr, cn);
                                cases[case_n].ch_expr[cn] = 0;

                                size_t dot_off = (size_t)(dot - hdr);
                                size_t dot_len = hdr_len_s - dot_off;
                                size_t lp_off = cc_find_char_top_level(dot, 0, dot_len, '(');
                                const char* lp = (lp_off < dot_len) ? (dot + lp_off) : NULL;
                                const char* rp = lp ? strrchr(dot, ')') : NULL;
                                if (!lp || !rp || rp <= lp) {
                                    char rel[1024];
                                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                                line, col, "syntax", "malformed @match case header");
                                    free(out);
                                    return NULL;
                                }
                                size_t an = (size_t)(rp - (lp + 1));
                                while (an > 0 && ((lp + 1)[0] == ' ' || (lp + 1)[0] == '\t')) { lp++; an--; }
                                while (an > 0 && ((lp + 1)[an - 1] == ' ' || (lp + 1)[an - 1] == '\t')) an--;
                                if (an >= sizeof(cases[case_n].arg_expr)) an = sizeof(cases[case_n].arg_expr) - 1;
                                memcpy(cases[case_n].arg_expr, lp + 1, an);
                                cases[case_n].arg_expr[an] = 0;

                                cases[case_n].kind = is_send ? 0 : 1;
                            }
                        }

                        case_n++;
                        p = cb_e;
                    }

                    if (case_n == 0) { i++; col++; continue; }

                    counter++;
                    char pro[4096];
                    size_t pn = 0;
                    pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                           "do { /* @match */\n"
                                           "  size_t __cc_match_idx_%lu = (size_t)-1;\n"
                                           "  int __cc_match_rc_%lu = 0;\n",
                                           counter, counter);

                    /* Prepare send temps and build cases array */
                    pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                           "  CCChanMatchCase __cc_match_cases_%lu[%d];\n",
                                           counter, case_n);
                    for (int ci = 0; ci < case_n; ci++) {
                        if (cases[ci].kind == 0) {
                            pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                                   "  __typeof__(%s) __cc_match_v_%lu_%d = (%s);\n"
                                                   "  __cc_match_cases_%lu[%d] = (CCChanMatchCase){ .ch = (%s).raw, .send_buf = &__cc_match_v_%lu_%d, .recv_buf = NULL, .elem_size = sizeof(__cc_match_v_%lu_%d), .is_send = true };\n",
                                                   cases[ci].arg_expr, counter, ci, cases[ci].arg_expr,
                                                   counter, ci, cases[ci].ch_expr, counter, ci, counter, ci);
                        } else if (cases[ci].kind == 1) {
                            pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                                   "  __cc_match_cases_%lu[%d] = (CCChanMatchCase){ .ch = (%s).raw, .send_buf = NULL, .recv_buf = (void*)(%s), .elem_size = sizeof(*(%s)), .is_send = false };\n",
                                                   counter, ci, cases[ci].ch_expr, cases[ci].arg_expr, cases[ci].arg_expr);
                        } else {
                            /* cancelled() doesn't touch channels; leave entry unused */
                            pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                                   "  __cc_match_cases_%lu[%d] = (CCChanMatchCase){0};\n",
                                                   counter, ci);
                        }
                    }

                    /* Select */
                    if (cancel_idx >= 0) {
                        pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                               "  if (cc_is_cancelled()) {\n"
                                               "    __cc_match_idx_%lu = %d;\n"
                                               "  } else {\n"
                                               "    __cc_match_rc_%lu = cc_chan_match_select(__cc_match_cases_%lu, %d, &__cc_match_idx_%lu, cc_current_deadline());\n"
                                               "  }\n",
                                               counter, cancel_idx,
                                               counter, counter, case_n, counter);
                    } else {
                        pn += (size_t)snprintf(pro + pn, sizeof(pro) - pn,
                                               "  __cc_match_rc_%lu = cc_chan_match_select(__cc_match_cases_%lu, %d, &__cc_match_idx_%lu, cc_current_deadline());\n",
                                               counter, counter, case_n, counter);
                    }

                    (void)pn; /* best-effort; if truncated, compilation will fail and point here */

                    /* Emit prefix up to @match, then prologue */
                    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                    cc_sb_append(&out, &out_len, &out_cap, pro, strlen(pro));

                    /* switch */
                    char sw[256];
                    snprintf(sw, sizeof(sw),
                             "  switch (__cc_match_idx_%lu) {\n",
                             counter);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, sw);
                    for (int ci = 0; ci < case_n; ci++) {
                        char cs[64];
                        snprintf(cs, sizeof(cs), "    case %d:\n", ci);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, cs);
                        cc_sb_append(&out, &out_len, &out_cap, src + cases[ci].body_off_s, cases[ci].body_off_e - cases[ci].body_off_s);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "\n      break;\n");
                    }
                    cc_sb_append_cstr(&out, &out_len, &out_cap,
                                       "    default: break;\n"
                                       "  }\n"
                                       "  (void)__cc_match_rc_");
                    char suf[64];
                    snprintf(suf, sizeof(suf), "%lu;\n", counter);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, suf);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "} while(0);\n");

                    last_emit = body_e;
                    i = body_e;
                    continue;
                }
            }
        }

        i++; col++;
    }

    if (last_emit == 0) return NULL;
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Canonicalize `@with_deadline(expr) { ... }` to `with_deadline(expr) { ... }`
   without otherwise lowering the construct. This is phase-1 CC normalization:
   the scope remains part of canonical CC and is lowered later. */
static char* cc__canonicalize_with_deadline_syntax(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    int in_str = 0;
    int in_chr = 0;

    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;

        if (in_line_comment) {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            if (c == '\n') in_line_comment = 0;
            i++;
            continue;
        }
        if (in_block_comment) {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            if (c == '*' && c2 == '/') {
                cc_sb_append(&out, &out_len, &out_cap, &c2, 1);
                i += 2;
                in_block_comment = 0;
                continue;
            }
            i++;
            continue;
        }
        if (in_str) {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            if (c == '\\' && i + 1 < n) {
                cc_sb_append(&out, &out_len, &out_cap, &c2, 1);
                i += 2;
                continue;
            }
            if (c == '"') in_str = 0;
            i++;
            continue;
        }
        if (in_chr) {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            if (c == '\\' && i + 1 < n) {
                cc_sb_append(&out, &out_len, &out_cap, &c2, 1);
                i += 2;
                continue;
            }
            if (c == '\'') in_chr = 0;
            i++;
            continue;
        }

        if (c == '/' && c2 == '/') {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            cc_sb_append(&out, &out_len, &out_cap, &c2, 1);
            i += 2;
            in_line_comment = 1;
            continue;
        }
        if (c == '/' && c2 == '*') {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            cc_sb_append(&out, &out_len, &out_cap, &c2, 1);
            i += 2;
            in_block_comment = 1;
            continue;
        }
        if (c == '"') {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            i++;
            in_str = 1;
            continue;
        }
        if (c == '\'') {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            i++;
            in_chr = 1;
            continue;
        }

        if (c == '@') {
            size_t j = i + 1;
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\r' || src[j] == '\n')) j++;
            const char* kw = "with_deadline";
            size_t kw_len = strlen(kw);
            if (j + kw_len <= n && memcmp(src + j, kw, kw_len) == 0) {
                char after = (j + kw_len < n) ? src[j + kw_len] : 0;
                if (!after || !cc_is_ident_char(after)) {
                    i = j;
                    continue;
                }
            }
        }

        cc_sb_append(&out, &out_len, &out_cap, &c, 1);
        i++;
    }

    return out;
}

/* Lower canonical `with_deadline(expr) { ... }` into:
     { CCDeadline __cc_dlN = cc_deadline_after_ms((uint64_t)(expr));
       CCDeadline* __cc_prevN = cc_deadline_push(&__cc_dlN);
       @defer cc_deadline_pop(__cc_prevN);
       { ... } }

   This is phase-3 lowering: canonical CC no longer contains the `@` alias,
   but host-facing parsing still needs the scope expanded into runtime
   scaffolding. */
static char* cc__lower_with_deadline_syntax(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    int in_str = 0;
    int in_chr = 0;
    unsigned long counter = 0;

    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;

        if (in_line_comment) {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            if (c == '\n') in_line_comment = 0;
            i++;
            continue;
        }
        if (in_block_comment) {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            if (c == '*' && c2 == '/') {
                cc_sb_append(&out, &out_len, &out_cap, &c2, 1);
                i += 2;
                in_block_comment = 0;
                continue;
            }
            i++;
            continue;
        }
        if (in_str) {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            if (c == '\\' && i + 1 < n) {
                cc_sb_append(&out, &out_len, &out_cap, &c2, 1);
                i += 2;
                continue;
            }
            if (c == '"') in_str = 0;
            i++;
            continue;
        }
        if (in_chr) {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            if (c == '\\' && i + 1 < n) {
                cc_sb_append(&out, &out_len, &out_cap, &c2, 1);
                i += 2;
                continue;
            }
            if (c == '\'') in_chr = 0;
            i++;
            continue;
        }

        if (c == '/' && c2 == '/') {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            cc_sb_append(&out, &out_len, &out_cap, &c2, 1);
            i += 2;
            in_line_comment = 1;
            continue;
        }
        if (c == '/' && c2 == '*') {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            cc_sb_append(&out, &out_len, &out_cap, &c2, 1);
            i += 2;
            in_block_comment = 1;
            continue;
        }
        if (c == '"') {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            i++;
            in_str = 1;
            continue;
        }
        if (c == '\'') {
            cc_sb_append(&out, &out_len, &out_cap, &c, 1);
            i++;
            in_chr = 1;
            continue;
        }

        if (cc_is_ident_start(c)) {
            size_t s0 = i;
            i++;
            while (i < n && cc_is_ident_char(src[i])) i++;
            size_t sl = i - s0;
            int is_wd = (sl == strlen("with_deadline") && memcmp(src + s0, "with_deadline", sl) == 0);
            if (!is_wd) {
                cc_sb_append(&out, &out_len, &out_cap, src + s0, sl);
                continue;
            }
            /* Ensure token boundary before. */
            if (s0 > 0 && cc_is_ident_char(src[s0 - 1])) {
                cc_sb_append(&out, &out_len, &out_cap, src + s0, sl);
                continue;
            }

            /* Skip whitespace then expect '(' */
            size_t j = i;
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\r' || src[j] == '\n')) j++;
            if (j >= n || src[j] != '(') {
                /* Just an identifier occurrence. */
                cc_sb_append(&out, &out_len, &out_cap, src + s0, sl);
                i = j;
                continue;
            }
            size_t expr_l = j + 1;
            int par = 1;
            int in_s2 = 0, in_lc2 = 0, in_bc2 = 0;
            char q2 = 0;
            size_t k = expr_l;
            for (; k < n; k++) {
                char ch = src[k];
                char ch2 = (k + 1 < n) ? src[k + 1] : 0;
                if (in_lc2) { if (ch == '\n') in_lc2 = 0; continue; }
                if (in_bc2) { if (ch == '*' && ch2 == '/') { in_bc2 = 0; k++; } continue; }
                if (in_s2) {
                    if (ch == '\\' && k + 1 < n) { k++; continue; }
                    if (ch == q2) in_s2 = 0;
                    continue;
                }
                if (ch == '/' && ch2 == '/') { in_lc2 = 1; k++; continue; }
                if (ch == '/' && ch2 == '*') { in_bc2 = 1; k++; continue; }
                if (ch == '"' || ch == '\'') { in_s2 = 1; q2 = ch; continue; }
                if (ch == '(') par++;
                else if (ch == ')') {
                    par--;
                    if (par == 0) break;
                }
            }
            if (k >= n || par != 0) {
                /* Give up; emit original token. */
                cc_sb_append(&out, &out_len, &out_cap, src + s0, sl);
                i = j;
                continue;
            }
            size_t expr_r = k; /* points at ')' */
            size_t after_paren = expr_r + 1;
            while (after_paren < n && (src[after_paren] == ' ' || src[after_paren] == '\t' || src[after_paren] == '\r' || src[after_paren] == '\n')) after_paren++;

            /* Optional "as <ident>" clause: binds CCDeadline* <ident> inside
               the block so user code can inspect the active deadline. */
            const char* as_ident = NULL;
            size_t as_ident_len = 0;
            if (after_paren + 2 < n && src[after_paren] == 'a' && src[after_paren + 1] == 's' &&
                (after_paren + 2 >= n || !cc_is_ident_char(src[after_paren + 2]))) {
                size_t cur = after_paren + 2;
                while (cur < n && (src[cur] == ' ' || src[cur] == '\t' || src[cur] == '\r' || src[cur] == '\n')) cur++;
                if (cur < n && cc_is_ident_start(src[cur])) {
                    size_t id_s = cur;
                    cur++;
                    while (cur < n && cc_is_ident_char(src[cur])) cur++;
                    as_ident = src + id_s;
                    as_ident_len = cur - id_s;
                    while (cur < n && (src[cur] == ' ' || src[cur] == '\t' || src[cur] == '\r' || src[cur] == '\n')) cur++;
                    after_paren = cur;
                }
            }

            if (after_paren >= n || src[after_paren] != '{') {
                /* Not a block form; emit original token sequence. */
                cc_sb_append(&out, &out_len, &out_cap, src + s0, sl);
                i = j;
                continue;
            }
            size_t body_s = after_paren;
            int br = 1;
            int in_s3 = 0, in_lc3 = 0, in_bc3 = 0;
            char q3 = 0;
            size_t m = body_s + 1;
            for (; m < n; m++) {
                char ch = src[m];
                char ch2 = (m + 1 < n) ? src[m + 1] : 0;
                if (in_lc3) { if (ch == '\n') in_lc3 = 0; continue; }
                if (in_bc3) { if (ch == '*' && ch2 == '/') { in_bc3 = 0; m++; } continue; }
                if (in_s3) {
                    if (ch == '\\' && m + 1 < n) { m++; continue; }
                    if (ch == q3) in_s3 = 0;
                    continue;
                }
                if (ch == '/' && ch2 == '/') { in_lc3 = 1; m++; continue; }
                if (ch == '/' && ch2 == '*') { in_bc3 = 1; m++; continue; }
                if (ch == '"' || ch == '\'') { in_s3 = 1; q3 = ch; continue; }
                if (ch == '{') br++;
                else if (ch == '}') {
                    br--;
                    if (br == 0) { m++; break; }
                }
            }
            if (m > n || br != 0) {
                cc_sb_append(&out, &out_len, &out_cap, src + s0, sl);
                i = j;
                continue;
            }
            size_t body_e = m; /* points just after '}' */

            counter++;
            char hdr[768];
            if (as_ident) {
                snprintf(hdr, sizeof(hdr),
                         "{ CCDeadline __cc_dl%lu = cc_deadline_after_ms((uint64_t)(%.*s)); "
                         "CCDeadline* %.*s = &__cc_dl%lu; "
                         "CCDeadline* __cc_prev%lu = cc_deadline_push(%.*s); "
                         "@defer cc_deadline_pop(__cc_prev%lu); ",
                         counter,
                         (int)(expr_r - expr_l), src + expr_l,
                         (int)as_ident_len, as_ident, counter,
                         counter,
                         (int)as_ident_len, as_ident,
                         counter);
            } else {
                snprintf(hdr, sizeof(hdr),
                         "{ CCDeadline __cc_dl%lu = cc_deadline_after_ms((uint64_t)(%.*s)); "
                         "CCDeadline* __cc_prev%lu = cc_deadline_push(&__cc_dl%lu); "
                         "@defer cc_deadline_pop(__cc_prev%lu); ",
                         counter,
                         (int)(expr_r - expr_l), src + expr_l,
                         counter, counter, counter);
            }
            cc_sb_append_cstr(&out, &out_len, &out_cap, hdr);
            cc_sb_append(&out, &out_len, &out_cap, src + body_s, body_e - body_s);
            cc_sb_append_cstr(&out, &out_len, &out_cap, " }");

            i = body_e;
            continue;
        }

        /* default: copy */
        cc_sb_append(&out, &out_len, &out_cap, &c, 1);
        i++;
    }

    return out;
}

static size_t cc__skip_leading_decl_specs(const char* s, size_t ty_start);

/* Scan backward from `from` in `s` looking for the start of a type
 * prefix — i.e. the position right after the most-recent decl-boundary
 * char at paren-depth 0.  Delimits on `; { } , < \n` (plus a lone
 * opening `(`, which closes a parenthesized declarator).
 *
 * Pre-metaclass this walked backward byte-by-byte with no awareness of
 * comments or string literals, so a `;` or `(` inside a `// ... \n` or
 * `/ * ... * /` block upstream of the type position could stop the
 * scan inside a comment and pull comment bytes into the type prefix.
 * Route through `cc_rfind_char_top_level` (util/text.h) which does a
 * forward prepass to label comment/string bytes, then walks backward
 * over code bytes only with matching bracket semantics. */
static size_t cc__scan_back_to_delim(const char* s, size_t from) {
    if (!s) return from;
    return cc_rfind_char_top_level(s, 0, from, ";{},<\n");
}

static void cc__sb_append_fmt_local(char** out,
                                    size_t* out_len,
                                    size_t* out_cap,
                                    const char* fmt,
                                    ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    cc_sb_append(out, out_len, out_cap, buf, (size_t)n);
}

static int cc__is_template_tag_start(const char* src, size_t n, size_t pos) {
    return pos < n && (cc_is_ident_start(src[pos]) || src[pos] == '_');
}

/* Odd run of '\\' immediately before '$' => that '$' is literal (`\${` -> `${` in output). */
static int cc__is_escaped_dollar(const char* src, size_t body_s, size_t dollar_pos) {
    if (!src || dollar_pos < body_s || src[dollar_pos] != '$') return 0;
    size_t k = dollar_pos;
    int bs = 0;
    while (k > body_s && src[k - 1] == '\\') {
        bs++;
        k--;
    }
    return (bs % 2) == 1;
}

static int cc__scan_interp_body(const char* src,
                                size_t n,
                                size_t brace_pos,
                                size_t* body_end_out) {
    size_t i = brace_pos + 1;
    int brace_depth = 1;
    int paren_depth = 0;
    int bracket_depth = 0;
    int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
    if (!src || !body_end_out || brace_pos >= n || src[brace_pos] != '{') return -1;
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (in_line_comment) {
            if (c == '\n') in_line_comment = 0;
            i++;
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && c2 == '/') {
                in_block_comment = 0;
                i += 2;
                continue;
            }
            i++;
            continue;
        }
        if (in_str) {
            if (c == '\\' && i + 1 < n) {
                i += 2;
                continue;
            }
            if (c == '"') in_str = 0;
            i++;
            continue;
        }
        if (in_chr) {
            if (c == '\\' && i + 1 < n) {
                i += 2;
                continue;
            }
            if (c == '\'') in_chr = 0;
            i++;
            continue;
        }
        if (c == '/' && c2 == '/') {
            in_line_comment = 1;
            i += 2;
            continue;
        }
        if (c == '/' && c2 == '*') {
            in_block_comment = 1;
            i += 2;
            continue;
        }
        if (c == '"') {
            in_str = 1;
            i++;
            continue;
        }
        if (c == '\'') {
            in_chr = 1;
            i++;
            continue;
        }
        if (c == '{') {
            brace_depth++;
            i++;
            continue;
        }
        if (c == '}') {
            brace_depth--;
            if (brace_depth == 0 && paren_depth == 0 && bracket_depth == 0) {
                *body_end_out = i;
                return 0;
            }
            i++;
            continue;
        }
        if (c == '(') paren_depth++;
        else if (c == ')' && paren_depth > 0) paren_depth--;
        else if (c == '[') bracket_depth++;
        else if (c == ']' && bracket_depth > 0) bracket_depth--;
        i++;
    }
    return -1;
}

static int cc__scan_template_literal(const char* src,
                                     size_t n,
                                     size_t tick_pos,
                                     size_t* tick_end_out) {
    size_t i = tick_pos + 1;
    if (!src || !tick_end_out || tick_pos >= n || src[tick_pos] != '`') return -1;
    while (i < n) {
        char c = src[i];
        if (c == '`') {
            *tick_end_out = i;
            return 0;
        }
        if (c == '$' && i + 1 < n) {
            size_t brace_pos = (size_t)-1;
            if (src[i + 1] == '{') {
                brace_pos = i + 1;
            } else if (i + 2 < n && src[i + 1] == '~' &&
                       cc__is_template_tag_start(src, n, i + 2)) {
                size_t t = i + 2;
                while (t < n && cc_is_ident_char(src[t])) t++;
                if (t < n && src[t] == '{') brace_pos = t;
            }
            if (brace_pos != (size_t)-1 &&
                !cc__is_escaped_dollar(src, tick_pos + 1, i)) {
                size_t body_end = 0;
                if (cc__scan_interp_body(src, n, brace_pos, &body_end) != 0) return -1;
                i = body_end + 1;
                continue;
            }
        }
        i++;
    }
    return -1;
}

static size_t cc__scan_to_top_level_delim(const char* src,
                                          size_t n,
                                          size_t start,
                                          char delim1,
                                          char delim2) {
    size_t i = start;
    int paren_depth = 0, brace_depth = 0, bracket_depth = 0;
    int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (in_line_comment) {
            if (c == '\n') in_line_comment = 0;
            i++;
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && c2 == '/') {
                in_block_comment = 0;
                i += 2;
                continue;
            }
            i++;
            continue;
        }
        if (in_str) {
            if (c == '\\' && i + 1 < n) {
                i += 2;
                continue;
            }
            if (c == '"') in_str = 0;
            i++;
            continue;
        }
        if (in_chr) {
            if (c == '\\' && i + 1 < n) {
                i += 2;
                continue;
            }
            if (c == '\'') in_chr = 0;
            i++;
            continue;
        }
        if (c == '/' && c2 == '/') {
            in_line_comment = 1;
            i += 2;
            continue;
        }
        if (c == '/' && c2 == '*') {
            in_block_comment = 1;
            i += 2;
            continue;
        }
        if (c == '"') {
            in_str = 1;
            i++;
            continue;
        }
        if (c == '\'') {
            in_chr = 1;
            i++;
            continue;
        }
        if (c == '`') {
            size_t tick_end = 0;
            if (cc__scan_template_literal(src, n, i, &tick_end) != 0) return n;
            i = tick_end + 1;
            continue;
        }
        if (c == '(') paren_depth++;
        else if (c == ')') {
            if (paren_depth == 0 && brace_depth == 0 && bracket_depth == 0 &&
                (c == delim1 || c == delim2)) {
                return i;
            }
            if (paren_depth > 0) paren_depth--;
        } else if (c == '{') brace_depth++;
        else if (c == '}') {
            if (brace_depth > 0) brace_depth--;
        } else if (c == '[') bracket_depth++;
        else if (c == ']') {
            if (bracket_depth > 0) bracket_depth--;
        }
        if (paren_depth == 0 && brace_depth == 0 && bracket_depth == 0 &&
            (c == delim1 || c == delim2)) {
            return i;
        }
        i++;
    }
    return n;
}

static void cc__append_c_string_escaped(char** out,
                                        size_t* out_len,
                                        size_t* out_cap,
                                        const char* src,
                                        size_t len) {
    cc_sb_append(out, out_len, out_cap, "\"", 1);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\\' && i + 1 < len) {
            unsigned char next = (unsigned char)src[i + 1];
            switch (next) {
                case 'n': cc_sb_append(out, out_len, out_cap, "\\n", 2); i++; continue;
                case 'r': cc_sb_append(out, out_len, out_cap, "\\r", 2); i++; continue;
                case 't': cc_sb_append(out, out_len, out_cap, "\\t", 2); i++; continue;
                case '\\': cc_sb_append(out, out_len, out_cap, "\\\\", 2); i++; continue;
                case '"': cc_sb_append(out, out_len, out_cap, "\\\"", 2); i++; continue;
                case '`': cc_sb_append(out, out_len, out_cap, "`", 1); i++; continue;
                case '$': cc_sb_append(out, out_len, out_cap, "$", 1); i++; continue;
                default:
                    break;
            }
        }
        if (c == '\\') cc_sb_append(out, out_len, out_cap, "\\\\", 2);
        else if (c == '"') cc_sb_append(out, out_len, out_cap, "\\\"", 2);
        else if (c == '\n') cc_sb_append(out, out_len, out_cap, "\\n", 2);
        else if (c == '\r') cc_sb_append(out, out_len, out_cap, "\\r", 2);
        else if (c == '\t') cc_sb_append(out, out_len, out_cap, "\\t", 2);
        else cc_sb_append(out, out_len, out_cap, (const char*)&c, 1);
    }
    cc_sb_append(out, out_len, out_cap, "\"", 1);
}

static size_t cc__template_literal_decoded_len(const char* src, size_t len) {
    size_t out = 0;
    size_t i = 0;
    if (!src) return 0;
    while (i < len) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\\' && i + 1 < len) {
            unsigned char next = (unsigned char)src[i + 1];
            switch (next) {
                case 'n':
                case 'r':
                case 't':
                case '\\':
                case '"':
                case '`':
                case '$':
                    out++;
                    i += 2;
                    continue;
                default:
                    break;
            }
        }
        out++;
        i++;
    }
    return out;
}

static void cc__emit_template_literal_push(char** out,
                                           size_t* out_len,
                                           size_t* out_cap,
                                           const char* builder_name,
                                           const char* arena_name,
                                           const char* src,
                                           size_t len) {
    size_t decoded_len;
    if (!builder_name || !src || len == 0) return;
    decoded_len = cc__template_literal_decoded_len(src, len);
    cc__sb_append_fmt_local(out, out_len, out_cap,
                            "cc_string_push_buffer(&%s, ",
                            builder_name);
    cc__append_c_string_escaped(out, out_len, out_cap, src, len);
    cc__sb_append_fmt_local(out, out_len, out_cap, ", %zu, ", decoded_len);
    cc_sb_append_cstr(out, out_len, out_cap, arena_name);
    cc_sb_append_cstr(out, out_len, out_cap, "); ");
}

static int cc__rewrite_template_body(char** out,
                                     size_t* out_len,
                                     size_t* out_cap,
                                     const char* src,
                                     size_t n,
                                     size_t body_s,
                                     size_t body_e,
                                     const char* builder_name,
                                     const char* policy_name,
                                     const char* arena_name) {
    size_t i = body_s;
    size_t lit_start = body_s;
    while (i < body_e) {
        if (src[i] == '$' && i + 1 < body_e) {
            size_t tag_s = 0, tag_e = 0, brace_pos = 0;
            int tagged = 0;
            int ok = 0;
            if (src[i + 1] == '{') {
                brace_pos = i + 1;
                ok = 1;
            } else if (i + 2 < body_e && src[i + 1] == '~' &&
                       cc__is_template_tag_start(src, body_e, i + 2)) {
                size_t t = i + 2;
                while (t < body_e && cc_is_ident_char(src[t])) t++;
                if (t < body_e && src[t] == '{') {
                    tagged = 1;
                    tag_s = i + 2;
                    tag_e = t;
                    brace_pos = t;
                    ok = 1;
                }
            }
            if (ok && cc__is_escaped_dollar(src, body_s, i)) ok = 0;
            if (!ok) {
                i++;
                continue;
            }
            if (lit_start < i) {
                cc__emit_template_literal_push(out, out_len, out_cap, builder_name, arena_name, src + lit_start, i - lit_start);
            }
            {
                size_t expr_end = 0;
                if (cc__scan_interp_body(src, n, brace_pos, &expr_end) != 0) return -1;
                if (policy_name && policy_name[0]) {
                    cc__sb_append_fmt_local(out, out_len, out_cap,
                                            "cc_string_push_policy(&%s, %s, %s, ",
                                            builder_name, policy_name, arena_name);
                    if (tagged) {
                        cc_sb_append_cstr(out, out_len, out_cap, "cc_slice_from_cstr(");
                        cc__append_c_string_escaped(out, out_len, out_cap, src + tag_s, tag_e - tag_s);
                        cc_sb_append_cstr(out, out_len, out_cap, "), ");
                    } else {
                        cc_sb_append_cstr(out, out_len, out_cap, "cc_slice_empty(), ");
                    }
                    cc_sb_append_cstr(out, out_len, out_cap, "cc__string_slot_arg((");
                    cc_sb_append(out, out_len, out_cap, src + brace_pos + 1, expr_end - (brace_pos + 1));
                    cc_sb_append_cstr(out, out_len, out_cap, "), ");
                    cc_sb_append_cstr(out, out_len, out_cap, arena_name);
                    cc_sb_append_cstr(out, out_len, out_cap, ")); ");
                } else {
                    if (tagged) return -2;
                    cc__sb_append_fmt_local(out, out_len, out_cap,
                                            "cc__string_slot_push(&%s, (",
                                            builder_name);
                    cc_sb_append(out, out_len, out_cap, src + brace_pos + 1, expr_end - (brace_pos + 1));
                    cc_sb_append_cstr(out, out_len, out_cap, "), ");
                    cc_sb_append_cstr(out, out_len, out_cap, arena_name);
                    cc_sb_append_cstr(out, out_len, out_cap, "); ");
                }
                i = expr_end + 1;
                lit_start = i;
                continue;
            }
        }
        i++;
    }
    if (lit_start < body_e) {
        cc__emit_template_literal_push(out, out_len, out_cap, builder_name, arena_name, src + lit_start, body_e - lit_start);
    }
    return 0;
}

static char* cc__rewrite_string_templates(const char* src, size_t n, const char* input_path) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0, last_emit = 0;
    int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
    int line = 1, col = 1;
    int rewrite_count = 0;
    if (!src || n == 0) return NULL;
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (c == '\n') { line++; col = 1; }
        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; if (c != '\n') col++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; col += 2; continue; } i++; col++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; col += 2; continue; } if (c == '"') in_str = 0; i++; col++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; col += 2; continue; } if (c == '\'') in_chr = 0; i++; col++; continue; }
        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; col += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; col += 2; continue; }
        if (c == '"') { in_str = 1; i++; col++; continue; }
        if (c == '\'') { in_chr = 1; i++; col++; continue; }
        if (c == '@' && i + 6 < n && memcmp(src + i, "@slice(", 7) == 0) {
            size_t arg_s = cc_skip_ws_and_comments(src, n, i + 7);
            size_t arg_e = cc__scan_to_top_level_delim(src, n, arg_s, ')', '\0');
            if (arg_e >= n || src[arg_e] != ')') {
                char rel[1024];
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                line, col, "syntax", "unterminated @slice(...)");
                free(out);
                return (char*)-1;
            }
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
            cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_slice_from_cstr(");
            cc_sb_append(&out, &out_len, &out_cap, src + arg_s, arg_e - arg_s);
            cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
            rewrite_count++;
            last_emit = arg_e + 1;
            i = arg_e + 1;
            continue;
        }
        if (c == '@' && i + 7 < n && memcmp(src + i, "@string(", 8) == 0) {
            size_t arg1_s = cc_skip_ws_and_comments(src, n, i + 8);
            size_t arg1_e = cc__scan_to_top_level_delim(src, n, arg1_s, ',', ')');
            if (arg1_e >= n) {
                char rel[1024];
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                line, col, "syntax", "unterminated @string(...)");
                free(out);
                return (char*)-1;
            }
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
            if (src[arg1_e] == ')') {
                char rel[1024];
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                line, col, "syntax", "@string(...) requires an arena argument");
                free(out);
                return (char*)-1;
            }
            {
                if (arg1_s < n && src[arg1_s] == '`') {
                    size_t tick_e = 0;
                    size_t arg2_s, arg2_e;
                    char builder_name[64], arena_name[64];
                    if (cc__scan_template_literal(src, n, arg1_s, &tick_e) != 0) {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                        line, col, "syntax", "unterminated template literal in @string(...)");
                        free(out);
                        return (char*)-1;
                    }
                    arg2_s = cc_skip_ws_and_comments(src, n, tick_e + 1);
                    if (arg2_s >= n || src[arg2_s] != ',') {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                        line, col, "syntax", "@string(`...`, arena) requires a trailing arena argument");
                        free(out);
                        return (char*)-1;
                    }
                    arg2_s = cc_skip_ws_and_comments(src, n, arg2_s + 1);
                    arg2_e = cc__scan_to_top_level_delim(src, n, arg2_s, ')', '\0');
                    if (arg2_e >= n || src[arg2_e] != ')') {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                        line, col, "syntax", "unterminated @string(`...`, arena)");
                        free(out);
                        return (char*)-1;
                    }
                    snprintf(builder_name, sizeof(builder_name), "__cc_tpl_%d", rewrite_count);
                    snprintf(arena_name, sizeof(arena_name), "__cc_tpl_arena_%d", rewrite_count);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "({ ");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "CCArena* ");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, arena_name);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, " = (");
                    cc_sb_append(&out, &out_len, &out_cap, src + arg2_s, arg2_e - arg2_s);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "); ");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "CCString ");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, builder_name);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, " = cc_string_new(); ");
                    {
                        int tpl_rc = cc__rewrite_template_body(&out, &out_len, &out_cap, src, n,
                                                               arg1_s + 1, tick_e, builder_name,
                                                               NULL, arena_name);
                        if (tpl_rc != 0) {
                            char rel[1024];
                            const char* msg = (tpl_rc == -2)
                                ? "tagged template slots require @string(policy, `...`, arena)"
                                : "unterminated interpolation in @string template";
                            cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                            line, col, "syntax", msg);
                            free(out);
                            return (char*)-1;
                        }
                    }
                    cc_sb_append_cstr(&out, &out_len, &out_cap, builder_name);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "; })");
                    rewrite_count++;
                    last_emit = arg2_e + 1;
                    i = arg2_e + 1;
                    continue;
                }
                size_t arg2_s = cc_skip_ws_and_comments(src, n, arg1_e + 1);
                if (arg2_s < n && src[arg2_s] == '`') {
                    size_t tick_e = 0;
                    size_t arg3_s, arg3_e;
                    char builder_name[64], policy_name[64], arena_name[64];
                    if (cc__scan_template_literal(src, n, arg2_s, &tick_e) != 0) {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                        line, col, "syntax", "unterminated template literal in @string(...)");
                        free(out);
                        return (char*)-1;
                    }
                    arg3_s = cc_skip_ws_and_comments(src, n, tick_e + 1);
                    if (arg3_s >= n || src[arg3_s] != ',') {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                        line, col, "syntax", "@string(policy, `...`, arena) requires a trailing arena argument");
                        free(out);
                        return (char*)-1;
                    }
                    arg3_s = cc_skip_ws_and_comments(src, n, arg3_s + 1);
                    arg3_e = cc__scan_to_top_level_delim(src, n, arg3_s, ')', '\0');
                    if (arg3_e >= n || src[arg3_e] != ')') {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                        line, col, "syntax", "unterminated @string(policy, `...`, arena)");
                        free(out);
                        return (char*)-1;
                    }
                    snprintf(builder_name, sizeof(builder_name), "__cc_tpl_%d", rewrite_count);
                    snprintf(policy_name, sizeof(policy_name), "__cc_tpl_policy_%d", rewrite_count);
                    snprintf(arena_name, sizeof(arena_name), "__cc_tpl_arena_%d", rewrite_count);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "({ ");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "CCArena* ");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, arena_name);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, " = (");
                    cc_sb_append(&out, &out_len, &out_cap, src + arg3_s, arg3_e - arg3_s);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "); ");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "CCStringPolicy ");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, policy_name);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, " = (");
                    cc_sb_append(&out, &out_len, &out_cap, src + arg1_s, arg1_e - arg1_s);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "); ");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "CCString ");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, builder_name);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, " = cc_string_new(); ");
                    if (cc__rewrite_template_body(&out, &out_len, &out_cap, src, n,
                                                  arg2_s + 1, tick_e, builder_name,
                                                  policy_name, arena_name) != 0) {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                        line, col, "syntax", "unterminated interpolation in @string template");
                        free(out);
                        return (char*)-1;
                    }
                    cc_sb_append_cstr(&out, &out_len, &out_cap, builder_name);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "; })");
                    rewrite_count++;
                    last_emit = arg3_e + 1;
                    i = arg3_e + 1;
                    continue;
                } else {
                    size_t arg2_e = cc__scan_to_top_level_delim(src, n, arg2_s, ')', '\0');
                    if (arg2_e >= n || src[arg2_e] != ')') {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                        line, col, "syntax", "unterminated @string(expr, arena)");
                        free(out);
                        return (char*)-1;
                    }
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_string_from((");
                    cc_sb_append(&out, &out_len, &out_cap, src + arg1_s, arg1_e - arg1_s);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "), (");
                    cc_sb_append(&out, &out_len, &out_cap, src + arg2_s, arg2_e - arg2_s);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "))");
                    rewrite_count++;
                    last_emit = arg2_e + 1;
                    i = arg2_e + 1;
                    continue;
                }
            }
        }
        i++;
        col++;
    }
    if (rewrite_count == 0) {
        free(out);
        return NULL;
    }
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

char* cc_rewrite_string_templates_text(const char* src, size_t n, const char* input_path) {
    return cc__rewrite_string_templates(src, n, input_path);
}

static void cc__mangle_type_name(const char* src, size_t len, char* out, size_t out_sz);
static void cc__mangle_container_type_param(const char* src, size_t len, char* out, size_t out_sz);
static void cc__canonicalize_ufcs_alias_target(char* out, size_t out_sz, const char* type_src);
static const char* cc__lookup_scoped_type_alias(const char* src, size_t limit, const char* alias_name, char* out_type, size_t out_type_sz);
static int cc__parse_typedef_alias_stmt(const char* stmt_start,
                                        const char* stmt_end,
                                        char* alias_name,
                                        size_t alias_name_sz,
                                        char* alias_type,
                                        size_t alias_type_sz);
static int cc__parse_decl_name_and_type_fallback(const char* stmt_start,
                                                 const char* stmt_end,
                                                 char* decl_name,
                                                 size_t decl_name_sz,
                                                 char* decl_type,
                                                 size_t decl_type_sz);

/* Rewrite channel handle types (surface syntax) into runtime handle structs.

   - `T[~ ... >] name` -> `CCChanTx name`
   - `T[~ ... <] name` -> `CCChanRx name`

   Side effect: registers the element type so later lowering can emit
   direct `cc_channel_send(tx, v)` / `cc_channel_recv(rx, &out)` calls
   with the correct typed channel declarations in scope.

   Requires explicit direction ('>' or '<'). Hard errors otherwise.
   Text-based: not valid C, so TCC must see rewritten code. */

/* Helper: wrap a closure with typed parameter for CCClosure1.
   Transforms: [captures](Type param) => body
   Into:       [captures](intptr_t __arg) => { Type param = (Type)__arg; body }
   
   If param is already intptr_t, returns the original unchanged. */
static void cc__wrap_typed_closure1(const char* closure, char* out, size_t out_cap) {
    if (!closure || !out || out_cap == 0) return;
    out[0] = 0;
    
    /* Find '[' (captures start) */
    const char* p = closure;
    while (*p && *p != '[') p++;
    if (!*p) { strncpy(out, closure, out_cap - 1); out[out_cap - 1] = 0; return; }
    
    /* Find '](' to get to params */
    const char* cap_start = p;
    while (*p && !(*p == ']' && *(p + 1) == '(')) p++;
    if (!*p) { strncpy(out, closure, out_cap - 1); out[out_cap - 1] = 0; return; }
    
    size_t cap_len = p - cap_start + 1;  /* include ] */
    p++;  /* skip ] */
    if (*p != '(') { strncpy(out, closure, out_cap - 1); out[out_cap - 1] = 0; return; }
    p++;  /* skip ( */
    
    /* Skip whitespace */
    while (*p && (*p == ' ' || *p == '\t')) p++;
    
    /* Extract parameter type */
    const char* type_start = p;
    /* Scan type - handle pointers like "CCArena*" */
    while (*p && *p != ')' && *p != ' ' && *p != '\t') {
        if (*p == '*') { p++; break; }  /* pointer type ends at * */
        p++;
    }
    size_t type_len = p - type_start;
    
    /* Check if it's already intptr_t */
    if (type_len == 8 && strncmp(type_start, "intptr_t", 8) == 0) {
        strncpy(out, closure, out_cap - 1);
        out[out_cap - 1] = 0;
        return;
    }
    
    /* Skip whitespace to parameter name */
    while (*p && (*p == ' ' || *p == '\t')) p++;
    
    /* Extract parameter name */
    const char* name_start = p;
    while (*p && *p != ')' && *p != ' ' && *p != '\t') p++;
    size_t name_len = p - name_start;
    
    if (name_len == 0) {
        /* No param name, type might BE the name (e.g., just "r") - don't wrap */
        strncpy(out, closure, out_cap - 1);
        out[out_cap - 1] = 0;
        return;
    }
    
    /* Find => and body */
    while (*p && !(*p == '=' && *(p + 1) == '>')) p++;
    if (!*p) { strncpy(out, closure, out_cap - 1); out[out_cap - 1] = 0; return; }
    p += 2;  /* skip => */
    while (*p && (*p == ' ' || *p == '\t')) p++;
    
    /* Rest is body */
    const char* body = p;
    
    /* Build wrapped closure:
       [captures](intptr_t __arg) => { Type name = (Type)__arg; body } */
    char type_buf[128], name_buf[64];
    if (type_len >= sizeof(type_buf)) type_len = sizeof(type_buf) - 1;
    if (name_len >= sizeof(name_buf)) name_len = sizeof(name_buf) - 1;
    memcpy(type_buf, type_start, type_len);
    type_buf[type_len] = 0;
    memcpy(name_buf, name_start, name_len);
    name_buf[name_len] = 0;
    
    /* Check if body is already a block */
    int body_is_block = (*body == '{');
    
    if (body_is_block) {
        /* Body is { ... }, insert declaration after { */
        snprintf(out, out_cap, "%.*s(intptr_t __arg) => { %s %s = (%s)__arg; %s",
                 (int)cap_len, cap_start, type_buf, name_buf, type_buf, body + 1);
    } else {
        /* Body is expression, wrap in block */
        snprintf(out, out_cap, "%.*s(intptr_t __arg) => { %s %s = (%s)__arg; return %s; }",
                 (int)cap_len, cap_start, type_buf, name_buf, type_buf, body);
    }
}

static char* cc__rewrite_chan_handle_types(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;

    size_t i = 0;
    size_t last_emit = 0;
    CCScannerState scanner;
    cc_scanner_init(&scanner);

    while (i < n) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scanner, src, n, &i)) continue;

        char c = src[i];
        if (c == '[') {
            size_t j = i + 1;
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j < n && src[j] == '~') {
                /* Check for 'owned' keyword - skip owned channels, they're handled by later pass */
                int is_owned = 0;
                {
                    size_t scan = j + 1;
                    /* Skip capacity expression - can be:
                     * - digits: 4, 16
                     * - expressions: (cap + 2), cap * 2
                     * - identifiers: my_cap
                     * Stop when we hit 'owned', '>', '<', or ']' */
                    while (scan < n && src[scan] != ']') {
                        char sc = src[scan];
                        /* Skip whitespace */
                        if (sc == ' ' || sc == '\t') { scan++; continue; }
                        /* Check for 'owned' keyword before processing identifiers */
                        if (scan + 5 <= n && memcmp(src + scan, "owned", 5) == 0) {
                            char next = (scan + 5 < n) ? src[scan + 5] : 0;
                            int is_ident = (next == '_' || (next >= 'A' && next <= 'Z') || 
                                           (next >= 'a' && next <= 'z') || (next >= '0' && next <= '9'));
                            if (!is_ident) break;  /* Found 'owned' keyword */
                        }
                        /* Skip digits, identifiers, operators */
                        if ((sc >= '0' && sc <= '9') || sc == '_' ||
                            (sc >= 'a' && sc <= 'z') || (sc >= 'A' && sc <= 'Z') ||
                            sc == '+' || sc == '-' || sc == '*' || sc == '/') { scan++; continue; }
                        /* Skip parenthesized expressions */
                        if (sc == '(') {
                            int depth = 1;
                            scan++;
                            while (scan < n && depth > 0) {
                                if (src[scan] == '(') depth++;
                                else if (src[scan] == ')') depth--;
                                scan++;
                            }
                            continue;
                        }
                        break;  /* Hit something else (like '>' or '<') */
                    }
                    while (scan < n && (src[scan] == ' ' || src[scan] == '\t')) scan++;
                    if (scan + 5 <= n && memcmp(src + scan, "owned", 5) == 0) {
                        char next = (scan + 5 < n) ? src[scan + 5] : 0;
                        int is_ident = (next == '_' || (next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z') || (next >= '0' && next <= '9'));
                        if (!is_ident) {
                            is_owned = 1;
                        }
                    }
                }
                
                if (is_owned) {
                    /* Transform owned channel: T[~N owned { ... }] varname; */
                    /* Skip capacity expression (may include parentheses) to find 'owned' */
                    size_t scan = j + 1;
                    while (scan < n && src[scan] != ']') {
                        char sc = src[scan];
                        if (sc == ' ' || sc == '\t') { scan++; continue; }
                        /* Check for 'owned' keyword */
                        if (scan + 5 <= n && memcmp(src + scan, "owned", 5) == 0) {
                            char next = (scan + 5 < n) ? src[scan + 5] : 0;
                            int is_kw = !(next == '_' || (next >= 'A' && next <= 'Z') || 
                                         (next >= 'a' && next <= 'z') || (next >= '0' && next <= '9'));
                            if (is_kw) break;
                        }
                        if ((sc >= '0' && sc <= '9') || sc == '_' ||
                            (sc >= 'a' && sc <= 'z') || (sc >= 'A' && sc <= 'Z') ||
                            sc == '+' || sc == '-' || sc == '*' || sc == '/') { scan++; continue; }
                        if (sc == '(') {
                            int depth = 1;
                            scan++;
                            while (scan < n && depth > 0) {
                                if (src[scan] == '(') depth++;
                                else if (src[scan] == ')') depth--;
                                scan++;
                            }
                            continue;
                        }
                        break;
                    }
                    while (scan < n && (src[scan] == ' ' || src[scan] == '\t')) scan++;
                    scan += 5;  /* Skip "owned" */
                    while (scan < n && (src[scan] == ' ' || src[scan] == '\t')) scan++;
                    
                    if (scan >= n || src[scan] != '{') {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scanner.line, scanner.col, "channel", "owned channel requires { ... } block");
                        free(out);
                        return NULL;
                    }
                    
                    /* Find matching '}' for the owned block */
                    size_t brace_start = scan;
                    int brace_depth = 0;
                    int in_str2 = 0, in_chr2 = 0;
                    size_t brace_end = scan;
                    for (size_t bi = scan; bi < n; bi++) {
                        char bc = src[bi];
                        if (in_str2) { if (bc == '\\' && bi + 1 < n) { bi++; continue; } if (bc == '"') in_str2 = 0; continue; }
                        if (in_chr2) { if (bc == '\\' && bi + 1 < n) { bi++; continue; } if (bc == '\'') in_chr2 = 0; continue; }
                        if (bc == '"') { in_str2 = 1; continue; }
                        if (bc == '\'') { in_chr2 = 1; continue; }
                        if (bc == '{') brace_depth++;
                        else if (bc == '}') {
                            brace_depth--;
                            if (brace_depth == 0) { brace_end = bi; break; }
                        }
                    }
                    
                    if (brace_depth != 0) {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scanner.line, scanner.col, "channel", "unterminated owned block");
                        free(out);
                        return NULL;
                    }
                    
                    /* Find ']' after the owned block */
                    size_t k = brace_end + 1;
                    while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
                    if (k >= n || src[k] != ']') {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scanner.line, scanner.col, "channel", "expected ']' after owned block");
                        free(out);
                        return NULL;
                    }
                    
                    /* Find variable name after ] */
                    size_t var_start = k + 1;
                    while (var_start < n && (src[var_start] == ' ' || src[var_start] == '\t')) var_start++;
                    size_t var_end = var_start;
                    while (var_end < n && (src[var_end] == '_' || (src[var_end] >= 'A' && src[var_end] <= 'Z') ||
                           (src[var_end] >= 'a' && src[var_end] <= 'z') || (src[var_end] >= '0' && src[var_end] <= '9'))) var_end++;
                    
                    if (var_end == var_start) {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scanner.line, scanner.col, "channel", "expected variable name after owned channel type");
                        free(out);
                        return NULL;
                    }
                    
                    /* Find semicolon */
                    size_t semi = var_end;
                    while (semi < n && src[semi] != ';') semi++;
                    if (semi >= n) {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scanner.line, scanner.col, "channel", "expected ';' after owned channel declaration");
                        free(out);
                        return NULL;
                    }
                    
                    /* Extract element type (before [) */
                    size_t ty_start = cc__scan_back_to_delim(src, i);
                    char elem_ty[256];
                    size_t elem_len = i - ty_start;
                    if (elem_len >= sizeof(elem_ty)) elem_len = sizeof(elem_ty) - 1;
                    memcpy(elem_ty, src + ty_start, elem_len);
                    elem_ty[elem_len] = 0;
                    while (elem_len > 0 && (elem_ty[elem_len - 1] == ' ' || elem_ty[elem_len - 1] == '\t')) elem_ty[--elem_len] = 0;
                    
                    /* Extract capacity (between ~ and owned) - handles expressions like (cap + 2) */
                    char cap_expr[128] = "0";
                    {
                        size_t cs = j + 1;
                        while (cs < brace_start && (src[cs] == ' ' || src[cs] == '\t')) cs++;
                        size_t ce = cs;
                        /* Scan capacity expression, handling parentheses */
                        int paren_depth = 0;
                        while (ce < brace_start) {
                            char ec = src[ce];
                            if (ec == '(') { paren_depth++; ce++; continue; }
                            if (ec == ')') { paren_depth--; ce++; continue; }
                            if (paren_depth > 0) { ce++; continue; }  /* Inside parens, include everything */
                            /* Outside parens: stop at whitespace before 'owned' */
                            if (ec == ' ' || ec == '\t') {
                                size_t peek = ce;
                                while (peek < brace_start && (src[peek] == ' ' || src[peek] == '\t')) peek++;
                                if (peek + 5 <= brace_start && memcmp(src + peek, "owned", 5) == 0) break;
                            }
                            if (memcmp(src + ce, "owned", 5) == 0) break;
                            ce++;
                        }
                        /* Trim trailing whitespace */
                        while (ce > cs && (src[ce - 1] == ' ' || src[ce - 1] == '\t')) ce--;
                        if (ce > cs) {
                            size_t cl = ce - cs;
                            if (cl >= sizeof(cap_expr)) cl = sizeof(cap_expr) - 1;
                            memcpy(cap_expr, src + cs, cl);
                            cap_expr[cl] = 0;
                        }
                    }
                    
                    /* Extract variable name */
                    char var_name[128];
                    size_t vlen = var_end - var_start;
                    if (vlen >= sizeof(var_name)) vlen = sizeof(var_name) - 1;
                    memcpy(var_name, src + var_start, vlen);
                    var_name[vlen] = 0;
                    
                    /* Extract owned block content (closures) - keep it for closure pass */
                    char owned_content[4096];
                    size_t owned_len = brace_end - brace_start - 1;  /* Exclude { and } */
                    if (owned_len >= sizeof(owned_content)) owned_len = sizeof(owned_content) - 1;
                    memcpy(owned_content, src + brace_start + 1, owned_len);
                    owned_content[owned_len] = 0;
                    
                    /* Generate transformed code:
                     * Note: We can't fully expand the closures here, but we can generate
                     * a form that's valid C and will be processed by later passes. */
                    
                    /* For now, emit a comment and let pass_channel_syntax handle it later.
                     * But we need to emit something valid for TCC to parse...
                     * 
                     * Actually, the cleanest is to emit the manual API form:
                     * CCChan* varname = cc_chan_create_owned(cap, sizeof(elem), create, destroy, reset);
                     * But we need to extract the closures...
                     *
                     * Simplest working approach: emit placeholders that will compile
                     * and mark with a special comment for later pass.
                     */
                    
                    /* Emit up to the owned channel */
                    if (ty_start > last_emit) {
                        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                    }
                    
                    /* Emit transformed owned channel as a placeholder that compiles.
                     * The closures are embedded and will be processed by closure pass. */
                    char buf[8192];
                    static int owned_id = 0;
                    int id = owned_id++;
                    
                    /* Extract closures from owned_content by looking for .create, .destroy, .reset */
                    char create_c[2048] = "{0}";
                    char destroy_c[2048] = "{0}";
                    char reset_c[2048] = "{0}";
                    
                    /* Simple extraction: find ".field = " and copy until next "," or "}" at same depth */
                    const char* p = owned_content;
                    while (*p) {
                        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')) p++;
                        if (*p != '.') { p++; continue; }
                        p++;
                        char field[32];
                        size_t fn = 0;
                        while (*p && fn + 1 < sizeof(field) && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_')) {
                            field[fn++] = *p++;
                        }
                        field[fn] = 0;
                        while (*p && (*p == ' ' || *p == '\t')) p++;
                        if (*p != '=') continue;
                        p++;
                        while (*p && (*p == ' ' || *p == '\t')) p++;
                        
                        /* Find end of closure */
                        const char* closure_start = p;
                        int depth = 0;
                        int in_s = 0;
                        while (*p) {
                            if (in_s) { if (*p == '\\' && *(p+1)) p++; else if (*p == '"') in_s = 0; p++; continue; }
                            if (*p == '"') { in_s = 1; p++; continue; }
                            if (*p == '(' || *p == '[' || *p == '{') depth++;
                            else if (*p == ')' || *p == ']' || *p == '}') depth--;
                            if (depth < 0 || (depth == 0 && *p == ',')) break;
                            p++;
                        }
                        
                        size_t clen = p - closure_start;
                        char* dest = NULL;
                        size_t dcap = 0;
                        if (strcmp(field, "create") == 0) { dest = create_c; dcap = sizeof(create_c); }
                        else if (strcmp(field, "destroy") == 0) { dest = destroy_c; dcap = sizeof(destroy_c); }
                        else if (strcmp(field, "reset") == 0) { dest = reset_c; dcap = sizeof(reset_c); }
                        
                        if (dest && clen < dcap) {
                            memcpy(dest, closure_start, clen);
                            dest[clen] = 0;
                            /* Trim trailing whitespace */
                            while (clen > 0 && (dest[clen-1] == ' ' || dest[clen-1] == '\t' || dest[clen-1] == '\n')) dest[--clen] = 0;
                        }
                    }
                    
                    /* Wrap destroy/reset closures to handle typed parameters.
                       Converts [](CCArena* a) => ... to [](intptr_t __arg) => { CCArena* a = (CCArena*)__arg; ... } */
                    char destroy_wrapped[2048], reset_wrapped[2048];
                    cc__wrap_typed_closure1(destroy_c, destroy_wrapped, sizeof(destroy_wrapped));
                    cc__wrap_typed_closure1(reset_c, reset_wrapped, sizeof(reset_wrapped));
                    
                    /* Generate code with closure variables and channel creation */
                    snprintf(buf, sizeof(buf),
                             "/* owned channel %s */\n"
                             "CCClosure0 __cc_owned_%d_create = %s;\n"
                             "CCClosure1 __cc_owned_%d_destroy = %s;\n"
                             "CCClosure1 __cc_owned_%d_reset = %s;\n"
                             "CCChan* %s = cc_chan_create_owned(%s, sizeof(%s), "
                             "__cc_owned_%d_create, __cc_owned_%d_destroy, __cc_owned_%d_reset)",
                             var_name,
                             id, create_c,
                             id, destroy_wrapped,
                             id, reset_wrapped,
                             var_name, cap_expr, elem_ty,
                             id, id, id);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, buf);
                    
                    /* Advance past the semicolon */
                    last_emit = semi;  /* Leave ; to be emitted */
                    while (i <= semi) {
                        if (src[i] == '\n') { scanner.line++; scanner.col = 1; }
                        else scanner.col++;
                        i++;
                    }
                    continue;
                }
                
                /* Find ']' (same line, best-effort) */
                size_t k = j + 1;
                while (k < n && src[k] != ']' && src[k] != '\n') k++;
                if (k >= n || src[k] != ']') {
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                            scanner.line, scanner.col, "channel", "unterminated channel handle type (missing ']')");
                    free(out);
                    return NULL;
                }

                int saw_gt = 0, saw_lt = 0, saw_ordered = 0;
                for (size_t t = j; t < k; t++) {
                    if (src[t] == '>') saw_gt = 1;
                    if (src[t] == '<') saw_lt = 1;
                    /* Check for 'ordered' keyword */
                    if (t + 7 <= k && memcmp(src + t, "ordered", 7) == 0) {
                        char before = (t == j) ? ' ' : src[t - 1];
                        char after = (t + 7 < k) ? src[t + 7] : ' ';
                        /* Inline ident check to avoid forward declaration */
                        int before_is_ident = (before == '_' || (before >= 'A' && before <= 'Z') || 
                                               (before >= 'a' && before <= 'z') || (before >= '0' && before <= '9'));
                        int after_is_ident = (after == '_' || (after >= 'A' && after <= 'Z') || 
                                              (after >= 'a' && after <= 'z') || (after >= '0' && after <= '9'));
                        if (!before_is_ident && !after_is_ident) {
                            saw_ordered = 1;
                        }
                    }
                }
                if (saw_gt && saw_lt) {
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                            scanner.line, scanner.col, "channel", "channel handle type cannot be both send ('>') and recv ('<')");
                    free(out);
                    return NULL;
                }
                if (!saw_gt && !saw_lt) {
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                            scanner.line, scanner.col, "channel", "channel handle type requires direction: use 'T[~ ... >]' or 'T[~ ... <]'");
                    free(out);
                    return NULL;
                }
                /* Validate: 'ordered' only allowed on rx channels */
                if (saw_ordered && saw_gt) {
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                            scanner.line, scanner.col, "channel", "'ordered' modifier only allowed on receive (<) channel");
                    free(out);
                    return NULL;
                }

                size_t ty_start = cc__scan_back_to_delim(src, i);
                size_t type_start = cc__skip_leading_decl_specs(src, ty_start);
                char elem_ty[256];
                size_t elem_len = i - type_start;
                if (elem_len >= sizeof(elem_ty)) elem_len = sizeof(elem_ty) - 1;
                memcpy(elem_ty, src + type_start, elem_len);
                elem_ty[elem_len] = 0;
                while (elem_len > 0 && (elem_ty[elem_len - 1] == ' ' || elem_ty[elem_len - 1] == '\t')) {
                    elem_ty[--elem_len] = 0;
                }
                char mangled_elem[128];
                char typed_handle[160];
                cc__mangle_type_name(elem_ty, elem_len, mangled_elem, sizeof(mangled_elem));
                snprintf(typed_handle, sizeof(typed_handle), "%s_%s",
                         saw_gt ? "CCChanTx" : "CCChanRx", mangled_elem);

                if (ty_start < last_emit) {
                    /* overlapping/odd context; just ignore and continue */
                } else {
                    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                    cc_sb_append(&out, &out_len, &out_cap, src + ty_start, type_start - ty_start);
                    /* Emit CCChanTx for send, CCChanRx for recv (ordered is a flag, not a type) */
                    cc_sb_append_cstr(&out, &out_len, &out_cap, saw_gt ? "CCChanTx" : "CCChanRx");
                    last_emit = k + 1; /* skip past ']' */
                }

                {
                    CCTypeRegistry* reg = cc_type_registry_get_global();
                    if (reg && mangled_elem[0]) {
                        cc_type_registry_add_channel(reg, elem_ty, mangled_elem);

                        size_t v = k + 1;
                        while (v < n && (src[v] == ' ' || src[v] == '\t')) v++;
                        if (v < n && cc_is_ident_start(src[v])) {
                            size_t var_start = v;
                            while (v < n && cc_is_ident_char(src[v])) v++;
                            if (v > var_start) {
                                char var_name[128];
                                size_t vn_len = v - var_start;
                                if (vn_len >= sizeof(var_name)) vn_len = sizeof(var_name) - 1;
                                memcpy(var_name, src + var_start, vn_len);
                                var_name[vn_len] = 0;
                                cc_type_registry_add_var(reg, var_name, typed_handle);
                            }
                        }
                    }
                }

                /* advance to k+1 */
                while (i < k + 1) {
                    if (src[i] == '\n') { scanner.line++; scanner.col = 1; }
                    else scanner.col++;
                    i++;
                }
                continue;
            }
        }

        i++; scanner.col++;
    }

    if (last_emit < n) {
        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    }
    return out;
}

static int cc_is_ident_char_local(char c) {
    return (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'));
}

/* Scan backwards from pos to find the start of a member access chain (e.g., obj.field or ptr->field).
   Returns the start position of the full expression.
   Handles chains like a.b.c, a->b->c, (*p)->field, arr[i].field, func().field. */
static size_t cc_scan_back_for_member_access(const char* src, size_t pos, size_t limit) {
    if (pos == 0 || pos <= limit) return pos;
    
    size_t p = pos;
    while (p > limit && (src[p-1] == ' ' || src[p-1] == '\t')) p--;
    
    int has_access = 0;
    if (p > limit && src[p-1] == '.') {
        has_access = 1;
        p--;
    } else if (p >= 2 + limit && src[p-1] == '>' && src[p-2] == '-') {
        has_access = 1;
        p -= 2;
    }
    
    if (!has_access) return pos;
    
    while (p > limit && (src[p-1] == ' ' || src[p-1] == '\t')) p--;
    int last_was_ident = 0;
    while (p > limit) {
        if (cc_is_ident_char(src[p-1])) {
            p--;
            last_was_ident = 1;
        } else if (src[p-1] == '.') {
            p--;
            last_was_ident = 0;
        } else if (p >= 2 + limit && src[p-1] == '>' && src[p-2] == '-') {
            p -= 2;
            last_was_ident = 0;
        } else if (src[p-1] == ')') {
            if (last_was_ident) break;
            int depth = 1;
            size_t q = p - 1;
            while (q > limit && depth > 0) {
                q--;
                if (src[q] == ')') depth++;
                else if (src[q] == '(') depth--;
            }
            if (depth == 0) p = q;
            else break;
            last_was_ident = 0;
        } else if (src[p-1] == ']') {
            int depth = 1;
            size_t q = p - 1;
            while (q > limit && depth > 0) {
                q--;
                if (src[q] == ']') depth++;
                else if (src[q] == '[') depth--;
            }
            if (depth == 0) p = q;
            else break;
        } else {
            break;
        }
        while (p > limit && (src[p-1] == ' ' || src[p-1] == '\t')) {
            if (last_was_ident) return p;
            p--;
        }
    }
    return p;
}

/* Seed UFCS receiver types that are introduced by ordinary declarations or CC
   syntax sugar, but are not otherwise recorded by the textual type rewrites.
   This keeps the AST-aware UFCS pass fed with receiver type names without
   rewriting UFCS call text in preprocessing. */
static void cc__seed_ufcs_receiver_types(const char* src, size_t n) {
    CCTypeRegistry* reg = cc_type_registry_get_global();
    if (!src || n == 0 || !reg) return;

    size_t si = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);
    while (si < n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &si)) continue;
        if (!cc_is_ident_start(src[si])) { si++; continue; }

        size_t type_start = si;
        while (si < n && cc_is_ident_char(src[si])) si++;
        size_t type_len = si - type_start;
        const char* canonical_type = NULL;
        if ((type_len == sizeof("CCFile") - 1 && memcmp(src + type_start, "CCFile", type_len) == 0) ||
            (type_len == sizeof("File") - 1 && memcmp(src + type_start, "File", type_len) == 0)) {
            canonical_type = "CCFile";
        } else if ((type_len == sizeof("CCArena") - 1 && memcmp(src + type_start, "CCArena", type_len) == 0) ||
                   (type_len == sizeof("Arena") - 1 && memcmp(src + type_start, "Arena", type_len) == 0)) {
            canonical_type = "CCArena";
        } else {
            continue;
        }

        size_t j = cc_skip_ws_and_comments(src, n, si);
        int is_ptr = 0;
        while (j < n && src[j] == '*') {
            is_ptr = 1;
            j++;
            j = cc_skip_ws_and_comments(src, n, j);
        }
        if (j >= n || !cc_is_ident_start(src[j])) continue;

        size_t var_start = j;
        while (j < n && cc_is_ident_char(src[j])) j++;
        size_t var_len = j - var_start;
        if (var_len == 0 || var_len >= 128) continue;

        if (cc_skip_ws_and_comments(src, n, j) < n && src[cc_skip_ws_and_comments(src, n, j)] == '(') continue;

        {
            char var_name[128];
            memcpy(var_name, src + var_start, var_len);
            var_name[var_len] = '\0';
            if (strcmp(canonical_type, "CCFile") == 0) {
                cc_type_registry_add_var(reg, var_name, is_ptr ? "CCFile*" : "CCFile");
            } else {
                cc_type_registry_add_var(reg, var_name, is_ptr ? "CCArena*" : "CCArena");
            }
        }
    }

}

typedef struct {
    char name[128];
    char type[256];
} CCUfcsVarInfo;

typedef struct {
    char struct_name[128];
    char field_name[128];
    char field_type[256];
} CCUfcsFieldInfo;

static void cc__trim_span_ws(const char** start, const char** end) {
    if (!start || !end || !*start || !*end) return;
    while (*start < *end && (**start == ' ' || **start == '\t' || **start == '\n' || **start == '\r')) (*start)++;
    while (*end > *start) {
        char c = (*end)[-1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') (*end)--;
        else break;
    }
}

static void cc__copy_type_base(char* out, size_t out_sz, const char* type_name) {
    size_t len = 0;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!type_name) return;
    len = strlen(type_name);
    while (len > 0 && (type_name[len - 1] == ' ' || type_name[len - 1] == '\t')) len--;
    while (len > 0 && type_name[len - 1] == '*') len--;
    while (len > 0 && (type_name[len - 1] == ' ' || type_name[len - 1] == '\t')) len--;
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, type_name, len);
    out[len] = '\0';
}

static void cc__normalize_bool_family_type(char* type_name, size_t type_name_sz) {
    if (!type_name || type_name_sz == 0 || !strstr(type_name, "_Bool")) return;
    char tmp[256];
    size_t out = 0;
    for (size_t i = 0; type_name[i] && out + 1 < sizeof(tmp); ) {
        if (strncmp(type_name + i, "_Bool", 5) == 0) {
            if (out + 4 >= sizeof(tmp)) break;
            memcpy(tmp + out, "bool", 4);
            out += 4;
            i += 5;
            continue;
        }
        tmp[out++] = type_name[i++];
    }
    tmp[out] = '\0';
    snprintf(type_name, type_name_sz, "%s", tmp);
}

static int cc__type_is_known_ufcs_family_base(const char* type_name);

static int cc__is_slice_ufcs_method(const char* method_name) {
    return method_name &&
           (strcmp(method_name, "to_str") == 0 ||
            strcmp(method_name, "hdr") == 0 ||
            strcmp(method_name, "len") == 0 ||
            strcmp(method_name, "trim") == 0 ||
            strcmp(method_name, "trim_left") == 0 ||
            strcmp(method_name, "trim_right") == 0 ||
            strcmp(method_name, "is_empty") == 0 ||
            strcmp(method_name, "at") == 0 ||
            strcmp(method_name, "sub") == 0 ||
            strcmp(method_name, "starts_with") == 0 ||
            strcmp(method_name, "ends_with") == 0 ||
            strcmp(method_name, "eq") == 0 ||
            strcmp(method_name, "eq_cstr") == 0 ||
            strcmp(method_name, "str") == 0 ||
            strcmp(method_name, "bytes") == 0);
}

static void cc__normalize_ufcs_type_name(char* out, size_t out_sz, const char* type_name) {
    const char* start;
    const char* end;
    const char* base_end;
    const char* bang;
    const char* chan_l;
    const char* chan_r;
    const char* elem_s;
    const char* elem_e;
    int chan_is_tx = 0;
    int chan_is_rx = 0;
    int ptr_count = 0;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!type_name) return;
    start = type_name;
    end = type_name + strlen(type_name);
    cc__trim_span_ws(&start, &end);
    if (start >= end) return;
    base_end = end;
    while (base_end > start) {
        char c = base_end[-1];
        if (c == ' ' || c == '\t') {
            base_end--;
            continue;
        }
        if (c == '*') {
            ptr_count++;
            base_end--;
            continue;
        }
        break;
    }
    while (base_end > start && (base_end[-1] == ' ' || base_end[-1] == '\t')) base_end--;
    {
        const char* slice_l = memchr(start, '[', (size_t)(base_end - start));
        if (slice_l) {
            const char* slice_r = memchr(slice_l, ']', (size_t)(base_end - slice_l));
            const char* colon = memchr(slice_l, ':', slice_r ? (size_t)(slice_r - slice_l) : 0);
            if (slice_r && slice_r + 1 == base_end && colon) {
                const char* bang = memchr(slice_l, '!', (size_t)(slice_r - slice_l));
                snprintf(out, out_sz, "%s%.*s",
                         bang ? "CCSliceUnique" : "CCSlice",
                         ptr_count, "********");
                return;
            }
        }
    }
    chan_l = memchr(start, '[', (size_t)(base_end - start));
    if (chan_l) {
        chan_r = memchr(chan_l, ']', (size_t)(base_end - chan_l));
        if (chan_r && memchr(chan_l, '~', (size_t)(chan_r - chan_l)) != NULL) {
            char mangled_elem[128];
            if (memchr(chan_l, '>', (size_t)(chan_r - chan_l)) != NULL) chan_is_tx = 1;
            if (memchr(chan_l, '<', (size_t)(chan_r - chan_l)) != NULL) chan_is_rx = 1;
            if (chan_is_tx || chan_is_rx) {
                elem_s = start;
                elem_e = chan_l;
                cc__trim_span_ws(&elem_s, &elem_e);
                if (elem_e > elem_s) {
                    cc__mangle_type_name(elem_s, (size_t)(elem_e - elem_s), mangled_elem, sizeof(mangled_elem));
                    if (mangled_elem[0]) {
                        snprintf(out, out_sz, "%s_%s%.*s",
                                 chan_is_tx ? "CCChanTx" : "CCChanRx",
                                 mangled_elem,
                                 ptr_count, "********");
                        return;
                    }
                }
            }
        }
    }
    if (((size_t)(base_end - start) > 4 && memcmp(start, "Vec<", 4) == 0 && base_end[-1] == '>') ||
        ((size_t)(base_end - start) > 6 && memcmp(start, "Vec::[", 6) == 0 && base_end[-1] == ']') ||
        ((size_t)(base_end - start) > 6 && memcmp(start, "CCVec<", 6) == 0 && base_end[-1] == '>') ||
        ((size_t)(base_end - start) > 8 && memcmp(start, "CCVec::[", 8) == 0 && base_end[-1] == ']')) {
        size_t prefix = ((size_t)(base_end - start) > 8 && memcmp(start, "CCVec::[", 8) == 0) ? 8 :
                        (((size_t)(base_end - start) > 6 && memcmp(start, "CCVec<", 6) == 0) ? 6 :
                         ((start[3] == ':') ? 6 : 4));
        size_t inner_len = (size_t)(base_end - (start + prefix) - 1);
        snprintf(out, out_sz, "__CC_VEC(%.*s)%.*s",
                 (int)inner_len, start + prefix, ptr_count, "********");
        return;
    }
    if ((size_t)(base_end - start) > 9 && memcmp(start, "__CC_VEC(", 9) == 0 && base_end[-1] == ')') {
        char mangled_inner[128];
        const char* inner_s = start + 9;
        size_t inner_len = (size_t)(base_end - inner_s - 1);
        cc__mangle_type_name(inner_s, inner_len, mangled_inner, sizeof(mangled_inner));
        if (mangled_inner[0]) {
            snprintf(out, out_sz, "CCVec_%s%.*s", mangled_inner, ptr_count, "********");
            return;
        }
    }
    if (((size_t)(base_end - start) > 4 && memcmp(start, "Map<", 4) == 0 && base_end[-1] == '>') ||
        ((size_t)(base_end - start) > 6 && memcmp(start, "Map::[", 6) == 0 && base_end[-1] == ']')) {
        size_t prefix = (start[3] == ':') ? 6 : 4;
        size_t inner_len = (size_t)(base_end - (start + prefix) - 1);
        snprintf(out, out_sz, "__CC_MAP(%.*s)%.*s",
                 (int)inner_len, start + prefix, ptr_count, "********");
        return;
    }
    if ((size_t)(base_end - start) > 9 && memcmp(start, "__CC_MAP(", 9) == 0 && base_end[-1] == ')') {
        const char* inner_s = start + 9;
        const char* inner_e = base_end - 1;
        const char* comma = NULL;
        int depth = 0;
        char mangled_key[128];
        char mangled_val[128];
        for (const char* q = inner_s; q < inner_e; q++) {
            char c = *q;
            if (c == '(' || c == '[' || c == '<' || c == '{') depth++;
            else if (c == ')' || c == ']' || c == '>' || c == '}') depth--;
            else if (c == ',' && depth == 0) { comma = q; break; }
        }
        if (comma) {
            const char* key_s = inner_s;
            const char* key_e = comma;
            const char* val_s = comma + 1;
            const char* val_e = inner_e;
            cc__trim_span_ws(&key_s, &key_e);
            cc__trim_span_ws(&val_s, &val_e);
            cc__mangle_type_name(key_s, (size_t)(key_e - key_s), mangled_key, sizeof(mangled_key));
            cc__mangle_type_name(val_s, (size_t)(val_e - val_s), mangled_val, sizeof(mangled_val));
            if (mangled_key[0] && mangled_val[0]) {
                snprintf(out, out_sz, "Map_%s_%s%.*s", mangled_key, mangled_val, ptr_count, "********");
                return;
            }
        }
    }
    bang = memchr(start, '!', (size_t)(base_end - start));
    if (bang && (bang + 1) < base_end && bang[1] == '>') {
        const char* ok_s = start;
        const char* ok_e = bang;
        const char* err_s = bang + 2;
        const char* err_e = base_end;
        char mangled_ok[128];
        char mangled_err[128];
        cc__trim_span_ws(&ok_s, &ok_e);
        cc__trim_span_ws(&err_s, &err_e);
        if (err_s < err_e && *err_s == '(') {
            err_s++;
            if (err_e > err_s && err_e[-1] == ')') err_e--;
            cc__trim_span_ws(&err_s, &err_e);
        }
        if (ok_e > ok_s && err_e > err_s) {
            cc__mangle_type_name(ok_s, (size_t)(ok_e - ok_s), mangled_ok, sizeof(mangled_ok));
            cc__mangle_type_name(err_s, (size_t)(err_e - err_s), mangled_err, sizeof(mangled_err));
            if (mangled_ok[0] && mangled_err[0]) {
                snprintf(out, out_sz, "CCResult_%s_%s%.*s",
                         mangled_ok, mangled_err, ptr_count, "********");
                return;
            }
        }
    }
    /* (retired) The `T?` -> `CCOptional_T` receiver-type inference was once
     * handled here. Optionals are gone; the T? detector in the main
     * preprocess pass emits a diagnostic before we reach this path. */
    {
        size_t base_len = (size_t)(base_end - start);
        CCTypeRegistry* reg = cc_type_registry_get_global();
        char base_name[256];
        const char* alias = NULL;
        if (base_len >= out_sz) base_len = out_sz - 1;
        if (base_len >= sizeof(base_name)) base_len = sizeof(base_name) - 1;
        memcpy(base_name, start, base_len);
        base_name[base_len] = '\0';
        if (cc__type_is_known_ufcs_family_base(base_name)) {
            snprintf(out, out_sz, "%s", base_name);
        } else if (reg && (alias = cc_type_registry_lookup_alias(reg, base_name)) && *alias) {
            snprintf(out, out_sz, "%s", alias);
        } else {
            snprintf(out, out_sz, "%s", base_name);
        }
        while (ptr_count-- > 0 && strlen(out) + 1 < out_sz) strcat(out, "*");
    }
}

static int cc__type_is_parser_vec(const char* type_name) {
    return type_name && strncmp(type_name, "__CC_VEC", 8) == 0;
}

static int cc__type_is_parser_map(const char* type_name) {
    return type_name && strncmp(type_name, "__CC_MAP", 8) == 0;
}

static int cc__type_is_known_ufcs_family_base(const char* type_name) {
    if (!type_name || !type_name[0]) return 0;
    return strncmp(type_name, "CCVec_", 6) == 0 ||
           strncmp(type_name, "Map_", 4) == 0 ||
           strcmp(type_name, "CCString") == 0 ||
           strcmp(type_name, "CCSlice") == 0 ||
           strcmp(type_name, "CCSliceUnique") == 0 ||
           strcmp(type_name, "CCSliceShared") == 0 ||
           strcmp(type_name, "CCArena") == 0 ||
           strcmp(type_name, "CCNursery") == 0 ||
           strcmp(type_name, "CCCommand") == 0 ||
           strcmp(type_name, "CCFile") == 0 ||
           strncmp(type_name, "CCResult_", 9) == 0 ||
           strncmp(type_name, "CCChanTx_", 9) == 0 ||
           strncmp(type_name, "CCChanRx_", 9) == 0 ||
           cc__type_is_parser_vec(type_name) ||
           cc__type_is_parser_map(type_name);
}

static const char* cc__lookup_ufcs_var_type(const CCUfcsVarInfo* vars, size_t var_count, const char* name) {
    if (!vars || !name) return NULL;
    for (size_t i = 0; i < var_count; i++) {
        if (strcmp(vars[i].name, name) == 0) return vars[i].type;
    }
    return NULL;
}

static const char* cc__lookup_ufcs_field_type(const CCUfcsFieldInfo* fields,
                                              size_t field_count,
                                              const char* struct_name,
                                              const char* field_name) {
    if (!fields || !struct_name || !field_name) return NULL;
    for (size_t i = 0; i < field_count; i++) {
        if (strcmp(fields[i].struct_name, struct_name) == 0 &&
            strcmp(fields[i].field_name, field_name) == 0) {
            return fields[i].field_type;
        }
    }
    return NULL;
}

static void cc__resolve_registered_alias_type_name(CCTypeRegistry* reg,
                                                   const char* type_name,
                                                   char* out,
                                                   size_t out_sz) {
    char input_buf[256];
    size_t len;
    int ptr_count = 0;
    const char* alias = NULL;
    char base[256];
    if (!out || out_sz == 0) return;
    if (!type_name) {
        out[0] = '\0';
        return;
    }
    snprintf(input_buf, sizeof(input_buf), "%s", type_name);
    type_name = input_buf;
    out[0] = '\0';
    len = strlen(type_name);
    while (len > 0 && (type_name[len - 1] == ' ' || type_name[len - 1] == '\t')) len--;
    while (len > 0 && type_name[len - 1] == '*') {
        ptr_count++;
        len--;
        while (len > 0 && (type_name[len - 1] == ' ' || type_name[len - 1] == '\t')) len--;
    }
    if (len >= sizeof(base)) len = sizeof(base) - 1;
    memcpy(base, type_name, len);
    base[len] = '\0';
    if (cc__type_is_known_ufcs_family_base(base)) {
        snprintf(out, out_sz, "%s", base);
        while (ptr_count-- > 0 && strlen(out) + 1 < out_sz) strcat(out, "*");
        return;
    }
    if (reg) alias = cc_type_registry_lookup_alias(reg, base);
    snprintf(out, out_sz, "%s", (alias && *alias) ? alias : base);
    while (ptr_count-- > 0 && strlen(out) + 1 < out_sz) strcat(out, "*");
}

static void cc__record_ufcs_var(CCUfcsVarInfo* vars,
                                size_t* var_count,
                                size_t var_cap,
                                const char* name,
                                const char* type_name) {
    char normalized_type[256];
    if (!vars || !var_count || !name || !type_name || !name[0] || !type_name[0]) return;
    cc__normalize_ufcs_type_name(normalized_type, sizeof(normalized_type), type_name);
    if (!normalized_type[0]) return;
    for (size_t i = 0; i < *var_count; i++) {
        if (strcmp(vars[i].name, name) == 0) {
            strncpy(vars[i].type, normalized_type, sizeof(vars[i].type) - 1);
            vars[i].type[sizeof(vars[i].type) - 1] = '\0';
            return;
        }
    }
    if (*var_count >= var_cap) return;
    strncpy(vars[*var_count].name, name, sizeof(vars[*var_count].name) - 1);
    vars[*var_count].name[sizeof(vars[*var_count].name) - 1] = '\0';
    strncpy(vars[*var_count].type, normalized_type, sizeof(vars[*var_count].type) - 1);
    vars[*var_count].type[sizeof(vars[*var_count].type) - 1] = '\0';
    (*var_count)++;
}

static void cc__record_ufcs_field(CCUfcsFieldInfo* fields,
                                  size_t* field_count,
                                  size_t field_cap,
                                  const char* struct_name,
                                  const char* field_name,
                                  const char* field_type) {
    char normalized_type[256];
    if (!fields || !field_count || !struct_name || !field_name || !field_type) return;
    if (*field_count >= field_cap) return;
    cc__normalize_ufcs_type_name(normalized_type, sizeof(normalized_type), field_type);
    if (!normalized_type[0]) return;
    strncpy(fields[*field_count].struct_name, struct_name, sizeof(fields[*field_count].struct_name) - 1);
    fields[*field_count].struct_name[sizeof(fields[*field_count].struct_name) - 1] = '\0';
    strncpy(fields[*field_count].field_name, field_name, sizeof(fields[*field_count].field_name) - 1);
    fields[*field_count].field_name[sizeof(fields[*field_count].field_name) - 1] = '\0';
    strncpy(fields[*field_count].field_type, normalized_type, sizeof(fields[*field_count].field_type) - 1);
    fields[*field_count].field_type[sizeof(fields[*field_count].field_type) - 1] = '\0';
    (*field_count)++;
}

/* cc__parse_decl_name_and_type — now delegated to cc_parse_decl_name_and_type in util/text.h */

/* cc__is_non_decl_stmt_type_pre_ufcs — now cc_is_non_decl_stmt_type in util/text.h */

static int cc__stmt_head_has_member_access(const char* stmt_start, const char* stmt_end);

static const char* cc__lookup_scoped_ufcs_var_type(const char* src,
                                                   size_t limit,
                                                   const char* var_name,
                                                   char* out_type,
                                                   size_t out_type_sz) {
    CCTypeRegistry* reg = cc_type_registry_get_global();
    typedef struct {
        int scope_id;
        char type_name[256];
    } LocalDecl;
    enum { MAX_DECLS = 256, MAX_SCOPES = 256 };
    LocalDecl decls[MAX_DECLS];
    int decl_count = 0;
    int scope_stack[MAX_SCOPES];
    int scope_depth = 1;
    int next_scope_id = 1;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    size_t stmt_start = 0;
    size_t i = 0;
    if (!src || !var_name || !var_name[0] || !out_type || out_type_sz == 0) return NULL;
    out_type[0] = '\0';
    scope_stack[0] = 0;
    while (i < limit) {
        char c = src[i];
        char c2 = (i + 1 < limit) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < limit) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < limit) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        if (c == '{') {
            if (scope_depth < MAX_SCOPES) scope_stack[scope_depth++] = next_scope_id++;
            stmt_start = i + 1;
            i++;
            continue;
        }
        if (c == '}') {
            int closing_scope = (scope_depth > 1) ? scope_stack[scope_depth - 1] : 0;
            while (decl_count > 0 && decls[decl_count - 1].scope_id == closing_scope) decl_count--;
            if (scope_depth > 1) scope_depth--;
            stmt_start = i + 1;
            i++;
            continue;
        }
        if (c == ';') {
            char decl_name[128];
            char decl_type[256];
            int stmt_has_member_access = cc__stmt_head_has_member_access(src + stmt_start, src + i);
            cc_parse_decl_name_and_type(src + stmt_start, src + i,
                                         decl_name, sizeof(decl_name),
                                         decl_type, sizeof(decl_type));
            if ((!decl_name[0] || strcmp(decl_name, var_name) != 0 || !decl_type[0]) &&
                !stmt_has_member_access) {
                (void)cc__parse_decl_name_and_type_fallback(src + stmt_start, src + i,
                                                            decl_name, sizeof(decl_name),
                                                            decl_type, sizeof(decl_type));
            }
            if (decl_name[0] &&
                strcmp(decl_name, var_name) == 0 &&
                !stmt_has_member_access &&
                !cc_is_non_decl_stmt_type(decl_type) &&
                decl_count < MAX_DECLS) {
                char alias_type[256];
                const char* alias = NULL;
                decls[decl_count].scope_id = scope_stack[scope_depth - 1];
                cc__normalize_ufcs_type_name(decls[decl_count].type_name,
                                             sizeof(decls[decl_count].type_name),
                                             decl_type);
                if (reg && !cc__type_is_known_ufcs_family_base(decls[decl_count].type_name)) {
                    alias = cc_type_registry_lookup_alias(reg, decls[decl_count].type_name);
                }
                if (alias && *alias) {
                    snprintf(decls[decl_count].type_name, sizeof(decls[decl_count].type_name), "%s", alias);
                }
                if (!cc__type_is_known_ufcs_family_base(decls[decl_count].type_name) &&
                    cc__lookup_scoped_type_alias(src, i, decls[decl_count].type_name,
                                                 alias_type, sizeof(alias_type))) {
                    snprintf(decls[decl_count].type_name, sizeof(decls[decl_count].type_name), "%s", alias_type);
                }
                decl_count++;
            }
            stmt_start = i + 1;
        }
        i++;
    }
    if (decl_count == 0) return NULL;
    strncpy(out_type, decls[decl_count - 1].type_name, out_type_sz - 1);
    out_type[out_type_sz - 1] = '\0';
    return out_type;
}

static int cc__stmt_head_has_member_access(const char* stmt_start, const char* stmt_end) {
    int par = 0, br = 0, brc = 0, ang = 0;
    if (!stmt_start || !stmt_end || stmt_end <= stmt_start) return 0;
    for (const char* p = stmt_start; p < stmt_end; ++p) {
        char c = *p;
        char c2 = (p + 1 < stmt_end) ? p[1] : 0;
        if (c == '(') par++;
        else if (c == ')' && par > 0) par--;
        else if (c == '[') br++;
        else if (c == ']' && br > 0) br--;
        else if (c == '{') brc++;
        else if (c == '}' && brc > 0) brc--;
        else if (c == '<') ang++;
        else if (c == '>' && ang > 0) ang--;
        else if (c == '=' && par == 0 && br == 0 && brc == 0 && ang == 0) break;
        else if (c == '.' && par == 0 && br == 0 && brc == 0 && ang == 0) return 1;
        else if (c == '-' && c2 == '>' && par == 0 && br == 0 && brc == 0 && ang == 0) return 1;
    }
    return 0;
}

static int cc__parse_typedef_alias_stmt(const char* stmt_start,
                                        const char* stmt_end,
                                        char* alias_name,
                                        size_t alias_name_sz,
                                        char* alias_type,
                                        size_t alias_type_sz) {
    const char* s = stmt_start;
    const char* e = stmt_end;
    const char* alias_end;
    const char* alias_start;
    const char* type_start;
    const char* type_end;
    if (!stmt_start || !stmt_end || stmt_end <= stmt_start) return 0;
    if (!alias_name || alias_name_sz == 0 || !alias_type || alias_type_sz == 0) return 0;
    alias_name[0] = '\0';
    alias_type[0] = '\0';
    while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) e--;
    if (e <= s) return 0;
    if ((size_t)(e - s) < 7 || memcmp(s, "typedef", 7) != 0) return 0;
    type_start = s + 7;
    while (type_start < e && (*type_start == ' ' || *type_start == '\t' || *type_start == '\n' || *type_start == '\r')) type_start++;
    if (type_start >= e) return 0;
    alias_end = e;
    while (alias_end > type_start && (alias_end[-1] == ' ' || alias_end[-1] == '\t' || alias_end[-1] == '\n' || alias_end[-1] == '\r')) alias_end--;
    alias_start = alias_end;
    while (alias_start > type_start && cc_is_ident_char(alias_start[-1])) alias_start--;
    if (alias_start == alias_end || !cc_is_ident_start(*alias_start)) return 0;
    type_end = alias_start;
    while (type_end > type_start && (type_end[-1] == ' ' || type_end[-1] == '\t' || type_end[-1] == '\n' || type_end[-1] == '\r')) type_end--;
    if (type_end <= type_start) return 0;
    {
        size_t alias_len = (size_t)(alias_end - alias_start);
        size_t type_len = (size_t)(type_end - type_start);
        if (alias_len >= alias_name_sz) alias_len = alias_name_sz - 1;
        if (type_len >= alias_type_sz) type_len = alias_type_sz - 1;
        memcpy(alias_name, alias_start, alias_len);
        alias_name[alias_len] = '\0';
        memcpy(alias_type, type_start, type_len);
        alias_type[type_len] = '\0';
    }
    return 1;
}

static int cc__parse_decl_name_and_type_fallback(const char* stmt_start,
                                                 const char* stmt_end,
                                                 char* decl_name,
                                                 size_t decl_name_sz,
                                                 char* decl_type,
                                                 size_t decl_type_sz) {
    const char* s = stmt_start;
    const char* e = stmt_end;
    const char* scan_end;
    const char* name_end;
    const char* name_start;
    const char* type_end;
    int par = 0, br = 0, brc = 0, ang = 0;
    if (!stmt_start || !stmt_end || stmt_end <= stmt_start) return 0;
    if (!decl_name || decl_name_sz == 0 || !decl_type || decl_type_sz == 0) return 0;
    decl_name[0] = '\0';
    decl_type[0] = '\0';
    while (s < e && isspace((unsigned char)*s)) s++;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    if (e <= s) return 0;
    scan_end = e;
    for (const char* p = s; p < e; ++p) {
        char c = *p;
        if (c == '(') par++;
        else if (c == ')' && par > 0) par--;
        else if (c == '[') br++;
        else if (c == ']' && br > 0) br--;
        else if (c == '{') brc++;
        else if (c == '}' && brc > 0) brc--;
        else if (c == '<') ang++;
        else if (c == '>' && ang > 0) ang--;
        else if (c == '=' && par == 0 && br == 0 && brc == 0 && ang == 0) {
            scan_end = p;
            break;
        }
    }
    while (scan_end > s && isspace((unsigned char)scan_end[-1])) scan_end--;
    name_end = scan_end;
    while (name_end > s && !cc_is_ident_char(name_end[-1])) name_end--;
    name_start = name_end;
    while (name_start > s && cc_is_ident_char(name_start[-1])) name_start--;
    if (name_start == name_end || !cc_is_ident_start(*name_start)) return 0;
    type_end = name_start;
    while (type_end > s && isspace((unsigned char)type_end[-1])) type_end--;
    if (type_end <= s) return 0;
    {
        size_t name_len = (size_t)(name_end - name_start);
        size_t type_len = (size_t)(type_end - s);
        if (name_len >= decl_name_sz) name_len = decl_name_sz - 1;
        if (type_len >= decl_type_sz) type_len = decl_type_sz - 1;
        memcpy(decl_name, name_start, name_len);
        decl_name[name_len] = '\0';
        memcpy(decl_type, s, type_len);
        decl_type[type_len] = '\0';
    }
    return 1;
}

static const char* cc__lookup_scoped_type_alias(const char* src,
                                                size_t limit,
                                                const char* alias_name,
                                                char* out_type,
                                                size_t out_type_sz) {
    size_t i = 0;
    size_t typedef_start = (size_t)-1;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    if (!src || !alias_name || !alias_name[0] || !out_type || out_type_sz == 0) return NULL;
    out_type[0] = '\0';
    while (i < limit) {
        char c = src[i];
        char c2 = (i + 1 < limit) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < limit) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < limit) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        if (typedef_start == (size_t)-1 &&
            i + 7 <= limit &&
            memcmp(src + i, "typedef", 7) == 0 &&
            (i == 0 || !cc_is_ident_char(src[i - 1])) &&
            (i + 7 == limit || !cc_is_ident_char(src[i + 7]))) {
            typedef_start = i;
            i += 7;
            continue;
        }
        if (c == ';') {
            if (typedef_start != (size_t)-1) {
                char decl_name[128];
                char decl_type[256];
                if (!cc__parse_typedef_alias_stmt(src + typedef_start, src + i,
                                                  decl_name, sizeof(decl_name),
                                                  decl_type, sizeof(decl_type))) {
                    typedef_start = (size_t)-1;
                    i++;
                    continue;
                }
                if (decl_name[0] && strcmp(decl_name, alias_name) == 0) {
                    const char* type_src = decl_type;
                    cc__canonicalize_ufcs_alias_target(out_type, out_type_sz, type_src);
                    return out_type;
                }
                typedef_start = (size_t)-1;
            }
        }
        i++;
    }
    return NULL;
}

static void cc__collect_generic_ufcs_types(const char* src,
                                           size_t n,
                                           CCUfcsVarInfo* vars,
                                           size_t* var_count,
                                           size_t var_cap,
                                           CCUfcsFieldInfo* fields,
                                           size_t* field_count,
                                           size_t field_cap) {
    CCTypeRegistry* reg = cc_type_registry_get_global();
    size_t i = 0;
    size_t typedef_start = (size_t)-1;
    CCScannerState scan;
    if (!src) return;
    cc_scanner_init(&scan);
    while (i < n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (src[i] == '{') {
            size_t close = i;
            while (close > 0 && isspace((unsigned char)src[close - 1])) close--;
            if (close > 0 && src[close - 1] == ')') {
                size_t open = close - 1;
                int depth = 1;
                while (open > 0 && depth > 0) {
                    open--;
                    if (src[open] == ')') depth++;
                    else if (src[open] == '(') depth--;
                }
                if (depth == 0) {
                    size_t param_start = open + 1;
                    size_t p = param_start;
                    int par = 0, br = 0;
                    while (p <= close - 1) {
                        if (p == close - 1 || (src[p] == ',' && par == 0 && br == 0)) {
                            char decl_name[128];
                            char decl_type[256];
                            cc_parse_decl_name_and_type(src + param_start, src + p,
                                                        decl_name, sizeof(decl_name),
                                                        decl_type, sizeof(decl_type));
                            if (decl_name[0] &&
                                strcmp(decl_type, "void") != 0 &&
                                !cc_is_non_decl_stmt_type(decl_type)) {
                                cc__record_ufcs_var(vars, var_count, var_cap, decl_name, decl_type);
                                if (reg && !cc_type_registry_lookup_var(reg, decl_name)) {
                                    cc_type_registry_add_var(reg, decl_name, decl_type);
                                }
                            }
                            param_start = p + 1;
                        } else if (src[p] == '(') {
                            par++;
                        } else if (src[p] == ')' && par > 0) {
                            par--;
                        } else if (src[p] == '[') {
                            br++;
                        } else if (src[p] == ']' && br > 0) {
                            br--;
                        }
                        p++;
                    }
                }
            }
        }
        if (typedef_start == (size_t)-1 &&
            i + 7 <= n &&
            memcmp(src + i, "typedef", 7) == 0 &&
            (i == 0 || !cc_is_ident_char(src[i - 1])) &&
            (i + 7 == n || !cc_is_ident_char(src[i + 7]))) {
            typedef_start = i;
        }
        if (src[i] == ';') {
            if (typedef_start != (size_t)-1) {
                char decl_name[128];
                char decl_type[256];
                if (reg &&
                    cc__parse_typedef_alias_stmt(src + typedef_start, src + i,
                                                 decl_name, sizeof(decl_name),
                                                 decl_type, sizeof(decl_type)) &&
                    decl_name[0]) {
                    const char* type_src = decl_type;
                    char normalized[256];
                    cc__canonicalize_ufcs_alias_target(normalized, sizeof(normalized), type_src);
                    if (normalized[0]) cc_type_registry_add_alias(reg, decl_name, normalized);
                }
                typedef_start = (size_t)-1;
            }
        }
        if (i + 6 <= n && memcmp(src + i, "typedef", 7) == 0 && !cc_is_ident_char(src[i + 7])) {
            size_t j = cc_skip_ws_and_comments(src, n, i + 7);
            if (j + 5 <= n && memcmp(src + j, "struct", 6) == 0 && !cc_is_ident_char(src[j + 6])) {
                size_t body_l = cc_skip_ws_and_comments(src, n, j + 6);
                /* Skip an optional struct tag between "struct" and "{" so
                 * `typedef struct Foo { ... } Foo;` registers its fields the
                 * same way the anonymous `typedef struct { ... } Foo;` does. */
                if (body_l < n && cc_is_ident_start(src[body_l])) {
                    size_t tag_end = body_l;
                    while (tag_end < n && cc_is_ident_char(src[tag_end])) tag_end++;
                    body_l = cc_skip_ws_and_comments(src, n, tag_end);
                }
                size_t body_r = 0;
                if (body_l < n && src[body_l] == '{' && cc_find_matching_brace(src, n, body_l, &body_r)) {
                    size_t name_pos = cc_skip_ws_and_comments(src, n, body_r + 1);
                    if (name_pos < n && cc_is_ident_start(src[name_pos])) {
                        char struct_name[128];
                        size_t sn = 0;
                        size_t p = name_pos;
                        while (p < n && cc_is_ident_char(src[p])) {
                            if (sn + 1 < sizeof(struct_name)) struct_name[sn] = src[p];
                            sn++;
                            p++;
                        }
                        struct_name[sn < sizeof(struct_name) ? sn : sizeof(struct_name) - 1] = '\0';
                        {
                            const char* body = src + body_l + 1;
                            const char* body_end = src + body_r;
                            const char* stmt = body;
                            while (stmt < body_end) {
                                const char* semi = memchr(stmt, ';', (size_t)(body_end - stmt));
                                if (!semi) break;
                                char field_name[128];
                                char field_type[256];
                                cc_parse_decl_name_and_type(stmt, semi, field_name, sizeof(field_name), field_type, sizeof(field_type));
                                if (field_name[0] && field_type[0]) {
                                    cc__record_ufcs_field(fields, field_count, field_cap, struct_name, field_name, field_type);
                                    if (reg) {
                                        cc_type_registry_add_field(reg, struct_name, field_name, field_type);
                                    }
                                }
                                stmt = semi + 1;
                            }
                        }
                    }
                }
            }
        }
        if (!cc_is_ident_start(src[i]) ||
            (i > 0 && (cc_is_ident_char(src[i - 1]) || src[i - 1] == '@'))) { i++; continue; }
        {
            size_t type_start = i;
            while (i < n && cc_is_ident_char(src[i])) i++;
            size_t type_end = i;
            if ((type_end - type_start == 6 && memcmp(src + type_start, "struct", 6) == 0) ||
                (type_end - type_start == 5 && memcmp(src + type_start, "union", 5) == 0)) {
                size_t tag = cc_skip_ws_and_comments(src, n, type_end);
                if (tag < n && cc_is_ident_start(src[tag])) {
                    size_t tag_end = tag;
                    while (tag_end < n && cc_is_ident_char(src[tag_end])) tag_end++;
                    type_end = tag_end;
                    i = tag_end;
                }
            }
            if (type_end - type_start == sizeof("__CC_VEC") - 1 &&
                memcmp(src + type_start, "__CC_VEC", sizeof("__CC_VEC") - 1) == 0) {
                size_t macro_l = cc_skip_ws_and_comments(src, n, type_end);
                size_t macro_r = 0;
                if (macro_l < n && src[macro_l] == '(' && cc_find_matching_paren(src, n, macro_l, &macro_r)) {
                    type_end = macro_r + 1;
                }
            } else if (type_end - type_start == sizeof("__CC_MAP") - 1 &&
                       memcmp(src + type_start, "__CC_MAP", sizeof("__CC_MAP") - 1) == 0) {
                size_t macro_l = cc_skip_ws_and_comments(src, n, type_end);
                size_t macro_r = 0;
                if (macro_l < n && src[macro_l] == '(' && cc_find_matching_paren(src, n, macro_l, &macro_r)) {
                    type_end = macro_r + 1;
                }
            }
            size_t j = cc_skip_ws_and_comments(src, n, type_end);
            while (j < n && src[j] == '*') {
                j++;
                j = cc_skip_ws_and_comments(src, n, j);
            }
            if (j < n && cc_is_ident_start(src[j])) {
                size_t var_start = j;
                while (j < n && cc_is_ident_char(src[j])) j++;
                if (cc_skip_ws_and_comments(src, n, j) < n && src[cc_skip_ws_and_comments(src, n, j)] != '(') {
                    char type_name[256];
                    char var_name[128];
                    size_t tn = type_end - type_start;
                    size_t vn = j - var_start;
                    if (tn >= sizeof(type_name)) tn = sizeof(type_name) - 1;
                    if (vn >= sizeof(var_name)) vn = sizeof(var_name) - 1;
                    memcpy(type_name, src + type_start, tn);
                    type_name[tn] = '\0';
                    memcpy(var_name, src + var_start, vn);
                    var_name[vn] = '\0';
                    {
                        size_t k = cc_skip_ws_and_comments(src, n, type_end);
                        while (k < var_start && (src[k] == '*' || src[k] == ' ' || src[k] == '\t')) {
                            if (src[k] == '*') strncat(type_name, "*", sizeof(type_name) - strlen(type_name) - 1);
                            k++;
                        }
                    }
                    cc__record_ufcs_var(vars, var_count, var_cap, var_name, type_name);
                    if (reg) {
                        const char* existing = cc_type_registry_lookup_var(reg, var_name);
                        if (!existing) cc_type_registry_add_var(reg, var_name, type_name);
                    }
                }
            }
        }
    }
}

static int cc__scan_generic_ufcs_call_site(const char* src,
                                           size_t n,
                                           size_t pos,
                                           size_t* out_sep_pos,
                                           size_t* out_method_start,
                                           size_t* out_method_end,
                                           size_t* out_recv_start,
                                           size_t* out_paren_pos,
                                           size_t* out_paren_end,
                                           char* out_method_name,
                                           size_t out_method_name_sz,
                                           char* out_recv_expr,
                                           size_t out_recv_expr_sz) {
    size_t sep_pos;
    size_t method_start;
    size_t method_end;
    size_t recv_start;
    size_t paren_pos;
    size_t paren_end = 0;
    if (!src || pos >= n || !out_method_name || out_method_name_sz == 0 ||
        !out_recv_expr || out_recv_expr_sz == 0) {
        return 0;
    }
    if (src[pos] == '.') {
        sep_pos = pos;
        method_start = cc_skip_ws_and_comments(src, n, pos + 1);
    } else if (pos + 1 < n && src[pos] == '-' && src[pos + 1] == '>') {
        sep_pos = pos;
        method_start = cc_skip_ws_and_comments(src, n, pos + 2);
    } else {
        return 0;
    }
    if (method_start >= n || !cc_is_ident_start(src[method_start])) return 0;
    method_end = method_start;
    while (method_end < n && cc_is_ident_char(src[method_end])) method_end++;
    if (method_end - method_start >= out_method_name_sz) return 0;
    memcpy(out_method_name, src + method_start, method_end - method_start);
    out_method_name[method_end - method_start] = '\0';
    paren_pos = cc_skip_ws_and_comments(src, n, method_end);
    if (paren_pos >= n || src[paren_pos] != '(') return 0;
    if (!cc_find_matching_paren(src, n, paren_pos, &paren_end)) return 0;
    recv_start = cc_scan_back_for_member_access(src, method_start, 0);
    if (recv_start >= sep_pos || sep_pos - recv_start >= out_recv_expr_sz) return 0;
    {
        const char* recv_s = src + recv_start;
        const char* recv_e = src + sep_pos;
        cc__trim_span_ws(&recv_s, &recv_e);
        if (recv_e <= recv_s || (size_t)(recv_e - recv_s) >= out_recv_expr_sz) return 0;
        memcpy(out_recv_expr, recv_s, (size_t)(recv_e - recv_s));
        out_recv_expr[recv_e - recv_s] = '\0';
    }
    if (out_sep_pos) *out_sep_pos = sep_pos;
    if (out_method_start) *out_method_start = method_start;
    if (out_method_end) *out_method_end = method_end;
    if (out_recv_start) *out_recv_start = recv_start;
    if (out_paren_pos) *out_paren_pos = paren_pos;
    if (out_paren_end) *out_paren_end = paren_end;
    return 1;
}

static int cc__resolve_generic_ufcs_receiver_type(const char* recv,
                                                  const char* source_text,
                                                  size_t use_offset,
                                                  const CCUfcsVarInfo* vars,
                                                  size_t var_count,
                                                  const CCUfcsFieldInfo* fields,
                                                  size_t field_count,
                                                  char* out_type,
                                                  size_t out_type_sz,
                                                  int* out_recv_is_ptr) {
    char expr[256];
    char local_type[256];
    size_t len;
    char root[128];
    const char* p;
    const char* type_name;
    int recv_is_ptr = 0;
    CCTypeRegistry* reg = cc_type_registry_get_global();
    if (!recv || !out_type || out_type_sz == 0) return 0;
    out_type[0] = '\0';
    if (out_recv_is_ptr) *out_recv_is_ptr = 0;
    while (*recv == ' ' || *recv == '\t' || *recv == '\n' || *recv == '\r') recv++;
    len = strlen(recv);
    while (len > 0 && (recv[len - 1] == ' ' || recv[len - 1] == '\t' || recv[len - 1] == '\n' || recv[len - 1] == '\r')) len--;
    if (len == 0 || len >= sizeof(expr)) return 0;
    memcpy(expr, recv, len);
    expr[len] = '\0';
    p = expr;
    if (*p == '&') {
        recv_is_ptr = 1;
        p++;
        while (*p == ' ' || *p == '\t') p++;
    }
    if (!cc_is_ident_start(*p)) return 0;
    {
        size_t rn = 0;
        while (cc_is_ident_char(p[rn])) rn++;
        if (rn >= sizeof(root)) rn = sizeof(root) - 1;
        memcpy(root, p, rn);
        root[rn] = '\0';
        p += rn;
    }
    type_name = NULL;
    if (source_text) {
        type_name = cc__lookup_scoped_ufcs_var_type(source_text, use_offset, root,
                                                    local_type, sizeof(local_type));
    }
    if (!type_name && reg) {
        type_name = cc_type_registry_resolve_receiver_expr_at(reg, recv, source_text, use_offset, &recv_is_ptr);
    }
    if (!type_name) type_name = cc__lookup_ufcs_var_type(vars, var_count, root);
    if (!type_name) return 0;
    strncpy(out_type, type_name, out_type_sz - 1);
    out_type[out_type_sz - 1] = '\0';
    cc__resolve_registered_alias_type_name(reg, out_type, out_type, out_type_sz);
    {
        char normalized_recv_type[256];
        cc__normalize_ufcs_type_name(normalized_recv_type, sizeof(normalized_recv_type), out_type);
        if (normalized_recv_type[0]) snprintf(out_type, out_type_sz, "%s", normalized_recv_type);
    }
    if (source_text) {
        char base_type[256];
        char alias_type[256];
        cc__copy_type_base(base_type, sizeof(base_type), out_type);
        if (base_type[0] &&
            !cc__type_is_known_ufcs_family_base(base_type) &&
            cc__lookup_scoped_type_alias(source_text, use_offset, base_type, alias_type, sizeof(alias_type))) {
            size_t stars = 0;
            size_t out_len = strlen(out_type);
            while (out_len > 0 && out_type[out_len - 1] == '*') { stars++; out_len--; }
            snprintf(out_type, out_type_sz, "%s", alias_type);
            while (stars-- > 0 && strlen(out_type) + 1 < out_type_sz) strcat(out_type, "*");
        }
    }
    if (source_text) {
        char base_type[256];
        char alias_type[256];
        cc__copy_type_base(base_type, sizeof(base_type), out_type);
        if (base_type[0] &&
            strcmp(base_type, out_type) != 0 &&
            !cc__type_is_known_ufcs_family_base(base_type)) {
            if (cc__lookup_scoped_type_alias(source_text, use_offset, base_type, alias_type, sizeof(alias_type))) {
                size_t stars = 0;
                size_t out_len = strlen(out_type);
                while (out_len > 0 && out_type[out_len - 1] == '*') { stars++; out_len--; }
                snprintf(out_type, out_type_sz, "%s", alias_type);
                while (stars-- > 0 && strlen(out_type) + 1 < out_type_sz) strcat(out_type, "*");
            }
        } else if (base_type[0] && !cc__type_is_known_ufcs_family_base(base_type)) {
            if (cc__lookup_scoped_type_alias(source_text, use_offset, base_type, alias_type, sizeof(alias_type))) {
                snprintf(out_type, out_type_sz, "%s", alias_type);
            }
        }
    }
    while (*p) {
        char field_name[128];
        char base_type[256];
        size_t fn = 0;
        int is_arrow = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '.' ) {
            p++;
        } else if (*p == '-' && p[1] == '>') {
            is_arrow = 1;
            p += 2;
        } else {
            break;
        }
        while (*p == ' ' || *p == '\t') p++;
        if (!cc_is_ident_start(*p)) return 0;
        while (cc_is_ident_char(*p)) {
            if (fn + 1 < sizeof(field_name)) field_name[fn] = *p;
            fn++;
            p++;
        }
        field_name[fn < sizeof(field_name) ? fn : sizeof(field_name) - 1] = '\0';
        cc__copy_type_base(base_type, sizeof(base_type), out_type);
        if (is_arrow && base_type[0] == '\0') return 0;
        type_name = cc__lookup_ufcs_field_type(fields, field_count, base_type, field_name);
        if (!type_name) return 0;
        strncpy(out_type, type_name, out_type_sz - 1);
        out_type[out_type_sz - 1] = '\0';
        cc__resolve_registered_alias_type_name(reg, out_type, out_type, out_type_sz);
        {
            char normalized_recv_type[256];
            cc__normalize_ufcs_type_name(normalized_recv_type, sizeof(normalized_recv_type), out_type);
            if (normalized_recv_type[0]) snprintf(out_type, out_type_sz, "%s", normalized_recv_type);
        }
        if (source_text) {
            char resolved_base[256];
            char alias_type[256];
            cc__copy_type_base(resolved_base, sizeof(resolved_base), out_type);
            if (resolved_base[0] &&
                !cc__type_is_known_ufcs_family_base(resolved_base) &&
                cc__lookup_scoped_type_alias(source_text, use_offset, resolved_base, alias_type, sizeof(alias_type))) {
                size_t stars = 0;
                size_t out_len = strlen(out_type);
                while (out_len > 0 && out_type[out_len - 1] == '*') { stars++; out_len--; }
                snprintf(out_type, out_type_sz, "%s", alias_type);
                while (stars-- > 0 && strlen(out_type) + 1 < out_type_sz) strcat(out_type, "*");
            }
        }
    }
    if (out_recv_is_ptr) *out_recv_is_ptr = recv_is_ptr || strchr(out_type, '*') != NULL;
    return 1;
}

static int cc__ufcs_preceded_by_await(const char* src, size_t recv_start) {
    size_t j;
    if (!src || recv_start == 0) return 0;
    j = recv_start;
    while (j > 0 && (src[j - 1] == ' ' || src[j - 1] == '\t' || src[j - 1] == '\n' || src[j - 1] == '\r')) j--;
    if (j < 5) return 0;
    if (memcmp(src + j - 5, "await", 5) != 0) return 0;
    if (j > 5 && cc_is_ident_char(src[j - 6])) return 0;
    return 1;
}

static char* cc__rewrite_generic_family_ufcs_impl(const char* src, size_t n, int parser_safe) {
    CCUfcsVarInfo vars[512];
    CCUfcsFieldInfo fields[256];
    CCTypeRegistry* reg = cc_type_registry_get_global();
    size_t var_count = 0, field_count = 0;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    size_t last_emit = 0;
    CCScannerState scan;
    if (!src || n == 0) return NULL;
    cc__collect_generic_ufcs_types(src, n, vars, &var_count, sizeof(vars) / sizeof(vars[0]),
                                   fields, &field_count, sizeof(fields) / sizeof(fields[0]));
    cc_scanner_init(&scan);
    while (i < n) {
        size_t sep_pos;
        size_t method_start;
        size_t method_end;
        size_t recv_start;
        size_t paren_pos;
        size_t paren_end = 0;
        char method_name[64];
        char recv_expr[256];
        char recv_type[256];
        char recv_type_base[256];
        int recv_is_ptr = 0;
        int family_by_value = 0;
        int family_pass_direct = 0;
        int parser_vec = 0;
        int parser_map = 0;
        int command_like = 0;
        int file_like = 0;
        int arena_like = 0;
        int string_like = 0;
        int slice_like = 0;
        int nursery_like = 0;
        int chan_tx = 0;
        int chan_rx = 0;
        const char* channel_callee = NULL;
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (!cc__scan_generic_ufcs_call_site(src, n, i, &sep_pos, &method_start, &method_end,
                                             &recv_start, &paren_pos, &paren_end,
                                             method_name, sizeof(method_name),
                                             recv_expr, sizeof(recv_expr))) {
            i++;
            continue;
        }
        if (recv_start > 0) {
            size_t rs = recv_start;
            while (rs > 0 && (src[rs - 1] == ' ' || src[rs - 1] == '\t')) rs--;
            if (rs > 0 && src[rs - 1] == '.') { i++; continue; }
            if (rs >= 2 && src[rs - 2] == '-' && src[rs - 1] == '>') { i++; continue; }
        }
        if (!cc__resolve_generic_ufcs_receiver_type(recv_expr, src, sep_pos,
                                                    vars, var_count, fields, field_count,
                                                    recv_type, sizeof(recv_type), &recv_is_ptr)) {
            i++;
            continue;
        }
        cc__copy_type_base(recv_type_base, sizeof(recv_type_base), recv_type);
        cc__normalize_bool_family_type(recv_type_base, sizeof(recv_type_base));
        if (reg && !cc__type_is_known_ufcs_family_base(recv_type_base)) {
            const char* alias = cc_type_registry_lookup_alias(reg, recv_type_base);
            if (alias && *alias) {
                size_t star_count = 0;
                size_t recv_len = strlen(recv_type);
                while (recv_len > 0 && recv_type[recv_len - 1] == '*') {
                    star_count++;
                    recv_len--;
                }
                snprintf(recv_type, sizeof(recv_type), "%s", alias);
                while (star_count-- > 0 && strlen(recv_type) + 1 < sizeof(recv_type)) strcat(recv_type, "*");
                cc__copy_type_base(recv_type_base, sizeof(recv_type_base), recv_type);
                cc__normalize_bool_family_type(recv_type_base, sizeof(recv_type_base));
            }
        }
        if (!cc__type_is_known_ufcs_family_base(recv_type_base)) {
            char alias_type[256];
            if (cc__lookup_scoped_type_alias(src, sep_pos, recv_type_base, alias_type, sizeof(alias_type))) {
                size_t star_count = 0;
                size_t recv_len = strlen(recv_type);
                while (recv_len > 0 && recv_type[recv_len - 1] == '*') {
                    star_count++;
                    recv_len--;
                }
                snprintf(recv_type, sizeof(recv_type), "%s", alias_type);
                while (star_count-- > 0 && strlen(recv_type) + 1 < sizeof(recv_type)) strcat(recv_type, "*");
                cc__copy_type_base(recv_type_base, sizeof(recv_type_base), recv_type);
                cc__normalize_bool_family_type(recv_type_base, sizeof(recv_type_base));
            }
        }
        parser_vec = cc__type_is_parser_vec(recv_type_base);
        parser_map = cc__type_is_parser_map(recv_type_base);
        command_like = (strcmp(recv_type_base, "CCCommand") == 0 ||
                        strcmp(recv_type_base, "CCCommand*") == 0);
        file_like = (strcmp(recv_type_base, "CCFile") == 0 ||
                     strcmp(recv_type_base, "CCFile*") == 0);
        /* CCArena UFCS is fully owned by its @comptime hook (cc_arena_lower_c).
           We still tag arena_like for the gate below so its calls are skipped
           by this text pass and lowered by the AST/text-fallback pipeline. */
        arena_like = (strcmp(recv_type_base, "CCArena") == 0 ||
                      strcmp(recv_type_base, "CCArena*") == 0);
        string_like = (strcmp(recv_type_base, "CCString") == 0 ||
                       strcmp(recv_type_base, "CCString*") == 0);
        slice_like = (strcmp(recv_type_base, "CCSlice") == 0 ||
                      strcmp(recv_type_base, "CCSlice*") == 0 ||
                      strcmp(recv_type_base, "CCSliceUnique") == 0 ||
                      strcmp(recv_type_base, "CCSliceUnique*") == 0 ||
                      strcmp(recv_type_base, "CCSliceShared") == 0 ||
                      strcmp(recv_type_base, "CCSliceShared*") == 0);
        if (slice_like && !cc__is_slice_ufcs_method(method_name)) {
            i++;
            continue;
        }
        nursery_like = (strcmp(recv_type_base, "CCNursery") == 0 ||
                        strcmp(recv_type_base, "CCNursery*") == 0);
        chan_tx = (strncmp(recv_type_base, "CCChanTx_", 9) == 0 ||
                   strcmp(recv_type_base, "CCChanTx") == 0 ||
                   strcmp(recv_type_base, "CCChanTx*") == 0);
        chan_rx = (strncmp(recv_type_base, "CCChanRx_", 9) == 0 ||
                   strcmp(recv_type_base, "CCChanRx") == 0 ||
                   strcmp(recv_type_base, "CCChanRx*") == 0);
        if (!(strncmp(recv_type_base, "CCVec_", 6) == 0 ||
              strncmp(recv_type_base, "Map_", 4) == 0 ||
              parser_vec || parser_map || command_like || file_like || arena_like || string_like || slice_like || nursery_like ||
              chan_tx || chan_rx ||
              strncmp(recv_type_base, "CCResult_", 9) == 0)) {
            i++;
            continue;
        }
        if ((chan_tx || chan_rx) && cc__ufcs_preceded_by_await(src, recv_start)) {
            i++;
            continue;
        }
        if (nursery_like || arena_like) {
            /* CCNursery and CCArena have registered @comptime hooks
               (cc_nursery_lower_c, cc_arena_lower_c) whose return values are
               ordinary struct/scalar-typed C, so the CC_PARSER_MODE stub
               member signatures are compatible with the real lowered
               callables. Skip rewriting here so the hook is authoritative:
               rewriting would bypass the hook and silently produce default
               `cc_<family>_<method>` callees that may not exist (e.g.
               comptime_type_arena_hooks_smoke block 2: `arena.avail()` must
               lower via the hook to `arena_avail`, not `cc_arena_avail`).
               CCFile/CCCommand/CCString also have hooks now (Phase 1b/1c)
               but their real methods return structured types (CCResult
               families, CCString*) that the default parser-mode member
               stubs can't model without a wider type-registry upgrade; for
               now the preprocess-text path keeps eagerly lowering them to
               avoid parser-mode stub/signature mismatches. See the
               `file-command-string-skip` follow-up for the larger fix. */
            i++;
            continue;
        }
        if (chan_tx || chan_rx) {
            /* Channel UFCS now stays alive for the parser/TCC + later type-owned path. */
            i++;
            continue;
        }
        family_by_value = (strncmp(recv_type_base, "CCResult_", 9) == 0);
        family_pass_direct = parser_map || (strncmp(recv_type_base, "Map_", 4) == 0);
        if (strncmp(recv_type_base, "CCResult_", 9) == 0) {
            if (!(strcmp(method_name, "value") == 0 ||
                  strcmp(method_name, "error") == 0 ||
                  strcmp(method_name, "unwrap_or") == 0 ||
                  strcmp(method_name, "is_ok") == 0 ||
                  strcmp(method_name, "is_err") == 0)) {
                i++;
                continue;
            }
            if (strcmp(method_name, "value") == 0) {
                strcpy(method_name, "unwrap");
            } else if (strcmp(method_name, "error") == 0) {
                strcpy(method_name, "error");
            }
        }
        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, recv_start - last_emit);
        {
            char concrete_type[256];
            char callee_family[256];
            int is_result_bool_method = 0;
            concrete_type[0] = '\0';
            callee_family[0] = '\0';
            if (strncmp(recv_type_base, "CCResult_", 9) == 0 &&
                (strcmp(method_name, "is_ok") == 0 || strcmp(method_name, "is_err") == 0)) {
                is_result_bool_method = 1;
            }
            if (parser_vec || parser_map) {
                const char* lp = strchr(recv_type_base, '(');
                const char* rp = lp ? strrchr(recv_type_base, ')') : NULL;
                if (parser_vec && lp && rp && rp > lp + 1) {
                    char mangled_elem[128];
                    cc__mangle_type_name(lp + 1, (size_t)(rp - (lp + 1)), mangled_elem, sizeof(mangled_elem));
                    snprintf(concrete_type, sizeof(concrete_type), "CCVec_%s", mangled_elem);
                } else if (parser_map && lp && rp && rp > lp + 1) {
                    const char* comma = NULL;
                    int par = 0, br = 0, brc = 0;
                    for (const char* q = lp + 1; q < rp; q++) {
                        if (*q == '(') par++;
                        else if (*q == ')' && par > 0) par--;
                        else if (*q == '[') br++;
                        else if (*q == ']' && br > 0) br--;
                        else if (*q == '{') brc++;
                        else if (*q == '}' && brc > 0) brc--;
                        else if (*q == ',' && par == 0 && br == 0 && brc == 0) {
                            comma = q;
                            break;
                        }
                    }
                    if (comma) {
                        const char* key_s = lp + 1;
                        const char* key_e = comma;
                        const char* val_s = comma + 1;
                        const char* val_e = rp;
                        char mangled_key[128];
                        char mangled_val[128];
                        cc__trim_span_ws(&key_s, &key_e);
                        cc__trim_span_ws(&val_s, &val_e);
                        cc__mangle_type_name(key_s, (size_t)(key_e - key_s), mangled_key, sizeof(mangled_key));
                        cc__mangle_type_name(val_s, (size_t)(val_e - val_s), mangled_val, sizeof(mangled_val));
                        snprintf(concrete_type, sizeof(concrete_type), "Map_%s_%s", mangled_key, mangled_val);
                    }
                }
            }
            if (is_result_bool_method) {
                cc_sb_append_cstr(&out, &out_len, &out_cap,
                                  strcmp(method_name, "is_ok") == 0 ? "cc_is_ok" : "cc_is_err");
            } else if (channel_callee) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, channel_callee);
            } else if (concrete_type[0]) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, concrete_type);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "_");
                cc_sb_append_cstr(&out, &out_len, &out_cap, method_name);
            } else if (parser_vec && parser_safe) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "__cc_vec_generic_");
                cc_sb_append_cstr(&out, &out_len, &out_cap, method_name);
            } else if (parser_map && parser_safe) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "__cc_map_generic_");
                cc_sb_append_cstr(&out, &out_len, &out_cap, method_name);
            } else if (command_like) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_command_");
                cc_sb_append_cstr(&out, &out_len, &out_cap, method_name);
            } else if (file_like) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_file_");
                cc_sb_append_cstr(&out, &out_len, &out_cap, method_name);
            } else if (arena_like) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_arena_");
                cc_sb_append_cstr(&out, &out_len, &out_cap, method_name);
            } else if (string_like) {
                const char* string_method = method_name;
                if (strcmp(method_name, "append") == 0) string_method = "push";
                cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_string_");
                cc_sb_append_cstr(&out, &out_len, &out_cap, string_method);
            } else if (slice_like) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_slice_");
                cc_sb_append_cstr(&out, &out_len, &out_cap, method_name);
            } else if (nursery_like) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_nursery_");
                cc_sb_append_cstr(&out, &out_len, &out_cap, method_name);
            } else {
                cc__copy_type_base(callee_family, sizeof(callee_family), recv_type_base);
                cc_sb_append_cstr(&out, &out_len, &out_cap, callee_family[0] ? callee_family : recv_type_base);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "_");
                cc_sb_append_cstr(&out, &out_len, &out_cap, method_name);
            }
        }
        cc_sb_append_cstr(&out, &out_len, &out_cap, "(");
        if (channel_callee || family_by_value || family_pass_direct || nursery_like || recv_is_ptr) {
            cc_sb_append_cstr(&out, &out_len, &out_cap, recv_expr);
        } else {
            cc_sb_append_cstr(&out, &out_len, &out_cap, "&");
            cc_sb_append_cstr(&out, &out_len, &out_cap, recv_expr);
        }
        {
            size_t args_start = paren_pos + 1;
            size_t args_end = paren_end;
            while (args_start < args_end &&
                   (src[args_start] == ' ' || src[args_start] == '\t' || src[args_start] == '\n' || src[args_start] == '\r')) {
                args_start++;
            }
            while (args_end > args_start &&
                   (src[args_end - 1] == ' ' || src[args_end - 1] == '\t' || src[args_end - 1] == '\n' || src[args_end - 1] == '\r')) {
                args_end--;
            }
            if (args_end > args_start) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                cc_sb_append(&out, &out_len, &out_cap, src + args_start, args_end - args_start);
            }
        }
        cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
        last_emit = paren_end + 1;
        i = paren_end + 1;
    }
    if (last_emit == 0) return NULL;
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Parser-survival text rewriter for concrete family UFCS. Kept narrow so TCC's
   stub parse sees lowered receiver forms for fragile nested contexts; the AST
   UFCS pass remains authoritative for everything else. */
char* cc_rewrite_generic_family_ufcs_parser_safe(const char* src, size_t n) {
    return cc__rewrite_generic_family_ufcs_impl(src, n, 1);
}

/* Skip leading decl-specs (storage-class / type-qualifier keywords and
 * `@attribute` tokens) that sit in front of a base type, so the
 * result-type / optional-type prefix scanners land on the first real
 * type token.
 *
 * History (metaclass fix — follow-up bug [F5]):
 *
 *   This helper used to enumerate a short list of plain C keywords
 *   (`typedef static inline extern const volatile`) and nothing else.
 *   Every time the language grew a new `@attribute` (`@async`,
 *   `@noblock`, `@latency_sensitive`, `@errhandler`, `@destroy`, ...)
 *   the caller in `cc__rewrite_result_types` silently pulled the
 *   attribute INTO the mangled result-type name, so e.g.
 *     @noblock static RedisReply !>(CCError) fn(...)
 *   produced `CCResult_noblock_static_RedisReply_CCError fn(...)` and
 *   `@noblock` never reached the decl-attribute hook — `pass_autoblock`
 *   then wrapped every call site in `cc_run_blocking_task_intptr`.
 *
 *   The zoom-out is that backward-scan-for-type-prefix is a generic
 *   "walk past leading decl-specs" operation and the set of legal
 *   decl-specs is: (a) the standard C storage-class / type-qualifier
 *   keywords, (b) any `@IDENT` attribute (by construction these are
 *   never part of a type name).  This function now matches both.
 *   Adding a new `@attribute` to the language requires no change here.
 */
static size_t cc__skip_leading_decl_specs(const char* s, size_t ty_start) {
    size_t p = ty_start;
    if (!s) return p;
    /* Known C storage-class / type-qualifier keywords.  Attributes
     * (`@IDENT`) are handled generically below so new ones don't need
     * to be added here. */
    static const char* kw[] = {
        "typedef", "static", "inline", "extern", "const", "volatile",
        "register", "auto", "restrict", "_Atomic", "_Noreturn",
        "_Thread_local", "thread_local",
        NULL
    };
    while (s[p] == ' ' || s[p] == '\t') p++;
    for (;;) {
        int matched = 0;
        /* `@IDENT` — any attribute token.  We consume the `@` plus the
         * identifier that follows; we do NOT consume any `(...)` that
         * may trail an attribute (e.g. `@errhandler(CCError e)`) — that
         * argument list belongs to the attribute, not to the type, and
         * the result-type caller re-emits the entire skipped prefix
         * verbatim so this is fine either way.  Bare `@IDENT` is the
         * common case (`@async`, `@noblock`, `@latency_sensitive`,
         * `@destroy`). */
        if (s[p] == '@' && cc_is_ident_char_local(s[p + 1]) &&
            !(s[p + 1] >= '0' && s[p + 1] <= '9')) {
            p++; /* '@' */
            while (cc_is_ident_char_local(s[p])) p++;
            matched = 1;
        } else {
            for (int k = 0; kw[k]; k++) {
                size_t kn = strlen(kw[k]);
                if (strncmp(s + p, kw[k], kn) == 0 && !cc_is_ident_char_local(s[p + kn])) {
                    p += kn;
                    matched = 1;
                    break;
                }
            }
        }
        if (!matched) break;
        /* Also skip line/block comments between decl-specs so prose
         * like `static COMMENT int !>(E) ...` (where COMMENT is a
         * `slash-star ... star-slash` block) doesn't drop us onto the
         * generic fallback. */
        for (;;) {
            while (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n') p++;
            if (s[p] == '/' && s[p + 1] == '/') {
                p += 2;
                while (s[p] && s[p] != '\n') p++;
                continue;
            }
            if (s[p] == '/' && s[p + 1] == '*') {
                p += 2;
                while (s[p] && !(s[p] == '*' && s[p + 1] == '/')) p++;
                if (s[p]) p += 2;
                continue;
            }
            break;
        }
    }
    return p;
}

/* Mangle a type name for use in CCOptional_T or CCResult_T_E.
   - Strips leading/trailing whitespace
   - Replaces spaces with underscores
   - Replaces '*' with 'ptr'
   - Replaces '[' and ']' with '_' */
static void cc__mangle_type_name(const char* src, size_t len, char* out, size_t out_sz) {
    cc_result_spec_mangle_type(src, len, out, out_sz);
}

/* Check if a token is a known C base type (for error detection) */
static int cc__is_known_base_type(const char* s, size_t len) {
    static const char* types[] = {
        "int", "char", "void", "bool", "float", "double",
        "long", "short", "unsigned", "signed", "size_t",
        "int8_t", "int16_t", "int32_t", "int64_t",
        "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "intptr_t", "uintptr_t", "ptrdiff_t", NULL
    };
    for (int i = 0; types[i]; i++) {
        if (strlen(types[i]) == len && strncmp(s, types[i], len) == 0) return 1;
    }
    return 0;
}

/* Retired: optional types (`T?`) are gone. This pass now serves purely as a
 * diagnostic emitter — when it spots a `T?` sigil in a type context it reports
 * the retirement and refuses to continue. Non-type uses of `?` (ternary `?:`,
 * CC `?>` suffix, `??` operators) are untouched. */
static char* cc__rewrite_optional_types(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return NULL;

    size_t i = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);

    while (i < n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;

        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;

        if (c == '?' && c2 != ':' && c2 != '?' && c2 != '>') {
            if (i > 0) {
                char prev = src[i - 1];
                if (cc_is_ident_char(prev) || prev == ')' || prev == ']' || prev == '>') {
                    size_t ident_end = i;
                    size_t ident_start = ident_end;
                    while (ident_start > 0 && cc_is_ident_char(src[ident_start - 1])) ident_start--;
                    size_t ident_len = ident_end - ident_start;

                    if (ident_len > 0 && ident_len < 64) {
                        char first_char = src[ident_start];
                        int looks_like_type = (first_char >= 'A' && first_char <= 'Z') ||
                                              cc__is_known_base_type(src + ident_start, ident_len);
                        if (looks_like_type) {
                            char name[64];
                            size_t vlen = ident_len < sizeof(name) - 1 ? ident_len : sizeof(name) - 1;
                            memcpy(name, src + ident_start, vlen);
                            name[vlen] = '\0';

                            char rel[1024];
                            cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>",
                                                                rel, sizeof(rel)),
                                            scan.line, scan.col, "syntax",
                                            "optional type '%s?' has been retired", name);
                            fprintf(stderr,
                                    "  hint: replace with one of:\n"
                                    "    - nullable pointer `%s*` (NULL = absent)\n"
                                    "    - `bool op(%s* out)` out-parameter\n"
                                    "    - in-band sentinel (empty slice / -1 / etc.)\n"
                                    "    - result type `%s !>(CCError)` for fallible operations\n"
                                    "  see cc/include/ccc/DEPRECATIONS.md for the full migration matrix\n",
                                    name, name, name);
                            return NULL;
                        }
                    }
                }
            }
        }

        i++;
    }

    /* No T? found — return NULL so the pass-chain keeps the original buffer. */
    return NULL;
}


/* Rewrite result constructors for parser mode:
   - `cc_ok_CCResult_T_E(v)` -> `__CC_RESULT_OK(0, 0, v)`
   - `cc_err_CCResult_T_E(e)` -> `__CC_RESULT_ERR(0, 0, e)`
   This avoids typed constructors during TCC parsing while preserving arg checking. */
static char* cc__rewrite_result_constructors(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    size_t i = 0;
    size_t last_emit = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);
    
    while (i < n) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        
        const char* ok_prefix = "cc_ok_CCResult_";
        const char* err_prefix = "cc_err_CCResult_";
        size_t ok_len = 15;  /* strlen("cc_ok_CCResult_") */
        size_t err_len = 16; /* strlen("cc_err_CCResult_") */
        
        int is_ok = (i + ok_len < n && strncmp(src + i, ok_prefix, ok_len) == 0);
        int is_err = (i + err_len < n && strncmp(src + i, err_prefix, err_len) == 0);
        
        if (is_ok || is_err) {
            /* Ensure not part of a longer identifier */
            if (i > 0 && cc_is_ident_char(src[i - 1])) {
                i++;
                continue;
            }
            
            size_t prefix_len = is_ok ? ok_len : err_len;
            size_t j = i + prefix_len;
            
            /* Skip mangled type name */
            while (j < n && cc_is_ident_char(src[j])) j++;
            
            /* Skip whitespace to find '(' */
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j >= n || src[j] != '(') {
                i++;
                continue;
            }
            
            /* Parse argument list */
            size_t args_start = j + 1;
            size_t k = args_start;
            int depth = 1;
            int in_s = 0, in_c = 0;
            while (k < n && depth > 0) {
                char ch = src[k];
                if (in_s) { if (ch == '\\' && k + 1 < n) k++; else if (ch == '"') in_s = 0; k++; continue; }
                if (in_c) { if (ch == '\\' && k + 1 < n) k++; else if (ch == '\'') in_c = 0; k++; continue; }
                if (ch == '"') { in_s = 1; k++; continue; }
                if (ch == '\'') { in_c = 1; k++; continue; }
                if (ch == '(') depth++;
                else if (ch == ')') depth--;
                if (depth > 0) k++;
            }
            if (depth != 0) {
                i++;
                continue;
            }
            
            size_t args_end = k;
            size_t call_end = k + 1;
            
            /* Emit everything up to function name.
               Route through the parser-mode helper macros so TCC sees the value/error
               expression in the AST, but we avoid spelling a raw generic compound
               literal inline in control-flow contexts like `return ...;` inside switch. */
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
            if (is_ok) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_RESULT_OK(0, 0, ");
            } else {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_RESULT_ERR(0, 0, ");
            }
            cc_sb_append(&out, &out_len, &out_cap, src + args_start, args_end - args_start);
            cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
            last_emit = call_end;
            i = call_end;
            continue;
        }
        
        i++;
    }
    
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* ============================================================================
 * Generic container syntax lowering: CCVec<T> -> CCVec_T, Map<K,V> -> Map_K_V
 * Also: cc_vec_new<T>(...) -> CCVec_T_init(...), map_new<K,V>(...) -> Map_K_V_init(...)
 * Legacy Vec/vec_new spellings remain accepted during migration.
 * ============================================================================ */

/* Normalize T[:] -> CCSlice, T[:!] -> CCSliceUnique in a type string */
static void cc__normalize_slice_in_type(char* type, size_t type_sz) {
    if (!type || !strstr(type, "[:")) return;
    if (strstr(type, "!]"))
        snprintf(type, type_sz, "CCSliceUnique");
    else
        snprintf(type, type_sz, "CCSlice");
}

static void cc__trim_type_string(char* type) {
    size_t len;
    size_t start = 0;
    if (!type) return;
    len = strlen(type);
    while (start < len && (type[start] == ' ' || type[start] == '\t' ||
                           type[start] == '\n' || type[start] == '\r')) start++;
    while (len > start && (type[len - 1] == ' ' || type[len - 1] == '\t' ||
                           type[len - 1] == '\n' || type[len - 1] == '\r')) len--;
    if (start > 0 && len > start) memmove(type, type + start, len - start);
    type[len - start] = '\0';
}

static void cc__canonicalize_container_param_type(char* type, size_t type_sz) {
    char inner[256];
    char canonical_key[256];
    char canonical_val[256];
    char mangled_inner[128];
    char mangled_key[128];
    char mangled_val[128];
    char* comma = NULL;
    int depth = 0;

    if (!type || type_sz == 0) return;
    cc__trim_type_string(type);
    cc__normalize_slice_in_type(type, type_sz);

    if ((strncmp(type, "Vec::[", 6) == 0 && type[strlen(type) - 1] == ']') ||
        (strncmp(type, "Vec<", 4) == 0 && type[strlen(type) - 1] == '>') ||
        (strncmp(type, "CCVec::[", 8) == 0 && type[strlen(type) - 1] == ']') ||
        (strncmp(type, "CCVec<", 6) == 0 && type[strlen(type) - 1] == '>') ||
        (strncmp(type, "__CC_VEC(", 9) == 0 && type[strlen(type) - 1] == ')')) {
        size_t prefix = (strncmp(type, "__CC_VEC(", 9) == 0) ? 9 :
                        ((strncmp(type, "CCVec::[", 8) == 0) ? 8 :
                         ((strncmp(type, "CCVec<", 6) == 0) ? 6 :
                          ((type[3] == ':') ? 6 : 4)));
        size_t inner_len = strlen(type) - prefix - 1;
        if (inner_len >= sizeof(inner)) inner_len = sizeof(inner) - 1;
        memcpy(inner, type + prefix, inner_len);
        inner[inner_len] = '\0';
        cc__canonicalize_container_param_type(inner, sizeof(inner));
        cc__mangle_container_type_param(inner, strlen(inner), mangled_inner, sizeof(mangled_inner));
        snprintf(type, type_sz, "CCVec_%s", mangled_inner);
        return;
    }

    if ((strncmp(type, "Map::[", 6) == 0 && type[strlen(type) - 1] == ']') ||
        (strncmp(type, "Map<", 4) == 0 && type[strlen(type) - 1] == '>') ||
        (strncmp(type, "__CC_MAP(", 9) == 0 && type[strlen(type) - 1] == ')')) {
        size_t prefix = (strncmp(type, "__CC_MAP(", 9) == 0) ? 9 : ((type[3] == ':') ? 6 : 4);
        const char* params = type + prefix;
        size_t params_len = strlen(type) - prefix - 1;
        size_t key_len;
        size_t val_off;
        size_t val_len;

        for (size_t i = 0; i < params_len; i++) {
            char c = params[i];
            if (c == '<' || c == '[' || c == '(' || c == '{') depth++;
            else if (c == '>' || c == ']' || c == ')' || c == '}') depth--;
            else if (c == ',' && depth == 0) {
                comma = (char*)params + i;
                break;
            }
        }
        if (!comma) return;

        key_len = (size_t)(comma - params);
        if (key_len >= sizeof(canonical_key)) key_len = sizeof(canonical_key) - 1;
        memcpy(canonical_key, params, key_len);
        canonical_key[key_len] = '\0';

        val_off = (size_t)(comma - params) + 1;
        val_len = params_len - val_off;
        if (val_len >= sizeof(canonical_val)) val_len = sizeof(canonical_val) - 1;
        memcpy(canonical_val, params + val_off, val_len);
        canonical_val[val_len] = '\0';

        cc__canonicalize_container_param_type(canonical_key, sizeof(canonical_key));
        cc__canonicalize_container_param_type(canonical_val, sizeof(canonical_val));
        cc__mangle_container_type_param(canonical_key, strlen(canonical_key), mangled_key, sizeof(mangled_key));
        cc__mangle_container_type_param(canonical_val, strlen(canonical_val), mangled_val, sizeof(mangled_val));
        snprintf(type, type_sz, "Map_%s_%s", mangled_key, mangled_val);
        return;
    }
}

static void cc__canonicalize_ufcs_alias_target(char* out, size_t out_sz, const char* type_src) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!type_src || !type_src[0]) return;
    snprintf(out, out_sz, "%s", type_src);
    cc__canonicalize_container_param_type(out, out_sz);
    if (out[0] && strcmp(out, type_src) != 0) return;
    cc__normalize_ufcs_type_name(out, out_sz, type_src);
    cc__canonicalize_container_param_type(out, out_sz);
}

/* Mangle a type parameter for container names (int -> int, char[:] -> charslice, etc.) */
static void cc__mangle_container_type_param(const char* src, size_t len, char* out, size_t out_sz) {
    if (!src || len == 0 || !out || out_sz == 0) { if (out && out_sz > 0) out[0] = 0; return; }
    /* Trim whitespace */
    size_t i = 0;
    while (i < len && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r')) i++;
    size_t e = len;
    while (e > i && (src[e - 1] == ' ' || src[e - 1] == '\t' || src[e - 1] == '\n' || src[e - 1] == '\r')) e--;
    size_t j = 0;
    for (size_t k = i; k < e && j < out_sz - 1; k++) {
        char c = src[k];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        if (c == '*') { 
            if (j + 3 < out_sz) { out[j++] = 'p'; out[j++] = 't'; out[j++] = 'r'; }
        } else if (c == '[') {
            if (j + 5 < out_sz && k + 2 < e && src[k + 1] == ':' && src[k + 2] == ']') {
                out[j++] = 's'; out[j++] = 'l'; out[j++] = 'i'; out[j++] = 'c'; out[j++] = 'e';
                k += 2;
            } else {
                out[j++] = '_';
            }
        } else if (c == ']' || c == ',' || c == '<' || c == '>') {
            out[j++] = '_';
        } else {
            out[j++] = c;
        }
    }
    while (j > 0 && out[j - 1] == '_') j--;
    out[j] = 0;
}

/* Find matching '>' for '<' at position langle. Returns 1 on success. */
static int cc__find_matching_angle(const char* b, size_t bl, size_t langle, size_t* out_rangle) {
    if (!b || langle >= bl || b[langle] != '<') return 0;
    int ang = 1, par = 0, brk = 0, br = 0;
    int ins = 0; char q = 0;
    int in_lc = 0, in_bc = 0;
    for (size_t p = langle + 1; p < bl; p++) {
        char ch = b[p];
        char ch2 = (p + 1 < bl) ? b[p + 1] : 0;
        if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; p++; } continue; }
        if (ins) { if (ch == '\\' && p + 1 < bl) { p++; continue; } if (ch == q) ins = 0; continue; }
        if (ch == '/' && ch2 == '/') { in_lc = 1; p++; continue; }
        if (ch == '/' && ch2 == '*') { in_bc = 1; p++; continue; }
        if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
        if (ch == '(') par++;
        else if (ch == ')') { if (par) par--; }
        else if (ch == '[') brk++;
        else if (ch == ']') { if (brk) brk--; }
        else if (ch == '{') br++;
        else if (ch == '}') { if (br) br--; }
        else if (ch == '<' && par == 0 && brk == 0 && br == 0) ang++;
        else if (ch == '>' && par == 0 && brk == 0 && br == 0) {
            ang--;
            if (ang == 0) { if (out_rangle) *out_rangle = p; return 1; }
        }
    }
    return 0;
}

/* Find matching ']' for '[' at position lbracket. Returns 1 on success.
   Handles nesting of [], (), {}, <>, strings, and comments. */
static int cc__find_matching_bracket(const char* b, size_t bl, size_t lbracket, size_t* out_rbracket) {
    if (!b || lbracket >= bl || b[lbracket] != '[') return 0;
    int brk = 1, par = 0, ang = 0, br = 0;
    int ins = 0; char q = 0;
    int in_lc = 0, in_bc = 0;
    for (size_t p = lbracket + 1; p < bl; p++) {
        char ch = b[p];
        char ch2 = (p + 1 < bl) ? b[p + 1] : 0;
        if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; p++; } continue; }
        if (ins) { if (ch == '\\' && p + 1 < bl) { p++; continue; } if (ch == q) ins = 0; continue; }
        if (ch == '/' && ch2 == '/') { in_lc = 1; p++; continue; }
        if (ch == '/' && ch2 == '*') { in_bc = 1; p++; continue; }
        if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
        if (ch == '(') par++;
        else if (ch == ')') { if (par) par--; }
        else if (ch == '[') brk++;
        else if (ch == ']') {
            brk--;
            if (brk == 0 && par == 0) { if (out_rbracket) *out_rbracket = p; return 1; }
        }
        else if (ch == '{') br++;
        else if (ch == '}') { if (br) br--; }
        else if (ch == '<') ang++;
        else if (ch == '>') { if (ang) ang--; }
    }
    return 0;
}

/* Rewrite generic container syntax:
   - CCVec<T> / CCVec::[T] -> __CC_VEC(T_mangled)  (parser-safe macro)
   - Map<K, V> / Map::[K, V] -> __CC_MAP(K_mangled, V_mangled)*  (parser-safe macro)
   - cc_vec_new<T>(...) / cc_vec_new::[T](...) -> __CC_VEC_INIT(T_mangled, ...)
   - map_new<K, V>(...) / map_new::[K, V](...) -> __CC_MAP_INIT(K_mangled, V_mangled, ...)
   CCVec::[T] is the preferred syntax; legacy Vec::[T] and <T> spellings are
   accepted for backward compatibility during migration.
   Also tracks variable declarations for UFCS resolution. */
char* cc_rewrite_generic_containers(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return NULL;
    
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    size_t last_emit = 0;
    CCScannerState scanner;
    cc_scanner_init(&scanner);
    
    CCTypeRegistry* reg = cc_type_registry_get_global();
    
    while (i < n) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scanner, src, n, &i)) continue;
        
        /* Look for CCVec<, CCVec::[, legacy Vec<, Vec::[, cc_vec_new<, cc_vec_new::[,
           legacy vec_new<, vec_new::[, and map_new variants. */
        int is_vec_type = 0, is_map_type = 0, is_vec_new = 0, is_map_new = 0;
        int use_bracket = 0;
        size_t kw_start = i;
        size_t kw_len = 0;
        
        if (i == 0 || !cc_is_ident_char(src[i - 1])) {
            if (i + 8 <= n && memcmp(src + i, "CCVec::[", 8) == 0) {
                is_vec_type = 1; kw_len = 5; use_bracket = 1;
            } else if (i + 6 <= n && memcmp(src + i, "CCVec<", 6) == 0) {
                is_vec_type = 1; kw_len = 5;
            } else if (i + 6 <= n && memcmp(src + i, "Vec::[", 6) == 0) {
                is_vec_type = 1; kw_len = 3; use_bracket = 1;
            } else if (i + 4 <= n && memcmp(src + i, "Vec<", 4) == 0) {
                is_vec_type = 1; kw_len = 3;
            } else if (i + 6 <= n && memcmp(src + i, "Map::[", 6) == 0) {
                is_map_type = 1; kw_len = 3; use_bracket = 1;
            } else if (i + 4 <= n && memcmp(src + i, "Map<", 4) == 0) {
                is_map_type = 1; kw_len = 3;
            } else if (i + 13 <= n && memcmp(src + i, "cc_vec_new::[", 13) == 0) {
                is_vec_new = 1; kw_len = 10; use_bracket = 1;
            } else if (i + 11 <= n && memcmp(src + i, "cc_vec_new<", 11) == 0) {
                is_vec_new = 1; kw_len = 10;
            } else if (i + 10 <= n && memcmp(src + i, "vec_new::[", 10) == 0) {
                is_vec_new = 1; kw_len = 7; use_bracket = 1;
            } else if (i + 8 <= n && memcmp(src + i, "vec_new<", 8) == 0) {
                is_vec_new = 1; kw_len = 7;
            } else if (i + 10 <= n && memcmp(src + i, "map_new::[", 10) == 0) {
                is_map_new = 1; kw_len = 7; use_bracket = 1;
            } else if (i + 8 <= n && memcmp(src + i, "map_new<", 8) == 0) {
                is_map_new = 1; kw_len = 7;
            }
        }
        
        if (is_vec_type || is_map_type || is_vec_new || is_map_new) {
            size_t delim_start = use_bracket ? kw_start + kw_len + 2 : kw_start + kw_len;
            size_t delim_end = 0;
            int found = 0;
            
            if (use_bracket) {
                found = cc__find_matching_bracket(src, n, delim_start, &delim_end);
            } else {
                found = cc__find_matching_angle(src, n, delim_start, &delim_end);
            }
            
            if (!found) {
                const char* what = is_vec_type ? "CCVec" : is_map_type ? "Map" : is_vec_new ? "cc_vec_new" : "map_new";
                char rel[1024];
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                        scanner.line, scanner.col, "type", "unclosed '%s' generic - missing '%c'",
                        what, use_bracket ? ']' : '>');
                fprintf(stderr, "  hint: generic syntax is '%s::[Type]' e.g., 'CCVec::[int]'\n", what);
                free(out);
                return NULL;
            }
            
            /* Extract type parameters (between the opening and closing delimiters) */
            const char* params = src + delim_start + 1;
            size_t params_len = delim_end - delim_start - 1;
            
            char mangled[256] = {0};
            char elem_type[128] = {0};
            char key_type[128] = {0};
            char val_type[128] = {0};
            char orig_elem_type[128] = {0};  /* Original type for macro emission */
            char orig_key_type[128] = {0};
            char orig_val_type[128] = {0};
            
            if (is_vec_type || is_vec_new) {
                /* Single type parameter - save both original and mangled */
                /* Original (trimmed): copy params as-is for macro */
                size_t ti = 0;
                while (ti < params_len && (params[ti] == ' ' || params[ti] == '\t')) ti++;
                size_t te = params_len;
                while (te > ti && (params[te-1] == ' ' || params[te-1] == '\t')) te--;
                size_t orig_len = te - ti;
                if (orig_len >= sizeof(orig_elem_type)) orig_len = sizeof(orig_elem_type) - 1;
                memcpy(orig_elem_type, params + ti, orig_len);
                orig_elem_type[orig_len] = 0;
                cc__canonicalize_container_param_type(orig_elem_type, sizeof(orig_elem_type));
                cc__normalize_slice_in_type(orig_elem_type, sizeof(orig_elem_type));
                
                cc__mangle_container_type_param(orig_elem_type, strlen(orig_elem_type), elem_type, sizeof(elem_type));
                snprintf(mangled, sizeof(mangled), "CCVec_%s", elem_type);
                
                if (reg) {
                    cc_type_registry_add_vec(reg, orig_elem_type, mangled);
                }
            } else {
                /* Two type parameters: K, V */
                const char* comma = NULL;
                int depth = 0;
                for (size_t k = 0; k < params_len; k++) {
                    char pc = params[k];
                    if (pc == '<' || pc == '[') depth++;
                    else if (pc == '>' || pc == ']') depth--;
                    else if (pc == ',' && depth == 0) {
                        comma = params + k;
                        break;
                    }
                }
                
                if (!comma) {
                    i++;
                    continue;
                }
                
                size_t k_len = (size_t)(comma - params);
                size_t v_start = (size_t)(comma - params) + 1;
                size_t v_len = params_len - v_start;
                
                /* Save original key type (trimmed) */
                {
                    size_t ti = 0;
                    while (ti < k_len && (params[ti] == ' ' || params[ti] == '\t')) ti++;
                    size_t te = k_len;
                    while (te > ti && (params[te-1] == ' ' || params[te-1] == '\t')) te--;
                    size_t orig_len = te - ti;
                    if (orig_len >= sizeof(orig_key_type)) orig_len = sizeof(orig_key_type) - 1;
                    memcpy(orig_key_type, params + ti, orig_len);
                    orig_key_type[orig_len] = 0;
                    cc__canonicalize_container_param_type(orig_key_type, sizeof(orig_key_type));
                }
                
                /* Save original val type (trimmed) */
                {
                    const char* vparams = params + v_start;
                    size_t ti = 0;
                    while (ti < v_len && (vparams[ti] == ' ' || vparams[ti] == '\t')) ti++;
                    size_t te = v_len;
                    while (te > ti && (vparams[te-1] == ' ' || vparams[te-1] == '\t')) te--;
                    size_t orig_len = te - ti;
                    if (orig_len >= sizeof(orig_val_type)) orig_len = sizeof(orig_val_type) - 1;
                    memcpy(orig_val_type, vparams + ti, orig_len);
                    orig_val_type[orig_len] = 0;
                    cc__canonicalize_container_param_type(orig_val_type, sizeof(orig_val_type));
                }
                
                cc__normalize_slice_in_type(orig_key_type, sizeof(orig_key_type));
                cc__normalize_slice_in_type(orig_val_type, sizeof(orig_val_type));
                
                cc__mangle_container_type_param(orig_key_type, strlen(orig_key_type), key_type, sizeof(key_type));
                cc__mangle_container_type_param(orig_val_type, strlen(orig_val_type), val_type, sizeof(val_type));
                snprintf(mangled, sizeof(mangled), "Map_%s_%s", key_type, val_type);
                
                if (reg) {
                    cc_type_registry_add_map(reg, orig_key_type, orig_val_type, mangled);
                }
            }
            
            /* Emit everything up to this point */
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, kw_start - last_emit);
            
            if (is_vec_type || is_map_type) {
                /* Emit parser-safe macro call instead of bare type name.
                   In parser mode: __CC_VEC(T) -> __CCVecGeneric
                   In real mode:   __CC_VEC(T) -> CCVec_T */
                if (is_vec_type) {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_VEC(");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, elem_type);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                } else {
                    /* Map uses pointers (the generated init returns a stable handle). */
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_MAP(");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, key_type);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, val_type);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ")*");
                }
                last_emit = delim_end + 1;
                
                /* Try to extract variable name for type registry */
                if (reg) {
                    size_t j = cc_skip_ws_and_comments(src, n, delim_end + 1);
                    if (j < n && (cc_is_ident_start(src[j]) || src[j] == '*')) {
                        /* Count any leading * so we can preserve pointer-ness in
                         * the seeded type (the text-based UFCS fallback looks at
                         * the seeded string to decide whether to pass `recv` or
                         * `&recv`).  For Map<K,V> the canonical form `Map_K_V`
                         * is already a struct typedef, and the declaration
                         * `__CC_MAP(K,V)* var` is a single pointer; seeding
                         * plain `Map_K_V` would lose that and the fallback
                         * would emit `&var` instead of `var`. */
                        int ptr_count = 0;
                        while (j < n && src[j] == '*') { j++; ptr_count++; }
                        /* Map<K,V> declarations always end with `)*` in the
                         * emitted macro form, so the first `*` belongs to the
                         * Map pointer typedef semantics.  Treat it as implicit
                         * pointer-ness by appending it to the seeded type. */
                        if (is_map_type && ptr_count == 0) ptr_count = 1;
                        j = cc_skip_ws_and_comments(src, n, j);
                        if (j < n && cc_is_ident_start(src[j])) {
                            size_t var_start = j;
                            while (j < n && cc_is_ident_char(src[j])) j++;
                            char var_name[128];
                            size_t vn_len = j - var_start;
                            if (vn_len < sizeof(var_name)) {
                                memcpy(var_name, src + var_start, vn_len);
                                var_name[vn_len] = 0;
                                if (ptr_count > 0) {
                                    char seeded[160];
                                    int stars = ptr_count > 8 ? 8 : ptr_count;
                                    snprintf(seeded, sizeof(seeded), "%s%.*s", mangled,
                                             stars, "********");
                                    cc_type_registry_add_var(reg, var_name, seeded);
                                } else {
                                    cc_type_registry_add_var(reg, var_name, mangled);
                                }
                            }
                        }
                    }
                }
            } else {
                /* cc_vec_new::[T](...) or map_new::[K,V](...) -> __CC_VEC_INIT/__CC_MAP_INIT macro */
                /* Find the opening paren and extract args */
                size_t j = cc_skip_ws_and_comments(src, n, delim_end + 1);
                if (j < n && src[j] == '(') {
                    /* Find the closing paren */
                    size_t paren_end = 0;
                    if (cc_find_matching_paren(src, n, j, &paren_end)) {
                        /* Emit macro call with type param first, then original args */
                        if (is_vec_new) {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_VEC_INIT(");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, elem_type);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                            /* Emit the arena argument(s) */
                            cc_sb_append(&out, &out_len, &out_cap, src + j + 1, paren_end - j - 1);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                        } else {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_MAP_INIT(");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, key_type);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, val_type);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                            /* Emit the arena argument */
                            cc_sb_append(&out, &out_len, &out_cap, src + j + 1, paren_end - j - 1);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                        }
                        last_emit = paren_end + 1;
                        i = paren_end + 1;
                        continue;
                    }
                }
                /* Fallback if no paren found */
                if (is_vec_new) {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_VEC_INIT(");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, elem_type);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ", NULL)");
                } else {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_MAP_INIT(");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, key_type);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, val_type);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ", NULL)");
                }
                last_emit = delim_end + 1;
            }
            
            i = delim_end + 1;
            continue;
        }
        
        i++; scanner.col++;
    }
    
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

static CCResultSpecTable cc__result_specs = {0};

static void cc__add_result_type(const char* ok, size_t ok_len, const char* err, size_t err_len,
                                const char* mangled_ok, const char* mangled_err) {
    (void)cc_result_spec_table_add(&cc__result_specs,
                                   ok, ok_len, err, err_len,
                                   mangled_ok, mangled_err);
}

/* Rewrite result types:
   - `T!>(E)` -> `__CC_RESULT(T_mangled, E_mangled)`
   The '!>' sigil is followed by error type in parentheses.
   This syntax is unambiguous and easy to parse.
   Also collects unique (T, E) pairs for later emission of CC_DECL_RESULT_SPEC calls.
   
   NOTE: Do NOT reset cc__result_specs.count here - cc__rewrite_inferred_result_ctors
   may have already registered types from function signatures, and we must keep those. */
static char* cc__rewrite_result_types(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    size_t i = 0;
    size_t last_emit = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);
    
    while (i < n) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        /* Detect `T!>(E)` pattern: type followed by '!>' followed by '(' error type ')' */
        if (c == '!' && c2 == '>') {
            /* Found '!>' sigil - now find the error type in parentheses */
            size_t sigil_pos = i;
            size_t j = i + 2;  /* skip '!>' */
            
            /* Skip whitespace */
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) j++;
            
            /* If '!>' is not followed by '(' it is not a result-type
             * annotation; it is the statement operator `func() !>;` /
             * `func() !> STMT;` / `func() !> { ... }` handled by
             * pass_result_unwrap in phase 3.  Skip past the sigil and
             * keep scanning. */
            if (j >= n || src[j] != '(') {
                i = sigil_pos + 2;
                scan.col += 2;
                continue;
            }
            /* If the non-ws char immediately before `!>` is `)` (a closing
             * paren of a call or expression), this is the binder form
             * `CALL !> (e) BODY` of the `!>` statement operator — not a
             * type annotation.  Let pass_result_unwrap handle it later. */
            {
                size_t bk = sigil_pos;
                while (bk > 0 && (src[bk - 1] == ' ' || src[bk - 1] == '\t' ||
                                  src[bk - 1] == '\r' || src[bk - 1] == '\n')) bk--;
                if (bk > 0 && src[bk - 1] == ')') {
                    i = sigil_pos + 2;
                    scan.col += 2;
                    continue;
                }
            }
            if (j < n && src[j] == '(') {
                j++;  /* skip '(' */
                
                /* Skip whitespace inside parens */
                while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) j++;
                
                /* Find matching ')' - track nesting for complex types like Error<A, B> */
                size_t err_start = j;
                int paren_depth = 1;
                int in_s = 0, in_c = 0;
                while (j < n && paren_depth > 0) {
                    char ch = src[j];
                    if (in_s) { if (ch == '\\' && j + 1 < n) j++; else if (ch == '"') in_s = 0; j++; continue; }
                    if (in_c) { if (ch == '\\' && j + 1 < n) j++; else if (ch == '\'') in_c = 0; j++; continue; }
                    if (ch == '"') { in_s = 1; j++; continue; }
                    if (ch == '\'') { in_c = 1; j++; continue; }
                    if (ch == '(') paren_depth++;
                    else if (ch == ')') paren_depth--;
                    if (paren_depth > 0) j++;
                }
                
                if (paren_depth != 0) {
                    /* ERROR: unclosed paren in error type */
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                            scan.line, scan.col, "type", "unclosed '(' in result error type");
                    fprintf(stderr, "  hint: result type syntax is 'T !> (ErrorType)'\n");
                    free(out);
                    return NULL;
                }
                
                if (paren_depth == 0) {
                    /* Found matching ')' at position j */
                    size_t err_end = j;
                    
                    /* Trim trailing whitespace from error type */
                    while (err_end > err_start && (src[err_end - 1] == ' ' || src[err_end - 1] == '\t' ||
                                                    src[err_end - 1] == '\n' || src[err_end - 1] == '\r')) {
                        err_end--;
                    }
                    
                    /* VALIDATE: error type cannot be empty */
                    if (err_end <= err_start) {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scan.line, scan.col, "type", "empty error type in '!> ()'");
                        fprintf(stderr, "  hint: specify an error type, e.g., 'int !> (Error)' or 'int !> (int)'\n");
                        free(out);
                        return NULL;
                    }
                    
                    size_t paren_close = j;
                    (void)paren_close;  /* unused but kept for clarity */
                    j++;  /* skip ')' */
                    
                    /* Scan back from '!>' to find the ok type start */
                    /* First skip any whitespace before '!>' */
                    size_t ty_end = sigil_pos;
                    while (ty_end > 0 && (src[ty_end - 1] == ' ' || src[ty_end - 1] == '\t')) ty_end--;
                    
                    size_t ty_start = cc__scan_back_to_delim(src, ty_end);
                    ty_start = cc__skip_leading_decl_specs(src, ty_start);
                    
                    /* VALIDATE: ok type cannot be empty */
                    if (ty_start >= ty_end) {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scan.line, scan.col, "type", "missing ok type before '!>'");
                        fprintf(stderr, "  hint: result type syntax is 'T !> (ErrorType)', where T is the success type\n");
                        free(out);
                        return NULL;
                    }
                    
                    if (ty_start < ty_end && err_start < err_end) {
                        size_t ty_len = ty_end - ty_start;
                        size_t err_len = err_end - err_start;
                        
                        char mangled_ok[256];
                        char mangled_err[256];
                        cc__mangle_type_name(src + ty_start, ty_len, mangled_ok, sizeof(mangled_ok));
                        cc__mangle_type_name(src + err_start, err_len, mangled_err, sizeof(mangled_err));
                        
                        if (mangled_ok[0] && mangled_err[0]) {
                            /* Collect this type pair for later declaration emission */
                            cc__add_result_type(src + ty_start, ty_len, src + err_start, err_len,
                                                mangled_ok, mangled_err);
                            
                            /* Emit everything up to ty_start */
                            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                            /* Emit CCResult_T_E - real type name */
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "CCResult_");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, mangled_ok);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "_");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, mangled_err);
                            last_emit = j;  /* skip past ')' */
                            
                            /* Register result-typed variable for UFCS.
                               Look for variable name after the type (skip ws and *). */
                            CCTypeRegistry* reg = cc_type_registry_get_global();
                            {
                                size_t v = cc_skip_ws_and_comments(src, n, j);
                                /* Skip pointer modifiers */
                                while (v < n && src[v] == '*') v++;
                                v = cc_skip_ws_and_comments(src, n, v);
                                if (v < n && cc_is_ident_start(src[v])) {
                                    size_t var_start = v;
                                    while (v < n && cc_is_ident_char(src[v])) v++;
                                    char var_name[128];
                                    char type_name[256];
                                    size_t vn_len = v - var_start;
                                    if (vn_len < sizeof(var_name)) {
                                        memcpy(var_name, src + var_start, vn_len);
                                        var_name[vn_len] = 0;
                                        /* Type name is CCResult_T_E */
                                        snprintf(type_name, sizeof(type_name), "CCResult_%s_%s",
                                                 mangled_ok, mangled_err);
                                        if (reg) cc_type_registry_add_var(reg, var_name, type_name);
                                        /* If the name is immediately followed by '(' this
                                         * is a function declaration/definition returning a
                                         * result type.  Register the name for the slice-7
                                         * unhandled-result diagnostic.  We gate on a real
                                         * identifier start so array/pointer-to-function
                                         * declarators like `int !> (E) (*fp)(void)` are
                                         * not treated as functions. */
                                        size_t q = cc_skip_ws_and_comments(src, n, v);
                                        if (q < n && src[q] == '(') {
                                            /* Record the fn name *and* the declared textual
                                             * error type so `pass_result_unwrap.c` can emit a
                                             * typed binder for `!>(e)` / `?>(e)` forms.  The
                                             * err_type slice here is the unmangled source
                                             * text (e.g. "CCIoError"). */
                                            cc_result_fn_registry_add_typed(var_name, vn_len,
                                                                             src + err_start,
                                                                             err_end - err_start);
                                        }
                                    }
                                }
                            }
                            
                            i = j;
                            continue;
                        }
                    }
                }
            }
        }
        
        i++;
    }
    
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    
    /* Result types are collected for codegen to emit CC_DECL_RESULT_SPEC declarations.
       Parser mode uses the __CC_RESULT(T, E) macro which expands to __CCResultGeneric,
       so no explicit typedefs needed here - the macro in cc_result.cch handles it. */
    (void)cc__result_specs.count;
    (void)input_path;
    return out;
}

/* Rewrite slice types:
   - `T[:]`  -> `CCSlice`
   - `T[:!]` -> `CCSliceUnique`
   Requires a closing ']' and a ':' after '['. */
static char* cc__rewrite_slice_types(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;

    size_t i = 0;
    size_t last_emit = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);

    while (i < n) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        
        char c = src[i];

        if (c == '[') {
            size_t j = i + 1;
            size_t colon = (size_t)-1;
            size_t close = (size_t)-1;
            while (j < n) {
                if (src[j] == ']') { close = j; break; }
                if (src[j] == ':' && colon == (size_t)-1) colon = j;
                if (src[j] == '\n') break;
                j++;
            }
            if (colon != (size_t)-1 && close != (size_t)-1) {
                int valid_slice = 1;
                int bang_count = 0;
                for (size_t t = i + 1; t < close; t++) {
                    unsigned char ch = (unsigned char)src[t];
                    if (ch == ':') continue;
                    if (ch == '!') { bang_count++; continue; }
                    if (ch == ' ' || ch == '\t') continue;
                    if (isalnum(ch) || ch == '_') continue;
                    valid_slice = 0;
                    break;
                }
                if (bang_count > 1) valid_slice = 0;
                if (!valid_slice) { i++; continue; }
                size_t k = close;
                size_t unique_pos = close;
                int is_unique = 0;
                while (unique_pos > colon && (src[unique_pos - 1] == ' ' || src[unique_pos - 1] == '\t')) unique_pos--;
                if (unique_pos > colon && src[unique_pos - 1] == '!') is_unique = 1;
                if (k >= n || src[k] != ']') {
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                            scan.line, scan.col, "type", "unterminated slice type (missing ']')");
                    free(out);
                    return NULL;
                }

                size_t ty_start = cc__scan_back_to_delim(src, i);
                if (ty_start < last_emit) { /* odd overlap */ }
                else {
                    size_t type_start = cc__skip_leading_decl_specs(src, ty_start);
                    /* Emit everything up to the slice element type, preserving
                       leading decl/function specifiers like `static inline`. */
                    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                    cc_sb_append(&out, &out_len, &out_cap, src + ty_start, type_start - ty_start);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, is_unique ? "CCSliceUnique" : "CCSlice");
                    last_emit = k + 1; /* skip past ']' */
                }

                /* advance scan to after ']' */
                i = k + 1;
                continue;
            }
        }

        i++;
    }

    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Simple pass: convert `if @try (` to `if (try ` so the main rewriter handles both syntaxes */
static char* cc__normalize_if_try_syntax(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0, last_emit = 0;
    
    while (i + 9 < n) {  /* "if @try (" is 9 chars minimum */
        /* Look for "if" followed by whitespace, "@try", whitespace, "(" */
        if (src[i] == 'i' && src[i+1] == 'f' && 
            (i == 0 || !cc_is_ident_char(src[i-1])) &&
            (src[i+2] == ' ' || src[i+2] == '\t' || src[i+2] == '\n')) {
            size_t j = i + 2;
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
            if (j + 4 < n && src[j] == '@' && src[j+1] == 't' && src[j+2] == 'r' && src[j+3] == 'y') {
                size_t k = j + 4;
                while (k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\n')) k++;
                if (k < n && src[k] == '(') {
                    /* Found "if @try (" - convert to "if (try " */
                    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "if (try ");
                    last_emit = k + 1;  /* Skip past the '(' */
                    i = k + 1;
                    continue;
                }
            }
        }
        i++;
    }
    if (last_emit == 0) return NULL;
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Rewrite if (try T x = expr) { ... } else { ... } to expanded form */
static char* cc__rewrite_try_binding(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0, last_emit = 0;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    
    while (i < n) {
        char c = src[i], c2 = (i+1 < n) ? src[i+1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i+1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i+1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Detect if (try ...) */
        if (c == 'i' && c2 == 'f') {
            int ws = (i == 0) || !cc_is_ident_char(src[i-1]);
            int we = (i+2 >= n) || !cc_is_ident_char(src[i+2]);
            if (ws && we) {
                size_t if_start = i, j = i + 2;
                while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
                if (j < n && src[j] == '(') {
                    j++;
                    while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
                    /* Check for 'try' */
                    if (j+3 <= n && src[j] == 't' && src[j+1] == 'r' && src[j+2] == 'y' &&
                        (j+3 >= n || !cc_is_ident_char(src[j+3]))) {
                        size_t after_try = j + 3;
                        while (after_try < n && (src[after_try] == ' ' || src[after_try] == '\t' || src[after_try] == '\n')) after_try++;
                        
                        /* Find closing ')' */
                        size_t cond_end = after_try;
                        int paren = 1, in_s = 0, in_c = 0;
                        while (cond_end < n && paren > 0) {
                            char ec = src[cond_end];
                            if (in_s) { if (ec == '\\' && cond_end+1 < n) cond_end++; else if (ec == '"') in_s = 0; cond_end++; continue; }
                            if (in_c) { if (ec == '\\' && cond_end+1 < n) cond_end++; else if (ec == '\'') in_c = 0; cond_end++; continue; }
                            if (ec == '"') { in_s = 1; cond_end++; continue; }
                            if (ec == '\'') { in_c = 1; cond_end++; continue; }
                            if (ec == '(') paren++;
                            else if (ec == ')') { paren--; if (paren == 0) break; }
                            cond_end++;
                        }
                        if (paren != 0) { i++; continue; }
                        
                        /* Find '=' */
                        size_t eq = after_try;
                        while (eq < cond_end && src[eq] != '=') eq++;
                        if (eq >= cond_end) { i++; continue; }
                        
                        /* Type and var: after_try to eq, var is last ident */
                        size_t tv_end = eq;
                        while (tv_end > after_try && (src[tv_end-1] == ' ' || src[tv_end-1] == '\t' || src[tv_end-1] == '\n')) tv_end--;
                        size_t var_end = tv_end, var_start = var_end;
                        while (var_start > after_try && cc_is_ident_char(src[var_start-1])) var_start--;
                        if (var_start >= var_end) { i++; continue; }
                        
                        size_t type_end = var_start;
                        while (type_end > after_try && (src[type_end-1] == ' ' || src[type_end-1] == '\t' || src[type_end-1] == '\n')) type_end--;
                        size_t type_start = after_try;
                        while (type_start < type_end && (src[type_start] == ' ' || src[type_start] == '\t' || src[type_start] == '\n')) type_start++;
                        if (type_start >= type_end) { i++; continue; }
                        
                        /* Expr: eq+1 to cond_end */
                        size_t expr_start = eq + 1;
                        while (expr_start < cond_end && (src[expr_start] == ' ' || src[expr_start] == '\t' || src[expr_start] == '\n')) expr_start++;
                        size_t expr_end = cond_end;
                        while (expr_end > expr_start && (src[expr_end-1] == ' ' || src[expr_end-1] == '\t' || src[expr_end-1] == '\n')) expr_end--;
                        if (expr_start >= expr_end) { i++; continue; }
                        
                        /* Find then-block */
                        size_t k = cond_end + 1;
                        while (k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\n')) k++;
                        if (k >= n || src[k] != '{') { i++; continue; }
                        
                        size_t then_start = k;
                        int brace = 1; k++; in_s = 0; in_c = 0;
                        while (k < n && brace > 0) {
                            char ec = src[k];
                            if (in_s) { if (ec == '\\' && k+1 < n) k++; else if (ec == '"') in_s = 0; k++; continue; }
                            if (in_c) { if (ec == '\\' && k+1 < n) k++; else if (ec == '\'') in_c = 0; k++; continue; }
                            if (ec == '"') { in_s = 1; k++; continue; }
                            if (ec == '\'') { in_c = 1; k++; continue; }
                            if (ec == '{') brace++; else if (ec == '}') brace--;
                            k++;
                        }
                        size_t then_end = k;
                        
                        /* Check for else */
                        size_t else_start = 0, else_end = 0, m = k;
                        while (m < n && (src[m] == ' ' || src[m] == '\t' || src[m] == '\n')) m++;
                        if (m+4 <= n && src[m] == 'e' && src[m+1] == 'l' && src[m+2] == 's' && src[m+3] == 'e' &&
                            (m+4 >= n || !cc_is_ident_char(src[m+4]))) {
                            m += 4;
                            while (m < n && (src[m] == ' ' || src[m] == '\t' || src[m] == '\n')) m++;
                            if (m < n && src[m] == '{') {
                                else_start = m; brace = 1; m++; in_s = 0; in_c = 0;
                                while (m < n && brace > 0) {
                                    char ec = src[m];
                                    if (in_s) { if (ec == '\\' && m+1 < n) m++; else if (ec == '"') in_s = 0; m++; continue; }
                                    if (in_c) { if (ec == '\\' && m+1 < n) m++; else if (ec == '\'') in_c = 0; m++; continue; }
                                    if (ec == '"') { in_s = 1; m++; continue; }
                                    if (ec == '\'') { in_c = 1; m++; continue; }
                                    if (ec == '{') brace++; else if (ec == '}') brace--;
                                    m++;
                                }
                                else_end = m;
                            }
                        }
                        
                        /* Emit expansion */
                        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, if_start - last_emit);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "{ __typeof__(");
                        cc_sb_append(&out, &out_len, &out_cap, src + expr_start, expr_end - expr_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, ") __cc_try_bind = (");
                        cc_sb_append(&out, &out_len, &out_cap, src + expr_start, expr_end - expr_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "); if (__cc_try_bind.ok) { ");
                        cc_sb_append(&out, &out_len, &out_cap, src + type_start, type_end - type_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, " ");
                        cc_sb_append(&out, &out_len, &out_cap, src + var_start, var_end - var_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, " = __cc_try_bind.u.value; ");
                        cc_sb_append(&out, &out_len, &out_cap, src + then_start + 1, then_end - then_start - 2);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, " }");
                        if (else_end > else_start) {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, " else ");
                            cc_sb_append(&out, &out_len, &out_cap, src + else_start, else_end - else_start);
                        }
                        cc_sb_append_cstr(&out, &out_len, &out_cap, " }");
                        
                        last_emit = (else_end > 0) ? else_end : then_end;
                        i = last_emit;
                        continue;
                    }
                }
            }
        }
        i++;
    }
    if (last_emit == 0) return NULL;
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Simple pass: rewrite @defer(err) -> @defer */
static char* cc__rewrite_defer_syntax(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0, last_emit = 0;
    
    while (i + 6 < n) {
        if (src[i] == '@' && strncmp(src + i + 1, "defer", 5) == 0) {
            size_t j = i + 6;
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
            if (j < n && src[j] == '(') {
                /* Found @defer( - skip parenthesized part */
                size_t k = j + 1;
                int paren = 1;
                while (k < n && paren > 0) {
                    if (src[k] == '(') paren++;
                    else if (src[k] == ')') paren--;
                    k++;
                }
                if (paren == 0) {
                    /* Rewrite @defer(...) -> @defer */
                    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "@defer ");
                    last_emit = k;
                    i = k;
                    continue;
                }
            }
        }
        i++;
    }
    
    if (last_emit == 0) return NULL;
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* ---- @async void return-type rewrite ----------------------------------- */
/* `@async void fn(...)` is functionally the same as any other `@async` fn —
 * the body is lowered into a state-machine that always returns
 * `CCTaskIntptr`.  But phase-3 reparse happens BEFORE the async lowering,
 * and call-sites such as `nursery->spawn_async(fn(args))` lower (via UFCS)
 * to `cc_nursery_spawn_async(n, fn(args))`, whose second parameter is
 * `CCTask` (typedef'd to `int` in parser mode).  With `fn` still declared
 * `void`, reparse fails with "cannot convert 'void' to 'int'".
 *
 * The fix is a narrow text-level rewrite: replace the explicit `void`
 * return type of `@async` functions with the marker typedef
 * `CCAsyncVoidRet` (also `int` in parser mode, `CCTaskIntptr` at runtime).
 * The async lowering (`async_ast.c`) treats `CCAsyncVoidRet` as
 * equivalent to the original `void`, so bare `return;` inside the body
 * remains legal.
 *
 * Only `@async` declarations are rewritten; plain `void fn(...)` is
 * left alone.  Function-body text (where `void` might appear as a
 * local cast or pointer type) is untouched because the rewrite is
 * anchored on `@async` + an identifier followed by `(`. */
char* cc__rewrite_async_void_ret(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    /* Early exit if no real `@async` appears outside comments/strings.
     * Pre-metaclass this used raw `strstr` which returned a hit on the
     * sigil inside a block comment and forced the full scan every time. */
    if (!cc_contains_token_top_level(src, n, "@async")) return NULL;

    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t last_emit = 0;
    int changed = 0;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;

    for (size_t i = 0; i < n; ) {
        char c = src[i], c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }

        /* Match '@async' at a word boundary. */
        if (c == '@' && i + 6 <= n && memcmp(src + i + 1, "async", 5) == 0
                && (i + 6 >= n || !cc_is_ident_char(src[i + 6]))) {
            /* Skip '@async' plus trailing @-attributes (e.g. @nonblocking,
             * @latency_sensitive) and whitespace to land on the return-type
             * token. */
            size_t j = i + 6;
            for (;;) {
                while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r'))
                    j++;
                if (j < n && src[j] == '@') {
                    size_t k = j + 1;
                    while (k < n && cc_is_ident_char(src[k])) k++;
                    if (k == j + 1) break;
                    j = k;
                    continue;
                }
                break;
            }
            size_t rt_start = j;
            /* Return-type must be the literal 'void' identifier (no pointer
             * or qualifier), followed by whitespace, an identifier, and an
             * opening '('. */
            if (rt_start + 4 <= n && memcmp(src + rt_start, "void", 4) == 0
                    && (rt_start + 4 >= n || !cc_is_ident_char(src[rt_start + 4]))) {
                size_t after_void = rt_start + 4;
                size_t p = after_void;
                while (p < n && (src[p] == ' ' || src[p] == '\t' || src[p] == '\n' || src[p] == '\r'))
                    p++;
                if (p < n && cc_is_ident_start(src[p])) {
                    size_t ne = p;
                    while (ne < n && cc_is_ident_char(src[ne])) ne++;
                    size_t q = ne;
                    while (q < n && (src[q] == ' ' || src[q] == '\t' || src[q] == '\n' || src[q] == '\r'))
                        q++;
                    if (q < n && src[q] == '(') {
                        /* Commit rewrite: emit source up through rt_start,
                         * then the marker typedef, then resume after the
                         * 'void' token. */
                        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, rt_start - last_emit);
                        cc_sb_append(&out, &out_len, &out_cap, "CCAsyncVoidRet", 14);
                        last_emit = after_void;
                        changed = 1;
                        i = after_void;
                        continue;
                    }
                }
            }
            /* Not a match; advance past '@async' so we don't scan it again. */
            i = rt_start > i ? rt_start : i + 1;
            continue;
        }
        i++;
    }

    if (!changed) {
        free(out);
        return NULL;
    }
    if (last_emit < n) {
        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    }
    cc_sb_append(&out, &out_len, &out_cap, "", 1);  /* NUL */
    out_len -= 1;
    (void)out_len;
    return out;
}

/* ---- @await lowering ---------------------------------------------------- */
/* Rewrites  @await fname(args...)  ->  cc_block_on(ReturnType, fname(args...))
 * where ReturnType is taken from the @async declaration of fname.
 * For unknown callees the @await prefix is stripped and the expression is kept
 * as-is (channel ops are already blocking in synchronous context).
 * This pass runs in phase-1 so @async return types have already been through
 * cc__rewrite_result_types (e.g. void !>(CCError) -> CCResult_void_CCError). */

#define CC__AT_AWAIT_MAX_FNS 256
typedef struct { char name[128]; char ret[128]; } CCAtAwaitFn;
static CCAtAwaitFn  cc__at_await_fns[CC__AT_AWAIT_MAX_FNS];
static int          cc__at_await_fn_count = 0;

static void cc__collect_async_ret_types(const char* src, size_t n) {
    cc__at_await_fn_count = 0;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    for (size_t i = 0; i < n; ) {
        char c = src[i], c2 = (i+1 < n) ? src[i+1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c=='*' && c2=='/') { in_bc = 0; i++; } i++; continue; }
        if (in_str) { if (c=='\\' && i+1<n) { i+=2; continue; } if (c=='"') in_str=0; i++; continue; }
        if (in_chr) { if (c=='\\' && i+1<n) { i+=2; continue; } if (c=='\'') in_chr=0; i++; continue; }
        if (c=='/' && c2=='/') { in_lc=1; i+=2; continue; }
        if (c=='/' && c2=='*') { in_bc=1; i+=2; continue; }
        if (c=='"') { in_str=1; i++; continue; }
        if (c=='\'') { in_chr=1; i++; continue; }

        /* Detect '@async' keyword */
        if (c == '@' && i+6 <= n && memcmp(src+i+1,"async",5) == 0
                     && (i+6 >= n || (!cc_is_ident_char(src[i+6])))) {
            size_t j = i + 6;
            while (j < n && (src[j]==' '||src[j]=='\t'||src[j]=='\n'||src[j]=='\r')) j++;
            size_t rt_start = j;

            /* Walk forward to find  <fname> (  – last ident before '(' */
            size_t fname_s = (size_t)-1, fname_e = (size_t)-1;
            size_t k = j;
            while (k < n && src[k] != '{' && src[k] != ';') {
                if (src[k] == '(' && fname_e != (size_t)-1) break;
                if (cc_is_ident_start(src[k]) || (k > 0 && cc_is_ident_char(src[k]))) {
                    fname_s = k;
                    while (k < n && cc_is_ident_char(src[k])) k++;
                    fname_e = k;
                } else { k++; }
            }
            if (fname_s != (size_t)-1 && fname_e > fname_s
                    && k < n && src[k] == '('
                    && cc__at_await_fn_count < CC__AT_AWAIT_MAX_FNS) {
                CCAtAwaitFn* f = &cc__at_await_fns[cc__at_await_fn_count];
                /* function name */
                size_t nl = fname_e - fname_s;
                if (nl >= sizeof(f->name)) nl = sizeof(f->name)-1;
                memcpy(f->name, src+fname_s, nl); f->name[nl] = '\0';
                /* return type: from rt_start to fname_s, trim trailing whitespace */
                size_t re = fname_s;
                while (re > rt_start && (src[re-1]==' '||src[re-1]=='\t'||src[re-1]=='\n')) re--;
                size_t rl = re - rt_start;
                if (rl >= sizeof(f->ret)) rl = sizeof(f->ret)-1;
                if (rl > 0) { memcpy(f->ret, src+rt_start, rl); f->ret[rl] = '\0'; }
                else         { strcpy(f->ret, "int"); }
                cc__at_await_fn_count++;
            }
            i = (j > i+6) ? j : i+1;
            continue;
        }
        i++;
    }
}

/* Rewrite call-site `@blocking callee(args)` / `@noblock callee(args)`.
 *
 * Leaves a comment-marker form that survives TCC parsing as whitespace
 * but can be recovered by pass_autoblock by scanning source text
 * immediately before each CALL node:
 *
 *   @blocking foo(x)  ->  /*@CC_SITE=blocking*\/ foo(x)
 *   @noblock  foo(x)  ->  /*@CC_SITE=noblock*\/ foo(x)
 *
 * Decl-level attrs (`@blocking int foo(...)`) are *not* rewritten: the
 * decl-form has a type token between `@blocking` and the name, so the
 * pattern `@blocking IDENT (` does not match.  Function definitions
 * (where the closing `)` is followed by `{`) are also skipped — the
 * TCC cc-ext hook handles those.  Spec §8.2.2 precedence rule 1. */
char* cc__rewrite_at_call_site_mode(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    /* Early exit if neither `@blocking` nor `@noblock` appears outside
     * comments/strings.  The cheap pre-check avoids building the output
     * buffer in the common case. */
    if (!cc_contains_token_top_level(src, n, "@blocking") &&
        !cc_contains_token_top_level(src, n, "@noblock")) {
        return NULL;
    }

    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t last_emit = 0;
    int changed = 0;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;

    for (size_t i = 0; i < n; ) {
        char c = src[i], c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }

        if (c != '@') { i++; continue; }

        const char* mode = NULL;
        size_t kw_len = 0;
        if (i + 9 <= n && memcmp(src + i + 1, "blocking", 8) == 0 &&
            (i + 9 == n || !cc_is_ident_char(src[i + 9]))) {
            mode = "blocking"; kw_len = 8;
        } else if (i + 8 <= n && memcmp(src + i + 1, "noblock", 7) == 0 &&
                   (i + 8 == n || !cc_is_ident_char(src[i + 8]))) {
            mode = "noblock"; kw_len = 7;
        }
        if (!mode) { i++; continue; }

        /* Need: `@<mode> IDENT (<balanced>) <not-'{'>` to call this a call-site.
         * Probe ahead without committing. */
        size_t p = i + 1 + kw_len;
        while (p < n && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) p++;
        if (p >= n || !cc_is_ident_start(src[p])) { i++; continue; }
        size_t id_s = p;
        while (p < n && cc_is_ident_char(src[p])) p++;
        size_t id_e = p;
        size_t after_id = p;
        while (p < n && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) p++;
        if (p >= n || src[p] != '(') { i++; continue; }

        /* Find matching ')' respecting strings/chars. */
        size_t m = p;
        int depth = 0;
        int in_s = 0, in_q = 0;
        while (m < n) {
            char mc = src[m];
            if (in_s) { if (mc == '\\' && m + 1 < n) { m += 2; continue; } if (mc == '"') in_s = 0; m++; continue; }
            if (in_q) { if (mc == '\\' && m + 1 < n) { m += 2; continue; } if (mc == '\'') in_q = 0; m++; continue; }
            if (mc == '"') { in_s = 1; m++; continue; }
            if (mc == '\'') { in_q = 1; m++; continue; }
            if (mc == '(') depth++;
            else if (mc == ')') { depth--; if (depth == 0) { m++; break; } }
            m++;
        }
        if (depth != 0) { i++; continue; }
        size_t rparen_end = m;

        /* Lookahead: if next non-ws char is `{`, it's a function
         * definition — leave the marker intact for TCC cc-ext. */
        size_t q = rparen_end;
        while (q < n && (src[q] == ' ' || src[q] == '\t' || src[q] == '\r' || src[q] == '\n')) q++;
        if (q < n && src[q] == '{') { i++; continue; }

        /* Emit everything before `@<mode>`, then our marker + original
         * call expression text verbatim.  Using a block comment keeps
         * TCC happy (treated as whitespace) and preserves text layout
         * reasonably well for source-position-based passes. */
        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
        if (strcmp(mode, "blocking") == 0) {
            cc_sb_append_cstr(&out, &out_len, &out_cap, "/*@CC_SITE=blocking*/ ");
        } else {
            cc_sb_append_cstr(&out, &out_len, &out_cap, "/*@CC_SITE=noblock*/ ");
        }
        cc_sb_append(&out, &out_len, &out_cap, src + id_s, rparen_end - id_s);
        last_emit = rparen_end;
        i = rparen_end;
        changed = 1;
        (void)id_e; (void)after_id;
    }

    if (!changed) { free(out); return NULL; }
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Rewrite @await <expr> in any context.
 * If <expr> is a call to a known @async function, emit cc_block_on(T, expr).
 * Otherwise strip @await and keep the expression (channel ops etc. are
 * already blocking in synchronous context). */
char* cc__rewrite_at_await(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    /* Early exit only if `@await` is absent outside comments/strings. */
    if (!cc_contains_token_top_level(src, n, "@await")) return NULL;

    cc__collect_async_ret_types(src, n);

    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t last_emit = 0;
    int changed = 0;
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;

    for (size_t i = 0; i < n; ) {
        char c = src[i], c2 = (i+1 < n) ? src[i+1] : 0;
        if (in_lc) { if (c=='\n') in_lc=0; i++; continue; }
        if (in_bc) { if (c=='*'&&c2=='/') { in_bc=0; i++; } i++; continue; }
        if (in_str) { if (c=='\\'&&i+1<n){i+=2;continue;} if(c=='"') in_str=0; i++;continue; }
        if (in_chr) { if (c=='\\'&&i+1<n){i+=2;continue;} if(c=='\'') in_chr=0; i++;continue; }
        if (c=='/'&&c2=='/') { in_lc=1; i+=2; continue; }
        if (c=='/'&&c2=='*') { in_bc=1; i+=2; continue; }
        if (c=='"') { in_str=1; i++; continue; }
        if (c=='\'') { in_chr=1; i++; continue; }

        /* Detect '@await' token */
        if (c == '@' && i+6 <= n && memcmp(src+i+1,"await",5) == 0
                     && (i+6 >= n || !cc_is_ident_char(src[i+6]))) {
            size_t j = i + 6;
            while (j < n && (src[j]==' '||src[j]=='\t')) j++;

            /* Flush source up to (not including) '@await' */
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);

            /* Check if next token is an identifier (potential function name) */
            const char* ret_type = NULL;
            size_t call_start = j, call_end = j;
            if (j < n && cc_is_ident_start(src[j])) {
                size_t id_s = j;
                while (j < n && cc_is_ident_char(src[j])) j++;
                size_t id_e = j;
                while (j < n && (src[j]==' '||src[j]=='\t')) j++;

                if (j < n && src[j] == '(') {
                    /* It's a function call – look up the return type */
                    char fname[128];
                    size_t fl = id_e - id_s;
                    if (fl >= sizeof(fname)) fl = sizeof(fname)-1;
                    memcpy(fname, src+id_s, fl); fname[fl] = '\0';
                    for (int fi = 0; fi < cc__at_await_fn_count; fi++) {
                        if (strcmp(cc__at_await_fns[fi].name, fname) == 0) {
                            ret_type = cc__at_await_fns[fi].ret; break;
                        }
                    }
                    /* Find matching ')' of the call */
                    size_t m = j; int depth = 0;
                    while (m < n) {
                        char mc = src[m];
                        if (mc=='"') { m++; while(m<n&&src[m]!='"'){if(src[m]=='\\')m++;m++;} }
                        else if (mc=='(') depth++;
                        else if (mc==')') { depth--; if(depth==0){m++;break;} }
                        m++;
                    }
                    call_start = id_s; call_end = m;
                } else {
                    /* Identifier but not a call – keep from id_s onward */
                    call_start = id_s; call_end = id_e;
                }
            } else {
                /* Not an identifier – keep everything from j onward as-is;
                 * we'll just strip the '@await' prefix. */
                call_start = j; call_end = j;
            }

            if (ret_type && call_end > call_start) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_block_on(");
                cc_sb_append_cstr(&out, &out_len, &out_cap, ret_type);
                cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                cc_sb_append(&out, &out_len, &out_cap, src+call_start, call_end-call_start);
                cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
            } else if (call_end > call_start) {
                /* Unknown async fn or non-call – keep expr, strip @await */
                cc_sb_append(&out, &out_len, &out_cap, src+call_start, call_end-call_start);
            }
            last_emit = call_end;
            changed = 1;
            i = call_end;
            continue;
        }
        i++;
    }

    if (!changed) { free(out); return NULL; }
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src+last_emit, n-last_emit);
    return out;
}

/* Rewrite try expressions: try expr -> cc_try(expr) */
static char* cc__rewrite_try_exprs(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    size_t i = 0;
    size_t last_emit = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);
    
    while (i < n) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        
        char c = src[i];
        
        /* Detect 'try' keyword followed by expression (not try { block } form) */
        if (c == 't' && i + 2 < n && src[i+1] == 'r' && src[i+2] == 'y') {
            /* Check word boundary before */
            int word_start = (i == 0) || !cc_is_ident_char(src[i-1]);
            /* Check word boundary after */
            int word_end = (i + 3 >= n) || !cc_is_ident_char(src[i+3]);
            
            if (word_start && word_end) {
                size_t after_try = i + 3;
                /* Skip whitespace */
                while (after_try < n && (src[after_try] == ' ' || src[after_try] == '\t' || src[after_try] == '\n')) after_try++;
                
                /* Check it's not try { block } form */
                if (after_try < n && src[after_try] == '{') {
                    /* try { ... } block form - skip, not handled here */
                } else if (after_try < n && (cc_is_ident_start(src[after_try]) || src[after_try] == '(')) {
                    /* 'try expr' form - find end of expression */
                    size_t expr_start = after_try;
                    size_t expr_end = expr_start;
                    
                    /* Scan expression with balanced parens/braces */
                    int paren = 0, brace = 0, bracket = 0;
                    int in_s = 0, in_c = 0;
                    while (expr_end < n) {
                        char ec = src[expr_end];
                        
                        if (in_s) { if (ec == '\\' && expr_end + 1 < n) { expr_end += 2; continue; } if (ec == '"') in_s = 0; expr_end++; continue; }
                        if (in_c) { if (ec == '\\' && expr_end + 1 < n) { expr_end += 2; continue; } if (ec == '\'') in_c = 0; expr_end++; continue; }
                        if (ec == '"') { in_s = 1; expr_end++; continue; }
                        if (ec == '\'') { in_c = 1; expr_end++; continue; }
                        
                        if (ec == '(' ) { paren++; expr_end++; continue; }
                        if (ec == ')' ) { if (paren > 0) { paren--; expr_end++; continue; } else break; }
                        if (ec == '{' ) { brace++; expr_end++; continue; }
                        if (ec == '}' ) { if (brace > 0) { brace--; expr_end++; continue; } else break; }
                        if (ec == '[' ) { bracket++; expr_end++; continue; }
                        if (ec == ']' ) { if (bracket > 0) { bracket--; expr_end++; continue; } else break; }
                        
                        /* End expression at ';', ',', or unbalanced ')' */
                        if (paren == 0 && brace == 0 && bracket == 0) {
                            if (ec == ';' || ec == ',') break;
                        }
                        
                        expr_end++;
                    }
                    
                    /* Only rewrite if we found a valid expression */
                    if (expr_end > expr_start) {
                        /* Emit: everything up to 'try', then 'cc_try(', then expr, then ')' */
                        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_try(");
                        cc_sb_append(&out, &out_len, &out_cap, src + expr_start, expr_end - expr_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                        last_emit = expr_end;
                        i = expr_end;
                        continue;
                    }
                }
            }
        }
        
        i++;
    }
    
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Rewrite `*res` -> `cc_unwrap(res)` for variables declared with CCResult_*
 * type. Two-pass approach:
 *   1. Scan for CCResult_<T>_<E> or `__CC_RESULT(T, E)` variable declarations.
 *   2. Rewrite `*varname` to `cc_unwrap(varname)`.
 *
 * (retired) The optional arm of this pass, which recognized CCOptional_<T>
 * declarations and rewrote `*opt` -> `cc_unwrap_opt(opt)`, has been removed
 * along with the optional surface. */
static char* cc__rewrite_optional_unwrap(const char* src, size_t n) {
    if (!src || n == 0) return NULL;

    /* Pass 1: Collect Result variable names. */
    #define MAX_UNWRAP_VARS 256
    char* vars[MAX_UNWRAP_VARS];
    int var_count = 0;

    size_t i = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);

    while (i < n && var_count < MAX_UNWRAP_VARS) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;

        char c = src[i];
        int is_cc_result = (c == 'C' && i + 8 < n && strncmp(src + i, "CCResult_", 9) == 0);
        int is_macro_result = (c == '_' && i + 13 < n && strncmp(src + i, "__CC_RESULT(", 12) == 0);

        if (is_cc_result || is_macro_result) {
            size_t type_end = i;
            if (is_cc_result) {
                type_end += 9;
                while (type_end < n && (cc_is_ident_char(src[type_end]) || src[type_end] == '_')) type_end++;
            } else {
                type_end += 12;
                int paren_depth = 1;
                while (type_end < n && paren_depth > 0) {
                    if (src[type_end] == '(') paren_depth++;
                    else if (src[type_end] == ')') paren_depth--;
                    type_end++;
                }
            }
            size_t ws_end = type_end;
            while (ws_end < n && (src[ws_end] == ' ' || src[ws_end] == '\t' || src[ws_end] == '\n')) ws_end++;
            if (ws_end < n && cc_is_ident_start(src[ws_end])) {
                size_t var_start = ws_end;
                while (ws_end < n && cc_is_ident_char(src[ws_end])) ws_end++;
                size_t var_len = ws_end - var_start;
                size_t after_ws = ws_end;
                while (after_ws < n && (src[after_ws] == ' ' || src[after_ws] == '\t')) after_ws++;
                if (after_ws < n && (src[after_ws] == '=' || src[after_ws] == ';' || src[after_ws] == ',')) {
                    char* varname = (char*)malloc(var_len + 1);
                    if (varname) {
                        memcpy(varname, src + var_start, var_len);
                        varname[var_len] = 0;
                        vars[var_count++] = varname;
                    }
                }
            }
            i = type_end;
            continue;
        }

        i++;
    }

    if (var_count == 0) return NULL;

    /* Pass 2: Rewrite `*varname` to `cc_unwrap(varname)`. */
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;

    i = 0;
    size_t last_emit = 0;
    cc_scanner_init(&scan);

    while (i < n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;

        char c = src[i];

        if (c == '*') {
            size_t star_pos = i;

            int is_ptr_type_decl = 0;
            if (star_pos > 0) {
                size_t prev = star_pos - 1;
                while (prev > 0 && (src[prev] == ' ' || src[prev] == '\t')) prev--;
                if (prev < n && cc_is_ident_char(src[prev])) {
                    is_ptr_type_decl = 1;
                }
            }

            if (is_ptr_type_decl) {
                i++;
                continue;
            }

            i++;
            while (i < n && (src[i] == ' ' || src[i] == '\t')) i++;
            if (i < n && cc_is_ident_start(src[i])) {
                size_t var_start = i;
                while (i < n && cc_is_ident_char(src[i])) i++;
                size_t var_len = i - var_start;

                int found_idx = -1;
                for (int j = 0; j < var_count; j++) {
                    if (strlen(vars[j]) == var_len && strncmp(vars[j], src + var_start, var_len) == 0) {
                        found_idx = j;
                        break;
                    }
                }

                if (found_idx >= 0) {
                    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, star_pos - last_emit);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_unwrap(");
                    cc_sb_append(&out, &out_len, &out_cap, src + var_start, var_len);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                    last_emit = i;
                    continue;
                }
            }
            i = star_pos + 1;
            continue;
        }

        i++;
    }

    for (int j = 0; j < var_count; j++) {
        free(vars[j]);
    }

    if (last_emit == 0) return NULL;
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

// Rewrite @link("lib") directives into marker comments that the linker phase can extract.
// Input:  @link("curl")
// Output: a comment containing __CC_LINK__ curl
char* cc__rewrite_link_directives(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    size_t last_emit = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    int in_str = 0;
    int in_chr = 0;

    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;

        /* Track comment/string state */
        if (in_line_comment) {
            if (c == '\n') in_line_comment = 0;
            i++;
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && c2 == '/') {
                i += 2;
                in_block_comment = 0;
                continue;
            }
            i++;
            continue;
        }
        if (in_str) {
            if (c == '\\' && i + 1 < n) { i += 2; continue; }
            if (c == '"') in_str = 0;
            i++;
            continue;
        }
        if (in_chr) {
            if (c == '\\' && i + 1 < n) { i += 2; continue; }
            if (c == '\'') in_chr = 0;
            i++;
            continue;
        }

        /* Check for comment/string start */
        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }

        /* Look for @link("...") */
        if (c == '@' && i + 5 < n && strncmp(src + i, "@link(", 6) == 0) {
            size_t start = i;
            i += 6;  /* skip @link( */
            
            /* Skip whitespace */
            while (i < n && (src[i] == ' ' || src[i] == '\t')) i++;
            
            /* Expect " */
            if (i < n && src[i] == '"') {
                i++;  /* skip opening " */
                size_t lib_start = i;
                
                /* Find closing " */
                while (i < n && src[i] != '"' && src[i] != '\n') i++;
                
                if (i < n && src[i] == '"') {
                    size_t lib_end = i;
                    i++;  /* skip closing " */
                    
                    /* Skip whitespace and closing ) */
                    while (i < n && (src[i] == ' ' || src[i] == '\t')) i++;
                    if (i < n && src[i] == ')') {
                        i++;  /* skip ) */
                        
                        /* Success! Emit up to @link, then emit marker comment */
                        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, start - last_emit);
                        
                        // Emit marker: __CC_LINK__ libname 
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "/* __CC_LINK__ ");
                        cc_sb_append(&out, &out_len, &out_cap, src + lib_start, lib_end - lib_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, " */");
                        
                        last_emit = i;
                        continue;
                    }
                }
            }
            /* If parsing failed, continue from after @ */
            i = start + 1;
        }

        i++;
    }

    if (last_emit == 0) return NULL;  /* No rewrites */
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Check that channel ops inside @async functions are awaited.
   Returns 0 if OK, -1 if errors were emitted.
   This runs BEFORE rewrites so we can see the original source forms. */
static int cc__check_async_chan_await(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return 0;

    int errors = 0;
    int line = 1, col = 1;
    int in_async_fn = 0;      /* 1 if inside @async function body */
    int async_brace_depth = 0; /* Brace depth when we entered the async function */

    /* Comment/string tracking */
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    int brace_depth = 0;

    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;

        /* Track position */
        if (c == '\n') { line++; col = 1; } else { col++; }

        /* Skip comments and strings */
        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; col++; } continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i++; col++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i++; col++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; col++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; col++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }

        /* Track braces */
        if (c == '{') brace_depth++;
        else if (c == '}') {
            brace_depth--;
            if (in_async_fn && brace_depth < async_brace_depth) {
                in_async_fn = 0; /* Exited async function */
            }
        }

        /* Detect @async function start */
        if (c == '@') {
            size_t j = i + 1;
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j + 5 <= n && memcmp(src + j, "async", 5) == 0 && (j + 5 >= n || !cc_is_ident_char(src[j + 5]))) {
                /* Found @async - find the function body */
                j += 5;
                /* Skip to opening brace */
                int found_brace = 0;
                while (j < n) {
                    if (src[j] == '{') { found_brace = 1; break; }
                    if (src[j] == ';') break; /* Declaration, not definition */
                    j++;
                }
                if (found_brace) {
                    in_async_fn = 1;
                    async_brace_depth = brace_depth + 1; /* We'll hit the '{' soon */
                }
            }
            continue;
        }

        /* Inside @async function, check for channel ops.
           Note: We only check chan_send/chan_recv here, not UFCS form (.send/.recv).
           UFCS form is handled by the UFCS pass which correctly emits task variants in await context. */
        if (in_async_fn) {
            /* Check for chan_send, chan_recv (macro forms) */
            int is_chan_op = 0;
            const char* op_name = NULL;
            size_t op_len = 0;

            if (i + 9 <= n && memcmp(src + i, "chan_send", 9) == 0 && (i + 9 >= n || !cc_is_ident_char(src[i + 9]))) {
                /* Make sure it's not preceded by identifier char (to avoid matching cc_chan_send) */
                if (i > 0 && cc_is_ident_char(src[i - 1])) { /* skip */ }
                else { is_chan_op = 1; op_name = "chan_send"; op_len = 9; }
            } else if (i + 9 <= n && memcmp(src + i, "chan_recv", 9) == 0 && (i + 9 >= n || !cc_is_ident_char(src[i + 9]))) {
                if (i > 0 && cc_is_ident_char(src[i - 1])) { /* skip */ }
                else { is_chan_op = 1; op_name = "chan_recv"; op_len = 9; }
            }

            if (is_chan_op) {
                /* Check if preceded by "await" (skipping whitespace/comments backwards) */
                int has_await = 0;
                size_t k = i;
                while (k > 0) {
                    k--;
                    char ck = src[k];
                    if (ck == ' ' || ck == '\t' || ck == '\n' || ck == '\r') continue;
                    /* Check for "await" ending at position k */
                    if (k >= 4 && memcmp(src + k - 4, "await", 5) == 0) {
                        /* Ensure "await" is not part of a larger identifier */
                        if (k >= 5 && cc_is_ident_char(src[k - 5])) break;
                        has_await = 1;
                    }
                    break;
                }

                if (!has_await) {
                    char rel[1024];
                    cc_path_rel_to_repo(input_path, rel, sizeof(rel));
                    cc_pp_error_cat(rel, line, col, "async", "channel operation '%s' must be awaited in @async function", op_name);
                    fprintf(stderr, "  hint: add 'await' before this call\n");
                    errors++;
                }
                i += op_len - 1; /* Skip past the op name */
            }
        }
    }

    return errors > 0 ? -1 : 0;
}

/* Track @async functions for @nonblocking inference. */
typedef struct {
    char name[128];
    int is_explicit_nonblocking;  /* Has @nonblocking attribute */
    int has_loop_with_chan_op;    /* Contains loop with channel ops */
} CCAsyncFnInfo;

#define CC_MAX_ASYNC_FNS 256
static CCAsyncFnInfo cc__async_fns[CC_MAX_ASYNC_FNS];
static int cc__async_fn_count = 0;

/* Check if an @async function is nonblocking (either explicit or inferred).
   Note: Reserved for future cross-file analysis. */
__attribute__((unused))
static int cc__fn_is_nonblocking(const char* name) {
    for (int i = 0; i < cc__async_fn_count; i++) {
        if (strcmp(cc__async_fns[i].name, name) == 0) {
            if (cc__async_fns[i].is_explicit_nonblocking) return 1;
            if (!cc__async_fns[i].has_loop_with_chan_op) return 1;
            return 0;
        }
    }
    /* Unknown function - assume nonblocking (could be from another file) */
    return 1;
}

/* Collect @async function info and check cc_block_on calls.
   Warns if cc_block_on is called with a function that has loops with channel ops.
   Returns number of warnings. */
static int cc__check_block_on_nonblocking(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return 0;

    cc__async_fn_count = 0;
    int warnings = 0;

    /* Pass 1: Collect @async functions and determine if they're @nonblocking */
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    int line = 1, col = 1;

    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;

        if (c == '\n') { line++; col = 1; } else { col++; }

        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; col++; } continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i++; col++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i++; col++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; col++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; col++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }

        /* Look for @async or @async @nonblocking */
        if (c == '@' && i + 5 < n && memcmp(src + i + 1, "async", 5) == 0 && !cc_is_ident_char(src[i + 6])) {
            size_t j = i + 6;
            int is_explicit_nb = 0;

            /* Skip whitespace and check for @nonblocking */
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
            if (j + 12 < n && src[j] == '@' && memcmp(src + j + 1, "nonblocking", 11) == 0 && !cc_is_ident_char(src[j + 12])) {
                is_explicit_nb = 1;
                j += 12;
            }

            /* Skip to function name: skip return type and get identifier before '(' */
            while (j < n && src[j] != '(' && src[j] != '{' && src[j] != ';') j++;
            if (j >= n || src[j] != '(') continue;

            /* Back up to find function name */
            size_t paren = j;
            j--;
            while (j > i && (src[j] == ' ' || src[j] == '\t')) j--;
            if (j <= i || !cc_is_ident_char(src[j])) continue;

            size_t name_end = j + 1;
            while (j > i && cc_is_ident_char(src[j - 1])) j--;
            size_t name_start = j;
            size_t name_len = name_end - name_start;

            if (name_len == 0 || name_len >= 127) continue;

            /* Find function body */
            j = paren + 1;
            int depth = 1;
            while (j < n && depth > 0) {
                if (src[j] == '(') depth++;
                else if (src[j] == ')') depth--;
                j++;
            }
            while (j < n && src[j] != '{' && src[j] != ';') j++;
            if (j >= n || src[j] != '{') continue;

            size_t body_start = j;
            depth = 1;
            j++;
            while (j < n && depth > 0) {
                if (src[j] == '{') depth++;
                else if (src[j] == '}') depth--;
                j++;
            }
            size_t body_end = j;

            /* Check for loops with channel ops in the body */
            int has_loop_with_chan = 0;

            for (size_t k = body_start; k < body_end && !has_loop_with_chan; k++) {
                /* Simple check: look for 'for' or 'while' keyword */
                int is_for = (k + 3 <= body_end && memcmp(src + k, "for", 3) == 0);
                int is_while = (k + 5 <= body_end && memcmp(src + k, "while", 5) == 0);
                if (!is_for && !is_while) continue;
                if (k > 0 && cc_is_ident_char(src[k - 1])) continue;
                size_t kw_len = is_for ? 3 : 5;
                if (k + kw_len < body_end && cc_is_ident_char(src[k + kw_len])) continue;

                /* Found a loop keyword - scan to find its body, handling parentheses in for() header */
                size_t loop_start = k + kw_len;
                int paren_depth = 0;
                while (loop_start < body_end) {
                    char lc = src[loop_start];
                    if (lc == '(') paren_depth++;
                    else if (lc == ')') paren_depth--;
                    else if (lc == '{' && paren_depth == 0) break;  /* Found loop body */
                    else if (lc == ';' && paren_depth == 0) break;  /* Single-statement loop (no braces) */
                    loop_start++;
                }
                if (loop_start >= body_end || src[loop_start] != '{') continue; /* Skip single-line loops for now */

                int ld = 1;
                size_t loop_end = loop_start + 1;
                while (loop_end < body_end && ld > 0) {
                    if (src[loop_end] == '{') ld++;
                    else if (src[loop_end] == '}') ld--;
                    loop_end++;
                }

                /* Check for channel ops in loop body - look for .send( or .recv( */
                for (size_t m = loop_start; m < loop_end; m++) {
                    if (m + 5 <= loop_end && memcmp(src + m, ".send", 5) == 0) {
                        has_loop_with_chan = 1;
                        break;
                    }
                    if (m + 5 <= loop_end && memcmp(src + m, ".recv", 5) == 0) {
                        has_loop_with_chan = 1;
                        break;
                    }
                    /* Also check for chan_send/chan_recv macro forms */
                    if (m + 9 <= loop_end && memcmp(src + m, "chan_send", 9) == 0) {
                        has_loop_with_chan = 1;
                        break;
                    }
                    if (m + 9 <= loop_end && memcmp(src + m, "chan_recv", 9) == 0) {
                        has_loop_with_chan = 1;
                        break;
                    }
                }
            }

            /* Store function info */
            if (cc__async_fn_count < CC_MAX_ASYNC_FNS) {
                memcpy(cc__async_fns[cc__async_fn_count].name, src + name_start, name_len);
                cc__async_fns[cc__async_fn_count].name[name_len] = '\0';
                cc__async_fns[cc__async_fn_count].is_explicit_nonblocking = is_explicit_nb;
                cc__async_fns[cc__async_fn_count].has_loop_with_chan_op = has_loop_with_chan;
                cc__async_fn_count++;
            }

            i = body_end - 1;
            continue;
        }
    }

    /* Pass 2: Check cc_block_on calls */
    in_lc = in_bc = in_str = in_chr = 0;
    line = 1; col = 1;

    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;

        if (c == '\n') { line++; col = 1; } else { col++; }

        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; col++; } continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i++; col++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i++; col++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; col++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; col++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }

        /* Look for cc_block_on( */
        if (c == 'c' && i + 11 < n && memcmp(src + i, "cc_block_on", 11) == 0 && src[i + 11] == '(') {
            int call_line = line, call_col = col;
            size_t j = i + 12;

            /* Skip the type argument */
            while (j < n && src[j] != ',') j++;
            if (j >= n) continue;
            j++; /* Skip comma */

            /* Skip whitespace */
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;

            /* Extract the function name being called */
            if (j >= n || !cc_is_ident_start(src[j])) continue;
            size_t fn_start = j;
            while (j < n && cc_is_ident_char(src[j])) j++;
            size_t fn_len = j - fn_start;

            if (fn_len == 0 || fn_len >= 127) continue;

            char fn_name[128];
            memcpy(fn_name, src + fn_start, fn_len);
            fn_name[fn_len] = '\0';

            /* Check if this function is known and not @nonblocking */
            for (int fi = 0; fi < cc__async_fn_count; fi++) {
                if (strcmp(cc__async_fns[fi].name, fn_name) == 0) {
                    if (!cc__async_fns[fi].is_explicit_nonblocking && cc__async_fns[fi].has_loop_with_chan_op) {
                        char rel[1024];
                        cc_path_rel_to_repo(input_path, rel, sizeof(rel));
                        fprintf(stderr, "%s:%d:%d: warning: cc_block_on with '%s' may deadlock\n",
                                rel, call_line, call_col, fn_name);
                        fprintf(stderr, "%s:%d:%d: note: '%s' has channel ops in a loop; consider @nursery for concurrency or larger buffer\n",
                                rel, call_line, call_col, fn_name);
                        warnings++;
                    }
                    break;
                }
            }

            i = j - 1;
        }
    }

    return warnings;
}

/* Infer result constructor types from enclosing function signature.
   Within a function returning `T!E`:
     cc_ok(v)   -> cc_ok(T, v)     for T!CCError
     cc_ok(v)   -> cc_ok(T, E, v)  for T!E (custom error)
     cc_err(e)  -> cc_err(T, e)    for T!CCError  
     cc_err(e)  -> cc_err(T, E, e) for T!E (custom error)
   
   This allows users to write just `cc_ok(42)` and have the compiler infer the type. */
static char* cc__rewrite_inferred_result_ctors(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t last_emit = 0;
    
    /* Track current function's result type */
    char current_ok_type[128] = {0};   /* e.g., "int" */
    char current_err_type[128] = {0};  /* e.g., "CCError" or custom */
    int brace_depth = 0;
    int fn_brace_depth = -1;
    
    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    
    for (size_t i = 0; i < n; ) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        /* Skip comments and strings */
        if (in_lc) { if (c == '\n') in_lc = 0; i++; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        
        if (c == '/' && c2 == '/') { in_lc = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Track braces */
        if (c == '{') {
            brace_depth++;
            i++;
            continue;
        }
        if (c == '}') {
            brace_depth--;
            /* `fn_brace_depth` holds the depth BEFORE the function body's
               `{`, so the reset must fire when brace_depth decrements past
               that value (i.e. the matching `}` closed the body).  The old
               `brace_depth < fn_brace_depth` check was strictly less-than,
               which let `current_ok_type` / `current_err_type` leak into
               the next function.  That was harmless under the previous
               regime — every `cc_ok`/`cc_err` call was flattened through
               the `__CC_RESULT_OK(0, 0, v)` placeholder so the stale T/E
               never reached TCC — but with real typed ctors the stale
               types now surface as "cannot convert struct CCResult_B_CCError
               to struct CCResult_A_CCError" between adjacent functions. */
            if (fn_brace_depth >= 0 && brace_depth <= fn_brace_depth) {
                current_ok_type[0] = 0;
                current_err_type[0] = 0;
                fn_brace_depth = -1;
            }
            i++;
            continue;
        }
        
        /* Detect function returning T!E or T!>(E) - look for pattern: T!E name( or T!>(E) name( 
           Handle space before ! (e.g., "MyData !> (MyError)" or "MyData*!>(MyError)") */
        if (c == '!' && c2 != '=' && fn_brace_depth < 0 && i > 0) {
            /* Skip backwards over whitespace to find the type */
            size_t prev_idx = i - 1;
            while (prev_idx > 0 && (src[prev_idx] == ' ' || src[prev_idx] == '\t')) prev_idx--;
            char prev = src[prev_idx];
            /* Valid chars before !: identifier chars, closing brackets, pointer star */
            if (cc_is_ident_char(prev) || prev == ')' || prev == ']' || prev == '>' || prev == '*') {
                /* Check for error type after ! - two forms:
                   1. T!E (simple form)
                   2. T!>(E) (arrow form with parentheses) */
                size_t j = i + 1;
                size_t err_start = 0, err_end = 0;
                
                /* Check for !> arrow form */
                if (j < n && src[j] == '>') {
                    j++; /* skip '>' */
                    while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                    if (j < n && src[j] == '(') {
                        j++; /* skip '(' */
                        while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                        err_start = j;
                        while (j < n && cc_is_ident_char(src[j])) j++;
                        err_end = j;
                        while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                        if (j < n && src[j] == ')') j++; /* skip ')' */
                    }
                } else {
                    /* Simple !E form */
                    while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                    if (j < n && cc_is_ident_start(src[j])) {
                        err_start = j;
                        while (j < n && cc_is_ident_char(src[j])) j++;
                        err_end = j;
                    }
                }
                
                if (err_start < err_end) {
                    
                    /* Skip whitespace, then check for identifier followed by '(' */
                    while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '*')) j++;
                    if (j < n && cc_is_ident_start(src[j])) {
                        size_t name_start = j;
                        while (j < n && cc_is_ident_char(src[j])) j++;
                        (void)name_start; /* function name, not needed */
                        while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                        if (j < n && src[j] == '(') {
                            /* Skip params to find '{' */
                            int pdepth = 1;
                            j++;
                            while (j < n && pdepth > 0) {
                                if (src[j] == '(') pdepth++;
                                else if (src[j] == ')') pdepth--;
                                else if (src[j] == '"') { j++; while (j < n && src[j] != '"') { if (src[j] == '\\' && j+1<n) j++; j++; } }
                                else if (src[j] == '\'') { j++; while (j < n && src[j] != '\'') { if (src[j] == '\\' && j+1<n) j++; j++; } }
                                j++;
                            }
                            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;
                            if (j < n && src[j] == '{') {
                                /* Found function definition! Extract ok and err types */
                                size_t ty_start = cc__scan_back_to_delim(src, i);
                                if (ty_start < i) {
                                    ty_start = cc__skip_leading_decl_specs(src, ty_start);
                                    size_t ty_len = i - ty_start;
                                    if (ty_len < sizeof(current_ok_type) - 1) {
                                        /* Trim whitespace */
                                        while (ty_len > 0 && (src[ty_start + ty_len - 1] == ' ' || src[ty_start + ty_len - 1] == '\t')) ty_len--;
                                        memcpy(current_ok_type, src + ty_start, ty_len);
                                        current_ok_type[ty_len] = 0;
                                    }
                                    size_t err_len = err_end - err_start;
                                    if (err_len < sizeof(current_err_type) - 1) {
                                        memcpy(current_err_type, src + err_start, err_len);
                                        current_err_type[err_len] = 0;
                                    }
                                    fn_brace_depth = brace_depth;
                                    
                                    /* Register this result type so it gets declared in parse stubs.
                                       This is critical for pointer result types that are declared first. */
                                    char mangled_ok[128], mangled_err[128];
                                    cc__mangle_type_name(current_ok_type, strlen(current_ok_type), mangled_ok, sizeof(mangled_ok));
                                    cc__mangle_type_name(current_err_type, strlen(current_err_type), mangled_err, sizeof(mangled_err));
                                    cc__add_result_type(current_ok_type, strlen(current_ok_type),
                                                        current_err_type, strlen(current_err_type),
                                                        mangled_ok, mangled_err);
                                }
                            }
                        }
                    }
                }
            }
            i++;
            continue;
        }
        
        /* Rewrite cc_ok(...) and cc_err(...) when inside result-returning function */
        if (current_ok_type[0] && c == 'c' && i + 5 < n) {
            int is_ok = (memcmp(src + i, "cc_ok(", 6) == 0);
            int is_err = (memcmp(src + i, "cc_err(", 7) == 0);
            
            if (is_ok || is_err) {
                /* Check word boundary */
                int word_start = (i == 0) || !cc_is_ident_char(src[i - 1]);
                if (word_start) {
                    size_t macro_start = i;
                    size_t paren_pos = i + (is_ok ? 5 : 6);
                    
                    /* Count args to determine if short form */
                    size_t args_start = paren_pos + 1;
                    size_t j = args_start;
                    int depth = 1;
                    int comma_count = 0;
                    int in_s = 0, in_c = 0;
                    while (j < n && depth > 0) {
                        char ch = src[j];
                        if (in_s) { if (ch == '\\' && j+1<n) j++; else if (ch == '"') in_s = 0; j++; continue; }
                        if (in_c) { if (ch == '\\' && j+1<n) j++; else if (ch == '\'') in_c = 0; j++; continue; }
                        if (ch == '"') { in_s = 1; j++; continue; }
                        if (ch == '\'') { in_c = 1; j++; continue; }
                        if (ch == '(') depth++;
                        else if (ch == ')') { depth--; if (depth == 0) break; }
                        else if (ch == ',' && depth == 1) comma_count++;
                        j++;
                    }
                    
                    /* Short form: cc_ok(v) has 0 commas, cc_err(e) has 0 commas
                       Long form: cc_ok(T,v) has 1+ commas
                       Shorthand: cc_err(CC_ERR_*, "msg") has 1 comma - expand to CC_ERROR(...) */
                    int is_short = (comma_count == 0);
                    
                    /* cc_err shorthand: any 2-arg cc_err(KIND_EXPR, MSG) inside
                       a CCError-keyed result function — not just the literal
                       CC_ERR_* form.  The old check was text-based ("first arg
                       starts with CC_ERR_"), which let forwarding cases like
                       `cc_err(e.kind, "fwd")` fall through to the parser-mode
                       `__CCResultGeneric cc_err(int, const char*)` stub.  That
                       worked when every Result aliased __CCResultGeneric; now
                       that parser mode emits real typed Result structs, the
                       stub's return type no longer converts to the enclosing
                       function's CCResult_T_CCError and TCC rejects it.  Treat
                       the 2-arg form as the sugared equivalent of
                           cc_err_CCResult_T_E(CC_ERROR(KIND, MSG))
                       for both literal and expression KIND arguments. */
                    int is_err_shorthand = 0;
                    int is_err_shorthand_no_msg = 0;
                    if (is_err && strcmp(current_err_type, "CCError") == 0) {
                        if (comma_count == 1) {
                            is_err_shorthand = 1;
                        } else if (comma_count == 0) {
                            /* Single-arg `cc_err(CC_ERR_*)` form — the literal
                               CC_ERR_* prefix lets us synthesise the default
                               NULL message.  For non-literal single-arg forms
                               we still treat it as a plain typed ctor below. */
                            size_t k = args_start;
                            while (k < j && (src[k] == ' ' || src[k] == '\t')) k++;
                            if (k + 7 < j && memcmp(src + k, "CC_ERR_", 7) == 0) {
                                is_err_shorthand = 1;
                                is_err_shorthand_no_msg = 1;
                            }
                        }
                    }
                    
                    if (is_err_shorthand && depth == 0) {
                        /* Rewrite cc_err(CC_ERR_*) -> cc_err_CCResult_T_E(CC_ERROR(CC_ERR_*, NULL))
                           Rewrite cc_err(CC_ERR_*, "msg") -> cc_err_CCResult_T_E(CC_ERROR(CC_ERR_*, "msg"))
                           Type names must be mangled (e.g., MyData* -> MyDataptr) */
                        char mangled_ok[128], mangled_err[128];
                        cc__mangle_type_name(current_ok_type, strlen(current_ok_type), mangled_ok, sizeof(mangled_ok));
                        cc__mangle_type_name(current_err_type, strlen(current_err_type), mangled_err, sizeof(mangled_err));
                        
                        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, macro_start - last_emit);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_err_CCResult_");
                        cc_sb_append_cstr(&out, &out_len, &out_cap, mangled_ok);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "_");
                        cc_sb_append_cstr(&out, &out_len, &out_cap, mangled_err);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "(CC_ERROR(");
                        /* Copy args */
                        cc_sb_append(&out, &out_len, &out_cap, src + args_start, j - args_start);
                        if (is_err_shorthand_no_msg) {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ", NULL");
                        }
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "))");
                        last_emit = j + 1;
                        i = j + 1;
                        continue;
                    }
                    
                    if (is_short && depth == 0) {
                        /* Rewrite short form to typed constructor call:
                           cc_ok(x) -> cc_ok_CCResult_T_E(x)
                           cc_err(e) -> cc_err_CCResult_T_E(e)
                           Type names must be mangled (e.g., MyData* -> MyDataptr) */
                        char mangled_ok[128], mangled_err[128];
                        cc__mangle_type_name(current_ok_type, strlen(current_ok_type), mangled_ok, sizeof(mangled_ok));
                        cc__mangle_type_name(current_err_type, strlen(current_err_type), mangled_err, sizeof(mangled_err));
                        
                        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, macro_start - last_emit);
                        
                        if (is_ok) {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_ok_CCResult_");
                        } else {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_err_CCResult_");
                        }
                        cc_sb_append_cstr(&out, &out_len, &out_cap, mangled_ok);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "_");
                        cc_sb_append_cstr(&out, &out_len, &out_cap, mangled_err);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "(");
                        /* Copy argument */
                        cc_sb_append(&out, &out_len, &out_cap, src + args_start, j - args_start);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                        last_emit = j + 1;
                        i = j + 1;
                        continue;
                    }
                }
            }
        }
        
        i++;
    }
    
    if (last_emit == 0) return NULL;
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Check for cc_concurrent usage and emit error with migration guidance.
   cc_concurrent syntax is deprecated; use cc_block_all instead. */
static char* cc__rewrite_cc_concurrent(const char* src, size_t n) {
    if (!src || n == 0) return NULL;

    int in_lc = 0, in_bc = 0, in_str = 0, in_chr = 0;
    int line = 1;

    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;

        if (c == '\n') line++;

        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; i++; } continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
        if (c == '/' && c2 == '/') { in_lc = 1; i++; continue; }
        if (c == '/' && c2 == '*') { in_bc = 1; i++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }

        if (c == 'c' && i + 12 < n && memcmp(src + i, "cc_concurrent", 13) == 0) {
            if (i > 0 && cc_is_ident_char(src[i - 1])) continue;
            if (i + 13 < n && cc_is_ident_char(src[i + 13])) continue;

            cc_pp_error_cat("<input>", line, 1, "syntax", "'cc_concurrent' syntax is deprecated (use cc_block_all instead)");
            fprintf(stderr, "  note: use cc_block_all() instead:\n");
            fprintf(stderr, "    CCTaskIntptr tasks[] = { task1(), task2() };\n");
            fprintf(stderr, "    intptr_t results[2];\n");
            fprintf(stderr, "    cc_block_all(2, tasks, results);\n");
            return (char*)-1;
        }
    }

    return NULL;
}
int cc_preprocess_file(const char* input_path, char* out_path, size_t out_path_sz) {
    if (!input_path || !out_path || out_path_sz == 0) return -1;
    char tmp_path[] = "/tmp/cc_pp_XXXXXX.c";
    int fd = mkstemps(tmp_path, 2); /* keep .c suffix */
    if (fd < 0) return -1;

    /* Create type registry for this file (or reuse existing global) */
    CCTypeRegistry* reg = cc_type_registry_get_global();
    if (!reg) {
        reg = cc_type_registry_new();
        cc_type_registry_set_global(reg);
    }
    cc_type_registry_clear(reg);
    cc_result_spec_table_set_global(&cc__result_specs);

    FILE *in = fopen(input_path, "r");
    FILE *out = fdopen(fd, "w");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    /* Read full file so we can rewrite constructs that are not valid C syntax. */
    fseek(in, 0, SEEK_END);
    long in_n = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (in_n < 0 || in_n > (1 << 22)) { /* 4MB cap for now */
        fclose(in);
        fclose(out);
        unlink(tmp_path);
        return -1;
    }
    char* buf = (char*)malloc((size_t)in_n + 1);
    if (!buf) {
        fclose(in);
        fclose(out);
        unlink(tmp_path);
        return -1;
    }
    size_t got = fread(buf, 1, (size_t)in_n, in);
    buf[got] = 0;

    /* Check for unawaited channel ops in @async functions (before rewrites).
       Skip this check for temp/reparse files (they have already been checked). */
    const char* basename = strrchr(input_path, '/');
    basename = basename ? basename + 1 : input_path;
    int is_temp_file = (strncmp(basename, "cc_reparse_", 11) == 0 ||
                        strncmp(basename, "cc_pp_", 6) == 0 ||
                        strncmp(input_path, "/tmp/", 5) == 0);
    if (!is_temp_file) {
        int chan_err = cc__check_async_chan_await(buf, got, input_path);
        if (chan_err != 0) {
            fclose(in);
            fclose(out);
            free(buf);
            unlink(tmp_path);
            return -1;
        }
        /* Check for cc_block_on with non-@nonblocking functions (warning only) */
        cc__check_block_on_nonblocking(buf, got, input_path);
    }

    /* --- Apply preprocessing passes using chain helper --- */
    CCPassChain chain;
    cc_pass_chain_init(&chain, buf, got);
    
    /* Shared phase-1 canonical CC normalization bucket. */
    if (cc__apply_phase1_canonical_passes(&chain, input_path) != 0) goto chain_cleanup;
    /* Transitional pre-phase-3 exception: nursery handle prototype synthesis
       still runs outside the shared host-lowering bucket. */
    CC_CHAIN(chain, cc_rewrite_nursery_create_destroy_proto(chain.src, chain.len, input_path));
    /* Shared phase-3 host lowering bucket. */
    if (cc__apply_phase3_host_lowering_passes(&chain, input_path) != 0) goto chain_cleanup;
    
    const char* use = chain.src;

    /* Emit container type declarations from type registry */
    {
        size_t n_vec = cc_type_registry_vec_count(reg);
        size_t n_map = cc_type_registry_map_count(reg);
        size_t n_chan = cc_type_registry_channel_count(reg);
        
        if (n_vec > 0 || n_map > 0 || n_chan > 0) {
            fprintf(out, "/* --- CC generic container declarations --- */\n");
            fprintf(out, "#include <ccc/std/vec.h>\n");
            fprintf(out, "#include <ccc/std/map.h>\n");
            fprintf(out, "#include <ccc/cc_channel.h>\n");
            /* Vec/Map declarations must be skipped in parser mode where they're
               already typedef'd to generic placeholders in vec.cch/map.cch */
            fprintf(out, "#ifndef CC_PARSER_MODE\n");
            
            /* Emit Vec declarations */
            for (size_t i = 0; i < n_vec; i++) {
                const CCTypeInstantiation* inst = cc_type_registry_get_vec(reg, i);
                if (inst && inst->type1 && inst->mangled_name) {
                    /* Extract mangled element name from CCVec_xxx */
                    const char* mangled_elem = inst->mangled_name + 6; /* Skip "CCVec_" */
                    
                    /* Skip CCVec_char - it's predeclared in string.cch */
                    if (strcmp(mangled_elem, "char") == 0) {
                        continue;
                    }

                    /* Optionals retired — always use the 2-arg macro.
                     * vec.cch treats the legacy 3-arg form as ignoring OptT. */
                    fprintf(out, "CC_VEC_DECL_ARENA(%s, %s)\n", inst->type1, inst->mangled_name);
                }
            }
            
            /* Emit Map declarations (using default hash functions for known types) */
            for (size_t i = 0; i < n_map; i++) {
                const CCTypeInstantiation* inst = cc_type_registry_get_map(reg, i);
                if (inst && inst->type1 && inst->type2 && inst->mangled_name) {
                    const char* hash_fn = "cc_map_hash_i32";
                    const char* eq_fn = "cc_map_eq_i32";
                    if (strcmp(inst->type1, "int") == 0) {
                        hash_fn = "cc_map_hash_i32"; eq_fn = "cc_map_eq_i32";
                    } else if (strcmp(inst->type1, "CCSliceHdr") == 0) {
                        hash_fn = "cc_map_hash_slice_hdr"; eq_fn = "cc_map_eq_slice_hdr";
                    } else if (strstr(inst->type1, "64") != NULL) {
                        hash_fn = "cc_map_hash_u64"; eq_fn = "cc_map_eq_u64";
                    } else if (strstr(inst->type1, "slice") != NULL || strstr(inst->type1, "Slice") != NULL || strcmp(inst->type1, "charslice") == 0) {
                        hash_fn = "cc_map_hash_slice"; eq_fn = "cc_map_eq_slice";
                    }
                    /* Optionals retired — always use the 5-arg macro. */
                    fprintf(out, "CC_MAP_DECL_ARENA(%s, %s, %s, %s, %s)\n",
                            inst->type1, inst->type2, inst->mangled_name, hash_fn, eq_fn);
                }
            }

            for (size_t i = 0; i < n_chan; i++) {
                const CCTypeInstantiation* inst = cc_type_registry_get_channel(reg, i);
                if (inst && inst->type1 && inst->mangled_name) {
                }
            }
            
            fprintf(out, "#endif /* !CC_PARSER_MODE */\n");
            fprintf(out, "/* --- end container declarations --- */\n\n");
        }
    }

    /* If result types are used, include cc_result.cch with CC_PARSER_MODE
       so that __CC_RESULT(T, E) expands to __CCResultGeneric for TCC parsing. */
    if (cc__result_specs.count > 0) {
        fprintf(out, "/* --- CC result type support --- */\n");
        fprintf(out, "#ifndef CC_PARSER_MODE\n");
        fprintf(out, "#define CC_PARSER_MODE 1\n");
        fprintf(out, "#endif\n");
        fprintf(out, "#include <ccc/cc_result.h>\n");
        fprintf(out, "/* --- end result support --- */\n\n");
    }

    char rel[1024];
    fprintf(out, "#line 1 \"%s\"\n", cc_path_rel_to_repo(input_path, rel, sizeof(rel)));
    fputs(use, out);

    cc_pass_chain_free(&chain);
    free(buf);
    fclose(in);
    fclose(out);

    strncpy(out_path, tmp_path, out_path_sz - 1);
    out_path[out_path_sz - 1] = '\0';
    return 0;

chain_cleanup:
    /* Error path - cleanup allocations and return failure */
    cc_pass_chain_free(&chain);
    free(buf);
    fclose(in);
    fclose(out);
    if (!getenv("CC_KEEP_PP")) unlink(tmp_path);
    else fprintf(stderr, "CC_KEEP_PP: preprocess tmp kept at %s\n", tmp_path);
    return -1;
}

// Preprocess source string to output string (no temp files).
// skip_checks: if true, skip validation checks (use for reparse passes).
char* cc_preprocess_to_string_ex(const char* input, size_t input_len, const char* input_path, int skip_checks) {
    if (!input || input_len == 0) return NULL;

    /* Create type registry for this file (or reuse existing global) */
    CCTypeRegistry* reg = cc_type_registry_get_global();
    if (!reg) {
        reg = cc_type_registry_new();
        cc_type_registry_set_global(reg);
    }
    cc_type_registry_clear(reg);

    /* Make a copy of input since some rewrites modify in-place */
    char* buf = (char*)malloc(input_len + 1);
    if (!buf) return NULL;
    memcpy(buf, input, input_len);
    buf[input_len] = 0;
    size_t got = input_len;

    /* Check for unawaited channel ops in @async functions (before rewrites).
       Skip if requested (reparse passes) or if path looks like a temp file. */
    if (!skip_checks && input_path) {
        const char* basename = strrchr(input_path, '/');
        basename = basename ? basename + 1 : input_path;
        int is_temp_file = basename && (strncmp(basename, "cc_reparse_", 11) == 0 ||
                            strncmp(basename, "cc_pp_", 6) == 0 ||
                            strncmp(input_path, "/tmp/", 5) == 0);
        if (!is_temp_file) {
            int chan_err = cc__check_async_chan_await(buf, got, input_path);
            if (chan_err != 0) {
                free(buf);
                return NULL;
            }
            /* Check for cc_block_on with non-@nonblocking functions (warning only) */
            cc__check_block_on_nonblocking(buf, got, input_path);
        }
    }

    /* --- Apply preprocessing passes using chain helper --- */
    CCPassChain chain;
    cc_pass_chain_init(&chain, buf, got);
    
    /* Shared phase-1 canonical CC normalization bucket. */
    if (cc__apply_phase1_canonical_passes(&chain, input_path) != 0) goto chain_cleanup;
    /* Shared phase-3 bucket: parser/host-C survival and lowering. */
    if (cc__apply_phase3_host_lowering_passes(&chain, input_path) != 0) goto chain_cleanup;
    const char* use = chain.src;
    (void)chain.len;  /* use_n not needed here */

    /* Build output string using open_memstream (POSIX) */
    char* out_buf = NULL;
    size_t out_size = 0;
    FILE* out = open_memstream(&out_buf, &out_size);
    if (!out) {
        cc_pass_chain_free(&chain);
        free(buf);
        return NULL;
    }

    fprintf(out, "#ifndef __CC__\n");
    fprintf(out, "#define __CC__ 1\n");
    fprintf(out, "#endif\n");

    /* Emit container type declarations from type registry */
    {
        size_t n_vec = cc_type_registry_vec_count(reg);
        size_t n_map = cc_type_registry_map_count(reg);
        size_t n_chan = cc_type_registry_channel_count(reg);
        
        if (n_vec > 0 || n_map > 0 || n_chan > 0) {
            fprintf(out, "/* --- CC generic container declarations --- */\n");
            fprintf(out, "#include <ccc/std/vec.cch>\n");
            fprintf(out, "#include <ccc/std/map.cch>\n");
            fprintf(out, "#include <ccc/cc_channel.cch>\n");
            fprintf(out, "#ifndef CC_PARSER_MODE\n");
            
            /* Emit Vec declarations */
            for (size_t i = 0; i < n_vec; i++) {
                const CCTypeInstantiation* inst = cc_type_registry_get_vec(reg, i);
                if (inst && inst->type1 && inst->mangled_name) {
                    const char* mangled_elem = inst->mangled_name + 6; /* Skip "CCVec_" */
                    if (strcmp(mangled_elem, "char") == 0) continue;
                    /* Optionals retired — always use the 2-arg macro. */
                    fprintf(out, "CC_VEC_DECL_ARENA(%s, %s)\n", inst->type1, inst->mangled_name);
                }
            }
            
            /* Emit Map declarations */
            for (size_t i = 0; i < n_map; i++) {
                const CCTypeInstantiation* inst = cc_type_registry_get_map(reg, i);
                if (inst && inst->type1 && inst->type2 && inst->mangled_name) {
                    const char* hash_fn = "cc_map_hash_i32";
                    const char* eq_fn = "cc_map_eq_i32";
                    if (strcmp(inst->type1, "int") == 0) {
                        hash_fn = "cc_map_hash_i32"; eq_fn = "cc_map_eq_i32";
                    } else if (strcmp(inst->type1, "CCSliceHdr") == 0) {
                        hash_fn = "cc_map_hash_slice_hdr"; eq_fn = "cc_map_eq_slice_hdr";
                    } else if (strstr(inst->type1, "64") != NULL) {
                        hash_fn = "cc_map_hash_u64"; eq_fn = "cc_map_eq_u64";
                    } else if (strstr(inst->type1, "slice") != NULL || strstr(inst->type1, "Slice") != NULL || strcmp(inst->type1, "charslice") == 0) {
                        hash_fn = "cc_map_hash_slice"; eq_fn = "cc_map_eq_slice";
                    }
                    /* Optionals retired — always use the 5-arg macro. */
                    fprintf(out, "CC_MAP_DECL_ARENA(%s, %s, %s, %s, %s)\n",
                            inst->type1, inst->type2, inst->mangled_name, hash_fn, eq_fn);
                }
            }

            for (size_t i = 0; i < n_chan; i++) {
                const CCTypeInstantiation* inst = cc_type_registry_get_channel(reg, i);
                if (inst && inst->type1 && inst->mangled_name) {
                }
            }
            
            fprintf(out, "#endif /* !CC_PARSER_MODE */\n");
            fprintf(out, "/* --- end container declarations --- */\n\n");
        }
    }

    /* Include cc_result.cch whenever:
     *   - the TU declares Result types (`cc__result_specs.count > 0`), OR
     *   - the TU uses the `!>` / `?>` operators on raw pointer returns.
     * The latter case synthesizes `CCError` / `CC_ERR_NULL` in the lowered
     * output (see pass_result_unwrap.c / pass_err_syntax.c pointer-path
     * emission); without the include, those symbols would be undeclared. */
    /* Decide whether to force-include `cc_result.cch`.  Previously this
     * used a raw `strstr(use, "!>")` presence check, which accidentally
     * fired on `!>` sigils inside comments — including the top-of-file
     * doc comment in many tests.  Post-phase-3 the real `!>` / `?>`
     * sigils have already been lowered away, so a comment-aware check
     * on those alone would miss tests whose body uses the unwrap ops
     * but whose lowered output only references the generated symbols
     * (`CCError`, `CC_ERR_NULL`, `cc_ok`, `cc_err`, `cc_is_err`,
     * `cc_unwrap`).  Check for either:
     *   - any pre-lowering sigil that might still be there in passes
     *     that re-enter (code-aware to not be fooled by comments), or
     *   - any of the generated Result runtime symbols emitted by
     *     pass_result_unwrap / pass_err_syntax.
     * This replaces the previous by-accident behavior with an explicit
     * signal and removes the hidden dependency on comment contents. */
    size_t use_len = use ? strlen(use) : 0;
    int uses_unwrap_ops = use && (
        cc_contains_token_top_level(use, use_len, "!>") ||
        cc_contains_token_top_level(use, use_len, "?>") ||
        cc_contains_token_top_level(use, use_len, "CCError") ||
        cc_contains_token_top_level(use, use_len, "CC_ERR_NULL") ||
        cc_contains_token_top_level(use, use_len, "cc_ok") ||
        cc_contains_token_top_level(use, use_len, "cc_err") ||
        cc_contains_token_top_level(use, use_len, "cc_is_err") ||
        cc_contains_token_top_level(use, use_len, "cc_unwrap"));
    if (cc__result_specs.count > 0 || uses_unwrap_ops) {
        fprintf(out, "/* --- CC result type support --- */\n");
        fprintf(out, "#ifndef CC_PARSER_MODE\n");
        fprintf(out, "#define CC_PARSER_MODE 1\n");
        fprintf(out, "#endif\n");
        fprintf(out, "#include <ccc/cc_result.cch>\n");
        /* User-declared `CCResult_X_Y` specs are emitted later at `insert_pos`
         * (after the TU's `#include` / `typedef` prelude) as real typed
         * `CC_DECL_RESULT_SPEC(CCResult_X_Y, X, Y)` expansions.  At that point
         * both payload and error types are in scope, so the generated struct
         * carries the declared layout — not the legacy `intptr_t` aliased to
         * `__CCResultGeneric`.  See docs/known-bugs/redis_idiomatic_async.md
         * "parser-mode result-type collapse". */
        fprintf(out, "/* --- end result support --- */\n\n");
    }

    char rel[1024];
    {
        char* lowered_system_use = cc_rewrite_system_cch_includes_to_lowered_headers(use, strlen(use));
        if (lowered_system_use) {
            use = lowered_system_use;
        }
        fprintf(out, "#line 1 \"%s\"\n", cc_path_rel_to_repo(input_path ? input_path : "<string>", rel, sizeof(rel)));
        {
            size_t use_len = strlen(use);
            size_t insert_pos = 0;
            while (insert_pos < use_len) {
                size_t line_start = insert_pos;
                size_t line_end = line_start;
                while (line_end < use_len && use[line_end] != '\n') line_end++;
                size_t p = line_start;
                while (p < line_end && (use[p] == ' ' || use[p] == '\t' || use[p] == '\r')) p++;
                if (p + 1 < use_len && use[p] == '/' && use[p + 1] == '*') {
                    size_t end = p + 2;
                    while (end + 1 < use_len && !(use[end] == '*' && use[end + 1] == '/')) end++;
                    insert_pos = (end + 1 < use_len) ? end + 2 : use_len;
                    if (insert_pos < use_len && use[insert_pos] == '\n') insert_pos++;
                    continue;
                }
                if (p + 9 <= line_end && memcmp(use + p, "#include ", 9) == 0) {
                    insert_pos = (line_end < use_len) ? line_end + 1 : line_end;
                    continue;
                }
                /* Keep parser helper prototypes below leading preprocessor
                   directives too, so user typedef/struct blocks that follow
                   `#define`s can come before any helper signatures that mention
                   their types. */
                if (p < line_end && use[p] == '#') {
                    insert_pos = (line_end < use_len) ? line_end + 1 : line_end;
                    continue;
                }
                if (p == line_end || (p + 1 < line_end && use[p] == '/' && use[p + 1] == '/')) {
                    insert_pos = (line_end < use_len) ? line_end + 1 : line_end;
                    continue;
                }
                if ((p + 7 <= line_end && memcmp(use + p, "typedef", 7) == 0 && !cc_is_ident_char(use[p + 7])) ||
                    (p + 6 <= line_end && memcmp(use + p, "struct", 6) == 0 && !cc_is_ident_char(use[p + 6])) ||
                    (p + 5 <= line_end && memcmp(use + p, "union", 5) == 0 && !cc_is_ident_char(use[p + 5])) ||
                    (p + 4 <= line_end && memcmp(use + p, "enum", 4) == 0 && !cc_is_ident_char(use[p + 4]))) {
                    size_t q = p;
                    int brace_depth = 0;
                    int in_str = 0, in_chr = 0, in_lc = 0, in_bc = 0;
                    for (; q < use_len; q++) {
                        char c = use[q];
                        char c2 = (q + 1 < use_len) ? use[q + 1] : 0;
                        if (in_lc) {
                            if (c == '\n') in_lc = 0;
                            continue;
                        }
                        if (in_bc) {
                            if (c == '*' && c2 == '/') { in_bc = 0; q++; }
                            continue;
                        }
                        if (in_str) {
                            if (c == '\\' && c2) { q++; continue; }
                            if (c == '"') in_str = 0;
                            continue;
                        }
                        if (in_chr) {
                            if (c == '\\' && c2) { q++; continue; }
                            if (c == '\'') in_chr = 0;
                            continue;
                        }
                        if (c == '/' && c2 == '/') { in_lc = 1; q++; continue; }
                        if (c == '/' && c2 == '*') { in_bc = 1; q++; continue; }
                        if (c == '"') { in_str = 1; continue; }
                        if (c == '\'') { in_chr = 1; continue; }
                        if (c == '{') { brace_depth++; continue; }
                        if (c == '}') { if (brace_depth > 0) brace_depth--; continue; }
                        if (c == ';' && brace_depth == 0) {
                            q++;
                            if (q < use_len && use[q] == '\n') q++;
                            insert_pos = q;
                            break;
                        }
                    }
                    if (q >= use_len) insert_pos = use_len;
                    continue;
                }
                break;
            }
            fwrite(use, 1, insert_pos, out);
            {
                /* At `insert_pos` the TU's leading `#include` / `typedef`
                 * prelude has been emitted, so user-defined payload/error
                 * types (plus stdlib types pulled in via prelude) are in
                 * scope.  This is where we synthesize the *real* typed
                 * `CC_DECL_RESULT_SPEC(CCResult_X_Y, X, Y)` expansions for
                 * every spec the TU mentions that isn't already declared
                 * by a header — see the "parser-mode result-type collapse"
                 * follow-up in docs/known-bugs/redis_idiomatic_async.md.
                 *
                 * The `CCResult_%s_%s_DEFINED` header guard matches the
                 * one cc_result.cch / cc_io_error.cch stamp on their own
                 * pre-declared specs, so duplicate-definition is avoided
                 * when both a header and the user TU request the same
                 * Result type.
                 *
                 * We emit this block unconditionally whenever we entered
                 * the outer branch (Result syntax *or* `!>`/`?>` unwrap
                 * use), so that the `_Generic` arms below pick up stdlib-
                 * predeclared Result types (`CCResult_bool_CCIoError`
                 * returned by channel sends, etc.) even when the source
                 * never spells the typed `T!>(E)` name. */
                fprintf(out, "/* --- CC result type declarations (typed, post-prelude) --- */\n");
                for (size_t i = 0; i < cc__result_specs.count; i++) {
                    const CCResultSpec* spec = cc_result_spec_table_get(&cc__result_specs, i);
                    const char* ok_m = spec ? spec->mangled_ok : NULL;
                    const char* err_m = spec ? spec->mangled_err : NULL;
                    const char* ok_ty = spec ? spec->ok_type : NULL;
                    const char* err_ty = spec ? spec->err_type : NULL;
                    if (!ok_m || !err_m || !ok_ty || !err_ty) continue;
                    /* Stdlib-predeclared specs (`CCResult_CCDirIterptr_CCIoError`
                     * etc.) have their `CC_DECL_RESULT_SPEC` expansion baked
                     * into the owning `.cch` header (e.g. `dir.cch`), with no
                     * include-guard around it.  Re-emitting the spec here
                     * triggers "struct/union/enum already defined" at the
                     * duplicate typedef.  visit_codegen.c's post-prelude
                     * emission uses the same `is_stdlib_predeclared_name`
                     * check to skip them — mirror it here. */
                    {
                        char concrete[256];
                        cc_result_spec_format_name(ok_m, err_m, concrete, sizeof(concrete));
                        if (cc_result_spec_is_stdlib_predeclared_name(concrete)) continue;
                    }
                    int ok_is_void = (strcmp(ok_ty, "void") == 0);
                    fprintf(out, "#ifndef CCResult_%s_%s_DEFINED\n", ok_m, err_m);
                    fprintf(out, "#define CCResult_%s_%s_DEFINED 1\n", ok_m, err_m);
                    if (ok_is_void) {
                        /* Void result: union carries only the error field. */
                        fprintf(out,
                            "typedef struct CCResult_%s_%s {\n"
                            "    bool ok;\n"
                            "    union { char _dummy; %s error; } u;\n"
                            "} CCResult_%s_%s;\n",
                            ok_m, err_m, err_ty, ok_m, err_m);
                    } else {
                        fprintf(out, "CC_DECL_RESULT_SPEC(CCResult_%s_%s, %s, %s)\n",
                                ok_m, err_m, ok_ty, err_ty);
                    }
                    fprintf(out, "#endif\n");
                }
                /* Legacy parser-helper prototypes below remained useful for
                 * downstream passes that look for the `__cc_parser_result_*`
                 * symbols as markers of "this TU uses result types".  With
                 * the typed structs now in place these prototypes are
                 * linker-only (never called at runtime) and the stdlib-level
                 * inline helpers emitted by `CC_DECL_RESULT_SPEC` carry all
                 * real semantics. */
                for (size_t i = 0; i < cc__result_specs.count; i++) {
                    const CCResultSpec* spec = cc_result_spec_table_get(&cc__result_specs, i);
                    const char* ok = spec ? spec->mangled_ok : NULL;
                    const char* err = spec ? spec->mangled_err : NULL;
                    if (!ok || !err) continue;
                    int ok_is_void = (strcmp(spec->ok_type, "void") == 0);
                    fprintf(out, "bool __cc_parser_result_is_ok_CCResult_%s_%s(CCResult_%s_%s r);\n",
                            ok, err, ok, err);
                    fprintf(out, "bool __cc_parser_result_is_err_CCResult_%s_%s(CCResult_%s_%s r);\n",
                            ok, err, ok, err);
                    if (!ok_is_void) {
                        fprintf(out, "%s __cc_parser_result_unwrap_CCResult_%s_%s(CCResult_%s_%s r);\n",
                                spec->ok_type, ok, err, ok, err);
                    }
                    fprintf(out, "%s __cc_parser_result_error_CCResult_%s_%s(CCResult_%s_%s r);\n",
                            spec->err_type, ok, err, ok, err);
                    if (!ok_is_void) {
                        fprintf(out, "%s __cc_parser_result_unwrap_or_CCResult_%s_%s(CCResult_%s_%s r, %s def);\n",
                                spec->ok_type, ok, err, ok, err, spec->ok_type);
                    }
                }
                /* Parser-mode enumerated `_Generic` arms for the unified
                 * unwrap primitives.  Without these, `__cc_uw_value(r)`
                 * falls through to the `default: (__x__)` arm defined in
                 * cc_result.cch and returns the whole Result struct, so
                 * a `?>(e) handle(e)` ternary whose handler returns `T`
                 * fails TCC's conditional type-check with
                 *   "have 'struct CCResult_T_E' and 'struct T'".
                 * visit_codegen.c emits the same enumeration for the
                 * real compile path; we mirror it here so the initial
                 * parser-mode parse type-checks too.  Every CCResult_T_E
                 * struct shares layout `{ bool ok; union { T value; E
                 * error; } u; }`, so the casts pick out the right field
                 * regardless of T / E. */
                /* Forward-declare the stdlib-predeclared Result struct tags
                 * so `_Generic` can reference them by type name even when
                 * the TU doesn't `#include` the owning header.  `_Generic`
                 * arm selectors only require a declared type, not a
                 * complete one — if an arm's body is never evaluated
                 * (because the controlling expression has a different
                 * type) the incomplete struct is never accessed.  In TUs
                 * that *do* include e.g. `ccc/std/io.cch`, the header's
                 * `CC_DECL_RESULT_SPEC` expansion supplies the full
                 * definition and the typedef here is benignly repeated. */
                fprintf(out, "/* Forward-declare stdlib-predeclared Result tags. */\n");
                /* Also forward-declare `__CCResultGeneric` so TUs that never
                 * include `ccc/cc_result.cch` (e.g. C-style smoke tests that
                 * only use raw-pointer `!>` / `?>`) still compile — TCC-ext's
                 * UFCS stub emits `__CCResultGeneric` as the return type for
                 * channel-method calls, and `_Generic` arm selectors need the
                 * tag declared even when no such arm matches. */
                fprintf(out,
                    "#ifndef __CC_RESULT_GENERIC_FWD_DECLARED\n"
                    "#define __CC_RESULT_GENERIC_FWD_DECLARED 1\n"
                    "typedef struct __CCResultGeneric __CCResultGeneric;\n"
                    "#endif\n");
                for (int si = 0; ; si++) {
                    const CCStdlibPredeclaredResult* p = cc_result_spec_lookup_stdlib_predeclared_by_index(si);
                    if (!p) break;
                    if (!p->concrete_name) continue;
                    fprintf(out,
                        "#ifndef %s_FWD_DECLARED\n"
                        "#define %s_FWD_DECLARED 1\n"
                        "typedef struct %s %s;\n"
                        "#endif\n",
                        p->concrete_name, p->concrete_name,
                        p->concrete_name, p->concrete_name);
                }

                /* Helper: has this (ok_m, err_m) pair already been emitted as
                 * a `_Generic` arm?  Used to avoid duplicate arms when a
                 * user-defined spec happens to match a stdlib-predeclared
                 * one (harmless for correctness but breaks `_Generic` arm
                 * uniqueness). */
                #define CC_UW_ARM_EMIT_CHECK(tracker, key)                       \
                    ({ int __dup = 0;                                            \
                       for (size_t __di = 0; __di < (tracker)->count; __di++) {  \
                           if (strcmp((tracker)->names[__di], (key)) == 0) {     \
                               __dup = 1; break;                                 \
                           }                                                     \
                       }                                                         \
                       if (!__dup && (tracker)->count < 256) {                   \
                           snprintf((tracker)->names[(tracker)->count],          \
                                    sizeof((tracker)->names[0]), "%s", (key));   \
                           (tracker)->count++;                                   \
                       }                                                         \
                       __dup; })
                struct { char names[256][192]; size_t count; } seen;
                seen.count = 0;

                fprintf(out,
                    "#undef __cc_uw_is_err\n"
                    "#define __cc_uw_is_err(__x__) _Generic((__x__), \\\n"
                    /* TCC-ext's UFCS stub synthesises calls like
                     * `c->tx.send(p)` as returning `__CCResultGeneric`
                     * during the initial parser-mode parse (see
                     * `cc_ufcs_needs_result_generic_stub` in
                     * third_party/tcc/tccgen.c).  The stub shares the
                     * `{ bool ok; union { intptr_t value; __CCGenericError
                     * error; } u; }` layout of every typed CCResult_T_E, so
                     * we dispatch through the same cast-and-probe trick as
                     * the typed arms.  The arm is harmless in the real
                     * compile pass because UFCS rewriting replaces the
                     * stubbed call with a typed cc_channel_send / recv
                     * invocation whose return type hits a typed arm. */
                    "    __CCResultGeneric: (!((__CCResultGeneric*)(void*)&(__x__))->ok), \\\n");
                (void)CC_UW_ARM_EMIT_CHECK(&seen, "__CCResultGeneric");
                for (size_t i = 0; i < cc__result_specs.count; i++) {
                    const CCResultSpec* spec = cc_result_spec_table_get(&cc__result_specs, i);
                    if (!spec || !spec->mangled_ok || !spec->mangled_err) continue;
                    char key[192];
                    snprintf(key, sizeof(key), "CCResult_%s_%s", spec->mangled_ok, spec->mangled_err);
                    if (CC_UW_ARM_EMIT_CHECK(&seen, key)) continue;
                    fprintf(out,
                        "    %s: (!((%s*)(void*)&(__x__))->ok), \\\n",
                        key, key);
                }
                /* Always include arms for stdlib-predeclared Result types
                 * (e.g. `CCResult_bool_CCIoError` for channel sends,
                 * `CCResult_size_t_CCIoError` for file reads).  Without
                 * these, TUs that use `!>`/`?>` on a channel-send result
                 * but never spell a Result type in source fall through to
                 * the `default:` pointer-null check and TCC rejects the
                 * `== (void*)0` on a non-pointer first field. */
                for (int si = 0; ; si++) {
                    const CCStdlibPredeclaredResult* p = cc_result_spec_lookup_stdlib_predeclared_by_index(si);
                    if (!p) break;
                    if (!p->concrete_name) continue;
                    if (CC_UW_ARM_EMIT_CHECK(&seen, p->concrete_name)) continue;
                    fprintf(out,
                        "    %s: (!((%s*)(void*)&(__x__))->ok), \\\n",
                        p->concrete_name, p->concrete_name);
                }
                fprintf(out,
                    "    default: (*(void* const*)(void*)&(__x__) == (void*)0))\n");

                seen.count = 0;
                fprintf(out,
                    "#undef __cc_uw_value\n"
                    "#define __cc_uw_value(__x__) _Generic((__x__), \\\n"
                    /* See note above `__cc_uw_is_err`.  Returning
                     * `.u.value` (intptr_t) is safe because any consumer
                     * whose T differs will be rewritten by the real
                     * compile pass before TCC type-checks it. */
                    "    __CCResultGeneric: ((__CCResultGeneric*)(void*)&(__x__))->u.value, \\\n");
                (void)CC_UW_ARM_EMIT_CHECK(&seen, "__CCResultGeneric");
                for (size_t i = 0; i < cc__result_specs.count; i++) {
                    const CCResultSpec* spec = cc_result_spec_table_get(&cc__result_specs, i);
                    if (!spec || !spec->mangled_ok || !spec->mangled_err) continue;
                    /* Void-result types have no `.u.value` field; skip their
                     * `__cc_uw_value` arm.  The `?>`/`!>` rewriter never
                     * reads the value on a void result, so this arm being
                     * absent is fine — `default:` still covers raw pointers
                     * and typed non-Result LHSs. */
                    if (spec->ok_type && strcmp(spec->ok_type, "void") == 0) continue;
                    char key[192];
                    snprintf(key, sizeof(key), "CCResult_%s_%s", spec->mangled_ok, spec->mangled_err);
                    if (CC_UW_ARM_EMIT_CHECK(&seen, key)) continue;
                    fprintf(out,
                        "    %s: ((%s*)(void*)&(__x__))->u.value, \\\n",
                        key, key);
                }
                for (int si = 0; ; si++) {
                    const CCStdlibPredeclaredResult* p = cc_result_spec_lookup_stdlib_predeclared_by_index(si);
                    if (!p) break;
                    if (!p->concrete_name || !p->ok_type) continue;
                    if (strcmp(p->ok_type, "void") == 0) continue;
                    if (CC_UW_ARM_EMIT_CHECK(&seen, p->concrete_name)) continue;
                    fprintf(out,
                        "    %s: ((%s*)(void*)&(__x__))->u.value, \\\n",
                        p->concrete_name, p->concrete_name);
                }
                fprintf(out, "    default: (__x__))\n");

                seen.count = 0;
                fprintf(out,
                    "#undef __cc_uw_err_at\n"
                    "#define __cc_uw_err_at(__x__, __e__, __f__, __l__) _Generic((__x__), \\\n"
                    /* See note above `__cc_uw_is_err`.  The generic stub's
                     * `.u.error` is `__CCGenericError` which is layout-
                     * compatible with `CCError`, so the binder picks up
                     * usable fields during parser-mode typecheck. */
                    "    __CCResultGeneric: ((__CCResultGeneric*)(void*)&(__x__))->u.error, \\\n");
                (void)CC_UW_ARM_EMIT_CHECK(&seen, "__CCResultGeneric");
                for (size_t i = 0; i < cc__result_specs.count; i++) {
                    const CCResultSpec* spec = cc_result_spec_table_get(&cc__result_specs, i);
                    if (!spec || !spec->mangled_ok || !spec->mangled_err) continue;
                    char key[192];
                    snprintf(key, sizeof(key), "CCResult_%s_%s", spec->mangled_ok, spec->mangled_err);
                    if (CC_UW_ARM_EMIT_CHECK(&seen, key)) continue;
                    fprintf(out,
                        "    %s: ((%s*)(void*)&(__x__))->u.error, \\\n",
                        key, key);
                }
                for (int si = 0; ; si++) {
                    const CCStdlibPredeclaredResult* p = cc_result_spec_lookup_stdlib_predeclared_by_index(si);
                    if (!p) break;
                    if (!p->concrete_name) continue;
                    if (CC_UW_ARM_EMIT_CHECK(&seen, p->concrete_name)) continue;
                    fprintf(out,
                        "    %s: ((%s*)(void*)&(__x__))->u.error, \\\n",
                        p->concrete_name, p->concrete_name);
                }
                fprintf(out,
                    "    default: __cc_err_null_at(__e__, __f__, __l__))\n");
                #undef CC_UW_ARM_EMIT_CHECK

                fprintf(out, "/* --- end result type declarations --- */\n");
                {
                    int resume_line = 1;
                    for (size_t i = 0; i < insert_pos; i++) {
                        if (use[i] == '\n') resume_line++;
                    }
                    fprintf(out, "#line %d \"%s\"\n", resume_line, cc_path_rel_to_repo(input_path ? input_path : "<string>", rel, sizeof(rel)));
                }
            }
            fputs(use + insert_pos, out);
        }
        free(lowered_system_use);
    }
    fclose(out);

    cc_pass_chain_free(&chain);
    free(buf);
    return out_buf;

chain_cleanup:
    /* Error path - cleanup allocations and return failure */
    cc_pass_chain_free(&chain);
    free(buf);
    return NULL;
}

// Wrapper that runs all checks (default behavior for initial parse).
char* cc_preprocess_to_string(const char* input, size_t input_len, const char* input_path) {
    return cc_preprocess_to_string_ex(input, input_len, input_path, 0);
}

typedef struct {
    char* source_path;
    char* lowered_path;
} CCLoweredLocalHeader;

static CCLoweredLocalHeader* g_lowered_local_headers = NULL;
static size_t g_lowered_local_header_count = 0;
static size_t g_lowered_local_header_cap = 0;

static int cc__ensure_lowered_local_header_capacity(size_t needed) {
    if (g_lowered_local_header_cap >= needed) return 0;
    size_t new_cap = g_lowered_local_header_cap ? g_lowered_local_header_cap * 2 : 8;
    CCLoweredLocalHeader* nv;
    while (new_cap < needed) new_cap *= 2;
    nv = (CCLoweredLocalHeader*)realloc(g_lowered_local_headers, new_cap * sizeof(*nv));
    if (!nv) return -1;
    g_lowered_local_headers = nv;
    g_lowered_local_header_cap = new_cap;
    return 0;
}

static int cc__mkpath_local(const char* path) {
    char* p = NULL;
    char* sep = NULL;
    if (!path || !path[0]) return -1;
    p = strdup(path);
    if (!p) return -1;
    sep = p;
    while ((sep = strchr(sep + 1, '/')) != NULL) {
        *sep = '\0';
        if (mkdir(p, 0755) < 0 && errno != EEXIST) {
            free(p);
            return -1;
        }
        *sep = '/';
    }
    if (mkdir(p, 0755) < 0 && errno != EEXIST) {
        free(p);
        return -1;
    }
    free(p);
    return 0;
}

static int cc__dirname_local(const char* path, char* out, size_t out_sz) {
    const char* slash = NULL;
    size_t len = 0;
    if (!path || !out || out_sz == 0) return -1;
    slash = strrchr(path, '/');
    if (!slash) {
        if (out_sz < 2) return -1;
        strcpy(out, ".");
        return 0;
    }
    len = (size_t)(slash - path);
    if (len == 0) len = 1;
    if (len + 1 > out_sz) return -1;
    memcpy(out, path, len);
    out[len] = '\0';
    return 0;
}

static int cc__read_file_text(const char* path, char** out_buf, size_t* out_len) {
    FILE* f = NULL;
    long flen = 0;
    char* buf = NULL;
    size_t got = 0;
    if (!path || !out_buf || !out_len) return -1;
    *out_buf = NULL;
    *out_len = 0;
    f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen < 0) { fclose(f); return -1; }
    buf = (char*)malloc((size_t)flen + 1);
    if (!buf) { fclose(f); return -1; }
    got = fread(buf, 1, (size_t)flen, f);
    fclose(f);
    buf[got] = '\0';
    *out_buf = buf;
    *out_len = got;
    return 0;
}

static int cc__write_file_text(const char* path, const char* buf, size_t len) {
    FILE* f = NULL;
    if (!path || !buf) return -1;
    f = fopen(path, "w");
    if (!f) return -1;
    fwrite(buf, 1, len, f);
    fclose(f);
    return 0;
}

static int cc__build_stable_lowered_header_path(const char* abs_src,
                                                char* out_path,
                                                size_t out_path_sz) {
    char repo_root[PATH_MAX];
    size_t repo_len;
    const char* rel = NULL;
    size_t rel_len;
    if (!abs_src || !out_path || out_path_sz == 0) return -1;
    repo_root[0] = '\0';
    if (!cc_path_find_repo_root(abs_src, repo_root, sizeof(repo_root))) return -1;
    repo_len = strlen(repo_root);
    if (strncmp(abs_src, repo_root, repo_len) != 0) return -1;
    rel = abs_src + repo_len;
    if (*rel == '/') rel++;
    if (!*rel) return -1;
    rel_len = strlen(rel);
    if (snprintf(out_path, out_path_sz, "%s/out/include/%s", repo_root, rel) >= (int)out_path_sz) {
        return -1;
    }
    if (rel_len < 4 || strcmp(rel + rel_len - 4, ".cch") != 0) return -1;
    strcpy(out_path + strlen(out_path) - 4, ".h");
    return 0;
}

static int cc__match_local_include_line(const char* line,
                                        size_t len,
                                        size_t* out_path_s,
                                        size_t* out_path_e) {
    size_t p = 0;
    if (!line || !out_path_s || !out_path_e) return 0;
    while (p < len && (line[p] == ' ' || line[p] == '\t')) p++;
    if (p >= len || line[p] != '#') return 0;
    p++;
    while (p < len && (line[p] == ' ' || line[p] == '\t')) p++;
    if (p + strlen("include") >= len || strncmp(line + p, "include", strlen("include")) != 0) return 0;
    p += strlen("include");
    while (p < len && (line[p] == ' ' || line[p] == '\t')) p++;
    if (p >= len || line[p] != '"') return 0;
    *out_path_s = ++p;
    while (p < len && line[p] != '"') p++;
    if (p >= len) return 0;
    *out_path_e = p;
    return (*out_path_e > *out_path_s);
}

static char* cc__rewrite_local_cch_includes_impl(const char* src, size_t n, const char* current_path);
static const char* cc__lower_local_cch_header(const char* source_path) {
    char abs_src[PATH_MAX];
    char lowered_path[PATH_MAX];
    char lowered_dir[PATH_MAX];
    char* input = NULL;
    char* rewritten = NULL;
    char* lowered = NULL;
    size_t input_len = 0;
    size_t lowered_idx;
    if (!source_path || !source_path[0]) return NULL;
    if (!realpath(source_path, abs_src)) return NULL;
    for (size_t i = 0; i < g_lowered_local_header_count; ++i) {
        if (strcmp(g_lowered_local_headers[i].source_path, abs_src) == 0 &&
            access(g_lowered_local_headers[i].lowered_path, F_OK) == 0) {
            return g_lowered_local_headers[i].lowered_path;
        }
    }
    if (cc__build_stable_lowered_header_path(abs_src, lowered_path, sizeof(lowered_path)) != 0) return NULL;
    if (cc__dirname_local(lowered_path, lowered_dir, sizeof(lowered_dir)) != 0) return NULL;
    if (cc__mkpath_local(lowered_dir) != 0) return NULL;
    if (cc__read_file_text(abs_src, &input, &input_len) != 0) return NULL;
    rewritten = cc__rewrite_local_cch_includes_impl(input, input_len, abs_src);
    lowered = cc_lower_header_string(rewritten ? rewritten : input,
                                     rewritten ? strlen(rewritten) : input_len,
                                     abs_src);
    if (!lowered) lowered = strdup(rewritten ? rewritten : input);
    if (!lowered) return NULL;
    if (cc__write_file_text(lowered_path, lowered, strlen(lowered)) != 0) return NULL;
    if (cc__ensure_lowered_local_header_capacity(g_lowered_local_header_count + 1) != 0) return NULL;
    lowered_idx = g_lowered_local_header_count++;
    memset(&g_lowered_local_headers[lowered_idx], 0, sizeof(g_lowered_local_headers[lowered_idx]));
    g_lowered_local_headers[lowered_idx].source_path = strdup(abs_src);
    g_lowered_local_headers[lowered_idx].lowered_path = strdup(lowered_path);
    if (!g_lowered_local_headers[lowered_idx].source_path || !g_lowered_local_headers[lowered_idx].lowered_path) return NULL;
    free(input);
    free(rewritten);
    free(lowered);
    return g_lowered_local_headers[lowered_idx].lowered_path;
}

static char* cc__rewrite_local_cch_includes_impl(const char* src, size_t n, const char* current_path) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    int changed = 0;
    char current_dir[PATH_MAX];
    if (!src || !current_path) return NULL;
    if (cc__dirname_local(current_path, current_dir, sizeof(current_dir)) != 0) return NULL;
    while (i < n) {
        size_t line_end = i;
        size_t path_s = 0, path_e = 0;
        while (line_end < n && src[line_end] != '\n') line_end++;
        if (cc__match_local_include_line(src + i, line_end - i, &path_s, &path_e)) {
            size_t rel_len = path_e - path_s;
            if (rel_len >= 4 && strncmp(src + i + path_e - 4, ".cch", 4) == 0) {
                char rel_path[PATH_MAX];
                char child_path[PATH_MAX];
                const char* lowered_path;
                if (rel_len >= sizeof(rel_path)) rel_len = sizeof(rel_path) - 1;
                memcpy(rel_path, src + i + path_s, rel_len);
                rel_path[rel_len] = '\0';
                snprintf(child_path, sizeof(child_path), "%s/%s", current_dir, rel_path);
                lowered_path = cc__lower_local_cch_header(child_path);
                if (lowered_path) {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "#include \"");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, lowered_path);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "\"");
                    if (line_end < n && src[line_end] == '\n') cc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
                    changed = 1;
                    i = (line_end < n) ? line_end + 1 : line_end;
                    continue;
                }
            }
        }
        cc_sb_append(&out, &out_len, &out_cap, src + i, line_end - i);
        if (line_end < n && src[line_end] == '\n') cc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
        i = (line_end < n) ? line_end + 1 : line_end;
    }
    if (!changed) {
        free(out);
        return NULL;
    }
    return out;
}

char* cc_rewrite_local_cch_includes_to_lowered_headers(const char* src,
                                                       size_t input_len,
                                                       const char* input_path) {
    if (!src || input_len == 0 || !input_path || !input_path[0]) return NULL;
    return cc__rewrite_local_cch_includes_impl(src, input_len, input_path);
}

char* cc_rewrite_system_cch_includes_to_lowered_headers(const char* src, size_t n) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    int changed = 0;
    if (!src) return NULL;
    while (i < n) {
        size_t line_end = i;
        while (line_end < n && src[line_end] != '\n') line_end++;
        {
            size_t p = i;
            while (p < line_end && (src[p] == ' ' || src[p] == '\t')) p++;
            if (p < line_end && src[p] == '#') {
                p++;
                while (p < line_end && (src[p] == ' ' || src[p] == '\t')) p++;
                if (p + strlen("include") < line_end &&
                    strncmp(src + p, "include", strlen("include")) == 0) {
                    p += strlen("include");
                    while (p < line_end && (src[p] == ' ' || src[p] == '\t')) p++;
                    if (p < line_end && src[p] == '<') {
                        size_t close = p + 1;
                        while (close < line_end && src[close] != '>') close++;
                        if (close < line_end &&
                            close >= p + 5 &&
                            strncmp(src + close - 4, ".cch", 4) == 0) {
                            size_t path_end = close - 4;
                            cc_sb_append(&out, &out_len, &out_cap, src + i, path_end - i);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ".h");
                            cc_sb_append(&out, &out_len, &out_cap, src + close, line_end - close);
                            if (line_end < n && src[line_end] == '\n') {
                                cc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
                            }
                            changed = 1;
                            i = (line_end < n) ? line_end + 1 : line_end;
                            continue;
                        }
                    }
                }
            }
        }
        cc_sb_append(&out, &out_len, &out_cap, src + i, line_end - i);
        if (line_end < n && src[line_end] == '\n') cc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
        i = (line_end < n) ? line_end + 1 : line_end;
    }
    if (!changed) {
        free(out);
        return NULL;
    }
    return out;
}

char* cc_rewrite_header_type_syntax_shared(const char* src,
                                           size_t input_len,
                                           const char* input_path) {
    CCPassChain chain;
    char* out = NULL;
    CCTypeRegistry* reg = NULL;
    if (!src || input_len == 0) return NULL;

    /* Header lowering should share the same type-syntax understanding as the
       main preprocess pipeline for syntax that must not survive into plain C
       headers. Keep this intentionally limited to header-safe rewrites. */
    reg = cc_type_registry_get_global();
    if (!reg) {
        reg = cc_type_registry_new();
        cc_type_registry_set_global(reg);
    }
    cc_type_registry_clear(reg);

    cc_pass_chain_init(&chain, src, input_len);
    if (cc_pass_chain_apply(&chain, cc__rewrite_string_templates(chain.src, chain.len, input_path)) < 0) goto chain_cleanup;
    if (cc_pass_chain_apply(&chain, cc__rewrite_chan_handle_types(chain.src, chain.len, input_path)) < 0) goto chain_cleanup;
    if (cc_pass_chain_apply(&chain, cc__rewrite_slice_types(chain.src, chain.len, input_path)) < 0) goto chain_cleanup;
    if (cc_pass_chain_apply(&chain, cc_rewrite_generic_containers(chain.src, chain.len, input_path)) < 0) goto chain_cleanup;

    if (chain.src != src) out = strdup(chain.src);

chain_cleanup:
    cc_pass_chain_free(&chain);
    return out;
}

char* cc_preprocess_include_expanded(const char* input_path) {
    char repo_root[1024];
    char cmd[4096];
    FILE* pp = NULL;
    char* buf = NULL;
    size_t len = 0;
    size_t cap = 64 * 1024;
    if (!input_path || !input_path[0]) return NULL;
    repo_root[0] = '\0';
    if (cc_path_find_repo_root(input_path, repo_root, sizeof(repo_root))) {
        snprintf(cmd, sizeof(cmd),
                 "cc -E -D__CC__=1 -DCC_COMPTIME_SCAN=1 -x c -I\"%s/cc/include\" -I\"%s/out/include\" \"%s\" 2>/dev/null",
                 repo_root, repo_root, input_path);
    } else {
        snprintf(cmd, sizeof(cmd), "cc -E -D__CC__=1 -DCC_COMPTIME_SCAN=1 -x c \"%s\" 2>/dev/null", input_path);
    }
    pp = popen(cmd, "r");
    if (!pp) return NULL;
    buf = (char*)malloc(cap);
    if (!buf) {
        pclose(pp);
        return NULL;
    }
    while (!feof(pp)) {
        size_t avail = cap - len;
        size_t nread;
        if (avail < 4096) {
            size_t new_cap = cap * 2;
            char* new_buf = (char*)realloc(buf, new_cap);
            if (!new_buf) {
                free(buf);
                pclose(pp);
                return NULL;
            }
            buf = new_buf;
            cap = new_cap;
            avail = cap - len;
        }
        nread = fread(buf + len, 1, avail - 1, pp);
        len += nread;
        if (ferror(pp)) {
            free(buf);
            pclose(pp);
            return NULL;
        }
    }
    buf[len] = '\0';
    pclose(pp);
    return buf;
}

static int cc__apply_phase1_canonical_passes(CCPassChain* chain,
                                             const char* input_path) {
    if (!chain) return -1;
    /* Shared phase-1 bucket: normalize CC surface syntax into more canonical
       CC, but do not introduce parser stubs or host-C survival/lowering. */
    if (cc_pass_chain_apply(chain, cc__canonicalize_with_deadline_syntax(chain->src, chain->len)) < 0) return -1;
    if (cc_pass_chain_apply(chain, cc__rewrite_string_templates(chain->src, chain->len, input_path)) < 0) return -1;
    if (cc_pass_chain_apply(chain, cc__rewrite_chan_handle_types(chain->src, chain->len, input_path)) < 0) return -1;
    if (cc_pass_chain_apply(chain, cc__rewrite_slice_types(chain->src, chain->len, input_path)) < 0) return -1;
    if (cc_pass_chain_apply(chain, cc_rewrite_generic_containers(chain->src, chain->len, input_path)) < 0) return -1;
    if (cc_pass_chain_apply(chain, cc__rewrite_optional_types(chain->src, chain->len, input_path)) < 0) return -1;
    if (cc_pass_chain_apply(chain, cc__rewrite_inferred_result_ctors(chain->src, chain->len)) < 0) return -1;
    if (cc_pass_chain_apply(chain, cc__rewrite_result_types(chain->src, chain->len, input_path)) < 0) return -1;
    /* Rewrite `@async void fn(...)` -> `@async CCAsyncVoidRet fn(...)` so
     * that phase-3 reparse sees a task-returning signature (required for
     * spawn-site lowerings such as `n->spawn_async(fn(args))` to type-check).
     * async_ast recognises CCAsyncVoidRet as an originally-void declaration. */
    if (cc_pass_chain_apply(chain, cc__rewrite_async_void_ret(chain->src, chain->len)) < 0) return -1;
    /* Call-site `@blocking f(...)` / `@noblock f(...)` annotations
     * (spec §8.2.2 rule 1).  Runs before @await so an `@await`-wrapping
     * is not accidentally treated as a call-site mode host. */
    if (cc_pass_chain_apply(chain, cc__rewrite_at_call_site_mode(chain->src, chain->len)) < 0) return -1;
    /* @await fname(...) -> cc_block_on(ReturnType, fname(...)).  Runs after
     * result-type rewriting so the return types are already in canonical form. */
    if (cc_pass_chain_apply(chain, cc__rewrite_at_await(chain->src, chain->len)) < 0) return -1;
    if (cc_pass_chain_apply(chain, cc__normalize_if_try_syntax(chain->src, chain->len)) < 0) return -1;
    if (cc_pass_chain_apply(chain, cc__rewrite_try_binding(chain->src, chain->len)) < 0) return -1;
    return 0;
}

static int cc__apply_phase3_host_lowering_passes(CCPassChain* chain,
                                                 const char* input_path) {
    if (!chain) return -1;
    /* Shared phase-3 bucket: parser/host-C survival and lowering after
       canonical CC has been established and phase 2 comptime effects are
       conceptually available. */
    /* Phase-1 result-unwrap operators `?>` (expression) and `!>`
     * (statement) run before the legacy err-syntax rewrite. The `!>`
     * statement form lowers to the existing `@err` shorthands which the
     * legacy pass then processes, so order matters here. */
    {
        /* Invoke pass_result_unwrap whenever the source still contains `?>`
         * or `!>` operator sigils, OR whenever strict-mode is enabled (in
         * which case the final unhandled-result scan must run even if the
         * operators are absent). */
        const char* strict_env = getenv("CC_STRICT_RESULT_UNWRAP");
        int strict_on = (strict_env && strict_env[0] == '1' && strict_env[1] == 0);
        int has_ops = chain->src && chain->len > 0 &&
            (cc_contains_token_top_level(chain->src, chain->len, "?>") ||
             cc_contains_token_top_level(chain->src, chain->len, "!>"));
        if ((has_ops || strict_on) && chain->src && chain->len > 0) {
            /* Lift any trailing `@destroy { body }` suffix off of `!>` /
             * `?>` statements into a standalone `@defer { body };` placed
             * after the statement.  This runs BEFORE pass_result_unwrap
             * so the downstream unwrap parser (and the early TCC AST
             * probe that happens later in this phase) sees the canonical
             * form.  `@create(...) @destroy { ... };` is untouched
             * because its statement has no `!>` / `?>` operator.
             *
             * The same rewrite is also invoked in visit_codegen.c right
             * before the final `!>`/`?>` lowering, because visit_codegen
             * re-reads the raw source for its own text-pass pipeline. */
            if (cc_contains_token_top_level(chain->src, chain->len, "@destroy")) {
                char* ud_out = NULL;
                size_t ud_out_len = 0;
                int ud_r = cc__rewrite_unwrap_destroy_suffix(
                    chain->src, chain->len, input_path, &ud_out, &ud_out_len);
                (void)ud_out_len;
                if (ud_r < 0) return -1;
                if (ud_r > 0 && ud_out && cc_pass_chain_apply(chain, ud_out) < 0) return -1;
            }
            /* `!>` / `?>` lowering no longer needs a pointer-fn registry:
             * the Result-vs-pointer dispatch is handled at emission time
             * by the `__cc_uw_*` `_Generic` macros in `cc_result.cch`
             * (baseline arms) and `visit_codegen.c` (per-spec enumerated
             * arms).  The pre-lowering scan that used to populate a
             * pointer-fn registry has been removed — the lowering now
             * emits the same unified shape for every LHS and lets the C
             * type system pick the right arm at preprocessor expansion
             * time. */
            char* ru_out = NULL;
            size_t ru_out_len = 0;
            CCVisitorCtx ru_ctx = {.symbols = NULL, .input_path = input_path};
            int ru_r = cc__rewrite_result_unwrap(&ru_ctx, chain->src, chain->len, &ru_out, &ru_out_len);
            (void)ru_out_len;
            if (ru_r < 0) return -1;
            if (ru_r > 0 && ru_out && cc_pass_chain_apply(chain, ru_out) < 0) return -1;
        }
    }
    if (chain->src && chain->len > 0 &&
        (cc_contains_token_top_level(chain->src, chain->len, "@errhandler") ||
         cc_contains_token_top_level(chain->src, chain->len, "@err") ||
         cc_contains_token_top_level(chain->src, chain->len, "=<!") ||
         cc_contains_token_top_level(chain->src, chain->len, "<?"))) {
        char* err_out = NULL;
        size_t err_out_len = 0;
        CCVisitorCtx err_ctx = {.symbols = NULL, .input_path = input_path};
        int err_r = cc__rewrite_err_syntax(&err_ctx, chain->src, chain->len, &err_out, &err_out_len);
        (void)err_out_len;
        if (err_r < 0) return -1;
        if (err_r > 0 && err_out && cc_pass_chain_apply(chain, err_out) < 0) return -1;
    }
    if (cc_pass_chain_apply(chain, cc__lower_with_deadline_syntax(chain->src, chain->len)) < 0) return -1;
    if (cc_pass_chain_apply(chain, cc__rewrite_match_syntax(chain->src, chain->len, input_path)) < 0) return -1;
    /* (retired) cc__rewrite_optional_constructors used to run here. */
    cc__seed_ufcs_receiver_types(chain->src, chain->len);
    /* (retired) cc__rewrite_result_constructors used to rewrite typed
       cc_ok_CCResult_T_E(v) calls to the generic __CC_RESULT_OK(0, 0, v)
       placeholder when every Result aliased __CCResultGeneric in parser
       mode.  With real typed structs, the typed ctor calls parse and
       type-check as-is, so the rewrite is both redundant and harmful
       (it forced a __CCResultGeneric return into a typed Result slot). */
    if (cc_pass_chain_apply(chain, cc_rewrite_generic_family_ufcs_parser_safe(chain->src, chain->len)) < 0) return -1;
    if (cc_pass_chain_apply(chain, cc__rewrite_try_exprs(chain->src, chain->len)) < 0) return -1;
    if (cc_pass_chain_apply(chain, cc__rewrite_optional_unwrap(chain->src, chain->len)) < 0) return -1;
    if (cc_pass_chain_apply(chain, cc__rewrite_cc_concurrent(chain->src, chain->len)) < 0) return -1;
    if (cc_pass_chain_apply(chain, cc__rewrite_link_directives(chain->src, chain->len)) < 0) return -1;
    return 0;
}

static char* cc__canonicalize_cc_for_comptime(const char* input,
                                              size_t input_len,
                                              const char* input_path) {
    CCPassChain chain;
    char* out = NULL;
    CCTypeRegistry* reg = NULL;
    if (!input || input_len == 0) return NULL;

    /* Phase 1 should normalize CC surface syntax into a more canonical CC
       program, but stop short of parser stubs or host-C survival lowering.
       Keep this deliberately conservative while comptime execution still grows:
       normalize type/syntax sugar, not final C-facing mechanics. */
    cc_result_spec_table_reset(&cc__result_specs);
    cc_result_spec_table_set_global(&cc__result_specs);
    reg = cc_type_registry_get_global();
    if (!reg) {
        reg = cc_type_registry_new();
        cc_type_registry_set_global(reg);
    }
    cc_type_registry_clear(reg);

    cc_pass_chain_init(&chain, input, input_len);
    if (cc_pass_chain_apply(&chain, cc__rewrite_link_directives(chain.src, chain.len)) < 0) goto cleanup;
    if (cc__apply_phase1_canonical_passes(&chain, input_path) != 0) goto cleanup;

    out = strdup(chain.src);
cleanup:
    cc_pass_chain_free(&chain);
    return out;
}

char* cc_preprocess_comptime_source(const char* input_path) {
    char* expanded = NULL;
    char* canonical = NULL;
    if (!input_path || !input_path[0]) return NULL;
    /* Phase 1: canonicalize source for comptime.
       Current stage starts from the include-expanded CC source stream, then
       applies a conservative set of CC-to-CC normalization passes. Keep this
       behind a dedicated helper so phase 2 (execute/evaluate comptime) is
       decoupled from the eventual choice of canonical CC representation. */
    expanded = cc_preprocess_include_expanded(input_path);
    if (!expanded) return NULL;
    canonical = cc__canonicalize_cc_for_comptime(expanded, strlen(expanded), input_path);
    free(expanded);
    return canonical;
}

/* Inline parse-time stubs - minimal definitions for TCC to accept CC types.
   Uses guard macros so real CC headers can skip their own definitions. */
static const char* cc__parse_stubs = 
    "#ifndef CC_PARSE_STUBS_INLINE\n"
    "#define CC_PARSE_STUBS_INLINE 1\n"
    "#define CC_PARSER_MODE 1\n"
    /* Guard macros to prevent redefinition when real headers are included */
    "#define __CC_OPTIONAL_GENERIC_VALUE_DEFINED 1\n"
    "#define __CC_OPTIONAL_GENERIC_DEFINED 1\n"
    "#define __CC_GENERIC_ERROR_DEFINED 1\n"
    "#define __CC_RESULT_GENERIC_DEFINED 1\n"
    /* Struct definitions compatible with real headers' field layout.
       Use unsigned long instead of size_t since stddef.h may not be included yet. */
    "typedef struct __CCOptionalGenericValue { long x,y,z,w; void* ptr; void* data; unsigned long len; int id,kind,code; long _scalar; } __CCOptionalGenericValue;\n"
    "typedef struct __CCOptionalGeneric { int has; union { __CCOptionalGenericValue value; long _scalar; } u; } __CCOptionalGeneric;\n"
    "typedef struct __CCGenericError { int kind; int os_errno; int os_code; const char* message; } __CCGenericError;\n"
    "typedef struct __CCResultGeneric { int ok; union { long value; __CCGenericError error; } u; } __CCResultGeneric;\n"
    /* Channel handle stubs */
    "#define __CC_CHAN_TX_DEFINED 1\n"
    "#define __CC_CHAN_RX_DEFINED 1\n"
    "typedef struct CCChan CCChan;\n"
    "typedef struct { CCChan* raw; } CCChanTx;\n"
    "typedef struct { CCChan* raw; } CCChanRx;\n"
    /* Generic container stubs - define guards so real headers skip.
       Field counts must match real headers to allow their initializers. */
    "#define __CC_VEC_GENERIC_DEFINED 1\n"
    "#define __CC_MAP_GENERIC_DEFINED 1\n"
    "typedef struct { unsigned long len; unsigned long cap; void* arena; void* data; } __CCVecGeneric;\n"
    "typedef struct { unsigned long count; void* keys; void* vals; void* arena; } __CCMapGeneric;\n"
    "#define __CC_VEC(T) __CCVecGeneric\n"
    "#define __CC_MAP(K, V) __CCMapGeneric\n"
    "#define __CC_VEC_INIT(T, arena) ((__CCVecGeneric){0, 0, 0, 0})\n"
    "#define __CC_MAP_INIT(K, V, arena) ((__CCMapGeneric){0, 0})\n"
    /* Common Vec/Map types */
    "typedef __CCVecGeneric CCVec_int;\n"
    "typedef __CCVecGeneric CCVec_char;\n"
    "typedef __CCVecGeneric CCVec_float;\n"
    "typedef __CCMapGeneric Map_int_int;\n"
    "typedef __CCMapGeneric Map_charptr_int;\n"
    /* Key macros: rewritten T!>(E) -> __CC_RESULT(T,E).
     * (retired) __CC_OPTIONAL(T) and the `__cc_optional_*_impl` parser stubs
     * were removed along with the optional surface. */
    "#define __CC_RESULT(T, E) CCResult_##T##_##E\n"
    "/* Helper functions - extern (not inline) so TCC records the calls */\n"
    "__CCResultGeneric __cc_result_ok_impl(long v);\n"
    "__CCResultGeneric __cc_result_err_impl(int kind, const char* msg);\n"
    /* Common result types */
    "typedef __CCResultGeneric CCResult_int_CCError;\n"
    "typedef __CCResultGeneric CCResult_bool_CCError;\n"
    "typedef __CCResultGeneric CCResult_void_CCError;\n"
    "typedef __CCResultGeneric CCResult_size_t_CCError;\n"
    "typedef __CCResultGeneric CCResult_charptr_CCError;\n"
    "typedef __CCResultGeneric CCResult_voidptr_CCError;\n"
    /* CCError type */
    "#ifndef __CC_ERROR_TYPE_DEFINED\n"
    "#define __CC_ERROR_TYPE_DEFINED\n"
    "typedef struct { int kind; const char* message; } CCError;\n"
    "static inline CCError __cc_error_make(int kind, const char* msg) { CCError e = {kind, msg}; return e; }\n"
    "#define CC_ERROR(kind, msg) __cc_error_make((kind), (msg))\n"
    "#endif\n"
    /* Result constructors - variadic macros to accept any type.
       We use a comma expression to evaluate the arg (so it appears in AST) then return generic.
      the experimental AST/codegen path handles the real types later. */
    "#ifndef __CC_RESULT_CTORS_DEFINED\n"
    "#define __CC_RESULT_CTORS_DEFINED\n"
    "#define cc_ok(v) __cc_result_generic_ok()\n"
    "#define cc_err(...) __cc_result_generic_err()\n"
    "#endif\n"
    /* Accessor macros (optional-family macros retired). */
    "#define cc_is_ok(res) ((res).ok)\n"
    "#define cc_is_err(res) (!(res).ok)\n"
    "#define cc_unwrap(res) ((res).u.value)\n"
    "#define cc_unwrap_as(res, T) (*(T*)(void*)&(res).u.value)\n"
    "#define cc_unwrap_err(res) ((res).u.error)\n"
    "#define cc_unwrap_err_as(res, T) (*(T*)(void*)&(res).u.error)\n"
    "#endif\n";

// Simple preprocessing for the experimental AST/codegen path: rewrites type syntax and adds parse-time stubs.
// Type syntax (T?, T!>(E), T[~N>], Vec<T>) is rewritten to C-compatible names.
// Parse-time stubs provide placeholder definitions.
// Other CC syntax (try, await, closures, etc.) is handled by TCC hooks and AST passes.
char* cc_preprocess_simple(const char* input, size_t input_len, const char* input_path) {
    if (!input || input_len == 0) return NULL;
    
    /* Reset type collectors */
    cc_result_spec_table_reset(&cc__result_specs);
    cc_result_spec_table_set_global(&cc__result_specs);
    
    /* Create type registry for this file (or reuse existing global) */
    CCTypeRegistry* reg = cc_type_registry_get_global();
    if (!reg) {
        reg = cc_type_registry_new();
        cc_type_registry_set_global(reg);
    }
    cc_type_registry_clear(reg);
    
    /* Chain of preprocessing passes - each takes previous output */
    const char* cur = input;
    size_t cur_len = input_len;
    char* buffers[16] = {0};
    int buf_idx = 0;
    
    /* Pass 0: Rewrite @link("lib") -> marker comments for linker */
    buffers[buf_idx] = cc__rewrite_link_directives(cur, cur_len);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }

    /* Pass 1: Rewrite @slice/@string forms and eliminate raw backtick templates early. */
    buffers[buf_idx] = cc__rewrite_string_templates(cur, cur_len, input_path);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }

    /* Pass 2: Rewrite channel handle types T[~N >] -> CCChanTx, T[~N <] -> CCChanRx */
    buffers[buf_idx] = cc__rewrite_chan_handle_types(cur, cur_len, input_path);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }

    /* Pass 3: Rewrite slice types T[:]/T[n:]/T[:k]/T[:k!] -> CCSlice */
    buffers[buf_idx] = cc__rewrite_slice_types(cur, cur_len, input_path);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }

    /* Pass 4: Rewrite Vec<T>/Map<K,V> into parser-safe container forms early,
       so parser-survival UFCS can resolve field receiver types correctly. */
    buffers[buf_idx] = cc_rewrite_generic_containers(cur, cur_len, input_path);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Pass 4: Diagnose any lingering `T?` — optional types are retired. */
    buffers[buf_idx] = cc__rewrite_optional_types(cur, cur_len, input_path);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Pass 5: Infer result constructor types from enclosing function:
       cc_ok(x) -> cc_ok_CCResult_T_E(x) when inside a function returning T!E
       This must run BEFORE type rewrite so we can still detect T!E syntax. */
    buffers[buf_idx] = cc__rewrite_inferred_result_ctors(cur, cur_len);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Pass 5b: (retired) used to rewrite typed cc_ok_CCResult_T_E(v)
       to the generic __CC_RESULT_OK(0, 0, v) placeholder.  See the
       companion comment in cc__run_phase1_canonical_passes for why
       this pass is obsolete now that parser mode emits real typed
       result structs. */
    
    /* Pass 5c: Rewrite T!>(E) -> CCResult_T_E */
    buffers[buf_idx] = cc__rewrite_result_types(cur, cur_len, input_path);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    cc__seed_ufcs_receiver_types(cur, cur_len);
    /* Parser-only tolerance: raw closure literals can still contain already-
       concrete family UFCS before later AST passes lower those closures.
       Normalize those concrete family calls here so TCC can build the stub AST. */
    buffers[buf_idx] = cc_rewrite_generic_family_ufcs_parser_safe(cur, cur_len);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    /* Pass 6: Normalize if @try ( -> if (try  */
    buffers[buf_idx] = cc__normalize_if_try_syntax(cur, cur_len);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Pass 7: Rewrite if (try T x = expr) to expanded form */
    buffers[buf_idx] = cc__rewrite_try_binding(cur, cur_len);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }

    /* Pass 7b: Prototype explicit nursery-handle create/destroy declarations. */
    buffers[buf_idx] = cc_rewrite_nursery_create_destroy_proto(cur, cur_len, input_path);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Note: try expressions are handled natively by TCC (CC_AST_NODE_TRY) */
    
    /* Pass 8: Rewrite @defer(err) -> @defer */
    buffers[buf_idx] = cc__rewrite_defer_syntax(cur, cur_len);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Pass 9: Rewrite *opt_var and *result_var to unwrap calls */
    buffers[buf_idx] = cc__rewrite_optional_unwrap(cur, cur_len);
    if (buffers[buf_idx]) {
        cur = buffers[buf_idx];
        cur_len = strlen(cur);
        buf_idx++;
    }
    
    /* Build output with stubs and #line directive */
    char* out_buf = NULL;
    size_t out_size = 0;
    FILE* out = open_memstream(&out_buf, &out_size);
    if (!out) {
        for (int i = 0; i < buf_idx; i++) {
            if (buffers[i]) free(buffers[i]);
        }
        return NULL;
    }
    
    /* Prepend parse-time stubs (only once) */
    fputs(cc__parse_stubs, out);
    
    /* Note: User-defined Optional types (CCOptional_Point etc.) are emitted inline
       in the source at first use, so they have access to the actual type definition.
       This allows field access like p.u.value.x to work correctly. */
    
    /* Emit typedefs and constructor macros for user-defined Result types.
       In parser mode, we emit macros that cast away the value/error and call generic helpers.
       This allows type-checking to pass even though the actual types aren't fully defined. */
    for (size_t i = 0; i < cc__result_specs.count; i++) {
        const CCResultSpec* spec = cc_result_spec_table_get(&cc__result_specs, i);
        const char* ok = spec ? spec->mangled_ok : NULL;
        const char* err = spec ? spec->mangled_err : NULL;
        if (!ok || !err) continue;
        /* Emit only the parse-time alias here. Typed helper stubs are inserted
           after the source's leading includes, once payload/error types exist. */
        fprintf(out, "#ifndef CCResult_%s_%s_DEFINED\n", ok, err);
        fprintf(out, "#define CCResult_%s_%s_DEFINED 1\n", ok, err);
        fprintf(out, "typedef __CCResultGeneric CCResult_%s_%s;\n", ok, err);
        fprintf(out, "#endif\n");
    }
    
    /* Add #line directive for source mapping */
    char rel[1024];
    fprintf(out, "#line 1 \"%s\"\n", cc_path_rel_to_repo(input_path ? input_path : "<string>", rel, sizeof(rel)));
    
    /* Output processed source */
    fwrite(cur, 1, cur_len, out);
    
    fclose(out);
    
    /* Clean up intermediate buffers */
    for (int i = 0; i < buf_idx; i++) {
        if (buffers[i]) free(buffers[i]);
    }
    
    return out_buf;
}
