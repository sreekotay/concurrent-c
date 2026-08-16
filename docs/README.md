# Concurrent-C Documentation

## Getting Started

- **[Getting Started](getting-started.md)** — Install, first program, [arenas name a lifetime](getting-started.md#arenas-name-a-lifetime), concurrency, [`.shcc` scripts](getting-started.md#shcc-scripts), learning path
- **[Backwards compatibility](backwards_compatibility.md)** — Unit headers, version pins, bootstrap seeds
- **[JS / Python interop](js-py-modules.md)** — hosting (`pydemo.shcc`) and native module export
- **[When to run what](build-when.md)** — Install vs checkout vs stdlib vs lowerer vs ship vs cold smoke
- **[Language Concepts](language-concepts.md)** — Defer, results, UFCS, arenas (lifetime vs alloc policy) / provenance, closures
- **[@typehooks / @typeview](typehooks-typeviews.md)** — Tutorial: lifecycle hooks, is-a faces, allow-list views
- **[Cheatsheet](cheatsheet.md)** — Quick reference for common patterns
- **[Debugging](debugging.md)** — VS Code / Cursor debugging setup
- **[Sanitizers / fuzzing](sanitizers.md)** — ASan/TSan receipts, bridge Docker recipe, fuzz plan
- **[ILP32 Docker smoke](ilp32-docker.md)** — Linux i386 / ARM32 runtime smokes via Docker
- **[Script library (`.shcc`)](../spec/concurrent-c-spec-complete.md#95-script-library-shcc--cccscript)** — Shebang tools, synthetic `main`, `@task` dispatch

## Reference

- **[Specification](../spec/concurrent-c-spec-complete.md)** — Full language specification
- **[Standard Library](../spec/concurrent-c-stdlib-spec.md)** — Stdlib API reference
- **[Channels](../spec/concurrent-c-channel.md)** — Runtime channel state machine and wake/close invariants
- **[Build](../spec/concurrent-c-build.md)** — Compiler driver, outputs, cache, and target graph
- **[Grammar and SERDES](../spec/cc_serdes.md)** — Grammar engines and serialization operations
- **[Variants](../spec/draft_variants.md)** — Tagged-union semantics and packed layout
- **[Type views](../spec/draft_facets.md)** — `@typeview` faces and allow-lists (draft). Tutorial: [typehooks-typeviews.md](typehooks-typeviews.md)
- **[Allocator Strategy](../spec/draft_alloc_strategy.md)** — Arena release, heap overflow, and checkpoint/restore
- **[Fiber Scheduler](../spec/concurrent-c-scheduler.md)** — Scheduler state machine and park/wake contract
- **[Scheduler ops runbook](scheduler-ops-runbook.md)** — Build/test/diagnose loops for the scheduler
- **[Examples](../examples/)** — Working code examples with [learning path](../examples/README.md#learning-path-recommended-order)
- **[Performance](../perf/)** — Runtime benches, [interop baselines](../perf/baselines/), [compiler baseline](../perf/compiler_baseline.txt)

## Building the Compiler

**When to run what:** [build-when.md](build-when.md).

First checkout and day-to-day edit loops are there. Architecture / bootstrap
detail:

- [Compiler architecture](../cc/docs/ARCHITECTURE.md)
- [shadow_lower ops / layout](../cc/shadow/README.md)
- [Bootstrap snapshots](../cc/bootstrap/shadow_lower/README.md)
- [ILP32 Docker smoke](ilp32-docker.md)

## Quick Example

```c
#!ccc ccs
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
