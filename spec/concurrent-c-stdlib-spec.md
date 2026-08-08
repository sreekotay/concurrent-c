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

Portable atomics are provided separately by `<ccc/cc_atomic.cch>`, Python
interop by `<ccc/script/py.cch>`, and JavaScript interop by
`<ccc/script/js.cch>` (draft — guest surface implemented).

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
Vec::[T] vec_new::[T](CCArena *arena);
Map::[K, V] map_new::[K, V](CCArena *arena);
ArrayMap::[K, V] array_map_new::[K, V](CCArena *arena);
ArrayMap::[K, V] array_map_new_count::[K, V](CCArena *arena, size_t count);
```

`Vec::[T]` denotes the generated C family `CCVec_<T-mangling>`; the
CC-prefixed spellings (`CCVec::[T]`, `cc_vec_new::[T]`) name the same
instances and remain accepted as the instance layer. The spelling grid is
general: for any registered factory family,
`<snake(Family)>_<member>::[args](...)` calls
`<Family>_<mangled args>_<member>(...)` (language spec §12.1).
`vec_new::[T](arena)` calls:

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

Key support is a declared convention. A key type `K` is installed when both

```c
size_t cc_map_key_hash_<K-mangling>(K key);
int    cc_map_key_eq_<K-mangling>(K a, K b);
```

are visible — declared in the translation unit or an included header — and a
declared pair outranks the built-in table (`int`, 64-bit integers,
`CCSliceHdr`, `CCSlicePacked`, the slice family). Forward prototypes are
emitted above the spliced container declaration with the definitions' own
linkage, so the pair may be defined anywhere in the unit. A key type with no
declared pair and no built-in entry is ill-formed; the diagnostic names the
two functions to declare.

For a generated vector or map value, UFCS selects the corresponding generated
family function and passes the receiver by address. For public struct families
with `cc_<family>_<method>` functions, UFCS selects that prefixed function and
passes the receiver in the form required by its C signature. The exact
family-specific mappings are listed below.

## Slices

The language specification defines the `CCSlice`, `CCSliceUnique`,
`CCSliceShared`, and `CCSliceHdr` ABI and ownership rules. The standard library
also defines `CCSlicePacked` (pointer-sized held slice) and uses `CCSliceArray`,
a pointer-length sequence of `CCSlice` values.

Construction and lifetime functions are:

```c
CCSliceHdr cc_slice_hdr(CCSlice *s);
CCSlice cc_slice_empty(void);
CCSlice cc_slice_from_buffer(void *ptr, size_t len);
CCSlice cc_slice_from_static(void *ptr, size_t len);
CCSlice cc_slice_hdr_slice(CCSliceHdr *sh);
CCSlice cc_slice_from_parts(void *ptr, size_t len, uint64_t id, size_t available_len);
CCSlice char_to_slice(const char *cstr);
CCSlice const_char_to_slice(const char *cstr); /* alias; UFCS for const char* */
CCSlice unsigned_char_to_slice(const unsigned char *cstr);
CCSlice signed_char_to_slice(const signed char *cstr);
CCSlice const_unsigned_char_to_slice(const unsigned char *cstr);
CCSlice const_signed_char_to_slice(const signed char *cstr);
CCSlice cc_slice_from_cstr(const char *cstr);  /* alias of char_to_slice */
CCSliceUnique cc_adopt(void *ptr, size_t nbytes, CCSliceDeleter deleter);
void cc_slice_destroy(CCSlice *s);
```

`cc_slice_from_buffer` and `cc_slice_hdr_slice` produce untracked slices.
`char_to_slice` / `const_char_to_slice` (and the signedness variants) and
`cc_slice_from_static` produce canonical static slices. `cc_slice_from_cstr`
is an alias of `char_to_slice`. In Concurrent-C, `p->to_slice()` is UFCS:
`char*` → `char_to_slice`, `const char*` → `const_char_to_slice` (generic
cv-qualifier peel in the UFCS lowerer; signedness variants likewise).
A string literal whose destination is by-value `CCSlice`, `char[:]`,
`char[:0]`, `CCSliceShared`, or `CCSliceUnique` — call argument or
local/field initializer — lowers to `CC_SLICE_LIT(lit)` (sizeof-static;
`len` excludes NUL). Prefer `char[:0] s = "hi"` for sentinel borrows.
Pointer parameters and non-literal `char[N]` / `char*` variables are not
coerced — use `p->to_slice()` / `char_to_slice(p)` / `cc_slice_cstr(p)`.
Host-included C headers may spell the same ABI as `CCSlice`; Concurrent-C
path and CLI string surfaces are `char[:0]` (NUL-terminated borrow).
Ordinary slice-family sites deny field stores; see `draft_facets.md` §7b.
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

### Arena typed allocation

`allocT` is a type-formal member on arenas: `arena.allocT()` allocates
one `T`, `arena.allocT(n)` allocates `n` — the element type comes from
the declared pointer destination (`T* p = arena.allocT(n)`) or
explicitly via `arena.allocT::[T](n)`. Both lower to
`cc_arena_alloc_T` / `cc_arena_alloc_T_count`. An optional `@destroy`
on the declaration releases the allocation through its owning arena at
scope exit (`cc_arena_release`); where the slab cannot reclaim
individually, the release is a semantic no-op and the declaration
still states the allocation's scope.

### Arena-backed slice operations

`<ccc/std/slice.cch>` provides:

```c
CCResult_CCSlice_CCError cc_slice_clone_into(CCSlice *src, CCArena *arena);
CCResult_CCSliceHdr_CCError cc_slice_hdr_clone_into(CCSliceHdr *src, CCArena *arena);

/* Stabilize `*s` in `arena` (mutate in place). */
bool !>(CCError) cc_slice_materialize_in(CCSlice *s, CCArena *arena);

/* UTF-8 bytes → Unicode scalar values in `arena`. */
uint32_t[:] !>(CCError) cc_slice_utf8_codepoints(const CCSlice *s, CCArena *arena);

/* Checked index — same Result/error in all builds (no debug/release split). */
char !>(CCError) cc_slice_get_checked(CCSlice *s, size_t idx);
char !>(CCError) cc_slice_at(CCSlice *s, size_t idx);          /* alias of get_checked */
bool !>(CCError) cc_slice_set(CCSlice *s, size_t idx, char c);
```

`materialize_in` is a no-op when the slice is empty, canonical/static, or
already from `arena`'s provenance epoch; otherwise it clones into `arena` and
replaces `*s`. It does not free the prior view. UFCS: `s.materialize_in(arena)`.

`utf8_codepoints` decodes the receiver as UTF-8 into an arena-backed
`uint32_t[:]` (element count = codepoint count). Empty input yields an empty
typed slice without allocating. An all-ASCII prefix (and an all-ASCII slice)
takes a widen-only fast path. Truncated sequences, bad continuations, overlong
encodings, UTF-16 surrogates, and values above U+10FFFF return `CC_ERR_PARSE`;
missing arena or a null non-empty pointer returns `CC_ERR_INVALID_ARG`;
allocation failure returns `CC_ERR_OUT_OF_MEMORY`. UFCS:
`s.utf8_codepoints(arena)`.

Out-of-bounds or null-pointer index ops return `CC_ERR_INVALID_ARG`. Soft-zero
`at` is gone. Raw `s.ptr[i]` / `((char*)s.ptr)[i]` remains an untracked Gap
outside this surface.

### Packed slice handles

`<ccc/std/slice_packed.cch>` (prelude) defines `CCSlicePacked`: a pointer-sized
**held** slice for dense keys and interned payloads — the packed twin of
`CCSliceHdr`'s fat `{ptr,len}` hold. Small payloads stay inline in the word;
larger ones point at an arena `[u32 len][bytes…]` block (SDS-style). Inline
forms need no free; heap forms release with `cc_slice_packed_release` when the
arena supports individual reclaim (heap-overflow). Ephemeral borrows remain
`char[:]` / `CCSlice`. `CCString` remains the growable owner.

```c
typedef struct CCSlicePacked {
    uintptr_t w;
} CCSlicePacked;  /* 8 bytes on 64-bit little-endian hosts */

