# Concurrent-C Compiler Architecture

**Status:** Authoritative architecture decision record (ADR).
**Last updated:** 2026-05-28 (post fossil audit — see §6).
**Audience:** anyone tempted to globally restructure the compiler. Read this **before** proposing a redesign.

This document records the *why* of the compiler pipeline shape. The shape itself is documented in [`PIPELINE.md`](../src/visitor/PIPELINE.md) (call-site map) and [`PASS_INVENTORY.md`](../src/visitor/PASS_INVENTORY.md) (per-pass catalog). Operational status — what shipped, what's next, what's deferred — lives in [`COMPILER_CLEANUP_STATUS.md`](COMPILER_CLEANUP_STATUS.md).

---

## TL;DR

The pipeline shape is **text preprocess → initial TCC parse → AST lowering (two-stage batched) → middle reparses → async state machine → emit C → invoke host C compiler**.

That shape is determined by four hard constraints (§2). Every plausible global redesign converges back to it.

Three architectural layers (§3):

| Layer | Role | Reparses |
|-------|------|----------|
| **Front-end** | text preprocess (16 P-passes) → initial TCC parse → checker | 0 + 1 initial |
| **Middle-end** | type-driven AST lowering, batched into per-span `CCEditBuffer` applies | 2 (currently) + 1 (closure literal / statement lowering) |
| **Back-end** | async SM + final UFCS sweep + emit + invoke host C compiler | 1 (async SM) + 1 (final UFCS) |

Total: **6 reparses max per TU**, most conditional. Down from 9 pre-flip.

Six ADRs (§4) document the load-bearing decisions:

1. Use TCC's stub AST instead of writing a C parser.
2. Text-rewrite CC surface syntax before any parser sees it.
3. Per-span `CCEditBuffer` edits, not whole-file rewrites.
4. Two-stage Phase 3 batching.
5. `#line` directives + `CCSourceMap` for diagnostics.
6. Patch TCC's `pp_line` to stop swallowing negative-delta `#line` directives.

Five non-goals (§5) document architectural moves that have been **considered and rejected**. Each one would re-discover a constraint or sacrifice an investment. Don't undertake without re-reading this doc.

---

## 1. Reader's map

