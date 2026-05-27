# Compiler cleanup status (M0–M5.5)

**Last updated:** 2026-05-27  
**Smoke suite:** 459 tests passing under the new default (pre-expand on)
AND under the legacy non-expanded path (`CC_PRE_EXPAND=0 make smoke`)

### Recent (this session — 2026-05-27)

- **R3** **runtime: `!>` source-location propagation chain.** New `cc_rt_diag_record_unwrap_site` + query/print API in `cc/runtime/cc_rt_diag.{h,c}` (ring-buffered, no per-propagation allocation). Wired into both `__cc_uw_err_at` _Generic arms (baseline + per-TU enumerated in `visit_codegen.c`) AND the typed-callee fast path in `pass_result_unwrap.c`'s `cc__ru_emit_uw_err_binder`, so every `!>` propagation pushes its `(file, line)` regardless of which lowering arm fires. Forward-declarations live in `cc_result.cch` so user code picks them up via the prelude. New smoke: `tests/runtime/r3_unwrap_chain_smoke.ccs` — 2-deep `!>` propagation, verifies chain length, file substring, line numbers, and clear() reset. Audit entry added to `cc/src/diag/DIAG_AUDIT.md`.
- `9d37916` **nursery: fix lost-wakeup race in worker-frees alive_count barrier.** Added `atomic_thread_fence(memory_order_seq_cst)` on both sides of the Dekker pair in `cc_nursery_wait` and `cc_nursery_notify_child_done`; was hanging `stress/nested_nursery_deep.ccs` on ARM64. New regression smoke: `tests/nursery_worker_frees_race_stress_smoke.ccs` (4000-iteration shallow nursery hammer).
- `8c80304` **nursery: document why notify_child_done's wake is prev==1-conditional.** Considered moving `wake_primitive_wake_all` out of the `if (prev == 1)` block as "belt-and-suspenders" but on inspection it adds N futex syscalls per N-child nursery for zero correctness benefit (the gen-counter handles intermediate decrements correctly). Doc-only commit so the next reader doesn't propose the same change.
- `df28528` **codegen: hand-crafted-C polish for defer expansion + closure decls.** `cc__normalize_defer_stmt` wraps multi-stmt defer bodies as `{ ... }` with breathing space; brace-exit defer-flush snapshots and restores the trailing-`}` indent so the user's brace lands at its original column. Closure-decl groups in `pass_closure_literal_ast.c` get a blank line between adjacent closures' proto blocks.
- `7b9723b` **preprocess: L2 prelude rewriter — convert standard-C idioms TCC rejects.** New `cc/src/preprocess/cc_l2_rewriter.{h,c}` rewrites `offsetof(T,F)` → `__builtin_offsetof(T,F)` and `__attribute__((constructor(N)))` → `__attribute__((constructor))` (priority preserved through to host-compiler emit). Wired into both initial-parse and reparse paths. Two new smokes; adding a new idiom is a single function.
- `75a773a` **preprocess: closure-ID markers — foundation pass.** New `cc/src/preprocess/cc_closure_markers.{h,c}` injects `/*CC_CLO:N*/` comments before every closure literal in source order. Markers ride along in `root->parse_buffer` as inert comments. **Consumer migration (d.2) is documented but not yet implemented** — the obvious dual-injection-into-src_ufcs approach broke 8 unrelated smokes due to byte-position-sensitive text passes; see the header for the three viable plumbing options.
- `10db87e` **codegen: hand-crafted-C polish for return-path + function-cleanup epilogue.** Plain-`return X;` defer flush now detects mid-line vs multi-line context and emits accordingly (inline-spaced on one line, or indent-aligned across multiple lines). Function-cleanup epilogue (`__cc_cleanup_N:` label path) keeps the label at col 0 but indents the defer body + trailing `return`s to the function-body indent.

This is the single source of truth for the compiler cleanup workstream (M0–M5.5). See also [PIPELINE.md](../src/visitor/PIPELINE.md), [PASS_INVENTORY.md](../src/visitor/PASS_INVENTORY.md), [DIAG_AUDIT.md](../src/diag/DIAG_AUDIT.md), [M6_DEFERRED.md](../src/visitor/M6_DEFERRED.md).

---

## Shipped (complete)

