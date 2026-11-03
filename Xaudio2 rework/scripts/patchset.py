"""Patch spec emitter: the five Vince.exe patches (A, C, loader, B, D) as data, format per docs/ops/patchset.md.

Usage: python -I scripts/patchset.py spec
       python -I scripts/patchset.py apply (--input <exe> | --game-dir <dir>) --output <exe>
spec writes build/patchset/patchset.json (and nothing else), prints its path and sha256. Every byte comes from calling
the generators' pieces; placement-dependent fields are found by building each piece at two landings 0x01010101
apart, and the result is checked by rebuilding a third landing from template + fixups.
apply writes the spec, reads it back and applies its absent patches from the JSON alone (apply_spec); writes the
output exe and build/patchset/footprint.json (docs/ops/patchset.md). --game-dir input = pre_package_exe(dir).
"""
import argparse
import hashlib
import json
import runpy
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REC = runpy.run_path(str(ROOT / 'patches' / 'prepare_recovery.py'))
BASE, TAIL, END, PATCH_STATUS = REC['BASE'], REC['TAIL'], REC['END'], REC['PATCH_STATUS']
OUT = ROOT / 'build' / 'patchset' / 'patchset.json'
R1 = 0x900000            # derivation landings, never in the output
DELTA = 0x01010101
R2 = R1 + DELTA
R3 = 0x87B000            # rebuild check landing
MASK = 0xFFFFFFFF
UNIT = 20                # PE import descriptor size
ALIGNMENT = {'rva': 'SectionAlignment', 'raw': 'FileAlignment'}
PLACEMENT = {
    'rva': "align(max(VirtualAddress + max(VirtualSize, SizeOfRawData)) over the target's sections, SectionAlignment)",
    'raw': 'align(file length, FileAlignment)',
    'header': 'the 40 bytes after the last section header, all zero, ending at or before the lowest nonzero-size '
              'PointerToRawData',
}


def fields(b1, b2):
    """(at, v1, kind-delta) of each 4-byte field that differs between two builds; raises on any other difference."""
    if len(b1) != len(b2):
        raise ValueError('builds differ in length')
    out, i = [], 0
    while i < len(b1):
        if b1[i] == b2[i]:
            i += 1
            continue
        j = i
        while j < len(b1) and b1[j] != b2[j]:
            j += 1
        if (j - i) % 4:
            raise ValueError(f'differing run at {i} of {j - i} bytes is not 4-byte fields')
        for at in range(i, j, 4):
            v1, = struct.unpack_from('<I', b1, at)
            v2, = struct.unpack_from('<I', b2, at)
            d = (v2 - v1) & MASK
            if d not in (DELTA, -DELTA & MASK):
                raise ValueError(f'field at {at} moves by 0x{d:x}, not +-delta')
            out.append((at, v1, +1 if d == DELTA else -1))
        i = j
    return out


def label(labels, offset):
    names = [k for k, v in labels.items() if v == offset]
    if len(names) != 1:
        raise ValueError(f'no unique label at offset {offset}: {names}')
    return names[0]


def template(b, fixups):
    t = bytearray(b)
    for f in fixups:
        t[f['at']:f['at'] + 4] = bytes(4)
    return bytes(t)


def rebuild(tmpl, fixups, base, p):
    """Bytes of tmpl at base (field RVA = base + at) with fixups resolved against section P = p."""
    b = bytearray(tmpl)
    for f in fixups:
        t = f['target']
        target = t['rva'] if 'rva' in t else p + t['offset']
        field = base + f['at']
        if f['kind'] == 'rel32':
            struct.pack_into('<i', b, f['at'], target - (field + 4))
        else:
            struct.pack_into('<I', b, f['at'], target)
    return bytes(b)