CCSlicePacked cc_slice_packed_empty(void);
CCResult_CCSlicePacked_CCError cc_slice_to_packed(CCSlice *src, CCArena *arena); /* UFCS: src.to_packed(arena) */
CCSlicePacked cc_slice_packed_borrow(CCSlicePackedView *view);           /* probe only */
CCSlicePacked cc_slice_packed_borrow_slice(CCSlicePackedView *view, CCSlice src);
void cc_slice_packed_release(CCArena *arena, CCSlicePacked *r); /* heap only; clears *r */
uint32_t cc_slice_packed_len(const CCSlicePacked *r);
CCSlice cc_slice_packed_as_slice(const CCSlicePacked *r);  /* inline/view: storage must stay live */
int cc_slice_packed_is_inline(const CCSlicePacked *r);
int cc_slice_packed_is_view(const CCSlicePacked *r);
int cc_slice_packed_is_empty(const CCSlicePacked *r);
int cc_slice_packed_is_durable(const CCSlicePacked *r);
size_t cc_map_hash_slice_packed(CCSlicePacked r);
int cc_map_eq_slice_packed(CCSlicePacked a, CCSlicePacked b);
```

`Map` / `ArrayMap` sugar accepts `CCSlicePacked` keys via those hash/eq helpers.
Borrowed views are for probes only — do not insert them.

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

Dense held keys use `CCSlicePacked` (see Packed slice handles above), not
`CCString`.

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
storage. Heap growth multiplies capacity by 1.6 (`(cap * 8) / 5`). When growth
uses a different arena than the string's current heap owner, that is an arena
swap: the buffer moves to the new arena and the header owner updates. A growth
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
or `false`.

Floating-point formatting (`float_to_str`, `double_to_str`,
`cc_string_push_f32`, `cc_string_push_f64`, `cc_string_push_float`, and the
`float` / `double` arms of `cc_string_from`, push/append, and `@string`
interpolation) is locale-independent and uses the decimal point character
`.`. For a finite value it emits a shortest correctly rounded decimal
representation that round-trips under a correctly rounding decimal-to-binary
parse of the same IEEE width (`binary32` for `float`, `binary64` for
`double`):

- Let \(e\) be the decimal exponent of the value in normalized scientific
  form \(d \times 10^{e}\) with \(1 \le d < 10\). Fixed notation is used when
  \(-4 \le e \le 15\); otherwise scientific notation is used.
- Scientific notation uses a lowercase `e`, an explicit sign on the exponent,
  and at least two exponent digits (for example `1.2e+23`, `1e-05`, `1e+16`).
- Trailing zeros in the fractional significand are omitted. When fixed
  notation would otherwise contain no decimal point (the value is an integer),
  a trailing `.0` is appended, so every finite result contains either `.` or
  `e` (for example `0.0`, `1000.0`, `-1.0`).
- Negative zero formats as `-0.0`.

Non-finite values format as the lowercase literals `nan`, `inf`, and `-inf`.

## File and buffered I/O

`<ccc/std/io.cch>` defines `CCFile` and `CCBufWriter`. Buffered reads live in
`<ccc/std/bufio.cch>` as the generic `BufReader::[Src]`.

```c
typedef struct {
    FILE *handle;
} CCFile;

typedef struct {
    CCFile *file;
    char *buf;
    size_t cap;
    size_t len;
} CCBufWriter;
```

The file API is:

```c
int cc_file_open(CCFile *file, char[:0] path, const char *mode);
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

`path` is a NUL-terminated borrow (`char[:0]` / `CCSlice` ABI). String
literals coerce at the call site; `mode` remains `const char *` and is not
coerced. `cc_file_open` returns zero on success and `-1` on failure. The
failure's `errno` remains available separately.
`cc_file_close` ignores close errors. Value-returning reads return an empty
slice at EOF. The `_into` read forms return `ok(true)` when they write an
output value and `ok(false)` at EOF. `cc_file_read_line` includes the newline
when one is read. `cc_file_write` and `cc_file_write_buf` return the number of
bytes written. `cc_file_size` returns `Ok(0)` for a non-seekable stream and
does not change the current position.

`CCFile` UFCS maps file methods to `cc_file_*` and passes `&file`.

Caller-buffer Duplex fill is `read_buf_into`: `bool !>(CCIoError)` with EOF
model B (`Ok(true)` / `Ok(false)` / `Err`). `CCFile` exposes it as
`cc_file_read_buf_into`. `CCSocket` exposes the same shape as
`cc_socket_read_buf_into` (alias of `cc_socket_read_into`).

Standard stream writes are:

```c
CCResult_size_t_CCIoError cc_std_out_write(CCSlice data);
CCResult_size_t_CCIoError cc_std_err_write(CCSlice data);
CCResult_size_t_CCIoError cc_std_out_write_auto(value);
CCResult_size_t_CCIoError cc_std_err_write_auto(value);
```

The automatic forms accept a slice, C string, or `CCString` value or pointer.
These are byte writers for library code. Do not treat them as the script
console print surface (see Script console print below).

### `BufReader::[Src]`

`BufReader` is a `CC_GENERIC_FACTORY` monomorph over any Duplex-compatible
`Src` that provides `read_buf_into`. Each instance holds `Src *`, a caller-
or arena-owned byte buffer, and cursor state.

```c
BufReader::[CCSocket] br;
br.init(&sock, stack_buf, sizeof(stack_buf));
/* or */ br.init_arena(&sock, &arena, cap);

size_t pending = br.buffered();
bool got = br.fill() !>;                 /* Ok(false) at EOF */
CCSlice line = br.read_line() !>;        /* view into br's buffer; strips \r */
CCSlice bulk = br.read_exact(n) !>;      /* view; Err if short after EOF */
CCSlice owned = br.read_line_dup(&arena) !>;
```

| Method | Role |
|---|---|
| `init` / `init_arena` | bind source + buffer storage |
| `buffered` | unread bytes (`end - pos`) |
| `fill` | compact unread; call `src->read_buf_into` on the free tail |
| `read_line` | view through `\n` (strip trailing `\r`); `Ok(empty)` at clean EOF; `Err` if bytes remain without a newline at EOF |
| `read_exact` | view of `n` bytes; `Err` on short read after EOF |
| `read_line_dup` | arena-owned copy of one `read_line` |

Views from `read_line` / `read_exact` are invalidated by a later compacting
`fill`. File-only buffered writes remain:

```c
int cc_buf_writer_init(CCBufWriter *writer, CCFile *file, CCArena *arena, size_t cap);
CCResult_size_t_CCIoError cc_buf_writer_flush(CCBufWriter *writer);
CCResult_size_t_CCIoError cc_buf_writer_write(CCBufWriter *writer, CCSlice data);
```

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
int cc_file_open_async(CCExec *ex, CCFile *file, char[:0] path, const char *mode, CCAsyncHandle *handle);
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
bool cc_path_is_abs(char[:0] path);
char[:0] cc_path_join(CCArena *arena, char[:0] a, char[:0] b);
char[:0] cc_path_dirname(CCArena *arena, char[:0] path);
char[:0] cc_path_basename(CCArena *arena, char[:0] path);
```

Path arguments are NUL-terminated borrows (`char[:0]`). The returned `join`,
`dirname`, and `basename` slices are NUL-terminated and arena-backed. These
helpers implement POSIX path syntax only: `cc_path_sep()` is `/`, and
`cc_path_is_abs` recognizes only a leading `/`.

## Script console print

`<ccc/script/stdio.cch>` (script prelude, not `<ccc/std/…>`) defines
`CCStdio` for arena-backed stdin reads and line-oriented console writes.
When script `io` is in scope, preferred examples are handle-first. Data-first
UFCS and naked aliases remain valid (UFCS either way on the chosen receiver):

```c
io.println(path) !>;                  /* preferred when io is in scope */
io.eprintln(line) !>;
io.println(@string(`n=${n}`, &a)) !>;

path.println() !>;                    /* also OK: UFCS on data */
"literal".println() !>;               /* lit/cstr → CCSlice → cc_slice_* */
cstr_ptr.println() !>;
println(path) !>;                     /* naked alias → cc_println */
path.fprintln(STDERR_FILENO) !>;
```

When the *data* is the UFCS receiver, `CCSlice` / `CCString` call
`cc_slice_*` / `cc_string_*`; C string and string-literal receivers coerce to
a `CCSlice` temporary then `cc_slice_*`. There is no `cc_char_*` UFCS print
family (`cc_char_*` / `_Generic` arms are free-sugar / lowered-C only).

The prefix spelling aliases the declared `cc_println` family (core spec,
naked-calls rule) — with any translation-unit binding of the name taking the
call unchanged.

Guidelines:

- Prefer `io.println(data)` / `io.eprintln(data)` when `io` is in scope.
- Data-first and naked forms remain valid; choose `println` / `eprintln` /
  `fprintln` for the sink.
- Prefer `io.println(@string(…))` (or data-first on the temp) for formatted
  output; do not wrap temps in `cc_println` in new script source.
- `cc_println` / `cc_eprintln` are lowered-C sugar (driver-injected default
  `@errhandler`, `-E` desugar).
- Inside a custom `@errhandler` body, discard with a bound receiver
  (`CCString msg = …; (void)msg.eprintln();`). Do not use `!>` there.
- `<ccc/std/io.cch>` `cc_std_out_write` / `cc_std_err_write` remain the
  byte-writer API for non-script library code.

Returns are `CCResult_size_t_CCError`. Short names `println` / `eprintln` are
not macros (a function-like macro would expand in member position and destroy
the postfix spelling): the prefix spelling aliases the declared `cc_println`
family at call position only.

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

Plain-C consumers invoke `CC_MAP_DECL_ARENA(K, V, Name, HASH_FN, EQ_FN)`
directly with a built-in hash/eq pair (`cc_map_hash_i32`/`cc_map_eq_i32`,
`cc_map_hash_u64`/`cc_map_eq_u64`, `cc_map_hash_slice`/`cc_map_eq_slice`)
or a user-declared pair.

### Array maps

`<ccc/std/array_map.cch>` defines arena-backed array-map families with a pow2
`uint32_t` probe index and a dense `(key, value)` row store. Occupied probe
slots store an 8-bit hash fragment in the high byte and a 24-bit dense
index+1 in the low bits; lookup rejects fragment mismatches before key
equality. Live dense length is at most `2^24 - 2` so an occupied slot never
collides with the tomb sentinel:

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
V *Name_at_ptr(Name *map, size_t i);   /* dense row i in [0, len) */
K *Name_key_ptr(Name *map, size_t i);  /* dense row i in [0, len) */
K *Name_find_key_ptr(Name *map, K key); /* stored key equal to key, or null */
bool Name_remove(Name *map, K key);
bool Name_del(Name *map, K key);
size_t Name_len(const Name *map);
size_t Name_cap(const Name *map); /* probe-table bucket count */
size_t Name_live_bytes(const Name *map);
void Name_clear(Name *map);
```

