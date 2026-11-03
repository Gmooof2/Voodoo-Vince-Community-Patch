#include "NativeWaterRuntime.h"
#include "NativeWaterField.h"
#include "NativeWaterScene.h"
#include "NativeWaterMaterials.h"
#include "LegacyWaterAreas.h"
#include "LegacyWaterDefaults.h"
#include "LegacyWaterPuddles.h"
#include "LegacyWaterCube.h"
#include <math.h>
#include <new>
#include <string.h>

namespace vince {
namespace water {
namespace {
struct HeapBlock {
    void* data = nullptr;
    ~HeapBlock() {
        if (data)
            HeapFree(GetProcessHeap(), 0, data);
    }
    bool Allocate(size_t bytes) {
        data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
        return data != nullptr;
    }
};

bool EqualArea(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        const char x = *a >= 'A' && *a <= 'Z' ? char(*a + 'a' - 'A') : *a;
        const char y = *b >= 'A' && *b <= 'Z' ? char(*b + 'a' - 'A') : *b;
        if (x != y)
            return false;
    }
    return *a == *b;
}
const LegacyWaterArea* AreaSettings(const char* name) {
    const LegacyWaterArea* found = nullptr;
    for (const auto& area : kLegacyWaterAreas) {
        if (!EqualArea(name, area.area))
            continue;
        // Multiple authored records require explicit model/material mapping;
        // never silently select the first record for an ambiguous area.
        if (found)
            return nullptr;
        found = &area;
    }
    return found;
}

bool NativeTexture(void*, uintptr_t function, uint32_t resource, uintptr_t* result) {
#if defined(_M_IX86) || defined(__i386__)
    unsigned char header[0x2c];
    if (!result || !ReadNativeWaterMemory(nullptr, resource, header, sizeof(header)))
        return false;
    using Resolver = ID3D11ShaderResourceView*(__thiscall*)(void*);
    *result = reinterpret_cast<uintptr_t>(
        reinterpret_cast<Resolver>(function)(reinterpret_cast<void*>(resource)));
    return *result != 0;
#else
    (void)function;
    (void)resource;
    (void)result;
    return false;
#endif
}

const LegacyWaterPuddle* PuddleSettings(const char* name) {
    const LegacyWaterPuddle* found = nullptr;
    for (const auto& puddle : kLegacyWaterPuddles) {
        if (!EqualArea(name, puddle.area))
            continue;
        // One SwampPuddle hook cannot tell several authored puddles apart.
        if (found)
            return nullptr;
        found = &puddle;
    }
    return found;
}

// Remove any scale from the node matrix's axis rows; the authored region
// extents size the quad. Returns false for a degenerate axis.
bool RigidWorld(WaterMatrix& world) {
    for (unsigned row = 0; row < 3; ++row) {
        const float* axis = world.row[row];
        const float length = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        if (!(length > 1e-6f) || !(length < 1e6f))
            return false;
        for (unsigned col = 0; col < 3; ++col)
            world.row[row][col] /= length;
    }
    return true;
}

WaterVector Scroll(double clock, const float rate[2]) {
    // The caller supplies the engine's accumulated gameplay clock. Wrapping in
    // double preserves fractional movement during long play sessions.
    return {static_cast<float>(fmod(clock * rate[0], 1.0)),
            static_cast<float>(fmod(clock * rate[1], 1.0)), 0, 16};
}
} // namespace

struct NativeWaterRuntime::Surface {
    NativeWaterNodeSnapshot identity = {};
    uintptr_t blob = 0;
    uint64_t environmentToken = 0;
    LegacyWaterSurface renderer;
    HeapBlock cpu;
    LegacyWaterCadence cadence; // Area water only; the puddle uses puddleCadence_.
};

NativeWaterRuntime::~NativeWaterRuntime() { Reset(); }

bool NativeWaterRuntime::Initialize(ID3D11Device* device, LegacyWaterPrograms programs,
                                    NativeMemory memory, NativeResolveTexture resolver,
                                    void* user) {
    if (device_ || !device || !memory.read || !memory.imageBase)
        return false;
    device_ = device;
    device_->AddRef();
    // Native TextureManager supplies an opaque white1x1 for a missing handle.
    // Reproduce that fallback without borrowing the manager's private wrapper.
    const uint32_t white = 0xffffffffu;
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = description.Height = description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data = {&white, 4, 4};
    ID3D11Texture2D* texture = nullptr;
    HRESULT result = device_->CreateTexture2D(&description, &data, &texture);
    if (SUCCEEDED(result))
        result = device_->CreateShaderResourceView(texture, nullptr, &whiteView_);
    if (texture)
        texture->Release();
    if (FAILED(result)) {
        Reset();
        return false;
    }
    programs_ = programs;
    memory_ = memory;
    textureResolver_ = resolver ? resolver : NativeTexture;
    textureUser_ = user;
    diagnostics_ = {};
    diagnostics_.lastStatus = NativeRuntimeStatus::Ready;
    puddleDiagnostics_ = diagnostics_;
    return true;
}

void NativeWaterRuntime::ClearSurfaces() {
    for (auto*& surface : surfaces_) {
        if (!surface)
            continue;
        surface->~Surface();
        HeapFree(GetProcessHeap(), 0, surface);
        surface = nullptr;
    }
}
void NativeWaterRuntime::ClearPuddle() {
    // Surfaces live in HeapAlloc storage, only 8-byte aligned on x86.
    static_assert(alignof(Surface) <= MEMORY_ALLOCATION_ALIGNMENT,
                  "Surface must fit HeapAlloc alignment");
    if (!puddle_)
        return;
    puddle_->~Surface();
    HeapFree(GetProcessHeap(), 0, puddle_);
    puddle_ = nullptr;
}
void NativeWaterRuntime::Reset() {
    ClearSurfaces();
    ClearPuddle();
    if (puddleDepth_)
        puddleDepth_->Release();
    puddleDepth_ = nullptr;
    puddleGeneration_ = 0;
    puddleFromScene_ = false;
    if (whiteView_)
        whiteView_->Release();
    whiteView_ = nullptr;
    if (device_)
        device_->Release();
    device_ = nullptr;
    memory_ = {};
    programs_ = {};
    textureResolver_ = nullptr;
    textureUser_ = nullptr;
    generation_ = 0;
}
bool NativeWaterRuntime::Fail(NativeRuntimeStatus status, HRESULT graphics) {
    diagnostics_.lastStatus = status;
    diagnostics_.graphicsResult = graphics;
    ++diagnostics_.fallbacks;
    return false;
}
bool NativeWaterRuntime::PuddleFail(NativeRuntimeStatus status, HRESULT graphics) {
    puddleDiagnostics_.lastStatus = status;
    puddleDiagnostics_.graphicsResult = graphics;
    ++puddleDiagnostics_.fallbacks;
    return false;
}

bool NativeWaterRuntime::Render(ID3D11DeviceContext* context,
                                const NativeWaterInvocation& invocation, uint32_t generation) {
    ++diagnostics_.attempts;
    if (!device_ || !context)
        return Fail(NativeRuntimeStatus::NotInitialized);
    if (generation != generation_) {
        ClearSurfaces();
        generation_ = generation;
    }
    NativeWaterGlobals globals = {};
    NativeWaterScene scene = {};
    const NativeWaterSceneInput input = {invocation.worldMatrix, invocation.field, 0};
    diagnostics_.area[0] = 0;
    const bool haveGlobals = ReadNativeWaterGlobals(memory_, &globals) == NativeGlobalsStatus::Ok;
    if (haveGlobals) {
        unsigned i = 0;
        for (; globals.area[i] && i + 1 < sizeof(diagnostics_.area); ++i)
            diagnostics_.area[i] = globals.area[i];
        diagnostics_.area[i] = 0;
    }
    if (!haveGlobals || ReadNativeWaterScene(memory_, input, &scene) != NativeSceneStatus::Ok)
        return Fail(NativeRuntimeStatus::SceneUnavailable);
    const LegacyWaterArea* area = AreaSettings(globals.area);
    if (!area)
        return Fail(NativeRuntimeStatus::UnsupportedArea);
    NativeWaterNodeSnapshot node = {};
    if (ReadNativeWaterNode(memory_, invocation.node, &node) != NativeFieldStatus::Ok ||
        node.proceduralField != invocation.field || node.materialResource != invocation.material)
        return Fail(NativeRuntimeStatus::MeshUnavailable);
    NativeWaterMaterial material = {};
    if (ResolveNativeWaterMaterial(memory_, static_cast<uint32_t>(invocation.material),
                                   globals.materialManager, textureResolver_, textureUser_,
                                   &material, options_.defaultEnvironment) != NativeMaterialStatus::Ok)
        return Fail(NativeRuntimeStatus::MaterialUnavailable);
    const bool overlay = ShouldRenderLegacyOverlay(static_cast<int>(material.detailType));
    const LegacyOverlayVariant variant =
        OverlayVariantForMaterial(static_cast<int>(material.detailType));
    if (overlay && variant == LegacyOverlayVariant::Unsupported)
        return Fail(NativeRuntimeStatus::UnsupportedMaterial);

    unsigned slot = 16;
    for (unsigned i = 0; i < 16; ++i) {
        Surface* found = surfaces_[i];
        if (found && found->identity.node == node.node) {
            // Native pointers are only compared while the host guarantees a
            // level epoch. Resource reuse within a level also invalidates data.
            uint32_t blob = 0;
            const bool same =
                found->identity.modelResource == node.modelResource &&
                found->identity.proceduralField == node.proceduralField &&
                found->environmentToken == globals.lifetimeToken &&
                memory_.Value(node.modelResource + NativeWaterOffsets::ModelBlob, blob) &&
                found->blob == blob;
            if (!same) {
                found->~Surface();
                HeapFree(GetProcessHeap(), 0, found);
                surfaces_[i] = nullptr;
            }
            slot = i;
            break;
        }
        if (!found && slot == 16)
            slot = i;
    }
    if (slot == 16)
        return Fail(NativeRuntimeStatus::AllocationFailed);
    if (!surfaces_[slot]) {
        size_t span = 0;
        if (QueryNativeWaterModelSpan(memory_, node.modelResource, &span) != NativeFieldStatus::Ok)
            return Fail(NativeRuntimeStatus::MeshUnavailable);
        HeapBlock capture;
        if (!capture.Allocate(span))
            return Fail(NativeRuntimeStatus::AllocationFailed);
        NativeWaterMeshInfo info = {};
        uintptr_t blob = 0;
        if (CaptureNativeWaterModelRanges(memory_, node.modelResource, capture.data, span, &blob,
                                          &info) != NativeFieldStatus::Ok)
            return Fail(NativeRuntimeStatus::MeshUnavailable);
        if (info.materialCount != 1 || !info.triangleIndexCount)
            return Fail(NativeRuntimeStatus::UnsupportedMesh);
        const auto& defaults = kOriginalWaterRegionDefaults;
        const size_t pixels = size_t(defaults.fieldSize) * defaults.fieldSize;
        const uint64_t bytes =
            uint64_t(info.vertexCount) * (sizeof(DecodedWaterVertex) + sizeof(LegacyModelPosition) +
                                          sizeof(LegacyVertexGridSample)) +
            uint64_t(info.triangleIndexCount) * sizeof(uint32_t) +
            uint64_t(info.stripCount) * sizeof(DecodedWaterStrip) +
            pixels * (3 * sizeof(float) + sizeof(uint32_t));
        if (bytes > 64u * 1024u * 1024u)
            return Fail(NativeRuntimeStatus::UnsupportedMesh);
        void* allocation = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Surface));
        if (!allocation)
            return Fail(NativeRuntimeStatus::AllocationFailed);
        Surface* created = new (allocation) Surface();
        surfaces_[slot] = created;
        if (!created->cpu.Allocate(static_cast<size_t>(bytes))) {
            created->~Surface();
            HeapFree(GetProcessHeap(), 0, created);
            surfaces_[slot] = nullptr;
            return Fail(NativeRuntimeStatus::AllocationFailed);
        }
        auto* vertices = static_cast<DecodedWaterVertex*>(created->cpu.data);
        auto* positions = reinterpret_cast<LegacyModelPosition*>(vertices + info.vertexCount);
        auto* samples = reinterpret_cast<LegacyVertexGridSample*>(positions + info.vertexCount);
        auto* indices = reinterpret_cast<uint32_t*>(samples + info.vertexCount);
        auto* strips = reinterpret_cast<DecodedWaterStrip*>(indices + info.triangleIndexCount);
        auto* field0 = reinterpret_cast<float*>(strips + info.stripCount);
        auto* field1 = field0 + pixels;
        auto* bump = reinterpret_cast<uint32_t*>(field1 + pixels);
        auto* display = reinterpret_cast<float*>(bump + pixels);
        const NativeMeshStatus decoded =
            DecodeNativeWaterMesh(capture.data, span, defaults.density, vertices, info.vertexCount,
                                  indices, info.triangleIndexCount, strips, info.stripCount, &info);
        HeapBlock cube;
        if (!cube.Allocate(kLegacyDirectionCubePixels * sizeof(uint32_t))) {
            created->~Surface();
            HeapFree(GetProcessHeap(), 0, created);
            surfaces_[slot] = nullptr;
            return Fail(NativeRuntimeStatus::AllocationFailed);
        }
        auto* cubePixels = static_cast<uint32_t*>(cube.data);
        GenerateLegacyDirectionCube(cubePixels, kLegacyDirectionCubePixels);
        LegacyWaterSurfaceSettings settings = {defaults.fieldSize,
                                               defaults.fieldSize,
                                               defaults.density,
                                               defaults.verticalness,
                                               defaults.wrap,
                                               defaults.simulation,
                                               {1, 1}};
        if (area->presentFields & HasSpeed)
            settings.simulation.speed = area->speed;
        if (area->presentFields & HasTurbulence)
            settings.simulation.turbulence = area->turbulence;
        const LegacyWaterMesh mesh = {vertices, info.vertexCount, indices, info.triangleIndexCount};
        const LegacyWaterSurfaceStorage storage = {{field0, field1},
                                                   {pixels, pixels},
                                                   positions,
                                                   info.vertexCount,
                                                   samples,
                                                   info.vertexCount,
                                                   vertices,
                                                   info.vertexCount,
                                                   bump,
                                                   pixels,
                                                   display,
                                                   pixels};
        if (decoded != NativeMeshStatus::Ok ||
            created->renderer.Initialize(
                device_, programs_, mesh, {info.boundsLow[0], info.boundsLow[1], info.boundsLow[2]},
                storage, settings, cubePixels, kLegacyDirectionCubePixels) != SurfaceStatus::Ok) {
            created->~Surface();
            HeapFree(GetProcessHeap(), 0, created);
            surfaces_[slot] = nullptr;
            return Fail(NativeRuntimeStatus::SurfaceFailed);
        }
        created->identity = node;
        created->blob = blob;
        created->environmentToken = globals.lifetimeToken;
    }
    LegacyWaterDraw draw = {};
    const float* color =
        area->presentFields & HasColor ? area->color : kOriginalWaterRegionDefaults.color;
    const LegacyWaterSceneAppearance appearance = {{color[0], color[1], color[2], color[3]},
                                                   material.reflectivity,
                                                   Scroll(scene.clock18, material.uvRate1),
                                                   Scroll(scene.clock18, material.uvRate2)};
    if (!MakeLegacyWaterSceneConstants(scene, appearance, &draw.vertex, &draw.pixel))
        return Fail(NativeRuntimeStatus::SceneUnavailable);
    draw.textures = {
        reinterpret_cast<ID3D11ShaderResourceView*>(material.environmentView),
        material.baseView ? reinterpret_cast<ID3D11ShaderResourceView*>(material.baseView)
                          : whiteView_,
        material.detailView ? reinterpret_cast<ID3D11ShaderResourceView*>(material.detailView)
                            : whiteView_};
    draw.overlay = overlay && options_.overlayEnabled;
    draw.variant = variant;
    // Original RenderSetup disables depth writes. SwampWaterNode::Render
    // enables them for the base pass only when the camera is below its surface;
    // OverlaySetup disables them again. The backend already keeps overlay off.
    draw.writeDepth = scene.cameraPosition.y < scene.world.row[3][1] + node.boundsLow[1];
    Surface* surface = surfaces_[slot];
    frameSample_ = {scene.frameNumber,
                    scene.frameDelta,
                    {scene.cameraPosition.x, scene.cameraPosition.y, scene.cameraPosition.z},
                    scene.fogNear,
                    scene.fogFar,
                    {scene.fogColor.x, scene.fogColor.y, scene.fogColor.z},
                    material.reflectivity,
                    static_cast<uint32_t>(material.environmentView),
                    static_cast<uint32_t>(material.baseView),
                    static_cast<uint32_t>(material.detailView),
                    0};
    if (options_.fixedAreaCadence) {
        constexpr float kStep = static_cast<float>(LegacyWaterCadence::kStep);
        unsigned steps = options_.freezeSimulation
                             ? 0
                             : surface->cadence.Begin(scene.frameNumber, scene.frameDelta);
        frameSample_.steps = steps;
        for (; steps; --steps) {
            if (surface->renderer.Update(context, kStep, ++surface->cadence.step) != SurfaceStatus::Ok)
                return Fail(NativeRuntimeStatus::SurfaceFailed);
        }
        // Same token: shows the frame between the last two steps without stepping.
        if (surface->renderer.Update(context, 0, surface->cadence.step, surface->cadence.Blend()) !=
            SurfaceStatus::Ok)
            return Fail(NativeRuntimeStatus::SurfaceFailed);
    } else {
        const float delta = options_.freezeSimulation ? 0.0f : scene.frameDelta;
        if (surface->renderer.Update(context, delta, scene.frameNumber, 1.0f) != SurfaceStatus::Ok)
            return Fail(NativeRuntimeStatus::SurfaceFailed);
    }
    const HRESULT result = surface->renderer.Draw(context, draw);
    if (FAILED(result))
        return Fail(NativeRuntimeStatus::DrawFailed, result);
    diagnostics_.lastStatus = NativeRuntimeStatus::Ready;
    diagnostics_.graphicsResult = S_OK;
    ++diagnostics_.replacements;
    return true;
}

