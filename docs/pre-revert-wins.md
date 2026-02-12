# Pre-Revert Wins Log (Runtime/Perf)

This document captures the main wins we believed we had before the rollback to `HEAD`, so we keep the signal and avoid re-discovering the same ideas.

## Clear Wins (High Confidence)

- **Direct execution mode for non-yielding fibers (`no_yield`)**  
  Removed unnecessary coroutine context-switch overhead in the common no-yield path and materially closed the gap to pthread baselines.

- **Generation-driven worker wake model**  
  Replaced fragile spin/sleep heuristics with a simpler generation-ID style wake progression, improving correctness and reducing variance in idle/wake transitions.

- **Waiter-gated noyield completion signaling**  
  Avoided mutex/condvar signaling when no thread joiner is waiting; measured as a modest but repeatable win and considered safe enough to keep as default behavior.

- **Noyield spawn hot-path trimming**  
  Made expensive spawn metadata writes conditional on debug/telemetry modes, reducing unnecessary work on hot paths.

- **Keep observability counters off by default in hot completion path**  
  Making completion-path observability updates optional (`completed`-style counters) reduced hot-path atomic traffic.

## Probable Wins (Medium Confidence)

- **Dependency-biased join nudge (dispatch-only)**  
  Waking likely-relevant workers in blocked noyield joins showed small aggregate gains in matrix testing and was judged low-risk as a default-on nudge.

- **Micro-spin + bounded timed wait before deep idle sleeps**  
  Helped correctness/progress under racey wake windows while keeping idle behavior bounded.

## Process Wins (Convergence / Debugging)

- **Fine-grained join-stage telemetry**  
  Split join delays into fast path vs thread/fiber path and pre/post done components; identified pre-done coordination as dominant tail source.

- **Wake-to-work and queue-to-start latency histograms**  
  Exposed scheduler delay directly (not just end-to-end wall time), making tail behavior diagnosable.

- **No-work wake classification**  
  Breaking `post-wait no-work` into classes (timeout/spurious vs other causes) clarified where wasted wake cycles came from.

## Tried and Discarded (Useful Negative Results)

- Join promotion, long rewait, inline claim/fusion, event-word join path, and several aggressive rewait/spin variants were tested and rejected due to regressions, poor hit rates, or unstable variance.

## Telemetry Notes (Pre-Revert + Reapply Phase)

### Runtime knobs

- `CC_SPAWN_TIMING=1`  
  Enables spawn-path timing breakdown (`alloc`, `coro`, `push`, `wake`).

- `CC_WAKE_GEN_STATS=1`  
  Enables wake/join/queue diagnostic counters.

- `CC_WAKE_GEN_STATS_HEAVY=1`  
  Enables heavier timing paths (e.g. `rdtsc`-based histograms like wake->work and queue->start). Intended only for deep diagnosis.

### What telemetry showed

- Tail runs correlated with **scheduler delay counters** (especially queue->start delay and post-wait no-work classes), not raw spawn micro-cost.
- Join-path instrumentation repeatedly pointed to **thread-join wait cost** as a major contributor in bad runs.
- Wake/no-work classification was useful to distinguish actual work latency from timeout/spurious wake churn.

### Practical guidance

- For performance A/B: run with telemetry off (`CC_SPAWN_TIMING=0 CC_WAKE_GEN_STATS=0`).
- For low-perturbation diagnosis: `CC_WAKE_GEN_STATS=1` only.
- For deep investigations: add `CC_WAKE_GEN_STATS_HEAVY=1`, then treat results as potentially more perturbative.

### Baseline comparison snapshot (telemetry off)

- In recent clean A/B (same input, telemetry off), current branch remained slower than `HEAD` on both median and tail.
- This means instrumentation is useful for diagnosis, but runtime path changes still need to be reintroduced selectively and revalidated.

---

## Notes

- This is a memory of wins observed during the pre-revert iteration loop, not a claim that all wins are currently present in `HEAD`.
- Keep this list updated as we reintroduce changes with clean A/B validation.
