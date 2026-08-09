# Package-bridge stress catalog

Adversarial storms for the **Node ↔ Python process bridges**
(`concurrent-c-python` / `concurrent-c-node`). Latency benches stay under
`npm/cc-python/examples/`, `pypi/cc-node/cc_node/examples/`, and `perf/`.
CI correctness smokes stay under `tests/cc_*_bridge*`.

| Driver | Host | Target |
|--------|------|--------|
| [`js_python_chaos.js`](js_python_chaos.js) | Node | `concurrent-c-python` (in-process + isolated) |
| [`cc_node_stress_wire.py`](cc_node_stress_wire.py) | Python | `concurrent-c-node` (Node children) |
| [`run.sh`](run.sh) | both | runs both drivers |

## Run

```bash
./stress/bridge/run.sh                         # CHAOS_SCALE=quick
CHAOS_SCALE=full ./stress/bridge/run.sh
CHAOS_SCALE=soak ./stress/bridge/run.sh        # full sizes + longer soaks
SOAK_SECONDS=30 CHAOS_SCALE=soak ./stress/bridge/run.sh

OPENBLAS_NUM_THREADS=1 node --expose-gc stress/bridge/js_python_chaos.js
PYTHONPATH=pypi/cc-node python3 stress/bridge/cc_node_stress_wire.py
```

`CHAOS_SCALE`: `quick` < `full` < `soak` (`soak` implies full mode sizes).
`SOAK_SECONDS` overrides wall time for RSS / handle-leak soaks when
`CHAOS_SCALE=soak`. Needs `node` + `python3`. Isolated numpy modes skip
cleanly when numpy is absent. Not part of `tools/run_all.ccs --stress`.

## Status legend

| Tag | Meaning |
|-----|---------|
| **green** | Asserted in `./stress/bridge/run.sh` (quick); expected to pass |
| **soak** | Longer wall time / bigger N under `CHAOS_SCALE=soak` |
| **numpy** | Skips cleanly without numpy |
| **smoke** | Also pinned by a `tests/` smoke (boolean, not volume) |

---

## `js_python_chaos.js` (Node → Python)

| Mode | Status | What it hammers |
|------|--------|-----------------|
| `crash_storm` | green, numpy | Isolated children `_exit` mid-flight; survivors + parent stay healthy |
| `domain_fanout` | green | Many isolated domains answering at once |
| `shm_hail` | green, numpy | Concurrent large TypedArray / spill traffic |
| `teardown_derby` | green | Destroy races vs live calls |
| `callback_blizzard` | green | Nested Python→JS callbacks |
| `exception_hail` | green | Sync + async exception paths |
| `nested_callable` | green | Callables returned across the wire |
| `big_payload_hail` | green | Large inline / spill payloads |
| `isolated_handle_boomerang` | green | Handle return / nest through isolated broker |
| `isolated_pipeline_cbs` | green | Pipelined ops parked across `cbr` |
| `isolated_destroy_from_cb` | green, smoke | Destroy from inside a callback |
| `callback_buffer_path` | green | Buffer / TypedArray through callbacks |
| `parking_shm` | green | Parked replies × shm ownership |
| `keep_past_return` | green, smoke | Retain callables past return (lane) |
| `asyncio_lane_storm` | green | Asyncio lane concurrency |
| `release_during_suspend` | green | Release / GC during suspension |
| `destroy_during_thenable` | green, smoke | Destroy while a slow thenable is in flight |
| `abort_inject` | green | `os.abort` / `SIGABRT` on isolated children (stricter than `_exit`) |
| `mixed_hammer` | green, numpy | Two domains + event-loop ticks under load |
| `everything_concurrent` | green | Mixed shapes on many domains at once (`Promise.all`) |
| `lease_blender` | green | Buffer leases + drop mid-flight |
| `rss_soak` | green, soak | Create/churn/destroy RSS bound (in-process) |
| `rss_soak_isolated` | green, soak | Same for process children |
| `handle_leak_soak` | green, soak | Long-lived domain; `stats()` + RSS must re-settle |

## `cc_node_stress_wire.py` (Python → Node)

| Mode | Status | What it hammers |
|------|--------|-----------------|
| `multi_child_fanout` | green | Many Node children in parallel |
| `callback_blizzard` | green | Nested JS→Python callbacks |
| `shm_hail` | green | Large buffer spill / hail |
| `teardown_derby` | green | Destroy races |
| `eval_storm` | green | Rapid `eval` + call |
| `handle_boomerang` | green, smoke | Return JS handles into Python |
| `exception_hail` | green | Sync + thenable rejects |
| `thenable_storm` | green | Awaited thenables |
| `thenable_reject_storm` | green, smoke | Rejecting thenables |
| `destroy_from_callback` | green, smoke | Destroy from inside a callback |
| `ledger_churn` | green | `stats()` / release ledger |
| `callback_buffer_path` | green | Buffers through callbacks |
| `child_crash_storm` | green | `process.exit` / `SIGKILL` mid-call / mid-`cbr` |
| `abort_inject` | green | `process.abort` / `SIGABRT` (stricter than exit/kill) |
| `thenable_typed_array` | green, smoke | Thenables resolving to TypedArrays |
| `import_module_storm` | green, smoke | `import_module` churn |
| `cross_domain_barrage` | green | Foreign-handle / multi-domain discipline |
| `destroy_during_thenable` | green, smoke | Destroy while thenable in flight |
| `mixed_thenable_hammer` | green | Two domains + thenables together |
| `big_payload_hail` | green | Large payloads |
| `retained_callback_storm` | green | Callbacks retained across calls |
| `everything_concurrent` | green | Mixed shapes on **separate** bridges (one bridge per thread) |
| `rss_soak` | green, soak | Create/churn/destroy RSS bound |
| `handle_leak_soak` | green, soak | Long-lived domain; ledger + RSS re-settle |

## Negative / correctness smokes (not volume)

| Pack | Status |
|------|--------|
| `tests/cc_python_bridge_*.js` (+ `_neg`) | CI smokes — wire pins, destroy-from-cb, keep-past-return, reserved `$` keys, foreign handles, … |
| `tests/cc_node_bridge.py` (+ `_neg`) | Same for the Python host |

## Design notes

- **Soaks** are wall-clock loops, not fixed op counts. Use `CHAOS_SCALE=soak`
  and optionally `SOAK_SECONDS` for overnight-ish local runs; quick CI stays short.
- **Everything concurrent** interleaves several storm shapes. On cc-node the
  wire is single-threaded per bridge — workers each own a domain. On
  cc-python, multi-domain `Promise.all` is the natural fan-in.
- **Abort inject** is stricter than `_exit` / `process.exit`: native abort
  can dump core and must still reject in-flight ops with no hang / no
  zombie children. Expect noisy abort traps, macOS crash reports, and
  occasional broker `BrokenPipeError` on stderr — that is the point.
  Both drivers run this mode **last** so the noise does not bury later
  RESULTS.

## Related

- Package READMEs: [`npm/cc-python/README.md`](../../npm/cc-python/README.md),
  [`pypi/cc-node/README.md`](../../pypi/cc-node/README.md)
- Interop map: [`perf/README.md`](../../perf/README.md) (JS / Python Interop)
- Baseline receipts: [`perf/baselines/README.md`](../../perf/baselines/README.md)
