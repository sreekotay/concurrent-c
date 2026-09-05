# Declaration index: gap analysis

What the declaration index (`index.c`) resolves from declarations alone,
run over every UFCS call site in `tests/` and `examples/`, and the
(receiver type, method) pairs it cannot name. The second list is what the
stdlib headers must declare, as registrations or attributes, so the
lowering pass carries no table of names.

## Regenerate

```sh
make -C cc ccindex                      # out/cc/bin/ccindex
./out/cc/bin/ccindex --gaps tests/*.ccs examples/*.ccs examples/*/*.ccs > /tmp/gaps.md 2> /tmp/gaps.err
./out/cc/bin/ccindex --sites tests/*.ccs examples/*.ccs examples/*/*.ccs 2>/dev/null > /tmp/sites.txt
```

`--gaps` prints the tables below; its stderr names the files with parse
errors on their own lines. `--sites` prints one line per call site
(`file:line Type.method -> callee [source]` or the diagnostic). Single
questions:

```sh
./out/cc/bin/ccindex --dump FILE                     # symbols, hooks, types with method sets, Result specs
./out/cc/bin/ccindex --resolve FILE CCString len     # one (Type, method)
./out/cc/bin/ccindex --resolve FILE 'Map::[int, int]' get   # a type spelling is parsed
CC_INDEX_DEBUG_EXPANSIONS=1 ./out/cc/bin/ccindex --dump FILE   # parse errors inside macro / factory expansions
```

The include search path is `cc/include` (`-I dir` overrides). Run from
the repository root.

## Where a method comes from

The index answers `x.m()` for a receiver of canonical type `T` in this
order, and every answer is verified against a declaration (a function or
a function-like macro) before it is used:

1. `@typehooks on S { .ufcs = f }` for the narrowest subject matching `T`
   (exact, then `S*`, then `Fam_*`, then `*`). The handler body is read as
   rules: `if (method == "m") return emit("callee")`, `case "m":`,
   `return concat("prefix_", method)`. A handler that computes names from
   argument types is opaque and falls through; one that returns the empty
   slice rejects and falls through (the spec's "no custom rewrite").
   `x.destroy()` on a type with a `.destroy` hook uses the hook's callee.
2. `*_DECL_UFCS(Name)` registrations: `Name_m`.
3. `T_m` (a `struct Tag` receiver composes with `Tag`). The methods of a
   `CCResult_*` type, of a `CC_DECL_SLICE(T)` instance and of every
   generic instance come from here: the spec macro, the `CC_*_DECL_*`
   macro or the `CC_GENERIC_FACTORY` template is expanded and parsed as
   a synthetic header unit, so `CCResult_int_CCError_is_ok`,
   `CCSlice_int_at` and `CCVec_int_push` are ordinary declarations.
4. `cc_<snake(T)>_m`, and `<snake(T)>_m` for a type without the `CC`
   prefix (then `cc_<snake>_m` as well, which is what `cc_int_twice`
   needs).
5. The bare-name tier: a declared `m(T, ...)` (or `m(T*, ...)`,
   `m(void*, ...)`, or an arithmetic first parameter for an arithmetic
   receiver).
6. A typedef alias (`typedef CCArena MyArena;`, `typedef T[~1 <] Rx;`,
   `@typeview Mode on T` as `T_Restrict_Mode`) retries with the aliased type.
7. `@typeview on S { as: field; }` faces retry with the field's type.
8. `.ufcs_sink`.

Nothing in the index names a stdlib type or method. The one spelling it
carries is the `Vec` family's instance prefix `CCVec` (vec.cch documents
"the concrete C name is `CCVec_<T>`" in prose only; see the gap table).

## Corpus result

1456 files (every `.ccs` under `tests/` and `examples/`), 2945 UFCS call
sites. 43 files have parse errors on their own lines: 40 are `*_fail.ccs`
tests whose point is a parse diagnostic, plus `tests/arena_init_retired.ccs`,
`tests/arena_retired.ccs` (retired-syntax diagnostics) and
`tests/string_template_verbatim_smoke.ccs` (`${{ }}` in a template).

