"""Exercise the native worker with two loopback processes and a relay restart."""
from contextlib import ExitStack
import os
from pathlib import Path
from queue import Queue
import socket
import subprocess
import sys
from threading import Thread

from test_coop_relay import MTX, PASSWORD, auth, relay


def read_output(out, queue):
    for line in out:
        queue.put(line.rstrip())
    queue.put(None)


def check_wire(client, env):
    with socket.create_server(("127.0.0.1", 0)) as server:
        server.settimeout(5)
        port = server.getsockname()[1]
        env = {**env, "SAVAGEWIND_COOP_ENDPOINT": f"127.0.0.1:{port}", "SAVAGEWIND_COOP_PASSWORD": PASSWORD}
        with subprocess.Popen([client, "wire"], env=env, stdout=subprocess.PIPE, stdin=subprocess.PIPE,
                              text=True) as proc:
            try:
                assert proc.stdout is not None and proc.stdin is not None
                stream = Queue()
                Thread(target=read_output, args=(proc.stdout, stream), daemon=True).start()
                sock, _ = server.accept()
                with sock, sock.makefile("rb") as lines:
                    sock.settimeout(3)
                    challenge = b"01ABab9876543210"
                    sock.sendall(b"HEL")
                    sock.sendall(b"LO 7 5 " + challenge + b"\n")
                    assert lines.readline() + lines.readline() == auth(challenge)
                    sock.sendall(b"WELCOME\nMT")
                    assert stream.get(timeout=4) == "authenticated"
                    sock.sendall(b"X 7 " + MTX + b"\nMTX 8 " + MTX.replace(b"123.5", b"NaN") +
                                 b"\nMTX 9 " + MTX + b" 104\nMTX 9 " + MTX + b" -7\n" +
                                 b"WPN 9 1 Weapon_Sword_001 " + MTX + b"\nWPN 9 7 -\nWPN 9\nWPN 9 1 Bad! -\n" +
                                 b"BYE 10\nMTX 10 " + MTX + b"\n" +
                                 (b"MTX 9 " + MTX + b"\n") * 47 + b"BYE 9\n" + b"X" * 401)
                    assert not sock.recv(1)
                    proc.stdin.write("\n")
                    proc.stdin.flush()
                    assert proc.wait(timeout=4) == 0
            finally:
                if proc.poll() is None:
                    proc.kill()
    print("native framing, UTF-8 auth, self/extra peer filtering, bad WPN ignored, overflow and BYE: OK", flush=True)


def main():
    client = Path(sys.argv[1]).resolve()
    if os.name == "nt" and not client.suffix:
        client = client.with_suffix(".exe")
    env = {k: v for k, v in os.environ.items() if not k.startswith("SAVAGEWIND_COOP_")}
    for config in ({}, {"SAVAGEWIND_COOP_ENDPOINT": "127.0.0.1:47800"},
                   {"SAVAGEWIND_COOP_ENDPOINT": "127.0.0.1:invalid", "SAVAGEWIND_COOP_PASSWORD": PASSWORD}):
        subprocess.run([client, "off"], env={**env, **config}, check=True, timeout=4)
    print("disabled, missing password, invalid endpoint: OK", flush=True)
    check_wire(client, env)

    with ExitStack() as clients:
        procs, streams = [], []
        with relay() as port:
            env.update(SAVAGEWIND_COOP_ENDPOINT=f"127.0.0.1:{port}", SAVAGEWIND_COOP_PASSWORD=PASSWORD)
            for peer in (1, 2):
                proc = clients.enter_context(subprocess.Popen([client, str(peer)], env=env,
                                                             stdout=subprocess.PIPE, text=True))
                clients.callback(proc.kill)
                procs.append(proc)
                stream = Queue()
                streams.append(stream)
                assert proc.stdout is not None
                Thread(target=lambda out, q: [q.put(line.rstrip()) for line in out],
                       args=(proc.stdout, stream), daemon=True).start()
            for stream in streams:
                assert stream.get(timeout=24) == "round=1 exact=100"
            print("100 exact matrices and a worn sword each way: OK", flush=True)
        for stream in streams:
            assert stream.get(timeout=8) == "disconnected"
        with relay(port):
            for stream in streams:
                assert stream.get(timeout=12) == "round=2 exact=3"
            for proc in procs:
                assert proc.wait(timeout=8) == 0
            print("relay restart, fresh poses, stop/start reset: OK", flush=True)


if __name__ == "__main__":
    main()
