# Peer bench: Python from Node

`concurrent-c-python` vs the libraries people actually reach for. Same
helpers (`helpers.py`) on every side; each **in-process embed gets its
own Node process** (two libpythons in one address space will fight).

```bash
cd npm/cc-python/benchmarks/peers
npm install                          # node-calls-python + pythonia
# pymport has no Node 23 prebuild; from-source needs C++17:
CXXFLAGS=-std=c++17 npm install pymport
VIRTUAL_ENV=/path/to/venv node peer_bench.js
```

`--only cc-inproc,pythonia` narrows the set. `--peer pymport` runs one
adapter in this process (what the parent forks).

`npx pympip3 install numpy` if you want pymport's **bundled** interpreter
to have numpy; it does not share the repo `.venv`.

## Who is who

| peer | process | call style | what it is |
|---|---|---|---|
| **cc-inproc** | libpython in Node | sync; `py.task` is the Promise | this package, `create()` |
| **cc-iso** | child CPython | same as in-process | this package, `create({ isolated: true })` |
| **pymport** | libpython in Node | sync Proxy | [pymport](https://www.npmjs.com/package/pymport) — closest in-process peer |
| **ncp** | libpython in Node | `call(mod, "fn", …)` | [node-calls-python](https://www.npmjs.com/package/node-calls-python) |
| **pythonia** | child CPython | `await` every hop | [pythonia](https://www.npmjs.com/package/pythonia) (JSPyBridge) |
| **js** | — | native | naive loops; kernel baseline, not a bridge |

python-shell / python-bridge spawn a *script* (JSON/text), not a live
package graph — left out on purpose. Pyodide is WASM CPython.

## DX — same five jobs

Import numpy, dot two arrays, kwargs, catch `KeyError`, Python calls JS.

**cc-python** (in-process and isolated; same spelling):

```js
const { create, kwargs } = require('concurrent-c-python');
const py = create();                       // or create({ isolated: true })
const np = py.import('numpy');
np.dot(a, b);                              // Float64Array → memoryview
py.import('builtins').sorted(xs, kwargs({ reverse: true }));
try { m.missing(); } catch (e) { e.pyType; }  // 'KeyError'
h.apply(x => x + 1, 41);
await py.destroy();
```

**pymport** — trailing object is kwargs (the opposite of cc-python):

```js
const { pymport, proxify } = require('pymport');
const np = proxify(pymport('numpy'));
np.dot(a, b);                              // copies JS → Python
proxify(pymport('builtins')).sorted(xs, { reverse: true });
```

No domain `destroy()`. Interpreter is process-wide; numpy is whatever
`npx pympip3` installed (or a from-source rebuild against your Python).

**node-calls-python** — no Proxy; TypedArray becomes **bytes**, not floats:

```js
const py = require('node-calls-python').interpreter;
const h = py.importSync('./helpers.py');
py.callSync(h, 'dot', Array.from(a), Array.from(b));
py.callSync(h, 'do_sorted', xs, { reverse: true, __kwargs: true });
```

**pythonia** — child, every attribute and call is a Promise; `$` is kwargs:

```js
import { python } from 'pythonia';
const np = await python('numpy');
await np.dot(Array.from(a), Array.from(b));
await builtins.sorted$(xs, { reverse: true });
python.exit();
```

### Surface, not vibes

| | cc | pymport | ncp | pythonia |
|---|---|---|---|---|
| Default call | **blocks** this thread | blocks | `callSync` / `call` | **always await** |
| Event loop during CPU | `py.task` | their async API | `call()` | always async (still one child GIL) |
| Attr / call | Proxy | Proxy (`proxify`) | `call(handle, name)` | Proxy + `await` |
| Kwargs | `kwargs({k})` last; `{k}` is a **dict** | trailing `{k}` is kwargs | `{k, __kwargs:true}` | `fn$(…, {k})` |
| `Float64Array` | zero-copy memoryview (lease) | copy | **bytes** | serialized list |
| Exception class | `.pyType` | JS Error / message | message | message |
| JS callback | yes (sync; async via `py.task`) | yes | yes | yes |
| Crash isolation | only `{ isolated: true }` | no | no | child |
| Lifetime | `destroy()` / `using` | GC | process | `python.exit()` |
| Which Python | `usePython(venv)` / per-domain `python:` | bundled or rebuild | `addImportPath` | `PYTHON_BIN` |

The kwargs collision is the one that bites: a trailing `{ dtype: np.float64 }`
is keywords on pymport and a positional dict on cc-python. That is
deliberate on this side — see the package README.

## What the numbers mean

`peer_bench.js` uses each library's **idiomatic** buffer path (so ncp,
pymport, and pythonia copy 1M elements into a list; cc-inproc does not).
Rows:

- `sqrt` — crossing
- `dot16` / `norm16` — small buffer + crossing
- `dot1m` — bulk; this is where zero-copy shows
- `matmul128` — BLAS-3 checksum (kernel should dominate)
- `kwargs_sorted` / `except` / `callback` — API tax, not numpy

Host load moves absolutes; compare **ratios** on the same box.

Snapshot from this machine (darwin arm64, Node 23, CPython 3.14, numpy
2.5, `OPENBLAS_NUM_THREADS=1`) —
[`cc_python_peers_20260813.txt`](../../../../perf/baselines/cc_python_peers_20260813.txt):

| | cc-inproc | cc-iso | pymport | ncp | pythonia | JS loop |
|---|---|---|---|---|---|---|
| `sqrt` | 1.4µs | 17µs | 2.0µs | **0.78µs** | 21µs | 0.16µs |
| `dot` 16 | **2.1µs** | 28µs | 7.0µs | 5.4µs | 43µs | 0.36µs |
| `dot` 1M | **112µs** | 26ms | 223ms | 214ms | 723ms | 4.1ms |
| matmul 128 | **21µs** | 0.61ms | 3.4ms | 3.0ms | 9.2ms | 2.5ms |
| callback | 1.4µs | 42µs | 3.4µs | **0.92µs** | — | 0.12µs |
| `Float64Array` becomes | memoryview | ndarray | bytearray | bytes | (serialized) | native |

Tiny scalars: ncp's `callSync` beats a Proxy getattr. Anything with a
buffer: cc-inproc is ~2000× pymport/ncp on 1M `dot` because those two
copy through a Python `list`; pythonia is another 3× behind that on the
wire. Isolated cc is the same calling convention as in-process, ~200×
the in-process 1M row (shm + child), still ~8× pythonia.

pythonia skipped `kwargs_sorted` (must `for await` Python lists) and
`callback` (JS function wire) in this run — both APIs exist, they just
did not take the same helpers without extra ceremony.
