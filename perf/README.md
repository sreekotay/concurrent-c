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
| `parallel_hello.ccs` | Surface `@parallel` binary-tree reduction vs sequential. Compare via `compare_parallel_hello.sh`. |
| `../real_projects/raytracer/` | Weekend raytracer: C seq vs CC `@parallel for` vs Go. `./real_projects/raytracer/compare.sh`. |
| `parallel_hello_lowered.ccs` | Hand-lowered fork+grain sweep of the same tree (explicit spawn depth). |

### Runtime Stress And Application Patterns

| Benchmark | What it measures |
|-----------|------------------|
| `exclusive_named_lock.ccs` | Named exclusive sections: Zipf lock-by-name product throughput (directory+lock+scheduler) and single-caller serial fast-path cost. Compare via `compare_exclusive_named_lock.sh`. |
| `arena_contention_storm.ccs` | Per-fiber private-arena allocation throughput. |
| `arena_lifetime_bench.ccs` | Per-operation cost of the lifetime model: bump alloc (uncontended, and 4 threads on one arena), checkpoint child capture/restore (empty, `@scratch` shape, nested, grown), unsized vs sized release, size-class reuse, tip vs buried regrow, Vec push/get/churn through owner tokens, String append/promote/template, Map insert/destroy, owner header and token registry churn. `arena_lifetime_c_baseline.c` is the plain-C twin (malloc/free, hand bump arena with mark/reset, realloc-grown arrays, snprintf). `compare_arena_lifetime.sh [ref]` prints CC vs C with ratios, and current vs a ref's headers; receipts in `baselines/arena_lifetime_*.txt`. |
| `cancellation_avalanche.ccs` | Teardown speed and cleanup correctness for blocked task trees. |
| `mpmc_worker_pool.ccs` | Buffered producer -> worker-pool throughput and work distribution. |

### JS / Python Interop

Three boundaries, three places — don't mix them:

| Layer | API | Latency benches (stay put) | Adversarial storms | Receipts |
|-------|-----|----------------------------|--------------------|----------|
| **CC embeds JS/Python** | `CCJsDom` / `CCPy` (`js.cch` / `py.cch`) | [`py_baseline.ccs`](py_baseline.ccs), [`py_matplotlib_workload.ccs`](py_matplotlib_workload.ccs) | [`cc_embed_stress.ccs`](../stress/bridge/cc_embed_stress.ccs) (Waves A–C; via [`run.sh`](../stress/bridge/run.sh)) | [`baselines/py_baseline_20260809.txt`](baselines/py_baseline_20260809.txt) |
| **Native modules** (JS/Python import CC) | `js_module::[T]` / `py_module::[T]` | [`js_baseline.ccs`](js_baseline.ccs)+[`.js`](js_baseline.js), [`js_numpy.ccs`](js_numpy.ccs)+[`.js`](js_numpy.js) | host driver after `ccc build` | [`js_baseline_node_20260809.txt`](baselines/js_baseline_node_20260809.txt), [`js_numpy_node_20260808.txt`](baselines/js_numpy_node_20260808.txt), [`js_py_modules_20260809.txt`](baselines/js_py_modules_20260809.txt) |
| **Package bridges** (Node↔Python process) | `concurrent-c-python` / `concurrent-c-node` | [`npm/cc-python/examples/`](../npm/cc-python/examples/), [`pypi/cc-node/…/examples/`](../pypi/cc-node/cc_node/examples/) | **[`stress/bridge/`](../stress/bridge/)** ([catalog](../stress/bridge/bridge_stress.md)) | [`baselines/`](baselines/) (bridge + multiprocess rows) |

```bash
# Latency (stay under perf/ + package examples/)
./cc/bin/ccc run --release perf/py_baseline.ccs
./cc/bin/ccc build --release perf/js_baseline.ccs && node perf/js_baseline.js
node npm/cc-python/examples/js_numpy_bridge.js

# Package-bridge + CC embed storms (host-driven — not in run_all)
./stress/bridge/run.sh
CHAOS_SCALE=full ./stress/bridge/run.sh
CHAOS_SCALE=soak ./stress/bridge/run.sh   # see stress/bridge/bridge_stress.md
CHAOS_SCALE=quick ./out/cc/bin/ccc run stress/bridge/cc_embed_stress.ccs
```

`run_all --perf` skips `js_baseline` / `js_numpy` (need `ccc build` + `node`).
Full catalog + capture recipes: [`baselines/README.md`](baselines/README.md).

## Scheduler And Robustness Comparisons

The **Neckbeard Challenges** are six cross-language gauntlets
(`./perf/run_neckbeard_challenges.sh`). Each sub-script prints its own
per-language verdict; the harness forwards those blocks verbatim (no
single-scalar summary that strips kidnappers-drained, wake primitive,
peak threads, etc.).

