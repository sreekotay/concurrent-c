# CC Visitor Pipeline

This document describes the lowering passes in `visit_codegen.c`.

## Pass Ordering

Passes are organized into phases. Within each phase, passes may be batched
(sharing an EditBuffer) or run sequentially.

### Phase 1: Text Preprocessing (before TCC parse)

These passes handle syntax that TCC cannot parse directly.

| # | Pass | Input | Output | Notes |
|---|------|-------|--------|-------|
| 1 | generic_containers | `Vec<T>` | `Vec_T` | Monomorphization syntax |
| 2 | ufcs_containers | `v.push(x)` | `Vec_T_push(&v, x)` | Container method calls |
| 3 | std_io_ufcs | `std_out.write()` | `cc_std_out_write()` | I/O helpers |

### Phase 2: Text Passes (batched)

| # | Pass | Input | Output | Notes |
|---|------|-------|--------|-------|
| 4 | with_deadline | `with_deadline(ms) {...}` | CCDeadline scope + `@defer` | **Produces @defer for pass 16** |
| 5 | @match | `@match { case ch.recv(): }` | `switch` + `cc_chan_match_select` | Channel select syntax |

*These two passes share an EditBuffer because they operate on non-overlapping syntax.*

### Phase 3: Initial AST Passes (sequential)

After Phase 2, source is parsed with TCC to get stub-AST.

| # | Pass | Transform | Notes |
|---|------|-----------|-------|
| 6 | UFCS | `x.method(y)` → `method(&x, y)` | General UFCS |
| 7 | closure_calls | `c(x)` → `c.fn(c.env, x)` | Closure invocation |
| 8 | autoblock | Wrap blocking calls | Insert cc_block() wrappers |
| 9 | await_normalize | `await expr` → temp binding | Prepare for async lowering |

*These use whole-file replacement - cannot batch yet.*

### Phase 4: Channel Syntax (text)

| # | Pass | Transform |
|---|------|-----------|
| 10 | channel_pair | `channel_pair(&tx, &rx)` lowering |
| 11 | channel_types | `int[~4 >]` → `CCChanTx` |

### Phase 5: Closure Literals (reparse required)

| # | Pass | Transform |
|---|------|-----------|
| 12 | closure_literals | `\|x\| {...}` → `__cc_closure_make_N(...)` |

*Generates additional prototypes and definitions.*

### Phase 6: Structured Concurrency (batched, reparse required)

| # | Pass | Transform |
|---|------|-----------|
| 13 | spawn | `spawn(...)` → `cc_nursery_spawn*()` |
| 14 | nursery | `@nursery {...}` → CCNursery create/wait/free |
| 15 | arena | `@arena(...) {...}` → CCArena prologue/epilogue |

*These three share an EditBuffer - spawn must be inside nursery.*

### Phase 7: Defer (text)

| # | Pass | Transform | Notes |
|---|------|-----------|-------|
| 16 | defer | `@defer stmt;` → inject before `}` and `return` | **Consumes @defer from pass 4** |

### Phase 8: Async State Machine (reparse required)

| # | Pass | Transform |
|---|------|-----------|
| 17 | async | `@async fn()` → state machine |

*Final lowering pass. Transforms async functions into resumable state machines.*

## Dependencies

```
with_deadline (4) ──produces @defer──▶ defer (16)
spawn (13) ──must be inside──▶ nursery (14)
```

## Future Improvements

1. **Refactor whole-file passes to fine-grained edits** - Would enable batching phases 3 and 4.
   Passes that currently use whole-file replacement:
   - `pass_ufcs.c` - UFCS AST pass
   - `pass_closure_calls.c` - closure invocation
   - `pass_autoblock.c` - blocking call wrappers
   - `pass_await_normalize.c` - await temp binding
   
2. **Merge related passes** - UFCS passes (1-3, 6), channel passes (10-11)
   - These share common logic but have ordering dependencies
   
3. **Reduce reparses** - Currently ~5 TCC parses; could potentially reduce to 2-3
   - Requires careful dependency analysis between phases

4. **Extract channel code** - ✓ DONE
   - `pass_channel_syntax.h/.c` contains channel syntax functions
   - ~606 lines extracted from `visit_codegen.c`

5. **Extract type syntax code** - ✓ DONE
   - `pass_type_syntax.h/.c` contains slice/optional/result type syntax
   - ~815 lines extracted from `visit_codegen.c`
   - `visit_codegen.c` reduced from 2254 to 830 lines (63% reduction)

## Adding a New Pass

1. Create `pass_newfeature.c` and `pass_newfeature.h`
2. Use `#include "visitor/pass_common.h"` for shared infrastructure
3. Implement either:
   - `cc__collect_newfeature_edits(root, ctx, eb)` for fine-grained (preferred)
   - `cc__rewrite_newfeature_syntax(...)` for whole-file
4. Add to `visit_codegen.c` at the appropriate phase
5. Update this document
