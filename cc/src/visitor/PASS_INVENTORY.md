# CCC Pass Inventory

This document maps all compilation passes and preprocessing transforms, with consolidation candidates.

## Current Stats

- **Total lines**: ~21k across pass files
- **TCC reparses**: 5 (after phases 3, 4, 5, 6, 8)
- **Text-based passes**: 13
- **AST-based passes**: 8

## Preprocessing (preprocess.c) — 4,785 lines

Text transforms applied BEFORE TCC parsing:

| # | Function | Transform | Lines | Notes |
|---|----------|-----------|-------|-------|
| P1 | cc__rewrite_optional_types | `T?` → `CCOptional_T` | ~200 | Type syntax |
| P2 | cc__rewrite_result_types | `T!>(E)` → `CCResult_T_E` | ~250 | Type syntax |
| P3 | cc__rewrite_inferred_optional_ctors | `cc_some(v)` → `cc_some_CCOptional_T(v)` | ~200 | Constructor inference |
| P4 | cc__rewrite_inferred_result_ctors | `cc_ok(v)` → `cc_ok_CCResult_T_E(v)` | ~300 | Constructor inference |
| P5 | cc__rewrite_optional_constructors | `cc_some_CCOptional_T(v)` → compound literal | ~150 | Parse stub |
| P6 | cc__rewrite_result_constructors | `cc_ok_CCResult_T_E(v)` → compound literal | ~150 | Parse stub |
| P7 | cc__rewrite_arena_syntax | `@arena(...)` prologue | ~100 | Scope syntax |
| P8 | cc__expand_optional_decls | Inline typedef expansion | ~100 | Type expansion |
| P9 | cc__expand_result_decls | Inline typedef expansion | ~100 | Type expansion |

**Consolidation candidates:**
- P1+P3+P5+P8 → single "optional types" pass
- P2+P4+P6+P9 → single "result types" pass

## visit_codegen.c Pipeline — 1,214 lines

### Phase 1: Early Text (before TCC parse)

| # | Pass File | Function | Transform |
|---|-----------|----------|-----------|
| 1 | visit_codegen.c | cc__rewrite_closing_annotation | `@closing(ch)` → sub-nursery |
| 2 | visit_codegen.c | cc__rewrite_if_try_syntax | `if try` → result unwrap |

### Phase 2: Text Passes (batched)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 3 | pass_with_deadline_syntax.c | 430 | `with_deadline(ms)` → CCDeadline + @defer |
| 4 | pass_match_syntax.c | 600 | `@match` → switch + cc_chan_match_select |

### Phase 3: Initial AST Passes (sequential, REPARSE #1)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 5 | pass_ufcs.c | 432 | `x.method(y)` → `method(&x, y)` |
| 6 | pass_closure_calls.c | 748 | `c(x)` → `c.fn(c.env, x)` |
| 7 | pass_autoblock.c | 1,260 | Insert cc_block() wrappers |
| 8 | pass_await_normalize.c | 529 | `await expr` → temp binding |

### Phase 4: Channel Syntax (text, REPARSE #2)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 9 | pass_channel_syntax.c | 894 | channel_pair + `int[~4 >]` → CCChanTx |
| 10 | pass_type_syntax.c | 1,319 | slice/optional/result type text rewrites |

### Phase 5: Closure Literals (REPARSE #3)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 11 | pass_closure_literal_ast.c | 2,094 | `() => {...}` → __cc_closure_make_N |

### Phase 6: Structured Concurrency (batched, REPARSE #4)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 12 | pass_nursery_spawn_ast.c | 1,071 | spawn/nursery lowering |
| 13 | pass_arena_ast.c | 505 | @arena prologue/epilogue |

### Phase 7: Defer (text)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 14 | pass_defer_syntax.c | 494 | `@defer stmt;` → inject before } and return |

### Phase 8: Async State Machine (REPARSE #5)

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 15 | async_ast.c | 2,524 | @async fn → state machine |

### Other

| # | Pass File | Lines | Transform |
|---|-----------|-------|-----------|
| 16 | pass_strip_markers.c | 104 | Strip @async/@noblock/@latency_sensitive |
| 17 | checker.c | 1,657 | Semantic checks (slice move, provenance) |

## Consolidation Opportunities

### High Value (reduces reparses)

1. **Merge Phase 3 passes** — UFCS, closure_calls, autoblock, await_normalize
   - All are AST-based, sequential
   - Could share single AST traversal
   - Saves: 0 reparses but cleaner code

2. **Merge Phase 4 + early Phase 5** — channel_syntax + type_syntax + closure_literals
   - Reparse #2 and #3 could become one
   - Saves: 1 reparse

3. **Merge Phase 6** — spawn/nursery + arena
   - Already batched, but could be single pass
   - Saves: 0 reparses

### Medium Value (reduces complexity)

4. **Consolidate preprocessor type transforms** (P1-P9)
   - 9 separate text scans → 2 unified scans
   - One for optionals, one for results

5. **Merge text phases 2+7** — with_deadline + match + defer
   - All produce/consume @defer
   - Could share scanner state

### Low Value (cleanup)

6. **Inline small passes** — strip_markers (104 lines) into visit_codegen.c
7. **Extract reparse helper** — common pattern used 5 times

## Proposed New Pass Structure

```
Phase A: Preprocessing (text, 1 scan each)
  - optional_types (P1+P3+P5+P8)
  - result_types (P2+P4+P6+P9)
  - arena_syntax

Phase B: CC Syntax (text, batched)
  - closing_annotation
  - if_try_syntax
  - with_deadline
  - match_syntax

Phase C: Core AST (single reparse)
  - ufcs + closure_calls + autoblock + await_normalize

Phase D: Channel + Types (text, then reparse)
  - channel_pair + channel_types + slice/opt/result types

Phase E: Closures (reparse)
  - closure_literals

Phase F: Concurrency (batched, reparse)
  - spawn + nursery + arena

Phase G: Control Flow (text)
  - defer

Phase H: Async (final reparse)
  - async state machine
```

**Target: 4 reparses (down from 5), ~30% fewer text scans**

## Next Steps

1. Verify baseline (DONE: 177 tests pass)
2. Consolidate preprocessor type passes (P1-P9)
3. Merge Phase 3 AST passes
4. Merge Phase 4+5 to eliminate one reparse
5. Clean up pass ordering and dependencies
