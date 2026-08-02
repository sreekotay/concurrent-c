# SERDES 2-stage emit experiment

**Status: succession path (opt-in).** Beachhead metric on
`./scripts/test.sh --serdes --quick`: ~331 OK / ~812 total (~41%). Phase 1
cleanup (unify unwrap emit, cap hygiene, stub retirement, parse/emit splits,
leftover `?>` quarantine) is done; succession stays opt-in until the gate is
boring. Default `ccc` stays legacy. Opt in with:

```bash
ccc --frontend=serdes examples/hello.ccs -o /tmp/hello
# or: CC_FRONTEND=serdes ccc examples/hello.ccs -o /tmp/hello
```

`shadow_lower` is the product tool: emit text, host-cc build with emit/obj
cache under `out/.cc-build/serdes/`, or `--exe` (libtcc from the emit buffer).
Succession metric is **warm host-cc rebuild parity**, not libtcc-vs-clang.

Not a project to write a full C parser
([ARCHITECTURE NG-1](../../cc/docs/ARCHITECTURE.md): we *emit* C; host/TCC
*consume* it).

## Goal

A Concurrent-C program that:

1. **Stage 1:** pp-lex `.ccs` / `.cch` → cached `FileTape` (path-keyed, env-free)
2. **Stage 2:** splice `#include`, object-like macros, simple guards
3. **Lower + emit:** CC surface → **`.c`** or **`.h`** text
4. **Consume:** host `cc` links runtime objects; TCC reserved for comptime

```text
.ccs / .cch bytes
    → stage 1  tape (toks + comment spans)
    → stage 2  stitch (include / object-like #define / guards)  ← upfront, closed
    → whitelist AST (emit + diags + safety — not a compiler IR)
         ├─ shadow_emit_c → .c  → host cc + concurrent_c.o  → run
         └─ shadow_emit_h → .h  → host cc -c
    comptime: minimal TCC on product / factory path, not the lowerer
```

The AST is for **clean emission**, **errors** (`path:line:col` on user source),
and **safety analysis** — not compilation. Host `cc` compiles the emitted C.

Same front for both products; H adds `#pragma once` and rejects function bodies.

## Layout (concerns)

| path | concern |
|------|---------|
| `pp_tok.rules` | Stage‑1 grammar (C pp-tokens + CC `=>` `!>` `@`) |
| `pp_tape.cch` | Stage‑1 tape cache + `file:line` diags |
| `pp_stage2.cch` | Stage‑2 cpp subset + `pp_dir` static_map |
| `pp_ast.cch` | AST umbrella → core + parse |
| `pp_ast_core.cch` | Keywords, AstNode, Parser, spell helpers |
| `pp_ast_parse_stmt.cch` | Control-flow parsers + `parse_stmt` dispatch |
| `pp_ast_parse_unwrap.cch` | Unwrap / bang / result-local / ptr unwrap |
| `pp_ast_parse_spawn.cch` | Spawn / send_task / closure / capture infer |
| `pp_ast_parse_ext.cch` | External / TU parsers + `parse_tu` |
| `pp_emit.cch` | Emit umbrella → core + ufcs + stmt |
| `pp_emit_core.cch` | CEmit, bind/chan tables, `#line` / lead; leftover `?>` API |
| `pp_emit_ufcs.cch` | `lower_parts` + leftover peel (not a pipeline) |
| `pp_emit_unwrap.cch` | Result / bang / qmark / try_assign emit |
| `pp_emit_spawn.cch` | Closure make, spawn collect, defer epilogue |
| `pp_emit_async.cch` | `@async` poll-task beachhead |
| `pp_emit_tu.cch` | TU product (typedefs, Result specs, file switch) |
| `pp_emit_stmt.cch` | Stmt switch + destroy helpers |
| `pp_lower.cch` | Thin include of ast + emit |
| `c_pp_spike.cch` | Umbrella for tools/smokes |
| `shadow_lower.ccs` | Product CLI (emit / host-cc+cache / `--exe`) |
| `shadow_build.cch` | Emit/obj cache + host-cc link (ccc-compatible flags) |
| `shadow_tcc_compile.{c,h}` | Emit buffer → libtcc `--exe` (no .c on disk) |
| `fixtures/` | Architecture falsifiers (mid-struct include, guards, …) |
| `shadow/` | Goldens: mini, includes, frags, `hello`, trimmed recipe smoke twins, `cc_exec.h` |

