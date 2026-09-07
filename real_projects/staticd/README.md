# staticd — Concurrent-C static HTTP/1.1 + WebSocket

A small HTTP/1.1 file server written as a Concurrent-C specimen.
Sessions are rows; dests are workers. `poll()` waits; a ready fd steps
that row. The worker is the only writer. The file you read is the
server you run.

Dest-per-connection accept lives in
[`examples/recipe_tcp_echo.ccs`](../../examples/recipe_tcp_echo.ccs)
and redis. Not a second file server.

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
| Workers | Start 2, grow every 64 live conns, cap ncpu/2 (`--workers 0`). `--workers N` is the cap. `--workers 1` stays one dest |
| Pages | `--pages DIR` (off). Load `hello.js` (QuickJS) or `hello.py` (CPython) as a view: `GET(request)` → `Response`. Same ABI; both faces at one path → 500. Never executes `--root` `*.js` |
| TLS | `--tls-cert PEM` + `--tls-key PEM` (off). BearSSL server; process-wide load at startup. Build with `CC_ENABLE_TLS=1` (Makefile default) |
| WebSocket | Narrow echo subset: `Upgrade` + `Connection: upgrade` + `Sec-WebSocket-Version: 13` + base64-16 `Sec-WebSocket-Key` → `101`, then echo (text / binary), pong for ping, close for close. No fragments; payload capped to the row window |
| Body | Named-block ring: 256 × 64KB = 16MB BSS, key `(dev, ino, block)`, FNV probe only, reuse in place, idle cull. Pool `pread` on miss / unaligned Range / busy fill. 8-slot fd cache; pathname revalidate ≤1s (absolute); hold dups the fd. Send is a cursor on the row: one 64KB chunk per step, `POLLOUT` while left |

**Not in scope:** gzip, HTTP/2, multipart ranges, sendfile, directory
listing on by default, CGI, Node `require`, Django ORM.

## Build

From this directory. `ccc` is `../../out/cc/bin/ccc` (repo `make cc`).
TLS needs BearSSL (`make -C ../../cc bearssl`) and a runtime built with
`CC_ENABLE_TLS=1` — `make staticd` does both by default.

```bash
cd real_projects/staticd
./setup.sh                  # fixtures + darkhttpd sources; brew nginx/wrk if missing
make staticd                # ./out/staticd (TLS-capable)
make darkhttpd              # optional peer
./gen_fixtures.sh           # 1kb / 4kb / 64kb / 1mb / 10mb + index.html
```

`make` / `make all` builds staticd only. `make setup` is `./setup.sh`.
`make clean` removes `out/`. `CC_ENABLE_TLS=0 make staticd` skips BearSSL
(link will fail if the binary still references TLS symbols).

## Run

```bash
./out/staticd --listen 127.0.0.1:8080 --root ./fixtures
./out/staticd --workers 1   # one dest (isolate receipt / the page)
./out/staticd --root ./fixtures --pages ./pages --workers 1
./out/staticd --listen 127.0.0.1:8443 --root ./fixtures \
  --tls-cert ../../third_party/bearssl/samples/cert-ee-rsa.pem \
  --tls-key  ../../third_party/bearssl/samples/key-ee-rsa.pem
./out/staticd --help
```

| Flag | Default | |
|---|---|---|
| `-l` / `--listen ADDR` | `127.0.0.1:8080` | `host:port` |
| `-r` / `--root DIR` | `fixtures` | document root |
| `--pages DIR` | off | script pages jail (`.js` / `.py` views) |
| `--tls-cert PATH` | off | PEM cert chain (with `--tls-key`) |
| `--tls-key PATH` | off | PEM private key (with `--tls-cert`) |
| `-w` / `--workers N` | ncpu/2 (`0`) | cap; start 2, grow with live conns. `1` = one dest |
| `--index NAME` | `index.html` | one path segment; no `/` or `..` |
| `--list` | off | listing when the index is missing |
| `--header LINE` | none | extra response header; repeatable |

### Script pages

One process-wide QuickJS and one CPython (mutex across workers), so
in-memory page state is shared. Attach QuickJS with `CC_QUICKJS_SRC` (or
`./quickjs`); Python needs a discoverable libpython.

`pages/hello.js`:

```js
export function GET(request) {
  return new Response(`hello ${request.path}\n`, {
    headers: { "content-type": "text/plain; charset=utf-8" },
  });
}
```

`pages/hello.py` (same path as `hello.js` → 500; use one face per path):

```python
def GET(request):
    return Response(f"hello {request.path}\n", content_type="text/plain; charset=utf-8")
```