`init` / `init_count` return null on allocation failure. `insert` returns zero
on success and `-1` on failure. `get` / `get_ptr` return a pointer into the
dense store or null. `at_ptr` / `key_ptr` index the dense row store (live
indices `[0, len)`); out of range returns null. Prefer them (or `get_ptr`)
over by-value `CC_ARRAY_MAP_FOREACH` when releasing or mutating owned values.
`find_key_ptr` returns a pointer to the stored key equal to `key` (for owned-key
reclaim before `del`). `cap` is the probe-table capacity (power of two), not the
dense row capacity. `CC_ARRAY_MAP_FOREACH` iterates dense rows in insertion order
(swap-remove on delete may reorder).

Sugar `ArrayMap::[K, V]` / `array_map_new::[K, V]` /
`array_map_new_count::[K, V]` lowers to the `ArrayMap_<K>_<V>` family with the
same key hash/eq selection as `Map::[K, V]` for built-in key kinds (`int`,
`uint64_t`, `CCSlice`, `CCSliceHdr`, `CCSlicePacked`).

Array-map UFCS maps `insert`, `get`, `get_ptr`, `at_ptr`, `key_ptr`,
`find_key_ptr`, `remove`, `del`, `len`, `cap`, `live_bytes`, `clear`, and
`destroy` to the generated family.

### Shard maps (`CCShardMap`)

`<ccc/std/shard_map.cch>` provides an arena-owned string→string map cell.
It owns a `CCArena` and an `ArrayMap::[CCSlicePacked, CCString]`. Lookup
keys may be ordinary `CCSlice` values (borrow-packed for the probe); inserts
copy the key into a durable packed slice and the value into a `CCString` in
the map’s arena.

```c
CCShardMap m;
m.init(64);                    // cc_shard_map_init(&m, 64)
m.put(key, val);               // bool; false on OOM
CCString* v = m.get(key);      // NULL if absent
m.delete(key);                 // bool; true if an entry was removed
size_t n = m.len();
m.destroy();
```

**Rule (ownership):** Keys and values live in `m.arena`. `get` returns a
pointer into the dense store (valid until the entry is overwritten, deleted,
or the map is destroyed). `put` replaces an existing value in place (old
value released) or inserts a new durable key+value.

**Rule (UFCS):** Methods follow the stdlib `cc_shard_map_<method>` convention
(`init`, `destroy`, `get`, `put`, `delete`, `len`).

Pair with `CCShardMask` / `CCExclusive` when routing many `CCShardMap` cells
by key hash. Prefer raw `ArrayMap` when the value type is not `CCString` or
when the caller already owns the arena.

### Static maps

`<ccc/std/static_map.cch>` provides a comptime perfect-hash map. The
user-facing call takes a typed entry array:

```c
enum {
    CC_STATIC_MAP_CASE_SENSITIVE = 0,
    CC_STATIC_MAP_ASCII_CI = 1,
};

typedef struct Entry {
    const char *key;
    ValueType value; /* flat POD integers/enums only */
} Entry;

@comptime {
    Entry entries[] = {
        { "GET", { /* fields */ } },
    };
    static_map("name", entries, CC_STATIC_MAP_ASCII_CI);
}
```

Each entry type has a `const char *key` field and a real typed `value`
field. Values are ordinary compound literals — there is no stringified
initializer. The compiler rewrites the three-argument call into an
internal form that carries the value type name and layout (`sizeof` /
address offsets). At the call site the function searches for a
collision-free FNV-1a seed into a power-of-two slot table and emits
keys, values, slots, and:

```c
static const ValueType *name_get(CCSlice key);
```

Lookup is `hash(key) -> slot -> verify -> &value`, with exact or ASCII
case-insensitive verification according to `flags`. A miss returns null.
Invalid arguments, non-POD value fields, unreproducible value layout,
duplicate keys (under the selected match policy), keys requiring
C-string escaping, and failure to construct a perfect hash are
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
    char[:0] name;
    CCDirEntryType type;
} CCDirEntry;

typedef struct CCDirIter CCDirIter;

CCResult_CCDirIterptr_CCIoError cc_dir_open(CCArena *arena, char[:0] path);
CCResult_CCDirEntry_CCIoError cc_dir_next(CCDirIter *iter, CCArena *arena);
void cc_dir_close(CCDirIter *iter);

bool cc_path_exists(char[:0] path);
bool cc_path_is_dir(char[:0] path);
bool cc_path_is_file(char[:0] path);
CCResult_bool_CCIoError cc_dir_create(char[:0] path);
CCResult_bool_CCIoError cc_dir_create_all(char[:0] path);
CCResult_bool_CCIoError cc_dir_remove(char[:0] path);
CCResult_bool_CCIoError cc_file_remove(char[:0] path);
char[:0] cc_dir_cwd(CCArena *arena);
CCResult_bool_CCIoError cc_dir_chdir(char[:0] path);

CCSliceArray!>(CCIoError) cc_glob(char[:0] pattern, CCArena *arena);
bool cc_glob_match(char[:0] pattern, char[:0] name);
```

Path arguments are NUL-terminated borrows (`char[:0]`). After the final entry,
`cc_dir_next` returns `Err(cc_io_error_os(CC_IO_OTHER, 0))` (EOF sentinel:
`base.kind == CC_ERR_IO` and `os_code == 0`).
Entry names and glob paths are arena-backed `char[:0]`. `cc_dir_create` does
not create parents; `cc_dir_create_all` does. Globbing supports `*`, `?`, and
recursive `**`; `cc_glob_match` matches a single name with `*` and `?`.
`cc_glob` takes the pattern first and the arena last; it returns
`CCSliceArray!>(CCIoError)` of NUL-terminated path borrows (same aggregate
used by slice split helpers). Success with no matches is an empty array;
I/O and allocation failures are errors (not empty). UFCS:
`files.len()`, `files.get(i)` → `char[:0]`.

The accessor functions are:

```c
const char *cc_dir_entry_name_str(const CCDirEntry *entry);
bool cc_dir_entry_is_dir(const CCDirEntry *entry);
bool cc_dir_entry_is_file(const CCDirEntry *entry);
bool cc_dir_entry_is_symlink(const CCDirEntry *entry);
size_t cc_slice_array_len(const CCSliceArray *arr);
char[:0] cc_slice_array_get(const CCSliceArray *arr, size_t index);
```

UFCS on `CCDirEntry` and `CCSliceArray` maps these accessors to their
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
CCCommand cc_command_new(CCArena *arena, char[:0] program);
CCCommand cc_command(CCArena *arena, char[:0] program);
size_t cc_command_argc(const CCCommand *command);
const char *cc_command_get(const CCCommand *command, size_t index);
const char *cc_command_program(const CCCommand *command);

CCCommand *cc_command_arg(CCCommand *command, const char *arg);
CCCommand *cc_command_arg_slice(CCCommand *command, char[:0] arg);
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
CCCommand *cc_command_cwd(CCCommand *command, char[:0] cwd);
CCCommand *cc_command_env(CCCommand *command, const char **env);

const char **cc_command_argv(CCCommand *command);
CCProcessConfig cc_command_process_config(CCCommand *command);
CCResult_CCProcess_CCIoError cc_command_spawn(CCCommand *command);
CCResult_CCProcessOutput_CCIoError cc_command_run(CCCommand *command, CCArena *arena);
CCResult_CCProcessOutput_CCIoError cc_command_output(CCCommand *command, CCArena *arena);
CCResult_CCProcessOutput_CCIoError cc_command_output_with_input(CCCommand *command, CCArena *arena, CCSlice input);
CCResult_int_CCIoError cc_command_status(CCCommand *command);
```

