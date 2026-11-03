#include "NativeWaterHooks.h"
#include "NativeWaterMemory.h"
#include "MinHook.h"
#include <stddef.h>
#include <string.h>

namespace vince {
namespace water {

#if defined(_M_IX86) || defined(__i386__)
namespace {
using NodeRender = void(__thiscall*)(void*, const void*);
using FieldDraw = void(__thiscall*)(void*, const void*, void*);
using ConfigLoad = uintptr_t(__thiscall*)(void*);
using DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using CaptureEnvironment = void(__cdecl*)(uint32_t*, const float*, uint32_t);
using FrameFunction = int(__cdecl*)();
using StringConstruct = void*(__thiscall*)(void*, const char*);
using StringDestroy = void(__thiscall*)(void*);
using AddOneShot = void*(__thiscall*)(void*, const void*, const float*);
// SoundManager::PlaySound(name, flags, source node, bool, float, float).
using PlaySound = void(__cdecl*)(const void*, uint32_t, uint32_t, uint32_t, float, float);
struct HookState {
    void* targets[5]; // Four water hooks, then the optional capture hook.
    unsigned created;
    NodeRender nodeOriginal;
    FieldDraw fieldOriginal;
    DrawIndexed drawOriginal;
    ConfigLoad configOriginal;
    ID3D11DeviceContext* context;
    NativeWaterDrawCallback callback;
    void* user;
    DWORD renderThread;
    NativeWaterInvocation active;
    bool inReplacement;
    void** puddleSlot;
    void* puddleOriginal;
    CaptureEnvironment captureOriginal;
    NativeCaptureSizeCallback captureSize;
    void* captureUser;
    uintptr_t levelLoadReturn;
    NativePuddleRenderCallback puddleCallback;
    NativePuddleRenderCallback afterWaterNode;
    void* puddleUser;
    void* frameTarget;
    FrameFunction frameOriginal;
    StringConstruct stringConstruct;
    StringDestroy stringDestroy;
    AddOneShot addOneShot;
    PlaySound playSound;
    uintptr_t particleManager; // Address of the SwampParticleManager pointer.
#if VINCE_WATER_DIAGNOSTIC_KEYS
    void** wakeSlot;
    NodeRender wakeOriginal;
    bool (*wakeHidden)(void*);
    void* wakeUser;
#endif
};
HookState state = {};
volatile LONG levelGeneration = 0;
volatile LONG puddleCalls = 0, puddleForeignThread = 0, puddleNested = 0;

uintptr_t __fastcall LoadConfig(void* area, void*) {
    // Native46b780 consumes ECX only and returns with plain RET. Preserve EAX
    // across the atomic notification even though its inspected caller ignores it.
    const uintptr_t result = state.configOriginal(area);
    InterlockedIncrement(&levelGeneration);
    return result;
}

// Return addresses of the first water node render: the game's EBP chain from
// RenderNode's own frame, read with page checks (puddle draw-path diagnostics).
uint32_t renderCallers[6] = {};
volatile LONG renderCallersCaptured = 0;
void CaptureRenderCallers(uintptr_t frame) {
    for (unsigned i = 0; i < 6 && frame; ++i) {
        uint32_t saved[2] = {};
        if (!ReadNativeWaterMemory(nullptr, frame, saved, sizeof(saved)) || saved[0] <= frame)
            break;
        renderCallers[i] = saved[1];
        frame = saved[0];
    }
}

void __fastcall RenderNode(void* node, void*, const void* world) {
    if (GetCurrentThreadId() != state.renderThread) {
        state.nodeOriginal(node, world);
        return;
    }
    if (!InterlockedCompareExchange(&renderCallersCaptured, 1, 0))
        CaptureRenderCallers(reinterpret_cast<uintptr_t>(__builtin_frame_address(0)));
    const NativeWaterInvocation previous = state.active;
    state.active = {};
    state.active.node = reinterpret_cast<uintptr_t>(node);
    state.nodeOriginal(node, world);
    state.active = previous;
    if (state.afterWaterNode && !state.inReplacement && !previous.node) {
        state.inReplacement = true;
        state.afterWaterNode(state.puddleUser, state.context, reinterpret_cast<uintptr_t>(node),
                             reinterpret_cast<uintptr_t>(world));
        state.inReplacement = false;
    }
}
void __fastcall RenderField(void* field, void*, const void* world, void* material) {
    if (GetCurrentThreadId() != state.renderThread || !state.active.node) {
        state.fieldOriginal(field, world, material);
        return;
    }
    const NativeWaterInvocation previous = state.active;
    state.active.field = reinterpret_cast<uintptr_t>(field);
    state.active.worldMatrix = reinterpret_cast<uintptr_t>(world);
    state.active.material = reinterpret_cast<uintptr_t>(material);
    state.fieldOriginal(field, world, material);
    state.active = previous;
}
void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* context, UINT count, UINT start, INT base) {
    if (context == state.context && GetCurrentThreadId() == state.renderThread &&
        state.active.node && state.active.field && !state.inReplacement) {
        NativeWaterInvocation invocation = state.active;
        invocation.indexCount = count;
        invocation.startIndex = start;
        invocation.baseVertex = base;
        state.inReplacement = true;
        const bool replaced = state.callback(state.user, context, invocation);
        state.inReplacement = false;
        if (replaced)
            return;
    }
    state.drawOriginal(context, count, start, base);
}

// SwampPuddle vtable+0x1c, called by the render queue at 0x428593 as
// __thiscall(node, queued entry) with `ret 4`; the entry starts with its matrix.
void __fastcall RenderPuddle(void* node, void*, const void* world) {
    InterlockedIncrement(&puddleCalls);
    if (GetCurrentThreadId() != state.renderThread || !state.puddleCallback) {
        InterlockedIncrement(&puddleForeignThread);
        return;
    }
    if (state.inReplacement || state.active.node) {
        InterlockedIncrement(&puddleNested);
        return;
    }
    state.inReplacement = true;
    state.puddleCallback(state.puddleUser, state.context, reinterpret_cast<uintptr_t>(node),
                         reinterpret_cast<uintptr_t>(world));
    state.inReplacement = false;
}

// The water's own capture; one native reference owned here (resource
// refcount at +0, released with `lock dec` like LevelLoader's handle).
volatile LONG waterCapture = 0;

void __cdecl CaptureLevelEnvironment(uint32_t* handle, const float* position, uint32_t size) {
    if (reinterpret_cast<uintptr_t>(__builtin_return_address(0)) != state.levelLoadReturn) {
        state.captureOriginal(handle, position, size);
        return;
    }
    const bool retail = state.captureSize(state.captureUser) == NativeCaptureSize::Retail;
    state.captureOriginal(handle, position, retail ? 64u : size);
    const LONG previous = InterlockedExchange(&waterCapture, 0);
    if (previous)
        InterlockedDecrement(reinterpret_cast<volatile LONG*>(static_cast<uintptr_t>(previous)));
    uint32_t water = 0;
    state.captureOriginal(&water, position, 64);
    InterlockedExchange(&waterCapture, static_cast<LONG>(water));
}

struct PendingSplash {
    uint32_t critter, flags;
    float position[3];
};
PendingSplash pendingSplashes[8] = {};
unsigned pendingSplashCount = 0;
LONG pendingSplashGeneration = 0;
volatile LONG splashesPlayed = 0;
uint32_t splashRandom = 0;

// MSVC std::string as the game builds it (24 bytes); the game's own
// constructor and destructor own its layout.
struct alignas(4) NativeString {
    unsigned char bytes[24];
};

int __cdecl RunFrame() {
    if (pendingSplashCount && GetCurrentThreadId() == state.renderThread) {
        const unsigned count = pendingSplashCount;
        pendingSplashCount = 0;
        uint32_t manager = 0;
        // A level change between queueing and now may have freed the critters.
        if (pendingSplashGeneration == InterlockedCompareExchange(&levelGeneration, 0, 0) &&
            ReadNativeWaterMemory(nullptr, state.particleManager, &manager, sizeof(manager))) {
            for (unsigned i = 0; i < count; ++i) {
                const PendingSplash& splash = pendingSplashes[i];
                NativeString name;
                if (manager) {
                    state.stringConstruct(&name, "puddleSplash");
                    state.addOneShot(reinterpret_cast<void*>(static_cast<uintptr_t>(manager)),
                                     &name, splash.position);
                    state.stringDestroy(&name);
                }
                // BeepLib::Random() % 3 + 1; any uniform source gives the same choice.
                splashRandom = splashRandom * 1664525u + 1013904223u + GetTickCount();
                char sound[] = "puddlesplash00";
                sound[13] = static_cast<char>('1' + (splashRandom >> 16) % 3);
                state.stringConstruct(&name, sound);
                state.playSound(&name, splash.flags, splash.critter, 1, 0.0f, 0.0f);
                state.stringDestroy(&name);
                InterlockedIncrement(&splashesPlayed);
            }
        }
    }
    return state.frameOriginal();
}

#if VINCE_WATER_DIAGNOSTIC_KEYS
void __fastcall RenderWake(void* node, void*, const void* world) {
    if (state.wakeHidden && state.wakeHidden(state.wakeUser))
        return;
    state.wakeOriginal(node, world);
}
#endif

bool WriteSlot(void** slot, void* value) {
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection))
        return false;
    InterlockedExchangePointer(slot, value);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), protection, &ignored);
    return true;
}

