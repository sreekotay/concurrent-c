# Concurrent-C Standard Library Specification

**Version:** 1.0  
**Date:** 2026-03-24  
**Status:** Normative standard-library and lowering specification

---

## Goals and Scope

The Concurrent-C Standard Library (`<std/...>`) provides minimal, composable wrappers around common systems operations, designed to integrate seamlessly with Concurrent-C's core features (arenas, slices, async, error values) while maintaining portability and zero-overhead abstraction.

**Philosophy:** Stay small (only battle-tested essentials), be explicit (allocations visible, arenas passed), leverage CC features (T!>(E) for errors, slices for views, channels for streaming), remain optional (header-only, no mandatory linkage), and **use UFCS for natural, discoverable APIs**.

**UFCS Pattern:** Method syntax is primary. UFCS dispatch is resolved from the concrete receiver type, and stdlib families own their lowering contract.

```c
// UFCS-first (user API)
int len = s.len();
s.trim().split(",").map(process);

// Direct library-call style (when exposed)
char[:] trimmed = cc_slice_trim(s);
String built = @string(trimmed, &arena);
```

**Scope:** This document defines the standard-library surface APIs together with their normative lowering contracts at the C boundary.

---

## Design Principles

1. **UFCS-First API:** Methods on types are primary. Users write `s.len()` not `string_length(s)`.
   - Lowering is normative: each stdlib type or family defines the callee selected for UFCS, including whether the receiver is passed by address or by value.

2. **Header-Only:** All Phase 1 functions defined in headers; no compilation required.
3. **Explicit Allocation:** All allocations via `Arena*` passed as explicit parameters. No hidden allocators.
4. **Error Values:** All fallible operations return `T!>(E)` (Result types); no errno, no exceptions.
5. **Slices Everywhere:** Parameters and returns use `char[:]` views where appropriate; avoid unnecessary copies.
6. **Portability:** Abstract OS differences (e.g., path separators) transparently.
7. **No Dependencies:** Stdlib depends only on C standard library and CC language primitives.
8. **Integration with Async:** I/O functions have sync and `@async` variants where applicable.
9. **Single runtime TU:** Runtime impls are aggregated in `cc/runtime/concurrent_c.c` (`#include` of arena, io, scheduler, channels) so consumers can link one object/archive without juggling multiple runtime objects.
10. **Prefixed C ABI:** Public C names are prefixed (`CCString`, `CCArena`, `cc_file_*`) to avoid collisions. The compiler automatically resolves short aliases to their prefixed forms. Header implementations are `static inline` to keep stdlib header-only.
11. **Arena-first collections:** Collections default to arena-backed, bounded growth. Vectors/maps grow by allocating new buffers/tables in the provided arena and reusing them; old buffers remain until the arena resets. Growth fails if the arena is exhausted. Heap-backed helpers (kvec/khash style) are optional, tool-only, and never used by generated code unless explicitly included.
12. **Async backend auto-probe:** The runtime may auto-select a native async backend (io_uring/kqueue/poll) with a safe fallback to the portable executor. An environment override (e.g., `CC_RUNTIME_BACKEND=executor|poll|io_uring`) can force selection; otherwise a best-available backend is chosen lazily.

### UFCS Lowering Contract

For the standard library, UFCS lowering is part of the normative API surface:

- Dispatch is chosen from the resolved receiver type and the full receiver expression to the left of `.` or `->`.
- Stdlib families normally define custom lowering through type-owned registration such as `cc_type_register(...)`. `cc_ufcs_register(...)` remains a compatibility mechanism for UFCS-only registration.
- Constructors and helper calls such as `string_new(...)`, `cc_string_from(...)`, `@string(...)`, `file_open(...)`, `cc_vec_new::[T](...)`, and `map_new<K, V>(...)` are direct library calls or syntax sugar, not UFCS.
- Exact generated helper names are normative where this document names them explicitly; otherwise, the family-level lowering contract is normative.

### Type Notation Precedence

Type modifiers bind with this precedence (tightest first):

1. `*` (pointer)
2. `!>(E)` (result)
3. `[:]` (slice)

See **§2.3 Type Precedence** in the main language spec for complete rules.

**Examples:**

| Syntax | Parses as | Meaning |
|--------|-----------|---------|
| `char[:] !>(CCIoError)` | `(char[:]) !>(CCIoError)` | slice or error (e.g., `read()` return; empty slice = EOF) |
| `T* !>(CCIoError)` | `((T)*) !>(CCIoError)` | result of pointer (e.g., `cc_dir_open()` return) |
| `bool !>(CCIoError)` | `(bool) !>(CCIoError)` | iterator / recv / pop signal (`ok(true)` = value written to out-param, `ok(false)` = EOF/closed/drained) |

**Key distinction:**
- `T !>(E)` — "operation may fail; success is a `T`" (e.g., file read: error or data; empty slice means EOF)
- `T* !>(E)` — "operation may fail; if it succeeds, returns a pointer" (common for allocation / fallible lookup)
- `bool !>(E) op(T* out)` — iterators, `recv`, pop-style APIs; on `ok(true)` the out-param has been written

> **Retired.** The legacy optional-type constructor `T?` has been removed from this document. See the main spec §2.1 migration appendix for the mapping from `T?` to `T !>(E)`, `T*`, empty-slice, `bool` + out-param, or `-1` sentinels.

### Name Aliases (short vs prefixed)

All stdlib signature tables below use the **prefixed ABI names** (`CCString`, `CCFile`, `CCIoError`, `CCNetError`, `CCVec`, `CCMap`, `CCDuplex`, etc.) — these match the exact C ABI 1:1. The compiler automatically resolves these common short aliases in source code:

| Short alias | Prefixed ABI name |
|-------------|-------------------|
| `String` | `CCString` |
| `File` | `CCFile` |
| `IoError` | `CCIoError` |
| `NetError` | `CCNetError` |
| `ParseError` | `CCParseError` |
| `Vec` | `CCVec` |
| `Map` | `CCMap` |
| `Duplex` | `CCDuplex` |
| `Arena` | `CCArena` |

Free-form example code may use either form; all normative signature tables use the prefixed form.

### Type Macros

Convenience macros for constructing Result type names in C (interop only):

| Macro | Expands To | Example |
|-------|------------|---------|
| `CCRes(T, E)` | `CCResult_T_E` | `CCRes(int, CCError)` → `CCResult_int_CCError` |
| `CCResPtr(T, E)` | `CCResult_Tptr_E` | `CCResPtr(char, CCIoError)` → `CCResult_charptr_CCIoError` |

**Usage in headers vs source files:**

| Context | Syntax | Example |
|---------|--------|---------|
| `.ccs` source files | `T !>(E)` sigil syntax | `int !>(CCIoError) read_int(...)` |
| `.cch` header files | `CCRes(T, E)` macro | `CCRes(int, CCIoError) read_int(...)` |
| C interop headers | Explicit type name | `CCResult_int_CCIoError read_int(...)` |

The `CCRes(T, E)` macros are part of the C interop contract for headers and generated C. Implementations may use parser-only stubs internally, but that machinery does not change the visible family naming contract.

### Result Helpers

Macros for working with `T!>(E)` Result types:

| Helper | Returns | Description |
|--------|---------|-------------|
| `cc_is_ok(res)` | `bool` | True if result is success |
| `cc_is_err(res)` | `bool` | True if result is error |
| `cc_value(res)` | `T` | Get success payload (no branch check) |
| `cc_error(res)` | `E` | Get error payload (no branch check) |
| `cc_unwrap(res)` | `T` | Get value (for scalar types: int, size_t, bool) |
| `cc_unwrap_err(res)` | `E` | Get error (for scalar error types) |
| `cc_unwrap_as(res, T)` | `T` | Get value as specific type (for structs and pointers) |
| `cc_unwrap_err_as(res, E)` | `E` | Get error as specific type (for struct error types) |

**When to use which:**

| Value Type | Use | Example |
|------------|-----|---------|
| Scalar (int, size_t, bool) | `cc_unwrap(res)` | `int n = cc_unwrap(res);` |
| Pointer (`T*`) | `cc_unwrap_as(res, T*)` | `MyData* p = cc_unwrap_as(res, MyData*);` |
| Struct | `cc_unwrap_as(res, T)` | `CCDirEntry e = cc_unwrap_as(res, CCDirEntry);` |

**Note:** Pointer result types require `cc_unwrap_as` to avoid compiler warnings. This is a known limitation; future versions may infer the type automatically.

**Result struct layout:**

All Result family contracts use a unified C-visible struct layout:

```c
struct CCResult_T_E {
    bool ok;           // true = success, false = error
    union {
        T value;       // access via cc_value(res) or cc_unwrap*(...)
        E error;       // access via cc_error(res) or cc_unwrap_err*(...)
    } u;
};
```

**Idiomatic patterns:**

```c
// Pattern 1: Scalar result - use cc_unwrap
size_t !>(CCIoError) res = cc_file_write(file, data);
if (cc_is_err(res)) {
    handle_error(cc_error(res));
    return;
}
size_t bytes_written = cc_value(res);

// Pattern 2: Pointer result - use cc_unwrap_as with pointer type
CCDirIter* !>(CCIoError) iter_res = cc_dir_open(arena, ".");
if (cc_is_err(iter_res)) {
    CCIoError err = cc_unwrap_err_as(iter_res, CCIoError);
    printf("Error: %s\n", cc_io_error_str(err));
    return;
}
CCDirIter* iter = cc_unwrap_as(iter_res, CCDirIter*);

// Pattern 3: Struct result - use cc_unwrap_as with struct type
CCDirEntry !>(CCIoError) entry_res = cc_dir_next(iter, arena);
if (cc_is_err(entry_res)) {
    CCIoError err = cc_unwrap_err_as(entry_res, CCIoError);
    if (err.kind == CC_IO_OTHER && err.os_code == 0) {
        // EOF - not an error
    } else {
        handle_error(err);
    }
    return;
}
CCDirEntry entry = cc_unwrap_as(entry_res, CCDirEntry);
printf("Found: %s\n", (const char*)entry.name.ptr);
```

### Declaring Result Types in Headers

When defining custom Result types in `.cch` header files, use the `CCRes(T, E)` macro and `CC_DECL_RESULT_SPEC` with guards:

```c
// In your_types.cch
#include <ccc/cc_result.cch>

// Use CCRes(T, E) in function signatures
CCRes(MyData, MyError) my_function(int arg);

// Declare Result type with guards (prevents redefinition if compiler auto-generates)
#ifndef CCResult_MyData_MyError_DEFINED
#define CCResult_MyData_MyError_DEFINED 1
CC_DECL_RESULT_SPEC(CCResult_MyData_MyError, MyData, MyError)
#endif
```

**Why guards?** When you use `T !>(E)` syntax in `.ccs` files, the compiler automatically generates `CC_DECL_RESULT_SPEC` calls. The `#ifndef ..._DEFINED` guards prevent redefinition errors when explicit declarations exist in headers.

---

## Phase 1: Strings, I/O, and Collections

The Phase 1 stdlib includes three core modules: strings (manipulation and parsing), I/O (file and stream operations), and collections (dynamic arrays and hash maps). All are header-only or minimal-linkage, arena-backed, and designed for concurrent systems programming.

### 1. Strings (`<std/string.cch>`)

**Naming:** Language surface keeps `String` for UFCS ergonomics, but the C ABI is prefixed (`CCString`, `cc_string_*`). The compiler resolves short aliases automatically.

**Collections note:** Vectors/Maps are arena-backed by default. They may grow by allocating new buffers/tables in the arena and copying/rehashing; growth fails if the arena cannot satisfy the request. Optional heap-backed variants exist for tools/tests when explicitly included; generated code uses the arena-backed forms.

#### 1.1 Overview

Concurrent-C slices (`char[:]`) are efficient views for immutable string data. The stdlib builds on slices with:

- **String type:** Arena-backed growable string (used for building, formatting, accumulation).
- **Slice operations:** Non-owning UFCS methods on `char[:]` (split, trim, find, parse) that work with any slice.
- **Canonical string views:** `char[:0]` from `String.as_slice()` and `@slice("...")`.
- **Builder sugar:** `@string(expr, arena)` and `@string(policy, \`...\`, arena)` for direct and templated string construction.

All operations are accessible via UFCS method syntax for ergonomics:

```c
// Natural, chainable
str result = input
    .trim()
    .lower()
    .replace("foo", "bar")
    .slice(0, 10);

// Or fluent builder
String s = string_new(&arena);
s.append("Hello")
 .append(" ")
 .append("World");
```

---

#### 1.2 String Builder Type

```c
// Language surface: String
// C ABI (prefixed): typedef Vec_char CCString;   // String = Vec<char>
```

**Handle semantics:** `String` is a small, moveable handle to an arena-backed buffer (a Vec<char>). Copying a `String` aliases the same storage. To obtain an independent copy, use `as_slice().clone(a)`. String contents live until the arena is reset/freed.

**Direct construction surface**