void NativeWaterRuntime::BeginPuddleGeneration(uint32_t generation) {
    if (generation == puddleGeneration_)
        return;
    ClearPuddle();
    puddleGeneration_ = generation;
    puddleFromScene_ = false;
    occupantCount_ = 0;
    puddleRegion_ = {};
    regionRetry_ = 0;
    occupantsFromList_ = false;
    puddleCadence_ = {};
    puddleTime_ = 0;
}

SurfaceStatus NativeWaterRuntime::AdvancePuddle(ID3D11DeviceContext* context,
                                               const LegacyWaterPuddle& puddle,
                                               const NativeWaterScene& scene) {
    // Each fixed step is one FrameTick then SwampPuddle::Update.
    constexpr double kStep = LegacyWaterCadence::kStep;
    unsigned steps = options_.freezeSimulation ? 0 : puddleCadence_.Begin(scene.frameNumber, scene.frameDelta);
    for (; steps; --steps) {
        puddleTime_ += kStep;
        TickPuddleOccupants(puddle, scene, static_cast<float>(kStep));
        const SurfaceStatus status =
            puddle_->renderer.Update(context, static_cast<float>(kStep), ++puddleCadence_.step);
        if (status != SurfaceStatus::Ok)
            return status;
    }
    // Same token: uploads pending perturbs and the in-between frame without stepping.
    return puddle_->renderer.Update(context, 0, puddleCadence_.step, puddleCadence_.Blend());
}

