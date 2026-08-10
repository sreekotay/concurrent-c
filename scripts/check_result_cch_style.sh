#!/usr/bin/env bash
# Gate: .cch Result types/ctors use T !>(E) + cc_ok/cc_err, not hand-written
# CCResult_* / cc_ok_CCResult_* / CCRes* outside SPEC registrations and
# cc_result.cch. @emit(`...`) payloads are host C and may keep mangled ABI
# names and the CCRes* token-paste macros.
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
# CCRes / CCResPtr / CCRes_ok / CCRes_err: emit-only bridge (defined in cc_result).
HIT = re.compile(
    r'\b(?:CCResult_\w+|cc_ok_CCResult_\w+|cc_err_CCResult_\w+|'
    r'CCRes(?:Ptr)?(?:_ok|_err)?)\b'
)

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
    in_block = False
    for lineno, line in enumerate(text.splitlines(keepends=True), 1):
        start = pos
        pos += len(line)
        if SPEC_OR_GUARD.search(line):
            continue
        # Strip // and /* */ (including multi-line) before matching.
        code_chars = []
        i = 0
        n = len(line)
        while i < n:
            if in_block:
                if line[i:i + 2] == '*/':
                    in_block = False
                    i += 2
                else:
                    i += 1
                continue
            if line[i:i + 2] == '//':
                break
            if line[i:i + 2] == '/*':
                in_block = True
                i += 2
                continue
            code_chars.append(line[i])
            i += 1
        code = ''.join(code_chars)
        for m in HIT.finditer(code):
            tok = m.group(0)
            skip = False
            for rm in re.finditer(re.escape(tok), line):
                if in_spans(start + rm.start(), spans):
                    skip = True
                    break
            if skip:
                continue
            bad.append(f'{path}:{lineno}: {tok}')

if bad:
    print('check_result_cch_style: hand-written Result ABI outside SPEC/emit:',
          file=sys.stderr)
    for b in bad:
        print(f'  {b}', file=sys.stderr)
    sys.exit(1)
print('check_result_cch_style: ok')
PY
