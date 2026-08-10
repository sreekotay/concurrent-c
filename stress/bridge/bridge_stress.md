# Package-bridge stress catalog

Adversarial storms for the **Node ↔ Python process bridges**
(`concurrent-c-python` / `concurrent-c-node`). Latency benches stay under
`npm/cc-python/examples/`, `pypi/cc-node/cc_node/examples/`, and `perf/`.
CI correctness smokes stay under `tests/cc_*_bridge*`.

| Driver | Host | Target |
|--------|------|--------|
| [`js_python_fuzz.js`](js_python_fuzz.js) | Node | seeded in-process walk (`FUZZ_SEED`) |
| [`js_python_chaos.js`](js_python_chaos.js) | Node | `concurrent-c-python` (in-process + isolated) |
| [`cc_node_stress_wire.py`](cc_node_stress_wire.py) | Python | `concurrent-c-node` (Node children) |
| [`run.sh`](run.sh) | both | fuzz + both kitchen sinks |

## Run

```bash
./stress/bridge/run.sh                         # CHAOS_SCALE=quick (+ seeded fuzz)
CHAOS_SCALE=full ./stress/bridge/run.sh
CHAOS_SCALE=soak ./stress/bridge/run.sh        # full sizes + longer soaks
SOAK_SECONDS=30 CHAOS_SCALE=soak ./stress/bridge/run.sh
FUZZ_SEED=42 FUZZ_OPS=200 ./stress/bridge/run.sh   # replay fuzz walk

OPENBLAS_NUM_THREADS=1 node --expose-gc stress/bridge/js_python_fuzz.js
OPENBLAS_NUM_THREADS=1 node --expose-gc stress/bridge/js_python_chaos.js
PYTHONPATH=pypi/cc-node python3 stress/bridge/cc_node_stress_wire.py
./scripts/sanitize_bridge.sh fuzz              # Docker ASan: mem + fuzz
```

`CHAOS_SCALE`: `quick` < `full` < `soak` (`soak` implies full mode sizes).
`SOAK_SECONDS` overrides wall time for RSS / handle-leak soaks when
`CHAOS_SCALE=soak`. `FUZZ_SEED` / `FUZZ_OPS` control the seeded walk
(`js_python_fuzz.js`); every RESULT line includes the seed. Needs `node` +
`python3`. Isolated numpy modes skip cleanly when numpy is absent. Not
part of `tools/run_all.ccs --stress`.

## Status legend

| Tag | Meaning |
|-----|---------|
| **green** | Asserted in `./stress/bridge/run.sh` (quick); expected to pass |
| **soak** | Longer wall time / bigger N under `CHAOS_SCALE=soak` |
| **numpy** | Skips cleanly without numpy |
| **smoke** | Also pinned by a `tests/` smoke (boolean, not volume) |

---

## `js_python_fuzz.js` (seeded walk)

| Mode | Status | What it hammers |
|------|--------|-----------------|
| `fuzz_walk` | green | Random create/import/call/task/lease/keep-past/release/destroy/GC; `destroy_kick` expects create-during-destroy refuse or clean create; **RESULT lines carry `fuzz_seed=`** |

Replay: `FUZZ_SEED=<n> FUZZ_OPS=<m> node --expose-gc stress/bridge/js_python_fuzz.js`.
Default ops: quick 200 / full 800 / soak 2000 (`FUZZ_OPS` overrides).

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
| `mixed_hammer` | green, numpy | Two domains + event-loop ticks under load |
| `everything_concurrent` | green | Mixed shapes on many domains at once (`Promise.all`) |
| `escaped_closure` | green, smoke | JS closure captures proxy; destroy + GC; later invoke → closed |
| `lease_detach` | green, smoke | `ArrayBuffer.transfer` while lane holds a leased buffer |
| `sync_vs_lane_lease` | green | Sync `math.sqrt` while lane `fsum` holds a large lease |
| `promise_all_destroy` | green | `Promise.all` across N isolated domains; destroy a subset mid-flight (cooperative — see design notes) |
| `lease_blender` | green | Buffer leases + drop mid-flight |
| `rss_soak` | green, soak | Create/churn/destroy RSS bound (in-process) |
| `rss_soak_isolated` | green, soak | Same for process children |
| `handle_leak_soak` | green, soak | Long-lived domain; `stats()` + RSS must re-settle |
| `mixed_load_soak` | green, soak | Sync + lane + isolated churn; RSS bound (`SOAK_SECONDS`) |
| `kill_mid_spill` | green | `SIGKILL` child mid-SHM spill; no stray spill files |
| `shared_buf_kill_sibling` | green | Same JS `Float64Array` → two isolated spills; kill A mid-flight; B completes; no strays |
| `kill_respawn_loop` | green | Kill-to-cancel + mint replacement (correctness / RSS); latency in `js_isolated_cancel_churn.js` |
| `abort_inject` | green | `os.abort` / `SIGABRT` on isolated children (stricter than `_exit`) |

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
| `thenable_typed_array` | green, smoke | Thenables resolving to TypedArrays |
| `import_module_storm` | green, smoke | `import_module` churn |
| `cross_domain_barrage` | green | Foreign-handle / multi-domain discipline |
| `destroy_during_thenable` | green, smoke | Destroy while thenable in flight |
| `mixed_thenable_hammer` | green | Two domains + thenables together |
| `big_payload_hail` | green | Large payloads |
| `retained_callback_storm` | green | Callbacks retained across calls |
| `everything_concurrent` | green | Mixed shapes on **separate** bridges (one bridge per thread) |
| `escaped_closure` | green | Capture JS callable; destroy; invoke later → closed |
| `fanout_destroy` | green | Many children in flight; destroy a subset mid-flight (cooperative — see design notes) |
| `rss_soak` | green, soak | Create/churn/destroy RSS bound |
| `handle_leak_soak` | green, soak | Long-lived domain; ledger + RSS re-settle |
| `abort_inject` | green | `process.abort` / `SIGABRT` (stricter than exit/kill) |

