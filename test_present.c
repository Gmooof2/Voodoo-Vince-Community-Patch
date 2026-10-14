/*
 * Present-hold test host for VinceFix: imports X3DAudio1_7.dll at load time like Vince.exe, so
 * VinceFix's DllMain scans THIS exe's code for the game's Present call site and patches it
 * (build.ps1 writes VinceFix.ini with PresentHold=5 first). game_present() below reproduces that
 * call site byte for byte (the 20-byte pattern occurs once in this exe); the fake swap chain's
 * Present spins to a fixed 60 FPS grid like the NVIDIA limiter and returns 0x087A0001.
 * 660 frames of "work" (1.0 .. 7.0 ms) then Present. Checks:
 *   P. the 14-byte call-site prefix occurs exactly once in this exe's executable sections (host sanity),
 *   a. bytes at pattern+14 are now 50 E8 <rel32>, and the call target lies inside X3DAudio1_7.dll,
 *   b. every fake Present call got (fake swap chain, sync 1, flags 0) and 0x087A0001 came back,
 *   c. work <= 4.7 ms: (Present entry - frame start - 5 ms) >= -0.05 ms for all, <= 0.2 ms for
 *      >= 95 %, <= 2 ms for 99 %, max <= 10 ms (a busy PC's timer can wake late; frame 600, which follows VinceFix's stats-line write, is not counted),
 *   d. work >= 5.3 ms (7 ms): Present entry - (frame start + work) <= 0.3 ms for >= 95 % (no wait),
 *   e. entry-to-entry interval of consecutive held frames: >= 98 % within 0.1 ms of 16.667 ms,
 *   f. the first frame is not held (entry - (start + work) <= 2 ms; a held one would be 4 ms late).
 * build.ps1 also checks VinceFix.log (one patch line, stats lines). Exit code = number of failed checks.
 *
 * Auto scenario: when the VinceFix.ini next to this exe says PresentHold=auto (case-insensitive) the host runs 3600
 * frames instead (P and the patch check as above). Phases of 900 frames: A limiter on, work 1.0-3.0 ms, 4 % of the
 * frames 3.4 ms, plus ONE burst of 30 frames of 40 ms work at frames 450-479 (a loading stretch) and four 6 ms frames at
 * 605, 620, 635, 650 (one block, too few to raise the target, as a few frames Windows paused would be); B limiter on,
 * work 3.0-5.0 ms, 4 % 5.6 ms; C limiter OFF (the fake Present returns at once), work as in A; D limiter on again
 * (grid re-anchored at its first call), work as in A. "Held" frame = Present entry more than 0.1 ms after the end
 * of its work; its measured hold = Present entry - frame start (the previous Present's return). Checks:
 *   auto a. frames 800-899: measured hold 3.7 ms +- 0.3 for >= 95 % of the frames with work <= 3.0 ms
 *           (3.4 ms -> bucket 3.4-3.5 -> 3.5 + 0.2 = 3.7),
 *   auto b. no held frame in A after the first target (frames 301-899) holds more than 4.2 ms (the burst and the four 6 ms
 *           frames must not raise it),
 *   auto b2. fast raise: frames 1050-1149 (B, before its first window ends) already at 5.8 ms +- 0.3,
 *   auto c. frames 1700-1799: 5.8 ms +- 0.3 for >= 95 % of the frames with work <= 5.0 ms (5.6 ms -> bucket 5.6-5.7 -> 5.9),
 *   auto d. frames 2600-2699 (C): no hold, entry - (start + work) <= 0.3 ms for >= 95 %,
 *   auto e. frames 3500-3599: 3.7 ms +- 0.3 as in a,
 *   auto f. consecutive held frames in A, B, D (not within 5 frames of a phase start or the burst, nor a frame whose start
 *           followed a VinceFix.log write, which delays this host's start): entry-to-entry interval
 *           16.667 ms +- 0.1 ms for all but 2 % of the pairs, max deviation < 5 ms (a busy PC's timer wakes late now and then),
 *   auto g. never early: between consecutive held frames the measured hold never drops by more than 0.07 ms
 *           (a drop right after an upward spike is a late frame recovering, not an early one, if it ends no lower than the
 *           hold before the spike).
 *
 * Cap scenarios: when the ini has FrameCap=60 the fake Present has NO limiter (returns at once), so only VinceFix's cap
 * paces the frames; the fake swap chain's vtable has 16 slots (GetDesc at 12 returns a zeroed DXGI_SWAP_CHAIN_DESC, so
 * the display check finds the nearest monitor; any other slot called fails the run). The frame start is this host's
 * timestamp right after game_present returns. Selected by PresentHold: a number -> cap+hold (660 frames, work as in A),
 * 0 or none -> cap (660 frames, work as in A plus five 25 ms frames at 100, 200 .. 500), auto -> cap+auto (2400 frames:
 * work as in A, then as in B from frame 600, then as in A from 1200).
 *   cap+hold start / hold / entry: frame-start intervals mean 16.667 ms +- 0.01 and >= 98 % within 0.1 ms of it; Present
 *           entry at start + 4 ms +- 0.2 for >= 95 % of the frames with work <= 3.5 ms; >= 98 % of present-to-present
 *           intervals within 0.1 ms of the grid; carry: the earliest frame starts at the beginning and at the end of the run,
 *           measured against the exact grid (n * QPC frequency / 60), agree within 5 us (without the carry: about 40 us apart),
 *   cap start / present / late: the same grid; no hold (entry <= 0.3 ms after the work for >= 95 %); after each 25 ms
 *           frame the next start follows at once (interval = the work, +0 .. 0.3 ms) and the interval after that is
 *           16.667 ms +- 0.1 again (no short catch-up frame),
 *   cap+auto a, c, d, e, start, f, g: as the auto ones (3.7 / 5.8 ms), then slow lowering: frames 1700-1799 still 5.8 ms
 *           (only 2 lighter windows), frames 2300-2399 back at 3.7 ms (3 lighter windows agreed); all windows count as capped.
 * FrameCap stepping aside ([VinceFixTest] Limiter=60 with FrameCap=60 and PresentHold=4, HoldFlush=auto): 1200 frames,
 * work as in A, the fake 60 FPS limiter in Present for frames 0-599 and none after. aside / back: the last frame flushed
 * before the limiter's end is at most frame 120 (FrameCap stepped aside, so HoldFlush=auto stopped) and the first flushed
 * after it at most frame 721 (FrameCap back); one Flush per held frame while FrameCap runs, none while aside. 1 hold:
 * frames 6-594 present 4 ms +- 0.2 after the previous Present's return (never early), across the switch; 1 start / 1 entry:
 * the limiter's spacing; aside spacing: the presents around the switch stay on it. back spacing: the first frame back
 * holds 4 ms from the last aside return, the next starts one period after that return, the 30 after on the grid; 2 start /
 * 2 hold / 2 entry: the cap+hold checks from frame 725. build.ps1 checks one step-aside and one back-on line.
 * GPU-bound ([VinceFixTest] Limiter=gpu with FrameCap=60, PresentHold=0): frames 0-599 a Present that allows 58 FPS
 * (FrameCap waits nothing, Present blocks: like a GPU just too slow for the cap), frames 600-1199 a Present that takes
 * 1.5 ms (blocks, but FrameCap waits most of each frame). FrameCap must never step aside (build.ps1: no such line); 1 start:
 * the 58 FPS spacing, 2 start / 2 present: the 60 FPS grid, no hold; 2 steady: the slide its frames start (they wait both
 * in Present and after it) stops after one window, since Present's time doesn't follow it.
 * Frames whose start followed a VinceFix.log write (the host checks the log's size after every frame; window ends that
 * log a target, the 600-frame stats line) and the first frames are left out of the timing checks, because the write delays
 * this host's own frame start (not the DLL's grid). Window ends without a line are timed like other frames.
 *
 * HoldFlush: the fake swap chain's slot 7 is a fake GetDevice in every run (it checks for IID_ID3D11Device and hands out a
 * fake device, AddRef counted); the fake device has 43 slots (AddRef/Release counted, 40 = GetImmediateContext handing out a
 * fake context, AddRef counted), the fake context 115 (AddRef/Release counted, 111 = Flush); every other slot of both fails
 * the run. Runs without HoldFlush=1 in the ini check that none of them is ever called. With HoldFlush=1 (fixed 5 ms, or
 * FrameCap=60 + PresentHold=4) the fake Flush spins 0.2 ms, 3.5 ms in frames 50, 150 .. 650 (their work + Flush passes the
 * hold), and the cap+hold run gets 6 ms frames at 75, 175 .. 675 (past the hold, so not flushed). Those frames (and the one
 * after a long Flush) are left out of the timing checks above. Checks:
 *   flush count: never two Flushes in a frame, none in a frame that arrived after the hold (or the first frame), one in
 *           >= 99 % of the frames whose work fits the hold well (Windows pausing the host can make a frame late),
 *   flush order: every Flush ends before that frame's Present, and starts <= 0.1 ms after the frame's work ended (before
 *           the hold's wait, not after it) for >= 95 % of the flushed frames,
 *   flush late: a frame whose Flush passed the hold point enters Present <= 0.3 ms after the Flush (no further wait); one
 *           whose Flush ended before the hold (an auto hold above work + 3.5 ms) enters it at the hold of the frame before,
 *   flush refs: GetDevice (right IID), GetImmediateContext and Flush called equally often; every reference released
 *           (counts never below 0 or above 1, back to 0 at the end); device and context both held during each Flush.
 * The host prints how many Flushes frames 0-599 saw; build.ps1 compares that with the stats line's held and flushed counts.
 * [VinceFixTest] GetDevice=fail in the ini makes the fake GetDevice fail (E_NOINTERFACE): then exactly one GetDevice call
 * and nothing else (VinceFix stops trying), while the fixed hold checks still pass. GetContext=null makes the fake device
 * hand out no immediate context: one GetDevice and one GetImmediateContext, the device released, nothing more. In both,
 * VinceFix.log must not grow between frame 1's start and its Present (the failure line comes after that Present).
 * [VinceFixTest] FlushUs=<us> sets the fake Flush's time (default 200). PresentHold=auto with HoldFlush=1 runs phases A and
 * B only (1800 frames) and expects every auto target higher by the Flush time (a flushed frame's work includes its Flush);
 * the "held" test for frames then means "waited after the end of its Flush".
 *
 * Which scenario runs, and whether held frames are flushed, follows the DLL's rules (the host has its own copy of them):
 * FrameCap is off on a display no faster than the cap + 1 Hz, PresentHold at 61 Hz or less, and HoldFlush=auto (also no
 * HoldFlush line) flushes only while FrameCap runs. [VinceFixTest] RefreshHz=<n> points the DLL's user32!EnumDisplaySettingsW
 * import at a fake reporting n Hz (0 = unknown; checked to be called for the current mode of a named display); without it
 * the real display is used and assumed faster than 61 Hz. When the rules turn everything off, the "off" scenario runs (300
 * frames, fake limiter on): every frame must reach Present <= 0.3 ms after its work. Every run checks that GetDesc was called
 * at the first frame (the display check) and then only in frames 60, 120 .., once each (VinceFix's cheap check for a changed
 * display): never more per frame.
 *
 * Display changes ([VinceFixTest] Recheck=1 with FrameCap=60, PresentHold=4, RefreshHz=280): 1200 frames; the DLL's
 * MonitorFromWindow and GetMonitorInfoW imports also point at fakes (two monitors, \\.\DISPLAY1 and 2). Frame 300: the game
 * presents through a second fake swap chain, the display runs at 60 Hz (fake limiter on, as VSync); frame 630: its window is
 * on monitor 2 at 280 Hz (limiter off); frame 930: the same address describes a new 2560x1440 swap chain on another render
 * adapter. Checks: GetDesc and MonitorFromWindow at most twice per frame and about once per 60 frames in all, the new pointer
 * found at once (one GetDesc: the report), the move and the reused address within 60 frames; no Flush at 60 Hz, one per held
 * frame after; the cap's grid and the 4 ms hold in each 280 Hz phase, no hold at 60 Hz. build.ps1 checks the lines in order.
 * A limiter joining at another phase ([VinceFixTest] Limiter=join, FrameCap=60, PresentHold=4): from frame 300 the fake
 * limiter, its grid 6 ms after that frame's Present entry (both it and FrameCap wait), catching up after late frames (a late
 * call returns at once, the next waits for the following grid point); 25 / 30 / 25 ms frames at 780, 900 (both end one of
 * VinceFix's 60-frame windows, so the catch-up shortens the next) and 1050, then 33 ms at 1080 and 1200 (window ends:
 * slow / faster / slow / faster windows, which must not count as two faster ones in a row). Checks: FrameCap steps aside within 360 frames
 * of the join and never comes back (no Flush after it; build.ps1: one step-aside line, no back-on line), presents keep the
 * 60 FPS spacing through the switch (>= 98 % within 0.1 ms, none off by 1 ms), the 4 ms hold from the limiter's release
 * after it, and that the fake limiter really caught up.
 * A GPU just faster than the cap ([VinceFixTest] Limiter=edge, FrameCap=60, PresentHold=0): 900 frames of 2 ms work and a
 * 14.26 ms Present (61.5 FPS uncapped). FrameCap may step aside once, but must be on for good by frame 300 (at most 3
 * windows of uncapped frames) and keep the 60 FPS grid after; build.ps1: at most one step-aside line and one "ran faster"
 * back-on line. The recheck run also checks that each re-check's lines come after that frame's real Present.
 *
 * Setup report (first frame: gpu:, display:, power:, os: lines): the fake device answers QueryInterface(IID_IDXGIDevice) with
 * a fake IDXGIDevice whose GetAdapter gives a fake adapter ("VinceFix Test GPU", 0x10DE/0x2684, 1536 MB, driver 31.0.15.5222);
 * the swap chain's GetContainingOutput gives a fake output whose GetParent is that adapter ([VinceFixTest] Hybrid=1: a second,
 * Intel one, and the render adapter reports AMD; Output=fail: DXGI_ERROR_NOT_FOUND). Checks: every reference released, no
 * other slot or IID used, and VinceFix.log grows during frame 0 before its Present and not after it. build.ps1 checks the
 * four lines' text (exactly once each).
 *
 * Live reload (LiveReload=1 in the ini, with PresentHold=0 and FrameCap=0): the host rewrites VinceFix.ini itself while it runs,
 * at frame 240 to PresentHold=5 (fake limiter on), then to PresentHold=4 + FrameCap=60 (no HoldFlush line: auto, so flushed;
 * fake limiter off), then to 0 / 0 (limiter on), then saves that again unchanged, each time 245 frames after it saw
 * VinceFix's "settings reloaded:" line for the previous rewrite (it reads VinceFix.log at each frame's start while it waits;
 * that and the rewrite happen inside the frame's work time). Every key VinceFix reads meanwhile races a hostile editor (the
 * DLL's GetPrivateProfileStringW import first tries to take the file with no sharing; see contended_read). Checks: each
 * rewrite logged within 120 frames, exactly one such line per rewrite; the editor never got the file; then over 240 frames
 * from 5 frames after each pickup the timing of that phase: no hold before the first rewrite and after the last two, hold
 * 5 ms +- 0.2, never early and the limiter's spacing after the first, the 60 FPS grid, hold 4 ms and the present spacing
 * after the second; no Flush outside the FrameCap phase and one per held frame in it, every reference released. build.ps1
 * checks the four lines' exact text, and that the unchanged save logged only its summary line.
 */
typedef unsigned int DWORD;
typedef int BOOL;
typedef unsigned long ULONG_PTR;
typedef void *HANDLE;
typedef void *HMODULE;
typedef unsigned short WCHAR;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef long HRESULT;
#define WINAPI __stdcall
#define IMPORT __declspec(dllimport)

IMPORT void __cdecl X3DAudioInitialize(DWORD mask, float speed, unsigned char *handle);
IMPORT HMODULE WINAPI GetModuleHandleW(const WCHAR *name);
IMPORT HANDLE WINAPI GetStdHandle(DWORD which);
IMPORT BOOL WINAPI WriteFile(HANDLE file, const void *data, DWORD size, DWORD *written, void *overlapped);
IMPORT __attribute__((noreturn)) void WINAPI ExitProcess(DWORD code);
IMPORT BOOL WINAPI QueryPerformanceCounter(long long *count);
IMPORT BOOL WINAPI QueryPerformanceFrequency(long long *frequency);
IMPORT DWORD WINAPI GetModuleFileNameW(HMODULE module, WCHAR *name, DWORD size);
IMPORT DWORD WINAPI GetPrivateProfileStringW(const WCHAR *section, const WCHAR *key, const WCHAR *def, WCHAR *out, DWORD size, const WCHAR *file);
IMPORT BOOL WINAPI GetFileAttributesExW(const WCHAR *path, DWORD level, void *data);
IMPORT BOOL WINAPI VirtualProtect(void *address, ULONG_PTR size, DWORD protect, DWORD *old);
IMPORT HANDLE WINAPI CreateFileW(const WCHAR *path, DWORD access, DWORD share, void *security, DWORD disposition, DWORD flags, HANDLE templ);
IMPORT BOOL WINAPI ReadFile(HANDLE file, void *data, DWORD size, DWORD *read, void *overlapped);
IMPORT BOOL WINAPI CloseHandle(HANDLE handle);

int _fltused = 0;
static int failures;

