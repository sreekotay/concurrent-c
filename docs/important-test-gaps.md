# Important Test Gaps

This file tracks the highest-value concurrency cases that are not yet covered well enough by the current `tests/`, `stress/`, `perf/`, and `examples/` suites.

Scope:

- Focus on gaps or weak coverage only.
- Prefer deterministic semantic checks in `tests/`.
- Put long-running interleaving and soak cases in `stress/`.
- Add `perf/` only when there is a concrete performance question worth measuring.
- Add `examples/` only when the missing case needs a recommended user-facing pattern.

## Highest Priority

### `tests/`

#### `tests/fanout_partial_failure_cancel_siblings_smoke.ccs`

- Case: fan-out with partial failure.
- Goal: verify the default nursery semantic: first worker failure cancels siblings, blocked siblings wake cleanly, and the parent observes one deterministic failure outcome.
- Why here: this is a semantic contract check, so it should be small and deterministic.

#### `tests/fanout_partial_failure_collect_all_smoke.ccs`

- Case: fan-out with partial failure where the caller explicitly wants to keep going and collect all outcomes.
- Goal: verify an explicit "collect all" design using result channels or tagged results, including a mix of successes and failures without accidental sibling cancellation.
- Why here: the repo currently shows the default nursery behavior much better than the alternative policy that many real fan-out workloads need.

#### `tests/cancellation_partial_side_effects_smoke.ccs`

- Case: cooperative cancellation mid-operation with externally visible partial work.
- Goal: model a two-phase operation such as `reserve -> publish` or `write temp -> commit`, cancel between phases, and assert the exact final state plus any cleanup or compensation.
- Why here: cancellation is already covered, but not the harder "what state is the world in now?" question.

#### `tests/sequence_check_then_act_smoke.ccs`

- Case: thread-safe operation vs unsafe sequence.
- Goal: create a minimal check-then-act race where each step is individually safe but the pair is not, then lock in the safe replacement pattern next to it.
- Why here: this is the cleanest missing regression for the "thread-safe is a sequence property, not just an API property" lesson.

### `stress/`

#### `stress/protocol_state_machine_cancellation.ccs`

- Case: stateful protocol state machine over a long-lived connection.
- Goal: simulate a small multi-state session such as `HELLO -> READY -> STREAMING -> DRAINING -> CLOSED`, then inject cancellation, close, timeout, and out-of-order messages under load.
- Why here: the hard part is not one transition but many interleavings across a long-lived session.

#### `stress/fanout_partial_failure_matrix.ccs`

- Case: fan-out with partial failure at different points in worker lifetime.
- Goal: vary worker count, failure count, and failure timing such as `before send`, `while blocked`, and `after partial progress`, then verify the chosen policy stays consistent.
- Why here: once the smoke semantics exist, this stress test protects the policy against timing-sensitive regressions.

## Important But Secondary

### `examples/`

#### `examples/recipe_fanout_partial_failure.ccs`

- Case: explicit policy design for partial failure.
- Goal: show two patterns side by side: `fail fast and cancel siblings` and `collect all tagged results`.
- Why here: users need guidance because nurseries solve only one of the valid semantics.

#### `examples/recipe_cancellable_protocol_loop.ccs`

- Case: long-lived protocol loop with explicit state.
- Goal: show a small state machine with cancellation-aware receive, valid transition checks, and cleanup on exit.
- Why here: this complexity lives above the concurrency model, so an example is more valuable than prose alone.

### `perf/`

#### `perf/fanout_failure_teardown.ccs`

- Case: cost of sibling cancellation after first failure.
- Goal: measure teardown latency as worker count and blocked-wait depth increase.
- Why here: only worth adding after the semantic smoke tests land, but it is the most useful missing benchmark in this area.

## Already Covered Well Enough

These do not need urgent additions right now:

- Buffered channels masking backpressure.
- Async vs parallel distinction.
- Non-tree channel deadlock detection.
- Fairness and starvation, especially in `stress/` and `perf/`.
