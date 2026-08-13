#!/usr/bin/env bash
# Compile-and-run every runnable fence in the typehooks/typeview tutorial.
#
# A fence is runnable when it is ```c / ```ccs and starts with `#!ccc ccs`.
# Optional pin immediately after the fence:
#
#   <!-- smoke-stdout
#   expected line
#   -->
#
# The markdown is the source of truth — do not copy examples into tests/.
# Usage: scripts/test_doc_fences.sh [doc.md ...]
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
CCC="${CCC:-./cc/bin/ccc}"

if [ ! -x "$CCC" ]; then
  echo "[test_doc_fences] FAIL: $CCC not executable" >&2
  exit 1
fi

if [ "$#" -gt 0 ]; then
  DOCS=("$@")
else
  DOCS=(docs/typehooks-typeviews.md)
fi

# Pin so deleting a tutorial fence is a loud miss, not a quieter suite.
TYPEHOOKS_TUTORIAL_FENCES=7

python3 - "$CCC" "$TYPEHOOKS_TUTORIAL_FENCES" "${DOCS[@]}" <<'PY'
import pathlib, re, subprocess, sys, tempfile, os

ccc = sys.argv[1]
expected_tutorial = int(sys.argv[2])
docs = sys.argv[3:]

fence_re = re.compile(
    r"```(?:c|ccs)\n(.*?)```(?:\n<!-- smoke-stdout\n(.*?)-->)?",
    re.S,
)

fail = 0
tutorial_n = 0

for doc in docs:
    text = pathlib.Path(doc).read_text()
    blocks = fence_re.findall(text)
    runnable = []
    for body, stdout_pin in blocks:
        if body.startswith("#!ccc ccs"):
            runnable.append((body, stdout_pin.rstrip("\n") if stdout_pin else None))
    if doc.endswith("typehooks-typeviews.md"):
        tutorial_n = len(runnable)
        if tutorial_n != expected_tutorial:
            print(
                f"[test_doc_fences] FAIL: {doc} has {tutorial_n} runnable "
                f"fences, expected {expected_tutorial}",
                file=sys.stderr,
            )
            fail = 1
    if not runnable:
        print(f"[test_doc_fences] FAIL: {doc}: no #!ccc ccs fences", file=sys.stderr)
        fail = 1
        continue
    print(f"[test_doc_fences] {doc}: {len(runnable)} fences")
    for i, (body, pin) in enumerate(runnable, 1):
        with tempfile.NamedTemporaryFile(
            "w", suffix=".ccs", prefix=f"doc_fence_ex{i:02d}_", delete=False
        ) as f:
            f.write(body)
            path = f.name
        try:
            r = subprocess.run(
                [ccc, "run", path],
                capture_output=True,
                text=True,
            )
        finally:
            os.unlink(path)
        got = (r.stdout or "").rstrip("\n")
        if r.returncode != 0:
            err = (r.stderr or r.stdout or "").strip()
            print(f"[test_doc_fences]   ex{i:02d} FAIL rc={r.returncode}", file=sys.stderr)
            if err:
                print(err[-1200:], file=sys.stderr)
            fail = 1
            continue
        if pin is not None and got != pin:
            print(
                f"[test_doc_fences]   ex{i:02d} FAIL stdout\n"
                f"    expected: {pin!r}\n"
                f"    got:      {got!r}",
                file=sys.stderr,
            )
            fail = 1
            continue
        print(f"[test_doc_fences]   ex{i:02d} OK")

if fail:
    sys.exit(1)
print("[test_doc_fences] OK")
PY