typedef struct { DWORD low; long high; } Luid;
typedef struct {            /* DXGI_ADAPTER_DESC, 32-bit */
    WCHAR description[128];
    DWORD vendor_id, device_id, subsys_id, revision;
    ULONG_PTR dedicated_video_memory, dedicated_system_memory, shared_system_memory;
    Luid adapter_luid;
} DxgiAdapterDesc;
_Static_assert(sizeof(DxgiAdapterDesc) == 292 && __builtin_offsetof(DxgiAdapterDesc, vendor_id) == 256 && __builtin_offsetof(DxgiAdapterDesc, adapter_luid) == 284, "DXGI_ADAPTER_DESC layout");
typedef struct {            /* DXGI_SWAP_CHAIN_DESC */
    DWORD width, height, refresh_numerator, refresh_denominator, format, scanline_ordering, scaling, sample_count, sample_quality;
    DWORD buffer_usage, buffer_count;
    HANDLE output_window;
    BOOL windowed;
    DWORD swap_effect, flags;
} DxgiSwapChainDesc;
_Static_assert(sizeof(DxgiSwapChainDesc) == 60 && __builtin_offsetof(DxgiSwapChainDesc, output_window) == 44, "DXGI_SWAP_CHAIN_DESC layout");
static void out(const char *s) { DWORD n = 0, w; while (s[n]) n++; WriteFile(GetStdHandle((DWORD)-11), s, n, &w, 0); }
static void out_hex(ULONG_PTR v) { char b[12]; int i = 11; b[i] = 0; do { b[--i] = "0123456789ABCDEF"[v & 15]; v >>= 4; } while (v); out("0x"); out(b + i); }
static void out_dec(DWORD v) { char b[12]; int i = 11; b[i] = 0; do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v); out(b + i); }
static void out_us(double v) { if (v < 0) { out("-"); v = -v; } DWORD t = (DWORD)(v * 10.0 + 0.5); out_dec(t / 10); out("."); out_dec(t % 10); out(" us"); }
static void out_num2(double us) { if (us < 0) { out("-"); us = -us; } DWORD t = (DWORD)(us / 10.0 + 0.5); out_dec(t / 100); out("."); if (t % 100 < 10) out("0"); out_dec(t % 100); } /* microseconds as ms, 2 decimals */
static void out_ms(double us) { out_num2(us); out(" ms"); }
static void out_bytes(const BYTE *p, int n) { char b[4]; b[2] = ' '; b[3] = 0; for (int i = 0; i < n; i++) { b[0] = "0123456789ABCDEF"[p[i] >> 4]; b[1] = "0123456789ABCDEF"[p[i] & 15]; out(b); } }
static void check(int ok, const char *what) { out(ok ? "PASS  " : "FAIL  "); out(what); out("\r\n"); if (!ok) failures++; }
static double dsqrt(double x) { __asm__("fsqrt" : "+t"(x)); return x; }

#define NF 660
#define PRESENT_HRESULT ((HRESULT)0x087A0001)
static const DWORD work10[8] = {10, 20, 30, 40, 46, 15, 25, 70}; /* work per frame, tenths of a ms */
#define HELD(i) ((i) >= 1 && work10[(i) & 7] <= 47)
/* VinceFix writes its 600-frame stats line (file I/O, ~0.4 ms) inside the call that ends frame 599, after
   noting when Present returned (by design: the hold counts from the limiter's release, so the write uses
   frame 600's slack and its present stays on the grid, which e checks). This host's own start for frame 600
   is late by the write, so c, which measures from that start, skips it. The same goes for frame 2 in the HoldFlush
   failure runs: the failure line is written after frame 1's Present. */
#define STATS_FRAME 600
#define FAILURE_LOG_FRAME 2
static int watch_log; /* HoldFlush failure runs (see log_size) */
static int after_log_write(int i) { return i == STATS_FRAME || (watch_log && i == FAILURE_LOG_FRAME); }

/* The game's Present call site, exact bytes; the label marks the pattern start. Takes the renderer
   pointer (esi in the game) and returns Present's HRESULT. Keep these bytes out of all other code. */
extern BYTE site_pattern[];
__attribute__((naked)) static HRESULT WINAPI game_present(void *renderer __attribute__((unused))) {
    __asm__ volatile(
        "pushl %esi\n\t"
        "movl 8(%esp), %esi\n\t"
        ".globl _site_pattern\n"
        "_site_pattern:\n\t"
        ".byte 0x8B,0x86,0xAC,0x00,0x00,0x00\n\t" /* mov eax,[esi+0xAC]   swap chain */
        ".byte 0x6A,0x00\n\t"                     /* push 0               Flags */
        ".byte 0xFF,0xB6,0x9C,0x00,0x00,0x00\n\t" /* push [esi+0x9C]      SyncInterval */
        ".byte 0x8B,0x08\n\t"                     /* mov ecx,[eax] */
        ".byte 0x50\n\t"                          /* push eax */
        ".byte 0xFF,0x51,0x20\n\t"                /* call [ecx+0x20]      Present, vtable slot 8 */
        "popl %esi\n\t"
        "ret $4\n\t");
}

static const BYTE prefix[14] = {0x8B, 0x86, 0xAC, 0, 0, 0, 0x6A, 0, 0xFF, 0xB6, 0x9C, 0, 0, 0}; /* in .rdata, never scanned */

static long long freq, period, deadline;
static double us_per_tick;
#define NMAX 3600 /* frames of the longest scenario (the auto one) */
static long long start_t[NMAX], entry_t[NMAX], work_t[NMAX], return_t[NMAX]; /* return_t: when the fake Present returned */
static DWORD calls, bad_args, bad_ret, bad_slot_calls, desc_calls;
/* The game's renderer object as the call site reads it: the swap chain at +0xAC, SyncInterval at +0x9C. */
typedef struct { BYTE before_sync[0x9C]; DWORD sync_interval; BYTE before_swap_chain[0xAC - 0xA0]; void *swap_chain; BYTE rest[0x100 - 0xB0]; } Renderer;
_Static_assert(__builtin_offsetof(Renderer, sync_interval) == 0x9C && __builtin_offsetof(Renderer, swap_chain) == 0xAC && sizeof(Renderer) == 0x100, "renderer layout");
static Renderer renderer __attribute__((aligned(16)));
/* The fake swap chain, and a second one the recheck scenario switches the game to (same vtable). Every fake checks that
   VinceFix calls the one the game presents through (renderer.swap_chain). */
static struct { void **vtbl; } swapchain, swapchain2;
/* COM slots the fakes answer: the test's own count from dxgi.h / d3d11.h (VinceFix's is in vincefix.h), so a miscount in
   either shows up as a call of a slot that fails the run. */
#define SLOT_QUERY_INTERFACE 0         /* IUnknown */
#define SLOT_ADD_REF 1
#define SLOT_RELEASE 2
#define SLOT_GET_PARENT 6              /* IDXGIObject */
#define SLOT_GET_DEVICE 7              /* IDXGIDeviceSubObject */
#define SLOT_PRESENT 8                 /* IDXGISwapChain ... */
#define SLOT_SWAP_CHAIN_GET_DESC 12
#define SLOT_GET_CONTAINING_OUTPUT 15
#define SWAP_CHAIN_SLOTS 18            /* ... GetFrameStatistics 16, GetLastPresentCount 17 */
#define SLOT_GET_ADAPTER 7             /* IDXGIDevice */
#define DXGI_DEVICE_SLOTS 12
#define SLOT_ADAPTER_GET_DESC 8        /* IDXGIAdapter */
#define SLOT_CHECK_INTERFACE_SUPPORT 9
#define ADAPTER_SLOTS 10
#define OUTPUT_SLOTS 19                /* IDXGIOutput: GetDesc 7 ... GetFrameStatistics 18 */
#define SLOT_GET_IMMEDIATE_CONTEXT 40  /* ID3D11Device */
#define DEVICE_SLOTS 43
#define SLOT_FLUSH 111                 /* ID3D11DeviceContext */
#define CONTEXT_SLOTS 115
static void *vtable[SWAP_CHAIN_SLOTS];
static int limiter_on = 1, reanchor = 1; /* limiter off: Present returns at once; reanchor: the next limited call starts the grid */
static long long limit_period; /* the fake limiter's period: `period` (60 FPS) unless a scenario sets another */
static long long present_busy; /* with the limiter off: how long the fake Present spins (a driver working in Present), 0 = not */
static long long join_offset;  /* the first limited call starts the limiter's grid this long after its entry (and waits for it) */
static int limiter_catches_up; /* after a late call the limiter keeps its grid: the late call returns at once and the next
                                  one waits for the following grid point (a shorter interval), as a driver cap may; otherwise
                                  it skips to the next grid point after the entry */

/* HoldFlush failure runs: VinceFix.log's size at frame 1's start (the first held frame), at its Present entry and after
   its Present. The failure line must not be written inside the held frame (it would delay that present). */
typedef struct { DWORD attributes, created[2], accessed[2], written[2], size_high, size_low; } FileAttributeData; /* WIN32_FILE_ATTRIBUTE_DATA */
_Static_assert(sizeof(FileAttributeData) == 36, "WIN32_FILE_ATTRIBUTE_DATA layout");
static WCHAR log_path[520];
static DWORD log_at_start, log_at_present, log_after;
static DWORD log_size(void) { FileAttributeData data; return GetFileAttributesExW(log_path, 0 /* GetFileExInfoStandard */, &data) ? data.size_low : 0; }
/* Every run: the same around frame 0. The setup report and the display rules are written inside it before its Present
   (nothing is held or timed then), and nothing after that Present. */
static DWORD log_before_first, log_at_first_present, log_after_first;
static int watch_entry_log;         /* the recheck scenario: VinceFix.log's size at every Present entry */
static DWORD log_at_entry[NMAX];
static int frame_now;               /* the frame this host is in (fakes count their calls per frame by it) */
static void check_first_frame_log(void) {
    out("      VinceFix.log size before frame 0's game_present "); out_dec(log_before_first); out(", at its Present entry "); out_dec(log_at_first_present);
    out(", after it "); out_dec(log_after_first); out(" bytes\r\n");
    check(log_at_first_present > log_before_first && log_after_first == log_at_first_present,
          "first frame: the setup report is written before the first real Present, nothing after it");
}

/* NVIDIA-style 60 FPS limiter: the first call returns at once; later calls return on the next
   fixed-cadence grid point after their entry time (skipping points already past, without waiting).
   With the limiter switched off (auto scenario phase C, cap scenarios) it returns at once and the
   grid restarts at the first limited call. */
static HRESULT WINAPI fake_present(void *self, DWORD sync, DWORD flags) {
    long long now;
    QueryPerformanceCounter(&now);
    if (self != renderer.swap_chain || sync != 1 || flags != 0) bad_args++;
    if (calls < NMAX) entry_t[calls] = now;
    if (watch_log && calls == 1) log_at_present = log_size();
    if (calls == 0) log_at_first_present = log_size();
    if (watch_entry_log && calls < NMAX) log_at_entry[calls] = log_size(); /* the recheck scenario */
    if (!limiter_on) {
        reanchor = 1;
        long long end = now + present_busy;
        while (now < end) QueryPerformanceCounter(&now);
    }
    else if (reanchor) {
        deadline = now + join_offset;
        reanchor = 0;
        while (now < deadline) QueryPerformanceCounter(&now);
    }
    else if (limiter_catches_up) {
        deadline += limit_period;
        if (now - deadline > limit_period) deadline = now; /* far behind (a load): restart the grid */
        while (now < deadline) QueryPerformanceCounter(&now);
    }
    else {
        deadline += limit_period;
        while (deadline <= now) deadline += limit_period;
        do QueryPerformanceCounter(&now); while (now < deadline);
    }
    if (calls < NMAX) return_t[calls] = now;
    calls++;
    return PRESENT_HRESULT;
}

/* IDXGISwapChain::GetDesc (slot 12), which VinceFix calls at the first frame for the display check and the display: line,
   and then once per DISPLAY_CHECK_FRAMES frames to see whether the display changed: a 1920x1080 (the recheck scenario
   changes it) windowed blt-model (discard) swap chain with 2 buffers and flags 0x2. OutputWindow is 0, so MonitorFromWindow
   picks the nearest monitor. desc_n counts the calls per frame. */
#define DISPLAY_CHECK_FRAMES 60 /* VinceFix's */
static DWORD desc_width = 1920, desc_height = 1080;
static BYTE desc_n[NMAX];
static HRESULT WINAPI fake_get_desc(void *self, DxgiSwapChainDesc *desc) {
    desc_calls++;
    if (frame_now < NMAX && desc_n[frame_now] < 255) desc_n[frame_now]++;
    if (self != renderer.swap_chain) bad_args++;
    BYTE *bytes = (BYTE *)desc;
    for (unsigned i = 0; i < sizeof *desc; i++) bytes[i] = 0;
    desc->width = desc_width;
    desc->height = desc_height;
    desc->buffer_count = 2;
    desc->windowed = 1;
    desc->swap_effect = 0; /* DXGI_SWAP_EFFECT_DISCARD */
    desc->flags = 2;       /* DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH */
    return 0;
}

/* Every other vtable slot: VinceFix must not call it. */
static HRESULT WINAPI bad_slot(void) { bad_slot_calls++; bad_args++; return (HRESULT)0x80004005; }

/* ---- HoldFlush fakes: swap chain slot 7 GetDevice -> device (slot 40 GetImmediateContext) -> context (slot 111 Flush) ---- */
#define LONG_FLUSH_US 3500
static DWORD flush_us = 200; /* the fake Flush's spin; [VinceFixTest] FlushUs=<us> */
static const BYTE iid_d3d11_device[16] = {0xDB, 0x6D, 0x6F, 0xDB, 0x77, 0xAC, 0x88, 0x4E, 0x82, 0x53, 0x81, 0x9D, 0xF9, 0xBB, 0xF1, 0x40}; /* {db6f6ddb-ac77-4e88-8253-819df9bbf140} */
static struct { void **vtbl; } fake_device, fake_context;
static void *device_vtable[DEVICE_SLOTS], *context_vtable[CONTEXT_SLOTS];
static int flush_run, getdevice_fails, getcontext_null;
static DWORD getdevice_calls, getcontext_calls, flush_calls, bad_iid, device_bad_slots, context_bad_slots, bad_order;
static int device_refs, context_refs, refs_min, refs_max;
static BYTE flush_n[NMAX];
static long long flush_t[NMAX], flush_end_t[NMAX];
static int long_flush(int i) { return flush_run && i > 0 && i % 100 == 50; } /* frames whose fake Flush takes LONG_FLUSH_US */

static void ref_change(int *refs, int by) { *refs += by; if (*refs < refs_min) refs_min = *refs; if (*refs > refs_max) refs_max = *refs; }
static HRESULT WINAPI fake_get_device(void *self, const BYTE *iid, void **device) {
    getdevice_calls++;
    if (self != renderer.swap_chain || !device) { bad_args++; return (HRESULT)0x80004003; /* E_POINTER */ }
    int ok = iid != 0;
    for (int k = 0; ok && k < 16; k++) ok = iid[k] == iid_d3d11_device[k];
    if (!ok) bad_iid++;
    if (!ok || getdevice_fails) { *device = 0; return (HRESULT)0x80004002; /* E_NOINTERFACE */ }
    *device = &fake_device;
    ref_change(&device_refs, 1);
    return 0;
}
static DWORD WINAPI device_add_ref(void *self) { if (self != (void *)&fake_device) bad_args++; ref_change(&device_refs, 1); return (DWORD)device_refs; }
static DWORD WINAPI device_release(void *self) { if (self != (void *)&fake_device) bad_args++; ref_change(&device_refs, -1); return (DWORD)device_refs; }
static void WINAPI fake_get_context(void *self, void **context) {
    getcontext_calls++;
    if (self != (void *)&fake_device || !context) { bad_args++; return; }
    if (getcontext_null) { *context = 0; return; } /* [VinceFixTest] GetContext=null: a device without an immediate context */
    *context = &fake_context;
    ref_change(&context_refs, 1);
}
static DWORD WINAPI context_add_ref(void *self) { if (self != (void *)&fake_context) bad_args++; ref_change(&context_refs, 1); return (DWORD)context_refs; }
static DWORD WINAPI context_release(void *self) { if (self != (void *)&fake_context) bad_args++; ref_change(&context_refs, -1); return (DWORD)context_refs; }
/* Flush: records when it ran in the current frame (= the number of Present calls so far) and spins like a driver submitting work. */
static void WINAPI fake_flush(void *self) {
    long long now, end;
    QueryPerformanceCounter(&now);
    if (self != (void *)&fake_context) bad_args++;
    if (device_refs != 1 || context_refs != 1) bad_order++;
    DWORD i = calls;
    flush_calls++;
    if (i < NMAX) { if (flush_n[i] < 255) flush_n[i]++; flush_t[i] = now; }
    end = now + (long long)((DWORD)freq / 1000 * (long_flush((int)i) ? LONG_FLUSH_US : flush_us) / 1000);
    do QueryPerformanceCounter(&now); while (now < end);
    if (i < NMAX) flush_end_t[i] = now;
}
static HRESULT WINAPI device_bad_slot(void) { device_bad_slots++; bad_args++; return (HRESULT)0x80004005; }

/* ---- setup-report fakes (the first frame's gpu: and display: lines) ----
   device slot 0 QueryInterface(IID_IDXGIDevice) -> fake IDXGIDevice (slot 7 GetAdapter) -> fake render adapter (slot 8 GetDesc,
   slot 9 CheckInterfaceSupport(IID_IDXGIDevice)); swap chain slot 15 GetContainingOutput -> fake output (slot 6
   GetParent(IID_IDXGIAdapter)) -> the render adapter, or with [VinceFixTest] Hybrid=1 a second (Intel) adapter while the render
   one reports AMD. Output=fail makes GetContainingOutput fail with DXGI_ERROR_NOT_FOUND. Every other slot fails the run. */
static const BYTE iid_dxgi_device[16] = {0xFA, 0x77, 0xEC, 0x54, 0x77, 0x13, 0xE6, 0x44, 0x8C, 0x32, 0x88, 0xFD, 0x5F, 0x44, 0xC8, 0x4C};  /* {54ec77fa-1377-44e6-8c32-88fd5f44c84c} */
static const BYTE iid_dxgi_adapter[16] = {0xE1, 0xE7, 0x11, 0x24, 0xAC, 0x12, 0xCF, 0x4C, 0xBD, 0x14, 0x97, 0x98, 0xE8, 0x53, 0x4D, 0xC0}; /* {2411e7e1-12ac-4ccf-bd14-9798e8534dc0} */
#define FAKE_UMD_VERSION ((long long)((31u << 16) | 0) << 32 | ((15u << 16) | 5222)) /* 31.0.15.5222 */
#define DXGI_ERROR_NOT_FOUND ((HRESULT)0x887A0002)
typedef struct { void **vtbl; int refs, lowest; } FakeObject;
static void *dxgi_device_vtable[DXGI_DEVICE_SLOTS], *adapter_vtable[ADAPTER_SLOTS], *output_vtable[OUTPUT_SLOTS];
static FakeObject fake_dxgi_device, fake_render_adapter, fake_display_adapter, fake_output;
static int hybrid, output_fails;
static DWORD setup_bad_calls; /* wrong IID, wrong object, or a slot VinceFix must not call */
static DWORD render_luid = 0x1234; /* the render adapter's LUID; the recheck scenario changes it (the game now renders elsewhere) */

static int same_iid(const BYTE *iid, const BYTE *want) { for (int k = 0; k < 16; k++) if (!iid || iid[k] != want[k]) return 0; return 1; }
static DWORD WINAPI fake_add_ref(FakeObject *self) { return (DWORD)++self->refs; }
static DWORD WINAPI fake_release(FakeObject *self) { if (--self->refs < self->lowest) self->lowest = self->refs; return (DWORD)self->refs; }
static FakeObject *hand_out(FakeObject *object) { object->refs++; return object; }
static HRESULT WINAPI setup_bad_slot(void) { setup_bad_calls++; bad_args++; return (HRESULT)0x80004005; }

