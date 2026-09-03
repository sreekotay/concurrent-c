# Concurrent-C Specification

A C preprocessor that extends C syntax with first-class concurrency, desugaring to portable C.

**Core concept:** the lifetime of memory and the lifetime of tasks are explicitly bound to the structure of the code.

**Full name:** Concurrent-C  
**Abbreviation:** CC  
**Type:** C extension (preprocessor + minimal runtime)  
> **Status:** partially implemented. A surface is shipped only where this specification identifies an implementation-backed API or lowering; unsupported spellings are rejected.

This specification defines:

- Surface syntax and compile-time rules
- Observable runtime contract
- Lowering to C
- Script entry (`.shcc`) and the script library partner to the stdlib (§9.5)
- Translation-unit headers (`#!ccc ccs|cch`, OS shebang for scripts) (§1.7)
- File-start `#pragma(@prelude) off` / `#pragma(@linenumbers) off` / `#pragma(@per_tu)` (§1.8)

The lowering is part of this specification, not an implementation detail. Two conforming implementations must produce lowerings with identical observable behavior. Implementations may emit or inspect the lowered form via `--emit-c-only` (writes lowered C to `out/<stem>.c`) or `--emit-c-inspect` (writes the merged translation unit).

---

## The CC Principle of Orthogonal Concerns

The key goal: (as much as possible) the compiler enforces the **Shape** of the program (Memory/Tasks), while the runtime monitors the **Flow** of the program (Channels).

- **The Skeleton (Structure):** Nurseries and Arenas et al are hierarchical. They define the Ownership Tree. Their lifetimes are lexical and enforced by the compiler.
- **The Circulatory System (Flow):** Channels et al are a graph. They define the Communication Topology. Their lifetimes are dynamic and can "cross-cut" the ownership tree. The provenance model is what makes the graph safe.

The compiler enforces ownership shape statically. The runtime enforces dynamic
flow properties that cannot be proven statically. Every language construct has
an inspectable C lowering; generated C is a first-class build and debugging
artifact.

---

## Design Principles

These are normative. Where a construct could be defined more than one way, the reading that upholds these wins.

1. **No magic.** New behavior must be as predictable as C. A program is understood from what is on screen plus its lowering — never from hidden runtime policy. Cleanup registration (`@defer` / `@destroy` / owned capture) is visible in source; where hooks run (scope exit, soft-return epilogue, cancelled-resume, never-entered `env_drop`) is defined by this specification’s emit and observable in the lowered transcript.
2. **The lowering is the language.** The source-to-C lowering is part of this specification, not an implementation detail (see intro). Two conforming implementations produce lowerings with identical observable behavior.
3. **Library-owned policy.** The compiler provides compile-time registration,
   emission, and dispatch seams. Libraries own concrete type-family emission,
   linkage, erasure, specialization, and UFCS policy.
4. **No silent drop.** Every mandatory fallible result (`T!>(E)`) must be consumed. The safe forms (`?>`, `!>`) force handling at compile time; the explicit/lowered form traps loudly at runtime. Optional results (`T?>(E)`) use the same ABI and the same consume operators but declare that a bare statement `f();` is well-formed — that is an explicit optional ignore at the declaration, not a silent drop. Absence (nullable) may opt into the same unwrap protocol but is not forced into it.
5. **Local resolution.** Cleanup (`@destroy`/`@defer`), error handlers (inline or block-scoped, declaration-point), and dispatch (receiver type) resolve to a site visible at the use — never a dynamic search. Behaviors therefore compose by stacking, not by interaction.
6. **Binding is semantic.** *Where* a construct attaches (declaration vs. statement) determines its static guarantees, even when the runtime lowering is identical. `@destroy` is RAII because it binds to the declaration; `@defer` is a statement.
7. **Cleanup ledgers.** A cleanup ledger has two discharge authorities: the entered frame (always), and external closure `env_drop` (never-entered only).

---

## 1. Primitives

Concurrent-C extends C with six primitives. Everything else in this
specification is either sugar over these, an attribute on them, or a
library type defined in terms of them.

This section gives you the **shape** of each primitive with a minimal
example. Normative rules live in the sections referenced under each
entry; the Quick Reference that follows collects the full surface at a
glance. The rules each primitive obeys — value categories, moves,
closure captures, lexing — are in §2 Foundations.

### 1.1 UFCS — receiver-typed dispatch

Method-call syntax `x.f(...)` / `p->f(...)` dispatches on the resolved
type of the receiver expression. Any function registered against a type
is callable as a method on values of that type.

```c
CCNursery n = cc_nursery_create() !> @destroy;
n.spawn(() => printf("hi\n"));
```

UFCS is orthogonal to ownership: the same method works on a value, a
pointer, or a Result carrying either. Normative rules in §9.0.

### 1.2 Result types and unwrapping

A function that can fail returns `T!>(E)` or `T?>(E)`. Both spellings lower to
the same `CCResult_T_E` box and share the consume operators (`?>`, `!>`,
`@errhandler`). The marker chooses consumption policy:

| Return            | Bare `f();` at statement position |
| ----------------- | --------------------------------- |
| `T!>(E)`          | ill-formed                        |
| `T?>(E)`          | well-formed (optional ignore)     |

Three unwrap forms cover the full error space — each answers a different
question at the call site:

| Form                | Question                         | Body                |
| ------------------- | -------------------------------- | ------------------- |
| `expr ?> default`   | "what if it fails?"              | value               |
| `expr !>(e) { … }`  | "handle it here"                 | diverging statement |
| `expr !>;`          | "forward to the scope's policy"  | —                   |

```c
int !>(CCError) read_timeout(void);
void ?>(CCPrintError) log_line(CCSlice msg);

@errhandler(CCError e) cc_error_exit(e);         // or { cc_error_log(e); return 1; }

int t1 = read_timeout() ?> 30;                   // default on error
int t2 = read_timeout() !>(e) return 2;          // inline (must diverge)
int t3 = read_timeout() !>;                      // forward to @errhandler
log_line(msg);                                   // optional — bare discard ok
/* Non-hoisted equivalent of !>; with that binding:
 *   int t3 = read_timeout() !>(e) cc_error_exit(e);
 */
```

Normative rules in §3.1.

### 1.3 `@defer` — scope-exit actions

`@defer stmt;` schedules `stmt` to run on scope exit in reverse
declaration order, regardless of how the scope exits. `@defer(ok)` and
`@defer(err)` are conditional variants. `@destroy` is the declaration-
attached form: `T x = make() !> @destroy { cleanup; };` reads as "on
scope exit, run `cleanup` — and if `make()` failed, never construct `x`
in the first place."

```c
FILE* f = fopen(path, "r");
@defer      fclose(f);                  // always runs
@defer(ok)  printf("committed\n");
@defer(err) printf("rolled back\n");
```

Normative rules in §5.1.

### 1.4 `@async` / `@await` — cooperative suspension

`@async` marks functions that may suspend; `@await` marks an async-child poll
point. The compiler lowers `@async` to an explicit frame and poll function.

```c
@async int echo(CCChanTx tx, CCChanRx rx, int v) {
    if (!cc_io_avail(@await tx.send(v))) return -1;
    int reply;
    if (!cc_io_avail(@await rx.recv(&reply))) return -2;
    return reply;
}
```

`@await` is only legal inside `@async`. Synchronous code drives an async
function with `@await f(...)` at the top level or `cc_block_on(f(...))`.

**Sigil policy:** every CC-introduced keyword carries a leading `@`
(`@async`, `@await`, `@defer`, `@cancel_defer`, `@errhandler`,
`@destroy`, `@with_deadline`, `@parallel`, `@serial`, `@comptime`, `@blocking`,
`@nonblocking`, `@for`, `@typehooks`, `@typeview`). Bare forms are
reserved for plain C identifiers — `match`, `await`, `async`, `defer`,
etc. are legal variable / field / function names and never keywords
without the `@`. This eliminates identifier-collision and
keyword-in-comment scanning ambiguity across the compiler. See §2.3.

Normative rules in §8; lowering in Appendix J.

### 1.5 Provenance — arenas, slices, strings

Every value remembers where it came from. A slice `T[:]` carries
provenance metadata alongside its pointer and length: arena, stack,
static, or unique (received or adopted). The compiler uses provenance to
reject escapes (a stack slice captured into a task closure) and to
decide whether a slice is copyable or must move.

```c
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;
CCString greeting = @string(`hello ${now()}`, a);   // arena provenance
// greeting is copyable while `a` is live; captured into a task only if
// `a` provably outlives the task.
```

Strings and vectors build on slices and arenas; see §§3.4–3.5, §5, §9.1.

### 1.6 `@comptime` — compile-time evaluation

`@comptime` stages output-only C execution during translation. The staged code
executes through a C ABI and may register type hooks, emit C, request library
family instantiations, reflect registered C types, and project tested scalar
values into generated C. It does not execute as part of the output program.

```c
@comptime int LANES = 4;

@comptime if (sizeof(void*) == 8) {
    typedef long long word;
} else {
    typedef int       word;
}
```

Normative rules in §14.

### 1.7 Translation-unit header

A Concurrent-C unit names its kind on line 1. The header is not program text:
the implementation strips it before lowering and replaces it with the generated
C or H banner. Kind is not an extension property; a path suffix is only a
fallback when the header is absent.

| Kind | Line 1 |
| ---- | ------ |
| source (`ccs`) | `#!ccc ccs [version=MAJOR.MINOR[.PATCH[-SEED]]]` |
| header (`cch`) | `#!ccc cch [version=…]` |
| script (`shcc`) | `#!/usr/bin/env -S ./cc/bin/ccc [--as=shcc] [version=…]` |

The script form is an OS shebang so the kernel can exec the file. `--as=shcc`
is optional: a `ccc` interpreter shebang without `--as` is script kind.
`#!ccc shcc` is ill-formed — scripts must be OS-executable.

`version=MAJOR.MINOR[.PATCH[-SEED]]` pins the lowerer to a bootstrap
folder whose name the pin prefixes. The usual pin is MAJOR.MINOR (`0.3`
matches `0.3.3-156`). PATCH and SEED tighten the match (`0.3.2` matches
`0.3.2-121`; `0.3.2-12` does not). A shorter prefix (`0`) also matches.
The running toolchain lowers an unpinned unit, and also a pin that
prefixes the running version. Otherwise the newest matching seed's
prelowered `shadow_lower.c` is host-cc'd. A pin with no matching seed is
an error.

`--as=ccs|cch|shcc` and `version=` / `--ccc-version=` on the `ccc` command
line must agree with the file header when both are present. A header that
disagrees with a `.ccs` / `.cch` / `.shcc` suffix is ill-formed.

The implementation may copy a header-bearing unit into a cache file so a
pin-era lowerer never sees the magic line. Quoted `#include` of project
faces still resolves from the original unit's directory (the `#line` path),
not the cache directory. An included `.ccs` / `.cch` face with a unit
header is stripped the same way — the bang is not a preprocessor directive.
A local `.cch` that needs the including unit's pipeline — a non-`static`
file-scope function definition, `@string`, `@errhandler`, `?>`, and the
like — is spliced into that unit when the include is written in a `.ccs`
or inside an already-spliced face. `T !>(E)` on a declaration is result-type
syntax and does not by itself force a splice. Statement `!>` / `!>(e) {`
in extractable text (`static inline` helpers) is rewritten in the lowered
`.h` and does not force a splice. Method-call UFCS in an interface
header does not force a splice from a `.ccs`. A quoted interface `.cch`
extracts to a lowered `.h`. The extracted `.h` is host-cc input. A
member call in an extracted inline that names a declared `Type_method`
(same face, an included face, or the face that included this one) lowers
to that call; a field peel is not rewritten to the outer type's method.
A leftover member call in the `.h` is an error. The
lowerer still opens the original face from the quote directory (same)
roots as quoted `#include`) and harvests fields, type views, and
typehooks — the same original-face read used for `<ccc/…>`. A rewritten
`#include` of that `.h` is not an ordinary C header for that harvest.
Nested local includes inside that header
extract to their own `.h`; they do not splice into the including unit.
An impl-grade nested face without an owner `.ccs` is an error — move
the bodies to an owner `.ccs`, or include the face from that `.ccs`.
The owner is the same-stem sibling (`foo.cch` → `foo.ccs`), a chapter
face with that prefix (`piece_tree_rb.cch` → `piece_tree.ccs`), a
same-directory `.ccs` that includes the chapter (`document.ccs`
includes `utf8.cch`), or a same-directory face included from an owned
face (`workspace.cch` includes `ui_types.cch` → `workspace.ccs`). When
both a `.ccs` include and a face-includer exist, the `.ccs` include is
the owner. Other units extract decls and the owner unit splices the
bodies after the extracted parent include so names that face defines
are in scope. One include in one translation unit is either an
extracted `#include` of the lowered `.h` or a splice of the chapter
body, not both. A textual include guard does not apply to a splice.
A file-scope function body has one definition — the owner TU.
File-scope `static` on a function stays `static`: the owner splice
keeps the keyword and the body, and the extracted `.h` omits that
function. A file-scope function without `static` has one definition
in the owner TU; other TUs see a declaration in the extract. A file-scope data definition (`int xs[] = {1,2}`) becomes
`extern int xs[];` in the extract; the owner splice keeps the
initializer. `static` data stays `static` in the extract and is not
repeated in an owner include-graph splice.
`#ifdef` / `#if` in an extracted face stay
in the lowered `.h`; an object-like `#define` in this unit before the
include is host cpp and selects those arms, including function bodies
that sit under `#ifdef`. A pointer type in a declaration — a parameter,
file-scope declarator, or struct field (`Tag *name`) — that the face does
not already name as a type is not forwarded as `typedef struct Tag Tag` —
that invents a tagged struct and conflicts with an anonymous
`typedef struct { … } Tag` or an integer alias already in the unit.
A multiply in a function body (`d * 100ull`) is not a pointer type.
`CC_MAP_DECL_ARENA` / `CC_MAP_DECL_UFCS` / `CC_ARRAY_MAP_DECL` /
`CC_DECL_SLICE_SPEC` / `CC_DECL_RESULT_SPEC` name the type they bind.
If exactly one same-directory face defines the name and that face
can extract (not impl-grade without an owner), the extract includes
that face. If no same-directory face defines it, and exactly one
face in the including unit's include graph does, extract includes
that face. Chapter faces of one owner that share the name
(`piece_tree.cch` and `piece_tree_priv.cch`) are that one face; extract
includes the stem. Two faces with different owners is an error.
No same-directory face and no type of that name in the
including unit is an error. An impl-grade unowned parent already spliced into this
unit is not extracted from the leaf. Nested quoted includes in an
extracted `.h` use a path relative to that `.h` (same directory is
the basename). Nested quoted includes hoist
only when the included face defines a name this face uses and does
not define, and they insert after this face's definitions of names
the included face uses (`RtxBuf` before `ui_types.h`). A consumer
leaf included last stays in source order. The including unit's `#include` of the face stays
in source order. Nested local includes inside a spliced face are
processed the same way.

### 1.8 File-start pragmas

After the unit header (§1.7) — the shebang is not program text — a unit may
begin with:

```
#pragma(@prelude) off
#pragma(@linenumbers) off
#pragma(@per_tu)
```

File-start means after the header and after leading blank lines and comments.
Any other operand is ill-formed. The directives are consumed by lowering and
never reach the host compiler.

`#pragma(@prelude) off` injects no automatic prolog: a script unit does not
receive `<ccc/script/prelude.cch>`, the default `@errhandler`, or token-gated
`a` / `io` / `in` / `args`. Synthetic `main` still wraps top-level statements.
Emitted C omits the automatic `<stddef.h>` / `<stdint.h>` / `<stdlib.h>`
includes. A one-line provenance comment naming the `ccc` version may remain;
it does not affect compilation.

`#pragma(@linenumbers) off` omits `#line` and `CC_LN` from the emitted C.
`--no-line` on the `ccc` command line has the same effect and overrides the
pragma when both are present.

An unowned impl-grade face whose file-scope functions are all `static`
may splice into every translation unit as a private copy. A second splice
of an unowned face that has a non-`static` file-scope function is an
error — make those functions `static`, or give the face an owner `.ccs`.
Extract of an owned face whose owner `.ccs` is not in the link set is an
error.

`#pragma(@per_tu)` is optional. Presence (no `off`) requires every
file-scope function on that face to be `static`, even when only one
translation unit includes it.

---

Everything else in this specification is one of:

- **Sugar** over these primitives (e.g., `CALL() !> @destroy { D };`
  schedules declaration-bound cleanup on scope exit).
- **Attributes** (e.g., `@blocking`, `@nonblocking`, `@latency_sensitive`).
  `@blocking` / `@nonblocking` are *dual*: function declarations,
  lexical blocks, and individual call sites accept them, and they
  together define the execution-mode contract at every call edge (§8.2).
- **Library types** (e.g., `CCNursery`, `CCChan`, `CCMutex`, `CCVec`, `CCString`, `CCMap`), defined in terms of the primitives plus the runtime contract.

---

## Quick Reference: Keywords and Constructs

Syntax inventory, grouped by purpose. See §1 for the primitive taxonomy and individual sections for normative rules.

### Core Keywords

All CC-introduced keywords carry a leading `@` sigil. Bare identifiers
(`match`, `await`, `async`, `defer`, `nonblocking`, `blocking`, `comptime`,
…) are legal C names and never keywords on their own. This gives the
lexer an unambiguous sentinel for every CC construct and removes an
entire class of keyword-in-comment / keyword-in-identifier scanner
bugs.


| Keyword        | Purpose                                                                 | Example                                |
| -------------- | ----------------------------------------------------------------------- | -------------------------------------- |
| `@async`       | Mark function as asynchronous (state-machine lowered)                   | `@async int handler() { ... }`         |
| `@await`       | Suspend until an async operation completes                              | `int v = @await fetch();`              |
| `@defer`       | Schedule cleanup on scope exit                                          | `@defer file.close();`                 |
| `@defer(err)`  | Cleanup only on error return                                            | `@defer(err) free(ptr);`               |
| `@defer(ok)`   | Cleanup only on success return                                          | `@defer(ok) commit();`                 |
| `@cancel_defer` | Disarm a named `@defer` before it runs                                 | `@cancel_defer cleanup;`               |
| `@errhandler`  | Block-scoped handler for `!>;` / `@err`, selected by Result error type  | `@errhandler(CCError e) cc_error_exit(e);` |
| `@err`         | Forward current error to the matching `@errhandler` for that `E`        | `@err(e);`                             |
| `@with_deadline` | Apply deadline to a block                                             | `@with_deadline(seconds(5)) { … }`     |
| `@parallel`      | Join independent assignment arms, or walk an index range              | `@parallel { a = f(); b = g(); } !>.wait()!>;` |
| `@parallel spawn` | Same join; spawned arms are not denied (meeting admit)              | `@parallel spawn { produce(tx); consume(rx); } !>.wait()!>;` |
| `@parallel (pred)` | Same join; spawn if `pred`, otherwise run the arms in order         | `@parallel (d < k) { a = f(); b = g(); } !>.wait()!>;` |
| `@parallel for`  | Independent iterations over a half-open integer range                 | `@parallel for (i in 0..n) { … } !>.wait()!>;` |
| `@parallel seq (cond)` | Same join; `seq` names the denial: run the arms in order         | `@parallel seq (use_par) { a = f(); b = g(); } !>.wait()!>;` |
| `@parallel wait (ts) @for` | Ordered spawn loop over a turnstile; `bool !>(CCError)` | `bool fin = @parallel wait (ts) for (i in 0..n) { step(i) !>; } !>;` |
| `@serial`        | Multi-statement arm of `@parallel { }`; zero or one outer name        | `@serial { int t = f(); a = t; }`      |
| `@destroy`     | Attach cleanup to a result-unwrap                                       | `FILE* f = open() !> @destroy;`         |
| `@comptime`    | Compile-time evaluation / conditional                                   | `@comptime if (DEBUG) { }`             |
| `@blocking`    | Mark a call edge as going through `run_blocking` (function or site)     | `@blocking f();` — see §8.2            |
| `@nonblocking` | Mark a non-blocking execution-mode contract (function, block, or site) | `@nonblocking f();` — see §8.2      |
| `@noblock`     | Compatibility spelling for `@nonblocking`                              | `@noblock f();` — see §8.2          |
| `@latency_sensitive` | Disable dispatch coalescing for this `@async` fn                  | `@async @latency_sensitive void h() {}`|
| `@typehooks`   | Lifecycle / UFCS policy on a type                                       | `@typehooks on T { .destroy = …, };`   |
| `@typeview`    | Faces (`as:`) and allow-lists on a type                                 | `@typeview on T { as: file; };`        |
| `@scoped`      | Type tied to a lexical scope (cannot escape)                            | `@scoped type Guard::[T];`             |
| `@unsafe`      | Closure hatch: skip mutation-of-share checks on that spawn              | `n.spawn(@unsafe () => [&x] { x++; });` |
| `@slice`       | Build-time canonical sentinel slice                                     | `char[:0] m = @slice("recv");`         |
| `@string`      | Templated string: arena `String`, or arena-less bounded `char[:]` (§9.1.2) | `CCString s = @string("hi", arena);`  |
| `unsafe`       | (Bare) waive provenance and sendability in a block                      | `unsafe { ptr_cast(); }`               |


### Declaration and Statement Forms


| Form                            | Purpose                                                  | Example                                                  |
| ------------------------------- | -------------------------------------------------------- | -------------------------------------------------------- |
| `@async fn() { }`               | Define asynchronous function                             | `@async void handler() { }`                              |
| `@blocking fn() { }`            | Mark declaration — async callers route through `run_blocking` at call edges (§8.2) | `@blocking FILE* open_config() { … }`                    |
| `@nonblocking fn() { }`         | Mark declaration — async callers skip `run_blocking` at call edges (§8.2)          | `@nonblocking size_t strlen_nb(const char* s) { … }`     |
| `@latency_sensitive`            | Mark as latency-critical (no dispatch coalescing)        | `@async @latency_sensitive void handle() { }`            |
| `@scoped type T`                | Type tied to lexical scope (cannot escape)               | `@scoped type Guard::[T];`                               |
| `CALL() !> @destroy { D };`     | Resource lifetime declaration with error-checked cleanup | `CCNursery n = cc_nursery_create() !> @destroy;`    |
| `@defer stmt;`                  | Schedule statement to run on scope exit                  | `@defer file.close();`                                   |
| `@comptime if (cond) { }`       | Compile-time conditional                                 | `@comptime if (FEATURE_X) { }`                           |
| `@errhandler(E e) stmt` / `{ }` | Block-scoped handler for Result error type `E` (§3.1)    | `@errhandler(CCError e) cc_error_exit(e);` |
| `@parallel { arms }`            | `CCParallel !>(CCError)` join of `name = expr;`, `expr !>;`, or `@serial` arms (§8.11) | `@parallel { a = f(); b = g(); } !>.wait()!>;` |
| `@parallel spawn { arms }`      | Same join; spawned arms are not denied (§8.11.7)          | `@parallel spawn { produce(tx); consume(rx); } !>.wait()!>;` |
| `@parallel(h) { stmts }`        | Admit onto dest `h` (statement; value-copy)               | `@parallel(h) { handle(sock); }` |
| `@parallel (pred) { arms }`     | Same join; spawn if `pred`, else run in order (§8.11.3)   | `@parallel (d < k) { a = f(); b = g(); } !>.wait()!>;` |
| `@parallel for (i in lo..hi)`   | Independent iterations over a half-open range (§8.11.4)   | `@parallel for (y in 0..h) { row(y); } !>.wait()!>;` |
| `@parallel seq (cond) { arms }` | Same join; named sequential denial (§8.11.5)              | `@parallel seq (use_par) { a = f(); b = g(); } !>.wait()!>;` |
| `@parallel [seq (cond)] wait (ts) @for` | Ordered spawn loop over a turnstile; `bool !>(CCError)` (§8.11.6) | `bool fin = @parallel wait (ts) for (i in 0..n) { step(i) !>; } !>;` |
| `@serial { stmts }`             | Multi-statement arm of `@parallel { }` (§8.11.2)          | `@serial { int t = f(); a = t; }`          |
| `worker (w)`                    | Wait-for binder: the runner slot index (§8.11.6)          | `wait (ts) @for (i in 0..n) worker (w) { z[w]… }` |
| `cache (name, …)`               | Wait-for clause: warm scratch, instance unobservable (§8.11.6) | `wait (ts) cache (zs) @for (i in 0..n) { … &zs … }` |
| `@stage (gate, args…) { stmts }` | Ticket handshake in a wait-for body; unwraps Result wait/pass; fail on error exits. Not a Result (§8.11.6) | `@stage (ts.write, i) { out.write(d) !>; }` |
| `#pragma(@parallel) off` / `on` | Static denial: `@parallel` lowers sequentially (§8.11.8)  | `#pragma(@parallel) off`                   |
| `#pragma(@prelude) off` | No automatic prolog (§1.8) | `#pragma(@prelude) off` |
| `#pragma(@linenumbers) off` | Omit `#line` / `CC_LN` from emit (§1.8) | `#pragma(@linenumbers) off` |
| `#pragma(@per_tu)` | Optional: require all file-scope fns `static` (§1.8) | `#pragma(@per_tu)` |

**Call-site annotation forms** (see §8.2 for precedence):


| Form                | Purpose                                                  | Example                    |
| ------------------- | -------------------------------------------------------- | -------------------------- |
| `@blocking expr;`   | Force this call edge to route through `run_blocking`     | `@blocking helper();`      |
| `@nonblocking expr;`| Force this call edge to skip `run_blocking`              | `@nonblocking helper();`   |
| `@nonblocking { }`  | Set lexical ambient mode for direct call edges in block  | `@nonblocking { parse(); }`|


### Result unwrap operators (2)

Result-typed calls (`T!>(E)`) must be explicitly consumed. Two operators with cleanly separated roles cover every consumption path:


| Form                      | Context                | Purpose                                                                           | Example                                        |
| ------------------------- | ---------------------- | --------------------------------------------------------------------------------- | ---------------------------------------------- |
| `expr ?> default`         | expression             | Unwrap value or substitute a default value                                        | `int x = parse(s) ?> 0;`                       |
| `expr ?>(e) default_expr` | expression             | Unwrap value, or evaluate `default_expr` with `e` bound to the error              | `int x = parse(s) ?>(e) fallback_for(e.kind);` |
| `call !> body`            | statement / expression | Unwrap, or execute `body` on error. At expression position the body must diverge. | `int x = parse(s) !>(e) return cc_err(e);`     |
| `call !>;`                | statement / expression | Unwrap, or invoke the matching `@errhandler` for Result `E`                       | `flush() !>;`                                  |


**See §3.1** for full semantics (divergence rules at expression position, `@errhandler` registration, `@err(e);` forwards, bare-statement consumption rule), grammar, and lowering.

### Type Constructors (6)


| Constructor               | Meaning                                               | Example                         |
| ------------------------- | ----------------------------------------------------- | ------------------------------- |
| `T!>(E)`                  | Result type: success (T) or error (E)                 | `int!>(IoError) read(path);`    |
| `char[:]`                 | Slice (variable-length view with provenance metadata) | `void process(char[:] data);`   |
| `char[4:]`                | Fixed-length slice refinement                         | `void hash(char[32:] digest);`  |
| `char[:!]`                | Unique slice refinement (type-level ownership demand) | `void take(char[:!] buf);`      |
| `char[:0]`                | Sentinel slice refinement                             | `char[:0] name = s.as_slice();` |
| `char[:0!]`               | Sentinel unique slice refinement                      | `char[:0!] buf = recv(ch);`     |
| `T[~... >]` / `T[~... <]` | Channel handle type (tx / rx; topology in `~...`)     | `int[~n 1:1 >] tx;`             |


### Expression Forms (3)


| Form                                                     | Purpose                                                               | Example                                   |
| -------------------------------------------------------- | --------------------------------------------------------------------- | ----------------------------------------- |
| `@await expr`                                            | Suspend until task completes, unwrap result                           | `int result = @await fetch();`            |
| `@slice("...")`                                          | Build-time canonical sentinel slice                                   | `char[:0] mode = @slice("recv");`         |
| `@string(expr, arena)` / `@string(policy, \`..., arena)` | Direct or templated string construction (`${e}` and `$~tag{e}` slots) | `CCString msg = @string(user_id, arena);` |
| `@string(\`...\`, @scratch)` / `@scratch(N)`             | Temp stack arena for `@string` only — shared per function/closure (§9.1.4) | `println(@string(\`r=${ratio}\`, @scratch))` |
| `@string(\`...\`)` (no arena)                            | Bounded-template stack form: block-scoped buffer, yields `char[:]` borrow (§9.1.2) | `char[:] s = @string(\`v=${v}\`);`        |


### Parallel Forms


| Form | Purpose | Example |
| ---- | ------- | ------- |
| `@parallel { a = …; b = …; }` | `CCParallel !>(CCError)` join. Always tries to spawn. | `@parallel { left = f(); right = g(); } !>.wait()!>;` |
| `@parallel spawn { a = …; b = …; }` | Same join; spawned arms are not denied. | `@parallel spawn { produce(tx); consume(rx); } !>.wait()!>;` |
| `@parallel { @serial { … } … }` | Same join; an arm may be ordinary C (zero or one outer name). | `@parallel { @serial { int t = f(); a = t; } b = g(); } !>.wait()!>;` |
| `@parallel (pred) { a = …; b = …; }` | Same arms. Spawn if `pred`; otherwise serial. | `@parallel (d < k) { left = f(); right = g(); } !>.wait()!>;` |
| `@parallel for (i in lo..hi) { }` | Independent iterations over a half-open integer range. Bisects; may sequentialize. | `@parallel for (y in 0..h) { row(y); } !>.wait()!>;` |
| `@parallel seq (cond) { a = …; b = …; }` | Same arms; `seq` names the denial: run in order when `cond` is false. | `@parallel seq (use_par) { left = f(); right = g(); } !>.wait()!>;` |
| `@parallel [seq (cond)] wait (ts) for (i in lo..hi) { }` | Ordered spawn loop: `bool !>(CCError)`; `enter(i)` in loop order on the caller, depth-capped, `leave()` after each body. | `bool fin = @parallel wait (ts) for (i in 0..n) { step(i) !>; } !>;` |


### Deadline Scope Forms


| Form                              | Purpose                                                  | Example                                                         |
| --------------------------------- | -------------------------------------------------------- | --------------------------------------------------------------- |
| `@with_deadline(ms) { }`           | Make a relative deadline current for operations that consult it. | `@with_deadline(seconds(5)) { cc_chan_match_select(..., cc_current_deadline()); }` |
| `@with_deadline(ms) as handle { }` | Same, with the active `CCDeadline*` bound inside the block. | `@with_deadline(seconds(5)) as dl { if (cc_deadline_expired(dl)) break; }` |
| `@with_deadline(dl) { }`           | Push an existing `CCDeadline*`. No new clock. | `@with_deadline(dl) { recv() !>; }` |


### Library Functions

These are normal functions in `concurrent_c.h` with `cc_` prefix to avoid naming conflicts:


| Function                  | Purpose                                | Example                                   |
| ------------------------- | -------------------------------------- | ----------------------------------------- |
| `cc_ok(value)`            | Construct T!>(E) success (inferred)    | `return cc_ok(42);`                       |
| `cc_err(error)`           | Construct T!>(E) error (inferred)      | `return cc_err(MyError_NotFound);`        |
| `cc_ok(void)`             | Construct void!>(E) success (inferred) | `return cc_ok(void);`                     |
| `cc_err(CC_ERR_*, "msg")` | CCError shorthand                      | `return cc_err(CC_ERR_NOT_FOUND, "msg");` |
| `cc_ok(T, value)`         | T!>(CCError) success (explicit)        | `return cc_ok(int, 42);`                  |
| `cc_ok(T, E, value)`      | T!>(E) success (explicit)              | `return cc_ok(int, MyError, 42);`         |
| `cc_err(T, error)`        | T!>(CCError) error (explicit)          | `return cc_err(int, CC_ERROR(...));`      |
| `cc_err(T, E, error)`     | T!>(E) error (explicit)                | `return cc_err(int, MyError, err);`       |
| `cc_move(x)`              | Explicit move of move-only value       | `ch.send_take(arr, cc_move(arr));`        |
| `cc_current_deadline()`   | Handle of current `@with_deadline` scope | `CCDeadline* dl = cc_current_deadline();` |
| `cc_deadline_expired(dl)` | Polling check on a deadline handle     | `if (cc_deadline_expired(dl)) break;`     |
| `cc_cancel()` / `cc_is_cancelled()` | §8.5.3 fallback: target the bare current deadline scope | `while (!cc_is_cancelled()) { ... }` |
| `cc_io_avail(res)` | True exactly for `ok(true)` channel results | `while (cc_io_avail(rx.recv(&x))) { ... }` |


### Result Methods (UFCS)

Result types (`T!>(E)`) support these methods via UFCS:


| Method             | Purpose              | Example                                |
| ------------------ | -------------------- | -------------------------------------- |
| `r.is_ok()`        | Check if success     | `if (r.is_ok()) { ... }`               |
| `r.is_err()`       | Check if error       | `if (r.is_err()) handle(cc_error(r));` |
| `r.value()`        | Get value or abort   | `int v = r.value();`                   |
| `r.error()`        | Get error or abort   | `CCIoError e = r.error();`             |
| `r.unwrap_or(def)` | Get value or default | `int v = r.unwrap_or(0);`              |


**Macro helpers (for C interop/generated code):**


| Macro                      | Purpose                | Example                                           |
| -------------------------- | ---------------------- | ------------------------------------------------- |
| `cc_is_ok(res)`            | Check if success       | `if (cc_is_ok(res)) { ... }`                      |
| `cc_is_err(res)`           | Check if error         | `if (cc_is_err(res)) handle_err();`               |
| `cc_unwrap(res)`           | Get value (primitives) | `int v = cc_unwrap(res);`                         |
| `cc_unwrap_as(res, T)`     | Get value (structs)    | `MyStruct v = cc_unwrap_as(res, MyStruct);`       |
| `cc_unwrap_err(res)`       | Get error (primitives) | `CCError e = cc_unwrap_err(res);`                 |
| `cc_unwrap_err_as(res, E)` | Get error (structs)    | `CCIoError e = cc_unwrap_err_as(res, CCIoError);` |


**C ABI naming:** All runtime/stdlib symbols use `CC`*/`cc_`* prefixes to avoid collisions with user code. The compiler automatically resolves short aliases (`String`, `Arena`, etc.) to their prefixed forms (`CCString`, `CCArena`, etc.) during compilation.

---

## Style Guide

This section documents recommended style for Concurrent-C code.

**UFCS.** Method and free spellings name the same API when both exist
(`recv.method(args)` ↔ `cc_type_method(&recv, args)`). Concurrent-C examples
prefer the method form on the semantic receiver; the free twin remains valid
(lowered C, host headers, and sites where the receiver is awkward).

### Type Annotation Spacing

**Rule:** Type constructors are written **without spaces**. This applies to all compound type syntax.


| Type                  | Correct                  | Incorrect                                                   |
| --------------------- | ------------------------ | ----------------------------------------------------------- |
| Result                | `T!>(E)`                 | `T ! E` or `T! E`                                           |
| Slice                 | `char[:]`                | `char [:]` or `char[ : ]`                                   |
| Nested slice          | `char[::]`, `char[:::]`  | `char[:: ]` or C-array spellings                            |
| Fixed-length slice    | `char[n:]`               | `char[:n]`                                                  |
| Unique slice          | `char[:!]`               | `char[: !]`                                                 |
| Sentinel slice        | `char[:0]`               | `char[: 0 ]`                                                |
| Unique sentinel slice | `char[:0!]`              | `char[: 0 !]`                                               |
| Generic type          | `Vec::[int]`             | `Vec :: [ int ]`                                            |
| Nested generic        | `Map::[K, Vec::[int]]`   | `Map :: [ K, Vec :: [ int ] ]` (standard bracket nesting)   |


**Examples:**

**Nested slice note:** `T[::]`, `T[:::]`, and deeper forms mean nested slice ranks (`T[:]`, `T[:][:]`, `T[:][:][:]`, etc.). These are slice/view types, not C arrays. Refinements compose on the innermost rank (`T[:0:]`, `T[4::]`, etc.) and still lower to the same slice ABI family.

```c
// Correct
int!>(IoError) read_int(char[:] data) {
    char[:] trimmed = data.trim();
    return cc_ok(parse_int(trimmed));
}

Vec::[int] numbers@(arena) @destroy;
Map::[char[:], int] registry@(arena) @destroy;
```

**Rule (type arguments, normative).** `::[...]` specializes the name it
follows — free or member. On a member, the type argument binds the
member's type formal: `arena.allocT::[double](16)`,
`task.block_on::[double]()`. When the member has a typed destination,
the destination supplies the type and the argument may be omitted
(`double* xs = arena.allocT(16)`); with neither a destination nor an
explicit argument the call is ill-formed. `::[...]` on a member with no
type formal is ill-formed. A trailing capital `T` in a member name
marks a type-formal member.

```c

// Incorrect (visual noise, harder to parse)
int ! IoError read_int (char [ : ] data) {
    // ...
}
```

**Slice declarations:** type position and declarator position are equivalent — `char[:] s` ≡ `char s[:]`, in locals, parameters, and struct fields alike.

**Typed slice instances:** a non-char element type instantiates the `CCSlice` family factory: `double[:]` and `CCSlice::[double]` both name `CCSlice_double`, a distinct struct with a `CCSlice base` field, `@typeview on CCSlice_* { as: base; }`, and element-wise methods with `sizeof(T)` in hand, named `Name_<member>` (the same instance-prefix convention as Vec and Map families). `char[:]` stays bare `CCSlice`. `len()`/`at(i)`/`sub(a,b)` count and index elements (`sub` returns the same instance type); `bytes()` returns an honestly byte-measured `CCSlice` (`len` scales by `sizeof(T)`). Scalar instances are pre-declared in `cc_slice.cch`; any other element type auto-instantiates at first use — the compiler splices the factory body after the element's definition. A hand-written declaration (`CC_DECL_SLICE(T)` for a single-token element, `CC_DECL_SLICE_SPEC(Name, T)` otherwise) is honored and suppresses the splice, for plain-C consumers and headers. Instance types are distinct in `_Generic`, so type-directed dispatch (e.g. dynamic-sink marshaling) sees the element type in any expression position.

Erasure is a spelling: `xs.base` reads the raw element-counted core; passing an instance by value where `CCSlice` is expected autocasts through `bytes()` (scaled). Byte-oriented `CCSlice` methods remain reachable on instances through the typeview `as:` face retry; element-wise shadows win by name when declared. Two initializer forms lower specially:

- `char[:0] s = "lit";` / `char[:] s = "lit";` / `CCSlice s = "lit";` (and Unique/Shared) — a string literal initializing a by-value slice lowers to `CC_SLICE_LIT(lit)`: a canonical static view; `len` excludes the terminating NUL. Prefer the sentinel spelling `char[:0]` when the bytes are known NUL-terminated.
- `T xs[:] = {a, b, c};` — the elements materialize in a hidden block-scope backing array; the slice is an untracked view with `len` = element count. The view shares the enclosing block's lifetime, like the C array it replaces.

`{0}` and designated initializers (`{ .ptr = p, .len = n }`) remain ordinary C struct initialization of the slice header, not element lists.

Ordinary sites on the slice family may read `.ptr`, `.len`, and `.id`; they may not store fields. Typed `T[:]` is a distinct wrapper (`CCSlice_T` with a `CCSlice base` embed). `@typeview on CCSlice_* { as: base; }` retries a field miss through that embed, so `xs.len` / `xs.ptr` / `xs.id` lower to `xs.base.…`. `.ptr` is `char *`; element index is `(T *)xs.ptr`. Explicit `xs.base` remains the erased core. See `draft_as.md` and `draft_facets.md` for the unnamed `@typeview on CCSlice { r: *; }` facet.

---

## 2. Foundations

This section defines the rules about values, closures, lexing, and parsing that every primitive in §1 and every library type in later sections obeys.

**Rule:** Types with runtime-managed invariants that are provided by a
library family are initialized according to that family's emitted C contract.
Channel handles `T[~... >]` / `T[~... <]` are initialized by a channel-pair
constructor. Plain C types follow C initialization rules.

**Exception:** `T!>(E)` (Result types) do not require immediate initialization and instead follow **definite assignment** rules. A `T!>(E)` variable may be declared without an initializer if the compiler can prove it is assigned on all control-flow paths before any use or before scope exit.

**Rule (Concurrent-C type initialization):**

- `T!>(E)` uses **definite assignment**: the compiler verifies that a `T!>(E)` variable is assigned on all paths before any use or scope exit.
- Channel handles are initialized by a channel-pair constructor.
- `Map::[K,V]` requires an explicit initializer (needs arena).

**Rule (T!>(E) and destructors):** Definite assignment for `T!>(E)` is required because the destructor must know which arm (`T` or `E`) to drop. An uninitialized `T!>(E)` at scope exit would have undefined destructor behavior—the compiler cannot determine whether to drop `value` or `error`. This is why definite assignment analysis must cover all paths to scope exit, not just paths to explicit reads.

```c
int!>(Error) x;              // OK if definitely assigned before use
if (cond) {
    x = cc_ok(42);
} else {
    x = cc_err(Error.Oops);
}
use(x);                   // OK: assigned on all paths

int!>(Error) y;              // ERROR: not definitely assigned
if (cond) {
    y = cc_ok(1);
}
use(y);                   // ERROR: y may be uninitialized
// also ERROR at scope exit: destructor doesn't know which arm to drop
```

---

### 2.1 Value Categories & Moves

Concurrent-C distinguishes **copyable** and **move-only** values:

**Copyable types:**

- All C primitive types
- Structs containing only copyable fields
- `T!>(E)` where `T` and `E` are copyable

**Move-only types:**

- `CCTaskIntptr` (owns one pollable computation)
- `Map::[K,V]` (arena-backed, unique ownership)
- `T!>(E)` where `T` or `E` is move-only

**Slices (`T[:]`):** The slice type itself is always the same, but **copyability depends on the value's provenance**:

- Slices from arena, stack, or static sources are **copyable** (view slices)
- Slices from `recv()` or `adopt()` are **move-only** (unique slices, `id.is_unique=1`)

This is a value-level property, not a type-level distinction. The compiler tracks provenance to enforce it.

**Rule (foreign memory is untracked).** A slice over memory the program does not own — a buffer belonging to an embedded runtime, a `mmap`, a callback's argument — is minted with `cc_slice_from_buffer`, which records no provenance epoch. Claiming an arena's epoch for bytes that arena did not allocate makes the compiler's lifetime reasoning wrong in the one direction it cannot detect: the epoch would say the bytes outlive a reset that has nothing to do with them, or survive a scope that does not govern them. Untracked is the honest answer, and it means the borrow is valid only for as long as the foreign owner says. Copy into an arena to outlive that window. POSIX `CCMappedFile` (`<ccc/std/mmap.cch>`, opt-in) is the owner; `as_slice()` is untracked.

**Move semantics:**

```c
// For copyable types: assignment copies
int a = 1;
int b = a;    // copy, both valid
```

**Move-only types (like unique slices):**

Move-only values cannot be copied; ownership must be explicitly transferred with `cc_move()`:

```c
// Example 1: Unique slice from adopt()
unsafe {
    char[:] x = adopt(ptr, len, deleter);  // x is unique (move-only)
    char[:] y = x;                         // ERROR: cannot copy move-only value
    char[:] y = cc_move(x);                // OK: x is now invalid, y owns buffer
    use(x);                                // ERROR: use after move
}

// Example 2: Unique slice from channel
char[:] x;
bool !>(CCIoError) got = @await ch.recv(&x);   // recv writes into &x
if (cc_is_err(got) || !cc_value(got)) return; // channel closed/drained
char[:] y = x;                                // ERROR: cannot copy unique slice
char[:] y = cc_move(x);                       // OK: x is invalid, y owns buffer
```

**Move semantics for different types:**

**Results (`T!>(E)`):** Movable as whole values. To extract the success payload, use the `!>` / `?>` operators, statement-level `@err` / `@errhandler`, or explicit checks (`cc_is_ok` / `cc_value`, UFCS, etc.).

```c
int!>(Error) r = cc_ok(42);
int!>(Error) s = r;        // copies (int and Error are copyable)
int v = r !>(e) return cc_err(e);   // extracts value, propagates error
```

**Bare unique slices:** Moving invalidates source; subsequent use is a compile-time error.

```c
unsafe {
    char[:] buf = adopt(ptr, len, deleter);
    char[:] copy = cc_move(buf);
    use(buf);  // ERROR: use after move
}
```

**Rule (move contexts):** Move is implicit (or required) in:

- `return expr` (move-only value)
- By-value parameters: `fn(move_only_val)` moves the value
- `send_take(ch, slice)` moves on success
- Explicit `cc_move()` for clarity

**Rule (no implicit last-use move):** Move-only values have no implicit "last use" move. Explicit move or drop is required. This prevents silent moves from distant code.

**Rule (move dead-state):** `cc_move(x)` transfers the value and leaves `x`
empty. Generated drop, variant transition, and `@destroy` on the source
must not still own. The empty bytes are teardown, not a user diagnostic.

**Rule (use-after-move):** Compile-time error for:

- Bare unique slice after move
- Borrowed views from moved owner
- Any later read or write of the moved-from name that is not generated teardown

**Rule (function parameters):** Move-only values move by-value; pass as pointer to retain ownership:

```c
void take_ownership(char[:] buf);     // moves buf
void borrow(char[:]* buf);            // borrows, caller retains
```

---

### 2.2 Closure Captures and Thread Safety

When a closure is captured for use in another thread or task, the captured values are copied into the closure. This creates constraints on what can be captured.

**Rule (channel send vs closure capture):** Channel `send()` copies the value into channel-internal storage. Sendability rules for **closure captures** (threads/tasks) are separate from channel operations. Channel send of a **non-unique arena-backed slice borrow** is ill-formed (see **channel-stable-borrow** below); materialize first.

**Closure capture rules:**

```c
char buf[100];
char[:] stack_slice = buf[:];

// Channel send: OK (deep-copies bytes into channel buffer)
@await ch.send(stack_slice);

// Spawned task: ERROR (closure copies slice struct, ptr points to dead stack)
n.spawn(() => {
    use(stack_slice);  // BAD: the view points at the caller's stack frame
});
```

**Rule (capture eligibility):** A value can be captured in a thread/task closure iff:

1. It does not contain pointers to stack memory (which dies when the spawning function returns)
2. It does not contain pointers to arena memory that may be freed before the thread/task completes
3. It is not a scope-bound handle registered by a synchronization or resource library

**Capture eligibility table:**


| Type                     | Capturable?                    | Notes                                       |
| ------------------------ | ------------------------------ | ------------------------------------------- |
| Primitives               | Yes                            | Value types                                 |
| Structs                  | Yes iff all fields capturable  | Structural                                  |
| `T!>(E)`                 | Yes iff inner types capturable | Structural                                  |
| Arena slices             | **Depends**                    | Only if arena provably outlives thread/task |
| Static slices            | Yes                            | Lives forever                               |
| Stack slices             | **No**                         | Frame dies on return                        |
| Unique slices (`recv()`) | Yes                            | Owned, no external pointer                  |
| `Map::[K,V]`               | Yes iff contents capturable    | Move-only; moved into closure               |
| `CCTaskIntptr`             | Yes                            | Explicit poll/drop ownership                |
| Channels                 | Yes                            | Thread-safe handles                         |
| Registered scope-bound synchronization handle | **No**             | Scope-bound                                 |
| `CCNursery`             | Yes                            | Handle; children must not outlive it        |
| Raw pointers             | Yes                            | But safety is caller's responsibility       |


**Rule (stack slice escape):** Stack slices cannot be captured in any closure that may outlive the current stack frame. This is enforced at compile time.

**Rule (enforcement and UB):** Stack slice escape is always a compile-time error when the escape is provable. For cases where escape cannot be determined at compile time, the behavior is undefined in release builds and trapped (runtime error) in debug builds.

**Rule (arena slice capture):** Arena slices can be captured if the compiler can prove the arena outlives the thread/task. In practice, this means the arena must be declared in an enclosing scope that joins the thread/task before the arena is freed **or reset**.

**Rule (arena epoch pin on capture):** Capturing a non-unique arena-backed slice or arena-allocated pointer into a nursery/thread task **pins** that arena's provenance epoch until the nursery (or equivalent join scope) ends. While the pin is live, epoch-ending ops on that arena (`cc_arena_reset` / `cc_arena_restore` / `cc_arena_free` / `cc_arena_destroy` / `cc_arena_detach`) are a compile-time error. Materialize into a unique/stable slice (or `@unsafe`) to escape the pin.

**Rule (arena reset with live borrow):** `cc_arena_reset` / `cc_arena_restore` is a compile-time error when a derived arena slice borrow of that arena is still within its lexical enclosing block. End the borrow's scope before reset, or copy into another arena / unique slice first. Owner `free` / `destroy` at end of the same block after the last use remains well-formed.

**Rule (channel-stable-borrow):** Channel send of a non-unique slice view (arena-backed, stack, or untracked `from_buffer`) is a compile-time error. The payload may outlive the storage that minted the view. Unique (`T[:!]` / `adopt` / `recv`) and static / canonical slices may use ordinary `send` / `try_send`.

The default idiom is reserve-then-write (`send_into` / `try_send_into`, same family as `send_task`): admit a slot, construct the element into that slot, and land variable payload bytes in the provided arena. Building a temporary with foreign slice views and copying it through a later send is an anti-pattern.

**Rule (channel-stable-borrow, aggregates):** Channel send of a by-value aggregate is checked field-wise. A non-unique `CCSlice` / `T[:]` field is ill-formed unless the same aggregate also contains a `CCArena` field (ownership of the backing epoch rides with the message) or the slice field is unique / static. Aggregates that carry only `CCArena` plus length / offset (no slice on the wire) are well-formed; the receiver rebuilds a local view (for example with `arena.slice(ptr, len)` / `cc_arena_slice`). A `cc_slice_*` constructor used only to initialize a field of a named aggregate does not brand the whole aggregate as a bare slice borrow.

**Rule (pointer-channel-send-ban):** Channel send of a by-value aggregate that contains a raw pointer field (`T*`, `char*`, …) is a compile-time error. The sender may free the pointee while the receiver still holds a copy of the pointer. Branded fields (`CCSlice`, `CCArena`, …) are not raw pointers for this rule. Bare `T*` payloads (handle protocols) are outside this rule; prefer owned bytes (`T[:!]` / static slice) or aggregates without raw pointer fields.

**Rule (owned-buffer-child-free-ban):** `free` / `cc_slice_destroy` / `@destroy` on a non-owning arena-allocated pointer or non-unique arena slice view is a compile-time error. Only the owning scope may delete: arena `@destroy` / `cc_arena_free`, or unique `T[:!]` destruction. Untracked heap (`malloc`) remains outside this rule.

**Rule (pointer-alias capture mutation):** Value-capturing `T* p = &local` into a task/thread closure and then writing through `*p` / `p->field` is a compile-time error (same class as mutating a reference capture). Use the shipped `cc_atomic_*` surface, a registered synchronization library, or `@unsafe`.

**Capture list modes:**

| Spelling | Meaning |
|----------|---------|
| `x` | By-value copy into the env |
| `&x` / `@safe &x` | Borrow (pointer in env); not destroyed on drop |
| `@own x` | Owned value or owned pointer; destroyed on never-entered drop (below) |

`@own` on a pointer requires a destroyable pointee (registered destroy hook for
the pointee type). `@own` is not a typeview `as:` face: owning a field does not make the outer
type an is-a embed of that field’s type.

**Rule (dropped without call):** When a single-shot closure is destroyed without
the body having been entered, the implementation runs `env_drop`, which destroys
each owned capture in reverse capture-list order, then frees the environment.
Borrow / `@safe` captures are not destroyed. Owned captures are destroyed in
`env_drop`; the body uses them and must not also destroy them. After a
successful call, `env_drop` still runs; destroy hooks are idempotent (or fields
are already inert).

```c
static void cc_closure__N_env_drop(void* p) {
    cc_closure__N_env* e = (cc_closure__N_env*)p;
    if (!e) return;
    /* reverse capture-list order for @own fields */
    if (e->cap) { type_destroy(e->cap); e->cap = NULL; }
    free(e);
}
```

The compiler generates `env_drop` from the capture list and each owned capture’s
registered destroy. Hook order among owned captures is the capture-list order,
LIFO’d. Fixtures for this path target never-entered closures only (e.g.
`send_into` builder not built, cancel-before-start). They do not use “consumer
stopped receiving after `send_task`” — that API spawns at send, so the body
usually runs.

```c
void ok_pattern() {
    CCArena a = cc_arena_heap(kilobytes(64));
    char[:] s = a.alloc_slice_bytes(100);

    CCNursery n = cc_nursery_create() !> @destroy;
    n.spawn(() => {
        use(s);  // OK: nursery @destroy joins before arena freed
    });
}  // arena freed here, after children joined

void bad_pattern() {
    CCNursery n = cc_nursery_create() !> @destroy;
    {
        CCArena a = cc_arena_heap(kilobytes(64));
        char[:] s = a.alloc_slice_bytes(100);
        n.spawn(() => {
            use(s);  // ERROR: arena may be freed before the task runs
        });
    }  // arena freed here
}

void bad_reset_while_borrow() {
    CCArena a = cc_arena_heap(kilobytes(64));
    char[:] s = a.alloc_slice_bytes(100);
    use(s);
    cc_arena_reset(&a);  // ERROR: borrow of a still in scope
}

void ok_reset_after_borrow_scope() {
    CCArena a = cc_arena_heap(kilobytes(64));
    {
        char[:] s = a.alloc_slice_bytes(100);
        use(s);
    }
    cc_arena_reset(&a);  // OK
}
```

---

### 2.3 Lexing & Parsing

Concurrent-C extends C syntax with new operators and keywords in specific contexts:

**Keyword sigil policy (normative):** Every CC-introduced keyword
carries a leading `@` sigil at the lexer level. The set includes
(non-exhaustive): `@async`, `@await`, `@blocking`, `@noblock`,
`@match` (reserved and rejected), `@defer`, `@defer(err)`, `@defer(ok)`, `@cancel_defer`,
`@errhandler`, `@err`, `@destroy`, `@with_deadline`, `@parallel`, `@serial`, `@comptime`,
`@for`, `@switch`, `@latency_sensitive`, `@scoped`, `@unsafe`, `@slice`, `@string`,
`@typehooks`, `@typeview`.
The bare
identifiers `async`, `await`, `blocking`, `noblock`, `match`, `defer`,
`nursery`, `spawn`, `lock`, `comptime`, `cancel`, etc. are **not**
reserved — they may be used freely as variable, field, parameter, or
function names. Only the `@`-prefixed form triggers the compiler
construct. The single exception is `unsafe`, which is bare (inherited
from common C extensions).

This rule eliminates keyword-identifier collisions (e.g., a variable
named `@match` shadowing the construct) and gives every CC-aware scan
pass an unambiguous `@`-anchored sentinel, which removes an entire
class of keyword-in-comment / keyword-in-string false-positive bugs.

**Type-context operators (not valid in expression context):**


| Syntax                        | Meaning               | Notes                         |
| ----------------------------- | --------------------- | ----------------------------- |
| `T!>(E)`                      | Result type           | Postfix, binds error type `E` |
| `T[:]`                        | Slice type            | Distinct from C array `T[]`   |
| `T[~... >]` / `T[~... <]`     | Channel handle type   | `~` is not bitwise-not here   |
| `T[~n N:M >]` / `T[~n N:M <]` | Channel with topology | `N:M` is topology, not label  |

---

### 2.4 Documentation comments (CCDoc)

**CCDoc** is Concurrent-C’s documentation comment form. It is JSDoc-shaped:
a `/** … */` block whose tags use `@name` inside the comment. CCDoc tags do
not compete with language `@` keywords; they appear only inside documentation
comments.

**Attachment.** A CCDoc block that immediately precedes a declaration —
separated only by whitespace and ordinary (`//` / `/* … */`) comments —
documents that declaration. The block binds to the next declarator (function,
type, variable, or similar).

**Summary.** Free text before the first `@tag` is the summary. Compact UIs use
the one-line summary: the first non-empty line of that free text, or the first
paragraph up to a blank line when a multi-line summary is preferred.

**Tags.** The following tag names are the reserved CCDoc vocabulary. Tooling
may ignore unknown tags. Tag text runs to the end of the line unless a later
revision defines multi-line tag bodies.

| Tag | Meaning |
| --- | ------- |
| `@param <name> <text>` | Documents a parameter. |
| `@returns` / `@return <text>` | Documents the success / return value. |
| `@throws` / `@errors <text>` | Documents failure / Result error domain. |
| `@deprecated [<text>]` | Marks the declaration deprecated. |
| `@example <text>` | Short usage example. |
| `@see <ref>` | Cross-reference (name, path, or URI). |
| `@task [<text>]` | In `.shcc` units with synthetic `main`, opts the following declaration into `@` task discovery when the signature is a valid task shape (§9.5.2a). Optional text is the listing summary (else the leading free-text summary). In `.ccs` / non-task contexts the tag is documentation only and does not affect lowering. |

```c
/**
 * Read metrics from path into an arena-backed map.
 *
 * @param path  NUL-terminated filesystem path
 * @param a     arena for keys and map storage
 * @returns     populated Metrics map
 * @errors      CC_ERR_IO, CC_ERR_PARSE
 */
static Metrics !>(CCError) load_metrics(const char* path, CCArena a);
```

Aside from `.shcc` `@task` opt-in (§9.5.2a), CCDoc does not affect program
semantics or lowering. Emission of HTML indexes, hover cards, or markdown is a
tooling concern that consumes the same blocks.

---

## 3. Core Types

This section defines the fundamental value-level building blocks:

- **§3.1 Results (`T!>(E)` / `T?>(E)`)** — success or failure; mandatory vs optional consumption
- **§3.2 Type Precedence** — how type modifiers bind
- **§3.3 Arrays and Slices** — fixed arrays and views
- **§3.4 Slice ABI** — provenance metadata layout

---

### 3.1 Results (`T!>(E)` / `T?>(E)`)

`T!>(E)` and `T?>(E)` represent **success or failure** with an explicit error
value. Both lower to the same `CCResult_T_E` tagged union and share the consume
operators (`?>`, `!>`, `@errhandler`, `@err`). The marker is normative:

- **`T!>(E)` (mandatory).** Every call must be consumed: `?>`, `!>`, assignment
  to a result-typed destination, `return`, or `(void)` cast. A bare statement
  `f();` is ill-formed.
- **`T?>(E)` (optional).** Same ABI and same operators. A bare statement `f();`
  is well-formed — the declaration opts into optional ignore. Use `!>` when a
  failure must propagate (for example load-bearing I/O); omit it for diagnostics
  that must not hijack the ambient handler.

Unwrapping roles: `?>` is the default-value operator (pure expression RHS);
`!>` is the error-handler operator (statement or expression position); `@err(e);`
forwards inside a `!>` body; bare `call !>;` dispatches to a matching
`@errhandler`. See **Unwrapping Results** below.

- **Unified constructor syntax**:
  - **Inferred (preferred inside a function returning `T!>(E)`):**
    - `cc_ok(value)` - construct `T!>(E)` success (T,E inferred from function return type)
    - `cc_err(error)` - construct `T!>(E)` error (T,E inferred from function return type)
    - `cc_err(CC_ERR_*)` or `cc_err(CC_ERR_*, "msg")` - shorthand for `CC_ERROR(...)` when `E` is `CCError`
  - **Explicit (required outside return-context or when ambiguous):**
    - `cc_ok(T, value)` - construct `T!>(CCError)` success
    - `cc_ok(T, E, value)` - construct `T!>(E)` success (custom error type)
    - `cc_ok(void)` - construct `void!>(CCError)` success
    - `cc_err(T, error)` - construct `T!>(CCError)` error
    - `cc_err(T, E, error)` - construct `T!>(E)` error (custom error type)
- Shorthand for `Result::[T, E]`.

```c
// Inferred constructors inside a function returning T!>(E)
int!>(CCError) parse_int_safe(char[:] s) {
    if (s.len() == 0) return cc_err(CC_ERR_INVALID_ARG, "empty");
    int val = parse_int(s);
    return cc_ok(val);
}

// Inferred constructors (preferred)
int!>(CCError) x = cc_ok(42);
int!>(CCError) y = cc_err(CC_ERROR(CC_ERR_NOT_FOUND, "file not found"));
int!>(IoError) a = cc_ok(42);
int!>(IoError) b = cc_err(IoError_FileNotFound);

// Explicit constructors (still available)
int!>(CCError) x2 = cc_ok(int, 42);
int!>(CCError) y2 = cc_err(int, CC_ERROR(CC_ERR_NOT_FOUND, "file not found"));
int!>(IoError) a2 = cc_ok(int, IoError, 42);
int!>(IoError) b2 = cc_err(int, IoError, IoError_FileNotFound);

if (cc_is_ok(x)) use(cc_value(x));
else handle(cc_error(x));
```

**Lowering (normative):**

```c
// T!>(E) lowers to a tagged union:
struct CCResult_T_E {
    bool ok;
    union { T value; E error; } u;
};

// Constructor macros (explicit forms):
// cc_ok(void)        → cc_ok_CCResult_void_CCError()     // 1 arg: void!>(CCError)
// cc_ok(T, v)        → cc_ok_CCResult_T_CCError(v)       // 2 args: T!>(CCError)
// cc_ok(T, E, v)     → cc_ok_CCResult_T_E(v)             // 3 args: T!>(E) (custom)
// cc_err(T, e)       → cc_err_CCResult_T_CCError(e)      // 2 args: T!>(CCError)
// cc_err(T, E, e)    → cc_err_CCResult_T_E(e)            // 3 args: T!>(E) (custom)
```

Canonical accessors (normative):

- `cc_is_ok(r)` / `cc_is_err(r)` read the tag
- `cc_value(r)` reads the success payload
- `cc_error(r)` reads the error payload

Lowering is explicit and ABI-preserving:

- `cc_is_ok(r)` lowers to `(r).ok`
- `cc_is_err(r)` lowers to `(!(r).ok)`
- `cc_value(r)` lowers to `(r).u.value`
- `cc_error(r)` lowers to `(r).u.error`

These accessors are the idiomatic source-level API. Direct `.u.value` / `.u.error`
access is a C-interop detail, not the preferred surface style.

**Rule (reach for the accessors where the operators cannot go).** `!>` and `?>` are the surface style in ordinary CC code, but they are compiler constructs: they are lowered by passes that do not run over `@emit` fragments, and they are unavailable to anything the compiler does not process. The accessors are ordinary macros over the struct, so they work in plain C, in a macro body, and in generated code. A generator consuming a Result writes the accessors, not the sigil.

**Rule (name a Result without naming its box).** `__typeof__` yields the concrete type without evaluating its operand, so a Result is bound without spelling the mangled instance name:

```c
__typeof__(f(x)) r = f(x);
if (cc_is_err(r)) return handle(cc_error(r));
use(cc_value(r));
```

This is the form to use wherever the concrete name is unavailable or would be a guess — macro bodies, `@emit` templates, and reflection-driven generation, where the ok type is only known as a spelling. Writing `CCResult_long_long_CCError` by hand re-derives the canonicalizer's answer, and re-deriving it is how the two spellings drift.

**Lowering examples (normative):**

```c
// Source
bool !>(CCIoError) read_res = cc_file_read(in_ptr, &blk_arena, BLOCK_SIZE, &data);
if (cc_is_ok(read_res)) {
    bool available = cc_value(read_res);
} else {
    CCIoError e = cc_error(read_res);
}

// Lowered C
CCResult_bool_CCIoError read_res = cc_file_read(in_ptr, &blk_arena, BLOCK_SIZE, &data);
if (read_res.ok) {
    bool available = read_res.u.value;
} else {
    CCIoError e = read_res.u.error;
}
```

**Rule (active field):** Only the active union member is initialized and valid, as determined by `ok`. When `ok == true`, `u.value` is active; when `ok == false`, `u.error` is active. Reading the inactive member is undefined behavior (and is a compile-time error where statically provable).

**Rule (drop for T!>(E)):** On scope exit, the destructor for the **active arm** runs if it has destructor semantics:

- If `ok == true` and `T` has a destructor, drop `u.value`
- If `ok == false` and `E` has a destructor, drop `u.error`
- If the value was moved out via `!>` propagation or pattern match, no destructor runs for that arm

**Result methods (UFCS):**

Result types support the following methods via UFCS (Uniform Function Call Syntax):


| Method                 | Returns | Description                                     |
| ---------------------- | ------- | ----------------------------------------------- |
| `r.is_ok()`            | `bool`  | Returns `true` if result is success             |
| `r.is_err()`           | `bool`  | Returns `true` if result is error               |
| `r.value()`            | `T`     | Returns value if success, **aborts** if error   |
| `r.error()`            | `E`     | Returns error if present, **aborts** if success |
| `r.unwrap_or(default)` | `T`     | Returns value if success, `default` if error    |


**Macro helpers:**

For generated C code, these macros provide direct access without UFCS lowering:


| Macro                      | Returns | Description                                           |
| -------------------------- | ------- | ----------------------------------------------------- |
| `cc_is_ok(res)`            | `bool`  | True if result is success                             |
| `cc_is_err(res)`           | `bool`  | True if result is error                               |
| `cc_value(res)`            | `T`     | Get success payload (valid only when `cc_is_ok(res)`) |
| `cc_error(res)`            | `E`     | Get error payload (valid only when `cc_is_err(res)`)  |
| `cc_unwrap(res)`           | `T`     | Get value (for primitives)                            |
| `cc_unwrap_err(res)`       | `E`     | Get error (for primitives)                            |
| `cc_unwrap_as(res, T)`     | `T`     | Get value as type T (for structs)                     |
| `cc_unwrap_err_as(res, E)` | `E`     | Get error as type E (for structs)                     |


**Note:** Use `cc_unwrap(res)` for primitive types (int, pointer, etc.). For struct return types, use `cc_unwrap_as(res, StructType)` to ensure correct type handling.

**Type macros:**


| Macro            | Expands To        | Description                   |
| ---------------- | ----------------- | ----------------------------- |
| `CCRes(T, E)`    | `CCResult_T_E`    | Result type name (C interop)  |
| `CCResPtr(T, E)` | `CCResult_Tptr_E` | Result of pointer (C interop) |


```c
int!>(CCError) parse(char[:] s);

int!>(CCError) r = parse("42");

// Method-style access (UFCS)
if (r.is_ok()) {
    int v = r.value();      // safe after is_ok() check
    use(v);
}

// Or use unwrap_or for default value
int v = r.unwrap_or(0);      // returns 0 on error

// Canonical accessor style
if (cc_is_ok(r)) use(cc_value(r));

// Macro-style access (for generated C code)
// Prefer sigil types in .ccs; use CCRes* only for C interop or explicit name mangling.
int!>(CCError) res = parse("42");
if (cc_is_err(res)) {
    CCError err = cc_error(res);
    handle_error(err);
    return;
}
int value = cc_value(res);
```

**Lowering:** `r.value()` and `r.error()` lower through the result-family UFCS definition for `T!>(E)`; a backend may emit generic result helper macros or equivalent family helpers.

**Rule (value/error panic):** Calling `.value()` on an error result or `.error()` on a success result prints an error message and aborts. Use `.unwrap_or(default)` or check `.is_ok()` / `.is_err()` first when failure is possible.

**Declaring custom Result types in headers:**

When defining Result types in `.cch` header files, use guards to prevent redefinition:

```c
// In your_types.cch
#include <ccc/cc_result.cch>

// Declare custom Result type with guard
#ifndef CCResult_MyData_MyError_DEFINED
#define CCResult_MyData_MyError_DEFINED 1
CC_DECL_RESULT_SPEC(CCResult_MyData_MyError, MyData, MyError)
#endif

// Use CCRes(T, E) macro in function signatures
CCRes(MyData, MyError) my_function(int arg);
```

**Why guards?** The compiler automatically generates `CC_DECL_RESULT_SPEC` calls when you use `T !>(E)` syntax in `.ccs` source files. The `#ifndef ..._DEFINED` guards prevent redefinition errors when your header also declares the same type.

#### Unwrapping Results

**Invariants (normative).**

1. `**?>` is the default-value operator.** Its RHS is a pure C expression that produces a `T`. No divergent statements, no compound blocks, no bare shorthand. `EXPR ?>(e) DEFAULT_EXPR` scopes the binder `e` to `DEFAULT_EXPR`. Any other RHS shape (`?> { ... }`, `?> return …;`, `?> break;`, `?> continue;`, `?> goto …;`, `?> @err(…);`, or `?> ;`) is ill-formed and diagnosed with `'?>' RHS must be a value expression; use '!>' for error-handling logic` at the `?>` site.
2. `**!>` is the error-handler operator.** It is valid at both *statement position* (after `;`, `{`, `}`, or start-of-file, modulo label prefix) and *expression position* (RHS of `=`, inside `(`, `,`, `?`, `:`, `&&`, `||`, or immediately after `return`).
  - At **statement position** the body may fall through. Forms: `CALL !>;`, `CALL !> STMT;`, `CALL !> { BODY };`, `CALL !>(e) STMT;`, `CALL !>(e) { BODY };`.
  - At **expression position** the body must *visibly diverge*. Forms: `CALL !>(e) DIVERGENT_STMT;`, `CALL !>(e) { …; DIVERGENT_STMT };`, `CALL !> DIVERGENT_STMT;`, `CALL !> { …; DIVERGENT_STMT };`, and the bare `CALL !>;` which synthesizes a binder and inlines the matching `@errhandler` body for Result `E` (which must itself diverge). A non-divergent body at expression position is diagnosed with `expression-position '!>' body must diverge (return/break/continue/goto/@err/exit/abort/etc.)`. A bare `CALL !>;` at expression position with no matching `@errhandler` for `E` is ill-formed.
3. Error values are accessible only via an explicit `(ident)` binder on `?>` or `!>`. Neither operator creates an implicit `e` / `err` binding. The bare expression-position `!>;` form synthesizes a fresh internal binder that threads the error value through the inlined handler body.
4. `CALL !>;` at statement position runs a matching in-scope `@errhandler`, or the success path if the call succeeded. Candidates are `@errhandler` declarations in the current function whose declaration point is textually before the unwrap (block-nested, not hoisted); there is no cross-function search — that locality is why same-`E` re-entry is a compile error. From the unwrap outward, the first handler whose parameter type is exactly the call's Result error type `E` wins; if none, the first whose parameter `F` is reachable from `E` by a unique `@typeview` `as:` path wins (hop count is not a rank; two such face-matching handlers in the same block are ill-formed). No match is ill-formed: the diagnostic names the call's `T !>(E)` and the in-scope `@errhandler` parameter type(s) at the `!>` site. If that matching handler's body textually contains the call (same-`E` re-entry), the program is ill-formed — report via a helper such as `cc_error_log` / `cc_error_exit`, or an inline `!> { abort(); }`, not bare `!>;` inside the same-`E` handler.
5. `@err(IDENT);` inside a `!>` body forwards the bound error to the matching `@errhandler` for that unwrap's `E`. It is a **structured control-flow transfer** (not a returning call): any statement textually following it in the same block is unreachable and is a compile error.
6. `@errhandler(E e) STMT;` or `@errhandler(E e) { ... }` registers a block-local handler for Result error type `E`. A handler is in scope for a use only after its declaration point (same candidate set as invariant 4). The statement form is a thin forward (typically `cc_error_exit(e);`); the block form holds multiple statements. `CALL !>;` is the hoist of `CALL !>(e) STMT` when `STMT` is that handler body. When reached via `CALL !>;` at statement position, the handler body runs and control returns to the statement after the call — the handler may end in any statement. When reached via an `@err(e);` forward, via a bare expression-position `!>;`, or via an expression-position `!>` whose body inlines the handler, control never returns, so the handler body **must visibly diverge**. A `return` (or other soft-return) from the handler body discharges the enclosing function’s `@defer` / `@destroy` ledger via the same epilogue as any other return in that function (§5.1). Concretely: if any `@err(e);` targets a handler, or any expression-position `!>;` inlines a handler, that handler's body must end in one of:
  - `return EXPR;` / `return;`
    - `break;` / `continue;`
    - `goto LABEL;`
    - `@err(e);` (forwarding to an outer handler)
    - A call to one of the hardcoded noreturn functions: `exit`, `_Exit`, `_exit`, `abort`, `cc_error_exit`, `longjmp`, `siglongjmp`, `pthread_exit`, `__builtin_unreachable`, `__builtin_trap`
    - A `{ ... }` compound statement whose recursive last statement satisfies this rule.
     A forward-reached or `!>;`-inlined handler whose last statement does not satisfy this rule is a compile error at the `@errhandler` declaration site. The rule applies in `void` functions equally.
7. A **mandatory** result-typed call (`T!>(E)`) that is not consumed by `?>`, `!>`, `@err`, assignment to a result-typed destination, `return`, or a `(void)` cast is ill-formed. `(void)call();` is the one explicit-discard escape hatch for mandatory results. An **optional** result-typed call (`T?>(E)`) may appear as a bare statement `call();` — that is the declared optional-ignore form, not a silent drop.
8. A `!>` whose call is a bare name with no visible declaration in this unit or an included face is ill-formed. The compiler diagnoses that at the `!>` site and does not emit a Result unwrap.

**Grammar (normative, minus whitespace).**

```
qmark_expr    ::= expr '?>' expr
               |  expr '?>' '(' ident ')' expr   // RHS is always a pure C expression

bang_stmt     ::= call '!>' ';'                            // statement: use registered handler
               |  call '!>' stmt                            // statement: single-stmt body (may fall through)
               |  call '!>' '{' stmt* '}' ';'?              // statement: block body (may fall through)
               |  call '!>' '(' ident ')' stmt              // statement: binder + single stmt
               |  call '!>' '(' ident ')' '{' stmt* '}' ';'?  // statement: binder + block
               |  lvalue '=' call '!>' ';'                  // store Ok in lvalue
               |  lvalue '=' call '!>' stmt
               |  lvalue '=' call '!>' '{' stmt* '}' ';'?
               |  lvalue '=' call '!>' '(' ident ')' stmt
               |  lvalue '=' call '!>' '(' ident ')' '{' stmt* '}' ';'?

lvalue        ::= ident | '*' lvalue | lvalue '.' ident | lvalue '->' ident | lvalue '[' expr ']'

bang_expr     ::= call '!>' ';'                            // expression: matching @errhandler for E (inlined, must diverge)
               |  call '!>' divergent_stmt                  // expression: single divergent statement
               |  call '!>' '{' stmt* divergent_stmt '}'    // expression: block whose tail diverges
               |  call '!>' '(' ident ')' divergent_stmt    // expression: binder + divergent stmt
               |  call '!>' '(' ident ')' '{' stmt* divergent_stmt '}'  // expression: binder + block

divergent_stmt ::= 'return' expr? ';'
               |  'break' ';'
               |  'continue' ';'
               |  'goto' ident ';'
               |  '@err' '(' ident ')' ';'
               |  noreturn_call ';'           // exit/abort/longjmp/etc.

err_forward   ::= '@err' '(' ident ')' ';'                 // only inside bang_stmt or bang_expr body

err_handler   ::= '@errhandler' '(' type ident ')' '{' stmt* '}'
               |  '@errhandler' '(' type ident ')' stmt
```

**Semantics (normative, by form).**

- `EXPR ?> DEFAULT_EXPR` — Evaluate `EXPR` (a `T!>(E)` result) exactly once. If success, the expression's value is the unwrapped `T`. Otherwise the expression's value is `DEFAULT_EXPR`. `EXPR ?>(e) DEFAULT_EXPR` binds the error to `e`, scoped to `DEFAULT_EXPR`. `DEFAULT_EXPR` is always a pure C expression producing `T`.
- `CALL !>;` *(statement)* — Evaluate `CALL` exactly once. On success, the success payload is discarded. On error, dispatch uses the two-pass rule of invariant 4 (exact `E`, else unique `@typeview` `as:` path to a handler face `F`; binder projected along that path); control then falls through to the following statement. If no such handler is in scope, the program is ill-formed. If the call occurs inside that matching handler's body (same-`E` re-entry), the program is ill-formed.
- `lvalue = CALL !>;` *(statement)* — Same evaluation and error dispatch as statement `CALL !>;`. On success, store the Ok payload in `lvalue` (`p->f`, `p.f`, `*p`, `a[i]`, or a name). `CALL` may be a free-name or UFCS call. Binder and block variants store the same way. This is assignment, not a declaration.
- `CALL !> BODY` *(statement)* — Same, with `BODY` in place of the default handler. `BODY` may fall through. `@err(e);` inside `BODY` is ill-formed without a binder.
- `CALL !>(e) BODY` *(statement)* — Same, with the error bound to `e` for the scope of `BODY`. `@err(e);` inside `BODY` forwards to the matching `@errhandler` for `E` (see invariant 5).
- `CALL !>;` *(expression)* — Evaluate `CALL` exactly once. On success, the surrounding expression's value is the unwrapped `T`. On error, a synthesized binder captures the error and the matching `@errhandler` body for `E` is inlined in place of `BODY`; the handler must diverge, so control never returns past the `!>;`. If no matching handler is in scope, the program is ill-formed. If the call occurs inside that matching handler's body (same-`E` re-entry), the program is ill-formed.
- `CALL !> DIVERGENT_STMT;` and `CALL !> { …; DIVERGENT_STMT }` *(expression)* — Evaluate `CALL` exactly once. On success, the surrounding expression's value is the unwrapped `T`. On error, `DIVERGENT_STMT` (or the block) runs; because it cannot fall through, the surrounding expression has no observable value on that path. `!>(e) …` binds the error to `e` across the body.
- `@err(X);` — Inside a `!> (X) BODY` or `!> (X) { BODY }` (statement or expression position). Transfers control to the matching `@errhandler` for the unwrap's Result error type `E` (two-pass rule of invariant 4; binder projected along the chosen path), with the error value forwarded. Does not return.
- `@errhandler(E e) STMT;` / `@errhandler(E e) { BODY }` — Registers a block-local handler for Result error type `E`. `CALL !>;` at statement position, `@err(e);` forwards, and `CALL !>;` at expression position dispatch with the two-pass rule of invariant 4. Once `F` is chosen, the binder is projected along that unique path (member selection; `@typeview` `as:` declaration order applies only here, not to handler selection). When the unwrap's error type cannot be resolved as a Result `E` (pointer-returning calls and other untyped LHS forms), dispatch matches `@errhandler(CCError …)` — the same ambient error type those binders use. Subject to the divergence rule of invariant 6. Stdlib helpers: `cc_error_log(e)` (report, returns) and `cc_error_exit(e)` (report then `exit(1)`).

**Form selection (normative).** `!>` has no closing delimiter — a handler body runs to its terminating `;` — so the token after `!>` decides which form was written. The **bare** form (`CALL !>`, dispatching to the in-scope `@errhandler`) applies wherever that token cannot begin a statement:

- a closer or separator — `;`, `)`, `,`, `]`, `}`, `:`;
- an operator that is only ever infix — `<`, `>`, `=`, `?`, `%`, `/`, `^`, `|`, the compound assignments, and `==` `!=` `<=` `>=` `&&` `||` `<<` `>>`.

Anything else begins a handler body. Prefixes that can open a statement — `*`, `&`, `+`, `-`, `!`, `~`, `(`, an identifier — therefore always read as a body, because `f() !> *p = 0;` is equally a multiplication and a body and the choice cannot be inferred. Parenthesise the unwrap to say which was meant: `(f() !>) * p`. Since `)` is a terminator, that escape is always available.

Consequently the bare form composes: it lowers to a self-contained expression, so it may appear anywhere a value may, including as a call argument, an operand, an array subscript, or one declarator of several.

**Single-evaluation (normative).** Every operator listed above evaluates its LHS call expression exactly once. Lowerings MUST preserve this.

**Lowering (informative).**

- `EXPR ?> DEFAULT_EXPR` lowers to a statement-expression that stores `EXPR` in a temporary and yields `cc_is_ok(tmp) ? cc_value(tmp) : (DEFAULT_EXPR)`.
- `CALL !>(e) BODY` *at statement position* lowers to a block that stores the call in a temporary, tests `cc_is_err`, and on error introduces `e` bound to `cc_error(tmp)` before entering `BODY`.
- `CALL !>(e) BODY` *at expression position* lowers to `({ __typeof__(CALL) tmp = (CALL); if (cc_is_err(tmp)) { __typeof__(cc_error(tmp)) e = cc_error(tmp); BODY } cc_value(tmp); })`. Divergence of `BODY` guarantees control never falls through to the trailing `cc_value(tmp)` on the error path.
- `@err(e);` lowers to a jump into the registered handler body with the error value threaded through.

Implementations are free to vary the exact shape; the normative semantics are above.

**Examples.**

```c
// Default value on error (pure C expression on the RHS).
int x = parse_int(s) ?> 0;

// Default value with the error bound, still a pure C expression.
int x = parse_int(s) ?>(e) fallback_for(e.kind);

// Propagate the error, binding explicitly — uses !> at expression position.
int!>(CCError) propagate(char[:] s) {
    int a = parse_int(s) !>(e) return cc_err(CC_ERROR(e.kind, "prop_a"));
    int b = parse_int(s) !>(e) return cc_err(CC_ERROR(e.kind, "prop_b"));
    return cc_ok(a + b);
}

// Loop exit on error (expression-position !> with divergent body).
for (int i = 0; i < n; i++) {
    int v = maybe(i) !> break;
    total += v;
}

// Block body at expression position: multiple statements on the error path,
// ending in a divergent statement.  The block executes only on error;
// success still yields `int`.
int!>(CCError) parse_and_log(char[:] s) {
    int v = parse_int(s) !>(e) {
        metrics.record(e);
        log("parse failed: %d", (int)e.kind);
        return cc_err(CC_ERROR(e.kind, "parse"));
    };
    return cc_ok(v);
}

// Bare expression-position !>; delegates to the matching @errhandler for E.
int main(void) {
    @errhandler(CCError e) cc_error_exit(e);  // ≡ !>(e) cc_error_exit(e) at each site
    int v = parse_int(s) !>;
    return v;
}

// Stacked handlers: dispatch by Result error type, not textual nearest.
int main(void) {
    @errhandler(CCError e) { log(e); return 1; }
    @errhandler(CCIoError e) { log_io(e); return 2; }
    (void)stdio_println(msg);    // CCError  -> first handler
    (void)command_status(cmd) !>;   // CCIoError -> second handler
    return 0;
}

// Statement-position !>, block body (may fall through).
for (;;) {
    flush() !> { log("flush failed"); break; };
    ...
}

// Statement-position !> with forwarding.
int main(void) {
    @errhandler(CCError e) {
        log(e);
        return 1;
    }
    flush() !> (e) {
        metrics.record(e);
        @err(e);   // divergent: no code after this in the same block
    };
    return 0;
}

// Explicit discard when intentional.
(void)cleanup();
```

**Composition with `name@(args) @destroy`.** `?>` and `!>` participate in deferred cleanup like any other C statement: `@defer` scheduled entries run on scope exit regardless of which branch of `?>` / `!>` fired. Divergent RHS (`return`, `break`, `continue`) respect the scope boundary they cross; the surrounding `@defer` runs as usual.

**Declaration destructor suffix.** An unwrap in a declaration may be followed
by `@destroy { D }` or bodyless `@destroy`:

```
stmt ::= … call '!>' [ '(' ident ')' ] [ body ] '@destroy' '{' D '}' ';'
       | … call '?>' [ '(' ident ')' ] rhs  '@destroy' '{' D '}' ';'
       | … call '!>' [ '(' ident ')' ] [ body ] '@destroy' ';'
       | … call '?>' [ '(' ident ')' ] rhs  '@destroy' ';'
       | type ident '=' expr '@destroy' [ '{' D '}' ] ';'
```

Semantics: on the success path of `EXPR !> BODY @destroy { D }`, `D` is scheduled to run at scope exit in reverse-declaration order, as if a declaration-bound `@defer { D };` followed the declaration. On the error path, the handler body or divergent RHS has already left the scope, so `D` does not run. For `?> … @destroy { D }`, both branches yield a bound value, so `D` runs at scope exit unconditionally.

Any binder introduced by the host statement (e.g., the LHS of a declaration like `T* p = CALL() !> … @destroy { use(p); };`) is in scope inside `D` because `D` expands to a `@defer` in the surrounding block.

For a registered declared type, lowering resolves lifecycle hooks from the
translation unit's type registry. A pointer-typed declaration passes the value;
a value-typed declaration passes its address. Cleanup is a flat call list:

1. registered pre-destroy (if any)
2. call-site `@destroy { D }` body (if any)
3. registered destroy (if any)
4. each value field of the declared type, last declared to first, whose
   field type has a registered pre-destroy or destroy hook, or whose own
   value fields do (transitively): that type's chain on `&name.field`.
   Pointer, array, and function-pointer fields are omitted.

Built-in owner families use the same outer order: nursery wait, user body,
nursery free; arena or channel user body, then destroy/free.

Bodyless `@destroy` is well-formed when that list is non-empty. An empty
list is a compile error at `@destroy`. An explicit body remains valid
without a registered hook and lowers to the declaration-bound deferred
body plus any nested value-field chain.

`recv.destroy()` on a value receiver is ordinary UFCS: `Type_destroy` when
that function exists. Bodyless `@destroy` is the registered hook list
(outer pre-destroy / destroy, then value-field hooks). No full-chain
symbol is synthesized for `@destroy`. Registered destroy hooks are
idempotent.

The same declared-type hook lookup applies to a direct initializer such as
`CCArena a = cc_arena_heap(n) @destroy;`; an unwrap operator is not required
when the initializer is infallible. Pointer declarations pass the pointer value
to a registered hook, while value declarations pass the object's address.

**Nullable pointers.** Both operators also consume pointer-typed operands. For a pointer-typed `EXPR`, the failure condition is `EXPR == NULL` instead of the result's error arm. On failure, a `CCError` is synthesized with `kind == CC_ERR_NULL` and a compile-time-constant message of the form `NULL returned from <call expression> at <file>:<line>`; the binder forms (`!>(e)`, `?>(e)`) bind that synthesized error. Every form and rule above applies unchanged:

- `PTR_EXPR ?> DEFAULT_EXPR` yields the pointer when non-NULL, otherwise `DEFAULT_EXPR` (a pure expression of the pointer's type).
- Statement- and expression-position `!>` behave per invariant 2; bare `!>;` dispatches the synthesized `CCError` to a matching `@errhandler(CCError …)`.
- The `@destroy { D }` success-destructor suffix composes identically.
- Single evaluation holds: the pointer expression is evaluated exactly once.

Absence is not forced into the protocol: a pointer-typed call used as a plain statement or operand remains ordinary C. The unwrap semantics apply only where `?>` / `!>` are written.

```c
int*  p = get_ptr()      !> { log("missing"); return 1; }; // NULL -> body (diverges)
char* s = get_name()     ?> "anonymous";                   // NULL -> default
char* t = get_name()     !>(e) {                           // e.kind == CC_ERR_NULL,
    log(e.message);                                        // message names the call site
    return 1;
};
```

---

### 3.2 Type Precedence

Type modifiers bind with the following precedence (tightest first):

1. `*` (pointer)
2. `!>(E)` (result)
3. `[n]` `[:]` `[~n]` (array / slice / channel)

**Rationale:** Pointer binds tightest because "result of pointer" (`T*!>(E)` → `(T*)!>(E)`) is far more common than "pointer to result" (`(T!>(E))*`). Functions returning pointer-or-error are ubiquitous in systems code. `T!E` without `>` is not a result type.

**Examples:**


| Syntax        | Parses as              | Meaning                                      |
| ------------- | ---------------------- | -------------------------------------------- |
| `int`*        | `(int)`*               | pointer to int                               |
| `int*!>(E)`   | `((int)*)!>(E)`        | result of pointer (success=pointer, error=E) |
| `int!>(E)`    | `(int)!>(E)`           | result of int or E                           |
| `int!>(E)[~]` | `((int)!>(E))[~]`      | channel of results                           |
| `int*!>(E)[~]`| `(((int)*)!>(E))[~]`   | channel of (result of pointer)               |
| `int[:]*`     | `((int)[:])*`          | pointer to slice                             |
| `int*[:]`     | `((int)*)[:]`          | slice of pointers                            |


**Common patterns:**

```c
// Function returning pointer or error — the common case
Node*!>(IoError) find_node(int id);      // (Node*)!>(IoError)

// Function returning a nullable pointer (NULL = not found, no error)
Node* lookup(int id);                    // plain pointer with NULL sentinel

// Channel carrying results
int!>(Error)[~10 >] results_tx;          // sender for channel of results
```

**Note:** For a channel `ch : (T!>(E))[~]`, `ch.recv(&out)` returns `bool !>(CCIoError)`, where `ok(false)` means "closed+drained" and the caller interprets `out` only when `cc_value(got) == true`. If the element type is itself a result (`T!>(E)`), `err(e)` inside `out` is an application-level error value distinct from the channel-level `ok(false)` closed signal.

---

### 3.3 Arrays and Slices


| Type     | Meaning     | Storage                | Size                        |
| -------- | ----------- | ---------------------- | --------------------------- |
| `T[n]`   | fixed array | inline                 | compile-time                |
| `T[:]`   | slice       | ptr + len + provenance | runtime                     |
| `T[n:]`  | slice       | ptr + len + provenance | compile-time length         |
| `T[:!]`  | slice       | ptr + len + provenance | runtime + unique            |
| `T[:k]`  | slice       | ptr + len + provenance | runtime + sentinel          |
| `T[:k!]` | slice       | ptr + len + provenance | runtime + sentinel + unique |


Slices are *views*; they do not own memory.

**Rule (`T[n:]` semantics):** `T[n:]` is a slice type with a compile-time known length `n`. It has the same ABI as `T[:]` (24 bytes on 64-bit), but the type system statically guarantees `len == n`. This enables bounds-checked indexing to elide runtime checks. `T[n:]` implicitly converts to `T[:]` (information is erased, not lost).

**Rule (`!` marker semantics):** The `!` suffix on a slice type is a **type-level uniqueness guarantee**. A value typed `T[:!]` or `T[:k!]` is statically required to carry `id.is_unique=1` at the ABI level (§3.4) — i.e., the compiler rejects any assignment, copy, or parameter pass that would duplicate it outside a move context (`cc_move()`, `return`, `send_take`). Use `T[:!]` in function signatures to **demand** that callers hand over ownership. `T[:]` by contrast says nothing about uniqueness at the type level — the value may or may not be unique; the compiler relies on the runtime `is_unique` bit to enforce copy rules at the call site.

**Rule (`T[:k]` / `T[:k!]` semantics):** Sentinel slices are ABI-identical to `T[:]`. The sentinel value `k` is a type-level guarantee about the element just past the logical end of the view (typically `k = 0` to guarantee NUL-termination for C interop). On `T[:0]`, `len` is the payload and index `len` is a valid load of `0`. Applying `!` composes the two guarantees: `T[:k!]` demands both the sentinel and type-level uniqueness. The `is_cstr` id bit is the value-level twin of `[:0]`: it survives erase to `T[:]` so `to_c` / `to_cstr` copy only when the terminator is not already there.

**Implicit conversions:**

- `T[n]` → `T[:]`
- `T[n]` → `T[n:]`
- `T[n:]` → `T[:]`
- `T[:!]` → `T[:]` *(move context; drops type-level uniqueness, value's `is_unique` bit persists)*
- `T[:k]` → `T[:]`
- `T[:k!]` → `T[:k]` *(move context; drops type-level uniqueness, keeps sentinel)*
- `T[:k!]` → `T[:!]` *(drops sentinel, keeps type-level uniqueness)*
- `T[:k!]` → `T[:]` *(move context; drops both)*

**Explicit conversions:**

- `slice.ptr` is `char *` on `CCSlice` / `char[:]`; typed `T[:]` uses `.base.ptr`
- `slice.id` yields the provenance token
- `slice.to_c(arena)` yields `char[:0]` (`is_cstr`); copies into `arena` only when the bit is clear
- `slice.to_cstr(arena)` yields `char *` — `to_c(arena).ptr`

---

### 3.4 Slice ABI (24 bytes on 64-bit)

All slices lower to the following ABI:

```c
struct Slice_T {
    T*       ptr;      // 8 bytes: element pointer
    size_t   len;      // 8 bytes: element count
    uint64_t id;       // 8 bytes: allocation ID with flags (see below)
};
```

Backing-store capacity is not part of the slice ABI. Owners that need
remaining room (strings, vecs, arenas) keep capacity on the owner.

**ID field encoding:**

```
// Slice ID bit layout (uint64_t id):
//
// Bits 0–59 : allocation ID (opaque, non-zero for tracked allocations)
//   Grower-minted views (Vec / heap String as_slice) pack this field as:
//   Bits 0–31 : arena epoch
//   Bits 32–58 : generation (the owner's token)
//   Bit 59     : grower. Canonical static id has bits 0–59 all 1 and is not a grower.
// Bit 60    : is_cstr (`ptr[len]` is defined and 0)
// Bit 61    : is_transferable
// Bit 62    : is_subslice
// Bit 63    : is_unique
```

- **Bits 0–59 (allocation ID):** Unique per tracked allocation. 0 indicates no epoch (untracked, or static with only flags). A grower-minted view carries its owner's token; a leftover view after a moving Vec / heap String regrow is stale (the owner was reborn with a fresh token): Result `at` / `set` is `CC_ERR_INVALID_ARG`. In-place growth keeps the token. Tokens are issued by the generation registry in `[16, 2^27)`; when it cannot issue one (every token live) the owner's birth fails at that position. `id == 0` does not join.
- **Bit 60 — `is_cstr`:** 1 if `ptr[len]` is a defined `0` (C-string capability). Set on `CC_SLICE_LIT` / `from_static` / `cc_slice_cstr`. Cleared on `from_buffer` and brace inits. `sub` recomputes it (keep when the new end is the old end and the parent had it; otherwise set only when `ptr[end] == 0` is an in-payload load). Untracked is `(alloc == 0 && !unique)` — the cstr bit alone is not lifetime.
- **Bit 61 — `is_transferable`:** 1 if the allocation may be transferred across threads via `send_take`; 0 otherwise.
- **Bit 62 — `is_subslice`:** 1 if the slice does not cover the full allocation.
- **Bit 63 — `is_unique`:** 1 if the slice has destructor semantics and is move-only.

**Special ID values:**


| Condition                                           | Meaning                                                                                                           |
| --------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| `id == 0`                                           | Untracked slice (`from_buffer`, foreign). Not unique, not transferable, no cstr bit. |
| canonical alloc + `is_cstr`                         | Static / `CC_SLICE_LIT` / `from_static`. Channel-stable. `to_c` does not copy. |
| `id != 0 && is_unique == 0`                         | View slice (arena, stack). Copyable. Not transferable.                                                            |
| `id != 0 && is_unique == 1 && is_transferable == 1` | Unique, transferable slice (from `recv()`).                                                                       |
| `id != 0 && is_unique == 1 && is_transferable == 0` | Unique, non-transferable slice (from `adopt()`).                                                                  |


**Runtime table (debug builds):** Keyed by `id & 0x1FFFFFFFFFFFFFFF` (lower 61 bits), stores:

```c
struct AllocationRecord {
    void*    base_ptr;          // allocation start
    size_t   alen;              // allocation length
    uint64_t gen;               // generation (for staleness)
    void (*deleter)(void*);     // NULL unless from adopt()
};
```

**Rule (staleness detection):**

- Debug builds maintain the allocation table and trap on access to freed/reset allocations
- Release builds skip tracking; stale access is undefined behavior

### 3.5 Slice Ownership: Views vs Unique

Slices are either **views** (copyable, no destructor) or **unique** (move-only, has destructor).


| Category           | Source               | Copyable? | Transferable? | Has destructor? |
| ------------------ | -------------------- | --------- | ------------- | --------------- |
| **View**           | Arena, stack, static | Yes       | No            | No              |
| **Unique (recv)**  | `recv()`             | No        | **Yes**       | Yes             |
| **Unique (adopt)** | `adopt()`            | No        | No            | Yes             |


*Stack slices cannot escape their stack frame (compile-time enforced).

**Type-level vs value-level uniqueness:** Uniqueness is always carried on the value via the `id.is_unique` ABI bit (§3.4). A source declaration can **additionally** assert it at the type level with the `!` marker (`T[:!]`, `T[:k!]`), which moves the check from "when the compiler sees a value" to "when the compiler sees the signature." Use `T[:!]` / `T[:k!]` in function parameters and return types when the contract is ownership transfer; use plain `T[:]` / `T[:k]` when the callee only borrows and doesn't care about uniqueness. See §3.3 for the conversion lattice between type-level forms.

**View slices:**

- Created by arena allocation, local arrays, or string literals
- Copyable — multiple variables can reference the same allocation
- Arena/static views can cross thread boundaries; stack views cannot escape their frame
- No destructor — the arena (or program lifetime, or stack frame) manages the memory

**Unique slices:**

- Created by `recv()` (channel receive) or `adopt()` (FFI adoption)
- Move-only — copying is a compile-time error
- Has destructor that runs at scope exit (or is suppressed if moved)
- Only `recv()` slices can use `send_take` for zero-copy transfer (see §7.4)

**Rule (unique assignment ban):** Any assignment, copy, or parameter passing that would duplicate a slice value with `id.is_unique=1` is illegal unless it occurs in a move context (`move()`, `return`, `send_take`).

**Borrowed views:**

Subslicing a unique slice produces a **borrowed view**. At runtime, borrows are still `T[:]` values, but the compiler tracks an implicit owner reference.


| Property       | Borrowed view              |
| -------------- | -------------------------- |
| Runtime type   | `T[:]` (same as any slice) |
| `id.is_unique` | 0 (view, not unique)       |
| Copyable?      | Yes (within borrow region) |
| Sendable?      | Same as owner              |
| Lifetime       | Must not outlive owner     |


**Borrow rules:**

- Borrows are created by subslicing: `(*x)[a..b]`, `(*x)[..n]`, etc.
- Borrows can be freely copied within their valid region
- Borrows are invalidated when the owner is:
  - Moved (`move(x)`, `send_take(*x)`)
  - Destroyed (scope exit)
  - Reset (for arena-backed owners, `cc_arena_reset`)
- Using an invalidated borrow is a compile-time error (when detectable) or undefined behavior (when not statically detectable, debug builds trap)

```c
char[:] x;
bool !>(CCIoError) got = @await ch.recv(&x);   // x owns unique slice on ok(true)
// Assume got is ok(true) here.
char[:] borrow = x[0..10];                    // borrow is a view into x
use(borrow);                                  // OK
char[:] y = cc_move(x);                       // x moved to y
use(borrow);                                  // ERROR: borrow invalidated by move
```

**Rule (unique destruction):**

- Unique slices have an implicit destructor at scope exit
- Destructor runs on all exits: return, early return, `!>` propagation
- Destructor is suppressed if ownership is moved (`return`, `send_take`, `move()`)
- For `adopt()` slices: destructor calls the registered deleter
- For `recv()` slices: destructor frees the channel's buffer

**Slice Derivation Rules (Normative):**

When a slice value is derived from another slice (subslicing):


| Field                          | Behavior                                                                                       |
| ------------------------------ | ---------------------------------------------------------------------------------------------- |
| **Allocation ID (bits 0–60)**  | Preserved exactly                                                                              |
| `**is_unique` (bit 63)**       | Cleared (0) — derived slices are borrowed views, only the original owning slice remains unique |
| `**is_transferable` (bit 61)** | Preserved exactly — but borrowed views are never transferable because `is_unique == 0`         |
| `**is_subslice` (bit 62)**     | Set to 1 iff the derived slice is a proper subrange, or a full-range view of an existing subslice |


`**is_subslice` computation:**

- Full-range of a non-subslice (`start == 0 && end == s.len && !is_subslice(s)`) → `is_subslice = 0`
- Proper subrange (`start != 0` or `end != s.len`) → `is_subslice = 1`
- Full-range of an existing subslice → `is_subslice = 1`

This guarantees that only the owning unique non-subslice remains eligible for transfer, while derived views are rejected — with no runtime table required.

**Rule (ptr invariant):** For tracked allocations (unique slices from `recv()` or `adopt()`), the `ptr` field of the owning unique slice is always the allocation base pointer. Subslicing adjusts `ptr` and `len` and sets `is_subslice`. Combined with uniqueness and transferability flags, `send_take` eligibility is decidable without runtime lookup: unique + transferable + `!is_subslice`.

**Slice capture rules:** Stack slices cannot be captured in thread/task closures (compile-time enforced). Arena slices can be captured if the arena provably outlives the thread/task. Unique slices (from `recv()`) can always be captured. See §2.2 for complete capture rules.

---

## 4. Type Categories and Scope-Bound Values

This section defines categories of types that have special restrictions:

- **§4.1 Scope-bound values (`@scoped`)** — types tied to lexical scope
- **§4.2 Suspension points** — where scope-bound values cannot be held
- **§4.3 Compiler enforcement** — how violations are detected

The central rule: **No scope-bound value may be held across a suspension point.**

---

### 4.1 Scope-Bound Types (`@scoped`)

A type marked `@scoped` is **tied to a lexical scope** and cannot outlive that scope. Most importantly, a scope-bound value cannot be held across a suspension point (`@await`, `cc_block_on` / `cc_block_on_intptr`). Constructing a `CCTaskIntptr` from an `@async` call is not a suspension point (§8.3).

Types registered as scope-bound cannot cross the boundaries below.

**Characteristics:**

- Cannot be returned from a function
- Cannot be stored in a non-scoped struct field
- Cannot be passed into an `@async` function or held across `@await`
- Must be released before any suspension point
- Compiler enforces these restrictions at compile time

**Rule (scope-bound cannot escape):**

```c
@scoped struct ScopeToken { int value; };

struct Container {
    ScopeToken token;  // ERROR: scope-bound value in ordinary struct
};
```

**Rule (Suspension point releases @scoped):**

If a function has `@scoped` values in scope at a suspension point, that's a compile error. The typical fix is to release the value before suspending:

The value's lexical scope must end before `@await`.

---

### 4.2 Suspension Points

#### 4.2.1 Definition

A **suspension point** is any program point at which execution of the current task may suspend and later resume. At suspension points, **no scope-bound (`@scoped`) values may be held**.

**Suspension points:**

- `@await` expression (any `@await`)
- A suspending `send`, `send_take`, or `recv` in an `@async` body (these require `@await`)
- Call to `cc_block_on` / `cc_block_on_intptr` (explicit blocking)

An `@async` call constructs a `CCTaskIntptr` (§8.3). The parent does not suspend at that construction. Polling the task, `@await` of it, or `cc_block_on_intptr` is what suspends.

**Non-suspension points** (safe to hold scope-bound values):

- Call to a sync function
- An `@async` call used only to construct a `CCTaskIntptr` (no `@await` / poll / `cc_block_on`)
- Local variable creation / destruction
- Arithmetic, logic, control flow
- Non-blocking operations (`try_send`, `try_recv`, `close`)

#### 4.2.2 Cancellation and Deadline Observation

Suspension alone does not impose one cancellation policy. Observation depends
on the source and operation:

- Nursery cancellation is observed by `cc_cancelled()` /
  `n.is_cancelled()` and by runtime waits that explicitly use the
  nursery-aware cancel path.
- A `CCDeadline` is observed by operations passed that deadline or by
  operations that consult `cc_current_deadline()`. Expiry produces that
  operation's timeout result, normally `ETIMEDOUT`.
- `cc_cancel(CCDeadline*)` marks that deadline object; it does not cancel an
  unrelated nursery.
- Plain operations that do not consult either source do not acquire
  cancellation behavior merely because they suspend.

No source forcibly interrupts running C code. Each operation defines whether it
checks before parking, after waking, or both, and which in-band result it
returns. §8.5 lists the public sources and observation points.

**Rule (no cleanup at park):** `@defer` / `@destroy` hooks do not run at a
suspension point. A parked fiber still holds its resources; destroying them at
park would tear state a resumed frame still needs.

**Rule (entered vs never-entered):** External destruction of a closure
environment may run capture cleanup only when the closure body has never been
entered. Once a frame has entered, only that frame’s own exit path may
discharge its cleanup ledger — including cancelled-resume, where park returns
an error, the frame takes normal exits, and LIFO hooks run in the fiber. A
canceller must not invoke the closure’s `drop` path for an in-flight or parked
frame. (Fixture: park holding `@destroy` resources, cancel, assert hooks ran
in the fiber in LIFO order — not at the canceller.)

**@scoped Placement Rules (Normative):**

`@scoped` can appear in the following positions:

1. **Type declarations (primary use):**

```c
@scoped struct ScopeToken {
    int handle;
};
```

1. **Function parameters:**

```c
fn handle(@scoped RequestContext ctx) {
    use(ctx);
}
```

`@scoped` on a parameter means the value must be released before any suspension point.

**Invalid positions (compile error):**

- **Return types:** Cannot return @scoped values

Returning a scope-bound type is a compile-time error.

- **Variable declarations:** Variables are already scope-bound to their block; `@scoped` is implicit

Applying `@scoped` redundantly to a local is ill-formed.

- **Struct fields (in non-scoped struct):**

```c
struct Container {
    ScopeToken token;  // ERROR: scope-bound field in ordinary struct
};
```

- **Function pointers/closures:**

```c
@scoped fn ptr = &my_func;  // ❌ ERROR: meaningless on function types
```

**Interaction with `@async`:** A scope-bound parameter cannot remain live
across a suspension point. This ownership rule is independent of cancellation
and deadline observation.

---

### 4.3 Compiler Enforcement

The compiler checks scope-bound restrictions in several places:

**At suspension points:**

```c
@async void handler() {
    ScopeToken token = acquire_scope_token();
    @await io();  // ERROR: scope-bound token held across suspension
}
```

**At function boundaries:**

```c
void takes_token(ScopeToken token);
```

**At struct field assignment:**

```c
struct Holder {
    ScopeToken token;  // ERROR: cannot store scope-bound value
};
```

**Compiler error message example:**

```
error: scope-bound value `token` held across suspension point
  --> file.c:10:5
   |
9  |     ScopeToken token = acquire_scope_token();
   |                ----- token declared here
10 |         @await io_operation();
   |         ^^^^^ suspension point: cannot hold @scoped value
   |
help: release @scoped value before suspension
```

---

## 5. Arenas

This section defines the allocation model and lifetime boundaries:

- **§5.0 API** through **Usage** — constructors, alloc paths, overflow, concurrency
- **§5.1 `@defer`** — scoped cleanup
- **§5.2 Scoped arena lifetimes** — ordinary lexical scopes with `@destroy`

An arena names a lifetime. Its allocation strategy is an implementation
policy for storage belonging to that lifetime. Slices are views into that
storage. Size the root for the lifetime's typical live set. Heap overflow and
mid-lifetime `cc_arena_release` are escape hatches, not the steady-state path.
Prefer a separate arena when lifetimes diverge rather than long-lived release
churn.

When the slab/`block_max` budget cannot satisfy an allocation and heap overflow
is disabled (or fails), allocation returns `NULL`. Exhaustion is never
indistinguishable from success.

---

### 5.0 API

```c
// Constructors
CCArena cc_arena_heap(size_t bytes);     // heap root; block_max = 4; overflow on
CCArena cc_arena_create(size_t bytes);   // alias of cc_arena_heap
CCArena cc_arena_malloc(size_t bytes);   // durable: block_max = 1; overflow on; no extents
int cc_arena_buffer(CCArena* a, void* buf, size_t cap);  // user root; block_max = 1; overflow off
CCArena cc_arena_create_buffer(void* buf, size_t cap, unsigned block_max);
CCArena cc_arena_fixed_buffer(void* buf, size_t cap);    // create_buffer(..., CC_ARENA_FIXED)
#define cc_arena_stack(name, nbytes)     // stack root; block_max = 4; overflow on
#define CC_ARENA_STACK(name, nbytes)     // alias of cc_arena_stack
#define cc_arena_buf(name, ptr, nbytes)  // caller L1; same sugar as stack (no VLA)
#define CC_ARENA_BUF(name, ptr, nbytes)  // alias of cc_arena_buf
#define CC_ARENA_FIXED     1u            // root only
#define CC_ARENA_GROWABLE  0u            // unbounded extents (expert)
#ifndef CC_ARENA_DEFAULT_BLOCK_MAX
#define CC_ARENA_DEFAULT_BLOCK_MAX 4u
#endif
bool cc_arena_set_heap_overflow(CCArena a, bool enabled);

// Lifecycle (`*` is the binding slot — the only justified star)
void cc_arena_free(CCArena* a);          // drain ovf; free heap extents/root; clear handle
void cc_arena_reset(CCArena* a);         // drain ovf; unwind extents; restore original root
void cc_arena_destroy(CCArena* a);       // alias for cc_arena_free
CCArena cc_arena_detach(CCArena* a);     // heap-owned L1 only; empty on refuse

// Checkpoints (an active child arena: capture carves it, restore destroys it)
typedef struct CCArenaCheckpoint CCArenaCheckpoint;   // { child, parent, offset }
CCArenaCheckpoint cc_arena_checkpoint(CCArena a);           // C twin (@scratch); .arena == NULL: unarmed
bool cc_arena_restore(CCArenaCheckpoint checkpoint);         // C twin; false: refuse
CCArenaCheckpoint !>(CCError) cc_arena_try_checkpoint(CCArena a);
void !>(CCError) cc_arena_try_restore(CCArenaCheckpoint checkpoint);
size_t cc_arena_live(CCArena a);  // L1 + L2 + Main
CCArenaTier cc_arena_ptr_tier(CCArena a, const void* ptr);

// Shared alloc (thread-safe tip CAS + meta_lock on grow/ovf/chain)
void* cc_arena_alloc(CCArena a, size_t nbytes, size_t align);
void* cc_arena_realloc(CCArena old_a, CCArena new_a, void* p,
                       size_t old_n, size_t new_n, size_t align);
bool cc_arena_release(CCArena a, void* ptr);                 // unsized: hole, or last-live rewind
bool cc_arena_release_sized(CCArena a, void* ptr, size_t n);  // sized: tip pop / class list / hole
bool cc_arena_set_reuse(CCArena a, bool enabled);            // size-class lists for sized releases

// Owners (header in the slab tier, payload from the strategy, token-checked)
CCArenaOwner* cc_arena_owner_new(CCArena a, size_t bytes, size_t align);
bool  cc_arena_owner_live(const CCArenaOwner* o, uint32_t token);
void* cc_arena_owner_regrow(CCArenaOwner* o, uint32_t token, size_t bytes); // move rebirths the token
bool  cc_arena_owner_release(CCArenaOwner* o, uint32_t token);           // stale token: refused
uint64_t cc_arena_owner_slice_id(const CCArenaOwner* o);                 // grower view id

// Local alloc (exclusive owner only — UB if shared)
void* cc_arena_alloc_local(CCArena a, size_t nbytes, size_t align);       // current slab; NULL if full
void* cc_arena_alloc_local_grow(CCArena a, size_t nbytes, size_t align);  // local tip, unlocked grow, then ovf
void* cc_arena_realloc_local(CCArena a, void* p, size_t old_n, size_t new_n, size_t align);       // tip fit only
void* cc_arena_realloc_local_grow(CCArena a, void* p, size_t old_n, size_t new_n, size_t align);  // tip, else local grow/copy

#define cc_arena_alloc_T(T, arena)                 // shared default; UFCS: a.allocT()
#define cc_arena_alloc_T_count(T, arena, count)    // UFCS: a.allocT(n)
#define cc_arena_alloc_T_local(T, arena)
#define cc_arena_alloc_T_count_local(T, arena, count)
#define cc_arena_alloc_T_local_grow(T, arena)
#define cc_arena_alloc_T_count_local_grow(T, arena, count)

// Tracked slices (empty slice on failure; len==0 is also empty)
CCSlice cc_arena_alloc_slice_bytes(CCArena a, size_t len);  // UFCS: a.alloc_slice_bytes(n)
CCSlice cc_arena_alloc_slice(CCArena a, size_t elem_size, size_t count, size_t align);
CCSlice cc_arena_slice(CCArena a, void* ptr, size_t len);  // UFCS: a.slice(ptr, len)

int cc_arena_would_fit(CCArena a, size_t nbytes, size_t align);  // current slab only
size_t kilobytes(size_t n);
size_t megabytes(size_t n);
```

**Pool (normative):** `CCArenaPool` is an O(1) freelist of uniform objects over an
arena. Pool bump fills use shared `cc_arena_alloc`.

```c
typedef struct CCArenaPool CCArenaPool;
void cc_arena_pool_init(CCArenaPool* p, CCArena a, size_t sz);
int cc_arena_pool(CCArenaPool* p, size_t sz);       // owns its own arena
void* cc_arena_pool_alloc(CCArenaPool* p);          // UFCS: p.alloc()
void cc_arena_pool_free(CCArenaPool* p, void* ptr); // UFCS: p.free(ptr)
CCArena cc_arena_pool_detach(CCArenaPool* p);       // UFCS: p.detach_arena()
void cc_arena_pool_destroy(CCArenaPool* p);         // UFCS: p.destroy()
#define cc_arena_pool_stack(name, elem_size, nbytes)
#define CC_ARENA_POOL_STACK(name, elem_size, nbytes)  // alias
```

The pool is reclaimed when the underlying arena is reset or freed. If the pool
owns its arena (`cc_arena_pool`), `cc_arena_pool_destroy` frees that arena.
`cc_arena_pool_stack` / `CC_ARENA_POOL_STACK` expands to `cc_arena_stack` plus
`cc_arena_pool_init`.

---

### Model

**Root sizing.** An arena is a named lifetime. Storage is three tiers: **L1**
(the original root slab), **L2** (grown heap extents), **Main** (overflow).
Constructor `bytes` is L1 capacity. Size it for the typical live set of that
lifetime so traffic stays in slabs. `cc_arena_heap` / `cc_arena_stack` /
`cc_arena_buf` share one engine: L1 exactly `N`, `block_max =
CC_ARENA_DEFAULT_BLOCK_MAX` (4), Main overflow on after the slab budget. `cc_arena_malloc` is a durable fixed L1
(`block_max = 1`, overflow on, no L2) for stores that free entries
individually — not for scratch alloc storms. `cc_arena_buffer` /
`cc_arena_fixed_buffer` take a caller-owned L1 with overflow off by default;
enable overflow or raise `block_max` explicitly. `cc_arena_create_buffer`
overlays the host at the first `_Alignof(CCArenaHost)` address inside the
caller region (pad 0..align−1) and sets `block_max`. A non-empty region
that cannot hold that host plus one L1 byte is a fatal overlay error, not
a dead handle. Null or zero-size is a dead handle. Two-arg `@create(buf, cap)`
is `attach_buffer`: a frame host, the whole buffer as L1, no overlay.
`cc_arena_create` aliases `cc_arena_heap`.
`cc_arena_live` counts live objects on L1 + L2 + Main.

**`block_max`.** Affects future growth only.

- `0` (`CC_ARENA_GROWABLE`): unbounded extent chain (expert).
- `1` (`CC_ARENA_FIXED`): no extent growth. Default for `cc_arena_buffer`.
  `cc_arena_malloc` uses this with overflow on.
- `N > 1`: at most `N` slabs total (root is `block_idx == 0`). Default heap/stack
  budget is 4. Beyond the budget, allocation uses heap overflow when enabled;
  otherwise returns `NULL`.

When the active slab is full and growth is allowed, a new slab is installed of
size at least **max**(1.5× previous capacity, space for the pending allocation,
4096). The prior slab is pushed onto the extent chain; the root handle always
holds the active slab.

**Ownership.**

- Heap-rooted (`cc_arena_heap` / `cc_arena_malloc`): arena owns the root buffer;
  `cc_arena_free` frees it with heap extents and overflow.
- User/stack root (`cc_arena_buffer`, `cc_arena_stack`, `cc_arena_buf`): arena
  never frees the initial buffer. `cc_arena_free` frees heap extents and
  overflow only, then clears the handle (`base == NULL`). Re-init with
  `cc_arena_buffer` before reuse. In Concurrent-C, `cc_arena_stack` and
  `cc_arena_buf` attach `@destroy` so L2 and Main are freed at scope exit.
- Freeing never calls `free` on stack or static storage.

```c
CCArena a = cc_arena_heap(kilobytes(64)) @destroy;
int* xs = cc_arena_alloc_T_count(int, a, 100);

cc_arena_stack(scratch, 4096);
void* p = scratch.alloc(n, align);
scratch.reset();  // drain ovf, restore stack root

uint8_t frame[CC_ARENA_REGION_BYTES(4096)];
cc_arena_buf(win, frame, sizeof frame);  // overlay; host may sit after a pad
```

---

### Allocation paths

**Shared** (`cc_arena_alloc`, `cc_arena_realloc`, typed/slice helpers, vec/string/
containers, pool bump): tip bump is lock-free CAS on `offset`. Slab grow, overflow
list/chunk mutation, extent-chain walks, and live-count credit after a tip race
take the per-arena `meta_lock`. Stdlib defaults stay on this path.

**Local** (`*_local*`, `*_local_grow`): plain loads/stores on the active slab.
Require exclusive ownership by one thread/fiber for the duration of those calls;
sharing is undefined behavior. `cc_arena_alloc_local` / `cc_arena_realloc_local`
touch the current slab only (`NULL` when full / not a tip fit).
`cc_arena_alloc_local_grow` tries local tip, then unlocked slab grow, then
overflow — it does **not** bounce through `cc_arena_alloc`.
`cc_arena_realloc_local_grow` tries local tip realloc, else local grow + copy +
release. Opt into local paths only where exclusive ownership is assured.

**Tip realloc.** When `ptr + old_size` is the active tip and the new size fits
the active slab (or overflow chunk tip), realloc grows/shrinks in place. Otherwise
allocate at the new tip, copy the shared prefix, and `cc_arena_release` the old
pointer (or, for per-object overflow in the same arena, `realloc` the malloc
block). Cross-arena realloc allocates in `new_a`, copies, releases through `old_a`.

**`cc_arena_would_fit`:** reports whether the **current** slab can satisfy the
request without growing.

---

### Overflow and release

Heap overflow is the fallback after the slab/`block_max` budget (or when
`block_max == 1` so no extents exist). Toggle with `cc_arena_set_heap_overflow`
(fails to disable after overflow has been used). Overflow pointers stay
arena-owned; `cc_arena_reset` / `cc_arena_free` drain them. Using a pre-reset
overflow pointer is undefined behavior, same as a pre-reset slab pointer.

- **Growable / scratch** (`block_max != 1`): chunked overflow — bump inside
  64KiB (or larger) chunks on `ovf_chunks`. `cc_arena_release` punches a hole;
  chunks free on reset/free.
- **`cc_arena_malloc` / `block_max == 1`:** per-object overflow — each object is
  a separate malloc, linked on a doubly-linked `ovf_head` list.
  `cc_arena_release` unlinks and frees immediately.

**Release is a signal.** A container always releases what it owns, with the
size when it knows it (`cc_arena_release_sized`); the strategy decides what
the bytes become. In the slab tier a sized release of the active tip pops the
tip; on a reuse host (`cc_arena_set_reuse`) a sized release of a classed
block lists it for a later request of the same class (the block stays counted
live while listed); any other slab release is a hole that stays until reset.
Releasing the last live allocation on the root slab rewinds the tip to zero.
An unsized release can only be a hole or, if last live, a rewind. Per-object
overflow frees at once; chunk overflow punches a `DEAD` hole. A hole never
disables anything: checkpoints, children, and detach are unaffected. A pointer
the arena does not own (foreign, beyond the tip, already released and popped,
a size that cannot fit) is refused (`false`) and nothing is counted. The bump
tier carries no per-object header, so a second hole release of the same
block is indistinguishable from the first; owners guard that case with their
token (below).

**Rule:** `cc_arena_reset` runs attached destroy records, newest first (see
`draft_lifetime_parents.md`) — an active checkpoint child among them — then
frees outstanding overflow, unwinds extents, restores the original root
(`block_idx = 0`), clears the used-overflow and reuse flags, drops the owner
and class lists (they lived in the rewound slab), and advances provenance.
The arena stays live and may attach again.

**Rule:** `cc_arena_free` runs attached destroy records, newest first (see
`draft_lifetime_parents.md`), then steals and frees overflow, frees heap-owned
extent and root buffers, then clears the handle.

---

### Checkpoints

A checkpoint is an **active child arena**. `cc_arena_checkpoint(a)` carves a
child host on the remaining L1 tail of the innermost active host of `a`
(record node, host, L1 to the end of the slab), attaches it as a lifetime
child, and marks it active; the parent's tip parks at the slab end and the
carved region counts as one live allocation of the parent. When the tail is
too small the child is heap-rooted (a `CC_ARENA_CHILD_DEFAULT_BYTES` region);
a hard-capped host (`CC_ARENA_FIXED` with overflow off) with no tail returns
an unarmed handle (`checkpoint.arena == NULL`) and allocations keep landing
in the parent. The child inherits the parent's overflow permission and its
hard cap; otherwise it uses the default slab budget.

While the child is active:

- A fresh allocation through the parent handle (`cc_arena_alloc`, slices,
  owners, the local paths) lands in the innermost active child and carries
  that child's provenance epoch.
- `cc_arena_realloc(a, a, p, …)` and `cc_arena_release(a, p)` resolve the host
  on the active chain that owns `p` and act there. A pre-checkpoint owner
  therefore regrows in pre-checkpoint storage: when its host's tip is the
  child's region, the regrow takes a fresh extent of that host. A cross-arena
  realloc names its destination explicitly and moves into the destination's
  innermost active child.
- `cc_arena_remaining`, `cc_arena_would_fit`, and `cc_slice_is_from_arena_epoch`
  consult the active chain.
- A checkpoint taken through `a` nests inside the active child.
- `cc_arena_detach` and `cc_arena_adopt` refuse a host with an active child
  and refuse an active child itself.

Restore destroys the child: its extents and overflow are freed, its epoch is
dead (views minted in it fail the epoch check), and the parent's tip pops
back to the offset the child began at (a last-live pop rewinds to zero). The
next allocation through the parent lands at that offset. Holes in the child
never touch the parent; holes in the parent never touch the child.

Restore is LIFO for **armed** handles: it refuses while an inner checkpoint
that is still named by a live handle is active. `cp.abandon()` consumes the
handle and keeps the scratch — the child stays active (later allocations
keep landing in it) and dies with its parent, and an outer restore tears it
down as an ordinary child. Scope `@destroy` restores, else abandons. A
dropped handle leaves the child active until the parent resets or frees.
`cc_arena_restore` returns false and does not mutate on an unarmed or consumed
handle, a dead parent, a child that is no longer the parent's record (after a
restore, reset, or free), or an armed inner checkpoint. The child is verified
through the parent's record list before it is touched, so a stale handle is
never dereferenced. `cc_arena_try_checkpoint` / `cc_arena_try_restore` are the
Result surface; `cc_arena_checkpoint` / `cc_arena_restore` are C twins for
`@scratch` and existing C. Activation is a single-owner act on a shared
arena. Checkpoints do not change ownership rules.

```c
CCArena a = cc_arena_heap(megabytes(1)) @destroy;
CCArenaCheckpoint cp = a.try_checkpoint() !> @destroy;
char* tmp = cc_arena_alloc_T_count(char, a, 1024);
cp.try_restore() !>;  // reclaim post-checkpoint bytes
```

---

### Concurrency

- Shared alloc: tip CAS is lock-free; `meta_lock` serializes grow, overflow
  lists/chunks, chain walks, and live credit. `cc_arena_release`, overflow
  realloc, `cc_arena_reset`, and `cc_arena_free` take the same lock.
- Local paths require single-owner exclusive use — undefined behavior if shared.
- Reset/free still require no concurrent users and no live derived pointers
  (non-goal: no automatic generation / refcount). Arenas are not refcounted.

**Rule:** Arena-allocated slices may be sent on channels or captured in task
closures when §2.2 lifetime rules hold. Capturing or sending a non-unique
arena-backed view pins the provenance epoch until the join scope ends;
epoch-ending ops while a pin is live are ill-formed when statically visible.

**Rule (arena lifetime obligation):** `cc_arena_reset` / `cc_arena_free` /
`cc_arena_restore` must not run while any derived slice may still be used.
Statically visible conflicts (live lexical borrow, nursery capture) are
compile errors. Otherwise, violation is undefined behavior in release builds;
debug builds may trap via epoch helpers such as `cc_slice_is_from_arena_epoch`.
That helper is an epoch-range check on a process-wide counter, not an
arena-identity test; the caller already holds the slice/arena pairing.

```c
CCArena a = cc_arena_heap(megabytes(1));
char[:] s = a.alloc_slice_bytes(100);
CCNursery n = cc_nursery_create() !> @destroy;

n.spawn(() => {
    use(s);  // OK only while a outlives the join
});

a.free();  // BUG if the task may still use s
```

---

### Usage

- Size the root for the lifetime; treat overflow/release as escape, not policy.
- Split divergent lifetimes across arenas instead of long-lived release churn.
- Request/window scratch: `cc_arena_heap` / `cc_arena_stack` / `cc_arena_buf`
  with an appropriately sized root. Durable entry store with individual free:
  `cc_arena_malloc`.
- Shared path by default (stdlib, any shared arena). Exclusive request/fiber
  arenas may use `*_local_grow` for tip + grow + overflow without tip CAS.
- Fixed user buffer with a hard cap: `cc_arena_buffer` / `cc_arena_fixed_buffer`
  (overflow off). Expert unbounded extents: `block_max = 0` or
  `cc_arena_create_buffer(..., CC_ARENA_GROWABLE)`.

**Non-goal:** Arenas do not provide automatic deallocation or generational
lifetimes. Callers coordinate reset/free with thread and borrow lifetime.

---

### 5.1 `@defer`

`@defer stmt;` schedules `stmt` to run on scope exit.

- Runs on all returns, including `!>` propagation.
- LIFO order.
- No exceptions or unwinding.

**Soft-return and registration watermark.** A `return` inside a function that
has function-scope `@defer` / `@destroy` (or that must unwind nested block/loop
life, or open call-local `@scratch` checkpoints) lowers to a soft-return:
optional scratch restores, optional nested scope life, then `goto` the function
cleanup epilogue. Each function-scope cleanup site stamps a high-water mark at
its declaration; the epilogue runs site `i` only when the mark shows that site
was reached. Block/loop scopes use an analogous per-scope mark so break /
continue / soft-return do not name unreached locals. A braceless
`if` / `else` whose statement is `@defer` registers on the enclosing block
with a per-site reach flag (a later taken `@defer` must not enable a skipped
earlier arm). The `@defer` lowers to a C statement so the controlling `if`
does not attach to the next line. A loop body's `@defer` stays on that loop
scope.

**Conformance.** In any function with registered function-scope cleanup, every
`return` lowers to the soft-goto form that reaches the cleanup epilogue. Every
cleanup statement in that epilogue sits under its registration-threshold guard.
Implementations that emit a bare `return` or an unguarded hook for such a
function are non-conforming.

**Note (vs `@errhandler`):** `@defer` / `@defer(err)` schedule work at **scope exit** (or only when the function returns error/success). `**@errhandler`** defines behavior when a `call() !>;` statement or an `@err(e);` forward observes an **error**—at the unwrap site, not deferred to exit. Soft-return from an `@errhandler` body (including a bang-binder handler) uses the same cleanup epilogue as any other return in that function. See §3.1.

```c
CCArena scratch = cc_arena_heap(kilobytes(64));
@defer cc_arena_free(&scratch);
```

**Conditional defer (error/success):**

`@defer(err) stmt;` runs only if the function returns an error result.
`@defer(ok) stmt;` runs only if the function returns a success result.

These are useful for ownership transfer patterns where cleanup should only happen on error:

```c
Result*!>(IoError) compress_block(Block* blk) {
    CCArena res_arena = cc_arena_heap(blk->data.len + 4096);
    @defer(err) cc_arena_free(&res_arena);  // cleanup on error only
    
    Result* res = res_arena.allocT();  // cc_arena_alloc_T(Result, res_arena)
    if (!res) return cc_err(io_error(CC_IO_OUT_OF_MEMORY));
    
    // ... fill in res, do allocations ...
    
    // Transfer ownership: detach leaves res_arena empty, so cleanup is no-op
    res->arena = res_arena.detach() !>;  // cc_arena_detach(&res_arena)
    return cc_ok(res);
}
```

**Arena ownership transfer with `cc_arena_detach`:**

`cc_arena_detach(CCArena* a)` (UFCS: `a.detach() !>`) transfers arena-owned mallocs
(heap L1, L2, Main) to a new owner, leaving the source handle dead. It refuses a
stack or caller-owned L1 (the returned handle would dangle) and an outstanding
checkpoint loan; both cases leave the source unchanged and return `cc_err`.

```c
CCArena !>(CCError) cc_arena_detach(CCArena* a);  // heap-owned L1 only
```

After a successful detach:

- The source handle is dead (`a.a == NULL`) — cleanup is a no-op
- The returned handle owns all the memory and allocations
- The caller is responsible for eventually freeing the returned arena

**Rule:** `@defer(err)` and `@defer(ok)` are only valid in functions returning a result type (`T!>(E)`). Using them in a function returning a non-result type is a compile error.

**Lowering:**

```c
// @defer(err) STMT;
// lowers to (conceptual):
@defer { if (__cc_returning_error) { STMT; } }

// where __cc_returning_error is set by the return statement
// before running defers when the return value is a result with .ok == false
```

**Named defer:**

`@defer name: stmt;` creates a cancellable defer.

```c
@defer cleanup: cc_arena_free(&scratch);

// ... later, if ownership is transferred ...
@cancel_defer cleanup;  // defer will not run
```

**Rule:** `@cancel_defer name;` prevents the named defer from running. A second `@cancel_defer` of the same live name is a no-op. It is a compile error to `@cancel_defer` an unknown name, or to `@cancel_defer` a name before its `@defer` or outside the name's block. `@cancel_defer` requires a name.

**Rule:** The name introduced by `@defer name:` is scoped to the enclosing block, like a local variable declared at the `@defer` statement. Referencing it (including `@cancel_defer`) before the `@defer` statement or outside the block is a compile error.

**Lowering (implementation sketch, not surface syntax):**

```c
// @defer cleanup: STMT;
// ...
// @cancel_defer cleanup;

// lowers to:
bool __cleanup_active = true;
@defer { if (__cleanup_active) { STMT; } }
...
__cleanup_active = false;  // @cancel_defer cleanup;
```

**Note:** Lowering is conceptual; the backend may implement defers via a hidden stack of cleanup actions, not via nested `@defer` syntax.

**Use cases:**

```c
// Transaction commit/rollback
void!>(DbError) transfer(Db* db, Account from, Account to, int amount) {
    db.begin() !>(e) return cc_err(e);
    @defer rollback: db.rollback();
    
    db.debit(from, amount) !>(e) return cc_err(e);
    db.credit(to, amount) !>(e) return cc_err(e);
    
    @cancel_defer rollback;  // success: don't rollback
    db.commit() !>(e) return cc_err(e);
}

// Conditional cleanup
void!>(IoError) process(char[:] path, CCArena out) {
    CCArena scratch = cc_arena_heap(kilobytes(64));
    @defer cleanup: cc_arena_free(&scratch);
    
    char[:] data = read_file(&scratch, path) !>(e) return cc_err(e);
    
    if (should_keep(data)) {
        // Transfer to output arena
        char[:] copy = out->alloc_slice_bytes(data.len);
        if (!copy.ptr) return cc_err(io_error(CC_IO_OUT_OF_MEMORY));
        memcpy(copy.ptr, data.ptr, data.len);
        // Still want cleanup to run - don't cancel
    }
    
    // cleanup runs on all paths
}
```

---

### 5.2 Scoped Arena Lifetimes

Arena lifetime is expressed with ordinary C scopes and declaration-attached
cleanup. There is no dedicated arena block form in the language surface.

**Create + free:**

```c
{
    CCArena scratch = cc_arena_heap(kilobytes(64)) @destroy;
    char[:] tmp = read_file(&scratch, "x");
    use(tmp);
}
// scratch is freed here
```

The identifier `scratch` follows ordinary C lexical scoping. The `@destroy`
annotation on the declaration schedules `cc_arena_destroy(&scratch)` at scope
exit, including on `return` or result propagation.

**Scoped reset:**

```c
CCArena scratch = cc_arena_heap(kilobytes(256)) @destroy;

for (int i = 0; i < 10; i++) {
    CCArenaCheckpoint cp = scratch.checkpoint();
    char[:] tmp = scratch.alloc_slice_bytes(1024);
    process(tmp);
    cc_arena_restore(cp);  // memory reclaimed; post-checkpoint slices invalidated
}
```

Use checkpoints when a long-lived arena needs repeated scratch regions. A
checkpoint/restore pair is explicit and composes with ordinary control flow;
`@defer` may be used to restore on all exits from a nested scope when needed.

**Rule:** Any slice derived from an arena that is freed by `@destroy`, reset by
`cc_arena_reset`, or invalidated by `cc_arena_restore` is treated as potentially
invalid after that operation. Using it afterward is a compile error unless it is
proven independent (e.g., copied to longer-lived storage).

---

## 6. Threads

This section defines the shipped C-level atomic surface. Thread and scheduler
entry points are library APIs; their concrete contracts are defined by their
headers and the focused scheduler specification.

---

### 6.1 Thread and task closures

The language does not ship a `Thread` / `spawn_thread` family. Direct OS
threading uses the platform C API. `CCNursery.spawn` is the shipped structured
task surface. The capture rules below apply to spawned task closures and to
thread adapters supplied by a library.

**Rule (captures in thread closures):** Capturing a variable `v` into a thread closure is allowed iff:

1. `v` is capturable (see §2.2), AND
2. `v` is not assigned after the capture point, AND
3. No address of `v` is taken in a way that escapes the closure's scope

**Rule (value capture):** Thread closures capture by value by default. For copyable types, the captured value is a copy. For move-only types (e.g., `Map::[K,V]`, unique slices), the capture is a move and the original becomes invalid. Value-captured variables are immutable within the closure.

**Rule (reference capture):** Explicit reference capture `[&v]` creates a shared reference to the outer variable. Reference captures are subject to mutation checks:

- Read-only access is allowed
- Mutation is a compile error unless the access uses a shipped C atomic,
  channel operation, registered synchronization library, or `@unsafe`.

```c
int x = 0;
CCNursery n = cc_nursery_create() !> @destroy;

// Value capture (default): x is copied, immutable in closure
n.spawn(() => { printf("%d", x); });       // ✅ OK
n.spawn(() => { x++; });                   // ❌ ERROR: value capture is immutable

// Reference capture: explicit sharing with mutation check
n.spawn(() => [&x] { printf("%d", x); });  // ✅ OK: read-only
n.spawn(() => [&x] { x++; });              // ❌ ERROR: mutation of shared ref

// Shared atomic storage uses the shipped C surface.
cc_atomic_int counter = 0;
n.spawn(() => { cc_atomic_fetch_add(&counter, 1); });

// Escape hatch: @unsafe bypasses check
n.spawn(@unsafe () => [&x] { x++; });     // ⚠️ OK: explicit unsafe
```

This prevents data races while allowing explicit shared state through safe wrappers.

---

### 6.2 Direct OS Threading (Advanced)

**For most applications, use `CCNursery` (§8.1) instead of the APIs in this section.**

Direct OS thread control is rarely needed. Concrete thread entry points and
platform-specific affinity controls are library APIs, not Appendix D ABI
commitments. Appendix D specifies stable data and call layouts.

---

### 6.3 Locks

`Mutex::[T]`, `AsyncMutex::[T]`, their guard types, and `@lock` are not
language constructs. A use of any of these spellings is a compile-time error.
Programs use C mutex APIs or a library type that explicitly registers and emits
its own synchronization contract.

### 6.4 Atomic operations

The shipped portable surface is declared by `<ccc/cc_atomic.cch>`:

```c
typedef /* implementation-selected */ cc_atomic_int;
typedef /* implementation-selected */ cc_atomic_uint;
typedef /* implementation-selected */ cc_atomic_size;
typedef /* implementation-selected */ cc_atomic_i64;
typedef /* implementation-selected */ cc_atomic_u64;
typedef /* implementation-selected */ cc_atomic_intptr;

cc_atomic_fetch_add(ptr, value);
cc_atomic_fetch_sub(ptr, value);
cc_atomic_load(ptr);
cc_atomic_store(ptr, value);
cc_atomic_cas(ptr, expected_ptr, desired);
```

`fetch_add` and `fetch_sub` return the previous value. `load` returns the
observed value. `store` stores the supplied value. `cas` compares against
`*expected_ptr`, stores `desired` on success, and reports success as a boolean;
on the C11 path it updates `*expected_ptr` on failure according to
`atomic_compare_exchange_strong`.

On a C11 compiler these operations use sequentially consistent atomics. The
GCC/Clang compatibility path uses full-barrier `__sync` operations. If
`CC_ATOMIC_HAVE_REAL_ATOMICS` is zero, the provided fallback is not
thread-safe and concurrent use is invalid.

Explicitly ordered forms carry the named order on the C11 path:

```c
cc_atomic_load_relaxed(ptr);
cc_atomic_store_relaxed(ptr, value);
cc_atomic_fetch_add_relaxed(ptr, value);
cc_atomic_fetch_sub_relaxed(ptr, value);
cc_atomic_load_acquire(ptr);
cc_atomic_store_release(ptr, value);
cc_atomic_cas_acquire(ptr, expected_ptr, desired);
cc_atomic_cas_acq_rel(ptr, expected_ptr, desired);
```

Relaxed forms are for a location that a lock already serializes, where the
lock's acquire and release are the only ordering needed. `cas_acquire` and
`cas_acq_rel` may fail spuriously and are for a retry loop; acq_rel is the
form for a transition that also publishes what the caller did before it. On the compatibility path a relaxed
access is a volatile access and the acquire and release forms are full
barriers; the non-atomic fallback maps every form to its plain counterpart.

`Atomic::[T]` is not a live generic family.

---

## 7. Channels

This section defines message-passing and coordination primitives:

- **§7.1 Channel Types** — topologies and views
- **§7.2 Ordered Channels** — `ordered` flag and `cc_channel_send_task`
- **§7.3 Semantics** — buffering and termination
- **§7.4 Copy vs Transfer** — `send` vs `send_take`
- **§7.5 Select / Multiplex** — multiplexing operations
- **§7.6 Timeouts and Duration** — time-bounded operations
- **§7.7 Channel API** — function signatures
- **§7.8 Owned Channels** — resource pools
- **§7.9 Bidirectional Error Propagation** — pipeline error handling

---

Channels are typed queues for **handoff and coordination**.

Key properties:

- `recv(&out)` receives into caller-provided storage
- Blocking/suspending `send`, `send_take`, and `recv` return
  `bool!>(CCIoError)`
- Slices are copied by default; transfer is explicit

The channel result model is uniform:

- send `ok(true)` means the value was accepted;
- send `ok(false)` means the channel is closed;
- recv `ok(true)` means a value was written to `out`;
- recv `ok(false)` means the channel is closed and drained; and
- `err(e)` reports transport, task, cancellation-aware, or deadline-aware
  failure as defined by that operation.

`ok(false)` never means timeout, cancellation, or an application payload
error. Cancellation observed by a channel operation is
`err(cc_io_from_errno(ECANCELED))`; deadline expiry is
`err(cc_io_from_errno(ETIMEDOUT))`. A channel whose element type is
itself `T!>(E)` carries that application result as the payload written to
`out`; it does not merge the payload's tag with the channel-operation result.

**Rule:** Channels participate in the ordinary UFCS model at the **language surface**, but use explicit send/receive operations with caller-provided output storage rather than zero-argument receive.

The normative surface form is:

- `tx.send(v)`
- `rx.recv(&out)`
- `h.close()`
- `h.free()`

**Rule (dispatch):** UFCS dispatch is selected from the resolved receiver type, not from the method name alone. The receiver is the full expression to the left of `.` or `->`.

**Rule (channel families):** Standard channels are library-owned UFCS families keyed by the concrete channel handle type (`CCChanTx_`*, `CCChanRx_`*). The surface operations above are normative; exact generated helper names are backend details.

**Examples:**

```c
// Native CCC (preferred)
tx.send(job);
@await rx.recv(&result);
rx.close();
ch.free();

// Dispatch uses the resolved receiver expression
ctx.tx.send(job);
worker->rx.recv(&result);
holder.handle.close();
ptr->handle.free();
```

**Rule:** `@await` is only valid inside `@async` functions.

**Rule:** `recv(&out)`, `send()`, and `send_take()` are dual-mode operations:

- In `@async` code, they are suspension points and must be written as `@await rx.recv(&out)` / `@await tx.send(v)` / `@await tx.send_take(v)` unless used inside `select` (which implicitly awaits). The compiler may lower these further to task-returning runtime helpers internally, but that is not part of the language-level API.
- In sync code, they block the OS thread.

**Rule:** Channel operations keep the same **surface signatures** in sync and async contexts: `send(v)`, `recv(&out)`, `send_take(v)`, `close()`, and `free()`. The suspension behavior is contextual; the internal runtime lowering may differ, but the user-facing model stays in terms of these receiver-first operations.

### 7.1 Channel Types

Channels are categorized by **mode** (async vs sync) and **topology/direction**.

#### Mode: Async vs Sync


| Type                 | Meaning                 |
| -------------------- | ----------------------- |
| `T[~ ... async ...]` | async channel (default) |
| `T[~ ... sync ...]`  | sync channel (blocking) |


**Split handle model:**

- A channel is represented by **two distinct handles**:
  - `T[~... >]` — **send-only** handle (tx)
  - `T[~... <]` — **recv-only** handle (rx)
- A channel is created only by producing a `(tx, rx)` pair (see **Creation** below).
- `close(...)` is valid on any live channel handle. Closing affects the shared underlying channel state and is observed by all views as termination.

**Async channel topology:**


| Type          | Meaning                              |
| ------------- | ------------------------------------ |
| `T[~ ... >]`  | send-only handle (tx)                |
| `T[~ ... <]`  | recv-only handle (rx)                |
| `T[~n N:N >]` | buffered N:N sender                  |
| `T[~n N:N <]` | buffered N:N receiver                |
| `T[~n 1:1 >]` | single producer sender               |
| `T[~n 1:1 <]` | single consumer receiver             |
| `T[~n N:1 >]` | many producers sender                |
| `T[~n N:1 <]` | one consumer receiver                |
| `T[~n 1:N >]` | broadcast publisher (one producer)   |
| `T[~n 1:N <]` | broadcast subscriber (receiver view) |


**Sync channel topology:**


| Type                  | Meaning                             |
| --------------------- | ----------------------------------- |
| `T[~ ... sync ... >]` | send-only handle (tx), blocking     |
| `T[~ ... sync ... <]` | recv-only handle (rx), blocking     |
| `T[~n sync N:N >]`    | buffered N:N sender (blocking)      |
| `T[~n sync N:N <]`    | buffered N:N receiver (blocking)    |
| `T[~n sync 1:1 >]`    | single producer sender (blocking)   |
| `T[~n sync 1:1 <]`    | single consumer receiver (blocking) |
| etc.                  | (all topologies available)          |


**Grammar:**

```
channel_handle := element_type '[~' capacity? mode? topology? direction bp? ']'
capacity     := integer_constant_expr
mode         := 'async' | 'sync'
topology     := '1:1' | '1:N' | 'N:1' | 'N:N'
direction    := '>' | '<'   // REQUIRED (combined channels removed)
bp           := ',' ('Drop' | 'DropNew' | 'DropOld' | 'Drop_New' | 'Drop_Old')
```

- `capacity` must be a compile-time integer constant expression (or omitted for unbuffered).
- `mode` defaults to `async` if omitted.
- `topology` tokens `1:1`, `1:N`, `N:1`, `N:N` are parsed as single tokens.
- Whitespace between components is optional: `T[~10 async N:1]` and `T[~10asyncN:1]` are equivalent.
- When both `topology` and `direction` appear, `topology` comes first: `T[~10 1:N <]`.
- Backpressure, when present, comes **after** direction: `T[~10 >, DropOld]`. Omitting it is Block. `Drop` and `DropNew` (and `Drop_New`) are the same mode; `DropOld` (and `Drop_Old`) is distinct. There is no `, Block` token. See §8.4.0.

**Topology meanings:**

- `N:N` — any number of senders, any number of receivers
- `N:1` — any number of senders, exactly one receiver
- `1:N` — broadcast: one sender, subscribers get independent copies

**Creation (normative):**

Channels are created by producing a `(tx, rx)` pair:

```c
int[~10 >] tx;
int[~10 <] rx;
CCChan* ch = cc_channel_pair(&tx, &rx);  // returns the underlying channel
```

Notes:

- `cc_channel_pair` initializes both handles to the same underlying channel and returns a pointer to it.
- `tx` and `rx` are **capability handles** (typed views), not resources. They do not need to be freed.
- `close(tx)` / `close(rx)` closes the underlying channel. Idiomatically, close is scheduled on nursery teardown: `CCNursery n = cc_nursery_create() !> @destroy { tx.close(); };` (§8.1.4).
- `cc_channel_free(ch)` frees the channel. Always free the channel, not the handles.

**Ownership idiom:**

```c
int[~10 >] tx;
int[~10 <] rx;
CCChan* ch = cc_channel_pair(&tx, &rx) !> @destroy { cc_channel_free(ch); };

CCNursery outer = cc_nursery_create() !> @destroy;
outer.spawn(() => consumer(rx));

CCNursery inner = outer.create_child() !> @destroy { tx.close(); };
inner.spawn(() => producer(tx));
```

**Error channels:**

Channels can carry results using `T!>(E)` as the element type:

```c
int!>(ParseError)[~100 >] results_tx;        // async sender of Result::[int, ParseError]
int!>(ParseError)[~100 <] results_rx;        // async receiver of Result::[int, ParseError]
CCChan* results_ch = cc_channel_pair(&results_tx, &results_rx);

int!>(ParseError)[~100 sync >] sync_results_tx;   // sync sender
int!>(ParseError)[~100 sync <] sync_results_rx;   // sync receiver
CCChan* sync_results_ch = cc_channel_pair(&sync_results_tx, &sync_results_rx);
```

**Rule (recv result type):** `recv(&out)` returns `bool !>(CCIoError)`.

- `ok(true)` = a value was received and written into `out`
- `ok(false)` = the channel is closed and drained
- `err(e)` = transport/runtime failure

```c
// Async channel: must use @await
int!>(ParseError) r;
bool !>(CCIoError) got = @await results_rx.recv(&r);
if (cc_is_err(got)) {
    handle_io_error(cc_error(got));
} else if (!cc_value(got)) {
    // channel closed+drained
} else if (r.ok) {
    int v = r.value;   // application success
} else {
    ParseError e = r.error;  // application-level error value
}

// Sync channel: no @await
int!>(ParseError) r2;
bool !>(CCIoError) got2 = sync_results_rx.recv(&r2);
if (cc_is_err(got2)) {
    handle_io_error(cc_error(got2));
} else if (!cc_value(got2)) {
    // channel closed+drained
} else if (r2.ok) {
    int v = r2.value;
} else {
    ParseError e = r2.error;
}
```

**Broadcast subscription:**

```c
int[~10 1:N >] events;                     // async broadcast publisher (send-capable)
int[~10 1:N <] sub = events.subscribe();   // subscribe to async broadcast

int[~10 1:N sync >] sync_events;                // sync broadcast publisher
int[~10 1:N sync <] sync_sub = sync_events.subscribe();  // subscribe to sync
```

---

### 7.2 Ordered Channels and Task Sending

The `ordered` attribute is the channel's delivery-order guarantee. It states one property — the order of what flows through the channel is preserved — applied to the channel's payload kind:

- **Data channels:** `ordered` guarantees **per-sender FIFO** delivery. Items from a given sender are received in the order that sender sent them; interleaving between different senders is unspecified.
- **Task-handle channels** (used with `cc_channel_send_task`): `ordered` guarantees handles are received in **submission order**, not completion order. This is essential for pipelines where output order must match input order (e.g., parallel compression).

**Rule (default channels promise no order):** A channel not declared `ordered` makes **no** delivery-order promise. Code that relies on delivery order must declare the channel `ordered`.

**Ordered channel declaration:**

```c
T[~N ordered >] tx;    // ordered sender
T[~N ordered <] rx;    // ordered receiver
```

`**cc_channel_send_task` — spawn and queue:**

```c
cc_channel_send_task(tx, () => expr);            // spawn task, queue result
cc_channel_send_task(tx, () => [captures] expr); // with captures
```

`cc_channel_send_task` spawns a fiber immediately to execute the closure, then queues the task handle in the channel. The receiver's typed `recv` awaits the task internally and extracts the result.

**How it works:**


| Channel Declaration | Task Execution   | Result Delivery                              |
| ------------------- | ---------------- | -------------------------------------------- |
| `T[~N >]`           | Immediate, async | No order promise (typically completion order) |
| `T[~N ordered >]`   | Immediate, async | Submission order (FIFO)                      |


Both execute tasks immediately. The difference is purely in `recv` sequencing.

**Full example — parallel compression pipeline:**

```c
CompressedResult*[~16 ordered >] results_tx;
CompressedResult*[~16 ordered <] results_rx;
CCChan* ch = cc_channel_pair(&results_tx, &results_rx) !> @destroy { cc_channel_free(ch); };

CCNursery outer = cc_nursery_create() !> @destroy;
outer.spawn(() => {
    CompressedResult* r;
    while (cc_io_avail(results_rx.recv(&r))) {
        cc_file_write(out, r->data);
        cc_arena_free(&r->arena);
    }
});

// Producer nursery closes results_tx after all workers exit, so the consumer sees EOF.
CCNursery inner = outer.create_child() !> @destroy { results_tx.close(); };
while (read_block(&blk)) {
    cc_channel_send_task(results_tx, () => [blk] compress_block(blk));
}
```

**Error propagation:**

When the closure returns `T!>(E)`, errors flow through:

```c
cc_channel_send_task(results_tx, () => {
    CompressedResult*!>(CCIoError) res = compress_block(blk);
    return res;  // Error preserved!
});

// Consumer drains until closed-and-drained (see cc_io_avail idiom above)
CompressedResult* r;
while (cc_io_avail(results_rx.recv(&r))) {
    use(r);
}
```

`cc_channel_recv` on a task channel follows the same result model:

- `ok(true)` — value received
- `ok(false)` — channel closed and drained
- `err(e)` — task or transport failure

**Mixing values and tasks:**

The same channel can receive both values and tasks:

```c
int[~16 >] tx;
int[~16 <] rx;
cc_channel_pair(&tx, &rx);

cc_channel_send(tx, 42);                // send a value
cc_channel_send_task(tx, () => compute()); // send a task

int x;
cc_channel_recv(rx, &x);  // gets 42
cc_channel_recv(rx, &x);  // gets compute() result (awaited internally)
```

**Backpressure:**

Standard channel semantics apply:

- `cc_channel_send_task` spawns immediately
- If channel is full, blocks until space available
- Provides parallelism up to channel capacity, then natural backpressure

---

### 7.3 Semantics

- **Buffered channels** enqueue up to `n` items; sends block when full.
- **Unbuffered channels** rendezvous: sender blocks until receiver is ready.
- **Close** does not drop buffered values; they remain available for recv.
- **Termination** is observed when the channel is both closed and drained (`recv(&out)` returns `ok(false)`).

**Rule (delivery order):** Delivery order is guaranteed only on channels declared `ordered` (§7.2): per-sender FIFO on data channels, submission order on task-handle channels. A channel without `ordered` makes no delivery-order promise.

**Rule:** `recv(&out)` returns `ok(false)` only when the channel is closed and drained. It never uses `ok(false)` to represent an application-level payload error. For per-item errors, use `T!>(E)[~ <]` channels where the received value written to `out` is itself a `T!>(E)`.

**Rule (slice element ownership):** For slice element types, `send` deep-copies into channel-internal storage. While queued, the channel owns the copy. On successful `recv`, the receiver gets a **unique slice**; the receiver frees it on scope exit (or transfers it via `send_take` / return). If the value is never received (still buffered when channel is freed), the channel frees it.

**Rule (broadcast `1:N`):** Topology does not change backpressure. `1:N` still Blocks when a subscriber buffer is full unless the handle spells `Drop` / `DropNew` / `DropOld` (§8.4.0). Never-block broadcast is `T[~N 1:N >, DropOld]` (drop that subscriber's oldest, enqueue the new value). `Drop` / `DropNew` rejects the new send with `EAGAIN` and leaves the buffer unchanged.

**Rule (broadcast copy semantics):** On a `1:N` channel, `send` performs a deep copy for **each subscriber** at send time. Each subscriber receives an independent unique slice. For slice elements, this means N independent allocations for N subscribers.

**Rule (broadcast close):** Closing a `1:N` channel closes the broadcaster and all subscriber views. Subscribers may continue draining buffered values; after drain, `recv(&out)` returns `ok(false)`.

**Close semantics:**

- `close()` is idempotent — calling it multiple times is safe and has no additional effect.
- A send admitted after close is observed returns `ok(false)` and does not
  enqueue the value. A send already in the enqueue pipeline may complete.
- Failed `send_take` does not consume the slice — it remains valid.
- Buffered values are **not** dropped on close — receivers can drain them.

---

### 7.4 Copy vs Transfer

- `send(slice)` — deep-copies contents into channel-internal storage.
- `send_take(slice)` — transfers ownership of the backing allocation (zero-copy).
- `send_take` never falls back to copying; it fails if ineligible.
- Broadcast (`1:N`) forbids `send_take` (each subscriber needs its own copy).

`**send_take` Eligibility (Normative):**

A call to `send_take(ch, slice)` succeeds iff **all** of the following hold:

1. `slice.id.is_unique == 1`
2. `slice.id.is_transferable == 1`
3. `slice.id.is_subslice == 0`
4. The destination channel is not closed
5. The channel topology is not broadcast (`1:N`)

These conditions are checked using the slice's `id` field directly — **no runtime table lookup is required** in release builds.

**Rule (derived slices ineligible):** Any derived slice value (including subslices, rebindings, or copies) clears `is_unique` and is therefore ineligible for `send_take`, even if it covers the full allocation. Only the original owning unique slice can be transferred.

**Failure behavior:**

- If any eligibility condition fails, `send_take` returns `err(EINVAL)` and
  does not consume the slice.
- On success, ownership of the allocation is transferred to the channel and all borrows are invalidated.

```c
char[:] x;
bool !>(CCIoError) got = @await ch.recv(&x);  // on ok(true): is_unique=1, is_transferable=1, is_subslice=0
@await dst.send_take(x);                      // OK: transfers the unique slice

// Derived slices cannot be transferred:
char[:] view = x[0..x.len];                  // view has is_unique=0 (derived)
@await dst.send_take(view);                   // err(EINVAL): is_unique=0
```

**Why `adopt()` slices cannot use `send_take`:**

`adopt()` slices have `is_transferable == 0` because the user-provided deleter may not be thread-safe. If the slice were transferred to another thread and freed there, a thread-local allocator would corrupt memory. Channel-internal allocations (`recv()` slices) use a thread-safe allocator that we control, so they have `is_transferable == 1`.

```c
// ✓ Channel pipeline - zero copy
char[:] x;
bool !>(CCIoError) got = @await a.recv(&x);   // on ok(true): is_transferable=1
@await b.send_take(x);                        // OK

// ✗ FFI buffer - must copy
unsafe {
    char[:] s = adopt(p, 100, custom_free);  // is_transferable=0
}
@await ch.send(s);                    // OK: copies (safe)
@await ch.send_take(s);               // err(EINVAL): is_transferable=0
```

**Rule:** `adopt()` slices are unique (move-only, have destructor) but not transferable. Use `send` to copy them across channels.

#### Build Into Channel Storage

`send_into` and `try_send_into` are the default idiom for data that carries
payloads — the data-channel twin of `send_task`. Reserve a slot, then write
the element into that reserved output (destination-first, immediate mode).
Ordinary `send` / `try_send` remain for already-stable values (scalars,
static/canonical slices, unique slices).

The channel handle determines the element type and size; the surface does not
expose `.raw`, `sizeof(T)`, or errno-to-Result conversion.

```c
bool !>(CCIoError) queued =
    tx.try_send_into(
        (slot, arena) => [req] {
            *slot = build_reply(req, arena);  /* produce into the slot */
            return NULL;
        },
        payload_arena);
```

On a typed `T[~ … >]` handle, an untyped `(slot, arena)` builder infers
`slot` as `T*` and `arena` as `CCArena`. Explicit parameter types remain
allowed and are not overwritten. The builder's `slot` denotes uninitialized
storage for one `T`. `arena` is optional element payload backing supplied by
the caller (or an empty handle) — a write buffer for variable-sized bytes in `*slot`,
not a channel-owned pool the runtime resets. The runtime does not acquire or
extend that arena's lifetime; internal pointers in the constructed `T` remain
subject to their ordinary provenance and lifetime contracts.

**Rule (scoped slot):** The slot address is valid only during the builder call.
It must not escape, and program behavior must not depend on whether it names a
final channel slot, a receiver rendezvous buffer, or hidden staging storage.

**Rule (complete construction):** A builder that returns normally has fully
initialized `*out`. Builders are synchronous and must not suspend. Builder
failure is represented in the channel element itself (for example
`T!>(E)[~ ... >]`) or handled before the call; it is not added to the channel
operation's `CCIoError`.

**Rule (`try_send_into` admission):** The builder runs exactly once if an
element slot is admitted. If the channel is full, has no rendezvous partner,
or is closed/error-closed before admission, the builder does not run.
`EAGAIN` maps to `err(CC_IO_BUSY)` through the normal channel Result envelope;
graceful close maps to `ok(false)`. The call consumes the builder either
way: it is run exactly once, or dropped without running (its environment is
released).

**Rule (`send_into` backpressure):** `send_into` applies ordinary blocking
backpressure. An implementation first may attempt direct construction. If no
slot is immediately available, it may invoke the builder exactly once into
hidden staging storage and then perform an ordinary blocking send. Direct
construction versus staging is not observable except through performance and
the emitted lowering.

**Rule (delivery order):** Construction occupies the same delivery position as
an ordinary `send`. `ordered` data-channel guarantees therefore apply
unchanged. Task-handle and owned channels do not accept `send_into`.

**Lowering (normative):**

```c
// tx.try_send_into(builder, arena)
cc_channel_try_send_into(tx, builder, arena)

// typed macro / family lowering
cc_chan_result_with(tx.raw,
    cc_channel_raw_try_send_into(tx.raw, builder,
                                 cc_chan_elem_size(tx.raw), arena),
    false)
```

`send_into` lowers identically through `cc_channel_raw_send_into`. The raw
four-argument C forms remain available when the caller supplies `CCChan*`,
builder, element size, and arena explicitly.

---

### 7.5 Select / Multiplex

Channel multiplexing is a **library call**, not a statement form. The runtime
primitive `cc_chan_match_select(...)` waits on multiple channel operations.
`@match` is unsupported and is a compile-time error.

```c
int x; int v = 42;
CCChanMatchCase cases[2];
cases[0] = (CCChanMatchCase){ .ch = rx.raw, .send_buf = NULL, .recv_buf = &x, .elem_size = sizeof(x), .is_send = false };
cases[1] = (CCChanMatchCase){ .ch = tx.raw, .send_buf = &v,  .recv_buf = NULL, .elem_size = sizeof(v), .is_send = true  };
size_t ready = (size_t)-1;
int rc = cc_chan_match_select(cases, 2, &ready, cc_current_deadline());
switch (ready) {
    case 0: handle(x); break;
    case 1: sent();    break;
    default: /* rc = ETIMEDOUT (deadline) or EPIPE (closed) */ break;
}
```

- Callable from both plain-thread and fiber code; the caller parks until
  a case is ready or the deadline passes.
- First ready case wins; if multiple ready, one is chosen non-deterministically.

**Rule (select readiness on closed):**

- A `recv` case is ready if a value is available OR if the channel is closed+drained (binding `null`).
- A `send` / `send_take` case is ready if it can complete immediately. If the channel is closed, it is ready immediately and completes with `false`.

**Rule (selection):** The first case that successfully claims the select group
wins. If several cases are ready concurrently, the winner is unspecified.
Select provides no fairness or starvation guarantee. Losing registrations are
removed before the call returns.

---

### 7.6 Timeouts and Duration

**Duration type:**

`Duration` represents a time span with nanosecond precision:

```c
struct Duration {
    int64_t secs;    // seconds
    int32_t nanos;   // nanoseconds (0 to 999,999,999)
};
```

**Duration literals:**


| Literal | Meaning       |
| ------- | ------------- |
| `1ns`   | 1 nanosecond  |
| `1us`   | 1 microsecond |
| `1ms`   | 1 millisecond |
| `1s`    | 1 second      |
| `1m`    | 1 minute      |
| `1h`    | 1 hour        |


**Rule:** Duration literals are evaluated at compile time. Overflow is a compile-time error.

**Rule:** Duration arithmetic (`+`, `-`, `*`, `/`) is supported. Runtime overflow is undefined behavior (debug builds may trap).

**Timeouts:**

Timeout-bounded channel waits use the deadline argument of
`cc_chan_match_select`:

```c
int v = 0;
CCChanMatchCase c = (CCChanMatchCase){ .ch = rx.raw, .send_buf = NULL, .recv_buf = &v, .elem_size = sizeof(v), .is_send = false };
size_t ready = (size_t)-1;
CCDeadline d = cc_deadline_after_ms(100);
int rc = cc_chan_match_select(&c, 1, &ready, &d);
if (rc == ETIMEDOUT) handle_timeout();
```

**Rule:** Timeouts on multiplexed waits are expressed through the deadline parameter of `cc_chan_match_select` (there is no `timeout(...)` readiness case).

---

### 7.7 Channel API

```c
// Creation (normative): produce a (tx, rx) pair. Combined channels are not allowed.
int[~10 >] tx;
int[~10 <] rx;
cc_channel_pair(&tx, &rx);

// === ASYNC CHANNELS (int[~ ... >] / int[~ ... <]) ===
// send/recv operations require @await in @async code

// Core operations (must @await)
bool !>(CCIoError) sent = @await send(tx, value);
T x;
bool !>(CCIoError) got = @await recv(rx, &x);

// Slice transfer (must @await; send handle only)
bool !>(CCIoError) sent_take = @await send_take(tx, slice);

// Close (no @await; either live handle)
void close(tx);                         // idempotent
void close(rx);

// Non-blocking (no @await, either context)
bool !>(CCIoError) sent_now = try_send(tx, value);
bool !>(CCIoError) built_now = tx.try_send_into(builder, arena);
T y;
bool !>(CCIoError) got_now = try_recv(rx, &y);

// === SYNC CHANNELS (int[~ ... sync ... >] / int[~ ... sync ... <]) ===
// All operations block, no @await allowed

int[~10 sync >] stx;
int[~10 sync <] srx;
cc_channel_pair(&stx, &srx);

// Core operations (no @await, blocks)
bool !>(CCIoError) sent = send(&stx, value);
bool !>(CCIoError) built = stx.send_into(builder, arena);
T x; bool !>(CCIoError) got = recv(&srx, &x);   // blocks OS thread until received; ok(false) = closed+drained

// Slice transfer (no @await, blocks; send handle only)
bool !>(CCIoError) sent_take = send_take(&stx, slice);

// Close (no @await; either live handle)
void close(&stx);
void close(&srx);

// Non-blocking (no @await, either context)
bool !>(CCIoError) sent_now = try_send(stx, value);
T y; bool !>(CCIoError) got_now = try_recv(srx, &y);
```

**Operations Comparison:**


| Operation                 | Async `T[~ ... >]` / `T[~ ... <]`, inside `@async` | Async, fiber/sync context | Sync `T[~ ... sync ... >]` / `T[~ ... sync ... <]` |
| ------------------------- | --------------------------------------------------- | ------------------------- | -------------------------------------------------- |
| `send(ch, v)`             | `@await send(...)` ✅                                 | `send(...)` ✅ (blocks)    | `send(...)` ✅                                      |
| `recv(ch)`                | `@await recv(...)` ✅                                 | `recv(...)` ✅ (blocks)    | `recv(...)` ✅                                      |
| `send_take(ch, s)`        | `@await send_take(...)` ✅                            | `send_take(...)` ✅        | `send_take(...)` ✅                                 |
| `send_into(ch, b, a)`     | blocking edge¹                                        | `send_into(...)` ✅        | `send_into(...)` ✅                                 |
| `try_send_into(ch, b, a)` | `try_send_into(...)` ✅                               | `try_send_into(...)` ✅    | `try_send_into(...)` ✅                             |
| `try_send(ch, v)`         | `try_send(...)` ✅                                   | `try_send(...)` ✅         | `try_send(...)` ✅                                  |
| `try_recv(ch)`            | `try_recv(...)` ✅                                   | `try_recv(...)` ✅         | `try_recv(...)` ✅                                  |
| `close(ch)`               | `close(...)` ✅                                      | `close(...)` ✅            | `close(...)` ✅                                     |
| `subscribe(ch)`           | `subscribe(...)` ✅                                  | `subscribe(...)` ✅        | `subscribe(...)` ✅                                 |

¹ `send_into` currently uses the synchronous blocking-edge contract in an
`@async` body; it has no task-returning `@await` variant. `try_send_into`
returns immediately in every context.


**Rule (`@await` marks suspension points, not channel flavor):** `@await` is
required on suspending channel operations (`send()`, `recv(&out)`,
`send_take()`)
**inside `@async` function bodies** — the state-machine lowering must know its
suspension points explicitly. Omitting `@await` there is a compile error
(diagnosed as "channel operation must be awaited in @async function").

**Rule (fiber / synchronous context):** Outside `@async` bodies — `main`,
spawned closures, ordinary functions running on fibers or OS threads — the
same operations are called **without** `@await` and block the caller: a fiber
parks and the scheduler proceeds; a plain thread blocks, as a C programmer
expects. This is the form used throughout `examples/` and `real_projects/`.

**Rule (sync channel operations):** All operations on sync channel handles (`T[~ ... sync ... >]` / `T[~ ... sync ... <]`) that may block have no `@await` in any context. These include `send()`, `recv(&out)`, and `send_take()`. Adding `@await` is a compile error.

**Rule (non-blocking operations):** `try_send()`, `try_recv()`, `close()`, and `subscribe()` are valid on both async and sync channels without `@await`. They return immediately or have no return value.

`try_send` and `try_recv(&out)` return `bool !>(CCIoError)` with the same
`ok(true)` / `ok(false)` / `err(e)` meanings as `send` and `recv`. They return
immediately; an unavailable open channel is reported as
`err(cc_io_from_errno(EAGAIN))`.

**Rule (send_take conditional move):** `send_take` consumes the slice only on
`ok(true)`. On `ok(false)` or `err(e)`, the caller retains ownership.

```c
char[:] s = ...;
bool !>(CCIoError) sent = @await ch.send_take(s);
if (cc_is_err(sent) || !cc_value(sent)) {
    // Closed or failed: s remains valid.
    use(s);  // OK
}
// On ok(true), s is invalid.
```

**Rule (async ownership):** In `@async` code, ownership transfer via `send_take` occurs at the suspension point. After a successful `@await send_take`, the source slice is invalidated exactly as if moved synchronously. There is no "partial" or "pending" ownership state across the `@await`.

```c
char[:] x;
bool !>(CCIoError) got = @await ch.recv(&x);  // on ok(true): x owns unique slice
bool !>(CCIoError) sent = @await dst.send_take(x);
if (cc_io_avail(sent)) {
    // x is now invalid, ownership transferred
    use(x);  // ERROR: use after move
} else {
    // Channel was closed, x still valid
    use(x);  // OK
}
```

**Rule (send borrows slices):** When passing a slice to `send`, the slice is borrowed for the duration of the copy operation, not moved. This applies even to unique slices:

```c
char[:] u;
bool !>(CCIoError) got = @await src.recv(&u);   // on ok(true): u owns unique slice
@await dst.send(u);                             // borrows u, copies bytes, u still valid
use(u);                                        // OK: u still owns the buffer
```

Both `recv(&out)` and `try_recv(&out)` return buffered values after close.
After the buffer drains, both report `ok(false)`.

**Note (schematic signatures):** Signatures use `T[~ ... >]`* / `T[~ ... <]`* as shorthand for the channel handle family. The actual type system uses the full channel handle type including capacity, mode, topology, and direction:

```c
// Full type signatures (what the compiler sees):
bool !>(CCIoError) send::[T, N, Topo](T[~N Topo >]* ch, T value);       // send-only
bool !>(CCIoError) recv::[T, N, Topo](T[~N Topo <]* ch, T* out);      // recv-only
bool !>(CCIoError) try_send::[T, N, Topo](T[~N Topo >]* ch, T value);
bool !>(CCIoError) try_recv::[T, N, Topo](T[~N Topo <]* ch, T* out);
```

`cc_io_avail(r)` is the shipped convenience predicate for
`bool !>(CCIoError)`: it is exactly `cc_is_ok(r) && cc_value(r)`. It is useful
for loops that intentionally stop on either close or error; code that must
distinguish those cases checks `cc_is_err(r)` first.

Capacity `N` and topology `Topo` are erased at runtime (all use the same implementation), but the type system enforces view restrictions at compile time.

**Rule:** Calling `send` on a recv-only channel view (`T[~n <]`), or `recv(&out)` on a send-only channel view (`T[~n >]`), is a compile-time error.

**Rule:** Channel handles must be initialized via `cc_channel_pair(&tx, &rx)` (or equivalent constructor for special topologies). The combined form `T[~n]` is not allowed.

---

### 7.8 Owned Channels (Resource Pools)

Owned channels extend the channel primitive with **lifecycle management**, enabling the pool pattern where resources are borrowed and returned rather than transferred.

**Key distinction:**

- **Plain channels** transfer ownership (sender → receiver, one-way flow)
- **Owned channels** retain ownership (pool owns items, users borrow/return)

#### 7.8.1 Syntax

```c
T[~N owned {
    .create = () => create_expr,
    .destroy = (T item) => destroy_expr,
    .reset = (T item) => reset_expr      // optional
}] pool;
```

**Components:**


| Part       | Syntax   | Description                                             |
| ---------- | -------- | ------------------------------------------------------- |
| `owned`    | Modifier | Marks channel as resource pool                          |
| `.create`  | Closure  | Called when pool is empty and `recv` is called          |
| `.destroy` | Closure  | Called for each item when pool scope exits              |
| `.reset`   | Closure  | Called on each item when returned via `send` (optional) |


**Grammar:**

```
owned_channel := element_type '[~' capacity 'owned' lifecycle_block ']' identifier
lifecycle_block := '{' lifecycle_callbacks '}'
lifecycle_callbacks := lifecycle_callback (',' lifecycle_callback)*
lifecycle_callback := '.' ('create' | 'destroy' | 'reset') '=' closure_expr
closure_expr := '(' param_list? ')' '=>' ('[' capture_list ']')? expr
```

#### 7.8.2 Semantics

**Borrow semantics:**

```c
CCArena[~4 owned {
    .create = () => cc_arena_heap(4096),
    .destroy = (CCArena a) => cc_arena_free(&a),
    .reset = (CCArena a) => cc_arena_reset(&a)
}] arena_pool;

// Borrow: recv from pool
CCArena arena;
arena_pool.recv(&arena);  // Creates if empty, else returns existing

// Use the resource
// ...

// Return: send back to pool (auto-reset via .reset)
arena_pool.send(arena);   // Calls .reset before re-adding to pool
```

**Rule (create on empty):** When `recv` is called on an empty owned channel and capacity allows, `.create` is invoked to produce a new item. If capacity is reached and all items are borrowed, `recv` blocks until an item is returned.

**Rule (reset on return):** When `send` is called on an owned channel, `.reset` (if provided) is invoked on the item before it is added back to the pool. This enables automatic cleanup between borrows.

**Rule (destroy on scope exit):** When an owned channel goes out of scope, `.destroy` is called for each item in the pool. Items that are currently borrowed are **not** destroyed—the borrower retains responsibility until they return the item or the program terminates.

**Rule (no direction modifier):** Owned channels are implicitly bidirectional (both send and recv). Direction modifiers (`>`, `<`) are not allowed with `owned`.

#### 7.8.3 Lifetime and Ownership

**Rule (pool lifetime):** An owned channel's lifetime is the lexical scope in which it is declared. The pool must outlive all borrows.

**Rule (borrow tracking):** Owned channels do not track which task borrowed which item. Returning a different item than borrowed is allowed (useful for item exchange patterns). The contract is: every `recv` should be paired with a `send` of a compatible item.

**Rule (capacity semantics):** Capacity `N` specifies the maximum number of items the pool can hold. The pool may contain fewer items if some are borrowed or if `.create` hasn't been called enough times.

#### 7.8.4 Error Handling

**Rule (create failure):** If `.create` returns a value indicating failure (e.g., null pointer, zero-initialized struct), the behavior is undefined. `.create` should allocate successfully or abort.

**Rule (owned channel close):** Owned channels can be closed with `cc_channel_close()`. After close:

- `recv` returns immediately with the closed status
- Items still in the pool are destroyed via `.destroy`
- Borrowed items are not affected (borrower still holds them)

#### 7.8.5 Example: Arena Pool

```c
// Pool of 4 arenas, each 4KB
CCArena[~4 owned {
    .create = () => cc_arena_heap(4096),
    .destroy = (CCArena a) => cc_arena_free(&a),
    .reset = (CCArena a) => cc_arena_reset(&a)
}] arena_pool;

CCNursery n = cc_nursery_create() !> @destroy;
// Pool destroyed when enclosing scope ends: .destroy called for each arena
n.spawn(() => {
    CCArena arena;
    arena_pool.recv(&arena);  // Borrow
    void* p = cc_arena_alloc(arena, 100, 1);
    arena_pool.send(arena);   // Return (auto-reset)
});
```

#### 7.8.6 Example: Connection Pool

```c
typedef struct { int fd; bool valid; } Connection;

Connection[~10 owned {
    .create = () => [host, port] {
        Connection c = { .fd = connect_to(host, port), .valid = true };
        return c;
    },
    .destroy = (Connection c) => { if (c.valid) close(c.fd); },
    .reset = (Connection c) => { /* connections don't need reset */ }
}] conn_pool;

// Borrow connection
Connection conn;
conn_pool.recv(&conn);
send_request(conn.fd, req);
Response resp = recv_response(conn.fd);
conn_pool.send(conn);  // Return to pool
```

#### 7.8.7 Comparison: Plain vs Owned Channels


| Aspect    | Plain Channel                 | Owned Channel                      |
| --------- | ----------------------------- | ---------------------------------- |
| Ownership | Transfers (sender → receiver) | Retained (pool owns)               |
| Item flow | One-way (pass through)        | Circular (borrow/return)           |
| Cleanup   | Consumer's responsibility     | Pool handles via `.destroy`        |
| RAII      | Not needed                    | Required (`.create`/`.destroy`)    |
| Direction | Required (`>` or `<`)         | Forbidden (implicit bidirectional) |
| API       | `send`/`recv`                 | `send`/`recv` (same!)              |


**Rule (same API):** Owned channels use the same `send`/`recv` UFCS surface as plain channels. The semantic difference (transfer vs borrow) is determined by the `owned` modifier at declaration time.

---

### 7.9 Bidirectional Error Propagation

Channels support bidirectional error propagation for pipeline error handling. Typed `send` / `recv` return `bool !>(CCIoError)`.

**Functions:**

```c
// Close with error — after drain, recv() returns err(cc_io_from_errno(err))
// rather than ok(false) (the regular-close drained signal).
void cc_chan_close_err(CCChan* ch, int err);

// Close rx side with error — send() returns err(cc_io_from_errno(err))
// rather than blocking or returning ok(false).
void cc_chan_rx_close_err(CCChan* ch, int err);
```

**Use case:** In parallel pipelines, when a worker encounters an error, it can signal both upstream (to stop producers) and downstream (to stop consumers):

```c
// Worker hits error - propagate BOTH directions:
cc_chan_close_err(results_tx.raw, err);   // → consumer sees err on recv after drain
cc_chan_rx_close_err(blocks_rx.raw, err); // → producer sees err on send
return;
```

**Rule (upstream propagation):** After `cc_chan_rx_close_err(ch, err)`, subsequent `send()` operations on that channel return `err(cc_io_from_errno(err))` instead of blocking or returning `ok(false)`.

**Rule (downstream propagation):** After `cc_chan_close_err(ch, err)`, subsequent `recv()` operations return `err(cc_io_from_errno(err))` instead of `ok(false)` once the channel is drained.

**Rule (regular close unchanged):** `close()` / `cc_channel_close(ch)` still drain then return `ok(false)` on `recv` (the closed+drained signal). The raw errno layer uses `EPIPE` for that same condition; the typed envelope is `ok(false)`, not `err`.

---

## 8. Concurrency

Concurrent-C provides structured concurrency through nurseries (`CCNursery`, §8.1) and a lexical fork-join (`@parallel`, §8.11). A nursery is a scope-bound handle that manages task spawning, joining, and explicit cooperative cancellation. The brace and `for` forms of `@parallel` are `CCParallel !>(CCError)`: create can fail; `.wait()` is the join. The wait-for form is `bool !>(CCError)`. `@parallel` is not a nursery. For the rare case where OS-level thread control is required, low-level APIs exist but should not be used in typical application code.

This section specifies:

- **§8.1 Structured Concurrency with `CCNursery`** — the primary pattern for all concurrent work
- **§8.2 Blocking and Non-Blocking Call Edges** — how `@blocking` / `@nonblocking` (function-level and call-site; `@noblock` is a compatibility spelling) determine the mode of each call edge from an `@async` body
- **§8.3 Tasks** — `CCTaskIntptr` frame/poll/drop ownership
- **§8.4 Channels in Async vs Sync** — context-sensitive channel operations
- **§8.5 Cancellation** — operation-specific cooperative cancellation and deadlines
- **§8.6 Streaming** — channel-based producers
- **§8.7 Runtime API** — function signatures for tasks, timing, and sync bridging
- **§8.8 Blocking, Stalling, and Execution Contexts** — execution model for blocking operations, stalling classification, and cancellation guarantees
- **§8.9 Error handling in async and nurseries** — composition of result unwrap operators (`?>`, `!>`, `@err`, `@errhandler`) defined in §3.1 with async functions and nursery teardown
- **§8.10 Named exclusive sections (`CCExclusive`)** — arena-backed, name-keyed mutual exclusion for short critical sections; `acquire_when` gates entry on a predicate
- **§8.11 `@parallel`** — `CCParallel !>(CCError)` join of independent assignments or `@serial` arms (§8.11.2), live dest / `.wait()` / `.cancel()` / `.adopt()`, optional spawn predicate, `@parallel for` over a half-open index range, and `@parallel wait @for` as `bool !>(CCError)` (§8.11.6)
- **§8.12 Ordered pipeline turnstile (`CCTurnstile`)** — depth cap plus sequenced stages; stage wait/pass use create-on-first-touch gate cells

---

### 8.1 Structured Concurrency with `CCNursery`

A **nursery** is a join set with a handle. Every task is a child of some nursery.

```
OPEN ──spawn*──┬── JOINING ── EMPTY ── DEAD     owner stays (wait / @destroy)
               └── LEFT    ── EMPTY ── DEAD     owner gone (leave)
```

| Phase | Meaning |
| --- | --- |
| OPEN | admit spawn |
| JOINING | owner waiting |
| LEFT | handle consumed; children may still run |
| EMPTY | join set empty: close armed channels; leftover if the path was LEFT |
| DEAD | freed |

`wait` and `@destroy` keep the handle (OPEN → JOINING → EMPTY → DEAD). `leave` consumes the handle (OPEN → LEFT → EMPTY → DEAD). `close(tx)` arms this nursery's EMPTY to close `tx` on both paths. It is not teardown.

`CCNursery` is a library type. The join form is `a.create_nursery()` (handle in
the arena; the walk joins). `cc_nursery_create()` is the self-owned malloc form
(`@destroy` or `leave`). Nested cancel inheritance is `parent.create_child()`.
The construction-plus-destruction pattern is idiomatic:

```c
{
    CCNursery n = cc_nursery_create() !> @destroy {
        // runs after all children have joined
    };
    n.spawn(() => work1());
    n.spawn(() => work2());
}
```

The `@destroy` clause on the declaration schedules nursery teardown (which joins all children) at scope exit. Nothing in the lowered form runs implicitly — `@defer`-shaped lifetime (§5.1) is the normative mechanism.

Ordinary lexical blocks create nested nursery lifetimes; `n.spawn(...)` is
UFCS on the explicit nursery handle.

**Properties:**

- Tasks spawned on `n` are children of `n`.
- The nursery's `@destroy` waits for all children to complete before returning.
- Child task handles cannot outlive the nursery's scope (compile-time error if they escape).
- `n.wait()` joins every child and returns the first nonzero child error it
  records; it does not cancel siblings.
- `n.leave()` consumes the handle (OPEN → LEFT). The caller does not join. The runtime
  releases the join set after the last child is dead (EMPTY → DEAD).
- Peer tasks cannot wait on each other (compile-time error).

---

#### 8.1.1 Construction

`CCNursery !>(CCError) cc_arena_create_nursery(CCArena a)` (UFCS
`a.create_nursery()`) births a nursery into a live arena. A null or dead arena
aborts. `CCNursery !>(CCError) cc_nursery_create(void)` is the self-owned handle
(`leave` is allowed). `CCNursery !>(CCError) cc_nursery_create_child(CCNursery parent)`
(UFCS `parent.create_child()`) snapshots cancel and deadline from a required
parent handle; an empty parent (null host) aborts.

```c
@errhandler(CCError e) cc_error_exit(e);

CCArena frame = cc_arena_heap(kilobytes(8)) @destroy;
CCNursery outer = frame.create_nursery() !>;
CCNursery inner = outer.create_child() !> @destroy;
```

The `!>` operator consumes the result: on success, the value is bound; on error, control transfers to the matching `@errhandler` for that Result `E` (§3.1). The trailing `@destroy` is a `@defer`-shaped destructor (§5.1) that joins children on scope exit.

Nursery cleanup waits for children and discards the join integer. It does not
forward a child error to `@errhandler` or `!>`. Code that must observe a child
error calls `n.wait()` explicitly, checks the returned integer before leaving
the scope, and then allows the registered cleanup to free the nursery.

---

#### 8.1.2 Spawning

Spawn a child task via UFCS on the nursery handle:

```c
n.spawn(() => work());                 // lambda expression
n.spawn(worker_fn);                     // function reference
n.spawn(() => worker_with_arg(x));      // captured argument
```

The compiler enforces the following normative rules:

- **Rule (task handle escape):** A task handle returned by `spawn` may not be stored in a variable that outlives the nursery, returned from the enclosing function, or captured in closures escaping the nursery.
- **Rule (no peer joins):** A child task may not @await or otherwise join another sibling's completion.
- **Rule (join the set):** The caller joins the set with `@destroy` or `n.wait()`. `n.leave()` consumes the handle and does not join.

---

#### 8.1.3 Error Propagation and Cancellation

`n.cancel()` requests cooperative cancellation for children. Child
code observes it through `n.is_cancelled()`, `cc_cancelled()`, or a
wait that explicitly observes the current nursery. Nursery wait still joins
every admitted child before teardown returns. A child return value does not
cancel peers.

`n.spawn(...)` is admission: it fails when the child cannot be queued
(dead nursery, cancelled, OOM). A child body error is not spawn's result. `n.wait()`
is the join: it reports the first child join error, if any. A bare spawn
or wait that ignores Result is ill-formed.

```c
CCNursery n = cc_nursery_create() !> @destroy;
n.spawn(() => ok_task()) !>;
n.spawn(() => failing_task()) !>;   /* admission ok; body error is wait */
n.wait() !>(e) { return map_child_error(e); };
// The sibling runs to completion unless code explicitly calls n.cancel().
```

---

#### 8.1.4 Channel Close Ordering

A nursery's registered pre-destroy hook waits for all children. Its
`@destroy` body then runs, followed by the registered free hook. Close a
producer channel in that body to signal EOF to a consumer owned by an outer
nursery:

```c
CCNursery outer = cc_nursery_create() !> @destroy;
outer.spawn(() => consumer(rx)) !>;

CCNursery producers = outer.create_child() !> @destroy { tx.close(); };
producers.spawn(() => producer(tx)) !>;
```

The producer nursery joins producers, closes `tx`, and frees itself. The outer
nursery can then join the consumer after it drains to `ok(false)`.

`@destroy` waits, then frees. Join error on that wait is not dropped
(`cc_error_exit`). Use `n.wait() !>` when the join error has local policy.

Use nested nurseries to sequence producer-close before consumer-drain:

```c
CCNursery outer = cc_nursery_create() !> @destroy;
outer.spawn(() => consumer(rx)) !>;

CCNursery inner = outer.create_child() !> @destroy { tx.close(); };
for (int w = 0; w < N; w++) inner.spawn(() => worker(tx)) !>;
// inner's @destroy closes tx after workers exit; consumer drains and
// outer's @destroy joins the consumer.
```

**Registered close form.** `n.close(tx)` is UFCS for
`cc_nursery_close(n, tx)` (same registration as `cc_nursery_add_closing_tx`).
Deprecated: `close_on` / `cc_nursery_close_on`. It arms this nursery's EMPTY to
close `tx` after wait / `@destroy`, or on the LEFT path, and before nursery
storage is released:

```c
CCNursery n = cc_nursery_create() !> @destroy;
n.close(tx) !>;                 // equivalent to @destroy { tx.close(); }
n.spawn(() => producer(tx)) !>;
```

An explicit `@destroy { tx.close(); }` body and `n.close(tx)` have the same
observable close-after-join placement, but they are distinct lowerings.

`@nursery`, bare `nursery { ... }`, `spawn { ... }`, and `@closing(...)` are
unsupported spellings and are compile-time errors. Structured concurrency uses
an explicit `CCNursery` declaration and UFCS `spawn` / `close` / `leave` calls.

If `CC_NURSERY_CLOSING_RUNTIME_GUARD=1`, a receive that would park in the
current nursery waiting for a channel registered in that same nursery's
`close` set fails with `EDEADLK`. This immediate specialized guard is
optional and is independent of the scheduler's general detector (§8.7.1).

---

#### 8.1.5 Leave

`n.leave()` is UFCS for `cc_nursery_leave` and consumes the handle
(OPEN → LEFT). Deprecated: `abandon` / `cc_nursery_abandon`. `n.leave(ctx, finish)`
registers one leftover that runs at EMPTY on the LEFT path only, then leaves
(`cc_nursery_leave_with`). Deprecated leftover-only registration: `on_last` /
`cc_nursery_on_last` — prefer the two-argument `leave`. `wait` / `@destroy`
never run a leftover; the owner writes the next line at EMPTY.

```c
n.leave(q, finish_q);   // leftover at EMPTY on the LEFT path
n.leave();              // leave with no leftover
```

A program uses either `wait` / `@destroy` or `leave`. Mixing them is a
programming error (the runtime aborts). After `leave` the handle is
invalid: no `wait`, `free`, `spawn`, or `close`. Spawn after `leave` fails
with `EINVAL`.

When `alive_count` is already zero, EMPTY runs on the caller. When children
remain, the last child's completion runs EMPTY on a scheduler worker (the
child fiber is already dead). EMPTY closes registered channels, runs leftover
if the path was LEFT, and frees the nursery (DEAD).

Leftover does not run on `wait` / `@destroy`. The leftover must not `wait`,
`free`, or `leave` this nursery. `leave` is not cancellation; in-flight
work runs to completion. `leave` requires worker-frees mode (the default).
`on_signals` does not compose with `leave`. Drop a listener with an ordinary
assignment before `leave`, not as a leftover.

`@destroy` and `leave` do not compose: `@destroy` waits.

---

#### 8.1.6 Guarantees

A nursery guarantees:

- All spawned children are joined before the nursery's `@destroy` returns.
- No child outlives the join set. After `leave`, the handle is invalid;
  the set is released after the last child is dead.
- No forgotten-join deadlocks (impossible syntactically) on the `wait` /
  `@destroy` path.
- No cyclic peer waits (impossible syntactically).
- First recorded child error returned by an explicit `n.wait()`.
- Explicit cooperative cancellation through `n.cancel()`.
- Deterministic channel close ordering (via `@destroy { ch.close(); }`, or `n.close(tx)`), including on the LEFT path at EMPTY.

A nursery does **not** guarantee:

- Deadlock freedom for channel cycles across sibling nurseries.
- Fairness or starvation freedom.
- Immediate cancellation of blocking operations (cooperative; see §8.5).
- Stack unwinding on cancellation.
- That `leave` composes with `on_signals`.

---

### 8.2 Blocking and Non-Blocking Call Edges

An `@async` function compiles to a pollable state machine whose body
may yield cooperatively at every `@await`. When it calls a function
that may block an OS thread, the compiler wraps the call in
`run_blocking` (dispatches it to the thread pool and yields until the
worker returns). The question "does this call edge get wrapped?" is
answered by two annotations, **`@blocking`** and **`@nonblocking`**,
which are *dual*: each one can appear on a function declaration
(function ambient default), on a lexical block (block ambient default),
and on an individual call site (local override). `@noblock` is a
compatibility spelling for `@nonblocking`.

#### 8.2.1 State-machine gating

**Rule (state machines are gated on `@async`):** Only `@async`
functions are lowered to a state machine. A sync function labeled
`@blocking` or `@nonblocking` is still plain C — no frame lifting, no
suspension points, no yield mechanics. Its `@blocking` / `@nonblocking`
label is a *contract to async callers* describing how their call
edges should be lowered; it does not change how the function's own
body is compiled.

```c
@nonblocking void fast_helper(void); // plain C; async callers skip run_blocking
@blocking    FILE* slow_helper(void); // plain C; async callers wrap in run_blocking
             void plain_helper(void); // plain C; inherits caller/block ambient
```

#### 8.2.2 Call-edge mode resolution (normative)

At every call site inside an `@async` body, the compiler picks a mode
— **`@blocking`** (wrap in `run_blocking`, yield) or **`@nonblocking`**
(direct call, no yield) — using the following precedence:

1. **Call-site annotation** (highest precedence)
   - `@blocking f(...)` — force this edge through `run_blocking`.
   - `@nonblocking f(...)` — force this edge to skip `run_blocking`.
2. **Callee's annotation** (`@blocking` / `@nonblocking` on the callee)
   - `@blocking fn f(...) { … }` → edge mode is `@blocking`.
   - `@nonblocking fn f(...) { … }` → edge mode is `@nonblocking`.
   - **Definition wins TU-locally.** When the callee is defined in the
     same translation unit, the annotation on the **definition** is
     authoritative for every call edge to it in that TU. A forward
     declaration need not repeat the annotation, and call sites need no
     annotation of their own. This includes `static inline` definitions.
   - If a visible declaration carries `@nonblocking` but the same-TU
     definition does not, the definition's classification wins (the
     edge is lowered as blocking) and the compiler emits exactly one
     warning naming both coordinates:

     ```
     warning: async: declaration of 'f' at FILE:LINE promises @noblock
     but its definition at FILE:LINE does not carry it
     ```

   - For a callee with no definition in the TU (extern / FFI), the
     declaration's annotation applies (see §8.2.7).
3. **Lexical block ambient mode** (innermost annotated block)
   - `@blocking { ... }` → undecorated known-CC call edges default to `@blocking`.
   - `@nonblocking { ... }` → undecorated known-CC call edges default to `@nonblocking`.
4. **Caller's ambient mode** (one-hop only; see §8.2.4)
   - If the enclosing `@async` function was declared
     `@async @blocking` → edges default to `@blocking`.
   - If the enclosing `@async` function was declared
     `@async @nonblocking`  → edges default to `@nonblocking`.
5. **Callee category fallback** (lowest)
   - Other `@async` function → no wrapping (the two state machines
     compose directly via `@await`).
   - Extern / FFI / unknown indirect (function pointer) call →
     defaults to `@blocking` (conservative; see §8.2.5).
   - Non-FFI sync CC function with no annotation and no ambient
     override → defaults to `@blocking`.

#### 8.2.3 Default ambient for `@async`

**Rule:** A plain `@async fn` with neither `@blocking` nor `@nonblocking`
at the declaration resolves ambient to `@blocking` (rule 3 above is
effectively equivalent to rule 4's FFI/fallback default). This keeps
async code conservative by default — blocking-looking call sites in
an `@async` body are bounced to the thread pool unless explicitly
opted out.

```c
@async void conservative(int fd) {
    char buf[128];
    sys_read(fd, buf, 128);   // FFI → @blocking edge → run_blocking + yield
    helper(fd, buf);          // sync CC fn w/o annotation → @blocking edge
}

@async @nonblocking void hot_path(int fd) {
    // Ambient is @nonblocking: every call below is a direct call
    // unless the callee or site opts back in.
    some_pure_helper();             // direct call
    @blocking sys_read(fd, …);      // call-site override: run_blocking + yield
}
```

#### 8.2.4 One-hop inheritance

**Rule:** Ambient mode applies only at the **direct** call edges of the
function where it was declared. It is **not** transitive through the
call graph.

```c
           fn inner(void);           // no annotation
@nonblocking fn middle(void) {       // ambient @nonblocking
    inner();                         // edge at `middle` uses @nonblocking → direct call
}
@async @nonblocking fn outer(void) { // ambient @nonblocking
    middle();                        // edge at `outer` uses @nonblocking → direct call
}
```

Inside `middle`, the call `inner()` is lowered once using `middle`'s
own ambient. We do **not** re-lower `middle`'s body under `outer`'s
ambient — `middle` is compiled independently, and the edge into it
from `outer` is the only edge `outer`'s `@nonblocking` affects. This is
what makes `@blocking` / `@nonblocking` compatible with separate
compilation.

#### 8.2.5 Call-site overrides

A call-site annotation is the local exception to the ambient policy:

```c
@async @nonblocking void serve(CCChanRx rx) {
    // Hot path: ambient @nonblocking.  Most calls are direct.
    while (true) {
        RedisRequest req;
        if (@await rx.recv(&req) != 0) break;

        @nonblocking {
            fast_decode(&req);      // direct call (block ambient)
            fast_dispatch(&req);    // direct call
        }

        if (req.needs_disk) {
            @blocking write_log(&req);  // one-edge exception: bounce to pool
        }
    }
}
```

Call-site annotations are the primary ergonomic win: you write the hot
path in its natural shape and opt individual calls into (or out of)
thread-pool dispatch without restructuring the surrounding code.

#### 8.2.6 Lowering examples

```c
extern int           read (int fd, void* buf, int n);   // FFI → default @blocking
extern @nonblocking size_t strlen(const char* s);       // FFI + explicit @nonblocking
@blocking FILE* open_config(const char* path);          // sync CC fn, @blocking
@nonblocking size_t strlen_nb(const char* s);           // sync CC fn, @nonblocking

@async void example(int fd, const char* p) {
    char buf[128];

    //  edge mode     | reason
    // ---------------+-------------------------------------------------
    read(fd, buf, 128);       // @blocking | callee is FFI → fallback
    strlen("abc");            // @nonblocking | callee annotated @nonblocking
    open_config(p);           // @blocking | callee annotated @blocking
    strlen_nb("abc");         // @nonblocking | callee annotated @nonblocking
    other_async();            // (async)   | callee is @async; uses @await directly

    // Call-site overrides:
    @nonblocking read(fd, buf, 128); // @nonblocking | site beats callee-FFI default
    @blocking strlen("abc");         // @blocking    | site beats callee @nonblocking
}
```

Concrete C lowering for each edge mode uses
`cc_run_blocking_task_intptr`, a `CCTaskIntptr` child slot, and the common
poll/drop contract specified in **Appendix J.1.1**.

#### 8.2.7 FFI default and soundness

**Rule:** All `extern` functions (C FFI) default to `@blocking` at
call edges from `@async` bodies. Mark them `@nonblocking` to skip
wrapping:

```c
extern @nonblocking int    memcmp(const void* a, const void* b, size_t n);
extern @nonblocking void   memcpy(void* dst, const void* src, size_t n);
extern @nonblocking size_t strlen(const char* s);
```

**Rule:** Declaring `@nonblocking` on a function that may actually block
an OS thread is **undefined behavior**. The compiler may assume the
annotation is correct and elide wrapping safeguards. In debug builds,
implementations may add runtime checks; in release builds, violations
are not recovered.

**Rule:** Declaring `@blocking` on a function that never blocks is
always safe — it just imposes an unnecessary thread-pool dispatch
cost at async call edges. `@blocking` is the always-safe direction;
`@nonblocking` is the contract obligation.

#### 8.2.8 Indirect / function-pointer calls

**Rule:** When the callee at a call site is a function pointer, the
compiler cannot see a callee-declaration annotation. Mode resolution
falls through to the ambient / FFI-fallback steps. Call-site
annotations still apply and are the recommended way to pin mode for
indirect dispatch:

```c
@async void run(Handler* h) {
    @nonblocking h->fast(ctx);  // trust the indirect callee; no bounce
    @blocking h->io(ctx);       // indirect callee; force the bounce
}
```

#### 8.2.9 Summary

| Surface                               | Meaning                                                                 |
| ------------------------------------- | ----------------------------------------------------------------------- |
| `@blocking fn f() { … }`              | Declaration: at `@async` call edges to `f`, wrap in `run_blocking`.     |
| `@nonblocking fn f() { … }`           | Declaration: at `@async` call edges to `f`, skip `run_blocking`.        |
| `@async @blocking fn g() { … }`       | `g`'s body has ambient `@blocking`; edges default to wrapping.          |
| `@async @nonblocking fn g() { … }`    | `g`'s body has ambient `@nonblocking`; edges default to direct calls.   |
| `@nonblocking { … }`                  | Lexical block ambient: direct known-CC calls default to direct calls.   |
| `@blocking expr;`                     | Call-site: force this one edge to wrap (beats callee + ambient).        |
| `@nonblocking expr;`                  | Call-site: force this one edge to direct-call (beats callee + ambient). |

`@noblock` is accepted as a compatibility spelling for `@nonblocking`
in declaration, function-ambient, and call-site positions.

The composition rule is the "one-hop" principle: **ambient applies
only at the direct call edges of the function where it was declared,
never transitively.** Each function is compiled against its own
annotations; call edges are resolved pointwise.

---

### 8.3 Tasks

An `@async` call constructs a poll-based `CCTaskIntptr`. The constructor,
frame, poll, and drop ABI is defined once in Appendix J.1 and J.5.

```c
@async int work(int x) {
    return x + 1;
}

CCTaskIntptr t = work(5);
intptr_t value = 0;
int err = 0;
while (cc_task_intptr_poll(&t, &value, &err) == CC_FUTURE_PENDING) {
    cc_yield();
}
cc_task_intptr_free(&t);
```

Source `@await` stores the child task in the parent frame and drives the same
poll contract. The owner calls `cc_task_intptr_free` exactly once; freeing
invokes the frame drop callback. Use `CCNursery` for structured groups of tasks
and the C task APIs in `<ccc/cc_sched.cch>` at explicit runtime boundaries.

---

### 8.4 Channels: Async vs Sync (Type-Based)

Channels are **typed as async or sync** at declaration. Sync handles always block. Async handles suspend cooperatively inside `@async` (those ops are written `@await`) and park a fiber or block a thread when called bare outside `@async` (§7 Operations Comparison, §8.4.2).

Channels also support **backpressure modes** to handle overload gracefully in server workloads.

---

#### 8.4.0 Backpressure Modes

When a bounded channel is full, the handle's backpressure mode decides what `send` does. Mode is spelled after direction (§7.1). Topology (`1:N` included) does not pick a mode.

**Modes (runtime `CCChanMode`):**

| Spelling | Runtime | When full |
| -------- | ------- | --------- |
| *(omitted)* | `CC_CHAN_MODE_BLOCK` | Sender blocks/suspends until space. Default. |
| `Drop`, `DropNew`, `Drop_New` | `CC_CHAN_MODE_DROP_NEW` | New send is rejected (`err` with `EAGAIN` / `CC_ERR_WOULD_BLOCK`). Buffer unchanged. |
| `DropOld`, `Drop_Old` | `CC_CHAN_MODE_DROP_OLD` | Oldest queued value is discarded; the new value is enqueued. Send succeeds. |

**Channel syntax:**

```c
T[~N >]                    // Block (default)
T[~N <]                    // Block (default)
T[~N >, Drop]              // DropNew — reject the new send
T[~N <, DropNew]           // same as Drop
T[~N >, DropOld]           // drop oldest, enqueue new
T[~N 1:N >, DropOld]       // broadcast that never blocks: drop that subscriber's oldest
T[~N sync >]               // Block, sync
T[~N sync >, Drop]         // DropNew, sync
```

**Rules:**

- Mode is fixed at declaration; channel type is immutable.
- There is no `, Block` token and no `Sample` mode.
- `recv()` never observes a partial message: a DropOld discard removes a whole value.
- Delivery order follows the §7.3 rule (`ordered` channels guarantee it; others do not promise it).

---

#### 8.4.1 Channel Types

**Async channels** (most common):

```c
int[~ >] tx;
int[~ <] rx;
CCChan* ch = cc_channel_pair(&tx, &rx);
// ... use tx, rx ...
cc_channel_free(ch);
```

**Sync channels:**

```c
int[~ sync >] tx;
int[~ sync <] rx;
CCChan* ch = cc_channel_pair(&tx, &rx);
// ... use tx, rx ...
cc_channel_free(ch);
```

**Rule:** Channel mode is fixed in the handle type. An `int[~ ...]` handle is always async; an `int[~ sync ...]` handle is always sync. Operations must match the handle type.

---

#### 8.4.2 Async Channels (`int[~ ... >]` and `int[~ ... <]`)

Async channels **suspend cooperatively**. Inside `@async` functions their
suspending operations are marked with `@await`; in fiber or synchronous
context the same operations are called bare and block the caller (a fiber
parks; a thread blocks). See the context rules in §7 (Operations Comparison).

**Operations:**

```c
int[~ >] tx;
int[~ <] rx;
CCChan* ch = cc_channel_pair(&tx, &rx);

// Inside an @async function: @await marks the suspension point
int x;
bool !>(CCIoError) got = @await recv(rx, &x);          // suspends until received
bool !>(CCIoError) ok = @await send(tx, 42);           // suspends until sent

// Inside an @async function, bare ops are an error
recv(rx, &x);                             // ❌ ERROR: must be awaited in @async function
send(tx, 42);                             // ❌ ERROR: must be awaited in @async function

// In fiber/sync context (main, spawned closures), bare ops block the caller
while (cc_io_avail(rx.recv(&x))) { use(x); }   // ✅ parks the fiber, no @await

cc_channel_free(ch);                      // free the channel when done
```

**Rule:** Inside `@async` function bodies, suspending operations on async
channels require `@await`; omitting it is a compile error. Outside `@async`
bodies, the same operations take no `@await` and block the calling fiber or
thread.

**Cancellation integration:**

```c
@async void!>(Error) reader(int[~ <] ch) {
    int x;
    bool !>(CCIoError) got = @await ch.recv(&x);
    if (cc_is_err(got) && cc_error(got).os_code == ECANCELED)
        return cc_err(Error_Canceled);
    if (cc_is_err(got)) return cc_err(Error_Io);
    if (cc_value(got)) process(x);
}
```

Channel operations that observe nursery cancellation report
`cc_io_from_errno(ECANCELED)`; there is no separate cancellation result type.

---

#### 8.4.3 Sync Channels (`int[~ ... sync ... >]` and `int[~ ... sync ... <]`)

Sync channels **block the OS thread** and do NOT use `@await`. They are used for thread coordination and blocking operations.

**Operations:**

```c
int[~ sync >] tx;
int[~ sync <] rx;
cc_channel_pair(&tx, &rx);

// No @await allowed
int x;
bool !>(CCIoError) got = recv(rx, &x);    // blocks OS thread
bool !>(CCIoError) ok = send(tx, 42);     // blocks OS thread

// Cannot use @await
@await recv(rx, &x);                  // ❌ ERROR: cannot @await sync channel
@await send(tx, 42);                  // ❌ ERROR: cannot @await sync channel
```

**Rule:** All operations on sync channels do NOT use `@await`. Adding `@await` is a compile error.

**Multiplexing sync channels (rare):** there is no statement-level
select. When a single thread must race multiple sync channels, call the
runtime select primitive directly — `cc_chan_match_select(...)` blocks
the OS thread until a case is ready or the deadline passes (§8.5.1).
Prefer one thread/fiber per source.

---

#### 8.4.4 Type Signatures Document Intent

Function signatures make clear what context is required:

```c
// Clearly async
@async int!>(Error) async_reader(int[~ <] ch) {
    int x = 0;
    bool !>(CCIoError) got = @await recv(ch, &x);
    if (cc_is_err(got)) return cc_err(Error_Io);
    if (!cc_value(got)) return cc_err(Error_EOF);
    return cc_ok(x);
}

// Clearly sync (blocks)
void sync_worker(int[~ sync <] requests) {
    int req;
    bool !>(CCIoError) got = recv(requests, &req);
    if (cc_io_avail(got)) process(req);
}

// Caller knows exactly what to do based on channel type
```

**Benefit:** No surprises during refactoring. Change a function to `@async` and the compiler immediately tells you what operations need `@await`.

---

#### 8.4.5 Refactoring Safety

When refactoring a sync function to async, the compiler enforces correctness:

```c
// Original: sync
void sync_handler(int[~ sync <] requests) {
    int req;
    bool !>(CCIoError) got = recv(requests, &req);
    if (cc_io_avail(got)) process(req);
}

// Refactored to async with wrong channel type:
@async void async_handler(int[~ sync <] requests) {
    // bool !>(CCIoError) got = @await recv(requests, &req);
    // ERROR: sync channel operations cannot be awaited
    // This won't compile—we need to change the channel type
}

// Refactored correctly:
@async void async_handler(int[~ <] requests) {
    int req;
    bool !>(CCIoError) got = @await recv(requests, &req);
    if (cc_io_avail(got)) process(req);
}
```

The compiler forces you to fix the channel type when refactoring. No silent behavior changes.

---

#### 8.4.6 Error Handling

**Async channels:**

```c
int x;
bool !>(CCIoError) got = @await recv(&rx, &x);
if (cc_is_err(got) || !cc_value(got)) {
    // Channel closed and drained (ok(false)) or error
}

// With error values (element type is itself a result)
int!>(Error) maybe_x;
bool !>(CCIoError) got_e = @await recv(&error_rx, &maybe_x);
if (cc_is_err(got_e) || !cc_value(got_e)) {
    // Channel closed and drained
} else if (cc_is_ok(maybe_x)) {
    process(cc_value(maybe_x));        // received a successful payload
} else {
    handle(cc_error(maybe_x));         // received an application-level error
}
```

**Sync channels:**

```c
int x;
bool !>(CCIoError) got = recv(&rx, &x);
if (cc_is_err(got) || !cc_value(got)) {
    // Channel closed and drained (ok(false)) or error
}
```

Same error handling semantics; only difference is blocking vs suspending.

---

#### 8.4.7 Comparison Table


| Aspect                   | Async handles (`int[~ ... >]` / `int[~ ... <]`) | Sync handles (`int[~ ... sync ... >]` / `int[~ ... sync ... <]`) |
| ------------------------ | ----------------------------------------------- | ---------------------------------------------------------------- |
| **`@await` inside `@async`** | Required on `send` / `recv` / `send_take`    | Never (adding `@await` is a compile error)                       |
| **`@await` outside `@async`** | Never — bare ops park a fiber or block a thread | Never                                                          |
| **Blocks OS thread**     | Only when the caller is a plain thread waiting on the op | Yes, when the op waits                                 |
| **Use in `@async` code** | Yes (primary)                                   | Allowed; the op blocks, with no `@await`                         |
| **Use in sync / fiber code** | Yes — same surface, no `@await`              | Yes (primary)                                                    |
| **Multiplex via `cc_chan_match_select`** | Yes                             | Yes (blocks OS thread)                                           |
| **Cancellation result** | `err(cc_io_from_errno(ECANCELED))` when observed      | `err(cc_io_from_errno(ECANCELED))` when observed                  |
| **Example use**          | Async streams, work queues in nurseries         | Thread coordination, OS thread pools                             |


---

#### 8.4.8 Real-World Patterns

**Pattern 1: Producer-Consumer (Async)**

```c
int[~ >] work_tx;
int[~ <] work_rx;
CCChan* work_ch = cc_channel_pair(&work_tx, &work_rx);

@async void producer() {
    for (int i = 0; i < 100; i++) {
        @await send(&work_tx, i);
    }
}

@async void consumer() {
    int work;
    while (cc_io_avail(@await work_rx.recv(&work))) {
        process(work);
    }
}

CCNursery n = cc_nursery_create() !> @destroy { work_tx.close(); };
n.spawn(() => producer());
n.spawn(() => consumer());
// cc_channel_free(work_ch) is scheduled on the @destroy of the channel pair above.
```

**Pattern 2: Async with Deadline**

```c
@async int!>(Error) reader(int[~ <] ch) {
    int x;
    bool !>(CCIoError) got = @await ch.recv(&x);
    if (cc_is_err(got)) return cc_err(Error.Io);
    if (!cc_value(got)) return cc_err(Error.Eof);
    return cc_ok(x);
}

@with_deadline(seconds(5)) {
    size_t ready;
    int rc = cc_chan_match_select(cases, ncases, &ready, cc_current_deadline());
    if (rc == ETIMEDOUT) return cc_err(Error.Timeout);
}
```

Only operations that consult the current deadline observe its expiry.

---

#### 8.4.9 Split Handle Model

Each channel value has one clear capability:

```c
int[~ >] tx;
int[~ <] rx;
cc_channel_pair(&tx, &rx);

@await rx.recv(&x);        // async receive handle
tx.send(v);               // send handle

int[~ sync >] stx;
int[~ sync <] srx;
cc_channel_pair(&stx, &srx);
srx.recv(&y);             // sync receive handle, no @await
```

The language model does not include a combined send/recv channel handle. A program works with the capability it holds (`tx` or `rx`), and the operation set follows from that handle type.

---

### 8.5 Cancellation & Deadline

Cancellation and deadlines are cooperative and have distinct sources:

- `n.cancel()` marks a nursery and wakes its children so
  nursery-aware waits can re-check that state.
- `cc_cancelled()` and `n.is_cancelled()` poll nursery state.
- `cc_cancel(CCDeadline*)` marks one deadline object.
- Absolute deadline expiry wakes deadline-aware parks; the operation re-checks
  time and returns its timeout result.
- `cc_current_deadline()` exposes the innermost active deadline to operations
  such as channel select and exclusive `acquire_when`.
- Exclusive `acquire_when` / `wait_release` observe that deadline and, when a
  current nursery exists, that nursery's cancel. Absence of a nursery is not
  cancel of that wait.

An operation observes only the sources named by its API or lowering. Plain
`@await`, plain channel operations, nursery-aware channel operations, and
deadline-aware channel operations therefore need not report the same error.

---

#### 8.5.1 Channel Multiplexing without `@match`

`@match` is unsupported. Its use is a compile-time error. Multiplexing uses
the ordinary library call `cc_chan_match_select(...)`.

The selected index is consumed with ordinary C control flow.

**Idiom (direct select, with pre-block cancel check and deadline):**

```c
int x; int v = 42;
if (cc_is_cancelled()) {
    /* current deadline scope was cancelled */
    return cc_err(cc_io_from_errno(ECANCELED));
}
CCChanMatchCase cases[2];
cases[0] = (CCChanMatchCase){ .ch = rx.raw, .send_buf = NULL, .recv_buf = &x, .elem_size = sizeof(x), .is_send = false };
cases[1] = (CCChanMatchCase){ .ch = tx.raw, .send_buf = &v,  .recv_buf = NULL, .elem_size = sizeof(v), .is_send = true  };
size_t ready = (size_t)-1;
int rc = cc_chan_match_select(cases, 2, &ready, cc_current_deadline());
switch (ready) {
    case 0: process(x); break;      /* rx delivered into x */
    case 1: /* v sent */ break;
    default: /* rc = ETIMEDOUT (deadline) or EPIPE (closed) */ break;
}
```

- The `cc_is_cancelled()` check runs **before** parking. The select
  itself is woken by channel readiness or deadline expiry
  (`cc_current_deadline()` picks up the innermost `@with_deadline`
  scope); there is no mid-wait cancel routing.
- `cc_chan_match_select` parks the calling OS thread (or fiber via the
  scheduler). In fiber code, prefer fiber-per-source over building
  select loops.

---

#### 8.5.2 Operation-Specific Observation

The shipped channel surface is `send`, `send_take`, and `recv`, each returning
`bool !>(CCIoError)`. When one of these operations observes cancellation, it
returns `err(cc_io_from_errno(ECANCELED))`. There is no separate cancellation result
type and no second channel-operation family.

`cc_chan_match_select(cases, n, &ready, deadline)` observes the supplied
deadline's clock expiry and returns `ETIMEDOUT`. It does not route an explicit
deadline cancellation into a parked select. Poll `cc_is_cancelled()` before
parking when explicit cancellation must be observed. An `@await` around an
unrelated task does not become cancellation-aware merely because a deadline or
nursery is in scope.

---

#### 8.5.3 Polling-Based Fallback

Nursery state and deadline-scope state use different polling APIs. They are not interchangeable.

| API | Source | Typical use |
| --- | --- | --- |
| `cc_cancelled()` | current nursery | `while (!cc_cancelled()) { … }` |
| `n.is_cancelled()` | named nursery | poll a handle |
| `cc_is_cancelled()` | current `@with_deadline` scope | zero-argument language macro → `cc_is_cancelled_current()` |
| `cc_is_cancelled(const CCDeadline*)` | explicit deadline | C function; the `.c` translation unit `#undef`s the macro |

Mixing them is a silent wrong-source poll: `cc_is_cancelled()` does not observe nursery cancel; `cc_cancelled()` does not observe deadline cancel. Prefer the table row that names the source the wait is supposed to see.

```c
while (!cc_cancelled()) {
    do_nursery_work();
}

@with_deadline(seconds(5)) {
    while (!cc_is_cancelled()) {
        do_deadline_scoped_work();
    }
}
```

---

#### 8.5.4 Cancellation Semantics

No cancellation source forcibly interrupts C code or performs stack unwinding.
`cc_task_cancel` requests cancellation of a concrete task handle;
`n.cancel()` requests cancellation of nursery children. The runtime may
wake parked work to permit observation, but the operation still determines the
reported result. Channel cancellation is represented by
`cc_io_from_errno(ECANCELED)`.

---

#### 8.5.5 Deadline-Bounded Multiplexing

`cc_chan_match_select` observes the supplied deadline's **clock** and returns `ETIMEDOUT` on expiry. It does not return `ECANCELED` for `cc_cancel` on that deadline. Poll `cc_is_cancelled()` (or `cc_is_cancelled(&d)`) **before** parking when explicit cancellation must be observed (§8.5.1, §8.5.2).

```c
CCDeadline d = cc_deadline_after_ms(5000);
if (cc_is_cancelled(&d)) return cc_err(WorkerError_Canceled);
size_t ready = (size_t)-1;
int rc = cc_chan_match_select(cases, ncases, &ready, &d);
if (rc == ETIMEDOUT) return cc_err(WorkerError_Timeout);
if (rc != 0) return cc_err(WorkerError_Io);  /* EPIPE / other */
dispatch_case(ready);
```

#### 8.5.6 Guarantees and Limitations

- Cancellation never preempts running C code or unwinds the stack.
- Nursery child failure does not request sibling cancellation.
- Nursery cancellation must be requested explicitly.
- A wait exits promptly only if that wait observes the relevant source.
- Channel operations report observed cancellation as
  `cc_io_from_errno(ECANCELED)`.
- Deadline-aware operations report clock expiry as `ETIMEDOUT`.
- Code that uses `cc_io_avail` intentionally treats error and graceful close as
  the same loop-termination condition; inspect the result when that distinction
  matters.

---

#### 8.5.9 Deadline Primitive (Timeout Abstraction)

The shipped deadline API is:

```c
CCDeadline cc_deadline_none(void);
CCDeadline cc_deadline_after_ms(uint64_t ms);
bool cc_deadline_expired(const CCDeadline* d);
void cc_cancel(CCDeadline* d);
```

**Usage Pattern:**

```c
@with_deadline(seconds(5)) as active {
    if (cc_deadline_expired(active)) return cc_err(Error_Timeout);
    int rc = cc_chan_match_select(cases, ncases, &ready,
                                  cc_current_deadline());
    if (rc == ETIMEDOUT) return cc_err(Error_Timeout);
}
```

**Semantics:**

- `@with_deadline(ms) { ... }` constructs a deadline relative to the current
  clock with `cc_deadline_after_ms(ms)`, pushes it while the block executes,
  and restores the previous deadline on every exit. `ms` is a duration in
  milliseconds (`seconds`, `millis`, `micros`).
- `@with_deadline(dl) { ... }` when `dl` is a `CCDeadline*` pushes that
  object. It does not allocate a clock. The same `dl` may be named in
  several arms or re-pushed so operations that consult
  `cc_current_deadline()` see it.
- `@with_deadline(x) as handle { ... }` binds a `CCDeadline*` to whichever
  object that block pushed.
- Expiry is observed only by operations that accept or consult that deadline.
- `cc_cancel(&d)` and clock expiry make the same `CCDeadline` object expired;
  neither action cancels an unrelated nursery.

**Deadline propagation:**

A child observes a deadline only when the task/nursery construction API copies
or passes that deadline into the child context. Lexical nesting alone does not
invent descendant cancellation. Observation follows §4.2.2.

`@with_deadline` supplies ambient state; it does not inject checks into every
`@await`. Use `cc_deadline_expired` for an explicit poll or pass
`cc_current_deadline()` to an operation that accepts a deadline.

---

#### 8.5.10 Shielded Regions (`@with_shield`)

`@with_shield`, `@cancel_safe`, and `@cancel_unsafe` are unsupported spellings
and are compile-time errors. Code that must defer observation calls an
operation that does not observe the relevant source, then explicitly checks
that source at the required boundary.

### 8.6 Streaming

Streaming uses explicit channel parameters:

```c
@async void produce(int n, int[~ >]* out) {
    @defer out.close();  // closes the shared channel state when production finishes
    for (int i = 0; i < n; i++) {
        @await out.send(i);
    }
}

@async void consume() {
    int[~10 >] tx;
    int[~10 <] rx;
    CCChan* ch = cc_channel_pair(&tx, &rx) !> @destroy { cc_channel_free(ch); };

    CCNursery n = cc_nursery_create() !> @destroy { tx.close(); };
    n.spawn(() => produce(100, &tx));
    int x;
    while (cc_io_avail(rx.recv(&x))) use(x);
}
```

**Streaming with errors:**

```c
// Fail-fast: function can fail, channel carries plain values
@async void!>(IoError) read_lines(char[:] path, char[:][~]* out) {
    @defer out.close();
    File f = open(path) !>(e) return cc_err(e);
    while (true) {
        char[:] line = f.readline() !>(e) return cc_err(e);
        if (line.len == 0) break;        // EOF: readline returns an empty slice
        @await out.send(line);
    }
}

// Per-item errors: each item can independently fail
@async void parse_nums(char[:][~]* in, int!>(ParseError)[~]* out) {
    @defer out.close();
    char[:] line;
    while (cc_io_avail(@await in.recv(&line))) {
        @await out.send(parse_int(line));
    }
}
```

---

### 8.7 Runtime API

```c
// Task control
CCTaskIntptr task = async_fn(args);
CCFutureStatus cc_task_intptr_poll(CCTaskIntptr* task,
                                   intptr_t* out, int* err);
void cc_task_intptr_cancel(CCTaskIntptr* task);
void cc_task_intptr_free(CCTaskIntptr* task);
intptr_t cc_block_on_intptr(CCTaskIntptr task);

// Nursery cancellation
void cc_nursery_cancel(CCNursery n);
bool cc_nursery_is_cancelled(const CCNursery n);
bool cc_cancelled(void);  // current nursery
void cc_nursery_leave(CCNursery n);  // UFCS: n.leave() — OPEN → LEFT; EMPTY frees
CCResult_void_CCError cc_nursery_leave_with(CCNursery n, void* ctx, void (*finish)(void*));  // n.leave(ctx, finish)
CCResult_void_CCError cc_nursery_close(CCNursery n, CCChanTx tx);  // n.close(tx) — arm EMPTY to close tx
/* Deprecated — docs/deprecated.md */
void cc_nursery_abandon(CCNursery n);
CCResult_void_CCError cc_nursery_on_last(CCNursery n, void* ctx, void (*finish)(void*));
CCResult_void_CCError cc_nursery_close_on(CCNursery n, CCChanTx tx);

// Deadline cancellation and polling
void cc_cancel(CCDeadline* d);
bool cc_is_cancelled(const CCDeadline* d);
CCDeadline* cc_current_deadline(void);
bool cc_deadline_expired(const CCDeadline* d);

```

**Rule:** `cc_block_on_intptr` blocks the OS thread and consumes the task. It is
used at synchronous runtime boundaries, not from an `@async` frame.

**Rule (blocking-join re-entrancy):** `cc_block_on` and
`cc_block_on_intptr` must not be called from a worker
executing a `cc_run_blocking_task_intptr` task or from any thread currently
executing runtime-managed tasks.

**Detection and behavior:**

- **Debug builds:** Runtime detects a re-entrant blocking join and traps with a diagnostic.
- **Release builds:** Undefined behavior (likely deadlock or scheduler corruption).

**Example (WRONG):**

```c
@async void f() {
    g();            // edge mode @blocking → cc_run_blocking_task_intptr
}

void g() {
    CCTaskIntptr t = async_work();
    int result = (int)cc_block_on_intptr(t); // ERROR: blocking join on pool worker
}
```

**Example (RIGHT):**

```c
@async void f() {
    // Option 1: avoid a blocking join by using @await
    int result = @await async_work();  // preferred
    
    // Option 2: move cc_block_on_intptr to a sync boundary
    // (do not call it from inside @async, even via nesting)
}

// CORRECT: block at a sync boundary (not inside @async or run_blocking)
void sync_boundary() {
    CCTaskIntptr t = async_work();
    int result = (int)cc_block_on_intptr(t); // OK at a sync boundary
    use(result);
}
```

---

#### 8.7.1 Deadlock Detection

**Problem:** A single-task `cc_block_on` context can deadlock if its task waits
on a channel peer that is not running.

```c
// DEADLOCK: buffer=4, sends=5, no concurrent consumer
int[~4 >] tx;
cc_block_on(void, producer(tx, 5));  // Hangs forever on 5th send
```

**Contract:** Deadlock detection is a **runtime** service. Compile-time coverage is deliberately narrow — exactly two checks.

#### Compile-time checks

1. **`@closing(...)` is rejected.** The diagnostic directs the caller to an
   explicit nursery:

   ```
   error: async: `@closing(...)` is unsupported; use an explicit nursery and
   `@destroy { chan.close(); }` or `n.close(chan)`
   ```

2. **`cc_block_on` heuristic warning.** `cc_block_on(T, f(...))` where `f` is an `@async` function that performs channel operations inside a loop and is not marked `@nonblocking` produces a warning:

   ```
   warning: cc_block_on with 'f' may deadlock
   note: 'f' has channel ops in a loop; consider explicit nursery concurrency
   or a larger buffer
   ```

   This is a heuristic, not a proof. Marking the function `@nonblocking` suppresses the warning; the compiler does not verify the annotation.

There is no general compile-time deadlock analysis. In particular, a consumer that receives inside the nursery that owns a channel's `close` compiles cleanly and deadlocks only at runtime; the fix is to move the consumer outside the owning nursery scope.

#### Runtime detection

The scheduler's monitor detects a deadlock when every worker thread is idle and internally parked fibers exist with no progress across a full stall interval (on the order of one second). On detection the runtime prints a diagnostic dump — worker and fiber counts, and each internally parked fiber with its park reason and the state of the channel it is parked on — and exits with code 124 (like `timeout`), so stuck programs surface in CI instead of hanging.

- Fibers inside `cc_external_wait_enter/leave` or `cc_deadlock_suppress_enter/leave` scopes are excluded from the verdict; an external wait is not a deadlock.
- `CC_DEADLOCK_ABORT=0` downgrades the exit to a warning: the dump prints and the (deadlocked) program keeps running, which allows log capture.
- `CC_NURSERY_CLOSING_RUNTIME_GUARD=1` (opt-in): a recv that would wait forever on a channel whose `close` owner is the current nursery fails with `EDEADLK` instead of deadlocking.

Detection is best-effort: it cannot see deadlocks involving resources outside the runtime (other processes, foreign locks).

#### Task Combinators

Concurrent-C provides JavaScript-style task combinators for running multiple async tasks from sync code.

##### `cc_block_all` - Wait for All

Runs all tasks concurrently, waits for all to complete:

```c
CCTaskIntptr tasks[] = {
    fetch_user(id),
    fetch_posts(id),
    fetch_friends(id)
};

intptr_t results[3];
int err = cc_block_all(3, tasks, results);

if (err == 0) {
    User* user = (User*)results[0];
    Posts* posts = (Posts*)results[1];
    Friends* friends = (Friends*)results[2];
}
```

**Semantics:** Like JavaScript's `Promise.all()` - fails fast if any task errors.

##### `cc_block_race` - First to Complete

Returns as soon as the first task completes (success or failure):

```c
CCTaskIntptr tasks[] = {
    fetch_from_primary(),
    fetch_from_backup()
};

int winner;
intptr_t result;
cc_block_race(2, tasks, &winner, &result);

printf("Task %d finished first with result %ld\n", winner, (long)result);
```

**Semantics:** Like JavaScript's `Promise.race()`. Use cases:

- Timeout patterns (race task against timer)
- Redundant requests (first response wins)
- Speculative execution

##### `cc_block_any` - First Success

Returns first successful task; only fails if ALL tasks fail:

```c
CCTaskIntptr tasks[] = {
    try_cdn_1(),
    try_cdn_2(),
    try_cdn_3()
};

int winner;
intptr_t result;
int err = cc_block_any(3, tasks, &winner, &result);

if (err == 0) {
    printf("CDN %d responded with %ld\n", winner, (long)result);
} else {
    printf("All CDNs failed\n");
}
```

**Semantics:** Like JavaScript's `Promise.any()`. Use cases:

- Fallback chains
- Best-effort from multiple sources
- Load balancing with retry

##### API Reference

```c
// Wait for ALL tasks. Returns 0 on success.
// Results stored in results array (must have count elements).
int cc_block_all(int count, CCTaskIntptr* tasks, intptr_t* results);

// Wait for FIRST task to complete. Returns 0 on success.
// winner: index of completing task. result: its return value.
int cc_block_race(int count, CCTaskIntptr* tasks, int* winner, intptr_t* result);

// Wait for first SUCCESS. Returns 0 if any succeeded, ECANCELED if all failed.
// winner: index of first success. result: its return value.
int cc_block_any(int count, CCTaskIntptr* tasks, int* winner, intptr_t* result);
```

##### Comparison with JavaScript


| Concurrent-C    | JavaScript           | Behavior                          |
| --------------- | -------------------- | --------------------------------- |
| `cc_block_all`  | `Promise.all`        | Wait for all, fail fast           |
| `cc_block_race` | `Promise.race`       | First to complete wins            |
| `cc_block_any`  | `Promise.any`        | First success wins                |


##### Entry Points Comparison


| Entry Point     | Use Case                | Concurrent Tasks | Deadlock Risk           |
| --------------- | ----------------------- | ---------------- | ----------------------- |
| `cc_block_on`   | Single async task       | 1                | Yes if not @nonblocking |
| `cc_block_all`  | Fan-out/fan-in          | N                | No                      |
| `cc_block_race` | First wins              | N                | No                      |
| `cc_block_any`  | First success           | N                | No                      |
| `CCNursery`     | Inside @async functions | N                | No                      |


---

#### 8.7.2 Blocking Thread Pool

`cc_run_blocking_task` submits a closure to a lazily created bounded executor.
The worker count is `CC_BLOCKING_WORKERS` (default: the online CPU count below
four, otherwise four). Queue capacity is `CC_BLOCKING_QUEUE_CAP` (default:
256).

Submission applies backpressure when the queue is full. Allocation, executor
creation, or submission failure returns an invalid task. Submitted work is not
preempted. `cc_blocking_pool_stats` reports executor counters and the number of
failed submissions.

---

#### 8.7.3 Standard Error Types

Runtime and stdlib errors are ordinary Result error values. Their concrete C
types, constructors, and API use are specified in
`spec/concurrent-c-stdlib-spec.md`.

---

### 8.8 Execution Model (Normative)

This section defines how Concurrent-C classifies potentially blocking operations and how they interact with `@async` execution.

#### 8.8.1 Definitions

**Blocking:** An operation is blocking if it may suspend the calling thread for a non-zero duration.

**Stalling:** An operation is stalling if it may block for an unbounded or externally-dependent duration, including but not limited to:

- file I/O on pipes or special files
- network I/O
- reading from standard input
- synchronization waiting on external actors

**Pure (Non-Blocking):** An operation is pure if it:

- does not perform I/O
- does not wait on synchronization primitives
- and does not block except for bounded CPU execution

#### 8.8.2 Default Classification Rule

All non-`@async` functions are conservatively treated as potentially blocking.

This includes:

- user code
- library code
- and foreign function calls

**Rule:** This classification applies transitively — if a function calls a potentially blocking function, it is itself potentially blocking.

#### 8.8.3 @async Execution Rule

Calling a non-`@async` function from within an `@async` function must not block the async scheduler.

To satisfy this rule (see §8.2 / Appendix J.1.1):

- Each such call edge is resolved to mode `@blocking` or `@nonblocking` by the four-step precedence chain (call site → callee decl → caller ambient → FFI/fallback default). `@noblock` is the compatibility spelling of `@nonblocking`.
- `@blocking` edges construct `cc_run_blocking_task_intptr` tasks and use the
  ordinary child-task poll path.
- `@nonblocking` edges compile to a direct C call — the callee is contractually non-blocking (§8.2.7).
- Adjacent `@blocking` edges MAY be coalesced into a single dispatch (Appendix C.1).

**Coalescing Semantics:** Consecutive non-`@async` calls within the same lexical scope may be dispatched as one blocking unit. If an error occurs (exception, early return, propagated error), remaining calls in the unit are not executed.

#### 8.8.3.1 @latency_sensitive Annotation

A function annotated `@latency_sensitive` asserts that it must not experience unexpected latency from blocking dispatch coalescing.

**Valid placement:** `@latency_sensitive` is only meaningful on `@async` functions. Using it on sync functions is invalid (compile error).

```c
@async @latency_sensitive void handler(Request req) {  // ✅ Valid
    // ...
}

@latency_sensitive void sync_func() {  // ❌ ERROR: meaningless without @async
    // ...
}
```

**Semantics:**

```c
@async @latency_sensitive void handle_request(Request req) {
    char[:] parsed = parse(req.body);  // CPU work: runs inline
    log_audit(parsed);                 // Stalling I/O: separate dispatch
    send_response(req.fd, "OK");       // More stalling I/O: separate dispatch
}
```

**Rules:**

- `@latency_sensitive` functions may contain both CPU work and stalling I/O.
- The compiler must **not** coalesce stalling calls within `@latency_sensitive` functions.
- CPU work (pure, non-blocking operations) may be inlined and combined with other CPU work.
- Each stalling operation is dispatched separately to the blocking executor.
- Typical use: request handlers where latency SLA is critical.

**Guarantee:**

```
For @latency_sensitive functions:
- CPU operations run inline (0 dispatch overhead)
- Stalling operations are dispatched individually (predictable, observable latency)
- No surprise coalescing that would hide I/O latency behind CPU operations
```

**Trade-off:**

This may increase dispatch overhead (more blocking executor calls) but provides **latency predictability**. Use in request handlers and latency-critical paths.

#### 8.8.3.2 Linting Rule for @latency_sensitive

The compiler (translator) enforces a lint rule to catch latency violations:

**Rule:** `@latency_sensitive` functions can only call:

- `@nonblocking` functions (guaranteed non-blocking, inline)
- `@async` functions (must be awaited)
- Any function within `@await` context

**Violations (Compiler Warning/Error):**

Calling a non-`@async`, non-`@nonblocking` function without `@await` in a `@latency_sensitive` function is a compiler error or warning (depending on lint level).

**Example:**

```c
@nonblocking int parse_count(char[:] s);    // OK to call directly

@async void db_query(int count);        // Must be awaited

void process_logs(int count);           // Must be awaited or marked @nonblocking

@async @latency_sensitive void handler(Request req) {
    int count = parse_count(req.body);  // OK (@nonblocking, guaranteed fast)

    @await db_query(count);              // OK (awaited)

    process_logs(count);                // ERROR: blocking call in @latency_sensitive

    // Fix: Either @await it or mark it @nonblocking
}
```

The rule prevents an unclassified blocking call from violating the latency
guarantee.

#### 8.8.4 Blocking Executor Constraints

Blocking work is executed on a bounded blocking executor.

**Normative Requirements:**

- The executor must be bounded.
- Saturation must not deadlock the async runtime.
- If work cannot be scheduled due to saturation, the operation must fail deterministically.

**Saturation Behavior:**

- When the queue reaches `max_queue` capacity, new operations return `err` with `CC_ERR_WOULD_BLOCK` (Busy) immediately without queueing.
- Work already queued or in-flight continues to completion.
- The queue is FIFO; starvation is possible under sustained saturation.

#### 8.8.5 Stall Awareness

Operations that may stall indefinitely must be explicitly classified as such.

**Stalling Operations (by definition):**

- file open/read/write/sync
- stream reads
- any OS or FFI operation whose completion depends on external actors

**Guarantees:**

- Stalling operations may be offloaded to the blocking executor
- May fail with `CC_ERR_WOULD_BLOCK` if capacity is exhausted
- Have no guarantee of cancellation or bounded latency

#### 8.8.6 `@nonblocking` Contract

A function annotated `@nonblocking` asserts that it will never block or stall. `@noblock` is a compatibility spelling for the same annotation.

**Rules:**

- `@nonblocking` functions must not perform I/O, synchronization waits, or call non-`@nonblocking` functions
- The compiler must not wrap calls to `@nonblocking` functions when invoked from `@async`
- Violations detected at compile-time are errors

**Runtime Violations:**

- Debug builds: runtime trap with diagnostic
- Release builds: undefined behavior (likely deadlock or latency spike)

This annotation exists to allow high-confidence opt-out from conservative blocking assumptions.

#### 8.8.7 Standard Library Guarantees

**Pure Operations (non-blocking, never stall):**

- string and slice operations
- `String` builder operations (excluding allocation failure)
- `Vec` and `Map` operations (excluding allocation failure)

**Rules:**

- Must not be offloaded to the blocking executor
- Must not stall
- May only fail due to allocation exhaustion

**I/O Operations (all stalling):**

- All file and stream operations
- May block indefinitely
- May fail with `CC_ERR_WOULD_BLOCK`
- Subject to executor saturation rules

#### 8.8.8 Cancellation and Progress

Blocking and stalling operations provide no cancellation or progress guarantees.

**Specifically:**

- Cancellation requests are signals only; they do not forcibly preempt in-flight work
- In-flight blocking work may:
  - complete normally after cancellation is requested
  - fail with an error unrelated to cancellation
  - continue running on the blocking executor thread even after the task is cancelled
- Programs requiring strict latency bounds must avoid stalling operations in
  critical paths and use operation-specific deadline APIs.

#### 8.8.9 Interaction with Nurseries

When a spawned task stalls on I/O, the nursery continues scheduling other work. The nursery scope does not complete until all spawned tasks complete (including any offloaded blocking work). Nursery cancellation requests are propagated to in-flight work but provide no hard latency guarantees (§8.8.8).

#### 8.8.10 Classification principle

Latency control is explicit. Classification distinguishes bounded from
unbounded waits. `@nonblocking` is a checked assertion, not suppression of
blocking checks.

---

Deadlock detection, diagnostics, exclusions, and the
`CC_DEADLOCK_ABORT` control are specified in §8.7.1.

---

### 8.9 Error handling in async and nurseries

Errors in Concurrent-C are **value-based**, not exceptions. `T!>(E)` is the return type for functions that can fail; unwrap and handling syntax is defined normatively in §3.1 (`?>`, `!>`, `@err(e);`, `@errhandler`). `@defer` always runs; there is no unwinding.

Error handling in `@async` functions and nurseries uses the operators defined in **§3.1** (`?>` expression, `!>` statement, `@err(e);` forward, `@errhandler` registration). No `@await`- or nursery-specific error construct exists; everything composes through the same surface.

**Async call with default.**

```c
@async int!>(IoError) fetch(char[:] url);

@async void handler(char[:] url) {
    int len = (@await fetch(url)) ?> 0;   // default on error
    use(len);
}
```

**An error in the parent while a nursery is live.**

```c
@async int!>(IoError) process(char[:] url) {
    CCNursery n = cc_nursery_create() !> @destroy;
    n.spawn(() => subtask_a(url));
    int v = (@await subtask_b(url)) !>(e) return cc_err(e);  // cleanup joins; it does not cancel siblings
    return cc_ok(0);
}
```

**Mapping between error types.**

```c
int!>(AppError) parse_with_app_error(char[:] s) {
    int!>(ParseError) r = parse_int(s);
    return cc_is_ok(r) ? cc_ok(cc_value(r))
                       : cc_err(AppError.Parse(cc_error(r)));
}

int!>(AppError) pipeline(char[:] path) {
    char[:] s = read_with_app_error(path) !>(e) return cc_err(e);
    return parse_with_app_error(s);
}
```

For bail-out without a value (statement context), use `!>` with an `@errhandler` or a local `!> (e) BODY`. See §3.1 for the full grammar and semantics.

---

### 8.10 Named Exclusive Sections (`CCExclusive`)

When several fibers must briefly mutate the same named resource, a **named exclusive section** (`CCExclusive`) provides per-name mutual exclusion within one domain. Prefer channels and single-writer ownership when possible.

Each `CCExclusive` is its own name space: the same `uint64_t` name in one domain resolves to the same mutex; different domains never collide.

**Rule (short critical sections):** Critical sections under an exclusive guard must be short. Do not `@await`, park, or otherwise suspend while holding a guard. Holding a guard stalls other waiters on that name.

#### 8.10.1 Construction and storage

Construction allocates the section header and discovery map from a caller-supplied `CCArena` (handle by value). Mutex entries are allocated from an arena pool on that same arena on first resolve. The section stores a copy of the handle, not a pointer to the caller's binding.

```c
CCArena arena@(kilobytes(128)) @destroy;
CCExclusive excl = cc_exclusive_create(arena, 0) !>;     // default map (64)
// or: excl = cc_exclusive_create(arena, 256) !>;         // initial map hint
// UFCS: arena.create_exclusive(0) !>
```

- `cc_exclusive_create(arena, initial_cap)` rounds `initial_cap` up to the next power of two (minimum 2). `initial_cap == 0` selects the default capacity (64).
- A dead handle or allocation failure is `CC_ERR_INVALID_ARG` / `CC_ERR_OUT_OF_MEMORY`.

The discovery map is an open-addressing table keyed by `uint64_t` name. It grows under an internal create mutex when load is high (approximately 75% full): capacity doubles, live entries are rehashed, and the prior table is retired. The live map pointer is `_Atomic`: grow release-stores the new table after rehash, and lock-free lookups acquire-load that pointer before probing buckets. Retired tables are released with `cc_arena_release` at `cc_exclusive_destroy`, not at grow time, so lookups never observe a freed table.

#### 8.10.2 Mutex resolve

Resolve a name once and reuse the handle:

```c
CCExclusiveMutex m = excl.mutex(name);   // UFCS: cc_exclusive_mutex(excl.e, name)
```

`cc_exclusive_mutex` returns a `CCExclusiveMutex` carrying the section pointer, the name, and a cached runtime entry pointer. The first resolve for a name allocates a 64-byte-aligned entry (one cache line per entry, for false-share isolation) from the section's arena pool and inserts it into the discovery map. Subsequent resolves of the same name in the same domain return the same entry.

For hot loops, resolve once outside the loop rather than calling `excl.acquire(name)` each iteration (which resolves on every call).

#### 8.10.3 Acquire and release

The surface uses **acquire** / **release**, not lock / unlock:

```c
CCExclusiveGuard g = m.acquire();   // UFCS: cc_exclusive_mutex_acquire(&m)
... short critical section ...
g.release();                      // UFCS: cc_exclusive_guard_release(&g)
```

By-name acquire is also available:

```c
CCExclusiveGuard g = excl.acquire(name);  // UFCS: cc_exclusive_acquire(excl.e, name)
```

Multi-name acquire is deadlock-safe: names are always taken in ascending order.

```c
CCExclusiveGuard gs[8];
size_t n = excl.acquire_sorted(names, count, gs, 8);
  // UFCS: cc_exclusive_acquire_sorted(excl, names, count, gs, 8)
size_t n = excl.acquire_range(0, shard_count, gs, 8);  /* [lo, hi) */
  // UFCS: cc_exclusive_acquire_range(excl, 0, shard_count, gs, 8)
cc_exclusive_guards_release(gs, n);
```

`acquire_sorted` accepts unsorted input and deduplicates; both return `0` (no
locks held) when the unique name count exceeds `out_cap` or
`CC_EXCLUSIVE_ACQUIRE_MULTI_MAX` (64). Partial acquires are rolled back.

**Admitted builders (`_into`).** Each acquire shape has an `_into` form that
runs a builder under the held names instead of returning guards — the
exclusive twin of `send_into` (§7.4): admit the name set, run
`builder(slot, arena)` exactly once, release. No guards escape the call.

```c
Reply r;
bool ran = excl.acquire_into(name, &r, arena,
    (Reply* slot, CCArena a) => [req] {
        *slot = compute(req, a);   /* own the result before returning */
        return NULL;
    });
bool ran = excl.acquire_sorted_into(names, count, &r, arena, builder);
bool ran = excl.acquire_range_into(lo, hi, &r, arena, builder);  /* [lo, hi) */
```

The builder is an ordinary `CCClosure2`; builder closure literals lower as
in `send_into`. The call returns `true` iff the builder ran. `false` means
admission failed (invalid arguments, more than
`CC_EXCLUSIVE_ACQUIRE_MULTI_MAX` unique names, or `hi < lo`) and the builder
never ran: no locks are held and the slot is untouched — never a half state.
An empty admission (`count == 0` or `hi == lo`) runs the builder once with
no names held. The call consumes the builder either way: it is run exactly
once, or dropped without running (its environment is released).

**Rule (builder contract):** The builder is synchronous and must not suspend
(the critical-section rule applies to the builder body). When it returns,
`*slot` is fully constructed and nothing reachable from `*slot` aliases
state guarded by the held names: owning the result happens inside the
builder, before release. `arena` is passed through to the builder for owning
copies of the result; it may be `NULL` when the builder does not allocate.

**Rule (idempotent release):** `g.release()` is idempotent. After the first release, the guard's entry pointer is cleared to `NULL`; a second `release()` or `destroy()` on the same guard is a local no-op and does not touch the lock word. This is intentional so end-of-hold cleanup does not read as a double-unlock bug in review.

`g.destroy()` and `@destroy` on a guard are aliases for `g.release()`.

An acquire blocks until the caller owns the named entry. Uncontended acquire is an inlined compare-and-swap on the entry lock word; contended acquire uses a slow path that may park the current fiber.

#### 8.10.3.1 Conditioned acquire (`acquire_when`)

`acquire_when` is acquire gated on a predicate. The caller does not hold the name on entry. The implementation acquires, evaluates `pred` under the hold, and either returns holding or enqueues as a condition waiter, releases, and parks. On success the guard is held and `pred` was true under that hold.

```c
CCExclusiveGuard g = excl.acquire_when(name, pred, env) !> @destroy;
m.acquire_when(pred, env) !>;
excl.acquire_when_into(name, pred, env, &slot, arena, builder) !>;
```

`pred` is `int (*)(void* env)` (nonzero = true). It runs only while the name is held and must not suspend. Wake means retry, not “still true.” Anyone who makes `pred` become true signals that name **while still holding**:

```c
CCExclusiveGuard h = excl.acquire(name);
/* mutate so pred may be true */
h.signal();     /* one waiter */
h.broadcast();  /* every waiter */
h.release();
```

`signal` / `broadcast` wake condition waiters only (not lock waiters). The waiter is enqueued before release so a signal cannot land on an empty list in the gap between “pred is false” and park.

`wait_release` parks a fiber (`CC_FIBER_PARK`) or an OS thread (futex / ulock on the waiter). Plain `acquire` may spin on an OS thread; conditioned wait parks. The wait consults `cc_current_deadline()` (`@with_deadline` or `cc_deadline_push`): an expired deadline, including a remaining time of zero, is `CC_ERR_TIMEOUT` without parking. A cancelled deadline, or a cancelled current nursery when one exists, is `CC_ERR_CANCELLED`. `cc_cancelled()` is true when there is no current nursery; that is not cancel of this wait. Timeout and cancel return not holding.

| Result | Holding? | Meaning |
|--------|----------|---------|
| `cc_ok(g)` / `cc_ok()` | yes, then released by `_into` | `pred` observed true under the hold |
| `cc_err(CC_ERR_CANCELLED, …)` | no | parked wait cancelled; no guard escapes |
| `cc_err(CC_ERR_TIMEOUT, …)` | no | current deadline expired; pred still false |
| `cc_err(CC_ERR_INVALID_ARG, …)` | no | null domain or null `pred` |

`acquire_when_into` runs the builder once under that success invariant and releases; the builder never runs on error (slot untouched). The builder contract is the same as `acquire_into` (synchronous, no suspend, own-before-release).

Do not use this when the condition is “a message arrived” — that is a channel. `acquire_when` gates a short mutation of named shared state.

#### 8.10.4 Hash-shard geometry (`CCShardMask`)

Compose `CCExclusive` names `0 .. count-1` with a power-of-two shard mask:

```c
CCShardMask shards = cc_shard_mask_auto(64);       /* next pow2(ncpu), then clamp */
size_t i = shards.index(hash);                     /* hash & mask */
CCShardMask m = cc_shard_mask_ceil(n, max);        /* ceil n to pow2, then clamp */
CCShardMask m = cc_shard_mask_clamp(n, max);       /* floor n to pow2 in 1..max */
CCShardMask m = cc_shard_mask_make(8);             /* exact pow2, else {0,0} */
```

This is the Concurrent-C concurrent-map spine: N maps (or map pairs) + exclusive
names `0..N-1` + `shards.index(key_hash)`. Hold policy (one key / key set /
all shards) stays at the application layer.

#### 8.10.5 Explicit mutex free

```c
m.free();   // UFCS: cc_exclusive_mutex_free(&m)
```

Explicit free removes the name from the discovery map (leaving a tombstone for probing) and returns the entry to the section's arena pool. It requires the mutex not be held and no waiters queued; violating this aborts. Other `CCExclusiveMutex` handles that still reference the freed name become invalid. Freeing an already-cleared handle is a no-op. The same name may be resolved again afterward (possibly reusing the pooled entry).

#### 8.10.6 Destroy and arena lifetime

```c
excl.destroy();   // UFCS: cc_exclusive_destroy(excl)
```

Destroy tears down the internal create mutex and releases discovery-map tables (current and retired) via `cc_arena_release`. Pooled mutex entry storage remains allocated until the caller's arena is freed or reset.

**Rule (arena lifetime):** The caller must keep the supplying arena alive for the entire lifetime of the section. Do not `cc_arena_reset` or `cc_arena_free` the arena while a `CCExclusive` built from it is still live.

#### 8.10.7 Lock semantics

Each mutex entry's lock word uses three states:

| Value | Constant              | Meaning                                      |
| ----- | --------------------- | -------------------------------------------- |
| 0     | `CC_EXCL_FREE`        | Unlocked                                     |
| 1     | `CC_EXCL_LOCKED`      | Locked, no known waiters                     |
| 2     | `CC_EXCL_CONTENDED`    | Locked; waiters may be queued                |

The lock word is the first field of the runtime entry; a guard holds a single entry pointer and the inline fast paths cast it directly.

- **Uncontended acquire:** compare-and-swap `FREE → LOCKED`.
- **Release:** atomic swap to `FREE`; if the previous value was `CONTENDED`, wake exactly one queued waiter.
- **Contended path:** uses a barging wake protocol — the lock stays available while a woken fiber is being scheduled; the woken fiber re-contends rather than inheriting ownership directly.

In fiber context, contended waiters park on the scheduler after a bounded spin. Outside fiber context (plain OS threads with no fiber runtime), acquire spins until the lock is taken.

**Runtime API (normative):**

```c
CCExclusive cc_exclusive_create(CCArena arena, size_t initial_cap); /* Result */
void cc_exclusive_destroy(CCExclusive* excl);

CCExclusiveMutex cc_exclusive_mutex(CCExclusive* excl, uint64_t name);
void cc_exclusive_mutex_free(CCExclusiveMutex* m);

CCExclusiveGuard cc_exclusive_mutex_acquire(CCExclusiveMutex* m);
CCExclusiveGuard cc_exclusive_acquire(CCExclusive* excl, uint64_t name);
size_t cc_exclusive_acquire_sorted(CCExclusive* excl, const uint64_t* names,
                                   size_t count, CCExclusiveGuard* out,
                                   size_t out_cap);
size_t cc_exclusive_acquire_range(CCExclusive* excl, uint64_t lo, uint64_t hi,
                                  CCExclusiveGuard* out, size_t out_cap);
bool cc_exclusive_acquire_into(CCExclusive* excl, uint64_t name,
                               void* slot, CCArena arena, CCClosure2 builder);
bool cc_exclusive_acquire_sorted_into(CCExclusive* excl, const uint64_t* names,
                                      size_t count, void* slot, CCArena arena,
                                      CCClosure2 builder);
bool cc_exclusive_acquire_range_into(CCExclusive* excl, uint64_t lo, uint64_t hi,
                                     void* slot, CCArena arena,
                                     CCClosure2 builder);
void cc_exclusive_guards_release(CCExclusiveGuard* guards, size_t n);
void cc_exclusive_guard_release(CCExclusiveGuard* g);
void cc_exclusive_guard_destroy(CCExclusiveGuard* g);

CCShardMask cc_shard_mask_make(size_t count);
CCShardMask cc_shard_mask_clamp(size_t n, size_t max);
CCShardMask cc_shard_mask_ceil(size_t n, size_t max);
CCShardMask cc_shard_mask_auto(size_t max);
size_t cc_shard_mask_index(const CCShardMask* m, uint64_t hash);

void cc_exclusive_lock_entry_slow(void* entry);      /* slow acquire path */
void cc_exclusive_unlock_contended(void* entry);     /* wake one lock waiter */
int cc_exclusive_guard_wait_release(CCExclusiveGuard* g);
void cc_exclusive_guard_signal(CCExclusiveGuard* g);
void cc_exclusive_guard_broadcast(CCExclusiveGuard* g);
CCExclusiveGuard !>(CCError) cc_exclusive_acquire_when(CCExclusive* excl,
    uint64_t name, CCExclusivePred pred, void* env);
CCExclusiveGuard !>(CCError) cc_exclusive_mutex_acquire_when(
    CCExclusiveMutex* m, CCExclusivePred pred, void* env);
void !>(CCError) cc_exclusive_acquire_when_into(CCExclusive* excl, uint64_t name,
    CCExclusivePred pred, void* env, void* slot, CCArena arena,
    CCClosure2 builder);
void !>(CCError) cc_exclusive_mutex_acquire_when_into(CCExclusiveMutex* m,
    CCExclusivePred pred, void* env, void* slot, CCArena arena,
    CCClosure2 builder);
```

**UFCS surface (normative):**

- `excl.mutex(name)` — resolve
- `excl.acquire(name)` — resolve and acquire
- `excl.acquire_sorted(names, count, out, out_cap)` — unique ascending multi-acquire
- `excl.acquire_range(lo, hi, out, out_cap)` — contiguous ascending multi-acquire
- `excl.acquire_into(name, slot, arena, builder)` — admitted builder, one name
- `excl.acquire_sorted_into(names, count, slot, arena, builder)` — admitted builder, name set
- `excl.acquire_range_into(lo, hi, slot, arena, builder)` — admitted builder, name range
- `excl.acquire_when(name, pred, env)` — acquire when `pred` is true under the name
- `excl.acquire_when_into(name, pred, env, slot, arena, builder)` — same, admitted builder
- `excl.destroy()` — tear down section
- `m.acquire()` — acquire resolved mutex
- `m.acquire_when(pred, env)` / `m.acquire_when_into(pred, env, slot, arena, builder)`
- `m.free()` — explicit reclaim
- `g.release()` / `g.destroy()` — release guard
- `g.signal()` / `g.broadcast()` — wake condition waiters (while holding)
- `shards.index(hash)` — `CCShardMask` routing

---

### 8.11 `@parallel`

`@parallel` names a join of independent work. It is not a nursery. The brace form is `CCParallel !>(CCError)`: create can fail; `.wait()` is the join. The implementation may run some arms or iterations on other workers, or run all of them on the caller. `n.spawn` does not sequentialize; `@parallel` may.

The form is selected by the tokens after `@parallel`: `{` (always try to spawn; spawned arms may be denied), `spawn` then `{` (meeting admit: spawned arms are not denied, §8.11.7), `(name) { … }` with no `!>` (admit onto dest `name`; a statement, not a Result), `(` or `seq (` (spawn if the predicate, §8.11.3, §8.11.5), `wait (` (ordered spawn loop over a turnstile; an expression of type `bool !>(CCError)`, §8.11.6), or `@for` (bisected range, §8.11.4). `spawn` is a brace join; combining it with `for` or `wait` is ill-formed. Assignment join and `@parallel for` are `CCParallel !>(CCError)`. A statement consumes the Result and waits (`!>.wait()!>;`) or binds the handle (`CCParallel h = … !>;`). A bare construct is an unconsumed Result. The wait-for form is `bool !>(CCError)`.

`@parallel(h) { stmts }` admits a fiber onto live dest `h`. It is the growing form of dest: a name arrives after the brace. It is a statement: no `!>`, no dest bind. Spawned work is not denied. A later write of a snapshot name in the caller is not this fiber's object (accept `sock`, loop `x`). Pointer and array names copy the pointer. An atomic name is the caller's cell. Occupancy is who is still running. A finished admit is dropped before the next admit; the live index grows if it must (OOM is a programming error). `CC_PARALLEL_TASK_MAX` is brace width and the dest's inline pad, not a cap on this form. `h.wait()` joins the dest's arms and whoever is still admitted. Admit after join or leave is a programming error. Admit after cancel fails (`CC_ERR_CANCELLED`); dest-attach unwraps into the enclosing `@errhandler`. The same tokens with `!>` are the predicate join (§8.11.3).

**Captures.** A body or arm may use locals of the enclosing frame. A pointer-typed name is captured by copying the pointer; uses inside the body see that pointer value, and writes through it hit the pointee. An array name copies the decayed element pointer. An atomic name is the caller's cell. A `CCParallel` name is the caller's dest. Dest-live plant (a dest-bound construct, and dest-attach) snapshots every other read-only name at pack: that copy is the kick's argument. A later write on the caller is not this arm's object. A name the arm assigns, increments, or address-takes stays the frame object. Immediate-wait joins (`!>.wait()!>`) capture those names by reference. A write through a reference capture is visible to the frame after `.wait()` and to concurrently running arms or iterations, subject to the construct's independence and ordering rules. Reference-captured objects must outlive `.wait()`. A dest-live handle does not extend those objects' storage; a pointer capture does not require the pointer slot to outlive `.wait()`, only the pointee. The loop index of a `for` form is a per-iteration value, not a capture. These rules are distinct from closure-literal capture lists (§2.2), which copy by value because a closure may escape its frame.

#### 8.11.1 Assignment join

```c
T a, b;
@parallel {
    a = f();
    b = g();
} !>.wait()!>;
```

The block holds one or more arms. An arm is `name = expr;` where `name` is a simple identifier already in scope, `@serial { … }` (§8.11.2), or an expression statement with no assignment (no named write; the expression runs). After `.wait()`, every named write is visible and every expression arm has completed.

Two handles join by waiting them as effect arms of a new `@parallel`:

```c
CCParallel h1 = @parallel { a = f(); b = g(); } !>;
CCParallel h2 = @parallel { c = p(); d = q(); } !>;
@parallel {
    h1.wait() !>;
    h2.wait() !>;
} !>.wait()!>;
```

`h1.adopt(h2)` links a cancel tree. `h1.cancel()` cancels adopted children (newest first), then `h1`. `h2.cancel()` cancels `h2` only. Adopt is not a move: both handles stay live. Self, cycle, a second parent, a joined parent or child, and a full child list are errors.

A dest bound to the construct (`CCParallel h = @parallel { … } !>;`) is that handle before any arm runs. Binding starts the arms and does not join. The construct plants the dest: `h.live()` is true until `h.wait()` sets `h.joined` or `h.leave()` consumes the handle. `h.live()` is handle lifetime (planted and not joined or left). It is not “the work is still running” and not “results are ready.” Right after a kick the wave can be finished and `h.live()` is still true. Outputs the frame reads without `.wait()` use a dest cell (`h.cancelled`, `h.paused`, an atomic, `@stage`): the worker writes the payload, then publishes with that cell as the last store. `cc_parallel_empty()` is idle (`!h.live()`). `#pragma(@parallel) off` goes idle→joined with no live window; that sequential lowering is the one-core dest-live test (`@smoke_inline`): finish can run on the caller before the next statement. Kick is `if (h.live()) return; h = @parallel { … } !>;` — after wait the next bind overwrites `h`. Do not infer lifetime from `h.n` / `h.nt`. When there is a kick, the first arm has finished on the caller when the construct returns `h`. Remaining arms may still be running. `h.wait()` joins them and synchronizes-with every arm: after it returns, those arms' writes to captured objects are visible. `@parallel { … } !>.wait()!>;` waits before the statement completes. A dest bound to a one-arm `@parallel` whose arm is an assignment is ill-formed: this dest is never live on the caller. A dest bound to a one-arm expression (`CCParallel h = @parallel { work(); } !>;`) is well-formed: that arm is the worker (spawned); dest is live on the caller. A dest bound to a one-arm `@serial` is the same: that arm is the worker, including when it assigns an outer name. A planted dest's workers do not run on the caller. Spawn failure or env oom aborts. `#pragma(@parallel) off` is the one-core sequential test. `@parallel { work(); } !>.wait()!>;` is well-formed. Reading another arm's destinations before `.wait()` returns is undefined. Arms may `h.cancel()`, `h.adopt(…)`, `h.pause()`, and `h.resume()`. `h.wait()` in an arm of `h` is ill-formed. The first arm of a multi-arm construct is not a task of `h`: cancel from the caller stops spawned siblings; the caller continues.

`h.close(tx)` is UFCS for `cc_parallel_close(h, tx)`. It arms this dest's EMPTY to close `tx` after `.wait()`, or on the LEFT path after `.leave()`, before dest storage for the join set is released. EMPTY closes this dest's join set. A sibling consumer on the same dest does not unblock at that EMPTY: the produce arm calls `tx.close()`, or an inner dest or nursery registers the close. Waiting this dest while it still owns the parked consumer does not fire that close. Wait-for dest (`h.n` is the construct's nursery) registers on that nursery. `h.leave()` is UFCS for `cc_parallel_leave` and consumes the handle without joining. `h.leave(ctx, finish)` registers one leftover that runs at EMPTY on the LEFT path only, then leaves. Leftover does not run on `.wait()`. A program uses either `.wait()` or `.leave()`. Mixing them is a programming error (the runtime aborts). After `leave` the handle is not live: no `wait`, `close`, `leave`, or `spawn` into it. `leave` on a wait-for dest is a programming error — the construct joins before the statement ends. `leave` is not cancellation; in-flight arms run to completion. There is no dest `.abandon`. `@destroy` on a dest waits (the wait body). It does not cancel. A join error is not dropped (`cc_error_exit`). Dest-create `CCParallel h = @parallel { … } !> @destroy;` attaches that hook to the dest bind. `h.invalidate()` cancels the adopt tree, then waits. Idle, joined, or left is a no-op.

`h.cancelled` is an atomic flag. A concurrent load with `h.cancel()` is defined. After `h.wait()`, a load of `h.cancelled` is visible to the waiter.

`h.paused` is an atomic flag. A concurrent load with `h.pause()` / `h.resume()` is defined. `h.paused()` is that load. Pause and resume run on a live dest — they do not require `.wait()`. After `h.wait()`, a load of `h.paused` is visible to the waiter.

The construct honors `paused` at the seams it emits: the start of a spawned thunk, the next `@parallel for` half, the next leaf iteration, wait-for enter, wait-for ticket entry, and after `@stage` `wait` returns `ok` before the block. `cc_parallel_honor(h)` yields while paused. Cancel is a mark: it does not skip a thunk, and it unsticks a paused honor. An arm already past that seam is in-flight. A body that cares mid-arm still polls. `.wait()` does not resume; a paused dest whose remaining work is still at a seam does not finish until `.resume()` or `.cancel()`. `h.cancel()` wakes parks on fibers attached to the dest (channel send/recv, exclusive wait). Pause does not complete those parks. `resume()` does not complete a parked `wait` or `recv`.

`h.cancel()` is `bool !>(CCIoError)`. `ok(true)` means this call performed the live→cancelled transition on this handle or an adopted descendant. `ok(false)` means the tree was already cancelled, already joined, or idle. The first `ok(true)` is the transition; a later call is `ok(false)`. `h.pause()` / `h.resume()` are the same Result shape: `ok(true)` is this call's transition on a live dest; `ok(false)` is already in that state, idle, or already joined.

UFCS is the surface: `h.wait()` is `cc_parallel_wait(h)`, and the same for `cancel` / `pause` / `resume` / `live` / `close` / `leave`. A void host unwraps in place (`h.wait() !>(e) { (void)e; };`). That lowers. A `void !>(CCError)` wrapper is a seam for `return h.wait()` plus an `@errhandler` that maps `CCIoError` to `cc_ok()` — not a second protocol. When the host is void, the wrapper is unnecessary; kick/drop may cancel and wait a dest that already ended (`wait` of a joined dest is `ok`; `cancel` / `pause` / `resume` of a joined dest are `ok(false)`).

Spawned arms do not inherit the caller's current deadline scope. The first arm runs on the caller and sees `@with_deadline` / `cc_current_deadline()`. A spawned arm polls `h.cancelled` for this construct's cancel. `cc_is_cancelled()` and `cc_deadline_expired(cc_current_deadline())` in a spawned arm do not observe the caller's clock. When several arms share one deadline `D`, name it (`@with_deadline(...) as dl`) and use `dl` in those arms, or write `@with_deadline(dl)` to make `D` current there. That is a stated fact, not a context object.

Compound assignment, indirection, field and subscript destinations, declarations, and other statements are ill-formed as assignment arms. A bare `{ }` as a direct child of `@parallel { }` is ill-formed; braces are C scope, not an arm. A `for` statement as a direct child is ill-formed — the loop is a form of the keyword (§8.11.4).

Lowering is fork-join: the dest exists before any arm runs. A dest-live one-arm expression has no kick: that arm is spawned and attached to the dest. Otherwise the first arm runs on the caller and each remaining arm is spawned and attached to the dest. `.wait()` joins. If an unmarked spawn fails, that arm runs on the caller. `@parallel spawn`, dest-live, and dest-attach abort on a failed admit; they do not inline.

A `return` in any arm joins every spawned sibling, then returns from the function. The construct does not wait for a later or earlier arm that has not returned — including an arm that never will (an external hang). If two arms both `return`, which value is taken is not specified. On the sequential denial (`seq` false, or `#pragma(@parallel) off`) it is a normal C `return`: later arms do not run.

Arms must not race. Sharing a location across arms, or reading another arm's destination, is undefined. The dest's `cancelled` and `paused` flags are the exception: a sibling may load them while `cancel()` / `pause()` / `resume()` store.

#### 8.11.2 `@serial`

```c
T a, b;
@parallel {
    @serial {
        int t = f();
        a = t;
    }
    b = g();
} !>.wait()!>;
```

`@serial { … }` is an arm of `@parallel { }`. The block is sequential: those statements are one sibling, in order. Its body is ordinary C. It may assign one simple outer name already in scope, or none (a statement arm: close, send, a loop). Locals, `if`, `for`, and inner `{ }` are C scope inside the arm. After the join, an outer write is visible the same way an assignment arm's write is. The arm may still spawn if it is not the first sibling.

`@serial` is legal only as a direct child of `@parallel { }`. It is not a handle, not an `else`, and not a file- or function-scope statement. It is ill-formed in `@parallel for`, nested inside another `@serial`, without a following `{ … }`, with an empty body, or with two or more distinct simple outer names. An assignment to a field, subscript, or indirection does not count as the outer name.

A `for` inside `@serial` is ordinary C. `return` in the arm is the assignment-join `return` of §8.11.1.

#### 8.11.3 Gated assignment join

```c
T left, right;
@parallel (d < k) {
    left  = f();
    right = g();
} !>.wait()!>;
```

`@parallel (pred) { … }` is the same join as §8.11.1, including `@serial` arms. When `pred` is true, lowering matches §8.11.1. When `pred` is false, the arms run in order on the caller and spawn is not attempted. The body always executes; `pred` chooses scheduling, not presence. There is no `else`. An empty predicate is ill-formed. `@parallel (pred) for` is ill-formed.

Independence is unchanged: reading another arm's destination is undefined on both paths.

#### 8.11.4 `@parallel for`

```c
@parallel for (i in lo..hi) {
    work(i);
} !>.wait()!>;
```

`lo..hi` is a half-open integer range. `i` is an `int` bound in the body for each iteration in `[lo, hi)`. The body is ordinary statements. A C `for (;;)` head is ill-formed; the `in` spelling matches `@comptime for`.

A `for` statement as a direct child of `@parallel { }` is ill-formed. The loop is a form of the keyword, not a statement the brace happens to contain.

Lowering bisects the range: one half is spawned, the other runs on the caller, then the spawn is joined. A span of length 0 or 1 runs as an ordinary sequential `for`. Nested `@parallel for` bisects the same way. If a spawn fails, that half runs on the caller. Each new half and each leaf iteration calls `cc_parallel_honor` on the dest in the walk env (null when the form has no dest): yield while paused, do not skip the piece.

Iterations must not race. Disjoint writes (`img[y * w + x] = …` for distinct `(x, y)`) are the caller's fact. A `goto` whose target is not a label in the same `for` body is ill-formed.

`break`, `continue`, and `return` are the same statements as in a `for`. `continue` finishes this iteration. `break` stops new iterations: a shared stop flag ends the range; a sibling half already spawned is joined (in-flight work finishes). `return` is `break` then `return` from the function after that join. The construct does not wait for a lower index that has not returned — including an iteration that never will. If two iterations both `return`, which value is taken is not specified. An error from `!>` in the body uses the enclosing `@errhandler`, not this stop flag.

`n.spawn` remains the tool when the program names a task lifetime or an explicit tile size.

#### 8.11.5 `seq` — the named denial

```c
@parallel seq (use_par) {
    left  = f();
    right = g();
} !>.wait()!>;
```

`@parallel seq (cond) { … }` is the gated assignment join of §8.11.3 with the denial written out: when `cond` is true the join tries to spawn; when false the arms run in order on the caller. `seq` names what happens when parallelism is not granted — the same body runs sequentially. Because `cond` is an ordinary runtime expression, one body carries two schedules: differential testing and adaptive dispatch flip a flag, not the code. `@parallel seq (cond) for` without `wait` is ill-formed; the `for` denial spelling is §8.11.6.

#### 8.11.6 `wait` — ordered spawn loop

```c
bool fin = @parallel seq (use_par) wait (ts) for (i in 0..n) {
    step(job, i) !>;
} !>;
```

The wait-for form is an expression of type `bool !>(CCError)`. `ok(true)` means the range ran out. `ok(false)` means a `break` that targets this wait-for cancelled the nursery. A ticket error is `err`. `continue` does not produce `false`. `return` leaves the function after drain and does not yield the construct's Result. Assignment join and bisect `@parallel for` remain statements.

`CCParallel h = @parallel wait (ts) for (…) { … } !>;` binds a dest that is live (`h.live()`) during the enter loop. Tickets may `h.pause()`, `h.resume()`, and `h.cancel()`. `h.n` is the construct's nursery: cancel marks the dest and cancels that nursery so enter stops. The construct runs the loop on the caller and joins before the statement ends; after it, `h` is joined. `!>.wait()!>` remains the consume spelling when there is no dest. A targeting `break` still requires a bool bind.

`@parallel wait (gate) for (i in lo..hi) { … }` runs the loop as an ordered spawn loop. `gate` is the name of a `CCTurnstile` or `CCTurnstileRW` (§8.12) in scope; any other type is ill-formed. Iterations are tickets: the construct calls `enter(i)` on the caller in loop order — the depth cap bounds in-flight iterations — then spawns the body. `leave()` runs after the body on every path. If a spawn is denied, that iteration's body runs on the caller before the next `enter`; the token is never leaked. `wait` without `for`, or with an assignment-join body, is ill-formed.

The optional `seq (cond)` prefix composes: when `cond` is false the same body runs as a plain sequential `for` on the caller, with no `enter`/`leave` and no spawn. The construct also takes this path when it cannot allocate its join scope. Stage `wait`/`pass` still run: on the sequential path `pass` precedes the successor's `wait` in program order, so the gate cell is UNARMED and the wait returns immediately.

An optional `worker (name)` after the range binds `name` as an `int`: the index of the runner slot executing the ticket, in `[0, cap)`. Two live tickets never share a slot. Like the loop variable, the binder is a per-ticket value, not a capture. On every sequential path it binds `0`. `worker` without `wait` is ill-formed. The surface idiom for reusable per-ticket scratch is `cache (name)`, not an index.

`cache (name, …)` after `wait` names enclosing locals the construct adopts as warm scratch. Each ticket gets exclusive use of an instance of that type whose contents are the initializer (cold) or the state some earlier ticket left (warm). Which instance a ticket gets is unobservable. Sequential paths, including `seq` denial and `#pragma(@parallel) off`, keep the one declared instance — deleting the clause leaves the serial program. A wait-for may instantiate additional copies, up to the turnstile cap; it runs the initializer on each extra and the declaration's `@destroy` on each extra at join. After the construct the declared name holds some instance's state — unspecified which. Slot 0 is the declared instance; extras are not copies of it. `cache` without `wait` is ill-formed. A name is adopted by at most one wait-for. A body-local name, the loop variable, and a `worker` binder are ill-formed in `cache`. A cache name does not appear in an `@stage` block: `@stage` is loop-carried identity; `cache` is unobservable instance identity.

Body statements may raise with `!>`. Errors are stop-starting: a failure stops new tickets from entering, in-flight iterations finish, and after the brace a ticket error re-raises into the innermost `@errhandler` for `CCError`, or into an attached `!>(e) { … }` on the construct. A handler must be in scope on both schedules unless that attached tail is present. A failed `enter` takes no token and joins the same way. `wait` and `pass` are `void !>(CCError)`: a handshake failure is a ticket error and runs that same handler. Because an entered successor is parked in `wait(i+1)` until ticket `i` discharges the stage, an entered ticket must discharge every `@stage` on every path. A success exit `pass`es unpassed stages (waiter wakes `ok`). An error exit `fail`s them: the gate becomes FAILED and a parked `wait` wakes as `err(CANCELLED)`. Ticket `i` errors before or inside a stage; ticket `i+1` already in `wait` does not hang — it wakes with that error, skips the block, `fail`s its own remaining stages, and `leave`s. The predecessor's body error and the successor's cancelled wait are both ticket errors; which one re-raises is not specified. `@defer` and `@errhandler` at the top level of the body are ill-formed; the structural form of the discharge is `@stage`.

`@stage (gate, args…) { … }` is a statement inside a wait-for body, not a Result: it unwraps `gate.wait(args…) !>;`, the block, `gate.pass(args…) !>;`. `gate` is spelled as the receiver the calls apply to — `ts.read`, `ts.write`, a `CCTurnstile` with the stage index in `args` (`@stage (t, k, i)`), or a pointer to either. Each `@stage` is a ticket-scoped handshake on a named gate cell: whichever of `wait` or `pass`/`fail` touches the cell first creates it (ARMED, UNARMED, or FAILED); the other completes the handshake. A successor may therefore `wait` before this ticket reaches the block. `wait` has two completions: `pass` wakes `ok`; `fail` wakes `err(CANCELLED)`. Pause is not a third completion. Honor after `wait` returns `ok`, before the block; `resume()` does not complete `wait`. Work inside the block may be empty or guarded (`if (chain) { … }`); the handshake still happens. The `@stage` itself is a top-level statement of the wait-for body — nested in `if`, a loop, or another `@stage` is ill-formed. Inside the block the ticket is ordered and exclusive for that stage — loop-carried state reads as it does in the sequential loop. `@stage` outside a wait-for body is ill-formed. A raise inside the block exits the ticket; the error-path `fail` still happens.

The loop-carried case is the point: state that hops from ticket `i` to `i+1` (a chained compression dictionary, a running checksum, an output file position) sits in an `@stage` block in the body and reads exactly as it does in the sequential loop. The parallel run and the denied run produce the same output.

A wait-for whose body can so `break` cannot discard the bool: `bool fin = @parallel wait (ts) for (…) { … if (c) break; } !>;` is the form. `int` and `_Bool` are the C spellings of the same Ok payload. `fin = @parallel wait (…) for (…) { … } !>;` binds a name already in scope. A bare wait-for statement with no targeting `break` is implicitly unwrapped (`!>;`) into the matching handler. Discarding the bool when `break` is present is ill-formed. A bool bind on assignment join or `@parallel for` is ill-formed.

`break`, `continue`, and `return` in the body are the same statements as in a `for`. On the sequential path they are those statements. On the parallel path they join first: `break` and `return` cancel so no new ticket enters; in-flight tickets finish and pass every `@stage` they have not passed; a cancelled ticket skips the work inside an `@stage`. `return` is `break` then `return`: the same drain as `break`, then the function returns that value. The construct does not wait for a lower ticket that has not returned. If two tickets both `return`, which value is taken is not specified. A later ticket's error, including `CANCELLED` from the drain, does not beat an earlier ticket's `return`; a lower failing ticket still beats a later `return`. `break` alone leaves the construct as `ok(false)`. `continue` finishes the ticket the same way without cancelling. Stage-guarded effects and other self-serializing writes are the serial program's; ambient effects of a drained successor (an atomic, a log line, a counter) may have occurred. A `goto` whose target is not a label in the same wait-for body is ill-formed: the body is a ticket, and a jump cannot leave it.

#### 8.11.7 Grain and limits

An assignment or `@serial` arm after the first is spawned as a fiber and attached to the dest; `.wait()` joins. `@parallel for` spawns one half of a span at each bisection; a span of length 0 or 1 is a sequential `for`. If spawn fails or the adaptive gate refuses the spawn (task kind INVALID), that arm or leftover span runs on the caller. Denied work still runs. The construct does not estimate how much work an arm contains. A caller who knows a cutoff writes it on the join (`@parallel (d < k) { … }`) so the same arms run in parallel above the cut and in order below it.

`@parallel` MAY run arms serially when no arm's progress depends on a sibling running concurrently. `@parallel spawn` does not deny the spawned arms. Meeting admit (`@parallel spawn`, dest-live, dest-attach) does not take the adaptive deny path; a failed admit (OOM) aborts (`cc_parallel_die`). It does not run the sibling on the caller. A brace `@parallel` that names a blocking channel operation on any arm, or that captures a channel, is ill-formed unless it is `spawn` or `#pragma(@parallel) off`. Complementary `send`/`recv` is that case, including through a helper. `close` and `try_send` / `try_recv` are not that case. If a join denies a sibling and then parks on a channel, the implementation aborts (the park names the denied join). A nursery `n.spawn` is a different construct. A hang is diagnosed by the deadlock detector (park reason, parked fiber). A cutoff predicate (`@parallel (d < k)`) is optional: it names a floor the caller already knows. Ungated brace joins still adapt when they cannot rendezvous.

The implementation denies a `cc_parallel_spawn` when the site's leaf arms are cheaper than a spawn and the ready queue is already busy. A site classified as heavy (REAL) is never denied. The gate keys sites by thunk pointer and does not apply to `@parallel wait` / nursery / `cc_nursery_spawn*`. `#pragma(@parallel) off` and `seq` false are the user's sequential schedule.

The range bounds of `@parallel for` are converted to `int`. A span whose length does not fit in `int` is outside this form.

An implementation may reject a function that exceeds a finite number of `@parallel` constructs, assignment arms, or captured names. The first arm of an assignment join always runs on the caller.

#### 8.11.8 `#pragma(@parallel)` — static denial

```c
#pragma(@parallel) off
…                        /* @parallel here lowers sequentially */
#pragma(@parallel) on
```

`#pragma(@parallel) off` makes every `@parallel` construct that follows it lower to its sequential form: join arms run in source order on the caller frame, `@for` and `wait @for` are plain loops, and no task, environment, or scheduler call is emitted — the compiled function is the linear program. A wait-for under `off` is still `bool !>(CCError)`: a targeting `break` is `ok(false)`, and the range running out is `ok(true)`. `on` restores the parallel lowering. The toggle is positional: it applies from the directive to the next toggle or the end of the translation unit, at file scope or statement position.

Where `seq (cond)` is the runtime denial — one body, two schedules, chosen by a flag — the pragma is the compile-time denial. Under `off` a `seq`/gated predicate is not evaluated: the annotation is inert, and the program is the loop as written. `worker` binders bind `0`; `cache` names keep the one declared instance; `@stage` blocks emit their `wait`/`pass` calls, which complete immediately when `pass` precedes `wait` in program order. A wait-for under `off` has no re-raise edge, so it does not require a `CCError` handler in scope. Any operand other than `off` or `on` is ill-formed. The directive is consumed by the lowering and never reaches the host compiler.

**Grammar (normative, minus whitespace).**

```
parallel_stmt ::= '@parallel' parallel_join
               |  '@parallel' '(' pred ')' parallel_join
               |  '@parallel' 'seq' '(' pred ')' parallel_join
               |  '@parallel' 'for' '(' ident 'in' expr '..' expr ')' block
               |  wait_for_stmt
               |  wait_for_bind

wait_for_expr ::= '@parallel' [ 'seq' '(' pred ')' ] 'wait' '(' ident ')'
                  [ 'cache' '(' ident { ',' ident } ')' ]
                  'for' '(' ident 'in' expr '..' expr ')'
                  [ 'worker' '(' ident ')' ] block

wait_for_stmt ::= wait_for_expr [ bang_tail ]

wait_for_bind ::= ('bool' | 'int' | '_Bool') ident '=' wait_for_expr bang_tail
               |  ident '=' wait_for_expr bang_tail

bang_tail     ::= '!>' ';'
               |  '!>' '(' ident ')' stmt
               |  '!>' '(' ident ')' '{' stmt* '}' ';'?

parallel_join ::= '{' parallel_arm+ '}'

parallel_arm  ::= ident '=' expr ';'
               |  serial_arm

serial_arm    ::= '@serial' '{' stmt+ '}'

stage_stmt    ::= '@stage' '(' expr { ',' expr } ')' block
```

`wait_for_expr` has type `bool !>(CCError)`. `wait_for_bind` unwraps the Ok bool into `ident`. A wait-for whose body contains a `break` that targets that wait-for is ill-formed unless it is a `wait_for_bind`. A `wait_for_stmt` with no targeting `break` and no `bang_tail` is implicitly `!>;`. `bang_tail` `!>(e) { … }` is the re-raise handler for that construct. A `wait_for_bind` whose right-hand side is not a wait-for is ill-formed.

`pred` is a nonempty expression. `seq`, `wait`, `cache`, and `worker` are contextual words after `@parallel`, not keywords elsewhere. The `wait` ident names a `CCTurnstile` or `CCTurnstileRW` in scope. `ident` on an assignment arm, and the unique simple outer name assigned by a `serial_arm`, are names already in scope. `serial_arm` is a `parallel_arm` only. In `stage_stmt` the first expression is the gate receiver; the rest are the `wait`/`pass` arguments. `stage_stmt` is a top-level statement of a wait-for body only — not nested in `if`, a loop, or another `@stage`. Work inside the block may be guarded. `cache` names are declarations of the enclosing scope; the clause is a ticket-scoped adoption, not a statement of the wait-for body. `@stage` is a statement, not a Result.

---

### 8.12 Ordered pipeline turnstile (`CCTurnstile`)

A **turnstile** bounds how many workers run at once (a depth channel of `cap` tokens) and sequences tickets through one or more stages. Stage `k` ticket `i` cannot run until ticket `i-1` has passed that stage. `enter(i)` receives a depth token. Stage `wait`/`pass`/`fail` use named exclusive gate cells created on first touch: `wait` creates ARMED and parks if the cell is absent; `pass` creates UNARMED if absent, or completes an ARMED cell and wakes the waiter `ok`; `fail` marks FAILED and wakes a parked waiter as `err`. The map upsert is the happen-before. Each name has exactly two touchers; the second frees the cell so live names stay O(in-flight). The worker waits and passes each stage, then `leave()` returns the token. A null stage or exclusive, and exclusive INVALID/TIMEOUT/CANCELLED/FAILED, are `void !>(CCError)` — not a silent return.

Declare the turnstile before the nursery so `@destroy` joins children before the depth channel is freed. Create is `T !>(CCError)`: a dead arena, `cap < 1`, `n_stages < 1`, exclusive-table allocation, or depth-channel allocation is an error — not a zeroed handle.

```c
CCTurnstile t@(cap, n_stages, arena) !> @destroy;
t.enter(i) !>;
t.stage(k).wait(i) !>;
t.stage(k)->pass(i) !>;
t.wait(k, i) !>;
t.pass(k, i) !>;
t.leave() !>;

CCTurnstileRW ts@(cap, arena) !> @destroy;
ts.enter(i) !>;
ts.read.wait(i) !>;   ts.read.pass(i) !>;
ts.write.wait(i) !>;  ts.write.pass(i) !>;
ts.leave() !>;
```

`CCTurnstileRW` is the two-stage face: `read` and `write` are `stages[0]` and `stages[1]` (`as: core` for `enter` / `leave` / `stage`). A closed depth channel is `CC_IO_CONNECTION_CLOSED`, not `Ok(false)` — `!>` on `Ok(false)` would spawn without a token.

**Runtime API (normative):**

```c
void !>(CCError) cc_turnstile_init(CCTurnstile* t, int cap, int n_stages,
                                  CCArena arena, CCTurnstileStage* slots);
CCTurnstile !>(CCError) cc_turnstile_create(int cap, int n_stages,
                                           CCArena arena);
void cc_turnstile_destroy(CCTurnstile* t);
bool !>(CCIoError) cc_turnstile_enter(CCTurnstile* t, int i);
bool !>(CCIoError) cc_turnstile_leave(CCTurnstile* t);
CCTurnstileStage* cc_turnstile_stage(CCTurnstile* t, int k);
void !>(CCError) cc_turnstile_wait(CCTurnstile* t, int k, int i);
void !>(CCError) cc_turnstile_pass(CCTurnstile* t, int k, int i);
void !>(CCError) cc_turnstile_fail(CCTurnstile* t, int k, int i);
void !>(CCError) cc_turnstile_stage_wait(CCTurnstileStage* s, int i);
void !>(CCError) cc_turnstile_stage_pass(CCTurnstileStage* s, int i);
void !>(CCError) cc_turnstile_stage_fail(CCTurnstileStage* s, int i);
CCTurnstileRW !>(CCError) cc_turnstile_rw_create(int cap, CCArena arena);
void cc_turnstile_rw_destroy(CCTurnstileRW* w);
```

**UFCS surface (normative):**

- `t.enter(i)` / `t.leave()` — depth token
- `t.stage(k)` — `stages[k]`, or `NULL` if `k` is out of range
- `t.wait(k, i)` / `t.pass(k, i)` / `t.fail(k, i)` — index form (`void !>(CCError)`)
- `s.wait(i)` / `s.pass(i)` / `s.fail(i)` — one stage
- `ts.read` / `ts.write` — `CCTurnstileRW` stage aliases

Header: `<ccc/cc_turnstile.cch>` (also via prelude).

---

## 9. Standard Library (UFCS-First Design)

This section defines the core standard library using **UFCS-first design**: method syntax is primary and UFCS lowering is type-directed and library-owned.

**Design principle:** UFCS is resolved from the concrete receiver type. The library owning that type or family defines the callee contract, including whether the receiver is passed by address or by value. Direct library-call forms may also be exposed, but they are API choices rather than the definition of UFCS itself.

**IMPORTANT DESIGN CONSIDERATION:** Concurrent-C generally orders parameters by semantic driver, with support context following unless the context itself is the controlling operand. This is especially visible in UFCS lowering, where the receiver is ordinarily the semantic subject. Accordingly, support context such as allocators usually appears last in direct-call APIs.

**Core UFCS rules (normative):**

- The receiver is the full expression to the left of `.` or `->`.
- The compiler resolves the receiver to a concrete type before choosing a UFCS lowering rule.
- Field access and mixed member chains participate normally: `holder.arena.free()` dispatches on `holder.arena`; `ptr->arena.free()` dispatches on `ptr->arena`. An explicit field selection is not the outer type's `@typeview` allow-list or `as:` retry — `d->tree.destroy()` is a method on the field's type; `d->len()` with no peel is still the outer.
- When multiple links appear in a chain, the nearest concrete typed receiver in the chain determines dispatch.
- Standard-library families define canonical lowered C namespaces (`cc_file_`*, `cc_arena_`*, `cc_string_*`, `cc_slice_*`, `cc_channel_*`); internal erased-core helpers remain implementation details.

**Rule (C-first dispatch, normative):** Ordinary C struct/union member access wins over UFCS. For `receiver.name(args)` (or `receiver->name(args)`), if `name` is a data member of the receiver's resolved type — including a function-pointer member — the expression is an ordinary C member access/call and UFCS is **not** considered. UFCS applies only when `name` is **not** a member of the receiver type. Shadowing semantics follow plain C: members shadow any UFCS family entry of the same name without ambiguity or diagnostic. This makes UFCS an error-trap layer on top of C member access rather than a competing dispatch rule.

**Rule (unresolved is ill-formed, normative):** If UFCS applies (the receiver's resolved type has no matching member and the receiver type has a registered UFCS family or a type-driven fallback namespace) but no callable can be produced — because the registered `.ufcs` handler returns the empty slice, no fallback family is registered for the receiver type, or the synthesized callee does not exist at link time — the program is ill-formed. The compiler must diagnose this at compile time with the receiver type, the method name, and the source location of the call, rather than silently falling through to ambient name lookup. A receiver whose type is not a struct/union and has no registered UFCS family is simply not a UFCS call and the ordinary C lookup rules apply unchanged (no diagnostic).

**Rule (scalar value receivers, normative):** A receiver of scalar arithmetic type (`double`, `float`, `int`, `short`, `long`, `long long`, `size_t`) dispatches to the family `cc_<mangled type>_<method>`, where multi-word type names join with `_` (`long long` → `cc_long_long_<method>`). The receiver is passed by value, never by address; cv-qualifiers on the receiver do not vary the callee. The composed callee is used only when that function is verifiably declared in the translation unit or an included header — these are ordinary declarations, never synthesized — otherwise the call site is left unchanged and is ill-formed C. A numeric literal is a receiver only when parenthesized: `(1.5).halve()` dispatches on `double`, `(42).twice()` on `int`, with the literal's type read lexically from its suffix under C rules; one leading unary minus may appear inside the parentheses. `1.5.halve()` without parentheses is not a UFCS call. Unsigned suffixes and `char` receivers do not participate. A declared `<type>_<method>` snake spelling for the same receiver keeps its ordinary dispatch precedence.

**Rule (naked calls and print aliases, normative):** A call `name(args)` is C: it dispatches to the declared function or macro named `name`, or is ill-formed. Exactly six names alias when `<ccc/stdio.cch>` is visible and the translation unit binds none of them itself: `print`, `println`, `eprint`, `eprintln` at call position dispatch to `cc_print` / `cc_println` / `cc_eprint` / `cc_eprintln` (`_Generic` over the data argument); `fprint` / `fprintln` dispatch to `cc_fprint` / `cc_fprintln` with **fd first, then data** (`fprintln(STDERR_FILENO, path)` — fprintf-shaped). Member UFCS stays data-first (`path.fprintln(STDERR_FILENO)`). A translation-unit binding of one of these names — function, function-like macro, or declaration shape — takes the call unchanged, and member position (`s.println()`) never aliases. Prefix spellings for other functions come by declaration: a declared `f(T, …)` is callable as `f(x, …)` by C and as `x.f(…)` by the bare-name tier — one declaration, both spellings. Method families reach prefix position through an imported handle whose type carries them (`CCStdio io = cc_stdio_create(&a); io.println(x)`).

**Rule (universal bare-name tier, normative):** When every family composition for `recv.f(args)` fails to name a declared function, `f` itself is the final candidate: the call dispatches to a declared function `f` whose first parameter takes the receiver — `u.mean(6.0)` lowers to `mean(u, 6.0)`; `pp->get_x()` lowers to `get_x(pp)`. Compatibility is uniform where lossless and exact where lossy. A value receiver of type `T` matches a first parameter of type `T` exactly (no arithmetic conversions — dispatch never converts the receiver's value), or of type `T*` / `const T*` via `&recv` (addressable receivers only). A pointer receiver of type `T*` matches pointer parameters under C's pointer rules — exact `T*`, qualifier-adding `const T*`, and `void*` / `const void*` — one-way: a `const T*` receiver matches only const-qualified parameters. A dereference is never synthesized: pointer receivers match pointer parameters only. A first parameter of `void*` never matches a value receiver (the address synthesis and the type erasure are not combined implicitly). Zero-parameter functions never capture a receiver. Members, composed family spellings, and a receiver type's registered dynamic sink all outrank the bare name — a sink-registered type's unresolved methods belong to its sink, and a later-declared ambient function cannot capture them. Declarations participate from the translation unit, included headers, and the parse's symbol table — which sees system headers, so `d.fabs()` with `math.h` included dispatches to `fabs(d)`. Unprototyped (old-style) declarations never participate. A composed callee that is not verifiably declared is never emitted while a bare-name match exists.

**Rule (family member sets, normative):** A generic family instance's method set derives from the family's declaration form: the `##_<member>` tokens of the family macro's body are the members (`Name##_push` declares `push`; `NAME##_sub` declares `sub`). Factory-emitted families derive their member set from the emitted fragment's `<mangled>_<member>` definitions. Dispatch trusts composed spellings exactly for this derived set — members are macro-generated and invisible to textual declaration checks — and an unresolved method on an instance enumerates it. Instances are extensible by declaration: a visible function spelling the composed name (`CCVec_double_median(CCVec_double*, …)`, `CCSlice_double_sum(CCSlice_double*, …)`) makes `v.median(…)` / `s.sum(…)` dispatch to it, with no change to the family header or the compiler.

**Rule (method chains, normative):** A UFCS call whose receiver is itself a call expression is well-formed when the receiver's return type is known — derived from the family declaration form for instance members, read from the visible declaration otherwise. The chain lowers as if the receiver were first bound to a temporary of that type; each subsequent link then resolves against that variable under the ordinary rules (members, extensions, `@typeview` `as:` face retry, the strict ladder), so `xs.sub(1, 3).len()`, `ps.at(2).y`, and scalar chains like `d.halve().twice()` mean exactly what their bound-temporary spellings mean. A trailing field access binds to the last link's result. A failing link diagnoses against its own receiver type, enumerating that instance's installed methods.

**Rule (fallible chains, normative):** `!>` links hops whose calls return a Result: `py.a()!>.b()!>;` unwraps each hop and dispatches the next method on the unwrapped value, and `py.a()!>(e){ … }.b()` recovers that hop alone with the handler's written control flow. Each linked hop lowers to its own statement binding a temporary of the hop's ok type, so a bare `!>` targets the enclosing `@errhandler` and the final hop keeps the original destination, including destination-typed extraction. Fallible chains live where statements do: at statement position or as the whole right-hand side of a declaration or assignment. `?>` never links; parenthesize a fallback to continue from it.

**UFCS Equivalence (Normative):**

For named types that use ordinary fallback UFCS, method syntax lowers to the receiver-type method family for that type.

- Pointer-style APIs receive the address of a value receiver and the pointer itself for a pointer receiver.
- By-value APIs receive the receiver value directly.

For types or families that register custom UFCS lowering with the type-registration machinery, `x.method(args)` lowers according to the registered handler instead. Standard-library families may be implemented either by `@typehooks on …` (or the equivalent `cc_type_register(...)` marker form) in Concurrent-C source or by equivalent built-in family contracts; the resulting receiver-type-driven lowering is the normative behavior.

This enables two usage styles:

```c
// UFCS style (primary for most users; chains naturally)
char[:] result = input.trim().lower(arena);
size_t len = result.len();

// Direct library-call style (preferred for composition and generic code)
char[:] result = lower(trim(input), arena);
size_t len = len(result);

// Both styles can mix freely
char[:] trimmed = input.trim();              // UFCS
size_t sz = len(trimmed);                    // Direct library call
char[:] final = lower(trimmed, arena);       // Direct library call
```

Functional composition becomes natural with direct library calls:

```c
// Map over a vector using free functions
int[] squared = vec_map(numbers, (int x) => x * x);

// Chain via free functions
Vec::[int] result = vec_map(vec_filter(input, is_even), double);
```

---

### 9.0 UFCS Registration and Custom Lowering

UFCS is a type-owned extensible language feature. Libraries declare custom lowering rules at compile time by registering hooks for a concrete type or type family.

**Primary registration API:**

```c
typedef CCSlice (*CCTypeCreateHandler)(CCSlice type_name, CCSliceArray argv, CCSliceArray arg_types, CCArena arena);
typedef CCSlice (*CCTypeUfcsHandler)(CCSlice recv_type, CCSlice method, CCSlice mode, CCSliceArray argv, CCSliceArray arg_types, CCArena arena);

typedef struct {
    const char* callee1;
    const char* callee2;
    const void* callable;
} CCTypeCreateHook;

typedef struct {
    const char* pre_callee;
    const char* callee;
} CCTypeDestroyHook;

typedef struct {
    const char* callee;
    const char* arg_wrap;
} CCTypeDynamicHook;

typedef struct {
    int has_niche;
    unsigned size;
    unsigned align;
    unsigned offset;
    unsigned width;
    unsigned long long sentinel;
} CCTypeNicheHook;

typedef struct {
    CCTypeCreateHook create;
    CCTypeDestroyHook destroy;
    CCTypeUfcsHandler ufcs;
    CCTypeDynamicHook ufcs_sink;
    CCTypeNicheHook niche;
} CCTypeHooks;

int cc_type_register(const char* type_name, CCTypeHooks hooks);
```

**Registration rules (normative):**

- Preferred surface is `@typehooks on Subject { … };` with a strict C
  designated-initializer body. The compiler rewrites it to the marker form
  `(void)cc_type_register("Subject", (CCTypeHooks){ … })` inside `@comptime`
  before discovery (see `spec/draft_typehooks.md`).
- Registrations also appear as ordinary `@comptime` calls to
  `cc_type_register(...)` or `cc_type_define(...)` (same shape; both spellings
  are recognized).
- Registration discovery runs over the preprocessed translation unit, including
  included `.cch` headers, before type-owned lowering uses the registry.
  `cc_type_register(...)` / `cc_type_define(...)` are compile-time marker APIs
  and return `0`; see §14.5 for pipeline ordering.
- `Subject` / `type_name` names either an exact concrete type such as `CCArena`
  or a trailing-wildcard family such as `CCChanTx_*` or `CCChanRx_*`.
- The hooks body / second argument must be a hooks object literal, typically
  `(CCTypeHooks){ ... }` on the marker form.
- Registrations are library-owned. The compiler selects a UFCS lowering rule from the resolved receiver type, not from the method name alone.
- Handlers may be named functions or non-capturing lambdas.
- `.create` is the type-owned construction hook. The compiler selects the overload from the declared type plus the `name@(args)` argument list.
- `.destroy` may register a pre-destroy hook, a destroy hook, or both. `pre_callee` runs before `callee`.
- The `.ufcs` hook is responsible only for choosing the lowered callee family. It does not execute the call.
- Returning the empty slice means "no custom rewrite; fall back to ordinary receiver-type UFCS".
- `.ufcs_sink` is the last-resort unresolved-method hook. Unresolved methods lower to `callee(&recv, "method", N, arg_wrap(a1), …)`. The sink is destination-aware: wherever a typed destination is visible the callee composes as `<callee>_<mangled dest>` when that function is declared (compose-then-verify; plain callee otherwise). `.ufcs_dynamic` and `.ufcs_dynamic2` are accepted spellings of `.ufcs_sink`.
- `.niche` donates a bit pattern a valid instance never exhibits, so a `@variant(packed)` arm of this type can carry the discriminant (`spec/draft_variants.md`, packed layout). `cc_type_niche(size, align, offset, width, sentinel)` is the helper.
- `.cast` is dest-convert. The handler receives the source type, the requested dest type, and `kind` (`implicit` or `explicit`) and returns a callee name, the UFCS pass tag, or empty (hard reject). Implicit sites (decl-init, `=`, and a by-value slice call argument) ask the dest type only. Dest may insert a wrap; dest must not insert a peel. A slice dest may wrap `CCString` / `CCString*` as `as_slice`, `CCVec_T` / `CCVec_T*` as `Name_as_slice` when the dest element is `T` (`int[:]` ← `Vec::[int]`; `char[:]` ← `Vec::[char]`), or a bound `char*` / `const char*` as `cc_slice_cstr`. `v.as_slice()` remains the explicit spelling. For-in does not dest-cast: `@for (x in v)` walks the live vec.
- `.len` names the extent (`cc_type_len_field` or `cc_type_len_call`). It returns `size_t` — naked, not Result. The hook checks the minimum (usually nothing). Ordinary sites may read `x.len` / `x.len()`; they may not store it. `T[n]` `.len` is the constexpr bound `n`.
- `.access` is the compiler-internal walk slot (`cc_type_access_load` or `cc_type_access_call`). It returns `T` / the slot — naked, not Result. Users write `@for (v in s)` / `@for (&v in s) { … } !>;` / `@for (i, v in s)` / `@for (a, b in s, t) { … } !>;`, not `s.access(i)`. A slice header and a `T[n]` bound snapshot `.len` and the data pointer at entry. A grower (`CCVec_*`, `CCString`) snapshots the same way when the body does not grow, shrink, or rebind the subject. If the body can change the subject's extent, the condition re-reads `.len` and a write re-reads before the store; `i >= len` is `CC_ERR_INVALID_ARG` (`"for-in write"`) — that check is the mut walk's Result, not a skip and not `set()`. Point access stays `at` / `set` (Result). A user type joins for-in by registering both arms. `CCSlice` / `CCSlice_*`, `CCVec_*`, and `CCString` register both.
- `.create` may be registered either as fixed callee strings (`cc_type_create_call(...)`, `cc_type_create_overloads(...)`) or as a callable hook via `cc_type_create_hook(...)`.
- Recognized hook fields are `.create`, `.destroy`, `.ufcs`, `.cast`, `.len`, `.access`, `.ufcs_sink`, and `.niche`.

**UFCS handler contract (normative):**

- `recv_type` is the resolved receiver type name used for dispatch.
- `method` is the invoked method name.
- `mode` is an optional lowering mode chosen by the language surface. For example, async-aware families may distinguish ordinary calls from `@await`-driven lowering here. If unused, handlers should ignore it.
- `argv` is the rewritten argument-expression list only. It does not include the receiver.
- `arg_types` is the compile-time inferred type list for `argv`, positionally aligned with it.
- `arena` is temporary compile-time storage for constructing the returned callee name.

**By-value receiver lowering:** A UFCS handler normally selects an address-style receiver-family callee. If a family wants the receiver passed by value instead, it must return the lowered symbol via `cc_ufcs_emit_value(...)` or `cc_ufcs_emit_value_cstr(...)`.

**Create hook contract (normative):**

- `.create` is selected from the declared type that appears on the left-hand side of `T name@(args)`.
- The compiler implicitly selects the registered creation overload from the `@(args)` argument count.
- The current implementation supports at most two explicit `@(args)` arguments, including callable create hooks.
- `cc_type_create_call("callee")` registers the one-argument form.
- `cc_type_create_overloads("callee1", "callee2")` registers one- and two-argument forms on the same `.create` hook.
- `CC_TYPE_CREATE_DECL("callee")` marks a create overload as declaration-form: lowering emits `callee(name, args);` instead of `T name = callee(args);`. It is only valid on a value binder.
- `cc_type_create_hook(handler)` registers a callable create hook; it receives `type_name`, `argv`, `arg_types`, and `arena`, and must return the lowered callee name as a slice.
- `cc_type_destroy_call("callee")` registers the destroy phase only.
- `cc_type_pre_destroy_call("callee")` registers the pre-destroy phase only; it runs before any call-site `@destroy { ... }` body.
- `cc_type_destroy_hooks("pre", "destroy")` registers both destroy phases.
- `T name@(args)` is well-formed only when `T` has a `.create` hook (or a `_new` factory on a generic instance). Typedef aliases use the base type's hooks. A dest type with neither a hook nor a `_new` / folklore callee is a compile-time error naming `T`. It must be followed by explicit ownership syntax: either `@destroy` or `@detach`. Omitting both is a compile-time error.
- When the create callee returns `T!>(E)`, dest-mint is `T name@(args) !> @destroy` (or `@detach`). A written `!>` always unwraps. `cc_adopt` is the exception — adopt is not a Result create.
- `@detach` does not take a cleanup body.
- For `T name@(args) @destroy { body };`, lowering order is: registered `pre_callee`, then call-site `body`, then registered `callee`, then the value-field chain (§3.1).
- `arg_types` for `.create` is inferred from the `@(args)` argument list. Implementations may leave complex local expressions unknown.

**Preferred registration style:**

```c
@typehooks on CCFile {
    .ufcs = (recv_type, method, mode, argv, arg_types, arena) => {
        (void)recv_type;
        (void)mode;
        (void)argv;
        (void)arg_types;
        return cc_slice_concat2(
            cc_slice_from_buffer("cc_file_", sizeof("cc_file_") - 1),
            method, arena);
    },
};

@typehooks on CCChanTx_* {
    .ufcs = cc_channel_tx_lower_c,
};

@typehooks on CCChanRx_* {
    .ufcs = cc_channel_rx_lower_c,
};

@typehooks on CCSlice_* {
    .len = cc_type_len_field("len"),
    .access = cc_type_access_load("ptr"),
};
```

UFCS registration and typed lifecycle hooks (`create`, `destroy`) use the same type-owned registration machinery.

Prefer `@typehooks` for UFCS together with lifecycle hooks. `cc_type_register` / `cc_type_define` and `cc_ufcs_register(...)` remain accepted dual forms (see `docs/deprecated.md`).

Is-a faces and allow-lists use `@typeview on Subject { as: field; r: …; }` (see `spec/draft_facets.md`).

This same contract applies to standard-library families such as channels, files, strings, arenas, vectors, maps, and results. Family-specific naming and lowering remain library policy rather than compiler policy; shared erased-core machinery is permitted so long as the family contract is preserved.

---

### 9.1 Strings

**Type:** `String` — small growable string builder (`CCString`)

```c
// C ABI: CCString (SSO inline or arena-backed heap header)
// Language surface alias: String → CCString; Arena → CCArena
```

`String` is a small, moveable handle. Short values stay inline; larger values live in an arena-owned buffer. Copying a `String` aliases the same storage. To obtain an independent copy, use `as_slice().clone(a)` / `cc_string_from_slice`. Heap contents live until released or their arena is reset/freed.

`String.as_slice()` returns a length-keyed `char[:]` / `CCSlice` view (not necessarily NUL-terminated). Dest-init `char[:] v = s` and assign `v = s` insert that view. Call `s.cstr(arena)` / `cc_string_cstr` when a `const char*` is required.

**Template literal dedent (normative).** Every backtick template —
`@string`, `@emit`, wherever a template literal appears — dedents against
its closing backtick. When the closer is alone on its line (only spaces
and tabs before it), that whitespace is the **margin**: exactly that
prefix is stripped from every content line, and the margin before the
closer is stripped with it. A closer that shares its line with content
means no dedent, so one-line templates pass through byte-for-byte, and an
empty margin strips nothing. The rule applies per physical line of the
template, interpolation and verbatim spans included. The line after the
opening backtick starts the margined region; the opening line's remainder
is not margined (it begins mid-line).

A non-blank content line indented less than the margin is a compile error
naming the line — never a silently reduced margin. Blank lines are exempt
and pass through unchanged.

```c
    py.exec(@string(`
        def scale(xs, k):
            return [x * k for x in xs]
        `, arena).as_slice()) !>;
    // the callee receives "def scale(xs, k):\n    return [x * k for x in xs]\n"
```

The template sits at the code's indentation; its content still means
column 0, and relative indentation inside the block (the body's four
spaces) is preserved.

#### 9.1.1 Core API

```c
// C ABI / library constructors (language aliases String/Arena accepted)
String   cc_string_new(void);                     // empty inline; no arena yet
String   cc_string_with_capacity(CCArena a, size_t cap);
String   cc_string_from(expr, CCArena a);          // expression-generic helper
String   cc_string_from_slice(CCArena a, char[:] initial);
char[:0] @slice("...");                           // build-time canonical slice
String   @string(expr, CCArena a);                 // literal/single-value builder
String   @string(policy, `...`, CCArena a);        // templated builder
String   @string(`...`, @scratch);                // temp stack arena (§9.1.4)
String   @string(`...`, @scratch(N));             // sized temp stack arena (§9.1.4)
char[:]  @string(`...`);                          // arena-less bounded template (§9.1.2)

String* cc_string_push(String* s, value, CCArena a);          // _Generic dispatch
String* cc_string_push_slice(String* s, char[:] data, CCArena a);
String* cc_string_push_char(String* s, char c, CCArena a);
String* cc_string_push_int(String* s, int64_t value, CCArena a);
String* cc_string_push_uint(String* s, uint64_t value, CCArena a);
String* cc_string_push_float(String* s, double value, CCArena a);
String* cc_string_clear(String* s);
char[:]  cc_string_as_slice(const String* s);     // length view
const char* cc_string_cstr(String* s, CCArena a);  // ensures NUL; NULL on failure
bool     cc_string_failed(const String* s);       // poisoned after growth failure

// UFCS (primary for users; arena last where growth may allocate)
String* s.append(value, CCArena a);     // alias for push
String* s.push(value, CCArena a);
String* s.push_char(char c, CCArena a);
String* s.push_int(int64_t value, CCArena a);
String* s.push_uint(uint64_t value, CCArena a);
String* s.push_float(double value, CCArena a);
String* s.clear();
char[:] s.as_slice();                 // also dest-init: char[:] v = s
const char* s.cstr(CCArena a);
size_t  s.len();
size_t  s.cap();
bool    s.failed();
String  <primitive>.to_str(CCArena a);  // e.g. 42.to_str(arena)
```

**Slice lifetime:** The slice returned by `as_slice()` remains valid until the next mutating call on the same `String` (e.g., `push`, `clear`) or until its arena storage is released/reset. For stable references, clone into another arena.

**String construction model:**

- `@slice("...")` yields a build-time canonical `char[:0]` inside the existing slice/provenance model.
- `@string(expr, a)` builds a `String` from a literal, `char`*, `char[:]`, `String`, or a value that supports `to_str(a)`.
- `@string(policy, \`..., a)`lowers to`String` builder operations over literal chunks plus interpolation slots.
- `@string(\`...\`)` with no arena is the bounded-template stack form: it yields a `char[:]` borrow of a block-scoped buffer and requires every interpolation to have a statically bounded width (§9.1.2).
- `@string(..., @scratch)` / `@scratch(N)` injects a shared function/closure stack arena for the `@string` arena operand only (§9.1.4).
- Template slots are string-oriented. Accepted slot forms are `char*`, `char[:]`, and `String`; non-string values may bridge through `expr.to_str(a)` if the receiver type provides that UFCS conversion.
- Interpolation syntax: only `${expr}` and `$~tag{expr}` start a slot (where `tag` is a C identifier). `${expr}` is **untagged**—the policy gets an empty tag slice and the value slice. `$~tag{expr}` is **tagged**—the policy gets the tag slice `"tag"` and the value slice, so policies can distinguish holes (metadata, escaping tiers, i18n keys, and so on). Any other `$` in the template is literal text, so ordinary uses like prices or macros do not need escaping.
- To emit a literal `${` or `$~tag{` sequence, prefix `$` with backslash: `\${` and `\$~…` are not slots; the backslash is removed and the string helpers emit the remainder (same rules as other template backslash escapes, e.g. an even run of `\` before `$` restores slot parsing, as in `\\${x}`).
- Backtick template bodies follow **Template literal dedent** above. Unwrap and Result sigils (`!>`, `?>`) that appear as characters inside a backtick `@string` / `@emit` template body are template text, not operators.

Example:

```c
CCArena arena = cc_arena_heap(megabytes(1));
String s = cc_string_new();
s.push("count=", arena)
 .push_char('x', arena)
 .push_int(42, arena);
char[:] view = s.as_slice();
if (s.failed()) { /* growth/OOM — do not treat partial text as success */ }

String msg = @string(42, arena);
String html = @string(html_policy, `<h1>${title}</h1>`, arena);
// Tag example: policy sees tag "meta" for the second hole
String row = @string(row_policy, `name=${name}; age=$~meta{age}`, arena);
```

#### 9.1.2 Arena-less `@string` — bounded-template stack form

`@string(`...`)` with no arena argument formats into a block-scoped stack buffer and yields a `char[:]` **borrow** of that buffer — no arena, no allocation, not a `String`.

**Boundedness rule (normative).** The arena-less form is legal iff every `${expr}` interpolation has a statically bounded maximum formatted width. The bounded types and their widths (bytes, worst-case decimal including sign):


| Interpolation type                                     | Max width |
| ------------------------------------------------------ | --------- |
| `char`                                                 | 1         |
| `bool`                                                 | 5         |
| `signed char`                                          | 4         |
| `unsigned char`                                        | 3         |
| `short`                                                | 6         |
| `unsigned short`                                       | 5         |
| `int`                                                  | 11        |
| `unsigned int`                                         | 10        |
| `long` / `unsigned long` / `long long` / `unsigned long long` | 20 |


Any other interpolation type (slices, `String`, floating point, pointers, …) is a **compile error** at the `@string` site, naming the offending interpolation and suggesting the arena form:

```
error: arena-less @string: interpolation '${name}' has no statically bounded
width (allowed: ${int}/${i64}/${u64}/${bool}/${char}); pass an arena:
@string(`...`, arena)
```

Policy-tagged slots (`$~tag{expr}`) require an arena and are not available in the arena-less form.

**Semantics (normative).**

- The buffer is a block-scoped `char[K]` where `K` is an integer constant expression: the decoded literal byte count plus the sum of the per-slot bounds above. The yielded slice's `len` is the written byte count; `K` is the buffer size and is not carried on the slice.
- The slice is a borrow with **block lifetime and stack provenance**: it stays valid for the rest of the enclosing block, including across later statements and nested blocks. Each `@string` site gets its own buffer.
- The form is a pure expression and may appear anywhere an expression may (initializer, argument position, …).
- `bool` formats as `true` / `false`; integers format in decimal.
- A literal-only template is legal (bound = decoded literal bytes); the empty template yields the empty slice. Template escapes decode as usual (`\n` is one byte).
- Formatted output cannot exceed the computed bound by construction; an overflow indicates a compiler bug and aborts loudly at runtime.

```c
int v = 42;
char[:] s = @string(`v=${v}!`);   // "v=42!"; buffer bound K = 2 + 11 + 1
take_slice(@string(`arg=${v}`));  // expression position
```

The arena form is unchanged: the same template with an arena yields an owned `String` and accepts unbounded interpolation types (slices, `String`, `double` / `float`, `const char*`, …). For unbounded values with stack-only storage, pass a fixed-buffer arena (`cc_arena_fixed_buffer`, §5); on exhaustion the result is a poisoned `String` (§9.1.3), never truncated output.

#### 9.1.3 `String` failure poison (sticky)

`String` is an owner. A builder step that cannot acquire storage (arena exhaustion — including a too-small fixed-buffer arena — or a size overflow) **poisons** the `String` instead of truncating it:

- Every subsequent push is a sticky no-op returning `NULL`.
- `len()` reads 0, `as_slice()` is empty, `cc_string_cstr()` returns `NULL`.
- `cc_string_failed(&s)` reports the poisoned state.
- `cc_string_clear(&s)` is the explicit recovery: it resets the value to a valid empty string.

#### 9.1.4 `@scratch` — temporary arena operand for `@string`

`@scratch` and `@scratch(N)` are legal **only** as the arena argument of `@string` (including `@string(policy, \`...\`, @scratch)` and `@string(expr, @scratch)`). They are not expressions and not general `CCArena` bindings.

**Lowering.** All `@string(..., @scratch)` / `@scratch(N)` sites in the same function or closure body share one stack arena injected at the start of that body:

```c
int main(void) {
    CC_ARENA_STACK(__cc_str_scratch, 1024);   // max of default and any @scratch(N)
    CCString s = @string(`r=${ratio}`, __cc_str_scratch);
    println(@string(`x=${x}`, __cc_str_scratch));
}
```

- Default size is 1024 bytes; `@scratch(N)` contributes `N` (`N` is a positive integer constant). The shared arena size is the **max** of the default and every `@scratch(N)` in that function/closure.
- Nested closures get their own shared scratch (C shadowing of `__cc_str_scratch`).
- Overflow follows `CC_ARENA_STACK` (stack-first, then ordinary growth / `String` poison rules).
- Freestanding `@scratch` (or use outside `@string`) is a compile error. Prefer `CC_ARENA_STACK` / `cc_arena_heap` for named or long-lived arenas.

**Call-local reclaim.** A call-local `@string(..., @scratch)` (e.g. `println(@string(\`…\`, @scratch))`) checkpoints the shared scratch before building the temp and restores after the consuming call. Earlier bound products in the same function remain valid; the temp's bump (and any extent growth for that temp) is reclaimed. Bound forms (`CCString s = @string(..., @scratch)`) keep their bytes for the function/closure lifetime and do not restore around the initializer.

A product that must outlive the consuming call — including an argument of `return` (`cc_script_sh`, `cc_script_sh_read`, `@destroy` return-rewrite) — is bound to a local first (`CCString line = @string(\`…\`, @scratch); return f(line);`). `return f(@string(\`…\`, @scratch))` is call-local: the temp is reclaimed after `f` returns, and that nesting does not compose with `@destroy` return-rewrite.

Newlines after the comma are whitespace — a wrapped `@scratch` is the same operand. `@scratch` is not a `CCArena` binding; there is no `scratch.destroy()`.

**Escape (normative).** Products of `@string(..., @scratch)` have function/closure lifetime. It is a compile-time error to:

- `return` that `String` (or a slice/view derived from it),
- assign it into a variable declared in an **outer** block,
- capture it into a closure or task that may outlive the enclosing function or closure.

Same-scope use (`CCString s = @string(..., @scratch); println(s);`) is fine. Call-local borrows (`println(@string(..., @scratch))`) are fine.

`@string(...)` templated construction follows the same contract: if the destination arena cannot hold the output, the result is a failed `String` — never partial bytes.

#### 9.1.5 Formatting

Formatted text uses `@string` templates and `cc_string_from` / push helpers
(stdlib Strings). There is no separate printf-style `format` entry point.

```c
CCString msg = @string(`Hello ${name}! Score: ${score}`, arena);
if (cc_string_failed(&msg)) { /* arena could not hold the output */ }
```

---

### 9.2 Slices with UFCS

**Type:** `T[:]` — Mutable view into contiguous data

`char[:0]` is the sentinel (NUL-terminated) refinement; its ABI is `CCSlice`.
Path, file, directory, command, CLI string, and script path faces use
`char[:0]` for NUL-terminated borrows. Host-included C headers may spell the
same parameter as `CCSlice`.

#### 9.2.0 C strings and call-site literals

```c
CCSlice char_to_slice_n(const char *p, size_t n);
CCSlice const_char_to_slice_n(const char *p, size_t n);
/* signedness variants: unsigned_char_to_slice_n, signed_char_to_slice_n, … */
CCSlice cc_slice_cstr(const char *cstr);

p->to_slice_n(n);   /* UFCS: char* → char_to_slice_n, const char* → const_char_to_slice_n */
```

**Rule (slice string-literal coerce):** A string literal whose destination is
by-value `CCSlice`, `char[:]`, `char[:0]`, `CCSliceShared`, or `CCSliceUnique`
— as a call argument or as a local/field initializer — lowers to
`CC_SLICE_LIT(lit)` (sizeof-static; `len` excludes NUL). A bound `char*` /
`const char*` dest-casts to a by-value byte slice through `.cast` — the
insert is `cc_slice_cstr`. Counted `char[N]` is not that insert; wrap those
with `p->to_slice_n(n)` / `char_to_slice_n(p, n)`. Host C has no dest-cast.
This is not general `char[N]` UFCS.

#### 9.2.1 Core Methods

```c
size_t len(CCSlice *s);
CCSlice sub(CCSlice s, size_t start, size_t end);

char !>(CCError) at(CCSlice *s, size_t index);           /* = get_checked */
char !>(CCError) get_checked(CCSlice *s, size_t index);
bool !>(CCError) set(CCSlice *s, size_t index, char c);
s.len();
s.sub(start, end);
s.at(i);
s.get_checked(i);
s.set(i, c);
s.ptr;
s.id;
```

Ordinary sites may read `.ptr` / `.len` / `.id`; they may not store fields.
Typed instances expose `len` / `sub` / `at` on `CCSlice_<T>`; the core
fields live on `.base`.

#### 9.2.2 Query Methods

Byte-slice (`CCSlice` / `char[:]`) query helpers are the shipped family in
`<ccc/cc_slice.cch>` / stdlib. There is no generic `T[:].contains` /
`T[:].find`.

```c
bool is_empty(CCSlice *s);
bool has(CCSlice *s, CCSlice needle);
bool has_ci(CCSlice *s, CCSlice needle);
bool starts_with(CCSlice *s, CCSlice prefix);
bool ends_with(CCSlice *s, CCSlice suffix);
bool eq(CCSlice *s, CCSlice other);

s.is_empty();
s.has(needle);
s.has_ci(needle);
s.starts_with(prefix);
s.ends_with(suffix);
s.eq(other);
```

`has` / `has_ci` are substring presence. Full signatures and index-of helpers
are in `spec/concurrent-c-stdlib-spec.md`.

#### 9.2.3 Mutation Methods

Byte-slice mutation is in-place length, indexed write, and dest-bulk.
`dst.copy(src)` copies `src.len` bytes (regions must not overlap).
`dst.copy_overlap(src)` is the same copy when regions may overlap; the
source view stays live. `dst.fill(c)` writes `c` through `dst.len`.
Each is `bool !>(CCError)`: a null window, or dest shorter than `src`
on the copy family, is `CC_ERR_INVALID_ARG`. There is no generic
`T[:].reverse` / `sort`. Ownership transfer is `cc_move` / `return` /
`send_take`, not dest-bulk.

```c
bool !>(CCError) truncate(CCSlice *s, size_t n);
bool !>(CCError) set(CCSlice *s, size_t idx, char c);
CCSlice sub(CCSlice s, size_t start, size_t end);

s.truncate(n);
s.set(i, c);
s.sub(start, end);
```

Vec `truncate(n)` shrinks the live extent and is a no-op when `n` is at
or above `len`. Slice `truncate` rejects `n` above `len`.

#### 9.2.4 Iteration

A **for-in** subject is a bound name, a field path off a bind
(`s`, `t->words`, `t.words`), or a **view** off that path whose type
answers both `.len` and `.access` (§9 type-owned registration) —
`s.sub(lo, hi)`, `s.trim()`, `str.as_slice()`. A view is bound to a
hidden local for the walk (`T tmp = expr; @for (v in tmp)`). Mut walk
stores through that same local (the header is a view; the store is into
the receiver's backing). Binders are
comma-separated identifiers at depth 0. Arithmetic and an untyped call
are ill-formed. A pointer type (`T*`) is not an extent —
`@for (v in p)` is ill-formed.

Stdlib extents: `CCSlice` / `CCSlice_*` (`.len` field, load of `ptr`;
typed instances hop those names through `as: base`), `CCVec_*` (`.len` / `data`),
`CCString` (`.len` field, `cc_string_data` for the load — SSO-safe).
`T[n]` uses the constexpr bound `n` and an index load.

```c
@for (i in lo..hi) { ... }   /* sequential range; hi < lo is empty */
@for (v in s) { ... }        /* walk: snapshot or live .len, then .access load */
@for (&v in s) { ... } !>;   /* mut walk: v = … is .access store; write bound is Result */
@for (&v in s.sub(lo, hi)) { ... } !>;
@for (i, v in s) { ... }     /* enumerate: i is size_t, v is the load */
@for (i, &v in s) { ... } !>;
@for (a, b in s, t) { ... } !>;  /* zip: void !>(CCError) */
@for (&a, &b in s, t) { ... } !>;

// Point (not the walk)
for (size_t i = 0; i < s.len; i++) {
    char item = s.at(i) !>;
}
```

A for-in **value** binder is not a C object. `v` in the body is the load
(a copy) and is const in that loop. Writing `v` (`v =`, `v +=`, `++v`)
is ill-formed: `&v` in the pattern stores into the walk; a local rebinds
the copy. Unary `&v` is ill-formed in either pattern — a C pointer into
the extent is peel of the subject (`s.ptr`). The range binder `i` in
`@for (i in lo..hi)` and the enumerate index are ordinary `size_t`
locals. `@for (&i in lo..hi)` is ill-formed.

**Mut walk:** `@for (&v in s) { … } !>;` (or `@for (i, &v in s)`). Type:
`void !>(CCError)`. The `.access` hook stays naked. `v = x` / `v += x`
/ `++v` store through the same `.access` peel as the load (`s.ptr[i]`,
`v.data[i]`, `cc_string_data`, `a[i]`). A slice header and a `T[n]` bound
are frozen: the walk snapshots `.len` and the data pointer at entry; a
write is in range by that trip count. A grower (`CCVec_*`, `CCString`)
snapshots the same way when the body does not grow, shrink, or rebind the
subject. If the body can change the subject's extent, the condition
re-reads `.len` and a write re-reads before the store; `i >= len` is
`CC_ERR_INVALID_ARG` (`"for-in write"`). That check is the error — not a
skip, not `set()`. Consume with `!>;`
(enclosing `@errhandler`) or `!>(e) { … }`. A bare mut walk is an
unconsumed Result. Field assignment (`v.x =`) is ill-formed — assign
the binder, or peel the subject. Zip with `&` binders is still zip
(Result); the zip length check is the other zip error.

**Zip:** the construct is a statement of type `void !>(CCError)`. Consume
with `!>;` (enclosing `@errhandler`) or `!>(e) { … }`. A bare zip is an
unconsumed Result. If the two live lengths differ, the Result is
`CC_ERR_INVALID_ARG`. There is no silent min. The walk runs only when
the lengths are equal. Copy walk, enumerate, and range are not Results:
a trailing `!>` is ill-formed. Mut walk and zip are Results.

A minted `T[:]` from `Vec` / heap `CCString` (`as_slice` / dest-init)
captures a generation on `id`. Owner realloc that **moves** the backing
kills that generation. Result `at` / `set` on the leftover view is
`CC_ERR_INVALID_ARG` (`slice: backing moved`). In-place growth keeps
the generation — the leftover view still names its prefix. `id == 0`
(`from_buffer`) is untracked and does not join this check. Walking the
owner (`@for (x in v)`) uses the live header. Walking the leftover
view still loads `ptr` (Gap, same as `s.ptr[i]`).

The walk is not “a nicer `s[i]`.” Users do not write `s.access(i)`. C
`for (;;)` is unchanged. `@parallel for (i in lo..hi)` is §8.11.4.

---

### 9.3 Arrays

Arrays in CC are `T[N]` (fixed-size, stack or struct-embedded). They do not
grow a generic `fill` / `sort` / `reverse` method family. `T[N]` is a
for-in subject (§9.2.4): `.len` is the constexpr bound `N`. Byte views use
`char_to_slice_n` / `to_slice_n` over the storage; typed growable sequences
use `Vec::[T]`.

### 9.4 Numeric Types with Methods

Checked `int64_t` arithmetic (`cc_add_i64_checked` / `sub` / `mul`) and scalar
`_to_str` / `cc_string_from` live in the stdlib (`spec/concurrent-c-stdlib-spec.md`).
There is no generic numeric method family (`abs` / `min` / `max` on every
integer type).

---

### 9.5 Script Library (`.shcc` / `ccc/script/`)

The **script library** (`cc_scriptlib`) is the scripting partner to the standard
library. It targets short Concurrent-C programs that orchestrate tools: read
bytes, parse with `@grammar` / SERDES, format with `@string`, spawn processes,
and print reports. Domain codecs (JSON, RESP, …) stay in application or
example headers; scriptlib supplies only the pipes.

Headers live under `<ccc/script/>`. The full stdlib surface used underneath is
defined in `concurrent-c-stdlib-spec.md` and §9.

#### 9.5.1 Language and file extension

A `.shcc` translation unit is the same language as `.ccs`. The `.shcc`
extension is distinct from the `.ccs` / `.cch` source and header pair. Kind
comes from the unit header (§1.7); a `.shcc` suffix is the fallback when the
header is absent. The driver applies an entry rewrite before the ordinary
Concurrent-C pipeline:

1. Strip a leading `#!` shebang line when present (`#!/usr/bin/env -S ./cc/bin/ccc [--as=shcc] [version=…]`).
2. Force-include `<ccc/script/prelude.cch>` at translation-unit scope.
3. When the unit has no top-level `main`, partition the body and inject a
   synthetic `int main(int argc, char **argv)`:
   - **TU scope:** `#` preprocessor lines, `@grammar` / `@comptime` blocks,
     `typedef` and `struct` / `enum` / `union` type definitions, function
     definitions and prototypes, and file-scope `static` / `extern`
     declarations.
   - **Synthetic `main`:** statements and non-static runtime-init declarations
     (including `name@(args) @destroy` locals).
4. Inject a default `@errhandler(CCError)` **inside** synthetic `main` and
   each `@task` body that prints `cc_error_str(e)` to stderr and returns
   `1`, so statement-level `!>` works without a local handler. Dispatch is
   type-matched (§3.1): a user `@errhandler` for a different error type
   (for example `CCIoError`) coexists with the default; a user
   `@errhandler(CCError)` in the same scope overrides the default for
   `CCError`. `CCIoError` Results reach this handler via `@typeview` `as: base`;
   Io constructors fill the face message so the print is not blank.
5. Token-gated script predecls `a` / `io` / `in` / `args` (same bindings as
   one-liner mode; see `draft_script_oneliners.md` §1.1) are injected into
   the synthetic `main` wrap only — the top-level statement body — when the
   identifier appears as a code token there and that body does not already
   declare the name. Injected shapes: `a` is a 1 MiB arena; `io` is
   `CCStdio` on a private `__cc_io_arena` (not `a`); `in` is `char[:]` from
   `io.read_all() !>`; `args` is `CCSlice` over `argv + 1`. `in` implies
   `io`; `io` implies its arena (and thus `a` only when `a` is also
   referenced). `@task` bodies are not predeclared and declare these names
   explicitly when needed. One-liner `-n`/`-p` locals `line` / `nr` are not
   ambient file predecls.
6. An explicit top-level `main` together with any MAIN-classified top-level
   statement is ill-formed.
7. Stamp provenance so diagnostics refer to the original `.shcc`: raw
   `#line` before TU-scope chunks; masked `CC_LN` markers before each
   statement chunk inside synthetic `main` (raw mid-function `#line` is
   unsafe for `name@(args) @destroy` parsing). Markers are unmasked to
   `#line` before host compile.

#### 9.5.2 Driver invocation

`ccc` treats a first positional argument ending in `.shcc` as an implicit
`run` (shebang-friendly):

```text
ccc [ccc-flags...] path/to/tool.shcc [script-args...]
```

Args after the script path are program arguments (inserted after `--` for the
build-run step). Explicit `ccc run path.shcc [-- args...]` remains valid.

Recommended shebang (run from the repo root; `./cc/bin/ccc` is resolved
relative to the process cwd, not the script path):

```text
#!/usr/bin/env -S ./cc/bin/ccc --as=shcc
```

`--as=shcc` is optional. A `ccc` interpreter shebang without `--as` is still
script kind. `.ccs` inputs are unchanged: they do not auto-run from a bare
positional path.

#### 9.5.2a `@task` entry dispatch

When the driver injects synthetic `main` (no explicit top-level `main`), it
discovers TU-scope task functions and dispatches on the first program argument
when that argument begins with `@`:

| Invocation | Behavior |
| ---------- | -------- |
| `tool.shcc` | Run the synthetic-main statement body (default script). |
| `tool.shcc @` | Print discovered tasks (sorted) to stdout; exit 0. Each line is `name` left-aligned in a width column, then two spaces, then the CCDoc one-line summary (§2.4) when non-empty. |
| `tool.shcc @name args…` | Strip `@name` from `argv`, then call the task (`name(argc', argv')` or `name()`). |
| `tool.shcc @unknown …` | Print an error and the task list (with summaries when present) to stderr; exit 2. |

`ccc` auto-run / `ccc run` / `ccc build run` propagate the program exit
status (including 2). Spawn/driver failures still surface as non-zero.

A **task** is a translation-unit function definition that is opted in by an
immediately preceding CCDoc block containing `@task` (§2.4), with one of these
shapes:

```c
static int name(void) { … }                 /* or empty parameter list */
static int name(int argc, char **argv) { … }
static int name(int argc, char *argv[]) { … }
```

(`static` / `extern` / `inline` optional). The argc/argv form requires an `int`
parameter with no pointer indirection, and a `char` pointer parameter whose
pointer depth is at least two (`*` and a trailing `[]` each count one level).
Prototypes without a body, non-`int` returns, and `main` are not tasks. A CCDoc
`@task` on an incompatible declaration is ill-formed. Functions with a valid
shape but no `@task` tag are not tasks. Explicit `main` disables `@task`
dispatch for that unit; multi-entry scripts omit explicit `main`.

When `@name` selects a task, the statement body is not executed. Task names are
ordinary C identifiers; the CLI `@` sigil is not part of the function name.
Listing summary text is the `@task` tag’s optional text when non-empty,
otherwise the leading free-text summary.

```text
./tools/perf.shcc @perf_baseline
./tools/perf.shcc @perf_regress --update
./tools/perf.shcc @
```

#### 9.5.3 Prelude and headers

`<ccc/script/prelude.cch>` includes `<ccc/std/prelude.cch>` and the script
headers below. Scripts do not `#include` the prelude; the driver injects it.

| Header | Role |
| ------ | ---- |
| `<ccc/stdio.cch>` | `CCStdio` reads; console print (`io.println` / data UFCS / naked aliases) |
| `<ccc/std/cli.cch>` | `@grammar(cli)` comptime engine and argv runtime (`cc_parse_args` / `cc_prepare_args` / `cc_print_usage`). `.shcc` gets this from the script prelude; `.ccs` includes it before `@grammar(cli)`. |
| `<ccc/std/json.cch>` | RFC 8259 JSON codecs (`jstr` / `jstr_enc`). Opt-in; factories are `JsonRfc` / `JsonKeep` / `JsonDom` (`<ccc/std/json*.rules>`). Product schemas stay in the TU. |
| `<ccc/script/pathx.cch>` | Repo-root discovery and `char[:0]` path join |
| `<ccc/script/file.cch>` | Read / write / copy / print by `char[:0]` path |
| `<ccc/script/sh.cch>` | `cc_sh_run`, `cc_script_sh`, `cc_script_ccc`, `cc_script_sh_read`, `cc_script_task_exe`, `cc_script_task_shcc` |
| `<ccc/script/temp.cch>` | `CCTempFile` with Result create and `@destroy` cleanup |

Arena parameters follow the stdlib convention: **arena last** on allocating
APIs. Fallible script helpers return `T !>(CCError)` (or the corresponding
`CCResult_*_CCError` form) unless noted.

#### 9.5.4 `CCStdio` and console print

```c
CCArena a@(megabytes(1)) @destroy;
CCStdio io@(a) @destroy;

char[:] in = io.read_all() !>;
io.write_all(out.as_slice()) !>;
```

`CCStdio` binds an arena for growing reads (`read_all` / `read_line`) and
offers `write_all` / `println` / `eprintln` that take a `CCSlice` or
`CCString`. When script `io` is in scope, preferred examples are handle-first.
Data-first UFCS and naked aliases remain valid (UFCS either way on the chosen
receiver):

```c
io.println(path);               /* preferred when io is in scope */
io.eprintln(line);
io.println(@string(`n=${n}`, a));

path.println();                 /* also OK: UFCS on data */
"literal".println();            /* lit/cstr → CCSlice temp → cc_slice_* */
println(path);                  /* naked alias → cc_println */
path.fprintln(STDERR_FILENO);   /* UFCS: data, then fd */
fprintln(STDERR_FILENO, path);  /* naked: fd first, then data */
```

When the *data* is the UFCS receiver, `CCSlice` / `CCString` call `cc_slice_*` /
`cc_string_*`; C string and string-literal receivers coerce to a `CCSlice`
temporary then `cc_slice_*`. There is no `cc_char_*` UFCS print family
(`cc_char_*` / `_Generic` arms are free-sugar / lowered-C only).

Console print returns `void ?>(CCPrintError)` (or `size_t ?>(CCPrintError)` when
the ok payload is the byte count). `CCPrintError` is a separate domain from
`CCError` — diagnostic print is not load-bearing I/O. Bare `println` /
`eprintln` at statement position is well-formed. Use `!>` or
`@errhandler(CCPrintError …)` only when a print failure must propagate; keep
`!>` on socket, file, and other load-bearing I/O.

Short names `print` / `println` / `eprint` / `eprintln` / `fprint` / `fprintln`
are not free macros — a function-like `#define println(x)` would steal UFCS
`x.println()`. The `cc_print*` macros exist as lowered-C sugar (driver inject,
naked-alias targets, `-E` desugar).
The injected default `@errhandler(CCError)` prints with
`(void)cc_eprintln(cc_error_str(e))`. Custom handlers should report via
`cc_error_log` / `cc_error_exit` (or `!> { abort(); }` on the print Result when
failure must propagate). Template formatting uses language `@string`.

#### 9.5.5 Path, file, process, and temp helpers

```c
/* @grammar(cli) Opts { … } + cc_prepare_args(Opts, argc, argv, &a, &opts, stderr) */

char[:0] root = cc_script_repo_root(cc_slice_cstr(argv[0]), &a) !>;
char[:0] baseline = cc_script_path_join(root, "perf/compiler_baseline.txt", &a);
if (!cc_script_path_exists(baseline)) { /* … */ }

CCSlice bytes = cc_file_read_path(path, &a) !>;
cc_file_copy(src, dst, &a) !>;
cc_script_print_file(path, &a);

CCTempFile tmp = cc_temp_file(&a) !> @destroy;
cc_sh_run(program, arg, &a) !>;  /* program/arg are char[:0]; literals coerce */

/* @task bodies: forward remaining argv to a repo-relative tool */
return cc_script_task_exe(argc, argv, "scripts/format.sh");
return cc_script_task_shcc(argc, argv, "tools/cc_perf_check.shcc");
```

Path helpers take NUL-terminated borrows (`char[:0]` / `CCSlice` ABI). String
literals coerce at by-value slice parameters; a bound `char*` dest-casts
through `.cast` (`cc_slice_cstr`). Counted `char[N]` uses `p->to_slice_n(n)`.
`cc_script_repo_root` walks from the
current working directory (and, failing that, from `dirname(argv0)`) looking
for a Concurrent-C repo marker (`cc/src/cc_main.c`,
`perf/compiler_baseline.txt`, or `.git`). Returned path slices are
NUL-terminated for C interop.

`cc_sh_run` builds a `CCCommand`, runs it to completion, and fails with
`CCError` when the process exits non-zero.

`cc_script_sh` / `cc_script_ccc` / `cc_script_sh_read` take one `@string`
line (whitespace words; `'…'` / `"…"` keep a word together), set cwd to
the project root, and inherit stdio. `cc_script_sh` runs the first word as
the program. `cc_script_ccc` runs `$CCC` or `<root>/cc/bin/ccc` or `PATH`
`ccc`, and prepends absolute `--out-dir` / `--bin-dir`. `cc_script_sh_read`
returns trimmed stdout as a `CCString` on the function `@scratch` arena
(`cc_script_sh_read_at` takes an explicit arena). Bind the `@string` before
`cc_script_sh_read` when the line must outlive the call.

`cc_script_task_exe` / `cc_script_task_shcc` resolve a path under the repo
root, set cwd to that root, inherit stdio, forward `argv[1..]`, and return the
process exit status (printing a short stderr diagnostic on spawn failure).
`cc_script_task_shcc` builds the `.shcc` with `ccc build --link` into
`bin/<repo-relative-path>` with `/` mapped to `__` and `.shcc` stripped
(cache / mtime gated), then execs that binary — so nested orchestration
pays for a rebuild only when needed, not a second auto-run.

#### 9.5.6 Scripting model

Prefer Concurrent-C structure for script *content*, and scriptlib for
orchestration:

- **Parse:** `@grammar(schema|rules)` (and SERDES where appropriate) over
  file or stdin bytes; `@grammar(cli)` / `cc_prepare_args` over argv.
- **Format:** `@string(\`…${expr}…\`)` into a `CCString` / slice, then
  `CCStdio` or file write.
- **Glue:** path join, temp files, `cc_sh_run` / `cc_script_sh` — thin
  wrappers over `<ccc/std/>` process, dir, and I/O APIs.

Example (stdin transform):

```c
#!/usr/bin/env -S ./cc/bin/ccc --as=shcc

CCArena a@(megabytes(1)) @destroy;
CCStdio io@(a) @destroy;
char[:] in = io.read_all() !>;
/* … transform into out … */
io.write_all(out.as_slice()) !>;
```

Example (parse → map → report) uses the same prelude, a top-level `@grammar`
and helpers at TU scope, and statement body in synthetic `main` — see
`tools/cc_perf_check.shcc` and `examples/serdes/json/tools/minify.shcc`.

#### 9.5.7 Out of scope for scriptlib

Scriptlib does not define:

- JSON, RESP, or other domain codecs
- A second string-formatting language beyond `@string` / `cc_format`
- Implicit global arenas or ambient process state beyond the driver rewrite
  above

Those remain stdlib, language, or application concerns.

---

## 10. FFI and Unsafe Operations

Because CC is a C preprocessor, **native C interop is first-class**. The entire C standard library and existing C code is immediately accessible. This section defines escape hatches for when CC's safety checks must be bypassed:

- **§10.1 `unsafe {}`** — bypassing compile-time checks (slice provenance, sendability)
- **§10.2 `adopt`** — adopting FFI allocations as CC slices

---

### 10.1 `unsafe {}`

`unsafe {}` bypasses compile-time checks for:

- slice provenance
- sendability enforcement

It does **not** disable escape / borrow checks (stack-slice capture into an escaping closure, scope-bound values). Those stay enforced. Mutation-of-share on a spawned closure is a separate hatch: `@unsafe` on that closure (§6.2), not the `unsafe {}` block.

**Rule:** `unsafe {}` affects only the enclosed block and does not propagate to callees unless they are lexically inside the block.

**Rule:** Runtime debug assertions only apply when the relevant metadata exists (e.g., tracked allocations). Slices created in `unsafe` without provenance metadata will not trigger debug assertions that depend on that metadata.

```c
unsafe {
    char* p = get_from_c();
    char[:] s = p[..len];
    // provenance not tracked (id = 0)
}
```

---

### 10.2 Adopting FFI Allocations

C APIs that return owned buffers can be adopted as unique slices:

```c
T[:] adopt::[T](void* ptr, size_t count, void (*deleter)(void*));
```

- Only valid inside `unsafe {}`.
- Produces a **unique** slice (move-only, has destructor).
- `count` is element count, not byte size.
- Deleter receives the original `ptr` and is called exactly once when ownership ends.
- `NULL` deleter is valid (no cleanup action).

**Rule:** `adopt()` slices are unique but **not transferable** via `send_take`. The deleter may not be thread-safe, so cross-thread transfer could cause memory corruption. Use `send` (which copies) to pass adopted data through channels.

**Rule:** Unique slices from `adopt()` are move-only. Copying is a compile-time error. Ownership transfers via function return or `move()`. When the slice is destroyed (scope exit) without being moved, the deleter is invoked.

```c
unsafe {
    auto buf = c_make_buffer();
    char[:] s = adopt(buf.ptr, buf.len, c_free_buffer);
    // s is now a unique slice
}
// c_free_buffer called here when s goes out of scope
```

**Passing to channels:**

```c
unsafe {
    char[:] s = adopt(c_alloc(1000), 1000, c_free);
    @await ch.send(s);       // OK: copies data, s still valid
    @await ch.send_take(s);  // ERROR: adopt() slices not transferable
}
// c_free called here
```

---

## 11. Surface Syntax Notes

This section documents syntactic sugar and conventions:

- **UFCS / Methods** — method call syntax
- **UFCS auto-deref** — pointer convenience
- **Loops** — for-in walk / enumerate / zip / range; C `for (;;)` unchanged
- **Slicing** — subslice syntax
- **String literals** — static slices
- **String-literal `switch` cases** — slice subject with `case "…":`
- **Closures** — lambda syntax
- **Type inference** — `auto` keyword
- **Structs** — struct syntax and initialization
- **Enums** — C enums (no payloads)
- **Generics** — generic types and functions

---

**Methods / UFCS:**

`x.method(args)` uses UFCS lowering. Dispatch is selected from the resolved receiver type, and libraries define custom lowering through type-owned registration, normally `@typehooks on …`. Depending on the API, the lowered call may pass the receiver by value or by pointer; that is family policy rather than surface syntax.

```c
tx.send(v);        // lowers to send(tx, v)
rx.recv(&out);     // lowers to recv(rx, &out)
tx.close();        // lowers to close(tx)
holder.arena.free();   // dispatches on holder.arena
ptr->arena.free();     // dispatches on ptr->arena
slice.len;         // field access (not a call)
```

**Rule:** The full expression to the left of `.` or `->` is the receiver. Registered UFCS families use the callee chosen by their handler; ordinary fallback UFCS uses the receiver-type method family for the resolved type. Channels follow the same model: the surface form is `tx.send(v)` / `rx.recv(&out)`, while generated C may lower those further to `cc_channel_`*, `CC_TYPED_CHAN_`*, or other family-owned helpers.

**UFCS receiver syntax:**

UFCS uses the receiver operator honestly:

- `x.method(...)` is for value receivers
- `p->method(...)` is for pointer receivers
- `ptr->field.method(...)` is valid when `field` is a value receiver
- `p.method(...)` where `p` has pointer type is invalid

```c
int[~10 >]* tx_ptr = &tx;
tx_ptr->send(42);   // lowers to send(tx_ptr, 42)
tx_ptr->close();    // lowers to close(tx_ptr)

ArenaHolder* holder_ptr = &holder;
holder_ptr->arena.free();   // dispatches on holder_ptr->arena
```

**Rule (UFCS in `@defer`):** UFCS works uniformly in `@defer` statements regardless of whether the receiver is a value or pointer:

```c
int[~10 >] tx;
@defer tx.close();        // OK: lowers to close(tx)

int[~10 >]* tx_ptr = get_tx();
@defer tx_ptr->close();   // OK: lowers to close(tx_ptr)
```

**String-literal `switch` cases:**

A `switch` whose subject has slice type (`CCSlice` and the documented slice
family) may use string-literal case labels:

```c
@switch (name) {
case "GET":
    return 1;
case "A":
case "B":
    return 10;
default:
    return 0;
}
```

**Rule (subject):** The subject expression has a slice type. A known non-slice
subject with string case labels is ill-formed.

**Rule (labels):** Each `case` label is a simple string literal (no escape
sequences). Consecutive string cases fall through as in C. Mixing string and
non-string case labels in one `switch` is ill-formed. Duplicate string labels
in one `switch` are ill-formed.

**Rule (semantics):** Control flow matches ordinary C `switch` over an equality
match of the subject against each label (as by `CCSlice_eq_cstr`).

**Lowering (informative):** Runtime emission may lower string-literal cases
through a perfect-hash / static-map-style table to an integer `switch`.
`@comptime` execution may lower the same surface to an equality
(`CCSlice_eq_cstr`) if/else chain. Observable behavior follows the rules above
in both paths.

**Loops:**

Traditional C `for(;;)` is unchanged. For-in subjects and zip failure
are §9.2.4.

```c
@for (v in s) { ... }            // walk: snapshot or live .len, then .access
@for (&v in s) { ... } !>;       // mut walk: write bound is Result
@for (i, v in s) { ... }         // enumerate
@for (a, b in s, t) { ... } !>;  // zip; void !>(CCError)
@for (i in lo..hi) { ... }       // sequential range
```

**Walk lowering:**

```c
/* frozen slice / T[n], or a grower the body does not resize */
size_t __n = /* s.len at entry */;
T *__p = /* .access pointer at entry */;
for (size_t __i = 0; __i < __n; ++__i) {
    T v = __p[__i];
    /* v = x → v = (__p[__i] = x); */
    BODY
}

/* grower whose body can change extent — write re-reads .len */
for (size_t __i = 0; __i < /* s.len hook */; ++__i) {
    T v = /* .access load */;
    /* v = x → if (__i >= s.len) { e = "for-in write"; goto @errhandler } */
    /*         else access-store */
}

/* @for (i, v in s) { BODY } — i is size_t */
for (size_t i = 0; i < /* s.len hook */; ++i) {
    T v = /* .access load after the bound */;
    BODY
}

/* @for (a, b in s, t) { BODY } !>; */
if (/* s.len */ != /* t.len */) {
    /* CC_ERR_INVALID_ARG — not a silent min; !>; / !>(e){…} sees the kind */
} else {
    for (size_t __i = 0; __i < /* s.len */; ++__i) {
        A a = /* s.access */;
        B b = /* t.access */;
        BODY
    }
}
```

`@for @await` is rejected. Async channel iteration uses an explicit
`while (cc_io_avail(@await rx.recv(&value)))` loop.

**Slicing:**

Subslice syntax creates views into existing slices:

```c
s[start..end]    // elements [start, end)
s[start..]       // elements [start, len)
s[..end]         // elements [0, end)
s[..]            // equivalent to s
```

**Rule (checked-index):** Protected byte-slice index ops (`at` / `get_checked` / `set`) return `CC_ERR_INVALID_ARG` on out-of-bounds or null in **all** builds — no debug/release split. Raw `s.ptr[i]` indexing and unchecked C stores are outside this surface (Gap). Subslice ops that cannot form a valid range yield an empty view.

**String literals:**

String literals used as slice values have static provenance and are sendable.
Slice string-literal coerce (§9.2.0) wraps a bare literal at a by-value
`CCSlice` / `char[:]` / `char[:0]` / `CCSliceShared` / `CCSliceUnique`
parameter or initializer as `CC_SLICE_LIT(lit)`. Prefer `char[:0] s = "hello";`
for sentinel borrows. A bound `char*` dest-casts through `.cast`
(`cc_slice_cstr`). Counted `char[N]` uses `p->to_slice_n(n)`.

**Closures:**

Closures use arrow syntax with optional capture list:

```c
() => { stmt; }               // no parameters, implicit value capture
(x) => { stmt; }              // one parameter (type inferred)
(int x, int y) => { stmt; }   // typed parameters
x => expr                     // single parameter, expression body

// Explicit capture list (after arrow, v3 syntax)
() => [x] { stmt; }           // explicit value capture
() => [&x] { stmt; }          // reference capture (explicit sharing)
() => [x, &y] { stmt; }       // mixed: x by value, y by reference
() => [p = &local] { *p; }    // init-capture: value-capture expr as fresh name
```

**Capture semantics:**

- **Value capture (default):** Closures capture by value. For copyable types, the captured value is a copy. For move-only types, the capture is a move and the original becomes invalid. **Value-captured variables are immutable within the closure.**
- **Reference capture (`[&x]`):** Explicitly captures a reference to the outer variable. The closure shares the variable with the outer scope. Reference captures are subject to mutation checks (see below).
- **Init-capture (`[alias = expr]`):** Value-captures `expr` under a fresh name `alias` (supported forms: `ident`, `&ident`). `[p = &local]` is the idiomatic way to pass a pointer into a mutating callee without reference-capturing `local`. Writing through such a pointer in a task-escaping closure is subject to the same alias-mutation checks as `T* p = &local` outside the capture list.
- **Capture-all banned:** The forms `[&]` and `[=]` (capture all by reference/value) are not allowed. Each captured variable must be listed explicitly.

**Rule (modification requires `[&x]`):** To modify a captured variable, you must use reference capture `[&x]`. Attempting to modify a value-captured variable is a compile error.

**Reference capture mutation check:**

For thread/task closures, reference captures (`[&x]`) are checked for mutation:

- **Read-only access:** Allowed. The closure may read the referenced variable.
- **Mutation:** Compile error unless access uses the shipped C atomic surface,
  a channel, a registered synchronization library, or `@unsafe`.

```c
int counter = 0;
CCNursery n = cc_nursery_create() !> @destroy;

// ✅ OK: read value-captured variable
n.spawn(() => { printf("%d", counter); });

// ❌ ERROR: cannot modify value-captured variable
n.spawn(() => { counter++; });
// error: cannot modify value-captured variable 'counter'
// help: use [&counter] for reference capture

// ✅ OK: read-only reference capture
n.spawn(() => [&counter] { printf("%d", counter); });

// ❌ ERROR: mutation of shared reference
n.spawn(() => [&counter] { counter++; });
// error: mutation of shared reference 'counter' in spawned task
// help: use cc_atomic_*, a registered synchronization library, or @unsafe

cc_atomic_int safe_counter = 0;
n.spawn(() => { cc_atomic_fetch_add(&safe_counter, 1); });

// ⚠️ OK: explicit unsafe (you own this race)
n.spawn(@unsafe () => [&counter] { counter++; });
```

**Mutation patterns detected:**


| Pattern                              | Classification          |
| ------------------------------------ | ----------------------- |
| `x = ...`                            | Write (error)           |
| `x++`, `++x`, `x--`, `--x`           | Write (error)           |
| `x += ...`, `-=`, `                  | =`, etc.                |
| `foo(&x)` where foo takes `T`*       | Potential write (error) |
| `foo(&x)` where foo takes `const T`* | Read (OK)               |
| `y = x`, `f(x)`, `x.field`           | Read (OK)               |


For thread/task closures, captured values must also be capturable (see §2.2).

**Type inference:**

`auto` infers the type from the initializer:

```c
auto x = 42;                // int
auto t = work();            // CCTaskIntptr where work is @async
auto it = iter(&m);         // MapIter::[K, V]
```

**Structs:**

Struct syntax follows C with designated initializers:

```c
struct Point { int x; int y; }
struct Msg { int id; char[:] body; }

// Initialization
Point p = { .x = 1, .y = 2 };
Point q = { 1, 2 };              // positional
Msg m = { .id = 1, .body = s };

// Compound literals
use((Point){ .x = 1, .y = 2 });
```

**Enums:**

Enums are C enums. They have no payloads and no `is` pattern. Construction and matching are ordinary C (`E_Arm`, `==`, `switch`). Error kinds that carry an OS code use a struct field (shipped: `CCIoError.os_code` on a `CCError` face), not an enum payload.

```c
enum Color { Color_Red, Color_Green, Color_Blue };

enum Color c = Color_Red;
if (c == Color_Green) { ... }
switch (c) {
case Color_Blue: ...
}
```

**Generics:** See §12 for comprehensive generics documentation.

**Built-in generic types:**

- `CCTaskIntptr` — pollable async task handle
- `Vec::[T]` — dynamic array
- `Map::[K, V]` — inline open-addressing hash map
- `ArrayMap::[K, V]` — probe index + dense key/value rows
- `T[~... >]` / `T[~... <]` — channel handles for element type T

**Built-in non-generic types:**

- `CCNursery` — structured concurrency handle (§8.1)
- `CCTurnstile` / `CCTurnstileRW` — depth + ordered stages (§8.12)
- `Arena` — memory arena
- `Ordering` — memory ordering enum (`relaxed`, `acquire`, `release`, `acq_rel`, `seq_cst`)
- `Duration` — time span (secs + nanos)
- `CCDeadline` — deadline object (§8.5)
- `CCIoError` / `CCError` — shipped error faces (§3.1, Appendix E)

---

## 12. Generic Family Instantiation

`Name::[args]` is an instantiation use of a library-owned generic family.
It is not declarative generic syntax. Declarations such as
`struct Pair::[A, B] { ... }`, `void swap::[T](...)`, generic parameter
lists, partial inference, and generic impl blocks are unsupported and
produce compile-time errors.

Use-site arguments are type spellings or non-negative decimal integer
literals (`SmallVec::[int, 8]`). Multi-word types join with `_` in the
concrete name (`SmallVec::[long long, 8]` → `SmallVec_long_long_8`); the
factory receives each argument as a C spelling string slice (`arg(0)` is
`long long`). Other numeric spellings (floats, hex, a leading minus) are
ill-formed.

### 12.1 Registered factories

A library defines a family with `CC_GENERIC_FACTORY(Name[, arity])` or the
underlying `cc_generic_register` API (§14.10). At each `Name::[args]` use, the
compiler:

1. computes the canonical concrete name,
2. invokes the registered base factory once for that concrete name,
3. invokes registered extensions in registration order,
4. splices the returned C definition before first use, and
5. rewrites the source use to the concrete C name.

The base factory must exist and return a non-empty C fragment. A
`Name::[args]` use with no registered factory for `Name` is ill-formed,
diagnosed at the use site. Each concrete name is emitted once per
translation unit. Extensions may return an empty fragment.

`T[:]` with a non-char element is sugar for `CCSlice::[T]` and uses the
`CC_GENERIC_FACTORY(CCSlice, 1)` registered in `cc_slice.cch`. `char[:]`
stays the erased `CCSlice` type and does not instantiate.

`Vec::[T]` and `vec_new::[T]` use `CC_GENERIC_FACTORY(Vec, 1)` registered
in `vec.cch`. The concrete type is the struct `CCVec_<T>`. A header
`CC_VEC_DECL_ARENA` that names the same `CCVec_<T>` instance (the shipped
`CCVec_char` / `CCVec_size_t`) suppresses the splice.

`Map::[K,V]` and `map_new::[K,V]` use `CC_GENERIC_FACTORY(Map, 2)` registered
in `map_forward.cch`. The concrete type is the pointer `Map_<K>_<V>*`. A
hand-written `CC_MAP_DECL_ARENA` that names the same `Map_<K>_<V>` instance
suppresses the splice.

`ArrayMap::[K,V]`, `array_map_new::[K,V]`, and `array_map_new_count::[K,V]`
use `CC_GENERIC_FACTORY(ArrayMap, 2)` registered in `array_map.cch`. The
concrete type is the pointer `ArrayMap_<K>_<V>*`. A hand-written
`CC_ARRAY_MAP_DECL` that names the same `ArrayMap_<K>_<V>` instance
suppresses the splice.

An instance is requested by `Name::[args]`, by `T[:]` for the slice family,
or by the mangled type in a type position (a declaration, parameter, field,
or typedef). An identifier `Family_rest` in expression text is not a request.

**Rule (free-name member calls, normative).** For every registered family,
`<snake(Family)>_<member>::[args](call-args)` lowers to
`<Family>_<mangled args>_<member>(call-args)` — the same grid as
`vec_new::[T]` / `map_new::[K, V]`. snake(Family) is the family name
lowercased with `_` inserted before an uppercase letter that follows a
lowercase one (`Pair` → `pair`, `LruCache` → `lru_cache`). The call requests
the instantiation, so the monomorph is emitted even when the type is spelled
nowhere else; the named member must exist in the instance's member set. A free
name followed by `::[` that matches neither a built-in generic form nor a
registered family's grid is ill-formed, diagnosed with the spelling and the
registered families.

**Rule (member-position generic calls, normative).** `recv.member::[args](call-args)`
where `recv` has type `Foo` — or `CCFoo`, since families are spelled without the
`CC` prefix — resolves to the registered factory `<snake(Foo)>_<member>`. The
receiver becomes the instance's first argument, so the call is equivalent to the
free-name spelling `<snake(Foo)>_<member>::[args](recv, call-args)` and lowers to
the same monomorph. A receiver that is not already a pointer is passed by
address. Both spellings request the same instantiation; neither is primary.

Resolution requires the receiver's type, so the receiver must be an expression
whose type is known at the use site. The type arguments are explicit: a member
call carries no destination to infer them from.

`::[...]` on a member that names neither a type formal nor a registered factory
is ill-formed, diagnosed with the receiver type, the factory name that would
have resolved it, and the registered factories.

### 12.2 Library-owned policy

The family owns emitted definitions, C symbol names, internal erasure, linkage,
and specialization. The compiler owns registration lookup, canonical argument
spelling, duplicate-emission prevention, source attribution, splice placement,
and use-site rewriting. The language does not infer generic parameters from
ordinary calls.

Shipped families include `Vec::[T]`, `Map::[K,V]`, `ArrayMap::[K,V]`, result
families, and registered user/library families. Their public C names and
operations are defined by the owning headers. A family may emit specialized C
for one instantiation and erased wrappers for another without changing the
`Name::[args]` source rule.

### 12.3 UFCS composition

After instantiation and type resolution, UFCS dispatch uses the concrete
receiver type under §9.0. A generic factory may emit its own C operations and
register a type or family UFCS hook. Generic instantiation does not create a
second method system, does not invent a member from the instance being
scheduled, and does not bypass the C-member-first rule in §9.0. A method
exists when the instance fragment or a later declaration in the translation
unit defines `Name_meth` (or the family header's `##_` set names it). An
unknown method is ill-formed: the compiler diagnoses the miss with the
receiver type and the instance's installed methods. The UFCS operator is the
receiver shape: `.` on a value (first argument `&recv`), `->` on a pointer
(first argument `recv`). Map and ArrayMap sugar is `Name*` — an arena-allocated
header — so calls use `->`. Vec is the struct itself, so calls use `.`.

Generated C is ordinary first-class C: it participates in parsing, type
checking, linkage, diagnostics, emitted-C inspection, and subsequent UFCS/type
registration exactly as other lowered C does.

---

## 13. Collections

Standard collection types are defined in the **Standard Library Specification** (`concurrent-c-stdlib-spec.md`):

- `**Vec::[T]`** — arena-backed dynamic array (`<std/vec.cch>`)
- `**Map::[K,V]`** — arena-backed inline open-addressing map (`<std/map.cch>`)
- `**ArrayMap::[K,V]`** — arena-backed index + dense rows (`<std/array_map.cch>`)

These types are generic, use UFCS methods, and require a `CCArena` at
construction. See the stdlib spec for full API reference, rules, and examples.

**Quick reference:**

```c
// Vec::[T]
Vec::[T] v@(arena) @destroy;
v.push(value);
v.truncate(n);
T* x = v.get_ptr(index);
T[:] slice = v.as_slice();

// Map::[K,V] — tiny K/V, max probe locality
Map::[K, V] m@(arena) @destroy;
m->insert(key, value);
V* x = m->get_ptr(key);
m->remove(key);

// ArrayMap::[K,V] — wide values; empty buckets stay cheap
ArrayMap::[K, V] am@(arena) @destroy;
ArrayMap::[K, V] sized = array_map_new_count::[K, V](arena, 1024);
am->insert(key, value);
V* y = am->get_ptr(key);
am->del(key);
```

`Vec::[T]` instantiates `CC_GENERIC_FACTORY(Vec, 1)`
in `vec.cch` (`Vec::[int]` → `CCVec_int`). `Map::[K,V]` instantiates
`CC_GENERIC_FACTORY(Map, 2)` in `map_forward.cch` (`Map::[int,int]` →
`Map_int_int*`). `ArrayMap::[K,V]` instantiates `CC_GENERIC_FACTORY(ArrayMap, 2)`
in `array_map.cch` (`ArrayMap::[int,int]` → `ArrayMap_int_int*`). The
CC-prefixed spellings (`CCVec::[T]`, `cc_vec_new::[T]`) name the same Vec
instances and remain accepted as the instance layer. UFCS method calls on
containers lower through that family contract; implementations may use direct
concrete symbols such as `CCVec_int_push(&v, x)` or thin family wrappers over
shared erased-core helpers. See the stdlib spec for full lowering rules.

---

## 14. Compile-Time Evaluation (`@comptime`)

This section defines output-only C staging. `@comptime` code is compiled and
executed during translation through the C ABI exposed by the included headers.
Its effects are compile outputs: emitted C, registrations, instantiation
requests, diagnostics, and projected literals. It is absent from the output
program unless it explicitly emits C.

The staged program uses ordinary C values and ABI calls. Reflection exposes
copied names, type spellings, scalar metadata, and field records; it does not
expose compiler-owned AST pointers.

- **§14.1 Constant expressions** — what counts as compile-time
- **§14.2 `@comptime` declarations** — compile-time storage
- **§14.3 `@comptime` parameters** — compile-time arguments
- **§14.4 `@comptime if`** — compile-time branching
- **§14.4a `@comptime(expr)`** — value-position literal hoisting
- **§14.5 `@comptime {}` blocks** — compile-time execution for initialization
- **§14.6 Built-ins** — minimal type/ABI queries
- **§14.7 Static assertions** — compile-time invariants
- **§14.8 Restrictions** — what `@comptime` cannot do
- **§14.9 Compile-time emission** — generating C from `@comptime` code
- **§14.10 User-defined generic lowering** — templates and compiled factories
- **§14.11 Compile-time reflection** — type and field introspection

---

### 14.1 Constant Expressions (Normative)

A **constant expression** is an expression evaluable during compilation.

Constant expressions may use:

- Literals
- `sizeof(T)`, `alignof(T)`, `offsetof(T, field)`
- Arithmetic/bitwise/boolean operations
- Casts between integer types (if no overflow beyond target width)
- References to other `@comptime` values
- Calls to `@comptime` functions (see §14.2), if all arguments are constant expressions
- Enum values

**Rule:** A constant expression must not depend on runtime state (globals with runtime initialization, function calls without `@comptime`, I/O, allocation, atomics, mutexes, channels, tasks).

---

### 14.2 `@comptime` Declarations

`@comptime` on a variable requires compile-time evaluation and gives it static storage duration.

```c
@comptime int A = 1 + 2;
@comptime size_t PAGE = kilobytes(4);
@comptime char[:] VERSION = "1.0.0";
```

**Rule:** The initializer must be a constant expression.

**Rule:** `@comptime` variables are immutable.

**`@comptime` functions:**

Functions marked `@comptime` can be evaluated at compile time:

```c
@comptime int fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

@comptime size_t align_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

@comptime int mask(@comptime int bits) {
    return (1 << bits) - 1;
}
```

**Rule:** A function used to produce a constant expression may only call other
constant-evaluable functions and use constant-evaluable operations. A staged
function invoked by an executing `@comptime {}` block or generic factory may
call C ABI functions available to the compile-time translation unit, including
arena allocation, registration, emission, and file I/O. Those effects occur
during translation and cannot be projected as live runtime pointers.

**Rule:** `@comptime` functions can also be called at runtime (they are valid runtime functions too).

---

### 14.3 `@comptime` Parameters

Functions may take compile-time parameters explicitly:

```c
@comptime int mask(@comptime int bits) {
    return (1 << bits) - 1;
}

@comptime int M = mask(8);  // M = 255
```

**Rule:** Arguments passed to a `@comptime` parameter must be constant expressions.

**Rule:** A `@comptime` parameter may be used wherever a constant expression is required (array lengths, channel capacities, switch case values, etc.).

**Important interaction with generics:** `@comptime` parameters express
compile-time values in ordinary functions (array lengths, channel
capacities). Generic factories also receive decimal integer use-site
arguments as string slices (`Name::[int, 8]`).

```c
// @comptime parameter drives array size
int sum_n(@comptime int N, int[N] xs) {
    int sum = 0;
    for (int i = 0; i < N; i++) sum += xs[i];
    return sum;
}
```

---

### 14.4 `@comptime if`

`@comptime if (COND) { ... } else { ... }` chooses a branch at compile time.

```c
void print_any::[T](T x) {
    @comptime if (is_slice(T)) {
        print_slice(x);
    } else {
        print_primitive(x);
    }
}

void serialize::[T](T value, char[~]* out) {
    @comptime if (is_slice(T)) {
        serialize_slice(value, out);
    } else @comptime if (is_result(T)) {
        serialize_result(value, out);
    } else {
        serialize_primitive(value, out);
    }
}
```

**Rule:** `COND` must be a constant expression.

**Rule:** Only the selected branch is type-checked and lowered; the other branch is discarded. This enables type-specific code that would otherwise fail to compile.

**Rule (splice, not block):** The branch braces delimit the text to splice; they do not introduce a scope. The selected branch's text lands directly in whatever scope the construct sits in — the same construct emits declarations at file scope, where a block would be a syntax error. A branch that declares a name declares it in the enclosing scope. Write an explicit block when a scope is wanted:

```c
@comptime if (is_fallible(T)) { { T v = call() !>; use(v); } }
```

The same rule governs `@comptime for` bodies (§14.11): each iteration splices unbraced, so a body that declares a name needs an explicit block to declare it once per iteration rather than once per program point.

---

### 14.4a Value-position `@comptime(expr)` (Normative)

**Form:** `@comptime(` *expr* `)` — distinguished from `@comptime { ... }` (block), `@comptime if/for` (control flow), and `@comptime` function definitions by the token immediately following `@comptime` being `(`.

**Semantics:** The implementation evaluates *expr* at compile time (with registered `@comptime` functions in scope) and **splices the result in place** as a C constant-expression literal. The hoisted literal is visible in the lowered translation unit (e.g. `int i = 60;`), so the transformation is auditable without replaying the comptime evaluator.

```c
@comptime int fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

int main(void) {
    int         i = @comptime(fib(10) + 5);       /* lowers to: int i = 60; */
    double      d = @comptime(1.0 / 4.0);         /* 0.25 */
    const char* s = @comptime("hello, " "world"); /* "hello, world" */
    int         a[@comptime(4)];                  /* array bound */
}
```

**Rule (pipeline order):** Value hoisting runs **after** `@comptime if/for` pruning and **before** `@emit` / `@string` template lowering. Only live (non-pruned) sites are evaluated.

**Rule (projectable types):** The value must be projectable to a C literal: integers, floating point, `_Bool`, and strings (`char*`, `const char*`, string literals, and `CCSlice` with string contents). Pointers to comptime memory, aggregates, and other non-scalar values are a compile-time error at the use site.

**Rule (caching):** Identical *expr* text within one translation unit may be evaluated once and reused (implementation-defined memoization).

**Rule (memory):** Literal projection uses stack-first growable `CCArena` storage (same pattern as `@emit`). The hoist pass owns one arena per translation unit; `cc_comptime_exec_eval_literal` copies the projected text into the caller-supplied arena. There is no fixed byte cap beyond available memory.

**Rule (API):** `cc_comptime_exec_eval_literal(expr, opts, &lit, &len, …, arena)` requires a non-NULL `arena` as the final argument; returned `lit` points into arena storage until `cc_arena_free(arena)`.

**Rule (diagnostics):** Errors are reported at the `@comptime(` use site with source location; the user need not inspect generated C to locate the failure.

**Distinction from `@comptime` declarations:** `@comptime T name = expr;` introduces compile-time **storage**; `@comptime(expr)` is an **expression-position** splice with no persistent symbol.

---

### 14.5 `@comptime {}` Blocks

A `@comptime { ... }` block runs during compilation and may be used to initialize `@comptime` variables and static data.

```c
@comptime u32 CRC_TABLE[256];

@comptime {
    for (int i = 0; i < 256; i++) {
        CRC_TABLE[i] = crc32_seeded(i);
    }
}
```

**Registration and lowering note (normative):**

- `@comptime` blocks are also the place where libraries publish compile-time registrations such as the rewritten form of `@typehooks` / `cc_type_register(...)`.
- Registrations must work from ordinary source files and ordinary included headers. User code must not need special registration-only macros or source guards.
- The implementation preprocesses and canonicalizes the full translation unit,
  including included `.cch` headers, before executing compile-time registration
  code. Hidden ephemeral lowered headers or equivalent internal forms may be
  used while collecting registrations.
- The observable rule is source-first: registrations written in normal CC source participate in the same build without requiring a separate user-maintained registration artifact.

**Pipeline note (informative):** One valid implementation strategy is:

1. Canonicalize/preprocess CC for compile-time discovery.
2. Execute compile-time code and collect registrations for the whole translation unit, including included local headers.
3. Lower the translation unit to ordinary C using the collected registrations.

**Rule:** A `@comptime {}` block may assign only to:

- `@comptime` variables
- Compile-time-known static storage declared in the same translation unit

**Rule:** Control flow inside `@comptime {}` is allowed (`if`, `for`, `while`,
`switch`) as long as all conditions are constant-expression decidable.
String-literal case labels (§11) are valid in `@comptime {}` with the same
surface rules as at runtime.

**Rule (`type_of` in an executed block):** The body of `@comptime {}` is compiled
as host C. Structural `type_of(T)` members that appear there are lowered to host
calls before that compile — `type_of(T).nfields` is `cc_reflect_field_count("T")`
for a user type, the same count `@comptime for (f in type_of(T).fields)` walks.
`@comptime for` in the main translation unit is a compile-time unroll, not host C,
and keeps the `type_of` spelling.

---

### 14.6 Built-in `@comptime` Queries

The following are available in constant expressions:

```c
@comptime size_t sizeof(type T);
@comptime size_t alignof(type T);
@comptime size_t offsetof(type T, field F);

@comptime bool is_pointer(type T);
@comptime bool is_slice(type T);
@comptime bool is_result(type T);

// Helpful for specialization:
@comptime bool is_sendable(type T);
@comptime bool is_copyable(type T);
```

**Rule (`is_sendable` / `is_copyable`):** These are defined by the same structural rules as §2.1 and §2.2. They are compile-time predicates over types.

---

### 14.7 Static Assertions

`@comptime_assert` checks conditions at compile time:

```c
@comptime_assert(sizeof(int) == 4, "expected 32-bit int");
@comptime_assert(alignof(void*) >= 4, "pointer alignment too small");
@comptime_assert(BUFFER_SIZE >= 1024, "buffer too small");
```

**Rule:** If the condition is false, compilation fails with the provided message.

**Rule:** `@comptime_assert` is a statement, and its condition is folded by the compiler — structural type facts (`type_of(T).kind`, `.nfields`) and host-C constant expressions over in-scope types. It cannot see a construct whose meaning depends on a local declaration at the call site, `_Generic` above all.

`cc_static_assert(COND, why)` covers that case: an assertion in **expression position**, whose condition the host compiler folds.

```c
#define KIND_SUPPORTED(x) _Generic((x), int: 1, double: 1, default: 0)
#define KIND(x) \
    (cc_static_assert(KIND_SUPPORTED(x), kind_wants_an_int_or_a_double), \
     _Generic((x), int: kind_of_int, double: kind_of_double, \
                   default: kind_of_other)(x))
```

**Rule:** `why` is an **identifier**, not a string, and it is the diagnostic. A failing assertion reports it at the call site — `error: negative width in bit-field 'kind_wants_an_int_or_a_double'` — so the identifier should read as the sentence a caller needs.

**Rule:** A type-dispatching macro that can be given an unsupported type SHOULD guard itself this way, and SHOULD keep a `default:` arm in the dispatch it guards. The assertion says what is wrong; the arm's argument mismatch names the offending type. Neither alone is enough: without the assertion, a missing arm reports only "does not match any association", and a `default:` arm calling an incompatible function is a warning that defers the real failure to link time.

**Guidance:** `cc_static_assert` is deliberately C89 — bitfields and struct types inside `sizeof` both predate C99, and a negative width is a constraint violation every conforming compiler must diagnose. It needs no statement expression and no advertised C version, so it holds on hosts where `_Static_assert` would be replaced by a message-losing compatibility macro.

---

### 14.8 Restrictions

Staged execution may call C functions available to the compile-time C
translation unit, including arena allocation and file I/O. Such effects happen
while compiling and do not become runtime effects of the generated program.
Pointers into staged storage may not be projected into emitted runtime C as
live pointer values. Emission APIs copy or retain bytes according to their
documented arena contract. Undefined behavior in staged C has the same
consequences as undefined behavior in host C execution.

---

### 14.9 Compile-Time Emission

`@comptime {}` blocks and `@comptime` functions execute in an in-process compile-time evaluator. Emitted C is spliced into the translation unit at a named anchor.

```c
int cc_emit_cstr(CCEmitAnchor anchor, const char* c_fragment);
int cc_emit_format(CCEmitAnchor anchor, const char* fmt, ...);   // printf-style

void cc_instantiate_vec(const char* elem_mangled);
void cc_instantiate_map(const char* key_mangled, const char* val_mangled);
void cc_instantiate_chan(const char* elem_mangled);
```

```c
typedef enum CCEmitAnchor {
    CC_EMIT_AFTER_PRELUDE    = 0,   // after the file prelude, before first use
    CC_EMIT_BEFORE_FIRST_USE = 1,   // immediately before the first use site
    CC_EMIT_AT_COMPTIME_SITE = 2,   // at the @comptime block's source position
} CCEmitAnchor;
```

**Rule:** `cc_emit_*` text is splice-once per anchor/site; consecutive emits at the same anchor/site concatenate into one block.

**Rule (anchor after what the fragment references).** A fragment is spliced verbatim at its anchor and is subject to ordinary C declaration order. Emitted code that calls or names something declared in the source must be anchored where that declaration already stands — `CC_EMIT_AT_COMPTIME_SITE` for a `@comptime` block that follows the declarations it reflects over. `CC_EMIT_AFTER_PRELUDE` precedes the file's own declarations, so a wrapper spliced there calls an undeclared function; the real definition then conflicts with the implicit one, and the error names the *source* line rather than the emitted call.

**Rule (site splice into an enumerator list).** When `@comptime {}` or `@comptime for` sits inside `enum { … }`, `CC_EMIT_AT_COMPTIME_SITE` inserts the fragment as enumerators at that construct — not after the translation unit. A trailing enumerator written after the construct (for example `NPROP`) follows the last emitted member, so its implicit value is one past the last explicit member. Arrays sized by that enumerator use that count.

**Rule (a fragment is host C).** Splicing happens after the passes that lower CC syntax, so a fragment may not contain `!>`, `?>`, `@errhandler`, or any other construct those passes handle — the parser reports `'@' statements require CC external parser`. Emitted code consumes a Result through the accessors (§Results), which are ordinary macros.

**Rule (a template's text is emitted once per instance).** Everything between the backticks reaches every generated copy, comments included. Explanatory prose belongs beside the generator, not inside its template, where it is duplicated code rather than documentation.

**Rule:** `cc_instantiate_*` forces monomorphization of a built-in family even when the type is never spelled in source.

**Two `@emit` spellings.**

| Form | Returns | Use when |
|------|---------|----------|
| `@emit(\`...\`, arena)` | `CCSlice` | Generic factories and any `@comptime` function that builds definition text and returns it; the fragment is built into the caller-supplied `CCArena` |
| `@emit(CCEmitAnchor, \`...\`)` | `void` (lowers to splice side effect) | `@comptime {}` blocks and `@comptime for` bodies that emit declarations at a named anchor |

Both forms share the same backtick `${...}` grammar as `@string`. Each `${expr}` slot uses type-driven dispatch (`cc_emit_tpl_append_slot` / C11 `_Generic`); supported types are `CCSlice`, C strings (`char*` / `const char*` / char arrays), integers, and floating-point.

**Rule (anchored `@emit` diagnostics):** A compile error inside `@emit(anchor, \`...\`)` is reported in the source file at the template line that caused it — not a cache path, not `emit.c`, and not the `@comptime` keyword. An undeclared `${expr}` names the interpolation. A host-C error in the generated fragment names the corresponding template line and shows that line.

**Rule:** The return form takes an explicit `arena` argument and **must** supply one; the anchored form takes no arena (it builds into a private stack arena, splices, and frees). Mixing the two — an arena on the anchored form, or a missing arena on the return form — is a compile error.

**Rule:** `@comptime for` bodies that contain a backtick `@emit` are wrapped in `@comptime { }` per unrolled iteration so template lowering runs through the comptime executor.

**Memory model:** `@emit` builds through a `CCString` over a `CCArena` (stack-first, heap-spill), so a single fragment has no fixed byte cap — large templates grow into arena heap rather than erroring. The arena owns the fragment bytes for the return form (the caller persists or copies them before freeing the arena); the anchored form's arena is local and freed after the splice. The factory's final returned definition is still bounded by the splice buffer (`CC_EMIT_TPL_BUF_SIZE`, 8192 bytes) and overflow there is a compile error, not silent truncation.

---

### 14.10 User-Defined Generic Lowering

A library defines how its generic lowers to C with a single mechanism: a compiled
`@comptime` factory bound to a name. The canonical form is the `CC_GENERIC_FACTORY`
sugar, which lowers to a `cc_generic_register` call plus the factory function:

```c
CC_GENERIC_FACTORY(Pair, 2) {
    return @emit(`
        typedef struct { ${arg(0)} first; ${arg(1)} second; } ${mangled};
        static inline ${mangled} ${mangled}_make(${arg(0)} a, ${arg(1)} b) {
            ${mangled} r; r.first = a; r.second = b; return r;
        }`, arena);
}
```

The factory body has implicit parameters and returns the definition text via
`@emit(\`...\`, arena)`:

- `generic_name` (`CCSlice`) — the registered name, e.g. `Pair`
- `mangled` (`CCSlice`) — the mangled instantiation name at the use site
- `type_args` (`CCSliceArray`) — the type-argument spellings
- `arena` (`CCArena`) — scratch arena for building the fragment

**Sugar ergonomics.** The implicit parameters are auto-voided, so a body that
ignores one needn't write `(void)…`. `arg(i)` is shorthand for
`type_args.items[i]` (available inside factory bodies). The optional integer
arity in `CC_GENERIC_FACTORY(Name, N)` injects the standard guard
`if (type_args.len < N || !mangled.ptr) return cc_slice_empty();` so the body
needn't repeat it; omit it (`CC_GENERIC_FACTORY(Name)`) to do your own argument
checking.

`cc_generic_register("Name", handler)` is the underlying primitive; the sugar is
the preferred form:

```c
int cc_generic_register(const char* name, void* factory);
```

**Guidance:** `CC_GENERIC_FACTORY` and `cc_generic_register` produce identical lowering (the sugar expands to a `@comptime` factory function plus a `cc_generic_register` call), so use the sugar by default; reach for `cc_generic_register` directly only to decouple the factory from its registration — registering one factory under multiple names, reusing the factory function elsewhere, or registering programmatically.

**Rule:** At a `Name::[args]` use site the compiler computes a unique mangled name, invokes the factory once per distinct instantiation, splices the result, and rewrites the use site to the mangled name. Returning the empty slice is a lowering failure.

**Extending a generic.** A generic name has exactly one *base* factory
(`CC_GENERIC_FACTORY` / `cc_generic_register`) plus any number of *extension*
factories declared with `CC_GENERIC_FACTORY_EXTEND(Name[, arity]) { … }` (sugar
for the `cc_generic_register_extend("Name", handler)` primitive). Extensions let
operations on a generic type be defined separately from — and without editing —
the factory that defines the type, e.g. core methods in one header and optional
ones in another:

```c
// core: defines the type
CC_GENERIC_FACTORY(Box, 1) {
    return @emit(`typedef struct { ${arg(0)} value; } ${mangled};`, arena);
}

// elsewhere: adds an operation, no edit to the base
CC_GENERIC_FACTORY_EXTEND(Box, 1) {
    return @emit(`static inline ${arg(0)} ${mangled}_get(${mangled} b)
                  { return b.value; }`, arena);
}
```

**Rule (extension lowering):** At a use site the base factory runs first (it must
define the type), then every extension in registration order; the fragments are
concatenated into the single definition emitted once per mangled name. Because
the base runs first, extension fragments may reference the base's symbols
(`${mangled}`, its fields). Registration order across files is irrelevant — the
base may register before or after its extensions.

**Rule (base required):** A base factory must return a non-empty fragment.
Instantiating a name that has only extension factories and no base is a
compile-time error at the use site (*"generic 'X' is extended but never
defined"*). An *extension* may return the empty slice to emit nothing, which is
the supported way to specialize conditionally (e.g. emit `_inverse` only when
`R == C`).

**Rule:** Each `${expr}` slot in `@emit(\`...\`, arena)` lowers to `cc_emit_tpl_append_slot(...)`, which uses C11 `_Generic` on the expression type to pick the append helper (`CCSlice`, integers, floating-point, or C string). Slot dispatch does not depend on variable names.

A factory compiles in-process on the libtcc comptime evaluator on first use (the same evaluator that runs `@comptime` blocks). First-use lowering is in the millisecond range. The relocated factory code stays resident for the remainder of the compile; if libtcc is unavailable the compiler falls back to a host-compiled shared object.

**Rule:** This is the same registration machinery as UFCS custom lowering (§9.0): the library owns the C lowering, the compiler owns the splice.

**Rule (invalid emit is attributed to the use site):** Before a factory's returned definition is spliced, the compiler validates it for **syntactic** well-formedness. A syntactically invalid emit is reported at the **use site** (`Name::[args]` file:line:col), not as an error buried in the merged translation unit. The diagnostic includes the full generated definition (line-numbered, with the offending line flagged) and a note locating the originating factory by `file:line` — resolved through `#line` directives, so a factory harvested from a header blames the header source the author wrote. Validation is syntax-only: a syntactically valid emit that is *semantically* wrong (e.g. references an unknown type) is still surfaced by the host C compiler against the merged unit.

**Tooling:** `--emit-c-inspect[=PATH]` writes the merged translation unit to a file (default `out/<stem>.inspect.c`) for inspection. On a successful lowering it is the full pre-parse merged unit; when lowering fails inside a generic factory it is the unit reconstructed in source context up to the first blocking error (the closest inspectable artifact to the final lowered C, which is not produced for input that cannot be parsed). The flag does not change whether the build succeeds or fails; it only writes the artifact. This is implementation tooling, not part of the conformance surface.

---

### 14.11 Compile-Time Reflection

`type_of(T)` yields compile-time type information. Numeric and structural members fold to constant expressions usable anywhere a constant is required:

```c
type_of(T).size        // size_t  — sizeof(T)
type_of(T).align       // size_t  — alignof(T)
type_of(T).kind        // cc_type_kind
type_of(T).nfields     // field count (in `@comptime {}`, the host reflect count)
type_of(T).name        // const char* display spelling
```

`@comptime for` unrolls a body once per declared field of a struct `T`:

```c
@comptime for (f in type_of(T).fields) {
    // f        -> the field identifier   (t.f resolves to t.<field>)
    // f.name   -> field name as a string literal
    // f.index  -> 0-based field index
    // f.type   -> the field's type spelling (usable in sizeof, declarations, ...)
}
```

Compiled factories and `@comptime` blocks read the same field set through value helpers or the `cc_reflect_field_*` byte-buffer callbacks:

```c
typedef struct CCReflectField { char name[128]; char type[128]; int index; } CCReflectField;
int cc_reflect_field_at(const char* type_name, int idx, CCReflectField* out);  // 0 ok, -1 err

int cc_reflect_field_count(const char* type_name);                              // -1 if unknown
int cc_reflect_field_name(const char* type_name, int idx, char* buf, int buf_sz);
int cc_reflect_field_type(const char* type_name, int idx, char* buf, int buf_sz);
```

**Rule:** `@comptime for` loop variables (`f.name`, `f.type`, `f.typestr`, `f.index`) and `${...}` slots inside `@emit` share the same field metadata.

**Rule:** `f.is_as` is 1 for a member listed in a `@typeview` `as:` group and 0 otherwise. Composition is a fact about the declaration, not about the type, so it is reflected per field — which is what lets a walk descend through composition:

```c
@comptime for (f in type_of(Wrap).fields) {
    @comptime if (f.is_as) {
        @comptime for (m in type_of(f.type).methods) { /* ... m(&w.f) ... */ }
    }
}
```

**Rule (splice, not block):** The body braces delimit the text to unroll; they do not introduce a scope, and neither does an iteration. Each iteration's text splices directly into the scope holding the construct — which is what lets a loop at file scope emit declarations. A body that declares a name declares it once per iteration in one scope, so reusing a name across iterations needs an explicit block:

```c
@comptime for (m in type_of(T).methods) {
    { m.ret r = t.m(); use(r); }
}
```

**Rule (token substitution vs. template slot).** A loop variable substitutes as **tokens**, before any template is lowered, so `m`, `m.params`, `m.args`, `m.type` and `m.ret` are written bare inside a backtick template and their text lands in the fragment. A `${...}` slot is different: it appends the **value of an expression** at emit time, so only the string-valued members belong there — `${m.name}` and `${f.typestr}` — and that is exactly the case where the text must be pasted into a longer identifier:

```c
@emit(CC_EMIT_AT_COMPTIME_SITE, `
    static int wrap_${m.name} m.params {     /* slot builds the name; params are tokens */
        m.ret v = m m.args;                  /* call the method, forwarding by name */
        return (int)v;
    }
`);
```

Putting a token-valued member in a slot (`${m.params}`) is ill-formed — a parameter list is not an expression — and putting a string-valued one bare yields a string literal where an identifier was meant.

**Rule:** Field name and type accessors write at most `buf_sz - 1` bytes plus a NUL and return the byte count, or `-1` when `idx` is out of range. A returned type spelling may be passed back into `cc_reflect_field_count` to descend into nested struct fields.

**Rule:** The field parser models these member-declarator forms exactly, one `CCReflectField` per declared name:

| Member form | Example | Reflected name / `type` |
|---|---|---|
| Scalar / pointer | `double *p;` | `p` / `double*` |
| Multi-declarator | `int a, *b;` | `a` / `int`, then `b` / `int*` |
| Array (incl. multi-dim) | `char buf[16];` · `int g[2][3];` | `buf` / `char[16]` · `g` / `int[2][3]` |
| Function pointer | `int (*cb)(int, int);` | `cb` / `int (*)(int, int)` |
| Named bitfield | `unsigned f : 4;` | `f` / `unsigned` (width is validated, not exposed) |

For the array and function-pointer forms the `type` spelling carries the extent / signature, so it is exact for `sizeof` and `t.f` access but is **not** usable as a declaration prefix (`${f.type} x;`).

**Rule:** Field reflection is otherwise all-or-nothing. `type_of(T).fields`, `@comptime for`, and `cc_reflect_field_*` share one field parser, which never produces a partial or guessed field set. If `T` is not found, or any member uses a form the parser cannot spell exactly as a usable `type` — an inline anonymous or nested aggregate definition (`struct { ... } m;`), an anonymous member, an unnamed (padding) bitfield, or a pointer-to-array — reflection yields no fields: `cc_reflect_field_count` returns `-1` and `@comptime for` is a compile-time error. (A member that names an already-defined aggregate, e.g. `struct Foo m;`, is an ordinary scalar field and reflects normally.)

#### Method reflection

`type_of(T).methods` enumerates `T`'s methods — the functions whose first parameter is `T` or `T*`, which is what declaring a method means (§9). They are enumerated in declaration order, from the same sources the loop can see:

```c
@comptime for (m in type_of(T).methods) {
    // m          -> the method's identifier (t.m() calls it)
    // m.name     -> the method name as a string literal
    // m.index    -> 0-based index
    // m.type     -> the declared return-type spelling, `!>(E)` included
    // m.ret      -> the ok type alone: `int` for `int !>(CCError)`
    // m.err      -> the error type alone: `CCError`, empty when infallible
    // m.fallible -> 1 when the return type carries `!>(E)`, else 0
    // m.params   -> the declared parameter list, parentheses included
}
```

**Rule:** `m.ret` and `m.fallible` are reflected rather than derived in the emitted code, because fallibility changes the *shape* of what is emitted — a caller unwraps or does not — and shape selection has to happen before the code exists.

**Rule:** `m.params` is the parameter list the source wrote, verbatim and parenthesized, so it is both a sequence and a usable signature fragment. A parenthesized declaration list is a `@comptime for` sequence in its own right, which is what makes the nested walk two primitives rather than a second enumeration mode:

```c
@comptime for (m in type_of(T).methods) {
    @comptime for (p in m.params) {
        // p -> the parameter's identifier; p.name / p.type / p.typestr / p.index
    }
}
@comptime for (p in (int a, const char* s)) { /* the same construct, by hand */ }
```

**Rule:** Every declared parameter is reported, in order, with the receiver at index 0 — being the first parameter is what makes the function a method, so it is a fact reflection states rather than hides. `()` and `(void)` are the empty list and unroll zero times.

**Rule:** A declaration list reflects under the same all-or-nothing discipline as struct fields, through the same declarator parser. An entry that is unnamed, or spelled in a form the parser cannot model exactly, is a compile-time error naming the list — never a dropped entry, which would leave `p.index` naming a different position than the source does.

The compiler routes `@comptime if` predicate evaluation and `@comptime for` field loading through the libtcc comptime executor when `CC_COMPTIME_UNIFIED_EXEC=1` (default). When `CC_COMPTIME_UNIFIED_EXEC=0`, predicate evaluation and field loading use the structural text resolver. Both `@string` and `@emit` share one backtick `${...}` scanner (`preprocess/template_scan.c`). `@emit` slot values are appended via type-driven `_Generic` dispatch in `cc_emit_tpl.cch`, not name heuristics.

---

## 15. Non-Goals and Explicit Omissions

The following are explicitly out of scope for this specification:

- Preemptive cancellation
- Implicit fairness or scheduler guarantees
- Ambient or thread-local cancellation contexts
- Effect typing for async or cancellation
- Automatic memory reclamation beyond arena semantics

These omissions are intentional and preserve explicit control, predictable C lowering, and implementability across platforms.

## Appendix A: FFI Details & Unsafe Code

This appendix provides comprehensive guidance for C interoperability and unsafe operations.

### A.1 `unsafe {}` Block Semantics

`unsafe {}` suspends compile-time safety checks for the enclosed block only. The following checks are disabled:

- **Slice provenance tracking:** Slices created in `unsafe` do not have tracked allocation IDs.
- **Sendability verification:** Non-sendable types may be value-captured in closures within `unsafe`.

Escape / borrow checks remain in force. A stack slice or stack reference that outlives the frame is still a compile error. Mutation of a shared reference capture is waived only by `@unsafe` on that closure (`n.spawn(@unsafe () => [&x] { x++; })`), not by wrapping the spawn in `unsafe {}`.

**Rule (scope):** `unsafe` is lexically scoped. Nested `unsafe` blocks are allowed; rules only apply within the `unsafe` block and do not propagate to function calls unless they are lexically inside the block.

**Rule (metadata):** Slices created in `unsafe` without provenance metadata will not trigger debug assertions that depend on allocation tracking.

**Examples:**

```c
// Adopting raw C pointer
unsafe {
    extern char* c_function();
    char* raw_ptr = c_function();
    char[:] s = raw_ptr[..100];  // provenance not tracked
}

// Casting away sendability (value capture). Escape checks still run.
struct NonSendable { pthread_t tid; };
CCNursery n = cc_nursery_create() !> @destroy;

unsafe {
    NonSendable ns = get_non_sendable();
    n.spawn(() => { use(ns); });  // OK: sendability waived; value copy
}

int x = 0;
unsafe {
    n.spawn(() => [&x] { x++; });  // ERROR: mutation of shared ref
                                    // (use @unsafe on the closure)
}
```

### A.2 Adopting FFI Allocations with `adopt()`

C APIs often return owned buffers. The `adopt()` function integrates these into Concurrent-C's ownership model.

**Signature:**

```c
T[:] adopt::[T](void* ptr, size_t count, void (*deleter)(void*));
```

**Parameters:**

- `ptr`: Pointer to the allocation (may be NULL for zero-length slices)
- `count`: Number of elements of type `T` (not byte size)
- `deleter`: Function called once when ownership ends (may be NULL for no cleanup)

**Constraints:**

- Only valid inside `unsafe {}`.
- Produces a **unique** slice (move-only, non-copyable, has destructor).
- Deleter receives the original `ptr` and is called exactly once when ownership ends.
- `NULL` deleter is valid (no cleanup action).

**Safety Rules:**

1. **Correct size:** `count` must be the correct element count. Off-by-one errors cause buffer overruns or underruns.
2. **Valid pointer:** `ptr` must be a valid allocation (not NULL unless count is 0, not dangling, not stack memory).
3. **Correct deleter:** Deleter must match the allocation mechanism (malloc → free, new → delete, etc.). Mismatches cause corruption or leaks.
4. **Non-transferable:** Adopted slices cannot be sent across threads via `send_take()` because the deleter may not be thread-safe. Use `send()` (which copies) instead.
5. **Move-only:** Adopted slices are unique. Copying is a compile-time error. Ownership transfers via return or explicit `move()`. When destroyed without being moved, the deleter is invoked.

**Examples:**

**Basic adoption:**

```c
unsafe {
    extern char* c_make_string(int len);
    
    // C function returns owned buffer
    char* raw = c_make_string(100);
    
    // Adopt into unique slice (calls c_free when scope exits)
    char[:] s = adopt(raw, 100, c_free);
    
    // Use the slice...
    print_string(s);
    
    // s destroyed here, c_free(raw) called
}
```

**Passing through channels (copying, not transferring):**

```c
unsafe {
    extern uint8_t* read_file(const char* path);
    extern void free_buffer(void* ptr);
    
    uint8_t[:] file_data = adopt(
        read_file("data.bin"), 
        1024, 
        free_buffer
    );
    
    // Copy (safe): data is copied into channel
    @await ch.send(file_data);
    
    // ERROR: adopted slices not transferable
    // @await ch.send_take(file_data);
    
    // file_data destroyed here, free_buffer called
}
```

**Error handling with adopted memory:**

```c
unsafe {
    extern char* parse_result_alloc();
    extern void parse_result_free(void* ptr);
    extern bool parse_is_error(char* p);
    
    char* result_ptr = parse_result_alloc();
    char[:] result = adopt(result_ptr, 256, parse_result_free);
    
    if (parse_is_error(result_ptr)) {
        // result destroyed here, parse_result_free called automatically
        return error("parse failed");
    }
    
    // Use result...
    // Destroyed here, parse_result_free called
}
```

**Zero-length slices:**

```c
unsafe {
    // Adopting NULL with zero count is valid
    char[:] empty = adopt(null, 0, null);
    
    assert(empty.len == 0);
}
```

### A.3 Interoperating with C Code

**Calling C functions:**

```c
extern int c_function(int arg);  // declare C function
int result = c_function(42);     // call it
```

**Passing slices to C:**

```c
extern void c_process(char* ptr, size_t len);

char[:] s = get_string();
c_process(s.ptr, s.len);  // decompose slice into ptr/len
```

**Receiving slices from C:**

```c
extern void c_fill_buffer(char* ptr, size_t len);

CCArena arena = cc_arena_heap(megabytes(1));
CCVec::[char] buf = cc_vec_new::[char](arena);
buf.reserve(1000);
c_fill_buffer(buf.data, buf.cap());  // fill with C code
```

**C struct interop:**

```c
// C code
struct Point { int x; int y; };
typedef struct Point Point;

// CC code
struct Point { int x; int y; }

extern void c_process_point(Point p);
Point p = {.x = 10, .y = 20};
c_process_point(p);
```

### A.4 FFI Safety Checklist

When adopting C code:

- ✅ **Pointer validity:** Ensure all pointers are valid (not dangling, not NULL unless size is 0)
- ✅ **Size accuracy:** Element counts match actual allocation size
- ✅ **Deleter correctness:** Deleter matches allocation mechanism (malloc/free, new/delete, arena, etc.)
- ✅ **Thread safety:** For adopted slices sent across threads, use `send()` not `send_take()`
- ✅ **Lifetime tracking:** Move-only adoption prevents use-after-free
- ✅ **No casting:** Avoid unsafe casts; use `unsafe {}` for explicit provenance breaking

---

## Appendix B: Diagnostics & Error Handling

This appendix specifies compile-time vs runtime error detection and undefined behavior (UB) boundaries.

### B.1 Compile-Time Errors (Required)

The following must be diagnosed at compile time:


| Category                                                      | Examples                                                                                                             | Spec Section |
| ------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- | ------------ |
| Type errors                                                   | Result field access on wrong branch                                                                                  | §3.1   |
| Provenance errors                                             | Slice outlives arena (statically provable)                                                                           | §§3.3–3.5   |
| Sendability errors                                            | Non-sendable capture in spawn closure                                                                                | §2.2, §8.3 |
| Ownership errors                                              | Copy of unique slice, use after move                                                                                 | §2.1, §3.5 |
| Borrow errors                                                 | Borrow outlives owner (statically provable)                                                                          | §4.2        |
| Async errors                                                  | Missing `@await`, invalid suspension point                                                                            | §8.2        |
| Blocking context errors                                       | invalid `@blocking` edge or blocking-task re-entry                                                                   | §8.8        |
| @latency_sensitive violations                                 | Blocking call in @latency_sensitive function                                                                         | §8.8.3.1    |
| Comptime errors                                               | Non-constant in `@comptime` context                                                                                  | §14         |
| Syntax errors                                                 | Invalid Concurrent-C syntax                                                                                          | §11         |
| Result unwrap                                                 | `.value`/`.error` on wrong branch                                                                                    | §3.1        |
| Use after move                                                | Accessing move-only value after transfer                                                                             | §2.1        |
| Unsafe adoption                                               | `adopt()` outside `unsafe {}`                                                                                        | §10.2, Appendix A.2 |
| Result unwrap — missing default                               | `expr ?>` / `expr ?>(e)` with nothing on RHS                                                                         | §3.1        |
| Result unwrap — '?>' misuse for error handling                | `?>` RHS is a divergent statement, a `{ ... }` block, or the bare `?>;` shorthand; use `!>` for error-handling logic | §3.1        |
| Result unwrap — expression-position '!>' body must diverge    | `expr !>(e) { log(e); };` in a declaration initializer or other expression context whose body falls through          | §3.1        |
| Result unwrap — '!>;' at expression position requires handler | `int x = call() !>;` with no enclosing `@errhandler` in scope                                                        | §3.1        |
| Result unwrap — no matching '@errhandler' for error type      | `call() !>;` / `@err` where in-scope handlers exist but none match Result `E`                                        | §3.1        |
| Result unwrap — bad binder                                    | `expr ?>() RHS`, `expr ?>(123) RHS`, `call() !> () BODY`                                                             | §3.1        |
| Result unwrap — missing body                                  | `call() !> (e) ;`                                                                                                    | §3.1        |
| Result forward — unbound binder                               | `@err(X);` where `X` is not the enclosing `!>` binder                                                                | §3.1        |
| Result forward — dead code                                    | statement following `@err(e);` in the same block                                                                     | §3.1        |
| Handler non-divergent                                         | Forward-reached `@errhandler` body last statement is not one of the approved divergent forms                         | §3.1        |
| Unhandled result call                                         | bare `f();` where `f` returns `T!>(E)`                                                                               | §3.1        |


### B.2 Runtime Errors (Debug Builds)

The following are detected at runtime in debug builds only:


| Category           | Signal                             | Behavior                    |
| ------------------ | ---------------------------------- | --------------------------- |
| Stale slice access | Slice `id` not in allocation table | Trap/abort with diagnostic  |
| Arena overflow     | Allocation exceeds arena capacity  | Trap with allocation size   |
| Channel overflow   | Buffer full (`CC_ERR_WOULD_BLOCK`) | Signaled via `CCIoError`    |
| Deadlock           | All workers idle + parked fibers, no progress (all builds; §8.7.1) | Diagnostic dump + exit 124 |
| Stack slice escape | Stack-backed slice escapes frame   | Trap (stack capture safety) |
| Bounds violation   | Array/slice access out of bounds   | Return None or error        |


**Deadlock diagnostics:** When detected, the runtime prints each internally parked fiber with its park reason and the state of the channel it is parked on (§8.7.1).

**Stack escape detection:** Implemented via runtime metadata on stack-backed slices (debug-only overhead).

### B.3 Undefined Behavior in Release Builds

The following are undefined behavior in release builds (no diagnostic required):


| Category            | Consequences                           | Notes                                               |
| ------------------- | -------------------------------------- | --------------------------------------------------- |
| Stale slice access  | Read garbage, crash, memory corruption | Enabled via unsafe, wrong adopt() deleter           |
| Use after move      | Read garbage, crash, double-free       | Move-only violation, compile-time error when caught |
| Double free         | Memory corruption, crash               | Manually calling deleter twice on adopt() slice     |
| Data races          | Corruption, crashes, non-determinism   | Shared mutable state without synchronization        |
| Inactive union read | Read garbage from wrong arm            | Result with wrong branch                            |
| Stack slice escape  | Use invalidated stack memory           | Stack-backed slice used after frame exit            |
| Bounds violation    | Read/write out of bounds               | Bypassing bounds checks in unsafe                   |
| Overflow            | Integer wrapping (as per C spec)       | Unchecked arithmetic                                |


**Rule:** Debug builds should trap on all detectable UB (see §H.2). Release builds may assume UB does not occur and optimize accordingly.

---

## Appendix C: Implementation Notes

This appendix provides guidance for implementers of the CC-to-C translator. All guidance is non-normative; implementations may use different strategies as long as semantic guarantees are met.

### C.1 Coalescing Contiguous `@blocking` Edges

**Context:** Edge-mode resolution and the one-state-per-edge C lowering
of `@blocking` / `@nonblocking` / `async→async` are specified normatively
in **§8.2** and **Appendix J.1.1**. This appendix describes an
optional implementation optimization: coalescing adjacent `@blocking`
edges into a single thread-pool dispatch.

**Optimization:** A compiler MAY merge two or more lexically adjacent
`@blocking` call edges into one `cc_run_blocking_task_intptr` task
when **all** of the following hold:

1. The edges are not separated by an `@await`, a `async→async`
   edge, a `@nonblocking` edge, a suspension-capable statement, a
   label, or a scope boundary.
2. None of the merged calls is marked `@latency_sensitive`
   (see C.2).
3. No merged call observes a side-effect ordering dependency the
   compiler can't preserve locally (aliasing through escaped
   pointers, volatile loads, atomic operations with non-relaxed
   ordering).
4. The merge does not extend a captured-frame field's lifetime
   across a suspension point it would not otherwise cross.

**Observable effect:** Coalescing is a pure performance optimization.
It preserves the happens-before ordering of the individual calls
(they run serially on a single worker) and yields exactly one yield
point for the caller instead of N.

**Example:**

```c
@async void process(int fd) {
    int a = compute1();     // edge 1: @blocking (FFI default)
    int b = compute2();     // edge 2: @blocking
    int c = @await fetch_data();
    int d = compute3();     // edge 3: @blocking
}
```

A conforming implementation MAY lower edges 1 and 2 into a single
closure (one submit, one park, one take) while keeping edge 3 as
its own submit. A conforming implementation MAY also emit three
separate submits — both are correct with respect to §8.2 / J.1.1.

```c
// One legal lowering (coalesced edges 1+2):
@await cc_run_blocking_task_intptr(^{ a = compute1(); b = compute2(); });
c = @await fetch_data();
@await cc_run_blocking_task_intptr(^{ d = compute3(); });
```

**Variable hoisting:** Locals written inside a coalesced closure must
still live on the `@async` frame (§J.1 / J.1.1); coalescing does not
change frame layout beyond collapsing per-edge handle/return-slot
fields where safe.

### C.2 Batching Performance & @latency_sensitive

**@latency_sensitive optimization:** Functions marked `@latency_sensitive` must not be coalesced into larger batches.

**Rule for compiler:** When coalescing batches would cause a `@latency_sensitive` call to be batched with other non-@async calls, do not coalesce. Keep the @latency_sensitive call in its own batch.

**Example:**

```c
@async @latency_sensitive void fast_handler() {
    setup();          // bounded work
    data = process(); // bounded work (latency-sensitive)
    @await send(result);
}
```

**Lowering:** If `setup()` and `process()` would normally batch together, @latency_sensitive prevents it. Each has its own batch (or both are inlined if very fast).

### C.3 Stack Capture Safety and Mutation Checking

**Problem:** Closures capturing locals by reference can outlive the scope if escaped (sent to channel, spawned to thread). Additionally, shared mutable state across tasks causes data races.

**Solution:** Compiler enforces:

1. Escaping closures do not capture stack-local references unsafely
2. Reference captures in spawned closures are checked for mutation

**Rules:**

- Value capture (default) → safe (captures are copies or moves)
- Reference capture `[&x]` in non-escaping closure → allowed
- Reference capture `[&x]` in escaping closure → allowed for read-only access, mutation is error
- Reference capture `[&x]` with mutation requires shipped C synchronization,
  a registered synchronization library, or `@unsafe`.

**Example (escape safety):**

```c
@async void bad() {
    int x = 42;
    CCNursery n = cc_nursery_create() !> @destroy;
    n.spawn(() => { use(x); });  // OK: value capture (copy)
}
```

**Example (mutation checking):**

```c
@async void bad_race() {
    int counter = 0;
    CCNursery n = cc_nursery_create() !> @destroy;
    n.spawn(() => [&counter] { counter++; });  // ERROR: mutation of shared ref
    n.spawn(() => [&counter] { counter++; });
}

@async void ok_atomic() {
    cc_atomic_int counter = 0;
    CCNursery n = cc_nursery_create() !> @destroy;
    n.spawn(() => { cc_atomic_fetch_add(&counter, 1); });
    n.spawn(() => [&counter] { counter++; });
}

@async void ok_readonly() {
    int config = 42;
    CCNursery n = cc_nursery_create() !> @destroy;
    n.spawn(() => [&config] { printf("%d", config); });  // OK: read-only
    n.spawn(() => [&config] { printf("%d", config); });
}
```

**Mutation detection:** The compiler analyzes the closure body for writes to reference-captured variables. Detected patterns include assignment, compound assignment, increment/decrement, and passing address to non-const pointer parameters.

### C.4 Slice Provenance Tracking (Debug Only)

**Implementation:** Maintain an allocation table mapping slice IDs to arena/pool allocations.

**On slice creation:**

1. Assign a unique allocation ID (incremental or hash-based)
2. Store ID in slice.id field
3. Record in allocation table: `{ id, arena*, ptr, alen }`

**On slice access (debug only):**

1. Check if slice.id is in allocation table
2. Verify slice.ptr is within [ arena[id].ptr, arena[id].ptr + arena[id].alen )
3. If not, trap with diagnostic

**Performance:** Debug builds pay the cost of table lookups. Release builds skip validation (assume no UB).

### C.5 Arena Implementation Hints

**Growable chained bump allocator:**

A host owns a chain of slab records, newest first. Each record is created with its bytes and never rewritten: `base` and `capacity` are immutable, and `state` packs the tip and the live count into one word (`live:32 | offset:32`). A bump, a tip pop, a last-live rewind, a tip regrow in place, and a checkpoint carve are each one CAS on that word, so the tip and the count can never disagree and none of them needs the lock. The host embeds its first slab; grow allocates a fresh record and publishes it as `slab` with a release store, leaving the previous record exactly as it was, so a bump that raced the grow still lands on the slab it read and is credited there. One slab holds at most `CC_ARENA_SLAB_MAX` (4 GiB) bytes.

```c
struct CCArenaSlab {
    uint8_t*  base;        // immutable
    size_t    capacity;    // immutable, <= CC_ARENA_SLAB_MAX
    /* atomic */ uint64_t state;   // live:32 | offset:32, every change a CAS
    size_t    tail_carved; // where a live tail child's region begins (== capacity when none)
    CCArenaSlab* prev;     // the slab this one grew from (NULL for L1)
    uint32_t  flags;       // HEAP_OWNED: cc_free(base) at teardown
    uint16_t  block_idx;   // 0 = L1
};

struct CCArenaHost {
    CCArenaSlab* slab;     // current slab; grow publishes a new record (release), readers acquire
    CCArenaSlab  l1;       // the first slab's record, in the host
    uint64_t provenance;   // monotonic arena id / epoch
    uint32_t _flags;       // ALLOW/USED_HEAP_OVERFLOW, HOST_INLINE, TAIL_CHILD, REUSE, ...
    uint16_t block_max;    // budget: 0 = unbounded, 1 = fixed, N = max
    /* ovf_head / ovf_chunks / overflow_bytes / meta_lock / children / active / owner_free / reuse_free — see cc_arena.cch */
};
```

The meta lock serializes grow, the overflow lists, the owner and class freelists, active-child swaps, and lifetime-parent records; it never covers the slab word.

**Growth (slow path):**

```
On a bump that does not fit the current slab:
  1. Take the lock; retry the bump (another thread may have grown).
  2. If block_max != 1 and block_idx + 1 < block_max (or block_max == 0):
       allocate a slab record + buffer (max(1.5× cap, need, 4096), capped at
       CC_ARENA_SLAB_MAX), chain it to the current record, publish it as the
       current slab, retry the bump
  3. Else if heap overflow is enabled: allocate via overflow path
  4. Else return NULL
```

**Reset:** Drain overflow, free every grown slab (buffer and record), make the embedded L1 record current again with `state = 0`, advance provenance.

**Checkpoint/Restore:** A checkpoint is an active child arena. Capture carves a child host on the innermost active host's L1 tail (or heap-roots one when the tail is too small; an uncappable host with no tail returns an unarmed handle), attaches it as a lifetime child, parks the parent tip at the slab end, and sets `active`. Fresh allocations through the parent forward to the innermost active child; realloc and release act on the host that owns the pointer. Restore verifies the child against the parent's record list, refuses while an armed inner checkpoint is active, then frees the child (extents, overflow, epoch) and pops the parent tip back to the child's start offset. Abandon consumes the handle and leaves the child active. Attach records, holes, and overflow never refuse a capture or a restore; reset and free tear down an active child like any other.

**Per-request pattern:**

```c
while (true) {
    Request req = accept_connection();
    CCArena req_arena = cc_arena_heap(megabytes(1));
    
    handle_request(&req, &req_arena);
    
    cc_arena_reset(&req_arena);  // drain ovf; unwind extents; restore root
}
```

### C.6 Channel Implementation Hints

**Wait-free queues for async channels:**

Use a concurrent queue (MPMC or SPMC) with intrusive linked lists or ring buffers. No allocation during send/recv.

**Blocking channels:**

Use a condition variable + mutex for synchronization. Simpler than async channels but less performant.

### C.7 Generating Clean C Code

**Goal:** Emitted C should be readable and inspectable.

**Rules:**

1. Preserve variable names and structure
2. Expand macros into inline operations
3. Use meaningful generated identifiers (e.g., `__batch_1`, `__task_42`)
4. Add comments explaining non-obvious lowerings
5. Provide `--emit-c-only` / `--emit-c-inspect` flags for debugging

**Example:**

```c
// CC code:
@async void process() {
    int a = compute();
    int b = @await fetch();
}

// Generated C:
Task__process__0 process__init() {
    ProcessState* state = malloc(sizeof(ProcessState));
    state->pc = 0;
    return (Task__process__0) { .opaque = state };
}

// ... state machine lowering with clear labels
```

### C.9 Debugging Environment Variables

The CC compiler supports environment variables for debugging compilation issues:


| Variable                  | Purpose                                          |
| ------------------------- | ------------------------------------------------ |
| `CC_DUMP_LOWERED=<path>`  | Dump the lowered source written to TCC to `<path>` |
| `CC_DEBUG_STUB_NODES=1`   | Dump stub AST nodes (arenas, nurseries, etc.)    |
| `CC_KEEP_PP=1`            | Keep temporary preprocessed files for inspection |


**Usage example:**

```bash
# See exactly what TCC receives (useful for "lvalue expected" errors)
CC_DUMP_LOWERED=out/lowered.c ccc build myfile.ccs && less out/lowered.c

# Inspect arena/nursery AST node spans
CC_DEBUG_STUB_NODES=1 ccc build myfile.ccs

# Keep preprocessed temp files
CC_KEEP_PP=1 ccc build myfile.ccs
```

**Common debugging scenarios:**

1. **"lvalue expected" errors:** Use `CC_DUMP_LOWERED=<path>` to inspect the lowered source. Look for garbled type declarations (e.g., `Tcc_unwrap(x)` instead of `T* x`).
2. **Arena/nursery issues:** Use `CC_DEBUG_STUB_NODES=1` to verify AST node spans match your source.
3. **Result type redefinition errors:** Ensure your `.cch` headers use `#ifndef CCResult_T_E_DEFINED` guards around `CC_DECL_RESULT_SPEC` calls.

---

## Appendix D: ABI Commitments

This appendix documents stable layout and calling conventions for binary compatibility and debugging.

### D.1 Type Layouts

**Result (`T!>(E)`) layout:**

```c
struct CCResult_T_E {
    _Bool ok;         // C11 _Bool (1 byte)
    // padding to max(alignof(T), alignof(E))
    union { T value; E error; } u;
};
// sizeof(CCResult_T_E) = max_align + max(sizeof(T), sizeof(E))
```

**Example (`CCResult_int_ParseError`):**

If ParseError is 8 bytes:

```c
struct CCResult_int_ParseError {
    _Bool ok;              // 1 byte
    // 7 bytes padding
    union {
        int value;         // 4 bytes (in 8-byte union)
        ParseError error;  // 8 bytes
    } u;                   // 8 bytes
};
// sizeof = 16 bytes
```

**Slice (`T[:]`) layout (64-bit platforms):**

```c
struct Slice_T {
    T*       ptr;      // 8 bytes (pointer)
    size_t   len;      // 8 bytes (length)
    uint64_t id;       // 8 bytes (allocation ID + flags)
};
// sizeof(Slice_T) = 24 bytes on 64-bit platforms
```

**ID field encoding (bit layout):**

```
Bit 63: is_unique         (1 = has destructor, move-only)
Bit 62: is_subslice       (1 = doesn't cover entire allocation)
Bit 61: is_transferable   (1 = safe to send_take across threads)
Bits 0–60: allocation_id  (0 = static/untracked)
```

**32-bit platforms:** Slice layout is implementation-defined. Implementations may use smaller pointer/size fields (4 bytes each) resulting in 12–16 byte slices, or preserve 64-bit fields for compatibility. Portable code should not assume slice size.

**Duration layout:**

```c
struct Duration {
    int64_t secs;     // 8 bytes (relative seconds)
    int32_t nanos;    // 4 bytes (nanoseconds, 0–999999999)
    // 4 bytes padding
};
// sizeof(Duration) = 16 bytes
```

### D.2 Calling Conventions

`**@async` function lowering:**

`@async` functions lower according to Appendix J.1: a constructor returns
`CCTaskIntptr`, which owns a heap frame, poll callback, and drop callback. The
frame contains state, lifted parameters and locals, result storage, and child
task slots. Progress occurs when the task is polled; the poll callback returns
`CC_FUTURE_PENDING`, `CC_FUTURE_READY`, or `CC_FUTURE_ERR`.

**Channel operations:**

Channel operations are lowered from typed-handle methods and constructor syntax:

```c
CCChan* cc_channel_pair(CCChanTx* tx, CCChanRx* rx);
CCResult_bool_CCIoError cc_channel_send(CCChanTx tx, T value);
CCResult_bool_CCIoError cc_channel_recv(CCChanRx rx, T* out);
void cc_channel_close(CCChanTx tx);
```

Surface `tx.send(value)`, `rx.recv(&out)`, `tx.close()`, and `cc_channel_pair(&tx, &rx)` lower to these channel-family contracts.

### D.3 Alignment Requirements

**Standard C alignment:** All types follow C alignment rules:

- Primitives: `alignof(int) = 4` (typically), `alignof(long) = 8`, etc.
- Pointers: `alignof(T*) = 8` on 64-bit
- `_Bool`: `alignof(_Bool) = 1`
- Slices: `alignof(T[:]) = alignof(T*)` (pointer alignment)

**Arena allocation alignment:**

```c
void* cc_arena_alloc(CCArena a, size_t nbytes, size_t align);
```

`align` is a power of two (`>= 1`). Typed helpers use `_Alignof(T)`.

### D.4 Binary Compatibility

**Stability guarantee:** A released ABI does not:

- Change field offsets in existing structs
- Change enum variant values
- Remove or rename functions
- Change calling conventions

**Debugging support:** The ABI is designed to be inspectable via standard C debuggers. Slice metadata (id field) enables provenance-aware debugging in custom tools.

---

### `@nonblocking` Contract

A function marked `@nonblocking` (compatibility spelling `@noblock`) asserts it will never block:

**Rules:**

- No I/O, no synchronization waits, no channel operations
- Only CPU work (arithmetic, string ops, local structures)
- Compiler does not wrap in blocking executor
- Violations: Runtime trap (debug), UB (release)

**Example:**

```c
@nonblocking int parse_count(char[:] s) {
    // Safe: only CPU work
    return (int)atoi(s.ptr);  // via FFI
}

@async void db_query(int count);  // Must @await

@async @latency_sensitive void handler(Request req) {
    int count = parse_count(req.body);  // OK (@nonblocking)
    @await db_query(count);              // OK (awaited)
}
```

### @latency_sensitive Linting Rule

A function marked `@latency_sensitive` asserts it must not experience unexpected latency from coalescing:

**Rules:**

- Compiler must not coalesce stalling calls within function
- Only `@nonblocking` and awaited `@async` calls allowed
- Each stalling operation dispatches separately
- Latency is predictable and observable

**Violations:**

```c
void process_logs(int count);  // Unknown: might block

@async @latency_sensitive void handler(Request req) {
    int count = parse(req.body);  // ✅ OK (CPU)
    @await db_get(count);          // ✅ OK (awaited)
    process_logs(count);          // ❌ ERROR: might block
}
```

**Compiler output:**

```
error: non-@nonblocking, non-@async call in @latency_sensitive function
  → process_logs(count);

  Fix: Mark process_logs @nonblocking, or make it @async and @await it
```

---

## Appendix E: Standard Error Types & Backpressure

### Error Types

Shipped faces are structs, not payload enums. `CCIoError` is a `CCError` face plus `os_code`:

```c
typedef struct {
    CCError base;
    int32_t os_code;
} CCIoError;

@typeview on CCIoError { as: base; };
```

Kind aliases (`CC_IO_FILE_NOT_FOUND`, `CC_IO_BUSY` / `CC_ERR_WOULD_BLOCK`, …) live on `CCErrorKind`. Parse and bounds errors are ordinary C enums or library structs with no payload constructors. Constructors, `cc_io_from_errno`, `cc_io_avail`, and the `CC_IO_*` table are in `spec/concurrent-c-stdlib-spec.md` (I/O errors).

### Backpressure Modes

See §8.4.0. Three live modes:

| Mode | Spelling | Queue full | Sender |
| ---- | -------- | ---------- | ------ |
| **Block** (default) | omitted | waits | blocks/suspends |
| **DropNew** | `Drop`, `DropNew` | unchanged | `err` / `EAGAIN` |
| **DropOld** | `DropOld` | discards head | succeeds |

Handles: `T[~N >, Drop]`, `T[~N <, DropOld]`. There is no `Sample` mode and no `, Block` token.

---

## Appendix F: Server Programming Patterns

### Pattern 1: Request Handler

```c
@async @latency_sensitive Response!>(IoError) my_handler(Request* req, CCArena a) {
    // CPU work: inline (compiler inlines aggressively)
    Parsed p = parse(req.body);
    
    // Stalling I/O: separate dispatch (visible latency)
    DbResult res = @await db_get(p.id, a) !>(e) return cc_err(e);
    
    // CPU work: encode (inline)
    Response resp = {
        .status = 200,
        .body = encode_json(res, a)
    };
    
    return resp;
}
```

**Properties:**

- `@latency_sensitive` prevents surprise coalescing
- Compiler enforces only `@nonblocking` and awaited `@async` calls
- Latency is observable (I/O dispatch is separate from CPU)

### Pattern 2: Backpressure Strategy

```c
// Two pipelines, two strategies

LogEvent[~10000 >, DropOld] access_logs;  // High-volume, lossy (drop oldest)
LogEvent[~1000 >] audit_logs;             // Block (default), critical

// In handler:
access_tx.send(access_event);            // DropOld: never blocks
audit_tx.send(audit) !>(e) return cc_err(e);
```

**Properties:**

- Access logs drop oldest (never blocks)
- Audit logs block (critical)

### Pattern 3: Complete Server

```c
@async void main() {
    ServerConfig cfg = {
        .port = 8080,
        .max_workers = 32,
        .max_connections = 1000,
        .request_timeout = seconds(5),
        .handler = my_handler,
    };
    
    @await server_loop(cfg);
}
```

**server_loop handles:**

- Accept connections
- Create per-request arena
- Apply deadline (via `@with_deadline`)
- Spawn N workers (nursery)
- Graceful shutdown

**User provides:** Handler + config (~30 lines total)

---

### Pattern 4: Bidi Stream “First-Close Wins”

For bidi protocols, the default rule SHOULD be:

- Reader and writer report close/error through an explicit shared shutdown
  channel or explicit `n.cancel()`
- Teardown happens exactly once, in one place, and each blocking operation is
  bounded by an API that accepts the current deadline

This avoids “both sides race to close” bugs and makes shutdown reviewable.

---

### Pattern 6: Deadline Layering for Long-Lived Connections

**Strong recommendation:** Avoid wrapping an entire long-lived serve loop in one large `@with_deadline(...)` unless you are intentionally enforcing an end-to-end SLA.

Prefer layered deadlines:

- **Handshake deadlines:** short `@with_deadline` around negotiation (TLS/WS upgrade/initial headers)
- **Idle/heartbeat deadlines:** renewed on activity (timer task + cancellation, or per-iteration short deadline)
- **Teardown deadlines:** short, bounded shutdown/drain through
  deadline-aware operations

This keeps deadlines precise and prevents “everything is always under a deadline” from becoming the default mental model.

---

## Appendix G: Terminology Summary

### Keywords & Annotations


| Keyword              | Meaning                                    | Usage                       |
| -------------------- | ------------------------------------------ | --------------------------- |
| `@async`             | Function may suspend                       | Mark async functions        |
| `@nonblocking`       | Never blocks/allocates                     | Mark pure utilities         |
| `@noblock`           | Compatibility spelling for `@nonblocking`  | Same as `@nonblocking`      |
| `@latency_sensitive` | No coalescing allowed                      | Mark request handlers       |
| `@scoped`            | Cannot escape scope                        | Mark safe cross-thread refs |
| `spawn`              | UFCS on `CCNursery`                        | `n.spawn(() => { … })`     |
| `@defer`             | Defer cleanup                              | Guarantee execution         |
| `@await`              | Suspend on async                           | Call @async functions       |
| `CCNursery`          | Structured concurrency                     | Scope with tasks            |
| `@with_deadline`      | Apply timeout                              | Enforce deadline            |


### Type Sugar


| Sugar                                         | Full                                                  | Meaning                         |
| --------------------------------------------- | ----------------------------------------------------- | ------------------------------- |
| `T!>(E)`                                      | `Result::[T, E]`                                        | Either T or error E             |
| `T[:]`                                        | Slice of T                                            | Pointer + length                |
| `T[~... >]` / `T[~... <]`                     | `AsyncChanTx::[T]` / `AsyncChanRx::[T]`                   | Async channel handles           |
| `T[~N ... >]` / `T[~N ... <]`                 | `AsyncChanTx::[T, N]` / `AsyncChanRx::[T, N]`             | Async handles, capacity N       |
| `T[~N >, Drop]` / `T[~N <, DropOld]`          | same handles with backpressure (§8.4.0)               | DropNew / DropOld after direction |
| `T[~ ... sync ... >]` / `T[~ ... sync ... <]` | `SyncChanTx::[T]` / `SyncChanRx::[T]`                     | Sync channel handles            |


---

## Appendix H: Complete Example: HTTP Server

```c
#include <ccc/std/prelude.cch>
#include <ccc/std/server.cch>
#include <ccc/std/log.cch>

// Handler: Mark @latency_sensitive to ensure predictable latency
@async @latency_sensitive Response!>(IoError) api_handler(Request* req, CCArena a) {
    // CPU work: parse (inlined, no latency)
    UserId user_id = parse_user_id(req.path) !>(e) return cc_err(e);
    
    // Stalling I/O: fetch from database (separate dispatch, observable)
    User user = @await db_get_user(user_id, a) !>(e) return cc_err(e);
    
    // CPU work: encode (inlined)
    char[:] json = encode_user_json(&user, a) !>(e) return cc_err(e);
    
    // Return response
    return Response {
        .status = 200,
        .headers = "Content-Type: application/json\r\n",
        .body = json
    };
}

// Main: Configure and run server
@async void main() {
    ServerConfig cfg = {
        .port = 8080,
        .max_workers = 32,
        .max_connections = 1000,
        .request_timeout = seconds(5),
        .handler = api_handler,
    };
    
    @await server_loop(cfg);
}
```

**Properties:**

- ✅ Handler marked `@latency_sensitive` (compiler prevents blocking)
- ✅ Deadline per request (timeout enforced via `server_loop`)
- ✅ Per-request arena (automatic reset, no leaks)
- ✅ ~20 lines of user code for complete server
- ✅ Safe, fast, observable

---

## Appendix J: C Lowering Strategy

This appendix specifies how Concurrent-C constructs lower to portable C, ensuring stable ABI and readable generated code. Implementation must follow these rules to prevent ABI divergence and resource bugs.

### J.1 `@async` Frame and Poll Contract

Each `@async` definition lowers to:

1. a concrete heap-allocated frame containing `int __st`, `intptr_t __r`,
   typed parameter slots named `__p_<name>`, typed lifted local slots, lifted
   await-result slots, and a fixed `CCTaskIntptr __t[N]` child-task array;
2. a poll function with ABI
   `CCFutureStatus poll(void* frame, intptr_t* out, int* err)`;
3. a drop function `void drop(void* frame)`; and
4. a public constructor returning `CCTaskIntptr`.

The constructor allocates the frame with `calloc`, copies parameters into the
typed frame slots, initializes state zero, and returns
`cc_task_intptr_make_poll_ex(poll, NULL, frame, drop)`. Allocation failure
returns a zero-initialized task.

The poll function casts its frame, executes a `for (;;)` around
`switch (frame->__st)`, and performs on-CPU transitions with `continue`.
State `999` stores `frame->__r` in `out` and returns `CC_FUTURE_READY`; an
invalid state or null frame returns `CC_FUTURE_ERR`.

At `@await`, the frame owns a child `CCTaskIntptr`. Polling calls
`cc_task_intptr_poll`. It returns `CC_FUTURE_PENDING` only when that child is
pending. On ready or error, the lowering consumes the child result, frees that
child task slot, updates state, and continues.

The task owns its frame through the registered drop callback. Drop frees every
child slot with `cc_task_intptr_free` and then frees the frame. A completed task
retains its result until the task owner consumes or frees the task. Lifted
locals have their C types preserved, including arrays and result-family types.
These shapes are pinned by emitted-C lowering tests.

---

### J.1.1 Call-Edge Lowering (`@blocking` / `@nonblocking`)

Call-edge classification follows §8.2. A direct `@nonblocking` edge remains a
direct C call in the current state and adds no child task. An async child call
constructs a `CCTaskIntptr` in the frame and uses the J.1 child-poll contract.

A blocking edge is rewritten to the shipped `cc_run_blocking_task_intptr(...)`
task-family call, stored in a child slot, and polled through the same
`cc_task_intptr_poll` path as any other await. Return storage is lifted onto the
frame when it remains live. This common `CCTaskIntptr` poll/drop path is the
complete blocking-edge task ABI.

Sync functions do not receive frame/poll lowering. Call-site mode markers are
removed from emitted C after the selected direct or task-wrapped call is
formed. Lowering-shape tests pin both the count of
`cc_run_blocking_task_intptr` wrappers and the absence of mode markers.

---

### J.2 Cancellation and Deadline Lowering

The lowering does not inject a universal cancellation branch at every
`@await`. A deadline scope pushes its `CCDeadline*` with
`cc_deadline_push` and restores the previous pointer with `cc_deadline_pop`.
An operation that supports ambient deadlines obtains it with
`cc_current_deadline()` and passes its absolute time to the scheduler boundary.
Nursery-aware waits consult the current nursery separately. Each operation
maps cancellation or expiry to its own documented result as required by
§4.2.2 and §8.5.

---

### J.4 `@parallel` Lowering

An assignment `@parallel` block lowers to `cc_parallel_spawn` of a file-scope thunk per arm after the first, the first arm on the caller, then `cc_parallel_join` in reverse spawn order. An assignment thunk writes `*out = <arm rhs>` through a stack environment that lives until join returns. A `@serial` thunk with an outer name copies the destination in, runs the body, and writes it back through `*out`. A statement `@serial` runs the body with no destination. If spawn returns `CC_TASK_KIND_INVALID` (adaptive deny or spawn failure), the thunk runs on the caller. `@parallel spawn`, dest-live, and dest-attach do not: a failed admit is `cc_parallel_die`. `#pragma(@parallel) off` is the sequential dest-live lowering.

`@parallel (pred) { … }` lowers to `if (!(pred)) {` the same arms in order `} else` a file-scope spawn helper. A false predicate does not call spawn. The helper holds `CCTask` and env so the sequential path stays a small function.

`@parallel for (i in lo..hi)` lowers to a file-scope walk that bisects `[lo, hi)`: spawn one half, walk the other, join. A span of length ≤ 1, or a failed spawn, is a C `for` over that span. The walk environment is stack-allocated at the call site and copied for the spawned half.

Neither form introduces a nursery.

---

### J.3 Slice and Buffer Ownership in Async Frames

**Rule:** Locals that contain slices, adopted buffers, or move-only values live on the frame and follow move semantics.

**Storage:**

```c
@async void handler(char[:] request_body, CCArena a) {
    char[:] trimmed = request_body.trim();  // View: points into request_body
    char[:] owned = request_body.clone(a);  // Copy: heap-allocated in arena
    
    // Frame layout:
    struct Frame {
        char[:] request_body;      // Move-only value; stored in frame
        CCArena a;                 // Handle; stored in frame by value
        char[:] trimmed;           // View; stored in frame; points to request_body
        char[:] owned;             // Move-only; stored in frame; heap-allocated
    };
}
```

**Ownership semantics:**

- **Moved values:** If a move-only value is moved, the frame field becomes invalidated (compiler marks it with `__moved_flag` or similar). Subsequent use is a compile error (move checker detects in type system).
- **Views (non-owning slices):** Views remain valid as long as the data they reference is valid. Compiler uses borrow checker to ensure views don't outlive their source.
- **Arena-allocated buffers:** Remain valid as long as the arena is not reset.
  The compiler cannot prove every reset point, so undetected violations remain
  a logic error.
- **Adopted buffers:** Drop glue calls the deleter when frame is destroyed or field is overwritten.

**Drop:** The emitted drop callback frees every owned child task slot and the
frame. Source-level declaration cleanup already lowered into the state machine
runs according to its `@defer` / `@destroy` control-flow placement. There is no
`@drop` annotation.

---

### J.4 Readable C Mapping

**Principle:** Generated C should be "readable conventional C", not compiler magic. A developer should be able to understand the lowered C without specialized knowledge.

**Guidelines:**

1. **Readable state machines:** Use named structs and clear switch statements:
  ```c
   switch (frame->__state) {
       case 0: goto state_start;
       case 1: goto state_after_first_await;
       case 2: goto state_after_second_await;
   }
  ```
   Not: abstract bytecode or nested function pointers.
2. **Clear local storage:** Frame structs list all locals with original names:
  ```c
   struct Frame {
       int __state;
       UserId user_id;
       User user;
       char[:] json_response;
   };
  ```
   Not: void* arrays or opaque tags.
3. **Labeled drops and cleanups:** Explicit drop glue:
  ```c
   on_frame_drop: {
       if (frame->fd != INVALID_FD) close(frame->fd);
       cc_arena_reset(frame->arena);
   }
  ```
4. **Explicit cancellation checks:** Visible `if (token->cancelled)` in source.
5. **Lowered-C inspection:** For debugging and spec validation:
  ```bash
   ccc --emit-c-only -o output.c input.ccs
  ```
   Generates the lowered C with:
  - Comments mapping each line back to source location
  - Marked suspension points
  - Drop points annotated
  - Helpful for understanding ABI, validating correctness, and teaching

**Benefits:**

- Developers can read generated C if needed
- Easier to debug (gdb can step through lowered C)
- Spec tests can validate both semantic and structural properties
- Demystifies the language ("it's just C underneath")

---

### J.5 Task and Scheduler Integration

`CCTaskIntptr` owns the frame/drop pair created by an `@async` constructor and
the poll callback that advances it.

```c
typedef struct {
    /* runtime-owned poll state */
} CCTaskIntptr;

CCFutureStatus cc_task_intptr_poll(CCTaskIntptr* task,
                                   intptr_t* out, int* err);
void cc_task_intptr_free(CCTaskIntptr* task);
```

Polling returns `CC_FUTURE_PENDING`, `CC_FUTURE_READY`, or `CC_FUTURE_ERR`.
The caller must not concurrently poll the same task. `cc_task_intptr_free`
invokes the registered drop callback exactly once for an owned frame and clears
the task. Readiness does not itself transfer frame ownership away from the task.

---

## Lowering commitments


| Component           | Lowering                                         | Contract         |
| ------------------- | ------------------------------------------------ | ---------------- |
| @async functions    | Stackless state machines (switch + goto)         | Normative        |
| Cancellation/deadline observation | Operation-specific runtime checks | Normative |
| Slice ownership     | Frame-local; borrow checker enforces             | Normative        |
| Readable C          | Named structs, labeled states, explicit cleanups | Design principle |
| --emit-c-only       | Lowered-C output with source mapping             | Shipped          |


These rules prevent ABI surprises and ensure the implementation can generate boring, understandable C code.

---

## Summary

The main body defines source syntax, static semantics, and observable behavior.
§9.5 defines `.shcc` entry rewriting and the `ccc/script/` orchestration
surface. Appendix J defines the shipped C lowering contract. Focused
specifications define scheduler and channel runtime state machines.