| Milestone | What landed |
|-----------|-------------|
| **M0** | `PIPELINE.md`, `DIAG_AUDIT.md`, `PASS_INVENTORY` audit, `perf/baseline_M0.txt`, `scripts/capture_baseline.sh`, orphan/unwired notes |
| **M0.5** | `cc/src/diag/` — `cc_diag_emit`, `CCSourceMap`, `cc_diag_mangle`, `CCEditBuffer` span fields, `DEBUG_VARS.md`, `--show-lowered=<phase>`, driver init |
| **M3** | `cc_preprocess_for_initial_parse`, `cc_preprocess_for_reparse`, `cc_preprocess_for_light_reparse` (phase-1 skip when safe) |
| **M5** | `third_party/tcc-patches/tcc_ext_api.h`, `cc/src/parser/tcc_ext_api.c` — versioned API, diag bridge stubs, comment-aware search wrappers, recognizer hook registration |

---

## Shipped (partial — code exists, not fully integrated)

| Milestone | Done | Remaining |
|-----------|------|-----------|
| **M1** | `cc_build_parse_input()` in `parse.c` | `visit_codegen.c` still duplicates prep; source map not threaded through codegen |
| **M2** | `cc__apply_batched_phase3_passes()` | **Default is sequential** (429 tests). Opt-in: `CC_BATCH_PHASE3=1` (experimental; had regressions when default-on) |
| **M4** | `mangle.h` included in closure pass | Whole-file closure lift unchanged; `cc_diag_mangle_symbol` not used for emitted names |
| **M5.5** | `cc_macro_recognizer.c` hooks registered at parse | **Token synthesis into TCC lexer not implemented** — `#define CHAN(T) T[~4 >]` still fails; see [tests/macro/README.md](../../tests/macro/README.md) |
| **Runtime R0** | `cc/runtime/cc_rt_diag.c` stubs in runtime | R1, R2, R4, R5 (async backtrace naming, channel deadlock text, etc.) not implemented |
| **Runtime R3** | `cc_rt_diag_*_unwrap_*` API + macro/codegen wiring | `!>` propagation chain landed; see `cc/src/diag/DIAG_AUDIT.md` |

---

## Deferred

| Milestone | Notes |
|-----------|--------|
| **M6** | Pilot stub-AST for `T[~N >]`; retire P4 text pass. See [M6_DEFERRED.md](../src/visitor/M6_DEFERRED.md). **Likely superseded by M7 pre-expand.** |

## M7 — Pre-expand integration (in progress)

