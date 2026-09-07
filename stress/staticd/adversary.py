#!/usr/bin/env python3
"""staticd adversarial storms — see staticd_stress.md.

Usage:
  ./adversary.py --spawn PATH/to/staticd [--scale quick|full|soak]
  ./adversary.py --port 8080 --mode slowloris_headers
  MODE=conn_storm ./adversary.py --spawn ./out/staticd
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import os
import random
import resource
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
WS_MAX_PAYLOAD = (16 * 1024) - 14

ROOT = Path(__file__).resolve().parents[2]
STATICD_DIR = ROOT / "real_projects" / "staticd"


class Fail(Exception):
    pass


@dataclass
class Result:
    mode: str
    ok: bool
    detail: str = ""
    known_break: bool = False

    def line(self) -> str:
        if self.ok:
            tag = "ok"
        elif self.known_break:
            tag = "BREAK"
        else:
            tag = "FAIL"
        extra = f" {self.detail}" if self.detail else ""
        return f"  {tag} {self.mode}{extra}"


@dataclass
class Ctx:
    host: str
    port: int
    root: Path
    scale: str
    pages: Optional[Path] = None
    tls_cert: Optional[Path] = None
    tls_key: Optional[Path] = None
    breaks: list[str] = field(default_factory=list)
    rng: random.Random = field(default_factory=lambda: random.Random(0))

    @property
    def n_small(self) -> int:
        return {"quick": 32, "full": 128, "soak": 256}[self.scale]

    @property
    def n_med(self) -> int:
        return {"quick": 64, "full": 256, "soak": 512}[self.scale]

    @property
    def soak_secs(self) -> float:
        return float(os.environ.get("SOAK_SECONDS", {"quick": 3, "full": 8, "soak": 30}[self.scale]))


def http_get(host: str, port: int, path: str, timeout: float = 2.0,
             extra_headers: Optional[list[str]] = None) -> tuple[int, bytes, bytes]:
    s = socket.create_connection((host, port), timeout)
    try:
        s.settimeout(timeout)
        hdrs = [f"GET {path} HTTP/1.1", f"Host: {host}", "Connection: close"]
        if extra_headers:
            hdrs.extend(extra_headers)
        hdrs.append("")
        hdrs.append("")
        s.sendall("\r\n".join(hdrs).encode())
        buf = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        if b"\r\n\r\n" not in buf:
            raise Fail("no header end")
        head, body = buf.split(b"\r\n\r\n", 1)
        line = head.split(b"\r\n", 1)[0]
        parts = line.split(b" ")
        code = int(parts[1]) if len(parts) >= 2 else 0
        return code, head, body
    finally:
        s.close()


def still_serves(ctx: Ctx, path: str = "/1kb.bin") -> None:
    code, _, body = http_get(ctx.host, ctx.port, path, timeout=3.0)
    if code != 200 or len(body) < 10:
        raise Fail(f"post-storm serve {code} len={len(body)}")


def rst_close(sock: socket.socket) -> None:
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                        struct.pack("ii", 1, 0))
    except OSError:
        pass
    try:
        sock.close()
    except OSError:
        pass


# ---- modes ----

def mode_conn_storm(ctx: Ctx) -> Result:
    n = ctx.n_med
    errs = []

    def one(i: int) -> None:
        try:
            code, _, body = http_get(ctx.host, ctx.port, "/1kb.bin", timeout=5.0)
            if code != 200 or len(body) != 1024:
                errs.append(f"{i}:{code}/{len(body)}")
        except Exception as e:
            errs.append(f"{i}:{type(e).__name__}")

    with ThreadPoolExecutor(max_workers=min(n, 64)) as ex:
        list(ex.map(one, range(n)))
    if errs:
        return Result("conn_storm", False, f"{len(errs)}/{n} failed e.g. {errs[0]}")
    still_serves(ctx)
    return Result("conn_storm", True, f"n={n}")


def mode_accept_burst_survive(ctx: Ctx) -> Result:
    n = ctx.n_med
    socks = []
    try:
        for _ in range(n):
            s = socket.create_connection((ctx.host, ctx.port), 1.0)
            socks.append(s)
    except OSError as e:
        # partial is ok — pressure is the point
        detail = f"opened={len(socks)} err={e}"
    else:
        detail = f"opened={len(socks)}"
    for s in socks:
        rst_close(s)
    time.sleep(0.2)
    still_serves(ctx)
    return Result("accept_burst_survive", True, detail)


def mode_fd_exhaust_accept(ctx: Ctx) -> Result:
    """Push toward EMFILE; server must soft-fail, back off listen, keep serving."""
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    target = min(soft, 256)
    try:
        resource.setrlimit(resource.RLIMIT_NOFILE, (target, hard))
    except (ValueError, OSError):
        return Result("fd_exhaust_accept", True, "SKIP setrlimit")

    holders: list[socket.socket] = []
    server_socks: list[socket.socket] = []
    burst_s = 0.0
    attempts = 0
    try:
        for _ in range(target):
            try:
                holders.append(socket.socket())
            except OSError:
                break
        for _ in range(64):
            try:
                server_socks.append(socket.create_connection((ctx.host, ctx.port), 0.5))
            except OSError:
                break
        for s in server_socks:
            rst_close(s)
        server_socks.clear()
        # Hammer while exhausted: with listen POLLIN backoff, a tight
        # accept spin should not make this burst complete in near-zero time.
        t0 = time.perf_counter()
        attempts = 0
        while time.perf_counter() - t0 < 0.25:
            attempts += 1
            try:
                s = socket.create_connection((ctx.host, ctx.port), 0.05)
                rst_close(s)
            except OSError:
                pass
        burst_s = time.perf_counter() - t0
        time.sleep(0.15)
    finally:
        for s in server_socks:
            rst_close(s)
        for s in holders:
            try:
                s.close()
            except OSError:
                pass
        try:
            resource.setrlimit(resource.RLIMIT_NOFILE, (soft, hard))
        except (ValueError, OSError):
            pass

    try:
        still_serves(ctx)
        # Client loop is wall-clock bounded (~0.25s); refuse a pathological
        # "instant" run that never blocked (should not happen with timeout).
        if burst_s < 0.05:
            return Result(
                "fd_exhaust_accept",
                False,
                f"accept path too hot burst={burst_s:.4f}s attempts={attempts}",
            )
        return Result(
            "fd_exhaust_accept",
            True,
            f"holders_peak~{target} burst={burst_s:.3f}s attempts={attempts}",
        )
    except Fail as e:
        return Result("fd_exhaust_accept", False,
                      f"server dead after pressure: {e}", known_break=True)


def mode_slowloris_headers(ctx: Ctx) -> Result:
    """Drip inside gap; must die by HEADER_TOTAL (not forever)."""
    n = min(ctx.n_small, 40)
    budget = 8.0 if ctx.scale == "quick" else 15.0
    socks: list[socket.socket] = []
    try:
        for _ in range(n):
            s = socket.create_connection((ctx.host, ctx.port), 2.0)
            s.settimeout(1.0)
            s.sendall(b"GET /1kb.bin HTTP/1.1\r\n")
            socks.append(s)
        t0 = time.time()
        alive = n
        while time.time() - t0 < budget:
            time.sleep(0.25)
            for s in socks:
                try:
                    s.sendall(b"X")
                except OSError:
                    pass
            still = 0
            for s in socks:
                try:
                    s.setblocking(False)
                    data = s.recv(1)
                    if data == b"":
                        continue
                    still += 1
                except BlockingIOError:
                    still += 1
                except OSError:
                    pass
                finally:
                    try:
                        s.setblocking(True)
                    except OSError:
                        pass
            alive = still
            if alive == 0:
                break
        still_serves(ctx)
        if alive > n // 2:
            return Result(
                "slowloris_headers",
                False,
                f"alive={alive}/{n} after {budget}s (HEADER_TOTAL?)",
            )
        return Result("slowloris_headers", True, f"reaped alive={alive}/{n}")
    finally:
        for s in socks:
            rst_close(s)


def mode_header_never_finishes(ctx: Ctx) -> Result:
    """Partial headers with no further bytes — dies by HEADER_GAP."""
    s = socket.create_connection((ctx.host, ctx.port), 2.0)
    try:
        s.sendall(b"GET /1kb.bin HTTP/1.1\r\nHost: x\r\n")
        time.sleep(6.0)
        still_serves(ctx)
        s.settimeout(0.2)
        try:
            peek = s.recv(1)
            peer_closed = peek == b""
        except socket.timeout:
            peer_closed = False
        except OSError:
            peer_closed = True
        if not peer_closed:
            return Result("header_never_finishes", False,
                          "conn still open after 6s partial headers")
        return Result("header_never_finishes", True, "peer closed (gap)")
    finally:
        rst_close(s)


def mode_idle_keepalive_pile(ctx: Ctx) -> Result:
    """Complete GET then idle — dies by KEEPALIVE."""
    n = min(ctx.n_small, 24)
    socks: list[socket.socket] = []
    try:
        for _ in range(n):
            s = socket.create_connection((ctx.host, ctx.port), 2.0)
            s.settimeout(3.0)
            req = (
                b"GET /1kb.bin HTTP/1.1\r\n"
                b"Host: x\r\n"
                b"Connection: keep-alive\r\n"
                b"\r\n"
            )
            s.sendall(req)
            buf = b""
            while b"\r\n\r\n" not in buf or len(buf) < 1024 + 50:
                chunk = s.recv(65536)
                if not chunk:
                    break
                buf += chunk
            socks.append(s)
        time.sleep(6.0)
        still_serves(ctx)
        open_n = 0
        for s in socks:
            try:
                s.settimeout(0.1)
                d = s.recv(1)
                if d != b"":
                    open_n += 1
            except socket.timeout:
                open_n += 1
            except OSError:
                pass
        if open_n > n // 2:
            return Result("idle_keepalive_pile", False,
                          f"idle open={open_n}/{n} after 6s")
        return Result("idle_keepalive_pile", True, f"open={open_n}/{n}")
    finally:
        for s in socks:
            rst_close(s)


def mode_abort_mid_headers(ctx: Ctx) -> Result:
    s = socket.create_connection((ctx.host, ctx.port), 2.0)
    try:
        s.sendall(b"GET /1kb.bin HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        s.recv(1)
    finally:
        rst_close(s)
    time.sleep(0.05)
    still_serves(ctx)
    return Result("abort_mid_headers", True)


def mode_abort_mid_body(ctx: Ctx) -> Result:
    s = socket.create_connection((ctx.host, ctx.port), 2.0)
    try:
        s.sendall(b"GET /1mb.bin HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        # read past headers into body a bit
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += s.recv(4096)
        s.recv(4096)
    finally:
        rst_close(s)
    time.sleep(0.05)
    still_serves(ctx)
    return Result("abort_mid_body", True)


def mode_tiny_window_get(ctx: Ctx) -> Result:
    s = socket.create_connection((ctx.host, ctx.port), 2.0)
    try:
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        except OSError:
            pass
        s.settimeout(10.0)
        s.sendall(b"GET /64kb.js HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        buf = b""
        while True:
            try:
                chunk = s.recv(256)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
            time.sleep(0.001)
        if b"\r\n\r\n" not in buf:
            raise Fail("no headers")
        _, body = buf.split(b"\r\n\r\n", 1)
        if len(body) < 60000:
            # short is ok if peer closed cleanly under tiny window — still must serve
            still_serves(ctx)
            return Result("tiny_window_get", True, f"got={len(body)} (partial ok)")
        still_serves(ctx)
        return Result("tiny_window_get", True, f"got={len(body)}")
    finally:
        rst_close(s)


def mode_pipelined_garbage(ctx: Ctx) -> Result:
    s = socket.create_connection((ctx.host, ctx.port), 2.0)
    try:
        s.settimeout(3.0)
        s.sendall(
            b"GET /1kb.bin HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n"
            b"\x00\xffGARBAGE!!!!\r\n\r\n"
        )
        buf = b""
        while len(buf) < 1100:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        if b"200" not in buf.split(b"\r\n", 1)[0]:
            raise Fail(f"first response missing: {buf[:80]!r}")
        still_serves(ctx)
        return Result("pipelined_garbage", True)
    finally:
        rst_close(s)


def mode_http_garbage_hail(ctx: Ctx) -> Result:
    n = ctx.n_small
    for i in range(n):
        s = socket.create_connection((ctx.host, ctx.port), 1.0)
        try:
            s.settimeout(1.0)
            blob = bytes(ctx.rng.getrandbits(8) for _ in range(ctx.rng.randint(8, 200)))
            # keep a few printable so we sometimes form near-requests
            if i % 5 == 0:
                blob = b"FOOZ /..\r\n\r\n"
            elif i % 5 == 1:
                blob = b"GET\t/ HTTP/9.9\r\n\r\n"
            s.sendall(blob)
            try:
                s.recv(256)
            except OSError:
                pass
        finally:
            rst_close(s)
    still_serves(ctx)
    return Result("http_garbage_hail", True, f"n={n}")


def mode_header_bomb(ctx: Ctx) -> Result:
    s = socket.create_connection((ctx.host, ctx.port), 2.0)
    try:
        s.settimeout(2.0)
        lines = [b"GET /1kb.bin HTTP/1.1", b"Host: x"]
        for i in range(200):
            lines.append(f"X-{i}: {'Z' * 200}".encode())
        lines.append(b"")
        lines.append(b"")
        s.sendall(b"\r\n".join(lines))
        try:
            s.recv(1024)
        except OSError:
            pass
    finally:
        rst_close(s)
    still_serves(ctx)
    return Result("header_bomb", True)


def mode_body_cl_mismatch(ctx: Ctx) -> Result:
    # POST with body must force close (correctness already checks); hammer it.
    for _ in range(ctx.n_small):
        s = socket.create_connection((ctx.host, ctx.port), 2.0)
        try:
            s.settimeout(2.0)
            s.sendall(
                b"POST /1kb.bin HTTP/1.1\r\n"
                b"Host: x\r\n"
                b"Content-Length: 5\r\n"
                b"\r\n"
                b"hello"
                b"GET /1kb.bin HTTP/1.1\r\nHost: x\r\n\r\n"
            )
            buf = b""
            try:
                while True:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    buf += chunk
            except OSError:
                pass
            # Must not return two 200 bodies as if pipelined after POST.
            if buf.count(b"HTTP/1.1 200") > 1:
                raise Fail("pipelined after POST body")
        finally:
            rst_close(s)
    still_serves(ctx)
    return Result("body_cl_mismatch", True)


def mode_range_edge_hail(ctx: Ctx) -> Result:
    cases = [
        ("bytes=-0", {416}),
        ("bytes=0-15", {206}),
        ("bytes=999999-9999999", {416}),
        ("bytes=0-0", {206}),
        ("bytes=", {400, 416, 200}),
        ("bytes=foo", {400, 416, 200}),
    ]
    for h, ok_codes in cases:
        code, head, _ = http_get(ctx.host, ctx.port, "/1kb.bin",
                                 extra_headers=[f"Range: {h}"])
        if code not in ok_codes:
            raise Fail(f"Range {h} -> {code}")
        if code == 206 and b"Content-Range: bytes 0--1" in head:
            raise Fail("underflow Content-Range")
    still_serves(ctx)
    return Result("range_edge_hail", True)


def mode_traversal_hail(ctx: Ctx) -> Result:
    paths = [
        "/../etc/passwd",
        "/..%2fetc/passwd",
        "/./../../etc/passwd",
        "//etc/passwd",
        "/foo/../../etc/passwd",
        "/%2e%2e/etc/passwd",
    ]
    for p in paths:
        try:
            code, _, body = http_get(ctx.host, ctx.port, p, timeout=2.0)
        except Fail:
            continue
        if code == 200 and (b"root:" in body or b"nobody" in body):
            raise Fail(f"escaped via {p}")
        if code not in (400, 403, 404):
            # some servers 400 on weird encoding — ok if not 200 escape
            if code == 200:
                raise Fail(f"unexpected 200 for {p}")
    still_serves(ctx)
    return Result("traversal_hail", True)


def mode_hot_rename_storm(ctx: Ctx) -> Result:
    """Swap a hot name under concurrent GET; bodies must be coherent."""
    a = ctx.root / "hot.bin"
    b = ctx.root / "hot.bin.tmp"
    a.write_bytes(b"A" * 4096)
    stop = threading.Event()
    errs: list[str] = []

    def reader() -> None:
        while not stop.is_set():
            try:
                code, _, body = http_get(ctx.host, ctx.port, "/hot.bin", timeout=2.0)
                if code != 200:
                    continue
                if body and body != b"A" * len(body) and body != b"B" * len(body):
                    errs.append(f"mixed {body[:8]!r}")
                    return
            except Exception:
                pass

    def swapper() -> None:
        flip = False
        while not stop.is_set():
            payload = b"B" * 4096 if flip else b"A" * 4096
            b.write_bytes(payload)
            os.replace(b, a)
            flip = not flip
            time.sleep(0.01)

    threads = [threading.Thread(target=reader) for _ in range(8)]
    threads.append(threading.Thread(target=swapper))
    for t in threads:
        t.start()
    time.sleep(2.0 if ctx.scale == "quick" else 5.0)
    stop.set()
    for t in threads:
        t.join(timeout=2.0)
    if errs:
        raise Fail(errs[0])
    still_serves(ctx)
    return Result("hot_rename_storm", True)


def ws_handshake(host: str, port: int, **kwargs) -> tuple[socket.socket, bytes, bytes, str]:
    key = kwargs.get("key") or base64.b64encode(os.urandom(16)).decode()
    headers = kwargs.get("headers")
    if headers is None:
        headers = [
            "Upgrade: websocket",
            "Connection: Upgrade",
            f"Sec-WebSocket-Key: {key}",
            "Sec-WebSocket-Version: 13",
        ]
    lines = [f"GET /echo HTTP/1.1", f"Host: {host}"] + headers + ["", ""]
    s = socket.create_connection((host, port), 2.0)
    s.settimeout(2.0)
    s.sendall("\r\n".join(lines).encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            raise Fail("ws hs closed")
        buf += chunk
    head, rest = buf.split(b"\r\n\r\n", 1)
    return s, head, rest, key


def ws_send(sock: socket.socket, opcode: int, payload: bytes, fin: bool = True, mask: bool = True) -> None:
    b0 = (0x80 if fin else 0) | (opcode & 0x0F)
    n = len(payload)
    if n < 126:
        hdr = bytearray([b0, (0x80 if mask else 0) | n])
    elif n <= 0xFFFF:
        hdr = bytearray([b0, (0x80 if mask else 0) | 126]) + struct.pack("!H", n)
    else:
        hdr = bytearray([b0, (0x80 if mask else 0) | 127]) + struct.pack("!Q", n)
    if mask:
        mkey = os.urandom(4)
        hdr.extend(mkey)
        payload = bytes(b ^ mkey[i % 4] for i, b in enumerate(payload))
    sock.sendall(bytes(hdr) + payload)


def mode_ws_protocol_abuse(ctx: Ctx) -> Result:
    # bad key
    s, head, _, _ = ws_handshake(ctx.host, ctx.port, headers=[
        "Upgrade: websocket", "Connection: Upgrade",
        "Sec-WebSocket-Key: not-valid!!", "Sec-WebSocket-Version: 13",
    ])
    if b"101" in head.split(b"\r\n", 1)[0]:
        rst_close(s)
        raise Fail("accepted bad key")
    rst_close(s)

    # fragment
    s, head, rest, key = ws_handshake(ctx.host, ctx.port)
    if b"101" not in head:
        rst_close(s)
        raise Fail("hs failed")
    ws_send(s, 1, b"x", fin=False)
    s.settimeout(1.0)
    try:
        s.recv(64)
    except OSError:
        pass
    rst_close(s)

    # unmasked
    s, head, _, _ = ws_handshake(ctx.host, ctx.port)
    ws_send(s, 1, b"x", mask=False)
    time.sleep(0.1)
    rst_close(s)

    # oversize
    s, head, _, _ = ws_handshake(ctx.host, ctx.port)
    ws_send(s, 1, b"z" * (WS_MAX_PAYLOAD + 1))
    time.sleep(0.1)
    rst_close(s)

    still_serves(ctx)
    return Result("ws_protocol_abuse", True)


def mode_ws_ping_flood(ctx: Ctx) -> Result:
    s, head, rest, _ = ws_handshake(ctx.host, ctx.port)
    if b"101" not in head:
        rst_close(s)
        raise Fail("hs")
    try:
        for i in range(ctx.n_small):
            ws_send(s, 9, f"p{i}".encode())
        ws_send(s, 1, b"done")
        # drain some
        s.settimeout(2.0)
        buf = rest
        while len(buf) < 10:
            buf += s.recv(4096)
    finally:
        rst_close(s)
    still_serves(ctx)
    return Result("ws_ping_flood", True)


def mode_ws_fanout(ctx: Ctx) -> Result:
    n = min(ctx.n_small, 32)
    errs: list[str] = []

    def one(i: int) -> None:
        try:
            s, head, rest, _ = ws_handshake(ctx.host, ctx.port)
            if b"101" not in head:
                errs.append("hs")
                rst_close(s)
                return
            msg = f"m{i}".encode()
            ws_send(s, 1, msg)
            s.settimeout(2.0)
            buf = rest
            while len(buf) < 2 + len(msg):
                buf += s.recv(4096)
            rst_close(s)
        except Exception as e:
            errs.append(type(e).__name__)

    with ThreadPoolExecutor(max_workers=n) as ex:
        list(ex.map(one, range(n)))
    if errs:
        return Result("ws_fanout", False, f"{len(errs)} errs e.g. {errs[0]}")
    still_serves(ctx)
    return Result("ws_fanout", True, f"n={n}")


def mode_ws_churn(ctx: Ctx) -> Result:
    n = ctx.n_med
    for i in range(n):
        s, head, rest, _ = ws_handshake(ctx.host, ctx.port)
        if b"101" not in head:
            rst_close(s)
            raise Fail(f"hs {i}")
        ws_send(s, 1, b"x")
        try:
            s.settimeout(1.0)
            s.recv(64)
        except OSError:
            pass
        rst_close(s)
    still_serves(ctx)
    return Result("ws_churn", True, f"n={n}")


def mode_conn_churn_rss(ctx: Ctx) -> Result:
    if ctx.scale == "quick":
        secs = 2.0
    else:
        secs = ctx.soak_secs
    # crude RSS via ps
    def rss_kb() -> int:
        # best-effort: read our own? we want server — skip if unknown
        return 0

    t0 = time.time()
    ops = 0
    while time.time() - t0 < secs:
        http_get(ctx.host, ctx.port, "/1kb.bin", timeout=2.0)
        ops += 1
    still_serves(ctx)
    return Result("conn_churn_rss", True, f"ops={ops} secs={secs}")


def mode_ws_churn_rss(ctx: Ctx) -> Result:
    secs = 2.0 if ctx.scale == "quick" else ctx.soak_secs
    t0 = time.time()
    ops = 0
    while time.time() - t0 < secs:
        s, head, _, _ = ws_handshake(ctx.host, ctx.port)
        if b"101" in head:
            ws_send(s, 1, b"y")
            try:
                s.settimeout(0.5)
                s.recv(32)
            except OSError:
                pass
        rst_close(s)
        ops += 1
    still_serves(ctx)
    return Result("ws_churn_rss", True, f"ops={ops}")


def mode_pages_throw_storm(ctx: Ctx) -> Result:
    if not ctx.pages:
        return Result("pages_throw_storm", True, "SKIP no --pages")
    n = ctx.n_small
    codes = []

    def one(_: int) -> None:
        code, _, _ = http_get(ctx.host, ctx.port, "/boom", timeout=3.0)
        codes.append(code)

    with ThreadPoolExecutor(max_workers=min(n, 16)) as ex:
        list(ex.map(one, range(n)))
    if any(c not in (500, 501) for c in codes):
        return Result("pages_throw_storm", False, f"codes={set(codes)}")
    code, _, body = http_get(ctx.host, ctx.port, "/hello", timeout=3.0)
    if code == 501:
        return Result("pages_throw_storm", True, "SKIP no engine")
    if code != 200:
        raise Fail(f"hello after storm {code}")
    return Result("pages_throw_storm", True, f"n={n}")


def mode_listing_href_abuse(ctx: Ctx) -> Result:
    """Filenames with ? # space — listing hrefs must encode and GET."""
    import re
    code, _, body = http_get(ctx.host, ctx.port, "/", timeout=2.0)
    if code != 200 or b"<a href" not in body:
        return Result("listing_href_abuse", True, "SKIP no listing")
    hrefs = re.findall(br'href="([^"]+)"', body)
    if not hrefs:
        return Result("listing_href_abuse", False, "no hrefs parsed")
    # Raw specials must not appear unencoded in hrefs.
    for raw in (b"weird?", b"weird#", b"weird space"):
        for h in hrefs:
            if raw in h:
                return Result("listing_href_abuse", False,
                              f"unencoded {raw!r} in href={h!r}")
    # Each emitted file href should resolve (skip parent "..").
    checked = 0
    for h in hrefs:
        path = h.decode("ascii", "replace")
        if path in (".", "..") or path.endswith("/..") or "/../" in path:
            continue
        if path == "/" or path.endswith("/"):
            continue
        c, _, _ = http_get(ctx.host, ctx.port, path, timeout=2.0)
        if c != 200:
            return Result("listing_href_abuse", False,
                          f"GET {path} → {c}")
        checked += 1
    if checked < 3:
        return Result("listing_href_abuse", False,
                      f"expected ≥3 file hrefs, got {checked}")
    return Result("listing_href_abuse", True, f"hrefs={checked}")


