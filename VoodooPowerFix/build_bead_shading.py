"""Stage a bead-only reflection trial on top of the approved pickup/halo build."""
import argparse
import json
from pathlib import Path
import re
import struct
import sys

from build_pickup_shaders import SHADERS, file_va, sha
from inspect_pickups import entries, read_entry
from pickup_shading import rebuild_compressed

MARKER = 74.875
POWER = struct.unpack('<f', struct.pack('<f', 74.89))[0]
MODELS = {'bead_base.gator': 'envBead_64_.dds',
          'bead2_base.gator': 'envBead2_64_.dds',
          'bead3_base.gator': 'envBead3_64_.dds'}


def bead_source(source, index):
    material = 'in_p.vMaterial.w' if index == 34 else 'gMaterial.specularPower'
    weight = 'in_p.vMaterial.x' if index == 34 else 'gMaterial.reflectivity'
    old = f'abs({material} - 20.125) < 0.001 ? 20.0 : {material}'
    if old not in source:
        raise ValueError('Missing existing pickup exponent correction')
    source = source.replace(old, f'abs({material} - {MARKER}) < 0.001 ? {POWER} : ({old})')
    old = '''else
            {
                color.xyz += specularColor * reflectionColor.xyz;
            }'''
    if source.count(old) != 1:
        raise ValueError('Unexpected existing reflection branch')
    new = f'''else if (abs({material} - {MARKER}) < 0.001)
            {{
                // Beads have no mask. Preserve their lighting and blend using
                // material reflectivity rather than the PC specular RGB.
                float3 beadBase = fColor.rgb * in_p.vColor.rgb * diffuse;
                color.rgb = saturate(lerp(saturate(beadBase), reflectionColor.rgb,
                    saturate({weight})) + specularColor * specular + gPhongEmissiveColor.rgb);
            }}
            {old}'''
    return source.replace(old, new)


def patch_slot(exe, index, prior, replacement):
    offset, capacity, _, _ = SHADERS[index]
    if exe[offset:offset+capacity] != prior.ljust(capacity, b'\0'):
        raise ValueError(f'Installed shader {index} differs from approved source build')
    if replacement[:4] != b'DXBC' or len(replacement) > capacity:
        raise ValueError('Shader exceeds embedded capacity')
    descriptor = struct.pack('<II', file_va(exe, offset), len(prior))
    sites = [m.start() for m in re.finditer(re.escape(descriptor), exe)]
    if len(sites) != 1:
        raise ValueError('Ambiguous shader descriptor')
    result = bytearray(exe)
    result[offset:offset+capacity] = replacement.ljust(capacity, b'\0')
    struct.pack_into('<I', result, sites[0]+4, len(replacement))
    return bytes(result), sites[0]


def mark_bead(model, name):
    if name not in MODELS or model[:8] != b'GATR\x1d\0\0\0':
        raise ValueError('Unexpected bead model/version')
    if struct.unpack_from('<I', model, 0x1c)[0] != 1:
        raise ValueError('Expected one bead material')
    material, = struct.unpack_from('<I', model, 0x3c)
    table, pool = struct.unpack_from('<II', model, 0x4c)
    indices = struct.unpack_from('<6H', model, material)
    if indices != (3, 65535, 65535, 4, 65535, 65535):
        raise ValueError('Unexpected bead texture bindings')
    def string(i):
        start = pool + struct.unpack_from('<I', model, table+4*i)[0]
        return model[start:model.index(0, start)].decode('ascii')
    if string(3) != 'bead.dds' or string(4) != MODELS[name]:
        raise ValueError('Unexpected bead texture names')
    reflectivity, power = struct.unpack_from('<ff', model, material+0x14)
    if reflectivity != 1 or power != POWER:
        raise ValueError('Bead settings already modified')
    result = bytearray(model)
    struct.pack_into('<f', result, material+0x18, MARKER)
    return bytes(result)


