# Tutorial: `@typehooks` and `@typeview`

Two file-scope declarations attach policy to a type. They share the
`on Subject` shape and nothing else — do not merge the bodies.

| | `@typehooks` | `@typeview` |
|--|--|--|
| Answers | How this type is created, destroyed, walked, and (optionally) how `x.method` lowers | Which names a caller may use, and which embedded fields are “is-a” faces |
| Typical job | Bodyless `@destroy` / `name@(args)` for *your* type | A wrapper that should act like the field it contains, or a narrower face of one object |
| Body | Strict C designated init: `.destroy = …,` | Groups: `as:`, `r:`, `w:`, `rw:` |

You do **not** need `@typehooks` to add a method. Declaring
`static int box_bump(Box* b)` already installs `b.bump()` and `bptr->bump()`. Hooks are for
lifecycle and for library-owned naming families.
Methods come from ordinary functions. Naming-policy hooks (including stdlib’s `*`) are how whole families share a convention without the compiler special-casing type names — you can read the hook; it isn’t magic.

Subject may be an exact type (`Box`), a pointer key (`MyRes*`), or a
trailing-`*` family (`Fam_*`). Same match rule on both forms:
**narrowest pattern wins**; two equal-score matches are ill-formed.

---

## 1. `@typehooks` — lifecycle

Write the functions. Then name them on the type. Bodyless `@destroy` and
`name@(args)` call those functions. Bodyless `@destroy` needs a non-empty
destroy chain (a hook on the type, or on a value field’s type).
Stdlib types already ship one (`CCArena`, `CCFile`, `CCNursery`, …).

```c
#!ccc ccs
#include <ccc/std/prelude.cch>
#include <stdio.h>

typedef struct {
    int fd;
    int alive;
} Port;

static Port port_open(int fd) {
    Port p;
    p.fd = fd;
    p.alive = 1;
    return p;
}

static void port_close(Port* p) {
    if (!p || !p->alive) return;
    p->alive = 0;
}

@typehooks on Port {
    .create = port_open,
    .destroy = port_close,
};

int main(void) {
    Port p@(3) @destroy;   /* port_open(3), then port_close */
    printf("fd=%d\n", p.fd);
    return 0;
}
```
<!-- smoke-stdout
fd=3
-->

Rules that matter in practice:

- The body is a **C designated initializer**: leading `.`, one RHS per arm,
  commas between arms, trailing comma allowed.
- `.create` / `.destroy` / `.ufcs` are the functions themselves.
- `name@(args)` picks the create function from the **declared type** on
  the left plus the argument list.
- `name@(args)` must be followed by
  `@destroy` or `@detach`. Omitting both is an error.
- `@destroy` attaches cleanup to **successful declaration construction**,
  not only to `name@(args)`. `Port p = {0} @destroy;` and
  `Port p = port_open(3) @destroy;` run the destroy chain at scope
  exit the same way. After `!>`, construction succeeded only if the
  unwrap did.
- `@destroy;` runs the type’s destroy chain: registered pre-destroy →
  registered destroy → value fields with hooks, last-declared to first.
  `@destroy { … }` inserts the block between pre-destroy and destroy.
  `x.destroy()` is UFCS (`Type_destroy` when that function exists). See §3.

Pointer and family subjects use the same body (`MyRes*`, `CCChanTx_*`).
A trailing-`*` subject is one registration for every match. `.ufcs`
still sees the concrete `recv_type`, so the hook can compose a per-type
name. An exact `@typehooks on Fam_alpha` beats `@typehooks on Fam_*`.

### UFCS — the compiler asks you for a callee

`p.put(data)` is not a field. `.ufcs` runs at compile time, sees the
name after the dot, and returns the callee as `char[:]`. Special cases
in the `switch`; everything else is `port_` plus the method name.

Anyone else can then declare `port_ready` — first parameter `Port*` —
and `p.ready()` just works. No edit to the struct. No new `@typehooks` arm.

