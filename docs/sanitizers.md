# Sanitizers and fuzzing

Periodic AddressSanitizer / ThreadSanitizer passes for the Concurrent-C
runtime, plus a recipe for ASan on the `concurrent-c-python` N-API addon.
PR CI runs the CC runtime lanes on Linux (`.github/workflows/ci.yml` job
`sanitizers`). Darwin ASan+fibers hangs — use Linux/Docker for ASan.

Linux TSan follows OS threads, not CC fibers, and will flag nursery
`free` / last-child `end_state` load at wait/teardown against a worker
that is already ordered by `alive_count`. Those reports are false
positives; extra waits/fences on that path are a real throughput hit.
The test scripts load `scripts/tsan_fiber.supp` instead. New races
outside those frames still fail the job.

Native `ccc` links `--ld-flags` only; pass `-fsanitize=…` on both
`--cc-flags` and `--ld-flags` (Linux clang will not pull libtsan/asan
from instrumented `.o` files). Docker TSan needs `seccomp=unconfined`
so `personality(ADDR_NO_RANDOMIZE)` works.

## How to run (CC runtime)

```bash
# Focused race smokes (quick)
./scripts/test_tsan.sh
./scripts/test_tsan.sh --all          # broader set

# Stress under sanitizers
./scripts/stress_sanitize.sh asan
./scripts/stress_sanitize.sh tsan
./scripts/stress_sanitize.sh sanitizers   # both

# real_projects mains (pigz_idiomatic, pigz_channel, pigz_cc, redis_idiomatic, levenshtein)
./scripts/real_projects_sanitize.sh asan
./scripts/real_projects_sanitize.sh tsan
./scripts/real_projects_sanitize.sh fuzz   # ASan binaries + random inputs
./scripts/real_projects_sanitize.sh all
# Darwin auto-uses Docker for runtime (ASan+fibers hang on host); Linux CI is native.
```

Requires `clang` for TSan on macOS. See also [`debugging.md`](debugging.md).

## How to run (package bridge addon)

The `.node` addon is loaded into a stock Node process. ASan must be
active **before** `dlopen`, so:

| Host | Notes |
|------|--------|
| **macOS** | SIP strips `DYLD_INSERT_LIBRARIES` for typical Node builds. Building `bin/cc_python_asan.node` works; loading it into Node fails with “Interceptors are not working.” Use Linux/Docker for runtime ASan. |
| **Linux** | Build with `-fsanitize=address -shared-libasan`, then `LD_PRELOAD` the clang ASan runtime before `node`. |

### Build (repo tree, any host with clang + `ccc`)

```bash
CC=clang ./cc/bin/ccc build -g npm/cc-python/src/cc_python.ccs \
  -o bin/cc_python_asan.node \
  --cc-flags "-fsanitize=address -fno-omit-frame-pointer -g" \
  --ld-flags "-fsanitize=address" \
  --no-cache
```

### Runtime (Linux / Docker) — helper

```bash
./scripts/sanitize_bridge.sh          # mem + neg under ASan in Docker
./scripts/sanitize_bridge.sh fuzz     # mem + seeded walk
./scripts/sanitize_bridge.sh chaos    # also CHAOS_SCALE=quick
./scripts/sanitize_bridge.sh tsan     # mem under TSan
./scripts/sanitize_bridge.sh tsan-fuzz
./scripts/sanitize_bridge.sh mem      # mem only

# Progress / hang detection (defaults shown):
HEARTBEAT_SECS=15 SANITIZE_BRIDGE_TIMEOUT=180 ./scripts/sanitize_bridge.sh
```

Progress (so a hang is obvious):

- Host: UTC start banner before `docker run` (image pull can be silent).
- Inside the container: `[+elapsed HH:MM:SS] >>> step` / `OK` for apt,
  compile, link.
- Apt install is quiet (logged under `/tmp/apt-*.log` in the container).
- Each Node suite streams lines as `    | …` (stdout) / `    ! …` (stderr).
- Every `HEARTBEAT_SECS` a `... alive suite=… last=…` line repeats; if
  that freezes, the suite hung.
- Suites past `SANITIZE_BRIDGE_TIMEOUT` are `KILL`ed (exit 124/137).
- SEGV is reported as `FAIL suite=… SEGV (exit 139)` rather than looking
  like a hang.

Manual sketch (aarch64 bookworm; adjust the ASan `.so` path for amd64):

