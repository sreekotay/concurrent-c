# Concurrent-C Deprecations

This file tracks deprecations in the public Concurrent-C surface. Each entry
lists what was retired, why, and how to migrate.

## Optional types (`T?`, `CCOptional_T`, `cc_is_some`, `cc_unwrap_opt`, …)

**Status:** Retired. The compiler now emits a diagnostic when it encounters
`T?` in a type context.

**Why:** Optionals served several distinct purposes that each have a more
appropriate, lower-cost shape in Concurrent-C:

- Container lookups want a nullable pointer so callers can read-in-place.
- Iterators and `pop` operations want `bool` + out-parameter so no intermediate
  struct is materialized.
- Stream reads want an in-band sentinel (empty slice for EOF).
- Fallible operations want a proper `Result` with an error channel.

Maintaining `CCOptional_T` and its TCC parser-mode scaffolding for all of these
roles simultaneously made the compiler, standard library, and generated C code
more complex than the underlying semantics warrant.

### Migration matrix

| Old (`T?`)                         | New                                 |
| ---------------------------------- | ----------------------------------- |
| `T? vec.get(i)`                    | `T* vec.get(i)` (NULL if absent)    |
| `T? map.get(k)`                    | `V* map.get(k)` (NULL if absent)    |
| `T? vec.pop()`                     | `bool vec.pop(T* out)`              |
| `T? it.next()`                     | `bool it.next(T* out)`              |
| `CCSlice? backend_read(...)`       | `CCSlice read(...)` (`len == 0` on EOF) |
| `CCSlice? buf_reader_next(...)`    | `CCSlice !>(CCIoError)` (`len == 0` on EOF) |
| `CCOpt(int) maybe_lookup(...)`     | `int !>(CCError)` or `int* (NULL absent)` |
| `cc_is_some(opt)` / `cc_is_none(opt)` | `p != NULL` / `p == NULL` or inspect bool |
| `cc_unwrap_opt(opt)` / `*opt`      | `*p` (after null-check)             |
| `cc_unwrap_or(opt, d)`             | `p ? *p : d`                        |
| `CCOpt(T)` / `CCOpt_some` / `CCOpt_none` | one of the shapes above        |

### Stdlib helpers that changed signature

- `cc_vec_*_get(v, i)`  →  returns `T*`
- `cc_vec_*_pop(v, T* out)`  →  returns `bool`
- `cc_map_*_get(m, k)`  →  returns `V*`
- `backend_read(…, CCSlice* out)` / `backend_read_line(…, CCSlice* out)` — EOF is an empty slice.
- `CCBufReader` / `cc_buf_reader_*`  →  `BufReader::[Src]` (`buffered` / `fill` / `read_line` / `read_exact`).
- `cc_parsed_args_value_at(…)` / `cc_parsed_args_last_value(…)`  →  return `const CCSlice*` (NULL if absent).

### Compiler / internal notes

- `cc_optional.cch` has been removed; use `cc_result.cch` for `CCRes(T, E)` / `CCRes_ok` / `CCRes_err` / `CCResPtr*`.
- The preprocessor's `cc__rewrite_optional_types` pass is now a diagnostic-only
  pass: it detects stray `T?` sigils and reports a migration hint.
- The TCC UFCS fallback that inferred `__CCOptionalGeneric` as the return type
  of `Vec.get` / `Vec.pop` has been disabled.
- The optionals-era `_FULL` aliases (`CC_VEC_DECL_ARENA_FULL`,
  `CC_VEC_DECLARE_GUARDED[_FULL]`, `CC_VEC_DECL_HEAP_FULL`,
  `CC_MAP_DECL_ARENA_FULL`, `CC_MAP_DECL_{INT,U64,SLICE}_FULL`) have been
  removed; call `CC_VEC_DECL_ARENA` / `CC_VEC_DECL_HEAP` /
  `CC_MAP_DECL_ARENA` directly.
- The fixed-key map wrappers `CC_MAP_DECL_INT` / `CC_MAP_DECL_U64` /
  `CC_MAP_DECL_SLICE` have been removed. In Concurrent-C source, spell the
  key type (`Map::[int, V]`); a user key type installs by declaring
  `cc_map_key_hash_<K>` / `cc_map_key_eq_<K>`. In plain C, invoke
  `CC_MAP_DECL_ARENA(K, V, Name, HASH_FN, EQ_FN)` with the hash/eq pair
  (`cc_map_hash_i32`/`cc_map_eq_i32`, `cc_map_hash_u64`/`cc_map_eq_u64`,
  `cc_map_hash_slice`/`cc_map_eq_slice`).
- `CC_DECL_SLICE_SPEC` takes two parameters (`NAME`, `T`); the former
  middle `SNAKE` parameter is gone — members are `NAME##_<member>`
  (`CCSlice_double_at`), the same instance-prefix convention as Vec/Map.
  `CC_DECL_SLICE(T)` is the single-token convenience. Concurrent-C source
  rarely needs either: `T[:]` / `CCSlice::[T]` auto-instantiates.
