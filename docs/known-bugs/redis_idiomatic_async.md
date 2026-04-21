# Compiler limitations blocking the `redis_idiomatic` `@async` migration

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
