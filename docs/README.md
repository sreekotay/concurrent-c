# Concurrent-C Documentation

## Getting Started

- **[Getting Started](getting-started.md)** — Install, build, and run your first program
- **[Cheatsheet](cheatsheet.md)** — Quick reference for common patterns
- **[Debugging](debugging.md)** — VS Code / Cursor debugging setup

## Reference

- **[Specification](../spec/concurrent-c-spec-complete.md)** — Full language specification
- **[Standard Library](../spec/concurrent-c-stdlib-spec.md)** — Stdlib API reference
- **[Examples](../examples/)** — Working code examples with [learning path](../examples/README.md#learning-path-recommended-order)

## Building the Compiler

```bash
cd cc && make
```

The compiler binary is at `cc/bin/ccc`.

## Quick Example

```c
#include "cc_runtime.cch"
#include <stdio.h>

int main(void) {
    @nursery {
        spawn(() => printf("Hello from task A!\n"));
        spawn(() => printf("Hello from task B!\n"));
    }
    printf("Done.\n");
    return 0;
}
```

```bash
./cc/bin/ccc run examples/hello.ccs
```
