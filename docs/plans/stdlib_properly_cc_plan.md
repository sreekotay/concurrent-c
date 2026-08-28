# Concurrent-C stdlib "properly CC" modernization plan

Living plan. Inventory from the 2026-07-25 pass; policy updated for current taste.

## Guiding principles

| Check | Meaning |
|-------|---------|
| **tutorial = idiomatic = performant = production** | One surface. No beginner dialect, no second "real" API, no slow path taught first. |
| **Dissolve lifetime; express ownership** | Prefer nursery/`close_on`/`@destroy`, arena provenance, channel transfer/`send_into`, owner fibers. Lifetime should look like app protocol (Redis conn/reply), not refcount algebra. |
| **Explicit arenas; no default magic** | Allocating edges name a `CCArena*` (or `name@(args)` / `@destroy`). No hidden thread-local heap for user-visible payloads. |
| **C is first-class** | Every blessed CC API has a clean, greppable `cc_*` twin suitable as the lowered / plain-C call. UFCS is sugar over that name—not a second ABI. Avoid unspeakable lowered symbols and "CC-only" Result wrappers with junk C names. |
| **No API museum (yet)** | In-tree out-param net/HTTP surfaces are not sacred. Prefer flipping primary names to Result and updating callers in the same change over long dual-ship. Keep a thin internal helper only when it simplifies the `.c` implementation—not as a taught second API. |
| **`@errhandler` stays** | Scope-level fatal policy is a forced decision for `!>;`, not wallpaper. Do not add ambient main defaults or `cc_fatal_main` helpers (more variants to learn). Inline `!>(e){…}` for one call; hoisted `@errhandler` for many. |

Wrong directions: Fil-C race-then-panic; Rust Mutex-first culture; `Mutex::[T]` / `@lock` as language (spec §6.3); sugar that hides allocation/copy.

---

## Executive summary

Drift is concentrated in **net / HTTP / TLS / DNS** (`CCNetError*` / `CCHttpErrorInfo*` out-params) while recipes, channels, files, process, slices, and redis’s non-net paths already use `T!>(E)` / `!>` / `@errhandler`. Secondary drift: arena create vocabulary, prelude vs `cc_runtime` teaching, capture-recipe comments naming forbidden Mutex/`Atomic::[T]` generics.

**Direction:** Make Result + explicit arenas the primary `cc_*` surface; update redis/tcp/http in the same phases; rewrite spec present-tense to match. Dual-ship only as a short mechanical bridge inside a PR if TCC/`CC_DECL_RESULT_SPEC` needs it—not as a release-long compat story.

---

## Properly-CC API checklist

| Axis | Rule |
|------|------|
| **Result shape** | Fallible ops return `T!>(E)` (via `CC_DECL_RESULT_SPEC` / lowered `CCResult_*` as needed). Domain errors (`CCIoError`, `CCNetError`, `CCError`)—not `int` errno + out-param. |
| **No taught error out-params** | `E* out_err` is not the curriculum or redis path. Remove or demote to private `cc__*` when flipping. |
| **C twin quality** | Public name is `cc_<subsystem>_<verb>` (e.g. `cc_tcp_listen`). Same function is what UFCS lowers to / what C calls. Result return is part of that signature, not a parallel `cc_tcp_listen_result`. |
| **Nullability** | Map lookups stay NULL-not-found. A root box (`CCBox` / `CCArena` / `CCNursery` / `CCExclusive`) is a handle; dead means `p == NULL`. Child-from-parent and fallible **use** are Result — not empty handle + `out_err`, not `if (!p)` as the recipe. |
| **Arena params** | Explicit `CCArena*` / `name@(args)`. Stabilization via `materialize_in` / `clone_into`. |
| **UFCS completeness** | `recv.method(...)` ↔ `cc_type_method(...)`. |
| **`@destroy` / ownership** | Resources register create/destroy; happy path shows `@destroy` / `@defer`. |
| **Fiber-awareness** | Document park vs `@noblock`. Shared business state → channels / owner fiber; atomics for counters. |
| **One job per tool** | e.g. `materialize_in` vs `clone_into`; `read` vs `read_into`. |
| **Spec** | Present-tense normative under `spec/` only. |

