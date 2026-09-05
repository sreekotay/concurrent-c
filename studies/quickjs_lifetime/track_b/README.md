# Track B — natural timer pressure

**Status:** CC + Rust hosts implemented against pinned libuv; txiki reference optional.

## Goal

Port a txiki.js-shaped timer slice (setTimeout / clearTimeout + shutdown)
onto Concurrent-C and Rust, then compare against an upstream txiki C
reference build. Lifetime facts come from the work itself: libuv handle
lifetime ≠ logical timer ≠ JS callback claim.

## Why not quickjs-libc timers

QuickJS-ng’s `quickjs-libc` timers are not txiki’s libuv integration.
Track B requires new libuv glue — not [`js.cch`](../../../cc/include/ccc/script/js.cch)
(Node host loop).

## Pins

See [`../PINNED.md`](../PINNED.md):

| | SHA |
|--|-----|
| txiki.js (`saghul/txiki.js`) | `75b8cdf3f54380c239eed7f613e75dfe01b79334` |
| libuv (txiki submodule) | `aabb7651de73ec2f1a74361ca3430eed1a62e402` |

Upstream timer kernel mirrored: `src/timers.c` (`uv_timer_t` + `JSValue` Dup + close-after-fire).

## Run

```sh
export CC_QUICKJS_SRC=/path/to/pinned/quickjs-ng
./studies/quickjs_lifetime/scripts/run_track_b.sh
```

Builds pinned libuv into `deps/` (gitignored), runs CC then Rust on
[`workload/timers.js`](workload/timers.js). Set `TXIKI_BIN` for an upstream
reference (see [`c/README.md`](c/README.md)).

## Layout

| Path | Role |
|------|------|
| `cc/timers_host.ccs` | libuv timer host + JS globals |
| `cc/driver.ccs` | load workload, run loop, oracle |
| `rust/` | rquickjs twin + `uv_ffi.c` |
| `c/` | txiki reference build notes |
| `workload/timers.js` | shared natural workload |
| `results/` | local receipts (gitignored) |

## Lifetime shape (containment check)

```text
setTimeout(fn)
  → DupValue(fn)          # JS claim
  → uv_timer_init/start   # handle lifetime
clearTimeout / fire / shutdown
  → FreeValue(fn)         # drop claim
  → uv_close              # end handle (async free in close cb)
```

Stdlib `quickjs.cch` gained only `cc_qjs_execute_pending_job` (Promise drain).
