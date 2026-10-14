/*
 * Audio, the job of the DLL VinceFix stands in for, plus one fix:
 *   X3DAudioInitialize / X3DAudioCalculate jump to the real DLL in the system directory (thunks.s), loaded on first use
 *   rather than in DllMain.
 *   Audio device details: the game calls IXAudio2::GetDeviceDetails(0) from its main thread about 40 times a minute
 *   (each new 2D sound), and XAudio2 2.7 re-enumerates the audio endpoints each time (about 1.8 ms). Vince.exe's
 *   ole32!CoCreateInstance import slot is routed through VinceFix; when it creates an XAudio2 2.7 engine, slot 4 of the
 *   engine's vtable is pointed at a cache of the index 0 result, filled by the game's own call at engine init and
 *   refreshed when it creates another engine.
 */
#include "vincefix.h"

IMPORT HMODULE WINAPI LoadLibraryW(const WCHAR *path);
IMPORT DWORD WINAPI GetSystemDirectoryW(WCHAR *buffer, DWORD size);
IMPORT BOOL WINAPI GetModuleHandleExW(DWORD flags, const void *name, HMODULE *module);
IMPORT void WINAPI ExitProcess(DWORD code);
IMPORT int WINAPI MessageBoxW(HANDLE owner, const WCHAR *text, const WCHAR *caption, DWORD type);

/* ---- X3DAudio forwarding (called from thunks.s on first use) ------------------------------------------------------ */

/* Read by thunks.s */
void *vf_real_init;
void *vf_real_calc;

#define MB_ICONERROR 0x10

void __cdecl vf_ensure_real(void) {
    if (vf_real_init && vf_real_calc) return;
    WCHAR path[520];
    HMODULE real = 0;
    if (GetSystemDirectoryW(path, 520) && append_name(path, 520, L"X3DAudio1_7.dll")) real = LoadLibraryW(path);
    void *init = real ? GetProcAddress(real, "X3DAudioInitialize") : 0;
    void *calc = real ? GetProcAddress(real, "X3DAudioCalculate") : 0;
    Line line; begin(&line);
    if (real && init && calc && real != g_self) {
        put(&line, "X3DAudio: forwarding to system copy at "); put_hex(&line, (ULONG_PTR)real);
    } else {
        /* Without the real DLL there is nothing sensible to forward to, and returning would leave the game's audio
           settings uninitialized. Without VinceFix the game would refuse to start here, so fail just as clearly. */
        put(&line, "X3DAudio: ERROR loading the system X3DAudio1_7.dll (error "); put_dec(&line, GetLastError(), 0);
        put(&line, "); exiting");
        emit(&line);
        MessageBoxW(0, L"VinceFix (X3DAudio1_7.dll in the game folder) could not load the system copy of "
                       L"X3DAudio1_7.dll from the DirectX June 2010 runtime.\n\nReinstall the DirectX runtime, "
                       L"or delete X3DAudio1_7.dll from the game folder to remove VinceFix.",
                    L"VinceFix", MB_ICONERROR);
        ExitProcess(1);
    }
    emit(&line);
    vf_real_calc = calc;
    vf_real_init = init;
}

/* ---- XAudio2 device details cache --------------------------------------------------------------------------------- */
/*
 * Vince.exe creates one XAudio2 2.7 engine at startup (CoCreateInstance at +0x42EBC, on the main thread, before
 * Initialize) and keeps it until exit. It calls GetDeviceDetails(0) once at init (+0x42F64, mask for
 * X3DAudioInitialize), once per stream start (+0x5CE05, result unused) and once per new 2D sound (+0x448B8, mask picks
 * the pan layout for SetOutputMatrix). XAudio2 2.7 re-enumerates endpoints through MMDevAPI on every call (the 1.8 ms)
 * but only rebuilds its own device list when the number of active render endpoints changes, and never follows default
 * device changes; the mastering voice, the X3DAudio mask and the matrix size are all fixed at init. So the answer cached
 * from the init call is the one that matches the voice the game actually plays through, and the cache is only
 * refreshed when the game creates another engine.
 */
static const GUID CLSID_XAudio2_27 = {0x5a508685, 0xa254, 0x4fba, {0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf}};
static const GUID CLSID_XAudio2_27_Debug = {0xdb05ea35, 0x0329, 0x4d4b, {0xa5, 0x3a, 0x6d, 0xea, 0xd0, 0x3d, 0x38, 0x52}};
static const GUID IID_IXAudio2_27 = {0x8bcf1f58, 0x9fe7, 0x4583, {0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb}};

typedef struct {                  /* XAUDIO2_DEVICE_DETAILS (XAudio2 2.7) */
    WCHAR device_id[256], display_name[256];
    DWORD role;
    WORD format_tag, channels;    /* OutputFormat, a WAVEFORMATEXTENSIBLE: its WAVEFORMATEX ... */
    DWORD samples_per_second, bytes_per_second;
    WORD block_align, bits_per_sample, extra_size;
    WORD valid_bits_per_sample;   /* ... then Samples, */
    DWORD channel_mask;           /* dwChannelMask, the only field the game reads, */
    GUID sub_format;              /* and SubFormat */
} DeviceDetails;
_Static_assert(sizeof(DeviceDetails) == 0x42C && offset_of(DeviceDetails, role) == 0x400 &&
                   offset_of(DeviceDetails, channel_mask) == 0x418,
               "XAUDIO2_DEVICE_DETAILS layout");
