# Concurrent-C Standard Library Specification

This document defines the shipped standard-library headers, public types, public C
callees, and standard-library UFCS families.

## Headers

The standard library provides these headers under `<ccc/std/...>`:

- `prelude.cch`
- `slice.cch`
- `string.cch`
- `io.cch`
- `vec.cch`
- `map.cch`, `map_forward.cch`, and `map_impl.cch`
- `array_map.cch`
- `dir.cch`
- `process.cch`
- `exec.cch`
- `async_io.cch`
- `future.cch`
- `task.cch`
- `cli.cch`
- `net.cch`
- `dns.cch`
- `tls.cch`
- `http.cch`
- `hash.cch`

Portable atomics are provided separately by `<ccc/cc_atomic.cch>`.

`<ccc/std/prelude.cch>` includes the core runtime headers and the stdlib slice,
string, I/O, vector, map-forward, array-map, directory, process, command,
async-I/O, and future headers. Networking, DNS, TLS, HTTP, CLI, task, hash, and
full map headers are included explicitly when needed.

Public C types use the `CC` prefix and public C functions use the `cc_` prefix.
Unless a section states otherwise, a slice returned from an operation that
accepts a `CCArena *` remains valid until that arena releases or reuses its
storage.

## Generic factories and UFCS

The generic collection factories are:

```c
CCVec::[T] cc_vec_new::[T](CCArena *arena);
Map::[K, V] map_new::[K, V](CCArena *arena);
ArrayMap::[K, V] array_map_new::[K, V](CCArena *arena);
ArrayMap::[K, V] array_map_new_count::[K, V](CCArena *arena, size_t count);
```

`CCVec::[T]` denotes the generated C family `CCVec_<T-mangling>`.
`cc_vec_new::[T](arena)` calls:

```c
CCVec_<T-mangling>_init(arena, CC_VEC_INITIAL_CAP)
```

`Map::[K, V]` denotes the generated C family
`Map_<K-mangling>_<V-mangling>` (inline open-addressing). `map_new::[K, V](arena)`
calls:

```c
Map_<K-mangling>_<V-mangling>_init(arena)
```

`ArrayMap::[K, V]` denotes the generated C family
`ArrayMap_<K-mangling>_<V-mangling>` (pow2 `u32` probe index + dense key/value
rows). `array_map_new::[K, V](arena)` calls:

```c
ArrayMap_<K-mangling>_<V-mangling>_init(arena)
```

`array_map_new_count::[K, V](arena, count)` calls:

```c
ArrayMap_<K-mangling>_<V-mangling>_init_count(arena, count)
```

Prefer `ArrayMap` when values are wide (empty buckets stay small). Prefer `Map`
when keys and values are tiny and probe locality matters.

For a generated vector or map value, UFCS selects the corresponding generated
family function and passes the receiver by address. For public struct families
with `cc_<family>_<method>` functions, UFCS selects that prefixed function and
passes the receiver in the form required by its C signature. The exact
family-specific mappings are listed below.

## Slices

The language specification defines the `CCSlice`, `CCSliceUnique`,
`CCSliceShared`, and `CCSliceHdr` ABI and ownership rules. The standard library
also uses `CCSliceArray`, a pointer-length sequence of `CCSlice` values.

Construction and lifetime functions are:

```c
CCSliceHdr cc_slice_hdr(CCSlice *s);
CCSlice cc_slice_empty(void);
CCSlice cc_slice_from_buffer(void *ptr, size_t len);
CCSlice cc_slice_from_static(void *ptr, size_t len);
CCSlice cc_slice_hdr_slice(CCSliceHdr *sh);
CCSlice cc_slice_from_parts(void *ptr, size_t len, uint64_t id, size_t available_len);
CCSlice cc_slice_from_cstr(const char *cstr);
CCSliceUnique cc_adopt(void *ptr, size_t nbytes, CCSliceDeleter deleter);
void cc_slice_destroy(CCSlice *s);
```

`cc_slice_from_buffer` and `cc_slice_hdr_slice` produce untracked slices.
`cc_slice_from_static` and `cc_slice_from_cstr` produce canonical static slices.
`cc_adopt` registers the supplied deleter and produces a unique,
non-transferable slice. `cc_slice_destroy` invokes that deleter at most once
for a still-registered unique slice and then clears the slice.

The query and view operations are:

```c
bool cc_slice_is_empty(CCSlice *s);
size_t cc_slice_capacity(CCSlice s);
const char *cc_slice_str(CCSlice *s);
const uint8_t *cc_slice_bytes(CCSlice *s);
bool cc_slice_is_ascii(CCSlice s);
bool cc_slice_get(CCSlice s, size_t idx, char *out);
size_t cc_slice_index_of(CCSlice s, CCSlice needle, bool *found);
size_t cc_slice_last_index_of(CCSlice s, CCSlice needle, bool *found);
size_t cc_slice_count(CCSlice s, CCSlice needle);
CCSlice cc_slice_trim_set(CCSlice s, CCSlice chars);
uint64_t cc_slice_hash64(CCSlice s);
size_t cc_slice_len(CCSlice *s);
CCSlice cc_slice_trim(CCSlice *s);
CCSlice cc_slice_trim_left(CCSlice *s);
CCSlice cc_slice_trim_right(CCSlice *s);
CCSlice cc_slice_sub(CCSlice s, size_t start, size_t end);
bool cc_slice_starts_with(CCSlice *s, CCSlice prefix);
bool cc_slice_ends_with(CCSlice *s, CCSlice suffix);
bool cc_slice_eq(CCSlice *s, CCSlice other);
bool cc_slice_eq_cstr(CCSlice *s, const char *cstr);
```

An invalid `cc_slice_sub` range returns an empty slice. A subslice clears
uniqueness, preserves transferability, and marks a view that does not cover the
full allocation as a subslice. `cc_slice_get` reports absence through its
`bool` return (non-Result C twin). The index-of helpers report absence through
`found`.

Slice UFCS maps `hdr`, `len`, `trim`, `trim_left`, `trim_right`, `sub`,
`starts_with`, `ends_with`, `eq`, `eq_cstr`, and `destroy` to the corresponding
`CCSlice_*` or `cc_slice_*` function. Checked index UFCS (`at`, `get_checked`,
`set`) is documented under arena-backed slice operations below.

### Arena-backed slice operations

`<ccc/std/slice.cch>` provides:

```c
CCResult_CCSlice_CCError cc_slice_clone_into(CCSlice *src, CCArena *arena);
CCResult_CCSliceHdr_CCError cc_slice_hdr_clone_into(CCSliceHdr *src, CCArena *arena);

/* Stabilize `*s` in `arena` (mutate in place). */
bool !>(CCError) cc_slice_materialize_in(CCSlice *s, CCArena *arena);

/* Checked index — same Result/error in all builds (no debug/release split). */
char !>(CCError) cc_slice_get_checked(CCSlice *s, size_t idx);
char !>(CCError) cc_slice_at(CCSlice *s, size_t idx);          /* alias of get_checked */
bool !>(CCError) cc_slice_set(CCSlice *s, size_t idx, char c);
```

