# Package-bridge stress (Node ↔ Python)

Adversarial storms for the **process bridges** — not the CC embed /
native-module latency benches.

**Catalog (modes, status, contracts):** [`bridge_stress.md`](bridge_stress.md)

`destroy()` on these bridges is **cooperative** (close + drain): in-flight
work may still fulfill. Hard-cancel (`SIGKILL` / abort) is a separate
set of modes that must reject — including same-buffer dual-spill sibling
kill and kill+respawn loops. CPU-bound cancel has no clean form; latency
of kill+respawn is
[`npm/cc-python/examples/js_isolated_cancel_churn.js`](../../npm/cc-python/examples/js_isolated_cancel_churn.js).
Details in the catalog design notes.

| Leave alone | Lives here |
|-------------|------------|
| `perf/py_baseline.ccs`, `perf/js_baseline.*`, `perf/js_numpy.*` | — |
| `npm/cc-python/examples/js_*.js` (RESULT latency demos) | — |
| `pypi/cc-node/cc_node/examples/{bench_wire,use_node}.py` | — |
| — | [`js_python_fuzz.js`](js_python_fuzz.js) — seeded random walk (`FUZZ_SEED`) |
| — | [`js_python_chaos.js`](js_python_chaos.js) — kitchen sink + soaks |
| — | [`cc_node_stress_wire.py`](cc_node_stress_wire.py) — kitchen sink + soaks |

CI correctness smokes stay under `tests/cc_python_bridge_*.js` /
`tests/cc_node_bridge.py` — those pin booleans (escaped closure, lease
detach, SystemExit, …); these push volume and races.

## Run

```bash
./stress/bridge/run.sh                         # quick
CHAOS_SCALE=full ./stress/bridge/run.sh        # bigger N
CHAOS_SCALE=soak ./stress/bridge/run.sh        # full sizes + multi-second soaks
SOAK_SECONDS=30 CHAOS_SCALE=soak ./stress/bridge/run.sh

# individually
FUZZ_SEED=42 node --expose-gc stress/bridge/js_python_fuzz.js
OPENBLAS_NUM_THREADS=1 node --expose-gc stress/bridge/js_python_chaos.js
PYTHONPATH=pypi/cc-node python3 stress/bridge/cc_node_stress_wire.py
./scripts/sanitize_bridge.sh fuzz    # Docker ASan: mem + fuzz walk
```

`CHAOS_SCALE`: `quick` < `full` < `soak` (`soak` implies full mode sizes).
`FUZZ_SEED` / `FUZZ_OPS` replay the seeded walk (RESULT lines include the seed).
Fuzz monitoring: stderr `[+Ns] alive …` every `FUZZ_HEARTBEAT_SECS` (default 5),
`PROGRESS` every `FUZZ_PROGRESS_OPS`, hard exit 124 at `FUZZ_TIMEOUT`
(default 60s quick).

Needs `node` + `python3`. Isolated numpy modes skip cleanly when numpy is
absent. Not part of `tools/run_all.ccs --stress` (host-driven, not `.ccs`).

Receipts: redirect a clean run into `perf/baselines/` when you want a dated
snapshot (see [`perf/baselines/README.md`](../../perf/baselines/README.md)).

Sanitizers / Docker ASan / seeded fuzz / wire libFuzzer:
[`docs/sanitizers.md`](../../docs/sanitizers.md).
