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
