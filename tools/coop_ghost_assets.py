#!/usr/bin/env python3
"""Build the co-op spike's idle ghost Link actor from YOUR game's own RomFS, into a RomFS overlay folder.

    python tools/coop_ghost_assets.py [--romfs extracted/romfs] [--out build/coop-overlay] [--name CoopPlayer]

then run the game with SAVAGEWIND_ROMFS_OVERLAY=<out> SAVAGEWIND_COOP_TEST=local SAVAGEWIND_COOP_ACTOR=<name>.

The actor is the Sheikah monk's actor pack (Npc_DungeonPriest001: an NPC that just stands and waits) whose ActorLink
takes the pause-menu Link's model (PauseMenuPlayer), an animation list of its own (the menu Link's, plus the "Wait"
sequence the monk's AI plays, as the menu's idle animation) and no physics, attention, chemistry or effects (users set
to Dummy): a Link that is drawn, stands idle and nothing touches. Three files are written:

    Actor/Pack/<name>.sbactorpack                      the new pack (the monk's and the menu Link's resources)
    Actor/ActorInfo.product.sbyml                      the game's actor table plus an entry for <name>
    System/Resource/ResourceSizeTable.product.srsizetable  the size table plus entries for the new files

Nothing from the game is embedded in this script: it only reads the given RomFS and writes into --out, never into the
RomFS. The output is the player's own game data: keep it out of git (build/ is ignored) and never share it.
"""
import argparse
import bisect
import os
import struct
import sys
import zlib

from botw_save_convert import Byml, yaz0_decompress

DONOR = 'Npc_DungeonPriest001'  # behaviour: the monk's idle AI
LOOK = 'PauseMenuPlayer'        # model and animations: the menu's Link
# ActorLink users taken from LOOK (the animation list is LOOK's plus IDLE), and the ones that would give the ghost
# a body or reactions
FROM_LOOK = ('ModelUser', 'ASUser')
IDLE = ('Wait', 'Player_PauseMenuWait')  # the sequence the monk's AI plays, as the menu Link's idle animation
DUMMY = ('PhysicsUser', 'AttentionUser', 'ChemicalUser', 'ElinkUser', 'SlinkUser', 'XlinkUser')
# ActorInfo keys describing the model (the rest stays the monk's)
MODEL_KEYS = ('bfres', 'mainModel', 'boundingForTraverse', 'aabbMin', 'aabbMax')
INST_SIZE = 700000      # actor heap; the monk's is sized for its own model, Link's needs more
TRAVERSE_DIST = 99999   # never unloaded for distance while it stands next to the player


def crc(name):
    return zlib.crc32(name.encode())


# ---------------------------------------------------------------------------------------------------------
# Yaz0
# ---------------------------------------------------------------------------------------------------------

def yaz0_literal(d, align=0):
    """Yaz0 with literal bytes only (a 0xFF flag before every 8), what the game's decoder reads.
    shortcut: ~12% bigger than the game's own packing, write a matcher if a loader turns out to size by it."""
    out = bytearray(b'Yaz0' + struct.pack('>II', len(d), align) + bytes(4))
    for i in range(0, len(d), 8):
        out.append(0xFF)
        out += d[i:i + 8]
    return bytes(out)


def yaz0_align(d):
    return struct.unpack_from('>I', d, 8)[0] if d[:4] == b'Yaz0' else 0


# ---------------------------------------------------------------------------------------------------------
# SARC (little endian): [(name, data, alignment)]
# ---------------------------------------------------------------------------------------------------------

def sarc_name_hash(name, key=0x65):
    h = 0
    for b in name.encode():
        h = (h * key + b) & 0xFFFFFFFF
    return h


def sarc_read(d):
    if d[:4] != b'SARC' or d[6:8] != b'\xff\xfe':
        raise ValueError('not a little-endian SARC archive')
    data_off, = struct.unpack_from('<I', d, 0x0C)
    if d[0x14:0x18] != b'SFAT':
        raise ValueError('SARC: SFAT missing')
    nodes, = struct.unpack_from('<H', d, 0x1A)
    names = 0x20 + nodes * 16 + 8
    out = []
    for i in range(nodes):
        _, attr, start, end = struct.unpack_from('<IIII', d, 0x20 + i * 16)
        if not attr >> 24:
            raise ValueError('SARC: nameless file')
        o = names + (attr & 0xFFFFFF) * 4
        a = data_off + start
        # the archive keeps each file at the alignment its loader needs; the lowest set bit of where it is covers it
        out.append((d[o:d.index(b'\0', o)].decode(), d[a:data_off + end], min(a & -a, 0x2000)))
    return out


