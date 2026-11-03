"""Shared PC HOT archive read/repack (Voodoo Vince Remastered). Stdlib only, reads bytes, never writes files.

Layout (cookbook C0026): 28-byte header `<4s6I` = ('HOT ', 1, hdr_start, data_start, total, name_table, count);
entries at 0x1C, 32 B `<Q4IQ` = (name_off, hdr_size, hdr_off, raw, packed, data_off). raw = whole payload size
(header + data); packed = zlib-stored size of the data part, 0 when stored raw. Name pool at name_table, NUL-terminated
basenames. Regions after the table, in file order: name pool, entry headers [hdr_start, data_start), entry data.
Xbox 0x18-byte entries are refused (name_table != 0x1C + 32 * count).

Padding (measured on the stock archives, T2.c1):
- hdr_size == 0 archives (modelsAndAnims, world, nested anims/models/data/verts): each data blob is followed by one
  NUL, then zeros to a 4-byte boundary (VinceJanicePupilFix/make_pupilfix.py:hot_pack rule).
- textures.hot: data blobs aligned to 128, no forced NUL.
- sounds.hot: gaps of 0..32 zero bytes that no position or size rule reproduces.
So each data blob's trailing gap is kept as parsed bytes and re-emitted for untouched entries; a changed entry gets
the rule pad (NUL + align 4 when the archive has no entry headers, else align to the archive's data alignment).

Sound banks: entry header = 72-byte RIFF/WAVE header, data = PCM. On carnival/area_insidekosmobot/sounds.hot (20
sounds) the RIFF `data` chunk size at hdr+68 equals the u32 at hdr+hdr_size-4 for every sound: with hdr_size 72 they
are the same field, and it equals raw - hdr_size (Noise overload/vince_volume_ab.py:88-89 reads it as hdr+hlen-4).

Every read is bounds-checked; malformed input raises Refused.
"""
import struct
import zlib
from collections import namedtuple

HEADER = struct.Struct('<4s6I')
ENTRY = struct.Struct('<Q4IQ')
TABLE = HEADER.size  # 0x1C

Entry = namedtuple('Entry', 'name name_off hdr_size hdr_off raw packed data_off')


class Refused(Exception):
    """The bytes are not a well-formed PC HOT archive (or bank) this module accepts."""


class Archive:
    """Parsed archive: header fields, ordered entries, name pool, and the parsed bytes they index."""

    def __init__(self, data, version, hdr_start, data_start, name_table, entries, pool, gaps, data_align):
        self.data = data
        self.version = version
        self.hdr_start = hdr_start
        self.data_start = data_start
        self.name_table = name_table
        self.entries = entries
        self.pool = pool
        self.gaps = gaps  # entry index -> zero-or-more bytes kept after that entry's stored data
        self.data_align = data_align
        self.by_name = {e.name: i for i, e in enumerate(entries)}

    def entry(self, name):
        i = self.by_name.get(name)
        if i is None:
            raise Refused(f'no entry {name!r}')
        return self.entries[i]

    def stored(self, name):
        """The entry's data bytes as stored in the archive (zlib stream when packed)."""
        e = self.entry(name)
        return self.data[e.data_off:e.data_off + stored_size(e)]

    def header(self, name):
        e = self.entry(name)
        return self.data[e.hdr_off:e.hdr_off + e.hdr_size]

    def payload(self, name):
        """Entry header + data, the data zlib-decompressed when packed; size checked against raw."""
        e = self.entry(name)
        blob = self.stored(name)
        if e.packed:
            d = zlib.decompressobj()
            try:
                blob = d.decompress(blob, e.raw - e.hdr_size + 1)
            except zlib.error as x:
                raise Refused(f'{name}: bad zlib stream: {x}') from None
            if not d.eof or d.unused_data or d.unconsumed_tail:
                raise Refused(f'{name}: zlib stream size mismatch')
        out = self.header(name) + blob
        if len(out) != e.raw:
            raise Refused(f'{name}: payload {len(out)} B, table says {e.raw}')
        return out


def stored_size(e):
    return e.packed or e.raw - e.hdr_size


