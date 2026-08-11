# py_module vs nanobind — func microbenchmark

Python is the host. Concurrent-C (`py_module`) and [nanobind](https://nanobind.readthedocs.io/en/latest/benchmark.html) each provide an extension module with the same large set of trivial multi-arg adds; this harness measures **compile time**, **stripped binary size**, and **call overhead** (release builds only).

Methodology follows nanobind’s own [func suite](https://github.com/wjakob/nanobind/blob/master/docs/microbenchmark.ipynb): every permutation of a six-type argument list as `test_NNNN(a,b,c,d,e,f) → float`.

## Type list

| This harness | nanobind’s notebook |
|---|---|
| `int32_t` | `uint16_t` |
| `uint32_t` | `int32_t` |
| `int64_t` | `uint32_t` |
| `uint64_t` | `int64_t` |
| `float` | `uint64_t` |
| `double` | `float` |

`uint16_t` is replaced by `double` because `CC_PY_IN` has no scalar `unsigned short` arm (refuse rather than silent widen). Cardinality stays **720** permutations. Both sides use the same six types.

CC maps each free function to a module method on one `Bench` type (`Bench_test_NNNN`); from Python the call is still `m.test_0000(1,2,3,4,5,6)` — module state is unused. Nanobind’s class-heavy suite is out of scope (CC modules are not per-call classes).

## Run

```bash
# full (720 methods, median of 5 builds + 5×10M-call runtimes)
python3 perf/py_bind_micro/run.py > perf/baselines/py_bind_micro_$(date +%Y%m%d).txt

# smoke
python3 perf/py_bind_micro/run.py --limit 24 --samples 1 --iters 200000
```

Needs `ccc` (repo `cc/bin/ccc`), `cmake`, and a writable tree. The harness creates `perf/py_bind_micro/.venv` and `pip install nanobind` there.

## Reading RESULT lines

- `RESULT func compile_s {cc,nanobind}` — median wall seconds for a clean release rebuild
- `RESULT func size_bytes {cc,nanobind}` — stripped artifact
- `RESULT func runtime_ns_per_call {cc,nanobind,python}` — median ns/call
- `*_ratio_cc_over_nb` — stable comparison; absolute ns moves with host load
