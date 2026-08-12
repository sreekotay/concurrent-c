# Comptime Capability Model — "just CC, run early"

**Status:** proposal (2026-05-30)
**Supersedes:** the capability/restriction half of spec §14.8 (the `@comptime`
"forbidden" list). Does **not** change the emission machinery in
`COMPTIME_INSTANTIATION_SEAM.md`; it governs *what comptime code is allowed to
do*, that doc governs *how its output is spliced*.

---

## 0. One principle

> **Comptime is CC executed at compile time — the same language, the same
> functions. Execution is unbounded; the only invariant is that comptime
> *emits* code, it never *rewrites* it.**

Everything below follows from refusing to make `@comptime` a separate dialect.
The moment comptime grows its own capability tiers, effect markers, or blessed
I/O intrinsics, userspace has a *second* language to reason about — and the cost
of metaprogramming stops being "it's just CC" and becomes "it's CC plus a rules
appendix." The narrowest interaction surface is the one with nothing new in it.

This is, precisely: **Jai's execution freedom (the whole language runs at
compile time) + CC's additive-only program effect (no AST rewrite).**

---

## 1. What changes vs current §14.8

Current §14.8 forbids, at compile time: I/O, heap allocation (incl. arena),
channels/tasks/threads, atomics/mutex, calling non-`@comptime` functions,
runtime-local addresses, UB.

**Revised:** those capability bans are **lifted**. Comptime may:

- compute freely — the full language, real execution (already true of the E0
  libtcc executor; the "constant-expression decidable" framing is retired),
- **allocate** (heap and arena) — you need it to build intermediate values,
- call **ordinary CC functions**, including the **normal file API**, exactly as
  at runtime. There is no `@embedFile`-style special form and no `@comptime(...)`
  effect annotation. You read a file the same way you read one at runtime.

The one thing that remains a hard rule is **not** a capability restriction — see
§3. It is the additive-only invariant, which is a *program-semantics* rule, not
an *effect* rule.

### Spec §14.8 rewrite sketch

> **§14.8 (revised).** `@comptime` code is ordinary CC evaluated during
> compilation. It may compute, allocate, and call ordinary functions including
> file and other host APIs. It may **not** modify, replace, or otherwise rewrite
> existing declarations or program text; its only effect on the program is to
> **emit** new code through the §14.9 emission API. Channels and tasks are
> runtime constructs; using them at compile time is permitted only insofar as the
> evaluator can execute them and is otherwise a normal evaluation error — not a
> special-cased prohibition.

---

## 2. Dependency tracking — explicit, via pragma

Allowing comptime file reads raises one correctness question: if a read file
changes, the build must rebuild. This is declared **explicitly, and only
explicitly**:

```c
#pragma cc_depends("schema.json")
```

**Why explicit and not automatic.** A normal file call (`fread`, etc.) is a
*normal file call* — at compile time it behaves exactly as it does at runtime,
with no hidden effect on the build graph. Auto-observing reads would make file
functions **non-uniform** between comptime and runtime (secretly registering a
build dependency in one and not the other), which breaks the whole "comptime is
just CC, same functions, same behavior" principle. There is no expectation that
`fread` tracks anything — so it doesn't. The dependency is a separate, visible
statement.

**Tradeoff (expected, not a surprise).** If you read a file and don't declare it,
a change to that file won't trigger a rebuild — exactly as a custom build step's
inputs have always needed explicit Makefile dependencies. Tracking is always and
only the pragma; file functions never carry build semantics.

### 2a. `#pragma cc_depends` — landed (2026-05-30)

Implemented in the driver (`cc/src/cc_main.c`). The path is resolved relative to
the source file's directory; the dependency's **content** (FNV-1a, missing files
fold a stable sentinel) is folded into the **emit cache key** (`emit_key`) for
both the single-file and `build.cc`-target compile paths. Editing a declared
file therefore re-triggers CC emission even when the `.ccs` mtime is unchanged;
an undeclared read stays untracked (the §2 tradeoff).

