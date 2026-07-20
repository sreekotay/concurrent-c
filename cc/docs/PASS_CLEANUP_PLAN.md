# Compiler-Internals Deep Cycle: span-anchored passes

Position (settled): **text is the INTERFACE** — emitted C, lowered headers,
comptime output all stay text; that stability thesis stands. The problem is
INTERNAL: passes that have AST access still locate their edits by re-scanning
text from (line, col), across three coordinate systems (original file,
pre-expand buffer, parse buffer) related only by `#line` walking. Every pass
bug in the record — UFCS logical/physical collision, closure proto
mid-signature splice, unwrap live-range flattening — is this one defect
expressed in different passes.

Baseline tagged `pre-ast-cleanup` (branch `tag/pre-ast-cleanup`; the remote
refuses tag objects).

## The one missing primitive — now prototyped

`CCEdit` is already byte-offset anchored (edit_buffer.c), a shared
comment/string-aware scanner exists (`util/text_scan.h` CCInertScan), and a
generated→origin span map exists (`diag/source_map.c`). What was missing:
**AST nodes could not name a byte offset** — `CCASTStubNode` carried
best-effort (line, col) only, so every pass re-derived offsets via
`cc__offset_of_line_col_1based` line-walking (12 sites in pass_ufcs alone,
21 in async_ast).

Prototyped in this cycle (patch: `third_party/tcc-patches/0002-cc-ast-byte-offsets.patch`,
submodule working tree; needs a push to sreekotay/tinycc to land for real):

- `cc_tok_off` tracked in the lexer beside `tok_col`: byte offset of the
  current token's start in the top-level in-memory buffer (-1 in nested
  include streams, where buf_ptr is a window).
- `CCASTStubNode.off_start/off_end` (long): token-exact start for every
  node; exclusive end at the first token after the construct (same
  best-effort scope as line_end). Refined further at the sites that stamp
  col_start/col_end.
- CC-side mirror updates were validated locally, then REVERTED on main:
  they must land ATOMICALLY with the tinycc patch or a clean clone gets a
  layout skew between libtcc's node array and the CC-side view (the exact
  segfault class the prototype hit — the stub layout is mirrored in SIX
  places: visitor_ast_common.h, pass_common.h, checker.c,
  pass_closure_calls.c, pass_ufcs.c, visit_codegen.c). The full CC-side
  diff is preserved in main's history (the "open the span-anchored deep
  cycle" commit). Consolidating the six mirrors into ONE header with a
  size assert is part of Phase 1 and removes this hazard class.
- Validation performed: `CC_DEBUG_STUB_NODES=2` sliced node text straight
  from the parse buffer by offset — token-exact starts on every node.

Offsets address the PARSE buffer (which the root retains as
`parse_buffer`); edits target the codegen buffer. Phase 2 closes that gap.

## Inventory (full survey 2026-07-20)

Pipeline: pre-parse text canonicalization (`cc_build_parse_input`: comptime
prep → phase-1 canonical passes → closure markers → cpp expand → relower →
L2) → initial parse → checker → `visit_codegen` with up to 5 gated
REPARSES interleaving batched AST-edit passes (UFCS → closure_calls +
autoblock + await_normalize → closure literals → async machine → final
UFCS sweep).

Per-pass classification (fragility = line→offset rescans + private inline
comment/string state machines instead of CCInertScan):

| pass | class | rescans | inline SMs | notes |
|---|---|---|---|---|
| pass_ufcs | AST→text | 12 | 0 | private `#line` logical/physical dual-mapper + "last match wins" + physical retry; runs 3× |
| async_ast | AST→text | 21 | 3 | largest rescanner; 3,713 lines |
| pass_autoblock | AST→text | 6 | 13 | worst scanner hygiene: zero CCInertScan |
| pass_await_normalize | AST→text | 6 | 0 | |
| pass_closure_literal_ast | AST→text | 4 | 0 | (line,col)-tuple closure identity + `=>` forward-scan recovery; `/*CC_CLO:N*/` markers exist but fallback heuristic still live |
| pass_closure_calls | AST→text | 2 | 0 | |
| checker | AST-only | 0 | 0 | the reference model |
| pass_result_unwrap | text | 0 | 0 | disciplined, but flattens handler bodies to ONE physical line purely to protect downstream offset math |
| pass_defer/err/type_syntax | text | 0 | 4 each | pre-parse surface syntax; irreducibly text (no nodes for type syntax) |
| pass_channel_syntax, unwrap_destroy | text | 0 | 0 | clean |

Duplicated scanner families to consolidate: template scanners
(template_scan.c canonical vs preprocess.c triplicate:
`cc__is_escaped_dollar` / `cc__scan_interp_body` / `cc__is_template_tag_start`
/ `cc__scan_template_literal`), brace/paren/ws matchers re-implemented in
parse.c (`cc__find_matching_{brace,paren}_parse`, `cc__skip_ws*_parse`),
ident predicates (`cc__is_ident_*_parse`), private `#line` walker
(`pass_ufcs.c cc__offset_of_logical_line` vs diag/source_map).

## Phases (each gated: full suite + 5-TU byte-identity + clean bootstrap)

1. **Land the offset substrate — atomically.** Push the TCC patch (needs
   `sreekotay/tinycc` added to session scope, or apply
   `0002-cc-ast-byte-offsets.patch` by hand), bump the submodule, and
   re-apply the CC-side mirror fields IN THE SAME CHANGE (diff preserved
   in main history). Consolidate the six struct mirrors into
   pass_common.h alone with a size assert. Add an offsets smoke
   (CC_DEBUG_STUB_NODES=2 golden).
2. **Buffer-bridging map.** The rewriter chain (build_parse_input) emits
   (derived_off ↔ source_off) anchor pairs at every splice — each rewriter
   knows both offsets at splice time. One module answers
   `parse_off → codegen_off`; retire per-pass `#line` walking
   (pass_ufcs's dual-mapper first).
3. **Migrate fragile-first:** pass_ufcs (drop both mapping heuristics; CALL
   nodes already carry UFCS aux metadata + now exact off_start) →
   pass_closure_literal_ast (finish `/*CC_CLO:N*/` marker identity, delete
   the `=>` recovery scan) → pass_autoblock (13 inline SMs → CCInertScan)
   → async_ast (anchoring only; analysis stays) → pass_result_unwrap
   (drop the one-physical-line flattening).
4. **Scanner consolidation.** One canonical scan module; delete the
   parse.c/preprocess.c duplicates; PASS_INVENTORY rules #1/#2 become
   lint-greppable (CI check: no new `in_block_comment` locals outside the
   shared scanner).
5. **Reparse diet (stretch).** With exact offsets + the bridge map, batched
   stages stop invalidating each other's coordinates; target dropping the
   5-reparse ceiling.

Non-goals: no AST for OUTPUT (text emission stays; the byte-identity gate
depends on it), no full CC AST rewrite — the stub table + offsets is
deliberately the smallest structure that removes the failure class.
