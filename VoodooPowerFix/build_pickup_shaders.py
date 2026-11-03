"""Build a pickup-scoped Phong correction from locally extracted game sources.

Game shader source and resulting executable/DXBC remain in ignored build/.
This helper never modifies the installation. The original non-pickup branch
retains its source expression. A model material marker selects the correction.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

MARKER = 20.125
SHADERS = {
    34: (0x4ECB20, 133124, 'a72f4df45fc470bbacae4e774f03aafa84847787add3538275f572a9f336b660', 'OptimizedPhongPS'),
    36: (0x51F358, 130404, '2e6abe97b4e781a6dce22b32938db28e2707b24aa68b6177d4544af9047bb0f3', 'PhongPS'),
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


STREAMS = (9, 12, 11, 10, 8)


def source_for(folder, index, corrected, comparison=False):
    texts = {stream: (folder / f'{index:02d}-stream{stream}-0.hlsl').read_text() for stream in STREAMS}
    return source_text(texts, index, corrected, comparison)


def source_text(texts, index, corrected, comparison=False):
    """Pure core of source_for: texts = {PDB stream number: extracted HLSL text}."""
    pieces = ['#define USE_VINCE_MATH 1\n#define USE_AMBIENT_LIGHT 1\n']
    # Reconstruct the include graph in dependency order, retaining include guards.
    for stream in STREAMS:
        text = texts[stream]
        text = re.sub(r'^\s*#include[^\n]*', '', text, flags=re.M)
        if stream == 10 and corrected and index == 36:
            text = text.replace('gMaterial.specularPower',
                f'(abs(gMaterial.specularPower - {MARKER}) < 0.001 ? 20.0 : gMaterial.specularPower)')
        if stream == 8 and corrected:
            old = 'color.xyz += specularColor * reflectionColor.xyz;'
            if text.count(old) != 1:
                raise ValueError('Unexpected Phong source')
            marker = 'in_p.vMaterial.w' if index == 34 else 'gMaterial.specularPower'
            enabled = 'in_p.vMaterial.x' if index == 34 else 'gMaterial.reflectivity'
            if index == 34:
                text = text.replace('const float specularPower = in_p.vMaterial.w;',
                    f'const float specularPower = abs(in_p.vMaterial.w - {MARKER}) < 0.001 ? 20.0 : in_p.vMaterial.w;')
            # Replace the combined lighting+reflection expression only for a
            # marked pickup. Original Xbox gatorenvmap interpolates lit base
            # and environment using reflection-mask RGB times material
            # reflectivity (forced to 1 by Xbox when a reflection mask exists).
            # The Xbox final combiner adds its vertex lighting specular term.
            # Keep PC lighting here, but remove its incorrect mask on that term.
            selector = f'abs({marker} - {MARKER}) < 0.001'
            if comparison:
                selector += f' && {enabled} > 0.5'
            new = f'''if ({selector})
            {{
                float3 legacyBase = fColor.rgb * in_p.vColor.rgb * diffuse;
                float3 legacyWeight = saturate(specularColor);
                color.rgb = saturate(lerp(saturate(legacyBase), reflectionColor.rgb, legacyWeight) + specular);
            }}
            else
            {{
                {old}
            }}'''
            text = text.replace(old, new)
        pieces.append(text)
    return '\n'.join(pieces)


def file_va(buf, offset):
    pe = struct.unpack_from('<I', buf, 60)[0]
    count = struct.unpack_from('<H', buf, pe + 6)[0]
    optional = pe + 24
    base = struct.unpack_from('<I', buf, optional + 28)[0]
    sections = optional + struct.unpack_from('<H', buf, pe + 20)[0]
    for i in range(count):
        _, rva, size, raw = struct.unpack_from('<4I', buf, sections + i * 40 + 8)
        if raw <= offset < raw + size:
            return base + rva + offset - raw
    raise ValueError('Shader outside PE sections')


def patch_shader(buf, index, shader):
    offset, capacity, expected, _ = SHADERS[index]
    if sha(buf[offset:offset + capacity]) != expected:
        raise ValueError(f'Unexpected original shader {index}')
    if len(shader) > capacity or shader[:4] != b'DXBC':
        raise ValueError('Replacement shader does not fit')
    descriptor = struct.pack('<II', file_va(buf, offset), capacity)
    sites = [m.start() for m in re.finditer(re.escape(descriptor), buf)]
    if len(sites) != 1:
        raise ValueError(f'Expected exactly one shader descriptor, found {sites}')
    out = bytearray(buf)
    out[offset:offset + capacity] = shader.ljust(capacity, b'\0')
    struct.pack_into('<I', out, sites[0] + 4, len(shader))
    return bytes(out), sites[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sources', type=Path, required=True)
    parser.add_argument('--d3d-tools', type=Path, required=True)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=Path('build/pickup-shading-v1'))
    parser.add_argument('--comparison', action='store_true', help='Build historical runtime reflectivity/F8 gate; default is always-on')
    args = parser.parse_args()
    if args.output.exists():
        raise ValueError('Choose a fresh output folder')
    sys.path.insert(0, str(args.d3d_tools))
    from d3d_tools import compile_shader, validate_pixel_shader
    original = args.exe.read_bytes()
    result = original
    args.output.mkdir(parents=True)
    manifest = dict(marker=MARKER, comparison=args.comparison, input_sha256=sha(original), shaders=[])
    for index, (_, capacity, _, entry) in SHADERS.items():
        for corrected in (False, True):
            kind = 'patched' if corrected else 'baseline'
            source = args.output / f'{index}-{kind}.hlsl'
            source.write_text(source_for(args.sources, index, corrected, args.comparison))
            bytecode = compile_shader(source, entry, 'ps_4_1')
            validate_pixel_shader(bytecode)
            (args.output / f'{index}-{kind}.dxbc').write_bytes(bytecode)
            if corrected:
                result, descriptor = patch_shader(result, index, bytecode)
                manifest['shaders'].append(dict(index=index, bytes=len(bytecode),
                    capacity=capacity, descriptor_offset=descriptor, sha256=sha(bytecode)))
    manifest['output_sha256'] = sha(result)
    manifest['validation'] = 'D3DCompile ps_4_1 and WARP CreatePixelShader validation passed for baseline and modified shaders'
    (args.output / 'Vince.exe').write_bytes(result)
    (args.output / 'Vince.exe.before-pickup-shading').write_bytes(original)
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))
    print('Staged only; installed game unchanged.')


if __name__ == '__main__':
    main()
