# TODO: Result Type Declaration Ordering

## Problem

When user code defines a custom struct and uses it in a result type:

```c
typedef struct { ... } CompressedResult;

CompressedResult*!>(CCIoError) compress_block(...) { ... }

// Later usage:
CCResPtr(CompressedResult, CCIoError) res = compress_block(...);
```

The compiler needs to emit:
```c
CC_DECL_RESULT_SPEC(CCResult_CompressedResultptr_CCIoError, CompressedResult*, CCIoError)
```

But the current insertion logic in `visit_codegen.c` inserts this declaration
**before** the `CompressedResult` struct is defined, causing a compilation error.

## Current Workaround

In `pigz_cc/pigz_cc.ccs`, we manually declare the result type after the struct:

```c
} CompressedResult;

CC_DECL_RESULT_SPEC(CCResult_CompressedResultptr_CCIoError, CompressedResult*, CCIoError)
```

## Proper Fix

The result type declaration insertion logic in `cc/src/visitor/visit_codegen.c` 
should:

1. Scan for the struct definition (e.g., `} CompressedResult;`)
2. Insert the `CC_DECL_RESULT_SPEC` **after** the struct definition
3. But still **before** the first usage of the result type

The current heuristic backs up to the "enclosing function" which doesn't work
when the struct definition is between the function start and the usage.

## Files Involved

- `cc/src/visitor/visit_codegen.c` - insertion logic
- `cc/src/visitor/pass_type_syntax.c` - type collection  
- `real_projects/pigz/pigz_cc/pigz_cc.ccs` - workaround applied
