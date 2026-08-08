# Comptime

Implements `@comptime if` / `@comptime for` resolution, `@comptime { }` block
execution (libtcc), compiled generic factories, and the emit-plan splice layer.

## Pipeline order

The initial parse (`build_parse_input.c`) calls `cc_comptime_prepare_source()`
before comptime execution:

1. `cc__resolve_comptime_if` — expand `@comptime if/for`, prune dead branches
2. `cc__resolve_comptime_value` — hoist `@comptime(expr)` to C literals (after if/for prune)
3. `cc_rewrite_string_templates_text` — lower `@emit` / `@string` backtick templates

The emit side (`visit_codegen.c`) does not re-run `cc_comptime_prepare_source()`;
it drives comptime instantiation/splicing through the emit-plan layer
(`cc_emit_plan_splice_comptime_fragments`, `cc_emit_plan_apply_comptime_instantiations`
in `preprocess/emit_plan.{c,h}`). Keep the prepare pass order and the emit-plan layer
consistent when changing comptime ordering.

## Environment flags

| Variable | Default | Effect |
|----------|---------|--------|
| `CC_COMPTIME_UNIFIED_EXEC` | **on** (`1`) | Route `@comptime if` predicates and `@comptime for` field loads through libtcc first. Set `=0` for legacy structural-only resolution. |
| `CC_COMPTIME_EXEC_TIMEOUT_MS` | `5000` | Wall-clock limit for libtcc comptime TUs. |
| `CC_DEBUG_COMPTIME_EXEC` | off | Log libtcc errors from comptime execution. |
| `CC_DEBUG_COMPTIME_EXEC_DUMP` | — | Write the compiled comptime TU to a file path. |
| `CC_COMPTIME_NO_CACHE` | off | Disable the on-disk dylib cache for the host-cc hook path (UFCS / type-register hooks, and the generic-factory fallback). Generic factories compile in-process on libtcc and don't use this cache. |

## Shared modules

- `preprocess/template_scan.c` — backtick `${...}` scanner shared by `@string` and `@emit`
- `preprocess/comptime_prepare.c` — ordered `@comptime if/for` + template prepare pass
- `preprocess/emit_limits.h` — shared buffer caps (hard errors on overflow)
- `include/ccc/cc_emit_tpl_core.inc.cch` — single source for `@emit` append helpers + `_Generic` slot dispatch
- `comptime/emit_tpl_prelude.inc.h` — **generated** libtcc prelude (`tools/gen_emit_tpl_prelude.sh`)
- `cc_emit_tpl.cch` — user/dylib-facing header; includes `cc_emit_tpl_core.inc.cch`

Regenerate the libtcc prelude after editing the core **or** the reflect/
instantiate decls in `cc_instantiate.cch` (those live in the generator's
TAIL, not in the core):

```sh
tools/gen_emit_tpl_prelude.sh
# then commit emit_tpl_prelude.inc.h if it changed
```

## User-facing `@emit` projection

- **`@emit(\`...\`, arena)`** — returns `CCSlice` built into the caller's `CCArena*`; use in generic factories (the return form requires the arena).
- **`@emit(CCEmitAnchor, \`...\`)`** — splices at an anchor from `@comptime {}` / `@comptime for` (no arena: builds into a private stack arena, splices, frees).
- `${expr}` slots dispatch by **expression type** (`_Generic`), not variable names.
- `@comptime for` + backtick `@emit`: each unrolled iteration is wrapped in `@comptime { }` when the body contains `@emit(` with a backtick template (detected structurally, not by substring).