The directive is **stripped before lowering** (`cc__blank_pragma_cc_depends` in
`build_parse_input.c`, length-preserving) so it never reaches the emitted C and
never trips the host compiler's unknown-pragma warning. It carries no codegen
meaning — its only effect is on the build graph.

Proof: `tests/comptime_depends_smoke` (pragma + comptime file read + `@emit`,
against a committed fixture) plus a manual cache check — unchanged inputs reuse
the cached `.c`, editing the declared file re-emits.

**Not yet:** computed-path deps (§6.1) — only a string-literal path is scanned;
and `--hermetic` (§4).

---

## 3. The invariant that does *not* relax: additive-only

Even with full execution and unrestricted I/O, comptime's **only** effect on the
*program* is to emit new code via `cc_emit_*`. It does not expose an AST, cannot
rewrite or replace existing declarations, and cannot instrument or transform code
you wrote.

This is the line that preserves local reasoning: you can read a declaration and
know what it means, because nothing mutates it behind your back. Effects (I/O,
allocation, nondeterminism) are about the **build environment**; they are
orthogonal to, and never escalate into, power over your **source**. Lifting the
capability bans (§1) does not move this line.

(This is also why CC declines the Jai/Lisp "self-modifying source" power: it is
the C++-class mistake — non-local, invisible meaning — relocated to compile time.
See `COMPTIME_INSTANTIATION_SEAM.md` D6 for the emit/factory model that stays
strictly additive.)

---

## 4. Reproducibility & hermetic builds — opt-in, at the tool layer

"Just CC" means comptime *can* read the clock, the network, or the environment,
so builds are not guaranteed reproducible by construction. There is deliberately
**no in-language marker** for this (a marker would be the dialect creep §0
rejects).

Instead, reproducibility is an **opt-in build mode**, e.g. `--hermetic`, that
sandboxes the comptime executor (block network, clock, randomness, and ambient
env reads; allow tracked file reads). CI and release builds turn it on; everyday
builds need not. The *capability* of a hermetic build survives; it simply lives
in the build tool, not in the language surface.

---

## 5. Implementation consequences (honest costs)

- **Executor hosts ~the full CC runtime.** "Comptime is just CC" means ordinary
  functions must resolve when run in the libtcc evaluator — a bigger lift than
  injecting a curated host API (`cc_emit_*` / `cc_reflect_*`). This is the real
  price of the cleaner model.

  **Landed (2026-05-30) — first step toward §1.** The libtcc executor TU already
  links the system libc, so ordinary C/libc calls resolve and run at compile time
  — the capability bans of §1 are effectively lifted for the libc surface today.
  The generated prelude (`emit_tpl_prelude.inc.h`, via
  `tools/gen_emit_tpl_prelude.sh`) now `#include`s `<stddef.h> <stdio.h>
  <stdarg.h> <string.h> <stdint.h> <stdlib.h> <stdbool.h> <ctype.h>`, so
  `fopen`/`fgets`/`fwrite`/`sscanf`, `malloc`/`free`/`getenv`/`atoi`,
  `tolower`/`isalpha`, etc. all work inside `@comptime` with no special form and
  no effect annotation — exactly as §0/§1 require. Proof:
  `tests/comptime_fileio_smoke` reads a file at compile time and bakes the result
  in (via `@emit` templates). Still outstanding for "~the full CC runtime":
  resolving arbitrary *user* CC functions (not just libc) in the evaluator, and
  the `#pragma cc_depends` rebuild wiring below.
- **`#pragma cc_depends`.** Parse the pragma and feed its path (+ content hash)
  into the existing TU dependency set — the same set `#include` headers populate
  — and wire into the incremental/rebuild machinery. Plain comptime file calls
  are *not* hooked: they carry no build semantics (§2).
- **Tasks/channels at compile time.** Inherently runtime ideas. Let them
  work-if-the-executor-can or fail as ordinary evaluation errors. Do **not** add
  a special prohibition — that would reintroduce exactly the dialect this model
  removes.

---

## 6. Open decisions

