# Face dispatch of a bare unwrap

`face_dispatch.ccs`: a function returning `bool !>(CCIoError)` with only
`@errhandler(CCError e)` in scope, and a bare `!>` on a `CCIoError`
result. Output:

```
CCError handler ran (via face)
ok(false)
```

The bare `!>` resolves through the `as:` face onto `CCError` when no
exact handler exists, even though the function's own error type is the
exact one. Seen live in `redis_std.ccs` `exec`: the `.mget` and `.keys`
arms unwrap `enc->array_len(...)` and `enc->bulk(...)` under a `CCError`
handler whose policy is "write -ERR", so a socket failure mid-array is
answered by a second write to the dead socket. One line fixes it
(`@errhandler(CCIoError e) { return cc_err(e); }` at the top of `exec`,
as `execute` already has). The design point is section 1.3's cost: a
face extends a handler's reach, and the reach crosses a policy line the
file's own header draws.

## Return of a Result under an exact handler (seed 0.4.0-411)

`redis_std.ccs` `exec` carries a comment that a top-level
`@errhandler(CCIoError e)` makes `return enc->…()` lower to brk on
the Ok path, and works around it with `!>(e) { return cc_err(e); }` at
five sites. Three probes on 0.4.0-411:

| Probe | Shape | Result |
|-------|-------|--------|
| `return_under_handler.ccs` | one exact handler, `return g();` | Ok and Err paths correct |
| `return_two_handlers.ccs` | `CCIoError` re-raise plus `CCError` to `-ERR`, `@switch` on a variant, `return enc->m()` through a typeview pointer | Ok, `-ERR` as Ok, and I/O re-raise all correct |
| `return_two_handlers_scratch.ccs` | the same with `@string(…, @scratch)` inside a case | ping: ok(true);echo(-1): -ERR written, ok(true);echo(fail): io err (expected); |

The "brk" in the comment is the switch-scratch-reclaim defect fixed in
main's `796b454` ("@scratch reclaim before @switch cases jumped over
init (host brk)"). On 411 the top-level handler is the right form and
the five per-site handlers and the comment can go.

## Seed 0.4.0-414: the face is gone

`face_dispatch.ccs` no longer compiles, with two diagnostics that name
the rule and the types:

```
error: '@errhandler(CCError)' handles an error nothing in its scope raises;
       remove it, or unwrap a Result of 'CCError' under it
error: no matching '@errhandler' for error type 'CCIoError': 'io_fail' is
       'bool !>(CCIoError)'; in-scope '@errhandler' is 'CCError'
```

The misroute this study found is now a compile error. `redis_std.ccs`
on main still compiles on 414 with its five per-site handlers, which
are now merely unnecessary.
