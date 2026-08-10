#!/usr/bin/env node
/**
 * concurrent-c-python: Correct Usage Guide
 *
 * Two complementary modes with different trade-offs:
 * 1. In-Process: Synchronous, low latency (~5µs), limited packages, can chain handles
 * 2. Isolated: Asynchronous, crash-isolated, full Python ecosystem, ~100µs latency
 *
 * Choose based on your use case, not by trial-and-error.
 */
'use strict';

const ccpy = require('concurrent-c-python');

async function section(title) {
  console.log('\n' + '═'.repeat(70));
  console.log(`  ${title}`);
  console.log('═'.repeat(70) + '\n');
}

(async () => {
  console.log('\n╔════════════════════════════════════════════════════════════════╗');
  console.log('║  concurrent-c-python: Correct Patterns                         ║');
  console.log('╚════════════════════════════════════════════════════════════════╝\n');

  // ==================== SECTION 1: In-Process Mode ====================
  await section('PART 1: In-Process Mode (Sync, Low Latency, Limited Packages)');

  console.log('Use in-process when:');
  console.log('  ✓ Working with builtins, math, string operations');
  console.log('  ✓ Need low latency (~5µs per call)');
  console.log('  ✓ Want synchronous code patterns');
  console.log('  ✓ Need dict/exec workspaces\n');

  const py1 = ccpy.create();  // In-process (default)
  const builtins = py1.import('builtins');
  const math = py1.import('math');

  // ─── Example 1.1: Basic Operations ───
  console.log('Example 1.1: Basic math operations');
  const sqrtResult = await py1.task(math.sqrt)(16);
  console.log(`  math.sqrt(16) = ${sqrtResult}`);

  const absResult = await py1.task(builtins.abs)(-42);
  console.log(`  abs(-42) = ${absResult}\n`);

  // ─── Example 1.2: Define and call Python functions ───
  console.log('Example 1.2: Define Python function with dict/exec');
  const namespace = builtins.dict();
  builtins.exec(`
def greet(name):
    return f"Hello, {name}!"

def analyze_list(data):
    return {
        'sum': sum(data),
        'mean': sum(data) / len(data),
        'max': max(data),
        'min': min(data)
    }
  `, namespace);

  const greet = namespace.get('greet');
  const greeting = await py1.task(greet)('World');
  console.log(`  greet("World") = "${greeting}"`);

  const analyze = namespace.get('analyze_list');
  const stats = await py1.task(analyze)([1, 2, 3, 4, 5]);
  console.log(`  analyze_list([1..5]) = ${JSON.stringify(stats)}\n`);

  // ─── Example 1.3: Handle chaining (works in in-process) ───
  console.log('Example 1.3: Handle chaining (in-process can chain)');
  const pyList = builtins.list();
  pyList.append(10);
  pyList.append(20);
  pyList.append(30);

  const listLen = await py1.task(builtins.len)(pyList);
  console.log(`  Created list with 3 items, len(list) = ${listLen}\n`);

  await py1.destroy();

  // ==================== SECTION 2: Isolated Mode ====================
  await section('PART 2: Isolated Mode (Async, Full Python Ecosystem, Crash Isolated)');

  console.log('Use isolated when:');
  console.log('  ✓ Need NumPy, SciPy, pandas, or other C-extensions');
  console.log('  ✓ Processing large arrays (where kernel dominates)');
  console.log('  ✓ Want automatic system Python discovery');
  console.log('  ✓ Need crash isolation (failed Python code doesn\'t crash Node)');
  console.log('  ✓ Want true multi-core scaling (N domains = N GILs)\n');

  const py2 = ccpy.create({ isolated: true });
  const np = py2.import('numpy');

  // ─── Example 2.1: NumPy operations ───
  console.log('Example 2.1: NumPy operations (only available in isolated mode)');
  const arr = new Float64Array([1, 2, 3, 4, 5]);
  const npSum = await py2.task(np.sum)(arr);
  const npMean = await py2.task(np.mean)(arr);
  console.log(`  np.sum([1..5]) = ${npSum}`);
  console.log(`  np.mean([1..5]) = ${npMean}\n`);

  // ─── Example 2.2: FFT (NumPy exclusive) ───
  console.log('Example 2.2: FFT - Signal processing (NumPy exclusive)');
  const signal = new Float64Array(1000);
  for (let i = 0; i < 1000; i++) {
    signal[i] = Math.sin(i / 50) + Math.cos(i / 100);  // Composite wave
  }

  const start = performance.now();
  const fftResult = await py2.task(np.fft.fft)(signal);
  const elapsed = performance.now() - start;

  console.log(`  FFT on 1000 samples completed in ${elapsed.toFixed(2)}ms`);
  console.log(`  Result is handle: ${typeof fftResult}\n`);

  // ─── Example 2.3: Same-domain handle chaining ───
  console.log('Example 2.3: Same-domain handle chaining (works in isolated!)');
  console.log('  Chaining fft result to np.abs() - this DOES work:');

  try {
    const magnitude = await py2.task(np.abs)(fftResult);
    console.log(`  ✓ Chaining successful! Result: ${typeof magnitude} (handle)\n`);
  } catch (e) {
    console.log(`  ✗ Unexpected error: ${e.message}\n`);
  }

  // ─── Example 2.4: Large array performance ───
  console.log('Example 2.4: Performance - When NumPy wins');
  console.log('  Comparing NumPy vs Pure JavaScript for dot product:');
  console.log('  (NumPy overhead is ~2ms per call, but computation scales)\n');

  const sizes = [1000, 10000, 100000];
  console.log('  Size       | NumPy(ms) | JS(ms)  | Winner');
  console.log('  ' + '-'.repeat(50));

  for (const size of sizes) {
    const a = new Float64Array(size);
    const b = new Float64Array(size);
    for (let i = 0; i < size; i++) {
      a[i] = Math.random();
      b[i] = Math.random();
    }

    // NumPy dot product
    const start1 = performance.now();
    const npResult = await py2.task(np.dot)(a, b);
    const npTime = performance.now() - start1;

    // Pure JS dot product
    const start2 = performance.now();
    let jsResult = 0;
    for (let i = 0; i < size; i++) jsResult += a[i] * b[i];
    const jsTime = performance.now() - start2;

    const winner = jsTime < npTime ? 'JS' : 'NumPy';
    console.log(
      `  ${size.toString().padEnd(10)} | ` +
      `${npTime.toFixed(2).padEnd(9)} | ` +
      `${jsTime.toFixed(3).padEnd(7)} | ${winner}`
    );
  }
  console.log();

  await py2.destroy();

  // ==================== SECTION 3: Real-World Pattern ====================
  await section('PART 3: Real-World Example - Audio Analysis');

  const py3 = ccpy.create({ isolated: true });
  const np3 = py3.import('numpy');
  const builtins3 = py3.import('builtins');

  console.log('Scenario: Analyze 1 second of audio at 44.1kHz\n');

  // Generate test signal (440 Hz sine wave)
  const sampleRate = 44100;
  const duration = 1;
  const audioSignal = new Float64Array(sampleRate * duration);
  for (let i = 0; i < audioSignal.length; i++) {
    const t = i / sampleRate;
    audioSignal[i] = Math.sin(2 * Math.PI * 440 * t);  // 440 Hz tone (A note)
  }

  console.log(`Generated signal: ${audioSignal.length} samples at ${sampleRate}Hz\n`);

  // Define analysis function in Python
  const namespace3 = builtins3.dict();
  builtins3.exec(`
import numpy as np
from numpy.fft import fft, fftfreq

def analyze_audio(signal, sample_rate):
    # Compute FFT
    fft_result = fft(signal)
    magnitude = np.abs(fft_result)

    # Frequency axis
    freqs = fftfreq(len(signal), 1/sample_rate)

    # Peak frequency (positive frequencies only)
    positive_freqs = freqs[:len(freqs)//2]
    positive_mag = magnitude[:len(magnitude)//2]
    peak_idx = np.argmax(positive_mag)
    peak_freq = positive_freqs[peak_idx]
    peak_mag = positive_mag[peak_idx]

    return {
        'peak_frequency': float(peak_freq),
        'peak_magnitude': float(peak_mag),
        'total_energy': float(np.sum(magnitude**2))
    }
  `, namespace3);

  // Call the analysis function
  const analyzeAudio = namespace3.get('analyze_audio');
  const audioAnalysis = await py3.task(analyzeAudio)(audioSignal, sampleRate);

  console.log('Audio Analysis Results:');
  console.log(`  Peak frequency: ${audioAnalysis.peak_frequency.toFixed(1)} Hz (expected 440 Hz)`);
  console.log(`  Peak magnitude: ${audioAnalysis.peak_magnitude.toFixed(0)}`);
  console.log(`  Total energy: ${audioAnalysis.total_energy.toFixed(0)}\n`);

  await py3.destroy();

  // ==================== SECTION 4: Key Takeaways ====================
  await section('KEY TAKEAWAYS');

  console.log('1. TWO MODES, TWO PURPOSES:');
  console.log('   In-Process: Sync, fast, no packages → use for builtins/math');
  console.log('   Isolated:   Async, slower, full Python → use for NumPy\n');

  console.log('2. MAKE YOUR CHOICE UPFRONT:');
  console.log('   Don\'t try to "fix" in-process with isolated - choose the right one\n');

  console.log('3. HANDLE CHAINING:');
  console.log('   ✓ In-process: Handles chain freely between calls');
  console.log('   ✓ Isolated: Same-domain handles chain (np.fft → np.abs)');
  console.log('   ✗ Isolated: Cannot pass bridge handles created in parent\n');

  console.log('4. PERFORMANCE ECONOMICS:');
  console.log('   Isolated mode: ~2ms overhead + compute time');
  console.log('   Use NumPy only for large arrays (>100K elements)');
  console.log('   For small arrays, pure JavaScript is faster\n');

  console.log('5. ASYNC MATTERS:');
  console.log('   In-process: Can use sync patterns (dict/exec)');
  console.log('   Isolated: Everything is async (await all calls)\n');

  console.log('╔════════════════════════════════════════════════════════════════╗');
  console.log('║                    ✓ Examples Complete                         ║');
  console.log('╚════════════════════════════════════════════════════════════════╝\n');

})().catch(err => {
  console.error('Fatal error:', err.message);
  process.exit(1);
});