```c
String string_new(Arena* a);
String cc_string_from(expr, Arena* a);            // expression-generic helper
String cc_string_from_slice(Arena* a, char[:] initial);
char[:0] @slice("...");
String @string(expr, Arena* a);
String @string(policy, `...`, Arena* a);

// UFCS surface methods
String* String.append(char[:] data);          // Append data, return for chaining
String* String.push(char[:] data);            // Alias of append
String* String.push_char(char c);             // Append single character
String* String.push_int(i64 value);           // Append formatted i64
String* String.push_float(f64 value);         // Append formatted f64
String* String.push_uint(u64 value);          // Append formatted u64
String* String.clear();                       // Clear contents, reuse allocation
char[:0] String.as_slice();                   // Get immutable sentinel view
size_t String.len();                          // Length in bytes (Vec<T> UFCS)
size_t String.cap();                          // Capacity (Vec<T> UFCS)
String <primitive>.to_str(Arena* a);          // e.g. 42.to_str(&arena)
```

**Normative lowering:**
- `s.append(data)` and `s.push(data)` lower to `cc_string_push(&s, data)`.
- `s.push_char(c)` lowers to `cc_string_push_char(&s, c)`.
- `s.push_int(v)` lowers to `cc_string_push_int(&s, v)`.
- `s.push_uint(v)` lowers to `cc_string_push_uint(&s, v)`.
- `s.push_float(v)` lowers to `cc_string_push_float(&s, v)`.
- `s.clear()` lowers to `cc_string_clear(&s)`.
- `s.as_slice()` lowers to `cc_string_as_slice(&s)`.
- `s.len()` / `s.cap()` lower to `cc_string_len(&s)` / `cc_string_cap(&s)`.
- `@slice("...")` lowers to a build-time canonical `char[:0]` via `cc_slice_from_cstr("...")`.
- `@string(expr, a)` lowers to `cc_string_from((expr), (a))`.
- `@string(policy, \`...\`, a)` lowers to `cc_string_new(a)` plus `cc_string_push_slice(...)` / `cc_string_push_policy(...)` calls for literal and interpolated segments.
- Built-in primitive `x.to_str(a)` forms lower to helpers such as `int_to_str(x, a)` or `double_to_str(x, a)`.

**Slice Lifetime:** The sentinel slice returned by `as_slice()` remains valid until the next mutating call on the same `String` (e.g., `append()`, `clear()`). For stable references, use `.clone()` to create an independent copy in the arena.

**Template slots:** `@string(policy, \`...\`, a)` accepts string-oriented slot expressions (`char*`, `char[:]`, `String`). Non-string values may bridge through the conventional `to_str(a)` UFCS helper when available.
Backtick template bodies preserve whitespace, indentation, and embedded newlines exactly as written, matching ordinary JavaScript template-literal whitespace behavior (no implicit dedent or trim).

**Builder Pattern (Fluent, UFCS-enabled):**

```c
Arena arena = arena(megabytes(1));

// Method chaining via UFCS
String sb = string_new(&arena);
sb.append("count=")
  .append("x")
  .push_int(42);
char[:0] result = sb.as_slice();  // "count=x42"

// Or more readable step-by-step
String greeting = string_new(&arena);
greeting.append("Hello");
greeting.push_char(' ');
greeting.append("world");
printf("%.*s\n", (int)greeting.as_slice().len, greeting.as_slice().ptr);

String msg = @string(42, &arena);
String html = @string(html_policy, `<h1>${title}</h1>`, &arena);
```

**Practical Examples:**

```c
Arena arena = arena(megabytes(1));

// Build JSON
String json = string_new(&arena);
json.append("{\"name\":\"")
    .append(name)
    .append("\",\"age\":")
    .push_int(age)
    .append("}");
char[:] json_str = json.as_slice();

// Build SQL query with conditionals
String query = string_new(&arena);
query.append("SELECT * FROM users WHERE 1=1");
if (has_filter_name)
    query.append(" AND name='")
         .append(filter_name)
         .append("'");
if (has_filter_age)
    query.append(" AND age > ")
         .push_int(min_age);
```

---

#### 1.3 String Slice Operations (char[:])

UFCS methods on immutable `char[:]` views. These are allocation-free and work with any slice.

**Normative lowering:** `char[:]` methods dispatch on the slice family itself. Representative emitted callees include `cc_slice_trim(...)`, `cc_slice_trim_set(...)`, and related `cc_slice_*` helpers for query, transform, and parsing operations.

##### Core Methods

```c
size_t  char[:].len();                          // Length in bytes

// Safe indexing (never traps). Returns a pointer into the slice or NULL.
char*   char[:].get(size_t index);              // NULL if out of bounds

// View (no allocation). Empty slice (len == 0) on invalid range.
char[:] char[:].slice(size_t start, size_t end);  // empty slice if start > end or end > len

// Copy to new allocation
char[:] char[:].clone(CCArena* a);              // Byte-for-byte copy into arena (UTF-8 unchanged)
char*   char[:].c_str(CCArena* a);              // Copy len bytes + NUL terminator; returns char* for C interop
```

**Slice Safety:** `.slice(start, end)` returns an empty slice (`len == 0`) if `start > end` or `end > len`. Otherwise, it returns a view (no allocation). This keeps the call total (non-trapping, non-erroring) and preserves the "empty slice is the absent sentinel" convention used across stdlib slice APIs.

**Example:**
```c
char[:] s = "hello";
assert(s.slice(1, 4) == "ell");        // valid range
assert(s.slice(4, 1).len == 0);        // invalid: start > end -> empty
assert(s.slice(0, 99).len == 0);       // invalid: end > len   -> empty
assert(s.slice(5, 5).len == 0);        // empty slice at end (valid, bordering)
```

##### Query Methods

```c
bool    char[:].is_empty();                    // Check if empty
bool    char[:].is_ascii();                    // Check if all ASCII
bool    char[:].starts_with(char[:] prefix);   // Check prefix
bool    char[:].ends_with(char[:] suffix);     // Check suffix
bool    char[:].contains(char[:] needle);      // Check contains
ssize_t char[:].index_of(char[:] needle);      // Find index (-1 if not found)
ssize_t char[:].last_index_of(char[:] needle); // Find last index (-1 if not found)
size_t  char[:].count(char[:] needle);         // Count occurrences
```

##### Transform Methods (return owned String or slice)

```c
String  char[:].upper(Arena* a);               // Uppercase → new String
String  char[:].lower(Arena* a);               // Lowercase → new String
char[:] char[:].trim();                        // Trim whitespace (view)
char[:] char[:].trim_left();                   // Trim left (view)
char[:] char[:].trim_right();                  // Trim right (view)
char[:] char[:].trim_set(char[:] chars);       // Trim custom chars (view)
String  char[:].replace(Arena* a, char[:] old, char[:] new);  // Replace all
String  char[:].repeat(Arena* a, size_t times); // Repeat n times
```

##### Parse Methods

```c
i64  !>(CCI64ParseError)   char[:].parse_i64();    // Parse to i64
u64  !>(CCI64ParseError)   char[:].parse_u64();    // Parse to u64
f64  !>(CCFloatParseError) char[:].parse_f64();    // Parse to f64
bool !>(CCBoolParseError)  char[:].parse_bool();   // Parse to bool ("true"/"false")
```

**Error types:**
```c
enum CCI64ParseError   { InvalidChar, Overflow, Underflow };
enum CCFloatParseError { InvalidChar, Overflow };
enum CCBoolParseError  { InvalidValue };
```

##### Split Methods

```c
struct CCStringSplitIter {
    char[:] remaining;
    char[:] delim;
};

CCStringSplitIter char[:].split(char[:] delim);                 // Create iterator
bool              CCStringSplitIter.next(char[:]* out);         // Advance iterator; true if a field was written
char[:][:]        char[:].split_all(CCArena* a, char[:] delim); // Collect all at once
```

**Examples:**

```c
// Slice query and trim
char[:] input = "  hello world  ";
char[:] trimmed = input.trim();        // "hello world"
bool has_hello = trimmed.contains("hello");  // true

// Parse with error handling
char[:] num_str = "42";
i64 !>(CCI64ParseError) result = num_str.parse_i64();
if (try i64 val = result) {
    printf("Parsed: %ld\n", val);
} else {
    printf("Parse error\n");
}

// Split and iterate
char[:] csv = "alice,bob,charlie";
CCStringSplitIter it = csv.split(",");
char[:] name;
while (it.next(&name)) {
    printf("Name: %.*s\n", (int)name.len, name.ptr);
}

// Split all at once
CCArena arena = arena(megabytes(1));
char[:][:] names = csv.split_all(&arena, ",");
for (size_t i = 0; i < names.len; i++) {
    printf("%.*s\n", (int)names[i].len, names[i].ptr);
}
```

---

#### 1.4 UTF-8 Notes

All strings are UTF-8. Basic operations (split, find, trim on whitespace) work on bytes and are safe for UTF-8. Multi-byte character handling (grapheme clusters, normalization) is deferred to Phase 2 if needed.

---

### 2. I/O (`<std/io.cch>` and `<std/fs.cch>`)

#### 2.1 Overview

Concurrent-C I/O wrappers provide safe alternatives to C's stdio.h and POSIX I/O:

- Replace errno with `T !>(CCIoError)` Result types.
- Wrap file handles in opaque types for safety.
- Provide sync and `@async` variants.
- Allocate into arenas where needed.
- Use UFCS methods for natural I/O operations.

**Non-blocking guarantee:** All pure in-memory operations (String builder, Vec<T>, Map<K,V>, char[:] slicing, trimming, parsing) are `@noblock`—they never yield to the scheduler and are safe to call in deadline-sensitive code.

**Blocking behavior:**

File open/read/write/sync operations are **stalling operations** as defined in § 7.8.5 of the language specification. All I/O operations may block indefinitely, may fail with `IoError::Busy` under executor saturation, and provide no cancellation guarantees outside of deadline scopes.

For complete blocking/stalling semantics, see **§ 7.8 (Blocking, Stalling, and Execution Contexts)**. For cancellation behavior inside deadline scopes (how suspension points check for deadline expiration), see **§ 7.5 (Cancellation & Deadline)**. Pure in-memory operations (strings, slices, Vec, Map) are guaranteed non-blocking and must always run inline (§ 7.8.7).

#### 2.2 Basic I/O Errors

```c
enum CCIoError {
    PermissionDenied,
    FileNotFound,
    InvalidArgument,
    Interrupted,
    OutOfMemory,
    Busy,              // Blocking pool saturated, queue is full
    ConnectionClosed,  // Normal closure (like EOF for streams; not an error condition)

    // Platform-specific error code preserved for diagnostics.
    // On POSIX: errno. On Windows: GetLastError()/WSAGetLastError() (implementation-defined).
    Other(i32 os_code),
};

enum CCParseError {
    InvalidUtf8,
    Truncated,
};

// Note: EOF is not an error. Streaming APIs return an empty slice (len == 0) at end-of-stream.
// EOF represents stream exhaustion, not failure; check data.len == 0 to detect EOF.
```

---

#### 2.3 File Type and Methods

```c
// Opaque file handle
typedef struct CCFile CCFile;

// Direct library-call constructors
CCFile !>(CCIoError)        cc_file_open(CCArena* a, char[:] path, char[:] mode);  // "r", "w", "a"
@async CCFile !>(CCIoError) cc_file_open_async(CCArena* a, char[:] path, char[:] mode);

// UFCS surface methods

// Streaming read: returns slice with data, empty slice on EOF.
// Reads up to n bytes; returns slice of actual bytes read.
// EOF: returns ok with empty slice (len == 0).
char[:] !>(CCIoError) CCFile.read(CCArena* a, size_t n);

// Read one line into arena (line ending handling: accepts \n and \r\n).
// EOF: returns ok with empty slice (len == 0).
char[:] !>(CCIoError) CCFile.read_line(CCArena* a);

// Read entire file into arena. Returns empty slice for empty files.
// This is NOT a streaming API — use read() or read_line() for streaming.
char[:] !>(CCIoError) CCFile.read_all(CCArena* a);

// Write all bytes from data.
size_t !>(CCIoError) CCFile.write(char[:] data);

// Read into caller-provided buffer (no allocation).
// For streaming scenarios where you want to reuse the same buffer.
// Returns bytes read; 0 on EOF.
size_t !>(CCIoError) CCFile.read_buf(void* buf, size_t n);

// Write from caller-provided buffer (no slice overhead).
// For streaming scenarios where you want to avoid slice construction.
// Returns bytes written.
size_t !>(CCIoError) CCFile.write_buf(const void* buf, size_t n);

i64    !>(CCIoError) CCFile.seek(i64 offset, int whence);   // SEEK_SET/CUR/END
i64    !>(CCIoError) CCFile.tell();                         // Current position
size_t !>(CCIoError) CCFile.size();                         // File size (0 for non-seekable)

// Flush to disk; observes flush errors.
void   !>(CCIoError) CCFile.sync();

// Close is best-effort and infallible (no error returned).
// Call sync() before close() to observe flush failures.
void                 CCFile.close();

// Async variants (same signatures, just async)
@async char[:] !>(CCIoError) CCFile.read_async(CCArena* a, size_t n);
@async char[:] !>(CCIoError) CCFile.read_line_async(CCArena* a);
@async char[:] !>(CCIoError) CCFile.read_all_async(CCArena* a);
@async size_t  !>(CCIoError) CCFile.write_async(char[:] data);
```