`program`, `cwd`, and `arg_slice` take NUL-terminated path/token borrows
(`char[:0]`). String literals coerce at those by-value slice parameters;
`cc_command_arg` / `cc_command_stdin` keep `const char *` faces.
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

`block_on` is also a type-formal member on task values: `T v =
task.block_on();` takes `T` from the destination, and
`task.block_on::[T]()` spells it explicitly where no destination is
visible.

## Command-line parsing

Declare options with `@grammar(cli)` (see `spec/cc_serdes.md`). The compiler
emits a typed options struct plus `Name_parse_args` / `Name_prepare` /
`Name_print_usage`. Call sites use the stable macros `cc_parse_args`,
`cc_prepare_args`, and `cc_print_usage`. The same faces and
`<ccc/std/cli.cch>` runtime apply to `.ccs` and `.shcc`:

```c
@grammar(cli) Opts {~~~~
    help: flag -h, --help desc "Show help"
    jobs: opt i64 -j, --jobs attach as N default 4 desc "Worker count"
    file: rest string desc "Inputs"
~~~~}

Opts opts = {0};
bool go = cc_prepare_args(Opts, argc, argv, &arena, &opts, stderr) !>;
if (!go) return 0; /* help flag named `help` → Ok(false); usage printed */
```

`opt string` / `rest string` fields are `char[:0]` (NUL-terminated borrows of
argv or of a `default "..."` literal). Defaults apply before the argv overlay;
`_present` stays false until the user sets the option. `cc_prepare_args` is a
`bool !>(CCError)` face: `Ok(true)` proceed, `Ok(false)` help (usage printed
for a `flag` named `help`), `Err` bad argv (usage printed). `cc_parse_args`
fills only (defaults, then argv). `cc_print_usage` prints the generated usage.

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

TCP listen, accept, and connect return Result (`T!>(CCNetError)`, lowered as
`CCResult_*_CCNetError`). On error the handle has `fd == -1`.

```c
CCResult_CCSocket_CCNetError cc_tcp_connect(const char *addr, size_t addr_len);
CCResult_CCListener_CCNetError cc_tcp_listen(CCSlice addr);
CCResult_CCSocket_CCNetError cc_listener_accept(CCListener *listener);
void cc_listener_close(CCListener *listener);

CCIoError cc_net_to_io_error(CCNetError err);

CCResult_bool_CCIoError cc_socket_read(CCSocket *socket, CCArena *arena, size_t max_bytes, CCSlice *out);
CCResult_bool_CCIoError cc_socket_read_into(CCSocket *socket, char *buf, size_t max_bytes, size_t *out);
CCResult_bool_CCIoError cc_socket_read_into_deadline(CCSocket *socket, char *buf, size_t max_bytes, size_t *out, const CCDeadline *deadline);
CCResult_bool_CCIoError cc_socket_try_read_into(CCSocket *socket, char *buf, size_t max_bytes, size_t *out);
CCResult_size_t_CCIoError cc_socket_write(CCSocket *socket, const char *data, size_t len);
CCResult_size_t_CCIoError cc_socket_write_deadline(CCSocket *socket, const char *data, size_t len, const CCDeadline *deadline);
void cc_socket_shutdown(CCSocket *socket, CCShutdownMode mode, CCNetError *out_err);
void cc_socket_close(CCSocket *socket);
CCSlice cc_socket_peer_addr(CCSocket *socket, CCArena *arena, CCNetError *out_err);
CCSlice cc_socket_local_addr(CCSocket *socket, CCArena *arena, CCNetError *out_err);
```

Listen `addr` is a NUL-terminated borrow (`char[:0]` / `CCSlice`), same shape
as `cc_file_open`'s path. Connect still takes a length-delimited
`host:port` / IPv4 `address:port` / bracketed IPv6. Idiomatic use is unwrap
sugar on the greppable `cc_*` names:

```c
CCListener ln = cc_tcp_listen(addr) !>;
CCSocket sock = cc_listener_accept(&ln) !>;
CCSocket client = cc_tcp_connect(addr, len) !>;
```

`cc_net_to_io_error` maps connection closed/reset to `CC_IO_CONNECTION_CLOSED`,
timeout to `CC_IO_BUSY`, and other net errors to `CC_IO_OTHER`.

Socket byte read and write return `CCIoError` Results (same domain as files and
channels). Blocking and deadline reads use EOF model B:

- `Ok(true)` — bytes available; payload is written to the out-parameter
- `Ok(false)` — clean peer close (`recv` returned 0 / FIN); not an error
- `Err(e)` — RST, timeout, and other failures

Deadline expiry is `Err` with `CC_IO_BUSY`. Non-blocking `cc_socket_try_read_into`
is three-way: `Ok(true)` / `Ok(false)` as above, and would-block (`EAGAIN` /
`EWOULDBLOCK`) as `Err` with `CC_IO_BUSY` — never `Ok(false)`. Writes return
`size_t!>(CCIoError)` with the number of bytes written (may be short); there is
no EOF story for writes.

```c
while (cc_io_avail(cc_socket_read(sock, arena, n, &data))) { /* ... */ }
size_t n = 0;
bool got = sock.read_into(buf, cap, &n) !>;
```

`CCSocket` and `CCListener` UFCS map synchronous method names to
`cc_socket_*` and `cc_listener_*` (including Result-returning `accept` and
`read_into`). No networking `_async` C callees are defined by this API.

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
linkage with `curl`. Request APIs are Result-primary: the Ok arm is
`CCHttpResponse` and the Err arm is `CCHttpErrorInfo`.

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
CCHttpResponse!>(CCHttpErrorInfo) cc_http_get(CCArena *arena, const char *url, size_t url_len);
CCHttpResponse!>(CCHttpErrorInfo) cc_http_post(CCArena *arena, const char *url, size_t url_len, const char *body, size_t body_len);
CCHttpClient cc_http_client_new(CCHttpClientConfig config);
CCHttpClient cc_http_client_default(void);
CCHttpResponse!>(CCHttpErrorInfo) cc_http_client_get(CCHttpClient *client, CCArena *arena, const char *url, size_t url_len);
CCHttpResponse!>(CCHttpErrorInfo) cc_http_client_post(CCHttpClient *client, CCArena *arena, const char *url, size_t url_len, const char *body, size_t body_len);
CCHttpResponse!>(CCHttpErrorInfo) cc_http_client_request(CCHttpClient *client, CCArena *arena, CCHttpRequest request);
```

Response body, headers, final URL, and any arena-backed error `message` use
the caller-supplied request arena and share that lifetime. `message` may also
be empty or refer to a static string. The default configuration uses a
30-second timeout, follows at most ten redirects, limits a response to 64 MiB,
and verifies TLS certificates. HTTP does not define `_async` callees.

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

CCParsedUrl!>(CCHttpError) cc_url_parse(const char *url, size_t url_len);
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

Atomic operations are also methods on the atomic value: for a receiver whose
type is a `cc_atomic_*` typedef, `recv.method(args)` composes
`cc_atomic_<method>(&recv, args)` when the header declares that operation.

```c
cc_atomic_int n;
n.store(40);
int old = n.fetch_add(2);     // cc_atomic_fetch_add(&n, 2)
int cur = n.load();
int want = 42;
bool ok = n.cas(&want, 100);  // cc_atomic_cas(&n, &want, 100)
```

`CC_ATOMIC_HAVE_REAL_ATOMICS` is `1` when the header selects C11 atomics or
GCC/Clang atomic builtins. It is `0` for TinyCC and unknown-compiler fallback
implementations. Operations in a `0` configuration are not thread-safe.

## Python interop

### Model

An embedded Python interpreter anchors Python objects the way an arena
anchors allocations: every object has a home interpreter, its lifetime ends
no later than its home's, and nothing crosses homes implicitly. Crossing is
an explicit, costed operation (§7).

| Type | Role |
| ---- | ---- |
| `CCPy` | one interpreter handle; `arena` is its scratch |
| `CCPyObj` | an object reference, anchored to its home `CCPy` |
| `CCPyError` | Python exception with a `CCError base @as` face (§4) |

```c
#include <ccc/script/py.cch>

