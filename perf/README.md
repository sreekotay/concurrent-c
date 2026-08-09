# Concurrent-C Performance & Robustness Benchmarks

This directory contains the runnable throughput benchmarks, scheduler diagnostics, and runtime stress-style microbenchmarks for Concurrent-C.

## Running The Suite

From the repo root:

```bash
./cc/bin/ccc run tools/run_all.ccs -- --perf
./cc/bin/ccc run tools/run_all.ccs -- --all
```

Run an individual benchmark directly:

```bash
./cc/bin/ccc run perf/perf_channel_throughput.ccs
./cc/bin/ccc run perf/channel_contention.ccs
./cc/bin/ccc run perf/work_stealing_efficiency.ccs
```

## Benchmark Inventory

### Channels And Message Passing

| Benchmark | What it measures |
|-----------|------------------|
| `perf_channel_throughput.ccs` | Buffered, unbuffered, and single-thread channel ops/sec. |
| `perf_buffered_base.ccs` | Minimal buffered 1 producer / 1 consumer throughput. |
| `perf_buffered_core.ccs` | Buffered throughput using low-level public runtime APIs. |
| `perf_buffered_ladder.ccs` | Loop vs fiber vs buffered-channel overhead ladder. |
| `perf_unbuffered_rendezvous_base.ccs` | Contended unbuffered rendezvous throughput and waiter churn. |
| `perf_zero_copy.ccs` | Payload-size sensitivity from `int` to large structs. |
| `perf_match_select.ccs` | `cc_chan_match_select` multi-channel select overhead. |
| `channel_contention.ccs` | Cross-channel interference between independent pipelines. |
| `channel_wake_wave.ccs` | Wake-to-run latency for one parked receiver per worker. |
| `thundering_herd.ccs` | Latency to wake a single waiter from a large herd. |
| `channel_fairness.ccs` | Distribution skew diagnostic for buffered wake behavior. |

### Spawn, Async, And Scheduler Overhead

| Benchmark | What it measures |
|-----------|------------------|
| `spawn_simple.ccs` | Minimal nursery spawn/join throughput. |
| `spawn_sequential.ccs` | Sequential spawn + join cost via `@async`. |
| `spawn_nursery.ccs` | Batched nursery spawn throughput. |
| `spawn_nursery_simple.ccs` | Nursery throughput with simpler task bodies. |
| `spawn_nursery_direct.ccs` | Nursery throughput with direct function calls. |
| `spawn_fiber_direct.ccs` | Raw internal fiber spawn/join throughput. |
| `perf_spawn_ladder.ccs` | Nursery vs `block_all` vs direct fiber overhead breakdown. |
| `perf_async_overhead.ccs` | Async task creation, execution, and blocking overhead. |
| `work_stealing_efficiency.ccs` | Cost of load balancing when work starts localized. |
| `perf_gobench_async_pressure.ccs` | Go-bench-style pressure from many parked async recv tasks. |
| `perf_gobench_blocking_pressure.ccs` | Parked waiters plus blocking-task scheduler pressure. |
| `fiber_overhead_profile.ccs` | Fiber vs thread overhead for heavy and minimal tasks. |

### Runtime Stress And Application Patterns

| Benchmark | What it measures |
|-----------|------------------|
| `exclusive_named_lock.ccs` | Named exclusive sections: Zipf lock-by-name product throughput (directory+lock+scheduler) and single-caller serial fast-path cost. Compare via `compare_exclusive_named_lock.sh`. |
| `arena_contention_storm.ccs` | Per-fiber private-arena allocation throughput. |
| `cancellation_avalanche.ccs` | Teardown speed and cleanup correctness for blocked task trees. |
| `mpmc_worker_pool.ccs` | Buffered producer -> worker-pool throughput and work distribution. |

### JS / Python Interop

Three boundaries, three places — don't mix them:

| Layer | API | Latency benches | Adversarial storms | Receipts |
|-------|-----|-----------------|--------------------|----------|
| **CC embeds Python** | `CCPy` / `cc_py_new` | [`py_baseline.ccs`](py_baseline.ccs), [`py_matplotlib_workload.ccs`](py_matplotlib_workload.ccs) | *(add under `perf/` / `stress/` — pure `ccc run`)* | [`baselines/py_baseline_20260809.txt`](baselines/py_baseline_20260809.txt) |
| **Native modules** (JS/Python import CC) | `js_module::[T]` / `py_module::[T]` | [`js_baseline.ccs`](js_baseline.ccs)+[`.js`](js_baseline.js), [`js_numpy.ccs`](js_numpy.ccs)+[`.js`](js_numpy.js) | `ccc build` then host driver (same as latency) | [`js_baseline_node_20260809.txt`](baselines/js_baseline_node_20260809.txt), [`js_numpy_node_20260808.txt`](baselines/js_numpy_node_20260808.txt), [`js_py_modules_20260809.txt`](baselines/js_py_modules_20260809.txt) |
| **Package bridges** (Node↔Python process) | `concurrent-c-python` / `concurrent-c-node` | [`npm/cc-python/examples/`](../npm/cc-python/examples/), [`pypi/cc-node/…/examples/`](../pypi/cc-node/cc_node/examples/) | [`js_bridge_chaos.js`](../npm/cc-python/examples/js_bridge_chaos.js), [`stress_wire.py`](../pypi/cc-node/cc_node/examples/stress_wire.py) | [`baselines/`](baselines/) (bridge + multiprocess rows) |

