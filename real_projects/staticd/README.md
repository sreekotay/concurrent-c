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
| WebSocket | `Upgrade: websocket` → `101`, then echo (text / binary), pong for ping, close for close. No fragments; payload cap 64KB |
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
and grows to ncpu/2 as `g_live` crosses 192, 256, … — the accept storm
splits the zip. ncpu as a *start* count oversubscribes a CPU-bound zip.

## Shape

```
main → open cfg → serve(cfg)
  signal → g_stop
  worker × 2..cap               // start 2; grow every 64 conns; cap ncpu/2
    poll → step rows → accept → reap
    step: fill | send chunk | handle_http | WS frame
    handle_http: pages arm (MISS→static) | file | upgrade
```

Keep-alive and WebSocket are the row staying in the table, not a dest
that stays live. Add workers to use more cores; they share the listen fd.
The table is `Vec` of `Session*` on a worker arena; slots are a pool on
that arena. `poll()` is a tape, not a second table. TLS wraps once at
accept; `session_fill` / `session_write_all` are the one transport face.
`SessAct` (`wait` / `close`) is how a step finishes; `dead` is only the
reap mark. TLS handshake drops increment `g_tls_fail` (shutdown summary).

Encode methods never touch the socket. The jail walks each path
component with `openat(O_NOFOLLOW)`. The fd cache re-resolves the name
at most once a second (absolute, not idle-sliding); a hold dups the fd
so a `rename` swap can close the slot while in-flight holds finish. The
ring reuses the same `(dev, ino, block)` slot; idle `refs==0` slots
become holes once a second.

## Architecture

| Name | What it is |
|------|------------|
| **Worker dest** | `g_app`. Start 2 (1 if cap is 1); `maybe_grow` dest-attaches up to the cap. Owns the poll tape and the live table. |
| **Session row** | `Session*` in the table. Socket, read buf, send cursor (`FileHold` / off / left). |
| **Send cursor** | One `FILE_SEND_CHUNK` per `session_step`. `POLLOUT` while `send_left`. Close-after-body is `send_close`, not `dead` before the cursor drains. |

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
| `staticd.ccs` | Server (`worker_run` is the story) |
| `staticd_ws.cch` | SHA-1 / base64 / WS frame tape |
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
