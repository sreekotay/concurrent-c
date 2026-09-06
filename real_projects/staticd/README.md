# staticd — Concurrent-C static HTTP/1.1 server

A small HTTP/1.1 file server written as a Concurrent-C specimen. Accept,
header encode, and body flush are separate on the page the same way
[`redis_async_sketch.ccs`](../redis/redis_async_sketch.ccs) separates hold,
encode, and ship. The file you read is the server you run.

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
| Body | Named-block ring: 256 × 64KB = 16MB BSS, key `rel`+block, reuse in place, fstat ≤1s, idle cull. Pool `pread` on miss / unaligned Range. 1s idle last-8 fd cache. Dest-attach onto `g_clients`, turnstile cap 2 |

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

| Server | Port | |
|--------|------|---|
| **staticd** | 8080 | this specimen |
| **nginx** | 8081 | `sendfile on`, `tcp_nopush on`, one worker |
| **darkhttpd** | 8082 | |
| **caddy** | 8083 | `INCLUDE_CADDY=1` |

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

Receipt columns: **p50 / p75 / p90 / p99** (ms), RPS, process RSS, errors.
Fixtures are deterministic (`gen_fixtures.sh`); bodies are gitignored,
`fixtures/manifest.txt` is checked in. Each block page-caches the fixture
tree and shuffles server order. Servers stay up for the whole matrix, so
RSS is cumulative (the 16MB ring faults in as blocks are first filled).

Local receipts land under `benchmarks/` (gitignored). Table: Darwin 25.5.0
arm64, 10 CPUs, wrk `-t2 -d5s --timeout 15s`, 3 rounds, median of measured
(round 0 discarded). Zero socket errors on every cell.

| file | c | staticd rps | nginx | darkhttpd | staticd p50 | nginx p50 | darkhttpd p50 | staticd RSS | nginx RSS | darkhttpd RSS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4kb.html | 1 | 54385 | 32628 | 27854 | 0.017 | 0.030 | 0.035 | 2.5 | 8.7 | 1.7 |
| 4kb.html | 10 | 140496 | 72104 | 55282 | 0.057 | 0.131 | 0.158 | 4.7 | 8.7 | 1.7 |
| 4kb.html | 100 | 145354 | 72916 | 56274 | 0.605 | 1.31 | 1.73 | 18.7 | 8.7 | 1.7 |
| 1mb.bin | 1 | 7354 | 6845 | 5239 | 0.132 | 0.142 | 0.176 | 18.8 | 6.3 | 1.4 |
| 1mb.bin | 10 | 10441 | 14238 | 6212 | 0.93 | 0.405 | 1.59 | 18.9 | 6.4 | 1.4 |
| 1mb.bin | 100 | 9972 | 12744 | 5886 | 9.79 | 4.55 | 16.79 | 34.0 | 7.1 | 1.5 |
| 10mb.bin | 1 | 716 | 795 | 574 | 1.33 | 1.23 | 1.69 | 45.3 | 7.1 | 1.5 |
| 10mb.bin | 10 | 756 | 1013 | 544 | 12.6 | 7.90 | 17.29 | 44.2 | 7.0 | 1.5 |
| 10mb.bin | 100 | 666 | 0 | 559 | 134 | — | 174 | 51.3 | 7.2 | 1.5 |

RPS and p50 (ms) from the receipt. RSS is process RSS in MB (`rss_kb/1024`;
nginx = master + workers). nginx 10mb / c=100 completed 0 requests.

## Shape

```
accept (@parallel dest g_clients)
  └─ handle_client
       └─ loop: BufReader request
            ├─ ReqLine (query stripped) + path_is_safe
            ├─ FileHold → openat jail / 1s fd cache (fstat ≤1s)
            │    or directory: --index, else --list / 403
            ├─ @typeview Encode → status + headers into Conn.out
            ├─ conn_flush (headers only)
            └─ send_file_body → checkout_block (256 × 64KB ring)
                 miss / unaligned Range → g_send_pool pread
                 n > 64KB: @parallel(g_clients) per window, turnstile cap 2
```

Encode methods never touch the socket. The jail is `openat`. The fd cache
slides a 1s idle window and closes the fd when the last holder drops an
expired slot. The ring reuses the same name+block slot; idle `refs==0`
slots become holes once a second.

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
