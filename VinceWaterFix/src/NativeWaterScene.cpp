#include "NativeWaterScene.h"
#include <float.h>

namespace vince {
namespace water {
namespace {

bool Finite(float value) { return value >= -FLT_MAX && value <= FLT_MAX; }
bool Finite(double value) { return value >= -DBL_MAX && value <= DBL_MAX; }
bool Finite(WaterVector value) {
    return Finite(value.x) && Finite(value.y) && Finite(value.z) && Finite(value.w);
}
bool Finite(const WaterMatrix& value) {
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 4; ++col)
            if (!Finite(value.row[row][col]))
                return false;
    return true;
}

float PixelConstantChannel(float value) {
    // Xbox D3DSetPixelShaderConstant544650 packs clamped float channels using
    // multiplier255.0 and SSEnearest-even (distinct from GetARGB's255.1 bias).
    if (value < 0)
        value = 0;
    if (value > 1)
        value = 1;
    const float scaled = value * 255.0f;
    unsigned rounded = static_cast<unsigned>(scaled);
    const float fraction = scaled - static_cast<float>(rounded);
    if (fraction > .5f || (fraction == .5f && (rounded & 1u)))
        ++rounded;
    return static_cast<float>(rounded) / 255.0f;
}

template <class T>
bool At(const NativeMemory& memory, uintptr_t base, uintptr_t offset, T& output) {
    return base && offset <= UINTPTR_MAX - base && memory.Value(base + offset, output);
}
bool Pointer(const NativeMemory& memory, uintptr_t base, uintptr_t offset, uintptr_t& output) {
    uint32_t pointer = 0;
    if (!At(memory, base, offset, pointer) || !pointer)
        return false;
    output = pointer;
    return true;
}

WaterMatrix Multiply(const WaterMatrix& a, const WaterMatrix& b) {
    WaterMatrix result = {};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 4; ++col)
            for (unsigned inner = 0; inner < 4; ++inner)
                result.row[row][col] += a.row[row][inner] * b.row[inner][col];
    return result;
}

bool ReadFogMirror(const NativeMemory& memory, uintptr_t renderer, WaterVector& color,
                   WaterVector& distances) {
    // 45f110(stage,registerIndex,source,count): mirror pointer lives atdesc+14,
    //  descriptor byte length at+8. PSstage is2, VSstage0. Mirror includes the
    //  scene fog enable/disable decision already made by native450cf0.
    uintptr_t table = 0, descriptor = 0, bytes = 0;
    uint32_t length = 0;
    return Pointer(memory, renderer, 0x6bc, table) && Pointer(memory, table, 8, descriptor) &&
           At(memory, descriptor, 8, length) && length >= 46 * 16 &&
           Pointer(memory, descriptor, 0x14, bytes) && At(memory, bytes, 44 * 16, color) &&
           At(memory, bytes, 45 * 16, distances);
}

bool ReadAreaName(const NativeMemory& memory, char (&output)[128]) {
    // 46b780 copies the requested area into this24-byte MSVC std::string;
    // 46b7e4 passes it to config/<area>.tdf path construction. Length+10,cap+14;
    //  capacity<16 is inline storage, otherwise its first DWORD is a char pointer.
    const uintptr_t string = memory.Address(0xb6670c);
    uint32_t length = 0, capacity = 0;
    if (!At(memory, string, 0x10, length) || !At(memory, string, 0x14, capacity) || !length ||
        length >= sizeof(output) || capacity < length)
        return false;
    uintptr_t data = string;
    if (capacity >= 16 && !Pointer(memory, string, 0, data))
        return false;
    if (!memory.Read(data, output, size_t(length) + 1) || output[length])
        return false;
    for (uint32_t i = 0; i < length; ++i)
        if (static_cast<unsigned char>(output[i]) < 32 ||
            static_cast<unsigned char>(output[i]) > 126)
            return false;
    return true;
}

} // namespace