def parse(data):
    """bytes -> Archive; raises Refused on anything out of bounds or unexpected."""
    data = bytes(data)
    n = len(data)
    if n < TABLE:
        raise Refused('truncated header')
    magic, version, hdr_start, data_start, total, name_table, count = HEADER.unpack_from(data, 0)
    if magic != b'HOT ':
        raise Refused('not a HOT archive')
    if version != 1:
        raise Refused(f'unsupported version {version}')
    if total != n:
        raise Refused(f'total size {total} != file size {n}')
    if count > (n - TABLE) // ENTRY.size or name_table != TABLE + ENTRY.size * count:
        raise Refused('entry count / name table mismatch (Xbox 0x18 entries are not supported)')
    if not name_table <= hdr_start <= data_start <= n:
        raise Refused('header/data start out of range')
    entries, pool_end = [], name_table
    for i in range(count):
        name_off, hs, ho, raw, packed, off = ENTRY.unpack_from(data, TABLE + ENTRY.size * i)
        start = name_table + name_off
        if name_off >= hdr_start - name_table:
            raise Refused(f'entry {i}: name offset out of range')
        end = data.find(b'\0', start, hdr_start)
        if end <= start:
            raise Refused(f'entry {i}: name not NUL-terminated inside the pool')
        try:
            name = data[start:end].decode('ascii')
        except UnicodeDecodeError:
            raise Refused(f'entry {i}: non-ASCII name') from None
        pool_end = max(pool_end, end + 1)
        if hs and not (hdr_start <= ho and ho + hs <= data_start):
            raise Refused(f'{name}: header out of range')
        if raw < hs:
            raise Refused(f'{name}: size smaller than header')
        e = Entry(name, name_off, hs, ho, raw, packed, off)
        if off < data_start or off + stored_size(e) > n:
            raise Refused(f'{name}: data out of range')
        entries.append(e)
    if len({e.name for e in entries}) != count:
        raise Refused('duplicate entry names')
    _no_overlap([(e.hdr_off, e.hdr_size) for e in entries if e.hdr_size], hdr_start, 'header')
    order = sorted(range(count), key=lambda i: entries[i].data_off)
    _no_overlap([(entries[i].data_off, stored_size(entries[i])) for i in order], data_start, 'data')
    gaps = {}
    for k, i in enumerate(order):
        end = entries[i].data_off + stored_size(entries[i])
        nxt = entries[order[k + 1]].data_off if k + 1 < count else n
        gaps[i] = data[end:nxt]
    align = 128
    while align > 4 and any(e.data_off % align for e in entries):
        align //= 2
    return Archive(data, version, hdr_start, data_start, name_table, entries, data[name_table:hdr_start], gaps,
                   align)


def _no_overlap(regions, floor, what):
    pos = floor
    for off, size in sorted(regions):
        if off < pos:
            raise Refused(f'{what} regions overlap at {off}')
        pos = off + size


def _pad(pos, align, nul):
    p = pos + (1 if nul else 0)
    return b'\0' * (p + (-p % align) - pos)


def repack(archive, replace, pad4=False):
    """Rebuild the archive with entries replaced ({name: new payload = header + data}); other entries' stored bytes
    are kept verbatim. A changed entry is recompressed with zlib level 9 when it was stored packed, else stored raw;
    its header size is fixed (the new payload's first hdr_size bytes replace the header in place).
    pad4: every entry's trailing gap (kept ones too) becomes zeros to a 4-byte boundary, no forced NUL = the legacy
    VoodooPowerFix/pickup_shading.py:rebuild_compressed layout (phase 1.6 T3.c1); hdr_size == 0 archives only."""
    a = archive
    if pad4 and any(x.hdr_size for x in a.entries):
        raise Refused('pad4 applies to archives without entry headers only')
    for name, new in replace.items():
        e = a.entry(name)
        if len(new) < e.hdr_size:
            raise Refused(f'{name}: new payload shorter than its {e.hdr_size}-byte header')
    out = bytearray(TABLE + ENTRY.size * len(a.entries))
    out += a.pool
    headers = bytearray(a.data[a.hdr_start:a.data_start])
    for name in sorted(replace):
        e = a.entry(name)
        headers[e.hdr_off - a.hdr_start:e.hdr_off - a.hdr_start + e.hdr_size] = replace[name][:e.hdr_size]
    out += headers
    order = sorted(range(len(a.entries)), key=lambda i: a.entries[i].data_off)
    if order:
        out += a.data[a.data_start:a.entries[order[0]].data_off]
    else:
        out += a.data[a.data_start:]
    table, has_hdrs = {}, any(x.hdr_size for x in a.entries)
    for i in order:
        e = a.entries[i]
        off = len(out)
        if e.name in replace:
            new = replace[e.name]
            body = bytes(new[e.hdr_size:])
            blob = zlib.compress(body, 9) if e.packed else body
            raw, packed = len(new), len(blob) if e.packed else 0
            out += blob
            out += _pad(len(out), a.data_align if has_hdrs else 4, not (has_hdrs or pad4))
        else:
            raw, packed = e.raw, e.packed
            out += a.data[e.data_off:e.data_off + stored_size(e)]
            out += _pad(len(out), 4, False) if pad4 else a.gaps[i]
        table[i] = (e.name_off, e.hdr_size, e.hdr_off, raw, packed, off)
    for i, fields in table.items():
        ENTRY.pack_into(out, TABLE + ENTRY.size * i, *fields)
    HEADER.pack_into(out, 0, b'HOT ', a.version, a.hdr_start, a.data_start, len(out), a.name_table, len(a.entries))
    return bytes(out)


def bank(archive):
    """Sound-bank view: {name: (pcm_off, pcm_size)} for every 16-bit PCM entry (names from the HOT name pool).
    pcm_size = RIFF `data` chunk size at hdr+68, rounded down to whole samples (as CatFix/catfix.py:parse_bank)."""
    a, n, out = archive, len(archive.data), {}
    for e in a.entries:
        h = a.data[e.hdr_off:e.hdr_off + e.hdr_size]
        if e.hdr_size < 72 or e.packed or h[:4] != b'RIFF' or h[8:12] != b'WAVE' or h[64:68] != b'data':
            raise Refused(f'{e.name}: no RIFF/WAVE header')
        fmt, = struct.unpack_from('<H', h, 20)
        bits, = struct.unpack_from('<H', h, 34)
        if fmt != 1 or bits != 16:
            raise Refused(f'{e.name}: not 16-bit PCM')
        size, = struct.unpack_from('<I', h, 68)
        size -= size % 2
        if size > e.raw - e.hdr_size or e.data_off + size > n:
            raise Refused(f'{e.name}: PCM past its entry')
        out[e.name] = (e.data_off, size)
    return out
