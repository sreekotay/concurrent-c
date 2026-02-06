# Debugging Concurrent-C programs (VS Code / Cursor)

This repo's driver (`ccc`) already defines stable output locations (see `spec/concurrent-c-build.md`):

- Generated C: `out/<stem>.c`
- Linked binary: `bin/<stem>`

## Quickstart: debug the currently open `.ccs`

This repo includes:

- `.vscode/tasks.json` (build/run via `./cc/bin/ccc`)
- `.vscode/launch.json` (LLDB launch config)

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

## Scheduler synchronization invariants (fiber scheduler)

Key invariants in `cc/runtime/fiber_sched.c` that should not be violated:

- `running_lock` protects `mco_resume()` from concurrent resume attempts.
- `cc__fiber_unpark()` is non-blocking and handles `PARKED -> QUEUED` and `PARKING -> QUEUED` (CAS).
  If the fiber is `RUNNING` or `QUEUED`, it sets `pending_unpark = 1`.
- `cc__fiber_park_if(flag, expected, reason)` only parks if `*flag == expected` and `pending_unpark == 0`.
  Uses a two-phase CAS: `RUNNING -> PARKING -> PARKED`. Either phase can be interrupted by unpark.
- `join_lock` + `join_waiter_fiber` provide a handshake so joiners never miss a wakeup.
  `done=1` and `state=FIBER_DONE` are set under the join lock.
- Wake counters are debug-only telemetry (if enabled) and are not used for correctness.
- Enqueue paths must transition to `QUEUED` exactly once (CAS from expected state); stale queue entries are dropped.
- Fiber state transitions: `CREATED -> QUEUED -> RUNNING -> PARKING -> PARKED -> QUEUED -> RUNNING -> DONE`.
  The `PARKING` state is visible to concurrent `unpark` calls and enables safe cancellation of the park.
- `pending_unpark` is a per-park-attempt latch, not a persistent flag. It must be cleared
  (`cc__fiber_clear_pending_unpark()`) before entering a new wait context (e.g., select park loop)
  to avoid consuming a stale signal from an unrelated prior operation.

If you add new scheduler behaviors, update these invariants and extend the stress tests.

## Channel/select debugging (lock-free + @match)

### Key invariants (what must stay true)
- Close stops admission: send must not accept new work after close.
- Recv drains in-flight work: recv may return close only when there is no buffered data and no in-flight enqueue remaining (`lfqueue_inflight` counter).
- Wait nodes must stay on the channel's wait list whenever the fiber might park. The "remove → dequeue → re-add" pattern is prohibited; use "check count under lock → stay-on-list-and-park" instead.
- Direct handoff (`notified == DATA`) must be checked before any buffer dequeue to avoid overwriting handed-off data.
- Unbuffered rendezvous: nursery cancellation is checked only *before* committing to the wait list, not inside the waiting loop. In-progress rendezvous operations must complete.
- Select must have a single winner; non-winners cancel and rearm; wake should force a recheck, not "consume" a wake.
- Notification values are typed: `CC_CHAN_NOTIFY_NONE` (0), `DATA` (1), `CANCEL` (2), `CLOSE` (3), `SIGNAL` (4).

### Environment flags

| Variable | Description |
|----------|-------------|
| `CC_CHAN_DEBUG=1` | Channel operation logs + global debug counter dump on deadlock |
| `CC_CHAN_DEBUG_VERBOSE=1` | Detailed select/wake/park logging (noisy — can cause timeouts) |
| `CC_CHAN_NO_LOCKFREE=1` | Force mutex-based channel path (isolate lock-free bugs) |
| `CC_DEBUG_DEADLOCK_RUNTIME=1` | Dump all fibers (parked + done) and per-channel state on deadlock |
| `CC_DEADLOCK_ABORT=0` | Continue after deadlock detection (for log capture) |
| `CC_DEBUG_JOIN=1` | Log fiber join/unpark operations to stderr |
| `CC_DEBUG_WAKE=1` | Log wake counter increments |
| `CC_PARK_DEBUG=1` | Log `park_if` decisions (skip reasons, pending_unpark) |