NativeSceneStatus ReadNativeWaterScene(const NativeMemory& memory,
                                       const NativeWaterSceneInput& input,
                                       NativeWaterScene* output) {
    if (!output || !memory.read || !memory.imageBase || !input.worldMatrix)
        return NativeSceneStatus::InvalidArgument;
    NativeWaterScene result = {};
    if (!Pointer(memory, memory.Address(0xb653f4), 0, result.game) ||
        !Pointer(memory, memory.Address(0xb65538), 0, result.renderer) ||
        !Pointer(memory, result.game, 0x40, result.camera) ||
        !Pointer(memory, result.game, 8, result.timer) ||
        !At(memory, result.game, 0x60, result.frameNumber) ||
        !memory.Value(input.worldMatrix, result.world) ||
        !At(memory, result.renderer, 0x54, result.view) ||
        !At(memory, result.renderer, 0x94, result.projection) ||
        !At(memory, result.camera, 0x2c, result.cameraPosition.x) ||
        !At(memory, result.camera, 0x30, result.cameraPosition.y) ||
        !At(memory, result.camera, 0x34, result.cameraPosition.z) ||
        !At(memory, result.timer, 0, result.frameDelta) ||
        !At(memory, result.timer, 0x18, result.clock18) ||
        !At(memory, result.timer, 0x20, result.clock20))
        return NativeSceneStatus::UnreadableMemory;
    result.cameraPosition.w = 1;
    uintptr_t levelManager = 0;
    if (Pointer(memory, result.game, 0x14, levelManager))
        Pointer(memory, levelManager, 0, result.levelObject);
    result.hasAreaName = ReadAreaName(memory, result.areaName);
    result.viewProjection = Multiply(result.view, result.projection);
    WaterVector distances = {};
    if (!ReadFogMirror(memory, result.renderer, result.fogColor, distances))
        return NativeSceneStatus::MissingFogConstants;
    result.fogNear = distances.z;
    result.fogFar = distances.w;
    if (input.waterField) {
        if (!At(memory, input.waterField, 0x18, result.nativeFieldColor))
            return NativeSceneStatus::UnreadableMemory;
        result.hasNativeFieldColor = true;
    }
    if (input.waterRegion) {
        if (!At(memory, input.waterRegion, 0x54, result.originalDensity) ||
            !At(memory, input.waterRegion, 0x58, result.originalFieldSize))
            return NativeSceneStatus::UnreadableMemory;
        if (!Finite(result.originalDensity) || result.originalDensity <= 0 ||
            !result.originalFieldSize ||
            (result.originalFieldSize & (result.originalFieldSize - 1)))
            return NativeSceneStatus::InvalidValue;
        result.hasRegionGridSettings = true;
    }
    if (!Finite(result.world) || !Finite(result.view) || !Finite(result.projection) ||
        !Finite(result.viewProjection) || !Finite(result.cameraPosition) ||
        !Finite(result.frameDelta) || !Finite(result.clock18) || !Finite(result.clock20) ||
        !Finite(result.fogColor) || !Finite(result.fogNear) || !Finite(result.fogFar) ||
        (result.hasNativeFieldColor && !Finite(result.nativeFieldColor)))
        return NativeSceneStatus::InvalidValue;
    *output = result;
    return NativeSceneStatus::Ok;
}

bool MakeLegacyWaterSceneConstants(const NativeWaterScene& scene,
                                   const LegacyWaterSceneAppearance& appearance,
                                   LegacyVertexConstants* vertex, LegacyPixelConstants* pixel) {
    if (!vertex || !pixel || !Finite(scene.world) || !Finite(scene.viewProjection) ||
        !Finite(scene.cameraPosition) || !Finite(scene.fogColor) || !Finite(scene.fogNear) ||
        !Finite(scene.fogFar) || !Finite(appearance.areaColor) ||
        !Finite(appearance.materialReflectivity) || !Finite(appearance.scroll1) ||
        !Finite(appearance.scroll2))
        return false;
    LegacyVertexConstants v = {};
    LegacyPixelConstants p = {};
    v.world = scene.world;
    v.viewProjection = scene.viewProjection;
    v.cameraPosition = scene.cameraPosition;
    v.waterColor = appearance.areaColor;
    v.scroll1 = appearance.scroll1;
    v.scroll2 = appearance.scroll2;
    const float range = scene.fogFar - scene.fogNear;
    if (!Finite(range))
        return false;
    if (range > 0) {
        v.fogParameters = {1.0f / range, scene.fogNear / range, 0, 0};
        if (!Finite(v.fogParameters))
            return false;
    } // Disabled/equal fog endpoints produce zero fog, never a NaN division.
    p.fogColor = scene.fogColor;
    p.baseConstant0 = {0, 0, 0, 1};
    p.baseConstant1 = {0, 0, 0, PixelConstantChannel(appearance.materialReflectivity)};
    p.overlayLight = {PixelConstantChannel(.707f), PixelConstantChannel(.707f),
                      PixelConstantChannel(.707f), PixelConstantChannel(.85f)};
    p.overlayAmbient = {0, 0, .6f, 1};
    *vertex = v;
    *pixel = p;
    return true;
}

} // namespace water
} // namespace vince