static HRESULT WINAPI fake_device_query_interface(void *self, const BYTE *iid, void **object) {
    if (self != (void *)&fake_device || !object || !same_iid(iid, iid_dxgi_device)) { setup_bad_calls++; if (object) *object = 0; return (HRESULT)0x80004002; }
    *object = hand_out(&fake_dxgi_device);
    return 0;
}
static HRESULT WINAPI fake_get_adapter(FakeObject *self, void **adapter) {
    if (self != &fake_dxgi_device || !adapter) { setup_bad_calls++; return (HRESULT)0x80004003; }
    *adapter = hand_out(&fake_render_adapter);
    return 0;
}
static HRESULT WINAPI fake_adapter_get_desc(FakeObject *self, DxgiAdapterDesc *desc) {
    static const WCHAR render_name[] = L"VinceFix Test GPU", display_name[] = L"VinceFix Test iGPU";
    int render = self == &fake_render_adapter;
    if ((!render && self != &fake_display_adapter) || !desc) { setup_bad_calls++; return (HRESULT)0x80004003; }
    const WCHAR *name = render ? render_name : display_name;
    for (int k = 0; k < 128; k++) desc->description[k] = 0;
    for (int k = 0; name[k]; k++) desc->description[k] = name[k];
    desc->vendor_id = render ? (hybrid ? 0x1002 : 0x10DE) : 0x8086;
    desc->device_id = render ? 0x2684 : 0x4680;
    desc->subsys_id = desc->revision = 0;
    desc->dedicated_video_memory = render ? 1536u << 20 : 128u << 20;
    desc->dedicated_system_memory = desc->shared_system_memory = 0;
    desc->adapter_luid.low = render ? render_luid : 0x5678;
    desc->adapter_luid.high = 0;
    return 0;
}
static HRESULT WINAPI fake_check_interface_support(FakeObject *self, const BYTE *iid, long long *version) {
    if (self != &fake_render_adapter || !version || !same_iid(iid, iid_dxgi_device)) { setup_bad_calls++; return (HRESULT)0x887A0004; }
    *version = FAKE_UMD_VERSION;
    return 0;
}
static HRESULT WINAPI fake_get_containing_output(void *self, void **output) {
    if (self != renderer.swap_chain || !output) { setup_bad_calls++; return (HRESULT)0x80004003; }
    if (output_fails) { *output = 0; return DXGI_ERROR_NOT_FOUND; }
    *output = hand_out(&fake_output);
    return 0;
}
static HRESULT WINAPI fake_output_get_parent(FakeObject *self, const BYTE *iid, void **parent) {
    if (self != &fake_output || !parent || !same_iid(iid, iid_dxgi_adapter)) { setup_bad_calls++; return (HRESULT)0x80004002; }
    *parent = hand_out(hybrid ? &fake_display_adapter : &fake_render_adapter);
    return 0;
}
static void init_fake_object(FakeObject *object, void **vtable, int slots) {
    object->vtbl = vtable;
    for (int i = 0; i < slots; i++) vtable[i] = (void *)setup_bad_slot;
    vtable[SLOT_ADD_REF] = (void *)fake_add_ref;
    vtable[SLOT_RELEASE] = (void *)fake_release;
}
static void init_setup_fakes(void) {
    init_fake_object(&fake_dxgi_device, dxgi_device_vtable, DXGI_DEVICE_SLOTS);
    dxgi_device_vtable[SLOT_GET_ADAPTER] = (void *)fake_get_adapter;
    init_fake_object(&fake_render_adapter, adapter_vtable, ADAPTER_SLOTS);
    fake_display_adapter.vtbl = adapter_vtable;
    adapter_vtable[SLOT_ADAPTER_GET_DESC] = (void *)fake_adapter_get_desc;
    adapter_vtable[SLOT_CHECK_INTERFACE_SUPPORT] = (void *)fake_check_interface_support;
    init_fake_object(&fake_output, output_vtable, OUTPUT_SLOTS);
    output_vtable[SLOT_GET_PARENT] = (void *)fake_output_get_parent;
}
/* The setup report's objects: all released again (references back at 0, never below), no wrong call. */
static void check_setup_fakes(void) {
    FakeObject *all[4] = {&fake_dxgi_device, &fake_render_adapter, &fake_display_adapter, &fake_output};
    int balanced = 1;
    for (int k = 0; k < 4; k++) if (all[k]->refs != 0 || all[k]->lowest < 0) balanced = 0;
    out("      setup report objects: references now IDXGIDevice "); out_dec((DWORD)fake_dxgi_device.refs); out(", render adapter ");
    out_dec((DWORD)fake_render_adapter.refs); out(", display adapter "); out_dec((DWORD)fake_display_adapter.refs); out(", output ");
    out_dec((DWORD)fake_output.refs); out(balanced ? " (never below 0)" : " (UNBALANCED)"); out("; "); out_dec(setup_bad_calls); out(" wrong calls\r\n");
    check(balanced && setup_bad_calls == 0, "setup report: every IDXGIDevice, adapter and output reference released, no other slot or IID used");
}

/* ---- [VinceFixTest] RefreshHz=<n>: a fake refresh rate for the game's display ----
   X3DAudio1_7.dll's import of user32!EnumDisplaySettingsW is pointed at fake_enum_display_settings before the first frame
   (when VinceFix checks the display), so the display rules can be tested on any monitor without test code in the DLL. */
typedef struct {            /* DEVMODEW (wingdi.h); only the fields used here are named */
    WCHAR device_name[32];
    WORD spec_version, driver_version, size, driver_extra;
    DWORD fields;
    BYTE position_to_pels_height[108];
    DWORD display_frequency;
    BYTE icm_to_panning[32];
} DevMode;
_Static_assert(sizeof(DevMode) == 220 && __builtin_offsetof(DevMode, size) == 68 && __builtin_offsetof(DevMode, display_frequency) == 184, "DEVMODEW layout");
typedef struct { DWORD names, time_date_stamp, forwarder_chain, dll_name, slots; } ImportDescriptor; /* IMAGE_IMPORT_DESCRIPTOR */
_Static_assert(sizeof(ImportDescriptor) == 20, "IMAGE_IMPORT_DESCRIPTOR layout");
typedef struct {            /* IMAGE_SECTION_HEADER; only the fields used here are named */
    BYTE name[8];
    DWORD virtual_size, virtual_address, raw_size, raw_pointer, relocations, line_numbers;
    WORD relocation_count, line_number_count;
    DWORD characteristics;
} SectionHeader;
_Static_assert(sizeof(SectionHeader) == 40 && __builtin_offsetof(SectionHeader, characteristics) == 36, "IMAGE_SECTION_HEADER layout");
#define PE_HEADER_OFFSET 0x3C              /* IMAGE_DOS_HEADER.e_lfanew */
#define PE_SECTION_COUNT (4 + 2)           /* from the PE signature: IMAGE_FILE_HEADER.NumberOfSections */
#define PE_OPTIONAL_HEADER_SIZE (4 + 16)   /* IMAGE_FILE_HEADER.SizeOfOptionalHeader */
#define PE_OPTIONAL_HEADER (4 + 20)        /* the optional header, after the signature and the file header */
#define PE_IMAGE_SIZE (PE_OPTIONAL_HEADER + 56) /* IMAGE_OPTIONAL_HEADER32.SizeOfImage */
#define SECTION_EXECUTE 0x20000000         /* IMAGE_SCN_MEM_EXECUTE */
#define PAGE_READWRITE 0x04
#define PE_IMPORT_DIRECTORY (4 + 20 + 104) /* from the PE signature: signature, file header, optional header to data directory 1 (PE32) */
#define IMPORT_BY_ORDINAL 0x80000000u
static int refresh_faked;
static DWORD fake_refresh_hz, enum_calls, enum_bad_args;

/* ---- [VinceFixTest] Recheck=1: two fake monitors ----
   X3DAudio1_7.dll's user32!MonitorFromWindow and GetMonitorInfoW imports also point at fakes: the game's window is on fake
   monitor 1 or 2 (fake_monitor), named \\.\DISPLAY1 / \\.\DISPLAY2, and the fake EnumDisplaySettingsW must be asked for
   that one. monitor_n counts MonitorFromWindow calls per frame. */
typedef struct { DWORD size; long monitor[4], work[4]; DWORD flags; WCHAR device[32]; } MonitorInfoEx; /* MONITORINFOEXW */
_Static_assert(sizeof(MonitorInfoEx) == 104 && __builtin_offsetof(MonitorInfoEx, device) == 40, "MONITORINFOEXW layout");
#define FAKE_MONITOR(n) ((HANDLE)(ULONG_PTR)(0x1000 + (n)))
#define DISPLAY_NAME_DIGIT 11 /* \\.\DISPLAYn */
static int monitors_faked;
static DWORD fake_monitor = 1, monitor_calls, monitor_bad_args;
static BYTE monitor_n[NMAX];
static HANDLE WINAPI fake_monitor_from_window(HANDLE window, DWORD flags) {
    monitor_calls++;
    if (frame_now < NMAX && monitor_n[frame_now] < 255) monitor_n[frame_now]++;
    if (window != 0 || flags != 2 /* MONITOR_DEFAULTTONEAREST */) monitor_bad_args++;
    return FAKE_MONITOR(fake_monitor);
}
static BOOL WINAPI fake_get_monitor_info(HANDLE monitor, MonitorInfoEx *info) {
    static const WCHAR name[] = L"\\\\.\\DISPLAY1";
    if ((monitor != FAKE_MONITOR(1) && monitor != FAKE_MONITOR(2)) || !info || info->size != sizeof *info) { monitor_bad_args++; return 0; }
    for (unsigned k = 0; k < sizeof name / sizeof name[0]; k++) info->device[k] = name[k];
    info->device[DISPLAY_NAME_DIGIT] = monitor == FAKE_MONITOR(2) ? L'2' : L'1';
    return 1;
}

static BOOL WINAPI fake_enum_display_settings(const WCHAR *device, DWORD mode, DevMode *devmode) {
    enum_calls++;
    if (!device || device[0] != L'\\' || mode != (DWORD)-1 /* ENUM_CURRENT_SETTINGS */ || !devmode || devmode->size != sizeof *devmode) { enum_bad_args++; return 0; }
    if (monitors_faked && device[DISPLAY_NAME_DIGIT] != (WCHAR)(L'0' + fake_monitor)) enum_bad_args++; /* asked for the wrong monitor */
    devmode->display_frequency = fake_refresh_hz;
    return 1;
}
static int same_name(const char *a, const char *b) { /* ASCII, case-insensitive */
    for (; *a && (*a | 0x20) == (*b | 0x20); a++, b++) {}
    return !*a && !*b;
}
/* The import slot of dll!function in a loaded module's import table, 0 if there is none. */
static void **find_import_slot(const BYTE *module, const char *dll, const char *function) {
    const BYTE *nt = module + *(const DWORD *)(module + PE_HEADER_OFFSET);
    DWORD rva = *(const DWORD *)(nt + PE_IMPORT_DIRECTORY);
    for (const ImportDescriptor *d = (const ImportDescriptor *)(module + rva); rva && d->dll_name; d++) {
        if (!d->names || !same_name((const char *)(module + d->dll_name), dll)) continue;
        const DWORD *names = (const DWORD *)(module + d->names);
        for (int i = 0; names[i]; i++)
            if (!(names[i] & IMPORT_BY_ORDINAL) && same_name((const char *)(module + names[i] + 2 /* Hint */), function)) return (void **)(module + d->slots) + i;
    }
    return 0;
}
static int point_import_at(const char *dll, const char *function, void *fake) { /* in X3DAudio1_7.dll; 0 if not found */
    void **slot = find_import_slot((const BYTE *)GetModuleHandleW(L"X3DAudio1_7.dll"), dll, function);
    DWORD protect;
    if (!slot || !VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &protect)) return 0;
    *slot = fake;
    VirtualProtect(slot, sizeof *slot, protect, &protect);
    return 1;
}
static int fake_the_refresh_rate(DWORD hz) {
    fake_refresh_hz = hz;
    return point_import_at("user32.dll", "EnumDisplaySettingsW", (void *)fake_enum_display_settings);
}
static int fake_the_monitors(void) {
    monitors_faked = point_import_at("user32.dll", "MonitorFromWindow", (void *)fake_monitor_from_window) &&
                     point_import_at("user32.dll", "GetMonitorInfoW", (void *)fake_get_monitor_info);
    return monitors_faked;
}
static void check_fake_display(void) {
    if (!refresh_faked) return;
    out("      fake display: "); out_dec(fake_refresh_hz); out(" Hz, EnumDisplaySettingsW called "); out_dec(enum_calls); out(" time(s), ");
    out_dec(enum_bad_args); out(" with unexpected arguments\r\n");
    check(enum_calls >= 1 && enum_bad_args == 0, "fake display: VinceFix asked EnumDisplaySettingsW for the current mode of a named display");
}
static HRESULT WINAPI context_bad_slot(void) { context_bad_slots++; bad_args++; return (HRESULT)0x80004005; }

static int count_prefix(const BYTE **first) { /* matches of prefix[] in this exe's executable sections */
    const BYTE *base = (const BYTE *)GetModuleHandleW(0), *nt = base + *(const DWORD *)(base + PE_HEADER_OFFSET);
    const SectionHeader *sec = (const SectionHeader *)(nt + PE_OPTIONAL_HEADER + *(const WORD *)(nt + PE_OPTIONAL_HEADER_SIZE));
    int count = 0;
    *first = 0;
    for (int s = 0; s < *(const WORD *)(nt + PE_SECTION_COUNT); s++, sec++) {
        if (!(sec->characteristics & SECTION_EXECUTE)) continue;
        const BYTE *code = base + sec->virtual_address;
        DWORD size = sec->virtual_size;
        for (DWORD i = 0; i + 14 <= size; i++) {
            int k = 0;
            while (k < 14 && code[i + k] == prefix[k]) k++;
            if (k == 14) { if (!count) *first = code + i; count++; }
        }
    }
    return count;
}

static void sort(double *a, int n) { for (int i = 1; i < n; i++) { double v = a[i]; int j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; } }
static double pct(const double *a, int n, int p) { return n ? a[(n - 1) * p / 100] : 0.0; }
static int count_le(const double *a, int n, double limit) { int c = 0; for (int i = 0; i < n; i++) if (a[i] <= limit) c++; return c; }

static double cdel[NF], dlag[NF], ivl[NF];

/* ---- scenarios for PresentHold=auto and FrameCap (chosen from the VinceFix.ini next to the exe) ---------------------- */
/* SC_OFF: everything turned off at the first frame (a 60 Hz display); SC_CAP_ASIDE / SC_CAP_GPU: FrameCap with a second
   limiter, or a "GPU-bound" Present ([VinceFixTest] Limiter=60 / gpu); SC_LIVE: LiveReload=1; SC_CAP_JOIN: a second limiter
   that joins mid-run at another phase ([VinceFixTest] Limiter=join); SC_RECHECK: the display changes mid-run (Recheck=1) */
enum { SC_AUTO, SC_CAP_HOLD, SC_CAP_ONLY, SC_CAP_AUTO, SC_OFF, SC_CAP_ASIDE, SC_CAP_GPU, SC_LIVE, SC_CAP_JOIN, SC_RECHECK, SC_CAP_EDGE };
/* SC_CAP_EDGE ([VinceFixTest] Limiter=edge, FrameCap=60, PresentHold=0): a GPU just faster than the cap. Each frame's work
   is 2 ms and its Present takes EDGE_BUSY_US (the GPU rendering it), so the game would run at 61.5 FPS uncapped. Capped at
   60, Present blocks 14.3 ms while FrameCap waits only 0.4 ms after it: that looks like another limiter. */
#define EDGE_BUSY_US 14260
#define EDGE_SETTLED 300    /* frames by which FrameCap must be on for good */
#define JOIN_AT 300         /* SC_CAP_JOIN: the frame whose Present the limiter joins */
#define JOIN_OFFSET_US 6000 /* its grid starts this long after that Present's entry: FrameCap still waits after it */
#define JOIN_SETTLED 360    /* frames after JOIN_AT within which FrameCap must have stepped aside (6 windows) */
#define RECHECK_SWAP 300    /* SC_RECHECK: the game presents through a second swap chain, on a 60 Hz display */
#define RECHECK_MOVE 630    /* its window moves to the other (280 Hz) monitor */
#define RECHECK_REUSE 930   /* a new swap chain at the same address (2560x1440), on another render adapter */
#define GPU_FPS 58          /* SC_CAP_GPU: the frame rate its Present allows in the first phase, just below the cap */
#define BUSY_PRESENT_US 1500 /* SC_CAP_GPU, second phase: Present takes this long, while FrameCap paces */
#define WINDOW 300          /* VinceFix's auto measurement window, in frames */
#define BURST_LEN 30
#define HELD_LAG_US 100.0   /* held frame: its Present entry is more than this after the end of its work (then entry - start is the hold) */
/* work per frame in tenths of a ms: 24 values from 1.0 to 3.0 (phase A, C, D) or 3.0 to 5.0 (B), and one tail value per cycle of 25 (4 %;
   the spec's "about 3 %" would make the 97th percentile of the burst window's 270 holdable frames sit on the edge of the bucket) */
static const DWORD workA[25] = {10, 25, 16, 30, 12, 21, 28, 14, 19, 26, 11, 23, 30, 15, 18, 27, 13, 22, 29, 17, 24, 20, 10, 20, 34};
static const DWORD workB[25] = {30, 45, 36, 50, 32, 41, 48, 34, 39, 46, 31, 43, 50, 35, 38, 47, 33, 42, 49, 37, 44, 40, 30, 40, 56};

static int scen, nframes, ph_len, b_first, b_last, burst_at;
static const char *sc_name;
static double grid_us;                        /* the frame-start spacing the scenario expects */
static double auto_shift_us;                  /* auto targets rise by the Flush time when held frames are flushed (their work includes it) */
/* per frame: Present entry - start, and the wait before Present: entry - (start + work), or entry - the end of its Flush */
static double hold_us[NMAX], lag_us[NMAX], scratch[NMAX];

static unsigned char preempted[NMAX]; /* this host's work loop ended > 0.3 ms late: Windows paused the test itself */
static void measure_frames(int n) {
    for (int i = 0; i < n; i++) {
        hold_us[i] = (double)(entry_t[i] - start_t[i]) * us_per_tick;
        long long waited_from = flush_n[i] ? flush_end_t[i] : start_t[i] + work_t[i]; /* a frame that ran past the hold after its Flush didn't wait */
        lag_us[i] = (double)(entry_t[i] - waited_from) * us_per_tick;
    }
}

