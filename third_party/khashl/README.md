# khashl

Single-header macro-based generic hash table library by Attractive Chaos.

**Source:** https://github.com/attractivechaos/khashl  
**Version:** r40  
**License:** MIT

## Why khashl?

- **1-bit per bucket** flag storage (vs 1 byte in naive implementations)
- **No tombstones** - uses backshift deletion for better performance over time
- **Fibonacci hashing** - better distribution than modulo
- **Linear probing** - cache-friendly
- **Hash salt support** - DoS protection
- **Custom allocator hooks** - `Kmalloc`, `Kcalloc`, `Krealloc`, `Kfree`

## Usage in Concurrent-C

We wrap khashl with arena-backed allocation in `cc/include/std/map.cch`.
The K* macros are defined before including khashl.h to route allocations
through `CCArena`.
