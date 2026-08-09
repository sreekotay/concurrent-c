# cc-python isolated-domain broker: a FULL CPython per domain, speaking
# line-JSON on stdio — the same wire discipline as cc-node's broker.cjs,
# mirrored.  Dependency-free (stdlib only); numpy is the USER's, imported
# on request like any module.
#
# Protocol (one JSON object per line, strict request/response):
#   -> {"op":"import","name":m}            <- {"h":id} | {"v":plain} | {"e":msg}
#   -> {"op":"get","h":id,"name":a}        <- value/handle/error
#   -> {"op":"call","h":id,"args":[...]}   <- value/handle/error
#   -> {"op":"str","h":id}                 <- {"v":"..."}
#   -> {"op":"release","h":id}             <- {"v":n}   (remaining live)
#   -> {"op":"stats"}                      <- {"v":n}
#   -> {"op":"close"}                      <- {"v":true}, then exit
#
# Values: finite numbers / str / bool / None / lists / dicts cross by
# value; non-finite floats tag as {"nf":"inf"|"-inf"|"nan"}; typed
# buffers as {"ta":kind,"b64":...} (numpy arrays when numpy is loadable
# in this child, else array.array); everything else is a handle.  A JS
# function argument arrives as {"$f":id}: calling it sends
# {"cb":true,"cbid":id,"args":[...]} and BLOCKS on the reply line
# {"cbr":...} — nested callbacks compose by strict alternation, and the
# parent may take arbitrarily long (awaiting its own promises) before
# replying.  EOF on stdin is revocation: drop everything, exit.
import base64
import json
import math
import os
import sys

_handles = {}
_next = [1]
_out = sys.stdout
# Big buffers spill through shared memory (tmpfs on Linux) instead of
# base64: one memcpy per side, no inflation, no JSON bloat.  The dir
# comes from the parent; files are consumed-and-unlinked per message.
_shm_dir = os.environ.get('CC_PY_SHM_DIR') or (
    '/dev/shm' if os.path.isdir('/dev/shm') else None)
_shm_seq = [0]
_SPILL = 1 << 16


def _shm_write(raw):
    _shm_seq[0] += 1
    path = os.path.join(_shm_dir, 'ccpy-%d-%d' % (os.getpid(), _shm_seq[0]))
    with open(path, 'wb') as f:
        f.write(raw)
    return path


def _put(v):
    h = _next[0]
    _next[0] += 1
    _handles[h] = v
    return h


def _np():
    try:
        import numpy
        return numpy
    except Exception:
        return None


_TA = {
    'f64': ('d', 'float64'), 'f32': ('f', 'float32'),
    'i32': ('i', 'int32'), 'i64': ('q', 'int64'),
    'u8': ('B', 'uint8'),
}
_TA_BY_DTYPE = {dt: k for k, (_, dt) in _TA.items()}


def _decode(v):
    if isinstance(v, dict):
        if '$h' in v:
            return _handles[v['$h']]
        if '$f' in v:
            fid = v['$f']
            def cb(*args):
                _send({'cb': True, 'cbid': fid,
                       'args': [_encode_val(a) for a in args]})
                line = sys.stdin.readline()
                if not line:
                    raise RuntimeError('bridge is closed')
                r = json.loads(line)
                if 'e' in r:
                    raise RuntimeError(r['e'])
                return _decode(r.get('cbr'))
            return cb
        if '$nf' in v:
            return {'inf': math.inf, '-inf': -math.inf}.get(v['$nf'],
                                                            math.nan)
        if '$ta' in v:
            raw = base64.b64decode(v['b64'])
            np = _np()
            if np is not None:
                return np.frombuffer(raw, dtype=_TA[v['$ta']][1]).copy()
            import array
            a = array.array(_TA[v['$ta']][0])
            a.frombytes(raw)
            return a
        if '$shm' in v:
            path = v['$shm']
            try:
                np = _np()
                if np is not None:
                    return np.fromfile(path, dtype=_TA[v['t']][1])
                import array
                a = array.array(_TA[v['t']][0])
                with open(path, 'rb') as f:
                    a.frombytes(f.read())
                return a
            finally:
                try:
                    os.unlink(path)
                except OSError:
                    pass
        return {k: _decode(x) for k, x in v.items()}
    if isinstance(v, list):
        return [_decode(x) for x in v]
    return v


