"""cc-node: JavaScript (and every npm package) from Python.

    import cc_node
    js = cc_node.create()                 # an Isolation Domain: one node child
    _ = js.require('lodash')              # resolved from YOUR cwd's node_modules
    _.chunk([1, 2, 3, 4, 5], 2)           # [[1, 2], [3, 4], [5]]
    js.destroy()

The mirror of the cc-python bridge, same rules pointed the other way:
attribute access is property lookup (methods arrive bound), a call is a
call, plain data (finite numbers, strings, booleans, None, lists and
non-empty dicts of the same) crosses by value; an empty dict/object
stays a live handle (bags keep property access).  Everything else stays
a live handle owned by the domain.  A thenable result is awaited in the
child before the reply, so async package APIs work with nothing extra.
A Python callable passed as an argument becomes a JS function; its
exceptions cross back as JS errors and vice versa, messages intact.
Handles never cross domains; every door after destroy() answers
articulately; destroy is idempotent and `with cc_node.create() as js:`
scopes it.
"""
import array
import atexit
import base64
import json
import math
import os
import shutil
import subprocess
import tempfile

__all__ = ["create", "JsError", "JsHandle", "__version__"]
__version__ = "0.13.0"

# Typed buffers cross as typed arrays; big ones spill through shared
# memory (tmpfs where available) — one memcpy per side, receiver
# consumes-and-unlinks, sender sweeps after the reply.
_TA_TYPECODE = {"f64": "d", "f32": "f", "i32": "i", "i64": "q", "u8": "B"}
_TA_DTYPE = {"f64": "float64", "f32": "float32", "i32": "int32",
             "i64": "int64", "u8": "uint8"}
_TA_BY_TYPECODE = {tc: k for k, tc in _TA_TYPECODE.items()}
_TA_BY_DTYPE = {dt: k for k, dt in _TA_DTYPE.items()}
_SHM_SPILL = 1 << 16
_shm_seq = [0]


def _numpy():
    try:
        import numpy
        return numpy
    except Exception:
        return None


def _shm_base():
    d = os.environ.get("CC_NODE_SHM_DIR")
    if d:
        return d
    return "/dev/shm" if os.path.isdir("/dev/shm") else tempfile.gettempdir()


class JsError(RuntimeError):
    """A JavaScript error, message intact."""


_live = []


def _atexit():
    for b in list(_live):
        try:
            b.destroy()
        except Exception:
            pass


atexit.register(_atexit)


class JsHandle:
    """A live JavaScript value owned by its domain."""

    __slots__ = ("_d", "_h")

    def __init__(self, domain, hid):
        object.__setattr__(self, "_d", domain)
        object.__setattr__(self, "_h", hid)

    def __getattr__(self, name):
        return self._d._req("get", h=self._h, name=name)

    def __call__(self, *args):
        return self._d._req("call", h=self._h,
                            args=[self._d._encode(a) for a in args])

    def __str__(self):
        return self._d._req("str", h=self._h)

    def __repr__(self):
        return "<JsHandle #%d%s>" % (self._h,
                                     " (closed)" if self._d.closed else "")

    def __del__(self):
        # Never nest a sync wire op from GC into an in-flight _req — that
        # steals the outer reply (e.g. returning a callback arg handle).
        try:
            if not self._d.closed:
                self._d._queue_release(self._h)
        except Exception:
            pass


