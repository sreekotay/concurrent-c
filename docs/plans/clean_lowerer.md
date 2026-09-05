# Clean lowerer

A replacement for the lowering pass: the same language, the same runtime
and stdlib, the same artifacts, built as one structured pass that keeps
source lines, has no text rewriting, no fixed buffers, no tables of names,
and one diagnostic sink. This document is the design and the milestone
plan. The audit it answers is [`compiler_internals.md`](../compiler_internals.md).

## 1. What must hold

1. **Behavioral parity.** Every test under `tests/` that passes today
   passes with the new lowerer, every example compiles, and the real
   projects build. Parity is on program behaviour, not on identical C.
2. **Source lines survive.** Every emitted line maps to one user line.
   Synthetic scaffolds are pinned to the statement they expand. Host
   compiler diagnostics come back with the user's file, line and column,
   and with the user's spelling of the construct, not the lowered name.
3. **No text rewriting.** Lowering reads an AST and prints. A construct
   the parser does not understand is a diagnosed error at its position,
   never a span copied through in the hope that the host compiler accepts
   it.
4. **No fixed buffers for user-sized text.** Names, spans, expressions
   and emitted text live in growable arena-backed builders. Numeric caps
   that remain are named in one header and diagnosed when hit.
5. **Tables live in user space.** Whatever the lowerer needs to know about
   a function or type comes from its declaration: an attribute, a type
   hook, a `CC_DECL_*` macro, or a `.rules` file. The lowerer ships with
   no list of stdlib names.
6. **One TU model, stated once.** The user TU, the runtime unity TU, the
   lowered headers, the comptime seam and the cache keys are defined in
   one place and the same code path serves `.ccs`, `.cch` and `.shcc`.
7. **Builds from a C compiler alone.** No seed, no sed-patched snapshot,
   no promote step.

## 2. The language surface

From the spec's quick reference and the constructs the current lowerer
models. Each row says how the clean lowerer handles it and which runtime
entry points it targets (the names come from what the current lowerer
emits; the runtime does not change).

### 2.1 Plain C

Declarations with declarators, statements, expressions with C precedence,
typedefs, structs, enums, function pointers, `switch`, labels, `goto`,
preprocessor lines. The lowerer parses all of it; it does not type-check
beyond what lowering needs. This is the part that makes "a whitelist plus
raw spans" unnecessary: a real recursive-descent C parser for C11 minus
nothing the stdlib or the tests use is about 4k lines.

### 2.2 Results and error flow

| Construct | Lowering | Targets |
|---|---|---|
| `T!>(E)` / `T?>(E)` types | `CCResult_T_E`, with the spec emitted once per TU | `CC_DECL_RESULT_SPEC`, `CC_DECL_RESULT_SPEC_VOID` |
| `cc_ok(...)`, `cc_err(...)` inferred forms | typed constructors from the enclosing function's declared Result | `cc_ok_T_E`, `cc_err_T_E` |
| `expr !>;` | temp, test, dispatch to the in-scope handler for `E`, value | `__cc_uw_is_err`, `__cc_uw_value`, `__cc_uw_err_at`, `cc_rt_diag_record_unwrap_site` |
| `expr !> body`, `expr !>(e) body` | same, with the body inline; divergence checked at expression position | |
| `expr ?> default`, `?>(e) expr` | conditional expression | |
| `@errhandler(E e) stmt` | block-scoped handler table keyed by `E`; hoisted label when the handler diverges | `cc_error_exit` is the user's spelling, not a default |
| `@err(e);` | forward to the handler for `E` | |
| `r.is_ok()` etc. | Result UFCS from the spec table | |
| `@destroy` on an unwrap | destroy registration from the type hook | `cc_arena_destroy` etc. via hooks, never a compiler list |
| `@defer`, `@defer(ok)`, `@defer(err)`, `@cancel_defer` | scope stack with per-exit emission; soft returns | |

### 2.3 Strings, slices, print

| Construct | Lowering | Targets |
|---|---|---|
| `@string(\`…\`, arena)` | one push per literal or slot, on their own lines | `cc_string_new`, `cc_string_push_buffer`, `cc__string_slot_push` |
| `@string(\`…\`, @scratch)` / `@scratch(N)` | one function-scoped stack arena; call-local checkpoint per consuming statement | `cc_arena_stack`, `cc_arena_checkpoint_local`, `cc_arena_restore_local` |
| `@string(\`…\`)` (no arena) | bounded stack buffer, `char[:]` borrow | |
| `char[:]`, `char[4:]`, `char[:!]`, `char[:0]` | `CCSlice` family types; refinements are safety facts | |
| `@slice("…")` | sentinel slice literal | |
| `println` family | ordinary functions declared in `stdio.cch`; the optional-Result discard rule comes from their declared type | `cc_println`, `cc_eprintln`, `cc_fprintln` |

