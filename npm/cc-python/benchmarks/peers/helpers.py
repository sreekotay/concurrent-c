"""Shared Python helpers for the peer bench.

Imported as a module (cc / pymport / pythonia / node-calls-python) so every
bridge runs the same functions. Numpy is optional: math / kwargs / callback /
exception rows still run without it.
"""
import math

try:
    import numpy as np
    HAS_NUMPY = True
except Exception:
    np = None
    HAS_NUMPY = False


def has_numpy():
    return bool(HAS_NUMPY)


def sqrt(x):
    return float(math.sqrt(x))


def describe(x):
    t = type(x).__name__
    dt = getattr(getattr(x, "dtype", None), "name", None)
    if dt:
        return t + ":" + str(dt)
    return t


def raise_key():
    raise KeyError("missing")


def do_sorted(xs, reverse=False):
    return sorted(list(xs), reverse=bool(reverse))


def apply(fn, x):
    return fn(x)


def dot(a, b):
    return float(np.dot(np.asarray(a, dtype=np.float64),
                        np.asarray(b, dtype=np.float64)))


def matmul_sum(a, b, n):
    n = int(n)
    A = np.asarray(a, dtype=np.float64).reshape(n, n)
    B = np.asarray(b, dtype=np.float64).reshape(n, n)
    return float(np.sum(A @ B))


def norm(a):
    return float(np.linalg.norm(np.asarray(a, dtype=np.float64)))
