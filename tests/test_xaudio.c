/*
 * XAudio2 test host for VinceFix 1.2: imports X3DAudio1_7.dll at load time and
 * ole32!CoCreateInstance like Vince.exe (MTA, CLSCTX_INPROC_SERVER, IID_IXAudio2), then
 * checks the GetDeviceDetails cache against the real xaudio2_7.dll (DirectX June 2010):
 *   1. an engine made with ole32's own CoCreateInstance (not through this exe's import)
 *      is left alone and gives the baseline answers,
 *   2. creations through the import that are not an IXAudio2 2.7 engine pass straight
 *      through and patch nothing,
 *   3. an engine made through the import gets slot 4 routed into VinceFix, with slot 3
 *      and the page protection unchanged,
 *   4. GetDeviceDetails(0) returns S_OK and the baseline bytes (every byte, over a
 *      poisoned buffer), served from the cache after the first call and much faster;
 *      other indexes return what they returned before,
 *   5. 8 threads calling at once get the baseline bytes, both with a warm cache and
 *      racing the refresh right after another engine is created,
 *   6. xaudio2_7.dll stays loaded and patched after CoFreeUnusedLibrariesEx.
 * build.ps1 also checks VinceFix.log: one import hook, one vtable patch, 2-9 refreshes.
 * Exit code = number of failed checks.
 */
typedef unsigned int DWORD;
typedef int BOOL;
typedef unsigned long ULONG_PTR;
typedef void *HANDLE;
typedef void *HMODULE;
typedef unsigned short WCHAR;
typedef unsigned char BYTE;
typedef long HRESULT;
#define WINAPI __stdcall
#define IMPORT __declspec(dllimport)

typedef struct { DWORD data1; unsigned short data2, data3; BYTE data4[8]; } GUID;
typedef struct { void *base, *allocation_base; DWORD allocation_protect; ULONG_PTR region_size; DWORD state, protect, type; } MEMORY_BASIC_INFORMATION;

IMPORT void __cdecl X3DAudioInitialize(DWORD mask, float speed, unsigned char *handle);
IMPORT HRESULT WINAPI CoInitializeEx(void *reserved, DWORD model);
IMPORT HRESULT WINAPI CoCreateInstance(const GUID *clsid, void *outer, DWORD context, const GUID *iid, void **object);
IMPORT void WINAPI CoFreeUnusedLibrariesEx(DWORD delay, DWORD reserved);
IMPORT HMODULE WINAPI GetModuleHandleW(const WCHAR *name);
IMPORT void *WINAPI GetProcAddress(HMODULE module, const char *name);
IMPORT HANDLE WINAPI GetStdHandle(DWORD which);
IMPORT BOOL WINAPI WriteFile(HANDLE file, const void *data, DWORD size, DWORD *written, void *overlapped);
IMPORT void WINAPI ExitProcess(DWORD code);
IMPORT HANDLE WINAPI CreateThread(void *security, ULONG_PTR stack, DWORD(WINAPI *start)(void *), void *arg, DWORD flags, DWORD *id);
IMPORT DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD ms);
IMPORT BOOL WINAPI CloseHandle(HANDLE handle);
IMPORT ULONG_PTR WINAPI VirtualQuery(const void *address, MEMORY_BASIC_INFORMATION *info, ULONG_PTR size);
IMPORT BOOL WINAPI QueryPerformanceCounter(long long *count);
IMPORT BOOL WINAPI QueryPerformanceFrequency(long long *frequency);
IMPORT void WINAPI Sleep(DWORD ms);

int _fltused = 0;
static int failures;
static void out(const char *s) { DWORD n = 0, w; while (s[n]) n++; WriteFile(GetStdHandle((DWORD)-11), s, n, &w, 0); }
static void out_hex(ULONG_PTR v) { char b[12]; int i = 11; b[i] = 0; do { b[--i] = "0123456789ABCDEF"[v & 15]; v >>= 4; } while (v); out("0x"); out(b + i); }
static void out_dec(DWORD v) { char b[12]; int i = 11; b[i] = 0; do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v); out(b + i); }
static void check(int ok, const char *what) { out(ok ? "PASS  " : "FAIL  "); out(what); out("\r\n"); if (!ok) failures++; }

static const GUID CLSID_XAudio2 = {0x5a508685, 0xa254, 0x4fba, {0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf}};
static const GUID IID_IXAudio2 = {0x8bcf1f58, 0x9fe7, 0x4583, {0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb}};
static const GUID IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID CLSID_MMDeviceEnumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const GUID IID_IMMDeviceEnumerator = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const GUID CLSID_Nonexistent = {0x0badf00d, 0x1234, 0x5678, {0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78}};