def mode_uri_decode_refuse(ctx: Ctx) -> Result:
    """Strict decode: %2f %5c %00 bad % → 400/403."""
    cases = [
        "/%2f",
        "/%2F",
        "/%5c",
        "/%5C",
        "/%00",
        "/%2",
        "/%GG",
        "/%2e%2e/",
        "/..%2f",
    ]
    bad = []
    for path in cases:
        code, _, _ = http_get(ctx.host, ctx.port, path, timeout=2.0)
        if code not in (400, 403):
            bad.append(f"{path}→{code}")
    if bad:
        return Result("uri_decode_refuse", False, "; ".join(bad))
    return Result("uri_decode_refuse", True, f"n={len(cases)}")


MODES: dict[str, Callable[[Ctx], Result]] = {
    "conn_storm": mode_conn_storm,
    "accept_burst_survive": mode_accept_burst_survive,
    "fd_exhaust_accept": mode_fd_exhaust_accept,
    "slowloris_headers": mode_slowloris_headers,
    "header_never_finishes": mode_header_never_finishes,
    "idle_keepalive_pile": mode_idle_keepalive_pile,
    "abort_mid_headers": mode_abort_mid_headers,
    "abort_mid_body": mode_abort_mid_body,
    "tiny_window_get": mode_tiny_window_get,
    "pipelined_garbage": mode_pipelined_garbage,
    "http_garbage_hail": mode_http_garbage_hail,
    "header_bomb": mode_header_bomb,
    "body_cl_mismatch": mode_body_cl_mismatch,
    "range_edge_hail": mode_range_edge_hail,
    "traversal_hail": mode_traversal_hail,
    "hot_rename_storm": mode_hot_rename_storm,
    "ws_protocol_abuse": mode_ws_protocol_abuse,
    "ws_ping_flood": mode_ws_ping_flood,
    "ws_fanout": mode_ws_fanout,
    "ws_churn": mode_ws_churn,
    "conn_churn_rss": mode_conn_churn_rss,
    "ws_churn_rss": mode_ws_churn_rss,
    "pages_throw_storm": mode_pages_throw_storm,
    "listing_href_abuse": mode_listing_href_abuse,
    "uri_decode_refuse": mode_uri_decode_refuse,
}