## Negative / correctness smokes (not volume)

| Pack | Status |
|------|--------|
| `tests/cc_python_bridge_mem.js` | Escaped closure, lease detach mid-lane, keep-past-return, … |
| `tests/cc_python_bridge_neg.js` | Foreign handles, proxy traps, NaN/Inf, SystemExit, … |
| `tests/cc_python_bridge_*.js` | Wire pins, destroy-from-cb, keep-past-return, … |
| `tests/cc_node_bridge.py` (+ `_neg`) | Same for the Python host |

## Design notes

### Cooperative `destroy()` vs hard-cancel

Process-bridge `destroy()` / `close()` is **cooperative**: send farewell
`close`, drain in-flight wire work, then wait (with a kill fallback if
the child ignores close). In-flight calls may still **fulfill** with a
correct value; they are not required to reject. Modes that destroy a
subset mid-flight (`promise_all_destroy`, `fanout_destroy`,
`destroy_during_thenable`) therefore assert composition, not cancel:

- every promise / worker **settles** (no hang)
- fulfilled values are **correct**
- destroyed domains end **`closed`**
- surviving domains stay **usable**, then close cleanly
- no stray SHM spill files

**Hard-cancel** is a different contract — child dies without drain
(`SIGKILL`, `os.abort` / `process.abort`, `_exit`). Those modes
(`kill_mid_spill`, `shared_buf_kill_sibling`, `kill_respawn_loop`,
`abort_inject`, `child_crash_storm`, `crash_storm`) require in-flight
ops to **reject** with closed/exited and must not leak spills or leave
zombies. Do not stretch sleeps to force cooperative destroy into looking
like hard-cancel; that is tuning a race, not testing the API.

**CPU-bound cancel** has no clean form (cannot unwind BLAS). The suite
encodes Unix reality: cooperative fulfill *or* kill the worker. Latency
of kill+respawn is a perf question —
[`npm/cc-python/examples/js_isolated_cancel_churn.js`](../../npm/cc-python/examples/js_isolated_cancel_churn.js).

**Same JS buffer, two isolated domains:** each call stages its **own**
spill file (not one shared mapping). `shared_buf_kill_sibling` kills A
mid-spill while B reads a sibling spill from the same `Float64Array`.

### Other

- **Soaks** are wall-clock loops, not fixed op counts. Use `CHAOS_SCALE=soak`
  and optionally `SOAK_SECONDS` for overnight-ish local runs; quick CI stays short.
- **Everything concurrent** interleaves several storm shapes. On cc-node the
  wire is single-threaded per bridge — workers each own a domain. On
  cc-python, multi-domain `Promise.all` is the natural fan-in.
- **Unclean death** modes run last so Abort traps / broker
  `BrokenPipeError` noise does not bury later RESULTS.
- **Lease detach**: `ArrayBuffer.transfer` while a lane call holds the
  buffer must not crash or silently corrupt — correct sum (pin held) or
  an articulate error both pass.
- **Escaped closures**: a JS closure that captures a proxy must answer
  `bridge is closed` after destroy + GC — never use-after-free.

## Wire libFuzzer

[`fuzz/`](fuzz/) — standalone C codec for `$shm` / `$ta` / `$h` / `$nf`
(no Node). `./scripts/fuzz_wire_codec.sh`. Nightly:
`bridge-asan-nightly.yml`.

## Deferred (not in suite yet)

- Worker-thread policy (“one domain graph per thread”) + violation test
- Multi-GB OOM / cgroup pressure modes (manual / soak-only machines)

## Related

- Sanitizers / ASan / fuzz: [`docs/sanitizers.md`](../../docs/sanitizers.md)
- Package READMEs: [`npm/cc-python/README.md`](../../npm/cc-python/README.md),
  [`pypi/cc-node/README.md`](../../pypi/cc-node/README.md)
- Interop map: [`perf/README.md`](../../perf/README.md) (JS / Python Interop)
- Baseline receipts: [`perf/baselines/README.md`](../../perf/baselines/README.md)
