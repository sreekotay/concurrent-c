# Minimal TCC Patch Design

Goal: Reduce TCC patch from ~1300 lines to ~150 lines by externalizing CC logic.

## Current State

| File | Lines Added | Purpose |
|------|-------------|---------|
| tccgen.c | ~1300 | Statement/expr parsing, AST recording, UFCS |
| tcc.h | ~100 | State fields, node types |
| tccpp.c | ~30 | `=>` token |
| libtcc.c | ~150 | Parse API, ext_parser setup |

## Proposed External Functions

```c
// Declared in tcc.h, implemented in cc/src/parser/cc_ext_parser.c

// Statement hook - handles @arena, @defer, @nursery, spawn
// Returns 1 if consumed, 0 to continue normal parsing
int cc_ext_try_stmt(void);

// Expression hook - handles closures, await
// Returns 1 if consumed (result on vstack), 0 to continue
int cc_ext_try_expr(void);

// Unary hook - handles await prefix
int cc_ext_try_unary(void);

// UFCS hook - called when member access might be UFCS
// Fills cc_last_recv_type, returns 1 if UFCS detected
int cc_ext_check_ufcs(int member_tok);

// AST recording (could stay in TCC or move external)
void cc_ext_record_call(int flags);
void cc_ext_record_node_start(int kind);
void cc_ext_record_node_end(void);
```

## Minimal TCC Hooks

### tccgen.c - Statement Hook (~10 lines)
```c
// In block(), before switch(t)
#ifdef CONFIG_CC_EXT
    if (cc_ext_try_stmt())
        goto again;
#endif
```

### tccgen.c - Expression Hook (~10 lines)
```c
// In unary(), at default case
#ifdef CONFIG_CC_EXT
    if (cc_ext_try_expr())
        break;
#endif
```

### tccgen.c - UFCS Hook (~20 lines)
```c
// In postfix, member access
#ifdef CONFIG_CC_EXT
    if (cc_ext_check_ufcs(tok)) {
        // UFCS detected, method call already set up
        continue;
    }
#endif
```

## Size Comparison

| Component | Current | After |
|-----------|---------|-------|
| tccgen.c additions | ~1300 | ~50 |
| tcc.h additions | ~100 | ~100 (state fields needed) |
| tccpp.c additions | ~30 | ~30 (token needed) |
| libtcc.c additions | ~150 | ~50 |
| **Total TCC patch** | **~1580** | **~230** |

## Benefits

1. **Smaller TCC patch** - easier to maintain, review, upstream
2. **CC logic in CC codebase** - easier to modify without touching TCC
3. **Cleaner separation** - TCC provides hooks, CC implements extensions
4. **Faster iteration** - change CC code without rebuilding TCC

## Implementation Notes

- External functions need access to TCC internals (tok, next(), etc.)
- Could expose via `tcc_state` or dedicated accessor functions
- CC parser code would `#include` TCC headers for type definitions