| Outcome | Sites |
|---|---|
| resolved: cc_snake_method | 1175 |
| resolved: Type_method (factory instance) | 485 |
| resolved: typehooks | 383 |
| resolved: Result | 311 |
| resolved: Type_method | 270 |
| resolved: ufcs_sink | 80 |
| resolved: snake_method | 67 |
| resolved: bare_name | 27 |
| resolved: DECL_UFCS | 8 |
| resolved (all sources) | 2806 |
| unresolved, receiver type known | 102 |
| receiver type unknown | 37 |

Sites whose receiver is an expression the tool can type (a local, a
parameter, a global, a field of a known struct, a call of a declared
function, a chained method, an unwrap of a Result) are 2908 of 2945.
`x.f()` where `f` is a field of `x`'s struct is a C call (the C-member-first
rule) and is not counted.

## Unresolved with a known receiver type

Every pair, from `--gaps`. Rows whose sites are all in `*_fail.ccs` tests
are the diagnostic those tests expect (`nosuch`, `no_such_method`,
`ghost`, `missing`, ...); the table keeps them for completeness.

| Receiver type | Method | Sites | Only in `_fail` tests |
|---|---|---|---|
| `cc_atomic_int` | `load` | 20 |  |
| `CCArena` | `allocT` | 8 |  |
| `cc_atomic_int` | `fetch_add` | 7 |  |
| `cc_atomic_int` | `store` | 7 |  |
| `char` | `strlen` | 4 |  |
| `ArgToy` | `mix` | 2 |  |
| `Box` | `m` | 2 |  |
| `CCResult_int_CCError` | `is_err` | 2 |  |
| `CCResult_int_CCError` | `is_ok` | 2 |  |
| `CCResult_int_CCError` | `unwrap_or` | 2 |  |
| `CCTaskIntptr` | `block_on` | 2 |  |
| `RespCmd` | `to_str` | 2 |  |
| `Shard` | `del` | 2 | yes |
| `double` | `fabs` | 2 |  |
| `ArrayMap_int_int` | `nosuch` | 1 | yes |
| `AsCommentMiss` | `no_such_method` | 1 | yes |
| `AsMissWrap` | `no_such_method` | 1 | yes |
| `CCChanRx_int` | `missing` | 1 | yes |
| `CCChanTx_CCResult_int_CCIoError` | `send_task_hybrid` | 1 |  |
| `CCChanTx_int` | `nosuch` | 1 | yes |
| `CCChanTx_intptr_t` | `send_task_hybrid` | 1 |  |
| `CCNursery` | `alloc` | 1 |  |
| `CCNursery` | `remaining` | 1 |  |
| `CCResult_bool_CCError` | `is_err` | 1 |  |
| `CCResult_bool_CCError` | `value` | 1 |  |
| `CCResult_int_CCError` | `nosuch` | 1 | yes |
| `CCResult_int_CCError` | `value` | 1 |  |
| `CCSlice` | `access` | 1 | yes |
| `CCSlice_double` | `ghost` | 1 | yes |
| `CCSlice_double` | `nosuch` | 1 | yes |
| `CCVec_double` | `nosuch` | 1 | yes |
| `CCVec_int` | `no_such_method` | 1 | yes |
| `CCVec_uint64_t` | `len` | 1 |  |
| `Fam_alpha` | `write` | 1 | yes |
| `JsonNode` | `count` | 1 |  |
| `JsonNode` | `first` | 1 |  |
| `JsonNode` | `next` | 1 |  |
| `PairReader` | `at_end` | 1 |  |
| `PairReader` | `next` | 1 |  |
| `PairReader` | `nope` | 1 | yes |
| `Port` | `gone` | 1 |  |
| `Reply` | `measure` | 1 |  |
| `Reply` | `to_str` | 1 |  |
| `Store` | `get` | 1 | yes |
| `Temp` | `gone` | 1 | yes |
| `Tweet` | `get` | 1 |  |
| `Widget` | `nonexistent_method` | 1 | yes |
| `cc_atomic_int` | `cas` | 1 |  |
| `cc_atomic_u64` | `fetch_add` | 1 |  |
| `cc_atomic_u64` | `load` | 1 |  |
| `double` | `ghost` | 1 | yes |
| `int` | `len` | 1 |  |

## Receiver type unknown

