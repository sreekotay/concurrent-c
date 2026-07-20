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

LANDED (atomically, the house way): the offset enhancement rides the
EXISTING rolling-patch mechanism — `third_party/tcc-patches/0001-cc-ext-hooks.patch`
is the single upstream-mirror→tree diff, applied into the submodule
working tree by `scripts/apply_tcc_patches.sh` (idempotent), regenerated
by `scripts/regen_tcc_patches.sh` (root Makefile: tcc-patch-apply /
tcc-patch-regen / tcc-update-check). The submodule now pins the PRISTINE
upstream mirror commit (origin/upstream-mob) rather than a fork branch
with hooks baked in, so:
  - no pushes to the tinycc fork are ever required — the whole CC delta
    lives in this repo's patch file;
  - upstream upgrades follow the documented flow (bump pin → apply →
    adjust → regen → tcc-update-check) and FAIL LOUDLY at apply time if
    upstream moved under a hook;
  - cc/Makefile auto-applies the patch set and builds libtcc on fresh
    clones (guard rule on libtcc.a; .gitmodules ignores the expected
    patched-tree dirt).

What the enhancement carries:
- `cc_tok_off` tracked in the lexer beside `tok_col`: byte offset of the
  current token's start in the top-level in-memory buffer (-1 in nested
  include streams, where buf_ptr is a window).
- `CCASTStubNode.off_start/off_end` (long): token-exact start for every
  node; exclusive end at the first token after the construct (same
  best-effort scope as line_end). Refined at the col_start/col_end sites.
- ONE CC-side mirror (`pass_common.h` CCNodeView) with a
  _Static_assert(sizeof CCASTStubNode == sizeof CCNodeView) DRIFT GUARD
  in tcc_bridge.c — the six hand-copied layouts are gone (PR #83).
- Validation: `CC_DEBUG_STUB_NODES=2` slices node text straight from the
  parse buffer by offset (token-exact starts; unknown streams report -1).
- UFCS CALL nodes stash the MEMBER construct's offset (`cc_last_member_off`,
  captured at the `.`/`->` separator token, matching the line/col
  semantics) so `off_start` names the exact edit anchor, not the call head.
- Macro-replay honesty: tokens replayed from `macro_ptr` (macro expansion,
  `unget_tok`, saved blocks) have NO position in the top-level buffer, so
  the lexer stamps `cc_tok_off = -1` for them and `cc_ast_cur_off`/
  `cc_ast_col_off` refuse to answer while replay is active — unknown beats
  a stale offset pointing at the invocation's tail. Constructs born inside
  macro expansion (e.g. UFCS inside `assert(...)`) carry off_start = -1
  and keep using the text fallback, which is the pre-existing contract
  (ufcs_macro_arg_smoke exists to pin it).
- Corpus-wide SELF-CHECK (fail loudly): `cc__collect_ufcs_edits` verifies
  every UFCS node's method name actually sits at `off_start` in the
  retained `root->parse_buffer` (separator-anchored). Warns by default;
  `CC_STRICT_OFFSETS=1` makes it fatal — the full suite passes strict.

Offsets address the PARSE buffer (which the root retains as
`parse_buffer`); edits target the codegen buffer. Phase 2 closes that gap.

Known pre-existing bug (surfaced by this work, NOT caused by it — main
reproduces byte-for-byte): a function-pointer MEMBER call in user code
(`c->cb(4)` where `cb` is a struct field, with the prelude included)
sends the checker's `walk` into unbounded recursion → stack overflow.
The UFCS probe classifies any `expr->name(` as a UFCS candidate and
drops the receiver; somewhere downstream the stub-node parent chain
cycles. Fix belongs to the checker/UFCS-classification cleanup in phase 3.

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

1. **Offset substrate: LANDED** (rolling patch + upstream pin, above),
   including the UFCS member-offset stash, macro-replay -1 semantics,
   parse-buffer retention on the reparse root, and the corpus-wide
   strict self-check. Mirror consolidation done (PR #83). Remaining:
   an offsets golden smoke (CI can run the suite with
   `CC_STRICT_OFFSETS=1` — strict is green today).
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
