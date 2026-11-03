"""Restore the opening animation of the Bump-Off floor gates (carnival/area_bumpercarbumpoff).

The gates are BumperSwitch objects: model bumperswitch_base.gator, animation BumperSwitch_activate
(bones buttonpush, door01open, door02open). Voodoo Vince Remastered starts that animation exactly
as the Xbox prototype does, but its re-exported model has all 421 vertices skinned to bone 2 (the
base), so the door leaves and the button never move. This rewrites the per-vertex bone index of
the model to the prototype's assignment; nothing else changes.

Library use (AIO build pipelines): patch_models_hot(data) takes the area's current
modelsAndAnims.hot bytes and returns new bytes, or None when the model is already fixed. Only the
bone fields of that one model change; every other archive entry is carried over unchanged.

Commands:
  status  <game>               report the gate model: retail, fixed, or unknown
  stage   <game> <stage_dir>   read the game, write the fixed modelsAndAnims.hot + manifest
  install <game> <stage_dir>   copy the staged file in, backing up the original
  restore <game> <stage_dir>   put the backed-up original back

Guards: the model is only patched when its bytes match the pinned retail Steam 1.14.2.0 hash, and
is recognised as done when it matches the pinned fixed hash; anything else is refused. Other
entries of modelsAndAnims.hot may have been changed by other mods. Install and restore
additionally check whole-file hashes so they never overwrite unexpected files.
"""
import argparse
import hashlib
import json
import shutil
import struct
import zlib
from pathlib import Path

AREA = 'carnival/area_bumpercarbumpoff'
INNER = 'models.hot'
MODEL = 'bumperswitch_base.gator'
RETAIL_SHA256 = 'eac2c3728dd35003d1d19242a90a7160932a243a8dd619f5c8a326c74ed6de0d'
FIXED_SHA256 = '39eb34c4f63dd63e637e7435ce87258b4982c64115b3e5caf173711fffd9fe6c'

# Bone of each vertex in order, as (bone, run length). Taken from the Xbox prototype's model
# (its vertex stores bone * 3), matched to the remaster's vertices by position and mesh piece.
BONE_RUNS = [(2, 52), (3, 16), (4, 16), (2, 44), (3, 32), (4, 32), (2, 16), (3, 8), (4, 8), (3, 8),
             (4, 8), (2, 132), (5, 49)]
GATR_VERTEX = 64         # remaster GATR vertex: pos float3, uint, bone uint @16, ...
BONE_OFFSET = 16
VERTICES = 0x80


def sha(data):
    return hashlib.sha256(data).hexdigest()


# ---------------------------------------------------------------- HOT archives

def hot_entries(data):
    """PC HOT layout: 28-byte header, 32-byte entries, name pool, optional zlib."""
    if data[:4] != b'HOT ':
        raise ValueError('Not a HOT archive')
    version, _, _, size, names, count = struct.unpack_from('<6I', data, 4)
    if version != 1 or size != len(data) or names != 28 + 32 * count:
        raise ValueError('Unsupported HOT layout')
    out = {}
    for i in range(count):
        pos = 28 + 32 * i
        name_off, hs, ho, raw, packed, off = struct.unpack_from('<Q4IQ', data, pos)
        end = data.index(b'\0', names + name_off)
        name = data[names + name_off:end].decode('ascii')
        out[name] = dict(pos=pos, hs=hs, ho=ho, raw=raw, packed=packed, off=off)
    return out


def read_entry(data, e):
    blob = data[e['off']:e['off'] + (e['packed'] or e['raw'] - e['hs'])]
    blob = data[e['ho']:e['ho'] + e['hs']] + (zlib.decompress(blob) if e['packed'] else blob)
    if len(blob) != e['raw']:
        raise ValueError('Unexpected entry size')
    return blob


def rebuild(data, replace):
    """Rewrite an archive with some entries replaced, keeping each entry's compression."""
    es = hot_entries(data)
    if any(e['hs'] for e in es.values()):
        raise ValueError('Rewriting archives with separate entry headers is not supported')
    start = min(e['off'] for e in es.values())
    out = bytearray(data[:start])
    for name, e in sorted(es.items(), key=lambda kv: kv[1]['off']):
        if name in replace:
            new = replace[name]
            blob = zlib.compress(new, 9) if e['packed'] else new
            struct.pack_into('<II', out, e['pos'] + 16, len(new), len(blob) if e['packed'] else 0)
        else:
            blob = data[e['off']:e['off'] + (e['packed'] or e['raw'])]
        out += b'\0' * (-len(out) % 4)
        struct.pack_into('<Q', out, e['pos'] + 24, len(out))
        out += blob
    out += b'\0' * (-len(out) % 4)
    struct.pack_into('<I', out, 16, len(out))
    rebuilt = hot_entries(bytes(out))
    for name, e in es.items():
        expected = replace.get(name) or read_entry(data, e)
        if read_entry(out, rebuilt[name]) != expected:
            raise ValueError(f'Archive round trip failed: {name}')
    return bytes(out)


# ---------------------------------------------------------------- the model

def find_model(data):
    """(models.hot bytes, its model entry) of a modelsAndAnims.hot."""
    outer = hot_entries(data)
    if INNER not in outer:
        raise ValueError(f'{INNER} missing')
    models = read_entry(data, outer[INNER])
    e = next((e for n, e in hot_entries(models).items() if n.lower() == MODEL), None)
    if e is None:
        raise ValueError(f'{MODEL} missing')
    return models, e


