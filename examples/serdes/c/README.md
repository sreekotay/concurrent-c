# SERDES 2-stage emit experiment

**Status: experiment — may be abandoned.** Not a commitment to replace
production `ccc`, and not a project to write a full C parser
([ARCHITECTURE NG-1](../../cc/docs/ARCHITECTURE.md): we *emit* C; TCC/host
*consume* it).

## Goal

A Concurrent-C program that:

1. **Stage 1:** pp-lex `.ccs` / `.cch` → cached `FileTape` (path-keyed, env-free)
2. **Stage 2:** splice `#include`, object-like macros, simple guards
3. **Lower + emit:** CC surface → **`.c`** or **`.h`** text
4. **Consume:** host `cc` / TCC parse that text (extra parse is intentional)

```text
.ccs / .cch bytes
    → stage 1  PpTok collect → FileTape (cached by path)
    → stage 2  splice / macros / guards / <…> passthrough
    → whitelist AST (pp_ast) + typedef oracle
         ├─ shadow_emit_c → .c
         └─ shadow_emit_h → .h
              → host cc / TCC
```

Same front for both products; H adds `#pragma once` and rejects function bodies.

## Layout (concerns)

| path | concern |
|------|---------|
| `pp_tok.rules` | Stage‑1 grammar (C pp-tokens + CC `=>` `!>` `@`) |
| `pp_tape.cch` | Stage‑1 tape cache + `file:line` diags |
| `pp_stage2.cch` | Stage‑2 cpp subset + `pp_dir` static_map |
| `pp_ast.cch` | Whitelist AST + parser |
| `pp_emit.cch` | AST → C/H text |
| `pp_lower.cch` | Thin include of ast + emit |
| `c_pp_spike.cch` | Umbrella for tools/smokes |
| `shadow_lower.{ccs,sh}` | Explicit CLI (does **not** replace `ccc`) |
| `fixtures/` | Architecture falsifiers (mid-struct include, guards, …) |
| `shadow/` | Goldens: mini, includes, `result_frag`, `hello`, recipe beachheads, `cc_exec.h` |

## Beachhead

`shadow/hello.ccs` ↔ `examples/hello.ccs`.
`shadow/recipe_result.ccs` ↔ `examples/recipe_result_error_handling.ccs`
(`T!>(E)`, `!>` / `?>`, `@string` / `@scratch`, block `@errhandler`).
`shadow/recipe_defer.ccs` ↔ `examples/recipe_defer_cleanup.ccs`
(`void!>(E)`, `@defer` / `(ok)` / `(err)`, stmt unwrap, `cc_is_err`).
`shadow/recipe_unwrap.ccs` ↔ `examples/recipe_unwrap_destroy_forms.ccs`
(`Type*` unwrap, `@destroy` / bare, chan `>`/`<`, static helpers).
`shadow/recipe_arena.ccs` ↔ `examples/recipe_arena_scope.ccs`
(value `@destroy`, `for`, arena UFCS: remaining/checkpoint/allocT/restore).
`shadow/recipe_capture.ccs` ↔ `examples/recipe_explicit_capture.ccs`
(bare blocks, block-scoped nursery `@destroy`, value/ref spawn captures).
`shadow/recipe_pipeline.ccs` ↔ `examples/recipe_channel_pipeline.ccs`
(`while`, chan send/recv, `close_on`, atomics, nested nurseries).
`shadow/recipe_timeout.ccs` ↔ `examples/recipe_timeout.ccs`
(`@with_deadline` / `as`, `while`, `name++`).
`shadow/recipe_worker.ccs` ↔ `examples/recipe_worker_pool.ccs`
(nested nurseries, chan close, capture types).
`shadow/recipe_ufcs.ccs` ↔ `examples/recipe_ufcs_forms.ccs`
(UFCS rewrite, `!>.meth()`, chains, typed slices beachhead).
`shadow/recipe_async.ccs` ↔ `examples/recipe_async_await.ccs`
(`@async` as sync fn beachhead; `@await` stripped to blocking channel ops).
`shadow/recipe_exclusive.ccs` ↔ `examples/recipe_exclusive_named.ccs`
(anonymous typedef, statics, `@create`, exclusive UFCS).
`shadow/recipe_long_lived.ccs` ↔ `examples/recipe_long_lived_store.ccs`
(`Map::[K,V]`, result helpers, store UFCS / `!>(e){…}`).
`shadow/recipe_fanout_capture.ccs` ↔ `examples/recipe_fanout_capture.ccs`
(fan-out spawn, per-task capture semantics).
`shadow/recipe_http_get.ccs` ↔ `examples/recipe_http_get.ccs`
(`@link`, global array init, `char buf[N]`, template `@string`/`@scratch`).
`shadow/recipe_tcp_echo.ccs` ↔ `examples/recipe_tcp_echo.ccs`
(`@grammar` skip, `do-while`, `break`/`continue`, expression-body spawn).
`shadow/recipe_ordered_parallel.ccs` ↔ `examples/recipe_ordered_parallel.ccs`
(`ordered` channels, `send_task` UFCS, `eprintln`).
`shadow/recipe_generics.ccs` — **shadow twin** of `examples/recipe_user_generics.ccs`
(`CC_GENERIC_FACTORY`/`@emit` is compile-time; twin shows post-instantiation
`Pair_int_double`/`Pair_int_int` surface).

