#include "pass_unwrap_destroy.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "comptime/symbols.h"
#include "util/text.h"
#include "visitor/pass_common.h"

#define cc__append_n   cc_sb_append
#define cc__append_str cc_sb_append_cstr
CC_DEFINE_SB_APPEND_FMT

/* Ambient symbol table consulted for bodyless `@destroy;` on user types
 * registered via `@comptime cc_type_register(...)`.  Callers that have a
 * live `CCSymbolTable*` should call cc_unwrap_destroy_set_symbols() before
 * invoking cc__rewrite_unwrap_destroy_suffix(); resolver falls back to the
 * builtin-owned hardcoded list when no table is set.
 *
 * Thread-local to stay safe under the compiler's parallel-driver use.  Follows
 * the same pattern as cc_ufcs_set_symbols() in cc/src/visitor/ufcs.c. */
static _Thread_local CCSymbolTable* g_ud_symbols = NULL;

void cc_unwrap_destroy_set_symbols(CCSymbolTable* symbols) {
    g_ud_symbols = symbols;
}

/* Skip C comments and string/char literals starting at `s[*i]`.  Advances
 * `*i` past the skipped region.  Returns 1 if any input was consumed. */
static int cc__ud_skip_comment_or_string(const char* s, size_t n, size_t* i) {
    if (*i >= n) return 0;
    char c = s[*i];
    if (c == '/' && *i + 1 < n && s[*i + 1] == '/') {
        *i += 2;
        while (*i < n && s[*i] != '\n') (*i)++;
        return 1;
    }
    if (c == '/' && *i + 1 < n && s[*i + 1] == '*') {
        *i += 2;
        while (*i + 1 < n && !(s[*i] == '*' && s[*i + 1] == '/')) (*i)++;
        if (*i + 1 < n) *i += 2;
        return 1;
    }
    if (c == '"' || c == '\'') {
        char q = c;
        (*i)++;
        while (*i < n && s[*i] != q) {
            if (s[*i] == '\\' && *i + 1 < n) { *i += 2; continue; }
            (*i)++;
        }
        if (*i < n) (*i)++;
        return 1;
    }
    return 0;
}

/* Return 1 if position `pos` in `s` lies inside a `//` line comment on
 * the same line.  Used by the backward scanner to skip over commented
 * characters without mistaking them for statement terminators. */
static int cc__ud_pos_in_line_comment(const char* s, size_t pos) {
    size_t line_start = pos;
    while (line_start > 0 && s[line_start - 1] != '\n') line_start--;
    size_t j = line_start;
    int in_str = 0;
    char qch = 0;
    while (j < pos) {
        char c = s[j];
        if (in_str) {
            if (c == '\\' && j + 1 < pos) { j += 2; continue; }
            if (c == qch) in_str = 0;
            j++;
            continue;
        }
        if (c == '"' || c == '\'') { in_str = 1; qch = c; j++; continue; }
        if (c == '/' && j + 1 < pos && s[j + 1] == '/') return 1;
        j++;
    }
    return 0;
}

/* Walk backward from `pos` (exclusive) to find the start of the current
 * statement — right after the nearest enclosing `{`, `}`, `;`, or the
 * start of file.  Balanced `{ ... }` bodies are traversed transparently
 * by counting depth.  String/comment aware. */