def payload(gen, section, labels_of):
    """Section payload template + fixups of gen(P) -> bytes, P = payload RVA."""
    b1 = gen(R1)
    fixups = []
    for at, v1, sign in fields(b1, gen(R2)):
        if sign > 0:   # absolute RVA into this section
            off = v1 - R1
            fixups.append({'at': at, 'kind': 'rva32',
                           'target': {'section': section, 'offset': off, 'label': label(labels_of(R1), off)}})
        else:          # relative to a fixed game RVA
            rel, = struct.unpack('<i', struct.pack('<I', v1))
            fixups.append({'at': at, 'kind': 'rel32', 'target': {'rva': R1 + at + 4 + rel}})
    tmpl = template(b1, fixups)
    assert rebuild(tmpl, fixups, R3, R3) == gen(R3), f'{section} payload rebuild'
    return tmpl, fixups


def sites(edits_at, section, labels_of):
    """Site records of edits_at(P) -> [(rva, before, after)]; +delta fields are rel32 into section."""
    e1, e2, e3 = edits_at(R1), edits_at(R2), edits_at(R3)
    out = []
    for (rva, before, after), (_, _, after2), (_, _, after3) in zip(e1, e2, e3):
        if len(before) != len(after):
            raise ValueError(f'site 0x{rva:x} changes length')
        fixups = []
        for at, v1, sign in fields(after, after2):
            if sign < 0:
                raise ValueError(f'site 0x{rva:x} field {at} moves against the landing')
            rel, = struct.unpack('<i', struct.pack('<I', v1))
            off = rva + at + 4 + rel - R1
            fixups.append({'at': at, 'kind': 'rel32',
                           'target': {'section': section, 'offset': off, 'label': label(labels_of(R1), off)}})
        rec = {'rva': rva, 'length': len(after), 'before': before.hex()}
        if fixups:
            tmpl = template(after, fixups)
            assert rebuild(tmpl, fixups, rva, R3) == after3, f'site 0x{rva:x} rebuild'
            rec.update(after_template=tmpl.hex(), fixups=fixups)
        else:
            assert after == after3, f'site 0x{rva:x} depends on placement'
            rec['after'] = after.hex()
        out.append(rec)
    return out


def headers(edits, section, copy_value):
    out = []
    for field, op, value in edits:
        if value == 'directory':
            value = {'section': section, 'of': 'copy_len', 'add': copy_value}
        elif isinstance(value, str):
            value = {'section': section, 'of': value, 'add': 0}
        out.append({'field': field, 'op': op, 'value': value})
    return out


def sig(rva, b):
    return {'rva': rva, 'hex': b.hex()}


def record(pid, generator, signatures=(), site_list=(), sections=(), relocs=(), header_list=()):
    return {'id': pid, 'generator': generator, 'signatures': list(signatures), 'sites': list(site_list),
            'sections': list(sections), 'relocs': list(relocs), 'headers': list(header_list)}


def patch_a():
    name = BASE['VACFIX_NAME'].decode('ascii')
    entries = lambda r: {k: v - r for k, v in BASE['vacfix_code'](r)[1].items()}
    tmpl, fixups = payload(lambda r: BASE['vacfix_code'](r)[0], name, entries)
    site_list = sites(lambda r: BASE['site_edits'](BASE['vacfix_code'](r)[1]), name, entries)
    section = {'name': name, 'characteristics': BASE['VACFIX_CHARACTERISTICS'], 'alignment': ALIGNMENT,
               'copy': None, 'payload': tmpl.hex(), 'fixups': fixups}
    signatures = [sig(BASE['STREAM_RVA'], BASE['ORIGINAL'])] + [sig(r, b) for r, b in BASE['SIGNATURES']]
    return record('A', 'patches/prepare_patch.py', signatures, site_list, [section],
                  header_list=headers(BASE['HEADER_EDITS'], name, None))