```bash
./examples/serdes/c/shadow_lower.sh examples/hello.ccs -o /tmp/hello_shadow.c
./examples/serdes/c/shadow_lower.sh examples/recipe_result_error_handling.ccs -o /tmp/recipe_shadow.c
./examples/serdes/c/shadow_lower.sh examples/recipe_defer_cleanup.ccs -o /tmp/defer_shadow.c
./examples/serdes/c/shadow_lower.sh examples/recipe_unwrap_destroy_forms.ccs -o /tmp/unwrap_shadow.c
./examples/serdes/c/shadow_lower.sh examples/recipe_arena_scope.ccs -o /tmp/arena_shadow.c
./examples/serdes/c/shadow_lower.sh examples/recipe_explicit_capture.ccs -o /tmp/capture_shadow.c
./examples/serdes/c/shadow_lower.sh examples/recipe_channel_pipeline.ccs -o /tmp/pipeline_shadow.c
./examples/serdes/c/shadow_lower.sh examples/recipe_timeout.ccs -o /tmp/timeout_shadow.c
./examples/serdes/c/shadow_lower.sh examples/recipe_worker_pool.ccs -o /tmp/worker_shadow.c
./examples/serdes/c/shadow_lower.sh examples/recipe_ufcs_forms.ccs -o /tmp/ufcs_shadow.c
./examples/serdes/c/shadow_lower.sh examples/recipe_async_await.ccs -o /tmp/async_shadow.c
./examples/serdes/c/shadow_lower.sh examples/recipe_exclusive_named.ccs -o /tmp/exclusive_shadow.c
./examples/serdes/c/shadow_lower.sh examples/recipe_long_lived_store.ccs -o /tmp/long_lived_shadow.c
./examples/serdes/c/shadow_lower.sh examples/recipe_fanout_capture.ccs -o /tmp/fanout_shadow.c
./examples/serdes/c/shadow_lower.sh examples/recipe_http_get.ccs -o /tmp/http_shadow.c
./examples/serdes/c/shadow_lower.sh examples/recipe_tcp_echo.ccs -o /tmp/tcp_shadow.c
./examples/serdes/c/shadow_lower.sh examples/recipe_ordered_parallel.ccs -o /tmp/ordered_shadow.c
```

**Emit quality:** product C should read like hand-lowered code — short lines,
named temps, blocks instead of mega-line statement expressions. Source
comments and blank lines are replayed from the tape; inserted lowering
nests from each statement’s source indent (spaces/tabs). `#line N "path"`
markers at source boundaries (plus resync before `__FILE__`/`__LINE__`
err sites) map host diagnostics back to the original `.ccs`/`.cch`. Host
`cc -c` is asserted on `mini` only.

## Smokes

Experiment smokes stay out of the default driver path:

```bash
./out/cc/bin/ccc run --no-cache tests/c_pp_stage_spike_smoke.ccs
./out/cc/bin/ccc run --no-cache tests/c_pp_shadow_emit_smoke.ccs
bash examples/serdes/c/shadow/diff_lower_header.sh
bash examples/serdes/c/shadow/diff_stdlib_exec.sh
bash examples/serdes/c/shadow/diff_stdlib_async_runtime.sh
```

Compiler harvest (production, independent of this tree):

```bash
./out/cc/bin/ccc run --no-cache tests/comptime_static_map_in_header_smoke.ccs
```

## Non-goals (hard)

- Replacing production P-passes / `cc_lower_header` / TCC stub pipeline
- Full ISO cpp (`##`, function-like macros, rich `#if`)
- General C/CC AST (arbitrary exprs, full UFCS, async)
- Claiming PCH wins from stage‑1 lex cache alone
- Merging `shadow_lower` into the default `ccc` pipeline

## Kill criteria (abandon without guilt)

1. **Coverage wall** — whitelist AST costs more than text-shadowing `lower_header`
2. **Cpp wall** — real `.cch` needs function-like macros / rich `#if` first
3. **No dual use** — nothing reusable by production beyond tapes / harvest lessons
4. **Time box** — no stdlib-adjacent `.cch` shadow-diff that beats pass soup

On kill: leave this tree + smokes as a study; do not merge into the driver.

## Go criteria

Met for the trimmed `result_frag` beachhead (`CCResult_` names match
`lower_header`; include tape reuse; host `cc` clean on mini).

**Soft signal (met):** real `examples/hello.ccs` lowers via `shadow_lower`.

**Hard go (met):** shadow-lower of real stdlib headers matches production
on the normalized C surface and host `cc -c` consumes the product:

- `cc/include/ccc/cc_exec.cch` → `diff_stdlib_exec.sh`
- `cc/include/ccc/cc_async_runtime.cch` → `diff_stdlib_async_runtime.sh`
  (`const T*` returns; unlocks the same family on nursery/io)

Production lower of these headers is near-passthrough; shadow keeps the
same API with `#pragma once` + `#line` instead of the `#ifndef` guard dance.

Larger stdlib headers (`cc_result`, `cc_arena`, …) still hit the coverage /
cpp walls — grow the whitelist only when the next header is a clean
beachhead, not a general C parser.

## Working rules

- Goldens on every emit/parse change; host `cc -c` on mini products
- No default `ccc` pipeline changes from this tree
- Grow **emit/lower** whitelist, not a general C parser
- Nested stmt lists stay on `AstNode.body[]` (do not append into
  `kids_storage` while a parent list is still open)

## Explicit tool

```bash
./examples/serdes/c/shadow_lower.sh examples/serdes/c/shadow/result_frag.cch -o /tmp/x.h
./out/cc/bin/ccc run --no-cache examples/serdes/c/shadow_lower.ccs -- path.cch -o out.h
```

**P-pass “walk tape instead of rescan”:** frozen — no production scanner is
switched in this experiment.
