# JS / Python interop — host **or** export

One boundary, two doors (same headers, same marshalling):

| Door | Who owns `main` | What you get |
|------|-----------------|--------------|
| **Hosting** | Concurrent-C | `cc_py_new` / (JS) host an engine; call foreign packages with UFCS + `!>` |
| **Module export** | Node or CPython | one `.ccs` → `.node` and/or `.abi3.so`; they `require` / `import` your type |

**Process bridges** (any foreign package, heavier):

- **Python from Node** — npm [`concurrent-c-python`](https://www.npmjs.com/package/concurrent-c-python) · in-tree [`npm/cc-python`](../npm/cc-python)
- **JavaScript from Python** — pip [`concurrent-c-node`](https://pypi.org/project/concurrent-c-node/) · in-tree [`pypi/cc-node`](../pypi/cc-node)

`destroy()` on these bridges is cooperative (close + drain); hard child
death (`SIGKILL` / abort) is a separate reject contract. Stress catalog
(package peers **and** CC embed Waves A–C):
[`stress/bridge/bridge_stress.md`](../stress/bridge/bridge_stress.md)
(`./stress/bridge/run.sh`, including [`cc_embed_stress.ccs`](../stress/bridge/cc_embed_stress.ccs)).

Publish both (bump patch versions, pack, upload):
`./scripts/publish_bridges.sh --publish` (pack only: omit `--publish`;
`--minor` / `--major` / `--no-bump` optional).

[Concurrent-C](https://github.com/sreekotay/concurrent-c) is a strict
C11-superset preprocessor: `.ccs` lowers to plain C and compiles with
your host C compiler.

## Hosting — CC owns main

Open an interpreter, import a package, call it. Failures are Results;
attributes are methods; typed destinations extract scalars without an
extra binding.

**Python** — script form [`examples/py/pydemo.shcc`](../examples/py/pydemo.shcc)
(fuller tour: [`examples/recipe_py_interop.ccs`](../examples/recipe_py_interop.ccs)):

```c
#!/usr/bin/env -S ./cc/bin/ccc
#include <ccc/script/py.cch>

CCPy py = cc_py_new(false, &a) !> @destroy;

CCPyObj math = py.import("math") !> @destroy;
double v = math.sqrt(2.0) !>;
double pv = math.pow(2.0, 10) !>;

CCPyObj stats = py.import("statistics") !> @destroy;
double xs[:] = {1.0, 2.0, 3.0, 4.0, 42.0};
double mv = stats.mean(xs) !>;

CCSlice vs = py.import("sys")!>.get("version")!>.as_slice()!>;
```

```sh
ccc examples/py/pydemo.shcc
# or:  ccc run examples/recipe_py_interop.ccs
```

Probe with `cc_py_available()` when you want a clean skip without libpython.
Costs for this door: [`perf/py_baseline.ccs`](../perf/py_baseline.ccs) ·
[`perf/baselines/py_baseline_20260809.txt`](../perf/baselines/py_baseline_20260809.txt).

**JavaScript** — same UFCS call surface, same lifetime rule, one
constructor with the transport as the flag, mirroring
`concurrent-c-python`'s `create()` / `create({isolated: true})`:

```c
CCJsDom js = cc_js_new(false, &a) !> @destroy;   /* in-process node   */
CCJsDom js = cc_js_new(true,  &a) !> @destroy;   /* node child        */

double v = js.eval("Math.sqrt(2)")!>.as_f64() !>;
CCJsDomVal os = js.require("os") !> @destroy;
CCSlice plat = os.platform()!>.as_slice(&a) !>;
long long cpus = os.availableParallelism() !>;
```

The flag at the call site is the boundary, because the crossing
profiles differ: **hosted** (`false`) embeds a full Node — V8, libuv,
npm modules — in your process (sub-µs ops; one per process, V8's rule;
needs libnode-dev, first use compiles a small cached shim; macOS
Homebrew ships `node.h` but not libnode, so hosted returns
`libnode not found` — use isolated there);
**isolated** (`true`) spawns a `node` child per handle on the
`concurrent-c-node` wire (~100-170µs/hop; N domains, separate heaps, crash
isolation; needs only `node` on PATH).  Same ops, same materialization
rules either way; `require` resolves against the working directory in
both, so `npm install` next to your program is the whole setup.

The wire lives on dedicated fds with id-paired replies — stdio stays
the user's, so `console.log` in evaluated code reaches the real stdout
and can never collide with (or forge) a protocol reply.  Isolated is
crash isolation, not a security sandbox: the child inherits your
environment and privileges — do not run untrusted code through it.

```sh
ccc examples/js/jsdemo.shcc               # hosted when libnode exists, else isolated
ccc run examples/recipe_js_isolated.ccs   # N domains + crash isolation, measured
ccc run examples/recipe_js_host.ccs       # the raw loop-thread door (zero-overhead tier)
```

Sources: [`examples/js/jsdemo.shcc`](../examples/js/jsdemo.shcc) ·
[`examples/recipe_js_host.ccs`](../examples/recipe_js_host.ccs) ·
guest mode: [`examples/js/jsdemo_mod.ccs`](../examples/js/jsdemo_mod.ccs).  First use
compiles a small embedder shim against the node development headers
(Debian/Ubuntu: `apt install libnode-dev`) and caches it under
`~/.cache/concurrent-c/js-host`.  From Python, any-npm-package goes
through the process bridge [`concurrent-c-node`](../pypi/cc-node).

## Module export — Node / Python own main

Write a page of Concurrent-C, get native modules for either ecosystem —
or both from the same file, several classes at a time:

```c
#include <ccc/script/py.cch>
#include <ccc/script/js.cch>

typedef struct Counter { long long n; } Counter;
static long long Counter_bump(Counter *self, long long by = 1) {
    return self->n += by;
}

typedef struct Stats { double sum; long long n; } Stats;
static void Stats_add(Stats *self, double x) { self->sum += x; self->n++; }
static double Stats_mean(Stats *self) {
    return self->n ? self->sum / (double)self->n : 0;
}

static const Counter cseed = { .n = 0 };
static const Stats sseed = { 0 };

@comptime cc_py_export("counters", "Counter", &cseed);
@comptime cc_py_export("counters", "Stats",   &sseed);
@comptime cc_js_export("counters", "Counter", &cseed);
@comptime cc_js_export("counters", "Stats",   &sseed);
```

Build (one line):

```sh
ccc build counters.ccs   # → bin/counters.node + bin/counters.abi3.so (same bytes)
```

Use in JavaScript — one `require`, each class namespaced under its
snake-case name (a single-export module stays flat):

```js
const m = require('./bin/counters.node');

m.counter.bump(4);          // 4
m.counter.bump({ by: 2 });  // 6 — a trailing object binds arguments by name
m.stats.add(3); m.stats.add(5);
m.stats.mean();             // 4
```

Use in Python — same shape, `import` the module, classes namespaced
inside (a single-class module stays flat there too):

```python
import counters           # bin/ on PYTHONPATH

counters.counter.bump(4)      # 4
counters.counter.bump(by=2)   # 6 — real keyword arguments
counters.stats.add(3); counters.stats.add(5)
counters.stats.mean()         # 4.0
```

No flag says "module": the TU exports types and defines no `main`, so
the build links a shared object.  The module name is the directive's
first argument, always explicit — never a file name or declaration
order.  A TU may publish SEVERAL modules (different first arguments):
all of them live in one build, and the loaded name selects the module
— the `PyInit_<name>` entry symbol on the Python side, the required
basename on the JS side.  Each registration copies the seed into a
fresh instance, so two modules sharing a class never share state.
What you get, measured (4-vCPU x86-64, node 22 / python 3.11):

| | |
|---|---|
| call from Node | **40ns** (~130x the generic `concurrent-c-python` bridge's 5.3µs crossing) |
| call from Python | **68ns** |
| 16-element `Float64Array` → zero-copy slice → sum | **94ns** |
| 1M-element slice sum | 1.3ms (memory-bound C loop) |
| artifact | **26KB `.node`** / 35KB `.abi3.so`, libc-only, dead-stripped, one exported symbol |

One `.node` loads in any Node-API host (Node, Electron, Bun, Deno);
the `.abi3.so` is a stable-ABI CPython extension (3.x, no per-version
builds).  A dual-target TU builds ONE object under two names
(hardlinked — the bytes are identical, each embedding resolves its
runtime lazily); `--module=py` / `--module=js` narrows to one.

## The rules (there is one)

**The module IS the type.**  Every visible function whose first
parameter is `T` or `T*` becomes a module function; the module's state
is one `T`, seeded by the pointer you export.  Everything else follows
from C:

- `long long by = 1` — a default argument.  JS may also pass a trailing
  plain object binding by name (`c.bump({by: 2})`); Python gets real
  keywords (`counter.bump(by=2)`).
- `Counter__clamp` (double underscore) reflects as `_clamp` — internal.
  Wrap-and-export is the rename story; the wrapper is one line.
- A fallible method (`!>(CCError)`) crosses as the exception the error
  KIND maps to — `CC_ERR_INVALID_ARG` is a `TypeError` in JS and a
  `ValueError` in Python — message intact, `code` carrying the kind.

**Instances and threads** (the contract, so you don't have to guess):
the state is one `T` **per realm**, not per process — every Node
`worker_thread` that requires the module gets a fresh `T` (freed with
its environment), and every Python subinterpreter gets its own module
instance.  Within a realm you are never entered concurrently: a Node
environment runs JS on one thread, and the Python trampolines hold the
GIL for the whole call, so calls on one `T` serialize — each call is
atomic with respect to the others.  What remains yours: C globals you
share across realms, and threads you start inside a call that touch
`T` (or a `double[:]` borrow — the lease is exactly the call) after
the call returns.  Free-threaded (no-GIL) CPython is out of scope.
Multiple independent instances inside one realm is not a module-export
story today — your `T` holds them (handle-passing), the same way a C
library would.

## Buffers are zero-copy borrows

```c
static double Sig_sum(Sig *self, double[:] xs) {          // Float64Array
    double acc = 0;                                       // borrows in place
    for (size_t i = 0; i < xs.base.len; i++)
        acc += ((double *)xs.base.ptr)[i];
    return acc;
}
static void Sig_fill(Sig *self, double[:] xs, double v);  // writes land in
                                                          // the caller's array
static double[:] Sig_row(Sig *self);                      // returns materialize
                                                          // as a fresh one
```

A matching `Float64Array` (or numpy array / buffer on the Python side)
borrows zero-copy for the call; a plain `Array` or mismatched dtype
converts per element.  That 94ns sum above is this path.

## Calling back out

A method may take `CCJsVal` / `CCPyObj` — a live host value — and call
through it with the same UFCS surface the embedding headers give
everywhere else (`obj.step(21) !>`).  Errors cross back with their
messages.  See `tests/js_module_double_result_mod.ccs` for the full
gamut: slices, kwargs, errors, outbound objects, BigInt-range ints.

## Plain C rides along

Concurrent-C is a C superset compiled by your host C compiler: any C
function in the TU is available to your methods, any C library links
with `@link("m")`-style directives, and existing `.c`/`.h` code can sit
next to the exported type unchanged.  If you can call it from C, you
can export it to Python and Node — the reflection only looks at the
`T`-first functions.

## Which tool, when

| you want | use | cost per call |
|---|---|---|
| call *Python packages* from CC | **hosting** — [`pydemo.shcc`](../examples/py/pydemo.shcc) / [`recipe_py_interop.ccs`](../examples/recipe_py_interop.ccs) | see `py_baseline` |
| call *into JS* from CC (guest) | [`jsdemo.shcc`](../examples/js/jsdemo.shcc) (Node owns env; same UFCS) | napi trampoline |
| *your* C/CC compute in JS or Python | **module export** (below) | 40-94ns |
| any *Python package* from Node, in-process | `npm i concurrent-c-python` | ~5µs sync, zero-copy buffers |
| N×numpy, crash isolation, per-domain venvs | `concurrent-c-python` isolated domains | ~100µs RTT, shm bulk |
| any *npm package* from Python | `pip install concurrent-c-node` | ~300µs RTT, shm bulk |

**Hosting / call-out demos:** [`examples/py/pydemo.shcc`](../examples/py/pydemo.shcc)
(Python, CC owns main), [`examples/js/jsdemo.shcc`](../examples/js/jsdemo.shcc)
(JS guest, Node owns main), [`examples/recipe_py_interop.ccs`](../examples/recipe_py_interop.ccs).

**Module export:** [`examples/recipe_js_module.ccs`](../examples/recipe_js_module.ccs),
[`examples/recipe_py_module.ccs`](../examples/recipe_py_module.ccs),
[`tests/dual_module_export_mod.ccs`](../tests/dual_module_export_mod.ccs)
(dual-target), [`tests/js_module_double_result_mod.ccs`](../tests/js_module_double_result_mod.ccs)
(the gamut).

## Every crossing, measured

One coherent day on a 4-vCPU x86-64 shared VM (node 22, python 3.11,
numpy 2.5; this box swings ±40% run to run — dated baselines with the
exact RESULT lines live under [`perf/baselines/`](../perf/baselines/),
catalogued in [`perf/baselines/README.md`](../perf/baselines/README.md):
[`js_py_modules_20260809.txt`](../perf/baselines/js_py_modules_20260809.txt),
[`js_baseline_node_20260809.txt`](../perf/baselines/js_baseline_node_20260809.txt),
[`py_baseline_20260809.txt`](../perf/baselines/py_baseline_20260809.txt)).

**Hosting** (CC owns main — Python packages in-process):

| crossing | cost |
|---|---|
| see [`py_baseline_20260809.txt`](../perf/baselines/py_baseline_20260809.txt) | embed / call / buffer paths |

**Native modules** (export — your code, in-process, reflected):

| crossing | cost |
|---|---|
| Node → CC call | **40ns** |
| Python → CC call | **68ns** |
| Node → CC, 16-elem `Float64Array` zero-copy borrow + sum | 94ns |
| Node → CC, 1M-elem slice sum | 1.3ms (memory-bound) |
| artifact | 26KB `.node` / 35KB `.abi3.so` |

**`concurrent-c-python`, in-process** (any Python package from Node, zero-copy):

| crossing | cost |
|---|---|
| sync call, 16-elem dot (the crossing itself) | 5.3µs |
| 1M-elem `np.dot`, zero-copy lease | 239µs — 5.7x the JS loop (best recorded 158µs / 8.45x) |
| lane (task) call, 1M dot | 232µs — off-thread for free |
| lane pipelined, 16-elem | 11µs |
| event-loop liveness during bulk numpy | 99 ticks/100ms via lane, 0 sync |
| JS ∥ numpy overlap (balanced) | 1.81x |
| numpy ∥ numpy, one interpreter, BLAS pinned | 1.60-2.02x |

**`concurrent-c-python`, isolated domains** (full CPython per child):

| crossing | cost |
|---|---|
| spawn + import numpy | ~100-440ms (warm/cold) |
| wire round trip | ~100µs |
| 8MB argument, shm spill | 6.4ms (base64 wire before it: 153ms — 24x) |
| 4 domains, same numpy workload | 2-4x vs one (3.97x at the box's quietest) |

**`concurrent-c-node`** (any npm package from Python):

| crossing | cost |
|---|---|
| spawn a node child | 28ms |
| wire round trip | 105µs |
| Python-callback round trip (JS → Python → JS) | 153µs |
| 8MB `array('d')` argument, shm spill | 9.5ms (as a JSON list: 499ms — 52x) |

The gradient is the point: **ns** when the code is yours (a module),
**µs** in-process when the package is Python's, **~100µs + shm** when
you want processes between you — and every tier states its costs.
