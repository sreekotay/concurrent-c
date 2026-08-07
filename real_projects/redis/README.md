# redis_cc

This project mirrors the structure of `real_projects/pigz/`, but targets Redis.

The supported Concurrent-C Redis variants are:

- `redis_idiomatic.ccs` — default server (hash-sharded `CCExclusive` by CPU
  count; connection fibers execute in place). Benchmark target for
  `./bench_robust.sh`.
- `redis_go_twin.ccs` — Go-sized teaching twin of `redis.go` (same command
  surface; `acquire_into` for holds). Not the bench target.
- `redis_async_sketch.ccs` — same tiny command surface as `redis_go_twin`,
  with `ln.serve` + `n->spawn(() => handle_client(...))` per connection
  (sync fiber; BufReader parks). Default listen `127.0.0.1:6381`.
  Not the bench target.
- `redis_owner.ccs` — channel / single-owner-fiber variant (historical).
- `redis_cc/redis_cc.ccs` — future modular production port (scaffold).

## Layout

- `setup.sh` fetches upstream Redis into `redis_c/`
- `redis_idiomatic.ccs` is the default single-file implementation
- `redis_go_twin.ccs` / `redis.go` are paired minimal architecture sketches
  (`make redis_go_twin`)
- `redis_async_sketch.ccs` is the async-IO sibling of that sketch
  (`make redis_async_sketch`)
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
4. execute under a per-shard named exclusive (`CCExclusive`; shard count =
   next power of two of online CPUs — no user tuning). Optional
   `CC_REDIS_SHARDS` is a bench override only.
5. RESP encode on the connection side after release

`redis_owner` instead routes commands over channels to a single owner fiber
(step 4–5 differ; always one shard).

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
is the current default runnable implementation.