bool SupportedImage(uintptr_t base) {
    NativeMemory memory = {ReadNativeWaterMemory, nullptr, base};
    IMAGE_DOS_HEADER dos = {};
    if (!memory.Value(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        dos.e_lfanew > 0x1000)
        return false;
    IMAGE_NT_HEADERS32 pe = {};
    if (!memory.Value(base + static_cast<uint32_t>(dos.e_lfanew), pe) ||
        pe.Signature != IMAGE_NT_SIGNATURE || pe.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        pe.FileHeader.TimeDateStamp != 0x5996a9bb ||
        pe.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        pe.OptionalHeader.SizeOfImage < 0x87a000)
        return false;
    // Other community patches append sections and grow SizeOfImage. Accept that
    // only when the inspected build's seven sections keep their exact layout.
    struct StockSection {
        char name[IMAGE_SIZEOF_SHORT_NAME];
        uint32_t address, size;
    };
    static const StockSection stock[] = {
        {".text", 0x1000, 0x2d4ad8},   {".rdata", 0x2d6000, 0x477e26},
        {".data", 0x74e000, 0x99ca4},  {".gfids", 0x7e8000, 0x2c},
        {".tls", 0x7e9000, 0x9},       {".rsrc", 0x7ea000, 0x50848},
        {".reloc", 0x83b000, 0x3e9b8}};
    const unsigned sections = pe.FileHeader.NumberOfSections;
    if (sections < 7 || sections > 96 ||
        (sections == 7) != (pe.OptionalHeader.SizeOfImage == 0x87a000))
        return false;
    const uintptr_t table = base + static_cast<uint32_t>(dos.e_lfanew) +
                            offsetof(IMAGE_NT_HEADERS32, OptionalHeader) +
                            pe.FileHeader.SizeOfOptionalHeader;
    for (unsigned i = 0; i < sections; ++i) {
        IMAGE_SECTION_HEADER section = {};
        if (!memory.Value(table + i * sizeof(section), section))
            return false;
        if (i >= 7) {
            if (section.VirtualAddress < 0x87a000)
                return false;
            continue;
        }
        if (memcmp(section.Name, stock[i].name, sizeof(section.Name)) ||
            section.VirtualAddress != stock[i].address ||
            section.Misc.VirtualSize != stock[i].size)
            return false;
    }
    // Stop before the relocated SEH handler pointer, so ASLR remains supported.
    const unsigned char node[] = {0x55, 0x8b, 0xec, 0x64, 0xa1, 0, 0, 0, 0, 0x6a, 0xff, 0x68};
    const unsigned char field[] = {0x53, 0x8b, 0xdc, 0x83, 0xec, 8,    0x83, 0xe4,
                                   0xf8, 0x83, 0xc4, 4,    0x55, 0x8b, 0x6b, 4};
    const unsigned char config[] = {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68};
    const unsigned char dxbc[] = {0x44, 0x58, 0x42, 0x43, 0xe9, 0x98, 0xb5, 0x5d, 0x04, 0x39,
                                  0x79, 0xa0, 0x79, 0x30, 0x1d, 0x96, 0xaf, 0xa3, 0x02, 0x87};
    unsigned char actual[sizeof(dxbc)];
    return memory.Read(memory.Address(0x49bc40), actual, sizeof(node)) &&
           !memcmp(actual, node, sizeof(node)) &&
           memory.Read(memory.Address(0x4b5830), actual, sizeof(field)) &&
           !memcmp(actual, field, sizeof(field)) &&
           memory.Read(memory.Address(0x46b780), actual, sizeof(config)) &&
           !memcmp(actual, config, sizeof(config)) &&
           memory.Read(memory.Address(0x9400c0), actual, sizeof(dxbc)) &&
           !memcmp(actual, dxbc, sizeof(dxbc));
}
} // namespace
#endif

