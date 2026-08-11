# Concurrent-C Compiler Overlay

**`ccc` is native-only** — it drives `shadow_lower` (sources under
`cc/shadow/`, bootstrap under `bootstrap/shadow_lower/`).
Architecture: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Ops/layout:
[shadow/README.md](shadow/README.md).

`--frontend=legacy` / `CC_FRONTEND=legacy` are hard errors (the multipass
visitor front has been removed).

## Structure

- `include/` — public headers for CC runtime/ABI
- `runtime/` — minimal runtime (scheduler, channels, arenas)
- `src/` — driver (`ccc`), comptime / `lower_headers` engine, shared sugar passes
- `shadow/` — native front sources (`shadow_lower.ccs`, `pp_*.cch`)
- `bootstrap/shadow_lower/` — committed lowered-C snapshots for `shadow_lower`
