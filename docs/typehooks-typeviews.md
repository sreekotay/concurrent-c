# Tutorial: `@typehooks` and `@typeview`

Two file-scope declarations attach policy to a type. They share the
`on Subject` shape and nothing else — do not merge the bodies.

| | `@typehooks` | `@typeview` |
|--|--|--|
| Answers | How this type is created, destroyed, and (optionally) how `x.method` lowers | Which names a caller may use, and which embedded fields are “is-a” faces |
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
Stdlib types already ship one (`CCArena`, `CCFile`, `CCNursery*`, …).

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
                         CCArena* arena) {
    (void)recv_type;
    (void)mode;
    (void)argv;
    (void)arg_types;
    switch (method) {
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
fails at that call — same source line, original form — naming the
missing symbol:

```
file.ccs:43:5: error: call to undeclared function 'port_gone'
   43 |     p.gone();
      |     ^
```

Grep `port_gone` in your sources to find (or write) the callee.

---

## 2. `@typeview` — faces and allow-lists

A view is a **type-system lens on the same object**. No second allocation,
no vtable, erased in the lowered C. Narrowing is implicit (like `T*` →
`const T*`). Widening never happens — there is no cast back to the full type.

### Faces (`as:`) — “this wrapper *is* its embed”

The common wrapper case. UFCS that misses on the outer type retries on the
named field.

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
    t.open("/tmp/tv.txt", "w");   /* → t.file.open(...) */
    t.write("hi\n");
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
Narrowest view wins, same score rule as `@typehooks`. Named modes on a
glob (`@typeview Encode on Fam_*`) are ill-formed — globs are unnamed
only:

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
    Fam_alpha a = {0};
    Fam_beta b = {0};
    a.open("/tmp/tv_glob_a.txt", "w");
    a.write("A\n");
    a.close();
    b.open("/tmp/tv_glob_b.txt", "w");
    b.write("B\n");
    b.close();
    printf("ok\n");
    return 0;
}
```
<!-- smoke-stdout
ok
-->

### Allow-lists — fewer names on the same object

Groups are comma-separated patterns ended by `;`:

| Group | Meaning |
|-------|---------|
| `r:` | Load a field or **call** a UFCS method |
| `w:` | Store through a field (`=`, `+=`, `++`, …) |
| `rw:` | Both |
| `as:` | Faces (not an allow-list) |

Methods belong under `r:` (or `rw:`). A method listed only under `w:` cannot
be called. `r:` / `w:` / `rw:` patterns match **field and method names
alike**, and may be trailing-`*` name globs (`out_*`, `get_*`) — membership
on this type, not a type-family subject. That is why a declaration-time
existence check cannot decide: `get_*` may match a method written later in
the file. A glob that matches nothing is a silent no-op at the declaration;
an exact name that does not exist is the same. Either way the miss shows up
at a use the list does not cover. `as:` stays exact field names — no name
glob. Construction stays open: designated init may name any field.

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

**Unnamed** — the allow-list *is* the ordinary surface of the type. No
parallel view name. A function whose **first** parameter is that type (or
pointer) sees the full object, so method bodies can still write private
fields:

```c
#!ccc ccs
#include <ccc/std/prelude.cch>
#include <stdio.h>

typedef struct Box {
    int secret;
    int len;
} Box;

@typeview on Box {
    r: len, bump;
};

static int box_bump(Box* b) {   /* first arg → full Box */
    b->secret++;
    return ++b->len;
}

int main(void) {
    Box b = { .secret = 7, .len = 0 };  /* init may name .secret */
    int n = b.bump();                   /* OK */
    /* b.secret; */                     /* ill-formed at ordinary use */
    printf("n=%d len=%d\n", n, b.len);
    return 0;
}
```
<!-- smoke-stdout
n=1 len=1
-->

**Named** — several faces of one owned type. Callers write the mode on the
parameter; a `Base*` narrows implicitly; a view pointer never widens back:

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
    if (!t->path.ptr || !t->path.len) return;
    unlink((const char*)t->path.ptr);
}

@typeview on TempFile {
    as: file;
};

@typehooks on TempFile {
    .destroy = temp_file_unlink,
};

int main(void) {
    TempFile t = {0} @destroy { t.close(); };  /* close, then unlink, then idempotent embed teardown */
    t.path = @slice("/tmp/tv_together.txt");
    t.open(t.path, "w");
    t.write("hi\n");
    printf("ok\n");
    return 0;
}
```
<!-- smoke-stdout
ok
-->

`as:` forwards `open` / `write` / `close` through `.file`. Cleanup is
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
- Caller should not see `flush` / `sock`? Named `@typeview Mode on T` and
  take `@typeview(Mode) T*`.
- Custom `x.method` → `cc_foo_<method>` family? `.ufcs` on `@typehooks`.
- Unresolved dynamic names (`obj.greet`)? `.ufcs_sink`, last resort.

---

## See also

- [Cheatsheet](cheatsheet.md) — destroy / UFCS one-liners
- [Language concepts](language-concepts.md) — `@destroy` and ordinary UFCS
- [Getting started](getting-started.md) — first program
- Spec: [type hooks](../spec/draft_typehooks.md),
  [type views](../spec/draft_facets.md),
  [type-owned registration](../spec/concurrent-c-spec-complete.md)
- Tests: `scripts/test_doc_fences.sh` runs every `#!ccc ccs` fence in
  this file (`compile-err` pins the `t.gone()` miss). Also
  `tests/typehooks_fn_idents_smoke.ccs`,
  `tests/typehooks_create_destroy_smoke.ccs`,
  `tests/typeview_as_ufcs_smoke.ccs`,
  `tests/typeview_glob_as_smoke.ccs`
