#!/usr/bin/env bash
# Pack concurrent-c-python / concurrent-c-node, then install them in a
# clean Linux container (no repo PYTHONPATH / no in-tree require) and run
# consumer smokes — the pre-upload gate for npm/PyPI.
#
#   ./scripts/smoke_bridge_packs.sh              # pack + docker smoke
#   ./scripts/smoke_bridge_packs.sh --no-pack    # reuse out/ artifacts
#   ./scripts/smoke_bridge_packs.sh --host       # also smoke on this host
#
# Needs: docker, ./cc/bin/ccc, node, python3 (+ build for pack).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

DO_PACK=1
DO_HOST=0
for arg in "$@"; do
  case "$arg" in
    --no-pack) DO_PACK=0 ;;
    --host) DO_HOST=1 ;;
    -h|--help)
      sed -n '2,14p' "$0" | sed 's/^# \?//'
      exit 0
      ;;
    *)
      echo "unknown arg: $arg" >&2
      exit 2
      ;;
  esac
done

command -v docker >/dev/null || { echo "need docker" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "docker daemon not running" >&2; exit 1; }

if [[ "$DO_PACK" -eq 1 ]]; then
  if [[ ! -x ./cc/bin/ccc && ! -x ./out/cc/bin/ccc ]]; then
    echo "missing ccc — build the toolchain first (make cc)" >&2
    exit 1
  fi
  command -v node >/dev/null || { echo "need node" >&2; exit 1; }
  command -v python3 >/dev/null || { echo "need python3" >&2; exit 1; }

  PY=python3
  if ! "$PY" -c 'import build' 2>/dev/null; then
    VENV="${TMPDIR:-/tmp}/cc-publish-venv"
    if [[ ! -x "$VENV/bin/python" ]]; then
      echo "== bootstrap $VENV (build)"
      python3 -m venv "$VENV"
      "$VENV/bin/pip" -q install -U pip build
    fi
    PY="$VENV/bin/python"
  fi

  echo "== pack concurrent-c-python"
  rm -f out/concurrent-c-python-*.tgz out/cc-python-*.tgz
  rm -rf npm/cc-python/bin npm/cc-python/vendor
  ./npm/cc-python/scripts/prepare-publish.sh
  NPM_TGZ="$(ls -1t out/concurrent-c-python-*.tgz | head -1)"

  echo "== pack concurrent-c-node"
  rm -rf out/pypi pypi/cc-node/dist pypi/cc-node/build pypi/cc-node/*.egg-info
  mkdir -p out/pypi
  "$PY" -m build --outdir out/pypi pypi/cc-node
else
  NPM_TGZ="$(ls -1t out/concurrent-c-python-*.tgz 2>/dev/null | head -1 || true)"
  [[ -n "$NPM_TGZ" ]] || { echo "no out/concurrent-c-python-*.tgz — drop --no-pack" >&2; exit 1; }
  ls out/pypi/concurrent_c_node-*.whl >/dev/null 2>&1 || {
    echo "no out/pypi/concurrent_c_node-*.whl — drop --no-pack" >&2
    exit 1
  }
fi

NPM_TGZ="$(cd "$(dirname "$NPM_TGZ")" && pwd)/$(basename "$NPM_TGZ")"
WHL="$(ls -1t out/pypi/concurrent_c_node-*-py3-none-any.whl | head -1)"
WHL="$(cd "$(dirname "$WHL")" && pwd)/$(basename "$WHL")"
SDIST="$(ls -1t out/pypi/concurrent_c_node-*.tar.gz | head -1)"
SDIST="$(cd "$(dirname "$SDIST")" && pwd)/$(basename "$SDIST")"

echo "== artifacts"
echo "   $NPM_TGZ"
echo "   $WHL"
echo "   $SDIST"

host_smoke() {
  local work
  work="$(mktemp -d "${TMPDIR:-/tmp}/cc-pack-host.XXXXXX")"
  echo "== host consumer smoke in $work"
  (
    cd "$work"
    npm init -y >/dev/null
    npm install "$NPM_TGZ"
    node --expose-gc -e '
(async () => {
  const ccpy = require("concurrent-c-python");
  const py = ccpy.create();
  if (py.import("math").sqrt(16) !== 4) throw new Error("inproc call");
  const iso = ccpy.create({ isolated: true });
  const math = await iso.import("math");
  if ((await math.sqrt(9)) !== 3) throw new Error("isolated call");
  await iso.destroy();
  py.destroy();
  console.log("host npm concurrent-c-python OK");
})().catch((e) => { console.error(e); process.exit(1); });
'
    python3 -m venv .venv
    .venv/bin/pip -q install -U pip
    .venv/bin/pip -q install "$WHL"
    .venv/bin/python - <<'PY'
import cc_node
js = cc_node.create()
assert js.eval("1+1") == 2
assert js.eval("Math.hypot(3,4)") == 5
assert js.eval("Promise.resolve(41)") == 41
js.destroy()
print("host pip concurrent-c-node OK")
PY
  )
  rm -rf "$work"
}

if [[ "$DO_HOST" -eq 1 ]]; then
  host_smoke || { echo "host consumer smoke FAILED" >&2; exit 1; }
fi

IMAGE="${CC_PACK_SMOKE_IMAGE:-node:22-bookworm}"
echo "== docker pull $IMAGE (if needed)"
docker pull "$IMAGE" >/dev/null

# Mount only the packed artifacts + a tiny in-container smoke script.
SMOKE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/cc-pack-smoke.XXXXXX")"
mkdir -p "$SMOKE_DIR/packs"
cp "$NPM_TGZ" "$WHL" "$SDIST" "$SMOKE_DIR/packs/"
cat >"$SMOKE_DIR/run.sh" <<'INNER'
#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
echo "== apt: python3 + build tools (cold install of cc-python compiles vendor C)"
apt-get update -qq
apt-get install -y -qq python3 python3-dev python3-venv python3-pip \
  build-essential >/dev/null

cd /work
mkdir -p npm-smoke pip-smoke
NPM_TGZ="$(ls -1 /packs/concurrent-c-python-*.tgz | head -1)"
WHL="$(ls -1 /packs/concurrent_c_node-*-py3-none-any.whl | head -1)"

echo "== npm install $NPM_TGZ (no repo on NODE_PATH)"
cd /work/npm-smoke
npm init -y >/dev/null
npm install "$NPM_TGZ"
node --expose-gc - <<'JS'
(async () => {
  const ccpy = require("concurrent-c-python");
  const py = ccpy.create();
  const math = py.import("math");
  if (math.sqrt(16) !== 4) throw new Error("sqrt");
  if (math.hypot(3, 4) !== 5) throw new Error("hypot");
  let threw = false;
  try { math.sqrt(-1); } catch (e) {
    threw = /domain|nonnegative/i.test(String(e && e.message));
  }
  if (!threw) throw new Error("python error did not cross");
  const iso = ccpy.create({ isolated: true });
  const imath = await iso.import("math");
  if ((await imath.sqrt(9)) !== 3) throw new Error("isolated");
  await iso.destroy();
  py.destroy();
  let closed = false;
  try { py.import("math"); } catch (e) {
    closed = /closed/i.test(String(e && e.message));
  }
  if (!closed) throw new Error("destroy did not close");
  console.log("RESULT npm_concurrent_c_python OK");
})().catch((e) => { console.error(e); process.exit(1); });
JS

echo "== pip install $WHL into venv (no PYTHONPATH)"
cd /work/pip-smoke
python3 -m venv .venv
.venv/bin/pip -q install -U pip
.venv/bin/pip -q install "$WHL"
.venv/bin/python - <<'PY'
import cc_node

js = cc_node.create()
assert js.eval("1 + 1") == 2
assert js.eval("Math.hypot(3, 4)") == 5
# thenable await in child
assert js.eval("Promise.resolve(41)") == 41
_ = js.require  # attribute present
js.destroy()
try:
    js.eval("1")
except Exception as e:
    assert "closed" in str(e).lower() or "destroy" in str(e).lower() or "exit" in str(e).lower(), e
else:
    raise SystemExit("destroy did not close")

# second domain + cross-check
a = cc_node.create()
b = cc_node.create()
assert a.eval("7") == 7
assert b.eval("8") == 8
a.destroy()
b.destroy()
print("RESULT pip_concurrent_c_node OK")
PY

echo "== summary"
echo "RESULT pack_smoke_clean true"
INNER
chmod +x "$SMOKE_DIR/run.sh"

echo "== docker run (fresh $IMAGE, packs only)"
docker run --rm \
  -v "$SMOKE_DIR/packs:/packs:ro" \
  -v "$SMOKE_DIR/run.sh:/run_smoke.sh:ro" \
  -w /work \
  "$IMAGE" \
  bash /run_smoke.sh

rm -rf "$SMOKE_DIR"
echo "== smoke_bridge_packs: OK"
echo "   ready to upload with: ./scripts/publish_bridges.sh --publish --no-bump"