namespace {
// WorldToField 0x5399d5 for a rigid row-vector world: local axis i =
// dot(p - origin, row i); u/v are extent-normalized x/z plus .5.
void PuddleLocal(const NativeWaterScene& scene, const float* position, float* local) {
    const float* origin = scene.world.row[3];
    const float offset[3] = {position[0] - origin[0], position[1] - origin[1],
                             position[2] - origin[2]};
    for (unsigned axis = 0; axis < 3; ++axis)
        local[axis] = offset[0] * scene.world.row[axis][0] + offset[1] * scene.world.row[axis][1] +
                      offset[2] * scene.world.row[axis][2];
}
} // namespace

unsigned NativeWaterRuntime::ReadOccupants(const LegacyWaterPuddle& puddle,
                                           const NativeWaterScene& scene,
                                           NativePuddleOccupant* found) {
    // The region script is cached per level; a failed search retries once a
    // second of steps rather than walking the region map every step.
    if (!ValidNativePuddleRegion(memory_, puddleRegion_)) {
        puddleRegion_ = {};
        if (regionRetry_) {
            --regionRetry_;
        } else if (!FindNativePuddleRegion(memory_, &puddleRegion_)) {
            regionRetry_ = 30;
        }
    }
    const int listed = puddleRegion_.script
                           ? ReadNativePuddleOccupants(memory_, puddleRegion_.script, found,
                                                       kMaxOccupants)
                           : -1;
    occupantsFromList_ = listed >= 0;
    if (listed >= 0)
        return static_cast<unsigned>(listed);
    // Fallback without the native list: Vince (World 0xb631ec +0x1c, position
    // +0x2c) inside the quad and near the floor (assumed window; the authored
    // plane is only .01 thick).
    uint32_t world = 0, vince = 0;
    NativePuddleOccupant& occupant = found[0];
    if (!memory_.Value(memory_.Address(0xb631ec), world) || !world ||
        !memory_.Value(uintptr_t(world) + 0x1c, vince) || !vince ||
        !memory_.Read(uintptr_t(vince) + 0x2c, occupant.position, sizeof(occupant.position)))
        return 0;
    float local[3];
    PuddleLocal(scene, occupant.position, local);
    const float u = local[0] / puddle.scale[0] + 0.5f, v = local[2] / puddle.scale[2] + 0.5f;
    if (!(u > 0 && u < 1 && v > 0 && v < 1) || local[1] < -0.5f || local[1] > 1.5f)
        return 0;
    occupant.id = kVinceFallbackId;
    occupant.isVince = true;
    occupant.critter = vince;
    return 1;
}

