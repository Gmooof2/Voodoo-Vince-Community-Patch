#include "LegacyWaterVertexGrid.h"

namespace vince { namespace water {
namespace {
const double MaxFloat = 3.4028234663852886e+38;
bool Finite(float x) { return x >= -MaxFloat && x <= MaxFloat; }
bool PowerOfTwo(size_t x) { return x && !(x & (x - 1)); }

VertexGridStatus ValidateHeightField(HeightFieldView h) {
    const size_t maximum = static_cast<size_t>(-1);
    if (!PowerOfTwo(h.width) || !PowerOfTwo(h.height)
        || h.width > 0x40000000u || h.height > 0x40000000u)
        return VertexGridStatus::InvalidDimensions;
    if (h.rowStride < h.width || (h.height - 1) > (maximum - h.width) / h.rowStride)
        return VertexGridStatus::InvalidStride;
    const size_t required = (h.height - 1) * h.rowStride + h.width;
    if (required > maximum / sizeof(float) || !h.data || h.dataCount < required)
        return VertexGridStatus::InvalidBuffer;
    for(size_t z=0;z<h.height;++z)
        for(size_t x=0;x<h.width;++x)
            if (!Finite(h.data[z*h.rowStride+x])) return VertexGridStatus::NonfiniteHeight;
    return VertexGridStatus::Ok;
}

VertexGridStatus Sample(HeightFieldView h, LegacyModelPosition source,
    LegacyModelPosition low, float dx, float dz, LegacyVertexGridSample& result) {
    if (!Finite(source.x) || !Finite(source.z)) return VertexGridStatus::InvalidCoordinate;
    const double productX = (double(source.x) - low.x) * dx;
    const double productZ = (double(source.z) - low.z) * dz;
    if (!(productX >= -MaxFloat && productX <= MaxFloat)
        || !(productZ >= -MaxFloat && productZ <= MaxFloat))
        return VertexGridStatus::InvalidCoordinate;
    // Retail Xbox stores each x87 coordinate product to float before the SSE
    // truncating integer conversion (0x150085/0x150096 and 0x1500a6/0x1500ae).
    // Preserve that rounding boundary: it can change the selected field cell.
    const float ix = static_cast<float>(productX);
    const float iz = static_cast<float>(productZ);
    if (!(ix >= -2147483648.0 && ix < 2147483648.0)
        || !(iz >= -2147483648.0 && iz < 2147483648.0))
        return VertexGridStatus::InvalidCoordinate;
    const size_t xMask = h.width - 1;
    const size_t zMask = h.height - 1;
    const size_t x = static_cast<uint32_t>(static_cast<int32_t>(ix)) & xMask;
    const size_t z = static_cast<uint32_t>(static_cast<int32_t>(iz)) & zMask;
    const double y = double(low.y) + h.data[z*h.rowStride+x];
    const double nx = double(h.data[z*h.rowStride+((x-1)&xMask)])
                    - h.data[z*h.rowStride+((x+1)&xMask)];
    const double nz = double(h.data[((z-1)&zMask)*h.rowStride+x])
                    - h.data[((z+1)&zMask)*h.rowStride+x];
    if (!(y >= -MaxFloat && y <= MaxFloat)
        || !(nx >= -MaxFloat && nx <= MaxFloat)
        || !(nz >= -MaxFloat && nz <= MaxFloat))
        return VertexGridStatus::ArithmeticOverflow;
    result = {static_cast<float>(y),static_cast<float>(nx),static_cast<float>(nz)};
    return VertexGridStatus::Ok;
}

} // namespace

VertexGridStatus SampleLegacyVertexGrid(HeightFieldView h,
    const LegacyModelPosition* positions, size_t count,
    LegacyModelPosition low, float density,
    LegacyVertexGridSample* output, size_t outputCount) {
    const VertexGridStatus status = ValidateHeightField(h);
    if (status != VertexGridStatus::Ok) return status;
    const size_t maximum = static_cast<size_t>(-1);
    if (count > maximum / sizeof(LegacyModelPosition)
        || count > maximum / sizeof(LegacyVertexGridSample)
        || (count && (!positions || !output || outputCount < count)))
        return VertexGridStatus::InvalidBuffer;
    if (!Finite(low.x) || !Finite(low.y) || !Finite(low.z) || !Finite(density))
        return VertexGridStatus::InvalidCoordinate;
    const float dx = static_cast<float>(h.width) * density;
    const float dz = static_cast<float>(h.height) * density;
    if (!Finite(dx) || !Finite(dz)) return VertexGridStatus::InvalidCoordinate;
    LegacyVertexGridSample sample = {};
    for(size_t vertex=0;vertex<count;++vertex) {
        const VertexGridStatus result = Sample(h,positions[vertex],low,dx,dz,sample);
        if (result != VertexGridStatus::Ok) return result;
    }
    for(size_t vertex=0;vertex<count;++vertex) {
        Sample(h,positions[vertex],low,dx,dz,sample);
        output[vertex] = sample;
    }
    return VertexGridStatus::Ok;
}

} }
