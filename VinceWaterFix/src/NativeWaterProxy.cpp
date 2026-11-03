// Optional experimental PC loader. The reusable renderer has no proxy dependency.
// This DLL is pinned after activation: restarting the game is the unload boundary.
#include "NativeWaterRuntime.h"
#include "NativeWaterHooks.h"
#include "NativeWaterMemory.h"
#include "NativeWaterOccupants.h"
#include "NativeWaterPrograms.generated.h"
#include <new>

namespace vince {
namespace water {
namespace {
using CreateDeviceFunction = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                              const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                              D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

HMODULE proxyModule = nullptr;
HMODULE systemD3D = nullptr;
INIT_ONCE systemOnce = INIT_ONCE_STATIC_INIT;
CreateDeviceFunction realCreateDevice = nullptr;
NativeWaterRuntime* runtime = nullptr;
volatile LONG initializationStarted = 0;
bool haveRuntimeStatus = false;
NativeRuntimeStatus lastRuntimeStatus = NativeRuntimeStatus::NotInitialized;
HRESULT lastGraphicsResult = S_OK;
unsigned runtimeLogEvents = 0;
#if VINCE_WATER_DIAGNOSTIC_KEYS
unsigned comparisonMode = 0;
bool comparisonKeyDown = false;
#endif

void Log(const char* message) {
    wchar_t path[MAX_PATH];
    DWORD count = GetModuleFileNameW(proxyModule, path, MAX_PATH);
    if (!count || count >= MAX_PATH)
        return;
    while (count && path[count - 1] != L'\\' && path[count - 1] != L'/')
        --count;
    const wchar_t name[] = L"VinceWater.log";
    if (count + sizeof(name) / sizeof(name[0]) > MAX_PATH)
        return;
    for (unsigned index = 0; index < sizeof(name) / sizeof(name[0]); ++index)
        path[count + index] = name[index];
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD bytes = 0;
    while (message[bytes])
        ++bytes;
    DWORD written = 0;
    WriteFile(file, message, bytes, &written, nullptr);
    CloseHandle(file);
}

struct LogLine {
    char bytes[256] = {};
    unsigned used = 0;
    void Text(const char* text) {
        while (*text && used + 1 < sizeof(bytes))
            bytes[used++] = *text++;
        bytes[used] = 0;
    }
    void Hex(uint32_t value) {
        Text("0x");
        const char digits[] = "0123456789abcdef";
        for (int shift = 28; shift >= 0 && used + 1 < sizeof(bytes); shift -= 4)
            bytes[used++] = digits[(value >> shift) & 15];
        bytes[used] = 0;
    }
    void Number(uint64_t value) {
        char digits[24];
        unsigned count = 0;
        do {
            digits[count++] = char('0' + value % 10);
            value /= 10;
        } while (value);
        while (count && used + 1 < sizeof(bytes))
            bytes[used++] = digits[--count];
        bytes[used] = 0;
    }
    void Fixed(float value) {
        // Three decimals are enough to compare against authored positions.
        if (!(value > -1e6f && value < 1e6f)) {
            Text("nan");
            return;
        }
        if (value < 0) {
            Text("-");
            value = -value;
        }
        const uint64_t scaled = static_cast<uint64_t>(value * 1000.0f + 0.5f);
        Number(scaled / 1000);
        Text(".");
        const unsigned fraction = static_cast<unsigned>(scaled % 1000);
        if (fraction < 100)
            Text("0");
        if (fraction < 10)
            Text("0");
        Number(fraction);
    }
};
bool havePuddleStatus = false;
NativeRuntimeStatus lastPuddleStatus = NativeRuntimeStatus::NotInitialized;
unsigned puddleLogEvents = 0;

const char* StatusName(NativeRuntimeStatus status) {
    switch (status) {
    case NativeRuntimeStatus::Ready:
        return "replacement draw succeeded";
    case NativeRuntimeStatus::NotInitialized:
        return "runtime not initialized";
    case NativeRuntimeStatus::UnsupportedArea:
        return "area requires explicit restoration settings";
    case NativeRuntimeStatus::SceneUnavailable:
        return "scene data unavailable";
    case NativeRuntimeStatus::MaterialUnavailable:
        return "material data unavailable";
    case NativeRuntimeStatus::UnsupportedMaterial:
        return "unsupported material";
    case NativeRuntimeStatus::MeshUnavailable:
        return "native mesh unavailable";
    case NativeRuntimeStatus::UnsupportedMesh:
        return "unsupported native mesh";
    case NativeRuntimeStatus::AllocationFailed:
        return "allocation failed";
    case NativeRuntimeStatus::SurfaceFailed:
        return "surface creation or update failed";
    case NativeRuntimeStatus::DrawFailed:
        return "replacement draw failed";
    }
    return "unknown runtime status";
}

// The first time each area reaches each outcome, so one play-through shows
// which areas still keep Remastered's water and why. The running-status log
// above is capped and mostly records transient scene reads during loading.
struct AreaOutcomes {
    char area[32];
    uint32_t seen; // Bit per NativeRuntimeStatus.
};
AreaOutcomes areaOutcomes[32] = {};

bool SameArea(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const char x = *a >= 'A' && *a <= 'Z' ? char(*a + 32) : *a;
        const char y = *b >= 'A' && *b <= 'Z' ? char(*b + 32) : *b;
        if (x != y)
            return false;
        if (!x)
            return true;
    }
}

void LogAreaOutcome(const NativeWaterDiagnostics& diagnostics) {
    if (!diagnostics.area[0] || diagnostics.lastStatus == NativeRuntimeStatus::SceneUnavailable)
        return;
    AreaOutcomes* entry = nullptr;
    for (auto& candidate : areaOutcomes) {
        if (!candidate.area[0]) {
            for (unsigned i = 0; i < sizeof(candidate.area); ++i)
                candidate.area[i] = diagnostics.area[i];
            entry = &candidate;
            break;
        }
        if (SameArea(candidate.area, diagnostics.area)) {
            entry = &candidate;
            break;
        }
    }
    const uint32_t bit = 1u << static_cast<unsigned>(diagnostics.lastStatus);
    if (!entry || (entry->seen & bit))
        return;
    entry->seen |= bit;
    LogLine line;
    line.Text("Water in ");
    line.Text(diagnostics.area);
    line.Text(": ");
    line.Text(StatusName(diagnostics.lastStatus));
    if (diagnostics.lastStatus != NativeRuntimeStatus::Ready)
        line.Text(" (Remastered water kept)");
    line.Text("\r\n");
    Log(line.bytes);
}

// First few splashes, to confirm in a log that entering the puddle splashes.
unsigned splashLogEvents = 0;
void QueueSplash(void*, uint32_t critter, bool isVince, const float position[3]) {
    QueueNativePuddleSplash(critter, isVince, position);
    if (splashLogEvents >= 8)
        return;
    ++splashLogEvents;
    LogLine line;
    line.Text("Puddle splash: ");
    line.Text(isVince ? "Vince" : "critter ");
    if (!isVince)
        line.Hex(critter);
    line.Text(" at ");
    line.Fixed(position[0]);
    line.Text(" ");
    line.Fixed(position[1]);
    line.Text(" ");
    line.Fixed(position[2]);
    line.Text("; splashes played before this one=");
    line.Number(NativePuddleSplashesPlayed());
    line.Text("\r\n");
    Log(line.bytes);
}

#if VINCE_WATER_DIAGNOSTIC_KEYS
// Circle investigation: per-frame scene inputs in Jimmy's Fanboat, buffered so
// the log itself causes no per-frame file I/O. One line per game frame.
char sampleBuffer[16 * 1024]; // About two seconds of lines per write.
unsigned sampleUsed = 0, samplesLogged = 0;
uint32_t lastSampleFrame = 0xffffffffu;
void FlushSamples() {
    if (!sampleUsed)
        return;
    sampleBuffer[sampleUsed] = 0;
    Log(sampleBuffer);
    sampleUsed = 0;
}
void LogFrameSample(const NativeWaterDiagnostics& diagnostics) {
    const NativeWaterFrameSample& s = runtime->LastFrameSample();
    if (samplesLogged >= 4000 || s.frame == lastSampleFrame ||
        !SameArea(diagnostics.area, "jimmysfanboat"))
        return;
    lastSampleFrame = s.frame;
    if (!samplesLogged++)
        Log("Samples: frame mode dt_ms steps camera_xyz fog_near fog_far fog_rgb reflectivity "
            "env base detail\r\n");
    LogLine line;
    line.Text("S ");
    line.Number(s.frame);
    line.Text(" ");
    line.Number(comparisonMode + 1);
    line.Text(" ");
    line.Fixed(s.delta * 1000.0f);
    line.Text(" ");
    line.Number(s.steps);
    const float values[] = {s.camera[0], s.camera[1], s.camera[2], s.fogNear, s.fogFar,
                            s.fogColor[0], s.fogColor[1], s.fogColor[2], s.reflectivity};
    for (float value : values) {
        line.Text(" ");
        line.Fixed(value);
    }
    const uint32_t views[] = {s.environmentView, s.baseView, s.detailView};
    for (uint32_t view : views) {
        line.Text(" ");
        line.Hex(view);
    }
    line.Text("\r\n");
    if (sampleUsed + line.used + 1 >= sizeof(sampleBuffer))
        FlushSamples();
    for (unsigned i = 0; i < line.used; ++i)
        sampleBuffer[sampleUsed++] = line.bytes[i];
    if (samplesLogged == 4000)
        FlushSamples();
}
#endif

void LogPuddleStatus(const char* path);
void ReportPuddleScene(uintptr_t waterNode);
void ReportPuddleOccupants();

bool DrawWater(void*, ID3D11DeviceContext* context, const NativeWaterInvocation& invocation) {
    if (!runtime)
        return false;
#if VINCE_WATER_DIAGNOSTIC_KEYS
    // Test-build input belongs to the optional proxy, never the AIO renderer.
    // Poll the high key bit on the owning thread; do not consume another app's
    // global "pressed since last call" bit or react while the game is unfocused.
    DWORD foregroundProcess = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
    const bool down = foregroundProcess == GetCurrentProcessId() &&
                      (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (down && !comparisonKeyDown) {
        comparisonMode = (comparisonMode + 1) % 4;
        const char* names[] = {"1: restored water, boat wake shown",
                               "2: restored water, boat wake hidden",
                               "3: Remastered water, boat wake hidden",
                               "4: Remastered water, boat wake shown"};
        LogLine line;
        line.Text("Comparison: ");
        line.Text(names[comparisonMode]);
        line.Text("\r\n");
        Log(line.bytes);
    }
    comparisonKeyDown = down;
    if (comparisonMode >= 2)
        return false; // Native Remastered water.
#endif
    const bool replaced = runtime->Render(context, invocation, NativeWaterLevelGeneration());
    const NativeWaterDiagnostics diagnostics = runtime->Diagnostics();
    LogAreaOutcome(diagnostics);
#if VINCE_WATER_DIAGNOSTIC_KEYS
    if (replaced)
        LogFrameSample(diagnostics);
    else if (!SameArea(diagnostics.area, "jimmysfanboat"))
        FlushSamples(); // Leaving the level writes what was recorded.
#endif
    if (runtimeLogEvents < 64 &&
        (!haveRuntimeStatus || diagnostics.lastStatus != lastRuntimeStatus ||
         diagnostics.graphicsResult != lastGraphicsResult)) {
        LogLine line;
        line.Text("Water: ");
        line.Text(StatusName(diagnostics.lastStatus));
        line.Text("; HRESULT=");
        line.Hex(static_cast<uint32_t>(diagnostics.graphicsResult));
        line.Text("; replacements=");
        line.Number(diagnostics.replacements);
        line.Text("; fallbacks=");
        line.Number(diagnostics.fallbacks);
        line.Text("\r\n");
        Log(line.bytes);
        haveRuntimeStatus = true;
        lastRuntimeStatus = diagnostics.lastStatus;
        lastGraphicsResult = diagnostics.graphicsResult;
        ++runtimeLogEvents;
    }
    return replaced;
}

// Remastered's dedicated water pass renders only the level's one water node
// (renderer +0x164) and nothing renders the puddle node, so draw the authored
// puddle right after that node's render, whether or not the water itself was
// on screen or replaced.
void DrawPuddleAfterWater(void*, ID3D11DeviceContext* context, uintptr_t waterNode,
                          uintptr_t world) {
    if (!runtime || runtime->PuddleDrawnByScene())
        return;
    const uint64_t attempts = runtime->PuddleDiagnostics().attempts;
    if (runtime->RenderAuthoredPuddle(context, world, NativeWaterLevelGeneration()))
        ReportPuddleScene(waterNode);
    if (runtime->PuddleDiagnostics().attempts != attempts)
        LogPuddleStatus("after the water pass");
    if (runtime->PuddleDiagnostics().lastStatus == NativeRuntimeStatus::Ready)
        ReportPuddleOccupants();
}

void LogPuddleStatus(const char* path) {
    const NativeWaterDiagnostics diagnostics = runtime->PuddleDiagnostics();
    // UnsupportedArea is every other level's normal state; log it only once.
    if (puddleLogEvents < 32 && (!havePuddleStatus || diagnostics.lastStatus != lastPuddleStatus)) {
        LogLine line;
        line.Text("Puddle: ");
        line.Text(diagnostics.lastStatus == NativeRuntimeStatus::Ready ? "restored draw succeeded"
                                                                        : StatusName(diagnostics.lastStatus));
        line.Text(" (");
        line.Text(path);
        line.Text("); HRESULT=");
        line.Hex(static_cast<uint32_t>(diagnostics.graphicsResult));
        if (diagnostics.lastStatus == NativeRuntimeStatus::Ready) {
            const WaterVector at = runtime->PuddlePosition();
            line.Text("; at ");
            line.Fixed(at.x);
            line.Text(" ");
            line.Fixed(at.y);
            line.Text(" ");
            line.Fixed(at.z);
        }
        line.Text("; draws=");
        line.Number(diagnostics.replacements);
        line.Text("; skipped=");
        line.Number(diagnostics.fallbacks);
        line.Text("\r\n");
        Log(line.bytes);
        havePuddleStatus = true;
        lastPuddleStatus = diagnostics.lastStatus;
        ++puddleLogEvents;
    }
}

void DrawPuddle(void*, ID3D11DeviceContext* context, uintptr_t node, uintptr_t world) {
    if (!runtime)
        return;
    runtime->RenderPuddle(context, node, world, NativeWaterLevelGeneration());
    LogPuddleStatus("scene render slot");
}

uint32_t ReadU32(uintptr_t address) {
    uint32_t value = 0;
    ReadNativeWaterMemory(nullptr, address, &value, 4);
    return value;
}

// Scene node: +4 type, +8 MSVC name string, +0x20 parent, +0x28 child count,
// +0x88 flags. Logs the chain from a node up to its root.
void LogNodeChain(const char* label, uintptr_t node, uint32_t base) {
    for (unsigned depth = 0; node && depth < 12; ++depth) {
        LogLine line;
        line.Text("  ");
        line.Text(label);
        line.Text(" ancestor ");
        line.Number(depth);
        line.Text(": vtable=");
        line.Hex(ReadU32(node) - base + 0x400000u);
        line.Text(" type=");
        line.Number(ReadU32(node + 4));
        line.Text(" flags=");
        line.Hex(ReadU32(node + 0x88));
        line.Text(" children=");
        line.Number(ReadU32(node + 0x28));
        line.Text(" name=");
        char name[17] = {};
        const uint32_t length = ReadU32(node + 8 + 0x10), capacity = ReadU32(node + 8 + 0x14);
        const uintptr_t text = capacity >= 16 ? ReadU32(node + 8) : node + 8;
        ReadNativeWaterMemory(nullptr, text, name, length < 16 ? length : 16);
        for (char& c : name)
            c = c && (c < 32 || c > 126) ? '?' : c;
        line.Text(name);
        line.Text("\r\n");
        Log(line.bytes);
        node = ReadU32(node + 0x20);
    }
}

// This frame's render queue, read from inside its consumer (0x428490):
// entries 0x54 bytes at [0xb655e4..0xb655e8) (+0x40 sort key, +0x44 node),
// drawn through the index list [0xbe79e0..0xbe79e4). Logs whether the water
// and puddle nodes were queued and listed.
void ReportRenderQueue(uintptr_t waterNode, uintptr_t puddleNode, uint32_t base) {
    const uint32_t entries = ReadU32(base - 0x400000u + 0xb655e4u);
    const uint32_t entriesEnd = ReadU32(base - 0x400000u + 0xb655e8u);
    const uint32_t indices = ReadU32(base - 0x400000u + 0xbe79e0u);
    const uint32_t indicesEnd = ReadU32(base - 0x400000u + 0xbe79e4u);
    const uint32_t count = entriesEnd > entries ? (entriesEnd - entries) / 0x54 : 0;
    const uint32_t listed = indicesEnd > indices ? (indicesEnd - indices) / 4 : 0;
    uint32_t waterIndex = 0xffffffffu, puddleIndex = 0xffffffffu, waterKey = 0, puddleKey = 0;
    for (uint32_t i = 0; i < count && i < 8192; ++i) {
        const uint32_t node = ReadU32(entries + i * 0x54 + 0x44);
        if (node == waterNode && waterIndex == 0xffffffffu) {
            waterIndex = i;
            waterKey = ReadU32(entries + i * 0x54 + 0x40);
        }
        if (puddleNode && node == puddleNode && puddleIndex == 0xffffffffu) {
            puddleIndex = i;
            puddleKey = ReadU32(entries + i * 0x54 + 0x40);
        }
    }
    bool waterListed = false, puddleListed = false;
    for (uint32_t i = 0; i < listed && i < 8192; ++i) {
        const uint32_t index = ReadU32(indices + i * 4);
        waterListed |= index == waterIndex;
        puddleListed |= index == puddleIndex;
    }
    LogLine line;
    line.Text("  render queue: entries=");
    line.Number(count);
    line.Text(" listed=");
    line.Number(listed);
    line.Text(" water ");
    line.Text(waterIndex == 0xffffffffu ? "not queued" : waterListed ? "queued+listed key=" : "queued key=");
    if (waterIndex != 0xffffffffu)
        line.Hex(waterKey);
    line.Text(" puddle ");
    line.Text(!puddleNode ? "unknown" : puddleIndex == 0xffffffffu ? "not queued"
                                        : puddleListed ? "queued+listed key=" : "queued key=");
    if (puddleIndex != 0xffffffffu)
        line.Hex(puddleKey);
    line.Text("\r\n");
    Log(line.bytes);
    if (puddleNode) {
        float bounds[6] = {};
        ReadNativeWaterMemory(nullptr, puddleNode + 0x64, bounds, sizeof(bounds));
        LogLine boundsLine;
        boundsLine.Text("  puddle node bounds low.x=");
        boundsLine.Hex(*reinterpret_cast<uint32_t*>(&bounds[0]));
        boundsLine.Text(" high.x=");
        boundsLine.Hex(*reinterpret_cast<uint32_t*>(&bounds[3]));
        boundsLine.Text(" flags=");
        boundsLine.Hex(ReadU32(puddleNode + 0x88));
        boundsLine.Text("\r\n");
        Log(boundsLine.bytes);
    }
}

// Logs the puddle's native occupant list (NativeWaterOccupants) whenever
// its membership changes.
uint32_t occupantGeneration = 0, lastOccupantKey = 0xffffffffu;
unsigned occupantLogEvents = 0;
void ReportPuddleOccupants() {
    const uint32_t generation = NativeWaterLevelGeneration();
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const NativeMemory memory = {ReadNativeWaterMemory, nullptr, base};
    static NativePuddleRegion region = {};
    static unsigned retry = 0;
    if (generation != occupantGeneration) {
        occupantGeneration = generation;
        lastOccupantKey = 0xffffffffu;
        region = {};
        retry = 0;
    }
    // The level-load reflection capture draws the puddle before the regions
    // exist, so keep looking (about once a second) until the region appears.
    if (!ValidNativePuddleRegion(memory, region)) {
        if (retry)
            --retry;
        else if (!FindNativePuddleRegion(memory, &region))
            retry = 60;
    }
    if (occupantLogEvents >= 40)
        return;
    NativePuddleOccupant occupants[8] = {};
    const int count = ValidNativePuddleRegion(memory, region)
                          ? ReadNativePuddleOccupants(memory, region.script, occupants, 8)
                          : -1;
    // Membership key: count and ids.
    uint32_t key = static_cast<uint32_t>(count) * 0x9e3779b1u;
    for (int i = 0; i < count; ++i)
        key = (key ^ occupants[i].id) * 0x01000193u;
    if (key == lastOccupantKey)
        return;
    lastOccupantKey = key;
    ++occupantLogEvents;
    LogLine line;
    line.Text("Puddle occupants: ");
    if (count < 0)
        line.Text(region.node ? "list unreadable" : "puddle region not found");
    else
        line.Number(static_cast<uint32_t>(count));
    line.Text(runtime->PuddleOccupantsFromList() ? " (ripples from the native list)\r\n"
                                                 : " (ripples from the Vince fallback)\r\n");
    Log(line.bytes);
    for (int i = 0; i < count; ++i) {
        LogLine entry;
        entry.Text("  ");
        entry.Text(occupants[i].name);
        entry.Text(" id=");
        entry.Number(occupants[i].id);
        entry.Text(" at ");
        entry.Fixed(occupants[i].position[0]);
        entry.Text(" ");
        entry.Fixed(occupants[i].position[1]);
        entry.Text(" ");
        entry.Fixed(occupants[i].position[2]);
        entry.Text(occupants[i].isVince ? " (Vince)\r\n" : "\r\n");
        Log(entry.bytes);
    }
}

// One-time report for why the scene never reached the patched puddle slot:
// hook counters plus the flags/type of every sibling of the drawn water node.
bool puddleSceneReported = false;
void ReportPuddleScene(uintptr_t waterNode) {
    if (puddleSceneReported)
        return;
    puddleSceneReported = true;
    const NativePuddleHookCounters counters = NativePuddleHookState();
    LogLine line;
    line.Text("Puddle diagnostics: slot calls=");
    line.Number(counters.calls);
    line.Text("; foreign thread=");
    line.Number(counters.foreignThread);
    line.Text("; nested=");
    line.Number(counters.nested);
    line.Text(counters.slotIsHook ? "; slot patched\r\n" : "; slot NOT patched\r\n");
    Log(line.bytes);
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    LogLine callers;
    callers.Text("  water render callers:");
    for (uint32_t address : counters.waterRenderCallers) {
        if (!address)
            break;
        callers.Text(" ");
        // Image addresses as Steam VAs; anything else (other modules) raw.
        callers.Hex(address >= base && address < base + 0x1000000u
                        ? address - static_cast<uint32_t>(base) + 0x400000u
                        : address);
    }
    callers.Text("\r\n");
    Log(callers.bytes);
    LogNodeChain("water", waterNode, static_cast<uint32_t>(base));
    NativePuddleRegion region = {};
    FindNativePuddleRegion(NativeMemory{ReadNativeWaterMemory, nullptr, base}, &region);
    const uintptr_t puddleNode = region.node;
    if (puddleNode)
        LogNodeChain("puddle", puddleNode, static_cast<uint32_t>(base));
    else
        Log("  puddle node not found in the region list\r\n");
    ReportRenderQueue(waterNode, puddleNode, static_cast<uint32_t>(base));
    uint32_t parent = 0, head = 0, entry = 0;
    if (!ReadNativeWaterMemory(nullptr, waterNode + 0x20, &parent, 4) || !parent ||
        !ReadNativeWaterMemory(nullptr, parent + 0x24, &head, 4) || !head ||
        !ReadNativeWaterMemory(nullptr, head, &entry, 4))
        return;
    for (unsigned count = 0; entry && entry != head && count < 48; ++count) {
        uint32_t child = 0, vtable = 0, type = 0, flags = 0, boundsLow = 0, next = 0;
        if (!ReadNativeWaterMemory(nullptr, entry + 8, &child, 4) ||
            !ReadNativeWaterMemory(nullptr, entry, &next, 4))
            break;
        ReadNativeWaterMemory(nullptr, child, &vtable, 4);
        ReadNativeWaterMemory(nullptr, child + 4, &type, 4);
        ReadNativeWaterMemory(nullptr, child + 0x88, &flags, 4);
        ReadNativeWaterMemory(nullptr, child + 0x64, &boundsLow, 4);
        LogLine child_line;
        child_line.Text("  sibling vtable=");
        child_line.Hex(vtable - static_cast<uint32_t>(base) + 0x400000u);
        child_line.Text(child == waterNode ? " (water)" : "");
        child_line.Text(" type=");
        child_line.Number(type);
        child_line.Text(" flags=");
        child_line.Hex(flags);
        child_line.Text(" boundsLow.x=");
        child_line.Hex(boundsLow);
        child_line.Text("\r\n");
        Log(child_line.bytes);
        entry = next;
    }
}

BOOL CALLBACK LoadSystemD3D(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH];
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    const wchar_t suffix[] = L"\\d3d11.dll";
    if (!length || length + sizeof(suffix) / sizeof(suffix[0]) > MAX_PATH)
        return TRUE;
    for (unsigned index = 0; index < sizeof(suffix) / sizeof(suffix[0]); ++index)
        path[length + index] = suffix[index];
    // Absolute system path prevents recursive loading of this proxy.
    systemD3D = LoadLibraryW(path);
    if (systemD3D)
        realCreateDevice =
            reinterpret_cast<CreateDeviceFunction>(GetProcAddress(systemD3D, "D3D11CreateDevice"));
    return TRUE;
}

void InitializeWater(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context || InterlockedCompareExchange(&initializationStarted, 1, 0))
        return;
    Log("VinceWater experimental native preview: initialization begins.\r\n");
    // Pin before installing any callbacks. Deliberate process-lifetime ownership
    // avoids DLL unload while MinHook trampolines or GPU objects remain live.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&InitializeWater), &pinned)) {
        Log("Water: could not pin loader; native rendering retained.\r\n");
        return;
    }
    static_assert(alignof(NativeWaterRuntime) <= MEMORY_ALLOCATION_ALIGNMENT,
                  "HeapAlloc storage cannot hold an over-aligned runtime");
    void* storage = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(NativeWaterRuntime));
    if (!storage) {
        Log("Water: runtime allocation failed.\r\n");
        return;
    }
    runtime = new (storage) NativeWaterRuntime();
    const uintptr_t gameBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const NativeMemory memory = {ReadNativeWaterMemory, nullptr, gameBase};
    if (!runtime->Initialize(device, EmbeddedNativeWaterPrograms(), memory)) {
        runtime->~NativeWaterRuntime();
        HeapFree(GetProcessHeap(), 0, runtime);
        runtime = nullptr;
        Log("Water: runtime initialization failed; native rendering retained.\r\n");
        return;
    }
    const NativeHookStatus hook = InstallNativeWaterHooks(gameBase, context, DrawWater, nullptr);
    LogLine line;
    line.Text("Water hook status=");
    line.Number(static_cast<unsigned>(hook));
    line.Text(hook == NativeHookStatus::Ok ? "; active, waiting for first water draw.\r\n"
                                           : "; hooks inactive, native rendering retained.\r\n");
    Log(line.bytes);
    if (hook != NativeHookStatus::Ok) {
        runtime->~NativeWaterRuntime();
        HeapFree(GetProcessHeap(), 0, runtime);
        runtime = nullptr;
        return;
    }
    // Independent of area water: a mismatch leaves the cut puddle undrawn.
    const NativeHookStatus puddle = InstallNativePuddleHook(DrawPuddle, nullptr, DrawPuddleAfterWater);
    LogLine puddleLine;
    puddleLine.Text("Puddle hook status=");
    puddleLine.Number(static_cast<unsigned>(puddle));
    puddleLine.Text(puddle == NativeHookStatus::Ok ? "; active for authored puddles.\r\n"
                                                   : "; inactive, puddle stays cut.\r\n");
    Log(puddleLine.bytes);
    if (puddle == NativeHookStatus::Ok) {
        // Without it the puddle still ripples; only the splash stays cut.
        const NativeHookStatus splash = InstallNativePuddleSplashHook();
        LogLine splashLine;
        splashLine.Text("Splash hook status=");
        splashLine.Number(static_cast<unsigned>(splash));
        splashLine.Text(splash == NativeHookStatus::Ok
                            ? "; puddle splash particles and sounds active.\r\n"
                            : "; inactive, puddle ripples without a splash.\r\n");
        Log(splashLine.bytes);
        if (splash == NativeHookStatus::Ok)
            runtime->SetPuddleSplash(QueueSplash, nullptr);
    }
