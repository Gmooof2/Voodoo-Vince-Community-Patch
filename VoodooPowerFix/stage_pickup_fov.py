"""Stage the required shared SpookyVS FOV correction without replacing other fixes."""
import argparse
import json
from pathlib import Path
import struct

from build_pickup_shaders import file_va, sha

OFFSET = 0x3903f8
SIZE = 49244
ORIGINAL = '8773977c9d1d164b084b22f28ceb3f0249fa0bdc3345f498819b49c6ec540224'
FIXED = '35500477c851492613172a1eeb991b50968b051bae4e92ef664bab4161c59ad2'


def patch_fov(exe, fixed):
    if len(fixed) != SIZE or sha(fixed) != FIXED:
        raise ValueError('Not the validated butler SpookyVS correction')
    state = sha(exe[OFFSET:OFFSET+SIZE])
    if state not in (ORIGINAL, FIXED):
        raise ValueError('Unknown SpookyVS; merge changes rather than overwrite')
    descriptor = struct.pack('<II', file_va(exe, OFFSET), SIZE)
    if exe.count(descriptor) != 1:
        raise ValueError('Unexpected SpookyVS descriptor pointer/length')
    return exe[:OFFSET] + fixed + exe[OFFSET+SIZE:]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--game', type=Path, required=True)
    p.add_argument('--exe', type=Path)
    p.add_argument('--fixed', type=Path, default=Path(__file__).resolve().parent.parent/'VinceSpookyFix/fix/SpookyVS_fixed_0x3903F8.bin')
    p.add_argument('--output', type=Path, default=Path('build/pickup-fov-correction-v1'))
    args = p.parse_args()
    if args.output.exists():
        raise ValueError('Choose a fresh recovery folder')
    before = (args.exe or args.game/'Vince.exe').read_bytes()
    after = patch_fov(before, args.fixed.read_bytes())
    for name, blob in [('before', before), ('after', after)]:
        target = args.output/name/'Vince.exe'
        target.parent.mkdir(parents=True)
        target.write_bytes(blob)
    manifest = dict(game=str(args.game.resolve()), status='Required shared SpookyVS FOV correction',
        files=[dict(path='Vince.exe', before=sha(before), after=sha(after))])
    (args.output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
