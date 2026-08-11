# cc-python isolated-domain broker: a FULL CPython per domain, speaking
# line-JSON on dedicated wire fds (3 in / 4 out; stdio stays the
# user's) — the same wire discipline as cc-node's broker.cjs, mirrored.
# Dependency-free (stdlib only); numpy is the USER's, imported on
# request like any module.
#
# Protocol (one JSON object per line; every request carries "id":n and
# the reply echoes it — the parent pairs replies by id, never by order,
# so a stray line on the channel can only be ignored, not misassigned):
#   -> {"op":"import","name":m}            <- {"h":id} | {"v":plain} | {"e":msg}
#   -> {"op":"get","h":id,"name":a}        <- value/handle/error
#   -> {"op":"call","h":id,"args":[...]}   <- value/handle/error
#      ("callp" adds "path":[...] and optional "kw":{name:enc,...} —
#       keyword arguments, f(*args, **kw))
#   -> {"op":"str","h":id}                 <- {"v":"..."}
#   -> {"op":"tojs","h":id,"path":[...]}   <- {"v":strict-plain} | {"e":msg}
#   -> {"op":"release","h":id}             <- {"v":n}   (remaining live)
#   -> {"op":"stats"}                      <- {"v":n}
#   -> {"op":"close"}                      <- {"v":true}, then exit
#
# Values: finite numbers / str / bool / None / lists / non-empty plain
# dicts cross by value; an empty dict stays a handle (so
# `await builtins.dict()` remains a live mapping for exec/namespaces —
# materializing it to JS `{}` made `.get` disappear).  Non-finite floats
# tag as {"nf":"inf"|"-inf"|"nan"}; typed buffers as {"ta":kind,"b64":...}
# (numpy arrays when numpy is loadable in this child, else array.array);
# everything else is a handle.  A JS function argument arrives as
# {"$f":id}: calling it sends {"cb":true,"cbid":id,"args":[...]} and
# BLOCKS on the reply line {"cbr":...} (or {"e":...}).  The parent may
# take arbitrarily long (awaiting its own promises) before replying, and
# may have already pipelined later ops onto the wire — those are parked
# until the cbr lands, then drained in order.  EOF on the request fd is
# revocation: drop everything, exit.
import base64
import json
import math
import os
import sys

_handles = {}
_next = [1]
# The wire lives on dedicated fds so user code owns stdin/stdout —
# print() reaches the real stdout and can never collide with a protocol
# reply.  Requests arrive on fd 3, replies leave on fd 4 (overridable
# for a parent that cannot pin fd numbers).
_in = os.fdopen(int(os.environ.get('CC_WIRE_IN', '3')), 'r',
                encoding='utf-8')
_out = os.fdopen(int(os.environ.get('CC_WIRE_OUT', '4')), 'w',
                 encoding='utf-8')
# Ops that arrived on stdin while a callback was blocked on its cbr.
_parked = []
# Big buffers spill through shared memory (tmpfs on Linux) instead of
# base64: one memcpy per side, no inflation, no JSON bloat.  The dir
# comes from the parent (its private 0700 bridge dir); files are 0600,
# exclusive-create, consumed-and-unlinked per message.
_shm_dir = os.environ.get('CC_PY_SHM_DIR') or (
    '/dev/shm' if os.path.isdir('/dev/shm') else None)
_shm_seq = [0]
_SPILL = 1 << 16


def _shm_write(raw):
    _shm_seq[0] += 1
    path = os.path.join(_shm_dir, 'ccpy-c%d-%d' % (os.getpid(), _shm_seq[0]))
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as f:
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
                # Strict cbr wait — but Promise.all on the parent can
                # pipeline later ops ahead of the reply.  Park those.
                while True:
                    line = _in.readline()
                    if not line:
                        raise RuntimeError('bridge is closed')
                    r = json.loads(line)
                    if 'cbr' in r or ('e' in r and 'op' not in r):
                        if 'e' in r:
                            raise RuntimeError(r['e'])
                        return _decode(r.get('cbr'))
                    _parked.append(r)
            return cb
        if '$nf' in v:
            if v['$nf'] == '-0':
                return -0.0
            return {'inf': math.inf, '-inf': -math.inf}.get(v['$nf'],
                                                            math.nan)
        if '$bi' in v:
            return int(v['$bi'])
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
        return True  # beyond 2^53 tags as {$bi: digits}; never a JSON number
    if isinstance(v, (list, tuple)):
        return all(_plain(x, depth + 1) for x in v)
    if isinstance(v, dict):
        return all(isinstance(k, str) and not k.startswith('$') and
                   _plain(x, depth + 1) for k, x in v.items())
    return False


