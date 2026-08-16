# Canonical generic name mangling

C never gave generic instantiations a shared name. Every project that fakes
generics (`vec_int`, `VecInt`, `vec_of_int_t`, …) invents its own scheme, so two
libraries' "vector of int" are different C symbols and cannot compose. Concurrent
C fixes this by **blessing one mangling recipe** and exposing it, so a library's
private generics line up byte-for-byte with the built-ins and with every other
library that follows the recipe.

This is naming, not namespacing: it stays C's flat symbol model. The win is that
`Foo::[int]` in library A and `Foo::[int]` in library B name the *same* C type, so
their generated code links and interoperates instead of clashing silently.

## The recipe

For `base::[arg0, arg1, …]` the canonical name is:

```
base + "_" + mangle(canon(arg0)) + "_" + mangle(canon(arg1)) + …
```

1. **canon(arg)** — normalize the type spelling (trim, collapse, fold surface
   sugar such as the slice spelling). This is `cc__canonicalize_container_param_type`.
2. **mangle(s)** — sanitize the canonical spelling into an identifier-safe token:
   - identifier tokens join with `_` (`long long` → `long_long`; `size_t` stays
     `size_t`),
   - `*`  → `ptr`,
   - `[:]` (slice) → `slice`,
   - `[`, `]`, `,`, `<`, `>` → `_`,
   - all other characters copied verbatim (decimal integer args stay digits),
   - trim trailing `_`.

The factory still receives each argument as a C spelling (`arg(0)` is
`long long`, not `long_long`). Underscore-splitting the compact name is not
how arity is recovered.

### Examples

| Instantiation            | Canonical name        |
|--------------------------|-----------------------|
| `Pair::[int, double]`    | `Pair_int_double`     |
| `Vec::[int]` / `CCVec::[int]` | `CCVec_int`      |
| `Map::[int, double]`     | `Map_int_double`      |
| `ArrayMap::[int, double]`| `ArrayMap_int_double` |
| `Box::[char*]`           | `Box_charptr`         |
| `Span::[char[:]]`        | `Span_slice`          |
| `SmallVec::[int, 8]`     | `SmallVec_int_8`      |
| `SmallVec::[long long, 8]` | `SmallVec_long_long_8` |
| `Map::[size_t, int]`     | `Map_size_t_int`      |

The Vec family name is `Vec`; the instance spelling is `CCVec_<T>` (not
`Vec_<T>`). Map/ArrayMap instances are `Name*` sugar; Vec is the struct.

## Using it

`cc_canonical_name(base, args, nargs, out, out_sz)` (declared in
`<ccc/cc_instantiate.cch>`, available in `@comptime` blocks and compiled factory
dylibs) returns the exact name the compiler uses, so a factory can:

- name privately emitted helpers consistently (`Pair_int_double_make`, …), and
- **register for composition interop** — produce the canonical name other
  libraries will look up, rather than a private spelling only it understands.

```c
@comptime {
    const char* args[2] = { "int", "double" };
    char name[128];
    cc_canonical_name("Pair", args, 2, name, sizeof(name));   /* "Pair_int_double" */
    /* emit `typedef … name;` and helpers keyed off `name` */
}
```

Backed by `cc_ct_canonical_name` (`cc/src/preprocess/preprocess.h`), the same
routine the rewriter uses for `Name::[args]`, so the helper can never drift from
what the compiler actually emits.

## Collisions are loud, never silent

C's one genuinely silent footgun is duplicate external symbols at link time. The
emit-provenance machinery (see `COMPTIME_INSTANTIATION_SEAM.md`, edge-push #5)
stamps every comptime-emitted fragment with its origin `#line`, which is what a
dup-emit-name detector uses to report two definitions of the same symbol with
*both* origins instead of letting the linker pick one. Following the canonical
recipe means a genuine "same type" dedups cleanly, while a genuine clash surfaces
with a diagnostic rather than disappearing.