def patch_c():
    fix = PATCH_STATUS['stereo_fix']()
    rva, orig, new = fix['CodeRva'], fix['CodeOrig'], fix['CodeNew']
    for r in fix['OldRelocs']:
        assert rva <= r and r + 4 <= rva + len(new), f'old reloc 0x{r:x} outside patch C code'
    relocs = [{'page': rva & ~0xFFF, 'type': 3, 'out': sorted(fix['OldRelocs']), 'in': [fix['NewReloc']]}]
    signatures = [sig(*fix['CtxBefore']), sig(rva, orig), sig(*fix['CtxAfter'])]
    site_list = [{'rva': rva, 'length': len(new), 'before': orig.hex(), 'after': new.hex()}]
    return record('C', 'scripts/patch_status.py', signatures, site_list, relocs=relocs)


def patch_loader():
    name = REC['VAUDIO_NAME'].decode('ascii')
    blob = REC['import_blob']
    assert blob(b'\x01' * UNIT, R3)[0][UNIT:] == blob(b'', R3 + UNIT)[0], 'import blob is not copy ++ payload'
    descriptors = b''    # payload model: no copied descriptors, P = section RVA + copy_len
    layout = blob(descriptors, R1)[1]
    labels = lambda r: {k: v for k, v in blob(b'', r)[1].items() if k != 'directory'}
    tmpl, fixups = payload(lambda r: blob(b'', r)[0], name, labels)
    copy = {'what': 'import descriptors', 'from': 'DataDirectory[1].VirtualAddress', 'unit': UNIT,
            'until': 'all-zero unit'}
    section = {'name': name, 'characteristics': REC['VAUDIO_CHARACTERISTICS'], 'alignment': ALIGNMENT,
               'copy': copy, 'payload': tmpl.hex(), 'fixups': fixups}
    return record('loader', 'patches/prepare_recovery.py', sections=[section],
                  header_list=headers(REC['IMPORT_HEADER_EDITS'], name, layout['directory'] - len(descriptors)))


def patch_b():
    base = TAIL['IMAGE_BASE']
    site, cave = TAIL['SITE_VA'] - base, TAIL['CAVE_VA'] - base
    site_new, cave_new = TAIL['build_site'](), TAIL['build_cave']()
    assert len(site_new) == len(TAIL['SITE_ORIG']) and len(cave_new) == len(TAIL['CAVE_ORIG'])
    signatures = [sig(site, TAIL['SITE_ORIG']), sig(cave, TAIL['CAVE_ORIG'])]
    site_list = [{'rva': site, 'length': len(site_new), 'before': TAIL['SITE_ORIG'].hex(), 'after': site_new.hex()},
                 {'rva': cave, 'length': len(cave_new), 'before': TAIL['CAVE_ORIG'].hex(), 'after': cave_new.hex()}]
    return record('B', 'patches/vince_audio_tail_fix.py', signatures, site_list)


def patch_d():
    base = END['IMAGE_BASE']
    signatures, site_list = [], []
    for _, va, orig, new in END['sites']():
        signatures.append(sig(va - base, orig))
        site_list.append({'rva': va - base, 'length': len(new), 'before': orig.hex(), 'after': new.hex()})
    return record('D', 'patches/vince_audio_end_fix.py', signatures, site_list)


def build_spec():
    """The spec's JSON text (bytes); writes nothing."""
    patches = {'A': patch_a, 'C': patch_c, 'loader': patch_loader, 'B': patch_b, 'D': patch_d}
    composition = ['A', 'C', 'loader', 'B', 'D']
    obj = {'format': 1, 'composition': composition, 'image_base': PATCH_STATUS['IMAGE_BASE'],
           'placement': PLACEMENT, 'patches': [patches[p]() for p in composition]}
    return (json.dumps(obj, indent=1, sort_keys=True) + '\n').encode('utf-8')


def write_spec():
    text = build_spec()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(text)
    return text


def spec():
    text = write_spec()
    print(OUT)
    print(f'sha256 {hashlib.sha256(text).hexdigest()}  {len(text)} bytes')