**Good templates today:** `std/slice.cch`, `cc_channel.cch`, `std/io.cch` (mostly), `std/process.cch`, `std/exec.cch`, `std/slice_packed.cch`.

**Primary fix targets:** `std/tls.cch`, `std/dns.cch` (net listen/accept/connect/read/write + HTTP Result-primary landed); weaker: `cc_file_open`, `std/future.cch` out_err.

---

## Module inventory & drift

### Core / prelude

| Module | Path | Drift |
|--------|------|-------|
| Prelude | `std/prelude.cch` | Blessed entry unclear: recipes mix `cc_runtime.cch` vs prelude. Net/http/cli stay opt-in (good). |
| Runtime | `cc_runtime.cch` | Prefer teaching `cc_channel_*` over legacy `chan_*`. |
| I/O error | `cc_io_error.cch` | Comments claim network uses it; net does not. |

### Arena / slice / string

| Module | Path | Drift |
|--------|------|-------|
| Arena | `cc_arena.cch` | Vocabulary: `name@(args)` / `cc_arena_heap` / `cc_arena_create` / `cc_arena_create_buffer`. Bless the binder + heap alias; don’t teach three names. |
| Slice std | `std/slice.cch` | Partial win (`materialize_in`, checked `at`)—template for net buffers. |
| Slice packed | `std/slice_packed.cch` | Dense held twin of `CCSliceHdr` (SSO / SDS / probe view). Map-key sugar wired. |
| String | `std/string.cch` | Growable owner. Some push helpers still NULL-fail vs Result. |

### Concurrency

| Module | Path | Drift |
|--------|------|-------|
| Channel | `cc_channel.cch` | Typed Result path is good; keep as model. |
| Atomic | `cc_atomic.cch` | Recipes wrongly mention `CCAtomic::[T]` / `CCMutex::[T]`. Real: `cc_atomic_*`. |
| Future/task | `std/future.cch`, `std/task.cch` | `int* out_err`—lower priority than net. |

### I/O / process / CLI

| Module | Path | Drift |
|--------|------|-------|
| File | `std/io.cch` | Reads/writes Result; **`cc_file_open` → `int`**. |
| Process / exec | `std/process.cch`, `std/exec.cch` | Good. |
| CLI | `std/cli.cch` | Soft drift (`ok` / `exit_code`)—intentional UX; document, don’t force `!>`. |

### Networking (largest drift)

| Module | Path | Drift |
|--------|------|-------|
| Net | `std/net.cch`, `cc/runtime/net.c` | Listen/accept/connect + socket read/write are Result-primary. Remaining: UDP/DNS/peer_addr/shutdown still `CCNetError*` out-params. |
| HTTP | `std/http.cch`, `cc/runtime/http.c` | Result-primary (`CCHttpResponse!>(CCHttpErrorInfo)`, `cc_url_parse` → `CCParsedUrl!>(CCHttpError)`). |
| DNS / TLS | `std/dns.cch`, `std/tls.cch` | Out-param culture. DNS decls duplicated with net. |

### Real projects

- **redis_idiomatic.ccs:** Result + arenas + `try_send_into`; listen/accept/fill/write use `!>`. Net drift left is UDP/DNS/HTTP if any.
- **pigz_idiomatic.ccs:** Ordered `send_task` + arenas; little net. Arena spelling mixed.
- **Examples:** Result recipe gold; `recipe_tcp_echo` listen/accept/connect/read/write use `!>` / `cc_io_avail`; `recipe_http_get` uses Result `cc_http_get` + `!>`; `recipe_ordered_parallel` aligned with pigz.

---

## C-first naming policy

When flipping an API:

1. **Primary symbol** = the Result-returning `cc_*` function (e.g. `CCListener!>(CCNetError) cc_tcp_listen(addr, len)`).
2. **UFCS** lowers to that same `cc_*` name.
3. **Implementation** may keep a private `cc__tcp_listen_raw(...)` or out-param helper inside `net.c` if convenient—not exported in the taught header surface.
4. **Do not** ship `cc_tcp_listen` (out-param) + `cc_tcp_listen_result` (Result) as twin public APIs.
5. If a transitional inline wrapper is needed for one PR, delete the old public signature before the phase closes (same PR or immediate follow-up)—not a deprecation era.

Lowered Result types (`CCResult_CCListener_CCNetError`, etc.) are mechanical; the **callable name** is what must stay human.

---

## Phased plan

### Phase 0 — Policy freeze

- Stop new public out-param error APIs.
- Document blessed prelude tiers (below) in `examples/README.md`.
- This file is the checklist; `DEPRECATIONS.md` only if something must linger one release (default: no linger).

### Phase 1 — Arena + prelude curriculum

Blessed arena:

| Intent | Spelling |
|--------|----------|
| Heap-rooted | `CCArena a = cc_arena_heap(kilobytes(4)) @destroy;` (root=N, `block_max=4`, then overflow) |
| Stack-rooted | `cc_arena_stack(a, N);` — same defaults; declaration macro (stack root) |
| Explicit policy | `cc_arena_create_buffer(buf, cap, policy)` (expert) |
| Don’t teach | `cc_arena_create` / `cc_arena_malloc` as separate *kinds* |

Blessed includes:

- Concurrency-only recipes: `<ccc/cc_runtime.cch>`
- Stdlib I/O / collections: `<ccc/std/prelude.cch>`
- Net: prelude + `<ccc/std/net.cch>` (http/tls/cli as needed)

Fix arena recipe `//future` comment (doc or tiny UFCS—prefer doc unless alloc UFCS is trivial).

### Phase 2 — Net Result primary (redis + tcp)

**First slice (listen / accept / connect): done.**

Primary signatures (same `cc_*` names, Result return):

```c
CCListener ln = cc_tcp_listen(addr, len) !>;
CCSocket sock = cc_listener_accept(&ln) !>;  /* UFCS ln.accept() also returns Result */
CCSocket client = cc_tcp_connect(addr, len) !>;
```

Landed: Result-primary `cc_tcp_listen` / `cc_tcp_connect` / `cc_listener_accept`,
stdlib `cc_net_to_io_error`, redis + `recipe_tcp_echo` + `ping_server` callers,
`tests/tcp_listen_accept_connect_smoke.ccs`, present-tense Networking spec for
those three ops. On error, handles have `fd == -1`.

**Follow-up slice (socket read / write / fill): done.**

EOF model B — byte reads return `bool!>(CCIoError)` (file/channel domain):

| Outcome | Result |
|---------|--------|
| Got bytes | `Ok(true)` + out payload |
| Clean peer close (FIN) | `Ok(false)` — not an error |
| RST / timeout / other | `Err(...)` |
| try_read would-block | `Err(CC_IO_BUSY)` — never `Ok(false)` |

```c
while (cc_io_avail(cc_socket_read(sock, arena, n, &data))) { /* ... */ }
size_t n = 0;
bool got = cc_socket_read_into(sock, buf, cap, &n) !>;
size_t w = cc_socket_write(sock, p, len) !>;
```

Landed: Result-primary `cc_socket_read` / `read_into` / `read_into_deadline` /
`try_read_into` / `write` / `write_deadline`; redis `rr_fill` (FIN → `cc_ok(false)`,
try busy → fill-layer `cc_ok(false)`); `recipe_tcp_echo` + `ping_server`;
`tests/socket_read_eof_smoke.ccs` + adapted deadline smokes; Networking spec
present-tense for model B. Listen/accept stay on `CCNetError`; byte I/O uses
`CCIoError` so redis `@errhandler` I/O culture matches without out-params.

### Phase 3 — HTTP: done

Primary signatures (same `cc_*` names, Result return):

```c
CCHttpResponse resp = cc_http_get(arena, url, len) !>;
CCHttpResponse resp = cc_http_post(arena, url, len, body, body_len) !>;
CCHttpResponse resp = cc_http_client_get(&client, arena, url, len) !>;
CCParsedUrl u = cc_url_parse(url, len) !>;
```