def _plain(v, depth=0):
    if depth > 16:
        return False
    if v is None or isinstance(v, (bool, str)):
        return True
    if isinstance(v, float):
        return True  # non-finite handled at encode
    if isinstance(v, int):
        return -(2**53) < v < 2**53
    if isinstance(v, (list, tuple)):
        return all(_plain(x, depth + 1) for x in v)
    if isinstance(v, dict):
        return all(isinstance(k, str) and not k.startswith('$') and
                   _plain(x, depth + 1) for k, x in v.items())
    return False


def _encode_val(v):
    if isinstance(v, float) and not math.isfinite(v):
        return {'$nf': 'inf' if v == math.inf
                else '-inf' if v == -math.inf else 'nan'}
    if isinstance(v, (list, tuple)):
        return [_encode_val(x) for x in v]
    if isinstance(v, dict):
        return {k: _encode_val(x) for k, x in v.items()}
    return v


def _encode(v):
    np = _np()
    if np is not None and isinstance(v, np.generic):
        v = v.item()  # numpy scalar -> python scalar
    if np is not None and isinstance(v, np.ndarray):
        key = _TA_BY_DTYPE.get(str(v.dtype))
        # Inline small arrays; large ones stay handles (the shm lease
        # tier lifts this — for now crossing bulk data costs a copy and
        # says so in the docs).
        if key is not None and v.nbytes <= 1 << 16 and v.ndim == 1:
            return {'ta': key,
                    'b64': base64.b64encode(v.tobytes()).decode('ascii')}
        return {'h': _put(v)}
    if _plain(v):
        return {'v': _encode_val(v)}
    return {'h': _put(v)}


def _send(obj):
    _out.write(json.dumps(obj))
    _out.write('\n')
    _out.flush()


def _run(v):
    # A coroutine result runs to completion here — the child is serial
    # per request; PARALLELISM is domains, each a whole process.
    import inspect
    if inspect.iscoroutine(v):
        import asyncio
        return asyncio.run(v)
    return v


def _walk(req):
    v = _handles[req['h']]
    for name in req.get('path', []):
        v = getattr(v, name)
    return v


def _dispatch(req):
    op = req['op']
    if op == 'import':
        import importlib
        return _encode(importlib.import_module(req['name']))
    if op == 'getp':
        return _encode(_walk(req))
    if op == 'callp':
        f = _walk(req)
        args = [_decode(a) for a in req.get('args', [])]
        return _encode(_run(f(*args)))
    if op == 'ta':
        # Materialize a buffer-shaped value back to the parent: small
        # inline, big through the shm spill.
        v = _walk(req)
        np = _np()
        if np is not None and isinstance(v, np.ndarray):
            v = np.ascontiguousarray(v)
            key = _TA_BY_DTYPE.get(str(v.dtype))
            if key is None:
                return {'e': 'cc-python broker: unsupported dtype ' +
                             str(v.dtype)}
            raw = v.tobytes()
        else:
            import array
            if not isinstance(v, array.array):
                return {'e': 'cc-python broker: not a buffer-shaped value'}
            key = None
            for k, (tc, _) in _TA.items():
                if tc == v.typecode:
                    key = k
                    break
            if key is None:
                return {'e': 'cc-python broker: unsupported typecode ' +
                             v.typecode}
            raw = v.tobytes()
        if _shm_dir and len(raw) > _SPILL:
            return {'shm': _shm_write(raw), 't': key}
        return {'ta': key, 'b64': base64.b64encode(raw).decode('ascii')}
    if op == 'str':
        return {'v': str(_walk(req))}
    if op == 'release':
        _handles.pop(req['h'], None)
        return {'v': len(_handles)}
    if op == 'stats':
        return {'v': len(_handles)}
    if op == 'close':
        return {'v': True}
    return {'e': 'cc-python broker: unknown op ' + repr(op)}


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception as e:
            _send({'e': 'cc-python broker: bad request: %s' % e})
            continue
        try:
            resp = _dispatch(req)
        except Exception as e:
            resp = {'e': '%s: %s' % (type(e).__name__, e)}
        _send(resp)
        if req.get('op') == 'close':
            break


if __name__ == '__main__':
    main()
