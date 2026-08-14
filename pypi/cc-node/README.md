# concurrent-c-node

Call Node (and npm packages) from Python.
Native types, exceptions, callbacks, and async all cross the boundary.

Part of [Concurrent-C](https://github.com/sreekotay/concurrent-c) — a
strict C11-superset preprocessor: `.ccs` lowers to plain C and compiles
with your host C compiler. (This bridge itself is pure Python stdlib —
no native build.)

Map of the three boundaries (CC hosts JS, native modules, this package
bridge):
[JS / Python interop](https://github.com/sreekotay/concurrent-c/blob/main/docs/js-py-modules.md).

```python
from cc_node import require          # one session child (lazy)
_ = require('lodash')                # cwd node_modules
_.chunk([1, 2, 3, 4, 5], 2)          # [[1, 2], [3, 4], [5]]
```

A private child (`create()`) is still there when you want N Nodes or
an explicit lifetime — not required for the first call.

```python
import cc_node
js = cc_node.create()                # a private Node, not the session
semver = js.require('semver')
semver.satisfies('1.2.3', '^1.0.0')  # True
js.destroy()                         # or: with cc_node.create() as js:
```

The other direction (Python from Node):
[`concurrent-c-python`](https://www.npmjs.com/package/concurrent-c-python)
(in-process by default; vs pymport / ncp / pythonia in that README).

Every `create()` is a separate Node — real addons, crash isolation,
measurable wire. N domains = N processes. `require()` / `get()` share
one session for the process (the Jupyter kernel).

| | this package | CC hosted (`cc_js_new(false, …)`) |
|---|---|---|
| API | `require()` / `create()` | `.ccs` program |
| Where | child `node` | libnode in-process |
| Hot call | ~105µs RTT | sub-µs (needs libnode) |
| Bulk | shm (~9.5ms / 8MB) | in-process |
| Parallelism | N children | one process |
| Crash | child dies; parent lives | shared fate |

**Cheat sheet**

- Always a child `node`. A call blocks until JS answers; thenables wait
  in the child. No `{ async: true }`.
- `from cc_node import require` is the session. `create()` is a private
  child. `reset()` / `%js_reset` / `%reset` / atexit tear the session down.
- Scalars / `None` materialize; empty `{}` stays a handle; everything
  else is a `JsHandle` until `str()` / attrs / a call.
- Notebook: `import cc_node` registers `%%js` (no `%load_ext`). Same
  session as `require()`.
- `eval()` is one RTT, no extra globals. `%%js` / `eval_cell` install
  cwd `require` once.
- `--bind` is `Object.assign(globalThis, …)` of names you name (wire
  types only; no pickle). Missing names refuse.
- First Ctrl-C finishes the in-flight call (wire stays in sync).
  Interrupt again to kill the child.

```
pip install concurrent-c-node                 # needs node on PATH
pip install 'concurrent-c-node[jupyter]'      # IPython (%%js); require() does not need this
python -m cc_node.examples.use_node
python -m cc_node.examples.bench_wire
python -m cc_node.benchmarks.multi_domain
python -m cc_node.benchmarks.vs_alts          # vs pythonia / DIY node / pythonmonkey / mini-racer
```

## Jupyter / Colab

Same verb as pythonia: `require`. No `%load_ext`, no `create()`, no
`destroy()` for the happy path. One session child for the kernel;
`console.log` lands in the cell.

```python
%pip install concurrent-c-node
# if `node` is missing (typical Colab):
!apt-get install -y nodejs

from cc_node import require
require('lodash').chunk([1, 2, 3, 4, 5], 2)
```

`import cc_node` also registers `%%js` (IPython already running; the
`[jupyter]` extra is only if you want magics without IPython already
installed).

```python
%%js
console.log('hi')              # shows in the cell
require('lodash').chunk([1, 2, 3, 4, 5], 2)
```

```python
xs = [1, 2, 3, 4]

%%js -b xs -t chunks
xs.map(x => x * 2)             # wire types only; no pickle fallback
```

| | |
|---|---|
| `from cc_node import require` | session `require`; spawns on first call |
| `import cc_node` | registers magics; does **not** spawn until first `require()` / `%%js` |
| `%load_ext cc_node` | same, idempotent; not required |
| `%js 1+1` / `%%js` | eval on the same session; last expression is the result |
| `-b xs` / `--bind xs,n` | publish those Python names on `globalThis` for the cell |
| `-t chunks` / `--to` | store the result in the notebook namespace |
| `%js_stats` | handle-table size (spawns if needed) |
| `%js_reset` / `cc_node.reset()` | `destroy()` the session child; `%reset` does this too |
| `cc_node.get()` | the session the magics use; `create()` is still a private child |
| `cc_node.kernel()` | alias of `get()` |
| `cc_node.require('fs')` | `get().require('fs')` |
| `JsHandle` display | cheap `JsHandle #3` — repr does not cross the wire |

`eval()` is unchanged (one RTT, no extra globals). `%%js` / `eval_cell`
install cwd `require` once so cells look like Node (`require('path')`).
`--bind` is `Object.assign(globalThis, …)` — missing names and non-wire
types (`DataFrame`, a set) fail articulately. Reserved names
(`require`, `process`, `globalThis`, …) refuse so a bind cannot shadow
Node.

**Interrupt.** First Ctrl-C does not abandon the in-flight reply (that
would desync the wire and drop callbacks). The call finishes, the result
is discarded, the domain stays up. Interrupt again to **kill** the
child — cooperative `destroy()` cannot stop a JS `for (;;) {}`. Same
honesty as the rest of the bridge.

JS kernels (`tslab`, Deno Jupyter) are
[`concurrent-c-python`](https://www.npmjs.com/package/concurrent-c-python);
Colab is not that. There, default `create()` blocks the kernel thread —
`py.task` / `{ isolated: true }` when the loop must stay live.

## Measured

[`bench_wire`](https://github.com/sreekotay/concurrent-c/blob/main/pypi/cc-node/cc_node/examples/bench_wire.py)
·
[`cc_node_bridge_py_20260810.txt`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/cc_node_bridge_py_20260810.txt):

| what | result |
|---|---|
| spawn (first eval) | 28ms |
| wire RTT | 105µs |
| Python callback round trip | 153µs |
| 8MB `array('d')` via shm | 9.5ms |
| same 8MB as JSON list | 499ms (~52×) |

Wire: line-JSON on dedicated fds (stdio stays yours). Bulk spill: private
0700 dir, 0600 files, removed with the bridge.

### Vs pythonia / pythonmonkey / mini-racer / DIY node

Most “JS from Python” libraries are **not Node**. pythonia (PyPI
[`javascript`](https://pypi.org/project/javascript/), JSPyBridge) is the
packaged peer that is: `require()` on import, one child. Bulk is a
**sum** over 1M floats (`.length` on an in-process wrapper is free and
lies). Snapshot:
[`cc_node_vs_alts_20260813.txt`](https://github.com/sreekotay/concurrent-c/blob/main/perf/baselines/cc_node_vs_alts_20260813.txt)
· harness: [`benchmarks/vs_alts.py`](https://github.com/sreekotay/concurrent-c/blob/main/pypi/cc-node/cc_node/benchmarks/vs_alts.py).

| | cc-node | pythonia | DIY JSON stdio | `node -e` each | pythonmonkey | mini-racer |
|---|---|---|---|---|---|---|
| identity RTT | **20µs** | 42µs | 19µs | 23ms | **<1µs** | 104µs |
| callback | **40µs** | 106µs | — | — | 1µs | — |
| 8MB typed / list | **7.5ms shm** / 266ms | — / 481ms | — / 155ms | — | — / 609ms | — / 78ms |
| `require('fs')` | yes | yes | yes | yes | no | no |
| process | child `node` | child `node` | child `node` | new process/call | SpiderMonkey in-process | V8 isolate |

Tiny scalars: pythonmonkey’s in-process SM beats a child. Real Node
(`require('fs')`, native addons, callbacks, stdout stays yours): this
package — ~2× pythonia on identity, shm for bulk, Python callables are
sync (pythonia’s JS side sees a Promise). Isolated `node -e` per call is
~1000× a persistent child. Optional engines SKIP if not importable —
not package deps.

The other direction (Python from Node):
[`concurrent-c-python`](https://www.npmjs.com/package/concurrent-c-python)
vs pymport / ncp / pythonia.

## Surface

- Plain data (numbers, str, bool, `None`, lists, non-empty dicts) by
  value; else a domain-owned handle (attrs, calls, `str()` →
  `String()`). Non-finite floats are tagged.
- Handles are per-domain. `stats()` / `release()` / idempotent
  `destroy()`; afterwards: `bridge is closed`.
- `eval_cell(src, bindings=)` is the notebook door (`%%js`): cwd
  `require` once, optional `globalThis` binds; `eval()` stays one RTT.
  `get()` / `require()` / `eval()` at module level share that session.
- Crash isolation, not a sandbox. `destroy()` is cooperative; an
  in-flight CPU-bound call finishes or you kill the child
  ([`bridge_stress.md`](https://github.com/sreekotay/concurrent-c/blob/main/stress/bridge/bridge_stress.md)).

### Promises

Awaited in the child before the reply — no `async`/`await` on the
Python side. Same honesty as `concurrent-c-python`: a call blocks until
the other runtime answers.

```python
fetchish = js.eval('async (x) => { return { doubled: x * 2 } }')
fetchish(21)   # {'doubled': 42}
```

### Callbacks

```python
mapped = js.eval('(f) => [1, 2, 3].map(f)')(lambda x, *rest: x * 10)
# map passes (value, index, array) — take *rest
```

Exceptions cross both ways with messages intact.

### Buffers

`bytes` / `array.array` / 1-D numpy → typed arrays (and back). Small
inline; large via shm.

```python
import array
total = js.eval('(a) => a.reduce((s, x) => s + x, 0)')
total(array.array('d', range(1_000_000)))
```

## Common issues

**`Cannot find module`.** `require` / `import` resolve from the Python
process cwd (`node_modules` next to your notebook or program), not from
this wheel’s site-packages, and this package does **not** `npm install`
on a miss (pythonia does). `npm install lodash` in that directory is
the fix; or `create(node=…)` / `CC_NODE_BIN` when the wrong Node is on
`PATH`.

### Empty `{}` stays a handle

An empty object has to stay on the Node side — a materialized Python
`dict` would lose later property use that matches Node. So
`js.eval('({})')` returns a live handle:

```python
o = js.eval('({})')                    # JsHandle, not {}
js.eval('(o) => { o.x = 1; return o.x }')(o)   # 1
js.eval('({a: 1})')                    # {'a': 1} — data return
```

Non-empty plain objects still cross as Python `dict`s. Same-domain
handles chain (`h.update(…).digest(…)`).

**Wire cost vs tiny work.** Round trip is ~100µs; a one-line JS helper
on three numbers loses to pure Python. Prefer Python (or a native CC
module) for small/hot work; use the bridge when Node/npm owns the kernel.
Multi-core: `python -m cc_node.benchmarks.multi_domain` (~2.8× on 3
domains here).

## Choosing node

1. `create(node='/path/to/node')`
2. `CC_NODE_BIN`
3. `node` on `PATH`

Packages: cwd `node_modules`, same as Node itself.

From Concurrent-C (not Python): `cc_js_new(false, &a)` hosted
(libnode), or `cc_js_new(true, &a)` for this wire —
[`jsdemo.shcc`](https://github.com/sreekotay/concurrent-c/blob/main/examples/js/jsdemo.shcc).

## Publishing

**PyPI Trusted Publishing (OIDC)** — no API token:

1. [Publishing settings](https://pypi.org/manage/project/concurrent-c-node/settings/publishing/):
   owner `sreekotay`, repo `concurrent-c`, workflow `publish-cc-node.yml`,
   environment `pypi`
2. GitHub Environment `pypi`
3. Bump `pyproject.toml`, push, then:

```
gh workflow run publish-cc-node.yml
```

Both bridges (npm OIDC + PyPI OIDC):

```
./scripts/publish_bridges.sh --publish --minor
# fallbacks: --npm-local / --pypi-twine
```

Examples: `use_node`, `bench_wire`, `benchmarks.multi_domain`,
`benchmarks.vs_alts`.
Jupyter: `from cc_node import require` (or `import cc_node` then `%%js`).
Stress: [`stress/bridge/`](https://github.com/sreekotay/concurrent-c/tree/main/stress/bridge).  
Own hot path in C/CC → native module (40–90ns) —
[JS / Python interop](https://github.com/sreekotay/concurrent-c/blob/main/docs/js-py-modules.md).
