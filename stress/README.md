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
| `worker_pool_heavy` | 8 workers processing 500 jobs | Worker pool pattern, job throughput |
| `fanout_fanin` | Scatter-gather with 16 workers | Fan-out/fan-in, parallel processing |
| `closure_capture_storm` | 100 closures capturing different vars | Closure allocation, capture semantics |
| `defer_cleanup_storm` | 100 tasks with nested defers | Defer cleanup under concurrency |
| `unbuffered_rendezvous` | 50 producer/consumer pairs (sync) | Unbuffered channel rendezvous |
| `arena_concurrent` | 10 tasks allocating from shared arena | Arena thread safety |
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

Concurrent-C includes runtime deadlock detection (enabled by default). Disable it with:

```bash
CC_DEADLOCK_DETECT=0 ./cc/bin/ccc run stress/deadlock_detect_demo.ccs
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
  • cc_block_on() inside spawn() or @nursery
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
