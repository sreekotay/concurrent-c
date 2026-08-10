# Scheduler Operations Runbook

Practical guide for building, testing, and diagnosing the fiber scheduler.
Normative behavior is in `spec/concurrent-c-scheduler.md`.

## Files that matter

Default runtime path:

- `cc/runtime/sched_v2.c` / `sched_v2.h` — global ready queue, workers, sysmon,
  deadlock detection, park/signal/join.
- `cc/runtime/fiber_sched.c` / `fiber_internal.h` — public `cc__fiber_*` shims.
- `cc/runtime/fiber_sched_boundary.c` — waitable park/wake boundary used by
  channels and I/O.
- `cc/runtime/channel.c`, `nursery.c`, `task.c` — primary consumers.

Representative correctness pins:

- `tests/deadlock_suppress_scope_smoke.ccs`
- `tests/external_wait_scope_smoke.ccs`
- `tests/v2_deadline_cancel_smoke.ccs`
- Stress under `stress/` (e.g. legacy-named `work_stealing_race.ccs`, which
  exercises the global ready-queue race, plus `join_init_race.ccs`,
  `minimal_spawn_race.ccs`, and `fiber_spawn_join_tight.ccs`)
- Broad suite: `tools/run_all.ccs`

Performance comparison surface:

- `real_projects/pigz/` (`pigz_idiomatic` vs `pigz_pthread`)
- Payload: `real_projects/pigz/testdata/text_200mb.bin`
- `perf/run_neckbeard_challenges.sh` — six cross-language scheduler
  challenges; the Named Exclusive Lock challenge
  (`perf/compare_exclusive_named_lock.sh`) exercises the pool-growth
  hold decision under contention

## Environment knobs

Scheduler (see also the config table in the scheduler spec):

- `CC_V2_THREADS` / `CC_WORKERS` — worker count
- `CC_V2_TARGET_ACTIVE`, `CC_V2_SPIN_BEFORE_PARK`, `CC_V2_WAKE_SKIP_DEPTH`
- `CC_V2_JOIN_SPIN`, `CC_V2_PARK_EXTRAS_AT_STARTUP`, `CC_V2_CORO_POOL_MAX`
- `CC_V2_EAGER_THREADS` — inline worker-creation cap on the push path
  (default 2); set to the core count to restore fully-inline pool growth
- `CC_V2_GROW_RECHECK_US`, `CC_V2_GROW_RATE_US`, `CC_V2_GROW_DEPTH_X`,
  `CC_V2_GROW_ESCALATE_TICKS` — deferred pool-growth controller (see
  "Worker pool growth" below)
- `CC_V2_SYSMON_DETACH=0` — disable unchanged-dispatch worker eviction
- `CC_V2_STATS=1` / `CC_V2_SYSMON_STATS=1` — counters
- `CC_DEADLOCK_ABORT=0` — deadlock banner without `_exit(124)`
- `CC_DEADLOCK_PERSIST_MS=N` — override deadlock latch duration (default 1000)

Optional diagnostics (when present in the linked runtime):

- `CC_V2_STATS=1` — dump sched_v2 counters at exit (coro pool, join, wake, grow, spin)
- `CC_TASK_WAIT_STATS=1` / `CC_TASK_WAIT_STATS_DUMP=1` — `cc_block_on_intptr` wait
  attribution: spawn / fiber_v2 (ordered `send_task` await) / poll, with total ms
- `CC_DEBUG_WAKE`, `CC_DEBUG_DEADLOCK_RUNTIME`, `CC_DEBUG_SYSMON`

`CC_WORKER_GAP_STATS*` was removed — it was documented but never shipped a dump
path; use `CC_V2_STATS` + `CC_TASK_WAIT_STATS` instead.

Additional live implementation knobs are non-normative and may change:

- `CC_SCHED_STATS=1` — populate legacy scheduler diagnostic counters
- `CC_V3_SPEC_ASSERT=1` — enable scheduler-boundary assertion checks
- `CC_CHAN_MINIMAL_FAST_PATH=0` — disable the branded minimal channel path
- `CC_NURSERY_WORKER_FREES=0` — use the classic nursery join/wait cleanup path

For the current `CC_CHAN_*` tracing and isolation flags, see
`docs/debugging.md` under “Channel/select debugging.”

Use the smallest flag set that answers the question; verbose tracing changes
timing.

## Worker pool growth

The pool is a ratchet: workers are created on demand and never culled.
Growth happens on two paths:

- **Inline** — a push that finds work queued and no idle worker creates a
  pthread directly, but only up to `CC_V2_EAGER_THREADS` workers (default 2).
- **Deferred** — beyond that, the push CASes a one-shot grow-pending flag
  (the first requester per episode pays one sysmon wake syscall) and sysmon
  deliberates.

While a request is pending, sysmon rechecks every `CC_V2_GROW_RECHECK_US`
(default 25us; unreliable below ~10us due to kernel timeout resolution)
instead of sleeping its normal 20ms tick. Its slow-tick jobs (worker
eviction, park deadlines, deadlock detection, safety-net wake) are gated on
real elapsed time, so they keep their ~20ms cadence during fast rechecks.

Sysmon captures one (queue pops, timestamp) baseline per demand episode and
tracks the cumulative drain rate. Per recheck it grows one worker iff:

- **rate** — aggregate drain rate is below one pop per worker per
  `CC_V2_GROW_RATE_US` (default 100us): workers blocked or barely moving; or
- **depth** — ready-queue depth >= `CC_V2_GROW_DEPTH_X` (default 2) times
  the current pool size; 0 disables the depth trigger.