### Compile-time debug flags

| Flag | Description |
|------|-------------|
| `-DCC_DEBUG_SYSMON` | Log sysmon scaling decisions to stderr |
| `-DCC_DEBUG_FIBER` | Log fiber park/unpark edge cases |

**Note:** The old compile-time `-DCC_DEBUG_DEADLOCK` flag has been replaced by the runtime `CC_DEBUG_DEADLOCK_RUNTIME=1` environment variable. No recompile needed.

### Repro commands (common failures)
- Select deadlock:  
  `CC_CHAN_DEBUG=1 CC_CHAN_DEBUG_VERBOSE=1 CC_DEBUG_DEADLOCK_RUNTIME=1 CC_DEADLOCK_ABORT=0 ./cc/bin/ccc run stress/lost_wakeup_hammer.ccs --timeout 5`
- Lock-free recv deadlock (intermittent):  
  `CC_DEBUG_DEADLOCK_RUNTIME=1 CC_DEADLOCK_ABORT=0 ./cc/bin/ccc run perf/perf_channel_throughput.ccs --timeout 5`
- Pipeline data loss:  
  `CC_DEBUG_DEADLOCK_RUNTIME=1 CC_CHAN_DEBUG=1 CC_DEADLOCK_ABORT=0 ./cc/bin/ccc run stress/pipeline_long.ccs --timeout 10`

### Deadlock output example

With `CC_DEBUG_DEADLOCK_RUNTIME=1`:
```
╔══════════════════════════════════════════════════════════════╗
║                     DEADLOCK DETECTED                        ║
╚══════════════════════════════════════════════════════════════╝

Runtime state:
  Workers: 8 total (8 base, 0 temp), 8 unavailable
  Fibers:  2 parked, 4 completed, 6 spawned total, 2 pending

Parked fibers (waiting for unpark that will never come):
  [fiber 1] chan_send: buffer full, waiting for space at channel.c:2355 (obj=0x...)
  [chan 0x...] cap=8 count=8 closed=0 send_waiters=1 recv_waiters=0
    dbg: send_calls=10000 recv_calls=10000 enq_ok=9998 deq_ok=9998
    dbg: direct_send=1 direct_recv=1 recv_rm0=0 ...
  [fiber 2] chan_recv: buffer empty, waiting for data at channel.c:2798 (obj=0x...)
  [chan 0x...] cap=8 count=0 closed=0 send_waiters=0 recv_waiters=1

All fibers (every fiber ever created):
  [fiber 1] state=PARKED  done=0 pending_unpark=0 park_reason=chan_send ...
  [fiber 2] state=PARKED  done=0 pending_unpark=0 park_reason=chan_recv ...
  [fiber 3] state=DONE    done=1 pending_unpark=0 park_reason=none ...
```

### What to look for in logs
- `direct_send` != `direct_recv` → a direct handoff was written but never consumed. Check `notified == DATA` handling in recv.
- `recv_wake_no_waiter` > 0 → sender tried to signal a receiver but the wait list was empty. Potential gap in coverage.
- `send_calls` != `recv_calls` for a closed unbuffered channel → one side exited early (check for `ECANCELED` / nursery cancellation in the loop).
- `park_reason=chan_recv` with `closed=0` and `count=0` → channel not yet closed, but no data and no sender is running. Likely a lost close notification.
- `pending_unpark=1` on a `PARKED` fiber → unpark was latched but park committed anyway. This should not happen with `park_if`.
- Lots of `unpark_skip` in verbose logs → early unpark before a fiber parks (usually benign with `pending_unpark` latch).
- `CC_CHAN_DEBUG_VERBOSE=1` can slow execution enough to cause timeouts. Use it for targeted debugging, not stress runs.

### Run-all integration
- `tools/run_all.ccs` writes logs to `tmp/run_all_logs/<test>.{stdout,stderr}.txt`.
- On failure, inspect the per-test stderr for deadlock context.
