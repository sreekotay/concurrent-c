# Concurrent-C Specification

A C preprocessor that extends C syntax with first-class concurrency, desugaring to portable C.

**Full name:** Concurrent-C  
**Abbreviation:** CC  
**Type:** C extension (preprocessor + minimal runtime)  
**Draft Version:** 1.0-draft.10  
**Last Updated:** 2026-01-07

> **Status:** Complete, consolidated specification for CC-to-C translator implementation. **Spec Tests are normative.**

---

## Quick Reference: Keywords and Constructs

Concurrent-C adds ~40 new language constructs. Here's a quick overview:

### Core Keywords (7)

| Keyword | Purpose | Example |
|---------|---------|---------|
| `await` | Suspend and wait for async operation to complete | `data = await read_file(path);` |
| `try` | Unwrap Result type or propagate error up the call stack | `int x = try parse_int(s);` |
| `catch` | Handle errors from try blocks or catch expressions | `try { op(); } catch (Error e) { }` |
| `spawn` | Create a new task (only valid inside `@nursery`) | `spawn (worker_task());` |
| `defer` | Schedule cleanup code to run when scope exits (with `@defer` statement) | `@defer cleanup();` |
| `unsafe` | Disable safety checks in a block (e.g., for raw pointer casts) | `unsafe { ptr_cast(); }` |
| `comptime` | Mark code for compile-time evaluation (with `@comptime if`) | `@comptime if (DEBUG) { }` |

### Contextual Keywords (5)

These are only reserved in specific contexts, so they can be used as identifiers elsewhere:

| Keyword | Context | Purpose |
|---------|---------|---------|
| `async` | After `@` (in `@async fn`) | Mark function as asynchronous |
| `arena` | After `@` (in `@arena(a)`) | Define arena allocation scope |
| `lock` | After `@` (in `@lock (m) as var`) | Acquire mutex guard |
| `noblock` | After `@` (in `@noblock fn`) | Mark function as provably non-blocking |
| `closing` | In `@nursery closing(...)` | Auto-close channels when nursery exits |

### Special Block Forms (11)

| Form | Purpose | Example |
|------|---------|---------|
| `@async fn() { }` | Define asynchronous function | `@async void handler() { }` |
| `@latency_sensitive` | Mark as latency-critical (no dispatch coalescing) | `@async @latency_sensitive void handle() { }` |
| `@scoped type T` | Type tied to lexical scope (cannot escape) | `@scoped type Guard<T>;` |
| `@nursery { }` | Structured concurrency scope; spawn tasks here | `@nursery { spawn (t1()); spawn (t2()); }` |
| `@nursery closing(ch1, ch2)` | Nursery that closes channels on exit | `@nursery closing(ch) { }` |
| `@arena(a) { }` | Arena memory allocation scope | `@arena(a) { Vec<int> v = ...; }` |
| `@lock (m) as g { }` | Acquire mutex, bind guard to variable | `@lock (m) as guard { guard.data++; }` |
| `@match { case T x = ... }` | Multiplex on channel events | `@match { case int x = await ch: ... }` |
| `@defer stmt;` | Schedule statement to run on scope exit | `@defer file.close();` |
| `@for await (T x : ch) { }` | Async iteration (consume channel) | `@for await (int x : ch) { process(x); }` |
| `@comptime if (cond) { }` | Compile-time conditional | `@comptime if (FEATURE_X) { }` |

### Type Constructors (4)

| Constructor | Meaning | Example |
|-------------|---------|---------|
| `T!E` | Result type: success (T) or error (E) | `int!IoError read(path);` |
| `T?` | Optional type: value (T) or absent (null) | `int? find(arr, key);` |
| `char[:]` | Slice (variable-length view with provenance metadata) | `void process(char[:] data);` |
| `char[:!]` | Unique slice (move-only, exclusive ownership) | `char[:!] buf = recv(ch);` |

### Expression Forms (4)

| Form | Purpose | Example |
|------|---------|---------|
| `await expr` | Suspend until task completes, unwrap result | `int result = await fetch();` |
| `try expr` | Unwrap Result, propagate error if present | `File f = try open(path);` |
| `spawn (expr)` | Create new task (only valid in `@nursery`) | `spawn (handler(req));` |
| `result` conditional | Bind and unwrap in conditional (implicit try) | `if (try int x = parse(s)) { use(x); }` |

### Block Forms (2)

| Form | Purpose | Example |
|------|---------|---------|
| `with_deadline(d) { }` | Apply timeout/deadline to block | `with_deadline(seconds(5)) { await op(); }` |
| `try { } catch (E e) { }` | Multi-error handling in block | `try { op1(); op2(); } catch (SqlErr e) { }` |

### Library Functions (6)

These are normal functions in `concurrent_c.h` with `cc_` prefix to avoid naming conflicts:

| Function | Purpose | Example |
|----------|---------|---------|
| `cc_ok(T)` | Construct success Result | `return cc_ok(42);` |
| `cc_err(E)` | Construct error Result | `return cc_err(IoError.FileNotFound);` |
| `cc_move(x)` | Explicit move of move-only value | `ch.send_take(arr, cc_move(arr));` |
| `cc_cancel()` | Cancel current task or nursery | `if (timeout) cc_cancel();` |
| `cc_with_deadline(Duration)` | Create deadline scope (runtime function) | `cc_with_deadline(seconds(30)) { }` |
| `cc_is_cancelled()` | Check if current task is cancelled | `if (cc_is_cancelled()) return;` |

**C ABI naming:** All runtime/stdlib symbols use `CC*`/`cc_*` prefixes to avoid collisions with user code. Short aliases (`String`, `Arena`, etc.) are only available when the user opts in via `#include <std/prelude.cch>` and defining `CC_ENABLE_SHORT_NAMES` before inclusion. Default is prefixed-only.

---



**Concurrent-C is a C preprocessor for safe concurrent systems programming.** It extends C syntax with first-class async/await, message-passing channels, and structured concurrency, desugaring to standard portable C. Developers write CC code; the CC-to-C translator outputs clean, readable C suitable for any standards-compliant C compiler.

**Why a preprocessor?** C is portable, mature, and ubiquitous. Rather than replace it, CC enhances it: add concurrency syntax, generate efficient C, link with a minimal task scheduler and channel implementation. Users get modern concurrent programming without leaving the C ecosystem—mix CC and C freely in the same codebase.

**Core concepts:** Async functions (`@async`) desugar to state machines; channels (a `T[~... >]`/`T[~... <]` handle pair) desugar to queue types + two capability handles; arenas desugar to bump allocators with atomic ops; slices desugar to `{ ptr, len }` structs with provenance metadata. Everything has a clean C lowering; no mystery.

**Design philosophy:** Stay close to C (syntax, semantics, compilation model), make concurrency explicit (visible in function signatures and lowering), catch errors at compile time (via type system and provenance checking), and generate efficient C code (no intermediate VM or GC—just C + library).

---

## Mental Model: Core Abstractions

Before diving into syntax, here are the five core ideas that guide Concurrent-C:

### 1. Blocking Hierarchy

Understanding performance requires understanding what blocks:

```
PURE (Never blocks)
  └─ String, Vec, Map operations
     Arithmetic, local data structures
     Compiler optimization: Always inline

BOUNDED (Blocks for bounded time)
  └─ CPU work: parse, encode, compute
     Local synchronization (spin locks)
     Compiler decision: Can coalesce or separate
     User control: @latency_sensitive prevents coalescing

STALLING (Blocks indefinitely)
  └─ File I/O, network, database
     External synchronization (locks held elsewhere)
     Explicit classification: May fail with Busy
     User control: Deadline timeout, backpressure strategy

SATURATION (Capacity exhausted)
  └─ Blocking executor full
     Signal: IoError::Busy (fail-fast, observable)
     Strategy: Drop (lossy), Block (backpressure), Sample (sparse)
```

**Key insight:** Different operations need different handling. Pure ops are free. Bounded work is predictable. Stalling I/O needs explicit timeout and backpressure. Saturation needs a strategy.

### 2. Request-Scoped Resources

Three mechanisms prevent leaks and races in request handlers:

**Per-Request Arena:**
- Allocate all request data into one arena
- Reset in O(1) after request (pointer reset only)
- No manual `free()`; no fragmentation

**Deadline Per Request:**
- Timeout enforced cooperatively (cancellation)
- Prevents runaway requests
- Cancellation is observable (not silent)

**Scope-Bound Values (@scoped):**
- References cannot escape scope
- Compiler verifies (no unsafe casting)
- Safe to send across threads

**Result:** Complete request isolation, no allocations leak between requests, all resources tied to scope.

### 3. Concurrency Model

One primitive: **Nursery** (structured scope with spawned tasks)

```c
nursery {
    spawn (task1());
    spawn (task2());
    // Both tasks run concurrently
}
// Nursery waits for all tasks to complete (or cancel)
```

**Properties:**
- All spawned tasks complete before scope exits
- Cancellation propagates to all tasks
- Error from any task fails the nursery
- No deadlocks (guaranteed progress via cancellation)

**Channels:** Async-only or sync-only (determined at type, not context)

**Cancellation:** Cooperative but effective (propagates via @match, await, channel operations)

### 4. The @latency_sensitive Contract

Request handlers need predictable latency. Mark with `@latency_sensitive`:

```c
@async @latency_sensitive Response!IoError handler(Request* req, Arena* a) {
    // Compiler enforces: only @noblock and awaited @async calls allowed
}
```

**What this means:**
- No surprise coalescing (latency is observable)
- Latency violations caught at compile time
- Compiler: CPU work is inlined, I/O is separate dispatch

### 5. Backpressure Strategies

When I/O queues are full, three canonical strategies handle saturation. See **Appendix C: Standard Error Types & Backpressure** for detailed comparison table and use cases.

**Quick reference:**
- **Drop:** Discard oldest (lossy; access logs)
- **Block:** Block sender until space (backpressure; audit logs)
- **Sample:** Keep ~rate% deterministically (sparse; trace logs)

**Used in:** Channels (`T[~N >, Drop]` / `T[~N <, Drop]`), logging (`log_drop()`, `log_block()`, `log_sample()`)

---

## 1. Design Principles

1. **C superset with clean lowering** — A translation unit using no CC syntax is valid C. CC extends C with new syntax; the CC-to-C translator desugars all CC constructs to standard C code that any standards-compliant C compiler can compile. Lowering is transparent—developers can inspect generated C (`--emit-c` flag).

2. **Portable C output** — All CC features lower to C11 (or C99 + platform extensions). No intermediate representation, no VM. Generated code integrates seamlessly into existing C build systems (Make, CMake, Meson, etc.).

3. **Minimal runtime** — Only `@async` (task scheduler, channel types) requires a linked runtime. Arenas, slices, optionals, results—all lower to pure C structures. Users can replace the async runtime with custom implementations if needed.

4. **Explicit by default** — Effects visible in function signatures. Allocations pass `Arena*` explicitly. Errors return `T!E` (Result types). Async code is marked `@async`. Ownership transfer is explicit (via `move()` or function parameters).

5. **Scoped safety** — Arenas + tracked slices catch common lifetime errors; raw pointers remain unsafe. Compile-time provenance tracking (via slice metadata) enables `send_take` zero-copy transfers without runtime checks.

**Rule:** Types with runtime-managed invariants (channel handles `T[~... >]` / `T[~... <]`, `Task<T>`, `Mutex<T>`, `Atomic<T>`) are always initialized on declaration. Plain C types follow C initialization rules.

**Exception:** `T!E` (Result types) do not require immediate initialization and instead follow **definite assignment** rules. A `T!E` variable may be declared without an initializer if the compiler can prove it is assigned on all control-flow paths before any use or before scope exit.

**Rule (Concurrent-C type initialization):**
- `T?` defaults to `null` if no initializer is provided.
- `T!E` uses **definite assignment**: the compiler verifies that a `T!E` variable is assigned on all paths before any use or scope exit.
- Channels, tasks, `Mutex<T>`, and `Atomic<T>` are always initialized on declaration.
- `Map<K,V>` requires an explicit initializer (needs arena).

**Rule (T!E and destructors):** Definite assignment for `T!E` is required because the destructor must know which arm (`T` or `E`) to drop. An uninitialized `T!E` at scope exit would have undefined destructor behavior—the compiler cannot determine whether to drop `value` or `error`. This is why definite assignment analysis must cover all paths to scope exit, not just paths to explicit reads.

```c
int!Error x;              // OK if definitely assigned before use
if (cond) {
    x = cc_ok(42);
} else {
    x = cc_err(Error.Oops);
}
use(x);                   // OK: assigned on all paths

int!Error y;              // ERROR: not definitely assigned
if (cond) {
    y = cc_ok(1);
}
use(y);                   // ERROR: y may be uninitialized
// also ERROR at scope exit: destructor doesn't know which arm to drop
```

---

### 1.1 Value Categories & Moves

Concurrent-C distinguishes **copyable** and **move-only** values:

**Copyable types:**
- All C primitive types
- Structs containing only copyable fields
- `T?` and `T!E` where `T` and `E` are copyable
- `Mutex<T>`, `Atomic<T>` (handle types)

**Move-only types:**
- `Task<T>` (represents unique computation)
- `Map<K,V>` (arena-backed, unique ownership)
- `T?` where `T` is move-only
- `T!E` where `T` or `E` is move-only

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
char[:]? opt = await ch.recv();            // optional unique slice
if (!opt) return;                          // channel closed
char[:] x = *opt;                          // unwrap to get unique slice
char[:] y = x;                             // ERROR: cannot copy
char[:] y = cc_move(x);                    // OK: x is invalid, y owns buffer
```

**Move-only optionals:**

When an optional contains a move-only type (`T?` where `T` is move-only), moving the optional sets it to `null`:

```c
char[:]? x = await ch.recv();  // x is an optional unique slice
char[:]? y = cc_move(x);       // OK: move the optional
assert(!x);                    // OK: x is null now
x = await ch.recv();           // OK: can reassign the optional
```

**Move semantics for different types:**

**Results (`T!E`):** Movable as whole values. To extract inner value, use `try` or pattern matching.
```c
int!Error r = cc_ok(42);
int!Error s = r;        // copies (int and Error are copyable)
int v = try r;          // extracts value, propagates error
```

**Bare unique slices:** Moving invalidates source; subsequent use is a compile-time error.
```c
unsafe {
    char[:] buf = adopt(ptr, len, deleter);
    char[:] copy = cc_move(buf);
    use(buf);  // ERROR: use after move
}
```

**Optional with move-only value:** Moving sets optional to `null`; optional variable remains valid.
```c
char[:]? opt = await ch.recv();
char[:]? copy = cc_move(opt);  // opt becomes null
if (opt) { ... }               // OK: null is valid state
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

