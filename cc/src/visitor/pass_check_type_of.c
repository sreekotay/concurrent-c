/*
 * pass_check_type_of.c — see header banner for the API and tuning
 * knobs.  This file implements the scan in two passes over the
 * user TU's source buffer:
 *
 *   Pass 1 builds the "known names" set by adding:
 *     - hardcoded primitives + pre-baked vecs (compile-time tables);
 *     - every codegen-emitted Vec/Map name from `CCTypeRegistry`;
 *     - every `CC_TYPE_INFO_BEGIN(X)` declaration found in `src`.
 *   Doing the source scan first means a `CC_TYPE_INFO_BEGIN(Pair)`
 *   appearing AFTER a `type_of(Pair)` reference is still found.
 *
 *   Pass 2 walks `src` once more looking for `type_of(IDENT)` and
 *   `cc_type_of("IDENT")` call sites in real code (not inside
 *   comments, strings, char literals, or `#define` bodies — see
 *   `CCInertScan`).  For each call whose `IDENT` is not in the
 *   known set, emit `CC_DIAG_WARNING` with the file/line/col of
 *   the call and a hint pointing the user at the two registration
 *   options.
 *
 * The name set is a small linear array (dozens of entries in
 * practice).  Lookup is O(n) per query; if profiling ever shows
 * this matters, swap the backing store — the API doesn't change.
 */

#include "pass_check_type_of.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../diag/diag.h"
#include "../preprocess/type_registry.h"
#include "../util/text_scan.h"

/* ------------------------------------------------------------
 * Hardcoded known-name tables.
 *
 * These MUST stay in sync with `cc/runtime/cc_type_info.c`.  A
 * mismatch would either flag legitimate code as a warning
 * (annoying but recoverable) or silence a real typo (subtle bug);
 * the build invariant lives in that file's header comment.
 * ------------------------------------------------------------ */

static const char* const CC__TI_PRIMITIVES[] = {
    "int",  "char",   "short",  "long",
    "float","double", "size_t", "intptr_t", "bool",
};
enum { CC__TI_PRIMITIVES_N =
    sizeof(CC__TI_PRIMITIVES) / sizeof(CC__TI_PRIMITIVES[0]) };

static const char* const CC__TI_PREBAKED_VECS[] = {
    "CCVec_int",     "CCVec_char",    "CCVec_size_t", "CCVec_float",
    "CCVec_double",  "CCVec_voidptr", "CCVec_charptr","CCVec_intptr",
};
enum { CC__TI_PREBAKED_VECS_N =
    sizeof(CC__TI_PREBAKED_VECS) / sizeof(CC__TI_PREBAKED_VECS[0]) };

/* ------------------------------------------------------------
 * Tiny name-set helper.  Linear array, dedupe by strcmp; the
 * expected size is dozens, so O(n) lookup is fine.
 * ------------------------------------------------------------ */

typedef struct {
    char** items;
    size_t n;
    size_t cap;
} cc__nset;

static void cc__nset_init(cc__nset* s) {
    s->items = NULL;
    s->n = 0;
    s->cap = 0;
}

static void cc__nset_free(cc__nset* s) {
    for (size_t i = 0; i < s->n; i++) free(s->items[i]);
    free(s->items);
    s->items = NULL;
    s->n = s->cap = 0;
}

static int cc__nset_has(const cc__nset* s, const char* name, size_t name_len) {
    for (size_t i = 0; i < s->n; i++) {
        const char* it = s->items[i];
        if (strlen(it) == name_len && memcmp(it, name, name_len) == 0) return 1;
    }
    return 0;
}

static void cc__nset_add(cc__nset* s, const char* name, size_t name_len) {
    if (name_len == 0) return;
    if (cc__nset_has(s, name, name_len)) return;
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 32;
        char** g = (char**)realloc(s->items, nc * sizeof(*g));
        if (!g) return;
        s->items = g;
        s->cap = nc;
    }
    char* dup = (char*)malloc(name_len + 1);
    if (!dup) return;
    memcpy(dup, name, name_len);
    dup[name_len] = '\0';
    s->items[s->n++] = dup;
}