def patch_models(outer):
    es = entries(outer)
    if 'models.hot' not in es:
        return None
    inner = read_entry(outer, es['models.hot'])[1]
    models = entries(inner)
    names = set(models) & set(MODELS)
    if not names:
        return None
    if names != set(MODELS):
        raise ValueError('Incomplete bead family')
    changed = bytearray(inner)
    replacements = {}
    for name in sorted(names):
        e = models[name]
        if e['packed'] or e['hs']:
            raise ValueError('Expected plain nested model')
        model = read_entry(inner, e)[1]
        marked = mark_bead(model, name)
        changed[e['off']:e['off']+len(marked)] = marked
        replacements[name] = marked
    for name, e in models.items():
        if read_entry(changed, e)[1] != replacements.get(name, read_entry(inner, e)[1]):
            raise ValueError('Unrelated model payload changed')
    return rebuild_compressed(outer, {'models.hot': bytes(changed)})


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--game', type=Path, required=True)
    p.add_argument('--prior', type=Path, default=Path('build/pickup-shaders-always-on-v1'))
    p.add_argument('--output', type=Path, default=Path('build/bead-shaders-v1'))
    p.add_argument('--stage', type=Path, default=Path('build/bead-comparison-v1'))
    p.add_argument('--d3d-tools', type=Path, default=Path(__file__).resolve().parent.parent/'VinceWaterFix/tools')
    args = p.parse_args()
    if args.output.exists() or args.stage.exists():
        raise ValueError('Choose fresh build and recovery directories')
    sys.path.insert(0, str(args.d3d_tools.resolve()))
    from d3d_tools import compile_shader, validate_pixel_shader
    args.output.mkdir(parents=True)
    original = (args.game/'Vince.exe').read_bytes()
    result = original
    descriptors = []
    for index, (_, _, _, entry) in SHADERS.items():
        source = (args.prior/f'{index}-patched.hlsl').read_text()
        prior = (args.prior/f'{index}-patched.dxbc').read_bytes()
        # Recompile the local source to prove it describes the prior bytecode.
        baseline_path = args.output/f'{index}-baseline.hlsl'
        baseline_path.write_text(source)
        if compile_shader(baseline_path, entry, 'ps_4_1') != prior:
            raise ValueError('Prior source does not reproduce installed bytecode')
        (args.output/f'{index}-baseline.dxbc').write_bytes(prior)
        path = args.output/f'{index}-patched.hlsl'
        path.write_text(bead_source(source, index))
        bytecode = compile_shader(path, entry, 'ps_4_1')
        validate_pixel_shader(bytecode)
        (args.output/f'{index}-patched.dxbc').write_bytes(bytecode)
        result, descriptor = patch_slot(result, index, prior, bytecode)
        descriptors.append(dict(index=index, descriptor_offset=descriptor, sha256=sha(bytecode)))
    (args.output/'Vince.exe').write_bytes(result)
    manifest = dict(input_sha256=sha(original), output_sha256=sha(result),
                    marker=MARKER, original_exponent=POWER, shaders=descriptors)
    (args.output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    records = []
    def save(relative, before, after):
        for label, blob in [('before', before), ('after', after)]:
            target = args.stage/label/relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(blob)
        records.append(dict(path=relative.as_posix(), before=sha(before), after=sha(after)))
    save(Path('Vince.exe'), original, result)
    count = 0
    for path in sorted((args.game/'vincedata/levels').rglob('modelsAndAnims.hot')):
        before = path.read_bytes()
        after = patch_models(before)
        if after is not None:
            save(path.relative_to(args.game), before, after)
            count += 1
    if count != 7:
        raise ValueError(f'Expected seven bead archives, got {count}')
    (args.stage/'manifest.json').write_text(json.dumps(dict(game=str(args.game.resolve()),
        status='Always-on bead reflection trial; existing pickup shading/halo and F8 retained',
        files=records), indent=2)+'\n')
    print(f'Staged {len(records)} files with exact backups; installation unchanged.')


if __name__ == '__main__':
    main()
