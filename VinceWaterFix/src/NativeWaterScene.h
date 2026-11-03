#pragma once
#include "NativeWaterMemory.h"
#include "LegacyWaterContract.h"

namespace vince {
namespace water {

// Steam1.14.2.0 only. The loader must identify the supported executable before
// calling these readers. Every native pointer is read as32-bit, including in
// tests running on a64-bit process; imageBase supplies ASLR relocation.
enum class NativeSceneStatus {
    Ok,
    InvalidArgument,
    UnreadableMemory,
    InvalidValue,
    MissingFogConstants
};

struct NativeWaterSceneInput {
    uintptr_t worldMatrix; // The original WaterField::Draw argument,64 readable bytes.
    uintptr_t waterField;  // Optional: current new procedural field; color at+0x18.
    uintptr_t waterRegion; // Optional: validated authored region object, not the water node.
};

struct NativeWaterScene {
    uintptr_t game, renderer, camera, timer;
    uintptr_t levelObject; // Optional current *(Game+14)->0; pointer alone is NOT a
                           // generation ID.
    bool hasAreaName;
    char areaName[128]; // Read-only copy of the current config-area std::string.
    WaterMatrix world, view, projection, viewProjection;
    WaterVector cameraPosition;
    WaterVector fogColor;
    float fogNear, fogFar;
    float frameDelta; // Timer+0; same delta tested by native water worker49b720.
    uint32_t frameNumber; // Game+60; increments before each native timer update.
    double clock18; // Timer+18: accumulated scaled/clamped gameplay time; stops on pause.
    double clock20; // Timer+20: wall time since timer origin; continues on pause.
    bool hasNativeFieldColor;
    WaterVector nativeFieldColor; // Remastered authored color; NOT the restored original palette.
    bool hasRegionGridSettings;
    float originalDensity;      // Authored WaterRegion+0x54, when region explicitly supplied.
    uint32_t originalFieldSize; // WaterRegion+0x58, NOT procedural grid+8c/+90.
};

NativeSceneStatus ReadNativeWaterScene(const NativeMemory& memory,
                                       const NativeWaterSceneInput& input,
                                       NativeWaterScene* output);

// Explicit conversion after authoring/material selection. These values are
// caller-owned: original tint/reflectivity/UV offsets must not come from stale
// native constants left by a previous surface. Original turbulence/speed/
// elasticity/verticalness also require authored settings; the new procedural
// water object does not retain proven corresponding members.
struct LegacyWaterSceneAppearance {
    WaterVector areaColor;
    float materialReflectivity;
    WaterVector scroll1; // xy=already-computed offsets; w=16 as original overlay.
    WaterVector scroll2;
};

bool MakeLegacyWaterSceneConstants(const NativeWaterScene& scene,
                                   const LegacyWaterSceneAppearance& appearance,
                                   LegacyVertexConstants* vertex, LegacyPixelConstants* pixel);

} // namespace water
} // namespace vince