`materialize_in` is a no-op when the slice is empty, canonical/static, or
already from `arena`'s provenance epoch; otherwise it clones into `arena` and
replaces `*s`. It does not free the prior view. UFCS: `s.materialize_in(arena)`.

Out-of-bounds or null-pointer index ops return `CC_ERR_INVALID_ARG`. Soft-zero
`at` is gone. Raw `s.ptr[i]` / `((char*)s.ptr)[i]` remains an untracked Gap
outside this surface.

`<ccc/std/string.cch>` provides:

```c
CCSlice cc_slice_clone(CCArena *arena, CCSlice s);
char *cc_slice_c_str(CCArena *arena, CCSlice s);
CCSliceArray cc_slice_split_all(CCArena *arena, CCSlice s, CCSlice delim);
size_t cc_slice_concat_lenv(const CCSlice *parts, size_t count);
CCSlice cc_slice_concat_into(void *dst, size_t cap, const CCSlice *parts, size_t count);
CCSlice cc_slice_concat_many(CCArena *arena, const CCSlice *parts, size_t count);
CCSlice cc_concat(CCArena *arena, ...);
```

Clone operations copy bytes into the supplied arena. `cc_slice_c_str` appends a
NUL byte. `cc_slice_concat_into` returns an empty slice when the destination is
null or too small. `cc_concat` accepts one through eight `CCSlice`, C-string, or
`CCString` arguments.

### Strict conversions

The shipped slice conversion functions are:

```c
CCResult_int64_t_CCError cc_slice_to_i64(const CCSlice *s);
CCResult_uint64_t_CCError cc_slice_to_u64(const CCSlice *s);
CCResult_double_CCError cc_slice_to_f64(const CCSlice *s);
```

`cc_slice_to_i64` accepts decimal digits with an optional leading `-`.
`cc_slice_to_u64` accepts decimal digits only. Both consume the entire slice,
reject empty input, and return `CC_ERR_PARSE` for invalid syntax and
`CC_ERR_INVALID_ARG` for overflow.

`cc_slice_to_f64` applies `strtod` to a NUL-terminated copy and requires the
entire slice to be consumed. Empty or partially consumed input returns
`CC_ERR_PARSE`; overflow returns `CC_ERR_INVALID_ARG`; allocation failure
returns `CC_ERR_OUT_OF_MEMORY`.

UFCS maps `s.to_i64()`, `s.to_u64()`, and `s.to_f64()` to these functions with
`&s`.

### Checked integer arithmetic

```c
CCResult_int64_t_CCError cc_add_i64_checked(int64_t a, int64_t b);
CCResult_int64_t_CCError cc_sub_i64_checked(int64_t a, int64_t b);
CCResult_int64_t_CCError cc_mul_i64_checked(int64_t a, int64_t b);
```

Each function returns the exact result or `CC_ERR_INVALID_ARG` when the
operation overflows `int64_t`.

## Strings

`<ccc/std/string.cch>` defines the owning string builder:

```c
typedef struct CCString {
    union {
        char *data;
        char inline_buf[sizeof(void *)];
        uintptr_t _inline_word;
    };
    uint32_t len;
    uint32_t cap;
} CCString;
```

The public constructors, accessors, mutation functions, and lifetime functions
are:

```c
CCString cc_string_new(void);
CCString cc_string_with_capacity(CCArena *arena, size_t cap);
CCString cc_string_from_slice(CCArena *arena, CCSlice slice);
CCString cc_string_from(value, CCArena *arena);

bool cc_string_failed(const CCString *str);
size_t cc_string_len(const CCString *str);
size_t cc_string_cap(const CCString *str);
bool cc_string_is_inline(const CCString *str);
const char *cc_string_data_const(const CCString *str);
char *cc_string_data(CCString *str);
CCArena *cc_string_arena(const CCString *str);
uint64_t cc_string_provenance(const CCString *str);
CCSlice cc_string_as_slice(const CCString *str);
CCSlice cc_string_persist_slice(CCArena *arena, const CCString *str);
bool !>(CCError) cc_string_materialize_in(CCString *str, CCArena *arena);
const char *cc_string_cstr(CCString *str, CCArena *arena);

char *cc_string_reserve(CCString *str, size_t need, CCArena *arena);
CCString *cc_string_push_buffer(CCString *str, const char *buffer, uint32_t len, CCArena *arena);
CCString *cc_string_push_slice(CCString *str, CCSlice data, CCArena *arena);
CCString *cc_string_push_char(CCString *str, char c, CCArena *arena);
CCString *cc_string_push_int(CCString *str, int64_t value, CCArena *arena);
CCString *cc_string_push_uint(CCString *str, uint64_t value, CCArena *arena);
CCString *cc_string_push_f32(CCString *str, float value, CCArena *arena);
CCString *cc_string_push_f64(CCString *str, double value, CCArena *arena);
CCString *cc_string_push_float(CCString *str, double value, CCArena *arena);
CCString *cc_string_push_cstr(CCString *str, const char *value, CCArena *arena);
CCString *cc_string_clear(CCString *str);
CCString *cc_string_push(CCString *str, value, CCArena *arena);
CCString *cc_string_append(CCString *str, value, CCArena *arena);

void cc_string_release_heap(CCString *str);
void cc_string_release(CCString *str, CCArena *arena);
```

`cc_string_from` has `_Generic` associations for `CCSlice`, `char *`,
`const char *`, `CCString`, `CCString *`, `const CCString *`, `char`,
`signed char`, `unsigned char`, `short`, `unsigned short`, `int`, `unsigned`,
`long`, `unsigned long`, `long long`, `unsigned long long`, `float`, `double`,
and `bool`.

The push/append scalar dispatch has `_Generic` associations for `char`,
`signed char`, `unsigned char`, `short`, `unsigned short`, `int`, `unsigned`,
`long`, `unsigned long`, `long long`, `unsigned long long`, `float`, `double`,
and `bool`. These append decimal integers, one character for `char`, `true` or
`false` for `bool`, and floating-point text. The same dispatch also accepts
`CCSlice`, C strings, and `CCString` values and pointers.

The builder stores short values inline and stores larger values in arena-owned
storage. Heap growth multiplies capacity by 1.6 (`(cap * 8) / 5`). A growth
failure poisons the string. On a poisoned string,
`cc_string_failed` is true, push operations return null,
`cc_string_as_slice` is empty, and `cc_string_cstr` returns null.
`cc_string_clear` restores a valid empty string.

