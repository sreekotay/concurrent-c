/* Concurrent C intermediate representation (CC-IR).
 *
 * This is the "AST truth" layer that lowering passes will read and edit,
 * replacing the text-in / text-out pattern documented in
 * docs/refactor-ast-truth.md.
 *
 * Phase-1 scope (see that doc): we populate the IR with just enough kinds
 * to port `pass_result_unwrap` off raw text scanning.  Every other
 * construct is carried as a `CC_IR_OPAQUE_TEXT` passthrough.  As more
 * passes port, the enum grows — per the phase-1 decision we grow kinds
 * lazily, not all at once.
 *
 * Allocation model: per-TU arena.  All nodes belong to one arena; the
 * whole arena is freed in a single `cc_ir_arena_destroy()` call at end
 * of TU.  IR nodes do not own non-arena memory.
 *
 * Stable ids: every node has a monotonic `cc_ir_node_id_t` unique within
 * its arena.  Passes use node ids to cross-reference between edit
 * lists and the tree itself.
 */
#ifndef CC_IR_IR_H
#define CC_IR_IR_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- */
/* Arena                                                                */
/* ------------------------------------------------------------------- */

typedef struct CCIrArena CCIrArena;

/* Create a fresh arena.  Returns NULL on allocation failure. */
CCIrArena* cc_ir_arena_create(void);

/* Free the arena and every node/string allocated through it. */
void cc_ir_arena_destroy(CCIrArena* arena);

/* Allocate `n` zero-initialized bytes from the arena.  Returns NULL on
 * allocation failure.  Lifetime is tied to the arena. */
void* cc_ir_alloc(CCIrArena* arena, size_t n);

/* Duplicate `src[0..n)` into arena memory as a NUL-terminated buffer.
 * Returns NULL on allocation failure (or if `src` is NULL and `n > 0`). */
char* cc_ir_strndup(CCIrArena* arena, const char* src, size_t n);

/* Number of bytes currently live in the arena (for metrics). */
size_t cc_ir_arena_bytes(const CCIrArena* arena);

/* ------------------------------------------------------------------- */
/* Node kinds                                                           */
/* ------------------------------------------------------------------- */

/* Grown per phase — see docs/refactor-ast-truth.md §8 decision 1. */
typedef enum {
    CC_IR_INVALID = 0,

    /* The whole TU.  Root of the tree. */
    CC_IR_FILE,

    /* Passthrough: a slice of source text we haven't modelled yet.
     * During phase 1 most of the file is a single CC_IR_OPAQUE_TEXT
     * child of CC_IR_FILE, with the `!>`/`?>` forms carved out as
     * typed nodes around it. */
    CC_IR_OPAQUE_TEXT,

    /* `EXPR !>` / `EXPR !> BODY` / `EXPR !>(e) BODY`
     * Used in both statement and expression position; the parent kind
     * disambiguates.  See fields under `CCIrUnwrap` below. */
    CC_IR_UNWRAP_BANG,

    /* `EXPR ?> DEFAULT`, `EXPR ?>(e) RHS`, `EXPR ?> DIVERGENT` etc. */
    CC_IR_UNWRAP_Q,
} CCIrKind;

typedef uint32_t cc_ir_node_id_t;

/* ------------------------------------------------------------------- */
/* Spans                                                                */
/* ------------------------------------------------------------------- */

/* Byte offsets into the *current* source buffer for this phase.  When
 * a phase boundary re-emits text, spans are re-derived from the IR
 * itself, so drift is not a concern.
 *
 * `orig_line` / `orig_col` carry user-visible location for diagnostics
 * and #line directives; they're preserved across rewrites. */
typedef struct {
    size_t byte_start;   /* inclusive */
    size_t byte_end;     /* exclusive */
    int    orig_line;    /* 1-based; 0 if unknown */
    int    orig_col;     /* 1-based; 0 if unknown */
} CCIrSpan;

/* ------------------------------------------------------------------- */
/* Per-kind payloads                                                    */
/* ------------------------------------------------------------------- */

struct CCIrNode;

typedef struct {
    /* Contents of the slice, NUL-terminated and arena-owned.  `len` is
     * the strlen (same as byte_end - byte_start in a fresh build; may
     * drift if a pass edits the text portion of a mixed tree — always
     * trust `len`, not the span). */
    const char* text;
    size_t      len;
} CCIrOpaqueText;

/* Data carried by both CC_IR_UNWRAP_BANG and CC_IR_UNWRAP_Q.  The
 * parent kind disambiguates how the handler body is interpreted. */
