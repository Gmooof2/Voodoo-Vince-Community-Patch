"""Install/undo the light glow fix (restores light coronas such as the blue glow on top of the mausoleum urns).

One byte in Vince.exe's .text: the view node sorter's type table at VA 0x4a7c50 sends LightNode (type 9) to the
skip case, so LightNode::Render never runs. The patch routes type 9 to the sorted-render case.

The patch is signature-guarded, not tied to a whole-file hash: it applies to vanilla or to any modded exe whose
18-byte table is untouched, and refuses anything else. No section, header, import or relocation changes.

Exit codes: 0 ok, 1 refused (conflict or verification failure), 2 bad input.
"""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import sys

NAME, VERSION = 'VinceLightGlowFix', '1.0.0'
TABLE_VA = 0x4A7C50
ORIGINAL = bytes.fromhex('000102030405060808080508080807080101')   # case index for node types 1..18
PATCHED = bytes.fromhex('000102030405060802080508080807080101')    # type 9 (LightNode) -> case 2
BACKUP = 'Vince.exe.lightglow-backup'


def sha(blob):
    return hashlib.sha256(blob).hexdigest()


def table_offset(exe):
    """File offset of the table, via the section headers. Refuses a non-PE32 or relocated-base image."""
    if exe[:2] != b'MZ':
        raise ValueError('Not an MZ executable')
    pe = struct.unpack_from('<I', exe, 0x3C)[0]
    if exe[pe:pe + 4] != b'PE\0\0' or struct.unpack_from('<H', exe, pe + 24)[0] != 0x10B:
        raise ValueError('Not a PE32 executable')
    if struct.unpack_from('<I', exe, pe + 24 + 28)[0] != 0x400000:
        raise ValueError('ImageBase is not 0x400000')
    count = struct.unpack_from('<H', exe, pe + 6)[0]
    headers = pe + 24 + struct.unpack_from('<H', exe, pe + 20)[0]
    rva = TABLE_VA - 0x400000
    for i in range(count):
        vsize, va, rsize, raw = struct.unpack_from('<4I', exe, headers + 40 * i + 8)
        if va <= rva < va + min(vsize, rsize) and exe[headers + 40 * i:headers + 40 * i + 5] == b'.text':
            return raw + rva - va
    raise ValueError('Table is not in .text')


def classify(exe):
    off = table_offset(exe)
    current = exe[off:off + len(ORIGINAL)]
    state = 'installed' if current == PATCHED else 'base' if current == ORIGINAL else 'CONFLICT'
    return state, off, current


def write(path, data):
    # Retain existing ACLs; the output was verified before this call.
    with path.open('r+b') as f:
        f.write(data)
        f.truncate()
        f.flush()
        os.fsync(f.fileno())


def run(mode, game):
    path = game / 'Vince.exe'
    exe = path.read_bytes()
    state, off, current = classify(exe)
    print(f'{state}: {NAME} v{VERSION} (table at file 0x{off:x}: {current.hex()})')
    if state == 'CONFLICT':
        raise ValueError('The node-type table differs from both the original and the patch; another mod edits it')
    if mode == 'status':
        return
    want = PATCHED if mode == 'apply' else ORIGINAL
    if current == want:
        print(f'{mode}: nothing to do')
        return
    out = bytearray(exe)
    out[off:off + len(want)] = want
    out = bytes(out)
    diff = [i for i in range(len(exe)) if exe[i] != out[i]] if len(out) == len(exe) else None
    if diff != [off + 8]:
        raise ValueError('Internal check failed: the change is not exactly one byte')
    if mode == 'apply':
        backup = game / BACKUP
        if not backup.exists():
            with backup.open('xb') as f:
                f.write(exe)
        print(f'backup: {backup.name} ({sha(backup.read_bytes())[:16]}...)')
    if path.read_bytes() != exe:
        raise ValueError('Vince.exe changed during installation')
    write(path, out)
    if path.read_bytes() != out:
        write(path, exe)
        raise ValueError('Written file does not verify; original restored')
    print(f'{mode}: 1 byte changed at file 0x{off + 8:x}; Vince.exe sha256 {sha(out)}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('mode', choices=['status', 'apply', 'revert'])
    p.add_argument('--game', type=Path, required=True)
    args = p.parse_args()
    if not (args.game / 'Vince.exe').is_file():
        print(f'error: no Vince.exe in {args.game}', file=sys.stderr)
        sys.exit(2)
    try:
        run(args.mode, args.game)
    except (ValueError, OSError) as error:
        print(f'refused: {error}', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
