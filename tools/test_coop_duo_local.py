#!/usr/bin/env python3
"""Synthetic checks for tools/coop_duo_local.py (no game, no relay)."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import coop_duo_local as duo  # noqa: E402


class CoopDuoLocal(unittest.TestCase):
    def test_ini_sets_existing_and_appends_missing(self):
        out = duo.scratch_ini('# upscaler = dlss\nupscaler = dlss\nwindow_size = 1920x1080\nvsync = fast\n')
        self.assertIn('# upscaler = dlss\nupscaler = off\nwindow_size = 960x540\nvsync = fast\n', out)
        self.assertEqual(out.count('frame_generation = 0'), 1)
        self.assertEqual(out.count('upscaler ='), 2)  # the comment and the value

    def refused(self, *args):
        with mock.patch.object(sys, 'argv', ['coop_duo_local.py', *args]), mock.patch('subprocess.Popen') as popen, \
                contextlib.redirect_stderr(io.StringIO()) as err, self.assertRaises(SystemExit):
            duo.main()
        popen.assert_not_called()
        return err.getvalue()

    def test_refusals_start_nothing(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            (t / 'save').mkdir()
            (t / 'game').mkdir()
            (t / 'overlay').mkdir()
            (t / 'work').mkdir()
            (t / 'work' / 'old').touch()
            for name in ('savagewind.ini', 'game.exe'):
                (t / name).touch()
            base = ['--save', str(t / 'save'), '--settings', str(t / 'savagewind.ini'), '--exe', str(t / 'game.exe'),
                    '--game', str(t / 'game'), '--overlay', str(t / 'overlay')]
            self.assertIn('ghost overlay', self.refused(*base[:-1], str(t / 'missing')))
            self.assertIn('--work must be new or empty', self.refused(*base, '--work', str(t / 'work')))
            self.assertIn('settings not found', self.refused(*base[:2], '--settings', str(t / 'x.ini'), *base[4:]))


if __name__ == '__main__':
    unittest.main()
