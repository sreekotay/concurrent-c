# Examples

Demonstrations of Concurrent-C features and patterns.

## Quick Start

```bash
# Build and run any example
./cc/bin/ccc build run examples/hello.ccs
```

## Overview

### `hello.ccs`
Minimal concurrent hello world — shows nursery and spawn.

### Recipes (concurrency patterns)

| File | Pattern | Key Concept |
|------|---------|-------------|
| `recipe_parallel_tasks.ccs` | Fan-out | Nursery as join point |
| `recipe_channel_pipeline.ccs` | Producer/consumer | Nested nursery + `closing()` |
| `recipe_arena_scope.ccs` | Scoped memory | Arena reset per iteration |
| `recipe_defer_cleanup.ccs` | Cleanup | `@defer` on scope exit |
| `recipe_timeout.ccs` | Deadline | Cooperative cancellation |
| `recipe_worker_pool.ccs` | Worker pool | N workers, shared queue |

Run any recipe:
```bash
./cc/bin/ccc build run examples/recipe_parallel_tasks.ccs
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
