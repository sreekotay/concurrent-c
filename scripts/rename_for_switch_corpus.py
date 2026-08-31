#!/usr/bin/env python3
"""Rename CC corpus: for-in → @for, checked switch → @switch."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

DIRS = [
    ROOT / "examples",
    ROOT / "tests",
    ROOT / "docs",
    ROOT / "spec",
    ROOT / "perf",
    ROOT / "real_projects",
]

EXTRA_FILES = [
    ROOT / "vscode" / "cc-lsp" / "cc_lsp_hover.cch",
]

EXTENSIONS = {".ccs", ".cch", ".md", ".compile_err", ".shcc"}

# C for-loop header: has semicolon before closing paren, or empty infinite loop.
C_FOR_RE = re.compile(
    r"for\s*\(\s*(?:;|(?:int|long|size_t|uint(?:8|16|32|64)?_t|char|short|float|double|void|const\s+int)\b|[\w.]+\s*=)"
)

FOR_IN_HEADER_RE = re.compile(r"for\s*\(([^)]*)\)")
PARALLEL_FOR_RE = re.compile(r"@parallel\s+for\s*\(")
COMPTIME_FOR_RE = re.compile(r"@comptime\s+for\b")
VARIANT_CASE_RE = re.compile(r"case\s+\.")
STRING_CASE_RE = re.compile(r'case\s+"')


def is_for_in_header(header: str) -> bool:
    if " in " not in header and not re.search(r"\bin\b", header):
        return False
    if ";" in header:
        return False
    if re.search(r"=\s*[^=]", header) and ".." not in header:
        # `i = 0` style init, not range `0..3`
        return False
    return True


def transform_for_line(line: str) -> str:
    if COMPTIME_FOR_RE.search(line):
        return line
    if PARALLEL_FOR_RE.search(line):
        line = PARALLEL_FOR_RE.sub("@parallel for (", line, count=1)

    if C_FOR_RE.search(line):
        return line

    m = FOR_IN_HEADER_RE.search(line)
    if not m:
        return line
    if not is_for_in_header(m.group(1)):
        return line
    idx = m.start()
    if idx > 0 and line[idx - 1] == "@":
        return line
    return line[:idx] + "@for (" + m.group(1) + ")" + line[m.end() :]


def find_switch_end(text: str, start: int) -> int:
    """Return index after matching `}` for switch body starting at `start` (open `{`)."""
    depth = 0
    i = start
    in_str = None
    escape = False
    while i < len(text):
        c = text[i]
        if in_str:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == in_str:
                in_str = None
            i += 1
            continue
        if c in "\"'":
            in_str = c
            i += 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return len(text)


def is_checked_switch(body: str) -> bool:
    return bool(VARIANT_CASE_RE.search(body) or STRING_CASE_RE.search(body))


def transform_switches(text: str) -> str:
    out: list[str] = []
    i = 0
    switch_re = re.compile(r"(?<![@\w])switch\s*\(")
    while i < len(text):
        m = switch_re.search(text, i)
        if not m:
            out.append(text[i:])
            break
        out.append(text[i : m.start()])
        # Already @switch?
        if m.start() > 0 and text[m.start() - 1] == "@":
            out.append(text[m.start() : m.end()])
            i = m.end()
            continue
        # Find opening brace of switch body
        j = m.end()
        paren = 1
        while j < len(text) and paren:
            if text[j] == "(":
                paren += 1
            elif text[j] == ")":
                paren -= 1
            j += 1
        while j < len(text) and text[j] in " \t\n\r":
            j += 1
        if j >= len(text) or text[j] != "{":
            out.append(text[m.start() : j])
            i = j
            continue
        body_end = find_switch_end(text, j)
        body = text[j:body_end]
        if is_checked_switch(body):
            out.append("@switch (")
            out.append(text[m.end() : body_end])
        else:
            out.append(text[m.start() : body_end])
        i = body_end
    return "".join(out)


def transform_content(text: str) -> str:
    lines = text.splitlines(keepends=True)
    lines = [transform_for_line(line) for line in lines]
    return transform_switches("".join(lines))


SKIP_DIR_PARTS = {".cc-build", "out"}

def iter_files() -> list[Path]:
    files: list[Path] = []
    for d in DIRS:
        if not d.is_dir():
            continue
        for p in d.rglob("*"):
            if any(part in SKIP_DIR_PARTS for part in p.parts):
                continue
            if p.suffix in EXTENSIONS and p.is_file():
                files.append(p)
    for p in EXTRA_FILES:
        if p.is_file():
            files.append(p)
    return sorted(set(files))


def main() -> int:
    changed: list[Path] = []
    for path in iter_files():
        original = path.read_text(encoding="utf-8")
        updated = transform_content(original)
        if updated != original:
            path.write_text(updated, encoding="utf-8")
            changed.append(path)
    rel = [str(p.relative_to(ROOT)) for p in changed]
    print(f"changed {len(changed)} files")
    for r in rel:
        print(r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
