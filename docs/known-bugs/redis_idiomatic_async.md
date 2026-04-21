# Compiler limitations blocking the `redis_idiomatic` `@async` migration

**Status:** the six original bugs **and** all six follow-up bugs
([F1]/[F2]/[F3]/[F4]/[F5]/[F6]) are fixed.  The `@async` migration is
live in `real_projects/redis/redis_idiomatic.ccs` (handle_client and
owner_loop now run on `spawn_async` + V2-scheduler fibers).  The
parser-mode result-type collapse work from Bug [3] is still deferred
as a focused follow-up — tracked at the very bottom of this file.

Two new bugs surfaced while cleaning up the reply path, and a third
fell out of the [F8] fix:

- **[F7]** (OPEN) — tcc's parser rejects a statement-expression
  initializer (`T x = ({ ... });`) when it appears as the first
  statement of a freshly-opened `case LABEL: { ... }` compound.  This
  blocks the natural `T x = foo() !>;` lowering inside switch arms.
- **[F8]** (FIXED) — the `!>(e) BODY` binder form at expression
  position used to lower `e` as `__CCGenericError` instead of the
  declared error arm of the result.  Fixed in
  `d9ca37f compiler: unify !> / ?> lowering and harden text scanning`
  by threading the LHS result type through
  `pass_result_unwrap`'s expression-position rewrite the same way the
  statement-position decl-form already did.
- **[F9]** (FIXED) — uncovered by [F8]'s fix: inside an `@async`
  function, the now-correctly-typed binder `e` introduced by
  `!>(e) BODY` was picked up by `async_ast` as a local and frame-
  lifted, so the state-machine lowering emitted an invalid
  declaration of the form `CCIoError __f->e = ...;` (lvalue-
  expression in a declarator).  Fix: `pass_result_unwrap` now mangles
  the user binder to `__cc_pu_bind_<id>_<name>` and rewrites its
  word-bounded references inside the error-path body accordingly.
  The `__cc_pu_` prefix matches `async_ast`'s existing no-frame-lift
  rule, so the binder stays a true local and its declaration survives
  the identifier-rewrite pass intact.  Uncovered a latent pre-existing
  defect in the stdlib-predeclared result-spec seeding that was
  forcing `_Generic` arms for types whose defining header was not in
  the TU; `visit_codegen.c` now only seeds predeclared specs whose
  `CCResult_T_E` or ok-type name is referenced in the source.

The final two follow-ups ([F5] and [F6]) both fell out of the same
underlying pattern: **raw-text lowering passes that hand-roll
comment/string/bracket-aware scanning and keep missing cases as the
language grows new attributes**.  The fix is a metaclass one — a
small family of shared primitives in `cc/src/util/text.h`:

- `cc_find_char_top_level` — forward-find a delim character at paren
  depth 0, skipping comments, string/char literals, and balanced
  `()` / `[]` / `{}` groups.
- `cc_find_ident_top_level` — forward-find a word-bounded
  identifier, skipping comments and string literals.
- `cc_rfind_char_top_level` — backward equivalent of
  `cc_find_char_top_level` (with a forward prepass to label comment
  bytes) for decl-boundary walks.
- `cc_find_substr_top_level` / `cc_contains_token_top_level` —
  literal-substring search / presence check, skipping comments and
  string literals; the intended drop-in replacement for raw `strstr`
  on user source text.

The sweep migrated all call sites where the scanned text was
user-originated source (as opposed to compiler-generated metadata /
error-message text) across `preprocess.c`, `async_ast.c`,
`visit_codegen.c`, `pass_autoblock.c`, `pass_match_syntax.c`,
`pass_closure_calls.c`, `pass_closure_literal_ast.c`,
`pass_err_syntax.c`, `pass_ufcs.c`, and `ufcs.c`.  Scans over
already-extracted AST type strings (`aux_s2`, `type_name`, `elem_ty`,
etc.) were left on raw `strstr`/`strchr` — those strings do not
carry comments.

