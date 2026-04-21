# Refactor Plan — AST truth for lowering passes

**Status:** draft / proposal. No code changes yet.

## 1. Why we keep getting bitten

Every bug in the F1–F9 series (see `docs/known-bugs/redis_idiomatic_async.md`)
had the same shape, and the bugs before that mostly did too:

1. A lowering pass wants to find a construct in the user's source
   (an `@async` fn, a `!>(e) { ... }` binder, a `ch:<<`, a `create{...}`
   form, a `@match` arm, …).
2. It reads the current `src_ufcs` text buffer plus `root->nodes`
   (the TCC stub-node side table) and does hand-rolled,
   comment/string/bracket-aware scanning to locate the construct.
3. It splices in new text.
4. The next pass does a full reparse — positions have drifted.
5. Cross-pass invariants live in **identifier prefixes** (`__cc_pu_`,
   `__cc_ab_`, `__f->`) and **comment markers**.  Miss one, and the
   error surfaces in the lowered C as something like
   `CCIoError __f->e = ...;`.

The AST we have today is a flat stub side-table from a patched TCC;
`cc/src/ast/ast.h` says so directly:

> Transitional note: the current pipeline uses a patched TCC to return
> a stub-node side table … and the visitor consumes `root->nodes` for
> UFCS and arena lowering.  Keep those fields intact until the CC AST
> `items` array is populated.

So every pass is forced to re-discover structure in text, because the
only structured view we have (`root->nodes`) is flat, positional, and
invalidated by the previous pass's rewrite.

Size of the problem (counted on `HEAD` today):

| file                                 | LOC   | text-scan helper calls |
|--------------------------------------|-------|------------------------|
| `visit_codegen.c`                    | 4 576 |  38 + 39 reparse calls |
| `pass_closure_literal_ast.c`         | 3 714 | 142                    |
| `async_ast.c`                        | 3 451 |  28                    |
| `pass_result_unwrap.c`               | 2 473 |  79                    |
| `ufcs.c`                             | 2 278 |   8                    |
| `pass_autoblock.c`                   | 2 011 | 167                    |
| `pass_channel_syntax.c`              | 1 978 |   6                    |
| `pass_err_syntax.c`                  | 1 357 |  85                    |
| `pass_defer_syntax.c`                | 1 312 |  97                    |

## 2. Goal

Establish a **single structured representation** that every lowering
pass reads *and* edits, so that:

- Passes locate constructs by **node identity**, not by text scanning.
- Cross-pass contracts are enforced by **types/fields**, not by
  identifier prefixes or comments.
- A rewrite by pass *N* does not invalidate pass *N+1*'s positional
  assumptions.
- Reparses become an optimisation, not a correctness requirement.

### Non-goals (for this refactor)

- Replacing TCC as the parser.  We keep the patched TCC stub-node
  side-table as the source of truth for *parsing*; we build a thin CC
  IR on top of it.
- Redesigning the surface language.
- Changing how we emit C code at the bottom of the pipeline.
- A big-bang rewrite.  Every step must land with tests green.

## 3. Target architecture

### 3.1 A thin, mutable CC-IR layered on the TCC stub

Populate the existing `CCASTNode` / `CCASTRoot` types in
`cc/src/ast/ast.h` (today unused — everyone reads `root->nodes`).
The IR carries:

- `kind` + `span` + typed children (union by kind, not `children[]`).
- A back-pointer to the originating stub node (for parser diagnostics).
- A **stable id** per node (`cc_ast_node_id_t`, 32-bit monotonic).
- An **edit list** (see §3.2) instead of a `const char* text` field.

A single initial builder, `cc_ast_build_from_stub(root) -> CCASTRoot*`,
walks `root->nodes` once and produces the structured tree.  This
runs once after preprocess.

### 3.2 Edits are structured, text is derived

Passes do not call `cc__append_fmt` against `src_ufcs`.  Instead they
emit one of:

- `cc_ir_replace(node_id, new_subtree)`
- `cc_ir_wrap(node_id, wrapper_kind, {lhs?, rhs?})`
- `cc_ir_insert_before(node_id, new_node)` / `insert_after`
- `cc_ir_attach_closure(fn_id, closure_def)`
- `cc_ir_mark_attr(node_id, attr_name, attr_value)` (e.g. `@async`,
  `@noinline`, `frame_lifted=true`)

All three current "types of rewrite" reduce to these:

- *token-level* splices (most of `pass_result_unwrap`, `pass_err_syntax`).
- *statement-list* rewrites (`async_ast` state-machine, `pass_defer`).
- *function-level* wrappers (`pass_closure_literal_ast`,
  `pass_autoblock`).

