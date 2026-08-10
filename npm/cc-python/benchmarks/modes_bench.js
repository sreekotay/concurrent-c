#!/usr/bin/env node
/**
 * Mode costs on this machine. RESULT lines are machine-comparable.
 *
 *   node npm/cc-python/benchmarks/modes_bench.js
 *
 * In-process uses direct sync calls (not py.task). Isolated awaits.
 * Needs numpy visible to ambient python3 for the isolated rows.
 */
'use strict';

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

(async () => {
  // --- sync in-process: 1k cheap math calls -----------------------------
  {
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

  // --- isolated: same 1k cheap calls (wire dominates) -------------------
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

  // --- isolated numpy dot vs JS, several sizes --------------------------
  {
    const py = ccpy.create({ isolated: true });
    const np = py.import('numpy');
    for (const n of [1e3, 1e4, 1e5, 1e6]) {
      const [a, b] = fill(n);
      await np.dot(a, b);
      jsDot(a, b);
      const t0 = performance.now();
      await np.dot(a, b);
      const npMs = performance.now() - t0;
      const t1 = performance.now();
      jsDot(a, b);
      const jsMs = performance.now() - t1;
      console.log('RESULT iso_dot_n', n,
                  'numpy_ms', npMs.toFixed(3),
                  'js_ms', jsMs.toFixed(3),
                  'winner', npMs < jsMs ? 'numpy' : 'js');
    }
    await py.destroy();
  }

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
