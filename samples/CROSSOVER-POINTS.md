# Crossover Points: Where Each Method Wins

## Summary Table

| Workload | Crossover Point | Winner | Ratio |
|----------|-----------------|--------|-------|
| **Dot product** | ~1000 elements | In-process NumPy | 2x at breakpoint |
| **Matrix multiply** | ~32×32 | In-process NumPy | 5x at 32×32 |
| **Single isolated call** | Always | In-process (if possible) | 6.6x difference |
| **Batch loops** | > 5 calls | In-process NumPy | 16-28x at 10-100 calls |

---

## 1. Dot Product Crossover

```
100 elements:       JS 0.018ms  >> NumPy 0.248ms  ❌ JS wins (13x faster)
1K elements:        NumPy 0.047ms ✅ NumPy wins (2x faster)
10K elements:       NumPy 0.048ms ✅ NumPy wins (16x faster)
100K elements:      JS 0.204ms  ⚠️  Results noisy (variance)
1M elements:        JS 3.57ms   ⚠️  Inconsistent vs earlier test
```

**Crossover:** ~500-1000 elements

**Why:** At small sizes, function call overhead dominates. Once arrays are large enough for NumPy's SIMD advantage to show, NumPy wins. But results show variance at 100K+, suggesting SIMD optimization isn't deterministic.

---

## 2. Matrix Multiply Crossover

```
10×10:      ml-matrix 0.113ms  >> NumPy 0.180ms  ❌ ml-matrix wins
32×32:      NumPy 0.068ms      ✅ NumPy wins (5x faster)
64×64:      NumPy 0.088ms      ✅ NumPy wins (3x faster)
100×100:    NumPy 0.159ms      ✅ NumPy wins (2.3x faster)
150×150:    ml-matrix 0.348ms  ⚠️  ml-matrix faster (1.4x)
200×200:    NumPy 0.519ms      ✅ NumPy wins (3x faster)
300×300:    NumPy 1.069ms      ✅ NumPy wins (1.1x faster)
```

**Crossover 1:** ~32×32 (NumPy becomes competitive)
**Crossover 2:** ~150×150 (ml-matrix catches up briefly)
**Crossover 3:** ~200×200 (NumPy dominates again)

**Why:** 
- Small matrices: ml-matrix's pure JS is fast, NumPy overhead visible
- 32×32+: NumPy's BLAS kicks in, beats ml-matrix
- 150×150: ml-matrix has optimizations that briefly catch up
- 200×200+: NumPy's superiority clear

**Recommendation:** Use ml-matrix for <32×32, NumPy for ≥32×32 if available.

---

## 3. Isolated vs In-Process Overhead

```
Single call:        6.56x overhead (1.41ms vs 0.21ms)
10 operations:      16.21x overhead (5.09ms vs 0.31ms)
100 operations:     28.05x overhead (46.2ms vs 1.65ms)
```

**Pattern:** Overhead multiplies linearly with call count.

```
Cost per isolated call: ~1.2ms
Cost per in-process call: ~0.02ms
Difference per call: ~1.18ms
```

**Decision:**
- **1 call**: 6.56x slower (acceptable for crash isolation)
- **2-3 calls**: Maybe acceptable (10-15x overhead)
- **5+ calls**: Use in-process or pure JS (overhead too high)
- **100+ calls**: Never use isolated (28x worse)

---

## 4. Batch Operation Efficiency

**In-process loop scaling** (direct calls):
```
1 op:    0.21ms
10 ops:  0.31ms (overhead: 0.01ms/call)
100 ops: 1.65ms (overhead: 0.017ms/call)
```

✅ Nearly linear: overhead is only ~15µs per call

**Isolated loop scaling** (py.task() calls):
```
1 op:    1.41ms
10 ops:  5.09ms (overhead: 0.37ms/call)
100 ops: 46.2ms (overhead: 0.45ms/call)
```

❌ Overhead multiplies: ~450µs per call in loops

---

## Revised Decision Tree

```
if (operation == 'FFT' || operation == 'eigendecomposition' || 
    operation == 'SVD' || operation == 'unavailable_in_js') {
  // Must use NumPy
  if (production_app && crash_isolation_critical) {
    if (num_calls == 1) {
      return isolated_mode;  // 6.56x overhead acceptable
    } else {
      return in_process_mode;  // Overhead grows linearly
    }
  } else {
    return in_process_mode;  // Faster
  }
}

// Have a choice of library
if (operation == 'dot_product') {
  if (array_size < 500) {
    return javascript;  // JS faster
  } else if (array_size < 100000) {
    return in_process_numpy;  // NumPy wins
  } else {
    // Variance zone - results noisy, profile both
    return profile_or_use_javascript;
  }
}

if (operation == 'matrix_multiply') {
  if (matrix_size < 32 * 32) {
    return ml_matrix;  // ml-matrix faster
  } else if (matrix_size < 150 * 150) {
    return in_process_numpy;  // NumPy dominates
  } else {
    return in_process_numpy;  // Still win, use NumPy
  }
}

// Loops
if (loop_iterations > 5) {
  return in_process_mode || javascript;  // Never isolated mode
}

// Default: safe
return javascript;
```

---

## Surprising Findings

1. **In-process NumPy beats ml-matrix** from ~32×32 onwards, despite ml-matrix being pure JS
2. **Isolated mode is expensive**: 6.56x for a single call, 28x for 100 calls
3. **Dot product results are noisy** at large sizes (100K+), suggesting SIMD isn't deterministic
4. **ml-matrix has a bump** at 150×150 where it briefly catches NumPy before NumPy dominates again
5. **Batch loops**: In-process scales well (15µs/call), isolated catastrophically (450µs/call)

---

## Bottom Line

| Scenario | Recommendation | Crossover |
|----------|---|---|
| Single unavoidable NumPy op | In-process | N/A |
| Crash isolation critical | Isolated if ≤1 call, else in-process | 1 call @ 6.56x |
| Dot product 500-10K elements | In-process NumPy | ~1000 elem |
| Matrix ≥32×32 | In-process NumPy | 32×32 |
| Matrix <32×32 | ml-matrix | <32×32 |
| Batch operations | In-process | ~5 calls |
| Everything else | JavaScript | - |

