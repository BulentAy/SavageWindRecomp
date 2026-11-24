#!/usr/bin/env python3
"""Self-contained tests for coop_ghost_assets.py. No game files needed: a tiny RomFS is built by hand.

    python tools/test_coop_ghost_assets.py
"""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import coop_ghost_assets as g  # noqa: E402
from botw_save_convert import yaz0_decompress  # noqa: E402


def param(name, t, v):
    return [g.crc(name), t, v]


def as_list(*names):
    a = g.Aamp.__new__(g.Aamp)
    a.flags, a.pio_version, a.doc_type = 3, 0, b'xml'
    defines = [[g.crc('ASDefine_%d' % i), [param('Name', 8, n), param('Filename', 8, 'File' + n)]]
               for i, n in enumerate(names)]
    a.root = [g.crc('param_root'), [[g.crc('AddReses'), [], []], [g.crc('ASDefines'), [], defines]], []]
    return a.write()


def actor_link(**users):
    obj = [g.crc('LinkTarget'), [param(k, 20, v) for k, v in users.items()]]
    tags = [g.crc('Tags'), [param('Tag0', 20, 'Npc')]]
    scale = [g.crc('Scale'), [param('Value', 1, struct.pack('<f', 1.0)), param('Raw', 19, b'\1\2\3')]]
    a = g.Aamp.__new__(g.Aamp)
    a.flags, a.pio_version, a.doc_type = 3, 0, b'xml'
    a.root = [g.crc('param_root'), [[g.crc('child'), [], [scale]]], [obj, tags]]
    return a.write()


USERS = ('ModelUser', 'ASUser', 'AIProgramUser') + g.DUMMY


def make_romfs(root):
    def put(rel, data):
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(data)

    donor = g.sarc_write([
        ('Actor/ActorLink/%s.bxml' % g.DONOR, actor_link(**{u: 'Monk' + u for u in USERS}), 4),
        ('Actor/AIProgram/Wait.baiprog', b'AI' * 10, 8),
        ('Actor/Shared.bin', b'monk', 4)])
    look = g.sarc_write([
        ('Actor/ActorLink/%s.bxml' % g.LOOK, actor_link(**{u: 'Menu' + u for u in USERS}), 8),
        ('Actor/ModelList/Menu.bmodellist', b'ML' * 7, 0x80),
        ('Actor/ASList/MenuASUser.baslist', as_list('Alpha', 'Zulu'), 4),
        ('Actor/AS/%s.bas' % g.IDLE[1], b'AS', 4),
        ('Actor/Shared.bin', b'menu', 4)])
    put('Actor/Pack/%s.sbactorpack' % g.DONOR, g.yaz0_literal(donor))
    put('Actor/Pack/%s.sbactorpack' % g.LOOK, g.yaz0_literal(look))
    names = sorted([g.DONOR, g.LOOK, 'Zz'], key=g.crc)
    actors = [{'name': n, 'bfres': n, 'instSize': 10, 'profile': 'NPC', 'aabbMin': {'X': -1.0}} for n in names]
    actors[names.index(g.LOOK)].update(mainModel='Link', bfres='Link', aabbMin={'X': -2.5})
    hashes = [g.crc(n) if g.crc(n) < 1 << 31 else g.U32(g.crc(n)) for n in names]
    put('Actor/ActorInfo.product.sbyml', g.yaz0_literal(g.byml_write({'Actors': actors, 'Hashes': hashes}, 2)))
    t = g.Rstb(b'RSTB' + bytes(8))
    t.set('Actor/Pack/%s.bactorpack' % g.DONOR, 5000)
    t.set('Actor/ActorLink/%s.bxml' % g.DONOR, 1000)
    t.set('Actor/ASList/MenuASUser.baslist', 2000)
    t.names['Some/Collided.name'] = 7
    put('System/Resource/ResourceSizeTable.product.srsizetable', g.yaz0_literal(t.write()))


def snapshot(root):
    out = {}
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            with open(p, 'rb') as fh:
                out[p] = fh.read()
    return out


class Formats(unittest.TestCase):
    def test_yaz0_literal(self):
        for data in (b'', b'a', bytes(range(256)) * 3):
            z = g.yaz0_literal(data, 0x80)
            self.assertEqual(yaz0_decompress(z), data)
            self.assertEqual(g.yaz0_align(z), 0x80)

    def test_sarc_roundtrip_and_alignment(self):
        files = [('a/b.bin', b'x' * 5, 4), ('big.bfres', b'y' * 3, 0x2000), ('c', b'', 4)]
        d = g.sarc_write(files)
        back = {n: (b, a) for n, b, a in g.sarc_read(d)}
        self.assertEqual({n: b for n, (b, _) in back.items()}, {n: b for n, b, _ in files})
        self.assertGreaterEqual(back['big.bfres'][1], 0x2000)
        self.assertEqual(g.sarc_write(g.sarc_read(d)), d)

    def test_aamp_roundtrip(self):
        d = actor_link(ModelUser='A', ASUser='A')
        a = g.Aamp(d)
        self.assertEqual(g.Aamp(a.write()).root, a.root)
        self.assertEqual(a.write(), d)
        self.assertEqual(g.link_target(a)['ModelUser'][2], 'A')

    def test_byml_types_and_sharing(self):
        doc = {'a': [1, g.U32(0xFFFFFFF0), 1.5, True, 'str', None, {'k': 'v'}], 'b': {'k': 'v'},
               'c': g.I64(-5), 'd': g.F64(0.25), 'e': g.U64(1 << 40), 'f': []}
        d = g.byml_write(doc, 3)
        back = g.TypedByml(d).parse()
        self.assertEqual(back, doc)
        self.assertIsInstance(back['a'][1], g.U32)
        self.assertIs(back['a'][3], True)
        self.assertIsInstance(back['c'], g.I64)
        twice = g.byml_write({'a': {'k': 'v'}, 'b': {'k': 'v'}, 'c': {'k': 'w'}}, 2)
        root, = struct.unpack_from('<I', twice, 12)
        a, b, c = struct.unpack_from('<IxxxxIxxxxI', twice, root + 8)  # values of the three entries
        self.assertEqual(a, b)  # equal containers written once
        self.assertNotEqual(a, c)

    def test_rstb_names(self):
        t = g.Rstb(b'RSTB' + bytes(8))
        t.set('x', 3)
        t.names['named'] = 9
        t.set('named', 10)
        back = g.Rstb(t.write())
        self.assertEqual((back.get('x'), back.get('named'), back.get('missing')), (3, 10, None))


