"""Voodoo Vince Remastered: fix ghosts (the butler etc.) being squeezed and cut off at FOVs other than ~54 deg vertical.

The game pastes its "spooky" ghost layer onto the scene with a quad it sizes for a fixed
~53.9 deg vertical FOV. This replaces the SpookyVS vertex shader embedded in Vince.exe with
one that pins that quad to the screen corners (keeping the game's depth), so it works at any FOV.

Usage (game closed):
    python patch_spooky_fov.py "<path to Vince.exe>"            apply
    python patch_spooky_fov.py "<path to Vince.exe>" --restore  undo (puts back only the original shader block)
"""
import math, os, shutil, struct, sys

SHADER_OFFSET = 0x3903F8
SHADER_SIZE = 49244
ORIGINAL_HASH = 'd34cc26f-f0941543-d32e4527-0bbb377e'
BACKUP_SUFFIX = '.before-spookyfix'

# SpookyVS.hlsl from the game's own source with one added line (see SpookyVS_fixed.hlsl), compiled for vs_4_1.
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'SpookyVS_fixed.bin'), 'rb') as _f:
    FIXED_SHADER = _f.read()  # was a base64 literal here

_S = [7, 12, 17, 22] * 4 + [5, 9, 14, 20] * 4 + [4, 11, 16, 23] * 4 + [6, 10, 15, 21] * 4
_K = [int(abs(math.sin(i + 1)) * 2 ** 32) & 0xFFFFFFFF for i in range(64)]


def _transform(state, block):
    m = struct.unpack('<16I', block)
    a, b, c, d = state
    for i in range(64):
        if i < 16:
            f, g = (b & c) | (~b & d), i
        elif i < 32:
            f, g = (d & b) | (~d & c), (5 * i + 1) % 16
        elif i < 48:
            f, g = b ^ c ^ d, (3 * i + 5) % 16
        else:
            f, g = c ^ (b | ~d), (7 * i) % 16
        f = (f + a + _K[i] + m[g]) & 0xFFFFFFFF
        a, d, c = d, c, b
        b = (b + (((f << _S[i]) | (f >> (32 - _S[i]))) & 0xFFFFFFFF)) & 0xFFFFFFFF
    return [(x + y) & 0xFFFFFFFF for x, y in zip(state, (a, b, c, d))]


def dxbc_checksum(container):
    """D3D's shader container checksum (a modified MD5 over everything after the hash)."""
    data = container[20:]
    n = len(data)
    bits = (n * 8) & 0xFFFFFFFF
    left = n & 63
    full = n - left
    st = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476]
    for o in range(0, full, 64):
        st = _transform(st, data[o:o + 64])
    blk = bytearray(64)
    if left >= 56:
        blk[:left] = data[full:]
        blk[left] = 0x80
        st = _transform(st, bytes(blk))
        blk = bytearray(64)
        struct.pack_into('<I', blk, 0, bits)
    else:
        struct.pack_into('<I', blk, 0, bits)
        blk[4:4 + left] = data[full:]
        blk[4 + left] = 0x80
    struct.pack_into('<I', blk, 60, (bits >> 2) | 1)
    st = _transform(st, bytes(blk))
    return struct.pack('<4I', *st)


def hash_str(container):
    return '-'.join('%08x' % x for x in struct.unpack_from('<4I', container, 4))


def pad_to(container, size):
    """Add an empty private chunk so the shader is exactly size bytes, then re-sign it."""
    n = struct.unpack_from('<I', container, 28)[0]
    offs = [o + 4 for o in struct.unpack_from('<%dI' % n, container, 32)]
    body = container[32 + 4 * n:]
    pad_off = 32 + 4 * (n + 1) + len(body)
    pad_len = size - pad_off - 8
    out = bytearray(container[:32])
    struct.pack_into('<II', out, 24, size, n + 1)
    out += struct.pack('<%dI' % (n + 1), *(offs + [pad_off])) + body
    out += b'PRIV' + struct.pack('<I', pad_len) + bytes(pad_len)
    out[4:20] = dxbc_checksum(bytes(out))
    return bytes(out)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    exe = sys.argv[1]
    backup = exe + BACKUP_SUFFIX
    data = bytearray(open(exe, 'rb').read())
    if '--restore' in sys.argv[2:]:
        # Put back only the original shader block, so other patches applied since (e.g. particle_aspect.py) stay.
        if not os.path.exists(backup):
            sys.exit('No backup found at ' + backup)
        original = open(backup, 'rb').read()[SHADER_OFFSET:SHADER_OFFSET + SHADER_SIZE]
        if hash_str(original) != ORIGINAL_HASH or dxbc_checksum(original) != original[4:20]:
            sys.exit('The backup does not hold the original SpookyVS shader. Nothing changed.')
        data[SHADER_OFFSET:SHADER_OFFSET + SHADER_SIZE] = original
        with open(exe, 'r+b') as f:
            f.write(data)
        print('Restored the original SpookyVS in', exe)
        return

    current = bytes(data[SHADER_OFFSET:SHADER_OFFSET + SHADER_SIZE])
    if current[:4] != b'DXBC':
        sys.exit('Unexpected Vince.exe: no shader at the expected offset. Nothing changed.')
    patched = pad_to(FIXED_SHADER, SHADER_SIZE)
    if current == patched:
        print('Already patched.')
        return
    if hash_str(current) != ORIGINAL_HASH or dxbc_checksum(current) != current[4:20]:
        sys.exit('The SpookyVS shader in this Vince.exe is not the expected original (hash %s). Nothing changed.' % hash_str(current))
    assert dxbc_checksum(patched) == patched[4:20]

    if not os.path.exists(backup):
        shutil.copy2(exe, backup)
        print('Backup:', backup)
    data[SHADER_OFFSET:SHADER_OFFSET + SHADER_SIZE] = patched
    with open(exe, 'r+b') as f:
        f.write(data)
    print('Patched SpookyVS in', exe, '(new hash %s)' % hash_str(patched))


if __name__ == '__main__':
    main()