**Normative lowering:**
- `file.open(path, mode)` lowers to `cc_file_open(&file, path, mode)`.
- `file.read(a, n)` lowers to `cc_file_read(&file, a, n)`.
- `file.read_line(a)` lowers to `cc_file_read_line(&file, a)`.
- `file.read_all(a)` lowers to `cc_file_read_all(&file, a)`.
- `file.write(data)` lowers to `cc_file_write(&file, data)`.
- `file.read_buf(buf, n)` lowers to `cc_file_read_buf(&file, buf, n)`.
- `file.write_buf(buf, n)` lowers to `cc_file_write_buf(&file, buf, n)`.
- `file.seek(off, whence)` / `file.tell()` / `file.size()` / `file.sync()` / `file.close()` lower to the corresponding `cc_file_*` family function with `&file` as receiver.
- Async file methods use the same file family contract; implementations may lower them through direct `cc_file_*_async` calls or equivalent family wrappers.

**EOF Semantics (Unified):**

| Method | Return Type | EOF Behavior |
|--------|-------------|--------------|
| `read()` | `char[:] !>(CCIoError)` | Empty slice (`len == 0`) = EOF |
| `read_line()` | `char[:] !>(CCIoError)` | Empty slice (`len == 0`) = EOF |
| `read_all()` | `char[:] !>(CCIoError)` | N/A (reads entire file; empty slice for empty file) |

**Rule:** All streaming reads (`read()`, `read_line()`) return an empty slice on EOF. Check `data.len == 0` to detect EOF — there is no separate absent/EOF sentinel on top of the result.

**Examples:**

```c
CCArena arena = arena(megabytes(10));

// Read entire file (error handling)
CCFile !>(CCIoError) f = cc_file_open(&arena, "data.txt", "r");
if (try CCFile file = f) {
    char[:] content = try file.read_all(&arena);
    printf("Read %zu bytes\n", content.len);
    file.close();
} catch (CCIoError err) {
    printf("Error: %d\n", err);
}

// Read lines (empty slice = EOF)
CCFile !>(CCIoError) f = cc_file_open(&arena, "input.txt", "r");
if (try CCFile file = f) {
    while (true) {
        char[:] !>(CCIoError) line_result = file.read_line(&arena);
        if (cc_is_err(line_result)) {
            // Handle error
            break;
        }
        char[:] line = cc_value(line_result);
        if (line.len == 0) break;  // EOF (empty slice)
        printf("Line: %.*s\n", (int)line.len, line.ptr);
    }
    file.close();
}

// Write file
CCFile !>(CCIoError) out = cc_file_open(&arena, "output.txt", "w");
if (try CCFile file = out) {
    try file.write("Hello, world!\n");
    file.close();
}

// Async I/O
@async void process_file() {
    // Sync open is allowed; runtime may offload it to blocking pool if needed
    CCFile !>(CCIoError) f = cc_file_open(&arena, "data.txt", "r");
    if (try CCFile file = f) {
        char[:] data = try await file.read_all_async(&arena);
        process(data);
        file.close();
    }
}
```

#### 2.4 Standard Streams

```c
// UFCS methods on stdout/stderr (singletons)
void !>(CCIoError) std_out.write(char[:] data);
void !>(CCIoError) std_err.write(char[:] data);
// CCString overloads for ergonomics
void !>(CCIoError) std_out.write(CCString s);
void !>(CCIoError) std_err.write(CCString s);
// Overload resolution is handled by UFCS lowering; the compiler selects the
// best match and emits prefixed C ABI calls (`cc_std_out_write` or
// `cc_std_out_write_string`) with pointer-based signatures.

**Normative lowering:**
- `std_out.write(slice)` lowers to `cc_std_out_write(slice)`.
- `std_err.write(slice)` lowers to `cc_std_err_write(slice)`.
- `std_out.write(str)` lowers to `cc_std_out_write_string(&str)`.
- `std_err.write(str)` lowers to `cc_std_err_write_string(&str)`.

**UFCS receiver conversion (general rule):**
- Overload selection prefers an exact receiver type match.
- If no exact match, the compiler may apply these implicit receiver conversions (in order) to find a viable overload:
  1) `CCString -> char[:]` (view of contents via `cc_string_as_slice`)
  2) `char[N]` / string literal -> `char[:]`
  3) `CCSlice` alias -> `char[:]`
- If no overload is viable after these conversions, resolution fails.
```

**Examples:**

```c
std_out.write("Hello, world!\n");
std_err.write("An error occurred\n");
std_out.write(my_string); // CCString overload
```

#### 2.5 Structured Logging (`<std/log.cch>`)

Structured logging for servers and production systems. Supports multiple backpressure strategies (block, drop, sample) for different log types.

**Log Levels:**

```c
enum LogLevel {
    Debug,   // Development information
    Info,    // General operational events
    Warn,    // Potentially harmful situations
    Error,   // Error conditions
    Fatal,   // Fatal errors requiring shutdown
};
```

**Log Event:**

```c
struct LogEvent {
    LogLevel level;
    char[:] module;      // Module/component name (e.g., "http.handler", "db.pool")
    char[:] message;     // Log message (no allocation)
    u64 timestamp_ns;    // Nanoseconds since epoch
};
```

**Logging Functions:**

```c
// Log with drop strategy: never blocks request path
// Logs to stderr; drops silently if output is slow
void                 cc_log_drop(CCLogEvent evt);

// Log with block strategy: blocks up to timeout, fails request if timeout exceeded
// Use for audit/critical logs that cannot be dropped
void !>(CCIoError)   cc_log_block(CCLogEvent evt, Duration timeout);

// Log with sample strategy: deterministically keep ~rate fraction
// Use for high-volume logs (traces, metrics) that must be sparse
void                 cc_log_sample(CCLogEvent evt, f32 rate);

// Runtime configuration
struct LogConfig {
    LogLevel min_level;
    char[:] module_filter;  // Module name prefix filter (e.g., "http.*")
};

void Runtime.set_log_config(LogConfig cfg);
void Runtime.set_log_level(LogLevel level);
```

**Examples:**

```c
#include <ccc/std/prelude.cch>

LogEvent evt = {
    .level = Info,
    .module = "http.handler",
    .message = "GET /api/users 200 OK",
    .timestamp_ns = now_ns(),
};

// Access log (can drop if slow)
log_drop(evt);

// Audit log (fail request if timeout)
try log_block(evt, milliseconds(100));

// Trace log (keep 5% of events)
log_sample(evt, 0.05);
```

**Server Pattern:**

```c
@async @latency_sensitive void http_handler(Request req) {
    // Parse request (CPU work, inline)
    char[:] path = parse_path(req);
    
    // Access log (drop strategy, never blocks)
    LogEvent evt = {
        .level = Info,
        .module = "http",
        .message = path,
        .timestamp_ns = now_ns(),
    };
    log_drop(evt);
    
    // Process request...
    char[:] response = process(path);
    
    // Send response (stalling I/O, separate dispatch)
    try await send_response(req.fd, response);
}
```

**Design:**

- All logging functions are **non-allocating**.
- `LogEvent` is stack-allocated; messages are string slices.
- `log_drop()` never blocks (fail-safe for request handlers).
- `log_block()` allows controlled backpressure (for critical logs).
- `log_sample()` implements **deterministic sampling** (reproducible, fair).
- Runtime configuration is centralized; can be changed at startup.

**@latency_sensitive context:** Use `log_drop()` in `@latency_sensitive` handlers—it is `@noblock` and never yields. Avoid `log_block()` and `log_sample()` in deadline-sensitive code (they may suspend).

#### 2.6 Path Utilities (`<std/path.cch>`)

```c
// Path operations (mostly free functions, no state)
char[:] cc_path_join(CCArena* a, char[:] parent, char[:] child);  // Cross-platform path joining
char[:] cc_path_basename(char[:] path);     // Extract filename
char[:] cc_path_dirname(char[:] path);      // Extract directory
char[:] cc_path_extension(char[:] path);    // Extract extension

// Example: cross-platform
char[:] config = cc_path_join(&arena, home_dir, ".config/app.txt");
char[:] dir = cc_path_dirname(config);
char[:] name = cc_path_basename(config);
char[:] ext = cc_path_extension(config);
```

---

### 3. Collections (`<std/vec.cch>` and `<std/map.cch>`)

#### 3.1 Overview

CC's collections are **arena-backed**, **generic** (canonically via `::[...]` syntax), and **UFCS-enabled**. They enable safe, efficient data structures for concurrent systems: work queues, request buffers, caches, state tables.

#### 3.2 `CCVec::[T]` (Dynamic Array)

```c
// Generic dynamic array type (arena-backed)
typedef struct CCVec::[T] CCVec::[T];

// Direct library-call constructors
CCVec::[T] cc_vec_new::[T](CCArena* a);
CCVec::[T] cc_vec_with_capacity::[T](CCArena* a, size_t capacity);

// UFCS surface methods
void    CCVec::[T].push(T value);                  // Add element (grows as needed)
bool    CCVec::[T].pop(T* out);                    // Remove last into *out; false if empty
T*      CCVec::[T].get(size_t index);              // Bounds-safe get; NULL if out of bounds
T*      CCVec::[T].get_mut(size_t index);          // Mutable access; NULL if out of bounds

enum CCBoundsError { OutOfBounds };
void !>(CCBoundsError) CCVec::[T].set(size_t index, T value);   // Set with bounds check

void    CCVec::[T].clear();                        // Clear contents
size_t  CCVec::[T].len();                          // Length
size_t  CCVec::[T].cap();                          // Capacity
T[:]    CCVec::[T].as_slice();                     // View as T[:]

// Iterator
struct CCVecIter::[T] {
    CCVec::[T]* vec;
    size_t index;
};

CCVecIter::[T] CCVec::[T].iter();
bool           CCVecIter::[T].next(T* out);        // true if a value was written to *out
```

