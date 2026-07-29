# Concurrent-C Fiber Scheduler

Status: draft specification; the V2 scheduler is the shipped default path.

Normative contract for the fiber scheduler: state machine, ownership, park/wake,
sysmon duties, and externally observable guarantees. The shipped runtime path
is `cc/runtime/sched_v2.c` (global ready queue, worker pool, sysmon). Public
fiber APIs in `fiber_sched.c` / `fiber_sched_boundary.c` shim onto that path.

## Architecture

Hybrid fiber/thread model:

- **Fiber**: a coroutine (`minicoro`) plus scheduler metadata. One OS-visible
  stack per fiber (~2 MiB reserved with demand paging in optimized builds;
  larger in unoptimized builds).
- **Worker**: an OS thread that dequeues runnable fibers and runs them on
  their own stacks via `mco_resume`.
- **Ready queue**: a single, global, mutex-protected intrusive linked list of
  runnable fibers. All workers drain the same queue.
- **Sysmon**: one background thread that runs housekeeping on a fixed cadence
  (unchanged-dispatch eviction, deadline wakes, deadlock detection,
  safety-net wakes).

There are no per-worker local queues, no inboxes, and no work stealing. All
fiber handoff goes through the ready queue.

Worker count defaults to `sysconf(_SC_NPROCESSORS_ONLN)`, capped at
`V2_MAX_THREADS` (256), overridable via `CC_V2_THREADS` (or `CC_WORKERS`).

### Correctness goals

- No lost wakeups: every signal on a parked (or park-racing) fiber eventually
  produces execution.
- No double-schedule: a fiber is on the ready queue at most once at any
  instant.
- No concurrent stack use: at most one worker executes a given fiber's
  coroutine at a time.

## Fiber state machine

Each fiber has `_Atomic int state` holding one of five base states and one
flag:

```
IDLE     (0)  pooled / freshly allocated; not referenced by the scheduler
QUEUED   (1)  on the global ready queue
RUNNING  (2)  owned by exactly one worker, executing on its stack
PARKED   (3)  blocked; waiting for an external signal
DEAD     (4)  coroutine returned; joinable
```

Flag (OR-ed into the state word):

```
SIGNAL_PENDING (0x10)  a signal arrived while the fiber was RUNNING;
                       the post-resume commit path re-enqueues instead of
                       parking.
```

Transitions use `memory_order_acq_rel` on successful CAS and
`memory_order_acquire` on loads that observe published state.

### Transitions

```
spawn        : (new fiber)      → QUEUED
dispatch     : QUEUED           → RUNNING        (worker-side, CAS)
park commit  : RUNNING          → PARKED         (worker-side, CAS; success
                                                  requires SIGNAL_PENDING clear)
yield        : RUNNING          → QUEUED         (worker-side, after
                                                  cooperative yield)
signal       : PARKED           → QUEUED         (waker-side, CAS)
signal       : RUNNING          → RUNNING|SIGNAL_PENDING  (waker-side, CAS)
completion   : RUNNING          → DEAD           (worker-side, after coro DEAD)
release      : DEAD             → IDLE (pooled)
```

A signal against `QUEUED` is a no-op. A signal against `IDLE` or `DEAD` is
dropped.

## Fiber record

Defined in `sched_v2.c` as `struct fiber_v2`. Fields that affect observable
behavior:

- `mco_coro* coro` — coroutine handle; lazily bound on first dispatch.
- `_Atomic int state` — state word (above).
- `_Atomic int done` — set to 1 at completion, before notifying joiners.
- `void* (*entry_fn)(void*)`, `void* entry_arg`, `void* result`.
- `char result_buf[48]` — inline task-result storage (`cc_task_result_ptr`).
- `int yield_kind` — voluntary yield vs park before `mco_yield`.
- `const char* park_reason`, `void* park_obj` — diagnostics / deadlock walk.
- `struct timespec park_deadline`, `_Atomic int has_park_deadline` — see
  Deadline-aware park.
