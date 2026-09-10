#!/usr/bin/env python3
"""One-shot worker bail with a live keep-alive row (CC_SERVER_FAULT_BAIL_LIVE=1).

Expect: defer drops the row (peer RST) before tape.close(); respawn serves.
"""
from __future__ import annotations

import os
import signal
import socket
import subprocess
import sys
import time


def main() -> int:
    root = os.path.dirname(os.path.abspath(__file__))
    sd = os.path.normpath(os.path.join(root, "..", "..", "real_projects", "staticd"))
    bin_path = os.environ.get("STATICD_BIN", os.path.join(sd, "out", "staticd"))
    fixtures = os.path.join(sd, "fixtures")
    port = int(os.environ.get("PORT", "18094"))

    env = os.environ.copy()
    env["CC_SERVER_FAULT_BAIL_LIVE"] = "1"
    log_path = "/tmp/fault_bail_live.log"
    log = open(log_path, "w")
    p = subprocess.Popen(
        [bin_path, "--listen", f"127.0.0.1:{port}", "--root", fixtures, "--workers", "1"],
        env=env,
        cwd=sd,
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    try:
        s = None
        for _ in range(80):
            try:
                s = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                break
            except OSError:
                time.sleep(0.05)
        if s is None:
            print("FAIL listen")
            return 1

        # First accepted conn must be the one we bail with — no probe accept.
        s.sendall(
            b"GET /4kb.html HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n"
        )
        time.sleep(0.8)
        s.settimeout(0.5)
        old_dead = False
        try:
            peeked = s.recv(16, socket.MSG_PEEK)
            # Drop closes the fd → RST or clean EOF; either means the row went away.
            if not peeked:
                old_dead = True
                print("old_sock EOF")
            else:
                print("old_sock still_has", len(peeked))
        except OSError as e:
            old_dead = True
            print("old_sock", type(e).__name__)
        s.close()

        alive = False
        try:
            s2 = socket.create_connection(("127.0.0.1", port), timeout=2)
            s2.sendall(
                b"GET /4kb.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
            )
            body = b""
            s2.settimeout(2)
            while True:
                chunk = s2.recv(65536)
                if not chunk:
                    break
                body += chunk
                if len(body) > 5000:
                    break
            alive = b"200" in body and b"\r\n\r\n" in body
            print("respawn_body", len(body))
            s2.close()
        except OSError as e:
            print("respawn_err", e)

        ok = old_dead and alive
        print("PASS" if ok else "FAIL", "old_dead", old_dead, "respawn", alive)
        return 0 if ok else 1
    finally:
        p.send_signal(signal.SIGTERM)
        try:
            p.wait(timeout=3)
        except subprocess.TimeoutExpired:
            p.kill()


if __name__ == "__main__":
    sys.exit(main())