## Beachhead

Behavioral recipes gate on real `examples/recipe_*.ccs` via
`scripts/test_serdes_shadow.sh` — not shadow twins.

Smoke goldens keep a trimmed twin set only:
`shadow/hello.ccs` ↔ `examples/hello.ccs`.
`shadow/recipe_result.ccs` ↔ `examples/recipe_result_error_handling.ccs`
(`T!>(E)`, `!>` / `?>`, `@string` / `@scratch`, block `@errhandler`).
`shadow/recipe_ordered_parallel.ccs` ↔ `examples/recipe_ordered_parallel.ccs`
(`ordered` channels, `send_task` UFCS, `eprintln`).
`shadow/recipe_generics.ccs` ↔ `examples/recipe_user_generics.ccs`
(`CC_GENERIC_FACTORY` + `@emit(`…`)` instantiation).
`shadow/error_face_frag.cch` — trimmed `CCError` face from `cc_result`
(`CCErrorKind`, brace init, `static inline const char*`, long `switch` bodies).
`shadow/io_error_frag.cch` — trimmed `CCIoError` on that face (`#include`
splice, `/*@as*/`, `const char*` helpers, errno `switch`; no `_Generic` /
`CC_DECL_RESULT_SPEC`).

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
bash scripts/test_serdes.sh          # full parallel-path gate (preferred)
# or piecemeal:
./out/cc/bin/ccc run --no-cache tests/c_pp_stage_spike_smoke.ccs
./out/cc/bin/ccc run --no-cache tests/c_pp_shadow_emit_smoke.ccs
bash scripts/test_serdes_shadow.sh   # stdlib diffs + recipes + seam only
bash examples/serdes/c/shadow/diff_lower_header.sh
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
  (`const T*` returns)
- `cc/include/ccc/cc_nursery.cch` → `diff_stdlib_nursery.sh`
  (`const struct T*` protos; long static-inline param spans; comptime
  `#ifdef` normalized away — production blanks the body, shadow omits it)
- `cc/include/ccc/cc_chan_handle.cch` → `diff_stdlib_chan_handle.sh`
  (forward `struct Tag;`, `struct Tag*` fields, one-line anon typedefs;
  nested `__CC_CHAN_*_DEFINED` guards normalized away)
- `cc/include/ccc/cc_io_error.cch` → `diff_stdlib_io_error.sh`
  (`#define` passthrough, `_Generic` macro, `CC_DECL_RESULT_SPEC` raw line,
  `#if defined` inside `switch` via body span; host-cc unwrap helpers on `.c`)
- `cc_result` CCError face → `diff_stdlib_result_face.sh`
  (`error_face_frag.cch` through `cc_error_exit`; full `cc_result.cch`
  still hits Result-macro / cpp wall)

**Behavioral (`.ccs` product):** `run_recipes_shadow.sh` — shadow-lower
≥10 beachhead recipes (hello, result, arena, capture, timeout, defer,
unwrap, pipeline, worker, fanout, exclusive, async, ufcs), host-`cc` +
link `concurrent_c.o`, assert output. (`run_hello_shadow.sh` remains a
minimal subset.)

**Hosting seam:** tracked driver is `cc/scripts/shadow_lower.sh` (installed
to `cc/bin/shadow_lower` / `out/cc/bin/shadow_lower` by the Makefile).
`ccc --shadow-lower` / `ccc --shadow-run` are optional opt-in entry
points. Default `ccc` pipeline is unchanged. `run_via_seam.sh` exercises
shadow_lower → host `cc` + `concurrent_c.o`.

Production lower of these headers is near-passthrough; shadow keeps the
same API with `#pragma once` + `#line` instead of the `#ifndef` guard dance.

Larger stdlib headers (`cc_arena`, full `cc_result`, …) still hit coverage /
cpp walls. Trimmed frags cover enum + `@as` + `switch` + `const T*` returns;
grow the whitelist only when the next header is a clean beachhead, not a
general C parser. Keep pushing this parallel path; do not force a merge
into the TCC-heavy driver until the transform boundary is boring.

## How to test