| # | Comparison | Script | What it measures | What the result highlights |
|---|-----------|--------|------------------|----------------------------|
| 1 | **Syscall Kidnapping** | `compare_syscall.sh` | Scheduler responsiveness when many OS workers are trapped in blocking syscalls. | Replacement workers keep the runtime making progress; CC drains all kidnappers like a 1:1 runtime. |
| 2 | **Thundering Herd** | `compare_herd.sh` | Wake-up efficiency when many parked waiters are contending for one event. | Wake exactly one waiter instead of stampeding the herd. |
| 3 | **Channel Isolation** | `compare_contention_stability.sh` | Cross-channel interference when independent pipelines are hammered concurrently. | Low coupling across wake/sleep, scheduler, and allocator paths. |
| 4 | **Noisy Neighbor** | `compare_preemption.sh` | Scheduler fairness when one heartbeat task competes with CPU hogs that never yield. | Whether latency-sensitive work stays responsive under CPU pressure. |
| 5 | **Arena Allocation** | `compare_arena.sh` | Pure bump-pointer allocation throughput with private arenas and no shared allocator contention. | Measures the per-fiber arena strategy directly (Go/Zig rows are cross-strategy context). |
| 6 | **Named Exclusive Lock** | `compare_exclusive_named_lock.sh` | Zipf lock-by-name product throughput (directory+lock+scheduler) and single-caller serial fast-path cost. | Idiomatic name→mutex directories (`CCExclusive` vs `sync.Map` / `RwLock<HashMap>` / `Io.RwLock`). Product throughput, not a scheduler-only or fairness ranking. |

Related (not one of the six): `contention_workers4_stability.sh` tracks outlier frequency in the 4-worker channel-isolation case.

```bash
./perf/run_neckbeard_challenges.sh
```

Latest record: [`benchmarks/neckbeard_2026_08_14.txt`](benchmarks/neckbeard_2026_08_14.txt)
(manifest + full per-sample logs). That run is **DIRTY** (see the file's git
block) — a lab notebook, not a clean-commit receipt. `rustc` was not on PATH.
Headline tables from that run:

```text
=================================================================
CONCURRENT-C: THE NECKBEARD CHALLENGES
=================================================================

[1/6] Syscall Kidnapping Challenge...
-----------------------------------------------------------------
Implementation       Heartbeats   Kidnappers Completed
Pthread (Adler)      29           100 / 100
Concurrent-C         30           100 / 100
Go                   28           100 / 100
Zig                  29           100 / 100
-----------------------------------------------------------------

[2/6] Thundering Herd Challenge...
-----------------------------------------------------------------
Implementation               Avg Latency (ms)   Wake primitive
Pthread (condvar)            0.0176             pthread_cond_signal
Pthread (pipe herd)          1.7668             pipe write (herd case)
Concurrent-C                 0.0396             chan wake-one
Go                           0.0227             chan wake-one
Zig                          0.0164             pthread_cond_signal (C interop)
-----------------------------------------------------------------

[3/6] Channel Isolation Challenge...
min / mean / max
--------------------------------------------------------------------------
Implementation       Baseline (ms)          Contention (ms)        Interference %
Pthread              5.38 / 5.63 / 5.84     34.39 / 84.24 / 117.04 509.39 / 1401.01 / 2076.75
Concurrent-C         1.99 / 2.21 / 2.41     3.79 / 5.28 / 7.42     67.19 / 139.59 / 222.72
Go                   3.89 / 4.19 / 4.42     4.74 / 5.04 / 5.32     10.39 / 20.36 / 26.89
Zig                  9.61 / 10.54 / 11.97   35.00 / 39.38 / 41.87  244.55 / 275.75 / 321.38
--------------------------------------------------------------------------

[4/6] Noisy Neighbor Challenge...
-----------------------------------------------------------------
Implementation         Heartbeats   Peak Threads
Pthread (1:1)          28           17
Concurrent-C (4w)      29           18
Go (4P)                25           6
Zig (1:1)              28           17
-----------------------------------------------------------------

[5/6] Arena Contention Challenge...
-----------------------------------------------------------------
Implementation       Throughput (M/sec)
Pthread (Arena)      1310.62
Concurrent-C (Arena) 1367.99
Go (stack-promoted)  3713.45
Zig (c_allocator)    198.45
-----------------------------------------------------------------

[6/6] Named Exclusive Lock Challenge...
SUMMARY (median lock-ops/s; higher is better)
=================================================================
lang           zipf_product_ops/s  serial_fastpath_ops/s
cc                       26015837              384522950
go                       15494267              293255132
zig                       2923854              424065730
=================================================================
(no rustc on PATH; Zipf is directory+lock+scheduler product throughput,
not a scheduler-only or fairness ranking)

ALL CHALLENGES COMPLETED
=================================================================
```

To refresh the record (quiet machine, release toolchain present). The harness
prints a git/machine/compiler manifest first; a dirty tree is labeled as such
and is a lab notebook, not a clean-commit receipt.

```bash
./perf/run_neckbeard_challenges.sh --snapshot \
  "perf/benchmarks/neckbeard_$(date +%Y_%m_%d).txt"
```

Then point the "latest record" link above (and in the root README) at the new file.

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
- **Named Exclusive Lock:** Zipf product ops/s includes each runtime's name-directory + lock + scheduler; serial fast-path is one resolved mutex, one caller, no parallel scheduler. Same Zipf RNG/CDF and env knobs across languages (`CC_EXCL_*` — see the fairness contract at the top of `compare_exclusive_named_lock.sh`). Medians of timed trials; higher is better.
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
