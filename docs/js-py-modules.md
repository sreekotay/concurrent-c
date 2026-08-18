# JS / Python interop — host or export

Same headers and marshalling; two ownership models:

| | Who owns `main` | What you get |
|------|-----------------|--------------|
| **Hosting** | Concurrent-C | `cc_py_new` / JS host; call foreign packages with UFCS + `!>` |
| **Module export** | Node or CPython | one `.ccs` → `.node` and/or `.abi3.so`; they `require` / `import` your type |

A [more advanced example](#more-advanced-example) keeps the module
stateless: N instances are N host buffers.

**Process bridges** (any foreign package, heavier):

- **Python from Node** — npm [`concurrent-c-python`](https://www.npmjs.com/package/concurrent-c-python) · in-tree [`npm/cc-python`](../npm/cc-python)
- **JavaScript from Python** — pip [`concurrent-c-node`](https://pypi.org/project/concurrent-c-node/) · in-tree [`pypi/cc-node`](../pypi/cc-node)

`destroy()` is cooperative (close + drain); hard child death (`SIGKILL` /
abort) is a separate reject contract. Stress catalog (package peers and
CC embed Waves A–C):
[`stress/bridge/bridge_stress.md`](../stress/bridge/bridge_stress.md)
(`./stress/bridge/run.sh`, including [`cc_embed_stress.ccs`](../stress/bridge/cc_embed_stress.ccs)).

Publish: `./scripts/publish_bridges.sh --publish --minor` bumps, packs,
commits, and dispatches CI OIDC for both registries
(`publish-cc-python.yml` + `publish-cc-node.yml`). Fallbacks:
`--npm-local`, `--pypi-twine`. See
[`npm/cc-python/README.md`](../npm/cc-python/README.md) and
[`pypi/cc-node/README.md`](../pypi/cc-node/README.md).

[Concurrent-C](https://github.com/sreekotay/concurrent-c) is a strict
C11-superset preprocessor: `.ccs` lowers to plain C and compiles with
your host C compiler.

## Hosting — CC owns main

Open an interpreter, import a package, call it. Failures are Results;
attributes are methods; typed destinations extract scalars without an
extra binding.

**Python** — [`examples/py/pydemo.shcc`](../examples/py/pydemo.shcc)
(fuller: [`examples/recipe_py_interop.ccs`](../examples/recipe_py_interop.ccs)):

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
`cc_py_new(true, &a)` opens a **process-isolated** child (probe:
`cc_py_proc_available()`); objects stay remote handles / scalars — never
shared `PyObject*`. Same-process homes refuse foreign-home object args by
name; `obj.clone_into(&other)` pickle-copies between inproc interpreters
(process-isolated targets refuse). Unsigned inbound and `as_list` /
narrow integer destinations refuse out-of-range values rather than wrap.
Costs: [`perf/py_baseline.ccs`](../perf/py_baseline.ccs) ·
[`perf/baselines/py_baseline_20260809.txt`](../perf/baselines/py_baseline_20260809.txt).

Sync CC→Python callbacks — hand a CC function to Python as a callable
(`py_fn(fn, userdata)`). Integer userdata is by value; pointers use the
pointer host ABI. The host fn returns `CCPyObj !>(CCPyError)` — Ok via
`cc_py_i64` / friends, Err via `cc_py_host_error`. Works in-process
(minted `PyCFunction`) and process-isolated (`{$f}` wire).

```c
static CCPyObj !>(CCPyError) score(long long bias, const CCPyObj *args,
                                   int argc) {
    @errhandler(CCPyError e) { return cc_err(e); }
    if (argc < 1) return cc_err(cc_py_host_error("score: need one arg"));
    long long x = cc_py_obj_as_i64((CCPyObj *)&args[0]) !>;
    return cc_ok(cc_py_i64(args[0].home, x + bias));
}

py.exec(@slice(
    "def map_score(f, xs):\n"
    "    return [f(x) for x in xs]\n")) !>;
CCPyObj g = py.import("__main__") !> @destroy;
long long xs[:] = {1, 2, 3};
CCPyObj ys = g.map_score(py_fn(score, 40), xs) !> @destroy;
```

Smoke: [`tests/py_fn_smoke.ccs`](../tests/py_fn_smoke.ccs). Costs:
[`perf/py_fn_baseline.ccs`](../perf/py_fn_baseline.ccs) ·
[`perf/baselines/py_fn_baseline_20260810.txt`](../perf/baselines/py_fn_baseline_20260810.txt).

**JavaScript** — same UFCS surface and lifetime rule; transport is the
flag, mirroring `concurrent-c-python`'s `create()` /
`create({isolated: true})`:

```c
CCJsDom js = cc_js_new(false, &a) !> @destroy;   /* in-process node   */
CCJsDom js = cc_js_new(true,  &a) !> @destroy;   /* node child        */

double v = js.eval("Math.sqrt(2)")!>.as_f64() !>;
CCJsDomVal os = js.require("os") !> @destroy;
CCSlice plat = os.platform()!>.as_slice(&a) !>;
long long cpus = os.availableParallelism() !>;
```

**Hosted** (`false`) embeds Node in-process — sub-µs ops; one per
process (V8's rule); needs libnode-dev; first use compiles a small
cached shim. macOS Homebrew ships `node.h` but not libnode, so hosted
returns `libnode not found` — use isolated there.

**Isolated** (`true`) spawns a `node` child on the `concurrent-c-node`
wire (~100–170µs/hop; N domains, separate heaps, crash isolation; needs
`node` on PATH). `require` resolves against the working directory in
both. Thenables: isolated awaits on the wire; hosted returns a handle
(no loop-block await). Isolated typed arrays travel as inline `$ta`/`b64`.

Sync JS→CC callbacks — `js_fn(fn, userdata)` (hosted napi mint +
isolated `{$f}` wire; integers by value, pointers via
`CCJsDomHostUserdata`). Host fns return `CCJsDomVal !>(CCJsError)` — Ok
via `cc_js_i64` / friends, Err via `cc_js_host_error`.

```c
static CCJsDomVal !>(CCJsError) score(long long bias, const CCJsDomVal *args,
                                      int argc) {
    @errhandler(CCJsError e) { return cc_err(e); }
    if (argc < 1) return cc_err(cc_js_host_error("score: need one arg"));
    long long x = cc_js_dom_val_as_i64((CCJsDomVal *)&args[0]) !>;
    return cc_ok(cc_js_i64(args[0].dom, x + bias));
}

js.exec("globalThis.mapScore = (f, xs) => xs.map(f)") !>;
CCJsDomVal g = js.eval("globalThis") !> @destroy;
long long xs[:] = {1, 2, 3};
CCJsDomVal ys = g.mapScore(js_fn(score, 40), xs) !> @destroy;
```

Smoke: [`tests/js_dom_cb_smoke.ccs`](../tests/js_dom_cb_smoke.ccs). Costs:
[`perf/js_fn_baseline.ccs`](../perf/js_fn_baseline.ccs) ·
[`perf/baselines/js_fn_baseline_20260810.txt`](../perf/baselines/js_fn_baseline_20260810.txt).

**Host callbacks — limits** (refuse by name / Result / throw):

- Sync only — host ABI is `T !>(E)`, not a Promise.
- JS isolated typed arrays: inline ≤64KiB; larger SHM spill refuses.
- Python isolated: typed-array spill, kwargs, and `exec`/`eval` source refuse.
- Returning a host fn from a callback refuses.
- Hosted JS needs libnode; Dom ops nested inside a hosted trampoline
  refuse (`not reentrant` — use materialized args + Ok mints).

Wire: dedicated fds, id-paired replies — stdio stays yours. Crash
isolation, not a sandbox: the child inherits your environment — don't
run untrusted code. Stress:
[`stress/bridge/bridge_stress.md`](../stress/bridge/bridge_stress.md)
(§ CC embed interop waves).

```sh
ccc examples/js/jsdemo.shcc               # hosted when libnode exists, else isolated
ccc run examples/recipe_js_isolated.ccs   # N domains + crash isolation, measured
ccc run examples/recipe_js_host.ccs       # raw loop-thread door (zero-overhead tier)
```

Sources: [`examples/js/jsdemo.shcc`](../examples/js/jsdemo.shcc) ·
[`examples/recipe_js_host.ccs`](../examples/recipe_js_host.ccs) ·
guest: [`examples/js/jsdemo_mod.ccs`](../examples/js/jsdemo_mod.ccs).
Shim cache: `~/.cache/concurrent-c/js-host` (Debian/Ubuntu:
`apt install libnode-dev`). From Python, any npm package goes through
[`concurrent-c-node`](../pypi/cc-node)
(`from cc_node import require` in Jupyter or Colab; `%%js` on the same
session; one child, same calling convention).

## Module export — Node / Python own main

One `.ccs` → native modules for either ecosystem (or both), several
classes at a time:

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

```sh
ccc build counters.ccs   # → bin/counters.node + bin/counters.abi3.so (same bytes)
```

JavaScript — one `require`; classes under snake-case names (single-export
stays flat):

```js
const m = require('./bin/counters.node');

m.counter.bump(4);          // 4
m.counter.bump({ by: 2 });  // 6 — trailing object binds by name
m.stats.add(3); m.stats.add(5);
m.stats.mean();             // 4
```

Python — same shape:

```python
import counters           # bin/ on PYTHONPATH

counters.counter.bump(4)      # 4
counters.counter.bump(by=2)   # 6 — real keywords
counters.stats.add(3); counters.stats.add(5)
counters.stats.mean()         # 4.0
```

No "module" flag: the TU exports types and has no `main`, so the build
links a shared object. The module name is the directive's first
argument — always explicit. A TU may publish several modules (different
first arguments) in one build; the loaded name selects
(`PyInit_<name>` / required basename). Each registration copies the seed
into a fresh instance, so shared classes never share state.

Measured (4-vCPU x86-64, node 22 / python 3.11):

| | |
|---|---|
| call from Node | **40ns** (~130× the generic `concurrent-c-python` bridge's 5.3µs crossing) |
| call from Python | **68ns** |
| 16-element `Float64Array` → zero-copy slice → sum | **94ns** |
| 1M-element slice sum | 1.3ms (memory-bound C loop) |
| artifact | **26KB `.node`** / 35KB `.abi3.so`, libc-only, dead-stripped, one exported symbol |

One `.node` loads in any Node-API host (Node, Electron, Bun, Deno);
`.abi3.so` is stable-ABI CPython (3.x, no per-version builds). Dual-target
builds one object under two names (hardlinked); `--module=py` /
`--module=js` narrows to one.

## Rules

**The module is the type.** Every visible function whose first parameter
is `T` or `T*` becomes a module function; state is one `T`, seeded by
the exported pointer. The rest follows from C:

- `long long by = 1` — default argument. JS may pass a trailing plain
  object by name (`c.bump({by: 2})`); Python gets keywords
  (`counter.bump(by=2)`). A trailing plain object binds by name only
  when every key names a remaining parameter; any unmatched key
  refuses. Place an options-bag-shaped parameter before defaulted
  ones so a legitimate object argument is not read as a keyword bag.
- `Counter__clamp` (double underscore after the type) reflects as
  `_clamp` and is **not exported** — leading `_` is privacy at the
  boundary. Wrap-and-export if you want a public name
  (`Counter_clamp` → one line that calls `Counter__clamp`).
- Fallible methods (`!>(CCError)`) cross as the mapped exception —
  `CC_ERR_INVALID_ARG` → `TypeError` (JS) / `ValueError` (Python);
  message intact, `code` carries the kind.

**Instances and threads:** one `T` **per realm**, not per process — each
Node `worker_thread` and each Python subinterpreter gets its own
instance. Within a realm, calls serialize (Node is single-threaded; Python
trampolines hold the GIL for the call). You still own: C globals across
realms, and threads you start inside a call that touch `T` (or a
`double[:]` borrow — lease is exactly the call) after return.
Free-threaded CPython is out of scope. Multiple instances in one realm:
hold them in your `T` (handle-passing), as a C library would — or leave
`T` empty and keep each instance in a host buffer ([more advanced
example](#more-advanced-example)).

## Buffers

```c
static double Sig_sum(Sig *self, double[:] xs) {          // Float64Array /
    double acc = 0;                                       // numpy float64 /
    for (size_t i = 0; i < xs.base.len; i++)              // array.array('d')
        acc += ((double *)xs.base.ptr)[i];
    return acc;
}
static void Sig_fill(Sig *self, double[:] xs, double v);  // writes land in
                                                          // the caller's buffer
static double[:] Sig_row(Sig *self);                      // fresh TypedArray
                                                          // / typed memoryview
```

Matching `Float64Array` (JS) or a contiguous buffer exporter of the right
element type (Python: numpy `float64`, `array.array('d')`, memoryview)
borrows zero-copy for the call; plain `Array` / `list` or a mismatched
dtype converts per element. Returns materialize a fresh buffer on each
side (TypedArray / typed memoryview). The 94ns sum above is the JS
borrow path; Python is the same shape (stdlib `array.array` in
`tests/py_module_double_result_mod.ccs`).

## Calling back out

Methods may take `CCJsVal` / `CCPyObj` and call through with the same
UFCS surface (`obj.step(21) !>`). Errors cross with messages. Full
surface: `tests/js_module_double_result_mod.ccs` (slices, kwargs, errors,
outbound objects, exact BigInt for Python ints past 2^53).

## Plain C

Any C in the TU is available to methods; libraries link with
`@link("m")`-style directives; existing `.c`/`.h` can sit next to the
exported type. Reflection only looks at `T`-first functions — if you can
call it from C, you can export it.

## Which tool, when

| you want | use | cost per call |
|---|---|---|
| call *Python packages* from CC | **hosting** — [`pydemo.shcc`](../examples/py/pydemo.shcc) / [`recipe_py_interop.ccs`](../examples/recipe_py_interop.ccs) | see `py_baseline` |
| call *into JS* from CC (guest) | [`jsdemo.shcc`](../examples/js/jsdemo.shcc) (Node owns env; same UFCS) | napi trampoline |
| *your* C/CC compute in JS or Python | **module export** | 40–94ns |
| any *Python package* from Node, in-process | `npm i concurrent-c-python` | ~5µs sync, zero-copy buffers |
| N×numpy, crash isolation, per-domain venvs | `concurrent-c-python` isolated domains | ~100µs RTT, shm bulk |
| any *npm package* from Python | `pip install concurrent-c-node` | ~105µs RTT, shm bulk |

Same-machine vs pythonia / DIY `node` / pythonmonkey / mini-racer:
[`cc_node.benchmarks.vs_alts`](../pypi/cc-node/cc_node/benchmarks/vs_alts.py)
([`cc_node_vs_alts_20260813.txt`](../perf/baselines/cc_node_vs_alts_20260813.txt)).
Engines that are not Node can win a scalar call and still fail `require('fs')`.

**Hosting demos:** [`examples/py/pydemo.shcc`](../examples/py/pydemo.shcc),
[`examples/js/jsdemo.shcc`](../examples/js/jsdemo.shcc),
[`examples/recipe_py_interop.ccs`](../examples/recipe_py_interop.ccs).

**Module export:** [`examples/recipe_js_module.ccs`](../examples/recipe_js_module.ccs),
[`examples/recipe_py_module.ccs`](../examples/recipe_py_module.ccs),
[`tests/dual_module_export_mod.ccs`](../tests/dual_module_export_mod.ccs),
[`tests/js_module_double_result_mod.ccs`](../tests/js_module_double_result_mod.ccs),
[more advanced example](#more-advanced-example) (stateless module, host buffer).

## Measured

One day on a 4-vCPU x86-64 shared VM (node 22, python 3.11, numpy 2.5;
±40% run to run). Dated `RESULT` lines under
[`perf/baselines/`](../perf/baselines/), catalogued in
[`perf/baselines/README.md`](../perf/baselines/README.md):
[`js_py_modules_20260809.txt`](../perf/baselines/js_py_modules_20260809.txt),
[`js_baseline_node_20260809.txt`](../perf/baselines/js_baseline_node_20260809.txt),
[`py_baseline_20260809.txt`](../perf/baselines/py_baseline_20260809.txt),
[`py_bind_micro_20260811.txt`](../perf/baselines/py_bind_micro_20260811.txt).

**Hosting** (CC owns main — Python packages in-process):

| crossing | cost |
|---|---|
| see [`py_baseline_20260809.txt`](../perf/baselines/py_baseline_20260809.txt) | embed / call / buffer paths |
| Python→CC `py_fn` (in-process) | **~95ns** (~3× a Python function call) — [`py_fn_baseline_20260810.txt`](../perf/baselines/py_fn_baseline_20260810.txt) |
| Python→CC `py_fn` (isolated wire) | **~20µs**/call (CC→`operator.call`→host fn) |
| JS→CC `js_fn` (hosted napi mint) | same shape as in-process `py_fn` (mint + trampoline; measure with libnode) |
| JS→CC `js_fn` (isolated wire) | **~8µs**/call (~120× a JS function on the same wire) — [`js_fn_baseline_20260810.txt`](../perf/baselines/js_fn_baseline_20260810.txt) |

**Native modules** (export — your code, in-process):

| crossing | cost |
|---|---|
| Node → CC call | **40ns** |
| Python → CC call | **68ns** ([`js_py_modules_20260809.txt`](../perf/baselines/js_py_modules_20260809.txt)) |
| Python → CC `py_module` vs nanobind (720-method func suite) | **37.7ns** vs **35.6ns** (1.06×); compile 0.44×; size 0.98× — [`py_bind_micro_20260811.txt`](../perf/baselines/py_bind_micro_20260811.txt) |
| Node → CC, 16-elem `Float64Array` zero-copy borrow + sum | 94ns |
| Node → CC, 1M-elem slice sum | 1.3ms (memory-bound) |
| artifact | 26KB `.node` / 35KB `.abi3.so` |

**`concurrent-c-python`, in-process** (any Python package from Node, zero-copy):

Typed-array args are writable leases for the call; `proxy.toTypedArray()`
copies 1-D numeric results out. Host callbacks receive Proxies (missing
attrs throw) and buffer args as real TypedArrays (lease→callback copies).

| crossing | cost |
|---|---|
| sync call, 16-elem dot (the crossing itself) | 5.3µs |
| 1M-elem `np.dot`, zero-copy lease | 239µs — 5.7× the JS loop (best recorded 158µs / 8.45×) |
| lane (task) call, 1M dot | 232µs — off-thread |
| lane pipelined, 16-elem | 11µs |
| event-loop liveness during bulk numpy | 99 ticks/100ms via lane, 0 sync |
| JS ∥ numpy overlap (balanced) | 1.81× |
| numpy ∥ numpy, one interpreter, BLAS pinned | 1.60–2.02× |

**`concurrent-c-python`, isolated domains** (full CPython per child).
Default calls block the JS thread (same convention as in-process);
`py.task` overlaps children and keeps the event loop live.

| crossing | cost |
|---|---|
| spawn + import numpy | ~100–440ms (warm/cold) |
| wire round trip | ~100µs |
| 8MB argument, shm spill | 6.4ms (base64 wire before it: 153ms — 24×) |
| 4 domains, same numpy workload | 2–4× vs one (3.97× at the box's quietest) |

Head-to-head in-process vs isolated vs JS (dot / matmul / SVD):
[`npm/cc-python/benchmarks/modes_bench.js`](../npm/cc-python/benchmarks/modes_bench.js)
·
[`perf/baselines/cc_python_modes_bench_20260810.txt`](../perf/baselines/cc_python_modes_bench_20260810.txt).

| workload | in-process | isolated | JS |
|---|---|---|---|
| `sqrt` ×1 | ~3µs | ~21µs | — |
| `np.dot` 1M | 0.28ms | 10ms | 0.72ms |
| matmul 128 | 0.04ms | 0.41ms | 3.3ms |
| matmul 256 | 0.13ms | 0.99ms | 17.5ms |
| SVD 256 | 3.1ms | 3.4ms | — |
| 3 isolated domains | — | 2.8× seq | — |

BLAS-1 (`np.dot`) often loses to tight JS over the isolated wire; BLAS-3
matmul crosses over (~n≥128 vs naive JS here); SVD@256 is nearly tied
with in-process because the kernel dominates.

**`concurrent-c-node`** (any npm package from Python):

| crossing | cost |
|---|---|
| spawn a node child | 28ms |
| wire round trip | 105µs |
| Python-callback round trip (JS → Python → JS) | 153µs |
| 8MB `array('d')` argument, shm spill | 9.5ms (as a JSON list: 499ms — 52×) |

Costs by tier: **ns** for your own module, **µs** in-process for a Python
package, **~100µs + shm** when you want processes between you.

## More advanced example

The module is a service; the state is the caller's.

Running mean/variance (Welford). The module owns no state:
`st = [n, mean, m2]` lives in the caller's buffer — numpy float64,
`array.array('d')`, or `Float64Array` — borrowed zero-copy for exactly
the call, mutated in place. N instances = N host buffers; the host's GC
owns liveness; pickle / `structuredClone` work because the state is
host-real. The seed is empty: `Wf` is scratch, not data. The state
must be a typed buffer — a converted list discards the in-place
update.

```c
#include <ccc/script/py.cch>
#include <ccc/script/js.cch>

typedef struct Wf { char _; } Wf;

static void !>(CCError) Wf_step(Wf *self, double[:] st, double x) {
    if (st.base.len < 3) return cc_err(CC_ERR_INVALID_ARG, "state needs 3 doubles");
    double *s = (double *)st.base.ptr;
    double n = s[0] + 1.0, d = x - s[1];
    double mean = s[1] + d / n;
    s[0] = n;  s[1] = mean;  s[2] += d * (x - mean);
    return cc_ok();
}
static double Wf_mean(Wf *self, double[:] st) {
    return ((double *)st.base.ptr)[1];
}
static double !>(CCError) Wf_variance(Wf *self, double[:] st) {
    double *s = (double *)st.base.ptr;
    if (s[0] < 2.0) return cc_err(CC_ERR_INVALID_ARG, "need two samples");
    return cc_ok(s[2] / (s[0] - 1.0));
}

static const Wf seed = {0};
@comptime cc_py_export("welford", "Wf", &seed);
@comptime cc_js_export("welford", "Wf", &seed);
```

```sh
ccc build welford.ccs   # → bin/welford.node + bin/welford.abi3.so
```

Python — the class the first-minute user was looking for, ten lines:

```python
import array, welford

class Welford:
    def __init__(self):
        self.st = array.array('d', [0.0, 0.0, 0.0])   # real Python state:
    def step(self, x):                                 # picklable, copyable,
        welford.wf.step(self.st, x)                    # debugger-visible
    @property
    def mean(self):     return welford.wf.mean(self.st)
    @property
    def variance(self): return welford.wf.variance(self.st)  # ValueError if n < 2
```

JS — same shape, same count:

```js
const m = require('./bin/welford.node');

class Welford {
  st = new Float64Array(3);                    // real JS state: structuredClone,
  step(x)        { m.wf.step(this.st, x); }    // devtools, GC all just work
  get mean()     { return m.wf.mean(this.st); }
  get variance() { return m.wf.variance(this.st); }  // TypeError before 2 samples
}
```

[`real_projects/levenshtein`](../real_projects/levenshtein) is this
pattern at package scale — stateless service, host-owned data, judged
at parity by the package it reimplements.
