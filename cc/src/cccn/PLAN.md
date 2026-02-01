# CCCN Refactor Plan

## Naming Convention

**IMPORTANT**: `cccn` is only the binary name (for "CCC Next").

- Binary: `cccn` (to run alongside `ccc`)
- New AST types: `CCN` prefix = "CC Node" (not "CCCN")
  - `CCNNode` = Concurrent-C Node
  - `CCNSpan` = Concurrent-C Span
  - `CCNFile` = Concurrent-C File
- Functions: `ccn_*` for new AST helpers, `cc_*` for everything else
- No `cccn_` prefixes in code

The `CCN` types are distinct from the existing transitional types (`CCASTNode`, `CCSpan`)
in `src/ast/ast.h` used by the old `ccc` compiler.

## Goals

1. **Replace text-based patching** with proper AST-to-AST transformations
2. **Single parse** → multiple lowering passes → codegen (no reparsing)
3. **Keep existing `ccc`** fully functional while developing `cccn`

## Language Spec Flexibility

**IMPORTANT**: If a language construct is hard to parse or creates compiler complexity,
we have the option to change the language spec. The spec serves the implementation,
not the other way around.

Before implementing a tricky parse:
1. Discuss whether the syntax is worth the complexity
2. Consider alternative syntax that's easier to parse
3. Weigh user ergonomics vs. implementation cost

Examples of past simplifications:
- Channel syntax `int[~N >]` instead of something ambiguous
- `@nursery` block instead of implicit scoping

**Never change syntax without discussion**, but always consider it as an option.

## Design Decisions

### `.cch` files are lowered (DECIDED)

**Decision**: `.cch` files support full CC syntax and are lowered just like `.ccs` files.

This means headers can contain:
- `@async` function declarations
- Channel types like `int[~N >]`
- Result types like `T!>(E)`
- Optional types like `T?`
- Any other CC syntax

Implementation:
- When processing includes, lower `.cch` files through the same pipeline
- Cache lowered headers to avoid re-lowering on every include
- Output goes to `.h` files (or in-memory for TCC)

## Parse-Time Stubs (DECIDED)

**Decision**: Use a "parse-time stubs" header to make TCC accept CC type syntax without complex rewrites.

### Problem

CC introduces new type syntax:
- `T?` — Optional types (e.g., `int?`)
- `T!>(E)` — Result types (e.g., `int!>(CCError)`)

TCC doesn't understand these. Options considered:
1. **Text preprocessing** — Rewrite `T?` → `CCOptional_T` before TCC sees it
2. **TCC type patches** — Modify TCC's type parser to handle `?` and `!>`
3. **Parse-time stubs** — Give TCC placeholder types so it can parse, then codegen emits real types

### Solution: Parse-Time Stubs

We use a minimal preprocessing step (`T?` → `CCOptional_T`, `T!>(E)` → `CCResult_T_E`) combined
with a parse-time stubs header that defines these types as placeholders:

```c
// cc_parse_stubs.h - included automatically during TCC parsing
#define CC_PARSER_MODE 1

// All Optional/Result types resolve to generic placeholders
typedef struct __CCOptionalGeneric { int has; intptr_t value; } __CCOptionalGeneric;
typedef struct __CCResultGeneric { int ok; intptr_t value; } __CCResultGeneric;

// Common types as aliases
typedef __CCOptionalGeneric CCOptional_int;
typedef __CCOptionalGeneric CCOptional_bool;
typedef __CCResultGeneric CCResult_int_CCError;
// ... etc
```

**Why this works:**
- TCC sees valid C types and parses successfully
- The actual struct layout doesn't matter for parsing (no codegen happens in TCC)
- cccn's codegen emits proper `CC_DECL_OPTIONAL`/`CC_DECL_RESULT_SPEC` calls

**Trade-offs:**
- Small preprocessing step still needed for `T?` → `CCOptional_T` rewrite
- But it's simple pattern matching, not complex AST manipulation
- Stubs header is explicit and easy to extend

### Implementation

1. `cc_preprocess_simple()` — rewrites type syntax (`T?`, `T!>(E)`)
2. Parser includes `cc_parse_stubs.h` before TCC parsing
3. Codegen emits actual type declarations

This approach:
- Keeps TCC patches minimal (no type system changes)
- Makes parse behavior explicit (one header to look at)
- Separates concerns (preprocessing vs. parsing vs. codegen)

