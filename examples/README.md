# Examples

Demonstrations of Concurrent-C features and patterns.

## Quick Start

```bash
# Build and run any example
./cc/bin/ccc run examples/hello.ccs      # three named siblings (.ccs)
./cc/bin/ccc run examples/hello.shcc     # one-liner script (.shcc)
```

## Learning Path (Recommended Order)

New to Concurrent-C? Work through these in order:

| # | File | Concept | What you'll learn |
|---|------|---------|-------------------|
| 1  | `hello.ccs` | First `@parallel` | Three named siblings; `@parallel spawn`; `.wait()` joins |
| 2  | `recipe_result_error_handling.ccs` | Results & `@errhandler` | `?>` : `E → T`; `!>` : `E →` control flow; `(e)` exposes; bare `!>` routes |
| 3  | `recipe_unwrap_destroy_forms.ccs` | Unwrap shape | Same two ops × modifiers; `@destroy` on successful construction |
| 3a | `recipe_variant.ccs` | `@variant` | Tagged data (not Result); construct / `@switch` / `case .arm(bind):` / `?>` / `!>` |
| 4  | `recipe_ufcs_forms.ccs` | UFCS shape | One dispatch rule × spellings (families, bare-name, fallible chains) |
| 4a | `recipe_user_generics.ccs` | Generics | `Name::[args]` + `CC_GENERIC_FACTORY` — same rule as Vec/Map |
| 5  | `recipe_parallel.ccs` | `@parallel` | Join, `@serial`, dest, `@parallel for` (disjoint), `wait` + `@stage` (shared ticket) |
| 5c | `recipe_parallel_forms.ccs` | `@parallel` shape | One join + one dest × modifiers; dest bodies: `!>` inside, `return;`, `h.fail(e)` |
| 5a | `recipe_parallel_stream.ccs` | On-page stream | `@parallel spawn`; `tx.close()` next to produce; not a nursery |
| 5b | `recipe_parallel_empty.ccs` | Dest EMPTY-close | Consumer already in `recv`; inner dest `h.close(tx)` + `@destroy` / `leave` |
| 6  | `recipe_explicit_capture.ccs` | Capture | `@parallel` is the frame (no list); closure lists on `n.spawn` / `send_task` |
| 7  | `recipe_channel_pipeline.ccs` | EMPTY-close | Nested bag; `n.close(tx)` when the set is not on the page |
| 8  | `recipe_async_await.ccs` | Async/Await | `@async` call stacks vs `spawn`, `@await`, composition |
| 9  | `recipe_timeout.ccs` | Deadlines & cancel | `@with_deadline`; live dest `h.cancel()`; siblings poll `h.cancelled` |
| 10 | `recipe_worker_pool.ccs` | Dest pool | N workers + queue; close jobs next to last send; `.wait()` joins |
| 11 | `recipe_ordered_parallel.ccs` | Ordered channel | `send_task` + FIFO recv; `@parallel spawn`; produce `tx.close()` |
| 12 | `recipe_exclusive_named.ccs` | Named exclusivity | `CCExclusive`, resolve-once mutex, `acquire_when`, short guard CS |
| 12a | `recipe_turnstile.ccs` | Turnstile stages | `@parallel wait` + `@stage (ts.read/write, i)` — no nursery |
| 12b | `recipe_prepare_commit.ccs` | Prepare / join / hold / commit | Parallel prepares; `.wait()` is the join; hold only around commit; revert who finished |
| 13 | `recipe_arena_scope.ccs` | Memory | Arena names a lifetime; `@scratch` dies; keep by passing the arena last |
| 13a | `recipe_walk.ccs` | Walk / buffers | `@for in`; dest-bulk copy/move/fill; no memcpy / malloc |
| 14 | `recipe_owned_view.ccs` | Locality | Owned or view; constructors assume dead; failure is unchanged; faces at the use site |
| 15 | `recipe_long_lived_store.ccs` | Provenance | Anchoring request-lifetime views in a long-lived arena |
| 16 | `recipe_defer_cleanup.ccs` | Cleanup | `@defer` for resource management |

After these, explore the remaining recipes and build system examples.

## Overview

### `hello.ccs`
Minimal concurrent hello — three named siblings, `.wait()` joins.

### Recipes (concurrency patterns)

