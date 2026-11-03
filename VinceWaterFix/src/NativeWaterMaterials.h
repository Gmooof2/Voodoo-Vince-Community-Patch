#pragma once
#include "NativeWaterMemory.h"

namespace vince {
namespace water {

// Host executes the native __thiscall TextureResource resolver at functionAddress
// with textureResource as ECX, returning its borrowed ID3D11ShaderResourceView*.
// The host owns fault containment, render-thread execution and build validation.
// The callback must not release the resource/view. Retain an AddRef separately
// if using the returned view beyond the lifetime of its current native resource.
using NativeResolveTexture = bool (*)(void* user, uintptr_t functionAddress,
                                      uint32_t textureResource, uintptr_t* shaderResourceView);

struct NativeWaterMaterial {
    bool present;
    uint32_t detailType; // Native material enum, convert using LegacyWaterContract.
    uint32_t baseResource;
    uint32_t detailResource;
    uint32_t environmentResource;
    bool usedDefaultEnvironment;
    float reflectivity;
    float specularPower;
    float diffuseColor[4];
    float ambientColor[4];
    float specularColor[4];
    float emissiveColor[4];
    float reflectiveColor[4];
    float uvRate1[2]; // Authored rates, not accumulated offsets or shader scales.
    float uvRate2[2];
    uintptr_t baseView;
    uintptr_t detailView;
    uintptr_t environmentView;
};

enum class NativeMaterialStatus {
    Ok,
    InvalidArgument,
    UnreadableMemory,
    InvalidMaterialValue,
    MissingEnvironment,
    TextureResolutionFailed
};

struct NativeWaterGlobals {
    uint32_t game;
    uint32_t materialManager;
    uint32_t defaultEnvironmentResource;
    uint32_t areaLength;
    char area[128];         // Null-terminated bounded copy, original case preserved.
    uint64_t lifetimeToken; // Manager/capture pointer identity, not a unique epoch.
};

enum class NativeGlobalsStatus { Ok, InvalidArgument, NotReady, UnreadableMemory, InvalidAreaName };

// Copies current config area (global MSVC x86 std::string at0xb6670c), then
// Game->MaterialManager->default captured environment. No native calls/writes.
// Use the capture pointer token WITH region/field identity to invalidate cached
// resources. Pointer reuse can repeat tokens; a host must also reset on unload
// or its own level-load generation. No pointer-derived token is a unique epoch.
// All reads must occur at a host-selected point where level loading is stable.
// Output remains unchanged on failure. NotReady covers menus/loading/null data.
NativeGlobalsStatus ReadNativeWaterGlobals(const NativeMemory& memory, NativeWaterGlobals* output);

// Build-specific adapter for the inspected 32-bit Steam executable. A native
// ResourceHandle<MaterialResource> is one 32-bit resource pointer, NOT another
// wrapper. Pass that pointer as materialResource (zero permits native defaults).
// materialManager is Game singleton+0x2c; its +0x4c is the level-captured cube.
// Neither game memory nor native reference counts are changed by this adapter.
// Resolve callbacks may enter native code; the host controls their safety.
// Output remains unchanged on failure. On success, views are borrowed.
// defaultEnvironmentOverride, when nonzero, replaces only the MaterialManager
// +0x4c fallback (e.g. a retail-size level capture); explicit envmaps stay.
NativeMaterialStatus ResolveNativeWaterMaterial(const NativeMemory& memory,
                                                uint32_t materialResource, uint32_t materialManager,
                                                NativeResolveTexture resolveTexture,
                                                void* resolveUser, NativeWaterMaterial* output,
                                                uint32_t defaultEnvironmentOverride = 0);

} // namespace water
} // namespace vince
