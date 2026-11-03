"""Stage the scoped original-style pickup mask blur; never edits the game."""
import argparse
import json
from pathlib import Path
import re
import struct
import sys

from build_pickup_shaders import file_va, sha
from build_pickup_shaders import patch_shader, SHADERS
from stage_pickup_fov import patch_fov, FIXED as SPOOKY_FOV_SHA

OFFSET = 0x385308
CAPACITY = 45292
EXPECTED = 'ec92ff9478dd7abda2a1839fc87120e4d3e58167937b1d44e594974f0aad6ac4'
COMMON = '''
SamplerState state : register(s0);
Texture2D tex0 : register(t0);
struct HaloInput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD0;
};
'''
BASELINE = COMMON + '''
float4 SpookyPS(HaloInput p) : SV_TARGET {
    float4 t = tex0.Sample(state, p.uv);
    return float4(t.rgb, p.color.a * t.a);
}
'''
PATCHED = COMMON + '''
// Only the scoped pickup draw gets this RGB marker and a 640x480 mask.
// Preserve the existing ghost/composite expression for every other draw.
float4 SpookyPS(HaloInput p) : SV_TARGET {
    uint width, height;
    tex0.GetDimensions(width, height);
    // Grayscale marker also survives the engine's packed-color channel order.
    const float3 marker = float3(43.0, 43.0, 43.0) / 255.0;
    if (width == 640 && height == 480 && all(abs(p.color.rgb - marker) < 0.001)) {
        // Symmetric original kernel: each direction's .4/.3/.2/.1 taps,
        // combined at one half, first horizontally then vertically.
        const float offsets[8] = {-3.5,-2.5,-1.5,-0.5,0.5,1.5,2.5,3.5};
        const float weights[8] = {0.05,0.10,0.15,0.20,0.20,0.15,0.10,0.05};
        float alpha = 0;
        [unroll] for (int y = 0; y < 8; ++y) {
            [unroll] for (int x = 0; x < 8; ++x) {
                float2 uv = p.uv + float2(offsets[x] / 160.0, offsets[y] / 120.0);
                // Four reads average a coarse-pixel footprint from the denser mask.
                const float2 box = float2(0.25 / 160.0, 0.25 / 120.0);
                float coverage = tex0.SampleLevel(state, uv + box, 0).a;
                coverage += tex0.SampleLevel(state, uv - box, 0).a;
                coverage += tex0.SampleLevel(state, uv + float2(box.x,-box.y), 0).a;
                coverage += tex0.SampleLevel(state, uv + float2(-box.x,box.y), 0).a;
                alpha += weights[x] * weights[y] * coverage * 0.25;
            }
        }
        // RGB never comes from the reflective surface; its alpha is the mask.
        return float4(float3(178.0,79.0,230.0) / 255.0, saturate(alpha * p.color.a));
    }
    float4 t = tex0.Sample(state, p.uv);
    return float4(t.rgb, p.color.a * t.a);
}
'''


def patch_executable(original, shader, previous=None):
    old_length = CAPACITY
    expected = EXPECTED
    if previous:
        old_shader = (previous/'patched.dxbc').read_bytes()
        old_manifest = json.loads((previous/'manifest.json').read_text())
        if sha(old_shader) != old_manifest['shader_sha256']:
            raise ValueError('Previous shader does not match its manifest')
        expected = sha(old_shader.ljust(CAPACITY, b'\0'))
        old_length = len(old_shader)
    if sha(original[OFFSET:OFFSET+CAPACITY]) != expected:
        raise ValueError('Unexpected SpookyPS; refusing stale offsets')
    if shader[:4] != b'DXBC' or len(shader) > CAPACITY:
        raise ValueError('Replacement shader exceeds embedded slot')
    descriptor = struct.pack('<II', file_va(original, OFFSET), old_length)
    sites = [m.start() for m in re.finditer(re.escape(descriptor), original)]
    if len(sites) != 1:
        raise ValueError('Ambiguous native shader descriptor')
    out = bytearray(original)
    out[OFFSET:OFFSET+CAPACITY] = shader.ljust(CAPACITY, b'\0')
    struct.pack_into('<I', out, sites[0]+4, len(shader))
    return bytes(out), sites[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--d3d-tools', type=Path, required=True)
    parser.add_argument('--previous-build', type=Path, help='Verified prior halo build being upgraded')
    parser.add_argument('--spooky-fov-block', type=Path, default=Path('../VinceSpookyFix/fix/SpookyVS_fixed_0x3903F8.bin'), help='Required validated shared vertex-shader FOV correction')
    parser.add_argument('--surface-shaders', type=Path, help='Merge the always-on surface correction from this local build')
    parser.add_argument('--output', type=Path, default=Path('build/pickup-halo-shader-v3'))
    args = parser.parse_args()
    if args.output.exists():
        raise ValueError('Choose a fresh build folder')
    sys.path.insert(0, str(args.d3d_tools))
    from d3d_tools import compile_shader, validate_pixel_shader
    args.output.mkdir(parents=True)
    for name, source in (('baseline', BASELINE), ('patched', PATCHED)):
        path = args.output / (name+'.hlsl')
        path.write_text(source)
        shader = compile_shader(path, 'SpookyPS', 'ps_4_1')
        validate_pixel_shader(shader)
        (args.output/(name+'.dxbc')).write_bytes(shader)
    original = args.exe.read_bytes()
    result, descriptor = patch_executable(original, shader, args.previous_build)
    result = patch_fov(result, args.spooky_fov_block.read_bytes())
    if args.surface_shaders:
        for index, (off, capacity, _, _) in SHADERS.items():
            surface = (args.surface_shaders/f'{index}-patched.dxbc').read_bytes()
            if result[off:off+capacity] != surface.ljust(capacity, b'\0'):
                result, _ = patch_shader(result, index, surface)
    (args.output/'Vince.exe').write_bytes(result)
    (args.output/'Vince.exe.before-halo').write_bytes(original)
    manifest = dict(input_sha256=sha(original), output_sha256=sha(result),
        shader_sha256=sha(shader), shader_bytes=len(shader), descriptor_offset=descriptor,
        mask_size=[640,480], blur_reference_size=[160,120], coverage_samples=4, marker_rgb=0x2b2b2b, purple_rgb=0xb24fe6,
        spooky_fov_sha256=SPOOKY_FOV_SHA,
        scope='marked pickup composite only; other ghost draws retain native expression',
        validation='D3DCompile ps_4_1 and WARP shader creation validation passed')
    (args.output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
