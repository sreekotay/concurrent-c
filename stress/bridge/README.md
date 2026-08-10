# Package-bridge stress (Node ↔ Python)

Adversarial storms for the **process bridges** — not the CC embed /
native-module latency benches.

**Catalog (modes, status, contracts):** [`bridge_stress.md`](bridge_stress.md)

`destroy()` on these bridges is **cooperative** (close + drain): in-flight
work may still fulfill. Hard-cancel (`SIGKILL` / abort) is a separate
set of modes that must reject. Details in the catalog design notes.

| Leave alone | Lives here |
|-------------|------------|
| `perf/py_baseline.ccs`, `perf/js_baseline.*`, `perf/js_numpy.*` | — |
| `npm/cc-python/examples/js_*.js` (RESULT latency demos) | — |
| `pypi/cc-node/cc_node/examples/{bench_wire,use_node}.py` | — |
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
OPENBLAS_NUM_THREADS=1 node --expose-gc stress/bridge/js_python_chaos.js
PYTHONPATH=pypi/cc-node python3 stress/bridge/cc_node_stress_wire.py
```

`CHAOS_SCALE`: `quick` < `full` < `soak` (`soak` implies full mode sizes).

Needs `node` + `python3`. Isolated numpy modes skip cleanly when numpy is
absent. Not part of `tools/run_all.ccs --stress` (host-driven, not `.ccs`).

Receipts: redirect a clean run into `perf/baselines/` when you want a dated
snapshot (see [`perf/baselines/README.md`](../../perf/baselines/README.md)).
