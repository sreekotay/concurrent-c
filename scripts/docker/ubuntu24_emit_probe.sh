#!/usr/bin/env bash
# Inside ubuntu:24.04 container: install ccc like raytext CI, run emit probes.
#
# Env (set by smoke_ubuntu24.sh):
#   CCC_SRC       read-only mount (default /ccc)
#   RAYTEXT_SRC   read-only mount (optional /raytext)
#   CCC_CLONE     1 = clone github.com/sreekotay/concurrent-c instead of rsync CCC_SRC
#   CCC_REF       git ref when cloning (default main)
#   CCC_WORK      writable sandbox (default /work/concurrent-c)
#   RAYTEXT_WORK  writable sandbox (default /work/raytext)
set -euo pipefail

CCC_SRC="${CCC_SRC:-/ccc}"
RAYTEXT_SRC="${RAYTEXT_SRC:-/raytext}"
CCC_CLONE="${CCC_CLONE:-0}"
CCC_REF="${CCC_REF:-main}"
CCC_WORK="${CCC_WORK:-/work/concurrent-c}"
RAYTEXT_WORK="${RAYTEXT_WORK:-/work/raytext}"
PREFIX="${PREFIX:-/root/.local}"

rsync_tree() {
  local src="$1" dst="$2"
  mkdir -p "$dst"
  rsync -a --delete \
    --exclude out/ --exclude bin/ --exclude .git/objects/ \
    --exclude 'third_party/tcc/*.o' --exclude 'third_party/tcc/*.tmp' \
    "$src/" "$dst/" || {
      local rc=$?
      [ "$rc" -eq 24 ] || return "$rc"
      rsync -a --delete \
        --exclude out/ --exclude bin/ --exclude .git/objects/ \
        --exclude 'third_party/tcc/*.o' --exclude 'third_party/tcc/*.tmp' \
        "$src/" "$dst/"
    }
}

echo "== ubuntu24 emit probe ($(uname -m) $(uname -s)) =="

if [ "$CCC_CLONE" = "1" ]; then
  echo "== clone concurrent-c ref=$CCC_REF (CI parity) =="
  rm -rf "$CCC_WORK"
  git clone --filter=blob:none https://github.com/sreekotay/concurrent-c.git "$CCC_WORK"
  if [ -n "$CCC_REF" ] && [ "$CCC_REF" != "main" ]; then
    git -C "$CCC_WORK" checkout "$CCC_REF"
  fi
elif [ -d "$CCC_SRC/.git" ]; then
  if [ ! -d "$CCC_SRC" ]; then
    echo "error: CCC_SRC=$CCC_SRC not mounted" >&2
    exit 1
  fi
  echo "== git clone file://$CCC_SRC -> $CCC_WORK =="
  rm -rf "$CCC_WORK"
  git clone "file://$CCC_SRC" "$CCC_WORK"
else
  if [ ! -d "$CCC_SRC" ]; then
    echo "error: CCC_SRC=$CCC_SRC not mounted; use smoke_ubuntu24.sh or set CCC_CLONE=1" >&2
    exit 1
  fi
  echo "== rsync concurrent-c -> $CCC_WORK (no .git; submodules must be present) =="
  rsync_tree "$CCC_SRC" "$CCC_WORK"
fi

echo "== cc-install.sh (same flags as raytext CI) =="
PREFIX="$PREFIX" "$CCC_WORK/cc-install.sh" --no-editor-tools --no-add-to-path

export PATH="$PREFIX/bin:$PATH"
echo "== ccc --version =="
ccc --version
echo "== shadow_lower =="
ls -la "$PREFIX/bin/shadow_lower" 2>/dev/null || ls -la "$(dirname "$(command -v ccc)")/shadow_lower"

echo ""
echo "== concurrent-c for-in view smokes (--no-cache) =="
cd "$CCC_WORK"
ccc tests/for_in_sub_param_subj_smoke.ccs --no-cache
ccc tests/for_in_sub_result_param_subj_smoke.ccs --no-cache 2>&1 | tee /tmp/for_in_result.log
if rg -q 'for-in subject must be' /tmp/for_in_result.log; then
  echo "FAIL: for-in view rejected on Linux" >&2
  exit 1
fi

if [ -d "$RAYTEXT_SRC" ] || [ "$CCC_CLONE" = "1" ]; then
  :
else
  echo ""
  echo "== skip raytext (RAYTEXT_SRC not mounted) =="
  echo "ok: concurrent-c probes passed"
  exit 0
fi

if [ -d "$RAYTEXT_SRC" ]; then
  echo ""
  echo "== rsync raytext -> $RAYTEXT_WORK =="
  rsync_tree "$RAYTEXT_SRC" "$RAYTEXT_WORK"
elif [ -d "$RAYTEXT_WORK" ]; then
  echo "== raytext already in $RAYTEXT_WORK =="
else
  echo "== skip raytext (no source) =="
  exit 0
fi

echo ""
echo "== raytext layout_measure_smoke (--no-cache) =="
cd "$RAYTEXT_WORK"
ccc build --build-file build.cc --no-cache run layout_measure_smoke 2>&1 | tee /tmp/layout_smoke.log
if rg -q 'for-in subject must be|emit failed' /tmp/layout_smoke.log; then
  echo "FAIL: layout emit failed on Linux (Ubuntu parity)" >&2
  exit 1
fi

echo ""
echo "ok: ubuntu24 emit probes passed"