#if VINCE_WATER_DIAGNOSTIC_KEYS
    // Comparison build only: F7 modes 2 and 3 hide the fanboat wake, to test
    // whether it is the circle seen around Vince while driving.
    const NativeHookStatus wake = InstallNativeWakeSwitch(
        [](void*) { return comparisonMode == 1 || comparisonMode == 2; }, nullptr);
    LogLine wakeLine;
    wakeLine.Text("Wake switch status=");
    wakeLine.Number(static_cast<unsigned>(wake));
    wakeLine.Text(wake == NativeHookStatus::Ok ? "; F7 cycles water and boat wake modes.\r\n"
                                               : "; inactive, the wake always shows.\r\n");
    Log(wakeLine.bytes);
#endif
}
} // namespace
} // namespace water
} // namespace vince

extern "C" HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType,
                                            HMODULE software, UINT flags,
                                            const D3D_FEATURE_LEVEL* levels, UINT levelCount,
                                            UINT sdkVersion, ID3D11Device** device,
                                            D3D_FEATURE_LEVEL* chosenLevel,
                                            ID3D11DeviceContext** immediateContext) {
    using namespace vince::water;
    InitOnceExecuteOnce(&systemOnce, LoadSystemD3D, nullptr, nullptr);
    if (!realCreateDevice) {
        Log("Water: system D3D11CreateDevice unavailable.\r\n");
        return E_FAIL;
    }
    const HRESULT result =
        realCreateDevice(adapter, driverType, software, flags, levels, levelCount, sdkVersion,
                         device, chosenLevel, immediateContext);
    if (SUCCEEDED(result)) {
        ID3D11Device* actualDevice = device ? *device : nullptr;
        ID3D11DeviceContext* actualContext = immediateContext ? *immediateContext : nullptr;
        bool releaseDevice = false, releaseContext = false;
        if (!actualDevice && actualContext) {
            actualContext->GetDevice(&actualDevice);
            releaseDevice = true;
        }
        if (!actualContext && actualDevice) {
            actualDevice->GetImmediateContext(&actualContext);
            releaseContext = true;
        }
        InitializeWater(actualDevice, actualContext);
        if (releaseContext && actualContext)
            actualContext->Release();
        if (releaseDevice && actualDevice)
            actualDevice->Release();
    }
    return result;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        vince::water::proxyModule = module;
        DisableThreadLibraryCalls(module);
    }
    // No loader-lock rendering, hooks, allocations, destruction or unhooking.
    return TRUE;
}
