# Compiler limitations blocking the `redis_idiomatic` `@async` migration

**Status:** the six original bugs are fixed and the `@async` migration is
live in `real_projects/redis/redis_idiomatic.ccs` (handle_client and
owner_loop now run on `spawn_async` + V2-scheduler fibers).  Three narrow
follow-up bugs were surfaced while getting the migration to compile; see
the "Follow-up bugs surfaced during migration" section below.  The
parser-mode result-type collapse work from Bug [3] is also still a
follow-up — tracked at the very bottom of this file.

`real_projects/redis/redis_idiomatic.ccs` would ideally look like the
sketch:

```
@async void main() {
    CCListener ln = cc_tcp_listen(...) !> @destroy;
    CCNursery* server = cc_nursery_create(NULL) !> @destroy;

    while (true) {
        CCSocket client = await ln.accept();
        server->spawn_async(handle_client(client));
    }
}

@async void handle_client(CCSocket client) {
    RedisConn* conn = redis_conn_new(...) !> @destroy;
    RespReader  reader = ...;

    @for await (RedisRequest req : parse_requests(&reader, &client)) {
        RedisReply reply = await execute_request(&db, req);
        await conn.send_reply(reply);
    }
}
```

Each connection would then ride on a cheap V2-scheduler fiber instead
of a full hybrid worker (`spawn_async` is ~1000x cheaper than
`spawnhybrid`). Four distinct compiler bugs currently block that
migration.

Until they are fixed, `handle_client` and `owner_loop` stay on the
hybrid-closure spawn path. The short form of each bug is mirrored in
the `bug:` / workaround comment block at the top of
`redis_idiomatic.ccs`.

---

## [1] `@async` body combined with `!>` unwrap-binder — hard-fatal

The async state-machine pass lifts locals into a coroutine frame by
rewriting
```
T x = EXPR;        =>    __f->x = EXPR;
```
but for locals whose `EXPR` came out of a `!>`-unwrap expansion it
leaves the type prefix in place and emits literally
```
T __f->x = ({ ... });
```
which is invalid C and fails reparse.

Minimal repro:
```
$ cat /tmp/async_unwrap_repro.ccs
#include <ccc/cc_runtime.cch>
#include <ccc/cc_nursery.cch>
#include <ccc/std/task.cch>

static bool !>(CCError) may_fail(int x) {
    if (x < 0) { CCError e = {0}; e.message = "neg"; return cc_err(e); }
    return cc_ok(true);
}

@async int worker(int x) {
    @errhandler(CCError e) { (void)e; return 9; }
    bool got = may_fail(x) !>;
    (void)got;
    may_fail(x) !> { return 7; };
    return 0;
}
```
The reparse failure dump (`/tmp/cc_reparse_fail_*.c`) contains the
literal line
```
bool __f->got = ({ ... });
```
which is invalid C.

**Workaround:** keep `handle_client` / `owner_loop` as plain sync
functions and spawn via `spawn` / `spawnhybrid` closures, so no `!>`
form ever has to cross an async-frame rewrite.

**Fix:** `pass_async` needs to strip the type prefix from decl-inits
whose RHS is a result-unwrap expansion and move the declaration into
the frame struct (same treatment as plain `T x = expr;`).

---

## [2] `@async void` plus `spawn_async` call form — type-checks against `int`

`server->spawn_async(fn(args))` reparses the inner call as an
expression whose value feeds `cc_nursery_spawn_async`'s `int` return,
so `@async void fn(...)` errors at the spawn site with
```
cannot convert 'void' to 'int'
```

Every async smoke test threads `int` through for this reason (see
`tests/async_direct_handoff_smoke.ccs`,
`tests/async_request_reply_handoff_smoke.ccs`).

**Workaround:** n/a while [1] blocks the migration — not applicable
on the hybrid-closure path.

**Fix:** the lowering needs to distinguish spawn-site (discard the
result) from await-site (bind to a value).

---

## [3] `!>(e)` binder does not propagate the declared error type

At `handle_client`'s Phase D, the call-site shape
```
bool !>(CCIoError) rr_fill(...);
...
rr_fill(&reader, &client) !>(e) { report_io_err(&client, conn, e); ... };
```
would read best. But `pass_result_unwrap` lowers the binder `e` as
`__CCGenericError` regardless of the result's declared error type, and
`__CCGenericError` does not coerce to `CCIoError`, so the body fails
to type-check.