- `wake_primitive done_wake` — thread-context joiners.
- `cc__fiber* _Atomic join_waiter_fiber` — at most one fiber-context joiner.
- `CCNursery* saved_nursery`, `admission_nursery` — structured-concurrency
  context captured at spawn; nursery queries follow the fiber, not the OS
  thread.
- `uint32_t deadlock_suppress_depth`, `external_wait_depth` — detector
  exemptions.
- `uint64_t generation`, `_Atomic uint64_t wait_ticket` — pooled-fiber ABA
  defense for waiters / tickets.
- Intrusive links for the free list and the global `all_fibers` list.

## Fiber pool

Fibers are pooled to amortize coroutine allocation:

- Free list is a lock-free Treiber stack. Alloc pops with acquire CAS; free
  pushes with release CAS.
- Coroutine memory survives reuse: free path uninits without releasing the
  stack allocation; next dispatch re-inits in place when possible.
- Generation bumps on each alloc for ABA-safe wait tickets.
- `all_fibers` is every fiber ever allocated (under `all_fibers_mu`), walked
  by sysmon for deadlock detection and deadline expiry.

## Ready queue

`v2_queue` is a doubly-linked intrusive FIFO:

- Protected by a short-critical-section lock (`os_unfair_lock` on Apple;
  atomic-flag spinlock with `sched_yield` backoff elsewhere).
- `count` is an `_Atomic size_t` kept consistent with the list under the lock.
- Push returns the pre-push depth (used by wake-skip-depth).

There is no overflow list and no upper bound; the queue grows with the number
of runnable fibers.

## Wake primitive

`wake_primitive` is the OS-level sleep/wake:

- Linux: `futex(FUTEX_WAIT)` / `FUTEX_WAKE`.
- macOS: `__ulock_wait` / `__ulock_wake`.
- Other: `pthread_cond_t` fallback.

It holds a single `_Atomic uint32_t value` counter. Waiters snapshot under
acquire; wakers increment and wake one or all. Every worker owns one wake
primitive; so do sysmon and every fiber's `done_wake`.

## Spawn

`sched_v2_spawn[_in_nursery](fn, arg, nursery)`:

1. Allocate a `fiber_v2` from the free list or heap.
2. Populate entry metadata and nursery pointers.
3. Do **not** create a coroutine on the spawn path.
4. Store `state = QUEUED` (release).
5. Push onto the ready queue; `sched_v2_wake(-1)`.

Coroutine binding is deferred to the first dispatching worker. Concurrent
`mco_create` calls are capped by the worker count.

## Worker dispatch

Workers run `thread_v2_main`. Each iteration:

1. **Admission** (optional, `CC_V2_TARGET_ACTIVE`): CAS-increment
   `running_workers` only if the result stays `<= target`. On failure, skip
   the drain and park.
2. **Drain**: self-drain the ready queue (`sched_v2_wake(tid)`), running
   fibers until empty.
3. **Post-drain identity check**: if `slot.generation != my_generation`, the
   worker was evicted and exits without touching shared state.
4. **Park**: mark idle, fence, snapshot `wake.value`, re-check the ready
   queue (Dekker recheck). If still empty, wait on the wake primitive.

Dispatch of one fiber (`thread_v2_run_fiber`):

1. CAS `QUEUED → RUNNING`.
2. Lazy coroutine bind (`mco_create`, in-place `mco_init` for pooled DEAD
   coroutines, or resume a previously parked fiber).
3. Publish the dispatch epoch for sysmon unchanged-dispatch detection.
4. `mco_resume(coro)`.
5. Post-resume:
   - finished → `DEAD` + `done=1`, fence, unpark `join_waiter_fiber`,
     broadcast `done_wake`.
   - voluntary yield or `SIGNAL_PENDING` → store `QUEUED`, re-enqueue.
   - otherwise CAS `RUNNING → PARKED`; on CAS failure (signal won),
     re-enqueue.

## Park / yield

Fiber-side primitives (called on the fiber stack):

