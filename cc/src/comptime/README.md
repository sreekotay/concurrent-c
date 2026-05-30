# Comptime

Implements `@comptime if` / `@comptime for` resolution, `@comptime { }` block
execution (libtcc), compiled generic factories, and the emit-plan splice layer.

## Pipeline order

Both initial parse (`build_parse_input.c`) and emit (`visit_codegen.c`) call
`cc_comptime_prepare_source()` before comptime execution:

1. `cc__resolve_comptime_if` — expand `@comptime if/for`, prune dead branches
2. `cc_rewrite_string_templates_text` — lower `@emit` / `@string` backtick templates

Do not reorder or skip one pass without updating the other call site.

## Environment flags

| Variable | Default | Effect |
|----------|---------|--------|
| `CC_COMPTIME_UNIFIED_EXEC` | **on** (`1`) | Route `@comptime if` predicates and `@comptime for` field loads through libtcc first. Set `=0` for legacy structural-only resolution. |
| `CC_COMPTIME_EXEC_TIMEOUT_MS` | `5000` | Wall-clock limit for libtcc comptime TUs. |
| `CC_DEBUG_COMPTIME_EXEC` | off | Log libtcc errors from comptime execution. |
| `CC_DEBUG_COMPTIME_EXEC_DUMP` | — | Write the compiled comptime TU to a file path. |
| `CC_COMPTIME_NO_CACHE` | off | Disable dylib cache for compiled type hooks / factories. |

## Shared modules

- `preprocess/template_scan.c` — backtick `${...}` scanner shared by `@string`, `@emit`, and `cc_generic_template` expansion
- `preprocess/comptime_prepare.c` — ordered `@comptime if/for` + template prepare pass
- `preprocess/emit_limits.h` — shared buffer caps (hard errors on overflow)
- `include/ccc/cc_emit_tpl_core.inc.cch` — single source for `@emit` append helpers + `_Generic` slot dispatch
- `comptime/emit_tpl_prelude.inc.h` — **generated** libtcc prelude (`tools/gen_emit_tpl_prelude.sh`)
- `cc_emit_tpl.cch` — user/dylib-facing header; includes `cc_emit_tpl_core.inc.cch`

Regenerate the libtcc prelude after editing the core:

```sh
tools/gen_emit_tpl_prelude.sh
```

## User-facing `@emit` projection

- **`@emit(\`...\`)`** — returns `CCSlice`; use in generic factories.
- **`@emit(CCEmitAnchor, \`...\`)`** — splices at an anchor from `@comptime {}` / `@comptime for`.
- `${expr}` slots dispatch by **expression type** (`_Generic`), not variable names.
- `@comptime for` + backtick `@emit`: each unrolled iteration is wrapped in `@comptime { }` when the body contains `@emit(` with a backtick template (detected structurally, not by substring).