`GET /hello` → `hello.js` or `hello.py`. Both → 500. Missing → fall through
to `--root`. Throw / no export → 500. Engine missing → 501 (never the
source as `application/javascript`). The sample `pages/` tree ships both
faces for the dual-file check; for a live `/hello`, keep only one.

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
                            # symlink jail, rename under a hot name, WS echo
```

Missing nginx / darkhttpd / caddy are skipped. Traversal may be 400, 403, or
404; staticd is 403.

## Bench

Latency-first. Peers (missing ones are skipped):

| Server | Port | |
|--------|------|---|
| **staticd** | 8080 | this specimen |
| **nginx** | 8081 | `sendfile on`, `tcp_nopush on`, `multi_accept on`, `worker_connections 8192`, one worker |
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
STATICD_WORKERS=4 ./bench_latency.sh
./compare.sh                # correctness + directional
./compare.sh --full         # correctness + receipt
```

Knobs: `REPEATS`, `DURATION`, `CONCURRENCY`, `FILES`, `FULL`, `SMOKE`,
`ISOLATE`, `TIMEOUT`, `INCLUDE_NGINX`, `INCLUDE_DARKHTTPD`,
`INCLUDE_CADDY`, `STATICD_WORKERS`, `BENCH_OUT`.

Receipt columns: **p50 / p75 / p90 / p99** (ms), RPS, process RSS, errors.
Fixtures are deterministic (`gen_fixtures.sh`); bodies are gitignored,
`fixtures/manifest.txt` is checked in. Each block page-caches the fixture
tree and shuffles server order. `ISOLATE=1` (default) starts a fresh
process for that cell only — RSS is the cell.

Local receipts land under `benchmarks/` (gitignored). Isolate cells pass
`--workers 1` so the nginx peer stays one worker. Default starts two dests
and grows to ncpu/2 as `CCServer.live` crosses 192, 256, … — the accept storm
splits the zip. ncpu as a *start* count oversubscribes a CPU-bound zip.

### Receipt (2026-09-07)

`./compare.sh` on this machine (Darwin arm64, 10 CPUs). Correctness
PASS (staticd + nginx + darkhttpd). Directional wrk: 1s, 3-round median
(round 0 discarded), isolate, `--workers 1`, page-cached fixtures. 0
errors except nginx `10mb.bin` @ c=100 (empty — Darwin `sendfile` wedge).

**4kb.html** — staticd every cell

| c | staticd rps / p50 / RSS | nginx | darkhttpd |
|---|-------------------------|-------|-----------|
| 1 | **55.7k** / 0.017 ms / 2.4 MB | 35.0k / 0.028 / 12 MB | 28.9k / 0.034 / 1.8 MB |
| 10 | **171k** / 0.046 / 2.8 MB | 68.1k / 0.134 / 12 MB | 57.8k / 0.150 / 1.8 MB |
| 100 | **169k** / 0.569 / 4.4 MB | 73.1k / 1.34 / 12 MB | 57.6k / 1.66 / 1.8 MB |

**1mb.bin** — staticd at c=1 and c=10 rps; nginx p50 and c=100 rps

| c | staticd rps / p50 / RSS | nginx | darkhttpd |
|---|-------------------------|-------|-----------|
| 1 | **7.2k** / 0.135 ms / 3.6 MB | 6.8k / 0.142 / 12 MB | 5.7k / 0.169 / 1.8 MB |
| 10 | **11.3k** / 0.843 / 3.8 MB | 9.3k / 0.707 / 12 MB | 5.8k / 1.59 / 1.8 MB |
| 100 | 10.5k / 9.50 / 5.3 MB | **11.7k** / 4.67 / 13 MB | 5.1k / 18.1 / 1.8 MB |

**10mb.bin** — nginx quicker per request; staticd more rps at c=100

| c | staticd rps / p50 / RSS | nginx | darkhttpd |
|---|-------------------------|-------|-----------|
| 1 | 681 / 1.36 ms / 14 MB | **808** / 1.21 / 12 MB | 556 / 1.73 / 1.7 MB |
| 10 | 1.04k / 9.60 / 14 MB | **1.30k** / 6.36 / 12 MB | 618 / 15.9 / 1.8 MB |
| 100 | **1.00k** / 92.5 / 16 MB | 0 (wedge) | 545 / 159 / 1.8 MB |

Body send is a 64KB cursor (`try_write`, `.wait_out` on short/BUSY).
File body is the named-block ring; page and listing bodies are the same
cursor over a pool slab (`session_queue_mem`). Close-after-body is
`send_close` on the cursor — never “headers flushed, drop the body”.

