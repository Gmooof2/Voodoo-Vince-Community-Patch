#pragma once
#include <stddef.h>
#include <stdint.h>
#include "LegacyWaterContract.h"

namespace vince {
namespace water {

enum class NativeMeshStatus {
    Ok,
    InvalidArgument,
    UnsupportedLayout,
    TruncatedData,
    InvalidIndex,
    InvalidStrip,
    InvalidVertex,
    InsufficientCapacity
};

struct NativeWaterMeshInfo {
    uint32_t vertexCount;
    uint32_t triangleIndexCount;
    uint32_t stripCount;
    uint32_t materialCount;
    float boundsLow[3];
    float boundsHigh[3];
};

struct DecodedWaterStrip {
    uint32_t materialId;
    uint32_t firstIndex;
    uint32_t indexCount;
};

// Read-only decoder for PC GATR version29 static water meshes, not a cast over
// ModelResource. At the inspected native call site the complete blob is at
// ModelResource+0x150. The integrating host supplies its VALIDATED byte length.
// Probe/Decode reject bone/ex-vertex/prelit variants rather than guessing layout.
// Output topology is triangle list, converted from uint16 source strips; output
// ranges preserve each strip's material assignment. The host draws ranges with
// their corresponding material/resources instead of merging material states.
NativeMeshStatus ProbeNativeWaterMesh(const void* blob, size_t byteCount,
                                      NativeWaterMeshInfo* information);

// density is the original waterdensity setting. Original-derived bump UVs are
// (position-bounds.low).xz*density*.16; material/detail UVs are native float2s.
// The original constructor initializes normal=(0,1,0), then per-frame sampled
// displacement updates y and normal.x/z through SampleLegacyVertexGrid.
// Caller owns all buffers. No allocation, exceptions, or native memory writes.
// Buffers must not overlap the blob/each other. All validation precedes writes.
NativeMeshStatus DecodeNativeWaterMesh(const void* blob, size_t byteCount, float density,
                                       DecodedWaterVertex* vertices, size_t vertexCapacity,
                                       uint32_t* triangleIndices, size_t indexCapacity,
                                       DecodedWaterStrip* strips, size_t stripCapacity,
                                       NativeWaterMeshInfo* information);

} // namespace water
} // namespace vince
