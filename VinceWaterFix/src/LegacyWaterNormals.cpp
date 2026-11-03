#include "LegacyWaterNormals.h"

#if !defined(__clang__) && !defined(__GNUC__)
#include <math.h>
#endif

namespace vince { namespace water {
namespace {

bool IsFinite(float value) {
    return value >= -3.402823466e+38f && value <= 3.402823466e+38f;
}

bool IsPowerOfTwo(size_t value) {
    return value && !(value & (value - 1));
}

uint32_t ToUnorm8(double value) {
    if (value <= 0.0) return 0;
    if (value >= 1.0) return 255;
    // Xbox Vector4::GetARGB uses a slightly biased multiplier, not 255.
    // Preserve its float rounding before integer conversion.
    const float scaled = static_cast<float>(value) * 255.1f;
    const uint32_t integral = static_cast<uint32_t>(scaled);
    const double fraction = scaled - integral;
    return integral + (fraction > 0.5 || (fraction == 0.5 && (integral & 1)));
}

uint32_t Pack(double red, double green, double blue, double alpha) {
    return (ToUnorm8(alpha) << 24) | (ToUnorm8(red) << 16)
         | (ToUnorm8(green) << 8) | ToUnorm8(blue);
}

NormalMapStatus Validate(HeightFieldView h, float verticalness,
                         const uint32_t* output, size_t outputCount) {
    const size_t max = static_cast<size_t>(-1);
    if (!IsPowerOfTwo(h.width) || !IsPowerOfTwo(h.height))
        return NormalMapStatus::InvalidDimensions;
    if (h.rowStride < h.width || (h.height - 1) > (max - h.width) / h.rowStride)
        return NormalMapStatus::InvalidStride;
    const size_t required = (h.height - 1) * h.rowStride + h.width;
    if (required > max / sizeof(float) || !h.data || h.dataCount < required)
        return NormalMapStatus::InvalidBuffer;
    if (h.height > max / h.width || h.width * h.height > max / sizeof(uint32_t)
        || !output || outputCount < h.width * h.height)
        return NormalMapStatus::InvalidBuffer;
    if (!IsFinite(verticalness) || verticalness <= 0.0f)
        return NormalMapStatus::InvalidVerticalness;
    for (size_t y = 0; y < h.height; ++y)
        for (size_t x = 0; x < h.width; ++x)
            if (!IsFinite(h.data[y * h.rowStride + x]))
                return NormalMapStatus::NonfiniteHeight;
    return NormalMapStatus::Ok;
}

} // namespace

NormalMapStatus PackLegacyArgb(float red, float green, float blue, float alpha,
                               uint32_t* output) {
    if (!output) return NormalMapStatus::InvalidBuffer;
    if (!IsFinite(red) || !IsFinite(green) || !IsFinite(blue) || !IsFinite(alpha))
        return NormalMapStatus::NonfiniteColor;
    *output = Pack(red, green, blue, alpha);
    return NormalMapStatus::Ok;
}

NormalMapStatus GenerateLegacyNormalMap(HeightFieldView h, float verticalness,
                                        uint32_t* output, size_t outputCount) {
    const NormalMapStatus status = Validate(h, verticalness, output, outputCount);
    if (status != NormalMapStatus::Ok) return status;
    const size_t xMask = h.width - 1;
    const size_t yMask = h.height - 1;
    for (size_t y = 0; y < h.height; ++y) {
        const size_t row = y * h.rowStride;
        const size_t above = ((y - 1) & yMask) * h.rowStride;
        const size_t below = ((y + 1) & yMask) * h.rowStride;
        for (size_t x = 0; x < h.width; ++x) {
            // Promote before subtracting: finite FLT_MAX inputs remain safe.
            const double dx = double(h.data[row + ((x - 1) & xMask)])
                            - double(h.data[row + ((x + 1) & xMask)]);
            const double dz = double(h.data[above + x]) - double(h.data[below + x]);
            const double up = verticalness;
            const double lengthSquared = dx * dx + up * up + dz * dz;
#if defined(__clang__) || defined(__GNUC__)
            const double inverseLength = 1.0 / __builtin_sqrt(lengthSquared);
#else
            const double inverseLength = 1.0 / sqrt(lengthSquared);
#endif
            output[y * h.width + x] = Pack(4.0 * dx * inverseLength + 0.5,
                                          0.5 * up * inverseLength + 0.5,
                                          4.0 * dz * inverseLength + 0.5,
                                          double(h.data[row + x]) * 2.0 + 0.75);
        }
    }
    return NormalMapStatus::Ok;
}

} } // namespace vince::water
