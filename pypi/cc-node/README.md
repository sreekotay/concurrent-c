# cc-node

JavaScript — and every npm package — from Python.

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

**Async is free**: a promise-returning package API looks synchronous
from Python — the thenable is awaited in the child before the reply.
And bulk data crosses through **shared memory**: an 8MB array in
**9ms** where the same values as a JSON list take 583ms.

```
pip install cc-node       # pure Python, stdlib only; needs node on PATH
```

The mirror of [`cc-python`](https://github.com/sreekotay/concurrent-c/tree/main/npm/cc-python)
— same domain model, same materialization rules, pointed the other way.
No engine embedding: the domain **is** a spawned `node` child (found on
`PATH`, or `CC_NODE_BIN`, or `create(node=...)`), so you get real Node
— full stdlib, native addons, whatever npm installs — and process
isolation for free.  `npm install` next to your Python program;
`require` resolves from your cwd.

- **Values**: plain data (finite numbers, strings, booleans, `None`,
  lists/dicts of the same) crosses by value; everything else is a live
  handle owned by the domain — attribute access is property lookup
  (methods arrive bound), calls are calls, `str()` is `String()`.
  Non-finite floats cross tagged, never silently nulled.
- **Typed buffers cross as typed arrays**: `bytes`, `array.array`, and
  1-D numpy arrays become `Float64Array`/`Int32Array`/`Uint8Array`/…
  and come back as numpy arrays (or `array.array` without numpy).
  Small buffers inline; big ones spill through shared memory — one
  memcpy per side, receiver consumes the spill file, sender sweeps it
  if the child died first.  Nothing strays.
- **Callbacks**: a Python callable crosses as a JS function.  JS
  calling conventions apply (`Array.map` calls with value, index,
  array — take `*rest`).  Exceptions map both ways, messages intact.
- **The domain rules hold**: handles never cross bridges; `stats()` is
  the handle ledger and `release()` drops one early; `destroy()` is
  idempotent, every door answers `bridge is closed` after, and the
  child dies with the bridge (and on host exit, via stdin EOF).

The wire is strict request/response JSON over stdio with the
shared-memory spill for bulk data — the same discipline cc-python's
isolated domains speak, mirrored.  True pinned zero-copy leases remain
future work.

## Measured

From [`examples/bench_wire.py`](https://github.com/sreekotay/concurrent-c/blob/main/pypi/cc-node/examples/bench_wire.py)
on a 4-vCPU x86-64 box, node 22 / python 3.11 (baselines under
`perf/baselines/` in the repo):

| what | result |
|---|---|
| spawn a domain (node child, first eval) | 28ms |
| wire round trip (smallest call) | 116µs |
| Python-callback round trip (JS → Python → JS) | 238µs |
| 8MB `array('d')` argument, shm spill | **9.2ms** |
| the same 8MB as a JSON list | 583ms — the spill is **63x** |

A worked tour (builtin Node modules, chains, callbacks, thenables,
buffers — no npm install needed):
[`examples/use_node.py`](https://github.com/sreekotay/concurrent-c/blob/main/pypi/cc-node/examples/use_node.py).

And when the hot path is YOUR code rather than an npm package, skip the
wire entirely: a page of Concurrent-C (or C) exports as a native module
for Python and Node both — 40-90ns calls, stable-ABI artifacts.  See
[Native modules for Node and Python](https://github.com/sreekotay/concurrent-c/blob/main/docs/js-py-modules.md).
