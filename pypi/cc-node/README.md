# concurrent-c-node

JavaScript — and every npm package — from Python.

Part of [Concurrent-C](https://github.com/sreekotay/concurrent-c) — a
strict C11-superset preprocessor: `.ccs` lowers to plain C and compiles
with your host C compiler.  (This bridge itself is pure Python.)

Map of the three boundaries (CC hosts Python, native modules, this
package bridge):
[JS / Python interop](https://github.com/sreekotay/concurrent-c/blob/main/docs/js-py-modules.md).

```python
import cc_node

js = cc_node.create()                # always a SEPARATE node process
_ = js.require('lodash')             # resolved from YOUR cwd's node_modules
_.chunk([1, 2, 3, 4, 5], 2)          # [[1, 2], [3, 4], [5]]
_.sortBy([{'n': 3}, {'n': 1}], 'n')  # dicts cross as objects, and back

semver = js.require('semver')
semver.satisfies('1.2.3', '^1.0.0')  # True

js.destroy()                         # or: with cc_node.create() as js: ...
```

## Separate process by design

Unlike [`concurrent-c-python`](https://github.com/sreekotay/concurrent-c/tree/main/npm/cc-python)
(whose **default** embeds libpython in the Node process, with
`{ isolated: true }` as the child-process opt-in), **every**
`cc_node.create()` is already the isolated tier: one spawned `node`
child per domain.  There is no in-process Node embed from Python —
you get real Node (full stdlib, native addons, whatever `npm install`
put next to your program), crash isolation, and a wire you can measure.

N domains are N OS processes: **full multi-core speedup** — fan work
across `create()` handles and they run on separate cores, no shared
event-loop or GIL between them.

| | this package — separate `node` process | Concurrent-C hosted (not this wheel) |
|---|---|---|
| API | `cc_node.create()` from Python | `cc_js_new(false, &a)` in a `.ccs` program |
| Where JS runs | own `node` child | libnode in the CC process |
| Hot call | **~105µs** wire RTT | sub-µs (needs `libnode-dev`) |
| Bulk buffers | shm spill — 8MB in **9.5ms** (52× a JSON list) | in-process |
| Parallelism | **N children = N cores — full multi-core speedup** | one process (V8's rule) |
| Crash | child dies → error; Python parent lives | shared fate with the host |

```
pip install concurrent-c-node   # needs node on PATH (or point at one)
python -m cc_node.examples.use_node
python -m cc_node.examples.bench_wire
python -m cc_node.benchmarks.multi_domain   # N children, thread-fanned
```

## Measured (separate-process wire)

From `python -m cc_node.examples.bench_wire` (sources under
[`cc_node/examples/`](https://github.com/sreekotay/concurrent-c/blob/main/pypi/cc-node/cc_node/examples/))
on a 4-vCPU x86-64 box, node 22 / python 3.11
([`perf/baselines/cc_node_bridge_py_20260810.txt`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/cc_node_bridge_py_20260810.txt);
catalog: [`perf/baselines/README.md`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/README.md)):

| what | result |
|---|---|
| spawn a domain (node child, first eval) | **28ms** |
| wire round trip (smallest call) | **105µs** |
| Python-callback round trip (JS → Python → JS) | **153µs** |
| 8MB `array('d')` argument, shm spill | **9.5ms** |
| the same 8MB as a JSON list | 499ms — the spill is **52x** |

The wire is strict request/response JSON on dedicated fds — replies
pair by request id, and stdio stays yours, so `console.log` in
evaluated JS reaches the real stdout and can never collide with a
protocol reply — with the shared-memory spill for bulk data (private
0700 per-bridge directory, 0600 exclusive-create files, removed with
the bridge).  The same discipline concurrent-c-python's
`{ isolated: true }` domains speak, mirrored.

One boundary, stated plainly: the domain is **crash isolation, not a
security sandbox** — the node child inherits your environment and runs
with your OS privileges, so do not run untrusted JavaScript through
it.

The bridge is **pure Python, stdlib only** — no compiled code, no
dependencies, nothing to build.  Import stays `import cc_node`.
Examples ship in the wheel. Same domain model and materialization
rules as the npm sibling, pointed the other way:

- **Values**: plain data (finite numbers, strings, booleans, `None`,
  lists and non-empty dicts/objects of the same) crosses by value; an
  empty `{}` stays a live handle (so bags you mint in JS keep property
  access). Everything else is a live handle owned by the domain —
  attribute access is property lookup (methods arrive bound), calls are
  calls, `str()` is `String()`. Non-finite floats cross tagged, never
  silently nulled.
- **The domain rules hold**: handles never cross bridges; `stats()` is
  the handle ledger and `release()` drops one early; `destroy()` is
  idempotent, every door answers `bridge is closed` after, and the
  child dies with the bridge (and on host exit, via wire-fd EOF).
  Teardown is **cooperative** (farewell `close` + drain, then wait /
  kill-fallback): in-flight calls may still return a correct value.
  There is no clean cancel of CPU-bound JS work — wait, or kill the
  child (`SIGKILL` / `process.abort`) and create a new domain. Hard death
  must reject in-flight ops. See
  [`bridge_stress.md`](https://github.com/sreekotay/concurrent-c/blob/main/stress/bridge/bridge_stress.md).

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

## Common issues

**`Cannot find module '…'`.** `require` / `import` resolve from the
**Python process cwd** (`node_modules` next to your program), not from
the site-packages install of this wheel. `npm install lodash` in the
project directory is the fix; or pass `create(node='/path/to/node')` /
`CC_NODE_BIN` when the wrong Node is on `PATH`. Missing-module errors
name that cwd rule.

**Empty `{}` is a live handle.** `js.eval('({})')` stays a `JsHandle`
so later property use matches Node. Non-empty plain objects still cross
as Python `dict`s (data returns). Same-domain handles chain
(`h.update(…).digest(…)`); foreign-domain handles do not.

**Thenables are awaited in the child.** Promise-based npm APIs need no
`async`/`await` on the Python side — the call blocks until settle (or
raises `JsError` on reject). That is the opposite of
`concurrent-c-python`'s isolated surface, where every call is already a
JS Promise you must await.

**Wire cost vs tiny work.** Round trip is ~100µs class; a one-line JS
helper on three numbers loses to pure Python. Prefer Python (or a native
CC module) for small/hot work; use the bridge when Node/npm owns the
kernel (crypto, parsers, large buffers via shm).

**Crash isolation, not a sandbox.** The child inherits your environment
and privileges — do not evaluate untrusted JavaScript. `destroy()` is
cooperative; CPU-bound JS is not preemptible (wait or kill + new domain).

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

Writing Concurrent-C itself rather than Python?  The zero-IPC hosted
tier is `cc_js_new(false, &a)` (needs libnode) —
[`examples/js/jsdemo.shcc`](https://github.com/sreekotay/concurrent-c/blob/main/examples/js/jsdemo.shcc);
`cc_js_new(true, &a)` is the same separate-process wire this package
speaks, from CC.

## Publishing

From the Concurrent-C repo root (packs this wheel and the npm sibling):

```
./scripts/publish_bridges.sh              # → out/pypi/concurrent_c_node-* (+ npm tgz)
./scripts/publish_bridges.sh --publish    # bump patch, pack, twine + npm publish
```

A worked tour (builtin Node modules, chains, callbacks, thenables,
buffers — no npm install needed):
`python -m cc_node.examples.use_node`. Wire RTT / shm:
`python -m cc_node.examples.bench_wire`. Multi-core domains (threads):
`python -m cc_node.benchmarks.multi_domain`.

Adversarial multi-child storm (escaped closures, cooperative
fanout-destroy, abort inject, handle-leak / RSS soaks):
[`stress/bridge/`](https://github.com/sreekotay/concurrent-c/tree/main/stress/bridge)
— `./stress/bridge/run.sh` (`CHAOS_SCALE=full` / `soak` for bigger N).
Mode catalog + destroy contracts:
[`bridge_stress.md`](https://github.com/sreekotay/concurrent-c/blob/main/stress/bridge/bridge_stress.md)
(latency demos stay in `cc_node/examples/`).

And when the hot path is YOUR code rather than an npm package, skip the
wire entirely: a page of Concurrent-C (or C) exports as a native module
for Python and Node both — 40-90ns calls, stable-ABI artifacts.  See
[Native modules for Node and Python](https://github.com/sreekotay/concurrent-c/blob/main/docs/js-py-modules.md).
