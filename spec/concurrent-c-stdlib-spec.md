# Concurrent-C Standard Library Specification

**Version:** 1.0 (Phase 1: Strings & I/O, UFCS-First)  
**Date:** 2026-01-07  
**Status:** UFCS-optimized specification for header-only implementation

---

## Goals and Scope

The Concurrent-C Standard Library (`<std/...>`) provides minimal, composable wrappers around common systems operations, designed to integrate seamlessly with Concurrent-C's core features (arenas, slices, async, error values) while maintaining portability and zero-overhead abstraction.

**Philosophy:** Stay small (only battle-tested essentials), be explicit (allocations visible, arenas passed), leverage CC features (T!E for errors, slices for views, channels for streaming), remain optional (header-only, no mandatory linkage), and **use UFCS for natural, discoverable APIs**.

**UFCS Pattern:** All types use method syntax via UFCS. Free functions exist for genericity and composition but methods are primary for users.

```c
// UFCS-first (user API)
int len = s.len();
s.trim().split(",").map(process);

// Free function (normative, for genericity)
int len(&s);
split(&s, ",");
```

**Phase 1 Focus:** String manipulation and file I/O—the most commonly requested operations that reduce verbosity in real code.

**Future Phases:** Collections (v1.1), concurrency patterns (v1.2), others as community feedback drives.

---

## Design Principles

1. **UFCS-First API:** Methods on types are primary; free functions are normative forms. Users write `s.len()` not `string_length(s)`.
   - All UFCS methods desugar to free functions; calling either form is always equivalent and supported.

2. **Header-Only:** All Phase 1 functions defined in headers; no compilation required.
3. **Explicit Allocation:** All allocations via `Arena*` passed as explicit parameters. No hidden allocators.
4. **Error Values:** All fallible operations return `T!E` (Result types); no errno, no exceptions.
5. **Slices Everywhere:** Parameters and returns use `char[:]` views where appropriate; avoid unnecessary copies.
6. **Portability:** Abstract OS differences (e.g., path separators) transparently.
7. **No Dependencies:** Stdlib depends only on C standard library and CC language primitives.
8. **Integration with Async:** I/O functions have sync and `@async` variants where applicable.
9. **Single runtime TU:** Runtime impls are aggregated in `cc/runtime/concurrent_c.c` (`#include` of arena, io, scheduler, channels) so consumers can link one object/archive without juggling multiple runtime objects.
10. **Prefixed C ABI:** Public C names are prefixed (`CCString`, `CCArena`, `cc_file_*`) to avoid collisions. `std/prelude.cch` can optionally expose short aliases when `CC_ENABLE_SHORT_NAMES` is defined; default is prefixed-only. Header implementations are `static inline` to keep stdlib header-only.
11. **Arena-first collections:** Collections default to arena-backed, bounded growth. Vectors/maps grow by allocating new buffers/tables in the provided arena and reusing them; old buffers remain until the arena resets. Growth fails if the arena is exhausted. Heap-backed helpers (kvec/khash style) are optional, tool-only, and never used by generated code unless explicitly included.
12. **Async backend auto-probe:** The runtime may auto-select a native async backend (io_uring/kqueue/poll) with a safe fallback to the portable executor. An environment override (e.g., `CC_RUNTIME_BACKEND=executor|poll|io_uring`) can force selection; otherwise a best-available backend is chosen lazily.

### Type Notation Precedence

Type modifiers bind with this precedence (tightest first):

1. `?` (optional)
2. `!E` (result)
3. `[:]` (slice)

See **§2.3 Type Precedence** in the main language spec for complete rules.

**Examples:**

| Syntax | Parses as | Meaning |
|--------|-----------|---------|
| `char[:]?` | `(char[:])?` | optional slice (e.g., `find()` return) |
| `char[:]!IoError` | `(char[:])!IoError` | slice or error (e.g., `read_all()` return) |
| `char[:]?!IoError` | `((char[:])?)!IoError` | result whose ok-value is optional (e.g., streaming `read()`) |
| `T!E?` | `((T)!E)?` | optional result (e.g., `recv()` on error channel) |

**Key distinction:**
- `T?!E` — "operation may fail; if it succeeds, value may be absent" (streaming read: error, EOF, or data)
- `T!E?` — "value may be absent; if present, it's a result" (channel recv: closed, or ok/err)

---

## Phase 1: Strings, I/O, and Collections

The Phase 1 stdlib includes three core modules: strings (manipulation and parsing), I/O (file and stream operations), and collections (dynamic arrays and hash maps). All are header-only or minimal-linkage, arena-backed, and designed for concurrent systems programming.

### 1. Strings (`<std/string.cch>`)

**Naming:** Language surface keeps `String` for UFCS ergonomics, but the C ABI is prefixed (`CCString`, `cc_string_*`). Short aliases are available only if `CC_ENABLE_SHORT_NAMES` is defined before including `std/prelude.cch`.

**Collections note:** Vectors/Maps are arena-backed by default. They may grow by allocating new buffers/tables in the arena and copying/rehashing; growth fails if the arena cannot satisfy the request. Optional heap-backed variants exist for tools/tests when explicitly included; generated code uses the arena-backed forms.

#### 1.1 Overview

Concurrent-C slices (`char[:]`) are efficient views for immutable string data. The stdlib builds on slices with:

- **String type:** Arena-backed growable string (used for building, formatting, accumulation).
- **Slice operations:** Non-owning UFCS methods on `char[:]` (split, trim, find, parse) that work with any slice.

All operations are accessible via UFCS method syntax for ergonomics:

```c
// Natural, chainable
str result = input
    .trim()
    .lower()
    .replace("foo", "bar")
    .slice(0, 10);

// Or fluent builder
StringBuilder sb = StringBuilder::new();
sb.append("Hello")
  .append(" ")
  .append("World")
  .build();
```

---

#### 1.2 String Builder Type

```c
// Language surface: String
// C ABI (prefixed): typedef struct CCString CCString;
```

