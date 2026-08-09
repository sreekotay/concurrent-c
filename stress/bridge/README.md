# Package-bridge stress (Node ↔ Python)

Adversarial storms for the **process bridges** — not the CC embed /
native-module latency benches.

| Leave alone | Lives here |
|-------------|------------|
| `perf/py_baseline.ccs`, `perf/js_baseline.*`, `perf/js_numpy.*` | — |
| `npm/cc-python/examples/js_*.js` (RESULT latency demos) | — |
| `pypi/cc-node/cc_node/examples/{bench_wire,use_node}.py` | — |
| — | [`js_python_chaos.js`](js_python_chaos.js) — kitchen sink + RSS soak |
| — | [`cc_node_stress_wire.py`](cc_node_stress_wire.py) — kitchen sink + RSS soak |

CI correctness smokes stay under `tests/cc_python_bridge_*.js` /
`tests/cc_node_bridge.py` — those pin booleans; these push volume and races.

## Run

```bash
./stress/bridge/run.sh                         # quick
CHAOS_SCALE=full ./stress/bridge/run.sh        # bigger N
CHAOS_SCALE=soak ./stress/bridge/run.sh        # full sizes + multi-second RSS soaks

# individually
OPENBLAS_NUM_THREADS=1 node --expose-gc stress/bridge/js_python_chaos.js
PYTHONPATH=pypi/cc-node python3 stress/bridge/cc_node_stress_wire.py
```

`CHAOS_SCALE`: `quick` < `full` < `soak` (`soak` implies full mode sizes).

Needs `node` + `python3`. Isolated numpy modes skip cleanly when numpy is
absent. Not part of `tools/run_all.ccs --stress` (host-driven, not `.ccs`).

Receipts: redirect a clean run into `perf/baselines/` when you want a dated
snapshot (see [`perf/baselines/README.md`](../../perf/baselines/README.md)).