static size_t cc__ud_stmt_start_backward(const char* s, size_t pos) {
    size_t i = pos;
    int brace_depth = 0;
    int paren_depth = 0;
    while (i > 0) {
        size_t k = i - 1;
        char c = s[k];
        /* Skip block comment: when `s[k-1..k+1]` is a `*` `/` pair
         * terminating a `/ * ... * /` block, rewind `i` to just before
         * the matching `/ *`. */
        if (c == '/' && k > 0 && s[k - 1] == '*') {
            size_t j = k - 1;
            while (j > 0 && !(s[j - 1] == '/' && s[j] == '*')) j--;
            i = (j > 0) ? (j - 1) : 0;
            continue;
        }
        /* Skip line comment: if `k` is inside a `//...` on this line,
         * rewind `i` to the position of the `//` so we still process any
         * code tokens (e.g. a `{` or `}`) that appear before the comment
         * on the same line. */
        if (c != '\n' && cc__ud_pos_in_line_comment(s, k)) {
            size_t line_start = k;
            while (line_start > 0 && s[line_start - 1] != '\n') line_start--;
            size_t slash = line_start;
            int in_str = 0; char qch = 0;
            while (slash < k) {
                char ch = s[slash];
                if (in_str) {
                    if (ch == '\\' && slash + 1 < k) { slash += 2; continue; }
                    if (ch == qch) in_str = 0;
                    slash++;
                    continue;
                }
                if (ch == '"' || ch == '\'') { in_str = 1; qch = ch; slash++; continue; }
                if (ch == '/' && slash + 1 < k && s[slash + 1] == '/') break;
                slash++;
            }
            i = slash;
            continue;
        }
        if (c == ')') { paren_depth++; i--; continue; }
        if (c == '(') {
            if (paren_depth > 0) paren_depth--;
            i--;
            continue;
        }
        if (c == '}') { brace_depth++; i--; continue; }
        if (c == '{') {
            if (brace_depth > 0) { brace_depth--; i--; continue; }
            return i; /* stop AFTER the `{` — stmt starts here */
        }
        if (c == ';' && paren_depth == 0 && brace_depth == 0) return i;
        i--;
    }
    return 0;
}

/* Extract the declared variable name from a statement range
 * `s[stmt_a .. op_pos)` that looks like `Type ptr_or_ref name = expr ...`.
 * Fills `*name_a`, `*name_b` with the identifier span if found.  The scan
 * looks for the top-level `=` sign in the range, walks back over spaces,
 * and grabs the preceding identifier token.  Returns 1 on success. */
static int cc__ud_extract_decl_name(const char* s, size_t stmt_a, size_t op_pos,
                                    size_t* name_a, size_t* name_b) {
    /* Find top-level `=` between stmt_a and op_pos. */
    size_t eq = (size_t)-1;
    int paren = 0, brace = 0, brack = 0;
    size_t i = stmt_a;
    while (i < op_pos) {
        /* Use local scanner for comments/strings. */
        size_t save = i;
        if (cc__ud_skip_comment_or_string(s, op_pos, &i)) continue;
        if (i == save) {
            char c = s[i];
            if (c == '(') paren++;
            else if (c == ')') { if (paren > 0) paren--; }
            else if (c == '[') brack++;
            else if (c == ']') { if (brack > 0) brack--; }
            else if (c == '{') brace++;
            else if (c == '}') { if (brace > 0) brace--; }
            else if (c == '=' && paren == 0 && brace == 0 && brack == 0) {
                if (i + 1 < op_pos && s[i + 1] == '=') { i += 2; continue; }
                eq = i;
                break;
            }
            i++;
        }
    }
    if (eq == (size_t)-1) return 0;
    /* Walk back over whitespace. */
    size_t p = eq;
    while (p > stmt_a && isspace((unsigned char)s[p - 1])) p--;
    size_t nb = p;
    while (p > stmt_a && (isalnum((unsigned char)s[p - 1]) || s[p - 1] == '_')) p--;
    size_t na = p;
    if (na >= nb) return 0;
    /* Leading char must be ident-start. */
    if (!(isalpha((unsigned char)s[na]) || s[na] == '_')) return 0;
    *name_a = na;
    *name_b = nb;
    return 1;
}

/* Normalize the declared type span `s[type_a .. type_b)` into a lookup
 * key suitable for `cc_symbols_lookup_type_destroy_call`: strip storage
 * classes / qualifiers (static/const/volatile/restrict) and collapse
 * interior whitespace, then copy into `out`.  Out-param `*had_pointer`
 * is set to 1 if the span contains `*` (so callers can decide whether
 * the registered destroy callee wants `name` or `&name`).  Returns 0 on
 * success, -1 if the result would not fit in `out`. */