CCPy py = cc_py_new(&a) !> @destroy;
CCPyObj np = py.import("numpy") !> @destroy;
CCPyObj arr = np.call("arange", 10) !> @destroy;
double s = arr.call("sum").as_f64() !>;
```

### Loading

`cc_py_new` resolves `libpython3` with `dlopen` at first use; no CC binary
carries a Python dependency by existing. A missing or unloadable library is
a `CCPyError` at the `cc_py_new` call — the same posture as a missing binary
in `cc_command`. Bindings target the limited C API (`Py_LIMITED_API` / abi3):
one binding serves every 3.x, and nothing couples to interpreter internals.
Interpreter creation (§6) is the single exception, and it is resolved by name
so its absence is an error at that call rather than a load failure.
`py.cch` is never in the prelude; scripts include it.

`cc_py_available()` is the boolean form of the load question, for programs
that degrade rather than fail when Python is absent:

```c
if (!cc_py_available()) { puts("SKIP (no libpython)"); return 0; }
CCPy py = cc_py_new(&arena) !>;
```

The probe is the loader — same search order, same `CC_LIBPYTHON` override —
so it cannot disagree with the constructor. After a true probe, `!>` on
`cc_py_new` means what it says: a real initialization failure.

Spawning subprocesses while a `CCPy` is live is safe; `fork` without `exec`
is not supported.

### Running source text

`py.exec(src)` runs statements; `py.eval(expr)` yields the value of an
expression as `CCPyObj !>(CCPyError)`. Both evaluate in the interpreter's
`__main__` namespace, so a definition from one call is visible to the next
and to `py.import("__main__")`. They route through `builtins.exec` / `eval`
rather than `PyRun_*`, which is outside the limited ABI.

`eval` resolves through the handle's own destination-aware sink, so a
typed destination extracts directly — no intermediate object in user
space, the same resolution `CCPyObj` calls get:

```c
double d = py.eval(@slice("2.5 * triple(2)")) !>;
long long n = py.eval(@slice("1 << 40")) !>;
```

### Objects and calls

`CCPyObj` is opaque. `.get(name)` reads an attribute; `.call(name, args…)`
calls a method; both return `CCPyObj !>(CCPyError)`. Arguments marshal by
type: `int` / `int64_t` → Python `int`, `double` → `float`, `bool` →
`bool`, `CCSlice` / `char[:0]` → `str`, a typed slice (`double[:]` is
`CCSlice_double`, …) → `list` of its scalar element type, `CCPyObj` →
itself (same home required, §7). No other type marshals; there is no
deep conversion. Typed-slice dispatch is `_Generic` over the scalar
instance structs, so any expression of an instance type marshals as a
list; a slice erased to plain `CCSlice` (via `bytes()` or `.base`)
marshals as `str`. Non-scalar instances have no marshal arm and fail
at compile time.

Marshalling a typed slice builds one Python object per element, so the cost is
per element and the layout is rebuilt on the far side even when both sides
already agree on it.

`py_buf(x)` hands the slice over as a `memoryview` onto the CC buffer instead,
copying nothing:

```c
double s = m.sum_buffer(py_buf(xs)) !>;     /* CC side   */
np.frombuffer(mv, dtype=np.float64).sum()   #  Python side
```

The view borrows, and the borrow ends with the call: when the call
returns, the view is refcount-checked and released. A callee that kept
the view fails the call with a `CCPyError` naming the argument, and the
view is dead besides — a later touch raises `ValueError`, never a read
through freed CC memory. Compute through it or copy (`bytes(mv)`,
`numpy.frombuffer(mv).copy()`) inside the call.

The check's limit: a derived object kept past the call — a nested
memoryview, a numpy `frombuffer` array — shares the interpreter's
managed buffer without referencing the view, so it escapes both the
refcount check and the release; the buffer protocol snapshots the
pointer and has no revocation. Such a keep outlives its right to read
exactly as before, so the rule stands even where it cannot be enforced:
copy on the Python side to keep the data. The view is read-only, so a
callee cannot write back through it. A memoryview carries no element
type, so the callee names the dtype; that is the trade for not copying.

`py_buf` applies to a typed slice, which is what has a contiguous run to
borrow. Any other argument passes through unchanged and marshals normally.

`obj.as_list::[T](&arena)` converts a Python sequence to a typed run of
`T` — numbers to CC scalars, strings to arena-backed slices — and
`obj.as_map::[K, V](&arena, m)` fills a Map and yields the pair count. The
type argument names the element type(s); an element that will not convert
is a `CCPyError` naming the index.

A contiguous buffer exporter (a numpy array, `array.array`, `bytes`) whose
element format and size match `T` is read with one `memcpy` into the arena
instead of one boxed object per element; anything else — a plain `list`, or
an exporter of a different dtype — takes the per-element walk, so the result
is identical either way. Raw bytes only move when both sides agree on what
they mean: a `float32` array asked for as `double` converts per element
rather than being reinterpreted.

`f.map::[T](&arena, cols…)` calls a callable once per row across column
slices, in one crossing: each argument is a typed slice of equal length,
row `i` passes element `i` of every column, and the results land as a
`T[:]` run in the arena — `CCSlice !>(CCPyError)`. Columns are
independently typed, so a row may be a heterogeneous argument tuple
(`f.map::[double](&a, xs, ks, zs)` with `double`, `int64_t`, `double`
columns). The type argument names the result element type, the same
reading as `as_list`. Columns of unequal length are a `CCPyError` naming
the column, before any call runs; a row whose call raises or whose result
does not convert is a `CCPyError` naming the row. The free spelling is
`cc_py_obj_map(T, &f, &arena, cols…)`, to eight columns.

One crossing for N calls removes the per-call FFI overhead but not the
Python calls themselves, so it lands between the per-call form and a loop
run natively in Python; the rows section of `perf/py_baseline.ccs` prices
all three.

`py_kw(name, value)` makes a call argument bind by keyword instead of by
position. It marshals `value` by the same type rule and tags it with
`name`; keyword arguments may appear in any order and in any number, and
the remaining arguments keep their positional order. A name the callee
does not accept is an ordinary Python `TypeError`.

```c
CCSlice s = m.greet("world", py_kw("greeting", "hello")) !>.as_slice() !>;
```

A `char*` argument marshals through the C string API, so it truncates at an
embedded NUL; pass a slice to carry arbitrary bytes.

`CCPyError` carries `type_name` (the exception class) and `traceback` (the
formatted Python traceback, present when the failure crossed Python
frames), both anchored in the handle's scratch arena.

`CCPyObj`'s dynamic sink is destination-aware (`.ufcs_dynamic2`):
wherever a typed destination is visible — a declaration
`T name = obj.method(args…)`, an assignment to a resolvable lvalue, or
a cast `(T)obj.method(args…)` directly wrapping the call — the
destination joins UFCS resolution, and the call lowers through the
library's destination-typed variant (`cc_py_obj_callm_double`,
`_float`, `_int`, `_int64_t`, `_long_long`) when one is declared. The
variant runs the same call, extracts the destination type, and releases
the intermediate object — it never reaches user space:

```c
double v = math.sqrt(2.0) !>;
int    i = (int)math.sqrt(2.0) !>;
```

The variant returns `T !>(CCPyError)`, so the site consumes it like any
Result; a composed cast is absorbed — it spells the destination and
performs no conversion, and it never consumes the Result (the sigil
does). Declaration and assignment destinations apply only when the call
is the whole right-hand side — an operand of a larger expression has no
single expected type in C; a cast is its own destination anywhere.
Destinations without a declared variant lower through the plain sink
and keep the `CCPyObj` Result. A scalar destination with no declared
variant is ill-formed — the plain Result can never initialize it — and
the diagnostic names the destination and enumerates the installed
variants.

Extraction semantics are the library's: `double`/`float` accept any
Python number (`float` narrows); integer destinations extract Python
ints exactly and truncate Python floats toward zero (C cast and Python
`int()` agree); a result outside the destination's range is a
`CCPyError`, not a truncation.

Explicit extraction remains for held objects:
`.as_i64() !>`, `.as_f64() !>`, `.as_slice() !>` (`str`/`bytes` copied into
the home handle's scratch arena from `cc_py_new(&arena)`). Override the
destination with `.as_slice_into(&dst) !>`. The result slice is minted with
that arena's provenance epoch. Anything else stays a `CCPyObj`.

`cc_py_new(&arena)` stores `arena` on the handle. Error text and default
`.as_slice()` allocate from it. Every `CCPyObj` carries `home` pointing at
that handle so obj methods can reach the scratch arena.

Reference lifetime rides the destroy machinery: `CCPyObj`'s registered
destroy hook releases the reference under its home's lock, so `@destroy`
and `.destroy()` work and chain as for any type. Releasing after the home
interpreter is destroyed is a no-op (hook idempotence).

Ownership is compile-checked at the declaration: a local
`CCPyObj name = init;` must bind `@destroy`, be returned, or be released
by hand (`cc_py_obj_release(&name)`) — a handle that would silently drop
its reference on scope exit is an error, since the leak has no diagnostic
on either side of the boundary. Pointer declarations are borrows and
exempt; chain intermediates are compiler temporaries the fused sinks
already release.

### Errors

```c
typedef struct {
    CCError base @as;   /* kind + message: str(exception), arena-copied */
    CCSlice type_name;  /* exception class: `ValueError`, `KeyError`, … */
    CCSlice traceback;  /* formatted traceback; empty if none crossed */
} CCPyError;
```

A Python exception surfaces as `CCPyError`: `base.message` is the
exception's `str()` captured at raise time and copied into the handle's
scratch arena. The message remains valid until that arena is reset or
freed. Bootstrap failures before a handle exists (missing libpython, null
arena) use a process-static buffer. The script register's default
`@errhandler(CCError)` prints the face. An exact `@errhandler(CCPyError)`
claims the same face when a typed handler is preferred.

### Blocking

Interpreter calls are blocking-shaped: ill-formed in `@noblock` context,
serialized per interpreter. A single `CCPy` behaves as one implicit
exclusive; fibers contending it park like any blocking call.

### Interpreters

`cc_py_new` is the only constructor, and it yields an interpreter. The first
call in a process takes the one `Py_Initialize` creates. Every later call
creates an isolated interpreter with its own GIL, so two handles run Python
in parallel:

```c
CCPy a = cc_py_new(&arena) !> @destroy;
CCPy b = cc_py_new(&arena) !> @destroy;   /* isolated: its own GIL */
```

There is no pool type. A set of interpreters is an ordinary array of `CCPy`,
distributed like anything else; sharing one is passing the handle, the same
discipline an arena has.

Isolation is not best-effort. Where the runtime cannot create a second
interpreter — no per-interpreter GIL before CPython 3.12, or an extension
that refuses one — `cc_py_new` fails with that reason rather than returning
a handle to the first. A silently shared interpreter would serialize on one
GIL and share module state with no signal, which surfaces as unexplained
throughput collapse instead of an error at startup.

- `py.isolated` is 1 for an interpreter holding its own GIL, 0 for the
  process interpreter.
- Each isolated interpreter holds its own module state: an import per
  interpreter that uses it.
- Extensions must opt in to per-interpreter GIL support; those that predate
  multi-phase init refuse, and their import error surfaces verbatim.
  Pure-Python modules work unconditionally.
- On a free-threaded build (PEP 703) parallelism needs no isolation at all:
  one interpreter serves many threads, and `cc_py_new` means only what it
  says.

Entering an interpreter is a GIL handoff, not a pointer swap: every call
attaches its handle's thread state, releasing the lock of whichever
interpreter was current. A fiber cannot migrate mid-call, so a call always
completes in the interpreter it entered.

Using a handle from more than one OS thread is not yet supported. Two things
are required and only the first is built:

- A `PyThreadState` belongs to one OS thread — CPython keeps the current one
  in thread-local storage and derives per-thread bookkeeping from it — so each
  thread makes its own state per interpreter on first touch. Serializing
  CC-side entry does not substitute for this: there is no race to prevent, the
  state simply describes the wrong thread, and reusing it faults inside the
  interpreter even under perfect mutual exclusion.
- A thread must release the interpreter's GIL before blocking on anything
  else. Entry currently acquires and holds until the next entry on that
  thread, so a thread that parks while holding one deadlocks every other
  thread wanting that interpreter. Entry and exit must be symmetric —
  acquire on the way in, release on the way out — before a handle can cross
  threads.

### Host modules

When CC hosts the interpreter it injects modules under a `cc.` namespace, the
way any embedder installs host bindings. Python imports them normally:

```python
import cc.host
r = cc.host.add(20, 22)
```

The name is host-scoped on purpose. `import cc.host` declares a dependency on
running under CC, and an injected module can never shadow a package on
`sys.path` — a naked name could, since the host's modules resolve first.

Registration is per interpreter, not per process: `sys.modules` is
per-interpreter, and each interpreter — including an isolated one — builds its
own module instance, so per-module state is per interpreter. Sharing one
instance is what a per-interpreter GIL forbids. This also rules out
`PyImport_AppendInittab`, whose importer declines any dotted name.

An exported function runs with the GIL held, on a thread CPython chose: it
must not block, and a `T !>(CCError)` return raises at the boundary. A call
that does not match the declared parameters is an ordinary Python `TypeError`.

Exports use the vectorcall entry shape: arguments arrive as a pointer into
CPython's value stack with a count, so a call allocates no argument tuple, the
arity check is a comparison, and reading an argument is an array index.

Calls in the other direction are shaped the same way. A method name is
interned once per interpreter and kept on the handle, so a call is a dict
probe rather than the construction of a temporary string; arguments without
keywords go to `PyObject_Vectorcall` as an array. Keyword arguments still
build a tuple and a dict, since that is what carries them.

There is no default location on disk. Compiling a host module into a real
importable artifact is future work; the module definition is the same either
way, so only the registration differs.

### Exposing a CC type as a Python module

`py_expose::[T]` installs a CC type as an importable module under `cc.`:

```c
py.expose::[Counter]("counter", &seed) !>;        // installs cc.counter
py_expose::[Counter](&py, "counter", &seed) !>;   // the same call
```

The module is the type. Its methods are the module's functions and the
receiver is the module's per-interpreter state — the same reading of "first
parameter" UFCS dispatch runs on, so nothing new declares what a member is.
The Python name is the method's UFCS member name, so the export list and the
callable list are one list while the long searchable symbol stays at file
scope.

Return shapes map by declaration: a valued method returns its marshalled
value, a `void` method returns `None`, and a `T !>(E)` method raises at the
boundary rather than handing a Result to a caller that cannot act on one. An
argument of the wrong type or count is an ordinary Python `TypeError`.

The registrar returns the interpreter, so a registration composes into a
chain, including into another `expose`. A member-generic hop resolves after
the hop before it is hoisted into a typed temporary; the factory reflects on
a snapshot of the translation unit taken before lowering, so an instantiation
requested late in the pipeline generates the same code as one requested
early.

Registration fails host-side — no interpreter, no module — so it reports a
plain `CCError` rather than a Python exception nobody raised. A type with no
methods, an unnamed parameter, or a method whose Result box cannot be named
is rejected at the use site.

### The other door: Python imports CC

`py_module::[T]` is the same exposure pointed the other way: where
`py_expose` installs a module into an interpreter the CC program owns,
`py_module` creates the module and returns it — which is exactly what a
CPython extension entry point must do:

```c
/* counter.ccs — `ccc build counter.ccs` produces counter.abi3.so */
#include <ccc/script/py.cch>

