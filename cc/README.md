# Concurrent-C Compiler Overlay

Holds CC-specific tooling layered on top of upstream TCC.

Structure:
- `include/` — public headers for CC runtime/ABI.
- `runtime/` — minimal runtime (scheduler, channels, arenas).
- `src/` — compiler front-end extensions, visitor, comptime, and codegen.