| File | Pattern | Key Concept |
|------|---------|-------------|
| `recipe_parallel.ccs` | `@parallel` | Independent join / range; wait-for + `@stage` is the shared write ticket |
| `recipe_parallel_forms.ccs` | `@parallel` matrix | Join vs dest × modifiers; dest bodies resolve `!>` inside, `return;` ends the fiber, `h.fail(e)` is the dest error |
| `recipe_parallel_stream.ccs` | On-page stream | `@parallel spawn`; produce `tx.close()`; consume `recv` until EOF |
| `recipe_parallel_empty.ccs` | Dest EMPTY-close | Consumer already in `recv`; `h.close(tx)` on the producer dest |
| `recipe_explicit_capture.ccs` | Capture | `@parallel` is the frame (no list); closure lists on `n.spawn` / `send_task` |
| `recipe_channel_pipeline.ccs` | EMPTY-close | Nested bag; closer at inner EMPTY |
| `recipe_async_await.ccs` | Async/Await | `@async`, `@await`, `cc_block_on` |
| `recipe_worker_pool.ccs` | Dest pool | N workers, shared queue; host grow/retract is `thrdqueue` |
| `recipe_exclusive_named.ccs` | Named exclusive | Domain + `mutex(name)` once + `acquire_when` + guard unlock |
| `recipe_turnstile.ccs` | Turnstile stages | `@parallel wait`; `@stage (ts.read, i)` then `@stage (ts.write, i)` |
| `recipe_prepare_commit.ccs` | Prepare + commit | `@parallel` join, `hold_sorted`, revert finished sides |
| `recipe_arena_scope.ccs` | Scoped memory | Named lifetime; keep a product by passing the arena last |
| `recipe_walk.ccs` | Walk / buffers | Extent walk; `dst.copy` / `move` / `fill`; `clone_into`; vec dest-init |
| `recipe_owned_view.ccs` | Owned or view | Construct / destroy / reopen; epochs as fields; Measure cannot `replace` |
| `recipe_long_lived_store.ccs` | Long-lived store | Explicit provenance movement into an arena-owned store |
| `recipe_defer_cleanup.ccs` | Cleanup | `@defer` on scope exit |
| `recipe_timeout.ccs` | Deadline | Ambient / bound clock; `h.cancel()` stops siblings via `h.cancelled` |
| `recipe_result_error_handling.ccs` | Results | `?>` : `E → T`; `!>` : `E →` control flow; `(e)` / bare `!>` |
| `recipe_variant.ccs` | `@variant` | One active arm; `case .arm(bind):`; field-path `@switch`; exhaustive check |
| `recipe_unwrap_destroy_forms.ccs` | Unwrap matrix | Two ops × modifiers; `@destroy` on successful construction |
| `recipe_ufcs_forms.ccs` | UFCS matrix | One rule × spellings, including bare-name and fallible chains |
| `recipe_user_generics.ccs` | User generics | `CC_GENERIC_FACTORY` — same `Name::[args]` rule as Vec/Map |
| `recipe_ordered_parallel.ccs` | Ordered stream | `send_task` + FIFO recv; `@parallel spawn`; produce `tx.close()` |

### Python interop (one boundary, two doors)

| File | Direction | Key Concept |
|------|-----------|-------------|
| `recipe_py_interop.ccs` | CC embeds Python | `cc_py_available()`, `py.exec`/`.eval`, member calls, `as_list`, error surface with `cc_error_site()` |
| `recipe_py_module.ccs` | Python imports CC | `py_module::[T]` + exported `PyInit_<name>` → `ccc build` links `<name>.abi3.so`, `import <name>` just works |

```bash
./cc/bin/ccc run examples/recipe_py_interop.ccs        # CC owns main, Python is the guest
./cc/bin/ccc build examples/recipe_py_module.ccs       # Python owns main, CC is the module
PYTHONPATH=bin python3 -c "import counter; print(counter.bump(4))"
```

Both directions share one binding (dlopen'd stable ABI, no link-time
Python dependency), one marshalling ruleset, and one benchmark
([`perf/py_baseline.ccs`](../perf/py_baseline.ccs); latest receipt
[`perf/baselines/py_baseline_20260809.txt`](../perf/baselines/py_baseline_20260809.txt)).
`examples/py/pydemo.shcc` is the same embed door in script form.
`examples/js/jsdemo.shcc` is the guest-module script (Node loads CC).
In-process hosting: `recipe_js_host.ccs`; N isolated domains:
`recipe_js_isolated.ccs`.
`examples/qjs/qjsdemo.shcc` is the in-process QuickJS embed
([`docs/quickjs.md`](../docs/quickjs.md)).

```bash
./cc/bin/ccc examples/js/jsdemo.shcc    # build js_demo.node + Math.sqrt/… via node
```

### Comparison: where Rust wins (data races)

| Directory | Point |
|-----------|--------|
| `compare_rust_data_race/` | Capture list makes the share visible (`[p]`); Rust still wins on proving the pointee is Sync-safe |

```bash
./examples/compare_rust_data_race/run.sh
```

Run any recipe:
```bash
./cc/bin/ccc build run examples/recipe_channel_pipeline.ccs
```

### SERDES / JSON

`serdes/json/` benches the stdlib RFC 8259 factory (`<ccc/std/json.cch>`,
`include JsonDom`) plus direct-to-struct schemas and a hand golden DOM
(`json.h`). Recognition rejects unescaped `U+0000`–`U+001F`; the golden
scanner still accepts raw controls. Full ladder:
`./examples/serdes/json/bench.sh -a`. Latest receipt:
[`serdes/json/benchmark_baseline_2026_08_15.txt`](serdes/json/benchmark_baseline_2026_08_15.txt).

### Networking Examples

| File | Demonstrates |
|------|--------------|
| `recipe_tcp_echo.ccs` | Accept until stop; `@parallel(h)` admits each handle onto the dest |
| `recipe_http_get.ccs` | Parallel HTTP requests with `@parallel for` |

HTTP examples require libcurl (system curl on macOS) and `-DCC_ENABLE_HTTP=1`. The
source already declares `@link("curl")`, so linking is automatic:
```bash
./cc/bin/ccc build run examples/recipe_http_get.ccs -DCC_ENABLE_HTTP=1
```

### Build System Examples

| Directory | Demonstrates |
|-----------|--------------|
| `build_stub/` | `CC_CONST`, `CC_OPTION`, CLI overrides |
| `build_graph/` | Multi-target builds with `CC_TARGET` |
| `mixed_c/` | CC + plain C interop |
| `multi/` | Multi-file CC builds |

Run build system examples:
```bash
# build_stub: compile-time constants
./cc/bin/ccc build --build-file examples/build_stub/build.cc --dump-consts

# build_graph: multi-target
./cc/bin/ccc build run --build-file examples/build_graph/build.cc

# mixed_c: CC + C interop
./cc/bin/ccc build run --build-file examples/mixed_c/build.cc
```

## See Also

- `spec/concurrent-c-spec-complete.md` — Language specification
- `spec/concurrent-c-build.md` — Build system specification
- `tests/` — More usage examples (as test cases)
