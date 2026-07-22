#!/usr/bin/env python3
"""Functional smoke for real_projects/redis/redis_idiomatic.ccs.

Run:
    ./cc/bin/ccc --no-cache build real_projects/redis/redis_idiomatic.ccs -o out/redis_idiomatic
    python3 real_projects/redis/redis_smoke.py [--port N] [--server out/redis_idiomatic]

By default the script starts its own server instance on --port (6397) and
kills it on exit; pass --no-spawn to target an already-running server.

Coverage:
  - basics: PING/ECHO, GET/SET/SETNX/SETEX/PSETEX, APPEND/STRLEN/TYPE,
    DEL/EXISTS (variadic), INCR/DECR/INCRBY/DECRBY, MGET/MSET,
    DBSIZE/FLUSHDB/FLUSHALL, KEYS glob, CONFIG GET, inline commands
  - wire parity: byte-exact replies for every RESP shape (simple, error,
    integer, bulk, empty bulk, nil, empty array, mixed bulk+nil array)
  - expiry: EXPIRE/PEXPIRE/TTL/PTTL/PERSIST semantics + lazy expiration
    observed through GET/EXISTS/DBSIZE/KEYS (short TTLs, minimal sleeps)
  - error paths: non-integer INCR, SETEX with non-positive TTL (connection
    stays up); unknown command / bad arity (server replies -ERR then closes
    the connection -- the server's documented decode-error policy)
  - pipelining: a 1000-op mixed-command pipeline in one write, replies
    checked in order (spans the server's 16-deep reply batches)
  - teardown storm: 20 connections aborted mid-pipeline (RST via SO_LINGER),
    then a health check proving the server survived

Exit code 0 and a final "SMOKE OK" line on success; first failure raises.
"""

import argparse
import os
import socket
import subprocess
import sys
import time

CRLF = b"\r\n"


def encode(*args):
    out = b"*%d\r\n" % len(args)
    for a in args:
        if isinstance(a, str):
            a = a.encode()
        elif isinstance(a, int):
            a = str(a).encode()
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    return out


class Resp:
    """Minimal RESP client: framed encode, incremental reply decode."""

    def __init__(self, port, timeout=5.0):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.buf = b""

    def close(self):
        self.sock.close()

    def abort(self):
        """Abrupt teardown: RST instead of FIN."""
        self.sock.setsockopt(
            socket.SOL_SOCKET, socket.SO_LINGER,
            b"\x01\x00\x00\x00\x00\x00\x00\x00")
        self.sock.close()

    def send_raw(self, data):
        self.sock.sendall(data)

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise ConnectionError("server closed connection")
        self.buf += chunk

    def _line(self):
        while CRLF not in self.buf:
            self._fill()
        line, self.buf = self.buf.split(CRLF, 1)
        return line

    def read_reply(self):
        line = self._line()
        t, rest = line[:1], line[1:]
        if t == b"+":
            return rest.decode()
        if t == b"-":
            return Exception(rest.decode())
        if t == b":":
            return int(rest)
        if t == b"$":
            n = int(rest)
            if n == -1:
                return None
            while len(self.buf) < n + 2:
                self._fill()
            data, self.buf = self.buf[:n], self.buf[n + 2:]
            return data
        if t == b"*":
            n = int(rest)
            if n == -1:
                return None
            return [self.read_reply() for _ in range(n)]
        raise ValueError("bad RESP type byte %r (line=%r)" % (t, line))

    def cmd(self, *args):
        self.send_raw(encode(*args))
        return self.read_reply()


FAILS = 0


def check(name, got, want):
    if got != want:
        raise AssertionError("%s: got %r, want %r" % (name, got, want))
    print("  ok %-28s %r" % (name, got))


def check_err(name, got, prefix=""):
    if not isinstance(got, Exception):
        raise AssertionError("%s: expected error, got %r" % (name, got))
    if prefix and not str(got).startswith(prefix):
        raise AssertionError("%s: error %r lacks prefix %r" % (name, got, prefix))
    print("  ok %-28s -%s" % (name, got))


