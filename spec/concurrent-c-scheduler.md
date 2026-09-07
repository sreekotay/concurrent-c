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
The live pool starts at one worker and grows on demand up to that cap;
workers are never culled. See Worker pool growth.

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
- `CCNursery saved_nursery`, `admission_nursery` — structured-concurrency
  context captured at spawn; nursery queries follow the fiber, not the OS
  thread.
- `uint32_t deadlock_suppress_depth`, `external_wait_depth` — detector
  exemptions.
- `uint64_t generation`, `_Atomic uint64_t wait_ticket` — pooled-fiber ABA
  defense for waiters / tickets.
- Intrusive links for the free list and the global `all_fibers` list.

## Fiber pool

Fibers are pooled to amortize coroutine allocation:

- Two free lists under `free_list_mu`: `free_list` holds records that still
  carry a coroutine; `free_list_bare` holds records with no coro. Alloc
  prefers the coro-carrying list, which avoids mmap/munmap churn when the
  coro pool is at cap.
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

## `@parallel` spawn gate

Brace-form and `@parallel for` arms go through `cc_parallel_spawn` /
`cc_par_timed_run` (`cc/runtime/scheduler.c`). Lowering is
`cc/shadow/pp_emit_stmt.cch`. The public inline gate is
`cc_parallel_deny_fast` / `CC_PAR_NOTE_INLINE_ARM` in
`cc/include/ccc/cc_sched.cch`.

If this site's leaf arms are cheaper than a spawn, the gate does not spawn
except a 1-in-2^20 resample. A denied spawn returns an INVALID task; the
join runs the arm inline. Nothing strands. REAL work is never denied.

`@parallel wait`, nursery, and `cc_nursery_spawn*` do not go through
`cc_parallel_spawn`.

`@parallel` arms may run serially. Progress of one arm must not depend on
a sibling of the same construct running concurrently (for example an
unbuffered rendezvous between two arms of one `@parallel`). Channels plus
a nursery/`spawn` guarantee independent fibers. A hang is diagnosed by the
deadlock detector (park reason, parked fiber).

### Site table

A site is keyed by the construct's thunk function pointer. The table is a
fixed 256-slot open-addressed insert-only map. Overflow stays virgin and
always spawns. `CC_PAR_ADAPT=0` always spawns. Identical-code folding may
merge sites that share a thunk.

### Sample

Only a leaf that runs to completion is a sample. The sampler rejects the
duration if the arm suspended, issued a nested `cc_parallel_spawn`, or
absorbed an inlined (denied) child. Duration is thread CPU time
(`CLOCK_THREAD_CPUTIME_ID`). Sites that never yield a clean sample stay
virgin and keep spawning. Lowered code counts an inlined arm at the run
(`CC_PAR_NOTE_INLINE_ARM`), not only at the decide.

After `CC_PAR_LEARN_ATTEMPTS` (1<<20) wrapped virgin attempts, wrapping
stops; the site still spawns.

### Verdicts

- **VIRGIN** — no clean sample yet. Spawn, wrapped in a timing trampoline.
- **CHURN** — cheap leaf (below `CC_PAR_CHURN_NS`, default 8000 ns, about
  5× spawn+join of ~1.5 µs). Always deny except a 1-in-2^20 resample.
  A shallow ready queue is not a reason to admit.
- **REAL** — heavy leaf. Never denied.

One heavy clean sample commits REAL immediately. Virgin plus one cheap
clean sample commits CHURN. Demoting REAL to CHURN takes 3 consecutive
cheap clean resamples (`CC_PAR_CHEAP_STREAK`). Classified sites keep a
sparse resample so a wrong verdict can flip.

### Virgin flood bound

If ready-queue depth ≥ 512 (`CC_PAR_FLOOD_DEPTH`) and the site has never
had a wrapped arm suspend (`saw_suspend` clear), deny. A parked virgin
(channel wait) keeps spawning. Join parks latch `saw_suspend` for this
bound only: they do not commit REAL and they do not block CHURN. A
recursive `@parallel` joins on every inner node; cheap leaves still
classify. Meetings use `@parallel spawn`. A denied join that then parks
on a channel aborts.

