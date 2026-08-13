# concurrent-c-python

Python from Node. Any module, zero copies, host-controlled lifetime.
Native types, exceptions, callbacks, and async all cross the boundary.

Built with [Concurrent-C](https://github.com/sreekotay/concurrent-c) — a
strict C11-superset preprocessor: `.ccs` lowers to plain C and compiles
with your host C compiler.

Map of the three boundaries (CC hosts Python, native modules, this
package bridge):
[JS / Python interop](https://github.com/sreekotay/concurrent-c/blob/main/docs/js-py-modules.md).

```js
const py = require('concurrent-c-python').create();  // in-process
// const py = require('concurrent-c-python').create({ isolated: true });
const np = py.import('numpy');

const a = new Float64Array(1_000_000).map((_, i) => i % 97);
const b = new Float64Array(1_000_000).map((_, i) => i % 89);

np.dot(a, b);   // 1M-element dot product: 5-8x FASTER than the same
                // loop in JS — the arrays cross as zero-copy leases,
                // numpy's BLAS does the math, a JS number comes back

py.destroy();   // one sweep: every handle, the arena, the interpreter
```

**Mental model.** A domain owns the interpreter and every handle.
Proxies are borrows from that domain. `destroy()` / `using` / GC of the
last proxy → one sweep. Default call blocks this thread until Python
answers. `py.task` is the Promise door. `{ isolated: true }` is crash
isolation and a different Python — not a different calling convention.
Scalars and `None` materialize; everything else is a proxy until
`toJS()` / `toTypedArray()` / `String()`.

**Cheat sheet**

- Default call blocks. `py.task` is the only Promise.
- `{ isolated: true }` is not a different `await` story.
- Trailing `{…}` is a positional dict; `kwargs({…})` means keywords.
- `===` is not Python `is`. `if (proxy)` is always true (`typeof` is `'function'`).
- Overlap isolated domains with `Promise.all([a.task(f)(), b.task(g)()])`.

Modes tour: [`examples/modes_tour.js`](examples/modes_tour.js).  
Costs (`RESULT` lines): [`benchmarks/modes_bench.js`](benchmarks/modes_bench.js)
(`VIRTUAL_ENV=…` if in-process needs numpy).

```
npm install concurrent-c-python
```

Prebuilt where shipped; otherwise compiles vendored C at install (`cc`).
~100KB `.node`, libc only, N-API stable ABI.

## In-process vs isolated

| | `create()` | `create({ isolated: true })` |
|---|---|---|
| Where | libpython in this process | child CPython |
| Calls | sync; `py.task` → Promise | same |
| Hot path | ~µs; zero-copy buffers | ~20–100µs RTT; copy/shm |
| Parallelism | lane / subinterpreters (3.12+) | N children, N GILs |
| Crash | can take Node with it | child dies; parent lives |
| Python / venv | process-wide `usePython(...)` | per domain (`python:`) |

Use in-process when that runtime has your packages and you want the
BLAS-3 / matmul prize (zero-copy typed arrays). Prefer
`create({ isolated: true })` for long-running services, ambient/`pip`
packages, crash isolation, or multi-core fan-out — and **pin the npm
version** in prototypes. Don’t judge modes on `np.dot` alone — see
Measured (matmul/SVD).

**Worker threads.** In-process libpython is owned by **at most one Node
thread** (the first `create()` wins). Further `create()` calls on that
same thread are fine; a `create()` from another `worker_threads` Worker
refuses with `in-process bridge already owned by another thread; use
create({ isolated: true })`. Concurrent in-process `create()` from
several workers used to abort the process (`_PyImport_Init`); isolated
domains are fully worker-safe. Proxies are not structured-cloneable
(`DataCloneError`) — do not share handles across threads.

Receipts:
[`js_numpy_bridge_node_20260810.txt`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/js_numpy_bridge_node_20260810.txt)
·
[`js_multiprocess_numpy_node_20260810.txt`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/js_multiprocess_numpy_node_20260810.txt)
·
[`cc_python_modes_bench_20260810.txt`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/cc_python_modes_bench_20260810.txt).

## Common issues

**`No module named 'numpy'` with `create()`.** In-process loads a linked
libpython (often a bare embed). Your shell’s `pip install` does not
change that. Point the bridge at a Python that already has the package:

```js
// in-process: pick the runtime once, before first create()
ccpy.usePython('/path/to/venv');          // or a python3 binary
const py = ccpy.create();

// or: child process — uses PATH / that venv’s site-packages
const py = ccpy.create({
  isolated: true,
  python: '/path/to/venv',                // optional; per domain
});
```

See [Choosing the Python](#choosing-the-python).

### Handles and GC

Proxies are collected by `FinalizationRegistry`, which only runs after
**event-loop turns**. A fully-synchronous loop that mints handles can
look like a leak (`stats()` in the thousands, RSS climbing) until you
`await` / `setImmediate` / return to the loop — then the ledger drains.

In hot sync loops, call `py.release(h)` (or scope with `using py =
create()` / `destroy()`) instead of relying on GC. `stats()` reports
live handles; the bridge warns once if the count climbs past 5 000
without a release.

### Dead domains

After `destroy()` / child exit, `import` and calls reject with
`bridge is closed` — they do not mint a proxy that fails only later.

### Stdout / `print` / environ

In-process, Python shares Node’s process: `print` / `sys.stdout` are the
same fds, and `os.environ["X"] = ...` is live `process.env.X` (locale and
other process-global state too). Fine for trusted code; easy to corrupt
proxy/TLS/DNS knobs or a parent that parses stdout. Redirect in Python
(`contextlib.redirect_stdout`), or use `create({ isolated: true })` when
you need a trust boundary — **in-process Python is your process**.

**Empty `dict()` stays a handle.** An `exec` namespace must stay on the
Python side — a JS `{}` has no `.get` and cannot accumulate bindings.

```js
const g = builtins.dict();
builtins.exec(`def f(x): return x + 1`, g);
g.get('f')(41);   // 42
```

Same-domain handles chain (`const fft = np.fft.fft(buf); np.abs(fft)`).

## Surface

- Attribute chains are Python (`np.linalg.norm`). Scalars materialize;
  everything else stays a proxy. `String(proxy)` / `util.inspect` →
  `str()`. Assignment is `setattr` (mappings fall back to
  `__setitem__`). On mappings, **keys win** over methods: `d.get` is
  the value for key `"get"` when present — use `builtins.getattr` for
  the method. `Object.keys` / `in` use mapping keys / `__contains__`;
  `getOwnPropertyDescriptor` returns accessors (cheap reflection;
  values pay on `[[Get]]`). Bare `builtins.eval` / `exec` (no globals
  dict) use the domain's `__main__` namespace — same as isolated.
  `using py = create()` sync-disposes the domain. Python exceptions
  expose `.pyType` (and `.code`) as the class name (e.g. `KeyError`),
  not `CC_ERR_INTERNAL`.

- Typed-array args (`Float64`/`Float32`/`Int32`/`BigInt64`/`Uint8Array`
  and Node `Buffer`) are zero-copy **memoryviews** for the call — same
  Python type at `n=0` and `n>0` (never a list). Writable (writes land
  in the caller's array); kept past return is an error, not corruption.
  They are not numpy ndarrays — wrap with `np.asarray(mv)` when you need
  one. Bulk results: `proxy.toTypedArray()` (in-process and isolated)
  copies a 1-D numeric buffer out as a real TypedArray.
  Plain JS **arrays** and **objects** cross as Python `list` / `dict`
  (nested scalars, handles, null/undefined→`None`). Typed arrays stay
  top-level only (the zero-copy lease). Keys starting with `$` are
  reserved.
- Handles are per-domain (`stats()` / `release(proxy)`). `destroy()` /
  `using` / GC sweeps once; afterwards: `bridge is closed`. Unknown
  attribute access on a handle throws (not silent `undefined`) — host
  callbacks receive the same Proxies as call results. Proxies use a
  function target (`typeof === 'function'`), so **`if (proxy)` is always
  true** — check `len` / compare to `None` / `.toJS()` for emptiness.
  **`===` is not Python identity**: each crossing mints a new proxy for
  the same underlying object; use a Python `is` helper if you need that.
- JS numbers → `int` or `float`; Python `int` past 2^53 comes back as
  exact `BigInt` (full range — never a lossy double). `BigInt` args
  round-trip to Python `int`. Signed `-0` stays a float (not collapsed
  to `0`). Lone UTF-16 surrogates in strings are refused. Proxies are
  function-targets and iterable (`for…of` / `Symbol.iterator` →
  `__next__`). Exceptions with empty `str(exc)` still name the type.
  Bridge doors (`then`, `toString` / `toJS` / `toJSON` / `toTypedArray`)
  can shadow Python names — use `builtins.getattr`.
- **`toJS` / `toJSON` (one materializer).** Both run the same strict
  one-crossing deep copy: JSON-safe scalars, `dict` / `list` / `tuple`
  only. Int dict keys become string keys; cycles and unrepresentables
  (`set`, `bytes`, callables, …) refuse with type + path
  (`cannot materialize set at $.tags — convert with list(...)`) — never
  `str()`-coerce. `JSON.stringify(p)` and `JSON.stringify(p.toJS())`
  agree (same sync function) in both modes. Cost: reflection (`Object.keys`)
  stays cheap; materialize pays once. After materialize, host JSON rules
  apply: **`NaN` / `±Infinity` stringify to `null`** (ECMA-262 — same as a
  plain `{x: NaN}` in JS; scipy results often hit this). Exact `BigInt`
  fields throw the native `TypeError` from `JSON.stringify` (no bridge
  special-case).
- Isolated is crash isolation, not a sandbox. Spill files: private 0700
  dir per bridge, removed on destroy.

## `py.task`

```js
const norm = py.task(np.linalg.norm);
await norm(new Float64Array(1_000_000));
await py.destroy();
```

Task calls are Promises on a per-domain lane (FIFO, GIL). Everything
else stays sync. The event loop stays live while the lane runs; sync
calls on a busy domain wait for the GIL then run. Queued work rejects on
`destroy()`; an in-flight call (including CPU-bound BLAS) may still
finish — wait, or kill an isolated child and create a new domain
([`js_isolated_cancel_churn.js`](examples/js_isolated_cancel_churn.js)).

### Parallelism

1. **Lane ∥ JS** — numpy on the lane, JS on main; wall ≈ max (~1.5–1.8×).
2. **Lane ∥ sync, one interpreter** — BLAS releases the GIL. Pin BLAS
   threads or they compete (`OPENBLAS_NUM_THREADS=1`):

```js
const p = py.task(np.dot)(a, b);
np.dot(c, d);
await p;
```

3. **Sibling in-process domains** — CPython 3.12+ only. Numpy’s C
   extension refuses subinterpreters; use (4) for that.
4. **Isolated domains** — full child per `create({ isolated: true })`.
   Default calls still block; overlap N children with `py.task`.
   Large results via `arr.toTypedArray()`. Per-domain `python:` /
   `VIRTUAL_ENV` / `./.venv` / `python3`.

```js
const py = ccpy.create({ isolated: true });
const np = py.import('numpy');
const s = np.sum(buf);
```

Warm spawn+import ~109ms, wire ~98µs, 8MB shm arg ~6.4ms; 4 domains
~2–4× one domain when the box is quiet. Examples:
[`js_numpy_bridge_async.js`](examples/js_numpy_bridge_async.js),
[`js_two_interp.js`](examples/js_two_interp.js),
[`js_multiprocess_numpy.js`](examples/js_multiprocess_numpy.js).

Isolated and in-process kwargs are explicit and last — there is no
`key=` sugar on the JS call site:

```js
const { kwargs } = require('concurrent-c-python');
fmt(1, kwargs({ sep: '+' }));           // in-process and isolated
builtins.sorted(xs, kwargs({ key: neg }));  // not sorted(xs, {key: neg})
```

A trailing plain object is still a positional dict — only `kwargs(...)`
means keywords.

## `async def` and callbacks

**Do not `await` an in-process coroutine proxy.** That would look like
transparent async, but the same thenable-hijack defense that refuses
unawaited Promises on the sync path is what would make it work — so a
bare coroutine stays an unscheduled handle. Schedule with `py.task`
(lane asyncio) or `asyncio.run` on the Python side:

```js
b.exec(`
import asyncio
async def crawl(fetch, urls):
    return await asyncio.gather(*(fetch(u) for u in urls.split(',')))
`, ns);
await py.task(ns.get('crawl'))(jsFetch, 'a,b,c');
```

A JS function argument becomes a Python callable. On the lane, the
executor releases the GIL while main runs your function; async
callbacks suspend until the Promise settles. Sync bridge calls refuse a
thenable return (use `py.task`). Exceptions keep `Type: message`.

```js
await py.task(helper)(async (x) => {
  const row = await fetchThing(x);
  return await py.task(np.mean)(row);
}, seed);
```

## Choosing the Python

Loaded at first `create()`, most-specific first; a broken choice fails
loudly:

```js
ccpy.usePython('/home/app/.venv');
ccpy.usePython('/usr/bin/python3.11');
const py = ccpy.create();
ccpy.python();  // { loaded, version, lib, how }
```

Order: `usePython` → `CC_LIBPYTHON` → `VIRTUAL_ENV` → `./.venv` →
soname discovery. One in-process runtime per process; isolated domains
pick their own.

## Build / publish

**npm Trusted Publishing (OIDC)** — provenance on every release, no publish token:

1. Package [Access](https://www.npmjs.com/package/concurrent-c-python/access) →
   Trusted Publisher → GitHub Actions: owner `sreekotay`, repo `concurrent-c`,
   workflow `publish-cc-python.yml`, environment `npm`, allow `npm publish`
2. GitHub Environment `npm`
3. After the first green OIDC publish: Publishing access → require 2FA and
   **disallow tokens**; revoke automation tokens

```
./scripts/publish_bridges.sh
./scripts/publish_bridges.sh --publish --minor   # packs locally; npm+PyPI via CI
ccc build npm/cc-python/src/cc_python.ccs        # → bin/cc_python.node
```

`CC_PYTHON_ADDON` overrides addon path. Own hot path in C/CC → native
module (40–90ns) —
[JS / Python interop](https://github.com/sreekotay/concurrent-c/blob/main/docs/js-py-modules.md).

## Measured

Catalog: [`perf/baselines/README.md`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/README.md).

### In-process — `create()`

[`js_numpy_bridge.js`](examples/js_numpy_bridge.js) ·
[`js_numpy_bridge_node_20260810.txt`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/js_numpy_bridge_node_20260810.txt):

| what | result |
|---|---|
| 1M `np.dot` | 192µs (~6.4× JS loop) |
| 1M `np.sum` / `np.std` | 420µs / 2.0ms |
| 16-elem dot (crossing) | 4.4µs sync; 11µs lane |
| 1M dot on lane | 259µs |

Lane overlap ([`js_numpy_bridge_async.js`](examples/js_numpy_bridge_async.js)):
99× 1ms ticks during 100ms numpy; JS∥numpy ~1.5×; numpy∥numpy ~1.6×
with BLAS pinned.

### Isolated — `create({ isolated: true })`

[`js_multiprocess_numpy.js`](examples/js_multiprocess_numpy.js) ·
[`js_multiprocess_numpy_node_20260810.txt`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/js_multiprocess_numpy_node_20260810.txt):

| what | result |
|---|---|
| spawn + import numpy (warm) | 109ms |
| wire RTT | 98µs |
| 8MB shm arg | 6.4ms |
| 4 domains vs 1 | ~2.2× (up to ~4× quiet) |

### Same box — in-process / isolated / JS

[`modes_bench.js`](benchmarks/modes_bench.js) ·
[`cc_python_modes_bench_20260810.txt`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/cc_python_modes_bench_20260810.txt)
(checksum returns for matmul/SVD):

| workload | in-process | isolated | JS |
|---|---|---|---|
| `sqrt` ×1 | ~3µs | ~21µs | — |
| `np.dot` 1M | 0.28ms | 10ms | 0.72ms |
| matmul 128 | 0.04ms | 0.41ms | 3.3ms |
| matmul 256 | 0.13ms | 0.99ms | 17.5ms |
| SVD 256 | 3.1ms | 3.4ms | — |
| 3 isolated domains | — | 2.8× seq | — |

Host load moves absolutes; re-run the bench locally.
Stress: [`stress/bridge/`](https://github.com/sreekotay/concurrent-c/tree/main/stress/bridge).
