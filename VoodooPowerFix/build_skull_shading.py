"""Stage the power skull (world and HUD) onto the approved pickup reflection blend.

The skull models share the pickup's material family (powerIcon base, envPowerIcon
cube, powerSkull mask) but were never marked, so they kept the Remaster blend.
Marking them with the existing pickup marker reuses the installed shader branch;
no executable bytes change. Apply/revert use pickup_shading.py's installer.
"""
import argparse
import json
from pathlib import Path
import struct

from build_pickup_shaders import MARKER, sha
from inspect_pickups import entries, read_entry
from pickup_shading import rebuild_compressed

MODELS = {'mysterybox_base.gator': (5, 0xffff, 0xffff, 6, 7, 0xffff),
          'goldenjunebug_base.gator': (5, 0xffff, 0xffff, 6, 7, 0xffff),
          'interfacemysterybox_base.gator': (4, 0xffff, 0xffff, 5, 6, 0xffff),
          'interfacegoldenjunebug_base.gator': (4, 0xffff, 0xffff, 5, 6, 0xffff)}
TEXTURES = ('powericon.dds', 'envpowericon.dds', 'powerskull.dds')


def mark_skull(model, name):
    if model[:8] != b'GATR\x1d\0\0\0' or struct.unpack_from('<I', model, 0x1c)[0] != 1:
        raise ValueError(f'Unexpected skull model layout: {name}')
    material, = struct.unpack_from('<I', model, 0x3c)
    table, pool = struct.unpack_from('<II', model, 0x4c)
    indices = struct.unpack_from('<6H', model, material)
    if indices != MODELS[name]:
        raise ValueError(f'Unexpected skull texture bindings: {name}')
    def string(i):
        start = pool + struct.unpack_from('<I', model, table + 4 * i)[0]
        return model[start:model.index(0, start)].decode('ascii').lower()
    if tuple(string(indices[i]) for i in (0, 3, 4)) != TEXTURES:
        raise ValueError(f'Unexpected skull texture names: {name}')
    reflectivity, power = struct.unpack_from('<ff', model, material + 0x14)
    if power == MARKER:
        return bytes(model)
    if reflectivity != 0 or power != 20.0:
        raise ValueError(f'Skull material already modified: {name}')
    result = bytearray(model)
    struct.pack_into('<f', result, material + 0x18, MARKER)
    return bytes(result)


def patch_models(outer):
    es = entries(outer)
    if 'models.hot' not in es:
        return None
    inner = read_entry(outer, es['models.hot'])[1]
    models = entries(inner)
    names = set(models) & set(MODELS)
    changed = bytearray(inner)
    replacements = {}
    for name in sorted(names):
        e = models[name]
        if e['packed'] or e['hs']:
            raise ValueError('Expected plain nested model')
        model = read_entry(inner, e)[1]
        marked = mark_skull(model, name)
        if marked != model:
            changed[e['off']:e['off'] + len(marked)] = marked
            replacements[name] = marked
    if not replacements:
        return None
    for name, e in models.items():
        if read_entry(changed, e)[1] != replacements.get(name, read_entry(inner, e)[1]):
            raise ValueError('Unrelated model payload changed')
    return rebuild_compressed(outer, {'models.hot': bytes(changed)}), sorted(replacements)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--game', type=Path, required=True)
    p.add_argument('--stage', type=Path, default=Path('build/skull-shading-v1'))
    args = p.parse_args()
    if args.stage.exists():
        raise ValueError('Choose a fresh recovery directory')
    records = []
    for path in sorted((args.game / 'vincedata/levels').rglob('modelsAndAnims.hot')):
        before = path.read_bytes()
        patched = patch_models(before)
        if patched is None:
            continue
        after, names = patched
        relative = path.relative_to(args.game)
        for label, blob in [('before', before), ('after', after)]:
            target = args.stage / label / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(blob)
        records.append(dict(path=relative.as_posix(), before=sha(before), after=sha(after), models=names))
    if not records:
        raise ValueError('No unmarked skull models found')
    (args.stage / 'manifest.json').write_text(json.dumps(dict(game=str(args.game.resolve()),
        marker=MARKER, status='Power skull world/HUD models marked for existing pickup blend; executable unchanged',
        files=records), indent=2) + '\n')
    for r in records:
        print(r['path'], ', '.join(r['models']))
    print(f'Staged {len(records)} archives with exact backups; installation unchanged.')


if __name__ == '__main__':
    main()