| Receiver kind | Sites |
|---|---|
| ident | 23 |
| member | 9 |
| !> | 4 |
| call | 1 |

| Method | Sites |
|---|---|
| `write` | 19 |
| `as_slice` | 7 |
| `hdr` | 3 |
| `twice` | 3 |
| `copy` | 2 |
| `len` | 2 |
| `wait` | 1 |

## What the stdlib must declare

The rows above that are not expected failures, grouped by the header that
owns them. Each is a place where the current lowerer carries a table
(`docs/compiler_internals.md` section 8) and the clean lowerer will not.

**`cc_atomic.cch` — `cc_atomic_int` / `cc_atomic_u64`: `load`, `store`,
`fetch_add`, `cas` (36 sites).** The receiver is a typedef of `_Atomic int`
and the callees are the function-like macros `cc_atomic_load(ptr, ...)`,
`cc_atomic_fetch_add(ptr, val)`. The index composes `cc_atomic_int_load`
and `int_load`; neither exists. Needed: `@typehooks on cc_atomic_* { .ufcs
= <prefix "cc_atomic_"> }` and, because the callees are macros with no
parameter types, the receiver-by-pointer decision cannot come from a
declaration: either declare typed `static inline` wrappers
(`cc_atomic_int_load(cc_atomic_int*)`) or give the hook contract a
by-pointer spelling next to `cc_ufcs_emit_value`.

**`cc_arena.cch` — `CCArena.allocT` (8 sites).** `arena.allocT(n)` lowers
today to `cc_arena_alloc_T_count(<destination type>, arena, n)`: the
callee's first argument is the *destination* type of the assignment. No
declaration can express that; it is one of the two remaining
compiler-owned rewrites (with `T[n].len`). Options: a `.ufcs` rule that
names `cc_arena_alloc_T_count` plus a declaration attribute that marks a
macro parameter as "the destination type"; or drop `allocT` from the
surface in favour of `cc_arena_alloc_T(T, arena)`.

**`cc_nursery.cch` — `CCNursery.alloc`, `.remaining` (2 sites).** The
current lowerer special-cases `cc_nursery_arena(` (internals section 5.4).
`CCNursery` is `struct { CCNurseryHost *p; }`; the arena is reached through
`cc_nursery_arena(n)`. Needed: a `.ufcs` rule or, if the arena becomes a
field, `@typeview on CCNursery { as: arena; }` so the arena's method set
applies.

**`cc_channel.cch` — `CCChanTx_*.send_task_hybrid` (2 sites).** The
registered hook `cc_channel_tx_lower_c` returns
`"cc_channel_send_task_hybrid"` for this method, and nothing in
`cc/include` or `cc/runtime` declares that name (the sole occurrence is the
hook line 664). The current lowerer resolves the method through its own
spawn-family table. Needed: declare `cc_channel_send_task_hybrid` (macro or
function) next to `cc_channel_send_task`, or drop the rule.

**`std/task.cch` — `CCTaskIntptr.block_on` (2 sites).** `cc_block_on_intptr`
is a macro with an unrelated name (and a parser-mode stub `(0)`). Needed:
`cc_task_intptr_block_on(CCTaskIntptr)` declared, or a `.ufcs` rule.