#define SLOT_GET_DEVICE_DETAILS 4 /* IXAudio2 (2.7), counted in vincefix.h */
#define MAX_REFRESH_LOG_LINES 64
#define CLSCTX_INPROC_SERVER 0x1
#define GET_MODULE_HANDLE_EX_FLAG_PIN 0x1
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x4

typedef HRESULT(WINAPI *CoCreateInstanceFn)(const GUID *clsid, void *outer, DWORD context, const GUID *iid, void **object);
typedef HRESULT(WINAPI *GetDeviceDetailsFn)(void *self, DWORD index, DeviceDetails *details);

static CoCreateInstanceFn g_real_cocreate;

/* One entry per distinct engine vtable patched (normally one, xaudio2_7.dll+0x1390). Written only under g_patch_lock and
   published by g_vtable_count before the slot is patched, so the wrapper always finds its entry. */
static struct { void **vtable; GetDeviceDetailsFn real; } g_vtables[4];
static volatile long g_vtable_count;

static volatile long g_generation;    /* bumped for every new engine */
static void *g_cache_lock;            /* SRWLOCK */
static long g_cached_generation = -1; /* guarded by g_cache_lock */
static DeviceDetails g_cached;        /* guarded by g_cache_lock */
static volatile long g_hits_since_refresh;
static volatile long g_refresh_logged;
static DWORD g_ticks_per_us; /* QueryPerformanceFrequency / 1e6 (10 on current Windows); 32-bit math, no CRT helpers */

static int guid_equal(const GUID *a, const GUID *b) { return same_bytes(a, b, sizeof *a); }

static GetDeviceDetailsFn real_details_for(void *self) {
    void **vtable = *(void ***)self;
    long count = __atomic_load_n(&g_vtable_count, __ATOMIC_ACQUIRE);
    for (long i = 0; i < count; i++) if (g_vtables[i].vtable == vtable) return g_vtables[i].real;
    return count ? g_vtables[0].real : 0;
}

static HRESULT WINAPI cached_device_details(void *self, DWORD index, DeviceDetails *details) {
    GetDeviceDetailsFn real = real_details_for(self);
    if (!real) return E_FAIL; /* unreachable: the slot is patched only after an entry exists */
    if (index != 0 || !details) return real(self, index, details);
    long generation = __atomic_load_n(&g_generation, __ATOMIC_ACQUIRE);
    AcquireSRWLockExclusive(&g_cache_lock);
    int hit = g_cached_generation == generation;
    if (hit) copy_bytes(details, &g_cached, sizeof g_cached);
    ReleaseSRWLockExclusive(&g_cache_lock);
    if (hit) { __atomic_add_fetch(&g_hits_since_refresh, 1, __ATOMIC_RELAXED); return 0; }

    /* Miss: the caller gets exactly what XAudio2 writes (outside the lock), and a success is cached. XAudio2 writes all
       0x42C bytes, filling the arrays after each string's terminator with leftovers from its own stack; hits hand out
       that same copy. */
    long long start, end;
    QueryPerformanceCounter(&start);
    HRESULT result = real(self, index, details);
    QueryPerformanceCounter(&end);
    if (result >= 0) {
        AcquireSRWLockExclusive(&g_cache_lock);
        copy_bytes(&g_cached, details, sizeof g_cached);
        g_cached_generation = generation; /* tagged with the generation read before the call */
        ReleaseSRWLockExclusive(&g_cache_lock);
    }
    long logged = __atomic_fetch_add(&g_refresh_logged, 1, __ATOMIC_RELAXED);
    if (logged <= MAX_REFRESH_LOG_LINES) {
        Line line; begin(&line);
        if (logged == MAX_REFRESH_LOG_LINES) put(&line, "XAudio2 GetDeviceDetails(0): further refreshes are not logged");
        else {
            put(&line, "XAudio2 GetDeviceDetails(0): queried XAudio2 (");
            put_dec(&line, g_ticks_per_us ? (DWORD)(end - start) / g_ticks_per_us : 0, 0);
            put(&line, " us, ");
            if (result >= 0) { put(&line, "channel mask "); put_hex(&line, details->channel_mask); put(&line, ", now cached"); }
            else { put(&line, "FAILED "); put_hex(&line, (DWORD)result); put(&line, ", not cached"); }
            put(&line, "); "); put_dec(&line, (unsigned)__atomic_exchange_n(&g_hits_since_refresh, 0, __ATOMIC_RELAXED), 0);
            put(&line, " calls served from the cache before this");
        }
        emit(&line);
    }
    return result;
}

/* Runs on the thread that created the engine, before the game calls Initialize, so no XAudio2 thread exists yet.
   Serialized in case engines are ever created on several threads. */