NativeHookStatus InstallNativeWaterHooks(uintptr_t base, ID3D11DeviceContext* context,
                                         NativeWaterDrawCallback callback, void* user) {
#if defined(_M_IX86) || defined(__i386__)
    if (state.context)
        return NativeHookStatus::AlreadyInstalled;
    if (!base || !context || !callback || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return NativeHookStatus::InvalidArgument;
    if (!SupportedImage(base))
        return NativeHookStatus::UnsupportedExecutable;
    const MH_STATUS initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
        return NativeHookStatus::HookLibraryFailure;
    state.targets[0] = reinterpret_cast<void*>(base + 0x9bc40);
    state.targets[1] = reinterpret_cast<void*>(base + 0xb5830);
    state.targets[2] = (*reinterpret_cast<void***>(context))[12];
    state.targets[3] = reinterpret_cast<void*>(base + 0x6b780);
    void* detours[] = {reinterpret_cast<void*>(&RenderNode), reinterpret_cast<void*>(&RenderField),
                       reinterpret_cast<void*>(&Draw), reinterpret_cast<void*>(&LoadConfig)};
    void** originals[] = {reinterpret_cast<void**>(&state.nodeOriginal),
                          reinterpret_cast<void**>(&state.fieldOriginal),
                          reinterpret_cast<void**>(&state.drawOriginal),
                          reinterpret_cast<void**>(&state.configOriginal)};
    for (unsigned i = 0; i < 4; ++i) {
        if (MH_CreateHook(state.targets[i], detours[i], originals[i]) != MH_OK) {
            RemoveNativeWaterHooks();
            return NativeHookStatus::HookCreationFailure;
        }
        ++state.created;
    }
    state.context = context;
    context->AddRef();
    state.callback = callback;
    state.user = user;
    state.renderThread = GetCurrentThreadId();
    InterlockedIncrement(&levelGeneration);
    // Enable each owned target; never apply somebody else's queued MinHook work.
    for (unsigned i = 0; i < 4; ++i) {
        if (MH_EnableHook(state.targets[i]) != MH_OK) {
            RemoveNativeWaterHooks();
            return NativeHookStatus::HookEnableFailure;
        }
    }
    return NativeHookStatus::Ok;
#else
    (void)base;
    (void)context;
    (void)callback;
    (void)user;
    return NativeHookStatus::UnsupportedArchitecture;
#endif
}

NativeHookStatus InstallNativePuddleHook(NativePuddleRenderCallback callback, void* user,
                                         NativePuddleRenderCallback afterWaterNode) {
#if defined(_M_IX86) || defined(__i386__)
    if (!state.context || !callback)
        return NativeHookStatus::InvalidArgument;
    if (state.puddleSlot)
        return NativeHookStatus::AlreadyInstalled;
    const uintptr_t base = reinterpret_cast<uintptr_t>(state.targets[0]) - 0x9bc40;
    NativeMemory memory = {ReadNativeWaterMemory, nullptr, base};
    // Load stores the vtable with `mov dword ptr [edi], imm32` at 0x538654.
    uint8_t store[6] = {};
    uint32_t vtable[10] = {};
    const uintptr_t table = memory.Address(0x6e0f0c);
    if (!memory.Read(memory.Address(0x538654), store, sizeof(store)) || store[0] != 0xc7 ||
        store[1] != 0x07 || !memory.Read(table, vtable, sizeof(vtable)))
        return NativeHookStatus::UnsupportedExecutable;
    uint32_t stored = 0;
    memcpy(&stored, store + 2, sizeof(stored));
    const uint32_t stub = static_cast<uint32_t>(memory.Address(0x407f70));
    if (stored != table || vtable[0] != memory.Address(0x537f40) || vtable[6] != stub ||
        vtable[7] != stub || vtable[8] != stub || vtable[9] != memory.Address(0x407f60))
        return NativeHookStatus::UnsupportedExecutable;
    state.puddleCallback = callback;
    state.afterWaterNode = afterWaterNode;
    state.puddleUser = user;
    void** slot = reinterpret_cast<void**>(table) + 7;
    state.puddleOriginal = *slot;
    if (!WriteSlot(slot, reinterpret_cast<void*>(&RenderPuddle))) {
        state.puddleCallback = nullptr;
        state.afterWaterNode = nullptr;
        state.puddleUser = nullptr;
        return NativeHookStatus::HookEnableFailure;
    }
    state.puddleSlot = slot;
    return NativeHookStatus::Ok;
#else
    (void)callback;
    (void)user;
    return NativeHookStatus::UnsupportedArchitecture;
#endif
}

NativeHookStatus InstallNativeEnvironmentCaptureHook(NativeCaptureSizeCallback gameCapture,
                                                     void* user) {
#if defined(_M_IX86) || defined(__i386__)
    if (!state.context || !gameCapture || state.created != 4)
        return state.captureOriginal ? NativeHookStatus::AlreadyInstalled
                                     : NativeHookStatus::InvalidArgument;
    const uintptr_t base = reinterpret_cast<uintptr_t>(state.targets[0]) - 0x9bc40;
    NativeMemory memory = {ReadNativeWaterMemory, nullptr, base};
    // `push 0x100` at 0x4f942e, `call 0x48c550` at 0x4f9463, and the capture's
    // prologue up to its relocated SEH handler.
    const unsigned char push[] = {0x68, 0x00, 0x01, 0x00, 0x00};
    const unsigned char prologue[] = {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68};
    unsigned char actual[sizeof(push)] = {};
    uint8_t call[5] = {};
    int32_t relative = 0;
    if (!memory.Read(memory.Address(0x4f942e), actual, sizeof(push)) ||
        memcmp(actual, push, sizeof(push)) || !memory.Read(memory.Address(0x4f9463), call, 5) ||
        call[0] != 0xe8 || !memory.Read(memory.Address(0x48c550), actual, sizeof(prologue)) ||
        memcmp(actual, prologue, sizeof(prologue)))
        return NativeHookStatus::UnsupportedExecutable;
    memcpy(&relative, call + 1, sizeof(relative));
    if (memory.Address(0x4f9468) + relative != memory.Address(0x48c550))
        return NativeHookStatus::UnsupportedExecutable;
    state.captureSize = gameCapture;
    state.captureUser = user;
    state.levelLoadReturn = memory.Address(0x4f9468);
    state.targets[4] = reinterpret_cast<void*>(memory.Address(0x48c550));
    if (MH_CreateHook(state.targets[4], reinterpret_cast<void*>(&CaptureLevelEnvironment),
                      reinterpret_cast<void**>(&state.captureOriginal)) != MH_OK)
        return NativeHookStatus::HookCreationFailure;
    ++state.created;
    if (MH_EnableHook(state.targets[4]) != MH_OK)
        return NativeHookStatus::HookEnableFailure;
    return NativeHookStatus::Ok;
#else
    (void)gameCapture;
    (void)user;
    return NativeHookStatus::UnsupportedArchitecture;
#endif
}

#if VINCE_WATER_DIAGNOSTIC_KEYS
NativeHookStatus InstallNativeWakeSwitch(bool (*hidden)(void*), void* user) {
#if defined(_M_IX86) || defined(__i386__)
    if (!state.context || !hidden)
        return NativeHookStatus::InvalidArgument;
    if (state.wakeSlot)
        return NativeHookStatus::AlreadyInstalled;
    const uintptr_t base = reinterpret_cast<uintptr_t>(state.targets[0]) - 0x9bc40;
    NativeMemory memory = {ReadNativeWaterMemory, nullptr, base};
    // The slot must still hold the render whose prologue reads the segment
    // count (+0x30bc) it draws, and slot 0 the wake's destructor.
    const unsigned char prologue[] = {0x56, 0x8b, 0xf1, 0x83, 0xbe, 0xbc, 0x30, 0x00, 0x00, 0x04};
    unsigned char actual[sizeof(prologue)] = {};
    uint32_t vtable[8] = {};
    const uintptr_t table = memory.Address(0x6e4128);
    if (!memory.Read(table, vtable, sizeof(vtable)) || vtable[0] != memory.Address(0x550de0) ||
        vtable[7] != memory.Address(0x555970) ||
        !memory.Read(memory.Address(0x555970), actual, sizeof(actual)) ||
        memcmp(actual, prologue, sizeof(prologue)))
        return NativeHookStatus::UnsupportedExecutable;
    state.wakeHidden = hidden;
    state.wakeUser = user;
    void** slot = reinterpret_cast<void**>(table) + 7;
    state.wakeOriginal = reinterpret_cast<NodeRender>(*slot);
    if (!WriteSlot(slot, reinterpret_cast<void*>(&RenderWake))) {
        state.wakeHidden = nullptr;
        return NativeHookStatus::HookEnableFailure;
    }
    state.wakeSlot = slot;
    return NativeHookStatus::Ok;
#else
    (void)hidden;
    (void)user;
    return NativeHookStatus::UnsupportedArchitecture;
#endif
}
#endif

NativeHookStatus InstallNativePuddleSplashHook() {
#if defined(_M_IX86) || defined(__i386__)
    if (!state.context || state.created < 4)
        return NativeHookStatus::InvalidArgument;
    if (state.frameTarget)
        return NativeHookStatus::AlreadyInstalled;
    const uintptr_t base = reinterpret_cast<uintptr_t>(state.targets[0]) - 0x9bc40;
    NativeMemory memory = {ReadNativeWaterMemory, nullptr, base};
    // Each address must be what the inspected exe uses: the level loop calls
    // the frame function at 0x50b4f9, and WaterRegion's vinceWaterSplash calls
    // PlaySound at 0x538358 and AddOneShot on [0xb653ec] at 0x538386..0x538391,
    // building both names with the string constructor/destructor below.
    auto calls = [&](uint32_t site, uint32_t target) {
        uint8_t call[5] = {};
        int32_t relative = 0;
        if (!memory.Read(memory.Address(site), call, 5) || call[0] != 0xe8)
            return false;
        memcpy(&relative, call + 1, sizeof(relative));
        return memory.Address(site + 5) + relative == memory.Address(target);
    };
    auto starts = [&](uint32_t address, const unsigned char* bytes, size_t count) {
        unsigned char actual[16] = {};
        return count <= sizeof(actual) && memory.Read(memory.Address(address), actual, count) &&
               !memcmp(actual, bytes, count);
    };
    const unsigned char sehPrologue[] = {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68};
    const unsigned char construct[] = {0x55, 0x8b, 0xec, 0x56, 0x8b, 0xf1, 0x8b, 0x4d, 0x08};
    const unsigned char destroy[] = {0x56, 0x8b, 0xf1, 0x8b, 0x46, 0x14};
    uint8_t load[6] = {};
    uint32_t loaded = 0;
    if (!calls(0x50b4f9, 0x50baa0) || !calls(0x538358, 0x443cd0) ||
        !calls(0x538391, 0x43d470) || !calls(0x538377, 0x407e70) ||
        !calls(0x5383a0, 0x407f20) || !starts(0x50baa0, sehPrologue, sizeof(sehPrologue)) ||
        !starts(0x443cd0, sehPrologue, sizeof(sehPrologue)) ||
        !starts(0x43d470, sehPrologue, sizeof(sehPrologue)) ||
        !starts(0x407e70, construct, sizeof(construct)) ||
        !starts(0x407f20, destroy, sizeof(destroy)) ||
        !memory.Read(memory.Address(0x538386), load, sizeof(load)) || load[0] != 0x8b ||
        load[1] != 0x0d)
        return NativeHookStatus::UnsupportedExecutable;
    memcpy(&loaded, load + 2, sizeof(loaded));
    if (loaded != memory.Address(0xb653ec))
        return NativeHookStatus::UnsupportedExecutable;
    state.stringConstruct = reinterpret_cast<StringConstruct>(memory.Address(0x407e70));
    state.stringDestroy = reinterpret_cast<StringDestroy>(memory.Address(0x407f20));
    state.addOneShot = reinterpret_cast<AddOneShot>(memory.Address(0x43d470));
    state.playSound = reinterpret_cast<PlaySound>(memory.Address(0x443cd0));
    state.particleManager = memory.Address(0xb653ec);
    void* target = reinterpret_cast<void*>(memory.Address(0x50baa0));
    if (MH_CreateHook(target, reinterpret_cast<void*>(&RunFrame),
                      reinterpret_cast<void**>(&state.frameOriginal)) != MH_OK)
        return NativeHookStatus::HookCreationFailure;
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        return NativeHookStatus::HookEnableFailure;
    }
    state.frameTarget = target;
    return NativeHookStatus::Ok;
#else
    return NativeHookStatus::UnsupportedArchitecture;
#endif
}

