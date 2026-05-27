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

Every fiber spawned via `n->spawn_async(callee(...))` carries the user-
facing callee name + spawn-site `(file, line)`, so any code running on
that fiber can answer "what async task am I?" via
`cc_rt_diag_current_async_info`.

| Layer | Mechanism |
|-------|-----------|
| Per-fiber storage | `fiber_v2` gains `diag_user_name`, `diag_file`, `diag_line`. Cleared on alloc/recycle so pooled fibers never inherit the previous task's name. Setter/getter in `sched_v2.c` (`sched_v2_fiber_set_diag_name`, `sched_v2_fiber_get_diag_name`). |
| Spawn path | `cc_nursery_spawn_async_named(n, task, name, file, line)` populates an extended `cc_nursery_async_spawn` struct; `cc__nursery_async_runner` stamps the fiber on first entry (so `sched_v2_current_fiber()` returns the new fiber). Anonymous `cc_nursery_spawn_async` still works and delegates with NULL/0 metadata. |
| Spawn-site lowering | UFCS rewrite in `preprocess.c` for `n->spawn_async(callee(...))` emits `cc_nursery_spawn_async_named(n, callee(...), "callee", __FILE__, __LINE__)`. Callee extracted from args via leading-identifier scan; opaque expressions fall back to `"<async>"`. `__FILE__`/`__LINE__` resolve to the user's source location via the `#line` directives CC emits. |
| User API | `cc_rt_diag_current_async_info(&name, &file, &line)` — forward-declared in `cc_nursery.cch`. Reads the running fiber's slot, falls back to the process-global `g_last_async` when called outside fiber context. Must be invoked as `@noblock cc_rt_diag_current_async_info(...)` inside `@async` bodies to bypass autoblock wrapping (which would route the call through a worker thread where `sched_v2_current_fiber()` is NULL). |
| Smoke | `tests/runtime/r1_async_name_smoke.ccs` — two named tasks; verifies each fiber sees its own name + file + line, and that `main` (no fiber) gets a truthful "no naming info" response. |

**Known limitations**:

- Names live as long as the underlying C string literals embedded in the
  lowered output (program lifetime in practice), so no copy / no
  allocation on the spawn path.
- Non-`spawn_async` fiber births (closure spawn via `n->spawn(...)`,
  legacy `cc_nursery_spawn`, the orphan `pass_nursery_spawn_ast.c`) do
  not yet stamp names — those fibers report `"no info"` to the query.
  Closures don't have a single user-visible name; threading per-closure
  source markers is a follow-up.
- The query requires `@noblock` at the call site inside `@async`
  bodies. A future iteration could move the decoration into the prelude
  declaration once the `.cch` → `.h` lowering preserves CC sigils for
  pass_autoblock to consume.