| command | what |
|---------|------|
| `./scripts/test.sh` | Production `ccc` only — does **not** run the shadow lowerer |
| `./scripts/test_serdes.sh` | SERDES parallel path: stage/emit smokes + stdlib hard-go + recipes |

`c_pp_*` smokes under `tests/` are skipped by default `cc_test` unless
`CC_TEST_SERDES=1` or `--filter c_pp_`.

## Working rules

- Goldens on every emit/parse change; host `cc -c` on mini products
- Default `ccc` stays legacy; opt-in via `--frontend=serdes` / `CC_FRONTEND=serdes`
- Grow **emit/lower** whitelist, not a general C parser
- Nested stmt lists stay on `AstNode.body[]` (do not append into
  `kids_storage` while a parent list is still open)
- Fair bench: warm vs warm host-cc (`shadow/bench_vs_ccc.sh`)

## Direction (real shape — zero text mangling)

Target:

```text
bytes → stage1 tape (toks + comment spans)
     → stage2 stitch (include / #define / guards)   ← upfront, closed
     → whitelist AST (types + Call + sticky trivia + type map)
     → emit C once (#line / err_at → original .ccs/.cch)
     → cache (out/.cc-build/serdes/<fp>/) → host cc + link
       (or --exe: libtcc from emit buffer)
```

- **Stitch early.** Macros/defines/includes before AST; never re-expand in emit.
- **Trivia sticky.** Lead comments/`#line` from `tok_off`/`file_id` attached at
  parse; emit only prints — no comment recovery after string rewrite.
- **Zero post-parse mangling on the CC surface.** Structured types +
  `AST_UFCS_*` (table emit via `shadow_ufcs_lower_parts`); Map/Vec/`char[:]`,
  `@await`/`@create` spelled at parse; channel_pair / return `!>` / cond
  shapes handled at emit sites. No `shadow_lower_expr_beachhead` pipeline.
  Leftover text (mainly `@string` chains) peels left-to-right through
  `shadow_ufcs_peel_left` → `lower_parts` — not a second IR soup.
- **Opaque C ≠ mangling.** Already-C blobs (switch cases, enum lists,
  `AST_RAW_LINE`, unparsed static-fn bodies) pass through as text. That is
  product policy, not CC sugar rewrite. `SHADOW_RAW_BODY_REWRITE` defaults
  **off** (opaque copy); set to `1` only for an explicit legacy fallback.
  `scripts/test_serdes.sh` asserts the default stays `0` and deleted mangling
  helpers stay gone.
- AST grows only for emit, diags, and safety — not a general C compiler IR.
  `pp_ast*` / `pp_emit*` are split by concern for readability; umbrellas keep
  include order for tools.
- Recipe twins deleted except smoke goldens; recipes behavioral via
  `scripts/test_serdes_shadow.sh`. Do not weaken stdlib hard-go or comptime.

## Next gaps (after cleanup)

Prioritized by fail mass × language value on serdes-quick:

1. **Safety / diag oracles** — retired `@`, sync `@await`, bare `!>;`, slice move /
   unique provenance, arena borrow, unwrap diverge / unhandled-result, `@err`
   forward/deadcode, closure ref/alias mutation, `@as` pointer/dup/ambiguous,
   ordered-tx, async bare `chan_send`/`chan_recv`, channel send pointer-field /
   non-stable slice (`pp_ast_safety.cch`); still missing variant/comptime /
   grammar/type_of (~18 silent-accept)
2. **`@comptime` / `@emit`** — blocks still comment-stripped (factory sugar works)
3. **`@variant` AST + emit** — no whitelist surface yet
4. **Real `@async` / `@await` SM** — replace poll-wrapper beachhead
5. UFCS leftovers / `@as` forwarding; slice provenance; `@string` templates

## Explicit tool

```bash
./out/cc/bin/shadow_lower examples/serdes/c/shadow/result_frag.cch -o /tmp/x.h
./out/cc/bin/shadow_lower examples/serdes/c/shadow/io_error_frag.cch -o /tmp/io_error.h
./out/cc/bin/shadow_lower examples/serdes/c/shadow/error_face_frag.cch -o /tmp/error_face.h
./out/cc/bin/ccc --shadow-lower path.cch -o out.h
bash examples/serdes/c/shadow/run_via_seam.sh examples/hello.ccs
```

**P-pass “walk tape instead of rescan”:** frozen — no production scanner is
switched in this experiment.
