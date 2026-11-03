#include "NativeWaterMesh.h"

namespace vince {
namespace water {
namespace {
struct Layout {
    const unsigned char* bytes;
    size_t size;
    uint32_t vertices, indices, strips, materials;
    uint32_t vertexOffset, indexOffset, stripOffset;
    float low[3], high[3];
};

uint32_t Read32(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t Read16(const unsigned char* p) { return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8)); }
float ReadFloat(const unsigned char* p) {
    const uint32_t bits = Read32(p);
    float result;
    unsigned char* out = reinterpret_cast<unsigned char*>(&result);
    const unsigned char* in = reinterpret_cast<const unsigned char*>(&bits);
    for (unsigned i = 0; i < 4; ++i)
        out[i] = in[i];
    return result;
}
bool Finite(float v) { return v >= -3.4028234663852886e38 && v <= 3.4028234663852886e38; }
bool Range(size_t size, uint32_t offset, uint32_t count, size_t stride) {
    return offset <= size && count <= (size - offset) / stride;
}

NativeMeshStatus Parse(const void* blob, size_t byteCount, Layout& l) {
    if (!blob)
        return NativeMeshStatus::InvalidArgument;
    if (byteCount < 0x80)
        return NativeMeshStatus::TruncatedData;
    const unsigned char* b = static_cast<const unsigned char*>(blob);
    if (Read32(b) != 0x52544147u || Read32(b + 4) != 29 || Read32(b + 8) > 1 || b[12] || b[13] ||
        b[14])
        return NativeMeshStatus::UnsupportedLayout;
    l.bytes = b;
    l.size = byteCount;
    l.vertices = Read32(b + 0x10);
    l.indices = Read32(b + 0x14);
    l.strips = Read32(b + 0x18);
    l.materials = Read32(b + 0x1c);
    l.vertexOffset = Read32(b + 0x30);
    l.indexOffset = Read32(b + 0x34);
    l.stripOffset = Read32(b + 0x38);
    // D3D strip cut0xffff is reserved here; do not reinterpret as vertex65535.
    if (!l.vertices || l.vertices > 65535 || !l.strips || !l.materials)
        return NativeMeshStatus::UnsupportedLayout;
    if (l.vertexOffset < 0x80 || l.indexOffset < 0x80 || l.stripOffset < 0x80 ||
        !Range(byteCount, l.vertexOffset, l.vertices, 64) ||
        !Range(byteCount, l.indexOffset, l.indices, 2) ||
        !Range(byteCount, l.stripOffset, l.strips, 48))
        return NativeMeshStatus::TruncatedData;
    const size_t starts[3] = {l.vertexOffset, l.indexOffset, l.stripOffset};
    const size_t lengths[3] = {size_t(l.vertices) * 64, size_t(l.indices) * 2,
                               size_t(l.strips) * 48};
    for (unsigned a = 0; a < 3; ++a)
        for (unsigned c = a + 1; c < 3; ++c)
            if (lengths[a] && lengths[c] && starts[a] < starts[c] + lengths[c] &&
                starts[c] < starts[a] + lengths[a])
                return NativeMeshStatus::UnsupportedLayout;
    for (unsigned axis = 0; axis < 3; ++axis) {
        l.low[axis] = ReadFloat(b + 0x54 + axis * 4);
        l.high[axis] = ReadFloat(b + 0x60 + axis * 4);
        if (!Finite(l.low[axis]) || !Finite(l.high[axis]) || l.low[axis] > l.high[axis])
            return NativeMeshStatus::InvalidVertex;
    }
    // Decode only fields whose float layout is demonstrated by native copying.
    for (uint32_t i = 0; i < l.vertices; ++i) {
        const unsigned char* v = b + l.vertexOffset + size_t(i) * 64;
        const unsigned offsets[] = {0, 4, 8, 48, 52, 56, 60};
        for (unsigned offset : offsets)
            if (!Finite(ReadFloat(v + offset)))
                return NativeMeshStatus::InvalidVertex;
    }
    return NativeMeshStatus::Ok;
}

// Validate every strip/index and optionally emit expanded triangle-list data.
NativeMeshStatus Expand(const Layout& l, uint32_t* output, DecodedWaterStrip* ranges,
                        uint32_t& emitted) {
    uint32_t consumed = 0;
    emitted = 0;
    for (uint32_t strip = 0; strip < l.strips; ++strip) {
        const unsigned char* record = l.bytes + l.stripOffset + size_t(strip) * 48;
        const uint32_t count = Read32(record);
        const uint32_t material = Read16(record + 0x14);
        if (count > l.indices - consumed || material >= l.materials)
            return NativeMeshStatus::InvalidStrip;
        const uint32_t first = emitted;
        uint32_t a = 0, b = 0, run = 0;
        for (uint32_t local = 0; local < count; ++local) {
            const uint32_t c = Read16(l.bytes + l.indexOffset + size_t(consumed + local) * 2);
            if (c == 0xffffu) {
                run = 0;
                continue;
            }
            if (c >= l.vertices)
                return NativeMeshStatus::InvalidIndex;
            if (run >= 2 && a != b && b != c && a != c) {
                if (emitted > 0xffffffffu - 3)
                    return NativeMeshStatus::UnsupportedLayout;
                if (output) {
                    output[emitted] = (run & 1) ? b : a;
                    output[emitted + 1] = (run & 1) ? a : b;
                    output[emitted + 2] = c;
                }
                emitted += 3;
            }
            a = b;
            b = c;
            ++run;
        }
        if (ranges)
            ranges[strip] = {material, first, emitted - first};
        consumed += count;
    }
    return consumed == l.indices ? NativeMeshStatus::Ok : NativeMeshStatus::InvalidStrip;
}

void Information(const Layout& l, uint32_t count, NativeWaterMeshInfo& info) {
    info.vertexCount = l.vertices;
    info.triangleIndexCount = count;
    info.stripCount = l.strips;
    info.materialCount = l.materials;
    for (unsigned axis = 0; axis < 3; ++axis) {
        info.boundsLow[axis] = l.low[axis];
        info.boundsHigh[axis] = l.high[axis];
    }
}

} // namespace

NativeMeshStatus ProbeNativeWaterMesh(const void* blob, size_t byteCount,
                                      NativeWaterMeshInfo* info) {
    if (!info)
        return NativeMeshStatus::InvalidArgument;
    Layout l = {};
    NativeMeshStatus status = Parse(blob, byteCount, l);
    if (status != NativeMeshStatus::Ok)
        return status;
    uint32_t count = 0;
    status = Expand(l, nullptr, nullptr, count);
    if (status != NativeMeshStatus::Ok)
        return status;
    Information(l, count, *info);
    return NativeMeshStatus::Ok;
}

NativeMeshStatus DecodeNativeWaterMesh(const void* blob, size_t byteCount, float density,
                                       DecodedWaterVertex* vertices, size_t vertexCapacity,
                                       uint32_t* indices, size_t indexCapacity,
                                       DecodedWaterStrip* strips, size_t stripCapacity,
                                       NativeWaterMeshInfo* info) {
    if (!info || !Finite(density))
        return NativeMeshStatus::InvalidArgument;
    Layout l = {};
    NativeMeshStatus status = Parse(blob, byteCount, l);
    if (status != NativeMeshStatus::Ok)
        return status;
    uint32_t count = 0;
    status = Expand(l, nullptr, nullptr, count);
    if (status != NativeMeshStatus::Ok)
        return status;
    if (!vertices || !strips || (count && !indices) || vertexCapacity < l.vertices ||
        indexCapacity < count || stripCapacity < l.strips)
        return NativeMeshStatus::InsufficientCapacity;
    const double scale = double(density) * double(.16f);
    for (uint32_t i = 0; i < l.vertices; ++i) {
        const unsigned char* v = l.bytes + l.vertexOffset + size_t(i) * 64;
        for (unsigned axis = 0; axis < 2; ++axis) {
            const unsigned component = axis ? 2 : 0;
            const double uv = (double(ReadFloat(v + component * 4)) - l.low[component]) * scale;
            if (!(uv >= -3.4028234663852886e38 && uv <= 3.4028234663852886e38))
                return NativeMeshStatus::InvalidVertex;
        }
    }
    for (uint32_t i = 0; i < l.vertices; ++i) {
        const unsigned char* v = l.bytes + l.vertexOffset + size_t(i) * 64;
        for (unsigned axis = 0; axis < 3; ++axis)
            vertices[i].position[axis] = ReadFloat(v + axis * 4);
        vertices[i].normal[0] = 0.f;
        vertices[i].normal[1] = 1.f;
        vertices[i].normal[2] = 0.f;
        vertices[i].bumpUv[0] =
            static_cast<float>((double(vertices[i].position[0]) - l.low[0]) * scale);
        vertices[i].bumpUv[1] =
            static_cast<float>((double(vertices[i].position[2]) - l.low[2]) * scale);
        for (unsigned axis = 0; axis < 2; ++axis) {
            vertices[i].materialUv[axis] = ReadFloat(v + 48 + axis * 4);
            vertices[i].detailUv[axis] = ReadFloat(v + 56 + axis * 4);
        }
    }
    Expand(l, indices, strips, count);
    Information(l, count, *info);
    return NativeMeshStatus::Ok;
}

} // namespace water
} // namespace vince
