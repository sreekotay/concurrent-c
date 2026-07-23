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

## Environment knobs

Scheduler (see also the config table in the scheduler spec):

- `CC_V2_THREADS` / `CC_WORKERS` — worker count
- `CC_V2_TARGET_ACTIVE`, `CC_V2_SPIN_BEFORE_PARK`, `CC_V2_WAKE_SKIP_DEPTH`
- `CC_V2_JOIN_SPIN`, `CC_V2_PARK_EXTRAS_AT_STARTUP`, `CC_V2_CORO_POOL_MAX`
- `CC_V2_SYSMON_DETACH=0` — disable unchanged-dispatch worker eviction
- `CC_V2_STATS=1` / `CC_V2_SYSMON_STATS=1` — counters
- `CC_DEADLOCK_ABORT=0` — deadlock banner without `_exit(124)`

Optional diagnostics (when present in the linked runtime):

- `CC_WORKER_GAP_STATS`, `CC_WORKER_GAP_STATS_DUMP`, `CC_WORKER_GAP_STATS_LIVE`
- `CC_TASK_WAIT_STATS`, `CC_TASK_WAIT_STATS_DUMP`
- `CC_DEBUG_WAKE`, `CC_DEBUG_DEADLOCK_RUNTIME`, `CC_DEBUG_SYSMON`

Additional live implementation knobs are non-normative and may change:

- `CC_SCHED_STATS=1` — populate legacy scheduler diagnostic counters
- `CC_V3_SPEC_ASSERT=1` — enable scheduler-boundary assertion checks
- `CC_CHAN_MINIMAL_FAST_PATH=0` — disable the branded minimal channel path
- `CC_NURSERY_WORKER_FREES=0` — use the classic nursery join/wait cleanup path

For the current `CC_CHAN_*` tracing and isolation flags, see
`docs/debugging.md` under “Channel/select debugging.”

Use the smallest flag set that answers the question; verbose tracing changes
timing.

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
CC_WORKER_GAP_STATS=1 CC_WORKER_GAP_STATS_DUMP=1 \
CC_TASK_WAIT_STATS=1 CC_TASK_WAIT_STATS_DUMP=1 \
./real_projects/pigz/out/pigz_idiomatic /tmp/pigz_idio.bin \
  >/tmp/pigz_idio.out 2>/tmp/pigz_idio.err

cp real_projects/pigz/testdata/text_200mb.bin /tmp/pigz_thr.bin
CC_WORKER_GAP_STATS=1 CC_WORKER_GAP_STATS_DUMP=1 \
CC_TASK_WAIT_STATS=1 CC_TASK_WAIT_STATS_DUMP=1 \
./real_projects/pigz/out/pigz_pthread /tmp/pigz_thr.bin \
  >/tmp/pigz_thr.out 2>/tmp/pigz_thr.err
```

Useful counter groups when available: sleep/wait volume, global pop attempts,
wake-source split, non-worker spawn publish path.

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
