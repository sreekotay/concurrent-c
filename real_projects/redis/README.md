# redis_cc

This project mirrors the structure of `real_projects/pigz/`, but targets Redis.

The supported Concurrent-C Redis variants are:

- `redis_idiomatic.ccs` — default server (table-wide `CCExclusive`; connection
  fibers execute in place). Benchmark target for `./bench_robust.sh`.
- `redis_owner.ccs` — channel / single-owner-fiber variant (historical).
- `redis_cc/redis_cc.ccs` — future modular production port (scaffold).

## Layout

- `setup.sh` fetches upstream Redis into `redis_c/`
- `redis_idiomatic.ccs` is the default single-file implementation
- `redis_owner.ccs` is the N:1 owner-fiber alternative (`make redis_owner`)
- `redis_smoke.py` is the functional smoke (basics, expiry, 1000-op pipeline,
  abrupt-disconnect storm); it spawns `out/redis_idiomatic` itself:
  `python3 redis_smoke.py`
- `redis_cc/redis_cc.ccs` is the multi-file production port (scaffold)
- `reply_path_bench.ccs` and `reply_path_threaded_bench.ccs` are explicit reply-path microbench experiments, not server variants
- `bench_robust.sh` runs an order-randomized, warmup-discarded variant with per-round statistics
- `bench_lock_cmp.sh` compares upstream vs idiomatic vs owner
- `bench_conn_sweep.sh` sweeps `redis-benchmark -c` and prints `redis_idiomatic` RSS during/after load (per-connection vs fixed baseline)
- `bench_then_memlog.sh`, `mem_account.sh`, and `profile_compare.sh` are focused memory/profile helpers

## Upstream Redis Policy

Upstream Redis is treated as read-only reference material:

- fetch it locally
- build it locally
- benchmark against it locally
- do not patch it
- do not commit it to git

The intended git boundary is:

- commit: Concurrent-C sources, scripts, docs, and reproducibility metadata
- ignore: `redis_c/`, tarballs, binaries, local benchmark artifacts, `.rdb`, `.aof`

## Shared Architecture

Default (`redis_idiomatic`) shape:

1. accept loop
2. one fiber per client connection
3. RESP decode on the connection side
4. execute under a table-wide named exclusive (`CCExclusive`)
5. RESP encode on the connection side after release

`redis_owner` instead routes commands over channels to a single owner fiber
(step 4–5 differ). The main future scaling knob is shard / per-key exclusives,
not a different programming model.

## Bootstrap

```bash
./setup.sh
make upstream
make redis_idiomatic redis_cc
```

Quick comparison runs:

```bash
cd real_projects/redis
./bench_robust.sh                       # order-randomized, warmup-discarded
PIPELINE=16 ./bench_robust.sh
CLIENTS=1 PIPELINE=1 ./bench_robust.sh
REPEATS=5 PIPELINE=16 ./bench_robust.sh
CLIENTS_SWEEP="1 5 50" ./bench_conn_sweep.sh   # RSS vs concurrent clients
```

`CLIENTS` controls connection concurrency (`redis-benchmark -c`).
`PIPELINE` controls pipeline depth (`redis-benchmark -P`).
`REPEATS` runs the suite multiple times and reports median/range summaries.

`redis_cc` remains a scaffold for the eventual modular port. `redis_idiomatic`
(table lock) is the current default runnable implementation.
