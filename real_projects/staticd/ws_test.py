#!/usr/bin/env python3
"""staticd WebSocket correctness — narrow echo subset.

Usage:
  ./ws_test.py 127.0.0.1:8080
  ./ws_test.py --port 8080
  ./ws_test.py --spawn ./out/staticd   # start a throwaway staticd, then test

Covers handshake Accept, text/binary echo, ping→pong, close, and the
rejection / close paths claimed in README (bad key, missing upgrade
headers, version, fragment, unmasked client, oversized payload).
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import os
import socket
import struct
import subprocess
import tempfile
import time
from typing import Optional

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
# Must match WS_MAX_PAYLOAD in staticd_ws.cch (16K window − 14).
WS_MAX_PAYLOAD = (16 * 1024) - 14


class Fail(Exception):
    pass


def accept_of(key: str) -> str:
    return base64.b64encode(hashlib.sha1(key.encode() + GUID).digest()).decode()


def recvn(sock: socket.socket, n: int, extra: bytes = b"") -> tuple[bytes, bytes]:
    out = extra
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise Fail(f"short read want={n} have={len(out)}")
        out += chunk
    return out[:n], out[n:]


def send_frame(
    sock: socket.socket,
    opcode: int,
    payload: bytes,
    *,
    fin: bool = True,
    mask: bool = True,
) -> None:
    b0 = (0x80 if fin else 0) | (opcode & 0x0F)
    n = len(payload)
    if n < 126:
        hdr = bytearray([b0, (0x80 if mask else 0) | n])
    elif n <= 0xFFFF:
        hdr = bytearray([b0, (0x80 if mask else 0) | 126])
        hdr.extend(struct.pack("!H", n))
    else:
        hdr = bytearray([b0, (0x80 if mask else 0) | 127])
        hdr.extend(struct.pack("!Q", n))
    if mask:
        mkey = os.urandom(4)
        hdr.extend(mkey)
        payload = bytes(b ^ mkey[i % 4] for i, b in enumerate(payload))
    sock.sendall(bytes(hdr) + payload)


def read_frame(sock: socket.socket, extra: bytes = b"") -> tuple[int, bytes, bytes]:
    hdr, extra = recvn(sock, 2, extra)
    fin = (hdr[0] & 0x80) != 0
    opcode = hdr[0] & 0x0F
    masked = (hdr[1] & 0x80) != 0
    n = hdr[1] & 0x7F
    if n == 126:
        ext, extra = recvn(sock, 2, extra)
        n = struct.unpack("!H", ext)[0]
    elif n == 127:
        ext, extra = recvn(sock, 8, extra)
        n = struct.unpack("!Q", ext)[0]
    if masked:
        mkey, extra = recvn(sock, 4, extra)
        body, extra = recvn(sock, n, extra)
        body = bytes(b ^ mkey[i % 4] for i, b in enumerate(body))
    else:
        body, extra = recvn(sock, n, extra)
    if not fin:
        raise Fail("server fragment")
    if masked:
        raise Fail("server masked")
    return opcode, body, extra


def handshake(
    host: str,
    port: int,
    *,
    path: str = "/echo",
    key: Optional[str] = None,
    headers: Optional[list[str]] = None,
    timeout: float = 2.0,
) -> tuple[socket.socket, bytes, bytes]:
    if key is None:
        key = base64.b64encode(os.urandom(16)).decode()
    lines = [
        f"GET {path} HTTP/1.1",
        f"Host: {host}",
    ]
    if headers is None:
        lines += [
            "Upgrade: websocket",
            "Connection: Upgrade",
            f"Sec-WebSocket-Key: {key}",
            "Sec-WebSocket-Version: 13",
        ]
    else:
        lines += headers
    lines.append("")
    lines.append("")
    s = socket.create_connection((host, port), timeout)
    s.settimeout(timeout)
    s.sendall("\r\n".join(lines).encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            raise Fail("handshake closed before headers")
        buf += chunk
    head, rest = buf.split(b"\r\n\r\n", 1)
    return s, head, rest


def status_line(head: bytes) -> bytes:
    return head.split(b"\r\n", 1)[0]


def expect_101(head: bytes, key: str) -> None:
    if b"101" not in status_line(head):
        raise Fail(f"status {status_line(head)!r}")
    want = accept_of(key).encode()
    if want not in head:
        raise Fail("Sec-WebSocket-Accept mismatch")


def closed_soon(sock: socket.socket, timeout: float = 1.0) -> bool:
    sock.settimeout(timeout)
    try:
        data = sock.recv(64)
        return data == b""
    except (TimeoutError, socket.timeout, ConnectionResetError, BrokenPipeError):
        return False
    except OSError:
        return True


def case(name: str, fn) -> None:
    try:
        fn()
        print(f"  ok {name}")
    except Fail as e:
        raise SystemExit(f"FAIL WS {name}: {e}") from e
    except Exception as e:
        raise SystemExit(f"FAIL WS {name}: {type(e).__name__}: {e}") from e


def run_suite(host: str, port: int) -> None:
    def text_echo():
        key = base64.b64encode(os.urandom(16)).decode()
        s, head, rest = handshake(host, port, key=key)
        expect_101(head, key)
        send_frame(s, 1, b"hello")
        op, body, rest = read_frame(s, rest)
        if op != 1 or body != b"hello":
            raise Fail(f"echo op={op} body={body!r}")
        s.close()

    def binary_echo():
        key = base64.b64encode(os.urandom(16)).decode()
        s, head, rest = handshake(host, port, key=key)
        expect_101(head, key)
        payload = bytes(range(256))
        send_frame(s, 2, payload)
        op, body, rest = read_frame(s, rest)
        if op != 2 or body != payload:
            raise Fail(f"binary op={op} len={len(body)}")
        s.close()

    def multi_echo():
        key = base64.b64encode(os.urandom(16)).decode()
        s, head, rest = handshake(host, port, key=key)
        expect_101(head, key)
        for msg in (b"a", b"bb", b"ccc"):
            send_frame(s, 1, msg)
            op, body, rest = read_frame(s, rest)
            if op != 1 or body != msg:
                raise Fail(f"multi got {body!r}")
        s.close()

    def ping_pong():
        key = base64.b64encode(os.urandom(16)).decode()
        s, head, rest = handshake(host, port, key=key)
        expect_101(head, key)
        send_frame(s, 9, b"ping-body")
        op, body, rest = read_frame(s, rest)
        if op != 10 or body != b"ping-body":
            raise Fail(f"pong op={op} body={body!r}")
        s.close()

    def client_close():
        key = base64.b64encode(os.urandom(16)).decode()
        s, head, rest = handshake(host, port, key=key)
        expect_101(head, key)
        send_frame(s, 8, b"\x03\xe8bye")  # 1000 + reason
        op, body, rest = read_frame(s, rest)
        if op != 8:
            raise Fail(f"close reply op={op}")
        if not closed_soon(s):
            raise Fail("socket still open after close handshake")
        s.close()

    def reject_bad_key():
        # Not base64-of-16: too short / wrong alphabet.
        s, head, _ = handshake(
            host,
            port,
            headers=[
                "Upgrade: websocket",
                "Connection: Upgrade",
                "Sec-WebSocket-Key: not-valid!!",
                "Sec-WebSocket-Version: 13",
            ],
        )
        st = status_line(head)
        if b"101" in st:
            raise Fail(f"accepted bad key: {st!r}")
        if b"400" not in st:
            raise Fail(f"want 400 got {st!r}")
        s.close()

    def reject_missing_version():
        key = base64.b64encode(os.urandom(16)).decode()
        s, head, _ = handshake(
            host,
            port,
            headers=[
                "Upgrade: websocket",
                "Connection: Upgrade",
                f"Sec-WebSocket-Key: {key}",
            ],
        )
        # No Version → not an upgrade; staticd serves HTTP (file miss → 404).
        if b"101" in status_line(head):
            raise Fail("upgraded without Version")
        s.close()

    def reject_missing_connection_upgrade():
        key = base64.b64encode(os.urandom(16)).decode()
        s, head, _ = handshake(
            host,
            port,
            headers=[
                "Upgrade: websocket",
                "Connection: keep-alive",
                f"Sec-WebSocket-Key: {key}",
                "Sec-WebSocket-Version: 13",
            ],
        )
        if b"101" in status_line(head):
            raise Fail("upgraded without Connection: upgrade")
        s.close()

    def fragment_closes():
        key = base64.b64encode(os.urandom(16)).decode()
        s, head, rest = handshake(host, port, key=key)
        expect_101(head, key)
        send_frame(s, 1, b"part", fin=False)
        if not closed_soon(s):
            raise Fail("fragment did not close")
        s.close()

    def unmasked_closes():
        key = base64.b64encode(os.urandom(16)).decode()
        s, head, rest = handshake(host, port, key=key)
        expect_101(head, key)
        send_frame(s, 1, b"x", mask=False)
        if not closed_soon(s):
            raise Fail("unmasked client frame did not close")
        s.close()

    def oversized_closes():
        key = base64.b64encode(os.urandom(16)).decode()
        s, head, rest = handshake(host, port, key=key)
        expect_101(head, key)
        send_frame(s, 1, b"x" * (WS_MAX_PAYLOAD + 1))
        if not closed_soon(s, timeout=2.0):
            raise Fail("oversized payload did not close")
        s.close()

    def max_payload_echo():
        key = base64.b64encode(os.urandom(16)).decode()
        s, head, rest = handshake(host, port, key=key)
        expect_101(head, key)
        # Keep under window: 126-extended header + mask + payload must fit
        # the 16K read buf. Use a modest large frame that needs 16-bit len.
        payload = os.urandom(2000)
        send_frame(s, 2, payload)
        op, body, rest = read_frame(s, rest)
        if op != 2 or body != payload:
            raise Fail(f"large echo op={op} len={len(body)}")
        s.close()

    print(f"== websocket {host}:{port} ==")
    case("text echo", text_echo)
    case("binary echo", binary_echo)
    case("multi echo", multi_echo)
    case("ping → pong", ping_pong)
    case("client close", client_close)
    case("reject bad key", reject_bad_key)
    case("reject missing Version", reject_missing_version)
    case("reject missing Connection upgrade", reject_missing_connection_upgrade)
    case("fragment closes", fragment_closes)
    case("unmasked closes", unmasked_closes)
    case("oversized closes", oversized_closes)
    case("large binary echo", max_payload_echo)
    print("websocket: PASS")


def wait_port(host: str, port: int, timeout: float = 5.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), 0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise SystemExit(f"FAIL WS: port {port} not open")


def spawn_staticd(bin_path: str, port: int) -> tuple[subprocess.Popen, str]:
    root = tempfile.mkdtemp(prefix="staticd-ws-")
    # Empty root is fine — WS upgrades before file serve.
    log = open(os.path.join(root, "staticd.log"), "w")
    proc = subprocess.Popen(
        [bin_path, "--listen", f"127.0.0.1:{port}", "--root", root, "--workers", "1"],
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    return proc, root


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", nargs="?", help="host:port (default 127.0.0.1:PORT)")
    ap.add_argument("--port", type=int, default=None, help="port on 127.0.0.1")
    ap.add_argument(
        "--spawn",
        metavar="STATICD",
        help="start STATICD on --port (default 18080) for the suite",
    )
    args = ap.parse_args()

    host, port = "127.0.0.1", 8080
    if args.target:
        if ":" in args.target:
            host, p = args.target.rsplit(":", 1)
            port = int(p)
        else:
            port = int(args.target)
    if args.port is not None:
        port = args.port

    proc = None
    root = None
    try:
        if args.spawn:
            if args.port is None and not args.target:
                port = 18080
            proc, root = spawn_staticd(args.spawn, port)
            wait_port(host, port)
        run_suite(host, port)
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        if root is not None:
            # best-effort cleanup
            try:
                for name in os.listdir(root):
                    os.remove(os.path.join(root, name))
                os.rmdir(root)
            except OSError:
                pass


if __name__ == "__main__":
    main()
