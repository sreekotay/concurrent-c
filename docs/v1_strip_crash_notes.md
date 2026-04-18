# V1 Strip — Crash Investigation Notes

Snapshot date: 2026-04-14 / 2026-04-18 session

## Session context
After V1 scheduler retirement (commits `462a75a` through `a09524c`), running
`tools/run_all.ccs --all` hard-panicked the Mac **twice**, each time during
the perf category. The `--trace` flag added in commit `c4fcd24` (run_all
writes each START/END with `fflush` + `fsync(fileno(file))`) let us reconstruct
exactly which test was running when the kernel went down.

## Definitive findings (from `tmp/run_all_logs/run_trace.log`)

### Crasher #1: `perf/spawn_sequential.ccs`
Last trace line before first crash was a dangling START for this test with
no matching END. Ran 10,000 iterations of `@async simple_task(i); cc_block_on(int, task)`.
Added to run_all skip list in commit `a3a338a` (temporary).

### Crasher #2: `perf/perf_spawn_ladder.ccs`  **ROOT CAUSE IDENTIFIED**
Trace pinpointed this as the second crasher. Uses the legacy-V1 fiber API
directly (declares the externs itself):
```c
extern struct fiber_task* cc_fiber_spawn(void* (*fn)(void*), void* arg);
extern int cc_fiber_join(struct fiber_task* t, void** out_result);
extern void cc_fiber_task_free(struct fiber_task* t);
```
`bench_direct_fiber_batch_once` runs `total_spawns/batch_size` batches of
`batch_size=1000` fibers, `samples=7` times, through a
spawn-all-then-join-all pattern.

### Why it links after the V1 strip
We kept `cc_fiber_spawn` / `cc_fiber_join` / `cc_fiber_task_free` as V2
shims in `cc/runtime/fiber_sched.c` (~lines 960–1010). `cc_fiber_spawn`
calls `sched_v2_spawn` and returns the `fiber_v2*` with bit 0 tagged as
`CC_FIBER_V2_TAG_LOCAL`, cast to `fiber_task*`. `cc_fiber_join` detects
the tag and dispatches to `sched_v2_join`.

### Why it OOM-panics the kernel
**The V2 ownership contract is `sched_v2_join` + `sched_v2_fiber_release`.**
`sched_v2_join` drops the join reference but does NOT free the `fiber_v2`
or unmap its stack. The canonical path, `cc_block_on_intptr` at
`cc/runtime/task.c:643-650`, calls both:
```c
sched_v2_join(fv->fiber, &result);
r = cc__task_take_v2_result(fv->fiber, result);
sched_v2_fiber_release(fv->fiber);
```
Our V1 `cc_fiber_task_free` shim was written as a no-op on the assumption
that `sched_v2_join` already released the fiber. That assumption was wrong.
Result: every `cc_fiber_spawn + cc_fiber_join + cc_fiber_task_free` cycle
leaks a full `fiber_v2` including its mmap'd stack (tens of KB to ~1MB).

`perf_spawn_ladder` runs `100,000` spawns × `7` samples × one benchmark
that uses this path = **~700k leaked V2 fibers**. Multiple GB of mmap'd
stack → Mach VM exhaustion → kernel panic (not graceful ENOMEM).

## Fix applied (uncommitted as of writing)
Edited `cc/runtime/fiber_sched.c::cc_fiber_task_free` to call
`sched_v2_fiber_release` on V2-tagged handles:
```c
void cc_fiber_task_free(fiber_task* f) {
    if (!f) return;
    if (cc__is_v2_fiber_local((cc__fiber*)f)) {
        sched_v2_fiber_release(cc__untag_v2_fiber_local((cc__fiber*)f));
    }
}
```
This restores the V1 "join then free" contract on top of V2.

## Status of other rc=1 failures in the trace
Completed cleanly (got END lines, not crashes — investigate separately):
- `examples/recipe_http_get.ccs` — curl/macOS SDK `accept()` header clash;
  pre-existing, unrelated to V1 strip.
- `stress/lost_wakeup_hammer.ccs` — failed rc=1, unknown cause.
- `stress/backpressure_cycle_ring3_deadline.ccs` — failed rc=1, unknown cause.

Expected rc=1 (intentional deadlock, detector firing):
- `stress/deadlock_detect_demo.ccs`
- `stress/complex_deadlock.ccs`

## Open question: `perf/spawn_sequential.ccs`
Still in run_all skip list. Uses `@async` (lowers to `cc_fiber_spawn_closure0`
per `pass_channel_syntax.c:1934`) and `cc_block_on(int, ...)` which goes
through `cc_block_on_intptr`'s `CC_TASK_KIND_FIBER_V2` branch — that one
DOES call `sched_v2_fiber_release`, so it should not leak the same way.
Needs a narrow test run (low `ITERATIONS`) to see if it has its own bug
or if it was collateral damage from the leak in perf_spawn_ladder
left over from the earlier runs.

## Git state at time of note
- Branch `main`
- Last committed work: `a3a338a tools/run_all: skip spawn_sequential (machine-crasher)`
- Uncommitted: the `cc_fiber_task_free` fix above.
- 11 V1-strip commits landed since `4ac55e2`; `cc/runtime/fiber_sched.c`
  down from 6499 → ~1296 lines.

## TODO next (ordered, safe)
1. Rebuild runtime (`make -C cc -j8`).
2. Run `perf/spawn_sequential.ccs` in isolation with low ITERATIONS and
   `/usr/bin/time -l` to diagnose it without risking a system crash.
3. If it's clean, un-skip it in `tools/run_all.ccs`.
4. Run `perf/perf_spawn_ladder.ccs` in isolation (same way) to confirm
   the `cc_fiber_task_free` fix eliminates the leak.
5. Commit the fix.
6. Re-run `tools/run_all.ccs --all --trace` end to end.
7. Triage `stress/lost_wakeup_hammer.ccs` and
   `stress/backpressure_cycle_ring3_deadline.ccs` if still failing.
