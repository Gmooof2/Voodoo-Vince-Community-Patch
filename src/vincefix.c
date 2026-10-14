/*
 * VinceFix: stand-in X3DAudio1_7.dll for Voodoo Vince Remastered (32-bit).
 *
 * 1. Thread pinning: Vince.exe pins Main to CPU 0, WorkItem0-3 to CPUs 1-4 and both
 *    stream threads to CPU 4 through its import of kernel32!SetThreadAffinityMask.
 *    This replaces that one import slot in Vince.exe (not the kernel32 function, so
 *    drivers and overlays in the process keep their own affinity handling) with a
 *    no-op that leaves the thread free to run on any CPU.
 * 2. DPI: the game ignores display scaling and renders below native resolution.
 *    This marks the process per-monitor DPI aware before the game creates its window,
 *    the same mode the "Override high DPI scaling: Application" compat setting gives.
 * 3. X3DAudioInitialize / X3DAudioCalculate jump to the real DLL in the system
 *    directory (thunks.s), loaded on first use rather than in DllMain.
 * 4. Audio device details: the game calls IXAudio2::GetDeviceDetails(0) from its main
 *    thread about 40 times a minute (each new 2D sound), and XAudio2 2.7 re-enumerates
 *    the audio endpoints each time (about 1.8 ms). Vince.exe's ole32!CoCreateInstance
 *    import slot is routed through VinceFix; when it creates an XAudio2 2.7 engine,
 *    slot 4 of the engine's vtable is pointed at a cache of the index 0 result, filled
 *    by the game's own call at engine init and refreshed when it creates another engine.
 *
 * Log: VinceFix.log next to this DLL (or in %TEMP% if that folder is not writable).
 * Built freestanding (no CRT, no SDK headers); see build.ps1.
 */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef unsigned long ULONG_PTR;
typedef void *HANDLE;
typedef void *HMODULE;
typedef unsigned short WCHAR; /* type of L"" literals on this target */

#define WINAPI __stdcall
#define IMPORT __declspec(dllimport)

typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;

IMPORT HMODULE WINAPI GetModuleHandleW(const WCHAR *name);
IMPORT void *WINAPI GetProcAddress(HMODULE module, const char *name);
IMPORT HMODULE WINAPI LoadLibraryW(const WCHAR *path);
IMPORT DWORD WINAPI GetSystemDirectoryW(WCHAR *buffer, DWORD size);
IMPORT DWORD WINAPI GetModuleFileNameW(HMODULE module, WCHAR *buffer, DWORD size);
IMPORT DWORD WINAPI GetTempPathW(DWORD size, WCHAR *buffer);
IMPORT BOOL WINAPI VirtualProtect(void *address, ULONG_PTR size, DWORD protect, DWORD *old);
IMPORT BOOL WINAPI GetProcessAffinityMask(HANDLE process, ULONG_PTR *processMask, ULONG_PTR *systemMask);
IMPORT HANDLE WINAPI GetCurrentProcess(void);
IMPORT DWORD WINAPI GetCurrentProcessId(void);
IMPORT DWORD WINAPI GetThreadId(HANDLE thread);
IMPORT HANDLE WINAPI CreateFileW(const WCHAR *path, DWORD access, DWORD share, void *security, DWORD disposition, DWORD flags, HANDLE templ);
IMPORT BOOL WINAPI WriteFile(HANDLE file, const void *data, DWORD size, DWORD *written, void *overlapped);
IMPORT BOOL WINAPI CloseHandle(HANDLE handle);
IMPORT void WINAPI GetLocalTime(SYSTEMTIME *time);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT BOOL WINAPI DisableThreadLibraryCalls(HMODULE module);
IMPORT void WINAPI ExitProcess(DWORD code);
IMPORT void WINAPI AcquireSRWLockExclusive(void **lock);
IMPORT void WINAPI ReleaseSRWLockExclusive(void **lock);
IMPORT BOOL WINAPI QueryPerformanceCounter(long long *count);
IMPORT BOOL WINAPI QueryPerformanceFrequency(long long *frequency);
IMPORT BOOL WINAPI SetProcessDPIAware(void); /* static user32 import: user32 initializes before this DLL */
IMPORT int WINAPI MessageBoxW(HANDLE owner, const WCHAR *text, const WCHAR *caption, DWORD type);