`cc_string_materialize_in` leaves inline and empty strings unchanged. A heap
string already owned by `arena` (same arena pointer or matching provenance) is
a no-op; otherwise the bytes are copied into a new string in `arena` and
`*str` is replaced. UFCS: `s.materialize_in(arena)`.

Scalar conversion helpers have the form:

```c
CCString <scalar-type>_to_str(<scalar-type> value, CCArena *arena);
```

The named helpers are `char_to_str`, `signed_char_to_str`,
`unsigned_char_to_str`, `short_to_str`, `unsigned_short_to_str`, `int_to_str`,
`unsigned_to_str`, `long_to_str`, `unsigned_long_to_str`, `long_long_to_str`,
`unsigned_long_long_to_str`, the `int8_t` through `uint64_t` fixed-width
families, `intptr_t_to_str`, `uintptr_t_to_str`, `float_to_str`,
`double_to_str`, and `bool_to_str`. The `intptr_t` and `uintptr_t` helpers are
direct per-type entry points; they are not distinct associations in
`cc_string_from`. Integer formatting is decimal, boolean formatting is `true`
or `false`, and floating-point formatting uses `%g` unless the optional XJB
formatter is enabled.

## File and buffered I/O

`<ccc/std/io.cch>` defines:

```c
typedef struct {
    FILE *handle;
} CCFile;

typedef struct {
    CCFile *file;
    char *buf;
    size_t cap;
    size_t len;
    size_t pos;
    int eof;
} CCBufReader;

typedef struct {
    CCFile *file;
    char *buf;
    size_t cap;
    size_t len;
} CCBufWriter;
```

The file API is:

```c
int cc_file_open(CCFile *file, const char *path, const char *mode);
void cc_file_close(CCFile *file);
CCResult_CCSlice_CCIoError cc_file_read_all(CCFile *file, CCArena *arena);
CCResult_CCSlice_CCIoError cc_file_read(CCFile *file, CCArena *arena, size_t n);
CCResult_bool_CCIoError cc_file_read_into(CCFile *file, CCArena *arena, size_t n, CCSlice *out);
CCResult_CCSlice_CCIoError cc_file_read_line(CCFile *file, CCArena *arena);
CCResult_bool_CCIoError cc_file_read_line_into(CCFile *file, CCArena *arena, CCSlice *out);
CCResult_size_t_CCIoError cc_file_write(CCFile *file, CCSlice data);
CCResult_size_t_CCIoError cc_file_read_buf(CCFile *file, void *buf, size_t n);
CCResult_bool_CCIoError cc_file_read_buf_into(CCFile *file, void *buf, size_t n, size_t *out);
CCResult_size_t_CCIoError cc_file_write_buf(CCFile *file, const void *buf, size_t n);
CCResult_size_t_CCIoError cc_file_sync(CCFile *file);
CCResult_size_t_CCIoError cc_file_seek(CCFile *file, long offset, int whence);
CCResult_size_t_CCIoError cc_file_tell(CCFile *file);
CCResult_size_t_CCIoError cc_file_size(CCFile *file);
```

`cc_file_open` returns zero on success and `-1` on failure. The failure's
`errno` remains available separately.
`cc_file_close` ignores close errors. Value-returning reads return an empty
slice at EOF. The `_into` read forms return `ok(true)` when they write an
output value and `ok(false)` at EOF. `cc_file_read_line` includes the newline
when one is read. `cc_file_write` and `cc_file_write_buf` return the number of
bytes written. `cc_file_size` returns `Ok(0)` for a non-seekable stream and
does not change the current position.

`CCFile` UFCS maps file methods to `cc_file_*` and passes `&file`.

Standard stream writes are:

```c
CCResult_size_t_CCIoError cc_std_out_write(CCSlice data);
CCResult_size_t_CCIoError cc_std_err_write(CCSlice data);
CCResult_size_t_CCIoError cc_std_out_write_auto(value);
CCResult_size_t_CCIoError cc_std_err_write_auto(value);
```

The automatic forms accept a slice, C string, or `CCString` value or pointer.

Buffered I/O uses:

```c
int cc_buf_reader_init(CCBufReader *reader, CCFile *file, CCArena *arena, size_t cap);
CCResult_CCSlice_CCIoError cc_buf_reader_next(CCBufReader *reader, size_t n);
CCResult_CCSlice_CCIoError cc_buf_reader_read_line(CCBufReader *reader, CCArena *arena);
int cc_buf_writer_init(CCBufWriter *writer, CCFile *file, CCArena *arena, size_t cap);
CCResult_size_t_CCIoError cc_buf_writer_flush(CCBufWriter *writer);
CCResult_size_t_CCIoError cc_buf_writer_write(CCBufWriter *writer, CCSlice data);
```

An empty successful buffered-reader result denotes EOF.

### Async file operations

`<ccc/std/async_io.cch>` defines:

```c
typedef struct CCAsyncHandle {
    CCChan *done;
    volatile int cancelled;
} CCAsyncHandle;

void cc_async_handle_init(CCAsyncHandle *handle);
void cc_async_handle_free(CCAsyncHandle *handle);
int cc_async_wait(CCAsyncHandle *handle);
int cc_async_wait_timed(CCAsyncHandle *handle, const struct timespec *absolute_deadline);
int cc_async_wait_deadline(CCAsyncHandle *handle, const CCDeadline *deadline);
void cc_async_cancel(CCAsyncHandle *handle);
```

Submission functions return the channel submission result. Completion writes
operation results into caller-provided storage and sends the operation's
completion code through the handle. In particular, `cc_file_open_async`
completes with the actual `cc_file_open` result, including `-1` on open
failure; this code is not an errno value.

```c
int cc_file_open_async(CCExec *ex, CCFile *file, const char *path, const char *mode, CCAsyncHandle *handle);
int cc_file_close_async(CCExec *ex, CCFile *file, CCAsyncHandle *handle);
int cc_file_read_all_async(CCExec *ex, CCFile *file, CCArena *arena, CCSlice *out, CCAsyncHandle *handle);
int cc_file_read_async(CCExec *ex, CCFile *file, CCArena *arena, size_t n, CCSlice *out, CCAsyncHandle *handle);
int cc_file_read_line_async(CCExec *ex, CCFile *file, CCArena *arena, CCSlice *out, CCAsyncHandle *handle);
int cc_file_write_async(CCExec *ex, CCFile *file, CCSlice data, size_t *out_written, CCAsyncHandle *handle);
```

### Path helpers

Path helpers are part of `<ccc/std/io.cch>`:

```c
char cc_path_sep(void);
bool cc_path_is_abs(CCSlice path);
CCSlice cc_path_join(CCArena *arena, CCSlice a, CCSlice b);
CCSlice cc_path_dirname(CCArena *arena, CCSlice path);
CCSlice cc_path_basename(CCArena *arena, CCSlice path);
```