void QueueNativePuddleSplash(uint32_t critter, bool isVince, const float position[3]) {
#if defined(_M_IX86) || defined(__i386__)
    if (!state.frameTarget || GetCurrentThreadId() != state.renderThread || !critter)
        return;
    const LONG generation = InterlockedCompareExchange(&levelGeneration, 0, 0);
    if (pendingSplashCount && pendingSplashGeneration != generation)
        pendingSplashCount = 0;
    if (pendingSplashCount >= sizeof(pendingSplashes) / sizeof(pendingSplashes[0]))
        return;
    pendingSplashGeneration = generation;
    // FrameTick: Vince through SoundManager with flags 0x20, other critters
    // through Critter::PlaySound (the same call with 0x28).
    PendingSplash& splash = pendingSplashes[pendingSplashCount++];
    splash = {critter, isVince ? 0x20u : 0x28u, {position[0], position[1], position[2]}};
#else
    (void)critter;
    (void)isVince;
    (void)position;
#endif
}

uint32_t NativePuddleSplashesPlayed() {
#if defined(_M_IX86) || defined(__i386__)
    return static_cast<uint32_t>(InterlockedCompareExchange(&splashesPlayed, 0, 0));
#else
    return 0;
#endif
}

uint32_t NativeWaterEnvironmentCapture() {
#if defined(_M_IX86) || defined(__i386__)
    return static_cast<uint32_t>(InterlockedCompareExchange(&waterCapture, 0, 0));
#else
    return 0;
#endif
}

