#!/usr/bin/env python3
# Generate a number-heavy corpus: a JSON array of N floats (deterministic, no ws).
# Usage: python3 gen_numbers.py [N] > numbers_big.json
import sys
n = int(sys.argv[1]) if len(sys.argv) > 1 else 300000
out = ['[']
out.append(','.join('%.6f' % ((i * 2654435761 % 1000000) / 1000.0) for i in range(n)))
out.append(']')
sys.stdout.write(''.join(out))
