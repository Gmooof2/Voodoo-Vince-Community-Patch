#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vince { namespace water {

// Width/height are nonzero powers of two, matching Xbox WaterField.
// rowStride counts float elements. dataCount includes inter-row padding.
struct HeightFieldView {
    const float* data;
    size_t dataCount;
    size_t width;
    size_t height;
    size_t rowStride;
};

enum class NormalMapStatus {
    Ok,
    InvalidDimensions,
    InvalidStride,
    InvalidBuffer,
    InvalidVerticalness,
    NonfiniteHeight,
    NonfiniteColor
};

// Clamp finite RGBA to [0,1], multiply by the original 255.1f bias,
// round to nearest-even bytes, and return numeric
// 0xAARRGGBB (BGRA bytes on little-endian machines). Output changes on success.
NormalMapStatus PackLegacyArgb(float red, float green, float blue, float alpha,
                               uint32_t* output);

// Observed Xbox encoding, using wrapped CENTRAL differences:
// n=normalize(left-right, verticalness, previousRow-nextRow);
// RGB=(8*n.x,n.y,8*n.z)*0.5+0.5; A=height*2+0.75.
// Output is tightly packed width*height pixels, in numeric ARGB format above.
// Input/output storage must not overlap. All validation precedes output writes.
// verticalness must be finite and positive. No allocation or exceptions occur.
// The algorithm matches Xbox; its SSE reciprocal-square-root approximation is not emulated.
NormalMapStatus GenerateLegacyNormalMap(HeightFieldView heights, float verticalness,
                                        uint32_t* output, size_t outputCount);

} } // namespace vince::water