class Bridge:
    """The domain: one spawned node child, one handle table, one wire."""

    def __init__(self, node=None):
        broker = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "broker.cjs")
        node = node or os.environ.get("CC_NODE_BIN", "node")
        # Spills live in a private 0700 per-bridge directory (predictable
        # names in a shared /dev/shm invite pre-creation races and
        # umask-dependent exposure); the child writes its spills there
        # too, and the directory goes with the bridge.
        self._shm_dir = tempfile.mkdtemp(
            prefix="ccnode-%d-" % os.getpid(), dir=_shm_base())
        # The wire lives on dedicated fds; stdio is inherited, so
        # console.log in evaluated JS reaches the real stdout and can
        # never collide with a protocol reply.  pass_fds keeps our fd
        # numbers in the child, so the broker learns them from the env.
        req_r, req_w = os.pipe()
        resp_r, resp_w = os.pipe()
        env = dict(os.environ,
                   CC_WIRE_IN=str(req_r), CC_WIRE_OUT=str(resp_w),
                   CC_NODE_SHM_DIR=self._shm_dir)
        try:
            self._p = subprocess.Popen(
                [node, broker],
                pass_fds=(req_r, resp_w),
                env=env,
                cwd=os.getcwd(),
            )
        except FileNotFoundError:
            for fd in (req_r, req_w, resp_r, resp_w):
                os.close(fd)
            shutil.rmtree(self._shm_dir, ignore_errors=True)
            raise JsError(
                "cc-node: no node executable (install Node, or set "
                "CC_NODE_BIN)") from None
        os.close(req_r)
        os.close(resp_w)
        self._wire_w = os.fdopen(req_w, "wb")
        self._wire_r = os.fdopen(resp_r, "rb")
        self.closed = False
        self._nid = 1
        self._cbs = {}
        self._ncb = 1
        self._shm_out = []
        self._depth = 0
        self._parked = {}
        self._pending_release = []
        self._close_pending = False
        self._destroy_done = False
        _live.append(self)

    # ---- wire ----

    def _send(self, obj):
        line = json.dumps(obj, allow_nan=False) + "\n"
        try:
            self._wire_w.write(line.encode("utf-8"))
            self._wire_w.flush()
        except (BrokenPipeError, ValueError, OSError):
            # The child died under this write (e.g. killed from inside a
            # callback).  Every door answers the same way after death.
            self.closed = True
            raise JsError("cc-node: the node child exited") from None

    def _queue_release(self, hid):
        self._pending_release.append(hid)
        if self._depth == 0:
            self._flush_releases()

    def _flush_releases(self):
        while self._pending_release and not self.closed:
            hid = self._pending_release.pop(0)
            try:
                self._req("release", h=hid)
            except Exception:
                pass

    def _shm_write(self, raw):
        _shm_seq[0] += 1
        path = os.path.join(self._shm_dir, "s%d" % _shm_seq[0])
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "wb") as f:
            f.write(raw)
        return path

    def _flush_shm(self):
        # The child unlinks spill files as it decodes; this sweep only
        # matters when it died first (ENOENT is the normal case).
        for path in self._shm_out:
            try:
                os.unlink(path)
            except OSError:
                pass
        del self._shm_out[:]

    def _take_reply(self, msg):
        if "e" in msg:
            raise JsError(msg["e"])
        return self._decode_result(msg)

    def _wait_reply(self, rid):
        parked = self._parked.pop(rid, None)
        if parked is not None:
            return self._take_reply(parked)
        while True:
            line = self._wire_r.readline()
            if not line:
                self.closed = True
                raise JsError("cc-node: the node child exited")
            msg = json.loads(line)
            if "cb" in msg:
                self._serve_callback(msg)
                continue
            mid = msg.get("id")
            if mid == rid:
                return self._take_reply(msg)
            if mid is not None:
                # Nested _req (GC release, etc.) can overtake; park by id.
                self._parked[mid] = msg
                continue
            raise JsError("cc-node: protocol violation (unexpected reply)")

    def _req(self, op, **kw):
        if self.closed:
            raise JsError("cc-node: bridge is closed")
        rid = self._nid
        self._nid += 1
        kw["id"] = rid
        kw["op"] = op
        self._depth += 1
        try:
            try:
                self._send(kw)
            except (BrokenPipeError, ValueError):
                self.closed = True
                raise JsError("cc-node: the node child exited") from None
            return self._wait_reply(rid)
        finally:
            self._depth -= 1
            if self._depth == 0:
                self._flush_shm()
                if self._close_pending:
                    self._finish_destroy()
                else:
                    self._flush_releases()

    def _serve_callback(self, msg):
        fn = self._cbs.get(msg["cb"])
        try:
            if fn is None:
                raise JsError("cc-node: unknown callback")
            args = [self._decode_result(a) for a in msg["args"]]
            self._send({"cbr": msg["cbid"], "v": self._encode(fn(*args))})
        except Exception as e:  # crosses back as a JS error, message intact
            try:
                self._send({"cbr": msg["cbid"], "e": str(e)})
            except Exception:
                pass  # child gone; the in-flight op surfaces EOF next read

    # ---- values ----

    def _encode(self, a):
        if isinstance(a, JsHandle):
            if a._d is not self:
                raise JsError("cc-node: handle belongs to another bridge")
            return {"$h": a._h}
        if callable(a):
            fid = self._ncb
            self._ncb += 1
            self._cbs[fid] = a
            return {"$f": fid}
        if a is None or isinstance(a, (bool, int, str)):
            return a
        if isinstance(a, float):
            if math.isnan(a):
                return {"$nf": "nan"}
            if math.isinf(a):
                return {"$nf": "inf" if a > 0 else "-inf"}
            return a
        if isinstance(a, (list, tuple)):
            return [self._encode(x) for x in a]
        if isinstance(a, dict):
            out = {}
            for k, v in a.items():
                if not isinstance(k, str):
                    raise JsError("cc-node: dict keys must be strings")
                if k.startswith("$"):
                    raise JsError("cc-node: dict keys starting with '$' are "
                                  "reserved on the wire")
                out[k] = self._encode(v)
            return out
        enc = self._encode_buffer(a)
        if enc is not None:
            return enc
        raise JsError(
            "cc-node: unsupported argument type: %r "
            "(numbers, str, bool, None, list/tuple, dict with str keys, "
            "bytes/array.array/1-D numpy, callables, or this domain's "
            "JsHandle — same-domain handles chain)"
            % (type(a),))

    def _encode_buffer(self, a):
        # Typed buffers cross as typed arrays: bytes/array.array/1-D
        # numpy — small inline as base64, big through the shared-memory
        # spill (one memcpy per side; the receiver consumes-and-unlinks).
        raw = None
        kind = None
        if isinstance(a, (bytes, bytearray, memoryview)):
            raw, kind = bytes(a), "u8"
        elif isinstance(a, array.array):
            kind = _TA_BY_TYPECODE.get(a.typecode)
            if kind is not None:
                raw = a.tobytes()
        else:
            np = _numpy()
            if np is not None and isinstance(a, np.ndarray) and a.ndim == 1:
                kind = _TA_BY_DTYPE.get(str(a.dtype))
                if kind is not None:
                    raw = np.ascontiguousarray(a).tobytes()
        if raw is None or kind is None:
            return None
        if len(raw) > _SHM_SPILL:
            path = self._shm_write(raw)
            self._shm_out.append(path)
            return {"$shm": path, "t": kind}
        return {"$ta": kind, "b64": base64.b64encode(raw).decode("ascii")}

    def _decode_buffer(self, msg):
        if "shm" in msg:
            path = msg["shm"]
            try:
                with open(path, "rb") as f:
                    raw = f.read()
            finally:
                try:
                    os.unlink(path)
                except OSError:
                    pass
        else:
            raw = base64.b64decode(msg["b64"])
        kind = msg.get("t") or msg.get("ta")
        np = _numpy()
        if np is not None:
            return np.frombuffer(raw, dtype=_TA_DTYPE[kind]).copy()
        a = array.array(_TA_TYPECODE[kind])
        a.frombytes(raw)
        return a

    def _decode_result(self, msg):
        if "u" in msg:
            return None
        if "h" in msg:
            return JsHandle(self, msg["h"])
        if "nf" in msg:
            return {"nan": math.nan, "inf": math.inf,
                    "-inf": -math.inf}[msg["nf"]]
        if "ta" in msg or "shm" in msg:
            return self._decode_buffer(msg)
        return msg.get("v")

    # ---- surface ----

    def require(self, name):
        return self._req("require", name=name)

    def import_module(self, name):
        return self._req("import", name=name)

    def eval(self, src):
        return self._req("eval", src=src)

    def release(self, handle):
        if not isinstance(handle, JsHandle) or handle._d is not self:
            raise JsError("cc-node: handle belongs to another bridge")
        return self._req("release", h=handle._h)

    def stats(self):
        return self._req("stats")

    def destroy(self):
        """Idempotent teardown.  Nested destroy (e.g. from a Python
        callback while the broker waits on cbr) defers the farewell
        close until the in-flight wire op unwinds — nesting close into
        the cbr slot hangs the child."""
        if self._destroy_done:
            return
        if self._depth > 0:
            self.closed = True
            self._close_pending = True
            return
        self._finish_destroy()

    def _finish_destroy(self):
        if self._destroy_done:
            return
        self._destroy_done = True
        self._close_pending = False
        self.closed = True
        try:
            if self._p.poll() is None and not self._wire_w.closed:
                rid = self._nid
                self._nid += 1
                self._send({"id": rid, "op": "close"})
        except Exception:
            pass
        try:
            self._wire_w.close()
        except Exception:
            pass
        # Drain to EOF so the broker's farewell reply has somewhere to
        # land — closing the reply fd under its write is an EPIPE crash
        # in the child.
        try:
            while self._wire_r.readline():
                pass
        except Exception:
            pass
        try:
            self._p.wait(timeout=5)
        except Exception:
            try:
                self._p.kill()
            except Exception:
                pass
        try:
            self._wire_r.close()
        except Exception:
            pass
        shutil.rmtree(self._shm_dir, ignore_errors=True)
        if self in _live:
            _live.remove(self)

    close = destroy

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.destroy()
        return False


def create(node=None):
    return Bridge(node=node)