**Policy going forward:** any new lowering pass that needs to
pattern-match against raw user source must use these primitives
instead of `strstr` / `strchr` / byte-by-byte loops.  Treat a new
call to `strstr` or `strchr` on user-source text in a lowering pass
as a review-block comment.

Smoke tests covering the metaclass fix (commit-aware scans under
comment bait) live at:

- `tests/async_param_scan_comment_paren_smoke.ccs` — async_ast
  param-list scan.
- `tests/match_case_header_comment_bait_smoke.ccs` — `@match`
  case-header `.recv` / `.send` discrimination.
- `tests/closure_decl_line_comment_semi_bait_smoke.ccs` —
  pass_closure_calls decl-line `;` / `(` / `=` scan.
- `tests/autoblock_call_comment_paren_bait_smoke.ccs` —
  pass_autoblock `call_txt` paren span extraction.

---

## [F9] async_ast frame-lifts the `!>(e) BODY` binder and emits an invalid declarator — FIXED

### Symptom

Inside an `@async` function, the clean binder shape

```c
bool more = rr_fill(&reader, &client) !>(e) {
    write_reply_now(&client, conn, io_err_reply(e));
    return 0;
};
```

fails to reparse after async state-machine lowering.  The dump shows
`async_ast` has rewritten the unwrap-pass-generated binder declaration
with a frame-member-access LHS:

```c
case 25: {
  __f->more = ({
    __typeof__(rr_fill(&__f->reader, &__f->__p_client)) __cc_pu_x_12
        = (rr_fill(&__f->reader, &__f->__p_client));
    if (__cc_uw_is_err(__cc_pu_x_12)) {
        CCIoError __f->e = *(CCIoError*)(void*)&((__cc_pu_x_12).u.error);  // <-- invalid
        write_reply_now(&__f->__p_client, __f->conn, io_err_reply(__f->e));
        __f->__cc_retval_53 = (0);
        __f->__cc_ret_set_53 = 1;
        goto __cc_cleanup_53;
    }
    __cc_uw_value(__cc_pu_x_12);
  });
  ...
}
```

`CCIoError __f->e = ...;` is not a valid declaration — a member-access
expression can't appear in a declarator.  tcc fails the reparse with
`';' expected (got '->')` pointing at a sibling frame-field assignment
a few lines later.  Regular (non-`@async`) functions are unaffected:
`redis_conn_queue_reply` uses the same binder shape and builds cleanly.

### Evidence

- Repro: adding `rr_fill(&reader, &client) !>(e) { ... return 0; };`
  inside `handle_client` (`@async int handle_client(...)`) in
  `real_projects/redis/redis_idiomatic.ccs`.
- `CC_DUMP_LOWERED` on the same file shows the bogus
  `CCIoError __f->e = ...;` line as above.
- `redis_conn_queue_reply` (plain static fn, same file) uses
  `send(slot) !>(e) { cc_channel_close(tx, e); return false; }` and
  lowers + compiles fine.  The bug is specifically about `async_ast`
  frame-lifting, not the unwrap lowering itself.

### Root cause hypothesis

After [F8]'s fix, `pass_result_unwrap` correctly introduces `e` as a
local of the declared error type (`CCIoError`) inside the error-path
block.  `async_ast`'s local-variable scan then treats `e` as a
frame-lifted local (since the enclosing statement yields a value that's
assigned to a frame field — the `bool more = ...` in the example) and
rewrites every occurrence of the identifier `e` to `__f->e`, **including
the declaration itself**.  The correct handling is either:

1. Leave unwrap-pass-introduced binders as non-frame locals (they're
   scoped to a single block that can't contain a suspension point
   anyway, so they don't need frame storage), or
2. If they must be frame-lifted, emit the declaration as a frame-field
   reference without a type prefix (assignment, not declaration).

Option 1 is the simpler fix — mark the binder declaration site with a
"no-lift" annotation in the unwrap lowering, and teach `async_ast` to
skip such marked locals.

### Fix (applied in `pass_result_unwrap.c`)

The cleanest realization of Option 1 turned out to be name-mangling
rather than a side-channel marker.  All three sites in
`pass_result_unwrap.c` that emit a `!>(e) BODY` / `?>(e) RHS` binder
now rename the user-chosen binder to
`__cc_pu_bind_<id>_<orig_name>` and rewrite every word-bounded
occurrence of the original name inside the error-path body / RHS
accordingly (comment- and string-literal-aware via
`cc_find_ident_top_level`).  The `__cc_pu_` prefix already matches
`async_ast`'s existing no-frame-lift rule for unwrap-pass temporaries
(alongside `__cc_pu_x_*`, `__cc_pu_r_*`, `__cc_pu_be_*`), so the
binder survives into the state-machine lowering as a true local and
its `TYPE name = ...;` declaration stays valid C.

The fix also uncovered a latent pre-existing defect in the stdlib-
predeclared result-spec seeding: `visit_codegen.c` was injecting
`_Generic` arms for every stdlib `CCResult_T_E` regardless of whether
the TU had included the defining `ccc/std/<X>.cch` header.  The seed
now only admits specs whose `CCResult_T_E` or ok-type name is
referenced in the source text.  Together these fixes allow the
idiomatic binder shape

```c
bool more = rr_fill(&reader, &client) !>(e) {
    write_reply_now(&client, conn, io_err_reply(e));
    return 0;
};
```

to build and run inside `@async` bodies.  Regression guard:
`tests/async_unwrap_bang_binder_no_frame_lift_smoke.ccs`.

The outer result *binding* (e.g. `bool more = ...`) as a frame local
is orthogonal and was always fine — only the unwrap-pass-introduced
inner binder tripped the rewrite.  Plain `res.is_err()` /
`res.error()` / `res.value()` accesses on a manually-destructured
result also lower without issue and remain a safe fallback.

---

## [F8] `!>(e) BODY` binder at expression position loses declared error type (becomes `__CCGenericError`) — FIXED (d9ca37f)

### Symptom

At expression position, `CALL !>(e) { ... }` introduces a binder `e`
scoped to `BODY`.  Per spec §3.1 the binder's type should be the error
arm of the result type of `CALL` — for `CCResult_bool_CCIoError` that is
`CCIoError`.  The pre-fix lowering instead bound `e` as the parser's
generic error struct `__CCGenericError`, so any attempt to use `e` as
a `CCIoError` (pass to `cc_channel_close`, read `.kind`, copy into a
`CCIoError` slot) failed to compile:

```c
// idiomatic shape we want at every owner-side channel-send site:
return conn->reply_tx.send(slot) !>(e) {
    cc_channel_close(conn->reply_tx, e);   // expects CCIoError
    return false;
};
```

…errors with:

```
error: cannot convert 'struct __CCGenericError' to 'CCIoError'
```

Statement-position `!>(e) BODY` had the same issue for typed results.

### Fix (d9ca37f)

`pass_result_unwrap`'s expression-position rewrite now threads the LHS
result type's error arm through to the binder declaration the same
way the statement-position decl-form already does.  See the unified
`!>` / `?>` lowering refactor in
`cc/src/visitor/pass_result_unwrap.c`.

### Follow-up

Fixing [F8] surfaced **[F9]** — inside `@async` functions, the now-
correctly-typed binder `e` gets frame-lifted by `async_ast` and the
lowering emits an invalid `CCIoError __f->e = ...;` declaration.
Regular (non-`@async`) functions are unaffected; `redis_conn_queue_reply`
uses the binder form and builds cleanly.  See [F9] above.

---

## [F7] tcc rejects `T x = ({ ... });` as the first stmt of a `case L: {` block — OPEN

### Symptom

Inside a `switch`, the very first statement of a freshly-opened
case-block cannot be a declaration whose initializer is a GCC
statement-expression.  Concretely, the `!>` decl-position lowering

```c
switch (kind) {
    case K_A: {
        CCSlice dst = alloc(arena, len) !>;   // source
        ...
    }
}
```

lowers (correctly, per existing tests) to

```c
switch (kind) {
    case K_A: {
        CCSlice dst =({ __typeof__(alloc(arena, len)) __cc_pu_e_1
                        = (alloc(arena, len));
                        if (cc_is_err(__cc_pu_e_1)) {
                            CCError __cc_pu_be_1 = *(CCError*)(...);
                            return cc_err(__cc_pu_be_1);
                        }
                        cc_value(__cc_pu_e_1);
                      });
        ...
    }
}
```

…and tcc bails with

```
error: '{' expected (got ';')
```

pointing at the `!>;` source line.  The error is emitted **once**
(for the first case-arm of the switch); subsequent arms with the
identical shape parse fine.  clang accepts the lowered output as-is.

### Evidence

- Repro: `real_projects/redis/redis_idiomatic.ccs`,
  `reply_materialize`.  Pre-cleanup it used a manual
  `is_err()/error()/value()` triplet; when collapsed to
  `CCSlice dst = reply_alloc_bytes(arena, n) !>;` (with a function-scope
  `@errhandler`), tcc rejects only the first arm.
- Viewing `CC_DUMP_LOWERED=/tmp/cc_lowered.c` output confirms the
  `case L: {` brace is present and the statement-expr initializer is
  well-formed (matching the parse that clang accepts).
- Non-case-head positions (e.g. `bool has = iter_next(...) !>;` inside
  a `for` loop body in `handle_client`) lower and parse fine.

### Root cause hypothesis

tcc's statement-vs-declaration disambiguator inside a just-opened
compound statement appears to peek one token after `=` expecting
either an rvalue-starting token or `{` (brace-enclosed initializer),
and treats the GCC `({` statement-expression opener as a malformed
brace-initializer rather than as an expression.  Only the **first**
stmt position of a compound has this stricter lookahead; later
positions go through the normal expression parser and handle `({` as
the stmt-expr extension.

### Workaround (applied in `redis_idiomatic.ccs`)

Avoid `!>` in the case-head declaration.  Use an inline-scoped macro
that `return cc_err(...)`s on OOM instead, then write the case arms as
plain `char* dst = alloc(n);` declarations:

```c
static bool !>(CCError) reply_materialize(RedisReply* reply, CCArena* arena) {
    if (!reply || !arena) return cc_err(mkerr(CC_ERR_INVALID_ARG, "null arg"));

#define alloc(n) ({ \
        char* __dst = (char*)cc_arena_alloc(arena, (n), 1); \
        if ((n) > 0 && !__dst) return cc_err(mkerr(CC_ERR_OUT_OF_MEMORY, "reply OOM")); \
        __dst; \
    })

    switch (reply->kind) {
        case REDIS_REPLY_RAW: {
            char* dst = alloc(reply->data.len);
            ...
        }
        ...
    }
#undef alloc
}
```

The `({ ... })` is still in the lowered output, but now as the
expander of a user-written macro rather than as the `!>` lowering's
synthesised initializer, and the expansion happens at a non-head
position once the macro is substituted — so tcc is happy.

### Fix sketch

Two possible fix points:

1. **tcc-level (preferred):** relax the first-stmt-of-compound
   parser so that `= ({` is recognised as a statement-expression
   initializer.  Likely a single-site change in
   `cc/tcc/tccgen.c:decl_initializer` where the parser currently
   eagerly dispatches on the `{` after `=`.
2. **`pass_result_unwrap`-level workaround:** when the `!>` decl
   form appears as the first statement of a compound, emit a leading
   no-op statement (e.g. `;` or `(void)0;`) before the declaration
   so the `({` initializer is no longer at the strict head position.

Option 1 is the right long-term fix.  Option 2 unblocks the idiomatic
source shape at every `!>` call site in case-heads without userland
ceremony.

---

## [F5] `!>(E)` preprocessor swallows `@noblock` / `static` / storage-class keywords into the result-type name — FIXED

### Symptom

Declaring a fast-path function with both a `!>` result type and a
`@noblock` decl-attribute does NOT get the noblock flag — instead, the
result-type preprocessor greedy-scans backward to find the base type
and pulls `@noblock static` into its type-prefix.  Concrete repro from
`redis_idiomatic.ccs`:

```c
// Source:
@noblock static RedisReply !>(CCError) db_execute(RedisDb*, RedisRequest*);

// CC_DUMP_LOWERED output (preprocessed, before tcc parse):
CCResult_noblock_static_RedisReply_CCError db_execute(RedisDb*, RedisRequest*);
//      ^^^^^^^^^^^^^^^^^ swallowed into the type name
```

The generated symbol is `CCResult_noblock_static_RedisReply_CCError`
(an auto-generated result typedef), and because `@noblock` never
reaches the `tccgen.c` decl-attribute hook, `cc_pending_fn_attrs` bit
1 is never set.  `pass_autoblock` therefore treats `db_execute` as a
potentially-blocking callee and wraps every call site in
`cc_run_blocking_task_intptr(...)` — every hot-path op bounces through
the blocking-task threadpool.

### Evidence

- `CC_DEBUG_AUTOBLOCK_CALLS=1` on the owner_loop call:
  `  attrs async=0 noblock=0 under_await=0 chan=0` (expected noblock=1).
- Generated C shows `cc_run_blocking_task_intptr(closure_over(db_execute))`
  at every owner_loop iteration (see `case 12`/`case 13` of
  `__cc_async_owner_loop_*_poll`).
- Removing `!>(CCError)` (e.g. returning plain `RedisReply`) makes
  `@noblock` parse correctly, confirming the preprocessor is the
  culprit.

### Root cause

`cc__rewrite_result_types` (in `cc/src/preprocess/preprocess.c`) maps
`<BASE-TYPE-TOKENS> !>(<ERR>)` to `CCResult_<BASE>_<ERR>` by walking
*tokens backward from `!>`* to gather the base type.  The token walker
treats `@noblock`, `@latency_sensitive`, storage-class keywords
(`static`, `extern`), and type-qualifier keywords as part of the base
type instead of stopping at them.

Expected behavior: stop the backward scan at `@`-attributes and at
storage-class / qualifier keywords; only include actual type tokens.

### Workaround (applied in `redis_idiomatic.ccs`)

Split into a `@noblock` inner helper that returns `CCResult_*` via
direct typedef (no `!>`), plus a thin `!>`-returning wrapper.  The
autoblock pass sees `@noblock` cleanly on the inner helper because
there is no `!>` to confuse the preprocessor.

### Fix sketch for the preprocessor

In the `!>` base-type-prefix scanner, add a keyword blocklist that
stops the backward scan on any of: `@async`, `@noblock`,
`@latency_sensitive`, `static`, `extern`, `inline`, `_Noreturn`,
`register`, `auto`, `thread_local`, `_Thread_local`.  Emit those
kept-prefix tokens verbatim (unchanged) in the rewritten output,
before the synthesised `CCResult_<BASE>_<ERR>` type name.

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

## [F6] async_ast parameter-list / frame-field scanner leaks across function boundaries — FIXED

### Symptom

Adding any new helper function near an existing `@async` function
(whether the helper itself is `@async` or a plain sync `!>`-returning
function) can make the existing `@async` fail to lower, with errors
that look like anything from `"cancel" is not implemented in defer
lowering` to `function without file scope cannot be static` to
`switch expected`.

The common thread: the state-machine frame struct or the entry-point
function's parameter list gets populated with text scraped from
comments, from other functions, or from `!>` expansions — not from
the declared signature.

### Concrete repros (all from the same compilation of `redis_idiomatic.ccs`)

**Frame-field fabrication from comment prose.**  Given

```c
/* Teardown order (cancel-rx, then drop-ref) is encoded in the
 * @destroy body ... */
@async int handle_client(CCSocket client, DbHandle* h, uint64_t conn_id) { ... }
```

async_ast emits a frame struct with fields named after the comment's
hyphenated words:

```c
typedef struct __cc_async_handle_client_60000_frame {
  int __st;
  intptr_t __r;
  struct RedisConn * __f->conn;
  cancel- __p_rx;        // <-- "cancel-rx" from the comment
  then drop- __p_ref;    // <-- "then drop-ref" from the comment
  CCTaskIntptr __t[1];
} __cc_async_handle_client_60000_frame;
```

**Parameter-list scrape from a parenthetical in a neighbouring
comment.**  With a later comment reading
`db serialization is by topology (req channel is N:1, owner is the
only receiver), ...`, async_ast emits the entry-point for the
*previous* `@async` function with that parenthetical *as its parameter
list*:

```c
CCTaskIntptr drain_replies(req channel is N:1, owner is the
 * only receiver) {
  ...
  __f->__p_N = N;
  __f->__p_receiver = receiver;
  ...
}
```

**Parameter-list scrape from a `!>` statement-expression.**  With

```c
RedisConn* conn = redis_conn_create(conn_id) !> { return 1; } @destroy { ... };
```

inside `handle_client`, async_ast picks up the lowered `({ ... })`
block of the `!>` as the entry-point's parameter list:

```c
CCTaskIntptr handle_client({ __typeof__(redis_conn_create(conn_id))
                             __cc_pu_x_4 = (redis_conn_create(conn_id));
                             if (...) { ... } (__cc_pu_x_4); }) {
  ...
}
```

All three variants fail the subsequent tcc reparse with a different
spurious error.  None of them are bugs in the handle_client source —
the declared signature is always
`(CCSocket client, DbHandle* h, uint64_t conn_id)`.

### Trigger conditions

Both of these must hold:

1. Another function (sync or `@async`) sits next to the `@async`
   function in the file, separated only by comments.
2. That other function's body (or the preceding block comment)
   contains either (a) a parenthetical phrase, (b) a hyphen-joined
   word pair, or (c) a `!>` expression that lowers to a statement
   expression.

Under those conditions the scanner mis-identifies what counts as the
`@async` function's signature or frame locals.

### Root cause (hypothesis)

`cc__find_decl_type_in_stmt_list` and the parameter-list extractor in
`async_ast.c` look at raw text and identify declarations / parameters
by pattern — parentheses, commas, identifiers — without tracking
comment or string/statement-expression boundaries.  When a neighbour
emits a plausible-looking `(a, b)` blob nearby, they bind to it.

### Workaround (applied in `redis_idiomatic.ccs`)

1. Keep each `@async` function's body self-contained: do not split
   sub-phases (submit, drain) into sibling `@async` / `!>`-returning
   helpers.  `handle_client` keeps the submit and drain loops inline
   for this reason.
2. Flatten the comments that surround `@async` functions — no
   parentheticals, no `Err(FOO)` / `call(args)` prose, no hyphenated
   phrases like `cancel-rx`.
3. Place plain sync helpers that use `!>` far enough away from any
   `@async` function that the scanner does not cross between them
   (in practice: put them in a different translation unit, or above
   the first `@async` with enough intervening non-`!>` code).

(1) and (2) are what's landed today.  (3) is useful guidance for
future work.

### Fix sketch

Teach the body-scanner in `async_ast.c` to:

- Skip `//` and `/* ... */` regions when searching for parameter
  lists, decls, or frame fields.
- Never cross a `{` / `}` boundary that is not the declared function's
  own body boundary.  Specifically, anchor the parameter-list lookup
  to the `(` that immediately follows the function name token, not the
  next `(` in raw text.
- Reject parameter-list candidates that contain `{`, `;`, or block
  comment markers — those indicate the scanner has escaped into a
  statement expression or a comment.

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
All three are now fixed at the compiler level; any remaining
source-side workarounds in `real_projects/redis/redis_idiomatic.ccs`
can be removed.

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

### [F2] `pass_async` keeps the type prefix on result-typed decls of any shape — FIXED

**Status:** Fixed — see `cc/src/visitor/async_ast.c` +
`tests/async_frame_result_typed_decl_smoke.ccs`.

**Symptom (pre-fix):** a local whose type is a result
(`bool !>(CCIoError) r;` / `RedisReply !>(CCError) r = ...`) inside an
`@async` body whose lifetime crosses a yield point emitted
```
CCResult_bool_CCIoError __f->r = init;       (with init)
CCResult_bool_CCIoError __f->r;               (no init)
```
The decl is not valid C once the var name has been rewritten to a
frame member access, and Phase 3 reparse / real compile both fail.

**Root cause:** async runs *before* `pass_type_syntax.c` rewrites
`T !>(E)` to `CCResult_T_E` (see `visit_codegen.c` — async at
`cc_async_rewrite_state_machine_ast`, result-type rewrite further
down), so the raw-text decl scanners see shapes like
`bool !>(CCIoError) r = maybe_io(i)`.  The scalar/array/struct decl
branches walk ident tokens separated by whitespace to count
`type_tok_n`; the walker broke at the `!` in `!>`, stopped at one
token and failed the `>= 2` check, dropping the statement onto the
generic "emit as-is with ident rewrites" fallback.  `pass_type_syntax`
then rewrote `bool !>(CCIoError)` -> `CCResult_bool_CCIoError` in-place
in the already-lowered body, producing the invalid decl above.

**Fix:** added `cc__skip_ws_and_result_sigil` in `async_ast.c` that,
after the normal whitespace/comment skip, also consumes any
`!>(...)` / `?>(...)` result-type suffix (with full paren matching).
Replaced `q = cc__skip_ws(q)` with `q = cc__skip_ws_and_result_sigil(q)`
at the six token-loop sites in the three decl-strip branches (scalar,
array, struct/other).  The walker now steps across the sigil, finds
the variable-name token on the other side, counts `type_tok_n == 2`
and emits the correct `__f->r = init;` assignment.

Reordering the passes (running `cc__rewrite_result_types_text` before
async) would also fix this but has a much bigger blast radius — other
intermediate passes may rely on the `!>(E)` form; the edge patch in
async keeps the pipeline ordering stable.

### [F3] Orphan unwrap error-tmps hoisted to the frame as an incomplete type — FIXED

**Status:** Fixed — see `cc/src/visitor/async_ast.c` +
`tests/async_orphan_err_tmp_not_hoisted_smoke.ccs`.

**Symptom (pre-fix):** a statement-position `!>` inside an `@async`
body caused async to reserve
```
struct __CCResultGeneric __cc_er_r_N;
```
in the coroutine frame struct.  In real-compile mode
`__CCResultGeneric` is only forward-declared (`cc_result.cch` guards
the definition behind `CC_PARSER_MODE`), so the field has incomplete
type:
```
error: field has incomplete type 'struct __CCResultGeneric'
```

**Root cause:** `pass_err_syntax.c` emits per-site temporaries named
`__cc_er_r_N` / `__cc_er_d_N` when it processes the legacy `@err`
surface.  Result-unwrap `!>` lowerings route through this same pass,
producing the same tmp names.  The expansion lives entirely inside a
`{ ... }` block at the use site (e.g.
`{ __typeof__(CALL) __cc_er_r_N = (CALL); if (cc_is_err(...)) { ... } }`),
so the tmp never needs to cross a yield point.  The async pass's
local-hoist loop saw the name in the stub AST, though, and
speculatively reserved a frame field for it.  It had the same issue
already for `__cc_pu_*` (pass_result_unwrap) and `__cc_ab_*`
(autoblocking) temps — both already explicitly skipped — but no skip
existed for the `__cc_er_*` family.

**Fix:** added `if (strncmp(n[i].aux_s1, "__cc_er_", 8) == 0) continue;`
next to the existing `__cc_pu_` / `__cc_ab_` skips in async_ast's
local-hoist loop.  Option 1 from the original fix menu ("stop
emitting orphan frame fields when the unwrap form is inlined at the
use site"); no frame-field emission path is taken for these tmps at
all after the skip.

### [F4] `DECL_ITEM.line_start` points at a use-site, not the decl — FIXED

**Status:** Fixed — see `cc/src/visitor/async_ast.c` (stmt-list
fallback for result-typed frame fields).  Surfaced after [F1]/[F2]/[F3]
were fixed and the F2 workaround helpers (`rr_fill_split`,
`db_execute_split`) were removed from `redis_idiomatic.ccs`.

**Symptom:** the frame struct for `handle_client`'s `@async` lowering
emitted
```
struct __CCResultGeneric fill_res;
```
instead of the concrete `CCResult_bool_CCIoError fill_res;`, which
then failed to compile (`struct __CCResultGeneric` is only
forward-declared in real-compile mode, per [F3]'s root cause).

The same frame also had `unsigned long s;` and `struct RedisReply * slot;`
fall through the same fallback path — those happened to produce
valid C by accident (`aux_s2` had a concrete spelling), so they
didn't break the build, but the fallback path was firing for them
too.

**Root cause:** async_ast's frame-field type extraction has two
strategies, in order:
1. Scan the source line at `n[i].line_start` for the declarator name
   (`aux_s1`) and take everything before it up to the previous `;`/
   `{`/`}`/`:` as the type prefix.
2. Fall back to `n[i].aux_s2` (the AST's declared type).

Strategy (1) reliably fires when the decl is on a short line near
the top of the function (as in the F2 smoke test), because the
ident's first occurrence on its own line IS the decl.  In
`handle_client`, though, the DECL_ITEM for `fill_res` carried a
`line_start` pointing at a **use site** several lines below the
actual decl — specifically
```
if (!CCResult_bool_CCIoError_unwrap(fill_res)) {
```
(the post-rewrite shape after pass_result_unwrap).  The ident was
found on that line, walk-back hit no terminator, ty_text became
`"if (!CCResult_bool_CCIoError_unwrap("` trimmed, which matched the
`"if"` control-flow-keyword reject filter, dropped ty_text to
`NULL`, and fell through to aux_s2 = `struct __CCResultGeneric`.

Three of `handle_client`'s frame-hoisted locals (`s`, `slot`,
`fill_res`) showed the same pattern — all three had `col_start = 0`
and `line_start` pointing at a non-decl use (for-loop header, if
body, if condition respectively).  For `s`/`slot` the aux_s2
fallback produced valid C (`unsigned long` / `struct RedisReply *`);
for `fill_res` it produced the incomplete struct.

Why `line_start` points at a use: DECL_ITEM nodes can be
"re-pinned" when a later pass rewrites the source (pass_err_syntax
expands `!>` bodies, pass_result_unwrap rewrites `cc_is_err()` /
`._unwrap()` call shapes).  The rebinding picks the ident's FIRST
occurrence in the rewritten text, which for multi-use locals is
typically the initial use, not the decl.

Not worth fixing in the rebind itself: the stub-AST positions
produced by TCC's parser-mode pass are best-effort for any ident
that survives multiple text rewrites, and most passes that need the
original decl position scan the text directly rather than trusting
these positions.

**Fix:** added a third extraction strategy that kicks in when
strategies (1) + (2) would emit an unsafe parser-mode placeholder
(`struct __CCResultGeneric` / `__CCResultGeneric`): scan the already-
built stmt-list (recursively, across nested blocks and if/while/for
bodies) for a ST_SEMI whose text has the shape
`<type-prefix> NAME = ...` or `<type-prefix> NAME`, and take the
type-prefix as the frame-field type.  The stmt-list text is the
current post-rewrite state of the function body and still contains
the literal `bool !>(CCIoError) fill_res = rr_fill(...)` shape
(pass_type_syntax has not run yet on this text), so the emitted
frame field becomes `bool !>(CCIoError) fill_res;` and
pass_type_syntax subsequently rewrites it to
`CCResult_bool_CCIoError fill_res;`.

The stmt-list scan is bounded to the function body (it's the
already-constructed AST of the @async function), so it cannot pick
up unrelated decls from other functions.

**Related note:** the primary line-scan still fires first and still
works in the common case (F2 smoke test path).  The stmt-list
fallback only runs when the line-scan rejects its ty_text as
control-flow noise AND aux_s2 is a parser-mode placeholder.  If the
line-scan is eventually tightened (e.g. by consulting the stmt-list
first to find the real decl line), this fallback can go away.

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
