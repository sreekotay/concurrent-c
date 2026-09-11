# Concurrent-C Compiler Architecture

**Status:** Authoritative architecture for how `.ccs` / `.cch` become C.
**Audience:** anyone changing that path, or proposing a redesign of it.

There is one lowerer. Its sources are `cc/lower/*.cch` / `*.ccs`; its tools
are `out/cc/bin/cclex_cc`, `ccparse_cc`, `cclower_cc` and `ccindex_cc`. The
driver `ccc` (`cc/src/cc_main.c`, C) prepares the unit, runs the lowerer,
and compiles and links what it emits.

The C each construct lowers to is written down in
[`cc/lower/LOWERING.md`](../lower/LOWERING.md). Seeds and the promote
workflow: [`cc/bootstrap/lowerer/README.md`](../bootstrap/lowerer/README.md).

---

## TL;DR

```text
.ccs / .cch bytes
    → driver (cc/src/cc_main.c)
         harvest, prepare and execute the unit's `@comptime` blocks (libtcc),
         splice the fragments back, splice the module's members,
         stage the result under out/.cc-build/clean_comptime/
    → cclower_cc --lower
         lex    → tokens; every token carries file, offset, line, column
         parse  → AST; every node carries the span it came from
         index  → declarations of this unit and of every `.cch` it includes
         lower  → AST to AST, one step per construct family
         print  → C with `#line`, and a `.map` beside it
    → driver
         host cc -c the lowered C, then link with the runtime and @link libs
```

`cclower_cc --lower` also turns a quoted `#include "x.cch"` into
`#include <rel/x.h>` and lowers that face in header mode, transitively, under
`--root` / `--h-root`. Nothing on this path rewrites text to make a construct
parse.

Three layers:

| Layer | Role | Reparses |
|-------|------|----------|
| **Tokens** | One lexer for C plus the CC tokens; comments and blank lines are tokens | 0 |
| **AST + index** | Recursive-descent parse; every fact about a name comes from a declaration | 0 |
| **Print + host** | One C product with `#line` and a source map; driver-side `cc -c` and link | 0 |

---

## 1. Reader's map

| If you want to … | Read |
|------------------|------|
| Why this shape | §2 (constraints) + §3 (layers) + §4 (ADRs) |
| What the pipeline does today, and where it is fragile | [`docs/compiler_internals.md`](../../docs/compiler_internals.md) |
| The C a construct lowers to | [`cc/lower/LOWERING.md`](../lower/LOWERING.md) |
| What each lowering step owns | the step list at the top of [`cc/lower/lower.cch`](../lower/lower.cch) |
| Bootstrap / promote | [bootstrap README](../bootstrap/lowerer/README.md) |
| What the index still cannot answer from a declaration | [`cc/lower/INDEX_GAPS.md`](../lower/INDEX_GAPS.md) |
| `@grammar` / wire SERDES | [`spec/cc_serdes.md`](../../spec/cc_serdes.md) |

---

## 1.5 Design principle: fail loudly

Same gradient as the rest of the project:

```text
runtime-at-a-distance → runtime-at-the-site → link → compile →
apply/patch → impossible by construction
```

Quiet success that means "couldn't" is forbidden (see root `CLAUDE.md`).
Concrete corollaries here:

- A construct a step cannot lower is a diagnostic at its position, never a
  node left for the host compiler to trip over.
- A UFCS miss on a bound receiver is diagnosed; no callee is invented.
- Steps after an erroring step still run, so one file reports every error it
  has, and the driver refuses to print C when the sink has errors.
- Seed and ODR bugs must fail the cold `make` path, not only the machine
  that generated the seed.
- Driver options the lowerer cannot honor must **refuse** or be handled in
  `ccc` before exec, never silently dropped. `build.cc` / `-D` / dumps /
  `--compile` are handled in the driver and forwarded as host flags.

---

## 2. Constraints

### C1. We emit C; we do not own a C compiler

Host `cc` (and libtcc for comptime) compiles and links. The lowerer's job is
a **readable C product** with `#line` back to user source, not an optimizing
IR and not a second C frontend.

**Therefore:** the AST models the whole surface the corpus writes, C
included. Nothing is copied through as text in the hope that the host
compiler accepts it.

