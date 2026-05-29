#ifndef CC_CLOSURE_MARKERS_H
#define CC_CLOSURE_MARKERS_H

#include <stddef.h>

/* Stable closure-ID markers — foundation pass.
 *
 * Walks a source buffer and injects /\*CC_CLO:N*\/ markers
 * immediately before every closure literal in source order
 * (numbered 1, 2, 3, ...).  Markers are plain C block comments
 * that TCC (and the host compiler) strip during parse — they
 * have no semantic effect on the compiled output.
 *
 * The motivation is to give downstream closure-walking code a
 * stable, in-text anchor for each closure literal that doesn't
 * depend on AST `(line, col)` coordinates surviving every text
 * transform in the pipeline (CPP expansion, prelude prepending,
 * reparse sanitization, etc.) and doesn't depend on the
 * heuristic "find next `=>` skipping inert regions" recovery
 * scan.  Both have been sources of subtle bugs:
 *
 *   - The AST `(line, col)` drift after `#include <ccc/...>`
 *     directives leaving TCC's line counter offset relative to
 *     the visitor's working buffer.
 *
 *   - The `=>` text scan matching arrows inside comments (fixed
 *     reactively in commit f5b1d2a-ish via the comment-skip in
 *     `cc__find_next_arrow_skipping_inert` — the syscall_kidnap
 *     reproducer).
 *
 * With markers in place, the recovery path can resolve a
 * closure by its source-order index (which the AST also
 * preserves via left-to-right parse) directly to the offset
 * of its marker, with zero ambiguity.
 *
 * INVARIANT — line-number preservation: markers are inserted
 * IN-LINE on the same line as the closure they mark.  No
 * newlines are added, so all downstream `(line, col)` numbers
 * stay valid.  Column numbers within a marked line drift by
 * the marker's width (typically 14-16 bytes).
 *
 * Identification of closure literals: any real-code `=>` token
 * is a CC closure arrow (standard C has no `=>` operator).
 * From the arrow we walk backwards over optional whitespace
 * and either a `(...)` parameter list or an identifier to find
 * the closure's start.  Inert regions (strings, char literals,
 * comments, preprocessor-directive bodies) are skipped via the
 * shared `CCInertScan` from `util/text_scan.h`.
 *
 * Scope of this commit (foundation only): markers are injected
 * and harmless.  Consumers (the closure-descriptor resolution
 * in pass_closure_literal_ast.c) still use the existing
 * heuristic stack.  A follow-up commit switches the recovery
 * path to consume markers, and a later one removes the
 * heuristic entirely once we trust the marker path on the
 * full smoke suite.
 *
 * (d.2) IMPLEMENTATION NOTE — DUAL-INJECTION DOESN'T WORK:
 *
 * The obvious approach for letting the closure pass consume
 * markers is to *also* inject markers into `src_all` (the
 * visitor's working buffer in visit_codegen.c) so column-based
 * AST resolution against src_all lands at the same logical
 * position as in `root->parse_buffer` (which already has
 * markers from `cc_build_parse_input`).  This was tried on
 * 2026-05-27 and broke 8 unrelated smokes (closure_unsafe_ref,
 * result_custom_types, closure_field_capture, etc.) — many
 * text passes that walk src_all/src_ufcs are byte-position
 * sensitive in ways the inline marker bytes disrupt, even
 * though CCInertScan skips the marker comments.
 *
 * The proper consumer migration needs ONE of:
 *
 *   (A) A separate marker offset table, stored on `CCASTRoot`
 *       or in the visitor ctx, populated from `root->parse_buffer`
 *       at AST-build time and read by pass_closure_literal_ast.c
 *       — no src_ufcs modification.  Cleanest but requires
 *       widening the AST root + plumbing through every
 *       reparse call site.
 *
 *   (B) Inject markers locally just before the closure pass
 *       and strip them after, in a try/finally pattern around
 *       `cc__rewrite_closure_literals_with_nodes`.  Smaller
 *       blast radius but introduces a stripping pass and only
 *       protects the closure-literal pass — other consumers
 *       (cc__infer_closure_end_off, etc.) still see no markers.
 *
 *   (C) Map markers from parse_buffer offsets to src_ufcs
 *       offsets by walking both buffers line-by-line and
 *       converting marker offsets via the line-table mapping.
 *       Feasible (lines match between buffers because markers
 *       and CPP-expanded headers preserve line numbers via
 *       #line directives), but more code than (A).
 *
 * Recommendation: do (A).  The plumbing is one struct field
 * + a malloc'd `(start_off, id)[]` table + reading it from the
 * pass.  Defer until there's a concrete need beyond the
 * "principled architecture" win — the current heuristic
 * stack passes 458/458 smokes.
 *
 * Returns a newly malloc'd buffer with markers injected
 * (caller frees), NULL when no closures were found (zero-cost
 * no-op path for source files without any `=>`), or NULL with
 * `*out_len` unchanged on alloc failure.
 *
 * Diagnostic env: `CC_DEBUG_CLOSURE_MARKERS=1` prints
 * `[cc:clo-markers] path: injected N markers` per TU. */
char* cc_inject_closure_markers(const char* src, size_t src_len, size_t* out_len);

#endif /* CC_CLOSURE_MARKERS_H */
