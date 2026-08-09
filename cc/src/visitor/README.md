# Visitor & Passes (legacy front)

> **Not the default product front.** `ccc` defaults to serdes
> ([ARCHITECTURE.md](../../docs/ARCHITECTURE.md)). These passes run only with
> `--frontend=legacy`. Legacy ADR:
> [LEGACY_ARCHITECTURE.md](../../docs/LEGACY_ARCHITECTURE.md).

- Pass 0: const/comptime function collection (populate symbol table).
- Pass 1: single comprehensive visitor for types, moves/provenance, async
  semantics, @scoped checks, comptime branch selection, monomorph
  instantiation, and C codegen (sync + async state machines). Checks
  `send_take` eligibility at compile time (unique + transferable, non-subslice).

## Pipeline & cleanup status

- **Authoritative pipeline map:** [PIPELINE.md](PIPELINE.md)
- **Pass inventory:** [PASS_INVENTORY.md](PASS_INVENTORY.md)
- **M0–M5.5 ship status:** [COMPILER_CLEANUP_STATUS.md](../../docs/COMPILER_CLEANUP_STATUS.md)
- **M6 (deferred):** [M6_DEFERRED.md](M6_DEFERRED.md)

## Compiler debugging (legacy)

Environment variables and flags (see [DEBUG_VARS.md](../diag/DEBUG_VARS.md)):

```bash
CC_FRONTEND=legacy CC_DEBUG_REPARSE=1 ccc build foo.ccs
CC_FRONTEND=legacy CC_DEBUG_DIAG=1 ccc build foo.ccs
CC_FRONTEND=legacy ccc build foo.ccs --show-lowered=phase3
```

Phase 3 lowering runs in two batched stages (UFCS, then closure_calls +
autoblock + await_normalize); see [PIPELINE.md](PIPELINE.md).

Baseline metrics: [`scripts/capture_baseline.sh`](../../../scripts/capture_baseline.sh)
→ [`perf/compiler_baseline.txt`](../../../perf/compiler_baseline.txt)
(`make perf-baseline` / see [`perf/README.md`](../../../perf/README.md#compiler-perf-baseline)).
