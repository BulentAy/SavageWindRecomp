# Co-op v5 pose transport (development)

The runtime transport is off unless `SAVAGEWIND_COOP_ENDPOINT` is set. With it,
each game update (the VFR hook in `runtime/hle/coop.c`) hands Link's 3x4 matrix
to the worker and places one ghost actor (`SAVAGEWIND_COOP_ACTOR`, default
`CoopPlayer` from `tools/coop_ghost_assets.py`, loaded with
`SAVAGEWIND_ROMFS_OVERLAY`) at the first remote peer's pose: spawned once a
pose arrives within 120 m, translation interpolated 120 ms behind the newest
pose with the newest rotation, written to the actor matrix every update, and
deleted on `BYE` or after 2 s without poses. World exits and reboots forget the
actor without touching it. `[coop] TRACK` lines (10 per second) log local and
remote positions, the placed pose and the next-update readback error.

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

To watch two instances on one PC (each with copies of the save and settings,
windowed at 960x540), build the overlay once and run
`python tools/coop_duo_local.py --save <save folder> --settings <savagewind.ini>`.
It starts a loopback relay with a one-time password and both windows; closing
both windows stops it. Only the focused window takes keyboard or controller
input: click a window to move its Link, then watch it in the other window.

Run `make test-coop-net` (or `make test-coop-net PY='py -3.12'` with Windows
UCRT64 and system Python). Tests use synthetic poses and loopback sockets only.
