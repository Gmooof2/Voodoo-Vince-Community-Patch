"""Install/undo the fog-zone sort fix on Vince.exe (needs Vincesmokefix's particle order, which it refines).

Translucent objects in front of a fog zone are drawn after it again, so the zone no longer fogs them (Crypt City
gate bars), while objects inside or behind it stay fogged (Kosmo's cabin smoke). Data in patchset.json: two
appended sections (.vfogz, the code, read-execute; .vfogd, the one-byte flag, read-write, zero-filled, at the code
section's VA + data.rva_offset, which is the next section-aligned VA) and 4 signature-checked call sites.
The sections go at the next free place (RVA after the last section, raw at the end of the file, headers in the next
two zero slots); the code's placement-dependent dwords are fixed up from its address, so it works on vanilla or on an
exe that other mods have already extended. Anything not matching the original or the patched bytes is refused.
Revert removes both sections when they are still the last two at the end of the file, else leaves both unreferenced.

Exit codes: 0 ok, 1 refused (conflict or verification failure), 2 bad input.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parent
BASE = 0x400000


def sha(blob):
    return hashlib.sha256(blob).hexdigest()


def align(v, a):
    return (v + a - 1) & ~(a - 1)


class PE:
    def __init__(self, b):
        if b[:2] != b'MZ':
            raise ValueError('Not an MZ executable')
        self.b = b
        self.o = struct.unpack_from('<I', b, 0x3C)[0]
        if b[self.o:self.o + 4] != b'PE\0\0' or struct.unpack_from('<H', b, self.o + 24)[0] != 0x10B:
            raise ValueError('Not a PE32 executable')
        self.opt = self.o + 24
        if struct.unpack_from('<I', b, self.opt + 28)[0] != BASE:
            raise ValueError('ImageBase is not 0x400000')
        self.n = struct.unpack_from('<H', b, self.o + 6)[0]
        self.st = self.opt + struct.unpack_from('<H', b, self.o + 20)[0]
        self.salign, self.falign = struct.unpack_from('<2I', b, self.opt + 32)
        self.secs = []
        for i in range(self.n):
            q = self.st + 40 * i
            vs, va, rs, rp = struct.unpack_from('<4I', b, q + 8)
            self.secs.append({'name': bytes(b[q:q + 8]).rstrip(b'\0').decode('latin1'), 'hdr': q,
                              'vs': vs, 'va': va, 'rs': rs, 'rp': rp})

    def v2f(self, va):
        r = va - BASE
        for s in self.secs:
            if s['va'] <= r < s['va'] + min(max(s['vs'], 1), s['rs']):
                return s['rp'] + r - s['va']
        raise ValueError(f'VA {va:#x} is not in a section')

    def section(self, name):
        return next((s for s in self.secs if s['name'] == name), None)

    def chars(self, s):
        return struct.unpack_from('<I', self.b, s['hdr'] + 36)[0]


def payload_at(spec, section_va):
    sec = spec['section']
    p = bytearray(bytes.fromhex(sec['payload_hex']))
    if sha(p) != sec['payload_sha256']:
        raise ValueError('Corrupt payload in patchset.json')
    delta = section_va - sec['link_va']
    for f in sec['fixups']:
        v = struct.unpack_from('<I', p, f['offset'])[0]
        struct.pack_into('<I', p, f['offset'], (v + (delta if f['kind'] == 'base' else -delta)) & 0xffffffff)
    return bytes(p)


def site_after(site, section_va):
    out, h = b'', site['after']
    while h:
        if h.startswith('{call:+'):
            end = h.index('}')
            off = int(h[7:end], 16)
            out += b'\xe8' + struct.pack('<i', section_va + off - (site['va'] + len(out) + 5))
            h = h[end + 1:]
        else:
            nxt = h.find('{')
            chunk = h if nxt < 0 else h[:nxt]
            out += bytes.fromhex(chunk)
            h = h[len(chunk):]
    if len(out) * 2 != len(site['before']):
        raise ValueError('Bad site template')
    return out


def classify(spec, exe):
    pe = PE(exe)
    sec = pe.section(spec['section']['name'])
    sva = BASE + sec['va'] if sec else None
    states = []
    for s in spec['sites']:
        f = pe.v2f(s['va'])
        cur = exe[f:f + len(s['before']) // 2]
        if cur.hex() == s['before']:
            states.append('original')
        elif sva is not None and cur == site_after(s, sva):
            states.append('patched')
        else:
            states.append('foreign')
    section_ok = None
    if sec:
        want = payload_at(spec, sva)
        dat, d = pe.section(spec['data']['name']), spec['data']
        section_ok = (sec['vs'] == len(want) and exe[sec['rp']:sec['rp'] + len(want)] == want
                      and pe.chars(sec) == spec['section']['characteristics']
                      and dat is not None and dat['va'] == sec['va'] + d['rva_offset'] and dat['vs'] == d['size']
                      and pe.chars(dat) == d['characteristics'] and dat['rs'] >= d['size']
                      and not any(exe[dat['rp']:dat['rp'] + dat['rs']]))
    if set(states) == {'original'}:
        state = 'base'          # a dead section left by a revert does not count
    elif set(states) == {'patched'} and section_ok:
        state = 'installed'
    else:
        state = 'CONFLICT'
    return state, pe, states


def apply(spec, exe):
    pe = PE(exe)
    b = bytearray(exe)
    for name in (spec['section']['name'], spec['data']['name']):
        if pe.section(name):
            raise ValueError(f"A {name} section is already present (left by an earlier revert); "
                             'restore the backup first')
    if struct.unpack_from('<I', b, pe.opt + 96 + 4 * 8)[0]:
        raise ValueError('Exe has a certificate table')
    last = max(pe.secs, key=lambda s: s['va'])
    va = align(last['va'] + max(last['vs'], last['rs']), pe.salign)
    end = max(s['rp'] + s['rs'] for s in pe.secs)
    if len(b) != end:
        raise ValueError('Exe has data after its last section (overlay); refusing to append')
    raw = align(len(b), pe.falign)
    hdr = pe.st + 40 * pe.n
    if hdr + 80 > struct.unpack_from('<I', b, pe.opt + 60)[0] or any(b[hdr:hdr + 80]):
        raise ValueError('No two free section-header slots')
    payload = payload_at(spec, BASE + va)
    d = spec['data']
    dva = va + d['rva_offset']                  # the payload reaches its flag at code VA + rva_offset
    if dva != align(va + len(payload), pe.salign):
        raise ValueError('patchset data.rva_offset does not follow the code section')
    rsize, drsize = align(len(payload), pe.falign), align(d['size'], pe.falign)
    b += b'\0' * (raw - len(b)) + payload + b'\0' * (rsize - len(payload)) + b'\0' * drsize
    name = spec['section']['name'].encode().ljust(8, b'\0')
    b[hdr:hdr + 40] = name + struct.pack('<IIIIIIHHI', len(payload), va, rsize, raw, 0, 0, 0, 0,
                                         spec['section']['characteristics'])
    b[hdr + 40:hdr + 80] = d['name'].encode().ljust(8, b'\0') + struct.pack(
        '<IIIIIIHHI', d['size'], dva, drsize, raw + rsize, 0, 0, 0, 0, d['characteristics'])
    struct.pack_into('<H', b, pe.o + 6, pe.n + 2)
    struct.pack_into('<I', b, pe.opt + 56, align(dva + d['size'], pe.salign))
    for s in spec['sites']:
        f = pe.v2f(s['va'])
        b[f:f + len(s['before']) // 2] = site_after(s, BASE + va)
    return bytes(b)


def revert(spec, exe):
    pe = PE(exe)
    b = bytearray(exe)
    sec, dat = pe.section(spec['section']['name']), pe.section(spec['data']['name'])
    for s in spec['sites']:
        f = pe.v2f(s['va'])
        b[f:f + len(s['before']) // 2] = bytes.fromhex(s['before'])
    by_va = sorted(pe.secs, key=lambda s: s['va'])
    if (dat is not None and by_va[-2:] == [sec, dat] and sec['hdr'] == pe.st + 40 * (pe.n - 2)
            and dat['hdr'] == pe.st + 40 * (pe.n - 1) and sec['rp'] + sec['rs'] == dat['rp']
            and dat['rp'] + dat['rs'] == len(b)):
        b[sec['hdr']:sec['hdr'] + 80] = b'\0' * 80
        struct.pack_into('<H', b, pe.o + 6, pe.n - 2)
        prev = by_va[-3]
        struct.pack_into('<I', b, pe.opt + 56, align(prev['va'] + max(prev['vs'], prev['rs']), pe.salign))
        del b[sec['rp']:]
        note = 'sections removed'
    else:
        note = 'sections left in place (another mod appended a section after them); they are unreferenced'
    return bytes(b), note


def write(path, data):
    with path.open('r+b') as f:
        f.write(data)
        f.truncate()
        f.flush()
        os.fsync(f.fileno())


def run(mode, game, spec):
    path = game / 'Vince.exe'
    exe = path.read_bytes()
    state, pe, states = classify(spec, exe)
    print(f'{state}: {spec["name"]} v{spec["version"]}')
    for site, st in zip(spec['sites'], states):
        if st != {'base': 'original', 'installed': 'patched'}.get(state):
            print(f'  {st:8} {site["patch"]} @ {site["va"]:#x}')
    if state == 'CONFLICT':
        raise ValueError('Vince.exe is partly patched or another mod changed these bytes; merge by hand'
                         + (f" (requires: {spec['requires']})" if spec.get('requires') else ''))
    if mode == 'status':
        return
    if (mode == 'apply') == (state == 'installed'):
        print(f'{mode}: nothing to do')
        return
    if mode == 'apply':
        out, note = apply(spec, exe), f"sections {spec['section']['name']}, {spec['data']['name']} appended"
        if classify(spec, out)[0] != 'installed':
            raise ValueError('Internal check failed after apply')
        backup = game / spec['backup']
        if not backup.exists():
            with backup.open('xb') as f:
                f.write(exe)
        print(f'backup: {backup.name}')
    else:
        out, note = revert(spec, exe)
        if classify(spec, out)[0] != 'base':
            raise ValueError('Internal check failed after revert')
    if path.read_bytes() != exe:
        raise ValueError('Vince.exe changed during installation')
    write(path, out)
    if path.read_bytes() != out:
        write(path, exe)
        raise ValueError('Written file does not verify; original restored')
    print(f'{mode}: {note}; Vince.exe sha256 {sha(out)}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('mode', choices=['status', 'apply', 'revert'])
    p.add_argument('--game', type=Path, required=True)
    args = p.parse_args()
    if not (args.game / 'Vince.exe').is_file():
        print(f'error: no Vince.exe in {args.game}', file=sys.stderr)
        sys.exit(2)
    try:
        run(args.mode, args.game, json.loads((ROOT / 'patchset.json').read_text()))
    except (ValueError, OSError) as error:
        print(f'refused: {error}', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
