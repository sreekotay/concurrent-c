# Third-Party Dependencies

All dependencies are Git submodules for easy version tracking and updates.

## Dependencies

| Directory | Library | License | Purpose | Build |
|-----------|---------|---------|---------|-------|
| `tcc/` | TinyCC | LGPL 2.1 | C parser/compiler foundation | (auto) |
| `bearssl/` | BearSSL | MIT | TLS support (`<std/tls.cch>`) | `make bearssl` |
| `curl/` | libcurl | MIT | HTTP client (`<std/http.cch>`) | `make curl` |

**Note**: BearSSL and libcurl are opt-in. Only build/link what you need.

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

## libcurl

Battle-tested HTTP client library.

- **License**: MIT-style (curl license)
- **Build**: `make curl` (outputs `third_party/curl/build/lib/libcurl.a`)
- **TLS**: We configure curl to use BearSSL (so you need `make bearssl` first)

### Minimal Build

We build curl with only HTTP/HTTPS support (disabled: FTP, LDAP, SMTP, etc.)
This keeps the library size reasonable (~400KB).

### Usage in build.cc

```c
// Option 1: Use system libcurl (recommended)
CC_TARGET_LIBS myapp curl
CC_TARGET_DEFINE myapp CC_ENABLE_HTTP=1

// Option 2: Use vendored libcurl (requires: make curl-build)
CC_TARGET_LIBS myapp third_party/curl/build/lib/libcurl.a
CC_TARGET_LIBS myapp third_party/bearssl/build/libbearssl.a
CC_TARGET_INCLUDE myapp third_party/curl/include
CC_TARGET_DEFINE myapp CC_ENABLE_HTTP=1
```

### FUTURE: Zero-Copy Investigation

Currently, libcurl copies received data from its internal buffer to our arena.
This is a potential optimization target:

- Investigate CURLOPT_WRITEFUNCTION receiving user buffer pointer
- May require patching curl's recv path
- Goal: recv() directly into arena, eliminating one memory copy

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