| Phase | Status | What landed |
|-------|--------|-------------|
| **M7.A** (opt-in, no regressions) | **Shipped** | `cc_cpp_expand()` runs TCC's CPP after `cc_preprocess_for_initial_parse` so the prepended container/result-type `#include` lines resolve. GCC-style `# N "file" flags` markers normalized to bare C99 `#line` to prevent TCC's parser from re-triggering system-header inclusion. Opt-in via `CC_PRE_EXPAND=1`. **429/429 smoke pass; examples/stress baselines unchanged (same 2 pre-existing failures: `recipe_tcp_echo.ccs`, `syscall_kidnap.ccs`).** |
| **M7.B** (`#define`-aware scanner) | **Shipped** | `CCScannerState` now tracks `in_pp` and treats any `#`-led line (with backslash-newline continuations) as non-code, so all 13 phase-1 passes that use `cc_scanner_skip_non_code` (`cc__rewrite_chan_handle_types`, `cc_rewrite_slice_types`, `cc_rewrite_generic_containers`, etc.) no longer rewrite tokens inside `#define`/`#include`/`#if` bodies. The visitor-side `cc__rewrite_chan_handle_types_text` in `pass_channel_syntax.c` (which has its own ad-hoc scanner) was also taught the same `in_pp`/`pp_continued`/`at_line_start` plumbing — covered by `tests/m7b_define_chan_body_unused_smoke.ccs` which previously failed with "too many basic types" when an unused `#define LOOKS_LIKE_CHAN(T) T[~4 >]` was present. **436/436 smoke pass; CC_PRE_EXPAND=1 still parity with baseline.** The CHAN macro definition now survives intact through phase-1 (verified via debug dump); CPP correctly expands `CHAN(int)` to `int[~4 >]`. |
| **M7.C** (post-expand re-lower + reparse plumbing) | **Partially shipped** | **(a) Registry-preserving re-lower** (`cc_relower_cc_type_syntax_preserving_registry`) added in `preprocess.{c,h}` — wraps the same four header-safe lowerings as `cc_rewrite_header_type_syntax_shared` but deliberately does NOT call `cc_type_registry_clear`, so it is safe to run after the main preprocess has populated the registry. Wired into `cc_build_parse_input` right after `cc_cpp_expand`: the initial parse now compiles macro-generated CC type syntax (e.g. `int[~4 >]` from `#define CHAN(T) T[~4 >]`) into `CCChanTx_T` without disturbing existing Result/Vec/Map registrations. **(b) Reparse pre-expand** wired into `cc__reparse_source_to_ast` just before `cc_tcc_bridge_parse_string_to_ast`, gated behind a separate `CC_PRE_EXPAND_REPARSE=1` env (see below). 429/429 smoke pass with `CC_PRE_EXPAND=1` (M7.A behavior preserved). Full macro CHAN end-to-end still needs the reparse path to also run pre-expand without regressions; tracked under M7.C2 caveat below. |
| **M7.C2** caveat | Opt-in only | `CC_PRE_EXPAND_REPARSE=1` runs CPP over the FINAL reparse buffer (after `cc_preprocess_for_reparse` + `cc__prepend_reparse_prelude` + parser-helper rewrites — earlier placement causes `__mbstate_t` double-decl). Validated end-to-end pipeline but regresses 4 smoke tests (`async_chan_await_works_smoke`, `async_channel_typed_lowered_smoke`, `call_site_noblock_smoke`, `ufcs_nested_std_io_smoke`) because CPP-expanded reparse output changes AST shapes in ways that confuse the async-AST and a few UFCS passes. Kept opt-in so it can be unblocked one pass at a time without disturbing the default. |
| **M7.C3** (M1-lite visitor plumbing + heap bug fix) | **Shipped** | **(a) AST root carries the pre-expand text.** `CCASTRoot` gained two owned fields: `parse_buffer` (post-CPP-expand + post-relower, the exact text TCC parsed) and `parse_buffer_pre_relower` (post-CPP but still with `[~ ... >]` chan brackets intact). `cc_build_parse_input` now copies the pre-relower buffer before running the in-place re-lower; `parse.c` transfers ownership of both to the AST root, and `cc_tcc_bridge_free_ast` frees them. **(b) Visitor ctx exposes the buffer.** `CCVisitorCtx` gained `pre_expanded_buf`/`pre_expanded_len` (NULL when pre-expand off). `walk.c` populates them from the root, preferring the pre-relower copy so bracket-based scanners get a view that still has `[~ ... >]` even when the AST sees `CCChanTx_int`. **(c) Channel-pair scanner fallback.** `cc__find_chan_decl_before` is now parameterized by an `alt_buf`/`alt_len` fallback. When the raw user source doesn't contain a `[~ ... >] name;` decl (e.g. the user wrote `CHAN(int) tx;`), the scanner searches the pre-expand buffer too; on hit, the caller uses the matching buffer for `cc__parse_chan_bracket_spec`. **(d) `cc_cpp_expand` heap-safety fix.** On macOS, `open_memstream(3)` returns a buffer whose reserved capacity extends past its logical end — a later `malloc()` can land inside that capacity and silently scribble over the trailing NUL when the caller writes its new allocation. `cc__rewrite_chan_handle_types` then scanned past the original end into the caller's chunk and produced a buffer ~2× the expected size. `cc_cpp_expand` now re-packs its output into a fresh tight allocation before returning, which permanently retires that footgun for all callers. **436/436 smoke pass, both default and `CC_PRE_EXPAND=1`.** Full end-to-end macro CHAN compile still blocked on `CC_PRE_EXPAND_REPARSE` regressions (M7.C2). |

---

## Reparse count (current)

- **~9** `cc__reparse_source_to_ast` sites in `visit_codegen.c` + 1 initial parse
- **Target** (after M2 default batch + M4 fine-grained): 3–4

---

## Recommended next work

> **Central blocker risk callout:** M1 (the visitor refactor) is the
> load-bearing piece for four otherwise-stalled items: macro CC-syntax
> end-to-end, flipping `CC_PRE_EXPAND=1` to default, retiring redundant
> `_cch → _h` rewrites, and fixing the `m0_5_diag_origin_line_fail`
> source-map drift.  The earlier framing — "just unblock the four
> `CC_PRE_EXPAND_REPARSE` regressions and flip the flag" —
> understated the problem: those four passes fail because
> `visit_codegen.c` reads `src_all` from disk (small, user-source-
> shaped buffer) while the pre-expand reparse's AST stores
> `fn->lbrace/rbrace` as offsets into a much larger inlined-headers
> buffer.  No per-pass plumbing fixes that coordinate mismatch; we
> have to make the visitor's working buffer agree with the AST's
> parse buffer.  This is the actual M1 refactor.  Doing it
> incrementally is safe (the M7.C3 plumbing is already in place to
> support it), but it is bigger than one commit.