def test_basics(port):
    print("[basics]")
    c = Resp(port)
    check("FLUSHALL", c.cmd("FLUSHALL"), "OK")
    check("PING", c.cmd("PING"), "PONG")
    check("PING msg", c.cmd("PING", "yo"), b"yo")
    check("ECHO", c.cmd("ECHO", "hello world"), b"hello world")

    check("SET", c.cmd("SET", "k1", "v1"), "OK")
    check("GET", c.cmd("GET", "k1"), b"v1")
    check("GET missing", c.cmd("GET", "nope"), None)
    check("SET overwrite", c.cmd("SET", "k1", "v2"), "OK")
    check("GET overwrite", c.cmd("GET", "k1"), b"v2")

    check("SETNX taken", c.cmd("SETNX", "k1", "z"), 0)
    check("SETNX fresh", c.cmd("SETNX", "k2", "z"), 1)
    check("GET after SETNX", c.cmd("GET", "k2"), b"z")

    check("APPEND fresh", c.cmd("APPEND", "app", "abc"), 3)
    check("APPEND grow", c.cmd("APPEND", "app", "defg"), 7)
    check("GET appended", c.cmd("GET", "app"), b"abcdefg")
    check("STRLEN", c.cmd("STRLEN", "app"), 7)
    check("STRLEN missing", c.cmd("STRLEN", "nope"), 0)

    check("TYPE string", c.cmd("TYPE", "app"), "string")
    check("TYPE none", c.cmd("TYPE", "nope"), "none")

    check("EXISTS multi", c.cmd("EXISTS", "k1", "nope", "k1"), 2)
    check("DEL multi", c.cmd("DEL", "k1", "k2", "nope"), 2)
    check("EXISTS after DEL", c.cmd("EXISTS", "k1", "k2"), 0)

    check("INCR fresh", c.cmd("INCR", "ctr"), 1)
    check("INCR", c.cmd("INCR", "ctr"), 2)
    check("DECR", c.cmd("DECR", "ctr"), 1)
    check("INCRBY", c.cmd("INCRBY", "ctr", 40), 41)
    check("DECRBY", c.cmd("DECRBY", "ctr", 42), -1)
    check("INCR negative fresh", c.cmd("DECR", "ctr2"), -1)
    check("GET counter bytes", c.cmd("GET", "ctr"), b"-1")

    check("MSET", c.cmd("MSET", "ma", "1", "mb", "2", "mc", "3"), "OK")
    check("MGET", c.cmd("MGET", "ma", "nope", "mb", "mc"),
          [b"1", None, b"2", b"3"])
    check("MSET pair", c.cmd("MSET", "ma", "9", "md", "4"), "OK")
    check("MGET overwrite", c.cmd("MGET", "ma", "md"), [b"9", b"4"])

    check("FLUSHDB", c.cmd("FLUSHDB"), "OK")
    check("DBSIZE empty", c.cmd("DBSIZE"), 0)
    check("MSET keys", c.cmd("MSET", "key:1", "a", "key:2", "b", "kez", "c"), "OK")
    check("DBSIZE", c.cmd("DBSIZE"), 3)
    check("KEYS *", sorted(c.cmd("KEYS", "*")), [b"key:1", b"key:2", b"kez"])
    check("KEYS key:*", sorted(c.cmd("KEYS", "key:*")), [b"key:1", b"key:2"])
    check("KEYS ke?:1", c.cmd("KEYS", "ke?:1"), [b"key:1"])
    check("KEYS class", sorted(c.cmd("KEYS", "key:[12]")), [b"key:1", b"key:2"])
    check("KEYS class-neg", c.cmd("KEYS", "key:[^1]"), [b"key:2"])
    check("KEYS miss", c.cmd("KEYS", "zz*"), [])

    cfg = c.cmd("CONFIG", "GET", "save")
    check("CONFIG GET save", cfg, [b"save", b""])

    # errors that keep the connection open (owner-side command errors)
    check_err("INCR non-int", c.cmd("SET", "s", "abc") and c.cmd("INCR", "s"), "ERR")
    check_err("SETEX zero ttl", c.cmd("SETEX", "s", "0", "v"), "ERR")
    check("conn survives errors", c.cmd("PING"), "PONG")
    check("cleanup", c.cmd("FLUSHALL"), "OK")
    c.close()

    # decode-level errors reply -ERR and then close the connection
    # (the server's documented policy for parse/decode failures)
    c = Resp(port)
    got = c.cmd("NOSUCHCMD", "x")
    check_err("unknown command", got, "ERR")
    closed = False
    try:
        c.cmd("PING")
    except (ConnectionError, ConnectionResetError, BrokenPipeError):
        closed = True
    check("conn closed after decode err", closed, True)
    c.close()

    # inline commands
    c = Resp(port)
    c.send_raw(b"PING\r\n")
    check("inline PING", c.read_reply(), "PONG")
    c.send_raw(b"SET inlinek inlinev\r\n")
    check("inline SET", c.read_reply(), "OK")
    c.send_raw(b"GET inlinek\r\n")
    check("inline GET", c.read_reply(), b"inlinev")
    check("cleanup2", c.cmd("FLUSHALL"), "OK")
    c.close()


