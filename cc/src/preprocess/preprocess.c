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
#include <stdint.h>

#include <ccc/cc_slice.h>
#include <ccc/cc_arena.h>
#include <ccc/cc_ufcs_families.h>

#include "header/lower_header.h"
#include "comptime/const_eval.h"
#include "comptime/executor.h"
#include "comptime/symbols.h"
#include "parser/symsig.h"
#include "preprocess/cpp_expand.h"
#include "preprocess/emit_plan.h"
#include "preprocess/script_entry.h"
#include "preprocess/comptime_prepare.h"
#include "preprocess/emit_limits.h"
#include "preprocess/template_scan.h"
#include "preprocess/type_registry.h"
#include "preprocess/type_graph.h"
#include "preprocess/variant_lower.h"
#include "result_spec.h"
#include "util/cache_evict.h"
#include "util/path.h"
#include "util/result_fn_registry.h"
#include "util/text.h"
#include "util/text_scan.h"
#include "visitor/ufcs.h"
#include "visitor/visitor.h"
#include "visitor/pass_channel_syntax.h"
#include "visitor/pass_type_syntax.h"
#include "visitor/pass_err_syntax.h"
#include "visitor/pass_result_unwrap.h"
#include "visitor/pass_unwrap_destroy.h"

extern long g_cc_pass_error_count;

/* Dest-trap dedup: per-TU (reset via cc_ufcs_reset_dest_trap_dedup). */
static char g_ufcs_dest_reported[64][256];
static int g_ufcs_dest_reported_n;

void cc_ufcs_reset_dest_trap_dedup(void) {
    g_ufcs_dest_reported_n = 0;
}

/* Test-only: cache CC_UFCS_BASELINE once (additive harness). */
static int __attribute__((unused)) cc__ufcs_baseline_cached(void) {
    static int checked;
    static int on;
    if (!checked) {
        on = getenv("CC_UFCS_BASELINE") != NULL;
        checked = 1;
    }
    return on;
}

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
    int in_tpl;         /* Inside a backtick template literal */
    int in_pp;          /* Inside a preprocessor directive (#define / #include / etc.) */
    int pp_continued;   /* Previous non-WS char on the current pp line was '\\' */
    int at_line_start;  /* Last non-whitespace was a newline (or BOF) */
    int line;  /* Current line (1-based), updated if track_pos is true */
    int col;   /* Current column (1-based), updated if track_pos is true */
} CCScannerState;

/* Initialize scanner state */
static void cc_scanner_init(CCScannerState* s) {
    s->in_line_comment = 0;
    s->in_block_comment = 0;
    s->in_str = 0;
    s->in_chr = 0;
    s->in_tpl = 0;
    s->in_pp = 0;
    s->pp_continued = 0;
    s->at_line_start = 1;
    s->line = 1;
    s->col = 1;
}

/* Process current character and advance past non-code (comments, strings).
 * Returns 1 if we're in a comment/string (caller should continue to next char).
 * Returns 0 if we're at actual code (caller should process the character).
 * Updates *pos to skip multi-char sequences.
 * Updates s->line and s->col to track position.
 *
 * skip_templates: when non-zero (default), well-formed `...` literals are one
 * inert region (apostrophes inside cannot open char literals). When zero,
 * backticks are left as code so callers that substitute loop vars into
 * `@emit` / `@string` bodies (`f.index` inside `${...}`), or that resolve
 * nested `@comptime if`/`for` after that substitution, can see them.
 */
static int cc_scanner_skip_non_code_ex(CCScannerState* s, const char* src,
                                       size_t n, size_t* pos,
                                       int skip_templates) {
    size_t i = *pos;
    if (i >= n) return 0;
    
    char c = src[i];
    char c2 = (i + 1 < n) ? src[i + 1] : 0;
    
    /* Track newlines for position */
    if (c == '\n') { s->line++; s->col = 1; } else { s->col++; }
    
    /* Inside line comment */
    if (s->in_line_comment) {
        if (c == '\n') {
            s->in_line_comment = 0;
            s->at_line_start = 1;
        }
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

    /* Inside an unterminated backtick template (fallback path). */
    if (s->in_tpl) {
        if (c == '\\' && i + 1 < n) {
            *pos += 2;
            s->col++;
        } else {
            if (c == '`') s->in_tpl = 0;
            (*pos)++;
        }
        return 1;
    }


    /* Inside a preprocessor directive (#define, #include, #if, ...).
     * Skip until the end of the logical line (handles backslash-newline
     * continuations). Body bytes are reported as "non-code" so text
     * passes never rewrite them. */
    if (s->in_pp) {
        if (c == '\n') {
            if (s->pp_continued) {
                /* Continuation: stay in pp mode for the next line. */
                s->pp_continued = 0;
            } else {
                s->in_pp = 0;
                s->at_line_start = 1;
            }
            (*pos)++;
            return 1;
        }
        if (c == '\\') {
            /* Look ahead past optional spaces/tabs to see if next char is
             * '\n'; if so, this is a line-continuation. */
            size_t k = i + 1;
            while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
            s->pp_continued = (k < n && src[k] == '\n') ? 1 : 0;
            (*pos)++;
            return 1;
        }
        if (c != ' ' && c != '\t') s->pp_continued = 0;
        (*pos)++;
        return 1;
    }

    /* Detect start of a preprocessor directive: `#` as first non-WS on a line.
     * Once detected, enter in_pp and let the next iteration skip the body. */
    if (s->at_line_start && c == '#') {
        s->in_pp = 1;
        s->pp_continued = 0;
        s->at_line_start = 0;
        (*pos)++;
        return 1;
    }

    /* Update at_line_start tracking for non-pp code. */
    if (c == '\n') s->at_line_start = 1;
    else if (c != ' ' && c != '\t') s->at_line_start = 0;

    /* Check for start of comment/string/template */
    if (c == '/' && c2 == '/') { s->in_line_comment = 1; *pos += 2; s->col++; return 1; }
    if (c == '/' && c2 == '*') { s->in_block_comment = 1; *pos += 2; s->col++; return 1; }
    if (c == '"') { s->in_str = 1; (*pos)++; return 1; }
    if (c == '`') {
        if (!skip_templates) {
            /* Leave ticks + body as code (comptime-for slot substitution). */
            if (c != '\n') s->col--;
            return 0;
        }
        size_t tick_end = 0;
        /* Opening tick already counted in s->col above. Skip a well-formed
         * literal as one inert region so apostrophes inside cannot open a
         * char literal (e.g. `don't` in -e oneliners). Leave unterminated
         * ticks as code so callers (scan_to_top_level / @string) diagnose. */
        if (cc_tpl_scan_literal(src, n, i, &tick_end) == 0) {
            size_t k;
            for (k = i + 1; k <= tick_end; k++) {
                if (src[k] == '\n') { s->line++; s->col = 1; }
                else s->col++;
            }
            *pos = tick_end + 1;
            return 1;
        }
        if (c != '\n') s->col--;
        return 0;
    }
    if (c == '\'') { s->in_chr = 1; (*pos)++; return 1; }
    
    /* At actual code - undo the col++ since caller will handle this char */
    if (c != '\n') s->col--;
    
    return 0;
}

static int cc_scanner_skip_non_code(CCScannerState* s, const char* src, size_t n,
                                    size_t* pos) {
    return cc_scanner_skip_non_code_ex(s, src, n, pos, 1);
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
                                             const char* input_path,
                                             int skip_comptime_surface);
static int cc__apply_phase3_host_lowering_passes(CCPassChain* chain,
                                                 const char* input_path);
char* cc__rewrite_at_await(const char* src, size_t n); /* defined later */
static char* cc__normalize_template_recv_chains(const char* src, size_t n);

/* Python element-kind token for a CC scalar/slice type spelling. */
static const char* cc__py_elem_kind_token(const char* ty) {
    if (!ty) return "CC__PY_EL_SLICE";
    if (strcmp(ty, "double") == 0 || strcmp(ty, "float") == 0)
        return "CC__PY_EL_F64";
    if (strcmp(ty, "int64_t") == 0 || strcmp(ty, "long long") == 0)
        return "CC__PY_EL_I64";
    if (strcmp(ty, "int") == 0) return "CC__PY_EL_INT";
    return "CC__PY_EL_SLICE";
}

static int cc__find_matching_bracket(const char* b, size_t bl, size_t lbracket, size_t* out_rbracket);

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
    CCScannerState scan;
    cc_scanner_init(&scan);
    size_t i = start;
    while (i < end) {
        if (cc_scanner_skip_non_code(&scan, src, end, &i)) continue;
        char c = src[i];
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
        i++;
    }
    return 0;
}

static int cc__pp_builtin_destroy_info(const char* declared_type,
                                       const char** out_pre_callee,
                                       const char** out_callee,
                                       int* out_pass_address) {
    int saw_nursery = 0;
    int saw_arena = 0;
    int saw_checkpoint = 0;
    int saw_chan = 0;
    int saw_slice_unique = 0;
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
            } else if (len == sizeof("CCArenaCheckpoint") - 1 &&
                       memcmp(declared_type + start, "CCArenaCheckpoint", len) == 0) {
                saw_checkpoint = 1;
            } else if (len == sizeof("CCNursery") - 1 &&
                       memcmp(declared_type + start, "CCNursery", len) == 0) {
                saw_nursery = 1;
            } else if (len == sizeof("CCChan") - 1 &&
                       memcmp(declared_type + start, "CCChan", len) == 0) {
                saw_chan = 1;
            } else if (len == sizeof("CCSliceUnique") - 1 &&
                       memcmp(declared_type + start, "CCSliceUnique", len) == 0) {
                saw_slice_unique = 1;
            }
            continue;
        }
        i++;
    }
    if (saw_arena && !saw_nursery && !saw_chan && !saw_slice_unique && !saw_star) {
        if (out_callee) *out_callee = "cc_arena_destroy";
        if (out_pass_address) *out_pass_address = 1;
        return 1;
    }
    if (saw_checkpoint && !saw_arena && !saw_nursery && !saw_chan && !saw_slice_unique && !saw_star) {
        if (out_callee) *out_callee = "cc_arena_checkpoint_destroy";
        if (out_pass_address) *out_pass_address = 1;
        return 1;
    }
    if (saw_slice_unique && !saw_arena && !saw_nursery && !saw_chan && !saw_star) {
        /* Adopted unique slices: cc_slice_destroy runs the registered deleter. */
        if (out_callee) *out_callee = "cc_slice_destroy";
        if (out_pass_address) *out_pass_address = 1;
        return 1;
    }
    if (saw_nursery && !saw_star && !saw_arena && !saw_chan) {
        /* Handle value: destroy peels .n, waits, frees the host. */
        if (out_callee) *out_callee = "cc_nursery_destroy";
        if (out_pass_address) *out_pass_address = 1;
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

        /* Statement start = first code byte after prior `;`/`{`/`}`.
         * Skip delimiters that sit inside comments (task #53: a `;` in a
         * leading block comment must not mid-anchor `@destroy`).  Do not
         * call full-TU `cc_rfind_char_top_level` here — after include
         * expand the span exceeds its 1MiB mask and the fallback is
         * comment-blind again. */
        stmt_s = i;
        while (stmt_s > 0) {
            while (stmt_s > 0 &&
                   src[stmt_s - 1] != ';' &&
                   src[stmt_s - 1] != '{' &&
                   src[stmt_s - 1] != '}') {
                stmt_s--;
            }
            if (stmt_s == 0) break;
            {
                size_t delim = stmt_s - 1;
                int in_bc = 0;
                size_t k = delim;
                /* Non-nested block comments: walking left, opener-first
                 * means `delim` is inside; closer-first means outside. */
                while (k > 0) {
                    if (k >= 2 && src[k - 2] == '*' && src[k - 1] == '/') {
                        in_bc = 0;
                        break;
                    }
                    if (k >= 2 && src[k - 2] == '/' && src[k - 1] == '*') {
                        in_bc = 1;
                        break;
                    }
                    k--;
                }
                if (!in_bc && !cc_scan_pos_in_line_comment(src, delim))
                    break;
            }
            stmt_s--; /* step onto inert delim and keep seeking */
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
/* `T* p = cc_arena_alloc_T[_count](TY, ARENA, ...) @destroy [{ body }];`
 * → `T* p = ...; @defer { body (void)cc_arena_release(ARENA, p); };`
 * Runs after UFCS lowering so ARENA is spelled with the correct
 * reference form. Where the slab cannot reclaim, the release is a
 * semantic no-op; the declaration still states the allocation's scope. */
static char* cc__rewrite_alloct_destroy_annotations(const char* src, size_t n) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0, last_emit = 0, i = 0;
    size_t cur_stmt = 0;
    int changed = 0;
    CCScannerState scan;
    if (!src || n == 0) return NULL;
    /* Needs both the annotation and an arena alloc spelling. */
    if (!cc_contains_token_top_level(src, n, "@destroy") ||
        (!cc_contains_token_top_level(src, n, "cc_arena_alloc") &&
         !memmem(src, n, "cc_arena_alloc_T", 15)))
        return NULL;
    cc_scanner_init(&scan);
    while (i < n) {
        size_t stmt_s, eq, p, name_s, name_e, rhs_s, r, cs, cl, lp, rp;
        size_t a1, a2s, a2e, semi, body_s = 0, body_e = 0;
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (src[i] == ';' || src[i] == '{' || src[i] == '}') {
            cur_stmt = i + 1;
            i++;
            continue;
        }
        if (src[i] != '@' || i + 8 > n || memcmp(src + i, "@destroy", 8) != 0 ||
            (i + 8 < n && cc__pp_is_ident_char(src[i + 8]))) {
            i++;
            continue;
        }
        stmt_s = cc_skip_ws_and_comments(src, n, cur_stmt);
        if (stmt_s >= i || stmt_s < last_emit) { i++; continue; }
        if (!cc__pp_find_top_level_equal(src, stmt_s, i, &eq)) { i++; continue; }
        p = eq;
        while (p > stmt_s && isspace((unsigned char)src[p - 1])) p--;
        name_e = p;
        while (p > stmt_s && cc__pp_is_ident_char(src[p - 1])) p--;
        name_s = p;
        if (name_s >= name_e ||
            !(isalpha((unsigned char)src[name_s]) || src[name_s] == '_')) {
            i++;
            continue;
        }
        rhs_s = cc_skip_ws_and_comments(src, n, eq + 1);
        cs = rhs_s;
        r = rhs_s;
        while (r < i && cc__pp_is_ident_char(src[r])) r++;
        cl = r - cs;
        if (!((cl == 16 && !memcmp(src + cs, "cc_arena_alloc_T", 16)) ||
              (cl == 22 && !memcmp(src + cs, "cc_arena_alloc_T_count", 22)))) {
            i++;
            continue;
        }
        lp = cc_skip_ws_and_comments(src, i, r);
        if (lp >= i || src[lp] != '(' || !cc_find_matching_paren(src, i, lp, &rp)) {
            i++;
            continue;
        }
        a1 = cc_find_char_top_level(src, lp + 1, rp, ',');
        if (a1 >= rp) { i++; continue; }
        a2s = cc_skip_ws_and_comments(src, rp, a1 + 1);
        a2e = cc_find_char_top_level(src, a2s, rp, ',');
        if (a2e > rp) a2e = rp;
        while (a2e > a2s && isspace((unsigned char)src[a2e - 1])) a2e--;
        if (a2e <= a2s) { i++; continue; }
        /* Optional `{ body }` then the statement `;`. */
        semi = cc_skip_ws_and_comments(src, n, i + 8);
        if (semi < n && src[semi] == '{') {
            body_s = semi;
            if (!cc_find_matching_brace(src, n, body_s, &body_e)) { i++; continue; }
            semi = cc_skip_ws_and_comments(src, n, body_e + 1);
        }
        if (semi >= n || src[semi] != ';') { i++; continue; }
        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
        cc_sb_append_cstr(&out, &out_len, &out_cap, "; @defer { ");
        if (body_e > body_s) {
            for (size_t bi = body_s + 1; bi < body_e; bi++) {
                char bc = src[bi];
                char oc2 = (bc == '\n' || bc == '\r' || bc == '\t') ? ' ' : bc;
                cc_sb_append(&out, &out_len, &out_cap, &oc2, 1);
            }
            cc_sb_append_cstr(&out, &out_len, &out_cap, " ");
        }
        cc_sb_append_cstr(&out, &out_len, &out_cap, "(void)cc_arena_release(");
        cc_sb_append(&out, &out_len, &out_cap, src + a2s, a2e - a2s);
        cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
        cc_sb_append(&out, &out_len, &out_cap, src + name_s, name_e - name_s);
        cc_sb_append_cstr(&out, &out_len, &out_cap, "); };");
        last_emit = semi + 1;
        i = semi + 1;
        changed = 1;
    }
    if (!changed) {
        free(out);
        return NULL;
    }
    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

char* cc_rewrite_nursery_create_destroy_proto_ex(const char* src, size_t n, const char* input_path, CCSymbolTable* symbols) {
    (void)symbols;
    return cc__rewrite_builtin_owned_decl_annotations(src, n, input_path);
}

char* cc_rewrite_nursery_create_destroy_proto(const char* src, size_t n, const char* input_path) {
    return cc_rewrite_nursery_create_destroy_proto_ex(src, n, input_path, NULL);
}

/* `@match` was removed from the language (2026-07). The runtime select
   primitive (`cc_chan_match_select`) stays; the statement syntax is gone.
   Reject the reserved token loudly here in phase-1 so source that still
   spells `@match` gets a migration diagnostic instead of falling through
   to TCC as a mystery parse error.
   Returns NULL when the token is absent; (char*)-1 (pass-chain error
   sentinel) after printing the diagnostic when it is present. */
static char* cc__reject_match_syntax(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return NULL;
    if (!cc_contains_token_top_level(src, n, "@match")) return NULL;

    size_t i = 0;
    CCScannerState scanner;
    cc_scanner_init(&scanner);

    while (i < n) {
        /* Skip comments, strings and pp directives using helper (it keeps
         * scanner.line / scanner.col up to date for diagnostics). */
        if (cc_scanner_skip_non_code(&scanner, src, n, &i)) continue;

        char c = src[i];
        if (c == '@') {
            size_t j = i + 1;
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\r' || src[j] == '\n')) j++;
            if (j + 5 <= n && memcmp(src + j, "match", 5) == 0) {
                char after = (j + 5 < n) ? src[j + 5] : 0;
                if (!after || !cc_is_ident_char(after)) {
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                    scanner.line, scanner.col, "syntax",
                                    "'@match' was removed; multiplex channels with cc_chan_match_select(...) (see spec) or restructure with one fiber per source");
                    return (char*)-1;
                }
            }
        }

        /* cc_scanner_skip_non_code already advanced scanner.line/scanner.col
         * for this char (newlines included); just move past non-newline
         * code chars, whose col the helper un-counted for the caller. */
        i++;
        if (c != '\n') scanner.col++;
    }

    return NULL;
}

/* Canonicalize `@with_deadline(expr) { ... }` to `with_deadline(expr) { ... }`
   without otherwise lowering the construct. This is phase-1 CC normalization:
   the scope remains part of canonical CC and is lowered later. */
static char* cc__canonicalize_with_deadline_syntax(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    /* Presence gate: avoid a full-buffer clone when the construct is absent
     * (redis and most smoke TUs never spell it). */
    if (!cc_contains_token_top_level(src, n, "@with_deadline")) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);

    while (i < n) {
        size_t before = i;
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) {
            cc_sb_append(&out, &out_len, &out_cap, src + before, i - before);
            continue;
        }
        char c = src[i];

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
     { CCDeadline __cc_dlN;
       CCDeadline* __cc_useN = cc_deadline_scope(&__cc_dlN, (expr));
       CCDeadline* __cc_prevN = cc_deadline_push(__cc_useN);
       @defer cc_deadline_pop(__cc_prevN);
       { ... } }
   `expr` is milliseconds or an existing CCDeadline*.
   This is phase-3 lowering: canonical CC no longer contains the `@` alias,
   but host-facing parsing still needs the scope expanded into runtime
   scaffolding. */
static char* cc__lower_with_deadline_syntax(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    if (!cc_contains_token_top_level(src, n, "with_deadline")) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    unsigned long counter = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);

    while (i < n) {
        size_t before = i;
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) {
            cc_sb_append(&out, &out_len, &out_cap, src + before, i - before);
            continue;
        }
        char c = src[i];

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
            size_t after_paren = cc_skip_ws_and_comments(src, n, expr_r + 1);

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
                         "{ CCDeadline __cc_dl%lu; "
                         "CCDeadline* %.*s = cc_deadline_scope(&__cc_dl%lu, (%.*s)); "
                         "CCDeadline* __cc_prev%lu = cc_deadline_push(%.*s); "
                         "@defer cc_deadline_pop(__cc_prev%lu); ",
                         counter,
                         (int)as_ident_len, as_ident, counter,
                         (int)(expr_r - expr_l), src + expr_l,
                         counter,
                         (int)as_ident_len, as_ident,
                         counter);
            } else {
                snprintf(hdr, sizeof(hdr),
                         "{ CCDeadline __cc_dl%lu; "
                         "CCDeadline* __cc_use%lu = cc_deadline_scope(&__cc_dl%lu, (%.*s)); "
                         "CCDeadline* __cc_prev%lu = cc_deadline_push(__cc_use%lu); "
                         "@defer cc_deadline_pop(__cc_prev%lu); ",
                         counter, counter, counter,
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

    if (counter == 0) {
        free(out);
        return NULL;
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


/* Odd run of '\\' immediately before '$' => that '$' is literal (`\${` -> `${` in output). */



static size_t cc__scan_to_top_level_delim(const char* src,
                                          size_t n,
                                          size_t start,
                                          char delim1,
                                          char delim2) {
    size_t i = start;
    int paren_depth = 0, brace_depth = 0, bracket_depth = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);
    while (i < n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        char c = src[i];
        if (c == '`') {
            size_t tick_end = 0;
            if (cc_tpl_scan_literal(src, n, i, &tick_end) != 0) return n;
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

/* Encode raw bytes as a C string literal with NO template-escape
 * interpretation — a source `\n` is two bytes (backslash, n), unlike the
 * template escaper above.  Used for ${{...}} verbatim spans. */
static void cc__append_c_string_raw(char** out,
                                    size_t* out_len,
                                    size_t* out_cap,
                                    const char* src,
                                    size_t len) {
    cc_sb_append(out, out_len, out_cap, "\"", 1);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
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
    size_t pos = body_s;
    while (pos < body_e) {
        CCTemplatePiece piece;
        int r = cc_template_next_piece(src, n, body_s, body_e, &pos, &piece);
        if (r < 0) return r;
        if (r == 0) break;
        if (piece.lit_len > 0) {
            cc__emit_template_literal_push(out, out_len, out_cap, builder_name, arena_name,
                                           src + piece.lit_off, piece.lit_len);
        }
        if (piece.kind == CC_TPL_PIECE_VERBATIM && piece.expr_len > 0) {
            /* raw bytes: semantic length == raw length (no escape decoding),
             * and policies never apply to verbatim spans */
            cc__sb_append_fmt_local(out, out_len, out_cap,
                                    "cc_string_push_buffer(&%s, ", builder_name);
            cc__append_c_string_raw(out, out_len, out_cap,
                                    src + piece.expr_off, piece.expr_len);
            cc__sb_append_fmt_local(out, out_len, out_cap, ", %zu, ", piece.expr_len);
            cc_sb_append_cstr(out, out_len, out_cap, arena_name);
            cc_sb_append_cstr(out, out_len, out_cap, "); ");
        }
        if (piece.kind == CC_TPL_PIECE_SLOT || piece.kind == CC_TPL_PIECE_TAGGED_SLOT) {
            /* Stamp the slot so a comptime-TU compile error names the
             * interpolation, not a line inside the lowered builder. */
            {
                const char* lp = NULL;
                size_t lpl = 0;
                int sl = cc_user_line_for_offset(src, n, piece.expr_off, 1, &lp,
                                                 &lpl);
                if (sl > 0) {
                    if (lp && lpl > 0) {
                        char file[PATH_MAX];
                        char rel[1024];
                        const char* shown;
                        size_t fl = lpl < sizeof(file) - 1 ? lpl : sizeof(file) - 1;
                        memcpy(file, lp, fl);
                        file[fl] = '\0';
                        shown = cc_path_rel_to_repo(file, rel, sizeof(rel));
                        cc__sb_append_fmt_local(out, out_len, out_cap,
                                                "\n#line %d \"%s\"\n", sl,
                                                shown ? shown : file);
                    } else {
                        cc__sb_append_fmt_local(out, out_len, out_cap,
                                                "\n#line %d\n", sl);
                    }
                }
            }
            if (policy_name && policy_name[0]) {
                cc__sb_append_fmt_local(out, out_len, out_cap,
                                        "cc_string_push_policy(&%s, %s, %s, ",
                                        builder_name, policy_name, arena_name);
                if (piece.kind == CC_TPL_PIECE_TAGGED_SLOT) {
                    cc_sb_append_cstr(out, out_len, out_cap, "CC_SLICE_LIT(");
                    cc__append_c_string_escaped(out, out_len, out_cap,
                                                src + piece.tag_off, piece.tag_len);
                    cc_sb_append_cstr(out, out_len, out_cap, "), ");
                } else {
                    cc_sb_append_cstr(out, out_len, out_cap, "cc_slice_empty(), ");
                }
                cc_sb_append_cstr(out, out_len, out_cap, "cc__string_slot_arg((");
                cc_sb_append(out, out_len, out_cap, src + piece.expr_off, piece.expr_len);
                cc_sb_append_cstr(out, out_len, out_cap, "), ");
                cc_sb_append_cstr(out, out_len, out_cap, arena_name);
                cc_sb_append_cstr(out, out_len, out_cap, ")); ");
            } else {
                if (piece.kind == CC_TPL_PIECE_TAGGED_SLOT) return -2;
                cc__sb_append_fmt_local(out, out_len, out_cap,
                                        "cc__string_slot_push(&%s, (",
                                        builder_name);
                cc_sb_append(out, out_len, out_cap, src + piece.expr_off, piece.expr_len);
                cc_sb_append_cstr(out, out_len, out_cap, "), ");
                cc_sb_append_cstr(out, out_len, out_cap, arena_name);
                cc_sb_append_cstr(out, out_len, out_cap, "); ");
            }
        }
    }
    return 0;
}

/* ---- Arena-less `@string(`...`)` slot registry (diagnostics) -----------
 *
 * The bounded-template stack form enforces slot boundedness with _Generic
 * macros that have NO default arm, so an unbounded interpolation surfaces
 * as TCC's "type 'X' does not match any association" at the @string line.
 * To turn that into the contract diagnostic ("COMPILE ERROR naming the
 * offending interpolation and suggesting an arena", spec/draft_variants.md
 * §9.2), each lowered slot records (file basename, line, slot text) here;
 * the TCC stderr replay in parser/tcc_bridge.c rewrites matching error
 * lines via cc_string_stack_tpl_slots_for(). */
typedef struct CCStackTplSlotNote {
    char* base;   /* source file basename */
    int line;
    char* expr;   /* slot expression text */
} CCStackTplSlotNote;

static CCStackTplSlotNote* g_stack_tpl_slots = NULL;
static size_t g_stack_tpl_slot_count = 0;
static size_t g_stack_tpl_slot_cap = 0;

static const char* cc__stack_tpl_basename(const char* path) {
    const char* slash;
    if (!path) return "";
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void cc_string_stack_tpl_note_slot(const char* file, int line, const char* expr, size_t expr_len) {
    const char* base = cc__stack_tpl_basename(file);
    for (size_t k = 0; k < g_stack_tpl_slot_count; k++) {
        CCStackTplSlotNote* s = &g_stack_tpl_slots[k];
        if (s->line == line && strcmp(s->base, base) == 0 &&
            strlen(s->expr) == expr_len && strncmp(s->expr, expr, expr_len) == 0) {
            return;
        }
    }
    if (g_stack_tpl_slot_count == g_stack_tpl_slot_cap) {
        size_t new_cap = g_stack_tpl_slot_cap ? g_stack_tpl_slot_cap * 2 : 8;
        CCStackTplSlotNote* grown =
            (CCStackTplSlotNote*)realloc(g_stack_tpl_slots, new_cap * sizeof(*grown));
        if (!grown) return; /* registry is best-effort: raw TCC error still fires */
        g_stack_tpl_slots = grown;
        g_stack_tpl_slot_cap = new_cap;
    }
    {
        CCStackTplSlotNote* s = &g_stack_tpl_slots[g_stack_tpl_slot_count];
        s->base = strdup(base);
        s->line = line;
        s->expr = (char*)malloc(expr_len + 1);
        if (!s->base || !s->expr) {
            free(s->base);
            free(s->expr);
            return;
        }
        memcpy(s->expr, expr, expr_len);
        s->expr[expr_len] = '\0';
        g_stack_tpl_slot_count++;
    }
}

const char* cc_string_stack_tpl_slots_for(const char* file, int line) {
    static char joined[512];
    const char* base = cc__stack_tpl_basename(file);
    size_t used = 0;
    joined[0] = '\0';
    for (size_t k = 0; k < g_stack_tpl_slot_count; k++) {
        CCStackTplSlotNote* s = &g_stack_tpl_slots[k];
        int wrote;
        if (s->line != line || strcmp(s->base, base) != 0) continue;
        wrote = snprintf(joined + used, sizeof(joined) - used, "%s'${%s}'",
                         used ? " / " : "", s->expr);
        if (wrote < 0 || (size_t)wrote >= sizeof(joined) - used) break;
        used += (size_t)wrote;
    }
    return used ? joined : NULL;
}

/* Lower the arena-less bounded-template stack form of `@string`
 * (spec/draft_variants.md §9.2).  Emits a pure expression:
 *
 *   cc__string_stack_slice(
 *     cc__string_stack_push(
 *       cc__string_stack_lit(
 *         cc__string_stack_new((char[K]){0}, K), "v=", 2), (v)))
 *
 * with K = <decoded literal bytes> + cc__string_stack_bound((slot)) + ...
 * — an integer constant expression, so the compound literal is a plain
 * block-scoped char array sized exactly from the bound; the yielded
 * CCSlice is a borrow of it (block lifetime, stack provenance).  The
 * bound/push _Generic macros (std/string.cch) have NO default arm, which
 * is the boundedness check; see the slot registry above for how the
 * failure is reported.
 *
 * Returns 0 ok, -1 unterminated interpolation, -2 tagged slot (policies
 * need an arena), -3 unterminated ${{...}} verbatim span. */
static int cc__emit_string_stack_tpl(char** out, size_t* out_len, size_t* out_cap,
                                     const char* src, size_t n,
                                     size_t body_s, size_t body_e,
                                     const char* input_path, int line) {
    enum { CC_SSTK_LIT = 0, CC_SSTK_RAW = 1, CC_SSTK_SLOT = 2 };
    typedef struct { int kind; size_t off; size_t len; } CCStackLink;
    CCStackLink* links = NULL;
    size_t link_count = 0, link_cap = 0;
    size_t lit_total = 0;
    size_t pos = body_s;
    int rc = 0;
    for (;;) {
        CCTemplatePiece piece;
        int r = cc_template_next_piece(src, n, body_s, body_e, &pos, &piece);
        if (r < 0) { rc = r; goto done; }
        if (r == 0) break;
        if (piece.kind == CC_TPL_PIECE_TAGGED_SLOT) { rc = -2; goto done; }
        if (piece.lit_len > 0 || (piece.kind == CC_TPL_PIECE_VERBATIM && piece.expr_len > 0) ||
            piece.kind == CC_TPL_PIECE_SLOT) {
            size_t need = link_count + 2;
            if (need > link_cap) {
                size_t new_cap = link_cap ? link_cap * 2 : 8;
                CCStackLink* grown;
                if (new_cap < need) new_cap = need;
                grown = (CCStackLink*)realloc(links, new_cap * sizeof(*grown));
                if (!grown) { rc = -1; goto done; }
                links = grown;
                link_cap = new_cap;
            }
        }
        if (piece.lit_len > 0) {
            links[link_count].kind = CC_SSTK_LIT;
            links[link_count].off = piece.lit_off;
            links[link_count].len = piece.lit_len;
            link_count++;
            lit_total += cc__template_literal_decoded_len(src + piece.lit_off, piece.lit_len);
        }
        if (piece.kind == CC_TPL_PIECE_VERBATIM && piece.expr_len > 0) {
            links[link_count].kind = CC_SSTK_RAW;
            links[link_count].off = piece.expr_off;
            links[link_count].len = piece.expr_len;
            link_count++;
            lit_total += piece.expr_len;
        }
        if (piece.kind == CC_TPL_PIECE_SLOT) {
            links[link_count].kind = CC_SSTK_SLOT;
            links[link_count].off = piece.expr_off;
            links[link_count].len = piece.expr_len;
            link_count++;
        }
    }
    if (link_count == 0) {
        cc_sb_append_cstr(out, out_len, out_cap, "cc_slice_empty()");
        goto done;
    }
    {
        /* K: exact compile-time bound (integer constant expression). */
        char* kbuf = NULL;
        size_t klen = 0, kcap = 0;
        cc__sb_append_fmt_local(&kbuf, &klen, &kcap, "%zuu", lit_total);
        for (size_t j = 0; j < link_count; j++) {
            if (links[j].kind != CC_SSTK_SLOT) continue;
            cc_sb_append_cstr(&kbuf, &klen, &kcap, " + cc__string_stack_bound((");
            cc_sb_append(&kbuf, &klen, &kcap, src + links[j].off, links[j].len);
            cc_sb_append_cstr(&kbuf, &klen, &kcap, "))");
        }
        cc_sb_append_cstr(out, out_len, out_cap, "cc__string_stack_slice(");
        for (size_t j = link_count; j > 0; j--) {
            cc_sb_append_cstr(out, out_len, out_cap,
                              links[j - 1].kind == CC_SSTK_SLOT ? "cc__string_stack_push("
                                                                : "cc__string_stack_lit(");
        }
        cc_sb_append_cstr(out, out_len, out_cap, "cc__string_stack_new((char[");
        cc_sb_append(out, out_len, out_cap, kbuf, klen);
        cc_sb_append_cstr(out, out_len, out_cap, "]){0}, ");
        cc_sb_append(out, out_len, out_cap, kbuf, klen);
        cc_sb_append_cstr(out, out_len, out_cap, ")");
        free(kbuf);
    }
    for (size_t j = 0; j < link_count; j++) {
        if (links[j].kind == CC_SSTK_SLOT) {
            cc_sb_append_cstr(out, out_len, out_cap, ", (");
            cc_sb_append(out, out_len, out_cap, src + links[j].off, links[j].len);
            cc_sb_append_cstr(out, out_len, out_cap, "))");
            cc_string_stack_tpl_note_slot(input_path, line, src + links[j].off, links[j].len);
        } else if (links[j].kind == CC_SSTK_RAW) {
            cc_sb_append_cstr(out, out_len, out_cap, ", ");
            cc__append_c_string_raw(out, out_len, out_cap, src + links[j].off, links[j].len);
            cc__sb_append_fmt_local(out, out_len, out_cap, ", %zu)", links[j].len);
        } else {
            cc_sb_append_cstr(out, out_len, out_cap, ", ");
            cc__append_c_string_escaped(out, out_len, out_cap, src + links[j].off, links[j].len);
            cc__sb_append_fmt_local(out, out_len, out_cap, ", %zu)",
                                    cc__template_literal_decoded_len(src + links[j].off,
                                                                     links[j].len));
        }
    }
    cc_sb_append_cstr(out, out_len, out_cap, ")");
done:
    free(links);
    return rc;
}

/* ---- `@string(..., @scratch)` / `@scratch(N)` ---------------------------
 * Sugar for a function/closure-scoped stack arena passed as the @string arena
 * operand. All sites in the same function or closure body share one
 * cc_arena_stack(__cc_str_scratch, max N) at that body's '{'; every arena
 * arg rewrites to &__cc_str_scratch. Not a general expression. */
#define CC_STR_SCRATCH_DEFAULT_BYTES 1024
#define CC_STR_SCRATCH_MAX_SITES 256

typedef struct {
    size_t brace;   /* function/closure '{' after which to inject cc_arena_stack */
    size_t arg_s;   /* start of @scratch... in source */
    size_t arg_e;   /* exclusive end of arena arg */
    size_t nbytes;
    int id;
    int line;
    int col;
} CCStrScratchSite;

/* Parse `@scratch` / `@scratch(N)` starting at `s`. `scan_e` is a hint from
 * the top-level `)` scan (which stops at the first paren-balanced `)`, so for
 * `@scratch(N)` it points at N's closing paren). On success, `*out_arg_e` is
 * the exclusive end of the scratch token (past `@scratch` or past `@scratch(N)`). */
static int cc__parse_at_scratch_arg(const char* src, size_t n, size_t s, size_t scan_e,
                                    size_t* out_nbytes, size_t* out_arg_e) {
    size_t p;
    size_t val;
    if (!src || !out_nbytes || !out_arg_e || s >= n) return 0;
    s = cc_skip_ws_and_comments(src, n, s);
    if (s + 8 > n || memcmp(src + s, "@scratch", 8) != 0) return 0;
    p = s + 8;
    if (p < n && (isalnum((unsigned char)src[p]) || src[p] == '_')) return 0;
    p = cc_skip_ws_and_comments(src, n, p);
    if (p >= n || src[p] != '(') {
        /* Bare `@scratch` — optional trailing ws up to scan_e. */
        size_t end = p;
        if (scan_e > s && scan_e <= n) {
            size_t t = cc_skip_ws_and_comments(src, n, p);
            if (t > scan_e) return -1;
            /* allow only whitespace between keyword and the @string ')' */
            while (p < scan_e) {
                char c = src[p];
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return -1;
                p++;
            }
            end = scan_e;
        }
        *out_nbytes = CC_STR_SCRATCH_DEFAULT_BYTES;
        *out_arg_e = end;
        return 1;
    }
    /* `@scratch(N)` — parse through the closing paren even when scan_e
     * landed on that same `)` (top-level delim scan quirk). */
    p = cc_skip_ws_and_comments(src, n, p + 1);
    if (p >= n || !isdigit((unsigned char)src[p])) return -1;
    val = 0;
    while (p < n && isdigit((unsigned char)src[p])) {
        size_t digit = (size_t)(src[p] - '0');
        if (val > 1024u * 1024u * 64u / 10) return -1;
        val = val * 10 + digit;
        p++;
    }
    p = cc_skip_ws_and_comments(src, n, p);
    if (p >= n || src[p] != ')') return -1;
    p++; /* past ')' of @scratch(N) */
    if (val == 0) return -1;
    *out_nbytes = val;
    *out_arg_e = p;
    return 1;
}

static int cc__scratch_ident_eq_range(const char* s, size_t n, const char* kw) {
    size_t klen;
    if (!s || !kw) return 0;
    klen = strlen(kw);
    return n == klen && memcmp(s, kw, klen) == 0;
}

/* Control / attribute bodies that open with `name(...) {` but are not
 * function or closure scopes for @scratch sharing. */
static int cc__scratch_is_non_fn_kw_range(const char* s, size_t n) {
    return cc__scratch_ident_eq_range(s, n, "if") ||
           cc__scratch_ident_eq_range(s, n, "for") ||
           cc__scratch_ident_eq_range(s, n, "while") ||
           cc__scratch_ident_eq_range(s, n, "switch") ||
           cc__scratch_ident_eq_range(s, n, "catch") ||
           cc__scratch_ident_eq_range(s, n, "errhandler") ||
           cc__scratch_ident_eq_range(s, n, "with_deadline") ||
           cc__scratch_ident_eq_range(s, n, "destroy");
}

/* True when `src[brace]` opens a function or closure body (`name(...) {`,
 * `=> {`, or `=> [...] {`). */
static int cc__lbrace_is_fn_or_closure_body(const char* src, size_t n, size_t brace) {
    size_t j;
    if (!src || brace >= n || src[brace] != '{') return 0;
    j = brace;
    while (j > 0) {
        char c = src[j - 1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        j--;
    }
    if (j == 0) return 0;
    /* `=> {` */
    if (src[j - 1] == '>' && j >= 2 && src[j - 2] == '=') return 1;
    /* `=> [captures] {` */
    if (src[j - 1] == ']') {
        size_t k = j - 1;
        int depth = 1;
        while (k > 0 && depth > 0) {
            k--;
            if (src[k] == ']') depth++;
            else if (src[k] == '[') depth--;
        }
        if (depth != 0) return 0;
        while (k > 0) {
            char c = src[k - 1];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            k--;
        }
        if (k >= 2 && src[k - 1] == '>' && src[k - 2] == '=') return 1;
        return 0;
    }
    /* `name(...) {` — reject control/attr keywords and `@name(...) {`. */
    if (src[j - 1] != ')') return 0;
    {
        size_t close_paren = j - 1;
        size_t open_paren = close_paren;
        size_t name_end, name_start;
        int par = 1;
        while (open_paren > 0) {
            open_paren--;
            if (src[open_paren] == ')') par++;
            else if (src[open_paren] == '(') {
                par--;
                if (par == 0) break;
            }
        }
        if (par != 0) return 0;
        name_end = open_paren;
        while (name_end > 0) {
            char c = src[name_end - 1];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            name_end--;
        }
        name_start = name_end;
        while (name_start > 0) {
            unsigned char c = (unsigned char)src[name_start - 1];
            if (!(isalnum(c) || c == '_')) break;
            name_start--;
        }
        if (name_start == name_end) return 0;
        if (cc__scratch_is_non_fn_kw_range(src + name_start, name_end - name_start))
            return 0;
        if (name_start > 0 && src[name_start - 1] == '@') return 0;
        return 1;
    }
}

/* Innermost function/closure `{` enclosing `pos`, else innermost block `{`. */
static int cc__find_enclosing_lbrace(const char* src, size_t n, size_t pos, size_t* out_brace) {
    size_t stack[256];
    int sp = 0;
    int k;
    CCScannerState scan;
    size_t i = 0;
    if (!src || !out_brace || pos > n) return 0;
    cc_scanner_init(&scan);
    while (i < pos) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (i >= pos) break;
        if (src[i] == '{') {
            if (sp < (int)(sizeof(stack) / sizeof(stack[0]))) stack[sp++] = i;
            i++;
        } else if (src[i] == '}') {
            if (sp > 0) sp--;
            i++;
        } else {
            i++;
        }
    }
    if (sp <= 0) return 0;
    for (k = sp - 1; k >= 0; k--) {
        if (cc__lbrace_is_fn_or_closure_body(src, n, stack[k])) {
            *out_brace = stack[k];
            return 1;
        }
    }
    *out_brace = stack[sp - 1];
    return 1;
}

static int cc__line_col_at(const char* src, size_t n, size_t pos, int* line, int* col) {
    CCScannerState scan;
    size_t i = 0;
    if (!src || !line || !col) return 0;
    cc_scanner_init(&scan);
    while (i < pos && i < n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        i++;
    }
    *line = scan.line;
    *col = scan.col;
    return 1;
}

/* `@name` followed by `delim`, with whitespace and comments allowed between,
 * and `@name` matched as a whole token.
 *
 * Replaces `memcmp(src + i, "@name(", ...)`, which required the delimiter to
 * be the very next byte, so a comment sitting between a sigil and its `(` made
 * the call invisible and it was left unlowered — reaching the C compiler as an
 * `@` that means nothing there.  A comment in that gap is inert filler, like a
 * space.
 *
 * Returns 1 and sets `*out_after` to the offset just past the delimiter. */
static int cc__sigil_delim_after(const char* src, size_t n, size_t i,
                                 const char* name, char delim,
                                 size_t* out_after) {
    size_t ln = strlen(name);
    size_t p;
    if (i + ln > n || memcmp(src + i, name, ln) != 0) return 0;
    if (i + ln < n && cc_is_ident_char(src[i + ln])) return 0;
    p = cc_skip_ws_and_comments(src, n, i + ln);
    if (p >= n || src[p] != delim) return 0;
    if (out_after) *out_after = p + 1;
    return 1;
}

/* Collect @string arena args that are @scratch / @scratch(N). Returns
 * site count, -1 on error. */

static int cc__collect_string_scratch_sites(const char* src, size_t n, const char* input_path,
                                           CCStrScratchSite* sites, int max_sites) {
    CCScannerState scan;
    size_t i = 0;
    int count = 0;
    if (!src || !sites || max_sites <= 0) return 0;
    cc_scanner_init(&scan);
    while (i < n) {
        size_t arg1_s, arg1_e;
        size_t arena_s = 0, arena_e = 0;
        size_t nbytes = 0;
        size_t brace = 0;
        int prc;
        size_t sig_after = 0;
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (!cc__sigil_delim_after(src, n, i, "@string", '(', &sig_after)) {
            i++;
            continue;
        }
        arg1_s = cc_skip_ws_and_comments(src, n, sig_after);
        arg1_e = cc__scan_to_top_level_delim(src, n, arg1_s, ',', ')');
        if (arg1_e >= n) {
            i++;
            continue;
        }
        if (src[arg1_e] == ')' ) {
            /* Arena-less or malformed — no scratch site. */
            i = arg1_e + 1;
            continue;
        }
        if (arg1_s < n && src[arg1_s] == '`') {
            size_t tick_e = 0;
            size_t arg2_s;
            if (cc_tpl_scan_literal(src, n, arg1_s, &tick_e) != 0) {
                i++;
                continue;
            }
            arg2_s = cc_skip_ws_and_comments(src, n, tick_e + 1);
            if (arg2_s >= n || src[arg2_s] != ',') {
                i = tick_e + 1;
                continue;
            }
            arena_s = cc_skip_ws_and_comments(src, n, arg2_s + 1);
            arena_e = cc__scan_to_top_level_delim(src, n, arena_s, ')', '\0');
            if (arena_e >= n || src[arena_e] != ')') {
                i = arg2_s + 1;
                continue;
            }
        } else {
            size_t arg2_s = cc_skip_ws_and_comments(src, n, arg1_e + 1);
            if (arg2_s < n && src[arg2_s] == '`') {
                size_t tick_e = 0;
                size_t arg3_s;
                if (cc_tpl_scan_literal(src, n, arg2_s, &tick_e) != 0) {
                    i++;
                    continue;
                }
                arg3_s = cc_skip_ws_and_comments(src, n, tick_e + 1);
                if (arg3_s >= n || src[arg3_s] != ',') {
                    i = tick_e + 1;
                    continue;
                }
                arena_s = cc_skip_ws_and_comments(src, n, arg3_s + 1);
                arena_e = cc__scan_to_top_level_delim(src, n, arena_s, ')', '\0');
                if (arena_e >= n || src[arena_e] != ')') {
                    i = arg3_s + 1;
                    continue;
                }
            } else {
                arena_s = arg2_s;
                arena_e = cc__scan_to_top_level_delim(src, n, arena_s, ')', '\0');
                if (arena_e >= n || src[arena_e] != ')') {
                    i = arg2_s;
                    continue;
                }
            }
        }
        {
            size_t scratch_e = arena_e;
            prc = cc__parse_at_scratch_arg(src, n, arena_s, arena_e, &nbytes, &scratch_e);
            if (prc == 0) {
                i = arena_e + 1;
                continue;
            }
            if (prc < 0) {
                char rel[1024];
                int line = 1, col = 1;
                cc__line_col_at(src, n, arena_s, &line, &col);
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                line, col, "syntax",
                                "malformed @scratch in @string arena argument "
                                "(use @scratch or @scratch(N) with N > 0)");
                return -1;
            }
            /* After @scratch(N), only whitespace may remain before @string ')'. */
            {
                size_t after = cc_skip_ws_and_comments(src, n, scratch_e);
                size_t string_close = cc__scan_to_top_level_delim(src, n, after, ')', '\0');
                /* For bare @scratch, scan_e was already the string ')'. For
                 * @scratch(N), scratch_e is past N's ')' and after should be
                 * the string-closing ')'. */
                if (after < n && src[after] == ')')
                    string_close = after;
                else if (string_close >= n || src[string_close] != ')') {
                    char rel[1024];
                    int line = 1, col = 1;
                    cc__line_col_at(src, n, arena_s, &line, &col);
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                    line, col, "syntax",
                                    "malformed @scratch in @string arena argument "
                                    "(use @scratch or @scratch(N) with N > 0)");
                    return -1;
                }
                (void)string_close;
            }
            if (!cc__find_enclosing_lbrace(src, n, i, &brace)) {
                char rel[1024];
                int line = 1, col = 1;
                cc__line_col_at(src, n, i, &line, &col);
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                line, col, "syntax",
                                "@string(..., @scratch) requires an enclosing function or block");
                return -1;
            }
            if (count >= max_sites) {
                char rel[1024];
                int line = 1, col = 1;
                cc__line_col_at(src, n, i, &line, &col);
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                line, col, "syntax",
                                "too many @scratch sites in one translation unit");
                return -1;
            }
            sites[count].brace = brace;
            sites[count].arg_s = arena_s;
            sites[count].arg_e = scratch_e;
            sites[count].nbytes = nbytes;
            sites[count].id = count;
            cc__line_col_at(src, n, arena_s, &sites[count].line, &sites[count].col);
            count++;
            i = scratch_e;
        }
    }
    return count;
}

static int cc__scratch_site_in_span(const CCStrScratchSite* sites, int n_sites,
                                    size_t pos) {
    int k;
    for (k = 0; k < n_sites; k++) {
        if (pos >= sites[k].arg_s && pos < sites[k].arg_e) return 1;
    }
    return 0;
}

/* Error if @scratch appears outside an @string arena arg. */
static int cc__reject_orphan_at_scratch(const char* src, size_t n, const char* input_path,
                                        const CCStrScratchSite* sites, int n_sites) {
    CCScannerState scan;
    size_t i = 0;
    cc_scanner_init(&scan);
    while (i < n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (i + 8 <= n && memcmp(src + i, "@scratch", 8) == 0) {
            size_t after = i + 8;
            if (after < n && (isalnum((unsigned char)src[after]) || src[after] == '_')) {
                i++;
                continue;
            }
            if (!cc__scratch_site_in_span(sites, n_sites, i)) {
                char rel[1024];
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scan.line, scan.col, "syntax",
                                "@scratch is only valid as the arena argument of @string(...); "
                                "use cc_arena_stack for a named scratch arena");
                return -1;
            }
            i = after;
            continue;
        }
        i++;
    }
    return 0;
}

static int cc__scratch_site_cmp_brace(const void* a, const void* b) {
    const CCStrScratchSite* sa = (const CCStrScratchSite*)a;
    const CCStrScratchSite* sb = (const CCStrScratchSite*)b;
    if (sa->brace < sb->brace) return -1;
    if (sa->brace > sb->brace) return 1;
    if (sa->id < sb->id) return -1;
    if (sa->id > sb->id) return 1;
    return 0;
}

static int cc__scratch_site_cmp_arg(const void* a, const void* b) {
    const CCStrScratchSite* sa = (const CCStrScratchSite*)a;
    const CCStrScratchSite* sb = (const CCStrScratchSite*)b;
    if (sa->arg_s < sb->arg_s) return -1;
    if (sa->arg_s > sb->arg_s) return 1;
    return 0;
}

/* Expand @scratch arena args; NULL = no sites, (char*)-1 = error.
 * One shared cc_arena_stack(__cc_str_scratch, max N) per function/closure. */
static char* cc__expand_string_scratch(const char* src, size_t n, const char* input_path) {
    CCStrScratchSite sites[CC_STR_SCRATCH_MAX_SITES];
    CCStrScratchSite by_brace[CC_STR_SCRATCH_MAX_SITES];
    CCStrScratchSite by_arg[CC_STR_SCRATCH_MAX_SITES];
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t pos = 0;
    int n_sites;
    int bi = 0, ai = 0;
    if (!src || n == 0) return NULL;
    if (!cc_contains_token_top_level(src, n, "@scratch")) return NULL;
    n_sites = cc__collect_string_scratch_sites(src, n, input_path, sites, CC_STR_SCRATCH_MAX_SITES);
    if (n_sites < 0) return (char*)-1;
    if (cc__reject_orphan_at_scratch(src, n, input_path, sites, n_sites) != 0)
        return (char*)-1;
    if (n_sites == 0) return NULL;
    memcpy(by_brace, sites, (size_t)n_sites * sizeof(sites[0]));
    memcpy(by_arg, sites, (size_t)n_sites * sizeof(sites[0]));
    qsort(by_brace, (size_t)n_sites, sizeof(by_brace[0]), cc__scratch_site_cmp_brace);
    qsort(by_arg, (size_t)n_sites, sizeof(by_arg[0]), cc__scratch_site_cmp_arg);
    while (pos < n || bi < n_sites || ai < n_sites) {
        size_t next_inj = (bi < n_sites) ? by_brace[bi].brace + 1 : (size_t)-1;
        size_t next_rep = (ai < n_sites) ? by_arg[ai].arg_s : (size_t)-1;
        size_t next;
        if (next_inj == (size_t)-1 && next_rep == (size_t)-1) break;
        if (next_inj == (size_t)-1) next = next_rep;
        else if (next_rep == (size_t)-1) next = next_inj;
        else next = (next_inj <= next_rep) ? next_inj : next_rep;
        if (pos < next) {
            cc_sb_append(&out, &out_len, &out_cap, src + pos, next - pos);
            pos = next;
        }
        if (next == next_inj) {
            size_t nbytes = by_brace[bi].nbytes;
            size_t brace = by_brace[bi].brace;
            while (bi < n_sites && by_brace[bi].brace == brace) {
                if (by_brace[bi].nbytes > nbytes) nbytes = by_brace[bi].nbytes;
                bi++;
            }
            cc__sb_append_fmt_local(&out, &out_len, &out_cap,
                                    " cc_arena_stack(__cc_str_scratch, %zu); ",
                                    nbytes);
        } else {
            cc_sb_append_cstr(&out, &out_len, &out_cap, "&__cc_str_scratch");
            pos = by_arg[ai].arg_e;
            ai++;
        }
    }
    if (pos < n) cc_sb_append(&out, &out_len, &out_cap, src + pos, n - pos);
    return out;
}

static char* cc__rewrite_string_templates(const char* src, size_t n, const char* input_path) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0, last_emit = 0;
    int rewrite_count = 0;
    CCScannerState scan;
    if (!src || n == 0) return NULL;
    if (!cc_contains_token_top_level(src, n, "@slice") &&
        !cc_contains_token_top_level(src, n, "@emit") &&
        !cc_contains_token_top_level(src, n, "@string")) {
        return NULL;
    }
    cc_scanner_init(&scan);
    while (i < n) {
        size_t sig_after = 0;
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        char c = src[i];
        if (c == '@' && cc__sigil_delim_after(src, n, i, "@slice", '(', &sig_after)) {
            size_t arg_s = cc_skip_ws_and_comments(src, n, sig_after);
            size_t arg_e = cc__scan_to_top_level_delim(src, n, arg_s, ')', '\0');
            if (arg_e >= n || src[arg_e] != ')') {
                char rel[1024];
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scan.line, scan.col, "syntax", "unterminated @slice(...)");
                free(out);
                return (char*)-1;
            }
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
            /* Same expansion as call-arg lit coerce; @slice is optional sugar. */
            cc_sb_append_cstr(&out, &out_len, &out_cap, "CC_SLICE_LIT(");
            cc_sb_append(&out, &out_len, &out_cap, src + arg_s, arg_e - arg_s);
            cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
            rewrite_count++;
            last_emit = arg_e + 1;
            i = arg_e + 1;
            continue;
        }
        if (c == '@' && cc__sigil_delim_after(src, n, i, "@emit", '(', &sig_after)) {
            size_t arg1_s = cc_skip_ws_and_comments(src, n, sig_after);
            int has_anchor = 0;
            int anchor_val = 0;
            size_t tick_s = arg1_s;
            size_t tick_e = 0;
            char builder_name[64], arena_name[64];
            if (arg1_s < n && src[arg1_s] != '`') {
                size_t anchor_e = cc__scan_to_top_level_delim(src, n, arg1_s, ',', ')');
                if (anchor_e >= n) {
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                    scan.line, scan.col, "syntax", "unterminated @emit(...)");
                    free(out);
                    return (char*)-1;
                }
                if (src[anchor_e] == ')') {
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                    scan.line, scan.col, "syntax", "@emit(anchor, `...`) requires a template literal");
                    free(out);
                    return (char*)-1;
                }
                if (src[arg1_s] >= '0' && src[arg1_s] <= '9') {
                    while (arg1_s < anchor_e && src[arg1_s] >= '0' && src[arg1_s] <= '9')
                        anchor_val = anchor_val * 10 + (src[arg1_s++] - '0');
                    has_anchor = 1;
                } else if (anchor_e - arg1_s == (int)strlen("CC_EMIT_AFTER_PRELUDE") &&
                           memcmp(src + arg1_s, "CC_EMIT_AFTER_PRELUDE", anchor_e - arg1_s) == 0) {
                    anchor_val = 0; has_anchor = 1;
                } else if (anchor_e - arg1_s == (int)strlen("CC_EMIT_BEFORE_FIRST_USE") &&
                           memcmp(src + arg1_s, "CC_EMIT_BEFORE_FIRST_USE", anchor_e - arg1_s) == 0) {
                    anchor_val = 1; has_anchor = 1;
                } else if (anchor_e - arg1_s == (int)strlen("CC_EMIT_AT_COMPTIME_SITE") &&
                           memcmp(src + arg1_s, "CC_EMIT_AT_COMPTIME_SITE", anchor_e - arg1_s) == 0) {
                    anchor_val = 2; has_anchor = 1;
                } else {
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                    scan.line, scan.col, "syntax", "unknown @emit anchor");
                    free(out);
                    return (char*)-1;
                }
                tick_s = cc_skip_ws_and_comments(src, n, anchor_e + 1);
            }
            if (tick_s >= n || src[tick_s] != '`') {
                char rel[1024];
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scan.line, scan.col, "syntax", "@emit(...) requires a backtick template literal");
                free(out);
                return (char*)-1;
            }
            if (cc_tpl_scan_literal(src, n, tick_s, &tick_e) != 0) {
                char rel[1024];
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scan.line, scan.col, "syntax", "unterminated template literal in @emit(...)");
                free(out);
                return (char*)-1;
            }
            {
                /* After the template literal:
                 *   - anchored splice form `@emit(anchor, `...`)` takes NO arena;
                 *     it is self-contained (inline cc_arena_stack, splice, free).
                 *   - return form `@emit(`...`, arena)` REQUIRES an explicit arena
                 *     and yields a CCSlice persisted into it (mirrors @string). */
                size_t after = cc_skip_ws_and_comments(src, n, tick_e + 1);
                size_t close_p;
                size_t arena_s = 0, arena_e = 0;
                int have_arena = 0;
                char stack_name[80];
                if (after < n && src[after] == ',') {
                    arena_s = cc_skip_ws_and_comments(src, n, after + 1);
                    arena_e = cc__scan_to_top_level_delim(src, n, arena_s, ')', '\0');
                    if (arena_e >= n || src[arena_e] != ')') {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                        scan.line, scan.col, "syntax", "unterminated @emit(`...`, arena)");
                        free(out);
                        return (char*)-1;
                    }
                    have_arena = 1;
                    close_p = arena_e;
                } else if (after < n && src[after] == ')') {
                    close_p = after;
                } else {
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                    scan.line, scan.col, "syntax", "unterminated @emit(...)");
                    free(out);
                    return (char*)-1;
                }
                if (has_anchor && have_arena) {
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                    scan.line, scan.col, "syntax",
                                    "@emit(anchor, `...`) splices and takes no arena");
                    free(out);
                    return (char*)-1;
                }
                if (!has_anchor && !have_arena) {
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                    scan.line, scan.col, "syntax",
                                    "@emit(`...`) requires an arena: @emit(`...`, arena)");
                    free(out);
                    return (char*)-1;
                }
                /* `@emit(`...`, arena)` BUILDS a fragment and yields it; the
                 * anchor form is what splices.  Written as a bare statement
                 * the fragment is constructed and dropped, and the code it
                 * was meant to emit silently never appears. */
                if (have_arena) {
                    size_t tail = cc_skip_ws_and_comments(src, n, close_p + 1);
                    if (tail < n && src[tail] == ';') {
                        size_t b = cc_rskip_ws_and_comments(src, i);
                        if (b == 0 || src[b - 1] == ';' || src[b - 1] == '{' ||
                            src[b - 1] == '}') {
                            char rel[1024];
                            cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                            scan.line, scan.col, "syntax",
                                            "@emit(`...`, arena) builds a fragment and yields it; "
                                            "discarding it emits nothing");
                            fprintf(stderr, "  note: to splice from a @comptime block, use "
                                            "@emit(CC_EMIT_AFTER_PRELUDE, `...`)\n");
                            fprintf(stderr, "  note: in a generic factory, return it: "
                                            "return @emit(`...`, arena);\n");
                            free(out);
                            return (char*)-1;
                        }
                    }
                }
                cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                snprintf(builder_name, sizeof(builder_name), "__cc_et_s_%d", rewrite_count);
                snprintf(arena_name, sizeof(arena_name), "__cc_et_a_%d", rewrite_count);
                snprintf(stack_name, sizeof(stack_name), "__cc_et_as_%d", rewrite_count);
                if (has_anchor) {
                    /* Self-contained: stack arena, build, splice (cc_emit_raw
                       copies), then free.  arena_name is a CCArena* alias so the
                       shared body rewriter sees a pointer in both forms. */
                    cc__sb_append_fmt_local(&out, &out_len, &out_cap,
                                            "({ cc_arena_stack(%s, %d); CCArena* %s = &%s; "
                                            "CCString %s = cc_string_new(); ",
                                            stack_name, CC_EMIT_TPL_BUF_SIZE,
                                            arena_name, stack_name, builder_name);
                } else {
                    cc__sb_append_fmt_local(&out, &out_len, &out_cap,
                                            "({ CCArena* %s = (", arena_name);
                    cc_sb_append(&out, &out_len, &out_cap, src + arena_s, arena_e - arena_s);
                    cc__sb_append_fmt_local(&out, &out_len, &out_cap,
                                            "); CCString %s = cc_string_new(); ", builder_name);
                }
                {
                    int tpl_rc = cc__rewrite_template_body(&out, &out_len, &out_cap, src, n,
                                                           tick_s + 1, tick_e,
                                                           builder_name, NULL, arena_name);
                    if (tpl_rc != 0) {
                        char rel[1024];
                        const char* msg = (tpl_rc == -2)
                            ? "tagged template slots require @string(policy, `...`, arena)"
                            : (tpl_rc == -3)
                                ? "unterminated ${{...}} verbatim span in @emit template"
                                : "unterminated interpolation in @emit template";
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                        scan.line, scan.col, "syntax", msg);
                        free(out);
                        return (char*)-1;
                    }
                }
                if (has_anchor) {
                    /* Stamp the template body so a host-C error in the
                     * spliced fragment names this file:line, not the
                     * @comptime keyword (and not emit.c).  First body
                     * byte — often the newline after ` — is the #line
                     * origin; later fragment lines increment from there. */
                    const char* lp = NULL;
                    size_t lpl = 0;
                    int ol = cc_user_line_for_offset(src, n, tick_s + 1, 1, &lp, &lpl);
                    char file[PATH_MAX];
                    char rel[1024];
                    const char* shown;
                    if (lp && lpl > 0) {
                        size_t fl = lpl < sizeof(file) - 1 ? lpl : sizeof(file) - 1;
                        memcpy(file, lp, fl);
                        file[fl] = '\0';
                        shown = cc_path_rel_to_repo(file, rel, sizeof(rel));
                    } else {
                        shown = cc_path_rel_to_repo(input_path ? input_path : "<input>",
                                                    rel, sizeof(rel));
                    }
                    if (ol <= 0) ol = scan.line;
                    cc__sb_append_fmt_local(&out, &out_len, &out_cap,
                                            "cc_emit_tpl_splice_at(%d, \"%s\", %d, cc_string_as_slice(&%s)); "
                                            "cc_arena_free(%s); 0; })",
                                            anchor_val, shown ? shown : "<input>", ol,
                                            builder_name, arena_name);
                } else {
                    cc__sb_append_fmt_local(&out, &out_len, &out_cap,
                                            "cc__string_persist_slice(%s, &%s); })",
                                            arena_name, builder_name);
                }
                rewrite_count++;
                last_emit = close_p + 1;
                i = close_p + 1;
                continue;
            }
        }
        if (c == '@' && cc__sigil_delim_after(src, n, i, "@string", '(', &sig_after)) {
            size_t arg1_s = cc_skip_ws_and_comments(src, n, sig_after);
            size_t arg1_e = cc__scan_to_top_level_delim(src, n, arg1_s, ',', ')');
            if (arg1_e >= n) {
                char rel[1024];
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scan.line, scan.col, "syntax", "unterminated @string(...)");
                free(out);
                return (char*)-1;
            }
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
            if (src[arg1_e] == ')' && !(arg1_s < n && src[arg1_s] == '`')) {
                /* Non-template forms keep requiring an arena; a backtick
                 * template with no arena is the bounded stack form
                 * (spec/draft_variants.md §9.2), handled below. */
                char rel[1024];
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scan.line, scan.col, "syntax", "@string(...) requires an arena argument");
                free(out);
                return (char*)-1;
            }
            {
                if (arg1_s < n && src[arg1_s] == '`') {
                    size_t tick_e = 0;
                    size_t arg2_s, arg2_e;
                    char builder_name[64], arena_name[64];
                    if (cc_tpl_scan_literal(src, n, arg1_s, &tick_e) != 0) {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                        scan.line, scan.col, "syntax", "unterminated template literal in @string(...)");
                        free(out);
                        return (char*)-1;
                    }
                    arg2_s = cc_skip_ws_and_comments(src, n, tick_e + 1);
                    if (arg2_s < n && src[arg2_s] == ')') {
                        /* Arena-less bounded-template stack form (§9.2):
                         * stack buffer sized from the static bound, yields
                         * a char[:] borrow instead of an owned CCString. */
                        int tpl_rc = cc__emit_string_stack_tpl(&out, &out_len, &out_cap, src, n,
                                                               arg1_s + 1, tick_e,
                                                               input_path, scan.line);
                        if (tpl_rc != 0) {
                            char rel[1024];
                            const char* msg = (tpl_rc == -2)
                                ? "tagged template slots require @string(policy, `...`, arena)"
                                : (tpl_rc == -3)
                                    ? "unterminated ${{...}} verbatim span in @string template"
                                    : "unterminated interpolation in @string template";
                            cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                            scan.line, scan.col, "syntax", msg);
                            free(out);
                            return (char*)-1;
                        }
                        rewrite_count++;
                        last_emit = arg2_s + 1;
                        i = arg2_s + 1;
                        continue;
                    }
                    if (arg2_s >= n || src[arg2_s] != ',') {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                        scan.line, scan.col, "syntax", "@string(`...`, arena) requires a trailing arena argument");
                        free(out);
                        return (char*)-1;
                    }
                    arg2_s = cc_skip_ws_and_comments(src, n, arg2_s + 1);
                    arg2_e = cc__scan_to_top_level_delim(src, n, arg2_s, ')', '\0');
                    if (arg2_e >= n || src[arg2_e] != ')') {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                        scan.line, scan.col, "syntax", "unterminated @string(`...`, arena)");
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
                                : (tpl_rc == -3)
                                    ? "unterminated ${{...}} verbatim span in @string template"
                                    : "unterminated interpolation in @string template";
                            cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                            scan.line, scan.col, "syntax", msg);
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
                    if (cc_tpl_scan_literal(src, n, arg2_s, &tick_e) != 0) {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                        scan.line, scan.col, "syntax", "unterminated template literal in @string(...)");
                        free(out);
                        return (char*)-1;
                    }
                    arg3_s = cc_skip_ws_and_comments(src, n, tick_e + 1);
                    if (arg3_s >= n || src[arg3_s] != ',') {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                        scan.line, scan.col, "syntax", "@string(policy, `...`, arena) requires a trailing arena argument");
                        free(out);
                        return (char*)-1;
                    }
                    arg3_s = cc_skip_ws_and_comments(src, n, arg3_s + 1);
                    arg3_e = cc__scan_to_top_level_delim(src, n, arg3_s, ')', '\0');
                    if (arg3_e >= n || src[arg3_e] != ')') {
                        char rel[1024];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                        scan.line, scan.col, "syntax", "unterminated @string(policy, `...`, arena)");
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
                    {
                        int tpl_rc = cc__rewrite_template_body(&out, &out_len, &out_cap, src, n,
                                                               arg2_s + 1, tick_e, builder_name,
                                                               policy_name, arena_name);
                        if (tpl_rc != 0) {
                            char rel[1024];
                            const char* msg = (tpl_rc == -3)
                                ? "unterminated ${{...}} verbatim span in @string template"
                                : "unterminated interpolation in @string template";
                            cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                            scan.line, scan.col, "syntax", msg);
                            free(out);
                            return (char*)-1;
                        }
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
                                        scan.line, scan.col, "syntax", "unterminated @string(expr, arena)");
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
    }
    if (rewrite_count == 0) {
        free(out);
        return NULL;
    }
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

char* cc_rewrite_string_templates_text(const char* src, size_t n, const char* input_path) {
    char* scratch = cc__expand_string_scratch(src, n, input_path);
    char* out;
    if (scratch == (char*)-1) return (char*)-1;
    if (scratch) {
        out = cc__rewrite_string_templates(scratch, strlen(scratch), input_path);
        if (out == (char*)-1) {
            free(scratch);
            return (char*)-1;
        }
        if (!out) return scratch; /* injections only (should not happen with @string) */
        free(scratch);
        return out;
    }
    return cc__rewrite_string_templates(src, n, input_path);
}

char* cc_normalize_template_recv_chains_text(const char* src, size_t n) {
    return cc__normalize_template_recv_chains(src, n);
}

/* Lower `CC_GENERIC_FACTORY(Name[, arity]) { ... }` sugar into the canonical
 * pair of constructs the comptime collector already understands:
 *
 *   @comptime{cc_generic_register("Name",__cc_gfac_Name);}
 *   @comptime CCSlice __cc_gfac_Name(CCSlice generic_name, CCSlice mangled,
 *                                    CCSliceArray type_args, CCArena *arena)
 *   { (void)generic_name;...(void)arena; [if (type_args.len < arity || ...) ...] ... }
 *
 * The `CC_GENERIC_FACTORY(...)` token span and the body's opening `{` are
 * replaced; everything after `{` is verbatim, so body line numbers are
 * preserved.  Conveniences:
 *   - auto-void: the four implicit params are `(void)`-cast at the top of the
 *     body, so factories needn't write `(void)generic_name;` etc.;
 *   - optional arity: `CC_GENERIC_FACTORY(Name, K)` injects the standard
 *     `if (type_args.len < K || !mangled.ptr) return cc_slice_empty();` guard;
 *   - `arg(i)` accessor: a factory-TU-only macro (see the comptime prelude)
 *     expands to `type_args.items[(i)]`.
 *
 * `CC_GENERIC_FACTORY_EXTEND(Name[, arity]) { ... }` lowers to the same pair but
 * with `cc_generic_register_extend` and a unique handler symbol
 * (`__cc_gfac_ext_<Name>_<seq>`) so multiple extensions of one generic coexist
 * (the base uses the stable `__cc_gfac_<Name>`).  At a use site the seam runs
 * the base then every extension in registration order (see emit_plan.c).
 *
 * Returns NULL when the source contains no occurrence, or (char*)-1 on a
 * malformed factory header. */
static char* cc__rewrite_generic_factory(const char* src, size_t n, const char* input_path) {
    static const char kw[] = "CC_GENERIC_FACTORY";
    static const char kw_ext[] = "CC_GENERIC_FACTORY_EXTEND";
    const size_t kwlen = sizeof(kw) - 1;
    const size_t kwlen_ext = sizeof(kw_ext) - 1;
    /* Process-global so every lowered extension handler gets a unique C symbol,
       even across separately-rewritten files in one compilation. */
    static unsigned cc__gfac_ext_seq = 0;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0, last_emit = 0;
    int rewrite_count = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);

    while (i < n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        char c = src[i];
        int is_extend = 0;
        size_t mlen = 0;
        if (c == 'C') {
            /* Match the longer `..._EXTEND` keyword first; whole-ident
               boundaries keep the two from shadowing each other. */
            if (i + kwlen_ext <= n && memcmp(src + i, kw_ext, kwlen_ext) == 0 &&
                (i == 0 || !cc_is_ident_char(src[i - 1])) &&
                (i + kwlen_ext >= n || !cc_is_ident_char(src[i + kwlen_ext]))) {
                is_extend = 1; mlen = kwlen_ext;
            } else if (i + kwlen <= n && memcmp(src + i, kw, kwlen) == 0 &&
                       (i == 0 || !cc_is_ident_char(src[i - 1])) &&
                       (i + kwlen >= n || !cc_is_ident_char(src[i + kwlen]))) {
                is_extend = 0; mlen = kwlen;
            }
        }
        if (mlen) {
            const char* kwname = is_extend ? "CC_GENERIC_FACTORY_EXTEND" : "CC_GENERIC_FACTORY";
            size_t p = cc_skip_ws_and_comments(src, n, i + mlen);
            char name[128];
            size_t nl = 0;
            if (p >= n || src[p] != '(') {
                char rel[1024];
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scan.line, scan.col, "syntax",
                                "%s requires (Name) { ... }", kwname);
                free(out);
                return (char*)-1;
            }
            p = cc_skip_ws_and_comments(src, n, p + 1);
            if (p >= n || !cc_is_ident_start(src[p])) {
                char rel[1024];
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scan.line, scan.col, "syntax",
                                "%s(Name) requires an identifier name", kwname);
                free(out);
                return (char*)-1;
            }
            while (p < n && cc_is_ident_char(src[p]) && nl + 1 < sizeof(name))
                name[nl++] = src[p++];
            name[nl] = '\0';
            p = cc_skip_ws_and_comments(src, n, p);
            /* Optional arity: CC_GENERIC_FACTORY(Name, K) injects the standard
               `if (type_args.len < K || !mangled.ptr) return cc_slice_empty();`
               guard so the body needn't repeat it. */
            int arity = -1;
            if (p < n && src[p] == ',') {
                p = cc_skip_ws_and_comments(src, n, p + 1);
                if (p >= n || src[p] < '0' || src[p] > '9') {
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                    scan.line, scan.col, "syntax",
                                    "%s(%s, N) — N must be an integer arity", kwname, name);
                    free(out);
                    return (char*)-1;
                }
                arity = 0;
                while (p < n && src[p] >= '0' && src[p] <= '9')
                    arity = arity * 10 + (src[p++] - '0');
                p = cc_skip_ws_and_comments(src, n, p);
            }
            if (p >= n || src[p] != ')') {
                char rel[1024];
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                scan.line, scan.col, "syntax",
                                "%s(%s ... — expected ')' after name", kwname, name);
                free(out);
                return (char*)-1;
            }
            /* Locate the body `{` so we can inject the auto-void/guard prologue
               right after it (preserving inter-`)`-`{` whitespace, hence body
               line numbers). */
            {
                size_t brace = cc_skip_ws_and_comments(src, n, p + 1);
                if (brace >= n || src[brace] != '{') {
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                    scan.line, scan.col, "syntax",
                                    "%s(%s) requires a { ... } body", kwname, name);
                    free(out);
                    return (char*)-1;
                }
                /* The base uses the stable symbol `__cc_gfac_<Name>` (last-wins
                   registration); each extension gets a process-unique symbol so
                   many extensions of one generic can coexist in the factory TU. */
                char handler_sym[256];
                if (is_extend)
                    snprintf(handler_sym, sizeof(handler_sym), "__cc_gfac_ext_%s_%u",
                             name, cc__gfac_ext_seq++);
                else
                    snprintf(handler_sym, sizeof(handler_sym), "__cc_gfac_%s", name);
                cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "@comptime{");
                cc_sb_append_cstr(&out, &out_len, &out_cap,
                                  is_extend ? "cc_generic_register_extend(\""
                                            : "cc_generic_register(\"");
                cc_sb_append_cstr(&out, &out_len, &out_cap, name);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "\",");
                cc_sb_append_cstr(&out, &out_len, &out_cap, handler_sym);
                cc_sb_append_cstr(&out, &out_len, &out_cap, ");} @comptime CCSlice ");
                cc_sb_append_cstr(&out, &out_len, &out_cap, handler_sym);
                cc_sb_append_cstr(&out, &out_len, &out_cap,
                                  "(CCSlice generic_name, CCSlice mangled, "
                                  "CCSliceArray type_args, CCArena *arena)");
                /* Preserve the original `)`..`{` span (newlines included). */
                cc_sb_append(&out, &out_len, &out_cap, src + p + 1, brace - (p + 1));
                /* Auto-void the implicit params (unused ones won't warn; used
                   ones are unaffected) so factory bodies stay boilerplate-free. */
                cc_sb_append_cstr(&out, &out_len, &out_cap,
                                  "{ (void)generic_name;(void)mangled;(void)type_args;(void)arena; ");
                if (arity >= 0) {
                    cc__sb_append_fmt_local(&out, &out_len, &out_cap,
                                            "if (type_args.len < %d || !mangled.ptr) "
                                            "return cc_slice_empty(); ", arity);
                }
                for (size_t q = i; q <= brace; q++)
                    if (src[q] == '\n') { scan.line++; scan.col = 1; }
                rewrite_count++;
                last_emit = brace + 1;
                i = brace + 1;
                continue;
            }
        }
        i++;
    }
    if (rewrite_count == 0) {
        free(out);
        return NULL;
    }
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

char* cc_rewrite_generic_factory_text(const char* src, size_t n, const char* input_path) {
    return cc__rewrite_generic_factory(src, n, input_path);
}

int cc_scan_template_literal_end(const char* src, size_t n, size_t tick_pos, size_t* tick_end_out) {
    return cc_tpl_scan_literal(src, n, tick_pos, tick_end_out);
}

static void cc__mangle_type_name(const char* src, size_t len, char* out, size_t out_sz);
static void cc__mangle_container_type_param(const char* src, size_t len, char* out, size_t out_sz);
static int cc__slice_instance_for_elem(const char* src, size_t elem_s, size_t elem_e,
                                       char* out, size_t out_sz,
                                       char* out_prefix, size_t prefix_sz);
int cc__ufcs_method_return_type(const char* recv_type_base, const char* method,
                                const char* src, size_t n,
                                char* out, size_t out_sz);
static int cc__call_return_type(const char* fname, const char* src, size_t n,
                                char* out, size_t out_sz);
static int cc__free_call_name_is_keyword(const char* s, size_t n);
static int cc__fn_return_type(const char* src, size_t n, const char* name,
                              char* out, size_t out_sz);
static void cc__enumerate_family_variants(const char* src, size_t n,
                                          const char* prefix,
                                          char* out, size_t out_sz);
static int cc__sink_returns_result(const char* src, size_t n, const char* name);
/* Cross-pass memo: container mangled-name -> real C type spellings of its
 * parameters (see definition below for why phase-3 reparses need it). */
static void cc__ctype_memo_put(const char* mangled, const char* type1, const char* type2);
static const char* cc__ctype_memo_get_type1(const char* mangled);
static const char* cc__ctype_memo_get_type2(const char* mangled);
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
                    size_t brace_end = 0;
                    if (!cc_find_matching_brace(src, n, brace_start, &brace_end)) {
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
                            if (*p == '/' && *(p+1) == '/') { p += 2; while (*p && *p != '\n') p++; continue; }
                            if (*p == '/' && *(p+1) == '*') {
                                p += 2;
                                while (*p && !(*p == '*' && *(p+1) == '/')) p++;
                                if (*p) p += 2;
                                continue;
                            }
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

/* Backward string/char-literal skip: when a backward scan lands on a
 * `"`/`'` (the literal's CLOSING quote in source order), resolve the
 * opener with a forward scan from the line start (proper escape rules)
 * and jump `*i` to it.  Mirrors `cc__err_skip_string_or_char_backward`
 * in pass_err_syntax.c (not shareable: these files have no common
 * header for the low-level scanners). */
static void cc__pp_skip_string_or_char_backward(const char* s, size_t* i) {
    size_t close = *i;
    char qch = s[close];
    if (qch != '"' && qch != '\'') return;
    size_t line_start = close;
    while (line_start > 0 && s[line_start - 1] != '\n') line_start--;
    size_t k = line_start;
    size_t open = close;
    int in_q = 0;
    char cur_q = 0;
    while (k <= close) {
        char c = s[k];
        if (!in_q) {
            if (c == '"' || c == '\'') {
                in_q = 1;
                cur_q = c;
                open = k;
                if (k == close) break;
                k++;
                continue;
            }
            k++;
            continue;
        }
        if (c == '\\' && k + 1 <= close) {
            k += 2;
            continue;
        }
        if (c == cur_q) {
            if (k == close) {
                *i = open;
                return;
            }
            in_q = 0;
            cur_q = 0;
            k++;
            continue;
        }
        k++;
    }
    *i = open;
}

/* Backward bracket match for `)`/`]` at `close_pos`, comment- and
 * string-aware: block comments rewind whole, bracket bytes inside
 * `// ...` comments are ignored, and string/char literals are jumped
 * via the forward-verified opener.  Returns the matching opener's
 * position, or `(size_t)-1` when unmatched within (limit, close_pos]. */
static size_t cc__pp_match_bracket_backward(const char* src, size_t close_pos,
                                            size_t limit, char open_ch, char close_ch) {
    int depth = 1;
    size_t q = close_pos;
    while (q > limit && depth > 0) {
        q--;
        char cq = src[q];
        if (cq == '/' && q > 0 && src[q - 1] == '*') {
            /* End of a block comment: rewind to its opener. */
            size_t q2 = q - 1, op2 = (size_t)-1;
            while (q2 > 0) {
                q2--;
                if (src[q2] == '*' && q2 > 0 && src[q2 - 1] == '/') { op2 = q2 - 1; break; }
            }
            if (op2 != (size_t)-1) { q = op2; continue; }
        }
        if (cq == '"' || cq == '\'') {
            cc__pp_skip_string_or_char_backward(src, &q);
            continue;
        }
        if (cq == close_ch || cq == open_ch) {
            if (cc_scan_pos_in_line_comment(src, q)) continue;
            if (cq == close_ch) depth++;
            else {
                depth--;
                if (depth == 0) return q;
            }
        }
    }
    return (size_t)-1;
}

/* Scan backwards from pos to find the start of a member access chain (e.g., obj.field or ptr->field).
   Returns the start position of the full expression.
   Handles chains like a.b.c, a->b->c, (*p)->field, arr[i].field, func().field. */
/* True when src[open+1..close) is a parenthesized C type used as a cast
 * prefix: `(T)`, `(T*)`, `(struct S*)`, etc. */
static int cc__ufcs_paren_is_cast_type_only(const char* src, size_t open, size_t close) {
    size_t i;
    size_t ident_runs = 0;
    if (!src || close <= open + 1) return 0;
    i = open + 1;
    while (i < close && (src[i] == ' ' || src[i] == '\t')) i++;
    while (i < close) {
        if (cc_is_ident_start(src[i])) {
            while (i < close && cc_is_ident_char(src[i])) i++;
            ident_runs++;
            while (i < close && (src[i] == ' ' || src[i] == '\t')) i++;
            continue;
        }
        if (src[i] == '*') {
            i++;
            while (i < close && (src[i] == ' ' || src[i] == '\t' || src[i] == '*')) i++;
            continue;
        }
        return 0;
    }
    return ident_runs >= 1;
}

/* Skip backwards over horizontal whitespace AND comments, stopping at a real
 * newline.
 *
 * The receiver back-walk below is line-bounded by design: a newline before `.`
 * ends the receiver, so swapping in `cc_rskip_ws_and_comments` would newly
 * lower multi-line chains — a behaviour change well past comment inertness.
 * A block comment that itself spans a newline stops the walk for that reason,
 * so only a comment sitting within the line is filler. */
static size_t cc__rskip_hspace_and_comments(const char* src, size_t p, size_t limit) {
    for (;;) {
        size_t q = p;
        while (q > limit && (src[q - 1] == ' ' || src[q - 1] == '\t')) q--;
        if (q >= limit + 2 && src[q - 1] == '/' && src[q - 2] == '*') {
            size_t r = q - 2;
            int saw_nl = 0;
            while (r > limit + 1 && !(src[r - 2] == '/' && src[r - 1] == '*')) {
                if (src[r - 1] == '\n') saw_nl = 1;
                r--;
            }
            if (r <= limit + 1 || saw_nl) return p;
            p = r - 2;
            continue;
        }
        return q;
    }
}

static size_t cc_scan_back_for_member_access(const char* src, size_t pos, size_t limit) {
    if (pos == 0 || pos <= limit) return pos;
    
    size_t p = pos;
    p = cc__rskip_hspace_and_comments(src, p, limit);
    
    int has_access = 0;
    if (p > limit && src[p-1] == '.') {
        has_access = 1;
        p--;
    } else if (p >= 2 + limit && src[p-1] == '>' && src[p-2] == '-') {
        has_access = 1;
        p -= 2;
    }
    
    if (!has_access) return pos;
    
    p = cc__rskip_hspace_and_comments(src, p, limit);
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
            size_t q;
            size_t before;
            if (last_was_ident) break;
            q = cc__pp_match_bracket_backward(src, p - 1, limit, '(', ')');
            if (q == (size_t)-1) break;
            /* Prefix cast `(T)primary` must not join the UFCS receiver:
             *   (int64_t)((T*)(e))->field.len()
             * Call receivers `f(x)->m()` still scan through (`f` before `(`). */
            before = cc__rskip_hspace_and_comments(src, q, limit);
            if (!(before > limit && cc_is_ident_char(src[before - 1])) &&
                cc__ufcs_paren_is_cast_type_only(src, q, p - 1))
                break;
            p = q;
            last_was_ident = 0;
        } else if (src[p-1] == ']') {
            size_t q = cc__pp_match_bracket_backward(src, p - 1, limit, '[', ']');
            if (q != (size_t)-1) p = q;
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

static int cc__ufcs_fn_name_in_text(const char* src, size_t n, const char* name) {
    size_t nlen;
    if (!src || !name || !name[0]) return 0;
    nlen = strlen(name);
    if (nlen == 0 || nlen >= n) return 0;
    for (size_t i = 0; i + nlen < n; ++i) {
        if (src[i] != name[0]) continue;
        if (memcmp(src + i, name, nlen) != 0) continue;
        if (i > 0 && cc_is_ident_char(src[i - 1])) continue;
        {
            size_t j = i + nlen;
            if (j < n && cc_is_ident_char(src[j])) continue;
            j = cc_skip_ws_and_comments(src, n, j);
            if (j < n && src[j] == '(') return 1;
        }
    }
    return 0;
}

static int cc__match_kw_at(const char* src, size_t n, size_t pos, const char* kw) {
    size_t klen;
    if (!src || !kw) return 0;
    klen = strlen(kw);
    if (pos + klen > n || memcmp(src + pos, kw, klen) != 0) return 0;
    if (pos > 0 && cc_is_ident_char(src[pos - 1])) return 0;
    if (pos + klen < n && cc_is_ident_char(src[pos + klen])) return 0;
    return 1;
}

/* Once-per-buffer index of types declared via CC_MAP_DECL_UFCS /
 * CC_ARRAY_MAP_DECL_UFCS (and their __cc_* marker spellings).  Text UFCS
 * used to re-scan the whole TU for every `.method(` site; redis pays that
 * with zero user map decls. */
typedef struct {
    const char* src;
    size_t n;
    char** names;
    size_t* lens;
    size_t count;
    size_t cap;
    int built;
} CCMapUfcsDeclIndex;

static _Thread_local CCMapUfcsDeclIndex g_map_ufcs_decl_idx;

static void cc__map_ufcs_decl_idx_reset(void) {
    size_t i;
    for (i = 0; i < g_map_ufcs_decl_idx.count; i++) free(g_map_ufcs_decl_idx.names[i]);
    free(g_map_ufcs_decl_idx.names);
    free(g_map_ufcs_decl_idx.lens);
    memset(&g_map_ufcs_decl_idx, 0, sizeof(g_map_ufcs_decl_idx));
}

static int cc__map_ufcs_decl_idx_push(const char* name, size_t nlen) {
    char* copy;
    size_t i;
    if (!name || nlen == 0) return 0;
    for (i = 0; i < g_map_ufcs_decl_idx.count; i++) {
        if (g_map_ufcs_decl_idx.lens[i] == nlen &&
            memcmp(g_map_ufcs_decl_idx.names[i], name, nlen) == 0)
            return 0;
    }
    if (g_map_ufcs_decl_idx.count == g_map_ufcs_decl_idx.cap) {
        size_t cap = g_map_ufcs_decl_idx.cap ? g_map_ufcs_decl_idx.cap * 2 : 8;
        char** nn = (char**)realloc(g_map_ufcs_decl_idx.names, cap * sizeof(*nn));
        size_t* nl = (size_t*)realloc(g_map_ufcs_decl_idx.lens, cap * sizeof(*nl));
        if (!nn || !nl) {
            free(nn);
            free(nl);
            return -1;
        }
        g_map_ufcs_decl_idx.names = nn;
        g_map_ufcs_decl_idx.lens = nl;
        g_map_ufcs_decl_idx.cap = cap;
    }
    copy = (char*)malloc(nlen + 1);
    if (!copy) return -1;
    memcpy(copy, name, nlen);
    copy[nlen] = '\0';
    g_map_ufcs_decl_idx.names[g_map_ufcs_decl_idx.count] = copy;
    g_map_ufcs_decl_idx.lens[g_map_ufcs_decl_idx.count] = nlen;
    g_map_ufcs_decl_idx.count++;
    return 0;
}

static void cc__map_ufcs_decl_idx_build(const char* src, size_t n) {
    static const char* const kws[] = { "CC_MAP_DECL_UFCS", "CC_ARRAY_MAP_DECL_UFCS" };
    static const char* const markers[] = { "__cc_map_decl_ufcs__", "__cc_array_map_decl_ufcs__" };
    size_t ki, mi, i;
    cc__map_ufcs_decl_idx_reset();
    g_map_ufcs_decl_idx.src = src;
    g_map_ufcs_decl_idx.n = n;
    g_map_ufcs_decl_idx.built = 1;
    if (!src || n == 0) return;
    for (ki = 0; ki < sizeof(kws) / sizeof(kws[0]); ++ki) {
        size_t kwlen = strlen(kws[ki]);
        for (i = 0; i + kwlen < n; ++i) {
            size_t p, ident_start, ident_end;
            if (!cc__match_kw_at(src, n, i, kws[ki])) continue;
            p = cc_skip_ws_and_comments(src, n, i + kwlen);
            if (p >= n || src[p] != '(') continue;
            p = cc_skip_ws_and_comments(src, n, p + 1);
            ident_start = p;
            if (p >= n || !cc_is_ident_start(src[p])) continue;
            p++;
            while (p < n && cc_is_ident_char(src[p])) p++;
            ident_end = p;
            p = cc_skip_ws_and_comments(src, n, p);
            if (p >= n || src[p] != ')') continue;
            if (ident_end > ident_start)
                (void)cc__map_ufcs_decl_idx_push(src + ident_start, ident_end - ident_start);
            i = ident_end ? ident_end - 1 : i;
        }
    }
    for (mi = 0; mi < sizeof(markers) / sizeof(markers[0]); ++mi) {
        size_t mpre = strlen(markers[mi]);
        for (i = 0; i + mpre < n; ++i) {
            size_t j;
            if (memcmp(src + i, markers[mi], mpre) != 0) continue;
            if (i > 0 && cc_is_ident_char(src[i - 1])) continue;
            j = i + mpre;
            if (j >= n || !cc_is_ident_start(src[j])) continue;
            while (j < n && cc_is_ident_char(src[j])) j++;
            if (j + 1 < n && cc_is_ident_char(src[j])) continue;
            (void)cc__map_ufcs_decl_idx_push(src + i + mpre, j - (i + mpre));
            i = j ? j - 1 : i;
        }
    }
}

static int cc__source_declares_map_ufcs(const char* src, size_t n, const char* type_name) {
    char base[128];
    size_t tlen;
    size_t i;
    if (!src || !type_name || !type_name[0]) return 0;
    while (*type_name == ' ' || *type_name == '\t') type_name++;
    if (strncmp(type_name, "struct ", 7) == 0) type_name += 7;
    else if (strncmp(type_name, "union ", 6) == 0) type_name += 6;
    tlen = strlen(type_name);
    while (tlen > 0 && (type_name[tlen - 1] == '*' || type_name[tlen - 1] == ' ' || type_name[tlen - 1] == '\t')) tlen--;
    if (tlen == 0 || tlen >= sizeof(base)) return 0;
    memcpy(base, type_name, tlen);
    base[tlen] = '\0';
    if (!g_map_ufcs_decl_idx.built || g_map_ufcs_decl_idx.src != src ||
        g_map_ufcs_decl_idx.n != n)
        cc__map_ufcs_decl_idx_build(src, n);
    if (g_map_ufcs_decl_idx.count == 0) return 0;
    for (i = 0; i < g_map_ufcs_decl_idx.count; i++) {
        if (g_map_ufcs_decl_idx.lens[i] == tlen &&
            memcmp(g_map_ufcs_decl_idx.names[i], base, tlen) == 0)
            return 1;
    }
    return 0;
}

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
            strcmp(method_name, "has") == 0 ||
            strcmp(method_name, "index_of") == 0 ||
            strcmp(method_name, "last_index_of") == 0 ||
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
    if ((size_t)(base_end - start) > 8 && memcmp(start, "CCVec::[", 8) == 0 && base_end[-1] == ']') {
        size_t prefix = 8;
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
    if ((size_t)(base_end - start) > 11 && memcmp(start, "ArrayMap::[", 11) == 0 &&
        base_end[-1] == ']') {
        size_t inner_len = (size_t)(base_end - (start + 11) - 1);
        snprintf(out, out_sz, "__CC_ARRAY_MAP(%.*s)%.*s",
                 (int)inner_len, start + 11, ptr_count, "********");
        return;
    }
    if (((size_t)(base_end - start) > 4 && memcmp(start, "Map<", 4) == 0 && base_end[-1] == '>') ||
        ((size_t)(base_end - start) > 6 && memcmp(start, "Map::[", 6) == 0 && base_end[-1] == ']')) {
        size_t prefix = (start[3] == ':') ? 6 : 4;
        size_t inner_len = (size_t)(base_end - (start + prefix) - 1);
        snprintf(out, out_sz, "__CC_MAP(%.*s)%.*s",
                 (int)inner_len, start + prefix, ptr_count, "********");
        return;
    }
    if ((size_t)(base_end - start) > 15 && memcmp(start, "__CC_ARRAY_MAP(", 15) == 0 &&
        base_end[-1] == ')') {
        const char* inner_s = start + 15;
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
                snprintf(out, out_sz, "ArrayMap_%s_%s%.*s", mangled_key, mangled_val,
                         ptr_count, "********");
                return;
            }
        }
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
    {
        size_t base_len = (size_t)(base_end - start);
        CCTypeRegistry* reg = cc_type_registry_get_global();
        char base_name[256];
        const char* alias = NULL;
        if (base_len >= out_sz) base_len = out_sz - 1;
        if (base_len >= sizeof(base_name)) base_len = sizeof(base_name) - 1;
        memcpy(base_name, start, base_len);
        base_name[base_len] = '\0';
        if (reg && (alias = cc_type_registry_lookup_alias(reg, base_name)) && *alias)
            snprintf(out, out_sz, "%s", alias);
        else
            snprintf(out, out_sz, "%s", base_name);
        while (ptr_count-- > 0 && strlen(out) + 1 < out_sz) strcat(out, "*");
    }
}

static int cc__type_is_parser_vec(const char* type_name) {
    return cc_ufcs_type_is_parser_vec(type_name);
}

static int cc__type_is_parser_map(const char* type_name) {
    return cc_ufcs_type_is_parser_map(type_name);
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

/* One-pass scoped decl/typedef index for UFCS receiver resolution.
 * `cc__lookup_scoped_ufcs_var_type` / `cc__lookup_scoped_type_alias` used to
 * rescan src[0..use_offset) on every call site; redis-sized TUs make that
 * the dominant cost inside `cc__resolve_generic_ufcs_receiver_type`. */
typedef struct {
    size_t pos; /* terminating ';' */
    int scope_id;
    char name[128];
    char type[256];
} CCUfcsIdxDecl;

typedef struct {
    size_t pos; /* terminating ';' */
    char name[128];
    char type[256];
} CCUfcsIdxTypedef;

typedef struct {
    const char* src;
    size_t n;
    CCUfcsIdxDecl* decls;
    size_t n_decls;
    size_t decls_cap;
    CCUfcsIdxTypedef* typedefs;
    size_t n_typedefs;
    size_t typedefs_cap;
    size_t* scope_close; /* close pos per scope_id; (size_t)-1 if open at EOF */
    size_t n_scopes;
    size_t scopes_cap;
    int ready;
} CCUfcsScopeIndex;

static CCUfcsScopeIndex g_ufcs_scope_idx;

static void cc__ufcs_scope_idx_reset(void) {
    free(g_ufcs_scope_idx.decls);
    free(g_ufcs_scope_idx.typedefs);
    free(g_ufcs_scope_idx.scope_close);
    memset(&g_ufcs_scope_idx, 0, sizeof(g_ufcs_scope_idx));
}

static int cc__ufcs_scope_idx_ensure_scopes(size_t need) {
    size_t* nv;
    size_t cap;
    size_t i;
    if (g_ufcs_scope_idx.scopes_cap >= need) return 0;
    cap = g_ufcs_scope_idx.scopes_cap ? g_ufcs_scope_idx.scopes_cap : 64;
    while (cap < need) cap *= 2;
    nv = (size_t*)realloc(g_ufcs_scope_idx.scope_close, cap * sizeof(*nv));
    if (!nv) return -1;
    for (i = g_ufcs_scope_idx.scopes_cap; i < cap; i++) nv[i] = (size_t)-1;
    g_ufcs_scope_idx.scope_close = nv;
    g_ufcs_scope_idx.scopes_cap = cap;
    return 0;
}

static const char* cc__ufcs_idx_typedef_before(size_t limit,
                                               const char* alias_name,
                                               char* out_type,
                                               size_t out_type_sz) {
    size_t i;
    if (!alias_name || !alias_name[0] || !out_type || out_type_sz == 0) return NULL;
    out_type[0] = '\0';
    if (cc_ufcs_type_is_known_family_base(alias_name)) return NULL;
    for (i = 0; i < g_ufcs_scope_idx.n_typedefs; i++) {
        const CCUfcsIdxTypedef* td = &g_ufcs_scope_idx.typedefs[i];
        if (td->pos >= limit) break;
        if (strcmp(td->name, alias_name) != 0) continue;
        snprintf(out_type, out_type_sz, "%s", td->type);
        return out_type;
    }
    return NULL;
}

/* Build scoped-decl/typedef index, and optionally harvest UFCS vars/fields in
 * the same pass (avoids a second full-file walk in the rewrite). */
static void cc__ufcs_scope_idx_build_ex(const char* src, size_t n,
                                        CCUfcsVarInfo* vars, size_t* var_count,
                                        size_t var_cap, CCUfcsFieldInfo* fields,
                                        size_t* field_count, size_t field_cap) {
    CCTypeRegistry* reg = cc_type_registry_get_global();
    enum { MAX_SCOPES = 256 };
    int scope_stack[MAX_SCOPES];
    int scope_depth = 1;
    int next_scope_id = 1;
    CCScannerState scan;
    size_t stmt_start = 0;
    size_t typedef_start = (size_t)-1;
    size_t i = 0;
    int harvest = vars && var_count && fields && field_count;
    cc__ufcs_scope_idx_reset();
    if (!src || n == 0) return;
    g_ufcs_scope_idx.src = src;
    g_ufcs_scope_idx.n = n;
    scope_stack[0] = 0;
    if (cc__ufcs_scope_idx_ensure_scopes(1) != 0) return;
    g_ufcs_scope_idx.n_scopes = 1;
    g_ufcs_scope_idx.scope_close[0] = (size_t)-1;
    if (harvest) {
        *var_count = 0;
        *field_count = 0;
    }
    cc_scanner_init(&scan);
    while (i < n) {
        char c;
        /* Directive-aware: a brace in a #define body must not corrupt
         * the scope table for the whole TU. */
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        c = src[i];
        if (typedef_start == (size_t)-1 &&
            i + 7 <= n &&
            memcmp(src + i, "typedef", 7) == 0 &&
            (i == 0 || !cc_is_ident_char(src[i - 1])) &&
            (i + 7 == n || !cc_is_ident_char(src[i + 7]))) {
            typedef_start = i;
        }
        /* typedef struct { fields } Name — field harvest (same as collect). */
        if (harvest && i + 6 <= n && memcmp(src + i, "typedef", 7) == 0 &&
            !cc_is_ident_char(src[i + 7])) {
            size_t j = cc_skip_ws_and_comments(src, n, i + 7);
            if (j + 5 <= n && memcmp(src + j, "struct", 6) == 0 &&
                !cc_is_ident_char(src[j + 6])) {
                size_t body_l = cc_skip_ws_and_comments(src, n, j + 6);
                size_t body_r = 0;
                if (body_l < n && cc_is_ident_start(src[body_l])) {
                    size_t tag_end = body_l;
                    while (tag_end < n && cc_is_ident_char(src[tag_end])) tag_end++;
                    body_l = cc_skip_ws_and_comments(src, n, tag_end);
                }
                if (body_l < n && src[body_l] == '{' &&
                    cc_find_matching_brace(src, n, body_l, &body_r)) {
                    size_t name_pos = cc_skip_ws_and_comments(src, n, body_r + 1);
                    if (name_pos < n && cc_is_ident_start(src[name_pos])) {
                        char struct_name[128];
                        size_t sn = 0;
                        size_t p = name_pos;
                        const char* stmt;
                        const char* body_end;
                        while (p < n && cc_is_ident_char(src[p])) {
                            if (sn + 1 < sizeof(struct_name)) struct_name[sn] = src[p];
                            sn++;
                            p++;
                        }
                        struct_name[sn < sizeof(struct_name) ? sn : sizeof(struct_name) - 1] = '\0';
                        stmt = src + body_l + 1;
                        body_end = src + body_r;
                        while (stmt < body_end) {
                            size_t stmt_off = (size_t)(stmt - src);
                            size_t end_off = (size_t)(body_end - src);
                            size_t semi_off =
                                cc_find_char_top_level(src, stmt_off, end_off, ';');
                            char field_name[128];
                            char field_type[256];
                            int field_is_as = 0;
                            const char* semi;
                            if (semi_off >= end_off) break;
                            semi = src + semi_off;
                            cc_parse_decl_name_and_type_ex(
                                stmt, semi, field_name, sizeof(field_name),
                                field_type, sizeof(field_type), &field_is_as);
                            if (field_name[0] && field_type[0]) {
                                cc__record_ufcs_field(fields, field_count, field_cap,
                                                      struct_name, field_name, field_type);
                                if (reg)
                                    (void)cc_type_registry_add_field_ex(
                                        reg, struct_name, field_name, field_type,
                                        field_is_as);
                            }
                            stmt = semi + 1;
                        }
                    }
                }
            }
        }
        if (c == '{') {
            /* An initializer brace — `Foo p = {41};`, `x = (Foo){...};` —
             * is an expression, not a scope: a `{` after the statement's
             * top-level `=` must not reset the statement head, or the
             * declaration never gets indexed and the receiver falls to
             * the flat table (where an unrelated function's parameter of
             * the same name can win). Skip the balanced group whole. */
            {
                size_t eqp = 0;
                if (cc__pp_find_top_level_equal(src, stmt_start, i, &eqp)) {
                    size_t be = 0;
                    if (cc_find_matching_brace(src, n, i, &be)) {
                        i = be + 1;
                        continue;
                    }
                }
            }
            if (harvest) {
                size_t close = cc_rskip_ws_and_comments(src, i);
                if (close > 0 && src[close - 1] == ')') {
                    size_t open = close - 1;
                    int depth = 1;
                    while (open > 0 && depth > 0) {
                        open--;
                        if (src[open] == '/' && open > 0 && src[open - 1] == '*') {
                            size_t q2 = open - 1, op2 = (size_t)-1;
                            while (q2 > 0) {
                                q2--;
                                if (src[q2] == '*' && q2 > 0 && src[q2 - 1] == '/') {
                                    op2 = q2 - 1;
                                    break;
                                }
                            }
                            if (op2 != (size_t)-1) { open = op2; continue; }
                        }
                        if (src[open] == ')' || src[open] == '(') {
                            if (cc_scan_pos_in_line_comment(src, open)) continue;
                            if (src[open] == ')') depth++;
                            else depth--;
                        }
                    }
                    if (depth == 0) {
                        size_t param_start = open + 1;
                        size_t p = param_start;
                        int par = 0, br = 0;
                        while (p <= close - 1) {
                            if (p == close - 1 || (src[p] == ',' && par == 0 && br == 0)) {
                                char decl_name[128];
                                char decl_type[256];
                                cc_parse_decl_name_and_type(
                                    src + param_start, src + p, decl_name,
                                    sizeof(decl_name), decl_type, sizeof(decl_type));
                                if (decl_name[0] && strcmp(decl_type, "void") != 0 &&
                                    !cc_is_non_decl_stmt_type(decl_type)) {
                                    cc__record_ufcs_var(vars, var_count, var_cap,
                                                        decl_name, decl_type);
                                    if (reg && !cc_type_registry_lookup_var(reg, decl_name))
                                        cc_type_registry_add_var(reg, decl_name, decl_type);
                                }
                                param_start = p + 1;
                            } else if (src[p] == '(') par++;
                            else if (src[p] == ')' && par > 0) par--;
                            else if (src[p] == '[') br++;
                            else if (src[p] == ']' && br > 0) br--;
                            p++;
                        }
                    }
                }
            }
            if (scope_depth < MAX_SCOPES) {
                int sid = next_scope_id++;
                scope_stack[scope_depth++] = sid;
                if (cc__ufcs_scope_idx_ensure_scopes((size_t)sid + 1) != 0) return;
                if ((size_t)sid + 1 > g_ufcs_scope_idx.n_scopes)
                    g_ufcs_scope_idx.n_scopes = (size_t)sid + 1;
                g_ufcs_scope_idx.scope_close[sid] = (size_t)-1;
            }
            stmt_start = i + 1;
            i++;
            continue;
        }
        if (c == '}') {
            if (scope_depth > 1) {
                int closing_scope = scope_stack[scope_depth - 1];
                if ((size_t)closing_scope < g_ufcs_scope_idx.scopes_cap)
                    g_ufcs_scope_idx.scope_close[closing_scope] = i;
                scope_depth--;
            }
            stmt_start = i + 1;
            i++;
            continue;
        }
        if (c == ';') {
            char decl_name[128];
            char decl_type[256];
            size_t stmt_eff = cc_skip_ws_and_comments(src, i, stmt_start);
            int stmt_has_member_access =
                cc__stmt_head_has_member_access(src + stmt_eff, src + i);
            if (typedef_start != (size_t)-1) {
                char td_name[128];
                char td_type[256];
                if (cc__parse_typedef_alias_stmt(src + typedef_start, src + i,
                                                 td_name, sizeof(td_name),
                                                 td_type, sizeof(td_type)) &&
                    td_name[0] &&
                    /* Family/canonical spellings are not alias keys. */
                    !cc_ufcs_type_is_known_family_base(td_name)) {
                    CCUfcsIdxTypedef* slot;
                    char normalized[256];
                    if (g_ufcs_scope_idx.n_typedefs == g_ufcs_scope_idx.typedefs_cap) {
                        size_t cap = g_ufcs_scope_idx.typedefs_cap
                                         ? g_ufcs_scope_idx.typedefs_cap * 2
                                         : 32;
                        CCUfcsIdxTypedef* nv = (CCUfcsIdxTypedef*)realloc(
                            g_ufcs_scope_idx.typedefs, cap * sizeof(*nv));
                        if (!nv) return;
                        g_ufcs_scope_idx.typedefs = nv;
                        g_ufcs_scope_idx.typedefs_cap = cap;
                    }
                    slot = &g_ufcs_scope_idx.typedefs[g_ufcs_scope_idx.n_typedefs++];
                    slot->pos = i;
                    snprintf(slot->name, sizeof(slot->name), "%s", td_name);
                    cc__canonicalize_ufcs_alias_target(slot->type, sizeof(slot->type),
                                                       td_type);
                    if (harvest && reg) {
                        cc__canonicalize_ufcs_alias_target(normalized, sizeof(normalized),
                                                           td_type);
                        if (normalized[0])
                            cc_type_registry_add_alias(reg, td_name, normalized);
                    }
                }
                typedef_start = (size_t)-1;
            }
            /* Same decl harvest as the old per-call scoped walk (including
             * field-shaped semis inside typedef struct bodies). */
            cc_parse_decl_name_and_type(src + stmt_eff, src + i,
                                         decl_name, sizeof(decl_name),
                                         decl_type, sizeof(decl_type));
            if ((!decl_name[0] || !decl_type[0]) && !stmt_has_member_access) {
                (void)cc__parse_decl_name_and_type_fallback(
                    src + stmt_eff, src + i, decl_name, sizeof(decl_name),
                    decl_type, sizeof(decl_type));
            }
            if (decl_name[0] &&
                !stmt_has_member_access &&
                !cc_is_non_decl_stmt_type(decl_type)) {
                CCUfcsIdxDecl* slot;
                char alias_type[256];
                const char* alias = NULL;
                if (g_ufcs_scope_idx.n_decls == g_ufcs_scope_idx.decls_cap) {
                    size_t cap = g_ufcs_scope_idx.decls_cap
                                     ? g_ufcs_scope_idx.decls_cap * 2
                                     : 128;
                    CCUfcsIdxDecl* nv = (CCUfcsIdxDecl*)realloc(
                        g_ufcs_scope_idx.decls, cap * sizeof(*nv));
                    if (!nv) return;
                    g_ufcs_scope_idx.decls = nv;
                    g_ufcs_scope_idx.decls_cap = cap;
                }
                slot = &g_ufcs_scope_idx.decls[g_ufcs_scope_idx.n_decls];
                slot->pos = i;
                slot->scope_id = scope_stack[scope_depth - 1];
                snprintf(slot->name, sizeof(slot->name), "%s", decl_name);
                cc__normalize_ufcs_type_name(slot->type, sizeof(slot->type),
                                             decl_type);
                if (reg)
                    alias = cc_type_registry_lookup_alias(reg, slot->type);
                if (alias && *alias)
                    snprintf(slot->type, sizeof(slot->type), "%s", alias);
                if (cc__ufcs_idx_typedef_before(i, slot->type, alias_type,
                                               sizeof(alias_type)))
                    snprintf(slot->type, sizeof(slot->type), "%s", alias_type);
                g_ufcs_scope_idx.n_decls++;
                if (harvest) {
                    cc__record_ufcs_var(vars, var_count, var_cap, slot->name, slot->type);
                    if (reg && !cc_type_registry_lookup_var(reg, slot->name))
                        cc_type_registry_add_var(reg, slot->name, slot->type);
                }
            }
            stmt_start = i + 1;
        }
        /* Type + var pattern harvest (Vec/Map macros, struct tags, …). */
        if (harvest && cc_is_ident_start(src[i]) &&
            !(i > 0 && (cc_is_ident_char(src[i - 1]) || src[i - 1] == '@'))) {
            size_t type_start = i;
            size_t type_end;
            size_t j;
            while (i < n && cc_is_ident_char(src[i])) i++;
            type_end = i;
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
                if (macro_l < n && src[macro_l] == '(' &&
                    cc_find_matching_paren(src, n, macro_l, &macro_r))
                    type_end = macro_r + 1;
            } else if (type_end - type_start == sizeof("__CC_MAP") - 1 &&
                       memcmp(src + type_start, "__CC_MAP", sizeof("__CC_MAP") - 1) == 0) {
                size_t macro_l = cc_skip_ws_and_comments(src, n, type_end);
                size_t macro_r = 0;
                if (macro_l < n && src[macro_l] == '(' &&
                    cc_find_matching_paren(src, n, macro_l, &macro_r))
                    type_end = macro_r + 1;
            }
            j = cc_skip_ws_and_comments(src, n, type_end);
            while (j < n && src[j] == '*') {
                j++;
                j = cc_skip_ws_and_comments(src, n, j);
            }
            if (j < n && cc_is_ident_start(src[j])) {
                size_t var_start = j;
                while (j < n && cc_is_ident_char(src[j])) j++;
                if (cc_skip_ws_and_comments(src, n, j) < n &&
                    src[cc_skip_ws_and_comments(src, n, j)] != '(') {
                    char type_name[256];
                    char var_name[128];
                    size_t tn = type_end - type_start;
                    size_t vn = j - var_start;
                    size_t k;
                    if (tn >= sizeof(type_name)) tn = sizeof(type_name) - 1;
                    if (vn >= sizeof(var_name)) vn = sizeof(var_name) - 1;
                    memcpy(type_name, src + type_start, tn);
                    type_name[tn] = '\0';
                    memcpy(var_name, src + var_start, vn);
                    var_name[vn] = '\0';
                    k = cc_skip_ws_and_comments(src, n, type_end);
                    while (k < var_start &&
                           (src[k] == '*' || src[k] == ' ' || src[k] == '\t')) {
                        if (src[k] == '*')
                            strncat(type_name, "*",
                                    sizeof(type_name) - strlen(type_name) - 1);
                        k++;
                    }
                    cc__record_ufcs_var(vars, var_count, var_cap, var_name, type_name);
                    if (reg && !cc_type_registry_lookup_var(reg, var_name))
                        cc_type_registry_add_var(reg, var_name, type_name);
                }
            }
            continue;
        }
        i++;
    }
    g_ufcs_scope_idx.ready = 1;
}

static void __attribute__((unused)) cc__ufcs_scope_idx_build(const char* src,
                                                             size_t n) {
    cc__ufcs_scope_idx_build_ex(src, n, NULL, NULL, 0, NULL, NULL, 0);
}

static const char* cc__lookup_scoped_ufcs_var_type(const char* src,
                                                   size_t limit,
                                                   const char* var_name,
                                                   char* out_type,
                                                   size_t out_type_sz) {
    size_t di;
    const CCUfcsIdxDecl* best = NULL;
    if (!src || !var_name || !var_name[0] || !out_type || out_type_sz == 0) return NULL;
    out_type[0] = '\0';
    if (!g_ufcs_scope_idx.ready || g_ufcs_scope_idx.src != src) return NULL;
    for (di = 0; di < g_ufcs_scope_idx.n_decls; di++) {
        const CCUfcsIdxDecl* d = &g_ufcs_scope_idx.decls[di];
        size_t close;
        if (d->pos >= limit) break;
        if (strcmp(d->name, var_name) != 0) continue;
        if ((size_t)d->scope_id >= g_ufcs_scope_idx.n_scopes) continue;
        close = g_ufcs_scope_idx.scope_close[d->scope_id];
        /* Original walk processes bytes [0, limit). A `}` at `close` is
         * applied only when close < limit, so the scope is still open when
         * close >= limit. */
        if (close < limit) continue;
        best = d;
    }
    if (!best) return NULL;
    strncpy(out_type, best->type, out_type_sz - 1);
    out_type[out_type_sz - 1] = '\0';
    if (getenv("CC_DEBUG_SCOPED_VAR"))
        fprintf(stderr, "scoped_var: '%s' -> '%s' (indexed)\n", var_name, out_type);
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
    /* Comment-aware trims: `typedef T Alias / *c* / ;` and
     * `typedef T / *c* / Alias;` must still harvest `Alias` / `T`. */
    s = stmt_start + cc_skip_ws_and_comments(stmt_start, (size_t)(stmt_end - stmt_start), 0);
    e = stmt_start + cc_rskip_ws_and_comments(stmt_start, (size_t)(stmt_end - stmt_start));
    if (e <= s) return 0;
    if ((size_t)(e - s) < 7 || memcmp(s, "typedef", 7) != 0) return 0;
    type_start = s + 7;
    type_start = stmt_start + cc_skip_ws_and_comments(stmt_start, (size_t)(e - stmt_start),
                                                      (size_t)(type_start - stmt_start));
    if (type_start >= e) return 0;
    alias_end = stmt_start + cc_rskip_ws_and_comments(stmt_start, (size_t)(e - stmt_start));
    alias_start = alias_end;
    while (alias_start > type_start && cc_is_ident_char(alias_start[-1])) alias_start--;
    if (alias_start == alias_end || alias_start < type_start || !cc_is_ident_start(*alias_start)) return 0;
    type_end = stmt_start + cc_rskip_ws_and_comments(stmt_start, (size_t)(alias_start - stmt_start));
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
    /* Comment-aware trims: `int x / *c* / ;` must harvest `x`, not a
     * trailing comment token. */
    s = stmt_start + cc_skip_ws_and_comments(stmt_start, (size_t)(stmt_end - stmt_start), 0);
    e = stmt_start + cc_rskip_ws_and_comments(stmt_start, (size_t)(stmt_end - stmt_start));
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
    scan_end = stmt_start + cc_rskip_ws_and_comments(stmt_start, (size_t)(scan_end - stmt_start));
    if (scan_end <= s) return 0;
    name_end = scan_end;
    while (name_end > s && !cc_is_ident_char(name_end[-1])) name_end--;
    name_start = name_end;
    while (name_start > s && cc_is_ident_char(name_start[-1])) name_start--;
    if (name_start == name_end || !cc_is_ident_start(*name_start)) return 0;
    type_end = stmt_start + cc_rskip_ws_and_comments(stmt_start, (size_t)(name_start - stmt_start));
    if (type_end <= s) return 0;
    {
        size_t name_len = (size_t)(name_end - name_start);
        size_t type_len = (size_t)(type_end - s);
        const char* before;
        /* Reject call arguments (`fn(&name)` / `fn(name)`): the last ident is
         * not a declarator.  Primary `cc_parse_decl_name_and_type` already
         * rejects these; this fallback must not invent a poison type. */
        before = name_start;
        while (before > s && isspace((unsigned char)before[-1])) before--;
        if (before > s && (before[-1] == '&' || before[-1] == '(' || before[-1] == ',')) {
            return 0;
        }
        if (name_len >= decl_name_sz) name_len = decl_name_sz - 1;
        if (type_len >= decl_type_sz) type_len = decl_type_sz - 1;
        memcpy(decl_name, name_start, name_len);
        decl_name[name_len] = '\0';
        memcpy(decl_type, s, type_len);
        decl_type[type_len] = '\0';
        if (cc_is_non_decl_stmt_type(decl_type)) {
            decl_name[0] = '\0';
            decl_type[0] = '\0';
            return 0;
        }
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
    CCScannerState scan;
    if (!src || !alias_name || !alias_name[0] || !out_type || out_type_sz == 0) return NULL;
    out_type[0] = '\0';
    if (cc_ufcs_type_is_known_family_base(alias_name)) return NULL;
    if (g_ufcs_scope_idx.ready && g_ufcs_scope_idx.src == src)
        return cc__ufcs_idx_typedef_before(limit, alias_name, out_type, out_type_sz);
    cc_scanner_init(&scan);
    while (i < limit) {
        char c;
        if (cc_scanner_skip_non_code(&scan, src, limit, &i)) continue;
        c = src[i];
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
            size_t close = cc_rskip_ws_and_comments(src, i);
            if (close > 0 && src[close - 1] == ')') {
                size_t open = close - 1;
                int depth = 1;
                while (open > 0 && depth > 0) {
                    open--;
                    /* Comment-aware: rewind block comments whole and
                     * ignore parens inside `// ...` comments so a
                     * parenthetical in a header comment cannot desync
                     * the param-list match. */
                    if (src[open] == '/' && open > 0 && src[open - 1] == '*') {
                        size_t q2 = open - 1, op2 = (size_t)-1;
                        while (q2 > 0) {
                            q2--;
                            if (src[q2] == '*' && q2 > 0 && src[q2 - 1] == '/') { op2 = q2 - 1; break; }
                        }
                        if (op2 != (size_t)-1) { open = op2; continue; }
                    }
                    if (src[open] == ')' || src[open] == '(') {
                        if (cc_scan_pos_in_line_comment(src, open)) continue;
                        if (src[open] == ')') depth++;
                        else depth--;
                    }
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
                            /* Top-level ';' only — memchr would split on ';'
                             * inside a trailing block comment on the previous
                             * field and poison the next field's registered type
                             * (e.g. ReplyTx → garbage → cc_arena_try_send_into). */
                            while (stmt < body_end) {
                                size_t stmt_off = (size_t)(stmt - src);
                                size_t end_off = (size_t)(body_end - src);
                                size_t semi_off = cc_find_char_top_level(src, stmt_off, end_off, ';');
                                if (semi_off >= end_off) break;
                                const char* semi = src + semi_off;
                                char field_name[128];
                                char field_type[256];
                                int field_is_as = 0;
                                cc_parse_decl_name_and_type_ex(stmt, semi, field_name, sizeof(field_name),
                                                               field_type, sizeof(field_type), &field_is_as);
                                if (field_name[0] && field_type[0]) {
                                    cc__record_ufcs_field(fields, field_count, field_cap, struct_name, field_name, field_type);
                                    if (reg) {
                                        (void)cc_type_registry_add_field_ex(reg, struct_name, field_name,
                                                                            field_type, field_is_as);
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
                                           size_t out_recv_expr_sz,
                                           size_t* out_targ_a,
                                           size_t* out_targ_b) {
    size_t sep_pos;
    size_t method_start;
    size_t method_end;
    size_t recv_start;
    size_t paren_pos;
    size_t paren_end = 0;
    if (out_targ_a) *out_targ_a = 0;
    if (out_targ_b) *out_targ_b = 0;
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
    /* `::[...]` specializes the name it follows — member position:
     * `recv.method::[T](args)`. The bracket span is reported to the
     * caller; resolution decides whether the member has a type formal. */
    if (paren_pos + 1 < n && src[paren_pos] == ':' && src[paren_pos + 1] == ':') {
        size_t br = cc_skip_ws_and_comments(src, n, paren_pos + 2);
        size_t rb = 0;
        if (br >= n || src[br] != '[' ||
            !cc__find_matching_bracket(src, n, br, &rb))
            return 0;
        if (out_targ_a) *out_targ_a = br + 1;
        if (out_targ_b) *out_targ_b = rb;
        paren_pos = cc_skip_ws_and_comments(src, n, rb + 1);
    }
    if (paren_pos >= n || src[paren_pos] != '(') return 0;
    if (!cc_find_matching_paren(src, n, paren_pos, &paren_end)) return 0;
    recv_start = cc_scan_back_for_member_access(src, method_start, 0);
    if (recv_start >= sep_pos || sep_pos - recv_start >= out_recv_expr_sz) return 0;
    {
        const char* recv_s = src + recv_start;
        const char* recv_e;
        /* A comment between the receiver and its `.` is not part of the
         * receiver expression.  Leaving it in ended the captured text with a
         * comment close rather than a paren, so the call-receiver chain hoist
         * declined and the hop was emitted unlowered. */
        size_t recv_end = cc__rskip_hspace_and_comments(src, sep_pos, recv_start);
        recv_e = src + (recv_end > recv_start ? recv_end : sep_pos);
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

/* Parse a C cast primary `(T*)(e)` / `(T*)e` / `(T)e`. On success, writes the
 * cast type (including trailing `*`) and points *rest at the first byte after
 * the cast operand. */
static int cc__ufcs_parse_c_cast_primary(const char* s,
                                         char* type_out,
                                         size_t type_out_sz,
                                         const char** rest_out) {
    const char* p;
    const char* type_s;
    const char* type_e;
    size_t stars = 0;
    size_t tlen;
    if (!s || !type_out || type_out_sz == 0 || !rest_out) return 0;
    p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '(') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (!cc_is_ident_start(*p)) return 0;
    type_s = p;
    while (cc_is_ident_char(*p)) p++;
    type_e = p;
    while (*p == ' ' || *p == '\t') p++;
    while (*p == '*') {
        stars++;
        p++;
        while (*p == ' ' || *p == '\t') p++;
    }
    if (*p != ')') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    /* Operand: parenthesized expr, or a bare primary starting with ident/'('. */
    if (*p == '(') {
        int depth = 0;
        const char* q = p;
        do {
            if (*q == '(') depth++;
            else if (*q == ')') depth--;
            q++;
        } while (*q && depth > 0);
        if (depth != 0) return 0;
        p = q;
    } else if (cc_is_ident_start(*p)) {
        while (cc_is_ident_char(*p)) p++;
    } else {
        return 0;
    }
    tlen = (size_t)(type_e - type_s);
    if (tlen + stars + 1 > type_out_sz) return 0;
    memcpy(type_out, type_s, tlen);
    while (stars--) type_out[tlen++] = '*';
    type_out[tlen] = '\0';
    *rest_out = p;
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
    int from_cast = 0;
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
    /* `((T*)(e))->field` / `(T*)(e)->field`: peel cast primary so family text
     * UFCS can lower residual sites the AST final sweep would otherwise keep. */
    while (*p == '(') {
        const char* open = p;
        const char* close;
        const char* after;
        int depth = 0;
        const char* q = p;
        do {
            if (*q == '(') depth++;
            else if (*q == ')') depth--;
            q++;
        } while (*q && depth > 0);
        if (depth != 0) break;
        close = q - 1;
        after = close + 1;
        while (*after == ' ' || *after == '\t') after++;
        if (*after == '.' || (*after == '-' && after[1] == '>') || *after == '\0') {
            char cast_type[256];
            const char* cast_rest = NULL;
            char inner[256];
            size_t ilen = (size_t)(close - (open + 1));
            if (ilen == 0 || ilen >= sizeof(inner)) break;
            memcpy(inner, open + 1, ilen);
            inner[ilen] = '\0';
            if (cc__ufcs_parse_c_cast_primary(inner, cast_type, sizeof(cast_type), &cast_rest)) {
                while (*cast_rest == ' ' || *cast_rest == '\t') cast_rest++;
                if (*cast_rest == '\0') {
                    snprintf(out_type, out_type_sz, "%s", cast_type);
                    p = after;
                    from_cast = 1;
                    break;
                }
            }
        }
        break;
    }
    if (from_cast) {
        type_name = out_type;
        goto cc__ufcs_recv_field_walk;
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
    {
        const char* call_p = p;
        while (*call_p == ' ' || *call_p == '\t') call_p++;
        if (*call_p == '(') {
            const char* q = call_p;
            int depth = 0;
            do {
                if (*q == '(') depth++;
                else if (*q == ')') depth--;
                q++;
            } while (*q && depth > 0);
            while (*q == ' ' || *q == '\t') q++;
            /* Parser-survival bridge for the preceding pass's `key.hdr()`
               rewrite. The real fix is to let the parser/AST retry a missing
               member-call parse through registered UFCS dispatch, then this
               text pass no longer needs to preserve intermediate UFCS types. */
            if (depth == 0 && *q == '\0' && strcmp(root, "cc_slice_hdr") == 0) {
                snprintf(out_type, out_type_sz, "CCSliceHdr");
                if (out_recv_is_ptr) *out_recv_is_ptr = 0;
                return 1;
            }
        }
    }
    type_name = NULL;
    if (source_text) {
        type_name = cc__lookup_scoped_ufcs_var_type(source_text, use_offset, root,
                                                    local_type, sizeof(local_type));
    }
    if (!type_name && reg) {
        /* Full-expression resolve already walks `.` / `->`. Returning that
         * type and then walking the postfix again looks up `armed` on the
         * field type and fails — header lowering has no AST UFCS sweep. */
        type_name = cc_type_registry_resolve_receiver_expr_at(reg, recv, source_text, use_offset, &recv_is_ptr);
        if (type_name) {
            strncpy(out_type, type_name, out_type_sz - 1);
            out_type[out_type_sz - 1] = '\0';
            cc__resolve_registered_alias_type_name(reg, out_type, out_type, out_type_sz);
            {
                char normalized_recv_type[256];
                cc__normalize_ufcs_type_name(normalized_recv_type, sizeof(normalized_recv_type), out_type);
                if (normalized_recv_type[0]) snprintf(out_type, out_type_sz, "%s", normalized_recv_type);
            }
            if (out_recv_is_ptr) *out_recv_is_ptr = recv_is_ptr || strchr(out_type, '*') != NULL;
            return 1;
        }
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
            strcmp(base_type, out_type) != 0) {
            if (cc__lookup_scoped_type_alias(source_text, use_offset, base_type, alias_type, sizeof(alias_type))) {
                size_t stars = 0;
                size_t out_len = strlen(out_type);
                while (out_len > 0 && out_type[out_len - 1] == '*') { stars++; out_len--; }
                snprintf(out_type, out_type_sz, "%s", alias_type);
                while (stars-- > 0 && strlen(out_type) + 1 < out_type_sz) strcat(out_type, "*");
            }
        } else if (base_type[0]) {
            if (cc__lookup_scoped_type_alias(source_text, use_offset, base_type, alias_type, sizeof(alias_type))) {
                snprintf(out_type, out_type_sz, "%s", alias_type);
            }
        }
    }
cc__ufcs_recv_field_walk:
    (void)type_name;
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
        {
            const char* after_member = p;
            while (*after_member == ' ' || *after_member == '\t') after_member++;
            if (*after_member == '(') {
                const char* q = after_member;
                int depth = 0;
                do {
                    if (*q == '(') depth++;
                    else if (*q == ')') depth--;
                    q++;
                } while (*q && depth > 0);
                if (depth != 0) return 0;
                if (cc__lookup_ufcs_field_type(fields, field_count, base_type, field_name)) return 0;
                if ((strcmp(base_type, "CCSlice") == 0 ||
                     strcmp(base_type, "CCSliceUnique") == 0 ||
                     strcmp(base_type, "CCSliceShared") == 0) &&
                    strcmp(field_name, "hdr") == 0) {
                    snprintf(out_type, out_type_sz, "CCSliceHdr");
                    p = q;
                } else {
                    return 0;
                }
            } else {
                type_name = cc__lookup_ufcs_field_type(fields, field_count, base_type, field_name);
                if (!type_name) return 0;
                strncpy(out_type, type_name, out_type_sz - 1);
                out_type[out_type_sz - 1] = '\0';
            }
        }
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

static int cc__ufcs_recv_expr_is_addressable(const char* recv) {
    const char* p = recv;
    if (!p) return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (!cc_is_ident_start(*p)) return 0;
    while (cc_is_ident_char(*p)) p++;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '.') {
            p++;
        } else if (p[0] == '-' && p[1] == '>') {
            p += 2;
        } else {
            break;
        }
        while (*p == ' ' || *p == '\t') p++;
        if (!cc_is_ident_start(*p)) return 0;
        while (cc_is_ident_char(*p)) p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    return *p == '\0';
}

/* Compose the scalar value-receiver family callee (`cc_double_halve`,
 * `cc_long_long_twice`) for an admitted scalar base; 0 for non-scalars. */
static int cc__compose_scalar_ufcs_callee(char* out,
                                          size_t out_sz,
                                          const char* type_base,
                                          const char* method_name) {
    const char* fam = cc_ufcs_scalar_recv_family(type_base);
    int wrote;
    if (!out || out_sz == 0 || !fam || !method_name || !method_name[0]) return 0;
    wrote = snprintf(out, out_sz, "cc_%s_%s", fam, method_name);
    return wrote > 0 && (size_t)wrote < out_sz;
}

/* Parenthesized numeric-literal receiver (`(1.5).halve()`): classify the
 * literal's C type lexically from its suffix — no suffix with `.`/exponent
 * → double, `f`/`F` → float, bare integer → int, `l`/`L` → long, `ll`/`LL`
 * → long long; one leading unary minus allowed inside the parens. Unsigned
 * suffixes, hex/octal shapes, and unparenthesized literals stay
 * unclassified (fail closed; `1.5.halve()` is never recognized). */
static int cc__ufcs_scalar_literal_recv_type(const char* recv, char* out_type, size_t out_sz) {
    const char* p;
    const char* end;
    int is_float = 0;
    int f_suffix = 0;
    int l_count = 0;
    if (!recv || !out_type || out_sz == 0) return 0;
    p = recv;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '(') return 0;
    end = recv + strlen(recv);
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if (end <= p + 1 || end[-1] != ')') return 0;
    p++;
    end--;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if (p < end && *p == '-') {
        p++;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
    }
    if (p >= end || !isdigit((unsigned char)*p)) return 0;
    if (*p == '0' && p + 1 < end && (p[1] == 'x' || p[1] == 'X')) return 0;
    while (p < end && isdigit((unsigned char)*p)) p++;
    if (p < end && *p == '.') {
        is_float = 1;
        p++;
        while (p < end && isdigit((unsigned char)*p)) p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char* q = p + 1;
        if (q < end && (*q == '+' || *q == '-')) q++;
        if (q >= end || !isdigit((unsigned char)*q)) return 0;
        is_float = 1;
        p = q;
        while (p < end && isdigit((unsigned char)*p)) p++;
    }
    while (p < end) {
        char c = *p;
        if ((c == 'f' || c == 'F') && !f_suffix && !l_count) {
            f_suffix = 1;
        } else if ((c == 'l' || c == 'L') && !f_suffix && l_count < 2) {
            l_count++;
        } else {
            return 0;
        }
        p++;
    }
    if (f_suffix) {
        if (!is_float) return 0;
        snprintf(out_type, out_sz, "float");
        return 1;
    }
    if (is_float) {
        if (l_count) return 0;
        snprintf(out_type, out_sz, "double");
        return 1;
    }
    snprintf(out_type, out_sz, "%s",
             l_count == 2 ? "long long" : (l_count == 1 ? "long" : "int"));
    return 1;
}

/* Validate [ty_a, ty_b) as a pure type spelling — identifier tokens
 * and `*` only, at least one token, no statement keywords — then write
 * it whitespace-normalized and cv-stripped into out. Returns 1/0. */
static int cc__ufcs_dest_span_type(const char* s, size_t ty_a, size_t ty_b,
                                   char* out, size_t out_sz) {
    {
        size_t q = ty_a;
        size_t first_s = 0, first_e = 0;
        int toks = 0;
        while (q < ty_b) {
            /* A comment inside the type span is inert filler; without this
             * its `/` reaches the ident test and the destination is lost. */
            q = cc_skip_ws_and_comments(s, ty_b, q);
            if (q >= ty_b) break;
            if (s[q] == '*') { q++; toks++; continue; }
            if (!cc_is_ident_char(s[q])) return 0;
            if (toks == 0) first_s = q;
            while (q < ty_b && cc_is_ident_char(s[q])) q++;
            if (toks == 0) first_e = q;
            toks++;
        }
        if (toks == 0) return 0;
        if (toks == 1 && s[ty_b - 1] == '*') return 0;
        if (first_e > first_s) {
            size_t fl = first_e - first_s;
            if ((fl == 6 && !memcmp(s + first_s, "return", 6)) ||
                (fl == 4 && !memcmp(s + first_s, "case", 4)) ||
                (fl == 4 && !memcmp(s + first_s, "goto", 4)))
                return 0;
        }
    }
    {
        size_t q = ty_a, dn = 0;
        while (q < ty_b && dn + 1 < out_sz) {
            size_t q2 = cc_skip_ws_and_comments(s, ty_b, q);
            if (q2 != q) {
                if (dn > 0 && out[dn - 1] != ' ') out[dn++] = ' ';
                q = q2;
                continue;
            }
            out[dn++] = s[q++];
        }
        while (dn > 0 && out[dn - 1] == ' ') dn--;
        out[dn] = 0;
        if (!dn) return 0;
    }
    {
        char* d = out;
        for (;;) {
            if (strncmp(d, "const ", 6) == 0) { d += 6; continue; }
            if (strncmp(d, "volatile ", 9) == 0) { d += 9; continue; }
            break;
        }
        if (d != out) memmove(out, d, strlen(d) + 1);
        if (!out[0]) return 0;
    }
    return 1;
}

/* Destination at a dynamic-sink call site, so a `.ufcs_sink`
 * can compose `<callee>_<mangled T>` — the destination is one more
 * input to UFCS resolution wherever a typed destination is visible.
 * Three shapes:
 *   1 — declaration `T name = recv.method(...)`: out = T (normalized,
 *       cv-stripped).
 *   2 — assignment `lvalue = recv.method(...)` where the lvalue is an
 *       identifier or `.`/`->` path: out = the path text; the caller
 *       resolves its declared type. Deref/index lvalues are shape 0.
 *   3 — cast `(T)recv.method(...)`: out = T; *out_cast_a = offset of
 *       the cast's `(` so a composing caller can absorb the cast (the
 *       variant already returns T's rail — the cast is a destination
 *       spelling, not a conversion, and it never consumes a Result).
 * Comment-aware back-walk from `recv_start`. Returns the shape, 0 for
 * no visible destination. */
static int cc__ufcs_sink_dest_type(const char* s, size_t recv_start,
                                   char* out, size_t out_sz,
                                   size_t* out_cast_a) {
    size_t p = recv_start;
    size_t ns, ne, ty_a, ty_b;
    int has_sep = 0;
    if (!s || !out || out_sz == 0) return 0;
    out[0] = 0;
    /* Walk back over `= lvalue`; comments can sit between `=` and the
     * call.  `cc_rskip_ws_and_comments` rewinds block comments via the
     * opener search (comment-context-safe) and line comments via the
     * forward line verify. */
    p = cc_rskip_ws_and_comments(s, p);
    /* Shape 3: a cast directly wrapping the call (may span lines). */
    if (p >= 3 && s[p - 1] == ')') {
        size_t rp = p - 1;
        size_t lp = rp;
        while (lp > 0) {
            char c = s[lp - 1];
            if (c == '(') break;
            if (c == ')' || c == ';' || c == '{' || c == '}')
                return 0;
            lp--;
        }
        if (lp == 0) return 0;
        lp--;
        if (!cc__ufcs_dest_span_type(s, lp + 1, rp, out, out_sz)) return 0;
        if (out_cast_a) *out_cast_a = lp;
        return 3;
    }
    if (p == 0 || s[p - 1] != '=') return 0;
    p--;
    if (p > 0 && strchr("=!<>+-*/%&|^", s[p - 1])) return 0; /* ==, +=, ... */
    p = cc_rskip_ws_and_comments(s, p);
    ne = p;
    /* Ident, or `.`/`->` path (shape 2 lvalues). */
    for (;;) {
        size_t e2 = p;
        while (p > 0 && cc_is_ident_char(s[p - 1])) p--;
        if (p == e2) return 0;
        {
            size_t r = cc_rskip_ws_and_comments(s, p);
            if (r >= 1 && s[r - 1] == '.') {
                has_sep = 1;
                p = cc_rskip_ws_and_comments(s, r - 1);
                continue;
            }
            if (r >= 2 && s[r - 1] == '>' && s[r - 2] == '-') {
                has_sep = 1;
                p = cc_rskip_ws_and_comments(s, r - 2);
                continue;
            }
        }
        break;
    }
    ns = p;
    if (ns == ne) return 0;
    ty_b = p;
    /* Statement boundary (a newline bounds before any prior-line text,
     * so a comment on the preceding line never enters the span), then
     * classify: an all-whitespace [ty_a, ty_b) span is an assignment
     * (shape 2); otherwise it must be a pure type spelling —
     * identifier tokens and `*` only, at least one token. */
    while (p > 0 && !strchr(";{}(),:", s[p - 1]) && s[p - 1] != '\n') p--;
    ty_a = p;
    {
        size_t q = ty_a;
        while (q < ty_b && isspace((unsigned char)s[q])) q++;
        if (q >= ty_b) {
            size_t k = ns, dn = 0;
            while (k < ne && dn + 1 < out_sz) {
                size_t k2 = cc_skip_ws_and_comments(s, ne, k);
                if (k2 > k) { k = k2; continue; }
                out[dn++] = s[k++];
            }
            out[dn] = 0;
            return dn ? 2 : 0;
        }
    }
    if (has_sep) return 0; /* `T a.b = ...` is not a declaration */
    return cc__ufcs_dest_span_type(s, ty_a, ty_b, out, out_sz) ? 1 : 0;
}

/* Method-call chains: a UFCS call whose RECEIVER is itself a call
 * expression (`xs.sub(1,3).len()` after link 1 lowers, or
 * `d.halve().twice()` on scalar families) cannot lower through the
 * variable-rooted rails — the receiver has no declaration to read a
 * type from, and an rvalue receiver cannot take `&`. Normalize: bind
 * the leading call to a temporary of its (textually derived) return
 * type inside a GNU statement expression and re-anchor the remaining
 * chain on that variable:
 *
 *   ({ T0 __cc_chain_N = F(args); __cc_chain_N.m1(a1)...; })
 *
 * Every existing rail then applies to each remaining link on ordinary
 * variables — members, extensions, @as retry, sinks, and the strict
 * ladder — across the engine's iteration loop; a chain of depth d
 * fully lowers in ~2d iterations. Result-returning bases are left for
 * the result rails. Consumes through any trailing field suffix so
 * `ps.at(2).y` shapes stay inside the statement expression. */
static int cc__try_normalize_ufcs_chain(const char* src, size_t n,
                                        size_t recv_start, size_t sep_pos,
                                        const char* recv_expr,
                                        char** out, size_t* out_len,
                                        size_t* out_cap,
                                        size_t* last_emit, size_t* i_inout) {
    static _Thread_local int g_chain_tmp_id = 0;
    char fname[128];
    char t0[256];
    size_t fl = 0, p, chain_end;
    size_t rn = strlen(recv_expr);
    /* Receiver must be one whole call: ident '(' ... ')' with nothing
     * after the matching paren. */
    if (rn < 3 || recv_expr[rn - 1] != ')') return 0;
    if (!cc_is_ident_start(recv_expr[0])) return 0;
    while (fl < rn && cc_is_ident_char(recv_expr[fl])) fl++;
    if (fl == 0 || fl >= sizeof(fname)) return 0;
    memcpy(fname, recv_expr, fl);
    fname[fl] = 0;
    if (cc__free_call_name_is_keyword(recv_expr, fl)) return 0;
    p = fl;
    while (p < rn && (recv_expr[p] == ' ' || recv_expr[p] == '\t')) p++;
    if (p >= rn || recv_expr[p] != '(') return 0;
    {
        /* The '(' must match the final ')'. */
        int depth = 0;
        size_t q = p;
        for (; q < rn; q++) {
            if (recv_expr[q] == '(') depth++;
            else if (recv_expr[q] == ')') {
                depth--;
                if (depth == 0) break;
            }
        }
        if (q != rn - 1) return 0;
    }
    if (!cc__call_return_type(fname, src, n, t0, sizeof(t0))) return 0;
    if (!t0[0] || strcmp(t0, "void") == 0) return 0;
    if (strncmp(t0, "CCResult_", 9) == 0) return 0; /* result rails own */
    /* Consume the chain: (./->) ident [(...)] links, then any pure
     * field tail; stop at anything else. */
    chain_end = sep_pos;
    p = sep_pos;
    for (;;) {
        size_t q = p;
        if (q < n && src[q] == '.') q++;
        else if (q + 1 < n && src[q] == '-' && src[q + 1] == '>') q += 2;
        else break;
        q = cc_skip_ws_and_comments(src, n, q);
        if (q >= n || !cc_is_ident_start(src[q])) break;
        while (q < n && cc_is_ident_char(src[q])) q++;
        {
            size_t r = cc_skip_ws_and_comments(src, n, q);
            if (r < n && src[r] == '(') {
                size_t pe;
                if (!cc_find_matching_paren(src, n, r, &pe)) break;
                q = pe + 1;
            }
        }
        chain_end = q;
        p = cc_skip_ws_and_comments(src, n, q);
        if (p >= n) break;
        if (src[p] != '.' && !(p + 1 < n && src[p] == '-' && src[p + 1] == '>'))
            break;
    }
    if (chain_end <= sep_pos) return 0;
    {
        int id = ++g_chain_tmp_id;
        char head[512];
        int wrote = snprintf(head, sizeof(head), "({ %s __cc_chain_%d = %s; __cc_chain_%d",
                             t0, id, recv_expr, id);
        if (wrote <= 0 || (size_t)wrote >= sizeof(head)) return 0;
        cc_sb_append(out, out_len, out_cap, src + *last_emit,
                     recv_start - *last_emit);
        cc_sb_append_cstr(out, out_len, out_cap, head);
        cc_sb_append(out, out_len, out_cap, src + sep_pos, chain_end - sep_pos);
        cc_sb_append_cstr(out, out_len, out_cap, "; })");
    }
    *last_emit = chain_end;
    *i_inout = chain_end;
    return 1;
}

static int g_ufcs_scope_idx_locked = 0;
/* Header `.cch` → `.h` runs the text UFCS pass with no AST sweep. Same-file
 * wrappers (`cc_closure1_drop` for `c.drop()`) must not count as callees. */
static int g_ufcs_header_lowering = 0;
static char g_ufcs_header_path[PATH_MAX];
static int cc__included_cch_contains_fn_except(const char* name, const char* except_abs);
/* Set when a type-formal member site errored (missing/invalid type
 * source); the parser-safe wrapper turns it into the error sentinel. */
static _Thread_local int g_ufcs_typeformal_err = 0;

/* Defined below, after the generic-instantiation machinery they belong to;
 * the UFCS tier above calls them for member-position factory sites. */
/* `ident` followed by `::[`, with whitespace and comments allowed in both
 * gaps.  Requiring the three bytes to sit flush against the identifier made a
 * comment between the type name and its `::[` hide the whole site, so the
 * instantiation never happened and the raw spelling reached the C compiler.
 * Returns 1 and sets `*out_lb` to the `[`. */
static int cc__ident_generic_bracket(const char* src, size_t n, size_t id_e,
                                     size_t* out_lb) {
    size_t p = cc_skip_ws_and_comments(src, n, id_e);
    if (p + 1 >= n || src[p] != ':' || src[p + 1] != ':') return 0;
    p = cc_skip_ws_and_comments(src, n, p + 2);
    if (p >= n || src[p] != '[') return 0;
    if (out_lb) *out_lb = p;
    return 1;
}

/* A built-in container spelling `Name::[`, with whitespace and comments
 * allowed in the gaps.  Same defect as the user-generic site: the name and
 * `::[` were one literal, so a comment between them hid the spelling. */
static int cc__builtin_generic_at(const char* src, size_t n, size_t i,
                                  const char* name, size_t* out_lb) {
    size_t ln = strlen(name);
    if (i + ln > n || memcmp(src + i, name, ln) != 0) return 0;
    if (i + ln < n && cc_is_ident_char(src[i + ln])) return 0;
    return cc__ident_generic_bracket(src, n, i + ln, out_lb);
}


static int cc__split_type_args(const char* params, size_t params_len,
                               char orig_args[][128], char mang_args[][128],
                               int max_args);
static int cc__emit_generic_instance(const char* gname,
                                     char orig_args[][128], char mang_args[][128],
                                     int nargs, const char* src, size_t n,
                                     size_t use_pos, size_t br_close,
                                     const char* input_path,
                                     char* mangled, size_t mangled_cap);

static char* cc__rewrite_generic_family_ufcs_impl(const char* src, size_t n, int parser_safe,
                                                 const char* input_path) {
    CCUfcsVarInfo vars[512];
    CCUfcsFieldInfo fields[256];
    CCTypeRegistry* reg = cc_type_registry_get_global();
    size_t var_count = 0, field_count = 0;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    size_t last_emit = 0;
    int own_idx = 0;
    CCScannerState scan;
    if (!src || n == 0) return NULL;
    /* One pass: scoped-decl index + UFCS var/field harvest. Nested calls on
     * receiver substrings must not clobber the parent index (lock). */
    if (!g_ufcs_scope_idx_locked) {
        cc__ufcs_scope_idx_build_ex(src, n, vars, &var_count,
                                    sizeof(vars) / sizeof(vars[0]), fields,
                                    &field_count, sizeof(fields) / sizeof(fields[0]));
        g_ufcs_scope_idx_locked = 1;
        own_idx = 1;
    } else {
        cc__collect_generic_ufcs_types(src, n, vars, &var_count,
                                       sizeof(vars) / sizeof(vars[0]), fields,
                                       &field_count,
                                       sizeof(fields) / sizeof(fields[0]));
    }
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
        int nursery_spawn_named = 0; /* task-form spawn → spawn_async_named */
        int chan_tx = 0;
        int chan_rx = 0;
        int map_decl_like = 0;
        int wildcard_like = 0;
        int scalar_like = 0;
        int scalar_literal = 0;
        int slice_spec_like = 0;
        int atomic_like = 0;
        int recv_addressable = 1;
        size_t targ_a = 0, targ_b = 0;
        char wildcard_callee[256];
        const char* channel_callee = NULL;
        wildcard_callee[0] = '\0';
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (!cc__scan_generic_ufcs_call_site(src, n, i, &sep_pos, &method_start, &method_end,
                                             &recv_start, &paren_pos, &paren_end,
                                             method_name, sizeof(method_name),
                                             recv_expr, sizeof(recv_expr),
                                             &targ_a, &targ_b)) {
            i++;
            continue;
        }
        if (recv_start < last_emit) {
            i++;
            continue;
        }
        if (recv_start > 0) {
            size_t rs = recv_start;
            while (rs > 0 && (src[rs - 1] == ' ' || src[rs - 1] == '\t')) rs--;
            if (rs > 0 && src[rs - 1] == '.') { i++; continue; }
            if (rs >= 2 && src[rs - 2] == '-' && src[rs - 1] == '>') { i++; continue; }
        }
        /* Call-expression receiver: normalize the chain onto a typed
         * temporary and let later iterations lower each link. */
        if (cc__try_normalize_ufcs_chain(src, n, recv_start, sep_pos, recv_expr,
                                         &out, &out_len, &out_cap,
                                         &last_emit, &i))
            continue;
        if (!cc__resolve_generic_ufcs_receiver_type(recv_expr, src, sep_pos,
                                                    vars, var_count, fields, field_count,
                                                    recv_type, sizeof(recv_type), &recv_is_ptr)) {
            /* Parenthesized numeric-literal receiver: type it lexically from
             * the suffix ((1.5) → double). Composes only through the scalar
             * value-receiver family below (compose-then-verify). */
            if (!cc__ufcs_scalar_literal_recv_type(recv_expr, recv_type, sizeof(recv_type))) {
                i++;
                continue;
            }
            scalar_literal = 1;
            recv_is_ptr = 0;
        }
        cc__copy_type_base(recv_type_base, sizeof(recv_type_base), recv_type);
        cc__normalize_bool_family_type(recv_type_base, sizeof(recv_type_base));
        if (reg) {
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
        {
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
        if (reg && strcmp(method_name, "as_slice") == 0 &&
            strncmp(recv_type_base, "CCVec_", 6) == 0) {
            /* A typed-vec slice view names the element's slice instance:
             * when the instance is known declared, register its spec and
             * `base` @as field (mirrors `T[:]` naming) so arg-position
             * autocast and spec dispatch type the returned value. */
            size_t vc = cc_type_registry_vec_count(reg);
            size_t vi;
            for (vi = 0; vi < vc; vi++) {
                const CCTypeInstantiation* vin = cc_type_registry_get_vec(reg, vi);
                char probe[256];
                char inst_name[160];
                if (!vin || !vin->mangled_name || !vin->type1) continue;
                if (strcmp(vin->mangled_name, recv_type_base) != 0) continue;
                if (cc_slice_spec_instance_for_elem(vin->type1, probe,
                                                    sizeof(probe)) == 0)
                    (void)cc__slice_instance_for_elem(vin->type1, 0,
                                                      strlen(vin->type1),
                                                      inst_name, sizeof(inst_name),
                                                      NULL, 0);
                break;
            }
        }
        /* Family gate (normalize/gate only): derived member or declared
         * Instance_method extension. Misses stay for the AST strict ladder
         * — do not invent an unverified composed spelling here. */
        if ((strncmp(recv_type_base, "Map_", 4) == 0 ||
             strncmp(recv_type_base, "ArrayMap_", 9) == 0 ||
             strncmp(recv_type_base, "CCVec_", 6) == 0 ||
             strncmp(recv_type_base, "CCResult_", 9) == 0) &&
            !cc_ufcs_family_accepts_in_tu(recv_type_base, method_name, src, n)) {
            i++;
            continue;
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
        /* Type-formal members: `::[...]` specializes the member name it
         * follows.  arena.allocT() / arena.allocT(n) lower to
         * cc_arena_alloc_T / cc_arena_alloc_T_count; task.block_on()
         * lowers to cc_block_on(T, task).  The type comes from the
         * explicit `::[T]` or from the declared destination. */
        {
            /* Sequence/mapping extraction and row batching: the type
             * argument names the element type(s), and the worker macros
             * take them as a leading argument — the same shape as
             * cc_arena_alloc_T.  The worker resolves by what the
             * receiver's header installs (cc_py_obj_as_list,
             * cc_js_val_map, …): snake-compose it and require it be TU-
             * or header-visible — no receiver-type names here. */
            int is_as_list = strcmp(method_name, "as_list") == 0;
            int is_as_map = strcmp(method_name, "as_map") == 0;
            int is_tf_map = strcmp(method_name, "map") == 0;
            char tf_worker[400];
            tf_worker[0] = 0;
            /* Only a `::[...]` site engages the worker: a bare `.map(f)`
             * on a dynamic receiver stays an ordinary member call (a JS
             * array's own .map is real), while a spelled type formal
             * binds the batch worker. */
            if ((is_as_list || is_as_map || is_tf_map) && targ_b > targ_a &&
                recv_type_base[0]) {
                char base[192], snake[192];
                size_t bl;
                snprintf(base, sizeof(base), "%s", recv_type_base);
                bl = strlen(base);
                while (bl > 0 && (base[bl - 1] == '*' || base[bl - 1] == ' '))
                    base[--bl] = 0;
                if (bl > 2 && base[0] == 'C' && base[1] == 'C' &&
                    base[2] >= 'A' && base[2] <= 'Z')
                    memmove(base, base + 2, bl - 1);
                cc_emit_plan_snake_name(base, snake, sizeof(snake));
                snprintf(tf_worker, sizeof(tf_worker), "cc_%s_%s", snake,
                         method_name);
                if (!(cc__ufcs_fn_name_in_text(src, n, tf_worker) ||
                      cc_included_cch_contains_fn(tf_worker)))
                    tf_worker[0] = 0;
            }
            if (tf_worker[0]) {
                char targs[192];
                targs[0] = 0;
                if (targ_b > targ_a) {
                    const char* ta = src + targ_a;
                    const char* tb = src + targ_b;
                    cc__trim_span_ws(&ta, &tb);
                    if (tb > ta && (size_t)(tb - ta) < sizeof(targs)) {
                        memcpy(targs, ta, (size_t)(tb - ta));
                        targs[tb - ta] = 0;
                    }
                }
                if (!targs[0]) {
                    cc_pp_error_cat("<input>", scan.line, 1, "type",
                                    is_as_list
                                        ? "as_list needs its element type: "
                                          "obj.as_list::[T](&arena)"
                                    : is_tf_map
                                        ? "map needs its result element type: "
                                          "f.map::[T](&arena, cols...)"
                                        : "as_map needs its key and value types: "
                                          "obj.as_map::[K, V](&arena, m)");
                    g_ufcs_typeformal_err = 1;
                    i = paren_end + 1;
                    continue;
                }
                cc_sb_append(&out, &out_len, &out_cap, src + last_emit,
                             recv_start - last_emit);
                cc_sb_append_cstr(&out, &out_len, &out_cap, tf_worker);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "(");
                cc_sb_append_cstr(&out, &out_len, &out_cap, targs);
                cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                if (!recv_is_ptr)
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "&");
                cc_sb_append_cstr(&out, &out_len, &out_cap, recv_expr);
                {
                    size_t as = cc_skip_ws_and_comments(src, paren_end, paren_pos + 1);
                    if (as < paren_end) {
                        cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                        cc_sb_append(&out, &out_len, &out_cap, src + as,
                                     paren_end - as);
                    }
                }
                cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                last_emit = paren_end + 1;
                i = paren_end + 1;
                continue;
            }
            int is_allocT = arena_like && strcmp(method_name, "allocT") == 0;
            int is_block_on = (strncmp(recv_type_base, "CCTask", 6) == 0 ||
                               strcmp(recv_type_base, "CCAsyncVoidRet") == 0) &&
                              strcmp(method_name, "block_on") == 0;
            if (is_allocT || is_block_on) {
                char ty[128];
                ty[0] = 0;
                if (targ_b > targ_a) {
                    const char* ta = src + targ_a;
                    const char* tb = src + targ_b;
                    cc__trim_span_ws(&ta, &tb);
                    if (tb > ta && (size_t)(tb - ta) < sizeof(ty)) {
                        memcpy(ty, ta, (size_t)(tb - ta));
                        ty[tb - ta] = 0;
                    }
                } else {
                    char dest[256];
                    size_t cast_a = 0;
                    if (cc__ufcs_sink_dest_type(src, recv_start, dest,
                                                sizeof(dest), &cast_a) == 1) {
                        size_t dl = strlen(dest);
                        if (is_allocT) {
                            /* Destination must be a pointer; the element
                             * type is the destination minus one star. */
                            if (dl > 1 && dest[dl - 1] == '*') {
                                dest[--dl] = 0;
                                while (dl > 0 && dest[dl - 1] == ' ') dest[--dl] = 0;
                                snprintf(ty, sizeof(ty), "%s", dest);
                            }
                        } else {
                            snprintf(ty, sizeof(ty), "%s", dest);
                        }
                    }
                }
                if (!ty[0]) {
                    cc_pp_error_cat("<input>", scan.line, 1, "type",
                                    is_allocT
                                        ? "arena.allocT needs its element type: "
                                          "declare a typed pointer destination "
                                          "(T* p = arena.allocT(n)) or spell it "
                                          "explicitly (arena.allocT::[T](n))"
                                        : "task.block_on needs its result type: "
                                          "declare a typed destination "
                                          "(T v = task.block_on()) or spell it "
                                          "explicitly (task.block_on::[T]())");
                    g_ufcs_typeformal_err = 1;
                    i = paren_end + 1;
                    continue;
                }
                cc_sb_append(&out, &out_len, &out_cap, src + last_emit,
                             recv_start - last_emit);
                if (is_allocT) {
                    size_t args_s = cc_skip_ws_and_comments(src, paren_end, paren_pos + 1);
                    int has_count = args_s < paren_end;
                    cc_sb_append_cstr(&out, &out_len, &out_cap,
                                      has_count ? "cc_arena_alloc_T_count("
                                                : "cc_arena_alloc_T(");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ty);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                    if (!recv_is_ptr)
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "&");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, recv_expr);
                    if (has_count) {
                        cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                        cc_sb_append(&out, &out_len, &out_cap, src + paren_pos + 1,
                                     paren_end - (paren_pos + 1));
                    }
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                } else {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_block_on(");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ty);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                    if (recv_is_ptr)
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "*");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, recv_expr);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                }
                last_emit = paren_end + 1;
                i = paren_end + 1;
                continue;
            }
            /* `::[...]` on any other member: no type formal to bind.  A
             * member-position factory site was already rewritten to its
             * free-name spelling before instantiation, so anything still
             * wearing `::[` here resolved to neither. */
            if (targ_b > targ_a) {
                char base[192], snake[192], cand[352], names[512];
                size_t bl;
                snprintf(base, sizeof(base), "%s", recv_type_base);
                bl = strlen(base);
                while (bl > 0 && (base[bl - 1] == '*' || base[bl - 1] == ' '))
                    base[--bl] = 0;
                if (bl > 2 && base[0] == 'C' && base[1] == 'C' &&
                    base[2] >= 'A' && base[2] <= 'Z')
                    memmove(base, base + 2, bl - 1);
                cc_emit_plan_snake_name(base, snake, sizeof(snake));
                snprintf(cand, sizeof(cand), "%s_%s", snake, method_name);
                cc_pp_error_cat("<input>", scan.line, 1, "type",
                                "no type-formal member '%s' for receiver type "
                                "'%s' — `::[...]` binds a member that declares a "
                                "type formal (allocT, block_on, as_list, as_map, map) "
                                "or a registered generic factory named '%s'",
                                method_name, recv_type_base, cand);
                if (cc_emit_plan_generic_factory_names_csv(names, sizeof(names)) > 0)
                    fprintf(stderr, "  note: registered generic factories: %s\n", names);
                g_ufcs_typeformal_err = 1;
                i = paren_end + 1;
                continue;
            }
        }
        if (slice_like && !cc__is_slice_ufcs_method(method_name)) {
            i++;
            continue;
        }
        /* Typed slice instances (CC_DECL_SLICE_SPEC): derived members
         * lower textually here — composed as NAME##_<member> by the
         * default branch below — so postfix continuations after the
         * call parse as plain C. Anything else (byte methods via the
         * `base` @as retry, declared extensions, the strict ladder)
         * stays with the AST pass. */
        /* Atomic receivers: the cc_atomic_* typedefs dispatch to the
         * type-generic cc_atomic_<op> macros with the receiver's
         * address — `counter.fetch_add(1)` is cc_atomic_fetch_add(
         * &counter, 1). Compose-then-verify against the header's
         * function-like defines; a non-member falls to the ladder. */
        atomic_like = (strncmp(recv_type_base, "cc_atomic_", 10) == 0);
        if (atomic_like) {
            char acand[128];
            if ((size_t)snprintf(acand, sizeof(acand), "cc_atomic_%s",
                                 method_name) >= sizeof(acand) ||
                !cc_included_cch_declares_fn(acand)) {
                i++;
                continue;
            }
        }
        slice_spec_like = (cc_slice_spec_lookup(recv_type_base, NULL, NULL) == 0);
        if (slice_spec_like && !cc_ufcs_family_has_member(recv_type_base, method_name)) {
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
        if (strcmp(recv_type_base, "CCChan") == 0 ||
            strcmp(recv_type_base, "CCChan*") == 0) {
            /* Raw CCChan UFCS (recv/try_recv/close/free) is owned by the
               compiler's channel dispatch (cc_ufcs_channel_callee), which
               supplies the element-size argument the snake_case twin
               (`cc_chan_recv(ch, out, size)`) requires.  The wildcard
               composer below would emit a two-argument `cc_chan_recv(ch,
               out)` and fail arity checking, so skip these call sites and
               let the AST UFCS pass lower them. */
            i++;
            continue;
        }
        map_decl_like = cc__source_declares_map_ufcs(src, n, recv_type_base);
        if (!(strncmp(recv_type_base, "CCVec_", 6) == 0 ||
              strncmp(recv_type_base, "ArrayMap_", 9) == 0 ||
              strncmp(recv_type_base, "Map_", 4) == 0 ||
              parser_vec || parser_map || command_like || file_like || arena_like || string_like || slice_like || nursery_like ||
              chan_tx || chan_rx || map_decl_like || slice_spec_like ||
              atomic_like ||
              strncmp(recv_type_base, "CCResult_", 9) == 0)) {
            /* CC*-prefixed stdlib types follow the snake_case twin convention
             * (`CCListener.accept` → `cc_listener_accept`) even when the
             * callee spelling is only visible after header splice.  Requiring
             * `cc__ufcs_fn_name_in_text` here left pure UFCS call sites
             * (`ln.accept()` with no explicit `cc_listener_accept` in the TU)
             * unlowered through canonicalize's pre-splice `!>` expansion,
             * so binders fell through to the ambient `CCError` _Generic arm. */
            /* Types with named `@as` embeds: leave member calls for the AST
             * UFCS pass (as-retry / flat `.destroy()`). Inventing a CC*
             * snake_case twin here invents missing callees like
             * `cc_temp_file_write` and breaks the host parse. */
            if (reg && cc_type_registry_has_as_field(reg, recv_type_base)) {
                i++;
                continue;
            }
            {
                const char* sink_callee = NULL;
                const char* sink_wrap = NULL;
                int has_sink =
                    reg && cc_type_registry_get_dynamic_sink(
                               reg, recv_type_base, &sink_callee, &sink_wrap) == 0;
                int composed =
                    !cc__lookup_ufcs_field_type(fields, field_count,
                                                recv_type_base, method_name) &&
                    cc_ufcs_compose_default_callee(wildcard_callee,
                                                   sizeof(wildcard_callee),
                                                   recv_type_base, method_name);
                int real = composed &&
                           (g_ufcs_header_lowering
                                ? cc__included_cch_contains_fn_except(
                                      wildcard_callee, g_ufcs_header_path)
                                : (cc__ufcs_fn_name_in_text(src, n, wildcard_callee) ||
                                   cc_included_cch_contains_fn(wildcard_callee)));
                /* Sink-typed receiver, method with no real callee: lower to
                 * `sink(&recv, "method", N, wrap(a1), ...)` here so `!>`
                 * binders type against the sink's Result. Real methods keep
                 * the conventional path. */
                if (has_sink && composed && !real &&
                    (recv_is_ptr || cc__ufcs_recv_expr_is_addressable(recv_expr))) {
                    char sink_dest_callee[512];
                    size_t sink_emit_from = recv_start;
                    /* `.ufcs_sink`: the destination participates in
                     * resolution. At `T name = recv.method(...)` — or an
                     * assignment to a resolvable lvalue — compose
                     * `<sink>_<mangled T>` and use it when that function
                     * is declared; plain sink otherwise
                     * (compose-then-verify). */
                    if (cc_type_registry_dynamic_sink_dest_aware(reg, recv_type_base)) {
                        char dest_ty[256];
                        char dest_mangled[128];
                        size_t cast_a = 0;
                        int dm = cc__ufcs_sink_dest_type(src, recv_start,
                                                         dest_ty, sizeof(dest_ty),
                                                         &cast_a);
                        /* Decl/assign destinations require the call be the
                         * whole RHS: a subexpression operand has no single
                         * expected type in C, so the statement head is not
                         * its destination. After the call only `;` or a
                         * sigil (`!>` / `?>`) may follow. A cast (shape 3)
                         * is its own spelled destination and composes
                         * anywhere. */
                        if (dm == 1 || dm == 2) {
                            size_t q = cc_skip_ws_and_comments(src, n, paren_end + 1);
                            if (!(q >= n || src[q] == ';' ||
                                  ((src[q] == '!' || src[q] == '?') &&
                                   q + 1 < n && src[q + 1] == '>')))
                                dm = 0;
                        }
                        if (dm == 2) {
                            /* Assignment: the destination is the lvalue's
                             * declared type. */
                            char lv_type[256];
                            int lv_ptr = 0;
                            if (cc__resolve_generic_ufcs_receiver_type(
                                    dest_ty, src, recv_start,
                                    vars, var_count, fields, field_count,
                                    lv_type, sizeof(lv_type), &lv_ptr) &&
                                lv_type[0])
                                snprintf(dest_ty, sizeof(dest_ty), "%s", lv_type);
                            else
                                dm = 0;
                        }
                        if (dm) {
                            int variant_ok = 0;
                            cc__mangle_type_name(dest_ty, strlen(dest_ty),
                                                 dest_mangled, sizeof(dest_mangled));
                            if (dest_mangled[0] &&
                                (size_t)snprintf(sink_dest_callee, sizeof(sink_dest_callee),
                                                 "%s_%s", sink_callee, dest_mangled) <
                                    sizeof(sink_dest_callee) &&
                                (cc__ufcs_fn_name_in_text(src, n, sink_dest_callee) ||
                                 cc_included_cch_contains_fn(sink_dest_callee))) {
                                variant_ok = 1;
                                sink_callee = sink_dest_callee;
                                /* A composed cast is absorbed: the variant
                                 * already returns the spelled type's rail. */
                                if (dm == 3 && cast_a >= last_emit)
                                    sink_emit_from = cast_a;
                            }
                            /* Scalar destination with no installed variant:
                             * the plain sink's return can never initialize a
                             * scalar, so the site is provably ill-formed —
                             * fail articulate now, enumerating what IS
                             * installed, instead of leaking host noise.
                             * Non-scalar destinations keep the plain-sink
                             * fallback (it may be exactly right). */
                            if (!variant_ok && dest_mangled[0] &&
                                cc_ufcs_scalar_recv_family(dest_ty) &&
                                cc__sink_returns_result(src, n, sink_callee)) {
                                char key[256];
                                const char* lp = NULL;
                                size_t lpl = 0;
                                int line = cc_user_line_for_offset(src, n, recv_start,
                                                                   1, &lp, &lpl);
                                char shown[256];
                                int dup = 0, ri;
                                if (lp && lpl > 0 && lpl < sizeof(shown)) {
                                    memcpy(shown, lp, lpl);
                                    shown[lpl] = 0;
                                } else {
                                    snprintf(shown, sizeof(shown), "<input>");
                                }
                                snprintf(key, sizeof(key), "%s:%d:%s:%s", shown,
                                         line, sink_callee, dest_mangled);
                                for (ri = 0; ri < g_ufcs_dest_reported_n && !dup; ri++)
                                    if (strcmp(g_ufcs_dest_reported[ri], key) == 0)
                                        dup = 1;
                                if (!dup) {
                                    char inst[512];
                                    if (g_ufcs_dest_reported_n <
                                        (int)(sizeof(g_ufcs_dest_reported) /
                                              sizeof(g_ufcs_dest_reported[0])))
                                        snprintf(
                                            g_ufcs_dest_reported[g_ufcs_dest_reported_n++],
                                            sizeof(g_ufcs_dest_reported[0]), "%s", key);
                                    char fam_prefix[300];
                                    cc_pp_error_cat(shown, line, 1, "type",
                                                    "destination '%s' is not an installed "
                                                    "destination of %s",
                                                    dest_ty, sink_callee);
                                    snprintf(fam_prefix, sizeof(fam_prefix), "%s_",
                                             sink_callee);
                                    cc__enumerate_family_variants(src, n, fam_prefix,
                                                                  inst, sizeof(inst));
                                    if (inst[0])
                                        fprintf(stderr,
                                                "%s:%d:1: note: installed destinations "
                                                "(mangled): %s\n",
                                                shown, line, inst);
                                    else
                                        fprintf(stderr,
                                                "%s:%d:1: note: no destination variants "
                                                "of %s are declared\n",
                                                shown, line, sink_callee);
                                    fprintf(stderr,
                                            "%s:%d:1: note: the plain sink's return "
                                            "cannot initialize a scalar destination\n",
                                            shown, line);
                                    g_cc_pass_error_count++;
                                }
                            }
                        }
                    }
                    size_t args_start = paren_pos + 1;
                    size_t args_end = paren_end;
                    /* Inert-only argument list is an EMPTY list. */
                    args_start = cc_skip_ws_and_comments(src, args_end, args_start);
                    args_end = cc_rskip_ws_and_comments(src, args_end);
                    if (args_end < args_start) args_end = args_start;
                    cc_sb_append(&out, &out_len, &out_cap, src + last_emit,
                                 sink_emit_from - last_emit);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, sink_callee);
                    cc_sb_append_cstr(&out, &out_len, &out_cap,
                                      recv_is_ptr ? "((" : "(&(");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, recv_expr);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "), \"");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, method_name);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "\"");
                    {
                        /* Count then emit top-level comma-separated args,
                         * each lifted by the wrapper macro. */
                        int argc = 0;
                        int pass;
                        for (pass = 0; pass < 2; pass++) {
                            size_t s0 = args_start;
                            size_t q = args_start;
                            int depth = 0, in_s = 0, in_c = 0, in_lc2 = 0, in_bc2 = 0;
                            if (pass == 1) {
                                char nbuf[16];
                                snprintf(nbuf, sizeof(nbuf), ", %d", argc);
                                cc_sb_append_cstr(&out, &out_len, &out_cap, nbuf);
                            }
                            for (; q <= args_end; q++) {
                                int at_end = (q == args_end);
                                char c = at_end ? '\0' : src[q];
                                if (!at_end && in_lc2) {
                                    if (c == '\n') in_lc2 = 0;
                                    continue;
                                }
                                if (!at_end && in_bc2) {
                                    if (c == '*' && q + 1 < args_end && src[q + 1] == '/') { in_bc2 = 0; q++; }
                                    continue;
                                }
                                if (!at_end && in_s) {
                                    if (c == '\\' && q + 1 < args_end) q++;
                                    else if (c == '"') in_s = 0;
                                    continue;
                                }
                                if (!at_end && in_c) {
                                    if (c == '\\' && q + 1 < args_end) q++;
                                    else if (c == '\'') in_c = 0;
                                    continue;
                                }
                                if (!at_end) {
                                    if (c == '/' && q + 1 < args_end && src[q + 1] == '/') { in_lc2 = 1; q++; continue; }
                                    if (c == '/' && q + 1 < args_end && src[q + 1] == '*') { in_bc2 = 1; q++; continue; }
                                    if (c == '"') { in_s = 1; continue; }
                                    if (c == '\'') { in_c = 1; continue; }
                                    if (c == '(' || c == '[' || c == '{') { depth++; continue; }
                                    if (c == ')' || c == ']' || c == '}') { depth--; continue; }
                                    if (!(c == ',' && depth == 0)) continue;
                                }
                                if (q > s0) {
                                    if (pass == 0) argc++;
                                    else {
                                        cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                                        cc_sb_append_cstr(&out, &out_len, &out_cap, sink_wrap);
                                        cc_sb_append_cstr(&out, &out_len, &out_cap, "(");
                                        cc_sb_append(&out, &out_len, &out_cap, src + s0, q - s0);
                                        cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                                    }
                                }
                                s0 = q + 1;
                            }
                        }
                    }
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                    last_emit = paren_end + 1;
                    i = paren_end + 1;
                    continue;
                }
                if (!scalar_literal && real &&
                    !cc_ufcs_scalar_recv_family(recv_type_base)) {
                    wildcard_like = 1;
                } else if (!recv_is_ptr &&
                           cc__compose_scalar_ufcs_callee(wildcard_callee,
                                                          sizeof(wildcard_callee),
                                                          recv_type_base, method_name) &&
                           (g_ufcs_header_lowering
                                ? cc__included_cch_contains_fn_except(
                                      wildcard_callee, g_ufcs_header_path)
                                : (cc__ufcs_fn_name_in_text(src, n, wildcard_callee) ||
                                   cc_included_cch_contains_fn(wildcard_callee)))) {
                    /* Scalar value receiver (`d.halve()`, `(1.5).halve()`):
                     * `cc_<mangled type>_<method>`, receiver by value. */
                    wildcard_like = 1;
                    scalar_like = 1;
                } else {
                    i++;
                    continue;
                }
            }
        }
        if ((chan_tx || chan_rx) && cc__ufcs_preceded_by_await(src, recv_start)) {
            i++;
            continue;
        }
        if (arena_like) {
            /* CCArena has a registered @comptime hook (cc_arena_lower_c).
               Leave hook-owned methods for the AST/hook path so rewriting
               cannot invent default `cc_arena_<method>` callees that may not
               exist (e.g. comptime_type_arena_hooks_smoke: `arena.avail()`
               must lower via the hook to `arena_avail`, not `cc_arena_avail`).
               Fixed lifecycle methods match the default twins and are safe to
               text-lower — needed so `@defer arena.destroy()` cleanup sites
               do not force a final-UFCS reparse when phase-3 reused the
               initial AST (defer bodies often lack a UFCS stub node). */
            if (!(strcmp(method_name, "destroy") == 0 ||
                  strcmp(method_name, "free") == 0 ||
                  strcmp(method_name, "reset") == 0)) {
                i++;
                continue;
            }
        }
        if (chan_tx || chan_rx) {
            if (chan_tx && strcmp(method_name, "send") == 0) {
                channel_callee = "cc_channel_send";
            } else if (chan_rx && strcmp(method_name, "recv") == 0) {
                channel_callee = "cc_channel_recv";
            } else {
                i++;
                continue;
            }
        }
        family_by_value = (strncmp(recv_type_base, "CCResult_", 9) == 0) || scalar_like;
        /* Mixed-convention runtime families (cc_slice_*): receiver
         * passing follows the declared first parameter — by value when
         * the family fn takes the receiver type by value, &recv when it
         * takes a pointer. Same rule as the bare tier. */
        if (slice_like && !family_by_value) {
            char scand[128], fparam[256];
            if ((size_t)snprintf(scand, sizeof(scand), "cc_slice_%s",
                                 method_name) < sizeof(scand) &&
                cc_included_cch_fn_first_param(scand, fparam, sizeof(fparam)) &&
                !strchr(fparam, '*'))
                family_by_value = 1;
        }
        family_pass_direct = parser_map || map_decl_like ||
                             (strncmp(recv_type_base, "ArrayMap_", 9) == 0) ||
                             (strncmp(recv_type_base, "Map_", 4) == 0);
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
        recv_addressable = cc__ufcs_recv_expr_is_addressable(recv_expr);
        if (wildcard_like && !scalar_like && !recv_is_ptr && !recv_addressable) {
            static int g_wildcard_recv_tmp_id = 0;
            int tmp_id = ++g_wildcard_recv_tmp_id;
            char tmp_name[64];
            char* lowered_recv = cc__rewrite_generic_family_ufcs_impl(recv_expr, strlen(recv_expr), parser_safe,
                                                                 input_path);
            const char* emit_recv = lowered_recv ? lowered_recv : recv_expr;
            size_t args_start = paren_pos + 1;
            size_t args_end = paren_end;
            snprintf(tmp_name, sizeof(tmp_name), "__cc_ufcs_recv_%d", tmp_id);
            /* Inert-only argument list is an EMPTY list. */
            args_start = cc_skip_ws_and_comments(src, args_end, args_start);
            args_end = cc_rskip_ws_and_comments(src, args_end);
            if (args_end < args_start) args_end = args_start;
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, recv_start - last_emit);
            cc_sb_append_cstr(&out, &out_len, &out_cap, "({ ");
            cc_sb_append_cstr(&out, &out_len, &out_cap, recv_type);
            cc_sb_append_cstr(&out, &out_len, &out_cap, " ");
            cc_sb_append_cstr(&out, &out_len, &out_cap, tmp_name);
            cc_sb_append_cstr(&out, &out_len, &out_cap, " = ");
            cc_sb_append_cstr(&out, &out_len, &out_cap, emit_recv);
            cc_sb_append_cstr(&out, &out_len, &out_cap, "; ");
            cc_sb_append_cstr(&out, &out_len, &out_cap, wildcard_callee);
            cc_sb_append_cstr(&out, &out_len, &out_cap, "(&");
            cc_sb_append_cstr(&out, &out_len, &out_cap, tmp_name);
            if (args_end > args_start) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                cc_sb_append(&out, &out_len, &out_cap, src + args_start, args_end - args_start);
            }
            cc_sb_append_cstr(&out, &out_len, &out_cap, "); })");
            free(lowered_recv);
            last_emit = paren_end + 1;
            i = paren_end + 1;
            continue;
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
                        int is_array_map =
                            (strncmp(recv_type_base, "__CC_ARRAY_MAP", 14) == 0);
                        cc__trim_span_ws(&key_s, &key_e);
                        cc__trim_span_ws(&val_s, &val_e);
                        cc__mangle_type_name(key_s, (size_t)(key_e - key_s), mangled_key, sizeof(mangled_key));
                        cc__mangle_type_name(val_s, (size_t)(val_e - val_s), mangled_val, sizeof(mangled_val));
                        snprintf(concrete_type, sizeof(concrete_type), "%s_%s_%s",
                                 is_array_map ? "ArrayMap" : "Map",
                                 mangled_key, mangled_val);
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
                if (strcmp(method_name, "spawn") == 0) {
                    /* Closure `() =>` / CCClosure0 value → spawn_closure0;
                     * else @async task → spawn_async_named. */
                    size_t as = paren_pos + 1;
                    size_t ae = paren_end;
                    int has_arrow = 0;
                    int is_closure0 = 0;
                    size_t k;
                    int depth = 0;
                    as = cc_skip_ws_and_comments(src, ae, as);
                    ae = cc_rskip_ws_and_comments(src, ae);
                    if (ae < as) ae = as;
                    for (k = as; k + 1 < ae; k++) {
                        char ch = src[k];
                        if (ch == '(' || ch == '[' || ch == '{') depth++;
                        else if (ch == ')' || ch == ']' || ch == '}') {
                            if (depth) depth--;
                        } else if (depth == 0 && ch == '=' && src[k + 1] == '>') {
                            has_arrow = 1;
                            break;
                        }
                    }
                    if (has_arrow) {
                        is_closure0 = 1;
                    } else if (as < ae) {
                        /* Bare / dotted CCClosure0 value: look up arg type. */
                        char aname[96];
                        char aty[256];
                        size_t ni = 0;
                        size_t p = as;
                        while (p < ae && ni + 1 < sizeof(aname) &&
                               cc_is_ident_char(src[p])) {
                            aname[ni++] = src[p++];
                        }
                        aname[ni] = '\0';
                        p = cc_skip_ws_and_comments(src, ae, p);
                        if (ni && p >= ae &&
                            cc__lookup_scoped_ufcs_var_type(src, sep_pos, aname,
                                                           aty, sizeof(aty)) &&
                            strstr(aty, "CCClosure0") != NULL) {
                            is_closure0 = 1;
                        } else if (as + 10 < ae &&
                                   memcmp(src + as, "cc_closure", 10) == 0) {
                            /* cc_closure__N…_make() / similar. */
                            size_t q = as;
                            while (q < ae && src[q] != '(') q++;
                            if (q < ae && src[q] == '(') {
                                const char* mk = src + as;
                                size_t mlen = q - as;
                                if (mlen >= 5 &&
                                    memcmp(mk + mlen - 5, "_make", 5) == 0)
                                    is_closure0 = 1;
                            }
                        }
                    }
                    if (is_closure0) {
                        cc_sb_append_cstr(&out, &out_len, &out_cap,
                                         "cc_nursery_spawn_closure0");
                    } else {
                        nursery_spawn_named = 1;
                        cc_sb_append_cstr(&out, &out_len, &out_cap,
                                         "cc_nursery_spawn_async_named");
                    }
                } else if (strcmp(method_name, "spawnhybrid") == 0) {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_nursery_spawnhybrid_closure0");
                } else if (strcmp(method_name, "close_on") == 0) {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_nursery_add_closing_tx");
                } else if (strcmp(method_name, "spawn_async") == 0) {
                    nursery_spawn_named = 1;
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_nursery_spawn_async_named");
                } else {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_nursery_");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, method_name);
                }
            } else if (atomic_like) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_atomic_");
                cc_sb_append_cstr(&out, &out_len, &out_cap, method_name);
            } else if (map_decl_like) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, recv_type_base);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "_");
                cc_sb_append_cstr(&out, &out_len, &out_cap, method_name);
            } else if (wildcard_like) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, wildcard_callee);
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
            /* An argument list of inert bytes only is an EMPTY list. */
            args_start = cc_skip_ws_and_comments(src, args_end, args_start);
            args_end = cc_rskip_ws_and_comments(src, args_end);
            if (args_end < args_start) args_end = args_start;
            if (args_end > args_start) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                cc_sb_append(&out, &out_len, &out_cap, src + args_start, args_end - args_start);
            }
            /* R1: trailing diag args for task-form spawn / spawn_async. */
            if (nursery_spawn_named) {
                char callee_name[96];
                size_t ci = args_start;
                size_t cj = 0;
                while (ci < args_end && cj + 1 < sizeof(callee_name) &&
                       (cc_is_ident_char(src[ci]) || src[ci] == ':')) {
                    callee_name[cj++] = src[ci++];
                }
                callee_name[cj] = '\0';
                if (cj == 0 || ci >= args_end || src[ci] != '(') {
                    snprintf(callee_name, sizeof(callee_name), "%s", "<async>");
                }
                cc_sb_append_cstr(&out, &out_len, &out_cap, ", \"");
                cc_sb_append_cstr(&out, &out_len, &out_cap, callee_name);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "\", __FILE__, __LINE__");
            }
        }
        cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
        last_emit = paren_end + 1;
        i = paren_end + 1;
    }
    if (last_emit == 0) {
        if (own_idx) {
            cc__ufcs_scope_idx_reset();
            g_ufcs_scope_idx_locked = 0;
        }
        return NULL;
    }
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    if (own_idx) {
        cc__ufcs_scope_idx_reset();
        g_ufcs_scope_idx_locked = 0;
    }
    return out;
}

/* Parser-survival text rewriter for concrete family UFCS. Kept narrow so TCC's
   stub parse sees lowered receiver forms for fragile nested contexts; the AST
   UFCS pass remains authoritative for everything else. */
static int cc__channel_ufcs_recv_expr_char(char c) {
    return cc_is_ident_char(c) || c == '.' || c == '-' || c == '>' || c == ']' || c == ')';
}

static char* cc__rewrite_channel_send_recv_ufcs_parser_safe(const char* src, size_t n) {
    char* out = NULL;
    size_t out_len = 0;
    size_t out_cap = 0;
    size_t last_emit = 0;
    CCScannerState scan;
    if (!src || n == 0) return NULL;
    cc_scanner_init(&scan);
    for (size_t i = 0; i + 6 < n; ) {
        const char* fn = NULL;
        size_t method_len = 0;
        size_t recv_start;
        size_t open;
        size_t close = 0;
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (memcmp(src + i, ".send(", 6) == 0) {
            fn = "cc_channel_send";
            method_len = 5;
        } else if (memcmp(src + i, ".recv(", 6) == 0) {
            fn = "cc_channel_recv";
            method_len = 5;
        } else {
            i++;
            continue;
        }
        recv_start = i;
        while (recv_start > 0 && cc__channel_ufcs_recv_expr_char(src[recv_start - 1])) recv_start--;
        if (recv_start == i) {
            i++;
            continue;
        }
        if (cc__ufcs_preceded_by_await(src, recv_start)) {
            i++;
            continue;
        }
        {
            int has_member_chain = memchr(src + recv_start, '.', i - recv_start) != NULL;
            if (!has_member_chain) {
                for (size_t q = recv_start; q + 1 < i; q++) {
                    if (src[q] == '-' && src[q + 1] == '>') {
                        has_member_chain = 1;
                        break;
                    }
                }
            }
            if (!has_member_chain) {
                i++;
                continue;
            }
        }
        open = i + method_len;
        if (open >= n || src[open] != '(' || !cc_find_matching_paren(src, n, open, &close)) {
            i++;
            continue;
        }
        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, recv_start - last_emit);
        cc_sb_append_cstr(&out, &out_len, &out_cap, fn);
        cc_sb_append_cstr(&out, &out_len, &out_cap, "(");
        cc_sb_append(&out, &out_len, &out_cap, src + recv_start, i - recv_start);
        if (close > open + 1) {
            cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
            cc_sb_append(&out, &out_len, &out_cap, src + open + 1, close - open - 1);
        }
        cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
        last_emit = close + 1;
        i = close + 1;
    }
    if (!out) return NULL;
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* ---- free-call family symmetry ------------------------------------- *
 *
 * `name(arg1, …)` where `name` has no visible declaration is the prefix
 * spelling of `arg1.name(…)`: one resolution, two spellings. The family
 * callee composes from arg1's type exactly as the method form would and
 * is used only when that function is verifiably declared. A real
 * declaration or macro named `name` always wins — such call sites are
 * left untouched, as is any site whose composition or verification
 * fails (those remain the implicit-declaration errors they are today).
 */

static int cc__free_call_name_is_keyword(const char* s, size_t len) {
    static const char* const kws[] = {
        "if",       "while",   "for",     "switch",  "return",
        "sizeof",   "do",      "else",    "case",    "goto",
        "typedef",  "defined", "select",  "await",   "spawn",
        "_Generic", "_Alignof", "_Static_assert", "__typeof__", "typeof",
        NULL,
    };
    size_t k;
    for (k = 0; kws[k]; ++k)
        if (strlen(kws[k]) == len && memcmp(s, kws[k], len) == 0) return 1;
    return 0;
}

/* Statement keywords that legitimately precede a call expression:
 * `return println(...)`, `else println(...)`, … — an identifier char
 * before the name is a declaration shape ONLY when the preceding word
 * is not one of these. */
/* Backward skip over spaces, tabs and `/ * ... * /` block comments only —
 * for probes whose acceptance must NOT start crossing bare newlines.
 * `pos` is an exclusive end; returns the new exclusive end.  Same rewind
 * idiom as the await-lookback (search left for the comment opener;
 * adjacent `* /` cannot occur in real code outside a comment). */
static size_t cc__rskip_sp_tab_block_comments(const char* src, size_t pos) {
    for (;;) {
        while (pos > 0 && (src[pos - 1] == ' ' || src[pos - 1] == '\t')) pos--;
        if (pos >= 2 && src[pos - 1] == '/' && src[pos - 2] == '*') {
            size_t c = pos - 2, open = (size_t)-1;
            while (c > 0) {
                c--;
                if (src[c] == '*' && c > 0 && src[c - 1] == '/') { open = c - 1; break; }
            }
            if (open == (size_t)-1) return pos;
            pos = open;
            continue;
        }
        return pos;
    }
}

static int cc__free_call_prev_word_is_stmt_kw(const char* src, size_t b) {
    size_t e, s;
    static const char* const kws[] = {
        "return", "else", "case", "do", "goto", NULL,
    };
    size_t k;
    /* Skip whitespace and block comments backwards so
     * `return / *c* / println(x)` still reads `return`; keep the walk
     * same-line (bare newlines end it, as before). */
    e = cc__rskip_sp_tab_block_comments(src, b);
    s = e;
    while (s > 0 && cc_is_ident_char(src[s - 1])) s--;
    if (s == e) return 0;
    for (k = 0; kws[k]; ++k)
        if (strlen(kws[k]) == e - s && memcmp(src + s, kws[k], e - s) == 0)
            return 1;
    return 0;
}

/* Sorted unique identifier sets for free-call / included-.cch decl probes.
 * Built once per rewrite (or TU include set) instead of rescanning text per name. */
typedef struct {
    char** names;
    size_t n;
    size_t cap;
} CCNameSet;

static int cc__callable_name_cmp(const void* a, const void* b);

static void cc__name_set_free(CCNameSet* s) {
    size_t i;
    if (!s) return;
    for (i = 0; i < s->n; i++) free(s->names[i]);
    free(s->names);
    s->names = NULL;
    s->n = s->cap = 0;
}

static int cc__name_set_push(CCNameSet* s, const char* name, size_t nlen) {
    char* copy;
    char** nv;
    if (!s || !name || nlen == 0 || nlen >= 192) return -1;
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 64;
        nv = (char**)realloc(s->names, cap * sizeof(*nv));
        if (!nv) return -1;
        s->names = nv;
        s->cap = cap;
    }
    copy = (char*)malloc(nlen + 1);
    if (!copy) return -1;
    memcpy(copy, name, nlen);
    copy[nlen] = 0;
    s->names[s->n++] = copy;
    return 0;
}

static void cc__name_set_finalize(CCNameSet* s) {
    size_t w = 0, r;
    if (!s || s->n <= 1) return;
    qsort(s->names, s->n, sizeof(char*), cc__callable_name_cmp);
    for (r = 0; r < s->n; r++) {
        if (w > 0 && strcmp(s->names[w - 1], s->names[r]) == 0) {
            free(s->names[r]);
            continue;
        }
        s->names[w++] = s->names[r];
    }
    s->n = w;
}

static int cc__name_set_has(const CCNameSet* s, const char* name) {
    char* key = (char*)name;
    if (!s || !s->names || !name || !name[0]) return 0;
    return bsearch(&key, s->names, s->n, sizeof(char*),
                   cc__callable_name_cmp) != NULL;
}

/* Walk back past whitespace/comments to the previous code char before `pos`.
 * `cc_rskip_ws_and_comments` handles block AND line comments (a trailing
 * `// ...` on the previous line no longer reads as the previous token). */
static size_t cc__tu_decl_prev_code(const char* src, size_t pos) {
    return cc_rskip_ws_and_comments(src, pos);
}

/* `T !>(E) name(` — the name sits after the `)` of `!>(E)`, not a type. */
static int cc__prev_is_result_bang_err(const char* src, size_t b) {
    size_t rp, lp, k, depth;
    if (!src || b == 0 || src[b - 1] != ')') return 0;
    rp = b - 1;
    depth = 1;
    lp = rp;
    while (lp > 0) {
        lp--;
        if (src[lp] == ')') depth++;
        else if (src[lp] == '(') {
            depth--;
            if (depth == 0) break;
        }
    }
    if (depth != 0) return 0;
    k = cc_rskip_ws_and_comments(src, lp);
    if (k < 2 || src[k - 1] != '>' || src[k - 2] != '!') return 0;
    return 1;
}

static int cc__decl_prev_ok(const char* src, size_t b) {
    if (b == 0) return 0;
    if (src[b - 1] == '*') return 1;
    /* `char[:] name(` / `T[:] name(` — slice sugar ends on `]`. */
    if (src[b - 1] == ']') return 1;
    /* `T !>(E) name(` — Result err-type paren. */
    if (src[b - 1] == ')' && cc__prev_is_result_bang_err(src, b)) return 1;
    if (cc_is_ident_char(src[b - 1]) &&
        !cc__free_call_prev_word_is_stmt_kw(src, b))
        return 1;
    return 0;
}

static int cc__tu_ident_paren_is_decl(const char* src, size_t n, size_t name_pos,
                                      size_t nlen) {
    size_t q = name_pos + nlen;
    size_t b;
    while (q < n && (src[q] == ' ' || src[q] == '\t')) q++;
    if (q >= n || src[q] != '(') return 0;
    b = cc__tu_decl_prev_code(src, name_pos);
    return cc__decl_prev_ok(src, b);
}




/* Nonzero when a decl-shaped occurrence of `name(` exists: the
 * occurrence's previous code token ends in an identifier char or `*`
 * (a type or declarator precedes it). Call sites are preceded by
 * operators/punctuation and don't match. */
static int cc__tu_declares_fn(const char* src, size_t n, const char* name) {
    size_t nlen = strlen(name);
    size_t i = 0;
    CCScannerState scan;
    if (!nlen) return 0;
    cc_scanner_init(&scan);
    while (i + nlen <= n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (src[i] != name[0]) { i++; continue; }
        if (i > 0 && cc_is_ident_char(src[i - 1])) { i++; continue; }
        if (i + nlen > n || memcmp(src + i, name, nlen) != 0) { i++; continue; }
        if (i + nlen < n && cc_is_ident_char(src[i + nlen])) { i += nlen; continue; }
        if (cc__tu_ident_paren_is_decl(src, n, i, nlen)) return 1;
        i += nlen;
    }
    return 0;
}




/* Naming grid: the unprefixed container spellings `Vec::[T]` /
 * `vec_new::[T]` are the canonical surface, matching `Map::[K, V]` /
 * `map_new::[K, V]`; the CC-prefixed twins remain accepted as the
 * instance/mangled layer (`Vec::[int]` and `CCVec::[int]` both name
 * `CCVec_int`). Normalized here, before any recognizer, so every
 * downstream pass sees one spelling. Comment/string-aware. */
/* Naked print names at call position alias the declared `cc_print*` family
 * (script/stdio.cch). Fixed six-name alias, not a general tier:
 *   print/println/eprint/eprintln(data)     — data only
 *   fprint/fprintln(fd, data)               — fd first (fprintf-shaped)
 * A TU-declared function or macro of the same name wins (real binding
 * always wins). Member position (`s.println()` / `s.fprintln(fd)`) is
 * untouched — the method rails own it. A C preprocessor macro cannot
 * provide these names: a function-like macro expands in member position
 * too and would destroy every postfix spelling. */
static int cc__tu_defines_fnlike_macro(const char* src, size_t n, const char* name) {
    return cc_text_defines_fnlike_macro(src, n, name);
}

char* cc_rewrite_naked_print_aliases(const char* src, size_t n) {
    /* Longest-first so fprintln wins over fprint, etc. */
    static const char* const names[] = {
        "fprintln", "eprintln", "println", "fprint", "eprint", "print"
    };
    enum { CC__NAKED_PRINT_N = 6 };
    char* out = NULL;
    size_t out_len = 0, out_cap = 0, last_emit = 0;
    CCScannerState scan;
    size_t i = 0;
    int usable[CC__NAKED_PRINT_N];
    int checked = 0;
    if (!src || n == 0) return NULL;
    cc_scanner_init(&scan);
    while (i < n) {
        int k;
        size_t nl, e;
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (src[i] != 'p' && src[i] != 'e' && src[i] != 'f') { i++; continue; }
        if (i > 0 && (cc_is_ident_char(src[i - 1]) || src[i - 1] == '.' ||
                      (i > 1 && src[i - 1] == '>' && src[i - 2] == '-'))) {
            i++;
            continue;
        }
        for (k = 0; k < CC__NAKED_PRINT_N; k++) {
            nl = strlen(names[k]);
            if (i + nl < n && memcmp(src + i, names[k], nl) == 0 &&
                !cc_is_ident_char(src[i + nl]))
                break;
        }
        if (k == CC__NAKED_PRINT_N) { i++; continue; }
        nl = strlen(names[k]);
        e = i + nl;
        while (e < n && (src[e] == ' ' || src[e] == '\t')) e++;
        if (e >= n || src[e] != '(') { i += nl; continue; }
        {
            /* Skip back over whitespace and comments: `.  println(` (and
             * `. / *c* / println(`) is member position. */
            size_t b = cc_rskip_ws_and_comments(src, i);
            if (b > 0 && (src[b - 1] == '.' ||
                          (b > 1 && src[b - 1] == '>' && src[b - 2] == '-') ||
                          src[b - 1] == '#' || src[b - 1] == '&' || src[b - 1] == '*')) {
                i += nl;
                continue;
            }
            if (b > 0 && cc_is_ident_char(src[b - 1]) &&
                !cc__free_call_prev_word_is_stmt_kw(src, b)) {
                i += nl; /* declaration shape: `T println(...)` */
                continue;
            }
        }
        if (!checked) {
            for (int j = 0; j < CC__NAKED_PRINT_N; j++)
                usable[j] = !cc__tu_declares_fn(src, n, names[j]) &&
                            !cc__tu_defines_fnlike_macro(src, n, names[j]);
            checked = 1;
        }
        if (!usable[k]) { i += nl; continue; }
        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
        cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_");
        cc_sb_append(&out, &out_len, &out_cap, src + i, nl);
        last_emit = i + nl;
        i += nl;
    }
    if (!out) return NULL;
    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

static char* cc__rewrite_container_surface_aliases(const char* src, size_t n) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0, last_emit = 0;
    CCScannerState scan;
    size_t i = 0;
    if (!src || n == 0) return NULL;
    cc_scanner_init(&scan);
    while (i < n) {
        const char* rep = NULL;
        size_t tok = 0;
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if ((src[i] != 'V' && src[i] != 'v') ||
            (i > 0 && cc_is_ident_char(src[i - 1]))) {
            i++;
            continue;
        }
        if (i + 6 <= n && memcmp(src + i, "Vec::[", 6) == 0) {
            rep = "CCVec";
            tok = 3;
        } else if (i + 10 <= n && memcmp(src + i, "vec_new::[", 10) == 0) {
            rep = "cc_vec_new";
            tok = 7;
        }
        if (!rep) { i++; continue; }
        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
        cc_sb_append_cstr(&out, &out_len, &out_cap, rep);
        i += tok;
        last_emit = i;
    }
    if (!out) return NULL;
    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Ok-type spelling for a concrete Result name, from the spec table
 * (fallback: the mangled middle, which equals the spelling for
 * single-token ok types like CCPyObj). */
static CCResultSpecTable cc__result_specs = {0};
static int cc__result_ok_type_for(const char* concrete, char* out, size_t sz) {
    size_t i;
    if (!concrete || !out || sz == 0) return 0;
    for (i = 0; i < cc__result_specs.count; i++) {
        const CCResultSpec* spec = cc_result_spec_table_get(&cc__result_specs, i);
        if (spec && spec->concrete_name[0] &&
            strcmp(spec->concrete_name, concrete) == 0 && spec->ok_type[0]) {
            snprintf(out, sz, "%s", spec->ok_type);
            return 1;
        }
    }
    if (strncmp(concrete, "CCResult_", 9) == 0) {
        /* No spec entry: split the mangled middle at the LAST `_CC` --
         * error types are CC-prefixed by convention (CCError, CCPyError,
         * CCIoError, ...). The recovered ok segment is its mangled
         * spelling, which equals the type spelling for single-token
         * types; multiword ok types need the spec entry. */
        const char* mid = concrete + 9;
        const char* t = mid;
        const char* last = NULL;
        while ((t = strstr(t, "_CC")) != NULL) {
            last = t;
            t++;
        }
        if (last && last > mid) {
            size_t ml = (size_t)(last - mid);
            if (ml + 1 < sz) {
                memcpy(out, mid, ml);
                out[ml] = 0;
                /* A pointer ok type mangles its star away (`CCDirIter*` ->
                 * `CCDirIterptr`, the same convention as `voidptr` and
                 * `charptr`).  Put it back: the recovered segment is used as a
                 * C type, and `CCDirIterptr` is not one — without this the
                 * fallback yields a token that cannot compile, which reads as
                 * a puzzling error in generated code rather than as the
                 * unmangling that did not happen. */
                if (ml > 3 && strcmp(out + ml - 3, "ptr") == 0 && ml + 1 < sz) {
                    out[ml - 3] = '*';
                    out[ml - 2] = 0;
                }
                return 1;
            }
        }
    }
    return 0;
}

/* Fallible chains: `!>` is the link. Each hop unwraps, then the next
 * dispatches on the unwrapped value:
 *
 *   py.a() !> .b() !>;              every hop spells its unwrap
 *   py.a() !>(e) { ... } .b() !>;   per-link recovery
 *
 * Lowered by hoisting each linked hop to its own statement binding a
 * temporary of the hop's ok type, so every `!>` stays in statement
 * position with its written meaning (bare targets the enclosing
 * @errhandler; handlers keep their control flow), and the final hop
 * stays attached to the original destination -- destination-typed
 * extraction works unchanged. Chains therefore live where statements
 * do: at statement position or as the whole right-hand side of a
 * declaration or assignment. Fires once the producing call is in
 * lowered form (`F(args)` with a readable CCResult_* return); the
 * engine loop alternates with the UFCS pass until neither changes.
 * `?>` never links -- parenthesize instead. */
static char* cc__rewrite_result_chain_links(const char* src, size_t n) {
    static _Thread_local int g_rc_tmp_id;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0, last_emit = 0;
    CCScannerState scan;
    size_t i = 0;
    if (!src || n == 0) return NULL;
    cc_scanner_init(&scan);
    size_t cur_stmt = 0; /* code position after the last ;/{/} (comment-aware) */
    while (i + 1 < n) {
        size_t sep_end, q, estart, eend, fs, fe, link_dot, stmt_head;
        char fname[128];
        char rt[256];
        char okty[128];
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (src[i] == ';' || src[i] == '{' || src[i] == '}') {
            cur_stmt = i + 1;
            i++;
            continue;
        }
        if (src[i] != '!' || src[i + 1] != '>') { i++; continue; }
        if (i > 0 && src[i - 1] == '!') { i += 2; continue; }
        /* Separator: `!>` alone, or with `(e)` binder and/or `{...}`. */
        sep_end = i + 2;
        q = cc_skip_ws_and_comments(src, n, sep_end);
        if (q < n && src[q] == '(') {
            size_t pe;
            if (!cc_find_matching_paren(src, n, q, &pe)) { i += 2; continue; }
            q = cc_skip_ws_and_comments(src, n, pe + 1);
            if (q >= n || src[q] != '{') { i += 2; continue; }
        }
        if (q < n && src[q] == '{') {
            size_t be;
            if (!cc_find_matching_brace(src, n, q, &be)) { i += 2; continue; }
            sep_end = be + 1;
        }
        /* A link only when a call follows the separator. */
        link_dot = cc_skip_ws_and_comments(src, n, sep_end);
        if (link_dot >= n || src[link_dot] != '.') { i += 2; continue; }
        q = cc_skip_ws_and_comments(src, n, link_dot + 1);
        if (q >= n || !cc_is_ident_start(src[q])) { i += 2; continue; }
        while (q < n && cc_is_ident_char(src[q])) q++;
        q = cc_skip_ws_and_comments(src, n, q);
        /* A member-generic hop, `.ident::[T](args)`, is a link too: the hop
         * hoisted before it gives this one a typed receiver, at which point
         * the member normalization resolves the `::[...]` and instantiates.
         * Not recognizing it left the `!>` for the statement-unwrap form,
         * which consumed the producer and shredded the rest of the line. */
        if (q + 2 < n && src[q] == ':' && src[q + 1] == ':') {
            size_t br = cc_skip_ws_and_comments(src, n, q + 2);
            size_t rb = 0;
            if (br >= n || src[br] != '[' ||
                !cc__find_matching_bracket(src, n, br, &rb)) { i += 2; continue; }
            q = cc_skip_ws_and_comments(src, n, rb + 1);
        }
        if (q >= n || src[q] != '(') { i += 2; continue; }
        /* Producer: one lowered call `F(args)` ending just before `!>`.
         * Comments may sit between `)` and `!>`. */
        eend = cc_rskip_ws_and_comments(src, i);
        if (eend <= last_emit || src[eend - 1] != ')') { i += 2; continue; }
        {
            /* Find the `(` matching the final `)` via the masked backward
             * scan (comments/strings inside the arg span are inert).
             * Excluding the final `)` leaves the producer's `(` unmatched,
             * so the scan stops just after it. */
            size_t p = cc_rfind_char_top_level(src, last_emit, eend - 1, "");
            size_t lp = p;
            while (lp > last_emit && (src[lp - 1] == ' ' || src[lp - 1] == '\t'))
                lp--;
            if (lp <= last_emit || src[lp - 1] != '(') { i += 2; continue; }
            fe = lp - 1;
            while (fe > last_emit && (src[fe - 1] == ' ' || src[fe - 1] == '\t'))
                fe--;
            fs = fe;
            while (fs > last_emit && cc_is_ident_char(src[fs - 1])) fs--;
            if (fs == fe || !cc_is_ident_start(src[fs])) { i += 2; continue; }
            if (fs > last_emit &&
                (src[fs - 1] == '.' || src[fs - 1] == '>')) { i += 2; continue; }
            estart = fs;
        }
        if (fe - fs >= sizeof(fname)) { i += 2; continue; }
        memcpy(fname, src + fs, fe - fs);
        fname[fe - fs] = 0;
        if (!cc__fn_return_type(src, n, fname, rt, sizeof(rt))) {
            if (getenv("CC_DEBUG_RCHAIN"))
                fprintf(stderr, "[rchain] no return type for '%s'\n", fname);
            i += 2; continue;
        }
        if (strncmp(rt, "CCResult_", 9) != 0) {
            if (getenv("CC_DEBUG_RCHAIN"))
                fprintf(stderr, "[rchain] '%s' returns '%s' (not Result)\n", fname, rt);
            i += 2; continue;
        }
        if (!cc__result_ok_type_for(rt, okty, sizeof(okty))) {
            if (getenv("CC_DEBUG_RCHAIN"))
                fprintf(stderr, "[rchain] no ok type for '%s'\n", rt);
            i += 2; continue;
        }
        if (getenv("CC_DEBUG_RCHAIN"))
            fprintf(stderr, "[rchain] link: %s -> %s ok=%s\n", fname, rt, okty);
        /* Hoist point: the chain must sit at statement position or be
         * the whole RHS of a declaration/assignment -- the span from
         * the statement head to the chain carries no open paren. */
        {
            /* The chain must be the statement, or the whole RHS of the
             * statement's one `=`: from the (comment-aware) statement
             * head, only declarator tokens may precede the producer. */
            size_t b = estart;
            int bad = 0;
            int saw_eq = 0;
            stmt_head = cc_skip_ws_and_comments(src, n, cur_stmt);
            if (stmt_head > estart || stmt_head < last_emit) {
                if (getenv("CC_DEBUG_RCHAIN"))
                    fprintf(stderr, "[rchain] stmt head out of range\n");
                i += 2; continue;
            }
            q = stmt_head;
            while (q < estart) {
                size_t q2 = cc_skip_ws_and_comments(src, estart, q);
                if (q2 > q) { q = q2; continue; }
                if (src[q] == '=') {
                    if (saw_eq || (q + 1 < n && src[q + 1] == '=') ||
                        (q > stmt_head && (src[q - 1] == '=' || src[q - 1] == '!' ||
                                           src[q - 1] == '<' || src[q - 1] == '>'))) {
                        bad = 1;
                        break;
                    }
                    saw_eq = 1;
                    b = q; /* RHS begins after this */
                    q++;
                    continue;
                }
                if (cc_is_ident_char(src[q]) || src[q] == '*' || src[q] == ' ' ||
                    src[q] == '\t' || src[q] == '\n' || src[q] == '\r' ||
                    src[q] == '[' || src[q] == ']' || src[q] == ':' ||
                    src[q] == '.' || src[q] == '-' || src[q] == '>') {
                    q++;
                    continue;
                }
                bad = 1;
                break;
            }
            if (bad || (!saw_eq && stmt_head != estart)) {
                if (getenv("CC_DEBUG_RCHAIN"))
                    fprintf(stderr, "[rchain] not statement-position: |%.*s|\n",
                            (int)(estart - stmt_head), src + stmt_head);
                i += 2;
                continue;
            }
            (void)b;
        }
        /* Hoist one linked hop per pass: `OK __cc_rc_N = <call><sep>;`
         * then reattach the next `.m(...)` (and any further `!>` links)
         * to the original destination. Further Result hops wait until
         * UFCS lowers them to plain calls so each temp gets that hop's
         * own ok type — not the first producer's. */
        {
            int id = ++g_rc_tmp_id;
            char head[192];
            char tmp[48];
            size_t m, npe, next_s, next_e;
            m = cc_skip_ws_and_comments(src, n, sep_end);
            next_s = m; /* at '.' */
            m = cc_skip_ws_and_comments(src, n, m + 1);
            while (m < n && cc_is_ident_char(src[m])) m++;
            m = cc_skip_ws_and_comments(src, n, m);
            /* Same hop grammar as the recognition scan above — a re-walk that
             * accepts less than the recognizer turns "link found" into a
             * silent no-op, and the `!>` then falls to the statement-unwrap
             * form, which shreds the line. */
            if (m + 2 < n && src[m] == ':' && src[m + 1] == ':') {
                size_t br2 = cc_skip_ws_and_comments(src, n, m + 2);
                size_t rb2 = 0;
                if (br2 >= n || src[br2] != '[' ||
                    !cc__find_matching_bracket(src, n, br2, &rb2)) {
                    i = sep_end;
                    continue;
                }
                m = cc_skip_ws_and_comments(src, n, rb2 + 1);
            }
            if (m >= n || src[m] != '(' ||
                !cc_find_matching_paren(src, n, m, &npe)) {
                i = sep_end;
                continue;
            }
            next_e = npe + 1;
            snprintf(head, sizeof(head), "%s __cc_rc_%d = ", okty, id);
            snprintf(tmp, sizeof(tmp), "__cc_rc_%d", id);
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit,
                         stmt_head - last_emit);
            cc_sb_append_cstr(&out, &out_len, &out_cap, head);
            cc_sb_append(&out, &out_len, &out_cap, src + estart,
                         sep_end - estart);
            cc_sb_append_cstr(&out, &out_len, &out_cap, ";\n    ");
            cc_sb_append(&out, &out_len, &out_cap, src + stmt_head,
                         estart - stmt_head);
            cc_sb_append_cstr(&out, &out_len, &out_cap, tmp);
            cc_sb_append(&out, &out_len, &out_cap, src + next_s,
                         next_e - next_s);
            last_emit = next_e;
            i = next_e;
        }
    }
    if (!out) return NULL;
    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}


char* cc_rewrite_generic_family_ufcs_parser_safe(const char* src, size_t n,
                                                const char* input_path) {
    char* cur = NULL;
    size_t cur_len = n;
    int hit_cap = 0;
    if (!src || n == 0) return NULL;
    /* Free-call family rewrite runs once in phase-1 (before template
     * lowering so `@string` args are still visible). Re-running it inside
     * every parser-safe entry was redundant and dominated large-TU cost. */
    for (int iter = 0; iter < 8; ++iter) {
        const char* in = cur ? cur : src;
        int changed = 0;
        char* next = cc__rewrite_generic_family_ufcs_impl(in, cur_len, 1, input_path);
        if (next) {
            if (cur) free(cur);
            cur = next;
            cur_len = strlen(cur);
            changed = 1;
        }
        /* Fallible chains lower once their producer is a plain call;
         * alternate with the UFCS pass until neither changes. */
        in = cur ? cur : src;
        next = cc__rewrite_result_chain_links(in, cur_len);
        if (next) {
            if (cur) free(cur);
            cur = next;
            cur_len = strlen(cur);
            changed = 1;
        }
        /* A chain hop can BE a member-generic call: the hop before it binds a
         * temp whose declared type only exists in this rewritten text, so the
         * member spelling cannot resolve until now.  Normalize it to the
         * free-name spelling and instantiate — safe at this depth because the
         * factory reflects on the pristine snapshot, not on this buffer. */
        in = cur ? cur : src;
        if (memmem(in, cur_len, "::[", 3)) {
            next = cc_rewrite_generic_containers(in, cur_len, input_path);
            if (next == (char*)-1) {
                free(cur);
                return (char*)-1;
            }
            if (next) {
                if (cur) free(cur);
                cur = next;
                cur_len = strlen(cur);
                changed = 1;
            }
        }
        if (getenv("CC_DEBUG_FIXPOINT_DUMP")) {
            char dp[256];
            FILE* df;
            snprintf(dp, sizeof(dp), "%s/fixpoint_%d.c",
                     getenv("CC_DEBUG_FIXPOINT_DUMP"), iter);
            df = fopen(dp, "w");
            if (df) { fwrite(cur ? cur : src, 1, cur_len, df); fclose(df); }
        }
        if (!changed) {
            hit_cap = 0;
            break;
        }
        hit_cap = (iter == 7);
    }
    if (hit_cap) {
        fprintf(stderr,
                "<input>:1:1: warning: type: UFCS parser-safe rewrite hit "
                "iteration cap (8); deeper method chains may be incomplete\n");
    }
    {
        const char* in = cur ? cur : src;
        char* chan = cc__rewrite_channel_send_recv_ufcs_parser_safe(in, cur_len);
        if (chan) {
            if (cur) free(cur);
            cur = chan;
            cur_len = strlen(cur);
        }
    }
    if (g_ufcs_typeformal_err) {
        g_ufcs_typeformal_err = 0;
        free(cur);
        return (char*)-1;
    }
    return cur;
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
/* Type qualifiers a Result's ok type may carry.  Storage class legitimately
 * varies between declarations of the same box; these do not — a box holding
 * `char*` and one holding `const char*` are the same box, so two spellings
 * that disagree here silently share whichever arrived first. */
#define CC_TYQ_CONST     1u
#define CC_TYQ_VOLATILE  2u
#define CC_TYQ_RESTRICT  4u
#define CC_TYQ_ATOMIC    8u

static size_t cc__skip_leading_decl_specs_ex(const char* s, size_t ty_start,
                                             unsigned* out_quals) {
    size_t p = ty_start;
    if (out_quals) *out_quals = 0;
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
    p = (size_t)(cc_skip_ws_and_comments_ptr(s + p) - s);
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
                    if (out_quals) {
                        if (strcmp(kw[k], "const") == 0)         *out_quals |= CC_TYQ_CONST;
                        else if (strcmp(kw[k], "volatile") == 0) *out_quals |= CC_TYQ_VOLATILE;
                        else if (strcmp(kw[k], "restrict") == 0) *out_quals |= CC_TYQ_RESTRICT;
                        else if (strcmp(kw[k], "_Atomic") == 0)  *out_quals |= CC_TYQ_ATOMIC;
                    }
                    p += kn;
                    matched = 1;
                    break;
                }
            }
        }
        if (!matched) break;
        /* Comments between decl-specs are inert filler, like the spaces. */
        p = (size_t)(cc_skip_ws_and_comments_ptr(s + p) - s);
    }
    return p;
}

static size_t cc__skip_leading_decl_specs(const char* s, size_t ty_start) {
    return cc__skip_leading_decl_specs_ex(s, ty_start, NULL);
}

/* Which qualifiers the first declaration of a concrete box name carried.
 * A later declaration reaching the same name with a different set is sharing
 * a box it does not spell the same way — the language's rule (C applies
 * `const` at the binding site, so one box serves both), but silently, and
 * whichever came first wins for everyone. */
typedef struct { char name[288]; unsigned quals; int line; } CCResultQualSeen;
static CCResultQualSeen* g_result_quals = NULL;
static size_t g_result_qual_count = 0, g_result_qual_cap = 0;

static void __attribute__((unused)) cc__result_quals_reset(void) {
    free(g_result_quals);
    g_result_quals = NULL;
    g_result_qual_count = g_result_qual_cap = 0;
}

static void cc__qual_names(unsigned q, char* out, size_t sz) {
    out[0] = 0;
    if (!q) { snprintf(out, sz, "unqualified"); return; }
    if (q & CC_TYQ_CONST)    strncat(out, "const ",    sz - strlen(out) - 1);
    if (q & CC_TYQ_VOLATILE) strncat(out, "volatile ", sz - strlen(out) - 1);
    if (q & CC_TYQ_RESTRICT) strncat(out, "restrict ", sz - strlen(out) - 1);
    if (q & CC_TYQ_ATOMIC)   strncat(out, "_Atomic ",  sz - strlen(out) - 1);
    { size_t l = strlen(out); if (l && out[l - 1] == ' ') out[l - 1] = 0; }
}

/* Returns 1 when this sighting conflicts with an earlier one. */
static int cc__result_quals_note(const char* concrete, unsigned quals, int line,
                                 unsigned* out_first, int* out_first_line) {
    for (size_t i = 0; i < g_result_qual_count; i++) {
        if (strcmp(g_result_quals[i].name, concrete) != 0) continue;
        if (g_result_quals[i].quals == quals) return 0;
        if (out_first) *out_first = g_result_quals[i].quals;
        if (out_first_line) *out_first_line = g_result_quals[i].line;
        return 1;
    }
    if (g_result_qual_count == g_result_qual_cap) {
        size_t nc = g_result_qual_cap ? g_result_qual_cap * 2 : 32;
        CCResultQualSeen* g = (CCResultQualSeen*)realloc(g_result_quals, nc * sizeof(*g));
        if (!g) return 0;
        g_result_quals = g;
        g_result_qual_cap = nc;
    }
    snprintf(g_result_quals[g_result_qual_count].name,
             sizeof(g_result_quals[g_result_qual_count].name), "%s", concrete);
    g_result_quals[g_result_qual_count].quals = quals;
    g_result_quals[g_result_qual_count].line = line;
    g_result_qual_count++;
    return 0;
}

/* Mangle a type name for use in CCResult_T_E.
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
                /* Rewind directly-abutting block comments so `T/ *c* /?`
                 * still reads `T` as the receiver (whitespace is
                 * intentionally NOT skipped — `T ?` is not a candidate
                 * today and stays that way). */
                size_t pe = i;
                while (pe >= 2 && src[pe - 1] == '/' && src[pe - 2] == '*') {
                    size_t q = pe - 2, open = (size_t)-1;
                    while (q > 0) {
                        q--;
                        if (src[q] == '*' && q > 0 && src[q - 1] == '/') { open = q - 1; break; }
                    }
                    if (open == (size_t)-1) break;
                    pe = open;
                }
                char prev = pe > 0 ? src[pe - 1] : src[0];
                if (pe > 0 &&
                    (cc_is_ident_char(prev) || prev == ')' || prev == ']' || prev == '>')) {
                    size_t ident_end = pe;
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



/* ============================================================================
 * Generic container syntax lowering: CCVec::[T] -> CCVec_T, Map<K,V> -> Map_K_V
 * Also: cc_vec_new::[T](...) -> CCVec_T_init(...), map_new<K,V>(...) -> Map_K_V_init(...)
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

    if ((strncmp(type, "CCVec::[", 8) == 0 && type[strlen(type) - 1] == ']') ||
        (strncmp(type, "__CC_VEC(", 9) == 0 && type[strlen(type) - 1] == ')')) {
        size_t prefix = (strncmp(type, "__CC_VEC(", 9) == 0) ? 9 :
                        8;
        size_t inner_len = strlen(type) - prefix - 1;
        if (inner_len >= sizeof(inner)) inner_len = sizeof(inner) - 1;
        memcpy(inner, type + prefix, inner_len);
        inner[inner_len] = '\0';
        cc__canonicalize_container_param_type(inner, sizeof(inner));
        cc__mangle_container_type_param(inner, strlen(inner), mangled_inner, sizeof(mangled_inner));
        snprintf(type, type_sz, "CCVec_%s", mangled_inner);
        return;
    }

    if ((strncmp(type, "ArrayMap::[", 11) == 0 && type[strlen(type) - 1] == ']') ||
        (strncmp(type, "__CC_ARRAY_MAP(", 15) == 0 && type[strlen(type) - 1] == ')') ||
        (strncmp(type, "Map::[", 6) == 0 && type[strlen(type) - 1] == ']') ||
        (strncmp(type, "Map<", 4) == 0 && type[strlen(type) - 1] == '>') ||
        (strncmp(type, "__CC_MAP(", 9) == 0 && type[strlen(type) - 1] == ')')) {
        int is_array = (strncmp(type, "ArrayMap::[", 11) == 0) ||
                       (strncmp(type, "__CC_ARRAY_MAP(", 15) == 0);
        size_t prefix = is_array
                            ? ((strncmp(type, "__CC_ARRAY_MAP(", 15) == 0) ? 15 : 11)
                            : ((strncmp(type, "__CC_MAP(", 9) == 0) ? 9
                                                                   : ((type[3] == ':') ? 6 : 4));
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
        snprintf(type, type_sz, "%s_%s_%s", is_array ? "ArrayMap" : "Map", mangled_key, mangled_val);
        return;
    }
}

static void cc__canonicalize_ufcs_alias_target(char* out, size_t out_sz, const char* type_src) {
    size_t len;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!type_src || !type_src[0]) return;
    snprintf(out, out_sz, "%s", type_src);
    cc__canonicalize_container_param_type(out, out_sz);
    if (!(out[0] && strcmp(out, type_src) != 0)) {
        cc__normalize_ufcs_type_name(out, out_sz, type_src);
        cc__canonicalize_container_param_type(out, out_sz);
    }
    /* Map/ArrayMap sugar is a pointer handle (`typedef __CC_MAP(...)* Name`).
     * Keep that pointer-ness on the alias so UFCS passes `recv`, not `&recv`. */
    len = strlen(out);
    if (len > 0 && out[len - 1] != '*' &&
        (strncmp(out, "ArrayMap_", 9) == 0 || strncmp(out, "Map_", 4) == 0) &&
        len + 1 < out_sz) {
        out[len] = '*';
        out[len + 1] = '\0';
    }
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

/* Copy the type-argument span `src[s..e)` into `out` as code-only text:
 * comments are dropped and each whitespace-or-comment run collapses to one
 * space, so `unsigned` and `long` stay two tokens.  A type argument feeds
 * both the mangled instance name and the text handed to a generic factory,
 * so a comment that survives the capture lands inside a C identifier. */
static void cc__copy_type_arg_text(const char* src, size_t s, size_t e,
                                   char* out, size_t out_sz) {
    size_t j = 0;
    if (!out || out_sz == 0) return;
    out[0] = 0;
    if (!src || s >= e) return;
    s = cc_skip_ws_and_comments(src, e, s);
    while (s < e && j + 1 < out_sz) {
        size_t k = cc_skip_ws_and_comments(src, e, s);
        if (k > s) {
            s = k;
            if (s < e && j > 0) out[j++] = ' ';
            continue;
        }
        out[j++] = src[s++];
    }
    while (j > 0 && out[j - 1] == ' ') j--;
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

/* Rewrite generic container syntax (canonical bracket form only):
   - CCVec::[T] -> __CC_VEC(T_mangled)  (parser-safe macro)
   - Map::[K, V] -> __CC_MAP(K_mangled, V_mangled)*  (parser-safe macro)
   - ArrayMap::[K, V] -> __CC_ARRAY_MAP(K_mangled, V_mangled)*
   - cc_vec_new::[T](...) -> __CC_VEC_INIT(T_mangled, ...)
   - map_new::[K, V](...) -> __CC_MAP_INIT(K_mangled, V_mangled, ...)
   - array_map_new::[K, V](...) -> __CC_ARRAY_MAP_INIT(...)
   - array_map_new_count::[K, V](arena, n) -> __CC_ARRAY_MAP_INIT_COUNT(...)
   The angle-bracket spellings (Vec<T>, CCVec<T>, Map<K, V>, vec_new<T>,
   map_new<K, V>, ...) and the prefixless Vec::[T]/vec_new::[T] are retired:
   they are detected only to emit a migration error.  `::[ ... ]` is the single
   instantiation surface for both built-in containers and user generic
   factories (`Name::[args]`).  Also tracks variable declarations for UFCS. */
/* The TU as it read when generic instantiation FIRST ran for it.
 *
 * A factory reflects on the source it is handed, and the two spellings of an
 * instantiation must hand it the same source — but a chain cannot resolve a
 * member-generic hop until the previous hop's temp has a type, which is many
 * passes later, when `!>(CCError)` is already lowered and a fallible method no
 * longer looks fallible.  Snapshotting the buffer at the first containers
 * entry decouples WHEN an instantiation happens from WHAT its factory sees:
 * a late site reflects on the same bytes an early one did. */
static char* g_reflect_snapshot = NULL;
static size_t g_reflect_snapshot_len = 0;
static char g_reflect_snapshot_path[1024];

static void cc__reflect_snapshot_capture(const char* src, size_t n,
                                         const char* input_path) {
    const char* key = input_path ? input_path : "<input>";
    if (g_reflect_snapshot && strcmp(g_reflect_snapshot_path, key) == 0) return;
    free(g_reflect_snapshot);
    g_reflect_snapshot = (char*)malloc(n + 1);
    if (!g_reflect_snapshot) { g_reflect_snapshot_len = 0; return; }
    memcpy(g_reflect_snapshot, src, n);
    g_reflect_snapshot[n] = '\0';
    g_reflect_snapshot_len = n;
    snprintf(g_reflect_snapshot_path, sizeof(g_reflect_snapshot_path), "%s", key);
}


/* Split a `::[ ... ]` type-argument list into original and mangled spellings.
 *
 * Shared by the free-name and member spellings for the same reason the
 * instantiation is: they must agree on what `::[K, V]` means down to the
 * canonicalization, or the same text would name two different monomorphs.
 * Nesting is tracked so `Map::[int, Vec::[int]]` splits at the top level only.
 * Comments are inert on both counts: they must not read as separators, and
 * they must not survive into a captured argument.
 * Returns the count, or -1 if there are more than `max_args`. */
static int cc__split_type_args(const char* params, size_t params_len,
                               char orig_args[][128], char mang_args[][128],
                               int max_args) {
    size_t a_s = 0;
    int depth = 0, nargs = 0;
    for (size_t k = 0; k <= params_len; ) {
        char pc;
        if (k < params_len) {
            size_t sk = cc_skip_ws_and_comments(params, params_len, k);
            if (sk > k) { k = sk; continue; }
        }
        pc = (k < params_len) ? params[k] : ',';
        if (k < params_len && (pc == '<' || pc == '[' || pc == '(')) { depth++; k++; continue; }
        if (k < params_len && (pc == '>' || pc == ']' || pc == ')')) { depth--; k++; continue; }
        if (pc == ',' && depth == 0) {
            if (nargs >= max_args) return -1;
            cc__copy_type_arg_text(params, a_s, k,
                                   orig_args[nargs], sizeof(orig_args[nargs]));
            if (orig_args[nargs][0]) {
                cc__canonicalize_container_param_type(orig_args[nargs], sizeof(orig_args[nargs]));
                cc__mangle_container_type_param(orig_args[nargs], strlen(orig_args[nargs]),
                                                mang_args[nargs], sizeof(mang_args[nargs]));
                nargs++;
            }
            a_s = k + 1;
        }
        k++;
    }
    return nargs;
}

/* Produce the monomorph for `gname::[args]` and emit its definition once.
 *
 * Shared by the two spellings that request an instantiation: the free-name
 * site (`Name::[T](...)`) and the member site (`recv.member::[T](...)`).  They
 * differ only in how the surrounding call text is rewritten — the receiver
 * moves into argument position for one and not the other — never in how the
 * instance is produced, so producing it lives here and neither spelling can
 * drift from the other's diagnostics.
 *
 * `use_pos` is the offset of the use site (for line/column attribution) and
 * `br_close` the offset of its closing `]` (for the --emit-c-inspect dump).
 * Writes the mangled instance name.  Returns 0, or -1 having reported. */
static int cc__emit_generic_instance(const char* gname,
                                     char orig_args[][128], char mang_args[][128],
                                     int nargs, const char* src, size_t n,
                                     size_t use_pos, size_t br_close,
                                     const char* input_path,
                                     char* mangled, size_t mangled_cap) {
    /* mangled name: Name_marg1_marg2... */
    {
        int mo = snprintf(mangled, mangled_cap, "%s", gname);
        for (int a = 0; a < nargs && mo > 0 && (size_t)mo < mangled_cap; a++)
            mo += snprintf(mangled + mo, mangled_cap - (size_t)mo, "_%s", mang_args[a]);
    }

    /* invoke compiled factory once per mangled name. */
    {
        CCArena def_ar = cc_arena_heap_c(64 * 1024);
        char* def = NULL;
        char rel[1024];
        int use_line = 1, use_col = 1;
        for (size_t k = 0; k < use_pos && k < n; k++) {
            if (src[k] == '\n') { use_line++; use_col = 1; } else { use_col++; }
        }
        if (!cc_arena_is_live(def_ar)) {
            cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                            use_line, use_col, "type",
                            "compiled generic factory '%s' def arena OOM",
                            gname);
            return -1;
        }
        {
            char ferr[512];
            /* Reflect on the snapshot when one exists: a factory must see
             * the same program regardless of which pass requested the
             * instantiation.  Line/col attribution and the inspect dump keep
             * using the live buffer, where the use site actually is. */
            const char* rsrc = g_reflect_snapshot ? g_reflect_snapshot : src;
            size_t rlen = g_reflect_snapshot ? g_reflect_snapshot_len : n;
            CCGenProduceStatus ps = cc_emit_plan_produce_generic_def(
                gname, mangled, orig_args, nargs,
                rsrc, rlen, input_path, &def_ar, &def, ferr, sizeof(ferr));
            if (ps == CC_GEN_PRODUCE_ENSURE_FAILED) {
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                use_line, use_col, "type",
                                "compiled generic factory '%s' failed to compile: %s",
                                gname, ferr);
                {
                    const char* handler = cc_emit_plan_lookup_generic_factory_handler(gname);
                    int hline = handler ? cc_comptime_fn_registry_lookup_line(handler) : 0;
                    const char* hfile = handler ? cc_comptime_fn_registry_lookup_file(handler) : NULL;
                    if (handler && hline > 0) {
                        char hrel[1024];
                        const char* hf = cc_path_rel_to_repo(
                            hfile ? hfile : (input_path ? input_path : "<input>"),
                            hrel, sizeof(hrel));
                        fprintf(stderr,
                                "  note: in @comptime factory '%s' at %s:%d\n",
                                handler, hf, hline);
                    }
                }
                cc_arena_free(&def_ar);
                return -1;
            }
            if (ps == CC_GEN_PRODUCE_INVOKE_FAILED) {
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                use_line, use_col, "type",
                                "compiled generic factory '%s' failed for '%s' with %d type "
                                "argument%s (empty fragment: arity-guard rejection, @emit "
                                "arena OOM, or runtime error)",
                                gname, mangled, nargs, nargs == 1 ? "" : "s");
                cc_arena_free(&def_ar);
                return -1;
            }
        }
        if (!def) def = "";
        /* A factory that raised cc_emit_error reported its own constraint
         * violation; fail here (attributed to the use site) even when the
         * fragment it still returned happens to parse as C. */
        if (cc_emit_plan_take_exec_error()) {
            cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                            use_line, use_col, "type",
                            "compiled generic factory '%s' reported an error for '%s'",
                            gname, mangled);
            cc_arena_free(&def_ar);
            return -1;
        }
        /* Validate the generated definition at the emit site so a malformed
         * factory fails here, attributed to the use site, rather than surfacing
         * as a confusing error deep in the merged translation unit. */
        {
            char verr[512];
            int frag_line = 0;
            if (cc_comptime_validate_c_fragment(def, &frag_line, verr, sizeof(verr)) != 0) {
                if (!cc_emit_plan_generic_invalid_report_once(mangled)) {
                    cc_arena_free(&def_ar);
                    return -1;
                }
                cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                use_line, use_col, "type",
                                "compiled generic factory '%s' produced invalid C for '%s': %s",
                                gname, mangled, verr);
                /* Echo the full generated definition with line numbers, flagging
                 * the offending line (">"), so the factory author sees the C they
                 * actually produced — not just one isolated line. */
                {
                    if (frag_line > 0)
                        fprintf(stderr, "  note: in generated definition, line %d:\n", frag_line);
                    else
                        fprintf(stderr, "  note: in generated definition:\n");
                    size_t dl = strlen(def);
                    while (dl && (def[dl - 1] == '\n' || def[dl - 1] == '\r')) dl--;
                    const char* end = def + dl;
                    int total = (dl == 0) ? 0 : 1;
                    for (const char* p = def; p < end; p++) if (*p == '\n') total++;
                    int lo = 1, hi = total;
                    if (total > 40 && frag_line > 0) {
                        lo = frag_line - 3; if (lo < 1) lo = 1;
                        hi = frag_line + 3; if (hi > total) hi = total;
                    }
                    int cur = 1;
                    const char* ls = def;
                    while (ls < end && cur <= hi) {
                        const char* le = ls;
                        while (le < end && *le != '\n') le++;
                        if (cur >= lo)
                            fprintf(stderr, "  %c %4d | %.*s\n",
                                    cur == frag_line ? '>' : ' ', cur,
                                    (int)(le - ls), ls);
                        ls = le + 1;
                        cur++;
                    }
                    if (hi < total)
                        fprintf(stderr, "  … (%d more line%s)\n",
                                total - hi, (total - hi) == 1 ? "" : "s");
                }
                /* Inspect artifact (--emit-c-inspect): reconstruct the merged
                 * translation unit with the bad definition spliced in at the
                 * prelude and this use site lowered to the mangled name, then
                 * write it to the requested path.  The merged TU is never flushed
                 * when the front-end aborts, so this is the closest inspectable
                 * artifact to the real lowered C — the factory output shown
                 * exactly where it lands.  (It is invalid by construction; the
                 * host compiler is not run on it.  Any *other* generic use sites
                 * after this one stay un-lowered, since the rewrite aborts here —
                 * the dump is faithful up to this first blocking error.)  Without
                 * the flag we just point the user at it. */
                {
                    const char* insp = getenv("CC_EMIT_C_INSPECT");
                    if (insp && insp[0]) {
                        FILE* gf = fopen(insp, "w");
                        if (gf) {
                            fprintf(gf,
                                "/* CC: reconstructed translation unit for '%s' (INVALID).\n"
                                "   The generated definition below failed to parse; it is\n"
                                "   shown in source context up to the first blocking error.\n"
                                "   The factory that produced it is '%s'. */\n",
                                mangled, gname);
                            size_t dl = strlen(def);
                            fwrite(def, 1, dl, gf);
                            if (dl == 0 || def[dl - 1] != '\n') fputc('\n', gf);
                            /* Source with this use site (src[i..br_close]) lowered
                             * to the mangled type name. */
                            fwrite(src, 1, use_pos, gf);
                            fwrite(mangled, 1, strlen(mangled), gf);
                            if (br_close + 1 < n)
                                fwrite(src + br_close + 1, 1, n - (br_close + 1), gf);
                            fclose(gf);
                            fprintf(stderr, "  note: translation unit written to %s\n", insp);
                        }
                    } else {
                        fprintf(stderr, "  note: re-run with --emit-c-inspect to dump the full translation unit\n");
                    }
                }
                {
                    const char* handler = cc_emit_plan_lookup_generic_factory_handler(gname);
                    int hline = handler ? cc_comptime_fn_registry_lookup_line(handler) : 0;
                    const char* hfile = handler ? cc_comptime_fn_registry_lookup_file(handler) : NULL;
                    if (handler && hline > 0) {
                        char hrel[1024];
                        const char* hf = cc_path_rel_to_repo(
                            hfile ? hfile : (input_path ? input_path : "<input>"),
                            hrel, sizeof(hrel));
                        fprintf(stderr,
                                "  note: in @comptime factory '%s' at %s:%d\n",
                                handler, hf, hline);
                    }
                }
                cc_arena_free(&def_ar);
                return -1;
            }
        }
        cc_emit_plan_generic_def_emit_once(mangled, def);
        cc_emit_plan_note_generic_instance(gname, mangled, def);
        /* A generated definition is spliced after the prelude, so the passes
         * that need its signatures cannot read it where it lands.  Register
         * the result-returning ones now: without this, `!>` on a factory's
         * own function has no ok type to unwrap to and silently leaves the
         * Result in place, which fails as a type error nowhere near the
         * cause. */
        cc_result_fn_registry_scan_source(def, strlen(def));
        cc_arena_free(&def_ar);
    }
    return 0;
}

/* D6.0: handle a user generic-factory use site `Name::[arg, ...]`.  Returns 1 if
 * `*io_i` is at such a site for a library-registered template (output appended,
 * indices advanced past the `]`), 0 otherwise (caller falls through to the
 * built-in CCVec/Map handling).  The library owns the C lowering (its template);
 * the compiler owns the mangle (`Name_arg1_arg2`), the dedup, and the splice. */
static int cc__try_rewrite_user_generic(const char* src, size_t n, const char* input_path,
                                        char** out, size_t* out_len, size_t* out_cap,
                                        size_t* io_i, size_t* io_last_emit) {
    size_t i = *io_i;
    size_t id_e, br_open, br_close, params_len;
    const char* params;
    char gname[128];
    char member[128];
    size_t gen_lb = 0;
    char orig_args[8][128];
    char mang_args[8][128];
    int nargs = 0;

    member[0] = 0;
    if (!(i == 0 || !cc_is_ident_char(src[i - 1]))) return 0;
    if (i >= n || !cc_is_ident_start(src[i])) return 0;
    id_e = i;
    while (id_e < n && cc_is_ident_char(src[id_e])) id_e++;
    if (!cc__ident_generic_bracket(src, n, id_e, &gen_lb)) return 0;
    if (id_e - i == 0 || id_e - i >= sizeof(gname)) return 0;
    memcpy(gname, src + i, id_e - i);
    gname[id_e - i] = 0;
    if (!cc_emit_plan_has_generic_factory(gname)) {
        /* Free-name member call: `<snake(Family)>_<member>::[targs](args)`
         * lowers to `<Family>_<mangled targs>_<member>(args)` — the same
         * grid as `vec_new::[T]`.  Member position (`recv.m::[T]`) belongs
         * to the type-formal member tier, not this one. */
        char family[128];
        size_t member_off = 0;
        size_t b = cc_rskip_ws_and_comments(src, i);
        if (b > 0 && (src[b - 1] == '.' ||
                      (b > 1 && src[b - 1] == '>' && src[b - 2] == '-')))
            return 0;
        if (!cc_emit_plan_generic_factory_for_snake_call(gname, family, sizeof(family),
                                                         &member_off))
            return 0;
        snprintf(member, sizeof(member), "%s", gname + member_off);
        snprintf(gname, sizeof(gname), "%s", family);
    }

    br_open = gen_lb; /* '[' */
    if (!cc__find_matching_bracket(src, n, br_open, &br_close)) return 0;
    params = src + br_open + 1;
    params_len = br_close - br_open - 1;

    nargs = cc__split_type_args(params, params_len, orig_args, mang_args, 8);
    if (nargs <= 0) return 0;

    /* mangled name: Name_marg1_marg2... */
    char mangled[256];
    if (cc__emit_generic_instance(gname, orig_args, mang_args, nargs, src, n,
                                  i, br_close, input_path,
                                  mangled, sizeof(mangled)) != 0)
        return -1;

    /* Free-name member call: the member must exist in the instance's
     * emitted definition — miss articulately, listing what does. */
    if (member[0] && !cc_emit_plan_generic_instance_has_member(mangled, member)) {
        char rel[1024];
        int use_line = 1;
        for (size_t k = 0; k < i && k < n; k++)
            if (src[k] == '\n') use_line++;
        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                        use_line, 1, "type",
                        "generic family '%s' has no member '%s' (no '%s_%s' in its "
                        "emitted definition)",
                        gname, member, mangled, member);
        {
            const char* csv = cc_emit_plan_generic_instance_members_csv(mangled);
            if (csv && csv[0])
                fprintf(stderr, "  note: members of %s: %s\n", mangled, csv);
            else
                fprintf(stderr, "  note: %s emits no '%s_<member>' functions\n",
                        mangled, mangled);
        }
        return -1;
    }

    cc_sb_append(out, out_len, out_cap, src + *io_last_emit, i - *io_last_emit);
    cc_sb_append(out, out_len, out_cap, mangled, strlen(mangled));
    if (member[0]) {
        cc_sb_append(out, out_len, out_cap, "_", 1);
        cc_sb_append(out, out_len, out_cap, member, strlen(member));
    }
    *io_last_emit = br_close + 1;
    *io_i = br_close + 1;
    return 1;
}

static int cc__source_has_generic_container_syntax(const char* src, size_t n) {
    if (!src || n < 4) return 0;
    /* Match only syntax the rewriter actually lowers — not mangled names like
     * CCVec_int / Map_int_int that appear after instantiation. */
    if (memmem(src, n, "::[", 3)) return 1;
    if (memmem(src, n, "Map<", 4)) return 1;
    /* Retired spellings (Vec<T>, cc_vec_new<T>, …) — scanned only to emit the
     * migration error, not because they produce a monomorph. */
    if (memmem(src, n, "Vec<", 4)) return 1;
    if (memmem(src, n, "CCVec<", 6)) return 1;
    if (memmem(src, n, "vec_new<", 8)) return 1;
    if (memmem(src, n, "cc_vec_new<", 11)) return 1;
    return 0;
}

/* Normalize a member-position generic call to its free-name spelling.
 *
 *     py.expose::[Counter]("counter", &seed)
 *  -> py_expose::[Counter](&py, "counter", &seed)
 *
 * `recv.member::[T](args)` where recv has type `Foo` (or `CCFoo`) names the
 * factory `<snake(Foo)>_<member>` — the same grid the free-name spelling uses.
 * The receiver becomes the instance's ordinary first parameter, which is the
 * same reading of "first parameter is the receiver" that UFCS dispatch already
 * runs on, and the reading `py_expose::[T]` itself applies to decide what a
 * method is.
 *
 * This runs BEFORE instantiation rather than in the UFCS tier that knows
 * receiver types, for one reason: a factory reflects on the source it is
 * handed.  By UFCS time `!>(CCError)` has been lowered and a fallible method
 * no longer looks fallible, so instantiating there would hand the factory a
 * different program than the free-name spelling does, and the two spellings
 * would silently generate different code.  Rewriting to text first means there
 * is one instantiation path, not two that are meant to agree.
 *
 * Returns a new buffer, or NULL when nothing matched. */
static char* cc__rewrite_member_generic_to_free_name(const char* src, size_t n) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0, last_emit = 0, i = 0;
    CCUfcsVarInfo* vars = NULL;
    CCUfcsFieldInfo* fields = NULL;
    size_t var_count = 0, field_count = 0;
    int built = 0, any = 0;
    CCScannerState scan;

    if (!src || n == 0 || !memmem(src, n, "::[", 3)) return NULL;
    cc_scanner_init(&scan);

    while (i < n) {
        size_t sep_pos, m_s, m_e, recv_start, paren_pos, paren_end, targ_a, targ_b;
        char method_name[128], recv_expr[256];
        char rtype[192], base[192], snake[192], cand[352];
        int recv_is_ptr = 0;
        size_t bl;

        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (src[i] != '.' && !(src[i] == '-' && i + 1 < n && src[i + 1] == '>')) { i++; continue; }
        if (!cc__scan_generic_ufcs_call_site(src, n, i, &sep_pos, &m_s, &m_e,
                                             &recv_start, &paren_pos, &paren_end,
                                             method_name, sizeof(method_name),
                                             recv_expr, sizeof(recv_expr),
                                             &targ_a, &targ_b)) {
            i++;
            continue;
        }
        /* Only `::[...]` sites; a plain member call is not this tier's. */
        if (targ_b <= targ_a) { i = paren_end + 1; continue; }

        if (!built) {
            vars = (CCUfcsVarInfo*)calloc(512, sizeof(*vars));
            fields = (CCUfcsFieldInfo*)calloc(512, sizeof(*fields));
            if (!vars || !fields) { free(vars); free(fields); free(out); return NULL; }
            cc__collect_generic_ufcs_types(src, n, vars, &var_count, 512,
                                           fields, &field_count, 512);
            built = 1;
        }
        if (!cc__resolve_generic_ufcs_receiver_type(recv_expr, src, recv_start,
                                                    vars, var_count, fields,
                                                    field_count, rtype,
                                                    sizeof(rtype), &recv_is_ptr)) {
            i = paren_end + 1;
            continue;
        }
        snprintf(base, sizeof(base), "%s", rtype);
        bl = strlen(base);
        while (bl > 0 && (base[bl - 1] == '*' || base[bl - 1] == ' ')) base[--bl] = 0;
        /* Families are spelled without the `CC` prefix (Vec, Map), so a
         * `CC`-prefixed receiver names the family its instance layer serves. */
        if (bl > 2 && base[0] == 'C' && base[1] == 'C' &&
            base[2] >= 'A' && base[2] <= 'Z')
            memmove(base, base + 2, bl - 1);
        cc_emit_plan_snake_name(base, snake, sizeof(snake));
        snprintf(cand, sizeof(cand), "%s_%s", snake, method_name);
        if (!snake[0] || !cc_emit_plan_has_generic_factory(cand)) {
            /* Leave it for the type-formal tier, which owns both the built-in
             * type-formal members and the diagnostic when it is neither. */
            i = paren_end + 1;
            continue;
        }

        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, recv_start - last_emit);
        cc_sb_append_cstr(&out, &out_len, &out_cap, cand);
        cc_sb_append_cstr(&out, &out_len, &out_cap, "::[");
        cc_sb_append(&out, &out_len, &out_cap, src + targ_a, targ_b - targ_a);
        cc_sb_append_cstr(&out, &out_len, &out_cap, "](");
        if (!recv_is_ptr) cc_sb_append_cstr(&out, &out_len, &out_cap, "&");
        cc_sb_append(&out, &out_len, &out_cap, src + recv_start, sep_pos - recv_start);
        {
            size_t as = cc_skip_ws_and_comments(src, paren_end, paren_pos + 1);
            if (as < paren_end) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                cc_sb_append(&out, &out_len, &out_cap, src + as, paren_end - as);
            }
        }
        cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
        last_emit = paren_end + 1;
        i = paren_end + 1;
        any = 1;
    }

    free(vars);
    free(fields);
    if (!any) { free(out); return NULL; }
    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

char* cc_rewrite_generic_containers(const char* src, size_t n, const char* input_path) {
    char* owned = NULL;
    if (!src || n == 0) return NULL;
    /* Before the syntax gate: the first call per TU defines "pristine" even
     * when the only generic surface appears later in the fixpoint. */
    cc__reflect_snapshot_capture(src, n, input_path);
    if (!cc__source_has_generic_container_syntax(src, n)) return NULL;

    /* Member spelling first, so instantiation below sees one form. */
    owned = cc__rewrite_member_generic_to_free_name(src, n);
    if (owned) { src = owned; n = strlen(owned); }

    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    size_t last_emit = 0;
    CCScannerState scanner;
    cc_scanner_init(&scanner);
    
    CCTypeGraph* graph = cc_type_graph_get_global();
    CCTypeRegistry* reg = graph ? cc_type_graph_registry(graph) : NULL;
    
    while (i < n) {
        /* Skip comments and strings using shared helper */
        if (cc_scanner_skip_non_code(&scanner, src, n, &i)) continue;

        /* D6.0: library-registered generic factory `Name::[args]` (checked
         * before the built-in CCVec/Map keywords; those names are never
         * registered as templates, so they fall through unaffected). */
        {
            int gr = cc__try_rewrite_user_generic(src, n, input_path, &out, &out_len, &out_cap,
                                                  &i, &last_emit);
            if (gr < 0) {
                free(out);
                free(owned);
                return (char*)-1;
            }
            if (gr) continue;
        }

        /* Look for canonical CCVec::[ / cc_vec_new::[ and Map forms. */
        int is_vec_type = 0, is_map_type = 0, is_vec_new = 0, is_map_new = 0;
        int use_bracket = 0;
        size_t blb = 0;
        const char* retired_vec_syntax = NULL;
        size_t kw_start = i;
        size_t kw_len = 0;
        
        if (i == 0 || !cc_is_ident_char(src[i - 1])) {
            if (cc__builtin_generic_at(src, n, i, "CCVec", &blb)) {
                is_vec_type = 1; kw_len = 5; use_bracket = 1;
            } else if (cc__builtin_generic_at(src, n, i, "Vec", &blb)) {
                /* Surface name; same monomorph as CCVec::[T] (CCVec_T). */
                is_vec_type = 1; kw_len = 3; use_bracket = 1;
            } else if (i + 6 <= n && memcmp(src + i, "CCVec<", 6) == 0) {
                retired_vec_syntax = "CCVec<T>";
            } else if (i + 4 <= n && memcmp(src + i, "Vec<", 4) == 0) {
                retired_vec_syntax = "Vec<T>";
            } else if (cc__builtin_generic_at(src, n, i, "ArrayMap", &blb)) {
                is_map_type = 2; kw_len = 8; use_bracket = 1; /* 2 ⇒ ArrayMap */
            } else if (cc__builtin_generic_at(src, n, i, "Map", &blb)) {
                is_map_type = 1; kw_len = 3; use_bracket = 1;
            } else if (i + 4 <= n && memcmp(src + i, "Map<", 4) == 0) {
                retired_vec_syntax = "Map<K, V>";
            } else if (cc__builtin_generic_at(src, n, i, "cc_vec_new", &blb)) {
                is_vec_new = 1; kw_len = 10; use_bracket = 1;
            } else if (cc__builtin_generic_at(src, n, i, "vec_new", &blb)) {
                is_vec_new = 1; kw_len = 7; use_bracket = 1;
            } else if (i + 11 <= n && memcmp(src + i, "cc_vec_new<", 11) == 0) {
                retired_vec_syntax = "cc_vec_new<T>";
            } else if (i + 8 <= n && memcmp(src + i, "vec_new<", 8) == 0) {
                retired_vec_syntax = "vec_new<T>";
            } else if (cc__builtin_generic_at(src, n, i, "array_map_new_count", &blb)) {
                /* Must precede array_map_new::[ — that spelling is a prefix. */
                is_map_new = 3; kw_len = 19; use_bracket = 1;
            } else if (cc__builtin_generic_at(src, n, i, "array_map_new", &blb)) {
                is_map_new = 2; kw_len = 13; use_bracket = 1;
            } else if (cc__builtin_generic_at(src, n, i, "map_new", &blb)) {
                is_map_new = 1; kw_len = 7; use_bracket = 1;
            } else if (i + 8 <= n && memcmp(src + i, "map_new<", 8) == 0) {
                retired_vec_syntax = "map_new<K, V>";
            }
        }

        if (retired_vec_syntax) {
            char rel[1024];
            cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                    scanner.line, scanner.col, "type",
                    "retired generic spelling '%s'; use the '::[ ... ]' bracket form "
                    "(e.g. 'Vec::[int]', 'Map::[K, V]', 'vec_new::[T](...)', 'map_new::[K, V](...)')",
                    retired_vec_syntax);
            free(out);
            free(owned);
            return NULL;
        }
        
        if (is_vec_type || is_map_type || is_vec_new || is_map_new) {
            size_t delim_start = use_bracket ? blb : kw_start + kw_len;
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
                free(owned);
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
                /* Single type parameter: capture code-only text, since it
                 * feeds both the macro operand and the mangled name. */
                cc__copy_type_arg_text(params, 0, params_len,
                                       orig_elem_type, sizeof(orig_elem_type));
                cc__canonicalize_container_param_type(orig_elem_type, sizeof(orig_elem_type));
                cc__normalize_slice_in_type(orig_elem_type, sizeof(orig_elem_type));
                
                cc__mangle_container_type_param(orig_elem_type, strlen(orig_elem_type), elem_type, sizeof(elem_type));
                snprintf(mangled, sizeof(mangled), "CCVec_%s", elem_type);
                cc__ctype_memo_put(mangled, orig_elem_type, NULL);
                
                if (graph) {
                    cc_type_graph_request_vec(graph, orig_elem_type, mangled);
                }
            } else {
                /* Two type parameters: K, V */
                const char* comma = NULL;
                int depth = 0;
                for (size_t k = 0; k < params_len; ) {
                    char pc;
                    size_t sk = cc_skip_ws_and_comments(params, params_len, k);
                    if (sk > k) { k = sk; continue; }
                    pc = params[k];
                    if (pc == '<' || pc == '[') depth++;
                    else if (pc == '>' || pc == ']') depth--;
                    else if (pc == ',' && depth == 0) { comma = params + k; break; }
                    k++;
                }
                
                if (!comma) {
                    i++;
                    continue;
                }
                
                size_t k_len = (size_t)(comma - params);
                size_t v_start = (size_t)(comma - params) + 1;
                size_t v_len = params_len - v_start;
                
                /* Save original key/val types as code-only text. */
                (void)v_len;
                cc__copy_type_arg_text(params, 0, k_len,
                                       orig_key_type, sizeof(orig_key_type));
                cc__canonicalize_container_param_type(orig_key_type, sizeof(orig_key_type));

                cc__copy_type_arg_text(params, v_start, params_len,
                                       orig_val_type, sizeof(orig_val_type));
                cc__canonicalize_container_param_type(orig_val_type, sizeof(orig_val_type));
                
                cc__normalize_slice_in_type(orig_key_type, sizeof(orig_key_type));
                cc__normalize_slice_in_type(orig_val_type, sizeof(orig_val_type));
                
                cc__mangle_container_type_param(orig_key_type, strlen(orig_key_type), key_type, sizeof(key_type));
                cc__mangle_container_type_param(orig_val_type, strlen(orig_val_type), val_type, sizeof(val_type));
                if (is_map_type == 2 || is_map_new == 2 || is_map_new == 3) {
                    snprintf(mangled, sizeof(mangled), "ArrayMap_%s_%s", key_type, val_type);
                } else {
                    snprintf(mangled, sizeof(mangled), "Map_%s_%s", key_type, val_type);
                }
                cc__ctype_memo_put(mangled, orig_key_type, orig_val_type);
                
                if (graph) {
                    cc_type_graph_request_map(graph, orig_key_type, orig_val_type, mangled);
                }
            }
            
            /* Emit everything up to this point */
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, kw_start - last_emit);
            
            if (is_vec_type || is_map_type) {
                /* Emit parser-safe macro call instead of bare type name.
                   Vec now resolves to concrete registry-backed CCVec_T in both
                   modes; Map remains placeholder-backed in parser mode. */
                if (is_vec_type) {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_VEC(");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, elem_type);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                } else if (is_map_type == 2) {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_ARRAY_MAP(");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, key_type);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, val_type);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ")*");
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
                        } else if (is_map_new == 3) {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_ARRAY_MAP_INIT_COUNT(");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, key_type);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, val_type);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                            cc_sb_append(&out, &out_len, &out_cap, src + j + 1, paren_end - j - 1);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                        } else if (is_map_new == 2) {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_ARRAY_MAP_INIT(");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, key_type);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                            cc_sb_append_cstr(&out, &out_len, &out_cap, val_type);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
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
                } else if (is_map_new == 3) {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_ARRAY_MAP_INIT_COUNT(");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, key_type);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, val_type);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ", NULL, 0)");
                } else if (is_map_new == 2) {
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "__CC_ARRAY_MAP_INIT(");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, key_type);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, val_type);
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

        /* Python extraction members carry their element types as leading
         * macro arguments: `obj.as_list::[T](&a)` becomes
         * `cc_py_obj_as_list(T, &obj, &a)`.  Lowered here rather than in
         * the family engine because CCPyObj methods resolve on the AST
         * dynamic sink, which never sees a `::[` member. */
        if (src[i] == '.' && i > 0) {
            static const char* const pynames[] = { "as_list", "as_map" };
            int pk;
            for (pk = 0; pk < 2; pk++) {
                size_t mlen = strlen(pynames[pk]);
                size_t ms = cc_skip_ws_and_comments(src, n, i + 1);
                size_t br, rb = 0, lp, rp = 0, rs;
                if (ms + mlen > n || memcmp(src + ms, pynames[pk], mlen) != 0 ||
                    (ms + mlen < n && cc_is_ident_char(src[ms + mlen])))
                    continue;
                br = cc_skip_ws_and_comments(src, n, ms + mlen);
                if (br + 2 >= n || src[br] != ':' || src[br + 1] != ':' ||
                    src[br + 2] != '[')
                    continue;
                if (!cc__find_matching_bracket(src, n, br + 2, &rb)) continue;
                lp = cc_skip_ws_and_comments(src, n, rb + 1);
                if (lp >= n || src[lp] != '(' ||
                    !cc_find_matching_paren(src, n, lp, &rp))
                    continue;
                /* Receiver: a plain identifier immediately left of the `.`. */
                rs = cc_rskip_ws_and_comments(src, i);
                {
                    size_t re = rs;
                    while (rs > 0 && cc_is_ident_char(src[rs - 1])) rs--;
                    if (rs == re || !cc_is_ident_start(src[rs])) break;
                    if (rs > 0 && (src[rs - 1] == '.' || src[rs - 1] == '>'))
                        break;
                    cc_sb_append(&out, &out_len, &out_cap, src + last_emit,
                                 rs - last_emit);
                    cc_sb_append_cstr(&out, &out_len, &out_cap,
                                      pk == 0 ? "cc__py_obj_as_list_raw(&"
                                              : "cc__py_obj_as_map_raw(&");
                    cc_sb_append(&out, &out_len, &out_cap, src + rs, re - rs);
                    {
                        size_t as = cc_skip_ws_and_comments(src, rp, lp + 1);
                        if (as < rp) {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                            cc_sb_append(&out, &out_len, &out_cap, src + as,
                                         rp - as);
                        }
                    }
                    /* Element kinds resolve here, so the emitted call carries
                     * no type token — a type argument inside a macro call
                     * would parse as a declarator in parser mode. */
                    {
                        char ta[192];
                        const char* s0 = src + br + 3;
                        const char* s1 = src + rb;
                        size_t tn;
                        cc__trim_span_ws(&s0, &s1);
                        tn = (size_t)(s1 - s0);
                        if (tn >= sizeof(ta)) tn = sizeof(ta) - 1;
                        memcpy(ta, s0, tn);
                        ta[tn] = 0;
                        {
                            char* comma = strchr(ta, ',');
                            const char* k1 = ta;
                            const char* k2 = NULL;
                            if (comma) {
                                *comma = 0;
                                k2 = comma + 1;
                                while (*k2 == ' ' || *k2 == '\t') k2++;
                            }
                            {
                                size_t kl = strlen(k1);
                                while (kl > 0 && (k1[kl - 1] == ' ' || k1[kl - 1] == '\t'))
                                    ((char*)k1)[--kl] = 0;
                            }
                            cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                            cc_sb_append_cstr(&out, &out_len, &out_cap,
                                              cc__py_elem_kind_token(k1));
                            if (k2) {
                                cc_sb_append_cstr(&out, &out_len, &out_cap, ", ");
                                cc_sb_append_cstr(&out, &out_len, &out_cap,
                                                  cc__py_elem_kind_token(k2));
                            }
                        }
                    }
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
                    last_emit = rp + 1;
                    i = rp + 1;
                    goto py_extract_done;
                }
            }
        }
        /* Articulate `::[` misses: a free name followed by `::[` that no
         * recognizer consumed is an error here — never a host-parser
         * surprise.  Member position (`recv.m::[T]`) belongs to the
         * type-formal member tier and is skipped. */
        if (cc_is_ident_start(src[i]) && (i == 0 || !cc_is_ident_char(src[i - 1]))) {
            size_t id_end = i;
            while (id_end < n && cc_is_ident_char(src[id_end])) id_end++;
            if (cc__ident_generic_bracket(src, n, id_end, NULL)) {
                size_t b = cc_rskip_ws_and_comments(src, i);
                int member_pos = (b > 0 && (src[b - 1] == '.' ||
                                            (b > 1 && src[b - 1] == '>' &&
                                             src[b - 2] == '-')));
                if (!member_pos) {
                    char nm[128];
                    char rel[1024];
                    size_t nl = id_end - i;
                    int use_line = 1;
                    if (nl >= sizeof(nm)) nl = sizeof(nm) - 1;
                    memcpy(nm, src + i, nl);
                    nm[nl] = 0;
                    for (size_t k = 0; k < i && k < n; k++)
                        if (src[k] == '\n') use_line++;
                    if (cc_emit_plan_has_generic_factory(nm)) {
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>",
                                                            rel, sizeof(rel)),
                                        use_line, 1, "type",
                                        "malformed type-argument list after '%s::[' "
                                        "(expected '%s::[Type, ...]')",
                                        nm, nm);
                    } else {
                        char fams[512];
                        cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>",
                                                            rel, sizeof(rel)),
                                        use_line, 1, "type",
                                        "unknown generic name '%s' before '::[...]' — "
                                        "'::[' specializes the name it follows, but '%s' is "
                                        "neither a built-in generic form (Vec, Map, ArrayMap, "
                                        "vec_new, map_new, ...) nor '<family>_<member>' of a "
                                        "registered generic factory family",
                                        nm, nm);
                        if (cc_emit_plan_generic_factory_names_csv(fams, sizeof(fams)) > 0)
                            fprintf(stderr,
                                    "  note: registered generic factory families: %s\n",
                                    fams);
                        else
                            fprintf(stderr,
                                    "  note: no generic factory families are registered in "
                                    "this translation unit (define one with "
                                    "CC_GENERIC_FACTORY(Name, arity) { ... })\n");
                    }
                    free(out);
                    return (char*)-1;
                }
                i = id_end;
                continue;
            }
            i = id_end;
            continue;
        }

        i++; scanner.col++;
        py_extract_done: ;
    }

    /* `owned` is the member-normalized source the scan above ran on.  When
     * the scan changed nothing further, that normalization IS the result —
     * returning NULL here would report "no change" and silently drop it. */
    if (!out) return owned;
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    free(owned);
    return out;
}

/* ---- Container type memo ----
 *
 * `Map<K,V>` / `CCVec::[T]` lower to the parser-safe macros `__CC_MAP(K_m, V_m)`
 * / `__CC_VEC(T_m)`, where the parameter is the *mangled* token (`int*` becomes
 * `intptr`).  The phase-3 reparse re-derives container instantiations by
 * scanning those lowered macros (cc__register_lowered_{vec,map}_macros), but the
 * mangled token is not a valid C type — and the real container bodies
 * (map_impl.cch / vec.cch) now use the parameter as a real type (`V val`,
 * `T *data`).  The old parser stubs hid this by never using the parameter as a
 * type.  Mangling is lossy (`*`->`ptr`, `[:]`->`slice`, separators->`_`), so we
 * cannot demangle reliably.  Instead, capture the real spelling at the lowering
 * site (where `int*` is still present) keyed by mangled name, and recover it
 * during the reparse rescan.  The memo is process-global and deterministic
 * (mangled names are a pure function of the parameter types). */
typedef struct {
    char mangled[256];
    char type1[128];
    char type2[128];
} CCContainerTypeMemo;
static CCContainerTypeMemo cc__ctype_memo[1024];
static size_t cc__ctype_memo_count = 0;

static void cc__ctype_memo_put(const char* mangled, const char* type1, const char* type2) {
    if (!mangled || !mangled[0]) return;
    for (size_t i = 0; i < cc__ctype_memo_count; i++) {
        if (strcmp(cc__ctype_memo[i].mangled, mangled) == 0) return; /* keep first */
    }
    if (cc__ctype_memo_count >= sizeof(cc__ctype_memo) / sizeof(cc__ctype_memo[0])) return;
    CCContainerTypeMemo* m = &cc__ctype_memo[cc__ctype_memo_count++];
    snprintf(m->mangled, sizeof(m->mangled), "%s", mangled);
    snprintf(m->type1, sizeof(m->type1), "%s", type1 ? type1 : "");
    snprintf(m->type2, sizeof(m->type2), "%s", type2 ? type2 : "");
}

static const CCContainerTypeMemo* cc__ctype_memo_get(const char* mangled) {
    if (!mangled) return NULL;
    for (size_t i = 0; i < cc__ctype_memo_count; i++) {
        if (strcmp(cc__ctype_memo[i].mangled, mangled) == 0) return &cc__ctype_memo[i];
    }
    return NULL;
}

static const char* cc__ctype_memo_get_type1(const char* mangled) {
    const CCContainerTypeMemo* m = cc__ctype_memo_get(mangled);
    return (m && m->type1[0]) ? m->type1 : NULL;
}

static const char* cc__ctype_memo_get_type2(const char* mangled) {
    const CCContainerTypeMemo* m = cc__ctype_memo_get(mangled);
    return (m && m->type2[0]) ? m->type2 : NULL;
}

static void cc__register_lowered_vec_macros(const char* src) {
    if (!src) return;
    CCTypeGraph* graph = cc_type_graph_get_global();
    if (!graph) return;

    const char* p = src;
    while ((p = strstr(p, "__CC_VEC(")) != NULL) {
        const char* arg = p + 9;
        const char* end = strchr(arg, ')');
        if (!end) break;

        const char* s = arg;
        const char* e = end;
        cc__trim_span_ws(&s, &e);
        if (e > s) {
            char elem_type[128];
            char mangled[256];
            const char* real_elem;
            size_t len = (size_t)(e - s);
            if (len >= sizeof(elem_type)) len = sizeof(elem_type) - 1;
            memcpy(elem_type, s, len);
            elem_type[len] = '\0';
            snprintf(mangled, sizeof(mangled), "CCVec_%s", elem_type);
            /* Prefer the real type spelling captured at the lowering site; the
             * scanned `elem_type` is the mangled token (e.g. `intptr`). */
            real_elem = cc__ctype_memo_get_type1(mangled);
            cc_type_graph_request_vec(graph, real_elem ? real_elem : elem_type, mangled);
        }

        p = end + 1;
    }
}

static void cc__register_lowered_kv_macros(const char* src, const char* macro,
                                           const char* mangled_prefix) {
    if (!src || !macro || !mangled_prefix) return;
    CCTypeGraph* graph = cc_type_graph_get_global();
    size_t macro_len;
    const char* p;
    if (!graph) return;
    macro_len = strlen(macro);
    p = src;
    while ((p = strstr(p, macro)) != NULL) {
        const char* arg = p + macro_len;
        const char* end = strchr(arg, ')');
        const char* s;
        const char* e;
        const char* comma = NULL;
        int depth = 0;
        if (!end) break;
        s = arg;
        e = end;
        for (const char* q = s; q < e; q++) {
            char c = *q;
            if (c == '(' || c == '[' || c == '<' || c == '{') depth++;
            else if (c == ')' || c == ']' || c == '>' || c == '}') depth--;
            else if (c == ',' && depth == 0) {
                comma = q;
                break;
            }
        }
        if (comma) {
            char key_type[128];
            char val_type[128];
            char mangled[256];
            const char* ks = s;
            const char* ke = comma;
            const char* vs = comma + 1;
            const char* ve = e;
            size_t k_len;
            size_t v_len;
            cc__trim_span_ws(&ks, &ke);
            cc__trim_span_ws(&vs, &ve);
            k_len = (size_t)(ke - ks);
            v_len = (size_t)(ve - vs);
            if (k_len >= sizeof(key_type)) k_len = sizeof(key_type) - 1;
            if (v_len >= sizeof(val_type)) v_len = sizeof(val_type) - 1;
            memcpy(key_type, ks, k_len);
            key_type[k_len] = '\0';
            memcpy(val_type, vs, v_len);
            val_type[v_len] = '\0';
            snprintf(mangled, sizeof(mangled), "%s%s_%s", mangled_prefix, key_type, val_type);
            {
                const char* real_key = cc__ctype_memo_get_type1(mangled);
                const char* real_val = cc__ctype_memo_get_type2(mangled);
                cc_type_graph_request_map(graph,
                                          real_key ? real_key : key_type,
                                          real_val ? real_val : val_type,
                                          mangled);
            }
        }
        p = end + 1;
    }
}

static void cc__register_lowered_map_macros(const char* src) {
    cc__register_lowered_kv_macros(src, "__CC_MAP(", "Map_");
    cc__register_lowered_kv_macros(src, "__CC_ARRAY_MAP(", "ArrayMap_");
}

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
            j = cc_skip_ws_and_comments(src, n, j);
            
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
                size_t bk = cc_rskip_ws_and_comments(src, sigil_pos);
                if (bk > 0 && src[bk - 1] == ')') {
                    i = sigil_pos + 2;
                    scan.col += 2;
                    continue;
                }
            }
            if (j < n && src[j] == '(') {
                size_t lparen = j;
                j++;  /* skip '(' */

                /* Skip whitespace and comments inside parens */
                j = cc_skip_ws_and_comments(src, n, j);

                /* Find matching ')' - track nesting for complex types like Error<A, B> */
                size_t err_start = j;
                size_t rparen = 0;
                if (!cc_find_matching_paren(src, n, lparen, &rparen)) {
                    /* ERROR: unclosed paren in error type */
                    char rel[1024];
                    cc_pp_error_cat(cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                            scan.line, scan.col, "type", "unclosed '(' in result error type");
                    fprintf(stderr, "  hint: result type syntax is 'T !> (ErrorType)'\n");
                    free(out);
                    return NULL;
                }
                j = rparen;

                {
                    /* Found matching ')' at position j */
                    size_t err_end = j;
                    
                    /* Trim the trailing edge to code: a comment before the `)`
                     * would otherwise mangle into the Result type name. */
                    err_end = cc_rskip_ws_and_comments(src, err_end);
                    if (err_end < err_start) err_end = err_start;
                    
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
                    
                    /* Scan back from '!>' to find the ok type start.
                     * Skip whitespace before '!>', including newlines and cpp
                     * '# line' directive lines: when the ok type is a macro
                     * (e.g. `bool` -> `_Bool` via <stdbool.h> pulled in by the
                     * prelude), cpp separates the expanded type from the sigil
                     * with line markers, which a space/tab-only skip mistook
                     * for a missing type. */
                    size_t ty_end = sigil_pos;
                    for (;;) {
                        /* Comment-aware: `int /(*)c(*)!>(E)` must yield the ok
                         * type `int`, not a span whose comment bytes mangle
                         * into the Result name. */
                        ty_end = cc_rskip_ws_and_comments(src, ty_end);
                        if (ty_end == 0) break;
                        /* If the line ending at ty_end is a '# ...' directive, drop it and retry. */
                        size_t line_start = ty_end;
                        while (line_start > 0 && src[line_start - 1] != '\n') line_start--;
                        size_t p = line_start;
                        while (p < ty_end && (src[p] == ' ' || src[p] == '\t')) p++;
                        if (p < ty_end && src[p] == '#') { ty_end = line_start; continue; }
                        break;
                    }

                    size_t ty_start = cc__scan_back_to_delim(src, ty_end);
                    unsigned ok_quals = 0;
                    ty_start = cc__skip_leading_decl_specs_ex(src, ty_start, &ok_quals);
                    
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
                            /* The qualifier is gone by the time a spec reaches
                             * `cc_result_spec_table_add` — both declarations
                             * arrive spelled the same and look identical — so
                             * the conflict can only be seen here, where the
                             * span was captured. */
                            char concrete[288];
                            unsigned first_q = 0;
                            int first_line = 0;
                            snprintf(concrete, sizeof(concrete), "CCResult_%s_%s",
                                     mangled_ok, mangled_err);
                            /* A warning, not an error: sharing one box is the
                             * intended rule (C applies the qualifier at the
                             * binding site), and `result_type_canonical_smoke`
                             * pins it.  What was wrong is that it happened in
                             * silence, so whichever declaration came first
                             * decided const-ness for every other. */
                            if (cc__result_quals_note(concrete, ok_quals, scan.line,
                                                      &first_q, &first_line)) {
                                char a[64], b[64], rel[1024];
                                cc__qual_names(first_q, a, sizeof(a));
                                cc__qual_names(ok_quals, b, sizeof(b));
                                fprintf(stderr,
                                        "%s:%d:%d: warning: type: '%s' is declared with a %s "
                                        "ok type here and a %s one at line %d; qualifiers are "
                                        "not part of a box's identity, so both share one box "
                                        "and the first spelling wins\n",
                                        cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel)),
                                        scan.line, scan.col, concrete, b, a, first_line);
                            }
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
                                            /* Record the fn name, declared textual error
                                             * type, and concrete CCResult_T_E so unwrap can
                                             * emit typed binders / expression-position `!>;`
                                             * even when the callee lives in an unexpanded
                                             * header.  err_type is unmangled source text
                                             * (e.g. "CCIoError"). */
                                            cc_result_fn_registry_add_typed(var_name, vn_len,
                                                                             src + err_start,
                                                                             err_end - err_start,
                                                                             type_name,
                                                                             strlen(type_name));
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
       __CC_RESULT(T, E) preserves the concrete CCResult_T_E name in parser and
       real modes; later declaration emission supplies the typed struct. */
    (void)cc__result_specs.count;
    (void)input_path;
    return out;
}

static int cc__slice_tok_is(const char* s, size_t a, size_t b, const char* kw) {
    size_t kn = strlen(kw);
    return (b - a) == kn && memcmp(s + a, kw, kn) == 0;
}

/* Classify `[:...]` at `lb` as declarator-position (`T name[:]`) vs
 * type-position (`T[:] name`). The span [type_start, lb) must be a pure
 * decl prefix (identifiers and `*` only) with at least two tokens whose
 * last token is a plain identifier — that identifier is the variable
 * name and everything before it is the element type. Multi-word builtin
 * types (`unsigned char[:]`) and tags (`struct Foo[:]`) stay
 * type-position: their last token is a type keyword / tag name. */
static int cc__slice_declarator_name(const char* s, size_t type_start, size_t lb,
                                     size_t* out_ns, size_t* out_ne) {
    size_t p = type_start;
    size_t first_s = 0, first_e = 0;
    size_t prev_s = 0, prev_e = 0;
    size_t last_s = 0, last_e = 0;
    int ntok = 0;
    static const char* builtin_tail[] = {
        "char", "short", "int", "long", "float", "double",
        "signed", "unsigned", "bool", "void", "_Bool", NULL
    };
    while (p < lb) {
        p = cc_skip_ws_and_comments(s, lb, p);
        if (p >= lb) break;
        if (s[p] == '*') {
            prev_s = last_s; prev_e = last_e;
            last_s = p; last_e = p + 1;
            if (ntok == 0) { first_s = p; first_e = p + 1; }
            ntok++;
            p++;
            continue;
        }
        if (cc_is_ident_start(s[p])) {
            size_t e = p;
            while (e < lb && cc_is_ident_char(s[e])) e++;
            prev_s = last_s; prev_e = last_e;
            last_s = p; last_e = e;
            if (ntok == 0) { first_s = p; first_e = e; }
            ntok++;
            p = e;
            continue;
        }
        return 0; /* expression or complex declarator — not our shape */
    }
    if (ntok < 2) return 0;
    if (s[last_s] == '*') return 0;
    for (int k = 0; builtin_tail[k]; k++)
        if (cc__slice_tok_is(s, last_s, last_e, builtin_tail[k])) return 0;
    if (prev_e > prev_s && s[prev_s] != '*' &&
        (cc__slice_tok_is(s, prev_s, prev_e, "struct") ||
         cc__slice_tok_is(s, prev_s, prev_e, "union") ||
         cc__slice_tok_is(s, prev_s, prev_e, "enum")))
        return 0;
    /* `return x[a:b]`-style statements are not declarations. */
    if (cc__slice_tok_is(s, first_s, first_e, "return") ||
        cc__slice_tok_is(s, first_s, first_e, "sizeof") ||
        cc__slice_tok_is(s, first_s, first_e, "case") ||
        cc__slice_tok_is(s, first_s, first_e, "goto"))
        return 0;
    *out_ns = last_s;
    *out_ne = last_e;
    return 1;
}

/* Instance type name for a slice element span: `double` ->
 * CCSlice_double (open generic family, CC_DECL_SLICE_SPEC). Any
 * identifier-token element type except the char family instantiates;
 * char slices stay bare CCSlice (bytes are their elements), and
 * pointer/exotic spellings stay erased. Registers the instance in the
 * session slice-spec table and its `base` @as field so the AST UFCS
 * pass routes methods through as-retry. Outputs the type name and,
 * optionally, the member prefix — the instance name itself
 * (CCSlice_double_at), the same NAME##_ convention as Vec/Map. */
static int cc__slice_instance_for_elem(const char* src, size_t elem_s, size_t elem_e,
                                       char* out, size_t out_sz,
                                       char* out_prefix, size_t prefix_sz) {
    char norm[128];
    char mangled[128];
    size_t nn = 0;
    size_t p = elem_s;
    int is_char_family = 0;
    while (elem_e > elem_s && (src[elem_e - 1] == ' ' || src[elem_e - 1] == '\t' ||
                               src[elem_e - 1] == '\n' || src[elem_e - 1] == '\r'))
        elem_e--;
    while (p < elem_e) {
        size_t tok_s;
        p = cc_skip_ws_and_comments(src, elem_e, p);
        if (p >= elem_e) break;
        if (!cc_is_ident_start(src[p])) return 0;
        if (nn > 0 && nn + 1 < sizeof(norm)) norm[nn++] = ' ';
        tok_s = nn;
        while (p < elem_e && cc_is_ident_char(src[p])) {
            if (nn + 1 >= sizeof(norm)) return 0;
            norm[nn++] = src[p++];
        }
        if (nn - tok_s == 4 && memcmp(norm + tok_s, "char", 4) == 0)
            is_char_family = 1;
    }
    norm[nn] = '\0';
    if (nn == 0 || is_char_family) return 0;
    cc__mangle_type_name(norm, nn, mangled, sizeof(mangled));
    if (!mangled[0]) return 0;
    {
        int wrote = snprintf(out, out_sz, "CCSlice_%s", mangled);
        if (wrote <= 0 || (size_t)wrote >= out_sz) return 0;
    }
    {
        size_t nl = strlen(out);
        (void)cc_slice_spec_register(out, out, norm);
        (void)cc_type_registry_add_field_ex(cc_type_registry_get_global(), out,
                                            "base", "CCSlice", 1);
        if (out_prefix) {
            if (nl + 1 > prefix_sz) return 0;
            memcpy(out_prefix, out, nl + 1);
        }
    }
    return 1;
}

/* Lower a slice declaration initializer:
 *   `= "lit"` -> `= CC_SLICE_LIT("lit")`      (char element type)
 *   `= {...}` -> hidden block-scope backing array + view:
 *                `T __cc_slb_N[] = {...}; CCSlice N =
 *                 cc_slice_from_buffer(buf, element-count)`
 *   `= {}`    -> `= cc_slice_empty()`
 * `after` points just past the declarator (after `]` in declarator
 * position, after the variable name in type position). On success emits
 * the replacement from the element type onward (caller has emitted decl
 * specs) and returns the resume offset — the terminating `;` stays in
 * the source. Returns 0 for any other initializer shape. */
static size_t cc__slice_emit_decl_init(char** out, size_t* out_len, size_t* out_cap,
                                       const char* src, size_t n,
                                       size_t elem_s, size_t elem_e,
                                       size_t name_s, size_t name_e,
                                       size_t after) {
    size_t eq = cc_skip_ws_and_comments(src, n, after);
    size_t p;
    if (eq >= n || src[eq] != '=') return 0;
    if (eq + 1 < n && src[eq + 1] == '=') return 0;
    p = cc_skip_ws_and_comments(src, n, eq + 1);
    if (p >= n) return 0;
    while (elem_e > elem_s && (src[elem_e - 1] == ' ' || src[elem_e - 1] == '\t' ||
                               src[elem_e - 1] == '\n' || src[elem_e - 1] == '\r'))
        elem_e--;
    if (src[p] == '{') {
        size_t rb, semi, body;
        char inst[96];
        char snake[160];
        int is_inst;
        const char* slice_ty;
        if (!cc_find_matching_brace(src, n, p, &rb)) return 0;
        semi = cc_skip_ws_and_comments(src, n, rb + 1);
        if (semi >= n || src[semi] != ';') return 0; /* multi-declarator etc. */
        body = cc_skip_ws_and_comments(src, rb, p + 1);
        /* `{ .ptr = ..., ... }` and the `{0}` zero-init idiom are struct
         * initializers for the slice itself, not element lists. */
        if (body < rb && src[body] == '.') return 0;
        if (body < rb && src[body] == '0') {
            size_t z = cc_skip_ws_and_comments(src, rb, body + 1);
            if (z >= rb) return 0;
        }
        is_inst = cc__slice_instance_for_elem(src, elem_s, elem_e, inst, sizeof(inst),
                                              snake, sizeof(snake));
        slice_ty = is_inst ? inst : "CCSlice";
        if (body >= rb) {
            cc_sb_append_cstr(out, out_len, out_cap, slice_ty);
            cc_sb_append_cstr(out, out_len, out_cap, " ");
            cc_sb_append(out, out_len, out_cap, src + name_s, name_e - name_s);
            if (is_inst) {
                cc_sb_append_cstr(out, out_len, out_cap, " = ");
                cc_sb_append_cstr(out, out_len, out_cap, snake);
                cc_sb_append_cstr(out, out_len, out_cap, "_from_buffer(0, 0)");
            } else {
                cc_sb_append_cstr(out, out_len, out_cap, " = cc_slice_empty()");
            }
            return rb + 1;
        }
        if (elem_e <= elem_s) return 0;
        cc_sb_append(out, out_len, out_cap, src + elem_s, elem_e - elem_s);
        cc_sb_append_cstr(out, out_len, out_cap, " __cc_slb_");
        cc_sb_append(out, out_len, out_cap, src + name_s, name_e - name_s);
        cc_sb_append_cstr(out, out_len, out_cap, "[] = ");
        cc_sb_append(out, out_len, out_cap, src + p, rb + 1 - p);
        cc_sb_append_cstr(out, out_len, out_cap, "; ");
        cc_sb_append_cstr(out, out_len, out_cap, slice_ty);
        cc_sb_append_cstr(out, out_len, out_cap, " ");
        cc_sb_append(out, out_len, out_cap, src + name_s, name_e - name_s);
        if (is_inst) {
            cc_sb_append_cstr(out, out_len, out_cap, " = ");
            cc_sb_append_cstr(out, out_len, out_cap, snake);
            cc_sb_append_cstr(out, out_len, out_cap, "_from_buffer(__cc_slb_");
        } else {
            cc_sb_append_cstr(out, out_len, out_cap, " = cc_slice_from_buffer((void*)__cc_slb_");
        }
        cc_sb_append(out, out_len, out_cap, src + name_s, name_e - name_s);
        cc_sb_append_cstr(out, out_len, out_cap, ", sizeof(__cc_slb_");
        cc_sb_append(out, out_len, out_cap, src + name_s, name_e - name_s);
        cc_sb_append_cstr(out, out_len, out_cap, ")/sizeof((__cc_slb_");
        cc_sb_append(out, out_len, out_cap, src + name_s, name_e - name_s);
        cc_sb_append_cstr(out, out_len, out_cap, ")[0]))");
        return rb + 1;
    }
    {
        /* String literal (optionally u8/L/u/U prefixed); element type must
         * be `char` (cv-qualified is fine). */
        size_t e = p;
        size_t q = elem_s;
        int nchar = 0;
        if (e + 1 < n && src[e] == 'u' && src[e + 1] == '8') e += 2;
        else if (src[e] == 'L' || src[e] == 'u' || src[e] == 'U') e += 1;
        if (e >= n || src[e] != '"') return 0;
        while (q < elem_e) {
            size_t qe;
            q = cc_skip_ws_and_comments(src, elem_e, q);
            if (q >= elem_e) break;
            if (!cc_is_ident_start(src[q])) return 0;
            qe = q;
            while (qe < elem_e && cc_is_ident_char(src[qe])) qe++;
            if (cc__slice_tok_is(src, q, qe, "char")) nchar++;
            else if (!cc__slice_tok_is(src, q, qe, "const") &&
                     !cc__slice_tok_is(src, q, qe, "volatile"))
                return 0;
            q = qe;
        }
        if (nchar != 1) return 0;
        {
            size_t end = cc__scan_to_top_level_delim(src, n, p, ';', ',');
            size_t ie;
            if (end >= n || src[end] != ';') return 0;
            ie = end;
            while (ie > p && (src[ie - 1] == ' ' || src[ie - 1] == '\t' ||
                              src[ie - 1] == '\n' || src[ie - 1] == '\r'))
                ie--;
            cc_sb_append_cstr(out, out_len, out_cap, "CCSlice ");
            cc_sb_append(out, out_len, out_cap, src + name_s, name_e - name_s);
            cc_sb_append_cstr(out, out_len, out_cap, " = CC_SLICE_LIT(");
            cc_sb_append(out, out_len, out_cap, src + p, ie - p);
            cc_sb_append_cstr(out, out_len, out_cap, ")");
            return end;
        }
    }
}

/* Rewrite slice types:
   - `T[:]`  -> `CCSlice`
   - `T[:!]` -> `CCSliceUnique`
   Both type position (`T[:] name`) and declarator position (`T name[:]`)
   are accepted. String-literal and braced initializers lower through
   cc__slice_emit_decl_init. Requires a closing ']' and a ':' after '['. */
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
                    size_t name_s = 0, name_e = 0;
                    int decl_form = cc__slice_declarator_name(src, type_start, i, &name_s, &name_e);
                    /* Emit everything up to the slice element type, preserving
                       leading decl/function specifiers like `static inline`. */
                    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                    cc_sb_append(&out, &out_len, &out_cap, src + ty_start, type_start - ty_start);
                    if (decl_form) {
                        /* `T name[:] ...` — C declarator position. */
                        size_t resume = 0;
                        char inst[96];
                        if (!is_unique)
                            resume = cc__slice_emit_decl_init(&out, &out_len, &out_cap, src, n,
                                                              type_start, name_s,
                                                              name_s, name_e, k + 1);
                        if (resume) { last_emit = resume; i = resume; continue; }
                        if (is_unique)
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "CCSliceUnique ");
                        else if (cc__slice_instance_for_elem(src, type_start, name_s,
                                                             inst, sizeof(inst),
                                                             NULL, 0)) {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, inst);
                            cc_sb_append_cstr(&out, &out_len, &out_cap, " ");
                        } else
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "CCSlice ");
                        cc_sb_append(&out, &out_len, &out_cap, src + name_s, name_e - name_s);
                        last_emit = k + 1; /* skip past ']' */
                    } else {
                        size_t resume = 0;
                        char inst[96];
                        if (!is_unique) {
                            /* `T[:] name = "lit"` / `= {...}` — initializer lowering. */
                            size_t ns = cc_skip_ws_and_comments(src, n, k + 1);
                            if (ns < n && cc_is_ident_start(src[ns])) {
                                size_t ne = ns;
                                while (ne < n && cc_is_ident_char(src[ne])) ne++;
                                resume = cc__slice_emit_decl_init(&out, &out_len, &out_cap, src, n,
                                                                  type_start, i, ns, ne, ne);
                            }
                        }
                        if (resume) { last_emit = resume; i = resume; continue; }
                        if (is_unique)
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "CCSliceUnique");
                        else if (cc__slice_instance_for_elem(src, type_start, i,
                                                             inst, sizeof(inst),
                                                             NULL, 0))
                            cc_sb_append_cstr(&out, &out_len, &out_cap, inst);
                        else
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "CCSlice");
                        last_emit = k + 1; /* skip past ']' */
                    }
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
/* Defined with the param-list reflector; used here to strip defaults for C. */
static int cc__ct_param_default_at(const char* src, size_t ps, size_t pe,
                                   size_t* eq_pos);

/* Strip `name = literal` defaults from function parameter lists.  CC allows
 * the spelling for reflection / py_module optional kwargs; host C does not.
 * Call arguments like `foo(x = 1)` are left alone — only declaration-style
 * defaults (typed declarator on the left) are removed. */
static int cc__param_default_fn_name_ok(const char* src, size_t ns, size_t ne) {
    size_t n = ne - ns;
    if (n == 0) return 0;
#define CC__PD_KW(S) (n == sizeof(S) - 1 && memcmp(src + ns, S, sizeof(S) - 1) == 0)
    if (CC__PD_KW("if") || CC__PD_KW("for") || CC__PD_KW("while") ||
        CC__PD_KW("switch") || CC__PD_KW("sizeof") || CC__PD_KW("_Alignof") ||
        CC__PD_KW("alignof") || CC__PD_KW("typeof") || CC__PD_KW("__typeof__") ||
        CC__PD_KW("_Generic") || CC__PD_KW("return") || CC__PD_KW("case") ||
        CC__PD_KW("catch"))
        return 0;
#undef CC__PD_KW
    return 1;
}

static char* cc__rewrite_param_defaults(const char* src, size_t n,
                                        const char* input_path) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0, last_emit = 0;
    int changed = 0;
    CCScannerState scan;
    if (!src || n == 0) return NULL;
    cc_scanner_init(&scan);
    for (size_t i = 0; i < n; ) {
        size_t lp, rp, after, ns, ne, before;
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (src[i] != '(') { i++; continue; }
        lp = i;
        if (!cc_find_matching_paren(src, n, lp, &rp)) { i++; continue; }
        after = cc_skip_ws_and_comments(src, n, rp + 1);
        if (after >= n || (src[after] != '{' && src[after] != ';')) {
            i = rp + 1;
            continue;
        }
        before = cc_rskip_ws_and_comments(src, lp);
        if (before == 0 || !cc_is_ident_char(src[before - 1])) {
            i = rp + 1;
            continue;
        }
        ne = before;
        ns = ne;
        while (ns > 0 && cc_is_ident_char(src[ns - 1])) ns--;
        if (!cc__param_default_fn_name_ok(src, ns, ne)) {
            i = rp + 1;
            continue;
        }
        /* `recv.foo(...)` / `recv->foo(...)` are calls, not declarators. */
        before = cc_rskip_ws_and_comments(src, ns);
        if (before > 0 && (src[before - 1] == '.' ||
                           (before >= 2 && src[before - 1] == '>' &&
                            src[before - 2] == '-'))) {
            i = rp + 1;
            continue;
        }
        /* Rewrite the param list, omitting declaration-style defaults. */
        {
            size_t p = lp + 1;
            size_t chunk_from = last_emit;
            int list_changed = 0;
            char* piece = NULL;
            size_t piece_len = 0, piece_cap = 0;
            cc_sb_append(&piece, &piece_len, &piece_cap, src + chunk_from,
                         lp + 1 - chunk_from);
            while (p < rp) {
                CCScannerState s; cc_scanner_init(&s);
                size_t j = p, depth = 0, comma = rp;
                size_t ps, pe, eq;
                int dr;
                while (j < rp) {
                    if (cc_scanner_skip_non_code(&s, src, rp, &j)) continue;
                    char c = src[j];
                    if (c == '(' || c == '[') depth++;
                    else if (c == ')' || c == ']') { if (depth) depth--; }
                    else if (c == ',' && depth == 0) { comma = j; break; }
                    j++;
                }
                ps = p;
                pe = comma;
                dr = cc__ct_param_default_at(src, cc_skip_ws_and_comments(src, pe, ps),
                                             cc_rskip_ws_and_comments(src, pe), &eq);
                if (dr < 0) {
                    char rel[1024];
                    free(piece);
                    free(out);
                    cc_pp_error_cat(cc_path_rel_to_repo(
                                        input_path ? input_path : "<input>", rel,
                                        sizeof(rel)),
                                    scan.line, scan.col, "syntax",
                                    "parameter default must be a simple literal "
                                    "(integer, float, string, char, NULL, true, false)");
                    return (char*)-1;
                }
                if (dr > 0) {
                    size_t keep_end = cc_rskip_ws_and_comments(src, eq);
                    cc_sb_append(&piece, &piece_len, &piece_cap, src + ps,
                                 keep_end > ps ? keep_end - ps : 0);
                    if (comma < rp)
                        cc_sb_append(&piece, &piece_len, &piece_cap, ",", 1);
                    list_changed = 1;
                } else {
                    cc_sb_append(&piece, &piece_len, &piece_cap, src + ps,
                                 comma - ps);
                    if (comma < rp)
                        cc_sb_append(&piece, &piece_len, &piece_cap, ",", 1);
                }
                p = comma + 1;
            }
            if (list_changed) {
                cc_sb_append(&out, &out_len, &out_cap, piece, piece_len);
                last_emit = rp;
                changed = 1;
            }
            free(piece);
        }
        i = rp + 1;
    }
    if (!changed) {
        free(out);
        return NULL;
    }
    if (last_emit < n)
        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    cc_sb_append(&out, &out_len, &out_cap, "", 1);
    out_len -= 1;
    (void)out_len;
    return out;
}

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
    CCScannerState scan;
    cc_scanner_init(&scan);

    for (size_t i = 0; i < n; ) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        char c = src[i];

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
    CCScannerState scan;
    cc_scanner_init(&scan);
    for (size_t i = 0; i < n; ) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        char c = src[i];

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

/* Rewrite call-site `@blocking callee(args)` / `@noblock callee(args)`,
 * canonicalize preferred `@nonblocking` spelling, and mark lexical
 * `@nonblocking { ... }` / `@blocking { ... }` ambient blocks.
 *
 * Leaves a comment-marker form that survives TCC parsing as whitespace
 * but can be recovered by pass_autoblock by scanning source text
 * immediately before each CALL node:
 *
 *   @blocking foo(x)  ->  CC_SITE marker comment, then foo(x)
 *   @noblock  foo(x)  ->  CC_SITE marker comment, then foo(x)
 *   @nonblocking {    ->  CC_BLOCK marker comment, then {
 *
 * Decl-level attrs (`@blocking int foo(...)`) are *not* rewritten: the
 * decl-form has a type token between `@blocking` and the name, so the
 * pattern `@blocking IDENT (` does not match.  `@nonblocking` decl
 * attributes are rewritten to the existing `@noblock` spelling so the
 * TCC cc-ext hook records the same attr bit.  Function definitions
 * (where the closing `)` is followed by `{`) are otherwise skipped.
 * Spec §8.2.2 precedence rule 1. */
char* cc__rewrite_at_call_site_mode(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    /* Early exit if no mode marker appears outside
     * comments/strings.  The cheap pre-check avoids building the output
     * buffer in the common case. */
    if (!cc_contains_token_top_level(src, n, "@blocking") &&
        !cc_contains_token_top_level(src, n, "@noblock") &&
        !cc_contains_token_top_level(src, n, "@nonblocking")) {
        return NULL;
    }

    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t last_emit = 0;
    int changed = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);

    for (size_t i = 0; i < n; ) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        char c = src[i];

        if (c != '@') { i++; continue; }

        const char* mode = NULL;
        size_t kw_len = 0;
        int canonicalize_to_noblock = 0;
        if (i + 9 <= n && memcmp(src + i + 1, "blocking", 8) == 0 &&
            (i + 9 == n || !cc_is_ident_char(src[i + 9]))) {
            mode = "blocking"; kw_len = 8;
        } else if (i + 8 <= n && memcmp(src + i + 1, "noblock", 7) == 0 &&
                   (i + 8 == n || !cc_is_ident_char(src[i + 8]))) {
            mode = "noblock"; kw_len = 7;
        } else if (i + 12 <= n && memcmp(src + i + 1, "nonblocking", 11) == 0 &&
                   (i + 12 == n || !cc_is_ident_char(src[i + 12]))) {
            mode = "noblock"; kw_len = 11;
            canonicalize_to_noblock = 1;
        }
        if (!mode) { i++; continue; }

        /* Block ambient form: `@nonblocking { ... }` or `@blocking { ... }`.
         * The marker comment attaches to the immediately following block and
         * is interpreted by pass_autoblock as lexical ambient mode. */
        size_t p = i + 1 + kw_len;
        while (p < n && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) p++;
        if (p < n && src[p] == '{') {
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
            if (strcmp(mode, "blocking") == 0) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "/*@CC_BLOCK=blocking*/ ");
            } else {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "/*@CC_BLOCK=noblock*/ ");
            }
            last_emit = p;
            i = p;
            changed = 1;
            continue;
        }

        /* Need: `@<mode> IDENT (<balanced>) <not-'{'>` to call this a call-site.
         * Probe ahead without committing. */
        if (p >= n || !cc_is_ident_start(src[p])) {
            if (canonicalize_to_noblock) {
                cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "@noblock");
                last_emit = i + 1 + kw_len;
                i = last_emit;
                changed = 1;
                continue;
            }
            i++;
            continue;
        }
        size_t id_s = p;
        while (p < n && cc_is_ident_char(src[p])) p++;
        size_t id_e = p;
        size_t after_id = p;
        while (p < n && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r' || src[p] == '\n')) p++;
        if (p >= n || src[p] != '(') {
            if (canonicalize_to_noblock) {
                cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "@noblock");
                last_emit = i + 1 + kw_len;
                i = last_emit;
                changed = 1;
                continue;
            }
            i++;
            continue;
        }

        /* Find matching ')' respecting strings/chars/comments. */
        size_t rp = 0;
        if (!cc_find_matching_paren(src, n, p, &rp)) { i++; continue; }
        size_t rparen_end = rp + 1;

        /* Lookahead: if next non-ws char is `{`, it's a function
         * definition — leave the marker intact for TCC cc-ext. */
        size_t q = rparen_end;
        while (q < n && (src[q] == ' ' || src[q] == '\t' || src[q] == '\r' || src[q] == '\n')) q++;
        if (q < n && src[q] == '{') {
            if (canonicalize_to_noblock) {
                cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "@noblock");
                last_emit = i + 1 + kw_len;
                i = last_emit;
                changed = 1;
                continue;
            }
            i++;
            continue;
        }

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
        continue;

    }

    if (!changed) { free(out); return NULL; }
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* ---- @async body ranges (for @await context sensitivity) --------------- */
/* Byte ranges [start,end) of @async function bodies. Inside these, `@await`
 * must degrade to the bare `await` marker consumed by the async lowering —
 * stripping it (the sync-context behavior below) deletes the suspension
 * point, so e.g. `if (@await tx.send(v) != 0)` lowered the channel op as a
 * plain call and produced type errors. Outside these ranges the historical
 * sync-context behavior is unchanged. */
typedef struct { size_t start, end; } CCAsyncBodyRange;
#define CC__MAX_ASYNC_BODY_RANGES 512

static int cc__collect_async_body_ranges(const char* src, size_t n,
                                         CCAsyncBodyRange* out, int cap) {
    int count = 0;
    size_t i = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);
    while (i + 6 <= n && count < cap) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (src[i] == '@' && memcmp(src + i + 1, "async", 5) == 0 &&
            (i + 6 >= n || !cc_is_ident_char(src[i + 6]))) {
            /* Find the body '{' at paren depth 0 (stop at ';' = declaration). */
            size_t j = i + 6;
            int pdepth = 0, found = 0;
            CCScannerState jscan;
            cc_scanner_init(&jscan);
            while (j < n) {
                if (cc_scanner_skip_non_code(&jscan, src, n, &j)) continue;
                char d = src[j];
                if (d == '(') pdepth++;
                else if (d == ')') { if (pdepth > 0) pdepth--; }
                else if (d == '{' && pdepth == 0) { found = 1; break; }
                else if (d == ';' && pdepth == 0) break;
                j++;
            }
            if (found) {
                size_t b = j;
                int bdepth = 0;
                CCScannerState bscan;
                cc_scanner_init(&bscan);
                while (b < n) {
                    if (cc_scanner_skip_non_code(&bscan, src, n, &b)) continue;
                    if (src[b] == '{') bdepth++;
                    else if (src[b] == '}') {
                        bdepth--;
                        if (bdepth == 0) { b++; break; }
                    }
                    b++;
                }
                out[count].start = j;
                out[count].end = b;
                count++;
                i = b;
                continue;
            }
        }
        i++;
    }
    return count;
}

static int cc__pos_in_async_body(const CCAsyncBodyRange* ranges, int count, size_t pos) {
    for (int r = 0; r < count; r++) {
        if (pos >= ranges[r].start && pos < ranges[r].end) return 1;
    }
    return 0;
}

/* Rewrite @await <expr> in any context.
 * Inside an @async body: rewrite to the bare `await` marker (the async
 * lowering owns operand extent and suspension semantics).
 * In synchronous context: if <expr> is a call to a known @async function,
 * emit cc_block_on(T, expr); otherwise strip @await and keep the expression
 * (channel ops etc. are already blocking in synchronous context). */
char* cc__rewrite_at_await(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    /* Early exit only if `@await` is absent outside comments/strings. */
    if (!cc_contains_token_top_level(src, n, "@await")) return NULL;

    cc__collect_async_ret_types(src, n);

    static CCAsyncBodyRange async_ranges[CC__MAX_ASYNC_BODY_RANGES];
    int async_range_count = cc__collect_async_body_ranges(src, n, async_ranges, CC__MAX_ASYNC_BODY_RANGES);

    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t last_emit = 0;
    int changed = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);

    for (size_t i = 0; i < n; ) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        char c = src[i];

        /* Detect '@await' token */
        if (c == '@' && i+6 <= n && memcmp(src+i+1,"await",5) == 0
                     && (i+6 >= n || !cc_is_ident_char(src[i+6]))) {
            size_t j = i + 6;

            /* Inside an @async body: degrade to the bare `await` marker and
             * let the async lowering own operand extent + suspension. */
            if (cc__pos_in_async_body(async_ranges, async_range_count, i)) {
                cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "await");
                last_emit = j;
                changed = 1;
                i = j;
                continue;
            }

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
                    /* Find matching ')' of the call — comment- and
                     * char-literal-aware, so a ')' inside either does
                     * not terminate the span early. */
                    size_t m = j, rp = 0;
                    if (cc_find_matching_paren(src, n, m, &rp)) m = rp + 1;
                    else m = n;
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

/* Rewrite `*res` -> `cc_unwrap(res)` for variables declared with CCResult_*
 * type. Two-pass approach:
 *   1. Scan for CCResult_<T>_<E> or `__CC_RESULT(T, E)` variable declarations,
 *      recording each variable's LIVE RANGE: from its declaration to the end
 *      of the brace scope it was declared in (a parameter's scope is the
 *      function body that follows).  A `CCResult_...` local named `a` must
 *      not capture `*a` in some other function where `a` is a `T*`.
 *   2. Rewrite `*varname` to `cc_unwrap(varname)` only inside that range. */
static char* cc__rewrite_result_star_unwrap(const char* src, size_t n) {
    if (!src || n == 0) return NULL;

    /* Pass 1: Collect Result variable names + live ranges. */
    #define MAX_UNWRAP_VARS 256
    char* vars[MAX_UNWRAP_VARS];
    size_t var_from[MAX_UNWRAP_VARS];   /* decl position */
    size_t var_to[MAX_UNWRAP_VARS];     /* end of enclosing scope (n = open) */
    int var_depth[MAX_UNWRAP_VARS];     /* brace depth the decl lives at */
    int var_count = 0;

    size_t i = 0;
    int depth = 0;        /* brace depth */
    int pdepth = 0;       /* paren depth (param lists) */
    CCScannerState scan;
    cc_scanner_init(&scan);

    while (i < n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;

        char c = src[i];
        if (c == '{') { depth++; i++; continue; }
        if (c == '}') {
            depth--;
            if (depth < 0) depth = 0;
            /* close every var whose scope just ended */
            for (int j = 0; j < var_count; j++) {
                if (var_to[j] == n && var_depth[j] > depth) var_to[j] = i;
            }
            i++;
            continue;
        }
        if (c == '(') { pdepth++; i++; continue; }
        if (c == ')') { if (pdepth > 0) pdepth--; i++; continue; }
        if (var_count >= MAX_UNWRAP_VARS) { i++; continue; }

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
                        vars[var_count] = varname;
                        var_from[var_count] = var_start;
                        /* a decl inside parens is a parameter: it lives in
                         * the function body that follows, one level deeper */
                        var_depth[var_count] = pdepth > 0 ? depth + 1 : depth;
                        var_to[var_count] = n;
                        var_count++;
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
                    if (strlen(vars[j]) == var_len && strncmp(vars[j], src + var_start, var_len) == 0 &&
                        star_pos >= var_from[j] && star_pos < var_to[j]) {
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
    if (!cc_contains_token_top_level(src, n, "@link")) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    size_t last_emit = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);

    while (i < n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        char c = src[i];

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
            i = start;
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
    int in_async_fn = 0;      /* 1 if inside @async function body */
    int async_brace_depth = 0; /* Brace depth when we entered the async function */

    int brace_depth = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);

    for (size_t i = 0; i < n; ) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        char c = src[i];

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
            i++;
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
                /* Check if preceded by "await", skipping whitespace and
                 * block comments backwards — a block comment between the
                 * keyword and the call still counts as awaited. */
                int has_await = 0;
                size_t k = i;
                while (k > 0) {
                    k--;
                    char ck = src[k];
                    if (ck == ' ' || ck == '\t' || ck == '\n' || ck == '\r') continue;
                    if (ck == '/' && k > 0 && src[k - 1] == '*') {
                        size_t bk = k - 1;
                        int found = 0;
                        while (bk > 0) {
                            bk--;
                            if (src[bk] == '*' && bk > 0 && src[bk - 1] == '/') {
                                k = bk - 1;
                                found = 1;
                                break;
                            }
                        }
                        if (!found) break;
                        continue;
                    }
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
                    cc_pp_error_cat(rel, scan.line, scan.col, "async", "channel operation '%s' must be awaited in @async function", op_name);
                    fprintf(stderr, "  hint: add 'await' before this call\n");
                    errors++;
                }
                i += op_len - 1; /* Skip past the op name */
            }
        }
        i++;
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

/* Collect @async function info and check cc_block_on calls.
   Warns if cc_block_on is called with a function that has loops with channel ops.
   Returns number of warnings. */
static int cc__check_block_on_nonblocking(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return 0;

    cc__async_fn_count = 0;
    int warnings = 0;

    /* Pass 1: Collect @async functions and determine if they're @nonblocking */
    CCScannerState scan;
    cc_scanner_init(&scan);

    for (size_t i = 0; i < n; ) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        char c = src[i];

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
            if (j >= n || src[j] != '(') { i++; continue; }

            /* Back up to find function name (skipping whitespace and
             * block comments: `RT name / *c* / (` still reads `name`). */
            size_t paren = j;
            j = cc__rskip_sp_tab_block_comments(src, j);
            if (j == 0 || j - 1 <= i || !cc_is_ident_char(src[j - 1])) { i++; continue; }

            size_t name_end = j;
            while (j > i && cc_is_ident_char(src[j - 1])) j--;
            size_t name_start = j;
            size_t name_len = name_end - name_start;

            if (name_len == 0 || name_len >= 127) { i++; continue; }

            /* Find function body */
            j = paren + 1;
            int depth = 1;
            while (j < n && depth > 0) {
                if (src[j] == '(') depth++;
                else if (src[j] == ')') depth--;
                j++;
            }
            while (j < n && src[j] != '{' && src[j] != ';') j++;
            if (j >= n || src[j] != '{') { i++; continue; }

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

                /* Check for channel ops in loop body - look for .send( or
                 * .recv( (word-bounded, so `.sendfile(` does not count). */
                for (size_t m = loop_start; m < loop_end; m++) {
                    if (m + 5 <= loop_end &&
                        (memcmp(src + m, ".send", 5) == 0 ||
                         memcmp(src + m, ".recv", 5) == 0) &&
                        (m + 5 == loop_end || !cc_is_ident_char(src[m + 5]))) {
                        has_loop_with_chan = 1;
                        break;
                    }
                    /* Also check for chan_send/chan_recv macro forms */
                    if (m + 9 <= loop_end &&
                        (memcmp(src + m, "chan_send", 9) == 0 ||
                         memcmp(src + m, "chan_recv", 9) == 0) &&
                        (m == loop_start || !cc_is_ident_char(src[m - 1])) &&
                        (m + 9 == loop_end || !cc_is_ident_char(src[m + 9]))) {
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

            i = body_end;
            continue;
        }
        i++;
    }

    /* Pass 2: Check cc_block_on calls */
    cc_scanner_init(&scan);

    for (size_t i = 0; i < n; ) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        char c = src[i];

        /* Look for cc_block_on( */
        if (c == 'c' && i + 11 < n && memcmp(src + i, "cc_block_on", 11) == 0 && src[i + 11] == '(') {
            int call_line = scan.line, call_col = scan.col;
            size_t j = i + 12;

            /* Skip the type argument */
            while (j < n && src[j] != ',') j++;
            if (j >= n) { i++; continue; }
            j++; /* Skip comma */

            /* Skip whitespace */
            while (j < n && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j++;

            /* Extract the function name being called */
            if (j >= n || !cc_is_ident_start(src[j])) { i++; continue; }
            size_t fn_start = j;
            while (j < n && cc_is_ident_char(src[j])) j++;
            size_t fn_len = j - fn_start;

            if (fn_len == 0 || fn_len >= 127) { i++; continue; }

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
                        fprintf(stderr, "%s:%d:%d: note: '%s' has channel ops in a loop; consider explicit nursery concurrency or a larger buffer\n",
                                rel, call_line, call_col, fn_name);
                        warnings++;
                    }
                    break;
                }
            }

            i = j - 1;
        }
        i++;
    }

    return warnings;
}

/* Variable -> result-type table used by cc__rewrite_inferred_result_ctors.
 * Records variables declared with an explicit `CCResult_T_E` type (or the
 * `T !>(E) name` sugar) so short-form `cc_ok()`/`cc_err()` initializers and
 * assignments resolve against the variable's declared type instead of the
 * enclosing function's result type. */
typedef struct {
    char name[128];
    char rtype[256]; /* mangled, e.g. "CCResult_bool_CCError" */
} CCCtorTargetVar;

static void cc__ctor_target_vars_add(CCCtorTargetVar** vars, size_t* count, size_t* cap,
                                     const char* name, size_t name_len,
                                     const char* rtype, size_t rtype_len) {
    if (name_len == 0 || name_len >= sizeof((*vars)[0].name)) return;
    if (rtype_len == 0 || rtype_len >= sizeof((*vars)[0].rtype)) return;
    if (*count == *cap) {
        size_t nc = *cap ? *cap * 2 : 16;
        CCCtorTargetVar* nv = (CCCtorTargetVar*)realloc(*vars, nc * sizeof(**vars));
        if (!nv) return;
        *vars = nv;
        *cap = nc;
    }
    memcpy((*vars)[*count].name, name, name_len);
    (*vars)[*count].name[name_len] = 0;
    memcpy((*vars)[*count].rtype, rtype, rtype_len);
    (*vars)[*count].rtype[rtype_len] = 0;
    (*count)++;
}

/* Most-recent declaration wins so a redeclaration in a later function
 * shadows an earlier same-named variable. */
static const char* cc__ctor_target_vars_find(const CCCtorTargetVar* vars, size_t count,
                                             const char* name, size_t name_len) {
    for (size_t k = count; k > 0; k--) {
        if (strlen(vars[k - 1].name) == name_len &&
            memcmp(vars[k - 1].name, name, name_len) == 0) {
            return vars[k - 1].rtype;
        }
    }
    return NULL;
}

/* If the ctor call starting at `macro_start` is the RHS of
 * `<ident> = cc_ok(...)` / `<ident> = cc_err(...)` (declaration initializer
 * or plain reassignment), return the recorded result type for <ident>.
 * Returns NULL when the site is not an assignment (e.g. `return cc_ok(x)`,
 * a bare argument position) or the variable's type is unknown.  Member
 * assignments (`s.res = ...`, `p->res = ...`) are skipped: the bare field
 * name cannot be trusted to identify a type. */
static const char* cc__ctor_assign_target_type(const char* src, size_t macro_start,
                                               const CCCtorTargetVar* vars, size_t count) {
    size_t p = cc_rskip_ws_and_comments(src, macro_start);
    if (p == 0 || src[p-1] != '=') return NULL;
    p--;
    /* Reject `==`, `!=`, `<=`, `>=` and compound assignments. */
    if (p > 0 && strchr("=!<>+-*/%&|^", src[p-1])) return NULL;
    p = cc_rskip_ws_and_comments(src, p);
    size_t nm_end = p;
    while (p > 0 && cc_is_ident_char(src[p-1])) p--;
    if (nm_end == p || !cc_is_ident_start(src[p])) return NULL;
    if (p > 0 && (src[p-1] == '.' || (p > 1 && src[p-1] == '>' && src[p-2] == '-'))) return NULL;
    return cc__ctor_target_vars_find(vars, count, src + p, nm_end - p);
}

/* Infer result constructor types from enclosing function signature.
   Within a function returning `T!E`:
     cc_ok(v)   -> cc_ok(T, v)     for T!CCError
     cc_ok(v)   -> cc_ok(T, E, v)  for T!E (custom error)
     cc_err(e)  -> cc_err(T, e)    for T!CCError  
     cc_err(e)  -> cc_err(T, E, e) for T!E (custom error)
   
   This allows users to write just `cc_ok(42)` and have the compiler infer the type.
   When the constructor initializes (or is assigned to) a variable whose declared
   type is a concrete CCResult type, that type takes precedence over the enclosing
   function's result type. */
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

    /* Variables declared with a concrete CCResult type (explicit
       `CCResult_T_E name` or `T !>(E) name` sugar); used to resolve
       ctor targets for initializers/assignments. */
    CCCtorTargetVar* rvars = NULL;
    size_t rvar_count = 0, rvar_cap = 0;

    CCScannerState scan;
    cc_scanner_init(&scan);

    for (size_t i = 0; i < n; ) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;

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
        
        /* Record `CCResult_T_E name` declarations (not function declarators)
           so short-form ctors that initialize or assign these variables can
           resolve against the declared type. */
        if (c == 'C' && i + 9 < n && memcmp(src + i, "CCResult_", 9) == 0 &&
            (i == 0 || !cc_is_ident_char(src[i - 1]))) {
            size_t ty_start = i;
            size_t j = i + 9;
            while (j < n && cc_is_ident_char(src[j])) j++;
            size_t ty_end = j;
            size_t k = ty_end;
            while (k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\n' || src[k] == '\r')) k++;
            while (k < n && src[k] == '*') {
                k++;
                while (k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\n' || src[k] == '\r')) k++;
            }
            if (k < n && cc_is_ident_start(src[k])) {
                size_t nm_start = k;
                while (k < n && cc_is_ident_char(src[k])) k++;
                size_t nm_end = k;
                while (k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\n' || src[k] == '\r')) k++;
                if (k < n && src[k] != '(') {
                    cc__ctor_target_vars_add(&rvars, &rvar_count, &rvar_cap,
                                             src + nm_start, nm_end - nm_start,
                                             src + ty_start, ty_end - ty_start);
                }
            }
            i = ty_end;
            continue;
        }

        /* Record local `T !>(E) name [=|;]` declarations inside a function
           body.  The unwrap-operator forms (`!>;`, `!> @destroy`,
           `expr !>(e) { ... }`) never match because they are not followed
           by `(E) ident` and then `=`/`;`. */
        if (c == '!' && c2 == '>' && fn_brace_depth >= 0 && i > 0) {
            size_t j = i + 2;
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j < n && src[j] == '(') {
                j++;
                while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                size_t e_start = j;
                while (j < n && cc_is_ident_char(src[j])) j++;
                size_t e_end = j;
                while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                if (e_end > e_start && j < n && src[j] == ')') {
                    j++;
                    while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                    if (j < n && cc_is_ident_start(src[j])) {
                        size_t nm_start = j;
                        while (j < n && cc_is_ident_char(src[j])) j++;
                        size_t nm_end = j;
                        size_t k = nm_end;
                        while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
                        int is_decl = (k < n && (src[k] == ';' ||
                                      (src[k] == '=' && (k + 1 >= n || src[k + 1] != '='))));
                        if (is_decl) {
                            size_t ty_start = cc__scan_back_to_delim(src, i);
                            ty_start = cc__skip_leading_decl_specs(src, ty_start);
                            size_t ty_len = (ty_start < i) ? i - ty_start : 0;
                            while (ty_len > 0 && (src[ty_start + ty_len - 1] == ' ' ||
                                                  src[ty_start + ty_len - 1] == '\t')) ty_len--;
                            if (ty_len > 0 && ty_len < 128) {
                                char m_ok[128], m_err[128], full[300];
                                cc__mangle_type_name(src + ty_start, ty_len, m_ok, sizeof(m_ok));
                                cc__mangle_type_name(src + e_start, e_end - e_start, m_err, sizeof(m_err));
                                int fl = snprintf(full, sizeof(full), "CCResult_%s_%s", m_ok, m_err);
                                if (fl > 0 && (size_t)fl < sizeof(full)) {
                                    cc__ctor_target_vars_add(&rvars, &rvar_count, &rvar_cap,
                                                             src + nm_start, nm_end - nm_start,
                                                             full, (size_t)fl);
                                }
                            }
                        }
                    }
                }
            }
            /* Fall through to the shared i++ below: the `!>` text itself is
               left intact for the later result-type lowering pass. */
        }

        /* Detect function returning T!E or T!>(E) - look for pattern: T!E name( or T!>(E) name( 
           Handle space before ! (e.g., "MyData !> (MyError)" or "MyData*!>(MyError)") */
        if (c == '!' && c2 != '=' && fn_brace_depth < 0 && i > 0) {
            /* Skip backwards over whitespace and block comments to find
             * the type (`T / *doc* / !> (E) fn(` still reads `T`). */
            size_t prev_end = cc__rskip_sp_tab_block_comments(src, i);
            char prev = prev_end > 0 ? src[prev_end - 1] : src[0];
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
                    j = cc_skip_ws_and_comments(src, n, j);
                    if (j < n && src[j] == '(') {
                        j++; /* skip '(' */
                        j = cc_skip_ws_and_comments(src, n, j);
                        err_start = j;
                        while (j < n && cc_is_ident_char(src[j])) j++;
                        err_end = j;
                        j = cc_skip_ws_and_comments(src, n, j);
                        if (j < n && src[j] == ')') j++; /* skip ')' */
                    }
                } else {
                    /* Simple !E form */
                    j = cc_skip_ws_and_comments(src, n, j);
                    if (j < n && cc_is_ident_start(src[j])) {
                        err_start = j;
                        while (j < n && cc_is_ident_char(src[j])) j++;
                        err_end = j;
                    }
                }
                
                if (err_start < err_end) {
                    
                    /* Skip inert bytes and pointer stars, then check for an
                     * identifier followed by '('. */
                    for (;;) {
                        size_t k = cc_skip_ws_and_comments(src, n, j);
                        if (k < n && src[k] == '*') { j = k + 1; continue; }
                        j = k;
                        break;
                    }
                    if (j < n && cc_is_ident_start(src[j])) {
                        size_t name_start = j;
                        while (j < n && cc_is_ident_char(src[j])) j++;
                        (void)name_start; /* function name, not needed */
                        j = cc_skip_ws_and_comments(src, n, j);
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
                            j = cc_skip_ws_and_comments(src, n, j);
                            if (j < n && src[j] == '{') {
                                /* Found function definition! Extract ok and err types */
                                size_t ty_start = cc__scan_back_to_delim(src, i);
                                if (ty_start < i) {
                                    ty_start = cc__skip_leading_decl_specs(src, ty_start);
                                    size_t ty_len = i - ty_start;
                                    if (ty_len < sizeof(current_ok_type) - 1) {
                                        /* Trim whitespace and trailing comments */
                                        size_t ty_e = cc__rskip_sp_tab_block_comments(src, ty_start + ty_len);
                                        ty_len = (ty_e > ty_start) ? ty_e - ty_start : 0;
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
        
        /* Rewrite cc_ok(...) and cc_err(...) when inside a result-returning
           function, or when the ctor targets a variable with a known
           concrete CCResult type. */
        if ((current_ok_type[0] || rvar_count > 0) && c == 'c' && i + 5 < n) {
            int is_ok = (n - i >= 6 && memcmp(src + i, "cc_ok", 5) == 0 &&
                         !cc_is_ident_char(src[i + 5]));
            int is_err = (n - i >= 7 && memcmp(src + i, "cc_err", 6) == 0 &&
                          !cc_is_ident_char(src[i + 6]));

            if (is_ok || is_err) {
                /* Check word boundary */
                int word_start = (i == 0) || !cc_is_ident_char(src[i - 1]);
                /* The ctor name and its `(` are separate tokens; an inert run
                 * may sit between them. */
                size_t paren_pos =
                    cc_skip_ws_and_comments(src, n, i + (is_ok ? 5 : 6));
                if (word_start && paren_pos < n && src[paren_pos] == '(') {
                    size_t macro_start = i;

                    /* Count args to determine if short form */
                    size_t args_start = paren_pos + 1;
                    size_t j = args_start;
                    int depth = 1;
                    int brace_depth = 0;
                    int bracket_depth = 0;
                    int comma_count = 0;
                    int in_s = 0, in_c = 0, in_lc2 = 0, in_bc2 = 0;
                    while (j < n && depth > 0) {
                        char ch = src[j];
                        char ch2 = (j + 1 < n) ? src[j + 1] : 0;
                        if (in_lc2) { if (ch == '\n') in_lc2 = 0; j++; continue; }
                        if (in_bc2) { if (ch == '*' && ch2 == '/') { in_bc2 = 0; j += 2; } else j++; continue; }
                        if (in_s) { if (ch == '\\' && j+1<n) j++; else if (ch == '"') in_s = 0; j++; continue; }
                        if (in_c) { if (ch == '\\' && j+1<n) j++; else if (ch == '\'') in_c = 0; j++; continue; }
                        if (ch == '/' && ch2 == '/') { in_lc2 = 1; j += 2; continue; }
                        if (ch == '/' && ch2 == '*') { in_bc2 = 1; j += 2; continue; }
                        if (ch == '"') { in_s = 1; j++; continue; }
                        if (ch == '\'') { in_c = 1; j++; continue; }
                        if (ch == '{') brace_depth++;
                        else if (ch == '}') { if (brace_depth > 0) brace_depth--; }
                        else if (ch == '[') bracket_depth++;
                        else if (ch == ']') { if (bracket_depth > 0) bracket_depth--; }
                        else if (ch == '(') depth++;
                        else if (ch == ')' && brace_depth == 0 && bracket_depth == 0) { depth--; if (depth == 0) break; }
                        else if (ch == ',' && depth == 1 && brace_depth == 0 && bracket_depth == 0) comma_count++;
                        j++;
                    }
                    
                    /* Short form: cc_ok(v) has 0 commas, cc_err(e) has 0 commas
                       Long form: cc_ok(T,v) has 1+ commas
                       Shorthand: cc_err(CC_ERR_*, "msg") has 1 comma - expand to CC_ERROR(...) */
                    int is_short = (comma_count == 0);

                    /* Resolve the target result type.  A declaration
                       initializer or assignment to a variable with a known
                       CCResult type takes precedence over the enclosing
                       function's result type; `return cc_ok(...)` and bare
                       expression positions keep the function's type. */
                    const char* target_rtype =
                        cc__ctor_assign_target_type(src, macro_start, rvars, rvar_count);
                    char full_rtype[300] = {0};
                    if (target_rtype) {
                        snprintf(full_rtype, sizeof(full_rtype), "%s", target_rtype);
                    } else if (current_ok_type[0]) {
                        char mangled_ok[128], mangled_err[128];
                        cc__mangle_type_name(current_ok_type, strlen(current_ok_type), mangled_ok, sizeof(mangled_ok));
                        cc__mangle_type_name(current_err_type, strlen(current_err_type), mangled_err, sizeof(mangled_err));
                        snprintf(full_rtype, sizeof(full_rtype), "CCResult_%s_%s", mangled_ok, mangled_err);
                    }
                    size_t full_rtype_len = strlen(full_rtype);
                    int err_is_ccerror =
                        (full_rtype_len >= 8 &&
                         strcmp(full_rtype + full_rtype_len - 8, "_CCError") == 0);
                    
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
                    if (is_err && err_is_ccerror) {
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
                           Rewrite cc_err(CC_ERR_*, "msg") -> cc_err_CCResult_T_E(CC_ERROR(CC_ERR_*, "msg")) */
                        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, macro_start - last_emit);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_err_");
                        cc_sb_append_cstr(&out, &out_len, &out_cap, full_rtype);
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
                    
                    if (is_short && depth == 0 && full_rtype[0]) {
                        /* Rewrite short form to typed constructor call:
                           cc_ok(x) -> cc_ok_CCResult_T_E(x)
                           cc_err(e) -> cc_err_CCResult_T_E(e)
                           Into a CCError face, project unique @as (Io→base). */
                        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, macro_start - last_emit);
                        
                        if (is_ok) {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_ok_");
                        } else {
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "cc_err_");
                        }
                        cc_sb_append_cstr(&out, &out_len, &out_cap, full_rtype);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "(");
                        if (is_err && err_is_ccerror) {
                            size_t a0 = args_start, a1 = j;
                            char ident[128];
                            size_t ii = 0;
                            int is_id = 1;
                            char path[256];
                            int match = -1;
                            CCTypeRegistry* reg = cc_type_registry_get_global();
                            while (a0 < a1 && (src[a0] == ' ' || src[a0] == '\t' ||
                                               src[a0] == '\n' || src[a0] == '\r'))
                                a0++;
                            while (a1 > a0 && (src[a1 - 1] == ' ' || src[a1 - 1] == '\t' ||
                                               src[a1 - 1] == '\n' || src[a1 - 1] == '\r'))
                                a1--;
                            for (ii = 0; a0 + ii < a1 && ii + 1 < sizeof(ident); ii++) {
                                char ch = src[a0 + ii];
                                if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                      ch == '_' || (ii > 0 && ch >= '0' && ch <= '9'))) {
                                    is_id = 0;
                                    break;
                                }
                                ident[ii] = ch;
                            }
                            ident[ii] = 0;
                            if (is_id && ii > 0 && a0 + ii == a1 && reg) {
                                const char* oty = cc_type_registry_lookup_var(reg, ident);
                                if (oty && oty[0] && strcmp(oty, "CCError") != 0) {
                                    match = cc_type_registry_as_path_for_type(
                                        reg, oty, "CCError", path, sizeof(path));
                                    if (match == -2) {
                                        free(out);
                                        free(rvars);
                                        fprintf(stderr,
                                                "error: cc_err: ambiguous as: path "
                                                "from '%s' to 'CCError'\n",
                                                oty);
                                        return NULL;
                                    }
                                }
                            }
                            if (match == 0 && path[0]) {
                                cc_sb_append(&out, &out_len, &out_cap, src + a0, a1 - a0);
                                cc_sb_append_cstr(&out, &out_len, &out_cap, ".");
                                cc_sb_append_cstr(&out, &out_len, &out_cap, path);
                            } else {
                                /* Pass through when no unique @as path (see
                                 * pass_type_syntax.c — no hardcoded CCIoError). */
                                cc_sb_append(&out, &out_len, &out_cap, src + a0, a1 - a0);
                            }
                        } else {
                            cc_sb_append(&out, &out_len, &out_cap, src + args_start, j - args_start);
                        }
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
    
    free(rvars);
    if (last_emit == 0) {
        free(out);
        return NULL;
    }
    if (last_emit < n) cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Check for cc_concurrent usage and emit error with migration guidance.
   cc_concurrent syntax is deprecated; use cc_block_all instead. */
static char* cc__rewrite_cc_concurrent(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    if (!cc_contains_token_top_level(src, n, "cc_concurrent")) return NULL;

    CCScannerState scan;
    cc_scanner_init(&scan);

    for (size_t i = 0; i < n;) {
        char c;
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        c = src[i];

        if (c == 'c' && i + 12 < n && memcmp(src + i, "cc_concurrent", 13) == 0) {
            if (i > 0 && cc_is_ident_char(src[i - 1])) { i++; continue; }
            if (i + 13 < n && cc_is_ident_char(src[i + 13])) { i++; continue; }

            cc_pp_error_cat("<input>", scan.line, 1, "syntax", "'cc_concurrent' syntax is deprecated (use cc_block_all instead)");
            fprintf(stderr, "  note: use cc_block_all() instead:\n");
            fprintf(stderr, "    CCTaskIntptr tasks[] = { task1(), task2() };\n");
            fprintf(stderr, "    intptr_t results[2];\n");
            fprintf(stderr, "    cc_block_all(2, tasks, results);\n");
            return (char*)-1;
        }
        i++;
    }

    return NULL;
}
int cc_preprocess_file(const char* input_path, char* out_path, size_t out_path_sz) {
    if (!input_path || !out_path || out_path_sz == 0) return -1;
    char tmp_path[] = "/tmp/cc_pp_XXXXXX.c";
    int fd = mkstemps(tmp_path, 2); /* keep .c suffix */
    if (fd < 0) return -1;

    /* Per-TU type graph (wraps registry — see type_graph.h / COMPTIME_INSTANTIATION_SEAM.md). */
    CCTypeGraph* graph = cc_type_graph_ensure_global_cleared();
    if (!graph) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
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

    /* Explicit @comptime cc_instantiate_* requests (track C1).  Collect from
     * the raw file (blocks not yet blanked) and replay into the graph. */
    if (cc_emit_plan_comptime_instantiation_count() == 0) {
        cc_emit_plan_collect_comptime_instantiations(buf, got);
    }
    cc_emit_plan_apply_comptime_instantiations(graph);

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
    if (cc__apply_phase1_canonical_passes(&chain, input_path, 0) != 0) goto chain_cleanup;
    /* Transitional pre-phase-3 exception: nursery handle prototype synthesis
       still runs outside the shared host-lowering bucket. */
    CC_CHAIN(chain, cc_rewrite_nursery_create_destroy_proto(chain.src, chain.len, input_path));
    /* Shared phase-3 host lowering bucket. */
    if (cc__apply_phase3_host_lowering_passes(&chain, input_path) != 0) goto chain_cleanup;
    cc__register_lowered_vec_macros(chain.src);
    cc__register_lowered_map_macros(chain.src);
    
    const char* use = chain.src;

    /* Emit container type declarations from type registry */
    {
        size_t n_vec = cc_type_graph_vec_count(graph);
        size_t n_map = cc_type_graph_map_count(graph);
        size_t n_chan = cc_type_graph_channel_count(graph);
        size_t n_slice = cc_slice_spec_count();

        if (n_vec > 0 || n_map > 0 || n_chan > 0 || n_slice > 0) {
            cc_emit_plan_fprint_container_prelude(out, 0,
                n_vec > 0, n_map > 0, n_chan > 0);
            
            /* Emit Vec declarations */
            for (size_t i = 0; i < n_vec; i++) {
                cc_emit_plan_fprint_vec_decl(out, cc_type_graph_get_vec(graph, i));
            }

            /* Emit Map declarations */
            for (size_t i = 0; i < n_map; i++) {
                cc_emit_plan_fprint_map_decl(out, cc_type_graph_get_map(graph, i));
            }

            /* Typed slice instances no declaration covers */
            for (size_t i = 0; i < n_slice; i++) {
                const char* nm = NULL;
                const char* el = NULL;
                if (cc_slice_spec_get(i, &nm, &el) != 0) continue;
                if (!cc_slice_spec_tu_needs_decl(use, strlen(use), nm, el)) continue;
                fprintf(out, "CC_DECL_SLICE_SPEC(%s, %s)\n", nm, el);
            }

            for (size_t i = 0; i < n_chan; i++) {
                const CCTypeInstantiation* inst = cc_type_graph_get_channel(graph, i);
                if (inst && inst->type1 && inst->mangled_name) {
                }
            }

            fprintf(out, "/* --- end container declarations --- */\n\n");
        }
    }

    /* If result types are used, include cc_result.cch with CC_PARSER_MODE so
       parser-only fallbacks remain available while concrete Result names stay intact. */
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

static char* cc_preprocess_pipeline_ex(const char* input, size_t input_len, const char* input_path,
                                       int skip_checks, int skip_comptime_surface, int mode);
enum {
    CC_PP_MODE_FULL = 0,
    CC_PP_MODE_CANONICAL_ONLY = 1,
    CC_PP_MODE_EMIT_SPLICE_ONLY = 2,
};

char* cc_preprocess_to_string_ex(const char* input, size_t input_len, const char* input_path, int skip_checks) {
    return cc_preprocess_pipeline_ex(input, input_len, input_path, skip_checks, 0, CC_PP_MODE_FULL);
}

char* cc_preprocess_canonicalize(const char* input, size_t input_len, const char* input_path,
                                 int skip_checks, int skip_comptime_surface) {
    return cc_preprocess_pipeline_ex(input, input_len, input_path, skip_checks, skip_comptime_surface,
                                     CC_PP_MODE_CANONICAL_ONLY);
}

/* REPARSE DIET: emit-splice coordinate accounting.  Every splice insertion
 * lands at a known user-coordinate anchor and user text is copied verbatim
 * between anchors, so for user text AFTER the last anchor:
 *     out_off = user_off + delta.
 * The reparse wrapper reads this to advertise an EXACT
 * offset mapping on the AST root instead of falling back to line walking.
 * `user_text_rewritten` flags the one case where user bytes themselves
 * changed (system-include lowering) and the anchors are meaningless.
 * `delta` can be NEGATIVE (a splice may also drop text, e.g. consumed
 * directives) — consumers must not assume growth.
 * REENTRANCY: process globals, reset at the top of each emit-splice run
 * and read by the caller immediately after it returns.  The compiler is
 * single-threaded per TU; a nested emit-splice call between run and read
 * would clobber these — don't introduce one.  The wrapper's tail memcmp
 * verifies the accounting regardless, so a violation degrades to the
 * line-keyed fallback rather than corrupting offsets. */
static size_t g_cc_pp_splice_last_anchor = 0;
static long   g_cc_pp_splice_delta = 0;
static int    g_cc_pp_splice_user_text_rewritten = 0;

void cc_pp_get_splice_coord_info(size_t* last_anchor, long* delta, int* user_rewritten) {
    if (last_anchor) *last_anchor = g_cc_pp_splice_last_anchor;
    if (delta) *delta = g_cc_pp_splice_delta;
    if (user_rewritten) *user_rewritten = g_cc_pp_splice_user_text_rewritten;
}

char* cc_preprocess_emit_splice(const char* input, size_t input_len, const char* input_path,
                                int skip_checks) {
    return cc_preprocess_pipeline_ex(input, input_len, input_path, skip_checks, 0,
                                     CC_PP_MODE_EMIT_SPLICE_ONLY);
}

static char* cc_preprocess_pipeline_ex(const char* input, size_t input_len, const char* input_path,
                                       int skip_checks, int skip_comptime_surface, int mode) {
    if (!input || input_len == 0) return NULL;

    /* Per-TU type graph (wraps registry — see type_graph.h).  Reparses reuse
     * the active registry populated during initial parse / visitor passes. */
    /* Emit splice reuses the registry populated by a prior canonicalize pass. */
    CCTypeGraph* graph = (mode == CC_PP_MODE_EMIT_SPLICE_ONLY || skip_checks)
        ? cc_type_graph_get_global()
        : cc_type_graph_ensure_global_cleared();
    if (!graph && !skip_checks) return NULL;
    if (!graph) {
        graph = cc_type_graph_ensure_global_cleared();
        if (!graph) return NULL;
    }
    if (mode == CC_PP_MODE_CANONICAL_ONLY) {
        cc_result_spec_table_reset(&cc__result_specs);
        cc_result_spec_table_set_global(&cc__result_specs);
    } else if (mode == CC_PP_MODE_FULL) {
        cc_result_spec_table_set_global(&cc__result_specs);
    } else if (mode == CC_PP_MODE_EMIT_SPLICE_ONLY) {
        cc_result_spec_table_set_global(&cc__result_specs);
    }
    char* buf = NULL;
    size_t got = 0;
    if (mode != CC_PP_MODE_EMIT_SPLICE_ONLY) {
        buf = (char*)malloc(input_len + 1);
        if (!buf) return NULL;
        memcpy(buf, input, input_len);
        buf[input_len] = 0;
        got = input_len;

        /* Closer-anchored template dedent, before any pass reads a
         * template body.  Idempotent: a dedented closer sits at column 0,
         * so the fixpoint's repeat passes are no-ops. */
        {
            size_t dlen = 0;
            char* ded = cc_tpl_dedent_text(buf, got, input_path, &dlen);
            if (ded == (char*)-1) { free(buf); return NULL; }
            if (ded) {
                free(buf);
                buf = ded;
                got = dlen;
            }
        }

        if (!skip_checks) {
            if (cc_emit_plan_comptime_fragment_count() == 0) {
                cc_emit_plan_collect_comptime_emits(buf, got);
            }
            if (cc_emit_plan_comptime_instantiation_count() == 0) {
                cc_emit_plan_collect_comptime_instantiations(buf, got);
            }
        }
        /* Replay explicit @comptime cc_instantiate_* requests into the graph so
         * forced monomorphs are emitted even when the type is never spelled as
         * CCVec::[T] / Map::[K,V] in source (track C1). */
        if (!skip_checks) {
            cc_emit_plan_apply_comptime_instantiations(graph);
        }

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
    }

    /* --- Apply preprocessing passes using chain helper --- */
    CCPassChain chain;
    char* chain_owned = NULL;
    const char* use;
    size_t use_len;
    if (mode == CC_PP_MODE_EMIT_SPLICE_ONLY) {
        use = input;
        use_len = input_len;
        cc_pass_chain_init(&chain, NULL, 0);
    } else {
        cc_pass_chain_init(&chain, buf, got);
        if (cc__apply_phase1_canonical_passes(&chain, input_path, skip_comptime_surface) != 0) {
            goto chain_cleanup;
        }
        if (cc__apply_phase3_host_lowering_passes(&chain, input_path) != 0) goto chain_cleanup;
        cc__register_lowered_vec_macros(chain.src);
        cc__register_lowered_map_macros(chain.src);
        use = chain.src;
        use_len = chain.len;
        if (mode == CC_PP_MODE_CANONICAL_ONLY) {
            chain_owned = strdup(use);
            cc_pass_chain_free(&chain);
            free(buf);
            return chain_owned;
        }
    }

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

    /* Container declarations are spliced into the user source at
     * `container_pos` (after leading #includes, before typedefs that
     * reference container types).  See cc_emit_plan_compute_container_anchor. */

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
    use_len = use ? strlen(use) : 0;
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
    g_cc_pp_splice_last_anchor = 0;
    g_cc_pp_splice_delta = 0;
    g_cc_pp_splice_user_text_rewritten = 0;
    {
        char* lowered_system_use = cc_rewrite_system_cch_includes_to_lowered_headers(use, strlen(use));
        if (lowered_system_use) {
            use = lowered_system_use;
            g_cc_pp_splice_user_text_rewritten = 1;
        }
        fprintf(out, "#line 1 \"%s\"\n", cc_path_rel_to_repo(input_path ? input_path : "<string>", rel, sizeof(rel)));
        {
            size_t use_len = strlen(use);
            size_t insert_pos = cc_emit_plan_compute_prelude_insert_pos(use, use_len);
            CCEmitPlanResultDelay result_delay;
            CCEmitPlanContainerSchedule ctnr_sched;
            CCEmitPlanComptimeSchedule comptime_sched;
            unsigned char comptime_emitted[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS];
            memset(comptime_emitted, 0, sizeof(comptime_emitted));
            cc_emit_plan_build_result_delays(use, use_len, &cc__result_specs, insert_pos, &result_delay);
            cc_emit_plan_build_container_schedule(use, use_len, graph, &ctnr_sched);
            size_t container_pos = ctnr_sched.anchor_pos;
            size_t n_vec_ctnr = ctnr_sched.n_vec;
            size_t n_map_ctnr = ctnr_sched.n_map;
            size_t n_slice_ctnr = ctnr_sched.n_slice;
            int have_slice_anchor_decls = 0;
            for (size_t i = 0; i < n_slice_ctnr && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
                if (ctnr_sched.slice_emit[i] && !ctnr_sched.slice_delayed[i]) {
                    have_slice_anchor_decls = 1;
                    break;
                }
            }
            int have_container_decls =
                (n_vec_ctnr > 0 || n_map_ctnr > 0 || have_slice_anchor_decls);
            cc_emit_plan_build_comptime_schedule(use, use_len, input_path,
                                                  insert_pos, container_pos,
                                                  &comptime_sched);
            cc_emit_plan_warn_duplicate_symbols(use, use_len, input_path);
            {
                size_t cursor = have_container_decls ? container_pos : insert_pos;
                if (have_container_decls) {
                    g_cc_pp_splice_last_anchor = container_pos;
                    fwrite(use, 1, container_pos, out);
                    cc_emit_plan_fprint_container_prelude(out, 1,
                        n_vec_ctnr > 0, n_map_ctnr > 0, 0);
                    for (size_t i = 0; i < n_vec_ctnr && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
                        if (ctnr_sched.vec_delayed[i]) continue;
                        cc_emit_plan_fprint_vec_decl(out, cc_type_graph_get_vec(graph, i));
                    }
                    for (size_t i = 0; i < n_map_ctnr && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
                        if (ctnr_sched.map_delayed[i]) continue;
                        cc_emit_plan_fprint_map_decl(out, cc_type_graph_get_map(graph, i));
                    }
                    for (size_t i = 0; i < n_slice_ctnr && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
                        const char* nm = NULL;
                        const char* el = NULL;
                        size_t fu;
                        if (!ctnr_sched.slice_emit[i] || ctnr_sched.slice_delayed[i]) continue;
                        if (cc_slice_spec_get(i, &nm, &el) != 0) continue;
                        /* Attribute the expansion to the first use site so
                         * an error inside it (e.g. undeclared element type)
                         * points at the line that named the instance. */
                        fu = cc_emit_plan_find_ident_any(use, use_len, nm);
                        if (fu < use_len)
                            cc_emit_plan_fprint_line_directive(out, use, fu, input_path);
                        fprintf(out, "CC_DECL_SLICE_SPEC(%s, %s)\n", nm, el);
                    }
                    cc_emit_plan_fprint_container_epilogue(out);
                    cc_emit_plan_fprint_line_directive(out, use, container_pos, input_path);
                } else {
                    fwrite(use, 1, insert_pos, out);
                }
                int insert_emitted = 0;
                for (;;) {
                    size_t next_pos = use_len + 1;
                    if (!insert_emitted && cursor <= insert_pos && insert_pos < next_pos) {
                        next_pos = insert_pos;
                    }
                    for (size_t i = 0; i < n_vec_ctnr && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
                        if (!ctnr_sched.vec_delayed[i]) continue;
                        if (ctnr_sched.vec_pos[i] > cursor && ctnr_sched.vec_pos[i] < next_pos) {
                            next_pos = ctnr_sched.vec_pos[i];
                        }
                    }
                    for (size_t i = 0; i < n_map_ctnr && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
                        if (!ctnr_sched.map_delayed[i]) continue;
                        if (ctnr_sched.map_pos[i] > cursor && ctnr_sched.map_pos[i] < next_pos) {
                            next_pos = ctnr_sched.map_pos[i];
                        }
                    }
                    for (size_t i = 0; i < n_slice_ctnr && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
                        if (!ctnr_sched.slice_emit[i] || !ctnr_sched.slice_delayed[i]) continue;
                        if (ctnr_sched.slice_pos[i] > cursor && ctnr_sched.slice_pos[i] < next_pos) {
                            next_pos = ctnr_sched.slice_pos[i];
                        }
                    }
                    for (size_t i = 0; i < cc__result_specs.count && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
                        if (!result_delay.delayed[i]) continue;
                        if (result_delay.pos[i] > cursor && result_delay.pos[i] < next_pos) {
                            next_pos = result_delay.pos[i];
                        }
                    }
                    for (size_t i = 0; i < comptime_sched.n; i++) {
                        if (comptime_emitted[i]) continue;
                        if (comptime_sched.pos[i] > cursor && comptime_sched.pos[i] < next_pos) {
                            next_pos = comptime_sched.pos[i];
                        }
                    }
                    if (next_pos > use_len) {
                        if (cursor < use_len) {
                            fwrite(use + cursor, 1, use_len - cursor, out);
                        }
                        break;
                    }
                    if (next_pos > cursor) {
                        fwrite(use + cursor, 1, next_pos - cursor, out);
                    }
                    if (!insert_emitted && next_pos == insert_pos) {
                        insert_emitted = 1;
                        fprintf(out, "/* --- CC result type declarations (typed, post-prelude) --- */\n");
                for (size_t i = 0; i < cc__result_specs.count; i++) {
                    const CCResultSpec* spec = cc_result_spec_table_get(&cc__result_specs, i);
                    const char* ok_m = spec ? spec->mangled_ok : NULL;
                    const char* err_m = spec ? spec->mangled_err : NULL;
                    const char* ok_ty = spec ? spec->ok_type : NULL;
                    const char* err_ty = spec ? spec->err_type : NULL;
                    if (i < CC_EMIT_PLAN_MAX_DELAYED && result_delay.delayed[i]) continue;
                    if (!ok_m || !err_m || !ok_ty || !err_ty) continue;
                    /* Stdlib-predeclared specs (`CCResult_CCDirIterptr_CCIoError`
                     * etc.) have their `CC_DECL_RESULT_SPEC` expansion baked
                     * into the owning `.cch` header (e.g. `dir.cch`), with no
                     * include-guard around it.  Re-emitting the spec here
                     * triggers "struct/union/enum already defined" at the
                     * duplicate typedef.  The post-prelude
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
                        /* Void ok type: same generator, different shape.  The
                         * struct alone is not a Result — without cc_ok_/cc_err_
                         * the constructors stay undeclared, and without
                         * _is_err/_error the sigil cannot propagate. */
                        fprintf(out, "CC_DECL_RESULT_SPEC_VOID(CCResult_%s_%s, %s)\n",
                                ok_m, err_m, err_ty);
                    } else {
                        fprintf(out, "CC_DECL_RESULT_SPEC(CCResult_%s_%s, %s, %s)\n",
                                ok_m, err_m, ok_ty, err_ty);
                    }
                    fprintf(out, "#endif\n");
                }
                /* Forward-declare the stdlib-predeclared Result struct tags
                 * so `_Generic` can reference them by type name even when
                 * the TU doesn't `#include` the owning header.  `_Generic`
                 * arm selectors only require a declared type, not a
                 * complete one — if an arm's body is never evaluated
                 * (because the controlling expression has a different
                 * type) the incomplete struct is never accessed.  In TUs
                 * that *do* include e.g. `ccc/std/io.cch`, the header's
                 * `CC_DECL_RESULT_SPEC` expansion supplies the full
                 * definition and the typedef here is benignly repeated.
                 *
                 * These tags MUST precede the parser-helper prototypes
                 * below: the decl loop above skips stdlib-predeclared
                 * specs (their definition belongs to the owning header),
                 * so when a user spec hits a predeclared name in a TU
                 * that never includes that header, this tag is the only
                 * declaration of the type name the prototypes mention.
                 * An incomplete parameter type in a declaration is fine;
                 * an undeclared one is a parse error at the parameter. */
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
                /* Result types the include scan found declared in headers
                 * (`cc_result_fn_registry_scan_source`) join the roster
                 * too.  Without them, `!>` on a callee the registry cannot
                 * type — a macro name, a function pointer — falls to the
                 * `default:` arm, which hands back the WHOLE Result struct;
                 * a mismatched destination then dies in TCC as
                 * "'{' expected (got ';')" with nothing pointing here.
                 * Forward tags suffice: parser-mode TCC never evaluates an
                 * unselected arm's body, and the real compile emits its own
                 * roster from complete types (shadow_lower / host cc). */
                for (size_t ri = 0; ri < cc_result_fn_registry_count(); ri++) {
                    const char* t = cc_result_fn_registry_result_type_at(ri);
                    if (!t || strncmp(t, "CCResult_", 9) != 0) continue;
                    fprintf(out,
                        "#ifndef %s_FWD_DECLARED\n"
                        "#define %s_FWD_DECLARED 1\n"
                        "typedef struct %s %s;\n"
                        "#endif\n",
                        t, t, t, t);
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
                    if (i < CC_EMIT_PLAN_MAX_DELAYED && result_delay.delayed[i]) continue;
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
                 * The product emit path enumerates the same arms for the
                 * real compile; we mirror it here so the initial
                 * parser-mode parse type-checks too.  Every CCResult_T_E
                 * struct shares layout `{ bool ok; union { T value; E
                 * error; } u; }`, so the casts pick out the right field
                 * regardless of T / E. */
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
                    if (i < CC_EMIT_PLAN_MAX_DELAYED && result_delay.delayed[i]) continue;
                    if (!spec || !spec->mangled_ok[0] || !spec->mangled_err[0]) continue;
                    char key[192];
                    char arm[256];
                    snprintf(key, sizeof(key), "CCResult_%s_%s", spec->mangled_ok, spec->mangled_err);
                    if (CC_UW_ARM_EMIT_CHECK(&seen, key)) continue;
                    cc_emit_plan_format_result_arm(arm, sizeof(arm), key, CC_RESULT_ARM_IS_ERR, 0, 0);
                    fputs(arm, out);
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
                    {
                        char arm[256];
                        cc_emit_plan_format_result_arm(arm, sizeof(arm), p->concrete_name,
                                                       CC_RESULT_ARM_IS_ERR, 0, 0);
                        fputs(arm, out);
                    }
                }
                for (size_t ri = 0; ri < cc_result_fn_registry_count(); ri++) {
                    const char* t = cc_result_fn_registry_result_type_at(ri);
                    char arm[256];
                    if (!t || strncmp(t, "CCResult_", 9) != 0) continue;
                    if (CC_UW_ARM_EMIT_CHECK(&seen, t)) continue;
                    cc_emit_plan_format_result_arm(arm, sizeof(arm), t,
                                                   CC_RESULT_ARM_IS_ERR, 0, 0);
                    fputs(arm, out);
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
                    if (i < CC_EMIT_PLAN_MAX_DELAYED && result_delay.delayed[i]) continue;
                    if (!spec || !spec->mangled_ok[0] || !spec->mangled_err[0]) continue;
                    /* Void-result types have no `.u.value` field; skip their
                     * `__cc_uw_value` arm.  The `?>`/`!>` rewriter never
                     * reads the value on a void result, so this arm being
                     * absent is fine — `default:` still covers raw pointers
                     * and typed non-Result LHSs. */
                    if (strcmp(spec->ok_type, "void") == 0) continue;
                    char key[192];
                    char arm[256];
                    snprintf(key, sizeof(key), "CCResult_%s_%s", spec->mangled_ok, spec->mangled_err);
                    if (CC_UW_ARM_EMIT_CHECK(&seen, key)) continue;
                    cc_emit_plan_format_result_arm(arm, sizeof(arm), key, CC_RESULT_ARM_VALUE, 0, 0);
                    fputs(arm, out);
                }
                for (int si = 0; ; si++) {
                    const CCStdlibPredeclaredResult* p = cc_result_spec_lookup_stdlib_predeclared_by_index(si);
                    if (!p) break;
                    if (!p->concrete_name || !p->ok_type) continue;
                    if (strcmp(p->ok_type, "void") == 0) continue;
                    if (CC_UW_ARM_EMIT_CHECK(&seen, p->concrete_name)) continue;
                    {
                        char arm[256];
                        cc_emit_plan_format_result_arm(arm, sizeof(arm), p->concrete_name,
                                                       CC_RESULT_ARM_VALUE, 0, 0);
                        fputs(arm, out);
                    }
                }
                for (size_t ri = 0; ri < cc_result_fn_registry_count(); ri++) {
                    const char* t = cc_result_fn_registry_result_type_at(ri);
                    char arm[256];
                    if (!t || strncmp(t, "CCResult_", 9) != 0) continue;
                    /* Void-ok results have no `.u.value`; same skip as the
                     * spec-table loop above. */
                    if (strncmp(t, "CCResult_void_", 14) == 0) continue;
                    if (CC_UW_ARM_EMIT_CHECK(&seen, t)) continue;
                    cc_emit_plan_format_result_arm(arm, sizeof(arm), t,
                                                   CC_RESULT_ARM_VALUE, 0, 0);
                    fputs(arm, out);
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
                    if (i < CC_EMIT_PLAN_MAX_DELAYED && result_delay.delayed[i]) continue;
                    if (!spec || !spec->mangled_ok[0] || !spec->mangled_err[0]) continue;
                    char key[192];
                    char arm[256];
                    snprintf(key, sizeof(key), "CCResult_%s_%s", spec->mangled_ok, spec->mangled_err);
                    if (CC_UW_ARM_EMIT_CHECK(&seen, key)) continue;
                    cc_emit_plan_format_result_arm(arm, sizeof(arm), key, CC_RESULT_ARM_ERR, 0, 0);
                    fputs(arm, out);
                }
                for (int si = 0; ; si++) {
                    const CCStdlibPredeclaredResult* p = cc_result_spec_lookup_stdlib_predeclared_by_index(si);
                    if (!p) break;
                    if (!p->concrete_name) continue;
                    if (CC_UW_ARM_EMIT_CHECK(&seen, p->concrete_name)) continue;
                    {
                        char arm[256];
                        cc_emit_plan_format_result_arm(arm, sizeof(arm), p->concrete_name,
                                                       CC_RESULT_ARM_ERR, 0, 0);
                        fputs(arm, out);
                    }
                }
                for (size_t ri = 0; ri < cc_result_fn_registry_count(); ri++) {
                    const char* t = cc_result_fn_registry_result_type_at(ri);
                    char arm[256];
                    if (!t || strncmp(t, "CCResult_", 9) != 0) continue;
                    if (CC_UW_ARM_EMIT_CHECK(&seen, t)) continue;
                    /* Degrade to the CCError base view rather than the raw
                     * error struct: header error types wrap `CCError base
                     * @as` (CCIoError, CCPyError, ...), and a raw-typed
                     * binder makes the handler's `e.message` fail the
                     * parser-mode type-check.  Same reasoning as the
                     * `__CCResultGeneric` arm, whose error is CCError-
                     * layout "so the binder picks up usable fields". */
                    snprintf(arm, sizeof(arm),
                        "    %s: (*(CCError*)(void*)&(((%s*)(void*)&(__x__))->u.error)), \\\n",
                        t, t);
                    fputs(arm, out);
                }
                fprintf(out,
                    "    default: __cc_err_null_at(__e__, __f__, __l__))\n");
                #undef CC_UW_ARM_EMIT_CHECK

                fprintf(out, "/* --- end result type declarations --- */\n");
                cc_emit_plan_fprint_line_directive(out, use, insert_pos, input_path);
                    }
                    {
                        int emitted_delayed_container = 0;
                        for (size_t i = 0; i < n_vec_ctnr && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
                            if (!ctnr_sched.vec_delayed[i] || ctnr_sched.vec_pos[i] != next_pos) continue;
                            if (!emitted_delayed_container) {
                                fprintf(out, "/* --- CC delayed container declarations --- */\n");
                                emitted_delayed_container = 1;
                            }
                            cc_emit_plan_fprint_vec_decl(out, cc_type_graph_get_vec(graph, i));
                        }
                        for (size_t i = 0; i < n_map_ctnr && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
                            if (!ctnr_sched.map_delayed[i] || ctnr_sched.map_pos[i] != next_pos) continue;
                            if (!emitted_delayed_container) {
                                fprintf(out, "/* --- CC delayed container declarations --- */\n");
                                emitted_delayed_container = 1;
                            }
                            cc_emit_plan_fprint_map_decl(out, cc_type_graph_get_map(graph, i));
                        }
                        for (size_t i = 0; i < n_slice_ctnr && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
                            const char* nm = NULL;
                            const char* el = NULL;
                            size_t fu;
                            if (!ctnr_sched.slice_emit[i] || !ctnr_sched.slice_delayed[i] ||
                                ctnr_sched.slice_pos[i] != next_pos)
                                continue;
                            if (cc_slice_spec_get(i, &nm, &el) != 0) continue;
                            if (!emitted_delayed_container) {
                                fprintf(out, "/* --- CC delayed container declarations --- */\n");
                                emitted_delayed_container = 1;
                            }
                            fu = cc_emit_plan_find_ident_any(use, use_len, nm);
                            if (fu < use_len)
                                cc_emit_plan_fprint_line_directive(out, use, fu, input_path);
                            fprintf(out, "CC_DECL_SLICE_SPEC(%s, %s)\n", nm, el);
                        }
                        if (emitted_delayed_container) {
                            fprintf(out, "/* --- end delayed container declarations --- */\n");
                        }
                    }
                    {
                        int emitted_delayed_result = 0;
                        for (size_t i = 0; i < cc__result_specs.count && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
                            const CCResultSpec* spec = cc_result_spec_table_get(&cc__result_specs, i);
                            if (!result_delay.delayed[i] || result_delay.pos[i] != next_pos || !spec) continue;
                            if (!emitted_delayed_result) {
                                fprintf(out, "/* --- CC delayed result type declarations (after local typedefs) --- */\n");
                                emitted_delayed_result = 1;
                            }
                            if (cc_result_spec_is_stdlib_predeclared_name(spec->concrete_name)) continue;
                            int ok_is_void = (strcmp(spec->ok_type, "void") == 0);
                            fprintf(out, "#ifndef CCResult_%s_%s_DEFINED\n", spec->mangled_ok, spec->mangled_err);
                            fprintf(out, "#define CCResult_%s_%s_DEFINED 1\n", spec->mangled_ok, spec->mangled_err);
                            if (ok_is_void) {
                                fprintf(out, "CC_DECL_RESULT_SPEC_VOID(CCResult_%s_%s, %s)\n",
                                        spec->mangled_ok, spec->mangled_err, spec->err_type);
                            } else {
                                fprintf(out, "CC_DECL_RESULT_SPEC(CCResult_%s_%s, %s, %s)\n",
                                        spec->mangled_ok, spec->mangled_err,
                                        spec->ok_type, spec->err_type);
                            }
                            fprintf(out, "#endif\n");
                            fprintf(out, "bool __cc_parser_result_is_ok_CCResult_%s_%s(CCResult_%s_%s r);\n",
                                    spec->mangled_ok, spec->mangled_err,
                                    spec->mangled_ok, spec->mangled_err);
                            fprintf(out, "bool __cc_parser_result_is_err_CCResult_%s_%s(CCResult_%s_%s r);\n",
                                    spec->mangled_ok, spec->mangled_err,
                                    spec->mangled_ok, spec->mangled_err);
                            if (!ok_is_void) {
                                fprintf(out, "%s __cc_parser_result_unwrap_CCResult_%s_%s(CCResult_%s_%s r);\n",
                                        spec->ok_type, spec->mangled_ok, spec->mangled_err,
                                        spec->mangled_ok, spec->mangled_err);
                            }
                            fprintf(out, "%s __cc_parser_result_error_CCResult_%s_%s(CCResult_%s_%s r);\n",
                                    spec->err_type, spec->mangled_ok, spec->mangled_err,
                                    spec->mangled_ok, spec->mangled_err);
                            if (!ok_is_void) {
                                fprintf(out, "%s __cc_parser_result_unwrap_or_CCResult_%s_%s(CCResult_%s_%s r, %s def);\n",
                                        spec->ok_type, spec->mangled_ok, spec->mangled_err,
                                        spec->mangled_ok, spec->mangled_err, spec->ok_type);
                            }
                        }
                        if (emitted_delayed_result) {
                            fprintf(out, "/* --- end delayed result type declarations --- */\n");
                        }
                    }
                    {
                        int emitted_comptime = 0;
                        for (size_t i = 0; i < comptime_sched.n; i++) {
                            if (comptime_emitted[i] || comptime_sched.pos[i] != next_pos) continue;
                            if (!emitted_comptime) {
                                emitted_comptime = 1;
                            }
                            cc_emit_plan_fprint_comptime_fragment(out, comptime_sched.frag_index[i]);
                            comptime_emitted[i] = 1;
                        }
                    }
                    cc_emit_plan_fprint_line_directive(out, use, next_pos, input_path);
                    if (next_pos > g_cc_pp_splice_last_anchor)
                        g_cc_pp_splice_last_anchor = next_pos;
                    cursor = next_pos;
                }
            }
        }
        if (mode == CC_PP_MODE_EMIT_SPLICE_ONLY) {
            fflush(out);
            g_cc_pp_splice_delta = (long)out_size - (long)strlen(use);
        }
        free(lowered_system_use);
    }
    fclose(out);

    cc_pass_chain_free(&chain);
    if (mode != CC_PP_MODE_EMIT_SPLICE_ONLY) free(buf);
    return out_buf;

chain_cleanup:
    /* Error path - cleanup allocations and return failure */
    cc_pass_chain_free(&chain);
    if (mode != CC_PP_MODE_EMIT_SPLICE_ONLY) free(buf);
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
static int g_local_cch_lower_failed = 0;
/* TU extract of a quoted .cch must not wipe include/declare/result indexes
 * the caller already ingested. `lower_headers` batch still resets. */
static int g_header_lower_preserve_tu_state = 0;

static char** g_included_cch_sources = NULL;
static size_t g_included_cch_source_count = 0;
static size_t g_included_cch_source_cap = 0;

/* Path → file-text cache for the current TU. UFCS / family / sink probes
 * re-query included .cch headers hundreds to thousands of times per compile;
 * without this each probe re-opens and re-reads the same headers from disk. */
typedef struct {
    char* path;
    char* text;
    size_t len;
    char** callables; /* sorted unique `ident(` names in text */
    size_t n_callables;
    char** declares; /* sorted unique decl-shaped / `#define name(` names */
    size_t n_declares;
} CCPathTextCache;
static CCPathTextCache* g_path_text_cache = NULL;
static size_t g_path_text_cache_count = 0;
static size_t g_path_text_cache_cap = 0;

/* Union of declares indexes across registered included .cch sources. */
static CCNameSet g_incl_declares_union = {0};
static size_t g_incl_declares_union_for = 0;

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

static void cc__family_members_reset(void);

static void cc__path_text_cache_reset(void) {
    size_t i, j;
    for (i = 0; i < g_path_text_cache_count; i++) {
        free(g_path_text_cache[i].path);
        free(g_path_text_cache[i].text);
        for (j = 0; j < g_path_text_cache[i].n_callables; j++)
            free(g_path_text_cache[i].callables[j]);
        free(g_path_text_cache[i].callables);
        for (j = 0; j < g_path_text_cache[i].n_declares; j++)
            free(g_path_text_cache[i].declares[j]);
        free(g_path_text_cache[i].declares);
        g_path_text_cache[i].path = NULL;
        g_path_text_cache[i].text = NULL;
        g_path_text_cache[i].len = 0;
        g_path_text_cache[i].callables = NULL;
        g_path_text_cache[i].n_callables = 0;
        g_path_text_cache[i].declares = NULL;
        g_path_text_cache[i].n_declares = 0;
    }
    g_path_text_cache_count = 0;
    cc__name_set_free(&g_incl_declares_union);
    g_incl_declares_union_for = 0;
}

void cc_reset_included_cch_sources(void) {
    for (size_t i = 0; i < g_included_cch_source_count; i++)
        free(g_included_cch_sources[i]);
    g_included_cch_source_count = 0;
    cc__path_text_cache_reset();
    cc_ufcs_reset_dest_trap_dedup();
    cc__family_members_reset();
}

size_t cc_included_cch_source_count(void) { return g_included_cch_source_count; }

const char* cc_included_cch_source_path(size_t i) {
    if (i >= g_included_cch_source_count || !g_included_cch_sources) return NULL;
    return g_included_cch_sources[i];
}

static int cc__register_included_cch_source(const char* source_path) {
    char abs_src[PATH_MAX];
    char** nv;
    size_t cap;
    if (!source_path || !realpath(source_path, abs_src)) return -1;
    for (size_t i = 0; i < g_included_cch_source_count; i++) {
        if (strcmp(g_included_cch_sources[i], abs_src) == 0) return 0;
    }
    if (g_included_cch_source_count == g_included_cch_source_cap) {
        cap = g_included_cch_source_cap ? g_included_cch_source_cap * 2 : 16;
        nv = (char**)realloc(g_included_cch_sources, cap * sizeof(*nv));
        if (!nv) return -1;
        g_included_cch_sources = nv;
        g_included_cch_source_cap = cap;
    }
    g_included_cch_sources[g_included_cch_source_count] = strdup(abs_src);
    if (!g_included_cch_sources[g_included_cch_source_count]) return -1;
    g_included_cch_source_count++;
    return 1;
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

static int cc__callable_name_cmp(const void* a, const void* b) {
    const char* const* sa = (const char* const*)a;
    const char* const* sb = (const char* const*)b;
    return strcmp(*sa, *sb);
}

/* Index every `ident(` spelling in text (comment/string-blind — same
 * contract as cc_included_cch_contains_fn). Sorted for bsearch. */
static void cc__index_callables(CCPathTextCache* slot) {
    size_t i = 0;
    size_t cap = 0;
    if (!slot || !slot->text) return;
    slot->callables = NULL;
    slot->n_callables = 0;
    while (i < slot->len) {
        size_t s, e, q;
        if (!((slot->text[i] >= 'A' && slot->text[i] <= 'Z') ||
              (slot->text[i] >= 'a' && slot->text[i] <= 'z') ||
              slot->text[i] == '_')) {
            i++;
            continue;
        }
        if (i > 0 && cc_is_ident_char(slot->text[i - 1])) {
            while (i < slot->len && cc_is_ident_char(slot->text[i])) i++;
            continue;
        }
        s = i;
        while (i < slot->len && cc_is_ident_char(slot->text[i])) i++;
        e = i;
        q = e;
        while (q < slot->len && (slot->text[q] == ' ' || slot->text[q] == '\t'))
            q++;
        if (q >= slot->len || slot->text[q] != '(') continue;
        if (e > s && e - s < 192) {
            char* name = (char*)malloc(e - s + 1);
            char** nv;
            if (!name) continue;
            memcpy(name, slot->text + s, e - s);
            name[e - s] = 0;
            if (slot->n_callables == cap) {
                cap = cap ? cap * 2 : 64;
                nv = (char**)realloc(slot->callables, cap * sizeof(*nv));
                if (!nv) {
                    free(name);
                    continue;
                }
                slot->callables = nv;
            }
            slot->callables[slot->n_callables++] = name;
        }
    }
    if (slot->n_callables > 1) {
        size_t w = 0, r;
        qsort(slot->callables, slot->n_callables, sizeof(char*),
              cc__callable_name_cmp);
        for (r = 0; r < slot->n_callables; r++) {
            if (w > 0 && strcmp(slot->callables[w - 1], slot->callables[r]) == 0) {
                free(slot->callables[r]);
                continue;
            }
            slot->callables[w++] = slot->callables[r];
        }
        slot->n_callables = w;
    }
}

/* Decl-shaped twin of cc__index_callables (scanner-aware; matches
 * cc_included_cch_declares_fn). */
static void cc__index_declares(CCPathTextCache* slot) {
    size_t i = 0;
    CCScannerState scan;
    CCNameSet set = {0};
    if (!slot || !slot->text) return;
    slot->declares = NULL;
    slot->n_declares = 0;
    cc_scanner_init(&scan);
    while (i < slot->len) {
        size_t s, e, q, b;
        /* Function-like macros are real bindings, but the shared scanner
         * consumes directive lines whole — harvest `#define name(` here,
         * then let the scanner skip the line as usual.  The scan-state
         * guards keep a directive spelled inside a comment or string
         * from being harvested. */
        if (slot->text[i] == '#' && scan.at_line_start && !scan.in_pp &&
            !scan.in_line_comment && !scan.in_block_comment &&
            !scan.in_str && !scan.in_chr && !scan.in_tpl) {
            size_t noff = 0, nl = 0;
            int fnlike = 0;
            if (cc_scan_define_head(slot->text, slot->len, i, &noff, &nl, &fnlike) &&
                fnlike)
                (void)cc__name_set_push(&set, slot->text + noff, nl);
        }
        if (cc_scanner_skip_non_code(&scan, slot->text, slot->len, &i)) continue;
        if (!cc_is_ident_start(slot->text[i])) { i++; continue; }
        if (i > 0 && cc_is_ident_char(slot->text[i - 1])) {
            while (i < slot->len && cc_is_ident_char(slot->text[i])) i++;
            continue;
        }
        s = i;
        while (i < slot->len && cc_is_ident_char(slot->text[i])) i++;
        e = i;
        q = e;
        while (q < slot->len && (slot->text[q] == ' ' || slot->text[q] == '\t'))
            q++;
        if (q >= slot->len || slot->text[q] != '(') continue;
        b = cc_rskip_ws_and_comments(slot->text, s);
        if (b > 0 && cc__decl_prev_ok(slot->text, b))
            (void)cc__name_set_push(&set, slot->text + s, e - s);
    }
    cc__name_set_finalize(&set);
    slot->declares = set.names;
    slot->n_declares = set.n;
    /* Ownership moved into slot; don't free names via set. */
    set.names = NULL;
    set.n = set.cap = 0;
}

static int cc__cache_has_callable(const CCPathTextCache* slot, const char* name) {
    char* key = (char*)name;
    if (!slot || !slot->callables || !name || !name[0]) return 0;
    return bsearch(&key, slot->callables, slot->n_callables, sizeof(char*),
                   cc__callable_name_cmp) != NULL;
}

static int cc__cache_has_declare(const CCPathTextCache* slot, const char* name) {
    char* key = (char*)name;
    if (!slot || !slot->declares || !name || !name[0]) return 0;
    return bsearch(&key, slot->declares, slot->n_declares, sizeof(char*),
                   cc__callable_name_cmp) != NULL;
}

static CCPathTextCache* cc__path_text_cache_find(const char* path) {
    size_t i;
    if (!path) return NULL;
    for (i = 0; i < g_path_text_cache_count; i++) {
        if (g_path_text_cache[i].path &&
            strcmp(g_path_text_cache[i].path, path) == 0)
            return &g_path_text_cache[i];
    }
    return NULL;
}

static int cc__read_file_text_uncached(const char* path, char** out_buf, size_t* out_len) {
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

/* Borrowed text for `path` (valid until cc_reset_included_cch_sources).
 * Exact-path hit first — registered includes are already realpath'd, and
 * UFCS probes them thousands of times; paying realpath on every lookup
 * dominated the win from avoiding fopen. */
static const char* cc__path_text_cached(const char* path, size_t* out_len) {
    char abs_key[PATH_MAX];
    const char* key = path;
    size_t i;
    char* buf = NULL;
    size_t n = 0;
    CCPathTextCache* nv;
    if (!path || !path[0]) return NULL;
    for (i = 0; i < g_path_text_cache_count; i++) {
        if (g_path_text_cache[i].path &&
            strcmp(g_path_text_cache[i].path, path) == 0) {
            if (out_len) *out_len = g_path_text_cache[i].len;
            return g_path_text_cache[i].text;
        }
    }
    if (realpath(path, abs_key) && strcmp(abs_key, path) != 0) {
        key = abs_key;
        for (i = 0; i < g_path_text_cache_count; i++) {
            if (g_path_text_cache[i].path &&
                strcmp(g_path_text_cache[i].path, key) == 0) {
                if (out_len) *out_len = g_path_text_cache[i].len;
                return g_path_text_cache[i].text;
            }
        }
    }
    if (cc__read_file_text_uncached(path, &buf, &n) != 0 || !buf) return NULL;
    if (g_path_text_cache_count == g_path_text_cache_cap) {
        size_t cap = g_path_text_cache_cap ? g_path_text_cache_cap * 2 : 32;
        nv = (CCPathTextCache*)realloc(g_path_text_cache, cap * sizeof(*nv));
        if (!nv) {
            free(buf);
            return NULL;
        }
        g_path_text_cache = nv;
        g_path_text_cache_cap = cap;
    }
    g_path_text_cache[g_path_text_cache_count].path = strdup(key);
    g_path_text_cache[g_path_text_cache_count].text = buf;
    g_path_text_cache[g_path_text_cache_count].len = n;
    g_path_text_cache[g_path_text_cache_count].callables = NULL;
    g_path_text_cache[g_path_text_cache_count].n_callables = 0;
    g_path_text_cache[g_path_text_cache_count].declares = NULL;
    g_path_text_cache[g_path_text_cache_count].n_declares = 0;
    if (!g_path_text_cache[g_path_text_cache_count].path) {
        free(buf);
        return NULL;
    }
    cc__index_callables(&g_path_text_cache[g_path_text_cache_count]);
    cc__index_declares(&g_path_text_cache[g_path_text_cache_count]);
    g_path_text_cache_count++;
    /* New header text invalidates the union of declares indexes. */
    g_incl_declares_union_for = 0;
    if (out_len) *out_len = n;
    return buf;
}

static int cc__read_file_text(const char* path, char** out_buf, size_t* out_len) {
    const char* cached;
    size_t n = 0;
    char* copy;
    if (!path || !out_buf || !out_len) return -1;
    *out_buf = NULL;
    *out_len = 0;
    cached = cc__path_text_cached(path, &n);
    if (!cached) return -1;
    copy = (char*)malloc(n + 1);
    if (!copy) return -1;
    memcpy(copy, cached, n + 1);
    *out_buf = copy;
    *out_len = n;
    return 0;
}

/* Borrowed text for a registered included .cch (no free). */
static const char* cc__included_cch_text(size_t h, size_t* out_len) {
    if (h >= g_included_cch_source_count || !g_included_cch_sources[h]) return NULL;
    return cc__path_text_cached(g_included_cch_sources[h], out_len);
}

static void cc__register_included_cch_tree(const char* source_path);
static void cc__register_included_cch_imports(const char* source_path);
static int cc__included_cch_contains_fn_except(const char* name, const char* except_abs);

/* Register stdlib `.cch` trees for `#include <….[ch]|cch>` so `@as` metadata
 * is available when the TU already uses lowered `.h` includes (no `.cch→.h`
 * rewrite side-effect). */
static void cc__register_cch_trees_from_angle_includes(const char* src, size_t n) {
    size_t i = 0;
    if (!src || n == 0) return;
    while (i < n) {
        size_t line_end = i, p, close, path_s, path_e, rel_len;
        char rel[PATH_MAX], abs_src[PATH_MAX];
        while (line_end < n && src[line_end] != '\n') line_end++;
        p = i;
        while (p < line_end && (src[p] == ' ' || src[p] == '\t')) p++;
        if (p < line_end && src[p] == '#') {
            p++;
            while (p < line_end && (src[p] == ' ' || src[p] == '\t')) p++;
            if (p + 7 <= line_end && memcmp(src + p, "include", 7) == 0 &&
                (p + 7 == line_end || !cc_is_ident_char(src[p + 7]))) {
                p += 7;
                while (p < line_end && (src[p] == ' ' || src[p] == '\t')) p++;
                if (p < line_end && src[p] == '<') {
                    close = p + 1;
                    while (close < line_end && src[close] != '>') close++;
                    if (close < line_end) {
                        path_s = p + 1;
                        path_e = close;
                        rel_len = path_e - path_s;
                        if (rel_len >= 4 && rel_len < sizeof(rel)) {
                            int is_cch = memcmp(src + path_e - 4, ".cch", 4) == 0;
                            int is_h = !is_cch && memcmp(src + path_e - 2, ".h", 2) == 0;
                            if (is_cch || is_h) {
                                memcpy(rel, src + path_s, rel_len);
                                rel[rel_len] = '\0';
                                if (is_h) {
                                    /* foo.h → foo.cch */
                                    if (rel_len + 2 >= sizeof(rel)) goto next_line;
                                    memcpy(rel + rel_len - 2, ".cch", 4);
                                    rel[rel_len + 2] = '\0';
                                }
                                if (cc_path_resolve_system_cch(rel, abs_src,
                                                               sizeof(abs_src)))
                                    cc__register_included_cch_tree(abs_src);
                            }
                        }
                    }
                }
            }
        }
    next_line:
        i = line_end < n ? line_end + 1 : line_end;
    }
}

void cc_ingest_included_cch_struct_fields(CCTypeRegistry* reg) {
    size_t h;
    if (!reg) return;
    for (h = 0; h < g_included_cch_source_count; h++) {
        size_t fn = 0;
        const char* fsrc = cc__included_cch_text(h, &fn);
        if (!fsrc) continue;
        cc_type_registry_ingest_struct_fields(reg, fsrc, fn);
    }
}

int cc_included_cch_contains_fn(const char* name) {
    size_t h;
    if (!name || !name[0]) return 0;
    for (h = 0; h < g_included_cch_source_count; h++) {
        size_t fn = 0;
        CCPathTextCache* slot;
        if (!cc__included_cch_text(h, &fn)) continue;
        slot = cc__path_text_cache_find(g_included_cch_sources[h]);
        if (slot && cc__cache_has_callable(slot, name)) return 1;
    }
    return 0;
}

static int cc__included_cch_contains_fn_except(const char* name, const char* except_abs) {
    size_t h;
    if (!name || !name[0]) return 0;
    for (h = 0; h < g_included_cch_source_count; h++) {
        size_t fn = 0;
        CCPathTextCache* slot;
        if (except_abs && except_abs[0] && g_included_cch_sources[h] &&
            strcmp(g_included_cch_sources[h], except_abs) == 0)
            continue;
        if (!cc__included_cch_text(h, &fn)) continue;
        slot = cc__path_text_cache_find(g_included_cch_sources[h]);
        if (slot && cc__cache_has_declare(slot, name)) return 1;
    }
    return 0;
}

/* Append (comma-separated, deduped) the suffixes of decl-shaped
 * functions named `<prefix><suffix>` found in `text` — the installed
 * variants of a family, for diagnostics. Cold path. */
static void cc__append_family_suffixes(const char* text, size_t n,
                                       const char* prefix,
                                       char* out, size_t out_sz) {
    size_t plen = strlen(prefix);
    size_t i = 0;
    CCScannerState scan;
    if (!text || !plen) return;
    cc_scanner_init(&scan);
    while (i + plen < n) {
        size_t e, q, b;
        if (cc_scanner_skip_non_code(&scan, text, n, &i)) continue;
        if (text[i] != prefix[0]) { i++; continue; }
        if (i > 0 && cc_is_ident_char(text[i - 1])) { i++; continue; }
        if (i + plen > n || memcmp(text + i, prefix, plen) != 0) { i++; continue; }
        e = i + plen;
        if (e >= n || !cc_is_ident_char(text[e])) { i = e; continue; }
        while (e < n && cc_is_ident_char(text[e])) e++;
        q = e;
        while (q < n && (text[q] == ' ' || text[q] == '\t')) q++;
        if (q >= n || text[q] != '(') { i = e; continue; }
        b = cc_rskip_ws_and_comments(text, i);
        if (b == 0 || !(cc_is_ident_char(text[b - 1]) || text[b - 1] == '*')) {
            i = e;
            continue;
        }
        {
            char suf[96];
            size_t sl = e - (i + plen);
            size_t ol = strlen(out);
            if (sl > 0 && sl < sizeof(suf)) {
                char pat[100];
                memcpy(suf, text + i + plen, sl);
                suf[sl] = 0;
                snprintf(pat, sizeof(pat), ", %s,", suf);
                /* dedupe against ", suf," within the accumulated list */
                {
                    char hay[1024];
                    snprintf(hay, sizeof(hay), ", %s,", out);
                    if (!strstr(hay, pat)) {
                        if (ol + sl + 3 < out_sz) {
                            if (ol) {
                                out[ol++] = ',';
                                out[ol++] = ' ';
                            }
                            memcpy(out + ol, suf, sl);
                            out[ol + sl] = 0;
                        }
                    }
                }
            }
        }
        i = e;
    }
}

/* Family member sets derive from the declaration form itself: the
 * `##_<member>` tokens in a family macro's body ARE the member list
 * (`Name##_push`, `SNAKE##_sub`). Scans the included cch header whose
 * path ends with `header_suffix`, caches per header. Members are
 * text-invisible post-expansion, so dispatch trusts composed spellings
 * exactly for this derived set — and diagnostics can enumerate it. */
typedef struct {
    char suffix[64];
    char csv[1024];
    int loaded;
} CCFamilyMemberCache;
static _Thread_local CCFamilyMemberCache g_family_members[8];

static void cc__family_members_reset(void) {
    memset(g_family_members, 0, sizeof(g_family_members));
}

/* Open a family header for member-set / return-type scans. Tries the
 * registered include walk, then paths derived from it, then the repo
 * layout and CC_INCLUDE_PATH — so force-injected Result support
 * (`#include <ccc/cc_result.cch>` with no user `.cch` include) still
 * yields the derived member set. */
static int cc__family_header_open(const char* header_suffix,
                                  char** out_buf, size_t* out_len) {
    size_t h, pl, sl;
    char cand[PATH_MAX];
    if (!header_suffix || !header_suffix[0] || !out_buf || !out_len) return -1;
    *out_buf = NULL;
    *out_len = 0;
    sl = strlen(header_suffix);
    for (h = 0; h < g_included_cch_source_count; h++) {
        const char* path = g_included_cch_sources[h];
        if (!path) continue;
        pl = strlen(path);
        if (pl < sl || strcmp(path + pl - sl, header_suffix) != 0) continue;
        if (cc__read_file_text(path, out_buf, out_len) == 0 && *out_buf) return 0;
    }
    for (h = 0; h < g_included_cch_source_count; h++) {
        const char* path = g_included_cch_sources[h];
        const char* mark;
        if (!path) continue;
        mark = strstr(path, "/include/ccc/");
        if (!mark) continue;
        if ((size_t)snprintf(cand, sizeof(cand), "%.*s/include/ccc/%s",
                             (int)(mark - path), path, header_suffix) >=
            sizeof(cand))
            continue;
        if (cc__read_file_text(cand, out_buf, out_len) == 0 && *out_buf) return 0;
    }
    {
        char rel[PATH_MAX];
        if ((size_t)snprintf(rel, sizeof(rel), "ccc/%s", header_suffix) <
                sizeof(rel) &&
            cc_path_resolve_system_cch(rel, cand, sizeof(cand)) &&
            cc__read_file_text(cand, out_buf, out_len) == 0 && *out_buf)
            return 0;
    }
    return -1;
}

static void cc__family_scan_members(const char* fsrc, size_t fn,
                                    CCFamilyMemberCache* slot) {
    size_t i;
    for (i = 0; i + 3 < fn; i++) {
        if (fsrc[i] != '#' || fsrc[i + 1] != '#' || fsrc[i + 2] != '_')
            continue;
        {
            size_t ms = i + 3, me = ms;
            while (me < fn && cc_is_ident_char(fsrc[me])) me++;
            if (me > ms && me - ms < 96 && fsrc[ms] != '_') {
                char mem[96];
                char pat[100];
                char hay[1060];
                size_t ol = strlen(slot->csv);
                memcpy(mem, fsrc + ms, me - ms);
                mem[me - ms] = 0;
                snprintf(pat, sizeof(pat), ", %s,", mem);
                snprintf(hay, sizeof(hay), ", %s,", slot->csv);
                if (!strstr(hay, pat) && ol + (me - ms) + 3 < sizeof(slot->csv)) {
                    if (ol) {
                        slot->csv[ol++] = ',';
                        slot->csv[ol++] = ' ';
                    }
                    memcpy(slot->csv + ol, mem, me - ms + 1);
                }
            }
            i = me;
        }
    }
}

static const char* cc__family_members_csv(const char* header_suffix) {
    size_t ci;
    CCFamilyMemberCache* slot = NULL;
    char* fsrc = NULL;
    size_t fn = 0;
    for (ci = 0; ci < sizeof(g_family_members) / sizeof(g_family_members[0]); ci++) {
        if (g_family_members[ci].loaded &&
            strcmp(g_family_members[ci].suffix, header_suffix) == 0)
            return g_family_members[ci].csv;
        if (!slot && !g_family_members[ci].loaded) slot = &g_family_members[ci];
    }
    if (!slot) return "";
    snprintf(slot->suffix, sizeof(slot->suffix), "%s", header_suffix);
    slot->csv[0] = 0;
    slot->loaded = 1;
    if (cc__family_header_open(header_suffix, &fsrc, &fn) == 0 && fsrc) {
        cc__family_scan_members(fsrc, fn, slot);
        free(fsrc);
    }
    return slot->csv;
}

int cc_family_header_has_member(const char* header_suffix, const char* method) {
    const char* csv = cc__family_members_csv(header_suffix);
    char pat[110];
    char hay[1060];
    if (!csv[0] || !method || !method[0]) return 0;
    snprintf(pat, sizeof(pat), ", %s,", method);
    snprintf(hay, sizeof(hay), ", %s,", csv);
    return strstr(hay, pat) != NULL;
}

const char* cc_family_header_members(const char* header_suffix) {
    return cc__family_members_csv(header_suffix);
}

/* Builtin channel handle methods (not Name##_ in a family macro — they
 * lower via cc_ufcs_channel_callee). Keep in sync with ufcs.h. */
static int cc__ufcs_chan_handle_has_member(const char* base, const char* method) {
    int tx = (strncmp(base, "CCChanTx", 8) == 0);
    int rx = (strncmp(base, "CCChanRx", 8) == 0);
    int raw = (strcmp(base, "CCChan") == 0);
    if (!method || !method[0]) return 0;
    if (tx) {
        return strcmp(method, "send") == 0 || strcmp(method, "try_send") == 0 ||
               strcmp(method, "send_take") == 0 || strcmp(method, "send_task") == 0 ||
               strcmp(method, "send_task_hybrid") == 0 ||
               strcmp(method, "close") == 0 || strcmp(method, "free") == 0;
    }
    if (rx || raw) {
        return strcmp(method, "recv") == 0 || strcmp(method, "try_recv") == 0 ||
               strcmp(method, "close") == 0 || strcmp(method, "free") == 0;
    }
    return 0;
}

static const char* cc__ufcs_chan_handle_members(const char* base) {
    if (strncmp(base, "CCChanTx", 8) == 0)
        return "send, try_send, send_take, send_task, send_task_hybrid, close, free";
    if (strncmp(base, "CCChanRx", 8) == 0 || strcmp(base, "CCChan") == 0)
        return "recv, try_recv, close, free";
    return "";
}

const char* cc_ufcs_family_header_for(const char* base) {
    CCTypeRegistry* reg;
    size_t i, n;
    const char* suf;
    if (!base || !base[0]) return NULL;
    if (strncmp(base, "CCChanTx", 8) == 0 || strncmp(base, "CCChanRx", 8) == 0 ||
        strcmp(base, "CCChan") == 0)
        return NULL; /* handle allowlist, not a ##_ header */
    if (cc_slice_spec_lookup(base, NULL, NULL) == 0) return "cc_slice.cch";
    suf = cc_ufcs_family_header_suffix(base);
    if (suf) return suf;
    reg = cc_type_graph_active_registry(cc_type_graph_get_global());
    if (!reg) reg = cc_type_registry_get_global();
    if (!reg) return NULL;
    n = cc_type_registry_vec_count(reg);
    for (i = 0; i < n; i++) {
        const CCTypeInstantiation* t = cc_type_registry_get_vec(reg, i);
        if (t && t->mangled_name && strcmp(t->mangled_name, base) == 0)
            return "std/vec.cch";
    }
    n = cc_type_registry_map_count(reg);
    for (i = 0; i < n; i++) {
        const CCTypeInstantiation* t = cc_type_registry_get_map(reg, i);
        if (t && t->mangled_name && strcmp(t->mangled_name, base) == 0)
            return strncmp(base, "ArrayMap_", 9) == 0 ? "std/array_map.cch"
                                                      : "std/map_impl.cch";
    }
    n = cc_type_registry_channel_count(reg);
    for (i = 0; i < n; i++) {
        const CCTypeInstantiation* t = cc_type_registry_get_channel(reg, i);
        if (t && t->mangled_name && strcmp(t->mangled_name, base) == 0)
            return "cc_channel.cch";
    }
    return NULL;
}

int cc_ufcs_family_has_member(const char* base, const char* method) {
    const char* hdr;
    if (!base || !method || !method[0]) return 0;
    if (strncmp(base, "CCChanTx", 8) == 0 || strncmp(base, "CCChanRx", 8) == 0 ||
        strcmp(base, "CCChan") == 0)
        return cc__ufcs_chan_handle_has_member(base, method);
    if (cc_emit_plan_generic_instance_has_member(base, method)) return 1;
    hdr = cc_ufcs_family_header_for(base);
    if (!hdr) return 0;
    return cc_family_header_has_member(hdr, method);
}

const char* cc_ufcs_family_members_for(const char* base) {
    const char* hdr;
    if (!base || !base[0]) return "";
    if (strncmp(base, "CCChanTx", 8) == 0 || strncmp(base, "CCChanRx", 8) == 0 ||
        strcmp(base, "CCChan") == 0)
        return cc__ufcs_chan_handle_members(base);
    {
        const char* csv = cc_emit_plan_generic_instance_members_csv(base);
        if (csv && csv[0]) return csv;
    }
    hdr = cc_ufcs_family_header_for(base);
    if (!hdr) return "";
    return cc_family_header_members(hdr);
}


int cc_ufcs_generic_instance_known(const char* base) {
    return cc_emit_plan_generic_instance_known(base);
}

int cc_ufcs_family_accepts(const char* base, const char* method) {
    char ext[512];
    if (cc_ufcs_family_has_member(base, method)) return 1;
    if (!base || !method || !method[0]) return 0;
    if ((size_t)snprintf(ext, sizeof(ext), "%s_%s", base, method) >= sizeof(ext))
        return 0;
    return cc_included_cch_contains_fn(ext);
}

int cc_ufcs_family_accepts_in_tu(const char* base, const char* method,
                                 const char* src, size_t n) {
    char ext[512];
    if (cc_ufcs_family_accepts(base, method)) return 1;
    if (!base || !method || !src || n == 0) return 0;
    if ((size_t)snprintf(ext, sizeof(ext), "%s_%s", base, method) >= sizeof(ext))
        return 0;
    return cc__ufcs_fn_name_in_text(src, n, ext);
}

/* Read the family header's text (same roots as the member-set scan).
 * Caller frees. */
static int cc__family_header_read(const char* header_suffix,
                                  char** out_buf, size_t* out_len) {
    return cc__family_header_open(header_suffix, out_buf, out_len);
}

/* Raw return-type spelling of a family member: the token span before
 * `<Formal>##_<member>(` on its macro-body line, minus decl-spec
 * keywords. Still spells the macro's formals (T, Name, NAME, K, V,
 * SliceName); the caller substitutes the instance's actuals. */
static int cc_family_header_member_return(const char* header_suffix,
                                          const char* member,
                                          char* out, size_t out_sz) {
    char* fsrc = NULL;
    size_t fn = 0;
    size_t i;
    size_t mlen = strlen(member);
    int hit = 0;
    if (!member[0] || !out || out_sz == 0) return 0;
    out[0] = 0;
    if (cc__family_header_read(header_suffix, &fsrc, &fn) != 0) return 0;
    for (i = 0; i + 3 + mlen < fn && !hit; i++) {
        size_t formal_e, formal_s, line_s, q, ol;
        if (fsrc[i] != '#' || fsrc[i + 1] != '#' || fsrc[i + 2] != '_')
            continue;
        if (memcmp(fsrc + i + 3, member, mlen) != 0) continue;
        if (cc_is_ident_char(fsrc[i + 3 + mlen])) continue;
        q = i + 3 + mlen;
        while (q < fn && (fsrc[q] == ' ' || fsrc[q] == '\t')) q++;
        if (q >= fn || fsrc[q] != '(') continue;
        /* Formal instance-name token immediately before `##`. */
        formal_e = i;
        formal_s = formal_e;
        while (formal_s > 0 && cc_is_ident_char(fsrc[formal_s - 1])) formal_s--;
        if (formal_s == formal_e) continue;
        /* Return span: from this physical line's start to the formal. */
        line_s = formal_s;
        while (line_s > 0 && fsrc[line_s - 1] != '\n') line_s--;
        /* Copy tokens, skipping decl-spec keywords, normalizing ws. */
        ol = 0;
        q = line_s;
        while (q < formal_s && ol + 2 < out_sz) {
            char tok[64];
            size_t ts, tl;
            /* Comment-aware: a comment on the macro-body line must not
             * leak `*`/ident bytes into the harvested return type. */
            q = cc_skip_ws_and_comments(fsrc, formal_s, q);
            if (q >= formal_s) break;
            if (fsrc[q] == '*') {
                out[ol++] = '*';
                q++;
                continue;
            }
            if (!cc_is_ident_start(fsrc[q])) { q++; continue; }
            ts = q;
            while (q < formal_s && cc_is_ident_char(fsrc[q])) q++;
            tl = q - ts;
            if (tl >= sizeof(tok)) { ol = 0; break; }
            memcpy(tok, fsrc + ts, tl);
            tok[tl] = 0;
            if (strcmp(tok, "static") == 0 || strcmp(tok, "inline") == 0 ||
                strcmp(tok, "extern") == 0 || strcmp(tok, "const") == 0 ||
                strcmp(tok, "volatile") == 0)
                continue;
            if (ol && out[ol - 1] != '*' && ol + 1 < out_sz) out[ol++] = ' ';
            if (ol + tl + 1 >= out_sz) { ol = 0; break; }
            memcpy(out + ol, tok, tl);
            ol += tl;
        }
        out[ol] = 0;
        hit = ol > 0;
    }
    free(fsrc);
    return hit;
}

/* Whole-word formal → actual substitution over a type spelling. */
static void cc__subst_type_formals(char* buf, size_t sz,
                                   const char* const* formals,
                                   const char* const* actuals, int nf) {
    char tmp[256];
    size_t i = 0, o = 0;
    size_t n = strlen(buf);
    while (i < n && o + 1 < sizeof(tmp)) {
        if (cc_is_ident_start(buf[i]) && (i == 0 || !cc_is_ident_char(buf[i - 1]))) {
            size_t e = i;
            int k, done = 0;
            while (e < n && cc_is_ident_char(buf[e])) e++;
            for (k = 0; k < nf; k++) {
                size_t fl = strlen(formals[k]);
                if (e - i == fl && memcmp(buf + i, formals[k], fl) == 0) {
                    size_t al = strlen(actuals[k]);
                    if (o + al + 1 >= sizeof(tmp)) return;
                    memcpy(tmp + o, actuals[k], al);
                    o += al;
                    i = e;
                    done = 1;
                    break;
                }
            }
            if (done) continue;
            while (i < e && o + 1 < sizeof(tmp)) tmp[o++] = buf[i++];
            continue;
        }
        tmp[o++] = buf[i++];
    }
    tmp[o] = 0;
    snprintf(buf, sz, "%s", tmp);
}

/* Return-type spelling of the decl-shaped `name(` occurrence:
 * statement-head..name span minus decl-spec keywords, ws-normalized.
 * Same walk as cc__fn_returns_result_text. */
static int cc__decl_fn_return_type_text(const char* text, size_t n,
                                        const char* name,
                                        char* out, size_t out_sz) {
    size_t nlen = strlen(name);
    size_t i = 0;
    CCScannerState scan;
    if (!text || !nlen || !out || out_sz == 0) return 0;
    out[0] = 0;
    cc_scanner_init(&scan);
    while (i + nlen < n) {
        size_t q, a, b, ol;
        if (cc_scanner_skip_non_code(&scan, text, n, &i)) continue;
        if (text[i] != name[0]) { i++; continue; }
        if (i > 0 && cc_is_ident_char(text[i - 1])) { i++; continue; }
        if (i + nlen > n || memcmp(text + i, name, nlen) != 0) { i++; continue; }
        if (cc_is_ident_char(text[i + nlen])) { i += nlen; continue; }
        q = i + nlen;
        while (q < n && (text[q] == ' ' || text[q] == '\t')) q++;
        if (q >= n || text[q] != '(') { i += nlen; continue; }
        b = cc_rskip_ws_and_comments(text, i);
        if (b == 0 || !(cc_is_ident_char(text[b - 1]) || text[b - 1] == '*')) {
            i += nlen;
            continue;
        }
        a = b;
        while (a > 0 && !strchr(";{}()", text[a - 1]) && text[a - 1] != '\n') a--;
        ol = 0;
        q = a;
        while (q < b && ol + 2 < out_sz) {
            char tok[64];
            size_t ts, tl;
            /* Comment-aware: `static / *c* / int foo(` must not leak
             * comment bytes into the harvested return type. */
            q = cc_skip_ws_and_comments(text, b, q);
            if (q >= b) break;
            if (text[q] == '*') {
                out[ol++] = '*';
                q++;
                continue;
            }
            if (!cc_is_ident_start(text[q]) && text[q] != '@') { q++; continue; }
            if (text[q] == '@') {
                /* `@attr` decl attributes never join the type. */
                q++;
                while (q < b && cc_is_ident_char(text[q])) q++;
                continue;
            }
            ts = q;
            while (q < b && cc_is_ident_char(text[q])) q++;
            tl = q - ts;
            if (tl >= sizeof(tok)) { ol = 0; break; }
            memcpy(tok, text + ts, tl);
            tok[tl] = 0;
            if (strcmp(tok, "static") == 0 || strcmp(tok, "inline") == 0 ||
                strcmp(tok, "extern") == 0 || strcmp(tok, "const") == 0 ||
                strcmp(tok, "volatile") == 0 || strcmp(tok, "register") == 0 ||
                strcmp(tok, "_Thread_local") == 0)
                continue;
            if (ol && out[ol - 1] != '*' && ol + 1 < out_sz) out[ol++] = ' ';
            if (ol + tl + 1 >= out_sz) { ol = 0; break; }
            memcpy(out + ol, tok, tl);
            ol += tl;
        }
        out[ol] = 0;
        return ol > 0;
    }
    return 0;
}

/* Declared-function return type: the tcc-fed signature table first
 * (authoritative, sees system headers; populated once the parser-mode
 * parse has run), then the textual TU + included cch readers. */
static int cc__fn_return_type(const char* src, size_t n, const char* name,
                              char* out, size_t out_sz) {
    size_t h;
    if (cc_symsig_fn_return(name, out, out_sz)) return 1;
    if (src && cc__decl_fn_return_type_text(src, n, name, out, out_sz)) return 1;
    for (h = 0; h < g_included_cch_source_count; h++) {
        size_t fn = 0;
        const char* fsrc = cc__included_cch_text(h, &fn);
        if (!fsrc) continue;
        if (cc__decl_fn_return_type_text(fsrc, fn, name, out, out_sz)) return 1;
    }
    /* Generic-factory instance members live only in the emitted fragment. */
    {
        const char* d = cc_emit_plan_generic_instance_def_for_symbol(name);
        if (d && cc__decl_fn_return_type_text(d, strlen(d), name, out, out_sz))
            return 1;
    }
    return 0;
}

/* Family-instance bindings: header + formal/actual lists for a
 * container or typed-slice instance type. Returns 1 and fills the
 * arrays (capacity >= 3) on a known instance. */
static int cc__family_instance_bindings(const char* recv_type_base,
                                        const char** out_hdr,
                                        const char* formals[3],
                                        char actuals[3][160],
                                        int* out_nf) {
    const char* elem = NULL;
    CCTypeRegistry* reg = cc_type_registry_get_global();
    if (cc_slice_spec_lookup(recv_type_base, NULL, &elem) == 0 && elem) {
        *out_hdr = "cc_slice.cch";
        formals[0] = "NAME";
        snprintf(actuals[0], 160, "%s", recv_type_base);
        formals[1] = "T";
        snprintf(actuals[1], 160, "%s", elem);
        *out_nf = 2;
        return 1;
    }
    if (reg && strncmp(recv_type_base, "CCVec_", 6) == 0) {
        size_t i, c = cc_type_registry_vec_count(reg);
        for (i = 0; i < c; i++) {
            const CCTypeInstantiation* t = cc_type_registry_get_vec(reg, i);
            char sn[160];
            if (!t || !t->mangled_name || !t->type1) continue;
            if (strcmp(t->mangled_name, recv_type_base) != 0) continue;
            *out_hdr = "std/vec.cch";
            formals[0] = "Name";
            snprintf(actuals[0], 160, "%s", recv_type_base);
            formals[1] = "T";
            snprintf(actuals[1], 160, "%s", t->type1);
            formals[2] = "SliceName";
            if (cc_slice_spec_instance_for_elem(t->type1, sn, sizeof(sn)) == 0)
                snprintf(actuals[2], 160, "%s", sn);
            else
                snprintf(actuals[2], 160, "CCSlice");
            *out_nf = 3;
            return 1;
        }
    }
    if (reg && (strncmp(recv_type_base, "ArrayMap_", 9) == 0 ||
                strncmp(recv_type_base, "Map_", 4) == 0)) {
        size_t i, c = cc_type_registry_map_count(reg);
        for (i = 0; i < c; i++) {
            const CCTypeInstantiation* t = cc_type_registry_get_map(reg, i);
            if (!t || !t->mangled_name || !t->type1 || !t->type2) continue;
            if (strcmp(t->mangled_name, recv_type_base) != 0) continue;
            *out_hdr = strncmp(recv_type_base, "ArrayMap_", 9) == 0
                           ? "std/array_map.cch"
                           : "std/map_impl.cch";
            formals[0] = "Name";
            snprintf(actuals[0], 160, "%s", recv_type_base);
            formals[1] = "K";
            snprintf(actuals[1], 160, "%s", t->type1);
            formals[2] = "V";
            snprintf(actuals[2], 160, "%s", t->type2);
            *out_nf = 3;
            return 1;
        }
    }
    return 0;
}

/* Return type of `recv.method(...)`: family instances derive it from
 * the family header's macro body (formals substituted); otherwise the
 * composed callee's declaration is read textually from the TU or an
 * included cch. 1 on success. */
int cc__ufcs_method_return_type(const char* recv_type_base, const char* method,
                                const char* src, size_t n,
                                char* out, size_t out_sz) {
    const char* hdr = NULL;
    const char* formals[3];
    char actuals[3][160];
    int nf = 0;
    char cand[512];
    if (!recv_type_base || !recv_type_base[0] || !method || !method[0]) return 0;
    if (cc__family_instance_bindings(recv_type_base, &hdr, formals, actuals, &nf)) {
        const char* aptr[3];
        int k;
        if (!cc_family_header_has_member(hdr, method)) {
            /* Declared instance extension: read its return directly. */
            if ((size_t)snprintf(cand, sizeof(cand), "%s_%s", recv_type_base,
                                 method) < sizeof(cand) &&
                cc__fn_return_type(src, n, cand, out, out_sz))
                return 1;
            return 0;
        }
        /* Vec `as_slice` is declared by two variants (erased / typed);
         * mirror the emitters' choice, which SliceName already encodes. */
        if (strcmp(method, "as_slice") == 0 && nf == 3 &&
            strcmp(formals[2], "SliceName") == 0) {
            snprintf(out, out_sz, "%s", actuals[2]);
            return 1;
        }
        if (!cc_family_header_member_return(hdr, method, out, out_sz)) return 0;
        for (k = 0; k < nf; k++) aptr[k] = actuals[k];
        cc__subst_type_formals(out, out_sz, formals, aptr, nf);
        return out[0] != 0;
    }
    /* Declared extension spelling <Type>_<method>. */
    if ((size_t)snprintf(cand, sizeof(cand), "%s_%s", recv_type_base, method) <
            sizeof(cand) &&
        cc__fn_return_type(src, n, cand, out, out_sz))
        return 1;
    /* Scalar value-receiver family (cc_<mangled type>_<method>). */
    if (cc__compose_scalar_ufcs_callee(cand, sizeof(cand), recv_type_base,
                                       method) &&
        cc__fn_return_type(src, n, cand, out, out_sz))
        return 1;
    /* Default snake twin (CCListener.accept → cc_listener_accept). */
    if (cc_ufcs_compose_default_callee(cand, sizeof(cand), recv_type_base,
                                       method) &&
        cc__fn_return_type(src, n, cand, out, out_sz))
        return 1;
    return 0;
}

/* Return type of a call expression's callee `fname`: a composed family
 * member of a known instance (oracle above), or any declared function
 * (textual reader). */
static int cc__call_return_type(const char* fname, const char* src, size_t n,
                                char* out, size_t out_sz) {
    size_t fl = strlen(fname);
    size_t i;
    /* <instance>_<member> for a registered slice instance. */
    for (i = 0; i < cc_slice_spec_count(); i++) {
        const char* nm = NULL;
        size_t il;
        if (cc_slice_spec_get(i, &nm, NULL) != 0 || !nm) continue;
        il = strlen(nm);
        if (fl > il + 1 && strncmp(fname, nm, il) == 0 && fname[il] == '_' &&
            cc_ufcs_family_has_member(nm, fname + il + 1))
            return cc__ufcs_method_return_type(nm, fname + il + 1, src, n, out,
                                               out_sz);
    }
    {
        CCTypeRegistry* reg = cc_type_registry_get_global();
        if (reg) {
            size_t c = cc_type_registry_vec_count(reg);
            for (i = 0; i < c; i++) {
                const CCTypeInstantiation* t = cc_type_registry_get_vec(reg, i);
                size_t il;
                if (!t || !t->mangled_name) continue;
                il = strlen(t->mangled_name);
                if (fl > il + 1 && strncmp(fname, t->mangled_name, il) == 0 &&
                    fname[il] == '_' &&
                    cc_ufcs_family_has_member(t->mangled_name, fname + il + 1))
                    return cc__ufcs_method_return_type(t->mangled_name,
                                                       fname + il + 1, src, n,
                                                       out, out_sz);
            }
            c = cc_type_registry_map_count(reg);
            for (i = 0; i < c; i++) {
                const CCTypeInstantiation* t = cc_type_registry_get_map(reg, i);
                size_t il;
                if (!t || !t->mangled_name) continue;
                il = strlen(t->mangled_name);
                if (fl > il + 1 && strncmp(fname, t->mangled_name, il) == 0 &&
                    fname[il] == '_' &&
                    cc_ufcs_family_has_member(t->mangled_name, fname + il + 1))
                    return cc__ufcs_method_return_type(t->mangled_name,
                                                       fname + il + 1, src, n,
                                                       out, out_sz);
            }
        }
    }
    return cc__fn_return_type(src, n, fname, out, out_sz);
}

/* Map key hash/eq: the declared convention outranks the built-in
 * table — a key type K is installed when cc_map_key_hash_<mangled K>
 * and cc_map_key_eq_<mangled K> are both declared (noted from the TU
 * at canonicalize, or visible in an included cch header). Unknown key
 * types are an articulate error, never a silent i32 hash. */
static _Thread_local char g_map_key_notes[16][96];
static _Thread_local int g_map_key_note_static[16];
static _Thread_local int g_map_key_note_count;
static _Thread_local char g_map_key_errs[8][96];
static _Thread_local int g_map_key_err_count;

void cc_note_tu_map_key_pairs(const char* src, size_t n) {
    static const char pre[] = "cc_map_key_hash_";
    size_t i = 0;
    CCScannerState scan;
    if (!src) return;
    cc_scanner_init(&scan);
    while (i + sizeof(pre) - 1 < n) {
        size_t e, q, b;
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (src[i] != 'c') { i++; continue; }
        if (i > 0 && cc_is_ident_char(src[i - 1])) { i++; continue; }
        if (memcmp(src + i, pre, sizeof(pre) - 1) != 0) { i++; continue; }
        e = i + sizeof(pre) - 1;
        while (e < n && cc_is_ident_char(src[e])) e++;
        q = e;
        while (q < n && (src[q] == ' ' || src[q] == '\t')) q++;
        if (q >= n || src[q] != '(') { i = e; continue; }
        b = cc_rskip_ws_and_comments(src, i);
        if (b == 0 || !(cc_is_ident_char(src[b - 1]) || src[b - 1] == '*')) {
            i = e;
            continue;
        }
        {
            size_t sl = e - (i + sizeof(pre) - 1);
            char suf[96];
            char eqn[128];
            int k, dup = 0;
            if (sl == 0 || sl >= sizeof(suf)) { i = e; continue; }
            memcpy(suf, src + i + sizeof(pre) - 1, sl);
            suf[sl] = 0;
            snprintf(eqn, sizeof(eqn), "cc_map_key_eq_%s", suf);
            if (!cc__tu_declares_fn(src, n, eqn)) { i = e; continue; }
            for (k = 0; k < g_map_key_note_count && !dup; k++)
                if (strcmp(g_map_key_notes[k], suf) == 0) dup = 1;
            if (!dup && g_map_key_note_count < 16) {
                /* Staticness of the definition decides the forward
                 * prototype's linkage at the splice. */
                size_t a = b;
                int is_static = 0;
                while (a > 0 && !strchr(";{}", src[a - 1]) && src[a - 1] != '\n')
                    a--;
                {
                    size_t w = a;
                    while (w + 6 <= b) {
                        if (memcmp(src + w, "static", 6) == 0 &&
                            (w == a || !cc_is_ident_char(src[w - 1])) &&
                            !cc_is_ident_char(src[w + 6])) {
                            is_static = 1;
                            break;
                        }
                        w++;
                    }
                }
                g_map_key_note_static[g_map_key_note_count] = is_static;
                snprintf(g_map_key_notes[g_map_key_note_count++],
                         sizeof(g_map_key_notes[0]), "%s", suf);
            }
        }
        i = e;
    }
}

int cc_map_key_hasheq_ex(const char* key_type,
                         char* hash_out, size_t hs,
                         char* eq_out, size_t es,
                         int* out_tu_static) {
    static const struct { const char* pat; int substr;
                          const char* hash_fn; const char* eq_fn; } tbl[] = {
        { "int",           0, "cc_map_hash_i32",          "cc_map_eq_i32"          },
        { "CCSliceHdr",    0, "cc_map_hash_slice_hdr",    "cc_map_eq_slice_hdr"    },
        { "CCSlicePacked", 0, "cc_map_hash_slice_packed", "cc_map_eq_slice_packed" },
        { "charslice",     0, "cc_map_hash_slice",        "cc_map_eq_slice"        },
        { "64",            1, "cc_map_hash_u64",          "cc_map_eq_u64"          },
        { "slice",         1, "cc_map_hash_slice",        "cc_map_eq_slice"        },
        { "Slice",         1, "cc_map_hash_slice",        "cc_map_eq_slice"        },
    };
    char mangled[128];
    size_t t;
    int k;
    if (out_tu_static) *out_tu_static = -1;
    if (!key_type || !key_type[0]) return -1;
    cc__mangle_type_name(key_type, strlen(key_type), mangled, sizeof(mangled));
    if (mangled[0]) {
        char hname[160];
        char ename[160];
        int noted = 0;
        snprintf(hname, sizeof(hname), "cc_map_key_hash_%s", mangled);
        snprintf(ename, sizeof(ename), "cc_map_key_eq_%s", mangled);
        for (k = 0; k < g_map_key_note_count && !noted; k++)
            if (strcmp(g_map_key_notes[k], mangled) == 0) noted = k + 1;
        if (noted || (cc_included_cch_declares_fn(hname) &&
                      cc_included_cch_declares_fn(ename))) {
            snprintf(hash_out, hs, "%s", hname);
            snprintf(eq_out, es, "%s", ename);
            if (out_tu_static)
                *out_tu_static = noted ? g_map_key_note_static[noted - 1] : -1;
            return 0;
        }
    }
    for (t = 0; t < sizeof(tbl) / sizeof(tbl[0]); t++) {
        int hit = tbl[t].substr ? (strstr(key_type, tbl[t].pat) != NULL)
                                : (strcmp(key_type, tbl[t].pat) == 0);
        if (hit) {
            snprintf(hash_out, hs, "%s", tbl[t].hash_fn);
            snprintf(eq_out, es, "%s", tbl[t].eq_fn);
            return 0;
        }
    }
    {
        int dup = 0;
        for (k = 0; k < g_map_key_err_count && !dup; k++)
            if (strcmp(g_map_key_errs[k], key_type) == 0) dup = 1;
        if (!dup) {
            if (g_map_key_err_count < 8)
                snprintf(g_map_key_errs[g_map_key_err_count++],
                         sizeof(g_map_key_errs[0]), "%s", key_type);
            fprintf(stderr,
                    "cc: error: type: map key type '%s' has no installed "
                    "hash/eq\n", key_type);
            fprintf(stderr,
                    "cc: note: declare cc_map_key_hash_%s and "
                    "cc_map_key_eq_%s to install it\n",
                    mangled[0] ? mangled : "<mangled>",
                    mangled[0] ? mangled : "<mangled>");
            fprintf(stderr,
                    "cc: note: installed key kinds: int, int64/uint64, "
                    "CCSliceHdr, CCSlicePacked, slice family%s%s\n",
                    g_map_key_note_count ? "; declared: " : "",
                    g_map_key_note_count ? g_map_key_notes[0] : "");
            g_cc_pass_error_count++;
        }
    }
    snprintf(hash_out, hs, "cc_map_hash_i32");
    snprintf(eq_out, es, "cc_map_eq_i32");
    return 1;
}

int cc_map_key_hasheq(const char* key_type,
                      char* hash_out, size_t hs,
                      char* eq_out, size_t es) {
    return cc_map_key_hasheq_ex(key_type, hash_out, hs, eq_out, es, NULL);
}

/* Nonzero when the decl-shaped declaration of `name` (TU or included
 * cch) spells a `CCResult_` return: only such sinks make a scalar
 * destination provably ill-formed under plain lowering. */
static int cc__fn_returns_result_text(const char* text, size_t n,
                                      const char* name) {
    size_t nlen = strlen(name);
    size_t i = 0;
    CCScannerState scan;
    if (!text || !nlen) return 0;
    cc_scanner_init(&scan);
    while (i + nlen < n) {
        size_t q, a, b;
        if (cc_scanner_skip_non_code(&scan, text, n, &i)) continue;
        if (text[i] != name[0]) { i++; continue; }
        if (i > 0 && cc_is_ident_char(text[i - 1])) { i++; continue; }
        if (i + nlen > n || memcmp(text + i, name, nlen) != 0) { i++; continue; }
        if (cc_is_ident_char(text[i + nlen])) { i += nlen; continue; }
        q = i + nlen;
        while (q < n && (text[q] == ' ' || text[q] == '\t')) q++;
        if (q >= n || text[q] != '(') { i += nlen; continue; }
        b = cc_rskip_ws_and_comments(text, i);
        if (b == 0 || !(cc_is_ident_char(text[b - 1]) || text[b - 1] == '*')) {
            i += nlen;
            continue;
        }
        a = b;
        while (a > 0 && !strchr(";{}()", text[a - 1]) && text[a - 1] != '\n') a--;
        while (a + 9 <= b) {
            if (memcmp(text + a, "CCResult_", 9) == 0 &&
                (a == 0 || !cc_is_ident_char(text[a - 1])))
                return 1;
            a++;
        }
        return 0;
    }
    return 0;
}

static int cc__sink_returns_result(const char* src, size_t n, const char* name) {
    size_t h;
    if (src && cc__fn_returns_result_text(src, n, name)) return 1;
    for (h = 0; h < g_included_cch_source_count; h++) {
        size_t fn = 0;
        const char* fsrc = cc__included_cch_text(h, &fn);
        if (!fsrc) continue;
        if (cc__fn_returns_result_text(fsrc, fn, name)) return 1;
    }
    return 0;
}

/* Installed variants of `<prefix>` across the TU and included cch
 * headers, comma-separated into out. */
static void cc__enumerate_family_variants(const char* src, size_t n,
                                          const char* prefix,
                                          char* out, size_t out_sz) {
    size_t h;
    if (!out || out_sz == 0) return;
    out[0] = 0;
    if (src) cc__append_family_suffixes(src, n, prefix, out, out_sz);
    for (h = 0; h < g_included_cch_source_count; h++) {
        size_t fn = 0;
        const char* fsrc = cc__included_cch_text(h, &fn);
        if (!fsrc) continue;
        cc__append_family_suffixes(fsrc, fn, prefix, out, out_sz);
    }
}

/* Nonzero when `text` invokes CC_DECL_SLICE_SPEC with `name` as its
 * instance name, or CC_DECL_SLICE with the element token that pastes to
 * `name` — a hand-written instance the TU splice must not duplicate.
 * Comment/string-aware; the shared scanner treats directive lines as
 * non-code, so the macros' own #define lines never match. */
static int cc__text_has_slice_spec_decl(const char* text, size_t n,
                                        const char* name) {
    size_t i = 0;
    size_t name_len = strlen(name);
    CCScannerState scan;
    if (!text || !name_len) return 0;
    cc_scanner_init(&scan);
    while (i < n) {
        int is_spec = 0;
        size_t j, a, alen;
        if (cc_scanner_skip_non_code(&scan, text, n, &i)) continue;
        if (text[i] != 'C') { i++; continue; }
        if (i > 0 && cc_is_ident_char(text[i - 1])) { i++; continue; }
        if (i + 18 <= n && memcmp(text + i, "CC_DECL_SLICE_SPEC", 18) == 0 &&
            (i + 18 == n || !cc_is_ident_char(text[i + 18]))) {
            is_spec = 1;
            j = i + 18;
        } else if (i + 13 <= n && memcmp(text + i, "CC_DECL_SLICE", 13) == 0 &&
                   (i + 13 == n || !cc_is_ident_char(text[i + 13]))) {
            j = i + 13;
        } else {
            i++;
            continue;
        }
        while (j < n && (text[j] == ' ' || text[j] == '\t')) j++;
        if (j >= n || text[j] != '(') { i = j; continue; }
        j = cc_skip_ws_and_comments(text, n, j + 1);
        a = j;
        while (j < n && cc_is_ident_char(text[j])) j++;
        alen = j - a;
        if (is_spec) {
            if (alen == name_len && memcmp(text + a, name, alen) == 0) return 1;
        } else {
            if (name_len > 8 && strncmp(name, "CCSlice_", 8) == 0 &&
                alen == name_len - 8 && memcmp(text + a, name + 8, alen) == 0)
                return 1;
        }
        i = j;
    }
    return 0;
}

/* Nonzero when the TU must splice the declaration for slice instance
 * `name` (element `elem`): non-prebaked element, and no hand-written
 * CC_DECL_SLICE_SPEC/CC_DECL_SLICE for it in the TU or an included cch
 * header. */
int cc_slice_spec_tu_needs_decl(const char* src, size_t n,
                                const char* name, const char* elem) {
    size_t h;
    if (!name || !name[0]) return 0;
    if (cc_slice_spec_elem_is_prebaked(elem)) return 0;
    if (src && cc__text_has_slice_spec_decl(src, n, name)) return 0;
    for (h = 0; h < g_included_cch_source_count; h++) {
        size_t fn = 0;
        const char* fsrc = cc__included_cch_text(h, &fn);
        if (!fsrc) continue;
        if (cc__text_has_slice_spec_decl(fsrc, fn, name)) return 0;
    }
    return 1;
}

static void cc__ensure_incl_declares_union(void) {
    size_t h;
    if (g_incl_declares_union_for == g_included_cch_source_count) return;
    cc__name_set_free(&g_incl_declares_union);
    for (h = 0; h < g_included_cch_source_count; h++) {
        size_t j;
        CCPathTextCache* slot;
        (void)cc__included_cch_text(h, NULL);
        slot = cc__path_text_cache_find(g_included_cch_sources[h]);
        if (!slot || !slot->declares) continue;
        for (j = 0; j < slot->n_declares; j++)
            (void)cc__name_set_push(&g_incl_declares_union, slot->declares[j],
                                    strlen(slot->declares[j]));
    }
    cc__name_set_finalize(&g_incl_declares_union);
    g_incl_declares_union_for = g_included_cch_source_count;
}

/* Decl-shaped twin of cc_included_cch_contains_fn: comment/string-aware,
 * and a hit requires the occurrence's previous code char to be an
 * identifier char, `*`, `]` (`T[:]` / `char[:]` return sugar), or the
 * `)` of `!>(E)` (`T !>(E) name(`).
 * Doc-comment examples and `.method(` call spellings never match. Also
 * matches `#define name(` (a visible macro is a real binding). */
int cc_included_cch_declares_fn(const char* name) {
    if (!name || !name[0]) return 0;
    cc__ensure_incl_declares_union();
    return cc__name_set_has(&g_incl_declares_union, name);
}

int cc_lowered_local_declares_fn(const char* name) {
    size_t i;
    if (!name || !name[0]) return 0;
    for (i = 0; i < g_lowered_local_header_count; i++) {
        const char* path = g_lowered_local_headers[i].source_path;
        CCPathTextCache* slot;
        if (!path || !path[0]) continue;
        (void)cc__path_text_cached(path, NULL);
        slot = cc__path_text_cache_find(path);
        if (slot && cc__cache_has_declare(slot, name)) return 1;
    }
    return 0;
}

/* Drop a trailing parameter name (`char[:] bytes` → `char[:]`). A lone
 * type ident (`CCSlice`) is left intact. */
static void cc__param_drop_name(const char* src, size_t ps, size_t* pe) {
    size_t e, name_s;
    if (!src || !pe || *pe <= ps) return;
    e = *pe;
    while (e > ps && isspace((unsigned char)src[e - 1])) e--;
    if (e == ps || !cc_is_ident_char(src[e - 1])) return;
    while (e > ps && cc_is_ident_char(src[e - 1])) e--;
    name_s = e;
    while (e > ps && isspace((unsigned char)src[e - 1])) e--;
    if (e == ps) return;
    if (cc_is_ident_char(src[e - 1]) || src[e - 1] == '*' || src[e - 1] == ']' ||
        src[e - 1] == '>')
        *pe = name_s;
}

static int cc__norm_type_span(const char* src, size_t ps, size_t pe, char* out,
                              size_t out_sz) {
    size_t dn = 0;
    if (!src || !out || out_sz == 0) return 0;
    out[0] = 0;
    while (ps < pe && isspace((unsigned char)src[ps])) ps++;
    while (pe > ps && isspace((unsigned char)src[pe - 1])) pe--;
    while (ps < pe && dn + 1 < out_sz) {
        if (isspace((unsigned char)src[ps])) {
            if (dn > 0 && out[dn - 1] != ' ' && out[dn - 1] != '*')
                out[dn++] = ' ';
            ps++;
            continue;
        }
        if (src[ps] == '*' && dn > 0 && out[dn - 1] == ' ') dn--;
        if (cc_is_ident_char(src[ps]) && dn > 0 && out[dn - 1] == '*')
            out[dn++] = ' ';
        out[dn++] = src[ps++];
    }
    out[dn] = 0;
    return dn > 0;
}

static int cc__scan_fn_param_in_src(const char* fsrc, size_t fn, const char* name,
                                    size_t nlen, int argi, char* out,
                                    size_t out_sz) {
    size_t i = 0;
    CCScannerState scan;
    if (!fsrc || !name || !name[0] || argi < 0 || !out || out_sz == 0) return 0;
    cc_scanner_init(&scan);
    while (i + nlen < fn) {
        size_t q;
        if (cc_scanner_skip_non_code(&scan, fsrc, fn, &i)) continue;
        if (fsrc[i] != name[0]) { i++; continue; }
        if (i > 0 && cc_is_ident_char(fsrc[i - 1])) { i++; continue; }
        if (memcmp(fsrc + i, name, nlen) != 0) { i++; continue; }
        if (cc_is_ident_char(fsrc[i + nlen])) { i += nlen; continue; }
        q = i + nlen;
        while (q < fn && (fsrc[q] == ' ' || fsrc[q] == '\t')) q++;
        if (q < fn && fsrc[q] == '(') {
            size_t b = cc_rskip_ws_and_comments(fsrc, i);
            if (b > 0 && cc__decl_prev_ok(fsrc, b)) {
                size_t ps = q + 1;
                int depth = 0;
                int seen = 0;
                size_t pe = ps;
                while (pe < fn) {
                    char c = fsrc[pe];
                    if (c == '(') depth++;
                    else if (c == ')' && depth-- == 0) break;
                    else if (c == ',' && depth == 0) {
                        if (seen == argi) {
                            cc__param_drop_name(fsrc, ps, &pe);
                            return cc__norm_type_span(fsrc, ps, pe, out, out_sz);
                        }
                        seen++;
                        ps = pe + 1;
                    }
                    pe++;
                }
                if (seen == argi && pe > ps) {
                    cc__param_drop_name(fsrc, ps, &pe);
                    return cc__norm_type_span(fsrc, ps, pe, out, out_sz);
                }
                return 0;
            }
        }
        i += nlen;
    }
    return 0;
}

/* Parameter `argi` type for a decl-shaped `name(` in an included or
 * lowered-local `.cch`, whitespace-normalized. */
int cc_included_cch_fn_param(const char* name, int argi, char* out,
                             size_t out_sz) {
    size_t h, nlen;
    if (!name || !name[0] || argi < 0 || !out || out_sz == 0) return 0;
    out[0] = 0;
    nlen = strlen(name);
    for (h = 0; h < g_included_cch_source_count; h++) {
        size_t fn = 0;
        const char* fsrc = cc__included_cch_text(h, &fn);
        if (fsrc && cc__scan_fn_param_in_src(fsrc, fn, name, nlen, argi, out,
                                            out_sz))
            return 1;
    }
    for (h = 0; h < g_lowered_local_header_count; h++) {
        const char* path = g_lowered_local_headers[h].source_path;
        size_t fn = 0;
        const char* fsrc;
        if (!path || !path[0]) continue;
        fsrc = cc__path_text_cached(path, &fn);
        if (fsrc && cc__scan_fn_param_in_src(fsrc, fn, name, nlen, argi, out,
                                            out_sz))
            return 1;
    }
    return 0;
}

int cc_included_cch_fn_first_param(const char* name, char* out, size_t out_sz) {
    return cc_included_cch_fn_param(name, 0, out, out_sz);
}

/* Same decl-shaped `name(` rule as cc__scan_fn_param_in_src, but every
 * parameter of every first-seen decl — one pass per buffer. */
static int cc__each_fn_param_in_src(const char* fsrc, size_t fn,
                                    CCIncludedCchFnParamCb cb, void* ctx) {
    size_t i = 0;
    CCScannerState scan;
    if (!fsrc || !cb) return 0;
    cc_scanner_init(&scan);
    while (i < fn) {
        size_t s, e, q, b;
        if (cc_scanner_skip_non_code(&scan, fsrc, fn, &i)) continue;
        if (i >= fn) break;
        if (!cc_is_ident_start(fsrc[i])) {
            i++;
            continue;
        }
        if (i > 0 && cc_is_ident_char(fsrc[i - 1])) {
            while (i < fn && cc_is_ident_char(fsrc[i])) i++;
            continue;
        }
        s = i;
        while (i < fn && cc_is_ident_char(fsrc[i])) i++;
        e = i;
        q = e;
        while (q < fn && (fsrc[q] == ' ' || fsrc[q] == '\t')) q++;
        if (q >= fn || fsrc[q] != '(') continue;
        b = cc_rskip_ws_and_comments(fsrc, s);
        if (!(b > 0 && cc__decl_prev_ok(fsrc, b))) continue;
        {
            char name[192];
            char ty[160];
            size_t nlen = e - s;
            size_t ps = q + 1;
            int depth = 0;
            int argi = 0;
            size_t pe = ps;
            if (nlen == 0 || nlen >= sizeof(name)) continue;
            memcpy(name, fsrc + s, nlen);
            name[nlen] = 0;
            while (pe < fn) {
                char c = fsrc[pe];
                if (c == '(') {
                    depth++;
                } else if (c == ')' && depth-- == 0) {
                    if (pe > ps) {
                        size_t end = pe;
                        cc__param_drop_name(fsrc, ps, &end);
                        if (cc__norm_type_span(fsrc, ps, end, ty, sizeof(ty)) &&
                            ty[0] && strcmp(ty, "void") != 0) {
                            if (cb(name, argi, ty, ctx) != 0) return -1;
                        }
                    }
                    break;
                } else if (c == ',' && depth == 0) {
                    size_t end = pe;
                    cc__param_drop_name(fsrc, ps, &end);
                    if (cc__norm_type_span(fsrc, ps, end, ty, sizeof(ty)) &&
                        ty[0]) {
                        if (cb(name, argi, ty, ctx) != 0) return -1;
                    }
                    argi++;
                    ps = pe + 1;
                }
                pe++;
            }
        }
    }
    return 0;
}

int cc_included_cch_each_fn_param(CCIncludedCchFnParamCb cb, void* ctx) {
    size_t h;
    if (!cb) return 0;
    for (h = 0; h < g_included_cch_source_count; h++) {
        size_t fn = 0;
        const char* fsrc = cc__included_cch_text(h, &fn);
        if (fsrc && cc__each_fn_param_in_src(fsrc, fn, cb, ctx) != 0)
            return -1;
    }
    for (h = 0; h < g_lowered_local_header_count; h++) {
        const char* path = g_lowered_local_headers[h].source_path;
        size_t fn = 0;
        const char* fsrc;
        if (!path || !path[0]) continue;
        fsrc = cc__path_text_cached(path, &fn);
        if (fsrc && cc__each_fn_param_in_src(fsrc, fn, cb, ctx) != 0)
            return -1;
    }
    return 0;
}

static void cc__register_included_cch_tree(const char* source_path) {
    char abs_src[PATH_MAX];
    char source_dir[PATH_MAX];
    char* src = NULL;
    size_t n = 0, i = 0;
    int added;
    if (!source_path || !realpath(source_path, abs_src)) return;
    added = cc__register_included_cch_source(abs_src);
    if (added <= 0) return;
    if (cc__dirname_local(abs_src, source_dir, sizeof(source_dir)) != 0) return;
    if (cc__read_file_text(abs_src, &src, &n) != 0) return;

    /* Register Result-returning callees declared in this header so
     * expression-position `!>;` can resolve their concrete CCResult_T_E
     * without include-expanding the TU buffer. */
    cc_result_fn_registry_scan_source(src, n);

    while (i < n) {
        size_t line_end = i, p, path_s, path_e;
        char open = 0, close = 0;
        while (line_end < n && src[line_end] != '\n') line_end++;
        p = i;
        while (p < line_end && (src[p] == ' ' || src[p] == '\t')) p++;
        if (p < line_end && src[p++] == '#') {
            while (p < line_end && (src[p] == ' ' || src[p] == '\t')) p++;
            if (p + 7 <= line_end && memcmp(src + p, "include", 7) == 0 &&
                (p + 7 == line_end || !cc_is_ident_char(src[p + 7]))) {
                p += 7;
                while (p < line_end && (src[p] == ' ' || src[p] == '\t')) p++;
                if (p < line_end && (src[p] == '"' || src[p] == '<')) {
                    open = src[p++];
                    close = open == '"' ? '"' : '>';
                    path_s = p;
                    while (p < line_end && src[p] != close) p++;
                    path_e = p;
                    if (path_e > path_s + 4 &&
                        memcmp(src + path_e - 4, ".cch", 4) == 0) {
                        char rel[PATH_MAX], child[PATH_MAX];
                        size_t rel_len = path_e - path_s;
                        if (rel_len < sizeof(rel)) {
                            memcpy(rel, src + path_s, rel_len);
                            rel[rel_len] = '\0';
                            if (open == '"') {
                                snprintf(child, sizeof(child), "%s/%s", source_dir, rel);
                            } else if (cc_path_resolve_system_cch(rel, child,
                                                                  sizeof(child))) {
                                /* child filled */
                            } else {
                                child[0] = '\0';
                            }
                            if (child[0]) cc__register_included_cch_tree(child);
                        }
                    }
                }
            }
        }
        i = line_end < n ? line_end + 1 : line_end;
    }
    free(src);
}

/* Register `#include`d `.cch` trees without adding `source_path` itself.
 * Header lowering uses this so same-file wrappers are not "included callees". */
static void cc__register_included_cch_imports(const char* source_path) {
    char abs_src[PATH_MAX];
    char source_dir[PATH_MAX];
    char* src = NULL;
    size_t n = 0, i = 0;
    if (!source_path || !realpath(source_path, abs_src)) return;
    if (cc__dirname_local(abs_src, source_dir, sizeof(source_dir)) != 0) return;
    if (cc__read_file_text(abs_src, &src, &n) != 0) return;
    while (i < n) {
        size_t line_end = i, p, path_s, path_e;
        char open = 0, close = 0;
        while (line_end < n && src[line_end] != '\n') line_end++;
        p = i;
        while (p < line_end && (src[p] == ' ' || src[p] == '\t')) p++;
        if (p < line_end && src[p++] == '#') {
            while (p < line_end && (src[p] == ' ' || src[p] == '\t')) p++;
            if (p + 7 <= line_end && memcmp(src + p, "include", 7) == 0 &&
                (p + 7 == line_end || !cc_is_ident_char(src[p + 7]))) {
                p += 7;
                while (p < line_end && (src[p] == ' ' || src[p] == '\t')) p++;
                if (p < line_end && (src[p] == '"' || src[p] == '<')) {
                    open = src[p++];
                    close = open == '"' ? '"' : '>';
                    path_s = p;
                    while (p < line_end && src[p] != close) p++;
                    path_e = p;
                    if (path_e > path_s + 4 &&
                        memcmp(src + path_e - 4, ".cch", 4) == 0) {
                        char rel[PATH_MAX], child[PATH_MAX];
                        size_t rel_len = path_e - path_s;
                        if (rel_len < sizeof(rel)) {
                            memcpy(rel, src + path_s, rel_len);
                            rel[rel_len] = '\0';
                            if (open == '"') {
                                snprintf(child, sizeof(child), "%s/%s", source_dir, rel);
                            } else if (cc_path_resolve_system_cch(rel, child,
                                                                  sizeof(child))) {
                                /* child filled */
                            } else {
                                child[0] = '\0';
                            }
                            if (child[0]) cc__register_included_cch_tree(child);
                        }
                    }
                }
            }
        }
        i = line_end < n ? line_end + 1 : line_end;
    }
    free(src);
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

/* Where a header outside any repo lowers to.  Keyed by a hash of its absolute
 * path so the location is stable across runs (the incremental cache keys on
 * it) and two same-named headers in different directories cannot collide. */
static int cc__lowered_header_tmp_path(const char* abs_src, const char* base,
                                       char* out_path, size_t out_path_sz) {
    const char* tmp = getenv("TMPDIR");
    unsigned long long h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; abs_src[i]; i++) {
        h ^= (unsigned char)abs_src[i];
        h *= 1099511628211ULL;
    }
    if (!tmp || !tmp[0]) tmp = "/tmp";
    return snprintf(out_path, out_path_sz, "%s/cc-lowered-%ld/%016llx/%s",
                    tmp, (long)getuid(), h, base) >= (int)out_path_sz ? -1 : 0;
}

static int cc__build_stable_lowered_header_path(const char* abs_src,
                                                char* out_path,
                                                size_t out_path_sz) {
    char repo_root[PATH_MAX];
    size_t repo_len;
    const char* rel = NULL;
    size_t rel_len;
    if (!abs_src || !out_path || out_path_sz == 0) return -1;
    rel_len = strlen(abs_src);
    if (rel_len < 4 || strcmp(abs_src + rel_len - 4, ".cch") != 0) return -1;
    repo_root[0] = '\0';
    /* No repo root: lower to a temp directory rather than declining.  Declining
     * left the include pointing at the `.cch`, so CPP inlined raw CC syntax and
     * `!>` reached the parser unlowered, reporting something unrelated in a
     * file that looked fine — a silent no-op that has cost an afternoon twice. */
    repo_len = 0;
    if (cc_path_find_repo_root(abs_src, repo_root, sizeof(repo_root)))
        repo_len = strlen(repo_root);
    /* A root that does not contain this header is not this header's root: the
     * lookup falls back to the working directory's repo, and mapping the file
     * into an unrelated `out/include` is not possible. */
    if (repo_len == 0 || strncmp(abs_src, repo_root, repo_len) != 0) {
        const char* base = strrchr(abs_src, '/');
        base = base ? base + 1 : abs_src;
        if (cc__lowered_header_tmp_path(abs_src, base, out_path, out_path_sz) != 0)
            return -1;
        strcpy(out_path + strlen(out_path) - 4, ".h");
        return 0;
    }
    rel = abs_src + repo_len;
    if (*rel == '/') rel++;
    if (!*rel) return -1;
    rel_len = strlen(rel);
    if (snprintf(out_path, out_path_sz, "%s/out/include/%s", repo_root, rel) >= (int)out_path_sz) {
        return -1;
    }
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

/* ---------------------------------------------------------------------------
 * Implementation-grade local .cch headers.
 *
 * The eager .cch -> .h lowering above is interface-grade: it can rewrite type
 * syntax (`T !>(E)`, `char[:]`, containers) but cannot lower statement-level
 * CC constructs (`@errhandler`, `@defer`, `!>` unwrap statements, `@string`
 * templates), `@variant` declarations/uses, or anything that needs the TU's
 * comptime-executed type registry (packed variant arm sizes/niches).
 *
 * A local header that carries such constructs is therefore spliced RAW into
 * the including TU's text at the include site — before comptime preparation
 * and cc_preprocess_canonicalize — bracketed by `#line` provenance in and
 * out, so the full TU pipeline processes it exactly as if the user had
 * pasted it (C header-library semantics: each including TU compiles its own
 * static copy; the header's own include guard keeps repeat inclusion inert).
 * No lowered .h is written for these headers; the include line itself is
 * consumed by the splice. Plain interface headers keep the fast path above.
 *
 * Splice is only for a `.ccs` (or an already-spliced impl face) that
 * writes `#include "foo.cch"`. An interface umbrella extracts; nested
 * includes become `#include "foo.h"`. Dumping impl/UFCS descendants into
 * the umbrella's includer is how a TUI TU hits AST_CAP from one
 * `document.cch`. Impl-grade nested faces need a sibling `.ccs`, or a
 * direct include from a `.ccs`. Method-call UFCS does not force a splice
 * when the include is written in a `.ccs`; it still splices when nested
 * inside an already-spliced impl face (redis_db → redis_mem).
 */

#define CC_IMPL_CCH_BEGIN_MARK "/*cc:impl_cch_begin:"
#define CC_IMPL_CCH_END_MARK "/*cc:impl_cch_end:"

/* Skip an interface-grade `@typeview` / `@typehooks` form (stdlib lowering
 * strips these from the `.h` and recovers the facts from the original `.cch`).
 * Returns the index after the form, or 0 if this `@` is not one of those. */
static size_t cc__skip_type_policy_at(const char* src, size_t n, size_t i) {
    int is_tv, is_th;
    size_t p, body_r = 0;
    if (!src || i >= n || src[i] != '@') return 0;
    is_tv = cc_match_ident_kw(src, n, i + 1, "typeview");
    is_th = cc_match_ident_kw(src, n, i + 1, "typehooks");
    if (!is_tv && !is_th) return 0;
    p = cc_skip_ws_and_comments(src, n, i + 1 + (is_tv ? 8 : 9));
    if (p < n && src[p] == '(') {
        size_t rp = 0;
        if (!cc_find_matching_paren(src, n, p, &rp)) return i + 1;
        p = cc_skip_ws_and_comments(src, n, rp + 1);
    }
    if (p < n && cc_is_ident_start(src[p]) && !cc_match_ident_kw(src, n, p, "on")) {
        while (p < n && cc_is_ident_char(src[p])) p++;
        p = cc_skip_ws_and_comments(src, n, p);
    }
    if (cc_match_ident_kw(src, n, p, "on")) {
        p = cc_skip_ws_and_comments(src, n, p + 2);
        while (p < n && (cc_is_ident_char(src[p]) || src[p] == '*')) p++;
        p = cc_skip_ws_and_comments(src, n, p);
    }
    if (p < n && src[p] == '{' && cc_find_matching_brace(src, n, p, &body_r)) {
        p = cc_skip_ws_and_comments(src, n, body_r + 1);
        if (p < n && src[p] == ';') p++;
        return p;
    }
    return p > i ? p : i + 1;
}

/* `!>` statement unwrap (`!>;`, `!> {`, `!>(e) {`) vs result-type `T !>(E)`.
 * Both spell `!>(` — the type form is followed by a declarator, the
 * statement form by `{` or `;`. */
static int cc__bang_unwrap_is_stmt(const char* src, size_t n, size_t i) {
    size_t j, rp = 0;
    if (!src || i + 1 >= n || src[i] != '!' || src[i + 1] != '>') return 0;
    j = i + 2;
    while (j < n && (src[j] == ' ' || src[j] == '\t' ||
                     src[j] == '\n' || src[j] == '\r'))
        j++;
    if (j >= n || src[j] != '(') return 1;
    if (!cc_find_matching_paren(src, n, j, &rp)) return 1;
    j = cc_skip_ws_and_comments(src, n, rp + 1);
    return (j < n && (src[j] == '{' || src[j] == ';'));
}

/* True when header text contains constructs only the full TU pipeline can
 * lower.  Comment/string aware.  `@comptime` blocks/functions,
 * `@typeview` / `@typehooks`, and CC_GENERIC_FACTORY bodies are skipped
 * (the interface pipeline strips and harvests those the way stdlib
 * headers do); `T !>(E)` result-type syntax is allowed. */
static int cc__cch_text_is_impl_grade(const char* src, size_t n) {
    static const char fac_kw[] = "CC_GENERIC_FACTORY";
    static const char fac_kw_ext[] = "CC_GENERIC_FACTORY_EXTEND";
    const size_t fac_len = sizeof(fac_kw) - 1;
    const size_t fac_len_ext = sizeof(fac_kw_ext) - 1;
    size_t i = 0;
    CCScannerState scan;
    if (!src || n == 0) return 0;
    cc_scanner_init(&scan);
    while (i < n) {
        char c, c2;
        /* Directive-aware: an @-sigil inside a #define body does not by
         * itself make a header impl-grade (the macro expands into the
         * user TU, which is where it gets lowered). */
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        c = src[i];
        c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (c == 'C') {
            size_t mlen = 0;
            if (i + fac_len_ext <= n && memcmp(src + i, fac_kw_ext, fac_len_ext) == 0 &&
                (i == 0 || !cc_is_ident_char(src[i - 1])) &&
                (i + fac_len_ext >= n || !cc_is_ident_char(src[i + fac_len_ext])))
                mlen = fac_len_ext;
            else if (i + fac_len <= n && memcmp(src + i, fac_kw, fac_len) == 0 &&
                     (i == 0 || !cc_is_ident_char(src[i - 1])) &&
                     (i + fac_len >= n || !cc_is_ident_char(src[i + fac_len])))
                mlen = fac_len;
            if (mlen) {
                size_t p = cc_skip_ws_and_comments(src, n, i + mlen);
                size_t rp = 0, body_r = 0;
                if (p < n && src[p] == '(' && cc_find_matching_paren(src, n, p, &rp)) {
                    p = cc_skip_ws_and_comments(src, n, rp + 1);
                    if (p < n && src[p] == '{' && cc_find_matching_brace(src, n, p, &body_r)) {
                        i = body_r + 1;
                        continue;
                    }
                }
                i += mlen;
                continue;
            }
        }
        if (c == '@' && i + 1 < n && cc_is_ident_start(src[i + 1])) {
            {
                size_t after = cc__skip_type_policy_at(src, n, i);
                if (after) {
                    i = after;
                    continue;
                }
            }
            if (cc_match_ident_kw(src, n, i + 1, "comptime")) {
                size_t p = cc_skip_ws_and_comments(src, n, i + 1 + (sizeof("comptime") - 1));
                size_t body_r = 0;
                if (p < n && (cc_match_ident_kw(src, n, p, "if") ||
                              cc_match_ident_kw(src, n, p, "for")))
                    return 1;
                if (p < n && src[p] == '{' && cc_find_matching_brace(src, n, p, &body_r)) {
                    i = body_r + 1;
                    continue;
                }
                /* @comptime function definition: skip signature + body. */
                {
                    size_t lp = 0, rp = 0;
                    for (size_t q = p; q < n; q++) {
                        if (src[q] == ';' || src[q] == '{') break;
                        if (src[q] == '(') { lp = q; break; }
                    }
                    if (lp && cc_find_matching_paren(src, n, lp, &rp)) {
                        size_t b = cc_skip_ws_and_comments(src, n, rp + 1);
                        if (b < n && src[b] == '{' && cc_find_matching_brace(src, n, b, &body_r)) {
                            i = body_r + 1;
                            continue;
                        }
                    }
                }
                i++;
                continue;
            }
            return 1;
        }
        if (c == '?' && c2 == '>') return 1;
        if (c == '!' && c2 == '>') {
            if (cc__bang_unwrap_is_stmt(src, n, i)) return 1;
            i += 2; /* `T !>(E)` result-type syntax: interface pipeline handles it */
            continue;
        }
        i++;
    }
    return 0;
}

/* Per-process memo of .cch grade, keyed by realpath.  grade: 1 impl-grade,
 * 0 interface, -1 classification in progress (include cycle break). */
typedef struct {
    char* path;
    int grade;
} CCCchGradeMemo;

static CCCchGradeMemo* g_cch_grade_memo = NULL;
static size_t g_cch_grade_memo_count = 0;
static size_t g_cch_grade_memo_cap = 0;

static CCCchGradeMemo* cc__cch_grade_memo_find(const char* abs_src) {
    for (size_t i = 0; i < g_cch_grade_memo_count; i++) {
        if (strcmp(g_cch_grade_memo[i].path, abs_src) == 0) return &g_cch_grade_memo[i];
    }
    return NULL;
}

static void cc__cch_grade_memo_set(const char* abs_src, int grade) {
    CCCchGradeMemo* e = cc__cch_grade_memo_find(abs_src);
    if (e) { e->grade = grade; return; }
    if (g_cch_grade_memo_count == g_cch_grade_memo_cap) {
        size_t cap = g_cch_grade_memo_cap ? g_cch_grade_memo_cap * 2 : 8;
        CCCchGradeMemo* nv = (CCCchGradeMemo*)realloc(g_cch_grade_memo, cap * sizeof(*nv));
        if (!nv) return;
        g_cch_grade_memo = nv;
        g_cch_grade_memo_cap = cap;
    }
    g_cch_grade_memo[g_cch_grade_memo_count].path = strdup(abs_src);
    if (!g_cch_grade_memo[g_cch_grade_memo_count].path) return;
    g_cch_grade_memo[g_cch_grade_memo_count].grade = grade;
    g_cch_grade_memo_count++;
}

/* Own-text only. A nested impl-grade `#include "leaf.cch"` does not make
 * this file impl-grade — the leaf splices into the including unit unless
 * a sibling `.ccs` owns those bodies; this file still extracts to a `.h`. */
static int cc__local_cch_is_impl_grade(const char* abs_src) {
    char* src = NULL;
    size_t n = 0;
    int grade = 0;
    CCCchGradeMemo* memo = cc__cch_grade_memo_find(abs_src);
    if (memo) return memo->grade > 0;
    cc__cch_grade_memo_set(abs_src, -1);
    if (cc__read_file_text(abs_src, &src, &n) == 0 && src)
        grade = cc__cch_text_is_impl_grade(src, n) ? 1 : 0;
    free(src);
    cc__cch_grade_memo_set(abs_src, grade);
    return grade;
}

/* Headers already spliced into the current rewrite (one top-level call of
 * cc_rewrite_local_cch_includes_to_lowered_headers), by realpath.  A repeat
 * include of a spliced header is inert (its include guard would have made it
 * a no-op) and is replaced with a blank line. */
static char** g_spliced_impl_cch = NULL;
static size_t g_spliced_impl_cch_count = 0;
static size_t g_spliced_impl_cch_cap = 0;
/* 1 when rewriting a .ccs / spliced impl body: impl children splice.
 * 0 when lowering an interface `.cch` → `.h`: omit impl includes that
 * have no sibling `.ccs` (the including unit already spliced those
 * leaves). Sibling-backed leaves extract to `.h` even in this mode. */
static int g_rewrite_allow_impl_splice = 1;
/* Top-level `.ccs` of the current include rewrite. Used to distinguish
 * the defining sibling TU (`find.ccs` including `find.cch`) from every
 * other consumer. */
static const char* g_rewrite_root_path = NULL;
static int cc__lowered_header_needs_ufcs_splice(const char* body, size_t body_len);

static int cc__cch_sibling_ccs_path(const char* abs_cch, char* out, size_t cap) {
    size_t n;
    if (!abs_cch || !out || cap < 5) return 0;
    n = strlen(abs_cch);
    if (n < 4 || memcmp(abs_cch + n - 4, ".cch", 4) != 0) return 0;
    if (n - 4 + 5 > cap) return 0;
    memcpy(out, abs_cch, n - 4);
    memcpy(out + n - 4, ".ccs", 5);
    return 1;
}

static int cc__cch_has_sibling_ccs(const char* abs_cch) {
    char sib[PATH_MAX];
    if (!cc__cch_sibling_ccs_path(abs_cch, sib, sizeof(sib))) return 0;
    return access(sib, F_OK) == 0;
}

static int cc__cch_root_is_defining_ccs(const char* abs_cch) {
    char sib[PATH_MAX];
    char sib_real[PATH_MAX];
    if (!g_rewrite_root_path || !g_rewrite_root_path[0]) return 0;
    if (!cc__cch_sibling_ccs_path(abs_cch, sib, sizeof(sib))) return 0;
    if (access(sib, F_OK) != 0) return 0;
    if (realpath(sib, sib_real) && strcmp(g_rewrite_root_path, sib_real) == 0)
        return 1;
    return strcmp(g_rewrite_root_path, sib) == 0;
}

/* Sibling `.ccs` exists and this rewrite is not that file: extract decls. */
static int cc__cch_extract_for_other_tus(const char* abs_cch) {
    if (!cc__cch_has_sibling_ccs(abs_cch)) return 0;
    return !cc__cch_root_is_defining_ccs(abs_cch);
}

/* File-scope `foo(...) { ... }` → `foo(...);` so a sibling-backed `.cch`
 * can extract to a host `.h`. Bodies lower in the defining `.ccs`. */
static char* cc__strip_cch_function_bodies(const char* src, size_t n) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    int brace = 0;
    int paren = 0;
    char last_sig = 0;
    int changed = 0;
    CCScannerState scan;
    if (!src || n == 0) return NULL;
    cc_scanner_init(&scan);
    while (i < n) {
        size_t before = i;
        char c;
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) {
            cc_sb_append(&out, &out_len, &out_cap, src + before, i - before);
            continue;
        }
        c = src[i];
        if (c == '{' && brace == 0 && paren == 0 && last_sig == ')') {
            size_t body_r = 0;
            if (cc_find_matching_brace(src, n, i, &body_r)) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, ";");
                i = body_r + 1;
                last_sig = ';';
                changed = 1;
                continue;
            }
        }
        if (c == '{') brace++;
        else if (c == '}' && brace > 0) brace--;
        else if (c == '(') paren++;
        else if (c == ')' && paren > 0) paren--;
        if (c > 32) last_sig = c;
        cc_sb_append(&out, &out_len, &out_cap, src + i, 1);
        i++;
    }
    if (!changed) {
        free(out);
        return NULL;
    }
    return out;
}

static void cc__reset_spliced_impl_cch(void) {
    for (size_t i = 0; i < g_spliced_impl_cch_count; i++) free(g_spliced_impl_cch[i]);
    g_spliced_impl_cch_count = 0;
}

static int cc__impl_cch_was_spliced(const char* abs_src) {
    for (size_t i = 0; i < g_spliced_impl_cch_count; i++) {
        if (strcmp(g_spliced_impl_cch[i], abs_src) == 0) return 1;
    }
    return 0;
}

static int cc__impl_cch_mark_spliced(const char* abs_src) {
    if (g_spliced_impl_cch_count == g_spliced_impl_cch_cap) {
        size_t cap = g_spliced_impl_cch_cap ? g_spliced_impl_cch_cap * 2 : 8;
        char** nv = (char**)realloc(g_spliced_impl_cch, cap * sizeof(*nv));
        if (!nv) return -1;
        g_spliced_impl_cch = nv;
        g_spliced_impl_cch_cap = cap;
    }
    g_spliced_impl_cch[g_spliced_impl_cch_count] = strdup(abs_src);
    if (!g_spliced_impl_cch[g_spliced_impl_cch_count]) return -1;
    g_spliced_impl_cch_count++;
    return 0;
}

static char* cc__rewrite_local_cch_includes_impl(const char* src, size_t n, const char* current_path);
static int cc__lowered_header_needs_ufcs_splice(const char* body, size_t body_len);

/* #line  N "path" at the start of a unit-header wrap — dirname is the
 * original source dir (the wrap itself lives under unit_native/). */
static int cc__quote_file_from_line_bytes(const char* bytes, size_t len,
                                          char* dst, size_t cap) {
    size_t i = 0;
    size_t k;
    size_t nlen;
    if (!bytes || !dst || cap < 2) return 0;
    dst[0] = 0;
    if (len >= 3 && (unsigned char)bytes[0] == 0xef &&
        (unsigned char)bytes[1] == 0xbb && (unsigned char)bytes[2] == 0xbf)
        i = 3;
    if (i + 5 >= len || strncmp(bytes + i, "#line", 5) != 0) return 0;
    i += 5;
    while (i < len && (bytes[i] == ' ' || bytes[i] == '\t')) i++;
    while (i < len && bytes[i] >= '0' && bytes[i] <= '9') i++;
    while (i < len && (bytes[i] == ' ' || bytes[i] == '\t')) i++;
    if (i >= len || bytes[i] != '"') return 0;
    i++;
    k = i;
    while (k < len && bytes[k] != '"' && bytes[k] != '\n') k++;
    if (k >= len || bytes[k] != '"') return 0;
    nlen = k - i;
    if (nlen == 0 || nlen >= cap) return 0;
    memcpy(dst, bytes + i, nlen);
    dst[nlen] = 0;
    return 1;
}

static int cc__quote_dir_from_line_bytes(const char* bytes, size_t len,
                                         char* dst, size_t cap) {
    char path[PATH_MAX];
    char* slash;
    if (!cc__quote_file_from_line_bytes(bytes, len, path, sizeof(path)))
        return 0;
    slash = strrchr(path, '/');
    if (!slash) {
        snprintf(dst, cap, ".");
        return 1;
    }
    if (slash == path) {
        snprintf(dst, cap, "/");
        return 1;
    }
    *slash = 0;
    snprintf(dst, cap, "%s", path);
    return 1;
}

/* Canonical `.ccs` for this rewrite: `#line` on a unit_native wrap, else
 * realpath of the input. Cache wraps do not match `find.cch`'s sibling. */
static int cc__resolve_rewrite_root_ccs(const char* input_path,
                                        const char* src, size_t n,
                                        char* out, size_t cap) {
    char from_line[PATH_MAX];
    if (!out || cap < 2) return 0;
    out[0] = 0;
    if (src && n && cc__quote_file_from_line_bytes(src, n, from_line, sizeof(from_line))) {
        if (realpath(from_line, out)) return 1;
        {
            const char* qd = getenv("SHADOW_QUOTE_DIR");
            if (qd && qd[0]) {
                char join[PATH_MAX];
                const char* base = strrchr(from_line, '/');
                base = base ? base + 1 : from_line;
                snprintf(join, sizeof(join), "%s/%s", qd, base);
                if (realpath(join, out)) return 1;
            }
        }
    }
    if (input_path && input_path[0] && realpath(input_path, out)) return 1;
    return 0;
}

/* Extra quoted-include root when dirname(current_path) is a cache wrap.
 * Same sources as shadow_fill_quote_dir: SHADOW_QUOTE_DIR, else #line. */
static void cc__fill_quoted_cch_search_dir(const char* src, size_t n,
                                           char* dst, size_t cap) {
    const char* env;
    if (!dst || cap < 2) return;
    dst[0] = 0;
    env = getenv("SHADOW_QUOTE_DIR");
    if (env && env[0]) {
        snprintf(dst, cap, "%s", env);
        return;
    }
    if (src && n)
        (void)cc__quote_dir_from_line_bytes(src, n, dst, cap);
}

static int cc__try_quoted_cch_path(const char* dir, const char* rel,
                                   char* child_path, size_t child_cap,
                                   char* child_abs) {
    if (!dir || !dir[0] || !rel || !rel[0] || !child_path || child_cap < 2 ||
        !child_abs)
        return 0;
    snprintf(child_path, child_cap, "%s/%s", dir, rel);
    return realpath(child_path, child_abs) != NULL;
}

static int cc__path_ends_with(const char* p, const char* suf) {
    size_t n, s;
    if (!p || !suf) return 0;
    n = strlen(p);
    s = strlen(suf);
    return n >= s && memcmp(p + n - s, suf, s) == 0;
}

/* Splice an implementation-grade header's raw source into `out` in place of
 * its include line.  Nested local includes inside the header are processed
 * recursively (interface children lower to .h include lines, impl children
 * splice in turn).  `include_line_no` is the 1-based line of the include in
 * the including file, used to restore `#line` provenance after the splice.
 * Returns 0 on success, -1 when the header could not be read (caller falls
 * back to the interface path). */
static int cc__splice_impl_cch_into(char** out, size_t* out_len, size_t* out_cap,
                                    const char* child_abs,
                                    const char* current_path,
                                    size_t include_line_no) {
    char* body = NULL;
    size_t body_len = 0;
    char* rew = NULL;
    const char* use;
    size_t use_len;
    char ld[PATH_MAX + 64];
    if (cc__read_file_text(child_abs, &body, &body_len) != 0 || !body) {
        free(body);
        return -1;
    }
    if (cc__impl_cch_mark_spliced(child_abs) != 0) {
        free(body);
        return -1;
    }
    rew = cc__rewrite_local_cch_includes_impl(body, body_len, child_abs);
    use = rew ? rew : body;
    use_len = rew ? strlen(rew) : body_len;
    /* Angle `<ccc/….cch>` inside the splice is not pass_inc (those lines
     * sit under the header's `#ifndef` on the root tape).  Rewrite to `.h`
     * here — same as a top-level system include — so host cc never opens
     * raw stdlib `.cch` (`T !>(E)`, `@typehooks`). */
    {
        char* sys = cc_rewrite_system_cch_includes_to_lowered_headers(use,
                                                                     use_len);
        if (sys) {
            if (rew) free(rew);
            rew = sys;
            use = rew;
            use_len = strlen(rew);
        }
    }
    cc_sb_append_cstr(out, out_len, out_cap, CC_IMPL_CCH_BEGIN_MARK);
    cc_sb_append_cstr(out, out_len, out_cap, child_abs);
    cc_sb_append_cstr(out, out_len, out_cap, "*/\n");
    snprintf(ld, sizeof(ld), "#line 1 \"%s\"\n", child_abs);
    cc_sb_append_cstr(out, out_len, out_cap, ld);
    cc_sb_append(out, out_len, out_cap, use, use_len);
    if (use_len == 0 || use[use_len - 1] != '\n')
        cc_sb_append_cstr(out, out_len, out_cap, "\n");
    cc_sb_append_cstr(out, out_len, out_cap, CC_IMPL_CCH_END_MARK);
    cc_sb_append_cstr(out, out_len, out_cap, child_abs);
    cc_sb_append_cstr(out, out_len, out_cap, "*/\n");
    snprintf(ld, sizeof(ld), "#line %zu \"%s\"\n", include_line_no + 1, current_path);
    cc_sb_append_cstr(out, out_len, out_cap, ld);
    free(rew);
    free(body);
    return 0;
}

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
    cc__register_included_cch_tree(abs_src);
    for (size_t i = 0; i < g_lowered_local_header_count; ++i) {
        if (strcmp(g_lowered_local_headers[i].source_path, abs_src) == 0 &&
            access(g_lowered_local_headers[i].lowered_path, F_OK) == 0) {
            return g_lowered_local_headers[i].lowered_path;
        }
    }
    /* A give-up used to leave the include pointing at the `.cch`, so the
     * later parse reported something unrelated.  Fail at this include. */
#define CC__LOWER_GIVE_UP(step)                                                \
    do {                                                                       \
        fprintf(stderr,                                                        \
                "cc: error: cannot lower local header %s (%s: %s)\n",          \
                abs_src, (step), strerror(errno));                             \
        g_local_cch_lower_failed = 1;                                          \
        return NULL;                                                           \
    } while (0)
    if (cc__build_stable_lowered_header_path(abs_src, lowered_path, sizeof(lowered_path)) != 0)
        CC__LOWER_GIVE_UP("no lowered path");
    if (cc__dirname_local(lowered_path, lowered_dir, sizeof(lowered_dir)) != 0)
        CC__LOWER_GIVE_UP("dirname");
    if (cc__mkpath_local(lowered_dir) != 0)
        CC__LOWER_GIVE_UP("mkdir -p");
    if (cc__read_file_text(abs_src, &input, &input_len) != 0)
        CC__LOWER_GIVE_UP("read");
    if (cc__local_cch_is_impl_grade(abs_src) && !cc__cch_has_sibling_ccs(abs_src)) {
        fprintf(stderr,
                "cc: error: cannot extract impl-grade header %s "
                "(move bodies to a sibling .ccs, or #include it from a .ccs)\n",
                abs_src);
        g_local_cch_lower_failed = 1;
        free(input);
        return NULL;
    }
    {
        int saved_splice = g_rewrite_allow_impl_splice;
        g_rewrite_allow_impl_splice = 0;
        rewritten = cc__rewrite_local_cch_includes_impl(input, input_len, abs_src);
        g_rewrite_allow_impl_splice = saved_splice;
    }
    g_header_lower_preserve_tu_state++;
    lowered = cc_lower_header_string(rewritten ? rewritten : input,
                                     rewritten ? strlen(rewritten) : input_len,
                                     abs_src);
    g_header_lower_preserve_tu_state--;
    /* Never write raw `.cch` into the `.h` — that looks like a successful lower. */
    if (!lowered) CC__LOWER_GIVE_UP("lower");
    /* Only strip statement-level impl bodies. UFCS in an interface
     * `.cch` (`Type_destroy`, arena `.destroy()`) stays in the `.h`. */
    if (cc__local_cch_is_impl_grade(abs_src) && cc__cch_has_sibling_ccs(abs_src)) {
        char* stripped = cc__strip_cch_function_bodies(lowered, strlen(lowered));
        if (stripped) {
            free(lowered);
            lowered = stripped;
        }
    }
    if (cc__write_file_text(lowered_path, lowered, strlen(lowered)) != 0)
        CC__LOWER_GIVE_UP("write");
#undef CC__LOWER_GIVE_UP
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
    char search_dir[PATH_MAX];
    if (!src || !current_path) return NULL;
    if (cc__dirname_local(current_path, current_dir, sizeof(current_dir)) != 0) return NULL;
    /* Unit-header wraps live under unit_native/; quoted `.cch` sits next to
     * the original source.  Try dirname(current) first (C include rules),
     * then SHADOW_QUOTE_DIR / #line — same roots the tape uses later. */
    cc__fill_quoted_cch_search_dir(src, n, search_dir, sizeof(search_dir));
    while (i < n) {
        size_t line_end = i;
        size_t path_s = 0, path_e = 0;
        while (line_end < n && src[line_end] != '\n') line_end++;
        if (cc__match_local_include_line(src + i, line_end - i, &path_s, &path_e)) {
            size_t rel_len = path_e - path_s;
            if (rel_len >= 4 && strncmp(src + i + path_e - 4, ".cch", 4) == 0) {
                char rel_path[PATH_MAX];
                char child_path[PATH_MAX];
                char child_abs[PATH_MAX];
                const char* lowered_path;
                int found;
                if (rel_len >= sizeof(rel_path)) rel_len = sizeof(rel_path) - 1;
                memcpy(rel_path, src + i + path_s, rel_len);
                rel_path[rel_len] = '\0';
                found = cc__try_quoted_cch_path(current_dir, rel_path, child_path,
                                                sizeof(child_path), child_abs);
                if (!found && search_dir[0] &&
                    strcmp(search_dir, current_dir) != 0)
                    found = cc__try_quoted_cch_path(search_dir, rel_path,
                                                    child_path,
                                                    sizeof(child_path),
                                                    child_abs);
                if (!found)
                    snprintf(child_path, sizeof(child_path), "%s/%s",
                             current_dir, rel_path);
                /* Implementation-grade headers bypass .h lowering: splice
                 * their raw source into the stream so the full TU pipeline
                 * lowers it in context (see the block comment above
                 * cc__cch_text_is_impl_grade).
                 *
                 * Nested UFCS inside an already-spliced impl face still
                 * splices (redis_db → redis_mem). A `.ccs` that includes
                 * a UFCS-only header extracts it — one `.foo(` must not
                 * dump the file into the host TU.
                 *
                 * Do not splice every quoted `.cch` included from a `.ccs`.
                 * That inlines ordinary helpers: autoblock can no longer
                 * refuse an untyped string pack, and a syntax error in the
                 * header is blamed on the include site.  Chapter parent
                 * types stay in scope because the extracted `#include`
                 * is not hoisted above the TU declarations. */
                if (found) {
                    int splice_child = cc__local_cch_is_impl_grade(child_abs);
                    if (!splice_child && g_rewrite_allow_impl_splice) {
                        char* child_src = NULL;
                        size_t child_len = 0;
                        int ufcs = 0;
                        if (cc__read_file_text(child_abs, &child_src, &child_len) == 0 &&
                            child_src)
                            ufcs = cc__lowered_header_needs_ufcs_splice(child_src,
                                                                        child_len);
                        free(child_src);
                        /* Nested inside a spliced impl face (redis_mem), or
                         * the defining sibling `.ccs` — not every `.ccs`
                         * that includes an umbrella with one `.foo(`. */
                        if (ufcs &&
                            (cc__path_ends_with(current_path, ".cch") ||
                             cc__cch_root_is_defining_ccs(child_abs)))
                            splice_child = 1;
                        else if (ufcs && !cc__cch_has_sibling_ccs(child_abs)) {
                            /* Sibling-less leaf: its UFCS may bind
                             * registrations that live only in the parent TU
                             * (CC_MAP_DECL_UFCS in the .ccs, a TU-local
                             * generic instance). Lower it standalone and
                             * splice when method-call UFCS survives — an
                             * extracted `.h` with raw calls fails host
                             * compile blaming the header. Resolvable
                             * headers keep extracting. */
                            const char* lp = cc__lower_local_cch_header(child_abs);
                            if (lp) {
                                char* hb = NULL;
                                size_t hn = 0;
                                if (cc__read_file_text(lp, &hb, &hn) == 0 && hb &&
                                    cc__lowered_header_needs_ufcs_splice(hb, hn))
                                    splice_child = 1;
                                free(hb);
                            }
                        }
                    }
                    if (splice_child && cc__cch_extract_for_other_tus(child_abs))
                        splice_child = 0;
                    if (splice_child) {
                        if (!g_rewrite_allow_impl_splice) {
                            /* Lowering an interface `.h`: impl leaves with a
                             * sibling `.ccs` extract; otherwise fail loud. */
                            if (!cc__cch_has_sibling_ccs(child_abs)) {
                                fprintf(stderr,
                                        "cc: error: cannot extract impl-grade "
                                        "header %s (move bodies to a sibling "
                                        ".ccs, or #include it from a .ccs)\n",
                                        child_abs);
                                g_local_cch_lower_failed = 1;
                                free(out);
                                return NULL;
                            }
                        } else if (cc__impl_cch_was_spliced(child_abs)) {
                            /* Repeat include: the header's guard would make this
                             * inert; keep a blank line so following lines in this
                             * file keep their physical numbers. */
                            cc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
                            changed = 1;
                            i = (line_end < n) ? line_end + 1 : line_end;
                            continue;
                        } else {
                            size_t line_no = 1;
                            for (size_t k = 0; k < i; k++)
                                if (src[k] == '\n') line_no++;
                            if (cc__splice_impl_cch_into(&out, &out_len, &out_cap,
                                                         child_abs, current_path,
                                                         line_no) == 0) {
                                changed = 1;
                                i = (line_end < n) ? line_end + 1 : line_end;
                                continue;
                            }
                        }
                        /* Unreadable header, or sibling extract while lowering
                         * an umbrella: fall through to the interface path. */
                    }
                }
                lowered_path = cc__lower_local_cch_header(found ? child_abs
                                                               : child_path);
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
    /* One top-level rewrite = one logical translation unit: repeat includes
     * of an already-spliced implementation header are inert within it, but a
     * later rewrite (reparse, comptime dylib TU) must splice afresh. */
    g_local_cch_lower_failed = 0;
    cc__reset_spliced_impl_cch();
    {
        const char* saved_root = g_rewrite_root_path;
        char resolved[PATH_MAX];
        char* rewritten;
        resolved[0] = 0;
        if (cc__resolve_rewrite_root_ccs(input_path, src, input_len, resolved,
                                        sizeof(resolved)))
            g_rewrite_root_path = resolved;
        else
            g_rewrite_root_path = input_path;
        rewritten = cc__rewrite_local_cch_includes_impl(src, input_len, input_path);
        g_rewrite_root_path = saved_root;
        return rewritten;
    }
}

int cc_local_header_lower_failed(void) { return g_local_cch_lower_failed; }

size_t cc_lowered_local_header_count(void) { return g_lowered_local_header_count; }

const char* cc_lowered_local_header_source_path(size_t i) {
    if (i >= g_lowered_local_header_count || !g_lowered_local_headers) return NULL;
    return g_lowered_local_headers[i].source_path;
}

#define CC_LOCAL_CCH_BEGIN_MARK "/*cc:local_cch_begin:"
#define CC_LOCAL_CCH_END_MARK "/*cc:local_cch_end:"

static const CCLoweredLocalHeader* cc__find_lowered_header_by_include_path(const char* path,
                                                                           size_t path_len) {
    char want[PATH_MAX];
    char want_real[PATH_MAX];
    char have[PATH_MAX];
    size_t i;
    int want_ok;
    if (!path || path_len == 0 || path_len >= sizeof(want)) return NULL;
    memcpy(want, path, path_len);
    want[path_len] = '\0';
    want_ok = (realpath(want, want_real) != NULL);
    for (i = 0; i < g_lowered_local_header_count; ++i) {
        const char* lp = g_lowered_local_headers[i].lowered_path;
        if (!lp) continue;
        if (strcmp(lp, want) == 0) return &g_lowered_local_headers[i];
        if (want_ok && realpath(lp, have) && strcmp(have, want_real) == 0)
            return &g_lowered_local_headers[i];
    }
    return NULL;
}

/* True when a lowered local header still has method-call UFCS (`->name(` /
 * `.name(`) that needs the parent TU's phase3 pass.  Decl-only / schema
 * headers stay as `#include` so splicing them cannot perturb field maps. */
static int cc__lowered_header_needs_ufcs_splice(const char* body, size_t body_len) {
    size_t p = 0;
    if (!body || body_len < 4) return 0;
    while (p + 3 < body_len) {
        char c = body[p];
        char c2 = body[p + 1];
        if (c == '/' && c2 == '/') {
            p += 2;
            while (p < body_len && body[p] != '\n') p++;
            continue;
        }
        if (c == '/' && c2 == '*') {
            p += 2;
            while (p + 1 < body_len && !(body[p] == '*' && body[p + 1] == '/')) p++;
            if (p + 1 < body_len) p += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            p++;
            while (p < body_len) {
                if (body[p] == '\\' && p + 1 < body_len) { p += 2; continue; }
                if (body[p] == q) { p++; break; }
                p++;
            }
            continue;
        }
        if ((c == '-' && c2 == '>') || (c == '.')) {
            size_t k = p + ((c == '.') ? 1 : 2);
            if (k < body_len && (cc_is_ident_start(body[k]) || body[k] == '_')) {
                while (k < body_len && (cc_is_ident_char(body[k]) || body[k] == '_')) k++;
                while (k < body_len && (body[k] == ' ' || body[k] == '\t' || body[k] == '\n')) k++;
                if (k < body_len && body[k] == '(') return 1;
            }
        }
        p++;
    }
    return 0;
}

char* cc_splice_local_lowered_headers_for_codegen(const char* src, size_t n) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    int changed = 0;
    /* Track #line/CC_LN ledger while walking so a UFCS splice can restore the
     * including file after the nested body — otherwise every line below the
     * splice stays attributed to the nested .cch (host-C and AST diagnostics). */
    char cur_file[PATH_MAX];
    int cur_line = 1;
    int have_file = 0;
    cur_file[0] = '\0';
    if (!src || n == 0 || g_lowered_local_header_count == 0) return NULL;
    while (i < n) {
        size_t line_end = i;
        size_t path_s = 0, path_e = 0;
        long ledger_n = 0;
        const char* ledger_p = NULL;
        size_t ledger_pl = 0;
        int is_ledger = 0;
        while (line_end < n && src[line_end] != '\n') line_end++;
        is_ledger = cc_ledger_parse_line(src, i, line_end, &ledger_n, &ledger_p, &ledger_pl) &&
                    ledger_n > 0;
        if (is_ledger) {
            cur_line = (int)ledger_n; /* next physical line is user line n */
            if (ledger_p && ledger_pl > 0 && ledger_pl < sizeof(cur_file)) {
                memcpy(cur_file, ledger_p, ledger_pl);
                cur_file[ledger_pl] = '\0';
                have_file = 1;
            }
        }
        if (cc__match_local_include_line(src + i, line_end - i, &path_s, &path_e)) {
            const CCLoweredLocalHeader* h =
                cc__find_lowered_header_by_include_path(src + i + path_s, path_e - path_s);
            if (h && h->lowered_path && h->source_path) {
                char* body = NULL;
                size_t body_len = 0;
                if (cc__read_file_text(h->lowered_path, &body, &body_len) == 0 && body &&
                    cc__lowered_header_needs_ufcs_splice(body, body_len)) {
                    int include_line = cur_line;
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "#line 1 \"");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, h->source_path);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "\"\n");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, CC_LOCAL_CCH_BEGIN_MARK);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, h->lowered_path);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "*/\n");
                    cc_sb_append(&out, &out_len, &out_cap, body, body_len);
                    if (body_len == 0 || body[body_len - 1] != '\n')
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, CC_LOCAL_CCH_END_MARK);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, h->lowered_path);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "*/\n");
                    if (have_file) {
                        char ld[PATH_MAX + 64];
                        snprintf(ld, sizeof(ld), "#line %d \"%s\"\n",
                                 include_line + 1, cur_file);
                        cc_sb_append_cstr(&out, &out_len, &out_cap, ld);
                    }
                    free(body);
                    changed = 1;
                    cur_line = include_line + 1;
                    i = (line_end < n) ? line_end + 1 : line_end;
                    continue;
                }
                free(body);
            }
        }
        cc_sb_append(&out, &out_len, &out_cap, src + i, line_end - i);
        if (line_end < n && src[line_end] == '\n') cc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
        i = (line_end < n) ? line_end + 1 : line_end;
        if (!is_ledger) cur_line++;
    }
    if (!changed) {
        free(out);
        return NULL;
    }
    return out;
}

int cc_writeback_local_lowered_headers_from_codegen(char** src, size_t* n) {
    char* in;
    size_t in_len;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0;
    int changed = 0;
    const size_t begin_len = sizeof(CC_LOCAL_CCH_BEGIN_MARK) - 1;
    const size_t end_len = sizeof(CC_LOCAL_CCH_END_MARK) - 1;
    if (!src || !*src || !n) return 0;
    in = *src;
    in_len = *n;
    while (i < in_len) {
        const char* hit = strstr(in + i, CC_LOCAL_CCH_BEGIN_MARK);
        const char* path_s;
        const char* path_e;
        const char* body_s;
        const char* end;
        const char* end_path_e;
        size_t path_len;
        size_t hit_off;
        char path[PATH_MAX];
        if (!hit) {
            cc_sb_append(&out, &out_len, &out_cap, in + i, in_len - i);
            break;
        }
        hit_off = (size_t)(hit - in);
        path_s = hit + begin_len;
        path_e = strstr(path_s, "*/");
        if (!path_e) {
            if (hit_off > i) cc_sb_append(&out, &out_len, &out_cap, in + i, hit_off - i);
            cc_sb_append(&out, &out_len, &out_cap, hit, in_len - hit_off);
            break;
        }
        path_len = (size_t)(path_e - path_s);
        if (path_len == 0 || path_len >= sizeof(path)) {
            if (hit_off > i) cc_sb_append(&out, &out_len, &out_cap, in + i, hit_off - i);
            cc_sb_append(&out, &out_len, &out_cap, hit, begin_len);
            i = hit_off + begin_len;
            continue;
        }
        memcpy(path, path_s, path_len);
        path[path_len] = '\0';
        /* Drop the `#line 1 "<source.cch>"` the splice inserted immediately
         * before the begin mark.  Leaving it orphans every subsequent line
         * onto the nested header after writeback restores `#include`. */
        {
            size_t prefix_end = hit_off;
            const char* src_path = cc_lowered_header_source_for(path);
            if (src_path && hit_off > i && in[hit_off - 1] == '\n') {
                size_t ls = hit_off - 1;
                long ln = 0;
                const char* lp = NULL;
                size_t lpl = 0;
                while (ls > i && in[ls - 1] != '\n') ls--;
                if (cc_ledger_parse_line(in, ls, hit_off - 1, &ln, &lp, &lpl) && ln == 1 &&
                    lp && lpl == strlen(src_path) && memcmp(lp, src_path, lpl) == 0) {
                    prefix_end = ls;
                }
            }
            if (prefix_end > i)
                cc_sb_append(&out, &out_len, &out_cap, in + i, prefix_end - i);
        }
        body_s = path_e + 2;
        if (*body_s == '\n') body_s++;
        end = body_s;
        end_path_e = NULL;
        for (;;) {
            end = strstr(end, CC_LOCAL_CCH_END_MARK);
            if (!end) break;
            end_path_e = strstr(end + end_len, "*/");
            if (!end_path_e) { end = NULL; break; }
            if ((size_t)(end_path_e - (end + end_len)) == path_len &&
                memcmp(end + end_len, path, path_len) == 0) {
                break;
            }
            end = end + end_len;
            end_path_e = NULL;
        }
        if (!end || !end_path_e) {
            cc_sb_append(&out, &out_len, &out_cap, hit, begin_len);
            i = hit_off + begin_len;
            continue;
        }
        {
            size_t body_len = (size_t)(end - body_s);
            if (body_len > 0 && body_s[body_len - 1] == '\n') body_len--;
            if (cc__write_file_text(path, body_s, body_len) != 0) {
                free(out);
                return -1;
            }
            cc_sb_append_cstr(&out, &out_len, &out_cap, "#include \"");
            cc_sb_append_cstr(&out, &out_len, &out_cap, path);
            cc_sb_append_cstr(&out, &out_len, &out_cap, "\"\n");
            changed = 1;
            i = (size_t)(end_path_e + 2 - in);
            if (i < in_len && in[i] == '\n') i++;
        }
    }
    if (!changed) {
        free(out);
        return 0;
    }
    free(*src);
    *src = out;
    *n = out_len;
    return 0;
}

/* Harvest CC_GENERIC_FACTORY / _EXTEND blocks from every local .cch included by
 * the current TU (the lowered-header registry holds the transitive set, deduped
 * by realpath), so factories defined in headers register and run in the
 * including TU's comptime scope.  Each block is emitted verbatim, preceded by a
 * `#line <kwline> "<abs.cch>"` directive, so the comptime fn registry's
 * #line-aware resolver attributes the factory — and any diagnostic about its
 * emitted C — back to the .cch the user wrote.  The factory text is left in raw
 * `CC_GENERIC_FACTORY(...)` form; the caller appends it to the TU buffer where
 * the normal factory-rewrite + @emit lowering + comptime collection process it
 * uniformly with .ccs-defined factories.  Returns malloc'd text, or NULL when
 * no included header defines a factory. */
/* Collect the CC_GENERIC_FACTORY / _EXTEND blocks in one header's text. */
static int cc__harvest_factories_from(const char* src, size_t n, const char* path,
                                      char** out_p, size_t* out_len_p, size_t* out_cap_p);

char* cc_harvest_local_header_factories(void) {
    static const char kw[] = "CC_GENERIC_FACTORY";
    static const char kw_ext[] = "CC_GENERIC_FACTORY_EXTEND";
    const size_t kwlen = sizeof(kw) - 1;
    const size_t kwlen_ext = sizeof(kw_ext) - 1;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    int any = 0;
    for (size_t h = 0; h < g_lowered_local_header_count; h++) {
        const char* path = g_lowered_local_headers[h].source_path;
        char* src = NULL;
        size_t n = 0;
        if (!path || cc__read_file_text(path, &src, &n) != 0) continue;
        size_t i = 0;
        CCScannerState scan;
        cc_scanner_init(&scan);
        while (i < n) {
            if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
            char c = src[i];
            size_t mlen = 0;
            if (c == 'C') {
                if (i + kwlen_ext <= n && memcmp(src + i, kw_ext, kwlen_ext) == 0 &&
                    (i == 0 || !cc_is_ident_char(src[i - 1])) &&
                    (i + kwlen_ext >= n || !cc_is_ident_char(src[i + kwlen_ext])))
                    mlen = kwlen_ext;
                else if (i + kwlen <= n && memcmp(src + i, kw, kwlen) == 0 &&
                         (i == 0 || !cc_is_ident_char(src[i - 1])) &&
                         (i + kwlen >= n || !cc_is_ident_char(src[i + kwlen])))
                    mlen = kwlen;
            }
            if (mlen) {
                size_t start = i;
                size_t p = cc_skip_ws_and_comments(src, n, i + mlen);
                if (p >= n || src[p] != '(') { i++; continue; }
                size_t rp = 0;
                if (!cc_find_matching_paren(src, n, p, &rp)) { i++; continue; }
                p = cc_skip_ws_and_comments(src, n, rp + 1);
                if (p >= n || src[p] != '{') { i++; continue; }
                size_t body_l = p, body_r = 0;
                if (!cc_find_matching_brace(src, n, body_l, &body_r)) { i++; continue; }
                if (body_r <= body_l) { i++; continue; }
                int kwline = 1;
                for (size_t k = 0; k < start; k++) if (src[k] == '\n') kwline++;
                {
                    char ld[PATH_MAX + 64];
                    /* No leading blank: callers append after a body that already
                     * ends in `\n`.  An extra newline would map past TU EOF. */
                    snprintf(ld, sizeof(ld), "#line %d \"%s\"\n", kwline, path);
                    if (out_len > 0 && out[out_len - 1] != '\n')
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ld);
                }
                cc_sb_append(&out, &out_len, &out_cap, src + start, body_r + 1 - start);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
                any = 1;
                i = body_r + 1;
                continue;
            }
            i++;
        }
        free(src);
    }
    /* Installed `.cch` headers too: `lower_headers` blanks their factory blocks
     * out of the lowered `.h`, so an unharvested library factory simply never
     * registers and the use site reports the generic name as unknown.
     *
     * A local header can appear in BOTH lists, and harvesting it twice defines
     * its monomorphs twice, so skip anything the loop above already covered. */
    for (size_t h = 0; h < g_included_cch_source_count; h++) {
        size_t hn = 0;
        const char* hsrc;
        const char* hpath = g_included_cch_sources[h];
        int seen = 0;
        for (size_t l = 0; l < g_lowered_local_header_count && !seen; l++) {
            const char* lp = g_lowered_local_headers[l].source_path;
            if (lp && hpath && strcmp(lp, hpath) == 0) seen = 1;
        }
        if (seen) continue;
        hsrc = cc__included_cch_text(h, &hn);
        if (!hsrc || hn == 0) continue;
        any |= cc__harvest_factories_from(hsrc, hn, hpath ? hpath : "<header>",
                                          &out, &out_len, &out_cap);
    }
    if (!any) { free(out); return NULL; }
    return out;
}

static int cc__harvest_factories_from(const char* src, size_t n, const char* path,
                                      char** out_p, size_t* out_len_p,
                                      size_t* out_cap_p) {
    static const char kw[] = "CC_GENERIC_FACTORY";
    static const char kw_ext[] = "CC_GENERIC_FACTORY_EXTEND";
    const size_t kwlen = sizeof(kw) - 1;
    const size_t kwlen_ext = sizeof(kw_ext) - 1;
    char* out = *out_p;
    size_t out_len = *out_len_p, out_cap = *out_cap_p;
    int any = 0;
    size_t i = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);
    while (i < n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        size_t mlen = 0;
        if (src[i] == 'C') {
            if (i + kwlen_ext <= n && memcmp(src + i, kw_ext, kwlen_ext) == 0 &&
                (i == 0 || !cc_is_ident_char(src[i - 1])) &&
                (i + kwlen_ext >= n || !cc_is_ident_char(src[i + kwlen_ext])))
                mlen = kwlen_ext;
            else if (i + kwlen <= n && memcmp(src + i, kw, kwlen) == 0 &&
                     (i == 0 || !cc_is_ident_char(src[i - 1])) &&
                     (i + kwlen >= n || !cc_is_ident_char(src[i + kwlen])))
                mlen = kwlen;
        }
        if (mlen) {
            size_t start = i;
            size_t p = cc_skip_ws_and_comments(src, n, i + mlen);
            size_t rp = 0, body_l, body_r = 0;
            if (p >= n || src[p] != '(') { i++; continue; }
            if (!cc_find_matching_paren(src, n, p, &rp)) { i++; continue; }
            p = cc_skip_ws_and_comments(src, n, rp + 1);
            if (p >= n || src[p] != '{') { i++; continue; }
            body_l = p;
            if (!cc_find_matching_brace(src, n, body_l, &body_r) || body_r <= body_l) {
                i++; continue;
            }
            {
                int kwline = 1;
                char ld[PATH_MAX + 64];
                for (size_t k = 0; k < start; k++) if (src[k] == '\n') kwline++;
                snprintf(ld, sizeof(ld), "#line %d \"%s\"\n", kwline, path);
                if (out_len > 0 && out[out_len - 1] != '\n')
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
                cc_sb_append_cstr(&out, &out_len, &out_cap, ld);
            }
            cc_sb_append(&out, &out_len, &out_cap, src + start, body_r + 1 - start);
            cc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
            any = 1;
            i = body_r + 1;
            continue;
        }
        i++;
    }
    *out_p = out; *out_len_p = out_len; *out_cap_p = out_cap;
    return any;
}

/* Reusable @comptime functions in .cch files follow the same model as header
 * generic factories: harvest their raw definitions into the including TU,
 * then let the normal registry/executor path compile them. File-scope typedef
 * and enum declarations from those headers are installed as the comptime
 * executor prelude so call-site @comptime blocks can name the helper types. */
char* cc_harvest_header_comptime_functions(void) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    char* prelude = NULL;
    size_t prelude_len = 0, prelude_cap = 0;
    int any = 0;
    for (size_t h = 0; h < g_included_cch_source_count; h++) {
        const char* path = g_included_cch_sources[h];
        char* src = NULL;
        size_t n = 0, i = 0;
        int has_comptime_fn = 0;
        CCScannerState scan;
        if (!path || cc__read_file_text(path, &src, &n) != 0) continue;
        /* Only harvest helper types from headers that define a real @comptime
         * function — skip comments/strings, and ignore @comptime {/if/for. */
        i = 0;
        cc_scanner_init(&scan);
        while (i < n) {
            if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
            char c = src[i];
            if (c == '@' && cc_match_ident_kw(src, n, i + 1, "comptime")) {
                size_t p = cc_skip_ws_and_comments(src, n, i + 1 + strlen("comptime"));
                if (p < n && src[p] != '{' &&
                    !cc_match_ident_kw(src, n, p, "if") &&
                    !cc_match_ident_kw(src, n, p, "for")) {
                    has_comptime_fn = 1;
                    break;
                }
            }
            i++;
        }
        if (!has_comptime_fn) { free(src); continue; }
        /* Pass 1: typedef / enum helpers for the executor prelude only.
         * Restrict to this header's own helper surface — skip nested includes'
         * transitive types by only accepting declarations before the first
         * @comptime function. */
        i = 0;
        cc_scanner_init(&scan);
        while (i < n) {
            if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
            char c = src[i];
            if (c == '@' && cc_match_ident_kw(src, n, i + 1, "comptime")) break;
            if ((cc_match_ident_kw(src, n, i, "typedef") ||
                 cc_match_ident_kw(src, n, i, "enum")) &&
                (i == 0 || !cc_is_ident_char(src[i - 1]))) {
                size_t start = i;
                size_t p = i;
                int depth = 0;
                CCScannerState pscan;
                cc_scanner_init(&pscan);
                while (p < n) {
                    if (cc_scanner_skip_non_code(&pscan, src, n, &p)) continue;
                    char d = src[p];
                    if (d == '{') { depth++; p++; continue; }
                    if (d == '}') { if (depth) depth--; p++; continue; }
                    if (d == ';' && depth == 0) {
                        cc_sb_append(&prelude, &prelude_len, &prelude_cap,
                                     src + start, p + 1 - start);
                        cc_sb_append_cstr(&prelude, &prelude_len, &prelude_cap, "\n");
                        i = p + 1;
                        break;
                    }
                    p++;
                }
                if (p >= n) break;
                continue;
            }
            i++;
        }
        /* Pass 2: @comptime function definitions into the TU buffer. */
        i = 0;
        cc_scanner_init(&scan);
        while (i < n) {
            if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
            char c = src[i];
            if (c == '@' && cc_match_ident_kw(src, n, i + 1, "comptime")) {
                size_t start = i;
                size_t p = cc_skip_ws_and_comments(src, n, i + 1 + strlen("comptime"));
                size_t lparen = 0, rparen = 0, body_r = 0;
                if (p >= n || src[p] == '{' ||
                    cc_match_ident_kw(src, n, p, "if") ||
                    cc_match_ident_kw(src, n, p, "for")) {
                    i++;
                    continue;
                }
                {
                    size_t semi_q = cc_find_char_top_level(src, p, n, ';');
                    size_t brace_q = cc_find_char_top_level(src, p, n, '{');
                    size_t paren_q = cc_find_char_top_level(src, p, n, '(');
                    if (paren_q < n && paren_q < semi_q && paren_q < brace_q)
                        lparen = paren_q;
                }
                if (!lparen || !cc_find_matching_paren(src, n, lparen, &rparen)) {
                    i++;
                    continue;
                }
                p = cc_skip_ws_and_comments(src, n, rparen + 1);
                if (p >= n || src[p] != '{' ||
                    !cc_find_matching_brace(src, n, p, &body_r)) {
                    i++;
                    continue;
                }
                {
                    int line = 1;
                    char ld[PATH_MAX + 64];
                    for (size_t k = 0; k < start; k++) if (src[k] == '\n') line++;
                    /* No leading blank — see factory harvest above. */
                    snprintf(ld, sizeof(ld), "#line %d \"%s\"\n", line, path);
                    if (out_len > 0 && out[out_len - 1] != '\n')
                        cc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
                    cc_sb_append_cstr(&out, &out_len, &out_cap, ld);
                    cc_sb_append(&out, &out_len, &out_cap,
                                 src + start, body_r + 1 - start);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
                }
                any = 1;
                i = body_r + 1;
                continue;
            }
            i++;
        }
        free(src);
    }
    cc_comptime_fn_registry_set_prelude(prelude);
    free(prelude);
    if (!any) { free(out); return NULL; }
    return out;
}

/* Top-level `@comptime { ... }` in local .cch headers — same harvest model as
 * CC_GENERIC_FACTORY.  lower_header blanks these so the .h stays host-C; the
 * including TU re-runs them so CC_EMIT_AT_COMPTIME_SITE (e.g. static_map's
 * `<name>_get`) lands in the merged .c.  Skips `@comptime if/for` and
 * `@comptime` function definitions (handled elsewhere). */
char* cc_harvest_local_header_comptime_blocks(void) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    int any = 0;
    for (size_t h = 0; h < g_lowered_local_header_count; h++) {
        const char* path = g_lowered_local_headers[h].source_path;
        char* src = NULL;
        size_t n = 0;
        size_t i = 0;
        CCScannerState scan;
        if (!path || cc__read_file_text(path, &src, &n) != 0) continue;
        cc_scanner_init(&scan);
        while (i < n) {
            if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
            if (src[i] != '@' || !cc_match_ident_kw(src, n, i + 1, "comptime")) {
                i++;
                continue;
            }
            size_t start = i;
            size_t p = cc_skip_ws_and_comments(src, n, i + 1 + strlen("comptime"));
            size_t body_r = 0;
            /* Only plain blocks — not if/for/functions. */
            if (p >= n || src[p] != '{') {
                i++;
                continue;
            }
            if (!cc_find_matching_brace(src, n, p, &body_r)) {
                i++;
                continue;
            }
            {
                int line = 1;
                char ld[PATH_MAX + 64];
                for (size_t k = 0; k < start; k++) if (src[k] == '\n') line++;
                snprintf(ld, sizeof(ld), "#line %d \"%s\"\n", line, path);
                if (out_len > 0 && out[out_len - 1] != '\n')
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
                cc_sb_append_cstr(&out, &out_len, &out_cap, ld);
                cc_sb_append(&out, &out_len, &out_cap,
                             src + start, body_r + 1 - start);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
            }
            any = 1;
            i = body_r + 1;
        }
        free(src);
    }
    if (!any) { free(out); return NULL; }
    return out;
}

const char* cc_lowered_header_source_for(const char* lowered_path) {
    if (!lowered_path || !lowered_path[0]) return NULL;
    for (size_t i = 0; i < g_lowered_local_header_count; ++i) {
        if (strcmp(g_lowered_local_headers[i].lowered_path, lowered_path) == 0)
            return g_lowered_local_headers[i].source_path;
    }
    /* TCC may hand back a realpath-normalized spelling of the include it was
       given; fall back to comparing canonicalized paths. */
    char want[PATH_MAX];
    if (!realpath(lowered_path, want)) return NULL;
    for (size_t i = 0; i < g_lowered_local_header_count; ++i) {
        char have[PATH_MAX];
        if (realpath(g_lowered_local_headers[i].lowered_path, have) &&
            strcmp(have, want) == 0)
            return g_lowered_local_headers[i].source_path;
    }
    return NULL;
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
                            char rel[PATH_MAX], abs_src[PATH_MAX];
                            size_t rel_len = close - (p + 1);
                            if (rel_len < sizeof(rel)) {
                                memcpy(rel, src + p + 1, rel_len);
                                rel[rel_len] = '\0';
                                /* Register the raw `.cch` so factories /
                                 * @comptime harvest still see js_module /
                                 * py_module after the include is rewritten
                                 * to the blanked `.h`.  Resolve via
                                 * CC_INCLUDE_PATH so prefix installs work
                                 * for sources outside any checkout. */
                                if (cc_path_resolve_system_cch(rel, abs_src,
                                                               sizeof(abs_src)))
                                    cc__register_included_cch_tree(abs_src);
                            }
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

/* Host .h cannot keep parser-safe `__CC_VEC(T)` spellings: UFCS looks at
 * the field type text and needs `CCVec_T`. */
static char* cc__expand_header_vec_macros(const char* src, size_t n) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0, last = 0, i = 0;
    int any = 0;
    CCScannerState scan;
    if (!src || n == 0) return NULL;
    cc_scanner_init(&scan);
    while (i < n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (i + 9 <= n && memcmp(src + i, "__CC_VEC(", 9) == 0 &&
            (i == 0 || !cc_is_ident_char(src[i - 1]))) {
            size_t rp = 0;
            if (cc_find_matching_paren(src, n, i + 8, &rp)) {
                const char* a = src + i + 9;
                size_t alen = rp - (i + 9);
                while (alen && (*a == ' ' || *a == '\t')) { a++; alen--; }
                while (alen && (a[alen - 1] == ' ' || a[alen - 1] == '\t')) alen--;
                cc_sb_append(&out, &out_len, &out_cap, src + last, i - last);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "CCVec_");
                cc_sb_append(&out, &out_len, &out_cap, a, alen);
                last = rp + 1;
                i = rp + 1;
                any = 1;
                continue;
            }
        }
        if (i + 14 <= n && memcmp(src + i, "__CC_VEC_INIT(", 14) == 0 &&
            (i == 0 || !cc_is_ident_char(src[i - 1]))) {
            size_t rp = 0;
            if (cc_find_matching_paren(src, n, i + 13, &rp)) {
                const char* inner = src + i + 14;
                size_t ilen = rp - (i + 14);
                const char* comma = NULL;
                int depth = 0;
                size_t k;
                for (k = 0; k < ilen; k++) {
                    if (inner[k] == '(') depth++;
                    else if (inner[k] == ')') depth--;
                    else if (inner[k] == ',' && depth == 0) { comma = inner + k; break; }
                }
                if (comma) {
                    const char* t = inner;
                    size_t tlen = (size_t)(comma - inner);
                    const char* rest = comma + 1;
                    size_t rlen = (size_t)((src + rp) - rest);
                    while (tlen && (*t == ' ' || *t == '\t')) { t++; tlen--; }
                    while (tlen && (t[tlen - 1] == ' ' || t[tlen - 1] == '\t')) tlen--;
                    cc_sb_append(&out, &out_len, &out_cap, src + last, i - last);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "CCVec_");
                    cc_sb_append(&out, &out_len, &out_cap, t, tlen);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "_init((");
                    cc_sb_append(&out, &out_len, &out_cap, rest, rlen);
                    cc_sb_append_cstr(&out, &out_len, &out_cap, "), CC_VEC_INITIAL_CAP)");
                    last = rp + 1;
                    i = rp + 1;
                    any = 1;
                    continue;
                }
            }
        }
        i++;
    }
    if (!any) { free(out); return NULL; }
    cc_sb_append(&out, &out_len, &out_cap, src + last, n - last);
    return out;
}

char* cc_rewrite_header_type_syntax_shared(const char* src,
                                           size_t input_len,
                                           const char* input_path) {
    CCPassChain chain;
    char* out = NULL;
    if (!src || input_len == 0) return NULL;

    /* Header lowering should share the same type-syntax understanding as the
       main preprocess pipeline for syntax that must not survive into plain C
       headers. Keep this intentionally limited to header-safe rewrites. */
    if (!g_header_lower_preserve_tu_state) {
        if (!cc_type_graph_ensure_global_cleared()) return NULL;
        /* Isolated `.cch` → `.h` has no TU include ingest. Drop leftovers
         * from the previous header in this `lower_headers` process (readdir
         * order is not stable across hosts), then register imports — not this
         * file — so Exclusive/Vec callees resolve without treating same-file
         * wrappers as UFCS targets. */
        cc_reset_included_cch_sources();
        g_ufcs_header_path[0] = 0;
        if (input_path && input_path[0]) {
            if (!realpath(input_path, g_ufcs_header_path))
                g_ufcs_header_path[0] = 0;
            cc__register_included_cch_imports(input_path);
        }
    } else if (input_path && input_path[0]) {
        /* TU extract: keep include/declare/result indexes. Still name this
         * header for UFCS path notes. */
        if (!realpath(input_path, g_ufcs_header_path))
            g_ufcs_header_path[0] = 0;
    }

    cc_pass_chain_init(&chain, src, input_len);
    if (cc_pass_chain_apply(&chain, cc__normalize_template_recv_chains(chain.src, chain.len)) < 0) goto chain_cleanup;
    if (cc_pass_chain_apply(&chain, cc__rewrite_string_templates(chain.src, chain.len, input_path)) < 0) goto chain_cleanup;
    if (cc_pass_chain_apply(&chain, cc__rewrite_chan_handle_types(chain.src, chain.len, input_path)) < 0) goto chain_cleanup;
    if (cc_pass_chain_apply(&chain, cc__rewrite_slice_types(chain.src, chain.len, input_path)) < 0) goto chain_cleanup;
    if (cc_pass_chain_apply(&chain, cc_rewrite_generic_containers(chain.src, chain.len, input_path)) < 0) goto chain_cleanup;
    if (cc_pass_chain_apply(&chain, cc__expand_header_vec_macros(chain.src, chain.len)) < 0) goto chain_cleanup;
    g_ufcs_header_lowering = 1;
    if (cc_pass_chain_apply(&chain, cc_rewrite_generic_family_ufcs_parser_safe(chain.src, chain.len, input_path)) < 0) {
        g_ufcs_header_lowering = 0;
        goto chain_cleanup;
    }
    g_ufcs_header_lowering = 0;

    if (chain.src != src) out = strdup(chain.src);

chain_cleanup:
    g_ufcs_header_lowering = 0;
    g_ufcs_header_path[0] = 0;
    cc_pass_chain_free(&chain);
    return out;
}

char* cc_relower_cc_type_syntax_preserving_registry(const char* src,
                                                    size_t input_len,
                                                    const char* input_path) {
    CCPassChain chain;
    char* out = NULL;
    if (!src || input_len == 0) return NULL;
    /* Presence gate: same probe as build_parse_input's post-CPP path. */
    if (!memmem(src, input_len, "[~", 2) &&
        !memmem(src, input_len, "[:", 2) &&
        !memmem(src, input_len, "::[", 3) &&
        !cc_contains_token_top_level(src, input_len, "@string") &&
        !cc_contains_token_top_level(src, input_len, "@slice"))
        return NULL;

    /* DELIBERATELY do NOT touch the type registry. Caller has already gone
     * through phase-1+phase-3 and may have populated registries (Result,
     * Vec, Map, channel) that downstream passes depend on. We only run the
     * inner text rewrites to mop up CC syntax produced post-preprocess
     * (e.g. via cc_cpp_expand of a #define body). */
    cc_pass_chain_init(&chain, src, input_len);
    if (cc_pass_chain_apply(&chain, cc__normalize_template_recv_chains(chain.src, chain.len)) < 0) goto chain_cleanup;
    if (cc_pass_chain_apply(&chain, cc__rewrite_string_templates(chain.src, chain.len, input_path)) < 0) goto chain_cleanup;
    if (cc_pass_chain_apply(&chain, cc__rewrite_chan_handle_types(chain.src, chain.len, input_path)) < 0) goto chain_cleanup;
    if (cc_pass_chain_apply(&chain, cc__rewrite_slice_types(chain.src, chain.len, input_path)) < 0) goto chain_cleanup;
    if (cc_pass_chain_apply(&chain, cc_rewrite_generic_containers(chain.src, chain.len, input_path)) < 0) goto chain_cleanup;

    if (chain.src != src) out = strdup(chain.src);

chain_cleanup:
    cc_pass_chain_free(&chain);
    return out;
}

/* Process-local memo + on-disk cache for include-expanded source.
 *
 * `cc_preprocess_include_expanded` shells out to `cc -E` (~40ms + ~600KB on a
 * typical prelude TU). Within one process the same path is expanded at most
 * once (parse stashes the buffer; later passes reuse it). Across processes —
 * every cold `--no-cache` emit — a disk cache keyed by input/toolchain mtimes
 * plus `#line` dependency freshness skips the subprocess entirely.
 *
 * Disable with CC_INCEXP_NO_CACHE=1. Independent of the driver emit cache
 * (`--no-cache` / CC_NO_CACHE): those force re-emit, not re-expand. */
static char* cc__incexp_cache_path = NULL;
static char* cc__incexp_cache_buf = NULL;

static uint64_t cc__incexp_fnv64(const void* data, size_t n, uint64_t seed) {
    const uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
    const uint64_t FNV_PRIME = 0x100000001b3ULL;
    uint64_t h = seed ? seed : FNV_OFFSET;
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= FNV_PRIME; }
    return h;
}

static int cc__incexp_disk_disabled(void) {
    const char* e = getenv("CC_INCEXP_NO_CACHE");
    return (e && e[0] == '1');
}

static int cc__incexp_mkdir_p(const char* path) {
    char buf[1024];
    size_t len = path ? strlen(path) : 0;
    if (len == 0 || len + 1 >= sizeof(buf)) return -1;
    memcpy(buf, path, len + 1);
    for (size_t i = 1; i < len; ++i) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
            buf[i] = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int cc__incexp_cache_dir(char* out, size_t out_sz) {
    const char* home = getenv("HOME");
    const char* tmp = getenv("TMPDIR");
    int n;
    if (home && home[0]) {
        n = snprintf(out, out_sz, "%s/.cache/concurrent-c/incexp", home);
    } else if (tmp && tmp[0]) {
        n = snprintf(out, out_sz, "%s/cc-incexp", tmp);
    } else {
        n = snprintf(out, out_sz, "/tmp/cc-incexp");
    }
    if (n < 0 || (size_t)n >= out_sz) return -1;
    return cc__incexp_mkdir_p(out);
}

static uint64_t cc__incexp_fold_file_sig(uint64_t h, const char* path) {
    struct stat st;
    if (!path || !path[0] || stat(path, &st) != 0) {
        h = cc__incexp_fnv64("|miss=", 6, h);
        h = cc__incexp_fnv64(path ? path : "", path ? strlen(path) : 0, h);
        return h;
    }
    h = cc__incexp_fnv64(path, strlen(path), h);
    h = cc__incexp_fnv64(&st.st_mtime, sizeof(st.st_mtime), h);
    h = cc__incexp_fnv64(&st.st_size, sizeof(st.st_size), h);
    return h;
}

static uint64_t cc__incexp_disk_key(const char* input_path, const char* repo_root) {
    uint64_t h = 0;
    const char* cc_bin = getenv("CC");
    if (!cc_bin || !cc_bin[0]) cc_bin = "/usr/bin/cc";
    h = cc__incexp_fold_file_sig(h, input_path);
    h = cc__incexp_fold_file_sig(h, cc_bin);
    if (repo_root && repo_root[0]) {
        char ccc_bin[1100];
        snprintf(ccc_bin, sizeof(ccc_bin), "%s/cc/bin/.ccc-bin", repo_root);
        h = cc__incexp_fold_file_sig(h, ccc_bin);
        h = cc__incexp_fnv64("|inc=", 5, h);
        h = cc__incexp_fnv64(repo_root, strlen(repo_root), h);
    }
    /* Flag bits that change expand command / grammar splice behavior. */
    h = cc__incexp_fnv64("|v=3|incexp", 12, h);
    return h;
}

/* Verify a deps sidecar: each line is "mtime_sec\tsize\tpath". */
static int cc__incexp_deps_fresh(const char* deps_path) {
    FILE* f = fopen(deps_path, "r");
    char line[2048];
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        long long mtime = 0, size = 0;
        char path[1600];
        struct stat st;
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;
        if (sscanf(line, "%lld\t%lld\t%1599[^\n]", &mtime, &size, path) != 3) {
            fclose(f);
            return 0;
        }
        if (stat(path, &st) != 0 ||
            (long long)st.st_mtime != mtime ||
            (long long)st.st_size != size) {
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return 1;
}

static char* cc__incexp_disk_load(const char* cache_dir, uint64_t key, size_t* out_len) {
    char body_path[1280];
    char deps_path[1280];
    FILE* f = NULL;
    char* buf = NULL;
    long sz = 0;
    if (!cache_dir || !cache_dir[0]) return NULL;
    snprintf(body_path, sizeof(body_path), "%s/%016llx.i", cache_dir, (unsigned long long)key);
    snprintf(deps_path, sizeof(deps_path), "%s/%016llx.deps", cache_dir, (unsigned long long)key);
    if (!cc__incexp_deps_fresh(deps_path)) return NULL;
    f = fopen(body_path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0 || sz > (32L << 20)) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    buf[sz] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

/* Record input + unique `# N "path"` / `#line N "path"` markers as deps. */
static void cc__incexp_disk_store(const char* cache_dir, uint64_t key,
                                  const char* input_path,
                                  const char* buf, size_t len) {
    char body_path[1280];
    char deps_path[1280];
    char tmp_body[1300];
    char tmp_deps[1300];
    FILE* bf = NULL;
    FILE* df = NULL;
    char* paths[256];
    size_t path_n = 0;
    if (!cache_dir || !cache_dir[0] || !buf) return;

    snprintf(body_path, sizeof(body_path), "%s/%016llx.i", cache_dir, (unsigned long long)key);
    snprintf(deps_path, sizeof(deps_path), "%s/%016llx.deps", cache_dir, (unsigned long long)key);
    snprintf(tmp_body, sizeof(tmp_body), "%s.tmp", body_path);
    snprintf(tmp_deps, sizeof(tmp_deps), "%s.tmp", deps_path);

    /* Collect dependency paths (input first). */
    paths[path_n++] = (char*)input_path;
    {
        size_t i = 0;
        while (i < len) {
            size_t ls = i;
            while (i < len && buf[i] != '\n') i++;
            /* cpp markers are `# <num> "path"` or `#line <num> "path"` at bol */
            if (ls < len && buf[ls] == '#') {
                size_t p = ls + 1;
                while (p < i && (buf[p] == ' ' || buf[p] == '\t')) p++;
                if (p + 4 <= i && memcmp(buf + p, "line", 4) == 0) {
                    p += 4;
                    while (p < i && (buf[p] == ' ' || buf[p] == '\t')) p++;
                }
                if (p < i && buf[p] >= '0' && buf[p] <= '9') {
                    while (p < i && buf[p] >= '0' && buf[p] <= '9') p++;
                    while (p < i && (buf[p] == ' ' || buf[p] == '\t')) p++;
                    if (p < i && buf[p] == '"') {
                        size_t q = p + 1;
                        while (q < i && buf[q] != '"') q++;
                        if (q < i && q > p + 1 && path_n < (sizeof(paths) / sizeof(paths[0]))) {
                            size_t pl = q - (p + 1);
                            char* copy = (char*)malloc(pl + 1);
                            if (copy) {
                                memcpy(copy, buf + p + 1, pl);
                                copy[pl] = '\0';
                                int dup = 0;
                                for (size_t k = 0; k < path_n; ++k) {
                                    if (strcmp(paths[k], copy) == 0) { dup = 1; break; }
                                }
                                if (dup) free(copy);
                                else paths[path_n++] = copy;
                            }
                        }
                    }
                }
            }
            if (i < len) i++;
        }
    }

    bf = fopen(tmp_body, "wb");
    df = fopen(tmp_deps, "w");
    if (bf && df && fwrite(buf, 1, len, bf) == len) {
        for (size_t k = 0; k < path_n; ++k) {
            struct stat st;
            if (!paths[k] || stat(paths[k], &st) != 0) continue;
            fprintf(df, "%lld\t%lld\t%s\n",
                    (long long)st.st_mtime, (long long)st.st_size, paths[k]);
        }
        fclose(bf); bf = NULL;
        fclose(df); df = NULL;
        if (rename(tmp_body, body_path) != 0) unlink(tmp_body);
        if (rename(tmp_deps, deps_path) != 0) unlink(tmp_deps);
    } else {
        if (bf) fclose(bf);
        if (df) fclose(df);
        unlink(tmp_body);
        unlink(tmp_deps);
    }
    for (size_t k = 1; k < path_n; ++k) free(paths[k]);

    /* Nothing else ever deletes an entry: the key folds in the input's mtime,
     * so an edited file mints a new one and orphans the old. */
    (void)cc_cache_evict(cache_dir, 1024ULL * 1024ULL * 1024ULL);
}

char* cc_preprocess_include_expanded(const char* input_path) {
    char repo_root[1024];
    char cmd[4096];
    FILE* pp = NULL;
    char* buf = NULL;
    size_t len = 0;
    size_t cap = 64 * 1024;
    char tmp_grammar_path[128];
    const char* expand_path;
    char disk_dir[1024];
    uint64_t disk_key = 0;
    int disk_ok = 0;
    if (!input_path || !input_path[0]) return NULL;
    if (cc__incexp_cache_buf && cc__incexp_cache_path &&
        strcmp(cc__incexp_cache_path, input_path) == 0) {
        return strdup(cc__incexp_cache_buf);
    }

    repo_root[0] = '\0';
    (void)cc_path_find_repo_root(input_path, repo_root, sizeof(repo_root));
    disk_dir[0] = '\0';
    if (!cc__incexp_disk_disabled() && cc__incexp_cache_dir(disk_dir, sizeof(disk_dir)) == 0) {
        disk_key = cc__incexp_disk_key(input_path, repo_root);
        disk_ok = 1;
        {
            size_t cached_len = 0;
            char* cached = cc__incexp_disk_load(disk_dir, disk_key, &cached_len);
            if (cached) {
                char* dup = strdup(cached);
                if (dup) {
                    free(cc__incexp_cache_path);
                    free(cc__incexp_cache_buf);
                    cc__incexp_cache_path = strdup(input_path);
                    if (cc__incexp_cache_path) {
                        cc__incexp_cache_buf = cached;
                        return dup;
                    }
                    free(dup);
                }
                free(cached);
            }
        }
    }
    /* @grammar bodies are raw non-C bytes behind a fence; the system cpp
     * would eat any `#`-leading line inside them as a (bad) directive —
     * silently, since stderr is dropped.  Splice grammar decls FIRST so the
     * preprocessor only ever sees the generated C, and this expanded view
     * carries the generated types like every other stream. */
    tmp_grammar_path[0] = '\0';
    expand_path = input_path;
    {
        char* raw = NULL;
        size_t raw_len = 0;
        if (cc__read_file_text(input_path, &raw, &raw_len) == 0 && raw) {
            /* .shcc entry wrap before grammar splice / include-expand so
             * comptime registration sees the auto-prelude. */
            {
                size_t script_len = 0;
                char* script = cc_script_rewrite_source(input_path, raw, raw_len, &script_len);
                if (script) {
                    free(raw);
                    raw = script;
                    raw_len = script_len;
                }
            }
            char* spliced = cc_rewrite_grammar_decls_text(raw, raw_len, input_path);
            const char* expand_src = raw;
            size_t expand_len = raw_len;
            if (spliced && spliced != (char*)-1) {
                expand_src = spliced;
                expand_len = strlen(spliced);
            }
            if (expand_src != raw || cc_path_is_shcc(input_path)) {
                snprintf(tmp_grammar_path, sizeof(tmp_grammar_path), "/tmp/cc_pp_gram_XXXXXX");
                int fd = mkstemp(tmp_grammar_path);
                if (fd >= 0) {
                    FILE* tf = fdopen(fd, "wb");
                    if (tf) {
                        if (fwrite(expand_src, 1, expand_len, tf) == expand_len)
                            expand_path = tmp_grammar_path;
                        fclose(tf);
                    } else {
                        close(fd);
                    }
                    if (expand_path != tmp_grammar_path) {
                        unlink(tmp_grammar_path);
                        tmp_grammar_path[0] = '\0';
                    }
                } else {
                    tmp_grammar_path[0] = '\0';
                }
            }
            /* (char*)-1 = malformed decl: fall through and expand the raw
             * file; the primary parse path reports the diagnostic. */
            if (spliced && spliced != (char*)-1) free(spliced);
            free(raw);
        }
    }
    /* When expanding the grammar-spliced TEMP copy, quoted includes
     * (`#include "local.cch"`) must still resolve relative to the ORIGINAL
     * file's directory — the temp lives in /tmp. */
    {
        char inc_dir[1024];
        inc_dir[0] = '\0';
        if (expand_path != input_path) {
            const char* slash = strrchr(input_path, '/');
            if (slash && (size_t)(slash - input_path) < sizeof(inc_dir)) {
                size_t dl = (size_t)(slash - input_path);
                memcpy(inc_dir, input_path, dl);
                inc_dir[dl] = '\0';
            }
        }
        if (repo_root[0] || cc_path_find_repo_root(input_path, repo_root, sizeof(repo_root))) {
            snprintf(cmd, sizeof(cmd),
                     "cc -E -D__CC__=1 -x c%s%s%s -I\"%s/cc/include\" -I\"%s/out/include\" \"%s\" 2>/dev/null",
                     inc_dir[0] ? " -iquote\"" : "", inc_dir[0] ? inc_dir : "", inc_dir[0] ? "\"" : "",
                     repo_root, repo_root, expand_path);
        } else {
            snprintf(cmd, sizeof(cmd),
                     "cc -E -D__CC__=1 -x c%s%s%s \"%s\" 2>/dev/null",
                     inc_dir[0] ? " -iquote\"" : "", inc_dir[0] ? inc_dir : "", inc_dir[0] ? "\"" : "",
                     expand_path);
        }
    }
    pp = popen(cmd, "r");
    if (!pp) {
        if (tmp_grammar_path[0]) unlink(tmp_grammar_path);
        return NULL;
    }
    buf = (char*)malloc(cap);
    if (!buf) {
        pclose(pp);
        if (tmp_grammar_path[0]) unlink(tmp_grammar_path);
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
                if (tmp_grammar_path[0]) unlink(tmp_grammar_path);
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
            if (tmp_grammar_path[0]) unlink(tmp_grammar_path);
            return NULL;
        }
    }
    buf[len] = '\0';
    pclose(pp);
    if (tmp_grammar_path[0]) {
        unlink(tmp_grammar_path);
        /* Point cpp linemarkers for the spliced temp back at the real file so
         * downstream #line-aware diagnostics keep their provenance. */
        {
            char* fixed = NULL; size_t flen = 0, fcap = 0;
            size_t tl = strlen(tmp_grammar_path), il = strlen(input_path);
            const char* s = buf; int any = 0;
            for (;;) {
                const char* hit = strstr(s, tmp_grammar_path);
                if (!hit) break;
                cc_sb_append(&fixed, &flen, &fcap, s, (size_t)(hit - s));
                cc_sb_append(&fixed, &flen, &fcap, input_path, il);
                s = hit + tl;
                any = 1;
            }
            if (any) {
                cc_sb_append(&fixed, &flen, &fcap, s, strlen(s) + 1);
                free(buf);
                buf = fixed;
                len = flen - 1;
            } else {
                free(fixed);
            }
        }
    }
    if (disk_ok) {
        cc__incexp_disk_store(disk_dir, disk_key, input_path, buf, len);
    }
    /* Process-local memo: hand the caller an independent copy so its free()
     * never touches the cache master. */
    {
        char* dup = strdup(buf);
        if (dup) {
            free(cc__incexp_cache_path);
            free(cc__incexp_cache_buf);
            cc__incexp_cache_path = strdup(input_path);
            if (cc__incexp_cache_path) {
                cc__incexp_cache_buf = buf;   /* master retained */
                return dup;                   /* caller owns dup */
            }
            /* strdup(input_path) failed: don't cache, return buf directly. */
            cc__incexp_cache_buf = NULL;
            free(dup);
        }
    }
    return buf;
}

/* D1.0 — constexpr `type_of(T)` view (numeric/layout members).
 *
 * Folds `type_of(T).size` -> `sizeof(T)` and `type_of(T).align` -> `_Alignof(T)`
 * (both wrapped in a `(size_t)` cast to match the accessor return type).  These
 * are genuine C integer constant expressions, so they work in `static_assert`,
 * array dimensions, and `@comptime if` — and the backend, not a comptime VM,
 * computes the actual layout number (the "(1) layout out of scope" decision).
 *
 * The bare value form `type_of(T)` is left untouched: it keeps lowering through
 * the `type_of(T)` macro (`cc_type_of(#T)` -> runtime `const cc_type_info*`),
 * so runtime introspection and pointer identity are unchanged.  Only the
 * member-access shape `type_of(T).<member>` is recognized here; `type_of(T)->m`
 * (pointer deref, runtime) is deliberately NOT matched.
 *
 * Structural members (`.kind`, `.nfields`, `.name`, `.fields[...]`) are a later
 * D1 increment that reads the type graph; only numeric layout lands here.
 * Runs in `cc__apply_phase1_canonical_passes` (parse path) AND on the
 * shadow_lower emit path (the product `.c` is produced there).
 *
 * D1.1 adds the *structural* members the compiler can decide by name:
 *   - `.name`    -> `"T"` (constexpr string literal; the display spelling)
 *   - `.kind`    -> `CC_TK_PRIMITIVE` / `CC_TK_GENERIC_INST` for the reserved
 *                   primitive + container name sets (constexpr enum constant),
 *                   else the runtime `cc_type_of("T")->kind` read
 *   - `.nfields` -> `0` for primitives (constexpr), else runtime read
 * Members the compiler can't decide structurally fall back to the same runtime
 * `cc_type_of(#T)->member` the bare macro already produces — so every
 * `type_of(T).<member>` at least compiles, and the known cases are ICEs. */
static int cc__ti_name_is_primitive(const char* s, size_t n) {
    static const char* const prims[] = {
        "int", "char", "short", "long", "float",
        "double", "size_t", "intptr_t", "bool",
    };
    for (size_t k = 0; k < sizeof(prims) / sizeof(prims[0]); k++)
        if (strlen(prims[k]) == n && memcmp(prims[k], s, n) == 0) return 1;
    return 0;
}
static int cc__ti_name_is_container(const char* s, size_t n) {
    static const char* const pre[] = { "CCVec_", "ArrayMap_", "Map_", "CCChan" };
    for (size_t k = 0; k < sizeof(pre) / sizeof(pre[0]); k++) {
        size_t pl = strlen(pre[k]);
        if (n >= pl && memcmp(s, pre[k], pl) == 0) return 1;
    }
    return 0;
}

char* cc__lower_type_of_constexpr(const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0, last_emit = 0;
    int changed = 0;
    CCScannerState scan;
    static const char KW[] = "type_of";
    const size_t KWN = sizeof(KW) - 1;
    cc_scanner_init(&scan);
    while (i < n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        /* Match `type_of` at an identifier boundary (rejects `cc_type_of`,
         * `type_of_x`, etc.). */
        if (src[i] != 't' || i + KWN > n || memcmp(src + i, KW, KWN) != 0 ||
            (i > 0 && cc_is_ident_char(src[i - 1])) ||
            (i + KWN < n && cc_is_ident_char(src[i + KWN]))) {
            i++;
            continue;
        }
        size_t p = cc_skip_ws_and_comments(src, n, i + KWN);
        if (p >= n || src[p] != '(') { i++; continue; }
        p = cc_skip_ws_and_comments(src, n, p + 1);
        size_t id0 = p;
        while (p < n && cc_is_ident_char(src[p])) p++;
        size_t idn = p - id0;
        if (idn == 0) { i++; continue; }
        size_t q = cc_skip_ws_and_comments(src, n, p);
        if (q >= n || src[q] != ')') { i++; continue; }   /* bare IDENT only */
        q = cc_skip_ws_and_comments(src, n, q + 1);
        if (q >= n || src[q] != '.') { i++; continue; }    /* value/`->` forms untouched */
        size_t m = cc_skip_ws_and_comments(src, n, q + 1);
        size_t mem0 = m;
        while (m < n && cc_is_ident_char(src[m])) m++;
        size_t memn = m - mem0;
        const char* mem = src + mem0;
        const char* T = src + id0;          /* the bare type identifier */
        size_t Tn = idn;
        enum { MK_NONE, MK_SIZE, MK_ALIGN, MK_NAME, MK_KIND, MK_NFIELDS } which = MK_NONE;
        if (memn == 4 && memcmp(mem, "size", 4) == 0)        which = MK_SIZE;
        else if (memn == 5 && memcmp(mem, "align", 5) == 0)  which = MK_ALIGN;
        else if (memn == 4 && memcmp(mem, "name", 4) == 0)   which = MK_NAME;
        else if (memn == 4 && memcmp(mem, "kind", 4) == 0)   which = MK_KIND;
        else if (memn == 7 && memcmp(mem, "nfields", 7) == 0) which = MK_NFIELDS;
        if (which == MK_NONE) { i++; continue; }  /* leave other members alone */

        int is_prim = cc__ti_name_is_primitive(T, Tn);
        int is_cont = cc__ti_name_is_container(T, Tn);

        /* Emit prefix, then the folded/forwarded replacement for this member. */
        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
        switch (which) {
        case MK_SIZE:
            cc_sb_append_cstr(&out, &out_len, &out_cap, "((size_t)sizeof(");
            cc_sb_append(&out, &out_len, &out_cap, T, Tn);
            cc_sb_append_cstr(&out, &out_len, &out_cap, "))");
            break;
        case MK_ALIGN:
            cc_sb_append_cstr(&out, &out_len, &out_cap, "((size_t)_Alignof(");
            cc_sb_append(&out, &out_len, &out_cap, T, Tn);
            cc_sb_append_cstr(&out, &out_len, &out_cap, "))");
            break;
        case MK_NAME:
            cc_sb_append_cstr(&out, &out_len, &out_cap, "\"");
            cc_sb_append(&out, &out_len, &out_cap, T, Tn);
            cc_sb_append_cstr(&out, &out_len, &out_cap, "\"");
            break;
        case MK_KIND:
            if (is_prim) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "(CC_TK_PRIMITIVE)");
            } else if (is_cont) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "(CC_TK_GENERIC_INST)");
            } else {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "(cc_type_of(\"");
                cc_sb_append(&out, &out_len, &out_cap, T, Tn);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "\")->kind)");
            }
            break;
        case MK_NFIELDS:
            if (is_prim) {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "((size_t)0)");
            } else {
                cc_sb_append_cstr(&out, &out_len, &out_cap, "((size_t)cc_type_of(\"");
                cc_sb_append(&out, &out_len, &out_cap, T, Tn);
                cc_sb_append_cstr(&out, &out_len, &out_cap, "\")->nfields)");
            }
            break;
        default: break;
        }
        last_emit = m;
        i = m;
        changed = 1;
    }
    if (!changed) { free(out); return NULL; }
    if (last_emit < n)
        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* ===== D2.0 — `@comptime if (PRED) { ... } [else { ... }]` ===================
 *
 * Compile-time conditional code *selection*.  PRED is a self-contained integer
 * constant expression the compiler decides statically:
 *   - integer / char literals
 *   - the `CC_TK_*` kind constants and `true`/`false`
 *   - the structural `type_of(T).kind` / `type_of(T).nfields` views (classified
 *     by name, exactly as D1.1) for the reserved primitive + container sets
 *   - the full C operator set (`! ~ - +  * / %  + -  << >>  < <= > >=  == !=
 *     &  ^  |  &&  ||`) and parentheses
 * Anything the compiler can't fold statically is a *hard error*, never a silent
 * `false`: `sizeof`/`_Alignof`/`type_of(T).size` (layout — deliberately out of
 * scope, see the D1 "layout out of scope" decision), `cc_type_of(...)` runtime
 * reads, unknown identifiers, casts, and calls.
 *
 * The taken branch body is spliced verbatim where the construct began; the rest
 * is dropped, with newline-only padding so line numbers stay stable.  Runs as
 * the *first* canonical pass (and the matching emit slot) so a dead branch is
 * pruned before any other rewrite or instantiation collector sees it — true
 * conditional compilation, not dead-code emission. */

typedef struct { const char* s; size_t n; size_t i; int ok; } CCCEval;

static long cc__ce_expr(CCCEval* e);

static void cc__ce_ws(CCCEval* e) { e->i = cc_skip_ws_and_comments(e->s, e->n, e->i); }

static int cc__ce_kw_value(const char* s, size_t n, long* out) {
    /* Mirror of `cc_type_kind` in include/ccc/cc_type.cch — keep in sync. */
    static const struct { const char* k; long v; } tab[] = {
        { "CC_TK_UNKNOWN", 0 }, { "CC_TK_PRIMITIVE", 1 }, { "CC_TK_POINTER", 2 },
        { "CC_TK_STRUCT", 3 }, { "CC_TK_GENERIC_INST", 4 }, { "CC_TK_CLOSURE", 5 },
        { "CC_TK_FUNCTION", 6 }, { "CC_TK_ARRAY", 7 }, { "true", 1 }, { "false", 0 },
    };
    for (size_t k = 0; k < sizeof(tab) / sizeof(tab[0]); k++)
        if (strlen(tab[k].k) == n && memcmp(tab[k].k, s, n) == 0) { *out = tab[k].v; return 1; }
    return 0;
}

/* `type_of` already consumed; parse `(IDENT).member` and fold structurally. */
static long cc__ce_type_of_member(CCCEval* e) {
    cc__ce_ws(e);
    if (e->i >= e->n || e->s[e->i] != '(') { e->ok = 0; return 0; }
    e->i++; cc__ce_ws(e);
    size_t t0 = e->i;
    while (e->i < e->n && cc_is_ident_char(e->s[e->i])) e->i++;
    size_t tn = e->i - t0;
    if (tn == 0) { e->ok = 0; return 0; }
    cc__ce_ws(e);
    if (e->i >= e->n || e->s[e->i] != ')') { e->ok = 0; return 0; }
    e->i++; cc__ce_ws(e);
    if (e->i >= e->n || e->s[e->i] != '.') { e->ok = 0; return 0; }
    e->i++; cc__ce_ws(e);
    size_t m0 = e->i;
    while (e->i < e->n && cc_is_ident_char(e->s[e->i])) e->i++;
    size_t mn = e->i - m0;
    const char* T = e->s + t0;
    const char* M = e->s + m0;
    if (mn == 4 && memcmp(M, "kind", 4) == 0) {
        if (cc__ti_name_is_primitive(T, tn)) return 1; /* CC_TK_PRIMITIVE */
        if (cc__ti_name_is_container(T, tn)) return 4; /* CC_TK_GENERIC_INST */
        e->ok = 0; return 0;                           /* not statically decidable */
    }
    if (mn == 7 && memcmp(M, "nfields", 7) == 0) {
        if (cc__ti_name_is_primitive(T, tn)) return 0;
        e->ok = 0; return 0;
    }
    e->ok = 0; return 0; /* .size/.align/.name -> layout/string, out of scope */
}

static long cc__ce_primary(CCCEval* e) {
    cc__ce_ws(e);
    if (e->i >= e->n) { e->ok = 0; return 0; }
    char c = e->s[e->i];
    if (c == '(') {
        e->i++;
        long v = cc__ce_expr(e);
        cc__ce_ws(e);
        if (e->i >= e->n || e->s[e->i] != ')') { e->ok = 0; return 0; }
        e->i++;
        return v;
    }
    if (c >= '0' && c <= '9') {
        long v = 0;
        int base = 10;
        if (c == '0' && e->i + 1 < e->n && (e->s[e->i + 1] == 'x' || e->s[e->i + 1] == 'X')) {
            base = 16; e->i += 2;
        } else if (c == '0') {
            base = 8;
        }
        size_t st = e->i;
        while (e->i < e->n) {
            char d = e->s[e->i];
            int dv;
            if (d >= '0' && d <= '9') dv = d - '0';
            else if (base == 16 && d >= 'a' && d <= 'f') dv = 10 + (d - 'a');
            else if (base == 16 && d >= 'A' && d <= 'F') dv = 10 + (d - 'A');
            else break;
            if (dv >= base) break;
            v = v * base + dv;
            e->i++;
        }
        if (e->i == st && base != 8) { e->ok = 0; return 0; }
        while (e->i < e->n && (e->s[e->i] == 'u' || e->s[e->i] == 'U' ||
                               e->s[e->i] == 'l' || e->s[e->i] == 'L')) e->i++;
        return v;
    }
    if (c == '\'') {
        e->i++;
        long v;
        if (e->i < e->n && e->s[e->i] == '\\') {
            e->i++;
            if (e->i >= e->n) { e->ok = 0; return 0; }
            char esc = e->s[e->i++];
            switch (esc) {
            case 'n': v = '\n'; break; case 't': v = '\t'; break;
            case 'r': v = '\r'; break; case '0': v = '\0'; break;
            case '\\': v = '\\'; break; case '\'': v = '\''; break;
            default: v = (unsigned char)esc; break;
            }
        } else if (e->i < e->n) {
            v = (unsigned char)e->s[e->i++];
        } else { e->ok = 0; return 0; }
        if (e->i >= e->n || e->s[e->i] != '\'') { e->ok = 0; return 0; }
        e->i++;
        return v;
    }
    if (cc_is_ident_char(c)) {
        size_t id0 = e->i;
        while (e->i < e->n && cc_is_ident_char(e->s[e->i])) e->i++;
        size_t idn = e->i - id0;
        if (idn == 7 && memcmp(e->s + id0, "type_of", 7) == 0)
            return cc__ce_type_of_member(e);
        long kv;
        if (cc__ce_kw_value(e->s + id0, idn, &kv)) return kv;
        e->ok = 0; return 0; /* unknown identifier */
    }
    e->ok = 0; return 0;
}

static long cc__ce_unary(CCCEval* e) {
    cc__ce_ws(e);
    if (e->i < e->n) {
        char c = e->s[e->i];
        if (c == '!' || c == '~' || c == '-' || c == '+') {
            e->i++;
            long v = cc__ce_unary(e);
            switch (c) {
            case '!': return !v;
            case '~': return ~v;
            case '-': return -v;
            default:  return v;
            }
        }
    }
    return cc__ce_primary(e);
}

static long cc__ce_mul(CCCEval* e) {
    long v = cc__ce_unary(e);
    for (;;) {
        cc__ce_ws(e);
        if (e->i >= e->n) break;
        char c = e->s[e->i];
        if (c == '*') { e->i++; v = v * cc__ce_unary(e); }
        else if (c == '/') { e->i++; long r = cc__ce_unary(e); if (r == 0) { e->ok = 0; return 0; } v = v / r; }
        else if (c == '%') { e->i++; long r = cc__ce_unary(e); if (r == 0) { e->ok = 0; return 0; } v = v % r; }
        else break;
    }
    return v;
}

static long cc__ce_add(CCCEval* e) {
    long v = cc__ce_mul(e);
    for (;;) {
        cc__ce_ws(e);
        if (e->i >= e->n) break;
        char c = e->s[e->i];
        if (c == '+') { e->i++; v = v + cc__ce_mul(e); }
        else if (c == '-') { e->i++; v = v - cc__ce_mul(e); }
        else break;
    }
    return v;
}

static long cc__ce_shift(CCCEval* e) {
    long v = cc__ce_add(e);
    for (;;) {
        cc__ce_ws(e);
        if (e->i + 1 >= e->n) break;
        if (e->s[e->i] == '<' && e->s[e->i + 1] == '<') { e->i += 2; v = v << cc__ce_add(e); }
        else if (e->s[e->i] == '>' && e->s[e->i + 1] == '>') { e->i += 2; v = v >> cc__ce_add(e); }
        else break;
    }
    return v;
}

static long cc__ce_rel(CCCEval* e) {
    long v = cc__ce_shift(e);
    for (;;) {
        cc__ce_ws(e);
        if (e->i >= e->n) break;
        char c = e->s[e->i];
        char d = (e->i + 1 < e->n) ? e->s[e->i + 1] : 0;
        if (c == '<' && d == '=') { e->i += 2; v = (v <= cc__ce_shift(e)); }
        else if (c == '>' && d == '=') { e->i += 2; v = (v >= cc__ce_shift(e)); }
        else if (c == '<' && d != '<') { e->i++; v = (v < cc__ce_shift(e)); }
        else if (c == '>' && d != '>') { e->i++; v = (v > cc__ce_shift(e)); }
        else break;
    }
    return v;
}

static long cc__ce_eq(CCCEval* e) {
    long v = cc__ce_rel(e);
    for (;;) {
        cc__ce_ws(e);
        if (e->i + 1 >= e->n) break;
        char c = e->s[e->i], d = e->s[e->i + 1];
        if (c == '=' && d == '=') { e->i += 2; v = (v == cc__ce_rel(e)); }
        else if (c == '!' && d == '=') { e->i += 2; v = (v != cc__ce_rel(e)); }
        else break;
    }
    return v;
}

static long cc__ce_band(CCCEval* e) {
    long v = cc__ce_eq(e);
    for (;;) {
        cc__ce_ws(e);
        if (e->i >= e->n) break;
        char d = (e->i + 1 < e->n) ? e->s[e->i + 1] : 0;
        if (e->s[e->i] == '&' && d != '&') { e->i++; v = v & cc__ce_eq(e); }
        else break;
    }
    return v;
}

static long cc__ce_bxor(CCCEval* e) {
    long v = cc__ce_band(e);
    for (;;) {
        cc__ce_ws(e);
        if (e->i >= e->n) break;
        if (e->s[e->i] == '^') { e->i++; v = v ^ cc__ce_band(e); }
        else break;
    }
    return v;
}

static long cc__ce_bor(CCCEval* e) {
    long v = cc__ce_bxor(e);
    for (;;) {
        cc__ce_ws(e);
        if (e->i >= e->n) break;
        char d = (e->i + 1 < e->n) ? e->s[e->i + 1] : 0;
        if (e->s[e->i] == '|' && d != '|') { e->i++; v = v | cc__ce_bxor(e); }
        else break;
    }
    return v;
}

/* D2.1 short-circuit support: advance past one logical operand *without*
 * requiring it to fold.  Stops at a depth-0 `||` (always) or `&&` (when
 * `stop_andand`), at a closing `)`/`]` of an enclosing group, or at end of
 * input.  Honors nested parens/brackets and string/char literals.  This lets
 * `KNOWN_TRUE || <non-foldable>` and `KNOWN_FALSE && <non-foldable>` be decided
 * without the dead operand having to be compile-time constant. */
static void cc__ce_skip_balanced(CCCEval* e, int stop_andand) {
    int depth = 0;
    for (;;) {
        cc__ce_ws(e);
        if (e->i >= e->n) break;
        char c = e->s[e->i];
        char d = (e->i + 1 < e->n) ? e->s[e->i + 1] : 0;
        if (depth == 0) {
            if (c == '|' && d == '|') break;
            if (stop_andand && c == '&' && d == '&') break;
            if (c == ')' || c == ']') break;
        }
        if (c == '(' || c == '[') { depth++; e->i++; continue; }
        if (c == ')' || c == ']') { depth--; e->i++; continue; }
        if (c == '"' || c == '\'') {
            char q = c;
            e->i++;
            while (e->i < e->n && e->s[e->i] != q) {
                if (e->s[e->i] == '\\' && e->i + 1 < e->n) e->i++;
                e->i++;
            }
            if (e->i < e->n) e->i++;
            continue;
        }
        e->i++;
    }
    if (depth != 0) e->ok = 0; /* unbalanced -> not decidable */
}

static long cc__ce_land(CCCEval* e) {
    long v = cc__ce_bor(e);
    for (;;) {
        cc__ce_ws(e);
        if (e->i + 1 >= e->n || !(e->s[e->i] == '&' && e->s[e->i + 1] == '&')) break;
        e->i += 2;
        if (e->ok && !v) {
            cc__ce_skip_balanced(e, /*stop_andand=*/1); /* result already false */
        } else {
            long r = cc__ce_bor(e);
            v = (v && r);
        }
    }
    return v;
}

static long cc__ce_lor(CCCEval* e) {
    long v = cc__ce_land(e);
    for (;;) {
        cc__ce_ws(e);
        if (e->i + 1 >= e->n || !(e->s[e->i] == '|' && e->s[e->i + 1] == '|')) break;
        e->i += 2;
        if (e->ok && v) {
            cc__ce_skip_balanced(e, /*stop_andand=*/0); /* result already true */
        } else {
            long r = cc__ce_land(e);
            v = (v || r);
        }
    }
    return v;
}

static long cc__ce_expr(CCCEval* e) { return cc__ce_lor(e); }

/* Evaluate one predicate.  Returns 1 + sets *out on a fully-folded expression;
 * 0 if any token isn't compile-time decidable or there's trailing garbage. */
static int cc__comptime_eval_pred(const char* s, size_t n, long* out) {
    CCCEval e = { s, n, 0, 1 };
    long v = cc__ce_expr(&e);
    cc__ce_ws(&e);
    if (!e.ok || e.i != n) return 0;
    *out = v;
    return 1;
}

/* Append one '\n' to *out for each newline in src[a..b) — keeps line numbers
 * stable when a span (header / dropped branch) is removed. */
static void cc__sb_emit_newlines(char** out, size_t* len, size_t* cap,
                                 const char* src, size_t a, size_t b) {
    for (size_t k = a; k < b; k++)
        if (src[k] == '\n') cc_sb_append_cstr(out, len, cap, "\n");
}

/* D2.1: given `p` at the start of a `@comptime if`, set *end to the index just
 * past the *whole* construct, following any `else { ... }` / `else @comptime if
 * ...` chain (recursively).  Returns 1 on a well-formed construct, 0 otherwise.
 * Used to find what to drop when the head predicate selects a different arm. */
static int cc__ct_if_extent(const char* src, size_t n, size_t p, size_t* end) {
    static const char ATC[] = "@comptime";
    const size_t ATCN = sizeof(ATC) - 1;
    if (p + ATCN > n || memcmp(src + p, ATC, ATCN) != 0) return 0;
    size_t q = cc_skip_ws_and_comments(src, n, p + ATCN);
    if (!(q + 2 <= n && src[q] == 'i' && src[q + 1] == 'f' &&
          (q + 2 >= n || !cc_is_ident_char(src[q + 2])))) return 0;
    size_t lp = cc_skip_ws_and_comments(src, n, q + 2);
    if (lp >= n || src[lp] != '(') return 0;
    size_t pclose;
    if (!cc_find_matching_paren(src, n, lp, &pclose)) return 0;
    size_t tb = cc_skip_ws_and_comments(src, n, pclose + 1);
    if (tb >= n || src[tb] != '{') return 0;
    size_t tbc;
    if (!cc_find_matching_brace(src, n, tb, &tbc)) return 0;
    size_t cur = tbc + 1;
    size_t after = cc_skip_ws_and_comments(src, n, tbc + 1);
    if (after + 4 <= n && memcmp(src + after, "else", 4) == 0 &&
        (after + 4 >= n || !cc_is_ident_char(src[after + 4]))) {
        size_t ep = cc_skip_ws_and_comments(src, n, after + 4);
        if (ep < n && src[ep] == '{') {
            size_t ebc;
            if (!cc_find_matching_brace(src, n, ep, &ebc)) return 0;
            cur = ebc + 1;
        } else {
            size_t nend;
            if (!cc__ct_if_extent(src, n, ep, &nend)) return 0; /* else @comptime if */
            cur = nend;
        }
    }
    *end = cur;
    return 1;
}

/* D3.1b: brace-aware scan to the terminating `;` at the current nesting depth,
 * starting at `from`.  Returns the index just past that `;` (or n if none). */
static size_t cc__span_to_top_semicolon(const char* src, size_t n, size_t from) {
    CCScannerState s;
    cc_scanner_init(&s);
    size_t j = from, depth = 0;
    while (j < n) {
        if (cc_scanner_skip_non_code(&s, src, n, &j)) continue;
        char ch = src[j];
        if (ch == '{') depth++;
        else if (ch == '}') { if (depth) depth--; }
        else if (ch == ';' && depth == 0) return j + 1;
        j++;
    }
    return n;
}

/* D3.1b: a captured type-definition span is unusable as plain-C prelude text if
 * it carries CC-only surface syntax (decorators `@`, generic instantiation
 * `::`).  Such a def is dropped individually so it can't poison layout eval for
 * the rest of the file. */
static int cc__span_has_cc_syntax(const char* src, size_t a, size_t b) {
    for (size_t k = a; k < b; k++) {
        if (src[k] == '@') return 1;
        if (src[k] == ':' && k + 1 < b && src[k + 1] == ':') return 1;
    }
    return 0;
}

/* D3.1b: collect top-level type *definitions* — `typedef ...;` and bare
 * `struct/union/enum [TAG] { ... };` — from CC source so the TCC layout
 * evaluator (D3.0) can resolve `sizeof`/`_Alignof`/`__builtin_offsetof` of
 * user-declared types in `@comptime if` predicates.  Only bare definitions are
 * captured (a trailing declarator means it's a variable like `struct S {..} g;`
 * and is skipped), and CC-tainted spans are dropped individually.  Defs are
 * emitted in source order so in-file dependencies resolve.  Returns a malloc'd
 * string the caller frees (empty if none), or NULL on OOM. */
char* cc_ct_extract_type_decls_prelude(const char* src, size_t n) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);
    size_t i = 0, depth = 0;
    while (i < n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        char c = src[i];
        if (c == '{') { depth++; i++; continue; }
        if (c == '}') { if (depth) depth--; i++; continue; }
        if (depth != 0) { i++; continue; }
        if (i > 0 && cc_is_ident_char(src[i - 1])) { i++; continue; }
        size_t kwlen = 0;
        int is_typedef = 0;
        if (i + 7 <= n && memcmp(src + i, "typedef", 7) == 0 &&
            (i + 7 == n || !cc_is_ident_char(src[i + 7]))) { kwlen = 7; is_typedef = 1; }
        else if (i + 6 <= n && memcmp(src + i, "struct", 6) == 0 &&
                 (i + 6 == n || !cc_is_ident_char(src[i + 6]))) kwlen = 6;
        else if (i + 5 <= n && memcmp(src + i, "union", 5) == 0 &&
                 (i + 5 == n || !cc_is_ident_char(src[i + 5]))) kwlen = 5;
        else if (i + 4 <= n && memcmp(src + i, "enum", 4) == 0 &&
                 (i + 4 == n || !cc_is_ident_char(src[i + 4]))) kwlen = 4;
        if (!kwlen) { i++; continue; }
        size_t start = i, end;
        if (is_typedef) {
            end = cc__span_to_top_semicolon(src, n, i + kwlen);
        } else {
            /* require `[TAG] { ... } ;` (a bare aggregate definition) */
            size_t p = cc_skip_ws_and_comments(src, n, i + kwlen);
            while (p < n && cc_is_ident_char(src[p])) p++;   /* optional tag */
            p = cc_skip_ws_and_comments(src, n, p);
            if (p >= n || src[p] != '{') { i += kwlen; continue; } /* fwd decl */
            size_t bclose;
            if (!cc_find_matching_brace(src, n, p, &bclose)) { i += kwlen; continue; }
            size_t q = cc_skip_ws_and_comments(src, n, bclose + 1);
            if (q >= n || src[q] != ';') { i = bclose + 1; continue; } /* declarator */
            end = q + 1;
        }
        if (!cc__span_has_cc_syntax(src, start, end)) {
            cc_sb_append(&out, &out_len, &out_cap, src + start, end - start);
            cc_sb_append_cstr(&out, &out_len, &out_cap, "\n");
        }
        i = end;
    }
    if (!out) out = strdup("");
    return out;
}

/* D3.1(A): layout-aware fallback for predicates the self-contained D2 evaluator
 * can't decide (anything touching `sizeof`/`_Alignof`/`type_of(T).size` layout
 * or other host-C constant expressions).  Lower the structural `type_of` views
 * via D1 so `type_of(T).size` -> `((size_t)sizeof(T))` etc., then hand the
 * result to the in-process TCC constant evaluator (D3.0), which computes real
 * target-ABI layout numbers.  A small prelude supplies `size_t` and the
 * `CC_TK_*` enum so the lowered text resolves; `type_prelude` (D3.1b) appends
 * the file's in-scope type definitions so user-struct layout resolves too.
 * Predicates that still reference undefined names (e.g. an unclassified type's
 * runtime `kind`, or a struct whose def pulls in header-only types) fail
 * cleanly and fall through to the hard error.  Returns 1 + sets *out on a
 * decided predicate, 0 otherwise. */
static int cc__comptime_eval_pred_via_tcc(const char* pred, size_t n,
                                          const char* type_prelude, long* out) {
    char* work = (char*)malloc(n + 1);
    if (!work) return 0;
    memcpy(work, pred, n);
    work[n] = '\0';
    char* lowered = cc__lower_type_of_constexpr(work, n);
    const char* expr = lowered ? lowered : work;
    static const char base_prelude[] =
        "#ifdef __SIZE_TYPE__\ntypedef __SIZE_TYPE__ size_t;\n"
        "#else\ntypedef unsigned long size_t;\n#endif\n"
        "#define true 1\n#define false 0\n"
        "enum cc_type_kind { CC_TK_UNKNOWN=0, CC_TK_PRIMITIVE=1, CC_TK_POINTER=2,"
        " CC_TK_STRUCT=3, CC_TK_GENERIC_INST=4, CC_TK_CLOSURE=5,"
        " CC_TK_FUNCTION=6, CC_TK_ARRAY=7 };\n";
    /* Combine the static prelude with the caller's in-scope type definitions
     * so user-declared structs/unions/enums resolve under `sizeof`/`_Alignof`. */
    size_t tplen = type_prelude ? strlen(type_prelude) : 0;
    size_t base = sizeof(base_prelude) - 1;
    char* prelude = (char*)malloc(base + tplen + 1);
    if (!prelude) { free(work); free(lowered); return 0; }
    memcpy(prelude, base_prelude, base);
    if (tplen) memcpy(prelude + base, type_prelude, tplen);
    prelude[base + tplen] = '\0';
    int64_t v = 0;
    int ok = cc_tcc_eval_const_expr(prelude, expr, &v);
    free(prelude);
    free(work);
    free(lowered);
    if (!ok) return 0;
    *out = (long)v;
    return 1;
}

/* Phase-2 unified engine: evaluate @comptime if predicates exclusively via the
 * libtcc executor when CC_COMPTIME_UNIFIED_EXEC=1.  Falls back to the legacy
 * structural + TCC path when the flag is unset or the executor rejects the pred. */
static int cc__comptime_eval_pred_unified(const char* src, size_t n,
                                          const char* pred, size_t pred_n,
                                          const char* type_prelude, long* out) {
    if (cc_comptime_unified_exec_enabled()) {
        char* work = (char*)malloc(pred_n + 1);
        if (work) {
            memcpy(work, pred, pred_n);
            work[pred_n] = '\0';
            cc_comptime_fn_registry_scan(src, n);
            {
                int64_t iv = 0;
                if (cc_comptime_exec_eval_int(work, NULL, &iv, NULL, 0) == 0) {
                    *out = (long)iv;
                    free(work);
                    return 1;
                }
            }
            free(work);
        }
    }
    if (cc__comptime_eval_pred(pred, pred_n, out)) return 1;
    return cc__comptime_eval_pred_via_tcc(pred, pred_n, type_prelude, out);
}

/* Fix A: rewrite macro-expanded `cc_type_of("T")` back to the canonical
 * `type_of(T)` spelling so comptime if/for logic only handles one form.
 * Users may still write `cc_type_of("T")` by hand; pass_check_type_of keeps
 * accepting both.  Only rewrites the string-literal call form. */
static char* cc__normalize_cc_type_of_to_type_of(const char* src, size_t n) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0, last = 0;
    static const char KW[] = "cc_type_of";
    const size_t KWN = sizeof(KW) - 1;
    int changed = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);
    while (i < n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (!(i == 0 || !cc_is_ident_char(src[i - 1])) ||
            i + KWN > n || memcmp(src + i, KW, KWN) != 0 ||
            (i + KWN < n && cc_is_ident_char(src[i + KWN]))) {
            i++;
            continue;
        }
        size_t p = cc_skip_ws_and_comments(src, n, i + KWN);
        if (p >= n || src[p] != '(') { i++; continue; }
        size_t close;
        if (!cc_find_matching_paren(src, n, p, &close)) { i++; continue; }
        size_t q = cc_skip_ws_and_comments(src, n, p + 1);
        if (q >= close || src[q] != '"') { i++; continue; }
        size_t qs = q + 1, qe = qs;
        while (qe < close && src[qe] != '"') qe++;
        if (qe >= close) { i++; continue; }
        size_t tail = cc_skip_ws_and_comments(src, n, qe + 1);
        if (tail != close) { i++; continue; }
        cc_sb_append(&out, &out_len, &out_cap, src + last, i - last);
        cc_sb_append_cstr(&out, &out_len, &out_cap, "type_of(");
        cc_sb_append(&out, &out_len, &out_cap, src + qs, qe - qs);
        cc_sb_append_cstr(&out, &out_len, &out_cap, ")");
        last = close + 1;
        i = last;
        changed = 1;
    }
    if (!changed) return NULL;
    if (last < n) cc_sb_append(&out, &out_len, &out_cap, src + last, n - last);
    if (!out) out = strdup("");
    return out;
}

/* ============================================================
 * D4.0: `@comptime for (F in type_of(T).fields) { BODY }`
 * compile-time field iteration.  Unrolls BODY once per declared
 * field of struct T, substituting the loop variable F:
 *   F        -> the field's identifier   (so `p->F` -> `p->a`)
 *   F.name    -> "fieldname"              (string literal)
 *   F.type    -> the field's type spelling (bare tokens)
 *   F.typestr -> "type spelling"          (string literal; for cc_instantiate
 *                                          / cc_emit_format operands)
 *   F.index   -> the 0-based field index  (decimal literal)
 * T's definition must be in the same source buffer; fields are read
 * from the declared layout.  The member-declarator parser models
 * scalars/pointers, multi-declarators, arrays (incl. multi-dim),
 * function pointers, and named bitfields exactly.  Forms it cannot
 * spell as a usable `type` (inline anonymous/nested aggregate defs,
 * anonymous members, unnamed bitfields, pointer-to-array) and unknown
 * types are a hard error, never a silent skip — reflection must see
 * every field or none (never a partial or guessed set).
 * ============================================================ */

static void cc__ct_free_fields(CCCtField* f, size_t n) {
    if (!f) return;
    for (size_t i = 0; i < n; i++) {
        free(f[i].name); free(f[i].type); free(f[i].params); free(f[i].member);
    }
    free(f);
}

/* Locate the `{...}` body of the struct/union named `tname` (a typedef
 * name, or a `struct Tag`/`union Tag` spelling).  Sets [*bo,*bc] to the
 * brace offsets (bo at '{', bc at '}') and returns 1; 0 if not found. */
static int cc__ct_find_struct_body(const char* src, size_t n,
                                   const char* tname, size_t tlen,
                                   size_t* bo, size_t* bc) {
    int want_tag = 0;
    const char* tag = tname; size_t taglen = tlen;
    if (tlen > 7 && memcmp(tname, "struct ", 7) == 0) { want_tag = 1; tag = tname + 7; taglen = tlen - 7; }
    else if (tlen > 6 && memcmp(tname, "union ", 6) == 0) { want_tag = 1; tag = tname + 6; taglen = tlen - 6; }
    while (taglen && (tag[0] == ' ' || tag[0] == '\t')) { tag++; taglen--; }
    while (taglen && (tag[taglen - 1] == ' ' || tag[taglen - 1] == '\t')) taglen--;

    CCScannerState scan; cc_scanner_init(&scan);
    size_t i = 0, depth = 0;
    while (i < n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        char c = src[i];
        if (c == '{') { depth++; i++; continue; }
        if (c == '}') { if (depth) depth--; i++; continue; }
        if (depth != 0) { i++; continue; }
        if (i > 0 && cc_is_ident_char(src[i - 1])) { i++; continue; }
        int is_typedef = 0; size_t kwlen = 0;
        if (i + 7 <= n && memcmp(src + i, "typedef", 7) == 0 &&
            (i + 7 == n || !cc_is_ident_char(src[i + 7]))) { kwlen = 7; is_typedef = 1; }
        else if (i + 6 <= n && memcmp(src + i, "struct", 6) == 0 &&
                 (i + 6 == n || !cc_is_ident_char(src[i + 6]))) kwlen = 6;
        else if (i + 5 <= n && memcmp(src + i, "union", 5) == 0 &&
                 (i + 5 == n || !cc_is_ident_char(src[i + 5]))) kwlen = 5;
        if (!kwlen) { i++; continue; }
        if (is_typedef) {
            size_t semi = cc__span_to_top_semicolon(src, n, i + kwlen); /* index past ';' */
            size_t b1 = 0; int havebrace = 0;
            CCScannerState s2; cc_scanner_init(&s2);
            size_t j = i + kwlen;
            while (j < semi) {
                if (cc_scanner_skip_non_code(&s2, src, semi, &j)) continue;
                if (src[j] == '{') { b1 = j; havebrace = 1; break; }
                j++;
            }
            if (!want_tag && havebrace) {
                size_t b2;
                if (cc_find_matching_brace(src, n, b1, &b2) && semi > 0) {
                    /* typedef name = last identifier before the ';'
                     * (comment-aware: `} Name / *c* / ;` still reads Name) */
                    size_t s = semi - 1;
                    while (s > b2 && src[s] != ';') s--;
                    size_t ne = cc_rskip_ws_and_comments(src, s);
                    size_t ns = ne;
                    while (ns > b2 && cc_is_ident_char(src[ns - 1])) ns--;
                    if (ne > ns && ns > b2 && (ne - ns) == tlen && memcmp(src + ns, tname, tlen) == 0) {
                        *bo = b1; *bc = b2; return 1;
                    }
                }
            }
            i = semi; continue;
        }
        /* struct/union [TAG] { ... } */
        size_t p = cc_skip_ws_and_comments(src, n, i + kwlen);
        size_t tags = p;
        while (p < n && cc_is_ident_char(src[p])) p++;
        size_t tage = p;
        size_t bp = cc_skip_ws_and_comments(src, n, p);
        if (bp < n && src[bp] == '{') {
            size_t b2;
            if (cc_find_matching_brace(src, n, bp, &b2)) {
                if (want_tag && tage > tags && (tage - tags) == taglen &&
                    memcmp(src + tags, tag, taglen) == 0) {
                    *bo = bp; *bc = b2; return 1;
                }
                i = b2 + 1; continue;
            }
        }
        i += kwlen;
    }
    return 0;
}

/* Append one parsed field (name[0..namelen), NUL-terminated `type`). 0 = OOM. */
static int cc__ct_push_field(CCCtField** fs, size_t* fn, size_t* fc,
                             const char* name, size_t namelen, const char* type) {
    char* nm = (char*)malloc(namelen + 1);
    if (!nm) return 0;
    memcpy(nm, name, namelen); nm[namelen] = 0;
    char* ty = (char*)malloc(strlen(type ? type : "") + 1);
    if (!ty) { free(nm); return 0; }
    strcpy(ty, type ? type : "");
    if (*fn + 1 > *fc) {
        size_t nc = *fc ? *fc * 2 : 8;
        CCCtField* nb = (CCCtField*)realloc(*fs, nc * sizeof(CCCtField));
        if (!nb) { free(nm); free(ty); return 0; }
        *fs = nb; *fc = nc;
    }
    (*fs)[*fn].name = nm; (*fs)[*fn].type = ty; (*fn)++;
    (*fs)[*fn - 1].is_as = 0;
    (*fs)[*fn - 1].params = NULL;
    (*fs)[*fn - 1].member = NULL;
    return 1;
}

/* Normalize a member declaration span [ms,me) into a freshly-malloc'd string:
 * comments/strings dropped, every whitespace run collapsed to one space, the
 * result trimmed.  The declarator mini-parser below works on this flat form. */
static char* cc__ct_member_normalize(const char* src, size_t ms, size_t me) {
    char* out = (char*)malloc((me - ms) + 1);
    if (!out) return NULL;
    size_t o = 0;
    int pending_space = 0;
    CCScannerState s; cc_scanner_init(&s);
    size_t i = ms;
    while (i < me) {
        if (cc_scanner_skip_non_code(&s, src, me, &i)) { if (o) pending_space = 1; continue; }
        char c = src[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { if (o) pending_space = 1; i++; continue; }
        if (pending_space) { out[o++] = ' '; pending_space = 0; }
        out[o++] = c;
        i++;
    }
    out[o] = 0;
    return out;
}

/* Function-pointer declarator: `<base> (* name)(params)` (d[lparen]=='(' whose
 * first non-space is '*').  Emits {name, "<base> (*)(params)"}.  Returns 1 ok, 0
 * bail (array-of-fn-ptr / returns-fn-ptr / trailing junk are not modeled). */
static int cc__ct_parse_fnptr(const char* d, size_t dl, size_t lparen,
                              const char* base, char* base_out, size_t base_cap,
                              CCCtField** fs, size_t* fn, size_t* fc) {
    char base_buf[192];
    size_t be = lparen;
    while (be && d[be - 1] == ' ') be--;
    if (base) {
        if (be != 0) return 0;                 /* later declarator can't restate base */
        snprintf(base_buf, sizeof(base_buf), "%s", base);
    } else {
        if (be == 0 || be >= sizeof(base_buf)) return 0;
        memcpy(base_buf, d, be); base_buf[be] = 0;
        if (base_out && base_cap) snprintf(base_out, base_cap, "%s", base_buf);
    }
    size_t depth = 0, grp_close = dl;
    for (size_t i = lparen; i < dl; i++) {
        if (d[i] == '(') depth++;
        else if (d[i] == ')') { depth--; if (depth == 0) { grp_close = i; break; } }
    }
    if (grp_close >= dl) return 0;
    size_t k = lparen + 1;
    while (k < grp_close && d[k] == ' ') k++;
    if (k >= grp_close || d[k] != '*') return 0;
    int stars = 0;
    while (k < grp_close && (d[k] == ' ' || d[k] == '*')) { if (d[k] == '*') stars++; k++; }
    if (stars < 1 || stars > 4) return 0;
    if (k >= grp_close || !cc_is_ident_char(d[k])) return 0;
    size_t ns = k;
    while (k < grp_close && cc_is_ident_char(d[k])) k++;
    size_t ne = k;
    while (k < grp_close && d[k] == ' ') k++;
    if (k != grp_close) return 0;              /* e.g. (*tbl[3]) — not modeled */
    size_t p = grp_close + 1;
    while (p < dl && d[p] == ' ') p++;
    if (p >= dl || d[p] != '(') return 0;
    size_t pdepth = 0, pe = dl;
    for (size_t q = p; q < dl; q++) {
        if (d[q] == '(') pdepth++;
        else if (d[q] == ')') { pdepth--; if (pdepth == 0) { pe = q; break; } }
    }
    if (pe >= dl) return 0;
    size_t after = pe + 1;
    while (after < dl && d[after] == ' ') after++;
    if (after != dl) return 0;                 /* trailing junk → bail */
    char type[256];
    int tn = snprintf(type, sizeof(type), "%s (%.*s)(%.*s)",
                      base_buf, stars, "****", (int)(pe - (p + 1)), d + p + 1);
    if (tn < 0 || (size_t)tn >= sizeof(type) || tn >= 120) return 0;
    return cc__ct_push_field(fs, fn, fc, d + ns, ne - ns, type);
}

/* Parse one declarator `d[0..dl)` sharing `base` (NULL for the first declarator
 * of a member, where the base type is derived from the leading words and copied
 * to base_out).  Models pointers, arrays (incl. multi-dim), function pointers,
 * and a trailing named bitfield (width is validated but not exposed).  Emits one
 * field.  Returns 1 ok, 0 bail. */
static int cc__ct_parse_declarator(const char* d, size_t dl, const char* base,
                                   char* base_out, size_t base_cap,
                                   CCCtField** fs, size_t* fn, size_t* fc) {
    while (dl && d[0] == ' ') { d++; dl--; }
    while (dl && d[dl - 1] == ' ') dl--;
    if (dl == 0) return 0;

    /* Trailing bitfield `: width` (top level): width must be an integer literal;
     * the named field keeps its base type, width is dropped (not modeled). */
    {
        size_t depth = 0, colon = dl;
        for (size_t i = 0; i < dl; i++) {
            char c = d[i];
            if (c == '(' || c == '[') depth++;
            else if (c == ')' || c == ']') { if (depth) depth--; }
            else if (c == ':' && depth == 0) { colon = i; break; }
        }
        if (colon < dl) {
            size_t w = colon + 1;
            while (w < dl && d[w] == ' ') w++;
            size_t ws = w;
            while (w < dl && d[w] >= '0' && d[w] <= '9') w++;
            size_t we = w;
            while (w < dl && d[w] == ' ') w++;
            if (we == ws || w != dl) return 0;         /* non-literal width → bail */
            dl = colon;
            while (dl && d[dl - 1] == ' ') dl--;
        }
    }

    /* Function pointer: first top-level '(' whose first non-space is '*'. */
    {
        size_t depth = 0;
        for (size_t i = 0; i < dl; i++) {
            char c = d[i];
            if (c == '(') {
                if (depth == 0) {
                    size_t k = i + 1;
                    while (k < dl && d[k] == ' ') k++;
                    if (k < dl && d[k] == '*')
                        return cc__ct_parse_fnptr(d, dl, i, base, base_out, base_cap,
                                                  fs, fn, fc);
                }
                depth++;
            } else if (c == ')') { if (depth) depth--; }
        }
    }

    /* Trailing array suffixes `[..]...` (top level), collected right-to-left. */
    char arr[128]; size_t al = 0; arr[0] = 0;
    for (;;) {
        if (dl && d[dl - 1] == ']') {
            size_t depth = 0, open = dl;
            for (size_t i = dl; i-- > 0; ) {
                char c = d[i];
                if (c == ']') depth++;
                else if (c == '[') { if (depth) depth--; if (depth == 0) { open = i; break; } }
            }
            if (open >= dl) return 0;
            size_t slen = dl - open;
            if (al + slen >= sizeof(arr)) return 0;
            memmove(arr + slen, arr, al + 1);
            memcpy(arr, d + open, slen);
            al += slen;
            dl = open;
            while (dl && d[dl - 1] == ' ') dl--;
        } else break;
    }

    /* Remainder is `words and '*' ... name`. */
    const char* words[64]; size_t wlen[64]; int nw = 0, stars = 0;
    {
        size_t i = 0;
        while (i < dl) {
            char c = d[i];
            if (c == ' ') { i++; continue; }
            if (c == '*') { stars++; i++; continue; }
            if (cc_is_ident_char(c)) {
                size_t s0 = i;
                while (i < dl && cc_is_ident_char(d[i])) i++;
                if (nw < 64) { words[nw] = d + s0; wlen[nw] = i - s0; nw++; }
                else return 0;
                continue;
            }
            return 0;                                   /* unexpected char → bail */
        }
    }

    const char* name; size_t namelen;
    char base_buf[192];
    if (base) {
        if (nw != 1) return 0;                          /* later declarator = just a name */
        name = words[0]; namelen = wlen[0];
        snprintf(base_buf, sizeof(base_buf), "%s", base);
    } else {
        if (nw < 2) return 0;                           /* need base + name */
        name = words[nw - 1]; namelen = wlen[nw - 1];
        size_t bl = 0;
        for (int w = 0; w < nw - 1; w++) {
            if (w) { if (bl + 1 >= sizeof(base_buf)) return 0; base_buf[bl++] = ' '; }
            if (bl + wlen[w] >= sizeof(base_buf)) return 0;
            memcpy(base_buf + bl, words[w], wlen[w]); bl += wlen[w];
        }
        base_buf[bl] = 0;
        if (base_out && base_cap) snprintf(base_out, base_cap, "%s", base_buf);
    }

    char type[256];
    int tn = snprintf(type, sizeof(type), "%s", base_buf);
    if (tn < 0 || (size_t)tn >= sizeof(type)) return 0;
    size_t tl = (size_t)tn;
    for (int s = 0; s < stars; s++) { if (tl + 1 >= sizeof(type)) return 0; type[tl++] = '*'; }
    if (al) { if (tl + al >= sizeof(type)) return 0; memcpy(type + tl, arr, al); tl += al; }
    type[tl] = 0;
    if (tl >= 120) return 0;                             /* keep fixed-128 reflect buffers safe */
    return cc__ct_push_field(fs, fn, fc, name, namelen, type);
}

/* Parse one member declaration (already normalized, flat) into 1+ fields.
 * Splits on top-level commas (multi-declarator), deriving a shared base type
 * from the first declarator.  Returns 1 ok, 0 bail. */
static int cc__ct_parse_member(const char* m, CCCtField** fs, size_t* fn, size_t* fc) {
    size_t L = strlen(m);
    if (L == 0) return 0;
    /* Inline aggregate definitions (anonymous/nested struct/union/enum) carry a
     * brace and cannot be spelled as a usable `type`, so they stay all-or-none. */
    {
        size_t depth = 0;
        for (size_t i = 0; i < L; i++) {
            char c = m[i];
            if (c == '(' || c == '[') depth++;
            else if (c == ')' || c == ']') { if (depth) depth--; }
            else if (c == '{' || c == '}') return 0;
        }
    }
    char base[192]; base[0] = 0;
    int have_base = 0;
    size_t start = 0, depth = 0;
    for (size_t i = 0; i <= L; i++) {
        char c = (i < L) ? m[i] : ',';                  /* sentinel comma flushes last */
        if (i < L && (c == '(' || c == '[')) { depth++; continue; }
        if (i < L && (c == ')' || c == ']')) { if (depth) depth--; continue; }
        if (c == ',' && depth == 0) {
            if (!cc__ct_parse_declarator(m + start, i - start,
                                         have_base ? base : NULL,
                                         have_base ? NULL : base, sizeof(base),
                                         fs, fn, fc))
                return 0;
            have_base = 1;
            start = i + 1;
        }
    }
    return 1;
}

/* Parse a parenthesized parameter list `src[lp..rp]` (lp='(', rp=')') into a
 * field list, one entry per declared parameter, in declaration order.
 *
 * Unlike a struct member, every parameter carries its own base type, so each
 * comma-separated piece is parsed as a fresh declarator rather than sharing the
 * first one's base.  `(void)` and `()` yield the empty list.  A parameter using
 * a form the declarator parser cannot spell exactly — or an unnamed one, which
 * has no identifier to reflect — is a bail, so the caller errors rather than
 * handing back a guessed or partial list.  Returns 1 ok, 0 bail. */
/* Top-level bare `=` in [ps, pe), or pe if none.  Skips `==` / `!=` / compound
 * assigns so only a parameter default introducer matches. */
static size_t cc__ct_top_eq(const char* src, size_t ps, size_t pe) {
    CCScannerState s; cc_scanner_init(&s);
    size_t i = ps, depth = 0;
    while (i < pe) {
        if (cc_scanner_skip_non_code(&s, src, pe, &i)) continue;
        char c = src[i];
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') { if (depth) depth--; }
        else if (c == '=' && depth == 0) {
            char prev = (i > ps) ? src[i - 1] : 0;
            char next = (i + 1 < pe) ? src[i + 1] : 0;
            if (next != '=' && prev != '<' && prev != '>' && prev != '!' &&
                prev != '=' && prev != '+' && prev != '-' && prev != '*' &&
                prev != '/' && prev != '%' && prev != '&' && prev != '|' &&
                prev != '^')
                return i;
        }
        i++;
    }
    return pe;
}

/* True when [a,b) is a parameter-default literal: integer/float (opt sign +
 * suffix), string/char, or the idents NULL / true / false. */
static int cc__ct_is_param_default_literal(const char* src, size_t a, size_t b) {
    size_t i;
    if (a >= b) return 0;
    if (src[a] == '"') {
        i = a + 1;
        while (i < b) {
            if (src[i] == '\\') { if (i + 1 < b) i += 2; else return 0; continue; }
            if (src[i] == '"') {
                i++;
                while (i < b && (src[i] == ' ' || src[i] == '\t')) i++;
                return i == b;
            }
            i++;
        }
        return 0;
    }
    if (src[a] == '\'') {
        i = a + 1;
        while (i < b) {
            if (src[i] == '\\') { if (i + 1 < b) i += 2; else return 0; continue; }
            if (src[i] == '\'') {
                i++;
                while (i < b && (src[i] == ' ' || src[i] == '\t')) i++;
                return i == b;
            }
            i++;
        }
        return 0;
    }
    if (cc_is_ident_start(src[a])) {
        i = a;
        while (i < b && cc_is_ident_char(src[i])) i++;
        size_t n = i - a;
        int ok = (n == 4 && memcmp(src + a, "NULL", 4) == 0) ||
                 (n == 4 && memcmp(src + a, "true", 4) == 0) ||
                 (n == 5 && memcmp(src + a, "false", 5) == 0);
        while (i < b && (src[i] == ' ' || src[i] == '\t')) i++;
        return ok && i == b;
    }
    i = a;
    if (src[i] == '+' || src[i] == '-') i++;
    if (i >= b) return 0;
    if (src[i] == '0' && i + 1 < b && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
        i += 2;
        size_t hex = i;
        while (i < b && isxdigit((unsigned char)src[i])) i++;
        if (i == hex) return 0;
    } else {
        size_t dig = i;
        while (i < b && src[i] >= '0' && src[i] <= '9') i++;
        if (i < b && src[i] == '.') {
            i++;
            while (i < b && src[i] >= '0' && src[i] <= '9') i++;
        }
        if (i == dig) return 0;
        if (i < b && (src[i] == 'e' || src[i] == 'E')) {
            size_t e = ++i;
            if (i < b && (src[i] == '+' || src[i] == '-')) i++;
            while (i < b && src[i] >= '0' && src[i] <= '9') i++;
            if (i == e || (i == e + 1 && (src[e] == '+' || src[e] == '-'))) return 0;
        }
    }
    while (i < b && (src[i] == 'u' || src[i] == 'U' || src[i] == 'l' ||
                     src[i] == 'L' || src[i] == 'f' || src[i] == 'F'))
        i++;
    while (i < b && (src[i] == ' ' || src[i] == '\t')) i++;
    return i == b;
}

/* Declaration-style default at the end of param span [ps,pe).
 * Returns 1 and sets *eq_pos when `type name = literal` is present; 0 when
 * there is no default (including assignment expressions like `x = 1` with no
 * type); -1 when a bare `=` is present but the RHS is not a modeled literal. */
static int cc__ct_param_default_at(const char* src, size_t ps, size_t pe,
                                   size_t* eq_pos) {
    size_t eq, ds, de;
    char* left;
    CCCtField* fs = NULL; size_t fn = 0, fc = 0;
    int ok;
    if (eq_pos) *eq_pos = pe;
    eq = cc__ct_top_eq(src, ps, pe);
    if (eq >= pe) return 0;
    ds = cc_skip_ws_and_comments(src, pe, eq + 1);
    de = cc_rskip_ws_and_comments(src, pe);
    if (de <= ds) return -1;
    if (!cc__ct_is_param_default_literal(src, ds, de)) return -1;
    /* Left of `=` must be a real parameter declarator — otherwise this is an
     * assignment expression in a call argument, not a default. */
    {
        size_t le = cc_rskip_ws_and_comments(src, eq);
        if (le <= ps) return 0;
        left = cc__ct_member_normalize(src, ps, le);
        if (!left) return -1;
        ok = cc__ct_parse_declarator(left, strlen(left), NULL, NULL, 0, &fs, &fn, &fc);
        free(left);
        cc__ct_free_fields(fs, fn);
        if (!ok) return 0;
    }
    if (eq_pos) *eq_pos = eq;
    return 1;
}

/* `T[:] name` slice sugar in a reflected spelling: reflection reads the
 * pre-lowering snapshot, but factories emit host C, so the sugar rewrites
 * to the instance name the lowering itself produces — `CCSlice_<elem>`
 * under the canonical type mangle, the char family staying plain
 * `CCSlice`.  Returns a malloc'd rewrite, or NULL when `decl` carries no
 * slice sugar (the caller keeps its string). */
char* cc_ct_slice_sugar_rewrite(const char* decl) {
    size_t n, i, lb = 0, rb, ee;
    int have = 0, is_char = 0;
    char norm[128];
    size_t nn = 0;
    char mangled[128];
    char head[160];
    char* out;
    if (!decl) return NULL;
    n = strlen(decl);
    for (i = 0; i + 1 < n; i++) {
        if (decl[i] == '[') {
            size_t k = i + 1;
            while (k < n && (decl[k] == ' ' || decl[k] == '\t')) k++;
            if (k < n && decl[k] == ':') { lb = i; have = 1; break; }
        }
    }
    if (!have) return NULL;
    ee = lb;
    while (ee > 0 && (decl[ee - 1] == ' ' || decl[ee - 1] == '\t')) ee--;
    i = 0;
    while (i < ee) {
        size_t ts;
        while (i < ee && (decl[i] == ' ' || decl[i] == '\t')) i++;
        if (i >= ee) break;
        if (!cc_is_ident_start(decl[i])) return NULL;
        if (nn > 0 && nn + 1 < sizeof(norm)) norm[nn++] = ' ';
        ts = nn;
        while (i < ee && cc_is_ident_char(decl[i])) {
            if (nn + 1 >= sizeof(norm)) return NULL;
            norm[nn++] = decl[i++];
        }
        if (nn - ts == 4 && memcmp(norm + ts, "char", 4) == 0) is_char = 1;
    }
    norm[nn] = '\0';
    if (nn == 0) return NULL;
    rb = lb;
    while (rb < n && decl[rb] != ']') rb++;
    if (rb >= n) return NULL;
    if (is_char) {
        snprintf(head, sizeof(head), "CCSlice");
    } else {
        cc__mangle_type_name(norm, nn, mangled, sizeof(mangled));
        if (!mangled[0]) return NULL;
        if ((size_t)snprintf(head, sizeof(head), "CCSlice_%s", mangled) >=
            sizeof(head))
            return NULL;
    }
    {
        const char* rest = decl + rb + 1;
        while (*rest == ' ' || *rest == '\t') rest++;
        out = (char*)malloc(strlen(head) + 1 + strlen(rest) + 1);
        if (!out) return NULL;
        strcpy(out, head);
        if (*rest) { strcat(out, " "); strcat(out, rest); }
    }
    return out;
}

static int cc__ct_parse_param_list(const char* src, size_t lp, size_t rp,
                                   CCCtField** out, size_t* out_n) {
    CCCtField* fs = NULL; size_t fn = 0, fc = 0;
    size_t i = lp + 1;
    *out = NULL; *out_n = 0;
    /* An empty or `(void)` list has no parameters. */
    {
        size_t a = cc_skip_ws_and_comments(src, rp, i);
        size_t b = cc_rskip_ws_and_comments(src, rp);
        if (b <= a) { *out = NULL; *out_n = 0; return 1; }
        if (b - a == 4 && memcmp(src + a, "void", 4) == 0) { *out = NULL; *out_n = 0; return 1; }
    }
    while (i < rp) {
        CCScannerState s; cc_scanner_init(&s);
        size_t j = i, depth = 0, comma = rp;
        while (j < rp) {
            if (cc_scanner_skip_non_code(&s, src, rp, &j)) continue;
            char c = src[j];
            if (c == '(' || c == '[') depth++;
            else if (c == ')' || c == ']') { if (depth) depth--; }
            else if (c == ',' && depth == 0) { comma = j; break; }
            j++;
        }
        {
            size_t ps = cc_skip_ws_and_comments(src, comma, i);
            size_t pe = cc_rskip_ws_and_comments(src, comma);
            if (pe > ps) {
                size_t eq = pe;
                int dr = cc__ct_param_default_at(src, ps, pe, &eq);
                char* d;
                int ok;
                if (dr < 0) { cc__ct_free_fields(fs, fn); return 0; }
                if (dr > 0) pe = cc_rskip_ws_and_comments(src, eq);
                if (pe <= ps) { cc__ct_free_fields(fs, fn); return 0; }
                /* Slice sugar rewrites BEFORE normalization: `[:]` reads
                 * as a bitfield to the member grammar, so the raw span is
                 * rewritten to the lowered instance name first. */
                {
                    char* raw = (char*)malloc(pe - ps + 1);
                    char* rw = NULL;
                    if (raw) {
                        memcpy(raw, src + ps, pe - ps);
                        raw[pe - ps] = '\0';
                        rw = cc_ct_slice_sugar_rewrite(raw);
                        free(raw);
                    }
                    if (rw) {
                        d = cc__ct_member_normalize(rw, 0, strlen(rw));
                        free(rw);
                    } else {
                        d = cc__ct_member_normalize(src, ps, pe);
                    }
                }
                if (!d) { cc__ct_free_fields(fs, fn); return 0; }
                ok = cc__ct_parse_declarator(d, strlen(d), NULL, NULL, 0, &fs, &fn, &fc);
                free(d);
                if (!ok) { cc__ct_free_fields(fs, fn); return 0; }
            } else { cc__ct_free_fields(fs, fn); return 0; }  /* `a, , b` */
        }
        i = comma + 1;
    }
    *out = fs; *out_n = fn; return 1;
}

static void cc__ct_apply_typeview_as(const char* src, size_t n, const char* tname,
                                     CCCtField* fs, size_t nf) {
    char names[32][64];
    int nn, i, k;
    if (!src || !tname || !tname[0] || !fs || nf == 0) return;
    nn = cc_typeview_as_names_for_type(src, n, tname, names, 32);
    for (i = 0; i < (int)nf; i++) {
        if (!fs[i].name) continue;
        for (k = 0; k < nn; k++) {
            if (strcmp(fs[i].name, names[k]) == 0) {
                fs[i].is_as = 1;
                break;
            }
        }
    }
}

/* Parse the member declarations in struct body (bo='{', bc='}') into a field
 * list.  Returns 1 on success (caller frees *out via cc__ct_free_fields), 0 if
 * any member uses a form the parser cannot model exactly (so the caller errors
 * loudly — a partial/guessed field set is never produced). */
static int cc__ct_parse_fields_from_body(const char* src, size_t bo, size_t bc,
                                         CCCtField** out, size_t* out_n) {
    CCCtField* fs = NULL; size_t fn = 0, fc = 0;
    size_t i = bo + 1;
    while (i < bc) {
        CCScannerState s; cc_scanner_init(&s);
        size_t j = i, depth = 0, semi = bc;
        while (j < bc) {
            if (cc_scanner_skip_non_code(&s, src, bc, &j)) continue;
            char c = src[j];
            if (c == '{' || c == '(' || c == '[') depth++;
            else if (c == '}' || c == ')' || c == ']') { if (depth) depth--; }
            else if (c == ';' && depth == 0) { semi = j; break; }
            j++;
        }
        /* Skip leading whitespace AND comments: a block comment trailing the
         * previous field's `;` falls into this member's span. */
        size_t ms = cc_skip_ws_and_comments(src, semi, i);
        size_t me = semi;
        while (me > ms && (src[me - 1] == ' ' || src[me - 1] == '\t' || src[me - 1] == '\n' || src[me - 1] == '\r')) me--;
        if (me > ms) {
            /* `@as` composition: source spells `Base base @as;`, and the
             * parse-input rewrite turns it into a block-comment marker, so
             * both spellings reach here.  Record it — a walk over composition
             * has to tell a composed member from an ordinary field. */
            int member_is_as = 0;
            size_t before = fn;
            char* m;
            int ok;
            for (size_t q = ms; q + 3 <= me; q++) {
                if (src[q] == '@' && memcmp(src + q, "@as", 3) == 0 &&
                    (q + 3 == me || !cc_is_ident_char(src[q + 3]))) { member_is_as = 1; break; }
                if (src[q] == '/' && q + 7 <= me && memcmp(src + q, "/*@as*/", 7) == 0) {
                    member_is_as = 1; break;
                }
            }
            m = cc__ct_member_normalize(src, ms, me);
            if (!m) { cc__ct_free_fields(fs, fn); return 0; }
            if (member_is_as) {
                /* The marker is an attribute on the member, not part of its
                 * declarator — leave it in and the declarator parser reads it
                 * as an unsupported form. */
                char* w = m;
                for (char* r = m; *r; ) {
                    if (r[0] == '@' && r[1] == 'a' && r[2] == 's' &&
                        !cc_is_ident_char(r[3])) { r += 3; continue; }
                    if (r[0] == '/' && strncmp(r, "/*@as*/", 7) == 0) { r += 7; continue; }
                    *w++ = *r++;
                }
                *w = 0;
            }
            ok = cc__ct_parse_member(m, &fs, &fn, &fc);
            free(m);
            if (!ok) { cc__ct_free_fields(fs, fn); return 0; }
            if (member_is_as) for (size_t q = before; q < fn; q++) fs[q].is_as = 1;
        }
        i = semi + 1;
    }
    *out = fs; *out_n = fn; return 1;
}

int cc_ct_reflect_struct_fields(const char* src, size_t len, const char* type_name,
                                CCCtField** out, size_t* out_n) {
    if (out) *out = NULL;
    if (out_n) *out_n = 0;
    if (!src || !type_name || !type_name[0] || !out || !out_n) return 0;
    size_t bo, bc;
    size_t nlen = strlen(type_name);
    if (cc__ct_find_struct_body(src, len, type_name, nlen, &bo, &bc))
        return cc__ct_parse_fields_from_body(src, bo, bc, out, out_n);
    /* Types often live in included .cch while emit-producing `@comptime { }`
     * blocks are harvested into the TU (static_map-in-header). Mirror
     * cc__sm_find_typedef's included-source search. */
    for (size_t h = 0; h < g_included_cch_source_count; h++) {
        size_t fn = 0;
        const char* fsrc = cc__included_cch_text(h, &fn);
        if (!fsrc) continue;
        if (cc__ct_find_struct_body(fsrc, fn, type_name, nlen, &bo, &bc))
            return cc__ct_parse_fields_from_body(fsrc, bo, bc, out, out_n);
    }
    return 0;
}

int cc_ct_reflect_param_list(const char* params, CCCtField** out, size_t* out_n) {
    size_t n;
    if (out) *out = NULL;
    if (out_n) *out_n = 0;
    if (!params || !out || !out_n) return 0;
    n = strlen(params);
    if (n < 2 || params[0] != '(' || params[n - 1] != ')') return 0;
    return cc__ct_parse_param_list(params, 0, n - 1, out, out_n);
}

int cc_ct_reflect_param_default(const char* params, int idx, char* buf, int buf_sz) {
    size_t n, i, pidx;
    if (buf && buf_sz > 0) buf[0] = '\0';
    if (!params || idx < 0) return -1;
    n = strlen(params);
    if (n < 2 || params[0] != '(' || params[n - 1] != ')') return -1;
    i = 1;
    pidx = 0;
    while (i < n - 1) {
        CCScannerState s; cc_scanner_init(&s);
        size_t j = i, depth = 0, comma = n - 1;
        size_t ps, pe, eq, ds, de;
        int dr;
        while (j < n - 1) {
            if (cc_scanner_skip_non_code(&s, params, n - 1, &j)) continue;
            char c = params[j];
            if (c == '(' || c == '[') depth++;
            else if (c == ')' || c == ']') { if (depth) depth--; }
            else if (c == ',' && depth == 0) { comma = j; break; }
            j++;
        }
        ps = cc_skip_ws_and_comments(params, comma, i);
        pe = cc_rskip_ws_and_comments(params, comma);
        if ((int)pidx == idx) {
            if (pe <= ps) return 0;
            if (pe - ps == 4 && memcmp(params + ps, "void", 4) == 0) return 0;
            dr = cc__ct_param_default_at(params, ps, pe, &eq);
            if (dr < 0) return -1;
            if (dr == 0) return 0;
            ds = cc_skip_ws_and_comments(params, pe, eq + 1);
            de = pe;
            if (de <= ds || !buf || buf_sz <= 0) return (int)(de - ds);
            {
                size_t dn = de - ds;
                int cap = (int)dn < buf_sz - 1 ? (int)dn : buf_sz - 1;
                memcpy(buf, params + ds, (size_t)cap);
                buf[cap] = '\0';
                return cap;
            }
        }
        pidx++;
        i = comma + 1;
    }
    return -1;
}

void cc_ct_free_fields(CCCtField* fields, size_t n) {
    cc__ct_free_fields(fields, n);
}

/* ---- static_map call-site rewrite (typed value inference) ------------ */

/* Growable byte buffer for the rewrite output. */
typedef struct { char* p; size_t len, cap; } CCSmBuf;

static int cc__sm_append(CCSmBuf* b, const char* s, size_t n) {
    if (n == 0) return 1;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n + 1) nc *= 2;
        char* np = (char*)realloc(b->p, nc);
        if (!np) return 0;
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 1;
}

/* Trim ASCII whitespace from [*s, *s+*len). */
static void cc__sm_trim(const char** s, size_t* len) {
    const char* p = *s;
    size_t l = *len;
    while (l && (p[0] == ' ' || p[0] == '\t' || p[0] == '\n' || p[0] == '\r')) { p++; l--; }
    while (l && (p[l - 1] == ' ' || p[l - 1] == '\t' || p[l - 1] == '\n' || p[l - 1] == '\r')) l--;
    *s = p; *len = l;
}

/* Split the top-level (comma-separated) arguments of a call whose parenthesized
 * body is src[open+1 .. close).  Records each argument's [start,end) span.
 * Returns the argument count, or -1 if it exceeds `max`. */
static int cc__sm_split_args(const char* src, size_t open, size_t close,
                             size_t* starts, size_t* ends, int max) {
    int nargs = 0;
    size_t depth = 0;
    size_t argstart = open + 1;
    CCScannerState s; cc_scanner_init(&s);
    size_t i = open + 1;
    while (i < close) {
        if (cc_scanner_skip_non_code(&s, src, close, &i)) continue;
        char c = src[i];
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') { if (depth) depth--; }
        else if (c == ',' && depth == 0) {
            if (nargs >= max) return -1;
            starts[nargs] = argstart; ends[nargs] = i; nargs++;
            argstart = i + 1;
        }
        i++;
    }
    if (nargs >= max) return -1;
    starts[nargs] = argstart; ends[nargs] = close; nargs++;
    return nargs;
}

/* Find the declared element type of the array variable `var` by locating its
 * nearest declaration (`<Type> var[`) before `call_start`.  Takes only the
 * identifier immediately preceding `var` (optionally after `struct`/`enum`/
 * storage-class keywords) so comments between the prior statement and the
 * declaration cannot poison the scan.  Returns 1 on success. */
static int cc__sm_find_entry_type(const char* src, size_t n, const char* var,
                                  size_t vlen, size_t call_start,
                                  char* out, size_t out_sz) {
    size_t best = 0;
    int found = 0;
    size_t i = 0;
    while (i + vlen <= call_start) {
        size_t after, a, q, te, ts;
        if (i > 0 && cc_is_ident_char(src[i - 1])) { i++; continue; }
        if (memcmp(src + i, var, vlen) != 0) { i++; continue; }
        after = i + vlen;
        if (after < n && cc_is_ident_char(src[after])) { i++; continue; }
        a = after;
        while (a < n && (src[a] == ' ' || src[a] == '\t' || src[a] == '\n' || src[a] == '\r')) a++;
        if (a >= n || src[a] != '[') { i++; continue; }
        /* Immediately preceding identifier is the typedef / tag name
         * (comment-aware: `MyType / *c* / arr[` still reads MyType). */
        q = cc_rskip_ws_and_comments(src, i);
        te = q;
        while (q > 0 && cc_is_ident_char(src[q - 1])) q--;
        ts = q;
        if (te > ts) { best = i; found = 1; }
        i = after;
    }
    if (!found) return 0;
    {
        size_t q = cc_rskip_ws_and_comments(src, best);
        size_t te, ts;
        const char* t;
        size_t tl;
        te = q;
        while (q > 0 && cc_is_ident_char(src[q - 1])) q--;
        ts = q;
        t = src + ts;
        tl = te - ts;
        if (tl == 0 || tl + 1 >= out_sz) return 0;
        memcpy(out, t, tl);
        out[tl] = '\0';
        return 1;
    }
}

/* True when a top-level `typedef …;` span defines the type name `name`
 * (trailing typedef-name, or `typedef enum/struct name …`). */
static int cc__sm_typedef_defines(const char* decl, size_t len, const char* name) {
    size_t nlen, i, end, start, p;
    if (!decl || !name || !name[0] || len < 8) return 0;
    nlen = strlen(name);
    /* Comment-aware trims: `typedef ... Name / *c* / ;` still reads Name. */
    i = cc_rskip_ws_and_comments(decl, len);
    if (i == 0 || decl[i - 1] != ';') return 0;
    i--;
    i = cc_rskip_ws_and_comments(decl, i);
    end = i;
    while (i > 0 && cc_is_ident_char(decl[i - 1])) i--;
    start = i;
    if (end > start && (end - start) == nlen && memcmp(decl + start, name, nlen) == 0)
        return 1;
    /* Tag form: typedef enum Name / typedef struct Name */
    p = 0;
    while (p < len && (decl[p] == ' ' || decl[p] == '\t' ||
                       decl[p] == '\n' || decl[p] == '\r')) p++;
    if (p + 7 > len || memcmp(decl + p, "typedef", 7) != 0 ||
        (p + 7 < len && cc_is_ident_char(decl[p + 7]))) return 0;
    p = cc_skip_ws_and_comments(decl, len, p + 7);
    if (p + 4 <= len && memcmp(decl + p, "enum", 4) == 0 &&
        (p + 4 == len || !cc_is_ident_char(decl[p + 4])))
        p = cc_skip_ws_and_comments(decl, len, p + 4);
    else if (p + 6 <= len && memcmp(decl + p, "struct", 6) == 0 &&
             (p + 6 == len || !cc_is_ident_char(decl[p + 6])))
        p = cc_skip_ws_and_comments(decl, len, p + 6);
    else if (p + 5 <= len && memcmp(decl + p, "union", 5) == 0 &&
             (p + 5 == len || !cc_is_ident_char(decl[p + 5])))
        p = cc_skip_ws_and_comments(decl, len, p + 5);
    else return 0;
    if (p + nlen <= len && memcmp(decl + p, name, nlen) == 0 &&
        (p + nlen == len || !cc_is_ident_char(decl[p + nlen])))
        return 1;
    return 0;
}

/* Extract a top-level `typedef … Name;` from `src`.  Caller frees. */
static char* cc__sm_extract_typedef_from(const char* src, size_t n, const char* name) {
    CCScannerState scan;
    size_t i = 0, depth = 0;
    if (!src || !name || !name[0]) return NULL;
    cc_scanner_init(&scan);
    while (i < n) {
        size_t start, semi;
        char* out;
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (src[i] == '{') { depth++; i++; continue; }
        if (src[i] == '}') { if (depth) depth--; i++; continue; }
        if (depth != 0) { i++; continue; }
        if (!cc_match_ident_kw(src, n, i, "typedef")) { i++; continue; }
        start = i;
        semi = cc__span_to_top_semicolon(src, n, i + 7);
        if (semi == 0 || semi > n) { i++; continue; }
        if (cc__sm_typedef_defines(src + start, semi - start, name)) {
            out = (char*)malloc(semi - start + 1);
            if (!out) return NULL;
            memcpy(out, src + start, semi - start);
            out[semi - start] = '\0';
            return out;
        }
        i = semi;
    }
    return NULL;
}

/* Find `typedef … name;` in the TU text or any registered included .cch. */
static char* cc__sm_find_typedef(const char* src, size_t n, const char* name) {
    char* d = cc__sm_extract_typedef_from(src, n, name);
    size_t h;
    if (d) return d;
    for (h = 0; h < g_included_cch_source_count; h++) {
        size_t fn = 0;
        const char* fsrc = cc__included_cch_text(h, &fn);
        if (!fsrc) continue;
        d = cc__sm_extract_typedef_from(fsrc, fn, name);
        if (d) return d;
    }
    return NULL;
}

/* Install entry/value/enum typedefs into the comptime executor prelude so the
 * typed entry array can compile inside the standalone comptime TU. */
static int cc__sm_install_types_for_call(const char* src, size_t n,
                                         const char* entry_type,
                                         const char* value_type) {
    CCCtField* vfields = NULL;
    size_t vnf = 0;
    size_t f;
    char* vdecl;
    char* edecl;
    if (!entry_type || !value_type) return 0;

    /* Named types used by value fields first (enums / nested structs). */
    if (cc_ct_reflect_struct_fields(src, n, value_type, &vfields, &vnf)) {
        for (f = 0; f < vnf; f++) {
            const char* t = vfields[f].type;
            char tbuf[160];
            const char* ts;
            size_t tl;
            char* d;
            char nm[160];
            if (!t) continue;
            snprintf(tbuf, sizeof(tbuf), "%s", t);
            ts = tbuf; tl = strlen(ts);
            cc__sm_trim(&ts, &tl);
            if (tl == 0 || tl >= sizeof(tbuf)) continue;
            for (;;) {
                if (tl > 5 && memcmp(ts, "const", 5) == 0 &&
                    (ts[5] == ' ' || ts[5] == '\t')) {
                    ts += 5; tl -= 5; cc__sm_trim(&ts, &tl); continue;
                }
                if (tl > 8 && memcmp(ts, "volatile", 8) == 0 &&
                    (ts[8] == ' ' || ts[8] == '\t')) {
                    ts += 8; tl -= 8; cc__sm_trim(&ts, &tl); continue;
                }
                break;
            }
            if (tl == 0 || ts[tl - 1] == '*') continue;
            if (tl + 1 >= sizeof(nm)) continue;
            memcpy(nm, ts, tl); nm[tl] = '\0';
            /* Skip obvious C scalar spellings — only named typedefs matter. */
            if (!strcmp(nm, "char") || !strcmp(nm, "int") || !strcmp(nm, "short") ||
                !strcmp(nm, "long") || !strcmp(nm, "unsigned") || !strcmp(nm, "signed") ||
                !strcmp(nm, "size_t") || !strcmp(nm, "bool") || !strcmp(nm, "_Bool") ||
                strstr(nm, "int8_t") || strstr(nm, "int16_t") || strstr(nm, "int32_t") ||
                strstr(nm, "int64_t") || strstr(nm, "uint8_t") || strstr(nm, "uint16_t") ||
                strstr(nm, "uint32_t") || strstr(nm, "uint64_t"))
                continue;
            d = cc__sm_find_typedef(src, n, nm);
            if (d) {
                cc_comptime_fn_registry_append_prelude(d);
                free(d);
            }
        }
        cc_ct_free_fields(vfields, vnf);
    }

    vdecl = cc__sm_find_typedef(src, n, value_type);
    if (vdecl) {
        cc_comptime_fn_registry_append_prelude(vdecl);
        free(vdecl);
    }
    edecl = cc__sm_find_typedef(src, n, entry_type);
    if (edecl) {
        cc_comptime_fn_registry_append_prelude(edecl);
        free(edecl);
    }
    return 1;
}

char* cc_rewrite_static_map_calls_text(const char* src, size_t n, const char* input_path) {
    if (!src || n == 0) return NULL;
    if (!strstr(src, "static_map")) return NULL;

    CCSmBuf out = { 0 };
    int changed = 0;
    int failed = 0;
    size_t i = 0;
    CCScannerState s; cc_scanner_init(&s);

    while (i < n) {
        size_t before = i;
        if (cc_scanner_skip_non_code(&s, src, n, &i)) {
            if (!cc__sm_append(&out, src + before, i - before)) { failed = 1; break; }
            continue;
        }
        if (!cc_match_ident_kw(src, n, i, "static_map")) {
            if (!cc__sm_append(&out, src + i, 1)) { failed = 1; break; }
            i++;
            continue;
        }
        /* Candidate `static_map` token: locate its argument list. */
        size_t j = cc_skip_ws_and_comments(src, n, i + (sizeof("static_map") - 1));
        size_t rparen = 0;
        if (j >= n || src[j] != '(' || !cc_find_matching_paren(src, n, j, &rparen)) {
            if (!cc__sm_append(&out, src + i, 1)) { failed = 1; break; }
            i++;
            continue;
        }
        size_t starts[8], ends[8];
        int nargs = cc__sm_split_args(src, j, rparen, starts, ends, 8);
        if (nargs != 3) {
            /* Header definition (typed params) or already-expanded call: copy. */
            if (!cc__sm_append(&out, src + i, 1)) { failed = 1; break; }
            i++;
            continue;
        }
        const char* a0 = src + starts[0]; size_t a0l = ends[0] - starts[0];
        const char* a1 = src + starts[1]; size_t a1l = ends[1] - starts[1];
        const char* a2 = src + starts[2]; size_t a2l = ends[2] - starts[2];
        cc__sm_trim(&a0, &a0l);
        cc__sm_trim(&a1, &a1l);
        cc__sm_trim(&a2, &a2l);
        if (a0l == 0 || a0[0] != '"') {
            if (!cc__sm_append(&out, src + i, 1)) { failed = 1; break; }
            i++;
            continue;
        }
        /* arg1 must be a plain array-variable identifier. */
        int simple = a1l > 0;
        for (size_t k = 0; simple && k < a1l; k++)
            if (!cc_is_ident_char(a1[k])) simple = 0;
        if (!simple) {
            fprintf(stderr, "%s: error: static_map: second argument must be a "
                            "typed entry array variable\n",
                    input_path ? input_path : "<input>");
            failed = 1; break;
        }

        char entry_type[192];
        if (!cc__sm_find_entry_type(src, n, a1, a1l, i, entry_type, sizeof(entry_type))) {
            fprintf(stderr, "%s: error: static_map: cannot find the declaration of "
                            "entry array '%.*s'\n",
                    input_path ? input_path : "<input>", (int)a1l, a1);
            failed = 1; break;
        }
        CCCtField* fields = NULL;
        size_t nf = 0;
        if (!cc_ct_reflect_struct_fields(src, n, entry_type, &fields, &nf)) {
            fprintf(stderr, "%s: error: static_map: cannot reflect entry type '%s' "
                            "of array '%.*s'\n",
                    input_path ? input_path : "<input>", entry_type, (int)a1l, a1);
            failed = 1; break;
        }
        char value_type[160];
        value_type[0] = '\0';
        for (size_t f = 0; f < nf; f++) {
            if (fields[f].name && strcmp(fields[f].name, "value") == 0) {
                snprintf(value_type, sizeof(value_type), "%s", fields[f].type ? fields[f].type : "");
                break;
            }
        }
        cc_ct_free_fields(fields, nf);
        if (!value_type[0]) {
            fprintf(stderr, "%s: error: static_map: entry type '%s' has no 'value' field\n",
                    input_path ? input_path : "<input>", entry_type);
            failed = 1; break;
        }

        /* Typed arrays live in the comptime TU; install their types there. */
        (void)cc__sm_install_types_for_call(src, n, entry_type, value_type);

        /* Build the fully-typed internal call.  Layout crosses as real C:
         * sizeof for count/stride/value_size and address arithmetic for the
         * key/value offsets — no stringified initializer anywhere. */
        char repl[1024];
        char v[192];
        snprintf(v, sizeof(v), "%.*s", (int)a1l, a1);
        int rn = snprintf(repl, sizeof(repl),
            "static_map(%.*s, \"%s\", (const void*)(%s), "
            "sizeof(%s) / sizeof((%s)[0]), sizeof((%s)[0]), "
            "(size_t)((const char*)&(%s)[0].key - (const char*)&(%s)[0]), "
            "(size_t)((const char*)&(%s)[0].value - (const char*)&(%s)[0]), "
            "sizeof((%s)[0].value), %.*s)",
            (int)a0l, a0, value_type,
            v, v, v, v, v, v, v, v, v, (int)a2l, a2);
        if (rn < 0 || (size_t)rn >= sizeof(repl)) {
            fprintf(stderr, "%s: error: static_map: rewritten call too large\n",
                    input_path ? input_path : "<input>");
            failed = 1; break;
        }
        if (!cc__sm_append(&out, repl, (size_t)rn)) { failed = 1; break; }
        changed = 1;
        i = rparen + 1;
        s.at_line_start = 0;
    }

    if (failed) { free(out.p); return (char*)-1; }
    if (!changed) { free(out.p); return NULL; }
    if (!out.p) return NULL;
    return out.p;
}

/* ---- module export directives ----------------------------------------
 *
 * A module embedding declares its export sugar in its own header, beside
 * the CC_MODULE_ENTRY it guarantees:
 *
 *     CC_MODULE_EXPORT(cc_py_export,
 *         "$each{void *PyInit_$name(void) { ... }\n}")
 *
 * and `@comptime <directive>("module", "Type", seed[, "member"]);`
 * sites in a TU expand from that template — the module name always
 * explicit, sites sharing one module aggregating into it.  The
 * compiler implements one template language — `$module` (the group's
 * published name), `$T` (the exported type), `$name` (its snake-case
 * member name, or the string override), `$seed` (the seed expression),
 * `$count` (sites in the group) — with two region forms.  A grouped
 * template spells `$groups{...}` regions (several allowed), each
 * expanded once per distinct module with an optional inner `$each{...}`
 * expanded per site in that group; the whole expansion is one aggregate
 * stanza at the TU's LAST site, where every seed static is in scope.
 * The legacy forms remain for other embeddings: a template that is
 * nothing but one `$each` region expands every site in place, and text
 * around one `$each` aggregates all sites into a single stanza.  The
 * spliced text is the same registration a hand-written module spells —
 * the explicit stanza stays legal, and everything downstream (parse,
 * factories, codegen) cannot tell the two apart. */

static char* cc__mex_read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    char* buf;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 16 * 1024 * 1024) { fclose(f); return NULL; }
    buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Resolve a `<ccc/…>` include against the roots a build can run from:
 * the input's repo root (walking up for cc/include), the compiler's own
 * install tree, and CC_INCLUDE_PATH.  The single resolver shared with
 * the driver's module-entry detection. */
char* cc_module_header_read_text(const char* rel, const char* in_path) {
    char cand[PATH_MAX];
    char base[PATH_MAX];
    char* text;
    if (!rel || !in_path) return NULL;
    /* A buffer that already went through include lowering spells the
     * header as its `.h` product; the declarations live in the `.cch`
     * source, so try the twin when the literal name misses. */
    {
        size_t rl = strlen(rel);
        if (rl > 2 && strcmp(rel + rl - 2, ".h") == 0 && rl + 3 < PATH_MAX) {
            char twin[192 + 4];
            snprintf(twin, sizeof(twin), "%.*s.cch", (int)(rl - 2), rel);
            text = cc_module_header_read_text(twin, in_path);
            if (text) return text;
        }
    }
    {
        const char* rr = getenv("CC_REPO_ROOT");
        if (rr && rr[0]) {
            snprintf(cand, sizeof(cand), "%s/cc/include/%s", rr, rel);
            if ((text = cc__mex_read_file(cand)) != NULL) return text;
        }
    }
    if (realpath(in_path, base)) {
        char* slash;
        while ((slash = strrchr(base, '/')) != NULL) {
            *slash = 0;
            if (!base[0]) break;
            snprintf(cand, sizeof(cand), "%s/cc/include/%s", base, rel);
            if ((text = cc__mex_read_file(cand)) != NULL) return text;
        }
    }
    {
        ssize_t sn = readlink("/proc/self/exe", base, sizeof(base) - 1);
        if (sn > 0) {
            char* slash;
            base[sn] = 0;
            slash = strrchr(base, '/');
            if (slash) {
                *slash = 0;
                snprintf(cand, sizeof(cand), "%s/../include/%s", base, rel);
                if ((text = cc__mex_read_file(cand)) != NULL) return text;
                snprintf(cand, sizeof(cand), "%s/../../cc/include/%s", base, rel);
                if ((text = cc__mex_read_file(cand)) != NULL) return text;
            }
        }
    }
    {
        const char* env = getenv("CC_INCLUDE_PATH");
        if (env && env[0]) {
            char paths[2048];
            char* p;
            snprintf(paths, sizeof(paths), "%s", env);
            p = paths;
            while (p && *p) {
                char* sep = strchr(p, ':');
                if (sep) *sep = 0;
                if (*p) {
                    snprintf(cand, sizeof(cand), "%s/%s", p, rel);
                    if ((text = cc__mex_read_file(cand)) != NULL) return text;
                }
                p = sep ? sep + 1 : NULL;
            }
        }
    }
    return NULL;
}

/* Append `<ccc/…>` include paths found in `buf` to rels[], deduplicated.
 * Returns the new count. */
int cc_module_collect_ccc_includes(const char* buf, char (*rels)[192], int cap,
                                   int nrel) {
    const char* p = buf;
    while ((p = strstr(p, "#include")) != NULL && nrel < cap) {
        const char* lt = p + 8;
        const char* gt;
        size_t rl;
        int k, dup = 0;
        p += 8;
        while (*lt == ' ' || *lt == '\t') lt++;
        if (*lt != '<') continue;
        lt++;
        gt = strchr(lt, '>');
        if (!gt) continue;
        rl = (size_t)(gt - lt);
        if (rl == 0 || rl >= 192) continue;
        if (strncmp(lt, "ccc/", 4) != 0) continue;
        for (k = 0; k < nrel; k++)
            if (strncmp(rels[k], lt, rl) == 0 && rels[k][rl] == 0) { dup = 1; break; }
        if (dup) continue;
        memcpy(rels[nrel], lt, rl);
        rels[nrel][rl] = 0;
        nrel++;
    }
    return nrel;
}

typedef struct {
    char directive[64];
    char* tpl; /* malloc'd, escapes decoded, adjacent literals joined */
} CCMexTpl;

/* Parse `CC_MODULE_EXPORT(<ident>, "…" "…")` declarations out of one
 * header text.  The `#define CC_MODULE_EXPORT(...)` guard parses as no
 * declaration — its first argument is `...`, not an identifier. */
static int cc__mex_scan_tpls(const char* text, CCMexTpl* t, int cap, int nt) {
    size_t n = strlen(text);
    size_t i = 0;
    while (nt < cap) {
        const char* hit = strstr(text + i, "CC_MODULE_EXPORT");
        size_t j, dl;
        char dir[64];
        char* tpl = NULL;
        size_t tl = 0, tc = 0;
        if (!hit) break;
        i = (size_t)(hit - text);
        j = i + (sizeof("CC_MODULE_EXPORT") - 1);
        if ((i > 0 && cc_is_ident_char(text[i - 1])) ||
            (j < n && cc_is_ident_char(text[j]))) { i = j; continue; }
        j = cc_skip_ws_and_comments(text, n, j);
        if (j >= n || text[j] != '(') { i = j; continue; }
        j = cc_skip_ws_and_comments(text, n, j + 1);
        dl = 0;
        while (j < n && cc_is_ident_char(text[j]) && dl + 1 < sizeof(dir))
            dir[dl++] = text[j++];
        dir[dl] = 0;
        if (!dl) { i = j; continue; }
        j = cc_skip_ws_and_comments(text, n, j);
        if (j >= n || text[j] != ',') { i = j; continue; }
        j = cc_skip_ws_and_comments(text, n, j + 1);
        while (j < n && text[j] == '"') {
            char seg[2048];
            if (!cc_parse_c_string_literal(text, n, &j, seg, sizeof(seg))) {
                free(tpl);
                tpl = NULL;
                tl = 0;
                break;
            }
            cc_sb_append(&tpl, &tl, &tc, seg, strlen(seg));
            j = cc_skip_ws_and_comments(text, n, j);
        }
        if (!tpl || !tl || j >= n || text[j] != ')') { free(tpl); i = j; continue; }
        {
            int k, dup = 0;
            for (k = 0; k < nt; k++)
                if (strcmp(t[k].directive, dir) == 0) { dup = 1; break; }
            if (dup) { free(tpl); i = j; continue; }
        }
        snprintf(t[nt].directive, sizeof(t[nt].directive), "%s", dir);
        t[nt].tpl = tpl;
        nt++;
        i = j;
    }
    return nt;
}

/* Collect templates from the TU's `<ccc/…>` includes, one nested level —
 * the same depth the driver's entry detection reads. */
static int cc__mex_collect_templates(const char* src, const char* in_path,
                                     CCMexTpl* t, int cap) {
    char rels[24][192];
    int nrel, scanned = 0, nt = 0, depth;
    nrel = cc_module_collect_ccc_includes(src, rels, 24, 0);
    for (depth = 0; depth < 2; depth++) {
        int end = nrel;
        for (; scanned < end; scanned++) {
            char* text = cc_module_header_read_text(rels[scanned], in_path);
            if (!text) continue;
            nt = cc__mex_scan_tpls(text, t, cap, nt);
            if (depth == 0)
                nrel = cc_module_collect_ccc_includes(text, rels, 24, nrel);
            free(text);
        }
    }
    return nt;
}

/* Camel lowered to snake: Counter → counter, RowMap → row_map.  The same
 * rule the driver's factory name-source applies. */
static void cc__mex_snake(const char* t, char* out, size_t cap) {
    size_t m = 0, q;
    for (q = 0; t[q] && m + 2 < cap; q++) {
        char ch = t[q];
        if (ch >= 'A' && ch <= 'Z') {
            if (q > 0) out[m++] = '_';
            out[m++] = (char)(ch - 'A' + 'a');
        } else {
            out[m++] = ch;
        }
    }
    out[m] = 0;
}

typedef struct {
    size_t start; /* at the '@' */
    size_t end;   /* one past the ';' */
    char module[96]; /* the published module this site joins */
    char type_name[96];
    char name[96]; /* member name: snake of the type, or the override */
    char seed[256];
} CCMexSite;

/* Scan the TU for `@comptime <directive>(args);` statements.  Malformed
 * sites are hard errors (quiet mode suppresses the report for the
 * detection pre-scan; the compile pass repeats the scan loudly). */
static int cc__mex_scan_sites(const char* src, size_t n, const char* directive,
                              CCMexSite* sites, int cap, const char* input_path,
                              int quiet, int* out_err) {
    CCScannerState s;
    size_t i = 0;
    int ns = 0;
    size_t dl = strlen(directive);
    const char* in = input_path ? input_path : "<input>";
    cc_scanner_init(&s);
    while (i < n) {
        size_t j, lpar, rpar = 0, semi;
        size_t starts[4], ends[4];
        int nargs;
        CCMexSite* st;
        if (cc_scanner_skip_non_code(&s, src, n, &i)) continue;
        if (src[i] != '@' || !cc_match_ident_kw(src, n, i + 1, "comptime")) {
            i++;
            continue;
        }
        j = cc_skip_ws_and_comments(src, n, i + 1 + (sizeof("comptime") - 1));
        if (!cc_match_ident_kw(src, n, j, directive)) { i++; continue; }
        j = cc_skip_ws_and_comments(src, n, j + dl);
        if (j >= n || src[j] != '(') { i++; continue; }
        lpar = j;
        if (!cc_find_matching_paren(src, n, lpar, &rpar)) {
            if (!quiet)
                fprintf(stderr, "%s: error: %s: unterminated argument list\n",
                        in, directive);
            *out_err = 1;
            return ns;
        }
        semi = cc_skip_ws_and_comments(src, n, rpar + 1);
        if (semi >= n || src[semi] != ';') {
            if (!quiet)
                fprintf(stderr, "%s: error: %s: expected ';' after the export "
                                "directive\n",
                        in, directive);
            *out_err = 1;
            return ns;
        }
        if (ns >= cap) {
            if (!quiet)
                fprintf(stderr, "%s: error: %s: too many export directives in "
                                "one TU (max %d)\n",
                        in, directive, cap);
            *out_err = 1;
            return ns;
        }
        nargs = cc__sm_split_args(src, lpar, rpar, starts, ends, 4);
        st = &sites[ns];
        memset(st, 0, sizeof *st);
        st->start = i;
        st->end = semi + 1;
        /* Always explicit: ("module", "Type", seed[, "member_name"]).
         * The module names the published artifact; sites sharing one
         * module aggregate into it.  No short form — the published name
         * never falls out of a file name or a declaration order. */
        if (nargs < 3 || nargs > 4) {
            if (!quiet)
                fprintf(stderr,
                        "%s: error: %s takes (\"module\", \"Type\", seed[, "
                        "\"member_name\"]) — the module name is always "
                        "explicit\n",
                        in, directive);
            *out_err = 1;
            return ns;
        }
        {
            const char* a0 = src + starts[0];
            size_t a0l = ends[0] - starts[0];
            size_t p, q;
            int ok;
            cc__sm_trim(&a0, &a0l);
            p = (size_t)(a0 - src);
            if (a0l == 0 || a0[0] != '"' ||
                !cc_parse_c_string_literal(src, n, &p, st->module,
                                           sizeof(st->module))) {
                if (!quiet)
                    fprintf(stderr,
                            "%s: error: %s: the module name must be a string "
                            "literal — %s(\"mymodule\", \"Type\", &seed)\n",
                            in, directive, directive);
                *out_err = 1;
                return ns;
            }
            ok = st->module[0] != 0 &&
                 !(st->module[0] >= '0' && st->module[0] <= '9');
            for (q = 0; ok && st->module[q]; q++)
                if (!cc_is_ident_char(st->module[q])) ok = 0;
            if (!ok) {
                if (!quiet)
                    fprintf(stderr,
                            "%s: error: %s: module name '%s' must be a C "
                            "identifier (it names entry symbols and "
                            "artifacts)\n",
                            in, directive, st->module);
                *out_err = 1;
                return ns;
            }
        }
        {
            const char* a1 = src + starts[1];
            size_t a1l = ends[1] - starts[1];
            cc__sm_trim(&a1, &a1l);
            if (a1l > 0 && a1[0] == '"') {
                size_t p = (size_t)(a1 - src);
                if (!cc_parse_c_string_literal(src, n, &p, st->type_name,
                                               sizeof(st->type_name)))
                    st->type_name[0] = 0;
            } else if (a1l > 0 && a1l < sizeof(st->type_name)) {
                memcpy(st->type_name, a1, a1l);
                st->type_name[a1l] = 0;
            }
            {
                size_t q;
                int ok = st->type_name[0] != 0 &&
                         !(st->type_name[0] >= '0' && st->type_name[0] <= '9');
                for (q = 0; ok && st->type_name[q]; q++)
                    if (!cc_is_ident_char(st->type_name[q])) ok = 0;
                if (!ok) {
                    if (!quiet)
                        fprintf(stderr,
                                "%s: error: %s: '%.*s' is not a type name\n",
                                in, directive, (int)a1l, a1);
                    *out_err = 1;
                    return ns;
                }
            }
        }
        {
            const char* a2 = src + starts[2];
            size_t a2l = ends[2] - starts[2];
            cc__sm_trim(&a2, &a2l);
            if (a2l == 0 || a2l >= sizeof(st->seed)) {
                if (!quiet)
                    fprintf(stderr, "%s: error: %s: seed expression missing or "
                                    "too long\n",
                            in, directive);
                *out_err = 1;
                return ns;
            }
            memcpy(st->seed, a2, a2l);
            st->seed[a2l] = 0;
        }
        if (nargs >= 4) {
            const char* a3 = src + starts[3];
            size_t a3l = ends[3] - starts[3];
            size_t p;
            cc__sm_trim(&a3, &a3l);
            p = (size_t)(a3 - src);
            if (a3l == 0 || a3[0] != '"' ||
                !cc_parse_c_string_literal(src, n, &p, st->name,
                                           sizeof(st->name))) {
                if (!quiet)
                    fprintf(stderr,
                            "%s: error: %s: the member-name override must be "
                            "a string literal\n",
                            in, directive);
                *out_err = 1;
                return ns;
            }
        } else {
            cc__mex_snake(st->type_name, st->name, sizeof(st->name));
        }
        ns++;
        i = semi + 1;
    }
    return ns;
}

/* Split `head $each{body} tail`.  Exactly one region, braces balanced. */
static int cc__mex_tpl_split(const char* tpl, const char** head, size_t* hl,
                             const char** body, size_t* bl, const char** tail,
                             size_t* tl) {
    const char* e = strstr(tpl, "$each{");
    const char* b;
    const char* p;
    size_t depth = 1;
    if (!e || strstr(e + 1, "$each{")) return 0;
    b = e + 6;
    p = b;
    while (*p) {
        if (*p == '{') depth++;
        else if (*p == '}') {
            depth--;
            if (!depth) break;
        }
        p++;
    }
    if (depth) return 0;
    *head = tpl;
    *hl = (size_t)(e - tpl);
    *body = b;
    *bl = (size_t)(p - b);
    *tail = p + 1;
    *tl = strlen(p + 1);
    return 1;
}

/* Substitute one site into the $each body. */
static int cc__mex_expand(CCSmBuf* out, const char* body, size_t bl,
                          const CCMexSite* st, int count) {
    size_t i = 0;
    while (i < bl) {
        if (body[i] == '$') {
            if (i + 7 <= bl && memcmp(body + i, "$module", 7) == 0 &&
                (i + 7 == bl || !cc_is_ident_char(body[i + 7]))) {
                if (!cc__sm_append(out, st->module, strlen(st->module)))
                    return 0;
                i += 7;
                continue;
            }
            if (i + 5 <= bl && memcmp(body + i, "$name", 5) == 0 &&
                (i + 5 == bl || !cc_is_ident_char(body[i + 5]))) {
                if (!cc__sm_append(out, st->name, strlen(st->name))) return 0;
                i += 5;
                continue;
            }
            if (i + 5 <= bl && memcmp(body + i, "$seed", 5) == 0 &&
                (i + 5 == bl || !cc_is_ident_char(body[i + 5]))) {
                if (!cc__sm_append(out, st->seed, strlen(st->seed))) return 0;
                i += 5;
                continue;
            }
            if (i + 6 <= bl && memcmp(body + i, "$count", 6) == 0 &&
                (i + 6 == bl || !cc_is_ident_char(body[i + 6]))) {
                char nb[16];
                int nn = snprintf(nb, sizeof nb, "%d", count);
                if (!cc__sm_append(out, nb, (size_t)nn)) return 0;
                i += 6;
                continue;
            }
            if (i + 2 <= bl && body[i + 1] == 'T' &&
                (i + 2 == bl || !cc_is_ident_char(body[i + 2]))) {
                if (!cc__sm_append(out, st->type_name, strlen(st->type_name)))
                    return 0;
                i += 2;
                continue;
            }
        }
        if (!cc__sm_append(out, body + i, 1)) return 0;
        i++;
    }
    return 1;
}

/* One $groups{...} region expanded per distinct module: group text
 * (with $module / $count) wraps an optional $each expanded per site in
 * the group.  Groups keep first-appearance order; sites keep TU order
 * within a group. */
static int cc__mex_expand_groups_region(CCSmBuf* out, const char* body,
                                        size_t bl, const CCMexSite* sites,
                                        int ns, const char (*mods)[96],
                                        int nmod) {
    char* rb = (char*)malloc(bl + 1);
    const char *head, *ebody, *tail;
    size_t hl, ebl, tl;
    int has_each, m;
    if (!rb) return 0;
    memcpy(rb, body, bl);
    rb[bl] = 0;
    has_each = cc__mex_tpl_split(rb, &head, &hl, &ebody, &ebl, &tail, &tl);
    for (m = 0; m < nmod; m++) {
        const CCMexSite* first = NULL;
        int gcount = 0, si;
        for (si = 0; si < ns; si++) {
            if (strcmp(sites[si].module, mods[m]) != 0) continue;
            if (!first) first = &sites[si];
            gcount++;
        }
        if (!first) continue;
        if (has_each) {
            if (!cc__mex_expand(out, head, hl, first, gcount)) goto fail;
            for (si = 0; si < ns; si++) {
                if (strcmp(sites[si].module, mods[m]) != 0) continue;
                if (!cc__mex_expand(out, ebody, ebl, &sites[si], gcount))
                    goto fail;
            }
            if (!cc__mex_expand(out, tail, tl, first, gcount)) goto fail;
        } else {
            if (!cc__mex_expand(out, rb, bl, first, gcount)) goto fail;
        }
    }
    free(rb);
    return 1;
fail:
    free(rb);
    return 0;
}

/* Grouped template: literal segments interleaved with $groups{...}
 * regions (several allowed), the whole expansion one aggregate stanza.
 * Returns 0 on malformed region braces. */
static int cc__mex_expand_grouped(CCSmBuf* out, const char* tpl,
                                  const CCMexSite* sites, int ns) {
    char mods[16][96];
    int nmod = 0, si;
    const char* p = tpl;
    for (si = 0; si < ns; si++) {
        int m, seen = 0;
        for (m = 0; m < nmod; m++)
            if (strcmp(mods[m], sites[si].module) == 0) { seen = 1; break; }
        if (!seen && nmod < 16) {
            snprintf(mods[nmod], sizeof(mods[0]), "%s", sites[si].module);
            nmod++;
        }
    }
    while (*p) {
        const char* g = strstr(p, "$groups{");
        const char* b;
        const char* q;
        size_t depth = 1;
        if (!g) return cc__sm_append(out, p, strlen(p));
        if (!cc__sm_append(out, p, (size_t)(g - p))) return 0;
        b = g + 8;
        q = b;
        while (*q) {
            if (*q == '{') depth++;
            else if (*q == '}') {
                depth--;
                if (!depth) break;
            }
            q++;
        }
        if (depth) return 0;
        if (!cc__mex_expand_groups_region(out, b, (size_t)(q - b), sites, ns,
                                          mods, nmod))
            return 0;
        p = q + 1;
    }
    return 1;
}

/* Detection pre-scan for the driver: does the TU spell this directive,
 * and what artifact does the first site name?  Quiet — a malformed site
 * reads as "not a module" here and errors articulately at compile. */
int cc_module_export_tu_artifact(const char* src, size_t n,
                                 const char* directive, char* name_out,
                                 size_t cap) {
    CCMexSite sites[16];
    int err = 0, ns;
    if (!src || !directive || !directive[0] || !name_out || cap == 0) return 0;
    if (!strstr(src, directive)) return 0;
    ns = cc__mex_scan_sites(src, n, directive, sites,
                            (int)(sizeof(sites) / sizeof(sites[0])), NULL, 1,
                            &err);
    if (err || ns <= 0) return 0;
    snprintf(name_out, cap, "%s", sites[0].module);
    return ns;
}

/* Every distinct MODULE name, in first-appearance order — each group is
 * one published artifact (PyInit_counters and counters.node both come
 * from the group name, and a second group is a second name). */
int cc_module_export_tu_artifact_all(const char* src, size_t n,
                                     const char* directive,
                                     char names[][128], int max) {
    CCMexSite sites[16];
    int err = 0, ns, i, out = 0;
    if (!src || !directive || !directive[0] || !names || max <= 0) return 0;
    if (!strstr(src, directive)) return 0;
    ns = cc__mex_scan_sites(src, n, directive, sites,
                            (int)(sizeof(sites) / sizeof(sites[0])), NULL, 1,
                            &err);
    if (err || ns <= 0) return 0;
    for (i = 0; i < ns && out < max; i++) {
        int m, seen = 0;
        for (m = 0; m < out; m++)
            if (strcmp(names[m], sites[i].module) == 0) { seen = 1; break; }
        if (seen) continue;
        snprintf(names[out], 128, "%s", sites[i].module);
        out++;
    }
    return out;
}

char* cc_rewrite_module_export_directives_text(const char* src, size_t n,
                                               const char* input_path) {
    CCMexTpl tpls[8];
    typedef struct {
        size_t start, end;
        char* text;
    } CCMexRepl;
    CCMexRepl repls[128];
    int nt = 0, nr = 0, ti, failed = 0;
    if (!src || n == 0) return NULL;
    /* Cheap gate before any header read: a bare `@comptime <ident>(`
     * statement shape must appear (`if`/`for` and block forms excluded). */
    {
        int shape = 0;
        const char* p = src;
        while ((p = strstr(p, "@comptime")) != NULL) {
            const char* q = p + 9;
            char id[64];
            size_t m = 0;
            p += 9;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            while (cc_is_ident_char(*q) && m + 1 < sizeof(id)) id[m++] = *q++;
            id[m] = 0;
            if (!m || strcmp(id, "if") == 0 || strcmp(id, "for") == 0) continue;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '(') { shape = 1; break; }
        }
        if (!shape) return NULL;
    }
    nt = cc__mex_collect_templates(src, input_path, tpls,
                                   (int)(sizeof(tpls) / sizeof(tpls[0])));
    if (nt <= 0) return NULL;

    for (ti = 0; ti < nt && !failed; ti++) {
        CCMexSite sites[16];
        int ns, err = 0, si;
        const char *head, *body, *tail;
        size_t hl, bl, tl;
        ns = cc__mex_scan_sites(src, n, tpls[ti].directive, sites,
                                (int)(sizeof(sites) / sizeof(sites[0])),
                                input_path, 0, &err);
        if (err) { failed = 1; break; }
        if (ns <= 0) continue;
        if (nr + ns > (int)(sizeof(repls) / sizeof(repls[0]))) { failed = 1; break; }
        if (strstr(tpls[ti].tpl, "$groups{")) {
            /* Grouped template: one aggregate stanza at the LAST site,
             * each $groups region expanded per distinct module. */
            CCSmBuf eb = { 0 };
            if (!cc__mex_expand_grouped(&eb, tpls[ti].tpl, sites, ns)) {
                fprintf(stderr,
                        "%s: error: %s: malformed $groups{...} region in the "
                        "CC_MODULE_EXPORT template\n",
                        input_path ? input_path : "<input>",
                        tpls[ti].directive);
                free(eb.p);
                failed = 1;
                break;
            }
            for (si = 0; si < ns - 1; si++) {
                repls[nr].start = sites[si].start;
                repls[nr].end = sites[si].end;
                repls[nr].text = (char*)calloc(1, 1);
                nr++;
            }
            repls[nr].start = sites[ns - 1].start;
            repls[nr].end = sites[ns - 1].end;
            repls[nr].text = eb.p ? eb.p : (char*)calloc(1, 1);
            nr++;
            continue;
        }
        if (!cc__mex_tpl_split(tpls[ti].tpl, &head, &hl, &body, &bl, &tail,
                               &tl)) {
            fprintf(stderr, "%s: error: %s: CC_MODULE_EXPORT template needs "
                            "exactly one $each{...} region\n",
                    input_path ? input_path : "<input>", tpls[ti].directive);
            failed = 1;
            break;
        }
        if (nr + ns > (int)(sizeof(repls) / sizeof(repls[0]))) { failed = 1; break; }
        if (hl == 0 && tl == 0) {
            /* Independent stanzas: each site expands in place. */
            for (si = 0; si < ns && !failed; si++) {
                CCSmBuf eb = { 0 };
                if (!cc__mex_expand(&eb, body, bl, &sites[si], ns)) {
                    free(eb.p);
                    failed = 1;
                    break;
                }
                repls[nr].start = sites[si].start;
                repls[nr].end = sites[si].end;
                repls[nr].text = eb.p ? eb.p : (char*)calloc(1, 1);
                nr++;
            }
        } else {
            /* One aggregate stanza at the LAST site — every seed static
             * is in scope by then; earlier sites vanish. */
            CCSmBuf eb = { 0 };
            int ok = cc__sm_append(&eb, head, hl);
            for (si = 0; ok && si < ns; si++)
                ok = cc__mex_expand(&eb, body, bl, &sites[si], ns);
            ok = ok && cc__sm_append(&eb, tail, tl);
            if (!ok) {
                free(eb.p);
                failed = 1;
                break;
            }
            for (si = 0; si < ns - 1; si++) {
                repls[nr].start = sites[si].start;
                repls[nr].end = sites[si].end;
                repls[nr].text = (char*)calloc(1, 1);
                nr++;
            }
            repls[nr].start = sites[ns - 1].start;
            repls[nr].end = sites[ns - 1].end;
            repls[nr].text = eb.p ? eb.p : (char*)calloc(1, 1);
            nr++;
        }
    }
    for (ti = 0; ti < nt; ti++) free(tpls[ti].tpl);
    if (failed || nr == 0) {
        int r;
        for (r = 0; r < nr; r++) free(repls[r].text);
        return failed ? (char*)-1 : NULL;
    }
    /* Splice, ordered by site position (directives may interleave). */
    {
        CCSmBuf out = { 0 };
        size_t pos = 0;
        int r, k, ok = 1;
        for (r = 1; r < nr; r++) {
            CCMexRepl tmp = repls[r];
            for (k = r; k > 0 && repls[k - 1].start > tmp.start; k--)
                repls[k] = repls[k - 1];
            repls[k] = tmp;
        }
        for (r = 0; r < nr && ok; r++) {
            ok = cc__sm_append(&out, src + pos, repls[r].start - pos) &&
                 cc__sm_append(&out, repls[r].text,
                               repls[r].text ? strlen(repls[r].text) : 0);
            pos = repls[r].end;
        }
        ok = ok && cc__sm_append(&out, src + pos, n - pos);
        for (r = 0; r < nr; r++) free(repls[r].text);
        if (!ok) { free(out.p); return (char*)-1; }
        return out.p;
    }
}

/* ---- enum reflection (edge-push #1) ---------------------------------- */

static void cc__ct_free_enum_members(CCCtEnumMember* m, size_t n) {
    if (!m) return;
    for (size_t i = 0; i < n; i++) free(m[i].name);
    free(m);
}

/* Locate the `{...}` body of the enum named `tname` (a typedef name, or an
 * `enum Tag` spelling).  Sets [*bo,*bc] to the brace offsets and returns 1; 0
 * if not found.  Mirrors cc__ct_find_struct_body for the `enum` keyword. */
static int cc__ct_find_enum_body(const char* src, size_t n,
                                 const char* tname, size_t tlen,
                                 size_t* bo, size_t* bc) {
    int want_tag = 0;
    const char* tag = tname; size_t taglen = tlen;
    if (tlen > 5 && memcmp(tname, "enum ", 5) == 0) { want_tag = 1; tag = tname + 5; taglen = tlen - 5; }
    while (taglen && (tag[0] == ' ' || tag[0] == '\t')) { tag++; taglen--; }
    while (taglen && (tag[taglen - 1] == ' ' || tag[taglen - 1] == '\t')) taglen--;

    CCScannerState scan; cc_scanner_init(&scan);
    size_t i = 0, depth = 0;
    while (i < n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        char c = src[i];
        if (c == '{') { depth++; i++; continue; }
        if (c == '}') { if (depth) depth--; i++; continue; }
        if (depth != 0) { i++; continue; }
        if (i > 0 && cc_is_ident_char(src[i - 1])) { i++; continue; }
        int is_typedef = 0; size_t kwlen = 0;
        if (i + 7 <= n && memcmp(src + i, "typedef", 7) == 0 &&
            (i + 7 == n || !cc_is_ident_char(src[i + 7]))) { kwlen = 7; is_typedef = 1; }
        else if (i + 4 <= n && memcmp(src + i, "enum", 4) == 0 &&
                 (i + 4 == n || !cc_is_ident_char(src[i + 4]))) kwlen = 4;
        if (!kwlen) { i++; continue; }
        if (is_typedef) {
            size_t semi = cc__span_to_top_semicolon(src, n, i + kwlen);
            size_t b1 = 0; int havebrace = 0;
            CCScannerState s2; cc_scanner_init(&s2);
            size_t j = i + kwlen;
            while (j < semi) {
                if (cc_scanner_skip_non_code(&s2, src, semi, &j)) continue;
                if (src[j] == '{') { b1 = j; havebrace = 1; break; }
                j++;
            }
            if (!want_tag && havebrace) {
                /* Only a `typedef enum {…} Name;` qualifies (not typedef struct). */
                size_t kw = cc_skip_ws_and_comments(src, n, i + kwlen);
                int is_enum = (kw + 4 <= n && memcmp(src + kw, "enum", 4) == 0 &&
                               (kw + 4 == n || !cc_is_ident_char(src[kw + 4])));
                if (is_enum) {
                    size_t b2;
                    if (cc_find_matching_brace(src, n, b1, &b2) && semi > 0) {
                        size_t s = semi - 1;
                        while (s > b2 && src[s] != ';') s--;
                        /* Comment-aware: `} Name / *c* / ;` still reads Name */
                        size_t ne = cc_rskip_ws_and_comments(src, s);
                        size_t ns = ne;
                        while (ns > b2 && cc_is_ident_char(src[ns - 1])) ns--;
                        if (ne > ns && ns > b2 && (ne - ns) == tlen && memcmp(src + ns, tname, tlen) == 0) {
                            *bo = b1; *bc = b2; return 1;
                        }
                    }
                }
            }
            i = semi; continue;
        }
        /* enum [TAG] { ... } */
        size_t p = cc_skip_ws_and_comments(src, n, i + kwlen);
        size_t tags = p;
        while (p < n && cc_is_ident_char(src[p])) p++;
        size_t tage = p;
        size_t bp = cc_skip_ws_and_comments(src, n, p);
        if (bp < n && src[bp] == '{') {
            size_t b2;
            if (cc_find_matching_brace(src, n, bp, &b2)) {
                if (want_tag && tage > tags && (tage - tags) == taglen &&
                    memcmp(src + tags, tag, taglen) == 0) {
                    *bo = bp; *bc = b2; return 1;
                }
                i = b2 + 1; continue;
            }
        }
        i += kwlen;
    }
    return 0;
}

/* Parse `IDENT [= integer-literal]` enumerators in an enum body (bo='{',
 * bc='}') with C auto-increment.  Returns 1 on success (free *out via
 * cc__ct_free_enum_members); 0 if any member is not modeled (see header). */
static int cc__ct_parse_enum_members_from_body(const char* src, size_t bo, size_t bc,
                                               CCCtEnumMember** out, size_t* out_n) {
    CCCtEnumMember* ms = NULL; size_t mn = 0, mc = 0;
    long long next_val = 0;
    size_t i = bo + 1;
    while (i < bc) {
        /* Find the end of this enumerator: the next top-level ','. */
        CCScannerState s; cc_scanner_init(&s);
        size_t j = i, depth = 0, end = bc;
        while (j < bc) {
            if (cc_scanner_skip_non_code(&s, src, bc, &j)) continue;
            char c = src[j];
            if (c == '(' || c == '[' || c == '{') depth++;
            else if (c == ')' || c == ']' || c == '}') { if (depth) depth--; }
            else if (c == ',' && depth == 0) { end = j; break; }
            j++;
        }
        /* Skip leading whitespace AND comments: a block comment trailing the
         * previous enumerator's comma falls into this item's span. */
        size_t es = cc_skip_ws_and_comments(src, end, i);
        size_t ee = end;
        while (ee > es && (src[ee - 1] == ' ' || src[ee - 1] == '\t' || src[ee - 1] == '\n' || src[ee - 1] == '\r')) ee--;
        if (ee > es) {
            size_t k = es;
            if (!cc_is_ident_start(src[k])) { cc__ct_free_enum_members(ms, mn); return 0; }
            size_t ns = k;
            while (k < ee && cc_is_ident_char(src[k])) k++;
            size_t ne = k;
            while (k < ee && (src[k] == ' ' || src[k] == '\t' || src[k] == '\n' || src[k] == '\r')) k++;
            long long val;
            if (k < ee && src[k] == '=') {
                k++;
                while (k < ee && (src[k] == ' ' || src[k] == '\t' || src[k] == '\n' || src[k] == '\r')) k++;
                size_t vs = k, ve = ee;
                char vbuf[64];
                size_t vlen = ve - vs;
                char* endp = NULL;
                if (vlen == 0 || vlen >= sizeof(vbuf)) { cc__ct_free_enum_members(ms, mn); return 0; }
                memcpy(vbuf, src + vs, vlen); vbuf[vlen] = 0;
                errno = 0;
                val = strtoll(vbuf, &endp, 0);
                while (endp && (*endp == 'u' || *endp == 'U' || *endp == 'l' || *endp == 'L')) endp++;
                if (errno != 0 || endp == vbuf || !endp || *endp != '\0') {
                    /* Non-literal initializer: every member or none. */
                    cc__ct_free_enum_members(ms, mn); return 0;
                }
            } else if (k < ee) {
                /* Trailing junk after the name with no '='. */
                cc__ct_free_enum_members(ms, mn); return 0;
            } else {
                val = next_val;
            }
            next_val = val + 1;
            {
                size_t nlen = ne - ns;
                char* name = (char*)malloc(nlen + 1);
                if (!name) { cc__ct_free_enum_members(ms, mn); return 0; }
                memcpy(name, src + ns, nlen); name[nlen] = 0;
                if (mn + 1 > mc) {
                    size_t nc = mc ? mc * 2 : 8;
                    CCCtEnumMember* nb = (CCCtEnumMember*)realloc(ms, nc * sizeof(CCCtEnumMember));
                    if (!nb) { free(name); cc__ct_free_enum_members(ms, mn); return 0; }
                    ms = nb; mc = nc;
                }
                ms[mn].name = name; ms[mn].value = val; mn++;
            }
        }
        i = end + 1;
    }
    *out = ms; *out_n = mn; return 1;
}

int cc_ct_reflect_enum_members(const char* src, size_t len, const char* type_name,
                               CCCtEnumMember** out, size_t* out_n) {
    if (out) *out = NULL;
    if (out_n) *out_n = 0;
    if (!src || !type_name || !type_name[0] || !out || !out_n) return 0;
    size_t bo, bc;
    if (!cc__ct_find_enum_body(src, len, type_name, strlen(type_name), &bo, &bc))
        return 0;
    return cc__ct_parse_enum_members_from_body(src, bo, bc, out, out_n);
}

void cc_ct_free_enum_members(CCCtEnumMember* members, size_t n) {
    cc__ct_free_enum_members(members, n);
}

/* ---- type-kind classifier (edge-push #2) ----------------------------- */

static int cc__ct_word_is_primitive(const char* w, size_t n) {
    static const char* kws[] = {
        "void", "char", "short", "int", "long", "float", "double",
        "signed", "unsigned", "_Bool", "bool",
    };
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++)
        if (strlen(kws[i]) == n && memcmp(w, kws[i], n) == 0) return 1;
    return 0;
}

int cc_ct_reflect_type_kind(const char* src, size_t len, const char* type_name) {
    if (!type_name) return CC_REFLECT_KIND_UNKNOWN;
    size_t s = 0, e = strlen(type_name);
    while (s < e && (type_name[s] == ' ' || type_name[s] == '\t')) s++;
    while (e > s && (type_name[e - 1] == ' ' || type_name[e - 1] == '\t')) e--;
    if (e <= s) return CC_REFLECT_KIND_UNKNOWN;
    /* Pointer: any spelling ending in '*'. */
    if (type_name[e - 1] == '*') return CC_REFLECT_KIND_POINTER;
    /* Strip leading const/volatile qualifiers. */
    for (;;) {
        size_t ws = s;
        while (ws < e && (type_name[ws] == ' ' || type_name[ws] == '\t')) ws++;
        size_t we = ws;
        while (we < e && type_name[we] != ' ' && type_name[we] != '\t') we++;
        size_t wl = we - ws;
        if ((wl == 5 && memcmp(type_name + ws, "const", 5) == 0) ||
            (wl == 8 && memcmp(type_name + ws, "volatile", 8) == 0)) {
            s = we;
            continue;
        }
        break;
    }
    while (s < e && (type_name[s] == ' ' || type_name[s] == '\t')) s++;
    if (e <= s) return CC_REFLECT_KIND_UNKNOWN;
    /* Primitive: every whitespace-separated word is a C scalar keyword. */
    {
        int all_prim = 1, nwords = 0;
        size_t k = s;
        while (k < e) {
            while (k < e && (type_name[k] == ' ' || type_name[k] == '\t')) k++;
            if (k >= e) break;
            size_t ws = k;
            while (k < e && type_name[k] != ' ' && type_name[k] != '\t') k++;
            nwords++;
            if (!cc__ct_word_is_primitive(type_name + ws, k - ws)) { all_prim = 0; break; }
        }
        if (all_prim && nwords > 0) return CC_REFLECT_KIND_PRIMITIVE;
    }
    /* Aggregate vs enum: locate the body by name (handles typedef / tag forms).
     * Also search registered included .cch sources so header enums/structs
     * (e.g. RedisCommandKind) resolve for typed static_map POD checks. */
    {
        char nm[256];
        size_t nl = e - s;
        size_t bo, bc;
        size_t h;
        if (nl >= sizeof(nm)) return CC_REFLECT_KIND_UNKNOWN;
        memcpy(nm, type_name + s, nl); nm[nl] = 0;
        if (cc__ct_find_enum_body(src, len, nm, nl, &bo, &bc))
            return CC_REFLECT_KIND_ENUM;
        if (cc__ct_find_struct_body(src, len, nm, nl, &bo, &bc))
            return CC_REFLECT_KIND_STRUCT;
        for (h = 0; h < g_included_cch_source_count; h++) {
            size_t fn = 0;
            int kind = CC_REFLECT_KIND_UNKNOWN;
            const char* fsrc = cc__included_cch_text(h, &fn);
            if (!fsrc) continue;
            if (cc__ct_find_enum_body(fsrc, fn, nm, nl, &bo, &bc))
                kind = CC_REFLECT_KIND_ENUM;
            else if (cc__ct_find_struct_body(fsrc, fn, nm, nl, &bo, &bc))
                kind = CC_REFLECT_KIND_STRUCT;
            if (kind != CC_REFLECT_KIND_UNKNOWN) return kind;
        }
    }
    return CC_REFLECT_KIND_UNKNOWN;
}

/* ---- tag-filtered declaration reflection (edge-push #3) -------------- */

void cc_ct_free_tagged_fns(char** names, size_t n) {
    if (!names) return;
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
}

/* From `decl_start`, extract the name of a following function definition: the
 * identifier immediately preceding the first top-level '('.  Returns 1 + fills
 * out[] if the next decl is a function; 0 if it is not (e.g. a type/struct
 * with '{' or ';' before any '(').  Bounded scan. */
static int cc__ct_following_fn_name(const char* src, size_t len, size_t decl_start,
                                    char* out, size_t out_sz) {
    size_t i = cc_skip_ws_and_comments(src, len, decl_start);
    size_t last_id_s = 0, last_id_e = 0;
    int have_id = 0;
    size_t guard = 0;
    while (i < len && guard++ < 4096) {
        char c = src[i];
        if (c == '/' && i + 1 < len && (src[i + 1] == '/' || src[i + 1] == '*')) {
            i = cc_skip_ws_and_comments(src, len, i);
            continue;
        }
        if (c == '(') {
            if (have_id && last_id_e > last_id_s && last_id_e - last_id_s < out_sz) {
                size_t nl = last_id_e - last_id_s;
                memcpy(out, src + last_id_s, nl);
                out[nl] = 0;
                return 1;
            }
            return 0;
        }
        if (c == '{' || c == ';') return 0;  /* not a plain function definition */
        if (cc_is_ident_start(c)) {
            size_t s = i;
            while (i < len && cc_is_ident_char(src[i])) i++;
            last_id_s = s; last_id_e = i; have_id = 1;
            continue;
        }
        i++;
    }
    return 0;
}

int cc_ct_reflect_tagged_fns(const char* src, size_t len, const char* tag,
                             char*** out_names, size_t* out_n) {
    if (out_names) *out_names = NULL;
    if (out_n) *out_n = 0;
    if (!src || !tag || !tag[0] || !out_names || !out_n) return 0;
    size_t cap = 8, cnt = 0;
    char** names = (char**)malloc(cap * sizeof(char*));
    if (!names) return 0;
    size_t taglen = strlen(tag);
    const char* marker = "@tag:";
    size_t i = 0;
    while (i + 5 <= len) {
        if (memcmp(src + i, marker, 5) != 0) { i++; continue; }
        size_t t = i + 5;
        size_t ts = t;
        while (t < len && cc_is_ident_char(src[t])) t++;
        /* tag name must match exactly */
        int match = (t - ts == taglen) && (memcmp(src + ts, tag, taglen) == 0);
        /* end of the single-line marker comment: a block-comment close or EOL */
        size_t d = t;
        while (d < len && src[d] != '\n') {
            if (src[d] == '*' && d + 1 < len && src[d + 1] == '/') { d += 2; break; }
            d++;
        }
        if (d < len && src[d] == '\n') d++;
        if (match) {
            char nm[128];
            if (cc__ct_following_fn_name(src, len, d, nm, sizeof(nm))) {
                if (cnt == cap) {
                    size_t ncap = cap * 2;
                    char** nn = (char**)realloc(names, ncap * sizeof(char*));
                    if (!nn) { cc_ct_free_tagged_fns(names, cnt); return 0; }
                    names = nn; cap = ncap;
                }
                names[cnt] = strdup(nm);
                if (!names[cnt]) { cc_ct_free_tagged_fns(names, cnt); return 0; }
                cnt++;
            }
        }
        i = d;
    }
    *out_names = names;
    *out_n = cnt;
    return 1;
}

/* ---- canonical generic mangling (naming/composition) ----------------- */

int cc_ct_canonical_name(const char* base, const char* const* args, int nargs,
                         char* out, size_t out_sz) {
    if (!base || !out || out_sz == 0) {
        if (out && out_sz) out[0] = 0;
        return -1;
    }
    int mo = snprintf(out, out_sz, "%s", base);
    if (mo < 0 || (size_t)mo >= out_sz) { out[out_sz - 1] = 0; return -1; }
    for (int a = 0; a < nargs; a++) {
        const char* arg = args ? args[a] : NULL;
        if (!arg || !arg[0]) continue;
        char canon[256];
        snprintf(canon, sizeof(canon), "%s", arg);
        cc__canonicalize_container_param_type(canon, sizeof(canon));
        char mang[256];
        cc__mangle_container_type_param(canon, strlen(canon), mang, sizeof(mang));
        int w = snprintf(out + mo, out_sz - (size_t)mo, "_%s", mang);
        if (w < 0 || (size_t)mo + (size_t)w >= out_sz) { out[out_sz - 1] = 0; return -1; }
        mo += w;
    }
    return mo;
}

/* Phase-2 unified engine: load struct fields via cc_reflect_field_* instead of
 * parsing the struct body from source when CC_COMPTIME_UNIFIED_EXEC=1.
 * `is_as` comes from the reflect source (must be Concurrent-C, pre-strip). */
static int cc__ct_load_fields_via_reflect(const char* tname, size_t tlen,
                                          CCCtField** out, size_t* out_n) {
    if (!tname || tlen == 0 || tlen >= 256) return 0;
    char name[256];
    memcpy(name, tname, tlen);
    name[tlen] = '\0';
    int nf = cc_reflect_field_count(name);
    if (nf <= 0) return 0;
    CCCtField* fs = (CCCtField*)calloc((size_t)nf, sizeof(CCCtField));
    if (!fs) return 0;
    for (int i = 0; i < nf; i++) {
        char nbuf[256], tbuf[256];
        int as;
        if (cc_reflect_field_name(name, i, nbuf, (int)sizeof(nbuf)) < 0 ||
            cc_reflect_field_type(name, i, tbuf, (int)sizeof(tbuf)) < 0) {
            cc__ct_free_fields(fs, (size_t)nf);
            return 0;
        }
        as = cc_reflect_field_is_as(name, i);
        if (as < 0) {
            cc__ct_free_fields(fs, (size_t)nf);
            return 0;
        }
        fs[i].name = strdup(nbuf);
        fs[i].type = strdup(tbuf);
        fs[i].is_as = as ? 1 : 0;
        if (!fs[i].name || !fs[i].type) {
            cc__ct_free_fields(fs, (size_t)nf);
            return 0;
        }
    }
    *out = fs;
    *out_n = (size_t)nf;
    return 1;
}

/* Append one unrolled copy of BODY (body[bs..be)) with the loop variable
 * `lv` (length lvlen) substituted for field `f` at index `idx`. */
/* Collect the methods of `T`: functions whose FIRST parameter is `T` or `T*`.
 * That is the tier where declaring the function installs the method, and it is
 * the only one that is unambiguous and local — the bare-name tier is universal
 * and the dynamic sink is unknowable, so neither is reflectable.
 *
 * Reuses CCCtField: `name` is the method identifier, `type` its return-type
 * spelling, so the field substituter serves both loops unchanged.
 *
 * Method reflection is as-of-this-TU by construction: a method family is open,
 * so anyone can install one by declaring a function. */
static int cc__ct_load_methods(const char* src, size_t n,
                               const char* tname, size_t tlen,
                               CCCtField** out_fs, size_t* out_n) {
    CCCtField* fs = NULL;
    size_t fn = 0, fc = 0;
    CCScannerState sc;
    size_t i = 0;
    int depth = 0;
    if (!src || !tname || tlen == 0) return 0;
    cc_scanner_init(&sc);
    while (i < n) {
        size_t rp, ns, ne, ps, pe, ds;
        if (cc_scanner_skip_non_code(&sc, src, n, &i)) continue;
        if (src[i] == '{') { depth++; i++; continue; }
        if (src[i] == '}') { if (depth > 0) depth--; i++; continue; }
        if (depth != 0 || !cc_is_ident_start(src[i]) ||
            (i > 0 && cc_is_ident_char(src[i - 1]))) { i++; continue; }
        ns = i;
        while (i < n && cc_is_ident_char(src[i])) i++;
        ne = i;
        {
            size_t q = cc_skip_ws_and_comments(src, n, i);
            if (q >= n || src[q] != '(' || !cc_find_matching_paren(src, n, q, &rp)) continue;
            /* First parameter's declared type. */
            ps = cc_skip_ws_and_comments(src, n, q + 1);
            pe = ps;
            {
                int d2 = 0;
                while (pe < rp) {
                    char c = src[pe];
                    if (c == '(' || c == '[') d2++;
                    else if (c == ')' || c == ']') d2--;
                    else if (c == ',' && d2 == 0) break;
                    pe++;
                }
            }
            /* `T self` / `T* self`: match the type head, then require a `*` or
             * whitespace before the parameter name. */
            {
                const char* pp = src + ps;
                size_t pl = (pe > ps) ? pe - ps : 0;
                cc_result_spec_skip_qualifiers(&pp, &pl);
                if (pl <= tlen || memcmp(pp, tname, tlen) != 0) { i = rp + 1; continue; }
                if (cc_is_ident_char(pp[tlen])) { i = rp + 1; continue; }
            }
            /* Return type: back to the previous top-level delimiter.
             * Drop storage / function specs (`static`, `inline`, `@async`, …)
             * but keep cv-qualifiers — factories emit forward decls from
             * `m.ret` / `cc_reflect_method_ret`, and `const char *` vs
             * `char *` is a conflicting declaration. */
            ds = cc__scan_back_to_delim(src, ns);
            {
                unsigned quals = 0;
                char ty[256];
                char body[256];
                size_t de = cc_rskip_ws_and_comments(src, ns);
                size_t nl = ne - ns, tl;
                ds = cc__skip_leading_decl_specs_ex(src, ds, &quals);
                tl = (de > ds) ? de - ds : 0;
                if (tl >= sizeof(body)) tl = sizeof(body) - 1;
                memcpy(body, src + ds, tl); body[tl] = 0;
                if (quals) {
                    char qn[64];
                    cc__qual_names(quals, qn, sizeof(qn));
                    if (snprintf(ty, sizeof(ty), "%s %s", qn, body) < 0)
                        ty[0] = 0;
                } else {
                    memcpy(ty, body, tl + 1);
                }
                if (nl > 0 && !cc__ct_push_field(&fs, &fn, &fc, src + ns, nl, ty)) {
                    cc__ct_free_fields(fs, fn);
                    return 0;
                }
                /* The parameter list verbatim, parens included, so `m.params`
                 * substitutes to a sequence the `@comptime for` head parses
                 * and to a usable signature fragment anywhere else. */
                if (nl > 0) {
                    size_t plen = rp + 1 - q;
                    char* pl = (char*)malloc(plen + 1);
                    if (!pl) { cc__ct_free_fields(fs, fn); return 0; }
                    memcpy(pl, src + q, plen); pl[plen] = 0;
                    fs[fn - 1].params = pl;
                }
                /* The name a caller writes after the dot.  Computed here, where
                 * the receiver type is in hand, by the same composition UFCS
                 * dispatches through — an exported name and a callable name
                 * must not be two answers to one question. */
                if (nl > 0) {
                    char tn[256];
                    size_t cl = tlen < sizeof(tn) - 1 ? tlen : sizeof(tn) - 1;
                    const char* mem;
                    memcpy(tn, tname, cl); tn[cl] = 0;
                    mem = cc_ufcs_member_name_of(tn, fs[fn - 1].name);
                    {
                        size_t ml = strlen(mem);
                        char* mb = (char*)malloc(ml + 1);
                        if (!mb) { cc__ct_free_fields(fs, fn); return 0; }
                        memcpy(mb, mem, ml + 1);
                        fs[fn - 1].member = mb;
                    }
                    if (!fs[fn - 1].member) { cc__ct_free_fields(fs, fn); return 0; }
                }
            }
            i = rp + 1;
        }
    }
    *out_fs = fs;
    *out_n = fn;
    return fn > 0;
}

int cc_ct_reflect_type_methods(const char* src, size_t len, const char* type_name,
                               CCCtField** out, size_t* out_n) {
    if (out) *out = NULL;
    if (out_n) *out_n = 0;
    if (!src || !type_name || !type_name[0] || !out || !out_n) return 0;
    {
        size_t nlen = strlen(type_name);
        if (cc__ct_load_methods(src, len, type_name, nlen, out, out_n)) return 1;
        /* A type's methods may be declared in an included `.cch` while the
         * factory asking about them lives in the TU.  Same search the field
         * reader does, for the same reason. */
        for (size_t h = 0; h < g_included_cch_source_count; h++) {
            size_t fn = 0;
            const char* fsrc = cc__included_cch_text(h, &fn);
            if (!fsrc) continue;
            if (cc__ct_load_methods(fsrc, fn, type_name, nlen, out, out_n)) return 1;
        }
    }
    return 0;
}

/* Split a captured return-type spelling into its ok and error halves.
 * `int !>(CCError)` -> ok "int", err "CCError"; a plain type has no err.
 * Fallibility changes the SHAPE of emitted code — unwrap or not — so it is a
 * reflected fact, unlike anything `_Generic` can decide in the emitted code. */
static void cc__ct_split_ret(const char* ty, const char** ok_s, size_t* ok_n,
                             const char** er_s, size_t* er_n) {
    const char* b = ty ? strstr(ty, "!>") : NULL;
    *ok_s = ty ? ty : ""; *ok_n = ty ? strlen(ty) : 0;
    *er_s = ""; *er_n = 0;
    if (!b) return;
    *ok_n = (size_t)(b - ty);
    while (*ok_n > 0 && ((*ok_s)[*ok_n - 1] == ' ' || (*ok_s)[*ok_n - 1] == '\t')) (*ok_n)--;
    {
        const char* lp = strchr(b, '(');
        const char* rp = lp ? strrchr(lp, ')') : NULL;
        if (lp && rp && rp > lp + 1) {
            const char* a = lp + 1;
            while (a < rp && (*a == ' ' || *a == '\t')) a++;
            *er_s = a; *er_n = (size_t)(rp - a);
            while (*er_n > 0 && ((*er_s)[*er_n - 1] == ' ' || (*er_s)[*er_n - 1] == '\t')) (*er_n)--;
        }
    }
}

static int cc__span_has_comptime_ctrl(const char* src, size_t n, size_t lo, size_t hi);

static void cc__ct_append_field_body(char** out, size_t* ol, size_t* oc,
                                     const char* body, size_t bs, size_t be,
                                     const char* lv, size_t lvlen,
                                     const CCCtField* f, size_t idx,
                                     int is_method) {
    CCScannerState s; cc_scanner_init(&s);
    size_t i = bs, emit = bs;
    while (i < be) {
        /* Walk into backtick `@emit` / `@string` bodies: loop slots like
         * `${f.index}` / `${m.name}` live there. Skipping templates as inert
         * leaves `$` for libtcc ("lvalue expected"). */
        if (cc_scanner_skip_non_code_ex(&s, body, be, &i, /*skip_templates=*/0))
            continue;
        if ((i == 0 || !cc_is_ident_char(body[i - 1])) &&
            i + lvlen <= be && memcmp(body + i, lv, lvlen) == 0 &&
            (i + lvlen == be || !cc_is_ident_char(body[i + lvlen]))) {
            cc_sb_append(out, ol, oc, body + emit, i - emit);
            size_t a = i + lvlen;
            if (a < be && body[a] == '.') {
                size_t ms = a + 1, m = a + 1;
                while (m < be && cc_is_ident_char(body[m])) m++;
                size_t mlen = m - ms;
                if (mlen == 4 && memcmp(body + ms, "name", 4) == 0) {
                    cc_sb_append_cstr(out, ol, oc, "\"");
                    cc_sb_append(out, ol, oc, f->name, strlen(f->name));
                    cc_sb_append_cstr(out, ol, oc, "\"");
                    i = m; emit = i; continue;
                }
                if (is_method && ((mlen == 3 && memcmp(body + ms, "ret", 3) == 0) ||
                                  (mlen == 3 && memcmp(body + ms, "err", 3) == 0) ||
                                  (mlen == 8 && memcmp(body + ms, "ret_void", 8) == 0) ||
                                  (mlen == 8 && memcmp(body + ms, "fallible", 8) == 0))) {
                    const char *oks, *ers; size_t okn, ern;
                    cc__ct_split_ret(f->type, &oks, &okn, &ers, &ern);
                    if (mlen == 3 && body[ms] == 'r') cc_sb_append(out, ol, oc, oks, okn);
                    else if (mlen == 3) cc_sb_append(out, ol, oc, ers, ern);
                    else if (body[ms] == 'r')
                        /* Whether a call yields a value is a shape fact like
                         * fallibility: a generated caller either binds the
                         * result or must not name it. `void` is the whole
                         * spelling, never a prefix — `void*` is a value. */
                        cc_sb_append_cstr(out, ol, oc,
                                          (okn == 4 && memcmp(oks, "void", 4) == 0) ? "1" : "0");
                    else cc_sb_append_cstr(out, ol, oc, ern > 0 ? "1" : "0");
                    i = m; emit = i; continue;
                }
                if (is_method && mlen == 6 && memcmp(body + ms, "member", 6) == 0) {
                    /* The exported/callable short name, as a string literal —
                     * the same shape `.name` has, since both are text a
                     * generator pastes into a table or an identifier. */
                    const char* mm = f->member ? f->member : f->name;
                    cc_sb_append_cstr(out, ol, oc, "\"");
                    cc_sb_append(out, ol, oc, mm, strlen(mm));
                    cc_sb_append_cstr(out, ol, oc, "\"");
                    i = m; emit = i; continue;
                }
                if (is_method && mlen == 4 && memcmp(body + ms, "args", 4) == 0) {
                    /* The same list with the types dropped: every name is a
                     * local the generated body already declared, so forwarding
                     * a call is `${m.name}${m.args}` with no comma splicing. */
                    const char* pl = f->params ? f->params : "()";
                    CCCtField* ps = NULL; size_t pn = 0;
                    if (!cc__ct_parse_param_list(pl, 0, strlen(pl) - 1, &ps, &pn)) {
                        /* No name to forward.  Emit something that cannot
                         * compile and says so, rather than a call that quietly
                         * drops or misorders an argument. */
                        cc_sb_append_cstr(out, ol, oc,
                            "(cc__reflect_method_has_an_unnamed_parameter)");
                    } else {
                        cc_sb_append_cstr(out, ol, oc, "(");
                        for (size_t k = 0; k < pn; k++) {
                            if (k) cc_sb_append_cstr(out, ol, oc, ", ");
                            cc_sb_append(out, ol, oc, ps[k].name, strlen(ps[k].name));
                        }
                        cc_sb_append_cstr(out, ol, oc, ")");
                        cc__ct_free_fields(ps, pn);
                    }
                    i = m; emit = i; continue;
                }
                if (is_method && mlen == 6 && memcmp(body + ms, "params", 6) == 0) {
                    /* The declared list, parens and all.  In a `@comptime for`
                     * head it is the sequence to walk; anywhere else it is the
                     * signature fragment the source wrote. */
                    const char* pl = f->params ? f->params : "(void)";
                    cc_sb_append(out, ol, oc, pl, strlen(pl));
                    i = m; emit = i; continue;
                }
                if (mlen == 4 && memcmp(body + ms, "type", 4) == 0) {
                    cc_sb_append(out, ol, oc, f->type, strlen(f->type));
                    i = m; emit = i; continue;
                }
                if (mlen == 7 && memcmp(body + ms, "typestr", 7) == 0) {
                    /* B1: the field's type spelling as a string literal, for
                     * feeding cc_instantiate / cc_emit_format operands. */
                    cc_sb_append_cstr(out, ol, oc, "\"");
                    cc_sb_append(out, ol, oc, f->type, strlen(f->type));
                    cc_sb_append_cstr(out, ol, oc, "\"");
                    i = m; emit = i; continue;
                }
                if (mlen == 5 && memcmp(body + ms, "is_as", 5) == 0) {
                    cc_sb_append_cstr(out, ol, oc, f->is_as ? "1" : "0");
                    i = m; emit = i; continue;
                }
                if (mlen == 5 && memcmp(body + ms, "index", 5) == 0) {
                    char num[32];
                    int nl = snprintf(num, sizeof(num), "%zu", idx);
                    if (nl > 0) cc_sb_append(out, ol, oc, num, (size_t)nl);
                    i = m; emit = i; continue;
                }
                /* unknown `.member` — fall through to bare-identifier subst */
            }
            cc_sb_append(out, ol, oc, f->name, strlen(f->name));
            i += lvlen; emit = i; continue;
        }
        i++;
    }
    if (emit < be) cc_sb_append(out, ol, oc, body + emit, be - emit);
}

/* Try to expand a `@comptime for` whose `@comptime` keyword is at *io_i.
 * Returns 1 = expanded (out appended, *io_i and *io_last_emit advanced past the
 * construct), 0 = not a `@comptime for` here (caller handles `@comptime if`),
 * -1 = hard error. */
static int cc__try_expand_comptime_for(const char* src, size_t n, const char* input_path,
                                       char** out, size_t* out_len, size_t* out_cap,
                                       size_t* io_i, size_t* io_last_emit) {
    size_t i = *io_i;
    const size_t ATCN = sizeof("@comptime") - 1;
    size_t p = cc_skip_ws_and_comments(src, n, i + ATCN);
    if (!(p + 3 <= n && memcmp(src + p, "for", 3) == 0 &&
          (p + 3 >= n || !cc_is_ident_char(src[p + 3])))) return 0;

    /* Native shadow's whitelist cannot lower `@comptime for` that lives in a
     * harvested/spliced `.cch`. Expanding it here looks like success while the
     * subset still cannot represent the form — refuse loudly. */
    {
        const char* bm = CC_IMPL_CCH_BEGIN_MARK;
        const char* em = CC_IMPL_CCH_END_MARK;
        size_t bl = strlen(bm), el = strlen(em);
        size_t last_begin = (size_t)-1, k;
        for (k = 0; k + bl <= n && k <= i; k++) {
            if (memcmp(src + k, bm, bl) == 0) last_begin = k;
            else if (last_begin != (size_t)-1 && k + el <= n && k < i &&
                     memcmp(src + k, em, el) == 0)
                last_begin = (size_t)-1;
        }
        if (last_begin != (size_t)-1) {
            fprintf(stderr, "%s: error: unexpected token\n",
                    input_path ? input_path : "<input>");
            return -1;
        }
    }

    size_t lp = cc_skip_ws_and_comments(src, n, p + 3);
    if (lp >= n || src[lp] != '(') return 0;       /* not our shape; leave alone */
    size_t hp_close;
    if (!cc_find_matching_paren(src, n, lp, &hp_close)) return 0;

    /* header: NAME in type_of(T).fields (canonical spelling only; Fix A
     * normalizes cc_type_of("T") -> type_of(T) before we run). */
    size_t h = cc_skip_ws_and_comments(src, n, lp + 1);
    size_t lv_s = h;
    while (h < hp_close && cc_is_ident_char(src[h])) h++;
    size_t lvlen = h - lv_s;
    h = cc_skip_ws_and_comments(src, n, h);
    int ok = lvlen > 0 &&
             h + 2 <= hp_close && src[h] == 'i' && src[h + 1] == 'n' &&
             (h + 2 >= hp_close || !cc_is_ident_char(src[h + 2]));
    size_t ts = 0, te = 0;
    int want_methods = 0;
    int want_params = 0;
    size_t plist_lp = 0, plist_rp = 0;
    if (ok) {
        h = cc_skip_ws_and_comments(src, n, h + 2);
        /* A parenthesized declaration list is a sequence in its own right, and
         * it is what `m.params` substitutes to — reflection carries text, so
         * the loop walks the list the source wrote rather than a handle to it.
         * Written by hand it means exactly the same thing. */
        if (h < hp_close && src[h] == '(') {
            size_t pc;
            if (cc_find_matching_paren(src, n, h, &pc) && pc <= hp_close &&
                cc_skip_ws_and_comments(src, n, pc + 1) >= hp_close) {
                want_params = 1; plist_lp = h; plist_rp = pc;
            } else ok = 0;
        } else {
        const size_t TOFN = sizeof("type_of") - 1;
        if (h + TOFN <= hp_close && memcmp(src + h, "type_of", TOFN) == 0 &&
            (h + TOFN >= hp_close || !cc_is_ident_char(src[h + TOFN]))) {
            h += TOFN;
        } else ok = 0;
        if (ok) {
            h = cc_skip_ws_and_comments(src, n, h);
            size_t tpc;
            if (h < hp_close && src[h] == '(' &&
                cc_find_matching_paren(src, n, h, &tpc) && tpc <= hp_close) {
                /* `src[ts..te)` is copied verbatim as the struct name to look
                 * up, so the trailing edge must land on code too. */
                ts = cc_skip_ws_and_comments(src, n, h + 1);
                te = cc_rskip_ws_and_comments(src, tpc);
                if (te < ts) te = ts;
                size_t af = cc_skip_ws_and_comments(src, n, tpc + 1);
                if (af < hp_close && src[af] == '.') {
                    af = cc_skip_ws_and_comments(src, n, af + 1);
                    if (af + 6 <= hp_close && memcmp(src + af, "fields", 6) == 0) {
                        ok = 1;
                    } else if (af + 7 <= hp_close && memcmp(src + af, "methods", 7) == 0) {
                        ok = 1; want_methods = 1;
                    } else ok = 0;
                } else ok = 0;
            } else ok = 0;
        }
        }
    }
    if (!ok) {
        fprintf(stderr,
                "%s: error: malformed `@comptime for` — expected "
                "`@comptime for (NAME in type_of(T).fields|.methods) { ... }` "
                "or a parenthesized declaration list "
                "(`@comptime for (P in m.params) { ... }`).\n",
                input_path ? input_path : "<input>");
        return -1;
    }

    size_t bb = cc_skip_ws_and_comments(src, n, hp_close + 1);
    if (bb >= n || src[bb] != '{') return 0;
    size_t bb_close;
    if (!cc_find_matching_brace(src, n, bb, &bb_close)) return 0;

    CCCtField* fields = NULL; size_t nf = 0;
    size_t body_o, body_c;
    {
        char tname_buf[256];
        size_t tname_len = (te > ts) ? (te - ts) : 0;
        if (tname_len >= sizeof(tname_buf)) tname_len = sizeof(tname_buf) - 1;
        if (tname_len > 0) {
            memcpy(tname_buf, src + ts, tname_len);
            tname_buf[tname_len] = '\0';
        } else
            tname_buf[0] = '\0';
        if (want_params) {
            if (!cc__ct_parse_param_list(src, plist_lp, plist_rp, &fields, &nf)) {
                fprintf(stderr,
                        "%s: error: `@comptime for` over a declaration list needs "
                        "every entry named and spelled in a form reflection models "
                        "(`%.*s`).\n",
                        input_path ? input_path : "<input>",
                        (int)(plist_rp + 1 - plist_lp), src + plist_lp);
                return -1;
            }
        } else if (want_methods) {
            /* TU text, then registered included `.cch` (no CPP expand view). */
            if (!cc_ct_reflect_type_methods(src, n, tname_buf, &fields, &nf)) {
                fprintf(stderr,
                        "%s: error: `@comptime for ... .methods` found no method of "
                        "`%.*s` — a method is a function whose first parameter is "
                        "`T` or `T*`, declared where this loop can see it.\n",
                        input_path ? input_path : "<input>", (int)(te - ts), src + ts);
                return -1;
            }
        } else if (cc__ct_find_struct_body(src, n, src + ts, te - ts, &body_o,
                                           &body_c)) {
            /* Body in Concurrent-C: parse must succeed or refuse — do not
             * fall through to a partial type-pass registry table. */
            if (!cc__ct_parse_fields_from_body(src, body_o, body_c, &fields,
                                               &nf)) {
                cc__ct_free_fields(fields, nf);
                fprintf(stderr,
                        "%s: error: `@comptime for` needs an in-scope struct type "
                        "with simple fields; `%.*s` is unknown or has unsupported "
                        "field forms (arrays/bitfields/function-pointers/"
                        "multiple-declarators/nested aggregates).\n",
                        input_path ? input_path : "<input>", (int)(te - ts),
                        src + ts);
                return -1;
            }
            cc__ct_apply_typeview_as(src, n, tname_buf, fields, nf);
        } else if (cc__ct_load_fields_via_reflect(src + ts, te - ts, &fields,
                                                  &nf)) {
            /* Header-only / harvested types: host reflect walks included
             * `.cch` (+ registry for is_as). No full-TU CPP expand. */
            cc__ct_apply_typeview_as(src, n, tname_buf, fields, nf);
        } else {
            cc__ct_free_fields(fields, nf);
            fprintf(stderr,
                    "%s: error: `@comptime for` needs an in-scope struct type with "
                    "simple fields; `%.*s` is unknown or has unsupported field forms "
                    "(arrays/bitfields/function-pointers/multiple-declarators/nested "
                    "aggregates).\n",
                    input_path ? input_path : "<input>", (int)(te - ts), src + ts);
            return -1;
        }
    }

    cc_sb_append(out, out_len, out_cap, src + *io_last_emit, i - *io_last_emit);
    {
        /* Wrap only when the for body is emit-ready and has no further
         * `@comptime if`/`for` outside templates. Otherwise the wrap
         * imprisons unexpanded control flow in a libtcc `@comptime { }`. */
        int wrap_exec = cc_template_body_needs_emit_exec(src, n, bb + 1, bb_close) &&
                        !cc__span_has_comptime_ctrl(src, n, bb + 1, bb_close);
        for (size_t fi = 0; fi < nf; fi++) {
            if (wrap_exec) cc_sb_append_cstr(out, out_len, out_cap, "@comptime { ");
            cc__ct_append_field_body(out, out_len, out_cap, src, bb + 1, bb_close,
                                     src + lv_s, lvlen, &fields[fi], fi, want_methods);
            if (wrap_exec) cc_sb_append_cstr(out, out_len, out_cap, " } ");
        }
    }
    cc__ct_free_fields(fields, nf);
    *io_last_emit = bb_close + 1;
    *io_i = bb_close + 1;
    return 1;
}

/* True when [lo, hi) has a `@comptime if` / `@comptime for` outside templates.
 * Used so if-prune wrap-for-exec does not imprison an unexpanded for/if inside
 * `@comptime { }` (libtcc would then see `@` — silent wrong path). For-inside
 * emit backticks are ignored (skip_templates). */
static int cc__span_has_comptime_ctrl(const char* src, size_t n, size_t lo, size_t hi) {
    static const char ATC[] = "@comptime";
    const size_t ATCN = sizeof(ATC) - 1;
    CCScannerState scan;
    size_t i;
    if (!src || lo >= hi || hi > n) return 0;
    cc_scanner_init(&scan);
    i = lo;
    while (i < hi) {
        if (cc_scanner_skip_non_code(&scan, src, hi, &i)) continue;
        if (src[i] == '@' && i + ATCN <= hi && memcmp(src + i, ATC, ATCN) == 0 &&
            (i + ATCN == hi || !cc_is_ident_char(src[i + ATCN]))) {
            size_t p = cc_skip_ws_and_comments(src, hi, i + ATCN);
            if (p + 3 <= hi && memcmp(src + p, "for", 3) == 0 &&
                (p + 3 == hi || !cc_is_ident_char(src[p + 3])))
                return 1;
            if (p + 2 <= hi && src[p] == 'i' && src[p + 1] == 'f' &&
                (p + 2 == hi || !cc_is_ident_char(src[p + 2])))
                return 1;
        }
        i++;
    }
    return 0;
}

/* One sweep: resolve each *outermost* `@comptime if`/`@comptime for`
 * left-to-right.  Resolving strictly outermost-first (each construct's text is
 * spliced/expanded into `out` and the scan jumps past it) makes the fixpoint
 * loop handle arbitrary nesting: an `@comptime if` inside a `@comptime for`
 * body sees the loop variable already substituted; a dead `@comptime if` branch
 * is pruned before any `@comptime for` it contains is ever expanded. */
static char* cc__resolve_comptime_if_once(const char* src, size_t n, const char* input_path) {
    char* out = NULL;
    size_t out_len = 0, out_cap = 0, last_emit = 0;
    int changed = 0;
    CCScannerState scan;
    static const char ATC[] = "@comptime";
    const size_t ATCN = sizeof(ATC) - 1;
    size_t i = 0;
    if (!cc_contains_token_top_level(src, n, "@comptime")) return NULL;
    /* Host reflect verbs parse Concurrent-C (naked `@as`) + included `.cch`.
     * Header-only `@comptime for` does not need a full-TU CPP expand view. */
    cc_emit_plan_set_reflect_source(src, n);
    /* D3.1b: in-scope type definitions for the TCC layout fallback, built lazily
     * (and once) the first time a predicate needs the host evaluator. */
    char* type_prelude = NULL;
    int type_prelude_built = 0;
    cc_scanner_init(&scan);
    while (i < n) {
        /* Walk into `@emit` / `@string` backticks: after `@comptime for`
         * substitutes `m.fallible` etc. inside a template, the nested
         * `@comptime if` must still be visible to this sweep. Skipping
         * templates here leaves `@comptime if (0) { ... }` for the host. */
        if (cc_scanner_skip_non_code_ex(&scan, src, n, &i, /*skip_templates=*/0))
            continue;
        if (src[i] != '@' || i + ATCN > n || memcmp(src + i, ATC, ATCN) != 0) { i++; continue; }
        /* D4.0: `@comptime for (F in type_of(T).fields) { ... }` — expand here
         * (outermost-first) before the `@comptime if` handling below. */
        {
            int fr = cc__try_expand_comptime_for(src, n, input_path,
                                                 &out, &out_len, &out_cap, &i, &last_emit);
            if (fr == -1) { free(type_prelude); free(out); return (char*)-1; }
            if (fr == 1) { changed = 1; continue; }
        }
        size_t p = cc_skip_ws_and_comments(src, n, i + ATCN);
        /* Distinguish `@comptime if` from a plain `@comptime { }` block. */
        if (!(p + 2 <= n && src[p] == 'i' && src[p + 1] == 'f' &&
              (p + 2 >= n || !cc_is_ident_char(src[p + 2])))) { i++; continue; }
        size_t lp = cc_skip_ws_and_comments(src, n, p + 2);
        if (lp >= n || src[lp] != '(') { i++; continue; }
        size_t pred_close;
        if (!cc_find_matching_paren(src, n, lp, &pred_close)) { i++; continue; }
        long val = 0;
        if (!type_prelude_built) {
            type_prelude = cc_ct_extract_type_decls_prelude(src, n);
            type_prelude_built = 1;
        }
        if (!cc__comptime_eval_pred_unified(src, n,
                                            src + lp + 1, pred_close - (lp + 1),
                                            type_prelude, &val)) {
            fprintf(stderr,
                    "%s: error: `@comptime if` condition is not a compile-time "
                    "constant the compiler can decide.\n"
                    "  it must fold to an integer via structural type facts "
                    "(type_of(T).kind/.nfields, CC_TK_*) or a host-C constant "
                    "expression the backend can evaluate (sizeof/_Alignof/"
                    "__builtin_offsetof over in-scope types, integer arithmetic).\n"
                    "  it cannot reference runtime values or an unclassified "
                    "user type's kind/layout.\n",
                    input_path ? input_path : "<input>");
            free(type_prelude);
            free(out);
            return (char*)-1;
        }
        size_t tb = cc_skip_ws_and_comments(src, n, pred_close + 1);
        if (tb >= n || src[tb] != '{') {
            /* Must not silently skip — that looks like "no work" while the
             * programmer wrote a `@comptime if`. */
            fprintf(stderr,
                    "%s: error: `@comptime if` body must be `{ ... }` "
                    "(got bare statement or end of input).\n",
                    input_path ? input_path : "<input>");
            free(type_prelude);
            free(out);
            return (char*)-1;
        }
        size_t tb_close;
        if (!cc_find_matching_brace(src, n, tb, &tb_close)) {
            fprintf(stderr,
                    "%s: error: unterminated `{` in `@comptime if` body.\n",
                    input_path ? input_path : "<input>");
            free(type_prelude);
            free(out);
            return (char*)-1;
        }

        /* Else arm: none, `else { ... }`, or `else @comptime if ...` (chain). */
        enum { ELSE_NONE, ELSE_BLOCK, ELSE_CHAIN } else_kind = ELSE_NONE;
        size_t eb = 0, eb_close = 0;   /* ELSE_BLOCK body braces                */
        size_t chain_start = 0;        /* ELSE_CHAIN: nested `@comptime if` start */
        size_t construct_end;          /* exclusive end of the whole construct  */
        size_t after = cc_skip_ws_and_comments(src, n, tb_close + 1);
        if (after + 4 <= n && memcmp(src + after, "else", 4) == 0 &&
            (after + 4 >= n || !cc_is_ident_char(src[after + 4]))) {
            size_t ep = cc_skip_ws_and_comments(src, n, after + 4);
            if (ep < n && src[ep] == '{' && cc_find_matching_brace(src, n, ep, &eb_close)) {
                else_kind = ELSE_BLOCK; eb = ep; construct_end = eb_close + 1;
            } else if (cc__ct_if_extent(src, n, ep, &construct_end)) {
                else_kind = ELSE_CHAIN; chain_start = ep;
            } else {
                fprintf(stderr,
                        "%s: error: `@comptime if` else arm must be `else { ... }` "
                        "or `else @comptime if (...) { ... }`.\n",
                        input_path ? input_path : "<input>");
                free(type_prelude);
                free(out);
                return (char*)-1;
            }
        } else {
            construct_end = tb_close + 1;
        }

        /* Region spliced verbatim: the taken branch body, the else block body,
         * or (for a chain whose head is false) the nested `@comptime if` text —
         * which the fixpoint loop then resolves. */
        size_t keep_l = 0, keep_r = 0;
        if (val) {
            keep_l = tb + 1; keep_r = tb_close;
        } else if (else_kind == ELSE_BLOCK) {
            keep_l = eb + 1; keep_r = eb_close;
        } else if (else_kind == ELSE_CHAIN) {
            keep_l = chain_start; keep_r = construct_end;
        }

        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
        if (keep_r > keep_l) {
            /* A kept arm that contains `@emit(\`...\`)` must still run under
             * libtcc after the surrounding `@comptime if` vanishes. Without
             * this wrap the bare `@emit` is lowered but never executed —
             * silent empty splice. Do not wrap when the arm still has an
             * outer `@comptime for`/`if` — for-expand (or a later if prune)
             * will wrap; wrapping now feeds unexpanded `@` to libtcc.
             * Else-chains stay unwrapped so the fixpoint can resolve them. */
            int keep_is_chain = (!val && else_kind == ELSE_CHAIN);
            int wrap_exec = !keep_is_chain &&
                            !cc__span_has_comptime_ctrl(src, n, keep_l, keep_r) &&
                            cc_template_body_needs_emit_exec(src, n, keep_l, keep_r);
            cc__sb_emit_newlines(&out, &out_len, &out_cap, src, i, keep_l);
            if (wrap_exec) cc_sb_append_cstr(&out, &out_len, &out_cap, "@comptime { ");
            cc_sb_append(&out, &out_len, &out_cap, src + keep_l, keep_r - keep_l);
            if (wrap_exec) cc_sb_append_cstr(&out, &out_len, &out_cap, " } ");
            cc__sb_emit_newlines(&out, &out_len, &out_cap, src, keep_r, construct_end);
        } else {
            cc__sb_emit_newlines(&out, &out_len, &out_cap, src, i, construct_end);
        }
        last_emit = construct_end;
        i = construct_end;
        changed = 1;
    }
    if (!changed) { free(type_prelude); free(out); return NULL; }
    if (last_emit < n)
        cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    if (!out) out = strdup(""); /* whole construct(s) elided to empty */
    free(type_prelude);
    return out;
}

/* After if/for fixpoint, an arm that kept both `@emit` and a nested for may
 * leave a bare `@emit` at depth 0 once the for is peeled away. Wrap those
 * so libtcc still executes them (same as for-expand / if-prune wrap). */
static char* cc__wrap_orphan_anchored_emits(const char* src, size_t n) {
    typedef struct { size_t lo, hi; } Span;
    Span* covered = NULL;
    size_t covered_n = 0, covered_cap = 0;
    Span* orphans = NULL;
    size_t orphans_n = 0, orphans_cap = 0;
    CCScannerState scan;
    size_t i = 0;
    static const char ATC[] = "@comptime";
    const size_t ATCN = sizeof(ATC) - 1;
    if (!src || !n) return NULL;
    cc_scanner_init(&scan);
    while (i < n) {
        size_t sig_after = 0;
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (src[i] == '@' && i + ATCN <= n && memcmp(src + i, ATC, ATCN) == 0 &&
            (i + ATCN == n || !cc_is_ident_char(src[i + ATCN]))) {
            size_t p = cc_skip_ws_and_comments(src, n, i + ATCN);
            if (p < n && src[p] == '{') {
                size_t close = 0;
                if (cc_find_matching_brace(src, n, p, &close)) {
                    if (covered_n == covered_cap) {
                        size_t nc = covered_cap ? covered_cap * 2 : 8;
                        Span* nb = (Span*)realloc(covered, nc * sizeof(Span));
                        if (!nb) { free(covered); free(orphans); return (char*)-1; }
                        covered = nb; covered_cap = nc;
                    }
                    covered[covered_n].lo = i;
                    covered[covered_n].hi = close + 1;
                    covered_n++;
                    i = close + 1;
                    continue;
                }
            }
        }
        if (src[i] == '@' && cc__sigil_delim_after(src, n, i, "@emit", '(', &sig_after)) {
            size_t lp = sig_after - 1; /* '(' */
            size_t rp = 0;
            size_t arg1 = cc_skip_ws_and_comments(src, n, sig_after);
            /* Return-form `@emit(\`...\`, arena)` is an expression — do not
             * wrap. Only anchored splice `@emit(ANCHOR, \`...\`)` is a stmt. */
            if (arg1 < n && src[arg1] == '`') { i++; continue; }
            if (cc_find_matching_paren(src, n, lp, &rp)) {
                int inside = 0;
                size_t k;
                for (k = 0; k < covered_n; k++) {
                    if (i >= covered[k].lo && rp < covered[k].hi) { inside = 1; break; }
                }
                if (!inside) {
                    if (orphans_n == orphans_cap) {
                        size_t nc = orphans_cap ? orphans_cap * 2 : 8;
                        Span* nb = (Span*)realloc(orphans, nc * sizeof(Span));
                        if (!nb) { free(covered); free(orphans); return (char*)-1; }
                        orphans = nb; orphans_cap = nc;
                    }
                    orphans[orphans_n].lo = i;
                    orphans[orphans_n].hi = rp + 1;
                    orphans_n++;
                }
                i = rp + 1;
                continue;
            }
        }
        i++;
    }
    free(covered);
    if (!orphans_n) { free(orphans); return NULL; }
    {
        char* out = NULL;
        size_t ol = 0, oc = 0, last = 0, oi;
        for (oi = 0; oi < orphans_n; oi++) {
            cc_sb_append(&out, &ol, &oc, src + last, orphans[oi].lo - last);
            cc_sb_append_cstr(&out, &ol, &oc, "@comptime { ");
            cc_sb_append(&out, &ol, &oc, src + orphans[oi].lo,
                         orphans[oi].hi - orphans[oi].lo);
            cc_sb_append_cstr(&out, &ol, &oc, "; } ");
            last = orphans[oi].hi;
            /* Skip a trailing ';' already present — we emitted one. */
            size_t s = cc_skip_ws_and_comments(src, n, last);
            if (s < n && src[s] == ';') last = s + 1;
        }
        if (last < n) cc_sb_append(&out, &ol, &oc, src + last, n - last);
        free(orphans);
        return out ? out : strdup("");
    }
}

/* Re-run the sweep to a fixpoint so nested `@comptime if` inside a kept branch
 * (only visible after the outer splice) is also resolved. */
char* cc__resolve_comptime_if(const char* src, size_t n, const char* input_path) {
    /* Fix A: normalize cc_type_of("T") -> type_of(T) once before the fixpoint. */
    char* normalized = cc__normalize_cc_type_of_to_type_of(src, n);
    if (normalized) { src = normalized; n = strlen(normalized); }
    char* cur = NULL;
    const char* in = src;
    size_t inn = n;
    for (int iter = 0; iter < 64; iter++) {
        char* r = cc__resolve_comptime_if_once(in, inn, input_path);
        if (r == (char*)-1) { free(cur); free(normalized); return (char*)-1; }
        if (!r) break;
        free(cur);
        cur = r;
        in = cur;
        inn = strlen(cur);
    }
    {
        char* wrapped = cc__wrap_orphan_anchored_emits(in, inn);
        if (wrapped == (char*)-1) { free(cur); free(normalized); return (char*)-1; }
        if (wrapped) {
            free(cur);
            cur = wrapped;
        }
    }
    free(normalized);
    return cur;
}

/* Resolve the source origin (file + 1-based line) of byte offset `at`, honoring
 * any `#line N "file"` / `# N "file"` directives that precede it.  Without
 * directives this is a plain newline count (file_out left empty). */
static void cc__comptime_value_origin(const char* src, size_t at,
                                      char* file_out, size_t file_cap, int* line_out) {
    int line = 1;
    if (file_out && file_cap) file_out[0] = '\0';
    if (!src) { if (line_out) *line_out = 1; return; }
    size_t ls = 0;
    for (;;) {
        size_t le = ls;
        while (src[le] && src[le] != '\n') le++;
        if (at <= le) break;
        size_t p = ls;
        while (p < le && (src[p] == ' ' || src[p] == '\t')) p++;
        if (p < le && src[p] == '#') {
            size_t q = p + 1;
            while (q < le && (src[q] == ' ' || src[q] == '\t')) q++;
            if (q + 4 <= le && memcmp(src + q, "line", 4) == 0) {
                q += 4;
                while (q < le && (src[q] == ' ' || src[q] == '\t')) q++;
            }
            if (q < le && src[q] >= '0' && src[q] <= '9') {
                long nn = 0;
                while (q < le && src[q] >= '0' && src[q] <= '9') nn = nn * 10 + (src[q++] - '0');
                line = (int)nn;
                while (q < le && (src[q] == ' ' || src[q] == '\t')) q++;
                if (q < le && src[q] == '"' && file_out && file_cap) {
                    q++;
                    size_t fl = 0;
                    while (q < le && src[q] != '"') {
                        if (fl + 1 < file_cap) file_out[fl++] = src[q];
                        q++;
                    }
                    file_out[fl] = '\0';
                }
                if (!src[le]) break;
                ls = le + 1;
                continue;
            }
        }
        line++;
        if (!src[le]) break;
        ls = le + 1;
    }
    if (line_out) *line_out = line;
}

typedef struct CCValueHoistEntry {
    struct CCValueHoistEntry* next;
    uint64_t                hash;
    char*                   expr;
    char*                   lit;
    size_t                  litlen;
} CCValueHoistEntry;

static CCValueHoistEntry* cc__value_hoist_cache;

static uint64_t cc__value_hoist_hash(const char* s) {
    uint64_t h = 14695981039346656037ULL;
    if (!s) return h;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

static void cc__value_hoist_cache_free(void) {
    CCValueHoistEntry* e = cc__value_hoist_cache;
    while (e) {
        CCValueHoistEntry* n = e->next;
        free(e->expr);
        free(e);
        e = n;
    }
    cc__value_hoist_cache = NULL;
}

static CCValueHoistEntry* cc__value_hoist_lookup(const char* expr) {
    uint64_t h = cc__value_hoist_hash(expr);
    for (CCValueHoistEntry* e = cc__value_hoist_cache; e; e = e->next)
        if (e->hash == h && strcmp(e->expr, expr) == 0) return e;
    return NULL;
}

/* Evaluate expr or return a cached literal for this TU pass.  *out_lit points
 * into `arena`; owned until cc_arena_free(arena).  Returns rc from eval_literal. */
static int cc__value_hoist_eval(const char* expr,
                                char** out_lit, size_t* out_len,
                                char* err, size_t err_sz,
                                CCArena* arena) {
    CCValueHoistEntry* hit = cc__value_hoist_lookup(expr);
    if (hit) {
        *out_lit = hit->lit;
        *out_len = hit->litlen;
        return 0;
    }
    char* lit = NULL;
    size_t litlen = 0;
    int rc = cc_comptime_exec_eval_literal(expr, NULL, &lit, &litlen, err, err_sz, arena);
    if (rc != 0) return rc;
    CCValueHoistEntry* ent = (CCValueHoistEntry*)malloc(sizeof(*ent));
    if (!ent) {
        if (err && err_sz) snprintf(err, err_sz, "OOM caching comptime value");
        return -1;
    }
    ent->expr = strdup(expr);
    if (!ent->expr) {
        free(ent);
        if (err && err_sz) snprintf(err, err_sz, "OOM caching comptime value expr");
        return -1;
    }
    ent->hash = cc__value_hoist_hash(expr);
    ent->lit = lit;
    ent->litlen = litlen;
    ent->next = cc__value_hoist_cache;
    cc__value_hoist_cache = ent;
    *out_lit = lit;
    *out_len = litlen;
    return 0;
}

/* Value-position `@comptime(expr)`: evaluate `expr` at compile time and splice
 * its value as a C constant-expression literal in place.  Distinguished from
 * `@comptime { ... }` (block), `@comptime if/for` (control flow) and
 * `@comptime T name(...)` (fn definition) by the token immediately following
 * `@comptime` being `(`.  Consumed newlines are re-padded so downstream line
 * numbers stay stable.  Returns a malloc'd string on change, NULL if no value
 * form is present, or (char*)-1 on a hard error (after printing a diagnostic). */
char* cc__resolve_comptime_value(const char* src, size_t n, const char* input_path) {
    static const char KW[] = "@comptime";
    const size_t KWN = sizeof(KW) - 1;
    if (!src || n == 0) return NULL;
    if (!cc_contains_token_top_level(src, n, "@comptime")) return NULL;
    cc__value_hoist_cache_free();
    cc_arena_stack(hoist_arena, CC_EMIT_TPL_BUF_SIZE);
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 0, last = 0;
    int changed = 0, scanned = 0;
    CCScannerState scan;
    cc_scanner_init(&scan);
    while (i < n) {
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (i + KWN > n || memcmp(src + i, KW, KWN) != 0 ||
            (i > 0 && cc_is_ident_char(src[i - 1])) ||
            (i + KWN < n && cc_is_ident_char(src[i + KWN]))) {
            i++;
            continue;
        }
        size_t p = cc_skip_ws_and_comments(src, n, i + KWN);
        if (p >= n || src[p] != '(') { i++; continue; }  /* block / if / for / fn def */
        size_t close;
        if (!cc_find_matching_paren(src, n, p, &close)) { i++; continue; }
        size_t es = p + 1, ee = close;
        while (es < ee && (src[es] == ' ' || src[es] == '\t' || src[es] == '\n' || src[es] == '\r')) es++;
        while (ee > es && (src[ee - 1] == ' ' || src[ee - 1] == '\t' || src[ee - 1] == '\n' || src[ee - 1] == '\r')) ee--;
        if (es == ee) { i++; continue; }  /* empty @comptime() — not a value hoist */

        char* expr = (char*)malloc(ee - es + 1);
        if (!expr) {
            free(out);
            cc__value_hoist_cache_free();
            cc_arena_free(&hoist_arena);
            return (char*)-1;
        }
        memcpy(expr, src + es, ee - es);
        expr[ee - es] = '\0';

        if (!scanned) { cc_comptime_fn_registry_scan(src, n); scanned = 1; }

        char* lit = NULL;
        size_t litlen = 0;
        char err[512];
        err[0] = '\0';
        int rc = cc__value_hoist_eval(expr, &lit, &litlen, err, sizeof(err), &hoist_arena);
        if (rc != 0) {
            char rel[1024], ofile[1024];
            int oline = 1;
            cc__comptime_value_origin(src, i, ofile, sizeof(ofile), &oline);
            const char* relp = ofile[0]
                ? cc_path_rel_to_repo(ofile, rel, sizeof(rel))
                : cc_path_rel_to_repo(input_path ? input_path : "<input>", rel, sizeof(rel));
            if (rc == -2) {
                fprintf(stderr,
                        "%s:%d: error: @comptime(%s) value is not projectable to a C literal\n"
                        "  note: value-position @comptime supports integers, floating point, bool, and strings\n",
                        relp, oline, expr);
            } else {
                fprintf(stderr,
                        "%s:%d: error: @comptime(%s) could not be evaluated at compile time%s%s\n",
                        relp, oline, expr, err[0] ? ": " : "", err);
            }
            free(expr);
            free(out);
            cc__value_hoist_cache_free();
            cc_arena_free(&hoist_arena);
            return (char*)-1;
        }
        free(expr);

        size_t nl = 0;
        for (size_t k = i; k <= close && k < n; k++)
            if (src[k] == '\n') nl++;

        cc_sb_append(&out, &out_len, &out_cap, src + last, i - last);
        cc_sb_append(&out, &out_len, &out_cap, lit, litlen);
        while (nl--) cc_sb_append(&out, &out_len, &out_cap, "\n", 1);
        i = close + 1;
        last = i;
        changed = 1;
    }
    if (!changed) {
        cc__value_hoist_cache_free();
        cc_arena_free(&hoist_arena);
        free(out);
        return NULL;
    }
    cc_sb_append(&out, &out_len, &out_cap, src + last, n - last);
    if (!out) {  /* whole input collapsed to empty (shouldn't happen) */
        out = (char*)malloc(1);
        if (out) out[0] = '\0';
    }
    cc__value_hoist_cache_free();
    cc_arena_free(&hoist_arena);
    return out;
}

static char* cc__rewrite_channel_pair_pass(const char* src,
                                           size_t len,
                                           const char* input_path) {
    if (!src || len == 0) return NULL;
    CCVisitorCtx ctx = {.input_path = input_path};
    size_t out_len = 0;
    return cc__rewrite_channel_pair_calls_text(&ctx, src, len, &out_len);
}

static char* cc__rewrite_result_field_sugar_pass(const char* src, size_t len) {
    return cc__rewrite_result_field_sugar_text(NULL, src, len);
}

/* `@string(...)` receivers: capture the template and its first member
 * call into a typed temp before template lowering erases the callable
 * shape — `@string(T).m(A)` becomes
 * `({ TY __cc_tplrecv_N = @string(T); __cc_tplrecv_N.m(A); })`.
 * Arena form (`@string(`...`, &a)` / policy) yields CCString; the
 * arena-less bounded stack form (`@string(`...`)`) yields char[:] —
 * the temp type must match, or the hoist mis-types the chain. The
 * ident receiver then resolves on the normal rails in every position,
 * handler bodies included. */
static char* cc__normalize_template_recv_chains(const char* src, size_t n) {
    static _Thread_local int g_tplrecv_id = 0;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0, last_emit = 0, i = 0;
    CCScannerState scan;
    if (!src || n == 0) return NULL;
    cc_scanner_init(&scan);
    while (i < n) {
        size_t pe = 0, ape = 0, q, ms, me, open;
        if (cc_scanner_skip_non_code(&scan, src, n, &i)) continue;
        if (src[i] != '@' || !cc_match_ident_kw(src, n, i + 1, "string")) {
            i++;
            continue;
        }
        q = cc_skip_ws_and_comments(src, n, i + 1 + (sizeof("string") - 1));
        if (q >= n || src[q] != '(' || !cc_find_matching_paren(src, n, q, &pe)) {
            i++;
            continue;
        }
        open = q;
        q = cc_skip_ws_and_comments(src, n, pe + 1);
        if (q >= n || src[q] != '.') { i = pe + 1; continue; }
        ms = cc_skip_ws_and_comments(src, n, q + 1);
        me = ms;
        while (me < n && cc_is_ident_char(src[me])) me++;
        if (me == ms) { i = pe + 1; continue; }
        q = cc_skip_ws_and_comments(src, n, me);
        if (q >= n || src[q] != '(' || !cc_find_matching_paren(src, n, q, &ape)) {
            i = pe + 1;
            continue;
        }
        {
            int id = ++g_tplrecv_id;
            char frag[80];
            /* Match the template rewrite's classification: a lone
             * backtick template with no trailing arena yields a char[:]
             * borrow (CCSlice after slice lowering). Spell CCSlice here
             * so the statement-expr temp does not depend on slice-sugar
             * rewrite inside `({ ... })`. */
            const char* ty = "CCString";
            size_t arg1_s = cc_skip_ws_and_comments(src, n, open + 1);
            if (arg1_s < n && src[arg1_s] == '`') {
                size_t tick_e = 0;
                if (cc_tpl_scan_literal(src, n, arg1_s, &tick_e) == 0) {
                    size_t after = cc_skip_ws_and_comments(src, n, tick_e + 1);
                    if (after < n && src[after] == ')')
                        ty = "CCSlice";
                }
            }
            snprintf(frag, sizeof(frag), "({ %s __cc_tplrecv_%d = ", ty, id);
            cc_sb_append(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
            cc_sb_append_cstr(&out, &out_len, &out_cap, frag);
            cc_sb_append(&out, &out_len, &out_cap, src + i, (pe + 1) - i);
            snprintf(frag, sizeof(frag), "; __cc_tplrecv_%d", id);
            cc_sb_append_cstr(&out, &out_len, &out_cap, frag);
            cc_sb_append(&out, &out_len, &out_cap, src + (pe + 1), (ape + 1) - (pe + 1));
            cc_sb_append_cstr(&out, &out_len, &out_cap, "; })");
            last_emit = ape + 1;
            i = ape + 1;
        }
    }
    if (!out) return NULL;
    cc_sb_append(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

#define CC__CANON_STEP(name) \
    do { if (getenv("CC_DEBUG_CANON")) fprintf(stderr, "[cc:canon] %s\n", name); } while (0)

static int cc__apply_phase1_canonical_passes(CCPassChain* chain,
                                             const char* input_path,
                                             int skip_comptime_surface) {
    if (!chain) return -1;
    /* Shared phase-1 bucket: normalize CC surface syntax into more canonical
       CC, but do not introduce parser stubs or host-C survival/lowering. */
    /* Container surface aliases first: `Vec::[T]` / `vec_new::[T]` become
     * their CC-prefixed instance-layer spellings before any recognizer. */
    CC__CANON_STEP("cc__rewrite_container_surface_aliases"); if (cc_pass_chain_apply(chain, cc__rewrite_container_surface_aliases(chain->src, chain->len)) < 0) return -1;
    /* `@typehooks on T { .create = …, }` → `@comptime { cc_type_register(...) }`
     * so existing hook collectors see the legacy form. */
    CC__CANON_STEP("cc_rewrite_typehooks_to_register");
    if (cc_pass_chain_apply(chain, cc_rewrite_typehooks_to_register(chain->src, chain->len)) < 0)
        return -1;
    CC__CANON_STEP("cc_rewrite_arena_stack_destroy");
    if (cc_pass_chain_apply(chain, cc_rewrite_arena_stack_destroy(chain->src, chain->len)) < 0)
        return -1;
    /* @grammar declarations lower FIRST: the generated types (schemas,
     * Readers, tape Nodes) must be REAL in the canonical stream — the type
     * registry and comptime collectors are seeded from it, and UFCS receiver
     * resolution depends on the registry knowing these types. Idempotent:
     * a stream with no @grammar decls is untouched. Type-scoped calls
     * (`Tweet.parse(...)`) lower right after, for the same reason. */
    {
        char* g = cc_rewrite_grammar_decls_text(chain->src, chain->len, input_path);
        if (g == (char*)-1) return -1;
        if (cc_pass_chain_apply(chain, g) < 0) return -1;
        CC__CANON_STEP("cc_rewrite_type_scoped_calls_text"); if (cc_pass_chain_apply(chain, cc_rewrite_type_scoped_calls_text(chain->src, chain->len)) < 0) return -1;
    }
    /* D2.0: resolve `@comptime if (...)` first — dead branches must be pruned
     * before any other rewrite or instantiation collector sees them. */
    if (!skip_comptime_surface &&
        cc_pass_chain_apply(chain, cc__resolve_comptime_if(chain->src, chain->len, input_path)) < 0) return -1;
    CC__CANON_STEP("cc__canonicalize_with_deadline_syntax"); if (cc_pass_chain_apply(chain, cc__canonicalize_with_deadline_syntax(chain->src, chain->len)) < 0) return -1;
    CC__CANON_STEP("cc_rewrite_naked_print_aliases"); if (cc_pass_chain_apply(chain, cc_rewrite_naked_print_aliases(chain->src, chain->len)) < 0) return -1;
    cc_note_tu_map_key_pairs(chain->src, chain->len);
    CC__CANON_STEP("cc__normalize_template_recv_chains"); if (cc_pass_chain_apply(chain, cc__normalize_template_recv_chains(chain->src, chain->len)) < 0) return -1;
    if (!skip_comptime_surface &&
        cc_pass_chain_apply(chain, cc__rewrite_string_templates(chain->src, chain->len, input_path)) < 0) return -1;
    /* @variant + schema `one of` consumption dialect (spec/draft_variants.md,
     * spec/cc_serdes.md).  Runs BEFORE slice/result type lowering so arm
     * types like `T[:]` are still visible to those passes in the emitted
     * union, and before the `!>`/`?>` result-unwrap phase-3 passes so
     * projection suffix handlers are consumed here.  The decls pass resets
     * the registry, lowers @variant decls, and commits schema unions queued
     * by grammar emit; the uses pass is a no-op when the registry is empty. */
    {
        char* vd = cc_rewrite_variant_decls_text(chain->src, chain->len, input_path);
        if (vd == (char*)-1) return -1;
        if (cc_pass_chain_apply(chain, vd) < 0) return -1;
        if (cc_variant_registry_count() > 0) {
            char* vu = cc_rewrite_variant_uses_text(chain->src, chain->len, input_path);
            if (vu == (char*)-1) return -1;
            if (cc_pass_chain_apply(chain, vu) < 0) return -1;
        }
    }
    /* Channel-pair lowering must run while `[~ ... >]` / `[~ ... <]` bracket
     * declarations are still in the source; chan-handle lowering erases them. */
    CC__CANON_STEP("cc__rewrite_channel_pair_pass"); if (cc_pass_chain_apply(chain, cc__rewrite_channel_pair_pass(chain->src, chain->len, input_path)) < 0) return -1;
    CC__CANON_STEP("cc__rewrite_chan_handle_types"); if (cc_pass_chain_apply(chain, cc__rewrite_chan_handle_types(chain->src, chain->len, input_path)) < 0) return -1;
    CC__CANON_STEP("cc__rewrite_slice_types"); if (cc_pass_chain_apply(chain, cc__rewrite_slice_types(chain->src, chain->len, input_path)) < 0) return -1;
    CC__CANON_STEP("cc_rewrite_generic_containers"); if (cc_pass_chain_apply(chain, cc_rewrite_generic_containers(chain->src, chain->len, input_path)) < 0) return -1;
    CC__CANON_STEP("cc__rewrite_optional_types"); if (cc_pass_chain_apply(chain, cc__rewrite_optional_types(chain->src, chain->len, input_path)) < 0) return -1;
    /* D1.0: fold `type_of(T).size`/`.align` to `sizeof(T)`/`_Alignof(T)`.  After
     * container lowering so mangled names (`CCVec_int`) are already in place. */
    CC__CANON_STEP("cc__lower_type_of_constexpr"); if (cc_pass_chain_apply(chain, cc__lower_type_of_constexpr(chain->src, chain->len)) < 0) return -1;
    CC__CANON_STEP("cc__rewrite_inferred_result_ctors"); if (cc_pass_chain_apply(chain, cc__rewrite_inferred_result_ctors(chain->src, chain->len)) < 0) return -1;
    CC__CANON_STEP("cc__rewrite_result_types"); if (cc_pass_chain_apply(chain, cc__rewrite_result_types(chain->src, chain->len, input_path)) < 0) return -1;
    CC__CANON_STEP("cc__rewrite_result_field_sugar_pass"); if (cc_pass_chain_apply(chain, cc__rewrite_result_field_sugar_pass(chain->src, chain->len)) < 0) return -1;
    /* Parameter defaults (`int pad = 1`) are CC spelling for reflection /
     * py_module optional kwargs; strip them before host C sees the TU. */
    {
        char* pd = cc__rewrite_param_defaults(chain->src, chain->len, input_path);
        if (pd == (char*)-1) return -1;
        CC__CANON_STEP("cc__rewrite_param_defaults");
        if (cc_pass_chain_apply(chain, pd) < 0) return -1;
    }
    /* Rewrite `@async void fn(...)` -> `@async CCAsyncVoidRet fn(...)` so
     * that phase-3 reparse sees a task-returning signature (required for
     * spawn-site lowerings such as `n->spawn_async(fn(args))` to type-check).
     * async_ast recognises CCAsyncVoidRet as an originally-void declaration. */
    CC__CANON_STEP("cc__rewrite_async_void_ret"); if (cc_pass_chain_apply(chain, cc__rewrite_async_void_ret(chain->src, chain->len)) < 0) return -1;
    /* Call-site `@blocking f(...)` / `@noblock f(...)` annotations
     * (spec §8.2.2 rule 1).  Runs before @await so an `@await`-wrapping
     * is not accidentally treated as a call-site mode host. */
    CC__CANON_STEP("cc__rewrite_at_call_site_mode"); if (cc_pass_chain_apply(chain, cc__rewrite_at_call_site_mode(chain->src, chain->len)) < 0) return -1;
    /* @await fname(...) -> cc_block_on(ReturnType, fname(...)).  Runs after
     * result-type rewriting so the return types are already in canonical form. */
    CC__CANON_STEP("cc__rewrite_at_await"); if (cc_pass_chain_apply(chain, cc__rewrite_at_await(chain->src, chain->len)) < 0) return -1;
    return 0;
}

static int cc__apply_phase3_host_lowering_passes(CCPassChain* chain,
                                                 const char* input_path) {
    if (!chain) return -1;
    /* Shared phase-3 bucket: parser/host-C survival and lowering after
       canonical CC has been established and phase 2 comptime effects are
       conceptually available. */
    /* Pull `@as` field metadata from included .cch headers before text UFCS
     * and unwrap-destroy — those headers remain `#include` in the host
     * buffer and are otherwise invisible to TU-local field scans. */
    {
        CCTypeRegistry* reg = cc_type_registry_get_global();
        if (reg) {
            /* TUs that `#include <….h>` never hit the `.cch→.h` rewrite's
             * registration side-effect; map those includes back to `.cch`. */
            cc__register_cch_trees_from_angle_includes(chain->src, chain->len);
            cc_ingest_included_cch_struct_fields(reg);
        }
    }
    /* UFCS must lower `recv.method(...)` before `!>` / `?>` expand the
     * call into `__typeof__(call)` / `__cc_uw_*` form.  Expanding first
     * left UFCS callees like `ln.accept()` inside the unwrap temp, so
     * `__cc_uw_err_at`'s default arm typed the binder as `CCError` even
     * when the Result error arm is `CCNetError`.  Seed + text UFCS run
     * once before unwrap (LHS of `!>`) and once after (method calls that
     * live only in handler bodies, e.g. `server->is_cancelled()`). */
    cc__seed_ufcs_receiver_types(chain->src, chain->len);
    {
        /* TU typedef structs (with `@as`) are not covered by the CCFile/Arena
         * var seed above; ingest them before decl-time @as graph checks. */
        CCTypeRegistry* reg = cc_type_registry_get_global();
        if (reg) {
            cc_type_registry_ingest_struct_fields(reg, chain->src, chain->len);
            if (cc_type_registry_validate_as_graphs(reg, input_path, chain->src,
                                                    chain->len) != 0) {
                return -1;
            }
        }
    }
    if (cc_pass_chain_apply(chain, cc_rewrite_generic_family_ufcs_parser_safe(chain->src, chain->len, input_path)) < 0) {
        return -1;
    }
    /* After UFCS lowering: `T* p = cc_arena_alloc_T[_count](...) @destroy;`
     * releases through the owning arena (the lowered call's second
     * argument spells the correct arena reference). */
    if (cc_pass_chain_apply(chain, cc__rewrite_alloct_destroy_annotations(chain->src, chain->len)) < 0) return -1;
    /* Result-unwrap operators `?>` (expression) and `!>` (statement) run
     * before the legacy err-syntax rewrite. The `!>` statement form lowers
     * to the existing `@err` shorthands which the legacy pass then
     * processes, so order matters here. */
    {
        /* Invoke pass_result_unwrap whenever the source still contains `?>`
         * or `!>` operator sigils, OR whenever strict-mode is enabled (in
         * which case the final unhandled-result scan must run even if the
         * operators are absent).  Strict mode is on unless opted out with
         * `CC_STRICT_RESULT_UNWRAP=0`. */
        const char* strict_env = getenv("CC_STRICT_RESULT_UNWRAP");
        int strict_on = !(strict_env && strict_env[0] == '0' && strict_env[1] == 0);
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
             * Visitor unwrap-destroy also runs this rewrite on header /
             * factory text that never goes through shadow_lower parse. */
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
             * (baseline arms) and emit-plan Result-arm formatting
             * (per-spec enumerated arms).  The pre-lowering scan that used
             * to populate a pointer-fn registry has been removed — the
             * lowering now
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
    if (cc_pass_chain_apply(chain, cc__reject_match_syntax(chain->src, chain->len, input_path)) < 0) return -1;
    cc__seed_ufcs_receiver_types(chain->src, chain->len);
    /* Post-unwrap UFCS: handler bodies and other spans that still carry
     * member-call surface after `!>` expansion. */
    if (cc_pass_chain_apply(chain, cc_rewrite_generic_family_ufcs_parser_safe(chain->src, chain->len, input_path)) < 0) {
        return -1;
    }
    if (cc_pass_chain_apply(chain, cc__rewrite_result_star_unwrap(chain->src, chain->len)) < 0) return -1;
    if (cc_pass_chain_apply(chain, cc__rewrite_cc_concurrent(chain->src, chain->len)) < 0) return -1;
    if (cc_pass_chain_apply(chain, cc__rewrite_link_directives(chain->src, chain->len)) < 0) return -1;
    return 0;
}

char* cc_preprocess_comptime_source(const char* input_path) {
    if (!input_path || !input_path[0]) return NULL;
    /* Registration / UFCS discovery scans @comptime registration text from the
       include-expanded stream (user file + harvested header bodies). That text
       is already host-C / lowered .h content — running full phase-1 sugar
       lowering over the expanded prelude was the dominant emit-c cost and is
       not required for collecting cc_type_register / UFCS hooks.
       Surface sugar for the main TU still goes through build_parse_input. */
    {
        char* raw = cc_preprocess_include_expanded(input_path);
        size_t dlen = 0;
        char* ded;
        if (!raw) return NULL;
        /* Same dedent the main pipeline applies, so the comptime view of a
         * template (factory @emit bodies, reflection snapshots) matches the
         * lowered one byte for byte. */
        ded = cc_tpl_dedent_text(raw, strlen(raw), input_path, &dlen);
        if (ded == (char*)-1) { free(raw); return NULL; }
        if (ded) { free(raw); return ded; }
        return raw;
    }
}