1. **Closure-literal refactor — DONE (proto-placement layer).**
   `visit_codegen.c` now calls `cc__rewrite_closure_literals_with_nodes_ex`
   with `skip_inline_protos=1`, bypassing the brittle in-source walker
   (`cc__closure_proto_insert_off`).  File-scope forward decls are placed
   via the new `cc_find_first_func_def_offset` helper (just before the
   first top-level function definition — past `#include`s AND user
   typedefs).  Fixes the block-scope `static` failure in
   `examples/recipe_tcp_echo.ccs`.  Smoke clean in both modes.
   **Remaining layers** (separate bugs, NOT addressed by this refactor):
   - ~~`recipe_tcp_echo.ccs` layer 2: captured `sock` is not unpacked from
     `__env`~~ **FIXED (May 2026)** — was downstream fallout of layer 1
     plus the `=>`-in-comment scanner trap, not an actual capture-
     emission bug.  Verified end-to-end: `recipe_tcp_echo --test`
     completes the client/server round-trip cleanly.
   - ~~`syscall_kidnap.ccs`: capture-variant closure inside a `for` loop
     is not detected at all~~ **FIXED (May 2026)** — root cause was a
     class of bug: byte-level `=>` scanning loops in the closure
     descriptor recovery path did not skip C comments or string
     literals.  A `// Pattern: (a && b) => exit` comment between two
     real closures latched the recovery scanner onto the comment's
     `=>`, claimed a fake descriptor pointing into the comment, and
     starved the real heartbeat closure of its descriptor.  Fix:
     introduce `cc__find_next_arrow_skipping_inert` /
     `cc__find_prev_arrow_skipping_inert` (routing through the
     existing `cc__scan_skip_string_comment` machinery) and use them
     in all four `=>` scan sites in `pass_closure_literal_ast.c`
     (best-effort forward/backward, post-best-effort validation, and
     the descriptor-recovery scan).  Regression guards live in
     `tests/inert_*_tokens_smoke.ccs` (one file per CC scanner
     family — closure, channel, UFCS, `@create`/`@destroy`,
     `@async`/`@await`/`@defer`, Result-unwrap, generics) so any
     future text-scanner that forgets to skip inert regions will
     fail one of these tests.

