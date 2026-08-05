# Comptime Instantiation Seam — unified design

> **Note:** Default `ccc` is native (`shadow_lower`). Comptime still shares
> prepare/exec/splice seams with the legacy front; product holes are tracked
> in [cc/shadow/README.md](../../cc/shadow/README.md) (Next gaps).

**Status:** proposal (2026-05-29)  
**Goal:** Make `@comptime` as complete as the language allows, retire `CC_PARSER_MODE` stubs, and converge built-in generics (Vec/Map/Result), protocol hooks (UFCS/create/destroy), and `cc_type_info` introspection on **one compile-time seam**.

**Non-goal (for this doc):** Implement the full evaluator in one pass. This specifies the architecture and migration so work can land incrementally without another Vec/Map-style detour.

---

## 1. Problem: three pipelines, one product story

Today the compiler does compile-time work through **three unrelated mechanisms**:

| Mechanism | What it answers | Where it lives |
|-----------|-----------------|----------------|
| **Type registry** | Which `CCVec_T` / `Map_K_V` / `CCResult_T_E` monomorphs does this TU need? | `type_registry.c`, preprocess/codegen splice |
| **`cc_type_register` scan** | How does `@create` / UFCS lower for registered types? | `symbols.c`, `hook_compile.c`, dylibs |
| **`cc_type_info`** | What is `T`'s layout for `type_of(T)` / `cc_dyn_vec`? | `cc_type.cch`, runtime registry, codegen emit |

Plus a fourth, accidental layer:

| **`CC_PARSER_MODE`** | What can TCC's stub-AST pass swallow before real lowering? | `map.cch`, `task.cch`, `cc_closure.cch`, TCC `cc_parser_mode` |

The **generics vision** (spec §9, §12, §14) is comptime-shaped: monomorph at compile time, introspect via `type_of(T)`, register protocols in `@comptime`. The **implementation** that works for stdlib containers is macro monomorph + splice ordering — not `@comptime` emission.

Users and contributors should see **one model**:

> **Compile time builds a type graph for this TU; instantiation emits real C at a controlled anchor; runtime only executes what was emitted.**

---

## 1b. Comptime intrinsic surface — scanner unification

Zooming out from the individual tracks: the compiler "understands" a fixed set
of **builtin comptime intrinsics** — calls that appear inside `@comptime { … }`
and are interpreted by the compiler rather than executed at runtime:

| Intrinsic | Effect | Recognized in |
|-----------|--------|---------------|
| `cc_type_register` / `cc_type_define` | install create/destroy/UFCS protocol hooks | `comptime/symbols.c` |
| `cc_instantiate_vec` / `_map` / `_chan` | force a monomorph onto the type graph | `preprocess/emit_plan.c` |
| `cc_emit_cstr` | splice a C fragment at an anchor | `preprocess/emit_plan.c` |

Historically each was discovered by its **own** raw-text scanner, and the two
modules carried **duplicate micro-lexers** (positional keyword match, C
string-literal decode, brace/paren matching, ws+comment skipping). That is the
opposite of "one model": three scanners, two lexers, subtly divergent escape
and overflow handling.

**Convergence (done):**
- Shared scanning primitives now live once in `util/text.h`
  (`cc_match_ident_kw`, `cc_parse_c_string_literal`, plus the pre-existing
  `cc_find_matching_*` / `cc_skip_ws*`). `emit_plan.c` and `symbols.c` both
  route through them — single source of truth, no behavior change. `symbols.c`'s
  former private `cc__skip_ws_reg` / `cc__find_matching_reg` / `cc__match_kw_reg`
  / `cc__parse_string_literal_reg` are now thin shims over the shared ones.
- "What is a `@comptime` block, and where does its body span" is defined once,
  in `cc_match_comptime_block` (`util/text.h`). Both `emit_plan.c`'s intrinsic
  enumerator (`cc__emit_for_each_comptime_block`) and `symbols.c`'s
  hook-collection loop call it — there is now literally one block recognizer.
- `emit_plan.c` walks `@comptime` blocks through **one enumerator**
  (`cc__emit_for_each_comptime_block`) feeding **one table-driven dispatcher**
  (`cc__emit_visit_dispatch`), rather than a copy-pasted loop per intrinsic.

**Registry (done):** the `emit_plan.c` intrinsics are now a single data table —
`cc__comptime_intrinsics[] = {name, group, handler, …}` (mirroring the B4
UFCS-family table). The dispatcher scans each block body once and routes any
matched call to its handler; each public collector
(`…_collect_comptime_instantiations` / `…_emits`) just passes a `group` mask
selecting which intrinsics it wants. Adding or replacing an `emit_plan`
intrinsic is now a one-line table edit, and the `if (match_kw …)` chains are
gone from this module.

`cc_type_register` / `cc_type_define` remain handled in `comptime/symbols.c`
**by design**: they run in a different evaluation stage (they build the symbol
table and drive dylib compilation, and propagate parse errors), so they share
the block recognizer + lexers but not `emit_plan`'s static-buffer collectors.
Both spellings are matched there from one keyword list.

**North star (next):** this registry is the last text-matching layer before a
real comptime evaluator (Track D, C0–C5): the difference between "the compiler
text-matches N known calls" and "the compiler evaluates a `@comptime` block"
becomes a swap of the per-statement handler behind the same table.

---

## 2. Target architecture

```mermaid
flowchart TB
  subgraph inputs [TU inputs]
    SRC[User .ccs source]
    PRE[Prelude / headers]
  end

  subgraph phase1 [Phase 1 — canonical CC]
    CANON[Canonicalize syntax]
    COLLECT[Collect instantiation requests]
  end

  subgraph seam [Instantiation seam — NEW unified API]
    GRAPH[CCTypeGraph per TU]
    PLAN[InstantiationPlan ordered deps]
    EMIT[Emit C fragments + symbols]
  end

  subgraph phase2 [Phase 2 — comptime eval]
    EVAL[@comptime evaluator]
    REG[Register hooks + constexpr facts]
  end

  subgraph phase3 [Phase 3 — lower + parse once]
    LOWER[UFCS / async / closures / results]
    PARSE[Single host-C parse — no parser stubs]
  end

  SRC --> CANON
  PRE --> CANON
  CANON --> COLLECT
  COLLECT --> GRAPH
  EVAL --> GRAPH
  GRAPH --> PLAN
  PLAN --> EMIT
  EMIT --> LOWER
  REG --> LOWER
  LOWER --> PARSE
```

### 2.1 Core objects

**`CCTypeGraph`** (per TU, replaces parallel registry + symbol UFCS tables for *lookup*)

- Nodes: concrete types (`CCVec_int`, `Map_int_int`, `MyStruct`, `CCResult_T_E`)
- Edges: dependencies (payload `T` before `CCVec_T`, key before `Map_K_V`)
- Attributes per node:
  - `cc_type_info` layout (size, align, fields, flags)
  - **protocol slots**: create / destroy / ufcs handler (callable or template id)
  - **emission recipe**: which generator produces C text (`cc_gen_vec`, `cc_gen_map`, user comptime fn)

**`CCInstantiationRequest`** (anything that demands a monomorph or decl)

Collected from:

