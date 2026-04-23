#!/usr/bin/env python3
"""Emit a Jackson Allan cc.h copy whose public cc_* API uses a distinct prefix.

Concurrent-C stdlib headers expose cc_vec_* and other cc_* symbols; Jackson's
cc.h uses the same prefix. Renaming every \\bcc_ token to ccj_ avoids link-time
and same-TU redefinition clashes while keeping CC_* macro machinery unchanged.
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

_CC_PREFIX_RE = re.compile(r"\bcc_")


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("input", type=Path, help="Path to upstream cc.h")
    p.add_argument("output", type=Path, help="Path to write namespaced header")
    p.add_argument(
        "--prefix",
        default="ccj_",
        help="Replacement for leading cc_ (default: ccj_)",
    )
    args = p.parse_args()
    src = args.input.read_text(encoding="utf-8")
    out = f"/* Generated from {args.input.name} — do not edit; rerun script after vendor bump. */\n"
    out += _CC_PREFIX_RE.sub(args.prefix, src)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(out, encoding="utf-8")


if __name__ == "__main__":
    main()
