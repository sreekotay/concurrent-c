#!/usr/bin/env bash
# Clean-pack (and optionally publish) the Node↔Python bridge packages:
#   npm  concurrent-c-python  → out/concurrent-c-python-*.tgz
#   pip  concurrent-c-node    → out/pypi/concurrent_c_node-*
#
# Usage (from repo root):
#   ./scripts/publish_bridges.sh                 # clean + pack only (no version bump)
#   ./scripts/publish_bridges.sh --publish --minor
#       bump, pack, commit+push; npm + PyPI via CI OIDC (default)
#   ./scripts/publish_bridges.sh --publish --npm-local --pypi-twine
#   ./scripts/publish_bridges.sh --publish --no-bump
#
# After --publish (OIDC path), commits the bumped version files, pushes, and
# dispatches publish-cc-python.yml + publish-cc-node.yml (needs gh on PATH).
#
# Pre-upload consumer gate (fresh Linux install, no repo on the path):
#   ./scripts/smoke_bridge_packs.sh              # pack + Docker smoke
#   ./scripts/smoke_bridge_packs.sh --no-pack --host
#
# Needs: ./cc/bin/ccc, node, a C compiler, python3 + build (+ twine if --pypi-twine).
# Auth: gh (OIDC workflows); or npm login / ~/.pypirc for local fallbacks.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PUBLISH=0
BUMP=patch          # patch | minor | major | none
# Default: Trusted Publishing from CI. Override with --npm-local / --pypi-twine.
NPM_VIA="${NPM_VIA:-ci}"
PYPI_VIA="${PYPI_VIA:-ci}"
for arg in "$@"; do
  case "$arg" in
    --publish) PUBLISH=1 ;;
    --patch) BUMP=patch ;;
    --minor) BUMP=minor ;;
    --major) BUMP=major ;;
    --no-bump) BUMP=none ;;
    --npm-ci) NPM_VIA=ci ;;
    --npm-local) NPM_VIA=local ;;
    --pypi-ci) PYPI_VIA=ci ;;
    --pypi-twine) PYPI_VIA=twine ;;
    -h|--help)
      cat <<'EOF'
Clean-pack (and optionally publish) the Node↔Python bridge packages:
  npm  concurrent-c-python  → out/concurrent-c-python-*.tgz
  pip  concurrent-c-node    → out/pypi/concurrent_c_node-*

Usage (from repo root):
  ./scripts/publish_bridges.sh                      # clean + pack only
  ./scripts/publish_bridges.sh --publish --minor    # bump, pack; both via CI OIDC
  ./scripts/publish_bridges.sh --publish --npm-local --pypi-twine
  ./scripts/publish_bridges.sh --publish --no-bump

Needs: ./cc/bin/ccc, node, a C compiler, python3.
Auth: gh (for OIDC). After pack, --publish commits the two version files,
  pushes, and runs publish-cc-python.yml + publish-cc-node.yml.
  --npm-local: npm publish from this machine (no provenance).
  --pypi-twine: local twine + ~/.pypirc instead of PyPI OIDC.
EOF
      exit 0
      ;;
    *)
      echo "unknown arg: $arg (try --publish / --minor / --npm-local / --pypi-twine)" >&2
      exit 2
      ;;
  esac
done

case "$NPM_VIA" in
  local|ci) ;;
  *)
    echo "NPM_VIA must be local or ci (got $NPM_VIA)" >&2
    exit 2
    ;;
esac
case "$PYPI_VIA" in
  twine|ci) ;;
  *)
    echo "PYPI_VIA must be twine or ci (got $PYPI_VIA)" >&2
    exit 2
    ;;
esac
if [[ ! -x ./cc/bin/ccc ]]; then
  echo "missing ./cc/bin/ccc — build the toolchain first (make cc)" >&2
  exit 1
fi
command -v node >/dev/null || { echo "need node on PATH" >&2; exit 1; }
command -v python3 >/dev/null || { echo "need python3 on PATH" >&2; exit 1; }

# Prefer a throwaway venv when system python lacks build/(twine if local upload).
PY=python3
NEED_TWINE=0
if [[ "$PUBLISH" -eq 1 && "$PYPI_VIA" == twine ]]; then NEED_TWINE=1; fi
if ! "$PY" -c 'import build' 2>/dev/null || \
   { [[ "$NEED_TWINE" -eq 1 ]] && ! "$PY" -c 'import twine' 2>/dev/null; }; then
  VENV="$ROOT/out/.publish-bridges-venv"
  if [[ ! -x "$VENV/bin/python" ]]; then
    echo "== bootstrap $VENV (build${NEED_TWINE:+ + twine})"
    "$PY" -m venv "$VENV"
    if [[ "$NEED_TWINE" -eq 1 ]]; then
      "$VENV/bin/pip" -q install -U pip build twine
    else
      "$VENV/bin/pip" -q install -U pip build
    fi
  else
    if [[ "$NEED_TWINE" -eq 1 ]]; then
      "$VENV/bin/pip" -q install -U build twine >/dev/null
    else
      "$VENV/bin/pip" -q install -U build >/dev/null
    fi
  fi
  PY="$VENV/bin/python"
fi
"$PY" -c 'import build' || { echo "need Python build module" >&2; exit 1; }
if [[ "$NEED_TWINE" -eq 1 ]]; then
  "$PY" -c 'import twine' || { echo "need Python twine module" >&2; exit 1; }
fi