1. **Syntax scan** (today's registry): `CCVec::[T]`, `Map::[K,V]`, `T!>(E)`, chan brackets
2. **`@comptime`**: `cc_instantiate("CCVec", "int")`, `cc_emit_decl(...)`, `type_of(T).fields` driven codegen
3. **Implicit builtins**: prelude `CCVec_char`, stdlib Result specs

**`CCEmitAnchor`** (replaces ad-hoc `insert_pos` / `container_pos` / delayed splice)

- `CC_EMIT_AFTER_PRELUDE` — default for container/result monomorphs (user `#include`s + typedefs visible)
- `CC_EMIT_BEFORE_FIRST_USE` — delayed splice for local struct payloads (today's Result/Vec delayed path)
- `CC_EMIT_TOP` — rare; only for forward tags with no payload deps
- `CC_EMIT_COMPTIME_BLOCK` — inline at the `@comptime` site (for constexpr-only helpers)

Every emission records: fragment text, anchor, `#line` resync, and **symbols defined** (for duplicate detection).

---

## 3. Public API (headers)

New header: `ccc/cc_instantiate.cch` (name TBD; spec-visible).

### 3.1 Built-in instantiation (replaces implicit registry-only path)

```c
/* Request a monomorph. Idempotent per TU. Compiler collects at phase 1/2;
 * emission happens at the plan's anchor, not at the call site. */
void cc_instantiate_vec(const char* elem_mangled);
void cc_instantiate_map(const char* key_mangled, const char* val_mangled);
void cc_instantiate_result(const char* ok_mangled, const char* err_mangled);
void cc_instantiate_chan(const char* elem_mangled);

/* Generic entry for user factories (future §12): */
typedef struct CCInstantiateSpec {
    const char* family;       /* "CCVec", "SmallVec", "MyMap" */
    const char* mangled_name; /* "CCVec_int", "SmallVec_Point_8" */
    const char* const* type_args;
    size_t n_type_args;
    const long long* value_args;  /* @comptime int N */
    size_t n_value_args;
} CCInstantiateSpec;

void cc_instantiate(const CCInstantiateSpec* spec);
```

**Compiler lowering:** `CCVec::[T]` rewrite calls `cc_instantiate_vec(mangle(T))` inside a synthetic `@comptime` block (or records directly into the graph — user never writes this for builtins).

### 3.2 Comptime emission (new — makes comptime "complete")

```c
/* Emit a C declaration or statement into the TU at the given anchor.
 * Only callable from @comptime. Returns 0 on success. */
typedef enum CCEmitAnchor {
    CC_EMIT_AFTER_PRELUDE = 0,
    CC_EMIT_BEFORE_FIRST_USE = 1,
    CC_EMIT_AT_COMPTIME_SITE = 2,
} CCEmitAnchor;

int cc_emit_cstr(CCEmitAnchor anchor, const char* c_fragment);
int cc_emit_format(CCEmitAnchor anchor, const char* fmt, ...);

/* Register cc_type_info for a type the comptime block is defining.
 * Unifies cc_type_info_register with the graph. */
int cc_emit_type_info(const cc_type_info* ti);
```

**Example (user generic factory, future):**

```c
@comptime void instantiate_SmallVec(const char* T, int N) {
    cc_emit_format(CC_EMIT_AFTER_PRELUDE,
        "typedef struct { %s items[%d]; size_t len; } SmallVec_%s_%d;\n",
        T, N, T, N);
    /* cc_emit_type_info(...) from constexpr walk of fields */
}
```

### 3.3 Unified type registration (replaces split hooks)

Merge `cc_type_register(name, CCTypeHooks)` and `cc_type_info` into **one graph node**:

```c
typedef struct CCTypeProtocol {
    /* Construction — same semantics as today CCTypeCreateHook */
    CCTypeCreateHook create;
    CCTypeDestroyHook destroy;

    /* UFCS — redesigned (§4) */
    CCUfcsDispatchTable* ufcs;  /* or inline CCTypeUfcsHandler ufcs */

    /* Layout — optional; if NULL, compiler reflects from emitted decl */
    const cc_type_info* info;
} CCTypeProtocol;

/* Single registration API — only in @comptime */
int cc_type_define(const char* mangled_name, CCTypeProtocol proto);
```

**Migration:** `cc_type_register` becomes a thin wrapper calling `cc_type_define`. `CC_TYPE_INFO_BEGIN/END` becomes sugar that calls `cc_emit_type_info` + `cc_type_define` with layout only.

---

## 4. UFCS redesign

### 4.1 Today

1. Receiver type resolved (registry + AST)
2. Lookup string callee in `CCSymbolTable` or legacy pattern table
3. Optional: dylib callable from `cc_type_register(".ufcs = lambda")`
4. UFCS handler returns **callee name string**; pass rewrites text

Problems: stringly-typed, two registries, dylib for what could be constexpr tables, parser-mode stubs for types not yet in graph.

### 4.2 Target: dispatch from the type graph

```c
typedef enum CCUfcsMode {
    CC_UFCS_VALUE,   /* v.method()  — receiver is lvalue */
    CC_UFCS_PTR,     /* v.method()  — receiver is pointer */
    CC_UFCS_MOVE,    /* future */
} CCUfcsMode;

typedef struct CCUfcsCall {
    const char* recv_mangled;
    CCUfcsMode mode;
    const char* method;
    const char* const* arg_mangled_types;
    size_t n_args;
} CCUfcsCall;

/* Lowering returns an IR snippet or C callee, not an opaque string guess */
typedef struct CCUfcsLowering {
    enum { CC_UFCS_DIRECT, CC_UFCS_GENERIC_FAMILY, CC_UFCS_COMPTIME } kind;
    const char* callee;           /* Map_int_int_insert */
    const char* family;           /* "CCVec", "Map" */
    const char* method;           /* "push", "insert" */
} CCUfcsLowering;

int cc_ufcs_resolve(const CCTypeGraph* g, const CCUfcsCall* call, CCUfcsLowering* out);
```

**Built-in families** (`CCVec`, `Map`, `CCString`, channels) register at compiler startup as **family dispatchers** — not per-TU `cc_type_register` scans:

```c
/* Compiler builtin — not user API */
void cc__register_builtin_ufcs_families(CCTypeGraph* g);
```

User types still use `cc_type_define(..., .ufcs = my_handler)` where handler can:

- Return a fixed callee pattern (`MyType_%s` + method)
- Or call `cc_emit_format` to generate a one-off helper at comptime

**Dylib path:** demoted to escape hatch for hooks that truly need host C compilation during dev; long-term → TCC constexpr in-process (spec build path).

### 4.3 Parse-once implication

UFCS lowering moves to **phase 3 on canonical CC**, before the single host-C parse. TCC stub-AST no longer needs to "tolerate" `m.insert` — it never sees it. That retires UFCS-related parser-mode tolerance in `tccgen.c` once phase 3 is mandatory before parse.

---

## 5. `cc_containers` / Map and parser mode

Map stub exists because **prelude includes `map.cch` under `CC_PARSER_MODE`** while `cc_containers.cch` is gated out.

**Seam-level fix (no new comptime feature required):**

1. Split `map.cch` → `map_forward.cch` (macros, `__CC_MAP`, forward tags) + `map_impl.cch` (pulls `cc_containers`, real `CC_MAP_DECL`).
2. Prelude includes **only** `map_forward.cch`.
3. `CCInstantiationPlan` always emits `map_impl.cch` + `CC_MAP_DECL_ARENA(...)` at `CC_EMIT_AFTER_PRELUDE` (already implemented for Vec; Map uses same anchor).
4. Delete `#ifdef CC_PARSER_MODE` branch in `CC_MAP_DECL_ARENA`.

**Comptime alignment:** `cc_instantiate_map(K,V)` is the explicit request; graph ensures impl emission happens once at the right anchor. No fake struct in parser mode.

---

## 6. Comptime evaluator — what "as complete as possible" means

Phased capability matrix (each phase gates smoke + docs):

| Phase | Capability | Retires |
|-------|------------|---------|
| **C0** (now) | Text scan `cc_type_register`; blank `@comptime` blocks | — |
| **C1** | Collect `cc_instantiate_*` + graph; unified splice via plan | Duplicate registry paths in codegen |
| **C2** | `cc_emit_*` at anchors; constexpr `long long` + `sizeof`/`alignof` in `@comptime` | Some manual macro emission |
| **C3** | Parser builtin `type_of(T)` → `cc_type_info` constexpr view (3c.3) | Dual layout sources |
| **C4** | `@comptime if`, `@comptime` fn calls, monomorph cache (spec §14) | Zig-style factories |
| **C5** | TCC in-process constexpr (no dylib for hooks) | `hook_compile.c` dylib path |

**Restriction surface (spec §14.8)** stays: no I/O, channels, tasks, or allocation in `@comptime`. Hooks that need runtime become **emission** (generate C) not **execution**.

---

## 7. Pipeline reorder (enables parser mode retirement)

**Current:**

```
preprocess → cpp_expand → TCC stub-AST (CC_PARSER_MODE) → visit_codegen → host C
```

**Target:**

```
canonicalize → collect graph + run @comptime(C0–C5) → instantiate/emit plan
→ phase3 lower (UFCS/async/closures) → cpp_expand → TCC parse host C ONCE
```

Stub-AST pass becomes **optional dev flag** (`CC_STUB_AST=1`), then deleted.

| Stub | Seam replacement |
|------|------------------|
| `CC_VEC_DECL` parser branch | Removed (done) |
| `CC_MAP_DECL` parser branch | `map_impl` emission at anchor |
| `__CCResultGeneric` | Graph ensures `CC_DECL_RESULT_SPEC` before use; TCC sees real types |
| `CCTask → int` | `@async` return lowered to `CCTask` before parse; no stub parse of task ops |
| `CCClosure → intptr_t` | Closure structs emitted at anchor; stable IDs (4b) |

---

## 8. Implementation plan (commits)

Each commit: 462/462 default + `CC_PRE_EXPAND=0`.

### Track A — Seam infrastructure (no user-visible syntax change)

| # | Commit | Work |
|---|--------|------|
| A1 | `cc_type_graph.h/c` | Graph + requests; wrap existing `CCTypeRegistry` — **done** (`preprocess.c` wired; scoped-reparse clears active reg only) |
| A2 | `cc_emit_plan.c` | Unify `insert_pos`, `container_pos`, delayed Result/Vec splice — **done** (`emit_plan.h/c`; preprocess + visit_codegen anchors) |
| A3 | Wire `visit_codegen` + `preprocess.c` to plan only | Delete duplicate emission logic — **partial** (anchors + decl fprint done; Result `_Generic` *arm body* now centralized in `cc_emit_plan_format_result_arm`, both call sites share it; remaining: full result-splice control-flow still duplicated, intentionally — parser-mode adds `__CCResultGeneric` stub arm + fwd-decls, final-compile diag-wraps err arms) |
| A4 | `map_forward` / `map_impl` split | Remove Map `CC_PARSER_MODE` macro branch — **done** (`map_forward.cch` parser stub + convenience macros; `map_impl.cch` real bodies included only off the parse path; prelude pulls forward only; `emit_plan` parse path no longer includes `map_impl`) |

### Track B — Comptime API surface

| # | Commit | Work |
|---|--------|------|
| B1 | `cc_instantiate.cch` + collector | Builtin requests from existing rewrites — **partial** (header + graph collector; surface-syntax rewrites already record via `cc_type_graph_request_*`) |
| B2 | `cc_emit_cstr` (C2 minimal) | Host-side fragment buffer → plan anchors — **done** (`emit_plan` collects `cc_emit_cstr(anchor, "…")` from `@comptime` pre-blank in `build_parse_input.c`; splices at anchors in preprocess parse path + visit_codegen final `.c`; `tests/comptime_emit_cstr_smoke`) |
| B3 | `cc_type_define` wraps `cc_type_register` | **done** — `symbols.c` `@comptime` scanner recognizes both spellings identically (same `("name", CCTypeHooks{...})` shape, named + lambda handlers); header alias in `cc_type.cch`; `tests/comptime_type_define_smoke`. `cc_type_register` now the documented legacy alias |
| B4 | Builtin UFCS families on graph | **done** (registry seam) — `cc__register_builtin_ufcs_families` + `cc__ufcs_classify_family` in `ufcs.c` now own the mutually-exclusive receiver-family set (Vec/Map/Result, parser-macro + parser-stub spellings, channel tx/rx/raw) as one data table; the 7 scattered `strncmp/strcmp` predicates are thin membership tests over it. Behavior-identical (string/slice stringifiable axis left separate by design). Per-method emission ABI snippets remain inline (candidate for a later C-track). Single insertion point for future `cc_type_define` families |

### Track C — UFCS + parse-once

| # | Commit | Work |
|---|--------|------|
| C1 | Explicit `cc_instantiate_*` from `@comptime` | **done** — `@comptime { cc_instantiate_vec("int"); cc_instantiate_map("int","int"); }` forces a monomorph even when never spelled as `CCVec::[T]`/`Map::[K,V]`. Collected pre-blank, replayed into the graph (parse path) and global registry (final compile). `tests/comptime_instantiate_smoke` |
| C1b | UFCS resolve via graph | **done** — `ufcs.c` no longer reaches past the seam to `cc_type_registry_get_global()`; all 3 receiver-resolution sites obtain their registry via `cc_type_graph_active_registry(cc_type_graph_get_global())` (now exposed), so scope-aware registry selection lives in one place. Behavior-identical; dead per-channel tx/rx predicates removed |
| C2 | Phase3-before-parse experiment flag | `CC_PARSE_ONCE=1` |
| C3 | Remove TCC UFCS tolerance when parse-once default | TCC patch shrink |
| C4 | Drop `CC_PARSER_MODE` defines + `cc_parser_mode` | Stage 5 of original plan |

### Track D — Comptime completeness

| # | Commit | Work |
|---|--------|------|
| D1 | `type_of(T)` constexpr view (3c.3) | Comptime + runtime share `cc_type_info` — **D1.0 done** (numeric/layout) + **D1.1 done** (structural members) |
| D2 | `@comptime if` + constexpr eval (C4) | Spec §14 — **D2.0 done** (predicate evaluator + branch selection) + **D2.1 done** (`else @comptime if` chains + `&&`/`||` short-circuit); D3 next |
| D3 | TCC constexpr hooks (C5) | Replace dylib for simple hooks — **D3.0 done** (in-process `cc_tcc_eval_const_expr` seam) + **D3.1 done** (layout-aware `@comptime if`: (A) primitive/C-expr layout, (b) user-struct layout via in-scope type-def prelude) + **D3.2 done** (generic factories compile in-process on libtcc, no host-cc dylib; see 2026-05-31 log) |
| D4 | `type_of(T).fields` iteration (3c.3) | Reflection-driven codegen — **D4.0 done** (`@comptime for (F in type_of(T).fields) { ... }` unrolls per declared field with `F`/`F.name`/`F.type`/`F.typestr`/`F.index` substitution) |
| D5 | Instantiation seam (comptime-driven monomorphization) | **D5.0 done** (`cc_emit_format` parameterized emission + reflection *drives* `cc_instantiate_*`/`cc_emit_*`: `@comptime for` over a type's fields, expanded before collection, generates monomorphs and code); next: graduate user generic factories |

**D1 design (decided 2026-05-29): constexpr *hybrid* view, "layout out of scope".**
`type_of(T)` is a constexpr view sharing the one `cc_type_info` shape, split by
*which* fields are known when:
- **Numeric/layout** (`size`, `align`, later field `offset`) → lowered to backend
  constexprs (`sizeof(T)`, `_Alignof(T)`, `__builtin_offsetof`). Genuine integer
  constant expressions — usable in `static_assert` / array dims / `@comptime if` —
  but the *backend*, not a comptime VM, computes the number. The comptime evaluator
  never needs the target ABI.
- **Structural** (`kind`, `nfields`, `name`) → folded by the compiler from what
  it can decide by name (D1.1, done — see below).
- The bare value form `type_of(T)` and the pointer form `type_of(T)->m` keep their
  runtime `cc_type_of(#T)` semantics untouched — no churn for existing call sites.
- *Explicitly out of scope:* comptime branching on a layout **number** to choose a
  monomorph (instantiation happens pre-layout). A compiler-side ABI/layout model is
  a separate future milestone, not smuggled into D1.

**D1.0 landed (2026-05-29):** `cc__lower_type_of_constexpr` folds
`type_of(T).size`/`.align` → `((size_t)sizeof(T))`/`((size_t)_Alignof(T))` on both
the preprocess-for-parse path (`cc__apply_phase1_canonical_passes`) and the
visit_codegen emit path (the emitted `.c` is produced by the latter — both needed).
`tests/type_of_constexpr_smoke` proves ICE-ness via file-scope `_Static_assert` +
array-dim use. Smoke **466/466** default + `CC_PRE_EXPAND=0`.

**D2.0 landed (2026-05-29):** `@comptime if (PRED) { ... } [else { ... }]` —
compile-time conditional code *selection*. `cc__resolve_comptime_if` runs as the
**first** canonical pass (and matching emit slot) so the dead branch is pruned
before any other rewrite or instantiation collector sees it (true conditional
compilation, not dead-code emission). `PRED` is a self-contained integer
constant expression evaluated by `cc__comptime_eval_pred` (recursive-descent,
full C operator precedence): integer/char literals, `CC_TK_*` + `true`/`false`,
and `type_of(T).kind`/`.nfields` folded by name (same classification as D1.1).
Non-decidable predicates are a **hard error**, never a silent `false` —
`sizeof`/`_Alignof`/`type_of(T).size` (layout, out of scope), `cc_type_of(...)`
runtime reads, unknown identifiers, and casts. Taken branch spliced verbatim;
dropped spans newline-padded for stable line numbers; nested `@comptime if`
resolved by a sweep-to-fixpoint. Tests: `comptime_if_smoke` (dead branches hold
undeclared symbols → proves pruning; nested + file/stmt scope) +
`comptime_if_nonconst_fail` (runtime predicate → diagnostic). Smoke green.
**D4.0 landed (2026-05-29):** `@comptime for (F in type_of(T).fields) { BODY }` —
compile-time field iteration (reflection-driven codegen, the consumer side of the
runtime `cc_type_info.fields[]` model). Runs in the same outermost-first sweep as
`@comptime if` (so the two nest in any order: an `@comptime if` inside a `for`
body sees the loop variable already substituted; a dead `if` branch is pruned
before any `for` it contains is expanded). For each declared field of `T`, BODY is
emitted once with the loop variable substituted: `F` → the field identifier (so
`t.F` → `t.a`), `F.name` → `"field"` (string literal), `F.type` → the field's type
spelling, `F.index` → the 0-based index. `T`'s definition is read from the same
source buffer (`cc__ct_find_struct_body` + `cc__ct_parse_fields_from_body`, reusing
the D3.1b top-level scanner). Field forms that cannot be spelled as a usable
`type` (inline anonymous/nested aggregate defs, anonymous members, unnamed
bitfields, pointer-to-array) and unknown types are a **hard error** — reflection
sees every field or none, never silently zero. (Arrays, function pointers,
multi-declarators, and named bitfields *are* modeled — see the 2026-05-31
member-declarator parser entry.) Tests: `comptime_for_fields_smoke` (bare/`.name`/`.index`/`.type` over a
3-field struct; runtime-checked sums) + `comptime_for_unknown_fail` (unknown type →
diagnostic). Suite **473/473** both default and `CC_PRE_EXPAND=0`. *Boundary:*
header-defined structs aren't visible (same pre-expand seam as D3.1b); line numbers
after a `for` shift (expansion adds lines) — only affects files using the construct.

**D5.0 landed (2026-05-29):** the instantiation seam — reflection *drives*
instantiation/emission. Two pieces:
- **`cc_emit_format(anchor, fmt, args…)`** — parameterized comptime emission
  (the §3.2 primitive). The compiler substitutes `%s` (string-literal arg),
  `%d`/`%i` (integer-literal arg) and `%%` at collection time, then splices the
  result through the same anchor machinery as `cc_emit_cstr`. New `F.typestr`
  loop-substitution yields a field's type spelling *as a string literal*, so
  reflection can feed `cc_instantiate_*`/`cc_emit_format` operands (`F.type`
  stays bare for type positions).
- **Compose reflection → collectors.** The intrinsic collectors only see calls
  inside `@comptime { … }` blocks, and those blocks were blanked *before* the
  `@comptime for`/`if` resolver ran — so a loop nested in a block was discarded
  unexpanded. Fix: run `cc__resolve_comptime_if` on the **whole buffer before
  collection** on both the parse path (`build_parse_input`) and the emit path
  (`visit_codegen`, where it subsumes the old post-blank resolve). The resolver
  needs `T`'s definition, which lives in the buffer, not the block body. The
  `for`-header now accepts both `type_of(T)` (raw, parse path) and the
  macro-expanded `cc_type_of("T")` (emit path), making it order-independent
  w.r.t. `type_of` CPP expansion. Also fixed a latent truncation: the emit-path
  blank reset `src_ufcs_len` to the original file length, which is now wrong
  once the seam resolve expands the buffer (track the blanked length).
- **End-to-end:** `@comptime { @comptime for (f in type_of(T).fields) {
  cc_instantiate_vec(f.typestr); } }` materializes a `CCVec_<field-type>` per
  field, and a parallel `cc_emit_format` loop emits a constant per field.
  `tests/comptime_emit_format_smoke` (parameterized emission) +
  `tests/comptime_seam_reflection_smoke` (reflection drives both a Vec
  monomorph and per-field enum constants, runtime-checked). Suite **475/475**
  both default and `CC_PRE_EXPAND=0`.

---

## D6 — user generic factories (libraries own their lowering), modeled on UFCS

**Goal.** Let a *library* decide how its generic lowers to C — fully
monomorphic, type-erased core + typed shims, SoA, whatever — exactly as UFCS
lets a library decide how `recv.method(args)` lowers. `CCVec`/`Map`/`Chan` stop
being hardcoded compiler templates and become the first *registered* factories.

**The UFCS model we are copying** (`cc/src/visitor/ufcs.c`,
`cc/src/comptime/hook_compile.c`): a library writes a `@comptime` handler;
`cc_symbols_collect_type_registrations` scans for it; the referenced functions
are compiled into one refcounted dylib (`cc_comptime_compile_type_hooks`),
`dlopen`'d, and stored as a function pointer keyed by a *type pattern*. At a
call site the compiler invokes
`CCSlice fn(recv_type, method, mode, argv, arg_types, arena)` and the hook
*returns the C text* (the lowered callee) to splice. **Invariant: the library
owns the decision (returns C text); the compiler owns the splice.**

**The parallel for generics.** A factory is keyed by *generic name* (`Pair`),
triggered by an *instantiation* (`Pair::[int,double]`) rather than a call site,
and invoked once per unique type-argument tuple. The one delta vs UFCS: a
factory has **two outputs** — the *mangled name* (for use-site rewrite, the
direct analogue of UFCS's returned callee) and the *definition(s)* emitted once.

**Key constraint that shapes the staging:** a dylib-compiled factory runs *in
the compiler process*, where `cc_emit_format`/`cc_emit_cstr` are inert host
stubs. So a compiled factory must **return** its definition text (like a UFCS
hook returns a callee); it cannot call the emit intrinsics. The discovery →
instantiate → emit → rewrite machinery is identical regardless of whether the
"chosen lowering" comes from a declarative template or a compiled function, so:

- **D6.0 (declarative template, no dylib) — slice 1.** Library registers a
  template keyed by name + arity:
  ```c
  @comptime {
      cc_generic_template("Pair", 2,
          "typedef struct { $1 first; $2 second; } $0;\n"
          "static inline $0 $0_make($1 a, $2 b){ $0 r; r.first=a; r.second=b; return r; }\n");
  }
  ```
  `$0` = compiler-computed mangled name (`Pair_int_double`), `$1..$N` = the type
  args. The compiler recognizes `Pair::[args]` in the *same* place it recognizes
  `CCVec::[T]` (`cc_rewrite_generic_containers`), computes the mangle, expands
  the template once per unique instantiation into the emit-fragment channel
  (`CC_EMIT_AFTER_PRELUDE`, dedup by mangled name), and rewrites use-sites to the
  mangled name. Proves the whole seam (discovery + emit + rewrite) with minimal
  risk. The library still chooses the lowering strategy — a type-erased variant
  is just a different template (emit a shared core behind an include-guard +
  typed shims). *Mangling is compiler-default for now (`Name_arg1_arg2`).*

- **D6.1 (compiled factory) — slice 2.** Swap `cc_generic_template` for
  `cc_generic_register("Pair", pair_factory)` where `pair_factory` is a real
  `@comptime` function compiled through the *same* UFCS dylib path
  (`cc_comptime_compile_type_hooks`, new hook kind + wrapper). Contract:
  `CCSlice factory(CCSlice name, CCSlice mangled, CCSliceArray type_args,
  CCArena*)` returning the definition text. Arbitrary compile-time logic
  (branch on arity/N, choose monomorph vs type-erased) now possible.

- **D6.2 (value params).** Allow `SmallVec::[int, 8]` — non-type args threaded as
  string slices to the template/factory.

- **D6.3 (reflection in factories) — landed 2026-05-29.** A reflection callback
  crosses the user-space bind point as **bytes only** — no `cc_type_info*` (or
  any compiler-internal pointer) escapes, so the narrow waist that decouples
  `@comptime` code and compiled factories from compiler internals stays intact.
  Three host symbols are shared by both consumers:

  ```c
  int cc_reflect_field_count(const char* type_name);                       /* -1 if unknown */
  int cc_reflect_field_name (const char* type_name, int idx, char* buf, int buf_sz);
  int cc_reflect_field_type (const char* type_name, int idx, char* buf, int buf_sz);
  ```

  - **Executor path:** injected via `tcc_add_symbol` + externs in the executor
    prelude, so running `@comptime {}` blocks (and `@comptime fn`s) can read a
    struct's fields and drive codegen from that structured input.
  - **Factory path:** declared in the compiled-factory dylib TU and resolved
    against the compiler binary at `dlopen` time (dylibs build with `-undefined
    dynamic_lookup`; the symbols are global `T` in `.ccc-bin`). One contract
    serves both.
  - **Backing:** an on-demand scan of the current source buffer's `struct` /
    `typedef struct {…} Name;` definitions. The global type registry is **not
    yet populated with struct fields** when `@comptime` blocks / factories run
    (see `cc_build_parse_input` ordering), so reflection parses fields from
    source text. A returned `type` spelling can be fed straight back into
    `cc_reflect_field_count` to **recurse** into nested struct fields — recursion
    is preserved without a flattened snapshot or any node graph.
  - **Proofs:** `tests/comptime_reflect_fields_smoke` (executor block reflects a
    struct → emits per-field name/type accessors) and
    `tests/comptime_reflect_factory_smoke` (compiled `Describe::[Widget]` factory
    introspects its struct type arg and emits one accessor per field).
  - **Known v1 limits:** offsets are not provided (emitted C names fields
    directly, so the host C compiler computes layout).  (The original "skip
    nested aggregates / one declarator per field" limit was lifted — see the
    member-declarator parser entry dated 2026-05-31; inline anonymous/nested
    aggregate defs still reflect `-1`.)

- **D6.4 (collapse the builtins) — emission seam LANDED 2026-05-30.**
  Container monomorph declarations now emit through a **default-registered
  container-factory registry** (`cc_emit_plan_register_container_factory` /
  `cc_emit_plan_lookup_container_factory` in `emit_plan.c`).  The built-in
  Vec/Map emitters (`cc__builtin_vec_decl` / `cc__builtin_map_decl`, holding
  the historic `CC_VEC_DECL_ARENA(...)` / `CC_MAP_DECL_ARENA(...)` bodies) are
  registered by default under the kinds `"Vec"` / `"Map"`, and
  `cc_emit_plan_fprint_vec_decl` / `_map_decl` are thin dispatchers that look
  the kind up and call the registered factory.  So the hardcoded path is now
  literally "just the first registered factory," and a library can register an
  additional container kind (or override a built-in) through the same seam
  rather than the compiler special-casing each one.  Output is byte-identical
  (full suite 489/489).  `Chan` has no decl factory yet (the chan branch in
  `cc_preprocess_emit_splice` emits nothing today).
- **Option A (unified generic registry) — LANDED 2026-05-30.**  The three
  historic registries (container-decl factories, declarative generic templates,
  compiled generic factories) are now **one tagged registry** (`cc__generics[]`
  in `emit_plan.c`), keyed by `(name, kind)` where `kind` is:
  - `CC_GENERIC_NATIVE_DECL` — a compiler-native C emitter for a container
    monomorph's declaration (built-in **Vec/Map**, consumed by the type-graph
    emission loop via `cc_emit_plan_lookup_container_factory`);
  - `CC_GENERIC_COMPILED` — a `cc_generic_register` `@comptime` factory compiled
    in-process on the libtcc evaluator (host-cc dylib fallback) and invoked at the
    use site.

  (A third kind, `CC_GENERIC_TEMPLATE` — a declarative `cc_generic_template`
  `$0..$N` / `${...}` string tier — also existed here; it was **removed
  2026-05-31**, see the dated entry below.)

  So built-in containers stop being a *separate* mechanism: they are
  `NATIVE_DECL` entries that share the registry, the lookup, and the per-TU
  lifecycle plumbing with user `Name::[args]` factories (NATIVE_DECL built-ins
  persist; COMPILED is cleared per-TU via `cc__generic_remove_kind`).
  The **use-site invocation contract is a single entry point**,
  `cc_emit_plan_produce_generic_def(...)`, which ensures the factory is compiled
  (in-process on the libtcc evaluator, host-cc dylib only as fallback — see the
  2026-05-31 log below) then invokes it, returning a `CCGenProduceStatus`; the use-site
  rewrite in `cc_rewrite_generic_containers` calls it and owns only the
  use-site-attributed diagnostic.
  - *Map hash/eq as data — done 2026-05-30.* The built-in `cc__builtin_map_decl`
    no longer chooses the hash/eq pair via a hardcoded `if/else` chain; it
    consults a priority-ordered data table (`cc__map_key_hasheq`) with an i32
    fallback. The closed built-in key set is now explicit, and the table is the
    natural place a future *registrable* seam would prepend library-supplied key
    hashers (that step needs a comptime host verb to be user-callable, so it is
    deferred until there is a concrete consumer — building it now would be
    speculative generality with no caller).
  - *Use-site lowering for new container kinds — resolved as a non-goal.* A
    library can already add a brand-new container *kind* end to end through the
    factory seam: `CC_GENERIC_FACTORY(Pair){...}` (or the underlying
    `cc_generic_register`) plus a `Pair::[args]` use site lowers to a
    library-owned typedef and the mangled name
    (`tests/comptime_generic_factory_smoke`). Generalizing the *built-in*
    graph-backed macro/deferred-emission path (the dual-parse `__CC_VEC`/`__CC_MAP`
    placeholder + type-graph machinery) to arbitrary library kinds would re-open
    the blessed closed core; per the closed-core / open-edge split the open edge
    is correctly served by comptime factories, so this is intentionally **not**
    pursued.

- **D6.5 (single instantiation surface) — LANDED 2026-05-30.**  The
  angle-bracket generic spellings are fully retired.  `Vec<T>` / `CCVec<T>` /
  `vec_new<T>` already errored; `Map<K, V>` / `map_new<K, V>` now error too
  (`cc_rewrite_generic_containers` flags them with a migration diagnostic).
  The **single instantiation surface** for both built-in containers and user
  generic factories is the bracket form `Name::[args]` — `CCVec::[int]`,
  `Map::[K, V]`, `Pair::[A, B]`.  Negative tests `vec_legacy_spelling_retired`
  and `map_legacy_spelling_retired` lock the retirement in; the 9 Map smoke
  tests migrated to `::[...]`.  Fixing the migration surfaced a latent bug in
  the shared backward type-scan (`cc_rfind_char_top_level`): an unmatched
  opening `[` to the left now bounds the scan (mirroring the existing `(`
  case), so a slice element type inside a generic-arg list
  (`Map::[char[:], int]`) no longer swallows the container head into `CCSlice`.
  A `comptime.cch` umbrella header now re-exports the whole compile-time API
  (`cc_type` / `cc_instantiate` / `cc_emit_tpl`) so `@comptime` authors learn
  it from one include.  Full suite 490/490.

- **Single factory style — `CC_GENERIC_FACTORY` + template tier removed —
  LANDED 2026-05-31.**  The declarative `cc_generic_template("Name", arity,
  \`...\`)` tier was **deleted** in favor of one generic-lowering mechanism:
  compiled `@comptime` factories.  Rationale (the DX/perf analysis that drove
  it): a declarative template re-parses its `${...}` slot grammar at *every*
  use site (`cc_template_expand_generic` per instantiation), and the moment the
  template stops being a bare string literal (e.g. a computed `CCString`) it can
  no longer be lifted by the static text scanner — it would need the executor to
  produce the string, paying the *same* initial compile as a factory **plus** the
  per-use re-parse.  A compiled factory compiles once (cached `r->fn_ptr`) and is
  a bare function-pointer call per use.  So the template was strictly worse once
  generalized, with no perf argument left.
  - The canonical authoring form is now the **`CC_GENERIC_FACTORY(Name) { ... }`**
    sugar (a seam rewrite in `comptime_prepare.c` → `preprocess.c`'s
    `cc__rewrite_generic_factory`), which lowers to a `@comptime` factory function
    (implicit `generic_name` / `mangled` / `type_args` / `arena` params) plus a
    `cc_generic_register("Name", __cc_gfac_Name)` registration.  Only the
    `CC_GENERIC_FACTORY(Name)` token is replaced; the `{ ... }` body stays put so
    line numbers are preserved.
  - Removed: `CC_GENERIC_TEMPLATE` kind, `cc_emit_plan_{register,lookup}_generic_template`,
    the `cc_generic_template` collector + intrinsic-table entry + inline stub,
    `cc_template_expand_generic` / `cc_template_normalize_legacy_positional`
    (template_scan.c), the `$0/$1` legacy + adjacent-C-string-literal grammar,
    `CC_GENERIC_TEMPLATE_MAX`, and the `tmpl`/`use_factory` params of
    `cc_emit_plan_produce_generic_def`.  Migrated test sites:
    `comptime_generic_factory_smoke`, `comptime_canonical_name_smoke`.  Full suite
    506/506.

- **Arena-backed `@emit` + factory ergonomics + in-process factory compile —
  LANDED 2026-05-31.**  Three changes that finish the single-factory DX and close
  out the dylib path for generic factories:
  - **Arena-backed `@emit`.**  `@emit` lowering moved off a fixed static buffer
    onto a `CCString` over a `CCArena` (stack-first, heap-spill).  The two forms
    are now grammatically distinct: the return form `@emit(\`...\`, arena)` **must**
    take the caller's arena (the returned `CCSlice` points into it; the caller
    persists/copies before freeing), and the anchored form `@emit(anchor, \`...\`)`
    takes **no** arena (it declares a private `CC_ARENA_STACK`, builds, splices via
    `cc_emit_raw`, frees).  Supplying the wrong arity is a hard error.  The host
    passes a `CC_ARENA_STACK(…, CC_EMIT_TPL_BUF_SIZE)` to each factory invoke
    (`cc_emit_plan_invoke_generic_factory`); the returned definition is still
    bounded by the splice buffer (8192).  Retired the static-buffer
    `cc_emit_tpl_*` push/rewrite helpers.
  - **`CC_GENERIC_FACTORY` sugar ergonomics.**  The seam rewrite
    (`cc__rewrite_generic_factory`) now auto-voids the implicit params (bodies
    needn't `(void)…`), accepts an optional integer arity
    `CC_GENERIC_FACTORY(Name, N)` that injects the standard
    `if (type_args.len < N || !mangled.ptr) return cc_slice_empty();` guard, and
    the prelude defines `#define arg(i) (type_args.items[(i)])` (gated on
    `CC_COMPTIME_EXEC`) as shorthand for the type-arg slots.  Body line numbers are
    still preserved (the `)`..`{` span is copied verbatim).
  - **In-process factory compile (dylib path retired for factories).**  Isolated
    factory bodies now compile on the libtcc comptime evaluator
    (`cc_comptime_exec_compile_tu` / `_lookup_symbol` / `_release` in
    `executor.c`, sharing `cc__exec_new_state` with `@comptime` block execution)
    instead of a `posix_spawn` of the host cc + `dlopen` of a `.dylib`.  First-use
    lowering drops from a process spawn to milliseconds, and the factory runs in
    the *exact* environment as blocks (this is the C5 / D3.2 "no dylib for hooks"
    milestone, for generic factories).  The relocated `TCCState` stays resident in
    the hook module and is `tcc_delete`'d on `owner_free`.  `hook_compile.c` tries
    this for any `isolated_body` and falls back to the host-cc dylib on any failure
  (or when libtcc is unavailable).  A `cc__exec_in_block` flag gates the
  executor's timeout `longjmp` so a factory invoked later at a use site (outside
  any `setjmp`) can call host verbs safely.  The on-disk dylib content cache no
  longer applies on this path (in-process compile is already ms-fast).  Full
  suite 506/506.

- **Extension factories (`CC_GENERIC_FACTORY_EXTEND`) — LANDED 2026-05-31.**
  Operations on a generic type can now be defined separately from — and without
  editing — the base factory that defines the type.  A generic name has one
  *base* (`CC_GENERIC_FACTORY` / `cc_generic_register`) plus any number of
  *extensions* (`CC_GENERIC_FACTORY_EXTEND` / `cc_generic_register_extend`).
  - **Lowering.**  The seam rewrite (`cc__rewrite_generic_factory`) handles both
    keywords: the base lowers to the stable handler symbol `__cc_gfac_<Name>`
    (last-wins registration), each extension to a process-unique
    `__cc_gfac_ext_<Name>_<seq>` so many extensions coexist in the factory TU.
  - **Registry.**  `CCGenericReg` gained an ordered extension list
    (`ext_handlers` / `ext_fns` / `ext_owners`).
    `cc_emit_plan_register_generic_factory_extend` appends, creating the entry if
    the base hasn't registered yet (order across files is irrelevant), and
    `cc_emit_plan_has_generic_factory` is the new use-site gate so an extend-only
    name still reaches the base-required diagnostic.
  - **Dispatch.**  `cc_emit_plan_invoke_generic_factory` runs the base first
    (must emit a non-empty fragment that defines the type), then every extension
    in registration order, concatenating fragments (newline-separated) into the
    single definition emitted once per mangled name — so extensions may reference
    the base's `${mangled}`/fields.  An extension may return `cc_slice_empty()`
    to emit nothing (conditional specialization).
    `cc_emit_plan_ensure_generic_factory` errors at the use site when a name has
    extensions but no base (*"generic 'X' is extended but never defined"*).
  - **Proofs:** `tests/comptime_generic_factory_extend_smoke` (base type + two
    method extensions + a conditional extension that emits only for the `int`
    monomorph) and `tests/comptime_generic_factory_extend_no_base_fail` (extend
    with no base is rejected at the use site).

- **Field reflection: member-declarator parser (partial-reflection fix) —
  LANDED 2026-05-31.**  `cc__ct_parse_fields_from_body` was a char-reject scan
  that bailed the *whole* struct on the first member containing `[ : ( , {`
  (array, bitfield, function pointer, multi-declarator, or inline aggregate).
  It is now a real member-declarator mini-parser (`cc__ct_member_normalize` →
  `cc__ct_parse_member` → `cc__ct_parse_declarator` / `cc__ct_parse_fnptr`),
  shared verbatim by `type_of(T).fields`, `@comptime for`, and
  `cc_reflect_field_*`.  Modeled exactly, one `CCCtField` per declared name:
  - **Multi-declarator** `int a, *b;` → splits, distributing `*` per declarator.
  - **Arrays** (incl. multi-dim / array-of-pointer) → the extent rides in the
    `type` spelling (`char[16]`, `int[2][3]`, `char*[8]`).
  - **Function pointers** `int (*cb)(int,int)` → `int (*)(int, int)` (the name is
    extracted from the `(*name)` group).
  - **Named bitfields** `unsigned f : 4` → base type `unsigned` (width is
    validated as an integer literal, then dropped — not exposed).
  Array / fn-ptr `type` spellings are exact for `sizeof` and `t.f` access but are
  not declaration-prefix-usable.  The contract is unchanged where it matters:
  the parser **never** produces a partial or guessed set — forms it cannot spell
  as a usable `type` (inline anonymous/nested aggregate def, anonymous member,
  unnamed/padding bitfield, pointer-to-array) and unknown types still make the
  whole struct reflect as `-1`.  A 120-byte spelling cap keeps the fixed
  `CCReflectField` 128-byte buffers safe.  Proofs: `comptime_reflect_forms_smoke`
  (multi-decl + array + multi-dim + fn-ptr + bitfield, name+type checked) and
  `comptime_reflect_anon_reject_smoke` (inline-anon + unnamed-bitfield reflect
  `-1`; a clean struct alongside reflects normally), replacing the retired
  `comptime_reflect_array_reject_smoke` (arrays are now modeled).  Note: as
  before, reflection host verbs only run when a `@comptime {}` block routes
  through the executor (e.g. it contains a `for` loop); a no-loop block that only
  calls `cc_reflect_field_*` is a separate, pre-existing limitation.

- **Edge-push #1 — enum reflection — LANDED 2026-05-30.**  `cc_reflect_enum_count`
  / `cc_reflect_enum_name` / `cc_reflect_enum_value` (plus the `cc_reflect_enum_at`
  value sugar) mirror the struct-field reflection: a source-scan
  (`cc_ct_reflect_enum_members`) parses `enum`/`typedef enum` members with C
  auto-increment semantics, and the bytes-only/scalar verbs are injected into the
  libtcc executor and exported as real globals for dynamic_lookup dylibs.
  Explicit initializers must be integer literals; a non-literal initializer
  (`A | B`, `1 << 2`, `'c'`) makes the whole enum unreflectable — every member or
  none, never a guess (same contract as the struct reflector).  This is the
  enum↔string "first-week pattern".  Proof: `comptime_reflect_enum_smoke`.

- **Edge-push #4 — custom domain diagnostics — LANDED 2026-05-30.**
  `cc_emit_error` / `cc_emit_warning` are the dual of `cc_emit_raw`: a `@comptime`
  block (or compiled factory) raises a compiler diagnostic for a constraint it
  checks itself.  `cc_emit_error` marks the exec pass failed so the build stops
  exactly like a built-in diagnostic; `cc_emit_warning` is advisory.  Both are
  real globals (executor + dylib), attributed to the enclosing `@comptime`
  block's source line via the existing `cc__host_site_pos` seam (block-level for
  now; finer call-site attribution is the emit-provenance milestone, edge-push
  #5).  Diagnostic-raising blocks are routed through the executor (they are
  runtime side effects, not statically collectible emit text).  Proof:
  `comptime_emit_diag_smoke` (warning path) and `comptime_emit_error_fail`
  (error fails the build with the library-authored message).  Full suite 493/493.

- **Edge-push #2 — type-kind classifier — LANDED 2026-05-30.**  `cc_reflect_kind`
  returns a `CC_REFLECT_KIND_*` code (UNKNOWN/PRIMITIVE/POINTER/STRUCT/ENUM) for a
  type spelling, so a recursive serializer can decide per field whether to recurse
  (aggregate), table-map (enum), pointer-handle, or emit a scalar leaf.  Backed by
  `cc_ct_reflect_type_kind`, which classifies via the *body-finders* (not the field
  parser), so a struct with an unmodeled member still classifies as STRUCT; pointer
  is any spelling ending in `*`; primitive is every word a C scalar keyword; leading
  `const`/`volatile` are tolerated.  Deliberately small v1: cv-qualified aggregates
  and typedef-to-primitive aliases report UNKNOWN.  This work also fixed a
  pre-existing limitation — the struct field parser now skips a block comment
  trailing a member's `;` (same fix the enum reflector got).  Proof:
  `comptime_reflect_kind_smoke`.  Full suite 494/494.

- **Edge-push #5 — emit provenance — LANDED 2026-05-30.**  Every spliced comptime
  fragment is now wrapped with `#line` directives: an **origin** directive before
  the text so a downstream C-compiler error in generated code maps to the
  template/emit source, and a **restore** directive after so following user code
  keeps correct attribution.  Line numbers are computed up front against the
  line-aligned body buffer (insertions don't perturb them).  `cc_emit_raw_at`
  stamps an explicit origin (e.g. a template-literal's file:line) on a fragment;
  `cc_emit_error_at` / `cc_emit_warning_at` are the explicit-origin diagnostic
  variants (the plain forms stay block-level).  Raw/at-origin emits route through
  the executor (runtime side effects).  Applied in both emission paths — the
  preprocess fprint path and the codegen splice path.  Proof:
  `comptime_emit_provenance_smoke` (origin `#line` verified in generated C).
  Full suite 495/495.

- **Naming / composition — cc_canonical_name + dup detector — LANDED 2026-05-30.**
  `cc_canonical_name(base, args, nargs, …)` exposes the *exact* mangled name the
  rewriter uses for `base::[args]` (backed by `cc_ct_canonical_name`, the same
  routine, so it can't drift).  Blessing one recipe (see `GENERIC_MANGLING.md`)
  lets independent libraries' private generics name the same C symbol and compose,
  instead of each inventing an incompatible scheme.  Riding on emit provenance, a
  **dup-emit-name detector** (`cc_emit_plan_warn_duplicate_symbols`) warns — loud,
  never silent — when two fragments define the same top-level **function** symbol
  (C's one genuinely-silent link footgun), naming both origins.  It scans all
  collected fragments and is called from both emission paths.  Proof:
  `comptime_canonical_name_smoke` (helper matches the compiler's `Pair_int_double`)
  and `comptime_dup_emit_name_fail` (collision surfaced with both origins).
  Full suite 497/497.

- **Edge-push #3 — tag-filtered reflection — LANDED 2026-05-30.**
  *Surface decision (surface ≡ lowering):* the tag is an **opt-in, advertent
  comment marker** — the token `@tag:NAME` inside a single-line block or line
  comment immediately preceding a top-level function definition.  No new syntax,
  no parser/pass surface, purely a source-scan — exactly like the `/*CC_CLO:N*/`
  closure marker.  Comments survive into the reflect buffer (the same buffer the
  struct/enum reflectors read), and `@` never appears in C outside comments/
  strings, so the marker is unambiguous and zero-cost.  `cc_reflect_tagged_count`
  / `cc_reflect_tagged_name` (backed by `cc_ct_reflect_tagged_fns`) collect, in
  source order, the names of all functions carrying a tag, so a comptime block
  builds a registry / dispatch table with no central list to maintain.  v1 scope
  is functions (the registry pattern); the function name is the identifier before
  the first top-level `(` after the marker.  Proof: `comptime_reflect_tagged_smoke`
  (three `@tag:command` functions → a dispatch table; an untagged function is
  excluded; forward-decls + table emitted together — the realistic generator
  pattern).  Full suite 498/498.

  *Scanner comment-hardening (done):* both raw-text `@comptime` scanners — the
  block enumerator `cc__emit_for_each_comptime_block` (emit_plan.c) and the
  `@comptime` *function* registry scanner `cc_comptime_fn_registry_scan`
  (executor.c) — now track comment/string state and only attempt a match at an
  `@` in real code.  Previously prose containing the literal `@comptime` (e.g. a
  doc comment) was mis-read: the function scanner in particular would grab the
  next `name(...) { ... }` after the comment as a bogus comptime function and
  poison the executor TU.  Regression guard: `comptime_reflect_tagged_smoke` now
  carries `@comptime` in its header comment on purpose.

- **Invalid-emit diagnostic contract + `--emit-c-inspect` — LANDED 2026-06-01.**
  When a compiled factory emits C, the generated definition is validated at the
  *emit site* before it is spliced — `cc_comptime_validate_c_fragment`
  (executor.c) compiles the fragment in a minimal prelude and reports **only
  syntax errors** (`cc__frag_msg_is_syntax`; missing-context errors like unknown
  types are deliberately swallowed, since the fragment is judged out of its real
  TU context).  A reported error is attributed to the **use site** (`Name::[…]`
  file:line:col) and carries:
  - the full generated definition, line-numbered, with the offending line flagged
    by `>` (windowed to ±3 lines when the def exceeds 40 lines);
  - a `note: in @comptime factory '<handler>' at <file>:<line>` whose origin is
    resolved through `#line` directives (so header-harvested factories blame the
    `.cch` the user wrote, and `.ccs` factories fall back to the use-site file).

  *Coverage caveat (honest):* this is a **syntax-only** gate.  Semantically wrong
  but syntactically valid emits still surface downstream from the host compiler.
  Closing that needs the span-map work (pass emitted defs through to codegen so
  the host compiler validates the whole TU, with error spans remapped) — tracked,
  not yet done.

  *`--emit-c-inspect[=PATH]`* dumps the merged translation unit for inspection
  (default `out/<stem>.inspect.c`).  On a clean lowering it is the full pre-parse
  merged TU (dumped in `cc_build_parse_input` after canonicalization); when the
  build fails in a generic factory it is the TU **reconstructed in source context
  up to the first blocking error** (at the `cc_rewrite_generic_containers` failure
  site — the merged TU is never flushed otherwise, because unparseable C never
  reaches codegen).  The build still runs and fails as usual; the flag only adds
  the artifact.  Plumbed via the `CC_EMIT_C_INSPECT` env (set per input in
  `cc__compile_with_env`), so no pass signatures widen.  Default builds no longer
  write any temp sidecar — they print `note: re-run with --emit-c-inspect …`.
  Proofs: `comptime_factory_invalid_emit_fail`, `comptime_emit_bad_slot_fail`,
  `comptime_factory_line_provenance_fail`, `comptime_factory_in_header_bad_fail`
  (all assert the use-site error, the `>`-flagged def echo, and the file:line
  factory note).  Full suite 513/513.

---

## E0 — comptime executor keystone (Stage 0: real `@comptime {}` execution)

**Goal.** Replace "inert emit stubs + text scanning" with a real **in-process
executor** so running `@comptime` code can call live compiler APIs. This is the
foundation for spec §14.2/§14.5 (`@comptime` functions, `@comptime {}` loops).

**Engine (`cc/src/comptime/executor.c`).** Generalizes the D3.0 libtcc path
(`TCC_OUTPUT_MEMORY`): compile a driver TU wrapping the block body as
`__cc_ct_entry()`, register host symbols via `tcc_add_symbol`, relocate, run.
Watchdog via `longjmp` + `CC_COMPTIME_EXEC_TIMEOUT_MS` (default 5000ms), checked
on each host callback.

**Comptime C surface (libc + file I/O).** The executor TU is a real libtcc
translation unit linked against the system libc, so `@comptime` blocks may use
"as much C as possible," not only the host verbs.  The generated prelude
(`emit_tpl_prelude.inc.h`, from `tools/gen_emit_tpl_prelude.sh`) `#include`s
`<stddef.h> <stdio.h> <stdarg.h> <string.h> <stdint.h> <stdlib.h> <stdbool.h>
<ctype.h>`, so `fopen`/`fgets`/`fwrite`/`sscanf`, `malloc`/`getenv`/`atoi`,
`tolower`/`isalpha`, etc. all resolve and run at compile time.  This lets a block
read a data file (codegen tables, schemas) and bake the result into the program.
Proof: `tests/comptime_fileio_smoke` (write→read a scratch file, transform lines
with ctype+stdlib, emit functions).  Adding a header is a one-line edit to the
generator's `HDR` block followed by `gen_emit_tpl_prelude.sh`.

**Host API (`cc_emit_plan_host_*` in `emit_plan.c`).** Injected into the running
TU:
- `cc_emit_raw(anchor, ptr, len)` → emit-plan fragment buffer (coalesces
  consecutive emits at the same anchor/site into one splice block)
- `cc_instantiate_vec/map/chan` → comptime-instantiation list
- `cc_reflect_field_count/name/type` → struct-field reflection over the current
  source buffer (D6.3; shared with compiled factories via dynamic_lookup)
- `cc_type_of` → still a NULL stub; the bytes-only field reflection above is the
  landed reflection surface (a richer `cc_type_info` snapshot remains future work)

**Wiring.** `cc_emit_plan_exec_comptime_blocks` scans `@comptime {}` blocks;
those containing `for`/`while`/`do` or calls to registered `@comptime` functions
run through the executor (static text collection skipped for executed blocks).
Top-level `@comptime int fib(...)` defs are scanned into the executor TU prelude.
`cc_comptime_exec_eval_int` marshals integer results via `__cc_ce_result`.
Invoked on both parse path (`build_parse_input.c`) and emit path
(`visit_codegen.c`).

**E0.1 landed (2026-05-29):** `@comptime fn` calls + integer result marshaling.
Registry scan (`cc_comptime_fn_registry_scan`), executor TU includes collected
defs, blocks calling registered comptime fns route through libtcc. Proof:
`tests/comptime_fn_fib_smoke` — recursive `fib(10)`/`fib(15)` inside
`cc_emit_format` at compile time.

**D6.1 landed (2026-05-29):** compiled generic factories via
`cc_generic_register("Pair", pair_factory)` + dylib batch
(`CC_COMPTIME_TYPE_HOOK_GENERIC_FACTORY`, isolated handler TU via
`cc_comptime_compile_type_hooks_tu`). Factory returns definition text as
`CCSlice`; rewrite/emit/dedup unchanged from D6.0. Proof:
`tests/comptime_compiled_generic_factory_smoke`.

**Prerequisite fixes (landed with E0):**
- **Fix A:** `cc_type_of("T")` → `type_of(T)` normalization at
  `cc__resolve_comptime_if` entry; `@comptime for` header accepts canonical form
  only (dual-handling removed from the resolver; `pass_check_type_of` still accepts
  both spellings for user code).
- **Fix B:** include-expanded view (`cc_cpp_expand`) for `cc__ct_find_struct_body`
  only — header-defined structs visible to `@comptime for`. Local-buffer prelude
  kept for TCC layout predicates (expanded headers poison the evaluator).

**Proof:** `tests/comptime_crc_table_smoke` — `@comptime {}` loop builds a CRC32
table at compile time via `cc_emit_format`, runtime-verified against known values.
Suite **477/477** (one known-flaky nursery timing test under parallel load).

**Deferred (re-decide after keystone):** value-param generics (D6.2),
types-as-values, converging static intrinsic collectors onto the executor.

**D3.1 landed (2026-05-29):** layout-aware `@comptime if`, consuming the D3.0
seam. **(A)** When the self-contained D2 evaluator can't decide a predicate, the
resolver falls back to `cc__comptime_eval_pred_via_tcc`: it D1-lowers the
predicate (`type_of(T).size` → `((size_t)sizeof(T))`, `.align` → `_Alignof(T)`),
prepends a small prelude (`size_t`, `true`/`false`, the `CC_TK_*` enum), and asks
the in-process TCC evaluator for the value — so `sizeof`/`_Alignof`/
`__builtin_offsetof` and pure host-C constant expressions now decide branches
with real target-ABI numbers. The "layout out of scope" line from D2.0 is lifted
for primitives + C exprs. **(b)** User-declared types resolve too:
`cc__extract_type_decls_prelude` scans the source's top-level type *definitions*
(`typedef …;` and bare `struct/union/enum [TAG] { … };`; declarators like
`struct S {…} g;` and CC-tainted spans carrying `@`/`::` are skipped
individually) and appends them to the prelude, built lazily once per resolve
sweep. So `type_of(UserStruct).size`/`.align`, `sizeof(struct Tag)`, and
`__builtin_offsetof(UserStruct, m)` fold at compile time. *Boundary:* a def that
pulls in header-only types fails to compile in the bare prelude → that predicate
falls through to the hard error (graceful); per-type/transitive-dep extraction is
a later refinement. Predicates needing an unclassified type's *kind* still error
(layout ≠ kind). Tests: `comptime_if_layout_smoke` (primitive layout + C exprs)
and `comptime_if_user_layout_smoke` (user `typedef`/tagged-struct size/align/
offsetof; dead arms hold undefined symbols → proves pruning). Suite **471/471**
both default and `CC_PRE_EXPAND=0`.

**D3.0 landed (2026-05-29):** `cc_tcc_eval_const_expr(prelude, expr, &out)` —
in-process C constant-expression evaluation over the **public libtcc API only**
(no vendored-TCC change). Compiles `<prelude>\nlong long __cc_ce_result =
(long long)(<expr>);` to `TCC_OUTPUT_MEMORY`, relocates, reads the symbol. Gives
full target-ABI fidelity — `sizeof`, `_Alignof`, `__builtin_offsetof`, enum
constants, integer arithmetic — i.e. exactly the layout facts the self-contained
D2 evaluator deliberately could not compute. Relocation needs `libtcc1.a`; the
seam resolves its dir via `CC_TCC_LIB_PATH` env → build-time `CC_TCC_LIB_DIR`
(absolute, baked in the Makefile) → cwd-relative fallbacks, so it works from any
cwd (verified). Returns 0 (never aborts) when libtcc is unavailable or the expr
isn't constant. Self-test: `ccc __eval-const --selftest` (asserts arithmetic /
`sizeof` / `_Alignof` / enum / struct layout / non-constant rejection); wired
into `scripts/test.sh` as a suite guard since the .ccs harness can't reach a
compiler-internal. This is the **seam only** (chosen first, per the A/B fork);
the consumer — layout-aware `@comptime if` (A) or a constant-hook fast path
replacing dylib compilation (B) — is D3.1.

**D2.1 landed (2026-05-29):** two extensions to the same pass.
(1) *Chaining* — `else @comptime if (...) { ... }` arms via a recursive extent
finder (`cc__ct_if_extent`): a false head splices the nested `@comptime if`
verbatim and the fixpoint loop resolves it; a true head drops the whole tail.
The else arm must be `else { ... }` or `else @comptime if ...` (a bare runtime
`else if` is a clear error). (2) *Short-circuit* — `cc__ce_skip_balanced`
consumes one logical operand without requiring it to fold, so `KNOWN_TRUE ||
<non-foldable>` and `KNOWN_FALSE && <non-foldable>` are decided without the dead
operand being compile-time constant (honors nested parens/brackets + string/
char literals; unbalanced → not decidable). Test: `comptime_if_chain_smoke`
(head/middle/tail chain selection with undeclared symbols in dead arms; `||`/
`&&` short-circuit over `type_of(Unregistered).kind`). Smoke 469/469 both modes.

**D1.1 landed (2026-05-29):** same pass now folds the structural members:
`.name` → `"T"` (constexpr string literal); `.kind` → `CC_TK_PRIMITIVE` /
`CC_TK_GENERIC_INST` for the reserved primitive + container (`CCVec_`/`Map_`/`CCChan`)
name sets (constexpr enum constant), else the runtime `cc_type_of("T")->kind` read;
`.nfields` → `0` for primitives (constexpr), else the runtime read. Members the
compiler can't decide by name fall back to the same `cc_type_of(#T)->m` the bare
macro produces, so every `type_of(T).<member>` compiles and the known cases are
ICEs. `type_of_constexpr_smoke` extended (registered `TOPair` exercises both the
constexpr and runtime-forward paths). Smoke **466/466** both modes.

**Suggested order:** A1 → A2 → A4 (Map) → B1 → C1 → C2 → …  
Track D can parallelize after A2.

**Progress (2026-05-29):** A1, A2, A4, B2, C1, B3, B4, C1b landed + comptime
scanner unification fully done (§1b: shared lexers + one block recognizer
across `emit_plan.c` and `symbols.c`) + the `emit_plan` comptime-intrinsic
`{name → handler}` registry (§1b "Registry (done)"); A3 partial (arm-body
centralized). **`CC_PARSER_MODE` shrink: Map stub deleted** — Vec + Map now use
one real body in both modes via parser-safe `ccj_*` forward decls; also fixed
the latent pointer-type bug (mangled `intptr` token reaching the real body) via
a lowering→reparse type memo (see `COMPILER_CLEANUP_STATUS.md`). **Result
fallback shrunk to irreducible core** — eleven dead generic helpers
(`__cc_result_generic_ok/err` + nine `__CCResultGeneric_*` accessors) removed;
a spike confirmed the residual `__CCGenericError`/`__CCResultGeneric` tags +
`cc_ok`/`cc_err` stubs are load-bearing (the latter pinned to TCC-ext's channel-
method UFCS stub). Smoke green at **465/465** default + `CC_PRE_EXPAND=0`.
**C3 scoped** (see `COMPILER_CLEANUP_STATUS.md` "C3 scope"): correcting an earlier
overstatement — only **Result** is TCC-name-coupled (channel `.recv()/.send()`
UFCS placeholder return type); **CCTask → int** and **CCClosure → intptr_t** are
header stubs removable CC-side (ordering / earlier closure lowering), *not* C3.
The dead `__CCOptionalGeneric` TCC branch was **removed (2026-05-29)**.
`__CCResultGeneric` itself is a bounded, gated residual — **not a Track-D blocker**.
**Next:** open Track D (real `@comptime` evaluator) behind the now-unified
intrinsic surface; optionally bank the dead-`__CCOptionalGeneric` removal first.

---

## 9. Invariants (ratchet rules)

1. **One graph per TU** — no new `cc_type_registry_get_global()` callers; thread `CCTypeGraph*`.
2. **Every monomorph has an emission recipe** — builtins use `cc_gen_*`; users use `@comptime` + `cc_emit_*`.
3. **UFCS resolves through the graph** — no new string tables beside graph protocols.
4. **`cc_type_info` pointer identity** — one node per mangled name per binary; comptime `type_of` reads the same node.
5. **Anchors are explicit** — no top-of-file container blocks; prelude types always precede impl emission.
6. **`CC_PARSER_MODE` only shrinks** — each commit removes a stub branch or define site; never add new ones.

---

## 10. Success criteria

- [x] User can write `@comptime { cc_emit_cstr(CC_EMIT_AFTER_PRELUDE, "..."); }` and see it in the TU at the correct line. (B2)
- [x] `CCVec::[T]` / `Map::[K,V]` collected as graph requests; emission via plan; explicit `cc_instantiate_*` from `@comptime` honored. (A1/A2/C1)
- [x] Map `CC_PARSER_MODE` impl branch removed (parser stub now lives in `map_forward.cch`); UFCS map smokes unchanged. (A4)
- [x] `cc_type_register` documented as legacy alias of `cc_type_define`. (B3 — both spellings scanned identically)
- [ ] `CC_PARSER_MODE` undefined in normal builds; stub-AST behind `CC_STUB_AST`.
- [ ] Spec §9 UFCS + §14 comptime examples have a single implementation story in this doc.

---

## 11. Open questions

1. **Graph persistence across TUs** — link-time merge for `cc_type_info` IDs (deferred; `id` field reserved).
2. **Header units** — can `.cch` carry instantiation requests without linking? (Treat as separate graphs merged at link via symbol names.)
3. **Error messages** — instantiation failures should cite the *request site* (Vec push) not the emission anchor.
4. **Binary size policy** — spec "selectable link-time footprint": graph supports lazy `cc_instantiate` vs eager prelude (build flag).

---

## 12. Relation to existing docs

- **COMPILER_CLEANUP_STATUS.md** §4d/L3 — Vec ordering fix; Map/Task/Closure stubs; this doc supersedes the removal *order* with the seam model.
- **COMPTIME_REAL_WORLD_ANALYSIS.md** — built-in data structures stay macro monomorph; comptime invests in introspection + user factories, not reimplementing `CC_VEC_DECL` in Zig style.
- **spec §9 / §12 / §14** — normative target; this doc is the compiler's consolidation plan.
