#!/usr/bin/env python3
"""Rewrite CCS/CCH/MD samples after the CCArena box.

- Boolean `.base` on an arena handle → `cc_arena_is_live`
- Compound `!x.base || !y.base` liveness
- Host-field pokes on known arena names → `.a->field`
- Slab `.base` used as a pointer → `.a->base`
- `detach()` assignments gain `!>` unless already Result-typed
Does not touch `.base.ptr` / `.base.len` / `.base.message`.
Skips bootstrap snapshots.
"""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SKIP_DIR_PARTS = {
    "bootstrap",
    "last-good",
    "node_modules",
    ".git",
    "out",
    "third_party",
}

RECV = r"[A-Za-z_][\w]*(?:(?:->|\.)[A-Za-z_][\w]*|\[\w+\])*"

BOOL_PATTERNS = [
    (
        re.compile(rf"\bif\s*\(\s*!\s*({RECV})\.base\s*\)"),
        r"if (!cc_arena_is_live(\1))",
    ),
    (
        re.compile(rf"\bif\s*\(\s*({RECV})\.base\s*\)"),
        r"if (cc_arena_is_live(\1))",
    ),
    (
        re.compile(rf"\bassert\s*\(\s*!\s*({RECV})\.base\s*\)"),
        r"assert(!cc_arena_is_live(\1))",
    ),
    (
        re.compile(rf"\bassert\s*\(\s*({RECV})\.base\s*\)"),
        r"assert(cc_arena_is_live(\1))",
    ),
    (
        re.compile(rf"({RECV})\.base\s*==\s*NULL"),
        r"!cc_arena_is_live(\1)",
    ),
    (
        re.compile(rf"({RECV})\.base\s*!=\s*NULL"),
        r"cc_arena_is_live(\1)",
    ),
    (
        re.compile(rf"({RECV})\.base\s*==\s*0\b"),
        r"!cc_arena_is_live(\1)",
    ),
    (
        re.compile(rf"!\s*({RECV})\.base\s*\|\|\s*!\s*({RECV})\.base"),
        r"!cc_arena_is_live(\1) || !cc_arena_is_live(\2)",
    ),
]

# Handle receivers whose host fields samples poke.
ARENA_RECV = (
    r"(?:g_arena|g_shared|scratch|heap|second|taken|dst|first|other|"
    r"owner|child|kid|tmp|moved|bulk|ovf|tip|"
    r"conn(?:->|\.)reply_arena|got\.arena|"
    r"[a-zA-Z_][\w]*)"
)
HOST_FIELDS = (
    "block_max",
    "_flags",
    "block_idx",
    "ovf_head",
    "provenance",
)
# `prev` only in files that treat it as an arena host field.
PREV_FILES = {
    "tests/arena_concurrent_grow_smoke.ccs",
    "stress/arena_lifetime_chaos.ccs",
    "stress/arena_memory_storm.ccs",
}

# Slab-pointer `.base` (not liveness, not slice/error).
SLAB_BASE_FILES = {
    "tests/channel_send_arena_len_smoke.ccs",
    "tests/arena_buf_smoke.ccs",
    "stress/arena_memory_storm.ccs",
}

HOST_FIELD_FILES = {
    "tests/arena_concurrent_grow_smoke.ccs",
    "tests/string_arena_swap_smoke.ccs",
    "tests/slice_materialize_in_smoke.ccs",
    "tests/channel_send_struct_from_parts_smoke.ccs",
    "tests/channel_send_arena_len_smoke.ccs",
    "real_projects/redis/redis_idiomatic.ccs",
    "real_projects/redis/redis_owner.ccs",
    "stress/arena_malloc_cost_bench.ccs",
    "stress/arena_lifetime_chaos.ccs",
    "stress/arena_memory_storm.ccs",
    "stress/arena_memory_bench.ccs",
    "stress/arena_mixed_lifetime_split_bench.ccs",
}

SCAN_ROOTS = (
    "tests",
    "examples",
    "docs",
    "real_projects",
    "perf",
    "stress",
    "studies",
    "tools/fuzz_findings",
    "vscode",
    "npm",
    "spec",
)


def skip_path(p: Path) -> bool:
    parts = set(p.parts)
    if parts & SKIP_DIR_PARTS:
        return True
    if "shadow_lower" in p.parts and "bootstrap" in p.parts:
        return True
    return False


def peel_host_fields(text: str, rel: str) -> str:
    if rel not in HOST_FIELD_FILES:
        return text
    fields = list(HOST_FIELDS)
    if rel in PREV_FILES:
        fields.append("prev")
    for field in fields:
        text = re.sub(
            rf"(?<![\w>])({ARENA_RECV})\.{field}\b",
            rf"\1.a->{field}",
            text,
        )
        # undo double-peel if re-run
        text = text.replace(f".a->a->{field}", f".a->{field}")
    return text


def peel_slab_base(text: str, rel: str) -> str:
    if rel not in SLAB_BASE_FILES:
        return text
    # pointer arithmetic / compare against a buffer, not `.base.ptr`
    text = re.sub(
        rf"(?<![\w>])({ARENA_RECV})\.base\b(?!\.)",
        r"\1.a->base",
        text,
    )
    text = text.replace(".a->a->base", ".a->base")
    return text


def rewrite_detach(text: str) -> str:
    out = []
    for line in text.splitlines(keepends=True):
        stripped = line.lstrip()
        if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
            out.append(line)
            continue
        if "detach()" not in line:
            out.append(line)
            continue
        if "!>" in line:
            out.append(line)
            continue
        if re.search(r"CCArena\s*!>\s*\(\s*CCError\s*\)", line):
            out.append(line)
            continue
        line = re.sub(r"\.detach\(\)", r".detach() !>", line)
        out.append(line)
    return "".join(out)


def rewrite_text(text: str, rel: str) -> str:
    for rx, repl in BOOL_PATTERNS:
        text = rx.sub(repl, text)
    text = peel_host_fields(text, rel)
    text = peel_slab_base(text, rel)
    if rel.endswith((".ccs", ".cch", ".md")):
        text = rewrite_detach(text)
    return text


def main() -> None:
    nfiles = 0
    for rel_root in SCAN_ROOTS:
        root = ROOT / rel_root
        if not root.exists():
            continue
        for ext in (".ccs", ".cch", ".md"):
            for p in root.rglob(f"*{ext}"):
                if skip_path(p):
                    continue
                rel = str(p.relative_to(ROOT))
                raw = p.read_text(encoding="utf-8")
                new = rewrite_text(raw, rel)
                if new != raw:
                    p.write_text(new, encoding="utf-8")
                    nfiles += 1
                    print(rel)
    print(f"updated {nfiles} files")


if __name__ == "__main__":
    main()
