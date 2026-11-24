"""Loopback tests for the v5 pose relay; no game files or reference code."""
import hashlib
import os
from pathlib import Path
import socket
import subprocess
import sys
import unittest
from contextlib import contextmanager

RELAY = Path(__file__).with_name("coop_relay.py")
PASSWORD = "synthetic-örnek-🔑-42"
MTX = b"1 0 0 123.5 0 1 0 -24 0 0 1 8.25"


@contextmanager
def relay(port=0):
    with subprocess.Popen([sys.executable, "-u", str(RELAY), "--port", str(port)],
                          env={**os.environ, "COOP_PASSWORD": PASSWORD},
                          stdout=subprocess.PIPE) as proc:
        try:
            assert proc.stdout is not None
            ready = proc.stdout.readline().split()
            assert ready[:2] == [b"LISTEN", b"127.0.0.1"], ready
            yield int(ready[2])
        finally:
            proc.terminate()
            proc.wait(timeout=5)


@contextmanager
def peer(port):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock, sock.makefile("rb") as lines:
        hello, pid, version, challenge = lines.readline().split()
        assert hello == b"HELLO" and int(pid) > 0 and version == b"5" and len(challenge) == 16
        int(challenge, 16)
        yield sock, lines, pid, challenge


def auth(challenge, password=PASSWORD):
    return b"VER 5\nAUTH " + hashlib.sha256(challenge + password.encode()).hexdigest().encode() + b"\n"


class RelayTest(unittest.TestCase):
    def test_requires_server_password(self):
        env = {k: v for k, v in os.environ.items() if k != "COOP_PASSWORD"}
        proc = subprocess.run([sys.executable, str(RELAY), "--port", "0"], env=env,
                              capture_output=True, timeout=3)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn(b"password", proc.stderr.lower())

    def test_denies_wrong_password_or_missing_auth(self):
        with relay() as port, peer(port) as (sock, lines, _, nonce):
            sock.sendall(auth(nonce, "wrong"))
            self.assertEqual(lines.readline(), b"DENY\n")
            self.assertEqual(lines.readline(), b"")
            with peer(port) as (other, response, _, second_nonce):
                self.assertNotEqual(nonce, second_nonce)
                other.sendall(b"VER 5\nMTX " + MTX + b"\n")
                self.assertEqual(response.readline(), b"DENY\n")
                self.assertEqual(response.readline(), b"")

    def test_refuses_wrong_version(self):
        with relay() as port, peer(port) as (sock, lines, _, _):
            sock.sendall(b"VER 4\n")
            self.assertEqual(lines.readline(), b"REFUSE 5\n")
            self.assertEqual(lines.readline(), b"")

    def test_split_coalesced_forward_with_id_no_echo_and_bye(self):
        with relay() as port, peer(port) as (b, rx, bid, bnonce):
            b.sendall(auth(bnonce))
            self.assertEqual(rx.readline(), b"WELCOME\n")
            with peer(port) as (a, ack, aid, anonce):
                self.assertNotEqual(aid, bid)
                a.sendall(b"VE")
                a.sendall(auth(anonce)[2:] + b"MTX " + MTX + b" 7\nMTX " + MTX + b"\n")
                self.assertEqual(ack.readline(), b"WELCOME\n")
                for suffix in (b" 7\n", b"\n"):
                    self.assertEqual(rx.readline(), b"MTX " + aid + b" " + MTX + suffix)
                a.settimeout(0.15)
                with self.assertRaises(TimeoutError):
                    ack.readline()
            self.assertEqual(rx.readline(), b"BYE " + aid + b"\n")

    def test_invalid_matrices_are_not_forwarded(self):
        with relay() as port, peer(port) as (b, rx, _, bnonce), peer(port) as (a, ack, aid, anonce):
            b.sendall(auth(bnonce))
            a.sendall(auth(anonce))
            self.assertEqual(rx.readline(), b"WELCOME\n")
            self.assertEqual(ack.readline(), b"WELCOME\n")
            for bad in (b"NaN", b"inf", b"1e200", b"100001", b"1_000", b"0x1p0"):
                a.sendall(b"MTX " + MTX.replace(b"123.5", bad) + b"\n")
            a.sendall(b"MTX 0 0 0 0 0 0 0 0 0 0 0 0\nMTX " + MTX + b"\n")
            self.assertEqual(rx.readline(), b"MTX " + aid + b" " + MTX + b"\n")

    def test_disconnects_on_oversize_unterminated_line(self):
        with relay() as port, peer(port) as (sock, lines, _, nonce):
            sock.sendall(auth(nonce))
            self.assertEqual(lines.readline(), b"WELCOME\n")
            sock.sendall(b"X" * 401)
            self.assertEqual(lines.readline(), b"")


if __name__ == "__main__":
    unittest.main()
