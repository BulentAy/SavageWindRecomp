#!/usr/bin/env python3
"""Watch the co-op ghost on one PC: a loopback relay plus two windowed Savage Wind instances.

    python tools/coop_duo_local.py --save <your save folder> --settings <your savagewind.ini>

Each instance gets its own data root under --work (default: a new temporary folder) with COPIES of the save
folder and settings, so the originals are never written. The copies run windowed at 960x540 without upscaling or
frame generation, so two fit in memory. The ghost actor comes from tools/coop_ghost_assets.py (build/coop-overlay).
Load the same save region in both windows; each shows the other's Link. Close both windows (or Ctrl+C) to stop.
"""
import argparse
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parent.parent
INI = {'window_size': '960x540', 'fullscreen': '0', 'render_scale': '100', 'upscaler': 'off',
       'frame_generation': '0', 'dynamic_resolution': '0', 'discord_rpc': '0'}


def scratch_ini(text):
    """The settings with INI's values, set where the key is and appended otherwise."""
    lines, seen = [], set()
    for line in text.splitlines():
        key = line.split('=', 1)[0].strip()
        if '=' in line and not line.lstrip().startswith('#') and key in INI:
            line = f'{key} = {INI[key]}'
            seen.add(key)
        lines.append(line)
    return '\n'.join(lines + [f'{k} = {v}' for k, v in INI.items() if k not in seen]) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--save', type=Path, required=True, help='save folder to copy (never written)')
    ap.add_argument('--settings', type=Path, required=True, help='savagewind.ini to copy (never written)')
    ap.add_argument('--work', type=Path, help='new or empty folder for the two copies (default: a temporary one)')
    ap.add_argument('--exe', type=Path, default=REPO / 'build' / ('savagewind.exe' if os.name == 'nt' else 'savagewind'))
    ap.add_argument('--game', type=Path, default=REPO / 'extracted', help='game files (exefs/ and romfs/)')
    ap.add_argument('--overlay', type=Path, default=REPO / 'build' / 'coop-overlay')
    a = ap.parse_args()
    for p, what in ((a.save, 'save folder'), (a.game, 'game folder'), (a.overlay, 'ghost overlay (run tools/coop_ghost_assets.py)')):
        if not p.is_dir():
            ap.error(f'{what} not found: {p}')
    for p, what in ((a.settings, 'settings'), (a.exe, 'executable (build it first)')):
        if not p.is_file():
            ap.error(f'{what} not found: {p}')
    work = a.work or Path(tempfile.mkdtemp(prefix='savagewind-coop-'))
    if work.exists() and any(work.iterdir()):
        ap.error(f'--work must be new or empty: {work}')

    env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('SAVAGEWIND_', 'COOP_'))}
    ucrt = Path('C:/msys64/ucrt64/bin')  # (a source build's DLLs: docs/BUILD-windows.md)
    if os.name == 'nt' and ucrt.is_dir():
        env['PATH'] = f'{ucrt};{env.get("PATH", "")}'
    password = secrets.token_hex(16)  # this session only; never printed
    procs = []
    try:
        relay = subprocess.Popen([sys.executable, '-u', str(REPO / 'tools' / 'coop_relay.py'), '--port', '0'],
                                 env=dict(env, COOP_PASSWORD=password), stdout=subprocess.PIPE)
        procs.append(relay)
        listen = relay.stdout.readline().decode('ascii', 'replace').split()
        if listen[:1] != ['LISTEN']:
            sys.exit('relay did not start')
        endpoint = f'127.0.0.1:{listen[2]}'
        for n in (1, 2):
            root = work / f'p{n}'
            root.mkdir(parents=True)
            shutil.copytree(a.save, root / 'save')
            (root / 'savagewind.ini').write_text(scratch_ini(a.settings.read_text(encoding='utf-8')), encoding='utf-8')
            if (REPO / 'cache').is_dir():
                shutil.copytree(REPO / 'cache', root / 'cache')  # warm shaders; each copy writes its own
            child = dict(env, SAVAGEWIND_DATA_ROOT=str(root), SAVAGEWIND_SAVE_DIR=str(root / 'save'),
                         SAVAGEWIND_SETTINGS=str(root / 'savagewind.ini'), SAVAGEWIND_NO_LAUNCHER='1',
                         SAVAGEWIND_ROMFS_OVERLAY=str(a.overlay.resolve()), SAVAGEWIND_COOP_ENDPOINT=endpoint,
                         SAVAGEWIND_COOP_PASSWORD=password)
            with (root / 'run.log').open('wb') as log:
                procs.append(subprocess.Popen([str(a.exe.resolve()), str(a.game.resolve())], cwd=root, env=child,
                                              stdout=log, stderr=subprocess.STDOUT))
            print(f'player {n}: log {root / "run.log"}', flush=True)
        print(f'relay {endpoint}; close both game windows or press Ctrl+C to stop', flush=True)
        for p in procs[1:]:
            p.wait()
    except KeyboardInterrupt:
        pass
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
                p.wait()
    print(f'copies and logs kept in {work}')


if __name__ == '__main__':
    main()
