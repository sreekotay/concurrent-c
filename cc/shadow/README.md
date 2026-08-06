# `shadow_lower` sources (native front)

Product sources for the default `ccc` front live here (`cc/shadow/`).
Wire SERDES demos remain under `examples/serdes/{json,resp}`.

**Status: default `ccc` front**. Metric on `./scripts/test.sh --quick` /
`./scripts/test.sh --native`. Focused `scripts/test_shadow.sh` is
architectural smoke; `scripts/test_shadow_real_projects.sh` covers redis /
pigz / levenshtein. The tape → whitelist-AST → tape-fallback spine stays;
active hardening (not changing that spine):

1. **UFCS peel kill** — typed/instance receivers diagnose on structured miss;
   no `Map_CCSliceHdr_int_*` invent; leftover peel only for unbound/opaque.
2. **Stage-2 exhaustive directives** — implement / passthrough-by-design /
   hard error; `#if` never guesses the true arm; indented `#` recognized.
3. **ccc↔shadow_lower options contract** — forward release/debug/flags/target/
   sysroot/no-runtime/dry-run; driver handles `build.cc` / `-D` / dumps /
   `--compile` (host `-D` forward).

Default `ccc` is native. Opt out with `--frontend=legacy` only for archaeology.

`shadow_lower` is the product tool: emit text, host-cc build with emit/obj
cache under `out/.cc-build/native/`, or `--exe` (libtcc from the emit buffer).
Succession metric is **warm host-cc rebuild parity**, not libtcc-vs-clang.

Bootstrap snapshots (deliberate freezes of lowered C + local headers) live
under `cc/bootstrap/shadow_lower/` — see that README for the full
edit → `SHADOW_LOWER_SOURCE=ccs` → snapshot → promote loop. Default
`make -C cc` host-ccs `last-good`. Do **not** hand-edit committed `vN/`
trees; do **not** hand-copy `*.cch` into `out/include/cc/shadow/*.h`
(those headers are snapshot/bootstrap products). A `.cch` fix is not in the
seed until promote flips `last-good` to a new `vN`.

## Spine

Full constraints/ADRs: [ARCHITECTURE.md](../../cc/docs/ARCHITECTURE.md).


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
`scripts/test_shadow_recipes.sh` — not shadow twins.

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
./cc/shadow/shadow_lower.sh examples/hello.ccs -o /tmp/hello_shadow.c
./cc/shadow/shadow_lower.sh examples/recipe_result_error_handling.ccs -o /tmp/recipe_shadow.c
./cc/shadow/shadow_lower.sh examples/recipe_defer_cleanup.ccs -o /tmp/defer_shadow.c
./cc/shadow/shadow_lower.sh examples/recipe_unwrap_destroy_forms.ccs -o /tmp/unwrap_shadow.c
./cc/shadow/shadow_lower.sh examples/recipe_arena_scope.ccs -o /tmp/arena_shadow.c
./cc/shadow/shadow_lower.sh examples/recipe_explicit_capture.ccs -o /tmp/capture_shadow.c
./cc/shadow/shadow_lower.sh examples/recipe_channel_pipeline.ccs -o /tmp/pipeline_shadow.c
./cc/shadow/shadow_lower.sh examples/recipe_timeout.ccs -o /tmp/timeout_shadow.c
./cc/shadow/shadow_lower.sh examples/recipe_worker_pool.ccs -o /tmp/worker_shadow.c
./cc/shadow/shadow_lower.sh examples/recipe_ufcs_forms.ccs -o /tmp/ufcs_shadow.c
./cc/shadow/shadow_lower.sh examples/recipe_async_await.ccs -o /tmp/async_shadow.c
./cc/shadow/shadow_lower.sh examples/recipe_exclusive_named.ccs -o /tmp/exclusive_shadow.c
./cc/shadow/shadow_lower.sh examples/recipe_long_lived_store.ccs -o /tmp/long_lived_shadow.c
./cc/shadow/shadow_lower.sh examples/recipe_fanout_capture.ccs -o /tmp/fanout_shadow.c
./cc/shadow/shadow_lower.sh examples/recipe_http_get.ccs -o /tmp/http_shadow.c
./cc/shadow/shadow_lower.sh examples/recipe_tcp_echo.ccs -o /tmp/tcp_shadow.c
./cc/shadow/shadow_lower.sh examples/recipe_ordered_parallel.ccs -o /tmp/ordered_shadow.c
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
bash scripts/test_shadow.sh          # full parallel-path gate (preferred)
# or piecemeal:
./out/cc/bin/ccc run --no-cache tests/c_pp_stage_spike_smoke.ccs
./out/cc/bin/ccc run --no-cache tests/c_pp_shadow_emit_smoke.ccs
bash scripts/test_shadow_recipes.sh   # stdlib diffs + recipes + seam only
bash cc/shadow/shadow/diff_lower_header.sh
```

Compiler harvest (production, independent of this tree):

```bash
./out/cc/bin/ccc run --no-cache tests/comptime_static_map_in_header_smoke.ccs
```

## Non-goals (hard)

- Full ISO cpp (`##`, function-like macros, rich `#if`)
- General C/CC AST (arbitrary exprs, full UFCS, async)
- Claiming PCH wins from stage‑1 lex cache alone

## Historical kill criteria (superseded — front shipped)