### Inline fast path (native host cc)

Per construct the lowering emits a `static void*` site slot.
`cc_parallel_deny_fast` reads `CCParSiteGate` `{state, deny_depth}` (layout
prefix of the runtime site record; `_Static_assert` in `scheduler.c`).
CHURN yields an invalid task except a 1-in-2^20 resample. `deny_depth`
is unused on the fast path. `cc_parallel_site_gate` never returns NULL: adapt-off and table-full
return a static never-CHURN record. Emit cache and runtime version
together.

An assignment-only immediate-wait join (`name = expr` arms, no `return`
or `goto` in an arm, no dest) does not emit an exit cell. A CHURN deny
runs the arms as ordinary assignments on the caller. A spawn uses a
noinline helper so the denied recursion does not carry the task/env
frame.

Per-thread gate state (denied-sibling stack, inlined-arm count, resample)
tick) is one thread-local `CCParTls` block. A construct fetches it once
(`cc__par_tls()`) and passes the pointer to `cc_parallel_deny_enter`,
`cc_parallel_deny_fast`, `cc_parallel_note_denied`,
`CC_PAR_NOTE_INLINE_ARM`, and `cc_parallel_deny_leave`. The block is per
thread, not per fiber; the denied-sibling stack is a diagnostic and every
index into it is bounds-checked. Each helper also accepts the call without
the block argument and fetches the block itself; arity selects the shape,
so lowered code from either generation of the emitter compiles against
one header.

Under `CC_PARSER_MODE` or `__TINYC__`, `cc_parallel_deny_fast` is a no-op
and `cc__par_tls()` is a runtime call into the pthread-keyed bundle.
Every spawn hits `cc_parallel_spawn`. CHURN+deep wraps as resample;
wrapped CHURN admits are capped at depth 512.

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
2. While ready work remains, claim one idle worker and
   `wake_primitive_wake_one`.
3. If no idle worker remains, arm deferred pool growth when the ready
   queue is deeper than the live pool (or the pool is still below the
   eager cap). See Worker pool growth.
4. If `CC_V2_TARGET_ACTIVE` is set and `running_workers >= target`, stop.

### Wake skip depth (`CC_V2_WAKE_SKIP_DEPTH`)

Enqueue skips the external wake when pre-push depth is already
`>= wake_skip_depth` (default 4). Sysmon's every-tick unconditional wake
bounds the latency of a skipped wake to one sysmon interval.

## Worker pool growth

The pool starts at one worker and grows on demand up to the worker cap.
Workers are never culled.

A push that finds no idle worker creates a thread inline while the pool is
below the eager cap (default 2). Beyond that the push arms a one-shot
grow-pending flag and sysmon decides.

While grow-pending is set, sysmon rechecks on a short cadence (default
25 µs). It grows one worker when both:

- the cumulative drain rate since the episode baseline is below one pop
  per worker per 100 µs (workers blocked or running long CPU arms), or
  ready-queue depth is at least twice the live pool on three consecutive
  rechecks; and
- parks since the baseline do not exceed half the pops.

Neither test is read before the sample spans the rate period (100 µs).
In a shorter window a healthy pool is expected to show no pops, and the
park fraction is undefined at zero pops. A completion wave for many
clients is depth past twice the pool that drains in tens of
microseconds; it does not survive the dwell.

A high park fraction is run-to-park multiplexing (recv, accept, named
exclusive wait). Extra workers add traffic, not progress. A low park
fraction with a slow drain is CPU-bound work still occupying workers.

Otherwise it holds. A high pop rate on a shallow queue is park/wake churn.

An episode ends when the pool is at cap, admission is gated, or there is
true slack: every worker idle and the ready queue empty, or a spare
worker plus an empty queue that persists (~200 µs). An empty queue with
every worker busy is saturation, not slack; a single park between
CPU-bound arms is not slack either. The episode stays armed so the rate
trigger can keep recruiting.

