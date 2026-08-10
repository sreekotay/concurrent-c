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
import cc_node

js = cc_node.create()                # always a child `node` process
_ = js.require('lodash')             # cwd node_modules
_.chunk([1, 2, 3, 4, 5], 2)          # [[1, 2], [3, 4], [5]]

semver = js.require('semver')
semver.satisfies('1.2.3', '^1.0.0')  # True

js.destroy()                         # or: with cc_node.create() as js:
```

Unlike [`concurrent-c-python`](https://github.com/sreekotay/concurrent-c/tree/main/npm/cc-python)
(in-process by default), every `create()` here is a separate Node —
real addons, crash isolation, measurable wire. N domains = N processes.

| | this package | CC hosted (`cc_js_new(false, …)`) |
|---|---|---|
| API | `cc_node.create()` | `.ccs` program |
| Where | child `node` | libnode in-process |
| Hot call | ~105µs RTT | sub-µs (needs libnode) |
| Bulk | shm (~9.5ms / 8MB) | in-process |
| Parallelism | N children | one process |
| Crash | child dies; parent lives | shared fate |

```
pip install concurrent-c-node          # needs node on PATH
python -m cc_node.examples.use_node
python -m cc_node.examples.bench_wire
python -m cc_node.benchmarks.multi_domain
```

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
0700 dir, 0600 files, removed with the bridge. Crash isolation, not a
sandbox — don’t eval untrusted JS.

## Surface

- Plain data (numbers, str, bool, `None`, lists, non-empty dicts) by
  value. Empty `{}` stays a live handle. Else: domain-owned handle
  (attrs, calls, `str()` → `String()`). Non-finite floats are tagged.
- Handles stay in one domain. `stats()` / `release()` / idempotent
  `destroy()`; after close: `bridge is closed`. Teardown is cooperative;
  CPU-bound JS isn’t cancelable — wait or kill
  ([`bridge_stress.md`](https://github.com/sreekotay/concurrent-c/blob/main/stress/bridge/bridge_stress.md)).

### Promises

Awaited in the child before the reply — no `async`/`await` on the
Python side:

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
process cwd (`node_modules` next to your program), not from this wheel’s
site-packages. `npm install lodash` in the project directory is the fix;
or `create(node=…)` / `CC_NODE_BIN` when the wrong Node is on `PATH`.
Missing-module errors name that cwd rule.

### Empty `{}` stays a handle

An empty object has to stay on the Node side — a materialized Python
`{}`/`dict` would lose later property use that matches Node. So
`js.eval('({})')` returns a live handle:

```python
o = js.eval('({})')                    # JsHandle, not {}
js.eval('(o) => { o.x = 1; return o.x }')(o)   # 1
js.eval('({a: 1})')                    # {'a': 1} — data return
```

Non-empty plain objects still cross as Python `dict`s. Same-domain
handles chain (`h.update(…).digest(…)`); foreign-domain handles do not.

**Thenables settle in the child.** Promise-based npm APIs need no
`async`/`await` on the Python side — the call blocks until settle (or
raises on reject). Opposite of `concurrent-c-python` isolated, where
every call is already a JS Promise you must await.

**Wire cost vs tiny work.** Round trip is ~100µs; a one-line JS helper
on three numbers loses to pure Python. Prefer Python (or a native CC
module) for small/hot work; use the bridge when Node/npm owns the kernel.
Multi-core: `python -m cc_node.benchmarks.multi_domain` (~2.8× on 3
domains here).

**Crash isolation, not a sandbox.** The child inherits your environment
— don’t eval untrusted JS. `destroy()` is cooperative; CPU-bound JS is
not preemptible (wait or kill + new domain).

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

Local / npm sibling:

```
./scripts/publish_bridges.sh --publish --minor
gh workflow run publish-cc-node.yml
# twine fallback: … --pypi-twine
```

Examples: `use_node`, `bench_wire`, `benchmarks.multi_domain`.  
Stress: [`stress/bridge/`](https://github.com/sreekotay/concurrent-c/tree/main/stress/bridge).  
Own hot path in C/CC → native module (40–90ns) instead of the wire —
[JS / Python interop](https://github.com/sreekotay/concurrent-c/blob/main/docs/js-py-modules.md).
