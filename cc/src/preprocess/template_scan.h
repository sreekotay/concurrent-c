#ifndef CC_TEMPLATE_SCAN_H
#define CC_TEMPLATE_SCAN_H

#include <stddef.h>

typedef enum CCTemplatePieceKind {
    CC_TPL_PIECE_LITERAL = 0,
    CC_TPL_PIECE_SLOT = 1,
    CC_TPL_PIECE_TAGGED_SLOT = 2,
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

/* Scan the next literal or ${...} / $~tag{...} piece within [body_s, body_e).
 * Advances *pos.  Returns 1 on a piece, 0 when *pos >= body_e, -1 on error. */
int cc_template_next_piece(const char* src, size_t n,
                           size_t body_s, size_t body_e,
                           size_t* pos, CCTemplatePiece* out);

/* True when [body_s, body_e) contains `@emit(` with a backtick template (needs
 * comptime exec after `@comptime for` unrolling). */
int cc_template_body_needs_emit_exec(const char* src, size_t n,
                                     size_t body_s, size_t body_e);

#endif /* CC_TEMPLATE_SCAN_H */
