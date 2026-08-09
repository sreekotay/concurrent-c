/* cc-python postinstall: make sure an addon exists for this platform.
 *
 * 1. A shipped prebuilt (bin/cc_python-<platform>-<arch>.node) wins —
 *    N-API is a stable ABI, so one binary serves every node version.
 * 2. Otherwise compile from the vendored LOWERED C: plain C99 + any C
 *    compiler.  No node headers (the addon declares its napi surface),
 *    no Python headers (libpython is dlopen'd at runtime), no network.
 * 3. CC_PYTHON_SKIP_BUILD=1 skips (CI installs that never run Python).
 *
 * Failure is loud: an install that produces no addon and wasn't told to
 * skip exits nonzero with the reason. */
'use strict';

const { spawnSync } = require('child_process');
const path = require('path');
const fs = require('fs');

const plat = process.platform + '-' + process.arch;
const binDir = path.join(__dirname, 'bin');
const target = path.join(binDir, 'cc_python-' + plat + '.node');

if (fs.existsSync(target)) {
  console.log('cc-python: using prebuilt addon for ' + plat);
  process.exit(0);
}
if (process.env.CC_PYTHON_SKIP_BUILD === '1') {
  console.log('cc-python: CC_PYTHON_SKIP_BUILD=1 — no addon installed');
  process.exit(0);
}

const vendor = path.join(__dirname, 'vendor');
const tuC = path.join(vendor, 'cc_python.c');
const rtC = path.join(vendor, 'runtime', 'concurrent_c.c');
const inc = path.join(vendor, 'include');
if (!fs.existsSync(tuC) || !fs.existsSync(rtC)) {
  console.error('cc-python: no prebuilt for ' + plat +
                ' and no vendored sources in this package.');
  process.exit(1);
}

function pickCompiler() {
  const names = process.env.CC ? [process.env.CC] : ['cc', 'clang', 'gcc'];
  for (const n of names) {
    const r = spawnSync(n, ['--version'], { stdio: 'ignore' });
    if (r.status === 0) return n;
  }
  return null;
}

const cc = pickCompiler();
if (!cc) {
  console.error('cc-python: no C compiler found (set CC, or install ' +
                'cc/clang/gcc) — cannot build the addon for ' + plat);
  process.exit(1);
}

fs.mkdirSync(binDir, { recursive: true });
const isDarwin = process.platform === 'darwin';

// Only the module entry is anyone's ABI; exporting just it lets the
// linker dead-strip the rest (~5x smaller).
const exportsFile = path.join(binDir, 'exports-' + plat + '.txt');
fs.writeFileSync(exportsFile, isDarwin
    ? '_napi_register_module_v1\n'
    : '{ global: napi_register_module_v1; local: *; };\n');

const common = ['-O2', '-fPIC', '-ffunction-sections', '-fdata-sections',
                '-I' + inc];
const tuO = path.join(binDir, 'tu-' + plat + '.o');
const rtO = path.join(binDir, 'rt-' + plat + '.o');

console.log('cc-python: building addon for ' + plat + ' with ' + cc + ' ...');
function run(label, args) {
  const r = spawnSync(cc, args, { stdio: 'inherit' });
  if (r.status !== 0) {
    console.error('cc-python: ' + label + ' failed (compiler exit ' +
                  r.status + '). Set CC_PYTHON_ADDON to a prebuilt addon, ' +
                  'or CC_PYTHON_SKIP_BUILD=1 to install without one.');
    process.exit(1);
  }
}
/* Same arrangement as the repo build: the module TU compiles without
 * CC_ENABLE_ASYNC, the runtime with it. */
run('module compile', common.concat(['-c', tuC, '-o', tuO]));
run('runtime compile', common.concat(['-DCC_ENABLE_ASYNC', '-c', rtC, '-o', rtO]));
run('link', [tuO, rtO, '-shared', '-o', target, '-lpthread', '-lm', '-ldl']
    .concat(isDarwin
      ? ['-Wl,-dead_strip', '-Wl,-exported_symbols_list,' + exportsFile]
      : ['-Wl,--gc-sections', '-Wl,--version-script=' + exportsFile]));
if (!fs.existsSync(target)) {
  console.error('cc-python: source build produced no addon.');
  process.exit(1);
}
for (const f of [exportsFile, tuO, rtO]) {
  try { fs.unlinkSync(f); } catch (e) { /* cosmetic */ }
}
console.log('cc-python: built ' + path.relative(__dirname, target) +
            ' (' + fs.statSync(target).size + ' bytes)');
