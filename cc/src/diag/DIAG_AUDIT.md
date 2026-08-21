# Diagnostic quality audit

**Original audit:** 2026-05-26 (M0)  
**Status:** [COMPILER_CLEANUP_STATUS.md](../../docs/COMPILER_CLEANUP_STATUS.md)

---

## Done (M0.5 infrastructure)

| Item | Location |
|------|----------|
| `cc_diag_emit` / `cc_diag_emitf` | `cc/src/diag/diag.c` |
| `cc_diag_translate_tcc_error` | `cc/src/diag/diag.c` |
| `CCSourceMap` registry | `cc/src/diag/source_map.c` |
| `CCEditBuffer` origin spans | `cc_edit_buffer_add_ex`, `cc_edit_buffer_register_spans` |
| Lowered-symbol naming API | `cc/src/diag/mangle.c` |
| `CC_DEBUG_*` env vars | `cc/src/diag/DEBUG_VARS.md` |
| `--show-lowered=<phase>` | `cc_main.c` |
| Driver funnel on failure | `driver.c` — `cc_diag_init`, `cc_diag_print_all` |

Invariants I1–I8 are **defined**; full enforcement depends on per-pass span migration and the diagnostic test battery (not yet landed).

---

## Remaining (backlog)

### Raw TCC error leakage

| Location | Issue | Fix |
|----------|-------|-----|
| `tcc_bridge.c` | TCC errors not always routed through `cc_diag_translate_tcc_error` | Wire `tcc_ext_set_diag_callback` + map |
| `cc__report_reparse_failure` | Internal stage + `/tmp/cc_reparse_fail_*.c` dump | Map dump path; optional user-facing hint |
| Host C compile | `#line` not on every rewrite | Audit emit paths; extend `#line` emission |

### Span loss points

| Phase | Risk | Fix |
|-------|------|-----|
| Text preprocess | Line shifts | Record spans in `cc_build_parse_input` source map |
| Phase 3 reparses | AST/source desync | Thread `CCSourceMap` through `visit_codegen` |
| Closure literal lift | Whole-file replacement | M4 fine-grained edits + origin spans |
| Async state machine | `__cc_async_*` names | M4 + runtime R1 |

### Test battery (not landed)

Planned under `tests/diag/` with `// EXPECT-DIAG:` harness — see plan diagnostic-fidelity battery. None in CI yet.

### `#line` verification

M0.5 spike: add `tests/diag/line_directive_smoke.ccs` to CI when host-compile path is stable.

---

## Macro diagnostics (I9 — M5.5)

Requires `cc_macro_recognizer` token synthesis + TCC `macro_stack` in `cc_diag_translate_tcc_error`. Hooks registered; synthesis pending.

---

## Runtime R3 — `!>` source-location propagation chain (landed 2026-05-27)

Every `!>` propagation site pushes `(file, line)` onto a process-global
ring buffer at runtime, so an `@errhandler` (or any code catching a
propagated error) can walk the chain and show the user *where* the
error trickled through — not just the original cc_err message.

| Layer | Mechanism |
|-------|-----------|
| Macro hook | `__cc_uw_err_at` in `cc_result.cch` calls `cc_rt_diag_record_unwrap_site` via the comma operator on every `_Generic` arm (baseline + per-TU enumerated). |
| Typed-callee fast path | `cc__ru_emit_uw_err_binder` emits an explicit `cc_rt_diag_record_unwrap_site(file, line);` before the direct `(tmp).u.error` field access, so the optimization that bypasses `__cc_uw_err_at` still records. |
| Runtime storage | Fixed-size ring buffer (16 entries by default, `CC_RT_DIAG_UNWRAP_CHAIN_MAX`) in `cc/runtime/cc_rt_diag.c`; oldest entries drop on overflow with a counter surfaced via `cc_rt_diag_print_unwrap_chain`. |
| User API | `cc_rt_diag_unwrap_chain_len`, `cc_rt_diag_unwrap_site(i, &f, &l)`, `cc_rt_diag_print_unwrap_chain(fp)`, `cc_rt_diag_clear_unwrap_chain` (all forward-declared via the prelude). |
| Smoke | `tests/runtime/r3_unwrap_chain_smoke.ccs` — 2-deep `!>` chain. |

**Known limitation**: storage is a process-global, not `_Thread_local`.
Multi-OS-thread programs will interleave entries. Iterate to TLS once
we have a real multi-thread regression. Single-thread fiber programs
(the common case) get the natural chronological chain.

---

## Runtime R1 — async backtrace naming (landed 2026-05-27)

Every fiber spawned via `n.spawn_async(callee(...))` carries the user-
facing callee name + spawn-site `(file, line)`, so any code running on
that fiber can answer "what async task am I?" via
`cc_rt_diag_current_async_info`.