**Handle semantics:** `String` is a small, moveable handle to an arena-backed buffer. Copying a `String` aliases the same storage. To obtain an independent copy, use `as_slice().clone(a)`. String contents live until the arena is reset/freed.

**Factory (free function)**

```c
String string_new(Arena* a);
String string_from(Arena* a, char[:] initial);

// UFCS Methods only
String* String.append(char[:] data);          // Append data, return for chaining
String* String.append_char(char c);           // Append single character
String* String.append_int(i64 value);         // Append formatted i64
String* String.append_float(f64 value);       // Append formatted f64
String* String.append_uint(u64 value);        // Append formatted u64
String* String.append_if(bool cond, char[:] data);  // Conditional append
String* String.clear();                        // Clear contents, reuse allocation
char[:] String.as_slice();                     // Get immutable view
size_t String.len();                           // Length in bytes
size_t String.cap();                           // Capacity
```

**Slice Lifetime:** The slice returned by `as_slice()` remains valid until the next mutating call on the same `String` (e.g., `append()`, `clear()`). For stable references, use `.clone()` to create an independent copy in the arena.

**Builder Pattern (Fluent, UFCS-enabled):**

```c
Arena arena = arena(megabytes(1));

// Method chaining via UFCS
String sb = string_new(&arena);
sb.append("count=")
  .append("x")
  .append_int(42);
char[:] result = sb.as_slice();  // "count=x42"

// Or more readable step-by-step
String greeting = string_new(&arena);
greeting.append("Hello");
greeting.append_char(' ');
greeting.append("world");
printf("%.*s\n", (int)greeting.as_slice().len, greeting.as_slice().ptr);
```

**Practical Examples:**

```c
Arena arena = arena(megabytes(1));

// Build JSON
String json = string_new(&arena);
json.append("{\"name\":\"")
    .append(name)
    .append("\",\"age\":")
    .append_int(age)
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
         .append_int(min_age);
```

---

#### 1.3 String Slice Operations (char[:])

UFCS methods on immutable `char[:]` views. These are allocation-free and work with any slice.

##### Core Methods

```c
size_t  char[:].len();                          // Length in bytes

// Safe indexing (never traps)
char?   char[:].get(size_t index);              // None if out of bounds

// View (no allocation, None if invalid range)
char[:]? char[:].slice(size_t start, size_t end);  // None if start > end or end > len

// Copy to new allocation
char[:] char[:].clone(Arena* a);                // Byte-for-byte copy into arena (UTF-8 unchanged)
char*   char[:].c_str(Arena* a);                // Copy len bytes + NUL terminator; returns char* for C interop
```

**Slice Safety:** `.slice(start, end)` returns `None` if `start > end` or `end > len`. Otherwise, it returns a view (no allocation). This prevents invalid ranges while remaining type-safe.

**Example:**
```c
char[:] s = "hello";
assert(s.slice(1, 4).unwrap() == "ell");     // valid range
assert(!s.slice(4, 1));                      // invalid: start > end
assert(!s.slice(0, 99));                     // invalid: end > len
assert(s.slice(5, 5).unwrap().len() == 0);   // empty slice at end
```

##### Query Methods

```c
bool    char[:].is_empty();                    // Check if empty
bool    char[:].is_ascii();                    // Check if all ASCII
bool    char[:].starts_with(char[:] prefix);   // Check prefix
bool    char[:].ends_with(char[:] suffix);     // Check suffix
bool    char[:].contains(char[:] needle);      // Check contains
size_t? char[:].index_of(char[:] needle);      // Find index (null if not found)
size_t? char[:].last_index_of(char[:] needle); // Find last index
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
i64!     char[:].parse_i64();                  // Parse to i64
u64!     char[:].parse_u64();                  // Parse to u64
f64!     char[:].parse_f64();                  // Parse to f64
bool!    char[:].parse_bool();                 // Parse to bool ("true"/"false")
```

**Error types:**
```c
enum I64ParseError { InvalidChar, Overflow, Underflow };
enum FloatParseError { InvalidChar, Overflow };
enum BoolParseError { InvalidValue };
```

##### Split Methods

```c
struct StringSplitIter {
    char[:] remaining;
    char[:] delim;
};

StringSplitIter char[:].split(char[:] delim);  // Create iterator
char[:]?        StringSplitIter.next();        // Advance iterator
char[:][:] char[:].split_all(Arena* a, char[:] delim);  // Collect all at once
```

**Examples:**

```c
// Slice query and trim
char[:] input = "  hello world  ";
char[:] trimmed = input.trim();        // "hello world"
bool has_hello = trimmed.contains("hello");  // true

// Parse with error handling
char[:] num_str = "42";
i64! result = num_str.parse_i64();
if (try i64 val = result) {
    printf("Parsed: %ld\n", val);
} else {
    printf("Parse error\n");
}

// Split and iterate
char[:] csv = "alice,bob,charlie";
StringSplitIter it = csv.split(",");
while (char[:]? name = it.next()) {
    printf("Name: %.*s\n", (int)name.len, name.ptr);
}

// Split all at once
Arena arena = arena(megabytes(1));
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

- Replace errno with `T!IoError` Result types.
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
enum IoError {
    PermissionDenied,
    FileNotFound,
    InvalidArgument,
    Interrupted,
    OutOfMemory,
    Busy,           // Blocking pool saturated, queue is full
    ConnectionClosed,  // Normal closure (like EOF for streams; not an error condition)

    // Platform-specific error code preserved for diagnostics.
    // On POSIX: errno. On Windows: GetLastError()/WSAGetLastError() (implementation-defined).
    Other(i32 os_code),
};

enum ParseError {
    InvalidUtf8,
    Truncated,
};

// Note: EOF is not an error. Streaming APIs return Ok(None) at end-of-stream.
// EOF represents stream exhaustion, not failure; it is modeled as an Option to allow
// uniform while (try ...) streaming patterns without treating normal termination as exceptional.
```