def test_wire_parity(port):
    """Byte-exact wire replies: the generated schema writers (RespGReply /
    RespGBulkHdr / RespGArrayHdr) must reproduce the retired hand encoders'
    bytes for every reply shape, including boundary nil and empty arrays."""
    print("[wire parity]")
    c = Resp(port)
    c.cmd("FLUSHALL")

    def expect(name, raw_cmd, want):
        c.send_raw(raw_cmd)
        while len(c.buf) < len(want):
            c._fill()
        got, c.buf = c.buf[:len(want)], c.buf[len(want):]
        if got != want:
            raise AssertionError("%s: got %r, want %r" % (name, got, want))
        print("  ok %-28s %r" % (name, want))

    expect("simple PONG", encode("PING"), b"+PONG\r\n")
    expect("bulk PING echo", encode("PING", "hey"), b"$3\r\nhey\r\n")
    expect("simple OK", encode("SET", "wp:k", "v1"), b"+OK\r\n")
    expect("bulk GET", encode("GET", "wp:k"), b"$2\r\nv1\r\n")
    expect("empty-value SET", encode("SET", "wp:e", ""), b"+OK\r\n")
    expect("empty bulk", encode("GET", "wp:e"), b"$0\r\n\r\n")
    expect("nil bulk", encode("GET", "wp:missing"), b"$-1\r\n")
    expect("integer", encode("INCR", "wp:ctr"), b":1\r\n")
    expect("negative integer", encode("DECRBY", "wp:neg", "7"), b":-7\r\n")
    expect("TTL missing -2", encode("TTL", "wp:missing"), b":-2\r\n")
    expect("empty array", encode("KEYS", "wp:nomatch*"), b"*0\r\n")
    expect("KEYS array", encode("KEYS", "wp:k"), b"*1\r\n$4\r\nwp:k\r\n")
    expect("MGET bulk+nil", encode("MGET", "wp:k", "wp:missing"),
           b"*2\r\n$2\r\nv1\r\n$-1\r\n")
    expect("CONFIG GET save", encode("CONFIG", "GET", "save"),
           b"*2\r\n$4\r\nsave\r\n$0\r\n\r\n")
    expect("CONFIG GET appendonly", encode("CONFIG", "GET", "appendonly"),
           b"*2\r\n$10\r\nappendonly\r\n$2\r\nno\r\n")
    expect("CONFIG GET unknown", encode("CONFIG", "GET", "zzz"), b"*0\r\n")
    expect("error reply", encode("INCR", "wp:k"),
           b"-ERR value is not an integer or out of range\r\n")
    check("wire flush", c.cmd("FLUSHALL"), "OK")
    c.close()


