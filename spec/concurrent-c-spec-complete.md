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

1. **No magic.** New behavior must be as predictable as C. A program is understood from what is on screen plus its lowering — never from hidden runtime policy.
2. **The lowering is the language.** The source-to-C lowering is part of this specification, not an implementation detail (see intro). Two conforming implementations produce lowerings with identical observable behavior.
3. **Library-owned policy.** The compiler provides compile-time registration,
   emission, and dispatch seams. Libraries own concrete type-family emission,
   linkage, erasure, specialization, and UFCS policy.
4. **No silent drop.** Every fallible result must be used. The safe forms (`?>`, `!>`) force handling at compile time; the explicit/lowered form traps loudly at runtime. Absence (nullable) may opt into the same unwrap protocol but is not forced into it.
5. **Local resolution.** Cleanup (`@destroy`/`@defer`), error handlers (inline or function-level only), and dispatch (receiver type) resolve to a site visible at the use — never a dynamic search. Behaviors therefore compose by stacking, not by interaction.
6. **Binding is semantic.** *Where* a construct attaches (declaration vs. statement) determines its static guarantees, even when the runtime lowering is identical. `@destroy` is RAII because it binds to the declaration; `@defer` is a statement.

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
CCNursery* n = cc_nursery_create(NULL) !> @destroy;
n->spawn(() => printf("hi\n"));   // lowers to cc_nursery_spawn(n, ...)
```

UFCS is orthogonal to ownership: the same method works on a value, a
pointer, or a Result carrying either. Normative rules in §9.0.

### 1.2 Result types and unwrapping

A function that can fail returns `T!>(E)`. Three unwrap forms cover the
full error space — each answers a different question at the call site:

| Form                | Question                         | Body                |
| ------------------- | -------------------------------- | ------------------- |
| `expr ?> default`   | "what if it fails?"              | value               |
| `expr !>(e) { … }`  | "handle it here"                 | diverging statement |
| `expr !>;`          | "forward to the scope's policy"  | —                   |

```c
int !>(CCError) read_timeout(void);

@errhandler(CCError e) { fprintf(stderr, "%s\n", e.message); return 1; }

int t1 = read_timeout() ?> 30;                   // default on error
int t2 = read_timeout() !>(e) return 2;          // inline (must diverge)
int t3 = read_timeout() !>;                      // forward to @errhandler
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
(`@async`, `@await`, `@defer`, `@cancel`, `@errhandler`,
`@destroy`, `@with_deadline`, `@comptime`, `@blocking`,
`@nonblocking`, `@for`). Bare forms are
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
CCString greeting = @string(`hello ${now()}`, &a);   // arena provenance
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
| `@cancel`      | Cancel a named `@defer` before it runs                                  | `@cancel cleanup;`                     |
| `@errhandler`  | Block-scoped default handler for `!>;`                                  | `@errhandler(CCError e) { log(e); }`   |
| `@err`         | Forward current error to the enclosing handler                          | `@err(e);`                             |
| `@with_deadline` | Apply deadline to a block                                             | `@with_deadline(seconds(5)) { … }`     |
| `@destroy`     | Attach cleanup to a result-unwrap                                       | `FILE* f = open() !> @destroy;`         |
| `@comptime`    | Compile-time evaluation / conditional                                   | `@comptime if (DEBUG) { }`             |
| `@blocking`    | Mark a call edge as going through `run_blocking` (function or site)     | `@blocking f();` — see §8.2            |
| `@nonblocking` | Mark a non-blocking execution-mode contract (function, block, or site) | `@nonblocking f();` — see §8.2      |
| `@noblock`     | Compatibility spelling for `@nonblocking`                              | `@noblock f();` — see §8.2          |
| `@latency_sensitive` | Disable dispatch coalescing for this `@async` fn                  | `@async @latency_sensitive void h() {}`|
| `@scoped`      | Type tied to a lexical scope (cannot escape)                            | `@scoped type Guard::[T];`             |
| `@slice`       | Build-time canonical sentinel slice                                     | `char[:0] m = @slice("recv");`         |
| `@string`      | Templated string: arena `String`, or arena-less bounded `char[:]` (§9.1.2) | `CCString s = @string("hi", &arena);`  |
| `unsafe`       | (Bare) disable safety checks in a block                                 | `unsafe { ptr_cast(); }`               |


### Declaration and Statement Forms


| Form                            | Purpose                                                  | Example                                                  |
| ------------------------------- | -------------------------------------------------------- | -------------------------------------------------------- |
| `@async fn() { }`               | Define asynchronous function                             | `@async void handler() { }`                              |
| `@blocking fn() { }`            | Mark declaration — async callers route through `run_blocking` at call edges (§8.2) | `@blocking FILE* open_config() { … }`                    |
| `@nonblocking fn() { }`         | Mark declaration — async callers skip `run_blocking` at call edges (§8.2)          | `@nonblocking size_t strlen_nb(const char* s) { … }`     |
| `@latency_sensitive`            | Mark as latency-critical (no dispatch coalescing)        | `@async @latency_sensitive void handle() { }`            |
| `@scoped type T`                | Type tied to lexical scope (cannot escape)               | `@scoped type Guard::[T];`                               |
| `CALL() !> @destroy { D };`     | Resource lifetime declaration with error-checked cleanup | `CCNursery* n = cc_nursery_create(NULL) !> @destroy;`    |
| `@defer stmt;`                  | Schedule statement to run on scope exit                  | `@defer file.close();`                                   |
| `@comptime if (cond) { }`       | Compile-time conditional                                 | `@comptime if (FEATURE_X) { }`                           |
| `@errhandler(E e) { }`          | Block-scoped default handler for `!>;` / `@err`          | `@errhandler(CCIoError e) { log(e); return cc_err(e); }` |

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
| `call !>;`                | statement / expression | Unwrap, or invoke the enclosing `@errhandler`                                     | `flush() !>;`                                  |


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
| `@string(\`...\`)` (no arena)                            | Bounded-template stack form: block-scoped buffer, yields `char[:]` borrow (§9.1.2) | `char[:] s = @string(\`v=${v}\`);`        |


### Deadline Scope Forms


| Form                              | Purpose                                                  | Example                                                         |
| --------------------------------- | -------------------------------------------------------- | --------------------------------------------------------------- |
| `@with_deadline(ms) { }`           | Make a relative deadline current for operations that consult it. | `@with_deadline(seconds(5)) { cc_chan_match_select(..., cc_current_deadline()); }` |
| `@with_deadline(ms) as handle { }` | Same, with the active `CCDeadline*` bound inside the block. | `@with_deadline(seconds(5)) as dl { if (cc_deadline_expired(dl)) break; }` |


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
| Generic type          | `CCVec::[int]`           | `CCVec :: [ int ]`                                          |
| Nested generic        | `Map::[K, CCVec::[int]]` | `Map :: [ K, CCVec :: [ int ] ]` (standard bracket nesting) |


**Examples:**

**Nested slice note:** `T[::]`, `T[:::]`, and deeper forms mean nested slice ranks (`T[:]`, `T[:][:]`, `T[:][:][:]`, etc.). These are slice/view types, not C arrays. Refinements compose on the innermost rank (`T[:0:]`, `T[4::]`, etc.) and still lower to the same slice ABI family.

```c
// Correct
int!>(IoError) read_int(char[:] data) {
    char[:] trimmed = data.trim();
    return cc_ok(parse_int(trimmed));
}

CCVec::[int] numbers = cc_vec_new::[int](&arena);
Map::[char[:], int] registry = map_new::[char[:], int](&arena);

// Incorrect (visual noise, harder to parse)
int ! IoError read_int (char [ : ] data) {
    // ...
}
```

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

**Rule (use-after-move):** Compile-time error for:

