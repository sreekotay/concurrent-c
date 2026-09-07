# staticd adversarial stress catalog

Storms for the Concurrent-C static HTTP/1.1 + WebSocket specimen
(`real_projects/staticd`). Latency benches stay under
`real_projects/staticd/bench_latency.sh`. Correctness / WS subset gates
stay under `correctness.sh` and `ws_test.py`.

| Driver | Host | Target |
|--------|------|--------|
| [`adversary.py`](adversary.py) | Python | spawned `./out/staticd` (HTTP + WS wire) |
| [`run.sh`](run.sh) | both | quick / full / soak scales |

## Run

```bash
./stress/staticd/run.sh                         # CHAOS_SCALE=quick
CHAOS_SCALE=full ./stress/staticd/run.sh
CHAOS_SCALE=soak ./stress/staticd/run.sh
STATICD_BIN=real_projects/staticd/out/staticd ./stress/staticd/run.sh
MODE=slowloris_headers ./stress/staticd/run.sh  # single mode
make -C real_projects/staticd stress
```

`CHAOS_SCALE`: `quick` < `full` < `soak`. Needs `python3` and a built
`staticd`. Not part of `tools/run_all.ccs --stress` yet.

## Status legend

| Tag | Meaning |
|-----|---------|
| **green** | Asserted in `./stress/staticd/run.sh` (quick); expected to pass |
| **break** | Expected to expose a known hole until the hardening lands |
| **soak** | Longer wall / bigger N under `CHAOS_SCALE=soak` |
| **expect** | Outcome contract (see mode notes) |

---

## Failure-mode taxonomy

What we are trying to break, mapped to the specimen's seams:

| Seam | Failure mode | Modes |
|------|--------------|-------|
| Worker / accept | EMFILE soft-fail, worker exit without respawn, accept storm | `fd_exhaust_accept`, `conn_storm` |
| Deadlines | Slowloris header drip, idle keep-alive, write stall | `slowloris_headers`, `idle_keepalive_pile`, `write_stall_tiny_sndbuf` |
| Output path | `write_all` vs try_write cursor; abort mid-headers / mid-body | `abort_mid_headers`, `abort_mid_body`, `pipelined_close` |
| TLS handshake | Sync HOL / stepped HS drip | `tls_slow_client_hello` (SKIP without certs) |
| Block fill | Cold `pread` HOL across unrelated sockets | `cold_fill_hol` |
| HTTP parse | Garbage methods, huge headers, CL mismatch, Range edges | `http_garbage_hail`, `header_bomb`, `body_cl_mismatch`, `range_edge_hail` |
| Jail / fs | Traversal, symlink races, hot rename under GET | `traversal_hail`, `symlink_escape_race`, `hot_rename_storm` |
| WebSocket | Fragments, RSV, oversize, ping flood, fanout | `ws_protocol_abuse`, `ws_ping_flood`, `ws_fanout` |
| Pages | Slow handler mutex, GET leak, throw storm | `pages_slow_hold`, `pages_handler_leak`, `pages_throw_storm` |
| Resource | RSS / fd leak under churn | `conn_churn_rss`, `ws_churn_rss` |

---

## `adversary.py` modes

### Accept / worker

| Mode | Status | What it hammers | expect |
|------|--------|-----------------|--------|
| `conn_storm` | green | N concurrent TCP connects + GET /1kb.bin | all settle; server still serves |
| `fd_exhaust_accept` | green / break | Raise open-file soft limit pressure around accept | server stays up; later GET works (break = worker died) |
| `accept_burst_survive` | green | Burst connect/close without read | server still serves afterward |

### Deadlines / Slowloris

| Mode | Status | What it hammers | expect |
|------|--------|-----------------|--------|
| `slowloris_headers` | break → green | Many conns drip `GET / HTTP/1.1\r\n` one byte / 200ms | with deadlines: reap; without: fd pile (assert bound or mark break) |
| `idle_keepalive_pile` | break → green | Complete a GET then idle with keep-alive | idle deadline closes; server still serves |
| `header_never_finishes` | break → green | Open conn, send partial headers, stall | must not pin forever past budget |

### Output / backpressure