typedef struct Counter { long long n; } Counter;
static long long Counter_bump(Counter *self, long long by) { return self->n += by; }

void *PyInit_counter(void) {
    return py_module::[Counter]("counter", NULL);
}
```

```python
import counter          # Python owns main; CC is the module
counter.bump(4)
```

There is no flag and no keyword: a TU that exports `PyInit_<name>` and
defines no `main` IS a Python extension module — CPython's own
entry-point convention is the declaration — and the build obeys: PIC
objects, a shared link, `<name>.abi3.so` as the default output — the abi3 tag is in every
CPython finder's suffix list, so `import <name>` finds it by bare name,
and the filename advertises the stable-ABI promise. A TU with a
`main` stays an executable even if it mentions `PyInit_`.

The seed is the initial module state, copied in at import (NULL for
zeroed). Failure follows Python's convention at this boundary — NULL
with the exception set — not a CC Result, because the caller is the
import machinery.

The trampolines, marshalling, and error conversion are the same
machinery `py_expose` emits. Trampolines are registered as
`METH_FASTCALL|METH_KEYWORDS`: positional arguments and keywords bind by
reflected parameter name (receiver excluded). A parameter may spell a
default as a trailing `= <literal>` (integer, float, string, char, `NULL`,
`true`, `false`); missing arguments for those slots take the literal.
Once a default appears, every trailing parameter must also have one.
Host C does not receive the defaults — lowering strips them from the
ABI signature. Unexpected names, duplicates, and missing required
arguments raise `TypeError`. Keyword-only markers (`*`) are not modeled.

`CCSlice` / `char[:]` parameters still borrow CPython's UTF-8 buffer for
the call. `CCPyStr` parameters borrow the `str` object itself; UFCS
`s.codepoints(arena)` materializes an arena-backed `uint32_t[:]` of Unicode
scalars via Stable-ABI `PyUnicode_AsUCS4` (no UTF-8 round-trip).

A fallible method's error crosses as the
Python exception its kind maps to, message intact — the kind is what a
Python caller dispatches on:

| CC kind | Python exception |
|---|---|
| `CC_ERR_INVALID_ARG` | `ValueError` |
| `CC_ERR_NOT_FOUND` | `LookupError` |
| `CC_ERR_TIMEOUT` | `TimeoutError` |
| `CC_ERR_PERMISSION` | `PermissionError` |
| `CC_ERR_OUT_OF_MEMORY` | `MemoryError` |
| `CC_ERR_OVERFLOW` | `OverflowError` |
| anything else | `RuntimeError` |

The module IS the type. Every function whose first parameter is `T` or
`T*` and is visible at the use site becomes a module function — except
an underscore member: `T__helper` reflects as `_helper` and stays
internal, Python's own privacy signal applied at the boundary. Renaming
and exposing foreign functions need no separate mechanism: a method is
just a function, so the export list grows by writing one — a one-line
wrapper with the public name, calling the private implementation.

The module's state is one `T` — a stateful module, not a class, which
is the same sharing pure-Python module globals have: every importer in
an interpreter sees the one instance. There are no Python-side
instances, constructors, or properties: "construct" means the seed, a
module-level function is a method that ignores its receiver, and
several independent states are several modules. A class surface
(`PyType_FromSpec`, real instances) would be a separate verb, not a
growth of this one.

Lifecycle facts: trampolines run with the GIL held for their whole
body, so module state is GIL-serialized — thread-safe under today's
CPython without locks. Module creation is multi-phase (`PyInit_` returns
the def; the import machinery creates the module and runs the exec
slot), so every import context gets its own seeded instance: a fresh
spec (`importlib.util.module_from_spec`) and a legacy sub-interpreter
each see an independent state. `importlib.reload` is CPython's usual
extension-module no-op — state survives. Isolated sub-interpreters
(per-interpreter GIL) remain refused: the def deliberately carries no
multiple-interpreters slot until the binding's process-global error
scratch is honest there. A runtime without `PyModuleDef_Init` falls back
to single-phase creation.

A type with a `destroy` method gets it wired as the module's `m_free`:
the state tears down when the module object deallocates — the same
lifecycle rule `@destroy` applies everywhere else. The seed remains a
plain copy at exec time; resource acquisition belongs in methods, and
release in `destroy`.

The loader resolves symbols from the importing process itself (the
self-probe runs before any `dlopen` of a libpython, so two runtimes can
never meet in one process), and the module never initializes an
interpreter — it is a guest. The abi3 discipline the binding already
keeps means one built module serves every supported Python 3.x: one
`.so` (or wheel) per platform, not per Python version.

### Moves

Status: draft — not implemented.

Anchored lifetime implies explicit transfer, arena-style:

```c
CCPyObj there = obj.clone_into(&other) !>;
```

`clone_into` serializes in the home interpreter and rebuilds in the
target (pickle round-trip); objects exposing the buffer protocol take a
byte-copy fast path. The source reference is untouched; drop it separately
when the move is a handoff. Scalars need no transfer: extract to a CC
value, pass the value.

An object that does not serialize fails with `CCPyError` at the call —
crossing is loud, never partial.

### Teardown

`CCPy` destroy ends an interpreter this handle created, after releasing
pending references; the process interpreter is left alive, since
re-initializing CPython is unreliable and process exit reclaims it. Declare
the interpreter before the objects it anchors:
reverse-declaration destroy order then releases every reference before
finalization. References that outlive their home release as no-ops (§3).

### Benchmarks

`perf/py_baseline.ccs` prices the boundary in both directions against native
Python controls on the same rung: a call into CC against a Python function
call of the same shape, a call out of CC against the same function called
from Python, and bulk transfer — list marshal, `py_buf` borrow, buffer-probe
extraction — against numpy and `sum()` over data each side already owns.
Every mode's answer is cross-checked, so a marshalling bug reports as a
mismatch rather than as a timing. Snapshots collect under `perf/baselines/`.

`perf/py_matplotlib_workload.ccs` exercises the surface against a real
library end to end: data computed in CC, rendered off-screen to disk in
Python.

### Out of scope

- Deep container conversion (dict/list ↔ CC collections)
- Python callbacks into arbitrary CC closures (an exposed type's methods are
  the supported direction)
- Compiling an exposed module to a standalone importable artifact
- Free-threaded (no-GIL) CPython builds
- `fork` without `exec` while an interpreter is live
- Non-limited C API (interpreter internals)
- Async integration (interpreter calls stay blocking-shaped)

## JavaScript interop

Status: draft — the guest surface is implemented: `js_module::[T]` and
its marshaling, the loader, and the outbound direction inside an
exported call (a `CCJs *` parameter is the host, wired by the trampoline
and invisible to JS; `global`/`eval`/`exec`; the `CCJsVal` sink with
destination-typed variants; `.get`/`.as_*`/`.hold`; `f.map::[T]` row
batching).  Hosting (`cc_js_new`), engines, `js_expose`, and
`as_list`/`as_map` are not.

### Model

The surface is native binding, in both directions: a CC type becomes a
JavaScript module whose functions are compiled trampolines, and a
JavaScript value binds into CC as an anchored handle whose calls are
native calls. No call on the surface routes through evaluation of source
text; running source text exists separately, as bootstrap glue.

An environment anchors JavaScript values the way an interpreter anchors
Python objects (§Python interop): every value has a home environment, its
lifetime ends no later than its home's, and nothing crosses homes
implicitly.

| Type | Role |
| ---- | ---- |
| `CCJs` | one environment handle; `arena` is its scratch; `engine` names its backend |
| `CCJsVal` | a value reference, anchored to its home `CCJs` |
| `CCJsError` | a JavaScript exception with a `CCError base @as` face (§4) |

```c
#include <ccc/script/js.cch>

