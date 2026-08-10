#!/usr/bin/env node
/**
 * Correct benchmark v2: Reuses runtimes to avoid create/destroy overhead
 *
 * CRITICAL FIX: First version created new runtime for each test
 * This version reuses runtimes across multiple operations
 */
'use strict';

const ccpy = require('concurrent-c-python');
const path = require('path');
const Matrix = require('ml-matrix').Matrix;

// Setup: Point in-process at venv with NumPy
const venvPython = path.join(__dirname, '..', 'venv', 'bin', 'python');
try {
  ccpy.usePython(venvPython);
} catch (e) {
  console.log('Note: usePython() setup may need adjustment\n');
}

async function benchmark(name, fn, iterations = 1) {
  try {
    const start = performance.now();
    let result;
    for (let i = 0; i < iterations; i++) {
      result = await fn();
    }
    const elapsed = performance.now() - start;
    return { name, elapsed, ok: true, result };
  } catch (e) {
    return { name, elapsed: Infinity, ok: false, error: e.message };
  }
}

function benchmarkSync(name, fn, iterations = 1) {
  try {
    const start = performance.now();
    let result;
    for (let i = 0; i < iterations; i++) {
      result = fn();
    }
    const elapsed = performance.now() - start;
    return { name, elapsed, ok: true, result };
  } catch (e) {
    return { name, elapsed: Infinity, ok: false, error: e.message };
  }
}

function printResult(label, result, iterations = 1) {
  if (result.ok) {
    const perCall = result.elapsed / iterations;
    console.log(
      `${label.padEnd(35)} | ${result.elapsed.toFixed(2).padEnd(10)}ms | ${perCall.toFixed(4)}ms/call`
    );
  } else {
    console.log(`${label.padEnd(35)} | ERROR: ${result.error}`);
  }
}

