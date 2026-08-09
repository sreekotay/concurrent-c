# concurrent-c-node

JavaScript — and every npm package — from Python.

Part of [Concurrent-C](https://github.com/sreekotay/concurrent-c) — a
strict C11-superset preprocessor: `.ccs` lowers to plain C and compiles
with your host C compiler.  (This bridge itself is pure Python.)

```python
import cc_node

js = cc_node.create()                # an Isolation Domain: one node child
_ = js.require('lodash')             # resolved from YOUR cwd's node_modules
_.chunk([1, 2, 3, 4, 5], 2)          # [[1, 2], [3, 4], [5]]
_.sortBy([{'n': 3}, {'n': 1}], 'n')  # dicts cross as objects, and back

semver = js.require('semver')
semver.satisfies('1.2.3', '^1.0.0')  # True

js.destroy()                         # or: with cc_node.create() as js: ...
```

The bridge is **pure Python, stdlib only** — no compiled code, no
dependencies, nothing to build.  The domain **is** a spawned `node`
child (~28ms to first call), so you get real Node: full stdlib, native
addons, whatever npm installs.  Promise-based APIs look synchronous
from Python, and bulk data crosses through **shared memory** — an 8MB
array in **9ms** where the same values as a JSON list take 583ms.

```
pip install concurrent-c-node   # needs node on PATH (or point at one)
python -m cc_node.examples.use_node
python -m cc_node.examples.bench_wire
```

Import stays `import cc_node`. Examples ship in the wheel. The mirror of
[`concurrent-c-python`](https://github.com/sreekotay/concurrent-c/tree/main/npm/cc-python)
— same domain model, same materialization rules, pointed the other way:

- **Values**: plain data (finite numbers, strings, booleans, `None`,
  lists/dicts of the same) crosses by value; everything else is a live
  handle owned by the domain — attribute access is property lookup
  (methods arrive bound), calls are calls, `str()` is `String()`.
  Non-finite floats cross tagged, never silently nulled.
- **The domain rules hold**: handles never cross bridges; `stats()` is
  the handle ledger and `release()` drops one early; `destroy()` is
  idempotent, every door answers `bridge is closed` after, and the
  child dies with the bridge (and on host exit, via stdin EOF).

## Async is free

A thenable result is awaited **in the child** before the reply, so
promise-based package APIs need nothing special — no event loop on the
Python side, no `await`:

```python
fetchish = js.eval('async (x) => { return { doubled: x * 2 } }')
fetchish(21)                         # {'doubled': 42} — just a call
```

Whatever an npm package's API returns — value or promise — the call
site reads the same.

## Callbacks: Python functions as JS functions

A Python callable passed as an argument crosses as a JS function, and
may be called back any number of times — including from inside async
JS code:

```python
mapped = js.eval('(f) => [1, 2, 3].map(f)')(lambda x, *rest: x * 10)
# [10, 20, 30] — JS conventions apply: map passes (value, index, array),
# so a lambda takes *rest.  Exceptions cross both ways, messages intact.
```

Nested callbacks compose (the wire alternates strictly), and a Python
exception inside one surfaces as the JS error at the call site — and
vice versa.

## Buffers: typed arrays, shared memory

`bytes`, `array.array`, and 1-D numpy arrays cross as
`Float64Array` / `Int32Array` / `Uint8Array` / … and come back as numpy
arrays (or `array.array` without numpy):

```python
import array
total = js.eval('(a) => a.reduce((s, x) => s + x, 0)')
total(array.array('d', range(1_000_000)))   # crosses via shared memory
```

Small buffers inline; big ones spill through shared memory — one
memcpy per side, the receiver consumes the spill file, and the sender
sweeps it if the child died first.  Nothing strays, and nothing is
silently truncated: an unsupported type is an articulate error.

## Choosing the node

Same ambient-first rule as the rest of the family: the domain runs
whatever `node` your project runs.

1. `create(node='/path/to/node')` from code — per-domain.
2. `CC_NODE_BIN` in the environment.
3. `node` on `PATH`.

And *which packages* it sees is the working directory's
`node_modules` — `require` resolves exactly as node itself would there.
Run Python in your project, get your project's packages: `npm install`
next to your program is the whole setup.

## Measured

From `python -m cc_node.examples.bench_wire` (sources under
[`cc_node/examples/`](https://github.com/sreekotay/concurrent-c/blob/main/pypi/cc-node/cc_node/examples/))
on a 4-vCPU x86-64 box, node 22 / python 3.11
([`perf/baselines/cc_node_bridge_py_20260809.txt`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/cc_node_bridge_py_20260809.txt);
catalog: [`perf/baselines/README.md`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/README.md)):

| what | result |
|---|---|
| spawn a domain (node child, first eval) | 28ms |
| wire round trip (smallest call) | 116µs |
| Python-callback round trip (JS → Python → JS) | 238µs |
| 8MB `array('d')` argument, shm spill | **9.2ms** |
| the same 8MB as a JSON list | 583ms — the spill is **63x** |

The wire is strict request/response JSON over stdio with the
shared-memory spill for bulk data — the same discipline concurrent-c-python's
isolated domains speak, mirrored.  True pinned zero-copy leases remain
future work.

A worked tour (builtin Node modules, chains, callbacks, thenables,
buffers — no npm install needed):
`python -m cc_node.examples.use_node`.

Adversarial multi-child storm (fanout, callback blizzard, shm hail,
teardown derby): `python -m cc_node.examples.stress_wire`
(`CC_NODE_STRESS=full` for bigger N) — see
[`perf/README.md`](https://github.com/sreekotay/concurrent-c/blob/main/perf/README.md#js--python-interop).

And when the hot path is YOUR code rather than an npm package, skip the
wire entirely: a page of Concurrent-C (or C) exports as a native module
for Python and Node both — 40-90ns calls, stable-ABI artifacts.  See
[Native modules for Node and Python](https://github.com/sreekotay/concurrent-c/blob/main/docs/js-py-modules.md).