def sarc_write(files):
    files = sorted(files, key=lambda f: sarc_name_hash(f[0]))
    hashes = [sarc_name_hash(f[0]) for f in files]
    if len(set(hashes)) != len(hashes):
        raise ValueError('SARC: two names with the same hash')
    sfnt = bytearray()
    name_offs = []
    for name, _, _ in files:
        name_offs.append(len(sfnt) // 4)
        sfnt += name.encode() + b'\0'
        sfnt += bytes(-len(sfnt) % 4)
    head = 0x14 + 0x0C + 16 * len(files) + 8 + len(sfnt)
    data_off = head + (-head % max([f[2] for f in files] + [4]))
    data = bytearray()
    nodes = bytearray()
    for (name, blob, align), h, no in zip(files, hashes, name_offs):
        data += bytes(-(data_off + len(data)) % align)
        nodes += struct.pack('<IIII', h, 0x01000000 | no, len(data), len(data) + len(blob))
        data += blob
    total = data_off + len(data)
    out = bytearray(b'SARC' + struct.pack('<HHIIHH', 0x14, 0xFEFF, total, data_off, 0x100, 0))
    out += b'SFAT' + struct.pack('<HHI', 0x0C, len(files), 0x65) + nodes
    out += b'SFNT' + struct.pack('<HH', 8, 0) + sfnt
    out += bytes(data_off - len(out)) + data
    return bytes(out)


# ---------------------------------------------------------------------------------------------------------
# AAMP v2 (parameter archives: ActorLink and the other resource lists of an actor)
#   list:  [crc, [child lists], [objects]]     object: [crc, [params]]     param: [crc, type, value]
# String params' values are str; every other value stays its raw bytes.
# ---------------------------------------------------------------------------------------------------------

AAMP_STRINGS = {7, 8, 15, 20}           # string32, string64, string256, stringRef
AAMP_BUFFERS = {13: 4, 14: 4, 18: 4, 19: 1}  # buffer int, f32, u32, binary: element size
AAMP_SIZES = {0: 4, 1: 4, 2: 4, 3: 8, 4: 12, 5: 16, 6: 16, 9: 0x80, 10: 0x100, 11: 0x180, 12: 0x200, 16: 16, 17: 4}


class Aamp:
    def __init__(self, d):
        if d[:4] != b'AAMP' or struct.unpack_from('<I', d, 4)[0] != 2:
            raise ValueError('not an AAMP v2 file')
        self.flags, = struct.unpack_from('<I', d, 8)
        if self.flags & 1 == 0:
            raise ValueError('AAMP: big endian')
        self.pio_version, pio_off = struct.unpack_from('<II', d, 0x10)
        self.doc_type = d[0x30:d.index(b'\0', 0x30)]
        self.root = self._list(d, 0x30 + pio_off)

    @staticmethod
    def _list(d, o):
        h, lo, nl, oo, no = struct.unpack_from('<IHHHH', d, o)
        return [h, [Aamp._list(d, o + lo * 4 + i * 12) for i in range(nl)],
                [Aamp._obj(d, o + oo * 4 + i * 8) for i in range(no)]]

    @staticmethod
    def _obj(d, o):
        h, po, n = struct.unpack_from('<IHH', d, o)
        return [h, [Aamp._param(d, o + po * 4 + i * 8) for i in range(n)]]

    @staticmethod
    def _param(d, o):
        h, w = struct.unpack_from('<II', d, o)
        t, a = w >> 24, o + (w & 0xFFFFFF) * 4
        if t in AAMP_STRINGS:
            return [h, t, d[a:d.index(b'\0', a)].decode()]
        if t in AAMP_BUFFERS:
            n, = struct.unpack_from('<I', d, a - 4)
            return [h, t, d[a:a + n * AAMP_BUFFERS[t]]]
        if t not in AAMP_SIZES:
            raise ValueError('AAMP: unknown parameter type %d' % t)
        return [h, t, d[a:a + AAMP_SIZES[t]]]

    def objects(self, lst=None):
        """Every object, depth first."""
        lst = lst or self.root
        yield from lst[2]
        for child in lst[1]:
            yield from self.objects(child)

    def write(self):
        lists, objs, params = [], [], []
        queue = [self.root]  # breadth first: each list's children end up next to each other
        while queue:
            lst = queue.pop(0)
            lists.append(lst)
            queue += lst[1]
        for lst in lists:
            objs += lst[2]
        for obj in objs:
            params += obj[1]
        doc = self.doc_type + b'\0'
        doc += bytes(-len(doc) % 4)
        lists_at = 0x30 + len(doc)
        objs_at = lists_at + 12 * len(lists)
        params_at = objs_at + 8 * len(objs)
        data_at = params_at + 8 * len(params)
        data, strings, seen = bytearray(), bytearray(), {}
        where = []
        for _, t, v in params:  # data section first: the string section's start depends on its size
            if t in AAMP_STRINGS:
                where.append(None)
                continue
            if t in AAMP_BUFFERS:
                data += struct.pack('<I', len(v) // AAMP_BUFFERS[t])
            where.append(data_at + len(data))
            data += v + bytes(-len(v) % 4)
        strings_at = data_at + len(data)
        for i, (_, t, v) in enumerate(params):
            if t in AAMP_STRINGS:
                if v not in seen:
                    seen[v] = strings_at + len(strings)
                    strings += v.encode() + b'\0'
                    strings += bytes(-len(strings) % 4)
                where[i] = seen[v]
        out = bytearray()
        child = {id(lst): i for i, lst in enumerate(lists)}
        first_obj, n = {}, 0
        for lst in lists:
            first_obj[id(lst)] = n
            n += len(lst[2])
        for i, lst in enumerate(lists):
            at = lists_at + 12 * i
            lo = lists_at + 12 * child[id(lst[1][0])] if lst[1] else at
            oo = objs_at + 8 * first_obj[id(lst)] if lst[2] else at
            out += struct.pack('<IHHHH', lst[0], (lo - at) // 4, len(lst[1]), (oo - at) // 4, len(lst[2]))
        n = 0
        for i, obj in enumerate(objs):
            at = objs_at + 8 * i
            po = params_at + 8 * n if obj[1] else at
            out += struct.pack('<IHH', obj[0], (po - at) // 4, len(obj[1]))
            n += len(obj[1])
        for i, (h, t, _) in enumerate(params):
            at = params_at + 8 * i
            rel = (where[i] - at) // 4
            if rel >= 1 << 24:
                raise ValueError('AAMP: file too big')
            out += struct.pack('<II', h, rel | t << 24)
        size = strings_at + len(strings)
        head = b'AAMP' + struct.pack('<IIIIIIIIIII', 2, self.flags, size, self.pio_version, len(doc), len(lists),
                                     len(objs), len(params), len(data), len(strings), 0)
        return head + doc + bytes(out) + bytes(data) + bytes(strings)


# ---------------------------------------------------------------------------------------------------------
# BYML v2/v3, little endian, with its integer types kept so that the table writes back as it was
# ---------------------------------------------------------------------------------------------------------

class U32(int): pass
class I64(int): pass
class U64(int): pass
class F64(float): pass


class TypedByml(Byml):
    def _value(self, t, v):
        if t == 0xD3:
            return U32(v)
        if t in (0xD4, 0xD5, 0xD6):
            return {0xD4: I64, 0xD5: U64, 0xD6: F64}[t](super()._value(t, v))
        return super()._value(t, v)


def byml_write(root, version):
    key_set, str_set = set(), set()

    def collect(n):
        """n's identity for sharing equal containers, as the game's own tables do"""
        if isinstance(n, dict):
            key_set.update(n)
            ident[id(n)] = k = ('d',) + tuple((k, collect(v)) for k, v in sorted(n.items()))
            return k
        if isinstance(n, list):
            ident[id(n)] = k = ('l',) + tuple(collect(v) for v in n)
            return k
        if isinstance(n, str):
            str_set.add(n)
        return type(n).__name__, n
    ident = {}
    collect(root)
    keys = sorted(key_set, key=str.encode)
    strs = sorted(str_set, key=str.encode)
    key_ix = {k: i for i, k in enumerate(keys)}
    str_ix = {s: i for i, s in enumerate(strs)}
    out = bytearray(16)

    def align():
        out.extend(bytes(-len(out) % 4))

    def table(items):
        if not items:
            return 0
        align()
        at = len(out)
        blobs = [s.encode() + b'\0' for s in items]
        offs, o = [], 4 + 4 * (len(items) + 1)
        for b in blobs:
            offs.append(o)
            o += len(b)
        out.extend(struct.pack('<I', 0xC2 | len(items) << 8) + struct.pack('<%dI' % (len(offs) + 1), *offs, o))
        for b in blobs:
            out.extend(b)
        return at

    def scalar(v):
        """(type, u32 value or None for containers)"""
        if isinstance(v, dict):
            return 0xC1, None
        if isinstance(v, list):
            return 0xC0, None
        if isinstance(v, str):
            return 0xA0, str_ix[v]
        if isinstance(v, bool):
            return 0xD0, int(v)
        if v is None:
            return 0xFF, 0
        for cls, t, fmt in ((I64, 0xD4, '<q'), (U64, 0xD5, '<Q'), (F64, 0xD6, '<d')):
            if isinstance(v, cls):  # 8-byte values live outside the node
                align()
                out.extend(b'\0\0\0\0')  # (v3 keeps them 8-aligned after the node data; any aligned place reads)
                at = len(out)
                out.extend(struct.pack(fmt, v))
                return t, at
        if isinstance(v, U32):
            return 0xD3, v
        if isinstance(v, int):
            return 0xD1, v & 0xFFFFFFFF
        if isinstance(v, float):
            return 0xD2, struct.unpack('<I', struct.pack('<f', v))[0]
        raise ValueError('BYML: cannot write %r' % type(v))

    shared = {}

    def node(n):
        if ident[id(n)] in shared:
            return shared[ident[id(n)]]
        align()
        at = shared[ident[id(n)]] = len(out)
        if isinstance(n, dict):
            items = sorted(n.items(), key=lambda kv: key_ix[kv[0]])
            out.extend(struct.pack('<I', 0xC1 | len(items) << 8) + bytes(8 * len(items)))
            for i, (k, v) in enumerate(items):
                t, val = scalar(v)
                if val is None:
                    val = node(v)
                struct.pack_into('<II', out, at + 4 + 8 * i, key_ix[k] | t << 24, val)
        else:
            types = [scalar(v) if not isinstance(v, (dict, list)) else (0xC1 if isinstance(v, dict) else 0xC0, None)
                     for v in n]
            base = at + 4 + ((len(n) + 3) & ~3)
            out.extend(struct.pack('<I', 0xC0 | len(n) << 8) + bytes(t for t, _ in types))
            out.extend(bytes(base - len(out)) + bytes(4 * len(n)))
            for i, (v, (t, val)) in enumerate(zip(n, types)):
                if val is None:
                    val = node(v)
                struct.pack_into('<I', out, base + 4 * i, val)
        return at

    key_at, str_at = table(keys), table(strs)
    root_at = node(root)
    struct.pack_into('<2sHIII', out, 0, b'YB', version, key_at, str_at, root_at)
    return bytes(out)


# ---------------------------------------------------------------------------------------------------------
# ResourceSizeTable: "RSTB", u32 crc count, u32 name count, {crc32(path), size} sorted, {char[128], size}
# ---------------------------------------------------------------------------------------------------------

class Rstb:
    def __init__(self, d):
        if d[:4] != b'RSTB':
            raise ValueError('not a ResourceSizeTable')
        nc, nn = struct.unpack_from('<II', d, 4)
        self.crcs = dict(struct.iter_unpack('<II', d[12:12 + 8 * nc]))
        self.names = {}
        for i in range(nn):
            o = 12 + 8 * nc + 132 * i
            self.names[d[o:o + 128].split(b'\0')[0].decode()] = struct.unpack_from('<I', d, o + 128)[0]

    def get(self, path):
        return self.names.get(path, self.crcs.get(crc(path)))

    def set(self, path, size):
        if path in self.names:
            self.names[path] = size
        else:
            self.crcs[crc(path)] = size

    def write(self):
        out = bytearray(b'RSTB' + struct.pack('<II', len(self.crcs), len(self.names)))
        for c in sorted(self.crcs):
            out += struct.pack('<II', c, self.crcs[c])
        for n, s in self.names.items():
            out += n.encode().ljust(128, b'\0') + struct.pack('<I', s)
        return bytes(out)


# ---------------------------------------------------------------------------------------------------------
# The ghost actor
# ---------------------------------------------------------------------------------------------------------

def link_target(link):
    """The ActorLink's user parameters: {name: param}"""
    names = {crc(n): n for n in ('ActorNameJpn', 'Priority', 'AIProgramUser') + FROM_LOOK + DUMMY}
    for obj in link.objects():
        if obj[0] == crc('LinkTarget'):
            return {names.get(p[0], p[0]): p for p in obj[1]}
    raise ValueError('ActorLink: no LinkTarget')


def add_sequence(aslist, seq, filename):
    """aslist (an ASList) with one more sequence, kept sorted by name like the game's own lists"""
    defines = next((l for l in aslist.root[1] if l[0] == crc('ASDefines')), None)
    if defines is None:
        raise ValueError('ASList: no ASDefines')
    entries = [{p[0]: p for p in o[1]} for o in defines[2]]
    if any(crc('Name') not in e or crc('Filename') not in e for e in entries):
        raise ValueError('ASList: a sequence without Name/Filename')
    if any(e[crc('Name')][2] == seq for e in entries):
        return
    entries.append({crc('Name'): [crc('Name'), 8, seq], crc('Filename'): [crc('Filename'), 8, filename]})
    entries.sort(key=lambda e: e[crc('Name')][2].encode())
    defines[2] = [[crc('ASDefine_%d' % i), [e[crc('Name')], e[crc('Filename')]]] for i, e in enumerate(entries)]


def build_pack(donor, look, name):
    """The new pack's files and, for each file new to the game, (path, the game file whose size entry it starts
    from, that file's length, the new length)."""
    files = {n: (n, d, a) for n, d, a in sarc_read(look)}
    files.update({n: (n, d, a) for n, d, a in sarc_read(donor)})  # the monk's resources win on a shared path
    links = [n for n in files if n.startswith('Actor/ActorLink/')]
    donor_link = 'Actor/ActorLink/%s.bxml' % DONOR
    look_link = 'Actor/ActorLink/%s.bxml' % LOOK
    if donor_link not in links or look_link not in links:
        raise ValueError('expected %s and %s in the packs' % (donor_link, look_link))
    link = Aamp(files[donor_link][1])
    users, theirs = link_target(link), link_target(Aamp(files[look_link][1]))
    for key in FROM_LOOK + DUMMY:
        if key not in users or users[key][1] not in AAMP_STRINGS:
            raise ValueError('ActorLink: %s missing' % key)
    for key in FROM_LOOK:
        users[key][2] = theirs[key][2]
    for key in DUMMY:
        users[key][2] = 'Dummy'
    # the menu Link has no sequence the monk's AI asks for: its own list under the ghost's name gets one
    look_as = 'Actor/ASList/%s.baslist' % theirs['ASUser'][2]
    idle_as = 'Actor/AS/%s.bas' % IDLE[1]
    if look_as not in files or idle_as not in files:
        raise ValueError('expected %s and %s in the %s pack' % (look_as, idle_as, LOOK))
    aslist = Aamp(files[look_as][1])
    add_sequence(aslist, *IDLE)
    users['ASUser'][2] = name
    out = [f for n, f in files.items() if n not in links]
    out.append(('Actor/ActorLink/%s.bxml' % name, link.write(), files[donor_link][2]))
    out.append(('Actor/ASList/%s.baslist' % name, aslist.write(), files[look_as][2]))
    return out, [(n, src, len(files[src][1]), len(d)) for (n, d, _), src in zip(out[-2:], (donor_link, look_as))]


def build_actor_info(info, name):
    actors, hashes = info['Actors'], info['Hashes']
    by_name = {a['name']: a for a in actors}
    if name in by_name:
        raise ValueError('ActorInfo already has %s' % name)
    entry = dict(by_name[DONOR])
    look = by_name[LOOK]
    for k in MODEL_KEYS:
        entry.pop(k, None)
        if k in look:
            entry[k] = look[k]
    entry['name'] = name
    entry['instSize'] = INST_SIZE
    entry['traverseDist'] = TRAVERSE_DIST
    if [h & 0xFFFFFFFF for h in hashes] != sorted(h & 0xFFFFFFFF for h in hashes) or len(hashes) != len(actors):
        raise ValueError('ActorInfo: Hashes not sorted alongside Actors')
    h = crc(name)
    i = bisect.bisect_left([x & 0xFFFFFFFF for x in hashes], h)
    actors.insert(i, entry)
    hashes.insert(i, h if h < 1 << 31 else U32(h))  # the table's own convention: int when it fits, else uint
    return info


def rsize(n):
    return (n + 31) & ~31


def build(romfs, out_dir, name):
    def read(rel):
        with open(os.path.join(romfs, rel), 'rb') as f:
            return f.read()

    pack_rel = 'Actor/Pack/%s.sbactorpack'
    info_rel = 'Actor/ActorInfo.product.sbyml'
    rstb_rel = 'System/Resource/ResourceSizeTable.product.srsizetable'
    donor_z = read(pack_rel % DONOR)
    donor, look = yaz0_decompress(donor_z), yaz0_decompress(read(pack_rel % LOOK))
    files, derived = build_pack(donor, look, name)
    pack = sarc_write(files)

    info_z = read(info_rel)
    info_raw = yaz0_decompress(info_z)
    info_doc = TypedByml(info_raw)
    version = info_doc._u(2, 2)
    info = build_actor_info(info_doc.parse(), name)
    info_new = byml_write(info, version)

    table = Rstb(yaz0_decompress(read(rstb_rel)))
    sizes = {}
    pack_size = 'Actor/Pack/%s.bactorpack'  # (the table names a Yaz0 file without the 's')
    for rel, src, was, now in derived + [(pack_size % name, pack_size % DONOR, len(donor), len(pack))]:
        base = table.get(src)
        if base is None:
            raise ValueError('RSTB: no entry for %s' % src)
        if table.get(rel) is not None:  # (a game path with the same crc32 would need the name table)
            raise ValueError('RSTB: %s already has an entry' % rel)
        # the game file's room in proportion plus 1 KB: a bigger list sized like the original failed to create
        # (the reference's finding); a pack also gets the reference's 2x / 64 KB floor
        size = base * rsize(now) // rsize(was) + 1024
        if rel.startswith('Actor/Pack/'):
            size = max(size, 2 * rsize(now), 64 << 10)
        table.set(rel, size)
        sizes[rel] = size
    info_path = 'Actor/ActorInfo.product.byml'
    old = table.get(info_path)
    if old is not None:
        table.set(info_path, old + max(0, rsize(len(info_new)) - rsize(len(info_raw))))

    written = {
        pack_rel % name: yaz0_literal(pack, yaz0_align(donor_z)),
        info_rel: yaz0_literal(info_new, yaz0_align(info_z)),
        rstb_rel: yaz0_literal(table.write()),
    }
    for rel, blob in written.items():
        path = os.path.join(out_dir, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(blob)
    return written, sizes, len(files)


def inside(path, parent):
    path, parent = os.path.realpath(path), os.path.realpath(parent)
    return os.path.normcase(os.path.commonpath([path, parent])) == os.path.normcase(parent)


def main(argv=None):
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--romfs', default=os.path.join(repo, 'extracted', 'romfs'), help='your game\'s RomFS (read only)')
    ap.add_argument('--out', default=os.path.join(repo, 'build', 'coop-overlay'), help='overlay folder to write')
    ap.add_argument('--name', default='CoopPlayer', help='actor name (ASCII, up to 31 characters)')
    a = ap.parse_args(argv)
    if not a.name.isascii() or not a.name.isidentifier() or len(a.name) > 31:
        ap.error('--name: ASCII letters, digits and _ only, up to 31 characters')
    if not os.path.isdir(os.path.join(a.romfs, 'Actor', 'Pack')):
        ap.error('--romfs: %s has no Actor/Pack folder' % a.romfs)
    if inside(a.out, a.romfs) or inside(a.romfs, a.out):
        ap.error('--out must be outside the RomFS')
    try:
        written, sizes, n = build(a.romfs, a.out, a.name)
    except (OSError, ValueError, KeyError) as e:
        print('coop_ghost_assets: %s' % e, file=sys.stderr)
        return 1
    print('%s: %d files in the pack' % (a.name, n))
    for rel, blob in written.items():
        print('  %-56s %9d bytes' % (rel, len(blob)))
    for rel, size in sizes.items():
        print('  RSTB %-51s %9d' % (rel, size))
    print('overlay: %s' % os.path.abspath(a.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
