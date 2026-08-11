# CC Compiler Source

**Product front** is native (`shadow_lower`). Sources: `cc/shadow/`.
Driver: `cc_main.c` (native-only; `--frontend=legacy` is a hard error).

Subdirectories:

- `parser/` — `symsig` (signature queries for comptime / lower_headers)
- `ast/` — CC-specific node metadata used by remaining sugar helpers
- `visitor/` — text sugar used by `lower_headers` / comptime (`pass_*_syntax`,
  unwrap/destroy, errhandler lookup) — not a product front
- `preprocess/` — comptime seam, emit plan, variant/type registry
- `comptime/` — comptime evaluator + monomorph instantiation cache
- `diag/` — diagnostics
- `util/` — shared helpers

`cc_main.c` dispatches `.ccs` / `.shcc` to `shadow_lower`.