1. **Computed-path deps:** a literal `#pragma cc_depends("x")` cannot name a path
   the comptime code *computed*. If that case matters, a comptime-callable
   intrinsic form (`cc_depends(path)`) may be needed alongside the pragma;
   otherwise computed-path reads are simply untracked (and that is the author's
   responsibility, consistent with §2).
2. **`--hermetic` scope:** exactly which ambient sources it sandboxes; later
   milestone.

---

## 7a. Implementation roadmap — the two axes of "just CC"

"Comptime is just CC" (§0) decomposes into **two independent axes** with very
different costs. The deciding mechanic for both is **pass ordering**: comptime
executes *early*, before CC→C lowering.

```
build_parse_input.c:
  48  cc_comptime_prepare_source(...)   // @comptime for/if + STRING TEMPLATES lowered here
  51  cc_emit_plan_exec_comptime_blocks // <-- executor runs (sees C, not CC)
  72  cc_preprocess_canonicalize(...)   // closures/containers/channels/result/async lowered here
```

A construct is usable inside a `@comptime` block **iff it is already lowered by
line 51**. So:

- **Works today:** ordinary C, libc (file I/O, `malloc`, `ctype`, …), the host
  verbs (`cc_emit_*` / `cc_reflect_*`), and the **full template-string surface**
  (`@emit` / backtick `${}`), because string templates are lowered in `prepare`
  (line 48) — deliberately, since emission is the point of comptime.
- **Not yet:** closures (`=>`), `Vec::[T]`/`Map::[K,V]`, channel syntax
  (`[~ >]`), `async`/`await`, result `?`/`!` — all lowered after line 51.

### Axis 1 — the CC runtime *library* (types/functions)

The executor TU is `PRELUDE + @comptime-fn-defs + block-body`; it does **not**
see the file's `#include`s. So even header-only stdlib isn't reachable today.

- **Inline / `static inline` stdlib** (`CCSlice` ops, `cc_arena_*`, `CCString`
  helpers, UFCS composers incl. `cc_slice_concat2`): reachable cheaply by
  threading a *curated, TCC-safe* umbrella header into the executor TU. Only
  cost/risk: some `.cch` use constructs the in-memory TCC rejects — discovered
  header-by-header. **Slices + arena + string + vec + hash + map landed
  (2026-05-30); Axis 1 closed. See §7b.**
- **Compiled stdlib** (channels/liblfds, async/task runtime, atomics, format in
  `zmij`): real objects; would need `tcc_add_file(runtime.a)` or selective
  `tcc_add_symbol`. Heavier and semantically odd at build time. Per §5, let it
  link-and-run if the executor can, else it is an ordinary evaluation error — no
  special prohibition. **Deferred until demanded.**

### Axis 2 — the CC *language* surface

Making closures / containers / channels / `await` / result-ops run at comptime
requires routing the block body through the **same front-end lowering passes**
(canonicalize, closure/result/channel lowering, generic monomorph) *before*
TCC, then linking Axis-1's runtime. This is the genuine architectural lift and
is **deferred** until real users hit the wall; template strings already cover
the emission need.

### Build order (chosen)

1. **`#pragma cc_depends`** (§2) — **LANDED (2026-05-30).** See §2a.
2. **Curated Axis-1 umbrella** — **LANDED (2026-05-30): slices + arena + string +
   vec + hash + map; Axis 1 closed (vendored container TCC-hardened under
   `CC_COMPTIME`).** See §7b.
3. **Axis-2 front-end lowering** — only if demanded.

## 7b. Axis-1 umbrella — slices + arena + string + vec + hash + map landed (2026-05-30); Axis 1 closed