int _fltused = 0; /* referenced by the compiler when floats appear; no CRT here */

/* Read by thunks.s */
void *vf_real_init;
void *vf_real_calc;

static HMODULE g_self;
static WCHAR g_log_path[520];
static ULONG_PTR g_exe_base;

/* ---- logging ------------------------------------------------------------ */

typedef struct { char text[256]; unsigned length; } Line;

static void put(Line *line, const char *s) { while (*s && line->length < sizeof line->text - 2) line->text[line->length++] = *s++; }
static void put_dec(Line *line, unsigned long long v, int width) {
    char digits[24]; int n = 0;
    do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n < width) digits[n++] = '0';
    while (n && line->length < sizeof line->text - 2) line->text[line->length++] = digits[--n];
}
static void put_hex(Line *line, unsigned long long v) {
    char digits[20]; int n = 0;
    do { digits[n++] = "0123456789ABCDEF"[v & 15]; v >>= 4; } while (v);
    put(line, "0x");
    while (n && line->length < sizeof line->text - 2) line->text[line->length++] = digits[--n];
}
static void begin(Line *line) {
    SYSTEMTIME t; GetLocalTime(&t);
    line->length = 0;
    put_dec(line, t.wYear, 4); put(line, "-"); put_dec(line, t.wMonth, 2); put(line, "-"); put_dec(line, t.wDay, 2); put(line, " ");
    put_dec(line, t.wHour, 2); put(line, ":"); put_dec(line, t.wMinute, 2); put(line, ":"); put_dec(line, t.wSecond, 2); put(line, ".");
    put_dec(line, t.wMilliseconds, 3); put(line, "  pid "); put_dec(line, GetCurrentProcessId(), 0); put(line, "  ");
}
static void emit(Line *line) {
    line->text[line->length++] = '\r'; line->text[line->length++] = '\n';
    if (!g_log_path[0]) return;
    HANDLE f = CreateFileW(g_log_path, 0x4 /* FILE_APPEND_DATA */, 0x3, 0, 4 /* OPEN_ALWAYS */, 0x80, 0);
    if (f == (HANDLE)-1) return;
    DWORD written; WriteFile(f, line->text, line->length, &written, 0); CloseHandle(f);
}

static unsigned wlen(const WCHAR *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int append_name(WCHAR *dir, unsigned capacity, const WCHAR *name) {
    unsigned n = wlen(dir), m = wlen(name);
    if (n + m + 2 > capacity) return 0;
    if (n && dir[n - 1] != L'\\') dir[n++] = L'\\';
    for (unsigned i = 0; i <= m; i++) dir[n + i] = name[i];
    return 1;
}

static void init_log_path(void) {
    DWORD n = GetModuleFileNameW(g_self, g_log_path, 520);
    while (n && g_log_path[n - 1] != L'\\') n--;
    g_log_path[n] = 0;
    if (n && append_name(g_log_path, 520, L"VinceFix.log")) {
        HANDLE f = CreateFileW(g_log_path, 0x4, 0x3, 0, 4, 0x80, 0);
        if (f != (HANDLE)-1) { CloseHandle(f); return; }
    }
    g_log_path[0] = 0;
    if (GetTempPathW(520, g_log_path) && append_name(g_log_path, 520, L"VinceFix.log")) return;
    g_log_path[0] = 0;
}

/* ---- X3DAudio forwarding (called from thunks.s on first use) ------------- */

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
        /* Without the real DLL there is nothing sensible to forward to, and returning
           would leave the game's audio settings uninitialized. Without VinceFix the
           game would refuse to start here, so fail just as clearly. */
        put(&line, "X3DAudio: ERROR loading the system X3DAudio1_7.dll (error "); put_dec(&line, GetLastError(), 0);
        put(&line, "); exiting");
        emit(&line);
        MessageBoxW(0, L"VinceFix (X3DAudio1_7.dll in the game folder) could not load the system copy of "
                       L"X3DAudio1_7.dll from the DirectX June 2010 runtime.\n\nReinstall the DirectX runtime, "
                       L"or delete X3DAudio1_7.dll from the game folder to remove VinceFix.",
                    L"VinceFix", 0x10 /* MB_ICONERROR */);
        ExitProcess(1);
    }
    emit(&line);
    vf_real_calc = calc;
    vf_real_init = init;
}