def load_spec():
    """Write the spec, read it back: (spec dict, sha256 of the file)."""
    write_spec()
    text = OUT.read_bytes()
    return json.loads(text), hashlib.sha256(text).hexdigest()


class Image:
    """PE view of a bytearray for apply_spec (offsets per docs/ops/patchset.md)."""

    def __init__(self, data):
        self.data = data
        self.pe = struct.unpack_from('<I', data, 0x3c)[0]
        self.opt = self.pe + 24
        self.table = self.opt + struct.unpack_from('<H', data, self.pe + 20)[0]

    def count(self):
        return struct.unpack_from('<H', self.data, self.pe + 6)[0]

    def sections(self):
        """(name, VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData) of each section header."""
        out = []
        for i in range(self.count()):
            at = self.table + i * 40
            out.append((self.data[at:at + 8].rstrip(b'\0').decode('ascii', 'replace'),
                        *struct.unpack_from('<IIII', self.data, at + 8)))
        return out

    def offset(self, rva, n=1):
        for _, _, start, rsize, raw in self.sections():
            if start <= rva and rva + n <= start + rsize:
                return raw + rva - start
        raise ValueError(f'RVA 0x{rva:x}+{n} is not backed by file data')

    def u32(self, at):
        return struct.unpack_from('<I', self.data, at)[0]


def resolve(tmpl, fixups, base, places):
    """rebuild() with every section target looked up in places (name -> P)."""
    names = {f['target']['section'] for f in fixups if 'section' in f['target']}
    if len(names) > 1:
        raise ValueError(f'fixups target several sections {sorted(names)}')
    if names - places.keys():
        raise ValueError(f'fixup target section {names} not placed')
    return rebuild(tmpl, fixups, base, places[names.pop()] if names else 0)


