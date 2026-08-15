# Debugging Concurrent-C programs (VS Code / Cursor)

This repo's driver (`ccc`) already defines stable output locations (see `spec/concurrent-c-build.md`):

- Generated C: `out/<stem>.c`
- Linked binary: `bin/<stem>`

## Quickstart: debug the currently open `.ccs`

This repo includes:

- `.vscode/tasks.json` (build/run via `./cc/bin/ccc`)
- `.vscode/launch.json` (LLDB launch config)

Install the Concurrent-C syntax extension from the marketplace (`sreekotay.concurrent-c-syntax`) or via `./cc-install.sh`, which also copies the in-tree package and attempts to install CodeLLDB when the `code` / `cursor` CLI is available.

In VS Code or Cursor:

1. Open a `.ccs` file (e.g. `examples/hello.ccs`)
2. Run **Run and Debug → Debug Concurrent-C (current file)**

It will:

- Run `./cc/bin/ccc build --no-cache --keep-c --cc-flags "-g -O0 -fno-omit-frame-pointer" <file>`
- Launch `${workspaceFolder}/bin/${fileBasenameNoExtension}` under LLDB

## What the toolchain needs for a good debugging experience

### 1) Debug symbols

`ccc` should pass through user-provided C compiler flags:

- Use `--cc-flags "-g -O0 -fno-omit-frame-pointer"` for predictable stepping/backtraces.

### 2) Stable, inspectable outputs

The build spec's defaults are ideal for debugging:

- Always emit generated C under `out/`
- Put executables under `bin/`
- Keep generated outputs on failure (so you can open `out/<stem>.c` and compile commands)

### 3) Source mapping back to `.ccs`

To step through `.ccs` instead of the generated `.c`, the generated C should preserve original locations.

Minimum viable approach:

- Emit `#line` directives in the generated C mapping back to the original `.ccs` filename + line numbers.

With `#line` mapping, debuggers and diagnostics will typically attribute locations to the `.ccs` file even though the compiler is compiling `out/<stem>.c`.

## Concurrency debugging (ThreadSanitizer)

Use the provided scripts for regular race detection:

- Quick TSan run: `./scripts/test_tsan.sh`
- Full TSan run: `./scripts/test_tsan.sh --all`
- Stress tests under TSan: `./scripts/stress_sanitize.sh tsan`
- Stress tests under ASan: `./scripts/stress_sanitize.sh asan`
- real_projects ASan/TSan/fuzz: `./scripts/real_projects_sanitize.sh asan|tsan|fuzz`
- Bridge addon ASan (Linux/Docker): `./scripts/sanitize_bridge.sh`

Dated receipts, macOS SIP limits, and a fuzzing plan:
[`sanitizers.md`](sanitizers.md).

Notes:
- On macOS, TSan requires `clang`.
- TSan runs are slower; keep them focused on stress/race tests.

### Validating TSan suppressions

Some functions use `__attribute__((no_sanitize("thread")))` to suppress TSan checking. This is safe only when:
1. The function only accesses thread-local memory (e.g., local stack variables)
2. There are no real data races (suppression only hides false positives)

To validate a suppression is safe:
1. Create a stress test that calls the suppressed function concurrently from multiple threads/fibers
2. Run with TSan: `CC=clang CFLAGS="-fsanitize=thread" ./cc/bin/ccc run <test>`
3. If TSan reports races, investigate: either the suppression is masking a real race, or the suppression mechanism needs adjustment.

Example: `tests/tsan_closure_make_stress.c` validates that `cc_closure*_make` suppressions are safe (these functions only write to thread-local stack structs).

## Scheduler synchronization invariants (V2 fiber scheduler)

Normative state machine: `spec/concurrent-c-scheduler.md`. Shipped path:
`cc/runtime/sched_v2.c` (shimmed by `fiber_sched.c` / `fiber_sched_boundary.c`).

Base states (atomic word): `IDLE`, `QUEUED`, `RUNNING`, `PARKED`, `DEAD`, plus
flag `SIGNAL_PENDING` (OR-ed into the same word).

Key invariants:

- A signal against `PARKED` CASes to `QUEUED` and pushes the ready queue.
- A signal against `RUNNING` / `QUEUED` sets `SIGNAL_PENDING`; park commit
  requeues instead of parking when the bit is set (no lost wakeup).
- At most one worker executes a given fiber's coroutine at a time.
- A fiber is on the ready queue at most once.
- Join: at most one fiber-context waiter (`join_waiter_fiber`); thread waiters
  use the wake primitive. Prefer fiber-aware join from fiber context so the
  OS worker stays free.
- Sysmon: eviction, deadline wakes, deadlock detection, deferred pool growth.

If you add new scheduler behaviors, update the spec and extend the stress
tests (`stress/`, `tests/deadlock_*`, `tests/v2_*`).

### Wait / throughput diagnostics

| Variable | Effect |
|----------|--------|
| `CC_V2_STATS=1` | Dump coro pool, join, wake, grow, spin counters at exit |
| `CC_TASK_WAIT_STATS=1` + `CC_TASK_WAIT_STATS_DUMP=1` | Attribute `cc_block_on_intptr` waits: spawn / fiber_v2 (ordered `send_task` await) / poll |
| `CC_PIGZ_POOL_STATS=1` | pigz_cc only: input-arena pool borrow wait |

See `docs/scheduler-ops-runbook.md` for pigz A/B recipes.

