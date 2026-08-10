#!/usr/bin/env node
/**
 * Matrix Multiplication Benchmark: NumPy vs ml-matrix vs Pure JS vs GPU
 *
 * Tests all four approaches across matrix sizes from 50×50 to 1000×1000
 * to find crossover points and optimal use cases
 */
'use strict';

const ccpy = require('concurrent-c-python');
const path = require('path');
const Matrix = require('ml-matrix').Matrix;
const { GPU } = require('gpu.js');

// Setup
const venvPython = path.join(__dirname, '..', 'venv', 'bin', 'python');
try {
  ccpy.usePython(venvPython);
} catch (e) {}

const gpu = new GPU({ mode: 'cpu' }); // Use CPU mode for compatibility

// Create GPU kernel for matrix multiply
const gpuMatmul = gpu.createKernel(function (a, b, aSize) {
  let sum = 0;
  for (let i = 0; i < aSize; i++) {
    sum += a[this.thread.y][i] * b[i][this.thread.x];
  }
  return sum;
}).setOutput([1000, 1000]); // Max size

async function benchmarkAsync(fn, iterations = 1) {
  const start = performance.now();
  let result;
  for (let i = 0; i < iterations; i++) {
    result = await fn();
  }
  return {
    elapsed: (performance.now() - start) / iterations,
    result
  };
}

function benchmarkSync(fn, iterations = 1) {
  const start = performance.now();
  let result;
  for (let i = 0; i < iterations; i++) {
    result = fn();
  }
  return {
    elapsed: (performance.now() - start) / iterations,
    result
  };
}