Landed: Result-primary `cc_http_get` / `cc_http_post` / `cc_http_client_get` /
`cc_http_client_post` / `cc_http_client_request` with Err arm
`CCHttpErrorInfo` (rich code + optional net_error + message); `cc_url_parse`
→ `CCParsedUrl!>(CCHttpError)`; `recipe_http_get.ccs` uses `!>` for HTTP and
`@errhandler(CCError)` for nursery; HTTP stdlib spec present-tense
Result-primary with caller-arena lifetime for response/error slices. No
public `*_result` sidecar or taught `CCHttpErrorInfo*` out-param.

Compiler support: register
`CCResult_CCHttpResponse_CCHttpErrorInfo` /
`CCResult_CCParsedUrl_CCHttpError` in
`cc__stdlib_predeclared_result_specs`, and make
`cc__ru_err_type_from_result_name` consult that table so Err types with
underscores (`CCHttpErrorInfo`) type `!>(e)` binders correctly.

### Phase 4 — TLS / DNS

Mirror Phase 2. Deduplicate DNS decls.

### Phase 5 — File open + async polish

- `cc_file_open` → Result-shaped primary.
- Async/`future` out_err: wrap at high-level boundaries later; not blocking redis/pigz.

### Phase 6 — Capture curriculum + capabilities

- Fix Mutex/`Atomic::[T]` comments → channels / owner fiber / `cc_atomic_*`.
- Process config: prefer capability/config struct from `main` over ad-hoc `getenv` in library cores (design note; implementation can trail).

### Phase 7 — Optional tighten

Vec/string push Result-ify if it pays for itself. No out-param public leftovers in net/http.

---

## Migration mechanics (not dual-ship eras)

Per subsystem PR:

1. Implement Result return on the primary `cc_*` symbol (header + runtime as needed).
2. Point UFCS at that symbol.
3. Update all in-tree callers (redis, examples, tests, perf) in the **same** change set.
4. Spec Networking/HTTP sections rewritten present-tense to Result-primary.
5. Private raw helpers allowed; second public API not allowed.

Mechanical bridge inside a branch (old body called with `&err` then wrapped) is fine. Shipping both to users is not the goal.

---

## Curriculum & real_project impact

| Phase | Impact |
|-------|--------|
| 1 | README + arena recipe clarity; pigz spelling consistency optional |
| 2 | **redis** listen/accept/fill/write; **tcp echo** Result showcase end-to-end |
| 3 | **http get** matches result recipe — done |
| 5 | pigz open paths if they still check `int` |
| 6 | capture recipes honest about shared state |

---

## Non-goals

- Preserving out-param public APIs for external compat (none required yet).
- Ambient `@errhandler` / fatal helpers.
- `Mutex::[T]` / Fil-C / hidden allocation.
- HTTP/curl in default prelude.
- Rewriting `cc_containers.cch` into curriculum.
- Making TCC fake atomics real—document `CC_ATOMIC_HAVE_REAL_ATOMICS`.

---

## Open questions

1. **Net error domain (partially resolved):** Listen/accept/connect keep `CCNetError` for address/connect fidelity. Socket byte read/write use `CCIoError` (files/channels/redis). HTTP keeps rich `CCHttpErrorInfo` as the Result Err arm. Full unify still open for UDP/DNS/TLS.
2. **EOF for `socket.read` — resolved (model B):** `Ok(false)` + out-slice/count for clean FIN; RST/failures are `Err`; try would-block is `Err(CC_IO_BUSY)`.
3. **Prelude:** Keep net opt-in vs `std/prelude_net.cch` umbrella?
4. **Capabilities:** In-scope for this program or separate (redis/pigz getenv)?
5. **Arena alloc UFCS:** Implement `arena.alloc_T` or bless `cc_arena_alloc_T*` as the permanent C twin?

---

## Suggested next PR

**Phase 3 HTTP — landed.** Next: Phase 4 TLS/DNS Result-primary, or remaining net out-params (UDP/peer_addr/shutdown).
