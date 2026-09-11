# Own C parser

The lowerer (`cc/lower/*.cch` / `*.ccs`) parses Concurrent-C and lowers it
to C. TinyCC is a comptime machine for **already-lowered C**. Neither one
is a general C grammar with a preprocessor of its own.

This repo owns a **C23+ parser** (plus the GNU the tree actually writes).
It is a host-C library that sits on a pp-token stream, not a second
include engine.

A parser that names only the shapes it expects cannot name every token
(`#else` as a type name is the canonical failure). This repo owns a full
C23+ parser instead. We do **not** own a C compiler: parse + two
projections; host `cc` compiles the emit; TCC evaluates lowered C.

## Layers

| Piece | Owns |
|-------|------|
| **Tape** (stage-1 / stage-2) | pp-tokens, quote/angle include, `#line` |
| **C parser** (`cc/cparse/` or similar, host C) | TU, declarators, types, stmts, cpp |
| **lowerer** | `!>`, `@errhandler`, UFCS, comptime, quote-dir, `#pragma(@parallel)` |
| **TCC** | Evaluate / `--exe` lowered C. No ExtParser, no `!>` |

Do not reintroduce Stub-AST or UFCS-in-TCC
(`third_party/tcc-patches/HOOKS.md`).

Write the parser in **host C**, so it stands on its own and needs no
lowerer to build. Tools call the library through its C header.

`.ccs` is not C. First merge is C-shaped headers. Overlay later is a
**C-superset on the same pp-token tape** (CC productions as nodes), not
a rewrite back to C and not a second lexer.

## Preprocessor: most of classic cpp, not all of a compiler

Preserve (emit) does not expand. Evaluate (LSP / types / later overlay
understanding of `CC_DECL_SLICE(int)`) needs the **C11 expansion
algorithm** on **project units**. That is “all of classic cpp” in the
small sense: directives, object- and function-like macros, `#` / `##` /
`__VA_ARGS__`, `defined`, `#if` integer expressions, `#undef`, hide-set
rescan.

It is not “all of clang’s preprocessor.” Angle `.h` stays passthrough
(host `cc` sees libc). Do not parse system headers through this engine.

| Need | Why |
|------|-----|
| `#include` quote + angle policy | Already stage-2. Keep it. |
| `#pragma once` | Every header. |
| Object-like `#define` / include guards | Already stage-2. |
| `#ifdef` / `#ifndef` / `#if` / `#elif` / `#else` / `#endif` | `process.cch`, arena, atomic. |
| `defined`, `!` `&&` `\|\|`, integer compares | `#if defined(__APPLE__)`, `UINTPTR_MAX == UINT64_MAX`. |
| Function-like macros, `__VA_ARGS__`, N-arg dispatch | `cc_file_read`, `cc_channel_send`, `cc_match`. |
| `##` paste | Stdlib is a paste factory: `Name##_init`, `Map_##K##_##V`. |
| `#` stringify | Smaller, but in the same algorithm. |
| `#undef` | vec/map/js/py short-name dance. |
| `#error` / `#warning` / `#line` | Diagnose or pass through; do not invent meaning. |
| Config predicates | `__has_include` (filesystem + sysroot), `__has_feature` / `__has_builtin` as **known config answers**, not a clang plugin API. |

| Not this project | Why |
|------------------|-----|
| `#embed`, modules, `__VA_OPT__` | Unused in-tree. |
| `#include_next` | System-header game. |
| `_Pragma` | One vendor header (`ffc.h`). |
| Mid-expression / mid-declarator `#if` | Declaration-boundary only (fields, file-scope decls, stmt lists). Inside `#ifdef __cplusplus` it is bytes in the opaque span. |
| Expanding libc / clang resources | Host `cc` / clangd. |

`#pragma(@parallel)` is overlay, not C cpp.

### Two projections

Parse builds a **conditional AST** (predicates on nodes). Then:

| Consumer | Mode |
|----------|------|
| Lower / emit | **Preserve.** Print the `#if` tree. Host cpp selects (`_WIN32`). |
| LSP / types | **Evaluate** for one config (editor / `ccc` target / flags). |

Always-expand-and-discard is forbidden: lowering `process.cch` on a Mac
must not bake Darwin-only fields into the `.h`. Host `cpp` as a black
box is the same trap.

Preserve is pinned by `scripts/test_ifdef_passthrough.sh`. Evaluate
expands macros in `#if` the same way `--expand` does, then marks
`live=0/1` with the dead arm still in the tree. Leftover idents are 0
(`#define FOO 0` then `#if FOO` is false). `-Dname` is object-like
`1`; `-Dname=value` uses `value`. An **oracle** flattens one `-D` set
and diffs tokens against host `cpp` on the cparse fixtures.

LSP evaluate picks a config the same way emit’s host would (`ccc`
target, compile flags, editor setting). Grey the inactive arm. Do not
default to “this Mac” and lie about `_WIN32`.

## First merge (step 1 — done)

Standalone host-C library `cc/cparse/` + `out/cc/bin/cparse-dump`.
The parser consumes a **FileTape-shaped** stream (`IDENT` / `NUM` /
`STR` / `CHR` / `PUNCT`, `#` is a punct; `#ifdef` is `#` + `ifdef`).
`cparse_tokens` is the token seam a tool calls. Lex + `\`-newline splice
match the lowerer's lexer so we are not a second grammar. No UFCS rewrite, no emit rewrite. File-scope `T !>(E) name(...)` and
`@typeview` / `@comptime` / `@typehooks` are taped as C-superset
nodes so mixed headers are units; overlay still owns their meaning.
`--expand` runs classic cpp on the token stream.

Fixtures, not whole `process.cch` (that file already has `!>`):

- `tests/cparse/process_fields.c` — `CCProcess`-shaped `#ifdef _WIN32` struct
  + include-guard object-like `#define`
- `tests/cparse/attr_unused.c` — `static void __attribute__((unused)) name(int x) {}`

```bash
make -C cc/cparse
out/cc/bin/cparse-dump --tokens tests/cparse/process_fields.c
sh scripts/test_cparse.sh
```

Preserve reprints both arms and the directive lines. Evaluate
(`-D_WIN32` or not) marks `live=0/1`; the dead arm stays in the tree.
Emit preserve remains `scripts/test_ifdef_passthrough.sh`.

GNU is a **corpus**, not a dialect: mid-declarator `__attribute__`,
plus in-tree `unused` / `constructor` / `always_inline` / `noinline`
(and `typeof` when a header uses it). Grep, then stop.

When a shape moves onto this parser, **delete** the path it replaces.
No silent fallback.

`--expand` evaluates `#if` / `#define` / `#undef`, expands object- and
function-like macros (`#`, `##`, `__VA_ARGS__`, hide-set), and drops
dead arms. Oracle: token dump equals `cc -E -P -undef` on
`tests/cparse/macros.c` and `process_fields.c`.

A caller fills `CpTok` from its own token stream and asks cparse for
every struct member list (`#if`, comma names, nested `struct` / `union`).
Concurrent-C fields (`!>`, `[:]`, `[~`, generics) come back as spans the
caller parses — not flatten-as-C. A C declarator that only *contains*
type-position sugar (`double (*f)(char[:] x, int!>(E) r)`) stays a C
field; the caller rewrites those spellings the same way as function
parameters. Token and flat buffers are heap-sized; overflow fails loud.
Comma declarators (`size_t a, b, c`) flatten to one field node per name.
Nested `struct` / `union` fields: flatten keeps `start`/`end` when the
declarator outgrows `CpFlat.text` (512), and the caller reprints the
span instead of chopping. `struct Tag *name(...)` is a function, not a
tag decl. Oversized field / `#if` flatten text leaves `CpFlat.text`
empty and the span is reprinted. The library builds and ships on its own
(`out/cc/obj/cparse/libcparse.a`, `make -C cc/cparse`). Gate:
`cparse-dump --fields` and `scripts/test_cparse.sh`.

