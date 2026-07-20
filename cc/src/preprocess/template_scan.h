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
 * comptime exec after `@comptime for` unrolling). */
int cc_template_body_needs_emit_exec(const char* src, size_t n,
                                     size_t body_s, size_t body_e);

#endif /* CC_TEMPLATE_SCAN_H */
