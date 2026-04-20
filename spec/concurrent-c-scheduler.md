# Concurrent-C Fiber Scheduler

Technical specification of the scheduler state machine, ownership rules, and
wake protocol.

## Architecture

The scheduler is a hybrid fiber/thread model:

- **Fiber**: a coroutine (`minicoro`) plus scheduler metadata. One OS-visible
  stack (~2 MB reserved with demand paging) per fiber.
- **Worker**: an OS thread that dequeues runnable fibers and runs them on
  their own stacks via `mco_resume`.
- **Ready queue**: a single, global, mutex-protected intrusive linked list of
  runnable fibers. All workers drain from the same queue.
- **Sysmon**: a single background thread that runs housekeeping (syscall-age
  eviction, deadline wakes, deadlock detection, safety-net wakes) on a fixed
  cadence.

There are no per-worker local queues, no inboxes, and no work stealing. All
fiber handoff goes through the ready queue.

Worker count defaults to `sysconf(_SC_NPROCESSORS_ONLN)`, capped at
`V2_MAX_THREADS` (256), overridable via `CC_V2_THREADS`.

### Goals

- No lost wakeups: every unpark on a parked fiber eventually produces
  execution.
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
SIGNAL_PENDING (0x10)  an unpark arrived while the fiber was RUNNING;
                       the post-resume commit path will re-enqueue
                       instead of parking.
```

All transitions use `memory_order_acq_rel` on the CAS success side and
`memory_order_acquire` on loads that observe published state.

### Transitions

```
spawn        : (new fiber)      → QUEUED
dispatch     : QUEUED           → RUNNING        (worker-side, CAS)
park commit  : RUNNING          → PARKED         (worker-side, CAS; success
                                                  requires flag bit clear)
yield        : RUNNING          → QUEUED         (worker-side, after
                                                  cooperative mco_yield with
                                                  yield_kind=YIELD)