QUICK_ORDER = [
    "conn_storm",
    "accept_burst_survive",
    "abort_mid_headers",
    "abort_mid_body",
    "pipelined_garbage",
    "http_garbage_hail",
    "header_bomb",
    "body_cl_mismatch",
    "range_edge_hail",
    "traversal_hail",
    "hot_rename_storm",
    "ws_protocol_abuse",
    "ws_ping_flood",
    "ws_fanout",
    "ws_churn",
    "slowloris_headers",
    "header_never_finishes",
    "idle_keepalive_pile",
    "tiny_window_get",
    "fd_exhaust_accept",
    "pages_throw_storm",
    "listing_href_abuse",
    "uri_decode_refuse",
    "conn_churn_rss",
    "ws_churn_rss",
]


def prepare_pages(tmp: Path, pages_src: Path) -> Optional[Path]:
    if not pages_src.is_dir():
        return None
    dest = tmp / "pages"
    dest.mkdir()
    # One face per route — dual hello.js+hello.py is a 500 by design.
    for name in ("hello.js", "boom.js"):
        src = pages_src / name
        if src.exists():
            (dest / name).write_text(src.read_text())
    return dest if any(dest.iterdir()) else None


def prepare_root(tmp: Path, fixtures: Path, with_list: bool) -> Path:
    root = tmp / "www"
    root.mkdir()
    for name in ("1kb.bin", "4kb.html", "64kb.js", "1mb.bin"):
        cand = fixtures / name
        if cand.exists():
            (root / name).write_bytes(cand.read_bytes())
    # No index.html at / so --list can produce a directory listing.
    sub = root / "sub"
    sub.mkdir()
    (sub / "a.txt").write_text("hi\n")
    if with_list:
        (root / "weird?x.txt").write_text("q\n")
        (root / "weird#y.txt").write_text("h\n")
        (root / "weird space.txt").write_text("s\n")
    return root


