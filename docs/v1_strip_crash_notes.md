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

## Update — 2026-04-18 follow-on session

### `cc_fiber_task_free` leak fix (commit `768d9d0`)
Confirmed working. `--perf --trace` ran 31/31 tests (28 pass, 2 unrelated
failures, 1 server skip) without a single panic. Both legacy-API perf
tests (`perf/perf_spawn_ladder.ccs`, `perf/spawn_fiber_direct.ccs`) now
pass.

`perf/spawn_sequential.ccs` un-skipped in commit `91a525a` after a
controlled isolation run (100/1000/5000/10000 iters all clean, RSS flat
at 46 MB). It was never the actual leaker — it crashed the machine the
first time only because the leak in `perf_spawn_ladder` had already
exhausted Mach VM region capacity by the time it ran.

### Crasher #3: `stress/noisy_neighbor.ccs`  (still open)
Trace pinpoint: ran `tools/run_all --all --trace`, last START with no
END was `[1776547989433] START [41/44] stress/noisy_neighbor.ccs`.
Definitive panic point.

The test:
- sets `CC_V2_THREADS=4` (was `CC_FIBER_WORKERS=4` until V1 retirement;
  global rename done as part of the V1 cruft strip — V2 only honors
  `CC_V2_THREADS` / `CC_WORKERS`, see `cc/runtime/sched_v2.c:1122-1132`).
- spawns 15 CPU-hog fibers in a tight `while (!cc_cancelled())` loop with
  no yield/await/syscall — each one pins a worker thread.
- spawns 1 heartbeat fiber relying on `cc_sleep_ms(100)` to tick.
- relies on the V2 sysmon to give the heartbeat a slot when all workers
  are pinned.

This test passed cleanly in earlier traces (run before the perf leg).
It panicked the Mac when run after 40+ preceding stress tests. The
causal hypothesis is cumulative: mach VM regions / kqueue / dispatch
source state from prior subprocesses + a 3-second sustained 100% CPU
load + sysmon pool growth pushes the kernel over an internal limit.

### Other regressions visible across the two latest traces
- `stress/pipeline_repeat.ccs` was rc=0 in the first trace, rc=1 in
  the second. Flaky / racy under stress-suite load. Not a crash.
- `stress/lost_wakeup_hammer.ccs` rc=1 in trace 1 (status TBD in trace
  2 — the run panicked before it could be re-attempted).
- `stress/backpressure_cycle_ring3_deadline.ccs` rc=1 in trace 1.

### Decision
Skip `stress/noisy_neighbor.ccs` in `tools/run_all` so the full
`--all --trace` run can complete. Investigate it in isolation with the
same scaling protocol used for `perf/spawn_sequential` (low
NUM_HOGS / TEST_DURATION_SEC, watch RSS + thread count).

Update: `CC_FIBER_WORKERS` was retired everywhere in the tree (10 sites)
in favor of `CC_V2_THREADS`. The dead `#define CC_FIBER_WORKERS` in
`cc/runtime/fiber_sched.c` was deleted, and the V1-only "Concurrent-C V1"
comparison row in `perf/compare_syscall.sh` was collapsed into the single
"Concurrent-C" row. The compile-time knob no longer exists in the spec.

## Post-V1-strip test triage (6 failures from run_all --all)

After the full run_all completed without kernel panics (first time
since V1 retirement), 6 tests failed. Triage status:

1. **`perf/channel_wake_wave.ccs`** — FIXED (commit `a5a33d9`?)
   V1-strip casualty: called the retired `cc__fiber_set_worker_affinity`
   API. Added a no-op shim in `cc/runtime/fiber_sched.c` so the test
   links. Its mismatch_rounds counter is now meaningless (affinity is
   gone) but its actual pass criterion (`bad_value_rounds == 0`) still
   holds.

2. **`examples/recipe_http_get.ccs`** — SKIPPED (pre-existing toolchain
   issue). `curl/system.h` declares `accept()` before `<sys/socket.h>`
   pulls in the macOS SDK version, producing
   `incompatible types for redefinition of 'accept'`. Unrelated to
   the V1 strip; defer until we touch the curl integration.

3. **`stress/backpressure_cycle_ring3_deadline.ccs`** — FIXED
   (commit `b108d4b`). Root cause: `sched_v2_park()` had no deadline
   awareness, so `@with_deadline` wrapping a blocking `cc_chan_send`
   on a full-no-receiver channel would never time out — the V2
   deadlock detector (~200 ms) fired instead of `ETIMEDOUT` reaching
   the caller. Fix: added `park_deadline` + `has_park_deadline` to
   `fiber_v2`, a global atomic counter `g_v2_park_deadlines` so sysmon
   short-circuits when nothing has a deadline in flight, and hooked
   `sched_v2_wake_expired_parkers()` into the sysmon tick. Resolution
   is bounded by `V2_SYSMON_INTERVAL_MS` (20 ms).

4. **`stress/pipeline_repeat.ccs`** — FLAKY (pre-existing, not V1-strip
   related). 5-stage pipeline, 100 items per iter, cap=8 channels, 500
   iters. Passes 2/4 runs, fails with identical loss pattern:
   `count=92 sum=4554` = first 92 items delivered, last 8 (= one
   buffer-worth) lost. Strong signal of a close-vs-drain race in the
   lockfree backend — producer sends 100 items then closes, but close
   propagates through the pipeline before all 8 in-flight items are
   drained at some stage. Needs a full audit of
   `cc__chan_try_drain_lockfree_on_close` + `lfqueue_inflight`
   accounting under high close/send contention. Deferred — was flaky
   before the V1 strip.

5. **`perf/mem_stress.ccs`** — FIXED by timeout bump. Completes
   cleanly in ~21 s standalone; default 20 s timeout in run_all was
   tripping on a good run. Bumped to 40 s in `get_timeout_sec`.

6. **`perf/channel_contention.ccs`** — APPEARS FIXED. Failed once in
   the original trace but now passes 3/3 runs in isolation. May have
   been collateral damage from the deadline bug (#3) or genuinely
   flaky. Watch in future runs; if it recurs, treat as a lost-wake bug
   on the send side.
