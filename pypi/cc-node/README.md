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

The mirror of [`cc-python`](../../npm/cc-python) — same domain model,
same materialization rules, pointed the other way.  No engine embedding:
the domain **is** a spawned `node` child (found on `PATH`, or
`CC_NODE_BIN`), so you get real Node — full stdlib, native addons,
whatever npm installs — and process isolation for free.  `npm install`
next to your Python program; `require` resolves from your cwd.

- **Values**: plain data (finite numbers, strings, booleans, `None`,
  lists/dicts of the same) crosses by value; everything else is a live
  handle owned by the domain — attribute access is property lookup
  (methods arrive bound), calls are calls, `str()` is `String()`.
  Non-finite floats cross tagged, never silently nulled.
- **Async is free**: a thenable result is awaited in the child before
  the reply, so promise-based package APIs look synchronous from
  Python.
- **Callbacks**: a Python callable crosses as a JS function.  JS
  calling conventions apply (`Array.map` calls with value, index,
  array — take `*rest`).  Exceptions map both ways, messages intact.
- **The domain rules hold**: handles never cross bridges; `stats()` is
  the handle ledger and `release()` drops one early; `destroy()` is
  idempotent, every door answers `bridge is closed` after, and the
  child dies with the bridge (and on host exit, via stdin EOF).

The wire is strict request/response JSON over stdio — simple and
correct first.  The shared-memory lease tier (zero-copy buffers, the
same transport the process-isolated cc-python domains will use) layers
on without changing this surface.