### 2.4 UFCS and generics

| Construct | Lowering | Targets |
|---|---|---|
| `x.f(a)` | resolve receiver type from the declaration index; look up `f` in the type's method set: type hooks, `CC_*_DECL_UFCS` registrations, then the `Type_f` / `cc_<snake>_f` composition, verified against a declaration | user declarations |
| `Name::[args]` | canonical mangling from `GENERIC_MANGLING.md`; instantiation through the registered factory | `CC_GENERIC_FACTORY` |
| `Type.fn(...)` | `Type_fn(...)` | |
| `@typehooks on T {…}`, `@typeview on T {…}` | parsed as declarations into the method set and face table; `.ufcs` handlers compile through the comptime seam | `cc_type_register` |
| `@variant` | tagged union types, `case .arm(bind)`, field-path switch | |

### 2.5 Concurrency

| Construct | Lowering | Targets |
|---|---|---|
| `() => { … }`, `[&x]` captures, `@unsafe` | closure struct plus thunk; capture inference from the AST's free variables against the scope, no keyword list | `cc_closure0_make` |
| `n.spawn(...)`, `send_task` | closure make plus spawn | `cc_fiber_spawn_closure0`, `cc_channel_send_task` |
| `T[~n 1:1 >]` channel types, `channel_pair`, `send`/`recv`/`close` | channel handle types; methods from the registered UFCS set | `cc_channel_pair_create_named`, `cc_channel_*` |
| `@parallel` in all its forms, `@serial`, `@stage`, `worker`, `cache` | arm thunks and a join | `cc_parallel_spawn_admit`, `cc_parallel_join`, `cc_parallel_honor`, `cc_parallel_deny_*` |
| `@with_deadline(...)` [`as h`] | push/pop around the block | |
| `@async`, `@await`, `@blocking`, `@nonblocking`, `@latency_sensitive` | poll-task body plus wrapper, as today; the state machine is a later milestone | `cc_task_intptr_make_poll_ex`, `cc_block_on`, `cc_chan_result_from_errno` |
| `CCExclusive`, `CCTurnstile` | library types with UFCS; no lowerer knowledge | |

### 2.6 Compile time and TU

| Construct | Lowering |
|---|---|
| `@comptime {}`, `@comptime if`, `@comptime(expr)`, `@comptime fn` | the existing executor seam (libtcc in-process), fed AST spans instead of text; the run/skip decision from a declaration attribute, not a verb list |
| `@grammar(engine) Name {…}` | the existing engines, invoked on a parsed fenced body |
| `#!ccc` unit header, `#pragma(@prelude)`, `#pragma(@linenumbers)`, `#pragma(@per_tu)`, `#pragma(@parallel)` | TU-level flags read once |
| `.shcc` scripts | the same parser with the script prelude and synthetic `main` |
| `.cch` headers | the same parser in header mode: strip bodies of `static inline` where required, emit `#pragma once`, keep declarations |

Everything else in the spec (`unsafe {}`, `adopt`, atomics, `for in` walks,
`@scoped`, doc comments) is either plain C after a one-node rewrite or a
safety check on the AST.

## 3. Architecture

```text
bytes ──lex──▶ tokens (CC-aware; every token carries file_id, offset, line, col)
       ──parse──▶ AST (typed nodes, expression trees, spans on every node)
       ──index──▶ declaration index (this TU + included .cch, parsed the same way)
       ──check──▶ safety diagnostics (move, channel, unwrap, scratch escape)
       ──lower──▶ AST → AST (CC nodes replaced by C nodes + runtime calls)
       ──print──▶ emit.c + source map (emit line → user file, line, col)
       ──host──▶ cc -c, link; host diagnostics remapped through the source map
```

**Lexer.** One token grammar for C plus the CC tokens (`!>`, `?>`, `=>`,
`::[`, `[:`, `@word`, backtick templates with `${` nesting). Comments and
blank lines are tokens so the printer can replay them. `#line` in input is
a token that rebases positions.

**Parser.** Recursive descent, C11 subset plus CC forms, producing a
tagged-union AST where each kind has its own struct. Expressions are
trees; UFCS calls, unwraps, templates and closures are expression nodes.
Errors are collected with spans; the parser recovers at statement
boundaries so one file reports every error.

**Declaration index.** Functions, types, Result specs, UFCS registrations
and attributes, from this TU and from every included `.cch`, parsed with
the same parser. Replaces every name table in the audit: whether `f`
returns a Result, whether `T` has a `destroy`, which method set `x.f()`
resolves against, which argument of `cc_arena_alloc` is the arena, and
whether `println` may be discarded are all answered from declarations.
Attributes the stdlib needs and does not have yet are added to the
headers, not the lowerer.