#define SIZE 0x42C /* XAUDIO2_DEVICE_DETAILS */
#define VT(object) (*(void ***)(object))
typedef HRESULT(WINAPI *CoCreateFn)(const GUID *, void *, DWORD, const GUID *, void **);
typedef HRESULT(WINAPI *DetailsFn)(void *, DWORD, BYTE *);
typedef HRESULT(WINAPI *CountFn)(void *, DWORD *);
typedef HRESULT(WINAPI *InitializeFn)(void *, DWORD, DWORD);
typedef DWORD(WINAPI *ReleaseFn)(void *);

static HRESULT details(void *engine, DWORD index, BYTE *buffer, BYTE fill) {
    for (int i = 0; i < SIZE; i++) ((volatile BYTE *)buffer)[i] = fill;
    return ((DetailsFn)VT(engine)[4])(engine, index, buffer);
}
static int g_diffs_shown;
static int same(const BYTE *a, const BYTE *b) {
    for (int i = 0; i < SIZE; i++) {
        if (a[i] == b[i]) continue;
        if (g_diffs_shown++ < 3) {
            int n = 0;
            for (int j = i; j < SIZE; j++) n += a[j] != b[j];
            out("      first difference at +"); out_hex((DWORD)i); out(" ("); out_dec((DWORD)n); out(" bytes differ from there):\r\n      got  ");
            for (int j = i & ~15; j < (i & ~15) + 32 && j < SIZE; j++) { out_hex(a[j]); out(" "); }
            out("\r\n      want ");
            for (int j = i & ~15; j < (i & ~15) + 32 && j < SIZE; j++) { out_hex(b[j]); out(" "); }
            out("\r\n");
        }
        return 0;
    }
    return 1;
}
/* What a caller can use: DeviceID and DisplayName up to their terminators, Role and OutputFormat.
   XAudio2 fills the rest of the string arrays with leftovers from its own stack, which differ
   from call site to call site. */
static int same_string(const BYTE *a, const BYTE *b, int offset) {
    const unsigned short *x = (const unsigned short *)(a + offset), *y = (const unsigned short *)(b + offset);
    for (int i = 0; i < 256; i++) { if (x[i] != y[i]) return 0; if (!x[i]) return 1; }
    return 1;
}
static int same_details(const BYTE *a, const BYTE *b) {
    int ok = same_string(a, b, 0) && same_string(a, b, 0x200);
    for (int i = 0x400; ok && i < SIZE; i++) ok = a[i] == b[i];
    if (!ok && g_diffs_shown++ < 3) { out("      differs in the strings, role or format; full diff:\r\n"); g_diffs_shown--; same(a, b); }
    return ok;
}
static HRESULT initialize(void *engine) { return ((InitializeFn)VT(engine)[5])(engine, 0, 0xFFFFFFFFu /* XAUDIO2_DEFAULT_PROCESSOR */); }
static void release(void *engine) { if (engine) ((ReleaseFn)VT(engine)[2])(engine); }

static BYTE g_base[SIZE]; /* XAudio2's own answer (engine A, before the patch) */
static BYTE g_ref[SIZE];  /* the first answer through VinceFix: every cached copy must match it byte for byte */
static int g_full;        /* threads compare with g_ref byte for byte (warm cache) or with g_base field by field */
static void *g_engine;
static volatile long g_go, g_errors;
static DWORD WINAPI worker(void *arg) {
    BYTE buffer[SIZE];
    while (!__atomic_load_n(&g_go, __ATOMIC_ACQUIRE)) {}
    for (int i = 0; i < 2000; i++) {
        HRESULT hr = details(g_engine, 0, buffer, (BYTE)(ULONG_PTR)arg); /* per-thread poison catches partial copies */
        if (hr != 0 || !(g_full ? same(buffer, g_ref) : same_details(buffer, g_base))) __atomic_add_fetch(&g_errors, 1, __ATOMIC_RELAXED);
    }
    return 0;
}
static long run_threads(void *engine) {
    HANDLE threads[8];
    g_engine = engine; g_go = 0; g_errors = 0;
    for (int i = 0; i < 8; i++) threads[i] = CreateThread(0, 0, worker, (void *)(ULONG_PTR)(0xA0 + i), 0, 0);
    __atomic_store_n(&g_go, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < 8; i++) {
        if (!threads[i]) { g_errors += 1000000; continue; }
        if (WaitForSingleObject(threads[i], 60000) != 0) g_errors += 1000000;
        CloseHandle(threads[i]);
    }
    return g_errors;
}

