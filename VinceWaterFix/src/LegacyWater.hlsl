// Original-preview water pixel stages, expressed as readable HLSL.
// This file is an integration contract for a host that restores the original
// dynamic bump texture and both draw passes. It cannot replace the Remastered
// pixel shader alone: its inputs and resources deliberately follow Xbox roles.
// Build base: LegacyBasePS / ps_4_1. Build overlay: LegacyOverlayPS / ps_4_1
// with LEGACY_OVERLAY_PASS=1. LEGACY_OVERLAY_BLEND selects the decoded material
// variant below. These combine two material textures before water lighting;
// they do not select the separate framebuffer blending state of the draw.

cbuffer LegacyPixelConstants : register(b0)
{
    float4 legacyFogColor;
    float4 baseConstant0;    // Original PSconst0; embedded default alpha = 1.
    float4 baseConstant1;    // Original PSconst1; host uploads material reflectivity in alpha.
    float4 overlayLight;    // Original stage1 C0 / PSconst1: (.707,.707,.707,.85).
    float4 overlayAmbient;  // Original stage2 C0 / PSconst0; embedded default (0,0,.6,1).
};

struct LegacyPixelInput
{
    float4 position : SV_POSITION;
    float4 litWater : COLOR0; // Original WaterShader1 oD0: Lambert*tint, tint alpha.
    float4 encodedEye : COLOR1; // .5*unit(eye-worldPosition)+.5, as original oD1.
    float4 tex0 : TEXCOORD0;
    float4 tex1 : TEXCOORD1;
    float4 tex2 : TEXCOORD2;
    float4 tex3 : TEXCOORD3;
    float fogAmount : TEXCOORD4; // Original interpolated fog register, clamped by host.
};

float3 SignedRegister(float3 value) { return clamp(value, -1.0, 1.0); }
float UnsignedInput(float value) { return max(value, 0.0); }

float4 ApplyOriginalFinalCombiner(float4 color, float fogAmount)
{
    // Final combiner inputs ABCD=13030c00, EFG=00001c80.
    return saturate(float4(lerp(max(color.rgb, 0.0), legacyFogColor.rgb,
                               saturate(fogAmount)), max(color.a, 0.0)));
}

#ifndef LEGACY_OVERLAY_PASS
Texture2D<float4> bumpTexture : register(t0);
TextureCube<float4> normalizationCube : register(t2);
TextureCube<float4> reflectionCube : register(t3);
SamplerState bumpSampler : register(s0);
SamplerState normalizationSampler : register(s2);
SamplerState reflectionSampler : register(s3);

float3 OriginalDotMapping(float3 encoded)
{
    // PSDotMapping=0x111 selects D3D signed biased mapping, not RGB*2-1.
    // Preserve the original unorm8 convention around byte128. Texture filtering
    // and quantization can still differ between D3D11 and NV2A hardware.
    return (encoded * 255.0 - 128.0) / 127.0;
}

float4 LegacyBasePS(LegacyPixelInput input) : SV_TARGET
{
    // Texture modes [1,17,11,12]: PROJECT2D, DOTPRODUCT, DOT_RFLCT_DIFF,
    // DOT_RFLCT_SPEC. The host must supply original oT1/2/3 rows and eye q.
    float4 bump = bumpTexture.Sample(bumpSampler, input.tex0.xy / input.tex0.w);
    float3 mapped = OriginalDotMapping(bump.rgb);
    float3 projectedNormal = float3(dot(input.tex1.xyz, mapped),
                                    dot(input.tex2.xyz, mapped),
                                    dot(input.tex3.xyz, mapped));
    float3 encodedNormal = normalizationCube.Sample(normalizationSampler, projectedNormal).rgb;
    float3 eye = float3(input.tex1.w, input.tex2.w, input.tex3.w);
    float normalSquareLength = max(dot(projectedNormal, projectedNormal), 1.0e-12);
    float3 reflected = 2.0 * projectedNormal * dot(projectedNormal, eye) / normalSquareLength - eye;
    float3 environment = reflectionCube.Sample(reflectionSampler, reflected).rgb;

    // Stages0/1: signed difference, dot, shift right1, copy blue to alpha.
    float3 difference = SignedRegister(2.0 * (encodedNormal - input.encodedEye.rgb));
    float grazing = UnsignedInput(clamp(0.5 * dot(difference, 2.0 * encodedNormal - 1.0), -1.0, 1.0));
    // Stage2 alpha and stage3 RGB/alpha. The C0/C1 API mappings are exposed
    // explicitly so a host can preserve embedded defaults or actual uploads.
    float alpha = clamp(lerp(input.litWater.a, baseConstant0.a, grazing), -1.0, 1.0);
    float reflectedAmount = clamp(lerp(baseConstant1.a, baseConstant0.a, grazing), -1.0, 1.0);
    // Stage4 uses r1.a as an unsigned A input and signed D inversion.
    float3 rgb = SignedRegister(UnsignedInput(reflectedAmount) * environment +
                               (1.0 - UnsignedInput(reflectedAmount)) * input.litWater.rgb);
    return ApplyOriginalFinalCombiner(float4(rgb, alpha), input.fogAmount);
}
#else
Texture2D<float4> materialTexture : register(t0);
Texture2D<float4> detailTexture : register(t1);
Texture2D<float4> bumpTexture : register(t2);
SamplerState materialSampler : register(s0);
SamplerState detailSampler : register(s1);
SamplerState bumpSampler : register(s2);