**Workaround:** use the manual shape instead:
```
bool !>(CCIoError) res = rr_fill(...);
if (res.is_err()) { report_io_err(&client, conn, res.error()); return; }
if (!res.value()) return;
```
(`handle_client`'s Phase D in `redis_idiomatic.ccs` is the live
instance.)

**Fix:** `pass_result_unwrap` should propagate the declared error type
from the LHS annotation through to the binder in the body.

---

## [4] Thin `@async int` wrapper workaround for [1] ALSO does not work

Naive attempt: keep the real bodies as plain sync functions, add thin
`@async int` wrappers that call them, and `spawn_async` on the
wrappers:
```
static void handle_client_impl(...) { ... !> forms ... }
@async int handle_client(...) { handle_client_impl(...); return 0; }
```

Building such a file fails with
```
CC: async_ast: failed to build statement list for @async function
    'handle_client' (no body block + no braces)
```
even though the `@async` function clearly has a braced body. The
presence of a neighbouring sync function containing unwrap-binder
forms appears to throw off `async_ast`'s body scan (likely a raw-text
match leaking across function boundaries).

**Workaround:** none discovered that is cleaner than staying on the
hybrid-closure path from [1].

**Fix:** `async_ast` needs to scope its body-block lookup to the
`@async` function's own brace-range and not be perturbed by
neighbouring functions.

---

## [5] `!>` tokens inside comments are still parsed (diagnostic noise)

`pass_result_unwrap` scans the raw file text for the unwrap sigil
without tracking comment context, so a literal occurrence of the
sigil inside a block comment or line comment can fire either
```
error: syntax: expected ';' terminating '!>' body
```
or
```
error: syntax: expression-position '!>' body must diverge
       (return/break/continue/goto/@err/exit/abort/etc.)
```

This is not always fatal (see [6]), but a comment block with enough
sigils can tip over into a real parse error. It's why long-form
documentation like this file lives outside the source.

**Workaround:** don't put the sigil in comments; or put it in a
separate `docs/` file like this one.

**Fix:** strip or skip comments before the pass scans.

---

## [6] Spurious "terminating `!>` body" warning on every build

`ccc` prints
```
.../redis_idiomatic.ccs:447:1: error: syntax: expected ';'
    terminating '!> (CCError)' body
```
on every build of `redis_idiomatic.ccs`. The line number is
post-transform, the real source is fine, and the build still
succeeds (exit 0, binary produced).

**Workaround:** ignore.

**Fix:** `pass_result_unwrap` should only emit the diagnostic when it
actually bailed out, not on the no-op path.

---

## Follow-up bugs surfaced during migration

These only showed up once the six blocking bugs above were fixed and the
`@async int handle_client(...)` migration actually started compiling.
All three have source-side workarounds living in
`real_projects/redis/redis_idiomatic.ccs`; the workarounds should come
out once the compiler fixes land.

### [F1] `pass_async` keeps the type prefix on block-scoped decl-inits that cross a yield — FIXED

**Status:** fixed.  Smoke test:
`tests/async_frame_decl_comment_prefix_smoke.ccs`.

**Symptom:** a local's declaration inside a nested block (`while`/`for`
body, inner `{ ... }`, etc.) whose lifetime crossed a yield produced
```
T __f->x = init;
```
in the lowered output — invalid C.  The block-scope reparse dump shape
was e.g. `size_t __f->outstanding = 0;` in `handle_client`'s inner
`while (true)` body.

**Root cause:** not actually "block scope vs function scope".  The
async pass classifies each statement by inspecting `p[0]` after a
whitespace skip, then dispatches to one of several decl-strip
branches (pointer, multi-word scalar, array, struct, int/intptr_t).
The shared skip helper (`cc__skip_ws`) was aliased to `cc_skip_ws`,
which only eats space/tab/newline.  Statements preceded by a block or
line comment — the common case in block-scope bodies, where a
clarifying comment sits right before the decl — saw `p[0] == '/'`,
which none of the decl-strip branches recognize, so they fell through
to the generic "emit as-is with ident rewrites" fallback.  That
fallback ran `cc__rewrite_idents` on the raw text, turning
`outstanding` into `__f->outstanding` but leaving the `size_t`
prefix in place.