static void *g_patch_lock; /* SRWLOCK */
static void patch_engine_locked(void **vtable, void *current);
static void patch_engine(void *engine) {
    void **vtable = *(void ***)engine;
    AcquireSRWLockExclusive(&g_patch_lock);
    void *current = vtable[SLOT_GET_DEVICE_DETAILS];
    if (current != (void *)cached_device_details) patch_engine_locked(vtable, current); /* else: a vtable patched before */
    ReleaseSRWLockExclusive(&g_patch_lock);
}
static void patch_engine_locked(void **vtable, void *current) {
    Line line; begin(&line);
    /* Pin the DLL: 2.7's DllCanUnloadNow ignores live engines, so CoFreeUnusedLibraries could unload it under the game
       (and take the patch and the saved pointer with it). */
    HMODULE module = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, vtable, &module) || !module) {
        put(&line, "audio: XAudio2 vtable "); put_hex(&line, (ULONG_PTR)vtable); put(&line, " is not inside a loaded module; left uncached");
        emit(&line); return;
    }
    BYTE *base = (BYTE *)module;
    if ((ULONG_PTR)current - (ULONG_PTR)base >= pe_headers(base)->image_size) {
        put(&line, "audio: XAudio2 GetDeviceDetails "); put_hex(&line, (ULONG_PTR)current);
        put(&line, " is outside the engine's module (hooked by another tool?); left uncached");
        emit(&line); return;
    }
    DWORD protect;
    /* In xaudio2_7.dll the vtable sits in .text (execute-read). */
    if (!VirtualProtect(&vtable[SLOT_GET_DEVICE_DETAILS], sizeof(void *), PAGE_EXECUTE_READWRITE, &protect)) {
        put(&line, "audio: VirtualProtect on the XAudio2 vtable failed, error "); put_dec(&line, GetLastError(), 0);
        put(&line, "; left uncached"); emit(&line); return;
    }
    /* Publish {vtable, real} before the slot points at the wrapper, so the wrapper always finds it. A vtable seen before
       (slot since restored by someone else) reuses its entry. */
    long count = g_vtable_count, entry = 0;
    while (entry < count && g_vtables[entry].vtable != vtable) entry++;
    if (entry == (long)(sizeof g_vtables / sizeof g_vtables[0])) {
        VirtualProtect(&vtable[SLOT_GET_DEVICE_DETAILS], sizeof(void *), protect, &protect);
        put(&line, "audio: too many XAudio2 vtables; this engine is left uncached"); emit(&line); return;
    }
    __atomic_store_n(&g_vtables[entry].real, (GetDeviceDetailsFn)current, __ATOMIC_RELEASE);
    if (entry == count) {
        g_vtables[entry].vtable = vtable;
        __atomic_store_n(&g_vtable_count, count + 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&vtable[SLOT_GET_DEVICE_DETAILS], (void *)cached_device_details, __ATOMIC_RELEASE);
    VirtualProtect(&vtable[SLOT_GET_DEVICE_DETAILS], sizeof(void *), protect, &protect);
    put(&line, "audio: XAudio2 module "); put_hex(&line, (ULONG_PTR)base); put(&line, " pinned; vtable at +");
    put_hex(&line, (ULONG_PTR)vtable - (ULONG_PTR)base); put(&line, ", slot 4 (GetDeviceDetails, +");
    put_hex(&line, (ULONG_PTR)current - (ULONG_PTR)base); put(&line, ") now cached");
    emit(&line);
}

static int is_xaudio2_27(const GUID *clsid, void *outer, DWORD context, const GUID *iid) {
    return !outer && (context & CLSCTX_INPROC_SERVER) && clsid && iid && guid_equal(iid, &IID_IXAudio2_27) &&
           (guid_equal(clsid, &CLSID_XAudio2_27) || guid_equal(clsid, &CLSID_XAudio2_27_Debug));
}

static HRESULT WINAPI hooked_cocreate(const GUID *clsid, void *outer, DWORD context, const GUID *iid, void **object) {
    HRESULT result = g_real_cocreate(clsid, outer, context, iid, object);
    if (result >= 0 && object && *object && is_xaudio2_27(clsid, outer, context, iid)) {
        patch_engine(*object);
        __atomic_add_fetch(&g_generation, 1, __ATOMIC_SEQ_CST); /* a new engine asks XAudio2 once more */
    }
    return result;
}

/* DllMain: routes Vince.exe's CoCreateInstance import through hooked_cocreate. */
void hook_xaudio(void) {
    long long frequency = 0;
    QueryPerformanceFrequency(&frequency);
    g_ticks_per_us = frequency > 0 && frequency < 0x100000000ll ? (DWORD)frequency / 1000000u : 0;
    /* Set before the slot changes so the hook never sees a null real function. */
    g_real_cocreate = (CoCreateInstanceFn)GetProcAddress(GetModuleHandleW(L"ole32.dll"), "CoCreateInstance");
    ULONG_PTR old = patch_import("ole32.dll", L"ole32.dll", "CoCreateInstance", (void *)hooked_cocreate, "now routed through VinceFix");
    if (old) g_real_cocreate = (CoCreateInstanceFn)old;
}
