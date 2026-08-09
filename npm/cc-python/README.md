# cc-python

Python from Node. Any module, zero copies, host-controlled lifetime.

```js
const py = require('cc-python').create();
const np = py.import('numpy');

const a = new Float64Array(1_000_000).map((_, i) => i % 97);
const b = new Float64Array(1_000_000).map((_, i) => i % 89);

np.dot(a, b);   // 1M-element dot product: ~8x FASTER than the same
                // loop in JS — the arrays cross as zero-copy leases,
                // numpy's BLAS does the math, a JS number comes back

py.destroy();   // one sweep: every handle, the arena, the interpreter
```

The entire native bridge is a **~100KB `.node` file** with exactly one
linked dependency: libc.  No node-gyp, no Python headers at build time
(libpython is `dlopen`'d when you `create()`), no version matrix —
N-API's stable ABI means one binary per platform serves every node.

```
npm install cc-python     # prebuilt where shipped; otherwise compiles
                          # from vendored C with nothing but `cc`
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
const py = require('cc-python').create();       // no modes
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
const py = ccpy.create({ isolated: true, python: '/home/app/.venv' });
const np = py.import('numpy');          // zero round trips — chains are lazy
const s  = await np.sum(buf);           // cross-process is natively async
```

Measured ([`examples/js_multiprocess_numpy.js`](examples/js_multiprocess_numpy.js),
BLAS pinned): the same numpy workload on 1 → 2 → 4 domains scales
**1.00x → 2.13x → 3.97x** on a 4-core box — linear.  The costs are real
and stated: ~440ms to spawn a child and import numpy, ~134µs per wire
round trip (vs ~5µs in-process).  Bulk buffers spill through **shared
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
const ccpy = require('cc-python');

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
   the sibling `cc-node` bridge resolves `node_modules`.
5. Discovery (soname walk, 3.13 → 3.10).

One runtime per process: after the first load, a matching `usePython`
is a no-op and a different one throws, naming what already loaded.
Per-domain runtimes arrive with process-isolated domains.

## Building

The addon is ordinary Concurrent-C:

```
ccc build npm/cc-python/src/cc_python.ccs   # → bin/cc_python.node
```

`index.js` finds it at `npm/cc-python/bin/` or the repo `bin/`, or wherever
`CC_PYTHON_ADDON` points.  One stable-ABI binary per platform.

## Measured

From [`examples/js_numpy_bridge.js`](examples/js_numpy_bridge.js) — plain
`node`, `require('cc-python')`, numpy 2.5.1 on a 4-vCPU x86-64 box
(baselines checked into the repo under `perf/baselines/`):

| what | result |
|---|---|
| 1M-element `np.dot` through the bridge | **158µs/call — 8.45x the JS loop** (1.33ms) |
| 1M-element `np.sum` / `np.std` | 334µs / 1.8ms per call |
| 16-element dot (the crossing itself) | 5.1µs sync, 17µs pipelined through the lane |
| bridge size | **~100KB `.node`, libc-only** |

From [`examples/js_numpy_bridge_async.js`](examples/js_numpy_bridge_async.js)
— what the lane buys:

| what | result |
|---|---|
| 1ms ticks during 100ms of bulk numpy | **98 through the lane, 0 sync** — the loop stays alive |
| JS compute overlapped with numpy (balanced work) | **1.83x** — wall clock ≈ max, not sum |
| 1M dot awaited / pipelined | 217µs / 181µs per call |

And [`examples/js_two_interp.js`](examples/js_two_interp.js): two
isolated domains lease **the same `Float64Array`** zero-copy — JS is the
neutral ground — and their lanes run under two GILs: 5.7ms + 24.5ms of
work completes together in 20.1ms.

Numbers swing ±40% run-to-run on a small shared VM; the example files
print machine-comparable `RESULT` lines, so re-measuring on your box is
one command.
