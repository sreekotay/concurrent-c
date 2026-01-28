#!/bin/zsh
# Benchmark: pigz (pthread) vs pigz_cc (Concurrent-C)
set -e

RUNS=5

echo "========================================"
echo "pigz vs pigz_cc Benchmark (${RUNS} runs)"
echo "========================================"
echo ""

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
    { time ./pigz -k -f -p 4 /tmp/r.bin } 2>&1 | grep total
    rm -f /tmp/r.bin.gz
done
echo ""
echo "--- pigz_cc ---"
for i in {1..$RUNS}; do
    cp /tmp/random10.bin /tmp/r.bin
    { time ./pigz_cc_release -k -p 4 /tmp/r.bin } 2>&1 | grep total
    rm -f /tmp/r.bin.gz
done
echo ""

echo "========================================"
echo "TEXT DATA (10MB)"
echo "========================================"
echo "--- pigz ---"
for i in {1..$RUNS}; do
    cp /tmp/text10.txt /tmp/t.txt
    { time ./pigz -k -f -p 4 /tmp/t.txt } 2>&1 | grep total
    rm -f /tmp/t.txt.gz
done
echo ""
echo "--- pigz_cc ---"
for i in {1..$RUNS}; do
    cp /tmp/text10.txt /tmp/t.txt
    { time ./pigz_cc_release -k -p 4 /tmp/t.txt } 2>&1 | grep total
    rm -f /tmp/t.txt.gz
done
echo ""

echo "========================================"
echo "VERIFICATION"
echo "========================================"
cp /tmp/text10.txt /tmp/v.txt
./pigz_cc_release -k -p 4 /tmp/v.txt 2>/dev/null
orig=$(md5 -q /tmp/text10.txt)
decomp=$(gunzip -c /tmp/v.txt.gz | md5 -q)
rm -f /tmp/v.txt /tmp/v.txt.gz
if [ "$orig" = "$decomp" ]; then
    echo "✓ MD5 match - correctness verified"
else
    echo "✗ MD5 mismatch!"
fi

rm -f /tmp/random10.bin /tmp/text10.txt /tmp/r.bin /tmp/t.txt
echo ""
echo "Done."
