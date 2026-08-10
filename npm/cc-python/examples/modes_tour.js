#!/usr/bin/env node
/**
 * In-process vs isolated — patterns that match the current bridge.
 *
 *   node npm/cc-python/examples/modes_tour.js
 *
 * In-process: sync, ~µs crossings, packages only if that libpython has them
 * (usePython(venv) / a venv-built runtime). Isolated: child CPython, await
 * every call, ambient/system packages, same-domain handles chain.
 */
'use strict';

const ccpy = require('..');

function banner(t) {
  console.log('\n== ' + t);
}

(async () => {
  // ---- in-process: sync, no py.task for cheap calls --------------------
  banner('in-process (sync)');
  const py = ccpy.create();
  const math = py.import('math');
  const builtins = py.import('builtins');
  console.log('math.sqrt(16) =', math.sqrt(16));

  const ns = builtins.dict();
  builtins.exec(`
import json
def analyze(xs):
    # typed arrays cross as memoryviews; plain JS arrays do not (in-process)
    xs = list(xs)
    return json.dumps({"n": len(xs), "sum": sum(xs), "mean": sum(xs) / len(xs)})
`, ns);
  console.log('analyze(Float64Array) =',
              ns.get('analyze')(new Float64Array([1, 2, 3, 4, 5])));

  // Lane when you want the event loop free during longer work:
  const slow = py.task(math.factorial);
  console.log('task(factorial)(10) =', await slow(10));
  await py.destroy();

  // ---- isolated: await calls; empty dict stays a live handle -----------
  banner('isolated (async, system/ambient python)');
  const iso = ccpy.create({ isolated: true });
  const np = iso.import('numpy');
  const b = iso.import('builtins');

  const a = new Float64Array([1, 2, 3, 4]);
  console.log('np.sum =', await np.sum(a));

  const fft = await np.fft.fft(a);
  const mag = await np.abs(fft); // same-domain handle chain
  console.log('fft→abs length =',
              typeof mag.length === 'number' ? mag.length
              : (await mag.toTypedArray()).length);

  const g = await b.dict();
  await b.exec(`
def peak_hz(signal, rate):
    import numpy as np
    m = np.abs(np.fft.fft(signal))
    i = int(np.argmax(m[: len(m) // 2]))
    return float(i * rate / len(signal))
`, g);
  const peak = await g.get('peak_hz');
  const tone = new Float64Array(4410);
  for (let i = 0; i < tone.length; i++)
    tone[i] = Math.sin(2 * Math.PI * 440 * i / 44100);
  console.log('peak_hz ≈', (await peak(tone, 44100)).toFixed(1), '(want ~440)');

  await iso.destroy();
  console.log('\nok');
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
