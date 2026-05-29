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

## The freeze clock

The freeze trigger is **not** "the runtime is stable" (it already is). It is **the appearance of external source** — the day someone outside writes `.ccs` against `ccc`, the wave's wake becomes their breakage.

Before that point: be ruthless about unearned syntax while removal is still free. Gate 1.0 on the validation ladder being **tall and domain-diverse** (`DESIGN.md`), not on the substrate being ready. Let the projects you haven't run yet veto the freeze.