def _encode_val(v):
    """Wire-encode a value nested inside a cb/cbr payload.

    Unlike `_encode` (top-level reply shape), this returns the naked
    JSON value or a tagged form — callables and other live objects
    cross as {"$h": id}, never as a raw Python object (json.dumps
    would raise).  1-D buffers cross as {"$ta":...}/{"$shm":...}
    so JS callbacks see typed arrays, not opaque handles."""
    if isinstance(v, float):
        if math.copysign(1.0, v) < 0 and v == 0.0:
            return {'$nf': '-0'}
        if not math.isfinite(v):
            return {'$nf': 'inf' if v == math.inf
                    else '-inf' if v == -math.inf else 'nan'}
    if isinstance(v, (list, tuple)):
        return [_encode_val(x) for x in v]
    if isinstance(v, dict):
        return {k: _encode_val(x) for k, x in v.items()}
    if v is None or isinstance(v, bool) or isinstance(v, str):
        return v
    if isinstance(v, int):
        if -(2**53) < v < 2**53:
            return v
        return {'$bi': str(v)}
    if isinstance(v, float):
        return v
    np = _np()
    if np is not None and isinstance(v, np.ndarray) and v.ndim == 1:
        key = _TA_BY_DTYPE.get(str(v.dtype))
        if key is not None:
            raw = np.ascontiguousarray(v).tobytes()
            if len(raw) > _SPILL and _shm_dir:
                return {'$shm': _shm_write(raw), 't': key}
            return {'$ta': key,
                    'b64': base64.b64encode(raw).decode('ascii')}
    try:
        import array as _array
        if isinstance(v, _array.array):
            key = None
            for k, (tc, _) in _TA.items():
                if tc == v.typecode:
                    key = k
                    break
            if key is not None:
                raw = v.tobytes()
                if len(raw) > _SPILL and _shm_dir:
                    return {'$shm': _shm_write(raw), 't': key}
                return {'$ta': key,
                        'b64': base64.b64encode(raw).decode('ascii')}
    except Exception:
        pass
    return {'$h': _put(v)}


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
    # Empty dict is "plain" but must stay a handle: callers use
    # builtins.dict() as an exec/eval namespace and need .get / mutation
    # on the same object.  Non-empty plain dicts of scalars still cross
    # by value (data returns).
    if isinstance(v, dict) and len(v) == 0:
        return {'h': _put(v)}
    if _plain(v):
        return {'v': _encode_val(v)}
    return {'h': _put(v)}


def _tojs_strict(v, path='$', stack=None):
    """Strict JSON-safe materialize for toJS/toJSON — refuse with path."""
    if stack is None:
        stack = []
    oid = id(v)
    if oid in stack:
        raise TypeError(
            'cc-python: toJS: cannot materialize object at %s — '
            'cycle detected (circular reference)' % path)
    if isinstance(v, bool) or v is None or isinstance(v, str):
        return v
    if isinstance(v, int):
        if -(2 ** 53) < v < 2 ** 53:
            return v
        return {'$bi': str(v)}
    if isinstance(v, float):
        if v == 0.0 and math.copysign(1.0, v) < 0:
            return {'$nf': '-0'}
        if not math.isfinite(v):
            return {'$nf': 'inf' if v == math.inf
                    else '-inf' if v == -math.inf else 'nan'}
        return v
    if isinstance(v, dict):
        stack.append(oid)
        try:
            out = {}
            for k, x in v.items():
                if isinstance(k, str):
                    ks = k
                elif isinstance(k, int) and not isinstance(k, bool):
                    ks = str(k)
                else:
                    raise TypeError(
                        'cc-python: toJS: cannot materialize %s at %s — '
                        'dict keys must be str or int'
                        % (type(k).__name__, path))
                child = path + ('.' + ks if ks.isidentifier()
                                else '["%s"]' % ks)
                out[ks] = _tojs_strict(x, child, stack)
            return out
        finally:
            stack.pop()
    if isinstance(v, (list, tuple)):
        stack.append(oid)
        try:
            return [_tojs_strict(x, '%s[%d]' % (path, i), stack)
                    for i, x in enumerate(v)]
        finally:
            stack.pop()
    tn = type(v).__name__
    if tn in ('set', 'frozenset'):
        raise TypeError(
            'cc-python: toJS: cannot materialize %s at %s — '
            'convert with list(...)' % (tn, path))
    if tn in ('bytes', 'bytearray', 'memoryview'):
        raise TypeError(
            'cc-python: toJS: cannot materialize %s at %s — '
            'use toTypedArray() or list(...) for bytes' % (tn, path))
    raise TypeError(
        'cc-python: toJS: cannot materialize %s at %s — '
        'not a JSON-safe scalar, dict, list, or tuple' % (tn, path))


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
        try:
            return _encode(importlib.import_module(req['name']))
        except ModuleNotFoundError as e:
            name = req.get('name', '?')
            return {
                'e': (
                    'ModuleNotFoundError: No module named %r — install into '
                    'this child python (%s), or pass create({ isolated: true, '
                    'python: venvPath })'
                    % (name, sys.executable)
                )
            }
    if op == 'getp':
        return _encode(_walk(req))
    if op == 'callp':
        f = _walk(req)
        args = [_decode(a) for a in req.get('args', [])]
        kw = {k: _decode(v) for k, v in (req.get('kw') or {}).items()}
        return _encode(_run(f(*args, **kw)))
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
    if op == 'tojs':
        try:
            return {'v': _tojs_strict(_walk(req))}
        except TypeError as e:
            return {'e': str(e)}
    if op == 'release':
        _handles.pop(req['h'], None)
        return {'v': len(_handles)}
    if op == 'stats':
        return {'v': len(_handles)}
    if op == 'close':
        return {'v': True}
    return {'e': 'cc-python broker: unknown op ' + repr(op)}


def _next_req():
    if _parked:
        return _parked.pop(0)
    for line in _in:
        line = line.strip()
        if not line:
            continue
        try:
            return json.loads(line)
        except Exception as e:
            _send({'e': 'cc-python broker: bad request: %s' % e})
            continue
    return None


def main():
    while True:
        req = _next_req()
        if req is None:
            break
        try:
            resp = _dispatch(req)
        except Exception as e:
            resp = {'e': '%s: %s' % (type(e).__name__, e)}
        # Replies pair by request id — the parent discards any line
        # without the id it is waiting on (this channel is shared with
        # whatever user code prints).
        if isinstance(req, dict) and 'id' in req:
            resp['id'] = req['id']
        _send(resp)
        if isinstance(req, dict) and req.get('op') == 'close':
            break


if __name__ == '__main__':
    main()