/* ---- thread pinning ----------------------------------------------------- */

#define MAX_AFFINITY_LOG_LINES 32 /* the game makes 7 calls; cap in case a future version calls it per frame */
static volatile int g_affinity_logged;

static ULONG_PTR WINAPI no_affinity(HANDLE thread, ULONG_PTR requested) {
    ULONG_PTR process_mask = 0, system_mask = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
    int count = g_affinity_logged++;
    if (count > MAX_AFFINITY_LOG_LINES) return process_mask ? process_mask : 1;
    Line line; begin(&line);
    if (count == MAX_AFFINITY_LOG_LINES) {
        put(&line, "SetThreadAffinityMask: further ignored calls are not logged");
        emit(&line);
        return process_mask ? process_mask : 1;
    }
    put(&line, "SetThreadAffinityMask ignored: tid "); put_dec(&line, GetThreadId(thread), 0);
    put(&line, " requested "); put_hex(&line, requested);
    put(&line, " from Vince.exe+"); put_hex(&line, (ULONG_PTR)__builtin_return_address(0) - g_exe_base);
    emit(&line);
    return process_mask ? process_mask : 1; /* the thread's mask is still the process mask; nonzero = success */
}

static int names_equal(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* Points the host exe's import slot(s) for dll!function at replacement and returns
   the address the first patched slot held (0 if none was patched). `now` describes
   the replacement for the log. */
static ULONG_PTR patch_import(const char *dll, const WCHAR *dll_wide, const char *function, void *replacement, const char *now) {
    BYTE *base = (BYTE *)g_exe_base;
    DWORD nt = *(DWORD *)(base + 0x3C);
    DWORD imports_rva = *(DWORD *)(base + nt + 24 + 104); /* optional header data directory 1 */
    /* The loader resolves the exe's imports before running DLL initializers, so the
       slot should already hold this address; it is also how a slot is identified
       when the exe has no import-name table. */
    HMODULE module = GetModuleHandleW(dll_wide);
    ULONG_PTR resolved = module ? (ULONG_PTR)GetProcAddress(module, function) : 0;
    ULONG_PTR first_old = 0;
    Line line; begin(&line);
    for (DWORD *d = (DWORD *)(base + imports_rva); imports_rva && d[3]; d += 5) {
        if (!names_equal((const char *)(base + d[3]), dll)) continue;
        DWORD *names = d[0] ? (DWORD *)(base + d[0]) : 0; /* the FirstThunk array is already resolved; never read names from it */
        ULONG_PTR *slots = (ULONG_PTR *)(base + d[4]);
        for (int i = 0; names ? names[i] != 0 : slots[i] != 0; i++) {
            int match = names ? !(names[i] & 0x80000000u) && names_equal((const char *)(base + names[i] + 2), function)
                              : resolved && slots[i] == resolved;
            if (!match) continue;
            ULONG_PTR old = slots[i];
            DWORD protect;
            if (VirtualProtect(&slots[i], sizeof slots[i], 0x04 /* PAGE_READWRITE */, &protect)) {
                slots[i] = (ULONG_PTR)replacement;
                VirtualProtect(&slots[i], sizeof slots[i], protect, &protect);
                put(&line, "hook: Vince.exe import "); put(&line, function); put(&line, " at +"); put_hex(&line, (ULONG_PTR)&slots[i] - (ULONG_PTR)base);
                put(&line, " "); put(&line, now); put(&line, " (was "); put_hex(&line, old);
                put(&line, old == resolved ? ", " : ", NOT "); put(&line, dll);
                put(&line, old == resolved ? "'s)" : "'s: unresolved or hooked by another tool)");
                if (!first_old) first_old = old;
            } else {
                put(&line, "hook: VirtualProtect failed, error "); put_dec(&line, GetLastError(), 0);
            }
        }
    }
    if (!first_old) { put(&line, "hook: no "); put(&line, function); put(&line, " import found in the host; not installed"); }
    emit(&line);
    return first_old;
}

/* ---- XAudio2 device details cache --------------------------------------- */
/*
 * Vince.exe creates one XAudio2 2.7 engine at startup (CoCreateInstance at +0x42EBC,
 * on the main thread, before Initialize) and keeps it until exit. It calls
 * GetDeviceDetails(0) once at init (+0x42F64, mask for X3DAudioInitialize), once per
 * stream start (+0x5CE05, result unused) and once per new 2D sound (+0x448B8, mask
 * picks the pan layout for SetOutputMatrix). XAudio2 2.7 re-enumerates endpoints
 * through MMDevAPI on every call (the 1.8 ms) but only rebuilds its own device list
 * when the number of active render endpoints changes, and never follows default
 * device changes; the mastering voice, the X3DAudio mask and the matrix size are all
 * fixed at init. So the answer cached from the init call is the one that matches the
 * voice the game actually plays through, and the cache is only refreshed when the
 * game creates another engine.
 */
typedef long HRESULT;
typedef struct { DWORD data1; WORD data2, data3; BYTE data4[8]; } GUID;
IMPORT BOOL WINAPI GetModuleHandleExW(DWORD flags, const void *name, HMODULE *module);

static const GUID CLSID_XAudio2_27 = {0x5a508685, 0xa254, 0x4fba, {0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf}};
static const GUID CLSID_XAudio2_27_Debug = {0xdb05ea35, 0x0329, 0x4d4b, {0xa5, 0x3a, 0x6d, 0xea, 0xd0, 0x3d, 0x38, 0x52}};
static const GUID IID_IXAudio2_27 = {0x8bcf1f58, 0x9fe7, 0x4583, {0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb}};

static int guid_equal(const GUID *a, const GUID *b) {
    const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}
static void copy_bytes(BYTE *to, const BYTE *from, unsigned n) { while (n--) *to++ = *from++; }

#define DETAILS_SIZE 0x42C         /* XAUDIO2_DEVICE_DETAILS: DeviceID[256], DisplayName[256], Role, WAVEFORMATEXTENSIBLE */
#define DETAILS_CHANNEL_MASK 0x418 /* OutputFormat.dwChannelMask, the only field the game reads */
#define SLOT_GET_DEVICE_DETAILS 4  /* IXAudio2 (2.7): QueryInterface, AddRef, Release, GetDeviceCount, GetDeviceDetails, ... */
#define MAX_REFRESH_LOG_LINES 64

typedef HRESULT(WINAPI *CoCreateInstanceFn)(const GUID *clsid, void *outer, DWORD context, const GUID *iid, void **object);
typedef HRESULT(WINAPI *GetDeviceDetailsFn)(void *self, DWORD index, BYTE *details);

static CoCreateInstanceFn g_real_cocreate;

/* One entry per distinct engine vtable patched (normally one, xaudio2_7.dll+0x1390). Written
   only under g_patch_lock and published by g_vtable_count before the slot is patched, so the
   wrapper always finds its entry. */
static struct { void **vtable; GetDeviceDetailsFn real; } g_vtables[4];
static volatile long g_vtable_count;

static volatile long g_generation;    /* bumped for every new engine */
static void *g_cache_lock;            /* SRWLOCK */
static long g_cached_generation = -1; /* guarded by g_cache_lock */
static BYTE g_cached[DETAILS_SIZE];   /* guarded by g_cache_lock */
static volatile long g_hits_since_refresh;
static volatile long g_refresh_logged;
static DWORD g_ticks_per_us; /* QueryPerformanceFrequency / 1e6 (10 on current Windows); 32-bit math, no CRT helpers */

static GetDeviceDetailsFn real_details_for(void *self) {
    void **vtable = *(void ***)self;
    long count = __atomic_load_n(&g_vtable_count, __ATOMIC_ACQUIRE);
    for (long i = 0; i < count; i++) if (g_vtables[i].vtable == vtable) return g_vtables[i].real;
    return count ? g_vtables[0].real : 0;
}

static HRESULT WINAPI cached_device_details(void *self, DWORD index, BYTE *details) {
    GetDeviceDetailsFn real = real_details_for(self);
    if (!real) return (HRESULT)0x80004005; /* E_FAIL; unreachable: the slot is patched only after an entry exists */
    if (index != 0 || !details) return real(self, index, details);
    long generation = __atomic_load_n(&g_generation, __ATOMIC_ACQUIRE);
    AcquireSRWLockExclusive(&g_cache_lock);
    int hit = g_cached_generation == generation;
    if (hit) copy_bytes(details, g_cached, DETAILS_SIZE);
    ReleaseSRWLockExclusive(&g_cache_lock);
    if (hit) { __atomic_add_fetch(&g_hits_since_refresh, 1, __ATOMIC_RELAXED); return 0; }

    /* Miss: the caller gets exactly what XAudio2 writes (outside the lock), and a success is
       cached. XAudio2 writes all 0x42C bytes, filling the arrays after each string's
       terminator with leftovers from its own stack; hits hand out that same copy. */
    long long start, end;
    QueryPerformanceCounter(&start);
    HRESULT result = real(self, index, details);
    QueryPerformanceCounter(&end);
    if (result >= 0) {
        AcquireSRWLockExclusive(&g_cache_lock);
        copy_bytes(g_cached, details, DETAILS_SIZE);
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
            if (result >= 0) { put(&line, "channel mask "); put_hex(&line, *(DWORD *)(details + DETAILS_CHANNEL_MASK)); put(&line, ", now cached"); }
            else { put(&line, "FAILED "); put_hex(&line, (DWORD)result); put(&line, ", not cached"); }
            put(&line, "); "); put_dec(&line, (unsigned)__atomic_exchange_n(&g_hits_since_refresh, 0, __ATOMIC_RELAXED), 0);
            put(&line, " calls served from the cache before this");
        }
        emit(&line);
    }
    return result;
}