2. **M1 visitor refactor** — bigger than originally framed.
   A spike (May 2026) attempted the naive form — swap
   `src_all = cc__read_entire_file(ctx->input_path)` →
   `src_all = strdup(root->parse_buffer)` when `CC_PRE_EXPAND=1` —
   to align the visitor's working buffer with the AST's coordinate
   space.  Smoke went from 436/436 to ~62/436 under
   `CC_PRE_EXPAND=1`.  The dominant failure mode is **not** the
   reparse prelude (that part is solvable; see below) but that
   visitor text scanners then see the inlined CC runtime headers
   (`<ccc/cc_channel.cch>`, `<ccc/std/vec.cch>`, `<ccc/cc_result.cch>`,
   etc.) as part of `src_all`.  Patterns those scanners look for —
   `cc_channel_pair(`, `[~ ... >]`, UFCS calls, `@async`, `!>`, etc.
   — are present in the runtime headers themselves, so scanners
   match against header content and emit spurious diagnostics or
   rewrites.
   The real M1 lift is therefore three pieces:
   - **(a) Source-buffer unification.** One-line swap of `src_all`
     to `root->parse_buffer` when pre-expand is on.  **TODO** —
     blocked on (c) below.
   - **(b) Reparse prelude awareness.**  `cc__reparse_source_to_ast`
     skips `cc_preprocess_for_reparse` + `cc__prepend_reparse_prelude`
     when its input is pre-expanded (system headers + container
     `.cch` files already inlined → re-prepending double-decls
     `__mbstate_t` etc.).  Plumbing for this is already in place:
     `CCReparseFlags.src_is_pre_expanded` + the
     `cc__reparse_source_to_ast_ctx` wrapper.  Confirmed end-to-end
     in the spike: with the swap on and the flag set, reparse made
     it past the `__mbstate_t` wall before hitting the next class
     of failures.  **DONE (plumbing).**
   - **(c) `#line`-aware text scanners.**  Each visitor pass that
     scans `src_all` for syntactic patterns needs to filter by
     origin file (the file the nearest preceding `#line N "..."`
     points to) so it only acts on tokens that originated in the
     user TU.  **PARTIAL.**  Foundation landed (May 2026):
       - `cc/src/util/text_scan.h` exposes `CCInertScan` — the
         visitor-side shared scanner state.  It tracks comments,
         string/char literals, preprocessor-directive bodies
         (with `\`-line-continuation handling), and parses `#line`
         directives to maintain an `in_user_file` flag (by
         basename match against `ctx->input_path`).
       - `pass_result_unwrap.c::cc__find_bang_token_from` migrated
         to use `CCInertScan` (replaces ~50 lines of inline state
         machine with 10 lines of helper calls).  Proves the
         migration pattern for find-only scanners.
       - `tests/line_directive_origin_filter_smoke.ccs` validates
         that `#line` directives don't break compilation today (the
         test will be extended to also include CC tokens in
         non-user-TU regions once per-pass migrations land).
     **TODO (per-pass migration).** Roughly 10–15 visitor passes
     still inline their own comment/string/pp state machines.  Each
     should follow the `cc__find_bang_token_from` template:
       1. Add `#include "util/text_scan.h"`.
       2. Replace the inline state vars (`in_str`/`qch`/`in_lc`/
          `in_bc`/`in_pp`/etc.) with a `CCInertScan` initialized
          from `ctx->input_path`.
       3. At the top of each loop iteration, call
          `cc_inert_scan_step(&scan, src, n, &i)` and `continue`
          if it returns 1 (with output passes also appending the
          consumed range to their output buffer).
       4. After the step, optionally check `scan.in_user_file` to
          skip rewriting tokens that came from inlined headers.
     Until (a) lands, the `in_user_file` flag is always 1 (because
     `src_all` is still the raw user file with no `#line`
     directives), so today the migration is mechanical and risk-
     free — purely deduplicates the state machines.  When (a)
     flips on, the `in_user_file` filter kicks in automatically.
   - The M7.C3 plumbing (`CCASTRoot.parse_buffer*`,
     `CCVisitorCtx.pre_expanded_buf`,
     `cc__find_chan_decl_before` alt_buf pattern) is already there
     to support this work and remains useful as a fallback for
     scanners that aren't yet `#line`-aware.

3. **Flip `CC_PRE_EXPAND=1` to default** — **DONE 2026-05-26**.  Pre-expand
   is now the default for the initial parse; opt out with
   `CC_PRE_EXPAND=0` or `CC_PRE_EXPAND=` (empty).  Same 447/447 smoke
   under both new-default-on and legacy-off; same 47/47 stress.
   The dead reparse-side knob `CC_PRE_EXPAND_REPARSE` was removed at
   the same time (CPP-expanding the reparse buffer breaks AST coord
   alignment with `src_ufcs` until the full M1 swap lands).
   The `.env` sidecar pinning `tests/m0_5_diag_origin_line_fail.ccs`
   to `CC_PRE_EXPAND=` stays until the M1 visitor + source-map align;
   see that file's comment for the rationale.

4. **Retire redundant text passes** (post-flip). With CPP handling
   `#include` resolution unconditionally, several legacy passes become
   no-ops or near-no-ops: the local/system `_cch → _h` rewriters,
   parts of phase-1 chan_handle/slice/Generic lowering that re-run on
   already-lowered text, etc. Audit and remove.

**4a. `cc_type_info` runtime type system + erased containers.**
*Status: in progress (Commits 1, 2, 3a, 3b, 3c.2-lite, smell-sweep landed 2026-05-27).*

