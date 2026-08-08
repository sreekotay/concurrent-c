#ifndef CC_TEMPLATE_SCAN_H
#define CC_TEMPLATE_SCAN_H

#include <stddef.h>

typedef enum CCTemplatePieceKind {
    CC_TPL_PIECE_LITERAL = 0,
    CC_TPL_PIECE_SLOT = 1,
    CC_TPL_PIECE_TAGGED_SLOT = 2,
    CC_TPL_PIECE_VERBATIM = 3, /* ${{...}}: raw bytes, no escapes, no slots */
} CCTemplatePieceKind;

typedef struct CCTemplatePiece {
    CCTemplatePieceKind kind;
    size_t lit_off;
    size_t lit_len;
    size_t tag_off;
    size_t tag_len;
    size_t expr_off;
    size_t expr_len;
} CCTemplatePiece;

/* Scan the next literal, ${...} / $~tag{...} slot, or ${{...}} verbatim span
 * within [body_s, body_e).  A verbatim span passes its bytes through raw: no
 * escape interpretation, no interpolation; the first `}}` closes it (VERBATIM
 * pieces carry the raw content in expr_off/expr_len).  Advances *pos.
 * Returns 1 on a piece, 0 when *pos >= body_e, -1 on an unterminated
 * interpolation, -3 on an unterminated ${{...}} span. */
int cc_template_next_piece(const char* src, size_t n,
                           size_t body_s, size_t body_e,
                           size_t* pos, CCTemplatePiece* out);

/* Scan a backtick template literal starting at tick_pos (must point at '`').
 * Skips ${...} / $~tag{...} interpolations and ${{...}} verbatim spans while
 * hunting the closing backtick. 0 on success (*tick_end_out = closing tick),
 * -1 unterminated. THE canonical extent scanner — do not re-implement. */
int cc_tpl_scan_literal(const char* src, size_t n, size_t tick_pos, size_t* tick_end_out);

/* True when [body_s, body_e) contains `@emit(` with a backtick template (needs
 * comptime exec after `@comptime for` unrolling or `@comptime if` prune). */
int cc_template_body_needs_emit_exec(const char* src, size_t n,
                                     size_t body_s, size_t body_e);

/* Closer-anchored template dedent (the Swift/C# rule).  When a backtick
 * template's CLOSING tick is alone on its line, the whitespace before it
 * is the margin, and exactly that prefix is stripped from every content
 * line — so a template can sit at the code's indentation and its content
 * still means column 0.  A non-blank content line carrying less than the
 * margin is a compile error naming the line (Java's min-over-lines rule
 * would silently shift the whole block instead — a silent degradation).
 * A closer that shares its line with content means no dedent, so inline
 * one-line templates are untouched; an empty margin is a no-op, which is
 * every template written before this rule existed.
 *
 * Runs over a whole buffer, comment/string-aware via the shared scanner.
 * Returns a fresh dedented buffer (caller frees), NULL when no template
 * needed work (caller keeps the input), (char*)-1 after diagnosing. */
char* cc_tpl_dedent_text(const char* src, size_t n, const char* input_path,
                         size_t* out_len);

#endif /* CC_TEMPLATE_SCAN_H */