- Bare unique slice after move
- Borrowed views from moved owner

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
n->spawn(() => {
    use(stack_slice);  // BAD: stack_slice.ptr points to caller's stack frame
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
| `ThreadGroup`            | **No**                         | Must be used in creating thread             |
| `Scope`                  | **No**                         | Stack-bound capability                      |
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

```c
void ok_pattern() {
    CCArena a = cc_arena_heap(kilobytes(64));
    char[:] s = arena_alloc(char, &a, 100);
    
    ThreadGroup g = thread_group();
    g.spawn(() => {
        use(s);  // OK: g.join() happens before arena freed
    });
    g.join();
}  // arena freed here, after thread joined

void bad_pattern() {
    ThreadGroup g = thread_group();
    {
        CCArena a = cc_arena_heap(kilobytes(64));
        char[:] s = arena_alloc(char, &a, 100);
        g.spawn(() => {
            use(s);  // ERROR: arena may be freed before thread runs
        });
    }  // arena freed here
    g.join();  // thread may access freed memory
}

void bad_reset_while_borrow() {
    CCArena a = cc_arena_heap(kilobytes(64));
    char[:] s = arena_alloc(char, &a, 100);
    use(s);
    cc_arena_reset(&a);  // ERROR: borrow of a still in scope
}

void ok_reset_after_borrow_scope() {
    CCArena a = cc_arena_heap(kilobytes(64));
    {
        char[:] s = arena_alloc(char, &a, 100);
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
`@match` (reserved and rejected), `@defer`, `@defer(err)`, `@defer(ok)`, `@cancel`,
`@errhandler`, `@err`, `@destroy`, `@with_deadline`, `@comptime`,
`@for`, `@latency_sensitive`, `@scoped`, `@slice`, `@string`.
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
| `@task <text>` | One-line summary override for `.shcc` `@` task listing (§9.5.2a). When present, compact task UIs prefer this text over the leading summary. |

```c
/**
 * Read metrics from path into an arena-backed map.
 *
 * @param path  NUL-terminated filesystem path
 * @param a     arena for keys and map storage
 * @returns     populated Metrics map
 * @errors      CC_ERR_IO, CC_ERR_PARSE
 */
static Metrics !>(CCError) load_metrics(const char* path, CCArena* a);
```

CCDoc does not affect program semantics or lowering. Emission of HTML indexes,
hover cards, or markdown is a tooling concern that consumes the same blocks.

---

## 3. Core Types

This section defines the fundamental value-level building blocks:

- **§3.1 Results (`T!>(E)`)** — success or failure; statement-level `@err` / `@errhandler`
- **§3.2 Type Precedence** — how type modifiers bind
- **§3.3 Arrays and Slices** — fixed arrays and views
- **§3.4 Slice ABI** — provenance metadata layout

---

### 3.1 Results (`T!>(E)`)

`T!>(E)` represents **success or failure** with an explicit error value. Unwrapping is strictly explicit: every result-typed call is consumed by one of the two operators with cleanly separated roles — `?>` (default-value operator; pure expression RHS) or `!>` (error-handler operator; works at both statement and expression position) — or by an `@err(e);` forward inside a `!>` body, or by the registered `@errhandler` via the bare `call !>;` shorthand. Bare result-typed statements are ill-formed. See **Unwrapping Results** below.

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
  - At **expression position** the body must *visibly diverge*. Forms: `CALL !>(e) DIVERGENT_STMT;`, `CALL !>(e) { …; DIVERGENT_STMT };`, `CALL !> DIVERGENT_STMT;`, `CALL !> { …; DIVERGENT_STMT };`, and the bare `CALL !>;` which synthesizes a binder and inlines the enclosing `@errhandler` body (which must itself diverge). A non-divergent body at expression position is diagnosed with `expression-position '!>' body must diverge (return/break/continue/goto/@err/exit/abort/etc.)`. A bare `CALL !>;` at expression position with no enclosing `@errhandler` is diagnosed with `'!>;' at expression position requires an enclosing '@errhandler' in scope`.
3. Error values are accessible only via an explicit `(ident)` binder on `?>` or `!>`. Neither operator creates an implicit `e` / `err` binding. The bare expression-position `!>;` form synthesizes a fresh internal binder that threads the error value through the inlined handler body.
4. `CALL !>;` at statement position runs the registered default `@errhandler` on error, or the success path if the call succeeded.
5. `@err(IDENT);` inside a `!>` body forwards the bound error to the enclosing `@errhandler`. It is a **structured control-flow transfer** (not a returning call): any statement textually following it in the same block is unreachable and is a compile error.
6. `@errhandler(e) { ... }` is the unchanged registration spelling. When reached via `CALL !>;` at statement position, the handler body runs and control returns to the statement after the call — the handler may end in any statement. When reached via an `@err(e);` forward, via a bare expression-position `!>;`, or via an expression-position `!>` whose body inlines the handler, control never returns, so the handler body **must visibly diverge**. Concretely: if any `@err(e);` in the enclosing lexical scope targets a handler, or any expression-position `!>;` in scope of a handler inlines it, that handler's body must end in one of:
  - `return EXPR;` / `return;`
    - `break;` / `continue;`
    - `goto LABEL;`
    - `@err(e);` (forwarding to an outer handler)
    - A call to one of the hardcoded noreturn functions: `exit`, `_Exit`, `_exit`, `abort`, `longjmp`, `siglongjmp`, `pthread_exit`, `__builtin_unreachable`, `__builtin_trap`
    - A `{ ... }` compound statement whose recursive last statement satisfies this rule.
     A forward-reached or `!>;`-inlined handler whose last statement does not satisfy this rule is a compile error at the `@errhandler` declaration site. The rule applies in `void` functions equally.
7. A result-typed call that is not consumed by `?>`, `!>`, `@err`, assignment to a result-typed destination, `return`, or a `(void)` cast is ill-formed. `(void)call();` is the one explicit-discard escape hatch.

**Grammar (normative, minus whitespace).**

```
qmark_expr    ::= expr '?>' expr
               |  expr '?>' '(' ident ')' expr   // RHS is always a pure C expression

bang_stmt     ::= call '!>' ';'                            // statement: use registered handler
               |  call '!>' stmt                            // statement: single-stmt body (may fall through)
               |  call '!>' '{' stmt* '}' ';'?              // statement: block body (may fall through)
               |  call '!>' '(' ident ')' stmt              // statement: binder + single stmt
               |  call '!>' '(' ident ')' '{' stmt* '}' ';'?  // statement: binder + block

bang_expr     ::= call '!>' ';'                            // expression: delegate to outer @errhandler (inlined, must diverge)
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
```

**Semantics (normative, by form).**

- `EXPR ?> DEFAULT_EXPR` — Evaluate `EXPR` (a `T!>(E)` result) exactly once. If success, the expression's value is the unwrapped `T`. Otherwise the expression's value is `DEFAULT_EXPR`. `EXPR ?>(e) DEFAULT_EXPR` binds the error to `e`, scoped to `DEFAULT_EXPR`. `DEFAULT_EXPR` is always a pure C expression producing `T`.
- `CALL !>;` *(statement)* — Evaluate `CALL` exactly once. On success, the success payload is discarded. On error, the enclosing lexical `@errhandler(E e) { BODY }` runs; control then falls through to the following statement.
- `CALL !> BODY` *(statement)* — Same, with `BODY` in place of the default handler. `BODY` may fall through. `@err(e);` inside `BODY` is ill-formed without a binder.
- `CALL !>(e) BODY` *(statement)* — Same, with the error bound to `e` for the scope of `BODY`. `@err(e);` inside `BODY` forwards to the enclosing `@errhandler` (see invariant 5).
- `CALL !>;` *(expression)* — Evaluate `CALL` exactly once. On success, the surrounding expression's value is the unwrapped `T`. On error, a synthesized binder captures the error and the enclosing `@errhandler` body is inlined in place of `BODY`; the handler must diverge, so control never returns past the `!>;`.
- `CALL !> DIVERGENT_STMT;` and `CALL !> { …; DIVERGENT_STMT }` *(expression)* — Evaluate `CALL` exactly once. On success, the surrounding expression's value is the unwrapped `T`. On error, `DIVERGENT_STMT` (or the block) runs; because it cannot fall through, the surrounding expression has no observable value on that path. `!>(e) …` binds the error to `e` across the body.
- `@err(X);` — Inside a `!> (X) BODY` or `!> (X) { BODY }` (statement or expression position). Transfers control to the enclosing `@errhandler(E e) { HANDLER_BODY }` with the error value forwarded. Does not return.
- `@errhandler(E e) { BODY }` — Registers a block-local default handler. `CALL !>;` at statement position, `@err(e);` forwards, and `CALL !>;` at expression position all dispatch through this handler. Subject to the divergence rule of invariant 6.

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

// Bare expression-position !>; delegates to the enclosing @errhandler.
int main(void) {
    @errhandler(CCError e) {
        log(e);
        return 1;
    }
    int v = parse_int(s) !>;        // error -> handler -> return 1
    return v;
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

**Composition with `@defer` and `@create(...) @destroy`.** `?>` and `!>` participate in deferred cleanup like any other C statement: `@defer` scheduled entries run on scope exit regardless of which branch of `?>` / `!>` fired. Divergent RHS (`return`, `break`, `continue`) respect the scope boundary they cross; the surrounding `@defer` runs as usual.

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
a value-typed declaration passes its address. If both hooks are registered,
the order is registered pre-destroy hook, call-site body, registered destroy
hook. Built-in owner families use the same order: nursery wait, user body,
nursery free; arena or channel user body, then destroy/free.

Bodyless `@destroy` requires a registered pre-destroy or destroy hook and emits
only those hooks. If no hook is known, compilation fails at `@destroy`.
An explicit body remains valid without a registered hook and lowers to the
declaration-bound deferred body.

The same declared-type hook lookup applies to a direct initializer such as
`CCArena a = cc_arena_heap(n) @destroy;`; an unwrap operator is not required
when the initializer is infallible. Pointer declarations pass the pointer value
to a registered hook, while value declarations pass the object's address.

**Nullable pointers.** Both operators also consume pointer-typed operands. For a pointer-typed `EXPR`, the failure condition is `EXPR == NULL` instead of the result's error arm. On failure, a `CCError` is synthesized with `kind == CC_ERR_NULL` and a compile-time-constant message of the form `NULL returned from <call expression> at <file>:<line>`; the binder forms (`!>(e)`, `?>(e)`) bind that synthesized error. Every form and rule above applies unchanged:

- `PTR_EXPR ?> DEFAULT_EXPR` yields the pointer when non-NULL, otherwise `DEFAULT_EXPR` (a pure expression of the pointer's type).
- Statement- and expression-position `!>` behave per invariant 2; bare `!>;` dispatches the synthesized error to the enclosing `@errhandler`.
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
2. `!E` (result)
3. `[n]` `[:]` `[~n]` (array / slice / channel)

**Rationale:** Pointer binds tightest because "result of pointer" (`T*!E` → `(T*)!E`) is far more common than "pointer to result" (`(T!>(E))`*). Functions returning pointer-or-error are ubiquitous in systems code.

**Examples:**


| Syntax        | Parses as         | Meaning                                      |
| ------------- | ----------------- | -------------------------------------------- |
| `int`*        | `(int)`*          | pointer to int                               |
| `int*!E`      | `((int)*)!E`      | result of pointer (success=pointer, error=E) |
| `int!>(E)`    | `(int)!>(E)`      | result of int or E                           |
| `int!>(E)[~]` | `((int)!>(E))[~]` | channel of results                           |
| `int*!E[~]`   | `(((int)*)!E)[~]` | channel of (result of pointer)               |
| `int[:]*`     | `((int)[:])*`     | pointer to slice                             |
| `int*[:]`     | `((int)*)[:]`     | slice of pointers                            |


**Common patterns:**

```c
// Function returning pointer or error — the common case
Node*!>(IoError) find_node(int id);      // (Node*)!IoError

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

**Rule (`T[n:]` semantics):** `T[n:]` is a slice type with a compile-time known length `n`. It has the same ABI as `T[:]` (32 bytes), but the type system statically guarantees `len == n`. This enables bounds-checked indexing to elide runtime checks. `T[n:]` implicitly converts to `T[:]` (information is erased, not lost).

**Rule (`!` marker semantics):** The `!` suffix on a slice type is a **type-level uniqueness guarantee**. A value typed `T[:!]` or `T[:k!]` is statically required to carry `id.is_unique=1` at the ABI level (§3.4) — i.e., the compiler rejects any assignment, copy, or parameter pass that would duplicate it outside a move context (`cc_move()`, `return`, `send_take`). Use `T[:!]` in function signatures to **demand** that callers hand over ownership. `T[:]` by contrast says nothing about uniqueness at the type level — the value may or may not be unique; the compiler relies on the runtime `is_unique` bit to enforce copy rules at the call site.

**Rule (`T[:k]` / `T[:k!]` semantics):** Sentinel slices are ABI-identical to `T[:]`. The sentinel value `k` is a type-level guarantee about the element just past the logical end of the view (typically `k = 0` to guarantee NUL-termination for C interop). Applying `!` composes the two guarantees: `T[:k!]` demands both the sentinel and type-level uniqueness.

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

- `slice.ptr` yields `T`*

---

### 3.4 Slice ABI (32 bytes)

All slices lower to the following ABI:

```c
struct Slice_T {
    T*       ptr;      // 8 bytes: element pointer
    size_t   len;      // 8 bytes: element count
    uint64_t id;       // 8 bytes: allocation ID with flags (see below)
    size_t   alen;     // 8 bytes: allocation length (for send_take eligibility)
};
```

**ID field encoding:**

```
// Slice ID bit layout (uint64_t id):
//
// Bits 0–60 : allocation ID (opaque, non-zero for tracked allocations)
// Bit 61    : is_transferable
// Bit 62    : is_subslice
// Bit 63    : is_unique
```

- **Bits 0–60 (allocation ID):** Unique per tracked allocation. 0 indicates static or untracked memory.
- **Bit 61 — `is_transferable`:** 1 if the allocation may be transferred across threads via `send_take`; 0 otherwise.
- **Bit 62 — `is_subslice`:** 1 if the slice does not cover the full allocation.
- **Bit 63 — `is_unique`:** 1 if the slice has destructor semantics and is move-only.

**Special ID values:**


| Condition                                           | Meaning                                                                                                           |
| --------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| `id == 0`                                           | Static or untracked slice (string literals, unsafe slices). Not unique, not transferable, no provenance tracking. |
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
  - Reset (for arena-backed owners, `arena_reset`)
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
| `**is_subslice` (bit 62)**     | Set to 1 iff the derived slice does not cover the entire allocation                            |
| `**alen`**                     | Preserved as the allocation length for all derived slices                                      |


`**is_subslice` computation:**

- `s[..]` where `s.len == s.alen` → `is_subslice = 0`
- `s[0..s.len]` → `is_subslice = 0`
- `s[a..b]` where `a != 0` or `b != s.alen` → `is_subslice = 1`

This guarantees that full-range subslices remain eligible for transfer (if the source was transferable), while partial subslices are rejected — with no runtime table required.

**Rule (ptr invariant):** For tracked allocations (unique slices from `recv()` or `adopt()`), the `ptr` field of the owning unique slice is always the allocation base pointer. Subslicing adjusts `ptr` (and `len`) but preserves `alen`. This invariant, combined with `is_subslice`, ensures `send_take` eligibility is decidable without runtime lookup: `is_subslice == 0` implies `ptr` equals the allocation base and `len == alen`.

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

A type marked `@scoped` is **tied to a lexical scope** and cannot outlive that scope. Most importantly, a scope-bound value cannot be held across a suspension point (@await, @async call).

Types registered as scope-bound cannot cross the boundaries below.

**Characteristics:**

- Cannot be returned from a function (unless function is `@noawait`)
- Cannot be stored in a non-scoped struct field
- Cannot be passed across `@await` or `@async` function call boundaries
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

- `@await` expression (any @await)
- Call to `@async` function
- A suspending `send`, `send_take`, or `recv` in an `@async` body
- Call to `cc_block_on` / `cc_block_on_intptr` (explicit blocking)

**Non-suspension points** (safe to hold scope-bound values):

- Call to sync function (non-`@async`)
- Local variable creation / destruction
- Arithmetic, logic, control flow
- Non-blocking operations (`try_send`, `try_recv`, `close`)

#### 4.2.2 Cancellation and Deadline Observation

Suspension alone does not impose one cancellation policy. Observation depends
on the source and operation:

- Nursery cancellation is observed by `cc_cancelled()` /
  `cc_nursery_is_cancelled()` and by runtime waits that explicitly use the
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

`@scoped` on a parameter means the value must be released (or the function must be `@noawait`) before any suspension point.

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

- **Arena API** — creation, allocation, lifecycle
- **§5.1 `@defer`** — scoped cleanup
- **§5.2 Scoped arena lifetimes** — ordinary lexical scopes with `@destroy`

`spec/draft_alloc_strategy.md` specifies the implemented heap-overflow,
per-allocation release, reallocation, and checkpoint-gating contract.

---

Arenas own memory; slices are views into arena-owned storage.

```c
// Creation
CCArena cc_arena_heap(size_t bytes);          // heap-backed first slab; default block_max = 0
int cc_arena_buffer(CCArena* a, void* buf, size_t cap);  // caller-provided first slab; default block_max = 1

// Value-returning constructor with explicit growth policy. Use CC_ARENA_FIXED
// for a strict cap (no heap overflow), CC_ARENA_GROWABLE for a caller-owned
// root with unbounded heap-slab overflow, or a literal N > 1 to bound total
// blocks. cc_arena_fixed_buffer(buf, cap) is the 2-arg shorthand for
// CC_ARENA_FIXED and is the target of the `CCArena a = @create(buf, cap)`
// sugar.
CCArena cc_arena_create_buffer(void* buf, size_t cap, unsigned block_max);
CCArena cc_arena_fixed_buffer(void* buf, size_t cap);
#define CC_ARENA_FIXED     1u
#define CC_ARENA_GROWABLE  0u

// Stack scratch + arena in one declaration (storage is name##_cc_stack_buf[nbytes])
#define CC_ARENA_STACK(name, nbytes)

// Lifecycle
void cc_arena_free(CCArena* a);                   // free heap overflow slabs; clear handle
void cc_arena_reset(CCArena* a);                  // unwind to original user block; bump provenance
void cc_arena_destroy(CCArena* a);                // alias for cc_arena_free

// Checkpoints (cross-block aware)
typedef struct CCArenaCheckpoint CCArenaCheckpoint;
CCArenaCheckpoint cc_arena_checkpoint(CCArena* a);
void cc_arena_restore(CCArenaCheckpoint checkpoint);

// Allocation (grows when block_max allows; see Growable arenas)
void* cc_arena_alloc(CCArena* a, size_t nbytes, size_t align);
void* cc_arena_alloc_local(CCArena* a, size_t nbytes, size_t align);       // current slab only, non-atomic
void* cc_arena_alloc_local_grow(CCArena* a, size_t nbytes, size_t align);   // local then cc_arena_alloc if full
#define arena_alloc(T, arena, count)  // tracked, count elements
#define arena_alloc1(T, arena)        // tracked, 1 element
// Macros: cc_arena_alloc_T_*, cc_arena_alloc_T_*_local, cc_arena_alloc_T_*_local_grow

int cc_arena_would_fit(const CCArena* a, size_t nbytes, size_t align);  // current slab only (no grow)

**Fixed-size Pool API (normative):**
A `CCArenaPool` provides O(1) `alloc` and `free` for uniform objects by managing a freelist on top of an arena.
```c
typedef struct CCArenaPool CCArenaPool;
void cc_arena_pool_init(CCArenaPool* p, CCArena* a, size_t sz);
int cc_arena_pool(CCArenaPool* p, size_t sz);      // creates and owns its own arena
void* cc_arena_pool_alloc(CCArenaPool* p);         // UFCS: p.alloc()
void cc_arena_pool_free(CCArenaPool* p, void* ptr); // UFCS: p.free(ptr)
CCArena cc_arena_pool_detach(CCArenaPool* p);      // UFCS: p.detach_arena()
void cc_arena_pool_destroy(CCArenaPool* p);        // UFCS: p.destroy()
```

The pool is reclaimed when the underlying arena is reset or freed. If the pool owns its arena (created via `cc_arena_pool`), `cc_arena_pool_destroy` will free the arena.

**Stack-backed Pool (normative):**
`CC_ARENA_POOL_STACK(name, elem_size, nbytes)` declares a stack-backed arena and a pool initialized from it. It is equivalent to:

```c
CC_ARENA_STACK(name_arena, nbytes);
CCArenaPool name;
cc_arena_pool_init(&name, &name_arena, elem_size);
```

// Growth policy (field on CCArena; affects future growth only)
// a->block_max = 0;  // unbounded growth
// a->block_max = 1;  // fixed, no growth
// a->block_max = N;  // at most N blocks total

// Transfer (moves ownership of all blocks)
CCArena cc_arena_detach(CCArena* a);              // move ownership out

// Size helpers
size_t kilobytes(size_t n);            // n * 1024
size_t megabytes(size_t n);            // n * 1024 * 1024

```

**Arena ownership model:**

Arenas track their ownership per slab:
- **Heap-backed root** (created with `cc_arena_heap`) owns its initial buffer; `cc_arena_free` frees the active heap block and all heap overflow slabs.
- **User-backed initial buffer** (`cc_arena_buffer`, `CC_ARENA_STACK`, etc.) is never freed by the arena. `cc_arena_free` frees only **heap** extent buffers (overflow from growth), then clears the arena handle (`base` becomes NULL). Call `cc_arena_buffer` again before reuse after `free`.
- Extents in the growth chain record heap vs user ownership; freeing never calls `free` on stack or static storage.

This allows uniform cleanup for heap-first arenas:

```c
// Works for both heap and user-backed arenas
CCArena a = cc_arena_heap(kilobytes(64));  // or: cc_arena_buffer(&a, buf, sz)
// ... use arena ...
cc_arena_free(&a);  // frees heap-backed root and overflow; user root buffer untouched
```

**Stack-first scratch (normative):** `CC_ARENA_STACK(name, nbytes)` declares `uint8_t name##_cc_stack_buf[nbytes]` and a `CCArena name` initialized from that buffer. It is equivalent to `cc_arena_buffer(&name, name##_cc_stack_buf, sizeof(name##_cc_stack_buf)); name.block_max = 0;`. Hot allocations use the stack slab; overflow uses the same growth rules as heap arenas. Use `cc_arena_reset` to reclaim overflow and point `name.base` back at the stack buffer for repeated scopes; use `cc_arena_free` when discarding the handle (then re-init if needed).

**Growable arenas (normative):**

Arenas grow when `block_max` allows it. `cc_arena_heap` defaults to **growable** (`block_max = 0`, unbounded). `cc_arena_buffer` defaults to **fixed** (`block_max = 1`, no growth). When allocation exhausts the current block and growth is allowed, a new block is allocated; its size is at least **max**(1.5× the previous block’s capacity, space required for the **pending** allocation, 4096 bytes). The full previous block is pushed into a linked chain of extents. The root `CCArena` struct always holds the *active* block.

- `block_max = 0`: Unbounded growth (default for `cc_arena_heap`; `CC_ARENA_STACK` sets this after buffer initialization).
- `block_max = 1`: Fixed, no growth allowed (default for `cc_arena_buffer`).
- `block_max = N` (N > 1): At most N blocks total (initial user or heap block counts as block 0). Growth beyond the budget returns NULL.

```c
// Growable arena: will automatically allocate new blocks as needed
CCArena a = cc_arena_heap(kilobytes(4));
for (int i = 0; i < 10000; i++) {
    arena_alloc(int, &a, 100);  // seamlessly grows across blocks
}
cc_arena_free(&a);  // frees all blocks

// Heap-backed arena with explicit budget
CCArena b = cc_arena_heap(kilobytes(16));
b.block_max = 4;  // at most 4 blocks total
// ... after 4 blocks are exhausted, arena_alloc returns NULL

// Stack-first growable scratch (overflows to heap when needed)
CC_ARENA_STACK(scratch, 4096);
void *p = cc_arena_alloc(&scratch, n, align);
// ... cc_arena_reset(&scratch) to reuse stack slab, or cc_arena_free(&scratch) when done

// Stack-backed fixed arena (no heap overflow)
uint8_t buf[4096];
CCArena c;
cc_arena_buffer(&c, buf, sizeof(buf));  // block_max = 1, no growth

// Caller-provided buffer that may spill to heap
CCArena d;
cc_arena_buffer(&d, buf, sizeof(buf));
d.block_max = 0;  // unbounded growth beyond the initial user buffer
```

**Rule:** `cc_arena_reset` unwinds all grown extents, frees their buffers and extent structs, and restores the root arena to its original block. After reset, the arena is back to its initial capacity with `block_idx = 0`.

**Rule:** `cc_arena_checkpoint` / `cc_arena_restore` are cross-block aware. A checkpoint captures `block_idx` along with the allocation offset. Restoring a checkpoint taken in an earlier block unwinds the growth chain to that block, freeing all intervening blocks.

**Rule:** `cc_arena_free` walks the extent chain and frees **heap-owned** buffers only, then frees the active root buffer if it is heap-owned. User/stack/static initial buffers are never freed.

**Rule:** Changing `block_max` affects only future growth attempts. It does not rewrite existing extents or change ownership of the current block.

**Rule:** `cc_arena_alloc` uses atomic compare-and-swap on the bump offset and is safe to share across threads when every allocation goes through it (or equivalent synchronization).

**Rule:** `cc_arena_alloc_local` and `cc_arena_alloc_local_grow` use a non-atomic fast path on the current slab; they MUST only be used when **one** thread or fiber exclusively holds the arena during those calls. The “grow” half of `cc_arena_alloc_local_grow` delegates to `cc_arena_alloc`. `cc_arena_would_fit` reports whether the **current** slab can satisfy an allocation without growing (atomic load of offset).

**Rule:** Arena-allocated slices can be sent through channels or captured in thread closures when lifetime rules (§2.2) are satisfied.

**Rule (arena lifetime obligation):** `arena_reset` and `arena_free` must not be called while any slice derived from that arena may still be used on any thread. When the conflict is statically visible (live lexical borrow, or nursery spawn capture of an arena borrow), the compiler rejects `arena_reset` / `arena_restore`. When not statically detectable, violating this rule is undefined behavior in release builds; debug builds may trap via epoch helpers such as `cc_slice_is_from_arena_epoch`.

**Arena checkpoints (normative):**

- An arena checkpoint captures the current allocation state of an arena and allows later restoration.
- Restoring a checkpoint releases all allocations performed after the checkpoint.
- Checkpoints MUST NOT invalidate allocations made prior to the checkpoint.
- Arena checkpoints do not alter arena ownership or lifetime rules.
- Taking a checkpoint starts a fresh arena provenance epoch for subsequent allocations.
- Restoring a checkpoint restores the checkpoint's provenance epoch so post-checkpoint allocations become stale while prior allocations remain valid.

```c
CCArena a = cc_arena_heap(megabytes(1));
char[:] s = arena_alloc(char, &a, 100);
CCNursery* n = cc_nursery_create(NULL) !> @destroy;

n->spawn(() => {
    use(s);  // OK: a still alive
});

cc_arena_free(&a);  // BUG: spawned task may still be using s
```

**Design Rationale:**

**Thread-safe allocation:** Arenas use atomic operations (lock-free compare-and-swap or spin-locks) to make allocation thread-safe. A synchronized bump allocator adds ~10-20ns overhead per allocation vs unsynchronized, but is still ~4x faster than `malloc`. The benefit of "all arena slices are safely shareable across threads" outweighs this minor cost.

**Manual lifetime management:** The caller, not the runtime, is responsible for coordinating `arena_free`/`arena_reset` with thread lifecycle. This avoids the overhead of reference counting or cycle detection. It also gives users explicit control: a thread can hold slices from a long-lived arena while other threads allocate and reset scratch arenas independently.

**Non-goal:** Arenas do **not** provide automatic deallocation or generational lifetimes. Users must ensure all threads have finished using an arena before resetting or freeing it. Violations are not caught automatically in release builds.

**Note:** For single-owner hot paths, `cc_arena_alloc_local` / `cc_arena_alloc_local_grow` reduce overhead vs the atomic allocator; they are the supported alternative to a separate `Bump` type for the common per-fiber arena case.

---

### 5.1 `@defer`

`@defer stmt;` schedules `stmt` to run on scope exit.

- Runs on all returns, including `!>` propagation.
- LIFO order.
- No exceptions or unwinding.

**Note (vs `@errhandler`):** `@defer` / `@defer(err)` schedule work at **scope exit** (or only when the function returns error/success). `**@errhandler`** defines behavior when a `call() !>;` statement or an `@err(e);` forward observes an **error**—at the unwrap site, not deferred to exit. See §3.1.

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
    
    Result* res = arena_alloc(Result, &res_arena, 1);
    if (!res) return cc_err(io_error(CC_IO_OUT_OF_MEMORY));
    
    // ... fill in res, do allocations ...
    
    // Transfer ownership: detach leaves res_arena empty, so cleanup is no-op
    res->arena = arena_detach(&res_arena);
    return cc_ok(res);
}
```

**Arena ownership transfer with `arena_detach()`:**

`arena_detach(Arena* a)` transfers the arena's memory to a new owner, leaving the source arena empty. This enables clean ownership transfer out of scoped blocks:

```c
CCArena cc_arena_detach(CCArena* a);  // returns arena contents, leaves a empty
```

After detach:

- The source arena has `base = NULL` - any cleanup becomes a no-op
- The returned arena owns all the memory and allocations
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
@cancel cleanup;  // defer will not run
```

**Rule:** `@cancel name;` prevents the named defer from running. It is a compile error to `@cancel` a defer that has already run or been cancelled.

**Rule:** The name introduced by `@defer name:` is scoped to the enclosing block, like a local variable declared at the `@defer` statement. Referencing it (including `@cancel`) before the `@defer` statement or outside the block is a compile error.

**Lowering (implementation sketch, not surface syntax):**

```c
// @defer cleanup: STMT;
// ...
// @cancel cleanup;

// lowers to:
bool __cleanup_active = true;
@defer { if (__cleanup_active) { STMT; } }
...
__cleanup_active = false;  // @cancel cleanup;
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
    
    @cancel rollback;  // success: don't rollback
    db.commit() !>(e) return cc_err(e);
}

// Conditional cleanup
void!>(IoError) process(char[:] path, CCArena* out) {
    CCArena scratch = cc_arena_heap(kilobytes(64));
    @defer cleanup: cc_arena_free(&scratch);
    
    char[:] data = read_file(&scratch, path) !>(e) return cc_err(e);
    
    if (should_keep(data)) {
        // Transfer to output arena
        char[:] copy = arena_alloc<char>(out, data.len);
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
    CCArenaCheckpoint cp = cc_arena_checkpoint(&scratch);
    char[:] tmp = arena_alloc(char, &scratch, 1024);
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
CCNursery* n = cc_nursery_create(NULL) !> @destroy;

// Value capture (default): x is copied, immutable in closure
n->spawn(() => { printf("%d", x); });       // ✅ OK
n->spawn(() => { x++; });                   // ❌ ERROR: value capture is immutable

// Reference capture: explicit sharing with mutation check
n->spawn(() => [&x] { printf("%d", x); });  // ✅ OK: read-only
n->spawn(() => [&x] { x++; });              // ❌ ERROR: mutation of shared ref

// Shared atomic storage uses the shipped C surface.
cc_atomic_int counter = 0;
n->spawn(() => { cc_atomic_fetch_add(&counter, 1); });

// Escape hatch: @unsafe bypasses check
n->spawn(@unsafe () => [&x] { x++; });     // ⚠️ OK: explicit unsafe
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
channel_handle := element_type '[~' capacity? mode? topology? direction ']'
capacity     := integer_constant_expr
mode         := 'async' | 'sync'
topology     := '1:1' | '1:N' | 'N:1' | 'N:N'
direction    := '>' | '<'   // REQUIRED (combined channels removed)
```

- `capacity` must be a compile-time integer constant expression (or omitted for unbuffered).
- `mode` defaults to `async` if omitted.
- `topology` tokens `1:1`, `1:N`, `N:1`, `N:N` are parsed as single tokens.
- Whitespace between components is optional: `T[~10 async N:1]` and `T[~10asyncN:1]` are equivalent.
- When both `topology` and `direction` appear, `topology` comes first: `T[~10 1:N <]`.

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
- `close(tx)` / `close(rx)` closes the underlying channel. Idiomatically, close is scheduled on nursery teardown: `CCNursery* n = cc_nursery_create(NULL) !> @destroy { tx.close(); };` (§8.1.4).
- `cc_channel_free(ch)` frees the channel. Always free the channel, not the handles.

**Ownership idiom:**

```c
int[~10 >] tx;
int[~10 <] rx;
CCChan* ch = cc_channel_pair(&tx, &rx) !> @destroy { cc_channel_free(ch); };

CCNursery* outer = cc_nursery_create(NULL) !> @destroy;
outer->spawn(() => consumer(rx));

CCNursery* inner = cc_nursery_create(outer) !> @destroy { tx.close(); };
inner->spawn(() => producer(tx));
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

CCNursery* outer = cc_nursery_create(NULL) !> @destroy;
outer->spawn(() => {
    CompressedResult* r;
    while (cc_io_avail(results_rx.recv(&r))) {
        cc_file_write(out, r->data);
        cc_arena_free(&r->arena);
    }
});

// Producer nursery closes results_tx after all workers exit, so the consumer sees EOF.
CCNursery* inner = cc_nursery_create(outer) !> @destroy { results_tx.close(); };
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

**Rule (broadcast `1:N`):** `send` never blocks on subscribers; if a subscriber's buffer is full, the oldest value for that subscriber is dropped.

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
`slot` as `T*` and `arena` as `CCArena*`. Explicit parameter types remain
allowed and are not overwritten. The builder's `slot` denotes uninitialized
storage for one `T`. `arena` is optional element payload backing supplied by
the caller (or `NULL`) — a write buffer for variable-sized bytes in `*slot`,
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
graceful close maps to `ok(false)`.

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

CCNursery* n = cc_nursery_create(NULL) !> @destroy;
// Pool destroyed when enclosing scope ends: .destroy called for each arena
n->spawn(() => {
    CCArena arena;
    arena_pool.recv(&arena);  // Borrow
    void* p = cc_arena_alloc(&arena, 100);
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

Channels support bidirectional error propagation for pipeline error handling.

**Functions:**

```c
// Close with error - recv() returns this error instead of EPIPE
void cc_chan_close_err(CCChan* ch, int err);

// Close rx side with error - send() returns this error
void cc_chan_rx_close_err(CCChan* ch, int err);
```

**Use case:** In parallel pipelines, when a worker encounters an error, it can signal both upstream (to stop producers) and downstream (to stop consumers):

```c
// Worker hits error - propagate BOTH directions:
cc_chan_close_err(results_tx, err);   // → Writer sees error on recv
cc_chan_rx_close_err(blocks_rx, err); // → Reader sees error on send!
return;  // Clean exit, no manual drain needed
```

**Rule (upstream propagation):** When `cc_chan_rx_close_err(ch, err)` is called, subsequent `send()` operations on that channel return `err` instead of blocking or returning `EPIPE`.

**Rule (downstream propagation):** When `cc_chan_close_err(ch, err)` is called, subsequent `recv()` operations return `err` instead of `EPIPE` after the channel is drained.

**Rule (regular close unchanged):** `cc_channel_close(ch)` continues to work as before—recv returns `EPIPE` when closed and drained.

---

## 8. Concurrency

Concurrent-C provides a single, unified concurrency primitive: the **nursery** (`CCNursery`, §8.1). A nursery is a scope-bound handle that manages task spawning, joining, and explicit cooperative cancellation. For the rare case where OS-level thread control is required, low-level APIs exist but should not be used in typical application code.

This section specifies:

- **§8.1 Structured Concurrency with `CCNursery`** — the primary pattern for all concurrent work
- **§8.2 Blocking and Non-Blocking Call Edges** — how `@blocking` / `@noblock` (function-level and call-site) determine the mode of each call edge from an `@async` body
- **§8.3 Tasks** — `CCTaskIntptr` frame/poll/drop ownership
- **§8.4 Channels in Async vs Sync** — context-sensitive channel operations
- **§8.5 Cancellation** — operation-specific cooperative cancellation and deadlines
- **§8.6 Streaming** — channel-based producers
- **§8.7 Runtime API** — function signatures for tasks, timing, and sync bridging
- **§8.8 Blocking, Stalling, and Execution Contexts** — execution model for blocking operations, stalling classification, and cancellation guarantees
- **§8.9 Error handling in async and nurseries** — composition of result unwrap operators (`?>`, `!>`, `@err`, `@errhandler`) defined in §3.1 with async functions and nursery teardown
- **§8.10 Named exclusive sections (`CCExclusive`)** — arena-backed, name-keyed mutual exclusion for short critical sections

---

### 8.1 Structured Concurrency with `CCNursery`

A **nursery** is a scope-bound handle that manages the lifetime, cancellation, and completion of spawned child tasks. Nurseries enforce a tree-shaped concurrency structure: every task is a child of some nursery, and no task outlives its nursery.

`CCNursery` is a library type constructed with `cc_nursery_create(NULL)` and released via `@destroy`. The construction-plus-destruction pattern is idiomatic:

```c
{
    CCNursery* n = cc_nursery_create(NULL) !> @destroy {
        // runs after all children have joined
    };
    n->spawn(() => work1());
    n->spawn(() => work2());
}
```

The `@destroy` clause on the declaration schedules nursery teardown (which joins all children) at scope exit. Nothing in the lowered form runs implicitly — `@defer`-shaped lifetime (§5.1) is the normative mechanism.

Ordinary lexical blocks create nested nursery lifetimes; `n->spawn(...)` is
UFCS on the explicit nursery handle.

**Properties:**

- Tasks spawned on `n` are children of `n`.
- The nursery's `@destroy` waits for all children to complete before returning.
- Child task handles cannot outlive the nursery's scope (compile-time error if they escape).
- `cc_nursery_wait(n)` joins every child and returns the first nonzero child
  error it records; it does not cancel siblings.
- Peer tasks cannot wait on each other (compile-time error).

---

#### 8.1.1 Construction

`cc_nursery_create(CCNursery* parent)` returns `CCNursery*!>(CCError)`. Pass `NULL` for a top-level nursery; pass a parent nursery for nested structured concurrency.

```c
@errhandler(CCError e) { fprintf(stderr, "fatal: %s\n", e.message); abort(); }

CCNursery* outer = cc_nursery_create(NULL) !> @destroy;
CCNursery* inner = cc_nursery_create(outer) !> @destroy;
```

The `!>` operator consumes the result: on success, the value is bound; on error, control transfers to the enclosing `@errhandler` (§3.1). The trailing `@destroy` is a `@defer`-shaped destructor (§5.1) that joins children on scope exit.

Nursery cleanup invokes `cc_nursery_wait` as a pre-destroy hook and discards
its `int` return value. It does not forward a child error to `@errhandler` or
`!>`. Code that must observe a child error calls `cc_nursery_wait(n)`
explicitly, checks the returned integer before leaving the scope, and then
allows the registered cleanup to free the nursery.

---

#### 8.1.2 Spawning

Spawn a child task via UFCS on the nursery handle:

```c
n->spawn(() => work());                 // lambda expression
n->spawn(worker_fn);                     // function reference
n->spawn(() => worker_with_arg(x));      // captured argument
```

The compiler enforces the following normative rules:

- **Rule (task handle escape):** A task handle returned by `spawn` may not be stored in a variable that outlives the nursery, returned from the enclosing function, or captured in closures escaping the nursery.
- **Rule (no peer joins):** A child task may not @await or otherwise join another sibling's completion.
- **Rule (no explicit join):** The nursery's `@destroy` is the only legitimate join point.

---

#### 8.1.3 Error Propagation and Cancellation

`cc_nursery_cancel(n)` requests cooperative cancellation for children. Child
code observes it through `cc_nursery_is_cancelled(n)`, `cc_cancelled()`, or a
wait that explicitly observes the current nursery. Nursery wait still joins
every admitted child before teardown returns. A child return value does not
cancel peers; the spawn wrapper and library API determine how task errors are
reported.

```c
CCNursery* n = cc_nursery_create(NULL) !> @destroy;
n->spawn(() => ok_task());
n->spawn(() => failing_task());     // returns cc_err(E)
int child_err = cc_nursery_wait(n);
if (child_err != 0) return map_child_error(child_err);
// The sibling runs to completion unless code explicitly calls cc_nursery_cancel.
```

---

#### 8.1.4 Channel Close Ordering

A nursery's registered pre-destroy hook waits for all children. Its
`@destroy` body then runs, followed by the registered free hook. Close a
producer channel in that body to signal EOF to a consumer owned by an outer
nursery:

```c
CCNursery* outer = cc_nursery_create(NULL) !> @destroy;
outer->spawn(() => consumer(rx));

CCNursery* producers = cc_nursery_create(outer) !> @destroy { tx.close(); };
producers->spawn(() => producer(tx));
```

The producer nursery joins producers, closes `tx`, and frees itself. The outer
nursery can then join the consumer after it drains to `ok(false)`.

The pre-destroy hook discards the integer returned by `cc_nursery_wait`; its
purpose here is ordering, not error forwarding.

Use nested nurseries to sequence producer-close before consumer-drain:

```c
CCNursery* outer = cc_nursery_create(NULL) !> @destroy;
outer->spawn(() => consumer(rx));

CCNursery* inner = cc_nursery_create(outer) !> @destroy { tx.close(); };
for (int w = 0; w < N; w++) inner->spawn(() => worker(tx));
// inner's @destroy closes tx after workers exit; consumer drains and
// outer's @destroy joins the consumer.
```

**Registered close form.** `n->close_on(tx)` is UFCS for
`cc_nursery_add_closing_tx(n, tx)`. It registers `tx` to close after nursery
wait and before nursery storage is released:

```c
CCNursery* n = cc_nursery_create(NULL) !> @destroy;
n->close_on(tx);                    // equivalent to @destroy { tx.close(); }
n->spawn(() => producer(tx));
```

An explicit `@destroy { tx.close(); }` body and `close_on(tx)` have the same
observable close-after-join placement, but they are distinct lowerings.

`@nursery`, bare `nursery { ... }`, `spawn { ... }`, and `@closing(...)` are
unsupported spellings and are compile-time errors. Structured concurrency uses
an explicit `CCNursery*` declaration and UFCS `spawn` / `close_on` calls.

If `CC_NURSERY_CLOSING_RUNTIME_GUARD=1`, a receive that would park in the
current nursery waiting for a channel registered in that same nursery's
`close_on` set fails with `EDEADLK`. This immediate specialized guard is
optional and is independent of the scheduler's general detector (§8.7.1).

---

#### 8.1.5 Guarantees

A nursery guarantees:

- All spawned children are joined before the nursery's `@destroy` returns.
- No child outlives its nursery's scope.
- No forgotten-join deadlocks (impossible syntactically).
- No cyclic peer waits (impossible syntactically).
- First recorded child error returned by an explicit `cc_nursery_wait`.
- Explicit cooperative cancellation through `cc_nursery_cancel`.
- Deterministic channel close ordering (via `@destroy { ch.close(); }`, or `close_on` from C).

A nursery does **not** guarantee:

- Deadlock freedom for channel cycles across sibling nurseries.
- Fairness or starvation freedom.
- Immediate cancellation of blocking operations (cooperative; see §8.5).
- Stack unwinding on cancellation.

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

Channels are **explicitly typed as async or sync** at declaration. The type determines whether operations suspend (`@async` channel) or block (sync channel). This eliminates context-dependent behavior and ensures compiler safety.

Channels also support **backpressure modes** to handle overload gracefully in server workloads.

---

#### 8.4.0 Backpressure Modes

When a bounded channel is full, different workloads need different strategies. Backpressure mode is specified at channel declaration.

**Backpressure modes:**

```c
enum BackpressureMode {
    Block,           // Block sender until space available (default)
    Drop,            // Drop oldest (FIFO) when full
    Sample(f32 rate) // Keep ~rate fraction, drop rest (deterministic sampling)
};
```

**Channel syntax with backpressure:**

```c
T[~N >]                    // Bounded async sender, Block mode (default)
T[~N <]                    // Bounded async receiver, Block mode (default)
T[~N >, Block]             // Explicit Block mode (sender handle)
T[~N <, Block]             // Explicit Block mode (receiver handle)
T[~N >, Drop]              // Drop mode sender
T[~N <, Drop]              // Drop mode receiver
T[~N >, Sample(0.1)]       // Sample mode sender
T[~N <, Sample(0.1)]       // Sample mode receiver
T[~N sync >]               // Bounded sync sender, Block mode
T[~N sync <]               // Bounded sync receiver, Block mode
T[~N sync >, Drop]         // Bounded sync sender, Drop mode
T[~N sync <, Drop]         // Bounded sync receiver, Drop mode
```

**Behavior:** See Appendix C for full backpressure modes comparison.

In brief:

- **Block** (default): Sender blocks/suspends until space; guaranteed delivery.
- **Drop**: Sender always succeeds; oldest message discarded when full.
- **Sample(r)**: Sender always succeeds; ~r% of messages kept, rest deterministically dropped.

**Rules:**

- Mode is fixed at declaration; channel type is immutable.
- `send()` always succeeds in Drop/Sample modes (never blocks).
- `recv()` never observes partial effects from drops: a drop removes a whole message and nothing else. Delivery order follows the §7.3 rule (`ordered` channels guarantee it; others do not promise it).
- Sample rate must be in range `[0.0, 1.0]`; behavior at boundaries:
  - `Sample(0.0)`: drop all messages
  - `Sample(1.0)`: equivalent to Block mode

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
| **Must @await**           | Yes, always                                     | No, never                                                        |
| **Blocks OS thread**     | No                                              | Yes                                                              |
| **Use in `@async` code** | Yes (primary)                                   | No (use async instead)                                           |
| **Use in sync code**     | No (use task)                                   | Yes (primary)                                                    |
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

CCNursery* n = cc_nursery_create(NULL) !> @destroy { work_tx.close(); };
n->spawn(() => producer());
n->spawn(() => consumer());
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

- `cc_nursery_cancel(n)` marks a nursery and wakes its children so
  nursery-aware waits can re-check that state.
- `cc_cancelled()` and `cc_nursery_is_cancelled(n)` poll nursery state.
- `cc_cancel(CCDeadline*)` marks one deadline object.
- Absolute deadline expiry wakes deadline-aware parks; the operation re-checks
  time and returns its timeout result.
- `cc_current_deadline()` exposes the innermost active deadline to operations
  such as channel select.

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

Nursery state and deadline-scope state use different polling APIs:

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

`cc_cancelled()` polls the current nursery's cancellation state;
`cc_nursery_is_cancelled(n)` polls a named nursery. The zero-argument
`cc_is_cancelled()` language macro polls the current `@with_deadline` scope.
The underlying C function `cc_is_cancelled(const CCDeadline*)` polls an
explicit deadline. These names are not interchangeable.

---

#### 8.5.4 Cancellation Semantics

No cancellation source forcibly interrupts C code or performs stack unwinding.
`cc_task_cancel` requests cancellation of a concrete task handle;
`cc_nursery_cancel` requests cancellation of nursery children. The runtime may
wake parked work to permit observation, but the operation still determines the
reported result. Channel cancellation is represented by
`cc_io_from_errno(ECANCELED)`.

---

#### 8.5.5 Deadline-Bounded Multiplexing

```c
CCDeadline d = cc_deadline_after_ms(5000);
size_t ready = (size_t)-1;
int rc = cc_chan_match_select(cases, ncases, &ready, &d);
if (rc == ETIMEDOUT) return cc_err(WorkerError_Timeout);
if (rc == ECANCELED) return cc_err(WorkerError_Canceled);
if (rc != 0) return cc_err(WorkerError_Io);
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
  and restores the previous deadline on every exit.
- `@with_deadline(ms) as handle { ... }` binds a `CCDeadline*` to that active
  deadline for explicit inspection inside the block.
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

    CCNursery* n = cc_nursery_create(NULL) !> @destroy { tx.close(); };
    n->spawn(() => produce(100, &tx));
    int x;
    while (cc_io_avail(rx.recv(&x))) use(x);
}
```

**Streaming with errors:**

```c
// Fail-fast: function can fail, channel carries plain values
@async void!>(IoError) read_lines(char[:] path, char[:][~]* out) {
    defer out.close();
    File f = open(path) !>(e) return cc_err(e);
    while (true) {
        char[:] line = f.readline() !>(e) return cc_err(e);
        if (line.len == 0) break;        // EOF: readline returns an empty slice
        @await out.send(line);
    }
}