The returned `join`, `dirname`, and `basename` slices are NUL-terminated and
arena-backed. These helpers implement POSIX path syntax only:
`cc_path_sep()` is `/`, and `cc_path_is_abs` recognizes only a leading `/`.

## Collections

### Vectors

`<ccc/std/vec.cch>` defines arena-backed vector families with:

```c
#define CC_VEC_DECL_ARENA(T, Name) /* declares the Name family */
```

Each generated `Name` has `T *data` and `size_t len`, with capacity and arena
metadata stored by the vector core. Its public family is:

```c
Name Name_init(CCArena *arena, size_t initial_cap);
int Name_reserve(Name *vec, size_t need);
int Name_push(Name *vec, T value);
T *Name_push_ptr(Name *vec);
bool Name_pop(Name *vec, T *out);
T *Name_get(Name *vec, size_t index);
T *Name_get_ptr(Name *vec, size_t index);
int Name_set(Name *vec, size_t index, T value);
T *Name_at_grow(Name *vec, size_t index);
void Name_clear(Name *vec);
CCSlice Name_as_slice(const Name *vec);
uint64_t Name_provenance(const Name *vec);
size_t Name_len(const Name *vec);
size_t Name_cap(const Name *vec);
T *Name_begin(Name *vec);
T *Name_end(Name *vec);
T *Name_data(Name *vec);
```

`push`, `reserve`, and `set` return zero on success and `-1` on failure.
`pop` returns false when empty. `get` and `get_ptr` return null out of bounds.
`at_grow` extends the logical length through the requested index and returns
null on allocation failure. Growth multiplies capacity by 1.6 (`(cap * 8) / 5`).
A pointer or slice into a vector is invalidated by an operation that grows its
storage.

Vector UFCS maps these method names to the generated family with `&vec`.
`CCVec_char` and `CCVec_size_t` are predefined. `CC_VEC_FOREACH` iterates in
increasing index order.

`CC_VEC_DECL_HEAP(T, Name)` declares the heap-backed family with
`Name Name_init(void)`, `free`,
`reserve`, `push`, `push_ptr`, `pop`, `get`, `get_ptr`, `at_grow`, `clear`,
`len`, `cap`, `begin`, `end`, and `data`.

### Maps

`<ccc/std/map.cch>` defines arena-backed map families with:

```c
#define CC_MAP_DECL_ARENA(K, V, Name, HASH_FN, EQ_FN) /* declares the Name family */
```

The public generated family is:

```c
Name *Name_init(CCArena *arena);
Name *Name_init_count(CCArena *arena, size_t count);
void Name_destroy(Name *map);
int Name_insert(Name *map, K key, V value);
int Name_put(Name *map, K key, V value, int *ret);
V *Name_get(Name *map, K key);
V *Name_get_ptr(Name *map, K key);
bool Name_remove(Name *map, K key);
bool Name_del(Name *map, K key);
size_t Name_len(const Name *map);
size_t Name_cap(const Name *map);
void Name_clear(Name *map);
```

`init` and `init_count` return null on allocation failure. `insert` returns zero
on success and `-1` on failure. `put` writes `1` to `ret` for a new key, `0`
for a replaced key, and `-1` on failure; its nonnegative return is the bucket
index. `get` and `get_ptr` return a pointer to the stored value or null.
`remove` and `del` report whether the key existed.

Probe-table capacity stays a power of two (2× growth) for quadratic probing.
Map UFCS maps `insert`, `put`, `get`, `get_ptr`, `remove`, `del`, `len`, `cap`,
`clear`, and `destroy` to the generated family. `CC_MAP_FOREACH` exposes each
entry without defining a stable traversal order.

The convenience declarations are:

```c
#define CC_MAP_DECL_INT(V, Name)   CC_MAP_DECL_ARENA(int, V, Name, cc_map_hash_i32, cc_map_eq_i32)
#define CC_MAP_DECL_U64(V, Name)   CC_MAP_DECL_ARENA(uint64_t, V, Name, cc_map_hash_u64, cc_map_eq_u64)
#define CC_MAP_DECL_SLICE(V, Name) CC_MAP_DECL_ARENA(CCSlice, V, Name, cc_map_hash_slice, cc_map_eq_slice)
```

Their `_FULL(V, Name, OptV_ignored)` forms expand to the same generated family
and ignore the final argument.

### Array maps

`<ccc/std/array_map.cch>` defines arena-backed array-map families with a pow2
`uint32_t` probe index and a dense `(key, value)` row store:

```c
#define CC_ARRAY_MAP_DECL(K, V, Name, HASH_FN, EQ_FN) /* declares the Name family */
```

`EQ_FN` returns non-zero when keys are equal (same convention as `cc_map_eq_*`).

The public generated family is:

```c
Name *Name_init(CCArena *arena);
Name *Name_init_count(CCArena *arena, size_t count);
void Name_destroy(Name *map);
int Name_insert(Name *map, K key, V value);
V *Name_get(Name *map, K key);
V *Name_get_ptr(Name *map, K key);
bool Name_remove(Name *map, K key);
bool Name_del(Name *map, K key);
size_t Name_len(const Name *map);
size_t Name_cap(const Name *map); /* probe-table bucket count */
size_t Name_live_bytes(const Name *map);
void Name_clear(Name *map);
```

`init` / `init_count` return null on allocation failure. `insert` returns zero
on success and `-1` on failure. `get` / `get_ptr` return a pointer into the
dense store or null. `cap` is the probe-table capacity (power of two), not the
dense row capacity. `CC_ARRAY_MAP_FOREACH` iterates dense rows in insertion
order (swap-remove on delete may reorder).

Sugar `ArrayMap::[K, V]` / `array_map_new::[K, V]` /
`array_map_new_count::[K, V]` lowers to the `ArrayMap_<K>_<V>` family with the
same key hash/eq selection as `Map::[K, V]` for built-in key kinds (`int`,
`uint64_t`, `CCSlice`, `CCSliceHdr`).

Array-map UFCS maps `insert`, `get`, `get_ptr`, `remove`, `del`, `len`, `cap`,
`live_bytes`, `clear`, and `destroy` to the generated family.

### Static maps

`<ccc/std/static_map.cch>` provides a comptime perfect-hash map:

```c
typedef struct CCStaticMapEntry {
    const char *key;
    const char *value; /* C initializer source for one value_type */
} CCStaticMapEntry;

enum {
    CC_STATIC_MAP_CASE_SENSITIVE = 0,
    CC_STATIC_MAP_ASCII_CI = 1,
};

@comptime void static_map(const char *name,
                          const char *value_type,
                          const void *entries,
                          size_t count,
                          int flags);
```

`entries` is an array of `CCStaticMapEntry`. At the comptime call site the
function searches for a collision-free FNV-1a seed into a power-of-two slot
table and emits keys, values, slots, and:

```c
static const value_type *name_get(CCSlice key);
```

Lookup is `hash(key) -> slot -> verify -> &value`, with exact or ASCII
case-insensitive verification according to `flags`. A miss returns null.
Invalid arguments, duplicate keys (under the selected match policy), keys
requiring C-string escaping, and failure to construct a perfect hash are
compile-time errors.

### Hash helpers

`<ccc/std/hash.cch>` provides:

```c
uint64_t cc_hash_u64(uint64_t value);
uint64_t cc_hash_slice(CCSlice value);
bool cc_eq_slice(CCSlice a, CCSlice b);
```

## Directories and globbing

`<ccc/std/dir.cch>` defines:

```c
typedef enum {
    CC_DIRENT_FILE,
    CC_DIRENT_DIR,
    CC_DIRENT_SYMLINK,
    CC_DIRENT_OTHER
} CCDirEntryType;

typedef struct {
    CCSlice name;
    CCDirEntryType type;
} CCDirEntry;

typedef struct CCDirIter CCDirIter;

typedef struct {
    CCSlice *paths;
    size_t count;
    size_t capacity;
} CCGlobResult;
```

The API is:

```c
CCResult_CCDirIterptr_CCIoError cc_dir_open(CCArena *arena, const char *path);
CCResult_CCDirEntry_CCIoError cc_dir_next(CCDirIter *iter, CCArena *arena);
void cc_dir_close(CCDirIter *iter);

bool cc_path_exists(const char *path);
bool cc_path_is_dir(const char *path);
bool cc_path_is_file(const char *path);
CCResult_bool_CCIoError cc_dir_create(const char *path);
CCResult_bool_CCIoError cc_dir_create_all(const char *path);
CCResult_bool_CCIoError cc_dir_remove(const char *path);
CCResult_bool_CCIoError cc_file_remove(const char *path);
CCSlice cc_dir_cwd(CCArena *arena);
CCResult_bool_CCIoError cc_dir_chdir(const char *path);

CCGlobResult cc_glob(CCArena *arena, const char *pattern);
bool cc_glob_match(const char *pattern, const char *name);
```

After the final entry, `cc_dir_next` returns
`Err((CCIoError){ .kind = CC_IO_OTHER, .os_code = 0 })`. Entry names and glob
paths are allocated in the supplied arena. `cc_dir_create` does not create
parents; `cc_dir_create_all` does. Globbing supports `*`, `?`, and recursive
`**`; `cc_glob_match` matches a single name with `*` and `?`.

The accessor functions are:

```c
const char *cc_dir_entry_name_str(const CCDirEntry *entry);
bool cc_dir_entry_is_dir(const CCDirEntry *entry);
bool cc_dir_entry_is_file(const CCDirEntry *entry);
bool cc_dir_entry_is_symlink(const CCDirEntry *entry);
size_t cc_glob_result_len(const CCGlobResult *result);
const char *cc_glob_result_get(const CCGlobResult *result, size_t index);
```

UFCS on `CCDirEntry` and `CCGlobResult` maps these accessors to their
corresponding C functions.

## Processes and commands

`<ccc/std/process.cch>` defines:

```c
typedef struct CCProcess {
#ifdef _WIN32
    void *handle;
    uint32_t pid;
#else
    int pid;
#endif
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
} CCProcess;

typedef struct CCProcessStatus {
    bool exited;
    bool signaled;
    int exit_code;
} CCProcessStatus;

typedef struct CCProcessConfig {
    const char *program;
    const char **args;
    const char **env;
    const char *cwd;
    bool pipe_stdin;
    bool pipe_stdout;
    bool pipe_stderr;
    bool merge_stderr;
} CCProcessConfig;

typedef struct CCProcessOutput {
    CCSlice stdout_data;
    CCSlice stderr_data;
    CCProcessStatus status;
} CCProcessOutput;
```

`args` and `env` are null-terminated arrays. A null `env` inherits the current
environment, and a null `cwd` inherits the current working directory.

Process creation and management functions are:

```c
CCResult_CCProcess_CCIoError cc_process_spawn(const CCProcessConfig *config);
CCResult_CCProcess_CCIoError cc_process_spawn_simple(const char *program, const char **args);
CCResult_CCProcess_CCIoError cc_process_spawn_shell(const char *command);
CCResult_CCProcessStatus_CCIoError cc_process_wait(CCProcess *process);
CCResult_CCProcessStatus_CCIoError cc_process_try_wait(CCProcess *process);
CCResult_CCProcessStatus_CCIoError cc_process_wait_timeout_ms(CCProcess *process, int64_t timeout_ms);
CCResult_CCProcessStatus_CCIoError cc_process_wait_timeout(CCProcess *process, int timeout_sec);
CCResult_bool_CCIoError cc_process_kill(CCProcess *process, int signal);
int cc_process_id(const CCProcess *process);
```

`try_wait` returns `CC_IO_BUSY` while the process runs. Timed waits report an
`ETIMEDOUT` OS code when their timeout expires. A negative timeout waits
without a time bound.

Piped I/O and capture functions are:

```c
CCResult_size_t_CCIoError cc_process_write(CCProcess *process, CCSlice data);
CCResult_CCSlice_CCIoError cc_process_read(CCProcess *process, CCArena *arena, size_t max_bytes);
CCResult_CCSlice_CCIoError cc_process_read_stderr(CCProcess *process, CCArena *arena, size_t max_bytes);
void cc_process_close_stdin(CCProcess *process);
CCResult_CCSlice_CCIoError cc_process_read_all(CCProcess *process, CCArena *arena);
CCResult_CCSlice_CCIoError cc_process_read_all_stderr(CCProcess *process, CCArena *arena);
CCResult_CCProcessOutput_CCIoError cc_process_run_config(CCArena *arena, const CCProcessConfig *config);
CCResult_CCProcessOutput_CCIoError cc_process_run_with_input(CCArena *arena, const CCProcessConfig *config, CCSlice input);
CCResult_CCProcessOutput_CCIoError cc_process_run(CCArena *arena, const char *program, const char **args);
CCResult_CCProcessOutput_CCIoError cc_process_run_shell(CCArena *arena, const char *command);
```

The process-output accessors are:

```c
const char *cc_process_output_stdout_str(const CCProcessOutput *output);
size_t cc_process_output_stdout_len(const CCProcessOutput *output);
const char *cc_process_output_stderr_str(const CCProcessOutput *output);
size_t cc_process_output_stderr_len(const CCProcessOutput *output);
int cc_process_output_exit_code(const CCProcessOutput *output);
bool cc_process_output_success(const CCProcessOutput *output);
```

`cc_process_output_success` is true exactly when the process exited normally
and its exit code is zero.

Environment functions are:

```c
CCSlice cc_env_get(CCArena *arena, const char *name);
CCResult_bool_CCIoError cc_env_set(const char *name, const char *value);
CCResult_bool_CCIoError cc_env_unset(const char *name);
```

`cc_env_get` returns an empty slice when the variable is absent or its arena
allocation fails.

### Command builder

`<ccc/std/exec.cch>` defines the arena-backed `CCCommand` builder:

```c
CCCommand cc_command_new(CCArena *arena, const char *program);
CCCommand cc_command(CCArena *arena, const char *program);
size_t cc_command_argc(const CCCommand *command);
const char *cc_command_get(const CCCommand *command, size_t index);
const char *cc_command_program(const CCCommand *command);

CCCommand *cc_command_arg(CCCommand *command, const char *arg);
CCCommand *cc_command_arg_slice(CCCommand *command, CCSlice arg);
CCCommand *cc_command_arg_i64(CCCommand *command, int64_t value);
CCCommand *cc_command_arg_i32(CCCommand *command, int value);
CCCommand *cc_command_arg_if(CCCommand *command, bool condition, const char *arg);
CCCommand *cc_command_arg_i64_if(CCCommand *command, bool condition, int64_t value);
CCCommand *cc_command_arg_i32_if(CCCommand *command, bool condition, int value);
CCCommand *cc_command_stdin_pipe(CCCommand *command);
CCCommand *cc_command_stdin_slice(CCCommand *command, CCSlice input);
CCCommand *cc_command_stdin(CCCommand *command, const char *input);
CCCommand *cc_command_stdout_capture(CCCommand *command);
CCCommand *cc_command_stderr_capture(CCCommand *command);
CCCommand *cc_command_stderr_to_stdout(CCCommand *command);
CCCommand *cc_command_inherit_stdio(CCCommand *command);
CCCommand *cc_command_cwd(CCCommand *command, const char *cwd);
CCCommand *cc_command_env(CCCommand *command, const char **env);

const char **cc_command_argv(CCCommand *command);
CCProcessConfig cc_command_process_config(CCCommand *command);
CCResult_CCProcess_CCIoError cc_command_spawn(CCCommand *command);
CCResult_CCProcessOutput_CCIoError cc_command_run(CCCommand *command, CCArena *arena);
CCResult_CCProcessOutput_CCIoError cc_command_output(CCCommand *command, CCArena *arena);
CCResult_CCProcessOutput_CCIoError cc_command_output_with_input(CCCommand *command, CCArena *arena, CCSlice input);
CCResult_int_CCIoError cc_command_status(CCCommand *command);
```

`CCCommand` UFCS maps method names to `cc_command_*` and passes the receiver by
address. `cc_command_status` disables stdout and stderr capture, waits for the
process, and returns `Ok(status.exit_code)` for any completed process.

## Futures and tasks

`<ccc/std/future.cch>` defines:

```c
typedef enum {
    CC_FUTURE_PENDING,
    CC_FUTURE_READY,
    CC_FUTURE_ERR,
    CC_FUTURE_CANCELLED,
    CC_FUTURE_TIMEOUT
} CCFutureStatus;

typedef struct {
    CCAsyncHandle handle;
    void *result;
} CCFuture;

typedef void (*cc_future_cb)(CCFutureStatus status, void *user, int err);
```

The API is:

```c
void cc_future_init(CCFuture *future);
void cc_future_free(CCFuture *future);
CCFutureStatus cc_future_poll(CCFuture *future, int *out_err);
CCFutureStatus cc_future_wait(CCFuture *future, int *out_err);
CCFutureStatus cc_future_wait_deadline(CCFuture *future, const CCDeadline *deadline, int *out_err);
int cc_future_wait_peek_err(CCFuture *future, int *out_err);
void cc_future_cancel(CCFuture *future);
int cc_future_on_complete(CCExec *executor, CCFuture *future, cc_future_cb callback, void *user);
```

`result` points to caller-owned result storage. `poll` does not block.
`on_complete` schedules its callback on the supplied executor.

`<ccc/std/task.cch>` operates on `CCTask`:

```c
typedef CCFutureStatus (*cc_task_poll_fn)(void *frame, intptr_t *out_value, int *out_err);

CCTask cc_run_blocking_task(CCClosure0 closure);
CCTask cc_task_yield_once(void);
CCTask cc_task_make_poll(cc_task_poll_fn poll, void *frame, void (*drop)(void *frame));
CCTask cc_task_make_poll_ex(cc_task_poll_fn poll, int (*wait)(void *frame), void *frame, void (*drop)(void *frame));
intptr_t cc_block_on_intptr(CCTask task);
CCFutureStatus cc_task_poll(CCTask *task, intptr_t *out_value, int *out_err);
void cc_task_free(CCTask *task);
void cc_task_cancel(CCTask *task);
int cc_block_all(int count, CCTask *tasks, intptr_t *results);
int cc_block_race(int count, CCTask *tasks, int *winner, intptr_t *result);
int cc_block_any(int count, CCTask *tasks, int *winner, intptr_t *result);
int cc_blocking_pool_stats(CCExecStats *out_exec, uint64_t *out_submit_failures);
```

`cc_block_on(T, task)` casts the `intptr_t` result to `T`.
`cc_block_all` waits for every task. `cc_block_race` reports the first
completion. `cc_block_any` reports the first successful completion and returns
`ECANCELED` when every task fails.

## Command-line parsing

`<ccc/std/cli.cch>` defines declarative argument specifications with
`CCArgSpec`, parsed entries with `CCParsedArg`, the `CCParsedArgs` collection,
and the `CCCliParse` result. The public operations are:

```c
const char *cc_cli_prog(int argc, char **argv);
CCSlice cc_cli_prog_slice(int argc, char **argv);
CCParsedArgs cc_parsed_args(CCArena *arena);
CCCliParse cc_cli_parse_args(int argc, char **argv, const CCArgSpec *specs, size_t spec_count, CCArena *arena);
void cc_print_usage(FILE *out, CCSlice prog, const CCArgSpec *specs, size_t spec_count);
void cc_cli_parse_print(const CCCliParse *parse, FILE *out);

size_t cc_parsed_args_len(const CCParsedArgs *args);
const CCParsedArg *cc_parsed_args_get(const CCParsedArgs *args, size_t index);
bool cc_parsed_args_has(const CCParsedArgs *args, const char *key);
size_t cc_parsed_args_count(const CCParsedArgs *args, const char *key);
const CCSlice *cc_parsed_args_value_at(const CCParsedArgs *args, const char *key, size_t index);
const CCSlice *cc_parsed_args_last_value(const CCParsedArgs *args, const char *key);
bool cc_parsed_args_find_value_at(const CCParsedArgs *args, const char *key, size_t index, CCSlice *out);
bool cc_parsed_args_find_last_value(const CCParsedArgs *args, const char *key, CCSlice *out);
```

