# Third-Party Dependencies

All dependencies are Git submodules for easy version tracking and updates.

## Dependencies

| Directory | Library | License | Purpose | Required for | Build |
|-----------|---------|---------|---------|--------------|-------|
| `tcc/` | TinyCC | LGPL 2.1 | C parser/compiler foundation | compiler build | (auto) |
| `liblfds/` | liblfds 7.1.1 | Unlicense | Optional lock-free channel queue backend | — | (auto) |
| `bearssl/` | BearSSL | MIT | TLS support (`<std/tls.cch>`) | `<std/tls.cch>` | `make bearssl` |
| `curl/` | libcurl | MIT | HTTP client (`<std/http.cch>`) | `<std/http.cch>` | `make curl` |

Float formatting ([Żmij](https://github.com/vitaut/zmij)) is vendored in-tree at
`cc/runtime/vendor/` — not a submodule.

Fetch the build inputs with:

```bash
./scripts/fetch_submodules.sh              # tcc
./scripts/fetch_submodules.sh --full       # complete trees, for submodule work
./scripts/fetch_submodules.sh --with-liblfds
```

**Note**: BearSSL and libcurl are opt-in. Only build/link what you need.

## liblfds

An optional bounded-MPMC-queue backend for channels. `cc/runtime/channel.c`
probes for it with `__has_include` and defines `CC_HAVE_LIBLFDS` accordingly; the
native ring queue is the primary lock-free path either way, so builds without it
are fully supported (and are the default). It also has no TCC port, so a TCC host
build always sets `CC_NO_LIBLFDS`.

Its only host is `liblfds.org`, which has no mirror. Because a fetch failure
there is unrelated to whether the compiler can be built, it is opt-in rather
than a hard prerequisite.

## TCC

Upstream TinyCC with minimal CC integration hooks.

- **Patches**: `tcc-patches/` contains small hooks for CC integration (lexer/parser hooks, const-eval API)
- **Apply patches**: `make tcc-patch-apply`
- **Regenerate patches**: `make tcc-patch-regen`

### Submodule reachability guard

Before pushing a superproject commit that changes any submodule pointer, run:

```bash
make check-submodules
# or
npm run check:submodules
```

This verifies every gitlink recorded by the superproject is fetchable from the
submodule URL in `.gitmodules`. It catches the common failure mode where a local
submodule commit exists on your machine, gets recorded in `origin/main`, but was
never pushed to the submodule remote. Fresh clones then fail during
`git submodule update --init`.

For `third_party/tcc`, `.gitmodules` tracks `upstream-mob` on
`https://github.com/sreekotay/tinycc.git` — a pristine TinyCC mirror. CC hooks
live in `tcc-patches/` and are applied into the working tree
(`ignore = dirty`). The gitlink must be a commit already on that remote (usually
`origin/upstream-mob`); if check-submodules fails, fix the pin rather than
pushing a private fork tip:

```bash
git -C third_party/tcc fetch origin upstream-mob
git -C third_party/tcc checkout -f origin/upstream-mob
# from the superproject:
git add third_party/tcc
make check-submodules
```

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

