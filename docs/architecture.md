# Concurrent-C Compiler Integration Plan (TCC-Compatible)

## Goals
- Keep source-level compatibility with upstream TCC; minimize fork risk.
- Phase 1 includes comptime (`@comptime if`) and comptime generics (type-returning functions with `typedef Vec(int)` instantiation).
- Single main AST walk (plus a lightweight const-collection pre-pass) covering typing, move/provenance, async semantics, and codegen to C11.

## Repository Layout (proposed)
```
third_party/
  tcc/                 # upstream TCC (submodule or tarball drop)
  tcc-patches/         # tiny patch set: lexer/parser hooks, const-eval API
cc/
  include/             # public headers for runtime/ABI
  runtime/             # scheduler, channels, arenas (C)
  src/
    lexer/             # CC token/lex overlay (plugs into TCC)
    parser/            # CC grammar overlay
    ast/               # CC-specific node metadata (side-tables)
    visitor/           # single-pass visitor + const-pass
    comptime/          # comptime evaluator + generic instantiation cache
    codegen/           # C emission (sync + async state machines)
docs/
  architecture.md      # this file
```

## Minimal TCC Hook Surface (target)
1. **Lexer token hook:** register CC tokens without editing core tables (optional build flag).
2. **Parser extension hook:** allow CC productions to be parsed via callbacks without forking core grammar.
3. **Const-eval API:** expose TCC constexpr evaluator so `@comptime if` and type factories reuse it.
4. **AST extensibility:** allow side-table attachment (or a small optional field set) to avoid structural divergence.

These should be optional/no-op when CC mode is off, so they can be upstreamed or reapplied cleanly.

## Compilation Flow
1. **Pass 0 (const collection):** walk AST, collect consts/comptime functions into symbol table.
2. **Pass 1 (single visitor):**
   - Type resolution & monomorphization on demand (comptime functions returning types).
   - Move/provenance checks (unique slices, send_take eligibility bits).
   - Arena/`@scoped` rules (no escape, no suspension with scoped values).
   - Async semantics (`await` only in `@async`, auto-wrap blocking, `@latency_sensitive` disables batching).
   - Comptime evaluation (`@comptime if` branch selection, comptime calls for generics).
   - Codegen to C11 (sync functions direct, async as state machines).

## Runtime/ABI Notes (Phase 1)
- Slice ABI: `{ptr,len,id,alen}` with bits for unique/transferable/subslice; debug provenance table optional.
- Channels: async vs sync typed; backpressure modes; `send_take` zero-copy only when eligible; async `@match` only.
- Arenas: thread-safe bump allocator; explicit reset/free; slices invalidated on reset.
- Errors: `T?`, `T!E` lower to tagged unions; standard enums `IoError`, `ParseError`, `BoundsError`.
- Stdlib: header-only UFCS-first for String builder, char[:] ops, File I/O (sync/async), Vec/Map, logging, path; server shell later/minimal.

## Immediate Tasks
- Add `third_party/tcc` placeholder and `tcc-patches/` with README describing intended hook points.
- Stub CC overlay directories under `cc/` with README placeholders.
- Draft hook patch list against current upstream TCC (non-invasive, optional).
- Keep diffs tiny and documented for painless rebase.

