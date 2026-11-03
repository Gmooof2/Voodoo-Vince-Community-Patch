"""Install/undo the lighthouse beam and mirror fixes (Gwistlethorpe Mausoleum urns, house-of-mirrors) on Vince.exe.

Data in patchset.json: one appended code section (.vbeam) and 14 signature-checked sites in .text/.rdata.
The section goes at the next free place (RVA after the last section, raw at the end of the file, header in the next
zero slot); its placement-dependent dwords are fixed up from the section address, so it works on vanilla or on an
exe that other mods have already extended. Anything not matching the original or the patched bytes is refused.

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
BACKUP = 'Vince.exe.lighthouse-backup'


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
    vbeam = pe.section(spec['section']['name'])
    sva = BASE + vbeam['va'] if vbeam else None
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
    if vbeam:
        want = payload_at(spec, sva)
        section_ok = vbeam['vs'] == len(want) and exe[vbeam['rp']:vbeam['rp'] + len(want)] == want
    if set(states) == {'original'}:
        state = 'base'          # a dead .vbeam left by a revert does not count
    elif set(states) == {'patched'} and section_ok:
        state = 'installed'
    else:
        state = 'CONFLICT'
    return state, pe, states


def apply(spec, exe):
    pe = PE(exe)
    b = bytearray(exe)
    if pe.section(spec['section']['name']):
        raise ValueError('A .vbeam section is already present (left by an earlier revert); restore the backup first')
    if struct.unpack_from('<I', b, pe.opt + 96 + 4 * 8)[0]:
        raise ValueError('Exe has a certificate table')
    last = max(pe.secs, key=lambda s: s['va'])
    va = align(last['va'] + max(last['vs'], last['rs']), pe.salign)
    end = max(s['rp'] + s['rs'] for s in pe.secs)
    if len(b) != end:
        raise ValueError('Exe has data after its last section (overlay); refusing to append')
    raw = align(len(b), pe.falign)
    hdr = pe.st + 40 * pe.n
    if hdr + 40 > struct.unpack_from('<I', b, pe.opt + 60)[0] or any(b[hdr:hdr + 40]):
        raise ValueError('No free section-header slot')
    payload = payload_at(spec, BASE + va)
    rsize = align(len(payload), pe.falign)
    b += b'\0' * (raw - len(b)) + payload + b'\0' * (rsize - len(payload))
    name = spec['section']['name'].encode().ljust(8, b'\0')
    b[hdr:hdr + 40] = name + struct.pack('<IIIIIIHHI', len(payload), va, rsize, raw, 0, 0, 0, 0,
                                         spec['section']['characteristics'])
    struct.pack_into('<H', b, pe.o + 6, pe.n + 1)
    struct.pack_into('<I', b, pe.opt + 56, align(va + len(payload), pe.salign))
    for s in spec['sites']:
        f = pe.v2f(s['va'])
        b[f:f + len(s['before']) // 2] = site_after(s, BASE + va)
    return bytes(b)


def revert(spec, exe):
    pe = PE(exe)
    b = bytearray(exe)
    vbeam = pe.section(spec['section']['name'])
    for s in spec['sites']:
        f = pe.v2f(s['va'])
        b[f:f + len(s['before']) // 2] = bytes.fromhex(s['before'])
    last = max(pe.secs, key=lambda s: s['va'])
    if vbeam is last and vbeam['hdr'] == pe.st + 40 * (pe.n - 1) and vbeam['rp'] + vbeam['rs'] == len(b):
        b[vbeam['hdr']:vbeam['hdr'] + 40] = b'\0' * 40
        struct.pack_into('<H', b, pe.o + 6, pe.n - 1)
        prev = max((s for s in pe.secs if s is not vbeam), key=lambda s: s['va'])
        struct.pack_into('<I', b, pe.opt + 56, align(prev['va'] + max(prev['vs'], prev['rs']), pe.salign))
        del b[vbeam['rp']:]
        note = 'section removed'
    else:
        note = 'section left in place (another mod appended a section after it); it is unreferenced'
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
        raise ValueError('Vince.exe is partly patched or another mod changed these bytes; merge by hand')
    if mode == 'status':
        return
    if (mode == 'apply') == (state == 'installed'):
        print(f'{mode}: nothing to do')
        return
    if mode == 'apply':
        out, note = apply(spec, exe), 'section .vbeam appended'
        if classify(spec, out)[0] != 'installed':
            raise ValueError('Internal check failed after apply')
        backup = game / BACKUP
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
