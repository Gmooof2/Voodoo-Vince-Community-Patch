#pragma once

#include <stddef.h>

namespace vince { namespace water {

// These are D3D11 integration structures, not casts over either game's private
// objects. In particular the original vertex UVs require format decoding.
struct DecodedWaterVertex {
    float position[3];
    float normal[3];
    float bumpUv[2];
    float materialUv[2];
    float detailUv[2];
};

struct alignas(16) WaterVector {
    float x, y, z, w;
};

struct alignas(16) WaterMatrix {
    float row[4][4];
};

// LegacyWaterVS.hlsl b1. Row-vector convention: position * world * viewProj.
struct alignas(16) LegacyVertexConstants {
    WaterMatrix world;
    WaterMatrix viewProjection;
    WaterVector cameraPosition;
    WaterVector fogParameters; // x=slope,y=offset; fog=clip.w*x-y
    WaterVector waterColor;
    WaterVector scroll1; // (offsetU,offsetV,0,16)
    WaterVector scroll2;
};

// LegacyWater.hlsl b0. These belong to the full two-pass reconstruction, not
// to RemasterLegacyBasePS.hlsl, which preserves the native large b0 layout.
struct alignas(16) LegacyPixelConstants {
    WaterVector fogColor;
    WaterVector baseConstant0;
    WaterVector baseConstant1; // .w=material reflectivity
    WaterVector overlayLight;
    WaterVector overlayAmbient;
};

// Proven original draw predicate. Numerical values belong to the game's material
// format; they must not be confused with D3D11 blend-state enumeration values.
constexpr bool ShouldRenderLegacyOverlay(int materialDetailType) {
    return materialDetailType != 0 &&
        (materialDetailType < 13 || materialDetailType > 15);
}

// Patch shader IDs match LEGACY_OVERLAY_BLEND in LegacyWater.hlsl. The original
// material enumeration has gaps and a different order; unsupported inputs must
// not silently select the default shader.
enum class LegacyOverlayVariant {
    Unsupported = -1,
    Default = 0,
    Add = 1,
    Desaturate = 2,
    Illuminate = 3,
    Multiply = 4,
    Over = 5,
    Saturate = 6,
    Subtract = 7
};

constexpr LegacyOverlayVariant OverlayVariantForMaterial(int materialDetailType) {
    switch (materialDetailType) {
    case 0: return LegacyOverlayVariant::Default;
    case 1: return LegacyOverlayVariant::Over;
    case 4: return LegacyOverlayVariant::Add;
    case 5: return LegacyOverlayVariant::Subtract;
    case 6: return LegacyOverlayVariant::Multiply;
    case 10: return LegacyOverlayVariant::Saturate;
    case 11: return LegacyOverlayVariant::Desaturate;
    case 12: return LegacyOverlayVariant::Illuminate;
    default: return LegacyOverlayVariant::Unsupported;
    }
}

static_assert(sizeof(float) == 4, "Water contracts require 32-bit floats");
static_assert(sizeof(DecodedWaterVertex) == 48, "Decoded vertex layout mismatch");
static_assert(offsetof(DecodedWaterVertex, normal) == 12, "Normal offset mismatch");
static_assert(offsetof(DecodedWaterVertex, bumpUv) == 24, "Bump UV offset mismatch");
static_assert(offsetof(DecodedWaterVertex, materialUv) == 32, "Material UV offset mismatch");
static_assert(offsetof(DecodedWaterVertex, detailUv) == 40, "Detail UV offset mismatch");
static_assert(sizeof(LegacyVertexConstants) == 208, "VS constant size mismatch");
static_assert(offsetof(LegacyVertexConstants, cameraPosition) == 128, "Camera constant mismatch");
static_assert(offsetof(LegacyVertexConstants, scroll2) == 192, "Scroll constant mismatch");
static_assert(sizeof(LegacyPixelConstants) == 80, "PS constant size mismatch");
static_assert(offsetof(LegacyPixelConstants, overlayAmbient) == 64, "Ambient constant mismatch");

}} // namespace vince::water