void NativeWaterRuntime::TickPuddleOccupants(const LegacyWaterPuddle& puddle,
                                             const NativeWaterScene& scene, float delta) {
    NativePuddleOccupant found[kMaxOccupants] = {};
    const unsigned count = ReadOccupants(puddle, scene, found);
    // PuddleRegionMain::FrameTick 0x5143c4, per occupant in list order.
    constexpr float kEntered = 3.402823466e38f; // FLT_MAX: no field position yet.
    const auto& defaults = kOriginalPuddleDefaults;
    OccupantState next[kMaxOccupants];
    unsigned kept = 0;
    for (unsigned i = 0; i < count; ++i) {
        OccupantState state = {found[i].id, kEntered, kEntered};
        for (unsigned j = 0; j < occupantCount_; ++j)
            if (occupants_[j].id == found[i].id)
                state = occupants_[j];
        float local[3];
        PuddleLocal(scene, found[i].position, local);
        const float u = local[0] / puddle.scale[0] + 0.5f, v = local[2] / puddle.scale[2] + 0.5f;
        const bool inside = u > 0 && u < 1 && v > 0 && v < 1; // ProjectAndPerturb 0x5398f6
        if (state.u == kEntered || fabsf(u - state.u) > 1e-9f || fabsf(v - state.v) > 1e-9f) {
            // Entering splashes; any movement ripples at the new position.
            if (inside)
                puddle_->renderer.Perturb(
                    u, v, (state.u == kEntered ? defaults.perturbSplash : defaults.perturbMove) * delta);
            // FrameTick then spawns `puddleSplash` and plays puddlesplash01..03
            // at ProjectAndPerturb's point: the critter on the puddle plane
            // (local y = 0), whether or not it is inside the field.
            if (state.u == kEntered && splash_) {
                const float* up = scene.world.row[1];
                const float point[3] = {found[i].position[0] - local[1] * up[0],
                                        found[i].position[1] - local[1] * up[1],
                                        found[i].position[2] - local[1] * up[2]};
                splash_(splashUser_, found[i].critter, found[i].isVince, point);
            }
            state.u = u;
            state.v = v;
        } else if (static_cast<int64_t>(puddleTime_) != static_cast<int64_t>(puddleTime_ - delta)) {
            // Standing still: a small ripple at the last position each time
            // gameplay time crosses a second (both times truncated, cvttss2si).
            puddle_->renderer.Perturb(state.u, state.v, defaults.perturbStill * delta);
        }
        next[kept++] = state;
    }
    for (unsigned i = 0; i < kept; ++i)
        occupants_[i] = next[i];
    occupantCount_ = kept;
}

