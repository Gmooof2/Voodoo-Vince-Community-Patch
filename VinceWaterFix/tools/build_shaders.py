"""Build readable water reconstructions; never reads or modifies game files.

Usage: python tools/build_shaders.py [--output build/shaders]
Requires Windows' d3dcompiler_47.dll and D3D11 WARP. Compiles and validates all
entrypoints, then writes a manifest suitable for an integrating community patch.
"""
import argparse
import ctypes as ct
import hashlib
import json
from pathlib import Path

from d3d_tools import compile_shader, method, release, validate_pixel_shader

ROOT = Path(__file__).resolve().parents[1]
OVERLAY_VARIANTS = ('default', 'add', 'desaturate', 'illuminate', 'multiply',
                    'over', 'saturate', 'subtract')
# Proven by RenderOverlaySetup's indexed suffix table at preview VA0x607838.
# Omitted values have no corresponding recovered water overlay program.
ORIGINAL_DETAIL_TO_VARIANT = {0: 'default', 1: 'over', 4: 'add', 5: 'subtract',
                              6: 'multiply', 10: 'saturate', 11: 'desaturate', 12: 'illuminate'}


def validate_vertex_shader(code):
    runtime = ct.WinDLL('d3d11.dll', winmode=0x800)
    create = runtime.D3D11CreateDevice
    create.restype = ct.c_long
    create.argtypes = [ct.c_void_p, ct.c_uint, ct.c_void_p, ct.c_uint,
                       ct.c_void_p, ct.c_uint, ct.c_uint, ct.c_void_p,
                       ct.c_void_p, ct.c_void_p]
    device, context = ct.c_void_p(), ct.c_void_p()
    feature = ct.c_uint()
    try:
        status = create(None, 5, None, 0, None, 0, 7, ct.byref(device),
                        ct.byref(feature), ct.byref(context))
        if status < 0:
            raise RuntimeError(f'WARP creation failed: 0x{status & 0xffffffff:08x}')
        storage = ct.create_string_buffer(code)
        status = method(device, 12, ct.c_long, ct.c_void_p, ct.c_size_t,
                        ct.c_void_p, ct.c_void_p)(device, storage, len(code), None, None)
        if status != 1:
            raise RuntimeError(f'Vertex shader validation failed: 0x{status & 0xffffffff:08x}')
    finally:
        release(context)
        release(device)


def build(output):
    output.mkdir(parents=True, exist_ok=True)
    generated = output / 'generated'
    generated.mkdir(exist_ok=True)
    units = [
        ('RemasterLegacyBasePS', 'RemasterLegacyBasePS.hlsl', 'LegacyBasePS', 'ps_4_1', {}),
        ('LegacyBasePS', 'LegacyWater.hlsl', 'LegacyBasePS', 'ps_4_1', {}),
        ('LegacyBaseVS', 'LegacyWaterVS.hlsl', 'LegacyBaseVS', 'vs_4_1', {}),
        ('LegacyOverlayVS', 'LegacyWaterVS.hlsl', 'LegacyOverlayVS', 'vs_4_1', {}),
    ]
    for variant_id, name in enumerate(OVERLAY_VARIANTS):
        units.append((f'LegacyOverlayPS_{name}', 'LegacyWater.hlsl', 'LegacyOverlayPS',
                      'ps_4_1', {'LEGACY_OVERLAY_PASS': 1, 'LEGACY_OVERLAY_BLEND': variant_id}))
    manifest = {'schema_version': 1,
                'warning': 'Standalone shaders require the original host, simulation and two draw passes.',
                'overlay_variant_ids': dict(zip(OVERLAY_VARIANTS, range(len(OVERLAY_VARIANTS)))),
                'original_detail_type_to_variant': ORIGINAL_DETAIL_TO_VARIANT,
                'shaders': []}
    for name, filename, entry, profile, definitions in units:
        source_path = ROOT / 'src' / filename
        source = source_path.read_text(encoding='utf-8')
        path = source_path
        if definitions:
            preamble = ''.join(f'#define {key} {value}\n' for key, value in definitions.items())
            path = generated / f'{name}.hlsl'
            path.write_text(preamble + source, encoding='utf-8', newline='\n')
        code = compile_shader(path, entry, profile)
        if profile.startswith('ps_'):
            validate_pixel_shader(code)
        else:
            validate_vertex_shader(code)
        target = output / f'{name}.cso'
        target.write_bytes(code)
        manifest['shaders'].append({'name': name, 'source': f'src/{filename}', 'entry': entry,
                                    'profile': profile, 'defines': definitions, 'bytes': len(code),
                                    'source_sha256': hashlib.sha256(source_path.read_bytes()).hexdigest(),
                                    'bytecode_sha256': hashlib.sha256(code).hexdigest(),
                                    'warp_validated': True})
        print(f'{name}: {len(code)} bytes, compiled and WARP validated')
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build' / 'shaders')
    arguments = parser.parse_args()
    build(arguments.output)