The comptime executor now threads the compiler's own header search path
(`CC_INCLUDE_PATH`, the lowered-`.h` dir then the raw-`.cch` dir, colon-split)
into the TCC instance via `tcc_add_include_path`
(`cc/src/comptime/executor.c`), and the comptime prelude now `#include
<ccc/cc_slice.cch>` instead of hand-rolling a minimal `CCSlice`
(`tools/gen_emit_tpl_prelude.sh` → `emit_tpl_prelude.inc.h`). `cc_slice.cch` is
header-only and TCC-safe (only `cc_compat.cch` + `<string.h>`/`<ctype.h>`), and
its `CCSlice` is byte-identical to the old minimal one, so there is **no ABI
drift and no duplicate definition** — the real library *is* the prelude now.

Result: `@comptime` blocks can call the **real** inline slice vocabulary
(`cc_slice_from_cstr`, `cc_slice_trim`, `cc_slice_eq_cstr`, `cc_slice_count`,
`cc_slice_hash64`, `cc_slice_starts_with`, sub/index/…) and drive emission off
the results. Proof: `tests/comptime_slice_lib_smoke`.

**`cc_arena.cch` also landed (2026-05-30).** The prelude now `#include
<ccc/cc_arena.cch>` as well, so `@comptime` can use the real arena allocator
(`cc_arena_create`/`cc_arena_alloc`/`cc_arena_free`) and **arena-allocating slice
ops** like `cc_slice_concat2`. Two details made it work cleanly:

- `cc_atomic.cch` (pulled in by the arena header) has a `__TINYC__` branch —
  a `volatile` non-atomic fallback, which is correct for the single-threaded
  comptime executor.
- `cc_arena.cch` declares `extern cc_arena_prov_counter` (defined in the
  *compiled* runtime). The comptime TU is standalone and never links the
  runtime, so the prelude **defines a per-TU instance** (`= 0`); provenance ids
  only need uniqueness within one comptime run. Registration text that used to
  live under `#ifdef CC_COMPTIME_SCAN` is now file-scope `@typehooks` / factory
  harvest (stripped from host `.h`).

Proof: `tests/comptime_arena_smoke` (arena create + `cc_slice_concat2` + free,
all at compile time).

**`std/string.cch` also landed (2026-05-30) — the full `CCString` builder.**
This one was a *hybrid* header and needed a small surgery rather than a plain
include:

- It transitively `#include`s the heavy `cc_runtime` / `cc_type` / `cc_ufcs` /
  `vec` machinery — which is also what pulls in channel/future symbols
  (`cc_chan_recv`/`close`/`free` and a `cb`) that the comptime TU can never call.
- Its `CCString` builder core (`cc_string_push_buffer`, `push_slice`,
  `from_slice`, `as_slice`, `cstr`, `provenance`, `with_capacity`, `clear`) is
  *declared* in the header but *defined* in the compiled runtime
  (`cc/runtime/string.c`), absent from the `ccc` process.

A first wholesale-include attempt failed at *relocate* (`undefined symbol`,
including those channel symbols) — the header **compiles** fine under TCC, only
the link was unsatisfiable. The fix is a single `CC_COMPTIME` flag
(defined by the comptime prelude before the includes) inside `string.cch`:

- gates out the four heavy transitive includes — and the machinery that needs
  them (`CCVec_char`, result-spec declarations, the `cc_slice_parse_*` helpers),
  which also drops every channel/`cb` reference; and
- provides those eight `cc_string_*` functions as `static inline` (bodies
  mirrored from `string.c`), so the builder is self-contained.

`string.cch` stays the single definition site; normal builds (flag undefined)
are byte-for-byte unchanged. A standalone TCC probe confirms zero non-libc
undefined symbols under the flag. Proof: `tests/comptime_string_smoke`
(`cc_string_push_cstr`/`push_int`/`cstr`/`as_slice` at compile time).

**`std/vec.cch` + `std/hash.cch` also landed (2026-05-30) — closing the inline
data-structure surface.** The same `CC_COMPTIME` flag now also covers `vec`:

- `vec.cch` pulled `cc_runtime.cch` purely transitively (it uses *nothing* from
  it), and that include is what dragged the channel/`cb` symbols back in; it is
  now gated behind `#ifndef CC_COMPTIME`. `cc_vec.cch` itself only needs the
  arena+slice vocabulary already in the prelude.