signal       : PARKED           → QUEUED         (waker-side, CAS)
signal       : RUNNING          → RUNNING|SIGNAL_PENDING  (waker-side, CAS)
completion   : RUNNING          → DEAD           (worker-side, after coro DEAD)
release      : DEAD             → IDLE (pooled)
```

A signal against `QUEUED` is a no-op (the fiber is already runnable). A
signal against `IDLE` or `DEAD` is dropped.

## Fiber record

Defined in `sched_v2.c` as `struct fiber_v2`. Key fields:

- `mco_coro* coro` — coroutine handle; lazily bound on first dispatch.
- `_Atomic int state` — state word (above).
- `_Atomic int done` — set to 1 at completion, before notifying joiners.
- `void* (*entry_fn)(void*)`, `void* entry_arg`, `void* result`.
- `char result_buf[48]` — inline storage for task results; accessed via
  `cc_task_result_ptr` to avoid heap allocation on the hot path.
- `int yield_kind` — set by the fiber before `mco_yield` to distinguish
  voluntary yield (`V2_YIELD_YIELD`) from park (`V2_YIELD_PARK`).
- `const char* park_reason` — static string published for diagnostics.
- `void* park_obj` — pointer to the waitable the fiber is blocked on
  (channel, join target, etc.). Consulted by the deadlock detector.
- `struct timespec park_deadline`, `_Atomic int has_park_deadline` — see
  Deadline-aware park.
- `wake_primitive done_wake` — used by thread-context joiners.
- `cc__fiber* _Atomic join_waiter_fiber` — at most one fiber-context
  joiner.
- `CCNursery* saved_nursery`, `admission_nursery` — structured-concurrency
  context captured at spawn.
- `uint32_t deadlock_suppress_depth`, `external_wait_depth` — detector
  exemption counters.
- `fiber_v2* next`, `all_next` — free list + global all-fibers list
  intrusive links.

## Fiber pool

Fibers are pooled to amortize coroutine allocation (~2 MB stack each):

- Free list is a single lock-free Treiber stack (`g_v2.free_list`).
  Alloc pops with acquire CAS; free pushes with release CAS.
- Coroutine memory survives across reuse. `fiber_v2_free` calls
  `mco_uninit` (mark DEAD, run platform teardown) but leaves the allocation
  intact. `thread_v2_run_fiber` calls `mco_init` in place on the next
  dispatch, avoiding the fresh allocation.
- Generation counter on the fiber is bumped on each alloc for ABA-safe use
  by wait tickets.
- `all_fibers` is a singly-linked list of every fiber ever allocated,
  protected by `all_fibers_mu`. It is walked by sysmon for deadlock
  detection and deadline-expiration scanning.

## Ready queue

`v2_queue` is a doubly-free intrusive FIFO:

- Protected by `v2_slock`: `os_unfair_lock` on Apple, an atomic-flag
  spinlock with `sched_yield` backoff elsewhere. Critical section is a
  handful of pointer stores; mutex cost dominated profiles at high
  throughput.
- `count` is an `_Atomic size_t` kept relaxed-consistent with the linked
  list under the lock.
- `push` returns the pre-push depth (used by the wake-skip-depth
  optimization below).

There is no overflow list and no upper bound; the queue grows linearly in
the number of runnable fibers.

## Wake primitive

`wake_primitive` is the OS-level sleep/wake:

- Linux: `futex(FUTEX_WAIT)` / `FUTEX_WAKE`.
- macOS: `__ulock_wait` / `__ulock_wake` (stable private API used by
  libdispatch).
- Other: `pthread_cond_t` fallback.

It holds a single `_Atomic uint32_t value` counter. A waiter snapshots the
value under acquire; a waker increments and wakes one or all waiters. This
avoids the mutex/condvar pairing used elsewhere in the codebase.

Every worker owns one wake primitive; so does sysmon and every fiber's
`done_wake`.

## Spawn

`sched_v2_spawn[_in_nursery](fn, arg, nursery)`:

1. Allocate a `fiber_v2` from the free list or heap.
2. Populate `entry_fn`, `entry_arg`, `saved_nursery`, `admission_nursery`.
3. Do **not** create a coroutine. Leave `f->coro` as-is (NULL for fresh, or
   a dead but still-allocated mco_coro for a pooled fiber).
4. `atomic_store_explicit(&f->state, QUEUED, release)`.
5. Push onto the ready queue; `sched_v2_wake(-1)`.

Coroutine binding is deferred to the worker that first dispatches the
fiber (see Worker dispatch). This keeps the producer path to "alloc +
enqueue + wake" and moves the ~2 MB stack allocation off the spawn
caller, capping concurrent `mco_create` calls at the number of worker
threads.

## Worker dispatch

Workers run `thread_v2_main`. Each iteration:

1. **Admission** (optional, see `CC_V2_TARGET_ACTIVE`): CAS-increment
   `running_workers` only if the result would stay `<= target`. On failure,
   the worker skips the drain and goes straight to park.
2. **Drain**: `sched_v2_wake(tid)` self-drains the ready queue inline,
   popping and running fibers until the queue is empty.
3. **Post-drain identity check**: if `slot.generation != my_generation`,
   the worker has been evicted (see Sysmon) and exits without touching
   shared state.
4. **Park**: mark `is_idle=1`, increment `idle_workers`, `seq_cst` fence,
   snapshot `wake.value`, re-check the ready queue (Dekker recheck). If
   still empty, `wake_primitive_wait`.

Dispatch of a single fiber (`thread_v2_run_fiber`):

1. CAS `QUEUED → RUNNING`; if the state is not `QUEUED` the queue entry is
   a bug (asserted at stderr).
2. Lazy coroutine bind:
   - `coro == NULL`: `mco_create` with the entry trampoline and stack
     size.
   - `coro != NULL && mco_status == MCO_DEAD`: `mco_init` in place to
     reuse the pooled allocation; fall back to fresh allocation only if
     `mco_init` fails.
   - `coro != NULL && MCO_SUSPENDED`: resume a previously parked
     fiber.
3. Publish dispatch epoch (per-worker monotonic counter) so sysmon can
   detect kidnapped syscalls.
4. `mco_resume(coro)`.
5. Clear dispatch epoch.
6. Inspect post-resume state:
   - Coroutine finished → write `DEAD` + `done=1`, `seq_cst` fence,
     consume `join_waiter_fiber` (unpark), broadcast `done_wake`.
   - `yield_kind == YIELD` or `SIGNAL_PENDING` set → store `QUEUED`,
     re-enqueue.
   - Otherwise CAS `RUNNING → PARKED`. On CAS failure (another signaler
     beat us), re-enqueue.

## Park / yield

Fiber-side primitives are called from the fiber's own stack:

- `sched_v2_park()` sets `yield_kind = PARK`, calls `mco_yield`. Control
  returns to the worker, which runs the post-resume commit above.
- `sched_v2_yield()` sets `yield_kind = YIELD`, calls `mco_yield`. The
  commit path always re-enqueues.

The commit point for "this fiber is parked" is the successful
`RUNNING → PARKED` CAS on the worker stack, not on the fiber's own stack.
This ensures `PARKED` is only published once the fiber's stack is
quiescent — a signal observed before the commit turns into a re-enqueue.

### `park_if` contract

`CC_FIBER_PARK_IF(&flag, expected, reason)` parks the current fiber only if
the guard still holds. It is the primitive channels and join use.

Semantics:

1. Pre-park: `load_acquire(flag)`; if `!= expected`, return without
   parking.
2. Publish `park_reason` (and `park_deadline` if the `_until` variant).
3. `sched_v2_park()`.
4. Post-resume: re-check the deadline (if any) and return.

Waiters must loop on their own condition and tolerate spurious wakes; a
resume from `SIGNAL_PENDING` can return the fiber without any semantic
wake having been delivered.

## Signal (unpark)

`sched_v2_signal(f)` is the only wake primitive. It is called from:

- channel sends/receives closing the send/recv gap,
- join completion,
- sysmon deadline expiry,
- I/O readiness callbacks,
- nursery cancellation.

The implementation is a single CAS loop over `f->state`:

```
QUEUED                  → return (already runnable)
RUNNING                 → CAS to RUNNING|SIGNAL_PENDING; return
RUNNING|SIGNAL_PENDING  → return (already marked)
PARKED                  → CAS to QUEUED; on success push to ready queue
IDLE / DEAD             → drop
```

No per-fiber latch is needed: the `SIGNAL_PENDING` bit fills the role of
the prior `pending_unpark` flag, and because it lives in the same word as
`state` the RUNNING→PARKED commit and the signal cannot race in a way
that strands the wake.

## Wake (worker kick)

`sched_v2_wake` is separate from `sched_v2_signal`. Signal makes a fiber
runnable (ready-queue state); wake decides whether a worker thread needs
to be booted to drain it.

Called from two contexts:

### Self-drain (`worker_hint == tls_v2_thread_id`)

Used inside `thread_v2_main`'s drain phase. Pops fibers off the ready
queue and runs them inline, staying `is_idle=0` throughout. When the
queue empties, spin for a bounded budget (`CC_V2_SPIN_BEFORE_PARK`) before
returning to the outer park cycle; this hides fibers that arrive in the
microsecond-wide gap between "queue empty" and `__ulock_wait`.

### External (`worker_hint < 0`)

Called from producers (signal path, spawn path, kqueue thread, sysmon).

1. `seq_cst` fence. Pairs with the worker's `seq_cst` fence between
   `is_idle=1` and the ready-queue re-check. On weakly-ordered
   architectures release-store + acquire-load is insufficient: both sides
   can reorder the load ahead of the store, miss each other's publish,
   and both go to sleep.
2. If `idle_workers == 0`, return.
3. While `ready_queue.count > 0 && idle_workers > 0`, claim one idle
   worker (CAS `is_idle 1→0`), decrement `idle_workers`, and
   `wake_primitive_wake_one`.
4. If `TARGET_ACTIVE` is set and `running_workers >= target`, break
   (producer-side admission gate).

### Wake skip depth (`CC_V2_WAKE_SKIP_DEPTH`)

`sched_v2_enqueue_runnable` skips the external wake when the pre-push
queue depth is already `>= wake_skip_depth` (default 4). A drainer is
already on the queue and will self-drain to the new item. The safety net
for the push-then-park race is sysmon's every-tick unconditional wake
(below).

## Deadline-aware park

Fibers that need to give up after a wall-clock deadline (e.g.
`@with_deadline` around a blocking channel op) publish the deadline
before yielding:

- `sched_v2_fiber_set_park_deadline(f, deadline)`: writes
  `park_deadline`, sets `has_park_deadline` via atomic exchange, and
  increments a global counter if the flag transitioned 0→1.
- Sysmon's `wake_expired_parkers` walks `all_fibers` every tick. For
  each fiber with `has_park_deadline && state == PARKED && now >=
  deadline`, it calls `sched_v2_signal(f)`. The fiber resumes, re-checks
  the clock in `cc__fiber_park_if_impl`, observes the deadline, and
  returns `ETIMEDOUT` to its caller.
- The fiber clears the deadline on its own resume side; the counter is
  decremented there.

Resolution is bounded by `V2_SYSMON_INTERVAL_MS` (20 ms). The global
counter short-circuits the walk to a single relaxed load when no
deadlines are in flight.

## Join

`sched_v2_join(f, out_result)`:

1. Fast path: `load_acquire(f->done)`; if set, return immediately.
2. Optional spin: up to `CC_V2_JOIN_SPIN` iterations checking `done`
   (default 0 — spinning hasn't paid off in measured workloads).
3. Fiber-context joiner:
   - Publish `this` into `f->join_waiter_fiber` with release.
   - `seq_cst` fence. Pairs with the completer's `seq_cst` fence
     (after writing `done`). Without both fences, ARM64 can reorder the
     publisher's load of `done` before its store of
     `join_waiter_fiber`, producing a lost wake.
   - Loop: `CC_FIBER_PARK_IF(&f->done, 0, "sched_v2_join")` until
     `done` observed.
4. Thread-context joiner: `wake_primitive_wait(&f->done_wake, snapshot)`
   loop on `f->done`.
5. Completion (already shown under Worker dispatch) writes `done=1`,
   fences, consumes the `join_waiter_fiber` (tagged
   `UNPARK_REASON_TASK_DONE`), and broadcasts `done_wake`.

Only one fiber joiner is supported per target. Concurrent join attempts
race on the `join_waiter_fiber` CAS; later joiners must fall back to
thread-style `done_wake`.

## Sysmon

Single thread, periodic wake interval `V2_SYSMON_INTERVAL_MS` (20 ms). Per
tick:

1. **Syscall-age eviction.** Scan worker slots; any worker whose
   `dispatch_epoch` matches the sysmon-local cache from the previous
   tick (and is non-zero) has been running the same fiber for at least
   one tick. If the ready queue has backlog and the orphan count is
   below `V2_ORPHAN_SAFETY_CAP`, evict the worker in place (see
   Sysmon eviction).
2. **Deadline wakes.** `wake_expired_parkers()` signals parked fibers
   whose `park_deadline` has passed.
3. **Deadlock check.** `sched_v2_check_deadlock()` (see below).
4. **Safety-net wake.** If `ready_queue.count > 0 && idle_workers > 0`,
   issue `sched_v2_wake(-1)` unconditionally. Bounds the latency of any
   wake that was skipped via `WAKE_SKIP_DEPTH` to one tick.
5. **Stall diagnostics.** If `run_dead` hasn't advanced for
   `STALL_DIAG_TICKS` (~2 s), print a diagnostic snapshot of scheduler
   counters and fiber states.

### Sysmon eviction (kidnapped syscalls)

When a fiber makes a blocking kernel syscall (e.g. `read(2)` on a
non-async path), its worker thread is stuck in the kernel and cannot
drain the ready queue. Rather than grow the pool, sysmon replaces the
worker in place:

1. `pthread_detach` the old handle.
2. Reset per-slot transient state: wake primitive, `is_idle`,
   `dispatch_epoch`.
3. `atomic_fetch_add` the slot's `generation` — this is the identity
   token the replacement will read at entry.
4. `pthread_create` a new worker into the same slot index.

The kidnapped worker runs its fiber to completion in the kernel. When it
returns to `thread_v2_main`, the `slot.generation != my_generation`
check trips and it exits without touching shared state. The replacement
worker has fully taken over; the orphan self-reclaims via
`pthread_detach`.

Budget: at most `min(ready_queue.count, V2_ORPHAN_SAFETY_CAP -
live_orphans)` evictions per tick. Disabled by `CC_V2_SYSMON_DETACH=0`.

## Deadlock detection

`sched_v2_check_deadlock` (called from sysmon) evaluates:

- All workers are `is_idle == 1`.
- Ready queue is empty.
- At least one fiber is `PARKED` with both
  `external_wait_depth == 0` and `deadlock_suppress_depth == 0`.

If all three hold and the latch has persisted ≥ 1 s, the detector
prints a diagnostic banner and `_exit(124)`. `CC_DEADLOCK_ABORT=0`
prints the banner without exiting.

### Exemptions

A parked fiber is exempt from counting toward deadlock if:

- `external_wait_depth > 0` (entered via `cc_external_wait_enter`):
  something outside the scheduler — a kqueue callback, another thread —
  is expected to produce progress.
- `deadlock_suppress_depth > 0` (entered via
  `cc_deadlock_suppress_enter`): caller has asserted this park can
  legitimately outlast the detector's timer.

### External-progress exemption

Even when every parked fiber is fair game for the detector, if
`external_waits (parked-external + external_wait_threads) > 0` AND
every internally-parked fiber is blocked on `recv` of an **open**
channel, the detector resets the latch. The premise is that the
external source can still close the channel or send on it; "deadlock"
implies no actor can make progress, which doesn't apply here.

## Scheduler boundary (channel/I-O integration)

External wait sources use `cc_sched_fiber_wait[_until|_many]` in
`fiber_sched_boundary.c`. The boundary drives a four-phase protocol:

1. **try_complete**: optimistic non-blocking path. If the waitable can
   satisfy the wait without parking (buffer has data, rendezvous partner
   present, etc.), return `OK`.
2. **publish**: install the fiber as a waiter on the waitable. After this
   point, wakers may race the park.
3. **re-check**: another `try_complete` call catches the case where the
   completion landed between step 1 and step 2.
4. **park**: either the waitable's custom `park` op (typically
   `CC_FIBER_PARK_IF` on a waiter-local flag) or the default
   `cc__fiber_park_reason`. Deadline-aware waits go through the
   waitable's `park_until` op, which is responsible for the
   `set_park_deadline` / `clear_park_deadline` pair around the inner
   park.
5. **post-park try_complete**: a last check for wakes that arrived
   between publish and commit-to-PARKED.

`cc_sched_fiber_wait_many` extends this with a shared
`signaled_flag` + `selected_index`. Any waker wins by CAS on
`selected_index`; losers observe a non-negative `selected_index` on
their own recheck and cancel out. The boundary parks on `signaled_flag`
via `cc_sched_wait_on_flag`.

## Memory ordering

Required (asserted by implementation):

- `v2_queue` push/pop: store/load of links under `v2_slock`; `count` may
  be relaxed.
- `f->state`: CAS with `memory_order_acq_rel` on success,
  `memory_order_acquire` on failure; plain stores use
  `memory_order_release`, loads use `memory_order_acquire`.
- Worker Dekker pair: full `atomic_thread_fence(memory_order_seq_cst)`
  between `is_idle=1` (release store) and the subsequent
  `ready_queue.count` acquire load. Matching fence on the producer side
  between `count++` (under the queue lock) and the `idle_workers`
  acquire load.
- Join Dekker pair: `seq_cst` fence on the completer between
  `done=1` (release store) and the `join_waiter_fiber` exchange;
  matching `seq_cst` fence on the joiner between the
  `join_waiter_fiber` publish and the `done` load.
- `SIGNAL_PENDING` bit: set via CAS on `state` (same atomic object), so
  the handoff to the post-resume commit is naturally ordered.

Diagnostic counters (`g_v2_*`) use `memory_order_relaxed` and must not
be consulted for correctness.

## Configuration

### Environment variables

| Variable                         | Effect                                                                                                  |
| -------------------------------- | ------------------------------------------------------------------------------------------------------- |
| `CC_V2_THREADS=N`                | Fix worker count (default: online CPUs, capped at 256).                                                 |
| `CC_WORKERS=N`                   | Back-compat alias for `CC_V2_THREADS`.                                                                  |
| `CC_V2_TARGET_ACTIVE=N`          | Cap on concurrently-active (non-parked) workers. 0 disables.                                            |
| `CC_V2_PARK_EXTRAS_AT_STARTUP=1` | Non-primary workers park at startup rather than all draining the first enqueue.                         |
| `CC_V2_SPIN_BEFORE_PARK=N`       | cpu_relax iterations a worker polls the queue before committing to `__ulock_wait`. 0 disables.          |
| `CC_V2_WAKE_SKIP_DEPTH=N`        | Skip external wake when pre-push queue depth ≥ N. 0 always wakes. Default 4.                            |
| `CC_V2_JOIN_SPIN=N`              | Iterations a joiner busy-spins on `done` before parking. Default 0.                                     |
| `CC_V2_SYSMON_DETACH=0`          | Disable syscall-age eviction (pool hard-capped at `CC_V2_THREADS`).                                     |
| `CC_V2_STATS=1`                  | Enable hot-path stat counters and dump them at exit.                                                    |
| `CC_V2_SYSMON_STATS=1`           | Enable stat counters (no atexit dump).                                                                  |
| `CC_DEADLOCK_ABORT=0`            | Print deadlock banner but do not `_exit(124)`.                                                          |

### Compile-time constants (sched_v2.c)

| Name                         | Value                                 | Role                                                                              |
| ---------------------------- | ------------------------------------- | --------------------------------------------------------------------------------- |
| `V2_MAX_THREADS`             | 256                                   | Cap on active worker slots.                                                       |
| `V2_GLOBAL_QUEUE_SIZE`       | 4096                                  | (Unused; legacy ring size constant.)                                              |
| `V2_FIBER_STACK_SIZE`        | 2 MiB (release) / 8 MiB (debug)       | Per-fiber coroutine stack.                                                        |
| `V2_SYSMON_INTERVAL_MS`      | 20                                    | Sysmon tick.                                                                      |
| `V2_SYSMON_SYSCALL_AGE_NS`   | 20 ms                                 | Age threshold for in-place worker eviction.                                       |
| `V2_ORPHAN_SAFETY_CAP`       | 4096                                  | Maximum concurrent orphans before eviction is skipped for a tick.                 |
| `SCHED_V2_DEADLOCK_PERSIST_MS` | 1000                                | Latch duration before the detector fires.                                         |

## Implementation files

- `cc/runtime/sched_v2.c`, `sched_v2.h` — scheduler core, sysmon,
  deadlock detector, join, spawn.
- `cc/runtime/fiber_sched.c`, `fiber_internal.h` — public
  `cc__fiber_*` API (park, unpark, current, park-if, sleep). Thin shim
  over `sched_v2`.
- `cc/runtime/fiber_sched_boundary.c`, `fiber_sched_boundary.h` —
  `cc_sched_fiber_wait[_until|_many]` integration point for channels
  and I/O.
- `cc/runtime/wake_primitive.h` — OS-level wait/wake
  (futex / __ulock / condvar fallback).
- `cc/runtime/channel.c` — channel operations; consumes the scheduler
  boundary and park/signal primitives.
- `cc/runtime/nursery.c` — structured concurrency on top of
  `sched_v2_spawn_in_nursery` and `sched_v2_join`.
- `cc/runtime/minicoro.h` — coroutine implementation.
- `cc/include/ccc/cc_sched.cch` — public API declarations.
