# Channel / Fiber / Scheduler Audit — Fix Plan

**Generated**: 2026-04-14 (after close-drain unification landed in 2edecbd).
**Scope**: the drain-leak fix exposed a family of "pattern-gap" smells where one logical operation has multiple inlined implementations that drifted apart. This plan systematically closes each one in priority order with validation gates between phases.

**Invariant we are defending**: every logical operation on a channel (close-recv, post-enqueue-notify, post-dequeue-notify, Dekker pre-park, structured-error surface) has EXACTLY ONE canonical implementation that every call site routes through. Drift from a canonical path is a bug waiting to happen.

## Execution rules

- One phase per commit. No mixing.
- Validation gate between every phase: must pass before starting next phase.
- If a phase regresses a previously-passing test, roll back before advancing.
- Each phase small enough that if we crash the machine mid-way, recovery is trivial.

## Canaries (run after every phase)

| # | Test | Purpose |
|---|---|---|
| C1 | `stress/pipeline_repeat.ccs` x 20 | drain regression gate |
| C2 | `tests/pipeline_repeat_debug.ccs` x 20 | Dekker missed-wake gate (currently ~3/5 deadlock) |
| C3 | `stress/backpressure_cycle_ring3_deadline.ccs` | deadline-park works |
| C4 | `perf/channel_contention.ccs` | no perf regression |
| C5 | `tests/match_recv_smoke.ccs` + `match_send_smoke.ccs` + `chan_select_cancel_close_stale_smoke.ccs` | select drain paths |

---

## Phase 1 — FINDING #1: Dekker fence in `cc_chan_timed_recv` fiber path

**Severity**: BUG. Likely cause of the `pipeline_repeat_debug` deadlock the user saw.
**Effort**: 1 line.
**Location**: `cc/runtime/channel.c` ~line 4514.

**The gap**: sender side (`cc_chan_timed_send` @4321) has `atomic_thread_fence(memory_order_seq_cst)` after publishing `has_send_waiters=1`. Receiver side (`cc_chan_timed_recv`) forgot it. Blocking `cc_chan_recv` (@3944) has it; timed variant doesn't. Asymmetric copy-paste.

**Mechanism**: on arm64, `release-store(has_recv_waiters=1)` followed by `acquire-load(queue-state)` of a different location can reorder. Sender observing `has_recv_waiters=0` skips the wake; receiver parks forever.

**Fix**: insert `atomic_thread_fence(memory_order_seq_cst);` immediately after the `pthread_mutex_unlock(&ch->mu);` that follows `cc__chan_add_recv_waiter(ch, &node);`. Add a comment citing the matching `cc_chan_recv`@3944 and `cc_chan_timed_send`@4321 sites.

**Validation gate**: C1, C2 (THE important one — must pass 20/20), C3, C4.

**Exit criterion**: `pipeline_repeat_debug` passes 20/20 (was ~2/5 pre-fix).

---

## Phase 2 — FINDING #2: `tx_error_code` unification

**Severity**: BUG — `cc_chan_close_with(ch, cc_io_error(CC_IO_TIMEOUT))` users get silent `EPIPE` downgrade on 12 paths.
**Effort**: 20-line helper + `replace_all`-style fixes.
**Sites** (all in `cc/runtime/channel.c`):

| Line | Path | Current | Target |
|---|---|---|---|
| 2116 | send loop observes ch->closed | `return EPIPE` | `return cc__chan_tx_close_errno(ch)` |
| 2181 | send loop observes ch->closed | `return EPIPE` | same |
| 2192 | send body observes ch->closed | `return EPIPE` | same |
| 3336 | send fast-path closed | `return EPIPE` | same |
| 3345 | send direct-handoff closed | `return EPIPE` (under mu) | same |
| 3377 | cap==0 send closed | `return EPIPE` | same |
| 3386 | send body closed | `return EPIPE` | same |
| 3546 | send body closed | `return EPIPE` | same |
| 4047 | recv body closed | `return EPIPE` | same |
| 4074 | cap==0 recv closed | `return EPIPE` | same |
| 4081 | recv body closed (under mu) | `return EPIPE` | same |
| 4270 | timed_send body closed | `return EPIPE` | same |

**Fix**:
```c
/* Canonical close-errno surface.  ch->tx_error_code is populated by
 * cc_chan_close_with() to carry a structured CCIoError; when set it
 * must take precedence over the generic EPIPE so the user sees the
 * actual reason (CC_IO_TIMEOUT, CC_IO_CANCELLED, ...).  Use this in
 * every return path that observes ch->closed on the tx side or on
 * the rx side with no items left to drain. */
static inline int cc__chan_tx_close_errno(CCChan* ch) {
    return ch->tx_error_code ? ch->tx_error_code : EPIPE;
}
```

Apply mechanically. Keep existing `ch->rx_error_closed` checks untouched (different error channel).

**Validation gate**: C1, C2, C3, C5. Add a smoke that sets a non-default `tx_error_code` and asserts both recv and send surface it.

**Exit criterion**: `grep -n "return EPIPE;" cc/runtime/channel.c` returns only sites where `ch->closed` was NOT the observing condition (e.g., unbuffered-channel rendezvous with no partner, cap==0 pure EPIPE).

---

## Phase 3 — FINDING #3: post-enqueue wake helper