This is the strategic foundation under "generics with selectable
link-time footprint" and "performant + fully complete comptime."

  - Commit 1 (`11251dd`): `cc_type_info` struct in
    `cc/include/ccc/cc_type.cch` — fixed-shape on-disk record
    (name, mangled, id, size, align, kind, nfields, flags,
    fields[], copy_fn, drop_fn).  Primitive symbols
    (`__cc_ti_int`, `__cc_ti_char`, …) emitted from
    `cc/runtime/cc_type_info.c` and always linked.  Layout-locking
    smoke: `tests/cc_type_info_primitives_smoke.ccs`.
  - Commit 2 (`b2357ec`): `cc_dyn_vec` type-erased dynamic array
    in `cc/include/ccc/cc_dyn_vec.cch` + `cc/runtime/cc_dyn_vec.c`.
    SINGLE implementation (`cc_dyn_vec_push`, `_pop`, `_at`,
    `_clear`, `_free`, `_init`, `_reserve`) handles any element
    type via `ti->size`/`ti->align`/`ti->copy_fn`/`ti->drop_fn`.
    Smoke (`tests/cc_dyn_vec_basic_smoke.ccs`) exercises int,
    char, double AND a non-POD `OwnedCounter` with a `drop_fn`,
    proving the dispatch contract end-to-end.  Symbol audit on
    the test object confirms 7 undefined refs total — one per
    op, regardless of element type.
  - Commit 3a (this): per-T `cc_type_info` emission for codegen-
    generated `CC_VEC_DECL_ARENA(T)` / `CC_MAP_DECL_ARENA(K,V)`
    instantiations + the registry that makes `type_of(T)` work
    by name.  Three pieces:
      1. `visit_codegen.c` now emits a `static const cc_type_info
         __cc_ti_<mangled>` next to every generic container
         instantiation, plus a `__attribute__((constructor(102)))`
         that calls `cc_type_info_register(&__cc_ti_<mangled>)`
         at startup.
      2. `cc_type_info.c` ships a small global registry
         (`cc_type_info_register` / `cc_type_of`) and a
         `constructor(101)` that pre-registers all primitives.
      3. `cc_type.cch`: `type_of(T)` is now sugar for
         `cc_type_of(#T)`.  Why: TCC's initial parse rejected
         `&__cc_ti_<undeclared>` even when the symbol existed in
         the lowered output (codegen hadn't emitted it yet).  A
         function call with a string literal parses cleanly and
         doesn't depend on the symbol being declared at parse
         time.  Lookup is O(n) on a small array — measurable but
         negligible until type counts are in the thousands.
    Smoke: `tests/cc_type_info_generic_emit_smoke.ccs` covers
    primitives + custom-struct Vec + Map, identity stability
    across repeated lookups, distinctness across mangled names,
    and the NULL-for-unknown contract.
  - Commit 3b (this): registry entries for stdlib-pre-baked Vec
    typedefs (`CCVec_int`, `CCVec_char`, `CCVec_size_t`,
    `CCVec_float`, `CCVec_double`, `CCVec_voidptr`,
    `CCVec_charptr`, `CCVec_intptr` — typedef'd directly in
    `vec.cch` as `typedef __CCVecGeneric CCVec_X;` and therefore
    bypassed by the codegen emission path 3a hooked into).
    `cc_type_info.c` now ships a `static const cc_type_info`
    plus a `constructor(102)` registration for each.  Layout is
    mirrored as a private `cc__prebaked_vec_layout` (size/align
    matched to `__CCVecGeneric`); the smoke pins all pre-baked
    sizes equal to detect drift.  `type_of(CCVec_int)` now works
    out-of-the-box for unmodified user code that picks up the
    pre-baked typedef.
  - Commit 3c.2-lite (this): user struct registration via
    hand-rolled `CC_TYPE_INFO_BEGIN/FIELD/END` macros in
    `cc_type.cch`.  Each registered struct lands in the runtime
    registry with `kind=CC_TK_STRUCT`, full field metadata
    (name, type-pointer-to-another-cc_type_info, byte offset),
    and a `constructor` that auto-registers at startup.  Field
    types compose: a `WithPair { Pair inner; }` struct's
    `inner` field correctly resolves to the SAME
    `cc_type_info*` `type_of(Pair)` returns — the type graph
    traverses across struct boundaries via the registry.
    Smoke: `tests/cc_type_info_struct_introspect_smoke.ccs`
    pins kind/size/align/nfields, per-field name/type/offset
    correctness, and the cross-struct type-graph property.
    Same commit also added typed accessors (`cc_ti_kind`,
    `cc_ti_flags`, `cc_ti_size`, `cc_ti_align`, `cc_ti_nfields`)
    so user code reads the storage-narrowed fields back as
    their semantic types without casting.
  - Smell sweep (`d57c1af`, this): post-landing audit.  Fixed
    `CC_TF_ERASABLE` inconsistency (now set on every container
    emission path); standardized on `cc__ti_reg_*` prefix for
    auto-registrar functions (was split with `__cc_ti_reg_*` in
    codegen); refactored the duplicated Vec/Map `cc_type_info`
    sprintf in `visit_codegen.c` into one helper with snprintf
    truncation guard; added a real OOM diagnostic to
    `cc_type_info_register` (was silently dropping); fixed seven
    spots of doc drift (milestone refs, stale macro forms,
    removed-priority comments, accessor usage in examples).
    Added two contract smokes:
      - `cc_type_info_contracts_smoke`: pins de-dup-by-name
        (first-registration wins; shadow registrations rejected),
        accessor↔storage round-trip
        (`(uint16_t)cc_ti_kind(ti) == ti->kind`, etc.), and
        `CC_TF_ERASABLE` on all container paths.
      - `cc_type_info_nested_container_smoke`: pins
        `Vec<Vec<int>>` codegen emission for both layers,
        mangled name `CCVec_CCVec_int`, both layers kind
        `GENERIC_INST` + `CC_TF_ERASABLE`, outer size equals
        `sizeof(Vec<int>)`.
    Smoke: 453/453.
  - Commit 3c.1-lite (`<this commit>`): compile-time diagnostic
    for unregistered `type_of(T)` / `cc_type_of("T")` calls.
    New pass `cc/src/visitor/pass_check_type_of.c` runs after
    preprocess in `cc_build_parse_input`.  It walks the RAW user
    buffer (still pre-CPP, so `type_of(T)` and macro-style
    `CC_TYPE_INFO_BEGIN(X)` are visible) with `CCInertScan`,
    builds a "known names" set from (i) the 9 hardcoded
    primitives, (ii) the 8 hardcoded pre-baked Vec typedefs,
    (iii) every Vec/Map instantiation in this TU's
    `CCTypeRegistry`, (iv) every `CC_TYPE_INFO_BEGIN(X)` in the
    source, then flags every `type_of(X)` / `cc_type_of("X")`
    call whose `X` is none of those.  Warnings print inline at
    emit time (`cc_diag_print_all` only flushes on build
    failure, which a warning shouldn't cause).  Errors flow
    through the buffered path.  Tunable via two env vars:
    `CC_TYPE_OF_CHECK=0` disables; `CC_TYPE_OF_STRICT=1`
    upgrades warnings to errors and fails the build (CI use).
    Smoke: `tests/check_type_of_unregistered_fail.ccs` +
    `.env` (`CC_TYPE_OF_STRICT=1`) + `.compile_err` pins the
    diagnostic text, file/line/col, and that the strict-mode
    build fails.  All 454 existing smokes still pass with zero
    spurious warnings (verified by `grep -c "warning: type_of"`
    on the full build log under both default and `CC_PRE_EXPAND=0`).
    Total: 455/455.
  - Commit 3c.3 (deferred — highest risk): extend `@comptime`
    so `type_of(T).fields` walks struct layouts at compile time.
    This unlocks the "performant + fully complete" half of the
    strategic goal.

The legacy `cc_type_register(name, hooks)` comptime API is
untouched; the new entry-point is deliberately named
`cc_type_info_register(const cc_type_info*)` to avoid the name
collision.  Unification will happen in a later milestone once
3c's parser builtin lands.

**4b. Stable closure-IDs + delete `pass_closure_literal_ast.c`'s
recovery path.**  Today closure literals are matched between passes
by `(file, line_start, line_end, col_start)`.  That breaks whenever
a reparse pulls in a header whose `#line` directives drift the
TCC line counter relative to the working source buffer.  When it
breaks, `cc__closure_start_off_best_effort` falls back to a
forward `=>` scan ("the recovery path") which is fragile (it has
been the root cause of multiple inert-region bugs this session —
e.g. the `syscall_kidnap.ccs` regression) and *cannot* tell apart
genuine closures from arrow patterns in commented-out code.

The right shape is to give each closure literal a stable identity
at construction time:

  - Add a new pre-AST text pass `cc__inject_closure_ids` that
    walks the buffer once with `CCInertScan`, finds each `=>`,
    derives the closure's `(` start, and inserts a unique marker
    immediately before it (e.g. `/*CC_CLO:42*/`).  Markers are
    monotonic per-TU and survive every downstream rewrite (they
    live inside a C comment).
  - Teach `pass_closure_literal_ast.c` to look up closures by
    marker ID rather than by `(line_start, line_end, col_start)`.
    Each `CCClosureDesc` carries the same ID; pass-to-pass
    identity is now exact, not heuristic.
  - Delete `cc__closure_start_off_best_effort`'s arrow-scan
    fallbacks and the entire "recovery" branch in
    `cc__resolve_closure_descriptors_into`.

Cost: one new ~300-LOC text pass, ~150 LOC deleted from
`pass_closure_literal_ast.c`, a smoke test that puts comments
with `=>` between real closures (already partly covered by
`tests/inert_closure_tokens_smoke.ccs`).  Risk: medium — every
downstream pass that scans for closures has to switch to the
marker.  Scope to a single PR with a kill-switch env var until
447/447 holds for ≥3 stress runs.

**4c. L2 — Pre-parse rewrite for standard C idioms TCC rejects.**
*Status: deferred (no concrete trigger yet, but accumulating).*

Catalog of host-C constructs that TCC's stub-AST currently
rejects, forcing user code to write the non-standard spelling:

  - `__attribute__((constructor(N)))` — priority arg rejected.
    Workaround in place: drop the priority; registry doesn't
    need ordering.  Cost of restoring priorities later: zero
    (just re-add the arg, since the underlying mechanism still
    works on the host C compiler).
  - `offsetof(T, F)` from `<stddef.h>` — glibc/musl/Apple SDK
    expand to `((size_t)&(((T*)0)->F))` which trips stub-AST.
    Workaround in place: `__builtin_offsetof(T, F)`, hidden
    inside the `CC_TYPE_INFO_FIELD` macro.  If a future user-
    facing macro needs `offsetof`, prefer hiding it the same way.
  - `&undeclared_symbol` — rejected with "lvalue expected" even
    when the symbol is defined later in the lowered output.
    Workaround in place: `type_of(T)` does a registry lookup by
    name instead of `(&__cc_ti_##T)`.

Shape of the eventual fix: a single text pass run on the
pre-expand buffer (before TCC sees it) that rewrites these
constructs to forms stub-AST accepts.  Cost: ~200 LOC of pass +
table-driven rewrites + smoke per rewrite rule.  Trigger: when
the catalog hits ~6 entries OR when one of them starts costing
real user-code ergonomics (e.g. someone wants to write
`offsetof` in their own code, not just inside our macros).

**4d. L3 — TCC stub-AST C-dialect audit.**
*Status: deferred (depends on 4c's catalog being well-populated).*

The right long-term fix for the items above is to widen
stub-AST's accepted C dialect to match GCC/clang's behavior on
common idioms.  This is genuine TCC parser work — touching the
attribute parser, the unary-operator parser, etc.  Don't start
this until 4c's catalog gives a prioritized list; otherwise
risk doing the work in the wrong order.

Then in priority order (independent of M1):

5. **Doc sync** — this file; keep PIPELINE/PASS_INVENTORY aligned (ongoing)
6. **`tests/diag/` harness** — `EXPECT-DIAG` parsing; 3–5 smoke tests (protects I1–I8)
7. **M2 finish** — fix AST ordering so `CC_BATCH_PHASE3=1` is safe by default
8. **M4** — fine-grained closure `EditBuffer` + use `cc_diag_mangle_symbol` for entry names
9. **Runtime R1+** — consume serialized `.ccs.map` from compile
10. **M5.5 fallback** — only if M7/M1 turns out to need TCC-side help after all; otherwise drop. Current evidence says drop.

---

## Compiler debugging (quick reference)

| Env / flag | Effect |
|------------|--------|
| `CC_DEBUG_REPARSE=1` | Log each reparse stage name |
| `CC_DEBUG_LOWER=1` | Log lowering / edit-buffer apply steps |
| `CC_DEBUG_SPANS=1` | Log source-map insertions |
| `CC_DEBUG_DIAG=1` | Log every `cc_diag_emit` |
| `CC_DEBUG_REPARSE_DUMP_DIR=...` | Write intermediate buffers per reparse |
| `--show-lowered=<phase>` | Dump post-phase buffer (e.g. `phase3`) |
| `CC_BATCH_PHASE3=1` | Experimental batched Phase 3 collectors |
| `CC_PRE_EXPAND` | M7.A: run TCC `-E` (CPP) after text passes so all `#include` directives resolve before TCC's second-pass parse.  **Default-on (2026-05-26).**  Opt out with `CC_PRE_EXPAND=0` or `CC_PRE_EXPAND=` (empty) |
| ~~`CC_PRE_EXPAND_REPARSE=1`~~ | **Removed 2026-05-26.**  Was an opt-in CPP-expand of the FINAL reparse buffer; broke AST coord alignment with the visitor's working buffer.  Real fix requires the M1 visitor swap |
| `CC_DEBUG_PRE_EXPAND=1` | Log pre-expand attempts and TCC errors during CPP |
| `CC_DEBUG_PRE_EXPAND_DUMP=/path` | Dump the post-expand buffer to a file (M7.A debugging) |

Full list: [DEBUG_VARS.md](../src/diag/DEBUG_VARS.md).

Baseline capture: `scripts/capture_baseline.sh` → `perf/baseline_M0.txt`.
