# CC Compiler Source

Subdirectories:
- `lexer/` — CC token/lex overlay that hooks into TCC.
- `parser/` — CC grammar overlay.
- `ast/` — CC-specific node metadata (side-tables).
- `visitor/` — main single-pass visitor plus const-collection pre-pass.
- `comptime/` — comptime evaluator + monomorph instantiation cache.
- `codegen/` — C emission for sync/async, UFCS desugaring, channels, slices.