## Channel/select debugging (lock-free + cc_chan_match_select)

### Key invariants (what must stay true)
- Close stops admission: send must not accept new work after close.
- Recv drains in-flight work: recv may return close only when there is no buffered data and no in-flight enqueue remaining (`lfqueue_inflight` counter).
- Wait nodes must stay on the channel's wait list whenever the fiber might park. The "remove → dequeue → re-add" pattern is prohibited; use "check count under lock → stay-on-list-and-park" instead.
- Direct handoff (`notified == DATA`) must be checked before any buffer dequeue to avoid overwriting handed-off data.
- Unbuffered rendezvous: nursery cancellation is checked only *before* committing to the wait list, not inside the waiting loop. In-progress rendezvous operations must complete.
- Select must have a single winner; non-winners cancel and rearm; wake should force a recheck, not "consume" a wake.
- Notification values are typed: `CC_CHAN_NOTIFY_NONE` (0), `DATA` (1), `CANCEL` (2), `CLOSE` (3), `SIGNAL` (4).

### Environment flags

These are the flags the runtime actually reads (see `getenv` calls in
`cc/runtime/channel.c` and `cc/runtime/sched_v2.c`):

| Variable | Description |
|----------|-------------|
| `CC_DEADLOCK_ABORT=0` | Continue after deadlock detection instead of exiting 124 (for log capture) |
| `CC_CHAN_NO_LOCKFREE=1` | Force mutex-based channel path (isolate lock-free bugs) |
| `CC_CHAN_TRACE_FLOW=1` | Trace channel send/recv flow events to stderr |
| `CC_CHAN_TRACE_CLOSE=1` | Trace channel close events |
| `CC_CHAN_TRACE_RECV_EMPTY=1` | Trace recv-on-empty wait/wake transitions |
| `CC_CHAN_TRACE_REQ_WAKE=1` | Trace requested-wake events |
| `CC_CHAN_TRACE_OBJ=<ptr>` | Filter the traces above to a single channel object address |
| `CC_NURSERY_CLOSING_RUNTIME_GUARD=1` | Recv that waits on a channel whose `close_on` owner is the current nursery fails with `EDEADLK` instead of deadlocking |
| `CC_WORKERS=n` | Set worker thread count |
| `CC_V2_STATS=1` | Dump V2 scheduler counters at exit |
| `CC_TASK_WAIT_STATS=1` + `CC_TASK_WAIT_STATS_DUMP=1` | Attribute `block_on` waits (spawn / fiber_v2 / poll) |

### Repro commands (common failures)
- Select deadlock:  
  `CC_DEADLOCK_ABORT=0 ./cc/bin/ccc run stress/lost_wakeup_hammer.ccs --timeout 5`
- Lock-free recv deadlock (intermittent):  
  `CC_DEADLOCK_ABORT=0 ./cc/bin/ccc run perf/perf_channel_throughput.ccs --timeout 5`
- Pipeline data loss:  
  `CC_DEADLOCK_ABORT=0 CC_CHAN_TRACE_FLOW=1 ./cc/bin/ccc run stress/pipeline_long.ccs --timeout 10`

### Deadlock output example

The detector runs unconditionally; when all workers stall for ~1s it prints a
banner like this (and exits 124 unless `CC_DEADLOCK_ABORT=0`):

```
╔══════════════════════════════════════════════════════════════╗
║                     DEADLOCK DETECTED                        ║
╚══════════════════════════════════════════════════════════════╝

Runtime state:
  V2 workers: 4 total, 4 idle (all idle is the stall signal)
  V2 fibers:  1 parked internal, 0 parked external-wait, 0 parked deadlock-suppressed
  Internal parked fibers:
  (parked) v2_fiber=0x... state=PARKED reason=chan_recv_wait_empty obj=0x... last_thread=1 external_wait_depth=0 suppress_depth=0
    chan state: ch=0x... cap=1 count=0 lf_count=0 inflight=0 closed=0 ... has_recv_waiters=1 has_send_waiters=0 ...
  fiber totals: total=2 parked=1 internal_parked=1 skipped_parked=0

  v2 sched: threads=4 idle=4 ready_queue=0 alive_fibers=2 stall_detect=4 dead_total=0
  ...
Aborting with exit code 124. Set CC_DEADLOCK_ABORT=0 to continue.
```

Channels created via `cc_chan_pair_create` also get `chan user: name=… site=…:…`
lines in the dump (the R2 channel metadata), pointing back at the user's
declaration site.

### What to look for in the dump
- `reason=chan_recv_wait_empty` with `closed=0` and `count=0` → channel never closed, but no sender is running. Usually the closing-nursery foot-gun (consumer inside the nursery that owns `close_on`) or a producer that exited early.
- `reason=chan_send_*` with `count == cap` → buffer full and no consumer draining; check consumer lifetime and buffer sizing.
- `parked external-wait` / `parked deadlock-suppressed` counts are excluded from the verdict — fibers inside `cc_external_wait_enter/leave` / `cc_deadlock_suppress_enter/leave` scopes do not trigger the detector.
- `inflight` > 0 on a stalled channel → an in-flight lock-free enqueue never completed; that is a runtime bug, not an application bug.

### Run-all integration
- `tools/run_all.ccs` writes logs to `tmp/run_all_logs/<test>.{stdout,stderr}.txt`.
- On failure, inspect the per-test stderr for deadlock context.
