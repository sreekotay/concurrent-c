#!/bin/sh
# Assemble the publishable concurrent-c-python package: prebuilt addon for THIS
# platform + vendored lowered-C sources for every other one.
#
#   npm/cc-python/scripts/prepare-publish.sh          (from the repo root)
#
# The CC toolchain runs HERE, at publish time; installers need only a C
# compiler (or nothing, on a platform with a shipped prebuilt).  Run once
# per platform you want prebuilt, collecting bin/ artifacts before
# `npm publish`.  bin/ and vendor/ are regenerate-only products —
# .gitignore'd, never committed.
set -e
cd "$(dirname "$0")/../../.."   # repo root
PKG=npm/cc-python
PLAT="$(node -e 'console.log(process.platform + "-" + process.arch)')"

echo "== addon (prebuilt for $PLAT)"
mkdir -p $PKG/bin
./cc/bin/ccc build $PKG/src/cc_python.ccs -o $PKG/bin/cc_python-$PLAT.node

echo "== lowered C"
mkdir -p $PKG/vendor
rm -f $PKG/vendor/cc_python.c
./cc/bin/ccc build --emit-c-only $PKG/src/cc_python.ccs -o $PKG/vendor/cc_python.c

echo "== vendored headers + runtime (symlinks dereferenced)"
rm -rf $PKG/vendor/include $PKG/vendor/runtime
cp -rL out/include $PKG/vendor/include
cp -rL out/runtime $PKG/vendor/runtime
# ccc/vendor (third_party headers behind symlinks) ships in cc/include,
# not the lowered tree — the runtime includes it by <ccc/vendor/...>.
cp -rL cc/include/ccc/vendor $PKG/vendor/include/ccc/vendor

echo "== fallback build proves out (compile the vendored sources cold)"
TMP="$(mktemp -d)"
cc -O2 -fPIC -ffunction-sections -fdata-sections -I$PKG/vendor/include \
   -c $PKG/vendor/cc_python.c -o "$TMP/tu.o"
cc -O2 -fPIC -DCC_ENABLE_ASYNC -ffunction-sections -fdata-sections \
   -I$PKG/vendor/include -c $PKG/vendor/runtime/concurrent_c.c -o "$TMP/rt.o"
# Darwin and ELF take different export-map shapes (same as install.js).
if [ "$(uname -s)" = Darwin ]; then
  printf '_napi_register_module_v1\n' > "$TMP/exports.ver"
  LINK_EXTRA="-Wl,-dead_strip -Wl,-exported_symbols_list,$TMP/exports.ver"
else
  printf '{ global: napi_register_module_v1; local: *; };\n' > "$TMP/exports.ver"
  LINK_EXTRA="-Wl,--gc-sections -Wl,--version-script=$TMP/exports.ver"
fi
# shellcheck disable=SC2086
cc "$TMP/tu.o" "$TMP/rt.o" -shared -o "$TMP/probe.node" -lpthread -lm -ldl \
   $LINK_EXTRA
CC_PYTHON_ADDON="$TMP/probe.node" node -e '
const py = require(process.cwd() + "/npm/cc-python").create();
if (py.import("math").sqrt(16) !== 4) throw new Error("probe failed");
py.destroy();
console.log("   fallback addon works");'
rm -rf "$TMP"

echo "== pack"
cd $PKG && npm pack --pack-destination ../../out 2>/dev/null | tail -1 | \
  sed 's|^|   out/|'