| If you want to ... | Read |
|--------------------|------|
| Understand WHY the pipeline looks this way | §2 (constraints) + §3 (layers) + §4 (ADRs) |
| Understand WHAT each pass does | [`PASS_INVENTORY.md`](../src/visitor/PASS_INVENTORY.md) |
| See WHERE the reparses happen | [`PIPELINE.md`](../src/visitor/PIPELINE.md) |
| See WHAT shipped and WHAT's next | [`COMPILER_CLEANUP_STATUS.md`](COMPILER_CLEANUP_STATUS.md) |
| Propose a global redesign | §5 (non-goals) first, then §6 (what redesign would help) |
| Add a new pass or scanner | the invariants in [`PASS_INVENTORY.md`](../src/visitor/PASS_INVENTORY.md#invariants-for-new-text-scanners-must-follow) |
| Debug a Phase 3 ordering bug | §4 ADR-004 (two-stage rationale) |
| Debug a `#line` / diagnostic origin bug | §4 ADR-005 + ADR-006 |

---

## 2. The four constraints

These are the *non-negotiable* properties of the problem domain. They are not engineering choices — they are facts about CC and TCC that the pipeline must accommodate.

### C1. We do not own a C parser

We use TCC's stub-AST front-end (`CONFIG_CC_EXT`, see [`cc/README.md`](../README.md)) for parsing C. The stub AST gives us function decls, parameter lists, call nodes, span info, and limited declarator info — enough to drive type-aware lowering.

Writing a C parser is a ~year of work to reach parity:

- typedef-name vs identifier disambiguation (the lexer hack)
- declarator grammar (including K&R, the array-of-functions decay rule, `const`/`volatile` placement)
- `__attribute__((...))` parsing
- preprocessor directive handling (`#line`, `#pragma`, include search paths)
- the long tail of GCC/clang dialect support (statement expressions, `__builtin_*`, vector types, ...)

None of that work has any *CC-specific* upside. It would yield a parser that does what TCC already does, more slowly, with new bugs.

**Therefore:** TCC is load-bearing. The pipeline plans around it. The cost of a TCC upgrade (~2 days of patch reapply + test) is the price of admission.

### C2. CC surface syntax cannot be tokenized by a C lexer

Survey of CC tokens that TCC's lexer rejects or misreads:

| Token | C lexer interpretation | Pass that handles it |
|-------|------------------------|----------------------|
| `T[:]` | `T` `[` `:` `]` — illegal in a declarator | P3 `cc__rewrite_slice_types` |
| `T?` | `T` `?` `...` — start of ternary | P6 (retired) / diagnostic |
| `T!>(E)` | `T` `!` `>` `(` ... | P8 `cc__rewrite_result_types` |
| `int[~4 >]` | `int` `[` `~` `4` `>` `]` — illegal | P4 `cc__rewrite_chan_handle_types` |
| `Vec<T>` | `Vec` `<` `T` `>` — comparison chain | P5 `cc_rewrite_generic_containers` |
| `@match` | `@` (illegal in C) | P2 `cc__rewrite_match_syntax` |
| `with_deadline(ms) { ... }` | identifier + paren + block (no binding) | P1 `cc__rewrite_with_deadline_syntax` |
| `() => {...}` | parens + `=` + `>` + brace | P11 closure literal lift |

There is no fix for this that doesn't involve a custom lexer. Custom lexer + TCC parser is incoherent (the lexer would have to either re-tokenize TCC's output or pre-tokenize TCC's input — both require a parallel C lexer).

**Therefore:** a text-preprocess layer that rewrites CC surface syntax to C-shaped tokens is unavoidable. Today: 16 passes in [`cc/src/preprocess/preprocess.c`](../src/preprocess/preprocess.c) (~8.8k lines). The set will shrink as M7's CPP-expand work matures, but cannot go to zero.

### C3. Two of four AST-driven passes need type information

The Phase-3 lowering passes split by what information they consume:

| Pass | Information needed | Text-doable? |
|------|--------------------|--------------|
| UFCS (`pass_ufcs.c`) | type of receiver expression — to resolve `obj.method(args)` into `Type_method(obj, args)` | **NO** |
| `closure_calls` (`pass_closure_calls.c`) | type of callee — to detect `CCClosure1` / `CCClosure2` | **NO** |
| `autoblock` (`pass_autoblock.c`) | `@async` / `@blocking` annotations + call-graph proximity | YES in principle, but the existing AST walk is simpler |
| `await_normalize` (`pass_await_normalize.c`) | syntactic — find `await EXPR`, hoist | YES in principle |

UFCS and closure_calls genuinely require type info that only exists after TCC parses the source. Type info propagates through TCC's declarator grammar (typedefs, struct member access, function-pointer types) in a way that no text scanner can replicate.

**Therefore:** a typed AST visitor layer running *after* the initial TCC parse is unavoidable. The other two passes ride along because they share the same per-span `CCEditBuffer` infrastructure (see ADR-003).

### C4. UFCS produces new call sites

UFCS lowering rewrites `obj.method(args)` into `Type_method(obj, args)`. The output is a perfectly ordinary C function call.

The downstream Phase-3 passes (`closure_calls`, `autoblock`, `await_normalize`) are AST-driven. They iterate CALL nodes. They need the post-UFCS form to:

- match closure-typed CALLs (closure_calls)
- decide whether a CALL is blocking (autoblock)
- find call sites inside `await EXPR` (await_normalize)

If those passes walk the pre-UFCS AST, they miss the call sites that UFCS just produced.

This was proven the hard way (2026-05-28): a single-stage batched experiment (all four collectors into one `CCEditBuffer` apply) failed on `tests/async_await_channel_auto_destroy_smoke.ccs` because `await chan.send(x)` triggered both UFCS and await_normalize edits over the same byte range. The fix was the two-stage split (UFCS, then reparse barrier, then the other three).

**Therefore:** a reparse barrier between UFCS and the other Phase-3 passes is required. No purely AST-side mutation trick avoids it without rebuilding TCC's call-resolution machinery in our visitor.

---

## 3. Three architectural layers

The pipeline is best understood as three layers, not nine phases. (The Phase 1–9 numbering in [`PASS_INVENTORY.md`](../src/visitor/PASS_INVENTORY.md) is an artifact of how the code grew; the layers below are the right *mental* model.)

### Layer 1: Front-end — text + initial parse

**Job:** turn CC surface syntax into something TCC can parse, parse it, run the semantic checker.

**Components:**

- 16 preprocess passes (`cc/src/preprocess/preprocess.c`, ~8.8k lines) — see ADR-002 for the rationale.
- L2 prelude rewriter (`cc/src/preprocess/cc_l2_rewriter.c`) — fixes standard-C idioms TCC rejects (`offsetof`, `__attribute__((constructor(N)))`).
- Closure ID markers (`cc/src/preprocess/cc_closure_markers.c`) — injects `/*CC_CLO:N*/` comments for stable closure identity (foundation for future M4 work).
- CPP pre-expand (`cc/src/preprocess/cpp_expand.c`, M7.A) — runs TCC's CPP after text passes so `#include` and macro expansion happens before TCC's second-pass parse. Default-on.
- Initial parse: `cc_tcc_bridge_parse_string_to_ast` → root stub-AST.
- Checker: `cc/src/visitor/checker.c` — semantic checks (slice move, provenance, deadlock-by-construction).

**Reparses:** 1 (the initial parse). Text passes do not reparse.

**Why text and not AST:** see ADR-002. CC surface tokens fail TCC's lexer.

### Layer 2: Middle-end — type-driven AST lowering

**Job:** consume the typed AST, lower CC's typed constructs into vanilla C, batched into the smallest number of edit-apply-reparse cycles consistent with the producer-consumer ordering between passes.

**Components:**

- **Phase 3 Stage 1 (UFCS only):** `cc__collect_ufcs_edits` into a fresh `CCEditBuffer`, apply, reparse (conditional on edits).
- **Phase 3 Stage 2 (post-UFCS):** `cc__collect_closure_calls_edits` + `cc__collect_autoblocking_edits` + `cc__collect_await_normalize_edits` into one `CCEditBuffer`, apply, reparse (conditional on edits).
- **Phase 5 (closure literal lift + statement lowering):** `pass_closure_literal_ast.c` (~3.7k lines). Still a whole-file rewrite; M4 is the milestone to migrate to per-span.

**Reparses:** 2 in Phase 3 (each conditional) + 1 always in Phase 5 = 2–3 total.

**Why two stages in Phase 3:** see ADR-004 (UFCS producer/consumer).

**Why a separate Phase 5:** closure literal lift currently whole-file-rewrites the entire TU into a hoisted-function-table form. It can't compose into a per-span `CCEditBuffer` apply until M4 ships. When it does, Phase 5 collapses into Phase 3 Stage 2 and we drop a reparse.

### Layer 3: Back-end — async, final UFCS, emit, invoke host C

**Job:** finish lowering (async state machine), do a final cleanup pass for any UFCS surface syntax produced by spawn/nursery/defer lowering, emit C to disk, hand off to the host C compiler (TCC or cc).

**Components:**

- **Async state machine:** `cc/src/visitor/async_ast.c` (~3.6k lines) — `@async` fn → state machine.
- **Final UFCS sweep:** a second `pass_ufcs.c` invocation. Exists because earlier statement-level rewrites (defer, spawn, nursery) can synthesize new method-call surface syntax that UFCS needs to lower. Suspected fossil — pending audit. See [`COMPILER_CLEANUP_STATUS.md`](COMPILER_CLEANUP_STATUS.md) "Recommended next work".
- **Strip markers:** `cc/src/visitor/pass_strip_markers.c` — removes `@async` / `@noblock` / `@latency_sensitive` annotations from the buffer before emit.
- **Emit:** stream rewritten source to disk as `out/foo.c`.
- **Host C compiler:** invoked separately by `cc/src/driver.c` to compile the emitted C.

**Reparses:** 1 (async SM input) + 1 (final UFCS input) = 2 total.

**Why two:** async SM input is mandatory (needs an up-to-date AST after Phase 5). Final UFCS is the fossil candidate.

---

## 4. Architecture decisions (ADRs)

### ADR-001: Use TCC's stub AST instead of writing a C parser

**Status:** Accepted, load-bearing.
**Date:** 2025 (predates this doc; recorded here in 2026-05-28).

**Context:** CC needs a C parser to drive type-aware lowering for UFCS and closure-call lowering (constraint C3).

**Decision:** Extend TCC with `CONFIG_CC_EXT` hooks that expose a stub AST (`CC_AST_NODE_FUNC` / `_PARAM` / `_CALL` / `_DECL_ITEM` / `_BLOCK` / `_STMT`). Hold our changes as a patch (`third_party/tcc-patches/0001-cc-ext-hooks.patch`) so we can track upstream TCC.

**Consequences:**

- TCC bugs become our bugs (see ADR-006).
- TCC's accepted C dialect bounds what CC code can express. Workarounds for TCC limitations live in the L2 prelude rewriter and the L3 catalog (see `COMPILER_CLEANUP_STATUS.md`).
- The TCC upgrade workflow is documented in the root [`README.md`](../../README.md) "Updating TCC" section.

**Alternatives considered and rejected:**

- **Write our own C parser.** ~1 year of work; reinvents typedef disambiguation, declarator grammar, attribute parsing, `#line` handling. No CC-specific upside. The text-preprocess layer (C2) still needs to exist either way.
- **Use clang/libclang.** Adds a hard build dependency on LLVM (~hundreds of MB) for a project whose entire value proposition is being small and self-contained. Rejected.
- **Use only text-level lowering (no AST).** Fails C3 — UFCS needs receiver types that text can't compute.

### ADR-002: Text-rewrite CC surface syntax before TCC sees it

**Status:** Accepted.
**Date:** 2025.

**Context:** CC's surface tokens (`T[:]`, `T?`, `T!>E`, `int[~4 >]`, `Vec<T>`, `@match`, `=>`) fail TCC's lexer (constraint C2).

**Decision:** Run text-level preprocess passes that rewrite CC syntax to C-shaped tokens *before* TCC parses. Currently 16 passes in [`cc/src/preprocess/preprocess.c`](../src/preprocess/preprocess.c).

**Invariants for new text scanners** (enforce in code review): use `CCInertScan` from `cc/src/util/text_scan.h`, use comment-aware helpers from `cc/src/util/text.h`, add inert-region smoke tests under `tests/inert_*_tokens_smoke.ccs`, respect `CCInertScan.in_user_file` when M1 step (c) flips. Full list in [`PASS_INVENTORY.md`](../src/visitor/PASS_INVENTORY.md#invariants-for-new-text-scanners-must-follow).

**Consequences:**

- Text passes can't see types. Anything that needs types stays in the AST visitor layer.
- Text passes can't compose into a single `CCEditBuffer` apply across the layer boundary (CCEditBuffer is post-initial-parse).
- Bug class: text-scanner-meets-inert-region. We've hit it 5+ times (commits `43e0ebc`, `842dd8c`, `23ea0a5`, `22f2896`, `2aa5ad3`). `CCInertScan` is the canonical fix.

**Alternatives considered and rejected:**

- **Extend TCC's lexer to accept CC tokens.** Multi-week TCC fork work per token family (M5.5). Spike showed CPP pre-expand (M7.A) is a cheaper unlock for the macro case; M5.5 is on hold.
- **Stub-AST nodes for CC type syntax (M6).** Same multi-week cost per construct. Superseded by M7.
- **Defer the rewrite until after TCC parse.** Impossible — TCC's lexer rejects CC tokens before parsing.

### ADR-003: Per-span `CCEditBuffer` edits, not whole-file rewrites

**Status:** Accepted, mandatory for any new lowering pass.
**Date:** 2026-02-01 (infrastructure landed); 2026-05-28 (all four Phase-3 passes converted).

**Context:** Initial Phase-3 lowering passes each performed wholesale buffer rewrites: produce a new `char*` buffer with the entire TU rewritten, then assign it as the new working buffer. This made batching impossible — two passes that produced full-buffer outputs couldn't compose; their "edits" trivially overlap.

**Decision:** Every lowering pass emits per-span edits `(start_off, end_off, replacement, priority, owner)` into a shared `CCEditBuffer` ([`cc/src/visitor/edit_buffer.c`](../src/visitor/edit_buffer.c), ~360 lines). Edits are applied end-to-start so byte offsets stay valid. Overlapping edits from different owners are an error caught by the buffer.

**Consequences:**

- Multiple Phase-3 passes can collect into the same `CCEditBuffer` and apply once. We use this for Stage 2's three-pass batch.
- The mechanical refactor cost was real but bounded: ~1-2 days per pass (await_normalize → ufcs → closure_calls → autoblock).
- Latent bugs surface: e.g. `cc_debug_log("lower", "phase3 batched stage ...")` was unconditional and only became visible noise once the path was always-on (caught and fixed 2026-05-28).
- The Phase-5 closure literal lift remains whole-file. Migrating it (M4) is the next ADR-003-driven reduction.

**Alternatives considered and rejected:**

- **Sequential per-pass apply + reparse between every pass.** Old default; 4 reparses in Phase 3 alone. Killed 2026-05-28.
- **Single global edit buffer for the whole compiler.** Cross-layer edit buffer doesn't work — text preprocess passes have no AST to drive edits from, and they need to mutate the buffer before TCC sees it. The CCEditBuffer scope is correctly the AST-visitor layer.

### ADR-004: Two-stage Phase 3 batching

**Status:** Accepted, default path.
**Date:** 2026-05-28.

**Context:** With all four Phase-3 passes emitting per-span edits (ADR-003), the obvious next step was to batch all four into one `CCEditBuffer` apply + one reparse. The single-stage experiment regressed exactly one test: `tests/async_await_channel_auto_destroy_smoke.ccs`, where `await chan.send(x)` produced overlapping UFCS and await_normalize edits over the same byte range. Root cause: UFCS *produces* new call sites that the AST-driven downstream passes need to see (constraint C4).

**Decision:** Split Phase 3 into two stages with a reparse barrier between them:

- **Stage 1:** UFCS only. Apply + (conditional) reparse.
- **Stage 2:** closure_calls + autoblock + await_normalize, all into one `CCEditBuffer`. Apply + (conditional) reparse.

Orchestrator: `cc__apply_batched_phase3_passes(...)` in [`cc/src/visitor/visit_codegen.c`](../src/visitor/visit_codegen.c), called twice from the Phase-3 block (once per stage).

**Consequences:**

- Phase-3 reparses: 4 → 2 (each conditional on edits). Net savings: 2 reparses per TU in the common case.
- Stage 2's three passes are guaranteed to emit non-overlapping per-span edits (they target disjoint AST constructs: closure-typed CALLs, blocking CALLs under `@async`, `await` expressions).
- Stage 1's mandatory reparse cannot be eliminated by AST-side tricks without rebuilding TCC's call-resolution machinery in the visitor. It can be *folded* into Phase 5's reparse once M4 ships (closure literal → per-span); that's the next reduction target.

**Alternatives considered and rejected:**

- **Single-stage four-pass batch.** Regressed `async_await_channel_auto_destroy_smoke`. Failure was semantic, not a fix-with-priorities issue.
- **Virtual AST mutations (UFCS marks CALL nodes "as if already lowered" so downstream passes see them).** Requires reproducing TCC's name resolution for `Type_method(obj, ...)` against the augmented AST. 10x complexity increase for one reparse saved. Rejected.
- **Pre-lower UFCS in preprocess.** Fails C3 — UFCS needs receiver types.

### ADR-005: `#line` directives + `CCSourceMap` for diagnostics

**Status:** Accepted, load-bearing.
**Date:** 2026 M0.5 (infra); ongoing (per-pass adoption).

**Context:** The compiler rewrites the source buffer many times before emitting C. Without explicit source-mapping, host-C-compiler diagnostics and runtime traces would point at meaningless byte offsets in synthetic post-lowering text. Users need errors and panics to reference *their* source line.

**Decision:**

- Emit `#line N "user_file.ccs"` directives at every rewrite boundary so the host C compiler's diagnostics reference user source.
- Track translation explicitly via `CCSourceMap` ([`cc/src/diag/source_map.c`](../src/diag/source_map.c)) for compiler-internal mapping (e.g. translating TCC errors against the rewritten buffer back to user source).
- `CCInertScan` (`cc/src/util/text_scan.h`) tracks `in_user_file` so visitor passes can distinguish user code from inlined runtime headers (M1 step c).

**Consequences:**

- Every new text-rewriting pass must preserve `#line` discipline. Adding a pass that emits naked text without `#line` is a regression vector.
- TCC's `#line` handling is on the critical path (see ADR-006).
- Runtime diagnostics (R1, R2, R3) all consume `(file, line)` pairs that originate from `#line`-tracked source positions.

**Alternatives considered and rejected:**

- **Don't emit `#line`; let users debug rewritten C.** Unacceptable user experience. Rejected.
- **Maintain a full byte-level translation map without `#line`.** Doable but requires every diagnostic consumer to query it. `#line` is the universal standard; piggybacking on it costs nothing.

### ADR-006: Patch TCC's `pp_line` to stop swallowing negative-delta `#line` directives

**Status:** Accepted, patched in `third_party/tcc/tccpp.c` (~line 3875).
**Date:** 2026-05-27.

**Context:** TCC's preprocessor had a bug where `#line N "file"` directives with `N < (current_line + 8)` and `N < current_line` were silently dropped (the inner `while (d > 0)` loop is a no-op for `d < 0`). Concurrent-C routinely emits `#line N "file"` with N pointing *back* to a small line number after a large synthetic injection (e.g. resuming user source after prepending generated declarations). Symptom: `m0_5_diag_origin_line_fail` — diagnostics carried wrong line numbers.

**Decision:** Patch the condition from `level == 0 && f->line_ref && d < 8` to `level == 0 && f->line_ref && d >= 0 && d < 8`. Negative deltas now fall through to the regular `#line` emission. Patch is documented inline in `tccpp.c` with the CC-specific rationale.

**Consequences:**

- Our TCC fork is mandatory; the upstream-compatible alternative is to fix this upstream and wait for it to land. We chose the patch route.
- The patch is small (~3 lines + comment) and trivially rebases.
- Earlier `CCSourceMap` workarounds for this specific symptom were reverted (see M1_MIGRATION.md "Finding 2 fixed upstream").

**Alternatives considered and rejected:**

- **Avoid negative `#line` deltas in our emit.** Would require ordering all `#line` directives monotonically across the entire emitted file. Hostile to the natural emit order (per-region rewrites with their own per-region `#line` discipline).
- **Maintain a parallel byte-to-source map and bypass `#line` entirely.** Would require every host-C-compiler-emitted diagnostic to be re-translated by us. Untenable.

---

## 5. Non-goals (architectural moves to NOT undertake)

Each of these has been considered (some seriously, some only as devil's-advocate exercises) and rejected. The reasoning is recorded here so the next person doesn't have to rediscover it.

### NG-1: Don't write a CC-native parser

**Why tempting:** "If we owned the parser, we could lex CC tokens directly, eliminate the text-preprocess layer, and have one clean AST-driven pipeline."

**Why wrong:**

1. ~1 year of work to reach parity with TCC's parser. C is harder than it looks (typedef disambiguation, K&R declarators, `__attribute__` parsing, declarator grammar, preprocessor edge cases).
2. The text-preprocess layer **stays anyway**, because CC's surface tokens (`@match`, `with_deadline`, `Vec<T>` generics) are higher-level constructs that even a CC-native parser would want to desugar before parsing the lowered C. A parser for CC tokens just moves the rewrite from "before TCC" to "during CC parser" — same total work, different file.
3. The TCC stub-AST is good enough for our typed lowering needs (UFCS, closure_calls). Building a richer AST gains nothing concrete.
4. We'd lose the upstream TCC bug-fix pipeline. Today we benefit when TCC fixes bugs (and we patch when it doesn't, per ADR-006).

**Cost:** ~1 person-year. **Benefit:** ~0 (the text layer doesn't go away). **Don't do it.**

### NG-2: Don't move UFCS or closure_calls into the preprocess layer

**Why tempting:** "If all lowering happened in preprocess, the visitor layer could be deleted; we'd have no reparses at all."

**Why wrong:**

1. UFCS needs the type of `obj` in `obj.method(args)` to resolve `method` against the right typedef. Text scanners can't compute types — they'd need a parallel type-inference engine. That engine is exactly TCC's parser, just slower and ours.
2. closure_calls needs to distinguish a `CCClosure1`-typed callee from a regular function pointer. Same problem.
3. autoblock and await_normalize *could* move (annotation-driven and syntactic respectively), but the win is small (1 reparse saved at best) and they currently ride along in the Stage-2 batch for free.

**Cost:** rebuild type inference in the text layer. **Benefit:** unclear, possibly negative (text layer grows; AST layer doesn't shrink because Phase 5 / async SM still need it). **Don't do it.**

### NG-3: Don't fold Phase 3 Stage 1 and Stage 2 into one apply

**Why tempting:** "Two stages is one more reparse than one stage. If UFCS edits don't *physically* overlap with Stage 2 edits, can't we batch them?"

**Why wrong:**

1. UFCS doesn't just emit edits — it *produces new call sites* that Stage 2's AST-driven collectors must walk. Without the reparse barrier, Stage 2 walks the pre-UFCS AST and misses every UFCS-produced call.
2. Single-stage was tried (2026-05-28); regressed `async_await_channel_auto_destroy_smoke` (concrete failure on `await chan.send(x)`).
3. Schemes to avoid the reparse barrier — virtual AST mutations, edit-priority disambiguation, post-hoc fixup — all require reproducing TCC's name resolution against an augmented AST. 10x complexity for one conditional reparse saved.

**Cost:** rebuild call resolution in the visitor. **Benefit:** save ~1 reparse per TU. **Don't do it.**

The right way to reduce this reparse is **M4**: migrate closure literal lift (Phase 5) to per-span edits. That lets us fold Phase 5's reparse into Phase 3 Stage 2, dropping a *different* reparse without touching the UFCS dependency. See [`COMPILER_CLEANUP_STATUS.md`](COMPILER_CLEANUP_STATUS.md) M4.

### NG-4: Don't unify edit_buffer + source_map + diag into one mega-module

**Why tempting:** "They all carry source positions, they could share infrastructure."

**Why wrong:**

1. **Different lifetimes.** `CCEditBuffer` is per-pass-batch. `CCSourceMap` is per-TU. `CCDiag` is per-compile-job. Forcing one lifetime would either leak (longest wins) or over-free (shortest wins).
2. **Different consumers.** `CCEditBuffer` is consumed by `cc_edit_buffer_apply`. `CCSourceMap` is consumed by `cc_diag_translate_tcc_error`. `CCDiag` is consumed by `cc_diag_print_all`. Each consumer has zero overlap with the others.
3. **Different invariants.** `CCEditBuffer` enforces non-overlap. `CCSourceMap` enforces monotonicity. `CCDiag` enforces severity/origin tracking. Different.

The current split is right. Each module owns its invariants.

### NG-5: Don't rewrite the runtime side

**Why tempting:** "While we're cleaning up the compiler, why not the runtime too?"

**Why wrong:**

1. The runtime (R0–R3) is in good shape. The recent R1–R3 work shipped cleanly with smoke tests.
2. The runtime's structural problems are narrowly scoped: R4 / R5 (deeper send/recv source location at the stuck-on site) are well-defined incremental adds, not redesigns.
3. The compiler ↔ runtime interface is the `cc_rt_diag_*` API plus the closure ABI. Both are stable. Rewriting either would force compiler changes.

The runtime is **not** a refactor target. Leave it alone.

---

## 6. Where redesign **would** help

To be clear: this doc is not "don't ever change anything." It's "don't undertake a global restructure without first reading the constraints." There are real, scoped reductions worth doing:

### Active targets (in priority order)

1. ✅ **Audit the fossils (2026-05-28).** Three components were suspected dead code; audit results:
   - **Final UFCS sweep (reparse #6) — NOT a fossil. Kept.** Instrumented `cc__collect_ufcs_edits` count across the full 461-test smoke suite (393 reparse invocations). 150/393 (38%) produced ≥1 edit. The comment about defer/spawn/nursery synthesizing new UFCS surface syntax is accurate, not aspirational. Deletion would have regressed ~150 tests.
   - **`pass_nursery_spawn_ast.{c,h}` (1326 LOC) — DELETED.** All four exported entry points (`cc__rewrite_spawn_stmts_with_nodes`, `cc__rewrite_nursery_blocks_with_nodes`, `cc__collect_spawn_edits`, `cc__collect_nursery_edits`) had zero callers outside the file itself. The wildcard build pickup was the only thing keeping the .o alive. Spawn/nursery lowering is fully handled by `preprocess.c` + `pass_closure_literal_ast.c`.
   - **`cc__closure_proto_insert_off` walker (~80 LOC) — DELETED.** Both in-tree callers passed `skip_inline_protos=1`. Removed the walker, the `skip_inline_protos=0` codepath, the non-`_ex` wrapper, the `skip_inline_protos` parameter entirely, and renamed `_ex` back to the original name. Net: less surface, fewer modes.

   **Outcome:** −1406 LOC of dead code, 0 reparses saved (none of the fossils were on the reparse path), 461/461 still passing. The audit also validated the *non-fossil* (final UFCS sweep) so future readers don't redo the analysis.

2. ✅ **M4.a: gate Phase 5 reparse on `=>` presence (2026-05-28).** The Phase-5 closure-literal reparse was unconditional, even though `cc__rewrite_closure_literals_with_nodes` is a no-op for TUs without `=>` tokens. Added a `cc_contains_token_top_level(src_ufcs, ..., "=>")` guard around the entire Phase-5 block (reparse + closure pass call + free).

   **Measured impact (full 461-test smoke suite, before/after):**
   - Phase-5 reparses: 461 → **155** (−306, a 66% reduction).
   - 306/461 (66%) of TUs now skip a reparse + buffer alloc + closure-pass invocation entirely.
   - 461/461 tests still pass.

   The gating helper is inert-region-aware (`cc_contains_token_top_level` skips comments / strings / pp bodies), so the only false-positives are TUs with `=>` in real code that happens to be unreachable by the closure pass — those still pay the full cost, but cost no correctness.

3. **M4 follow-ups (deferred — see §6 "Targets that aren't worth it" below):** the original ARCHITECTURE.md claim that M4 would save 1 reparse for ALL TUs by folding Phase 5 into Phase 3 Stage 2 was **wrong**. Closure-literal lift is a *producer* for closure_calls (when a closure literal sits inside a closure-typed call's arg list, the literal must be lowered to `__cc_closure_make_N()` before closure_calls extracts arg text). That's the same producer/consumer ordering UFCS has, so folding closure_literals into Stage 2 would need a *new* third stage — net same reparse count. The real M4 win was M4.a (above).

4. **M1 step (c): `#line`-aware text scanners (finish).** 102/102 forward sites done; 3 backward + 1 special remaining (one fewer than pre-audit — the deleted nursery pass took 3 sites off the list). Unblocks the src_buffer swap (step a) which in turn unblocks dropping the `CC_PRE_EXPAND=0` opt-out and retiring redundant `_cch → _h` text passes.

   **Effort:** documented in M1_MIGRATION.md. **Win:** macro CC-syntax end-to-end, simpler reparse plumbing, source-map drift fixed.

### Targets that would help but are blocked

- Reducing preprocess pass count below 16 — blocked on M7.C (post-CPP-expand re-lower) for the channel-syntax case. Other consolidations (P8+P9, P11+P12) are low-value.
- TCC stub-AST nodes for CC type syntax (original M6) — superseded by M7 (CPP expand). Reopen only if a specific construct needs it.

### Targets that aren't worth it

- **Reduce reparses to 1.** Hard ceiling is set by C4 (UFCS producer/consumer), closure-literal-as-producer-for-closure-calls (analogous), and the async SM ordering. Realistic floor for TUs that use every feature is ~5 reparses (initial + Phase-3 stage 1 + Phase-3 stage 2 + Phase-5 closure-lift + async-SM + final-UFCS); simpler TUs hit fewer because most reparses are now conditional.
- **Fold Phase 5 (closure_literals) into Phase 3 Stage 2.** Same producer/consumer issue UFCS has (NG-3, ADR-004): when a closure literal sits inside a closure-typed call's arg list, closure_literals must lower it to `__cc_closure_make_N()` *before* closure_calls extracts the arg text. That forces a third Phase-3 stage with its own reparse barrier — net zero reparse savings, plus an additional stage's worth of orchestration complexity. The M4.a gating win (above) captured the easy savings without the structural cost.
- **Migrate `cc__rewrite_closure_literals_with_nodes` internals to per-span CCEditBuffer (the original M4.b spike target).** The function already builds an internal `Edit[2048]` per-span array — it just applies them inline via `cc__rewrite_with_edits` instead of pushing to the caller's edit buffer. Migrating wouldn't reduce reparse count (Phase 5 stays separate; see previous bullet). **No concrete payoff identified.** The orphaned `cc__collect_closure_edits` fake-collector — which was a wholesale `[0, src_len)` CCEditBuffer wrapper around the whole-file rewriter, never called — was deleted 2026-05-28 in the post-M4.a fossil sweep.
- **Merge text + AST layers.** Fails C2 + C3. See NG-1 and NG-2.
- **Replace TCC.** See NG-1.

---

## 7. How to evolve this document

When you make an architectural decision that warrants an ADR:

1. Add an entry to §4 with: Status, Date, Context, Decision, Consequences, Alternatives considered. Keep the style of ADR-001 through ADR-006.
2. If the decision invalidates a non-goal in §5, move that non-goal into §6 (or delete it if the underlying constraint has changed).
3. If the decision changes a layer in §3, update both the layer description AND `PIPELINE.md` / `PASS_INVENTORY.md`.
4. Update the "Last updated" line at the top.
5. Bump the date in the TL;DR if the count of layers / reparses / ADRs changed.

If you find yourself wanting to write a new ADR that contradicts an existing one, the contradiction itself is the most important content — write it explicitly. "ADR-007 supersedes ADR-004 because ..." is more useful than silently rewriting ADR-004.

---

## Cross-references

- [`PIPELINE.md`](../src/visitor/PIPELINE.md) — call-site map (where the reparses happen)
- [`PASS_INVENTORY.md`](../src/visitor/PASS_INVENTORY.md) — per-pass catalog (what each pass does)
- [`COMPILER_CLEANUP_STATUS.md`](COMPILER_CLEANUP_STATUS.md) — ship status (what landed, what's next)
- [`M1_MIGRATION.md`](M1_MIGRATION.md) — `#line`-aware scanner migration tracker
- [`M6_DEFERRED.md`](../src/visitor/M6_DEFERRED.md) — superseded TCC fork plans
- [`DEBUG_VARS.md`](../src/diag/DEBUG_VARS.md) — environment flags
- [`DIAG_AUDIT.md`](../src/diag/DIAG_AUDIT.md) — runtime diagnostics audit
