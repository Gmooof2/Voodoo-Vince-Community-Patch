#pragma once

#include <stdint.h>

namespace vince { namespace water {

inline constexpr unsigned kLegacyDirectionCubeSize = 32;
inline constexpr unsigned kLegacyDirectionCubeFaces = 6;
inline constexpr unsigned kLegacyDirectionCubePixels = 6 * 32 * 32;

// Reconstruct TextureManager's runtime-created Xbox preview NormCubemap.
// This is a direction lookup, NOT scene/environment reflection imagery. Despite
// its original name, the constructor does not normalize each texel's direction.
// Faces are +X,-X,+Y,-Y,+Z,-Z; each face contains row-major numeric 0xAARRGGBB.
// Upload as DXGI_FORMAT_B8G8R8A8_UNORM on little-endian Windows (not RGBA8).
// A host should use one mip level; the original allocation has one mip level.
// Returns false without writing if the pointer is null or capacity insufficient.
bool GenerateLegacyDirectionCube(uint32_t* argbPixels, unsigned capacity);

}} // namespace vince::water