```c
#!ccc ccs
#include <ccc/std/prelude.cch>
#include <ccc/cc_ufcs.cch>
#include <stdio.h>

typedef struct {
    int fd;
    int alive;
} Port;

static Port port_open(int fd) {
    Port p;
    p.fd = fd;
    p.alive = 1;
    return p;
}

static void port_close(Port* p) {
    if (!p || !p->alive) return;
    p->alive = 0;
}

static int port_put_2(Port* p, char[:] data) { /* .put → this name, not port_put */
    (void)p;
    return (int)data.len;
}

static char[:] port_ufcs(char[:] recv_type, char[:] method, char[:] mode,
                         CCSliceArray argv, CCSliceArray arg_types,
                         CCArena arena) {
    (void)recv_type;
    (void)mode; /* named `@typeview Mode on T` — otherwise empty */
    (void)argv;
    (void)arg_types;
    @switch (method) {
    case "put": return @slice("port_put_2");
    default:    return @string(`port_${method}`, arena).as_slice();
    }
}

@typehooks on Port {
    .create = port_open,
    .destroy = port_close,
    .ufcs = port_ufcs,
};

/* other files: compose port_<method> and it is a method */
static int port_ready(Port* p) {
    return p != NULL && p->alive;
}

int main(void) {
    Port p@(3) @destroy;
    int n = p.put("hello");    /* switch → port_put_2 */
    int ok = p.ready();        /* default → port_ready */
    printf("n=%d ok=%d fd=%d\n", n, ok, p.fd);
    return 0;
}
```
<!-- smoke-stdout
n=5 ok=1 fd=3
-->

`.ufcs` only chooses the name. It does not run the call. `.ufcs_sink` is
the last resort when nothing resolved (`obj.greet(...)`).

That power is also the cost. `p.put(...)` never mentions `port_put_2`, so
go-to-definition and grep on the call site will not find the callee. If
the composed name does not exist (`p.gone()` → `port_gone`), compile
fails at that call — same source line — as a type error, not a host
implicit-function rescue:

```
file.ccs:43: error: type: no UFCS method 'gone' for receiver type 'Port'
candidate port_gone (hook compose): not declared
```

Grep `port_gone` in your sources to find (or write) the callee.

### Extent — `.len` / `.access`

A type with both arms is a **for-in subject**. Ordinary sites may read
`x.len`; they may not store it. `.len` / `.access` are naked (`size_t`,
`T`) — the hook is not Result. `.access` is the compiler-internal slot
after `i < live len`. Users write the walk, not `s.access(i)`. Copy
walk / enumerate / range are void. Mut walk is `void !>(CCError)`: a
write re-reads `.len` when the body can change the subject's extent, and
`i >= len` is that error (`"for-in write"`),
not a skip. A slice / `T[n]` snapshots `.len` and the data pointer at
entry; a grower does the same when the body does not resize it. Zip is
also Result (unequal lengths). Point access stays
`s.at(i) !>` / `s.set(i, v) !>`. Slice fields (`.ptr` / `.len` / `.id`)
are readable and read-only. `CCString` hides the SSO union; use
`as_slice()` / `cstr()`.

`CCSlice` / `CCSlice_*`, `CCVec_*`, `CCString`, and `T[n]` already
register. `T*` is not an extent.

```c
#!ccc ccs
#include <ccc/std/prelude.cch>
#include <stdio.h>

int main(void) {
    int[:] xs = { 1, 2, 3 };
    int[:] ys = { 10, 20, 30 };
    int sum = 0;
    @for (v in xs)
        sum += v;
    @for (i, v in xs)
        sum += (int)i + v;
    @for (a, b in xs, ys) {
        sum += a * b;
    } !>;
    printf("%d\n", sum);
    return 0;
}
```
<!-- smoke-stdout
155
-->

| Form | Meaning |
|------|---------|
| `@for (v in s)` | walk (copy; `v =` / `&v` ill-formed) |
| `@for (&v in s) { … } !>;` | mut walk; `v =` is `.access` store; write bound is Result |
| `@for (i, v in s)` | enumerate; `i` is `size_t` |
| `@for (a, b in s, t) { … } !>;` | zip; `void !>(CCError)`; unequal → `@errhandler` |
| `@for (&a, b in s, t) { … } !>;` | zip mut; `a =` stores through `s` |
| `@for (i in lo..hi)` | sequential range; `hi < lo` is empty |

C `for (;;)` is unchanged. `@parallel for (i in lo..hi)` is the concurrent
cousin. A user type registers the same two arms (`tests/typehooks_len_access_smoke.ccs`).

