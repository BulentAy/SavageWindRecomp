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

The worker authenticates with `HELLO`/`VER`/`AUTH`/`WELCOME` and sends `MTX`
at up to 10 Hz. The optional trailing integer carries the sender's locomotion
state (101–111 in order: idle, walk, run, sprint, jump, fall, climb, climb idle,
swim, swim idle, glide; 0 means unavailable). The relay forwards it unchanged.
The sender reads Link's own animation sequence for special movement states and
uses horizontal speed for the ground gaits. The receiver plays the state at
its interpolated pose time, changing clips only on transitions. Missing or
unknown states still place the ghost and play idle. The worker never accesses
guest memory; it transfers copied poses and states via bounded queues.

The animation bridge is optional and version-pinned to BOTW 1.9.0. Rebuild
an existing idle-only overlay with `python tools/coop_ghost_assets.py`.
The new sequences use locally installed Link clips; never share the overlay.
Climb, swim and glide use representative clips, not the full player animation
blend graph or a visible paraglider.

To watch two instances on one PC (each with copies of the save and settings,
windowed at 960x540), build the overlay once and run
`python tools/coop_duo_local.py --save <save folder> --settings <savagewind.ini>`.
It starts a loopback relay with a one-time password and both windows; closing
both windows stops it. Only the focused window takes keyboard or controller
input: click a window to move its Link, then watch it in the other window.

Run `make test-coop-net` (or `make test-coop-net PY='py -3.12'` with Windows
UCRT64 and system Python). Tests use synthetic poses and loopback sockets only.