```bash
docker run --rm -v "$PWD":/src:ro -w /src node:20-bookworm bash -lc '
  apt-get update -qq && apt-get install -y -qq clang python3 python3-dev libpython3-dev
  ASAN_SO=/usr/lib/llvm-14/lib/clang/14.0.6/lib/linux/libclang_rt.asan-aarch64.so
  FLAGS="-fsanitize=address -shared-libasan -fno-omit-frame-pointer -g -O1 -fPIC"
  clang $FLAGS -Inpm/cc-python/vendor/include -c npm/cc-python/vendor/cc_python.c -o /tmp/tu.o
  clang $FLAGS -DCC_ENABLE_ASYNC -Inpm/cc-python/vendor/include \
        -c npm/cc-python/vendor/runtime/concurrent_c.c -o /tmp/rt.o
  clang -fsanitize=address -shared-libasan -shared -o /tmp/cc_python_asan.node \
        /tmp/tu.o /tmp/rt.o -lpthread -lm -ldl
  export LD_PRELOAD=$ASAN_SO CC_PYTHON_ADDON=/tmp/cc_python_asan.node
  export ASAN_OPTIONS=detect_leaks=0 CC_LIBPYTHON=/usr/lib/*/libpython3.*.so
  node --expose-gc tests/cc_python_bridge_mem.js
'
```

Prefer a **fresh** `ccc build` of `cc_python.ccs` on Linux when available;
the vendored C in `npm/cc-python/vendor/` may lag `cc/shadow` HEAD.
`scripts/sanitize_bridge.sh` emits that vendor tree on the host when missing
(CI checkouts never ship it; the Docker mount is read-only).

TSan on the addon (Linux / Docker):

```bash
./scripts/sanitize_bridge.sh tsan       # TSan link + dlopen gate
./scripts/sanitize_bridge.sh tsan-fuzz  # same (+ mem/fuzz if NODE_TSAN_BIN set)
```

Stock Node **cannot** `LD_PRELOAD` `libtsan` (SEGV in `__cxa_atexit` —
the main executable must itself be TSan-instrumented). The default gate
builds the `.node` with `-fsanitize=thread` and dlopens it from a small
TSan host (`seccomp=unconfined` so TSan can `personality(ADDR_NO_RANDOMIZE)`).

Full mem/fuzz under TSan needs a ThreadSanitizer-built Node:

```bash
NODE_TSAN_BIN=/path/to/tsan-node ./scripts/sanitize_bridge.sh tsan-fuzz
```

Prefer ASan first for lease / teardown UAF; TSan is the cross-thread
companion once a TSan Node is available.

---

## Latest receipt — 2026-08-19

**Host:** macOS 26, arm64, Apple clang; Linux via Docker (`linux/arm64`, `seccomp=unconfined`)  
**Seed:** `shadow_lower` last-good **0.3.3-173**

### CC runtime — green

| Command | Result |
|---------|--------|
| `./scripts/test_tsan.sh` | **13/13 OK** (Darwin; Linux with `tsan_fiber.supp`) |
| `./scripts/stress_sanitize.sh asan` | **8/8 OK** (Linux; Darwin ASan+fibers hangs) |
| `./scripts/stress_sanitize.sh tsan` | **8/8 OK** (Linux, same suppressions) |

Tests covered: `tsan_closure_*`, fiber/nursery join races, work-stealing /
park / inbox storms; stress set `spawn_storm` … `deadline_race` (see
`scripts/stress_sanitize.sh` / `scripts/test_tsan.sh`). PR CI: job
`sanitizers` on `ubuntu-latest`. Native link needs `--ld-flags -fsanitize=…`
as well as `--cc-flags`.

### real_projects — green (Linux / Docker)

Darwin auto-routes through Docker (`ASan`/`TSan` + fibers hang on host).
Harness forces `CC=clang`. Levenshtein **import** under TSan is skipped
(dlopen into stock CPython lacks the TSan runtime); build still required.

| Command | Result |
|---------|--------|
| `./scripts/real_projects_sanitize.sh asan` | **OK** — pigz_idiomatic run, pigz_cc build, redis smoke, levenshtein import |
| `./scripts/real_projects_sanitize.sh tsan` | **OK** — same mains; levenshtein import **SKIP** (dlopen); server stderr scanned for late TSan reports |
| `./scripts/real_projects_sanitize.sh fuzz` | **OK** — light ASan pigz input fuzz |

Nightly: [`.github/workflows/bridge-asan-nightly.yml`](../.github/workflows/bridge-asan-nightly.yml) runs bridge ASan fuzz, TSan dlopen gate, wire libFuzzer, chaos, and real_projects asan/tsan/fuzz. Redis smoke under ASan is skipped (fiber fake-stack CHECK on GHA clang); TSan still runs that smoke.

### Bridge addon ASan