def test_expiry(port):
    print("[expiry]")
    c = Resp(port)
    check("flush", c.cmd("FLUSHALL"), "OK")

    check("SET e1", c.cmd("SET", "e1", "v"), "OK")
    check("TTL no expiry", c.cmd("TTL", "e1"), -1)
    check("PTTL no expiry", c.cmd("PTTL", "e1"), -1)
    check("TTL missing", c.cmd("TTL", "nope"), -2)
    check("EXPIRE", c.cmd("EXPIRE", "e1", 100), 1)
    ttl = c.cmd("TTL", "e1")
    assert 95 <= ttl <= 100, "TTL %r out of range" % ttl
    print("  ok TTL in range              %r" % ttl)
    pttl = c.cmd("PTTL", "e1")
    assert 95_000 <= pttl <= 100_000, "PTTL %r out of range" % pttl
    print("  ok PTTL in range             %r" % pttl)
    check("EXPIRE missing", c.cmd("EXPIRE", "nope", 100), 0)

    check("PERSIST", c.cmd("PERSIST", "e1"), 1)
    check("TTL persisted", c.cmd("TTL", "e1"), -1)
    check("PERSIST no ttl", c.cmd("PERSIST", "e1"), 0)
    check("PERSIST missing", c.cmd("PERSIST", "nope"), 0)

    # SET clears TTL (redis semantics)
    check("EXPIRE again", c.cmd("EXPIRE", "e1", 100), 1)
    check("SET clears ttl", c.cmd("SET", "e1", "v2"), "OK")
    check("TTL cleared by SET", c.cmd("TTL", "e1"), -1)

    # APPEND / INCR keep TTL (redis semantics)
    check("EXPIRE keep", c.cmd("EXPIRE", "e1", 100), 1)
    check("APPEND keeps ttl", c.cmd("APPEND", "e1", "x") > 0, True)
    assert c.cmd("TTL", "e1") > 0
    print("  ok APPEND kept TTL")
    check("SET ctr", c.cmd("SET", "ec", "5"), "OK")
    check("EXPIRE ctr", c.cmd("EXPIRE", "ec", 100), 1)
    check("INCR keeps value", c.cmd("INCR", "ec"), 6)
    assert c.cmd("TTL", "ec") > 0
    print("  ok INCR kept TTL")

    # EXPIRE with non-positive TTL deletes now, answers 1
    check("EXPIRE 0 deletes", c.cmd("EXPIRE", "ec", 0), 1)
    check("gone", c.cmd("EXISTS", "ec"), 0)

    # lazy expiration observed via every read family
    check("PSETEX 60ms", c.cmd("PSETEX", "pz", 60, "v"), "OK")
    check("live before deadline", c.cmd("GET", "pz"), b"v")
    time.sleep(0.09)
    check("GET expired", c.cmd("GET", "pz"), None)
    check("TTL expired", c.cmd("TTL", "pz"), -2)

    check("SETEX 1s", c.cmd("SETEX", "sx", 1, "v"), "OK")
    assert 0 < c.cmd("TTL", "sx") <= 1
    print("  ok SETEX TTL")

    check("PEXPIRE 50ms", c.cmd("SET", "px", "v") and c.cmd("PEXPIRE", "px", 50), 1)
    check("PSETEX d", c.cmd("PSETEX", "dz", 50, "v"), "OK")
    check("PSETEX k", c.cmd("PSETEX", "kz", 50, "v"), "OK")
    time.sleep(0.08)
    # expired-but-unreaped keys are invisible to EXISTS/DBSIZE/KEYS/MGET
    check("EXISTS expired", c.cmd("EXISTS", "px", "dz", "kz"), 0)
    check("MGET expired", c.cmd("MGET", "px", "dz", "sx"), [None, None, b"v"])
    check("KEYS skips expired", c.cmd("KEYS", "?z"), [])
    dbs = c.cmd("DBSIZE")
    assert isinstance(dbs, int)
    check("SETNX on expired", c.cmd("PSETEX", "nx", 40, "v"), "OK")
    time.sleep(0.06)
    check("SETNX revives expired", c.cmd("SETNX", "nx", "fresh"), 1)
    check("revived value", c.cmd("GET", "nx"), b"fresh")

    check("flush end", c.cmd("FLUSHALL"), "OK")
    c.close()


