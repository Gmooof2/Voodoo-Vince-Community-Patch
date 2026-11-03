// A base-shading reconstruction for the Remastered water draw.
// Derived from the supplied Xbox preview's five register-combiner stages.
// This drop-in uses Remastered geometry: it does not restore the Xbox bump
// simulation, normalization cube quantization, or separate material overlay.
// See research/remaster-shaders.md and research/original-water-combiners.md.

cbuffer NativePixelConstants : register(b0)
{
    float4 eyePosition       : packoffset(c32);
    float4 material          : packoffset(c33); // x: engine material reflectivity
    float4 fogColor          : packoffset(c44);
    float4 fogDistances      : packoffset(c45); // z: near, w: far
    float4 waterColor        : packoffset(c51); // host restores per-area Xbox RGBA
};

TextureCube<float4> environmentMap : register(t3);
SamplerState environmentSampler : register(s0);

// All semantics match the shipped WaterVS output. Retaining the complete
// signature makes the contract clear even though this PS uses only three fields.
struct NativeWaterPixel
{
    float4 position : SV_POSITION;
    float4 color : COLOR0;
    float3 eyeDirection : COLOR1;
    float3 worldPosition : TEXCOORD3;
    float3 normal : TEXCOORD4;
    float3 tangent : TEXCOORD5;
    float3 bitangent : TEXCOORD6;
    float3 viewPosition : TEXCOORD7;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float2 uv2 : TEXCOORD2;
};

float3 SafeUnitVector(float3 value, float3 fallback)
{
    float squareLength = dot(value, value);
    return squareLength > 1.0e-12 ? value * rsqrt(squareLength) : fallback;
}

float4 LegacyBasePS(NativeWaterPixel input) : SV_TARGET
{
    float3 normal = SafeUnitVector(input.normal, float3(0.0, 1.0, 0.0));
    float3 eye = SafeUnitVector(input.eyeDirection, normal);

    // Original WaterShader1 vertex lighting, evaluated here because the native
    // WaterVS outputs constant color instead of the original lit vertex color.
    // Evaluating after interpolation is a documented geometry-path difference.
    const float3 originalLight = float3(0.707, 0.707, 0.707);
    float4 litWater = float4(saturate(waterColor.rgb * dot(normal, originalLight)),
                            saturate(waterColor.a));

    // Preserve the Xbox stage0 signed register clamp and stage1 divide by two.
    // Dropping these gives a different Fresnel curve. The native unit normal
    // substitutes for the Xbox normalization cube's decoded 8-bit sample.
    float3 difference = clamp(normal - eye, -1.0, 1.0);
    float grazing = saturate(0.5 * dot(difference, normal));
    float reflectionWeight = lerp(saturate(material.x), 1.0, grazing);
    float3 reflection = environmentMap.Sample(environmentSampler, reflect(-eye, normal)).rgb;
    float4 result = float4(lerp(litWater.rgb, reflection, reflectionWeight),
                           lerp(litWater.a, 1.0, grazing));

    // Retain the native distance-fog contract. Original Xbox fog came through
    // the vertex fog register, so this is intentionally a native integration step.
    float distanceToEye = length(eyePosition.xyz - input.worldPosition);
    float range = max(fogDistances.w - fogDistances.z, 1.0e-6);
    float visibility = saturate((fogDistances.w - distanceToEye) / range);
    result.rgb = lerp(fogColor.rgb, result.rgb, visibility);
    return saturate(result);
}
