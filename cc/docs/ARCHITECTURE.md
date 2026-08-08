# Concurrent-C Compiler Architecture

**Status:** Authoritative architecture for the **default** `ccc` front
(**native** / `shadow_lower`).
**Audience:** anyone changing how `.ccs` / `.cch` become C, or proposing a
redesign of that path.

The older multipass text-rewrite / TCC stub-AST front is opt-out
(`--frontend=legacy`). Its ADR is
[`LEGACY_ARCHITECTURE.md`](LEGACY_ARCHITECTURE.md).

Operational detail, file layout, gaps, and promote workflow live in
[`cc/shadow/README.md`](../../cc/shadow/README.md). Bootstrap snapshots:
[`cc/bootstrap/shadow_lower/README.md`](../bootstrap/shadow_lower/README.md).

---

## TL;DR

```text
.ccs / .cch bytes
    → stage 1  FileTape (pp-tokens + comment spans, path-keyed cache)
    → stage 2  stitch (#include / object-like #define / guards)  ← closed early
    → whitelist AST (emit + diags + safety — not a compiler IR)
         ├─ shadow_emit_c → .c  → host cc + concurrent_c.o
         └─ shadow_emit_h → .h  → host cc -c
    comptime: prepare/exec/splice via libshadow_comptime (+ TCC), not the lowerer
```

`ccc` is a thin driver: find `shadow_lower`, forward options, ensure runtime,
invoke host `cc`. Lowering lives in `cc/shadow/*.ccs` / `*.cch`, shipped
as host-cc'd bootstrap under `cc/bootstrap/shadow_lower/last-good`.

Three layers:

| Layer | Role | Reparses |
|-------|------|----------|
| **Tape** | Stage-1 lex + Stage-2 stitch | 0 — stitch is upfront and closed |
| **Whitelist AST** | Parse CC surface; attach sticky trivia; safety | 0 — emit walks the tree once |
| **Emit + host** | One C/`#line` product; cache; host `cc` (or `--exe` libtcc) | 0 on the CC surface |

No multipass text rewrite of CC sugar. Opaque C blobs may pass through as text
by policy; that is not a second lowering IR.

---

## 1. Reader's map

| If you want to … | Read |
|------------------|------|
| Why this shape | §2 (constraints) + §3 (layers) + §4 (ADRs) |
| What each source file owns | [cc/shadow/README.md](../../cc/shadow/README.md) Layout |
| Bootstrap / promote | [bootstrap README](../bootstrap/shadow_lower/README.md) |
| What's still missing | shadow_lower README **Next gaps** |
| Legacy multipass why/shape | [`LEGACY_ARCHITECTURE.md`](LEGACY_ARCHITECTURE.md) |
| `@grammar` / wire SERDES | [`spec/cc_serdes.md`](../../spec/cc_serdes.md) |

---

## 1.5 Design principle: fail loudly

Same gradient as the rest of the project:

```text
runtime-at-a-distance → runtime-at-the-site → link → compile →
apply/patch → impossible by construction
```

Quiet success that means "couldn't" is forbidden (see root `CLAUDE.md`).
Concrete corollaries for this front:

- Stage-2 `#if` never guesses the true arm — unimplemented → hard error or
  explicit passthrough-by-design, never a silent wrong branch.
- Typed/instance UFCS miss → diagnose; do not invent `Map_*_*` callees.
- Snapshot angle-includes and ODR link bugs must fail the cold `make` path,
  not only the machine that generated the snapshot.
- Driver options the lowerer cannot honor must **refuse** or be handled in
  `ccc` before exec — never silently dropped. `build.cc` / `-D` / dumps /
  `--compile` are handled in the driver and forwarded as host flags.

---

## 2. Constraints

### C1. We emit C; we do not own a C compiler

Host `cc` (and optionally libtcc for `--exe` / comptime) compiles and links.
The lowerer's job is a **readable C product** with `#line` back to user
source — not an optimizing IR, not a second C frontend.

**Therefore:** grow a whitelist AST only where emit, diagnostics, or safety
need structure. Already-legal C (enum lists, switch cases, opaque static-fn
bodies, `AST_RAW_LINE`) may pass through as text. That is product policy, not
an invitation to reintroduce string-soup rewriting of CC surface.

### C2. CC surface is not a C lexer vocabulary

