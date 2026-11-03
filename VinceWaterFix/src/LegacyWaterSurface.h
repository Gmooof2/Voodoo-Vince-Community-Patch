#pragma once
#include "LegacyWaterD3D11.h"
#include "LegacyWaterSimulation.h"
#include "LegacyWaterVertexGrid.h"

namespace vince { namespace water {

// CPU storage is owned by the AIO/host and must outlive this surface. Each
// array's capacity is checked during initialization. Source positions preserve
// the undeformed mesh; mutable vertices are the decoded rendering copy.
struct LegacyWaterSurfaceStorage {
    float* fields[2];
    size_t fieldCapacity[2];
    LegacyModelPosition* sourcePositions;
    size_t sourceCapacity;
    LegacyVertexGridSample* samples;
    size_t sampleCapacity;
    DecodedWaterVertex* vertices;
    size_t vertexCapacity;
    uint32_t* bumpPixels;
    size_t bumpCapacity;
    // Optional: heights shown between fixed steps (see Update's blend).
    float* displayField = nullptr;
    size_t displayCapacity = 0;
};

struct LegacyWaterSurfaceSettings {
    UINT width, height;
    float density, verticalness;
    bool wrap;
    LegacyWaterSimulationParameters simulation;
    LegacyWaterRandomState random;
    // Model water samples the field into its vertices (UpdateVertexGrid).
    // SwampPuddle::Update only steps the field and its bump map, so its quad
    // stays flat and ripples show through the normal map alone.
    bool sampleVertices = true;
};

enum class SurfaceStatus {
    Ok, InvalidArgument, AlreadyInitialized, NotInitialized,
    SimulationFailed, VertexSamplingFailed, NormalMapFailed, GraphicsFailed
};

// Cohesive original-water pipeline. No native addresses, hooks, allocation or
// frame clock policy. One instance per live model; destroy/reset it when the
// host's level/resource lifetime changes. Call only on its immediate render
// thread. The caller supplies a frame token so reflection/camera passes do not
// advance the simulation multiple times during the same game frame.
class LegacyWaterSurface {
public:
    SurfaceStatus Initialize(ID3D11Device* device, const LegacyWaterPrograms& programs,
        LegacyWaterMesh mesh, LegacyModelPosition boundsLow,
        LegacyWaterSurfaceStorage storage, LegacyWaterSurfaceSettings settings,
        const uint32_t* directionCube, size_t cubePixels);
    void Reset();
    // blend in [0,1] shows the heights that far from the previous step to the
    // latest one, so fixed-cadence hosts can display smoothly between steps one
    // step behind. 1 shows the latest step. Needs storage.displayField below 1.
    SurfaceStatus Update(ID3D11DeviceContext* context, float gameDelta, uint64_t frameToken,
                         float blend = 1.0f);
    HRESULT Draw(ID3D11DeviceContext* context, const LegacyWaterDraw& parameters);
    SimulationStatus Perturb(float u, float v, float amount);
private:
    LegacyWaterD3D11 renderer_;
    LegacyWaterSurfaceStorage storage_ = {};
    LegacyWaterSurfaceSettings settings_ = {};
    LegacyWaterSimulation simulation_ = {};
    LegacyModelPosition boundsLow_ = {};
    UINT vertexCount_ = 0;
    uint64_t lastToken_ = 0;
    float lastBlend_ = 1.0f;
    bool initialized_ = false, stepped_ = false, uploaded_ = false;
};

}} // namespace vince::water
