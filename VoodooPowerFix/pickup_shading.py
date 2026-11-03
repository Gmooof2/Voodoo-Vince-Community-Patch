"""Stage, install or undo the power-pickup-only reflection compositing fix.

Run stage after build_pickup_shaders.py. Locally staged game assets and exact
backups are never committed or distributed. Full hashes prevent stale installs
or restoring over later mods. Existing texture transplant is independent.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import zlib

from build_pickup_shaders import MARKER, sha
from inspect_pickups import entries, read_entry


def rebuild_compressed(buf, replace):
    es = entries(buf)
    start = min(e['off'] for e in es.values())
    out = bytearray(buf[:start])
    for name, e in es.items():
        if e['hs']:
            raise ValueError('Expected headerless nested archive resources')
        if name in replace:
            if not e['packed'] or e['packed'] == e['raw']:
                raise ValueError('Expected compressed outer models entry')
            data = replace[name]
            blob = zlib.compress(data, 9)
            struct.pack_into('<II', out, e['pos'] + 16, len(data), len(blob))
        else:
            blob = buf[e['off']:e['off'] + (e['packed'] or e['raw'])]
        out += b'\0' * (-len(out) % 4)
        struct.pack_into('<Q', out, e['pos'] + 24, len(out))
        out += blob
    out += b'\0' * (-len(out) % 4)
    struct.pack_into('<I', out, 16, len(out))
    for name, e in es.items():
        expected = replace[name] if name in replace else read_entry(buf, e)[1]
        if read_entry(out, entries(out)[name])[1] != expected:
            raise ValueError('Outer archive round trip failed')
    return bytes(out)


def mark_pickup(model):
    if model[:4] != b'GATR' or struct.unpack_from('<I', model, 4)[0] != 29:
        raise ValueError('Expected GATR29')
    material = struct.unpack_from('<I', model, 0x3c)[0]
    if struct.unpack_from('<I', model, 0x1c)[0] != 1:
        raise ValueError('Expected single-material pickup')
    if struct.unpack_from('<6H', model, material) != (3, 0xffff, 0xffff, 4, 5, 0xffff):
        raise ValueError('Unexpected pickup texture bindings')
    offset = material + 0x18
    if struct.unpack_from('<f', model, offset)[0] == MARKER:
        return bytes(model)
    if struct.unpack_from('<f', model, offset)[0] != 20.0:
        raise ValueError('Unexpected pickup specular power')
    result = bytearray(model)
    struct.pack_into('<f', result, offset, MARKER)
    assert result[:offset] == model[:offset] and result[offset+4:] == model[offset+4:]
    return bytes(result)


def patch_models(buf):
    outer = entries(buf)
    if 'models.hot' not in outer:
        return None
    _, inner = read_entry(buf, outer['models.hot'])
    models = entries(inner)
    if 'powericon_base.gator' not in models:
        return None
    e = models['powericon_base.gator']
    if e['hs'] or e['packed']:
        raise ValueError('Expected uncompressed pickup model')
    _, model = read_entry(inner, e)
    marked = mark_pickup(model)
    if marked == model:
        return None
    replacement = bytearray(inner)
    replacement[e['off']:e['off'] + len(marked)] = marked
    assert len(replacement) == len(inner)
    for name, entry in models.items():
        expected = marked if name == 'powericon_base.gator' else read_entry(inner, entry)[1]
        assert read_entry(replacement, entries(replacement)[name])[1] == expected
    return rebuild_compressed(buf, {'models.hot': bytes(replacement)})


def stage(game, shaders, output, debug_build=None):
    if output.exists():
        raise ValueError('Choose a fresh stage directory')
    shader_manifest = json.loads((shaders / 'manifest.json').read_text())
    exe = (game / 'Vince.exe').read_bytes()
    if sha(exe) != shader_manifest['input_sha256']:
        raise ValueError('Installed executable changed since shader build')
    records = []
    def save(relative, before, after):
        for folder, data in (('before', before), ('after', after)):
            path = output / folder / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        records.append(dict(path=relative.as_posix(), before=sha(before), after=sha(after)))
    patched_exe = (shaders / 'Vince.exe').read_bytes()
    if sha(patched_exe) != shader_manifest['output_sha256']:
        raise ValueError('Staged executable hash changed')
    save(Path('Vince.exe'), exe, patched_exe)
    if debug_build is not None:
        debug_manifest = json.loads((debug_build / 'manifest.json').read_text())
        before_debug = (game / 'XINPUT9_1_0.dll').read_bytes()
        after_debug = (debug_build / 'XINPUT9_1_0.dll').read_bytes()
        if sha(before_debug) != debug_manifest['input_sha256'] or sha(after_debug) != debug_manifest['output_sha256']:
            raise ValueError('Debug module changed since F8 build')
        save(Path('XINPUT9_1_0.dll'), before_debug, after_debug)
    for path in sorted((game / 'vincedata/levels').rglob('modelsAndAnims.hot')):
        before = path.read_bytes()
        after = patch_models(before)
        if after is not None:
            save(path.relative_to(game), before, after)
    if not any(r['path'].endswith('modelsAndAnims.hot') for r in records):
        raise ValueError('No pickup models found')
    manifest = dict(game=str(game.resolve()), marker=MARKER,
        status='experimental reflection compositing; retains PC lighting and per-pixel reflection',
        files=records)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    count = sum(r['path'].endswith('modelsAndAnims.hot') for r in records)
    print(f'Staged executable, {count} pickup model archives' + (' and F8 debug module.' if debug_build else '.') + ' All other model and animation resources verified unchanged.')


def install(output, reverse=False):
    manifest = json.loads((output / 'manifest.json').read_text())
    game = Path(manifest['game']).resolve()
    src, dst = ('after', 'before') if reverse else ('before', 'after')
    pending = []
    for record in manifest['files']:
        path = (game / record['path']).resolve()
        path.relative_to(game)
        blob = (output / dst / record['path']).read_bytes()
        if sha(blob) != record[dst]:
            raise ValueError(f'Corrupt staged file: {record["path"]}')
        if sha((output / src / record['path']).read_bytes()) != record[src]:
            raise ValueError(f'Corrupt recovery file: {record["path"]}')
        current = sha(path.read_bytes())
        if current == record[dst]:
            continue
        if current != record[src]:
            raise ValueError(f'File changed since preparation: {path}')
        pending.append((path, blob, record))
    completed = []
    def overwrite(path, blob):
        # Preserve the existing Windows ACL when replacement/delete rights are
        # restricted independently of write rights. Backups were hash-checked.
        with path.open('r+b') as f:
            f.write(blob)
            f.truncate()
            f.flush()
            os.fsync(f.fileno())
    try:
        for path, blob, record in pending:
            if sha(path.read_bytes()) != record[src]:
                raise ValueError(f'File changed during installation: {path}')
            temporary = path.with_name(path.name + '.pickup-shading-tmp')
            recorded = False
            try:
                with temporary.open('xb') as f:
                    f.write(blob)
                try:
                    os.replace(temporary, path)
                except PermissionError:
                    if sha(path.read_bytes()) != record[src]:
                        raise ValueError(f'File changed during replacement: {path}')
                    completed.append((path, record))
                    recorded = True
                    overwrite(path, blob)
            finally:
                if temporary.exists():
                    temporary.unlink()
            if not recorded:
                completed.append((path, record))
        for record in manifest['files']:
            if sha((game / record['path']).read_bytes()) != record[dst]:
                raise ValueError('Installed hash verification failed')
    except Exception:
        for path, record in reversed(completed):
            overwrite(path, (output / src / record['path']).read_bytes())
        raise
    print(f'{"Reverted" if reverse else "Installed"} pickup shading: {len(pending)} files changed; all {len(manifest["files"])} verified.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('stage', 'apply', 'revert'))
    parser.add_argument('--game', type=Path, help='Game folder; required for stage')
    parser.add_argument('--shaders', type=Path, default=Path('build/pickup-shaders-always-on-v1'))
    parser.add_argument('--debug-build', type=Path, help='Optional historical F8 comparison module; omitted in normal builds')
    parser.add_argument('--output', type=Path, default=Path('build/pickup-shading-always-on-v1'))
    args = parser.parse_args()
    if args.mode == 'stage' and args.game is None:
        parser.error('--game is required for stage')
    if args.mode == 'stage':
        stage(args.game, args.shaders, args.output, args.debug_build)
    else:
        install(args.output, args.mode == 'revert')


if __name__ == '__main__':
    main()