/* Runs on the thread that created the engine, before the game calls Initialize, so no
   XAudio2 thread exists yet. Serialized in case engines are ever created on several threads. */
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
    /* Pin the DLL: 2.7's DllCanUnloadNow ignores live engines, so CoFreeUnusedLibraries could
       unload it under the game (and take the patch and the saved pointer with it). */
    HMODULE module = 0;
    if (!GetModuleHandleExW(0x4 /* FROM_ADDRESS */ | 0x1 /* PIN */, vtable, &module) || !module) {
        put(&line, "audio: XAudio2 vtable "); put_hex(&line, (ULONG_PTR)vtable); put(&line, " is not inside a loaded module; left uncached");
        emit(&line); return;
    }
    BYTE *base = (BYTE *)module;
    DWORD nt = *(DWORD *)(base + 0x3C), image_size = *(DWORD *)(base + nt + 24 + 56);
    if ((ULONG_PTR)current - (ULONG_PTR)base >= image_size) {
        put(&line, "audio: XAudio2 GetDeviceDetails "); put_hex(&line, (ULONG_PTR)current);
        put(&line, " is outside the engine's module (hooked by another tool?); left uncached");
        emit(&line); return;
    }
    DWORD protect;
    /* In xaudio2_7.dll the vtable sits in .text (execute-read). */
    if (!VirtualProtect(&vtable[SLOT_GET_DEVICE_DETAILS], sizeof(void *), 0x40 /* PAGE_EXECUTE_READWRITE */, &protect)) {
        put(&line, "audio: VirtualProtect on the XAudio2 vtable failed, error "); put_dec(&line, GetLastError(), 0);
        put(&line, "; left uncached"); emit(&line); return;
    }
    /* Publish {vtable, real} before the slot points at the wrapper, so the wrapper always finds it.
       A vtable seen before (slot since restored by someone else) reuses its entry. */
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

