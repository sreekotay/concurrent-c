# Tutorial: `@typehooks` and `@typeview`

Two file-scope declarations attach policy to a type. They share the
`on Subject` shape and nothing else — do not merge the bodies.

| | `@typehooks` | `@typeview` |
|--|--|--|
| Answers | How this type is created, destroyed, and (optionally) how `x.method` lowers | Which names a caller may use, and which embedded fields are “is-a” faces |
| Typical job | Bodyless `@destroy` / `@create` for *your* type | A wrapper that should act like the field it contains, or a narrower face of one object |
| Body | Strict C designated init: `.destroy = …,` | Groups: `as:`, `r:`, `w:`, `rw:` |

You do **not** need `@typehooks` to add a method. Declaring
`static int box_bump(Box* b)` already installs `b.bump()` and `bptr->bump()`. Hooks are for
lifecycle and for library-owned naming families.

Subject may be an exact type (`Box`), a pointer key (`MyRes*`), or a
trailing-`*` family (`Fam_*`). Same match rule on both forms.

---

## 1. `@typehooks` — lifecycle

Write the functions. Then name them on the type. Bodyless `@destroy` and
`@create(...)` call those functions. No destroy hook → compile error.
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
    Port p = @create(3) @destroy;   /* port_open(3), then port_close */
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
- `@create(...)` picks the create function from the **declared type** on
  the left plus the argument list.
- If the type registers destroy, `@create(...)` must be followed by
  `@destroy` or `@detach`. Omitting both is an error.
- `@destroy` attaches to the **declaration**, not only to `@create`.
  `Port p = {0} @destroy;` and `Port p = port_open(3) @destroy;` run
  the destroy function at scope exit the same way.
- `@destroy;` runs the destroy function. `@destroy { … }` runs
  **pre-destroy → your block → destroy**.

Pointer and family subjects use the same body (`MyRes*`, `CCChanTx_*`).

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
    Port p = @create(3) @destroy;
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

A trailing-`*` subject installs the same face on every match that has the
field; types that match the glob but lack the field are skipped:

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
be called. Unknown non-glob names are ill-formed at the declaration.
Construction stays open: designated init may name any field.

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

Named modes on a glob subject (`@typeview Encode on Pat*`) are ill-formed —
globs are unnamed only.

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
    TempFile t = {0} @destroy;
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

`as:` forwards `open` / `write` / `close` through `.file`, and `@destroy`
still closes that embed. `@typehooks` is the extra step: unlink `path` so
the temp file is gone when the scope ends.

---

## 4. Checklist

- Adding `box_bump(Box*)`? Declare the function. Stop.
- Bodyless `@destroy` on your type? `@typehooks` + `.destroy`.
- `@create(...)` for that type? Same block, `.create`.
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
- Tests: `scripts/test_doc_fences.sh` compiles every `#!ccc ccs` fence in
  this file. Also `tests/typehooks_fn_idents_smoke.ccs`,
  `tests/typehooks_create_destroy_smoke.ccs`,
  `tests/typeview_as_ufcs_smoke.ccs`,
  `tests/typeview_glob_as_smoke.ccs`
