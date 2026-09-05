# staticd — Concurrent-C static HTTP/1.1 server

A small HTTP/1.1 file server written as a Concurrent-C specimen. Accept,
header encode, and body flush are separate on the page the same way
[`redis_async_sketch.ccs`](../redis/redis_async_sketch.ccs) separates hold,
encode, and ship. nginx is the aspirational bar; darkhttpd is the honest
tiny-C peer.

Tutorial = idiomatic = production: the file you read is the server you run.

## Features

| | |
|---|---|
| Methods | `GET`, `HEAD`, `OPTIONS` (204 + `Allow`) |
| HTTP/1.1 | keep-alive (HTTP/1.1 default; `Connection: close` / HTTP/1.0 close) |
| Time | `Date`, `Last-Modified`, `If-Modified-Since` → 304 |
| Range | one `Range: bytes=` → 206 / 416; `If-Range` (date match → 206, else 200) |
| MIME | extension → `static_map` → wire string (`text/html`, `application/javascript`, …) |
| Jail | `openat` + `O_NOFOLLOW` on a docroot fd. `.` / `..` / `//` / `/./` → 403. No rewrite, no chroot |
| Index | `--index NAME` (default `index.html`) for `/` and directory URLs |
| Listing | `--list` (off). Directory with no index → HTML table; without `--list` → 403 |
| Query | `?…` is recognized and ignored |
| Extra headers | `--header 'Name: value'` (repeatable; no CR/LF) |
| Body | mmap after header flush; 1s idle last-8 cache (unmap on last expired drop) |

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
                            # --index, --list, dir-without-list → 403
```

Missing nginx / darkhttpd / caddy are skipped. Traversal may be 400, 403, or
404; staticd is 403.

## Bench

Latency-first. Peers (missing ones are skipped):

| Server | Port | Role |
|--------|------|------|
| **staticd** | 8080 | this specimen |
| **nginx** | 8081 | production bar (`sendfile`) |
| **darkhttpd** | 8082 | tiny-C peer |
| **caddy** | 8083 | optional (`INCLUDE_CADDY=1`) |

```bash
./compare.sh --smoke        # correctness + 2s wrk, 4kb.html @ c=10
./bench_latency.sh          # directional: 1s × 3 rounds, 4kb / 1mb / 10mb, c=1/10/100
FULL=1 ./bench_latency.sh   # receipt: 30s × 5, five files
SMOKE=1 ./bench_latency.sh  # 2s, 4kb.html @ c=10 only
./compare.sh                # correctness + directional
./compare.sh --full         # correctness + receipt
```

Knobs: `REPEATS`, `DURATION`, `CONCURRENCY`, `FILES`, `FULL`, `SMOKE`,
`INCLUDE_NGINX`, `INCLUDE_DARKHTTPD`, `INCLUDE_CADDY`, `BENCH_OUT`.

Receipt columns match `wrk --latency`: **p50 / p75 / p90 / p99** (ms), RPS,
errors. Fixtures are deterministic (`gen_fixtures.sh`); bodies are gitignored,
`fixtures/manifest.txt` is checked in. Each block page-caches the fixture
tree and shuffles server order.

staticd is not expected to beat nginx on p99 at high concurrency. Chase
darkhttpd first; nginx is the bar. A dated receipt lives in
[`benchmarks/`](benchmarks/) — treat older files as historical (they predate
Date/Range and the 1s map cache).

## Shape

```
accept (@parallel dest)
  └─ handle_client
       └─ loop: BufReader request
            ├─ ReqLine (query stripped) + path_is_safe
            ├─ FileHold → openat jail / 1s map cache
            │    or directory: --index, else --list / 403
            ├─ @typeview Encode → status + headers into Conn.out
            ├─ conn_flush (headers only)
            └─ write_all(mmap body)   # listings are a small @string
```

Encode methods never touch the socket. File bytes stay mapped until after
header flush. The jail is still `map_at`. The cache slides a 1s idle window
on each hit and unmaps when the last holder drops an expired slot (or on
the next hold's idle sweep). A busy request is never unmapped underfoot.

## Layout

| Path | Purpose |
|------|---------|
| `staticd.ccs` | Server (`handle_one` is the story) |
| `staticd_http.cch` | Date / Range / header-CI tape |
| `staticd_fs.cch` | Jail, `FileHold`, 1s map cache, listing |
| `gen_fixtures.sh` | Fixture tree + manifest |
| `correctness.sh` | Golden gate |
| `bench_latency.sh` | Latency matrix |
| `compare.sh` | correctness + bench (`--smoke` / `--full`) |
| `nginx.conf.in` / `Caddyfile.in` | Peer configs (`FIXTURES_ROOT` substituted) |
| `setup.sh` | Fetch darkhttpd; brew nginx/wrk/hey |
| `bench_string_tpl.ccs` | `@string` vs `snprintf`; `make string_tpl` |
