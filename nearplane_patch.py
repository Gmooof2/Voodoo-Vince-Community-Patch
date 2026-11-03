"""Voodoo Vince Remastered: raise the camera near plane to stop depth z-fighting.

The game's camera uses near with a 24-bit depth buffer, so surfaces that
sit very close together (Vince's pupil on his eyeball) flicker when a cutscene
camera is far away, e.g. vince_ratraceprelude01 in area_ratracerodeo. Raising
the near plane to 0.1 gives 10x th= 0.01 e depth precision and removes the flicker.

The patch changes one float: the immediate in the camera constructor's
    mov dword ptr [esi + 0xB4], <near>     ; C7 86 B4 00 00 00 <float>
which sits right before
    mov dword ptr [esi + 0xB8], 1000.0     ; C7 86 B8 00 00 00 00 00 7A 44
The location is found by that byte pattern inside .text, not by a fixed offset,
so it works on an unmodified Vince.exe and on one carrying other patches.

usage:
    python nearplane_patch.py                  patch to 0.1 (default)
    python nearplane_patch.py --near 0.05      patch to another value
    python nearplane_patch.py --restore        put the original 0.01 back
    python nearplane_patch.py --check          only report the current value
    python nearplane_patch.py PATH\\Vince.exe   use a specific exe

Before the first change it saves a copy as Vince.exe.before-nearplane (it never
overwrites an existing backup). Close the game before running it.
"""
import argparse
import os
import re
import shutil
import struct
import sys

ORIGINAL_NEAR = 0.01
DEFAULT_NEAR = 0.1
BACKUP_SUFFIX = '.before-nearplane'

# mov [reg+0xB4], imm32 (near) ; mov [same reg+0xB8], 1000.0f (far)
PATTERN = re.compile(
    rb'\xC7([\x80-\x87])\xB4\x00\x00\x00(.{4})'
    rb'\xC7\1\xB8\x00\x00\x00\x00\x00\x7A\x44',
    re.DOTALL)


def text_section(data):
    if data[:2] != b'MZ':
        raise SystemExit('not an exe (no MZ header)')
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise SystemExit('not a PE file')
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    optsz = struct.unpack_from('<H', data, pe + 20)[0]
    image_base = struct.unpack_from('<I', data, pe + 24 + 28)[0]
    so = pe + 24 + optsz
    for i in range(nsec):
        name, vsize, va, rsize, raw = struct.unpack_from('<8sIIII', data, so + i * 40)
        if name.rstrip(b'\0') == b'.text':
            return raw, rsize, image_base + va
    raise SystemExit('no .text section found')


def find_near(data):
    raw, rsize, text_va = text_section(data)
    hits = [m for m in PATTERN.finditer(data, raw, raw + rsize)]
    if len(hits) != 1:
        raise SystemExit(f'expected exactly 1 camera near-plane instruction, found {len(hits)}; '
                         'this Vince.exe build is not supported')
    off = hits[0].start(2)
    return off, text_va + off - raw


def default_exe():
    here = os.path.dirname(os.path.abspath(__file__))
    for d in (os.getcwd(), here, os.path.dirname(here)):
        p = os.path.join(d, 'Vince.exe')
        if os.path.isfile(p):
            return p
    raise SystemExit('Vince.exe not found; pass its path as an argument')


def main():
    ap = argparse.ArgumentParser(description='Set the Voodoo Vince camera near plane.')
    ap.add_argument('exe', nargs='?', help='path to Vince.exe (default: next to this script or its parent folder)')
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--near', type=float, default=DEFAULT_NEAR, help=f'new near plane (default {DEFAULT_NEAR})')
    g.add_argument('--restore', action='store_true', help=f'set the original value {ORIGINAL_NEAR} again')
    g.add_argument('--check', action='store_true', help='only print the current value')
    args = ap.parse_args()

    exe = args.exe or default_exe()
    data = bytearray(open(exe, 'rb').read())
    off, va = find_near(data)
    cur = struct.unpack_from('<f', data, off)[0]
    print(f'{exe}\n  near plane at file offset {off:#x} (VA {va:#x}): {cur:g}')
    if args.check:
        return

    new = ORIGINAL_NEAR if args.restore else args.near
    if not 0.0 < new < 10.0:
        raise SystemExit(f'refusing near plane {new:g}: expected a small positive value')
    if struct.pack('<f', new) == data[off:off + 4]:
        print('  already set, nothing to do')
        return

    backup = exe + BACKUP_SUFFIX
    if not os.path.exists(backup):
        shutil.copy2(exe, backup)
        print(f'  backup saved: {backup}')
    struct.pack_into('<f', data, off, new)
    try:
        open(exe, 'wb').write(data)
    except PermissionError:
        raise SystemExit('  cannot write Vince.exe: close the game (or check file permissions) and run again')
    print(f'  near plane {cur:g} -> {new:g}')


if __name__ == '__main__':
    sys.exit(main())