typedef struct {
    /* The LHS expression being unwrapped, as raw text for now.  Will
     * become a proper CCIrNode* in step 1.3+. */
    const char* lhs_text;
    size_t      lhs_len;

    /* Optional binder name introduced by `!>(e) BODY` / `?>(e) RHS`.
     * NULL if the binder was omitted.  Owned by the arena. */
    const char* binder_name;

    /* Mangled name we emit into C (e.g. `__cc_pu_bind_42_e`).  Filled
     * in by the unwrap pass; NULL until then.  The `__cc_pu_` prefix
     * is what tells async_ast not to frame-lift this local today; once
     * async_ast ports (phase 2) this becomes a `frame_lifted=false`
     * bit on a dedicated let-node and the mangled-name string becomes
     * cosmetic. */
    const char* binder_mangled;

    /* Handler body or RHS expression, as raw text.  Same text-for-now
     * rationale as lhs_text. */
    const char* body_text;
    size_t      body_len;

    /* Declared error arm of the result type, e.g. "CCIoError" or
     * "__CCGenericError".  Threaded from the LHS type by the pass. */
    const char* err_type;

    /* Declared value arm of the result type, e.g. "int" or "bool". */
    const char* ok_type;

    /* True if the body provably diverges (return/break/continue/throw),
     * set by the analysis portion of the unwrap pass. */
    unsigned diverges : 1;

    /* True if this node was at statement position (declarator form
     * `TYPE x = EXPR !>(e) BODY;` or bare `EXPR !>;`), false if it was
     * at expression position. */
    unsigned stmt_position : 1;
} CCIrUnwrap;

/* ------------------------------------------------------------------- */
/* Node                                                                 */
/* ------------------------------------------------------------------- */

typedef struct CCIrNode {
    cc_ir_node_id_t id;
    CCIrKind        kind;
    CCIrSpan        span;

    /* Back-pointer to the originating TCC stub node, if any.  NULL for
     * synthetic nodes created by passes. */
    const void*     stub;

    /* Children array.  Ownership: arena.  Meaning of children is kind-
     * dependent; the common case for this phase is:
     *   CC_IR_FILE           -> ordered list of top-level chunks
     *                           (OPAQUE_TEXT + UNWRAP_* interleaved).
     *   CC_IR_UNWRAP_*       -> no children yet (lhs/body carried as
     *                           text in the payload).  Steps 1.3/1.4
     *                           will upgrade these to child nodes. */
    struct CCIrNode** children;
    size_t            children_len;
    size_t            children_cap;

    /* Per-kind payload.  Exactly one field is meaningful per kind. */
    union {
        CCIrOpaqueText opaque;
        CCIrUnwrap     unwrap;
    } as;
} CCIrNode;

/* Allocate a new node of the given kind from `arena`.  Children array
 * is empty; payload is zero-initialized.  Returns NULL on alloc failure. */
CCIrNode* cc_ir_node_new(CCIrArena* arena, CCIrKind kind, CCIrSpan span);

/* Append `child` to `parent->children`, growing the array as needed.
 * Returns 0 on success, -1 on alloc failure. */
int cc_ir_node_append_child(CCIrArena* arena,
                            CCIrNode* parent,
                            CCIrNode* child);

/* ------------------------------------------------------------------- */
/* Build / emit                                                         */
/* ------------------------------------------------------------------- */

struct CCASTRoot;

/* Build an IR tree from the current TU source plus the TCC stub-node
 * side-table carried in `root`.  The stub table is consulted for kinds
 * we already model; everything else becomes `CC_IR_OPAQUE_TEXT`.
 *
 * `input_path` is borrowed (used for diagnostics only).
 * Returns NULL on allocation failure or if arguments are invalid.
 * Caller owns `arena` and must outlive the returned root. */
CCIrNode* cc_ir_build_from_stub(CCIrArena* arena,
                                const struct CCASTRoot* root,
                                const char* src,
                                size_t src_len,
                                const char* input_path);

/* Emit the IR tree back to a flat text buffer.  On success returns 0
 * and sets `*out_src` (malloc'd, NUL-terminated) and `*out_len`.  The
 * caller owns `*out_src` (it's plain malloc, not arena-allocated —
 * the pipeline needs lifetime independent of the arena). */
int cc_ir_emit_text(const CCIrNode* root,
                    char** out_src,
                    size_t* out_len);

/* Debug dump of the IR tree to `fp` (kinds + spans + first N bytes of
 * text).  Used by `CC_IR_DUMP` env var and by golden tests. */
void cc_ir_dump(const CCIrNode* root, FILE* fp);

#ifdef __cplusplus
}
#endif

#endif /* CC_IR_IR_H */