`CCParsedArgs` UFCS maps `len`, `get`, `has`, `count`, `value_at`,
`last_value`, `find_value_at`, and `find_last_value` to `cc_parsed_args_*`.
`CCCliParse.print(out)` maps to `cc_cli_parse_print`.

## Networking

`<ccc/std/net.cch>` defines synchronous TCP, UDP, DNS, and address operations.

```c
typedef enum CCNetError {
    CC_NET_OK,
    CC_NET_CONNECTION_REFUSED,
    CC_NET_CONNECTION_RESET,
    CC_NET_CONNECTION_CLOSED,
    CC_NET_TIMED_OUT,
    CC_NET_HOST_UNREACHABLE,
    CC_NET_NETWORK_UNREACHABLE,
    CC_NET_ADDRESS_IN_USE,
    CC_NET_ADDRESS_NOT_AVAILABLE,
    CC_NET_INVALID_ADDRESS,
    CC_NET_DNS_FAILURE,
    CC_NET_TLS_HANDSHAKE_FAILED,
    CC_NET_TLS_CERTIFICATE_ERROR,
    CC_NET_OTHER
} CCNetError;

typedef struct CCSocket {
    int fd;
    uint8_t flags;
    void *watcher;
} CCSocket;

typedef union CCSocketSignal {
    long double __cc_align_ld;
    void *__cc_align_ptr;
    unsigned char __cc_storage[128];
} CCSocketSignal;

typedef struct CCListener {
    int fd;
    uint8_t flags;
    void *watcher;
} CCListener;

typedef struct CCUdpSocket {
    int fd;
    uint8_t flags;
} CCUdpSocket;

typedef struct CCUdpPacket {
    CCSlice data;
    CCSlice from_addr;
} CCUdpPacket;

typedef struct CCIpAddr {
    uint8_t family;
    union {
        uint8_t v4[4];
        uint8_t v6[16];
    } addr;
} CCIpAddr;
```

TCP and socket functions are:

```c
CCSocket cc_tcp_connect(const char *addr, size_t addr_len, CCNetError *out_err);
CCListener cc_tcp_listen(const char *addr, size_t addr_len, CCNetError *out_err);
CCSocket cc_listener_accept(CCListener *listener, CCNetError *out_err);
void cc_listener_close(CCListener *listener);

CCSlice cc_socket_read(CCSocket *socket, CCArena *arena, size_t max_bytes, CCNetError *out_err);
size_t cc_socket_read_into(CCSocket *socket, char *buf, size_t max_bytes, CCNetError *out_err);
size_t cc_socket_read_into_deadline(CCSocket *socket, char *buf, size_t max_bytes, CCNetError *out_err, const CCDeadline *deadline);
size_t cc_socket_try_read_into(CCSocket *socket, char *buf, size_t max_bytes, CCNetError *out_err, bool *out_would_block);
size_t cc_socket_write(CCSocket *socket, const char *data, size_t len, CCNetError *out_err);
size_t cc_socket_write_deadline(CCSocket *socket, const char *data, size_t len, CCNetError *out_err, const CCDeadline *deadline);
void cc_socket_shutdown(CCSocket *socket, CCShutdownMode mode, CCNetError *out_err);
void cc_socket_close(CCSocket *socket);
CCSlice cc_socket_peer_addr(CCSocket *socket, CCArena *arena, CCNetError *out_err);
CCSlice cc_socket_local_addr(CCSocket *socket, CCArena *arena, CCNetError *out_err);
```

`addr` is a length-delimited `host:port`, IPv4 `address:port`, or bracketed
IPv6 address. A socket read reports remote EOF as zero bytes with
`CC_NET_CONNECTION_CLOSED`. The deadline functions report
`CC_NET_TIMED_OUT` when the deadline expires. `try_read_into` reports
`EAGAIN` or `EWOULDBLOCK` by setting `out_would_block` while leaving the error
as `CC_NET_OK`.

`CCSocket` and `CCListener` UFCS map synchronous method names to
`cc_socket_*` and `cc_listener_*`. No networking `_async` C callees are
defined by this API.

Socket readiness signaling uses:

```c
void cc_socket_create_signal(CCSocket *socket, CCSocketSignal *out_signal);
void cc_socket_signal_init(CCSocketSignal *signal, CCSocket *socket);
void cc_socket_signal_free(CCSocketSignal *signal);
void cc_socket_signal_signal(CCSocketSignal *signal);
CCResult_bool_CCIoError cc_socket_signal_wait(CCSocketSignal *signal);
uint64_t cc_socket_signal_snapshot(CCSocketSignal *signal);
CCResult_bool_CCIoError cc_socket_signal_wait_since(CCSocketSignal *signal, uint64_t seen_epoch);
```

UDP functions are:

```c
CCUdpSocket cc_udp_bind(const char *addr, size_t addr_len, CCNetError *out_err);
size_t cc_udp_send_to(CCUdpSocket *socket, const char *data, size_t len, const char *addr, size_t addr_len, CCNetError *out_err);
CCUdpPacket cc_udp_recv_from(CCUdpSocket *socket, CCArena *arena, size_t max_bytes, CCNetError *out_err);
void cc_udp_close(CCUdpSocket *socket);
```

DNS and address functions are:

```c
CCSlice cc_dns_lookup(CCArena *arena, const char *hostname, size_t hostname_len, CCNetError *out_err);
CCSlice cc_ip_addr_to_string(CCIpAddr *addr, CCArena *arena);
CCIpAddr cc_ip_parse(const char *text, size_t len, CCNetError *out_err);
```

`cc_dns_lookup` returns an arena-backed contiguous sequence of `CCIpAddr`
values represented by `CCSlice`. `<ccc/std/dns.cch>` also declares
`cc_dns_lookup`; callers use this linked implementation. It additionally
declares the family selector and reverse lookup:

```c
typedef enum CCDnsFamily {
    CC_DNS_ANY = 0,
    CC_DNS_IPV4 = 4,
    CC_DNS_IPV6 = 6
} CCDnsFamily;

CCSlice cc_dns_lookup_family(CCArena *arena, const char *hostname, size_t hostname_len, CCDnsFamily family, CCNetError *out_err);
CCSlice cc_dns_reverse(CCArena *arena, const CCIpAddr *addr, CCNetError *out_err);
```

The shipped runtime does not define these two extension functions.

## HTTP

`<ccc/std/http.cch>` provides a synchronous libcurl-backed client and requires
linkage with `curl`.

