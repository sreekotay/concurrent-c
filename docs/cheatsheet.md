# Concurrent-C Cheatsheet

[README](../README.md) · [Getting Started](getting-started.md) ·
[The CC Way](the-cc-way.md)

Quick reference. Concepts: [language-concepts.md](language-concepts.md) · recipes:
[examples/README.md](../examples/README.md#learning-path-recommended-order) ·
spec: [spec/](../spec/).

---

## Build & Run

Every unit starts with a **line-1 header** (kind + optional lowerer pin). Emit
strips it; it is not program text. Details:
[backwards compatibility](backwards_compatibility.md).

```text
#!ccc ccs                         # source (.ccs)
#!ccc cch                         # header (.cch)
#!ccc ccs version=0.3             # pin lowerer (usual: MAJOR.MINOR)
#!/usr/bin/env -S ./cc/bin/ccc version=0.3     # script (.shcc); OS shebang
```

`version=MAJOR.MINOR` keeps that file on the 0.3 line when the toolchain
moves. PATCH and SEED (`0.3.3`, `0.3.3-156`) tighten the match. Omit the pin
only when you intentionally want whatever `ccc` is running. Suffix (`.ccs` /
`.cch` / `.shcc`) is the fallback when the header is absent — prefer the
header so kind and pin travel with the file.

```bash
ccc run file.ccs                    # build + run
ccc build run file.ccs              # same, explicit
ccc build run file.ccs -- --arg     # args to the binary
ccc path/to/tool.shcc [args…]       # .shcc → implicit run (shebang-friendly)
ccc --emit-c-only file.ccs          # emit C only → out/file.c
ccc --emit-c-only --no-line a.ccs -o include/generated/a.c
# host-cc of that .c: -DCC_ENABLE_ASYNC on concurrent_c.c; never -DCC_PARSER_MODE
ccc portable-install vendor/cccportable
ccc --cccportable vendor/cccportable --print-cflags   # author snippet
ccc --cccportable vendor/cccportable --print-libs
# consumer host-C tree: docs/cccportable.md
ccc build -O file.ccs               # release (-O2 -DNDEBUG)
ccc build -g file.ccs               # debug (-O0 -g); default is -O2, asserts kept
ccc version=0.3 run file.ccs        # pin lowerer from CLI (same form as header)
ccc --as=ccs file                   # kind when there is no suffix / header
```

Outputs: `./out` (generated C) and `./bin` (binaries), relative to cwd.

---

## Results — declare & consume (`T!>(E)`)

Fallible work returns `T!>(E)`. Produce with `cc_ok` / `cc_err`; consume every
result with `?>` or `!>`. Recipe:
[recipe_result_error_handling.ccs](../examples/recipe_result_error_handling.ccs).

### Declare / return

```c
int !>(CCError) read_config(CCSlice key) {
    if (!key.len) return cc_err(CC_ERR_INVALID_ARG, "empty key");
    if (key.eq_cstr("timeout")) return cc_ok(30);
    return cc_err(CC_ERR_NOT_FOUND, "key not found");
}
```

Inside a function whose return type is `T!>(E)`, `cc_ok(v)` / `cc_err(…)` infer
`T` and `E`. Forms:

| Form | Meaning |
|------|---------|
| `return cc_ok(v);` | success payload `v` |
| `return cc_ok();` | `void!>(E)` success |
| `return cc_err(e);` | propagate / wrap error `e` |
| `return cc_err(CC_ERR_*, "msg");` | build `CCError` when `E` is `CCError` |

Explicit when not in return context: `cc_ok(T, v)`, `cc_ok(T, E, v)`,
`cc_err(T, e)`, `cc_err(T, E, e)`.

### Consume

Two operators; three modifiers:

| | Maps |
|--|--|
| `?>` | `E → T` — stay a value (`x ?> default`) |
| `!>` | `E →` control flow — leave (`x !> { … }` / `x !>;`) |

| Modifier | Does |
|----------|------|
| `(e)` | Exposes `E` (`x !>(e) { … }` / `x ?>(e) …`) |
| bare `!>` | Routes `E` to the scope's `@errhandler` (`x !>;`) |
| `@destroy` | Cleanup on **successful declaration construction** |

```c
@errhandler(CCError e) cc_error_exit(e);   // bare !> routes here

int a = read_config("timeout") ?> 30;
int b = read_config("timeout") !>;                    // routes E
int c = read_config("timeout") !>(e) { /* local */ @err(e); };
int d = read_config("timeout") !>(e) return cc_err(e); // propagate
CCNursery n = cc_nursery_create() !> @destroy;
```

Tasks do not inherit `@errhandler` — re-bind inside each spawn body.

**Anti-pattern:** `T!>(MyError)` instead of `T!>(CCError)` does not force
callers to handle failures — a matching `@errhandler` or blind
`!>(e) return cc_err(e);` is enough. See
[language concepts §2](language-concepts.md#2-errors-map-to-a-value-or-to-control-flow).

### C lowered API

`T!>(E)` is sugar over a tagged union. The lowerer emits a concrete type and
typed constructors; plain C (and `@emit` / generators) use those, not `!>` / `?>`.

```c
/* Sugar                         Lowered */
int !>(CCError)                  CCResult_int_CCError
void !>(CCError)                 CCResult_void_CCError

return cc_ok(30);                return cc_ok_CCResult_int_CCError(30);
return cc_err(e);                return cc_err_CCResult_int_CCError(e);
```

Layout (from `CC_DECL_RESULT_SPEC`):

```c
typedef struct CCResult_int_CCError {
    bool ok;
    union { int value; CCError error; } u;
} CCResult_int_CCError;
```

**Prefer accessors** (work in `.ccs`, plain C, macros, generated code):

| API | Meaning |
|-----|---------|
| `cc_is_ok(r)` / `cc_is_err(r)` | tag |
| `cc_value(r)` / `cc_error(r)` | active arm (only after a check) |
| `r.is_ok()` / `r.value()` | same via UFCS (`.value()` aborts if err) |

```c
CCResult_int_CCError r = read_config("timeout");  /* or: __typeof__(read_config("")) r = … */
if (cc_is_ok(r)) use(cc_value(r));
else handle(cc_error(r));
/* peel after a check: r.ok / r.u.value / r.u.error — C interop detail */
```

In headers that name a Result for C callers, declare once with a guard:

```c
#ifndef CCResult_MyData_MyError_DEFINED
#define CCResult_MyData_MyError_DEFINED 1
CC_DECL_RESULT_SPEC(CCResult_MyData_MyError, MyData, MyError)
#endif
```

Do not hand-spell `CCResult_*` / `cc_ok_CCResult_*` in ordinary `.ccs` /
`.cch` bodies — write `T!>(E)` and `cc_ok` / `cc_err`. Reach for the mangled
names and accessors when operators cannot run (plain C, `@emit`, generators).

---

## `@variant` — data alternatives

Ordinary tagged data — not `T!>(E)`. One arm is always active. Name it at
construction. Read it only when that arm is protected.

**Docs:** [Language Concepts §2a](language-concepts.md#2a-data-alternatives-are-variant) ·
[Getting Started — variants](getting-started.md#variants) ·
[Spec](../spec/draft_variants.md)

**Recipe:** [recipe_variant.ccs](../examples/recipe_variant.ccs)

```c
@variant Cell {
    txt: CCString;
    num: int64_t;
};

@variant Flag { off: void; on: void; code: int; };

Cell n = { .num = 42 };
Cell t = { .txt = cc_string_from("hi", a) };
Flag f = { .on = {} };
```

`{0}` on a variant type is a compile error — it zero-initializes to the first arm
tag, not an empty value. Name the arm explicitly (`{ .num = 0 }`, not `{0}`).

A void arm is `{ .arm = {} }`. `.kind` is read-only. Tags change through construction or whole-variant
assign, not `v.kind = …`. Assign runs the old arm's destroy chain
(same-arm too). A
bare `.arm` resolves from the expected kind type (`if (v.kind == .num)`,
`case .num:`).

Projection (`v.num` / `p->num`) is legal only when protected:

| Protect | Meaning |
|---------|---------|
| `@switch (v)` / `@switch (p)` / `@switch (h.cell)` / `@switch (r->del)` + `case .arm:` / `case .arm(bind):` | each case dominates that arm; optional payload bind; every arm (`default:` forfeits the check) |
| `if (v.kind == .arm)` in the same block | syntactic, not data-flow |
| `v.arm ?> fallback` | inactive → value of the arm's type |
| `v.arm !> { … }` | inactive → handler (must diverge) |

Value, pointer, or **field-path** subject. Prefer `case .arm(bind):` when the
case body needs the payload without a local-copy rebind:

```c
@switch (rec->del) {
    case .text(buf):
        buf.bytes.copy(src) !>;
        break;
    case .pieces(n):
        use(n);
        break;
}

@switch (cell) {
    case .num(n): n += 1; break;
    case .txt(s): use(s.as_slice()); break;
}

int64_t n = cell->num ?> 0;
int64_t req = cell->num !> { return -1; };

cell = { .num = 7 };
```

The variant is data, not `T!>(E)`. Reading an inactive arm is fallible, so
`?>` / `!>` apply to the projection: inactive → fallback or leave. A `!>`
on a Result remains "the call failed".

`@variant(packed)` is at most two arms with a proved niche. Same
surface; no raw `.u`. Three-plus arms stay on the default layout.

A variant `switch` must name every arm; `default:` forfeits the check.
Unprotected projection, two arms in one initializer, and writing `.kind`
or `.u` are compile errors.

---

## Cleanup: `@defer` / `@destroy` / registration

`@destroy` attaches cleanup to **successful declaration construction** — `@defer`
sugar on the binding. After `!>`, that means the unwrap succeeded.

| Form | Meaning |
|------|---------|
| `@defer stmt;` | Always run on scope exit (LIFO) |
| `@defer name: stmt;` | Same, cancellable with `@cancel_defer name;` (idempotent) |
| `@cancel_defer name;` | Disarm that named defer; unknown / wrong-block name is a compile error |
| `@defer(ok) stmt;` | Only on success exit (`return cc_ok(…)` / normal return) |
| `@defer(err) stmt;` | Only on error exit (`return cc_err(…)`) |
| `T x = … @destroy { … };` | Explicit defer body on the binding, then the type’s destroy chain |
| `T x = … @destroy;` | Bodyless → the type’s destroy chain |
| `x.destroy()` | UFCS: `Type_destroy` when that function exists |

The chain: registered pre-destroy → `@destroy { body }` → registered destroy →
each **value** field whose type has a hook, last-declared to first
(transitively). Pointer, array, and function-pointer fields are omitted.
Bodyless `@destroy` with an empty chain is a **compile error**. Stdlib types
ship hooks (`CCNursery`, `CCArena`, channels, …). Register your own:

```c
@typehooks on MyRes {
    .destroy = cc_type_destroy_call("my_res_close"),
};
MyRes r = my_res_open() !> @destroy;
```

Nursery = wait → body → free. Nested struct fields with their own hooks run
after the outer hook.

```c
FILE* f = fopen(path, "w");
if (!f) return cc_err(CC_ERR_IO, "fopen failed");
@defer fclose(f);                 // always
@defer(ok)  commit(path);         // success path only
@defer(err) rollback(path);       // error return only

CCNursery n = cc_nursery_create() !> @destroy;
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;
```

Recipe: [recipe_defer_cleanup.ccs](../examples/recipe_defer_cleanup.ccs).

---

## UFCS

One rule: `recv.method(args)` calls the function the **receiver’s type** names.
**Usual path — no registration:** declaring the function installs the method
(contrast bodyless `@destroy`, which needs a registered destroy chain). Prefer
UFCS over the free-function spelling of the same API. `.destroy()` is that
UFCS path (`Type_destroy`).

```c
n.spawn(() => { … });      // cc_nursery_spawn(n, …)
tx.send(i) !>;
io.println("hi") !>;
v.push(10);                 // CCVec_int_push(&v, 10)
u.mean(6.0);                // mean(u, 6.0) — bare-name tier
get(21)!>.twice();          // unwrap, then method on the value

static double CCVec_double_median(CCVec_double* v) { … }
v.median();                 // declare = install

size_t Box_len(const Box *b);   /* header prototype — other TUs get b.len() */
size_t !>(CCError) Box_read_at(const Box *b, char *d, size_t n);
size_t Box_len(const Box *b) { … }  /* linked definition; not a static wrapper */
```

**Optional registration** (stdlib / your families):
`@typehooks on T { .ufcs = …, }` (strict C designated-init body). Legacy
`cc_type_register` / `cc_type_define`: [deprecated.md](deprecated.md).

```c
@typehooks on MyHandle* {
    .ufcs = my_handle_ufcs_lower_c,
};
```

Last-resort unresolved methods: `.ufcs_sink = cc_type_dynamic_call("callee", "WRAP")`.
Destination-aware: a typed dest composes `<callee>_<mangled dest>` when declared.

Receiver first; **arena last** when the call needs one. That is the
convention and what makes UFCS land on the data, not the arena:
`s.clone_into(a)` is `clone_into(s, a)`. Arena first would be
`a.clone_into(s)`. Recipe:
[recipe_ufcs_forms.ccs](../examples/recipe_ufcs_forms.ccs).
Tutorial: [typehooks-typeviews.md](typehooks-typeviews.md). Spec: `spec/draft_typehooks.md`.

---

## Generics (`Name::[args]`)

`Name::[args]` instantiates a library factory (`CC_GENERIC_FACTORY`). The
emitted `${mangled}_<member>` functions are the methods. `Vec`, `Map`,
`ArrayMap`, and non-char `T[:]` are that same rule — not a separate compiler
container path.

```c
Vec::[int] v@(arena) @destroy;            // struct CCVec_int; destroy releases
v.push(10);                              // dot: Vec is the struct
v.truncate(n);                           // shrink len; n >= len is a no-op
vec_new::[int](arena);                   // same instance
Vec::[char] w = vec_from::[char](p, n, c); // wrap; no grow / no release
static Vec::[int] g;                     // file-scope; same CCVec_int
typedef Vec::[int] Ints;                 // header alias keeps push / reserve
d->runs.truncate(n);                     // Vec field (also @typehooks owners)

Map::[int, double] m = map_new::[int, double](arena);
Map::[size_t, int] n = map_new::[size_t, int](arena);
m->insert(1, 2.5);                       // arrow: Map sugar is Name*

double[:] xs;                            // CCSlice_double (char[:] stays CCSlice)
SmallVec::[int, 8] sv;                   // integer args are factory string slices
SmallVec::[long long, 8] w;              // multi-word types stay one arg
```

`.` = value, `->` = pointer. Recipe:
[recipe_user_generics.ccs](../examples/recipe_user_generics.ccs). Spec
[§12.1](../spec/concurrent-c-spec-complete.md#121-registered-factories).

---

## Print

Prefer `io.println` when a `CCStdio` handle is in scope (`<ccc/stdio.cch>`):

```c
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;
CCStdio io = cc_stdio_create(a);
io.println("hi") !>;
io.println(@string(`n=${n}`, @scratch)) !>;
/* also fine: println("hi") !>;  /  msg.println() !>; */
```

---

## Template strings (`@string`)

Backtick templates interpolate into a `CCString` or a stack `char[:]` borrow.
There is no printf `format` entry point. Spec:
[§9.1](../spec/concurrent-c-spec-complete.md#91-strings).

| Form | Yields |
|------|--------|
| `@string(expr, a)` | `CCString` from a literal or `expr.to_str(a)` |
| `` @string(`…`, a) `` | templated `CCString` in arena `a` |
| `` @string(policy, `…`, a) `` | same; `${…}` / `$~tag{…}` slots go through `policy` |
| `` @string(`…`, @scratch) `` / `@scratch(N)` | same, shared function scratch (default 1 KiB) |
| `` @string(`…`) `` | `char[:]` stack borrow — ints / `bool` / `char` only |

```c
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;
int n = 42;
CCString msg = @string(`n=${n}; price=$100`, a);       // owned
char[:] hdr = @string(`:${n}\r\n`);                    // block-scoped borrow
println(@string(`len=${msg.len()}`, @scratch)) !>;     // throwaway
println(@string(`bulk=${s.sub(0, 4)}`, @scratch)) !>;  // text slice
```

Recipe: [recipe_walk.ccs](../examples/recipe_walk.ccs).

| In the template | Meaning |
|-----------------|--------|
| `${e}` | interpolate (`to_str` when `e` is not already text) |
| `$~tag{e}` | tagged slot (arena + policy); policy sees `"tag"` |
| other `$` | literal (`$100` needs no escape) |
| `\${` / `\$~` | emit `${` / `$~` |
| `${{…}}` | verbatim bytes — no escapes, no slots; inner backticks do not close the literal |

Multiline backticks **dedent** to the closing backtick's margin.

`@scratch` is only the arena operand of `@string`. Every site in a
function or closure shares one stack arena (not per line). Bind the
product (`CCString line =` `` @string(`…`, @scratch) ``) before a
consuming call. `return f(@string(…))` breaks `@destroy` return-rewrite;
a call-local `@string` is reclaimed after that call. To keep a product,
pass the arena it should live on (see [Keep](#keep-pass-the-arena-to-live-on)).
A newline before `@scratch` is fine. Do not `scratch.destroy()`. Do not
capture, send, or `return` a `@scratch` product (`@scratch string escapes
scope`). Arena-less `` @string(`…`) `` is a
compile error on slices, `CCString`, floats, or pointers (pass an arena).
Growth failure poisons the `CCString`; it never truncates.

---

## Structured concurrency

```c
@errhandler(CCError e) cc_error_exit(e);
{
    CCNursery n = cc_nursery_create() !> @destroy;
    n.spawn(() => do_work());
    n.spawn(() => do_other_work());
}
/* both tasks finished — nursery @destroy waited */
```

Nested: `outer.create_child()` parents the inner nursery under `outer`.
Siblings named on the page use `@parallel` (next). The nursery is late
admit — `n.spawn` is not `@parallel spawn`.

Lifecycle: OPEN → JOINING/LEFT → EMPTY → DEAD. `wait` / `@destroy` keep the
handle (OPEN → JOINING → EMPTY → DEAD). `leave` consumes it (OPEN → LEFT →
EMPTY → DEAD). `close(tx)` arms EMPTY to close `tx` on both paths — not
teardown. Optional leftover runs at EMPTY on the LEFT path only (not on
wait / `@destroy`):

```c
n.leave(q, finish_q);   // leftover at EMPTY; not cancel
n.leave();              // leave with no leftover
```

Use either `@destroy` / `wait` or `leave`, not both. Spec §8.1.5.

---

## Walk (`for in`)

The walk is not “a nicer `s[i]`.” `.len` / `.access` are naked (`size_t`,
`T`) — the hook is not Result. A slice / `T[n]` snapshots `.len` and the
data pointer at entry; a grower does the same when the body does not
resize it. Copy walk / enumerate / range are void. Mut walk is
`void !>(CCError)`: if the body can change the subject's extent, a write
re-reads `.len` and `i >= len` is that error (`"for-in write"`) — not a
skip. Zip is also Result (unequal
lengths). Point access is `s.at(i) !>`. Ordinary sites may read `.ptr` /
`.len` / `.id`; they may not store fields. A C string is
`s.to_c(scratch) !>` (`char[:0]`) or `s.to_cstr(scratch) !>` (`char *`).
Users do not write `s.access(i)`.

```c
@for (v in s) { … }            // walk (v is a copy; v = / &v are errors)
@for (&v in s) { … } !>;       // mut walk: v = … is .access store; write bound is Result
@for (i, v in s) { … }         // enumerate; i is size_t
@for (a, b in s, t) { … } !>;  // zip; void !>(CCError); unequal → @errhandler
@for (&a, b in s, t) { … } !>; // zip mut: a = … stores through s's peel
@for (i in lo..hi) { … }       // sequential range; hi < lo is empty
@for (ch in line.sub(lo, hi))  // view; compiler hoists a local
@for (&ch in line.sub(lo, hi)) { … } !>;
s.at(i) !>                    // point (Result)
s.set(i, v) !>
dst.copy(src) !> / dst.copy_overlap(src) !> / dst.fill(c) !>
s.clone_into(a) !>            // arena last
s.eq(other) / s.eq_cstr("x")  // compare slices; not memcmp in CC examples
char *p = s.ptr;              // peel; fields are read-only
```

Subjects: a name, field path, or view (`s.sub(lo, hi)`, `s.trim()`,
`str.as_slice()` / dest-init `char[:] v = str`). A vec is an extent —
`@for (x in v)` walks the grower. Dest-init `int[:] xs = v` is a view
header (same convert as string); `@for (x in v.as_slice())` is that
snapshot. A view is hoisted to a hidden local; mut walk stores
through that header into the receiver. `T*` is not an extent. C `for (;;)`
is unchanged.
`@parallel for` is the concurrent cousin — same `in`, independent iterations.
A text slice interpolates in `${…}`.
Tutorial: [typehooks — extent](typehooks-typeviews.md#extent--len--access).
Recipe: [recipe_walk.ccs](../examples/recipe_walk.ccs).

---

## `@parallel`

Lexical fork-join — names on the page, not a nursery. `@parallel { … }` is
`CCParallel !>(CCError)`. `.wait()` joins. Spec §8.11.
Recipe: [recipe_parallel.ccs](../examples/recipe_parallel.ccs).
Stream (no `n`): [recipe_parallel_stream.ccs](../examples/recipe_parallel_stream.ccs).

| Form | Meaning |
|------|---------|
| `@parallel { a = f(); b = g(); }` | Independent assignment arms. First on the caller; the rest may spawn. |
| `@parallel spawn { … }` | Same arms. Spawned siblings are not denied. A blocking send/recv or a captured channel on an unmarked join is ill-formed. |
| `@parallel(h) { … }` | Growing dest: admit onto `h`. Statement; no `!>`. Snapshot captures. Never deny. Live set; `h.wait()` joins who is still running. |
| `@parallel { h1.wait() !>; h2.wait() !>; }` | Expression arms. No assignment: the expression just runs. |
| `h1.adopt(h2)` | Cancel tree. `h1.cancel()` is child then parent; `h2.cancel()` is child only. |
| `CCParallel h = @parallel { … } !>;` | Starts arms; does not join. `h.live()` is planted and not joined or left. `cc_parallel_empty()` is idle. After `h.wait()`, `h.joined` and `!h.live()`. `h.close(tx)` arms EMPTY to close `tx` on wait and leave. `h.leave()` / `h.leave(ctx, finish)` consume without joining (leftover LEFT-only). Do not mix wait and leave. Next kick overwrites `h`. When there is a kick, the first arm has finished; siblings may still run. One assignment arm is ill-formed: this dest is never live on the caller. One expression arm is the worker (spawned); dest is live. Join with `!>.wait()!>`, or bind the dest. Pointer names copy the pointer; other names are by reference and must outlive `.wait()`. |
| `h.cancel()` | `bool !>(CCIoError)`. `true` = this call stored live→cancelled on `h` or an adopted child. Wakes parks on attached fibers. Pause does not complete a `recv`. |
| `h.live()` | Planted and not joined. Kick: `if (h.live()) return; h = @parallel { … } !>;`. Pause/resume/cancel of idle or joined are `ok(false)`. |
| `h.pause()` / `h.resume()` | `bool !>(CCIoError)`. `true` = this call's transition on a live dest. Does not require `.wait()`. Construct honors at thunk entry / next for-half / next leaf `i` / wait-for enter / after `@stage` wait. |
| `h.paused` / `h.paused()` | Atomic flag. Safe to poll from a sibling while `pause()` / `resume()` store. Visible after `h.wait()`. |
| `h.cancelled` | Atomic flag. Safe to poll from a sibling while `cancel()` stores. Visible after `h.wait()`. |
| void host | `h.wait() !>(e) { (void)e; };` — UFCS `!>` lowers in void. No Result wrapper. |
| `@serial { …; a = t; }` | Sequential block as one sibling. Ordinary C; zero or one outer name. |
| `@parallel spawn { @serial { …; tx.close(); } @serial { while (recv) } }` | On-page stream. Close next to produce. `.wait()` joins both. Not a nursery. |
| `@parallel (pred) { … }` | Same arms. Spawn if `pred`; otherwise run in order. Body always runs. |
| `@parallel for (i in lo..hi) { … }` | Independent iterations over `[lo, hi)`. Bisects; span 0 or 1 is a plain `for`. |
| `@parallel wait (ts) for (i in lo..hi)` | Ordered spawn loop on a turnstile. Type: `bool !>(CCError)` — `true` if the range finished. `CCParallel h = … !>;` is live during enter; the statement joins (§8.11.6). |
| `cache (zs)` | After `wait`: adopt enclosing scratch; instance identity unobservable. |
| `@stage (ts.read, i) { … }` | Ticket handshake in a wait-for body; pass on every exit. Not a Result. |
| `break` / `continue` / `return` | Same as `for`. Parallel path drains first. `return` is `break` then `return`. Two returns: unspecified which value (drain still happens). Join: `return` joins sibling arms first. Wait-for `break` is `ok(false)` and must be bound. |

```c
int a = 0, b = 0;
@parallel {                    // independent names; may deny
    a = f();
    b = g();
} !>.wait()!>;

CCParallel h1 = @parallel { a = f(); b = g(); } !>;
CCParallel h2 = @parallel { c = p(); d = q(); } !>;
@parallel {
    h1.wait() !>;
    h2.wait() !>;
} !>.wait()!>;

@parallel (d < k) {            // spawn if pred; else run in order
    @serial {
        int t = f();
        a = t;                 // exactly one outer name
    }
    b = g();
} !>.wait()!>;

@parallel for (i in 0..n) {    // half-open; bisects
    work(i);
} !>.wait()!>;

bool fin = @parallel wait (ts) for (i in 0..n) {
    @stage (ts, 0, i) { work(i); }
    if (done) break;           // ok(false); bind the bool
} !>;

int[~4 >] tx;
int[~4 <] rx;
cc_channel_pair(&tx, &rx) !> @destroy;
@parallel spawn {              // meeting: both arms live; do not deny
    @serial {
        for (int i = 1; i <= 3; i++)
            tx.send(i) !>;
        tx.close();            // EOF on the page
    }
    @serial {
        int v;
        while (cc_io_avail(rx.recv(&v)))
            use(v);
    }
} !>.wait()!>;
```

`@serial` is only a direct child of `@parallel { }`. Bare `{ }` is not an
arm. `for` as a direct child of `@parallel { }` is an error; `for` inside
`@serial` is ordinary C. The construct does not wait for a ticket that
has not returned. `@parallel spawn` admits a meeting. `n.spawn` admits
into a bag. They are not the same word.

---

## Channels

`T[~N >]` is a send end and `T[~N <]` is a receive end; `N` is the
buffer capacity.

Default pipeline is two `@parallel spawn` arms and `tx.close()` on the produce
arm — [recipe_parallel_stream.ccs](../examples/recipe_parallel_stream.ccs).
`n.close(tx)` arms EMPTY when the set is not on the page (dest-live /
leave): [recipe_channel_pipeline.ccs](../examples/recipe_channel_pipeline.ccs).

---

## Closures / captures

Spawn takes a closure. `[x]` copies `x` into the task and the copy is
immutable. `[&x]` shares the same variable for read-only access; mutation
requires shipped synchronization or an explicit `@unsafe` closure.

```c
n.spawn(() => { … });                 // no capture list
n.spawn(() => [x] { use(x); });       // immutable value copy
n.spawn(() => [&x] { use(x); });      // shared, read-only reference
```

Re-bind `@errhandler` inside the task. Do not capture stack / `@scratch` slices
past the frame; arena slices pin the arena until join.

---

## Named exclusive (`CCExclusive`)

Short critical sections on a `uint64_t` name. Do not `@await` under the hold.

```c
CCExclusive excl = cc_exclusive_create(arena, 0) !> @destroy;
CCExclusiveGuard g = excl.acquire(name);
… mutate …
g.release();

/* Park until pred is true *under* the name. Not a condvar-on-held-guard. */
CCExclusiveGuard w = excl.acquire_when(name, pred, env) !> @destroy;
/* held; pred was observed true while held */
```

The holder that makes `pred` true signals **while still holding**: `h.signal()`
/ `h.broadcast()`. The wait parks a fiber or an OS thread. An expired
`cc_current_deadline()` (`@with_deadline` / `cc_deadline_push`) is
`CC_ERR_TIMEOUT`. A cancelled deadline, or a cancelled nursery when one
exists, is `CC_ERR_CANCELLED`. No nursery is not cancel. Errors are
`CCError` — never a zeroed guard. `acquire_when_into` runs a builder once
under that invariant and releases.
Recipe: [recipe_exclusive_named.ccs](../examples/recipe_exclusive_named.ccs).

---

## Pipeline turnstile (`CCTurnstile`)

Depth cap plus ordered stages. Declare the turnstile **before** the nursery.

```c
CCTurnstileRW ts@(cap, arena) !> @destroy;
ts.enter(i) !>;
ts.read.wait(i) !>;   …  ts.read.pass(i) !>;
ts.write.wait(i) !>;  …  ts.write.pass(i) !>;
ts.leave() !>;

CCTurnstile t@(cap, n_stages, arena) !> @destroy;
t.enter(i) !>;
t.stage(k).wait(i) !>;  t.wait(k, i) !>;
```

Create is Result — `cap < 1`, a dead arena, or depth-channel OOM is
`CC_ERR_*`, not a dead value. `enter(i)` takes a depth token. Stage
`wait`/`pass`/`fail` are `void !>(CCError)`: a predecessor that errors
`fail`s the gate so a parked `wait` wakes with `err`, not `ok`. A closed
depth channel is an error, not `Ok(false)`.
`@parallel wait (ts) for` owns enter/leave — [recipe_parallel.ccs](../examples/recipe_parallel.ccs).
Two named stages: [recipe_turnstile.ccs](../examples/recipe_turnstile.ccs).

---

## Arenas name a lifetime

**An arena is a named lifetime, not an allocator strategy.** Storage is
three tiers (cache-shaped): **L1** root slab, **L2** grown extents, **Main**
overflow. Constructors pick how those tiers are obtained — always named
explicitly (`CCArena a = …`); there is no ambient or hidden arena.

Size L1 for the typical live set. Default heap/stack: bump in L1 → up to 4
slabs (L2, ~1.5×) → **Main** (`malloc`, still arena-owned; freed on reset /
`@destroy`). `a.live()` counts all three tiers.

```c
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;  // names the lifetime
char* p = a.allocT(64);
char[:] s = a.alloc_slice_bytes(32);   // arena provenance
/* C API: pass CCArena by value — cc_arena_alloc(a, n, align), cc_dir_cwd(scratch); not &a */

cc_arena_stack(tmp, 1024);             // same policy; L1 on the stack; @destroy at scope exit
cc_arena_buf(win, frame, sizeof frame); // same sugar; caller L1 (no VLA)
a.reset();                             // drain epoch; reuse L1

CCArenaCheckpoint cp = a.try_checkpoint() !> @destroy; // consumed loan
/* …scratch, including Main… */
cp.try_restore() !>;                   // or leave the scope: @destroy restores
```

Slices (`T[:]`) carry provenance. Views must not outlive their arena.
A mid-slab hole disables a new capture until last-live rewind or `reset`.
Restore of a handle whose overflow keep-set was released refuses.
`a.detach() !>` moves heap-owned mallocs only — a stack or caller L1 is refused.
Details: [getting-started § Arenas](getting-started.md#arenas-name-a-lifetime).

### Lifetime parents: attach / adopt / `create_*`

An arena is also a **lifetime parent**: it holds destroy records for other
objects and runs them newest-first (LIFO) at `free` **before** releasing
storage. `reset` runs the same walk — **attached children are contents and
die at reset** (the arena stays live and can attach again).

```c
CCArena owner = cc_arena_heap(kilobytes(4)) @destroy;

owner.attach(obj, obj_down);               // destroy record; fires at free/reset
CCArena child = owner.create_arena(512) !>; // L1 carved from owner: storage-bound
CCArena kid   = owner.create_arena(0) !>;   // heap-backed: movable
CCArenaPool* p = owner.create_pool(32) !>;  // pool on owner; no explicit destroy

/* Stack pool: place an 8-aligned handle. Do not declare CCArenaPool by value. */
cc_arena_pool_handle(pool);
if (!pool || cc_arena_pool(pool, 32) != 0) ...
@defer cc_arena_pool_destroy(pool);
/* `@parallel` capture needs the names spelled (the macro's identifiers are not captured): */
unsigned char pool_raw[CC__ARENA_POOL_HANDLE_BYTES];
CCArenaPool *pool = cc__arena_pool_place(pool_raw, sizeof(pool_raw));

CCArena tmp = cc_arena_heap(256) @destroy;
CCArena tmp2 = cc_arena_heap(256) @destroy;
CCArena moved = owner.adopt(&tmp) !>;       // move: tmp handle nulled; record attached
CCArena m2 = owner.try_adopt(&tmp2) !>;     // Result face (cc_arena_result.cch)
```

A `create_*` result carries no scope sigil — the owner holds the obligation.
A move consumes its source: the scope `@destroy` on `tmp` above discharges
vacuously on the dead husk.
Refusals are Result (`cc_err`), source untouched: dead parent or source,
parent mid-teardown, self/cycle, storage-bound source L1 (stack / caller /
carved), outstanding checkpoint loans. A second adopt of the same handle
refuses (dead source). Spec:
[draft_lifetime_parents.md](../spec/draft_lifetime_parents.md). Worked
examples: `tests/arena_lifetime_parent_smoke.ccs`.

---

## Keep: pass the arena to live on

`@scratch` is throwaway — gone after the consuming call. To **keep** a
product, pass the arena it should live on. That arena is the **last**
parameter (convention, and so UFCS binds the data: `s.clone_into(a)`
is `clone_into(s, a)`).

```c
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;

io.println(@string(`tmp`, @scratch)) !>;     // gone after println

CCString keep = @string(`keep me`, a);       // lives on `a`
CCString out  = cc_script_sh_read_at(keep, a) !>;

CCString load(CCSlice path, CCArena a);      // arena last → keep on `a`
s.clone_into(a);
```

Recipe: [recipe_arena_scope.ccs](../examples/recipe_arena_scope.ccs).
Walk / clone: [recipe_walk.ccs](../examples/recipe_walk.ccs).

---

## Locality (program shape)

Memory is owned or it is a view. Lifetime is a field. A constructor assumes
dead — reopen is `d.destroy(); d.from_view(src) !>;`, not `memset`. Fallible
work is `T!>(E)`; a giving-up path is **unchanged** or **`broken`**, not a
zero that looks like success. `char[:]` is `{ptr, len, id}` (storing it does
not take the bytes; `id == 0` is untracked). Faces (`@typeview`) are decided
at the use site.

[Getting started](getting-started.md#locality-owned-or-view) ·
[recipe_owned_view.ccs](../examples/recipe_owned_view.ccs) ·
[typehooks-typeviews.md](typehooks-typeviews.md).

---

## Absence (no `T?`)

| Shape | Use when |
|-------|----------|
| `T*` / bool+out | missing lookup / pop |
| empty slice | no bytes, or “not one piece” — not automatically EOF |
| `T!>(E)` | operation failed |

---

## Timeouts & cancellation

Recipe: [recipe_timeout.ccs](../examples/recipe_timeout.ccs).
`cc_sleep_ms` waits out its argument; poll the clock or the handle.

```c
@with_deadline(millis(50)) {
    while (!cc_deadline_expired(cc_current_deadline())) {
        do_work();
        cc_sleep_ms(10);
    }
}

@with_deadline(millis(50)) as dl {       // bind the clock
    while (!cc_deadline_expired(dl)) { … }
}

@with_deadline(dl) {                      // same object, now current here
    recv() !>;                           // sees D; does not mint a clock
}

if (cc_is_cancelled()) return;           // that deadline, not a nursery

CCParallel h = @parallel {
    @serial {
        while (!cc_deadline_expired(cc_current_deadline()))
            cc_sleep_ms(10);
        bool did = h.cancel() !>;        // true = this call did the transition
        saw = did ? 1 : 0;
    }
    @serial {
        int n = 0;
        while (!h.cancelled) {           // atomic; spawned arms do not see the clock
            n++;
            work();
        }
        ticks = n;
    }
} !>;
h.wait() !>;                             // required: bind does not join
```

---

## Async / await

Prefer `@parallel` when the siblings are on the page. Prefer `n.spawn`
when the set is not. Prefer `@async` / `@await` for one suspendable call
stack. Recipe: [recipe_async_await.ccs](../examples/recipe_async_await.ccs).

```c
@async int bump(int value) { return value + 1; }

int main(void) {
    @errhandler(CCError e) { cc_error_log(e); return 1; }
    int result = @await bump(41);
    return result == 42 ? 0 : 1;
}
```

---

## `.shcc` scripts

Same language as `.ccs`; script prelude + synthetic `main` when you omit
`main`. Ambient `a` / `io` / `in` / `args` when those names appear.
`ccc tool.shcc` is an implicit run. See
[getting-started § `.shcc`](getting-started.md#shcc-scripts) and
[spec §9.5](../spec/concurrent-c-spec-complete.md#95-script-library-shcc--cccscript).

```bash
ccc examples/py/pydemo.shcc         # CC hosts Python
ccc examples/js/jsdemo.shcc         # CC→JS (guest; Node owns env)
./tools/perf.shcc @                 # list @task entries
```

---

## Common patterns (pointers)

| Pattern | Recipe |
|---------|--------|
| `@variant` tagged data | [recipe_variant.ccs](../examples/recipe_variant.ccs) · [spec](../spec/draft_variants.md) · [§2a](language-concepts.md#2a-data-alternatives-are-variant) |
| Walk / dest-bulk buffers | [recipe_walk.ccs](../examples/recipe_walk.ccs) |
| Owned or view / reopen | [recipe_owned_view.ccs](../examples/recipe_owned_view.ccs) |
| Worker pool | [recipe_worker_pool.ccs](../examples/recipe_worker_pool.ccs) |
| `@parallel` (join, range, wait-for ticket) | [recipe_parallel.ccs](../examples/recipe_parallel.ccs) |
| On-page stream (`tx.close()` in produce) | [recipe_parallel_stream.ccs](../examples/recipe_parallel_stream.ccs) |
| Ordered stream (`send_task` + FIFO recv) | [recipe_ordered_parallel.ccs](../examples/recipe_ordered_parallel.ccs) |
| Prepare A+B / hold / commit | [recipe_prepare_commit.ccs](../examples/recipe_prepare_commit.ccs) |
| Channel pipeline | [recipe_channel_pipeline.ccs](../examples/recipe_channel_pipeline.ccs) |

---

## Build system (`build.cc`)

```c
CC_TARGET main exe main.ccs utils.ccs
CC_TARGET_LIBS main pthread
CC_DEFAULT main
```

```bash
ccc build                           # default target
ccc build run                       # build + run default
ccc build list
ccc build --build-file path/build.cc
```

---

## Python & JS

```c
#include <ccc/script/py.cch>
#include <ccc/script/js.cch>
```

**Embed Python** (CC owns `main`): [recipe_py_interop.ccs](../examples/recipe_py_interop.ccs).

**One file → native modules** (no `main` + export): [js-py-modules.md](js-py-modules.md).

```c
@comptime cc_py_export("counter", "Counter", &seed);   // → counter.abi3.so
@comptime cc_js_export("counter", "Counter", &seed);   // → counter.node
```

```bash
ccc build counter.ccs
PYTHONPATH=bin python3 -c "import counter; counter.bump(4)"
```

Bridges: npm [`concurrent-c-python`](https://www.npmjs.com/package/concurrent-c-python)
(Python from Node — default call blocks; `py.task` for a Promise; `{ isolated: true }`
is a child interpreter, not a different calling convention),
pip [`concurrent-c-node`](https://pypi.org/project/concurrent-c-node/)
(JS from Python — always a `node` child; thenables awaited in the child;
Jupyter/Colab: `from cc_node import require`).

---

## Environment

| Variable | Purpose |
|----------|---------|
| `CC` | Host C compiler |
| `CC_OUT_DIR` | Generated C + objects (default `out/`) |
| `CC_BIN_DIR` | Linked executables (default `bin/`) |
| `CC_HOME` | Override install tree resolution |
| `CC_NO_CACHE` | Disable incremental cache |
| `CC_CACHE_MAX_MB` | Cache cap (0 = uncapped) |
| `CC_CACHE_EVICT_INTERVAL` | Min seconds between sweeps (default 60) |

---

## Includes

```c
#include <ccc/cc_runtime.cch>      // nurseries, channels, core
#include <ccc/std/prelude.cch>     // kilobytes, vec/map/dir, … — not <stdio.h> / <string.h>
#include <ccc/stdio.cch>    // CCStdio / io.println
#include <ccc/script/prelude.cch>  // forced in for .shcc (<stdio.h> + std prelude); not <string.h>
#include <ccc/cc_atomic.cch>       // portable atomics
#include <stdio.h>                 // printf, … — include when you use C stdio
#include <string.h>                // memcpy, strcmp, … — include when you use C strings
#include "leaf.cch"                // local face; nested .cch is fine
```

A local `.cch` with statement unwrap (`!>(e) {`) is spliced into the
including unit only when that unit is a `.ccs` (or an already-spliced
impl face). `T !>(E)` on a declaration does not force that, and neither
does a `.foo(` in an interface header included from a `.ccs`. A
`@typehooks` / `@typeview` face still extracts to a lowered `.h`; callers
keep `char[:]` argument wrap and the proto's Result error type from the
original `.cch`. A quoted interface `.cch` extracts; nested includes
become their own `.h` (impl-grade nested faces need an owner `.ccs`,
or a direct include from that `.ccs`). `foo.ccs` owns `foo.cch`,
`foo_*.cch`, a same-directory `.ccs` that includes the chapter
(`document.ccs` → `utf8.cch`), and any same-directory face those files
include (`workspace.cch` → `ui_types.cch`). A `.ccs` include wins over
a face-includer. Those faces extract as decls in every other TU. One
include in one TU is extract or splice, not both. The owner splices
the bodies after the extracted parent include. The including TU's
`#include "foo.cch"` stays in source order so types declared above it
are in scope. Nested quoted includes
inside the extracted face hoist only when that face defines a name this
face uses, and they land after this face's definitions of names the
included face uses (`RtxBuf` before `ui_types.h`). A consumer leaf
included last (`nav.cch` after `RtxDoc`) stays put. Those includes
rewrite to the lowered `.h` path.

An object-like `#define FLAG` immediately before `#include "foo.cch"`
stays in this TU; `#ifdef FLAG` inside the extracted `.h` is host cpp,
including function bodies under that `#ifdef`. File-scope functions in
a `.cch` live in the owner TU; other TUs see decls of non-`static`
functions. File-scope `static` on a function stays `static` in the
owner splice and is omitted from the extract. An unowned impl-grade
face whose file-scope functions are all `static` splices a private copy
per TU. A non-`static` function on an unowned face may appear in one TU;
a second TU needs those functions `static` or an owner `.ccs`.
`#pragma(@per_tu)` is optional and requires all file-scope functions
`static`. A file-scope data definition becomes
`extern` in the extract; `static` data stays in the extract and is not
repeated in an owner include-graph splice.
A pointer type in a declaration (`Tag *name` in a parameter, file-scope
declarator, or struct field) that the face does not already name as a
type is not a guessed `typedef struct Tag Tag`; if exactly one
same-directory face defines the name and that face can extract, the
extract includes that face. If none does, and exactly one face in the
including unit's include graph does, extract includes that face. A
multiply in a function body is not a pointer type. `CC_MAP_DECL_*` /
`CC_DECL_SLICE_SPEC` / `CC_DECL_RESULT_SPEC` name the type they bind.
Two definers with different owners, or none (and the including unit
does not define it), is an error. Same-owner chapters (`foo.cch` / `foo_priv.cch`) count as
one; extract includes the stem. An impl-grade unowned parent is left to the including unit (already
spliced). Nested includes inside an extracted `.h` are relative to that
`.h`, not an absolute path.