`#elif` is a chained `#if` in the tree (`is_elif`). Preserve prints
the `#elif` line. Evaluate / expand take the first live arm; later
arms stay in the tree (evaluate) or are omitted (expand). `#if` /
`#elif` is a C11 integer constant expression: unary `!` `+` `-` `~`,
`*` `/` `%`, `+` `-`, `<<` `>>`, compares, `==` `!=`, `&` `^` `|`,
`&&` `||`, `?:`, parens, `defined`, numbers. Unused `?:` / `&&` /
`||` arms are not evaluated (`1 ? 1 : 1/0` is fine).

`__has_feature` / `__has_builtin` / `__has_include` are engine
builtins (`defined(__has_*)` is 1). Features: `thread_sanitizer` /
`address_sanitizer` follow `__SANITIZE_THREAD__` /
`__SANITIZE_ADDRESS__`. `__has_include("…")` searches the source
directory; `<…>` searches `-I`, the emit compiler's include roots
(`$CC -print-resource-dir` / `-print-file-name=include`), then
`/usr/include` (and the Apple SDK). `#ifdef` / `#ifndef` treat
`__has_*` as defined, same as `defined(__has_*)`. Unknown `NAME(...)`
after macro expansion fails loud. Parse
accepts that form so preserve of an unexpanded call does not die.

`__has_builtin` is the emit compiler (`CC=`, or the host compiler the
driver picks), not the compiler that built libcparse. Today: one known clang/gcc set.
Later: a table per backend; an unknown name probes that backend
(`$CC -E` on a one-line `#if __has_builtin(name)`) and caches
`(compiler, name)`. Preserve does not evaluate. Not this increment.

LSP diagnostics already go through `ccc --emit-c-only` (same lowerer).

Parse also accepts function-like `#define` (name + `(params) body` on
the node), `#undef` / `#pragma` / `#include` / `#error` / `#warning` /
`#line` as pass-through lines, and `typedef` that is not
`typedef struct {`. Evaluate installs function-like macros the same
way `--expand` does, and `#undef` removes them. `#include` is not
followed (host `cc` sees libc).

Real header units (parse + preserve + evaluate; not host `-E`):
every `cc/include/ccc/*.cch`, `cc/include/ccc/std/*.cch`, and
`cc/include/ccc/script/*.cch`. File-scope
prototypes, incomplete `typedef struct Tag Alias;`, tagged
`struct Tag;` / `struct Tag { … };`, `enum { … };` / `enum Tag { … };`,
file-scope objects (`T name[N] = { … };`, `extern T name;`), `alignas`
on fields, `const Typedef *name` fields, prefix-macro fields
(`CCJ_ALIGNAS Type name`), semicolon-less file-scope macro invokes
(`CC_DECL_RESULT_SPEC(...)`), `T !>(E) name(...)`, `@typeview` /
`@comptime` / `@typehooks` (opaque spans), and `extern "C" {` /
file-scope `}` (often split across `#ifdef __cplusplus`) are nodes.
Oracle stays on fixtures (`tests/cparse/header_unit.h`). `__cplusplus`
is 0 (not defined). `#ifdef __cplusplus` / `#if __cplusplus`
then-arms are an opaque span: preserve reprints the bytes, evaluate
marks `data cxx live=0`, C is not parsed. Nested `#if` inside that
span (mid-expression in a template) is just text. Overlay still owns
the meaning of `!>` / `@` / UFCS; cparse only tapes the span.

`AST_CAP` is 32768.

## Non-goals

- Teaching TCC Concurrent-C
- Skipping `.cch` as a language rule (headers are units when they are
  the file)
- A second IR, `#embed`, modules
- Replacing host-cc or the comptime libtcc executor
- Parsing system headers through this engine