**Severity**: DRIFT with latent BUG for `recv_signal`-socket channels (timed_send misses the socket wake).
**Effort**: ~100 lines refactored.
**Sites**: 14 instances of the post-enqueue notify sequence; 2 (`channel.c:4261`, `channel.c:4387`) call `cc__chan_signal_activity` instead of `cc__chan_signal_recv_ready`, silently dropping socket wakes.

**Fix**: extract and apply
```c
/* Canonical post-successful-enqueue wake sequence.  Every lock-free
 * fast-path that enqueues a value into a buffered channel MUST call
 * this before returning.  Ordering matters:
 *   1. wake one parked fiber-receiver    (cc__chan_signal_recv_waiter)
 *   2. signal pthread waiter             (pthread_cond_signal not_empty)
 *   3. release ch->mu                    (pthread_mutex_unlock)
 *   4. flush wake batch                  (wake_batch_flush)
 *   5. poke recv_signal socket + broadcast activity to multi-chan
 *      select waiters                    (cc__chan_signal_recv_ready)
 * Must be called with ch->mu HELD (step 3 is the unlock). */
static inline void cc__chan_post_enqueue_notify(CCChan* ch) {
    cc__chan_signal_recv_waiter(ch);
    pthread_cond_signal(&ch->not_empty);
    pthread_mutex_unlock(&ch->mu);
    wake_batch_flush();
    cc__chan_signal_recv_ready(ch);
}
```

Apply at every post-enqueue site. Callers that currently run `cc_chan_lock(ch); <notify steps>` become `cc_chan_lock(ch); cc__chan_post_enqueue_notify(ch);`.

**Validation gate**: C1 through C5 + `tools/run_all.ccs --all` subset (all channel tests).

**Exit criterion**: zero callers of `pthread_cond_signal(&ch->not_empty)` outside the helper; zero callers of `cc__chan_signal_activity` in post-enqueue context (still OK on the post-dequeue side — see Phase 4).

---

## Phase 4 — FINDING #4: post-dequeue wake helper

**Severity**: DRIFT only (no known live bug; future-proofing).
**Effort**: ~60 lines refactored.
**Sites**: 7 instances at `channel.c:3849, 4163, 4215, 4456, 4489, 4563` (+one more).

**Fix**:
```c
/* Canonical post-successful-dequeue wake sequence.  Symmetric with
 * cc__chan_post_enqueue_notify.  Must be called with ch->mu HELD. */
static inline void cc__chan_post_dequeue_notify(CCChan* ch) {
    cc__chan_wake_one_send_waiter(ch);
    pthread_cond_signal(&ch->not_full);
    pthread_mutex_unlock(&ch->mu);
    wake_batch_flush();
    cc__chan_signal_activity(ch);
}
```

Note: post-dequeue uses `signal_activity` (not `signal_recv_ready`) — senders wake via `not_full`, there is no "send_signal socket" equivalent, so broadcast-only is correct here. Document it in the helper comment.

**Validation gate**: C1 through C5 + full `tools/run_all.ccs --all`.

**Exit criterion**: zero callers of `pthread_cond_signal(&ch->not_full)` outside the helper.

---

## Phase 5 — FINDING #5 + #6: V1 residue strip

**Severity**: CRUFT — ~350 lines of unreachable code in `fiber_sched.c`.
**Effort**: mechanical delete + verify compile.

**Evidence**: `tls_current_fiber` is declared `= NULL` at `fiber_sched.c:711` and **never assigned anywhere else**. Every `if (tls_current_fiber) { ... }` branch is dead. Every field of `struct fiber_task` (lines 324–404) is unread.

**Scope**:
1. Delete `struct fiber_task` definition (lines 324–404, ~80 lines).
2. Delete `tls_current_fiber`, `tls_deadlock_suppress_depth`, `tls_external_wait_depth` TLS declarations and their mirror-write logic.
3. Collapse every `if (tls_current_fiber) { ... } else { ... }` to the `else` branch (V2-only).
   Sites: 762, 781, 797, 808, 833, 855, 1026, 1030, 1041, 1114, 1143, 1182, 1196, 1300.
4. Delete V1 tails of `cc__fiber_park_if_impl`, `cc__fiber_suspend_until_ready`, `cc__fiber_suspend_until_ready_or_cancel` (the code paths reached only when `sched_v2_in_context()` is false — impossible now).
5. Keep ABI-reserved `CC_TASK_KIND_FIBER` enum value in `task.c:562` (already documented as ABI-frozen).

**Validation gate**: clean build + all canaries + `tools/run_all.ccs --all` + `perf/run_neckbeard_challenges.sh`.

**Exit criterion**: `grep "tls_current_fiber" cc/runtime/` returns zero hits; `grep "struct fiber_task" cc/runtime/` returns zero hits (except possibly a forward-declared opaque typedef if anything external depends on it — verify and decide).

---

## Phase 6 — Final sign-off

- 1 full pass `tools/run_all.ccs --all`.
- `perf/run_neckbeard_challenges.sh`.
- Redis benchmark smoke (`real_projects/redis/bench_simple.sh`).
- Pigz benchmark smoke.
- Confirm no new `--trace` crashes.
- Tag the commit.

---

## Status tracker

- [ ] Phase 1: Dekker fence
- [ ] Phase 2: tx_error_code unification
- [ ] Phase 3: post-enqueue wake helper
- [ ] Phase 4: post-dequeue wake helper
- [ ] Phase 5: V1 residue strip
- [ ] Phase 6: sign-off
