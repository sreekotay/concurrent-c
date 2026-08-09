# concurrent-c-python

Python from Node. Any module, zero copies, host-controlled lifetime.

Built with [Concurrent-C](https://github.com/sreekotay/concurrent-c) — a
strict C11-superset preprocessor: `.ccs` lowers to plain C and compiles
with your host C compiler.

```js
const py = require('concurrent-c-python').create();
const np = py.import('numpy');

const a = new Float64Array(1_000_000).map((_, i) => i % 97);
const b = new Float64Array(1_000_000).map((_, i) => i % 89);

np.dot(a, b);   // 1M-element dot product: 5-8x FASTER than the same
                // loop in JS — the arrays cross as zero-copy leases,
                // numpy's BLAS does the math, a JS number comes back

py.destroy();   // one sweep: every handle, the arena, the interpreter
```

The entire native bridge is a **~100KB `.node` file** with exactly one
linked dependency: libc.  No node-gyp, no Python headers at build time
(libpython is `dlopen`'d when you `create()`), no version matrix —
N-API's stable ABI means one binary per platform serves every node.

```
npm install concurrent-c-python   # prebuilt where shipped; otherwise
                                  # compiles from vendored C with `cc`
```

- **Attribute chains are Python** — `np.linalg.norm` walks getattr;
  calls walk `PyObject_Vectorcall`.  Scalar results (and scalar
  attributes like `math.pi`) materialize as JS numbers/strings/booleans;
  everything else stays a live proxy.  `String(proxy)` is Python `str()`.
- **Typed arrays cross as leases** — a `Float64Array` argument is a
  zero-copy memoryview pinned for the call; a callee that keeps it past
  return is caught, not corrupted.
- **The bridge is the owner** — every reference minted through it is
  registered with it.  `destroy()` (or `using py = ...`, or GC of the
  whole graph) runs one atomic sweep; afterwards every outstanding
  handle answers `bridge is closed`, and double destroy is a no-op.
  Handles never cross bridges: a second `create()` is a fully isolated
  domain that rejects the first one's objects.
- `py.stats()` is the live-handle count; `py.release(proxy)` drops one
  early and returns the remainder.

## Async: `py.task`

Async-ness enters through exactly one primitive:

```js
const py = require('concurrent-c-python').create();  // no modes
const np = py.import('numpy');

const norm = py.task(np.linalg.norm);           // bind to the lane once
await norm(new Float64Array(1_000_000));        // hot loop, off-thread
await py.task(math.sqrt)(16);                   // one-shot, same primitive

await py.destroy();   // always a Promise: revoke, drain, one sweep
```

Everything else stays synchronous — `math.pi`, exploratory chains,
cheap calls — and a call site tells you the truth: a task call is a
Promise, everything else blocks.  Every domain has a latent **execution
lane** (one thread, started on first task call): task calls run there
FIFO — Python is serial under its per-interpreter GIL, so a lane loses
nothing within a domain — the Node event loop stays live while Python
works, and `Promise.all` across *domains* is real parallelism.  Python
exceptions arrive as rejections with the sync bridge's messages, and
handles pass freely between sync and task calls — flavor was never a
property of the handle.  A sync call on a busy domain waits for the
in-flight task's GIL, then jumps the queue: that is the meaning of
choosing sync at a call site.

Lifetimes extend, not bend: a job owns its Python references and pins
its typed-array buffers from submit to completion, so `release()` or
GC mid-flight cannot dangle it.  `destroy()` rejects queued calls
immediately, lets the in-flight call finish, and sweeps after the last
result is delivered.  An idle lane never keeps the process alive.

### Parallel numpy, today

Three real parallelism tiers, all measured
([`examples/js_numpy_bridge_async.js`](examples/js_numpy_bridge_async.js),
[`examples/js_two_interp.js`](examples/js_two_interp.js)):

**numpy ∥ your JavaScript** — the lane runs numpy while the main thread
computes: balanced work lands at **1.8x**, wall clock ≈ max instead of
sum.

**numpy ∥ numpy, one interpreter** — BLAS releases the GIL, so a sync
call on main overlaps a task call on the lane.  numpy's own BLAS thread
pool competes with this (each call already fans out), so the pattern
wants pinned BLAS — then the lane owns the parallelism:

```sh
OPENBLAS_NUM_THREADS=1 node app.js
```
```js
const dot = np.dot, dotT = py.task(np.dot);
const p = dotT(a, b);   // numpy on the lane...
dot(c, d);              // ...numpy on main, same interpreter
await p;                // measured: 2.02x — perfect two-lane scaling
```

**numpy ∥ sibling interpreters** — every in-process `create()` after
the first is an isolated subinterpreter with its **own GIL**: real
multi-core Python, sharing buffers zero-copy through JS as neutral
ground (one `Float64Array` leased into both).  Measured: 5.7ms + 24.5ms
of work in two domains completes together in 20.1ms.  The honest limit:
**numpy itself refuses subinterpreters** (the CPython C-extension
rule — articulate, not a crash), which is what the fourth tier is for.

**N × numpy: isolated domains** — `create({ isolated: true })` spawns a
FULL CPython child per domain: numpy in every one, N domains are N GILs
on N cores, a child crash is a rejected promise (the parent survives),
and per-domain python/venv selection is honest:

```js
const py = ccpy.create({ isolated: true });  // ambient python, own process
const np = py.import('numpy');          // zero round trips — chains are lazy
const s  = await np.sum(buf);           // cross-process is natively async
```

Every door on an isolated domain is async — `stats()`, `release()`,
and `str()` return Promises too, and buffer-shaped values come back
via `await arr.toTypedArray()`.  Each child resolves its Python by the
same ambient order as everything else (`VIRTUAL_ENV`, then `./.venv`,
then `python3`; `iso.pythonExe` reports the choice) — and because each
domain is its own process, the choice can also be **per-domain**, which
the in-process tier cannot offer:

```js
const a = ccpy.create({ isolated: true, python: '/home/app/.venv' });
const b = ccpy.create({ isolated: true, python: '/usr/bin/python3.11' });
```

Measured ([`examples/js_multiprocess_numpy.js`](examples/js_multiprocess_numpy.js),
[`js_multiprocess_numpy_node_20260809.txt`](../../perf/baselines/js_multiprocess_numpy_node_20260809.txt),
BLAS pinned): the same numpy workload on 4 domains runs **2-4x faster**
than on one (3.97x — linear — at our shared 4-vCPU box's quietest;
steal time bounds the rest).  The costs are real and stated: ~100-440ms
to spawn a child and import numpy (warm/cold), ~125µs per wire round
trip (vs ~5µs in-process).  Bulk buffers spill through **shared
memory** (one memcpy per side): an 8MB argument crosses in ~6.6ms —
23x the base64 wire it replaces — small arrays inline, big results stay
child-side handles (chain on them; `await arr.toTypedArray()` brings
the bytes back through the same spill).  Pick the tier by workload:
in-process for hot fine-grained calls and zero-copy buffers, isolated
for N-way parallel numpy, crash isolation, and per-domain environments.

`py.task(jsClosure)` is reserved for recorded batch graphs —
parameterized pipelines that ship N Python calls as one job (and, later,
across a process boundary) — and says so articulately until it exists.

## async def: the asyncio lane

A task call that returns a **coroutine** becomes an asyncio task on the
lane's own event loop — engaged lazily by the first one, so the plain
FIFO path (and its latency) is untouched until you use `async def`:

```js
const ns = b.dict();
b.exec(`
import asyncio
async def crawl(fetch, urls):
    return await asyncio.gather(*(fetch(u) for u in urls.split(',')))
`, ns);
await py.task(ns.get('crawl'))(jsFetch, 'a,b,c');
```

Tasks interleave — two staggered sleeps run in max, not sum, and
completion follows readiness, not submission order.  Inside a task, an
awaited JS callback returns an **awaitable**: `await cb(x)` suspends
only that task while the loop keeps running its siblings, and the
callback's promise may itself lean on tasks of the same domain.  Sync
callables keep every earlier shape, loop mode or not: *sync nests one
deep, async composes freely.*

Exceptions keep `Type: message` in both directions and across any
number of crossings: a coroutine's `ValueError: bad input` is the JS
rejection's message; a JS rejection raises `RuntimeError` at the
Python `await` (catchable there), and uncaught it crosses back with its
text intact.  `destroy()` cancels pending tasks — their promises answer
`bridge is closed` — then drains and sweeps as always.

## Callbacks: JS functions as Python callables

A JS function passed as an *argument* crosses as a Python callable:

```js
builtins.list(builtins.map((x) => x * 2, pyList));        // sync: reenters
await py.task(builtins.list)(builtins.map(jsFn, pyList)); // lane: hops home
```

On the lane, the executor releases the GIL and waits while the main
thread runs your function — so concurrent sync bridge work proceeds and
the loop stays free to serve the callback.  Arguments materialize by
the usual rule (scalars as scalars, held objects as proxies); returns
cross back the same way.  A JS throw becomes a Python exception with
your message, catchable in Python or surfacing as the call's error.

A lane-side callback may be **async**: return a Promise and the Python
call *suspends* — GIL released — until it settles.  From Python the
callable is still plainly synchronous: `cb(x)` returns the settled
value, or raises with the rejection's text.  While a callback is
suspended, the executor services its own queue, so the promise may even
depend on a task of the *same* domain:

```js
await py.task(helper)(async (x) => {
  const row = await fetchThing(x);       // the loop is live meanwhile
  return await py.task(np.mean)(row);    // same domain — runs nested
}, seed);
```

Suspensions nest LIFO and unwind as promises settle.  A *sync* bridge
call still refuses a thenable return articulately — main cannot block
on its own event loop — and the message points at `py.task`.
Lifetime is one rule: a registered callback pins the domain until
`destroy()` — the sweep releases the function references, and a
callable that outlives its bridge raises `bridge is closed` in Python.
A callback may even destroy its own bridge mid-call (or mid-suspension):
the in-flight call finishes when its promise settles, then the drain
runs.

## Numbers

JavaScript has one number type; integral values cross as Python `int`,
fractional as `float` (Python APIs that want a float accept an int — the
reverse is not true).  Python ints beyond 2^53 come back as `BigInt`.

## Choosing the Python

The runtime loads lazily at the first `create()`, chosen most-specific
first — and every explicit or ambient choice that is broken fails
loudly, never falling through to the wrong Python:

```js
const ccpy = require('concurrent-c-python');

ccpy.usePython('/home/app/.venv');          // a venv directory
ccpy.usePython('/usr/bin/python3.11');      // an interpreter executable
ccpy.usePython('/usr/lib/libpython3.12.so');// a runtime, directly

const py = ccpy.create();                   // loads the choice
ccpy.python();  // { loaded, version, lib, how } — the introspection door
```

1. `usePython(...)` from code (interpreters are interrogated via their
   own `sysconfig` — one spawn at selection time; venvs are adopted the
   way `bin/python` itself would be, so `sys.prefix` and site-packages
   are the venv's).
2. `CC_LIBPYTHON=/path` in the environment.
3. **Ambient `VIRTUAL_ENV`** — run node inside an activated venv and
   that venv is simply used.
4. **Ambient `./.venv`** — a project-local venv (the uv / poetry
   convention) is picked up from the working directory, the same way
   the sibling `concurrent-c-node` bridge resolves `node_modules`.
5. Discovery (soname walk, 3.13 → 3.10).

One runtime per process: after the first load, a matching `usePython`
is a no-op and a different one throws, naming what already loaded.
Per-domain runtimes arrive with process-isolated domains.

## Building / publishing

From the repo root, clean-pack (and optionally upload npm + the pip sibling):

```
./scripts/publish_bridges.sh              # → out/concurrent-c-python-*.tgz (+ pip wheel)
./scripts/publish_bridges.sh --publish    # pack, then npm publish + twine upload
```

The addon itself is ordinary Concurrent-C:

```
ccc build npm/cc-python/src/cc_python.ccs   # → bin/cc_python.node
```

`index.js` finds it at `npm/cc-python/bin/` or the repo `bin/`, or wherever
`CC_PYTHON_ADDON` points.  One stable-ABI binary per platform.

And when the hot path is YOUR code rather than a Python package, skip
the bridge entirely: a page of Concurrent-C (or C) exports as a native
module for Node and Python both — 40-90ns calls, 26KB artifacts.  See
[Native modules for Node and Python](../../docs/js-py-modules.md).

## Measured

From [`examples/js_numpy_bridge.js`](examples/js_numpy_bridge.js) — plain
`node`, `require('concurrent-c-python')`, numpy 2.5.1 on a 4-vCPU x86-64 box
([`perf/baselines/js_numpy_bridge_node_20260809.txt`](../../perf/baselines/js_numpy_bridge_node_20260809.txt);
catalog: [`perf/baselines/README.md`](../../perf/baselines/README.md)):

| what | result |
|---|---|
| 1M-element `np.dot` through the bridge | **239µs/call — 5.7x the JS loop** (1.36ms); best recorded 158µs / 8.45x |
| 1M-element `np.sum` / `np.std` | 364µs / 2.0ms per call |
| 16-element dot (the crossing itself) | 5.3µs sync, 11µs pipelined through the lane |
| 1M dot through the lane | 232µs/call — the off-thread call costs what the sync one does |
| bridge size | **~100KB `.node`, libc-only** |

From [`examples/js_numpy_bridge_async.js`](examples/js_numpy_bridge_async.js)
([`js_numpy_bridge_async_node_20260809.txt`](../../perf/baselines/js_numpy_bridge_async_node_20260809.txt))
— what the lane buys:

| what | result |
|---|---|
| 1ms ticks during 100ms of bulk numpy | **99 through the lane, 0 sync** — the loop stays alive |
| JS compute overlapped with numpy (balanced work) | **1.81x** — wall clock ≈ max, not sum |
| numpy ∥ numpy, one interpreter (BLAS pinned) | 1.60x this run, 2.02x at the box's quietest |

And [`examples/js_two_interp.js`](examples/js_two_interp.js): two
isolated domains lease **the same `Float64Array`** zero-copy — JS is the
neutral ground — and their lanes run under two GILs: 12.9ms + 25.3ms of
work completes together in 16.4ms.

Numbers swing ±40% run-to-run on a small shared VM; the example files
print machine-comparable `RESULT` lines, so re-measuring on your box is
one command.

Adversarial kitchen-sink (crash isolation, shm hail, teardown races,
callback blizzard): [`stress/bridge/`](../../stress/bridge/) —
`./stress/bridge/run.sh` (latency demos stay in `examples/`).
