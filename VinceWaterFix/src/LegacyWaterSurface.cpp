#include "LegacyWaterSurface.h"
#include <string.h>

namespace vince { namespace water {
namespace {
bool Finite(float value) { return value >= -3.402823466e38f && value <= 3.402823466e38f; }
bool PowerOfTwo(UINT value) { return value && !(value & (value - 1)); }
}

SurfaceStatus LegacyWaterSurface::Initialize(ID3D11Device* device,
    const LegacyWaterPrograms& programs, LegacyWaterMesh mesh, LegacyModelPosition low,
    LegacyWaterSurfaceStorage storage, LegacyWaterSurfaceSettings settings,
    const uint32_t* cube, size_t cubePixels) {
    if (initialized_) return SurfaceStatus::AlreadyInitialized;
    const uint64_t pixels = uint64_t(settings.width) * settings.height;
    if (!device || !mesh.vertices || !mesh.vertexCount || !mesh.indices || !mesh.indexCount ||
        !PowerOfTwo(settings.width) || !PowerOfTwo(settings.height) || pixels > SIZE_MAX ||
        !Finite(settings.density) || settings.density <= 0 ||
        !Finite(settings.verticalness) || settings.verticalness <= 0 ||
        !Finite(settings.simulation.speed) || !Finite(settings.simulation.turbulence) ||
        !Finite(settings.simulation.elasticity) || !Finite(low.x) || !Finite(low.y) || !Finite(low.z) ||
        !storage.fields[0] || !storage.fields[1] || storage.fields[0] == storage.fields[1] ||
        !storage.sourcePositions || !storage.samples || !storage.vertices || !storage.bumpPixels ||
        storage.fieldCapacity[0] < pixels || storage.fieldCapacity[1] < pixels ||
        storage.sourceCapacity < mesh.vertexCount || storage.sampleCapacity < mesh.vertexCount ||
        storage.vertexCapacity < mesh.vertexCount || storage.bumpCapacity < pixels ||
        (storage.displayField && (storage.displayCapacity < pixels ||
            storage.displayField == storage.fields[0] || storage.displayField == storage.fields[1])))
        return SurfaceStatus::InvalidArgument;
    const HRESULT status = renderer_.Initialize(device, programs, mesh,
        settings.width, settings.height, cube, cubePixels);
    if (FAILED(status)) return SurfaceStatus::GraphicsFailed;
    storage_ = storage;
    settings_ = settings;
    boundsLow_ = low;
    vertexCount_ = mesh.vertexCount;
    // A decoder may have written directly into storage.vertices.
    if (storage.vertices != mesh.vertices)
        memcpy(storage.vertices, mesh.vertices, size_t(mesh.vertexCount) * sizeof(DecodedWaterVertex));
    for (UINT i=0; i<mesh.vertexCount; ++i) {
        storage.sourcePositions[i] = {mesh.vertices[i].position[0],
            mesh.vertices[i].position[1], mesh.vertices[i].position[2]};
    }
    simulation_ = {{storage.fields[0], storage.fields[1]},
        {storage.fieldCapacity[0], storage.fieldCapacity[1]}, settings.width,
        settings.height, settings.width, 0, settings.wrap, false};
    const SimulationStatus reset = ResetLegacyWaterSimulation(simulation_);
    const SimulationStatus distort = reset == SimulationStatus::Ok ?
        InitializeLegacySurfaceDistortions(simulation_, settings.simulation.turbulence,
            &settings_.random) : reset;
    if (distort != SimulationStatus::Ok && distort != SimulationStatus::NoChange) {
        Reset();
        return SurfaceStatus::SimulationFailed;
    }
    initialized_ = true;
    return SurfaceStatus::Ok;
}

void LegacyWaterSurface::Reset() {
    renderer_.Reset();
    storage_ = {};
    simulation_ = {};
    settings_ = {};
    vertexCount_ = 0;
    lastToken_ = 0;
    lastBlend_ = 1.0f;
    initialized_ = stepped_ = uploaded_ = false;
}

SurfaceStatus LegacyWaterSurface::Update(ID3D11DeviceContext* context, float delta, uint64_t token,
    float blend) {
    if (!initialized_) return SurfaceStatus::NotInitialized;
    if (!context || !Finite(delta) || !(blend >= 0.0f && blend <= 1.0f) ||
        (blend < 1.0f && !storage_.displayField)) return SurfaceStatus::InvalidArgument;
    if (stepped_ && token == lastToken_ && uploaded_ && blend == lastBlend_) return SurfaceStatus::Ok;
    uploaded_ = false;
    if (!stepped_ || token != lastToken_) {
        const SimulationStatus step = StepLegacyWaterSimulation(simulation_,
            settings_.simulation, delta, &settings_.random);
        if (step != SimulationStatus::Ok && step != SimulationStatus::NoChange)
            return SurfaceStatus::SimulationFailed;
        stepped_ = true;
        lastToken_ = token;
    }
    lastBlend_ = blend;
    HeightFieldView heights = {simulation_.fields[simulation_.currentIndex],
        simulation_.capacities[simulation_.currentIndex], simulation_.width,
        simulation_.height, simulation_.rowStride};
    if (blend < 1.0f) {
        // After a step the other buffer holds the state before it.
        const float* previous = simulation_.fields[simulation_.currentIndex ^ 1];
        const float* latest = heights.data;
        for (size_t z = 0; z < simulation_.height; ++z)
            for (size_t x = 0; x < simulation_.width; ++x) {
                const size_t i = z * simulation_.rowStride + x;
                storage_.displayField[i] = previous[i] + (latest[i] - previous[i]) * blend;
            }
        heights.data = storage_.displayField;
        heights.dataCount = storage_.displayCapacity;
    }
    if (settings_.sampleVertices && SampleLegacyVertexGrid(heights, storage_.sourcePositions,
        vertexCount_, boundsLow_, settings_.density, storage_.samples,
        storage_.sampleCapacity) != VertexGridStatus::Ok)
        return SurfaceStatus::VertexSamplingFailed;
    if (GenerateLegacyNormalMap(heights, settings_.verticalness, storage_.bumpPixels,
        storage_.bumpCapacity) != NormalMapStatus::Ok) return SurfaceStatus::NormalMapFailed;
    for (UINT i=0; settings_.sampleVertices && i<vertexCount_; ++i) {
        storage_.vertices[i].position[1] = storage_.samples[i].displacedY;
        storage_.vertices[i].normal[0] = storage_.samples[i].normalX;
        storage_.vertices[i].normal[2] = storage_.samples[i].normalZ;
    }
    if (FAILED(renderer_.Upload(context, storage_.vertices, vertexCount_, storage_.bumpPixels,
        size_t(settings_.width) * settings_.height))) return SurfaceStatus::GraphicsFailed;
    uploaded_ = true;
    return SurfaceStatus::Ok;
}

HRESULT LegacyWaterSurface::Draw(ID3D11DeviceContext* context, const LegacyWaterDraw& parameters) {
    return initialized_ && uploaded_ ? renderer_.Draw(context, parameters) : E_UNEXPECTED;
}

SimulationStatus LegacyWaterSurface::Perturb(float u, float v, float amount) {
    if (!initialized_) return SimulationStatus::InvalidState;
    const SimulationStatus status = PerturbLegacyWater(simulation_, u, v, amount);
    if (status == SimulationStatus::Ok) uploaded_ = false;
    return status;
}

}} // namespace vince::water
