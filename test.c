/*
 * Test host for X3DAudio1_7.dll (VinceFix): a 32-bit console program that, like
 * Vince.exe, imports kernel32!SetThreadAffinityMask, then loads the built DLL from
 * its own folder and checks:
 *   1. its own SetThreadAffinityMask import became a no-op (the thread stays unpinned),
 *   2. kernel32's real function is untouched for everyone else,
 *   3. X3DAudioInitialize reaches the real system X3DAudio (handle gets filled in),
 *   4. the process became per-monitor DPI aware.
 * Exit code = number of failed checks.
 */
typedef unsigned int DWORD;
typedef int BOOL;
typedef unsigned long ULONG_PTR;
typedef void *HANDLE;
typedef void *HMODULE;
typedef unsigned short WCHAR;
#define WINAPI __stdcall
#define IMPORT __declspec(dllimport)

IMPORT HMODULE WINAPI GetModuleHandleW(const WCHAR *name);
IMPORT void *WINAPI GetProcAddress(HMODULE module, const char *name);
IMPORT HMODULE WINAPI LoadLibraryW(const WCHAR *path);
IMPORT DWORD WINAPI GetModuleFileNameW(HMODULE module, WCHAR *buffer, DWORD size);
IMPORT BOOL WINAPI GetProcessAffinityMask(HANDLE process, ULONG_PTR *processMask, ULONG_PTR *systemMask);
IMPORT HANDLE WINAPI GetCurrentProcess(void);
IMPORT HANDLE WINAPI GetCurrentThread(void);
IMPORT ULONG_PTR WINAPI SetThreadAffinityMask(HANDLE thread, ULONG_PTR mask);
IMPORT HANDLE WINAPI GetStdHandle(DWORD which);
IMPORT BOOL WINAPI WriteFile(HANDLE file, const void *data, DWORD size, DWORD *written, void *overlapped);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT void WINAPI ExitProcess(DWORD code);

int _fltused = 0;
static int failures;

static void out(const char *s) { DWORD n = 0, w; while (s[n]) n++; WriteFile(GetStdHandle((DWORD)-11), s, n, &w, 0); }
static void out_hex(ULONG_PTR v) { char b[12]; int i = 11; b[i] = 0; do { b[--i] = "0123456789ABCDEF"[v & 15]; v >>= 4; } while (v); out("0x"); out(b + i); }
static void check(int ok, const char *what) { out(ok ? "PASS  " : "FAIL  "); out(what); out("\r\n"); if (!ok) failures++; }

void WINAPI start(void) {
    ULONG_PTR process_mask = 0, system_mask = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
    out("process mask "); out_hex(process_mask); out("\r\n");

    WCHAR path[520];
    DWORD n = GetModuleFileNameW(0, path, 500);
    while (n && path[n - 1] != L'\\') n--;
    const WCHAR *name = L"X3DAudio1_7.dll";
    int i = 0;
    do { path[n + i] = name[i]; } while (name[i++]);
    HMODULE fix = LoadLibraryW(path);
    check(fix != 0, "built X3DAudio1_7.dll loads");
    if (!fix) ExitProcess(100);

    /* 1. our import is now the no-op */
    ULONG_PTR returned = SetThreadAffinityMask(GetCurrentThread(), 1);
    check(returned == process_mask, "hooked SetThreadAffinityMask returns the process mask");
    ULONG_PTR (WINAPI *real_set)(HANDLE, ULONG_PTR) =
        (ULONG_PTR (WINAPI *)(HANDLE, ULONG_PTR))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadAffinityMask");
    ULONG_PTR previous = real_set(GetCurrentThread(), process_mask);
    out("      thread mask after the hooked call: "); out_hex(previous); out("\r\n");
    check(previous == process_mask, "thread was not pinned by the hooked call");

    /* 2. kernel32 itself still works for other callers */
    real_set(GetCurrentThread(), 2);
    previous = real_set(GetCurrentThread(), process_mask);
    check(previous == 2, "kernel32 SetThreadAffinityMask itself is untouched");

    /* 3. X3DAudio forwarding: X3DAudioInitialize(SPEAKER_STEREO, speed of sound, handle) */
    void (__cdecl *initialize)(DWORD, float, unsigned char *) = (void (__cdecl *)(DWORD, float, unsigned char *))GetProcAddress(fix, "X3DAudioInitialize");
    void *calculate = GetProcAddress(fix, "X3DAudioCalculate");
    check(initialize != 0 && calculate != 0, "both X3DAudio exports present");
    unsigned char handle[20] = {0};
    if (initialize) initialize(0x3, 343.5f, handle);
    int filled = 0;
    for (int i = 0; i < 20; i++) filled |= handle[i];
    check(filled != 0, "X3DAudioInitialize reached the real X3DAudio (handle filled in)");
    HMODULE first = GetModuleHandleW(L"X3DAudio1_7.dll");
    out("      X3DAudio1_7.dll modules: fix "); out_hex((ULONG_PTR)fix); out(", first by name "); out_hex((ULONG_PTR)first); out("\r\n");

    /* 4. DPI awareness */
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    void *(WINAPI *get_context)(void) = (void *(WINAPI *)(void))GetProcAddress(user32, "GetThreadDpiAwarenessContext");
    int (WINAPI *awareness_of)(void *) = (int (WINAPI *)(void *))GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext");
    int awareness = (get_context && awareness_of) ? awareness_of(get_context()) : -1;
    out("      DPI awareness: "); out_hex((ULONG_PTR)awareness); out(" (2 = per-monitor)\r\n");
    check(awareness == 2, "process is per-monitor DPI aware");

    out(failures ? "SOME CHECKS FAILED\r\n" : "ALL CHECKS PASSED\r\n");
    ExitProcess((DWORD)failures);
}