- `sched_v2_park()` — yield with park intent; worker commits park.
- `sched_v2_yield()` — yield with re-enqueue intent.

The commit point for “this fiber is parked” is the successful
`RUNNING → PARKED` CAS on the worker stack, after the fiber stack is
quiescent. A signal observed before that commit becomes a re-enqueue.

### `park_if` contract

`CC_FIBER_PARK_IF(&flag, expected, reason)` parks only if the guard still
holds. Channels and join use it.

1. Pre-park: `load_acquire(flag)`; if `!= expected`, return without parking.
2. Publish `park_reason` (and `park_deadline` for the `_until` variant).
3. `sched_v2_park()`.
4. Post-resume: re-check deadline (if any) and return.

Waiters must loop on their own condition and tolerate spurious wakes.
`SIGNAL_PENDING` recovery may resume a fiber without a semantic wake.

In fiber context, blocking operations must park the fiber; they must not
block the OS worker thread.

## Signal (unpark)

`sched_v2_signal(f)` is the fiber wake entry point. Callers include channel
send/recv completion, join completion, sysmon deadline expiry, I/O readiness,
and nursery cancellation.

CAS loop over `f->state`:

```
QUEUED                  → return (already runnable)
RUNNING                 → CAS to RUNNING|SIGNAL_PENDING; return
RUNNING|SIGNAL_PENDING  → return
PARKED                  → CAS to QUEUED; on success push ready queue
IDLE / DEAD             → drop
```

`SIGNAL_PENDING` lives in the same atomic word as `state`, so the
RUNNING→PARKED commit and a concurrent signal cannot strand a wake.

## Wake (worker kick)

`sched_v2_wake` is separate from `sched_v2_signal`. Signal makes a fiber
runnable; wake decides whether a worker must drain the queue.

### Self-drain (`worker_hint == tls thread id`)

Used inside the worker drain phase. Pops and runs fibers until the queue is
empty, then optionally spins (`CC_V2_SPIN_BEFORE_PARK`) before returning to
the outer park cycle.

### External (`worker_hint < 0`)

Called from producers (signal, spawn, I/O thread, sysmon):

1. `seq_cst` fence paired with the worker's idle fence.
2. If `idle_workers == 0`, return.
3. While ready work and idle workers remain, claim one idle worker and
   `wake_primitive_wake_one`.
4. If `CC_V2_TARGET_ACTIVE` is set and `running_workers >= target`, stop.

### Wake skip depth (`CC_V2_WAKE_SKIP_DEPTH`)

Enqueue skips the external wake when pre-push depth is already
`>= wake_skip_depth` (default 4). Sysmon's every-tick unconditional wake
bounds the latency of a skipped wake to one sysmon interval.

## Deadline-aware park

Fibers that give up after a wall-clock deadline publish it before yielding:

- `sched_v2_fiber_set_park_deadline(f, deadline)` writes the deadline and
  sets `has_park_deadline`.
- Sysmon walks `all_fibers` each tick and `sched_v2_signal`s parked fibers
  whose deadline has passed. The fiber re-checks the clock and returns
  timeout to its caller (e.g. `ETIMEDOUT` under `@with_deadline`).
- The fiber clears the deadline on resume.

Resolution is bounded by `V2_SYSMON_INTERVAL_MS` (20 ms).

## Join

`sched_v2_join(f, out_result)`:

1. Fast path: `load_acquire(f->done)`; return if set.
2. Optional spin: up to `CC_V2_JOIN_SPIN` iterations (default 0).
3. Fiber-context joiner: publish into `join_waiter_fiber`, `seq_cst` fence
   paired with the completer, then `CC_FIBER_PARK_IF(&f->done, 0, …)` until
   done.
4. Thread-context joiner: wait on `done_wake` until `done`.
5. Completion writes `done=1`, fences, signals the fiber joiner (if any),
   and broadcasts `done_wake`.

Only one fiber joiner is supported per target. Concurrent join attempts race
on `join_waiter_fiber`; losers use thread-style `done_wake`.