static int cc__ud_normalize_type_name(const char* s, size_t type_a, size_t type_b,
                                      char* out, size_t out_cap,
                                      int* had_pointer) {
    if (!out || out_cap == 0) return -1;
    if (had_pointer) *had_pointer = 0;
    static const char* const skip_words[] = {
        "static", "const", "volatile", "restrict", "register", "extern", "inline"
    };
    static const int n_skip = (int)(sizeof(skip_words) / sizeof(skip_words[0]));

    size_t o = 0;
    size_t i = type_a;
    int last_was_space = 1;
    while (i < type_b) {
        char c = s[i];
        if (isspace((unsigned char)c)) {
            if (!last_was_space && o > 0 && o + 1 < out_cap) {
                out[o++] = ' ';
                last_was_space = 1;
            }
            i++;
            continue;
        }
        if (c == '*') {
            if (had_pointer) *had_pointer = 1;
            if (o > 0 && out[o - 1] == ' ') o--;
            if (o + 1 >= out_cap) return -1;
            out[o++] = '*';
            last_was_space = 0;
            i++;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t start = i;
            while (i < type_b && (isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
            size_t len = i - start;
            int skip = 0;
            for (int k = 0; k < n_skip; k++) {
                size_t kl = strlen(skip_words[k]);
                if (len == kl && memcmp(s + start, skip_words[k], kl) == 0) { skip = 1; break; }
            }
            if (skip) continue;
            if (o + len >= out_cap) return -1;
            memcpy(out + o, s + start, len);
            o += len;
            last_was_space = 0;
            continue;
        }
        if (o + 1 >= out_cap) return -1;
        out[o++] = c;
        last_was_space = 0;
        i++;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    if (o >= out_cap) return -1;
    out[o] = '\0';
    return 0;
}

/* Tighten the declared-type span.  `cc__ud_stmt_start_backward` returns
 * the start of the enclosing brace scope, which for a declaration like
 *
 *     @errhandler(CCError e) { ... }
 *     MyThing* t = create() !> @destroy;
 *
 * points at the start of the function body — so `[type_a .. type_b)` would
 * engulf the entire `@errhandler { ... }` prelude.  The builtin-hardcoded
 * resolver papers over this via substring match, but a symbol-table lookup
 * needs a clean key.
 *
 * Walk backward from the declared-name start, collecting contiguous type
 * tokens (identifiers, `*`, whitespace).  Stop at the first punctuation
 * that can't be part of a C type (`}`, `;`, `)`, `,`, `=`), which gives
 * the real start of the type text.  Returns the tightened start offset
 * (clamped to `type_a` at the low end). */
static size_t cc__ud_tighten_type_start(const char* s, size_t type_a, size_t name_a) {
    size_t k = name_a;
    while (k > type_a && isspace((unsigned char)s[k - 1])) k--;
    while (k > type_a) {
        char c = s[k - 1];
        if (isalnum((unsigned char)c) || c == '_' || c == '*' ||
            c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            k--;
            continue;
        }
        break;
    }
    while (k < name_a && isspace((unsigned char)s[k])) k++;
    return k;
}

/* Resolve destroy hooks from a `@comptime cc_type_register(...)` entry on
 * the ambient symbol table.  Fills `*pre_hook` / `*post_hook` (may be NULL
 * individually if not registered).  The registered destroy callee for a
 * pointer type takes `name` directly; for a non-pointer type it takes
 * `&name` — `*post_pass_address` is set to reflect that.  Returns 1 if
 * either hook resolved, 0 if neither did. */
static int cc__ud_symtab_owned_hooks(const char* s, size_t type_a, size_t type_b,
                                     const char** pre_hook,
                                     const char** post_hook,
                                     int* post_pass_address) {
    *pre_hook = NULL;
    *post_hook = NULL;
    *post_pass_address = 0;
    if (!g_ud_symbols) return 0;

    size_t tight_a = cc__ud_tighten_type_start(s, type_a, type_b);
    char key[256];
    int had_ptr = 0;
    if (cc__ud_normalize_type_name(s, tight_a, type_b, key, sizeof(key), &had_ptr) != 0) {
        return 0;
    }
    if (key[0] == '\0') return 0;

    const char* pre_callee = NULL;
    const char* post_callee = NULL;
    (void)cc_symbols_lookup_type_pre_destroy_call(g_ud_symbols, key, &pre_callee);
    (void)cc_symbols_lookup_type_destroy_call(g_ud_symbols, key, &post_callee);

    if (!pre_callee && !post_callee) return 0;

    *pre_hook = pre_callee;
    *post_hook = post_callee;
    /* Registered destroy callees take the raw pointer for pointer types
     * and `&name` for non-pointer value types — matches the convention
     * already used by pass_create.c when emitting the @create/@destroy
     * lowering. */
    *post_pass_address = had_ptr ? 0 : 1;
    return 1;
}

/* Return 1 if the declared type text `s[stmt_a .. name_a)` is
 * `CCNursery*` (possibly with spaces / `const`).  Fills a pair of
 * destructor callees `*pre_hook` / `*post_hook` — the former runs
 * BEFORE the user body, the latter AFTER.  For CCNursery this gives
 * the standard "wait for tasks, then free" lifecycle.  Returns 0 if
 * the type is not a recognized builtin-owned type. */
static int cc__ud_builtin_owned_hooks(const char* s, size_t type_a, size_t type_b,
                                      const char** pre_hook,
                                      const char** post_hook,
                                      int* post_pass_address) {
    *pre_hook = NULL;
    *post_hook = NULL;
    *post_pass_address = 0;
    int saw_nursery = 0, saw_arena = 0, saw_chan = 0, saw_star = 0;
    size_t i = type_a;
    while (i < type_b) {
        char c = s[i];
        if (c == '*') { saw_star = 1; i++; continue; }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t start = i;
            while (i < type_b && (isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
            size_t len = i - start;
            if (len == 9 && memcmp(s + start, "CCNursery", 9) == 0) saw_nursery = 1;
            else if (len == 7 && memcmp(s + start, "CCArena", 7) == 0) saw_arena = 1;
            else if (len == 6 && memcmp(s + start, "CCChan", 6) == 0) saw_chan = 1;
            continue;
        }
        i++;
    }
    if (saw_nursery && saw_star && !saw_arena && !saw_chan) {
        *pre_hook = "cc_nursery_wait";
        *post_hook = "cc_nursery_free";
        *post_pass_address = 0;
        return 1;
    }
    if (saw_arena && !saw_nursery && !saw_chan && !saw_star) {
        *pre_hook = NULL;
        *post_hook = "cc_arena_destroy";
        *post_pass_address = 1;
        return 1;
    }
    if (saw_chan && saw_star && !saw_nursery && !saw_arena) {
        *pre_hook = NULL;
        *post_hook = "cc_channel_free";
        *post_pass_address = 0;
        return 1;
    }
    return 0;
}

/* Return 1 if the range `s[a..b)` contains a bare `!>` or `?>` operator
 * (at top-level, respecting strings/comments and brace depth). */
static int cc__ud_range_has_unwrap_op(const char* s, size_t a, size_t b) {
    size_t i = a;
    while (i < b) {
        if (cc__ud_skip_comment_or_string(s, b, &i)) continue;
        char c = s[i];
        if ((c == '!' || c == '?') && i + 1 < b && s[i + 1] == '>') {
            /* Make sure it's the op (`!>` or `?>`), not `!>=` or `?>=`
             * or `!>>` shift (bogus for `?`, still skip defensively). */
            if (i + 2 < b && (s[i + 2] == '=' || s[i + 2] == '>')) {
                i += 2;
                continue;
            }
            return 1;
        }
        i++;
    }
    return 0;
}

/* Return 1 iff `s[i..i+8)` is the literal token `@destroy` at a word
 * boundary (next char is ws/{/(/;/\0). */
static int cc__ud_is_at_destroy(const char* s, size_t n, size_t i) {
    if (i + 8 > n) return 0;
    if (memcmp(s + i, "@destroy", 8) != 0) return 0;
    if (i + 8 < n) {
        char nx = s[i + 8];
        if (nx == '_' || isalnum((unsigned char)nx)) return 0;
    }
    return 1;
}

/* Append newline-preserving whitespace that mirrors `s[a..b)` so that
 * line numbers downstream stay stable. */
static void cc__ud_emit_pad(char** out, size_t* out_len, size_t* out_cap,
                            const char* s, size_t a, size_t b) {
    for (size_t i = a; i < b; i++) {
        char c = s[i];
        cc__append_n(out, out_len, out_cap, (c == '\n') ? "\n" : " ", 1);
    }
}

/* Compute 1-based (line, col) for source offset `off` in buffer `src`. */
static void cc__ud_offset_to_line_col(const char* src, size_t off,
                                      int* out_line, int* out_col) {
    int line = 1, col = 1;
    for (size_t i = 0; src && i < off; i++) {
        if (src[i] == '\n') { line++; col = 1; }
        else col++;
    }
    if (out_line) *out_line = line;
    if (out_col) *out_col = col;
}

int cc__rewrite_unwrap_destroy_suffix(const char* src,
                                      size_t n,
                                      const char* input_path,
                                      char** out_buf,
                                      size_t* out_len) {
    if (!src || n == 0 || !out_buf || !out_len) {
        if (out_buf) *out_buf = NULL;
        if (out_len) *out_len = 0;
        return 0;
    }
    *out_buf = NULL;
    *out_len = 0;

    char* out = NULL;
    size_t ol = 0, oc = 0;
    size_t last_emit = 0;
    int changed = 0;

    size_t i = 0;
    while (i < n) {
        if (cc__ud_skip_comment_or_string(src, n, &i)) continue;

        if (!cc__ud_is_at_destroy(src, n, i)) { i++; continue; }

        /* The unwrap-suffix form is only recognized when the current
         * statement contains `!>` or `?>`.  `@create(...) @destroy { ... }`
         * lives in statements without those operators, so pass_create
         * continues to own that shape. */
        size_t stmt_a = cc__ud_stmt_start_backward(src, i);
        if (!cc__ud_range_has_unwrap_op(src, stmt_a, i)) {
            i += 8;
            continue;
        }

        /* Parse `@destroy [ { body } ] ;`.  Bodyless form `@destroy;` is
         * meaningful for builtin-owned types (auto-free).  The `;` ending
         * the host statement is still required. */
        size_t after_kw = i + 8;
        size_t cur = after_kw;
        while (cur < n &&
               (src[cur] == ' ' || src[cur] == '\t' ||
                src[cur] == '\n' || src[cur] == '\r'))
            cur++;
        int have_body = 0;
        size_t body_s = 0, body_e = 0;
        if (cur < n && src[cur] == '{') {
            body_s = cur;
            if (!cc_find_matching_brace(src, n, body_s, &body_e)) {
                i = body_s + 1;
                continue;
            }
            have_body = 1;
            cur = body_e + 1;
            while (cur < n &&
                   (src[cur] == ' ' || src[cur] == '\t' ||
                    src[cur] == '\n' || src[cur] == '\r'))
                cur++;
        }
        size_t semi = cur;
        if (semi >= n || src[semi] != ';') {
            i = have_body ? body_e + 1 : after_kw;
            continue;
        }

        /* If the host statement is a declaration of a builtin-owned
         * type (e.g. `CCNursery* n = ...`), we need to ALSO emit the
         * implicit lifecycle hooks (wait + free for a nursery; destroy
         * for an arena) around the user-supplied body.  Otherwise a
         * plain `@defer { user_body }` suffices. */
        size_t name_a = 0, name_b = 0;
        int have_name = cc__ud_extract_decl_name(src, stmt_a, i, &name_a, &name_b);
        const char* pre_hook = NULL;
        const char* post_hook = NULL;
        int post_pass_addr = 0;
        if (have_name) {
            /* Type span is [stmt_a .. name_a), trimmed of trailing ws. */
            size_t type_b = name_a;
            while (type_b > stmt_a && isspace((unsigned char)src[type_b - 1])) type_b--;
            /* Prefer `@comptime cc_type_register` hooks over the hardcoded
             * builtin-owned trio.  The builtin types (CCNursery*, CCArena,
             * CCChan*) register themselves via cc_type_register too, so
             * when a symbol table is live the registered entries drive the
             * lowering uniformly.  The hardcoded list remains as a fallback
             * for call sites where no symbol table is installed (e.g.
             * out-of-tree tools invoking the pass directly). */
            if (!cc__ud_symtab_owned_hooks(src, stmt_a, type_b,
                                           &pre_hook, &post_hook, &post_pass_addr)) {
                cc__ud_builtin_owned_hooks(src, stmt_a, type_b,
                                           &pre_hook, &post_hook, &post_pass_addr);
            }
        }

        /* Bodyless `@destroy;` is only meaningful when the declared type has a
         * registered destructor (a builtin-owned type).  Otherwise there's
         * nothing for the synthesized `@defer { }` to run — refuse to silently
         * emit a no-op and tell the user what's needed. */
        if (!have_body && !pre_hook && !post_hook) {
            int line = 1, col = 1;
            cc__ud_offset_to_line_col(src, i, &line, &col);
            const char* f = input_path ? input_path : "<input>";
            if (have_name) {
                /* Trim the declared type span for the diagnostic. */
                size_t type_a = stmt_a;
                size_t type_b = name_a;
                while (type_a < type_b && isspace((unsigned char)src[type_a])) type_a++;
                while (type_b > type_a && isspace((unsigned char)src[type_b - 1])) type_b--;
                int type_len = (int)(type_b - type_a);
                fprintf(stderr,
                        "%s:%d:%d: error: @destroy: bodyless `@destroy;` requires a "
                        "registered destructor for type `%.*s`, but none is known\n",
                        f, line, col, type_len, src + type_a);
                fprintf(stderr,
                        "%s:%d:%d: note: provide an explicit cleanup body: "
                        "`@destroy { /* cleanup using %.*s */ }`\n",
                        f, line, col, (int)(name_b - name_a), src + name_a);
            } else {
                fprintf(stderr,
                        "%s:%d:%d: error: @destroy: bodyless `@destroy;` is only "
                        "allowed on a declaration of a type with a registered "
                        "destructor\n",
                        f, line, col);
                fprintf(stderr,
                        "%s:%d:%d: note: either provide `@destroy { ... }` with an "
                        "explicit cleanup body, or declare the value with a "
                        "registered owner type (e.g. CCNursery*, CCArena, CCChan*)\n",
                        f, line, col);
            }
            free(out);
            return -1;
        }

        /* Emit everything up to (but not including) the `@destroy` kw. */
        cc__append_n(&out, &ol, &oc, src + last_emit, i - last_emit);
        /* Pad the stripped `@destroy { body }` range with whitespace so
         * that the statement terminator `;` still lands on the original
         * line — keeps TCC line numbers in sync with the source view. */
        cc__ud_emit_pad(&out, &ol, &oc, src, i, semi);
        /* Emit the `;` terminating the original statement. */
        cc__append_n(&out, &ol, &oc, ";", 1);
        /* Now synthesize the standalone `@defer { ... };` on the same
         * line — the defer-syntax pass will attach it to the enclosing
         * scope. */
        cc__append_str(&out, &ol, &oc, " @defer { ");
        if (pre_hook && have_name) {
            cc__append_str(&out, &ol, &oc, pre_hook);
            cc__append_str(&out, &ol, &oc, "(");
            cc__append_n(&out, &ol, &oc, src + name_a, name_b - name_a);
            cc__append_str(&out, &ol, &oc, "); ");
        }
        /* The emitted `@defer { ... };` is injected inline on a single
         * output line.  Collapse any newlines inside the user body to
         * spaces so the entire synthesized statement stays on one line
         * — multi-line `@destroy` bodies otherwise leave stray newlines
         * and horizontal whitespace inside the `@defer` body that the
         * downstream defer pass compresses with `cc__append_newline_
         * padding`, shifting later text (spawns, closures) and
         * confusing the closure-literal pass.  Line numbers are already
         * preserved via the padding emitted for the original source
         * span. */
        if (have_body) {
            for (size_t bi = body_s + 1; bi < body_e; ) {
                char bc = src[bi];
                /* Strip `//` line comments: they would comment out everything
                 * after them on the collapsed single line. */
                if (bc == '/' && bi + 1 < body_e && src[bi + 1] == '/') {
                    while (bi < body_e && src[bi] != '\n') bi++;
                    continue;
                }
                /* Collapse newlines/tabs to spaces. */
                char out_ch = (bc == '\n' || bc == '\r' || bc == '\t') ? ' ' : bc;
                cc__append_n(&out, &ol, &oc, &out_ch, 1);
                bi++;
            }
        }
        if (post_hook && have_name) {
            cc__append_str(&out, &ol, &oc, " ");
            cc__append_str(&out, &ol, &oc, post_hook);
            cc__append_str(&out, &ol, &oc, "(");
            if (post_pass_addr) cc__append_str(&out, &ol, &oc, "&");
            cc__append_n(&out, &ol, &oc, src + name_a, name_b - name_a);
            cc__append_str(&out, &ol, &oc, ");");
        }
        cc__append_str(&out, &ol, &oc, " };");

        last_emit = semi + 1;
        i = last_emit;
        changed = 1;
    }

    if (!changed) {
        free(out);
        return 0;
    }

    if (last_emit < n) {
        cc__append_n(&out, &ol, &oc, src + last_emit, n - last_emit);
    }

    *out_buf = out;
    *out_len = ol;
    return 1;
}