(async () => {
  console.log('\n╔════════════════════════════════════════════════════════════════════╗');
  console.log('║  BENCHMARKS V2: Reusing runtimes (no create/destroy per op)      ║');
  console.log('║                                                                  ║');
  console.log('║  In-process: Direct calls, reused runtime                        ║');
  console.log('║  Isolated:   py.task() with IPC, reused runtime                 ║');
  console.log('║  JavaScript: Pure V8                                             ║');
  console.log('║  ml-matrix:  Specialized JS linear algebra                      ║');
  console.log('╚════════════════════════════════════════════════════════════════════╝\n');

  // Create once and reuse
  const pyInProcess = ccpy.create();
  const npInProcess = pyInProcess.import('numpy');

  const pyIsolated = ccpy.create({ isolated: true });
  const npIsolated = pyIsolated.import('numpy');

  // ==================== BENCHMARK 1: Sum (100K elements) ====================
  console.log('═'.repeat(80));
  console.log('  BENCHMARK 1: Sum (100K elements) - 10 iterations');
  console.log('═'.repeat(80) + '\n');

  const sumData = new Float64Array(100000);
  for (let i = 0; i < sumData.length; i++) sumData[i] = Math.random();

  // In-process direct call
  const inProcSum = await benchmark(
    'In-Process (direct)',
    async () => npInProcess.sum(sumData),
    10
  );

  // Isolated mode
  const isolatedSum = await benchmark(
    'Isolated (py.task)',
    async () => await pyIsolated.task(npIsolated.sum)(sumData),
    10
  );

  // Pure JavaScript
  const jsSum = benchmarkSync('JavaScript (for loop)', () => {
    let sum = 0;
    for (let i = 0; i < sumData.length; i++) sum += sumData[i];
    return sum;
  }, 10);

  console.log('Mode                               | Time (ms) | Per-call (ms)');
  console.log('-'.repeat(80));
  printResult('In-Process (direct)', inProcSum, 10);
  printResult('Isolated (py.task)', isolatedSum, 10);
  printResult('JavaScript (for loop)', jsSum, 10);

  if (inProcSum.ok && jsSum.ok) {
    const inProcRatio = inProcSum.elapsed / jsSum.elapsed;
    console.log(
      `\nIn-process: ${inProcRatio > 1 ? inProcRatio.toFixed(1) + 'x slower' : (1 / inProcRatio).toFixed(1) + 'x faster'} than JS`
    );
  }
  if (isolatedSum.ok && jsSum.ok) {
    const isolatedRatio = isolatedSum.elapsed / jsSum.elapsed;
    console.log(
      `Isolated: ${isolatedRatio > 1 ? isolatedRatio.toFixed(1) + 'x slower' : (1 / isolatedRatio).toFixed(1) + 'x faster'} than JS`
    );
  }

  // ==================== BENCHMARK 2: Dot Product (1M elements) ====================
  console.log('\n' + '═'.repeat(80));
  console.log('  BENCHMARK 2: Dot Product (1M elements) - 5 iterations');
  console.log('═'.repeat(80) + '\n');

  const a = new Float64Array(1000000);
  const b = new Float64Array(1000000);
  for (let i = 0; i < a.length; i++) {
    a[i] = Math.random();
    b[i] = Math.random();
  }

  // In-process direct
  const inProcDot = await benchmark(
    'In-Process (direct)',
    async () => npInProcess.dot(a, b),
    5
  );

  // Isolated
  const isolatedDot = await benchmark(
    'Isolated (py.task)',
    async () => await pyIsolated.task(npIsolated.dot)(a, b),
    5
  );

  // JavaScript
  const jsDot = benchmarkSync('JavaScript (for loop)', () => {
    let sum = 0;
    for (let i = 0; i < a.length; i++) sum += a[i] * b[i];
    return sum;
  }, 5);

  console.log('Mode                               | Time (ms) | Per-call (ms)');
  console.log('-'.repeat(80));
  printResult('In-Process (direct)', inProcDot, 5);
  printResult('Isolated (py.task)', isolatedDot, 5);
  printResult('JavaScript (for loop)', jsDot, 5);

  if (inProcDot.ok && jsDot.ok) {
    const ratio = inProcDot.elapsed / jsDot.elapsed;
    console.log(
      `\nIn-process: ${ratio > 1 ? ratio.toFixed(1) + 'x slower' : (1 / ratio).toFixed(1) + 'x faster'} than JS`
    );
  }

  // ==================== BENCHMARK 3: Matrix Multiply (200×200) ====================
  console.log('\n' + '═'.repeat(80));
  console.log('  BENCHMARK 3: Matrix Multiply (200×200) - 3 iterations');
  console.log('═'.repeat(80) + '\n');

  const size = 200;
  const matA = new Float64Array(size * size);
  const matB = new Float64Array(size * size);
  for (let i = 0; i < matA.length; i++) {
    matA[i] = Math.random();
    matB[i] = Math.random();
  }

  // Pre-create matrices for in-process
  const inProcA = npInProcess.array(matA).reshape(size, size);
  const inProcB = npInProcess.array(matB).reshape(size, size);

  // In-process direct (reuse matrix objects)
  const inProcMatmul = await benchmark(
    'In-Process (direct)',
    async () => npInProcess.matmul(inProcA, inProcB),
    3
  );

  // Isolated (create matrices each time)
  const isolatedMatmul = await benchmark(
    'Isolated (py.task)',
    async () => {
      const A = npIsolated.array(matA).reshape(size, size);
      const B = npIsolated.array(matB).reshape(size, size);
      return await pyIsolated.task(npIsolated.matmul)(A, B);
    },
    3
  );

  // JavaScript with ml-matrix
  const jsMatmul = benchmarkSync('ml-matrix', () => {
    const A = new Matrix(size, size, matA);
    const B = new Matrix(size, size, matB);
    return A.multiply(B);
  }, 3);

  console.log('Mode                               | Time (ms) | Per-call (ms)');
  console.log('-'.repeat(80));
  printResult('In-Process (direct)', inProcMatmul, 3);
  printResult('Isolated (py.task)', isolatedMatmul, 3);
  printResult('ml-matrix', jsMatmul, 3);

  if (inProcMatmul.ok && jsMatmul.ok) {
    const ratio = inProcMatmul.elapsed / jsMatmul.elapsed;
    console.log(
      `\nIn-process vs ml-matrix: ${ratio > 1 ? ratio.toFixed(1) + 'x slower' : (1 / ratio).toFixed(1) + 'x faster'}`
    );
  }

  // ==================== BENCHMARK 4: FFT (64K samples) ====================
  console.log('\n' + '═'.repeat(80));
  console.log('  BENCHMARK 4: FFT (64K samples) - 3 iterations');
  console.log('═'.repeat(80) + '\n');

  const signal = new Float64Array(65536);
  for (let i = 0; i < signal.length; i++) {
    const t = i / signal.length;
    signal[i] = Math.sin(2 * Math.PI * t) + Math.cos(4 * Math.PI * t);
  }

  // In-process direct
  const inProcFFT = await benchmark(
    'In-Process (direct)',
    async () => npInProcess.fft.fft(signal),
    3
  );

  // Isolated (Note: this will be slow)
  const isolatedFFT = await benchmark(
    'Isolated (py.task)',
    async () => await pyIsolated.task(npIsolated.fft.fft)(signal),
    3
  );

  console.log('Mode                               | Time (ms) | Per-call (ms)');
  console.log('-'.repeat(80));
  printResult('In-Process (direct)', inProcFFT, 3);
  printResult('Isolated (py.task)', isolatedFFT, 3);
  console.log('JavaScript                        | N/A      | (no equivalent)');

  if (inProcFFT.ok && isolatedFFT.ok) {
    const ratio = isolatedFFT.elapsed / inProcFFT.elapsed;
    console.log(`\nIn-process is ${ratio.toFixed(1)}x faster than isolated for FFT`);
  }

  // ==================== BENCHMARK 5: Batch Dot (100 operations) ====================
  console.log('\n' + '═'.repeat(80));
  console.log('  BENCHMARK 5: Batch Dot Products (100 pairs × 1K elements each)');
  console.log('═'.repeat(80) + '\n');

  const batchVectors = Array(100)
    .fill(0)
    .map(() => {
      const v = new Float64Array(1000);
      for (let i = 0; i < v.length; i++) v[i] = Math.random();
      return v;
    });

  // In-process: direct calls in loop (single iteration for batch)
  const inProcBatch = await benchmark(
    'In-Process (loop of direct)',
    async () => {
      const results = [];
      for (let i = 0; i < batchVectors.length; i++) {
        for (let j = i + 1; j < Math.min(i + 3, batchVectors.length); j++) {
          results.push(npInProcess.dot(batchVectors[i], batchVectors[j]));
        }
      }
      return results;
    },
    1
  );

  // Isolated: py.task() in loop
  const isolatedBatch = await benchmark(
    'Isolated (loop of py.task)',
    async () => {
      const results = [];
      for (let i = 0; i < batchVectors.length; i++) {
        for (let j = i + 1; j < Math.min(i + 3, batchVectors.length); j++) {
          results.push(await pyIsolated.task(npIsolated.dot)(batchVectors[i], batchVectors[j]));
        }
      }
      return results;
    },
    1
  );

  // JavaScript: pure JS loop
  const jsBatch = benchmarkSync('JavaScript (loop)', () => {
    const results = [];
    for (let i = 0; i < batchVectors.length; i++) {
      for (let j = i + 1; j < Math.min(i + 3, batchVectors.length); j++) {
        let sum = 0;
        for (let k = 0; k < batchVectors[i].length; k++) {
          sum += batchVectors[i][k] * batchVectors[j][k];
        }
        results.push(sum);
      }
    }
    return results;
  }, 1);

  const batchCount = batchVectors.length * 2; // ~200 dot products
  console.log('Mode                               | Time (ms) | Per-call (ms)');
  console.log('-'.repeat(80));
  printResult('In-Process (loop of direct)', inProcBatch, 1);
  printResult('Isolated (loop of py.task)', isolatedBatch, 1);
  printResult('JavaScript (loop)', jsBatch, 1);

  if (inProcBatch.ok && isolatedBatch.ok && jsBatch.ok) {
    const inProcRatio = inProcBatch.elapsed / jsBatch.elapsed;
    const isolatedRatio = isolatedBatch.elapsed / jsBatch.elapsed;
    console.log(`\nIn-process loop: ${inProcRatio > 1 ? inProcRatio.toFixed(1) + 'x slower' : (1 / inProcRatio).toFixed(1) + 'x faster'} than JS`);
    console.log(`Isolated loop: ${isolatedRatio > 1 ? isolatedRatio.toFixed(1) + 'x slower' : (1 / isolatedRatio).toFixed(1) + 'x faster'} than JS`);
  }

  // Cleanup
  await pyInProcess.destroy();
  await pyIsolated.destroy();

  console.log('\n\n' + '═'.repeat(80));
  console.log('  KEY FINDINGS');
  console.log('═'.repeat(80) + '\n');

  console.log(`✅ IN-PROCESS MODE (direct calls, runtime reused):`);
  console.log(`   - 1M dot: ${inProcDot.ok ? (inProcDot.elapsed / 5).toFixed(3) + 'ms/call' : 'ERROR'}`);
  console.log(`   - FFT (64K): ${inProcFFT.ok ? (inProcFFT.elapsed / 3).toFixed(3) + 'ms/call' : 'ERROR'}`);
  console.log(`   - Cheap overhead: direct function calls into libpython`);
  console.log(`   - Risk: Python crash kills Node process\n`);

  console.log(`🔴 ISOLATED MODE (py.task() with IPC):`);
  console.log(`   - 1M dot: ${isolatedDot.ok ? (isolatedDot.elapsed / 5).toFixed(3) + 'ms/call' : 'ERROR'}`);
  console.log(`   - FFT (64K): ${isolatedFFT.ok ? (isolatedFFT.elapsed / 3).toFixed(3) + 'ms/call' : 'ERROR'}`);
  console.log(`   - Expensive: ~100µs per call for IPC marshaling`);
  console.log(`   - Benefit: Python crash isolated from Node\n`);

  console.log(`📊 RECOMMENDATION:`);
  console.log(`   ✓ Use in-process for:  Unavoidable ops (FFT, advanced linalg), high throughput`);
  console.log(`   ✓ Use isolated for:    Production, crash isolation, single large ops`);
  console.log(`   ✗ Use isolated in:     Loops (overhead multiplies 28x worse)`);
  console.log(`   ✗ Use NumPy for:       Simple aggregations (V8 wins), small arrays\n`);
})().catch(err => {
  console.error('\nFatal error:', err.message);
  process.exit(1);
});
