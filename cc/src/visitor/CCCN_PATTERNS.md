# Portable Patterns from cccn

This document captures patterns from the experimental `cccn` compiler that can be
incrementally adopted in `ccc` to improve code quality.

## Already Adopted

### 1. CCEditBuffer (batch edits)
- **Source**: cccn's AST-to-AST philosophy
- **Adopted in**: `edit_buffer.h`, `edit_buffer.c`
- **Usage**: Phase 3 passes (UFCS, closure calls, autoblock, await) now batch edits

### 2. CCPassChain (preprocessing chain)
- **Source**: cccn's clean pass separation
- **Adopted in**: `preprocess.c`
- **Usage**: All 19 preprocessing passes use `CC_CHAIN` macro

## Ready to Adopt

### 3. StringSet/StringMap utilities
- **Source**: `cccn/util/string_set.h`
- **Value**: Track variable names, type mappings, global exclusions
- **Use cases**:
  - Closure capture: track globals to exclude
  - Symbol deduplication: avoid re-emitting declarations
  - Type inference: name → type lookups

### 4. Pass Context Pattern
- **Source**: `cccn/passes/pass_closure.c` (`ClosurePassCtx`)
- **Pattern**:
  ```c
  typedef struct {
      int next_id;           // Counter for unique IDs
      StringSet globals;     // Excluded names
      StringMap type_map;    // name → type
  } PassCtx;
  
  static void ctx_init(PassCtx* ctx) { ... }
  static void ctx_free(PassCtx* ctx) { ... }
  ```
- **Value**: Clean state management per pass

## Aspirational (Future)

### 5. Rich AST Types
- **Source**: `cccn/ast/ast.h` (`CCN_TYPE_CHAN_TX`, `CCN_STMT_NURSERY`, etc.)
- **Blocked by**: Would require TCC hook changes
- **Value**: Type-safe AST manipulation vs string parsing

### 6. Single Parse
- **Source**: cccn's "parse once, lower many" philosophy
- **Blocked by**: Closure/async transforms change text significantly
- **Value**: Eliminate 3 reparses (currently 4 total)

### 7. AST-to-AST Transforms
- **Source**: cccn's node replacement approach
- **Blocked by**: ccc's text-offset-based architecture
- **Value**: No string offset tracking bugs

## Pattern Comparison

| Pattern | ccc | cccn | Portable? |
|---------|-----|------|-----------|
| Batch edits | EditBuffer | AST lists | ✅ Done |
| Pass chaining | CCPassChain | implicit | ✅ Done |
| String utilities | text.h only | string_set.h | ✅ Easy |
| Pass context | ad-hoc | ClosurePassCtx | ✅ Easy |
| Rich AST | CCASTNode (flat) | CCNNode (typed) | ❌ TCC hooks |
| Single parse | 4 reparses | 1 parse | ❌ Architecture |

## Recommendation

1. **Copy `string_set.h`** to `util/` - immediately useful
2. **Adopt pass context pattern** - cleaner state management
3. **Keep cccn as reference** - design patterns, not code
