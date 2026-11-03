#include "LegacyWaterCube.h"
#include "LegacyWaterNormals.h"

namespace vince { namespace water {
namespace {

struct Direction { float x, y, z; };

// Literal face bases from TextureManager::TextureManager, preview default.exe
// 0x4b7573..0x4b77e8. Keep the unusual +/-Y orientation: replacing these with a
// conventional normalized-cube formula changes the lookup's original behavior.
constexpr Direction kFaceAxis[6] = {
    {1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
    {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
};
constexpr Direction kHorizontal[6] = {
    {0, 0, -1}, {0, 0, 1}, {0, 0, -1},
    {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}
};
constexpr Direction kVertical[6] = {
    {0, -1, 0}, {0, -1, 0}, {1, 0, 0},
    {-1, 0, 0}, {0, -1, 0}, {0, -1, 0}
};

float Encode(float axis, float horizontal, float vertical, float x, float y) {
    // Original x87 computes 2*i/31-1 using the stored float 2/31 constant.
    // Intermediate vector components are stored as floats before color packing.
    const float direction = axis + y * vertical + x * horizontal;
    return 0.5f * direction + 0.5f;
}

} // namespace

bool GenerateLegacyDirectionCube(uint32_t* argbPixels, unsigned capacity) {
    if (!argbPixels || capacity < kLegacyDirectionCubePixels) return false;
    const float coordinateStep = 2.0f / 31.0f;
    for (unsigned face = 0; face < kLegacyDirectionCubeFaces; ++face) {
        for (unsigned row = 0; row < kLegacyDirectionCubeSize; ++row) {
            const float y = static_cast<float>(row) * coordinateStep - 1.0f;
            for (unsigned column = 0; column < kLegacyDirectionCubeSize; ++column) {
                const float x = static_cast<float>(column) * coordinateStep - 1.0f;
                const Direction a = kFaceAxis[face];
                const Direction h = kHorizontal[face];
                const Direction v = kVertical[face];
                const unsigned index = (face * kLegacyDirectionCubeSize + row) *
                    kLegacyDirectionCubeSize + column;
                // Uses the same proven 255.1f/nearest-even packing as the
                // generated bump texture; these inputs are finite by construction.
                PackLegacyArgb(Encode(a.x, h.x, v.x, x, y),
                               Encode(a.y, h.y, v.y, x, y),
                               Encode(a.z, h.z, v.z, x, y), 1.0f, &argbPixels[index]);
            }
        }
    }
    return true;
}

}} // namespace vince::water
