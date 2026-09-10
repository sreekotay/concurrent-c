# Declaration index: gap analysis

What the declaration index (`index.cch`, `index_impl.cch`) resolves from
declarations alone, run over every UFCS call site in `tests/` and
`examples/`, and the (receiver type, method) pairs it cannot name. The
second list is what the stdlib headers must declare, as registrations or
attributes, so the lowering pass carries no table of names.

## Regenerate

```sh
make -C cc                              # bin/ccc, the compiler lower-cc builds with
CC_NO_CACHE=1 make -C cc lower-cc       # out/cc/bin/ccindex_cc
./out/cc/bin/ccindex_cc --gaps tests/*.ccs examples/*.ccs examples/*/*.ccs > /tmp/gaps.md 2> /tmp/gaps.err
./out/cc/bin/ccindex_cc --sites tests/*.ccs examples/*.ccs examples/*/*.ccs 2>/dev/null > /tmp/sites.txt
```

`ccindex_cc` is the Concurrent-C index; `out/cc/bin/ccindex` (from
`make -C cc`) is the C11 reference it is gated against. `--gaps` prints
the tables below; its stderr names the files with parse errors on their
own lines. `--sites` prints one line per call site
(`file:line Type.method -> callee [source]` or the diagnostic). Single
questions:

```sh
./out/cc/bin/ccindex_cc --dump FILE                     # symbols, hooks, types with method sets, Result specs
./out/cc/bin/ccindex_cc --resolve FILE CCString len     # one (Type, method)
./out/cc/bin/ccindex_cc --resolve FILE 'Map::[int, int]' get   # a type spelling is parsed
CC_INDEX_DEBUG_EXPANSIONS=1 ./out/cc/bin/ccindex_cc --dump FILE   # parse errors inside macro / factory expansions
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

Nothing in the index names a stdlib type or method. The spellings it
carries are the `Vec` family's instance prefix `CCVec` (vec.cch documents
"the concrete C name is `CCVec_<T>`" in prose only; see the gap table)
and a `_t` suffix, which it takes for a C standard typedef (`uint64_t`,
`size_t`) when it reads a type argument back out of a mangled instance
name.

## Corpus result

1487 files (every `.ccs` under `tests/` and `examples/`), 3163 UFCS call
sites. 54 files have parse errors on their own lines: 48 are `*_fail.ccs`
tests whose point is a parse diagnostic, plus `tests/ordered_tx_error.ccs`
(a `.compile_err` test without the suffix) and the five `*_retired.ccs`
tests (`arena_init_retired`, `arena_retired`, `map_legacy_spelling_retired`,
`nursery_closing_retired`, `vec_legacy_spelling_retired`: retired-syntax
diagnostics).

| Outcome | Sites |
|---|---|
| resolved: cc_snake_method | 1237 |
| resolved: Type_method (factory instance) | 551 |
| resolved: typehooks | 502 |
| resolved: Result | 320 |
| resolved: Type_method | 280 |
| resolved: ufcs_sink | 81 |
| resolved: snake_method | 70 |
| resolved: bare_name | 26 |
| resolved: DECL_UFCS | 8 |
| resolved (all sources) | 3075 |
| unresolved, receiver type known | 60 |
| receiver type unknown | 28 |

Sites whose receiver is an expression the tool can type (a local, a
parameter, a global, a field of a known struct, a call of a declared
function, a chained method, an unwrap of a Result) are 3135 of 3163.
`x.f()` and `p->f()` where `f` is a field of function or function-pointer
type are C calls (the C-member-first rule) and are not counted; a data
field named like a method (`Vec`'s `len`) does not hide the method.

## Unresolved with a known receiver type

Every pair, from `--gaps`. Rows whose sites are all in `*_fail.ccs` tests
are the diagnostic those tests expect (`nosuch`, `no_such_method`,
`ghost`, `missing`, ...); the table keeps them for completeness.

| Receiver type | Method | Sites | Only in `_fail` tests |
|---|---|---|---|
| `CCArena` | `allocT` | 8 |  |
| `PickToy` | `pick` | 3 |  |
| `ArgToy` | `mix` | 2 |  |
| `Box` | `m` | 2 |  |
| `CCTaskIntptr` | `block_on` | 2 |  |
| `RespCmd` | `to_str` | 2 |  |
| `Shard` | `del` | 2 | yes |
| `Widget` | `destroy` | 2 |  |
| `double` | `fabs` | 2 |  |
| `ArrayMap_int_int` | `nosuch` | 1 | yes |
| `AsCommentMiss` | `no_such_method` | 1 | yes |
| `AsMissWrap` | `no_such_method` | 1 | yes |
| `CCChanRx_int` | `missing` | 1 | yes |
| `CCChanTx_CCResult_int_CCIoError` | `send_task_hybrid` | 1 |  |
| `CCChanTx_int` | `nosuch` | 1 | yes |
| `CCChanTx_intptr_t` | `send_task_hybrid` | 1 |  |
| `CCResult_int_CCError` | `nosuch` | 1 | yes |
| `CCSlice` | `access` | 1 | yes |
| `CCSlice_double` | `ghost` | 1 | yes |
| `CCSlice_double` | `nosuch` | 1 | yes |
| `CCVec_double` | `nosuch` | 1 | yes |
| `CCVec_int` | `no_such_method` | 1 | yes |
| `Fam_alpha` | `write` | 1 | yes |
| `JsonNode` | `count` | 1 |  |
| `JsonNode` | `first` | 1 |  |
| `JsonNode` | `next` | 1 |  |
| `Pair` | `by_val_only` | 1 | yes |
| `Pair` | `get_x` | 1 | yes |
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
| `Widget` | `on_pair` | 1 |  |
| `Widget` | `on_value` | 1 |  |
| `char` | `strlen` | 1 |  |
| `double` | `ghost` | 1 | yes |
| `double` | `zap` | 1 | yes |
| `int` | `len` | 1 |  |

## Receiver type unknown

| Receiver kind | Sites |
|---|---|
| ident | 19 |
| member | 8 |
| !> | 1 |

| Method | Sites |
|---|---|
| `write` | 19 |
| `as_slice` | 5 |
| `hdr` | 3 |
| `twice` | 1 |

## What the stdlib must declare

The rows above that are not expected failures, grouped by the header that
owns them. Each is a place where a lowerer carries a rule in code (the
shadow lowerer's tables are `docs/compiler_internals.md` section 8; the
clean lowerer's are listed in `docs/plans/clean_lowerer.md` §1.5) and a
declaration would replace it.

**`cc_arena.cch` — `CCArena.allocT` (8 sites).** `arena.allocT(n)` lowers
to `cc_arena_alloc_T_count(<element type>, arena, n)`: the callee's first
argument is the element type of the assignment's destination pointer, or
the `::[T]` the site binds. No declaration can express that; the clean
lowerer keeps `allocT` (with `block_on` below) as the closed set of members
that bind a type formal, in `lower_ufcs.cch`, and it is one of the two
compiler-owned rewrites (with `T[n].len`). Options: a `.ufcs`
rule that names `cc_arena_alloc_T_count` plus a declaration attribute that
marks a macro parameter as "the destination type"; or drop `allocT` from
the surface in favour of `cc_arena_alloc_T(T, arena)`.

**`cc_channel.cch` — `CCChanTx_*.send_task_hybrid` (2 sites).** The
registered hook `cc_channel_tx_lower_c` returns
`"cc_channel_send_task_hybrid"` for this method, and nothing in
`cc/include` or `cc/runtime` declares that name (the sole occurrence is the
hook line). Both lowerers resolve the method by its spelling in their
spawn-family rules. Needed: declare `cc_channel_send_task_hybrid` (macro or
function) next to `cc_channel_send_task`, or drop the rule.

**`std/task.cch` — `CCTaskIntptr.block_on` (2 sites).** `cc_block_on_intptr`
is a macro with an unrelated name (and a parser-mode stub `(0)`); the clean
lowerer binds `block_on` from the destination type the way it binds
`allocT`. Needed: `cc_task_intptr_block_on(CCTaskIntptr)` declared, or a
`.ufcs` rule.

**`stdio.cch` — `cc_std_out.write`, `cc_std_err.write` (19 sites, "receiver
type unknown").** `cc_std_out` is not a declared object; it is an ambient
receiver in the table `cc_ufcs_ambient_rows` of `cc_ufcs_families.h`
(`std_out.write` -> `cc_std_out_write_auto`, a `_Generic` in `std/io.cch`),
which both lowerers read. Needed: declare the receivers
(`extern const CCStdOut cc_std_out;` with `cc_std_out_write(...)`, so the
snake rule applies) and delete the table.

**`std/vec.cch` — `Vec` instance prefix.** `Vec::[int]` names `CCVec_int`
but `Map::[K,V]` names `Map_K_V`; only a comment in vec.cch says so, and the
index hard-codes the one alias (`cc__family_prefix` / `cc__family_name` in
`index_impl.cch`). Needed: the factory states its instance prefix
(`CC_GENERIC_FACTORY(Vec, 1) as CCVec` or an attribute), and the
`__CC_VEC(T)` / `cc_vec_new::[T]` sugar follows the same declaration.

## Not gaps in the headers

- **Comptime- and grammar-generated declarations.** `JsonNode.*`,
  `RespCmd.to_str`, `Reply.*`, `Tweet.get`, `PairReader.*` (grammar
  engines), `Box.m` (a comptime splice), `ArgToy.mix` and `PickToy.pick`
  (`.ufcs` handlers that decide from `arg_types` or the call site; the
  index marks them opaque). These exist only after the comptime seam runs.
  The clean lowerer runs the seam before it lowers (the driver's comptime
  stage), indexes the emitted text the way it indexes macro expansions
  (`cc__index_expansion`), and runs opaque hooks through the executor.
- **Closure-typed fields.** `Widget.destroy`, `.on_value`, `.on_pair`
  (4 sites): `w->destroy()` where `destroy` is a `CCClosure0` field is a
  call on the value stored there, a rule in `lower_closures.cch` (the field
  shadows the method). The C-member-first rule covers function and
  function-pointer fields only, so the index reports the pair.
- **C library names.** `"s".strlen()`, `x.fabs()`: the bare-name tier to a
  function declared by a system header the index does not read. A policy
  decision for the lowerer: an undeclared bare name lowers to the C call
  `f(x, ...)` (what the host compiler will check) or is refused.
- **`T[n].len`** (`int.len`, 1 site): the spec makes it the constant bound,
  a lowering rule, not a declaration.
- **`Port.gone`** is the documented compose-then-verify failure
  (`docs/typehooks-typeviews.md`); the diagnostic is the point.
- **Untyped receivers** (28 sites; 9 after the ambient `cc_std_out` rows):
  `@variant` arm payloads (`r.simple.s.as_slice()`), grammar-generated
  structs (`hdr` on a schema-grammar union, `as_slice` on a RESP node),
  and one `_fail` test (`ufcs_chain_untypable_fail`). These need the type
  checker the lowerer gets in later milestones (`.access` hooks give the
  element type); the index itself needs nothing.

## Parser findings that limit the index

Found while running the corpus; the parser owns them.

- **`#if` regions**: `#if 0` / `#if 1`, `#ifdef __cplusplus` and the
  `defined(__cplusplus)` forms are decided statically and the inactive
  region is skipped (`parse_state.cch`, `pp_classify` / `pp_note`). Any
  other `#if` keeps both branches (there is no preprocessor), and duplicate
  declarations are merged by "a definition beats a prototype".
- **A macro the use site terminates**: `CC_DECL_BOX_ALIAS(Alias, Host)`
  ends in `typedef CCBox_##Host Alias` and the `;` comes from the use site
  (`cc_exclusive.cch:106`, `cc_nursery.cch:30`). The expansion is parsed
  without it, so `CC_INDEX_DEBUG_EXPANSIONS=1` reports "expected ',' or ';'
  after declarator, found end of file" at each; recovery keeps the typedef
  (`CCNursery` and `CCExclusive` are indexed), so the error is noise.