bool NativeWaterRuntime::RenderPuddle(ID3D11DeviceContext* context, uintptr_t node,
                                      uintptr_t worldMatrix, uint32_t generation) {
    ++puddleDiagnostics_.attempts;
    if (!device_ || !context || !node || !worldMatrix)
        return PuddleFail(NativeRuntimeStatus::NotInitialized);
    BeginPuddleGeneration(generation);
    NativeWaterGlobals globals = {};
    NativeWaterScene scene = {};
    const NativeWaterSceneInput input = {worldMatrix, 0, 0};
    if (ReadNativeWaterGlobals(memory_, &globals) != NativeGlobalsStatus::Ok ||
        ReadNativeWaterScene(memory_, input, &scene) != NativeSceneStatus::Ok)
        return PuddleFail(NativeRuntimeStatus::SceneUnavailable);
    const LegacyWaterPuddle* puddle = PuddleSettings(globals.area);
    if (!puddle)
        return PuddleFail(NativeRuntimeStatus::UnsupportedArea);
    // Only draw where the authored entity is; another matrix means the node is
    // not the puddle we restore, or the scene layout differs from the inspection.
    const float* origin = scene.world.row[3];
    const float dx = origin[0] - puddle->location[0], dy = origin[1] - puddle->location[1],
                dz = origin[2] - puddle->location[2];
    if (dx * dx + dy * dy + dz * dz > 0.25f || !RigidWorld(scene.world))
        return PuddleFail(NativeRuntimeStatus::SceneUnavailable);
    if (!DrawPuddle(context, node, *puddle, globals.materialManager, scene))
        return false;
    puddleFromScene_ = true;
    return true;
}