TLS still handshakes (and some writes) synchronously in the worker today;
that punctures the row/poll model for a slow client. Stepping the
handshake from readiness is the intended fit, not done here.

## Shape

```
main → open cfg → srv.listen / load_tls → serve
  srv.serve(stop, cfg, (s, enc) => [cfg] { session_app })
  worker × 2..cap               // grow every 64 conns; cap ncpu/2
    wait.poll → step rows → accept → reap
    session_step: fill | send chunk
    session_app: handle_http | WS frame
    handle_http: pages arm (MISS→static) | file | upgrade
```

Keep-alive and WebSocket are the row staying in the table, not a dest
that stays live. Add workers to use more cores; they share the listen fd.
The table is `Vec` of `Session*` on a worker arena; slots are a pool on
that arena. `poll()` is a tape, not a second table. Session embeds
`CCIoSess`; TLS wraps once at `bind_conn`. `SessAct` (`wait` / `close`)
is how a step finishes; `dead` is only the reap mark. Handshake drops
increment `srv.tls_fail` (shutdown summary). `CCServer.cch` is a local
face (not std) — dest zip + socket session (`CCIoSess`). `CCServer.ccs`
owns the header and is `serve`. The page is HTTP/WS + `main`.

Encode methods never touch the socket. The jail walks each path
component with `openat(O_NOFOLLOW)`. The fd cache re-resolves the name
at most once a second (absolute, not idle-sliding); a hold dups the fd
so a `rename` swap can close the slot while in-flight holds finish. The
ring reuses the same `(dev, ino, block)` slot; idle `refs==0` slots
become holes once a second.

## Architecture

| Name | What it is |
|------|------------|
| **Worker dest** | `srv.serve` plants dests (start 2, or 1 if cap is 1) and grows with live. Each worker owns the poll tape and live table. One app closure is borrow-invoked per ready window. |
| **Socket session** | `CCIoSess` on the server: sock / TLS / window / dead. Tape sees `io` only. |
| **HTTP/WS row** | `Session*` in the table. Embeds `CCIoSess`, send cursor (file hold or mem slab / off / left / `send_close`). |
| **Send cursor** | One chunk per `session_step`. `POLLOUT` while `send_left`. Disposition is `send_close` after the body drains — headers alone never close. |

Ring and fd cache share one `CCExclusive` (`cli_a.create_exclusive(4)`),
names `SYNC_BLOCK` and `SYNC_FC`. Hold is metadata only: drop the lock
before `pread` / `openat`. When a pathname refresh swaps the cached
inode, order is file cache then ring (`block_cull_id`). A contended
exclusive parks the fiber, not the OS worker. Date / Last-Modified
format onto the request arena.

Ring identity is `(dev, ino, block)` plus `mtime`/`mtime_nsec`/`len` from
`fstat` after `openat`. FNV-1a of the triple is the probe start only — a
path hash is not a file. Hardlinks share a slab. Same-second in-place
rewrites invalidate when the platform exposes nanosecond mtime. `ino == 0`
or an unaligned Range goes to the miss pool on `BlockCache`. A same-key
GET while `ready == 0 && refs != 0` pools too (no half-fill, no condvar).
A warm other key is not stolen; pressure pools. The miss pool is
`cc_arena_pool_stack` at the top of `main` (an 8 MB VLA SIGSEGVs Darwin's
default stack), owned by `g_blocks`.

## Layout

| Path | Purpose |
|------|---------|
| `staticd.ccs` | HTTP / WS / encode / `main` |
| `CCServer.cch` | Dest zip + `CCIoSess` + row types — not std |
| `CCServer.ccs` | Owner: `srv.serve` — workers / poll / grow, borrow-invoke |
| `staticd_ws.cch` | SHA-1 / base64 / WS frame tape |
| `staticd_http.cch` | Date / Range / header-CI tape |
| `staticd_block.cch` | `BlockCache` named-block ring (`checkout_block` / `block_cache_fill`) |
| `staticd_fs.cch` | Jail, `FileHold`, 1s fd cache, listing |
| `gen_fixtures.sh` | Fixture tree + manifest |
| `correctness.sh` | Golden gate |
| `bench_latency.sh` | Latency matrix |
| `compare.sh` | correctness + bench (`--smoke` / `--full`) |
| `nginx.conf.in` / `Caddyfile.in` | Peer configs (`FIXTURES_ROOT` substituted) |
| `setup.sh` | Fetch darkhttpd; brew nginx/wrk/hey |
| `bench_string_tpl.ccs` | `@string` vs `snprintf`; `make string_tpl` |