```bash
# Latency (CC embed / native module)
./cc/bin/ccc run --release perf/py_baseline.ccs
./cc/bin/ccc build --release perf/js_baseline.ccs && node perf/js_baseline.js

# Package-bridge chaos (host-driven; not in run_all --perf)
OPENBLAS_NUM_THREADS=1 node npm/cc-python/examples/js_bridge_chaos.js
CHAOS_SCALE=full OPENBLAS_NUM_THREADS=1 node npm/cc-python/examples/js_bridge_chaos.js
python -m cc_node.examples.stress_wire
CC_NODE_STRESS=full python -m cc_node.examples.stress_wire
```

`run_all --perf` skips `js_baseline` / `js_numpy` (need `ccc build` + `node`).
Full catalog + capture recipes: [`baselines/README.md`](baselines/README.md).

## Scheduler And Robustness Comparisons

These compare Concurrent-C against pthread and Go baselines on scheduler fairness and robustness under adversarial workloads.

| Comparison | Script | What it measures | What the result highlights |
|-----------|--------|------------------|----------------------------|
| **Syscall Kidnapping** | `compare_syscall.sh` | Scheduler responsiveness when many OS workers are trapped in blocking syscalls. | Replacement workers keep the runtime making progress. |
| **Thundering Herd** | `compare_herd.sh` | Wake-up efficiency when many parked waiters are contending for one event. | Wake exactly one waiter instead of stampeding the herd. |
| **Channel Isolation** | `compare_contention.sh` | Cross-channel interference when independent pipelines are hammered concurrently. | Low coupling across wake/sleep, scheduler, and allocator paths. |
| **Channel Stability (4 workers)** | `contention_workers4_stability.sh` | Outlier frequency in the 4-worker channel-isolation case. | Tracks how often trials drift toward serial-like placement. |
| **Noisy Neighbor** | `compare_preemption.sh` | Scheduler fairness when one heartbeat task competes with CPU hogs that never yield. | Whether latency-sensitive work stays responsive under CPU pressure. |
| **Arena Allocation** | `compare_arena.sh` | Pure bump-pointer allocation throughput with private arenas and no shared allocator contention. | Measures the per-fiber arena strategy directly. |

Run the comparison suite:

```bash
./perf/run_neckbeard_challenges.sh
```

Latest results from the comparison suite:

```text
=================================================================
CONCURRENT-C: SCHEDULER AND ROBUSTNESS COMPARISONS
=================================================================
Running all robustness and fairness comparisons...

[1/5] Syscall Kidnapping Challenge...
-----------------------------------------------------------------
Implementation       Heartbeats
Pthread              54
Concurrent-C         55
Go                   54
-----------------------------------------------------------------

[2/5] Thundering Herd Challenge...
-----------------------------------------------------------------
Implementation       Avg Latency (ms)
Pthread              3.7678
Concurrent-C         0.0144
Go                   0.0136
-----------------------------------------------------------------

[3/5] Channel Isolation Challenge...
-----------------------------------------------------------------
Implementation       Interference
Pthread              41.20%
Concurrent-C         9.10%
Go                   -14.16%
-----------------------------------------------------------------

[4/5] Noisy Neighbor Challenge...
-----------------------------------------------------------------
Implementation       Heartbeats
Pthread              59
Concurrent-C         55
Go                   48
-----------------------------------------------------------------

[5/5] Arena Contention Challenge...
-----------------------------------------------------------------
Implementation       Throughput (M/sec)
Pthread (Arena)      699.79
Concurrent-C (Arena) 1011.12
Go (mcache)          4752.49
-----------------------------------------------------------------

=================================================================
ALL CHALLENGES COMPLETED
=================================================================
```

## Go Comparison

Use these scripts to compare against the Go runtime directly.