def apply_patch(img, rec, places, spans, fields):
    """Apply one absent patch record to img; spans gets (patch, rva, offset, length, kind), fields offset -> patches."""
    data, pid = img.data, rec['id']
    align = BASE['align']
    section_align, file_align = struct.unpack_from('<II', data, img.opt + 32)
    for s in rec['signatures']:
        want = bytes.fromhex(s['hex'])
        at = img.offset(s['rva'], len(want))
        if data[at:at + len(want)] != want:
            raise ValueError(f"signature at RVA 0x{s['rva']:x} differs")
    sect = {}
    for s in rec['sections']:
        if s['alignment'] != ALIGNMENT:
            raise ValueError(f"section {s['name']} alignment {s['alignment']}")
        if any(struct.unpack_from('<II', data, img.opt + 96 + 4 * 8)):   # DataDirectory[4]
            raise ValueError('input has a certificate table')
        existing = img.sections()
        if any(e[0] == s['name'] for e in existing):
            raise ValueError(f"section {s['name']} already present")
        header = img.table + img.count() * 40
        if header + 40 > min(raw for _, _, _, n, raw in existing if n) or any(data[header:header + 40]):
            raise ValueError('no free section header slot')
        rva = align(max(va + max(vs, n) for _, vs, va, n, _ in existing), section_align)
        copied = b''
        if s['copy'] is not None:
            c = s['copy']
            if (c['what'], c['from'], c['until']) != ('import descriptors', 'DataDirectory[1].VirtualAddress',
                                                       'all-zero unit'):
                raise ValueError(f'unknown copy {c}')
            pos = img.offset(img.u32(img.opt + 104), c['unit'])
            while any(data[pos:pos + c['unit']]):
                if len(copied) > 4096:
                    raise ValueError('invalid import table')
                copied += data[pos:pos + c['unit']]
                pos += c['unit']
        p = rva + len(copied)
        places[s['name']] = p
        body = copied + resolve(bytes.fromhex(s['payload']), s['fixups'], p, places)
        raw, n = align(len(data), file_align), align(len(body), file_align)
        if raw > len(data):
            spans.append((pid, None, len(data), raw - len(data), 'section'))
        data.extend(bytes(raw + n - len(data)))
        data[raw:raw + len(body)] = body
        struct.pack_into('<8sIIIIIIHHI', data, header, s['name'].encode('ascii'), len(body), rva, n, raw,
                         0, 0, 0, 0, s['characteristics'])
        spans.append((pid, rva, raw, n, 'section'))
        spans.append((pid, header, header, 40, 'section-header'))
        sect[s['name']] = {'rva': rva, 'end': rva + len(body), 'SizeOfRawData': n, 'copy_len': len(copied)}
    for s in rec['sites']:
        before = bytes.fromhex(s['before'])
        at = img.offset(s['rva'], s['length'])
        if data[at:at + s['length']] != before:
            raise ValueError(f"site at RVA 0x{s['rva']:x} is not original")
        after = (bytes.fromhex(s['after']) if 'after' in s
                 else resolve(bytes.fromhex(s['after_template']), s['fixups'], s['rva'], places))
        if len(after) != s['length'] or len(before) != s['length']:
            raise ValueError(f"site at RVA 0x{s['rva']:x} length")
        data[at:at + len(after)] = after
        spans.append((pid, s['rva'], at, len(after), 'site'))
    for r in rec['relocs']:
        rva, size = struct.unpack_from('<II', data, img.opt + 136)
        if not size:
            raise ValueError('no relocation table')
        first = off = img.offset(rva, 8)
        block = None
        while off + 8 <= first + size:
            va, n = struct.unpack_from('<II', data, off)
            if n < 8:
                break
            if va == r['page']:
                block = (off, n)
                break
            off += n
        if block is None:
            raise ValueError(f"no relocation block for page 0x{r['page']:x}")
        off, n = block
        slots, entries = (n - 8) // 2, set()
        for (v,) in struct.iter_unpack('<H', data[off + 8:off + n]):
            if v >> 12 == r['type']:
                entries.add(r['page'] + (v & 0xFFF))
            elif v:
                raise ValueError(f"relocation entry 0x{v:04x} in page 0x{r['page']:x} is not type {r['type']}")
        keep = sorted((entries - set(r['out'])) | set(r['in']))
        if len(keep) > slots:
            raise ValueError('relocations do not fit their block')
        struct.pack_into(f'<{slots}H', data, off + 8,
                         *[(r['type'] << 12) | (e - r['page']) for e in keep], *[0] * (slots - len(keep)))
        spans.append((pid, rva + off - first + 8, off + 8, 2 * slots, 'reloc'))
    for h in rec['headers']:
        rel, fmt = BASE['HEADER_FIELDS'][h['field']]
        at, v = img.pe + rel, h['value']
        if isinstance(v, dict):
            v = sect[v['section']][v['of']] + v.get('add', 0)
        if h['op'] == 'add':
            v += struct.unpack_from(fmt, data, at)[0]
        elif h['op'] == 'align':
            v = align(v, section_align)
        elif h['op'] != 'set':
            raise ValueError(f"unknown header op {h['op']}")
        struct.pack_into(fmt, data, at, v)
        kind = 'import' if h['field'].startswith('DataDirectory[1].') else 'header'
        entry = fields.setdefault(at, [struct.calcsize(fmt), kind, []])
        if pid not in entry[2]:
            entry[2].append(pid)


