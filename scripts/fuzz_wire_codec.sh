#!/usr/bin/env bash
# Build + run libFuzzer on the isolated-bridge wire codec (no Node).
#
#   ./scripts/fuzz_wire_codec.sh              # 30s
#   FUZZ_SECONDS=120 ./scripts/fuzz_wire_codec.sh
#   ./scripts/fuzz_wire_codec.sh --smoke      # standalone smoke only
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
DIR="$ROOT/stress/bridge/fuzz"
OUT="${FUZZ_OUT:-$ROOT/out/fuzz/wire_codec}"
SECONDS_N="${FUZZ_SECONDS:-30}"
JOBS="${FUZZ_JOBS:-1}"

mkdir -p "$OUT/corpus" "$OUT"

if ! command -v clang >/dev/null 2>&1; then
  echo "fuzz_wire_codec: need clang" >&2
  exit 1
fi

echo "[fuzz_wire] smoke build (no libFuzzer)"
clang -std=c11 -O1 -g -Wall -Wextra -Werror \
  -DFUZZ_STANDALONE_MAIN \
  -I"$DIR" \
  "$DIR/wire_codec.c" "$DIR/wire_codec_fuzz.c" \
  -o "$OUT/wire_codec_smoke"
"$OUT/wire_codec_smoke"
echo "[fuzz_wire] smoke ok"

if [[ "${1:-}" == "--smoke" ]]; then
  exit 0
fi

echo "[fuzz_wire] libFuzzer build"
# Xcode clang often ships without libclang_rt.fuzzer_*.a — use Docker on
# Darwin, or any clang that links -fsanitize=fuzzer.
run_fuzzer() {
  local bin="$1"
  shift
  set +e
  "$bin" "$OUT/corpus" \
    -max_total_time="$SECONDS_N" \
    -workers="$JOBS" \
    -artifact_prefix="$OUT/crash-" \
    "$@" 2>&1 | tee "$OUT/last_run.log"
  local st=${PIPESTATUS[0]}
  set -e
  return "$st"
}

if [[ "$(uname -s)" == "Darwin" ]] && command -v docker >/dev/null 2>&1; then
  echo "[fuzz_wire] Darwin: libFuzzer via Docker (clang on host lacks fuzzer RT)"
  # Seed corpus on the host (avoids $ expansions inside the container script).
  python3 - <<PY
import struct, os
d = r'''$OUT/corpus'''
os.makedirs(d, exist_ok=True)
def w(name, b):
    open(os.path.join(d, name), 'wb').write(b)
w('null', bytes([0]))
w('nf', bytes([3, 0]))
raw = bytes(range(8))
w('ta_u8', bytes([5, 4]) + struct.pack('<I', 8) + raw)
w('shm_u8', bytes([6, 4]) + struct.pack('<I', 8) + raw)
w('list2', bytes([7]) + struct.pack('<H', 2) + bytes([0, 0]))
w('bad_key', bytes([8]) + struct.pack('<H', 1) + struct.pack('<H', 2) + b'\$x' + bytes([0]))
print('seeded', d)
PY
  docker run --rm --platform linux/arm64 \
    -v "$ROOT:/src:ro" \
    -v "$OUT:/out" \
    -w /src \
    debian:bookworm bash -c "
      set -euo pipefail
      apt-get update -qq
      DEBIAN_FRONTEND=noninteractive apt-get install -y -qq clang >/dev/null
      clang -std=c11 -O1 -g -Wall -Wextra \
        -fsanitize=fuzzer,address -fno-omit-frame-pointer \
        -Istress/bridge/fuzz \
        stress/bridge/fuzz/wire_codec.c stress/bridge/fuzz/wire_codec_fuzz.c \
        -o /out/wire_codec_fuzz
      set +e
      /out/wire_codec_fuzz /out/corpus \
        -max_total_time=${SECONDS_N} \
        -workers=${JOBS} \
        -artifact_prefix=/out/crash- \
        2>&1 | tee /out/last_run.log
      exit \${PIPESTATUS[0]}
    "
  st=$?
  if [ "$st" -ne 0 ]; then
    echo "[fuzz_wire] FAIL exit=$st (docker) — see $OUT/crash-* $OUT/last_run.log" >&2
    exit "$st"
  fi
  echo "[fuzz_wire] ok (${SECONDS_N}s, docker)"
  exit 0
fi

if ! clang -std=c11 -O1 -g -fsanitize=fuzzer -x c - -o /dev/null 2>/dev/null <<'EOF'
int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long n){(void)d;(void)n;return 0;}
EOF
then
  echo "fuzz_wire_codec: this clang cannot link -fsanitize=fuzzer" >&2
  echo "  (on macOS: install docker and re-run; CI uses ubuntu clang)" >&2
  exit 1
fi

clang -std=c11 -O1 -g -Wall -Wextra \
  -fsanitize=fuzzer,address -fno-omit-frame-pointer \
  -I"$DIR" \
  "$DIR/wire_codec.c" "$DIR/wire_codec_fuzz.c" \
  -o "$OUT/wire_codec_fuzz"

# Seed corpus with a few valid blobs from the smoke encoder path.
python3 - <<'PY' "$OUT/corpus"
import struct, sys, os
d = sys.argv[1]
os.makedirs(d, exist_ok=True)
def w(name, b):
    open(os.path.join(d, name), 'wb').write(b)
# null
w('null', bytes([0]))
# $nf nan
w('nf', bytes([3, 0]))
# $ta u8 len=8
raw = bytes(range(8))
w('ta_u8', bytes([5, 4]) + struct.pack('<I', 8) + raw)
# $shm u8
w('shm_u8', bytes([6, 4]) + struct.pack('<I', 8) + raw)
# list of two nulls
w('list2', bytes([7]) + struct.pack('<H', 2) + bytes([0, 0]))
# reserved key (should reject)
w('bad_key', bytes([8]) + struct.pack('<H', 1) + struct.pack('<H', 2) + b'$x' + bytes([0]))
print('seeded', d)
PY

echo "[fuzz_wire] run ${SECONDS_N}s jobs=${JOBS}"
set +e
"$OUT/wire_codec_fuzz" "$OUT/corpus" \
  -max_total_time="$SECONDS_N" \
  -workers="$JOBS" \
  -artifact_prefix="$OUT/crash-" \
  2>&1 | tee "$OUT/last_run.log"
st=${PIPESTATUS[0]}
set -e
# libFuzzer returns 0 on clean timeout; non-zero on crash find
if [ "$st" -ne 0 ]; then
  echo "[fuzz_wire] FAIL exit=$st — see $OUT/crash-* and $OUT/last_run.log" >&2
  exit "$st"
fi
echo "[fuzz_wire] ok (${SECONDS_N}s)"
exit 0