## Sysmon

Single thread, interval `V2_SYSMON_INTERVAL_MS` (20 ms). Per tick:

1. **Unchanged-dispatch eviction.** A worker whose `dispatch_epoch` is unchanged
   across a tick (and non-zero) has been running the same fiber for at least
   one tick. If the ready queue has backlog and orphans are below
   `V2_ORPHAN_SAFETY_CAP`, replace that worker in place (detach old, bump
   slot generation, create a new worker in the same slot). The kidnapped
   worker finishes its fiber, sees the generation mismatch, and exits.
   Disabled by `CC_V2_SYSMON_DETACH=0`.
2. **Deadline wakes.** Signal parked fibers past `park_deadline`.
3. **Deadlock check.** See below.
4. **Safety-net wake.** If `ready_queue.count > 0 && idle_workers > 0`,
   `sched_v2_wake(-1)`.
5. **Stall diagnostics.** If progress counters stall for ~2 s, print a
   diagnostic snapshot.

## Deadlock detection

`sched_v2_check_deadlock` (sysmon) fires when all of the following hold for
at least `SCHED_V2_DEADLOCK_PERSIST_MS` (1000 ms):

- All workers are idle.
- Ready queue is empty.
- At least one fiber is `PARKED` with `external_wait_depth == 0` and
  `deadlock_suppress_depth == 0`.

On fire: print a diagnostic banner and `_exit(124)`. `CC_DEADLOCK_ABORT=0`
prints the banner without exiting.

### Exemptions

A parked fiber does not count toward deadlock if:

- `external_wait_depth > 0` (`cc_external_wait_enter`): progress is expected
  from outside the scheduler (I/O callback, foreign thread, etc.).
- `deadlock_suppress_depth > 0` (`cc_deadlock_suppress_enter`): the caller
  asserts this park may outlast the detector timer.

### External-progress exemption

If `external_waits > 0` and every internally parked fiber is blocked on
`recv` of an **open** channel, the detector resets its latch. An external
actor can still send or close; that is not a closed system deadlock.

## Scheduler boundary (channel / I/O)

External wait sources use `cc_sched_fiber_wait[_until|_many]` in
`fiber_sched_boundary.c`:

1. **try_complete** — non-blocking fast path.
2. **publish** — install the fiber as a waiter (wakers may race from here).
3. **re-check** — catch completion between steps 1 and 2.
4. **park** — waitable-specific park (typically `CC_FIBER_PARK_IF`) or the
   default park reason. Deadline waits set/clear park deadlines around the
   inner park.
5. **post-park try_complete** — catch wakes that arrived between publish and
   park commit.

`cc_sched_fiber_wait_many` uses a shared `signaled_flag` + `selected_index`.
Any waker wins by CAS on `selected_index`; losers cancel out. After a
multi-wait wakes, losing wait nodes must be deregistered before returning to
user code.

## Memory ordering

Required:

- Ready-queue push/pop: link updates under the queue lock; `count` may be
  relaxed.
- `f->state`: CAS `acq_rel` on success, `acquire` on failure; plain stores
  `release`, loads `acquire`.
- Worker Dekker pair: `seq_cst` fence between idle publish and ready-queue
  re-check; matching fence on the producer between enqueue and idle-worker
  observation.
- Join Dekker pair: `seq_cst` fence on the completer between `done=1` and
  consuming `join_waiter_fiber`; matching fence on the joiner between
  publishing the waiter and loading `done`.
- `SIGNAL_PENDING`: same atomic object as `state`, so handoff to the
  post-resume commit is ordered with park commit.

Diagnostic counters use `memory_order_relaxed` and must not be used for
correctness.

## Externally observable guarantees

- Spawn eventually runs the entry function on some worker unless the program
  exits first.
- Parked fibers resume only after a signal (or a spurious/deadline wake);
  waiters must re-check their conditions.
