# Visitor & Passes

- Pass 0: const/comptime function collection (populate symbol table).
- Pass 1: single comprehensive visitor for types, moves/provenance, async semantics, @scoped checks, comptime branch selection, monomorph instantiation, and C codegen (sync + async state machines). Checks `send_take` eligibility at compile time (unique + transferable, non-subslice).

## Pipeline & cleanup status

- **Authoritative pipeline map:** [PIPELINE.md](PIPELINE.md)
- **Pass inventory:** [PASS_INVENTORY.md](PASS_INVENTORY.md)
- **M0–M5.5 ship status:** [COMPILER_CLEANUP_STATUS.md](../../docs/COMPILER_CLEANUP_STATUS.md)
- **M6 (deferred):** [M6_DEFERRED.md](M6_DEFERRED.md)

## Compiler debugging

Environment variables and flags (see [DEBUG_VARS.md](../diag/DEBUG_VARS.md)):

```bash
CC_DEBUG_REPARSE=1 ccc build foo.ccs
CC_DEBUG_DIAG=1 ccc build foo.ccs
ccc build foo.ccs --show-lowered=phase3
CC_BATCH_PHASE3=1 ccc build foo.ccs   # experimental Phase 3 batching
```

Baseline metrics: `scripts/capture_baseline.sh` → `perf/baseline_M0.txt`.

