#!/usr/bin/env python3
"""Emit-shape ratchet for spec §5.1 soft-return / registration watermark.

In any function with registered function-scope cleanup:
  - every bare `return` before `__cc_cleanup:` is forbidden (must soft-goto)
  - every cleanup statement in the epilogue sits under
    `if (__cc_defer_hw …)` or `if (__cc_shwN …)`

Final `return __cc_retval;` / `return NULL;` after the epilogue is allowed.
"""
from __future__ import annotations

import re
import sys

SIG = re.compile(
    r"^(?:static\s+)?(?:inline\s+)?[\w\s\*]+?\b(\w+)\s*\([^;{]*\)\s*\{\s*$"
)
BARE_RETURN = re.compile(r"^\s*return\b")
HW_GUARD = re.compile(r"\bif\s*\(\s*__cc_defer_hw\b")
SHW_GUARD = re.compile(r"\bif\s*\(\s*__cc_shw\d*\b")
CLEANUP_LABEL = re.compile(r"^__cc_cleanup\s*:")
# Soft-return epilogue tail (not a cleanup hook).
EPILOGUE_RETURN_OK = re.compile(
    r"^\s*(?:if\s*\(\s*__cc_ret_set\s*\)\s*)?return\s+(?:__cc_retval|NULL)\s*;\s*$"
)


def functions(lines: list[str]) -> list[tuple[str, int, int]]:
    out: list[tuple[str, int, int]] = []
    start = None
    name = None
    depth = 0
    for i, line in enumerate(lines):
        if start is None:
            m = SIG.match(line)
            if m and not line.lstrip().startswith("#"):
                start = i
                name = m.group(1)
                depth = 0
        if start is not None:
            depth += line.count("{") - line.count("}")
            if depth <= 0:
                out.append((name or "?", start, i))
                start = None
                name = None
    return out


def is_commentish(line: str) -> bool:
    s = line.strip()
    return not s or s.startswith("//") or s.startswith("/*") or s.startswith("*")


def is_scaffold(line: str) -> bool:
    """Control / label / soft-return scaffolding — not a cleanup hook body."""
    s = line.strip()
    if not s:
        return True
    if s in ("{", "}") or s.startswith(("else", "//", "/*", "*", "#")):
        return True
    if s.startswith("if ") or s.startswith("if("):
        return True
    if EPILOGUE_RETURN_OK.match(line):
        return True
    if s.startswith("return"):
        return True  # other returns checked separately
    if s.startswith("__cc_"):
        return True
    return False


def is_hook_stmt(line: str) -> bool:
    s = line.strip()
    return bool(s) and s.endswith(";") and not is_scaffold(line)


def check_file(path: str, src_label: str) -> list[str]:
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    errs: list[str] = []
    for name, a, b in functions(lines):
        body = lines[a : b + 1]
        text = "\n".join(body)
        if "__cc_defer_hw" not in text or "__cc_cleanup" not in text:
            continue

        hw_i = clean_i = None
        for j, line in enumerate(body):
            if hw_i is None and "__cc_defer_hw" in line:
                hw_i = j
            if clean_i is None and CLEANUP_LABEL.match(line.strip()):
                clean_i = j
        if hw_i is None or clean_i is None:
            errs.append(f"{src_label}:{name}: missing __cc_defer_hw / __cc_cleanup")
            continue

        for j in range(hw_i, clean_i):
            line = body[j]
            if is_commentish(line):
                continue
            if BARE_RETURN.search(line):
                errs.append(
                    f"{src_label}:{name}: bare return before __cc_cleanup "
                    f"(emitted line {a + j + 1}): {line.strip()}"
                )

        guard = 0
        for j in range(clean_i + 1, len(body)):
            line = body[j]
            if is_commentish(line):
                continue

            opens_guard = bool(HW_GUARD.search(line) or SHW_GUARD.search(line))
            if opens_guard:
                guard += max(1, line.count("{")) if "{" in line else 1
                # still fall through to brace accounting when `{`/`}` on same line

            if is_hook_stmt(line) and guard <= 0:
                errs.append(
                    f"{src_label}:{name}: unguarded cleanup stmt "
                    f"(emitted line {a + j + 1}): {line.strip()}"
                )

            opens = line.count("{")
            closes = line.count("}")
            if opens and not opens_guard and guard > 0:
                guard += opens
            if closes:
                # Leaving guarded if-blocks.
                guard = max(0, guard - closes)

            if BARE_RETURN.search(line) and not EPILOGUE_RETURN_OK.match(line):
                errs.append(
                    f"{src_label}:{name}: unexpected return in epilogue "
                    f"(emitted line {a + j + 1}): {line.strip()}"
                )
    return errs


def main() -> int:
    if len(sys.argv) < 2:
        print(
            "usage: lint_soft_return_emit.py EMITTED.c [SRC_LABEL]",
            file=sys.stderr,
        )
        return 2
    path = sys.argv[1]
    label = sys.argv[2] if len(sys.argv) > 2 else path
    errs = check_file(path, label)
    for e in errs:
        print(f"lint_soft_return_emit: {e}", file=sys.stderr)
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
