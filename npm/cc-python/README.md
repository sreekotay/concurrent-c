# concurrent-c-python

Python from Node. Any module, zero copies, host-controlled lifetime.

Built with [Concurrent-C](https://github.com/sreekotay/concurrent-c) — a
strict C11-superset preprocessor: `.ccs` lowers to plain C and compiles
with your host C compiler.

Map of the three boundaries (CC hosts Python, native modules, this
package bridge):
[JS / Python interop](https://github.com/sreekotay/concurrent-c/blob/main/docs/js-py-modules.md).

```js
const py = require('concurrent-c-python').create();         // in-process (default)
// const py = require('concurrent-c-python').create({ isolated: true }); // child process
const np = py.import('numpy');

const a = new Float64Array(1_000_000).map((_, i) => i % 97);
const b = new Float64Array(1_000_000).map((_, i) => i % 89);

np.dot(a, b);   // 1M-element dot product: 5-8x FASTER than the same
                // loop in JS — the arrays cross as zero-copy leases,
                // numpy's BLAS does the math, a JS number comes back

py.destroy();   // one sweep: every handle, the arena, the interpreter
```

## In-process (default) vs isolated (separate process)

`create()` embeds **libpython in this Node process**.
`create({ isolated: true })` spawns a **separate CPython child** — same
call surface, different crossing cost and failure domain:

| | `create()` — in-process | `create({ isolated: true })` — separate process |
|---|---|---|
| Where Python runs | same OS process as Node | own CPython child |
| Hot call | ~5µs crossing; 1M `np.dot` **~192µs** (6.4x a JS loop) | ~100µs wire RTT; bulk via shm |
| Buffers | zero-copy leases | shm spill (8MB arg **~6.4ms**) |
| Parallelism | lane / sibling subinterpreters (3.12+); still one process | **N children = N GILs on N cores — full multi-core speedup** |
| Crash | native crash can take Node with it | child dies → rejected promise; parent lives |
| Per-domain python/venv | process-wide `usePython(...)` | yes — each child picks its own |

Isolated is how you get **real core scaling with numpy** (and any other
C-extension that refuses in-process subinterpreters): each domain is a
full CPython, so N domains use N cores.  Pick **in-process** for hot
fine-grained calls and zero-copy buffers; pick **isolated** for that
multi-core speedup, crash isolation, and per-domain environments.
Measured receipts:
[`js_numpy_bridge_node_20260810.txt`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/js_numpy_bridge_node_20260810.txt)
(in-process) ·
[`js_multiprocess_numpy_node_20260810.txt`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/js_multiprocess_numpy_node_20260810.txt)
(isolated).

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
of work in two domains completes together in 20.1ms.  Two honest
limits: this tier needs **CPython 3.12+** (`Py_NewInterpreterFromConfig`
is the only door to a per-interpreter GIL — on 3.10/3.11 the first
domain works and a second refuses by name), and **numpy itself refuses
subinterpreters** (the CPython C-extension rule — articulate, not a
crash), which is what the fourth tier is for.

**N × numpy: isolated domains (separate process)** —
`create({ isolated: true })` spawns a FULL CPython child per domain
(not an in-process subinterpreter): numpy in every one, **N domains
are N GILs on N cores — full multi-core speedup**, a child crash is a
rejected promise (the parent survives), and per-domain python/venv
selection is honest:

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
[`js_multiprocess_numpy_node_20260810.txt`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/js_multiprocess_numpy_node_20260810.txt),
BLAS pinned): spawn+import numpy **~109ms** (warm), wire RTT **~98µs**,
8MB shm arg **~6.4ms**; the same numpy workload on 4 domains runs
**2-4x faster** than on one (2.22x this capture; up to ~4x when the box
is quiet).  Small arrays inline; big results stay child-side handles
(`await arr.toTypedArray()` brings bytes back through the same spill).

Teardown on an isolated domain is **cooperative**: `destroy()` sends
`close`, drains in-flight wire work, then waits (SIGKILL only if the
child ignores close). An in-flight call may still fulfill with a
correct value; after destroy every door answers `bridge is closed`.

There is **no clean cancel of CPU-bound native work** (BLAS, long C):
Python/`py.task` cannot unwind a running `np.dot`. Choices are wait
(cooperative `destroy` — in-flight may fulfill) or **kill the worker**
(`SIGKILL` / abort / `_exit`) and mint a new domain. Kill rejects
in-flight ops and must not leak SHM spill files. Correctness of that
path: [`stress/bridge/bridge_stress.md`](https://github.com/sreekotay/concurrent-c/blob/main/stress/bridge/bridge_stress.md).
Cost of kill+respawn:
[`examples/js_isolated_cancel_churn.js`](examples/js_isolated_cancel_churn.js).

Keyword arguments cross the wire explicitly marked and **last** —
`kwargs({...})` is the marker; a trailing plain object stays a
positional dict, never silently reinterpreted:

```js
const { kwargs } = require('concurrent-c-python');
const fmt = await b.eval('lambda a, *, sep="-": f"{a}{sep}end"');
await fmt(1, kwargs({ sep: '+' }));   // '1+end'
```

(In-process calls are positional today and refuse the marker by name —
bind keywords in Python, e.g. `functools.partial`.)

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
text intact.  `destroy()` revokes the domain — queued work rejects with
`bridge is closed`, then the lane drains and sweeps. In-flight work
already running may still settle; afterwards every door is closed.

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

## Proxies, stated plainly

A held Python object is a JS Proxy over a *function* target (so calls
trap), which has honest consequences worth knowing before handing one
to generic JS code:

- `typeof proxy === 'function'` even when the Python object is not
  callable.
- `then` is special: hidden in-process, "materialize the attribute" on
  isolated proxies — a Python object's genuine `.then` attribute needs
  `builtins.getattr(obj, 'then')`, not property access.
- `toString`/`toJS` (in-process) and `str`/`toTypedArray` (isolated)
  are bridge doors and shadow same-named Python attributes — reach the
  Python ones through `builtins.getattr` too.
- Symbol-keyed properties answer `undefined`: a proxy is not a JS
  iterable, and Python iterables do not grow `Symbol.iterator` —
  materialize to a list or typed array first.

None of this bites ordinary attribute-and-call use; it bites libraries
that introspect objects generically (duck-typed thenable checks,
spread/iteration, `typeof` dispatch).

## Trust and compatibility

- A **sync in-process call runs Python on the Node thread**: a long
  call blocks the event loop, and a native-extension crash is a Node
  process crash.  That is the price of ~5µs calls — `py.task` moves
  work off-thread, isolated domains move it out of the process.
- **In-process sibling domains need CPython 3.12+** (per-interpreter
  GILs).  On 3.10/3.11 the first domain works; a second refuses by
  name.  Isolated domains parallelize on any supported Python.
- `{ isolated: true }` is **crash isolation, not a security sandbox**:
  the child inherits your environment and runs with your OS
  privileges.  Do not run untrusted Python through it.
- Bulk-buffer spill files live in a **private 0700 per-bridge
  directory** (0600, exclusive-create), removed with the bridge.
- `using py = ...` disposes **without awaiting** (a sync dispose
  cannot await): teardown may still be draining when the scope exits.
  When completion matters, `await using py = ...` or
  `await py.destroy()` are the truthful forms.
- Linux and macOS.  A platform without a shipped prebuilt compiles the
  vendored C at install time and needs a C compiler.

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
./scripts/publish_bridges.sh --publish    # bump patch, pack, npm publish + twine
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
[Native modules for Node and Python](https://github.com/sreekotay/concurrent-c/blob/main/docs/js-py-modules.md).

## Measured

Two surfaces, two folders of receipts — **in-process default** vs
**`{ isolated: true }` separate process** — on a 4-vCPU x86-64 box,
numpy 2.5.1 (catalog:
[`perf/baselines/README.md`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/README.md)):

### In-process — `create()`

From [`examples/js_numpy_bridge.js`](examples/js_numpy_bridge.js)
([`js_numpy_bridge_node_20260810.txt`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/js_numpy_bridge_node_20260810.txt)):

| what | result |
|---|---|
| 1M-element `np.dot` through the bridge | **192µs/call — 6.43x the JS loop** (1.24ms) |
| 1M-element `np.sum` / `np.std` | 420µs / 2.0ms per call |
| 16-element dot (the crossing itself) | **4.4µs** sync; **11µs** pipelined through the lane |
| 1M dot through the lane | 259µs/call — the off-thread call costs what the sync one does |
| bridge size | **~100KB `.node`, libc-only** |

From [`examples/js_numpy_bridge_async.js`](examples/js_numpy_bridge_async.js)
([`js_numpy_bridge_async_node_20260810.txt`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/js_numpy_bridge_async_node_20260810.txt))
— what the lane buys while staying in-process:

| what | result |
|---|---|
| 1ms ticks during 100ms of bulk numpy | **99 through the lane, 0 sync** — the loop stays alive |
| JS compute overlapped with numpy | **1.47x** this capture (wall ≈ max, not sum) |
| numpy ∥ numpy, one interpreter (BLAS pinned) | **1.64x** this capture |

And [`examples/js_two_interp.js`](examples/js_two_interp.js): two
in-process sibling domains lease **the same `Float64Array`** zero-copy
— JS is the neutral ground — and their lanes run under two GILs
(CPython 3.12+).

### Isolated — `create({ isolated: true })` (separate process)

From [`examples/js_multiprocess_numpy.js`](examples/js_multiprocess_numpy.js)
([`js_multiprocess_numpy_node_20260810.txt`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/js_multiprocess_numpy_node_20260810.txt)):

| what | result |
|---|---|
| spawn + import numpy (warm) | **109ms** |
| wire round trip | **98µs** (~20× an in-process crossing) |
| 8MB argument, shm spill | **6.4ms** |
| same numpy workload, 4 domains vs 1 | **2.22x** this capture — **up to ~4x / linear in cores** when quiet |

Numbers swing ±40% run-to-run on a small shared VM; the example files
print machine-comparable `RESULT` lines, so re-measuring on your box is
one command.

Adversarial kitchen-sink (escaped closures, lease detach, cooperative
subset-destroy, SIGKILL mid-spill, abort inject, mixed soaks):
[`stress/bridge/`](https://github.com/sreekotay/concurrent-c/tree/main/stress/bridge) — `./stress/bridge/run.sh`.
Mode catalog + destroy contracts:
[`stress/bridge/bridge_stress.md`](https://github.com/sreekotay/concurrent-c/blob/main/stress/bridge/bridge_stress.md)
(latency demos stay in `examples/`).
