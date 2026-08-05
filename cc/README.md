# Concurrent-C Compiler Overlay

**Default front is native** — `ccc` drives `shadow_lower` (sources under
`cc/shadow/`, bootstrap under `bootstrap/shadow_lower/`).
Architecture: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Ops/layout:
[shadow/README.md](shadow/README.md).

`--frontend=legacy` / `CC_FRONTEND=legacy` selects the older multipass
text-rewrite path. See [docs/LEGACY_ARCHITECTURE.md](docs/LEGACY_ARCHITECTURE.md)
and `src/visitor/`.

## Structure

- `include/` — public headers for CC runtime/ABI
- `runtime/` — minimal runtime (scheduler, channels, arenas)
- `src/` — driver (`ccc`), legacy front (visitor / preprocess), comptime helpers
- `shadow/` — native front sources (`shadow_lower.ccs`, `pp_*.cch`)
- `bootstrap/shadow_lower/` — committed lowered-C snapshots for `shadow_lower`

## Legacy front: CC stub-AST node cheatsheet (patched TCC)

These nodes are emitted by `third_party/tcc` under `CONFIG_CC_EXT` and consumed
by the **legacy** visitor passes (`--frontend=legacy`):

- `CC_AST_NODE_FUNC`
  - `aux_s1`: function name
  - `aux_s2`: return type string (as parsed)
  - `aux1`: function attrs (bit0 = @async, bit1 = @noblock, bit2 = @latency_sensitive)
  - children: `CC_AST_NODE_PARAM` (one per parameter)
  - spans: `line_start/col_start/col_end` at the function identifier

- `CC_AST_NODE_PARAM`
  - `aux1`: token id (identifier)
  - `aux_s1`: parameter name
  - `aux_s2`: parameter type string
  - spans: `line_start/col_start/col_end` best-effort at the parameter identifier

- `CC_AST_NODE_CALL`
  - `aux1`: callee token (if available)
  - `aux2`: flags (bit0=receiver_is_ptr, bit1=is_ufcs, bits8+ = occurrence on line)
  - `aux_s1`: callee name string

Other nodes (DECL_ITEM, BLOCK, STMT, etc.) keep the same layout: `file`,
`line/col` spans, `aux_s*` payloads per node kind.

Source-map hygiene: spans are filled from token positions where available;
passes that splice source should prefer these spans over text searches. When
extending TCC, keep CC-ext blocks small and clearly marked to ease rebasing
against upstream TCC.