void WINAPI start(void) {
    unsigned char x3d[20] = {0};
    X3DAudioInitialize(0x3, 343.5f, x3d); /* keeps the load-time import of the DLL under test */

    HMODULE fix = GetModuleHandleW(L"X3DAudio1_7.dll");
    BYTE *fix_base = (BYTE *)fix;
    DWORD fix_size = fix ? *(DWORD *)(fix_base + *(DWORD *)(fix_base + 0x3C) + 24 + 56) : 0;
#define IN_FIX(p) ((ULONG_PTR)(p) - (ULONG_PTR)fix_base < fix_size)
    long long frequency = 0, t0, t1;
    QueryPerformanceFrequency(&frequency);
    DWORD ticks_per_us = (DWORD)frequency / 1000000u;
    if (!ticks_per_us) ticks_per_us = 1;

    check(CoInitializeEx(0, 0 /* COINIT_MULTITHREADED, as the game */) >= 0, "CoInitializeEx (MTA)");
    CoCreateFn real_cocreate = (CoCreateFn)GetProcAddress(GetModuleHandleW(L"ole32.dll"), "CoCreateInstance");

    /* 1. baseline engine, created around the hooked import */
    void *a = 0;
    HRESULT hr = real_cocreate(&CLSID_XAudio2, 0, 1 /* CLSCTX_INPROC_SERVER */, &IID_IXAudio2, &a);
    check(hr >= 0 && a, "engine A created with ole32's own CoCreateInstance");
    if (!a) { out("XAudio2 2.7 is not available (DirectX June 2010 runtime missing?)\r\n"); ExitProcess(100); }
    check(initialize(a) >= 0, "engine A Initialize");
    void **vtable = VT(a);
    void *original4 = vtable[4], *original3 = vtable[3];
    check(!IN_FIX(original4), "before any engine goes through the import, slot 4 is XAudio2's own");
    MEMORY_BASIC_INFORMATION before = {0}, after = {0};
    VirtualQuery(&vtable[4], &before, sizeof before);
    DWORD count = 0;
    ((CountFn)original3)(a, &count);
    BYTE base1[SIZE], buffer[SIZE];
    QueryPerformanceCounter(&t0);
    HRESULT base_hr = details(a, 0, g_base, 0);
    QueryPerformanceCounter(&t1);
    DWORD base_ticks = (DWORD)(t1 - t0);
    HRESULT base1_hr = count > 1 ? details(a, 1, base1, 0) : 0;
    HRESULT bad_hr = details(a, count, buffer, 0);
    out("      devices "); out_dec(count); out(", channel mask "); out_hex(*(DWORD *)(g_base + 0x418));
    out(", XAudio2's own call "); out_dec(base_ticks / ticks_per_us); out(" us, out-of-range index -> "); out_hex((DWORD)bad_hr); out("\r\n");
    check(base_hr == 0, "baseline GetDeviceDetails(0) succeeds");
    hr = details(a, 0, buffer, 0);
    check(hr == 0 && same(buffer, g_base), "XAudio2's own answer is repeatable (engine A, second call)");
    hr = details(a, 0, buffer, 0x77);
    check(hr == 0 && same(buffer, g_base), "XAudio2's own answer fills every byte (engine A, poisoned buffer)");
    check(bad_hr < 0, "baseline out-of-range index fails");

    /* 2. creations through the import that must pass straight through */
    void *p = (void *)1, *q = (void *)1;
    HRESULT h1 = real_cocreate(&CLSID_Nonexistent, 0, 1, &IID_IUnknown, &p);
    HRESULT h2 = CoCreateInstance(&CLSID_Nonexistent, 0, 1, &IID_IUnknown, &q);
    check(h1 < 0 && h1 == h2, "unknown CLSID: same failure through the import");
    void *unknown = 0;
    hr = CoCreateInstance(&CLSID_XAudio2, 0, 1, &IID_IUnknown, &unknown);
    check(hr >= 0 && unknown && vtable[4] == original4, "XAudio2 asked for IUnknown: created, nothing patched");
    release(unknown);
    void *aggregated = 0;
    hr = CoCreateInstance(&CLSID_XAudio2, a /* any live IUnknown as outer */, 1, &IID_IXAudio2, &aggregated);
    check(vtable[4] == original4, "XAudio2 with an outer object: nothing patched");
    if (hr >= 0) release(aggregated);
    hr = CoCreateInstance(&CLSID_XAudio2, 0, 1, &IID_IXAudio2, 0);
    check(hr < 0 && vtable[4] == original4, "XAudio2 with a NULL out pointer: fails, nothing patched");
    void *enumerator = 0;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, 0, 1, &IID_IMMDeviceEnumerator, &enumerator);
    check(hr >= 0 && enumerator && vtable[4] == original4, "another class (MMDeviceEnumerator): created, nothing patched");
    release(enumerator);

    /* 3. engine B through the import */
    void *b = 0;
    hr = CoCreateInstance(&CLSID_XAudio2, 0, 1, &IID_IXAudio2, &b);
    check(hr >= 0 && b, "engine B created through the import");
    if (!b) ExitProcess(100 + (DWORD)failures);
    check(initialize(b) >= 0, "engine B Initialize");
    check(VT(b) == vtable, "B shares A's vtable");
    check(IN_FIX(vtable[4]), "slot 4 (GetDeviceDetails) now points into VinceFix");
    check(vtable[3] == original3, "slot 3 (GetDeviceCount) untouched");
    VirtualQuery(&vtable[4], &after, sizeof after);
    out("      vtable page protection before "); out_hex(before.protect); out(", after "); out_hex(after.protect); out("\r\n");
    check(after.protect == before.protect, "vtable page protection restored");

    /* 4. answers */
    hr = details(b, 0, buffer, 0xCD);
    check(hr == 0 && same_details(buffer, g_base), "B GetDeviceDetails(0): S_OK and XAudio2's answer (strings, role, format; first call, from XAudio2)");
    for (int i = 0; i < SIZE; i++) g_ref[i] = buffer[i];
    int bad = 0;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 200; i++) { hr = details(b, 0, buffer, (BYTE)i); bad += hr != 0 || !same(buffer, g_ref); }
    QueryPerformanceCounter(&t1);
    DWORD hit_ticks = (DWORD)(t1 - t0); /* 200 calls, including the poison fill and compare */
    check(!bad, "200 more calls (poisoned buffers): all S_OK and byte-identical to the first answer");
    out("      cached call "); out_dec(hit_ticks * 5 / ticks_per_us); out(" ns average (incl. test overhead) vs "); out_dec(base_ticks / ticks_per_us); out(" us\r\n");
    check(hit_ticks * 10 < base_ticks * 200, "cached calls at least 10x faster than XAudio2's own");
    hr = details(a, 0, buffer, 0xEE);
    check(hr == 0 && same(buffer, g_ref), "engine A (same vtable) is served the same cached bytes");
    hr = details(b, count, buffer, 0);
    check(hr == bad_hr, "out-of-range index through the patch: same result as before");
    if (count > 1) {
        hr = details(b, 1, buffer, 0);
        check(hr == base1_hr && (hr < 0 || same_details(buffer, base1)), "index 1 passes through: same answer as before");
    }

    /* 5. concurrency with a warm cache */
    g_full = 1;
    check(run_threads(b) == 0, "8 threads x 2000 calls at once (warm cache): all S_OK and byte-identical to the first answer");

    /* 6. unload attempt, then a new engine */
    release(a); release(b);
    CoFreeUnusedLibrariesEx(0, 0); Sleep(50); CoFreeUnusedLibrariesEx(0, 0);
    HMODULE xaudio = GetModuleHandleW(L"xaudio2_7.dll");
    check(xaudio != 0, "xaudio2_7.dll still loaded after both engines are released and CoFreeUnusedLibrariesEx (pinned)");
    if (!xaudio) ExitProcess(100 + (DWORD)failures);
    check(IN_FIX(vtable[4]), "its vtable is still patched");
    void *c = 0;
    hr = CoCreateInstance(&CLSID_XAudio2, 0, 1, &IID_IXAudio2, &c);
    check(hr >= 0 && c && VT(c) == vtable, "engine C through the import reuses the patched vtable");
    if (!c) ExitProcess(100 + (DWORD)failures);
    check(initialize(c) >= 0, "engine C Initialize");
    g_full = 0;
    check(run_threads(c) == 0, "8 threads x 2000 calls racing the refresh after a new engine: all S_OK and XAudio2's answer");
    hr = details(c, 0, buffer, 0x5A);
    check(hr == 0 && same_details(buffer, g_base), "engine C GetDeviceDetails(0) afterwards: XAudio2's answer");
    release(c);

    out(failures ? "SOME CHECKS FAILED\r\n" : "ALL CHECKS PASSED\r\n");
    ExitProcess((DWORD)failures);
}