---

#### 2.3 File Type and Methods

```c
// Opaque file handle
typedef struct File File;

// Factory (free function)
File! file_open(Arena* a, char[:] path, char[:] mode);  // "r", "w", "a"
@async File! file_open_async(Arena* a, char[:] path, char[:] mode);

// UFCS Methods

// Streaming read: Ok(None) means EOF (normal termination), Err means actual failure.
// Reads up to n bytes; returns slice of actual bytes read.
char[:]?!IoError File.read(Arena* a, size_t n);

// Read one line into arena (line ending handling: accepts \n and \r\n).
// Ok(None) means EOF with no more data.
char[:]?!IoError File.read_line(Arena* a);

// Read entire file into arena. Returns empty slice for empty files.
// This is NOT a streaming API — use read() or read_line() for streaming.
char[:]!IoError File.read_all(Arena* a);

// Write all bytes from data.
size_t !IoError File.write(char[:] data);

i64    !IoError File.seek(i64 offset, int whence);      // SEEK_SET/CUR/END
i64    !IoError File.tell();                            // Current position

// Flush to disk; observes flush errors.
void   !IoError File.sync();

// Close is best-effort and infallible (no error returned).
// Call sync() before close() to observe flush failures.
void            File.close();

// Async variants (same signatures, just async)
@async char[:]?!IoError File.read_async(Arena* a, size_t n);
@async char[:]?!IoError File.read_line_async(Arena* a);
@async char[:]!IoError  File.read_all_async(Arena* a);
@async size_t!IoError   File.write_async(char[:] data);
```

**EOF Semantics (Unified):**

| Method | Return Type | EOF Behavior |
|--------|-------------|--------------|
| `read()` | `char[:]?!IoError` | `Ok(None)` = EOF |
| `read_line()` | `char[:]?!IoError` | `Ok(None)` = EOF |
| `read_all()` | `char[:]!IoError` | N/A (reads entire file; empty slice for empty file) |

**Rule:** All streaming reads (`read()`, `read_line()`) return `Ok(None)` at EOF. Non-streaming reads (`read_all()`) return the full content (empty slice if file is empty).

**Examples:**

```c
Arena arena = arena(megabytes(10));

// Read entire file (error handling)
File! f = file_open(&arena, "data.txt", "r");
if (try File file = f) {
    char[:] content = try file.read_all(&arena);
    printf("Read %zu bytes\n", content.len);
    file.close();
} catch (IoError err) {
    printf("Error: %d\n", err);
}

// Read lines (EOF is Ok(None))
File! f = file_open(&arena, "input.txt", "r");
if (try File file = f) {
    while (char[:]?!IoError line_result = file.read_line(&arena)) {
        if (try char[:]? line_opt = line_result) {
            if (!line_opt) break;  // EOF (Ok(None))
            char[:] line = *line_opt;
            printf("Line: %.*s\n", (int)line.len, line.ptr);
        }
        // If we get here with error already thrown, outer catch handles it
    }
    file.close();
}

// Write file
File! out = file_open(&arena, "output.txt", "w");
if (try File file = out) {
    try file.write("Hello, world!\n");
    file.close();
}

// Async I/O
@async void process_file() {
    // Sync open is allowed; runtime may offload it to blocking pool if needed
    File! f = file_open(&arena, "data.txt", "r");
    if (try File file = f) {
        char[:] data = try await file.read_all_async(&arena);
        process(data);
        file.close();
    }
}
```

#### 2.4 Standard Streams

```c
// UFCS methods on stdout/stderr (singletons)
void! std_out.write(char[:] data);
void! std_err.write(char[:] data);
// String overloads for ergonomics
void! std_out.write(String s);
void! std_err.write(String s);
// Overload resolution is handled by the compiler's UFCS lowering; both map to prefixed C ABI (`cc_std_out_write`, `cc_std_out_write_string`, etc.).

**UFCS receiver conversion (general rule):**
- Overload selection prefers an exact receiver type match.
- If no exact match, the compiler may apply these implicit receiver conversions (in order) to find a viable overload:
  1) `String -> char[:]` (view of contents)
  2) `char[N]` / string literal -> `char[:]`
  3) `CCSlice` alias -> `char[:]`
- If no overload is viable after these conversions, resolution fails.
```

**Examples:**

```c
std_out.write("Hello, world!\n");
std_err.write("An error occurred\n");
std_out.write(my_string); // String overload
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
void log_drop(LogEvent evt);

// Log with block strategy: blocks up to timeout, fails request if timeout exceeded
// Use for audit/critical logs that cannot be dropped
void! log_block(LogEvent evt, Duration timeout);

// Log with sample strategy: deterministically keep ~rate fraction
// Use for high-volume logs (traces, metrics) that must be sparse
void log_sample(LogEvent evt, f32 rate);

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
#include <std/prelude.cch>

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
char[:] path_join(Arena* a, char[:] parent, char[:] child);  // Cross-platform path joining
char[:] path_basename(char[:] path);     // Extract filename
char[:] path_dirname(char[:] path);      // Extract directory
char[:] path_extension(char[:] path);    // Extract extension

// Example: cross-platform
char[:] config = path_join(&arena, home_dir, ".config/app.txt");
char[:] dir = path_dirname(config);
char[:] name = path_basename(config);
char[:] ext = path_extension(config);
```

---

### 3. Collections (`<std/vec.cch>` and `<std/map.cch>`)

#### 3.1 Overview

CC's collections are **arena-backed**, **generic** (via `<T>` syntax), and **UFCS-enabled**. They enable safe, efficient data structures for concurrent systems: work queues, request buffers, caches, state tables.

#### 3.2 Vec<T> (Dynamic Array)