/* Auto scenario: four 6 ms frames scattered over one 60-frame block of phase A (frames 605, 620, 635, 650). VinceFix's block
   raise needs 6 frames that miss the target, so these (like frames Windows paused) must not raise it; b and a would see it. */
static int scattered_heavy(int i) { return scen == SC_AUTO && i >= 605 && i <= 650 && i % 15 == 5; }

/* SC_CAP_JOIN, after FrameCap stepped aside: late frames whose lost time the catching-up limiter takes off the next
   frame. 780 and 900 end a step-aside window (VinceFix's windows end at frames 60, 120 ..), so the early frame starts the
   next window: 25 ms of work there takes 8.3 ms off the next window, 30 ms (with the 4 ms hold) 13.3 ms. */
/* 1080 and 1200 (33 ms) each end a window, which the late frame makes slower, and the catch-up then takes 16.3 ms off the
   next one, which counts as faster: slow / faster / slow / faster. Two faster windows that aren't back to back must not
   bring FrameCap back (one alone is a catch-up). */
#define JOIN_HEAVY_FRAMES 5
static DWORD join_heavy_work10(int i) { return i == 780 || i == 1050 ? 250 : i == 900 ? 300 : i == 1080 || i == 1200 ? 330 : 0; }

static DWORD scen_work10(int i) {
    if (scen == SC_CAP_ONLY && i >= 100 && i <= 500 && i % 100 == 0) return 250; /* a few 25 ms frames */
    if (scen == SC_CAP_JOIN && join_heavy_work10(i)) return join_heavy_work10(i);
    if (scen == SC_CAP_EDGE) return 20;
    if (i >= burst_at && i < burst_at + BURST_LEN) return 400;                  /* the loading stretch (auto scenario) */
    if (scattered_heavy(i)) return 60;
    if (flush_run && scen == SC_CAP_HOLD && i % 100 == 75) return 60;           /* past the 4 ms hold: must not be flushed */
    return (i >= b_first && i < b_last ? workB : workA)[i % 25];
}
/* The fake limiter (a stand-in for the NVIDIA cap, or VSync on a 60 Hz display): phase C of the auto scenario and the cap
   scenarios have none. */
static int scen_limiter(int i) {
    if (scen == SC_CAP_ASIDE || scen == SC_CAP_GPU) return i < ph_len; /* the first phase only */
    if (scen == SC_CAP_JOIN) return i >= JOIN_AT;
    if (scen == SC_RECHECK) return i >= RECHECK_SWAP && i < RECHECK_MOVE; /* VSync on the 60 Hz display */
    return scen == SC_OFF || (scen == SC_AUTO && i / ph_len != 2);
}

/* VinceFix.log's size after each frame's game_present returned (taken inside the next frame's work time). VinceFix writes its
   render-thread lines (auto targets, the stats line) after the real Present, so a write delays THIS host's start of the next
   frame, although the DLL keeps that frame's present on time: follows_write marks those frames. */
static DWORD log_len[NMAX];
static unsigned char follows_write[NMAX];
static void mark_log_writes(int n) {
    for (int i = 0; i < n; i++) follows_write[i] = i >= 2 && log_len[i - 1] > log_len[i - 2];
}

/* Frames the timing checks leave out: the first ones, 5 frames around phase starts (work and limiter change) and the burst,
   and the frames whose start followed one of VinceFix's log writes (see follows_write). Window ends are timed like any other
   frame unless a line was written there. */
static int skip_frame(int i) {
    int p = i % ph_len;
    if (preempted[i] || follows_write[i]) return 1;
    if (long_flush(i) || long_flush(i - 1)) return 1; /* HoldFlush: a 3.5 ms Flush makes the frame late on purpose */
    if (p <= 5 || p >= ph_len - 5) return 1;
    return i >= burst_at - 5 && i < burst_at + BURST_LEN + 5;
}

static double dabs(double v) { return v < 0 ? -v : v; }
static void check_p(int ok, const char *tag, const char *what) { out(ok ? "PASS  " : "FAIL  "); out(sc_name); out(tag); out(": "); out(what); out("\r\n"); if (!ok) failures++; }
static void mean_sd(const double *a, int n, double *mean, double *sd) {
    double m = 0, v = 0;
    for (int i = 0; i < n; i++) m += a[i];
    if (n) m /= n;
    for (int i = 0; i < n; i++) v += (a[i] - m) * (a[i] - m);
    if (n) v /= n;
    *mean = m; *sd = dsqrt(v);
}
static void out_dist(const double *sorted, int n) { /* min / p5 / p50 / p95 / max of a sorted array, in ms */
    out("min "); out_ms(n ? sorted[0] : 0); out(", p5 "); out_ms(pct(sorted, n, 5)); out(", p50 "); out_ms(pct(sorted, n, 50));
    out(", p95 "); out_ms(pct(sorted, n, 95)); out(", max "); out_ms(n ? sorted[n - 1] : 0);
}

/* Frames first..last-1 with work <= max_work10 tenths of a ms: the measured hold lies within centre +- tol for >= 95 %. */
static void check_hold(const char *tag, int first, int last, DWORD max_work10, double centre_us, double tol_us, const char *what) {
    int n = 0, good = 0;
    for (int i = first; i < last; i++) {
        if (scen_work10(i) > max_work10 || skip_frame(i)) continue;
        scratch[n++] = hold_us[i];
        if (hold_us[i] >= centre_us - tol_us && hold_us[i] <= centre_us + tol_us) good++;
    }
    sort(scratch, n);
    out("      "); out(sc_name); out(tag); out(": frames "); out_dec((DWORD)first); out("-"); out_dec((DWORD)(last - 1)); out(", ");
    out_dec((DWORD)n); out(" with work <= "); out_ms(max_work10 * 100.0); out(", entry - start: "); out_dist(scratch, n);
    out("; "); out_dec((DWORD)good); out(" within "); out_ms(centre_us); out(" +- "); out_ms(tol_us); out("\r\n");
    check_p(n >= 50 && good * 100 >= n * 95, tag, what);
}

/* Frames first..last-1: no hold, the Present entry is <= 0.3 ms after the end of the work for >= 95 %. */
static void check_nohold(const char *tag, int first, int last, const char *what) {
    int n = 0;
    for (int i = first; i < last; i++) if (!skip_frame(i)) scratch[n++] = lag_us[i];
    sort(scratch, n);
    out("      "); out(sc_name); out(tag); out(": frames "); out_dec((DWORD)first); out("-"); out_dec((DWORD)(last - 1)); out(", ");
    out_dec((DWORD)n); out(" frames, entry - (start + work): p50 "); out_us(pct(scratch, n, 50)); out(", p95 "); out_us(pct(scratch, n, 95));
    out(", max "); out_us(n ? scratch[n - 1] : 0); out("\r\n");
    check_p(n >= 50 && count_le(scratch, n, 300.0) * 100 >= n * 95, tag, what);
}

/* Entry-to-entry interval of consecutive held frames (neither skipped) among frames first..last-1: |interval - grid| <= 0.1 ms
   for all but 2 % of the pairs, never >= 5 ms. Not for phase C of the auto scenario, where nothing paces the frames. */
static void check_smooth(const char *tag, int first, int last, const char *what) {
    int np = 0, nbad = 0, worst_i = 0;
    double worst = 0;
    for (int i = first > 1 ? first : 1; i < last; i++) {
        if (scen == SC_AUTO && i / ph_len == 2) continue;
        if (skip_frame(i) || skip_frame(i - 1) || lag_us[i] <= HELD_LAG_US || lag_us[i - 1] <= HELD_LAG_US) continue;
        double d = dabs((double)(entry_t[i] - entry_t[i - 1]) * us_per_tick - grid_us);
        np++;
        if (d > 100.0 && nbad++ < 8) { out(nbad == 1 ? "      first pairs off the spacing (frame: |off|): " : ", "); out_dec((DWORD)i); out(": "); out_us(d); }
        if (d > worst) { worst = d; worst_i = i; }
    }
    if (nbad) out("\r\n");
    out("      "); out(sc_name); out(tag); out(": "); out_dec((DWORD)np); out(" pairs of consecutive held frames, "); out_dec((DWORD)nbad);
    out(" with |interval - "); out_us(grid_us); out("| > 0.1 ms, worst "); out_us(worst); out(" (frame "); out_dec((DWORD)worst_i); out(")\r\n");
    /* 2 % and 5 ms: the timer has woken over 3 ms late now and then when the PC is busy (a game, OBS);
       a stall from a missed timer (the old 100 ms timeout) or a bad ramp still fails. */
    int need = last - first >= 600 ? 300 : (last - first) * 3 / 4; /* a whole run, or one live-reload phase (240 frames, all held) */
    check_p(np >= need && nbad * 50 <= np && worst < 5000.0, tag, what);
}

/* Never early: between consecutive held frames the measured hold (the hold in force plus noise) must not fall by more than 0.07 ms.
   A fall right after an upward spike of the frame before is that frame being late (timer noise), not this one early. */
static void check_early(const char *tag, const char *what) {
    int np = 0, early = 0, late = 0, worst_i = 0;
    double worst = 0;
    for (int i = 2; i < nframes; i++) {
        if (skip_frame(i) || skip_frame(i - 1) || lag_us[i] <= HELD_LAG_US || lag_us[i - 1] <= HELD_LAG_US) continue;
        np++;
        double drop = hold_us[i - 1] - hold_us[i];
        if (-drop > 70.0) late++;
        if (drop > 70.0 && !skip_frame(i - 2) && lag_us[i - 2] > HELD_LAG_US && hold_us[i - 1] - hold_us[i - 2] > 70.0 &&
            hold_us[i] >= hold_us[i - 2] - 70.0) continue; /* recovery, as long as it doesn't end below the hold before the spike */
        /* With FrameCap the hold counts from the grid point, not from this host's start: a late cap wake makes the start
           late and the measured hold shorter by as much, while the present stays on the grid (f checks that). */
        if (drop > 70.0 && scen != SC_AUTO && (double)(start_t[i] - start_t[i - 1]) * us_per_tick - grid_us > drop - 70.0) continue;
        if (drop > 70.0) early++;
        if (drop > worst) { worst = drop; worst_i = i; }
    }
    out("      "); out(sc_name); out(tag); out(": "); out_dec((DWORD)np); out(" pairs of consecutive held frames, worst fall of the measured hold "); out_us(worst);
    out(" (frame "); out_dec((DWORD)worst_i); out("), "); out_dec((DWORD)early); out(" falls > 0.07 ms, "); out_dec((DWORD)late); out(" rises > 0.07 ms (late wake-ups)\r\n");
    /* Small falls (0.07-0.2 ms, at most 1 in 500 pairs) are this host being preempted between VinceFix's timestamp and its own;
       a hold that really dropped (a ramp bug, a skipped wait) falls by more. */
    check_p(np >= 300 && early * 500 <= np && worst <= 200.0, tag, what);
}

static int count_off_grid(int m) { int n = 0; for (int i = 0; i < m; i++) if (dabs(scratch[i] - grid_us) > 100.0) n++; return n; }

/* Frame-start spacing among frames first..last-1 (cap scenarios): mean grid +- 0.01 ms, >= 98 % of intervals within 0.1 ms of
   the grid (a rare late timer wake on a busy PC moves one start; the next interval makes up for it, so the mean stays). The
   interval after a 25/40 ms frame is not on the grid. */
static void check_grid(const char *tag, int first, int last, const char *what) {
    int m = 0;
    for (int i = first > 1 ? first : 1; i < last; i++) {
        if (skip_frame(i) || skip_frame(i - 1) || scen_work10(i - 1) >= 200) continue;
        scratch[m++] = (double)(start_t[i] - start_t[i - 1]) * us_per_tick;
    }
    double mean, sd, mn = 1e9, mx = 0;
    mean_sd(scratch, m, &mean, &sd);
    for (int i = 0; i < m; i++) { if (scratch[i] < mn) mn = scratch[i]; if (scratch[i] > mx) mx = scratch[i]; }
    int off = count_off_grid(m);
    out("      "); out(sc_name); out(tag); out(": "); out_dec((DWORD)m); out(" frame-start intervals: mean "); out_us(mean); out(" (grid "); out_us(grid_us);
    out("), std dev "); out_us(sd); out(", min "); out_us(mn); out(", max "); out_us(mx); out(", "); out_dec((DWORD)off); out(" off the grid by > 0.1 ms\r\n");
    check_p(m >= 100 && dabs(mean - grid_us) <= 10.0 && off * 50 <= m, tag, what);
}

/* FrameCap's carry: the QPC ticks per period are rarely whole (10 MHz / 60 = 166666.67), so VinceFix adds up the fraction and
   makes a period one tick longer whenever it reaches a tick. Without that the grid would run 0.67 ticks per frame fast: 40 us
   over 600 frames, far inside check_grid's +- 10 us on the mean. Over the longest stretch without a grid restart (an interval
   more than 1 ms over the period), the earliest frame start among its first 50 frames and among its last 50, each measured
   against the exact grid (n * frequency / fps), must agree within 5 us. The earliest, because a start is never before its
   grid point but can be late (a late timer wake, this host paused). */
static void check_carry(const char *tag, int fps, const char *what) {
    DWORD f32 = (DWORD)freq; /* 32-bit math only: no CRT division helpers here */
    double ticks_per_period = (double)f32 / fps;
    long long restart_gap = (long long)((DWORD)ticks_per_period + f32 / 1000);
    int a = 1, best_a = 1, best_len = 0;
    for (int i = 2; i <= nframes; i++) {
        if (i < nframes && start_t[i] - start_t[i - 1] <= restart_gap) continue;
        if (i - a > best_len) { best_len = i - a; best_a = a; }
        a = i;
    }
    double head = 1e18, tail = 1e18;
    for (int k = 0; k < 50 && k < best_len; k++) {
        int late = best_len - 1 - k;
        double h = (double)(start_t[best_a + k] - start_t[best_a]) - k * ticks_per_period;
        double t = (double)(start_t[best_a + late] - start_t[best_a]) - late * ticks_per_period;
        if (h < head) head = h;
        if (t < tail) tail = t;
    }
    double drift = (tail - head) * us_per_tick, without = -(double)(f32 % (DWORD)fps) / fps * (best_len - 50) * us_per_tick;
    out("      "); out(sc_name); out(tag); out(": frames "); out_dec((DWORD)best_a); out("-"); out_dec((DWORD)(best_a + best_len - 1));
    out(" without a grid restart; earliest start against the exact grid drifts "); out_us(drift); out(" from the first 50 frames to the last 50 (without the carry: about ");
    out_us(without); out(")\r\n");
    check_p(best_len >= 300 && dabs(drift) <= 5.0, tag, what);
}

/* Present-entry spacing of consecutive frames of work <= 3.5 ms (all held) among frames first..last-1: >= 98 % within 0.1 ms
   of the grid. */
static void check_entry_sd(const char *tag, int first, int last, const char *what) {
    int m = 0;
    for (int i = first > 1 ? first : 1; i < last; i++) {
        if (skip_frame(i) || skip_frame(i - 1) || scen_work10(i) > 35 || scen_work10(i - 1) > 35) continue;
        scratch[m++] = (double)(entry_t[i] - entry_t[i - 1]) * us_per_tick;
    }
    double mean, sd;
    mean_sd(scratch, m, &mean, &sd);
    int off = count_off_grid(m);
    out("      "); out(sc_name); out(tag); out(": "); out_dec((DWORD)m); out(" present-to-present intervals: mean "); out_us(mean); out(", std dev "); out_us(sd);
    out(", "); out_dec((DWORD)off); out(" off the grid by > 0.1 ms\r\n");
    check_p(m >= 100 && off * 50 <= m, tag, what);
}

/* The burst (auto scenario, b): after the first target (frame 300) no held frame in phase A holds more than 4.2 ms (plus the
   Flush time when flushed; frames with a 3.5 ms Flush are late on purpose and left out). */
static void check_burst(const char *tag, const char *what) {
    int n = 0, over = 0, worst_i = 0;
    double worst = 0, limit = 4200.0 + auto_shift_us;
    for (int i = 301; i < ph_len; i++) {
        if (preempted[i] || long_flush(i) || lag_us[i] <= HELD_LAG_US) continue;
        n++;
        if (hold_us[i] > limit) over++;
        if (hold_us[i] > worst) { worst = hold_us[i]; worst_i = i; }
    }
    out("      "); out(sc_name); out(tag); out(": frames 301-"); out_dec((DWORD)(ph_len - 1)); out(": "); out_dec((DWORD)n); out(" held frames, longest measured hold ");
    out_ms(worst); out(" (frame "); out_dec((DWORD)worst_i); out("), "); out_dec((DWORD)over); out(" above "); out_ms(limit); out("; the "); out_dec(BURST_LEN);
    out(" frames of 40 ms work start at frame "); out_dec((DWORD)burst_at); out("\r\n");
    /* A raise by the burst would hold hundreds of frames longer; a single late timer wake on a busy PC is one frame. */
    check_p(n >= 100 && over * 100 <= n, tag, what);
}

/* Cap-only scenario: after each 25 ms frame the next frame starts at once (the grid restarts there, interval = the 25 ms of work)
   and the interval after that is the grid again (no short catch-up frame). */
static void check_long_frames(const char *tag, const char *what) {
    int n = 0, good = 0;
    for (int i = 1; i + 2 < nframes; i++) {
        if (scen_work10(i) < 200) continue;
        double work = (double)work_t[i] * us_per_tick, late = (double)(start_t[i + 1] - start_t[i]) * us_per_tick;
        double next = (double)(start_t[i + 2] - start_t[i + 1]) * us_per_tick;
        n++;
        if (late >= work - 1.0 && late <= work + 300.0 && dabs(next - grid_us) <= 100.0) good++;
        out("      "); out(sc_name); out(tag); out(": frame "); out_dec((DWORD)i); out(": interval to the next start "); out_us(late);
        out(", then "); out_us(next); out("\r\n");
    }
    check_p(n >= 3 && good == n, tag, what);
}

/* GetDesc while the display stays the same: at the first frame (the display check), then only in every
   DISPLAY_CHECK_FRAMES-th frame (frames 60, 120 ..: VinceFix's cheap check for a changed display), once. Returns how many
   frames of the first n saw another number of calls. */
