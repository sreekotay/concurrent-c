# Native modules for Node **and** Python, from one file

Write a page of Concurrent-C, get a native module for either ecosystem
— or both from the same file:

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

```sh
ccc build counter.ccs
node -e "const c = require('./bin/counter.node'); console.log(c.bump(4))"
PYTHONPATH=bin python3 -c "import counter; print(counter.bump(4))"
```

No flag says "module": the TU exports a type and defines no `main`, so
the build links a shared object, and the export names the artifact.
What you get, measured (4-vCPU x86-64, node 22 / python 3.11):

| | |
|---|---|
| call from Node | **40ns** (~130x the generic `cc-python` bridge's 5.3µs crossing) |
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
| any *Python package* from Node, in-process | `npm i cc-python` | ~5µs sync, zero-copy buffers |
| N×numpy, crash isolation, per-domain venvs | `cc-python` isolated domains | ~125µs RTT, shm bulk |
| any *npm package* from Python | `pip install cc-node` | ~300µs RTT, shm bulk |

Worked examples: [`examples/recipe_js_module.ccs`](../examples/recipe_js_module.ccs),
[`examples/recipe_py_module.ccs`](../examples/recipe_py_module.ccs),
[`tests/dual_module_export_mod.ccs`](../tests/dual_module_export_mod.ccs)
(dual-target), [`tests/js_module_double_result_mod.ccs`](../tests/js_module_double_result_mod.ccs)
(the gamut).  Baseline numbers: `perf/baselines/js_py_modules_20260809.txt`.
