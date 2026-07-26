#!/usr/bin/env bash
# measure_size_rss_ilp32.sh — binary size + peak RSS for pigz vs pigz_cc (Linux).
# Expects out/pigz and out/pigz_cc already built (e.g. after compare_ilp32.sh).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

SIZE_MB="${PIGZ_BENCH_MB:-20}"
WORKERS="${PIGZ_BENCH_WORKERS:-4}"
PIGZ="$SCRIPT_DIR/out/pigz"
PIGZ_CC="$SCRIPT_DIR/out/pigz_cc"
test -x "$PIGZ" && test -x "$PIGZ_CC"

echo "== binary sizes"
ls -lh "$PIGZ" "$PIGZ_CC"
echo "--- size(1) ---"
size "$PIGZ" "$PIGZ_CC" || true
echo "--- stripped ---"
cp "$PIGZ" /tmp/pigz.sym
cp "$PIGZ_CC" /tmp/pigz_cc.sym
strip /tmp/pigz.sym /tmp/pigz_cc.sym
ls -lh /tmp/pigz.sym /tmp/pigz_cc.sym
size /tmp/pigz.sym /tmp/pigz_cc.sym || true
file "$PIGZ" "$PIGZ_CC"

# Prefer existing testdata from benchmark.sh; else make a small compressible blob.
DATA_DIR="$SCRIPT_DIR/testdata"
INPUT="$DATA_DIR/text_${SIZE_MB}mb.bin"
if [ ! -f "$INPUT" ]; then
  mkdir -p "$DATA_DIR"
  # shellcheck source=download_silesia.sh
  source "$SCRIPT_DIR/download_silesia.sh"
  download_silesia "$DATA_DIR"
  target=$((SIZE_MB * 1000000))
  : > "$INPUT"
  files=$(find "$DATA_DIR/silesia" -type f -print | LC_ALL=C sort)
  while [ "$(wc -c < "$INPUT")" -lt "$target" ]; do
    # shellcheck disable=SC2086
    cat $files >> "$INPUT"
  done
  head -c "$target" "$INPUT" > "${INPUT}.tmp" && mv "${INPUT}.tmp" "$INPUT"
fi
echo "== input: $INPUT ($(wc -c < "$INPUT") bytes), -p $WORKERS"

peak_rss() {
  local label=$1
  local bin=$2
  shift 2
  # Prefer GNU time when installed; otherwise sample VmHWM from /proc (Debian
  # slim images often lack /usr/bin/time).
  if [ -x /usr/bin/time ]; then
    local rss_file="$TMP/rss.$$.$RANDOM"
    if ! /usr/bin/time -f '%M' -o "$rss_file" "$bin" "$@" >/dev/null 2>/dev/null; then
      echo "  $label: (command failed)"
      rm -f "$rss_file"
      return
    fi
    local kb
    kb=$(tr -cd '0-9' < "$rss_file")
    rm -f "$rss_file"
  else
    kb=$(python3 - "$bin" "$@" <<'PY'
import os, sys, time, subprocess
cmd = sys.argv[1:]
p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
peak = 0
status = f"/proc/{p.pid}/status"
while p.poll() is None:
    try:
        with open(status) as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    peak = max(peak, int(line.split()[1]))
                    break
                if line.startswith("VmRSS:") and peak == 0:
                    peak = max(peak, int(line.split()[1]))
    except FileNotFoundError:
        break
    time.sleep(0.005)
# final read
try:
    with open(status) as f:
        for line in f:
            if line.startswith("VmHWM:"):
                peak = max(peak, int(line.split()[1]))
except Exception:
    pass
rc = p.wait()
if rc != 0:
    sys.exit(rc)
print(peak)
PY
) || { echo "  $label: (command failed)"; return; }
  fi
  if [ -z "${kb:-}" ]; then
    echo "  $label: (failed to parse RSS)"
    return
  fi
  python3 -c "print(f'  $label: peak RSS = {int(\"$kb\")} KiB ({int(\"$kb\")/1024:.1f} MiB)')"
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "== compress peak RSS (3 runs)"
for bin in "$PIGZ" "$PIGZ_CC"; do
  echo "-- $(basename "$bin")"
  for i in 1 2 3; do
    cp "$INPUT" "$TMP/work.bin"
    peak_rss "run $i" "$bin" -k -p "$WORKERS" "$TMP/work.bin"
    rm -f "$TMP/work.bin.gz"
  done
done

echo "== decompress peak RSS (3 runs)"
"$PIGZ" -k -c -p "$WORKERS" "$INPUT" > "$TMP/in.gz"
for bin in "$PIGZ" "$PIGZ_CC"; do
  echo "-- $(basename "$bin")"
  for i in 1 2 3; do
    cp "$TMP/in.gz" "$TMP/work.gz"
    peak_rss "run $i" "$bin" -d -k -p "$WORKERS" "$TMP/work.gz"
    rm -f "$TMP/work"
  done
done

echo "Done (size/RSS)."