static HRESULT WINAPI hooked_cocreate(const GUID *clsid, void *outer, DWORD context, const GUID *iid, void **object) {
    HRESULT result = g_real_cocreate(clsid, outer, context, iid, object);
    if (result >= 0 && object && *object && !outer && (context & 0x1 /* CLSCTX_INPROC_SERVER */) && clsid && iid && guid_equal(iid, &IID_IXAudio2_27) &&
        (guid_equal(clsid, &CLSID_XAudio2_27) || guid_equal(clsid, &CLSID_XAudio2_27_Debug))) {
        patch_engine(*object);
        __atomic_add_fetch(&g_generation, 1, __ATOMIC_SEQ_CST); /* a new engine asks XAudio2 once more */
    }
    return result;
}

static void install_hooks(void) {
    BYTE *base = (BYTE *)GetModuleHandleW(0);
    DWORD nt = *(DWORD *)(base + 0x3C);
    if (*(DWORD *)(base + nt) != 0x00004550 || *(WORD *)(base + nt + 24) != 0x10B) {
        Line line; begin(&line); put(&line, "hook: host is not a PE32 image; not installed"); emit(&line); return;
    }
    g_exe_base = (ULONG_PTR)base;
    long long frequency = 0;
    QueryPerformanceFrequency(&frequency);
    g_ticks_per_us = frequency > 0 && frequency < 0x100000000ll ? (DWORD)frequency / 1000000u : 0;
    patch_import("kernel32.dll", L"kernel32.dll", "SetThreadAffinityMask", (void *)no_affinity, "now a no-op");
    /* Set before the slot changes so the hook never sees a null real function. */
    g_real_cocreate = (CoCreateInstanceFn)GetProcAddress(GetModuleHandleW(L"ole32.dll"), "CoCreateInstance");
    ULONG_PTR old = patch_import("ole32.dll", L"ole32.dll", "CoCreateInstance", (void *)hooked_cocreate, "now routed through VinceFix");
    if (old) g_real_cocreate = (CoCreateInstanceFn)old;
}