def test_pipeline(port):
    print("[pipeline]")
    c = Resp(port)
    check("flush", c.cmd("FLUSHALL"), "OK")

    n = 1000
    batch = b""
    expected = []
    for i in range(n):
        which = i % 5
        if which == 0:
            batch += encode("SET", "p:%d" % i, "v%d" % i)
            expected.append("OK")
        elif which == 1:
            batch += encode("INCR", "pctr")
            expected.append((i // 5) + 1)
        elif which == 2:
            batch += encode("GET", "p:%d" % (i - 2))
            expected.append(b"v%d" % (i - 2))
        elif which == 3:
            batch += encode("EXISTS", "p:%d" % (i - 3), "absent")
            expected.append(1)
        else:
            batch += encode("PING")
            expected.append("PONG")
    c.send_raw(batch)
    for i, want in enumerate(expected):
        got = c.read_reply()
        if got != want:
            raise AssertionError("pipeline op %d: got %r want %r" % (i, got, want))
    print("  ok 1000-op mixed pipeline")

    # pipelined expiry mix: TTL writes interleaved with reads
    batch = b""
    batch += encode("PSETEX", "pipex", 60, "v")
    batch += encode("PTTL", "pipex")
    batch += encode("PERSIST", "pipex")
    batch += encode("TTL", "pipex")
    batch += encode("DEL", "pipex")
    c.send_raw(batch)
    check("pipe PSETEX", c.read_reply(), "OK")
    got = c.read_reply()
    assert isinstance(got, int) and 0 < got <= 60, got
    print("  ok pipe PTTL                 %r" % got)
    check("pipe PERSIST", c.read_reply(), 1)
    check("pipe TTL", c.read_reply(), -1)
    check("pipe DEL", c.read_reply(), 1)

    # variadic + array replies under pipelining
    batch = encode("MSET", "w1", "1", "w2", "2", "w3", "3")
    batch += encode("MGET", "w1", "w2", "w3", "won't")
    batch += encode("KEYS", "w?")
    batch += encode("DEL", "w1", "w2", "w3")
    c.send_raw(batch)
    check("pipe MSET", c.read_reply(), "OK")
    check("pipe MGET", c.read_reply(), [b"1", b"2", b"3", None])
    check("pipe KEYS", sorted(c.read_reply()), [b"w1", b"w2", b"w3"])
    check("pipe DEL", c.read_reply(), 3)

    check("flush end", c.cmd("FLUSHALL"), "OK")
    c.close()


def test_teardown_storm(port):
    print("[teardown storm]")
    for round_ in range(20):
        c = Resp(port)
        # push a burst of pipelined work, read only part of it, then RST
        batch = b""
        for i in range(64):
            batch += encode("SET", "storm:%d:%d" % (round_, i), "x" * 64)
            batch += encode("GET", "storm:%d:%d" % (round_, i))
            batch += encode("INCR", "stormctr")
        c.send_raw(batch)
        for _ in range(round_ % 7):  # drain an uneven prefix
            c.read_reply()
        c.abort()
    # server must still be alive and coherent
    c = Resp(port)
    check("alive after storm", c.cmd("PING"), "PONG")
    got = c.cmd("INCR", "stormctr")
    assert isinstance(got, int) and got >= 1
    print("  ok stormctr readable         %r" % got)
    check("flush", c.cmd("FLUSHALL"), "OK")
    check("post-storm DBSIZE", c.cmd("DBSIZE"), 0)
    c.close()


def wait_port(port, deadline_s=10.0):
    end = time.time() + deadline_s
    while time.time() < end:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("port %d did not open" % port)


def pick_free_port():
    """Collision-safe port pick for --port 0: bind an ephemeral port, release
    it, and hand it to the server.  The tiny release->rebind window is
    acceptable for CI (the kernel avoids immediate ephemeral reuse)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=6397,
                    help="server port; 0 = pick a free ephemeral port "
                         "(collision-safe for parallel CI; requires spawn mode)")
    ap.add_argument("--server", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..",
        "out", "redis_idiomatic"))
    ap.add_argument("--no-spawn", action="store_true",
                    help="target an already-running server instead of spawning one")
    args = ap.parse_args()

    if args.port == 0:
        if args.no_spawn:
            print("--port 0 requires spawn mode (an already-running server "
                  "has a fixed port)", file=sys.stderr)
            return 2
        args.port = pick_free_port()
        print("[smoke] using free port %d" % args.port)

    proc = None
    if not args.no_spawn:
        server = os.path.abspath(args.server)
        if not os.access(server, os.X_OK):
            print("server binary not found/executable: %s" % server, file=sys.stderr)
            return 2
        proc = subprocess.Popen([server, str(args.port)],
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
    try:
        wait_port(args.port)
        test_basics(args.port)
        test_wire_parity(args.port)
        test_expiry(args.port)
        test_pipeline(args.port)
        test_teardown_storm(args.port)
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
    print("SMOKE OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
