# Python interop: interpreter arenas (`py.cch`)

Status: draft — light v1 implemented (single interpreter, dlopen + stable ABI;
`cc/include/ccc/script/py.cch`, demo `examples/py/pydemo.shcc`). Pools and
moves are draft-only.

## 1. Model

An embedded Python interpreter anchors Python objects the way an arena
anchors allocations: every object has a home interpreter, its lifetime ends
no later than its home's, and nothing crosses homes implicitly. Crossing is
an explicit, costed operation (§7).

| Type | Role |
| ---- | ---- |
| `CCPy` | one interpreter — the arena |
| `CCPyObj` | an object reference, anchored to its home `CCPy` |
| `CCPyPool` | sharded interpreters, one GIL each (§6) |
| `CCPyError` | Python exception with a `CCError base @as` face (§4) |

```c
#include <ccc/script/py.cch>

CCPy py = cc_py(&a) !> @destroy;
CCPyObj np = py.import("numpy") !> @destroy;
CCPyObj arr = np.call("arange", 10) !> @destroy;
double s = arr.call("sum").as_f64() !>;
```

## 2. Loading

`cc_py` resolves `libpython3` with `dlopen` at first use; no CC binary
carries a Python dependency by existing. A missing or unloadable library is
a `CCPyError` at the `cc_py` call — the same posture as a missing binary in
`cc_command`. Bindings target the limited C API (`Py_LIMITED_API` / abi3):
one binding serves every 3.x, and nothing couples to interpreter
internals. `py.cch` is never in the prelude; scripts include it.

The interpreter initializes once per process; a second `cc_py` returns a
handle to the same interpreter. Spawning subprocesses while a `CCPy` is
live is safe; `fork` without `exec` is not supported.

## 3. Objects and calls

`CCPyObj` is opaque. `.get(name)` reads an attribute; `.call(name, args…)`
calls a method; both return `CCPyObj !>(CCPyError)`. Arguments marshal by
type: `int` / `int64_t` → Python `int`, `double` → `float`, `bool` →
`bool`, `CCSlice` / `char[:0]` → `str`, a typed slice (`double[:]` is
`CCSlice_double`, …) → `list` of its scalar element type, `CCPyObj` →
itself (same home required, §7). No other type marshals; there is no
deep conversion. Typed-slice dispatch is `_Generic` over the scalar
instance structs, so any expression of an instance type marshals as a
list; a slice erased to plain `CCSlice` (via `bytes()` or `.base`)
marshals as `str`. Non-scalar instances have no marshal arm and fail
at compile time.

`CCPyObj`'s dynamic sink is destination-aware (`.ufcs_dynamic2`):
wherever a typed destination is visible — a declaration
`T name = obj.method(args…)`, an assignment to a resolvable lvalue, or
a cast `(T)obj.method(args…)` directly wrapping the call — the
destination joins UFCS resolution, and the call lowers through the
library's destination-typed variant (`cc_py_obj_callm_double`,
`_float`, `_int`, `_int64_t`, `_long_long`) when one is declared. The
variant runs the same call, extracts the destination type, and releases
the intermediate object — it never reaches user space:

```c
double v = math.sqrt(2.0) !>;
int    i = (int)math.sqrt(2.0) !>;
```

The variant returns `T !>(CCPyError)`, so the site consumes it like any
Result; a composed cast is absorbed — it spells the destination and
performs no conversion, and it never consumes the Result (the sigil
does). Declaration and assignment destinations apply only when the call
is the whole right-hand side — an operand of a larger expression has no
single expected type in C; a cast is its own destination anywhere.
Destinations without a declared variant lower through the plain sink
and keep the `CCPyObj` Result. A scalar destination with no declared
variant is ill-formed — the plain Result can never initialize it — and
the diagnostic names the destination and enumerates the installed
variants.

Extraction semantics are the library's: `double`/`float` accept any
Python number (`float` narrows); integer destinations extract Python
ints exactly and truncate Python floats toward zero (C cast and Python
`int()` agree); a result outside the destination's range is a
`CCPyError`, not a truncation.

Explicit extraction remains for held objects and arena-backed forms:
`.as_i64() !>`, `.as_f64() !>`, `.as_slice(&arena) !>` (`str`/`bytes`
copied into the arena). Anything else stays a `CCPyObj`.

Reference lifetime rides the destroy machinery: `CCPyObj`'s registered
destroy hook releases the reference under its home's lock, so `@destroy`
and `.destroy()` work and chain as for any type. Releasing after the home
interpreter is destroyed is a no-op (hook idempotence).

## 4. Errors

```c
typedef struct {
    CCError base @as;   /* kind + message: str(exception), arena-copied */
    CCPyObj exc;        /* the exception object, home-anchored */
} CCPyError;
```

A Python exception surfaces as `CCPyError`: `base.message` is the
exception's `str()` captured at raise time, so the script register's
default `@errhandler(CCError)` prints it through the face. An exact
`@errhandler(CCPyError)` claims the exception object (traceback,
re-inspection) when it matters.

## 5. Blocking

Interpreter calls are blocking-shaped: ill-formed in `@noblock` context,
serialized per interpreter. A single `CCPy` behaves as one implicit
exclusive; fibers contending it park like any blocking call.

## 6. Pools

`CCPyPool` shards sub-interpreters, each with its own GIL (per-interpreter
GIL interpreters, CPython 3.12+):

```c
CCPyPool pool = cc_py_pool(&a, cc_shard_mask_auto(CC_PY_MAX_SHARDS)) !> @destroy;
CCPy* shard = pool.at(i);
```

- Shards spin up lazily; each holds its own module state (an import per
  shard that uses it).
- Every `CCPyObj` is anchored to its shard. Using an object under another
  shard is an error at the call — homes do not blur.
- Extensions that predate multi-phase init do not support per-interpreter
  GILs; the pool surfaces that library's import error verbatim. Pure-Python
  modules work unconditionally.

## 7. Moves

Anchored lifetime implies explicit transfer, arena-style:

```c
CCPyObj there = obj.clone_into(pool.at(k)) !>;
```

`clone_into` serializes in the home interpreter and rebuilds in the
target (pickle round-trip); objects exposing the buffer protocol take a
byte-copy fast path. The source reference is untouched; drop it separately
when the move is a handoff. Scalars need no transfer: extract to a CC
value, pass the value.

An object that does not serialize fails with `CCPyError` at the call —
crossing is loud, never partial.

## 8. Teardown

`CCPy`/`CCPyPool` destroy finalizes interpreters after releasing pending
references. Declare the interpreter before the objects it anchors:
reverse-declaration destroy order then releases every reference before
finalization. References that outlive their home release as no-ops (§3).

## 9. First consumer

`tools/perf.shcc @perf_plot`: read `perf/compiler_baseline.txt` history,
plot with matplotlib through a single `CCPy`. Exercises load, import,
call, marshal, extraction, error face, and teardown; needs no pool.

## 10. Out of scope

- Deep container conversion (dict/list ↔ CC collections)
- Python calling into CC (callbacks, `emit-pymodule`)
- Free-threaded (no-GIL) CPython builds
- `fork` without `exec` while an interpreter is live
- Non-limited C API (interpreter internals)
- Async integration (interpreter calls stay blocking-shaped)
- Implicit cross-home use or implicit moves
