# Main Pre-V2 Baseline (2026-04-14)

These numbers were captured on `main` after landing the Phase 1 `err` syntax slice and before any V2/runtime merge.

## Contention

Command:

```sh
CC_CONTENTION_TRIALS=1 CC_CONTENTION_ITERATIONS=10000 ./perf/compare_contention.sh
```

Results:

| Implementation | Baseline (ms) | Contention (ms) | Interference |
| --- | ---: | ---: | ---: |
| Pthread | 0.33 | 1.15 | 243.41% |
| Concurrent-C | 0.28 | 0.74 | 162.19% |
| Go | 0.28 | 0.41 | 46.88% |

## Spawn Substrate

Command:

```sh
./cc/bin/ccc run perf/perf_spawn_v2_vs_thread.ccs
```

Results:

| Metric | Value |
| --- | ---: |
| `cc_thread_spawn` median | 6.987 ms |
| `cc_fiber_spawn_task_v2` median | 8.353 ms |
| V2 / thread throughput | 83.6% |
| V2 delta | 0.33 us/task |

## Redis (`redis_idiomatic`)

Build:

```sh
make -C real_projects/redis upstream redis_idiomatic
```

### Clients 50, Pipeline 1

Command:

```sh
REQUESTS=50000 CLIENTS=50 PIPELINE=1 REPEATS=3 ./real_projects/redis/bench_simple.sh
```

| Command | Upstream RPS | Idiomatic RPS | Idiomatic/Upstream | Upstream p50 ms | Idiomatic p50 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| SET | 139275.77 | 139664.80 | 1.003x | 0.183 | 0.207 |
| GET | 145348.83 | 139664.80 | 0.961x | 0.183 | 0.207 |
| INCR | 132626.00 | 131926.12 | 0.995x | 0.199 | 0.215 |

### Clients 50, Pipeline 16

Command:

```sh
REQUESTS=50000 CLIENTS=50 PIPELINE=16 REPEATS=3 ./real_projects/redis/bench_simple.sh
```

| Command | Upstream RPS | Idiomatic RPS | Idiomatic/Upstream | Upstream p50 ms | Idiomatic p50 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| SET | 1562499.88 | 1470588.12 | 0.941x | 0.447 | 0.351 |
| GET | 1923076.88 | 1470588.12 | 0.765x | 0.335 | 0.367 |
| INCR | 1724138.00 | 1388889.00 | 0.806x | 0.375 | 0.383 |

### Clients 1, Pipeline 1

Command:

```sh
REQUESTS=50000 CLIENTS=1 PIPELINE=1 REPEATS=3 ./real_projects/redis/bench_simple.sh
```

| Command | Upstream RPS | Idiomatic RPS | Idiomatic/Upstream | Upstream p50 ms | Idiomatic p50 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| SET | 55493.89 | 40551.50 | 0.731x | 0.015 | 0.023 |
| GET | 58072.01 | 38372.98 | 0.661x | 0.015 | 0.023 |
| INCR | 58616.65 | 35790.98 | 0.611x | 0.015 | 0.023 |