**Lowering.** AST to AST. Each CC node kind has one lowering function that
returns C nodes carrying the original span. Scaffolds that expand one
statement into many mark every generated statement with the statement's
span, so the printer pins them. Nothing in this phase touches text.

**Printer.** Walks C nodes and writes lines. It emits `#line` at every
file or line change and, for each emitted line, records `(user file, user
line, column offset)` in a source map beside `emit.c`. Long constructs are
printed one operand per line; there are no statement-expression
one-liners. Comments and blank lines from the tape are replayed.

**Diagnostics.** One sink. Every message has a span in user coordinates
and a construct name in the user's spelling. Host compiler output is
parsed, mapped through the source map to user coordinates, and lowered
identifiers are demangled (`cc_string_len` on a `CCString` receiver prints
as `.len()`) from the declaration index. Suggestions naming identifiers the
user cannot write are dropped.

**TU model.** Unchanged artifacts: `emit.c` and `tu.o` per unit, the
runtime unity TU per flag variant, lowered headers, comptime hook dylibs.
The lowered-header path is this parser in header mode, so the fifteen-pass
text pipeline goes. Cache keys fold the content of every input the
lowerer read, including the compiler binary and the runtime objects by
content, never by name.

**Implementation language: Concurrent-C, written the CC way.** The
lowerer is a CC program (`cc/lower/*.ccs`, faces in `cc/lower/*.cch`),
in the idiom of [`docs/the-cc-way.md`](../the-cc-way.md):

- **Arenas name lifetimes.** One arena per unit holds the tape, the AST
  and the index for that unit; it dies with the unit. Each pass takes the
  arena its products must outlive as its last parameter. Small, short
  results (a mangled name, a candidate list, one `#line` string) come
  from a `cc_arena_stack(name, N)` in the function that needs them, which
  overflows to the heap without ceremony when a name is longer than
  planned: no fixed `char[N]`, ever.
- **`@variant` for every node.** Tokens, types, expressions, statements
  and declarations are tagged data with one arm per kind, recursive arms
  through pointers; `@switch` with `case .arm(bind):` is the only way a
  pass reads them, so a new kind is a compile error in every pass that
  does not handle it.
- **Containers, not linked lists.** `Vec::[T]` for children, `Map::[CCSlice, T]`
  for the index; `@for` to walk them.
- **`@string` templates for every emitted string.** Mangled names, `#line`
  directives, diagnostics and the printed C are templates into the arena
  that owns the product, never `snprintf` into a buffer.
- **Results for everything that can fail.** `T!>(CcDiag)` from every
  parse, resolve and lower step; `!>` forwards, `?>` picks a recovery;
  no function returns `NULL` or `-1` to mean both "nothing" and "could
  not". A pass that cannot continue says so at the position that caused
  it.
- **UFCS for the API.** `tok.text()`, `unit.parse(a)`, `ix.method(recv, name)`,
  `node.span()`: methods are declared as `Type_method(Type*, ..., arena)`
  and read as calls on the value.
- **Typeviews for clarity.** Each pass sees the face it needs
  (`@typeview Read on CcUnit { r: *; }` for the printer and the checks;
  the lowering pass gets the writable face), and the AST's `as:` faces
  let a `CcExpr*` be used where the span or the kind is all that matters.

The seed question the C11 argument raised is answered the other way: a
lowerer that pins every line and never clips is what makes a committed
pre-lowered seed trustworthy. Until the clean lowerer lowers itself it is
built by the current compiler, in the shapes that compiler accepts
(`stress/break/break_ast_cc_way_smoke.ccs` is the list, each shape it
avoids pinned by a sibling `break_*` test); once it lowers its own sources
its emitted C is committed as the seed and the current lowerer's seed
retires.

**The C11 spine as reference.** A C11 lexer, parser, printer and index
were written first against the 2000-file corpus to fix the grammar and the
identity round-trip (`cc/lower/lex.c`, `parse.c`, `print.c`, `index.c`,
each with a corpus gate: `cclex --roundtrip`, `ccparse`, `cclower
--identity`, `ccindex`). They are the algorithmic reference and the gate
the CC port must pass; they are not the shipped lowerer and are deleted
when the port passes their gates.

**Size.** The essential lowerer is small: lexer 1k, parser 4k to 5k,
index 1k, lowering 5k to 6k, printer 1k, diagnostics 1k, driver glue 1k.
Fifteen to twenty thousand lines against about 150k today; the CC forms
(variants, templates, Results, `@for`) take a third off the C11 count.

## 4. Compatibility strategy

- **A flag, not a fork.** `ccc --lowerer=clean` (and `CC_LOWERER`) selects
  the new pass; the default stays the current one until parity. Both
  produce the same artifacts in the same cache layout.
- **Differential runner.** `scripts/lowerer_diff.sh` runs `cc_test` with
  both lowerers and reports, per test, pass/pass, pass/fail, fail/pass,
  with the construct list each test uses so progress is by construct.
