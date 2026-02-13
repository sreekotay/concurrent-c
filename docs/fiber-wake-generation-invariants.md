# Fiber Wake-Generation Invariants

This checklist documents the scheduler invariants for the generation-driven
worker sleep/wake protocol in `cc/runtime/fiber_sched.c`.

For scheduler v3 planning, this document is also the compatibility checklist for
preserving wake correctness while the implementation is switched behind
`CC_RUNTIME_V3`.

## Core idea

`g_sched.wake_prim.value` is a monotonic generation counter.
Every enqueue path that makes runnable work available must advance this
generation (either via `wake_primitive_wake_one()` or direct bump).

Workers snapshot a generation before sleeping and only sleep while the
generation remains unchanged.

## Required invariants

1. **Publish before wake-gen advance**
   - Runnable fiber must be visible in queue memory before wake generation is
     advanced.
   - Queue push release semantics + wake generation release store satisfy this.

2. **Every enqueue advances generation**
   - No conditional "maybe wake" path may skip generation advance.
   - Sleeping workers rely on generation mismatch to avoid missed sleeps.

3. **Worker snapshots generation before sleep transition**
   - Snapshot must happen before `sleeping++`.
   - If enqueue happens after snapshot but before wait, generation mismatch
     must make wait return immediately.

4. **Worker announces sleepability (`sleeping++`) before wait**
   - Spawners may decide whether to issue kernel wake based on `sleeping`.
   - Even when no wake syscall is issued, generation bump must still occur.

5. **Post-`sleeping++` local re-check**
   - Worker re-checks local/inbox/global runnable queues after `sleeping++`.
   - This is an optimization (skip syscall if work already visible), not the
     sole correctness mechanism.

6. **Wait condition couples to generation**
   - `wake_primitive_wait_timeout(wake_gen)` must only be called when current
     generation equals snapshot generation.
   - If generation changed, worker exits wait loop and retries dequeue.

7. **Single decrement for each increment**
   - Every successful `sleeping++` must have exactly one corresponding
     `sleeping--`, including early exits from post-increment queue hit.

## V3 lifecycle compatibility (required)

The generation protocol must remain valid across these worker states:

- `ACTIVE` -> executes fibers and transitions dequeued work to `RUNNING`.
- `IDLE_SPIN` -> bounded probe/steal loops before sleeping.
- `SLEEP` -> blocked on wake primitive coupled to generation snapshot.
- `DRAINING` -> refuses new work, drains local queue to global queue.
- `DEAD` -> terminal state.

Fiber lifecycle rules remain:

- Park path: `RUNNING -> PARKING -> PARKED` (or abort to `RUNNING`).
- Wake path: `PARKED -> WAKING -> RUNNABLE` (single claim owner only).
- Only `RUNNABLE` fibers are queue-eligible; only `PARKED` fibers are
  wake-claim eligible.

## Internal API boundary checklist (required)

Channel/waitable code must only interact with scheduler via this boundary:

- `schedule(Fiber*)`
- `worker_next()`
- `fiber_wait(waitable, io, ops)`
- `fiber_wake(Fiber*)`

Checks:

1. Channel/Waitable logic never enqueues directly to run queues.
2. Wake-all on close still uses claim protocol (`PARKED -> WAKING`) and does not
   bypass single-enqueue ownership.
3. `publish()` for post-close admissions fails deterministically; admitted
   pre-close in-flight waiters may still complete.

## Performance guidance

- Keep idle path branch count low; avoid multi-phase spin loops unless backed by
  data showing win for representative workloads.
- Prefer one-shot steal attempts over repeated steal scans in idle transitions.
- Use `CC_WAKE_GEN_STATS=1` to inspect generation protocol behavior:
  - sleep entries
  - generation bumps with no sleepers
  - stale-generation detected immediately after `sleeping++`
  - micro-spin hits and fast no-wait returns
  - steal attempts/hits
  - post-increment queue hits
  - wait calls and break reasons (queue-ready vs generation-advanced)

- Tune idle wait timeout with `CC_IDLE_WAIT_TIMEOUT_MS` (default `5` ms).
  Worker idle logic adaptively tightens timeout to `1ms` after generation-driven
  wakes and slowly backs off toward the configured max on timeout-like loops.

## Regression checks

When changing idle/wake code:

1. Run channel contention and nursery race tests.
2. Run pigz directjoin interleaved fiber/thread A/B.
3. Capture `CC_WAKE_GEN_STATS=1` and verify:
   - non-zero generation-advanced breaks under load
   - no pathological growth in wait calls relative to sleep entries