bool NativeWaterRuntime::RenderAuthoredPuddle(ID3D11DeviceContext* context,
                                              uintptr_t waterWorldMatrix, uint32_t generation) {
    if (!device_ || !context || !waterWorldMatrix)
        return false;
    BeginPuddleGeneration(generation);
    if (puddleFromScene_)
        return false;
    NativeWaterGlobals globals = {};
    if (ReadNativeWaterGlobals(memory_, &globals) != NativeGlobalsStatus::Ok)
        return false;
    const LegacyWaterPuddle* puddle = PuddleSettings(globals.area);
    if (!puddle)
        return false;
    ++puddleDiagnostics_.attempts;
    NativeWaterScene scene = {};
    const NativeWaterSceneInput input = {waterWorldMatrix, 0, 0};
    if (ReadNativeWaterScene(memory_, input, &scene) != NativeSceneStatus::Ok)
        return PuddleFail(NativeRuntimeStatus::SceneUnavailable);
    // Region node transform as the engine builds it (quaternion -> rows at
    // 0x420890, then translation); the region Scale is extents, not transform.
    const float x = puddle->rotation[0], y = puddle->rotation[1], z = puddle->rotation[2],
                w = puddle->rotation[3];
    const WaterMatrix world = {{{1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y), 0},
                                {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x), 0},
                                {2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y), 0},
                                {puddle->location[0], puddle->location[1], puddle->location[2], 1}}};
    scene.world = world;
    if (!RigidWorld(scene.world))
        return PuddleFail(NativeRuntimeStatus::SceneUnavailable);
    return DrawPuddle(context, kAuthoredPuddleIdentity, *puddle, globals.materialManager, scene);
}

