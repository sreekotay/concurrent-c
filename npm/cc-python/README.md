# cc-python

Python from Node over the Concurrent-C bridge.

```js
const py = require('cc-python').create();   // an Isolation Domain
const np = py.import('numpy');

const norm = np.linalg.norm(new Float64Array([3, 4]));   // 5 — zero copy

py.destroy();   // one sweep: every handle, the arena, the interpreter ref
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

`py.task(jsClosure)` is reserved for recorded batch graphs —
parameterized pipelines that ship N Python calls as one job (and, later,
across a process boundary) — and says so articulately until it exists.

## Numbers

JavaScript has one number type; integral values cross as Python `int`,
fractional as `float` (Python APIs that want a float accept an int — the
reverse is not true).  Python ints beyond 2^53 come back as `BigInt`.

## Building

The addon is ordinary Concurrent-C:

```
ccc build npm/cc-python/src/cc_python.ccs   # → bin/cc_python.node
```

`index.js` finds it at `npm/cc-python/bin/` or the repo `bin/`, or wherever
`CC_PYTHON_ADDON` points.  One stable-ABI binary per platform; the Python
runtime resolves lazily at `create()` (`CC_LIBPYTHON` overrides the probe).