/* ---- DPI ---------------------------------------------------------------- */

static void set_dpi_awareness(void) {
    typedef BOOL(WINAPI * SetContext)(HANDLE context);
    typedef HANDLE(WINAPI * GetContext)(void);
    typedef int(WINAPI * AwarenessOf)(HANDLE context);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    SetContext set_context = (SetContext)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    GetContext get_context = (GetContext)GetProcAddress(user32, "GetThreadDpiAwarenessContext");
    AwarenessOf awareness_of = (AwarenessOf)GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext");
    Line line; begin(&line);
    if (set_context) {
        if (set_context((HANDLE)-3 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE */)) put(&line, "DPI: process set to per-monitor DPI aware");
        else {
            DWORD error = GetLastError();
            put(&line, error == 5 ? "DPI: awareness was already set (compat setting or manifest); left as is"
                                  : "DPI: SetProcessDpiAwarenessContext failed, error ");
            if (error != 5) put_dec(&line, error, 0);
        }
    } else {
        put(&line, SetProcessDPIAware() ? "DPI: process set to system DPI aware (older Windows)" : "DPI: SetProcessDPIAware failed");
    }
    emit(&line);
    /* Report what is actually in effect: a "System" or "System (Enhanced)" compat
       override keeps the process unaware even though the call above was refused politely. */
    if (get_context && awareness_of) {
        int awareness = awareness_of(get_context());
        begin(&line);
        put(&line, "DPI: effective awareness "); put_dec(&line, (unsigned)awareness, 0);
        put(&line, awareness == 2 ? " (per-monitor; game renders at native resolution)"
                   : " -- WARNING: not per-monitor; display scaling fix NOT active (another compatibility setting overrides it)");
        emit(&line);
    }
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, void *reserved) {
    (void)reserved;
    if (reason == 1 /* DLL_PROCESS_ATTACH */) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        init_log_path();
        Line line; begin(&line); put(&line, "VinceFix 1.2 loaded"); emit(&line); /* also the install.ps1 marker */
        set_dpi_awareness();
        install_hooks();
    }
    return 1;
}
