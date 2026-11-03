#pragma once
#include "LegacyWaterNormals.h"

namespace vince { namespace water {

struct LegacyModelPosition { float x, y, z; };
struct LegacyVertexGridSample {
    float displacedY;
    float normalX;
    float normalZ;
};

enum class VertexGridStatus {
    Ok, InvalidDimensions, InvalidStride, InvalidBuffer,
    InvalidCoordinate, NonfiniteHeight, ArithmeticOverflow
};

// Readable CPU equivalent of original WaterField::UpdateVertexGrid sampling.
// Source positions and bounding-box low corner use model-local coordinates.
// density is the original WaterRegion waterdensity parameter. Source vertex y
// is deliberately ignored; displacedY = boundsLow.y + sampled water height.
// Sampling and slope neighbors always wrap, even for a bounded simulation.
// The integrating host updates only position.y and normal.x/z with the result;
// it preserves normal.y, position.x/z, UVs and other native mesh attributes.
// The host supplies decoded source positions, not guessed packed vertex bytes.
// No allocation/exceptions; input/output storage must not overlap. Validation
// completes before output changes. count zero permits null position/output.
VertexGridStatus SampleLegacyVertexGrid(HeightFieldView currentHeights,
    const LegacyModelPosition* sourcePositions, size_t count,
    LegacyModelPosition boundsLow, float density,
    LegacyVertexGridSample* output, size_t outputCount);

} }
