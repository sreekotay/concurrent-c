#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(dirname "$0")"
OUT_DIR="$SCRIPT_DIR/out"

if [ ! -f "$OUT_DIR/pigz_cc" ]; then
  echo "Error: $OUT_DIR/pigz_cc not found. Run 'make pigz_cc' first."
  exit 1
fi

cd "$OUT_DIR"

INPUT="smoke_input.bin"
CC_GZ="smoke_cc.gz"
CC_OUT="smoke_cc.out"

dd if=/dev/urandom of="$INPUT" bs=1024 count=64 2>/dev/null

./pigz_cc -k -c "$INPUT" > "$CC_GZ"
gunzip -c "$CC_GZ" > "$CC_OUT"

if cmp -s "$INPUT" "$CC_OUT"; then
  echo "pigz_cc smoke: PASS"
else
  echo "pigz_cc smoke: FAIL"
  exit 1
fi

rm -f "$INPUT" "$CC_GZ" "$CC_OUT"
