#pragma once
#include "LegacyWaterSurface.h"
#include "NativeWaterHooks.h"
#include "NativeWaterMemory.h"
#include "NativeWaterMaterials.h"
#include "NativeWaterOccupants.h"

namespace vince {
namespace water {

struct LegacyWaterPuddle;
struct NativeWaterScene;

enum class NativeRuntimeStatus {
    Ready,
    NotInitialized,
    UnsupportedArea,
    SceneUnavailable,
    MaterialUnavailable,
    UnsupportedMaterial,
    MeshUnavailable,
    UnsupportedMesh,
    AllocationFailed,
    SurfaceFailed,
    DrawFailed
};
// A critter entered the puddle: the host plays the game's own splash for
// critter (native address) at a world-space point on the puddle plane. Called
// on the render thread during a puddle step; the host must defer any native
// scene changes until the scene is not being drawn.
using NativePuddleSplashCallback = void (*)(void* user, uint32_t critter, bool isVince,
                                            const float position[3]);

struct NativeWaterDiagnostics {
    uint64_t attempts, replacements, fallbacks;
    NativeRuntimeStatus lastStatus;
    HRESULT graphicsResult;
    char area[32]; // Config area of the last attempt; empty when it was unreadable.
};

// Per-draw scene inputs of the last area-water replacement, for loader
// diagnostics (plain floats: see the alignment note on NativeWaterRuntime).
struct NativeWaterFrameSample {
    uint32_t frame;
    float delta;
    float camera[3];
    float fogNear, fogFar, fogColor[3];
    float reflectivity;
    uint32_t environmentView, baseView, detailView;
    uint32_t steps;
};

struct NativeWaterRuntimeOptions {
    bool freezeSimulation = false;
    bool overlayEnabled = true;
    // Area water steps at the Xbox's fixed 30 Hz like the puddle; false keeps
    // the v7.6 one-step-per-frame behaviour for comparison builds.
    bool fixedAreaCadence = true;
    // Native texture resource replacing the level-capture fallback reflection
    // (MaterialManager +0x4c) for water, e.g. a retail 64-pixel capture; 0 = native.
    uint32_t defaultEnvironment = 0;
};

// The Xbox build presents every second vblank (30 fps) with variable dt, and
// the field's damping/propagation are per step, so PC frame rates shrink and
// shorten its ripples. Replays fixed 1/30 steps, at most once per game frame.
struct LegacyWaterCadence {
    static constexpr double kStep = 1.0 / 30.0;
    uint32_t frame = 0;
    double accumulator = 0;
    uint64_t step = 0; // Surface token: one per fixed step.
    // Steps to take this frame (0 on a repeated pass of the same frame).
    unsigned Begin(uint32_t frameNumber, float frameDelta) {
        if (frame == frameNumber)
            return 0;
        frame = frameNumber;
        // SwampInput::Update clamps each frame to 0.1 s.
        accumulator += frameDelta < 0 ? 0 : frameDelta > 0.1f ? 0.1 : frameDelta;
        unsigned steps = 0;
        for (; accumulator >= kStep && steps < 3; ++steps)
            accumulator -= kStep;
        if (accumulator > kStep)
            accumulator = kStep; // Never replay a long stall.
        return steps;
    }
    // Display position between the previous and latest step: shown one step
    // behind so it interpolates, never extrapolates.
    float Blend() const { return static_cast<float>(accumulator / kStep); }
};

// Optional Steam adapter around the reusable renderer. Shader bytecode storage
// must outlive this object. The host calls Reset on a quiescent render thread
// before releasing its device and passes a NEW generation on EVERY level load,
// including reloads of the same area. It never modifies native objects/configs.
class NativeWaterRuntime {
  public:
    NativeWaterRuntime() = default;
    ~NativeWaterRuntime();
    NativeWaterRuntime(const NativeWaterRuntime&) = delete;
    NativeWaterRuntime& operator=(const NativeWaterRuntime&) = delete;
    bool Initialize(ID3D11Device* device, LegacyWaterPrograms programs, NativeMemory memory,
                    NativeResolveTexture textureResolver = nullptr, void* textureUser = nullptr);
    void Reset();
    bool Render(ID3D11DeviceContext* context, const NativeWaterInvocation& invocation,
                uint32_t levelGeneration);
    // Draws a restored authored puddle at the SwampPuddle render call; there is
    // no native draw to replace.
    bool RenderPuddle(ID3D11DeviceContext* context, uintptr_t node, uintptr_t worldMatrix,
                      uint32_t levelGeneration);
    // Fallback while the scene has not called the puddle's render slot in this
    // level: call after a successful area-water Render with the same matrix.
    // Draws the area's authored puddle from its authored transform.
    bool RenderAuthoredPuddle(ID3D11DeviceContext* context, uintptr_t waterWorldMatrix,
                              uint32_t levelGeneration);
    bool PuddleDrawnByScene() const { return puddleFromScene_; }
    // Fixed-cadence field steps taken in the current level (diagnostics/tests).
    uint64_t PuddleSteps() const { return puddleCadence_.step; }
    // Whether the last puddle step read the game's own occupant list.
    bool PuddleOccupantsFromList() const { return occupantsFromList_; }
    const NativeWaterDiagnostics& Diagnostics() const { return diagnostics_; }
    const NativeWaterFrameSample& LastFrameSample() const { return frameSample_; }
    const NativeWaterDiagnostics& PuddleDiagnostics() const { return puddleDiagnostics_; }
    // Translation of the last drawn puddle matrix, for loader diagnostics.
    WaterVector PuddlePosition() const {
        return {puddlePosition_[0], puddlePosition_[1], puddlePosition_[2], 1};
    }
    void SetOptions(NativeWaterRuntimeOptions options) { options_ = options; }
    void SetPuddleSplash(NativePuddleSplashCallback callback, void* user) {
        splash_ = callback;
        splashUser_ = user;
    }