## TCC Patch Strategy

**CRITICAL**: The `cccn` refactor does NOT add new TCC patches. We reuse the existing minimal hooks.

TCC modifications are kept in `third_party/tcc/` as a submodule with local commits.
Patches are exported to `third_party/tcc-patches/` for documentation/backup.

### Existing TCC Hooks (reused by cccn)

1. **Stub-AST Recording** (`cc_ast_record_start/end`)
   - TCC emits lightweight nodes during parsing
   - Kinds: FUNC, BLOCK, STMT, ARENA, CALL, CLOSURE, IDENT, etc.
   - Each node has: kind, parent, span, aux1/aux2, aux_s1/aux_s2

2. **External Parser Hooks** (`TCCExtParser`)
   - `try_cc_at_stmt()` — `@nursery`, `@arena`, `@defer`
   - `try_cc_spawn()` — `spawn()` statements
   - `try_cc_closure()` — `() => {}` syntax

3. **UFCS Metadata** (`cc_last_*` fields)
   - Records receiver type when `x.method()` is not a real member

### cccn Approach

```
TCC stub nodes  →  CCNNode tree  →  Lowering passes  →  C codegen
     ↑                 ↑
     │                 └── NEW: Rich AST with proper tree structure
     │
     └── EXISTING: Same hooks used by ccc
```

The conversion from TCC stub nodes to CCNNode is in `cccn/parser/parser.c`.
This is where the "magic" happens - transforming flat TCC output into a proper tree.

### Patch Philosophy

**Goal**: Keep TCC patches minimal for easier upstream upgrades.

But: Don't contort `cccn` design to avoid patches. If a small hook addition
makes the code significantly cleaner, that's a good trade-off.

Decision process:
1. Try to derive needed info from existing hook data
2. If awkward, consider a small hook addition
3. Document any new hooks in `third_party/tcc-patches/HOOKS.md`
4. Regenerate patch: `make tcc-patch-regen`

## Architecture

```
Source (.ccs)
    │
    ▼
┌─────────────┐
│   Parser    │  (reuse TCC hooks, populate CCNNode tree)
└─────────────┘
    │
    ▼
┌─────────────┐
│  AST (CCN)  │  ← Rich typed AST with CC constructs
└─────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│           Lowering Passes               │
│  1. UFCS: receiver.method() → func()    │
│  2. Closures: () => {} → struct+thunk   │
│  3. Async: @async fn → state machine    │
│  4. Arena/Nursery: scope → runtime calls│
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────┐
│   Codegen   │  → Emit plain C from lowered AST
└─────────────┘
    │
    ▼
Output (.c) → Host compiler (clang/gcc)
```

## Directory Structure

```
cc/src/cccn/
├── PLAN.md           # This file
├── ast/
│   └── ast.h         # CCNNode definitions (CC Node)
├── codegen/
│   ├── codegen.h
│   └── codegen.c     # AST → C emission
├── passes/
│   ├── passes.h
│   ├── pass_ufcs.c   # UFCS lowering
│   ├── pass_closure.c
│   └── pass_async.c
└── driver/
    └── cccn_main.c   # Entry point for `cccn` binary
```

## Phases

### Phase 1: Foundation ✓
- [x] Directory structure (`cc/src/cccn/`)
- [x] Makefile builds both `ccc` and `cccn`
- [x] Define CCNNode AST structures (`ast/ast.h`, `ast/ast.c`)
- [x] Basic codegen skeleton (`codegen/codegen.c`)

### Phase 2: Parser Integration (current)
- [x] Hook into TCC parser to build CCNNode tree
- [x] Basic conversion: FUNC → CCN_FUNC_DECL with body
- [ ] Improve conversion: capture @nursery, spawn, closures
- [ ] Validate AST matches source constructs

### Phase 3: Lowering Passes
- [x] UFCS pass structure (`pass_ufcs.c`)
- [ ] Test UFCS pass on real examples
- [ ] Closure literal pass
- [ ] Async state machine pass
- [ ] Arena/Nursery scope pass

### Phase 4: Feature Parity
- [ ] Run test suite with `cccn`
- [ ] Compare output with `ccc`
- [ ] Performance benchmarks

### Phase 5: Migration
- [ ] Deprecate `ccc` text-based passes
- [ ] Rename `cccn` → `ccc` (or keep both)