// Per-item errors: each item can independently fail
@async void parse_nums(char[:][~]* in, int!>(ParseError)[~]* out) {
    defer out.close();
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
void cc_nursery_cancel(CCNursery* n);
bool cc_nursery_is_cancelled(const CCNursery* n);
bool cc_cancelled(void);  // current nursery

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
   `@destroy { chan.close(); }` or `close_on(chan)`
   ```

2. **`cc_block_on` heuristic warning.** `cc_block_on(T, f(...))` where `f` is an `@async` function that performs channel operations inside a loop and is not marked `@nonblocking` produces a warning:

   ```
   warning: cc_block_on with 'f' may deadlock
   note: 'f' has channel ops in a loop; consider explicit nursery concurrency
   or a larger buffer
   ```

   This is a heuristic, not a proof. Marking the function `@nonblocking` suppresses the warning; the compiler does not verify the annotation.

There is no general compile-time deadlock analysis. In particular, a consumer that receives inside the nursery that owns a channel's `close_on` compiles cleanly and deadlocks only at runtime; the fix is to move the consumer outside the owning nursery scope.

#### Runtime detection

The scheduler's monitor detects a deadlock when every worker thread is idle and internally parked fibers exist with no progress across a full stall interval (on the order of one second). On detection the runtime prints a diagnostic dump — worker and fiber counts, and each internally parked fiber with its park reason and the state of the channel it is parked on — and exits with code 124 (like `timeout`), so stuck programs surface in CI instead of hanging.

- Fibers inside `cc_external_wait_enter/leave` or `cc_deadlock_suppress_enter/leave` scopes are excluded from the verdict; an external wait is not a deadlock.
- `CC_DEADLOCK_ABORT=0` downgrades the exit to a warning: the dump prints and the (deadlocked) program keeps running, which allows log capture.
- `CC_NURSERY_CLOSING_RUNTIME_GUARD=1` (opt-in): a recv that would wait forever on a channel whose `close_on` owner is the current nursery fails with `EDEADLK` instead of deadlocking.

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

- Each such call edge is resolved to mode `@blocking` or `@noblock` by the four-step precedence chain (call site → callee decl → caller ambient → FFI/fallback default).
- `@blocking` edges construct `cc_run_blocking_task_intptr` tasks and use the
  ordinary child-task poll path.
- `@noblock` edges compile to a direct C call — the callee is contractually non-blocking (§8.2.7).
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

- `@noblock` functions (guaranteed non-blocking, inline)
- `@async` functions (must be awaited)
- Any function within `@await` context

**Violations (Compiler Warning/Error):**

Calling a non-`@async`, non-`@noblock` function without `@await` in a `@latency_sensitive` function is a compiler error or warning (depending on lint level).

**Example:**

```c
@noblock int parse_count(char[:] s);    // OK to call directly

@async void db_query(int count);        // Must be awaited

void process_logs(int count);           // Must be awaited or marked @noblock

@async @latency_sensitive void handler(Request req) {
    int count = parse_count(req.body);  // ✅ OK (@noblock, guaranteed fast)
    
    @await db_query(count);              // ✅ OK (awaited)
    
    process_logs(count);                // ❌ ERROR: blocking call in @latency_sensitive
    
    // Fix: Either @await it or mark it @noblock
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

- When the queue reaches `max_queue` capacity, new operations return `IoError::Busy` immediately without queueing.
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
- May fail with `IoError::Busy` if capacity is exhausted
- Have no guarantee of cancellation or bounded latency

#### 8.8.6 @noblock Contract

A function annotated `@noblock` asserts that it will never block or stall.

**Rules:**

- `@noblock` functions must not perform I/O, synchronization waits, or call non-`@noblock` functions
- The compiler must not wrap calls to `@noblock` functions when invoked from `@async`
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
- May fail with `IoError::Busy`
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
unbounded waits. `@noblock` is a checked assertion, not suppression of
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
    CCNursery* n = cc_nursery_create(NULL) !> @destroy;
    n->spawn(() => subtask_a(url));
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

Construction allocates the section header and discovery map from a caller-supplied `CCArena*`. Mutex entries are allocated from an arena pool on that same arena on first resolve.

```c
CCArena arena = @create(kilobytes(128)) @destroy;
CCExclusive* excl = cc_exclusive_create(&arena);
// or: excl = cc_exclusive_create_sized(&arena, 256);  // initial map hint
```

- `cc_exclusive_create(arena)` is equivalent to `cc_exclusive_create_sized(arena, 64)`.
- `cc_exclusive_create_sized(arena, initial_cap)` rounds `initial_cap` up to the next power of two (minimum 2). `initial_cap == 0` selects the default capacity (64).
- Both functions return `NULL` when `arena` is `NULL` or allocation fails.

The discovery map is an open-addressing table keyed by `uint64_t` name. It grows under an internal create mutex when load is high (approximately 75% full): capacity doubles, live entries are rehashed, and the prior table is retired. Retired tables are released with `cc_arena_release` at `cc_exclusive_destroy`, not at grow time, so lock-free lookups never observe a freed table.

#### 8.10.2 Mutex resolve

Resolve a name once and reuse the handle:

```c
CCExclusiveMutex m = excl->mutex(name);   // UFCS: cc_exclusive_mutex(excl, name)
```

`cc_exclusive_mutex` returns a `CCExclusiveMutex` carrying the section pointer, the name, and a cached runtime entry pointer. The first resolve for a name allocates a 64-byte-aligned entry (one cache line per entry, for false-share isolation) from the section's arena pool and inserts it into the discovery map. Subsequent resolves of the same name in the same domain return the same entry.

For hot loops, resolve once outside the loop rather than calling `excl->acquire(name)` each iteration (which resolves on every call).

#### 8.10.3 Acquire and release

The surface uses **acquire** / **release**, not lock / unlock:

```c
CCExclusiveGuard g = m.acquire();   // UFCS: cc_exclusive_mutex_acquire(&m)
... short critical section ...
g.release();                      // UFCS: cc_exclusive_guard_release(&g)
```

By-name acquire is also available:

```c
CCExclusiveGuard g = excl->acquire(name);  // UFCS: cc_exclusive_acquire(excl, name)
```

Multi-name acquire is deadlock-safe: names are always taken in ascending order.

```c
CCExclusiveGuard gs[8];
size_t n = excl->acquire_sorted(names, count, gs, 8);
  // UFCS: cc_exclusive_acquire_sorted(excl, names, count, gs, 8)
size_t n = excl->acquire_range(0, shard_count, gs, 8);  /* [lo, hi) */
  // UFCS: cc_exclusive_acquire_range(excl, 0, shard_count, gs, 8)
cc_exclusive_guards_release(gs, n);
```

`acquire_sorted` accepts unsorted input and deduplicates; both return `0` (no
locks held) when the unique name count exceeds `out_cap` or
`CC_EXCLUSIVE_ACQUIRE_MULTI_MAX` (64). Partial acquires are rolled back.

**Rule (idempotent release):** `g.release()` is idempotent. After the first release, the guard's entry pointer is cleared to `NULL`; a second `release()` or `destroy()` on the same guard is a local no-op and does not touch the lock word. This is intentional so end-of-hold cleanup does not read as a double-unlock bug in review.

`g.destroy()` and `@destroy` on a guard are aliases for `g.release()`.

An acquire blocks until the caller owns the named entry. Uncontended acquire is an inlined compare-and-swap on the entry lock word; contended acquire uses a slow path that may park the current fiber.

#### 8.10.4 Hash-shard geometry (`CCShardMask`)

Compose `CCExclusive` names `0 .. count-1` with a power-of-two shard mask:

```c
CCShardMask shards = cc_shard_mask_auto(64);       /* pow2(ncpu), clamped */
size_t i = shards.index(hash);                     /* hash & mask */
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
excl->destroy();   // UFCS: cc_exclusive_destroy(excl)
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
CCExclusive* cc_exclusive_create(CCArena* arena);
CCExclusive* cc_exclusive_create_sized(CCArena* arena, size_t initial_cap);
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
void cc_exclusive_guards_release(CCExclusiveGuard* guards, size_t n);
void cc_exclusive_guard_release(CCExclusiveGuard* g);
void cc_exclusive_guard_destroy(CCExclusiveGuard* g);

CCShardMask cc_shard_mask_make(size_t count);
CCShardMask cc_shard_mask_clamp(size_t n, size_t max);
CCShardMask cc_shard_mask_auto(size_t max);
size_t cc_shard_mask_index(const CCShardMask* m, uint64_t hash);

void cc_exclusive_lock_entry_slow(void* entry);      /* slow acquire path */
void cc_exclusive_unlock_contended(void* entry);     /* wake one waiter */
```

**UFCS surface (normative):**

- `excl->mutex(name)` — resolve
- `excl->acquire(name)` — resolve and acquire
- `excl->acquire_sorted(names, count, out, out_cap)` — unique ascending multi-acquire
- `excl->acquire_range(lo, hi, out, out_cap)` — contiguous ascending multi-acquire
- `excl->destroy()` — tear down section
- `m.acquire()` — acquire resolved mutex
- `m.free()` — explicit reclaim
- `g.release()` / `g.destroy()` — release guard
- `shards.index(hash)` — `CCShardMask` routing

---

## 9. Standard Library (UFCS-First Design)

This section defines the core standard library using **UFCS-first design**: method syntax is primary and UFCS lowering is type-directed and library-owned.

**Design principle:** UFCS is resolved from the concrete receiver type. The library owning that type or family defines the callee contract, including whether the receiver is passed by address or by value. Direct library-call forms may also be exposed, but they are API choices rather than the definition of UFCS itself.

**IMPORTANT DESIGN CONSIDERATION:** Concurrent-C generally orders parameters by semantic driver, with support context following unless the context itself is the controlling operand. This is especially visible in UFCS lowering, where the receiver is ordinarily the semantic subject. Accordingly, support context such as allocators usually appears last in direct-call APIs.

**Core UFCS rules (normative):**

- The receiver is the full expression to the left of `.` or `->`.
- The compiler resolves the receiver to a concrete type before choosing a UFCS lowering rule.
- Field access and mixed member chains participate normally: `holder.arena.free()` dispatches on `holder.arena`; `ptr->arena.free()` dispatches on `ptr->arena`.
- When multiple links appear in a chain, the nearest concrete typed receiver in the chain determines dispatch.
- Standard-library families define canonical lowered C namespaces (`cc_file_`*, `cc_arena_`*, `cc_string_*`, `cc_slice_*`, `cc_channel_*`); internal erased-core helpers remain implementation details.

**Rule (C-first dispatch, normative):** Ordinary C struct/union member access wins over UFCS. For `receiver.name(args)` (or `receiver->name(args)`), if `name` is a data member of the receiver's resolved type — including a function-pointer member — the expression is an ordinary C member access/call and UFCS is **not** considered. UFCS applies only when `name` is **not** a member of the receiver type. Shadowing semantics follow plain C: members shadow any UFCS family entry of the same name without ambiguity or diagnostic. This makes UFCS an error-trap layer on top of C member access rather than a competing dispatch rule.

**Rule (unresolved is ill-formed, normative):** If UFCS applies (the receiver's resolved type has no matching member and the receiver type has a registered UFCS family or a type-driven fallback namespace) but no callable can be produced — because the registered `.ufcs` handler returns the empty slice, no fallback family is registered for the receiver type, or the synthesized callee does not exist at link time — the program is ill-formed. The compiler must diagnose this at compile time with the receiver type, the method name, and the source location of the call, rather than silently falling through to ambient name lookup. A receiver whose type is not a struct/union and has no registered UFCS family is simply not a UFCS call and the ordinary C lookup rules apply unchanged (no diagnostic).

**UFCS Equivalence (Normative):**

For named types that use ordinary fallback UFCS, method syntax lowers to the receiver-type method family for that type.

- Pointer-style APIs receive the address of a value receiver and the pointer itself for a pointer receiver.
- By-value APIs receive the receiver value directly.

For types or families that register custom UFCS lowering with the type-registration machinery, `x.method(args)` lowers according to the registered handler instead. Standard-library families may be implemented either by ordinary `cc_type_register(...)` calls in `@comptime` code or by equivalent built-in family contracts; the resulting receiver-type-driven lowering is the normative behavior.

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
CCVec::[int] result = vec_map(vec_filter(input, is_even), double);
```

---

### 9.0 UFCS Registration and Custom Lowering

UFCS is a type-owned extensible language feature. Libraries declare custom lowering rules at compile time by registering hooks for a concrete type or type family.

**Primary registration API:**

```c
typedef CCSlice (*CCTypeCreateHandler)(CCSlice type_name, CCSliceArray argv, CCSliceArray arg_types, CCArena* arena);
typedef CCSlice (*CCTypeUfcsHandler)(CCSlice recv_type, CCSlice method, CCSlice mode, CCSliceArray argv, CCSliceArray arg_types, CCArena* arena);

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
    CCTypeCreateHook create;
    CCTypeDestroyHook destroy;
    CCTypeUfcsHandler ufcs;
} CCTypeHooks;

int cc_type_register(const char* type_name, CCTypeHooks hooks);
```

**Registration rules (normative):**

- Registrations appear in ordinary `@comptime { ... }` code.
- Registration discovery runs over the preprocessed translation unit, including
  included `.cch` headers, before type-owned lowering uses the registry.
  `cc_type_register(...)` is a compile-time marker API and returns `0`; see
  §14.5 for pipeline ordering.
- `type_name` must be a string literal naming either an exact concrete type such as `"CCArena"` or a trailing-wildcard family such as `"CCChanTx_*"` or `"CCChanRx_*"`.
- The second argument must be a hooks object literal, typically `(CCTypeHooks){ ... }`.
- Registrations are library-owned. The compiler selects a UFCS lowering rule from the resolved receiver type, not from the method name alone.
- Handlers may be named functions or non-capturing lambdas.
- `.create` is the type-owned construction hook. The compiler selects the overload from the declared type plus the `@create(...)` argument list.
- `.destroy` may register a pre-destroy hook, a destroy hook, or both. `pre_callee` runs before `callee`.
- The `.ufcs` hook is responsible only for choosing the lowered callee family. It does not execute the call.
- Returning the empty slice means "no custom rewrite; fall back to ordinary receiver-type UFCS".
- `.create` may be registered either as fixed callee strings (`cc_type_create_call(...)`, `cc_type_create_overloads(...)`) or as a callable hook via `cc_type_create_hook(...)`.
- Recognized hook fields are `.create`, `.destroy`, and `.ufcs`.

**UFCS handler contract (normative):**

- `recv_type` is the resolved receiver type name used for dispatch.
- `method` is the invoked method name.
- `mode` is an optional lowering mode chosen by the language surface. For example, async-aware families may distinguish ordinary calls from `@await`-driven lowering here. If unused, handlers should ignore it.
- `argv` is the rewritten argument-expression list only. It does not include the receiver.
- `arg_types` is the compile-time inferred type list for `argv`, positionally aligned with it.
- `arena` is temporary compile-time storage for constructing the returned callee name.

**By-value receiver lowering:** A UFCS handler normally selects an address-style receiver-family callee. If a family wants the receiver passed by value instead, it must return the lowered symbol via `cc_ufcs_emit_value(...)` or `cc_ufcs_emit_value_cstr(...)`.

**Create hook contract (normative):**

- `.create` is selected from the declared type that appears on the left-hand side of `name = @create(...)`.
- The compiler implicitly selects the registered creation overload from the `@create(...)` argument count.
- The current implementation supports at most two explicit `@create(...)` arguments, including callable create hooks.
- `cc_type_create_call("callee")` registers the one-argument form.
- `cc_type_create_overloads("callee1", "callee2")` registers one- and two-argument forms on the same `.create` hook.
- `cc_type_create_hook(handler)` registers a callable create hook; it receives `type_name`, `argv`, `arg_types`, and `arena`, and must return the lowered callee name as a slice.
- `cc_type_destroy_call("callee")` registers the destroy phase only.
- `cc_type_pre_destroy_call("callee")` registers the pre-destroy phase only; it runs before any call-site `@destroy { ... }` body.
- `cc_type_destroy_hooks("pre", "destroy")` registers both destroy phases.
- If a type registers a destroy callee, then `name = @create(...)` must be followed by explicit ownership syntax: either `@destroy` or `@detach`. Omitting both is a compile-time error.
- `@detach` does not take a cleanup body.
- For `name = @create(...) @destroy { body };`, lowering order is: registered `pre_callee`, then call-site `body`, then registered `callee`.
- `arg_types` for `.create` is inferred from the `@create(...)` argument list. Implementations may leave complex local expressions unknown.

**Preferred registration style:**

```c
@comptime {
    (void)cc_type_register("CCFile", (CCTypeHooks){
        .ufcs = (recv_type, method, mode, argv, arg_types, arena) => {
            (void)recv_type;
            (void)mode;
            (void)argv;
            (void)arg_types;
            return cc_slice_concat2(
                cc_slice_from_buffer("cc_file_", sizeof("cc_file_") - 1),
                method, arena);
        },
    });

    (void)cc_type_register("CCChanTx_*", (CCTypeHooks){
        .ufcs = cc_channel_tx_lower_c,
    });

    (void)cc_type_register("CCChanRx_*", (CCTypeHooks){
        .ufcs = cc_channel_rx_lower_c,
    });
}
```

UFCS registration and typed lifecycle hooks (`create`, `destroy`) use the same type-owned registration machinery.

`cc_ufcs_register(...)` is the direct UFCS-only helper. `cc_type_register(...)` is the general registration form and may define UFCS together with lifecycle hooks.

This same contract applies to standard-library families such as channels, files, strings, arenas, vectors, maps, and results. Family-specific naming and lowering remain library policy rather than compiler policy; shared erased-core machinery is permitted so long as the family contract is preserved.

---

### 9.1 Strings

**Type:** `String` — Arena-backed growable string (Vec)

```c
// C ABI (prefixed): typedef Vec_char CCString;
// Language surface: String
```

`String` is a small, moveable handle to an arena-backed buffer. Copying a `String` aliases the same storage. To obtain an independent copy, use `as_slice().clone(a)`. String contents live until the arena is reset/freed.

`String.as_slice()` is the canonical sentinel string view: it returns `char[:0]`, not plain `char[:]`.

#### 9.1.1 Core API

```c
// Direct library-call constructors and C ABI
String   string_new(Arena* a);
String   cc_string_from(expr, Arena* a);          // expression-generic helper
String   cc_string_from_slice(Arena* a, char[:] initial);
char[:0] @slice("...");                           // build-time canonical slice
String   @string(expr, Arena* a);                 // literal/single-value builder
String   @string(policy, `...`, Arena* a);        // templated builder
char[:]  @string(`...`);                          // arena-less bounded template (§9.1.2)
String* cc_string_push(String* s, char[:] data);
String* cc_string_push_char(String* s, char c);
String* cc_string_push_int(String* s, i64 value);
String* cc_string_push_uint(String* s, u64 value);
String* cc_string_push_float(String* s, f64 value);
String* cc_string_clear(String* s);
char[:0] cc_string_as_slice(String* s);

// UFCS (primary for users)
String* s.append(char[:] data);        // alias for push
String* s.push(char[:] data);          // appends data
String* s.push_char(char c);
String* s.push_int(i64 value);
String* s.push_uint(u64 value);
String* s.push_float(f64 value);
String* s.clear();
char[:0] s.as_slice();
size_t  s.len();
size_t  s.cap();
String  <primitive>.to_str(Arena* a);           // e.g. 42.to_str(&arena)
```

**Slice lifetime:** The sentinel slice returned by `as_slice()` remains valid until the next mutating call on the same `String` (e.g., `append()`, `push()`, `clear()`). For stable references, use `.clone(a)`.

**String construction model:**

- `@slice("...")` yields a build-time canonical `char[:0]` inside the existing slice/provenance model.
- `@string(expr, a)` builds a `String` from a literal, `char`*, `char[:]`, `String`, or a value that supports `to_str(a)`.
- `@string(policy, \`..., a)`lowers to`String` builder operations over literal chunks plus interpolation slots.
- `@string(\`...\`)` with no arena is the bounded-template stack form: it yields a `char[:]` borrow of a block-scoped buffer and requires every interpolation to have a statically bounded width (§9.1.2).
- Template slots are string-oriented. Accepted slot forms are `char*`, `char[:]`, and `String`; non-string values may bridge through `expr.to_str(a)` if the receiver type provides that UFCS conversion.
- Interpolation syntax: only `${expr}` and `$~tag{expr}` start a slot (where `tag` is a C identifier). `${expr}` is **untagged**—the policy gets an empty tag slice and the value slice. `$~tag{expr}` is **tagged**—the policy gets the tag slice `"tag"` and the value slice, so policies can distinguish holes (metadata, escaping tiers, i18n keys, and so on). Any other `$` in the template is literal text, so ordinary uses like prices or macros do not need escaping.
- To emit a literal `${` or `$~tag{` sequence, prefix `$` with backslash: `\${` and `\$~…` are not slots; the backslash is removed and the string helpers emit the remainder (same rules as other template backslash escapes, e.g. an even run of `\` before `$` restores slot parsing, as in `\\${x}`).
- Backtick template bodies preserve whitespace, indentation, and embedded newlines exactly as written, matching ordinary JavaScript template-literal whitespace behavior (no implicit dedent or trim).

Example:

```c
CCArena arena = cc_arena_heap(megabytes(1));
String s = string_new(&arena);
s.append("count=")
 .push_char('x')
 .push_int(42);
char[:0] view = s.as_slice();

String msg = @string(42, &arena);
String html = @string(html_policy, `<h1>${title}</h1>`, &arena);
// Tag example: policy sees tag "meta" for the second hole
String row = @string(row_policy, `name=${name}; age=$~meta{age}`, &arena);
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

- The buffer is a block-scoped `char[K]` where `K` is an integer constant expression: the decoded literal byte count plus the sum of the per-slot bounds above. The yielded slice's capacity (`alen`) equals `K` exactly.
- The slice is a borrow with **block lifetime and stack provenance**: it stays valid for the rest of the enclosing block, including across later statements and nested blocks. Each `@string` site gets its own buffer.
- The form is a pure expression and may appear anywhere an expression may (initializer, argument position, …).
- `bool` formats as `true` / `false`; integers format in decimal.
- A literal-only template is legal (bound = decoded literal bytes); the empty template yields the empty slice. Template escapes decode as usual (`\n` is one byte).
- Formatted output cannot exceed the computed bound by construction; an overflow indicates a compiler bug and aborts loudly at runtime.

```c
int v = 42;
char[:] s = @string(`v=${v}!`);   // "v=42!"; s.alen == 2 + 11 + 1
take_slice(@string(`arg=${v}`));  // expression position
```

The arena form is unchanged: the same template with an arena yields an owned `String` and accepts unbounded interpolation types (slices, `String`, `double` / `float`, `const char*`, …). For unbounded values with stack-only storage, pass a fixed-buffer arena (`cc_arena_fixed_buffer`, §5); on exhaustion the result is a poisoned `String` (§9.1.3), never truncated output.

#### 9.1.3 `String` failure poison (sticky)

`String` is an owner. A builder step that cannot acquire storage (arena exhaustion — including a too-small fixed-buffer arena — or a size overflow) **poisons** the `String` instead of truncating it:

- Every subsequent push is a sticky no-op returning `NULL`.
- `len()` reads 0, `as_slice()` is empty, `cc_string_cstr()` returns `NULL`.
- `cc_string_failed(&s)` reports the poisoned state.
- `cc_string_clear(&s)` is the explicit recovery: it resets the value to a valid empty string.

`@string(...)` templated construction follows the same contract: if the destination arena cannot hold the output, the result is a failed `String` — never partial bytes.

#### 9.1.4 Formatting (Global)

```c
// Build formatted strings (use with Arena)
str format(Arena* a, str fmt, ...);      // varargs
str format(Arena* a, fmt_args args);     // structured

// Example
str msg = format(&arena, "Hello {}! Score: {}", name, score);
```

---

### 9.2 Slices with UFCS

**Type:** `T[:]` — Mutable view into contiguous data

#### 9.2.1 Core Methods

```c
// Normative
size_t len(T[:] s);
T[:] slice(T[:] s, int start, int end);
T* ptr(T[:] s);

// Byte-slice checked index (CCSlice / char[:]) — Result in all builds
char !>(CCError) at(CCSlice *s, size_t index);           /* = get_checked */
char !>(CCError) get_checked(CCSlice *s, size_t index);
bool !>(CCError) set(CCSlice *s, size_t index, char c);

// UFCS
size_t           s.len();
char !>(CCError) s.at(size_t i);
char !>(CCError) s.get_checked(size_t i);
bool !>(CCError) s.set(size_t i, char c);
T[:]             s.slice(int start, int end);
T*               s.ptr();
```

#### 9.2.2 Query Methods

```c
// Normative
bool is_empty(T[:] s);
bool contains(T[:] s, T value);
int find(T[:] s, T value);              // -1 if not found

// UFCS
bool s.is_empty();
bool s.contains(T value);
int  s.find(T value);
```

#### 9.2.3 Mutation Methods

```c
// Normative (mutate in place)
void reverse(T[:] s);
void sort(T[:] s);                      // uses default <
void fill(T[:] s, T value);
void copy(T[:] dest, T[:] src);

// UFCS
s.reverse();
s.sort();
s.fill(T value);
s.copy(T[:] src);
```

Example:

```c
int[:] nums = ...;
nums.sort();
nums.reverse();
if (nums.contains(42)) { ... }
```

#### 9.2.4 Iteration

```c
// Standard range-for (already in Surface Syntax)
for (T x : slice) { ... }

// Enumeration (with index)
for (int i = 0; i < slice.len(); i++) {
    char item = slice.at((size_t)i) !>;   /* byte slices; Result-checked */
}
```

---

### 9.3 Arrays

Arrays in CC are still `T[N]` (fixed-size, stack or struct-embedded). UFCS methods work on arrays too (they decay to slices):

```c
int arr[10];

arr.len();           // 10 (slice decay)
arr.fill(0);         // fill all
arr.sort();          // sort
arr.reverse();       // reverse

// View as slice
int[:] view = arr[..];
```

---

### 9.4 Numeric Types with Methods

Primitive numeric types get UFCS methods for common operations:

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
extension is distinct from the `.ccs` / `.cch` source and header pair. The driver
applies an entry rewrite before the ordinary Concurrent-C pipeline:

1. Strip a leading `#!` shebang line when present.
2. Force-include `<ccc/script/prelude.cch>` at translation-unit scope.
3. When the unit has no top-level `main`, partition the body and inject a
   synthetic `int main(int argc, char **argv)`:
   - **TU scope:** `#` preprocessor lines, `@grammar` / `@comptime` blocks,
     `typedef` and `struct` / `enum` / `union` type definitions, function
     definitions and prototypes, and file-scope `static` / `extern`
     declarations.
   - **Synthetic `main`:** statements and non-static runtime-init declarations
     (including `@create` / `@destroy` locals).
4. Inject a default `@errhandler(CCError)` **inside** synthetic `main` that
   prints `e.message` to stderr and returns `1`, so statement-level `!>` works
   without a local handler. A user `@errhandler` in the script body overrides
   the default for that scope.
5. An explicit top-level `main` together with any MAIN-classified top-level
   statement is ill-formed.

#### 9.5.2 Driver invocation

`ccc` treats a first positional argument ending in `.shcc` as an implicit
`run` (shebang-friendly):

```text
ccc [ccc-flags...] path/to/tool.shcc [script-args...]
```

Args after the script path are program arguments (inserted after `--` for the
build-run step). Explicit `ccc run path.shcc [-- args...]` remains valid.

Recommended shebang:

```text
#!/usr/bin/env -S ./cc/bin/ccc
```

`.ccs` inputs are unchanged: they do not auto-run from a bare positional path.

#### 9.5.2a `@task` entry dispatch

When the driver injects synthetic `main` (no explicit top-level `main`), it
discovers TU-scope task functions and dispatches on the first program argument
when that argument begins with `@`:

| Invocation | Behavior |
| ---------- | -------- |
| `tool.shcc` | Run the synthetic-main statement body (default script). |
| `tool.shcc @` | Print discovered tasks (sorted) to stdout; exit 0. Each line is the task name, and when a CCDoc one-line summary is available (§2.4), a summary column. |
| `tool.shcc @name args…` | Strip `@name` from `argv`, then `return name(argc', argv')`. |
| `tool.shcc @unknown …` | Print an error and the task list (with summaries when present) to stderr; exit 2. |

A **task** is a translation-unit function definition of the form

```c
static int name(int argc, char **argv) { … }
```

(`static` / `extern` / `inline` optional). The parameter list must include an
`int` parameter and a `char` pointer parameter with at least two `*` tokens
(typically `char **argv`). Prototypes without a body, non-`int` returns, and
`main` are not tasks. Explicit `main` disables `@task` dispatch for that unit;
multi-entry scripts omit explicit `main`.

When `@name` selects a task, the statement body is not executed. Task names are
ordinary C identifiers; the `@` sigil is CLI-only and is not part of the
function name.

A CCDoc block immediately before a task supplies its listing summary: `@task`
tag text when present, otherwise the leading one-line summary (§2.4).

```text
./tools/perf.shcc @perf_regress --update
./tools/perf.shcc @
```

#### 9.5.3 Prelude and headers

`<ccc/script/prelude.cch>` includes `<ccc/std/prelude.cch>` and the script
headers below. Scripts do not `#include` the prelude; the driver injects it.

| Header | Role |
| ------ | ---- |
| `<ccc/script/stdio.cch>` | `CCStdio` — arena-bound stdin/stdout/stderr helpers |
| `<ccc/script/cli.cch>` | Flag and positional argv helpers |
| `<ccc/script/pathx.cch>` | Repo-root discovery and C-string path join |
| `<ccc/script/file.cch>` | Read / write / copy / print by path |
| `<ccc/script/sh.cch>` | Thin process run wrapper |
| `<ccc/script/temp.cch>` | `CCTempFile` with Result create and `@destroy` cleanup |

Arena parameters follow the stdlib convention: **arena last** on allocating
APIs. Fallible script helpers return `T !>(CCError)` (or the corresponding
`CCResult_*_CCError` form) unless noted.

#### 9.5.4 `CCStdio`

```c
CCArena a = @create(megabytes(1)) @destroy;
CCStdio io = @create(&a) @destroy;

char[:] in = io.read_all() !>;
io.write_all(out.as_slice()) !>;
io.println(msg.as_slice()) !>;
io.eprintln(err.as_slice()) !>;
```

`CCStdio` binds an arena for growing reads. `println` / `eprintln` write a
slice then a trailing newline. Template formatting uses language `@string`,
not a scriptlib wrapper:

```c
CCString s = @string(`perf check: OK with ${warnings} warning(s)`, &a);
io.println(s.as_slice()) !>;
```

#### 9.5.5 Path, file, process, and temp helpers

```c
bool update = cc_cli_flag(argc, argv, "--update");
const char* pos0 = cc_cli_positional(argc, argv, 0);

CCSlice root = cc_script_repo_root(argv[0], &a) !>;
CCSlice baseline = cc_script_path_join((const char*)root.ptr,
                                      "perf/compiler_baseline.txt", &a);
if (!cc_script_path_exists(baseline)) { /* … */ }

CCSlice bytes = cc_file_read_path(path, &a) !>;
cc_file_copy(src_slice, dst_slice, &a) !>;
cc_script_print_file(path_slice, &a) !>;

CCTempFile tmp = cc_temp_file(&a) !> @destroy;
cc_sh_run(program_path, arg_path, &a) !>;
```

`cc_script_repo_root` walks from the current working directory (and, failing
that, from `dirname(argv0)`) looking for a Concurrent-C repo marker
(`cc/src/cc_main.c`, `perf/compiler_baseline.txt`, or `.git`). Returned path
slices from path helpers are NUL-terminated for C interop.

`cc_sh_run` builds a `CCCommand`, runs it to completion, and fails with
`CCError` when the process exits non-zero.

#### 9.5.6 Scripting model

Prefer Concurrent-C structure for script *content*, and scriptlib for
orchestration:

- **Parse:** `@grammar(schema|rules)` (and SERDES where appropriate) over
  file or stdin bytes.
- **Format:** `@string(\`…${expr}…\`)` into a `CCString` / slice, then
  `CCStdio` or file write.
- **Glue:** path join, temp files, `cc_sh_run`, CLI flags — thin wrappers over
  `<ccc/std/>` process, dir, and I/O APIs.

Example (stdin transform):

```c
#!/usr/bin/env -S ./cc/bin/ccc

CCArena a = @create(megabytes(1)) @destroy;
CCStdio io = @create(&a) @destroy;
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
- **Loops** — range and async iteration
- **Slicing** — subslice syntax
- **String literals** — static slices
- **Closures** — lambda syntax
- **Type inference** — `auto` keyword
- **Structs** — struct syntax and initialization
- **Enums** — sum types with payloads
- **Generics** — generic types and functions

---

**Methods / UFCS:**

`x.method(args)` uses UFCS lowering. Dispatch is selected from the resolved receiver type, and libraries define custom lowering through type-owned registration, normally `cc_type_register(...)`. Depending on the API, the lowered call may pass the receiver by value or by pointer; that is family policy rather than surface syntax.

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

**Rule (UFCS in defer):** UFCS works uniformly in `defer` statements regardless of whether the receiver is a value or pointer:

```c
int[~10 >] tx;
defer tx.close();        // OK: lowers to close(tx)

int[~10 >]* tx_ptr = get_tx();
defer tx_ptr->close();   // OK: lowers to close(tx_ptr)
```

**Loops:**

Traditional C `for(;;)` is unchanged.

```c
for (T x : slice) { ... }       // range-for over slice
```

**Range-for lowering:**

```c
// for (T x : slice) { BODY }
// lowers to:
for (size_t __i = 0; __i < slice.len; __i++) {
    T x = slice.ptr[__i];
    BODY
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

**Rule (checked-index):** Protected byte-slice index ops (`at` / `get_checked` / `set`) return `CC_ERR_INVALID_ARG` on out-of-bounds or null in **all** builds — no debug/release split. Raw `.ptr` indexing and unchecked C stores are outside this surface (Gap). Subslice ops that cannot form a valid range yield an empty view.

**String literals:**

String literals have type `char[:]` with static provenance:

```c
char[:] s = "hello";  // static slice, always valid
```

**Rule:** String literal slices are sendable and have `owner = NULL` (static provenance).

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
CCNursery* n = cc_nursery_create(NULL) !> @destroy;

// ✅ OK: read value-captured variable
n->spawn(() => { printf("%d", counter); });

// ❌ ERROR: cannot modify value-captured variable
n->spawn(() => { counter++; });
// error: cannot modify value-captured variable 'counter'
// help: use [&counter] for reference capture

// ✅ OK: read-only reference capture
n->spawn(() => [&counter] { printf("%d", counter); });

// ❌ ERROR: mutation of shared reference
n->spawn(() => [&counter] { counter++; });
// error: mutation of shared reference 'counter' in spawned task
// help: use cc_atomic_*, a registered synchronization library, or @unsafe

cc_atomic_int safe_counter = 0;
n->spawn(() => { cc_atomic_fetch_add(&safe_counter, 1); });

// ⚠️ OK: explicit unsafe (you own this race)
n->spawn(@unsafe () => [&counter] { counter++; });
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

Enums are sum types with optional payloads:

```c
enum IoError {
    FileNotFound,
    PermissionDenied,
    InvalidArgument,
    Interrupted,
    OutOfMemory,
    Other(i32 os_code),
    Busy,
}

// Construction
IoError e = IoError.FileNotFound;
IoError e2 = IoError.Other(42);

// Matching (if-let style)
if (e == IoError.FileNotFound) { ... }
if (e is IoError.Other(code)) { use(code); }
```

**Generics:** See §12 for comprehensive generics documentation.

**Built-in generic types:**

- `CCTaskIntptr` — pollable async task handle
- `CCVec::[T]` — dynamic array
- `Map::[K, V]` — inline open-addressing hash map
- `ArrayMap::[K, V]` — probe index + dense key/value rows
- `T[~... >]` / `T[~... <]` — channel handles for element type T

**Built-in non-generic types:**

- `ThreadGroup` — multi-thread coordination
- `Thread` — OS thread handle
- `Arena` — memory arena
- `Scope` — (internal) structured concurrency handle created implicitly in @async functions
- `Ordering` — memory ordering enum (`relaxed`, `acquire`, `release`, `acq_rel`, `seq_cst`)
- `Duration` — time span (secs + nanos)

---

## 12. Generic Family Instantiation

`Name::[args]` is an instantiation use of a library-owned generic family.
It is not declarative generic syntax. Declarations such as
`struct Pair::[A, B] { ... }`, `void swap::[T](...)`, generic parameter
lists, value-generic parameters, partial inference, and generic impl blocks are
unsupported and produce compile-time errors.

### 12.1 Registered factories

A library defines a family with `CC_GENERIC_FACTORY(Name[, arity])` or the
underlying `cc_generic_register` API (§14.10). At each `Name::[args]` use, the
compiler:

1. computes the canonical concrete name,
2. invokes the registered base factory once for that concrete name,
3. invokes registered extensions in registration order,
4. splices the returned C definition before first use, and
5. rewrites the source use to the concrete C name.

The base factory must exist and return a non-empty C fragment. Each concrete
name is emitted once per translation unit. Extensions may return an empty
fragment.

### 12.2 Library-owned policy

The family owns emitted definitions, C symbol names, internal erasure, linkage,
and specialization. The compiler owns registration lookup, canonical argument
spelling, duplicate-emission prevention, source attribution, splice placement,
and use-site rewriting. The language does not infer generic parameters from
ordinary calls.

Shipped families include `CCVec::[T]`, `Map::[K,V]`, `ArrayMap::[K,V]`, result
families, and registered user/library families. Their public C names and
operations are defined by the owning headers. A family may emit specialized C
for one instantiation and erased wrappers for another without changing the
`Name::[args]` source rule.

### 12.3 UFCS composition

After instantiation and type resolution, UFCS dispatch uses the concrete
receiver type. A generic factory may emit its own C operations and register a
type or family UFCS hook. Generic instantiation does not create a second method
system and does not bypass the C-member-first rule in §9.0.

Generated C is ordinary first-class C: it participates in parsing, type
checking, linkage, diagnostics, emitted-C inspection, and subsequent UFCS/type
registration exactly as other lowered C does.

---

## 13. Collections

Standard collection types are defined in the **Standard Library Specification** (`concurrent-c-stdlib-spec.md`):

- `**CCVec::[T]`** — arena-backed dynamic array (`<std/vec.cch>`)
- `**Map::[K,V]`** — arena-backed inline open-addressing map (`<std/map.cch>`)
- `**ArrayMap::[K,V]`** — arena-backed index + dense rows (`<std/array_map.cch>`)

These types are generic, use UFCS methods, and require an `Arena*` at
construction. See the stdlib spec for full API reference, rules, and examples.

**Quick reference:**

```c
// CCVec::[T]
CCVec::[T] v = cc_vec_new::[T](&arena);
v.push(value);
T* x = v.get_ptr(index);
T[:] slice = v.as_slice();

// Map::[K,V] — tiny K/V, max probe locality
Map::[K, V] m = map_new::[K, V](&arena);
m.insert(key, value);
V* x = m.get_ptr(key);
m.remove(key);

// ArrayMap::[K,V] — wide values; empty buckets stay cheap
ArrayMap::[K, V] am = array_map_new::[K, V](&arena);
ArrayMap::[K, V] sized = array_map_new_count::[K, V](&arena, 1024);
am.insert(key, value);
V* y = am.get_ptr(key);
am.del(key);
```

**Implementation note:** The `CCVec::[T]`, `Map::[K,V]`, and `ArrayMap::[K,V]`
syntax is compile-time sugar that lowers to concrete C family types (e.g.,
`CCVec::[int]` → `CCVec_int`, `ArrayMap::[int,int]` → `ArrayMap_int_int`). UFCS
method calls on containers lower through that family contract; implementations
may use direct concrete symbols such as `CCVec_int_push(&v, x)` or thin family
wrappers over shared erased-core helpers. See the stdlib spec for full lowering
rules.

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

**Important interaction with generics:** `@comptime` parameters are the standard way to express lightweight "value generics" (Zig-style) without introducing a separate template system.

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

- `@comptime` blocks are also the place where libraries publish compile-time registrations such as `cc_type_register(...)`.
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

**Rule:** Control flow inside `@comptime {}` is allowed (`if`, `for`, `while`) as long as all conditions are constant-expression decidable.

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

**Rule:** `cc_instantiate_*` forces monomorphization of a built-in family even when the type is never spelled in source.

**Two `@emit` spellings.**

| Form | Returns | Use when |
|------|---------|----------|
| `@emit(\`...\`, arena)` | `CCSlice` | Generic factories and any `@comptime` function that builds definition text and returns it; the fragment is built into the caller-supplied `CCArena*` |
| `@emit(CCEmitAnchor, \`...\`)` | `void` (lowers to splice side effect) | `@comptime {}` blocks and `@comptime for` bodies that emit declarations at a named anchor |

Both forms share the same backtick `${...}` grammar as `@string`. Each `${expr}` slot uses type-driven dispatch (`cc_emit_tpl_append_slot` / C11 `_Generic`); supported types are `CCSlice`, C strings (`char*` / `const char*` / char arrays), integers, and floating-point.

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
- `arena` (`CCArena*`) — scratch arena for building the fragment

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

**Implementation note:** A factory compiles in-process on the libtcc comptime evaluator on first use (the same evaluator that runs `@comptime` blocks), not by spawning the host C compiler — first-use lowering is in the millisecond range. The relocated factory code stays resident for the remainder of the compile; if libtcc is unavailable the compiler falls back to a host-compiled shared object.

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
type_of(T).nfields     // field count
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

Compiled factories and `@comptime` blocks read the same field set through value helpers or the legacy byte callbacks:

```c
typedef struct CCReflectField { char name[128]; char type[128]; int index; } CCReflectField;
int cc_reflect_field_at(const char* type_name, int idx, CCReflectField* out);  // 0 ok, -1 err

int cc_reflect_field_count(const char* type_name);                              // -1 if unknown
int cc_reflect_field_name(const char* type_name, int idx, char* buf, int buf_sz);
int cc_reflect_field_type(const char* type_name, int idx, char* buf, int buf_sz);
```

**Rule:** `@comptime for` loop variables (`f.name`, `f.type`, `f.typestr`, `f.index`) and `${...}` slots inside `@emit` share the same field metadata.

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

**Implementation note:** By default the compiler routes `@comptime if` predicate evaluation and `@comptime for` field loading through the libtcc comptime executor (`CC_COMPTIME_UNIFIED_EXEC=1`). Set `CC_COMPTIME_UNIFIED_EXEC=0` to use the legacy structural text resolver only. Both `@string` and `@emit` share one backtick `${...}` scanner (`preprocess/template_scan.c`). `@emit` slot values are appended via type-driven `_Generic` dispatch in `cc_emit_tpl.cch`, not name heuristics.

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
- **Sendability verification:** Non-sendable types may be captured in closures within `unsafe`.
- **Borrow checking:** Borrow lifetime rules are not enforced.

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

// Casting away sendability
struct NonSendable { pthread_t tid; };
CCNursery* n = cc_nursery_create(NULL) !> @destroy;

unsafe {
    NonSendable ns = get_non_sendable();
    n->spawn(() => { use(ns); });  // ERROR still: closure escapes
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
CCVec::[char] buf = cc_vec_with_capacity::[char](&arena, 1000);
c_fill_buffer(buf.ptr, buf.cap());  // fill with C code
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
| Channel overflow   | Buffer full (Busy)                 | Signaled via IoError::Busy  |
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
of `@blocking` / `@noblock` / `async→async` are specified normatively
in **§8.2** and **Appendix J.1.1**. This appendix describes an
optional implementation optimization: coalescing adjacent `@blocking`
edges into a single thread-pool dispatch.

**Optimization:** A compiler MAY merge two or more lexically adjacent
`@blocking` call edges into one `cc_run_blocking_task_intptr` task
when **all** of the following hold:

1. The edges are not separated by an `@await`, a `async→async`
   edge, a `@noblock` edge, a suspension-capable statement, a
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
    CCNursery* n = cc_nursery_create(NULL) !> @destroy;
    n->spawn(() => { use(x); });  // OK: value capture (copy)
}
```

**Example (mutation checking):**

```c
@async void bad_race() {
    int counter = 0;
    CCNursery* n = cc_nursery_create(NULL) !> @destroy;
    n->spawn(() => [&counter] { counter++; });  // ERROR: mutation of shared ref
    n->spawn(() => [&counter] { counter++; });
}

@async void ok_atomic() {
    cc_atomic_int counter = 0;
    CCNursery* n = cc_nursery_create(NULL) !> @destroy;
    n->spawn(() => { cc_atomic_fetch_add(&counter, 1); });
    n->spawn(() => [&counter] { counter++; });
}

@async void ok_readonly() {
    int config = 42;
    CCNursery* n = cc_nursery_create(NULL) !> @destroy;
    n->spawn(() => [&config] { printf("%d", config); });  // OK: read-only
    n->spawn(() => [&config] { printf("%d", config); });
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

The arena implementation uses a "swapping chain" pattern. The root `CCArena` struct always holds the current (active) block. On growth, the current block's state is pushed into a heap-allocated extent struct linked via `prev`, and the root is updated with a fresh, larger buffer.

```c
struct CCArena {
    uint8_t* base;       // current block's buffer
    size_t   capacity;   // current block's capacity
    size_t   offset;     // atomic: bytes used in current block
    uint64_t provenance; // monotonic arena id
    uint32_t _flags;     // CC_ARENA_FLAG_HEAP_OWNED, CC_ARENA_FLAG_IS_EXTENT
    uint16_t block_idx;  // current block generation (0 = initial)
    uint16_t block_max;  // budget: 0 = unbounded, 1 = fixed, N = max
    CCArena* prev;       // previous full block (NULL if none)
};
```

**Growth (slow path):**

```
On alloc failure in current block:
  1. Check budget: if block_idx + 1 >= block_max (and block_max > 0), return NULL
  2. Allocate extent struct (malloc), copy root state into it
  3. Allocate new buffer (1.5x capacity, min 4096)
  4. Push: extent->prev = root->prev; root->prev = extent
  5. Update root: base = new_buf, capacity = new_cap, offset = 0, block_idx++
  6. Retry allocation in the new block
```

**Reset:** Walk `prev` chain to tail (original block), free all intermediate buffers and extent structs, restore root to original state, set offset = 0.

**Checkpoint/Restore:** Checkpoint captures `{arena, offset, block_idx, provenance}` and immediately starts a fresh arena provenance epoch for subsequent allocations. Restore checks if `checkpoint.block_idx < arena->block_idx`; if so, it unwinds the chain by freeing the current block and all extents newer than the checkpoint, restoring the root to the target block, then restores the checkpoint's provenance epoch.

**Per-request pattern:**

```c
while (true) {
    Request req = accept_connection();
    Arena req_arena = arena_with_capacity(megabytes(1));
    
    handle_request(&req, &req_arena);
    
    arena_reset(&req_arena);  // O(1) if no growth; frees extents if grown
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
struct Result_T_E {
    _Bool ok;         // C11 _Bool (1 byte)
    // padding to max(alignof(T), alignof(E))
    union { T value; E error; } u;
};
// sizeof(Result_T_E) = max_align + max(sizeof(T), sizeof(E))
```

**Example (Result::[int, ParseError]):**

If ParseError is 8 bytes:

```c
struct Result_int_ParseError {
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
    size_t   alen;     // 8 bytes (arena-allocated length)
};
// sizeof(Slice_T) = 32 bytes on 64-bit platforms
```

**ID field encoding (bit layout):**

```
Bit 63: is_unique         (1 = has destructor, move-only)
Bit 62: is_subslice       (1 = doesn't cover entire allocation)
Bit 61: is_transferable   (1 = safe to send_take across threads)
Bits 0–60: allocation_id  (0 = static/untracked)
```

**32-bit platforms:** Slice layout is implementation-defined. Implementations may use smaller pointer/size fields (4 bytes each) resulting in 20–24 byte slices, or preserve 64-bit fields for compatibility. Portable code should not assume slice size.

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
void* arena_alloc_aligned(Arena* a, size_t nbytes, size_t align);
```

Default: `max(alignof(T), 16)`. Larger alignments on explicit request.

### D.4 Binary Compatibility

**Stability guarantee:** A released ABI does not:

- Change field offsets in existing structs
- Change enum variant values
- Remove or rename functions
- Change calling conventions

**Debugging support:** The ABI is designed to be inspectable via standard C debuggers. Slice metadata (id field) enables provenance-aware debugging in custom tools.

---

### @noblock Contract

A function marked `@noblock` asserts it will never block:

**Rules:**

- No I/O, no synchronization waits, no channel operations
- Only CPU work (arithmetic, string ops, local structures)
- Compiler does not wrap in blocking executor
- Violations: Runtime trap (debug), UB (release)

**Example:**

```c
@noblock int parse_count(char[:] s) {
    // Safe: only CPU work
    return (int)atoi(s.ptr);  // via FFI
}

@async void db_query(int count);  // Must @await

@async @latency_sensitive void handler(Request req) {
    int count = parse_count(req.body);  // ✅ OK (@noblock)
    @await db_query(count);              // ✅ OK (awaited)
}
```

### @latency_sensitive Linting Rule

A function marked `@latency_sensitive` asserts it must not experience unexpected latency from coalescing:

**Rules:**

- Compiler must not coalesce stalling calls within function
- Only @noblock and awaited @async calls allowed
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
error: non-@noblock, non-@async call in @latency_sensitive function
  → process_logs(count);
  
  Fix: Mark process_logs @noblock, or make it @async and @await it
```

---

## Appendix E: Standard Error Types & Backpressure

### Error Types

```c
enum IoError {
    PermissionDenied,
    FileNotFound,
    InvalidArgument,
    Interrupted,
    OutOfMemory,
    Busy,              // ← Executor saturation
    Other(i32 os_code)
};

enum ParseError {
    InvalidUtf8,
    Truncated,
};

enum BoundsError {
    OutOfBounds,
};
```

### Backpressure Modes

**Three modes for different workloads:**


| Mode                | Behavior          | Queue Full         | Sender               | Receiver       | Use Case                      |
| ------------------- | ----------------- | ------------------ | -------------------- | -------------- | ----------------------------- |
| **Block** (default) | Block until space | Waits              | Suspends/blocks      | All messages   | Default, backpressure desired |
| **Drop**            | Discard oldest    | Discards head      | Succeeds immediately | Recent msgs    | High-volume, tolerate loss    |
| **Sample**          | Keep ~rate%       | Deterministic drop | Succeeds immediately | ~rate% of msgs | Sparse traces, fair sampling  |


**Used in:**

- Channels: `T[~N >, Drop]` / `T[~N <, Drop]`, `T[~N >, Block]` / `T[~N <, Block]`, `T[~N >, Sample(0.05)]` / `T[~N <, Sample(0.05)]`
- Logging: `log_drop()`, `log_block()`, `log_sample()`

**Key property:** Sampling is deterministic (reproducible, fair), not random.

---

## Appendix F: Server Programming Patterns

### Pattern 1: Request Handler

```c
@async @latency_sensitive Response!>(IoError) my_handler(Request* req, Arena* a) {
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
- Compiler enforces only @noblock and awaited @async calls
- Latency is observable (I/O dispatch is separate from CPU)

### Pattern 2: Backpressure Strategy

```c
// Three pipelines, three strategies

LogEvent[~10000, Drop] access_logs;      // High-volume, lossy
LogEvent[~1000, Block] audit_logs;       // Low-volume, critical
LogEvent[~100000, Sample(0.05)] traces;  // Very high-volume, 5% kept

// In handler:
log_drop(access_event);              // Never blocks request
log_block(audit, ms(100)) !>(e) return cc_err(e);   // Fail if timeout
log_sample(trace, 0.05);             // Deterministic 5% kept
```

**Properties:**

- Access logs drop oldest (never blocks)
- Audit logs block up to timeout (critical)
- Trace logs sample deterministically (sparse)

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
  channel or explicit `cc_nursery_cancel`
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
| `@noblock`           | Never blocks/allocates                     | Mark pure utilities         |
| `@latency_sensitive` | No coalescing allowed                      | Mark request handlers       |
| `@scoped`            | Cannot escape scope                        | Mark safe cross-thread refs |
| `spawn`              | Create task                                | Method on `CCNursery`       |
| `defer`              | Defer cleanup                              | Guarantee execution         |
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
| `T[~N, Mode ... >]` / `T[~N, Mode ... <]`     | `AsyncChanTx::[T, N, Mode]` / `AsyncChanRx::[T, N, Mode]` | Async handles with backpressure |
| `T[~ ... sync ... >]` / `T[~ ... sync ... <]` | `SyncChanTx::[T]` / `SyncChanRx::[T]`                     | Sync channel handles            |


---

## Appendix H: Complete Example: HTTP Server

```c
#include <ccc/std/prelude.cch>
#include <ccc/std/server.cch>
#include <ccc/std/log.cch>

// Handler: Mark @latency_sensitive to ensure predictable latency
@async @latency_sensitive Response!>(IoError) api_handler(Request* req, Arena* a) {
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

### J.1.1 Call-Edge Lowering (`@blocking` / `@noblock`)

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

### J.3 Slice and Buffer Ownership in Async Frames

**Rule:** Locals that contain slices, adopted buffers, or move-only values live on the frame and follow move semantics.

**Storage:**

```c
@async void handler(char[:] request_body, Arena* a) {
    char[:] trimmed = request_body.trim();  // View: points into request_body
    char[:] owned = request_body.clone(a);  // Copy: heap-allocated in arena
    
    // Frame layout:
    struct Frame {
        char[:] request_body;      // Move-only value; stored in frame
        Arena* a;                  // Pointer; stored in frame
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
       arena_reset(frame->arena);
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