bump_versions() {
  local kind="$1"
  "$PY" - "$kind" <<'PY'
import json, pathlib, re, sys

kind = sys.argv[1]
if kind == "none":
    sys.exit(0)

def bump(v: str) -> str:
    parts = v.strip().split(".")
    if len(parts) != 3 or not all(p.isdigit() for p in parts):
        raise SystemExit(f"expected semver X.Y.Z, got {v!r}")
    maj, mi, pa = map(int, parts)
    if kind == "major":
        maj, mi, pa = maj + 1, 0, 0
    elif kind == "minor":
        mi, pa = mi + 1, 0
    elif kind == "patch":
        pa += 1
    else:
        raise SystemExit(f"unknown bump kind {kind!r}")
    return f"{maj}.{mi}.{pa}"

pkg = pathlib.Path("npm/cc-python/package.json")
data = json.loads(pkg.read_text())
old_npm, new_npm = data["version"], bump(data["version"])
data["version"] = new_npm
pkg.write_text(json.dumps(data, indent=2) + "\n")

pyproject = pathlib.Path("pypi/cc-node/pyproject.toml")
text = pyproject.read_text()
m = re.search(r'(?m)^version\s*=\s*"([^"]+)"\s*$', text)
if not m:
    raise SystemExit("pypi/cc-node/pyproject.toml: no version = \"…\" line")
old_py, new_py = m.group(1), bump(m.group(1))
text = text[: m.start(1)] + new_py + text[m.end(1) :]
pyproject.write_text(text)

print(f"   concurrent-c-python  {old_npm} → {new_npm}")
print(f"   concurrent-c-node    {old_py} → {new_py}")
PY
}

if [[ "$PUBLISH" -eq 1 && "$BUMP" != none ]]; then
  echo "== bump versions ($BUMP)"
  bump_versions "$BUMP"
elif [[ "$PUBLISH" -eq 1 ]]; then
  echo "== versions unchanged (--no-bump)"
fi

echo "== bridge suites gate the pack (stress quick tier + pinned rungs)"
node tests/bridge_wire.js
python3 tests/bridge_wire.py
./stress/bridge/run.sh

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
"$PY" -m build --outdir out/pypi pypi/cc-node
ls -1 out/pypi/concurrent_c_node-*

if [[ "$PUBLISH" -eq 0 ]]; then
  echo
  echo "packed only (versions untouched). before upload, consumer-smoke the packs:"
  echo "  ./scripts/smoke_bridge_packs.sh --no-pack --host"
  echo "then deploy with:"
  echo "  ./scripts/publish_bridges.sh --publish --no-bump"
  exit 0
fi

NPM_VER="$(node -p "require('./npm/cc-python/package.json').version")"
PY_VER="$("$PY" -c 'import re,pathlib; t=pathlib.Path("pypi/cc-node/pyproject.toml").read_text(); print(re.search(r"(?m)^version\s*=\s*\"([^\"]+)\"", t).group(1))')"
REF="$(git rev-parse --abbrev-ref HEAD)"
NEED_GH=0
if [[ "$NPM_VIA" == ci || "$PYPI_VIA" == ci ]]; then NEED_GH=1; fi
if [[ "$NEED_GH" -eq 1 ]] && ! command -v gh >/dev/null 2>&1; then
  echo "need gh on PATH to dispatch OIDC publish workflows" >&2
  exit 1
fi

if [[ "$NPM_VIA" == local ]]; then
  # Absolute path — some npm versions treat relative out/... as a git remote.
  NPM_TGZ="$(cd "$(dirname "$NPM_TGZ")" && pwd)/$(basename "$NPM_TGZ")"
  echo "== npm publish $NPM_TGZ (local — no provenance)"
  if ! npm publish "$NPM_TGZ" --access public; then
    echo "npm publish failed — run \`npm login\` and retry --publish --npm-local" >&2
    exit 1
  fi
else
  echo "== npm via CI OIDC (skipped local publish; provenance from Actions)"
fi

if [[ "$PYPI_VIA" == twine ]]; then
  echo "== twine upload out/pypi/concurrent_c_node-*"
  "$PY" -m twine upload --skip-existing out/pypi/concurrent_c_node-*
else
  echo "== PyPI via CI OIDC (skipped local twine)"
fi

if [[ "$NEED_GH" -eq 1 ]]; then
  # Version bumps must be on the default branch for OIDC; commit only those
  # two files so a dirty tree does not sweep unrelated edits into the release.
  git add npm/cc-python/package.json pypi/cc-node/pyproject.toml
  if ! git diff --cached --quiet; then
    git commit -m "Release bridges ${NPM_VER} / ${PY_VER}"
  else
    echo "   version files already committed"
  fi
  git push origin HEAD
  if [[ "$NPM_VIA" == ci ]]; then
    gh workflow run publish-cc-python.yml --ref "$REF"
    echo "   dispatched publish-cc-python.yml"
  fi
  if [[ "$PYPI_VIA" == ci ]]; then
    gh workflow run publish-cc-node.yml --ref "$REF"
    echo "   dispatched publish-cc-node.yml"
  fi
  echo "   watch: gh run watch"
fi

echo
echo "npm:"
echo "  https://www.npmjs.com/package/concurrent-c-python/v/${NPM_VER}"
if [[ "$NPM_VIA" == ci ]]; then
  echo "  (OIDC workflow — provenance after success)"
fi
echo "PyPI:"
echo "  https://pypi.org/project/concurrent-c-node/${PY_VER}/"
if [[ "$PYPI_VIA" == ci ]]; then
  echo "  (OIDC workflow running — may take ~1 min)"
fi