Text is **re-emitted from the IR** between phase boundaries (see
§3.3), not mutated in place.  That kills the "line numbers drifted"
class of bug at the root.

### 3.3 Fixed pipeline with explicit phase boundaries

Today `visit_codegen.c` is an ad-hoc orchestrator of ~20 rewrite
steps with 7+ reparses interleaved.  Replace that with 3 explicit
phases:

1. **Parse + build IR** — once, from preprocessed source.
2. **Desugar phases** (IR → IR), in a declared order.  Each phase
   only reads node kinds its predecessors promise to leave intact.
3. **Emit** — IR → C text, once, at the end.

Reparse of lowered C into TCC remains — but only at phase
boundaries where the IR and the text genuinely need to be
re-synchronised (e.g. after `@match` expansion brings in new type
information).  Those boundaries are the exception, and each has a
named phase-id.

### 3.4 Pass contract

Every pass declares:

```c
typedef struct CCIrPass {
    const char* name;            /* e.g. "result_unwrap" */
    CCAstKindMask reads;         /* kinds the pass may inspect */
    CCAstKindMask writes;        /* kinds the pass may create/edit */
    CCAstKindMask promises;      /* kinds that must not exist on exit */
    int (*run)(CCIrCtx* ctx);
} CCIrPass;
```

A debug build asserts `writes ⊆ reads ∪ writes-declared` and that no
`promises` kind survives the pass.  This replaces today's invariant
that lives only in `__cc_pu_` and `__cc_ab_` prefixes.

## 4. Migration plan (staged)

Principle: each step lands independently, tests stay green, and the
next step is easier.  Total effort is large; the first two phases
already buy most of the debuggability win.

### Phase 0 — hygiene (already mostly done)

- Shared text primitives in `cc/src/util/text.h`
  (`cc_find_ident_top_level`, `cc_find_substr_top_level`, etc.).
- Policy: no new `strstr`/`strchr` on user-source text in lowering
  passes (enforced by review; see §5 for automation).

### Phase 1 — IR skeleton + single pass ported

1. Flesh out `CCASTNode` into a real tagged union keyed by
   `CCASTKind`.  Add `cc_ast_node_id_t`, spans that refer to both
   pre- and post-lowering sources.
2. Write `cc_ast_build_from_stub()`.  Today this is effectively
   one-to-one with `root->nodes` plus a bit of grouping.
3. Pick the smallest pass with the highest bug density:
   **`pass_result_unwrap`** (already touched in F4/F8/F9).
   Port it so it reads the IR, not `src_ufcs`, and emits
   structured edits.  Keep the text-emitter for `!>`/`?>` as a
   last-mile step inside that pass.
4. Emit benchmarks: time per phase, node count, edit count.

Exit criteria: full test suite green; `pass_result_unwrap` no longer
owns any `strstr`/`cc_find_*` call against user source; the F4/F8/F9
smoke tests still pass; no new reparse was added.

### Phase 2 — drain the "frame-lift" family

Port `async_ast` and `pass_closure_literal_ast` next.  These are the
two passes whose invariants today live in identifier-prefix
conventions (`__cc_pu_`, `__cc_ab_`, `__f->`).  Replace those
conventions with an explicit `frame_lifted` bit on local-decl IR
nodes, and a `closure_capture` child list on closure nodes.

Exit criteria: `__cc_pu_` and `__cc_ab_` prefix checks in
`async_ast` become `node->frame_lifted == false` checks; the
identifier strings themselves keep working (we can still emit them)
but no pass consults the string.

### Phase 3 — statement-list rewrites

Port `pass_defer_syntax`, `pass_err_syntax`, `pass_channel_syntax`,
`pass_match_syntax`.  These are natural fits for the
`insert_before`/`insert_after` IR primitives because they already
operate on statement boundaries.

### Phase 4 — function-level wrappers

Port `pass_autoblock`, `pass_nursery_spawn_ast`,
`pass_with_deadline_syntax`.  These lift whole call expressions into
closures; in the IR this is `cc_ir_wrap(call_id,
CC_IR_AUTOBLOCK_CLOSURE, …)`.

### Phase 5 — collapse the reparse dance

With every pass emitting IR edits, most of the 7+ `cc__reparse_*`
calls in `visit_codegen.c` become unnecessary.  Keep only the
genuine phase-boundary reparses (currently probably two:
post-`@match`, post-UFCS chain resolution).