| Mode | Status | What it hammers | expect |
|------|--------|-----------------|--------|
| `abort_mid_headers` | green | Read 1 byte of response then RST | server survives; next GET ok |
| `abort_mid_body` | green | Abort during /1mb.bin body | server survives |
| `tiny_window_get` | green | `SO_RCVBUF` tiny; slow drain of /64kb.js | full body eventually or clean close |
| `pipelined_garbage` | green | Valid GET then junk on same conn | first response ok; conn closes or 400 |

### HTTP abuse

| Mode | Status | What it hammers | expect |
|------|--------|-----------------|--------|
| `http_garbage_hail` | green | Random method/version/target bytes | no crash; 4xx or close |
| `header_bomb` | green | Oversized header block / many headers | 400 or close; no crash |
| `body_cl_mismatch` | green | POST with CL + body; GET with fake CL | Connection: close; next request safe |
| `range_edge_hail` | green | `-0`, overflow, multi-range-ish junk, empty | 416/200/400; never 206 with underflow |

### Filesystem / jail

| Mode | Status | What it hammers | expect |
|------|--------|-----------------|--------|
| `traversal_hail` | green | `../`, `%2e%2e`, `//`, `/./` variants | 403/404; never fixture bytes |
| `hot_rename_storm` | green | Atomic rename under concurrent GET | bodies match one generation; no crash |
| `listing_href_abuse` | green / break | `--list` dir with `foo?bar`, `a#b`, spaces | links resolve or are encoded (break = 404 on click) |

### WebSocket

| Mode | Status | What it hammers | expect |
|------|--------|-----------------|--------|
| `ws_protocol_abuse` | green | fragment / unmasked / oversize / bad key / no version | close or 400; no hang |
| `ws_ping_flood` | green | N pings then echo | pongs + echo; no crash |
| `ws_fanout` | green | M concurrent echo sessions | all echo; server still serves HTTP |
| `ws_churn` | green | Rapid handshake/echo/close | no crash; HTTP still works |

### Pages (optional `--pages`)

| Mode | Status | What it hammers | expect |
|------|--------|-----------------|--------|
| `pages_throw_storm` | green | Parallel /boom | all 500; survivors /hello ok |
| `pages_handler_leak` | green | GET-only then handler-only then none | isolation; no cross-page GET |
| `pages_slow_hold` | break → green | Slow page while parallel static GET | static latency stays bounded (break = HOL) |

### TLS (optional certs)

| Mode | Status | What it hammers | expect |
|------|--------|-----------------|--------|
| `tls_http_ok` | green | HTTPS GET fixture | 200 |
| `tls_slow_client_hello` | break → green | Drip ClientHello | other conns progress (break = worker stuck) |

### Soaks

| Mode | Status | What it hammers | expect |
|------|--------|-----------------|--------|
| `conn_churn_rss` | green, soak | Connect/GET/close loop for `SOAK_SECONDS` | RSS delta bound; no crash |
| `ws_churn_rss` | green, soak | WS handshake/echo/close loop | RSS delta bound |

---

## Design notes

### Break vs green

Modes tagged **break** encode a known hole from the architecture review
(sync TLS / cold fill / missing deadlines / `write_all` backpressure /
global pages mutex). Until the fix lands they may:

- assert a **soft** contract (server process still alive after the storm), and
- print `BREAK known: <seam>` when the strong contract fails

so the suite stays runnable while documenting what still hurts. After
hardening, flip the mode to **green** and assert the strong contract.

### Cooperative close vs hard RST

Client `close()` / half-close is cooperative. `SO_LINGER(0)` RST is
hard-cancel. Modes that abort mid-response use RST on purpose — the
server must reap the row and keep serving others.

### Deadlines

`slowloris_*` / `idle_*` need a wall budget shorter than the suite
timeout. If the server has no idle/header deadline yet, the mode records
**break** when fds remain past the budget rather than hanging the driver
forever (driver enforces its own deadline and RSTs leftovers).

### Not in suite yet

- Real uncached disk HOL (needs a large file + purge cache; machine-specific)
- io_uring / async-fill completion wake races (lands with async fills)
- Multi-GB OOM / cgroup pressure
- Full RFC6455 UTF-8 / extension negotiation

## Related

- Specimen: [`real_projects/staticd/README.md`](../../real_projects/staticd/README.md)
- WS subset gate: [`real_projects/staticd/ws_test.py`](../../real_projects/staticd/ws_test.py)
- Bridge stress inspiration: [`stress/bridge/bridge_stress.md`](../bridge/bridge_stress.md)
