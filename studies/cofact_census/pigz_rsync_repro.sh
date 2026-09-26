#!/bin/sh
# pigz_cc --rsyncable drops input bytes and exits 0 (seed 0.4.0-419).
# The rolling hash can hit every 5 bytes; the segment array holds block/5
# entries, so a block whose hits fill it has no slot left for its tail
# segment, and compress_block deflates only the listed segments.
set -e
B=${1:-real_projects/pigz/out/pigz_cc}
T=$(mktemp -d)
python3 -c "import sys; sys.stdout.buffer.write(bytes([0x80,0,0,0,0x1f])*((3*131072)//5))" > $T/in.bin
"$B" -R -c $T/in.bin > $T/out.gz; echo "pigz_cc -R exit=$?"
gzip -t $T/out.gz || true
gzip -dc $T/out.gz > $T/back.bin 2>/dev/null || true
echo "in=$(stat -c%s $T/in.bin) back=$(stat -c%s $T/back.bin)"
cmp -s $T/in.bin $T/back.bin && echo identical || echo DIFFERENT