**`stdio.cch` — `cc_std_out.write`, `cc_std_err.write` (19 sites, "receiver
type unknown").** `cc_std_out` is not a declared object; it is an ambient
receiver in the table `cc_ufcs_ambient_rows` of `cc_ufcs_families.h`
(`std_out.write` -> `cc_std_out_write_auto`). Needed: declare the receivers
(`extern const CCStdOut cc_std_out;` with `cc_std_out_write(...)`, so the
snake rule applies) and delete the table.

**`cc_result.cch` — `CCResult_*.is_ok` etc. in a unit that includes no cc
header (8 sites, `tests/result_ufcs_methods.ccs`).** The Result method set
is the `CC_DECL_RESULT_SPEC` macro's expansion; a unit that spells `T!>(E)`
without including `cc_result.cch` has no macro to expand. Today the runtime
prelude supplies it. The driver must force-include the prelude the current
lowerer force-includes (`docs/compiler_internals.md` section 8.4) before
indexing; the index then needs nothing. `.shcc` scripts have the same
dependency on `<ccc/script/prelude.cch>`.

**`std/vec.cch` — `Vec` instance prefix.** `Vec::[int]` names `CCVec_int`
but `Map::[K,V]` names `Map_K_V`; only a comment in vec.cch says so, and the
index hard-codes the one alias. Needed: the factory states its instance
prefix (`CC_GENERIC_FACTORY(Vec, 1) as CCVec` or an attribute), and the
`__CC_VEC(T)` / `cc_vec_new::[T]` sugar follows the same declaration.

**`CCVec_uint64_t.len` (1 site).** The instance expands, but the expansion
parses `uint64_t *data;` as an expression because `uint64_t` is a
`<stdint.h>` typedef the parser has never seen. The driver's `known_types`
for expansions and headers must include the C standard typedef names (the
names come from the host headers, not from a stdlib list; parsing
`<stdint.h>` / `<stddef.h>` through the host preprocessor once and reading
their typedefs is the declaration-driven way).

## Not gaps in the headers

- **Comptime- and grammar-generated declarations.** `JsonNode.*`,
  `RespCmd.to_str`, `Reply.*`, `Tweet.get`, `PairReader.*` (grammar
  engines), `Box.m` (a comptime splice), `ArgToy.mix` (a `.ufcs` handler
  that dispatches on `arg_types`; the index marks it opaque). These exist
  only after the comptime seam runs. The lowerer indexes the seam's emitted
  text the way it indexes macro expansions (`cc__index_expansion`), and
  runs opaque hooks through the executor.
- **C library names.** `"s".strlen()`, `x.fabs()`: the bare-name tier to a
  function declared by a system header the index does not read. A policy
  decision for the lowerer: an undeclared bare name lowers to the C call
  `f(x, ...)` (what the host compiler will check) or is refused.
- **`T[n].len`** (`int.len`, 1 site): the spec makes it the constant bound,
  a lowering rule, not a declaration.
- **`Port.gone`** is the documented compose-then-verify failure
  (`docs/typehooks-typeviews.md`); the diagnostic is the point.
- **Untyped receivers** (37 sites after the ambient `cc_std_out` rows):
  `@variant` arm payloads (`r.simple.s.as_slice()`), grammar-generated
  structs, results of macros with no declared type (`cc_unwrap(r).len()`),
  and `@for` binders over generic containers. These need the type checker
  the lowerer gets in later milestones (`.access` hooks give the element
  type); the index itself needs nothing.

## Parser findings that limit the index

Found while running the corpus; the parser owns them.

- **Specifier macros**: `CC__ARENA_SYS void f(...)`, `CC__RESULT_SYS
  CC_NORETURN void cc_error_exit(...)` — an unknown identifier before the
  type is "expected a declaration" in cc_arena.cch (about 120 declarations
  lost, all internal `cc__arena_*`; recovery keeps the rest). The index
  knows these are macros whose bodies are `static inline` / empty; a parser
  hook that asks the index (or a `known_macros` list like `known_types`)
  removes the class. `CC_NORETURN` is handled: the index scans the
  specifier tokens before the name and sets `CC_F_NORETURN` when a macro
  spelled there has `noreturn` in any of its definitions.
- **`f((T*)&v)` in a statement** is parsed as a declaration with a
  parenthesised declarator when `f` is unknown (seen inside the
  `CC_VEC_DECL_ARENA_CORE` expansion: 3 errors per instance; the following
  functions still parse).
- **`p->m(args)`** is a call of a member, not a `CC_E_UFCS` node
  (`ccindex` treats it as a site unless `m` is a field). The lowerer needs
  the same check for `.` calls: `x.cb()` where `cb` is a function-pointer
  field is C.
- **`@parallel wait (ts)`** and the join's `!>.wait()` tail are represented
  as UFCS nodes on the identifier `@parallel`; `ccindex` skips them.
- **Nested generic arguments** (`Map::[int, Vec::[int]]`) arrive as
  `CC_T_VALUE` expressions; the index re-parses the text as a type.
- Both branches of `#if` / `#else` are parsed (there is no preprocessor):
  duplicate declarations are merged by "a definition beats a prototype".
