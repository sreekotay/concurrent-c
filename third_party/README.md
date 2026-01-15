# Third-Party Dependencies

All dependencies are Git submodules for easy version tracking and updates.

## Dependencies

| Directory | Library | License | Purpose |
|-----------|---------|---------|---------|
| `tcc/` | TinyCC | LGPL 2.1 | C parser/compiler foundation |
| `bearssl/` | BearSSL | MIT | TLS 1.2/1.3 implementation |

## TCC

Upstream TinyCC with minimal CC integration hooks.

- **Patches**: `tcc-patches/` contains small hooks for CC integration (lexer/parser hooks, const-eval API)
- **Apply patches**: `make tcc-patch-apply`
- **Regenerate patches**: `make tcc-patch-regen`

## BearSSL

Lightweight TLS library ideal for CC's arena-based memory model:

- **No dynamic allocation** — all buffers provided by caller
- **Small footprint** — ~30KB code for full TLS 1.2
- **MIT license** — compatible with CC's dual MIT/Apache licensing
- **Build**: `make bearssl` (outputs `third_party/bearssl/build/libbearssl.a`)

### Why BearSSL over alternatives?

| Library | License | Allocation | Size | Notes |
|---------|---------|------------|------|-------|
| **BearSSL** | MIT | Caller-provided | ~30KB | Perfect for arenas |
| mbedTLS | Apache 2.0 | malloc (hookable) | ~100KB | More features, larger |
| OpenSSL | Apache 2.0 | malloc | ~1MB | Too large, complex API |
| wolfSSL | GPL/Commercial | malloc | ~100KB | License problematic |

## Updating Dependencies

```bash
# Update all submodules to latest upstream
make deps-update

# Or update specific submodule
cd third_party/bearssl && git pull origin master
cd ../.. && git add third_party/bearssl && git commit -m "Update BearSSL"
```

## Building

```bash
# Build all dependencies
make deps

# Build specific dependency
make bearssl

# Clean
make bearssl-clean
```

