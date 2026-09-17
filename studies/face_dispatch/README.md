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
