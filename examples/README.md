# Examples

Demonstrations of Concurrent-C features and patterns.

## Quick Start

```bash
# Build and run any example
./cc/bin/ccc run examples/hello.ccs
```

## Learning Path (Recommended Order)

New to Concurrent-C? Work through these in order:

| # | File | Concept | What you'll learn |
|---|------|---------|-------------------|
| 1  | `hello.ccs` | First nursery | `cc_nursery_create`, `n->spawn()`, basic structured concurrency |
| 2  | `recipe_result_error_handling.ccs` | Results & `@errhandler` | `?>` : `E → T`; `!>` : `E →` control flow; `(e)` exposes; bare `!>` routes |
| 3  | `recipe_unwrap_destroy_forms.ccs` | Unwrap shape | Same two ops × modifiers; `@destroy` on successful construction |
| 4  | `recipe_ufcs_forms.ccs` | UFCS shape | One dispatch rule × spellings (families, bare-name, fallible chains) |
| 4a | `recipe_user_generics.ccs` | Generics | `Name::[args]` + `CC_GENERIC_FACTORY` — same rule as Vec/Map |
| 5  | `recipe_fanout_capture.ccs` | Multiple tasks | Spawning N tasks, fresh per-iteration captures |
| 6  | `recipe_explicit_capture.ccs` | Capture semantics | Value vs reference capture, mutation rules |
| 7  | `recipe_channel_pipeline.ccs` | Communication | Channels, owned close, producer/consumer |
| 8  | `recipe_async_await.ccs` | Async/Await | `@async` call stacks vs `spawn`, `@await`, composition |
| 9  | `recipe_timeout.ccs` | Cancellation | deadlines, cooperative exit |
| 10 | `recipe_worker_pool.ccs` | Real pattern | Putting it together: workers + channels |
| 11 | `recipe_ordered_parallel.ccs` | Ordered fan-out | `send_task` + ordered recv, FIFO without reorder buffer |
| 11a | `recipe_parallel.ccs` | `@parallel` | assignment join, `@serial` arms, `@parallel (pred)`, `@parallel for` |
| 12 | `recipe_exclusive_named.ccs` | Named exclusivity | `CCExclusive`, resolve-once mutex, `acquire_when`, short guard CS |
| 13 | `recipe_arena_scope.ccs` | Memory | Arena names a lifetime; alloc strategy is policy; `@destroy` ends the epoch |
| 14 | `recipe_long_lived_store.ccs` | Provenance | Anchoring request-lifetime views in a long-lived arena |
| 15 | `recipe_defer_cleanup.ccs` | Cleanup | `@defer` for resource management |

After these, explore the remaining recipes and build system examples.

## Overview

### `hello.ccs`
Minimal concurrent hello world — shows explicit nursery creation and task spawn.

### Recipes (concurrency patterns)

| File | Pattern | Key Concept |
|------|---------|-------------|
| `recipe_fanout_capture.ccs` | Fan-out | N tasks with captured data |
| `recipe_explicit_capture.ccs` | Capture semantics | Value vs reference capture |
| `recipe_channel_pipeline.ccs` | Producer/consumer | Nested ownership + channel close |
| `recipe_async_await.ccs` | Async/Await | `@async`, `@await`, `cc_block_on` |
| `recipe_worker_pool.ccs` | Worker pool | N workers, shared queue |
| `recipe_exclusive_named.ccs` | Named exclusive | Domain + `mutex(name)` once + `acquire_when` + guard unlock |
| `recipe_arena_scope.ccs` | Scoped memory | Named lifetime per iteration; bump/reset is policy |
| `recipe_long_lived_store.ccs` | Long-lived store | Explicit provenance movement into an arena-owned store |
| `recipe_defer_cleanup.ccs` | Cleanup | `@defer` on scope exit |
| `recipe_timeout.ccs` | Deadline | Cooperative cancellation |
| `recipe_result_error_handling.ccs` | Results | `?>` : `E → T`; `!>` : `E →` control flow; `(e)` / bare `!>` |
| `recipe_unwrap_destroy_forms.ccs` | Unwrap matrix | Two ops × modifiers; `@destroy` on successful construction |
| `recipe_ufcs_forms.ccs` | UFCS matrix | One rule × spellings, including bare-name and fallible chains |
| `recipe_user_generics.ccs` | User generics | `CC_GENERIC_FACTORY` — same `Name::[args]` rule as Vec/Map |
| `recipe_ordered_parallel.ccs` | Ordered fan-out | `send_task` + ordered recv, FIFO await |
| `recipe_parallel.ccs` | `@parallel` | Value join; `@serial` arms; `@parallel (pred)` spawn gate; `@parallel for` over `lo..hi` |

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
`examples/js/jsdemo.shcc` is the JS call-out surface in guest mode (Node
owns the env — `cc_js_new` hosting is not implemented yet):

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

`serdes/json/` is the RFC 8259 JSON-text factory (`json.rules`) plus
direct-to-struct schemas and a hand golden DOM (`json.h`). Recognition
rejects unescaped `U+0000`–`U+001F`; the golden scanner still accepts
raw controls. Full ladder: `./examples/serdes/json/bench.sh -a`. Latest
receipt: [`serdes/json/benchmark_baseline_2026_08_15.txt`](serdes/json/benchmark_baseline_2026_08_15.txt).

### Networking Examples

| File | Demonstrates |
|------|--------------|
| `recipe_tcp_echo.ccs` | TCP sockets, listen/accept/read/write |
| `recipe_http_get.ccs` | Parallel HTTP requests with explicit nursery handles |

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