bool NativeWaterRuntime::DrawPuddle(ID3D11DeviceContext* context, uintptr_t node,
                                    const LegacyWaterPuddle& authored, uint32_t materialManager,
                                    NativeWaterScene& scene) {
    const LegacyWaterPuddle* puddle = &authored;
    const float* origin = scene.world.row[3];
    // Retail material 4 is empty, so its reflectivity loads as zero and no
    // overlay is drawn. The level-captured cube stands in for the original
    // puddle's own 128px capture; the base pass blends by
    // lerp(reflectivity, 1, grazing), so it still shows at shallow angles.
    NativeWaterMaterial material = {};
    if (ResolveNativeWaterMaterial(memory_, 0, materialManager, textureResolver_,
                                   textureUser_, &material, options_.defaultEnvironment) !=
            NativeMaterialStatus::Ok ||
        !material.environmentView)
        return PuddleFail(NativeRuntimeStatus::MaterialUnavailable);
    if (!puddleDepth_) {
        // SwampPuddle::Render enables depth writes; no native stencil mask applies.
        D3D11_DEPTH_STENCIL_DESC depth = {};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        const HRESULT result = device_->CreateDepthStencilState(&depth, &puddleDepth_);
        if (FAILED(result))
            return PuddleFail(NativeRuntimeStatus::AllocationFailed, result);
    }
    if (puddle_ && puddle_->identity.node != node)
        ClearPuddle();
    if (!puddle_) {
        const auto& defaults = kOriginalPuddleDefaults;
        const size_t pixels = size_t(puddle->fieldSize) * puddle->fieldSize;
        const size_t bytes =
            4 * (sizeof(DecodedWaterVertex) + sizeof(LegacyModelPosition) +
                 sizeof(LegacyVertexGridSample)) +
            pixels * (3 * sizeof(float) + sizeof(uint32_t));
        void* allocation = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Surface));
        if (!allocation)
            return PuddleFail(NativeRuntimeStatus::AllocationFailed);
        puddle_ = new (allocation) Surface();
        HeapBlock cube;
        if (!puddle_->cpu.Allocate(bytes) ||
            !cube.Allocate(kLegacyDirectionCubePixels * sizeof(uint32_t))) {
            ClearPuddle();
            return PuddleFail(NativeRuntimeStatus::AllocationFailed);
        }
        auto* vertices = static_cast<DecodedWaterVertex*>(puddle_->cpu.data);
        auto* positions = reinterpret_cast<LegacyModelPosition*>(vertices + 4);
        auto* samples = reinterpret_cast<LegacyVertexGridSample*>(positions + 4);
        auto* field0 = reinterpret_cast<float*>(samples + 4);
        auto* field1 = field0 + pixels;
        auto* bump = reinterpret_cast<uint32_t*>(field1 + pixels);
        auto* display = reinterpret_cast<float*>(bump + pixels);
        // Original corner order and UVs. The packed texcoord 0x7ff is a
        // normalized short; the overlay VS later multiplies it by 16.
        const float hx = puddle->scale[0] * defaults.quadScale;
        const float hz = puddle->scale[2] * defaults.quadScale;
        const float d = defaults.density, packed = 2047.0f / 32767.0f;
        const float corners[4][2] = {{-1, -1}, {-1, 1}, {1, 1}, {1, -1}};
        for (unsigned i = 0; i < 4; ++i) {
            const float u = corners[i][0] > 0 ? 1.0f : 0.0f, v = corners[i][1] > 0 ? 1.0f : 0.0f;
            vertices[i] = {{corners[i][0] * hx, 0, corners[i][1] * hz}, {0, 1, 0},
                           {u * d, v * d}, {u * packed, v * packed}, {0, 0}};
        }
        static const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
        auto* cubePixels = static_cast<uint32_t*>(cube.data);
        GenerateLegacyDirectionCube(cubePixels, kLegacyDirectionCubePixels);
        const LegacyWaterSurfaceSettings settings = {puddle->fieldSize, puddle->fieldSize,
                                                     defaults.density,  defaults.verticalness,
                                                     defaults.wrap,     defaults.simulation,
                                                     {1, 1},            false};
        const LegacyWaterMesh mesh = {vertices, 4, indices, 6};
        const LegacyWaterSurfaceStorage storage = {{field0, field1}, {pixels, pixels},
                                                   positions,        4,
                                                   samples,          4,
                                                   vertices,         4,
                                                   bump,             pixels,
                                                   display,          pixels};
        if (puddle_->renderer.Initialize(device_, programs_, mesh, {-hx, 0, -hz}, storage, settings,
                                         cubePixels, kLegacyDirectionCubePixels) !=
            SurfaceStatus::Ok) {
            ClearPuddle();
            return PuddleFail(NativeRuntimeStatus::SurfaceFailed);
        }
        puddle_->identity.node = node;
    }
    LegacyWaterDraw draw = {};
    const LegacyWaterSceneAppearance appearance = {
        {puddle->color[0], puddle->color[1], puddle->color[2], puddle->color[3]},
        kOriginalPuddleDefaults.reflectivity, Scroll(scene.clock18, material.uvRate1),
        Scroll(scene.clock18, material.uvRate2)};
    if (!MakeLegacyWaterSceneConstants(scene, appearance, &draw.vertex, &draw.pixel))
        return PuddleFail(NativeRuntimeStatus::SceneUnavailable);
    draw.textures = {reinterpret_cast<ID3D11ShaderResourceView*>(material.environmentView),
                     whiteView_, whiteView_};
    draw.writeDepth = true;
    draw.baseDepth = puddleDepth_;
    if (AdvancePuddle(context, *puddle, scene) != SurfaceStatus::Ok)
        return PuddleFail(NativeRuntimeStatus::SurfaceFailed);
    const HRESULT result = puddle_->renderer.Draw(context, draw);
    if (FAILED(result))
        return PuddleFail(NativeRuntimeStatus::DrawFailed, result);
    puddlePosition_[0] = origin[0];
    puddlePosition_[1] = origin[1];
    puddlePosition_[2] = origin[2];
    puddleDiagnostics_.lastStatus = NativeRuntimeStatus::Ready;
    puddleDiagnostics_.graphicsResult = S_OK;
    ++puddleDiagnostics_.replacements;
    return true;
}

} // namespace water
} // namespace vince