`T[:]`, `T!>(E)`, `int[~4 >]`, `() => {…}`, `!>`, `@destroy`, etc. cannot be
honestly tokenized by a C lexer. The legacy front solved this with ~16
text-preprocess passes before TCC.

**Therefore:** Stage-1 uses a **pp-token grammar** (`pp_tok.rules`) that knows
CC tokens; Stage-2 stitches a closed cpp subset; the whitelist parser builds
structure from that tape. There is no "rewrite to C-shaped text, then hope
TCC's stub AST sees it" loop on the product path.

### C3. Diagnostics and safety need user coordinates

Errors must say `path:line:col` on the original `.ccs` / `.cch`. Provenance /
move / channel / unwrap checks need enough structure to refuse unprovable
cases loudly.

**Therefore:** trivia (lead comments, `#line`, `tok_off` / `file_id`) sticks
at parse. Emit prints; it does not recover comments by rescanning strings.
Safety walks the whitelist AST (and typed tables), not a post-mangle buffer.

### C4. The lowerer must bootstrap from a C compiler alone

A fresh clone with only host `cc` (plus patched `libtcc.a` for the driver)
must produce a working `shadow_lower` without already having native.

**Therefore:** committed bootstrap snapshots under
`cc/bootstrap/shadow_lower/vN/` (pointer `last-good`). Source of truth is
only `cc/shadow/*.ccs` / `*.cch` — every behavior fix is edited there first.
`out/include/cc/shadow/*.h` is a build product, not a second source tree.
Ship face changes via
`SHADOW_LOWER_SOURCE=ccs` → `snapshot_shadow_lower.sh` →
`promote_shadow_bootstrap.sh` — never by editing `out/include` or an existing
`vN/` to “land” a fix, never by patching `last-good`'s tree in place, and
never by copying `*.cch` onto `out/include/cc/shadow/*.h`. Promote creates a
**new** `vN` and flips `last-good`; that is the only way a face change enters
the committed seed.
Cold rebuild on a second platform is part of the gate — not optional smoke
on the generating machine only. See
[`cc/bootstrap/shadow_lower/README.md`](../bootstrap/shadow_lower/README.md).

### C5. Comptime is a seam, not the lowerer

`@comptime` / `@emit` / factory instantiation still run through
`shadow_comptime.c` + `libshadow_comptime.a` (prepare / exec / splice, TCC
where needed). The tape/AST/emit spine does not become a comptime VM.

**Therefore:** Stage-1 may see resolved `@comptime if` spelling and blanked
`@comptime` blocks; remaining holes are seam completeness, not "add another
emit peel."

**TCC sees only C.** Attributes (`@as`, …) are AST facts used while lowering;
product and comptime session buffers must not carry them (no comment-encoded
`@as`). Native `.ccs` path runs a **type pass** first when the harvested TU
contains `@comptime` (blank those sites, whitelist emit → `__cc_rf_T[]` +
`cc_ct_field_reg_*`); TCC sessions get a **slim prelude** (`__cc_rf_*`
tables only — not full type-pass TUs or re-injected typedefs). Comptime
prepare/exec reads `is_as` from that registry via `cc_reflect_field_*`;
header-only `.fields` / `.methods` use Concurrent-C text plus registered
included `.cch` (no full-TU CPP reflection view). Then the product lower runs.
Post-emit `shadow_product_host_c_ok` refuses leftover CC surface (`!>`, `[:]`,
`::`, `@as`, …), skipping preprocessor lines.

---

## 3. Layers

### L1 — Tape (stage 1 + stage 2)

- **Stage 1:** lex `.ccs` / `.cch` → `FileTape` (tokens + comment spans).
  Cache is path-keyed and env-free.
- **Stage 2:** splice `#include`, object-like `#define`, simple include
  guards. Directive policy is exhaustive: implement, passthrough-by-design,
  or hard error. Stitch finishes **before** AST; emit never re-expands.

### L2 — Whitelist AST

Parser modules (`pp_ast_parse_*.cch`) build only the node shapes emit and
safety need: stmts, unwrap/bang, spawn/closure, TU/externals, typed calls /
UFCS forms, sticky trivia. Umbrella headers preserve include order for tools;
splits are for readability, not pipeline stages.

This is **not** a general C/CC IR. Missing shape → extend the whitelist or
keep the span opaque — do not add a post-parse text mangler for CC sugar.

### L3 — Emit + host consume

- `shadow_emit_c` / `shadow_emit_h` walk the tree once.
- UFCS lowers through structured parts (`shadow_ufcs_lower_parts`); leftover
  peel is for unbound/opaque text only, left-to-right — not a second IR.
