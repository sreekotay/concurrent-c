#!/usr/bin/env bash
# Gate: .cch Result types/ctors use T !>(E) + cc_ok/cc_err, not hand-written
# CCResult_* / cc_ok_CCResult_* outside SPEC registrations and cc_result.cch.
# @emit(`...`) payloads are host C and may keep mangled ABI names.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

python3 - <<'PY'
from pathlib import Path
import re
import sys

ROOT = Path('.')
SPEC_OR_GUARD = re.compile(
    r'^\s*(CC_DECL_RESULT_SPEC(?:_VOID)?\(|#ifndef CCResult_|#define CCResult_)'
)
HIT = re.compile(r'\b(?:CCResult_\w+|cc_ok_CCResult_\w+|cc_err_CCResult_\w+)\b')

def emit_spans(text: str):
    spans = []
    for m in re.finditer(r'@emit\(\s*`', text):
        start = m.end()
        i = start
        while i < len(text):
            if text[i] == '\\':
                i += 2
                continue
            if text[i] == '`':
                break
            i += 1
        spans.append((start, i))
    return spans

def in_spans(pos, spans):
    for a, b in spans:
        if a <= pos < b:
            return True
    return False

bad = []
for path in sorted((ROOT / 'cc/include/ccc').rglob('*.cch')):
    if path.name == 'cc_result.cch':
        continue
    text = path.read_text()
    spans = emit_spans(text)
    pos = 0
    for lineno, line in enumerate(text.splitlines(keepends=True), 1):
        start = pos
        pos += len(line)
        if SPEC_OR_GUARD.search(line):
            continue
        for m in HIT.finditer(line):
            if in_spans(start + m.start(), spans):
                continue
            bad.append(f'{path}:{lineno}: {m.group(0)}')

if bad:
    print('check_result_cch_style: hand-written CCResult_* outside SPEC/emit:', file=sys.stderr)
    for b in bad:
        print(f'  {b}', file=sys.stderr)
    sys.exit(1)
print('check_result_cch_style: ok')
PY