```c
// Generic dynamic array type (arena-backed)
typedef struct Vec<T> Vec<T>;

// Factories (free functions)
Vec<T> vec_new<T>(Arena* a);
Vec<T> vec_with_capacity<T>(Arena* a, size_t capacity);

// UFCS Methods only
void    Vec<T>.push(T value);                // Add element (grows as needed)
T?      Vec<T>.pop();                        // Remove and return last
T?      Vec<T>.get(size_t index);            // Bounds-safe get (None if out of bounds)
T*      Vec<T>.get_mut(size_t index);        // Mutable access (NULL if out of bounds)

enum BoundsError { OutOfBounds };
void!BoundsError Vec<T>.set(size_t index, T value);   // Set with bounds check (error if out of bounds)

void    Vec<T>.clear();                      // Clear contents
size_t  Vec<T>.len();                        // Length
size_t  Vec<T>.cap();                        // Capacity
T[:]    Vec<T>.as_slice();                   // View as T[:]

// Iterator
struct VecIter<T> {
    Vec<T>* vec;
    size_t index;
};

VecIter<T> Vec<T>.iter();
T?         VecIter<T>.next();
```

**Examples:**

```c
Arena arena = arena(megabytes(1));

// Work queue (UFCS method syntax)
Vec<Task<void>> tasks = vec_new<Task<void>>(&arena);
tasks.push(async_work1());
tasks.push(async_work2());
tasks.push(async_work3());

// Iterate and await
VecIter<Task<void>> it = tasks.iter();
while (Task<void>? task = it.next()) {
    await *task;
}

// Buffer for accumulation
Vec<char> buffer = vec_new<char>(&arena);
for (size_t i = 0; i < input.len; i++) {
    buffer.push(input.ptr[i]);
}
char[:] result = buffer.as_slice();

// Bounds-safe access
Vec<int> data = vec_new<int>(&arena);
data.push(42);
int? val = data.get(0);      // Some(42)
int? oob = data.get(100);    // None (out of bounds)

// Size checks
if (data.len() > 0) {
    data.clear();
}
```

---

#### 3.3 Map<K, V> (Hash Map)

`Map<K, V>` is already defined in the Concurrent-C language spec. The stdlib provides it with UFCS methods:

```c
// Generic hash map (arena-backed)
typedef struct Map<K, V> Map<K, V>;

// Factory (free function)
Map<K, V> map_new<K, V>(Arena* a);

// UFCS Methods only
void    Map<K, V>.insert(K key, V value);    // Insert or update
V?      Map<K, V>.get(K key);                // Lookup (returns optional)
V*      Map<K, V>.get_mut(K key);            // Mutable reference
bool    Map<K, V>.remove(K key);             // Remove (true if existed)
void    Map<K, V>.clear();                   // Clear all entries
size_t  Map<K, V>.len();                     // Number of entries
size_t  Map<K, V>.cap();                     // Capacity
```

**Note on Iteration:** Map iteration is intentionally deferred to Phase 2 to avoid prematurely locking in traversal order semantics. Phase 1 focuses on insertion, lookup, and removal.

**Examples:**

```c
Arena arena = arena(megabytes(1));

// Simple cache (string → result)
Map<char[:], int> cache = map_new<char[:], int>(&arena);
cache.insert("key1", 100);
cache.insert("key2", 200);

int? val = cache.get("key1");     // Some(100)
int? miss = cache.get("key3");    // None

// State table for concurrent requests
struct Request { int id; char[:] path; };
Map<int, Request> active = map_new<int, Request>(&arena);

active.insert(req.id, req);
Request? found = active.get(42);
if (found) {
    process_request(*found);
}
active.remove(42);

// Conditional lookup and update
if (Request? r = active.get(id)) {
    r.path = new_path;
}
```

---

#### 3.4 Set<T> (Deferred)

`Set<T>` is deferred to Phase 2. Can be implemented as `Map<T, bool>` or a dedicated type; community feedback will guide the decision.

---

## Module Structure

```
<std/prelude.cch>        // Common imports (String, File, Vec, Map, etc.)
<std/string.cch>         // String builder and char[:] methods
<std/io.cch>             // File, StdStream
<std/log.cch>            // Structured logging (LogEvent, log_drop/block/sample)
<std/fs.cch>             // Path utilities
<std/vec.cch>            // Vec<T> dynamic array with UFCS methods
<std/map.cch>            // Map<K, V> hash map with UFCS methods
<std/net.cch>            // TCP/UDP sockets, Listener
<std/tls.cch>            // TLS client/server (wraps BearSSL/mbedTLS)
<std/http.cch>           // HTTP client (http_get, http_post, HttpClient)
<std/dns.cch>            // DNS resolution (dns_lookup)
<std/server.cch>         // server_loop canonical shell
<std/error.cch>          // Error enums (IoError, NetError, HttpError, etc.)
```

**Prelude Safety:** `<std/prelude.cch>` performs no implicit allocation or runtime initialization. It is a pure header include with zero hidden costs.

**Updated Prelude Example:**

```c
#include <std/prelude.cch>

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
    while (Task<void>? t = it.next()) {
        await *t;
    }
    
    // File I/O (UFCS)
    File! f = file_open(&arena, "data.txt", "r");
    if (try File file = f) {
        char[:] data = try file.read_all(&arena);
        
        // String processing (UFCS)
        char[:] trimmed = data.trim();
        StringSplitIter lines = trimmed.split("\n");
        while (char[:]? line = lines.next()) {
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
    File! f = file_open(&arena, path, "r");
    if (try File file = f) {
        int retry_count = 0;
        while (char[:]?!IoError line_result = file.read_line(&arena)) {
            if (line_result is Busy) {
                if (retry_count++ > 3) {
                    return cc_err(IoError::Busy);
                }
                await sleep(milliseconds(10 * retry_count));
                continue;
            }
            retry_count = 0;
            
            if (try char[:] line = line_result) {
                process(line);
            } else {
                break; // EOF
            }
        }
        file.close();
    }
}
```

**No eviction:** Pending operations are not killed on saturation. Bounded queue + fail-fast is safer than eviction (which risks corrupting `@scoped` resources, transactions, or in-flight cleanup).

