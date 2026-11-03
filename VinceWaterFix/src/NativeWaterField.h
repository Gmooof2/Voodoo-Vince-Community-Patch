#pragma once
#include "NativeWaterMemory.h"
#include "NativeWaterMesh.h"

namespace vince {
namespace water {

// Preferred-image addresses/offsets from Steam1.14.2.0 only. Validate the image
// identity before using these readers; the callback must safely reject stale
// memory. These helpers do not hook constructors or own native objects.
namespace NativeWaterOffsets {
constexpr uint32_t WaterNodeVtable = 0x00adbf78;
constexpr size_t WaterNodeBytes = 0xc0;
constexpr size_t NodeBounds = 0x64;
constexpr size_t NodeModel = 0xa8;
constexpr size_t NodeField = 0xac;
constexpr size_t NodeMaterial = 0xb0;
constexpr size_t NodeCachedVertexBuffer = 0xb8;
constexpr size_t NodeCachedVertices = 0xbc;
constexpr size_t ModelBlob = 0x150;
constexpr size_t ProceduralFieldBytes = 0xf8;
constexpr size_t FieldColor = 0x18;
constexpr size_t FieldStrength = 0x78;
constexpr size_t FieldElevation = 0x7c;
constexpr size_t FieldScale = 0x80;
constexpr size_t FieldWidth = 0x8c;
constexpr size_t FieldHeight = 0x90;
constexpr size_t FieldIndexCount = 0xcc;
constexpr size_t FieldIndices = 0xd0;
constexpr size_t FieldIndexBuffer = 0xd4;
} // namespace NativeWaterOffsets

enum class NativeFieldStatus {
    Ok,
    InvalidArgument,
    ReadFailed,
    WrongNodeType,
    InvalidField,
    MissingValidatedBlobLength,
    InsufficientCapacity,
    InvalidMesh,
    UnsupportedModelLayout
};

struct NativeWaterNodeSnapshot {
    uintptr_t node;
    uintptr_t modelResource;
    uintptr_t proceduralField;
    uintptr_t materialResource;
    uintptr_t cachedVertexBuffer;
    uintptr_t cachedVertices;
    float boundsLow[3];
    float boundsHigh[3];
};

// These are native remaster wave parameters, NOT original simulation settings.
struct NativeProceduralFieldSnapshot {
    uintptr_t field;
    uint32_t width, height, indexCount;
    uintptr_t cpuIndices, indexBuffer;
    float color[4];
    float waveStrength, waveElevation, waveScale;
};

NativeFieldStatus ReadNativeWaterNode(const NativeMemory& memory, uintptr_t nodeAddress,
                                      NativeWaterNodeSnapshot* output);
NativeFieldStatus ReadNativeProceduralField(const NativeMemory& memory, uintptr_t fieldAddress,
                                            NativeProceduralFieldSnapshot* output);

// Caller must obtain blobBytes from its resource/file ownership metadata (e.g.
// exact decompressed HOT entry length). A VirtualQuery region size is not an
// acceptable replacement: adjacent allocations can be readable but not belong
// to this model. Zero bytes fails closed. This routine reads only; source data
// stays native-owned and only the destination capture belongs to the patch.
NativeFieldStatus CaptureNativeWaterModel(const NativeMemory& memory,
                                          uintptr_t modelResourceAddress, size_t validatedBlobBytes,
                                          void* destination, size_t destinationCapacity,
                                          uintptr_t* blobAddress, NativeWaterMeshInfo* information);

// When native resource ownership does not expose a file byte count, build the
// minimum decoder capture from the header and ONLY its three referenced arrays.
// Structural limits: static GATR29, <=65535 vertices, <=4096 strips, <=64MiB span;
// all referenced ranges must be disjoint and each read must pass the callback.
// This is not a claim that readable pages prove object ownership: call only while
// the integrating host owns the qualified model/resource lifetime on its thread.
NativeFieldStatus QueryNativeWaterModelSpan(const NativeMemory& memory,
                                            uintptr_t modelResourceAddress,
                                            size_t* requiredCaptureBytes);
NativeFieldStatus CaptureNativeWaterModelRanges(const NativeMemory& memory,
                                                uintptr_t modelResourceAddress, void* destination,
                                                size_t destinationCapacity, uintptr_t* blobAddress,
                                                NativeWaterMeshInfo* information);

} // namespace water
} // namespace vince
