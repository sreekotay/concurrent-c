#!/bin/sh
# TMPDIR already ending in / must not make T//cc-script, and the clean
# lowerer's --root for a script must be the checkout, not that cache.
set -e
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
rm -rf /tmp/cc-script-path-smoke
mkdir -p /tmp/cc-script-path-smoke
log=/tmp/cc-script-path-smoke.log
cd "$root"
TMPDIR=/tmp/cc-script-path-smoke/ ./cc/bin/ccc --verbose --no-cache tests/script_cache_probe.shcc >"$log" 2>&1
status=$?
if grep -q '//cc-script' "$log"; then
  echo "script cache path joined a trailing slash: //cc-script" >&2
  exit 1
fi
py='import re,sys; t=open(sys.argv[1]).read(); m=re.search(r"--root (\S+)", t); print(m.group(1) if m else "")'
got=$(python3 -c "$py" "$log")
case "$got" in
  "")
    echo "clean lowerer was not invoked" >&2
    exit 1
    ;;
  *cc-script-*)
    echo "clean lowerer --root is the script cache ($got), not the project" >&2
    exit 1
    ;;
esac
if [ "$status" -ne 0 ]; then
  echo "probe script failed ($status)" >&2
  exit 1
fi
exit 0
