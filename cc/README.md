# Concurrent-C Compiler Overlay

`ccc` drives the lowerer (sources under `cc/lower/`, seeds under
`bootstrap/lowerer/`) and compiles and links its C itself.
Architecture: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). The lowerer:
[lower/LOWERING.md](lower/LOWERING.md).

`--frontend=legacy` / `CC_FRONTEND=legacy` are hard errors (the multipass
visitor front has been removed).

## Structure

- `include/` — public headers for CC runtime/ABI
- `runtime/` — minimal runtime (scheduler, channels, arenas)
- `src/` — driver (`ccc`), comptime / `lower_headers` engine, shared sugar passes
- `lower/` — the lowerer (`cclower.ccs` and its faces; `cclex`, `ccparse`, `ccindex` beside it)
- `bootstrap/lowerer/` — committed seeds: the lowered C stage zero builds the tools from