`CC_V2_EAGER_THREADS`, `CC_V2_GROW_RECHECK_US`, `CC_V2_GROW_RATE_US`,
`CC_V2_GROW_DEPTH_X`, `CC_V2_GROW_DEPTH_DWELL`, and
`CC_V2_GROW_ESCALATE_TICKS` are test overrides.

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
5. **Pool growth.** While grow-pending is set, sysmon rechecks on a short
   cadence instead of the 20 ms tick and may add one worker (see Worker
   pool growth). Slow-tick jobs stay gated on elapsed time.
6. **Stall diagnostics.** If at least one fiber is queued, running, or
   parked and progress counters stall for ~2 s, print a diagnostic
   snapshot. Idle leftover pool slots after a join are not waiters.
   Fibers parked only in `external_wait` / deadlock-suppress, or a host
   thread in `cc_external_wait_enter`, are not waiters for this
   diagnostic.

## Deadlock detection

`sched_v2_check_deadlock` (sysmon) fires when all of the following hold for
at least `SCHED_V2_DEADLOCK_PERSIST_MS` (1000 ms):

- All workers are idle.
- Ready queue is empty.
- At least one fiber is `PARKED` with `external_wait_depth == 0` and
  `deadlock_suppress_depth == 0`.

Sysmon samples idle / ready-queue / park-deadline every tick. The parked-fiber
walk runs only after that stall has persisted for the latch duration. An
I/O-wait pool (all idle, empty queue, no internal parks) does not walk every
tick.

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
| `CC_V2_THREADS=N`                | Worker-pool cap (default: online CPUs, capped at 256).                                         |
| `CC_WORKERS=N`                   | Alias for `CC_V2_THREADS` when the latter is unset.                                            |
| `CC_V2_EAGER_THREADS=N`          | Test: inline create up to N workers on the push path. Default 2.                               |
| `CC_V2_GROW_RECHECK_US=N`        | Test: sysmon grow-episode cadence. Default 25.                                                 |
| `CC_V2_GROW_RATE_US=N`           | Test: µs/pop/worker below which the rate trigger grows. Default 100.                           |
| `CC_V2_GROW_DEPTH_X=N`           | Test: grow when ready depth ≥ N× pool size. 0 disables. Default 2.                             |
| `CC_V2_GROW_DEPTH_DWELL=N`       | Test: the depth trigger must hold on N consecutive rechecks. Default 3.                        |
| `CC_V2_GROW_ESCALATE_TICKS=N`    | Test: grow on N consecutive slow ticks of queued work + no idle. 0 off.                        |
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
| `CC_PAR_ADAPT=0`                 | Disable the `@parallel` spawn gate (always spawn). Default on.                                 |
| `CC_PAR_CHURN_NS=N`              | Cheap/heavy leaf line in nanoseconds. Default 8000.                                            |
| `CC_PAR_ADAPT_BACKLOG=N`         | Unused on the CHURN fast path (site `deny_depth` still written). Default 4.                    |
| `CC_PAR_ADAPT_DEBUG=1`           | Dump the `@parallel` site table at exit.                                                       |

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
  detector, join, spawn; `free_list` / `free_list_bare`.
- `cc/runtime/scheduler.c` — `cc_parallel_spawn` / `cc_par_timed_run`
  adaptive spawn gate.
- `cc/runtime/fiber_sched.c`, `fiber_internal.h` — public `cc__fiber_*` API
  (park, unpark, current, park-if, sleep); shim over `sched_v2` on the
  default path.
- `cc/runtime/fiber_sched_boundary.c`, `fiber_sched_boundary.h` —
  `cc_sched_fiber_wait[_until|_many]` for channels and I/O.
- `cc/runtime/wake_primitive.h` — OS wait/wake.
- `cc/runtime/channel.c`, `nursery.c` — consumers of park/signal/spawn/join.
- `cc/runtime/minicoro.h` — coroutine implementation.
- `cc/include/ccc/cc_sched.cch` — public API; `cc_parallel_deny_fast`,
  `CC_PAR_NOTE_INLINE_ARM`, `CCParSiteGate`.
- `cc/shadow/pp_emit_stmt.cch` — `@parallel` lowering (inline gate + spawn).

Operational testing notes live in `docs/scheduler-ops-runbook.md`.