1. **Coverage wall** — whitelist AST costs more than text-shadowing `lower_header`
2. **Cpp wall** — real `.cch` needs function-like macros / rich `#if` first
3. **No dual use** — nothing reusable by production beyond tapes / harvest lessons
4. **Time box** — no stdlib-adjacent `.cch` shadow-diff that beats pass soup

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
points. Default `ccc` delegates to `shadow_lower`. `run_via_seam.sh` exercises
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
| `./scripts/test.sh` | Default suite via native / `shadow_lower` |
| `./scripts/test.sh --legacy` | Multipass text-rewrite front |
| `./scripts/test_shadow.sh` | Architectural smoke: stage/emit + stdlib hard-go + recipes |

`c_pp_*` smokes under `tests/` are skipped by default `cc_test` unless
`CC_TEST_SHADOW=1` or `--filter c_pp_`.

## Working rules

- Goldens on every emit/parse change; host `cc -c` on mini products
- Default `ccc` is native; opt out with `--frontend=legacy` / `CC_FRONTEND=legacy`
- Grow **emit/lower** whitelist, not a general C parser
- Nested stmt lists stay on `AstNode.body[]` (do not append into
  `kids_storage` while a parent list is still open)
- Fair bench: warm vs warm host-cc (`shadow/bench_vs_ccc.sh`)

## Direction (zero text mangling on the CC surface)

See also ARCHITECTURE.md. Target:

```text
bytes → stage1 tape (toks + comment spans)
     → stage2 stitch (include / #define / guards)   ← upfront, closed
     → whitelist AST (types + Call + sticky trivia + type map)
     → emit C once (#line / err_at → original .ccs/.cch)
     → cache (out/.cc-build/native/<fp>/) → host cc + link
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
  `scripts/test_shadow.sh` asserts the default stays `0` and deleted mangling
  helpers stay gone.
- AST grows only for emit, diags, and safety — not a general C compiler IR.
  `pp_ast*` / `pp_emit*` are split by concern for readability; umbrellas keep
  include order for tools.
- Recipe twins deleted except smoke goldens; recipes behavioral via
  `scripts/test_shadow_recipes.sh`. Do not weaken stdlib hard-go or comptime.

## Next gaps (after cleanup)

Prioritized by fail mass × language value on native-quick:

1. **Safety / diag oracles** — move/channel/unwrap/`T[:!]`/`@variant`/anon
   `@as`/`type_of`/unproven-pointer free refuse when unprovable; remaining:
   dominating-check allowlists for safe variant projections + richer points-to
2. **`@comptime` / `@emit`** — product path runs legacy prepare/exec/splice via
   `shadow_comptime.c` + `libshadow_comptime.a` (not a `pp_emit*` VM). Stage1
   gets original spelling with `@comptime if`/value resolved and `@comptime`
   blocks blanked (keeps `CC_GENERIC_FACTORY` for whitelist instantiate).
   Remaining holes: compiled-factory dylib path, type-register/UFCS comptime,
   header-local static_map, reflect factories (~24 comptime fails)
3. **`@variant` AST + emit** — no whitelist surface yet (~4 ebf + ~20 bf)
4. **Real `@async` / `@await` SM** — replace poll-wrapper beachhead.
   - Landed: `@await tx.send` / `rx.recv` → sync `bool !>(CCIoError)` via
     `cc_chan_result_from_errno` at the await edge (recipe + UFCS smokes).
   - Landed: `@async T!>(E)` — body returns `CCResult_*`; poll packs via
     `malloc` (POLL tasks have no fiber `result_buf`); `@await` copies out and
     frees. Smokes: `tests/async_result_return_smoke.ccs`,
     `examples/recipe_async_await.ccs`.
   - Still open: replace the poll-wrapper beachhead with a real state machine.
5. **UFCS / `@string` / `@scratch`**
   - Landed beachhead: `shadow_ufcs_fmt_call` builds call spellings with
     `@string(\`…\`, @scratch)` + `cc_arena_reset` after copy into `dst`;
     wired on invent / scalar / Result-value hop. Call-local
     `println(@string(…, @scratch))` checkpoints/restores the shared scratch
     (bound `@string(..., @scratch)` stays valid). Spec §9.1.4 call-local
     reclaim. Smoke: `tests/scratch_call_local_restore_smoke.ccs`.
   - Grow `fmt_call` to more call-shaped invent sites; leave cold `snprintf`
     ladders alone until touched.
   - Fixed-to-`cap` invent waist (fail loud on overflow; no growable spike).
   - Assignment / ternary `@string(..., @scratch)` must inject
     `__cc_str_scratch` (today only decl-init does).
   - Leftover-text `cc_println(({…@scratch…}))` reclaim (structured
     `AST_PRINTLN_TPL` only so far).
   - Standalone re-lower of `pp_emit_ufcs.cch` hangs — iterate via
     `out/include` face patch (local build aid) or umbrella until fixed;
     then snapshot/promote a **new** `vN` so it sticks in `last-good`.
     Never hand-patch an existing bootstrap `vN/` instead of promoting.
   - `@as` forwarding; remaining UFCS leftover peel kill.

## Explicit tool

```bash
./out/cc/bin/shadow_lower cc/shadow/shadow/result_frag.cch -o /tmp/x.h
./out/cc/bin/shadow_lower cc/shadow/shadow/io_error_frag.cch -o /tmp/io_error.h
./out/cc/bin/shadow_lower cc/shadow/shadow/error_face_frag.cch -o /tmp/error_face.h
./out/cc/bin/ccc --shadow-lower path.cch -o out.h
bash cc/shadow/shadow/run_via_seam.sh examples/hello.ccs
```

The legacy multipass scanners under `cc/src/visitor/` are not being ported
onto the tape; new language work lands in this tree.
