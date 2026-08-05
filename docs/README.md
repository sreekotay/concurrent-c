# Concurrent-C Documentation

## Getting Started

- **[Getting Started](getting-started.md)** — Install, first program, concurrency
- **[Language Concepts](language-concepts.md)** — Defer, results, UFCS, slices/arenas, closures
- **[Cheatsheet](cheatsheet.md)** — Quick reference for common patterns
- **[Debugging](debugging.md)** — VS Code / Cursor debugging setup
- **[ILP32 Docker smoke](ilp32-docker.md)** — Linux i386 (and planned ARM32) runtime smokes via Docker

## Reference

- **[Specification](../spec/concurrent-c-spec-complete.md)** — Full language specification
- **[Standard Library](../spec/concurrent-c-stdlib-spec.md)** — Stdlib API reference
- **[Channels](../spec/concurrent-c-channel.md)** — Runtime channel state machine and wake/close invariants
- **[Build](../spec/concurrent-c-build.md)** — Compiler driver, outputs, cache, and target graph
- **[Grammar and SERDES](../spec/cc_serdes.md)** — Grammar engines and serialization operations
- **[Variants](../spec/draft_variants.md)** — Tagged-union semantics and packed layout
- **[Allocator Strategy](../spec/draft_alloc_strategy.md)** — Arena release and heap-overflow behavior
- **[Fiber Scheduler](../spec/concurrent-c-scheduler.md)** — Scheduler state machine and park/wake contract
- **[Scheduler ops runbook](scheduler-ops-runbook.md)** — Build/test/diagnose loops for the scheduler
- **[Examples](../examples/)** — Working code examples with [learning path](../examples/README.md#learning-path-recommended-order)

## Building the Compiler

```bash
cd cc && make
```

The compiler binary is at `cc/bin/ccc`.

## Quick Example

```c
#include <ccc/cc_runtime.cch>
#include <stdio.h>

int main(void) {
    @errhandler(CCError e) cc_error_exit(e);
    CCNursery* n = cc_nursery_create(NULL) !> @destroy;
    n->spawn(() => printf("Hello from task A!\n"));
    n->spawn(() => printf("Hello from task B!\n"));
    return 0;
}
```

```bash
./cc/bin/ccc run examples/hello.ccs
```