Mut walk and zip hard-wire `CCError` — part of why fallible APIs should not
use a custom `E` to “force handling” ([language concepts §2](language-concepts.md#2-errors-map-to-a-value-or-to-control-flow)).

---

## 2. `@typeview` — faces and allow-lists

A view is a **type-system lens on the same object**. No second allocation,
no vtable, erased in the lowered C. Narrowing is implicit (like `T*` →
`const T*`). Widening never happens — there is no cast back to the full type.

Two mechanisms share one `@typeview` block:

| Mechanism | Groups | Job |
|-----------|--------|-----|
| **Is-a faces** | `as:` | UFCS (and related) retry through an embedded field — outer *is* inner for method lookup |
| **Allow-lists** | `r:`, `w:`, `rw:` | Which field loads, stores, and UFCS calls ordinary sites may use |

Groups are comma-separated patterns ended by `;` (or `}`). A bare comma-list
with no label is sugar for a single `r:` group.

### Pattern language

Patterns appear in two places — **subject** (after `on`) and **allow-list**
entries (`r:` / `w:` / `rw:`).

| Pattern | Where | Meaning |
|---------|-------|---------|
| `Box` | subject | exact type |
| `Fam_*` | subject | type-family glob — every struct whose name matches; **narrowest pattern wins**; equal score → ill-formed |
| `len`, `write` | allow-list | exact field or UFCS method name |
| `out_*`, `get_*` | allow-list | **prefix** glob — name starts with the literal before `*` |
| `*_len`, `*live` | allow-list | **suffix** glob — name ends with the literal after `*` |
| `*` | allow-list | every field and UFCS method on the type |
| `^secret`, `^p` | allow-list | **deny** — subtract this name after the allow-set is built; a matching deny wins |
| `r:^p` alone | allow-list | deny-only group implies `*` first — same as `r: *, ^p` |

**Subject globs** install the same view body on every matching type (`Fam_*`,
`CCSlice_*`, `CCBox_*`). Types that match but lack an `as:` field are
skipped. Unnamed (`@typeview on Fam_*`) is the ordinary surface of each
match. Named (`@typeview Encode on Fam_*`) is one mode for the family —
write `@typeview(Encode) Fam_alpha*` (mangles to `Fam_alpha_Restrict_Encode`).
A single `typedef` alias cannot name a family glob.

**Allow-list globs** match **field and method names alike** (`out_*` covers
`out_len` and any `out_foo()` UFCS). Membership is checked at the use site;
a glob that matches nothing at the declaration is a silent no-op (same as an
exact name written before the method exists). **`as:` uses exact field names
only** — no `^`, no name globs.

**Use kinds:**

| Group | Ordinary sites may |
|-------|-------------------|
| `r:` | load a field or **call** a UFCS method |
| `w:` | store through a field (`=`, `+=`, `++`, …) |
| `rw:` | both |
| `as:` | faces — not an allow-list |

Methods belong under `r:` (or `rw:`). A method listed only under `w:` cannot
be called. Construction stays open: designated init may name any field even
when an unnamed allow-list hides it later.

**Trusted bodies:** a function whose **first** parameter is the full type (or
pointer) sees every field in its body — how `cc_slice_*` mutates `.ptr`
while `@typeview on CCSlice { r: *; }` forbids field stores at ordinary sites.

### Faces (`as:`) — “this wrapper *is* its embed”

The common wrapper case. UFCS that misses on the outer type retries on the
named field. A field that is not a member of the outer retries the same
way (`xs.len` on a typed slice → `xs.base.len`). A local member wins.

```c
#!ccc ccs
#include <ccc/std/prelude.cch>
#include <stdio.h>

typedef struct {
    CCFile file;
    int tag;
} Temp;

@typeview on Temp {
    as: file;
};

int main(void) {
    @errhandler(CCError e) cc_error_exit(e);
    Temp t = {0};
    t.tag = 1;
    t.create("/tmp/tv.txt") !>;
    t.write("hi\n") !>;
    t.close();
    return 0;
}
```

`as:` names **value embeds** only, at most one path per target type.
Distinct types may each have a face (`as: file, path;`). UFCS that
misses on the outer type retries each face in **declaration order**.
If two faces both have the method, the call is ill-formed — write
`t.file.write(...)`. An `as:`-only unnamed view does **not** lock the
allow-list — other fields (`t.tag`) stay ordinary.

If no face has the method either, the error is at that call — original
form — and names the face that was tried:

```c
#!ccc ccs
#include <ccc/std/prelude.cch>
#include <stdio.h>

typedef struct {
    CCFile file;
    int tag;
} Temp;

@typeview on Temp {
    as: file;
};

int main(void) {
    Temp t = {0};
    t.tag = 1;
    t.gone();
    return 0;
}
```
<!-- compile-err
as: retry also failed
as: field: CCFile file
no UFCS method 'gone' for receiver type 'Temp'
-->

A trailing-`*` subject installs the same face on every match that has the
field; types that match the glob but lack the field are skipped.
Narrowest view wins, same score rule as `@typehooks`. Named modes use
the same glob (`@typeview Encode on Fam_*`); the parameter names a
concrete match:

```c
#!ccc ccs
#include <ccc/std/prelude.cch>
#include <stdio.h>

typedef struct {
    CCFile core;
    int tag;
} Fam_alpha;

typedef struct {
    CCFile core;
    int tag;
} Fam_beta;

@typeview on Fam_* {
    as: core;
};

int main(void) {
    @errhandler(CCError e) cc_error_exit(e);
    Fam_alpha a = {0};
    Fam_beta b = {0};
    a.create("/tmp/tv_glob_a.txt") !>;
    a.write("A\n") !>;
    a.close();
    b.create("/tmp/tv_glob_b.txt") !>;
    b.write("B\n") !>;
    b.close();
    printf("ok\n");
    return 0;
}
```
<!-- smoke-stdout
ok
-->

```c
#!ccc ccs
#include <ccc/std/prelude.cch>
#include <stdio.h>

typedef struct {
    int sock;
    int out_len;
} Fam_alpha;

typedef struct {
    int sock;
    int out_len;
} Fam_beta;

@typeview Encode on Fam_* {
    r: out_len, write;
};

static int fam_alpha_write(Fam_alpha* c, const char* data) {
    (void)c;
    (void)data;
    return 1;
}

static int fam_beta_write(Fam_beta* c, const char* data) {
    (void)c;
    (void)data;
    return 2;
}

static int use_a(@typeview(Encode) Fam_alpha* enc) {
    return enc->write("x") + enc->out_len;
}

static int use_b(@typeview(Encode) Fam_beta* enc) {
    return enc->write("y");
}

int main(void) {
    Fam_alpha a = { .out_len = 3 };
    Fam_beta b = {0};
    printf("%d %d\n", use_a(&a), use_b(&b));
    return 0;
}
```
<!-- smoke-stdout
4 2
-->

### Allow-lists — fewer names on the same object

#### Exact names and prefix globs

```c
#!ccc ccs
#include <ccc/std/prelude.cch>
#include <stdio.h>

typedef struct {
    int secret;
    int out_len;
    int get_hits;
} Bag;

@typeview on Bag {
    r: out_*, get_*;
};

static int bag_get_n(Bag* b) {   /* get_* → b.get_n() */
    return b->out_len;
}

int main(void) {
    Bag b = { .secret = 1, .out_len = 3, .get_hits = 0 };
    int n = b.get_n();
    printf("n=%d len=%d hits=%d\n", n, b.out_len, b.get_hits);
    /* b.secret; */                 /* ill-formed: not in out_* / get_* */
    return 0;
}
```
<!-- smoke-stdout
n=3 len=3 hits=0
-->

`out_*` is a **prefix** glob: `out_len` matches; `secret` does not. The same
rule covers UFCS names (`get_*` → `b.get_n()`).

#### Suffix globs

Leading `*` matches the **tail** of a name (`*_len` → `out_len`, `array_len`):

```c
#!ccc ccs
#include <ccc/std/prelude.cch>
#include <stdio.h>

typedef struct Conn {
    int sock;
    int out_len;
} Conn;

@typeview Encode on Conn {
    r: *_len;
};

static int use_enc(@typeview(Encode) Conn* enc) {
    return enc->out_len;
}

int main(void) {
    Conn c = { .out_len = 3 };
    printf("%d\n", use_enc(&c));
    return 0;
}
```
<!-- smoke-stdout
3
-->

#### Open surface and deny (`^`)

Bare `*` opens every name; `^field` subtracts one entry. A group of **only**
denies implies `*` first (`r:^secret` ≡ `r: *, ^secret`). Deny wins when both
match.

```c
#!ccc ccs
#include <ccc/std/prelude.cch>
#include <stdio.h>

typedef struct Box {
    int secret;
    int len;
} Box;

@typeview on Box {
    r: ^secret;
};

static int box_bump(Box* b) {   /* first arg → full Box */
    b->secret++;
    return ++b->len;
}

int main(void) {
    Box b = { .secret = 7, .len = 0 };  /* init may name .secret */
    int n = b.bump();                   /* OK — bump not denied */
    /* b.secret; */                     /* ill-formed at ordinary use */
    printf("n=%d len=%d\n", n, b.len);
    return 0;
}
```
<!-- smoke-stdout
n=1 len=1
-->

Stdlib uses the same shape on open families — slice field stores forbidden,
methods stay open; box host field hidden, methods stay open:

```c
@typeview on CCSlice   { r: *; };              /* reads OK; s.len = … ill-formed */
@typeview on CCBox_*   { as: p;  r: ^p; };     /* b.host() OK; b.p ill-formed */
@typeview on CCString  { r: ^data, ^inline_buf, ^_inline_word; };
```

#### Named modes — several faces of one type

```c
#!ccc ccs
#include <ccc/std/prelude.cch>
#include <stdio.h>

typedef struct Conn {
    int sock;
    CCString out;
} Conn;

@typeview Encode on Conn {
    r: write;
};

@typeview Ship on Conn {
    r: flush;
    rw: out;
};

static void conn_write(Conn* c, char[:] data) { (void)c; (void)data; }
static void conn_flush(Conn* c) { (void)c; }

static void encode_only(@typeview(Encode) Conn* c) {
    c->write("+OK\r\n");
    /* c->flush(); */   /* ill-formed: not in Encode */
    /* c->sock; */      /* ill-formed */
}

typedef @typeview(Encode) Conn* ConnEnc;

void handle(Conn* c) {
    encode_only(c);     /* Conn* → Encode, no cast */
    c->flush();         /* ship stays on the full pointer */
}

int main(void) {
    Conn c = {0};
    ConnEnc enc = &c;
    handle(&c);
    enc->write("x");
    printf("ok\n");
    return 0;
}
```
<!-- smoke-stdout
ok
-->

The arena header declares three named modes on `CCArena` — `Alloc`
(`r: alloc, remaining;`), `Parent` (`r: adopt, attach, create_*;`), and
`Region` (the union of both) — for signatures like
`@typeview(Parent) CCArena*` (construct and own, never reset or destroy).
The lowerer pins those modes so they survive header lowering. Viewed
faces (`as: (Region)field`) retry UFCS through the field under that
mode's allow-list (`tests/as_viewed_face_smoke.ccs`). A nursery born
with `owner.create_nursery()` exposes the same Region face
(`n.alloc(...)`).

---

## 3. Put them side by side, not in one block

A wrapper that answers file methods **and** deletes itself on `@destroy`:

```c
#!ccc ccs
#include <ccc/std/prelude.cch>
#include <stdio.h>
#include <unistd.h>

typedef struct {
    CCFile file;
    char[:] path;
} TempFile;

static void temp_file_unlink(TempFile* t) {
    if (!t->path.len) return;
    unlink((const char*)t->path.ptr);
}

@typeview on TempFile {
    as: file;
};

@typehooks on TempFile {
    .destroy = temp_file_unlink,
};

int main(void) {
    @errhandler(CCError e) cc_error_exit(e);
    TempFile t = {0} @destroy { t.close(); };  /* close, then unlink, then idempotent embed teardown */
    t.path = @slice("/tmp/tv_together.txt");
    t.create(t.path) !>;
    t.write("hi\n") !>;
    printf("ok\n");
    return 0;
}
```
<!-- smoke-stdout
ok
-->

`as:` retries any UFCS miss on the named embed (`t.write` → `t.file.write`),
not a CCFile-only method list. Open the embed explicitly:
`t.create(path) !>` (write+trunc into the embed; Err if it already
holds a fd) or `CCFile f@(path) !> @destroy` (read via the `.create`
hook). Cleanup is
**pre-destroy → `@destroy { }` body → outer destroy hook → value embeds**
last-declared to first. `file` is a `CCFile` value field with a destroy
hook, so `cc_file_close(&t.file)` runs in that last step. Bodyless
`@destroy` unlinks while the file may still be open — fine on POSIX, often
not on Windows. The portable spelling is the one above: `t.close()` in the
body, then `temp_file_unlink`, then `cc_file_close` on the embed. The
second close is a no-op — registered destroy hooks are idempotent
(`cc_file_close` nulls the handle and returns when already closed).

A family that needs both is the same split: `@typeview on Fam_* { as:
core; }` plus `@typehooks on Fam_* { .destroy = … }`. The glob is shared;
the bodies stay separate. An exact type still beats the family on each
form independently.

---

## 4. Checklist

- Adding `box_bump(Box*)`? Declare the function. Stop.
- Bodyless `@destroy` on your type? `@typehooks` + `.destroy` (or a value
  field whose type already has a hook).
- Close-before-unlink (or any “parts first”)? `@destroy { t.close(); }` —
  the body runs before the outer hook and embed teardown.
- `name@(args)` for that type? Same block, `.create`.
- Wrapper should reuse an embed’s methods? `@typeview` + `as: field`.
  Named pointer? `CC_DECL_BOX_ALIAS(Name, Host)` / `typedef CCBox::[Host] Name`
  plus `as: p` (inherited from `CCBox_*`). Host* methods retry through `.p`;
  dest-init does not mint on the teaching name.
- Caller should not see `flush` / `sock`? Named `@typeview Mode on T` and
  take `@typeview(Mode) T*`.
- Custom `x.method` → `cc_foo_<method>` family? `.ufcs` on `@typehooks`.
- Dest-init mint (`CCBox::[H] b = &x`, `char[:] v = str`)? `.cast` on the dest type (implicit|explicit + requested type).
- Extent / walk (`x.len`, `@for (v in s)` / `@for (&v in s) { … } !>;` / `@for (i, v in s)` / `@for (a, b in s, t) { … } !>;`)? `.len` + `.access` on `@typehooks` (naked). Mut walk's write bound is the Result. Users do not write `s.access(i)`. Slice fields are read-only. `CCString` hides `.data` (SSO).
- Hide a field on a family (`CCBox_*` `.p`, `CCString` `.data`)? Unnamed `@typeview` + `r:^name`
  (deny-only ≡ `r: *, ^name`). Prefix/suffix globs (`out_*`, `*_len`) for subsets. User UFCS stays.
- Type-family subject? `@typeview on Pat_* { … }` (unnamed surface) or
  `@typeview Mode on Pat_* { … }` plus `@typeview(Mode) Concrete*`. `as:` skips types without the field.
- Unresolved dynamic names (`obj.greet`)? `.ufcs_sink`, last resort.

---

## See also

- [Cheatsheet](cheatsheet.md) — destroy / UFCS one-liners
- [Language concepts](language-concepts.md) — `@destroy` and ordinary UFCS
- [Getting started](getting-started.md) — first program; [faces at the use site](getting-started.md#locality-owned-or-view)
- [recipe_owned_view.ccs](../examples/recipe_owned_view.ccs) — named `Measure` mode at the parameter; tutorial has globs and `^` deny
- Spec: [type hooks](../spec/draft_typehooks.md),
  [type views](../spec/draft_facets.md),
  [type-owned registration](../spec/concurrent-c-spec-complete.md)
- Tests: `scripts/test_doc_fences.sh` runs every `#!ccc ccs` fence in
  this file (`compile-err` pins the `t.gone()` miss). Also
  `tests/typehooks_fn_idents_smoke.ccs`,
  `tests/typehooks_create_destroy_smoke.ccs`,
  `tests/typehooks_len_access_smoke.ccs`,
  `tests/for_in_*_smoke.ccs`,
  `tests/typeview_as_ufcs_smoke.ccs`,
  `tests/typeview_glob_as_smoke.ccs`,
  `tests/typeview_glob_named_smoke.ccs`