- `cc_vec.cch` used `_Alignof(max_align_t)` for its header padding, but the
  comptime executor's TCC sysinclude does not declare `max_align_t`. Under
  `CC_COMPTIME` it uses the conventional maximal alignment (`16`) via the
  `CC__VEC_MAX_ALIGN` macro; normal builds keep `_Alignof(max_align_t)`.
- The typed `CCVec_T` wrappers (`CC_VEC_DECL_ARENA`) expand to *file-scope*
  static-inline definitions, so a comptime block drives the generic `CCVec`
  base API directly (`cc_vec_init`/`cc_vec_push_slot`/`v.data`/`v.len`).
- `hash.cch` is header-only and only depends on the now-guarded `string.cch`,
  so it rides along for free (`cc_hash_u64`, `cc_hash_slice`).

Proof: `tests/comptime_vec_smoke` (arena-backed `CCVec` build + FNV `cc_hash_slice`
at compile time).

**`std/map.cch` (`CCMap`) also landed (2026-05-30) — TCC-hardening the vendored
container.** This was the deferred holdout; bringing it in meant making the
~9.7k-line `cc_containers.cch` (the aggressively macro'd "ccj"/convenient-
containers lib) parse under the comptime TCC. The hostile constructs — all gated
behind `CC_COMPTIME`, normal builds byte-for-byte unchanged:

- `max_align_t` (absent from TCC's sysinclude): a concrete max-aligned union
  `ccj_comptime_max_align_ty` + a `CCJ_MAX_ALIGN` macro replace the six
  `_Alignof(max_align_t)` sites and the `ccj_max_align_ty` typedef.
- `_Alignas` with a **type** operand (`alignas(ccj_max_align_ty)`, 9 sites):
  TCC's `_Alignas` only accepts a *constant*, so `CCJ_ALIGNAS` → `_Alignas(16)`.
- `u""`/`U""` char16/char32 string-literal placeholders: TCC lacks them; comptime
  never instantiates wide-char string containers, so an empty narrow `""`
  placeholder is functionally identical (size/cap 0, contents never read).
- `_Generic` (76×) and `typeof` (5×) — verified TCC *does* support both, so no
  change needed there.

The map is otherwise self-contained (header-only `_Generic` dispatch, no runtime
backing — a standalone TCC probe shows zero non-libc undefined symbols). The
typed `CCMap_K_V` wrappers (`CC_MAP_DECL_ARENA`) are *file-scope* static-inline
defs (same constraint as `CCVec`), so the prelude pre-declares the common
metaprogramming key types — `CCMapII` (int→int), `CCMapU64I` (u64→int),
`CCMapSI` (CCSlice→int) — for a comptime block to use via `CCMapII_init`/
`_insert`/`_get`/`_len`. Adding the header costs ~10 ms of TCC parse per comptime
TU (measured). Proof: `tests/comptime_map_smoke`.

So the full inline+`malloc`-backed stdlib **data-structure** surface — slices,
arena, string, vec, hash, **map** — now runs at comptime. **Axis 1 is closed.**
What remains is genuinely out of reach for a comptime block: the compiled-runtime
pieces (channels/async, task/future, exec/sched/select/nursery/closure/tls,
io/net/dns/http/dir/process/cli, the type-erased `cc_dyn_vec`, real atomics, the
`zmij` float formatter) — no way to drive these from comptime anyway — and the CC
*language* surface, which remains Axis 2.

---

## 7. Relation to other docs

- `COMPTIME_INSTANTIATION_SEAM.md` — the emission/factory/reflection machinery
  this model governs the *inputs* to. Additive-only (§3) is enforced there.
- **spec §14.8** — to be rewritten per §1.
- The **comptime-grammar** idea (`@grammar(E) Name {…}` = a comptime generic
  whose argument is source) is *orthogonal*: this capability model governs what
  any comptime — including a grammar engine — is allowed to do. See
  [`GRAMMAR_DSL_PROPOSAL.md`](GRAMMAR_DSL_PROPOSAL.md) for the proposed surface.
