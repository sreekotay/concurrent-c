# Version pins

Write these before the first comparative run. Do not retune per
implementation.

| Component | Pin | Notes |
|-----------|-----|-------|
| quickjs-ng | `df836d1f4490dfc6a65dbceda8a71d14ddc7f45c` (HEAD at study start) | Clone: `git clone https://github.com/quickjs-ng/quickjs.git` then `git checkout` that SHA. Point `CC_QUICKJS_SRC` at the tree. |
| rquickjs | `0.12.2` | crates.io; ships quickjs-ng bindings — for a strict same-SHA compare, prefer building against `CC_QUICKJS_SRC` / sys crate when available; otherwise document the bundled engine SHA in the receipt |
| Concurrent-C | record `ccc` version string in each receipt | e.g. `ccc 0.3.4-320` |
| Host C compiler | system `cc` / `clang` | Same flags for CC adapter compile and Rust C deps |
| txiki.js (Track B) | `75b8cdf3f54380c239eed7f613e75dfe01b79334` | https://github.com/saghul/txiki.js (master at study start; `txiki-org/` 404s) |
| libuv (Track B) | `aabb7651de73ec2f1a74361ca3430eed1a62e402` | txiki `deps/libuv` @ that commit (`v1.x` branch tip when pinned) |

## Compiler flags (shared)

```text
-O2 -std=gnu11   # QuickJS adapter / C pieces
cargo --release  # Rust driver
```

No per-impl special `-D` GC knobs, no different QuickJS configure.

## Attaching the engine for CC

```sh
git clone https://github.com/quickjs-ng/quickjs.git /tmp/quickjs-ng
cd /tmp/quickjs-ng && git checkout df836d1f4490dfc6a65dbceda8a71d14ddc7f45c
export CC_QUICKJS_SRC=/tmp/quickjs-ng
```

The adapter cache is per engine identity
(`~/.cache/concurrent-c/qjs/<git-HEAD-or-path-hash>/`). Switching pins
builds a new locale; no wipe required. Optional cleanup of old locales:

```sh
# optional — reclaim disk only
rm -rf "${CC_QJS_CACHE:-$HOME/.cache/concurrent-c/qjs}"
```