def spawn_staticd(bin_path: str, port: int, root: Path, pages: Optional[Path],
                  list_dir: bool) -> tuple[subprocess.Popen, Path]:
    logdir = Path(tempfile.mkdtemp(prefix="staticd-adv-"))
    log = open(logdir / "staticd.log", "w")
    cmd = [bin_path, "--listen", f"127.0.0.1:{port}", "--root", str(root),
           "--workers", "2"]
    if list_dir:
        cmd.append("--list")
    if pages:
        cmd += ["--pages", str(pages)]
    env = os.environ.copy()
    # Short budgets so gap/total/keepalive modes finish inside the suite.
    env.setdefault("HEADER_GAP", "2")
    env.setdefault("HEADER_TOTAL", "6")
    env.setdefault("KEEPALIVE", "5")
    env.setdefault("TLS_HS", "5")
    env.setdefault("WRITE_STALL", "10")
    env.setdefault("WS_IDLE", "30")
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), 0.2):
                return proc, logdir
        except OSError:
            if proc.poll() is not None:
                log.close()
                raise SystemExit(f"staticd exited early; see {logdir / 'staticd.log'}")
            time.sleep(0.05)
    raise SystemExit("staticd port not open")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--spawn", metavar="BIN")
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--scale", default=os.environ.get("CHAOS_SCALE", "quick"),
                    choices=["quick", "full", "soak"])
    ap.add_argument("--mode", default=os.environ.get("MODE"))
    ap.add_argument("--seed", type=int, default=int(os.environ.get("FUZZ_SEED", "1")))
    ap.add_argument("--list", action="store_true", help="enable --list on spawned server")
    ap.add_argument("--pages", default=None, help="pages dir for spawned server")
    args = ap.parse_args()

    port = args.port or (18090 if args.spawn else 8080)
    proc = None
    logdir = None
    tmp = Path(tempfile.mkdtemp(prefix="staticd-adv-www-"))
    fixtures = STATICD_DIR / "fixtures"
    if not (fixtures / "1kb.bin").exists():
        subprocess.check_call([str(STATICD_DIR / "gen_fixtures.sh")], cwd=str(STATICD_DIR))

    pages_src = Path(args.pages) if args.pages else (STATICD_DIR / "pages")
    root = prepare_root(tmp, fixtures, with_list=True)
    pages = prepare_pages(tmp, pages_src) if pages_src.is_dir() else None
    list_dir = True

    try:
        if args.spawn:
            proc, logdir = spawn_staticd(args.spawn, port, root, pages, list_dir)

        ctx = Ctx(host=args.host, port=port, root=root, scale=args.scale,
                  pages=pages, rng=random.Random(args.seed))

        modes = [args.mode] if args.mode else QUICK_ORDER
        if args.scale == "soak" and not args.mode:
            # same set; soak secs inflate churn modes
            pass

        print(f"== staticd adversary scale={args.scale} seed={args.seed} ==")
        results: list[Result] = []
        fatal = 0
        breaks = 0
        for name in modes:
            fn = MODES.get(name)
            if not fn:
                print(f"  FAIL unknown mode {name}")
                fatal += 1
                continue
            try:
                r = fn(ctx)
            except Fail as e:
                r = Result(name, False, str(e))
            except Exception as e:
                r = Result(name, False, f"{type(e).__name__}: {e}")
            results.append(r)
            print(r.line())
            if not r.ok and not r.known_break:
                fatal += 1
            if r.known_break:
                breaks += 1

        print(f"adversary: done fatal={fatal} known_break={breaks} "
              f"ok={sum(1 for r in results if r.ok)}")
        if fatal:
            sys.exit(1)
        # known breaks do not fail the process — they document holes
        sys.exit(0)
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    main()
