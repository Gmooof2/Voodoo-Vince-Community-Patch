// VinceSteam heat shimmer: the Xbox HeatShimmer effect (prototype default.xbe HeatShimmer::Render 0xCBE30)
// on D3D11. One camera-facing quad per shimmer node. Two scrolling noise lookups (the second offset by the
// first, like the Xbox bump-env stages 1 and 2) bend the lookup into a copy of the scene, faded out by the
// Xbox circle mask (TextureManager::_circleTexture: cos(pi * distance from the centre), 0 past radius 0.5).
// Compiled by tools/compile_shaders.py into src/shaders.h.

cbuffer Shimmer : register(b0)
{
    float4 corner[4];   // clip-space quad corners, triangle-strip order
    float4 noise0;      // xy: uv scale, zw: uv offset of the first noise lookup
    float4 noise1;      // the same for the second lookup
    float4 perturb;     // xy: first lookup's offset of the second (noise uv units), zw: second's offset of the scene (pixels)
    float4 screen;      // xy: 1 / scene size in pixels, z: opacity, w: 1 = debug (a red disc instead of the haze)
};

Texture2D Noise : register(t0);
Texture2D Scene : register(t1);
SamplerState WrapLinear : register(s0);
SamplerState ClampLinear : register(s1);

struct V2P
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

V2P VS(uint id : SV_VertexID)
{
    V2P o;
    o.pos = corner[id & 3];
    o.uv = float2(id & 1, (id >> 1) & 1);
    return o;
}

float4 PS(V2P i) : SV_Target
{
    float mask = max(cos(3.14159265 * length(i.uv - 0.5)), 0.0);
    clip(mask - 1.0 / 256.0);
    float n0 = Noise.Sample(WrapLinear, i.uv * noise0.xy + noise0.zw).r * 2.0 - 1.0;
    float n1 = Noise.Sample(WrapLinear, i.uv * noise1.xy + noise1.zw + n0 * perturb.xy).r * 2.0 - 1.0;
    float3 c = Scene.Sample(ClampLinear, (i.pos.xy + n1 * perturb.zw) * screen.xy).rgb;
    if (screen.w > 0.5) return float4(1.0, 0.1, 0.1, 0.6 * mask);
    return float4(c, mask * screen.z);
}
