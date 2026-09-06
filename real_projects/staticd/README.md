# staticd — Concurrent-C static HTTP/1.1 server

A small HTTP/1.1 file server written as a Concurrent-C specimen. Accept,
header encode, and body flush are separate on the page the same way
[`redis_async_sketch.ccs`](../redis/redis_async_sketch.ccs) separates hold,
encode, and ship. The file you read is the server you run.

## Features

| | |
|---|---|
| Methods | `GET`, `HEAD`, `OPTIONS` (204 + `Allow`) |
| HTTP/1.1 | keep-alive (HTTP/1.1 default). `Connection` is a token list; `close` dominates |
| Time | `Date`, `Last-Modified`, `If-Modified-Since` → 304 |
| Range | one `Range: bytes=` → 206 / 416; `If-Range` (date match → 206, else 200) |
| MIME | extension → `static_map` → wire string (`text/html`, `application/javascript`, …) |
| Jail | Per-component `openat(O_NOFOLLOW)` under the docroot fd (intermediates `O_DIRECTORY`). `.` / `..` / `//` / `/./` → 403. Intermediate and leaf symlinks do not escape. No rewrite, no chroot |
| Index | `--index NAME` (default `index.html`) for `/` and directory URLs |
| Listing | `--list` (off). Directory with no index → HTML table; without `--list` → 403 |
| Query | `?…` is recognized and ignored |
| Extra headers | `--header 'Name: value'` (repeatable; no CR/LF) |
| Body | Named-block ring: 256 × 64KB = 16MB BSS, key `(dev, ino, block)`, FNV probe only, reuse in place, idle cull. Pool `pread` on miss / unaligned Range / busy fill. 8-slot fd cache; pathname revalidate ≤1s (absolute); hold dups the fd. Accept dest `g_clients`; chunk dest is a frame dest the send waits; turnstile cap 2. Ring and fd cache are two `CCExclusive` names |

**Not in scope:** TLS, gzip, HTTP/2, multipart ranges, sendfile, directory
listing on by default, CGI.

## Build

From this directory. `ccc` is `../../out/cc/bin/ccc` (repo `make cc`).

```bash
cd real_projects/staticd
./setup.sh                  # fixtures + darkhttpd sources; brew nginx/wrk if missing
make staticd                # ./out/staticd
make darkhttpd              # optional peer
./gen_fixtures.sh           # 1kb / 4kb / 64kb / 1mb / 10mb + index.html
```

`make` / `make all` builds staticd only. `make setup` is `./setup.sh`.
`make clean` removes `out/`.

## Run

```bash
./out/staticd --listen 127.0.0.1:8080 --root ./fixtures
./out/staticd --help
```

| Flag | Default | |
|---|---|---|
| `-l` / `--listen ADDR` | `127.0.0.1:8080` | `host:port` |
| `-r` / `--root DIR` | `fixtures` | document root |
| `--index NAME` | `index.html` | one path segment; no `/` or `..` |
| `--list` | off | listing when the index is missing |
| `--header LINE` | none | extra response header; repeatable |

```bash
# CORS + listing
./out/staticd --root ./fixtures --list \
    --header 'Access-Control-Allow-Origin: *'
```

```bash
curl -D- http://127.0.0.1:8080/4kb.html | head
curl -D- 'http://127.0.0.1:8080/4kb.html?v=1' | head
curl -D- -H 'Range: bytes=0-15' http://127.0.0.1:8080/4kb.html | head
```

## Check

```bash
./correctness.sh            # each peer: status, Content-Length, SHA-256, traversal
                            # staticd also: OPTIONS, query strip, --header,
                            # --index, --list, dir-without-list → 403,
                            # Range / 304 / If-Range, Connection tokens,
                            # symlink jail, rename under a hot name
```

Missing nginx / darkhttpd / caddy are skipped. Traversal may be 400, 403, or
404; staticd is 403.

## Bench

