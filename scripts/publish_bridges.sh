#!/usr/bin/env bash
# Clean-pack (and optionally publish) the Node↔Python bridge packages:
#   npm  concurrent-c-python  → out/concurrent-c-python-*.tgz
#   pip  concurrent-c-node    → out/pypi/concurrent_c_node-*
#
# Usage (from repo root):
#   ./scripts/publish_bridges.sh              # clean + pack only
#   ./scripts/publish_bridges.sh --publish    # pack, then npm + twine upload
#
# Needs: ./cc/bin/ccc, node, a C compiler, python3 + build (+ twine to publish).
# Auth: npm login / ~/.npmrc; PyPI via ~/.pypirc or TWINE_USERNAME/TWINE_PASSWORD.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PUBLISH=0
for arg in "$@"; do
  case "$arg" in
    --publish) PUBLISH=1 ;;
    -h|--help)
      cat <<'EOF'
Clean-pack (and optionally publish) the Node↔Python bridge packages:
  npm  concurrent-c-python  → out/concurrent-c-python-*.tgz
  pip  concurrent-c-node    → out/pypi/concurrent_c_node-*

Usage (from repo root):
  ./scripts/publish_bridges.sh              # clean + pack only
  ./scripts/publish_bridges.sh --publish    # pack, then npm + twine upload

Needs: ./cc/bin/ccc, node, a C compiler, python3 + build (+ twine to publish).
Auth: npm login / ~/.npmrc; PyPI via ~/.pypirc or TWINE_USERNAME/TWINE_PASSWORD.
EOF
      exit 0
      ;;
    *)
      echo "unknown arg: $arg (try --publish)" >&2
      exit 2
      ;;
  esac
done

if [[ ! -x ./cc/bin/ccc ]]; then
  echo "missing ./cc/bin/ccc — build the toolchain first (make cc)" >&2
  exit 1
fi
command -v node >/dev/null || { echo "need node on PATH" >&2; exit 1; }
command -v python3 >/dev/null || { echo "need python3 on PATH" >&2; exit 1; }
if ! python3 -c 'import build' 2>/dev/null; then
  echo "need Python 'build' module: python3 -m pip install build" >&2
  exit 1
fi
if [[ "$PUBLISH" -eq 1 ]] && ! python3 -c 'import twine' 2>/dev/null; then
  echo "need Python 'twine' module: python3 -m pip install twine" >&2
  exit 1
fi

echo "== clean prior bridge pack products"
rm -f out/concurrent-c-python-*.tgz out/cc-python-*.tgz
rm -rf out/pypi
rm -rf npm/cc-python/bin npm/cc-python/vendor
rm -rf pypi/cc-node/dist pypi/cc-node/build pypi/cc-node/*.egg-info

echo "== npm concurrent-c-python (prepare + pack)"
./npm/cc-python/scripts/prepare-publish.sh
NPM_TGZ="$(ls -1t out/concurrent-c-python-*.tgz | head -1)"
echo "   $NPM_TGZ"

echo "== pip concurrent-c-node (sdist + wheel)"
mkdir -p out/pypi
python3 -m build --outdir out/pypi pypi/cc-node
ls -1 out/pypi/concurrent_c_node-*

if [[ "$PUBLISH" -eq 0 ]]; then
  echo
  echo "packed only. deploy with:"
  echo "  ./scripts/publish_bridges.sh --publish"
  exit 0
fi

echo "== npm publish $NPM_TGZ"
npm publish "$NPM_TGZ" --access public

echo "== twine upload out/pypi/concurrent_c_node-*"
python3 -m twine upload out/pypi/concurrent_c_node-*

echo
echo "live:"
echo "  https://www.npmjs.com/package/concurrent-c-python"
echo "  https://pypi.org/project/concurrent-c-node/"