- **Golden emit is not the oracle.** Two lowerers may print different C;
  the test outcome and the host diagnostics are the oracles.
- **`stress/break/` grows first.** Every fragility in the audit becomes
  a program there before the code that removes it lands: the ten
  diagnostic probes with column-pinned `.compile_err`, a 3 KiB static
  function body, 129 UFCS sites in one expression, 200-byte type names,
  forty `@string` sites in one function, a `@grammar` inside a template
  literal, `.foo(` inside a comment inside a switch body, a user file
  under `/tmp`, nested `!>` in `@parallel` in `@scratch`.
- **Switch-over gate.** Full `cc_test`, examples, real projects, the
  shadow architectural smokes, and `stress/break` all green on the clean
  lowerer; then it becomes the default and the old tree is deleted in one
  commit.

## 5. Milestones

Each milestone is a commit series with its `stress/break` coverage and a
differential report.

- **M0 Corpus and harness.** `stress/break/` wired into `cc_test`, the
  differential runner, the `--lowerer` flag plumbed through the driver
  with the clean lowerer as a stub that refuses every unit.
- **M1 Identity.** Lexer, parser and printer for the C subset plus CC
  tokens. The clean lowerer parses every test, example and stdlib header
  and prints it back; the printed C of pure-C tests compiles and passes.
  This proves the parser covers the C surface the corpus uses.
- **M2 Results.** Result types and specs, `cc_ok`/`cc_err`, `!>` in all
  forms, `?>`, `@errhandler`, `@err`, `@destroy`, `@defer`. This is the
  highest-count construct family and the worst diagnostics today.
- **M3 UFCS.** The declaration index, method-set resolution, type hooks,
  `Type.fn`, generics and factories, `@variant`. A generic instance's C
  fragment is the family factory's `@emit` template, filled from the type
  arguments (`${mangled}`, `${arg(i)}`, `${arg_mangled(i)}`); the stdlib
  factories are such pure templates, with the C preprocessor making any
  remaining choice, so the runtime and stdlib hold no privilege. A factory
  whose body computes part of the fragment needs the body run, which is
  M7's comptime seam; until then such an instance is a diagnostic at the
  site, never a guessed fragment.
- **M4 Strings.** Templates, `@scratch`, slices, print family, `@slice`.
- **M5 Concurrency.** Closures and captures, spawn, channels, `@parallel`
  in all forms, deadlines, exclusive and turnstile.
- **M6 Async.** Poll-task bodies as today; the state machine after parity.
- **M7 Compile time and TU.** Comptime seam on AST spans, `@grammar`,
  header mode, `.shcc`, unit headers and pragmas.
- **M8 Self-lowering and switch-over.** The gate is a fixed point: the
  current compiler builds the clean lowerer; that binary lowers its own
  sources; the host compiler builds the result; that second binary lowers
  the same sources again and the two products are byte-identical. Only the
  second product becomes the seed. With that, `cc_test`, examples, real
  projects and `stress/break` green on the clean lowerer as the default,
  delete `cc/shadow`, the text engine, the old seeds and promote scripts,
  and the four cleanup documents. Nothing merges to the default branch
  as the shipped lowerer before this gate passes.

M0 and M1 carry no risk to the shipping compiler and are where work
starts.

## 6. Working with the branch

- `make -C cc` builds the C11 reference tools (`cclex`, `ccparse`,
  `cclower`, `ccindex`); `make -C cc lower-cc` builds the Concurrent-C ones
  (`cclex_cc`, `ccparse_cc`, `cclower_cc`) with the current compiler.
- Gates, each byte-for-byte against the reference over the corpus
  (`tests examples cc/include stress real_projects`): `cclex_cc --roundtrip`
  and `--dump`; `ccparse_cc --check` and `--dump` with
  `--known-types` from `ccparse_cc --collect-types` over `cc/include`;
  `cclower_cc --identity` and `--print` (the C and the `.map`).
- `ccc --lowerer=clean FILE` (or `CC_LOWERER=clean`) lowers through
  `cclower_cc --lower` into `out/.cc-build/clean/` and finishes in the
  driver's raw-C path. A quoted `#include "x.cch"` becomes
  `#include <rel/x.h>` (relative to the repository root) and the header is
  lowered in header mode to `out/.cc-build/clean/<rel>.h`, transitively;
  the tool takes the roots as `--root DIR --h-root DIR`.
  `scripts/lowerer_diff.sh [--filter S]` runs `cc_test` on both lowerers
  and prints the pass/fail matrix. The pass-on-shadow, fail-on-clean rows
  are the work list of the current milestone.
- Every shape the current compiler refuses in the lowerer's own sources is
  a `stress/break` entry with an `.xfail`; the port avoids it until the
  clean lowerer lands the fix, then the marker goes.
