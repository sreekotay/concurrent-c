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