| Layer | Mechanism |
|-------|-----------|
| Per-fiber storage | `fiber_v2` gains `diag_user_name`, `diag_file`, `diag_line`. Cleared on alloc/recycle so pooled fibers never inherit the previous task's name. Setter/getter in `sched_v2.c` (`sched_v2_fiber_set_diag_name`, `sched_v2_fiber_get_diag_name`). |
| Spawn path | `cc_nursery_spawn_async_named(n, task, name, file, line)` populates an extended `cc_nursery_async_spawn` struct; `cc__nursery_async_runner` stamps the fiber on first entry (so `sched_v2_current_fiber()` returns the new fiber). Anonymous `cc_nursery_spawn_async` still works and delegates with NULL/0 metadata. |
| Spawn-site lowering | UFCS rewrite in `preprocess.c` for `n.spawn_async(callee(...))` emits `cc_nursery_spawn_async_named(n, callee(...), "callee", __FILE__, __LINE__)`. Callee extracted from args via leading-identifier scan; opaque expressions fall back to `"<async>"`. `__FILE__`/`__LINE__` resolve to the user's source location via the `#line` directives CC emits. |
| User API | `cc_rt_diag_current_async_info(&name, &file, &line)` — forward-declared in `cc_nursery.cch`. Reads the running fiber's slot, falls back to the process-global `g_last_async` when called outside fiber context. Must be invoked as `@noblock cc_rt_diag_current_async_info(...)` inside `@async` bodies to bypass autoblock wrapping (which would route the call through a worker thread where `sched_v2_current_fiber()` is NULL). |
| Smoke | `tests/runtime/r1_async_name_smoke.ccs` — two named tasks; verifies each fiber sees its own name + file + line, and that `main` (no fiber) gets a truthful "no naming info" response. |

**Known limitations**:

- Names live as long as the underlying C string literals embedded in the
  lowered output (program lifetime in practice), so no copy / no
  allocation on the spawn path.
- Non-`spawn_async` fiber births (closure spawn via `n.spawn(...)`,
  legacy `cc_nursery_spawn`) do not yet stamp names — those fibers
  report `"no info"` to the query.
  Closures don't have a single user-visible name; threading per-closure
  source markers is a follow-up.
- The query requires `@noblock` at the call site inside `@async`
  bodies. A future iteration could move the decoration into the prelude
  declaration once the `.cch` → `.h` lowering preserves CC sigils for
  pass_autoblock to consume.

---

## Runtime R2 — channel deadlock diagnostic text (landed 2026-05-27)

Every channel created via the lowered `cc_channel_pair(&tx, &rx)` path
carries the user-facing handle-pair name (`"tx,rx"`) + creation-site
`(file, line)` on the `CCChan` struct.  The deadlock detector quotes
the metadata in its dump banner, and the same data is exposed to user
code via `cc_rt_diag_channel_meta`.

| Layer | Mechanism |
|-------|-----------|
| Per-channel storage | `CCChan` gains `diag_user_name`, `diag_file`, `diag_line`.  Zero-initialized by the channel-create memset.  Setter `cc_chan_set_diag_meta` is a single write — no lock; written exactly once on creation before any sender/receiver can see the channel.  Getter `cc_chan_get_diag_meta` is in `channel.c` where the layout is visible. |
| Create-site lowering | `pass_channel_syntax.c::cc__rewrite_channel_pair_calls_text` now emits the inline helper `cc_channel_pair_create_named(..., "tx,rx", __FILE__, __LINE__)` (defined in `cc_channel.cch`) instead of bare `cc_channel_pair_create(...)`.  The helper folds the create + diag-meta stamp into a single call — keeps the lowered C hand-crafted-looking and sidesteps the `@async`-frame-promotion collision that a statement-expression wrapper would trigger (locals declared inside `({ ... })` inside an `@async` body get rewritten to `__f->`-prefixed field accesses, which is invalid C for declarations). |
| Owned channel | `T[~N >] var @owned(...)` lowering also emits `cc_chan_set_diag_meta(var, "var", __FILE__, __LINE__)` right after `cc_chan_create_owned`, so single-handle owned channels are named too. |
| Deadlock dump | `sched_v2_dump_parked_fibers_for_verdict` (called from `sched_v2_check_deadlock`) now prints two extra lines per channel-parked fiber: `chan user: name=… site=…:…` (the R2 channel meta) and `task user: name=… site=…:…` (the R1 fiber name).  Channels created via the raw `cc_chan_create` API are silently skipped — the API truthfully says "no meta". |
| User API | `cc_rt_diag_channel_meta(ch, &name, &file, &line)` — forward-declared in `cc_channel.cch`.  Returns 1 if any field was populated, 0 otherwise; safe to pass NULL `ch`. |
| Smoke | `tests/runtime/r2_channel_meta_smoke.ccs` — verifies the meta round-trips through both `tx.raw` and `rx.raw` views; verifies raw `cc_chan_create` and NULL inputs both report "no info" truthfully.  Manual verification of the deadlock banner: build any program with an `@async` recv on an empty channel and run with `CC_DEADLOCK_ABORT=0` — the dump shows both `chan user:` and `task user:` lines. |

**Known limitations**:

- Strings are caller-owned C literals (lifetime = program), same model as R1.
- Channels created via the raw `cc_chan_create*` runtime API (no
  lowered path) report "no meta".  The CC ecosystem prefers the
  `T[~N >]` syntax; C-style direct creation is a deliberate
  back-compat surface.
- R2 reports the creation site, not the send/recv site that's stuck.
  Pairing the channel meta with the per-fiber R1 task name (both now
  in the deadlock dump) gives the reader both endpoints: "task `X`
  spawned at A:B is parked on channel `Y` created at C:D".  Surfacing
  the exact `cc_chan_send`/`cc_chan_recv` source location is R4/R5
  follow-up work.