Latency-first. Peers (missing ones are skipped):

| Server | Port | |
|--------|------|---|
| **staticd** | 8080 | this specimen |
| **nginx** | 8081 | `sendfile on`, `tcp_nopush on`, one worker |
| **darkhttpd** | 8082 | |
| **caddy** | 8083 | `INCLUDE_CADDY=1` |

```bash
make smoke                  # correctness + 2s wrk, 4kb.html @ c=10
make bench                  # ./bench_latency.sh (isolated RSS)
./compare.sh --smoke
./bench_latency.sh          # directional: 1s × 3 rounds, 4kb / 1mb / 10mb, c=1/10/100
FULL=1 ./bench_latency.sh   # receipt: 30s × 5, five files
SMOKE=1 ./bench_latency.sh  # 2s, 4kb.html @ c=10 only
ISOLATE=0 ./bench_latency.sh  # keep all peers up (RSS then cumulative)
./compare.sh                # correctness + directional
./compare.sh --full         # correctness + receipt
```

Knobs: `REPEATS`, `DURATION`, `CONCURRENCY`, `FILES`, `FULL`, `SMOKE`,
`ISOLATE`, `TIMEOUT`, `INCLUDE_NGINX`, `INCLUDE_DARKHTTPD`,
`INCLUDE_CADDY`, `BENCH_OUT`.

Receipt columns: **p50 / p75 / p90 / p99** (ms), RPS, process RSS, errors.
Fixtures are deterministic (`gen_fixtures.sh`); bodies are gitignored,
`fixtures/manifest.txt` is checked in. Each block page-caches the fixture
tree and shuffles server order. `ISOLATE=1` (default) starts a fresh
process for that cell only — RSS is the cell, not leftover stacks.

Local receipts land under `benchmarks/` (gitignored). rps/p50: Darwin
25.5.0 arm64, 10 CPUs, wrk `-t2 -d5s --timeout 15s`, 3 rounds, median
(round 0 discarded). RSS: `ISOLATE=1`, fresh process, 2s cell (`rss_kb/1024`;
nginx = master + workers). Zero socket errors on every cell.

| file | c | staticd rps | nginx | darkhttpd | staticd p50 | nginx p50 | darkhttpd p50 | staticd RSS | nginx RSS | darkhttpd RSS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4kb.html | 1 | 54385 | 32628 | 27854 | 0.017 | 0.030 | 0.035 | 2.3 | 8.7 | 1.7 |
| 4kb.html | 10 | 140496 | 72104 | 55282 | 0.057 | 0.131 | 0.158 | 4.0 | 8.6 | 1.7 |
| 4kb.html | 100 | 145354 | 72916 | 56274 | 0.605 | 1.31 | 1.73 | 18.3 | 8.7 | 1.7 |
| 1mb.bin | 1 | 7354 | 6845 | 5239 | 0.132 | 0.142 | 0.176 | 3.6 | 8.7 | 1.7 |
| 1mb.bin | 10 | 10441 | 14238 | 6212 | 0.93 | 0.405 | 1.59 | 6.5 | 8.8 | 1.7 |
| 1mb.bin | 100 | 9972 | 12744 | 5886 | 9.79 | 4.55 | 16.79 | 29.9 | 9.5 | 1.8 |
| 10mb.bin | 1 | 716 | 795 | 574 | 1.33 | 1.23 | 1.69 | 12.6 | 8.7 | 1.7 |
| 10mb.bin | 10 | 756 | 1013 | 544 | 12.6 | 7.90 | 17.29 | 15.0 | 8.8 | 1.7 |
| 10mb.bin | 100 | 666 | 0 | 559 | 134 | — | 174 | 41.3 | 9.5 | 1.7 |

nginx 10mb / c=100 completed 0 requests. 10mb / c=1 RSS is the ~10MB of
ring pages plus ~2MB base; c=100 is fiber stacks on top of that.