Function-scope decls typically sit right at the top of the function
without a leading comment, which is why this shape was only observed
in nested bodies.

**Fix:** pointed `async_ast.c`'s `cc__skip_ws` macro at a new
`cc_skip_ws_and_comments_ptr` helper in `cc/src/util/text.h`.  The
helper eats ws + `// ...` + `/* ... */`, so all ~50 call sites in the
pass (decl-strip branches, statement-boundary scanners, RHS advance
helpers) now transparently see past leading/intermediate comments.

No other branches or dispatch logic needed changes — the fix is one
macro redefinition plus one shared utility.

**Related passes:** `pass_result_unwrap.c` already had its own
`cc__skip_ws_comments_forward` for exactly this reason, so it was not
affected.  Other text-based rewrite passes (`visit_codegen.c`,
`pass_channel_syntax.c`, `pass_create.c`, `pass_await_normalize.c`,
`pass_nursery_spawn_ast.c`, `pass_type_syntax.c`) still use the
ws-only skip; if a similar "comment hides syntax" bug surfaces there,
the same one-line macro swap to `cc_skip_ws_and_comments_ptr` is the
cheap fix.

### [F2] `pass_async` keeps the type prefix on result-typed decls of any shape

Even after [F1] is worked around, a local whose type is a result
(`bool !>(CCIoError) r;` / `RedisReply !>(CCError) r = ...`) emits
```
CCResult_bool_CCIoError __f->r = init;       (with init)
CCResult_bool_CCIoError __f->r;               (no init)
```
i.e. the type-strip logic skips result-typed decls entirely, whether
they have an initializer or not.  This is independent of the block
nesting in [F1] — even at function scope, a `bool !>(E) x;` is
mis-emitted.

**Workaround in-source:** never let a result-typed local exist inside
an `@async` function body.  Consume the result entirely in a sync
helper (`rr_fill_split` / `db_execute_split` near the top of
`redis_idiomatic.ccs`), exposing value/error as primitive out-params,
and keep primitives in the async frame.

**Fix:** extend the strip logic to recognise `CCResult_*_*` /
`CCRes(...)` / `T !>(E)` as legitimate types to strip on frame move.
Probably one character's difference from the [F1] fix once it's in
hand.

### [F3] Orphan unwrap error-tmps hoisted to the frame as an incomplete type

`pass_err_syntax` emits per-`!>`-site tmp names like `__cc_er_r_N` /
`__cc_er_d_N`.  When the unwrap form is inlined at the use site via a
statement-expression with `__typeof__(expr) __cc_er_r_N = ...`, those
tmp names never need to escape.  `pass_async` sees the name, though,
and speculatively reserves a frame field for it as
`struct __CCResultGeneric __cc_er_r_N;`.

In parser mode `__CCResultGeneric` is a concrete struct and the field
compiles away as dead-weight.  In real-compile mode it's only a
forward declaration (`cc_result.cch` guards the definition behind
`#ifdef CC_PARSER_MODE`), so the frame struct fails to compile with
```
error: field has incomplete type 'struct __CCResultGeneric'
```

**Workaround in-source:** provide the same layout-compatible stub
locally in non-parser mode at the top of the `.ccs` file:
```
#ifndef CC_PARSER_MODE
#ifndef __CC_RESULT_GENERIC_DEFINED
#define __CC_RESULT_GENERIC_DEFINED
typedef struct __CCGenericError { ... } __CCGenericError;
typedef struct __CCResultGeneric { bool ok; union { intptr_t value;
                                                     __CCGenericError error; } u; } __CCResultGeneric;
#endif
#endif
```
The orphan fields are never read/written, so layout compatibility is
enough.

**Fix (pick one):**
1. Stop emitting the orphan frame fields when the unwrap form is
   inlined at the use site (no lifetime crosses a yield).
2. Emit the orphan fields with the concrete `CCResult_T_E` type that
   the unwrap-expansion actually produces.
3. Promote the parser-mode `__CCResultGeneric` definition to also be
   visible in non-parser-mode (it's still used by other unwrap
   helpers, so the layout is stable).

Option 1 is the cleanest; options 2/3 are trivial but leave dead
fields in the frame.

### Perf note