### UFCS Implementation

The pattern is consistent with the language:

> **The free function form is normative. `s.len()` is UFCS sugar for `len(&s)`.**

For each method on a type `T`:

```c
// Normative free function
size_t len(T* self);

// UFCS method (syntactic sugar)
size_t T.len();  // desugars to len(&self)
```

This allows both function composition and ergonomic method chaining.

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
    char[:]? p1 = it.next();
    assert(p1 && p1.len == 5);  // "hello"
    
    // Test contains
    assert(trimmed.contains("world"));
    assert(!trimmed.contains("xyz"));
    
    // Test parse
    i64! val = "42".parse_i64();
    assert(try i64 v = val && v == 42);
}

@test "vec and map methods" {
    Arena arena = arena(kilobytes(10));
    
    // Vec<T> methods
    Vec<int> v = vec_new<int>(&arena);
    v.push(10);
    v.push(20);
    assert(v.len() == 2);
    int? val = v.get(0);
    assert(try int v = val && v == 10);
    
    // Map<K,V> methods
    Map<char[:], int> m = map_new<char[:], int>(&arena);
    m.insert("x", 100);
    int? result = m.get("x");
    assert(try int r = result && r == 100);
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
// Duplex: unified interface for bidirectional, closeable streams
// Used by TLS wrappers, WebSocket handlers, HTTP/2, raw protocols, etc.
struct Duplex {
    @async char[:]?!IoError read(Arena* a);      // Read chunk; Ok(None) == EOF
    @async void!IoError write(char[:] bytes);    // Write chunk
    @async void!IoError shutdown(ShutdownMode mode);  // Half-close: Read, Write, or Both
    @async void!IoError close();                 // Close (equivalent to shutdown(Both))
};

enum ShutdownMode {
    Read,   // Stop reading; keep write side open (recv FIN, continue sending)
    Write,  // Stop writing; keep read side open (send FIN, continue receiving)
    Both,   // Close both sides
};

**ABI Contract (Normative):**

All interface values (including Duplex) lower to a two-pointer layout:

```c
typedef struct {
    void* self;                    // Opaque receiver state
    const DuplexVTable* vt;        // Method table
} Duplex;

typedef struct {
    Task_CharSliceOptIoErr (*read)(void* self, Arena* a);
    Task_VoidIoErr (*write)(void* self, CharSlice bytes);
    Task_VoidIoErr (*shutdown)(void* self, ShutdownMode mode);
    Task_VoidIoErr (*close)(void* self);
} DuplexVTable;
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
    char[:] body;                // Response body (small/unary fast path)
    Duplex? stream;              // Streaming response (use write() + shutdown(Write) to end)
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
typedef @async void!IoError (*ConnHandlerFn)(Duplex* conn, Arena* conn_arena);

// Unary request/response handler
typedef @async ServerAction!IoError (*ServerHandlerFn)(Request* req, Arena* req_arena);

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
    TlsConfig? tls;                        // Optional TLS wrapping
    
    // Optional lifecycle callbacks
    bool (*on_request_start)(Request* req);
    void (*on_request_end)(Request* req, Response* resp, Duration elapsed);
};
```

**Main Entry Point:**

```c
// Accept connections, perform TLS handshake if configured, spawn workers, manage lifetimes
@async void!IoError server_loop(ServerConfig cfg);
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
@async void!IoError server_loop(ServerConfig cfg) {
    int listener = try listen(cfg.port);
    
    @nursery {
        for (size_t i = 0; i < cfg.max_workers; i++) {
            spawn (server_worker(&cfg, listener));
        }
    }
}

@async void!IoError server_worker(ServerConfig* cfg, int listener) {
    while (true) {
        // Accept connection (raw socket)
        int raw_fd = try await accept(listener);
        
        // Perform TLS handshake if configured
        Duplex conn = if (cfg.tls) {
            try tls_handshake(raw_fd, cfg.tls)
        } else {
            Duplex.from_fd(raw_fd)
        };
        
        // Connection-scoped arena (lives until connection closes)
        Arena conn_arena = arena(megabytes(1));
        
        // Keep-alive loop: one or more requests per connection
        bool keep_alive = true;
        while (keep_alive) {
            // Request-scoped arena (reset per request)
            Arena req_arena = arena(megabytes(1));
            
            // Handle one request
            @match {
                case ServerAction!IoError action = handle_request(&conn, &req_arena, &conn_arena, cfg):
                    // Check for keep-alive
                    if (cfg.mode == Http1_KeepAlive && action != Takeover && !has_connection_close_header(action)) {
                        arena_reset(&req_arena);
                        continue;  // Loop for next request
                    } else {
                        keep_alive = false;  // Close connection
                    }
                
                case is_cancelled() | IoError err:
                    keep_alive = false;  // Break on error or cancellation
            }
        }
        
        // Close connection (Duplex.close or raw_fd)
        try conn.close();
        arena_reset(&conn_arena);
    }
}

@async ServerAction!IoError handle_request(Duplex* conn, Arena* req_arena, Arena* conn_arena, ServerConfig* cfg) {
    // Read request
    Request req = try await read_request_from_duplex(conn, req_arena);
    
    if (cfg.on_request_start) cfg.on_request_start(&req);
    
    // Call handler with deadline (unary only)
    with_deadline(deadline_after(cfg.request_timeout)) {
        ServerAction!IoError action = try cfg.handler(&req, req_arena);
        
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
                return cc_err(IoError::ConnectionClosed);  // Signal to break keep-alive loop
        }
    }
}
```

**Usage Example: Unary Request/Response (HTTP/1.1 with Keep-Alive)**

```c
#include <std/prelude.cch>
#include <std/server.cch>

@async @latency_sensitive ServerAction!IoError api_handler(Request* req, Arena* req_arena) {
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
@async ServerAction!IoError websocket_handler(Request* req, Arena* req_arena) {
    if (req.path == "/ws" && req.headers.contains("Upgrade")) {
        // Return takeover to handle WebSocket protocol
        return ServerAction.Takeover(websocket_connection);
    } else {
        return ServerAction.Reply({.status = 400, .body = "Not a WebSocket request"});
    }
}

@async void!IoError websocket_connection(Duplex* conn, Arena* conn_arena) {
    // Perform WebSocket handshake
    try await ws_handshake(conn);
    
    // Handle messages on connection (lives as long as connection lives)
    while (char[:]?!IoError frame = try await ws_read_frame(conn, conn_arena)) {
        if (!frame) break;  // Connection closed
        
        // Process frame; allocations live in conn_arena
        try await process_ws_message(conn, frame, conn_arena);
    }
    
    // Close connection
    try await conn.close();
}
```

**Usage Example: Streaming Response (SSE)**

```c
@async ServerAction!IoError sse_handler(Request* req, Arena* req_arena) {
    if (req.path == "/events") {
        // For SSE, use Takeover to control the connection directly
        return ServerAction.Takeover(sse_connection);
    } else {
        return ServerAction.Reply({.status = 404});
    }
}

@async void!IoError sse_connection(Duplex* conn, Arena* conn_arena) {
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
        .tls = TlsConfig {
            .cert_path = "/etc/certs/server.crt",
            .key_path = "/etc/certs/server.key"
        }
    };
    
    try await server_loop(cfg);  // Handlers receive decrypted requests
}
```

**Usage Example: Raw TCP Protocol (Takeover Mode)**

```c
@async ServerAction!IoError raw_protocol_handler(Request* req, Arena* req_arena) {
    // For raw TCP (not HTTP), handler always returns Takeover
    // The "Request" struct is just for convenience; raw protocols ignore it
    return ServerAction.Takeover(raw_protocol_loop);
}

@async void!IoError raw_protocol_loop(Duplex* conn, Arena* conn_arena) {
    // Speak custom protocol directly via conn.read() / conn.write()
    while (char[:]?!IoError msg = try await conn.read(conn_arena)) {
        if (!msg) break;
        
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
    @async void!IoError long_lived_handler(Duplex* conn, Arena* conn_arena) {
        size_t message_count = 0;
        ArenaCheckpoint cp = arena_checkpoint(conn_arena);
        while (char[:]?!IoError msg = try await conn.read(conn_arena)) {
            if (!msg) break;
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
- **Half-close:** Use `Duplex.shutdown(Read)` or `Duplex.shutdown(Write)` for proxies and protocols needing unidirectional closure. Standard sentinel `IoError::ConnectionClosed` indicates normal remote closure (not an error).
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
#include <std/prelude.cch>

@async void process_csv(char[:] filename) {
    Arena arena = arena(megabytes(1));
    
    File! f = file_open(&arena, filename, "r");
    if (try File file = f) {
        while (char[:]?!IoError line_result = file.read_line(&arena)) {
            if (try char[:]? line_opt = line_result) {
                if (!line_opt) break;  // EOF (Ok(None))
                char[:] line = *line_opt;
                
                // UFCS: trim and split naturally
                StringSplitIter it = line.trim().split(",");
                while (char[:]? field = it.next()) {
                    std_out.write("Field: ");
                    std_out.write(field.trim());
                    std_out.write("\n");
                }
            }
        }
        file.close();
    }
}
```

**Benefits:** Safe slices, no strtok, declarative loops, built-in error handling, method chaining, ~40% less code, more readable.

### Advanced Example: Data Processing Pipeline

```c
#include <std/prelude.cch>

@async void process_logs() {
    Arena arena = arena(megabytes(10));
    
    // Read file
    File! f = file_open(&arena, "logs.txt", "r");
    if (!try f) return;
    
    File file = f.unwrap();
    Map<int, int> status_counts = map_new<int, int>(&arena);
    
    // Process line by line (UFCS throughout)
    while (char[:]?!IoError line_result = file.read_line(&arena)) {
        if (try char[:]? line_opt = line_result) {
            if (!line_opt) break;  // EOF (Ok(None))
            char[:] line = *line_opt;
            
            // Skip comments
            if (line.starts_with("#")) continue;
            
            // Parse fields
            StringSplitIter fields = line.split(" ");
            if (char[:]? status_str = fields.next()) {
                i64! status = status_str.parse_i64();
                if (try i64 code = status) {
                    // Update count
                    int? count = status_counts.get((int)code);
                    int new_count = (count ? *count : 0) + 1;
                    status_counts.insert((int)code, new_count);
                }
            }
        }
    }
    
    file.close();
    
    // Report results
    String report = string_new(&arena);
    report.append("Status Code Summary:\n");
    // (iteration over map in Phase 2)
    
    std_out.write(report.as_slice());
}
```

---

### 5. Networking (`<std/net.cch>`)

Concurrent-C networking provides safe, async-first primitives for TCP/UDP, TLS, HTTP clients, and DNS. All operations integrate with the arena model and return `T!E` results.

#### 5.1 Design Principles

1. **Arena-first buffers:** All read operations allocate into caller-provided arenas. No hidden mallocs.
2. **Duplex unification:** TCP sockets and TLS connections expose the same `Duplex` interface as server connections.
3. **Explicit lifetimes:** Socket handles are resources that must be closed. Use `@defer` or `@nursery closing()`.
4. **Async by default:** All I/O is async. Sync wrappers exist but prefer async in concurrent code.

#### 5.2 TCP Sockets

```c
// TCP client connection
@async Socket!NetError tcp_connect(char[:] addr);  // "host:port" or "ip:port"

// TCP server listener
Listener!NetError tcp_listen(char[:] addr);        // "0.0.0.0:8080", "[::]:8080"

// Accept connection (async)
@async Socket!NetError Listener.accept();

// Listener lifecycle
void Listener.close();

// Socket is a Duplex — unified read/write interface
// Socket implements Duplex, so all Duplex methods work:
@async char[:]?!IoError Socket.read(Arena* a, size_t max_bytes);
@async size_t!IoError   Socket.write(char[:] data);
@async void!IoError     Socket.shutdown(ShutdownMode mode);
void                    Socket.close();

// Socket-specific methods
char[:]!NetError Socket.peer_addr();   // Remote address as string
char[:]!NetError Socket.local_addr();  // Local address as string
```

**Memory Provenance:**
- `read()` allocates the returned slice in the provided arena
- The slice is valid until the arena is reset/freed
- `write()` does NOT take ownership; data is copied to kernel buffers
- Socket handles contain no arena references; they're just fd wrappers

**Error Type:**
```c
enum NetError {
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
@async void!NetError fetch_data() {
    Arena arena = arena(megabytes(1));
    
    Socket conn = try await tcp_connect("example.com:80");
    @defer conn.close();
    
    try await conn.write("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n");
    
    // Read response into arena
    while (char[:]?!IoError chunk = try await conn.read(&arena, 4096)) {
        if (!chunk) break;  // EOF
        process(*chunk);
    }
}

// TCP server (low-level; prefer server_loop for HTTP)
@async void!NetError echo_server() {
    Listener ln = try tcp_listen("0.0.0.0:9000");
    @defer ln.close();
    
    @nursery {
        while (true) {
            Socket conn = try await ln.accept();
            spawn(() => handle_echo(conn));
        }
    }
}

@async void handle_echo(Socket conn) {
    Arena arena = arena(kilobytes(64));
    @defer conn.close();
    
    while (char[:]?!IoError data = try await conn.read(&arena, 1024)) {
        if (!data) break;
        try await conn.write(*data);
        arena_reset(&arena);  // Reuse buffer space
    }
}
```

#### 5.3 UDP Sockets

```c
// UDP socket (connectionless)
UdpSocket!NetError udp_bind(char[:] addr);  // Bind to local address

// Send to specific address
@async size_t!NetError UdpSocket.send_to(char[:] data, char[:] addr);

// Receive with sender address
struct UdpPacket {
    char[:] data;       // Packet data (allocated in arena)
    char[:] from_addr;  // Sender address (allocated in arena)
};
@async UdpPacket?!NetError UdpSocket.recv_from(Arena* a, size_t max_bytes);

void UdpSocket.close();
```

**Memory Provenance:**
- `recv_from()` allocates both `data` and `from_addr` in the provided arena
- Both slices share arena lifetime
- `send_to()` copies data to kernel; no ownership transfer

#### 5.4 TLS (`<std/tls.cch>`)

TLS wraps a Socket or Duplex to provide encrypted communication. The wrapped connection exposes the same Duplex interface — handlers don't need to know if they're speaking TLS.

```c
// TLS client configuration
struct TlsClientConfig {
    char[:]? ca_cert;           // Optional: custom CA cert path (None = system roots)
    bool verify_hostname;       // Default: true
    char[:]? sni_hostname;      // Optional: override SNI (None = use connect addr)
};

// TLS server configuration (same as ServerConfig.tls)
struct TlsServerConfig {
    char[:] cert_path;          // Server certificate
    char[:] key_path;           // Private key
    char[:]? client_ca;         // Optional: require client certs
};

// Wrap existing socket in TLS (client)
@async Duplex!NetError tls_connect(Socket sock, TlsClientConfig cfg);

// Convenience: TCP + TLS in one call
@async Duplex!NetError tls_connect_addr(char[:] addr, TlsClientConfig cfg);

// Wrap existing socket in TLS (server-side handshake)
@async Duplex!NetError tls_accept(Socket sock, TlsServerConfig cfg);

// TLS connection info (available after handshake)
struct TlsInfo {
    char[:] protocol_version;   // "TLSv1.3", "TLSv1.2"
    char[:] cipher_suite;       // "TLS_AES_256_GCM_SHA384"
    char[:]? peer_cert_subject; // Client cert subject (if client auth)
    char[:]? sni_hostname;      // SNI from client hello
};
TlsInfo? Duplex.tls_info();     // None if not a TLS connection
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
@async void!NetError secure_fetch() {
    Arena arena = arena(megabytes(1));
    
    // Connect with TLS (uses system CA roots by default)
    Duplex conn = try await tls_connect_addr("api.example.com:443", {
        .verify_hostname = true,
    });
    @defer conn.close();
    
    // Duplex interface is identical to plain Socket
    try await conn.write("GET /data HTTP/1.1\r\nHost: api.example.com\r\n\r\n");
    
    while (char[:]?!IoError chunk = try await conn.read(&arena, 4096)) {
        if (!chunk) break;
        process(*chunk);
    }
    
    // Optional: inspect TLS session
    if (TlsInfo? info = conn.tls_info()) {
        printf("Protocol: %.*s\n", (int)info.protocol_version.len, info.protocol_version.ptr);
    }
}
```

#### 5.5 HTTP Client (`<std/http.cch>`)

Convenience layer over TCP+TLS for common HTTP operations.

```c
// Simple GET/POST (one-shot, blocks until complete)
@async HttpResponse!HttpError http_get(Arena* a, char[:] url);
@async HttpResponse!HttpError http_post(Arena* a, char[:] url, char[:] body);

// Response structure
struct HttpResponse {
    u16 status;             // 200, 404, etc.
    char[:] status_text;    // "OK", "Not Found"
    char[:] headers;        // Raw headers
    char[:] body;           // Response body
};

// Configurable client
struct HttpClient {
    Duration timeout;           // Request timeout (default: 30s)
    TlsClientConfig? tls;       // TLS config (None = use defaults)
    char[:]? user_agent;        // User-Agent header
    bool follow_redirects;      // Follow 3xx (default: true, max 10)
};

HttpClient http_client_new();

// Builder methods
HttpClient* HttpClient.timeout(Duration d);
HttpClient* HttpClient.user_agent(char[:] ua);
HttpClient* HttpClient.no_redirects();

// Requests
@async HttpResponse!HttpError HttpClient.get(Arena* a, char[:] url);
@async HttpResponse!HttpError HttpClient.post(Arena* a, char[:] url, char[:] body);
@async HttpResponse!HttpError HttpClient.request(Arena* a, HttpRequest req);

// Full request control
struct HttpRequest {
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
enum HttpError {
    Net(NetError),          // Connection errors
    InvalidUrl,             // URL parse failure
    TooManyRedirects,
    InvalidResponse,        // Malformed HTTP response
    Timeout,
};
```

**Examples:**

```c
// Simple GET
@async void!HttpError fetch_json() {
    Arena arena = arena(megabytes(1));
    
    HttpResponse resp = try await http_get(&arena, "https://api.example.com/users");
    if (resp.status == 200) {
        // resp.body is valid until arena reset
        User[] users = parse_json_users(resp.body, &arena);
        process_users(users);
    }
}

// Configured client
@async void!HttpError fetch_with_config() {
    Arena arena = arena(megabytes(1));
    
    HttpClient client = http_client_new()
        .timeout(seconds(10))
        .user_agent("MyApp/1.0")
        .no_redirects();
    
    HttpResponse resp = try await client.get(&arena, "https://api.example.com/data");
    process(resp);
}

// Custom request
@async void!HttpError post_data() {
    Arena arena = arena(megabytes(1));
    
    HttpRequest req = {
        .method = "POST",
        .url = "https://api.example.com/submit",
        .headers = "Content-Type: application/json\r\nAuthorization: Bearer token\r\n",
        .body = "{\"name\": \"test\"}",
    };
    
    HttpClient client = http_client_new();
    HttpResponse resp = try await client.request(&arena, req);
}
```

#### 5.6 DNS (`<std/dns.cch>`)

Async DNS resolution. Integrates with system resolver by default.

```c
// Resolve hostname to addresses
@async IpAddr[]!NetError dns_lookup(Arena* a, char[:] hostname);

// IP address (v4 or v6)
struct IpAddr {
    enum { V4, V6 } family;
    union {
        u8 v4[4];
        u8 v6[16];
    };
};

// Format IP as string
char[:] IpAddr.to_string(Arena* a);

// Parse string to IP (no DNS, just parsing)
IpAddr!NetError ip_parse(char[:] s);
```

**Memory Provenance:**
- `dns_lookup()` returns slice of IpAddr allocated in arena
- `to_string()` allocates the formatted string in arena
- Both valid until arena reset

**Example:**

```c
@async void!NetError connect_by_name() {
    Arena arena = arena(kilobytes(4));
    
    IpAddr[] addrs = try await dns_lookup(&arena, "example.com");
    if (addrs.len == 0) {
        return cc_err(NetError::DnsFailure);
    }
    
    // Try each address until one works
    for (size_t i = 0; i < addrs.len; i++) {
        char[:] addr_str = addrs[i].to_string(&arena);
        char[:] full_addr = string_new(&arena)
            .append(addr_str)
            .append(":443")
            .as_slice();
        
        if (try Socket sock = await tcp_connect(full_addr)) {
            return handle_connection(sock);
        }
    }
    
    return cc_err(NetError::HostUnreachable);
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
@async void!IoError process_stream(Duplex* conn, Arena* arena) {
    ArenaCheckpoint cp = arena_checkpoint(arena);
    size_t count = 0;
    
    while (char[:]?!IoError msg = try await conn.read(arena, 4096)) {
        if (!msg) break;
        process(*msg);
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
- C `libc`: Keep what works (stdio, basic ops), add safety (T!E, slices, UFCS).

---

## Appendix: API Quick Reference

### String Methods on char[:]

| Method | Purpose |
|--------|---------|
| `.len()` | Length |
| `.get(i)` | Safe index (optional) |
| `.slice(start, end)` | Subslice view (optional) |
| `.c_str(a)` | NUL-terminated copy for C interop |
| `.clone(a)` | Copy to new allocation |
| `.trim()`, `.trim_left()`, `.trim_right()` | Trim whitespace |
| `.is_empty()`, `.is_ascii()` | Checks |
| `.starts_with()`, `.ends_with()`, `.contains()` | Prefix/suffix/contains |
| `.index_of()`, `.last_index_of()`, `.count()` | Find |
| `.upper(a)`, `.lower(a)` | Case conversion |
| `.replace(a, old, new)` | Replace all |
| `.split(delim)` | Iterator split |
| `.split_all(a, delim)` | Collect all |
| `.parse_i64()`, `.parse_f64()`, `.parse_bool()` | Parse |

### String Builder Methods

| Method | Purpose |
|--------|---------|
| `string_new(a)` | Create |
| `.append()`, `.append_char()`, `.append_int()`, `.append_float()` | Append |
| `.append_if()` | Conditional append |
| `.as_slice()` | Finalize to view |
| `.len()`, `.cap()` | Info |
| `.clear()` | Clear |

### File Methods

| Method | Purpose |
|--------|---------|
| `file_open(a, path, mode)` | Open |
| `.read()`, `.read_all()`, `.read_line()` | Read |
| `.write()` | Write |
| `.seek()`, `.tell()` | Position |
| `.sync()` | Flush |
| `.close()` | Close |

### Vec<T> Methods

| Method | Purpose |
|--------|---------|
| `vec_new<T>(a)` | Create |
| `.push()`, `.pop()` | Add/remove |
| `.get()`, `.set()` | Access |
| `.len()`, `.cap()` | Info |
| `.iter()` | Iterator |
| `.as_slice()` | View |
| `.clear()` | Clear |

### Map<K,V> Methods

| Method | Purpose |
|--------|---------|
| `map_new<K,V>(a)` | Create |
| `.insert()` | Insert/update |
| `.get()`, `.get_mut()` | Lookup |
| `.remove()` | Remove |
| `.len()`, `.cap()` | Info |
| `.clear()` | Clear |

---

**End of Specification**
