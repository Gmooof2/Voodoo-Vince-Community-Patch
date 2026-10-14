/*
 * Startup-path test (adapted from the critic's static-import host): imports
 * X3DAudio1_7.dll at load time like Vince.exe, so VinceFix's DllMain runs under the
 * process-start loader lock with this exe's imports already resolved. Checks:
 *   1. DPI awareness is per-monitor before any of this program's code runs,
 *   2. this exe's SetThreadAffinityMask import is a no-op (the thread stays unpinned),
 *   3. X3DAudioInitialize and X3DAudioCalculate both reach the real X3DAudio.
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

IMPORT void __cdecl X3DAudioInitialize(DWORD mask, float speed, unsigned char *handle);
IMPORT void __cdecl X3DAudioCalculate(const unsigned char *handle, const void *listener, const void *emitter, DWORD flags, void *dsp);
IMPORT HMODULE WINAPI GetModuleHandleW(const WCHAR *name);
IMPORT void *WINAPI GetProcAddress(HMODULE module, const char *name);
IMPORT BOOL WINAPI GetProcessAffinityMask(HANDLE process, ULONG_PTR *processMask, ULONG_PTR *systemMask);
IMPORT HANDLE WINAPI GetCurrentProcess(void);
IMPORT HANDLE WINAPI GetCurrentThread(void);
IMPORT ULONG_PTR WINAPI SetThreadAffinityMask(HANDLE thread, ULONG_PTR mask);
IMPORT HANDLE WINAPI GetStdHandle(DWORD which);
IMPORT BOOL WINAPI WriteFile(HANDLE file, const void *data, DWORD size, DWORD *written, void *overlapped);
IMPORT void WINAPI ExitProcess(DWORD code);

int _fltused = 0;
static int failures;
static void out(const char *s) { DWORD n = 0, w; while (s[n]) n++; WriteFile(GetStdHandle((DWORD)-11), s, n, &w, 0); }
static void out_hex(ULONG_PTR v) { char b[12]; int i = 11; b[i] = 0; do { b[--i] = "0123456789ABCDEF"[v & 15]; v >>= 4; } while (v); out("0x"); out(b + i); }
static void check(int ok, const char *what) { out(ok ? "PASS  " : "FAIL  "); out(what); out("\r\n"); if (!ok) failures++; }

#pragma pack(push, 1)
typedef struct { float x, y, z; } V3;
typedef struct { V3 front, top, pos, vel; void *cone; } Listener;
typedef struct { void *cone; V3 front, top, pos, vel; float inner, innerAngle; DWORD channels; float channelRadius;
  float *azimuths; void *vol, *lfe, *lpfd, *lpfr, *rev; float curveScaler, dopplerScaler; } Emitter;
typedef struct { float *matrix; float *delays; DWORD src, dst; float lpfd, lpfr, reverb, doppler, angle, distance, ev, lv; } Dsp;
#pragma pack(pop)

void WINAPI start(void) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    void *(WINAPI *get_context)(void) = (void *(WINAPI *)(void))GetProcAddress(user32, "GetThreadDpiAwarenessContext");
    int (WINAPI *awareness_of)(void *) = (int (WINAPI *)(void *))GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext");
    check(get_context && awareness_of && awareness_of(get_context()) == 2, "per-monitor DPI aware at program entry (set during process start)");

    ULONG_PTR process_mask = 0, system_mask = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
    SetThreadAffinityMask(GetCurrentThread(), 1); /* through this exe's import slot */
    ULONG_PTR (WINAPI *real_set)(HANDLE, ULONG_PTR) =
        (ULONG_PTR (WINAPI *)(HANDLE, ULONG_PTR))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadAffinityMask");
    ULONG_PTR previous = real_set(GetCurrentThread(), process_mask);
    out("      thread mask after the hooked call: "); out_hex(previous); out("\r\n");
    check(previous == process_mask, "static import of SetThreadAffinityMask is a no-op");

    unsigned char handle[20] = {0};
    X3DAudioInitialize(0x3, 343.5f, handle);
    int filled = 0;
    for (int i = 0; i < 20; i++) filled |= handle[i];
    check(filled != 0, "X3DAudioInitialize reached the real X3DAudio");

    float matrix[8] = {0}; float azimuth[1] = {0};
    Listener listener = {{0,0,1},{0,1,0},{0,0,0},{0,0,0},0};
    Emitter emitter = {0,{0,0,1},{0,1,0},{3,0,0},{0,0,0},0,0,1,0,azimuth,0,0,0,0,0,1.0f,1.0f};
    Dsp dsp = {matrix, 0, 1, 2, 0,0,0, 0.0f, 0,0,0,0};
    X3DAudioCalculate(handle, &listener, &emitter, 0x1 | 0x20 | 0x40 /* MATRIX | DOPPLER | EMITTER_ANGLE */, &dsp);
    out("      Calculate: distance bits "); out_hex(*(DWORD *)&dsp.distance); out(", doppler bits "); out_hex(*(DWORD *)&dsp.doppler); out("\r\n");
    check(dsp.distance > 2.99f && dsp.distance < 3.01f && dsp.doppler > 0.5f && (matrix[0] != 0 || matrix[1] != 0),
          "X3DAudioCalculate reached the real X3DAudio (distance 3, doppler set, matrix filled)");

    out(failures ? "SOME CHECKS FAILED\r\n" : "ALL CHECKS PASSED\r\n");
    ExitProcess((DWORD)failures);
}