CCJs js = cc_js_new(&a) !> @destroy;
CCJsVal m = js.import("./stats.node") !> @destroy;
double s = m.mean(xs) !>;
```

The binding ABI is Node-API: a versioned, engine-neutral C ABI that every
napi host (Node, Electron, Bun, Deno) implements, and that CC itself
implements over an engine it embeds. One binding layer serves every
backend; only where the symbols come from differs.

### Loading and engines

Symbols resolve with `dlopen`/`dlsym` at first use; no CC binary carries a
JavaScript dependency by existing. Resolution is the contract:

- A required symbol that does not resolve fails at table load, naming the
  symbol.
- An optional symbol leaves its slot empty, and the call that needs it
  reports cleanly at that call — a capability answer, never a stub that
  reports success.
- All binding is `RTLD_NOW`. A lazy failure would surface mid-call, far
  from the resolution that caused it.

The probe order: the process itself first (inside a napi host the symbols
are already present, and loading an engine would put two runtimes in one
process), then the `CC_LIBJS` override, then `libnode`, then the QuickJS
backend. `cc_js_available()` is the boolean form of the question and IS
the loader, so the probe and the constructor cannot disagree.

Engines differ in what they carry — `libnode` brings the Node standard
library and event loop; QuickJS brings a small runtime with cheap
isolation and no event loop — so engine identity is never silent:

- `js.engine` names the backend the handle got, and every `CCJsError`
  message carries it.
- `cc_js_new(&arena)` takes the first backend in probe order.
  `cc_js_new_with(&arena, CC_JS_QUICKJS)` (or `CC_JS_NODE`) demands one
  and fails cleanly when it is not loadable, naming what was demanded and
  what was found.

What a given engine build resolves is observed, not authored: a probe
program reports each table slot (resolved from host, implemented over the
engine, absent), and dated snapshots collect under `perf/baselines/` the
way benchmark baselines do. The spec does not enumerate symbols; the
table in `js.cch` is the one source of truth for what the surface asks
of a runtime.

### Binding CC into JS: `js_module::[T]`

`js_module::[T]` creates a Node-API module from a CC type — which is what
a napi addon entry point must return:

```c
/* counter.ccs — `ccc build counter.ccs` produces counter.node */
#include <ccc/script/js.cch>

typedef struct Counter { long long n; } Counter;
static long long Counter_bump(Counter *self, long long by = 1) { return self->n += by; }

