# CC Compiler Source

**Default product front** is native (`shadow_lower`), not the visitor tree
here. Sources: `cc/shadow/`. Driver dispatch: `cc_main.c`
(`--frontend=native|legacy`).

Subdirectories used by the **legacy** multipass front (`--frontend=legacy`):

- `lexer/` — CC token/lex overlay that hooks into TCC
- `parser/` — CC grammar overlay
- `ast/` — CC-specific node metadata (side-tables)
- `visitor/` — text-rewrite + stub-AST visitor passes (legacy only)
- `preprocess/` — P-passes / L2 rewriter shared with some native comptime seams
- `comptime/` — comptime evaluator + monomorph instantiation cache
- `codegen/` — C emission helpers used by the legacy path
- `diag/` — diagnostics; many `CC_DEBUG_*` vars apply to the legacy reparse path

Shared with both fronts: `cc_main.c` (driver), `util/`, parts of `comptime/`
and `preprocess/` (e.g. `libshadow_comptime.a` for `@comptime` on native).