| Step | Result |
|------|--------|
| Darwin `ccc build` → `bin/cc_python_asan.node` | **OK** (link needs `--ld-flags -fsanitize=address`) |
| Darwin load into Node | **Blocked** — ASan interceptors too late / SIP strips `DYLD_INSERT_LIBRARIES` |
| Linux Docker build (vendor C + `-shared-libasan`) | **OK** (~2.7 MB `.node`) |
| Linux Docker `cc_python_bridge_mem.js` (pre-fix) | **SEGV (exit 139)** after `keep_past_return_lane` when `destroy()` was fire-and-forget then the next `create()` ran; **no** ASan ERROR/SUMMARY |
| Bisect (`scripts/asan_mem_bisect.js` / `sanitize_bridge.sh bisect`) | Prefixes through keep-past-return **OK** if `await destroy()`; **SEGV** if destroy is kicked and the next rung `create()`s immediately — not cumulative earlier rungs |
| Fix | Native: `create()` refuses while any async lane drain is in flight (articulate error; no sync wait — close settles via TSFN). Mem suite awaits post-lane destroy and asserts `create_during_destroy` |
| Linux Docker `cc_python_bridge_neg.js` | Incomplete on image CPython **3.11** (second in-process interpreter needs 3.12+); not an ASan finding |
| Linux Docker `js_python_chaos.js` (quick) | Kitchen sink awaits post-lane `destroy()`; `sanitize_bridge.sh chaos` = mem+fuzz+chaos (neg skipped on 3.11) |
| Wire-codec libFuzzer | `./scripts/fuzz_wire_codec.sh` — chunked arena (no realloc-under-pointers); prior crash seed repro OK |
| Nightly CI | [`.github/workflows/bridge-asan-nightly.yml`](../.github/workflows/bridge-asan-nightly.yml) — bridge fuzz + wire libFuzzer + chaos + real_projects asan/tsan/fuzz |

**Status:** mem SEGV fixed; real_projects ASan + TSan green under Docker; seeded fuzz + wire libFuzzer + nightly workflow in tree. Bookworm’s CPython 3.11 skips multi-interp mem rungs / omits neg under `chaos` mode.

---

## Fuzzing — should we?

**Yes, but as a second tier after ASan on the bridge is green.** The stress
suite is scenario-driven; fuzzing earns its keep on **compositional**
sequences the kitchen sink never rolls.

### High value (bridge / wire)

| Target | Approach | Why |
|--------|----------|-----|
| Isolated wire codec | Structure-aware fuzz of JSON ops + `$shm` / `$ta` / `$h` blobs (parent encode ↔ `broker.py` decode) | Protocol violations and spill ownership bugs |
| In-process napi surface | libFuzzer or cargo-fuzz-style harness calling `create` / `getattr` / `invoke` / `release` / `close` with random arg shapes | Lease + ledger races ASan should catch |
| Seeded chaos | Random walk over create/import/call/task/release/destroy/GC with **seed in RESULT** | Repro of the feedback “property-based layer”; hangs on the existing chaos drivers |

Deterministic replay (log seed on every fail) is mandatory; otherwise
fuzz findings are noise.

### Medium value (CC runtime)

| Target | Approach |
|--------|----------|
| Channel / nursery API | Existing stress + `libFuzzer` on sequences of send/recv/close/cancel |
| shadow_lower | Already has parser/corpus paths; expand only if lowering bugs outpace smokes |

### Low value / defer

- Fuzzing Node’s own ABI or CPython internals
- Blind AFL on `.ccs` files (grammar is large; smokes + shadow tests denser)
- Multi-GB / cgroup fuzz (machine-bound; keep soak-only)

### Suggested order

1. ~~Close bridge ASan SEGV~~ — done (create-during-destroy guard).
2. ~~Seeded random walk~~ — `stress/bridge/js_python_fuzz.js`.
3. ~~Wire-codec libFuzzer~~ — `./scripts/fuzz_wire_codec.sh` /
   `stress/bridge/fuzz/`.
4. ~~Nightly CI~~ — `bridge-asan-nightly.yml` (fuzz + wire + chaos).

---

## Related

- [`debugging.md`](debugging.md) — TSan day-to-day
- [`stress/bridge/bridge_stress.md`](../stress/bridge/bridge_stress.md) — bridge contracts
- [`scripts/stress_sanitize.sh`](../scripts/stress_sanitize.sh), [`scripts/test_tsan.sh`](../scripts/test_tsan.sh)
- [`scripts/sanitize_bridge.sh`](../scripts/sanitize_bridge.sh) — Docker ASan helper for the addon
- [`scripts/run_monitored.sh`](../scripts/run_monitored.sh) — host heartbeats + wall kill (used by fuzz)
- [`scripts/real_projects_sanitize.sh`](../scripts/real_projects_sanitize.sh) — real_projects ASan/TSan/fuzz
- [`stress/bridge/js_python_fuzz.js`](../stress/bridge/js_python_fuzz.js) — seeded walk + in-process progress
