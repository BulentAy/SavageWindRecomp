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
with the optional 101–111 locomotion state (0 means unavailable). On BOTW 1.9.0
it appends the active clip key and source frame at up to 20 Hz for idle, ground
movement, turning, jumping, falling and landing. The key names a skeletal
leaf in Link's actual animation graph, including start and stop clips. The
ghost and its worn pieces seek to the same phase at their buffered pose time.
Outside these sequences, or when a clip is missing, the receiver uses the
coarse state. The worker never reads guest memory.

Rebuild the overlay with `python tools/coop_ghost_assets.py` and use the
updated relay and both updated clients. The tool generates the clip aliases
from the local RomFS and disables only the ghost NPC's automatic name label.
Never share the overlay. The strongest skeletal clip is mirrored, not the
full blend of simultaneous clips, secondary layers or foot IK.

Worn weapons travel as `WPN <slot> <Weapon_name> <3x4 pose>` (or `WPN <slot> -`
for an emptied slot; slots 0–5). The sender finds the `Weapon_*` actors the
game keeps on its Link (sword, sheath, shield, bow, quiver; near Link's axis and
off the ground) and sends each one's pose in Link's frame, so the same record
puts it on the back or in the hand. Changed slots go first, at most 10 records
per second, and worn slots are repeated about every 2 s for a late peer. The
relay forwards valid records as `WPN <id> ...` (name `Weapon_` plus up to 16
letters, digits or `_`, rigid rotation, translation within 4 m); older clients
ignore them. The receiver places a `CoopW_<kind>_<n>` copy (built by
`tools/coop_ghost_assets.py` from each weapon pack with a model) at the
ghost's pose times that relative pose every update, replaces it when the name
changes and deletes it with the ghost. A weapon with its own skeleton (Bokoblin
arms) plays its held pose, `Equiped`.

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
