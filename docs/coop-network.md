# Co-op v5 pose transport (development)

The runtime transport is off unless `SAVAGEWIND_COOP_ENDPOINT` is set. This
branch does not feed game poses into the transport or render a remote actor.

- Run `python tools/coop_relay.py` with `COOP_PASSWORD` set, or pass
  `--password-file PATH` for a UTF-8 password file. The default bind is
  `127.0.0.1:47800`; use `--host` to select a private LAN/VPN interface.
- Set `SAVAGEWIND_COOP_ENDPOINT=127.0.0.1:47800` (or the relay's numeric IPv4
  address and port) and `SAVAGEWIND_COOP_PASSWORD` to the matching password.
- Use a high-entropy session password and a trusted LAN/private VPN. v5 does
  not encrypt traffic or authenticate the server; captured challenges and
  digests permit offline guessing. Do not publish or port-forward the relay.

The worker authenticates with `HELLO`/`VER`/`AUTH`/`WELCOME`, sends `MTX`
without an animation field at up to 10 Hz, and receives relay-assigned peer
IDs. It tracks the first valid remote peer until `BYE` or disconnect; its pose
expires after two seconds of silence. The host API hands off copied matrices
via bounded queues; the game-thread caller drains events in order and keeps
only the latest pose. The worker never accesses guest memory.

Run `make test-coop-net` (or `make test-coop-net PY='py -3.12'` with Windows
UCRT64 and system Python). Tests use synthetic poses and loopback sockets only.