static int frames_off_desc_rhythm(int n) {
    int off = 0;
    for (int i = 0; i < n; i++) if (desc_n[i] != (i == 0 || i % DISPLAY_CHECK_FRAMES == 0)) off++;
    return off;
}
static void check_calls(DWORD bad_calls) {
    int desc_off = scen == SC_RECHECK ? 0 : frames_off_desc_rhythm(nframes); /* the recheck scenario checks its own */
    out("      "); out_dec(calls); out(" Present calls, "); out_dec(bad_args); out(" with wrong args, "); out_dec(bad_ret); out(" wrong return values, ");
    out_dec(bad_calls); out(" frames with a wrong call count, "); out_dec(bad_slot_calls); out(" calls of other swap chain slots, "); out_dec(desc_calls);
    out(" GetDesc call(s), "); out_dec((DWORD)desc_off); out(" frames off the once-per-"); out_dec(DISPLAY_CHECK_FRAMES); out("-frames rhythm\r\n");
    check_p(calls == (DWORD)nframes && bad_args == 0 && bad_ret == 0 && bad_calls == 0 && bad_slot_calls == 0 && desc_off == 0, "calls",
            "fake Present: one call per frame, args (fake swap chain, 1, 0), 0x087A0001 returned, no other slot called; GetDesc at the first frame and then once every 60 frames, never more");
}

/* ---- HoldFlush checks ---- */
static signed char expect_held[NMAX]; /* 1: work well inside the hold, 0: past it (or the first frame), -1: neither */

static void out_flush_counts(void) {
    out("      GetDevice "); out_dec(getdevice_calls); out(" call(s) ("); out_dec(bad_iid); out(" with another IID), GetImmediateContext ");
    out_dec(getcontext_calls); out(", Flush "); out_dec(flush_calls); out("; references now device "); out_dec((DWORD)device_refs);
    out(" / context "); out_dec((DWORD)context_refs); out(", lowest "); out(refs_min < 0 ? "-" : ""); out_dec((DWORD)(refs_min < 0 ? -refs_min : refs_min));
    out(", highest "); out_dec((DWORD)refs_max); out("; other device slots "); out_dec(device_bad_slots); out(", other context slots ");
    out_dec(context_bad_slots); out("\r\n");
}

/* Runs without a flush: only the first frame's setup report asks for the device (once); no context, no Flush. */
static void check_no_flush(void) {
    out_flush_counts();
    check(getdevice_calls == 1 && bad_iid == 0 && device_refs == 0 && getcontext_calls == 0 && flush_calls == 0 && device_bad_slots == 0 && context_bad_slots == 0,
          "no HoldFlush: GetDevice only once (the setup report, released), GetImmediateContext and Flush never");
}

/* [VinceFixTest] GetDevice=fail or GetContext=null: one try at the first held frame (after the setup report's GetDevice at
   the first frame), then VinceFix stops; every reference it got is released, and its log line comes after that frame's
   Present (VinceFix.log unchanged between the frame's start and its Present entry, grown after it). */
static void check_flush_failed(void) {
    out_flush_counts();
    if (getdevice_fails)
        check(getdevice_calls == 2 && bad_iid == 0 && getcontext_calls == 0 && flush_calls == 0 && device_refs == 0 && context_refs == 0 &&
                  device_bad_slots == 0 && context_bad_slots == 0,
              "HoldFlush with a failing GetDevice: tried once (right IID) after the setup report's try, then never again; no context, Flush or other device call");
    else
        check(getdevice_calls == 2 && bad_iid == 0 && getcontext_calls == 1 && flush_calls == 0 && device_refs == 0 && context_refs == 0 &&
                  refs_max == 1 && device_bad_slots == 0 && context_bad_slots == 0,
              "HoldFlush with no immediate context: GetImmediateContext once, the device released, then nothing more (GetDevice: setup report + that try)");
    out("      VinceFix.log size at frame 1's start "); out_dec(log_at_start); out(", at its Present entry "); out_dec(log_at_present);
    out(", after its Present "); out_dec(log_after); out(" bytes\r\n");
    check(log_at_start && log_at_present == log_at_start && log_after > log_at_present,
          "HoldFlush failure: the log line is written after the held frame's Present, not inside the hold");
}

/* The measured hold of the nearest held frame before frame i (within 5 frames), 0 if there is none. */
static double hold_before(int i) {
    for (int k = i - 1; k >= 1 && k >= i - 5; k--)
        if (!preempted[k] && !long_flush(k) && lag_us[k] > HELD_LAG_US) return hold_us[k];
    return 0.0;
}

static void check_flush(int n) {
    int multi = 0, not_n = 0, not_flushed = 0, held_n = 0, held_missing = 0, after_present = 0, timed = 0, long_n = 0, long_ok = 0, shown = 0;
    DWORD first = 0;
    double long_worst = 0;
    for (int i = 0; i < n; i++) {
        if (flush_n[i] > 1) multi++;
        if (i < STATS_FRAME) first += flush_n[i];
        if (expect_held[i] == 0) { not_n++; if (flush_n[i]) not_flushed++; }
        if (expect_held[i] == 1 && !preempted[i]) {
            held_n++;
            if (flush_n[i] != 1) { held_missing++; if (shown++ < 5) { out("      flush: frame "); out_dec((DWORD)i); out(" was expected to be held but saw "); out_dec(flush_n[i]); out(" Flush calls\r\n"); } }
        }
        if (!flush_n[i]) continue;
        if (flush_end_t[i] > entry_t[i]) after_present++;
        if (long_flush(i)) { /* passed the hold point: presents at once after the Flush; else (an auto hold above work + 3.5 ms): at the hold */
            double gap = (double)(entry_t[i] - flush_end_t[i]) * us_per_tick, hold = hold_before(i);
            int passed = (double)(flush_end_t[i] - start_t[i]) * us_per_tick >= hold;
            double off = passed ? gap : dabs(hold_us[i] - hold);
            long_n++;
            if (off <= 300.0) long_ok++;
            if (off > long_worst) long_worst = off;
            continue;
        }
        if (!preempted[i]) scratch[timed++] = (double)(flush_t[i] - (start_t[i] + work_t[i])) * us_per_tick;
    }
    sort(scratch, timed);
    out_flush_counts();
    out("      flush: frames 0-"); out_dec(STATS_FRAME - 1); out(": "); out_dec(first); out(" Flush calls\r\n"); /* build.ps1 reads this line */
    out("      flush count: "); out_dec((DWORD)held_n); out(" frames whose work fits the hold, "); out_dec((DWORD)held_missing); out(" without exactly one Flush; ");
    out_dec((DWORD)not_n); out(" past the hold (or first), "); out_dec((DWORD)not_flushed); out(" flushed; "); out_dec((DWORD)multi); out(" frames with more than one\r\n");
    check(multi == 0 && not_n >= 5 && not_flushed == 0 && held_n >= 300 && held_missing * 100 <= held_n,
          "flush count: one Flush per held frame (>= 99 %), none in frames that arrived after the hold or the first frame, never two");
    out("      flush order: "); out_dec((DWORD)after_present); out(" Flushes ended after their frame's Present; Flush start - end of the work over ");
    out_dec((DWORD)timed); out(" frames: p50 "); out_us(pct(scratch, timed, 50)); out(", p95 "); out_us(pct(scratch, timed, 95)); out(", max ");
    out_us(timed ? scratch[timed - 1] : 0); out("\r\n");
    check(after_present == 0 && timed >= 300 && count_le(scratch, timed, 100.0) * 100 >= timed * 95,
          "flush order: each Flush ends before its frame's Present and starts <= 0.1 ms after the work (before the hold's wait) for >= 95 %");
    out("      flush late: "); out_dec((DWORD)long_n); out(" frames with a 3.5 ms Flush, "); out_dec((DWORD)long_ok);
    out(" entered Present <= 0.3 ms after it, or at the hold if it ended before that (worst "); out_us(long_worst); out(")\r\n");
    check(long_n >= 5 && long_ok == long_n, "flush late: a frame whose Flush passed the hold point presents at once after the Flush (one that didn't, at the hold)");
    check(getdevice_calls == flush_calls + 1 && getcontext_calls == flush_calls && bad_iid == 0 && device_refs == 0 && context_refs == 0 &&
              refs_min == 0 && refs_max == 1 && bad_order == 0 && device_bad_slots == 0 && context_bad_slots == 0,
          "flush refs: GetDevice (IID_ID3D11Device), GetImmediateContext and Flush once each per flushed frame (GetDevice once more for the setup report), both held during Flush, all released");
}

/* ---- FrameCap steps aside (SC_CAP_ASIDE: FrameCap=60, PresentHold=4, HoldFlush=auto; the fake limiter for frames 0-599) ----
   VinceFix decides once per PACE_WINDOW frames. With the limiter from the start, the first window (frames 1-60) shows it,
   so frame 60 is the last one flushed; frame 600 turns the limiter off, the window of frames 601-660 shows that, and
   FrameCap is back (flushing again) from frame 661. Two windows are allowed for each, as a frame Windows paused can spoil
   one. Frames whose start is a Present return (FrameCap late behind the limiter, or aside) are timed from the fake
   Present's own return, so the step-aside line VinceFix writes after a Present doesn't hide the frame after it. */
#define PACE_WINDOW 60

/* Frames first..last-1 started at the previous Present's return: entry - that return = the hold +- 0.2 ms for >= 95 %,
   never more than 0.05 ms early. Left out: frames whose work (this host's, delayed by a log write or a pause) ended
   within 0.1 ms of the hold point, and the long-Flush ones. */
static void check_hold_from_return(const char *tag, int first, int last, double hold, const char *what) {
    int n = 0, good = 0, lowest_i = first;
    double lowest = 1e9;
    for (int i = first > 1 ? first : 1; i < last; i++) {
        if (long_flush(i) || (double)(start_t[i] + work_t[i] - return_t[i - 1]) * us_per_tick > hold - 100.0) continue;
        double h = (double)(entry_t[i] - return_t[i - 1]) * us_per_tick;
        n++;
        if (dabs(h - hold) <= 200.0) good++;
        if (h < lowest) { lowest = h; lowest_i = i; }
    }
    out("      "); out(sc_name); out(tag); out(": frames "); out_dec((DWORD)first); out("-"); out_dec((DWORD)(last - 1)); out(", ");
    out_dec((DWORD)n); out(" timed, entry - the previous Present's return: "); out_dec((DWORD)good); out(" within "); out_ms(hold);
    out(" +- 0.2 ms, shortest "); out_ms(lowest); out(" (frame "); out_dec((DWORD)lowest_i); out(")\r\n");
    check_p(n >= (last - first) / 2 && good * 100 >= n * 95 && lowest >= hold - 50.0, tag, what);
}

static void check_aside(double hold) {
    int last_on = 0, back = 0, flushed_n = 0, missing = 0;
    for (int i = 1; i < ph_len; i++) if (flush_n[i]) last_on = i;
    for (int i = nframes - 1; i >= ph_len; i--) if (flush_n[i]) back = i;
    for (int i = 1; i < nframes; i++) {
        if ((i > last_on && i < back) || preempted[i] || long_flush(i)) continue;
        flushed_n++;
        if (flush_n[i] != 1) missing++;
    }
    out_flush_counts();
    out("      "); out(sc_name); out("switch: last frame flushed with the other limiter "); out_dec((DWORD)last_on); out(", first flushed again after it stopped ");
    out_dec((DWORD)back); out("; "); out_dec((DWORD)missing); out(" of "); out_dec((DWORD)flushed_n); out(" frames while FrameCap ran without exactly one Flush\r\n");
    check_p(last_on >= PACE_WINDOW / 2 && last_on <= 2 * PACE_WINDOW, "aside", "FrameCap steps aside within 2 windows (120 frames) of the other limiter's start, and HoldFlush=auto stops flushing then");
    check_p(back > ph_len && back <= ph_len + 2 * PACE_WINDOW + 1, "back", "FrameCap comes back within 2 windows of the other limiter's end, and HoldFlush=auto flushes again");
    check_p(flushed_n >= 400 && missing * 100 <= flushed_n, "flush", "while FrameCap runs, one Flush per held frame (>= 99 %); none while it is aside");
    check(getdevice_calls == flush_calls + 1 && getcontext_calls == flush_calls && bad_iid == 0 && device_refs == 0 && context_refs == 0 &&
              bad_order == 0 && device_bad_slots == 0 && context_bad_slots == 0,
          "cap+aside: flush refs: GetDevice, GetImmediateContext and Flush once each per flushed frame (GetDevice once more for the setup report), all released");
    if (!last_on || !back) return;

    /* Stepping aside changes nothing about the timing here: FrameCap, late behind the (slightly slower) limiter, already
       started each frame at Present's return. Across the switch the hold and the limiter's spacing stay. */
    check_hold_from_return("1 hold", 6, ph_len - 5, hold, "with the other limiter, before and after stepping aside: presents at the previous Present's return + 4 ms +- 0.2 (>= 95 %), never early");
    int sw_bad = 0;
    double limiter_us = (double)limit_period * us_per_tick;
    for (int i = last_on - 3; i <= last_on + 3; i++)
        if (!preempted[i] && !preempted[i + 1] && dabs((double)(entry_t[i + 1] - entry_t[i]) * us_per_tick - limiter_us) > 100.0) sw_bad++;
    out("      "); out(sc_name); out("aside spacing: "); out_dec((DWORD)sw_bad); out(" of the 7 present-to-present intervals around frame "); out_dec((DWORD)last_on);
    out(" off the limiter's grid by > 0.1 ms\r\n");
    check_p(sw_bad == 0, "aside spacing", "the presents around the step-aside keep the limiter's spacing (no hurried or doubled frame)");

    /* Coming back: frame `back` is held from the last aside Present's return (not hurried, not waited twice), the next
       frame starts one period after that return (the grid re-anchored there), and the grid holds from there. */
    double first_hold = (double)(entry_t[back] - return_t[back - 1]) * us_per_tick;
    double anchor = (double)(start_t[back + 1] - return_t[back - 1]) * us_per_tick;
    int off = 0, n = 0;
    for (int i = back + 1; i < back + 31; i++) {
        if (preempted[i] || follows_write[i + 1] || long_flush(i) || long_flush(i + 1)) continue;
        n++;
        if (dabs((double)(start_t[i + 1] - start_t[i]) * us_per_tick - grid_us) > 100.0) off++;
    }
    out("      "); out(sc_name); out("back: frame "); out_dec((DWORD)back); out(" presents "); out_ms(first_hold); out(" after the last aside return, frame ");
    out_dec((DWORD)back + 1); out(" starts "); out_ms(anchor); out(" after it; then "); out_dec((DWORD)off); out(" of "); out_dec((DWORD)n);
    out(" frame-start intervals off the 60 FPS grid by > 0.1 ms\r\n");
    check_p(dabs(first_hold - hold) <= 200.0 && dabs(anchor - grid_us) <= 100.0 && n >= 20 && off == 0, "back spacing",
            "coming back, the grid re-anchors one period after the last Present return: the first frame holds 4 ms from it, the next starts 16.667 ms after it, the next 30 starts on the grid");
}

/* ---- a limiter that joins mid-run at another phase (SC_CAP_JOIN: FrameCap=60, PresentHold=4, HoldFlush=auto) ----
   The fake limiter joins at frame JOIN_AT with its grid JOIN_OFFSET_US after that frame's Present entry, so it releases
   each frame 6 ms after its Present call while FrameCap still waits 6.7 ms after that: both wait. VinceFix must slide
   FrameCap's grid toward the limiter's release and step aside within JOIN_SETTLED frames, with every present on the 60 FPS
   spacing meanwhile (no hurried or doubled frame), then stay aside through late frames the limiter catches up on. */
static void check_join(double hold) {
    int last_on = 0, before = 0, n = 0, off = 0, far = 0, worst_i = 0;
    double worst = 0;
    for (int i = 1; i < nframes; i++) if (flush_n[i]) { last_on = i; if (i < JOIN_AT) before++; }
    for (int i = JOIN_AT - 5; i < 775; i++) { /* to just before the first late frame (780) */
        if (preempted[i] || preempted[i - 1] || follows_write[i] || long_flush(i) || long_flush(i - 1)) continue;
        double d = dabs((double)(entry_t[i] - entry_t[i - 1]) * us_per_tick - grid_us);
        n++;
        if (d > 100.0) off++;
        if (d > 1000.0) far++;
        if (d > worst) { worst = d; worst_i = i; }
    }
    out_flush_counts();
    out("      "); out(sc_name); out("switch: "); out_dec((DWORD)before); out(" frames flushed before the limiter joined at frame "); out_dec(JOIN_AT);
    out(", the last one flushed (FrameCap stepped aside after it) "); out_dec((DWORD)last_on); out("\r\n");
    check_p(before >= 200 && last_on > JOIN_AT && last_on <= JOIN_AT + JOIN_SETTLED, "aside",
            "a limiter joining at another phase: FrameCap steps aside within 6 windows (360 frames), HoldFlush=auto stops then, and it never comes back (no Flush after that, through the late frames)");
    out("      "); out(sc_name); out("spacing: frames "); out_dec(JOIN_AT - 5); out("-774: "); out_dec((DWORD)n); out(" present-to-present intervals, ");
    out_dec((DWORD)off); out(" off 16.667 ms by > 0.1 ms, "); out_dec((DWORD)far); out(" by > 1 ms; worst "); out_us(worst); out(" (frame "); out_dec((DWORD)worst_i); out(")\r\n");
    check_p(n >= 400 && off * 50 <= n && far == 0, "spacing", "from the join through the slide and the step-aside: presents keep the 60 FPS spacing (>= 98 % within 0.1 ms, none off by 1 ms: no hurried or doubled frame)");
    if (last_on > JOIN_AT && last_on + 5 < 775)
        check_hold_from_return("hold", last_on + 5, 775, hold, "after stepping aside: presents at the limiter's release + 4 ms +- 0.2 (>= 95 %), never early");
    /* The limiter caught up after each late frame (kept its grid: three frame starts later it is back on it), so the
       windows after them really were short; the step-aside line count (build.ps1) shows FrameCap didn't come back. */
    int kept = 0;
    for (int i = 780; i < nframes - 3; i++) {
        if (!join_heavy_work10(i)) continue;
        double three = (double)(start_t[i + 3] - start_t[i]) * us_per_tick, next = (double)(start_t[i + 2] - start_t[i + 1]) * us_per_tick;
        out("      "); out(sc_name); out("late frame "); out_dec((DWORD)i); out(" ("); out_ms(join_heavy_work10(i) * 100.0); out(" of work): the next frame lasted ");
        out_ms(next); out(", three frame starts span "); out_ms(three); out("\r\n");
        if (dabs(three - 3 * grid_us) <= 300.0 && next < grid_us - 3000.0) kept++;
    }
    check_p(kept == JOIN_HEAVY_FRAMES, "catch-up", "the fake limiter caught up after each late frame (the next frame shorter, three frame starts later back on its grid)");
}