**Rule (get vs. get_mut):** Both return `T*` (a nullable pointer into the vector's buffer). `get` returns a `const`-like read-only view in source-level prose, but the underlying pointer is identical; `get_mut` exists as a distinct name for readability at call sites that will mutate. Returned pointers are valid until the next mutating operation on the vector (push/pop/set/clear or any growth).

**Normative lowering:** `CCVec::[T]` first lowers to the concrete container family `CCVec_<mangledT>`. Legacy `Vec::[T]` and `Vec<T>` spellings lower to the same family during the transition period. UFCS then lowers to that family contract. The visible concrete C family names are normative for interop: `CCVec_<mangledT>`, `CCVec_<mangledT>_init`, `CCVec_<mangledT>_push`, `CCVec_<mangledT>_get`, and related family members. Shared erased-core helpers remain an implementation detail.

**Examples:**

```c
CCArena arena = arena(megabytes(1));

// Work queue (UFCS method syntax)
CCVec::[Task::[void]] tasks = cc_vec_new::[Task::[void]](&arena);
tasks.push(async_work1());
tasks.push(async_work2());
tasks.push(async_work3());

// Iterate and await
CCVecIter::[Task::[void]] it = tasks.iter();
Task::[void] task;
while (it.next(&task)) {
    await task;
}

// Buffer for accumulation
CCVec::[char] buffer = cc_vec_new::[char](&arena);
for (size_t i = 0; i < input.len; i++) {
    buffer.push(input.ptr[i]);
}
char[:] result = buffer.as_slice();

// Bounds-safe access (pointer into the vector, NULL if out of bounds)
CCVec::[int] data = cc_vec_new::[int](&arena);
data.push(42);
int* val = data.get(0);      // non-NULL; *val == 42
int* oob = data.get(100);    // NULL (out of bounds)

// Size checks
if (data.len() > 0) {
    data.clear();
}
```

---

#### 3.3 CCMap::[K, V] (Hash Map)

`CCMap::[K, V]` is already defined in the Concurrent-C language spec. The stdlib provides it with UFCS methods:

```c
// Generic hash map (arena-backed)
typedef struct CCMap::[K, V] CCMap::[K, V];

// Direct library-call constructor
CCMap::[K, V] cc_map_new::[K, V](CCArena* a);

// UFCS surface methods
void    CCMap::[K, V].insert(K key, V value);     // Insert or update
V*      CCMap::[K, V].get(K key);                 // Lookup: pointer into table, NULL if absent
V*      CCMap::[K, V].get_mut(K key);             // Mutable reference, NULL if absent
bool    CCMap::[K, V].remove(K key);              // Remove (true if existed)
void    CCMap::[K, V].clear();                    // Clear all entries
size_t  CCMap::[K, V].len();                      // Number of entries
size_t  CCMap::[K, V].cap();                      // Capacity
```

**Normative lowering:** `CCMap::[K, V]` first lowers to a concrete container family such as `CCMap_int_char_ptr`. UFCS then lowers to that family contract. Implementations may realize the concrete family with direct symbols such as `CCMap_int_char_ptr_insert(&m, k, v)` or with thin wrappers/macros over a shared erased core; the family contract is normative, the erased core is an implementation detail.

**Rule (get returns pointer):** `get` / `get_mut` both return `V*` — a pointer directly into the map's table — or `NULL` if the key is absent. This matches the vector `get`/`get_mut` pattern and avoids copying `V` on each lookup. The returned pointer is valid until the next mutating operation on the map (`insert`, `remove`, `clear`, or any rehash).

**Note on Iteration:** Map iteration is intentionally deferred to Phase 2 to avoid prematurely locking in traversal order semantics. Phase 1 focuses on insertion, lookup, and removal.

**Examples:**

```c
CCArena arena = arena(megabytes(1));

// Simple cache (string → result)
CCMap::[char[:], int] cache = cc_map_new::[char[:], int](&arena);
cache.insert("key1", 100);
cache.insert("key2", 200);

int* val = cache.get("key1");    // non-NULL; *val == 100
int* miss = cache.get("key3");   // NULL

// State table for concurrent requests
struct Request { int id; char[:] path; };
CCMap::[int, Request] active = cc_map_new::[int, Request](&arena);

active.insert(req.id, req);
Request* found = active.get(42);
if (found) {
    process_request(*found);
}
active.remove(42);

// Conditional lookup and update
Request* r = active.get(id);
if (r) {
    r->path = new_path;
}
```

---

#### 3.4 Set<T> (Deferred)

`Set<T>` is deferred to Phase 2. Can be implemented as `Map<T, bool>` or a dedicated type; community feedback will guide the decision.

---

### 4. Directory & Filesystem (`<std/dir.cch>`)

#### 4.1 Overview

Cross-platform directory operations: iteration, path existence checks, directory creation/removal, and glob pattern matching. Works on macOS, Linux, BSD, and Windows.

#### 4.2 Directory Iteration

```c
// Directory entry type
enum CCDirEntryType {
    CC_DIRENT_FILE,
    CC_DIRENT_DIR,
    CC_DIRENT_SYMLINK,
    CC_DIRENT_OTHER
};

// Directory entry
struct CCDirEntry {
    char[:] name;           // Entry name (not full path)
    CCDirEntryType type;
};

// Open directory for iteration
CCDirIter*  !>(CCIoError) cc_dir_open(CCArena* arena, char[:] path);

// Read next entry (returns error with CC_IO_EOF when done)
CCDirEntry  !>(CCIoError) cc_dir_next(CCDirIter* iter, CCArena* arena);

// Close iterator
void cc_dir_close(CCDirIter* iter);
```

**Example:**

```c
CCArena arena = cc_heap_arena(megabytes(1));
CCDirIter* iter = try cc_dir_open(&arena, "src");

while (true) {
    CCDirEntry !>(CCIoError) entry_res = cc_dir_next(iter, &arena);
    if (cc_is_err(entry_res)) break;  // EOF or error

    CCDirEntry entry = cc_value(entry_res);
    printf("%s (%s)\n", entry.name.ptr,
           entry.type == CC_DIRENT_DIR ? "dir" : "file");
}

cc_dir_close(iter);
```

#### 4.3 Path Operations

```c
bool cc_path_exists(char[:] path);     // Check if path exists
bool cc_path_is_dir(char[:] path);     // Check if path is directory
bool cc_path_is_file(char[:] path);    // Check if path is regular file

bool !>(CCIoError) cc_dir_create(char[:] path);      // Create directory
bool !>(CCIoError) cc_dir_create_all(char[:] path);  // Create directory and parents
bool !>(CCIoError) cc_dir_remove(char[:] path);      // Remove empty directory
bool !>(CCIoError) cc_file_remove(char[:] path);     // Remove file

char[:]            cc_dir_cwd(CCArena* arena);       // Get current working directory
bool !>(CCIoError) cc_dir_chdir(char[:] path);       // Change working directory
```

#### 4.4 Glob Pattern Matching

```c
// Glob result: array of matching paths
struct CCGlobResult {
    char[:]* paths;     // Array of path slices
    size_t count;       // Number of matches
};

// Find files matching pattern
// Supports: * (any chars), ? (single char), ** (recursive)
CCGlobResult cc_glob(Arena* arena, char[:] pattern);

// Check if name matches pattern (no directory traversal)
bool cc_glob_match(char[:] pattern, char[:] name);
```

**Examples:**

```c
Arena arena = cc_heap_arena(megabytes(1));

// Find all .ccs files in current directory
CCGlobResult r1 = cc_glob(&arena, "*.ccs");
for (size_t i = 0; i < r1.count; i++) {
    printf("Found: %s\n", r1.paths[i].ptr);
}

// Find all .ccs files recursively
CCGlobResult r2 = cc_glob(&arena, "**/*.ccs");

// Find test files
CCGlobResult r3 = cc_glob(&arena, "tests/test_*.ccs");
```

---

### 5. Process Spawning (`<std/process.cch>`)

#### 5.1 Overview

Cross-platform process spawning, I/O piping, and environment management. Works on macOS, Linux, BSD, and Windows.

#### 5.2 Process Configuration

```c
struct CCProcessConfig {
    char[:] program;        // Program path or name
    char[:]* args;          // NULL-terminated argument array
    char[:]* env;           // Environment (NULL = inherit)
    char[:] cwd;            // Working directory (NULL = inherit)
    bool pipe_stdin;        // Create stdin pipe
    bool pipe_stdout;       // Create stdout pipe
    bool pipe_stderr;       // Create stderr pipe
    bool merge_stderr;      // Redirect stderr to stdout
};

struct CCProcessStatus {
    bool exited;            // Exited normally
    bool signaled;          // Killed by signal (POSIX)
    int exit_code;          // Exit code or signal number
};
```

#### 5.3 Process Spawning

```c
// Spawn with full configuration
CCProcess !>(CCIoError) cc_process_spawn(CCProcessConfig* config);

// Simple spawn (no pipes)
CCProcess !>(CCIoError) cc_process_spawn_simple(char[:] program, char[:]* args);

// Spawn shell command
CCProcess !>(CCIoError) cc_process_spawn_shell(char[:] command);
```

#### 5.4 Process Management

```c
// Wait for exit (blocking)
CCProcessStatus !>(CCIoError) cc_process_wait(CCProcess* proc);

// Check if exited (non-blocking, returns CC_IO_BUSY if running)
CCProcessStatus !>(CCIoError) cc_process_try_wait(CCProcess* proc);

// Send signal (POSIX: signal number; Windows: TerminateProcess)
bool !>(CCIoError) cc_process_kill(CCProcess* proc, int signal);

// Get process ID
int cc_process_id(CCProcess* proc);
```

#### 5.5 Process I/O

```c
// Write to stdin (requires pipe_stdin)
size_t !>(CCIoError) cc_process_write(CCProcess* proc, char[:] data);

// Read from stdout/stderr (requires pipe_stdout/pipe_stderr); empty slice = EOF
char[:] !>(CCIoError) cc_process_read(CCProcess* proc, CCArena* arena, size_t max);
char[:] !>(CCIoError) cc_process_read_stderr(CCProcess* proc, CCArena* arena, size_t max);

// Read all output until EOF
char[:] !>(CCIoError) cc_process_read_all(CCProcess* proc, CCArena* arena);
char[:] !>(CCIoError) cc_process_read_all_stderr(CCProcess* proc, CCArena* arena);

// Close stdin (signals EOF to child)
void cc_process_close_stdin(CCProcess* proc);
```

#### 5.6 Convenience: Run and Capture

```c
struct CCProcessOutput {
    char[:] stdout_data;
    char[:] stderr_data;
    CCProcessStatus status;
};

// Run command and capture all output (blocking)
CCProcessOutput !>(CCIoError) cc_process_run(CCArena* arena, char[:] program, char[:]* args);

// Run shell command and capture output
CCProcessOutput !>(CCIoError) cc_process_run_shell(CCArena* arena, char[:] command);
```

**Examples:**

```c
Arena arena = cc_heap_arena(megabytes(1));

// Run and capture output
CCProcessOutput out = try cc_process_run_shell(&arena, "ls -la");
printf("stdout: %.*s\n", (int)out.stdout_data.len, out.stdout_data.ptr);
printf("exit: %d\n", out.status.exit_code);

// Spawn with pipes for interactive use
CCProcessConfig cfg = {
    .program = "cat",
    .args = (char*[]){"cat", NULL},
    .pipe_stdin = true,
    .pipe_stdout = true
};
CCProcess proc = try cc_process_spawn(&cfg);

try cc_process_write(&proc, "hello\n");
cc_process_close_stdin(&proc);

char[:] output = try cc_process_read_all(&proc, &arena);
CCProcessStatus status = try cc_process_wait(&proc);
```

#### 5.7 Environment

```c
// Get environment variable (empty if not set)
char[:] cc_env_get(Arena* arena, char[:] name);

// Set/unset environment variable for current process
bool !>(CCIoError) cc_env_set(char[:] name, char[:] value);
bool !>(CCIoError) cc_env_unset(char[:] name);
```

---

## Module Structure

```
<std/prelude.cch>        // Common imports (String, File, Vec, Map, etc.)
<std/string.cch>         // String builder and char[:] methods
<std/io.cch>             // File, StdStream
<std/log.cch>            // Structured logging (LogEvent, log_drop/block/sample)
<std/fs.cch>             // Path utilities
<std/dir.cch>            // Directory iteration, glob patterns
<std/process.cch>        // Process spawning, environment
<std/vec.cch>            // Vec<T> dynamic array with UFCS methods
<std/map.cch>            // Map<K, V> hash map with UFCS methods
<std/net.cch>            // TCP/UDP sockets, Listener
<std/tls.cch>            // TLS client/server (wraps BearSSL/mbedTLS)
<std/http.cch>           // HTTP client (http_get, http_post, HttpClient)
<std/dns.cch>            // DNS resolution (dns_lookup)
<std/server.cch>         // server_loop canonical shell
<std/error.cch>          // Error enums (IoError, NetError, HttpError, etc.)
<cc_atomic.cch>          // Portable atomic operations (runtime header, not std/)
```

**Prelude Safety:** `<std/prelude.cch>` performs no implicit allocation or runtime initialization. It is a pure header include with zero hidden costs.

**Updated Prelude Example:**

```c
#include <ccc/std/prelude.cch>

@async void main() {
    Arena arena = arena(megabytes(1));
    
    // String builder (UFCS)
    String greeting = string_new(&arena);
    greeting.append("Hello").append(" ").append("World");
    std_out.write(greeting.as_slice());
    std_out.write("\n");
    
    // Work queue (UFCS)
    Vec<Task<void>> tasks = vec_new<Task<void>>(&arena);
    tasks.push(async_work1());
    tasks.push(async_work2());
    
    // State map (UFCS)
    Map<int, char[:] > state = map_new<int, char[:]>(&arena);
    state.insert(1, "processing");
    
    // Run all tasks
    VecIter<Task<void>> it = tasks.iter();
    Task<void> t;
    while (it.next(&t)) {
        await t;
    }

    // File I/O (UFCS)
    File !>(IoError) f = file_open(&arena, "data.txt", "r");
    if (try File file = f) {
        char[:] data = try file.read_all(&arena);

        // String processing (UFCS)
        char[:] trimmed = data.trim();
        StringSplitIter lines = trimmed.split("\n");
        char[:] line;
        while (lines.next(&line)) {
            std_out.write(line);
            std_out.write("\n");
        }

        file.close();
    }
}
```

---

## Implementation Notes

### Headers vs. Linkage

**Phase 1 (strings, basic I/O):** Mostly inline/static functions in headers. Minimal linkage.

**Future:** If complex (e.g., async I/O needing thread pool integration), provide optional libstd.a. Users can link or provide custom implementations.

### Arena Checkpoints

An **arena checkpoint** captures the current allocation state of an arena and allows later restoration (to bound memory growth in long-lived tasks).

**Standard library interface:**

```c
typedef struct ArenaCheckpoint ArenaCheckpoint;

ArenaCheckpoint arena_checkpoint(Arena*);
void arena_restore(ArenaCheckpoint);
```

**Semantics:**

- Restoring a checkpoint releases all allocations performed after the checkpoint.
- Checkpoints MUST NOT invalidate allocations made prior to the checkpoint.
- Arena checkpoints do not alter arena ownership or lifetime rules.
- Taking a checkpoint starts a fresh arena provenance epoch for subsequent allocations.
- Restoring a checkpoint restores the checkpoint's provenance epoch so post-checkpoint allocations become stale while prior allocations remain valid.

### Blocking Pool and Saturation Handling

Certain stdlib operations may stall indefinitely: file I/O, sync locks, sleep, DNS, etc. These use a bounded thread pool to avoid blocking the async reactor thread.

**Blocking-class operations:**
```c
File.read()
File.read_all()
File.read_line()
File.write()
File.sync()

Mutex<T>.lock()           // Future: if exposed
// sockets, DNS, etc. (future)
```

**Non-blocking operations (run inline in async task, no pool overhead):**
```c
char[:].trim()
char[:].split()
char[:].parse_i64()
Vec<T>.push()
Map<K,V>.insert()
// All pure CPU computation
```

**Pool limits and saturation:**

The blocking pool has:
- `max_threads`: number of worker threads (default: 2× CPU count)
- `max_queue`: maximum pending operations (default: 1000)

When `max_queue` is full, blocking operations return `IoError::Busy`. This is observable and lets the application decide whether to backoff, reject, or scale.

**Configuration:**
```c
Runtime.set_blocking_pool(
    .max_threads = 32,
    .max_queue = 1000
);
```

**Handling saturation:**
```c
@async void process_with_backoff() {
    CCFile !>(CCIoError) f = cc_file_open(&arena, path, "r");
    if (try CCFile file = f) {
        int retry_count = 0;
        while (true) {
            char[:] !>(CCIoError) line_result = file.read_line(&arena);
            if (cc_is_err(line_result)) {
                CCIoError err = cc_error(line_result);
                if (err.kind == CC_IO_BUSY) {
                    if (retry_count++ > 3) {
                        return cc_err(IoError::Busy);
                    }
                    await sleep(milliseconds(10 * retry_count));
                    continue;
                }
                // Other error, propagate
                return cc_err(err);
            }
            retry_count = 0;

            char[:] line = cc_value(line_result);
            if (line.len == 0) break;  // EOF (empty slice)
            process(line);
        }
        file.close();
    }
}
```

**No eviction:** Pending operations are not killed on saturation. Bounded queue + fail-fast is safer than eviction (which risks corrupting `@scoped` resources, transactions, or in-flight cleanup).

### UFCS Lowering Model

The pattern is consistent with the language and with the main specification's type-owned UFCS model:

> **The lowering contract is normative. `s.len()` dispatches from the resolved receiver type and lowers to that type family's stdlib callee contract.**

For each method on a type `T`:

```c
// Direct library-call / C ABI shape for a pointer-style family
size_t T_len(T* self);

// UFCS surface form
size_t T.len();  // lowers through T's UFCS family
```

This allows both function composition and ergonomic method chaining.

### Generic Container Syntax Lowering

The `CCVec::[T]` and `Map::[K, V]` syntax is **compile-time sugar** that lowers to concrete C family types. Those family names are stable at the C boundary and are part of the interop contract even when the implementation routes the actual storage/manipulation work through shared erased-core helpers:

```c
// CC source code
CCVec::[int] v = cc_vec_new::[int](&arena);
v.push(42);
v.push(100);
int* val = v.get(0);

// Lowers to (generated C)
CCVec_int v = CCVec_int_init(&arena, CC_VEC_INITIAL_CAP);
CCVec_int_push(&v, 42);
CCVec_int_push(&v, 100);
int* val = CCVec_int_get(&v, 0);
```

**Lowering rules:**

| Surface Syntax | Lowered Form |
|----------------|--------------|
| `CCVec::[T]` | `CCVec_mangled` where `mangled` is the canonical type-parameter mangling for `T` |
| `Vec::[T]` / `Vec<T>` | Same lowered family as `CCVec::[T]` during the transition period |
| `cc_vec_new::[T](&arena)` | `CCVec_mangled_init(&arena, CC_VEC_INITIAL_CAP)` |
| `Map<K, V>` | `Map_mangledK_mangledV` |
| `map_new<K, V>(&arena)` | `Map_mangledK_mangledV_init(&arena)` |
| `v.method(args)` | `CCVec_mangled_method(&v, args)` for vectors; `Map_mangledK_mangledV_method(&m, args)` for maps |

**Type mangling examples:**

| Type Parameter | Mangled Name |
|----------------|--------------|
| `int` | `int` |
| `char*` | `char_ptr` |
| `int*` | `int_ptr` |
| `size_t` | `size_t` |
| `struct Foo` | `struct_Foo` |

**Container declarations** are automatically emitted based on types used:

```c
// Automatically generated for CCVec::[int] usage
CC_VEC_DECL_ARENA(int, CCVec_int)

// Automatically generated for Map<int, char*> usage
CC_MAP_DECL_ARENA(int, char_ptr, Map_int_char_ptr, cc_map_hash_i32, cc_map_eq_i32)
```

### Testing

All functions are covered by **Spec Tests**—normative, executable tests in `.cc` format that serve as both spec and validation. Implementations must pass all Spec Tests.

**Example Spec Test:**

```c
@test "string slice operations" {
    Arena arena = arena(kilobytes(10));
    char[:] input = "  hello, world!  ";

    // Test trim
    char[:] trimmed = input.trim();
    assert(trimmed.len == 13);  // "hello, world!"

    // Test split
    StringSplitIter it = trimmed.split(", ");
    char[:] p1;
    assert(it.next(&p1));
    assert(p1.len == 5);        // "hello"

    // Test contains
    assert(trimmed.contains("world"));
    assert(!trimmed.contains("xyz"));

    // Test parse
    i64 !>(I64ParseError) val = "42".parse_i64();
    assert(try i64 v = val && v == 42);
}

@test "vec and map methods" {
    Arena arena = arena(kilobytes(10));

    // Vec<T> methods
    Vec<int> v = vec_new<int>(&arena);
    v.push(10);
    v.push(20);
    assert(v.len() == 2);
    int* val = v.get(0);
    assert(val && *val == 10);

    // Map<K,V> methods
    Map<char[:], int> m = map_new<char[:], int>(&arena);
    m.insert("x", 100);
    int* result = m.get("x");
    assert(result && *result == 100);
}
```

### Portability

**Windows/Unix Differences:**
- Path separators: `path_join()` handles abstraction.
- Line endings: `read_line()` handles both \n and \r\n transparently.
- I/O errors: Map OS-specific codes to `IoError` enum.

---

### 4. Server Shell (`<std/server.cch>`)

The `server_loop` function provides a canonical server shell that handles connection acceptance, worker spawning, deadline enforcement, arena management, TLS wrapping, and keep-alive. It supports unary request/response and long-lived connection patterns (WebSockets, SSE, streaming, keep-alive, raw TCP).

**Core Abstractions:**

```c
// CCDuplex: unified interface for bidirectional, closeable streams
// Used by TLS wrappers, WebSocket handlers, HTTP/2, raw protocols, etc.
struct CCDuplex {
    @async char[:] !>(CCIoError) read(CCArena* a);        // Read chunk; empty slice = EOF
    @async void    !>(CCIoError) write(char[:] bytes);    // Write chunk
    @async void    !>(CCIoError) shutdown(CCShutdownMode mode);  // Half-close: Read, Write, or Both
    @async void    !>(CCIoError) close();                 // Close (equivalent to shutdown(Both))
};

enum CCShutdownMode {
    Read,   // Stop reading; keep write side open (recv FIN, continue sending)
    Write,  // Stop writing; keep read side open (send FIN, continue receiving)
    Both,   // Close both sides
};

**ABI Contract (Normative):**

All interface values (including CCDuplex) lower to a two-pointer layout:

```c
typedef struct {
    void* self;                    // Opaque receiver state
    const CCDuplexVTable* vt;      // Method table
} CCDuplex;

typedef struct {
    Task_CharSliceIoErr (*read)(void* self, CCArena* a);
    Task_VoidIoErr      (*write)(void* self, CCSlice bytes);
    Task_VoidIoErr      (*shutdown)(void* self, CCShutdownMode mode);
    Task_VoidIoErr      (*close)(void* self);
} CCDuplexVTable;
```

**Ownership Rules (Normative):**

- Duplex value is a lightweight handle; it does NOT own the underlying resource
- `self` pointer lifetime is determined by context:
  - Raw socket Duplex: `self` points to runtime state (valid while connection open)
  - TLS-wrapped Duplex: `self` points to TLS session (valid while connection open)
  - Custom protocol Duplex: `self` may point to user state (valid while handler running)
- Method calls do NOT capture closures or additional environment beyond vtable + self
- **Closure responsibility:** For server shell context, see **ServerAction ownership rules** below. After calling `close()` or `shutdown(Both)`, the handler must not call any other methods on the same Duplex
- Duplex values are not thread-safe; they must not be shared across threads (use `send_take()` requires a `Duplex!SendDuplex` marker type, future phase)

// Request from client
struct Request {
    int fd;                      // Socket file descriptor (do not close; server manages)
    char[:] method;              // HTTP method ("GET", "POST", etc.)
    char[:] path;                // URL path ("/api/users", etc.)
    char[:] headers;             // Raw HTTP headers
    Duplex body;                 // Request body stream (use read() for chunked uploads)
};

// Response to send back
struct Response {
    u16 status;                  // HTTP status (200, 404, 500, etc.)
    char[:] headers;             // Response headers
    char[:] body;                // Response body (small/unary fast path; empty = use stream)
    CCDuplex* stream;            // Streaming response (NULL = unary); use write() + shutdown(Write) to end
};

// Handler returns either unary response or takes over connection
enum ServerAction {
    Reply(Response),                    // Send response; connection lifetime controlled by ServerMode + headers
    Takeover(ConnHandlerFn),            // Handler takes ownership of connection
};

**Ownership Rules (Normative):**

- **Reply case:** Server shell retains ownership of the Duplex. After handler returns `Reply(resp)`, server calls `send_response()` and manages further I/O per ServerMode:
  - Http1_Close: shell calls `conn.shutdown(Both)` or closes
  - Http1_KeepAlive: shell continues reading requests; loop owns the connection
  - RawTcp: undefined (should not reach Reply in RawTcp mode)
  
- **Takeover case:** Ownership of the Duplex transfers to `takeover_fn` when handler returns `Takeover(fn)`. Server shell does NOT call any methods on the Duplex after transfer. Takeover handler is responsible for:
  - All I/O (reads, writes, shutdowns)
  - Closure: must call `conn.close()` or `conn.shutdown(Both)` before returning
  - After `close()` / `shutdown(Both)`, no further method calls
  - Returning from takeover_fn signals to server: "connection now closed"
  
- **Duplex lifetime:** The Duplex is valid only within the handler (or takeover_fn). After handler returns (Reply or Takeover), it is invalid and must not be used.

- **Connection state:** If handler returns `Reply` in Http1_KeepAlive, the same Duplex is reused for the next request (server ownership). If handler returns `Takeover`, the Duplex is consumed and no further requests are parsed on that connection.

// Takeover handler (runs in a long-lived context)
typedef @async void !>(CCIoError) (*ConnHandlerFn)(CCDuplex* conn, CCArena* conn_arena);

// Unary request/response handler
typedef @async ServerAction !>(CCIoError) (*ServerHandlerFn)(Request* req, CCArena* req_arena);

// Server modes (how to treat connections)
enum ServerMode {
    Http1_Close,      // One request per connection; close after response
    Http1_KeepAlive,  // Multiple requests per connection; close on "Connection: close" or timeout
    RawTcp,           // No HTTP parsing; handler decides protocol
};

// TLS configuration
struct TlsConfig {
    char[:] cert_path;           // Path to certificate file
    char[:] key_path;            // Path to private key file
    // Implementation: TLS handshake is performed after accept;
    // connection becomes a Duplex that reads/writes decrypted bytes.
    // SNI, cipher, peer cert available via TLS metadata (Phase 2).
};

// Configuration for server_loop
struct ServerConfig {
    u16 port;                              // Listen port
    size_t max_workers;                    // Number of connection handler tasks
    size_t max_connections;                // Max concurrent connections
    Duration request_timeout;              // Per-request deadline (unary handlers only)
    ServerHandlerFn handler;               // User-provided handler
    ServerMode mode;                       // Http1_Close, Http1_KeepAlive, or RawTcp
    TlsConfig* tls;                        // Optional TLS wrapping (NULL = plaintext)
    
    // Optional lifecycle callbacks
    bool (*on_request_start)(Request* req);
    void (*on_request_end)(Request* req, Response* resp, Duration elapsed);
};
```

**Main Entry Point:**

```c
// Accept connections, perform TLS handshake if configured, spawn workers, manage lifetimes
@async void !>(CCIoError) server_loop(ServerConfig cfg);
```

**Lifetime and Deadline Rules:**

1. **Unary handler (returns `Reply`):**
   - Server wraps handler call in `with_deadline(request_timeout)`
   - Handler must return within deadline
   - Server sends response, resets `req_arena`, continues loop (keep-alive mode) or closes (close mode)
   - No deadline passed to handler; deadline is server's concern

2. **Takeover handler (returns `Takeover`):**
   - No deadline applied by server
   - Handler receives `conn_arena` (separate from req_arena, lives until connection closes)
   - Handler controls its own deadline semantics (may use `with_deadline()` explicitly, or no deadline)
   - Handler is responsible for all I/O, shutdown, and closure via Duplex
   - After handler returns, `conn_arena` is reset; connection closed

3. **Arena reset:**
   - `req_arena`: Reset after each request (fast; per-request lifetime)
   - `conn_arena`: Created when connection accepted; reset when connection closes (longer lifetime for stateful protocols)

4. **TLS interaction:**
   - If `cfg.tls` is present, server performs TLS handshake after accept
   - Handshake failures return `IoError::...` and connection is closed
   - Successful handshake wraps the raw fd in a Duplex (decryption/encryption transparent)
   - Handler receives Duplex (either raw fd or TLS-wrapped); no change to handler code

5. **Keep-alive semantics (Http1_KeepAlive mode):**
   - After each request-response cycle, server checks for "Connection: close" header in response
   - If absent, server loops back to read next request (reusing `conn_arena` for connection state)
   - Timeout or I/O error closes connection
   - Per-request deadline still applies to unary handlers

**Internal Pattern (Illustrative):**

```c
@async void !>(CCIoError) server_loop(ServerConfig cfg) {
    int listener = try listen(cfg.port);

    CCNursery* n = cc_nursery_create(NULL)
        !>(e) { return cc_err(CCIoError::OutOfMemory); }
        @destroy;
    {
        for (size_t i = 0; i < cfg.max_workers; i++) {
            n->spawn(() => server_worker(&cfg, listener));
        }
    }
}

@async void !>(CCIoError) server_worker(ServerConfig* cfg, int listener) {
    while (true) {
        // Accept connection (raw socket)
        int raw_fd = try await accept(listener);

        // Perform TLS handshake if configured
        CCDuplex conn = if (cfg.tls) {
            try tls_handshake(raw_fd, cfg.tls)
        } else {
            CCDuplex.from_fd(raw_fd)
        };

        // Connection-scoped arena (lives until connection closes)
        CCArena conn_arena = arena(megabytes(1));

        // Keep-alive loop: one or more requests per connection
        bool keep_alive = true;
        while (keep_alive) {
            // Request-scoped arena (reset per request)
            CCArena req_arena = arena(megabytes(1));

            // Handle one request
            @match {
                case ServerAction !>(CCIoError) action = handle_request(&conn, &req_arena, &conn_arena, cfg):
                    // Check for keep-alive
                    if (cfg.mode == Http1_KeepAlive && action != Takeover && !has_connection_close_header(action)) {
                        arena_reset(&req_arena);
                        continue;  // Loop for next request
                    } else {
                        keep_alive = false;  // Close connection
                    }

                case is_cancelled() | CCIoError err:
                    keep_alive = false;  // Break on error or cancellation
            }
        }

        // Close connection (CCDuplex.close or raw_fd)
        try conn.close();
        arena_reset(&conn_arena);
    }
}

@async ServerAction !>(CCIoError) handle_request(CCDuplex* conn, CCArena* req_arena, CCArena* conn_arena, ServerConfig* cfg) {
    // Read request
    Request req = try await read_request_from_duplex(conn, req_arena);

    if (cfg.on_request_start) cfg.on_request_start(&req);

    // Call handler with deadline (unary only)
    with_deadline(deadline_after(cfg.request_timeout)) {
        ServerAction !>(CCIoError) action = try cfg.handler(&req, req_arena);

        // Branch on response type
        @match {
            case Response resp = action (Reply):
                if (cfg.on_request_end) cfg.on_request_end(&req, &resp, deadline_remaining());
                try await send_response_to_duplex(conn, &resp);
                return cc_ok(resp);  // Return response for keep-alive check

            case ConnHandlerFn takeover_fn = action (Takeover):
                // Handler is taking over the connection
                try await takeover_fn(conn, conn_arena);
                // Takeover handler called close(); connection finished
                return cc_err(CCIoError::ConnectionClosed);  // Signal to break keep-alive loop
        }
    }
}
```

**Usage Example: Unary Request/Response (HTTP/1.1 with Keep-Alive)**

```c
#include <ccc/std/prelude.cch>
#include <ccc/std/server.cch>

@async @latency_sensitive ServerAction !>(CCIoError) api_handler(Request* req, CCArena* req_arena) {
    if (req.path == "/api/users") {
        User[] users = try await db_get_users(req_arena);
        char[:] json = encode_json(users, req_arena);
        Response resp = {
            .status = 200,
            .headers = "Content-Type: application/json\r\n",
            .body = json
        };
        return ServerAction.Reply(resp);
    } else {
        return ServerAction.Reply({
            .status = 404,
            .body = "Not Found"
        });
    }
}

@async void main() {
    ServerConfig cfg = {
        .port = 8080,
        .max_workers = 32,
        .request_timeout = seconds(5),
        .handler = api_handler,
        .mode = Http1_KeepAlive,  // Multiple requests per connection
        // .tls = TlsConfig { .cert_path = "cert.pem", .key_path = "key.pem" }
    };
    
    try await server_loop(cfg);
}
```

**Usage Example: WebSocket Upgrade (Takeover)**

```c
@async ServerAction !>(CCIoError) websocket_handler(Request* req, CCArena* req_arena) {
    if (req.path == "/ws" && req.headers.contains("Upgrade")) {
        // Return takeover to handle WebSocket protocol
        return ServerAction.Takeover(websocket_connection);
    } else {
        return ServerAction.Reply({.status = 400, .body = "Not a WebSocket request"});
    }
}

@async void !>(CCIoError) websocket_connection(CCDuplex* conn, CCArena* conn_arena) {
    // Perform WebSocket handshake
    try await ws_handshake(conn);

    // Handle messages on connection (lives as long as connection lives)
    while (true) {
        char[:] frame = try await ws_read_frame(conn, conn_arena);
        if (frame.len == 0) break;  // Connection closed (empty frame = EOF sentinel)

        // Process frame; allocations live in conn_arena
        try await process_ws_message(conn, frame, conn_arena);
    }

    // Close connection
    try await conn.close();
}
```

**Usage Example: Streaming Response (SSE)**

```c
@async ServerAction !>(CCIoError) sse_handler(Request* req, CCArena* req_arena) {
    if (req.path == "/events") {
        // For SSE, use Takeover to control the connection directly
        return ServerAction.Takeover(sse_connection);
    } else {
        return ServerAction.Reply({.status = 404});
    }
}

@async void !>(CCIoError) sse_connection(CCDuplex* conn, CCArena* conn_arena) {
    // Send SSE headers
    try await conn.write("HTTP/1.1 200 OK\r\n");
    try await conn.write("Content-Type: text/event-stream\r\n");
    try await conn.write("Connection: keep-alive\r\n\r\n");
    
    // Stream events
    for (size_t i = 0; i < 100; i++) {
        char[:] event = format_event(i, conn_arena);
        try await conn.write(event);
        await sleep(milliseconds(1000));
    }
    
    // Signal end by closing write side
    try await conn.shutdown(Write);
}
```

**Usage Example: TLS with HTTP/1.1**

```c
@async void main() {
    ServerConfig cfg = {
        .port = 443,
        .max_workers = 32,
        .request_timeout = seconds(5),
        .handler = api_handler,
        .mode = Http1_KeepAlive,
        .tls = &(TlsConfig) {
            .cert_path = "/etc/certs/server.crt",
            .key_path = "/etc/certs/server.key"
        }
    };
    
    try await server_loop(cfg);  // Handlers receive decrypted requests
}
```

**Usage Example: Raw TCP Protocol (Takeover Mode)**

```c
@async ServerAction !>(CCIoError) raw_protocol_handler(Request* req, CCArena* req_arena) {
    // For raw TCP (not HTTP), handler always returns Takeover
    // The "Request" struct is just for convenience; raw protocols ignore it
    return ServerAction.Takeover(raw_protocol_loop);
}

@async void !>(CCIoError) raw_protocol_loop(CCDuplex* conn, CCArena* conn_arena) {
    // Speak custom protocol directly via conn.read() / conn.write()
    while (true) {
        char[:] msg = try await conn.read(conn_arena);
        if (msg.len == 0) break;  // EOF (empty slice)

        char[:] response = process_protocol_message(msg, conn_arena);
        try await conn.write(response);
    }

    try await conn.close();
}

@async void main() {
    ServerConfig cfg = {
        .port = 9000,
        .max_workers = 32,
        .handler = raw_protocol_handler,
        .mode = RawTcp,  // No HTTP parsing; handler speaks protocol directly
    };
    
    try await server_loop(cfg);
}
```

**Design Notes:**

- `server_loop` is just the **shell**: accept, TLS wrap, deadline, arena management, keep-alive loop, unary dispatch, takeover transfer.
- User writes handlers with full control; handler signature is uniform across all modes.
- **Duplex abstraction:** Unifies raw socket, TLS-wrapped socket, HTTP/2 streams, WebSocket frames, etc. Handler code doesn't care about the underlying transport.
- **Unary handlers:** Fast-path for HTTP, gRPC, JSON-RPC. Deadline applied by server. Return `Reply(Response)`.
- **Takeover handlers:** For long-lived protocols (WebSocket, SSE, raw TCP, HTTP/2 with streams). No deadline. Return `Takeover(fn)` and receive `Duplex*` + `conn_arena`.
- **Keep-alive semantics:** In `Http1_KeepAlive` mode, server loops and reuses connection until "Connection: close" or error.
- **TLS:** First-class config; transparent to handlers. Duplex hides whether traffic is raw or TLS-wrapped.
- **Arena management:**
  - `req_arena`: Reset per request (fast; allows per-request pooling)
  - `conn_arena`: Reset per connection (allows connection state, stateful protocols)
  - For long-lived connections processing many messages, use arena checkpoint pattern to avoid unbounded growth:
    ```c
    @async void !>(CCIoError) long_lived_handler(CCDuplex* conn, CCArena* conn_arena) {
        size_t message_count = 0;
        CCArenaCheckpoint cp = arena_checkpoint(conn_arena);
        while (true) {
            char[:] msg = try await conn.read(conn_arena);
            if (msg.len == 0) break;  // EOF
            process(msg);
            message_count++;
            // Restore arena every N messages to prevent unlimited growth
            if (message_count % 100 == 0) {
                arena_restore(cp);
                cp = arena_checkpoint(conn_arena);
                message_count = 0;
            }
        }
        try await conn.close();
    }
    ```
- **Deadline and Cancellation:** For deadline semantics and how cancellation is checked at suspension points, see language spec **§ 7.5 (Cancellation & Deadline)** and **§ 3.2 (Suspension Points)**. This server shell applies `request_timeout` deadline to unary handlers; takeover handlers control their own deadline semantics (or use no deadline for long-lived connections like WebSocket).
- **Deadline narrative:** Unary handlers inherit deadline from `request_timeout`. Takeover handlers control their own deadline (or none). This makes streaming safe-by-default (server times out slow clients) while allowing long-lived connections (takeover handler explicitly manages timeouts).
- **Half-close:** Use `CCDuplex.shutdown(Read)` or `CCDuplex.shutdown(Write)` for proxies and protocols needing unidirectional closure. Standard sentinel `CCIoError::ConnectionClosed` indicates normal remote closure (not an error).
- **Multiplexed connections (Phase 2):** For HTTP/2 and gRPC, a future `MuxConn` abstraction will manage multiple independent streams over a single TCP/TLS connection, with each stream receiving its own `Duplex` and `stream_arena`. For now, Duplex assumes 1:1 connection-to-stream.
- Workers are spawned in a nursery; graceful shutdown on scope exit.
- **Shutdown (Phase 2):** Graceful shutdown via cancellation signal coming in Phase 2. Currently, `server_loop` runs until error or process exit.

**Extensibility:**

Users can:
- Return `Reply` for unary request/response patterns (HTTP, gRPC, JSON-RPC)
- Return `Takeover` for long-lived protocols (WebSocket, SSE, raw TCP, HTTP/2, custom protocols)
- Use `Duplex` for streaming request/response bodies (read for uploads, write for SSE/streaming)
- Implement custom protocol parsing in handlers
- Use `Duplex` interface for any bidirectional, closeable stream
- Configure TLS at the server level (decryption transparent to handlers)
- Choose keep-alive mode (Http1_Close for simplicity, Http1_KeepAlive for efficiency)
- Use `conn_arena` for connection state that must live beyond one request

---

## Rationale and Examples

### Before (Without Stdlib)

```c
#include <stdio.h>
#include <string.h>
#include <ctype.h>

void process_csv(char* filename) {
    FILE* f = fopen(filename, "r");
    if (!f) { perror("fopen"); return; }
    
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // Remove newline
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
        
        // Split by comma (no safe way, use strtok)
        char* token = strtok(line, ",");
        while (token) {
            // Trim whitespace manually
            char* start = token;
            while (isspace(*start)) start++;
            char* end = start + strlen(start) - 1;
            while (end > start && isspace(*end)) *end-- = '\0';
            
            printf("Field: %s\n", start);
            token = strtok(NULL, ",");
        }
    }
    fclose(f);
}
```

**Issues:** strtok modifies input, manual trimming, no error handling, imperative loops.

### After (With UFCS Stdlib)

```c
#include <ccc/std/prelude.cch>

@async void process_csv(char[:] filename) {
    Arena arena = arena(megabytes(1));
    
    CCFile !>(CCIoError) f = cc_file_open(&arena, filename, "r");
    if (try CCFile file = f) {
        while (true) {
            char[:] !>(CCIoError) line_result = file.read_line(&arena);
            if (cc_is_err(line_result)) break;  // Error
            char[:] line = cc_value(line_result);
            if (line.len == 0) break;  // EOF (empty slice)

            // UFCS: trim and split naturally
            CCStringSplitIter it = line.trim().split(",");
            char[:] field;
            while (it.next(&field)) {
                cc_std_out.write("Field: ");
                cc_std_out.write(field.trim());
                cc_std_out.write("\n");
            }
        }
        file.close();
    }
}
```

**Benefits:** Safe slices, no strtok, declarative loops, built-in error handling, method chaining, ~40% less code, more readable.

### Advanced Example: Data Processing Pipeline

```c
#include <ccc/std/prelude.cch>

@async void process_logs() {
    Arena arena = arena(megabytes(10));
    
    // Read file
    CCFile !>(CCIoError) f = cc_file_open(&arena, "logs.txt", "r");
    if (!try f) return;

    CCFile file = f.unwrap();
    CCMap::[int, int] status_counts = cc_map_new::[int, int](&arena);

    // Process line by line (UFCS throughout)
    while (true) {
        char[:] !>(CCIoError) line_result = file.read_line(&arena);
        if (cc_is_err(line_result)) break;  // Error
        char[:] line = cc_value(line_result);
        if (line.len == 0) break;  // EOF (empty slice)

        // Skip comments
        if (line.starts_with("#")) continue;

        // Parse fields
        CCStringSplitIter fields = line.split(" ");
        char[:] status_str;
        if (fields.next(&status_str)) {
            i64 !>(CCI64ParseError) status = status_str.parse_i64();
            if (try i64 code = status) {
                // Update count (nullable pointer lookup; 0 if absent)
                int* count = status_counts.get((int)code);
                int new_count = (count ? *count : 0) + 1;
                status_counts.insert((int)code, new_count);
            }
        }
    }

    file.close();

    // Report results
    CCString report = cc_string_new(&arena);
    report.append("Status Code Summary:\n");
    // (iteration over map in Phase 2)

    cc_std_out.write(report.as_slice());
}
```

---

### 5. Networking (`<std/net.cch>`)

Concurrent-C networking provides safe, async-first primitives for TCP/UDP, TLS, HTTP clients, and DNS. All operations integrate with the arena model and return `T!>(E)` results.

#### 5.1 Design Principles

1. **Arena-first buffers:** All read operations allocate into caller-provided arenas. No hidden mallocs.
2. **Duplex unification:** TCP sockets and TLS connections expose the same `Duplex` interface as server connections.
3. **Explicit lifetimes:** Socket handles are resources that must be closed. Use `@defer` (or `@closing(...)` for channel producer scopes).
4. **Async by default:** All I/O is async. Sync wrappers exist but prefer async in concurrent code.

#### 5.2 TCP Sockets

```c
// TCP client connection
@async CCSocket !>(CCNetError) cc_tcp_connect(char[:] addr);   // "host:port" or "ip:port"

// TCP server listener
CCListener !>(CCNetError) cc_tcp_listen(char[:] addr);         // "0.0.0.0:8080", "[::]:8080"

// Accept connection (async)
@async CCSocket !>(CCNetError) CCListener.accept();

// Listener lifecycle
void CCListener.close();

// CCSocket is a CCDuplex — unified read/write interface
// CCSocket implements CCDuplex, so all CCDuplex methods work:
@async char[:] !>(CCIoError) CCSocket.read(CCArena* a, size_t max_bytes);  // empty slice = EOF
@async size_t  !>(CCIoError) CCSocket.write(char[:] data);
@async void    !>(CCIoError) CCSocket.shutdown(CCShutdownMode mode);
void                         CCSocket.close();

// Socket-specific methods
char[:] !>(CCNetError) CCSocket.peer_addr();   // Remote address as string
char[:] !>(CCNetError) CCSocket.local_addr();  // Local address as string
```

**Memory Provenance:**
- `read()` allocates the returned slice in the provided arena
- The slice is valid until the arena is reset/freed
- `write()` does NOT take ownership; data is copied to kernel buffers
- Socket handles contain no arena references; they're just fd wrappers

**Error Type:**
```c
enum CCNetError {
    ConnectionRefused,
    ConnectionReset,
    ConnectionClosed,    // Normal remote close (not an error condition)
    TimedOut,
    HostUnreachable,
    NetworkUnreachable,
    AddressInUse,
    AddressNotAvailable,
    InvalidAddress,      // Parse failure for "host:port"
    DnsFailure,
    TlsHandshakeFailed,
    TlsCertificateError,
    Other(i32 os_code),
};
```

**Examples:**

```c
// TCP client
@async void !>(CCNetError) fetch_data() {
    CCArena arena = arena(megabytes(1));

    CCSocket conn = try await cc_tcp_connect("example.com:80");
    @defer conn.close();

    try await conn.write("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n");

    // Read response into arena (empty slice = EOF)
    while (true) {
        char[:] chunk = try await conn.read(&arena, 4096);
        if (chunk.len == 0) break;  // EOF
        process(chunk);
    }
}

// TCP server (low-level; prefer server_loop for HTTP)
@async void !>(CCNetError) echo_server() {
    CCListener ln = try cc_tcp_listen("0.0.0.0:9000");
    @defer ln.close();

    CCNursery* n = cc_nursery_create(NULL)
        !>(e) { return cc_err(CCNetError::Other(0)); }
        @destroy;
    {
        while (true) {
            CCSocket conn = try await ln.accept();
            n->spawn(() => handle_echo(conn));
        }
    }
}

@async void handle_echo(CCSocket conn) {
    CCArena arena = arena(kilobytes(64));
    @defer conn.close();

    while (true) {
        char[:] data = try await conn.read(&arena, 1024);
        if (data.len == 0) break;  // EOF
        try await conn.write(data);
        arena_reset(&arena);  // Reuse buffer space
    }
}
```

#### 5.3 UDP Sockets

```c
// UDP socket (connectionless)
CCUdpSocket !>(CCNetError) cc_udp_bind(char[:] addr);  // Bind to local address

// Send to specific address
@async size_t !>(CCNetError) CCUdpSocket.send_to(char[:] data, char[:] addr);

// Receive with sender address
struct CCUdpPacket {
    char[:] data;       // Packet data (allocated in arena); empty = no datagram available (non-blocking)
    char[:] from_addr;  // Sender address (allocated in arena)
};
// Blocking recv; returns the next datagram. On error returns CCNetError.
@async CCUdpPacket !>(CCNetError) CCUdpSocket.recv_from(CCArena* a, size_t max_bytes);

void CCUdpSocket.close();
```

**Memory Provenance:**
- `recv_from()` allocates both `data` and `from_addr` in the provided arena
- Both slices share arena lifetime
- `send_to()` copies data to kernel; no ownership transfer

#### 5.4 TLS (`<std/tls.cch>`)

TLS wraps a Socket or Duplex to provide encrypted communication. The wrapped connection exposes the same Duplex interface — handlers don't need to know if they're speaking TLS.

```c
// TLS client configuration
// Optional fields use empty slices to mean "not set" (use system/default):
//   - ca_cert.len == 0       -> use system root CAs
//   - sni_hostname.len == 0  -> derive SNI from connect addr
struct CCTlsClientConfig {
    char[:] ca_cert;            // Optional: custom CA cert path (empty = system roots)
    bool    verify_hostname;    // Default: true
    char[:] sni_hostname;       // Optional: override SNI (empty = use connect addr)
};

// TLS server configuration (same as ServerConfig.tls)
// client_ca.len == 0  -> do not require client certs.
struct CCTlsServerConfig {
    char[:] cert_path;          // Server certificate
    char[:] key_path;           // Private key
    char[:] client_ca;          // Optional: require client certs (empty = disabled)
};

// Wrap existing socket in TLS (client)
@async CCDuplex !>(CCNetError) cc_tls_connect(CCSocket sock, CCTlsClientConfig cfg);

// Convenience: TCP + TLS in one call
@async CCDuplex !>(CCNetError) cc_tls_connect_addr(char[:] addr, CCTlsClientConfig cfg);

// Wrap existing socket in TLS (server-side handshake)
@async CCDuplex !>(CCNetError) cc_tls_accept(CCSocket sock, CCTlsServerConfig cfg);

// TLS connection info (available after handshake)
// Optional string fields use empty slices to mean "not present":
//   - peer_cert_subject.len == 0 -> no client cert presented
//   - sni_hostname.len == 0      -> no SNI extension
struct CCTlsInfo {
    char[:] protocol_version;   // "TLSv1.3", "TLSv1.2"
    char[:] cipher_suite;       // "TLS_AES_256_GCM_SHA384"
    char[:] peer_cert_subject;  // Client cert subject (empty if no client auth)
    char[:] sni_hostname;       // SNI from client hello (empty if none)
};
// Returns NULL if the Duplex is not TLS-wrapped.
CCTlsInfo* CCDuplex.tls_info();
```

**Memory Provenance:**
- `TlsInfo` strings are allocated in an internal arena owned by the TLS session
- They remain valid for the lifetime of the Duplex
- When Duplex is closed, TlsInfo strings become invalid
- For long-term storage, copy with `.clone(your_arena)`

**Implementation Backend:**

CC's TLS implementation wraps **BearSSL** (primary) or **mbedTLS** (fallback):

| Library | License | Allocation Model | Notes |
|---------|---------|------------------|-------|
| **BearSSL** | MIT | Caller-provided buffers | Ideal for arena model; no malloc |
| **mbedTLS** | Apache 2.0 | Custom allocator hook | More features; hook to arena |

**Interop: BearSSL Buffer Model**

BearSSL requires caller-provided I/O buffers. This maps directly to CC arenas:

```c
// Internal: how CC wraps BearSSL (illustrative)
struct TlsSession {
    br_ssl_client_context cc;
    br_x509_minimal_context xc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];  // ~33KB, stack or arena
    Socket underlying;
    Arena* info_arena;  // For TlsInfo strings
};

// BearSSL's read callback — we implement to read from Socket
static int tls_sock_read(void* ctx, unsigned char* buf, size_t len) {
    TlsSession* s = ctx;
    // Blocking read into BearSSL's buffer (not user arena)
    return cc_blocking_recv(s->underlying.fd, buf, len);
}
```

**Key insight:** BearSSL's zero-allocation design means:
- No hidden mallocs during handshake or I/O
- I/O buffers can be stack-allocated or arena-backed
- Perfect fit for CC's "allocations are explicit" philosophy

**Example:**

```c
@async void !>(CCNetError) secure_fetch() {
    CCArena arena = arena(megabytes(1));

    // Connect with TLS (uses system CA roots by default)
    CCDuplex conn = try await cc_tls_connect_addr("api.example.com:443", {
        .verify_hostname = true,
    });
    @defer conn.close();

    // CCDuplex interface is identical to plain CCSocket
    try await conn.write("GET /data HTTP/1.1\r\nHost: api.example.com\r\n\r\n");

    while (true) {
        char[:] chunk = try await conn.read(&arena, 4096);
        if (chunk.len == 0) break;  // EOF
        process(chunk);
    }

    // Optional: inspect TLS session (NULL if not TLS)
    CCTlsInfo* info = conn.tls_info();
    if (info) {
        printf("Protocol: %.*s\n", (int)info->protocol_version.len, info->protocol_version.ptr);
    }
}
```

#### 5.5 HTTP Client (`<std/http.cch>`)

Convenience layer over TCP+TLS for common HTTP operations.

```c
// Simple GET/POST (one-shot, blocks until complete)
@async CCHttpResponse !>(CCHttpError) cc_http_get(CCArena* a, char[:] url);
@async CCHttpResponse !>(CCHttpError) cc_http_post(CCArena* a, char[:] url, char[:] body);

// Response structure
struct CCHttpResponse {
    u16     status;         // 200, 404, etc.
    char[:] status_text;    // "OK", "Not Found"
    char[:] headers;        // Raw headers
    char[:] body;           // Response body
};

// Configurable client. Optional fields use nullable pointer / empty slice:
//   - tls == NULL          -> use default TLS client config
//   - user_agent.len == 0  -> omit User-Agent header
struct CCHttpClient {
    CCDuration        timeout;          // Request timeout (default: 30s)
    CCTlsClientConfig* tls;             // TLS config (NULL = use defaults)
    char[:]           user_agent;       // User-Agent header (empty = omit)
    bool              follow_redirects; // Follow 3xx (default: true, max 10)
};

CCHttpClient cc_http_client_new();

// Builder methods
CCHttpClient* CCHttpClient.timeout(CCDuration d);
CCHttpClient* CCHttpClient.user_agent(char[:] ua);
CCHttpClient* CCHttpClient.no_redirects();

// Requests
@async CCHttpResponse !>(CCHttpError) CCHttpClient.get(CCArena* a, char[:] url);
@async CCHttpResponse !>(CCHttpError) CCHttpClient.post(CCArena* a, char[:] url, char[:] body);
@async CCHttpResponse !>(CCHttpError) CCHttpClient.request(CCArena* a, CCHttpRequest req);

// Full request control
struct CCHttpRequest {
    char[:] method;         // "GET", "POST", "PUT", etc.
    char[:] url;
    char[:] headers;        // Additional headers
    char[:] body;
};
```

**Memory Provenance:**
- All response data (status_text, headers, body) allocated in provided arena
- Response is valid until arena is reset/freed
- Request data (url, headers, body) is read but not owned — caller maintains lifetime

**Error Type:**
```c
enum CCHttpError {
    Net(CCNetError),        // Connection errors
    InvalidUrl,             // URL parse failure
    TooManyRedirects,
    InvalidResponse,        // Malformed HTTP response
    Timeout,
};
```

**Examples:**

```c
// Simple GET
@async void !>(CCHttpError) fetch_json() {
    CCArena arena = arena(megabytes(1));

    CCHttpResponse resp = try await cc_http_get(&arena, "https://api.example.com/users");
    if (resp.status == 200) {
        // resp.body is valid until arena reset
        User[] users = parse_json_users(resp.body, &arena);
        process_users(users);
    }
}

// Configured client
@async void !>(CCHttpError) fetch_with_config() {
    CCArena arena = arena(megabytes(1));

    CCHttpClient client = cc_http_client_new()
        .timeout(seconds(10))
        .user_agent("MyApp/1.0")
        .no_redirects();

    CCHttpResponse resp = try await client.get(&arena, "https://api.example.com/data");
    process(resp);
}

// Custom request
@async void !>(CCHttpError) post_data() {
    CCArena arena = arena(megabytes(1));

    CCHttpRequest req = {
        .method = "POST",
        .url = "https://api.example.com/submit",
        .headers = "Content-Type: application/json\r\nAuthorization: Bearer token\r\n",
        .body = "{\"name\": \"test\"}",
    };

    CCHttpClient client = cc_http_client_new();
    CCHttpResponse resp = try await client.request(&arena, req);
}
```

#### 5.6 DNS (`<std/dns.cch>`)

Async DNS resolution. Integrates with system resolver by default.

```c
// Resolve hostname to addresses
@async CCIpAddr[] !>(CCNetError) cc_dns_lookup(CCArena* a, char[:] hostname);

// IP address (v4 or v6)
struct CCIpAddr {
    enum { V4, V6 } family;
    union {
        u8 v4[4];
        u8 v6[16];
    };
};

// Format IP as string
char[:] CCIpAddr.to_string(CCArena* a);

// Parse string to IP (no DNS, just parsing)
CCIpAddr !>(CCNetError) cc_ip_parse(char[:] s);
```

**Memory Provenance:**
- `dns_lookup()` returns slice of IpAddr allocated in arena
- `to_string()` allocates the formatted string in arena
- Both valid until arena reset

**Example:**

```c
@async void !>(CCNetError) connect_by_name() {
    CCArena arena = arena(kilobytes(4));

    CCIpAddr[] addrs = try await cc_dns_lookup(&arena, "example.com");
    if (addrs.len == 0) {
        return cc_err(CCNetError::DnsFailure);
    }

    // Try each address until one works
    for (size_t i = 0; i < addrs.len; i++) {
        char[:] addr_str = addrs[i].to_string(&arena);
        char[:] full_addr = cc_string_new(&arena)
            .append(addr_str)
            .append(":443")
            .as_slice();

        if (try CCSocket sock = await cc_tcp_connect(full_addr)) {
            return handle_connection(sock);
        }
    }

    return cc_err(CCNetError::HostUnreachable);
}
```

#### 5.7 Interop Lessons

Wrapping C networking libraries teaches key interop patterns:

**Pattern 1: Buffer Ownership**

```c
// BAD: C library allocates, CC must free
char* result = some_c_lib_alloc();  // Who frees this?
// ...
free(result);  // Easy to forget

// GOOD: CC provides buffer, C library fills it
char buffer[1024];
int len = some_c_lib_read(buffer, sizeof(buffer));
// No ownership question — buffer is stack/arena

// GOOD: Hook allocator to arena
void* my_alloc(size_t n) { return arena_alloc(&my_arena, n); }
some_c_lib_set_allocator(my_alloc, NULL);
```

**Pattern 2: Callback Lifetimes**

```c
// C callback signature
typedef int (*read_fn)(void* ctx, unsigned char* buf, size_t len);

// CC closure can't be passed directly (different ABI)
// Solution: pass function pointer + void* context

struct CallbackContext {
    Socket* sock;
    CCDeadline* deadline;  // For cancellation checks
};

static int socket_read_callback(void* ctx, unsigned char* buf, size_t len) {
    CallbackContext* c = ctx;
    // Check cancellation
    if (cc_is_cancelled(c->deadline)) return -1;
    return cc_blocking_recv(c->sock->fd, buf, len);
}
```

**Pattern 3: Arena Checkpoint for Streaming**

```c
// Long-lived connection processing many messages
@async void !>(CCIoError) process_stream(CCDuplex* conn, CCArena* arena) {
    CCArenaCheckpoint cp = arena_checkpoint(arena);
    size_t count = 0;

    while (true) {
        char[:] msg = try await conn.read(arena, 4096);
        if (msg.len == 0) break;  // EOF
        process(msg);
        count++;
        
        // Restore arena periodically to prevent unbounded growth
        if (count % 100 == 0) {
            arena_restore(cp);
            cp = arena_checkpoint(arena);
        }
    }
}
```

**Pattern 4: Foreign Struct Wrapping**

```c
// C library struct with internal pointers
typedef struct {
    void* internal;
    // ...
} SomeLibHandle;

// CC wrapper adds provenance tracking
struct Socket {
    int fd;
    // No arena pointer — Socket doesn't own buffers
    // Buffers are provided per-operation
};

struct TlsSession {
    br_ssl_client_context ctx;   // BearSSL context (value, not pointer)
    Socket underlying;           // Owned socket
    Arena* info_arena;           // Owns TlsInfo strings
    // iobuf is stack-allocated in async frame or part of struct
};
```

---

### 6. Portable Atomics (`<cc_atomic.cch>`)

#### 6.1 Overview

`cc_atomic.cch` provides portable atomic operations across different compilers. This is a **runtime header** (not under `std/`) for users building concurrent data structures or needing explicit atomic memory operations.

**Portability Matrix:**

| Compiler | Backend | Thread-Safety |
|----------|---------|---------------|
| GCC/Clang (C11) | `<stdatomic.h>` | ✅ Full |
| Older GCC/Clang | `__sync_*` builtins | ✅ Full |
| TCC | Non-atomic fallback | ⚠️ Best-effort only |
| Other | Non-atomic fallback | ⚠️ Best-effort only |

**Design Note:** TCC does not support real atomics. When compiled with TCC as the final compiler, atomic operations fall back to non-atomic (volatile) operations. This is safe for single-threaded code but **not thread-safe** for concurrent access. For production concurrent code, use GCC or Clang.

#### 6.2 Types

```c
#include <ccc/cc_atomic.cch>

// Atomic integer types
cc_atomic_int       // atomic int
cc_atomic_uint      // atomic unsigned int
cc_atomic_size      // atomic size_t
cc_atomic_i64       // atomic int64_t
cc_atomic_u64       // atomic uint64_t
cc_atomic_intptr    // atomic intptr_t
```

All types are guaranteed to be at least as aligned as their non-atomic counterparts.

#### 6.3 Operations

```c
// Atomic fetch-and-add: returns previous value, adds val
T cc_atomic_fetch_add(T* ptr, T val);

// Atomic fetch-and-subtract: returns previous value, subtracts val
T cc_atomic_fetch_sub(T* ptr, T val);

// Atomic load: returns current value
T cc_atomic_load(T* ptr);

// Atomic store: sets value
void cc_atomic_store(T* ptr, T val);

// Atomic compare-and-swap: if *ptr == *expected, set *ptr = desired, return true
//                          else set *expected = *ptr, return false
bool cc_atomic_cas(T* ptr, T* expected, T desired);
```

All operations use **sequential consistency** (`memory_order_seq_cst`) for simplicity and safety. Relaxed orderings are not exposed; if you need them, use the underlying compiler intrinsics directly.

#### 6.4 Detection Macro

```c
// Defined to 1 if real atomics are available, 0 if using fallback
#if CC_ATOMIC_HAVE_REAL_ATOMICS
    // Using C11 atomics or __sync builtins — thread-safe
#else
    // Using non-atomic fallback — NOT thread-safe
    #warning "Atomics unavailable; concurrent code may have data races"
#endif
```

#### 6.5 Examples

**Counter:**

```c
#include <ccc/cc_atomic.cch>
#include <stdio.h>

cc_atomic_int g_counter = 0;

void increment(void) {
    cc_atomic_fetch_add(&g_counter, 1);
}

int get_count(void) {
    return cc_atomic_load(&g_counter);
}

void reset_count(void) {
    cc_atomic_store(&g_counter, 0);
}
```

**Lock-free stack (simple example):**

```c
#include <ccc/cc_atomic.cch>

struct Node {
    int value;
    struct Node* next;
};

cc_atomic_intptr g_stack_head = 0;  // NULL

void push(struct Node* node) {
    intptr_t old_head;
    do {
        old_head = cc_atomic_load(&g_stack_head);
        node->next = (struct Node*)old_head;
    } while (!cc_atomic_cas(&g_stack_head, &old_head, (intptr_t)node));
}

struct Node* pop(void) {
    intptr_t old_head;
    struct Node* node;
    do {
        old_head = cc_atomic_load(&g_stack_head);
        node = (struct Node*)old_head;
        if (!node) return NULL;
    } while (!cc_atomic_cas(&g_stack_head, &old_head, (intptr_t)node->next));
    return node;
}
```

**Concurrent accumulator in spawned tasks:**

```c
#include <ccc/cc_runtime.cch>
#include <ccc/cc_atomic.cch>

cc_atomic_int g_sum = 0;

int main(void) {
    CCNursery* n = cc_nursery_create(NULL) !>(e) { return 1; } @destroy;
    {
        for (int i = 0; i < 100; i++) {
            int val = i;
            n->spawn(() => {
                cc_atomic_fetch_add(&g_sum, val);
            });
        }
    }
    
    // Sum of 0..99 = 4950
    printf("sum = %d (expected 4950)\n", cc_atomic_load(&g_sum));
    return 0;
}
```

#### 6.6 When to Use

**Use `cc_atomic.cch` when:**
- Building custom concurrent data structures (queues, stacks, counters)
- Implementing lock-free algorithms
- Coordinating between spawned tasks without channels
- Porting existing C code that uses atomics

**Prefer channels and nurseries when:**
- Coordinating producer/consumer patterns → use channel `send`/`recv` UFCS
- Aggregating results from tasks → use channels or return values
- Synchronizing task completion → use `CCNursery* n = ... !> @destroy`

**Avoid atomics when:**
- A simple mutex would be clearer (atomics are hard to get right)
- Channel-based coordination is sufficient
- Compiling with TCC as final compiler (atomics degrade to non-atomic)

#### 6.7 Relationship to Runtime

The `cc_atomic.cch` header is used internally by:
- `cc_arena.cch` — for thread-safe bump allocation
- Channel implementation — for lock-free queue operations
- Nursery implementation — for task counting

Users can include it directly for their own needs. It has no dependencies beyond `<stdint.h>` and `<stddef.h>`.

---

## Future Phases

**Phase 2 (v1.1):**
- `Set<T>` (hash set, arena-backed).
- Advanced string: UTF-8 aware (grapheme clusters, normalization).
- Printf-style formatting if proven essential.
- Concurrency helpers: Fan-in/fan-out patterns, broadcast helpers.
- Map/Vec iteration methods (UFCS).
- WebSocket client (`ws_connect()`).
- HTTP/2 client support.

**Phase 3 (v1.2):**
- Advanced I/O: Async file ops, streams, buffering.
- Math: Safe div, sqrt with error handling.
- Time: Duration, Timestamp.
- Process/env: Environment variables, subprocess.
- Compression/hashing: zlib, xxhash wrappers (maybe).
- QUIC/HTTP3 (stretch goal).

---

## Versioning and Stability

Stdlib version independent of language version. Phase 1 = v1.0; Phase 2 = v1.1; etc.

**Stability Promise:** APIs in Phase 1 are stable; no breaking changes within v1.x minor versions.

---

## References and Inspiration

- Rust `std`: Minimal scope, safety-first, owned types, UFCS methods.
- Zig `std`: Explicit allocators, composability, low-level control.
- C `libc`: Keep what works (stdio, basic ops), add safety (`T !>(E)`, slices, UFCS).

---

## Appendix: API Quick Reference

### String Methods on char[:]

| Method | Purpose |
|--------|---------|
| `.len()` | Length |
| `.get(i)` | Safe index (returns `char*`; NULL if out of range) |
| `.slice(start, end)` | Subslice view (empty slice if range invalid) |
| `.c_str(a)` | NUL-terminated copy for C interop |
| `.clone(a)` | Copy to new allocation |
| `.trim()`, `.trim_left()`, `.trim_right()` | Trim whitespace |
| `.is_empty()`, `.is_ascii()` | Checks |
| `.starts_with()`, `.ends_with()`, `.contains()` | Prefix/suffix/contains |
| `.index_of()`, `.last_index_of()`, `.count()` | Find (returns `ssize_t`; `-1` if not found) |
| `.upper(a)`, `.lower(a)` | Case conversion |
| `.replace(a, old, new)` | Replace all |
| `.split(delim)` | Iterator split |
| `.split_all(a, delim)` | Collect all |
| `.parse_i64()`, `.parse_f64()`, `.parse_bool()` | Parse |

### String Builder Methods

| Method | Purpose |
|--------|---------|
| `string_new(a)` | Create |
| `.append()`, `.push_char()`, `.push_int()`, `.push_float()` | Append |
| `.append_if()` | Conditional append |
| `.as_slice()` | Finalize to view |
| `.len()`, `.cap()` | Info |
| `.clear()` | Clear |

### File Methods

| Method | Purpose |
|--------|---------|
| `file_open(a, path, mode)` | Open |
| `.read()`, `.read_all()`, `.read_line()` | Read (arena-allocated) |
| `.read_buf(buf, n)` | Read into caller buffer (no allocation) |
| `.write()` | Write (from slice) |
| `.write_buf(buf, n)` | Write from caller buffer (no slice) |
| `.seek()`, `.tell()`, `.size()` | Position/size |
| `.sync()` | Flush |
| `.close()` | Close |

### `CCVec::[T]` Methods

| Method | Purpose |
|--------|---------|
| `cc_vec_new::[T](a)` | Create |
| `.push(v)` | Append |
| `.pop(T* out)` | Remove last; returns `bool` (false if empty) |
| `.get(i)` | Access; returns `T*` (NULL if out of range) |
| `.set(i, v)` | Set; returns `void !>(CCBoundsError)` |
| `.len()`, `.cap()` | Info |
| `.iter()` | Iterator (see `CCVecIter::[T].next(T* out)`) |
| `.as_slice()` | View |
| `.clear()` | Clear |

### `CCMap::[K, V]` Methods

| Method | Purpose |
|--------|---------|
| `cc_map_new::[K, V](a)` | Create |
| `.insert(k, v)` | Insert/update |
| `.get(k)` | Lookup; returns `V*` (NULL if absent) |
| `.remove(k)` | Remove; returns `bool` (true if removed) |
| `.len()`, `.cap()` | Info |
| `.clear()` | Clear |

---

**End of Specification**
