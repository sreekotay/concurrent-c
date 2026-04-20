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
- `cc_buf_reader_next(…)` / `cc_buf_reader_read_line(…)`  →  return `CCRes(CCSlice, CCIoError)`; EOF is an empty slice.
- `cc_parsed_args_value_at(…)` / `cc_parsed_args_last_value(…)`  →  return `const CCSlice*` (NULL if absent).

### Compiler / internal notes

- `cc_optional.cch` has been removed; use `cc_result.cch` for `CCRes(T, E)` / `CCRes_ok` / `CCRes_err` / `CCResPtr*`.
- The preprocessor's `cc__rewrite_optional_types` pass is now a diagnostic-only
  pass: it detects stray `T?` sigils and reports a migration hint.
- The TCC UFCS fallback that inferred `__CCOptionalGeneric` as the return type
  of `Vec.get` / `Vec.pop` has been disabled.
- `CC_VEC_DECL_ARENA_FULL` is kept as a backward-compatible alias for
  `CC_VEC_DECL_ARENA` that ignores its `OptT` slot. `CC_MAP_DECL_ARENA_FULL`
  is similarly an alias for `CC_MAP_DECL_ARENA`.