void *napi_register_module_v1(void *env, void *exports) {
    return js_module::[Counter](env, exports, NULL);
}
```

```js
const counter = require('./counter.node');   // JS owns main; CC is the module
counter.bump(4);
```

There is no flag and no keyword: a TU that exports
`napi_register_module_v1` and defines no `main` IS a napi addon —
Node-API's own entry-point convention is the declaration — and the build
obeys: PIC objects, a shared link, `<name>.node` as the default output.
A TU with a `main` stays an executable even if it mentions the entry
point.

The module is the type, under the same reflection rules as
`py_module::[T]`: every visible function whose first parameter is `T` or
`T*` becomes a module function; an underscore member stays internal; the
receiver is the module's state, reached through the function's data slot;
the seed is the initial state, copied in at registration (NULL for
zeroed); a type with a `destroy` method gets it wired as the module's
finalizer. Parameter defaults bind as for Python exports. A call may
pass a trailing plain object whose properties bind by reflected parameter
name — JavaScript's own keyword convention — with unexpected names,
duplicates, and missing required arguments reported as `TypeError`.

Return shapes map by declaration: a valued method returns its marshalled
value, a `void` method returns `undefined`, and a `T !>(E)` method throws
at the boundary. The thrown error's class follows the CC kind — what a
JavaScript caller dispatches on — and its `code` property carries the CC
kind name, message intact:

| CC kind | JS error class |
|---|---|
| `CC_ERR_INVALID_ARG` | `TypeError` |
| `CC_ERR_OVERFLOW` | `RangeError` |
| anything else | `Error` |

One built artifact serves every napi host on a platform: Node, Electron,
Bun, Deno — and a CC QuickJS host (below). Failure at this boundary
follows JavaScript's convention — throw — not a CC Result, because the
caller is the host's module loader.

### Binding CC into JS: `js_expose::[T]`

`js_expose::[T]` is the same exposure into an environment the CC program
owns: it installs the type as `globalThis.cc.<name>`, the way any
embedder installs host bindings, with the same reflection, trampolines,
and error mapping as `js_module`. The name is host-scoped on purpose:
`cc.counter` declares a dependency on running under CC and can never
shadow an importable package.

```c
js.expose::[Counter]("counter", &seed) !>;
```

The registrar returns the environment, so registrations chain.
Registration fails host-side with a plain `CCError`.

### Binding JS into CC

`CCJsVal` is opaque. `obj.anything(args…)` calls the property natively:
the property key is created once per handle and held, the bound function
value is called directly with natively marshalled arguments — a call
costs like a call, never like an evaluation. `.get(name)` reads a
property; both return `CCJsVal !>(CCJsError)`; `!>` links hops as for
Python.

Arguments marshal by static type: `bool` → `boolean`, `double`/`float` →
`number`, `CCSlice` / `char[:0]` → `string`, a typed slice → a new
`TypedArray` of the matching element type (one copy), `CCJsVal` → itself
(same home required).

A typed-slice parameter of an exported method receives a JS array both
ways, priced by agreement: a `TypedArray` whose element type MATCHES the
destination borrows the caller's buffer for the call — zero copy — and
the borrow is WRITABLE: writes land in the caller's array, the in-place
idiom every napi addon uses.  A plain `Array`, or a `TypedArray` of a
different element type, converts per element into call scratch — raw
memory is only borrowed when both sides agree on what it means, so a
`Float32Array` asked for as `double[:]` converts rather than being
reinterpreted.  Both the borrow and the scratch copy end with the call; a
callee keeping the run past its return must copy.  A typed-slice RETURN
materializes a fresh `TypedArray` (one `ArrayBuffer`, one copy); the CC
side keeps owning its buffer.  `int64_t[:]` pairs with `BigInt64Array`.
Because `Array` and `TypedArray` are `typeof === 'object'`, the
trailing-object keyword convention excludes them: a trailing array is an
argument, never a keyword bag.

Integers follow one lossless rule both directions. A JavaScript `number`
is a double, so a CC integer whose magnitude is at most 2^53 marshals as
`number` and a larger one marshals as `BigInt`; inbound, an integer
destination accepts `number` and `BigInt` alike, range-checked against
the destination — a value that does not fit is an error naming the
argument, never a truncation.

`js_kw(name, value)` makes an argument bind by name: all named arguments
of a call fold into one trailing plain object, JavaScript's own
convention. `js_buf(x)` hands a typed slice over as an external
`ArrayBuffer` on the CC buffer, copying nothing. The borrow ends with the
call: the buffer is detached when the call returns, so every retained
reference — including a `TypedArray` built on it — is zero-length
afterwards and a later touch throws in the engine, never a read through
freed CC memory. The view is writable, and writes land in the CC buffer;
pass a copy to withhold write access. Detachment is engine-enforced,
so the borrow rule holds with no escape.

`obj.as_list::[T](&arena)` converts an `Array` or `TypedArray` to a typed
run of `T`; a `TypedArray` whose element type matches `T` is read with
one `memcpy`, anything else takes the per-element walk with the same
result. `obj.as_map::[K, V](&arena, m)` fills a Map from a `Map` or a
plain object's own enumerable properties. `f.map::[T](&arena, cols…)`
calls a callable once per row across column slices in one crossing, as
for Python.

The dynamic sink is destination-aware, with the same destination-typed
variants and extraction semantics as `CCPyObj`; explicit extraction
(`.as_f64()`, `.as_i64()`, `.as_slice()`) remains for held values.
Inbound strings copy into arena scratch — Node-API exposes no borrowed
UTF-8 — so a `CCSlice` parameter of an exported method is arena-backed
rather than a borrow of engine memory.

A `@variant` value crosses by arm, which is how a union-typed JavaScript
value binds natively — typed once at the boundary, checked projection
after. Outbound — a variant argument, or an exported method's variant
return — marshals the active arm by the rules above; a `void` arm
crosses as `null`. Inbound — a variant destination
`V v = obj.m(args…) !>;`, or a variant parameter of an exported method —
the arm is selected by the value's JavaScript runtime type: `boolean`
the `_Bool` arm; `number` and `BigInt` the numeric arm, range-checked as
always; `string` the slice arm, anchored in the handle's scratch arena;
a `TypedArray` the typed-slice arm of its element type; `null` and
`undefined` the `void` arm; any other value the `CCJsVal` arm when the
variant has one. Each class may claim at most one arm — a variant
offering two arms to one class is rejected at the use site, at compile
time. A value no arm accepts is reported at the boundary it crossed:
`TypeError` at an exported parameter, `CCJsError` at a CC destination,
naming the JavaScript type and the arms either way.

`@variant(packed)` binds identically — packing keeps the variant's
semantics surface, and marshalling goes through semantics, never layout —
so a packed `some`/`none` optional is the natural binding of a
JavaScript `T | null`, in both directions.

Ownership follows the Python rule: a local `CCJsVal` must bind
`@destroy`, be returned, or be released by hand; the destroy hook
releases the reference, and releasing after the home environment is gone
is a no-op.

### Errors

```c
typedef struct {
    CCError base @as;   /* kind + message: String(error), arena-copied */
    CCSlice name;       /* error class: `TypeError`, `RangeError`, … */
    CCSlice stack;      /* error.stack; empty if none */
} CCJsError;
```

A JavaScript exception surfaces as `CCJsError`: `name` is the error's
class, `stack` its formatted stack, both anchored in the handle's scratch
arena. The default `@errhandler(CCError)` prints the face; an exact
`@errhandler(CCJsError)` claims it.

### Threads and blocking

Environment calls are blocking-shaped: ill-formed in `@noblock` context,
serialized per environment. The thread rule is per backend, and it is the
one place the backends differ on the surface:

- A napi host environment belongs to its event-loop thread. Calls are
  calling-thread-only, and a fiber holding a call into such an
  environment must not migrate OS threads across it. There is no attach
  operation to make migration safe; cross-thread entry (a queued
  trampoline over a threadsafe function, parking the fiber on a channel)
  is future work.
- A QuickJS environment requires serialized access but is not pinned to
  an OS thread, so a handle may migrate between calls; per-environment
  serialization is the whole rule.

### Environments

`cc_js_new` yields an environment. Inside a napi host process the first
handle takes the host's environment; every later call — and every call in
a standalone process — creates one, where the backend can: each QuickJS
handle is its own runtime, so isolation is real and cheap; a backend that
cannot create another environment fails with that reason rather than
aliasing an existing one. There is no pool type; a set of environments is
an ordinary array of `CCJs`.

### A CC host is a napi host

The QuickJS backend implements the Node-API surface as real exported
symbols, so a CC program hosting QuickJS IS a napi host: a `.node`
artifact loads through the environment's module loader —

```js
import { bump } from './counter.node';
```

— its undefined `napi_*` references resolving against the process exactly
as they resolve against a `node` binary, `RTLD_NOW`, an unresolved symbol
failing at import and naming itself. An addon authored with
`js_module::[T]` resolves by construction: it references only what the
backend implements. A foreign addon loads when its references resolve;
one that asks for a capability the host lacks — an event loop, async
work — is answered at the asking call, not with a stub. The same artifact
loads in Node and in a CC host: one compiled module, every host, without
relinking either.

### Running source text

`js.exec(src)` runs statements and `js.eval(expr)` yields the completion
value, both in the environment's global scope, so a definition from one
call is visible to the next. Both are bootstrap glue — defining a helper,
selecting options — not a call path; everything after definition flows
through bound calls.

### Benchmarks

`perf/js_baseline.ccs` prices the boundary in both directions against
native JavaScript controls on the same rung — a call into CC against a
same-shape JS→JS call, a call out of CC against the same function called
from JS, and bulk transfer (`TypedArray` marshal, `js_buf` borrow,
typed-array extraction) against reductions over data each side already
owns — with every mode's answer cross-checked, per backend. The
composition workload — a napi host calling a CC module that reaches an
embedded Python interpreter, one `TypedArray` crossing both boundaries
with zero copies — prices the two interop surfaces end to end against the
same computation run in-process.

### Out of scope

- Full Node-API compatibility for arbitrary foreign addons (event loop,
  async work, `napi_define_class`, wrap/unwrap, threadsafe functions)
- Promise and async integration (environment calls stay blocking-shaped)
- The Node standard library under a QuickJS host
- Using one handle from more than one OS thread concurrently
- A class surface (real JS instances of a CC type) — a separate verb,
  not a growth of this one
- Deep container conversion (object/array graphs ↔ CC collections)
- Implicit cross-home use or implicit moves
