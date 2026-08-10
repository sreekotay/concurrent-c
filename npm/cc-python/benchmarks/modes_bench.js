#!/usr/bin/env node
/**
 * Mode costs on this machine. RESULT lines are machine-comparable.
 *
 *   node npm/cc-python/benchmarks/modes_bench.js
 *
 * Dot rows time in-process (sync), isolated (await), and a pure JS loop.
 * Point in-process at a numpy-capable runtime first, e.g.:
 *   VIRTUAL_ENV=/path/to/venv node …/modes_bench.js
 *   # or CC_PYTHON_VENV=/path/to/venv
 */
'use strict';

const fs = require('fs');
const path = require('path');
const ccpy = require('..');

function jsDot(a, b) {
  let s = 0;
  for (let i = 0; i < a.length; i++) s += a[i] * b[i];
  return s;
}

function fill(n) {
  const a = new Float64Array(n);
  const b = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    a[i] = (i % 97) * 0.01;
    b[i] = (i % 89) * 0.01;
  }
  return [a, b];
}

function pickVenv() {
  const cand = [
    process.env.CC_PYTHON_VENV,
    process.env.VIRTUAL_ENV,
    path.resolve('.venv'),
  ].filter(Boolean);
  for (const d of cand) {
    if (fs.existsSync(path.join(d, 'pyvenv.cfg'))) return d;
  }
  return null;
}

(async () => {
  // usePython must land before the first in-process create().
  const venv = pickVenv();
  if (venv) {
    ccpy.usePython(venv);
    console.log('RESULT inproc_python', venv);
  } else {
    console.log('RESULT inproc_python', 'ambient');
  }

  let inprocNp = null;
  let inprocPy = null;
  try {
    inprocPy = ccpy.create();
    inprocNp = inprocPy.import('numpy');
    // touch once so a missing module fails here
    inprocNp.dot(new Float64Array([1, 2]), new Float64Array([3, 4]));
    console.log('RESULT inproc_numpy', 'yes');
  } catch (e) {
    console.log('RESULT inproc_numpy', 'no',
                String(e.message || e).split('\n')[0].slice(0, 80));
    if (inprocPy) {
      try { await inprocPy.destroy(); } catch (_) { /* ignore */ }
    }
    inprocPy = null;
    inprocNp = null;
  }

  // --- cheap math: in-process sync vs isolated await --------------------
  if (inprocPy) {
    const math = inprocPy.import('math');
    math.sqrt(1);
    const t0 = performance.now();
    for (let i = 0; i < 1000; i++) math.sqrt(i + 1);
    const ms = performance.now() - t0;
    console.log('RESULT inproc_sqrt_1000_ms', ms.toFixed(2));
    console.log('RESULT inproc_sqrt_us', ((ms / 1000) * 1000).toFixed(2));
  } else {
    const py = ccpy.create();
    const math = py.import('math');
    math.sqrt(1);
    const t0 = performance.now();
    for (let i = 0; i < 1000; i++) math.sqrt(i + 1);
    const ms = performance.now() - t0;
    console.log('RESULT inproc_sqrt_1000_ms', ms.toFixed(2));
    console.log('RESULT inproc_sqrt_us', ((ms / 1000) * 1000).toFixed(2));
    await py.destroy();
  }

  {
    const py = ccpy.create({ isolated: true });
    const math = py.import('math');
    await math.sqrt(1);
    const t0 = performance.now();
    for (let i = 0; i < 1000; i++) await math.sqrt(i + 1);
    const ms = performance.now() - t0;
    console.log('RESULT iso_sqrt_1000_ms', ms.toFixed(2));
    console.log('RESULT iso_sqrt_us', ((ms / 1000) * 1000).toFixed(2));
    await py.destroy();
  }

  // --- np.dot: in-process vs isolated vs JS -----------------------------
  {
    const iso = ccpy.create({ isolated: true });
    const isoNp = iso.import('numpy');
    for (const n of [1e3, 1e4, 1e5, 1e6]) {
      const [a, b] = fill(n);
      jsDot(a, b);
      await isoNp.dot(a, b);
      if (inprocNp) inprocNp.dot(a, b);

      let inMs = null;
      if (inprocNp) {
        const t0 = performance.now();
        inprocNp.dot(a, b);
        inMs = performance.now() - t0;
      }

      const t1 = performance.now();
      await isoNp.dot(a, b);
      const isoMs = performance.now() - t1;

      const t2 = performance.now();
      jsDot(a, b);
      const jsMs = performance.now() - t2;

      const times = { js: jsMs, iso: isoMs };
      if (inMs !== null) times.inproc = inMs;
      let winner = 'js';
      let best = jsMs;
      if (inMs !== null && inMs < best) { best = inMs; winner = 'inproc'; }
      if (isoMs < best) { best = isoMs; winner = 'iso'; }

      console.log(
        'RESULT dot_n', n,
        'inproc_ms', inMs === null ? 'NA' : inMs.toFixed(3),
        'iso_ms', isoMs.toFixed(3),
        'js_ms', jsMs.toFixed(3),
        'winner', winner);
    }
    await iso.destroy();
  }

  if (inprocPy) await inprocPy.destroy();

  // --- N isolated domains: overlap via Promise.all ----------------------
  {
    const N = 3;
    const iters = 8e6;
    const work = `
def work(n):
    s = 0.0
    for i in range(n):
        s += (i * i) % 7
    return s
`;
    const domains = [];
    const fns = [];
    for (let i = 0; i < N; i++) {
      const py = ccpy.create({ isolated: true });
      const b = py.import('builtins');
      const g = await b.dict();
      await b.exec(work, g);
      domains.push(py);
      fns.push(await g.get('work'));
    }
    for (const f of fns) await f(iters / 10);
    const tSeq0 = performance.now();
    for (const f of fns) await f(iters);
    const seqMs = performance.now() - tSeq0;
    const tPar0 = performance.now();
    await Promise.all(fns.map((f) => f(iters)));
    const parMs = performance.now() - tPar0;
    console.log('RESULT iso_domains', N,
                'seq_ms', seqMs.toFixed(1),
                'par_ms', parMs.toFixed(1),
                'speedup', (seqMs / parMs).toFixed(2));
    for (const py of domains) await py.destroy();
  }

  console.log('done');
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