/* ---- the display changes mid-run (SC_RECHECK: FrameCap=60, PresentHold=4, HoldFlush=auto, RefreshHz=280, Recheck=1) ----
   Frame RECHECK_SWAP: the game presents through another swap chain, and the display runs at 60 Hz (VSync = the fake
   limiter): VinceFix must re-check at once and turn the cap and the hold off. RECHECK_MOVE: the same swap chain, its window
   now on the other (fake) monitor at 280 Hz: found by the next check (within 60 frames), both back on. RECHECK_REUSE: a new
   swap chain at the same address (another size, on another render adapter): found within 60 frames, both stay on. */
static void change_display(int i) {
    if (i == RECHECK_SWAP) { renderer.swap_chain = &swapchain2; fake_refresh_hz = 60; }
    if (i == RECHECK_MOVE) { fake_monitor = 2; fake_refresh_hz = 280; }
    if (i == RECHECK_REUSE) { desc_width = 2560; desc_height = 1440; render_luid = 0x9ABC; }
}
/* The first frame from `first` on with two GetDesc calls (the check that found the change, and the report after it). */
static int found_at(int first) { for (int i = first; i < nframes; i++) if (desc_n[i] == 2) return i; return -1; }

static void check_recheck(double hold) {
    int most_desc = 0, most_monitor = 0, moved = found_at(RECHECK_MOVE), reused = found_at(RECHECK_REUSE), off_flush = 0, held = 0, unflushed = 0;
    for (int i = 0; i < nframes; i++) {
        if (desc_n[i] > most_desc) most_desc = desc_n[i];
        if (monitor_n[i] > most_monitor) most_monitor = monitor_n[i];
    }
    DWORD desc_most = 1 + (DWORD)nframes / DISPLAY_CHECK_FRAMES + 3; /* the first frame, one check per 60 frames, a report per change */
    out("      "); out(sc_name); out("display checks: "); out_dec(desc_calls); out(" GetDesc calls (at most "); out_dec((DWORD)most_desc); out(" in a frame), ");
    out_dec(monitor_calls); out(" MonitorFromWindow calls (at most "); out_dec((DWORD)most_monitor); out(" in a frame), "); out_dec(monitor_bad_args);
    out(" with unexpected arguments; at the new swap chain (frame "); out_dec(RECHECK_SWAP); out(") "); out_dec(desc_n[RECHECK_SWAP]);
    out(" GetDesc call(s); the monitor move found at frame "); out_dec((DWORD)moved); out(", the reused address at frame "); out_dec((DWORD)reused); out("\r\n");
    check_p(desc_calls <= desc_most && monitor_calls <= desc_calls && most_desc <= 2 && most_monitor <= 2 && monitor_bad_args == 0 && desc_n[RECHECK_SWAP] == 1,
            "calls", "the display checks stay cheap: GetDesc and MonitorFromWindow about once every 60 frames, at most twice in a frame (a check and the report after it)");
    check_p(moved > RECHECK_MOVE && moved <= RECHECK_MOVE + DISPLAY_CHECK_FRAMES && reused > RECHECK_REUSE && reused <= RECHECK_REUSE + DISPLAY_CHECK_FRAMES,
            "found", "a monitor move and a new swap chain at the old address are each found by the next check (within 60 frames)");
    if (moved < 0 || reused < 0) return;
    /* Each re-check's lines (why, display:, gpu:, the rules) come after that frame's real Present: VinceFix.log is the
       same size at its Present entry as after the frame before, and grew by the end of the frame. */
    int found[3] = {RECHECK_SWAP, moved, reused}, after = 0;
    for (int k = 0; k < 3; k++) {
        int f = found[k];
        out("      "); out(sc_name); out("log: frame "); out_dec((DWORD)f); out(": VinceFix.log "); out_dec(log_len[f - 1]); out(" bytes after the frame before, ");
        out_dec(log_at_entry[f]); out(" at its Present entry, "); out_dec(log_len[f]); out(" after it\r\n");
        if (log_at_entry[f] == log_len[f - 1] && log_len[f] > log_at_entry[f]) after++;
    }
    check_p(after == 3, "log", "each re-check's lines are written after that frame's real Present, so they don't delay it");
    /* HoldFlush=auto flushes only while FrameCap runs: none from the new swap chain (60 Hz) until the move was found, one per
       held frame on the 280 Hz display (not in the frame each change was found in: it starts afresh). */
    for (int i = RECHECK_SWAP; i <= moved; i++) off_flush += flush_n[i];
    for (int i = moved + 2; i < nframes; i++) {
        if (i == reused || i == reused + 1 || preempted[i] || long_flush(i) || scen_work10(i) > 35) continue;
        held++;
        if (flush_n[i] != 1) unflushed++;
    }
    out_flush_counts();
    out("      "); out(sc_name); out("flush: "); out_dec((DWORD)off_flush); out(" Flush calls at 60 Hz; "); out_dec((DWORD)unflushed); out(" of ");
    out_dec((DWORD)held); out(" held frames at 280 Hz without exactly one\r\n");
    check_p(off_flush == 0 && held >= 400 && unflushed * 100 <= held, "flush", "no Flush while the 60 Hz display turned FrameCap off, one per held frame after the move to 280 Hz");
    check(getdevice_calls == flush_calls + 4 && getcontext_calls == flush_calls && bad_iid == 0 && device_refs == 0 && context_refs == 0 &&
              bad_order == 0 && device_bad_slots == 0 && context_bad_slots == 0,
          "recheck: flush refs: GetDevice, GetImmediateContext and Flush once each per flushed frame (GetDevice once more for each of the 4 display reports), all released");

    grid_us = 1000000.0 / 60.0;
    check_grid("1 start", 1, RECHECK_SWAP, "280 Hz, first swap chain: frame starts on the 60 FPS grid");
    check_hold("1 hold", 0, RECHECK_SWAP, 35, hold, 200.0, "280 Hz, first swap chain: presents at start + 4 ms +- 0.2 for >= 95 %");
    check_nohold("2 present", RECHECK_SWAP + 1, RECHECK_MOVE, "the new swap chain on the 60 Hz display: no hold, Present entry <= 0.3 ms after the work for >= 95 % (FrameCap and the hold off)");
    check_grid("3 start", moved + 5, RECHECK_REUSE, "after the move to the 280 Hz monitor: FrameCap back on, frame starts on the 60 FPS grid");
    check_hold("3 hold", moved + 5, RECHECK_REUSE, 35, hold, 200.0, "after the move to the 280 Hz monitor: the hold back on, presents at start + 4 ms +- 0.2");
    check_grid("4 start", reused + 5, nframes, "after the new swap chain at the old address: frame starts on the 60 FPS grid again");
    check_hold("4 hold", reused + 5, nframes, 35, hold, 200.0, "after the new swap chain at the old address: presents at start + 4 ms +- 0.2");
}

static void print_trace(void) { /* median measured hold of the held frames per 100 frames: the hold's course through the phases */
    out("      "); out(sc_name); out("trace, median entry - start of the held frames per 100 frames (ms/held frames; - = none held):");
    for (int b = 0; b * 100 < nframes; b++) {
        int m = 0;
        for (int i = b * 100; i < b * 100 + 100 && i < nframes; i++) if (lag_us[i] > HELD_LAG_US) scratch[m++] = hold_us[i];
        sort(scratch, m);
        if (b % 12 == 0) out("\r\n        ");
        out_dec((DWORD)(b * 100)); out(":");
        if (m) out_num2(pct(scratch, m, 50)); else out("-");
        out("/"); out_dec((DWORD)m); /* how many were held: a bin of one or two is a late frame, not a hold */
        out(" ");
    }
    out("\r\n");
}

static int path_next_to_exe(const WCHAR *name, WCHAR *path) { /* this exe's folder + name; 0 if it doesn't fit */
    DWORD n = GetModuleFileNameW(0, path, 520), m = 0;
    while (name[m]) m++;
    while (n && path[n - 1] != L'\\') n--;
    if (!n || n + m + 1 > 520) { path[0] = 0; return 0; }
    for (DWORD i = 0; i <= m; i++) path[n + i] = name[i];
    return 1;
}
static int ini_value(const WCHAR *section, const WCHAR *key, WCHAR *value) { /* key of the VinceFix.ini next to this exe (the file the DLL read at load); 0 if unset */
    WCHAR path[520];
    value[0] = 0;
    if (!path_next_to_exe(L"VinceFix.ini", path)) return 0;
    GetPrivateProfileStringW(section, key, L"", value, 32, path);
    return value[0] != 0;
}
static int ini_is_auto(const WCHAR *s) { /* "auto" in any case, then blanks or a ; comment */
    const char *word = "auto";
    while (*s == L' ' || *s == L'\t') s++;
    for (; *word; word++, s++) if ((*s | 0x20) != *word) return 0;
    while (*s == L' ' || *s == L'\t') s++;
    return !*s || *s == L';';
}
static int ini_flush_mode(void) { /* HoldFlush as VinceFix reads it: 1 on, 0 off (0 or empty), 2 auto (auto, no line, anything else) */
    WCHAR path[520], value[32];
    if (!path_next_to_exe(L"VinceFix.ini", path)) return 2;
    GetPrivateProfileStringW(L"VinceFix", L"HoldFlush", L"<none>", value, 32, path);
    if (value[0] == L'1' && (!value[1] || value[1] == L' ')) return 1;
    if (!value[0] || (value[0] == L'0' && (!value[1] || value[1] == L' '))) return 0;
    return 2;
}
static int ini_number(const WCHAR *s) { /* whole part of a decimal number, -1 if there is none */
    int v = 0, digits = 0;
    while (*s == L' ' || *s == L'\t') s++;
    for (; *s >= L'0' && *s <= L'9' && digits < 6; s++, digits++) v = v * 10 + (*s - L'0');
    return digits ? v : -1;
}

__attribute__((noreturn)) static void run_scenario(int which, int fps, int hold_ms) {
    static const char *const names[11] = {"auto ", "cap+hold ", "cap ", "cap+auto ", "off ", "cap+aside ", "cap+gpu ", "live ", "cap+join ", "recheck ", "cap+edge "};
    scen = which;
    sc_name = names[which];
    burst_at = -1000; b_first = b_last = 0; ph_len = 100000; /* cap scenarios: one phase, no burst */
    DWORD f32 = (DWORD)freq, bad_calls = 0;
    /* auto with HoldFlush: phases A and B only (they show the Flush counted as work; C and D don't involve the Flush) */
    if (which == SC_AUTO) { nframes = flush_run ? 1800 : 3600; ph_len = 900; b_first = 900; b_last = 1800; burst_at = 450; grid_us = (double)period * us_per_tick; }
    else if (which == SC_CAP_AUTO) { nframes = 2400; ph_len = 600; b_first = 600; b_last = 1200; grid_us = 1000000.0 / (double)fps; }
    else if (which == SC_OFF) { nframes = 300; grid_us = (double)period * us_per_tick; }
    else if (which == SC_CAP_ASIDE || which == SC_CAP_GPU) { nframes = 1200; ph_len = 600; grid_us = 1000000.0 / (double)fps; }
    else if (which == SC_CAP_JOIN) { nframes = 1270; grid_us = 1000000.0 / (double)fps; } /* to the window after 1200's */
    else if (which == SC_RECHECK) { nframes = 1200; grid_us = 1000000.0 / (double)fps; }
    else if (which == SC_CAP_EDGE) { nframes = 900; grid_us = 1000000.0 / (double)fps; }
    else { nframes = 660; grid_us = 1000000.0 / (double)fps; }
    if (which == SC_CAP_GPU) limit_period = (long long)(f32 / GPU_FPS);
    if (which == SC_CAP_JOIN) { limiter_catches_up = 1; join_offset = (long long)(f32 / 1000 * JOIN_OFFSET_US / 1000); }
    watch_entry_log = which == SC_RECHECK;
    if (which == SC_RECHECK) check(fake_the_monitors(), "recheck: X3DAudio1_7.dll's MonitorFromWindow and GetMonitorInfoW imports found and pointed at the fake monitors");
    auto_shift_us = flush_run ? (double)flush_us : 0.0;
    out("      "); out(sc_name); out("scenario: "); out_dec((DWORD)nframes); out(" frames");
    if (which == SC_OFF) out(", the fake limiter on; VinceFix should turn the hold and the cap off at the first frame");
    else if (which == SC_CAP_ASIDE) { out(", FrameCap "); out_dec((DWORD)fps); out(" FPS with the fake 60 FPS limiter in Present for frames 0-599, none after"); }
    else if (which == SC_CAP_GPU) {
        out(", FrameCap "); out_dec((DWORD)fps); out(" FPS; frames 0-599: a \"GPU-bound\" Present that allows "); out_dec(GPU_FPS);
        out(" FPS; frames 600-1199: Present takes "); out_ms(BUSY_PRESENT_US); out(" each, no limiter");
    }
    else if (which == SC_CAP_JOIN) {
        out(", FrameCap "); out_dec((DWORD)fps); out(" FPS; from frame "); out_dec(JOIN_AT); out(" a fake 60 FPS limiter that catches up after late frames, its grid ");
        out_ms(JOIN_OFFSET_US); out(" after that frame's Present entry; late frames (25, 30, 25, 33, 33 ms of work) at 780, 900, 1050, 1080, 1200");
    }
    else if (which == SC_CAP_EDGE) {
        out(", FrameCap "); out_dec((DWORD)fps); out(" FPS; work 2 ms and a Present that takes "); out_ms(EDGE_BUSY_US);
        out(" (a GPU that allows 61.5 FPS uncapped), no limiter");
    }
    else if (which == SC_RECHECK) {
        out(", FrameCap "); out_dec((DWORD)fps); out(" FPS; 280 Hz, then from frame "); out_dec(RECHECK_SWAP); out(" a second swap chain on a 60 Hz display (fake limiter on), from ");
        out_dec(RECHECK_MOVE); out(" its window on the other (280 Hz) monitor, from "); out_dec(RECHECK_REUSE); out(" a new swap chain at its address (2560x1440, another adapter)");
    }
    else if (which != SC_AUTO) { out(", FrameCap "); out_dec((DWORD)fps); out(" FPS, fake Present without a limiter"); }
    if (which == SC_CAP_HOLD || which == SC_CAP_ASIDE || which == SC_CAP_JOIN || which == SC_RECHECK) { out(", PresentHold "); out_dec((DWORD)hold_ms); out(" ms"); }
    if (flush_run) {
        out(", held frames flushed (Flush "); out_ms(flush_us); out(", 3.5 ms at 50, 150 ..");
        out(which == SC_CAP_HOLD ? "; 6 ms of work at 75, 175 ..)" : "; auto targets expected higher by the Flush time)");
    }
    out(", about "); out_dec((DWORD)(nframes * grid_us / 1000000.0 * (which == SC_AUTO ? 0.8 : 1.0) + 0.5)); out(" s (one core spins)\r\n");

    long long t0, now;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < nframes; i++) {
        frame_now = i;
        if (which == SC_RECHECK) change_display(i);
        limiter_on = scen_limiter(i);
        present_busy = which == SC_CAP_GPU && i >= ph_len ? (long long)(f32 / 1000 * BUSY_PRESENT_US / 1000)
                       : which == SC_CAP_EDGE              ? (long long)(f32 / 1000 * EDGE_BUSY_US / 1000) : 0;
        start_t[i] = t0;
        if (i == 0) log_before_first = log_size(); /* inside this frame's work time, so not timed */
        work_t[i] = (long long)(f32 / 1000 * scen_work10(i) / 10);
        long long end = t0 + work_t[i];
        do QueryPerformanceCounter(&now); while (now < end);
        preempted[i] = now - end > (long long)(f32 / 3333);
        HRESULT r = game_present(&renderer);
        QueryPerformanceCounter(&t0); /* next frame starts right after Present returned */
        log_len[i] = log_size();      /* inside the next frame's work time */
        if (i == 0) log_after_first = log_len[0];
        if (r != PRESENT_HRESULT) bad_ret++;
        if (calls != (DWORD)i + 1) bad_calls++;
    }
    int npre = 0, nwrites = 0;
    mark_log_writes(nframes);
    for (int i = 0; i < nframes; i++) { npre += preempted[i]; nwrites += follows_write[i]; }
    out("      "); out(sc_name); out("frames whose work loop Windows paused for > 0.3 ms (left out of the timing checks): "); out_dec((DWORD)npre);
    out("; frames that started after a VinceFix.log write (also left out): "); out_dec((DWORD)nwrites); out("\r\n");
    measure_frames(nframes);
    check_calls(bad_calls);
    check_fake_display();
    check_setup_fakes();
    check_first_frame_log();
    if (!flush_run) check_no_flush();
    else if (getdevice_fails || getcontext_null) check_flush_failed();
    else if (which == SC_CAP_ASIDE) check_aside(hold_ms * 1000.0);
    else if (which == SC_CAP_JOIN) check_join(hold_ms * 1000.0);
    else if (which == SC_RECHECK) check_recheck(hold_ms * 1000.0);
    else {
        for (int i = 0; i < nframes; i++) {
            DWORD w = scen_work10(i);
            if (which == SC_CAP_HOLD) expect_held[i] = (signed char)(i == 0 || w >= 50 ? 0 : w <= 35 ? 1 : -1);
            /* auto: nothing is held before the first target (frame 300) nor in the burst; a frame that waited must have been flushed */
            else expect_held[i] = (signed char)(i < WINDOW || w >= 200 ? 0 : !preempted[i] && lag_us[i] > 300.0 ? 1 : -1);
        }
        check_flush(nframes);
    }

    double s = auto_shift_us;
    if (which == SC_AUTO) {
        check_hold("a", 800, 900, 30, 3700.0 + s, 300.0, "end of phase A (frames 800-899): hold 3.7 ms (+ the Flush time if flushed) +- 0.3 for >= 95 % of the frames with work <= 3.0 ms");
        check_burst("b", "neither the 40 ms loading burst nor four scattered 6 ms frames in one block drive the hold up (no held frame in phase A after the first target holds more than 4.2 ms + the Flush time)");
        check_hold("b2", 1050, 1150, 50, 5800.0 + s, 300.0, "fast raise: early in phase B (frames 1050-1149, before its first window ends) the hold already covers the heavier work (5.8 ms + Flush +- 0.3)");
        check_hold("c", 1700, 1800, 50, 5800.0 + s, 300.0, "end of phase B (frames 1700-1799): hold 5.8 ms (+ Flush) +- 0.3 for >= 95 % of the frames with work <= 5.0 ms");
        if (nframes > 1800) {
            check_nohold("d", 2600, 2700, "end of phase C (limiter off, frames 2600-2699): no hold, Present entry <= 0.3 ms after the work for >= 95 %");
            check_hold("e", 3500, 3600, 30, 3700.0, 300.0, "end of phase D (frames 3500-3599): hold 3.7 ms +- 0.3 for >= 95 % of the frames with work <= 3.0 ms");
        }
        check_smooth("f", 1, nframes, "held presents keep the 16.667 ms spacing while the hold changes (phases A, B, D: |interval - 16.667 ms| <= 0.1 ms, <= 2 % of pairs worse, max < 5 ms)");
        check_early("g", "never early: the measured hold never falls by more than 0.07 ms between consecutive held frames (ramp 0.02 ms per frame)");
        print_trace();
    } else if (which == SC_CAP_HOLD) {
        check_grid("start", 1, nframes, "frame starts sit on the 60 FPS grid (mean 16.667 ms +- 0.01, >= 98 % of intervals within 0.1 ms)");
        check_carry("carry", fps, "FrameCap's carry: over 300+ frames the grid keeps the exact period (fraction of a tick carried; drift <= 5 us)");
        check_hold("hold", 0, nframes, 35, hold_ms * 1000.0, 200.0, "presents at start + the hold (4 ms +- 0.2) for >= 95 % of the frames with work <= 3.5 ms");
        check_entry_sd("entry", 1, nframes, ">= 98 % of present-to-present intervals within 0.1 ms of the grid");
    } else if (which == SC_CAP_ONLY) {
        check_grid("start", 1, nframes, "frame starts sit on the 60 FPS grid (mean 16.667 ms +- 0.01, >= 98 % of intervals within 0.1 ms)");
        check_nohold("present", 3, nframes, "no hold: Present entry <= 0.3 ms after the work for >= 95 % of the frames");
        check_long_frames("late", "a 25 ms frame is followed by an at-once start and then the grid interval again (no catch-up frame)");
    } else if (which == SC_OFF) {
        check_nohold("present", 1, nframes, "nothing runs: every frame reaches Present <= 0.3 ms after its work (>= 95 %), Present passes straight through");
    } else if (which == SC_CAP_ASIDE) {
        /* check_aside (above) checked the switches; these the spacing on either side of them */
        grid_us = (double)limit_period * us_per_tick;
        check_grid("1 start", 1, ph_len, "with the other limiter (frames 0-599, FrameCap running, then aside): frame starts on the limiter's 16.667 ms grid (mean +- 0.01, >= 98 % within 0.1 ms)");
        check_entry_sd("1 entry", 1, ph_len, "with the other limiter: >= 98 % of present-to-present intervals within 0.1 ms of its grid");
        grid_us = 1000000.0 / (double)fps;
        check_grid("2 start", ph_len + 2 * PACE_WINDOW + 5, nframes, "after FrameCap came back (no other limiter): frame starts on the 60 FPS grid (mean +- 0.01, >= 98 % within 0.1 ms)");
        check_hold("2 hold", ph_len + 2 * PACE_WINDOW + 5, nframes, 35, hold_ms * 1000.0, 200.0, "after FrameCap came back: presents at start + 4 ms +- 0.2 for >= 95 % of the frames");
        check_entry_sd("2 entry", ph_len + 2 * PACE_WINDOW + 5, nframes, "after FrameCap came back: >= 98 % of present-to-present intervals within 0.1 ms of the grid");
    } else if (which == SC_CAP_JOIN) {
        check_grid("1 start", 1, JOIN_AT, "FrameCap alone before the other limiter joins: frame starts on the 60 FPS grid (mean +- 0.01, >= 98 % within 0.1 ms)");
        check_hold("1 hold", 0, JOIN_AT, 35, hold_ms * 1000.0, 200.0, "FrameCap alone: presents at start + 4 ms +- 0.2 for >= 95 % of the frames");
    } else if (which == SC_CAP_EDGE) {
        /* FrameCap may step aside once (the window looks like another limiter), but must be back within a few windows and
           stay on: frames that ran uncapped (start interval 0.2 ms or more under the period) only before EDGE_SETTLED, at
           most 3 windows of them; build.ps1 checks at most one step-aside and one back-on line. */
        int uncapped = 0, last_uncapped = 0;
        for (int i = 2; i < nframes; i++)
            if (!preempted[i - 1] && (double)(start_t[i] - start_t[i - 1]) * us_per_tick < grid_us - 200.0) { uncapped++; last_uncapped = i; }
        out("      "); out(sc_name); out("frames that ran uncapped (faster than the cap): "); out_dec((DWORD)uncapped); out(", the last at frame ");
        out_dec((DWORD)last_uncapped); out("\r\n");
        check_p(uncapped <= 3 * PACE_WINDOW && last_uncapped < EDGE_SETTLED, "settles",
                "a GPU just faster than the cap: FrameCap is on for good within 300 frames (at most 3 windows uncapped), no flapping");
        check_grid("2 start", EDGE_SETTLED, nframes, "then capped: frame starts on the 60 FPS grid (mean +- 0.01, >= 98 % within 0.1 ms)");
    } else if (which == SC_RECHECK) {
        /* check_recheck (above) checked each phase */
    } else if (which == SC_CAP_GPU) {
        /* build.ps1 checks that VinceFix.log has no step-aside line */
        grid_us = (double)limit_period * us_per_tick;
        check_grid("1 start", 1, ph_len, "\"GPU-bound\" below the cap (frames 0-599): frames start at the rate Present allows (58 FPS grid, mean +- 0.01, >= 98 % within 0.1 ms)");
        grid_us = 1000000.0 / (double)fps;
        check_grid("2 start", ph_len, nframes, "Present busy 1.5 ms per frame (frames 600-1199): FrameCap still paces, frame starts on the 60 FPS grid (mean +- 0.01, >= 98 % within 0.1 ms)");
        check_nohold("2 present", ph_len, nframes, "Present busy 1.5 ms: no hold, Present entry <= 0.3 ms after the work for >= 95 %");
        /* Frames that wait both in Present and after it make FrameCap slide (50 us shorter periods) for one window; as
           Present's time doesn't follow, the slide must stop: from frame 720 on the rate is exactly the cap's again. */
        double steady = (double)(start_t[nframes - 1] - start_t[720]) * us_per_tick / (nframes - 1 - 720);
        out("      "); out(sc_name); out("2 steady: frames 720-"); out_dec((DWORD)nframes - 1); out(": mean frame-start interval "); out_us(steady); out("\r\n");
        check_p(dabs(steady - grid_us) <= 2.0, "2 steady", "Present busy 1.5 ms: a slide toward a limiter that isn't there stops after one window (from frame 720 the mean interval is the cap's within 2 us)");
    } else {
        check_hold("a", 500, 600, 30, 3700.0 + s, 300.0, "end of the first phase (frames 500-599): hold 3.7 ms (+ the Flush time if flushed) +- 0.3 for >= 95 % of the frames with work <= 3.0 ms");
        check_hold("c", 1100, 1200, 50, 5800.0 + s, 300.0, "end of the 3-5 ms phase (frames 1100-1199): hold 5.8 ms (+ Flush) +- 0.3 for >= 95 % of the frames with work <= 5.0 ms");
        check_hold("d", 1700, 1800, 30, 5800.0 + s, 300.0, "slow lowering: 500 frames into the lighter phase (frames 1700-1799, 2 windows) the hold is still 5.8 ms (+ Flush) +- 0.3");
        check_hold("e", 2300, 2400, 30, 3700.0 + s, 300.0, "end of the last phase (frames 2300-2399, after 3 lighter windows): the hold is back at 3.7 ms (+ Flush) +- 0.3 (a cap is on, so it is not switched off)");
        check_grid("start", 1, nframes, "frame starts sit on the 60 FPS grid (mean 16.667 ms +- 0.01, >= 98 % of intervals within 0.1 ms)");
        check_carry("carry", fps, "FrameCap's carry: over 300+ frames the grid keeps the exact period (fraction of a tick carried; drift <= 5 us)");
        check_smooth("f", 1, nframes, "held presents keep the 16.667 ms spacing while the hold changes (|interval - 16.667 ms| <= 0.1 ms, <= 2 % of pairs worse, max < 5 ms)");
        check_early("g", "never early: the measured hold never falls by more than 0.07 ms between consecutive held frames (ramp 0.02 ms per frame)");
        print_trace();
    }
    out(failures ? "SOME CHECKS FAILED\r\n" : "ALL CHECKS PASSED\r\n");
    ExitProcess((DWORD)failures);
}