/* ------------------------------------------------------------
 * Token helpers.
 * ------------------------------------------------------------ */

static int cc__is_ident_start(char c) {
    return (c == '_') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int cc__is_ident_cont(char c) {
    return cc__is_ident_start(c) || (c >= '0' && c <= '9');
}

/* Peek the identifier starting at `src[i]`.  On match, sets
 * `*out_len` to the identifier length and returns 1.  Otherwise
 * returns 0 and leaves `*out_len` unset. */
static int cc__peek_ident(const char* src, size_t n, size_t i,
                           size_t* out_len) {
    if (i >= n || !cc__is_ident_start(src[i])) return 0;
    size_t k = i + 1;
    while (k < n && cc__is_ident_cont(src[k])) k++;
    *out_len = k - i;
    return 1;
}

/* True iff src[i..i+kw_len] is exactly `kw` AND the next char
 * (if any) is not an identifier-continuation char (so we don't
 * mismatch `type_of_extended` as `type_of`). */
static int cc__match_kw(const char* src, size_t n, size_t i,
                         const char* kw, size_t kw_len) {
    if (i + kw_len > n) return 0;
    if (memcmp(src + i, kw, kw_len) != 0) return 0;
    if (i + kw_len < n && cc__is_ident_cont(src[i + kw_len])) return 0;
    /* Also guard against being mid-identifier (e.g. `xtype_of`). */
    if (i > 0 && cc__is_ident_cont(src[i - 1])) return 0;
    return 1;
}

/* Advance past whitespace (incl. newlines), returns new index. */
static size_t cc__skip_ws(const char* src, size_t n, size_t i) {
    while (i < n && (src[i] == ' ' || src[i] == '\t' ||
                     src[i] == '\n' || src[i] == '\r')) i++;
    return i;
}

/* Convert a byte offset into 1-based line + column.  Linear in
 * `off` but only called when we emit a diag, so it's fine. */
static void cc__off_to_line_col(const char* src, size_t off,
                                  int* out_line, int* out_col) {
    int line = 1, col = 1;
    for (size_t i = 0; i < off; i++) {
        if (src[i] == '\n') { line++; col = 1; }
        else col++;
    }
    *out_line = line;
    *out_col  = col;
}

/* Build a small `CCSourceSpan` for the call's identifier token. */
static CCSourceSpan cc__make_span(const char* src, int file_id,
                                    size_t name_off, size_t name_len) {
    int line, col;
    cc__off_to_line_col(src, name_off, &line, &col);
    CCSourceSpan sp;
    sp.start.file_id = file_id;
    sp.start.line    = line;
    sp.start.col     = col;
    sp.start.byte_off = name_off;
    sp.end.file_id   = file_id;
    sp.end.line      = line;
    sp.end.col       = col + (int)name_len;
    sp.end.byte_off  = name_off + name_len;
    return sp;
}

/* ------------------------------------------------------------
 * Pass 1: collect user-registered names from the source.
 * ------------------------------------------------------------ */

static void cc__collect_user_registered(const char* src, size_t n,
                                          const char* input_path,
                                          cc__nset* known) {
    CCInertScan scan;
    cc_inert_scan_init(&scan, input_path);

    /* Match `CC_TYPE_INFO_BEGIN(X)` and `CC_TYPE_INFO_END(X, ...)`
     * — both forms carry the registered name.  We accept either so
     * the user is free to mix definition-block layouts. */
    static const char KW_BEGIN[] = "CC_TYPE_INFO_BEGIN";
    static const char KW_END[]   = "CC_TYPE_INFO_END";

    for (size_t i = 0; i < n; ) {
        if (cc_inert_scan_step(&scan, src, n, &i)) continue;
        if (!scan.in_user_file) { i++; continue; }

        /* Quick first-char filter before the strncmp. */
        if (src[i] != 'C') { i++; continue; }

        const char* kw = NULL;
        size_t kw_len = 0;
        if (cc__match_kw(src, n, i, KW_BEGIN, sizeof(KW_BEGIN) - 1)) {
            kw = KW_BEGIN; kw_len = sizeof(KW_BEGIN) - 1;
        } else if (cc__match_kw(src, n, i, KW_END, sizeof(KW_END) - 1)) {
            kw = KW_END;   kw_len = sizeof(KW_END) - 1;
        }
        if (!kw) { i++; continue; }

        size_t k = cc__skip_ws(src, n, i + kw_len);
        if (k >= n || src[k] != '(') { i++; continue; }
        k = cc__skip_ws(src, n, k + 1);

        size_t ident_len = 0;
        if (!cc__peek_ident(src, n, k, &ident_len)) { i++; continue; }
        cc__nset_add(known, src + k, ident_len);

        /* Advance past the identifier — we don't care about the
         * rest of the macro args. */
        i = k + ident_len;
    }
}

/* ------------------------------------------------------------
 * Pass 2: walk `type_of(T)` and `cc_type_of("T")` call sites.
 * ------------------------------------------------------------ */

static void cc__emit_unregistered_warning(const char* phase,
                                            CCSourceSpan span,
                                            const char* ident,
                                            size_t ident_len,
                                            int strict) {
    /* Diagnostic level: warning by default; error under
     * `CC_TYPE_OF_STRICT=1` for CI / opt-in strict builds. */
    CCDiagLevel level = strict ? CC_DIAG_ERROR : CC_DIAG_WARNING;

    char msg[256];
    snprintf(msg, sizeof(msg),
        "type_of(%.*s): type '%.*s' has no registered cc_type_info "
        "in this TU; cc_type_of(\"%.*s\") will return NULL at runtime",
        (int)ident_len, ident,
        (int)ident_len, ident,
        (int)ident_len, ident);

    char hint[384];
    snprintf(hint, sizeof(hint),
        "register '%.*s' by either (a) using `Vec<%.*s>` somewhere "
        "in this TU so the compiler emits its cc_type_info, or "
        "(b) declaring it explicitly with "
        "CC_TYPE_INFO_BEGIN(%.*s) ... CC_TYPE_INFO_END(%.*s, \"%.*s\")",
        (int)ident_len, ident,
        (int)ident_len, ident,
        (int)ident_len, ident,
        (int)ident_len, ident,
        (int)ident_len, ident);

    /* Push through the shared diag pipe — this is what bumps
     * `cc_diag_error_count()` for the strict-mode build failure
     * downstream. */
    cc_diag_emit(level, phase, CC_CONSTRUCT_NONE, span, msg, hint);

    /* For WARNINGS, also print to stderr immediately.  The driver
     * only flushes the buffered diag list (`cc_diag_print_all`) on
     * build failure, so a quiet warning would never reach the user
     * without this inline print.  Errors go through the buffered
     * path only — printing here too would duplicate output once
     * the driver flushes on the strict-mode build failure. */
    if (level == CC_DIAG_ERROR) return;
    const char* fp = cc_diag_file_path(span.start.file_id);
    fprintf(stderr, "%s:%d:%d: warning: %s\n",
            fp ? fp : "<input>", span.start.line, span.start.col, msg);
    fprintf(stderr, "  hint: %s\n", hint);
}

static size_t cc__scan_type_of_calls(const char* src, size_t n,
                                       const char* input_path,
                                       int file_id,
                                       const cc__nset* known,
                                       int strict) {
    CCInertScan scan;
    cc_inert_scan_init(&scan, input_path);

    static const char KW_TYPE_OF[]    = "type_of";
    static const char KW_CC_TYPE_OF[] = "cc_type_of";

    size_t diag_count = 0;
    for (size_t i = 0; i < n; ) {
        if (cc_inert_scan_step(&scan, src, n, &i)) continue;
        if (!scan.in_user_file) { i++; continue; }

        /* Quick prefix filter. */
        if (src[i] != 't' && src[i] != 'c') { i++; continue; }

        size_t kw_len = 0;
        int is_cc_form = 0;
        if (cc__match_kw(src, n, i, KW_TYPE_OF, sizeof(KW_TYPE_OF) - 1)) {
            kw_len = sizeof(KW_TYPE_OF) - 1;
        } else if (cc__match_kw(src, n, i, KW_CC_TYPE_OF,
                                  sizeof(KW_CC_TYPE_OF) - 1)) {
            kw_len = sizeof(KW_CC_TYPE_OF) - 1;
            is_cc_form = 1;
        } else {
            i++;
            continue;
        }

        size_t after_kw = i + kw_len;
        size_t k = cc__skip_ws(src, n, after_kw);
        if (k >= n || src[k] != '(') { i = after_kw; continue; }
        k = cc__skip_ws(src, n, k + 1);

        size_t name_off = 0, name_len = 0;
        if (is_cc_form) {
            /* `cc_type_of("X")` — only diagnose string-literal
             * forms.  Skip non-literal calls (e.g. dynamic
             * `cc_type_of(buf)`) which the user explicitly opted
             * into runtime semantics for. */
            if (k >= n || src[k] != '"') { i = after_kw; continue; }
            size_t sstart = k + 1;
            size_t send = sstart;
            while (send < n && src[send] != '"' && src[send] != '\n') send++;
            if (send >= n || src[send] != '"') { i = after_kw; continue; }
            name_off = sstart;
            name_len = send - sstart;
            i = send + 1;
        } else {
            /* `type_of(X)` — bare identifier required.  Anything
             * else (compound expression, macro arg name in a
             * `#define` body — already filtered, etc.) is left
             * alone. */
            size_t id_len = 0;
            if (!cc__peek_ident(src, n, k, &id_len)) { i = after_kw; continue; }
            /* Verify the next non-ws is `)` so we don't latch on
             * to `type_of(X + Y)` or other compound shapes. */
            size_t after_id = cc__skip_ws(src, n, k + id_len);
            if (after_id >= n || src[after_id] != ')') { i = after_kw; continue; }
            name_off = k;
            name_len = id_len;
            i = after_id + 1;
        }

        if (name_len == 0) continue;
        if (cc__nset_has(known, src + name_off, name_len)) continue;

        CCSourceSpan span = cc__make_span(src, file_id, name_off, name_len);
        cc__emit_unregistered_warning("check_type_of", span,
                                       src + name_off, name_len, strict);
        diag_count++;
    }
    return diag_count;
}

/* ------------------------------------------------------------
 * Public entry point.
 * ------------------------------------------------------------ */

size_t cc__check_type_of_calls(const char* src, size_t n,
                                const char* input_path,
                                CCTypeRegistry* reg) {
    if (!src || n == 0) return 0;

    const char* off = getenv("CC_TYPE_OF_CHECK");
    if (off && off[0] == '0') return 0;
    const char* strict_env = getenv("CC_TYPE_OF_STRICT");
    int strict = (strict_env && strict_env[0] && strict_env[0] != '0');

    int file_id = input_path ? cc_diag_register_file(input_path) : 0;

    /* Build the known set. */
    cc__nset known;
    cc__nset_init(&known);

    for (size_t i = 0; i < (size_t)CC__TI_PRIMITIVES_N; i++) {
        cc__nset_add(&known, CC__TI_PRIMITIVES[i],
                      strlen(CC__TI_PRIMITIVES[i]));
    }
    for (size_t i = 0; i < (size_t)CC__TI_PREBAKED_VECS_N; i++) {
        cc__nset_add(&known, CC__TI_PREBAKED_VECS[i],
                      strlen(CC__TI_PREBAKED_VECS[i]));
    }
    if (reg) {
        size_t nvec = cc_type_registry_vec_count(reg);
        for (size_t i = 0; i < nvec; i++) {
            const CCTypeInstantiation* it = cc_type_registry_get_vec(reg, i);
            if (it && it->mangled_name) {
                cc__nset_add(&known, it->mangled_name,
                              strlen(it->mangled_name));
            }
        }
        size_t nmap = cc_type_registry_map_count(reg);
        for (size_t i = 0; i < nmap; i++) {
            const CCTypeInstantiation* it = cc_type_registry_get_map(reg, i);
            if (it && it->mangled_name) {
                cc__nset_add(&known, it->mangled_name,
                              strlen(it->mangled_name));
            }
        }
    }
    cc__collect_user_registered(src, n, input_path, &known);

    size_t emitted = cc__scan_type_of_calls(src, n, input_path,
                                             file_id, &known, strict);
    cc__nset_free(&known);
    return emitted;
}