Not an error for `T?` after move (it's null, a valid state).

**Rule (function parameters):** Move-only values move by-value; pass as pointer to retain ownership:
```c
void take_ownership(char[:] buf);     // moves buf
void borrow(char[:]* buf);            // borrows, caller retains
```

---

### 1.2 Closure Captures and Thread Safety

When a closure is captured for use in another thread or task, the captured values are copied into the closure. This creates constraints on what can be captured.

**Rule (channel send vs closure capture):** Channel `send()` deep-copies the value into channel-internal storage—**any value can be sent**. The sendability rules in this section apply only to **closure captures** for threads and tasks, not to channel operations.

**Closure capture rules:**

```c
char buf[100];
char[:] stack_slice = buf[:];

// Channel send: OK (deep-copies bytes into channel buffer)
await ch.send(stack_slice);

// Thread closure: ERROR (closure copies slice struct, ptr points to dead stack)
spawn_thread(() => {
    use(stack_slice);  // BAD: stack_slice.ptr points to caller's stack frame
});
```

**Rule (capture eligibility):** A value can be captured in a thread/task closure iff:
1. It does not contain pointers to stack memory (which dies when the spawning function returns)
2. It does not contain pointers to arena memory that may be freed before the thread/task completes
3. It is not a scope-bound handle (`LockGuard`, `AsyncGuard`, `ThreadGroup`, `Scope`)

**Capture eligibility table:**

| Type | Capturable? | Notes |
|------|-------------|-------|
| Primitives | Yes | Value types |
| Structs | Yes iff all fields capturable | Structural |
| `T?`, `T!E` | Yes iff inner types capturable | Structural |
| Arena slices | **Depends** | Only if arena provably outlives thread/task |
| Static slices | Yes | Lives forever |
| Stack slices | **No** | Frame dies on return |
| Unique slices (`recv()`) | Yes | Owned, no external pointer |
| `Mutex<T>` | Yes | Handle to synchronized state |
| `AsyncMutex<T>` | Yes | Handle to synchronized state |
| `Atomic<T>` | Yes | Lock-free primitive |
| `Map<K,V>` | Yes iff contents capturable | Move-only; moved into closure |
| `Task<T>` | Yes | Can be awaited from any thread |
| Channels | Yes | Thread-safe handles |
| `LockGuard<T>` | **No** | Scope-bound |
| `AsyncGuard<T>` | **No** | Scope-bound |
| `ThreadGroup` | **No** | Must be used in creating thread |
| `Scope` | **No** | Stack-bound capability |
| Raw pointers | Yes | But safety is caller's responsibility |

**Rule (stack slice escape):** Stack slices cannot be captured in any closure that may outlive the current stack frame. This is enforced at compile time.

**Rule (enforcement and UB):** Stack slice escape is always a compile-time error when the escape is provable. For cases where escape cannot be determined at compile time, the behavior is undefined in release builds and trapped (runtime error) in debug builds.

**Rule (arena slice capture):** Arena slices can be captured if the compiler can prove the arena outlives the thread/task. In practice, this means the arena must be declared in an enclosing scope that joins the thread/task before the arena is freed.

```c
void ok_pattern() {
    Arena a = arena(kilobytes(64));
    char[:] s = arena_alloc<char>(&a, 100);
    
    ThreadGroup g = thread_group();
    g.spawn(() => {
        use(s);  // OK: g.join() happens before arena freed
    });
    g.join();
}  // arena freed here, after thread joined

void bad_pattern() {
    ThreadGroup g = thread_group();
    {
        Arena a = arena(kilobytes(64));
        char[:] s = arena_alloc<char>(&a, 100);
        g.spawn(() => {
            use(s);  // ERROR: arena may be freed before thread runs
        });
    }  // arena freed here
    g.join();  // thread may access freed memory
}
```

---

### 1.3 Lexing & Parsing

Concurrent-C extends C syntax with new operators and keywords in specific contexts:

**Type-context operators (not valid in expression context):**

| Syntax | Meaning | Notes |
|--------|---------|-------|
| `T?` | Optional type | Postfix, not ternary `?:` |
| `T!E` | Result type | Postfix, binds error type `E` |
| `T[:]` | Slice type | Distinct from C array `T[]` |
| `T[~... >]` / `T[~... <]` | Channel handle type | `~` is not bitwise-not here |
| `T[~n N:M >]` / `T[~n N:M <]` | Channel with topology | `N:M` is topology, not label |

## Concurrent-C Language Constructs: Complete Reference

Concurrent-C extends C with ~40 new constructs for async/await, structured concurrency, and error handling. This table shows all of them.

| Category | Construct | Purpose | Example |
|----------|-----------|---------|---------|
| **Core Keywords** | `await` | Suspend until async operation completes | `char[:] data = await read_file(path);` |
| | `try` | Unwrap Result or propagate error | `int x = try parse_int(s);` |
| | `catch` | Handle errors from try blocks | `try { op(); } catch (Error e) { }` |
| | `spawn` | Create new task in nursery | `spawn (worker());` |
| | `defer` | Cleanup on scope exit (with `@defer`) | `@defer free(ptr);` |
| | `unsafe` | Disable safety checks in block | `unsafe { *ptr = value; }` |
| | `comptime` | Compile-time conditional (with `@comptime`) | `@comptime if (DEBUG) { }` |
| **@ Statements** | `@async` | Mark function as asynchronous | `@async void handler() { }` |
| | `@latency_sensitive` | Latency-critical (no coalescing) | `@async @latency_sensitive Response handle() { }` |
| | `@scoped` | Type tied to lexical scope | `@scoped type Guard<T>;` |
| | `@nursery` | Structured concurrency block | `@nursery { spawn (task()); }` |
| | `@nursery closing(...)` | Nursery with auto-channel-close | `@nursery closing(ch) { }` |
| | `@arena(a)` | Arena allocation scope | `@arena(a) { Vec<int> v = ...; }` |
| | `@lock (m) as var` | Mutex guard acquisition | `@lock (m) as g { g.value++; }` |
| | `@match` | Channel event dispatch | `@match { case T x = await ch: ... }` |
| | `@defer` | Deferred cleanup statement | `@defer cleanup();` |
| | `@for await (T x : ch)` | Async iteration loop | `@for await (int x : ch) { ... }` |
| | `@comptime if` | Compile-time conditional | `@comptime if (feature) { }` |
| **Type Constructors** | `T!E` | Result type (success or error) | `int!IoError read(path);` |
| | `T?` | Optional type (value or null) | `int? find(arr, key);` |
| | `char[:]` | Slice (variable-length view) | `void process(char[:] data);` |
| | `char[:!]` | Unique slice (move-only) | `char[:!] buf = recv(ch);` |
| **Expression Forms** | `await expr` | Suspend and unwrap task result | `int result = await fetch();` |
| | `try expr` | Unwrap Result, propagate error | `File f = try open(path);` |
| | `spawn (expr)` | Create task (must be in `@nursery`) | `spawn (handler(request));` |
| **Block Forms** | `cc_with_deadline(d) { }` | Apply timeout/deadline to block | `cc_with_deadline(seconds(5)) { await op(); }` |
| | `try { } catch (E e) { }` | Multi-error handling block | `try { op1(); op2(); } catch (Err e) { }` |
| **Library Functions** | `cc_ok(T)` | Construct success Result | `return cc_ok(42);` |
| | `cc_err(E)` | Construct error Result | `return cc_err(IoError.FileNotFound);` |
| | `cc_move(x)` | Explicit move of move-only value | `ch.send_take(arr, cc_move(arr));` |
| | `cc_cancel()` | Cancel current task/nursery | `if (timeout) cc_cancel();` |
| | `cc_with_deadline(Duration)` | Create deadline scope | `cc_with_deadline(seconds(30)) { }` |
| | `cc_is_cancelled()` | Check if task cancelled | `if (cc_is_cancelled()) return;` |

---

**Rules:**

- **Reserved words:** Only in CC mode. Pure-C files can use these as identifiers.
- **@ prefix:** Marks CC-specific block forms and statements. Helps distinguish from C control flow.
- **Contextual keywords:** `async`, `arena`, `lock`, `noblock`, `closing` are recognized only in specific contexts (after `@`, in nursery, etc.), not reserved globally. Safer for identifier reuse.
- **Library functions:** `ok`, `err`, `move`, `cancel`, `with_deadline`, `is_cancelled` are normal functions in `concurrent_c.h`. No special parsing needed.

**Rule:** In type contexts, `?`, `!`, `[:]`, `[~ ...]` are type constructors. In expression contexts, they retain C semantics (ternary, logical-not, etc.) or are syntax errors if ambiguous.

**Rule:** `@for await (T x : expr)` is parsed as a single statement form, not `for` followed by `await`.

---

## Style Guide

This section documents recommended style for Concurrent-C code.

### Type Annotation Spacing

**Rule:** Type constructors are written **without spaces**. This applies to all compound type syntax.

| Type | Correct | Incorrect |
|------|---------|-----------|
| Optional | `T?` | `T ?` |
| Result | `T!E` | `T ! E` or `T! E` |
| Slice | `char[:]` | `char [:]` or `char[ : ]` |
| Unique slice | `char[:!]` | `char[: !]` |
| Generic type | `Vec<int>` | `Vec < int >` |
| Nested generic | `Map<K, V>` | `Map < K, V >` (spaces ok inside `<>` for args) |
| Function return | `fn() -> T!E` | `fn ( ) -> T ! E` |

**Examples:**

```c
// Correct
int!IoError read_int(char[:] data) {
    char[:] trimmed = data.trim();
    return cc_ok(parse_int(trimmed));
}

Vec<int> numbers = vec_new<int>(&arena);
Map<char[:], int> registry = map_new<char[:], int>(&arena);

// Incorrect (visual noise, harder to parse)
int ! IoError read_int (char [ : ] data) {
    // ...
}
```

### Method Call Style

Both UFCS and free function forms are valid and equivalent. Choose based on context:

**Use UFCS for method chains:**
```c
char[:] result = input
    .trim()
    .lower(&arena)
    .slice(0, 10);
```

**Use free functions for composition:**
```c
char[:] result = slice(lower(trim(input), &arena), 0, 10);
```

**Mix as needed:**
```c
// Both valid; choose based on readability
x.method(y)       // UFCS style
method(x, y)      // Free function style
```

### Attribute Placement

All attributes use the `@` prefix and follow consistent spacing:

| Construct | Style | Notes |
|-----------|-------|-------|
| `@async` | `@async fn() { }` | No space before function name |
| `@latency_sensitive` | `@async @latency_sensitive fn() { }` | Stack before `@async` |
| `@noblock` | `@noblock fn() { }` | Marks synchronous non-blocking function |
| `@scoped` | `@scoped type Guard<T>;` | On type declarations |
| `@nursery` | `@nursery { ... }` | No parameters, always a block |
| `@arena` | `@arena(a) { ... }` | No space before parentheses |
| `@lock` | `@lock (m) as g { ... }` | Space before parentheses (statement form) |
| `@for await` | `@for await (T x : ch) { }` | Unified statement, no confusion with `for` + `await` |
| `@defer` | `@defer cleanup();` | Single statement after |
| `@match` | `@match { ... }` | Channel multiplexing |

---

## 2. Core Types

This section defines the fundamental value-level building blocks:

- **§2.1 Optionals (`T?`)** — presence or absence
- **§2.2 Results (`T!E`)** — success or failure
- **§2.3 Type Precedence** — how type modifiers bind
- **§2.4 Arrays and Slices** — fixed arrays and views
- **§2.5 Slice ABI** — provenance metadata layout

---

### 2.1 Optionals (`T?`)

`T?` represents **presence or absence** of a value.

* Either `null` or contains a `T`.
* Works for all `T`, not just pointers.

Operations:

```c
if (x) { ... }     // presence check
T v = *x;          // unwrap (only when proven non-null)
```

**Lowering:**

```c
// T? lowers to a tagged union:
struct Optional_T {
    bool has;
    union { T value; } u;
};
```

Surface syntax `*x` maps to `x.u.value` in lowered code.

**Rule (active field):** The `u.value` member is only valid when `has == true`. Reading `u.value` when `has == false` is undefined behavior.

**Rule (operators):**
- `if (x)` is sugar for `if (x.has)` (presence test)
- `*x` ("unwrap") yields `x.u.value` and is only legal when the compiler can prove `x.has == true` (otherwise compile-time error)

**Rule (unary `*` overloading):** Unary `*` is overloaded by operand type:
- If operand type is `U*` (pointer): `*` is pointer dereference (C behavior)
- If operand type is `U?` (optional): `*` is optional unwrap (Concurrent-C behavior)
- Otherwise: compile-time error

**Rule (move-only unwrap):** When `T` is a move-only type (e.g., unique slice), `*x` performs a **move** out of the optional. After the move:
- `x.has` becomes `false` (x is now null)
- `x` may be used normally as an optional (presence tests, assignment, re-assignment)
- Any aliases or borrows derived from the moved value are invalidated

**Rule (drop for T?):** On scope exit, if `x.has == true` and `T` has destructor semantics (e.g., unique slice), the destructor for `x.u.value` runs. If the value was moved out (making `x` null), no destructor runs.

**Rule (provability):** The compiler uses flow-sensitive analysis to determine presence:
- After `if (x) { ... }`, `x` is proven present inside the branch
- After `x = some_value`, `x` is proven present
- After `*x` (successful unwrap) on move-only `T`, `x` is proven null
- Functions returning `T?` are assumed to potentially return null

`T?` is **not** error handling. Use `T!E` for errors.

---

### 2.2 Results (`T!E`)

`T!E` represents **success or failure** with an explicit error value.

* Either `cc_ok(T)` or `cc_err(E)`.
* Shorthand for `Result<T, E>`.

```c
int!IoError x = cc_ok(42);
int!IoError y = cc_err(IoError.FileNotFound);

if (x.ok) use(x.value);
else handle(x.error);
```

**Lowering:**

```c
// T!E lowers to a tagged union:
struct Result_T_E {
    bool ok;
    union { T value; E error; } u;
};
```

Surface syntax `x.value` maps to `x.u.value`; `x.error` maps to `x.u.error`.

**Rule (active field):** Only the active union member is initialized and valid, as determined by `ok`. When `ok == true`, `u.value` is active; when `ok == false`, `u.error` is active. Reading the inactive member is undefined behavior (and is a compile-time error where statically provable).

**Rule (drop for T!E):** On scope exit, the destructor for the **active arm** runs if it has destructor semantics:
- If `ok == true` and `T` has a destructor, drop `u.value`
- If `ok == false` and `E` has a destructor, drop `u.error`
- If the value was moved out via `try` or pattern match, no destructor runs for that arm

---

### 2.3 Type Precedence

Type modifiers bind with the following precedence (tightest first):

1. `?` (optional)
2. `!E` (result)
3. `[n]` `[:]` `[~n]` (array / slice / channel)
4. `*` (pointer)

**Examples:**

| Syntax | Parses as | Meaning |
|--------|-----------|---------|
| `int?` | `(int)?` | optional int |
| `int!E` | `(int)!E` | result of int or E |
| `int?!E` | `((int)?)!E` | result whose ok-value is optional |
| `int!E?` | `((int)!E)?` | optional result (e.g., recv from error channel) |
| `int!E[~]` | `((int)!E)[~]` | channel of results |
| `int!E[~]*` | `(((int)!E)[~])*` | pointer to (channel of results) |
| `int[:]*` | `((int)[:])*` | pointer to slice |

**Note:** For a channel `ch : (T!E)[~]`, `ch.recv()` returns `(T!E)?` (i.e., `T!E?`), where `null` means "closed+drained" and `err(e)` is an application-level error value.

---

### 2.4 Arrays and Slices

| Type    | Meaning     | Storage                | Size                |
| ------- | ----------- | ---------------------- | ------------------- |
| `T[n]`  | fixed array | inline                 | compile-time        |
| `T[:]`  | slice       | ptr + len + provenance | runtime             |
| `T[:n]` | slice       | ptr + len + provenance | compile-time length |

Slices are *views*; they do not own memory.

**Rule (T[:n] semantics):** `T[:n]` is a slice type with a compile-time known length `n`. It has the same ABI as `T[:]` (32 bytes), but the type system statically guarantees `len == n`. This enables bounds-checked indexing to elide runtime checks. `T[:n]` implicitly converts to `T[:]` (information is erased, not lost).

**Implicit conversions:**

* `T[n]` → `T[:]`
* `T[n]` → `T[:n]`
* `T[:n]` → `T[:]`

**Explicit conversions:**

* `slice.ptr` yields `T*`

---

### 2.5 Slice ABI (32 bytes)

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

| Condition | Meaning |
|-----------|---------|
| `id == 0` | Static or untracked slice (string literals, unsafe slices). Not unique, not transferable, no provenance tracking. |
| `id != 0 && is_unique == 0` | View slice (arena, stack). Copyable. Not transferable. |
| `id != 0 && is_unique == 1 && is_transferable == 1` | Unique, transferable slice (from `recv()`). |
| `id != 0 && is_unique == 1 && is_transferable == 0` | Unique, non-transferable slice (from `adopt()`). |

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

### 2.6 Slice Ownership: Views vs Unique

Slices are either **views** (copyable, no destructor) or **unique** (move-only, has destructor).

| Category | Source | Copyable? | Transferable? | Has destructor? |
|----------|--------|-----------|---------------|-----------------|
| **View** | Arena, stack, static | Yes | No | No |
| **Unique (recv)** | `recv()` | No | **Yes** | Yes |
| **Unique (adopt)** | `adopt()` | No | No | Yes |

*Stack slices cannot escape their stack frame (compile-time enforced).

**View slices:**
- Created by arena allocation, local arrays, or string literals
- Copyable — multiple variables can reference the same allocation
- Arena/static views can cross thread boundaries; stack views cannot escape their frame
- No destructor — the arena (or program lifetime, or stack frame) manages the memory

**Unique slices:**
- Created by `recv()` (channel receive) or `adopt()` (FFI adoption)
- Move-only — copying is a compile-time error
- Has destructor that runs at scope exit (or is suppressed if moved)
- Only `recv()` slices can use `send_take` for zero-copy transfer (see §7.3)

**Rule (unique assignment ban):** Any assignment, copy, or parameter passing that would duplicate a slice value with `id.is_unique=1` is illegal unless it occurs in a move context (`move()`, `return`, `send_take`).

**Borrowed views:**

Subslicing a unique slice produces a **borrowed view**. At runtime, borrows are still `T[:]` values, but the compiler tracks an implicit owner reference.

| Property | Borrowed view |
|----------|---------------|
| Runtime type | `T[:]` (same as any slice) |
| `id.is_unique` | 0 (view, not unique) |
| Copyable? | Yes (within borrow region) |
| Sendable? | Same as owner |
| Lifetime | Must not outlive owner |

**Borrow rules:**
- Borrows are created by subslicing: `(*x)[a..b]`, `(*x)[..n]`, etc.
- Borrows can be freely copied within their valid region
- Borrows are invalidated when the owner is:
  - Moved (`move(x)`, `send_take(*x)`)
  - Destroyed (scope exit)
  - Reset (for arena-backed owners, `arena_reset`)
- Using an invalidated borrow is a compile-time error (when detectable) or undefined behavior (when not statically detectable, debug builds trap)

```c
char[:]? x = await ch.recv();  // x owns unique slice
char[:] borrow = (*x)[0..10]; // borrow is a view into x
use(borrow);                   // OK
char[:]? y = move(x);          // x moved to y
use(borrow);                   // ERROR: borrow invalidated by move
```

**Rule (unique destruction):**
- Unique slices have an implicit destructor at scope exit
- Destructor runs on all exits: return, early return, `try` propagation
- Destructor is suppressed if ownership is moved (`return`, `send_take`, `move()`)
- For `adopt()` slices: destructor calls the registered deleter
- For `recv()` slices: destructor frees the channel's buffer

**Slice Derivation Rules (Normative):**

When a slice value is derived from another slice (subslicing):

| Field | Behavior |
|-------|----------|
| **Allocation ID (bits 0–60)** | Preserved exactly |
| **`is_unique` (bit 63)** | Cleared (0) — derived slices are borrowed views, only the original owning slice remains unique |
| **`is_transferable` (bit 61)** | Preserved exactly — but borrowed views are never transferable because `is_unique == 0` |
| **`is_subslice` (bit 62)** | Set to 1 iff the derived slice does not cover the entire allocation |
| **`alen`** | Preserved as the allocation length for all derived slices |

**`is_subslice` computation:**
- `s[..]` where `s.len == s.alen` → `is_subslice = 0`
- `s[0..s.len]` → `is_subslice = 0`
- `s[a..b]` where `a != 0` or `b != s.alen` → `is_subslice = 1`

This guarantees that full-range subslices remain eligible for transfer (if the source was transferable), while partial subslices are rejected — with no runtime table required.

**Rule (ptr invariant):** For tracked allocations (unique slices from `recv()` or `adopt()`), the `ptr` field of the owning unique slice is always the allocation base pointer. Subslicing adjusts `ptr` (and `len`) but preserves `alen`. This invariant, combined with `is_subslice`, ensures `send_take` eligibility is decidable without runtime lookup: `is_subslice == 0` implies `ptr` equals the allocation base and `len == alen`.

**Slice capture rules:** Stack slices cannot be captured in thread/task closures (compile-time enforced). Arena slices can be captured if the arena provably outlives the thread/task. Unique slices (from `recv()`) can always be captured. See §1.2 for complete capture rules.

---

## 3. Type Categories and Scope-Bound Values

This section defines categories of types that have special restrictions:

- **§5.1 Scope-bound values (`@scoped`)** — types tied to lexical scope
- **§5.2 Suspension points** — where scope-bound values cannot be held
- **§3.3 Compiler enforcement** — how violations are detected

The central rule: **No scope-bound value may be held across a suspension point.**

---

### 3.1 Scope-Bound Types (`@scoped`)

A type marked `@scoped` is **tied to a lexical scope** and cannot outlive that scope. Most importantly, a scope-bound value cannot be held across a suspension point (await, @match, @async call).

**Types that are scope-bound:**

```c
@scoped type LockGuard<T>;           // Mutex guard
@scoped type AsyncGuard<T>;          // Async mutex guard
@scoped type BorrowRegion<T>;        // Borrow region (active borrow)
@scoped type DeferHandle;            // Defer cleanup handle
@scoped type FileHandle;             // File handle (future)
@scoped type Transaction;            // Database transaction (future)
```

**Characteristics:**

- Cannot be returned from a function (unless function is `@noawait`)
- Cannot be stored in a non-scoped struct field
- Cannot be passed across `await` or `@async` function call boundaries
- Must be released before any suspension point
- Compiler enforces these restrictions at compile time

**Example:**

```c
@async void!Error worker(Mutex<int>* m) {
    // ✅ CORRECT: guard doesn't cross suspension point
    {
        @lock (m) as guard {  // guard is @scoped
            guard++;
        }  // guard released here
    }
    
    await io_operation();  // ✅ OK: no @scoped values held
}

@async void!Error bad(Mutex<int>* m) {
    @lock (m) as guard {
        guard++;
        await io_operation();  // ❌ ERROR: @scoped value guard held across await
    }
}
```

**Rule (scope-bound cannot escape):**

```c
@scoped type LockGuard<T> { /* ... */ }

LockGuard<int> bad_return(Mutex<int>* m) {
    LockGuard<int> g = m.lock_guard();
    return g;  // ❌ ERROR: cannot return @scoped value
}

void take_guard(LockGuard<int> g) { }

void bad_pass(Mutex<int>* m) {
    @lock (m) as g {
        take_guard(g);  // ❌ ERROR: cannot pass @scoped across function boundary
    }
}

struct Container {
    guard: LockGuard<int>  // ❌ ERROR: cannot store @scoped in struct
}
```

**Rule (Suspension point releases @scoped):**

If a function has `@scoped` values in scope at a suspension point, that's a compile error. The typical fix is to release the value before suspending:

```c
@async void!Error correct(Mutex<int>* m) {
    let val = {
        @lock (m) as g {
            g + 1
        }  // guard released here
    };
    
    await operation();  // ✅ OK
    return cc_ok(val);
}
```

---

### 3.2 Suspension Points

#### 3.2.1 Definition

A **suspension point** is any program point at which execution of the current task may suspend and later resume. At suspension points, **no scope-bound (`@scoped`) values may be held**.

**Suspension points:**

- `await` expression (any await)
- `@match` statement
- Call to `@async` function
- Async iteration (`@for await`) (contains an `await` per iteration)
- Call to cancellation-aware channel operation (`recv_cancellable`, `send_cancellable`)
- Call to `block_on()` (rare, explicit blocking)

**Non-suspension points** (safe to hold scope-bound values):

- Call to sync function (non-`@async`)
- Local variable creation / destruction
- Arithmetic, logic, control flow
- Non-blocking operations (`try_send`, `try_recv`, `close`)

#### 3.2.2 Cancellation Observation at Suspension Points (Normative)

When a suspension point is executed inside an active `with_deadline(...)` scope, the suspension point MUST be treated as **cancellation-aware**.

**Rule:**

- Before suspending, and again immediately after resuming, the runtime MUST check whether cancellation has been requested for the current task.
- If cancellation has been requested, the suspension point MUST complete by returning `err(Cancelled)`.

**Requirements:**

- The enclosing function’s error type MUST be able to represent `Cancelled`.
- If the error type cannot represent `Cancelled`, the program is ill-formed and compilation MUST fail.

**Notes:**

- Cancellation remains cooperative. No suspension point is required to interrupt an in-progress blocking operation.
- Cancellation is only observed at suspension points; purely CPU-bound code is unaffected unless it explicitly checks cancellation (e.g., `cc_is_cancelled()` / `is_cancelled()`).

For complete deadline semantics, scoping rules, and runtime behavior, see **§ 7.5 (Cancellation & Deadline)**. This section defines the compiler-level guarantee; § 7.5 specifies the full runtime semantics.

**Example:**

```c
@async void!Error pattern(Mutex<int>* m) {
    // ✅ CORRECT: use guard, then release, then suspend
    let val = {
        @lock (m) as g {
            g + 1
        }
    };
    
    await io();  // ✅ No @scoped values held
    return cc_ok(val);
}

@async void!Error antipattern(Mutex<int>* m) {
    @lock (m) as g {
        let val = g + 1;
        
        // ❌ ERROR: guard held across await
        // This crosses the suspension point with @scoped value
        // await io();
        
        return cc_ok(val);  // OK: return is not a suspension point
    }
}
```

**@scoped Placement Rules (Normative):**

`@scoped` can appear in the following positions:

1. **Type declarations (primary use):**
```c
@scoped type LockGuard<T>;
@scoped struct Guard {
    handle: int;
};
@scoped interface Closeable {
    void close();
};
```

2. **Function parameters:**
```c
fn process(@scoped Mutex* lock) {
    // Function takes ownership of mutex guard; must release before returning
}

fn handle(@scoped RequestContext ctx) {
    // Context is tied to this function's scope
}
```

`@scoped` on a parameter means the value must be released (or the function must be `@noawait`) before any suspension point.

**Invalid positions (compile error):**

- **Return types:** Cannot return @scoped values
```c
@scoped Mutex* get_mutex() { }  // ❌ ERROR: cannot return @scoped
```

- **Variable declarations:** Variables are already scope-bound to their block; `@scoped` is implicit
```c
@scoped Mutex m;  // ⚠️ Redundant; already scoped to block. May be warning/error.
```

- **Struct fields (in non-scoped struct):**
```c
struct Container {
    @scoped guard: Mutex;  // ❌ ERROR: cannot store @scoped in regular struct
}
```

- **Function pointers/closures:**
```c
@scoped fn ptr = &my_func;  // ❌ ERROR: meaningless on function types
```

**Interaction with @async:** A function parameter marked `@scoped` cannot be held across a suspension point. The compiler enforces that all @scoped values are released before any `await`, `@async` call, or other suspension point.

---



All suspension points (both `await` and `@async` function calls) are implicitly cancellation-aware inside a deadline scope:

```c
@async void handler(Duration timeout) {
    with_deadline(timeout) {
        await fetch_data();    // ✅ Suspension point: cancellation-aware
        result = compute();    // Non-suspension: safe, CPU work only
        spawn (worker());        // ✅ If worker is @async: suspension point, cancellation-aware
        await store();         // ✅ Suspension point: cancellation-aware
    }
}
```

---

### 3.3 Compiler Enforcement

The compiler checks scope-bound restrictions in several places:

**At variable scope exit:**
```c
{
    @lock (m) as g {
        g++;
    }  // Guard must be released here
}
```

**At suspension points:**
```c
@async void handler() {
    LockGuard<int> g = ...;
    await io();  // ❌ ERROR: @scoped value g held across suspension
}
```

**At function boundaries:**
```c
void takes_guard(LockGuard<int> g) { }

@async void caller(Mutex<int>* m) {
    @lock (m) as g {
        takes_guard(g);  // ❌ ERROR: cannot pass @scoped across function boundary
    }
}
```

**At struct field assignment:**
```c
struct Holder {
    guard: LockGuard<int>  // ❌ ERROR: cannot store @scoped in struct
}
```

**Compiler error message example:**

```
error: scope-bound value `guard` held across suspension point
  --> file.c:10:5
   |
9  |     @lock (m) as guard {
   |                 ----- guard declared here
10 |         await io_operation();
   |         ^^^^^ suspension point: cannot hold @scoped value
   |
help: release @scoped value before suspension
   |
   | }
   | await io_operation();
```

---

## 4. Arenas

This section defines the allocation model and lifetime boundaries:

- **Arena API** — creation, allocation, lifecycle
- **§5.1 `defer`** — scoped cleanup
- **§5.2 Arena blocks** — structured arena lifetime

---

Arenas own memory; slices are views into arena-owned storage.

```c
// Creation
Arena arena(size_t size);

// Lifecycle
void  arena_free(Arena* a);            // free all memory
void  arena_reset(Arena* a);           // reclaim memory, invalidate slices

// Checkpoints
typedef struct ArenaCheckpoint ArenaCheckpoint;
ArenaCheckpoint arena_checkpoint(Arena* a);
void  arena_restore(ArenaCheckpoint checkpoint);

// Allocation
void* arena_alloc(Arena* a, size_t nbytes);       // raw bytes (no provenance)
T[:]  arena_alloc<T>(Arena* a, size_t nelems);    // tracked, nelems elements
T[:1] arena_alloc1<T>(Arena* a);                  // tracked, 1 element (sugar)

// Size helpers
size_t kilobytes(size_t n);            // n * 1024
size_t megabytes(size_t n);            // n * 1024 * 1024
size_t gigabytes(size_t n);            // n * 1024 * 1024 * 1024
```

**Rule:** All arenas use atomic operations internally and are safe to share across threads. Arena-allocated slices can be sent through channels or captured in thread closures.

**Rule (arena lifetime obligation):** `arena_reset` and `arena_free` must not be called while any slice derived from that arena may still be used on any thread. This is a programmer obligation. Debug builds may detect some violations; in release builds, violating this rule is undefined behavior.

**Arena checkpoints (normative):**

- An arena checkpoint captures the current allocation state of an arena and allows later restoration.
- Restoring a checkpoint releases all allocations performed after the checkpoint.
- Checkpoints MUST NOT invalidate allocations made prior to the checkpoint.
- Arena checkpoints do not alter arena ownership or lifetime rules.

```c
Arena a = arena(megabytes(1));
char[:] s = arena_alloc<char>(&a, 100);

spawn_thread(() => {
    use(s);  // OK: a still alive
});

arena_free(&a);  // BUG: thread may still be using s
```

**Design Rationale:**

**Thread-safe allocation:** Arenas use atomic operations (lock-free compare-and-swap or spin-locks) to make allocation thread-safe. A synchronized bump allocator adds ~10-20ns overhead per allocation vs unsynchronized, but is still ~4x faster than `malloc`. The benefit of "all arena slices are safely shareable across threads" outweighs this minor cost.

**Manual lifetime management:** The caller, not the runtime, is responsible for coordinating `arena_free`/`arena_reset` with thread lifecycle. This avoids the overhead of reference counting or cycle detection. It also gives users explicit control: a thread can hold slices from a long-lived arena while other threads allocate and reset scratch arenas independently.

**Non-goal:** Arenas do **not** provide automatic deallocation or generational lifetimes. Users must ensure all threads have finished using an arena before resetting or freeing it. Violations are not caught automatically in release builds.

**Note (future):** For allocation-heavy tight loops where profiling shows arena overhead is significant, a future version may add a `Bump` type—an unsynchronized bump allocator over a pre-allocated chunk. This would return raw pointers (no provenance tracking) and not be sendable, preserving the "all arena slices are sendable" invariant.

---

### 3.1 `@defer`

`@defer stmt;` schedules `stmt` to run on scope exit.

* Runs on all returns, including `try` propagation.
* LIFO order.
* No exceptions or unwinding.

```c
Arena scratch = arena(kilobytes(64));
@defer arena_free(&scratch);
```

**Named defer:**

`@defer name: stmt;` creates a cancellable defer.

```c
@defer cleanup: arena_free(&scratch);

// ... later, if ownership is transferred ...
cancel cleanup;  // defer will not run
```

**Rule:** `cancel name;` prevents the named defer from running. It is a compile error to cancel a defer that has already run or been cancelled.

**Rule:** The name introduced by `@defer name:` is scoped to the enclosing block, like a local variable declared at the `@defer` statement. Referencing it (including `cancel`) before the `@defer` statement or outside the block is a compile error.

**Lowering (implementation sketch, not surface syntax):**

```c
// @defer cleanup: STMT;
// ...
// cancel cleanup;

// lowers to:
bool __cleanup_active = true;
@defer { if (__cleanup_active) { STMT; } }
...
__cleanup_active = false;  // cancel cleanup;
```

**Note:** Lowering is conceptual; the backend may implement defers via a hidden stack of cleanup actions, not via nested `@defer` syntax.

**Use cases:**

```c
// Transaction commit/rollback
void!DbError transfer(Db* db, Account from, Account to, int amount) {
    try db.begin();
    @defer rollback: db.rollback();
    
    try db.debit(from, amount);
    try db.credit(to, amount);
    
    cancel rollback;  // success: don't rollback
    try db.commit();
}

// Conditional cleanup
void!IoError process(char[:] path, Arena* out) {
    Arena scratch = arena(kilobytes(64));
    @defer cleanup: arena_free(&scratch);
    
    char[:] data = try read_file(&scratch, path);
    
    if (should_keep(data)) {
        // Transfer to output arena
        char[:] copy = arena_alloc<char>(out, data.len);
        memcpy(copy.ptr, data.ptr, data.len);
        // Still want cleanup to run - don't cancel
    }
    
    // cleanup runs on all paths
}
```

**Rule (defer is scope-bound):** Defer statements create `@scoped` handles (see § 3.1). A defer statement or cancellable defer name cannot be held across a suspension point. Attempting to reference a defer across `await` or `@match` is a compile error. This prevents defers from running during async operations where cleanup order becomes unpredictable.

```c
@async void!Error good(Arena* scratch) {
    // ✅ CORRECT: defer within sync scope, not across suspension
    {
        @defer cleanup: arena_free(scratch);
        char[:] data = sync_read(scratch);
        process(data);
    }
    
    // defer has already run
    await io_operation();
}

@async void!Error bad(Arena* scratch) {
    @defer cleanup: arena_free(scratch);
    // ❌ ERROR: defer is @scoped and cannot be held across await
    // await io_operation();
}
```

---

### 3.2 Arena blocks

Arena blocks provide scoped arena lifetime (create form) and scoped reset (reset form).

**Create + free:**

```c
arena scratch(kilobytes(64)) {
    char[:] tmp = read_file(&scratch, "x");
    use(tmp);
}
// scratch is freed here
```

The identifier `scratch` is in scope only within the block.

**Scoped reset:**

```c
Arena scratch = arena(kilobytes(256));
defer arena_free(&scratch);

for (int i = 0; i < 10; i++) {
    arena(&scratch) {
        char[:] tmp = arena_alloc<char>(&scratch, 1024);
        process(tmp);
    }
    // scratch is reset here (memory reclaimed, slices invalidated)
}
```

The reset form is selected when the parenthesized expression has type `Arena*` (after applying `&` as usual in C). Both `arena(&a)` (where `a` is `Arena`) and `arena(ptr)` (where `ptr` is `Arena*`) are valid.

**Rule:** `arena(ptr) { ... }` resets `*ptr` exactly once at block exit, including on `return` or `try` propagation.

**Rule:** Arena blocks are scoped lifetime/reset sugar around a named arena.

**Lowering:**

```c
// arena scratch(size) { BODY }
// lowers to:
{
    Arena scratch = arena(size);
    defer arena_free(&scratch);
    BODY
}

// arena(ptr) { BODY }   // where ptr has type Arena*
// lowers to:
{
    defer arena_reset(ptr);
    BODY
}
```

**Rule:** Any slice derived from an arena that is freed or reset by an arena block is treated as potentially invalid after the block. Using it after the block is a compile error unless it is proven independent (e.g., copied to longer-lived storage).

---

## 5. Threads

This section defines OS-level parallelism and shared state:

- **Thread API** — spawning and joining
- **ThreadGroup** — coordinating multiple workers
- **Sendability** — what can cross thread boundaries
- **`Mutex<T>`** — mutex-protected shared state
- **`Atomic<T>`** — lock-free atomic primitives

---

### 5.1 Thread API

OS threads are for CPU parallelism.

```c
Thread spawn_thread(void (*fn)(void));   // spawn with function pointer
Thread spawn_thread(() => { ... });      // spawn with closure
void   Thread.join();                    // wait for completion
```

**Rule (captures in thread closures):** Capturing a variable `v` into a thread closure is allowed iff:
1. `v` is capturable (see §1.2), AND
2. `v` is not assigned after the capture point, AND
3. No address of `v` is taken in a way that escapes the closure's scope

**Rule (value capture):** Thread closures capture by value by default. For copyable types, the captured value is a copy. For move-only types (e.g., `Map<K,V>`, unique slices), the capture is a move and the original becomes invalid. Value-captured variables are immutable within the closure.

**Rule (reference capture):** Explicit reference capture `[&v]` creates a shared reference to the outer variable. Reference captures are subject to mutation checks:
- Read-only access is allowed
- Mutation is a compile error unless the type is a safe wrapper (`@atomic T`, `Atomic<T>`, `Mutex<T>`, channel handles), or the capture is inside `@unsafe`

```c
int x = 0;

// Value capture (default): x is copied, immutable in closure
spawn(() => { printf("%d", x); });     // ✅ OK
spawn(() => { x++; });                 // ❌ ERROR: value capture is immutable

// Reference capture: explicit sharing with mutation check
spawn([&x]() => { printf("%d", x); }); // ✅ OK: read-only
spawn([&x]() => { x++; });             // ❌ ERROR: mutation of shared ref

// Safe wrappers: mutation allowed
Atomic<int> counter = atomic_new(0);
spawn([&counter]() => { counter++; }); // ✅ OK: Atomic is thread-safe

// Escape hatch: @unsafe bypasses check
spawn(@unsafe [&x]() => { x++; });     // ⚠️ OK: explicit unsafe
```

This prevents data races while allowing explicit shared state through safe wrappers.

---

### 5.2 Direct OS Threading (Advanced)

**For most applications, use `@nursery { spawn ... }` instead of the APIs in this section.**

Direct OS thread control is rarely needed. For details on `ThreadGroup` and `spawn_thread` for NUMA/affinity/CPU-pinning use cases, see **Appendix D: Advanced Runtime Control**.

---

### 5.3 Mutex<T> (OS Mutex)

`Mutex<T>` provides mutex-protected access to shared state for any type `T` using OS-level synchronization.

```c
Mutex<T> mutex(T initial);                    // create with initial value
LockGuard<T> Mutex<T>.lock_guard();           // get a lock guard
void Mutex<T>.with_lock(void (*fn)(T*));      // callback-style access (can block)
```

#### 4.3.1 Structured Lock Access with `@lock`

The `@lock` statement provides clean, scoped access to mutex-protected data:

```c
@lock (m) as value {
    // value is T (auto-dereferenced from LockGuard<T>)
    // can read/modify value freely
}  // lock automatically released here
```

**Examples:**

```c
Mutex<int> counter = mutex(0);

// Simple increment
@lock (counter) as c {
    c++;
    c++;
}

// Struct access (auto-deref handles operator->)
Mutex<struct { int x; int y; }> state = mutex({0, 0});
@lock (state) as s {
    s.x = 10;
    s.y = 20;
}

// Complex operations
Mutex<Vec<int>> tasks = mutex(vec_new(&arena));
@lock (tasks) as task_list {
    vec_push(task_list, 42);
    vec_push(task_list, 43);
    int len = vec_len(task_list);
}
```

**Lowering (conceptual):**

```c
// @lock (m) as value { BODY }
// lowers to:
auto __guard = m.lock_guard();
T* value = &(*__guard);  // or operator-> access
{
    BODY
}
__guard goes out of scope, lock releases
```

#### 4.3.2 Lock Guard (Alternative Approach)

If you need the guard to escape the `@lock` block (rare), use direct `lock_guard()`:

```c
Mutex<int> counter = mutex(0);
auto g = counter.lock_guard();
int val = *g;  // explicit deref
*g += 1;
// explicitly or via scope exit
```

#### 4.3.3 Callback Style (Alternative)

For cases where explicit callback control is preferred (not recommended):

```c
counter.with_lock(c => {
    *c += 1;
});
```

**Note:** The `@lock (m) as var { }` statement form is preferred over callbacks because it supports all C control flow (return, break, continue) cleanly.

**`LockGuard<T>` semantics:**
- Automatic unlock on scope exit (RAII)
- Automatically dereferenced in `@lock` statements
- Can be manually dereferenced via `*guard` or `guard->field` if needed
- Not sendable—must not outlive the scope where created

**Rule:** `Mutex<T>` is sendable (can be shared across threads).

**Rule:** `Mutex<T>.lock_guard()` and `Mutex<T>.with_lock()` can block the OS thread. They are allowed in `@async` code **only via implicit `run_blocking`** wrapping. The compiler wraps `lock_guard()` / `with_lock()` calls in `@async` code automatically, treating them as non-`@async` function calls. Keep critical sections short since the entire thread pool thread is blocked while the lock is held.

**Lowering in `@async`:**

```c
@async void good(Mutex<int>* m) {
    @lock (m) as g {
        g++;
    }  // lowers to: await __implicit_nursery.run_blocking(() => {
       //     auto __guard = m.lock_guard();
       //     int& g = *__guard;
       //     g++;
       // })
}
```

The `run_blocking` wrapper ensures the lock is acquired and released within a single thread pool dispatch, preventing deadlock from re-entrancy.

```c
// ❌ WRONG: direct call to lock_guard() without @lock syntax
@async void bad(Mutex<int>* m) {
    auto g = m.lock_guard();  // ERROR: bare blocking call in @async
    g++;
}

// ❌ ALSO WRONG: guard held across await
@async void bad_await(Mutex<int>* m) {
    @lock (m) as g {
        g++;
        await some_async_work();  // ERROR: guard held across suspension point
    }
}

// ✅ CORRECT: use @lock in @async (wrapped automatically)
@async void good(Mutex<int>* m) {
    @lock (m) as g {
        g++;
    }  // implicit run_blocking wraps the lock/unlock
}
```

**Rule (handle semantics):** `Mutex<T>` is a handle type. Copying creates another reference to the same underlying synchronized state.

---

### 5.4 AsyncMutex<T> (Suspending Mutex)

`AsyncMutex<T>` provides mutex-protected access that **suspends** instead of blocking, making it safe for `@async` code.

```c
AsyncMutex<T> async_mutex(T initial);           // create with initial value
@async AsyncGuard<T> AsyncMutex<T>.lock();      // suspending lock
```

**Usage with `@lock` statement (recommended):**

```c
AsyncMutex<int> counter = async_mutex(0);

@async void increment() {
    @lock (await counter.lock()) as c {
        c++;
        c++;
    }  // lock automatically released
}
```

**Direct guard access (alternative):**

```c
AsyncMutex<int> counter = async_mutex(0);

@async void increment() {
    auto g = await counter.lock();  // suspends, does NOT block OS thread
    *g += 1;
}  // unlocks here
```

**`AsyncGuard<T>` semantics:**
- Automatic unlock on scope exit (RAII)
- Automatically dereferenced in `@lock` statements
- Can be manually dereferenced via `*guard` or `guard->field` if needed
- Not sendable—must not outlive the scope where created

**Rule:** `AsyncMutex<T>.lock()` is **suspending** (not a blocking operation). It is allowed in `@async` code.

**Rule:** `AsyncGuard<T>` is not sendable. It must not outlive the scope where it was created.

**Rule:** `AsyncMutex<T>` is sendable.

**Rule (scope-bound guard types):** `LockGuard<T>` and `AsyncGuard<T>` are `@scoped` types (see § 3.1). They cannot be held across suspension points. The compiler enforces this at compile time to prevent deadlocks and keep critical sections short.

```c
@async void bad(AsyncMutex<int>* m, int[~ >]* tx) {
    auto g = await m.lock();
    await tx.send(*g);  // ❌ ERROR: @scoped value guard held across suspension
}

@async void good(AsyncMutex<int>* m, int[~ >]* tx) {
    int val;
    {
        auto g = await m.lock();
        val = *g;
    }  // guard released: scope-bound value is out of scope
    await tx.send(val);  // ✅ OK: no @scoped value held
}
```

**Rule (no guard across run_blocking):** No mutex guard (`LockGuard` or `AsyncGuard`) may be live across a call to `run_blocking`. Since guards are `@scoped`, the compiler enforces this automatically. This prevents deadlocks where the blocking closure attempts to acquire the same mutex.

```c
@async void bad(AsyncMutex<int>* m) {
    auto g = await m.lock();
    // Cannot do any blocking/await operations while guard is held
    await async_work();  // ERROR: cannot await while holding guard
}

@async void good(AsyncMutex<int>* m) {
    int val;
    {
        auto g = await m.lock();
        val = *g;
    }  // guard released, now we can await
    await async_work();  // OK: no guard held
}
```

**Rule (handle semantics):** `AsyncMutex<T>` is a handle type. Copying creates another reference to the same underlying synchronized state.

**When to use which:**

| Type | Use case |
|------|----------|
| `Mutex<T>` | Sync code or functions that can block, short critical sections |
| `AsyncMutex<T>` | `@async` code, short critical sections (no await while held) |

---

### 5.5 Atomic<T> (Lock-Free)

`Atomic<T>` provides lock-free atomic operations for primitive types with configurable memory ordering.

```c
// Memory ordering (mirrors C11)
enum Ordering { relaxed, acquire, release, acq_rel, seq_cst }

// Creation
Atomic<T> atomic(T initial);

// Load/Store (default: seq_cst)
T    Atomic<T>.load(Ordering o = .seq_cst);
void Atomic<T>.store(T v, Ordering o = .seq_cst);

// Arithmetic
T    Atomic<T>.fetch_add(T delta, Ordering o = .seq_cst);
T    Atomic<T>.fetch_sub(T delta, Ordering o = .seq_cst);

// Bitwise
T    Atomic<T>.fetch_and(T mask, Ordering o = .seq_cst);
T    Atomic<T>.fetch_or(T mask, Ordering o = .seq_cst);
T    Atomic<T>.fetch_xor(T mask, Ordering o = .seq_cst);

// Min/Max
T    Atomic<T>.fetch_min(T v, Ordering o = .seq_cst);
T    Atomic<T>.fetch_max(T v, Ordering o = .seq_cst);

// Compare-and-swap
bool Atomic<T>.compare_exchange(T* expected, T desired,
                                Ordering success = .seq_cst,
                                Ordering failure = .seq_cst);
```

**Rule:** `Atomic<T>` is only available when `T` is one of: `bool`, integer types (`i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`), or pointer types.

**Rule (default memory ordering):** All atomic operations default to `seq_cst` (sequentially consistent) unless an explicit ordering is provided. This is the safest default and matches C11 behavior. Applications requiring relaxed ordering must explicitly specify it.

**Rule:** `Atomic<T>` is sendable.

**Rule (handle semantics):** `Atomic<T>` is a handle type. Copying creates another reference to the same underlying atomic state.

**Rule:** Ordering semantics match C11 `_Atomic` and lower directly to platform atomics/intrinsics.

**Example:**

```c
Atomic<int> counter = atomic(0);

spawn_thread(() => {
    counter.fetch_add(1, .relaxed);  // relaxed for simple counters
});

spawn_thread(() => {
    counter.fetch_add(1, .relaxed);
});

// ... after join ...
int final = counter.load(.acquire);  // 2
```

**Example (lock-free flag):**

```c
Atomic<bool> ready = atomic(false);
Atomic<int> data = atomic(0);

// Producer
data.store(42, .relaxed);
ready.store(true, .release);  // release ensures data visible

// Consumer
while (!ready.load(.acquire)) {}  // acquire syncs with release
int v = data.load(.relaxed);      // guaranteed to see 42
```

**Rule:** Non-`Mutex`, non-`Atomic` mutable state cannot be accessed from multiple threads.

---

## 6. Channels

This section defines message-passing and coordination primitives:

- **§7.1 Channel Types** — topologies and views
- **§7.2 Semantics** — buffering and termination
- **§7.3 Copy vs Transfer** — `send` vs `send_take`
- **§7.4 Select** — multiplexing operations
- **§7.5 Timeouts** — time-bounded operations
- **§7.6 Channel API** — function signatures

---

Channels are typed queues for **handoff and coordination**.

Key properties:

* `recv()` returns `T?` (explicit termination)
* `send` / `send_take` return `bool` (send-after-close is non-fatal)
* Slices are copied by default; transfer is explicit

**Rule:** The free function form is normative. `ch.recv()` is UFCS sugar for `recv(&ch)`; `ch.send(v)` lowers to `send(&ch, v)`.

**Rule:** `await` is only valid inside `@async` functions.

**Rule:** `recv()`, `send()`, and `send_take()` are dual-mode operations:
* In `@async` code, they are suspension points and must be written as `await ch.recv()` / `await ch.send(v)` / `await ch.send_take(v)` unless used inside `select` (which implicitly awaits).
* In sync code, they block the OS thread.

**Rule:** Channel operations have identical types in sync and async contexts. `recv()` returns `T?`, `send()` returns `bool`. The suspension behavior is contextual, not encoded in types. There is no `Task<T?>` wrapper—`await ch.recv()` yields `T?` directly.



### 6.1 Channel Types

Channels are categorized by **mode** (async vs sync) and **topology/direction**.

#### Mode: Async vs Sync

| Type        | Meaning                        |
| ----------- | ------------------------------ |
| `T[~ ... async ...]` | async channel (default) |
| `T[~ ... sync ...]` | sync channel (blocking) |

**Spec change (breaking): Combined channels removed**

- The combined channel type `T[~...]` (which could both send and receive) is **no longer allowed**.
- A channel is represented by **two distinct handles**:
  - `T[~... >]` — **send-only** handle (tx)
  - `T[~... <]` — **recv-only** handle (rx)
- A channel is created only by producing a `(tx, rx)` pair (see **Creation** below).
- `close(...)` is only valid on a **send** handle (`>`). Closing affects the underlying channel and is observed by the `recv` handle (`<`) as termination.

**Async channel topology:**
| Type        | Meaning                        |
| ----------- | ------------------------------ |
| `T[~ ... >]` | send-only handle (tx) |
| `T[~ ... <]` | recv-only handle (rx) |
| `T[~n N:N >]` | buffered N:N sender |
| `T[~n N:N <]` | buffered N:N receiver |
| `T[~n 1:1 >]` | single producer sender |
| `T[~n 1:1 <]` | single consumer receiver |
| `T[~n N:1 >]` | many producers sender |
| `T[~n N:1 <]` | one consumer receiver |
| `T[~n 1:N >]` | broadcast publisher (one producer) |
| `T[~n 1:N <]` | broadcast subscriber (receiver view) |

**Sync channel topology:**
| Type        | Meaning                        |
| ----------- | ------------------------------ |
| `T[~ ... sync ... >]` | send-only handle (tx), blocking |
| `T[~ ... sync ... <]` | recv-only handle (rx), blocking |
| `T[~n sync N:N >]` | buffered N:N sender (blocking) |
| `T[~n sync N:N <]` | buffered N:N receiver (blocking) |
| `T[~n sync 1:1 >]` | single producer sender (blocking) |
| `T[~n sync 1:1 <]` | single consumer receiver (blocking) |
| etc. | (all topologies available) |

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
channel_pair(&tx, &rx);  // the only way to create a channel
```

Notes:
- `channel_pair` initializes both handles to the same underlying channel.
- `close(&tx)` closes the underlying channel.
- `free(&tx)` and `free(&rx)` free their respective handles; freeing requires no other threads are using the channel (same as today).

**Error channels:**

Channels can carry results using `T!E` as the element type:

```c
int!ParseError[~100 >] results_tx;        // async sender of Result<int, ParseError>
int!ParseError[~100 <] results_rx;        // async receiver of Result<int, ParseError>
channel_pair(&results_tx, &results_rx);

int!ParseError[~100 sync >] sync_results_tx;   // sync sender
int!ParseError[~100 sync <] sync_results_rx;   // sync receiver
channel_pair(&sync_results_tx, &sync_results_rx);
```

**Rule (recv return type):** `recv()` always returns `(ElemType)?`. For a channel with element type `T!E`, `recv()` returns `(T!E)?` which is spelled `T!E?`. The outer `?` indicates channel state (null = closed+drained); the inner `!E` is application-level success/failure.

```c
// Async channel: must use await
int!ParseError? r = await results.recv();
if (!r) {
    // channel closed+drained
} else if (r.ok) {
    int v = r.value;  // application success
} else {
    ParseError e = r.error;  // application error
}

// Sync channel: no await
int!ParseError? r = results.recv();
if (!r) {
    // channel closed+drained
} else if (r.ok) {
    int v = r.value;
} else {
    ParseError e = r.error;
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

replaced_5 Semantics

* **Buffered channels** enqueue up to `n` items; sends block when full.
* **Unbuffered channels** rendezvous: sender blocks until receiver is ready.
* **Close** does not drop buffered values; they remain available for recv.
* **Termination** is observed when the channel is both closed and drained (`recv()` returns `null`).

**Rule:** `recv()` returns `null` only when the channel is closed and drained. It never returns `null` to represent an application-level error. For per-item errors, use `T!E[~ <]` channels where `recv()` returns `T!E?`.

**Rule (slice element ownership):** For slice element types, `send` deep-copies into channel-internal storage. While queued, the channel owns the copy. On successful `recv`, the receiver gets a **unique slice**; the receiver frees it on scope exit (or transfers it via `send_take` / return). If the value is never received (still buffered when channel is freed), the channel frees it.

**Rule (broadcast `1:N`):** `send` never blocks on subscribers; if a subscriber's buffer is full, the oldest value for that subscriber is dropped.

**Rule (broadcast copy semantics):** On a `1:N` channel, `send` performs a deep copy for **each subscriber** at send time. Each subscriber receives an independent unique slice. For slice elements, this means N independent allocations for N subscribers.

**Rule (broadcast close):** Closing a `1:N` channel closes the broadcaster and all subscriber views. Subscribers may continue draining buffered values; after drain, `recv()` returns `null`.

**Close semantics:**

* `close()` is idempotent — calling it multiple times is safe and has no additional effect.
* Sends after close return `false` and do not enqueue the value.
* Failed `send_take` does not consume the slice — it remains valid.
* Buffered values are **not** dropped on close — receivers can drain them.

---

replaced_5 Copy vs Transfer

* `send(slice)` — deep-copies contents into channel-internal storage.
* `send_take(slice)` — transfers ownership of the backing allocation (zero-copy).
* `send_take` never falls back to copying; it fails if ineligible.
* Broadcast (`1:N`) forbids `send_take` (each subscriber needs its own copy).

**`send_take` Eligibility (Normative):**

A call to `send_take(ch, slice)` succeeds iff **all** of the following hold:

1. `slice.id.is_unique == 1`
2. `slice.id.is_transferable == 1`
3. `slice.id.is_subslice == 0`
4. The destination channel is not closed
5. The channel topology is not broadcast (`1:N`)

These conditions are checked using the slice's `id` field directly — **no runtime table lookup is required** in release builds.

**Rule (derived slices ineligible):** Any derived slice value (including subslices, rebindings, or copies) clears `is_unique` and is therefore ineligible for `send_take`, even if it covers the full allocation. Only the original owning unique slice can be transferred.

**Failure behavior:**
- If any eligibility condition fails, `send_take` returns `false` and does not consume the slice (conditional move semantics).
- On success, ownership of the allocation is transferred to the channel and all borrows are invalidated.

```c
char[:]? x = await ch.recv();       // is_unique=1, is_transferable=1, is_subslice=0
await dst.send_take(*x);            // OK: transfers the unique slice

// Derived slices cannot be transferred:
char[:] view = (*x)[0..(*x).len];   // view has is_unique=0 (derived)
await dst.send_take(view);          // returns false: is_unique=0
```

**Why `adopt()` slices cannot use `send_take`:**

`adopt()` slices have `is_transferable == 0` because the user-provided deleter may not be thread-safe. If the slice were transferred to another thread and freed there, a thread-local allocator would corrupt memory. Channel-internal allocations (`recv()` slices) use a thread-safe allocator that we control, so they have `is_transferable == 1`.

```c
// ✓ Channel pipeline - zero copy
char[:]? x = await a.recv();         // is_transferable=1
await b.send_take(*x);               // OK

// ✗ FFI buffer - must copy  
unsafe {
    char[:] s = adopt(p, 100, custom_free);  // is_transferable=0
}
await ch.send(s);                    // OK: copies (safe)
await ch.send_take(s);               // returns false: is_transferable=0
```

**Rule:** `adopt()` slices are unique (move-only, have destructor) but not transferable. Use `send` to copy them across channels.

---

replaced_5 Select / Multiplex

`select` waits on multiple channel operations:

```c
@match {
    case int? x = ch.recv(): handle(x);
    case ch.send(v):         sent();
    case timeout(100ms):     retry();
}
```

* Works in both sync and `@async` code.
* In async code, suspends cooperatively.
* In sync code, blocks the OS thread.
* First ready case wins; if multiple ready, one is chosen non-deterministically.

**Rule:** In `@async` code, `select` is itself a suspension point; channel ops in case headers are implicitly awaited. No explicit `await` appears in case headers.

**Rule (select readiness on closed):**
- A `recv` case is ready if a value is available OR if the channel is closed+drained (binding `null`).
- A `send` / `send_take` case is ready if it can complete immediately. If the channel is closed, it is ready immediately and completes with `false`.

**Rule (select fairness):** When multiple cases are ready, selection uses round-robin arbitration. Each **dynamic select instance** maintains a hidden rotating start index (stored as a hidden local variable in the stack frame or async state machine). The first ready case starting from that index is chosen, then the index advances. This prevents systematic bias at a given select site (e.g., "case 0 always wins") but does not guarantee global fairness across concurrent executions.

**Clarification:** A "dynamic select instance" corresponds to a particular execution of a select statement. If a select is inside a loop, each iteration uses the same index variable (advancing each time). If a function containing a select is called multiple times, each call has its own index.

**Starvation caveat:** `select` does not guarantee progress if one case is continuously ready. If case 0 is always ready, other cases may be starved across repeated select iterations. Applications requiring stronger fairness guarantees should implement explicit scheduling or use channel-based work queues with blocking operations.

---

replaced_5 Timeouts and Duration

**Duration type:**

`Duration` represents a time span with nanosecond precision:

```c
struct Duration {
    int64_t secs;    // seconds
    int32_t nanos;   // nanoseconds (0 to 999,999,999)
};
```

**Duration literals:**

| Literal | Meaning |
|---------|---------|
| `1ns` | 1 nanosecond |
| `1us` | 1 microsecond |
| `1ms` | 1 millisecond |
| `1s` | 1 second |
| `1m` | 1 minute |
| `1h` | 1 hour |

**Rule:** Duration literals are evaluated at compile time. Overflow is a compile-time error.

**Rule:** Duration arithmetic (`+`, `-`, `*`, `/`) is supported. Runtime overflow is undefined behavior (debug builds may trap).

**Timeouts:**

Timeouts are defined via `select`:

```c
// In sync code:
int? v = recv_timeout(&ch, 100ms);

// In async code:
int? v = await recv_timeout(&ch, 100ms);

// Equivalent to:
int? v = null;
@match {
    case int? x = ch.recv(): { v = x; }
    case timeout(100ms):     { v = null; }
}
```

**Rule:** `timeout(Duration d)` is a select-only readiness case that becomes ready after duration `d` elapses.

**Rule:** `recv_timeout` follows the same dual-mode rules as `recv`: it requires `await` in `@async` code and blocks in sync code.

---

replaced_5 Channel API

```c
// Creation (normative): produce a (tx, rx) pair. Combined channels are not allowed.
int[~10 >] tx;
int[~10 <] rx;
channel_pair(&tx, &rx);

// === ASYNC CHANNELS (int[~ ... >] / int[~ ... <]) ===
// send/recv operations require await in @async code

// Core operations (must await)
bool ok = await send(&tx, value);      // suspends until sent
T? x = await recv(&rx);                // suspends until received

// Slice transfer (must await; send handle only)
bool ok = await send_take(&tx, slice);  // suspends, transfers ownership

// Close (no await; send handle only)
void close(&tx);                        // idempotent, no await

// Cancellation-aware variants (must await)
bool ok = await send_cancellable(&tx, value);   // returns false if cancelled
T!Cancelled? x = await recv_cancellable(&rx);   // returns err(Cancelled) if cancelled

// Non-blocking (no await, either context)
bool ok = try_send(&tx, value);        // returns false if full/closed, never awaits
T!RecvStatus x = try_recv(&rx);        // returns immediately

// Timeout (must await)
T? x = await recv_timeout(&rx, Duration d);

// === SYNC CHANNELS (int[~ ... sync ... >] / int[~ ... sync ... <]) ===
// All operations block, no await allowed

int[~10 sync >] stx;
int[~10 sync <] srx;
channel_pair(&stx, &srx);

// Core operations (no await, blocks)
bool ok = send(&stx, value);            // blocks OS thread until sent
T? x = recv(&srx);                      // blocks OS thread until received

// Slice transfer (no await, blocks; send handle only)
bool ok = send_take(&stx, slice);       // blocks, transfers ownership

// Close (no await; send handle only)
void close(&stx);                       // idempotent

// Non-blocking (no await, either context)
bool ok = try_send(&stx, value);        // returns false if full/closed
T!RecvStatus x = try_recv(&srx);        // returns immediately

// Timeout (blocks up to duration, no await)
T? x = recv_timeout(&srx, Duration d);
```

**Operations Comparison:**

| Operation | Async `T[~ ... >]` / `T[~ ... <]` | Sync `T[~ ... sync ... >]` / `T[~ ... sync ... <]` |
|-----------|---|---|
| `send(ch, v)` | `await send(...)` ✅ | `send(...)` ✅ |
| `recv(ch)` | `await recv(...)` ✅ | `recv(...)` ✅ |
| `send_take(ch, s)` | `await send_take(...)` ✅ | `send_take(...)` ✅ |
| `send_cancellable(ch, v)` | `await send_cancellable(...)` ✅ | N/A ❌ |
| `recv_cancellable(ch)` | `await recv_cancellable(...)` ✅ | N/A ❌ |
| `try_send(ch, v)` | `try_send(...)` ✅ | `try_send(...)` ✅ |
| `try_recv(ch)` | `try_recv(...)` ✅ | `try_recv(...)` ✅ |
| `recv_timeout(ch, d)` | `await recv_timeout(...)` ✅ | `recv_timeout(...)` ✅ |
| `close(ch)` | `close(...)` ✅ | `close(...)` ✅ |
| `subscribe(ch)` | `subscribe(...)` ✅ | `subscribe(...)` ✅ |

**Rule (async channel operations):** All operations on async channel handles (`T[~ ... >]` / `T[~ ... <]` or `T[~ ... async ... >/<]`) that may suspend require `await`. These include `send()`, `recv()`, `send_take()`, `recv_cancellable()`, `send_cancellable()`, and `recv_timeout()`. Omitting `await` is a compile error.

**Rule (sync channel operations):** All operations on sync channel handles (`T[~ ... sync ... >]` / `T[~ ... sync ... <]`) that may block have no `await`. These include `send()`, `recv()`, `send_take()`, and `recv_timeout()`. Adding `await` is a compile error.

**Rule (non-blocking operations):** `try_send()`, `try_recv()`, `close()`, and `subscribe()` are valid on both async and sync channels without `await`. They return immediately or have no return value.

**Rule:** Sync channels do not support cancellation-aware variants (`recv_cancellable`, `send_cancellable`). If you need cancellation, use async channels with `@match`.

**RecvStatus enum:**

```c
enum RecvStatus { WouldBlock, Closed }
```

**Rule (send_take conditional move):** `send_take` uses **conditional move** semantics: the slice argument is consumed only if the call returns `true`. On failure (channel closed), the slice remains valid and the caller retains ownership. This is special-case semantics for `send_take` only.

```c
char[:] s = ...;
if (!await ch.send_take(s)) {
    // Channel was closed, but s is still valid
    use(s);  // OK
}
// If send_take returned true, s is now invalid
```

**Rule (async ownership):** In `@async` code, ownership transfer via `send_take` occurs at the suspension point. After a successful `await send_take`, the source slice is invalidated exactly as if moved synchronously. There is no "partial" or "pending" ownership state across the `await`.

```c
char[:]? x = await ch.recv();    // x owns unique slice
bool ok = await dst.send_take(*x);
if (ok) {
    // *x is now invalid, ownership transferred
    use(*x);  // ERROR: use after move
} else {
    // Channel was closed, *x still valid
    use(*x);  // OK
}
```

**Rule (send borrows slices):** When passing a slice to `send`, the slice is borrowed for the duration of the copy operation, not moved. This applies even to unique slices:

```c
char[:]? u = await src.recv();  // unique slice
await dst.send(*u);              // borrows *u, copies bytes, *u still valid
use(*u);                         // OK: u still owns the buffer
```

- `try_recv` returns `ok(value)` if a value is available
- `try_recv` returns `err(RecvStatus.WouldBlock)` if the channel is empty but open
- `try_recv` returns `err(RecvStatus.Closed)` if the channel is closed **and** drained

**Note (try_recv vs recv):** Both `recv()` and `try_recv()` return values from a closed channel as long as buffered values remain. The "closed" status is only reported after all buffered values have been drained. `recv()` signals this via `null`; `try_recv()` signals it via `err(Closed)`.

**Note (schematic signatures):** Signatures use `T[~ ... >]*` / `T[~ ... <]*` as shorthand for the channel handle family. The actual type system uses the full channel handle type including capacity, mode, topology, and direction:

```c
// Full type signatures (what the compiler sees):
bool send<T, N, Topo>(T[~N Topo >]* ch, T value);  // send-only
T?   recv<T, N, Topo>(T[~N Topo <]* ch);           // recv-only
```

Capacity `N` and topology `Topo` are erased at runtime (all use the same implementation), but the type system enforces view restrictions at compile time.

**Rule:** Calling `send` on a recv-only channel view (`T[~n <]`), or `recv` on a send-only channel view (`T[~n >]`), is a compile-time error.

**Rule:** Channel handles must be initialized via `channel_pair(&tx, &rx)` (or equivalent constructor for special topologies). The combined form `T[~n]` is not allowed.

---

## 7. Concurrency

Concurrent-C provides a single, unified concurrency primitive: **nurseries**. A nursery is a structured concurrency construct that manages task spawning, automatic error propagation, and cooperative cancellation. For the rare case where OS-level thread control is required, low-level APIs exist but should not be used in typical application code.

This section specifies:

- **§7.1 Structured Concurrency with Nurseries** — the primary pattern for all concurrent work
- **§7.2 Async Functions and Automatic Task Batching** — how `@async` enables non-blocking I/O and automatic blocking call wrapping
- **§7.3 Tasks** — lazy async computations (Task.start is discouraged; use @nursery instead)
- **§7.4 Channels in Async vs Sync** — context-sensitive channel operations
- **§7.5 Cancellation** — cooperative task cancellation, automatically propagated in nurseries
- **§7.6 Streaming** — channel-based producers with @for await
- **§7.7 Runtime API** — function signatures for tasks, timing, and sync bridging
- **§7.8 Blocking, Stalling, and Execution Contexts** — execution model for blocking operations, stalling classification, and cancellation guarantees
- **§7.9–7.13 Error Handling** — Result types, try/catch, async errors, multiple error types

---

#### Recommendation: Use @nursery Everywhere

**For all concurrent work, use `@nursery { spawn ... }`** — single API, automatic error propagation, automatic sibling cancellation, and compile-time safety.

```c
// Fan-out/fan-in
@nursery {
    spawn (worker1());
    spawn (worker2());
    spawn (worker3());
}

// With error propagation
@nursery {
    spawn (ok_task());
    spawn (fail_task());  // error cancels siblings automatically
}

// With channel cleanup
@nursery closing(ch1, ch2) {
    spawn (producer(ch1));
    spawn (consumer(ch2));
}

// In @async (blocking calls wrapped automatically)
@async void handler() {
    blocking_call();  // compiler wraps in run_blocking
}
```

**Don't use:**
- ❌ `ThreadGroup` (advanced runtime control, Appendix D)
- ❌ `spawn_thread` (NUMA/affinity escape hatch, Appendix D)
- ❌ Direct OS threads in application code

---

### 7.1 Structured Concurrency with Nurseries

A **nursery** is a lexical structured-concurrency block that manages the lifetime, cancellation, and completion of spawned child tasks. Nurseries enforce a tree-shaped concurrency structure and provide compile-time guarantees against common deadlocks.

**Properties:**

- Tasks spawned within a nursery are children of that nursery
- The nursery waits for all children to complete before exiting
- Child task handles cannot outlive the nursery (compile-time error if they escape)
- If any child fails, all siblings are automatically cancelled
- Peer tasks cannot wait on each other (compile-time error)

**Syntax:**

```c
@nursery {
    spawn (work1());
    spawn (work2());
}  // implicitly joins all children
```

**With automatic channel close:**

```c
@nursery closing(ch1, ch2) {
    spawn (producer(ch1));
    spawn (consumer(ch2));
}  // closes ch1, ch2 after all children exit
```

---

#### 6.1.1 Nursery Semantics

**Task Lifetime:**

Every `spawn` inside a nursery creates a child task. The nursery implicitly waits for all children to complete before the block exits, even on early returns or errors.

```c
@nursery {
    spawn (long_running_task());
    spawn (another_task());
}  // blocks here until both tasks complete
```

**Error Propagation:**

If any spawned task returns an error or panics:
1. The nursery MUST cancel all remaining child tasks
2. The nursery MUST NOT complete until all child tasks have terminated (including observing cancellation and exiting)
3. The nursery block terminates with that error

Cancellation is observed according to **§ 3.2.2** and may be deferred by `with_shield` regions (**§ 7.5.2**).

```c
@nursery {
    spawn (ok_task());
    spawn (failing_task());  // returns err(E)
}
// nursery cancels ok_task, waits for cleanup, returns err(E)
```

**Cancellation:**

Cancellation is cooperative. Child tasks observe cancellation via `cc_is_cancelled()` or by awaiting operations that check the cancellation flag.

```c
@async void long_task() {
    while (!cc_is_cancelled()) {
        await do_work();
    }
}

@nursery {
    spawn (long_task());
}  // if another task fails, long_task observes cc_is_cancelled()
```

**Channel Close Ordering:**

The `closing(...)` clause ensures channels are closed after all children exit, eliminating close-before-send races:

```c
@nursery closing(out) {
    spawn (producer(out));   // sends to out
    spawn (consumer(out));   // receives from out
}
// Producer and consumer both exit before out is closed
```

---

#### 6.1.2 Compile-Time Restrictions (Normative)

**Rule (task handle escape):** A task handle created by `spawn` inside a nursery may not be:
- Returned from the nursery block
- Stored in a variable that outlives the nursery
- Captured by closures escaping the nursery

Violation is a compile-time error.

```c
// ERROR: task handle escapes
Task<void> t;
@nursery {
    t = spawn (work());  // ERROR: cannot store outside nursery
}
```

**Rule (no peer waits):** A child task may not await another child's completion inside the nursery. Violation is a compile-time error.

```c
// ERROR: peer task cannot wait on sibling
@nursery {
    Task<void> t1 = spawn task1();
    Task<void> t2 = spawn task2();
    
    await t1;  // ERROR: cannot await sibling in nursery
}
```

**Rule (no explicit join):** Child tasks may not call `threadgroup_join()` or equivalent join primitives. The nursery provides the only join point.

---

#### 6.1.3 Implicit Wrapping of Blocking Calls

Every `@async` function has an implicit nursery available for automatic wrapping of blocking calls. When the compiler detects a **direct call to a non-`@async` function** inside `@async` code, it automatically wraps that call in `run_blocking`.

This applies to:
- Extern functions (`extern int read(...)`)
- Concurrent-C functions without `@async` (`void helper(...)`)
- **Sync channel operations** (`recv()`, `send()` on `T[~ ... sync ...]` handles)
- **Synchronous mutex operations** (`Mutex<T>.lock_guard()`, `Mutex<T>.with_lock()` via `@lock` syntax)

**Async channels do NOT get wrapped** — they suspend cooperatively and work naturally in `@async` code via `await`.

```c
extern int sys_read(int fd, void* buf, int n);  // non-@async

@async void handler(int fd, int[~ <] async_rx, int[~ sync <] sync_rx) {
    char buf[128];
    sys_read(fd, buf, 128);        // Compiler auto-wraps: await __implicit_nursery.run_blocking(...)
    
    // Async channel: no wrapping needed, just await
    int x = await recv(&async_rx);  // Suspends cooperatively, no run_blocking
    
    // Sync channel: gets auto-wrapped
    int y = recv(&sync_rx);         // Compiler auto-wraps: await run_blocking(...)
    
    Mutex<int> m;
    @lock (m) as val {             // lock_guard() auto-wrapped: await run_blocking(...)
        val++;
    }
}
```

**Note:** This is why `@lock` and sync channels work in `@async` code even though they can block—they are automatically wrapped in `run_blocking`, which dispatches them to the thread pool. Keep critical sections short since the entire thread pool thread is blocked while the lock is held.

**Rule (automatic batching):** Multiple consecutive direct calls to non-`@async` functions (with no intervening suspension points) are automatically batched into a single thread pool dispatch for efficiency.

```c
@async void db_transaction(Db* db) {
    db_begin(db);           // batched together
    db_query(db, "SELECT");
    db_update(db, "UPDATE");
    db_commit(db);
    // Compiler batches all four into one dispatch
}
```

**Rule (suspension breaks batch):** A suspension point breaks the batch. Suspension points include `await`, `@match`, and `@async` function calls. Importantly, async channel operations (`await recv()` on async channels) are suspension points and break the batch.

**Rule (control flow does not break batching):** Conditional branches, loops, and other control flow structures are transparent to batching. Only explicit suspension points break a batch.

---

#### 6.1.4 Explicit Nursery with `spawn`

For fine-grained control over spawning and cancellation, use explicit `@nursery` blocks:

```c
@async void coordinated_work() {
    @nursery {
        spawn worker1();
        spawn worker2();
        spawn worker3();
    }
    // All three workers have completed and been joined here
}
```

**When to use explicit `@nursery`:**
- When you need to spawn multiple async children
- When you need error propagation and automatic cancellation
- When you want to pair with `closing(...)` for channel cleanup

**When auto-wrapping is sufficient:**
- Simple blocking calls in linear async code
- No explicit task spawning needed

---

#### 6.1.5 Desugaring (Normative)

A nursery:

```c
@nursery {
    spawn (f());
    spawn (g());
}
```

desugars to (approximately):

```c
{
    ThreadGroup __tg;
    threadgroup_init(&__tg);
    
    threadgroup_spawn(&__tg, f);
    threadgroup_spawn(&__tg, g);
    
    threadgroup_join(&__tg);
}
```

with compile-time restrictions enforced by the CC compiler (task handle escape checks, peer-join prevention).

The `closing(...)` clause:

```c
@nursery closing(ch) {
    spawn (producer(ch));
}
```

desugars to:

```c
{
    ThreadGroup __tg;
    threadgroup_init(&__tg);
    
    threadgroup_spawn(&__tg, () => producer(ch));
    threadgroup_join(&__tg);
    
    ch.close();  // closes AFTER join
}
```

---

#### 6.1.6 Nursery Guarantees

A nursery guarantees:

- ✅ All spawned tasks are joined
- ✅ No task outlives its lexical scope
- ✅ No forgotten join deadlocks (impossible syntactically)
- ✅ No cyclic peer waits (impossible syntactically)
- ✅ Deterministic error fan-out to all siblings (automatic)
- ✅ Deterministic cancellation propagation (automatic on error)
- ✅ Cancelled tasks will exit promptly (when they reach a cancellation-aware operation)
- ✅ Deterministic close ordering (when paired with `closing(...)`)

A nursery does **not** guarantee:

- ❌ Deadlock freedom for arbitrary channel cycles (outside nursery scope)
- ❌ Fairness or starvation freedom
- ❌ Immediate cancellation of blocking operations (cooperative; use @match or variants)
- ❌ Stack unwinding on cancellation (no destructors)

**Recommendation for cancellable tasks:** Use `@match` with cancellation-aware operations. Avoid plain `await` for operations that might block indefinitely on a cancelled task. See §7.5 Cancellation for examples.

---

#### 6.1.7 Implicit Nursery Lifetime

The implicit nursery in an `@async` function is created at function entry and joined at function exit (including early returns and error propagation).

```c
@async void!Error work() {
    int n = read(0, buf, 128);  // auto-wrapped in implicit nursery
    
    if (n < 0) return cc_err(Error.Fail);  // implicit nursery joined here
    
    return cc_ok(n);  // implicit nursery joined here
}
```

---

### 7.2 Async Functions and Automatic Task Batching

Every `@async` function has the compiler automatically wrap calls to non-`@async` functions in `run_blocking`, dispatching them to the thread pool. This is called **automatic task batching** and allows async code to look synchronous while maintaining non-blocking semantics.

**One function attribute controls execution behavior:**

- **`@async`:** Function may suspend cooperatively via `await`. Compiles to a pollable state machine. Tasks are lazy—nothing runs until awaited or `.start()`ed.

**The core rule:**

**Rule:** `@async` functions can directly call:
- Other `@async` functions (no wrapping needed)
- Any non-`@async` function (automatically wrapped in `run_blocking`)

All non-`@async` functions may block an OS thread. The compiler **automatically wraps** calls to non-`@async` functions when they appear inside `@async` functions. This is called **implicit `run_blocking` wrapping**.

```c
extern int sys_read(int fd, void* buf, int n);  // non-@async (can block)

void helper(int fd, char* buf) {
    return sys_read(fd, buf, 128);  // OK: helper is non-@async, can block freely
}

@async void handler(int fd) {
    char buf[128];
    
    // Both of these calls get auto-wrapped:
    sys_read(fd, buf, 128);         // direct non-@async call
    helper(fd, buf);                // indirect non-@async call
    // Desugars to:
    // await __implicit_nursery.run_blocking(() => sys_read(...));
    // await __implicit_nursery.run_blocking(() => helper(...));
}
```

This transformation makes the code look synchronous while maintaining async safety.

**Rule (transitive blocking):** Any non-`@async` function (whether it directly calls blocking code or not) may itself block. When an `@async` function calls any non-`@async` function, the compiler automatically wraps that call in `run_blocking`, allowing the code to look synchronous while remaining safe.

```c
extern int read(int fd, void* buf, int n);  // can block

void helper(int fd, char* buf) {
    return read(fd, buf, 128);  // OK: not in @async, can block freely
}

@async void good() {
    char buf[128];
    helper(0, buf);  // OK: helper is non-@async, auto-wrapped by compiler
    // Desugars to: await __implicit_nursery.run_blocking(() => helper(0, buf));
}
```

**Sync code has no restrictions:**

```c
void sync_code(int fd) {
    char buf[128];
    sys_read(fd, buf, 128);  // OK: sync code can block freely
}
```

**FFI and `extern` functions:**

**Rule:** All `extern` functions (C FFI) are treated as non-`@async` by default, meaning calls to them from `@async` functions will be auto-wrapped in `run_blocking`.

For FFI functions that are provably non-blocking (like `strlen`, `memcpy`, pure math functions), you can mark them `@noblock` to skip the wrapping:

```c
extern int read(int fd, void* buf, int n);        // non-@async, auto-wrapped in @async
extern @noblock int strlen(const char* s);        // explicitly non-blocking, no wrap needed
extern @noblock void memcpy(void* dst, void* src, size_t n);
```

**Rule:** Declaring `@noblock` on a function that may actually block is undefined behavior.

**Soundness expectation:** The compiler may assume `@noblock` is correct and eliminate wrapping safeguards. If a function declared `@noblock` actually blocks (e.g., on I/O or a mutex), the calling `@async` function may deadlock or exhibit other undefined behavior. This is a **contract obligation**—violations are not recoverable at runtime. In debug builds, implementations may add runtime checks to catch violations; in release builds, undefined behavior is not checked.

**Compiler enforcement:** The compiler may reorder, inline, or elide safety boundaries based on `@noblock` declarations. Do not lie to the compiler about blocking behavior.

---

### 7.3 Tasks

**Positioning:** Use `Task<T>` for single async computations that produce one result; use nurseries + channels for coordinated, multi-task work with backpressure.

```c
@async int work(int x) {
    return x + 1;
}

Task<int> t = work(5);    // created, not running
int v = await t;          // starts + waits
```

**Detached work:**

```c
@async void log_event(Event e) {
    await write_to_log(e);
}

log_event(evt).start();  // fire and forget
```

**Rule:** Detached tasks may only capture values proven to outlive the detach.

**Task lifecycle:**

| Operation | Behavior |
|-----------|----------|
| `Task<T> t = fn(args)` | Creates task, does not start |
| `await t` | Starts (if needed) + waits for result |
| `t.start()` | Starts detached, returns immediately |
| `await t` after `.start()` | Joins detached task, returns result |
| `await t` after previous `await t` | Returns cached result (double-await is safe) |

**Rule (double-await):** Awaiting an already-completed task returns the cached result immediately.

**Rule (detached task errors):** If a detached task returns `T!E` with an error and is never awaited, the error is silently discarded.

**Rule (Task<T> ABI):** `Task<T>` is an opaque handle. Its internal representation is implementation-defined.

**Recommendation (v1.0):** Prefer `@nursery` spawning over detached tasks (`Task<T>.start()`) unless explicit detachment is required. Structured concurrency via nurseries is safer and prevents lifetime footguns.

*Design note (non-normative):* Future versions may deprecate `Task<T>.start()` in favor of `@nursery` exclusively, pushing users toward structured concurrency by default. This would eliminate lifetime footguns around detached tasks and reduce the concurrency surface area.

---

### 7.4 Channels: Async vs Sync (Type-Based)

Channels are **explicitly typed as async or sync** at declaration. The type determines whether operations suspend (`@async` channel) or block (sync channel). This eliminates context-dependent behavior and ensures compiler safety.

Channels also support **backpressure modes** to handle overload gracefully in server workloads.

---

#### 7.4.0 Backpressure Modes

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
- `recv()` always returns in-order messages (no gaps caused by drops, except drops are not received).
- Sample rate must be in range `[0.0, 1.0]`; behavior at boundaries:
  - `Sample(0.0)`: drop all messages
  - `Sample(1.0)`: equivalent to Block mode

---

#### 7.4.1 Channel Types

**Async channels** (most common):
```c
int[~ >] tx;
int[~ <] rx;
channel_pair(&tx, &rx);
```

**Sync channels:**
```c
int[~ sync >] tx;
int[~ sync <] rx;
channel_pair(&tx, &rx);
```

**Rule:** Channel mode is fixed in the handle type. An `int[~ ...]` handle is always async; an `int[~ sync ...]` handle is always sync. Operations must match the handle type.

---

#### 7.4.3 Async Channels (`int[~ ... >]` and `int[~ ... <]`)

Async channels **suspend cooperatively** and require `await`. They are used in `@async` functions and with `@nursery`.

**Operations:**

```c
int[~ >] tx;
int[~ <] rx;
channel_pair(&tx, &rx);

// Must use await
int? x = await recv(&rx);                 // suspends, returns optional
bool ok = await send(&tx, 42);            // suspends, returns success
int? x = await recv_cancellable(&rx);     // returns err(Cancelled) if cancelled
bool ok = await send_cancellable(&tx, 42);

// Cannot use await
int x = recv(&rx);                        // ❌ ERROR: missing await
send(&tx, 42);                            // ❌ ERROR: missing await
```

**Rule:** All operations on async channels require `await`. Omitting `await` is a compile error (regardless of context).

**Cancellation integration:**

```c
@async void!Error reader(int[~ <] ch) {
    @match {
        case int x = await ch.recv():
            process(x);
        // implicit: case is_cancelled(): return cc_err(Cancelled);
    }
}
```

Async channels work with `@match` and have implicit cancellation cases.

---

#### 7.4.4 Sync Channels (`int[~ ... sync ... >]` and `int[~ ... sync ... <]`)

Sync channels **block the OS thread** and do NOT use `await`. They are used for thread coordination and blocking operations.

**Operations:**

```c
int[~ sync >] tx;
int[~ sync <] rx;
channel_pair(&tx, &rx);

// No await allowed
int? x = recv(&rx);                  // blocks OS thread
bool ok = send(&tx, 42);             // blocks OS thread
int? x = recv_cancellable(&ch);      // N/A: sync channels don't auto-support cancellation

// Cannot use await
int? x = await recv(&rx);            // ❌ ERROR: cannot await sync channel
bool ok = await send(&tx, 42);       // ❌ ERROR: cannot await sync channel
```

**Rule:** All operations on sync channels do NOT use `await`. Adding `await` is a compile error.

**No @match on sync channels:**

```c
int[~ sync <] rx;
@match {
    case x = recv(&rx):  // ❌ ERROR: @match requires async channel
        process(x);
}
```

Sync channels do not work with `@match`.

**Explicit blocking select (rare):**

```c
@match_blocking {
    case int x = recv(&sync_ch):
        process(x);
    case send(&other_sync_ch, 42):
        // sent
}
```

(Detailed semantics in advanced section.)

---

#### 6.4.4 Type Signatures Document Intent

Function signatures make clear what context is required:

```c
// Clearly async
@async void!Error async_reader(int[~ <] ch) {
    int x = await recv(&ch);  // obvious: must await
    return cc_ok(x);
}

// Clearly sync (blocks)
void sync_worker(int[~ sync <] requests) {
    int req = recv(&requests);  // obvious: blocks
    process(req);
}

// Caller knows exactly what to do based on channel type
```

**Benefit:** No surprises during refactoring. Change a function to `@async` and the compiler immediately tells you what operations need `await`.

---

#### 6.4.5 Refactoring Safety

When refactoring a sync function to async, the compiler enforces correctness:

```c
// Original: sync
void sync_handler(int[~ sync <] requests) {
    int req = recv(&requests);
    process(req);
}

// Refactored to async with wrong channel type:
@async void async_handler(int[~ sync <] requests) {
    // int req = recv(&requests);  // ❌ ERROR: cannot await, but needs to
    // This won't compile—we need to change the channel type
}

// Refactored correctly:
@async void async_handler(int[~ <] requests) {
    int req = await recv(&requests);  // ✅ CORRECT
    process(req);
}
```

The compiler forces you to fix the channel type when refactoring. No silent behavior changes.

---

#### 6.4.6 Error Handling

**Async channels:**

```c
int[~ <]? x = await recv(&rx);
if (!x) {
    // Channel closed and drained
}

// With error values
int!Error[~ <]? x = await recv(&error_rx);
if (try int val = *x) {
    process(val);
} else if (is_none(*x)) {
    // Channel closed
}
```

**Sync channels:**

```c
int[~ sync <]? x = recv(&rx);
if (!x) {
    // Channel closed and drained
}
```

Same error handling semantics; only difference is blocking vs suspending.

---

#### 6.4.7 Comparison Table

| Aspect | Async handles (`int[~ ... >]` / `int[~ ... <]`) | Sync handles (`int[~ ... sync ... >]` / `int[~ ... sync ... <]`) |
|--------|---|---|
| **Must await** | Yes, always | No, never |
| **Blocks OS thread** | No | Yes |
| **Use in `@async` code** | Yes (primary) | No (use async instead) |
| **Use in sync code** | No (use task) | Yes (primary) |
| **Works with `@match`** | Yes | No (`@match_blocking` rare) |
| **Implicit cancel case** | Yes | No |
| **Example use** | Async streams, work queues in nurseries | Thread coordination, OS thread pools |

---

#### 6.4.8 Real-World Patterns

**Pattern 1: Producer-Consumer (Async)**

```c
int[~ >] work_tx;
int[~ <] work_rx;
channel_pair(&work_tx, &work_rx);

@async void producer() {
    for (int i = 0; i < 100; i++) {
        await send(&work_tx, i);
    }
}

@async void consumer() {
    @match {
        case int work = await recv(&work_rx):
            process(work);
        // implicit cancel case
    }
}

@nursery closing(work_tx) {
    spawn (producer());
    spawn (consumer());
}
```

**Pattern 2: Thread Pool (Sync)**

```c
int[~ sync >] requests_tx;
int[~ sync <] requests_rx;
channel_pair(&requests_tx, &requests_rx);

int[~ sync >] responses_tx;
int[~ sync <] responses_rx;
channel_pair(&responses_tx, &responses_rx);

void worker_thread() {
    while (true) {
        int req = recv(&requests_rx);  // blocks
        int resp = process(req);
        send(&responses_tx, resp);  // blocks
    }
}

void main() {
    Thread t = spawn_thread(worker_thread);
    
    // Send request
    send(&requests_tx, 42);
    
    // Get response
    int resp = recv(&responses_rx);
    
    t.join();
}
```

No `await` anywhere in worker_thread. Blocks OS thread as expected.

**Pattern 3: Async with Timeout (using cancellation)**

```c
@nursery {
    spawn (reader(requests));
    spawn (timeout_enforcer());
}

@async void!Error reader(int[~ <] ch) {
    @match {
        case int x = await ch.recv():
            return cc_ok(x);
        // implicit cancel case (timeout will cancel)
    }
}

@async void!Error timeout_enforcer() {
    @match {
        case await sleep(Duration{5, 0}):
            return cc_err(Error.Timeout);
        // implicit cancel case
    }
}
```

No manual polling or timeouts in reader. Cancellation handles it.

---

#### 6.4.9 Migration from Dual-Mode

**Before (confusing, legacy combined channel):**
```c
int[~] ch;
int x = recv(&ch);  // What does this do? Depends on context!
```

**After (clear):**
```c
int[~ >] tx;
int[~ <] rx;
channel_pair(&tx, &rx);
int x = await recv(&rx);  // Async receive handle, must await (always)

int[~ sync >] stx;
int[~ sync <] srx;
channel_pair(&stx, &srx);
int x = recv(&srx);       // Sync receive handle, no await (always)
```

Each channel type has one clear set of operations. No context-dependent surprises.

---

### 7.5 Cancellation & Deadline

Cancellation is **cooperative but effective**. Tasks respond to cancellation by one of two mechanisms:

1. **Implicit cancellation case in `@match`** — automatic, zero-overhead (PRIMARY)
2. **Cancellation-aware operation variants** — explicit opt-in for non-select cases (SECONDARY)

This gives cancellation "teeth"—a cancelled task will exit immediately when it reaches a cancellation-aware operation, without requiring manual polling or timeouts everywhere.

---

#### 6.5.1 Implicit Cancellation Case in `@match`

**Rule (@match requires async channels):** `@match` works **only with async channel handles** (`T[~ ... >]` / `T[~ ... <]` or `T[~ ... async ... >/<]`). Sync handles (`T[~ ... sync ... >/<]`) do not support `@match`. If you need to multiplex sync channels, use low-level primitives outside of `@match` (rare).

**Rule (implicit cancel case):** When `@match` appears inside a cancellable context (nursery, spawned task, or any task that can be cancelled), the compiler implicitly adds a cancellation case:

```c
@async void!Error worker(int[~ <] ch) {  // async recv handle
    while (true) {
        @match {
            case int x = ch.recv():    // async channel recv, naturally awaitable
                process(x);
            // IMPLICIT (inside cancellable context):
            // case is_cancelled():
            //     return cc_err(Cancelled);
        }
    }
}

// Sync channels don't work with @match:
@async void!Error bad_sync(int[~ sync <] ch) {
    @match {
        case int x = ch.recv():  // ❌ ERROR: @match requires async channel
            process(x);
    }
}
```

**How it works:**
- `@match` checks all cases, including the implicit cancellation case
- If the task is cancelled and the cancel case can proceed, the `@match` immediately returns `err(Cancelled)`
- The task must have a `Cancelled` variant in its error type for implicit return to work
- If no `Cancelled` variant, the implicit case is a compile error

**Desugaring:**

```c
@async void!Error worker(int[~] ch) {
    while (true) {
        @match {
            case int x = ch.recv():
                process(x);
            case is_cancelled():  // implicit
                return cc_err(Error.Cancelled);
        }
    }
}
```

**Rule (cancellation-aware error types):** For tasks in a nursery that can be cancelled, define the error type with a `Cancelled` variant:

```c
enum WorkerError {
    Cancelled,      // ✅ allows implicit cancel case
    IoError(...),
    Timeout,
}

@async void!WorkerError worker() {
    @match {
        case result = async_io():
            return cc_ok(result);
        // implicit: case is_cancelled(): return cc_err(WorkerError.Cancelled);
    }
}
```

**Rule (implicit case only in cancellable contexts):** The implicit cancellation case is only added when:
- Inside an `@async` function that is spawned in a nursery
- Inside a task with a `Cancelled` error variant
- At a `@match` statement (not other await operations)

Non-cancellable tasks (e.g., main thread, task not in a nursery) do not get implicit cancel cases.

**Benefit:** Cancelled tasks immediately exit when they hit a `@match`, without requiring manual `is_cancelled()` polls or timeout workarounds.

---

#### 6.5.2 Cancellation-Aware Operation Variants

For operations **outside `@match`** (single await, channel recv/send, sleep), use explicit cancellation-aware variants.

**Guidance:** Inside an active `with_deadline(...)` scope, suspension points are already cancellation-aware per **§ 3.2.2**, so cancellation-aware variants are usually redundant. They remain useful outside deadline scopes (e.g., when responding to explicit task cancellation or nursery sibling cancellation without introducing an artificial deadline scope).

```c
// Channels
T!Cancelled? recv_cancellable(T[~ <]* rx);      // returns err(Cancelled) if cancelled
bool send_cancellable(T[~ >]* tx, T value);     // returns false if cancelled

// Tasks
T!Cancelled await task_cancellable<T>(Task<T!Cancelled> t);

// Sleep
Task<void!Cancelled> sleep_cancellable(Duration d);

// Timing
Task<T!Cancelled> with_timeout_cancellable<T>(Task<T!Cancelled> t, Duration d);
```

**Example (non-select context):**

```c
@async void!Error reader(int[~] ch) {
    while (true) {
        int!Cancelled? x = await ch.recv_cancellable();
        
        if (try err = x) {
            if (err == Cancelled) return cc_ok(());  // task was cancelled
            return cc_err(err);
        }
        
        process(*x);
    }
}
```

**Rule:** Cancellation-aware variants return `err(Cancelled)` (or equivalent error) if the task is cancelled, allowing the caller to decide how to respond. This is strictly opt-in—use the variant only if you want to observe cancellation.

**When to use variants:**
- Outside `@match` (single await, sleep, recv)
- When you need to distinguish cancellation from other completion reasons
- When you want explicit control over cancellation handling

---

#### 6.5.3 Polling-Based Fallback

For cases where neither implicit `@match` nor variants apply, use the polling-based API:

```c
@async void!Error work() {
    while (!cc_is_cancelled()) {
        int x = await long_operation();
        process(x);
    }
    return cc_err(Cancelled);
}
```

This is still supported but should be rare—most code uses `@match` or variants.

---

#### 6.5.4 Cancellation Semantics

**Rule:** Cancellation is **fully cooperative**. No task is forcibly interrupted.

- `t.cancel()` sets a cancellation flag on the task
- The flag is observable via:
  - Implicit case in `@match` (immediate)
  - `recv_cancellable()` / `send_cancellable()` (immediate)
  - `cc_is_cancelled()` (polling, manual)
- No async context unwinding or stack unwinding
- Outside an active `with_deadline(...)` scope, non-cancellation-aware awaits are unaffected (e.g., plain `await ch.recv()` keeps waiting). Inside `with_deadline(...)`, suspension points are cancellation-aware per **§ 3.2.2** (and may be deferred by `with_shield`, **§ 7.5.2**).

**Rule:** `t.cancel()` is only valid if `t` is a task with a `Cancelled` error variant. Attempting to cancel a task without `Cancelled` in its error type is a compile error.

**Rule (nursery cancellation propagation):** When a task in a nursery fails or is cancelled, the nursery cancels all sibling tasks. Siblings that use `@match` or cancellation-aware variants will observe the cancellation immediately. Siblings using plain `await` will continue waiting until they return or reach a cancellation-aware operation (unless they are inside an active `with_deadline(...)` scope, where suspension points are cancellation-aware per **§ 3.2.2**).

---

#### 6.5.5 Real-World Pattern: Timeout Enforcer

```c
enum WorkerError {
    Cancelled,
    Timeout,
    IoError(IoError),
}

@nursery {
    spawn (reader(requests));
    spawn (writer(responses));
    spawn (timeout_enforcer());
}

@async void!WorkerError reader(int[~ <] requests) {
    while (true) {
        @match {
            case int req = requests.recv():
                handle_request(req);
            // implicit: case is_cancelled(): return cc_err(Cancelled);
        }
    }
}

@async void!WorkerError writer(int[~ >] responses) {
    for (Response r : response_queue) {
        @match {
            case responses.send(r):
                // sent
            // implicit: case is_cancelled(): return cc_err(Cancelled);
        }
    }
}

@async void!WorkerError timeout_enforcer() {
    @match {
        case sleep(Duration{5, 0}):
            return cc_err(WorkerError.Timeout);
        // implicit: case is_cancelled(): return cc_ok({});
    }
}
```

**What happens:**
1. Timeout enforcer wakes after 5 seconds and returns `err(Timeout)`
2. Nursery cancels reader and writer (error propagation)
3. Both reader and writer hit their implicit cancel cases in `@match`
4. All three tasks exit, nursery completes with timeout error

**No hanging tasks. No timeouts needed in reader/writer. Clean cancellation propagation.** ✅

---

#### 6.5.6 Guarantees and Limitations

**What cancellation guarantees:**
- ✅ Tasks will exit when they reach a cancellation-aware operation
- ✅ Siblings in a nursery are cancelled when one fails
- ✅ Error propagates automatically to the nursery
- ✅ No explicit polling needed (use @match)

**What cancellation does NOT guarantee:**
- ❌ Preemption of blocking operations (non-cancellation-aware awaits keep waiting)
- ❌ Stack unwinding (no destructors or cleanup on cancel)
- ❌ Automatic timeout (use `with_timeout` if needed)
- ❌ Immediate exit (task exits when it reaches a cancellation point)

**Pattern for guarantee:** Use `@match` with cancellation-aware operations to ensure tasks can be cancelled promptly.

---

#### 6.5.7 Error Type Design

**Recommendation:** When designing tasks for a nursery, include `Cancelled` in the error type:

```c
// Good: @match implicit cases work
enum TaskError {
    Cancelled,              // ✅ allows implicit cancel
    Timeout,
    IoError(IoError),
}

@async void!TaskError worker() {
    @match {
        case int x = ch.recv():
            return cc_ok(x);
        // implicit: case is_cancelled(): return cc_err(TaskError.Cancelled);
    }
}

// Bad: no Cancelled variant, so implicit case can't work
@async void!IoError reader() {
    int x = await ch.recv();  // ❌ can't be cancelled effectively
    return cc_ok(x);
}
```

---

#### 6.5.8 Comparison: Implicit vs Explicit

| Scenario | Use |
|----------|-----|
| Channel recv/send + other waits | `@match` (implicit cancel) |
| Single operation outside select | `recv_cancellable()` or variant |
| Edge case fallback | `is_cancelled()` polling |

**Default:** Use `@match` (simplest, lowest overhead)

---

### 7.5.1 Deadline Primitive (Timeout Abstraction)

For request handling and other time-bounded operations, a simple `Deadline` type makes timeout patterns idiomatic and consistent.

**Type (minimal):**

```c
// Opaque type: stores absolute deadline timestamp (nanoseconds since epoch)
typedef struct Deadline Deadline;
```

**Factory Functions (in <std/time.cch>):**

```c
// Create deadline relative to now
Deadline deadline_after(Duration d);

// Create deadline at absolute timestamp
Deadline deadline_at(u64 timestamp_ns);

// Check if deadline has been exceeded
bool deadline_exceeded(Deadline d);

// Get remaining time (0 if exceeded, negative means already passed)
Duration deadline_remaining(Deadline d);
```

**Usage Pattern:**

```c
@async Response!IoError http_handler(Request* req, Arena* req_arena) {
    Deadline deadline = deadline_after(seconds(5));
    
    // Wrap deadline-sensitive operations
    with_deadline(deadline) {
        char[:] parsed = parse(req.body);
        DbResult result = try await db_query(parsed);
        Response resp = build_response(result, req_arena);
        try await send_response(req.fd, &resp);
    }
    // On deadline exceeded: with_deadline propagates cancellation
}

// Or on individual operations:
// (Optional) A standard-library helper MAY provide per-operation timeout sugar,
// but it is not part of the core language surface. The canonical form is
// `with_deadline(...) { ... }` around the relevant region.
```

**Semantics:**

- `with_deadline(d) { ... }` executes the block; if deadline exceeded, propagates cancellation
- Deadline exceeded triggers same cancellation mechanism as explicit cancellation
- Cancellation is **cooperative** (tasks exit at awaitable points)

**Deadline propagation (clarification):**

- A `with_deadline(d)` scope establishes a deadline that applies to all **descendant tasks created within the scope**.
- If the deadline expires, cancellation is requested for all tasks within the scope (and descendants).
- Cancellation is observed according to **§ 3.2.2** (and may be deferred by shielded regions; see **§ 7.5.2**).

**Design Notes:**

- Deadline is **just a timestamp** (u64 nanoseconds); no allocation
- No separate timeout type; Deadline is the unification
- Works with `@match` implicitly (cancelled operations fail)
- Idiomatic for request-scoped deadlines (every request gets one)

---

### 7.5.2 Shielded Regions (`with_shield`)

A **shielded region** temporarily suppresses observation of cancellation originating from enclosing deadline scopes for the duration of the region.

**Syntax:**

```c
with_shield {
    /* statements */
}
```

**Semantics:**

- While executing inside a `with_shield` region, suspension points MUST NOT observe cancellation requested by enclosing `with_deadline(...)` scopes.
- Explicit cancellation requests originating inside the shielded region (e.g., calling `cc_cancel()` from within the region) MUST still be observable.

**Exit rule:**

- Upon exiting a `with_shield` region, if cancellation has been requested, the next suspension point MUST observe it immediately (per **§ 3.2.2**).

**Constraints:**

- Shielded regions MUST NOT be unbounded.
- Shielded regions are intended for short, bounded cleanup operations (e.g., protocol shutdown, flushing buffers).
- Long-running or unbounded loops inside `with_shield` are ill-formed and SHOULD be rejected by static analysis.

---

### 7.5.3 Cancel-Safe API Surface (Recommended Contracts)

This subsection defines minimal vocabulary for documenting async APIs in the presence of cancellation at suspension points (**§ 3.2.2**).

**Cancellation-safe operation (contract):**

An `@async` operation is **cancellation-safe** if, when it returns `err(Cancelled)` at any suspension point during its execution, it leaves the program state:

- memory-safe
- resource-safe (no leaked ownership obligations)
- invariant-preserving for the operation’s abstraction boundary

Partial work MAY occur; if externally visible, it MUST be consistent and documented.

**Cancellation-unsafe operation (contract):**

An `@async` operation is **cancellation-unsafe** if observing cancellation mid-operation can leave externally visible state such that the caller lacks a well-defined recovery procedure (e.g., half-finished protocol state, transient invariants, ambiguous commit).

**Documentation tags (recommended):**

Async APIs SHOULD be documented as `@cancel_safe` or `@cancel_unsafe` (documentation metadata; not language syntax).

**Defaulting rule (recommended):** Standard library `@async` operations SHOULD be assumed `@cancel_safe` unless explicitly documented as `@cancel_unsafe`.

If an API is `@cancel_unsafe`, its documentation MUST specify:

- commit points (what may already have happened before cancellation is observed)
- required recovery action on cancellation (e.g., close/reset/retry)
- any externally visible partial state that may remain

**Lint rule (recommended):**

Tooling SHOULD warn/error when a `@cancel_unsafe` operation is awaited inside an active `with_deadline(...)` scope unless it is awaited within a `with_shield { ... }` region (or the API’s documented recovery action is performed on cancellation).

### 7.7 Streaming

Streaming uses explicit channel parameters:

```c
@async void produce(int n, int[~ >]* out) {
    defer out.close();  // close is only valid on send handles
    for (int i = 0; i < n; i++) {
        await out.send(i);
    }
}

@async void consume() {
    int[~10 >] tx;
    int[~10 <] rx;
    channel_pair(&tx, &rx);
    @nursery closing(tx) {
        spawn (produce(100, &tx));
        @for await (int x : rx) use(x);
    }
}
```

**Streaming with errors:**

```c
// Fail-fast: function can fail, channel carries plain values
@async void!IoError read_lines(char[:] path, char[:][~]* out) {
    defer out.close();
    File f = try open(path);
    while (true) {
        char[:]? line = try f.readline();
        if (!line) break;
        await out.send(*line);
    }
}

// Per-item errors: each item can independently fail
@async void parse_nums(char[:][~]* in, int!ParseError[~]* out) {
    defer out.close();
    @for await (char[:] line : in) {
        await out.send(parse_int(line));
    }
}
```

---

### 7.8 Runtime API

```c
// Internal Scope (created implicitly in @async functions for batching/wrapping)
struct Scope {
    void spawn(expr);                              // (internal) spawn async work
    @async T!E run_blocking<T, E>(() => T!E);  // (internal) run closure on thread pool
    void cancel();                                 // (internal) cancel all spawned work
    bool cancelled();                              // (internal) check cancellation
}

// Task control
Task<T> task = async_fn(args);         // lazy, not started
void   Task<T>.start();                // begin execution (detached)
T      await Task<T>;                  // suspend until complete
void   Task<T>.cancel();               // request cooperative cancellation

// Cancellation-aware variants (observe cancellation when awaited)
T!Cancelled      await task_cancellable<T>(Task<T!Cancelled> t);
T!Cancelled?     recv_cancellable<T>(T[~ <]* rx);      // returns err(Cancelled) if cancelled
bool             send_cancellable<T>(T[~ >]* tx, T value);
Task<void!Cancelled> sleep_cancellable(Duration d);
Task<T!Cancelled> with_timeout_cancellable<T>(Task<T!Cancelled> t, Duration d);

// Cancellation polling (valid only in @async functions)
bool   is_cancelled();

// Timing
Task<void> sleep(Duration d);
Task<T!E> with_timeout<T, E>(Task<T!E> t, Duration d);

// Sync bridge (can block)
T block_on<T>(Task<T> t);    // run task to completion, blocking

// Duration literals
1ns, 1us, 1ms, 1s, 1m, 1h
```

**Rule:** `with_timeout` requires error type `E` to have a `Timeout` variant.

**Rule:** `is_cancelled()` is only valid inside `@async` functions.

**Rule:** `block_on` can block the OS thread. It is most commonly used at sync boundaries outside of `@async` code. For sync-to-async bridging within `@async` code, use implicit nursery wrapping or structured concurrency patterns.

**Rule (block_on re-entrancy):** `block_on()` must not be called from within a `run_blocking` worker thread (i.e., from a non-`@async` function that was auto-wrapped inside `@async` code) or from any thread currently executing runtime-managed tasks. Violation causes deadlock or scheduler re-entrancy.

**Detection and behavior:**
- **Debug builds:** Runtime detects re-entrant `block_on` and traps with diagnostic error.
- **Release builds:** Undefined behavior (likely deadlock or scheduler corruption).

**Example (WRONG):**

```c
@async void f() {
    g();            // non-@async → auto-wrapped in run_blocking
}

void g() {
    Task<int> t = async_work();
    int result = block_on(t);   // ERROR: block_on called from threadpool thread!
}
```

**Example (RIGHT):**

```c
@async void f() {
    // Option 1: avoid block_on entirely by using await
    int result = await async_work();  // preferred
    
    // Option 2: move block_on to sync boundary
    // (don't call block_on from inside @async, even via nesting)
}

// CORRECT: call block_on from sync context (not inside @async or run_blocking)
void sync_boundary() {
    Task<int> t = async_work();
    int result = block_on(t);  // OK: called from actual sync context
    use(result);
}
```

---

### 7.7.1 Blocking Thread Pool

Certain operations may stall indefinitely (I/O, locks, OS calls). These run on a bounded thread pool to avoid blocking the async scheduler.

**Blocking-class operations:**
- File I/O: `File.read()`, `File.write()`, `File.sync()`
- Mutex lock: `Mutex<T>.lock()` (when contended)
- OS operations: `sleep()`, `join()`, system calls

**Non-blocking operations (run inline, no pool):**
- All pure computation: `trim()`, `split()`, `parse_i64()`, `push()`, `insert()`
- All UFCS methods on slices and collections

**Pool limits:**

```c
Runtime.set_blocking_pool(
    .max_threads = 32,
    .max_queue = 1000
);
```

- `max_threads`: number of worker threads (default: 2× CPU count)
- `max_queue`: maximum pending operations (default: 1000)

**Saturation behavior:**

When the queue is full, blocking operations return `IoError::Busy`:

```c
enum IoError {
    PermissionDenied,
    FileNotFound,
    InvalidArgument,
    Interrupted,
    OutOfMemory,
    Busy,                // Pool queue is full
    Other(i32 os_code),  // Platform error code
};
```

**Example:**

```c
@async void process_with_backoff() {
    File! f = file_open(&arena, path, "r");
    if (try File file = f) {
        int retry_count = 0;
        while (char[:]? !IoError line_result = file.read_line(&arena)) {
            if (try char[:]? line_opt = line_result) {
                if (!line_opt) break;  // EOF (Ok(None))
                char[:] line = *line_opt;
                
                // ... process line ...
                
                retry_count = 0;  // reset on success
            } else {
                // Error (not EOF) - handle backoff
                if (line_result is Busy) {
                    if (++retry_count > 3) {
                        return cc_err(IoError::Busy);
                    }
                    await sleep(milliseconds(10 * retry_count));
                }
            }
        }
        file.close();
    }
}
```

**Design:**

- Failure is observable (not silent queuing)
- App controls backoff strategy via explicit retry
- CPU-only work avoids pool overhead
- Pool stats available for monitoring/alerting

**No eviction:** In-flight operations are never killed. Only fail on queue saturation before queueing.

---

### 7.7.2 Standard Error Types

The runtime and stdlib define standard error types used across the standard library:

```c
// I/O operations
enum IoError {
    PermissionDenied,
    FileNotFound,
    InvalidArgument,
    Interrupted,
    OutOfMemory,
    Busy,                // Blocking pool saturated
    Other(i32 os_code),  // Platform error code
};

// String parsing
enum ParseError {
    InvalidUtf8,
    Truncated,
};

// Bounds checking
enum BoundsError {
    OutOfBounds,
};
```

These enums are used in stdlib operations like `File.read()`, `char[:].parse_i64()`, and `Vec<T>.set()`. Applications may compose multiple error types using wrapper enums (see § 7.5 Multiple error types).

---

### 7.9 Execution Model (Normative)

This section defines how Concurrent-C classifies potentially blocking operations and how they interact with `@async` execution.

#### 7.9.1 Definitions

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

#### 7.9.2 Default Classification Rule

All non-`@async` functions are conservatively treated as potentially blocking.

This includes:
- user code
- library code
- and foreign function calls

**Rule:** This classification applies transitively — if a function calls a potentially blocking function, it is itself potentially blocking.

#### 7.9.3 @async Execution Rule

Calling a non-`@async` function from within an `@async` function must not block the async scheduler.

To satisfy this rule:
- The compiler or runtime must automatically wrap such calls using a blocking execution mechanism (e.g., `run_blocking`).
- Multiple consecutive non-`@async` calls **may** be coalesced into a single blocking dispatch.
- The wrapping mechanism is an implementation detail and is not observable at the language level.

**Coalescing Semantics:** Consecutive non-`@async` calls within the same lexical scope may be dispatched as one blocking unit. If an error occurs (exception, early return, propagated error), remaining calls in the unit are not executed.

#### 7.9.3.1 @latency_sensitive Annotation

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

#### 7.9.3.2 Linting Rule for @latency_sensitive

The compiler (translator) enforces a lint rule to catch latency violations:

**Rule:** `@latency_sensitive` functions can only call:
- `@noblock` functions (guaranteed non-blocking, inline)
- `@async` functions (must be awaited)
- Any function within `await` context

**Violations (Compiler Warning/Error):**

Calling a non-`@async`, non-`@noblock` function without `await` in a `@latency_sensitive` function is a compiler error or warning (depending on lint level).

**Example:**

```c
@noblock int parse_count(char[:] s);    // OK to call directly

@async void db_query(int count);        // Must be awaited

void process_logs(int count);           // Must be awaited or marked @noblock

@async @latency_sensitive void handler(Request req) {
    int count = parse_count(req.body);  // ✅ OK (@noblock, guaranteed fast)
    
    try await db_query(count);          // ✅ OK (awaited)
    
    process_logs(count);                // ❌ ERROR: blocking call in latency_sensitive
    
    // Fix: Either await it or mark it @noblock
}
```

**Rationale:** Prevents accidental blocking calls that violate the latency guarantee.

#### 7.9.4 Blocking Executor Constraints

Blocking work is executed on a bounded blocking executor.

**Normative Requirements:**
- The executor must be bounded.
- Saturation must not deadlock the async runtime.
- If work cannot be scheduled due to saturation, the operation must fail deterministically.

**Saturation Behavior:**
- When the queue reaches `max_queue` capacity, new operations return `IoError::Busy` immediately without queueing.
- Work already queued or in-flight continues to completion.
- The queue is FIFO; starvation is possible under sustained saturation.

#### 7.9.5 Stall Awareness

Operations that may stall indefinitely must be explicitly classified as such.

**Stalling Operations (by definition):**
- file open/read/write/sync
- stream reads
- any OS or FFI operation whose completion depends on external actors

**Guarantees:**
- Stalling operations may be offloaded to the blocking executor
- May fail with `IoError::Busy` if capacity is exhausted
- Have no guarantee of cancellation or bounded latency

#### 7.9.6 @noblock Contract

A function annotated `@noblock` asserts that it will never block or stall.

**Rules:**
- `@noblock` functions must not perform I/O, synchronization waits, or call non-`@noblock` functions
- The compiler must not wrap calls to `@noblock` functions when invoked from `@async`
- Violations detected at compile-time are errors

**Runtime Violations:**
- Debug builds: runtime trap with diagnostic
- Release builds: undefined behavior (likely deadlock or latency spike)

This annotation exists to allow high-confidence opt-out from conservative blocking assumptions.

#### 7.9.7 Standard Library Guarantees

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

#### 7.9.8 Cancellation and Progress

Blocking and stalling operations provide no cancellation or progress guarantees.

**Specifically:**
- Cancellation requests are signals only; they do not forcibly preempt in-flight work
- In-flight blocking work may:
  - complete normally after cancellation is requested
  - fail with an error unrelated to cancellation
  - continue running on the blocking executor thread even after the task is cancelled
- Programs requiring strict latency bounds must avoid stalling operations in critical paths and may need explicit timeouts (e.g., `with_timeout_cancellable`)

#### 7.9.9 Interaction with Nurseries (Non-Normative)

When a spawned task stalls on I/O, the nursery continues scheduling other work. The nursery scope does not complete until all spawned tasks complete (including any offloaded blocking work). Nursery cancellation requests are propagated to in-flight work but provide no hard latency guarantees (§ 7.8.8).

#### 7.9.10 Design Intent (Non-Normative)

The blocking model is intentionally conservative:
- Safety is the default
- Latency control is explicit
- The distinction that matters is bounded vs unbounded wait, not "sync vs async"
- The primary escape hatch is truthful use of `@noblock`, not suppression of safety checks

---



**Deadlock detection:**

```c
// Debug runtime maintains:
// - Count of runnable tasks
// - Count of tasks blocked on channel operations
// - Wait graph: (task_id, channel_id, operation, timestamp)
```

**Rule (deadlock trap):** If there are tasks blocked on channel operations and no runnable tasks, the runtime triggers a deadlock trap with a diagnostic report.

**Rule:** Deadlock detection is debug-only. Release builds do not include this overhead.

---

## 8. Standard Library (UFCS-First Design)

This section defines the core standard library using **UFCS-first design**: method syntax is primary and ergonomic, free functions are normative.

**Design principle:** The free function form is definitive (for generic code, composition, functional programming). UFCS method syntax desugars to free functions and is the primary API for most users.

**UFCS Equivalence (Normative):**

Both forms are fully equivalent and compile identically:
- `x.method(args)` is syntactic sugar for `method(x, args)` (when `x` is a pointer or struct)
- `x.method(args)` with value `x` desugars to `method(&x, args)` (taking a pointer)
- The compiler treats both forms interchangeably

This enables two usage styles:

```c
// UFCS style (primary for most users; chains naturally)
char[:] result = input.trim().lower(arena);
size_t len = result.len();

// Free function style (preferred for composition and generic code)
char[:] result = lower(trim(input), arena);
size_t len = len(result);

// Both styles can mix freely
char[:] trimmed = input.trim();              // UFCS
size_t sz = len(trimmed);                    // Free function
char[:] final = lower(trimmed, arena);       // Free function
```

Functional composition becomes natural with free functions:

```c
// Map over a vector using free functions
int[] squared = vec_map(numbers, (int x) => x * x);

// Chain via free functions
Vec<int> result = vec_map(vec_filter(input, is_even), double);
```

---

### 8.1 Strings

**Type:** `str` — Immutable owned string

```c
struct str {
    char* ptr;
    size_t len;
    size_t cap;
}
```

Allocations come from the current arena. Strings have move semantics (copyable by default if the arena stays valid).

#### 8.1.1 Core Methods

```c
// Normative (free function form)
size_t len(str* s);
char at(str* s, int index);                  // bounds-checked, debug error on fail
str slice(str* s, int start, int end);      // substring [start, end)
char[:] as_slice(str* s);                   // view as char[:]

// UFCS methods (primary for users)
size_t  s.len();
char    s.at(int index);
str     s.slice(int start, int end);
char[:] s.as_slice();
```

Example:
```c
str s = "hello world";
size_t len = s.len();                    // 11
char first = s.at(0);                    // 'h'
str sub = s.slice(0, 5);                 // "hello"
char[:] view = s.as_slice();             // view
```

#### 8.1.2 Query Methods

```c
// Normative
bool is_empty(str* s);
bool is_ascii(str* s);
bool starts_with(str* s, str prefix);
bool ends_with(str* s, str suffix);
bool contains(str* s, str needle);
int index_of(str* s, str needle);        // -1 if not found
int last_index_of(str* s, str needle);
int count_occurrences(str* s, str needle);

// UFCS
bool s.is_empty();
bool s.is_ascii();
bool s.starts_with(str prefix);
bool s.ends_with(str suffix);
bool s.contains(str needle);
int  s.index_of(str needle);
int  s.last_index_of(str needle);
int  s.count(str needle);
```

Example:
```c
if (url.starts_with("https://") && url.contains("example.com")) {
    process_url(url);
}

int idx = text.index_of("pattern");
if (idx >= 0) {
    str after = text.slice(idx + 7, text.len());
}
```

#### 8.1.3 Transform Methods (Return Owned)

```c
// Normative
str upper(str* s, Arena* a);             // a = NULL uses current arena
str lower(str* s, Arena* a);
str trim(str* s, Arena* a);              // both ends
str trim_left(str* s, Arena* a);
str trim_right(str* s, Arena* a);
str replace(str* s, str old, str new, Arena* a);
str replace_all(str* s, str old, str new, Arena* a);
str repeat(str* s, int times, Arena* a);
str reverse(str* s, Arena* a);

// UFCS (arena defaults to current)
str s.upper();
str s.lower();
str s.trim();
str s.trim_left();
str s.trim_right();
str s.replace(str old, str new);
str s.replace_all(str old, str new);
str s.repeat(int times);
str s.reverse();
```

Example (with chaining):
```c
str result = input
    .trim()
    .lower()
    .replace("foo", "bar")
    .slice(0, 10);
    // Can't chain right now without explicit `await` but this shows intent
```

#### 8.1.4 Split / Join

```c
// Normative
str[:] split(str* s, str delimiter, Arena* a);
str join(str[:] parts, str separator, Arena* a);

// UFCS
str[:] s.split(str delimiter);
str s.join(str separator);  // on str[:] array
```

Example:
```c
str line = "one,two,three";
str[:] fields = line.split(",");

for (str field : fields) {
    process(field);
}

str joined = fields.join(" | ");
```

#### 8.1.5 Parsing

```c
// Normative
int? parse_int(str* s);
f64? parse_f64(str* s);
bool? parse_bool(str* s);

// UFCS
int?  s.parse_int();
f64?  s.parse_f64();
bool? s.parse_bool();
```

Example:
```c
str num_str = "42";
if (try int val = num_str.parse_int()) {
    process(val);
}
```

#### 8.1.6 Formatting (Global)

```c
// Build formatted strings (use with Arena)
str format(Arena* a, str fmt, ...);      // varargs
str format(Arena* a, fmt_args args);     // structured

// Example
str msg = format(&arena, "Hello {}! Score: {}", name, score);
```

---

### 8.2 Slices with UFCS

**Type:** `T[:]` — Mutable view into contiguous data

#### 8.2.1 Core Methods

```c
// Normative
size_t len(T[:] s);
T at(T[:] s, int index);
T[:] slice(T[:] s, int start, int end);
T* ptr(T[:] s);

// UFCS
size_t  s.len();
T       s.at(int i);
T[:]    s.slice(int start, int end);
T*      s.ptr();
```

#### 8.2.2 Query Methods

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

#### 8.2.3 Mutation Methods

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

#### 8.2.4 Iteration

```c
// Standard range-for (already in Surface Syntax)
for (T x : slice) { ... }

// Enumeration (with index)
for (int i = 0; i < slice.len(); i++) {
    T item = slice.at(i);
}
```

---

### 8.3 Arrays

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

### 8.4 Numeric Types with Methods

Primitive numeric types get UFCS methods for common operations:

####

This section defines the error model:

- **§7.1 Returning errors** — constructing results
- **§7.2 `try` propagation** — early return on error
- **§7.3 `catch` handling** — local error handling
- **§7.4 Async + errors** — `try await` composition
- **§7.5 Multiple error types** — explicit conversion

---

Errors in Concurrent-C are **value-based**, not exceptions.

* `T!E` is the return type for functions that can fail.
* `try` unwraps success or propagates error.
* `catch` handles errors locally.
* No unwinding — `defer` always runs.

---

### 7.9 Returning errors

```c
int!IoError read_value(char[:] path) {
    File f = try open(path);
    return try f.read_int();
}

// Explicit construction
int!IoError x = cc_ok(42);
int!IoError y = cc_err(IoError.FileNotFound);
```

---

### 7.10 `try` propagation

`try expr` unwraps `T!E` on success, or returns `err(e)` from the enclosing function on failure.

```c
int!IoError parse_file(char[:] path) {
    char[:] content = try read_file(path);
    return try parse_int(content);
}
```

**Rule:** `try` is valid when the enclosing function returns `U!E` with a compatible error type.

---

### 7.11 `catch` handling

`catch` converts a `T!E` to `T` by providing a fallback value or handler.

```c
// Default value on error
int x = parse_int(s) catch 0;

// Handle error explicitly (handler must yield T)
int y = parse_int(s) catch(e) {
    log(e);
    -1   // yields -1 (last expression is the value)
};
```

**Rule:** A `catch` handler must yield a value of type `T`. It cannot propagate errors. To selectively propagate, use `try` instead:

```c
// To re-propagate certain errors, use try + catch in caller
int!IoError read_or_default(char[:] path) {
    File f = try open(path);           // propagates IoError
    return f.read_int() catch 0;       // catch ParseError, yield 0
}

// Or handle everything explicitly
int z = read_int(path) catch(e) {
    if (e == IoError.FileNotFound) 0       // yield 0 for not-found
    else panic("unexpected error")     // or crash
};
```

**Rule:** The `catch` expression has type `T`, not `T!E`. After `catch`, the error has been handled.

---

#### 7.11.1 Try/Catch Blocks (Multiple Error Handling)

`try { block }` with multiple `catch` clauses handles errors within a block and dispatches to specific handlers based on error type.

```c
// Single catch clause
try {
    result = try db.query(sql);
    process(result);
} catch (SqlError e) {
    log("SQL error: %s", e.msg);
    return cc_err(e);  // or handle and return cc_ok(...)
}

// Multiple catch clauses (dispatched by type)
try {
    row = try db.query_row(sql);
    value = row.column_i64(0);  // may throw ParseError
} catch (SqlError e) {
    log("Database error");
    return cc_err(DatabaseError.QueryFailed);
} catch (ParseError e) {
    log("Parse error in column");
    return cc_err(DatabaseError.InvalidColumn);
}

// Catch and continue
try {
    process_item(item);
    return cc_ok();
} catch (SkippableError e) {
    log("Skipped: %s", e.reason);
    return cc_ok();  // Error handled; continue
} catch (FatalError e) {
    log("Fatal: %s", e.reason);
    return cc_err(e);  // Propagate fatal errors
}

// Nested try/catch for layered handling
try {
    try {
        data = try fetch_from_cache(key);
    } catch (CacheError e) {
        log("Cache miss");
        data = try fetch_from_network(key);  // Fallback: try network
    }
    return cc_ok(data);
} catch (NetworkError e) {
    log("All sources failed");
    return cc_err(e);
}
```

**Semantics:**
- `try { block }` executes the block
- If any `try expr` inside propagates an error, control transfers to the first matching `catch (Type var)` handler
- Error type matching is compile-time: only matching catch clauses are tried
- First matching catch clause executes; others are skipped (not fall-through)
- The catch handler can return `ok(...)`, `err(...)`, or propagate via `try`
- Multiple catch clauses are evaluated in order; first match wins

**Type safety:** If an error cannot be caught (no matching clause), it propagates to the enclosing function per `try` rules.

---

### 7.12 Async + errors

Async functions can return `T!E`:

```c
@async int!IoError fetch(char[:] url) {
    return try await http_get(url);
}

@async void!IoError process() {
    int len = try await fetch("http://...");
    use(len);
}
```

`try await` unwraps the task result, propagating errors.

---

### 7.13 Multiple error types

When composing functions with different error types, use explicit conversion with wrapper functions or inline error mapping:

```c
enum AppError { Io(IoError), Parse(ParseError) }

// Helper to convert error types
int!AppError parse_with_app_error(char[:] s) {
    int!ParseError r = parse_int(s);
    if (r.ok) return cc_ok(r.value);
    return cc_err(AppError.Parse(r.error));
}

int!AppError read_with_app_error(char[:] path) {
    char[:]!IoError r = read_file(path);
    if (r.ok) return cc_ok(r.value);
    return cc_err(AppError.Io(r.error));
}

int!AppError process(char[:] path) {
    char[:] s = try read_with_app_error(path);
    return parse_with_app_error(s);
}
```

**Rule:** Error type conversion is explicit. There is no implicit coercion from `T!E1` to `T!E2` even if `E1` can be embedded in `E2`.

---

## 8. FFI and Unsafe Operations

Because CC is a C preprocessor, **native C interop is first-class**. The entire C standard library and existing C code is immediately accessible. This section defines escape hatches for when CC's safety checks must be bypassed:

- **§8.1 `unsafe {}`** — bypassing compile-time checks (slice provenance, sendability)
- **§8.2 `adopt`** — adopting FFI allocations as CC slices

---

### 8.1 `unsafe {}`

`unsafe {}` bypasses compile-time checks for:

* slice provenance
* sendability enforcement

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

### 8.2 Adopting FFI Allocations

C APIs that return owned buffers can be adopted as unique slices:

```c
T[:] adopt<T>(void* ptr, size_t count, void (*deleter)(void*));
```

* Only valid inside `unsafe {}`.
* Produces a **unique** slice (move-only, has destructor).
* `count` is element count, not byte size.
* Deleter receives the original `ptr` and is called exactly once when ownership ends.
* `NULL` deleter is valid (no cleanup action).

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
    await ch.send(s);       // OK: copies data, s still valid
    await ch.send_take(s);  // ERROR: adopt() slices not transferable
}
// c_free called here
```

---

## 9. Surface Syntax Notes

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

`x.method(args)` is syntax sugar for `method(&x, args)` (value) or `method(x, args)` (pointer).

```c
tx.send(v);        // lowers to send(&tx, v)
tx.close();        // lowers to close(&tx)
slice.len;         // field access (not a call)
```

**Rule:** The free function form is normative. Method syntax is always desugaring.

**UFCS auto-deref:**

UFCS auto-dereferences one pointer level. If `p` has type `T*`, `p.method(v)` lowers to `method(p, v)`.

```c
int[~10 >]* tx_ptr = &tx;
tx_ptr.send(42);   // lowers to send(tx_ptr, 42)
tx_ptr.close();    // lowers to close(tx_ptr)
```

**Rule (UFCS in defer):** UFCS works uniformly in `defer` statements regardless of whether the receiver is a value or pointer:

```c
int[~10 >] tx;
defer tx.close();        // OK: lowers to close(&tx)

int[~10 >]* tx_ptr = get_tx();
defer tx_ptr.close();    // OK: lowers to close(tx_ptr)
```

**Loops:**

Traditional C `for(;;)` is unchanged.

```c
for (T x : slice) { ... }       // range-for over slice
@for await (T x : ch) { ... }    // async iteration over channel
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

**`@for await` lowering:**

```c
// @for await (T x : expr) { BODY }
// lowers to:
while (true) {
    T? __tmp = try await expr.next();
    if (!__tmp) break;     // Ok(None) indicates end-of-stream (EOF)
    T x = *__tmp;
    BODY
}
```

**Rules (async iteration):**

- `Ok(None)` indicates end-of-stream (EOF).
- Errors propagate normally.
- Suspension points inside async iteration are subject to **§ 3.2.2** and **§ 7.5.2**.

**`@for await` on pointers:**

`@for await (T x : c)` accepts `c` of type `T[~...]` or `T[~...]*`. Pointer form is implicitly dereferenced.

**Slicing:**

Subslice syntax creates views into existing slices:

```c
s[start..end]    // elements [start, end)
s[start..]       // elements [start, len)
s[..end]         // elements [0, end)
s[..]            // equivalent to s
```

**Rule:** Slice indexing and subslicing perform bounds checks in debug builds; out-of-bounds is a runtime error in debug and undefined behavior in release.

**String literals:**

String literals have type `char[:]` with static provenance:

```c
char[:] s = "hello";  // static slice, always valid
```

**Rule:** String literal slices are sendable and have `owner = NULL` (static provenance).

**Closures:**

Closures use arrow syntax with optional capture list:

```c
() => { stmt; }              // no parameters, implicit value capture
(x) => { stmt; }             // one parameter (type inferred)
(int x, int y) => { stmt; }  // typed parameters
x => expr                    // single parameter, expression body

// Explicit capture list (optional)
[x]() => { stmt; }           // explicit value capture (same as implicit)
[&x]() => { stmt; }          // reference capture (explicit sharing)
[x, &y]() => { stmt; }       // mixed: x by value, y by reference
```

**Capture semantics:**

- **Value capture (default):** Closures capture by value. For copyable types, the captured value is a copy. For move-only types, the capture is a move and the original becomes invalid.

- **Reference capture (`[&x]`):** Explicitly captures a reference to the outer variable. The closure shares the variable with the outer scope. Reference captures are subject to mutation checks (see below).

- **Capture-all banned:** The forms `[&]` and `[=]` (capture all by reference/value) are not allowed. Each captured variable must be listed explicitly.

**Reference capture mutation check:**

For thread/task closures, reference captures (`[&x]`) are checked for mutation:

- **Read-only access:** Allowed. The closure may read the referenced variable.
- **Mutation:** Compile error unless the type is a safe wrapper (`@atomic T`, `Atomic<T>`, `Mutex<T>`, channel handles), or the capture is inside `@unsafe`.

```c
int counter = 0;

// ✅ OK: read-only reference capture
spawn([&counter]() => { printf("%d", counter); });

// ❌ ERROR: mutation of shared reference
spawn([&counter]() => { counter++; });
// error: mutation of shared reference 'counter' in spawned task
// help: use Atomic<int>, Mutex<int>, or @unsafe [&counter]

// ✅ OK: safe wrapper
Atomic<int> safe_counter = atomic_new(0);
spawn([&safe_counter]() => { safe_counter++; });

// ⚠️ OK: explicit unsafe (you own this race)
spawn(@unsafe [&counter]() => { counter++; });
```

**Mutation patterns detected:**

| Pattern | Classification |
|---------|----------------|
| `x = ...` | Write (error) |
| `x++`, `++x`, `x--`, `--x` | Write (error) |
| `x += ...`, `-=`, `\|=`, etc. | Write (error) |
| `foo(&x)` where foo takes `T*` | Potential write (error) |
| `foo(&x)` where foo takes `const T*` | Read (OK) |
| `y = x`, `f(x)`, `x.field` | Read (OK) |

For thread/task closures, captured values must also be capturable (see §1.2).

**Type inference:**

`auto` infers the type from the initializer:

```c
auto x = 42;                // int
auto t = work();            // Task<T> where work returns @async T
auto it = iter(&m);         // MapIter<K, V>
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

**Generics:** See §10 for comprehensive generics documentation.

**Built-in generic types:**

* `Task<T>` — async task result
* `Mutex<T>` — OS mutex-protected shared state (operations can block)
* `LockGuard<T>` — RAII lock guard (from `Mutex<T>.lock_guard()`)
* `AsyncMutex<T>` — suspending mutex-protected shared state
* `AsyncGuard<T>` — RAII lock guard (from `AsyncMutex<T>.lock()`)
* `Atomic<T>` — lock-free atomic (primitives only)
* `Map<K, V>` — hash map
* `T[~... >]` / `T[~... <]` — channel handles for element type T

**Built-in non-generic types:**

* `ThreadGroup` — multi-thread coordination
* `Thread` — OS thread handle
* `Arena` — memory arena
* `Scope` — (internal) structured concurrency handle created implicitly in @async functions
* `Ordering` — memory ordering enum (`relaxed`, `acquire`, `release`, `acq_rel`, `seq_cst`)
* `RecvStatus` — non-blocking recv status enum (`WouldBlock`, `Closed`)
* `Duration` — time span (secs + nanos)

---

## 10. Generics

This section defines user-defined generic types and functions, with compile-time monomorphization and no trait system in v1.0.

- **§10.1 Syntax** — type parameters and value parameters
- **§10.2 Instantiation & monomorphization** — when code is generated
- **§10.3 Type inference** — how parameters are inferred
- **§10.4 Specialization** — `@comptime if` on types/values
- **§10.5 Semantics with ownership** — copy vs move through generics
- **§10.6 Restrictions (v1.0)** — what is intentionally missing

---

### 10.1 Syntax

Concurrent-C supports two kinds of generic parameters:

- **Type parameters:** `T`, `K`, `V`, etc.
- **Comptime value parameters:** compile-time integers/bools/etc used for sizes/layout.

**Generic function:**

```c
void swap<T>(T* a, T* b) {
    T tmp = *a;
    *a = *b;
    *b = tmp;
}
```

**Generic type (struct/enum):**

```c
enum Option<T> { None, Some(T) }

struct Pair<A, B> { A a; B b; }
```

**Comptime value parameters (for sizes/layout):**

```c
struct SmallVec<T, comptime int N> {
    T[N] inline_buf;
    int len;
}
```

**Parameter grammar (surface):**

```
generic_params := '<' generic_param (',' generic_param)* '>'
generic_param  := ident
               | 'comptime' type ident
```

- A type parameter is an identifier with no prefix: `T`.
- A value parameter is introduced with `comptime <type> <name>`: `comptime int N`.

**Rule:** A `comptime` value parameter is always a compile-time constant and may be used in:
- array lengths `T[N]`
- channel capacities `T[~N ... >]` / `T[~N ... <]`
- other constant-expression contexts (see §12)

---

### 10.2 Instantiation & Monomorphization (Normative)

Generics are implemented via compile-time monomorphization.

**Rule (instantiation):** A generic declaration produces no code by itself. Code is generated only when the program forms an instantiation by calling the function or naming the type with concrete arguments.

**Rule (uniqueness):** Each distinct set of generic arguments produces a distinct instantiation:
- `swap<int>` and `swap<u64>` are different instantiations.
- `SmallVec<int, 8>` and `SmallVec<int, 16>` are different instantiations.

**Rule (identity):** Two instantiations are the "same" only if all type arguments are identical and all comptime value arguments are equal.

**Lowering (conceptual):**
- `swap<T>` lowers to `swap__T_<mangled>(...)` per instantiation.
- `Option<T>` lowers to `Option__T_<mangled>` per instantiation.
- (Exact mangling is implementation-defined; uniqueness is required.)

---

### 10.3 Type Inference

**Rule (inference from arguments):** Type parameters may be inferred from call-site arguments.

```c
swap(&a, &b);     // infers T from a/b pointer types
```

**Rule (explicit instantiation):** The caller may specify generic arguments explicitly:

```c
swap<int>(&a, &b);
```

**Rule (partial explicit):** If some arguments are explicit and others omitted, omitted type parameters are inferred (if possible). Comptime value parameters must be explicit unless inferable from a dependent type.

```c
// N inferable from SmallVec<int, 8>*:
void push<T, comptime int N>(SmallVec<T, N>* v, T x);

SmallVec<int, 8> v;
push(&v, 3);   // infers T=int, N=8
```

**Rule (failure):** If inference cannot determine all parameters, compilation fails with a generic inference error.

---

### 10.4 Specialization (Zig-style)

Generic code can branch on type/value parameters using `@comptime if` (defined in §12).

```c
void copy<T>(T[:] dst, T[:] src) {
    @comptime if (sizeof(T) <= 8) {
        for (size_t i = 0; i < src.len; i++) dst.ptr[i] = src.ptr[i];
    } else {
        memcpy(dst.ptr, src.ptr, src.len * sizeof(T));
    }
}
```

**Rule:** Only the taken branch is type-checked; untaken branches are discarded.

---

### 10.5 Ownership and Moves in Generic Code

Generic code obeys the same copy/move rules as non-generic code.

**Rule (by-value parameters):** Passing a move-only value to `fn(T x)` moves it into the function. Passing a copyable value copies.

```c
void take<T>(T x) { use(x); }

char[:]? u = await ch.recv(); // u contains move-only slice
take(*u);                      // moves out (unwrap already moves), OK
```

**Rule (return):** Returning a move-only `T` returns by move (same as normal return rules).

**Rule (no hidden cloning):** The compiler must not implicitly copy/clone move-only values inside generic instantiations. If generic code attempts a copy (e.g., `T y = x;`) and `T` is move-only, it is a compile-time error for that instantiation.

---

### 10.6 Restrictions (v1.0)

Intentionally omitted in v1.0:

- Trait/protocol bounds (`<T: Hashable>`)
- User-defined compile-time interfaces
- Generic impl blocks / methods parameterized on `Self`
- Specialization by overload sets (only `@comptime if` specialization is supported)
- Runtime reflection over type `T`

**Rule:** Built-in generic types (`Task<T>`, `Mutex<T>`, `Map<K,V>`, etc.) follow the same monomorphization rules as user-defined generics.

---

## 11. Collections

This section defines built-in generic collection types: `Vec<T>` (dynamic array) and `Map<K,V>` (hash table). Both are arena-backed for predictable allocation and lifetime.

---

### 11.1 Vec<T> (Dynamic Array)

`Vec<T>` is a generic, arena-backed dynamic array with UFCS methods.

**Type definition:**

```c
typedef struct Vec<T> Vec<T>;
```

**Factories:**

```c
Vec<T> vec_new<T>(Arena* a);                          // Create empty vec
Vec<T> vec_with_capacity<T>(Arena* a, size_t capacity);  // Pre-allocate
```

**UFCS Methods:**

```c
// Mutating
void    Vec<T>.push(T value);                   // Add element, grows as needed
T?      Vec<T>.pop();                           // Remove and return last (None if empty)
void!BoundsError Vec<T>.set(size_t i, T value);       // Set with bounds check

// Querying
T?      Vec<T>.get(size_t index);               // Bounds-safe get (None if out of bounds)
T*      Vec<T>.get_mut(size_t index);           // Mutable reference (NULL if out of bounds)
size_t  Vec<T>.len();                           // Number of elements
size_t  Vec<T>.cap();                           // Allocated capacity
T[:]    Vec<T>.as_slice();                      // View as slice

// Bulk operations
void    Vec<T>.clear();                         // Remove all elements
```

**Iterator:**

```c
struct VecIter<T> {
    Vec<T>* vec;
    size_t index;
};

VecIter<T> Vec<T>.iter();                       // Create iterator
T?         VecIter<T>.next();                   // Get next element (None when exhausted)
```

**Rules:**

- `Vec<T>` is **move-only** when `T` is move-only. Copying a vec with non-copyable elements is a compile-time error.
- `Vec<T>` allocates from the arena provided at construction; elements do not shift between arenas.
- `push()` may cause reallocation; all existing elements remain valid.
- `clear()` resets length to 0 but does not deallocate capacity.
- Iteration over a modified vec (during iteration, elements added/removed) is undefined behavior.

**Examples:**

```c
Arena arena = arena(megabytes(1));

// Work queue (spawn and await async tasks)
Vec<Task<void>> tasks = vec_new<Task<void>>(&arena);
tasks.push(async_compute(data1));
tasks.push(async_compute(data2));
tasks.push(async_compute(data3));

VecIter<Task<void>> it = tasks.iter();
while (Task<void>? task = it.next()) {
    await *task;
}

// String accumulation
Vec<char> buffer = vec_new<char>(&arena);
for (size_t i = 0; i < input.len; i++) {
    buffer.push(input.ptr[i]);
}
char[:] result = buffer.as_slice();

// Bounds-safe access
Vec<int> data = vec_new<int>(&arena);
data.push(42);
int? val = data.get(0);       // Some(42)
int? oob = data.get(100);     // None (out of bounds)

// Error handling with set
if (Error? e = data.set(0, 99)) {
    handle_bounds_error(e);
}
```

---

### 11.2 Map<K, V> (Hash Table)

`Map<K, V>` is a generic, arena-backed hash table with UFCS methods.

**Type definition:**

```c
typedef struct Map<K, V> Map<K, V>;
```

**Factory:**

```c
Map<K, V> map_new<K, V>(Arena* a);              // Create empty map
```

**UFCS Methods:**

```c
// Mutating
void    Map<K, V>.insert(K key, V value);       // Insert or update
bool    Map<K, V>.remove(K key);                // Remove (true if existed)
void    Map<K, V>.clear();                      // Remove all entries

// Querying
V?      Map<K, V>.get(K key);                   // Lookup (None if missing)
V*      Map<K, V>.get_mut(K key);               // Mutable reference (NULL if missing)
size_t  Map<K, V>.len();                        // Number of entries
size_t  Map<K, V>.cap();                        // Capacity
```

**Rules:**

- `Map<K,V>` is **move-only**. Copying a map is a compile-time error. Share via `Mutex<Map<K,V>>`.
- Slice keys and values are deep-copied into the map's arena. Originals can be freed after insertion.
- `K` and `V` must have `hash(K) -> u64` and `eq(K, K) -> bool` free functions (defaults exist for primitives and slices).
- The map is **not thread-safe** but is **sendable**. Concurrent access requires external synchronization.
- Mutating a map during iteration is undefined behavior (Phase 2 will add safe iterators).

**Hash/Eq Protocol:**

Maps use free functions for equality and hashing. Defaults exist for all primitives, slices, and basic structs.

```c
// Custom key type
struct Point { int x; int y; }

u64  hash(Point p) {
    return ((u64)p.x * 31) ^ ((u64)p.y * 37);
}

bool eq(Point a, Point b) {
    return a.x == b.x && a.y == b.y;
}
```

**Examples:**

```c
Arena arena = arena(megabytes(1));

// String → int cache
Map<char[:], int> cache = map_new<char[:], int>(&arena);
cache.insert("hits", 100);
cache.insert("misses", 5);

int? val = cache.get("hits");    // Some(100)
int? miss = cache.get("nothere"); // None

// Request ID → state table
struct RequestState { int code; char[:] body; };
Map<int, RequestState> active = map_new<int, RequestState>(&arena);

active.insert(req.id, state);
RequestState? found = active.get(42);
if (found) {
    process_response(*found);
}
active.remove(42);
```

---

## 12. Compile-Time Evaluation (`comptime`)

This section defines compile-time computation as a restricted evaluation mode used for constants, specialization, and generic metaprogramming.

- **§12.1 Constant expressions** — what counts as compile-time
- **§12.2 `comptime` declarations** — compile-time storage
- **§12.3 `comptime` parameters** — compile-time arguments
- **§12.4 `@comptime if`** — compile-time branching
- **§12.5 `comptime {}` blocks** — compile-time execution for initialization
- **§12.6 Built-ins** — minimal type/ABI queries
- **§12.7 Restrictions** — what comptime cannot do

---

### 12.1 Constant Expressions (Normative)

A **constant expression** is an expression evaluable during compilation.

Constant expressions may use:

- Literals
- `sizeof(T)`, `alignof(T)`, `offsetof(T, field)`
- Arithmetic/bitwise/boolean operations
- Casts between integer types (if no overflow beyond target width)
- References to other `comptime` values
- Calls to `comptime` functions (see §12.2), if all arguments are constant expressions
- Enum values

**Rule:** A constant expression must not depend on runtime state (globals with runtime initialization, function calls without `comptime`, I/O, allocation, atomics, mutexes, channels, tasks).

---

### 12.2 `comptime` Declarations

`comptime` on a variable requires compile-time evaluation and gives it static storage duration.

```c
comptime int A = 1 + 2;
comptime size_t PAGE = kilobytes(4);
comptime char[:] VERSION = "1.0.0";
```

**Rule:** The initializer must be a constant expression.

**Rule:** `comptime` variables are immutable.

**`comptime` functions:**

Functions marked `comptime` can be evaluated at compile time:

```c
comptime int fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

comptime size_t align_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

comptime int mask(comptime int bits) {
    return (1 << bits) - 1;
}
```

**Rule:** `comptime` functions may only call other `comptime` functions, use `comptime`-safe operations (arithmetic, comparisons, control flow), and access `comptime` values.

**Rule:** `comptime` functions cannot perform I/O, allocate memory, or have side effects.

**Rule:** `comptime` functions can also be called at runtime (they are valid runtime functions too).

---

### 12.3 `comptime` Parameters

Functions may take compile-time parameters explicitly:

```c
comptime int mask(comptime int bits) {
    return (1 << bits) - 1;
}

comptime int M = mask(8);  // M = 255
```

**Rule:** Arguments passed to a `comptime` parameter must be constant expressions.

**Rule:** A `comptime` parameter may be used wherever a constant expression is required (array lengths, channel capacities, switch case values, etc.).

**Important interaction with generics:** `comptime` parameters are the standard way to express lightweight "value generics" (Zig-style) without introducing a separate template system.

```c
// Comptime parameter drives array size
int sum_n(comptime int N, int[N] xs) {
    int sum = 0;
    for (int i = 0; i < N; i++) sum += xs[i];
    return sum;
}
```

---

### 12.4 `@comptime if`

`@comptime if (COND) { ... } else { ... }` chooses a branch at compile time.

```c
void print_any<T>(T x) {
    @comptime if (is_slice(T)) {
        print_slice(x);
    } else {
        print_primitive(x);
    }
}

void serialize<T>(T value, char[~]* out) {
    @comptime if (is_slice(T)) {
        serialize_slice(value, out);
    } else @comptime if (is_optional(T)) {
        serialize_optional(value, out);
    } else {
        serialize_primitive(value, out);
    }
}
```

**Rule:** `COND` must be a constant expression.

**Rule:** Only the selected branch is type-checked and lowered; the other branch is discarded. This enables type-specific code that would otherwise fail to compile.

---

### 12.5 `comptime {}` Blocks

A `comptime { ... }` block runs during compilation and may be used to initialize `comptime` variables and static data.

```c
comptime u32 CRC_TABLE[256];

comptime {
    for (int i = 0; i < 256; i++) {
        CRC_TABLE[i] = crc32_seeded(i);
    }
}
```

**Rule:** A `comptime {}` block may assign only to:
- `comptime` variables
- Compile-time-known static storage declared in the same translation unit

**Rule:** Control flow inside `comptime {}` is allowed (`if`, `for`, `while`) as long as all conditions are constant-expression decidable.

---

### 12.6 Built-in `comptime` Queries

The following are available in constant expressions:

```c
comptime size_t sizeof(type T);
comptime size_t alignof(type T);
comptime size_t offsetof(type T, field F);

comptime bool is_pointer(type T);
comptime bool is_slice(type T);
comptime bool is_optional(type T);
comptime bool is_result(type T);

// Helpful for specialization:
comptime bool is_sendable(type T);
comptime bool is_copyable(type T);
```

**Rule (`is_sendable` / `is_copyable`):** These are defined by the same structural rules as §1.1 and §1.2. They are compile-time predicates over types.

---

### 12.7 Static Assertions

`comptime_assert` checks conditions at compile time:

```c
comptime_assert(sizeof(int) == 4, "expected 32-bit int");
comptime_assert(alignof(void*) >= 4, "pointer alignment too small");
comptime_assert(BUFFER_SIZE >= 1024, "buffer too small");
```

**Rule:** If the condition is false, compilation fails with the provided message.

---

### 12.8 Restrictions (v1.0)

At compile time, the following are forbidden:

- I/O
- Heap allocation (including arena creation/allocation)
- Touching channels, tasks, thread primitives
- Atomics/mutex operations
- Calling non-`comptime` functions
- Taking addresses of runtime locals (no "pointer into runtime")
- Relying on undefined behavior

**Rule:** A violation is a compile-time error.

---

## 13. Non-Goals and Explicit Omissions

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

unsafe {
    NonSendable ns = get_non_sendable();
    spawn (() => { use(ns); });  // ERROR still: closure escapes
}
```

### A.2 Adopting FFI Allocations with `adopt()`

C APIs often return owned buffers. The `adopt()` function integrates these into Concurrent-C's ownership model.

**Signature:**

```c
T[:] adopt<T>(void* ptr, size_t count, void (*deleter)(void*));
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
    await ch.send(file_data);
    
    // ERROR: adopted slices not transferable
    // await ch.send_take(file_data);
    
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

Arena arena = arena(megabytes(1));
Vec<char> buf = vec_with_capacity<char>(&arena, 1000);
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

| Category | Examples | Spec Section |
|----------|----------|--------------|
| Type errors | Optional/result field access on wrong branch | § 2.1, 2.2 |
| Provenance errors | Slice outlives arena (statically provable) | § 2.4–2.6 |
| Sendability errors | Non-sendable capture in spawn closure | § 1.2, § 7.3 |
| Ownership errors | Copy of unique slice, use after move | § 1.1, § 2.6 |
| Borrow errors | Borrow outlives owner (statically provable) | § 3.2 |
| Async errors | Missing `await`, invalid suspension point | § 7.2 |
| Blocking context errors | Blocking call in @async where auto-wrap unavailable | § 7.9 |
| @latency_sensitive violations | Blocking call in @latency_sensitive function | § 7.8.3.1 |
| Comptime errors | Non-constant in `comptime` context | § 12 |
| Syntax errors | Invalid Concurrent-C syntax | § 9 |
| Optional unwrap | `*x` without proven Some branch | § 2.1 |
| Result unwrap | `.value`/`.error` on wrong branch | § 2.2 |
| Use after move | Accessing move-only value after transfer | § 1.1 |
| Guard across suspension | Guard held across `await` or `run_blocking` | § 7.9 |
| Unsafe adoption | `adopt()` outside `unsafe {}` | § G.2 |

### B.2 Runtime Errors (Debug Builds)

The following are detected at runtime in debug builds only:

| Category | Signal | Behavior |
|----------|--------|----------|
| Stale slice access | Slice `id` not in allocation table | Trap/abort with diagnostic |
| Arena overflow | Allocation exceeds arena capacity | Trap with allocation size |
| Channel overflow | Buffer full (Busy) | Signaled via IoError::Busy |
| Deadlock | All tasks blocked + no runnable | Trap with task wait graph |
| Stack slice escape | Stack-backed slice escapes frame | Trap (stack capture safety) |
| Bounds violation | Array/slice access out of bounds | Return None or error |

**Deadlock diagnostics:** When detected, runtime prints a task wait graph showing which tasks are blocked on which resources.

**Stack escape detection:** Implemented via runtime metadata on stack-backed slices (debug-only overhead).

### B.3 Undefined Behavior in Release Builds

The following are undefined behavior in release builds (no diagnostic required):

| Category | Consequences | Notes |
|----------|-------------|-------|
| Stale slice access | Read garbage, crash, memory corruption | Enabled via unsafe, wrong adopt() deleter |
| Use after move | Read garbage, crash, double-free | Move-only violation, compile-time error when caught |
| Double free | Memory corruption, crash | Manually calling deleter twice on adopt() slice |
| Data races | Corruption, crashes, non-determinism | Shared mutable state without synchronization |
| Inactive union read | Read garbage from wrong arm | Optional/Result with wrong branch |
| Stack slice escape | Use invalidated stack memory | Stack-backed slice used after frame exit |
| Bounds violation | Read/write out of bounds | Bypassing bounds checks in unsafe |
| Overflow | Integer wrapping (as per C spec) | Unchecked arithmetic |

**Rule:** Debug builds should trap on all detectable UB (see §H.2). Release builds may assume UB does not occur and optimize accordingly.

---

## Appendix C: Implementation Notes

This appendix provides guidance for implementers of the CC-to-C translator. All guidance is non-normative; implementations may use different strategies as long as semantic guarantees are met.

### C.1 Automatic Wrapping of Blocking Calls in `@async` Functions

**Problem:** When an `@async` function calls a non-`@async` (blocking) function, the call must be dispatched to the blocking thread pool, not inlined.

**Solution:** The compiler batches contiguous non-`@async` calls and wraps each batch in a single `await run_blocking(...)`.

**Algorithm:**

1. Scan the `@async` function body for contiguous sequences of non-`@async` calls
2. A **batching boundary** is:
   - Function boundary
   - Await or other suspension point
   - `@async` function call
   - Variable declaration/assignment that depends on I/O
3. Group calls before each boundary into batches
4. Emit one `await run_blocking(closure)` per batch

**Example:**

```c
@async void process() {
    int a = compute1();     // non-@async call (batch 1)
    int b = compute2();     // non-@async call (batch 1)
    
    // [batching boundary: suspension point]
    int c = await fetch_data();
    
    // [batching boundary: start new batch]
    int d = compute3();     // non-@async call (batch 2)
    
    // [function exit boundary]
}
```

Lowers to:

```c
@async void process() {
    int a, b, c, d;
    
    // Batch 1: awaits run_blocking
    await run_blocking(() => {
        a = compute1();
        b = compute2();
    });
    
    // Suspension point
    c = await fetch_data();
    
    // Batch 2: awaits run_blocking
    await run_blocking(() => {
        d = compute3();
    });
}
```

**Variable hoisting:** Variables assigned in batches must be declared before the batch and remain in scope after.

### C.2 Batching Performance & @latency_sensitive

**@latency_sensitive optimization:** Functions marked `@latency_sensitive` must not be coalesced into larger batches.

**Rule for compiler:** When coalescing batches would cause a `@latency_sensitive` call to be batched with other non-@async calls, do not coalesce. Keep the @latency_sensitive call in its own batch.

**Example:**

```c
@async @latency_sensitive void fast_handler() {
    setup();          // bounded work
    data = process(); // bounded work (latency-sensitive)
    await send(result);
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
- Reference capture `[&x]` with mutation → requires `Mutex<T>`, `Atomic<T>`, or `@unsafe`

**Example (escape safety):**

```c
@async void bad() {
    int x = 42;
    nursery {
        spawn (() => { use(x); });  // ✅ OK: value capture (copy)
    }
}
```

**Example (mutation checking):**

```c
@async void bad_race() {
    int counter = 0;
    nursery {
        spawn ([&counter]() => { counter++; });  // ❌ ERROR: mutation of shared ref
        spawn ([&counter]() => { counter++; });
    }
}

@async void ok_atomic() {
    Atomic<int> counter = atomic_new(0);
    nursery {
        spawn ([&counter]() => { counter++; });  // ✅ OK: Atomic is thread-safe
        spawn ([&counter]() => { counter++; });
    }
}

@async void ok_readonly() {
    int config = 42;
    nursery {
        spawn ([&config]() => { printf("%d", config); });  // ✅ OK: read-only
        spawn ([&config]() => { printf("%d", config); });
    }
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

**Bump allocator:**

```c
struct Arena {
    void* base;      // start of allocation
    size_t size;     // total size
    size_t used;     // bytes used so far
};

void* arena_alloc(Arena* a, size_t nbytes, size_t align) {
    size_t aligned = (a->used + align - 1) / align * align;
    if (aligned + nbytes > a->size) return null;  // overflow
    void* result = a->base + aligned;
    a->used = aligned + nbytes;
    return result;
}

void arena_reset(Arena* a) {
    a->used = 0;  // O(1) reset
}
```

**Per-request pattern:**

```c
while (true) {
    Request req = accept_connection();
    Arena req_arena = arena_with_capacity(megabytes(1));
    
    handle_request(&req, &req_arena);
    
    arena_reset(&req_arena);  // O(1) cleanup
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
5. Provide `--emit-c` flag for debugging

**Example:**

```c
// CC code:
@async void process() {
    int a = compute();
    int b = await fetch();
}

// Generated C:
Task__process__0 process__init() {
    ProcessState* state = malloc(sizeof(ProcessState));
    state->pc = 0;
    return (Task__process__0) { .opaque = state };
}

// ... state machine lowering with clear labels
```

---

## Appendix D: ABI Commitments

This appendix documents stable layout and calling conventions for binary compatibility and debugging.

### D.1 Type Layouts

**Optional (`T?`) layout:**

```c
struct Optional_T {
    _Bool has;        // C11 _Bool (1 byte, values 0 or 1)
    // padding to alignof(T)
    union { T value; } u;
};
// sizeof(Optional_T) = alignof(T) + sizeof(T) (with padding)
```

**Example (Optional<int>):**

```c
struct Optional_int {
    _Bool has;        // 1 byte
    // 3 bytes padding (assume 4-byte int alignment)
    int value;        // 4 bytes
};
// sizeof = 8 bytes
```

**Result (`T!E`) layout:**

```c
struct Result_T_E {
    _Bool ok;         // C11 _Bool (1 byte)
    // padding to max(alignof(T), alignof(E))
    union { T value; E error; } u;
};
// sizeof(Result_T_E) = max_align + max(sizeof(T), sizeof(E))
```

**Example (Result<int, ParseError>):**

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
    int64_t secs;     // 8 bytes (seconds since epoch)
    int32_t nanos;    // 4 bytes (nanoseconds, 0–999999999)
    // 4 bytes padding
};
// sizeof(Duration) = 16 bytes
```

### D.2 Calling Conventions

**`@async` function lowering:**

`@async` functions lower to state-machine generators. The exact ABI is implementation-defined, but:

- `Task<T>` is an opaque pointer-sized handle (typically a `void*` pointing to heap-allocated state)
- The state includes registers (local variables), current PC, and result storage
- Calling an `@async` function allocates and initializes state but does not start execution
- Execution begins on first `await` of the returned task

**Channel operations:**

Channel operations (`send`, `recv`, `close`) are function calls to runtime library functions:

```c
// Async variant
@async void chan_send<T>(T[~ >]* tx, T value);
@async T? chan_recv<T>(T[~ <]* rx);
void chan_close<T>(T[~ >]* tx);

// Sync variant
void chan_send_blocking<T>(T[~ sync >]* tx, T value);
T? chan_recv_blocking<T>(T[~ sync <]* rx);
```

The channel handle is passed as the first argument.

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

**Stability guarantee:** Once released, the ABI for a given Concurrent-C version is stable. Future versions may:

- Add new fields to structs (at the end, with padding)
- Extend enum variants (at the end)
- Add new functions to the runtime

They must not:

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

@async void db_query(int count);  // Must await

@async @latency_sensitive void handler(Request req) {
    int count = parse_count(req.body);  // ✅ OK (@noblock)
    try await db_query(count);          // ✅ OK (awaited)
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
    try await db_get(count);      // ✅ OK (awaited)
    process_logs(count);          // ❌ ERROR: might block
}
```

**Compiler output:**

```
error: non-@noblock, non-@async call in @latency_sensitive function
  → process_logs(count);
  
  Fix: Mark process_logs @noblock, or make it @async and await it
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

| Mode | Behavior | Queue Full | Sender | Receiver | Use Case |
|------|----------|-----------|--------|----------|----------|
| **Block** (default) | Block until space | Waits | Suspends/blocks | All messages | Default, backpressure desired |
| **Drop** | Discard oldest | Discards head | Succeeds immediately | Recent msgs | High-volume, tolerate loss |
| **Sample** | Keep ~rate% | Deterministic drop | Succeeds immediately | ~rate% of msgs | Sparse traces, fair sampling |

**Used in:**
- Channels: `T[~N >, Drop]` / `T[~N <, Drop]`, `T[~N >, Block]` / `T[~N <, Block]`, `T[~N >, Sample(0.05)]` / `T[~N <, Sample(0.05)]`
- Logging: `log_drop()`, `log_block()`, `log_sample()`

**Key property:** Sampling is deterministic (reproducible, fair), not random.

---

## Appendix F: Server Programming Patterns

### Pattern 1: Request Handler

```c
@async @latency_sensitive Response!IoError my_handler(Request* req, Arena* a) {
    // CPU work: inline (compiler inlines aggressively)
    Parsed p = parse(req.body);
    
    // Stalling I/O: separate dispatch (visible latency)
    DbResult res = try await db_get(p.id, a);
    
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
try log_block(audit, ms(100));       // Fail if timeout
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
    
    try await server_loop(cfg);
}
```

**server_loop handles:**
- Accept connections
- Create per-request arena
- Apply deadline (via `with_deadline`)
- Spawn N workers (nursery)
- Graceful shutdown

**User provides:** Handler + config (~30 lines total)

---

### Pattern 4: Connection Lifetime Template (Handshake → Serve → Teardown)

This pattern is the recommended template for long-lived connections (WebSocket, gRPC streaming, custom protocols).

**Strong recommendation:** Structure connections into three phases:

1. **Handshake phase:** short, deadline-bounded
2. **Serve phase:** potentially long-lived; structured as reader+writer tasks in a nursery (“first error/close cancels siblings”)
3. **Teardown phase:** bounded, shielded cleanup to perform protocol-correct closes

```c
@async void!IoError handle_conn(Duplex* conn, Arena* conn_arena) {
    // 1) Handshake (short deadline)
    with_deadline(deadline_after(seconds(3))) {
        try await protocol_handshake(conn, conn_arena);
    }

    // 2) Serve (long-lived)
    @nursery {
        spawn (reader_task(conn, conn_arena));
        spawn (writer_task(conn, conn_arena));
        // Any child failure/close cancels siblings; nursery joins all children.
    }

    // 3) Teardown (bounded, shielded)
    with_shield {
        try await protocol_close(conn);              // best-effort protocol close
        try await drain_with_timeout(conn, ms(200)); // bounded drain/flush if applicable
    }

    return cc_ok(());
}
```

---

### Pattern 5: Bidi Stream “First-Close Wins”

For bidi protocols, the default rule SHOULD be:

- Any close/error in reader OR writer cancels the sibling task
- Teardown happens exactly once, in one place, and is bounded + shielded

This avoids “both sides race to close” bugs and makes shutdown reviewable.

---

### Pattern 6: Deadline Layering for Long-Lived Connections

**Strong recommendation:** Avoid wrapping an entire long-lived serve loop in one large `with_deadline(...)` unless you are intentionally enforcing an end-to-end SLA.

Prefer layered deadlines:

- **Handshake deadlines:** short `with_deadline` around negotiation (TLS/WS upgrade/initial headers)
- **Idle/heartbeat deadlines:** renewed on activity (timer task + cancellation, or per-iteration short deadline)
- **Teardown deadlines:** short, bounded shutdown/drain inside `with_shield`

This keeps deadlines precise and prevents “everything is always under a deadline” from becoming the default mental model.

---

## Appendix G: Terminology Summary

### Keywords & Annotations

| Keyword | Meaning | Usage |
|---------|---------|-------|
| `@async` | Function may suspend | Mark async functions |
| `@noblock` | Never blocks/allocates | Mark pure utilities |
| `@latency_sensitive` | No coalescing allowed | Mark request handlers |
| `@scoped` | Cannot escape scope | Mark safe cross-thread refs |
| `@match` | Pattern matching | Multiplex on channels |
| `spawn` | Create task | In nursery scope |
| `defer` | Defer cleanup | Guarantee execution |
| `try` | Propagate error | Chain operations |
| `await` | Suspend on async | Call @async functions |
| `nursery` | Structured concurrency | Scope with tasks |
| `with_deadline` | Apply timeout | Enforce deadline |
| `with_shield` | Suppress deadline cancellation observation | Bounded teardown/cleanup |

### Type Sugar

| Sugar | Full | Meaning |
|-------|------|---------|
| `T?` | `Option<T>` | Optional value |
| `T!E` | `Result<T, E>` | Either T or error E |
| `T[:]` | Slice of T | Pointer + length |
| `T[~... >]` / `T[~... <]` | `AsyncChanTx<T>` / `AsyncChanRx<T>` | Async channel handles |
| `T[~N ... >]` / `T[~N ... <]` | `AsyncChanTx<T, N>` / `AsyncChanRx<T, N>` | Async handles, capacity N |
| `T[~N, Mode ... >]` / `T[~N, Mode ... <]` | `AsyncChanTx<T, N, Mode>` / `AsyncChanRx<T, N, Mode>` | Async handles with backpressure |
| `T[~ ... sync ... >]` / `T[~ ... sync ... <]` | `SyncChanTx<T>` / `SyncChanRx<T>` | Sync channel handles |

---

## Appendix H: Complete Example: HTTP Server

```c
#include <std/prelude.cch>
#include <std/server.cch>
#include <std/log.cch>

// Handler: Mark @latency_sensitive to ensure predictable latency
@async @latency_sensitive Response!IoError api_handler(Request* req, Arena* a) {
    // CPU work: parse (inlined, no latency)
    UserId user_id = try parse_user_id(req.path);
    
    // Stalling I/O: fetch from database (separate dispatch, observable)
    User user = try await db_get_user(user_id, a);
    
    // CPU work: encode (inlined)
    char[:] json = try encode_user_json(&user, a);
    
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
    
    try await server_loop(cfg);
}
```

**Properties:**
- ✅ Handler marked `@latency_sensitive` (compiler prevents blocking)
- ✅ Deadline per request (timeout enforced via `server_loop`)
- ✅ Per-request arena (automatic reset, no leaks)
- ✅ ~20 lines of user code for complete server
- ✅ Safe, fast, observable

---

## Appendix I: Candidate Phase 1.1 Hoists

Phase 1.0 is complete and canonical for unary request/response and long-lived connection patterns. However, early stdlib exploration (especially `<std/server.cch>`) has revealed three patterns that appear repeatedly and suggest language-level hoists for Phase 1.1.

### I.1 Implicit Cancellation Checks at Await Points (High Priority)

**Pattern:**

Long-lived connection loops rely on timely cancellation when deadlines expire or connections close:

```c
@async void!IoError connection_handler(Duplex* conn, Arena* conn_arena) {
    with_deadline(deadline_after(seconds(30))) {
        while (char[:]?!IoError msg = try await conn.read(conn_arena)) {
            if (!msg) break;
            process(msg);
        }
    }
}
```

**Current behavior (§ 7.5, § 3.2):** Inside an active `with_deadline()` scope, suspension points must check for cancellation before and after suspension, requiring explicit @match scaffolding. The compiler enforces these checks at compilation time (as per § 3.2); the runtime behavior is defined in § 7.5.

**Proposed Phase 1.1 change:** Inside an active `with_deadline()` scope, all await points become implicitly cancellation-aware. On cancellation, the suspension point returns `err(Cancelled)` in-band. The loop naturally exits via `try` propagation without explicit `@match` scaffolding.

**Motivation:**
- Every long-lived loop (WebSocket, gRPC bidirectional stream, streaming read) needs this
- Correct-by-default: deadline semantics + await = safe cancellation
- Eliminates boilerplate `@select` in read/write loops
- Aligns with your existing deadline-aware cancellation semantics (§ 3.2, § 7.5)

**Impact:** Unary handlers (with deadline), takeover handlers (explicit deadline control), and request body streaming all become safer with zero additional code.

---

### I.2 Async Iteration / For-Await Construct (High Priority)

**Pattern:**

Many operations produce sequences of items asynchronously: request body chunks, response body chunks, WebSocket frames, gRPC messages, file reads. All follow the same pattern:

```c
// Request body
while (char[:]?!IoError chunk = try await req.body.read_chunk(arena)) {
    if (!chunk) break;
    process(chunk);
}

// Response body
while (char[:]?!IoError chunk = try await resp_iter.next(arena)) {
    if (!chunk) break;
    process(chunk);
}

// WebSocket frames
while (char[:]?!IoError frame = try await ws.read_frame(arena)) {
    if (!frame) break;
    process(frame);
}
```

**Proposed Phase 1.1 hoisting:** A language-level async iterator protocol and declarative for-await syntax:

```c
struct AsyncIterator<T> {
    @async T?!E next(Arena* a);
};

// Declarative for await
for await (char[:] chunk in req.body) {
    process(chunk);
}

// Desugars to:
while (char[:]?!IoError chunk_opt = try await req.body.next(arena)) {
    if (!chunk_opt) break;
    char[:] chunk = *chunk_opt;
    process(chunk);
}
```

**Motivation:**
- Appears in every streaming example: requests, responses, WebSocket, gRPC, file I/O
- Unifies mental model: "async iteration is like sync iteration, just with await"
- Reduces boilerplate `while` loops across all protocol stacks
- Aligns with Go (context propagation + range over channels) and Zig (iterator protocol)

**Impact:** HTTP/1 streaming bodies, WebSocket frames, gRPC messages, and custom protocols all feel declaratively similar.

---

### I.3 Half-Close and Shutdown Modes on Duplex (Medium Priority, Stdlib)

**Pattern:**

Real protocols often need unidirectional closure: proxies (forward until upstream closes, continue sending to downstream), gRPC (send messages, signal EOF on write, keep reading responses), TLS termination (clean close on write, drain read side).

```c
@async void!IoError proxy_handler(Duplex* client, Duplex* upstream, Arena* a) {
    // Bidirectional forwarding
    @nursery {
        spawn (forward_client_to_upstream(client, upstream, a));
        spawn (forward_upstream_to_client(upstream, client, a));
    }
    
    // One direction closed; signal other direction to close write
    try await upstream.shutdown(Write);
    try await client.shutdown(Write);
}
```

**Implementation notes:**
- Duplex.shutdown(Read) stops reading; keeps write open (recv FIN, continue sending)
- Duplex.shutdown(Write) stops writing; keeps read open (send FIN, continue receiving)
- Duplex.shutdown(Both) closes both (equivalent to close())
- Standard sentinel `IoError::ConnectionClosed` indicates normal remote closure (like EOF for files)

**Motivation:**
- Every proxy, gRPC server, and TLS terminator needs this
- Without it, cleanup code is fragile and error-prone
- Half-close is POSIX standard (shutdown(2)); making it explicit in Duplex prevents footguns

**Impact:** Proxies, gRPC bidirectional streams, and TLS multiplexing all become safe and idiomatic.

**Note:** This is stdlib-level, not language-level. Already implemented in Phase 1.0 `<std/server.cch>`.

---

### I.4 Future Candidates (Phase 2+)

The following patterns emerged but are deferred to Phase 2:

**Arena checkpoint helpers:** Long-lived connections (WebSocket, gRPC streaming, HTTP/2) can run for hours. A stdlib helper (`with_arena_checkpoint()` or per-message `arena.reset()`) prevents unbounded growth. Stdlib pattern guideline is in `<std/server.cch>` design notes; full stdlib support in Phase 2.

**MuxConn abstraction:** HTTP/2 and gRPC multiplex many independent streams over one TCP/TLS connection. A future `MuxConn` will wrap a Duplex, accept per-stream Duplex + stream_arena, and integrate with server_loop. For Phase 1.0, Duplex assumes 1:1 connection-to-stream; Protocol layers (HTTP/2, gRPC) implement multiplexing above Duplex.

**Errdefer / conditional defer:** Go idiom for "defer cleanup only on error path". Useful for partial upgrades (WebSocket handshake cleanup). Deferred pending further language design.

---

## Appendix J: C Lowering Strategy

This appendix specifies how Concurrent-C constructs lower to portable C, ensuring stable ABI and readable generated code. Implementation must follow these rules to prevent ABI divergence and resource bugs.

### J.1 @async Lowering Strategy (Stackless State Machines)

**Normative lowering:** @async functions lower to stackless state machines, not stackful coroutines.

**Shape:**

Each @async function `T fn(Args...)` is compiled to:

```c
// Frame state for fn
typedef struct {
    int __state;           // Current state (0=start, 1=after first await, etc.)
    // Locals live here (persist across suspensions)
    T __retval;
    ArgType1 arg1;
    ArgType2 arg2;
    // ... local variables ...
} Frame_fn;

// Poll function: resume execution from __state
Task_T fn__poll(Frame_fn* frame, Waker* waker) {
    switch (frame->__state) {
        case 0:
            goto state_0;
        case 1:
            goto state_1;
        // ... more states ...
        default:
            return Task_Completed(frame->__retval);
    }
    
    state_0: {
        // Original function body from start
        frame->arg1 = /* ... */;
        // First await:
        // Save locals, increment state, return to scheduler
        frame->__state = 1;
        return Task_Pending();
    }
    
    state_1: {
        // Resume after first await
        T result = get_await_result();
        frame->__retval = result;
        frame->__state = 2;
        return Task_Completed(frame->__retval);
    }
}

// Public Task wrapper
Task_T fn(Args... args) {
    Frame_fn* frame = allocate_frame();
    frame->__state = 0;
    // Store args in frame
    return Task_new(frame, (PollFn)fn__poll);
}
```

**Key rules:**
- Locals are stored on the frame struct, not the stack
- Each await introduces a new state number
- Control flow uses switch + goto (readable, compiles to jump table)
- Dropping frame performs drop glue for move-only locals
- No hidden stack allocation; frame size is compile-time determinable

**Why stackless:** Enables bounded memory per task, no stack growth surprises, and clean C mapping.

---

### J.2 Cancellation Check Lowering (Cheap & Optimizable)

**Normative rule:** Inside an active `with_deadline()` scope, all suspension points must check for cancellation before and after suspension. Checks must be cheap (single branch).

For full deadline semantics and cancellation behavior, see **language spec § 7.5 (Cancellation & Deadline)**. This section describes how the compiler lowers these semantics to portable C using single-branch checks.

**Lowering:**

```c
// with_deadline pushes a cancel token into scope context
with_deadline(deadline_after(seconds(5))) {
    // Token is now active; compiler pushes pointer to token into frame context
    
    while (try await conn.read(...)) {
        // Before await: check cancellation
        if (unlikely(token->cancelled)) {
            return cc_err(Cancelled);
        }
        
        // Await (suspension point)
        // Compiler generates: save state, return Pending
        
        // After await: check cancellation
        if (unlikely(token->cancelled)) {
            return cc_err(Cancelled);
        }
        
        process(data);
    }
}
```

**Cost reduction:**
- Use `unlikely()` to push check out of hot path
- Compiler may elide check if it can prove:
  - No deadline is active on this code path
  - Token is statically known to be unset
  - This requires flow analysis but is optional optimization
- Check compiles to single conditional branch in most cases

**Implementation notes:**
- Token is stored in thread-local or frame-local context (not a parameter to every await)
- Cancelled flag is a simple boolean or atomically-read flag
- Cost is single memory load + branch per suspension point

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
- **Arena-allocated buffers:** Remain valid as long as arena is not reset. The compiler cannot prove arena reset points at compile time, so this remains a logic contract. `@scoped` guards (like locks) prevent holding references across suspension points (compiler enforces).
- **Adopted buffers:** Drop glue calls the deleter when frame is destroyed or field is overwritten.

**Drop order:** On frame destruction, drop glue runs in reverse order of field declaration (standard). Custom drop order can be specified via `@drop` annotation (future phase).

---

### J.4 @for await Lowering (One Await Per Iteration)

**Rule:** `for await` must lower to exactly one `await` per loop iteration, with no hidden buffering or double-await.

**Lowering:**

```c
for await (char[:] chunk in req.body) {
    process(chunk);
}
```

Desugars to:

```c
{
    AsyncIterator<char[:]> iter = req.body;
    while (true) {
        char[:]?!IoError next_result = await iter.next(arena);
        
        if (try char[:] chunk = next_result) {
            // Chunk is valid here; process it
            process(chunk);
            // No re-evaluation of condition; straight back to await
        } else {
            // None or error; exit loop
            break;
        }
    }
}
```

**Key constraints:**
- Exactly one `await` per iteration (at `iter.next()`)
- `next_result` variable is allocated on frame; reused each iteration
- Compiler must not buffer or cache results across iterations
- If `next_result` is an error, `try` propagates; loop does not continue

---

### J.5 Interface ABI (Duplex and Future Interfaces)

**Rule:** All interface values lower to two-pointer layout: `{ void* self, const VTable* vt }`.

**Example (Duplex):**

```c
typedef struct {
    Task_CharSliceOptIoErr (*read)(void* self, Arena* a);
    Task_VoidIoErr (*write)(void* self, CharSlice bytes);
    Task_VoidIoErr (*shutdown)(void* self, ShutdownMode mode);
    Task_VoidIoErr (*close)(void* self);
} DuplexVTable;

typedef struct {
    void* self;
    const DuplexVTable* vt;
} Duplex;

// Calling a method
DuplexVTable* vt = duplex.vt;
Task_CharSliceOptIoErr result = vt->read(duplex.self, arena);
```

**Constraints:**
- Methods do NOT capture closures or additional environment
- `self` is an opaque pointer; lifetime is determined by context (not owned by Duplex)
- Thread safety is NOT guaranteed by ABI; it's a contract rule (no cross-thread Duplex sharing)

**Benefits:**
- Stable ABI across versions
- Interop with plain C libraries (can implement Duplex in C)
- No hidden allocations or reference counts

---

### J.6 Readable C Mapping Philosophy

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

5. **Optional -emit-lowered-c mode:** For debugging and spec validation:
   ```bash
   cc compiler -emit-lowered-c -o output.c input.ccs
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

### J.7 Task and Scheduler Integration

**Normative:** Tasks are thin wrappers around frames + scheduler metadata.

```c
typedef struct {
    Frame* frame;           // The state machine frame
    PollFn poll;            // Pointer to fn__poll function
    Waker* waker;           // Scheduler handle for resumption
    uint32_t state;         // Scheduler state (pending, ready, completed)
} Task_T;

// Polling a task
TaskPoll poll_result = task.poll(task.frame, task.waker);
// Returns: Pending, Completed(T), or Error
```

**Key rules:**
- Task does not own the frame (caller manages lifetime)
- `poll()` is re-entrant; same Task can be polled multiple times
- Cancellation is checked via waker context, not Task state
- Task completion frees the frame and runs drop glue

---

## Summary: ABI Stability Locks

| Component | Lowering | Status |
|-----------|----------|--------|
| @async functions | Stackless state machines (switch + goto) | ✅ Normative |
| Cancellation checks | Single branch, optimizable (unlikely()) | ✅ Normative |
| Slice ownership | Frame-local; borrow checker enforces | ✅ Normative |
| @for await | One await per iteration | ✅ Normative |
| Interface ABI | Two-pointer layout {void*, vtable*} | ✅ Normative |
| Readable C | Named structs, labeled states, explicit cleanups | ✅ Principle |
| -emit-lowered-c | Optional debug mode with source mapping | ✅ Recommendation |

These rules prevent ABI surprises and ensure the implementation can generate boring, understandable C code.

---

## Summary: Path to Phase 1.1

**Phase 1.0 (current):**
- ✅ Core language: §§ 1–12, deadline-aware cancellation at suspension points, core concurrency primitives
- ✅ Stdlib: Strings, I/O, Collections, Server shell (with Duplex, Takeover, arena split, TLS, keep-alive, shutdown)
- ✅ Result: Safe, ergonomic unary and long-lived connection patterns

**Phase 1.1 candidates (high-value, low-risk):**
- ⏳ Implicit cancellation at all await points inside with_deadline (language)
- ⏳ Async iteration / for-await (language)
- ✅ Half-close / Duplex.shutdown() + ConnectionClosed (stdlib, already in Phase 1.0)

**Phase 2:**
- Arena checkpoint helpers (stdlib)
- MuxConn abstraction (stdlib)
- Graceful shutdown via cancellation signals (language + stdlib)
- Errdefer or conditional defer (language)