/* ---- live reload scenario (LiveReload=1): this host rewrites VinceFix.ini while it runs -------------------------------- */
#define LIVE_START 240      /* frames before the first rewrite */
#define LIVE_PICKUP_MAX 120 /* 2 s: VinceFix checks the file at least once a second and lets an edit settle for 0.2 s */
#define LIVE_SETTLE 5       /* frames after the pickup left out of the checks */
#define LIVE_CHECKED 240    /* frames checked per phase */
#define LIVE_PHASES 4
static const char *const live_ini[LIVE_PHASES] = {
    "[VinceFix]\r\nLiveReload=1\r\nPresentHold=5\r\nFrameCap=0\r\n",
    "[VinceFix]\r\nLiveReload=1\r\nPresentHold=4\r\nFrameCap=60\r\n", /* no HoldFlush line: auto, so held frames are flushed */
    "[VinceFix]\r\nLiveReload=1\r\nPresentHold=0\r\nFrameCap=0\r\n",
    "[VinceFix]\r\nLiveReload=1\r\nPresentHold=0\r\nFrameCap=0\r\n"}; /* saved again unchanged: only the summary line */
static const int live_limiter[LIVE_PHASES] = {1, 0, 1, 1}; /* the fake limiter from each rewrite on: FrameCap paces phase 2 alone */
static char log_text[65536];

#define INVALID_HANDLE ((HANDLE)-1)
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ_WRITE 0x3
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define FILE_ATTRIBUTE_NORMAL 0x80
static HANDLE open_file(const WCHAR *path, DWORD access, DWORD share, DWORD disposition) {
    return CreateFileW(path, access, share, 0, disposition, FILE_ATTRIBUTE_NORMAL, 0);
}

static int write_ini(const char *text) { /* replaces the VinceFix.ini next to this exe */
    WCHAR path[520];
    DWORD n = 0, written = 0;
    while (text[n]) n++;
    if (!path_next_to_exe(L"VinceFix.ini", path)) return 0;
    HANDLE f = open_file(path, GENERIC_WRITE, FILE_SHARE_READ_WRITE, CREATE_ALWAYS);
    if (f == INVALID_HANDLE) return 0;
    BOOL ok = WriteFile(f, text, n, &written, 0) && written == n;
    CloseHandle(f);
    return ok;
}

/* ---- a hostile editor: VinceFix must keep VinceFix.ini open while it reads its keys ----
   For the live scenario X3DAudio1_7.dll's kernel32!GetPrivateProfileStringW import is pointed at contended_read, which
   first tries to take VinceFix.ini for itself (no sharing), as an editor saving the file may, and keeps it during the
   real call. Getting it means nobody else held the file: the real call then can't open it and reads the key as missing,
   and a reload read so would turn everything off. VinceFix holds its own handle during the read, so every try must fail. */
typedef DWORD(WINAPI *ProfileStringFn)(const WCHAR *section, const WCHAR *key, const WCHAR *def, WCHAR *out, DWORD size, const WCHAR *file);
static ProfileStringFn real_profile_string;
static WCHAR contended_path[520];
static volatile long editor_tries, editor_got; /* the watcher thread writes them; read at the end */
static DWORD WINAPI contended_read(const WCHAR *section, const WCHAR *key, const WCHAR *def, WCHAR *out, DWORD size, const WCHAR *file) {
    HANDLE editor = open_file(contended_path, GENERIC_READ | GENERIC_WRITE, 0, OPEN_EXISTING);
    editor_tries++;
    if (editor != INVALID_HANDLE) editor_got++;
    DWORD result = real_profile_string(section, key, def, out, size, file);
    if (editor != INVALID_HANDLE) CloseHandle(editor);
    return result;
}
static int contend_ini_reads(void) {
    void **slot = find_import_slot((const BYTE *)GetModuleHandleW(L"X3DAudio1_7.dll"), "kernel32.dll", "GetPrivateProfileStringW");
    DWORD protect;
    if (!path_next_to_exe(L"VinceFix.ini", contended_path) || !slot || !VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &protect)) return 0;
    real_profile_string = (ProfileStringFn)*slot;
    *slot = (void *)contended_read;
    VirtualProtect(slot, sizeof *slot, protect, &protect);
    return 1;
}

static int count_in_log(const char *key) { /* occurrences of key in VinceFix.log */
    DWORD got = 0;
    HANDLE f = open_file(log_path, GENERIC_READ, FILE_SHARE_READ_WRITE, OPEN_EXISTING);
    if (f == INVALID_HANDLE) return 0;
    ReadFile(f, log_text, sizeof log_text, &got, 0);
    CloseHandle(f);
    int count = 0;
    for (DWORD i = 0; i < got; i++) {
        int k = 0;
        while (key[k] && i + k < got && log_text[i + k] == key[k]) k++;
        if (!key[k]) count++;
    }
    return count;
}
/* Never early: the measured hold of frames first..last-1 never falls more than 0.05 ms below the fixed hold. */
static void check_not_early(const char *tag, int first, int last, double hold, const char *what) {
    int n = 0, lowest_i = first;
    double lowest = 1e9;
    for (int i = first; i < last; i++) if (!skip_frame(i)) { n++; if (hold_us[i] < lowest) { lowest = hold_us[i]; lowest_i = i; } }
    out("      "); out(sc_name); out(tag); out(": "); out_dec((DWORD)n); out(" frames, shortest measured hold "); out_ms(lowest); out(" (frame ");
    out_dec((DWORD)lowest_i); out(")\r\n");
    check_p(n >= 100 && lowest >= hold - 50.0, tag, what);
}

__attribute__((noreturn)) static void run_live(void) {
    int wrote[LIVE_PHASES], seen[LIVE_PHASES], phase = 0, waiting = 0, reloads = 0, extra_lines = 0, write_failed = 0;
    int next_write = LIVE_START, end = NMAX, i;
    scen = SC_LIVE;
    sc_name = "live ";
    burst_at = -1000; b_first = b_last = 0; ph_len = 100000;
    out("      live scenario: LiveReload=1, starting with PresentHold=0 and FrameCap=0 (fake limiter on); VinceFix.ini rewritten at frame ");
    out_dec(LIVE_START); out(" to PresentHold=5, then to PresentHold=4 + FrameCap=60 (fake limiter off), then to 0 / 0 (limiter on), then saved again "
                             "unchanged, each ");
    out_dec(LIVE_SETTLE + LIVE_CHECKED); out(" frames after VinceFix logged the previous one; every key read races a hostile editor; about 25 s (one core spins)\r\n");
    check(contend_ini_reads(), "live reload: X3DAudio1_7.dll's GetPrivateProfileStringW import found and pointed at the hostile editor");
    long long t0, now;
    DWORD f32 = (DWORD)freq, bad_calls = 0;
    QueryPerformanceCounter(&t0);
    for (i = 0; i < end; i++) {
        frame_now = i;
        start_t[i] = t0;
        if (i == 0) log_before_first = log_size();
        /* Inside this frame's work time, so not timed: look for VinceFix's line, then write the next ini when it is due. */
        if (waiting) {
            int lines = count_in_log("settings reloaded: ");
            int late = i - wrote[phase] > LIVE_PICKUP_MAX;
            if (lines != reloads || late) {
                if (lines > reloads + 1) extra_lines += lines - reloads - 1;
                seen[phase] = lines != reloads ? i : -1;
                reloads = lines;
                waiting = 0;
                next_write = i + LIVE_SETTLE + LIVE_CHECKED;
                if (++phase == LIVE_PHASES) end = next_write;
            }
        }
        if (!waiting && phase < LIVE_PHASES && i == next_write) {
            if (!write_ini(live_ini[phase])) write_failed++;
            wrote[phase] = i;
            waiting = 1;
            limiter_on = live_limiter[phase];
        }
        work_t[i] = (long long)(f32 / 1000 * scen_work10(i) / 10);
        long long work_end = t0 + work_t[i];
        do QueryPerformanceCounter(&now); while (now < work_end);
        preempted[i] = now - work_end > (long long)(f32 / 3333);
        HRESULT r = game_present(&renderer);
        QueryPerformanceCounter(&t0); /* next frame starts right after Present returned */
        if (i == 0) log_after_first = log_size();
        if (r != PRESENT_HRESULT) bad_ret++;
        if (calls != (DWORD)i + 1) bad_calls++;
    }
    nframes = i;
    int npre = 0;
    for (i = 0; i < nframes; i++) npre += preempted[i];
    out("      live frames whose work loop Windows paused for > 0.3 ms (left out of the timing checks): "); out_dec((DWORD)npre); out("\r\n");
    measure_frames(nframes);
    check_calls(bad_calls);
    check_setup_fakes();
    check_first_frame_log();

    /* A phase whose line never came is checked from where it should have been at the latest (and fails the pickup check). */
    int picked_up = write_failed == 0 && phase == LIVE_PHASES;
    for (int k = 0; k < phase; k++) {
        out("      live rewrite "); out_dec((DWORD)k + 1); out(": written in frame "); out_dec((DWORD)wrote[k]); out(", ");
        if (seen[k] >= 0) { out("logged by frame "); out_dec((DWORD)seen[k]); out(" ("); out_dec((DWORD)(seen[k] - wrote[k])); out(" frames)\r\n"); }
        else { out("no \"settings reloaded:\" line within "); out_dec(LIVE_PICKUP_MAX); out(" frames\r\n"); seen[k] = wrote[k] + LIVE_PICKUP_MAX; picked_up = 0; }
    }
    check(picked_up, "live reload: each rewrite of VinceFix.ini was picked up and logged within 2 s (120 frames)");
    int lines = count_in_log("settings reloaded: ");
    out("      live: "); out_dec((DWORD)lines); out(" \"settings reloaded:\" lines in VinceFix.log at the end, "); out_dec((DWORD)extra_lines); out(" more than one at a time\r\n");
    check(lines == LIVE_PHASES && extra_lines == 0, "live reload: exactly one \"settings reloaded:\" line per rewrite");
    if (phase < LIVE_PHASES) { out("SOME CHECKS FAILED\r\n"); ExitProcess((DWORD)failures + 1); }

    int first[LIVE_PHASES], last[LIVE_PHASES];
    for (int k = 0; k < LIVE_PHASES; k++) { first[k] = seen[k] + LIVE_SETTLE; last[k] = first[k] + LIVE_CHECKED; }
    grid_us = (double)period * us_per_tick; /* the fake limiter */
    check_nohold("0", 6, wrote[0], "before the first rewrite (PresentHold=0, FrameCap=0): Present passes straight through, entry <= 0.3 ms after the work for >= 95 %");
    check_hold("1 hold", first[0], last[0], 35, 5000.0, 200.0, "after rewrite 1 (PresentHold=5): presents at start + 5 ms +- 0.2 for >= 95 % of the frames");
    check_not_early("1 early", first[0], last[0], 5000.0, "after rewrite 1: never early (no frame presents more than 0.05 ms before start + 5 ms)");
    check_smooth("1 smooth", first[0], last[0], "after rewrite 1: held presents keep the limiter's 16.667 ms spacing (<= 2 % of pairs off by > 0.1 ms, max < 5 ms)");
    grid_us = 1000000.0 / 60.0; /* FrameCap=60 */
    check_grid("2 start", first[1], last[1], "after rewrite 2 (FrameCap=60, PresentHold=4, no fake limiter): frame starts on the 60 FPS grid (mean +- 0.01 ms, >= 98 % within 0.1 ms)");
    check_hold("2 hold", first[1], last[1], 35, 4000.0, 200.0, "after rewrite 2: presents at start + 4 ms +- 0.2 for >= 95 % of the frames");
    check_entry_sd("2 entry", first[1], last[1], "after rewrite 2: >= 98 % of present-to-present intervals within 0.1 ms of the grid");
    check_nohold("3", first[2], last[2], "after rewrite 3 (PresentHold=0, FrameCap=0): Present passes straight through again, entry <= 0.3 ms after the work for >= 95 %");
    check_nohold("4", first[3], last[3], "after saving it again unchanged: Present still passes straight through, entry <= 0.3 ms after the work for >= 95 %");
    out("      live: the hostile editor tried to take VinceFix.ini "); out_dec((DWORD)editor_tries); out(" times while VinceFix read it, and got it ");
    out_dec((DWORD)editor_got); out(" times\r\n");
    check(editor_tries >= LIVE_PHASES * 4 && editor_got == 0,
          "live reload: VinceFix keeps VinceFix.ini open while it reads the keys, so an editor can't take it for itself in between (each read would be \"missing\")");

    /* HoldFlush=auto flushes only while FrameCap runs: none before rewrite 2 was written, none once rewrite 3 was logged (the
       render thread takes a published change at its next frame, and the line comes after the publishing), one per held frame
       in between. */
    DWORD outside = 0, flushed_n = 0, not_one = 0;
    for (i = 0; i < nframes; i++) if (i < wrote[1] || i >= seen[2]) outside += flush_n[i];
    for (i = first[1]; i < last[1]; i++) if (!preempted[i]) { flushed_n++; if (flush_n[i] != 1) not_one++; }
    out_flush_counts();
    out("      live flush: "); out_dec(outside); out(" Flush calls outside the FrameCap phase; "); out_dec(not_one); out(" of its "); out_dec(flushed_n);
    out(" checked frames without exactly one\r\n");
    check(outside == 0 && flushed_n >= 200 && not_one * 100 <= flushed_n,
          "live reload: held frames are flushed only while FrameCap runs (HoldFlush=auto), one Flush each (>= 99 %)");
    check(getdevice_calls == flush_calls + 1 && getcontext_calls == flush_calls && bad_iid == 0 && device_refs == 0 && context_refs == 0 &&
              bad_order == 0 && device_bad_slots == 0 && context_bad_slots == 0,
          "live reload: flush refs: GetDevice, GetImmediateContext and Flush once each per flushed frame (GetDevice once more for the setup report), all released");
    out(failures ? "SOME CHECKS FAILED\r\n" : "ALL CHECKS PASSED\r\n");
    ExitProcess((DWORD)failures);
}

