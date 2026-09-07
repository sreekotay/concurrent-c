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
| Waiter / compact | kqueue/epoll unwatch after row shift; ready flags after swap-remove | `waiter_compact_live`, `waiter_reap_under_load` |
| Deadlines | Slowloris header drip, idle keep-alive | `slowloris_headers`, `idle_keepalive_pile` |
| Output path | abort mid-headers / mid-body | `abort_mid_headers`, `abort_mid_body` |
| HTTP parse | Garbage methods, huge headers, CL mismatch, Range edges | `http_garbage_hail`, `header_bomb`, `body_cl_mismatch`, `range_edge_hail` |
| Jail / fs | Traversal, hot rename under GET, listing href encode | `traversal_hail`, `hot_rename_storm`, `listing_href_abuse` |
| WebSocket | Fragments, RSV, oversize, ping flood, fanout | `ws_protocol_abuse`, `ws_ping_flood`, `ws_fanout`, `ws_churn` |
| Pages | Throw storm | `pages_throw_storm` |
| Resource | RSS / fd leak under churn | `conn_churn_rss`, `ws_churn_rss` |
| URI | Strict path decode refuse | `uri_decode_refuse` |

Planned (named in reviews, **not** in `MODES` yet): `tls_slow_client_hello`,
`tls_http_ok`, `cold_fill_hol`, `pages_slow_hold`, `pages_handler_leak`,
`write_stall_tiny_sndbuf`, `pipelined_close`, `symlink_escape_race`.
`cold_fill_hol` is the highest-value gap while `checkout_block` still blocks
on `pread`.

---

## `adversary.py` modes

### Accept / worker / waiter

| Mode | Status | What it hammers | expect |
|------|--------|-----------------|--------|
| `waiter_compact_live` | green | `--workers 1`; A,B,C keep-alive; RST A; second GET on B and C | both complete promptly. `run.sh` also rebuilds with `-DCC_SERVER_WAIT_POLL=1` as control |
| `waiter_reap_under_load` | green | N idle KA expire while a hot tail conn is under continuous GET | hot keeps answering through/after expiry (ready_ix remapped on swap) |
| `conn_storm` | green | N concurrent TCP connects + GET /1kb.bin | all settle; server still serves |
| `fd_exhaust_accept` | green | `--spawn` with lowered server `RLIMIT_NOFILE`; hold keep-alive GETs until accept stalls; spare soft-fail + backoff | server stays up; later GET works. SKIP without spawn. Receipt is behavioral (serve after release); does not yet assert `accept_soft` delta |
| `accept_burst_survive` | green | Burst connect/close without read | server still serves afterward |

### Deadlines / Slowloris

| Mode | Status | What it hammers | expect |
|------|--------|-----------------|--------|
| `slowloris_headers` | green | Many conns drip `GET / HTTP/1.1\r\n` one byte / 200ms | reap by header budget; server still serves |
| `idle_keepalive_pile` | green | Complete a GET then idle with keep-alive | idle deadline closes; server still serves |
| `header_never_finishes` | green | Open conn, send partial headers, stall | must not pin forever past budget |

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
| `listing_href_abuse` | green | `--list` dir with `foo?bar`, `a#b`, spaces | links resolve / encoded |

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

### URI

| Mode | Status | What it hammers | expect |
|------|--------|-----------------|--------|
| `uri_decode_refuse` | green | `%00`, bad `%`, decoded `/` `\` | 400 |

### Soaks

| Mode | Status | What it hammers | expect |
|------|--------|-----------------|--------|
| `conn_churn_rss` | green, soak | Connect/GET/close loop for `SOAK_SECONDS` | RSS delta bound; no crash |
| `ws_churn_rss` | green, soak | WS handshake/echo/close loop | RSS delta bound |

---

## Design notes

### Break vs green

Modes tagged **break** encode a known hole. Until the fix lands they may
assert a soft contract and print `BREAK known: <seam>`. After hardening,
flip to **green**. Planned modes above are not registered — do not treat
catalog names as implemented coverage.

### Cooperative close vs hard RST

Client `close()` / half-close is cooperative. `SO_LINGER(0)` RST is
hard-cancel. Modes that abort mid-response use RST on purpose — the
server must reap the row and keep serving others. `waiter_compact_live`
depends on that reap path.

### Deadlines

`slowloris_*` / `idle_*` need a wall budget shorter than the suite
timeout. Spawned servers get short env budgets (`HEADER_*`, `KEEPALIVE`).

### Not in suite yet

- Real uncached disk HOL (`cold_fill_hol` — needs large file + purge)
- `accept_soft` counter exposed for fd_exhaust receipt
- io_uring / async-fill completion wake races
- Multi-GB OOM / cgroup pressure
- Full RFC6455 UTF-8 / extension negotiation

## Related

- Specimen: [`real_projects/staticd/README.md`](../../real_projects/staticd/README.md)
- WS subset gate: [`real_projects/staticd/ws_test.py`](../../real_projects/staticd/ws_test.py)
- Bridge stress inspiration: [`stress/bridge/bridge_stress.md`](../bridge/bridge_stress.md)
