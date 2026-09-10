#!/usr/bin/env python3
"""tinysearch — tiny whole-file trigram index.

Index stores only: path, mtime, size, and which files contain which 3-byte grams.
Search intersects posting lists, then reads surviving files.

    python3 tinysearch.py index ~/src
    python3 tinysearch.py search 'TODO' ~/src
    python3 tinysearch.py search -i 'malloc' ~/src

Index file: <root>/.tinysearch.idx  (override with -f)
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys
import time
from collections import defaultdict
from pathlib import Path

SKIP_DIRS = {
    ".git", ".hg", ".svn", "node_modules", "__pycache__", ".venv",
    "venv", "target", "dist", "build", ".tinysearch.idx",
}
SKIP_SUFFIX = {
    ".png", ".jpg", ".jpeg", ".gif", ".webp", ".ico", ".pdf",
    ".zip", ".gz", ".bz2", ".xz", ".7z", ".tar",
    ".woff", ".woff2", ".ttf", ".eot",
    ".mp3", ".mp4", ".mkv", ".avi", ".mov",
    ".o", ".so", ".dylib", ".dll", ".a", ".exe",
    ".pyc", ".pyo", ".class", ".wasm",
}
MAX_FILE = 2 * 1024 * 1024  # 2 MiB
SAMPLE = 4096
MAGIC = b"TS01"
NUL = b"\x00"


def is_binary_sample(buf: bytes) -> bool:
    if not buf:
        return False
    if b"\x00" in buf:
        return True
    # high ratio of non-text bytes
    weird = sum(b < 9 or (13 < b < 32) or b > 126 for b in buf)
    return weird / len(buf) > 0.30


def grams_of(data: bytes) -> set[int]:
    """24-bit overlapping trigrams. Lowercased ASCII letters only folded."""
    if len(data) < 3:
        return set()
    # fold A-Z to a-z in a copy only if needed
    b = bytearray(data)
    for i, c in enumerate(b):
        if 65 <= c <= 90:
            b[i] = c + 32
    out: set[int] = set()
    n = len(b)
    for i in range(n - 2):
        out.add(b[i] | (b[i + 1] << 8) | (b[i + 2] << 16))
    return out


def grams_of_query(q: str) -> set[int]:
    return grams_of(q.encode("utf-8", "surrogateescape"))


def should_skip_dir(name: str) -> bool:
    return name in SKIP_DIRS or name.startswith(".")


def iter_files(root: Path):
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames[:] = [d for d in dirnames if not should_skip_dir(d)]
        for name in filenames:
            if name == ".tinysearch.idx":
                continue
            p = Path(dirpath) / name
            if p.suffix.lower() in SKIP_SUFFIX:
                continue
            yield p


# --- on-disk format ---
# MAGIC
# uint32 nfiles
# for each file:
#   uint64 mtime_ns, uint64 size, uint32 path_len, path bytes
# uint32 ngrams
# for each gram:
#   uint32 gram, uint32 npost, npost * uint32 file_id
# Paths stored relative to root, posix.


def save_index(path: Path, root: Path, files: list[tuple[str, int, int]], postings: dict[int, list[int]]) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<I", len(files)))
        for rel, mtime_ns, size in files:
            b = rel.encode("utf-8")
            f.write(struct.pack("<QQI", mtime_ns, size, len(b)))
            f.write(b)
        f.write(struct.pack("<I", len(postings)))
        for gram in sorted(postings):
            ids = postings[gram]
            f.write(struct.pack("<II", gram, len(ids)))
            f.write(struct.pack("<" + "I" * len(ids), *ids))
    tmp.replace(path)


def load_index(path: Path) -> tuple[list[tuple[str, int, int]], dict[int, list[int]]]:
    with path.open("rb") as f:
        if f.read(4) != MAGIC:
            raise SystemExit(f"bad index magic: {path}")
        (nfiles,) = struct.unpack("<I", f.read(4))
        files = []
        for _ in range(nfiles):
            mtime_ns, size, plen = struct.unpack("<QQI", f.read(20))
            rel = f.read(plen).decode("utf-8")
            files.append((rel, mtime_ns, size))
        (ngrams,) = struct.unpack("<I", f.read(4))
        postings: dict[int, list[int]] = {}
        for _ in range(ngrams):
            gram, npost = struct.unpack("<II", f.read(8))
            raw = f.read(4 * npost)
            postings[gram] = list(struct.unpack("<" + "I" * npost, raw))
    return files, postings


def file_meta(p: Path) -> tuple[int, int] | None:
    try:
        st = p.stat()
    except OSError:
        return None
    if not os.path.isfile(p) or st.st_size > MAX_FILE:
        return None
    return int(st.st_mtime_ns), int(st.st_size)


def index_root(root: Path, idx_path: Path) -> None:
    old_files: list[tuple[str, int, int]] = []
    old_post: dict[int, list[int]] = {}
    old_by_path: dict[str, tuple[int, int, int]] = {}
    if idx_path.exists():
        old_files, old_post = load_index(idx_path)
        for i, (rel, m, s) in enumerate(old_files):
            old_by_path[rel] = (i, m, s)

    # invert old postings: file_id -> grams  (only for reused files)
    old_grams_by_file: dict[int, list[int]] = defaultdict(list)
    for g, ids in old_post.items():
        for i in ids:
            old_grams_by_file[i].append(g)

    new_files: list[tuple[str, int, int]] = []
    new_post: dict[int, list[int]] = defaultdict(list)
    reused = scanned = skipped = 0
    t0 = time.perf_counter()

    for p in iter_files(root):
        meta = file_meta(p)
        if meta is None:
            skipped += 1
            continue
        mtime_ns, size = meta
        rel = p.relative_to(root).as_posix()
        fid = len(new_files)

        prev = old_by_path.get(rel)
        if prev and prev[1] == mtime_ns and prev[2] == size:
            new_files.append((rel, mtime_ns, size))
            for g in old_grams_by_file.get(prev[0], ()):
                new_post[g].append(fid)
            reused += 1
            continue

        try:
            data = p.read_bytes()
        except OSError:
            skipped += 1
            continue
        if is_binary_sample(data[:SAMPLE]):
            skipped += 1
            continue
        new_files.append((rel, mtime_ns, size))
        for g in grams_of(data):
            new_post[g].append(fid)
        scanned += 1

    save_index(idx_path, root, new_files, dict(new_post))
    dt = time.perf_counter() - t0
    print(
        f"indexed {len(new_files)} files "
        f"(scanned {scanned}, reused {reused}, skipped {skipped}) "
        f"→ {idx_path} ({idx_path.stat().st_size:,} bytes) in {dt:.2f}s"
    )


def search_root(root: Path, idx_path: Path, query: str, ignore_case: bool, regex: bool) -> int:
    if not idx_path.exists():
        print("no index; run: tinysearch.py index", file=sys.stderr)
        return 2
    files, postings = load_index(idx_path)
    qgrams = grams_of_query(query)
    if len(query.encode()) >= 3 and qgrams:
        # start from rarest gram
        lists = []
        missing = False
        for g in qgrams:
            ids = postings.get(g)
            if not ids:
                missing = True
                break
            lists.append(ids)
        if missing:
            return 0
        lists.sort(key=len)
        cand = set(lists[0])
        for lst in lists[1:]:
            cand.intersection_update(lst)
            if not cand:
                break
    else:
        cand = set(range(len(files)))

    flags = re.I if ignore_case else 0
    if regex:
        cre = re.compile(query.encode() if False else query, flags)
        def match(text: str) -> bool:
            return cre.search(text) is not None
    else:
        needle = query.lower() if ignore_case else query
        def match(text: str) -> bool:
            hay = text.lower() if ignore_case else text
            return needle in hay

    hits = 0
    for fid in sorted(cand):
        rel, _, _ = files[fid]
        p = root / rel
        try:
            raw = p.read_bytes()
        except OSError:
            continue
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            text = raw.decode("utf-8", "replace")
        if not match(text):
            continue
        # print matching lines
        for i, line in enumerate(text.splitlines(), 1):
            if match(line):
                print(f"{rel}:{i}:{line}")
                hits += 1
    return 0 if hits else 1


def default_idx(root: Path) -> Path:
    return root / ".tinysearch.idx"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Tiny whole-file trigram search index")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_idx = sub.add_parser("index", help="build or refresh index")
    p_idx.add_argument("root", type=Path, nargs="?", default=Path("."))
    p_idx.add_argument("-f", "--index", type=Path)

    p_s = sub.add_parser("search", help="search using the index")
    p_s.add_argument("query")
    p_s.add_argument("root", type=Path, nargs="?", default=Path("."))
    p_s.add_argument("-f", "--index", type=Path)
    p_s.add_argument("-i", "--ignore-case", action="store_true")
    p_s.add_argument("-e", "--regex", action="store_true")

    args = ap.parse_args(argv)
    root = args.root.resolve()
    idx = args.index or default_idx(root)

    if args.cmd == "index":
        index_root(root, idx)
        return 0
    return search_root(root, idx, args.query, args.ignore_case, args.regex)


if __name__ == "__main__":
    raise SystemExit(main())
