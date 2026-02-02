# CCC Pass Inventory

This document maps all compilation passes and preprocessing transforms, with consolidation candidates.

**Last updated**: 2026-02-01

## Current Stats

- **Total lines**: ~21k across pass files
- **TCC reparses**: 4 (spawn+nursery+arena batched)
- **Text-based passes in preprocess.c**: 19 functions
- **AST-based passes**: 8

## Preprocessing (preprocess.c) — ~4,800 lines

Text transforms applied BEFORE TCC parsing. Listed in execution order:

| # | Function | Transform | Lines | Notes |
|---|----------|-----------|-------|-------|
| P1 | cc__rewrite_with_deadline_syntax | `with_deadline(ms)` → CCDeadline scope | ~240 | Control flow |
| P2 | cc__rewrite_match_syntax | `@match` → switch + cc_chan_match_select | ~310 | Channel select |
| P3 | cc__rewrite_slice_types | `T[:]` → CCSlice_T | ~110 | Type syntax |
| P4 | cc__rewrite_chan_handle_types | `int[~4 >]` → CCChanTx_int | ~510 | Channel types |
| P5 | cc_rewrite_generic_containers | `Vec<T>` → Vec_T | ~250 | Generic types |
| P6 | cc_rewrite_ufcs_container_calls | `v.push(x)` → Vec_T_push | ~160 | Container UFCS |
| P7 | cc_rewrite_std_io_ufcs | `std_out.write()` → printf | ~230 | I/O UFCS |
| P8 | cc__rewrite_optional_types | `T?` → CCOptional_T | ~100 | Type syntax |
| P9 | cc__rewrite_optional_constructors | `cc_some_CCOptional_T(v)` → macro | ~120 | Parse stub |
| P10 | cc__rewrite_inferred_result_ctors | `cc_ok(v)` → `cc_ok_CCResult_T_E(v)` | ~260 | Constructor inference ⚠️ BEFORE P11 |
| P11 | cc__rewrite_result_types | `T!>(E)` → CCResult_T_E | ~155 | Type syntax |
| P12 | cc__rewrite_result_constructors | `cc_ok_CCResult_T_E(v)` → macro | ~70 | Parse stub |
| P13 | cc__normalize_if_try_syntax | `if @try (` → `if (try ` | ~25 | Syntax normalize |
| P14 | cc__rewrite_try_binding | `if (try T x = expr)` → expanded | ~150 | Result unwrap |
| P15 | cc__rewrite_try_exprs | `try expr` → `cc_try(expr)` | ~95 | Result unwrap |
| P16 | cc__rewrite_optional_unwrap | `*opt` → `cc_unwrap_opt(opt)` | ~230 | Optional unwrap |
| P17 | cc__rewrite_closing_annotation | `@closing(ch)` → sub-nursery | ~150 | Channel lifecycle |
| P18 | cc__rewrite_cc_concurrent | `cc_concurrent { }` → closure exec | ~70 | Concurrency |
| P19 | cc__rewrite_link_directives | `@link("lib")` → linker comment | ~460 | Link directives |

**Note**: P10 must run before P11 (needs to see `T!>(E)` syntax for type inference).

**Consolidation candidates:**
- P8+P9+P16 → single "optional pass" (3 scans → 1)
- P11+P12 → single "result types pass" (2 scans → 1, P10 stays separate due to ordering)
- P3+P4+P5 → single "type syntax pass" (potential, needs analysis)

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

4. **Consolidate preprocessor type transforms** (P8+P9+P16, P11+P12)
   - Currently: 19 separate text scans
   - Target: ~12 scans (merge related passes)
   - Note: P10 must stay separate (ordering constraint)

5. **Clean up pass chaining** in cc_preprocess_to_string_ex
   - Current: 18 repetitive 3-line blocks
   - Target: Helper macro for cleaner code

### Low Value (cleanup)

6. **Inline small passes** — strip_markers (104 lines) into visit_codegen.c
7. **Extract reparse helper** — common pattern used 5 times

## Immediate Cleanup: Pass Chaining Helper

Current pattern (repeated 18 times):
```c
char* rewritten_X = cc__rewrite_X(cur, cur_n, input_path);
const char* curN = rewritten_X ? rewritten_X : cur;
size_t curN_n = rewritten_X ? strlen(rewritten_X) : cur_n;
```

Target: Clean helper that tracks allocations for cleanup.

## Next Steps

1. ✅ Update this inventory to match reality
2. Create pass chaining helper in preprocess.c
3. Refactor cc_preprocess_to_string_ex to use helper
4. Run tests to verify no regressions
5. Then: Consider merging P8+P9+P16 (optional passes)
6. Then: Consider merging P11+P12 (result passes)