### C2. CC surface is not a C lexer vocabulary

`T[:]`, `T!>(E)`, `int[~4 >]`, `() => {…}`, `!>`, `@destroy` cannot be
honestly tokenized by a C lexer.

**Therefore:** one lexer owns C plus the CC tokens, including backtick
templates with `${` nesting, and the parser builds structure from those
tokens. Comments and blank lines are tokens so the printer can replay them.

### C3. Diagnostics and safety need user coordinates

Errors must say `path:line:col` on the original `.ccs` / `.cch`. Provenance,
move, channel and unwrap checks need enough structure to refuse unprovable
cases loudly.

**Therefore:** every token and every node carries its span; a node a step
creates copies the span of the node it replaces. The printer emits `#line`
from those spans and records, per emitted line, the user file, line and
column in a `.map` beside the C.

### C4. The lowerer must bootstrap from a C compiler alone

A fresh clone with only host `cc` (plus patched `libtcc.a` for the driver)
must produce working tools without already having them.

**Therefore:** committed seeds under
`cc/bootstrap/lowerer/MAJOR.MINOR.PATCH-SEED/`, with `last-good` naming the
running pin (`0.4.0-400`). A pin holds the lowered C of the four tools, the
C of module `lower` (`lower_cch.c`) and the faces that C includes; stage zero
of `make -C cc` host-compiles it. Source of truth is only `cc/lower/*.cch` /
`*.ccs`: every behaviour fix is edited there first, and a seed is
regenerate-only. `make -C cc lower-cc` rebuilds the tools from source with
the seeded ones, `./scripts/lowerer_selfhost.sh` is the fixed-point gate, and
`./scripts/ship_seed.sh --promote` freezes a new pin and flips `last-good`.
Cold rebuild on a second platform is part of the gate. The two 0.3 seeds that
remain are frozen C, kept so that a unit whose `#!ccc … version=` pin selects
the 0.3 line is lowered by the seed it names. `install` ships
`$PREFIX/bin/ccc` and the four tools beside it.

### C5. Comptime is a seam, not the lowerer

`@comptime` / `@emit` / factory instantiation run in the driver
(`cc/src/comptime/`, archived as `libshadow_comptime.a`, with libtcc where
needed): harvest, prepare, execute, splice. The lowerer reads the staged
unit that pipeline produced. A block that only registers type hooks stays in
the stage, because the index reads `cc_type_register(...)` off it as it reads
`@typehooks`, and the lowering drops the block.

**Therefore:** the lex/parse/index/lower/print spine does not become a
comptime VM. Remaining holes are seam completeness, not another peel.

**TCC sees only C.** Attributes (`as:`, …) are AST facts used while lowering;
the product and the comptime session buffers must not carry them.

---

## 3. Layers

### L1 — Tokens

`cc/lower/lex.cch` lexes bytes into a tape of tokens. Each token carries
`file_id`, offset, line and column; comments, blank lines and `#line` are
tokens, so positions rebase and the printer can replay trivia.

### L2 — AST and index

`cc/lower/parse_*.cch` is a recursive-descent parser for C plus the CC forms,
producing a variant AST with a span on every node. Errors carry spans and the
parser recovers at statement boundaries, so one file reports every error.

`cc/lower/index_impl.cch` holds the declaration index: functions, types,
Result specs, UFCS registrations and attributes, from this unit and from
every `.cch` it includes, parsed by the same parser. Whether `f` returns a
Result, whether `T` has a `destroy`, which method set `x.f()` resolves
against, and whether a call may be discarded are all answered from
declarations, not from a table of names in the compiler.

### L3 — Lowering, print, host

Lowering is AST to AST. `CcLowerer_lower_unit` runs one step per construct
family, in order: results, cleanup, includes, own, generics, slices, slice
arguments, strings, `as:` arguments, string switch, closures, create, for-in,
deadline, parallel, async, channels, variants, UFCS. Each step rewrites nodes
in place and copies the span of the node it replaces.

The printer walks the C nodes and writes lines, emitting `#line` at every
file or line change and recording the source map. Long constructs print one
operand per line.

