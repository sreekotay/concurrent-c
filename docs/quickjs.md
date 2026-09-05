# QuickJS — in-process embed

Header: [`ccc/script/quickjs.cch`](../cc/include/ccc/script/quickjs.cch).
Demo: [`examples/qjs/qjsdemo.shcc`](../examples/qjs/qjsdemo.shcc).
Normative surface: [stdlib spec — QuickJS interop](../spec/concurrent-c-stdlib-spec.md#quickjs-interop).
Adversarial lifetime study → [`studies/quickjs_lifetime/`](../studies/quickjs_lifetime/).

CC owns `main`. QuickJS is a guest in the same process. The header is
userspace — no compiler or runtime special case — same posture as
[`py.cch`](js-py-modules.md) / [`js.cch`](js-py-modules.md), and a
different machine: one `JSRuntime`, no child, no Node-API, no `require`.

The engine is **your** dependency. This repo does not vendor or download
it. Attach [bellard/quickjs](https://github.com/bellard/quickjs) or
[quickjs-ng](https://github.com/quickjs-ng/quickjs) — same files, same
loader. The adapter compiles `JS_NewClassID(rt, &id)` when the tree
defines `QJS_VERSION_MAJOR` (ng); otherwise Bellard's one-arg form.

```c
#include <ccc/script/quickjs.cch>

if (!cc_qjs_available()) {
    "SKIP (attach QuickJS — CC_QUICKJS_SRC or ./quickjs)".println();
    return 0;
}

CCQjs js = cc_qjs_new(a) !> @destroy;
CCQjsVal Math = js.eval("Math") !> @destroy;
double v = Math.sqrt(2.0) !>;
println(@string(`Math.sqrt(2.0) = ${v}`, a));

CCQjsVal jv = js.eval("JSON.stringify({ok:1})") !> @destroy;
char[:] vs = jv.as_slice() !>;
println(@string(`json           = ${vs}`, a));
```

```sh
# once per machine / project — either tree
git submodule add https://github.com/bellard/quickjs quickjs
# or:  git submodule add https://github.com/quickjs-ng/quickjs quickjs
# or:  export CC_QUICKJS_SRC=/path/to/quickjs

ccc examples/qjs/qjsdemo.shcc
```

Bind the extracted slice, then interpolate. Putting a quoted JS literal
inside `@string(\`...\`)` breaks the template.

## How a call works

`CCQjs` is one runtime + one context. `CCQjsVal` is a JS reference
anchored to that handle (`home`). Failures are `CCQjsError` (a `CCError`
face plus `name` / `stack`).

Unresolved methods go through the typehook sink:

1. `Math.sqrt(2.0)` → get property `sqrt` on the value, `JS_Call` with
   marshalled args.
2. `js.eval("…")` is the handle's dynamic member (not a free function
   you have to remember).
3. A typed destination (`double v = …`, `char[:] vs = …`) picks the
   dest-typed sink variant, extracts, and releases the intermediate JS
   value — no extra `CCQjsVal` bind.
4. `!>` unwraps the Result. `@destroy` on a live `CCQjs` / `CCQjsVal`
   drops the JS reference (and the runtime, for the handle).

`f.invoke(args…)` calls the value itself (same fact as Python).
`js.exec(src)` evaluates for side effects and discards the result.
`js.set(name, val)` / `val.get(name)` are the global / property hops.
Cross-home values refuse by name — a second `cc_qjs_new` is a second
runtime; there is no isolated-process flag.

```mermaid
flowchart LR
  src["CC: Math.sqrt(2.0)"] --> sink["ufcs_sink / dest extract"]
  sink --> adp["adapter .so"]
  adp --> qjs["QuickJS JS_Call"]
  qjs --> adp
  adp --> sink
```

## Why an adapter

The header never includes `quickjs.h` and never depends on `JSValue`
layout. `CCQjsRaw` is a 16-byte pack. An adapter `.so` exports
`cc__qjs_*` wrappers around the inlines (`JS_Eval`, `JS_FreeValue`, …).

`cc_qjs_available()` **is** the loader (and `cc_qjs_new` runs the same
path). Order:

1. `CC_LIBQJS` — `dlopen` an already-built adapter (must export
   `cc__qjs_*`, not raw `JS_*`).
2. `CC_QUICKJS_SRC` if that directory has `quickjs.h` + `quickjs.c`.
3. `./quickjs`, `./third_party/quickjs`, `./vendor/quickjs` (same files).
4. Else a named error: how to attach upstream (Bellard or ng). Absence
   is not a link failure and not a silent `NULL`.

A source tree is compiled once (`-std=gnu11 -fPIC -shared`, plus
`libregexp.c` / `libunicode.c` / `cutils.c` / `dtoa.c` when present;
not `libbf.c`) into
`~/.cache/concurrent-c/qjs/<src_id>/libccqjs_<tag>.so`.
`<src_id>` is the engine tree’s git `HEAD` (12 hex) when that tree is a
git checkout, otherwise a short hash of its absolute path — so switching
`CC_QUICKJS_SRC` / pins does not require wiping the cache. Override the
root with `CC_QJS_CACHE`. The per-`.so` tag still folds adapter text,
host compiler, path, size/mtime, and a cheap content fingerprint of
`VERSION` + `quickjs.h`, so an engine bump rebuilds.

## Memory

The constructor arena is the owner of bytes this header mints: error
text, default extracts, and `own::[T]` slots. Close the handle, then
release that arena — that is teardown.

`js.own::[T](value)` copies `T` into the handle arena and binds a JS
object to the slot. The source is consumed. Scalar UFCS methods on `T`
become JS functions. The JS finalizer **poisons** the opaque; it does
not free `T`. Pretending GC released the CC bytes would look like
success.

Per-object reclaim is a later language hoist of the generational store
in [`perf/wstore5.ccs`](../perf/wstore5.ccs), not this header.

## Not this header

| That | Here |
|------|------|
| [`js.cch`](js-py-modules.md) libnode / node child, `require`, napi | one in-process QuickJS |
| [`py.cch`](js-py-modules.md) libpython / isolated child | same-process only |
| Export a `.node` / `.abi3.so` | CC owns `main` |
| Folding into the `CCJs` probe order | a separate include |

## Tests

With the engine attached (`CC_QUICKJS_SRC` or `./quickjs`):

```sh
ccc examples/qjs/qjsdemo.shcc
ccc tests/qjs_exec_eval_smoke.shcc
ccc tests/qjs_call_dest_smoke.shcc
ccc tests/qjs_error_detail_smoke.shcc
ccc tests/qjs_home_cross_refuse_smoke.ccs
```

Without QuickJS those programs skip via `cc_qjs_available()`.