(async () => {
  console.log('\n╔════════════════════════════════════════════════════════════════════╗');
  console.log('║  MATRIX MULTIPLICATION: NumPy vs ml-matrix vs JS vs GPU           ║');
  console.log('╚════════════════════════════════════════════════════════════════════╝\n');

  // Setup runtimes once
  const pyInProcess = ccpy.create();
  const npInProcess = pyInProcess.import('numpy');

  const sizes = [50, 100, 150, 200, 300, 500, 750, 1000];

  console.log('═'.repeat(100));
  console.log('  Matrix Size | NumPy (ms) | ml-matrix (ms) | Pure JS (ms) | GPU.js (ms) | Winner');
  console.log('═'.repeat(100));

  const results = [];

  for (const size of sizes) {
    // Create test matrices
    const matA = new Float64Array(size * size);
    const matB = new Float64Array(size * size);
    for (let i = 0; i < matA.length; i++) {
      matA[i] = Math.random() * 100;
      matB[i] = Math.random() * 100;
    }

    // ==================== NumPy ====================
    const npA = npInProcess.array(matA).reshape(size, size);
    const npB = npInProcess.array(matB).reshape(size, size);

    const npResult = await benchmarkAsync(
      () => npInProcess.matmul(npA, npB),
      size <= 200 ? 3 : 1
    );

    // ==================== ml-matrix ====================
    const mlResult = benchmarkSync(() => {
      const A = new Matrix(size, size, matA);
      const B = new Matrix(size, size, matB);
      return A.multiply(B);
    }, size <= 200 ? 3 : 1);

    // ==================== Pure JavaScript ====================
    const jsResult = benchmarkSync(() => {
      const result = new Float64Array(size * size);
      for (let i = 0; i < size; i++) {
        for (let j = 0; j < size; j++) {
          let sum = 0;
          for (let k = 0; k < size; k++) {
            sum += matA[i * size + k] * matB[k * size + j];
          }
          result[i * size + j] = sum;
        }
      }
      return result;
    }, size <= 200 ? 2 : 1);

    // ==================== GPU.js ====================
    // Note: GPU.js has overhead, only test for medium+ sizes
    let gpuResult = { elapsed: Infinity, result: null };
    if (size >= 100 && size <= 500) {
      try {
        // Need to convert to 2D arrays for GPU
        const a2d = [];
        const b2d = [];
        for (let i = 0; i < size; i++) {
          a2d.push(Array.from(matA.slice(i * size, (i + 1) * size)));
          b2d.push(Array.from(matB.slice(i * size, (i + 1) * size)));
        }

        gpuResult = benchmarkSync(() => {
          try {
            const matSize = size;
            const gpuMatmulFunc = gpu.createKernel(function (a, b) {
              let sum = 0;
              for (let i = 0; i < this.constants.matSize; i++) {
                sum += a[this.thread.y][i] * b[i][this.thread.x];
              }
              return sum;
            })
              .setConstants({ matSize })
              .setOutput([size, size])
              .setLoopMaxIterations(size);

            const result = gpuMatmulFunc(a2d, b2d);
            return result;
          } catch (e) {
            throw e;
          }
        }, 1);
      } catch (e) {
        gpuResult = { elapsed: Infinity, error: e.message };
      }
    }

    // Find winner
    const times = {
      'NumPy': npResult.elapsed,
      'ml-matrix': mlResult.elapsed,
      'Pure JS': jsResult.elapsed,
      'GPU.js': gpuResult.elapsed
    };

    const winner = Object.entries(times).reduce((a, b) =>
      a[1] < b[1] ? a : b
    )[0];

    results.push({
      size,
      numpy: npResult.elapsed,
      mlmatrix: mlResult.elapsed,
      js: jsResult.elapsed,
      gpu: gpuResult.elapsed,
      winner
    });

    // Format output
    const gpuStr = gpuResult.elapsed === Infinity ? 'N/A' : gpuResult.elapsed.toFixed(4);
    console.log(
      `${`${size}×${size}`.padEnd(11)} | ` +
        `${npResult.elapsed.toFixed(4).padEnd(10)} | ` +
        `${mlResult.elapsed.toFixed(4).padEnd(14)} | ` +
        `${jsResult.elapsed.toFixed(4).padEnd(12)} | ` +
        `${gpuStr.padEnd(11)} | ${winner}`
    );
  }

  // Cleanup
  await pyInProcess.destroy();

  // ==================== ANALYSIS ====================
  console.log('\n' + '═'.repeat(100));
  console.log('  ANALYSIS');
  console.log('═'.repeat(100) + '\n');

  // Find crossover points
  console.log('CROSSOVER POINTS:\n');

  let numpyBeatsML = false;
  let numpyBeatsJS = false;
  let gpuWorth = false;

  for (const r of results) {
    if (!numpyBeatsML && r.numpy < r.mlmatrix) {
      console.log(`✅ NumPy beats ml-matrix at: ${r.size}×${r.size}`);
      console.log(`   NumPy: ${r.numpy.toFixed(4)}ms vs ml-matrix: ${r.mlmatrix.toFixed(4)}ms\n`);
      numpyBeatsML = true;
    }

    if (!numpyBeatsJS && r.numpy < r.js) {
      console.log(`✅ NumPy beats pure JS at: ${r.size}×${r.size}`);
      console.log(`   NumPy: ${r.numpy.toFixed(4)}ms vs JS: ${r.js.toFixed(4)}ms\n`);
      numpyBeatsJS = true;
    }

    if (!gpuWorth && r.gpu !== Infinity && r.gpu < r.numpy && r.gpu < r.mlmatrix) {
      console.log(`✅ GPU.js becomes fastest at: ${r.size}×${r.size}`);
      console.log(`   GPU.js: ${r.gpu.toFixed(4)}ms vs NumPy: ${r.numpy.toFixed(4)}ms\n`);
      gpuWorth = true;
    }
  }

  // Winner summary
  console.log('WINNER SUMMARY:\n');
  const winners = {};
  for (const r of results) {
    winners[r.winner] = (winners[r.winner] || 0) + 1;
  }

  for (const [method, count] of Object.entries(winners)) {
    console.log(`${method.padEnd(12)} wins: ${count}/${results.length} tests`);
  }

  // Performance scaling
  console.log('\n\nPERFORMANCE SCALING (O(n³) analysis):\n');
  console.log('Method        | 50×50    | 100×100  | 500×500   | Scaling Factor');
  console.log('-'.repeat(70));

  const s50 = results.find(r => r.size === 50);
  const s100 = results.find(r => r.size === 100);
  const s500 = results.find(r => r.size === 500);

  if (s50 && s100 && s500) {
    // NumPy scaling
    const npScale = s500.numpy / s100.numpy;
    console.log(
      `NumPy         | ${s50.numpy.toFixed(4)}ms | ${s100.numpy.toFixed(4)}ms  | ` +
        `${s500.numpy.toFixed(2)}ms   | ${npScale.toFixed(1)}x`
    );

    // ml-matrix scaling
    const mlScale = s500.mlmatrix / s100.mlmatrix;
    console.log(
      `ml-matrix     | ${s50.mlmatrix.toFixed(4)}ms | ${s100.mlmatrix.toFixed(4)}ms  | ` +
        `${s500.mlmatrix.toFixed(2)}ms   | ${mlScale.toFixed(1)}x`
    );

    // JS scaling
    const jsScale = s500.js / s100.js;
    console.log(
      `Pure JS       | ${s50.js.toFixed(4)}ms | ${s100.js.toFixed(4)}ms  | ` +
        `${s500.js.toFixed(2)}ms   | ${jsScale.toFixed(1)}x`
    );

    // GPU scaling (if available)
    if (s500.gpu !== Infinity) {
      const gpuScale = s500.gpu / s100.gpu;
      console.log(
        `GPU.js        | ${s50.gpu.toFixed(4)}ms | ${s100.gpu.toFixed(4)}ms  | ` +
          `${s500.gpu.toFixed(2)}ms   | ${gpuScale.toFixed(1)}x`
      );
    }
  }

  // Recommendations
  console.log('\n\nRECOMMENDATIONS:\n');
  console.log('Matrix Size   | Method      | Reason');
  console.log('-'.repeat(70));
  console.log('<50×50        | ml-matrix   | Pure JS overhead minimal, library optimized');
  console.log('50-150×150    | NumPy       | In-process beats ml-matrix');
  console.log('150-500×500   | NumPy       | Clear advantage, BLAS optimization');
  console.log('>500×500      | NumPy/GPU   | NumPy dominates (GPU overhead high in Node)');
  console.log('Crash safe    | ml-matrix   | No bridge, no Python crash risk');
  console.log('Production    | NumPy       | If crash isolation not critical');
})().catch(err => {
  console.error('\nError:', err.message);
  console.error(err.stack);
  process.exit(1);
});
