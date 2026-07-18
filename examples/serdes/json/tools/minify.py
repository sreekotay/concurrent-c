#!/usr/bin/env python3
# Strip structural whitespace ONLY, preserving string bytes exactly (no re-encoding).
# Unlike `json.dump`, this does not re-escape non-ASCII, so borrow/copy rates and
# checksums are unchanged — only the inter-token whitespace is removed.
# Usage: python3 minify.py < twitter.json > twitter_min.json
import sys
b = sys.stdin.buffer.read()
out = bytearray(); instr = False; esc = False
for ch in b:
    if instr:
        out.append(ch)
        if esc: esc = False
        elif ch == 0x5c: esc = True
        elif ch == 0x22: instr = False
    else:
        if ch in (0x20, 0x09, 0x0a, 0x0d):
            continue
        out.append(ch)
        if ch == 0x22: instr = True
sys.stdout.buffer.write(out)