### Phase 6 — retire `root->nodes` as a pass input

At this point only the initial builder reads the stub table.  The
stub table becomes an implementation detail of
`cc_ast_build_from_stub()`, and `visit_codegen.c` shrinks to an
orchestrator of declared `CCIrPass`es.

## 5. Test & safety strategy

- **Golden-IR tests**: for each ported pass, dump the IR before and
  after as a stable textual form (kinds + spans + attrs) and diff.
  Much cheaper to review than lowered-C diffs.
- **Differential testing during migration**: every ported pass also
  runs the old text-rewrite pass under `CC_VERIFY_IR=1` and asserts
  the emitted C matches.  Drop the verifier once the pass is stable.
- **Invariant assertions**: in debug builds, after each pass assert
  `writes` / `promises` masks hold.
- **Lint rule**: add a CI grep that forbids `strstr(`/`strchr(` in
  `cc/src/visitor/*.c` files except in an allow-listed set, to make
  regressions on the Phase-0 policy mechanical.
- Existing smoke suite (tests under `tests/`) must stay green at
  every phase boundary.  `tools/cc_test` is the gate.

## 6. Risks and mitigations

| risk                                        | mitigation                                  |
|---------------------------------------------|---------------------------------------------|
| IR ends up a second source of truth that drifts from TCC | Keep stub-node back-pointer; assert span equality for untouched nodes |
| Phase-1 pass port takes longer than budget  | Pick smallest bug-dense pass first; cut scope to just `!>` statement-form if needed |
| Performance regression from IR allocation   | Arena-allocate IR per TU; measure before/after |
| Divergence between differential verifier and new emitter | Ship verifier off by default; only flip on in CI for the ported pass |
| Large merge conflicts with concurrent work  | Land each phase as its own small PR; feature-flag new passes behind `CC_USE_IR=1` until stable |

## 7. What this buys us (measured against the recent bug tail)

- **F1, F2** (frame-lifting stripping type): would be a single
  `node->frame_lifted` check — no text scan.
- **F5, F6** (comment/string-aware scans): don't exist in an IR
  world — the IR has no comments or strings at the boundaries we
  scan today.
- **F7** (stmt-expr initializer in `case`): becomes an emitter
  concern; the IR can lower it to a fresh block without needing the
  surface form to parse.
- **F8** (binder error-type plumbing): the binder-type lives on the
  IR node; no "thread LHS type through by string manipulation".
- **F9** (binder frame-lifted as local): the binder IR node has
  `scope = expr_local, frame_lifted = false` — `async_ast` can't
  mistake it.  The `__cc_pu_` prefix mangling stops being a
  correctness mechanism and becomes just a naming convention for
  debug dumps.

## 8. Decisions locked in

1. **`CCASTKind` grows per phase.** Only the kinds the current phase
   needs land in the enum when that phase ports; we learn the shape
   as we go rather than guessing the full taxonomy up front.
2. **Per-TU arena allocation** for IR nodes (single bulk free at end
   of TU).  Refcounting is deferred until/unless a long-lived use
   case (LSP, incremental rebuilds) actually materialises.
3. **Split `visit_codegen.c` at phase 5**, not before.  Until the
   reparse count drops the orchestrator stays monolithic, because
   splitting it while reparses are still interleaved just spreads
   the mess across more files.
4. **Phase 1 starts with `pass_result_unwrap`.**  Highest bug
   density (F4/F8/F9), moderate size (2 473 LOC, 79 text-scan
   calls), and it already has the best smoke-test coverage in the
   tree — so the differential verifier has something to check
   against from day one.
5. `@match` exhaustiveness checking lands on the IR when
   `pass_match_syntax` ports in phase 3 — it needs arm structure
   that the lowered C doesn't carry.
6. Phase-boundary reparses reuse `cc_ast_build_from_stub()` (same
   builder, same IR type) rather than consuming raw stub nodes
   ad-hoc.

## 9. Rough sequencing

Phase 0: done.

- Phase 1 (IR skeleton + port `pass_result_unwrap`): ~1 week.
- Phase 2 (async + closure-literal frame-lift): ~1–2 weeks.
- Phase 3 (stmt-list passes): ~1 week.
- Phase 4 (fn-wrapper passes): ~1 week.
- Phase 5 (reparse collapse): ~2–3 days.
- Phase 6 (retire stub-table reads): cleanup, ~2 days.

Total ~5–6 weeks of focused work, landed as ~10 PRs, each with the
full test suite green.

---

Comments / revisions welcome inline before we start phase 1.