  private:
    struct Surface;
    Surface* surfaces_[16] = {};
    ID3D11Device* device_ = nullptr;
    ID3D11ShaderResourceView* whiteView_ = nullptr;
    LegacyWaterPrograms programs_ = {};
    NativeMemory memory_ = {};
    NativeWaterDiagnostics diagnostics_ = {};
    NativeWaterFrameSample frameSample_ = {};
    uint32_t generation_ = 0;
    NativeResolveTexture textureResolver_ = nullptr;
    void* textureUser_ = nullptr;
    NativeWaterRuntimeOptions options_ = {};
    Surface* puddle_ = nullptr;
    uint32_t puddleGeneration_ = 0;
    ID3D11DepthStencilState* puddleDepth_ = nullptr;
    NativeWaterDiagnostics puddleDiagnostics_ = {};
    // Plain floats: hosts place this object in HeapAlloc memory, which is only
    // 8-byte aligned on x86, so no member may require 16-byte alignment.
    float puddlePosition_[3] = {};
    bool puddleFromScene_ = false;
    // PuddleRegionMain::FrameTick state per native occupant entry id (last
    // field u/v, FLT_MAX = just entered). Without a readable native list the
    // runtime falls back to Vince alone (id kVinceFallbackId).
    struct OccupantState {
        uint32_t id;
        float u, v;
    };
    static constexpr unsigned kMaxOccupants = 8;
    static constexpr uint32_t kVinceFallbackId = 0xffffffffu;
    OccupantState occupants_[kMaxOccupants] = {};
    unsigned occupantCount_ = 0;
    NativePuddleRegion puddleRegion_ = {};
    unsigned regionRetry_ = 0;
    bool occupantsFromList_ = false;
    LegacyWaterCadence puddleCadence_ = {};
    double puddleTime_ = 0;
    NativePuddleSplashCallback splash_ = nullptr;
    void* splashUser_ = nullptr;
    SurfaceStatus AdvancePuddle(ID3D11DeviceContext* context, const LegacyWaterPuddle& puddle,
                                const NativeWaterScene& scene);
    void TickPuddleOccupants(const LegacyWaterPuddle& puddle, const NativeWaterScene& scene,
                             float delta);
    unsigned ReadOccupants(const LegacyWaterPuddle& puddle, const NativeWaterScene& scene,
                           NativePuddleOccupant* found);
    static constexpr uintptr_t kAuthoredPuddleIdentity = 1; // Not a native node.
    void ClearSurfaces();
    void ClearPuddle();
    void BeginPuddleGeneration(uint32_t generation);
    bool DrawPuddle(ID3D11DeviceContext* context, uintptr_t identity,
                    const LegacyWaterPuddle& puddle, uint32_t materialManager,
                    NativeWaterScene& scene);
    bool Fail(NativeRuntimeStatus status, HRESULT graphics = S_OK);
    bool PuddleFail(NativeRuntimeStatus status, HRESULT graphics = S_OK);
};

} // namespace water
} // namespace vince
