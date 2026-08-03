# Stability

CC has **two surfaces moving at two speeds**, with the lowering as the bearing between them.

| Surface | Status (pre-1.0) |
|---|---|
| ABI / runtime / lowered-C contract | **Stable.** Compiled objects link; the runtime interface is fixed. |
| Source syntax (`.ccs` surface) | **A moving wave.** Constructs are added, reshaped, and removed (see `DESIGN.md`). |

## What this means

- **Source stability is not yet promised.** Old-form source (`@nursery`, `try`, …) may not recompile on a newer `ccc`. The guarantee currently lives at the ABI/link layer — the *inverse* of what most language users assume. If you depend on CC today, depend on the ABI, not on source surviving unchanged.
- **The shock absorber:** because every feature lowers to a stable ABI, the surface can churn freely without breaking artifacts or the runtime. That is what buys the long pre-1.0 runway — usable *and* still evolving.
- **The silent-breakage trap:** ABI breakage screams (link errors); source-syntax breakage is *silent* until old source is recompiled, because the stable layer is not the moving one. `examples/` and `real_projects/` kept compiling on every change are the **only canary** for source breakage. Do not let that corpus lapse.

## What a host toolchain must provide

The lowering emits constructs that are not all in any single C standard, so the
bar is a **feature list**, not a standard version — that is what a toolchain is
actually checked against.

| Required | Where it comes from |
|---|---|
| `_Generic` | dispatch across the stdlib (133 uses; 76 in `cc_containers.cch`, 9 in `cc_result.cch`), all on the prelude path |
| GNU statement expressions `({ ... })` | the `!>` and `?>` lowerings, every `@emit` template, and the public macros `cc_unwrap`, `cc_unwrap_as`, `cc_unwrap_err` |
| `__typeof__` | destination-typed extraction in the same lowerings |
| C11 `max_align_t` | probed by the host profile, which rejects a toolchain without it |

`_Generic` on its own is widely available below full C11 — GCC 4.9+, Clang
3.0+, and the clang-derived embedded toolchains carry it without `_Atomic` or
`threads.h`. `max_align_t` is the one that does pull in C11: the host probe
compiles a program using it and fails the build otherwise, so a C99-only
toolchain is already rejected today. In-process parse sessions are set to the
same version (`CC_HOST_C_STD_OPTION`) so headers cannot take one set of
branches during the parse and another in the real compile.

Statement expressions are the real constraint. Most embedded toolchains have
them; MSVC never has. Removing them means relowering `!>` and `?>`, which is
why MSVC is not currently a host even though it is an intended target.

## The freeze clock

The freeze trigger is **not** "the runtime is stable" (it already is). It is **the appearance of external source** — the day someone outside writes `.ccs` against `ccc`, the wave's wake becomes their breakage.

Before that point: be ruthless about unearned syntax while removal is still free. Gate 1.0 on the validation ladder being **tall and domain-diverse** (`DESIGN.md`), not on the substrate being ready. Let the projects you haven't run yet veto the freeze.