/* The fixed scenario (PresentHold=5, fake limiter on): checks c-f from the top of the file, then HoldFlush's when it runs. */
__attribute__((noreturn)) static void run_fixed(void) {
    DWORD f32 = (DWORD)freq;
    if (flush_run) {
        out(getdevice_fails   ? "      fixed scenario, HoldFlush=1 with a fake GetDevice that fails (E_NOINTERFACE)\r\n"
            : getcontext_null ? "      fixed scenario, HoldFlush=1 with a fake device that has no immediate context\r\n"
                              : "      fixed scenario, HoldFlush=1 (fake Flush 0.2 ms, 3.5 ms in frames 50, 150 .. 650)\r\n");
    }

    long long t0, now;
    DWORD bad_calls = 0;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < NF; i++) {
        frame_now = i;
        start_t[i] = t0;
        if (i == 0) log_before_first = log_size(); /* inside this frame's work time, so not timed */
        if (watch_log && i == 1) log_at_start = log_size(); /* inside this frame's work time, so not timed */
        long long end = t0 + work_t[i];
        do QueryPerformanceCounter(&now); while (now < end);
        preempted[i] = now - end > (long long)(f32 / 3333); /* used by the HoldFlush checks only */
        HRESULT r = game_present(&renderer);
        QueryPerformanceCounter(&t0); /* next frame starts right after Present returned */
        if (i == 0) log_after_first = log_size();
        if (watch_log && i == 1) log_after = log_size();
        if (r != PRESENT_HRESULT) bad_ret++;
        if (calls != (DWORD)i + 1) bad_calls++;
    }

    out("      "); out_dec(calls); out(" Present calls, "); out_dec(bad_args); out(" with wrong args, "); out_dec(bad_ret); out(" wrong return values, "); out_dec(bad_calls);
    int desc_off = frames_off_desc_rhythm(NF);
    out(" frames with a wrong call count, "); out_dec(desc_calls); out(" GetDesc call(s), "); out_dec((DWORD)desc_off); out(" frames off the once-per-60-frames rhythm\r\n");
    check(calls == NF && bad_args == 0 && bad_ret == 0 && bad_calls == 0 && desc_off == 0,
          "fake Present: one call per frame, args (fake swap chain, 1, 0), 0x087A0001 returned to the caller, GetDesc at the first frame (display check) and then once every 60 frames, never more");
    check_fake_display();
    check_setup_fakes();
    check_first_frame_log();

    int nc = 0, nd = 0, ne = 0, min_i = 0, max_i = 0; /* frame numbers of the earliest and latest held Present */
    double min_d = 0, max_d = 0;
    for (int i = 1; i < NF; i++) { /* HoldFlush runs: frames with a 3.5 ms Flush are late on purpose (checked by flush late) */
        if (HELD(i) && !after_log_write(i) && !long_flush(i)) {
            double d = (double)(entry_t[i] - start_t[i]) * us_per_tick - 5000.0;
            if (!nc || d < min_d) { min_d = d; min_i = i; }
            if (!nc || d > max_d) { max_d = d; max_i = i; }
            cdel[nc++] = d;
        }
        else if (work10[i & 7] >= 53) dlag[nd++] = (double)(entry_t[i] - (start_t[i] + work_t[i])) * us_per_tick;
        if (i >= 2 && HELD(i) && HELD(i - 1) && !long_flush(i) && !long_flush(i - 1)) ivl[ne++] = (double)(entry_t[i] - entry_t[i - 1]) * us_per_tick;
    }
    sort(cdel, nc); sort(dlag, nd);

    out("      c: "); out_dec((DWORD)nc); out(" held frames, entry - start - 5 ms: min "); out_us(cdel[0]); out(", p50 "); out_us(pct(cdel, nc, 50));
    out(", p95 "); out_us(pct(cdel, nc, 95)); out(", max "); out_us(cdel[nc ? nc - 1 : 0]);
    out(" (earliest: frame "); out_dec((DWORD)min_i); out(", latest: frame "); out_dec((DWORD)max_i); out(")\r\n");
    int shown = 0;
    for (int i = 1; i < NF && shown < 5; i++) { /* diagnostics: the first few early frames */
        double d = (double)(entry_t[i] - start_t[i]) * us_per_tick - 5000.0;
        if (HELD(i) && !after_log_write(i) && !long_flush(i) && d < -50.0) { out(shown++ ? ", " : "         early (< -50 us): "); out("frame "); out_dec((DWORD)i); out(" "); out_us(d); }
    }
    if (shown) out("\r\n");
    out("         (frame "); out_dec(STATS_FRAME); out(" not counted: "); out_us((double)(entry_t[STATS_FRAME] - start_t[STATS_FRAME]) * us_per_tick - 5000.0);
    out("; its start follows the stats line's file write)\r\n");
    /* The max allows a rare late timer wake on a busy PC (5.7 ms seen once with OBS running); a missed timer used to cost 100 ms. */
    check(nc > 0 && cdel[0] >= -50.0 && count_le(cdel, nc, 200.0) * 100 >= nc * 95 && count_le(cdel, nc, 2000.0) * 100 >= nc * 99 && cdel[nc - 1] <= 10000.0,
          "hold: work <= 4.7 ms frames reach Present 5 ms after the frame start (never early, <= 0.2 ms late for 95 %, <= 2 ms for 99 %, max <= 10 ms)");

    out("      d: "); out_dec((DWORD)nd); out(" long frames, entry - (start + work): min "); out_us(dlag[0]); out(", p50 "); out_us(pct(dlag, nd, 50));
    out(", p95 "); out_us(pct(dlag, nd, 95)); out(", max "); out_us(dlag[nd ? nd - 1 : 0]); out("\r\n");
    check(nd > 0 && count_le(dlag, nd, 300.0) * 100 >= nd * 95, "no extra wait: 7 ms frames reach Present <= 0.3 ms after their work for 95 %");

    double mean = 0, var = 0;
    for (int i = 0; i < ne; i++) mean += ivl[i];
    if (ne) mean /= ne;
    for (int i = 0; i < ne; i++) var += (ivl[i] - mean) * (ivl[i] - mean);
    if (ne) var /= ne;
    double sd = dsqrt(var);
    out("      e: "); out_dec((DWORD)ne); out(" intervals between consecutive held presents: mean "); out_us(mean); out(", std dev "); out_us(sd); out("\r\n");
    int off = 0;
    for (int i = 0; i < ne; i++) if (dabs(ivl[i] - 16666.7) > 100.0) off++;
    out("         "); out_dec((DWORD)off); out(" of them off 16.667 ms by > 0.1 ms\r\n");
    check(ne > 0 && off * 50 <= ne, "steady: >= 98 % of the intervals between consecutive held presents are within 0.1 ms of 16.667 ms");

    double lag0 = (double)(entry_t[0] - (start_t[0] + work_t[0])) * us_per_tick;
    out("      f: first frame: entry - (start + work) = "); out_us(lag0); out("\r\n");
    check(lag0 <= 2000.0, "first frame is not delayed (no previous Present)");

    if (!flush_run) check_no_flush();
    else if (getdevice_fails || getcontext_null) check_flush_failed();
    else {
        measure_frames(NF);
        for (int i = 0; i < NF; i++) expect_held[i] = (signed char)(HELD(i) ? 1 : 0); /* not held: the first frame and the 7 ms ones */
        check_flush(NF);
    }

    out(failures ? "SOME CHECKS FAILED\r\n" : "ALL CHECKS PASSED\r\n");
    ExitProcess((DWORD)failures);
}

void WINAPI start(void) {
    static BYTE x3d[20];
    X3DAudioInitialize(0x3, 343.5f, x3d); /* keeps the X3DAudio1_7 import alive */

    QueryPerformanceFrequency(&freq);
    DWORD f32 = (DWORD)freq;
    us_per_tick = 1000000.0 / (double)freq;
    period = (long long)((f32 / 1000) * 16667 / 1000); /* 16.667 ms */
    for (int i = 0; i < NF; i++) work_t[i] = (long long)(f32 * work10[i & 7] / 10000);

    swapchain.vtbl = swapchain2.vtbl = vtable;
    for (int i = 0; i < SWAP_CHAIN_SLOTS; i++) vtable[i] = (void *)bad_slot;
    limit_period = period;
    vtable[SLOT_GET_DEVICE] = (void *)fake_get_device;
    vtable[SLOT_PRESENT] = (void *)fake_present;
    vtable[SLOT_SWAP_CHAIN_GET_DESC] = (void *)fake_get_desc;
    vtable[SLOT_GET_CONTAINING_OUTPUT] = (void *)fake_get_containing_output;
    fake_device.vtbl = device_vtable;
    for (int i = 0; i < DEVICE_SLOTS; i++) device_vtable[i] = (void *)device_bad_slot;
    device_vtable[SLOT_QUERY_INTERFACE] = (void *)fake_device_query_interface;
    device_vtable[SLOT_ADD_REF] = (void *)device_add_ref;
    device_vtable[SLOT_RELEASE] = (void *)device_release;
    device_vtable[SLOT_GET_IMMEDIATE_CONTEXT] = (void *)fake_get_context;
    fake_context.vtbl = context_vtable;
    for (int i = 0; i < CONTEXT_SLOTS; i++) context_vtable[i] = (void *)context_bad_slot;
    context_vtable[SLOT_ADD_REF] = (void *)context_add_ref;
    context_vtable[SLOT_RELEASE] = (void *)context_release;
    context_vtable[SLOT_FLUSH] = (void *)fake_flush;
    init_setup_fakes();
    renderer.swap_chain = &swapchain;
    renderer.sync_interval = 1;

    const BYTE *site = site_pattern, *found;
    int matches = count_prefix(&found);
    out("      host: "); out_dec((DWORD)matches); out(" match(es) of the 14-byte prefix, site "); out_hex((ULONG_PTR)site); out(", found "); out_hex((ULONG_PTR)found); out("\r\n");
    check(matches == 1 && found == site, "host: the call-site pattern occurs exactly once in this exe's code, in game_present");

    /* a. the patch (DllMain already ran at process start) */
    const BYTE *dll = (const BYTE *)GetModuleHandleW(L"X3DAudio1_7.dll");
    DWORD dll_size = dll ? *(const DWORD *)(dll + *(const DWORD *)(dll + PE_HEADER_OFFSET) + PE_IMAGE_SIZE) : 0;
    const BYTE *target = site + 20 + *(const int *)(site + 16);
    out("      bytes at +14: "); out_bytes(site + 14, 6); out(", call target "); out_hex((ULONG_PTR)target);
    out(", X3DAudio1_7.dll "); out_hex((ULONG_PTR)dll); out(" size "); out_hex(dll_size); out("\r\n");
    check(site[14] == 0x50 && site[15] == 0xE8 && dll && target >= dll && target < dll + dll_size,
          "patch: bytes at pattern+14 are 50 E8 <rel32> and the call target lies inside X3DAudio1_7.dll");

    /* Other scenarios: the VinceFix.ini next to the exe (written by build.ps1 for the DLL) says which. FrameCap=<fps> runs the cap
       scenarios (fake Present without a limiter, so only VinceFix paces the frames), else PresentHold=auto the auto one. */
    WCHAR hold_text[32], cap_text[32], device_text[32], context_text[32], flush_us_text[32], refresh_text[32], hybrid_text[32], output_text[32];
    ini_value(L"VinceFix", L"PresentHold", hold_text);
    ini_value(L"VinceFix", L"FrameCap", cap_text);
    ini_value(L"VinceFixTest", L"GetDevice", device_text);   /* "fail": the fake GetDevice says E_NOINTERFACE */
    ini_value(L"VinceFixTest", L"GetContext", context_text); /* "null": the fake GetImmediateContext gives no context */
    ini_value(L"VinceFixTest", L"FlushUs", flush_us_text);   /* the fake Flush's time in us (default 200) */
    ini_value(L"VinceFixTest", L"RefreshHz", refresh_text);  /* a fake refresh rate (default: the real display's, assumed > 61 Hz) */
    ini_value(L"VinceFixTest", L"Hybrid", hybrid_text);        /* "1": the display's output belongs to another (Intel) adapter */
    ini_value(L"VinceFixTest", L"Output", output_text);        /* "fail": GetContainingOutput fails */
    getdevice_fails = (device_text[0] | 0x20) == L'f';
    getcontext_null = (context_text[0] | 0x20) == L'n';
    hybrid = ini_number(hybrid_text) == 1;
    output_fails = (output_text[0] | 0x20) == L'f';
    if (ini_number(flush_us_text) > 0) flush_us = (DWORD)ini_number(flush_us_text);
    watch_log = getdevice_fails || getcontext_null;
    path_next_to_exe(L"VinceFix.log", log_path);
    int refresh = ini_number(refresh_text);
    if (refresh >= 0) {
        refresh_faked = fake_the_refresh_rate((DWORD)refresh);
        check(refresh_faked, "fake display: X3DAudio1_7.dll's EnumDisplaySettingsW import found and patched");
    }

    /* What VinceFix should run, by the rules of its decide() (this is the test's own copy of them): the cap is off on a
       display no faster than the cap + 1 Hz, the hold at 61 Hz or less, and HoldFlush=auto flushes only with the cap. */
    WCHAR live_text[32];
    ini_value(L"VinceFix", L"LiveReload", live_text);
    if (ini_number(live_text) == 1) run_live(); /* starts with everything off and rewrites the ini itself */
    int is_auto = ini_is_auto(hold_text), ini_hold = is_auto ? 0 : ini_number(hold_text), ini_cap = ini_number(cap_text);
    int refresh_known = refresh > 1, flush_mode = ini_flush_mode();
    int cap_on = ini_cap > 0 && !(refresh_known && refresh <= ini_cap + 1);
    int hold_on = (is_auto || ini_hold > 0) && !(refresh_known && refresh <= 61);
    flush_run = hold_on && (flush_mode == 1 || (flush_mode == 2 && cap_on));
    WCHAR limiter_text[32], recheck_text[32];
    ini_value(L"VinceFixTest", L"Limiter", limiter_text); /* "60": a second 60 FPS limiter; "gpu": a GPU-bound Present; "join": one joining later */
    ini_value(L"VinceFixTest", L"Recheck", recheck_text); /* "1": the display changes mid-run */
    if (cap_on && ini_number(limiter_text) == 60) run_scenario(SC_CAP_ASIDE, ini_cap, ini_hold);
    if (cap_on && (limiter_text[0] | 0x20) == L'g') run_scenario(SC_CAP_GPU, ini_cap, 0);
    if (cap_on && (limiter_text[0] | 0x20) == L'j') run_scenario(SC_CAP_JOIN, ini_cap, ini_hold);
    if (cap_on && (limiter_text[0] | 0x20) == L'e') run_scenario(SC_CAP_EDGE, ini_cap, 0);
    if (cap_on && ini_number(recheck_text) == 1) run_scenario(SC_RECHECK, ini_cap, ini_hold);
    if (cap_on) run_scenario(is_auto ? SC_CAP_AUTO : hold_on ? SC_CAP_HOLD : SC_CAP_ONLY, ini_cap, ini_hold);
    if (hold_on && is_auto) run_scenario(SC_AUTO, 0, 0);
    if (!hold_on) run_scenario(SC_OFF, 0, 0);
    run_fixed();
}