- A signal that races a park attempt is not lost (`SIGNAL_PENDING` or
  PARKED→QUEUED).
- At most one worker runs a given fiber stack at a time.
- Deadline parks surface timeout to the caller within about one sysmon
  interval after expiry.
- Deadlock among internally parked fibers (no external-wait / suppress
  exemption) aborts with exit code 124 by default.
- Fiber-context nursery identity follows the fiber across worker handoff
  (`saved_nursery`), including after sysmon eviction replaces a worker.

## Configuration

### Environment variables

| Variable                         | Effect                                                                                         |
| -------------------------------- | ---------------------------------------------------------------------------------------------- |
| `CC_V2_THREADS=N`                | Fix worker count (default: online CPUs, capped at 256).                                        |
| `CC_WORKERS=N`                   | Alias for `CC_V2_THREADS` when the latter is unset.                                            |
| `CC_V2_TARGET_ACTIVE=N`          | Cap concurrently active (non-parked) workers. 0 disables.                                      |
| `CC_V2_PARK_EXTRAS_AT_STARTUP=1` | Non-primary workers park at startup rather than all draining the first enqueue.                |
| `CC_V2_SPIN_BEFORE_PARK=N`       | Queue poll iterations before `__ulock_wait` / futex wait. 0 disables.                          |
| `CC_V2_WAKE_SKIP_DEPTH=N`        | Skip external wake when pre-push depth ≥ N. 0 always wakes. Default 4.                         |
| `CC_V2_JOIN_SPIN=N`              | Joiner busy-spin iterations on `done` before parking. Default 0.                               |
| `CC_V2_CORO_POOL_MAX=N`          | High-water cap for pooled coroutine allocations.                                               |
| `CC_V2_SYSMON_DETACH=0`          | Disable unchanged-dispatch eviction (pool hard-capped at `CC_V2_THREADS`).                    |
| `CC_V2_STATS=1`                  | Enable hot-path stat counters and dump them at exit.                                           |
| `CC_V2_SYSMON_STATS=1`           | Enable stat counters without atexit dump.                                                      |
| `CC_DEADLOCK_ABORT=0`            | Print deadlock banner but do not `_exit(124)`.                                                 |
| `CC_DEADLOCK_PERSIST_MS=N`       | Override the deadlock latch duration (default 1000).                                           |

### Compile-time constants (`sched_v2.c`)

| Name                           | Value                           | Role                                                              |
| ------------------------------ | ------------------------------- | ----------------------------------------------------------------- |
| `V2_MAX_THREADS`               | 256                             | Cap on active worker slots.                                       |
| `V2_FIBER_STACK_SIZE`          | 2 MiB (opt) / 8 MiB (debug)     | Per-fiber coroutine stack.                                        |
| `V2_SYSMON_INTERVAL_MS`        | 20                              | Sysmon tick.                                                      |
| `V2_ORPHAN_SAFETY_CAP`         | 4096                            | Max concurrent orphans before eviction is skipped for a tick.     |
| `SCHED_V2_DEADLOCK_PERSIST_MS` | 1000                            | Latch duration before the detector fires.                         |

## Implementation files

- `cc/runtime/sched_v2.c`, `sched_v2.h` — scheduler core, sysmon, deadlock
  detector, join, spawn.
- `cc/runtime/fiber_sched.c`, `fiber_internal.h` — public `cc__fiber_*` API
  (park, unpark, current, park-if, sleep); shim over `sched_v2` on the
  default path.
- `cc/runtime/fiber_sched_boundary.c`, `fiber_sched_boundary.h` —
  `cc_sched_fiber_wait[_until|_many]` for channels and I/O.
- `cc/runtime/wake_primitive.h` — OS wait/wake.
- `cc/runtime/channel.c`, `nursery.c` — consumers of park/signal/spawn/join.
- `cc/runtime/minicoro.h` — coroutine implementation.
- `cc/include/ccc/cc_sched.cch` — public API declarations.

Operational testing notes live in `docs/scheduler-ops-runbook.md`.
