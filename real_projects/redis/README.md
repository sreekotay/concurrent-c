# redis_cc

This project mirrors the structure of `real_projects/pigz/`, but targets Redis.

The supported Concurrent-C Redis variants are:

- `redis_idiomatic.ccs` — default / complete server: sketch control flow
  (hold → encode into Conn → flush) over full RedisDb. Shared reply helpers
  in `redis_reply.cch`. Benchmark target for `./bench_robust.sh`.
- `redis_async_sketch.ccs` — tiny command surface twin of `redis.go`
  (`ln.serve` + Conn encode). Teaching peer of the complete server.
  Default listen `127.0.0.1:6381`. Not the bench target.
- `redis_go_twin.ccs` — Go-sized teaching twin of `redis.go` (same command
  surface; `acquire_into` for holds). Not the bench target.
- `redis_idiomatic_spawn_async.ccs` — `@async` / `spawn_async` twin of the
  default (fixed `out_buf` encode). Optional; used by the async line-map gate.
- `redis_owner.ccs` — channel / single-owner-fiber variant (historical).
- `redis_cc/redis_cc.ccs` — future modular production port (scaffold).

## Layout

- `setup.sh` fetches upstream Redis into `redis_c/`
- `redis_idiomatic.ccs` is the default single-file implementation
- `redis_async_sketch.ccs` is the tiny sketch sibling (`make redis_async_sketch`)
- `redis_go_twin.ccs` / `redis.go` are paired minimal architecture sketches
  (`make redis_go_twin`)
- `redis_idiomatic_spawn_async.ccs` is the `@async` alternate shell
  (`make redis_idiomatic_spawn_async`)
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

1. accept via `ln.serve` + one sync fiber per connection
2. RESP decode on the connection side (grammar; argv borrows read buffer)
3. execute under a per-shard named exclusive (`CCExclusive`; shard count =
   next power of two of online CPUs — no user tuning). Optional
   `CC_REDIS_SHARDS` is a bench override only.
4. RESP encode into Conn.out after hold release; flush at end of pipeline window

`redis_owner` instead routes commands over channels to a single owner fiber
(step 3–4 differ; always one shard).

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