The driver then compiles the product with host `cc` and links it with the
runtime object, the module objects the unit reaches, and the `@link` libs.
It also owns paths and modes, `build.cc`, unit kind and `version=` pin
resolution, cache keys, and the runtime object.

---

## 4. ADRs

### ADR-S1: One structured pass, no text rewriting of CC surface

**Decision:** The lowerer parses the language into an AST and prints C from
it. A shape the parser does not model is a diagnostic, not a span copied
through.
**Rejected:** A whitelist parser with opaque spans; a post-parse expression
mangler; teaching the host compiler to parse CC.
**Why:** C1 to C3. A pass that can bail out quietly cannot be trusted with
positions, and a seed of its output cannot be trusted at all.

### ADR-S2: Declarations are the only table

**Decision:** Every fact about a name comes from its declaration through the
index: attributes, type hooks, `CC_DECL_*` macros, `.rules` files.
**Rejected:** Name tables in the compiler for print families, destroy
callees, map key hashes or method sets.
**Why:** A user type with the same declarations must get the same treatment.
What the lowerer still carries in code is the work list in
`cc/lower/INDEX_GAPS.md`.

### ADR-S3: Spans, not coordinates recomputed later

**Decision:** Spans stick from lex to print; scaffolds that expand one
statement into many pin every generated line to that statement.
**Rejected:** Re-scanning emitted text for a needle to place a diagnostic.
**Why:** C3, and a host diagnostic that comes back through the source map.

### ADR-S4: Driver / lowerer split and a frozen seed

**Decision:** `ccc` (C) prepares the unit, runs the tools, compiles and
links. The tools are Concurrent-C, bootstrapped from committed lowered C.
Source of truth is `cc/lower`; `last-good` is the cold-start seed.
**Rejected:** Shipping only a prebuilt binary; requiring the tools to build
themselves with no seed; hand-editing a pin to land a fix.
**Why:** C4. Promote stays a human-gated snapshot behind the selfhost gate.

### ADR-S5: Host `cc` is the product compiler; TCC is specialized

**Decision:** The default path is lower, then host `cc` plus runtime. TCC
serves comptime and driver parse hooks, not the everyday lower-and-run path.
**Rejected:** Making libtcc the succession metric.

### ADR-S6: One lowerer

**Decision:** One lowering path ships. `--lowerer=shadow` and
`CC_LOWERER=shadow` are errors. A unit pinned to the 0.3 line is lowered by
that line's frozen seed, which `ccc` builds on demand.
**Rejected:** Two fronts side by side; silent fallback between them.

---

## 5. Non-goals

- C23 `#embed` / modules / parsing system headers through this path
  (classic project-unit cpp is [`docs/c-parser.md`](../../docs/c-parser.md))
- A general compiler IR or SSA-style mid-end inside the lowerer
- Reviving `cc/src/visitor/pass_*.c` scanners as a product path
- Quietly accepting driver flags the lowerer ignores

---

## 6. What redesign would actually help

In priority order (fail mass × language value):

1. Richer safety / points-to so unprovable move, channel and unwrap cases
   refuse loudly rather than compile.
2. Closing the comptime and factory seam (dylib factories, type-register and
   UFCS comptime, header-local `static_map`) without pulling TCC into the
   lowering steps.
3. A real `@async` / `@await` state machine in place of the poll wrapper.
4. Emptying `cc/lower/INDEX_GAPS.md`: each entry is a declaration form that
   replaces a name the lowerer carries in code.

A redesign that reintroduces text rewriting of CC surface fights C1 to C3.

---

## 7. Naming

| Name | Means |
|------|--------|
| **the lowerer** | This architecture: tokens → AST + index → lowering steps → print |
| **`cclower_cc`** | The tool that lowers a unit; `cclex_cc`, `ccparse_cc` and `ccindex_cc` are its siblings |
| **`cc/lower/`** | Source tree for all four (`.ccs` / `.cch`) |
| **`spec/cc_serdes.md`** | `@grammar` engines and **wire** serialization, unrelated to this path; the C23 tok+syn experiment lives under `examples/serdes/c23/` |

**SERDES** means only grammar and wire serialization (`spec/cc_serdes.md`,
`examples/serdes/{json,resp}`).
