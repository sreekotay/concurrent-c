# Track B — upstream txiki.js reference

Pinned: `saghul/txiki.js` @ `75b8cdf3f54380c239eed7f613e75dfe01b79334`
(see [`../../PINNED.md`](../../PINNED.md)).

## Build

```sh
git clone https://github.com/saghul/txiki.js.git /tmp/txiki.js
cd /tmp/txiki.js
git checkout 75b8cdf3f54380c239eed7f613e75dfe01b79334
git submodule update --init --depth 1 deps/libuv deps/quickjs
# Full cmake build (pulls more deps; follow upstream README)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Run workload

```sh
export TXIKI_BIN=/tmp/txiki.js/build/tjs
$TXIKI_BIN run ../workload/timers.js
```

`scripts/run_track_b.sh` picks up `TXIKI_BIN` or `/tmp/txiki.js/build/tjs` when present.

The CC/Rust hosts intentionally mirror `src/timers.c` (uv_timer + JSValue claim),
not the full txiki runtime.
