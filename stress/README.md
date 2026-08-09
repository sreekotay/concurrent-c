# Concurrent-C Stress Tests

Stress tests that push the compiler and runtime with demanding patterns.

## Tests

| Test | Description | What it stresses |
|------|-------------|------------------|
| `async_await_flood` | 100 async tasks using cc_block_all | Async runtime, task combinators |
| `block_combinators_stress` | cc_block_all/race/any under load | Task combinator scalability |
| `channel_flood` | Many producers flooding a single consumer | Channel backpressure, contention |
| `spawn_storm` | Spawn 1000 concurrent tasks | Nursery spawn overhead, scheduling |
| `nursery_deep` | 20 levels of nested nurseries | Nursery stack, structured concurrency |
| `pipeline_long` | 50-stage processing pipeline | Channel chaining, throughput |
| `deadline_race` | Many tasks with competing deadlines | Deadline handling, timing |
| `cycle_ring3_deadline` | 3-task send-first ring under deadlines | Cyclic wait teardown, deadline escape |
| `backpressure_cycle_ring3_deadline` | 3-task buffered ring with all queues full | Backpressure cycles, deadline escape |
| `worker_pool_heavy` | 8 workers processing 500 jobs | Worker pool pattern, job throughput |
| `fanout_fanin` | Scatter-gather with 16 workers | Fan-out/fan-in, parallel processing |
| `closure_capture_storm` | 100 closures capturing different vars | Closure allocation, capture semantics |
| `unbuffered_rendezvous` | 50 producer/consumer pairs (sync) | Unbuffered channel rendezvous |
| `arena_concurrent` | 10 tasks allocating from shared arena | Arena thread safety |
| `arena_memory_storm` | Tip, stranding, budget→ovf, reset ping-pong, mixed align, churn, stack spill, concurrent nursery arenas | RSS / waste / reset reclaim |
| `arena_lifetime_chaos` | Mixed lifetimes, scramble-release, tip-vs-churn, shared fixed concurrent; checksums + RSS peak/residual (link `mem_sample.o`) | Lifetime / reclaim / false-pass |
| `arena_mixed_lifetime_bench` | Idiomatic mixed-lifetime RESULT peer (arena+release) | Cross-lang strategy compare |
| `c/` `go/` `zig/mixed_lifetime_bench.*` | malloc / GC peers (same workload, idiomatic reclaim) | Cross-lang baseline |
| `compare_mixed_lifetime.sh` | Shuffled multi-trial idiomatic mixed-lifetime compare | Strategy tradeoff map |
| `arena_memory_bench` | Heavy RESULT peer (tip/bulk/ovf/reset/churn) | Arena happy path vs escape |
| `c/` `go/` `zig/arena_memory_bench.*` | C / Go / Zig bump peers (same protocol) | Cross-lang baseline |
| `c/malloc_memory_bench.c` | Raw malloc/realloc baseline (same size streams) | Is plain malloc faster? |
| `compare_arena_memory.sh` | Build all, shuffle order per trial, average RESULT | Startup-fair compare |
| `join_handoff_storm` | Deep join chains on one worker | Join handshake ordering |
| `park_unpark_storm` | Single-worker unbuffered receive storm | Park/unpark correctness |
| `inbox_cross_worker_storm` | Nested spawns across workers | Inbox routing + stealing |

## Demos (Manual Run)

| Test | Description |
|------|-------------|
| `deadlock_detect_demo` | Intentional deadlock to demo detection (watchdog default-on) |

## Running

```bash
# Run a single test
./cc/bin/ccc run stress/channel_flood.ccs

# Run all stress tests
make stress-check

# Cross-lang arena memory compare (shuffled multi-trial; includes malloc baseline)
./stress/compare_arena_memory.sh
# ARENA_MEM_QUICK=1 ./stress/compare_arena_memory.sh          # lighter smoke
# ARENA_MEM_TRIALS=5 ARENA_MEM_SEED=42 ./stress/compare_arena_memory.sh

# Tier policy sweep (small root): block_max=1 vs 2/4/8 vs unbounded(0)
./stress/compare_arena_tiers.sh
# ARENA_MEM_QUICK=1 ARENA_TIER_POLICIES="1 4 8 0" ./stress/compare_arena_tiers.sh

# Root × N heatmap (where bulk_ovf dies — justify default block_max)
./stress/compare_arena_heatmap.sh
# ARENA_MEM_QUICK=1 ./stress/compare_arena_heatmap.sh

# Opposite case: cc_arena_malloc used for tons of scratch allocs
./stress/compare_arena_malloc_cost.sh

# Idiomatic mixed-lifetime compare (arena+release vs malloc vs GC)
./stress/compare_mixed_lifetime.sh
# MIX_LIFE_QUICK=1 ./stress/compare_mixed_lifetime.sh
#
# Memory: judge reclaim by arena gross/ovf; RSS peak is color. On Linux,
# RESULT lines also carry VmHWM + cgroup memory.peak (best in a container).
# macOS end-RSS often lags free — not leak proof.
# Shared helper: stress/mem_sample.h + mem_sample.c (host .o linked into CC bins).

# Run with sanitizers (TSan/ASan)
./scripts/stress_sanitize.sh tsan
./scripts/stress_sanitize.sh asan

# Manual loop (if needed)
for f in stress/*.ccs; do
    echo "=== $f ==="
    ./cc/bin/ccc run "$f" || echo "FAILED: $f"
done
```

## Deadlock Detection

Concurrent-C includes runtime deadlock detection (enabled by default). On detection it
aborts with exit code 124. To keep running (print the banner but do **not** exit), set
`CC_DEADLOCK_ABORT=0`:

```bash
CC_DEADLOCK_ABORT=0 ./cc/bin/ccc run stress/deadlock_detect_demo.ccs
```

When enabled, the runtime:
1. Tracks how many threads are blocked on channel ops or `cc_block_on`
2. Tracks progress (successful channel ops, task completions)
3. If all threads are blocked for 3+ seconds with no progress → **likely deadlock**

Example output:
```
╔══════════════════════════════════════════════════════════════╗
║              ⚠️  POTENTIAL DEADLOCK DETECTED ⚠️               ║
╠══════════════════════════════════════════════════════════════╣
║ 2 thread(s) blocked for 3.0+ seconds with no progress.     ║
╠══════════════════════════════════════════════════════════════╣
║ Blocked threads:                                             ║
╚══════════════════════════════════════════════════════════════╝
  Thread 0: blocked on cc_block_on (waiting for async task)
  Thread 1: blocked on chan_recv (channel empty, waiting for sender)

Common causes:
  • cc_block_on() inside spawn() or a nursery task
  • Producer/consumer mismatch (sends without receivers)
  • Missing channel close (receiver waiting forever)
```

## Writing New Stress Tests

Good stress tests should:
1. Use idiomatic CC patterns (nurseries, channels, closures)
2. Have measurable success criteria (counts, sums)
3. Exercise specific runtime components
4. Complete in reasonable time (<30s)
5. Be deterministic (same result each run)
