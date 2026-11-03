#include "NativeWaterField.h"

namespace vince {
namespace water {
namespace {
uint32_t U32(const unsigned char* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
float F32(const unsigned char* p) {
    const uint32_t bits = U32(p);
    float f;
    unsigned char* out = reinterpret_cast<unsigned char*>(&f);
    const unsigned char* in = reinterpret_cast<const unsigned char*>(&bits);
    for (unsigned i = 0; i < 4; ++i)
        out[i] = in[i];
    return f;
}
bool Finite(float x) { return x >= -3.4028234663852886e38 && x <= 3.4028234663852886e38; }
bool Bounds(const unsigned char* p, float* low, float* high) {
    for (unsigned axis = 0; axis < 3; ++axis) {
        low[axis] = F32(p + axis * 4);
        high[axis] = F32(p + 12 + axis * 4);
        if (!Finite(low[axis]) || !Finite(high[axis]) || low[axis] > high[axis])
            return false;
    }
    return true;
}
bool Pointer(uintptr_t address) { return address && address <= 0xffffffffu && !(address & 3); }
struct CapturePlan {
    uint32_t blob;
    unsigned char header[128];
    uint32_t offsets[3];
    size_t bytes[3];
    size_t span;
};
NativeFieldStatus Plan(const NativeMemory& memory, uintptr_t model, CapturePlan& plan) {
    if (!Pointer(model) || model > 0xffffffffu - NativeWaterOffsets::ModelBlob - 4)
        return NativeFieldStatus::InvalidArgument;
    if (!memory.Read(model + NativeWaterOffsets::ModelBlob, &plan.blob, 4) || !Pointer(plan.blob) ||
        !memory.Read(plan.blob, plan.header, sizeof(plan.header)))
        return NativeFieldStatus::ReadFailed;
    const unsigned char* h = plan.header;
    const uint32_t vertices = U32(h + 16), indices = U32(h + 20), strips = U32(h + 24),
                   materials = U32(h + 28);
    if (U32(h) != 0x52544147u || U32(h + 4) != 29 || U32(h + 8) > 1 || h[12] || h[13] || h[14] ||
        !vertices || vertices > 65535 || !strips || strips > 4096 || !materials ||
        materials > 65535)
        return NativeFieldStatus::UnsupportedModelLayout;
    const uint32_t counts[3] = {vertices, indices, strips};
    const size_t strides[3] = {64, 2, 48};
    plan.span = 128;
    for (unsigned r = 0; r < 3; ++r) {
        plan.offsets[r] = U32(h + 48 + r * 4);
        const uint64_t bytes = uint64_t(counts[r]) * strides[r];
        const uint64_t end = uint64_t(plan.offsets[r]) + bytes;
        if (plan.offsets[r] < 128 || end > 64u * 1024u * 1024u ||
            uint64_t(plan.blob) + end > 0x100000000ull)
            return NativeFieldStatus::UnsupportedModelLayout;
        plan.bytes[r] = static_cast<size_t>(bytes);
        if (end > plan.span)
            plan.span = static_cast<size_t>(end);
    }
    for (unsigned a = 0; a < 3; ++a)
        for (unsigned b = a + 1; b < 3; ++b)
            if (plan.bytes[a] && plan.bytes[b] &&
                plan.offsets[a] < plan.offsets[b] + plan.bytes[b] &&
                plan.offsets[b] < plan.offsets[a] + plan.bytes[a])
                return NativeFieldStatus::UnsupportedModelLayout;
    return NativeFieldStatus::Ok;
}

} // namespace

NativeFieldStatus ReadNativeWaterNode(const NativeMemory& memory, uintptr_t address,
                                      NativeWaterNodeSnapshot* output) {
    if (!output || !Pointer(address))
        return NativeFieldStatus::InvalidArgument;
    unsigned char bytes[NativeWaterOffsets::WaterNodeBytes];
    if (!memory.Read(address, bytes, sizeof(bytes)))
        return NativeFieldStatus::ReadFailed;
    if (U32(bytes) != memory.Address(NativeWaterOffsets::WaterNodeVtable))
        return NativeFieldStatus::WrongNodeType;
    NativeWaterNodeSnapshot snapshot = {};
    snapshot.node = address;
    snapshot.modelResource = U32(bytes + NativeWaterOffsets::NodeModel);
    snapshot.proceduralField = U32(bytes + NativeWaterOffsets::NodeField);
    snapshot.materialResource = U32(bytes + NativeWaterOffsets::NodeMaterial);
    snapshot.cachedVertexBuffer = U32(bytes + NativeWaterOffsets::NodeCachedVertexBuffer);
    snapshot.cachedVertices = U32(bytes + NativeWaterOffsets::NodeCachedVertices);
    if (!Pointer(snapshot.modelResource) || !Pointer(snapshot.proceduralField) ||
        !Bounds(bytes + NativeWaterOffsets::NodeBounds, snapshot.boundsLow, snapshot.boundsHigh))
        return NativeFieldStatus::InvalidField;
    *output = snapshot;
    return NativeFieldStatus::Ok;
}

NativeFieldStatus ReadNativeProceduralField(const NativeMemory& memory, uintptr_t address,
                                            NativeProceduralFieldSnapshot* output) {
    if (!output || !Pointer(address))
        return NativeFieldStatus::InvalidArgument;
    unsigned char bytes[NativeWaterOffsets::ProceduralFieldBytes];
    if (!memory.Read(address, bytes, sizeof(bytes)))
        return NativeFieldStatus::ReadFailed;
    NativeProceduralFieldSnapshot snapshot = {};
    snapshot.field = address;
    snapshot.width = U32(bytes + NativeWaterOffsets::FieldWidth);
    snapshot.height = U32(bytes + NativeWaterOffsets::FieldHeight);
    snapshot.indexCount = U32(bytes + NativeWaterOffsets::FieldIndexCount);
    snapshot.cpuIndices = U32(bytes + NativeWaterOffsets::FieldIndices);
    snapshot.indexBuffer = U32(bytes + NativeWaterOffsets::FieldIndexBuffer);
    snapshot.waveStrength = F32(bytes + NativeWaterOffsets::FieldStrength);
    snapshot.waveElevation = F32(bytes + NativeWaterOffsets::FieldElevation);
    snapshot.waveScale = F32(bytes + NativeWaterOffsets::FieldScale);
    // Constructor emits uint16 triangle-list indices; dimensions must fit that
    // proven format. Do not accept a wrapped/truncated count as valid geometry.
    const uint64_t vertices = uint64_t(snapshot.width) * snapshot.height;
    if (snapshot.width < 2 || snapshot.height < 2 || vertices > 65536 ||
        snapshot.indexCount != uint64_t(snapshot.width - 1) * (snapshot.height - 1) * 6 ||
        !Pointer(snapshot.cpuIndices) || !Pointer(snapshot.indexBuffer) ||
        !Finite(snapshot.waveStrength) || !Finite(snapshot.waveElevation) ||
        !Finite(snapshot.waveScale))
        return NativeFieldStatus::InvalidField;
    for (unsigned i = 0; i < 4; ++i) {
        snapshot.color[i] = F32(bytes + NativeWaterOffsets::FieldColor + i * 4);
        if (!Finite(snapshot.color[i]))
            return NativeFieldStatus::InvalidField;
    }
    *output = snapshot;
    return NativeFieldStatus::Ok;
}

NativeFieldStatus CaptureNativeWaterModel(const NativeMemory& memory, uintptr_t model,
                                          size_t length, void* destination, size_t capacity,
                                          uintptr_t* address, NativeWaterMeshInfo* information) {
    if (!Pointer(model) || !destination || !address || !information)
        return NativeFieldStatus::InvalidArgument;
    if (!length)
        return NativeFieldStatus::MissingValidatedBlobLength;
    if (length < 0x80 || capacity < length)
        return NativeFieldStatus::InsufficientCapacity;
    uint32_t pointer = 0;
    if (!memory.Read(model + NativeWaterOffsets::ModelBlob, &pointer, sizeof(pointer)))
        return NativeFieldStatus::ReadFailed;
    if (!Pointer(pointer))
        return NativeFieldStatus::InvalidField;
    if (!memory.Read(pointer, destination, length))
        return NativeFieldStatus::ReadFailed;
    NativeWaterMeshInfo info = {};
    if (ProbeNativeWaterMesh(destination, length, &info) != NativeMeshStatus::Ok)
        return NativeFieldStatus::InvalidMesh;
    *address = pointer;
    *information = info;
    return NativeFieldStatus::Ok;
}

NativeFieldStatus QueryNativeWaterModelSpan(const NativeMemory& memory, uintptr_t model,
                                            size_t* required) {
    if (!required)
        return NativeFieldStatus::InvalidArgument;
    CapturePlan plan = {};
    const NativeFieldStatus status = Plan(memory, model, plan);
    if (status != NativeFieldStatus::Ok)
        return status;
    *required = plan.span;
    return NativeFieldStatus::Ok;
}
NativeFieldStatus CaptureNativeWaterModelRanges(const NativeMemory& memory, uintptr_t model,
                                                void* destination, size_t capacity,
                                                uintptr_t* address,
                                                NativeWaterMeshInfo* information) {
    if (!destination || !address || !information)
        return NativeFieldStatus::InvalidArgument;
    CapturePlan plan = {};
    const NativeFieldStatus status = Plan(memory, model, plan);
    if (status != NativeFieldStatus::Ok)
        return status;
    if (capacity < plan.span)
        return NativeFieldStatus::InsufficientCapacity;
    unsigned char* out = static_cast<unsigned char*>(destination);
    // Unreferenced gaps contain no decoder input and are never read from native
    // memory. Initialize those bytes to keep the capture deterministic/private.
    for (size_t i = 0; i < plan.span; ++i)
        out[i] = 0;
    for (unsigned i = 0; i < 128; ++i)
        out[i] = plan.header[i];
    for (unsigned r = 0; r < 3; ++r)
        if (plan.bytes[r] &&
            !memory.Read(plan.blob + plan.offsets[r], out + plan.offsets[r], plan.bytes[r]))
            return NativeFieldStatus::ReadFailed;
    NativeWaterMeshInfo info = {};
    if (ProbeNativeWaterMesh(out, plan.span, &info) != NativeMeshStatus::Ok)
        return NativeFieldStatus::InvalidMesh;
    *address = plan.blob;
    *information = info;
    return NativeFieldStatus::Ok;
}

} // namespace water
} // namespace vince
