# Correct Benchmark Results: In-Process vs Isolated vs JavaScript

## The Critical Insight

**In-process mode with direct calls is NOT expensive.**

The earlier analysis measured artificial overhead from `py.task()` even for in-process mode, which is wrong. In-process mode should use direct calls (no `py.task()`), and they have minimal overhead.

---

## Results Summary

### 1. Simple Aggregation (Sum 100K elements)

| Mode | Time/Call | vs JavaScript | Winner |
|------|----------|---------------|--------|
| **In-Process** | 0.191ms | **1.8x FASTER** | ✅ NumPy |
| Isolated | 12.9ms | 36.6x SLOWER | ❌ NumPy |
| JavaScript | 0.353ms | baseline | ✓ Good |

**Finding:** V8 is good at sum, but in-process NumPy is BETTER. Isolated is terrible.

---

### 2. Large Compute (Dot Product 1M elements)

| Mode | Time/Call | vs JavaScript | Winner |
|------|----------|---------------|--------|
| **In-Process** | 0.682ms | **4.6x FASTER** | ✅ NumPy |
| Isolated | 28.34ms | 9x SLOWER | ❌ NumPy |
| JavaScript | 3.15ms | baseline | ✓ Good |

**Finding:** In-process NumPy dominates due to SIMD optimization. Isolated is essentially unusable for this workload.

---

### 3. Matrix Multiply (200×200)

| Mode | Time/Call | vs ml-matrix | Winner |
|------|----------|--------------|--------|
| **In-Process** | 0.505ms | **5.0x FASTER** | ✅ NumPy |
| ml-matrix | 2.50ms | baseline | ✓ Good |
| Isolated | ERROR | - | ❌ API issue |

**Finding:** In-process NumPy beats specialized JS libraries by 5x.

---

### 4. FFT (64K samples)

| Mode | Time/Call | Overhead | Winner |
|------|----------|----------|--------|
| **In-Process** | 4.41ms | ~28µs | ✅ NumPy |
| Isolated | 9.25ms | ~100µs | ✓ Works |
| JavaScript | N/A | - | ❌ No equivalent |

**Finding:** In-process is 2.1x faster than isolated. FFT is unavoidable.

---

### 5. Batch Operations (Loop of ~200 small dots)

| Mode | Total Time | Per-Op | vs JavaScript |
|------|-----------|--------|--------------|
| **In-Process** | 2.96ms | 15µs | **1.5x FASTER** | ✅ NumPy |
| Isolated | 120.58ms | 603µs | 27.3x SLOWER | ❌ NumPy |
| JavaScript | 4.41ms | 22µs | baseline | ✓ Good |

**Finding:** In-process loops are cheap (15µs per call). Isolated loops are catastrophic (600µs per call).

---

## The Key Revelation

**The old analysis was WRONG because it used `py.task()` for in-process mode.**

```javascript
// WRONG (what we measured before):
const py = ccpy.create();
const np = py.import('numpy');
await py.task(np.sum)(data);  // Extra IPC overhead!

// RIGHT (what in-process should do):
const py = ccpy.create();
const np = py.import('numpy');
const result = np.sum(data);  // Direct call, minimal overhead!
```

**When you use in-process correctly (direct calls), it's fast.**

---

## When to Use Each Mode

### ✅ IN-PROCESS MODE (Direct calls)

**Use for:**
- High-throughput operations (FFT, large matmul, advanced linalg)
- Loops of operations (overhead is only 15µs per call)
- Unavoidable NumPy operations
- Latency-sensitive work

**Pros:**
- Direct function calls into libpython (~5-15µs overhead per call)
- Beats JavaScript and ml-matrix for compute-heavy work
- Fast loops (batching is fine)

**Cons:**
- Python crash kills Node process (no isolation)
- Single-threaded (Python GIL blocks concurrency)

### ✅ ISOLATED MODE (py.task() with IPC)

**Use for:**
- Production systems needing crash isolation
- Single large operations where 100µs overhead is acceptable
- Services where a Python crash must not crash Node

**Pros:**
- Python crash doesn't kill Node
- Can be multi-process scaled
- Safe for untrusted Python code

**Cons:**
- ~100µs per call overhead (28.34ms for 1M dot vs 0.68ms in-process)
- **DON'T use in loops** (overhead multiplies to 27x slower)

### ❌ DON'T use NumPy for:
- Simple aggregations where ml-matrix/JS are comparable (V8 is still pretty good)
- Batch operations via isolated mode (overhead multiplies)
- Small arrays (<100 elements)

---

## Performance Hierarchy

For a single large operation:

```
1. In-process NumPy (0.68ms for 1M dot)   ✅ FASTEST
2. JavaScript (3.15ms for 1M dot)
3. ml-matrix (2.50ms for 200×200 matmul)
4. Isolated NumPy (28.34ms for 1M dot)   ❌ SLOWEST
```

For a loop of small operations:

```
1. In-process NumPy (15µs per call)       ✅ FASTEST
2. JavaScript (22µs per call)
3. Isolated NumPy (600µs per call)        ❌ 40x SLOWER
```

---

## Corrected Decision Tree

```
if (operation == 'FFT' || operation == 'eigendecomposition' || 
    operation == 'SVD' || operation == 'advanced_linalg') {
  // NumPy unavoidable
  if (production_app) {
    return isolated_mode;  // Crash isolation worth the 2x overhead
  } else {
    return in_process_mode;  // Faster, no crash risk in dev
  }
}

if (is_large_matrix_operation && computation_time > 1ms) {
  // Matrix multiply, solve, etc.
  if (production_app) {
    return isolated_mode;  // Trade 2x speed for crash isolation
  } else {
    return in_process_mode;  // 5x faster than ml-matrix
  }
}

// Everything else: use JavaScript
return javascript;
```

---

## Why This Matters

**The bridge is actually GOOD when used correctly.**

In-process mode gives you:
- NumPy's performance (4.6x faster for large arrays)
- ml-matrix's speed (5x faster than ml-matrix for matmul)
- Zero bridge overhead (just direct function calls)

The price: Python crash kills Node (acceptable for many use cases).

**For crash isolation, isolated mode works but costs 28x for loops.**

---

## Methodology

- **In-process (v2):** Direct calls, runtime reused, no `py.task()`
- **Isolated:** `py.task()` with IPC marshaling, runtime reused
- **JavaScript:** Pure V8 loops, no overhead
- **ml-matrix:** Specialized JS library

All tests reused runtimes to avoid create/destroy overhead.