```c
typedef enum CCHttpError {
    CC_HTTP_OK,
    CC_HTTP_NET_ERROR,
    CC_HTTP_INVALID_URL,
    CC_HTTP_TOO_MANY_REDIRECTS,
    CC_HTTP_INVALID_RESPONSE,
    CC_HTTP_TIMEOUT,
    CC_HTTP_HEADER_TOO_LARGE,
    CC_HTTP_BODY_TOO_LARGE
} CCHttpError;

typedef struct CCHttpErrorInfo {
    CCHttpError code;
    CCNetError net_error;
    CCSlice message;
} CCHttpErrorInfo;

typedef struct CCHttpResponse {
    uint16_t status;
    CCSlice status_text;
    CCSlice headers;
    CCSlice body;
    CCSlice url;
} CCHttpResponse;

typedef struct CCHttpRequest {
    CCSlice method;
    CCSlice url;
    CCSlice headers;
    CCSlice body;
} CCHttpRequest;

typedef struct CCHttpClientConfig {
    uint32_t timeout_ms;
    CCSlice user_agent;
    bool follow_redirects;
    uint8_t max_redirects;
    size_t max_response_size;
    bool verify_ssl;
} CCHttpClientConfig;

typedef struct CCHttpClient {
    CCHttpClientConfig config;
} CCHttpClient;
```

The API is:

```c
CCHttpClientConfig cc_http_client_config_default(void);
CCHttpResponse cc_http_get(CCArena *arena, const char *url, size_t url_len, CCHttpErrorInfo *out_err);
CCHttpResponse cc_http_post(CCArena *arena, const char *url, size_t url_len, const char *body, size_t body_len, CCHttpErrorInfo *out_err);
CCHttpClient cc_http_client_new(CCHttpClientConfig config);
CCHttpClient cc_http_client_default(void);
CCHttpResponse cc_http_client_get(CCHttpClient *client, CCArena *arena, const char *url, size_t url_len, CCHttpErrorInfo *out_err);
CCHttpResponse cc_http_client_post(CCHttpClient *client, CCArena *arena, const char *url, size_t url_len, const char *body, size_t body_len, CCHttpErrorInfo *out_err);
CCHttpResponse cc_http_client_request(CCHttpClient *client, CCArena *arena, CCHttpRequest request, CCHttpErrorInfo *out_err);
```

Response and error-info slices are arena-backed. The default configuration
uses a 30-second timeout, follows at most ten redirects, limits a response to
64 MiB, and verifies TLS certificates. HTTP does not define `_async` callees.

URL parsing is:

```c
typedef struct CCParsedUrl {
    CCSlice scheme;
    CCSlice host;
    uint16_t port;
    CCSlice path;
    CCSlice query;
    CCSlice fragment;
} CCParsedUrl;

CCParsedUrl cc_url_parse(const char *url, size_t url_len, CCHttpError *out_err);
```

The component slices point into the supplied URL buffer.

## TLS

`<ccc/std/tls.cch>` provides TLS client and server operations when the runtime
is built with BearSSL support.

```c
typedef struct CCTlsClientConfig {
    const char *ca_cert_path;
    size_t ca_cert_path_len;
    bool verify_hostname;
    const char *sni_hostname;
    size_t sni_hostname_len;
} CCTlsClientConfig;

typedef struct CCTlsServerConfig {
    const char *cert_path;
    size_t cert_path_len;
    const char *key_path;
    size_t key_path_len;
    const char *client_ca_path;
    size_t client_ca_path_len;
} CCTlsServerConfig;

typedef struct CCTlsInfo {
    CCSlice protocol_version;
    CCSlice cipher_suite;
    CCSlice peer_cert_subject;
    CCSlice sni_hostname;
} CCTlsInfo;

typedef struct CCTlsConn {
    void *ctx;
    void *iobuf;
    size_t iobuf_len;
    CCSocket underlying;
    CCArena *info_arena;
    uint8_t flags;
} CCTlsConn;
```

The connection API is:

```c
#define CC_TLS_IOBUF_SIZE (16384 + 16384 + 325)

CCTlsClientConfig cc_tls_client_config_default(void);
CCTlsConn cc_tls_connect(CCSocket socket, CCTlsClientConfig config, void *iobuf, size_t iobuf_len, CCArena *info_arena, CCNetError *out_err);
CCTlsConn cc_tls_connect_addr(const char *addr, size_t addr_len, CCTlsClientConfig config, CCArena *conn_arena, CCNetError *out_err);
CCTlsConn cc_tls_accept(CCSocket socket, CCTlsServerConfig config, void *iobuf, size_t iobuf_len, CCArena *info_arena, CCNetError *out_err);
CCSlice cc_tls_read(CCTlsConn *conn, CCArena *arena, size_t max_bytes, CCNetError *out_err);
size_t cc_tls_write(CCTlsConn *conn, const char *data, size_t len, CCNetError *out_err);
void cc_tls_shutdown(CCTlsConn *conn, CCShutdownMode mode, CCNetError *out_err);
void cc_tls_close(CCTlsConn *conn);
const CCTlsInfo *cc_tls_info(const CCTlsConn *conn);
```

The caller-provided I/O buffer remains valid for the connection lifetime and
has at least `CC_TLS_IOBUF_SIZE` bytes. `cc_tls_connect_addr` allocates this
buffer from `conn_arena`. TLS does not define `_async` callees.
`cc_tls_info` is a stub and always returns null; no session-info
slices are available through it.

Certificate-loading entry points are:

```c
CCTlsCertChain *cc_tls_load_cert_chain(CCArena *arena, const char *path, size_t path_len, CCNetError *out_err);
CCTlsPrivateKey *cc_tls_load_private_key(CCArena *arena, const char *path, size_t path_len, CCNetError *out_err);
CCTlsTrustAnchors *cc_tls_load_trust_anchors(CCArena *arena, const char *path, size_t path_len, CCNetError *out_err);
```

These certificate-loading functions are stubs. Each returns null and writes
`CC_NET_OTHER` to `out_err`.

## Portable atomics

`<ccc/cc_atomic.cch>` defines:

```c
typedef /* atomic integer */ cc_atomic_int;
typedef /* atomic unsigned integer */ cc_atomic_uint;
typedef /* atomic size_t */ cc_atomic_size;
typedef /* atomic int64_t */ cc_atomic_i64;
typedef /* atomic uint64_t */ cc_atomic_u64;
typedef /* atomic intptr_t */ cc_atomic_intptr;

old_value = cc_atomic_fetch_add(ptr, value);
old_value = cc_atomic_fetch_sub(ptr, value);
value = cc_atomic_load(ptr);
cc_atomic_store(ptr, value);
bool exchanged = cc_atomic_cas(ptr, expected_ptr, desired);
```

On a failed compare-and-swap, the C11 and fallback implementations update
`*expected_ptr` with the observed value. The GCC/Clang `__sync` implementation
returns false without updating `*expected_ptr`.

`CC_ATOMIC_HAVE_REAL_ATOMICS` is `1` when the header selects C11 atomics or
GCC/Clang atomic builtins. It is `0` for TinyCC and unknown-compiler fallback
implementations. Operations in a `0` configuration are not thread-safe.
