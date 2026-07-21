# UFCS: registered-type method calls unresolved in one band of a large TU

**Status:** open.  **Found:** 2026-07-21, redis write-family refactor.

In `real_projects/redis/redis_idiomatic.ccs` (at the commit introducing
receiver-first `redis_conn_*` write helpers), method spellings on a
registered type (`RedisConn*`, registered with only a `.destroy` hook)
lower correctly at some sites and are left as raw C member accesses at
others — the host compiler then fails with
`'RedisConn' has no member named 'flush_out'`.

The split is POSITIONAL, not semantic: `conn->write_reply(...)` /
`conn->flush_out()` lower fine inside `drain_pipeline_batch` and
`handle_client` (~line 1700), while the SAME methods on the SAME
receiver name/type fail throughout the write-family bodies
(~lines 1387–1466) and `redis_conn_write_reply_now`.

Ruled out by minimal probes (both lower correctly in a small TU):
- receiver is a function parameter (vs local / member expr);
- method call inside a same-family (`widget_*`) function body;
- snake_case callee-name resolution for hookless registrations.

Repro: check out the redis file at this commit, flip any one of the
plain `redis_conn_write_bytes(conn, ...)` calls inside
`redis_conn_write_reply` back to `conn->write_bytes(...)`, rebuild.

Workaround in tree: implementation bodies use plain calls; method
spelling kept at protocol-level call sites (which lower correctly).

Suspicion to check first: whatever bounds the UFCS collector's node
coverage or var-type registration in that specific band of this large
TU (the band sits between the mem-stats fprintf block and
`redis_conn_create`) — possibly an inert-scan state desync or a
collector early-out that a smaller TU never reaches.