| Script | Description |
|--------|-------------|
| `compare_benchmarks.sh` | Runs equivalent CC and Go benchmarks and reports the performance ratio. |
| `run_go_benchmarks.sh` | Runs only the Go benchmarks under `perf/go/`. |

## Benchmarking Notes

1. Use release builds for meaningful numbers:
   ```bash
   ./cc/bin/ccc build --release my_test.ccs
   ```
2. Strip binaries when comparing footprint.
3. Keep the machine quiet when collecting timing numbers.
4. Tune worker count with `CC_WORKERS` when exploring scheduler behavior.

## Interpreting Results

- **Read absolute latency first, ratios second.** Channel Isolation reports min/mean/max absolute ms for both baseline and contention; the Interference % is *derived* and baseline-relative — a faster happy path inflates it, so a low % can simply mean a slow baseline. Compare absolute Contention (ms): CC bounds the worst case without taxing the happy path.
- Negative interference in Channel Isolation means concurrent load did not slow the independent channel pairs down.
- **Syscall Kidnapping:** CC's V2 sysmon orphan-and-replaces workers pinned in blocking syscalls (detach off-pool, fresh worker takes the slot), so CC drains all kidnappers like a 1:1 runtime instead of capping at worker count. Any "tops out near N" baseline in the harness text is the *non-promoting* M:N strawman CC beats, not CC's behavior.
- **Arena Contention compares strategies, not one workload.** CC and Pthread run true bump arenas (pointer-bump, no per-alloc free) — apples-to-apples, and they land close. The harness's Go and Zig rows are *different strategies*: Go's non-escaping `make([]byte,16)` is **stack-promoted** by escape analysis (verified `does not escape` — it never reaches mcache), so its huge number is stack-bump throughput; Zig uses `c_allocator` malloc/free per alloc. Treat the headline as CC-vs-Pthread arena parity; the Go/Zig columns are cross-strategy context, not a like-for-like win/loss.
- High jitter in herd tests usually points to OS scheduling overhead rather than channel semantics.
- The Noisy Neighbor score is just total heartbeat ticks over the fixed run window, so higher is better.

---

## Compiler perf baseline

Tracked in [`compiler_baseline.txt`](compiler_baseline.txt). Captured by
`scripts/capture_baseline.sh` against the **default (native) frontend**
smoke suite (`tools/cc_test --jobs 8`).

### Is it accurate?

| Metric | Accurate now? | Notes |
|--------|---------------|-------|
| `reparse_sites_visit_codegen`, `loc_*` | **Yes** | Static greps of `cc/src/visitor/` — still meaningful for the legacy visitor tree that ships in-tree. |
| `suite_tests` | **Yes** | Current native smoke count (1006 as of 2026-08-09). Catches skipped/added tests. |
| `reparses_*` | **Numerically yes, as a guard no** | Default capture sees **0** because `CC_DEBUG_REPARSE` only logs from the **legacy** visitor pipeline. Native does not emit `[cc:reparse]` lines, so these no longer catch "extra reparse" regressions under the default front. To measure legacy reparses again: `CC_TEST_FRONTEND=legacy CC_DEBUG_REPARSE=1` (many smokes fail on legacy today — not the default gate). |
| `wall_real_seconds` | **Snapshot only** | Never fails the check; host noise dominates. |

So: **commit the file after intentional suite/LOC shape changes**; do **not**
treat `reparses_total=0` as "the compiler does no work" — it means "native
emits no visitor reparse counters."

### Workflow

```bash
make perf-baseline     # capture → perf/compiler_baseline.txt
make perf-regress      # compare vs committed baseline (.shcc; bash oracle: make perf-regress-oracle)
```

### What each metric guards against

| Metric | Source | Regression rule | What it catches |
|--------|--------|-----------------|-----------------|
| `reparse_sites_visit_codegen` | static grep | must equal baseline | A new `cc__reparse_source_to_ast_(ctx\|ex)` call site in the legacy visitor. |
| `loc_visit_codegen`, `loc_closure_pass`, `loc_visitor_dir` | `wc -l` | NOTICE if growth > +25% | Accidental bloat in visitor sources. |
| `suite_tests` | `cc_test:` line | must equal baseline | Accidentally skipped or added smoke tests. |
| `reparses_*` | `CC_DEBUG_REPARSE=1` on suite | must be ≤ baseline | **Legacy-front only.** Under native (default) these stay 0. |
| `wall_real_seconds` | `/usr/bin/time -p` | informational | OK / INFO / NOTICE bands only — never fails. |

### When to update

`make perf-baseline` after intentional suite-shape or visitor-LOC changes;
commit the new file in the same PR. Capture aggregates reparse lines from the
instrumented cold run *before* the warm timed pass (see
`scripts/capture_baseline.sh`).