The two knobs are independent: the rate is normalized by elapsed time since
the episode baseline, so a faster recheck cadence changes only reaction
latency, not the measurement. Lowering `CC_V2_GROW_RATE_US` raises the
drain-rate bar and biases toward growth.

Otherwise it holds. The episode ends (flag cleared, re-armed by the next
qualifying push) when the queue drains, the pool reaches its maximum size,
or `CC_V2_TARGET_ACTIVE` gates admission.

Why holding is correct: CPU-bound fibers don't park, so they generate a
near-zero pop rate and recruit via the rate trigger; a high pop rate only
comes from park/wake churn (e.g. contended locks), where extra workers only
add cache-line traffic. `CC_V2_GROW_RATE_US` has a wide flat optimum
(50–200us measured equivalent; at 25us and below recruitment starts to
leak on contended-lock workloads).

`CC_V2_GROW_ESCALATE_TICKS` (default 0 = off) is opt-in insurance: after N
consecutive slow ticks with a non-empty queue and nobody idle, grow one
worker per slow tick regardless of the rate test.

With `CC_V2_STATS=1` the dump includes a growth line:

```
[sched_v2 stats] grow (eager<=2 recheck=25us rate=100us/pop/worker depth_x=2 esc=0): requests=... stall=... backlog=... escalate=... held=... final_threads=4/8
```

- `requests` — pushes that armed the grow-pending flag (one per episode)
- `stall` / `backlog` — rechecks that grew via the rate / depth trigger
- `escalate` — workers added by slow-tick escalation
- `held` — rechecks that decided not to grow
- `final_threads` — pool size at exit / cap

A contended-lock workload that holds at a small `final_threads` with a large
`held` count is the controller working as designed, not a recruitment bug.

## Build

```sh
make -C cc
```

Rebuild pigz targets when measuring that workload:

```sh
make -C real_projects/pigz -B pigz_idiomatic pigz_pthread
```

Stale runtime objects produce misleading results; rebuild `cc` before
scheduler experiments.

## Test loops

### Smoke / stress

```sh
./out/cc/bin/ccc run --release --timeout 180 tools/run_all.ccs -- examples stress
```

Targeted:

```sh
# Despite its legacy name, this targets the global ready-queue race.
./out/cc/bin/ccc run --timeout 10 ./stress/work_stealing_race.ccs
./out/cc/bin/ccc run --timeout 10 ./stress/minimal_spawn_race.ccs
./out/cc/bin/ccc run --timeout 10 ./tests/deadlock_suppress_scope_smoke.ccs
./out/cc/bin/ccc run --timeout 10 ./tests/external_wait_scope_smoke.ccs
./out/cc/bin/ccc run --timeout 10 ./tests/v2_deadline_cancel_smoke.ccs
```

Flake soak (adjust count/timeout as needed):

```sh
for i in $(seq 1 50); do
  echo "run $i"
  ./out/cc/bin/ccc run --timeout 10 ./stress/work_stealing_race.ccs || break
done
```

Deadlock detector pins often set `CC_WORKERS=1` and `CC_DEADLOCK_ABORT=1`
inside the test child for a deterministic latch.

### Pigz counter comparison

```sh
cp real_projects/pigz/testdata/text_200mb.bin /tmp/pigz_idio.bin
CC_V2_STATS=1 \
CC_TASK_WAIT_STATS=1 CC_TASK_WAIT_STATS_DUMP=1 \
./real_projects/pigz/out/pigz_idiomatic /tmp/pigz_idio.bin \
  >/tmp/pigz_idio.out 2>/tmp/pigz_idio.err

# Full with dict chaining (default) vs independent (-i, fair vs idiomatic):
cp real_projects/pigz/testdata/text_200mb.bin /tmp/pigz_full.bin
CC_V2_STATS=1 CC_TASK_WAIT_STATS=1 CC_TASK_WAIT_STATS_DUMP=1 \
CC_PIGZ_POOL_STATS=1 \
./real_projects/pigz/out/pigz_cc -k -f /tmp/pigz_full.bin \
  >/tmp/pigz_full.out 2>/tmp/pigz_full.err

cp real_projects/pigz/testdata/text_200mb.bin /tmp/pigz_full_i.bin
CC_V2_STATS=1 CC_TASK_WAIT_STATS=1 CC_TASK_WAIT_STATS_DUMP=1 \
CC_PIGZ_POOL_STATS=1 \
./real_projects/pigz/out/pigz_cc -k -f -i /tmp/pigz_full_i.bin \
  >/tmp/pigz_full_i.out 2>/tmp/pigz_full_i.err
```

Useful counter groups: `fiber_v2 join wait` (ordered-channel await of compress
tasks), `CC_V2_STATS` parks/wake/grow, and `[pigz_cc pool]` borrow wait (reader
blocked on the input-arena pool). Dict chaining (`pigz_cc` without `-i`) adds
CPU work; compare `-i` before attributing a wall-time gap to the scheduler.

### Pass vs timeout capture

When hunting intermittent timeouts, keep one PASS log and one TIMEOUT log
under a dated directory (e.g. `tmp/wsr-capture/<stamp>/`) with the same env
flags, then diff sleep/wake/pop lines and any `[gap-live]` output. Patch only
what the differential implicates.

## Working rules

- Prefer evidence (counter deltas + stress pass rates) over intuition-only
  wake/pop tuning.
- Keep behavioral changes small and reversible; do not mix unrelated refactors
  into a liveness chase.
- After a scheduler change: targeted stress pins, then broader
  `tools/run_all.ccs` / smoke, then pigz if the change is performance-oriented.
