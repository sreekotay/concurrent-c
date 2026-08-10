# Interop baselines

Point-in-time `RESULT` snapshots for the JS/Python boundary benches. Diff
`RESULT` lines across dates; judge a change by **ratio drift** first
(absolute ns/call moves with host load). A snapshot that exists exited
zero — marshalling cross-checks passed.

Compiler suite metrics live separately in
[`../compiler_baseline.txt`](../compiler_baseline.txt) (see
[`../README.md`](../README.md#compiler-perf-baseline)).

## Latest snapshots (2026-08-10; js_py_modules: 2026-08-09)

| Surface | Latest | How to refresh |
|---------|--------|----------------|
| CC embeds Python | [`py_baseline_20260810.txt`](py_baseline_20260810.txt) | `./cc/bin/ccc run perf/py_baseline.ccs` (see below) |
| Node → CC module | [`js_baseline_node_20260810.txt`](js_baseline_node_20260810.txt) | `ccc build perf/js_baseline.ccs && node perf/js_baseline.js` |
| Native modules (Node + Python hot path) | [`js_py_modules_20260809.txt`](js_py_modules_20260809.txt) | see [`docs/js-py-modules.md`](../../docs/js-py-modules.md) |
| Node → numpy via `concurrent-c-python` | [`js_numpy_bridge_node_20260810.txt`](js_numpy_bridge_node_20260810.txt) | `node npm/cc-python/examples/js_numpy_bridge.js` |
| Same, async lane | [`js_numpy_bridge_async_node_20260810.txt`](js_numpy_bridge_async_node_20260810.txt) | `node npm/cc-python/examples/js_numpy_bridge_async.js` |
| Isolated domains × numpy | [`js_multiprocess_numpy_node_20260810.txt`](js_multiprocess_numpy_node_20260810.txt) | `node npm/cc-python/examples/js_multiprocess_numpy.js` |
| Isolated kill+respawn (cancel-via-kill) | *(capture locally)* | `node npm/cc-python/examples/js_isolated_cancel_churn.js` |
| Python → Node wire (`concurrent-c-node`) | [`cc_node_bridge_py_20260810.txt`](cc_node_bridge_py_20260810.txt) | `python -m cc_node.examples.bench_wire` |
| Node → CC → numpy compose | [`js_numpy_node_20260808.txt`](js_numpy_node_20260808.txt) | `ccc build perf/js_numpy.ccs && node perf/js_numpy.js` |

### Adversarial storms ([`stress/bridge/`](../../stress/bridge/) — not examples)

| Storm | Command |
|-------|---------|
| Both bridges | `./stress/bridge/run.sh` (`CHAOS_SCALE=full` for bigger N) |
| Node→Python kitchen-sink | `OPENBLAS_NUM_THREADS=1 node --expose-gc stress/bridge/js_python_chaos.js` |
| Python→Node multi-child | `PYTHONPATH=pypi/cc-node python3 stress/bridge/cc_node_stress_wire.py` |

Latency demos stay under `npm/cc-python/examples/` and
`pypi/cc-node/cc_node/examples/`. Redirect a clean storm run into
`perf/baselines/<name>_$(date +%Y%m%d).txt` when you want a receipt.

Older dated files in this directory are history — keep them.

## Capture recipes

**Python embed** (`perf/py_baseline.ccs`):

```bash
./cc/bin/ccc run perf/py_baseline.ccs > /tmp/pb.txt \
  && { echo "# perf/py_baseline.ccs snapshot"; \
       echo "# date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"; \
       echo "# host: $(uname -srm)"; \
       echo "# cc:   $(git rev-parse --short HEAD)"; \
       echo "#"; cat /tmp/pb.txt; } \
       > perf/baselines/py_baseline_$(date +%Y%m%d).txt
```

**Node → CC module** (`perf/js_baseline.ccs` + driver):

```bash
./cc/bin/ccc build perf/js_baseline.ccs
node perf/js_baseline.js > perf/baselines/js_baseline_node_$(date +%Y%m%d).txt
```

## Reading them

- `RESULT` lines are the machine surface — grep them, diff them across
  snapshots. Everything else is for people.
- The **ratios are the stable part.** Absolute ns/call moves with host load
  (this collection includes container-hosted runs); a mode measured against
  its native control on the same run mostly cancels that out.
- The outbound scalar modes (`cc_to_py_*` / `cc_to_js_*`) are the noisiest —
  treat a <15% move there as weather.
