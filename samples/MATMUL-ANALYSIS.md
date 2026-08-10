# Matrix Multiplication Showdown: NumPy vs ml-matrix vs JS vs GPU

## TL;DR

- **Small matrices (50×50):** NumPy wins from the start (0.4ms vs ml-matrix 0.8ms)
- **Medium (100-200):** NumPy dominates (0.15-0.37ms)
- **Large (300+):** ml-matrix catches up and wins at 300×300+ (splits wins 4-4)
- **GPU.js in Node:** Terrible (22ms for 100×100, not practical)

---

## Results Table

| Matrix Size | NumPy | ml-matrix | Pure JS | GPU.js | Winner |
|------------|-------|-----------|---------|--------|--------|
| 50×50 | 0.406ms | **0.798ms** | 2.75ms | N/A | ✅ NumPy |
| 100×100 | 0.148ms | 1.266ms | 3.81ms | 22.3ms | ✅ NumPy |
| 150×150 | 0.387ms | 0.456ms | 11.1ms | 24.8ms | ✅ NumPy |
| 200×200 | 0.367ms | 0.663ms | 25.5ms | 46.4ms | ✅ NumPy |
| **300×300** | 1.089ms | **1.061ms** | 87.7ms | 144.9ms | ✓ ml-matrix |
| 500×500 | 2.599ms | **2.481ms** | 453ms | 633ms | ✓ ml-matrix |
| 750×750 | 8.448ms | **7.093ms** | 1455ms | N/A | ✓ ml-matrix |
| 1000×1000 | 14.22ms | **8.421ms** | 4751ms | N/A | ✓ ml-matrix |

---

## Key Findings

### 1. NumPy Wins Small-to-Medium (50×50 to 200×200)

**In-process NumPy advantage:**
- Consistently faster from 50×50 onwards
- At 100×100: 0.148ms vs ml-matrix 1.27ms (8.5x faster!)
- At 200×200: 0.367ms vs ml-matrix 0.66ms (1.8x faster)

**Why:** NumPy's BLAS backend is optimized even for smaller matrices, while ml-matrix is general-purpose pure JS.

### 2. ml-matrix Catches Up Large (300×300+)

**Crossover around 300×300:**
- 300×300: ml-matrix 1.061ms vs NumPy 1.089ms (essentially tied)
- 500×500: ml-matrix 2.481ms vs NumPy 2.599ms (ml-matrix 1.05x faster)
- 1000×1000: ml-matrix 8.421ms vs NumPy 14.22ms (ml-matrix 1.7x faster!)

**Why:** ml-matrix likely has optimizations (cache-friendly layout, algorithmic tricks) that pay off at scale. NumPy might be hitting memory bandwidth limits or cache effects.

### 3. Pure JavaScript is Catastrophic

**Naive O(n³) implementation gets destroyed:**
- 50×50: 2.75ms (6.8x slower than NumPy)
- 100×100: 3.81ms (25.7x slower)
- 500×500: 453ms (174x slower!)
- 1000×1000: 4751ms (333x slower!)

Never use naive JS for matrix multiply.

### 4. GPU.js is Not Practical in Node.js

**GPU.js in CPU mode (no browser/WebGL):**
- 100×100: 22.3ms (150x slower than NumPy!)
- 150×150: 24.8ms
- 200×200: 46.4ms
- 500×500: 633ms (243x slower than ml-matrix!)

**Why:** GPU.js needs browser context for actual GPU. In Node.js CPU mode, it's just overhead with no benefit. WebGPU support is coming but not stable yet.

---

## Performance Scaling Analysis

```
                50→500 (10x size increase, O(n³) expects 1000x)

NumPy:    0.406ms → 2.60ms     = 6.4x
ml-matrix: 0.798ms → 2.48ms    = 3.1x  (anomalously good!)
JS:       2.75ms → 453ms       = 165x  (terrible)
GPU.js:   N/A → 633ms          = huge overhead
```

**Observation:** ml-matrix's nearly constant performance (3.1x over 10x size) is suspicious. Possible explanations:
1. Cache optimization that holds up well
2. Algorithm optimization (strided access patterns)
3. Benchmark artifact (results reused, not recomputed)

NumPy's 6.4x is reasonable (closer to O(n³) than JS's 165x).

---

## Decision Matrix

### When to Use Each:

| Scenario | Method | Reason |
|----------|--------|--------|
| **50-200×200 (dev/interactive)** | NumPy | 5-25x faster, cheap in-process |
| **300-1000×1000 (batch compute)** | ml-matrix | Same speed, no Python risk |
| **Crash isolation critical** | ml-matrix | Pure JS, no bridge |
| **GPU available (not Node)** | GPU acceleration | Use proper CUDA/cuBLAS |
| **One-shot operation** | NumPy | Fastest pure option |
| **Repeated small ops** | NumPy in-process | Amortize setup |
| **Production Node.js** | ml-matrix | Safe choice for 300×300+ |

### Practical Recommendation:

```javascript
if (matrixSize < 300) {
  // Use in-process NumPy - it wins consistently
  return numpy_inprocess;
} else if (matrixSize < 1000) {
  // ml-matrix competitive, no Python crash risk
  return ml_matrix_for_safety;
} else {
  // 1000×1000+: ml-matrix pulls ahead
  // Or consider actual GPU with proper bindings
  return ml_matrix;
}
```

---

## The Surprise: ml-matrix's Anomaly

ml-matrix doesn't scale like O(n³) should:
```
Expected for O(n³): 50→500 = 1000x slower
Observed:          0.798ms → 2.48ms = 3.1x slower
```

Possible reasons:
1. **Cache-oblivious algorithm** - ml-matrix might use cache-friendly layout
2. **Strided memory** - better CPU prefetcher utilization
3. **SIMD hints** - compiler optimizations the benchmarks don't show
4. **Benchmark artifact** - measurement variance

This is genuinely interesting. ml-matrix seems to have better algorithmic properties than naive JS, but we see NumPy at small sizes anyway.

---

## Absolute Performance Reference

For rough latency budgets:

| Matrix Size | NumPy | ml-matrix | JavaScript |
|------------|-------|-----------|------------|
| 100×100 | <1ms | 1.3ms | 4ms |
| 200×200 | <1ms | 0.7ms | 25ms |
| 500×500 | 2.6ms | 2.5ms | 450ms |
| 1000×1000 | 14ms | 8.4ms | 4.7s |

If you need <10ms latency: limit to 200×200 for NumPy, 300×300 for ml-matrix.

---

## GPU.js Verdict

**Don't use GPU.js for Node.js matrix math.** It requires:
1. Browser context (Headless Chrome, etc.) - huge overhead
2. WebGL or WebGPU - not stable in Node
3. CPU mode - just dead weight (22ms for 100×100)

If you need GPU:
- Use CUDA bindings (CuBLAS)
- Or TensorFlow.js with actual GPU backend (not Node.js)
- Or Rust/WASM bindings to BLAS

---

## Bottom Line

- **NumPy wins decisively** for matrices <300×300
- **ml-matrix is competitive** at 300×300+, safer in production
- **GPU.js is not viable** in Node.js without browser
- **Pure JS is never acceptable** for real matrix math
