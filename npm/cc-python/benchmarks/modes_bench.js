#!/usr/bin/env node
/**
 * Mode costs on this machine. RESULT lines are machine-comparable.
 *
 *   VIRTUAL_ENV=/path/to/venv node npm/cc-python/benchmarks/modes_bench.js
 *
 * Workloads:
 *   - cheap sqrt (crossing cost)
 *   - np.dot (BLAS-1; often too light to beat JS once isolated)
 *   - matmul nxn (BLAS-3; kernel should dominate)
 *   - SVD nxn (heavy; inproc vs iso only — no honest pure-JS peer)
 *
 * Matmul/SVD return a scalar checksum so isolated is not punished for
 * shipping an n² result matrix back over the wire.
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

/** Naive row-major matmul; returns sum(C) so shape matches the Python helpers. */
function jsMatmulSum(A, B, n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    for (let j = 0; j < n; j++) {
      let s = 0;
      for (let k = 0; k < n; k++) s += A[i * n + k] * B[k * n + j];
      sum += s;
    }
  }
  return sum;
}

function fillVec(n) {
  const a = new Float64Array(n);
  const b = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    a[i] = (i % 97) * 0.01;
    b[i] = (i % 89) * 0.01;
  }
  return [a, b];
}

function fillMat(n) {
  const A = new Float64Array(n * n);
  const B = new Float64Array(n * n);
  for (let i = 0; i < A.length; i++) {
    A[i] = ((i * 13) % 97) * 0.01;
    B[i] = ((i * 17) % 89) * 0.01;
  }
  return [A, B];
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

function pickWinner(times) {
  let winner = null;
  let best = Infinity;
  for (const [k, v] of Object.entries(times)) {
    if (v != null && v < best) { best = v; winner = k; }
  }
  return winner;
}

const HELPERS = `
import numpy as np

def matmul_sum(a, b, n):
    n = int(n)
    A = np.asarray(a, dtype=np.float64).reshape(n, n)
    B = np.asarray(b, dtype=np.float64).reshape(n, n)
    return float(np.sum(A @ B))

def svd_s0(a, n):
    n = int(n)
    A = np.asarray(a, dtype=np.float64).reshape(n, n)
    return float(np.linalg.svd(A, compute_uv=False)[0])
`;

function loadHelpers(py) {
  const b = py.import('builtins');
  const g = b.dict();
  b.exec(HELPERS, g);
  return {
    matmul_sum: g.get('matmul_sum'),
    svd_s0: g.get('svd_s0'),
  };
}

(async () => {
  const venv = pickVenv();
  if (venv) {
    ccpy.usePython(venv);
    console.log('RESULT inproc_python', venv);
  } else {
    console.log('RESULT inproc_python', 'ambient');
  }

  let inprocNp = null;
  let inprocPy = null;
  let inHelpers = null;
  try {
    inprocPy = ccpy.create();
    inprocNp = inprocPy.import('numpy');
    inprocNp.dot(new Float64Array([1, 2]), new Float64Array([3, 4]));
    inHelpers = loadHelpers(inprocPy);
    console.log('RESULT inproc_numpy', 'yes');
  } catch (e) {
    console.log('RESULT inproc_numpy', 'no',
                String(e.message || e).split('\n')[0].slice(0, 80));
    if (inprocPy) {
      try { await inprocPy.destroy(); } catch (_) { /* ignore */ }
    }
    inprocPy = null;
    inprocNp = null;
    inHelpers = null;
  }

  // --- cheap math -------------------------------------------------------
  {
    const py = inprocPy || ccpy.create();
    const owned = !inprocPy;
    const math = py.import('math');
    math.sqrt(1);
    const t0 = performance.now();
    for (let i = 0; i < 1000; i++) math.sqrt(i + 1);
    const ms = performance.now() - t0;
    console.log('RESULT inproc_sqrt_us', ((ms / 1000) * 1000).toFixed(2));
    if (owned) await py.destroy();
  }
  {
    const py = ccpy.create({ isolated: true });
    const math = py.import('math');
    math.sqrt(1);
    const t0 = performance.now();
    for (let i = 0; i < 1000; i++) math.sqrt(i + 1);
    const ms = performance.now() - t0;
    console.log('RESULT iso_sqrt_us', ((ms / 1000) * 1000).toFixed(2));
    await py.destroy();
  }

  const iso = ccpy.create({ isolated: true });
  const isoNp = iso.import('numpy');
  const isoHelpers = loadHelpers(iso);

  // --- BLAS-1: dot ------------------------------------------------------
  for (const n of [1e4, 1e5, 1e6]) {
    const [a, b] = fillVec(n);
    if (inprocNp) inprocNp.dot(a, b);
    isoNp.dot(a, b);
    jsDot(a, b);

    let inMs = null;
    if (inprocNp) {
      const t0 = performance.now();
      inprocNp.dot(a, b);
      inMs = performance.now() - t0;
    }
    const t1 = performance.now();
    isoNp.dot(a, b);
    const isoMs = performance.now() - t1;
    const t2 = performance.now();
    jsDot(a, b);
    const jsMs = performance.now() - t2;

    console.log(
      'RESULT dot_n', n,
      'inproc_ms', inMs == null ? 'NA' : inMs.toFixed(3),
      'iso_ms', isoMs.toFixed(3),
      'js_ms', jsMs.toFixed(3),
      'winner', pickWinner({ inproc: inMs, iso: isoMs, js: jsMs }));
  }

  // --- BLAS-3: matmul (checksum) ----------------------------------------
  for (const n of [32, 64, 128, 256, 512]) {
    const [A, B] = fillMat(n);
    if (inHelpers) inHelpers.matmul_sum(A, B, n);
    isoHelpers.matmul_sum(A, B, n);
    if (n <= 256) jsMatmulSum(A, B, n); // 512³ naive JS is minutes

    let inMs = null;
    if (inHelpers) {
      const t0 = performance.now();
      inHelpers.matmul_sum(A, B, n);
      inMs = performance.now() - t0;
    }
    const t1 = performance.now();
    isoHelpers.matmul_sum(A, B, n);
    const isoMs = performance.now() - t1;

    let jsMs = null;
    if (n <= 256) {
      const t2 = performance.now();
      jsMatmulSum(A, B, n);
      jsMs = performance.now() - t2;
    }

    console.log(
      'RESULT matmul_n', n,
      'inproc_ms', inMs == null ? 'NA' : inMs.toFixed(3),
      'iso_ms', isoMs.toFixed(3),
      'js_ms', jsMs == null ? 'NA' : jsMs.toFixed(3),
      'winner', pickWinner({ inproc: inMs, iso: isoMs, js: jsMs }));
  }

  // --- heavy: SVD singular-value[0] -------------------------------------
  for (const n of [64, 128, 256]) {
    const [A] = fillMat(n);
    if (inHelpers) inHelpers.svd_s0(A, n);
    isoHelpers.svd_s0(A, n);

    let inMs = null;
    if (inHelpers) {
      const t0 = performance.now();
      inHelpers.svd_s0(A, n);
      inMs = performance.now() - t0;
    }
    const t1 = performance.now();
    isoHelpers.svd_s0(A, n);
    const isoMs = performance.now() - t1;

    console.log(
      'RESULT svd_n', n,
      'inproc_ms', inMs == null ? 'NA' : inMs.toFixed(3),
      'iso_ms', isoMs.toFixed(3),
      'winner', pickWinner({ inproc: inMs, iso: isoMs }));
  }

  await iso.destroy();
  if (inprocPy) await inprocPy.destroy();

  // --- N isolated domains -----------------------------------------------
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
      const g = b.dict();
      b.exec(work, g);
      domains.push(py);
      fns.push(py.task(g.get('work')));
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
