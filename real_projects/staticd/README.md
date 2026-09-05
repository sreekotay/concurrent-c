# staticd — Concurrent-C static HTTP server

A one-file HTTP/1.1 static file server in Concurrent-C. Connection accept,
header encode, and body flush are separated on the page the same way
[`redis_async_sketch.ccs`](../redis/redis_async_sketch.ccs) separates hold,
encode, and ship.

**v1 scope:** `GET`/`HEAD`, keep-alive, `Content-Length`, extension
`Content-Type`. No TLS, Range, gzip, or HTTP/2.

## Quick start

```bash
cd real_projects/staticd
./setup.sh                  # fixtures + darkhttpd sources; brew nginx/wrk if available
make staticd darkhttpd
./compare.sh --smoke        # correctness + short latency row
```

Manual serve:

```bash
./out/staticd --listen 127.0.0.1:8080 --root ./fixtures
curl -D- http://127.0.0.1:8080/4kb.html | head
./out/staticd --help
```

## Architecture

```
accept (@parallel dest)
  └─ handle_client
       └─ loop: BufReader request
            ├─ path_is_safe (reject ..)
            ├─ mmap file
            ├─ @typeview Encode → status + headers into Conn.out
            ├─ conn_flush (headers only)
            └─ write_all(mmap body)   # large files never copy into the arena
```

Encode methods never touch the socket. Body bytes stay in the mapped file
until after header flush — arenas name the header lifetime, not the payload.

## Benchmark design (latency-first)

Peers (best-effort; missing peers are skipped):

| Server | Port | Role |
|--------|------|------|
| **staticd** | 8080 | CC specimen |
| **nginx** | 8081 | Production bar (`sendfile`) |
| **darkhttpd** | 8082 | Honest tiny-C peer |
| **caddy** | 8083 | Optional (`INCLUDE_CADDY=1`) |

```bash
./correctness.sh            # status + Content-Length + body SHA-256 + traversal
./bench_latency.sh          # directional: 3s × 3 rounds, 4kb / 1mb / 10mb
FULL=1 ./bench_latency.sh   # receipt: 30s × 5 rounds, five files
SMOKE=1 ./bench_latency.sh  # 2s, 4kb.html @ c=10
```

Knobs: `REPEATS`, `DURATION`, `CONCURRENCY`, `FILES`, `FULL`, `SMOKE`,
`INCLUDE_NGINX`, `INCLUDE_DARKHTTPD`, `INCLUDE_CADDY`, `BENCH_OUT`.

Receipt columns match `wrk --latency`: **p50 / p75 / p90 / p99** (ms), RPS,
errors. Fixtures are deterministic (`gen_fixtures.sh`); bodies are gitignored,
`fixtures/manifest.txt` is checked in. Each bench block page-caches the
fixture tree and randomizes server order (same discipline as redis
`bench_robust.sh`).

### Expectation

staticd is not expected to beat nginx on p99 at high concurrency. The
specimen wins on **readable ownership on the page** and **honest receipts**.
Chase **darkhttpd parity** first; nginx is the aspirational bar.

### Headline (2026-09-05, this machine)

From [`benchmarks/staticd_2026_09_04.txt`](benchmarks/staticd_2026_09_04.txt)
(wrk, 5s, REPEATS=3 with round 0 discarded, median of measured):

**`4kb.html @ c=100`** (representative small-file / load-test):

| server | p50 ms | p99 ms | rps |
|--------|--------|--------|-----|
| nginx | 0.98 | 92.7 | 58.9k |
| staticd | 2.13 | 17.5 | 43.8k |
| darkhttpd | 4.64 | 60.7 | 16.5k |

**`10mb.bin @ c=10`** (large-file streaming):

| server | p50 ms | p99 ms | rps |
|--------|--------|--------|-----|
| nginx | 11.5 | 93.0 | 352 |
| staticd | 11.8 | 100.9 | 383 |
| darkhttpd | 46.0 | 97.4 | 204 |

staticd clears darkhttpd on both rows and tracks nginx on large-file RPS;
nginx still leads small-file throughput at saturation. Full matrix in
[`benchmarks/`](benchmarks/).

## Layout

| Path | Purpose |
|------|---------|
| `staticd.ccs` | Server |
| `gen_fixtures.sh` | Build fixture tree + manifest |
| `correctness.sh` | Golden gate |
| `bench_latency.sh` | Latency matrix |
| `compare.sh` | correctness + bench (`--smoke`) |
| `nginx.conf.in` / `Caddyfile.in` | Peer configs (`FIXTURES_ROOT` substituted) |
| `setup.sh` | Fetch darkhttpd; brew nginx/wrk/hey |
| `bench_string_tpl.ccs` | Microbench: `@string` vs `snprintf` (path + header); `make string_tpl` |