NativePuddleHookCounters NativePuddleHookState() {
    NativePuddleHookCounters counters = {};
#if defined(_M_IX86) || defined(__i386__)
    counters.calls = static_cast<uint32_t>(InterlockedCompareExchange(&puddleCalls, 0, 0));
    counters.foreignThread =
        static_cast<uint32_t>(InterlockedCompareExchange(&puddleForeignThread, 0, 0));
    counters.nested = static_cast<uint32_t>(InterlockedCompareExchange(&puddleNested, 0, 0));
    counters.slotIsHook =
        state.puddleSlot && *state.puddleSlot == reinterpret_cast<void*>(&RenderPuddle);
    memcpy(counters.waterRenderCallers, renderCallers, sizeof(renderCallers));
#endif
    return counters;
}

uint32_t NativeWaterLevelGeneration() {
#if defined(_M_IX86) || defined(__i386__)
    return static_cast<uint32_t>(InterlockedCompareExchange(&levelGeneration, 0, 0));
#else
    return 0;
#endif
}

void RemoveNativeWaterHooks() {
#if defined(_M_IX86) || defined(__i386__)
    if (state.puddleSlot && *state.puddleSlot == reinterpret_cast<void*>(&RenderPuddle))
        WriteSlot(state.puddleSlot, state.puddleOriginal);
#if VINCE_WATER_DIAGNOSTIC_KEYS
    if (state.wakeSlot && *state.wakeSlot == reinterpret_cast<void*>(&RenderWake))
        WriteSlot(state.wakeSlot, reinterpret_cast<void*>(state.wakeOriginal));
#endif
    for (unsigned i = 0; i < state.created; ++i)
        MH_DisableHook(state.targets[i]);
    for (unsigned i = 0; i < state.created; ++i)
        MH_RemoveHook(state.targets[i]);
    if (state.frameTarget) {
        MH_DisableHook(state.frameTarget);
        MH_RemoveHook(state.frameTarget);
    }
    pendingSplashCount = 0;
    if (state.context)
        state.context->Release();
    // MinHook can be shared by the enclosing AIO. Its global allocator lifetime
    // belongs to that host; Uninitialize here could remove another mod's hooks.
    state = {};
#endif
}

} // namespace water
} // namespace vince
