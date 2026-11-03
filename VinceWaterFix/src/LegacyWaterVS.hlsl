// Readable reconstruction of preview Xbox WaterShader1 / WaterShader2.
// Pair these entries with LegacyBasePS / LegacyOverlayPS in LegacyWater.hlsl.
// This requires an integrating host to bind the original two passes/resources;
// it is not a drop-in shader for the current Remastered vertex layout.
// Build entries LegacyBaseVS / LegacyOverlayVS with target vs_4_1.

cbuffer LegacyVertexConstants : register(b1)
{
    // CPU supplies row-vector matrices: position * world * viewProjection.
    // They must be the same world/view transforms as used for the game mesh.
    row_major float4x4 legacyWorld;
    row_major float4x4 legacyViewProjection;
    float4 legacyCameraPosition; // World coordinates, original API constant104.
    float4 legacyFogParameters; // x=slope, y=offset; fog=clip.w*x-y.
    float4 legacyWaterColor;    // Original per-field RGBA.
    float4 legacyScroll1;       // (offsetU,offsetV,unused,16).
    float4 legacyScroll2;       // (offsetU,offsetV,unused,16).
};

struct LegacyVertexInput
{
    float3 position : POSITION;
    // Original simulated normal. x,z encode height slopes; y is not consumed.
    float3 normal : NORMAL;
    float2 bumpUv : TEXCOORD0;
    // Supply UV values after the original vertex attribute format decoding.
    // Original WaterVertex stores these as packed 32-bit values, not float2s.
    float2 materialUv : TEXCOORD1;
    float2 detailUv : TEXCOORD2;
};

struct LegacyPixelInput
{
    float4 position : SV_POSITION;
    float4 litWater : COLOR0;
    float4 encodedEye : COLOR1;
    float4 tex0 : TEXCOORD0;
    float4 tex1 : TEXCOORD1;
    float4 tex2 : TEXCOORD2;
    float4 tex3 : TEXCOORD3;
    float fogAmount : TEXCOORD4;
};

float3 LegacyWorldDirection(float3 direction)
{
    return mul(float4(direction, 0.0), legacyWorld).xyz;
}

LegacyPixelInput LegacyPositionAndFog(LegacyVertexInput input)
{
    LegacyPixelInput output = (LegacyPixelInput)0;
    float3 worldPosition = mul(float4(input.position, 1.0), legacyWorld).xyz;
    output.position = mul(float4(worldPosition, 1.0), legacyViewProjection);
    // NV2A applies its viewport scale/bias in the vertex program. D3D11
    // rasterization handles that conversion; preserve homogeneous clip output.
    // NV2A interpolates raw fog; its pixel stage clamps the interpolated result.
    // Clamping here changes triangles whose vertices cross either fog endpoint.
    output.fogAmount = output.position.w * legacyFogParameters.x
                     - legacyFogParameters.y;
    return output;
}

LegacyPixelInput LegacyBaseVS(LegacyVertexInput input)
{
    LegacyPixelInput output = LegacyPositionAndFog(input);
    float3 worldPosition = mul(float4(input.position, 1.0), legacyWorld).xyz;
    float3 eyeDelta = legacyCameraPosition.xyz - worldPosition;
    // Original programs require nondegenerate transforms and nonzero eyeDelta.
    float inverseEyeLength = rsqrt(dot(eyeDelta, eyeDelta));
    float3 eye = eyeDelta * inverseEyeLength;

    float3 first = normalize(LegacyWorldDirection(float3(1.0, input.normal.x, 0.0)));
    float3 second = normalize(LegacyWorldDirection(float3(0.0, input.normal.z, 1.0)));
    float3 surfaceNormal = normalize(cross(second, first));

    // NV2A color outputs clamp before interpolation, unlike fog/texture outputs.
    output.litWater = saturate(float4(dot(surfaceNormal, float3(.707, .707, .707))
                                     * legacyWaterColor.rgb, legacyWaterColor.a));
    // RGB is the encoded eye vector consumed by the combiner. The unused
    // fourth channel follows the program's rotated temporary-register write.
    output.encodedEye = saturate(.5 * float4(eye, inverseEyeLength * inverseEyeLength) + .5);
    output.tex0 = float4(input.bumpUv, 0.0, 1.0);
    output.tex1 = float4(first * .125, eye.x);
    output.tex2 = float4(surfaceNormal, eye.y);
    output.tex3 = float4(second * .125, eye.z);
    return output;
}

LegacyPixelInput LegacyOverlayVS(LegacyVertexInput input)
{
    LegacyPixelInput output = LegacyPositionAndFog(input);
    output.tex0 = float4(input.materialUv * legacyScroll1.w + legacyScroll1.xy, 0.0, 1.0);
    output.tex1 = float4(input.detailUv * legacyScroll2.w + legacyScroll2.xy, 0.0, 1.0);
    output.tex2 = float4(input.bumpUv, 0.0, 1.0);
    return output;
}
