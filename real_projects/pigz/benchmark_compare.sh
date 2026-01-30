#!/bin/zsh
# Benchmark: pigz (pthread) vs pigz_cc (Concurrent-C)
set -e

SCRIPT_DIR="$(dirname "$0")"
OUT_DIR="$SCRIPT_DIR/out"
RUNS=5

echo "========================================"
echo "pigz vs pigz_cc Benchmark (${RUNS} runs)"
echo "========================================"
echo ""

# Check binaries exist
if [ ! -f "$OUT_DIR/pigz" ]; then
    echo "Error: $OUT_DIR/pigz not found. Run 'make pigz' first."
    exit 1
fi
if [ ! -f "$OUT_DIR/pigz_cc" ]; then
    echo "Error: $OUT_DIR/pigz_cc not found. Run 'make pigz_cc' first."
    exit 1
fi

# Create test files
echo "Creating test data..."
dd if=/dev/urandom bs=1M count=10 of=/tmp/random10.bin 2>/dev/null
cat /usr/share/dict/words /usr/share/dict/words /usr/share/dict/words /usr/share/dict/words > /tmp/text10.txt
echo "Random: $(stat -f%z /tmp/random10.bin) bytes"
echo "Text:   $(stat -f%z /tmp/text10.txt) bytes"
echo ""

echo "========================================"
echo "RANDOM DATA (10MB)"
echo "========================================"
echo "--- pigz ---"
for i in {1..$RUNS}; do
    cp /tmp/random10.bin /tmp/r.bin
    { time $OUT_DIR/pigz -k -f -p 4 /tmp/r.bin } 2>&1 | grep total
    rm -f /tmp/r.bin.gz
done
echo ""
echo "--- pigz_cc ---"
for i in {1..$RUNS}; do
    cp /tmp/random10.bin /tmp/r.bin
    { time $OUT_DIR/pigz_cc -k -p 4 /tmp/r.bin } 2>&1 | grep total
    rm -f /tmp/r.bin.gz
done
echo ""

echo "========================================"
echo "TEXT DATA (10MB)"
echo "========================================"
echo "--- pigz ---"
for i in {1..$RUNS}; do
    cp /tmp/text10.txt /tmp/t.txt
    { time $OUT_DIR/pigz -k -f -p 4 /tmp/t.txt } 2>&1 | grep total
    rm -f /tmp/t.txt.gz
done
echo ""
echo "--- pigz_cc ---"
for i in {1..$RUNS}; do
    cp /tmp/text10.txt /tmp/t.txt
    { time $OUT_DIR/pigz_cc -k -p 4 /tmp/t.txt } 2>&1 | grep total
    rm -f /tmp/t.txt.gz
done
echo ""

echo "========================================"
echo "VERIFICATION"
echo "========================================"
cp /tmp/text10.txt /tmp/v.txt
$OUT_DIR/pigz_cc -k -p 4 /tmp/v.txt 2>/dev/null
orig=$(md5 -q /tmp/text10.txt)
decomp=$(gunzip -c /tmp/v.txt.gz | md5 -q)
rm -f /tmp/v.txt /tmp/v.txt.gz
if [ "$orig" = "$decomp" ]; then
    echo "MD5 match - correctness verified"
else
    echo "MD5 mismatch!"
fi

rm -f /tmp/random10.bin /tmp/text10.txt /tmp/r.bin /tmp/t.txt
echo ""
echo "Done."