def model_state(data):
    """'retail', 'fixed' or 'unknown'."""
    models, e = find_model(data)
    return {RETAIL_SHA256: 'retail', FIXED_SHA256: 'fixed'}.get(sha(read_entry(models, e)), 'unknown')


def fix_model(model):
    bones = [b for b, n in BONE_RUNS for _ in range(n)]
    if struct.unpack_from('<I', model, 0x10)[0] != len(bones):
        raise ValueError('Unexpected vertex count')
    out = bytearray(model)
    for i, bone in enumerate(bones):
        struct.pack_into('<I', out, VERTICES + GATR_VERTEX * i + BONE_OFFSET, bone)
    return bytes(out)


def patch_models_hot(data):
    """Return (new modelsAndAnims.hot bytes or None, report). Raises on unknown model data."""
    state = model_state(data)
    if state == 'fixed':
        return None, {'state': 'already fixed'}
    if state != 'retail':
        raise ValueError(f'{MODEL} is neither retail nor fixed; refusing to patch')
    models, e = find_model(data)
    if e['packed'] or e['hs']:
        raise ValueError('Expected an uncompressed model entry')
    old = read_entry(models, e)
    new = fix_model(old)
    if sha(new) != FIXED_SHA256:
        raise ValueError('Fixed model does not match the pinned hash')
    models_new = bytearray(models)
    models_new[e['off']:e['off'] + len(new)] = new
    inner = hot_entries(models)
    for n, entry in hot_entries(bytes(models_new)).items():
        if read_entry(bytes(models_new), entry) != (new if entry['off'] == e['off'] else read_entry(models, inner[n])):
            raise ValueError(f'{INNER} round trip failed: {n}')
    changed = sum(a != b for a, b in zip(old, new))
    return rebuild(data, {INNER: bytes(models_new)}), {'state': 'fixed now', 'bytes_changed': changed}


# ---------------------------------------------------------------- commands

REL = f'vincedata/levels/{AREA}/modelsAndAnims.hot'


def game_file(game):
    path = Path(game) / REL
    if not path.is_file():
        raise SystemExit(f'No {REL} under {game}')
    return path


def status(game):
    print(f'{AREA}: {model_state(game_file(game).read_bytes())}')


def stage(game, stage_dir):
    stage_dir = Path(stage_dir)
    if stage_dir.exists() and any(stage_dir.iterdir()):
        raise SystemExit('Choose an empty or new stage directory')
    data = game_file(game).read_bytes()
    try:
        fixed, report = patch_models_hot(data)
    except ValueError as e:
        raise SystemExit(str(e))
    if fixed is None:
        raise SystemExit(f'{AREA}: {report["state"]}; nothing to stage')
    out = stage_dir / 'files' / REL
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(fixed)
    manifest = {'files': {REL: {'source_sha256': sha(data), 'fixed_sha256': sha(fixed), 'report': report}}}
    (stage_dir / 'manifest.json').write_text(json.dumps(manifest, indent=1))
    print(f'{AREA}: {report["bytes_changed"]} bytes changed; staged in {stage_dir}')


def load_manifest(stage_dir):
    return json.loads((Path(stage_dir) / 'manifest.json').read_text())


def install(game, stage_dir):
    stage_dir = Path(stage_dir)
    for rel, info in load_manifest(stage_dir)['files'].items():
        target = Path(game) / rel
        current = sha(target.read_bytes())
        if current == info['fixed_sha256']:
            print(f'{rel}: already installed')
            continue
        if current != info['source_sha256']:
            raise SystemExit(f'{rel}: game file changed since staging; nothing installed')
        if sha((stage_dir / 'files' / rel).read_bytes()) != info['fixed_sha256']:
            raise SystemExit(f'{rel}: staged file is damaged; nothing installed')
        backup = stage_dir / 'backup' / rel
        backup.parent.mkdir(parents=True, exist_ok=True)
        if not backup.exists():
            shutil.copyfile(target, backup)
        shutil.copyfile(stage_dir / 'files' / rel, target)
        print(f'Installed {rel}')


def restore(game, stage_dir):
    stage_dir = Path(stage_dir)
    for rel, info in load_manifest(stage_dir)['files'].items():
        target = Path(game) / rel
        current = sha(target.read_bytes())
        if current == info['source_sha256']:
            print(f'{rel}: already the original')
            continue
        if current != info['fixed_sha256']:
            raise SystemExit(f'{rel}: game file is neither the fixed nor the original version; nothing restored')
        backup = stage_dir / 'backup' / rel
        if not backup.exists() or sha(backup.read_bytes()) != info['source_sha256']:
            raise SystemExit(f'{rel}: no valid backup; nothing restored')
        shutil.copyfile(backup, target)
        print(f'Restored {rel}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=['status', 'stage', 'install', 'restore'])
    parser.add_argument('game', help='Voodoo Vince Remastered install folder')
    parser.add_argument('stage_dir', nargs='?')
    args = parser.parse_args()
    if args.command != 'status' and not args.stage_dir:
        parser.error(f'{args.command} needs a stage_dir')
    if args.command == 'status':
        status(args.game)
    elif args.command == 'stage':
        stage(args.game, args.stage_dir)
    elif args.command == 'install':
        install(args.game, args.stage_dir)
    else:
        restore(args.game, args.stage_dir)


if __name__ == '__main__':
    main()
