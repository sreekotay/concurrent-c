# Concurrent-C Stress Tests

Stress tests that push the compiler and runtime with demanding patterns.

## Tests

| Test | Description | What it stresses |
|------|-------------|------------------|
| `channel_flood` | Many producers flooding a single consumer | Channel backpressure, contention |
| `spawn_storm` | Spawn 1000 concurrent tasks | Nursery spawn overhead, scheduling |
| `nursery_deep` | 20 levels of nested nurseries | Nursery stack, structured concurrency |
| `pipeline_long` | 50-stage processing pipeline | Channel chaining, throughput |
| `deadline_race` | Many tasks with competing deadlines | Deadline handling, timing |
| `worker_pool_heavy` | 8 workers processing 500 jobs | Worker pool pattern, job throughput |
| `fanout_fanin` | Scatter-gather with 16 workers | Fan-out/fan-in, parallel processing |
| `closure_capture_storm` | 100 closures capturing different vars | Closure allocation, capture semantics |

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

## Writing New Stress Tests

Good stress tests should:
1. Use idiomatic CC patterns (nurseries, channels, closures)
2. Have measurable success criteria (counts, sums)
3. Exercise specific runtime components
4. Complete in reasonable time (<30s)
5. Be deterministic (same result each run)