class Build(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.romfs = os.path.join(self.tmp.name, 'romfs')
        self.out = os.path.join(self.tmp.name, 'overlay')
        make_romfs(self.romfs)

    def tearDown(self):
        self.tmp.cleanup()

    def read(self, rel):
        with open(os.path.join(self.out, rel), 'rb') as f:
            return yaz0_decompress(f.read())

    def test_builds_overlay_without_touching_romfs(self):
        before = snapshot(self.romfs)
        self.assertEqual(g.main(['--romfs', self.romfs, '--out', self.out, '--name', 'Ghost']), 0)
        self.assertEqual(snapshot(self.romfs), before)

        pack = {n: b for n, b, _ in g.sarc_read(self.read('Actor/Pack/Ghost.sbactorpack'))}
        self.assertNotIn('Actor/ActorLink/%s.bxml' % g.DONOR, pack)
        self.assertNotIn('Actor/ActorLink/%s.bxml' % g.LOOK, pack)
        self.assertEqual(pack['Actor/Shared.bin'], b'monk')
        self.assertIn('Actor/ModelList/Menu.bmodellist', pack)
        users = {k: v[2] for k, v in g.link_target(g.Aamp(pack['Actor/ActorLink/Ghost.bxml'])).items()}
        self.assertEqual(users['ModelUser'], 'MenuModelUser')
        self.assertEqual(users['ASUser'], 'Ghost')
        seqs = [[p[2] for p in o[1]] for o in g.Aamp(pack['Actor/ASList/Ghost.baslist']).objects()]
        self.assertEqual(seqs, [['Alpha', 'FileAlpha'], [g.IDLE[0], g.IDLE[1]], ['Zulu', 'FileZulu']])
        self.assertIn('Actor/ASList/MenuASUser.baslist', pack)
        self.assertEqual(users['AIProgramUser'], 'MonkAIProgramUser')
        for k in g.DUMMY:
            self.assertEqual(users[k], 'Dummy')

        info = g.TypedByml(self.read('Actor/ActorInfo.product.sbyml')).parse()
        names = [a['name'] for a in info['Actors']]
        self.assertEqual([h & 0xFFFFFFFF for h in info['Hashes']], [g.crc(n) for n in names])
        self.assertEqual(info['Hashes'], sorted(info['Hashes']))
        ghost = info['Actors'][names.index('Ghost')]
        self.assertEqual((ghost['bfres'], ghost['mainModel'], ghost['aabbMin'], ghost['profile']),
                         ('Link', 'Link', {'X': -2.5}, 'NPC'))
        self.assertEqual((ghost['instSize'], ghost['traverseDist']), (g.INST_SIZE, g.TRAVERSE_DIST))

        t = g.Rstb(self.read('System/Resource/ResourceSizeTable.product.srsizetable'))
        self.assertEqual(t.get('Some/Collided.name'), 7)
        self.assertGreaterEqual(t.get('Actor/ActorLink/Ghost.bxml'), 1000)
        self.assertGreater(t.get('Actor/ASList/Ghost.baslist'), 2000)
        self.assertGreaterEqual(t.get('Actor/Pack/Ghost.bactorpack'), 64 << 10)
        self.assertEqual(t.get('Actor/Pack/%s.bactorpack' % g.DONOR), 5000)

    def test_refuses_out_inside_romfs(self):
        with self.assertRaises(SystemExit):
            g.main(['--romfs', self.romfs, '--out', os.path.join(self.romfs, 'x')])
        self.assertFalse(os.path.exists(os.path.join(self.romfs, 'x')))

    def test_missing_input_fails_cleanly(self):
        os.remove(os.path.join(self.romfs, 'Actor/Pack/%s.sbactorpack' % g.LOOK))
        self.assertEqual(g.main(['--romfs', self.romfs, '--out', self.out]), 1)
        self.assertFalse(os.path.exists(self.out))

    def test_refuses_taken_size_entry(self):
        rel = 'System/Resource/ResourceSizeTable.product.srsizetable'
        with open(os.path.join(self.romfs, rel), 'rb') as f:
            t = g.Rstb(yaz0_decompress(f.read()))
        t.crcs[g.crc('Actor/Pack/Ghost.bactorpack')] = 1  # as if a game path had the same crc32
        with open(os.path.join(self.romfs, rel), 'wb') as f:
            f.write(g.yaz0_literal(t.write()))
        self.assertEqual(g.main(['--romfs', self.romfs, '--out', self.out, '--name', 'Ghost']), 1)

    def test_refuses_existing_name_and_bad_name(self):
        self.assertEqual(g.main(['--romfs', self.romfs, '--out', self.out, '--name', 'Zz']), 1)
        with self.assertRaises(SystemExit):
            g.main(['--romfs', self.romfs, '--out', self.out, '--name', '../x'])


if __name__ == '__main__':
    unittest.main()
