#!/usr/bin/env python3
"""Savage Wind v5 MTX/WPN relay. Use only on a trusted LAN or private VPN."""
import argparse
import asyncio
import hashlib
import hmac
from itertools import count
import os
from pathlib import Path
import re
import secrets
import socket

MAX_LINE = 400
MAX_CLIENTS = 16
MAX_PENDING = 16 * 1024
DECIMAL = re.compile(rb"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?")
INTEGER = re.compile(rb"[+-]?[0-9]+")
WEAPON = re.compile(rb"Weapon_[A-Za-z0-9_]{1,16}")
GEAR_SLOTS = 6
GEAR_REACH = 4.0


def matrix(fields):
    if len(fields) not in (12, 13) or not all(DECIMAL.fullmatch(f) for f in fields[:12]):
        return False
    if len(fields) == 13 and (not INTEGER.fullmatch(fields[12]) or
                              not -(2**31) <= int(fields[12]) < 2**31):
        return False
    m = [float(f) for f in fields[:12]]
    if not all(abs(x) <= (100000 if i % 4 == 3 else 1.1) for i, x in enumerate(m)):
        return False
    rows = [m[i:i + 3] for i in (0, 4, 8)]
    for i, row in enumerate(rows):
        if not 0.75 <= sum(x * x for x in row) <= 1.25:
            return False
        if any(abs(sum(x * y for x, y in zip(row, other))) > 0.25 for other in rows[:i]):
            return False
    a, b, c = rows
    return sum(a[i] * (b[(i + 1) % 3] * c[(i + 2) % 3] - b[(i + 2) % 3] * c[(i + 1) % 3])
               for i in range(3)) > 0


def gear(fields):
    """WPN <slot> - | WPN <slot> <Weapon_ name> <3x4 pose in the sender's Link frame, within GEAR_REACH m>"""
    if len(fields) not in (2, 14) or fields[0] not in [str(i).encode() for i in range(GEAR_SLOTS)]:
        return False
    if len(fields) == 2:
        return fields[1] == b"-"
    return bool(WEAPON.fullmatch(fields[1])) and matrix(fields[2:]) and \
        all(abs(float(fields[2 + i])) <= GEAR_REACH for i in (3, 7, 11))


async def read_fields(reader):
    line = await reader.readuntil(b"\n")
    if any(not (32 <= b <= 126 or b in (9, 11, 12, 13)) for b in line[:-1]):
        raise ValueError("non-ASCII record")
    return line.split()


async def serve(host, port, password):
    online = set()
    connections = 0
    ids = count(1)

    def broadcast(source, line):
        for target in online - {source}:
            if target.is_closing():
                continue
            if target.transport.get_write_buffer_size() + len(line) > MAX_PENDING:
                target.transport.abort()
            else:
                target.write(line)

    async def handle(reader, writer):
        nonlocal connections
        if connections >= MAX_CLIENTS:
            writer.transport.abort()
            return
        connections += 1
        pid = str(next(ids)).encode("ascii")
        challenge = secrets.token_hex(8).encode("ascii")

        async def handshake():
            writer.write(b"HELLO " + pid + b" 5 " + challenge + b"\n")
            version = await read_fields(reader)
            if version != [b"VER", b"5"]:
                writer.write(b"REFUSE 5\n" if version[:1] == [b"VER"] else b"DENY\n")
                return False
            auth = await read_fields(reader)
            digest = hashlib.sha256(challenge + password).hexdigest().encode("ascii")
            if len(auth) != 2 or auth[0] != b"AUTH" or not hmac.compare_digest(digest, auth[1]):
                writer.write(b"DENY\n")
                return False
            writer.write(b"WELCOME\n")
            return True

        try:
            if not await asyncio.wait_for(handshake(), timeout=5):
                return
            online.add(writer)
            now = asyncio.get_running_loop().time
            last, tokens = now(), 30.0
            while True:
                fields = await read_fields(reader)
                t = now()
                tokens = min(30.0, tokens + (t - last) * 30.0) - 1
                last = t
                if tokens < 0:
                    return
                if not (fields[:1] == [b"MTX"] and matrix(fields[1:]) or fields[:1] == [b"WPN"] and gear(fields[1:])):
                    continue
                line = b" ".join((fields[0], pid, *fields[1:])) + b"\n"
                if len(line) <= MAX_LINE + 1:
                    broadcast(writer, line)
        except (OSError, ValueError, asyncio.IncompleteReadError, asyncio.LimitOverrunError):
            pass
        finally:
            if writer in online:
                online.remove(writer)
                broadcast(writer, b"BYE " + pid + b"\n")
                writer.transport.abort()  # don't retain a stalled authenticated writer on close
            else:
                writer.close()
            connections -= 1

    async with await asyncio.start_server(handle, host, port, family=socket.AF_INET, limit=MAX_LINE) as server:
        ip, bound_port = server.sockets[0].getsockname()
        print(f"LISTEN {ip} {bound_port}", flush=True)
        await server.serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", help="IPv4 bind address (default: loopback)")
    parser.add_argument("--port", type=int, default=47800)
    parser.add_argument("--password-file", type=Path, help="UTF-8 password; overrides COOP_PASSWORD")
    args = parser.parse_args()
    if not 0 <= args.port <= 65535:
        parser.error("port must be in 0..65535")
    try:
        password = (args.password_file.read_text(encoding="utf-8").rstrip("\n") if args.password_file
                    else os.environ.get("COOP_PASSWORD", "")).encode("utf-8")
    except (UnicodeError, OSError):
        parser.error("cannot read UTF-8 password")
    if not password or len(password) > 1024:
        parser.error("set a non-empty password (max 1024 UTF-8 bytes) with COOP_PASSWORD or --password-file")
    try:
        asyncio.run(serve(args.host, args.port, password))
    except KeyboardInterrupt:
        pass
    except OSError as exc:
        parser.exit(1, f"relay: {exc}\n")


if __name__ == "__main__":
    main()