// These stable IDs are the patch API, not an assumed match to game enum values.
#define LEGACY_BLEND_DEFAULT 0
#define LEGACY_BLEND_ADD 1
#define LEGACY_BLEND_DESATURATE 2
#define LEGACY_BLEND_ILLUMINATE 3
#define LEGACY_BLEND_MULTIPLY 4
#define LEGACY_BLEND_OVER 5
#define LEGACY_BLEND_SATURATE 6
#define LEGACY_BLEND_SUBTRACT 7
#ifndef LEGACY_OVERLAY_BLEND
#define LEGACY_OVERLAY_BLEND LEGACY_BLEND_DEFAULT
#endif

float4 MergeOriginalMaterials(float4 material, float4 detail)
{
    // Each SignedRegister corresponds to an actual intermediate Xbox combiner
    // write. Do not collapse these expressions across a clamp boundary.
#if LEGACY_OVERLAY_BLEND == LEGACY_BLEND_DEFAULT
    return material;
#elif LEGACY_OVERLAY_BLEND == LEGACY_BLEND_ADD
    return float4(SignedRegister(detail.rgb * detail.a + material.rgb), material.a);
#elif LEGACY_OVERLAY_BLEND == LEGACY_BLEND_SUBTRACT
    return float4(SignedRegister(-detail.rgb * detail.a + material.rgb), material.a);
#elif LEGACY_OVERLAY_BLEND == LEGACY_BLEND_DESATURATE
    float3 weighted = SignedRegister(detail.rgb * detail.a);
    return float4(SignedRegister(-weighted * material.rgb + material.rgb), material.a);
#elif LEGACY_OVERLAY_BLEND == LEGACY_BLEND_SATURATE
    float3 weighted = SignedRegister(detail.rgb * detail.a);
    return float4(SignedRegister(weighted * material.rgb + material.rgb), material.a);
#elif LEGACY_OVERLAY_BLEND == LEGACY_BLEND_ILLUMINATE
    float3 weighted = SignedRegister(2.0 * detail.rgb * detail.a);
    return float4(SignedRegister(material.rgb * weighted + material.rgb * (1.0 - detail.a)), material.a);
#elif LEGACY_OVERLAY_BLEND == LEGACY_BLEND_MULTIPLY
    float3 multiplier = SignedRegister(detail.rgb * detail.a + (1.0 - detail.a));
    return float4(SignedRegister(multiplier * material.rgb), material.a);
#elif LEGACY_OVERLAY_BLEND == LEGACY_BLEND_OVER
    float3 rgb = SignedRegister(detail.a * detail.rgb + material.rgb * (1.0 - detail.a));
    float remaining = clamp((1.0 - material.a) * (1.0 - detail.a), -1.0, 1.0);
    return float4(rgb, clamp(1.0 - max(remaining, 0.0), -1.0, 1.0));
#else
#error Unrecognized LEGACY_OVERLAY_BLEND
#endif
}

float4 LegacyOverlayPS(LegacyPixelInput input) : SV_TARGET
{
    // Default WaterPS2 has texture modes [1,0,1,0]. The seven named variants
    // use [1,1,1,0], sampling both materials before the shared bump lighting.
    float4 material = materialTexture.Sample(materialSampler, input.tex0.xy / input.tex0.w);
#if LEGACY_OVERLAY_BLEND != LEGACY_BLEND_DEFAULT
    float4 detail = detailTexture.Sample(detailSampler, input.tex1.xy / input.tex1.w);
    material = MergeOriginalMaterials(material, detail);
#endif
    float3 bump = bumpTexture.Sample(bumpSampler, input.tex2.xy / input.tex2.w).rgb;
    // Stage0 copies material. Stage1 dot/blue-to-alpha computes lighting.
    float light = clamp(dot(overlayLight.rgb, 2.0 * bump - 1.0), -1.0, 1.0);
    // Stage2 calculates RGB and alpha; stage3 consumes only its alpha/blue.
    float shade = clamp(UnsignedInput(light) * overlayAmbient.a +
                        overlayAmbient.b * (1.0 - UnsignedInput(light)), -1.0, 1.0);
    float3 rgb = SignedRegister(material.rgb * shade);
    return ApplyOriginalFinalCombiner(float4(rgb, material.a), input.fogAmount);
}
#endif