- Product CLI (`shadow_lower.ccs`): emit text, host-cc with emit/obj cache
  under `out/.cc-build/native/<fp>/`, or `--exe` (libtcc from the emit buffer).
- Succession metric: **warm host-cc rebuild parity**, not libtcc-vs-clang.

`ccc` (`cc/src/cc_main.c`) locates `shadow_lower`, forwards the options
contract (release/debug/flags/target/sysroot/no-runtime/dry-run), ensures
`concurrent_c.o` / runtime, and refuses unimplemented contract fields.

---

## 4. ADRs

### ADR-S1: Whitelist AST, not a full C parser

**Decision:** Parse only what we emit or check.
**Rejected:** Writing a general C parser "eventually."
**Why:** Same as legacy C1 upside argument — years of dialect work with no
CC-specific return. Opaque pass-through covers the C we do not lower.

### ADR-S2: Stitch early; never re-expand in emit

**Decision:** Stage-2 closes includes/defines/guards before AST.
**Rejected:** Macro expansion interleaved with emit or post-parse rescans.
**Why:** Coordinate stability and "trivia sticky" require a finished tape.

### ADR-S3: Zero post-parse mangling of CC surface

**Decision:** CC sugar is structured at parse or handled at a typed emit site.
**Rejected:** Beachhead expression pipelines / string rewrite of `!>` / UFCS /
channels after parse (`shadow_lower_expr_beachhead` and kin).
**Escape:** `SHADOW_RAW_BODY_REWRITE` defaults **off**; opaque C copy is the
product default. Architectural smokes assert mangling helpers stay gone.

### ADR-S4: Driver / lowerer split + bootstrap freeze

**Decision:** `ccc` (C) drives `shadow_lower` (CC, bootstrapped from committed
lowered C). Source of truth is the `.ccs` tree; `last-good` is the cold-start
seed.
**Rejected:** Shipping only a prebuilt binary; or requiring native to build
native with no snapshot.
**Why:** C4. Promote remains a human-gated snapshot, verified portable.

### ADR-S5: Host `cc` is the product compiler; TCC is specialized

**Decision:** Default link path is emit → host `cc` + runtime. TCC serves
comptime / `--exe` / driver parse hooks — not the everyday lower→run path.
**Rejected:** Making libtcc the succession metric for "native is done."

### ADR-S6: Legacy front is opt-out, not a parallel product story

**Decision:** Default `ccc` is **native** (`shadow_lower`).
`--frontend=legacy` remains for archaeology only (reparse dumps, visitor-only
warnings). Snapshot / `shadow_lower.sh` use the native binary — no legacy
chicken-egg emit path.
**Rejected:** Dual-default confusion; silent fallback between fronts.

---

## 5. Non-goals

- Full ISO cpp (`##`, function-like macros, rich `#if` evaluation)
- A general compiler IR or SSA-style mid-end inside `shadow_lower`
- Porting `cc/src/visitor/pass_*.c` scanners onto the tape
- Merging legacy Phase-N reparse counts into the native success metric
- Quietly accepting driver flags the lowerer ignores

---

## 6. What redesign would actually help

In priority order (fail mass × language value):

1. Richer safety / points-to so unprovable move/channel/unwrap refuses dominate.
2. Closing the comptime/factory seam (dylib factories, type-register/UFCS
   comptime, header-local `static_map`) without pulling TCC into emit.
3. First-class `@variant` and a real `@async`/`@await` state machine (replace
   poll-wrapper beachhead).
4. Shrinking leftover UFCS peel and `@string` template special cases.

A redesign that reintroduces multipass text rewrite of CC surface, or that
requires a full C parser before those land, fights C1–C3.

---

## 7. Naming

| Name | Means |
|------|--------|
| **native front** | This architecture: tape → whitelist AST → emit (`--frontend=native`) |
| **`shadow_lower`** | The product lowerer binary / `.ccs` implementing that front |
| **`spec/cc_serdes.md`** | `@grammar` engines and **wire** serialization — unrelated to this front |
| **`cc/shadow/`** | Source tree for `shadow_lower` (`.ccs` / `.cch`) |

Prefer "**native**" / "`shadow_lower`" for the compiler. **SERDES** means only
grammar / wire serialization (`spec/cc_serdes.md`, `examples/serdes/{json,resp}`).
