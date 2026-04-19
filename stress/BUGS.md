# Open / Recently-Fixed Concurrency Bugs — Regression Tests

This file indexes the stress-level repros checked in during the April 2026
V1-strip / V2-hardening pass.  Each entry lists:

- the bug id and symptom,
- the canonical repro file,
- the observed failure rate on pre-fix code,
- whether the fix is landed.

New bugs discovered in the scheduler/channel layer should get a one-file
minimal repro here and be listed in this table.

| Bug | Area                              | Repro                                                   | Pre-fix failure rate          | Fix status |
|-----|-----------------------------------|---------------------------------------------------------|-------------------------------|------------|
| #1  | `cc_thread_spawn` silent drop     | `stress/thread_spawn_overflow_checksum.ccs`             | ~23% checksum mismatch (perf) | **Fixed**  |
| #2  | `@match` fiber-identity mismatch  | `stress/match_lost_wakeup_min_repro.ccs`                | ~97% abort/hang               | Open       |
| #3  | Buffered-channel drain truncation | `stress/buffered_drain_truncation_min_repro.ccs`        | ~7%-47% (depth-sensitive)     | Open       |

The broader pre-existing repros covering the same areas:

- `stress/lost_wakeup_hammer.ccs` — larger-fan version of bug #2.
- `tests/pipeline_repeat_debug.ccs` — larger 6-stage version of bug #3
  (fails ~35-47%, can deadlock).

## Bug #1 — `cc_thread_spawn` silent task drop  _(FIXED)_

### Symptom

`cc_thread_spawn` returned an INVALID `CCTask` whenever its executor's
bounded queue was full; `cc_block_on_intptr(INVALID)` returned 0,
corrupting any XOR checksum across joined task results.
`perf/perf_spawn_v2_vs_thread.ccs` caught this at ~23% rates.

### Fix

- `cc/runtime/exec.c`: added unbounded-growth mode + `cc_exec_submit_blocking`.
- `cc/runtime/scheduler.c`: scheduler executor now defaults to unbounded
  (`qcap=0`); `cc_thread_spawn` uses `cc_exec_submit_blocking`.
- `cc/runtime/task.c`: `cc_run_blocking_task` uses `cc_exec_submit_blocking`
  against its intentionally bounded I/O pool (backpressure, not silent drop).

### Regression

`stress/thread_spawn_overflow_checksum.ccs` sets `CC_SPAWN_QUEUE_CAP=4`
via `setenv()` at the top of `main()` before any spawn, spawns 1024 tasks
per iteration × 16 iterations, and XOR-checksums the joined results.
A silent drop anywhere would show up as a mismatched checksum within a
few spawns.  Should PASS 100% on current code.

## Bug #2 — `@match` fiber-identity mismatch  _(OPEN)_

### Symptom

Under concurrent load with `@match`, `mco_yield` is called while
executing on a different coroutine's stack, producing `rc=12`
(`MCO_NOT_ENOUGH_SPACE`) and a SIGABRT crash.  Heisenbug — any
instrumentation on `sched_v2_park` masks it.

Triage found the trigger is the `@match` harness itself, not park
semantics: swapping `@match { case rx.recv(&v) ... }` for direct
`rx.recv(&v)` on the same fiber layout makes the failure disappear.
Even `@match` with one case (no branch selection) is enough to fire it
at ~97% over 30 runs.

### Repro

`stress/match_lost_wakeup_min_repro.ccs` — 1 sender + 1 receiver, 1
unbuffered channel, `@match` with one case, 100k iterations.  Fails
as a SIGABRT within ~1 second.

### Open work

- Audit `sched_v2_signal` + `sched_v2_enqueue_runnable` for
  exactly-once enqueue.
- Audit `cc_chan_match_deadline` register / recheck / park window for
  lost-wakeup races.
- Add non-perturbing `in_queue` CAS assertion to catch double-push.

## Bug #3 — Buffered-channel drain truncation on close  _(OPEN)_

### Invariant being violated

> Close / cancel / error must NOT ADMIT NEW sends — it must not change
> recv behavior for items that were already enqueued.  Receivers must
> drain the buffer to empty before EPIPE is surfaced.

### Symptom

A multi-stage pipeline (producer → relay → ... → consumer) with buffered
channels sometimes loses the TAIL of the stream: the terminal consumer
exits its `cc_io_avail(chan_recv(rx, &v))` loop after the upstream has
closed but before draining the last few items in the ring.

Forensics on one captured run showed all 100 source items fully sent
through every stage (`s4=100`) but the terminal consumer only read 94,
with the missing items' values being exactly the last 6 in the stream
(sum = 603, matching values 98..103).

### Root cause (pending fix)

`cc__chan_recv_resolve_closed` in `cc/runtime/channel.c` uses
`ring_tail == ring_head` as its "truly empty" check for the MPMC ring
path, entered on a non-atomic read of `ch->closed`.  The closed read is
unsynchronized and the tail/head acquire loads aren't a compound
operation, so it is possible to observe `closed=1` via cache coherence
while the acquire-load of `ring_tail` returns a pre-publish value — the
resolver returns EPIPE while items with published `seq` are still in
the ring.

### Repro

`stress/buffered_drain_truncation_min_repro.ccs` — 4-stage pipeline,
100 items × 500 iterations, buffer cap 8.  Fails at ~7%.

### Planned fix (see channel_audit_fix_plan.md)

A. In `cc__chan_recv_resolve_closed`: seq_cst fence before the first
   `tail/head` empty-check + re-try dequeue after the fence.

B. In `cc__chan_close_common`: seq_cst fence before `ch->closed = 1`.

C. (Cleanup pass) Promote `ch->closed` to `_Atomic int` with
   acquire/release and eliminate all non-atomic reads.
