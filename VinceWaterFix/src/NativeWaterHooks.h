#pragma once
#include <d3d11.h>
#include <stdint.h>

namespace vince {
namespace water {

struct NativeWaterInvocation {
    uintptr_t node;
    uintptr_t field;
    uintptr_t worldMatrix;
    uintptr_t material;
    UINT indexCount;
    UINT startIndex;
    INT baseVertex;
};

// Return true only after a replacement draw succeeds. False preserves the
// original draw. The hook scopes the original node/field calls and always lets
// their ownership, texture upload and cleanup code finish normally.
using NativeWaterDrawCallback = bool (*)(void* user, ID3D11DeviceContext* context,
                                         const NativeWaterInvocation& invocation);

enum class NativeHookStatus {
    Ok,
    AlreadyInstalled,
    InvalidArgument,
    UnsupportedExecutable,
    UnsupportedArchitecture,
    HookLibraryFailure,
    HookCreationFailure,
    HookEnableFailure
};

// Singleton glue for the inspected Steam PE32 build. Call outside DllMain,
// before rendering starts, on the owning render thread. Context must be the
// game's immediate context. Only our four target hooks are modified.
NativeHookStatus InstallNativeWaterHooks(uintptr_t imageBase, ID3D11DeviceContext* context,
                                         NativeWaterDrawCallback callback, void* user);

// Remastered keeps the cut SwampPuddle node in the scene but its render slot
// is the shared `ret 4` stub. node is that node; worldMatrix is the queued
// 64-byte render matrix the native scene passes to every node render.
using NativePuddleRenderCallback = void (*)(void* user, ID3D11DeviceContext* context,
                                            uintptr_t node, uintptr_t worldMatrix);

// Call after InstallNativeWaterHooks succeeds, on the same thread. Replaces one
// SwampPuddle vtable entry only after verifying it still holds the native stub
// and PuddleRegionMain::Load still installs that vtable. afterWaterNode (may be
// null) runs after every water node render on the render thread, with that
// node and the matrix the dedicated water pass (0x4a3724, renderer +0x164)
// passed: Remastered draws only that one water node there and nothing ever
// renders the puddle node, so a host can draw the puddle at this point.
NativeHookStatus InstallNativePuddleHook(NativePuddleRenderCallback callback, void* user,
                                         NativePuddleRenderCallback afterWaterNode = nullptr);

// Level-load reflection capture: LevelLoader calls 0x48c550(handle*, position,
// size) at 0x4f9463 with (0,1000,0) and 256; retail Xbox used 64. The hook
// lets the game's capture be made at 64 (everything that falls back to it, as
// on retail) and also makes a separate 64-pixel capture for the water, owned
// by this module (one reference, released at the next level load).
enum class NativeCaptureSize { Remastered, Retail };
using NativeCaptureSizeCallback = NativeCaptureSize (*)(void* user);
// Call after InstallNativeWaterHooks succeeds, on the same thread.
NativeHookStatus InstallNativeEnvironmentCaptureHook(NativeCaptureSizeCallback gameCapture,
                                                     void* user);
// The water's own 64-pixel capture of the current level (a native texture
// resource for the material resolver), or 0 before the first capture.
uint32_t NativeWaterEnvironmentCapture();

// Puddle splash (preview PuddleRegionMain::FrameTick 0x5144d0): the game's own
// one-shot `puddleSplash` particle and a random puddlesplash01..03 sound, whose
// assets Remastered still loads in the sewers. Remastered removed only the
// caller. The functions are the ones WaterRegion's `vinceWaterSplash` still
// uses (0x538358 SoundManager::PlaySound, 0x538391 SwampParticleManager::
// AddOneShot). Queued splashes play from a hook on the game's per-frame
// function 0x50baa0, at the start of the next frame and outside the scene
// draw, on the render thread. Call after InstallNativeWaterHooks succeeds.
NativeHookStatus InstallNativePuddleSplashHook();
// Render thread. Dropped when the level generation changes before playing.
void QueueNativePuddleSplash(uint32_t critter, bool isVince, const float position[3]);
// Splashes played so far, for loader diagnostics.
uint32_t NativePuddleSplashesPlayed();

#if VINCE_WATER_DIAGNOSTIC_KEYS
// Comparison builds only: FanBoatWake::Render (0x555970, vtable 0x6e4128 slot
// 7) skips its draw while hidden(user) returns true, so a test can show the
// game with and without its speed-dependent boat wake. Call after
// InstallNativeWaterHooks succeeds, on the same thread.
NativeHookStatus InstallNativeWakeSwitch(bool (*hidden)(void* user), void* user);
#endif

// Loader diagnostics: how often the scene called the patched slot, how many
// calls were refused, and whether the vtable entry still points at the hook.
struct NativePuddleHookCounters {
    uint32_t calls, foreignThread, nested;
    bool slotIsHook;
    uint32_t waterRenderCallers[6]; // Process addresses, first water node render.
};
NativePuddleHookCounters NativePuddleHookState();

// The loader must first stop rendering/callbacks. Never unload this code while
// a native draw is in flight and never call this from DllMain.
void RemoveNativeWaterHooks();

// Atomic configuration-load generation, incremented after every config reload
// including same-area reloads and loads on a worker thread. Use it to invalidate
// render caches on the render thread; this getter performs no GPU operations.
uint32_t NativeWaterLevelGeneration();

// Local-process implementation of NativeReadMemory: validates every page before
// copying, rejects guard/no-access pages and arithmetic wrap. No native writes.
bool ReadNativeWaterMemory(void* user, uintptr_t address, void* output, size_t bytes);

} // namespace water
} // namespace vince