def apply_spec(data, spec, spec_sha, work=None):
    """(output bytes, footprint dict) of the spec's absent patches applied to data; ValueError on any refusal.
    work: folder for the temporary status exe (default OUT.parent; the AIO build passes its own scratch dir)."""
    if spec.get('format') != 1:
        raise ValueError(f"spec format {spec.get('format')}")
    tmp = Path(work or OUT.parent) / f'status-{hashlib.sha256(data).hexdigest()[:16]}.exe'
    tmp.parent.mkdir(parents=True, exist_ok=True)
    tmp.write_bytes(data)
    try:
        st = PATCH_STATUS['status'](tmp)
    finally:
        tmp.unlink()
    for pid in spec['composition']:
        if st[pid] == 'partial':
            raise ValueError(f'{pid} is partial (foreign or half-applied bytes)')
    if st['loader'] == 'present':
        raise ValueError('input already has the VinceAudio loader')
    img = Image(bytearray(data))
    if img.u32(img.opt + 28) != spec['image_base']:
        raise ValueError(f"ImageBase is not 0x{spec['image_base']:x}")
    records = {r['id']: r for r in spec['patches']}
    places, spans, fields = {}, [], {}
    for pid in spec['composition']:
        if st[pid] == 'present':
            continue
        try:
            apply_patch(img, records[pid], places, spans, fields)
        except (ValueError, struct.error) as e:
            raise ValueError(f'patch {pid} refused ({e})') from None
    out = bytes(img.data)
    regions = []
    for pid, rva, at, n, kind in spans:
        regions.append({'patch': pid, 'rva': rva, 'file_offset': at, 'length': n, 'kind': kind,
                        'before': data[at:at + n].hex() if at < len(data) else '', 'after': out[at:at + n].hex()})
    order = {p: i for i, p in enumerate(spec['composition'])}
    for at, (n, kind, pids) in fields.items():
        regions.append({'patch': '+'.join(sorted(pids, key=order.get)), 'rva': at, 'file_offset': at, 'length': n,
                        'kind': kind, 'before': data[at:at + n].hex(), 'after': out[at:at + n].hex()})
    regions.sort(key=lambda r: (r['file_offset'], r['patch']))
    cover = bytearray(max(len(data), len(out)))
    for r in regions:
        for i in range(r['file_offset'], r['file_offset'] + r['length']):
            cover[i] += 1
    assert max(cover, default=0) <= 1, 'footprint regions overlap'
    for i in range(len(out)):
        if (i >= len(data) or data[i] != out[i]) and cover[i] != 1:
            raise AssertionError(f'changed byte at file offset 0x{i:x} outside the footprint')
    assert len(out) >= len(data), 'output shorter than input'
    footprint = {'format': 1, 'path': 'Vince.exe', 'before': hashlib.sha256(data).hexdigest(),
                 'after': hashlib.sha256(out).hexdigest(), 'spec': spec_sha, 'regions': regions}
    return out, footprint


def apply(args):
    try:
        src = Path(args.input) if args.input else REC['pre_package_exe'](args.game_dir)
    except ValueError as e:
        sys.exit(f'{e}; refusing, nothing written')
    out = Path(args.output).resolve()
    if out == src.resolve():
        sys.exit('--output must not be the input; nothing written')
    if args.game_dir and Path(args.game_dir).resolve() in out.parents:
        sys.exit('--output must not lie inside the game dir; nothing written')
    data = src.read_bytes()
    obj, spec_sha = load_spec()
    try:
        result, footprint = apply_spec(data, obj, spec_sha)
    except ValueError as e:
        sys.exit(f'{src}: {e}; nothing written')
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(result)
    fp = OUT.parent / 'footprint.json'
    fp.write_bytes((json.dumps(footprint, indent=1, sort_keys=True) + '\n').encode('utf-8'))
    print(f'Input:  {src}  sha256 {footprint["before"]}')
    print(f'Output: {out}  sha256 {footprint["after"]}')
    print(f'Footprint: {fp}  {len(footprint["regions"])} regions; spec sha256 {spec_sha}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='cmd', required=True)
    sub.add_parser('spec', help='write build/patchset/patchset.json')
    ap = sub.add_parser('apply', help='apply the spec to an exe; writes the output and build/patchset/footprint.json')
    source = ap.add_mutually_exclusive_group(required=True)
    source.add_argument('--input')
    source.add_argument('--game-dir')
    ap.add_argument('--output', required=True)
    args = parser.parse_args()
    if args.cmd == 'spec':
        spec()
    else:
        apply(args)


if __name__ == '__main__':
    main()
