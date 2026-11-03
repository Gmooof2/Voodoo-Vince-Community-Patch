#include "NativeWaterMaterials.h"

namespace vince {
namespace water {
namespace {
constexpr uint32_t kTextureResolverVA = 0x4613f0;

uint32_t U32(const unsigned char* bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
           (uint32_t(bytes[3]) << 24);
}

bool Finite(float value) { return value >= -3.402823466e+38f && value <= 3.402823466e+38f; }

bool Resolve(const NativeMemory& memory, uint32_t resource, NativeResolveTexture callback,
             void* user, uintptr_t& view) {
    view = 0;
    if (!resource)
        return true;
    return callback(user, memory.Address(kTextureResolverVA), resource, &view) && view;
}
} // namespace

NativeGlobalsStatus ReadNativeWaterGlobals(const NativeMemory& memory, NativeWaterGlobals* output) {
    if (!memory.read || !output)
        return NativeGlobalsStatus::InvalidArgument;
    NativeWaterGlobals globals = {};
    if (!memory.Value(memory.Address(0xb653f4), globals.game))
        return NativeGlobalsStatus::UnreadableMemory;
    if (!globals.game)
        return NativeGlobalsStatus::NotReady;
    if (!memory.Value(uintptr_t(globals.game) + 0x2c, globals.materialManager))
        return NativeGlobalsStatus::UnreadableMemory;
    if (!globals.materialManager)
        return NativeGlobalsStatus::NotReady;
    if (!memory.Value(uintptr_t(globals.materialManager) + 0x4c,
                      globals.defaultEnvironmentResource))
        return NativeGlobalsStatus::UnreadableMemory;
    if (!globals.defaultEnvironmentResource)
        return NativeGlobalsStatus::NotReady;

    unsigned char stringHeader[24] = {};
    const uintptr_t stringAddress = memory.Address(0xb6670c);
    if (!memory.Read(stringAddress, stringHeader, sizeof(stringHeader)))
        return NativeGlobalsStatus::UnreadableMemory;
    const uint32_t length = U32(stringHeader + 0x10);
    const uint32_t capacity = U32(stringHeader + 0x14);
    if (!length)
        return NativeGlobalsStatus::NotReady;
    if (length >= sizeof(globals.area) || length > capacity)
        return NativeGlobalsStatus::InvalidAreaName;
    const uintptr_t characters = capacity < 16 ? stringAddress : U32(stringHeader);
    if (!memory.Read(characters, globals.area, length + 1))
        return NativeGlobalsStatus::UnreadableMemory;
    if (globals.area[length] != '\0')
        return NativeGlobalsStatus::InvalidAreaName;
    for (uint32_t index = 0; index < length; ++index)
        if (globals.area[index] < 0x20 || globals.area[index] > 0x7e)
            return NativeGlobalsStatus::InvalidAreaName;
    globals.areaLength = length;
    globals.lifetimeToken =
        (uint64_t(globals.materialManager) << 32) | globals.defaultEnvironmentResource;
    *output = globals;
    return NativeGlobalsStatus::Ok;
}

NativeMaterialStatus ResolveNativeWaterMaterial(const NativeMemory& memory,
                                                uint32_t materialResource, uint32_t materialManager,
                                                NativeResolveTexture resolveTexture,
                                                void* resolveUser, NativeWaterMaterial* output,
                                                uint32_t defaultEnvironmentOverride) {
    if (!memory.read || !resolveTexture || !output)
        return NativeMaterialStatus::InvalidArgument;
    NativeWaterMaterial material = {};
    material.present = materialResource != 0;
    if (materialResource) {
        uint32_t textures[7] = {};
        float properties[26] = {};
        if (!memory.Read(uintptr_t(materialResource) + 0x24, textures, sizeof(textures)) ||
            !memory.Value(uintptr_t(materialResource) + 0x40, material.detailType) ||
            !memory.Read(uintptr_t(materialResource) + 0x44, properties, sizeof(properties)))
            return NativeMaterialStatus::UnreadableMemory;
        for (float value : properties)
            if (!Finite(value))
                return NativeMaterialStatus::InvalidMaterialValue;
        material.baseResource = textures[0];
        material.detailResource = textures[1];
        material.environmentResource = textures[2];
        material.reflectivity = properties[0];
        material.specularPower = properties[1];
        for (unsigned channel = 0; channel < 4; ++channel) {
            material.diffuseColor[channel] = properties[2 + channel];
            material.ambientColor[channel] = properties[6 + channel];
            material.specularColor[channel] = properties[10 + channel];
            material.emissiveColor[channel] = properties[14 + channel];
            material.reflectiveColor[channel] = properties[18 + channel];
        }
        for (unsigned channel = 0; channel < 2; ++channel) {
            material.uvRate1[channel] = properties[22 + channel];
            material.uvRate2[channel] = properties[24 + channel];
        }
    } else {
        // WaterField::Draw's explicit null-material branch, not guessed class
        // defaults. It uses these specular/emissive values and no overlay data.
        material.reflectivity = 0.5f;
        material.specularPower = 20.0f;
        material.specularColor[0] = material.specularColor[1] = material.specularColor[2] = 0.5f;
        material.specularColor[3] = 1.0f;
    }
    if (!material.environmentResource) {
        material.usedDefaultEnvironment = true;
        material.environmentResource = defaultEnvironmentOverride;
        if (!material.environmentResource &&
            (!materialManager ||
             !memory.Value(uintptr_t(materialManager) + 0x4c, material.environmentResource)))
            return NativeMaterialStatus::UnreadableMemory;
    }
    if (!material.environmentResource)
        return NativeMaterialStatus::MissingEnvironment;
    if (!Resolve(memory, material.baseResource, resolveTexture, resolveUser, material.baseView) ||
        !Resolve(memory, material.detailResource, resolveTexture, resolveUser,
                 material.detailView) ||
        !Resolve(memory, material.environmentResource, resolveTexture, resolveUser,
                 material.environmentView))
        return NativeMaterialStatus::TextureResolutionFailed;
    *output = material;
    return NativeMaterialStatus::Ok;
}

} // namespace water
} // namespace vince
