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
| 1 | `hello.ccs` | First nursery | `@nursery`, `spawn()`, basic structured concurrency |
| 2 | `recipe_fanout_capture.ccs` | Multiple tasks | Spawning N tasks, closure captures |
| 3 | `recipe_channel_pipeline.ccs` | Communication | Channels, `closing()`, producer/consumer |
| 4 | `recipe_async_await.ccs` | Async/Await | `@async` functions, `await`, `cc_block_on` |
| 5 | `recipe_timeout.ccs` | Cancellation | `@with_deadline`, cooperative exit |
| 6 | `recipe_worker_pool.ccs` | Real pattern | Putting it together: workers + channels |
| 7 | `recipe_arena_scope.ccs` | Memory | `@arena`, scoped allocation |
| 8 | `recipe_defer_cleanup.ccs` | Cleanup | `@defer` for resource management |

After these, explore the remaining recipes and build system examples.

## Overview

### `hello.ccs`
Minimal concurrent hello world — shows nursery and spawn.

### Recipes (concurrency patterns)

| File | Pattern | Key Concept |
|------|---------|-------------|
| `recipe_fanout_capture.ccs` | Fan-out | N tasks with captured data |
| `recipe_explicit_capture.ccs` | Capture semantics | Value vs reference capture |
| `recipe_channel_pipeline.ccs` | Producer/consumer | Nested nursery + `closing()` |
| `recipe_async_await.ccs` | Async/Await | `@async`, `await`, `cc_block_on` |
| `recipe_worker_pool.ccs` | Worker pool | N workers, shared queue |
| `recipe_arena_scope.ccs` | Scoped memory | Arena reset per iteration |
| `recipe_defer_cleanup.ccs` | Cleanup | `@defer` on scope exit |
| `recipe_timeout.ccs` | Deadline | Cooperative cancellation |
| `recipe_optional_values.ccs` | Optionals | `T?` types, find/map patterns |
| `recipe_result_error_handling.ccs` | Results | `T!E` error handling |

Run any recipe:
```bash
./cc/bin/ccc build run examples/recipe_channel_pipeline.ccs
```

### Networking Examples

| File | Demonstrates |
|------|--------------|
| `recipe_tcp_echo.ccs` | TCP sockets, listen/accept/read/write |
| `recipe_http_get.ccs` | Parallel HTTP requests with `@nursery` |

HTTP examples require libcurl (system curl on macOS):
```bash
./cc/bin/ccc build run --build-file examples/recipe_http_get.build.cc
```

### Build System Examples

| Directory | Demonstrates |
|-----------|--------------|
| `build_stub/` | `CC_CONST`, `CC_OPTION`, CLI overrides |
| `build_graph/` | Multi-target builds with `CC_TARGET` |
| `mixed_c/` | CC + plain C interop |
| `multi/` | Multi-file CC builds |
| `recipe_http_get.build.cc` | External library linking (`CC_TARGET_LIBS`) |

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
