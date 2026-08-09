# Native modules for Node **and** Python, from one file

[Concurrent-C](https://github.com/sreekotay/concurrent-c) is a strict
C11-superset preprocessor: `.ccs` lowers to plain C and compiles with
your host C compiler.  Write a page of it, get a native module for
either ecosystem — or both from the same file:

```c
#include <ccc/script/py.cch>
#include <ccc/script/js.cch>

typedef struct Counter { long long n; } Counter;

static long long Counter_bump(Counter *self, long long by = 1) {
    self->n += by;
    return self->n;
}

static const Counter seed = { .n = 0 };

@comptime cc_py_export("Counter", &seed);   // → bin/counter.abi3.so
@comptime cc_js_export("Counter", &seed);   // → bin/counter.node
```

Build (one line):

```sh
ccc build counter.ccs        # → bin/counter.node + bin/counter.abi3.so
```

Use in JavaScript:

```js
const counter = require('./bin/counter.node');

counter.bump(4);          // 4
counter.bump({ by: 2 });  // 6 — a trailing object binds arguments by name
```

Use in Python:

```python
import counter            # bin/ on PYTHONPATH

counter.bump(4)           # 4
counter.bump(by=2)        # 6 — real keyword arguments
```

No flag says "module": the TU exports a type and defines no `main`, so
the build links a shared object, and the export names the artifact.
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
| *your* C/CC compute in JS or Python | **a module (this page)** | 40-94ns |
| any *Python package* from Node, in-process | `npm i concurrent-c-python` | ~5µs sync, zero-copy buffers |
| N×numpy, crash isolation, per-domain venvs | `concurrent-c-python` isolated domains | ~125µs RTT, shm bulk |
| any *npm package* from Python | `pip install concurrent-c-node` | ~300µs RTT, shm bulk |

Worked examples: [`examples/recipe_js_module.ccs`](../examples/recipe_js_module.ccs),
[`examples/recipe_py_module.ccs`](../examples/recipe_py_module.ccs),
[`tests/dual_module_export_mod.ccs`](../tests/dual_module_export_mod.ccs)
(dual-target), [`tests/js_module_double_result_mod.ccs`](../tests/js_module_double_result_mod.ccs)
(the gamut).

## Every crossing, measured

One coherent day on a 4-vCPU x86-64 shared VM (node 22, python 3.11,
numpy 2.5; this box swings ±40% run to run — dated baselines with the
exact RESULT lines live under [`perf/baselines/`](../perf/baselines/)).

**Native modules** (this page — your code, in-process, reflected):

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
| wire round trip | ~125µs |
| 8MB argument, shm spill | 6.6ms (base64 wire before it: 153ms — 23x) |
| 4 domains, same numpy workload | 2-4x vs one (3.97x at the box's quietest) |

**`concurrent-c-node`** (any npm package from Python):

| crossing | cost |
|---|---|
| spawn a node child | 28ms |
| wire round trip | 116µs |
| Python-callback round trip (JS → Python → JS) | 238µs |
| 8MB `array('d')` argument, shm spill | 9.2ms (as a JSON list: 583ms — 63x) |

The gradient is the point: **ns** when the code is yours (a module),
**µs** in-process when the package is Python's, **~100µs + shm** when
you want processes between you — and every tier states its costs.