With all six blocking bugs fixed and the three follow-up bugs above
worked around in-source, `redis_idiomatic` builds and serves
correctness-clean under concurrent load.  Steady-state throughput,
however, is substantially *lower* than the `spawnhybrid`-closure
baseline (`redis_hybrid`):

| workload                                   | hybrid rps | idiomatic rps |
|--------------------------------------------|------------|---------------|
| pipelined P=16, c=16, PING_MBULK           | 1.56 M     | 161 K         |
| pipelined P=16, c=16, SET                  | 1.61 M     | 145 K         |
| pipelined P=16, c=16, GET                  | 1.85 M     | 149 K         |
| unpipelined, c=50, SET                     | 136 K      | 111 K         |
| unpipelined, c=50, GET                     | 109 K      | 119 K (~same) |

p50 latency on the pipelined path is 0.1 msec on hybrid vs ~1.7 msec
on idiomatic — consistent with each yield point in the state machine
costing a scheduler round-trip.

This isn't a correctness regression, but it means the migration is
not a pure win on a steady-state RESP workload: the `spawnhybrid`
baseline pinning a full OS thread per connection beats the
cheaper-spawn / more-yield-points `@async` shape here.  Revisit once
the V2 scheduler can reduce per-yield round-trip cost (or once the
handler body can be reshaped to have fewer yield points per batch —
e.g. coalescing the per-request channel send/recv into a batched
variant).

---

## Follow-up: parser-mode result-type collapse

The fix for Bug [3] is a *local* fix in `pass_result_unwrap.c`: the
unwrap pass extracts the plain callee name from the unwrap call, looks
up its declared error type in an extended `result_fn_registry`, and
emits a typed binder of the form

```
E e = *(E*)(void*)&((tmp).u.error);
```

This parses correctly in both modes:

- **Parser mode:** `(tmp).u.error` has type `__CCGenericError` (the
  layout-compatible placeholder).  The cast through `void*` recovers
  the declared `E`.  No runtime here — this code exists only for
  type-checking.
- **Real compilation:** `(tmp).u.error` already has type `E`.  The
  cast is an identity no-op.

The **root cause** behind Bug [3] is more general: in
`CC_PARSER_MODE`, every `CCResult_T_E` is collapsed to a single
`__CCResultGeneric` struct whose `u.error` field is
`__CCGenericError`.  That collapse was introduced deliberately in
commit `574471e` ("Implement macro-based parser-safe result types")
to handle the case where a user declares `T !> (MyError)` before
`MyError` is known to the parser — the preprocessor emits
`typedef __CCResultGeneric CCResult_T_MyError;` up-front so parsing
does not require `MyError` to be in scope.

That trade-off is over-eager for result types whose error type *is*
in scope at declaration time (the common case).  For those, the
parser-mode struct could be a real distinct type with a typed `u`
union, and `cc_error(r)`, `cc_value(r)`, and `_Generic` dispatch
would all behave correctly without the unwrap pass having to
synthesize typed binders on the side.

**Blast radius of unwinding the collapse:**

1. `cc/include/ccc/cc_result.cch` lines 277-309: change parser-mode
   `CC_DECL_RESULT_SPEC` to emit a real distinct struct with typed
   union fields (mirror the non-parser path).
2. Same file lines 381-413: delete the pre-declared generic aliases
   (`typedef __CCResultGeneric CCResult_int_CCError; ...`) and the
   `cc_ok_CCResult_*` / `cc_err_CCResult_*` `#define` macros, which
   would conflict with the inline functions produced by the changed
   macro.
3. `cc/src/preprocess/preprocess.c` line 6617: change the auto-stub
   emission from `typedef __CCResultGeneric CCResult_X_Y;` to either
   a forward struct declaration (when the error type is not yet in
   scope) or a full `CC_DECL_RESULT_SPEC` invocation (when it is).
   This requires ordering the stub emission *after* the user's
   typedefs for custom error types.
4. Audit passes that assume `r.u.value` is `intptr_t` in parser mode
   (notably `cc_unwrap_as` casts and any statement-expression lowering
   that treats the value as an integer).

Estimated scope: 150-300 lines of source changes plus test auditing.
Low-risk tests: existing stdlib result types use `CCError` / `CCIoError`
which have stable layouts.  High-risk areas: anything that does raw
pointer casts through `r.u` expecting a specific layout.

This is a focused follow-up PR, not part of the current bug fixes.
