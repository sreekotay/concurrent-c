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

## Phase 4b — FINDING: buffered enqueue/dequeue primitives leak signaling

**Severity**: DRIFT (with latent double-wake / under-wake bugs at callers).
**Effort**: 30-line refactor + 7 caller fixups.
**Trigger**: user concern — "the other 9 sites… I want smell and cleanliness without trading perf."

**The gap**: `cc_chan_enqueue` and `cc_chan_dequeue` were supposed to be pure ring-buffer ops but both buffered-branches inlined a *partial* post-enqueue / post-dequeue wake sequence (cond_signal + signal_recv_waiter / wake_one_send_waiter + signal_recv_ready / signal_activity). Every caller then had to decide whether to trust that embedded sequence or add their own on top. Result:

- 2 sites (post-Phase-3/4 refactor) called both the primitive and `cc__chan_post_{enqueue,dequeue}_notify` → **double wakes** (extra atomic ops, extra futex syscalls).
- 2 sites called the primitive and then only `unlock + flush + signal_activity` → relied silently on the primitive doing the real wakeup work, which would break invisibly if the primitive were ever changed.
- 1 site (cc_chan_send mutex path) called the primitive and then `unlock + flush + signal_recv_ready` → same hidden dependency.

Exactly the shape of the recv-side close-drain bug that started this audit: one logical op, several inlined implementations, caller-by-caller drift.

**Fix**:
1. Strip ALL signaling from the BUFFERED branches of `cc_chan_enqueue` and `cc_chan_dequeue`. They now do only the ring-buffer index math + value copy + trace.
2. Preserve unbuffered (cap=0) branches untouched — rendezvous completion uses `cond_broadcast(not_full)` with different semantics, separate code path.
3. Every buffered caller (3 enqueue, 4 dequeue) invokes `cc__chan_post_{enqueue,dequeue}_notify(ch, /*mu_held=*/1)` after the primitive. Document the contract in the primitive comment.

**Validation gate**: C1 through C5 + extended channel smoke suite (`chan_buffered_wakeup_smoke`, `chan_close_err_smoke`, `chan_close_wakeall_idempotent_smoke`, `chan_double_wake_nodup_smoke`, `chan_park_wake_lostwake_stress_smoke`, `chan_post_commit_wake_smoke`, `chan_pre_park_wake_smoke`, `chan_select_cancel_close_stale_smoke`, `chan_timed_ping_pong`) + `redis_idiomatic` end-to-end bench.

**Exit criterion**: zero `pthread_cond_signal(&ch->not_{empty,full})` calls inside `cc_chan_enqueue` / `cc_chan_dequeue` buffered branches; zero callers invoke the primitive without a canonical notify helper.

**Bonus cleanup**: collapse 4 inlined Dekker pre-park blocks into 2 helpers (`cc__chan_dekker_wake_{recv,send}_before_park`). These are asymmetric from post-enqueue/dequeue (no signal_activity / signal_recv_ready because nothing was produced/consumed — we are about to park) so they deserve their own name. Same drift-risk, same cure.

---

## Phase 5 — FINDING #5 + #6: V1 residue strip (DONE)

**Severity**: CRUFT — ~300 lines of unreachable code in `fiber_sched.c`.
**Effort**: mechanical delete + verify compile.

**Evidence**: `tls_current_fiber` was declared `= NULL` at `fiber_sched.c:711` and **never assigned anywhere else**. Every `if (tls_current_fiber) { ... }` branch was dead. Every field of `struct fiber_task` (lines 324–404) was unread. `tls_worker_id` was similarly declared `= -1` and never set — every `tls_worker_id >= 0` branch was dead.

**Completed scope**:
1. ✅ Deleted `struct fiber_task` field-full definition (lines 324–404, ~80 lines). Replaced with a forward declaration `typedef struct fiber_task fiber_task;` so `typedef struct fiber_task CCSchedFiber;` in `fiber_sched_boundary.h` continues to work as an opaque handle and so the legacy `cc_fiber_spawn` / `cc_fiber_join` / `cc_fiber_task_free` signatures don't break.
2. ✅ Deleted `tls_current_fiber` and `tls_worker_id` TLS declarations. **Kept** `tls_deadlock_suppress_depth` and `tls_external_wait_depth` — those are the per-OS-thread counters for pthread callers (not fibers) and are still written by the fall-through branches in `cc_deadlock_suppress_*` / `cc_external_wait_*`.
3. ✅ Collapsed every `if (tls_current_fiber) { ... } else { ... }` to the V2+pthread fall-through. Functions touched: `cc_deadlock_suppress_enter/leave/suppressed`, `cc_external_wait_enter/leave/active`, `cc__fiber_in_context`, `cc__fiber_current`, `cc_task_result_ptr`, `cc__fiber_suspend_until_ready`, `cc__fiber_suspend_until_ready_or_cancel`, `cc__fiber_set_park_obj`, `cc__fiber_clear_pending_unpark`, `cc__fiber_sleep_park`, `cc__fiber_publish_wait_ticket`, `cc__fiber_wait_ticket_matches`.
4. ✅ Deleted V1 tails of `cc__fiber_suspend_until_ready` and `cc__fiber_suspend_until_ready_or_cancel` (the code paths reached only when `sched_v2_in_context()` is false — impossible now outside a fiber, where the caller is a thread and the flag-loop is handled by the caller).
5. ✅ Simplified `cc__fiber_sleep_park` to nanosleep-only. V2 has no dedicated fiber timer park yet, so the V1 "yield to sleep queue drained by sysmon" path is unreachable; a future V2 timer park can be added in sched_v2.c and this shim rewired.
6. ✅ `cc__deadlock_thread_block` / `cc__deadlock_thread_unblock` → no-op shims (tls_worker_id is never set, so the guard was always true, so the counter bumps were dead).
7. ✅ `cc__sched_current_worker_id` → delegates directly to `sched_v2_current_worker_id()`.
8. ABI-reserved `CC_TASK_KIND_FIBER` enum value in `task.c:562` kept as-is (already documented as ABI-frozen).

**Net result**:
- `cc/runtime/fiber_sched.c`: 1325 → 1198 lines (-127 net; +98 insertions, -225 deletions).
- Zero live references to `tls_current_fiber` or `tls_worker_id` in `cc/runtime/`. Two remaining hits are comments documenting the retirement.
- Zero dead `if (tls_current_fiber) { ... }` branches; every shim is now a single-path V2-or-pthread dispatch.

**Validation**:
- Clean build, zero new warnings.
- Canaries: `pipeline_repeat` 20/20, `pipeline_repeat_debug` 20/20, `backpressure_cycle_ring3_deadline` 60/60, `channel_contention` interference 41.5–69% (noisy; no regressions), `match_recv_smoke`, `match_send_smoke`, `channel_cancel_and_close_err_smoke` ok.
- Extended channel smoke suite passes except for a **pre-existing** failure in `tests/chan_close_err_smoke.ccs` test 4 that reproduces on the pre-Phase-5 commit (verified via `git stash`). Tracked separately; not a Phase 5 regression.
- `real_projects/redis/bench_simple.sh`: hybrid 1.59–1.63M rps, 65–79% of upstream — matches post-Phase-4b baseline.

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
- [x] Phase 5: V1 residue strip
- [ ] Phase 6: sign-off