After wrk disconnects, dest children drop themselves off the live index
when the fiber is done. Idle RSS is the fiber pool floor (default 512
stacks, ~85 MB here), not leftover connections.

## Shape

```
accept (@parallel dest g_clients)
  └─ handle_client
       └─ loop: BufReader request
            ├─ ReqLine (query stripped) + path_is_safe
            ├─ FileHold → component-walk jail / 8-slot fd cache (pathname ≤1s)
            │    or directory: --index, else --list / 403
            ├─ @typeview Encode → status + headers into Conn.out
            ├─ conn_flush (headers only)
            └─ send_file_body → checkout_block (256 × 64KB ring)
                 miss / unaligned Range / busy fill → g_send_pool pread
                 n > 64KB: frame dest + turnstile cap 2
                           (wait dest before the stack turnstile dies)
```

Encode methods never touch the socket. The jail walks each path
component with `openat(O_NOFOLLOW)`. The fd cache re-resolves the name
at most once a second (absolute, not idle-sliding); a hold dups the fd
so a `rename` swap can close the slot while in-flight holds finish. The
ring reuses the same `(dev, ino, block)` slot; idle `refs==0` slots
become holes once a second.

## Architecture

Same bar as redis accept / encode / ship. Three names, do not mix:

| Name | What it is |
|------|------------|
| **Accept dest** | `g_clients`. Process-lifetime. One dest-attach per accepted socket. A bad GET must not `fail` this dest. Occupancy is who is still running — a finished child claims itself off the live index. |
| **Chunk dest** | Frame `pipe` inside `send_file_chunks`. Dest-attach the window writes here. The frame `@defer`s `pipe.wait()` so those fibers join before the stack turnstile is destroyed. |
| **Tickets** | Turnstile `enter` / `wait` / `leave` / `pass`. Cap 2 in-flight chunk writes. Fail tickets on the error path so waiters unstick. |

`g_clients` outlives any one send. Dest-attaching chunk writes onto it
would leave them running after `send_file_chunks` returns; the turnstile
lives on `cc_arena_stack`. Join the send dest first.

Ring and fd cache share one `CCExclusive` (`cli_a.create_exclusive(4)`),
names `SYNC_BLOCK` and `SYNC_FC`. Hold is metadata only: drop the lock
before `pread` / `openat`. When a pathname refresh swaps the cached
inode, order is file cache then ring (`block_cull_id`). A contended
exclusive parks the fiber, not the OS worker. Date / Last-Modified
format onto the request arena.

Ring identity is `(dev, ino, block)` from `fstat` after `openat`. FNV-1a
of that triple is the probe start only — a path hash is not a file.
Hardlinks share a slab. `ino == 0` or an unaligned Range goes to
`g_send_pool`. A same-key GET while `ready == 0 && refs != 0` pools too
(no half-fill, no condvar). A warm other key is not stolen; pressure
pools. `g_send_pool` is `cc_arena_pool_stack` at the top of `main`
(an 8 MB VLA SIGSEGVs Darwin's default stack).

## Layout

| Path | Purpose |
|------|---------|
| `staticd.ccs` | Server (`handle_one` is the story) |
| `staticd_http.cch` | Date / Range / header-CI tape |
| `staticd_block.cch` | Named-block ring (`checkout_block`) |
| `staticd_fs.cch` | Jail, `FileHold`, 1s fd cache, listing |
| `gen_fixtures.sh` | Fixture tree + manifest |
| `correctness.sh` | Golden gate |
| `bench_latency.sh` | Latency matrix |
| `compare.sh` | correctness + bench (`--smoke` / `--full`) |
| `nginx.conf.in` / `Caddyfile.in` | Peer configs (`FIXTURES_ROOT` substituted) |
| `setup.sh` | Fetch darkhttpd; brew nginx/wrk/hey |
| `bench_string_tpl.ccs` | `@string` vs `snprintf`; `make string_tpl` |
