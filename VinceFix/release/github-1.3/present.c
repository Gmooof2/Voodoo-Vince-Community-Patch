/*
 * The game's Present call, routed through held_present (render thread only):
 *   Present hold ([VinceFix] PresentHold=<ms> or auto): each frame is held until a fixed time after the previous frame's
 *   start (the previous Present's return under a frame-start limiter such as the NVIDIA cap, or FrameCap's grid point).
 *   Frames whose work varies then still reach the screen evenly spaced, for a few ms of latency instead of the full frame
 *   a present-paced limiter adds. PresentHold=auto picks the hold from the measured work per frame. HoldFlush (by
 *   default only while FrameCap runs) also flushes the game's D3D11 immediate context before each hold, so the GPU
 *   renders during it. The hold is off on a display of 61 Hz or less.
 *   Frame cap ([VinceFix] FrameCap=<fps>): the same call waits after Present until 1/fps after the previous frame start,
 *   a frame-start limiter like the NVIDIA cap, so the game (which otherwise runs at the monitor's refresh rate) runs at
 *   60 on any GPU. It steps aside by itself while another limiter (a driver or RTSS cap, or VSync) does the capping;
 *   see "FrameCap steps aside" below.
 *   The display rules (settings.c) run at the first frame and again when the display changes: when the game presents
 *   through another swap chain (checked every frame: one compare), and when, checked every DISPLAY_CHECK_FRAMES frames,
 *   the swap chain's description changed (a new one at the old address, a resize, a fullscreen switch) or its window is
 *   on another monitor (display_change() in setup_info.c). That frame passes straight through; after its real Present
 *   the re-check logs why, the display: line and the rules, and starts the settings afresh as at the first frame.
 *   Live reload ([VinceFix] LiveReload=1, read at load): changes the watcher thread (settings.c) publishes are taken at
 *   the next frame, without waiting for that thread or touching the file. The Present call is then patched even while
 *   everything is off (it is only passed through then).
 *
 * Vince.exe presents from one place (+0x641CF, the only reader of its SyncInterval field):
 *     mov eax,[esi+0xAC] / push 0 / push [esi+0x9C] / mov ecx,[eax] / push eax / call [ecx+0x20]
 * The last 6 bytes become push eax / call held_present, which has Present's stdcall signature, so the stack and the code
 * after the call are unchanged. The site is found by its bytes (exactly one match required) rather than by address.
 *
 * Under the NVIDIA cap the game's frame starts are evenly spaced (the cap waits inside Present after the frame is
 * queued), but each frame reaches Present after a varying amount of work (1.9 ms typical, 5 ms at p99), and G-SYNC shows
 * it as soon as it is rendered. The hold makes every frame that is done early wait until the same point after its start,
 * so presents keep the start spacing. A frame that took longer than the hold presents at once.
 *
 * PresentHold=auto picks the hold itself, so it suits slower and faster CPUs alike: per window of AUTO_WINDOW frames it
 * targets just above the 97th percentile of the game's work per frame, at most 2 ms below the capped frame time, and
 * turns it off while nothing waits inside Present (no frame-start cap). It raises the target within AUTO_BLOCK frames
 * when frames stop fitting (AUTO_BLOCK_MIN_MISSES or more of them, in a block of mostly holdable frames), and lowers it
 * only after AUTO_LOWER_WINDOWS windows agree. The hold in use moves toward the target by at most AUTO_RAMP_UP_US /
 * AUTO_RAMP_DOWN_US per frame, so no frame visibly shifts.
 *
 * FrameCap steps aside while a second limiter at the same rate (the NVIDIA cap, RTSS) waits inside Present. FrameCap's
 * grid starts at a Present return (the first frame, and every frame that was late), so the two lock in phase: the
 * other limiter releases the frame, FrameCap's next grid point falls at about the same time, and FrameCap waits about
 * nothing. Running both only makes HoldFlush=auto flush where it measured worse (with the NVIDIA cap). Per window of
 * PACE_WINDOW frames:
 *   - FrameCap steps aside when in PACE_STEP_SHARE or more of the frames the real Present took 1 ms or more while
 *     FrameCap waited less than 1 ms, and the frames came at the cap's rate (the window's mean frame time at most
 *     PACE_SLACK_US above the period). A GPU-bound game also blocks in Present and leaves FrameCap nothing to wait for,
 *     but below the cap's rate, so it never counts; one that could run just above the cap doesn't block in Present
 *     while FrameCap waits most of each frame. VSync on a fixed-refresh display faster than the cap + 1 Hz doesn't
 *     block either (a 60 FPS frame finds the previous one already shown), and at the cap + 1 Hz or less FrameCap is off.
 *   - While aside, the frame starts at Present's return, as under a driver cap; the hold (fixed or auto) counts from
 *     there, and HoldFlush=auto stops flushing (HoldFlush=1 goes on). FrameCap comes back when the real Present took
 *     1 ms or more in under 1 - PACE_STEP_SHARE of a window's frames (40 %) at the cap's rate or faster, or when the
 *     frames come PACE_FAST_US or more faster than the cap on average in PACE_FAST_WINDOWS windows in a row (the other
 *     limiter now caps higher, or what blocked Present was a GPU just faster than the cap: FrameCap then stays on while
 *     Present keeps blocking, so it can't flap; see pace_update). Slower
 *     windows (loading, a heavy scene) change nothing either way, so neither switch flaps.
 *   - The switches re-anchor cleanly: stepping aside, the frame that was waited for starts on FrameCap's grid and the
 *     next at the other limiter's release; coming back, the grid starts one period after the last Present return, as
 *     after a late frame. One log line per switch. Settings applied (load, first frame, live reload, a display change)
 *     start over with FrameCap running.
 *   - After a late frame a driver cap may release the next one early to keep its own grid. That shortens the window
 *     the early frame falls in by at most what the late frame lost, which a window of PACE_WINDOW frames only counts
 *     as faster from PACE_FAST_US per frame on (15 ms at 60): the late frame's own window is slower and decides nothing.
 *   - A limiter that joins mid-game at another phase releases the frames while FrameCap still waits after Present: most
 *     frames then wait both in Present (1 ms or more) and after it (1 ms or more). Such a window makes FrameCap's
 *     period PACE_SLIDE_US shorter per frame (0.3 % at 60 FPS, like the hold's own ramp: nothing visibly moves), so
 *     its grid slides earlier until its point meets the other limiter's release; then FrameCap waits about nothing and
 *     steps aside as above, with no frame hurried or held back. That takes at most one period's worth of sliding plus
 *     two or three windows (about 8 s at 60 FPS). If Present's time didn't grow with the slide over a window, nothing
 *     paces Present (a busy driver): the slide stops, and FrameCap keeps its rate, until the frames stop waiting both.
 *     A limiter whose release falls within 1 ms after the frames' Present calls looks like an ordinary Present and isn't
 *     detected; if it drifts earlier, one frame waits a period in Present (late), which restarts FrameCap's grid there.
 */
#include "vincefix.h"

IMPORT HANDLE WINAPI CreateWaitableTimerExW(void *security, const WCHAR *name, DWORD flags, DWORD access);
IMPORT BOOL WINAPI SetWaitableTimer(HANDLE timer, const long long *due, long period, void *routine, void *argument, BOOL resume);
IMPORT BOOL WINAPI FlushInstructionCache(HANDLE process, const void *address, ULONG_PTR size);
IMPORT BYTE WINAPI TryAcquireSRWLockShared(void **lock); /* BOOLEAN: one byte */
IMPORT void WINAPI ReleaseSRWLockShared(void **lock);

#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#define TIMER_ALL_ACCESS 0x1F0003

/* D3D11 / DXGI slots used here (counted in vincefix.h) */
#define SLOT_PRESENT 8                /* IDXGISwapChain */
#define SLOT_GET_IMMEDIATE_CONTEXT 40 /* ID3D11Device */
#define SLOT_FLUSH 111                /* ID3D11DeviceContext */
const GUID IID_ID3D11Device = {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};
typedef HRESULT(WINAPI *PresentFn)(void *swapchain, DWORD sync, DWORD flags);
typedef void(WINAPI *GetImmediateContextFn)(void *device, void **context);
typedef void(WINAPI *FlushFn)(void *context);

static const BYTE PRESENT_SITE[20] = {0x8B, 0x86, 0xAC, 0x00, 0x00, 0x00, 0x6A, 0x00, 0xFF, 0xB6,
                                      0x9C, 0x00, 0x00, 0x00, 0x8B, 0x08, 0x50, 0xFF, 0x51, 0x20};
#define PRESENT_SITE_PATCH 14   /* mov ecx,[eax] / push eax / call [ecx+0x20] (SLOT_PRESENT * 4): */
#define PRESENT_PATCH_SIZE 6    /* the 6 bytes that become push eax / call rel32 */
#define X86_PUSH_EAX 0x50
#define X86_CALL_REL32 0xE8
#define HOLD_SPIN_US 1500       /* the timer is set this much early; the rest is a QPC spin, so a frame is never early.
                                   High-resolution timers woke up to ~1 ms late in tests (500 us missed 11-16 % of frames) */
#define HOLD_FIRST_LOG 600      /* frames; then every HOLD_LOG_EVERY (written on the render thread, inside the next frame's hold) */
#define HOLD_LOG_EVERY 3600
#define AUTO_WINDOW 300         /* frames per measurement (5 s at 60 FPS) */
#define AUTO_PERCENTILE 97      /* the hold covers this share of the frames' work */
#define AUTO_MARGIN_US 200
#define AUTO_DEADBAND_US 200
#define AUTO_STEADY_SHARE 8     /* tenths of a window (or block) that must be holdable, and of a window near its median frame time */
#define AUTO_NEAR_MEDIAN 10     /* buckets (1 ms) either side of the median frame time that count as near it */
#define AUTO_RAMP_UP_US 50      /* per frame: the hold in use follows the target this fast, */
#define AUTO_RAMP_DOWN_US 20    /* so no single present moves visibly */
#define AUTO_BLOCK 60           /* frames: a heavier scene raises the target after one block */
#define AUTO_BLOCK_MIN_MISSES 6 /* frames of a block that must miss the target to raise it: a 97th-percentile target leaves
                                   about 2 per block above it by design, and a frame Windows paused looks the same */
#define AUTO_LOWER_WINDOWS 3    /* the target only drops when this many steady windows in a row want less */
#define AUTO_NOT_LOGGED 0xFFFFFFFFu
#define BUCKET_US 100           /* histogram buckets are 0.1 ms */
#define WORK_BUCKETS 161        /* the last collects everything from 16 ms */
#define INTERVAL_BUCKETS 401    /* the last collects everything from 40 ms */
#define LATENCY_BUCKETS 401     /* the stats line's latency figures; the last collects everything from 40 ms */
#define PACE_WINDOW 60          /* frames per step-aside decision (1 s at 60 FPS) */
#define PACE_STEP_SHARE 6       /* tenths of a window: steps aside at this share, comes back below 1 - it (60 % / 40 %) */
#define PACE_SLACK_US 100       /* a window whose mean frame time is at most this far above the cap's period holds the cap */
#define PACE_FAST_US 250        /* one this far below it or more runs faster than the cap (15 ms per window: more than a
                                   frame that started late at the window's edge can take off a capped window) */
#define PACE_FAST_WINDOWS 2     /* windows in a row that must run faster than the cap to bring FrameCap back */
#define PACE_SLIDE_US 50       /* per frame: FrameCap's period is this much shorter while it slides toward another limiter */
#define DISPLAY_CHECK_FRAMES 60 /* frames between the cheap checks for a changed display (1 s at 60 FPS) */

/* ---- state: all of it used only on the game's render thread (and at load), except where a comment says otherwise ---- */

Settings g_asked;
int g_setup_done;
int g_flush_stopped;
HANDLE g_game_window;
DWORD g_display_refresh;
long g_display_seq;
static void *g_swap_chain;     /* the swap chain the display was last checked for */
static DWORD g_display_frames; /* frames since the last check */

static DWORD g_qpc_frequency;  /* QueryPerformanceFrequency, checked at load to be 1e6 .. 1e8 */
static DWORD g_ticks_per_ms;   /* QueryPerformanceFrequency / 1000 */
static int g_active;           /* the hold or the cap runs; otherwise held_present only calls Present */
static DWORD g_hold_us, g_hold_ticks, g_spin_ticks; /* the hold in use */
static HANDLE g_hold_timer;    /* high-resolution waitable timer */
static long long g_hold_last;  /* the frame start the hold counts from (Present's return, or FrameCap's grid point); 0 before the first */
static long long g_frame_began; /* when that frame actually began (later than a grid point after a late wake) */
static DWORD g_cap_fps, g_cap_ticks, g_cap_remainder, g_cap_carry; /* period = g_cap_ticks + g_cap_remainder / g_cap_fps */

/* FrameCap steps aside */
static int g_cap_aside;          /* another limiter paces the game: FrameCap doesn't wait */
static DWORD g_pace_frames, g_pace_hits, g_pace_both; /* this window: frames, those that look paced by another limiter,
                                                         and those that waited both in Present and after it */
static long long g_pace_span;    /* this window: the sum of its frame-start intervals */
static long long g_pace_inside[2]; /* this window: the time inside Present, summed over its first and its second half */
static DWORD g_cap_slide_ticks;  /* FrameCap's period is this much shorter while it slides toward another limiter, else 0 */
static int g_pace_busy;          /* a slide found that Present's time didn't follow it: no slide until that changes */
static int g_pace_faster;        /* aside: windows in a row that ran faster than the cap */
static int g_cap_stays;          /* FrameCap came back for that: it doesn't step aside again while Present still blocks */

/* Auto mode */
static int g_hold_auto;
static DWORD g_auto_goal_us;                    /* the target the hold in use ramps to */
static DWORD g_auto_logged_us = AUTO_NOT_LOGGED, g_auto_log_frame; /* the target last logged, and at which frame */
static int g_auto_capped = 1;                   /* the last window counted as capped */
static DWORD g_auto_ceiling_us, g_auto_ceiling_bucket; /* the highest hold the last capped window allowed; 0 before it */
static DWORD g_work_hist[WORK_BUCKETS], g_interval_hist[INTERVAL_BUCKETS], g_window_frames, g_window_quick; /* this window */
static DWORD g_auto_wants[AUTO_LOWER_WINDOWS], g_auto_want_count; /* what the last steady windows wanted, newest first */
static struct { DWORD frames, misses, unholdable, top1, top2; } g_block; /* this block: its two longest missing frames' buckets */

/* HoldFlush. A failed lookup stops it for the session; its log line waits until after that frame's real Present. */
static int g_flush_runs;               /* the settings flush held frames (HoldFlush=1, or auto with FrameCap) */
static int g_flush_on;                 /* flush before each held frame: g_flush_runs, unless stopped or FrameCap stepped aside (auto) */
static HRESULT g_flush_failure;        /* GetDevice's result when the lookup failed */
static int g_flush_failure_unlogged;
static DWORD g_flush_typical_ticks;    /* running average of a flush's lookup + Flush time, each clamped at 1 ms */

/* The stats line: counts since the last change of settings (see reset_stats). */
typedef struct { DWORD ms, units, max; } Cost; /* a running sum (whole ms + 100 ns units below 1 ms) and the largest value (100 ns) */
static DWORD g_hold_frames, g_hold_held, g_hold_after, g_hold_quick, g_hold_late, g_hold_late_max_ticks;
static DWORD g_hold_next_log = HOLD_FIRST_LOG;
static DWORD g_auto_skipped;          /* unsteady windows */
static DWORD g_flushes, g_flush_late; /* held frames flushed; of them, the ones whose work + Flush passed the hold point */
static Cost g_flush_cost, g_lookup_cost; /* the Flush call; getting and releasing the device and context */
/* Since the last stats line. Time inside the real Present: where a driver frame cap (or VSync) waits, so the stats show
   which limiter paces the game. The latency figures: frame start to the real Present (the input-to-Present part of the
   latency; add PresentMon's MsUntilDisplayed for start to screen), the hold's own wait in held frames (with HoldFlush
   including the Flush), and FrameCap's wait after Present (PresentMon counts it as the next frame's CPU time). Times are
   summed in QPC ticks and sorted into 0.1 ms buckets by a multiply, so a frame costs no division. */
typedef struct { unsigned long long ticks; DWORD frames; DWORD hist[LATENCY_BUCKETS]; } Latency;
static Cost g_present_cost;
static DWORD g_present_frames;
static Latency g_start_to_present, g_hold_wait;
static unsigned long long g_cap_wait_ticks;
static DWORD g_cap_wait_frames;
static DWORD g_bucket_scale;          /* 2^32 / (QPC ticks per 0.1 ms), rounded up: bucket = ticks * scale >> 32 */

/* ---- tick math (32-bit only: no CRT division helpers; callers keep ticks below 2^31 and the products in range) ----- */

static DWORD ticks_to_units(DWORD ticks, DWORD units_per_ms) {
    return ticks / g_ticks_per_ms * units_per_ms + ticks % g_ticks_per_ms * units_per_ms / g_ticks_per_ms;
}
/* QPC ticks -> 100 ns units, clamped at 1 s (which keeps ticks_to_units' products in range). */
static DWORD ticks_to_100ns(long long ticks) {
    DWORD limit = g_ticks_per_ms * 1000;
    return ticks <= 0 ? 0 : ticks_to_units(ticks < limit ? (DWORD)ticks : limit, 10000);
}
static void cost_add(Cost *c, DWORD units) {
    c->units += units; c->ms += c->units / 10000; c->units %= 10000;
    if (units > c->max) c->max = units;
}
static DWORD average(unsigned long long sum, DWORD count) { /* no 64-bit division: halve sum and count until the sum fits */
    while (sum >> 32) { sum >>= 1; count >>= 1; }
    return count ? (DWORD)sum / count : 0;
}
static DWORD cost_average(const Cost *c, DWORD count) { return average((unsigned long long)c->ms * 10000 + c->units, count); } /* 100 ns units */
/* One frame's time into a latency figure, clamped at 1 s (which keeps the multiply in range). */
static void latency_add(Latency *l, long long ticks) {
    DWORD limit = g_ticks_per_ms * 1000, t = ticks <= 0 ? 0 : ticks < limit ? (DWORD)ticks : limit;
    DWORD bucket = (DWORD)((unsigned long long)t * g_bucket_scale >> 32);
    l->hist[bucket < LATENCY_BUCKETS - 1 ? bucket : LATENCY_BUCKETS - 1]++;
    l->ticks += t;
    l->frames++;
}

/* ---- the stats line ----------------------------------------------------------------------------------------------- */

static void put_tenths(Line *line, DWORD tenths) { put_dec(line, tenths / 10, 0); put(line, "."); put_dec(line, tenths % 10, 0); }
static void put_units_ms(Line *line, DWORD units) { put_dec(line, units / 10000, 0); put(line, "."); put_dec(line, units % 10000 / 100, 2); } /* 100 ns units as ms, 2 decimals */
static COLD void put_ticks_ms(Line *line, unsigned long long sum, DWORD count) { put_units_ms(line, ticks_to_100ns(average(sum, count))); } /* their average */

static DWORD hist_rank(const DWORD *hist, DWORD buckets, DWORD rank);
/* "avg 3.65 ms, p99 6.1 ms": the 99th percentile is its bucket's upper edge */
static void put_latency(Line *line, const Latency *l) {
    DWORD bucket = hist_rank(l->hist, LATENCY_BUCKETS, (l->frames * 99 + 99) / 100);
    put(line, "avg "); put_ticks_ms(line, l->ticks, l->frames); put(line, " ms, p99 ");
    if (bucket == LATENCY_BUCKETS - 1) put(line, "over 40");
    else put_tenths(line, l->frames ? bucket + 1 : 0);
    put(line, " ms");
}

/* ", 260 flushed (Flush total 62.09 ms, avg 238.8 us, max 3511.1 us; 8 ran past the hold; getting the context ...)" */
static void put_flush_stats(Line *line) {
    put(line, ", "); put_dec(line, g_flushes, 0); put(line, " flushed (Flush total "); put_dec(line, g_flush_cost.ms, 0);
    put(line, "."); put_dec(line, g_flush_cost.units / 100, 2); put(line, " ms, avg ");
    put_tenths(line, cost_average(&g_flush_cost, g_flushes)); put(line, " us, max "); put_tenths(line, g_flush_cost.max);
    put(line, " us; "); put_dec(line, g_flush_late, 0); put(line, " ran past the hold; getting the context avg ");
    put_tenths(line, cost_average(&g_lookup_cost, g_flushes)); put(line, " us, max "); put_tenths(line, g_lookup_cost.max);
    put(line, g_flush_stopped == FLUSH_STOPPED_NO_DEVICE    ? " us; stopped: no D3D11 device)"
              : g_flush_stopped == FLUSH_STOPPED_NO_CONTEXT ? " us; stopped: no D3D11 immediate context)"
                                                             : " us)");
}

/* At the first stats line of these settings: a warning when the limiters don't add up. A frame-start limiter (NVIDIA
   cap, RTSS front edge) waits inside Present; if almost nothing does, the hold itself is what limits the frame rate, and
   it only adds latency. (Auto mode turns itself off in that case instead; FrameCap is such a limiter, waiting after
   Present, and steps aside for another one by itself.) */
static void check_limiters(void) {
    if (!g_hold_auto && !g_cap_fps && g_hold_held >= HOLD_FIRST_LOG / 2 && g_hold_quick > g_hold_held / 2)
        log_text("present hold: WARNING: Present returned within 1 ms in most held frames, so no frame-rate cap is spacing "
                 "frame starts; the hold is limiting the frame rate and only adding latency. Set a cap or PresentHold=0");
}

/* The latency figures since the last stats line: "; frame start to real Present avg 3.65 ms, p99 6.1 ms; hold wait avg
   1.20 ms, p99 3.4 ms (590 held frames); cap wait avg 9.80 ms over 599 frames" (the hold and the cap when they run). */
static void put_latency_stats(Line *line) {
    put(line, "; frame start to real Present "); put_latency(line, &g_start_to_present);
    if (g_hold_auto || g_hold_us) {
        put(line, "; hold wait "); put_latency(line, &g_hold_wait);
        put(line, " ("); put_dec(line, g_hold_wait.frames, 0); put(line, " held frames)");
    }
    if (g_cap_fps) {
        put(line, "; cap wait avg "); put_ticks_ms(line, g_cap_wait_ticks, g_cap_wait_frames);
        put(line, " ms over "); put_dec(line, g_cap_wait_frames, 0); put(line, " frames"); /* fewer: FrameCap stepped aside in the others */
    }
}
static void clear_latency_stats(void) {
    zero(&g_start_to_present, sizeof g_start_to_present);
    zero(&g_hold_wait, sizeof g_hold_wait);
    g_cap_wait_ticks = g_cap_wait_frames = 0;
}

static COLD void log_hold_stats(void) {
    Line line; begin(&line);
    put(&line, "present hold stats: "); put_dec(&line, g_hold_frames, 0); put(&line, " frames");
    if (g_hold_auto || g_hold_us) {
        put(&line, ", "); put_dec(&line, g_hold_held, 0); put(&line, " held, "); put_dec(&line, g_hold_after, 0);
        put(&line, " arrived after the hold, "); put_dec(&line, g_hold_quick, 0); put(&line, " held ones left Present within 1 ms");
    }
    if (g_flush_runs || g_flush_stopped) put_flush_stats(&line);
    put(&line, "; timer woke late "); put_dec(&line, g_hold_late, 0);
    put(&line, " times (max "); put_dec(&line, ticks_to_units(g_hold_late_max_ticks, 1000), 0); put(&line, " us)");
    if (g_hold_auto) {
        put(&line, ", hold now "); put_ms(&line, g_hold_us); put(&line, " ms (auto, ");
        put_dec(&line, g_auto_skipped, 0); put(&line, " unsteady windows skipped)");
    }
    if (g_cap_fps) { put(&line, ", frame cap "); put_dec(&line, g_cap_fps, 0); put(&line, " FPS"); }
    put(&line, "; time inside Present: avg "); put_units_ms(&line, cost_average(&g_present_cost, g_present_frames));
    put(&line, " ms, max "); put_units_ms(&line, g_present_cost.max); put(&line, " ms (last "); put_dec(&line, g_present_frames, 0); put(&line, " frames)");
    put_latency_stats(&line);
    emit(&line);
    g_present_cost.ms = g_present_cost.units = g_present_cost.max = g_present_frames = 0;
    clear_latency_stats();
    if (g_hold_frames == HOLD_FIRST_LOG) check_limiters();
}

/* The stats line counts from the last change of settings (load, the first frame's display check, a live reload), so its
   figures and the frame-600 checks above always describe the settings that run. */
static void reset_stats(void) {
    g_hold_frames = g_hold_held = g_hold_after = g_hold_quick = g_hold_late = g_hold_late_max_ticks = 0;
    g_hold_next_log = HOLD_FIRST_LOG;
    g_auto_skipped = g_auto_log_frame = g_flushes = g_flush_late = g_present_frames = 0;
    g_flush_cost = g_lookup_cost = g_present_cost = (Cost){0, 0, 0};
    clear_latency_stats();
}

/* ---- waiting ------------------------------------------------------------------------------------------------------ */

/* Returns the time it stopped waiting (>= target). The timer is set HOLD_SPIN_US early and the rest is a spin: under
   load the timer has woken over 3 ms late, so it never ends a wait alone. */
static long long wait_until(long long target, long long now) {
    long long left = target - now;
    if (left > g_spin_ticks) {
        long long due = -(long long)ticks_to_units((DWORD)(left - g_spin_ticks), 10000); /* relative, 100 ns units */
        /* The timeout only matters if the timer never fires; then the spin finishes the wait. */
        if (SetWaitableTimer(g_hold_timer, &due, 0, 0, 0, 0)) WaitForSingleObject(g_hold_timer, ticks_to_units((DWORD)left, 1) + 1);
        QueryPerformanceCounter(&now);
        if (now > target) {
            DWORD late = (DWORD)(now - target < 0x7FFFFFFF ? now - target : 0x7FFFFFFF);
            g_hold_late++;
            if (late > g_hold_late_max_ticks) g_hold_late_max_ticks = late;
        }
    }
    while (now < target) { __builtin_ia32_pause(); QueryPerformanceCounter(&now); }
    return now;
}

/* Returns the next frame's start (the grid point, or `now` when late) and sets g_frame_began to when that frame really
   began (after a late timer wake, later than the grid point); FrameCap's wait is g_frame_began - now.
   FrameCap: a frame-start limiter like the NVIDIA cap. After the real Present returns, wait until one period after the
   previous frame start, which is then the next frame's start (the grid point itself, so a late timer wake doesn't shift
   later frames). A frame that is already late starts at once and the spacing restarts from it, instead of hurrying the
   frame after. */
static long long cap_wait(long long now) {
    if (!g_hold_last) { g_frame_began = now; return now; }
    long long next = g_hold_last + g_cap_ticks - g_cap_slide_ticks;
    g_cap_carry += g_cap_remainder; /* the period's fraction of a tick, so 60 means 60.000 */
    if (g_cap_carry >= g_cap_fps) { g_cap_carry -= g_cap_fps; next++; }
    if (now >= next) { g_frame_began = now; return now; }
    g_frame_began = wait_until(next, now);
    return next;
}

/* ---- FrameCap steps aside for another limiter (see the top of the file) ------------------------------------------- */

static void clear_pace_window(void) {
    g_pace_frames = g_pace_hits = g_pace_both = 0;
    g_pace_span = g_pace_inside[0] = g_pace_inside[1] = 0;
}

/* HoldFlush=auto flushes only while FrameCap runs; HoldFlush=1 always. Every change of what g_flush_on depends on goes
   through here (settings applied, FrameCap stepping aside or back), except stop_flush(), which only ever turns it off. */
static void update_flush(void) {
    g_flush_on = g_flush_runs && !g_flush_stopped && !(g_cap_aside && g_asked.flush == FLUSH_AUTO);
}

enum { CAP_ASIDE, CAP_BACK, CAP_BACK_FASTER }; /* stepped aside; back as the other limiter stopped; back as the game ran faster */

static COLD void set_cap_aside(int change) {
    int aside = change == CAP_ASIDE;
    g_cap_aside = aside;
    g_cap_carry = 0; /* coming back, the grid starts afresh from the last Present return (g_hold_last) */
    g_cap_slide_ticks = 0;
    g_pace_busy = g_pace_faster = 0;
    g_cap_stays = change == CAP_BACK_FASTER;
    update_flush();
    Line line; begin(&line);
    put(&line, aside ? "frame cap: another frame limiter (a driver or RTSS cap, or VSync) is waiting inside Present, so "
                       "VinceFix's cap steps aside; it comes back if that limiter stops"
               : change == CAP_BACK ? "frame cap: no other limiter is pacing the game any more; VinceFix's cap is back on"
                                    : "frame cap: without VinceFix's cap the game ran faster than the cap, so what waits "
                                      "inside Present is not a frame cap (a GPU just fast enough, say); VinceFix's cap is "
                                      "back on and stays on");
    if (g_flush_runs && !g_flush_stopped && g_asked.flush == FLUSH_AUTO)
        put(&line, aside ? " (HoldFlush=auto stops flushing meanwhile)" : " (HoldFlush=auto flushes again)");
    emit(&line);
}

/* A window in which most frames waited both inside Present and after it, while FrameCap runs: slide (or keep sliding)
   unless an earlier slide showed that Present's time doesn't follow it. A slide moves every frame PACE_SLIDE_US earlier
   against a limiter that waits inside Present, so Present's time grows by that much per frame: the window's second
   half spends (PACE_WINDOW / 2)^2 * PACE_SLIDE_US more in Present than its first. Under half of that, Present is just
   slow (a busy driver, VSync on a faster display), and the slide stops. Any other window ends a slide and the doubt. */
static void slide_update(int both_waited) {
    long long slide = PACE_SLIDE_US * g_ticks_per_ms / 1000;
    long long grown = g_pace_inside[1] - g_pace_inside[0];
    if (!both_waited) g_pace_busy = 0;
    else if (g_cap_slide_ticks && grown < (PACE_WINDOW / 2) * (PACE_WINDOW / 2) / 2 * slide) g_pace_busy = 1;
    g_cap_slide_ticks = both_waited && !g_pace_busy ? (DWORD)slide : 0;
}

/* Once per PACE_WINDOW frames, so kept out of held_present. A window slower than the cap (loading, a heavy scene)
   decides nothing.
   Aside, FrameCap comes back when Present stops blocking (the other limiter stopped), or after PACE_FAST_WINDOWS windows
   in a row ran faster than the cap: one alone can be a driver cap catching up after a late frame. Coming back for being
   faster, FrameCap stays on (g_cap_stays) until Present stops blocking: what blocked was not a cap at its rate but, say,
   a GPU that just manages 61-62 FPS. Capped at 60 such a GPU blocks Present while FrameCap waits under 1 ms, which looks
   like another limiter; without g_cap_stays FrameCap would step aside and back every second or two. */
static COLD void pace_update(void) {
    long long periods = (long long)PACE_WINDOW * g_cap_ticks, per_us = PACE_WINDOW * g_ticks_per_ms / 1000;
    int at_cap = g_pace_span <= periods + PACE_SLACK_US * per_us;
    int faster = g_pace_span <= periods - PACE_FAST_US * per_us;
    DWORD most = PACE_WINDOW * PACE_STEP_SHARE, few = PACE_WINDOW * (10 - PACE_STEP_SHARE); /* in tenths */
    int blocking = g_pace_hits * 10 >= few; /* Present still blocks in enough frames */
    /* Only back-to-back faster windows count: a slower one between two driver-cap catch-ups breaks the row. */
    g_pace_faster = g_cap_aside && faster ? g_pace_faster + 1 : 0;
    if (at_cap) {
        if (g_cap_aside) {
            if (!blocking) set_cap_aside(CAP_BACK);
            else if (g_pace_faster >= PACE_FAST_WINDOWS) set_cap_aside(CAP_BACK_FASTER);
        }
        else if (g_cap_stays) g_cap_stays = blocking;
        else if (g_pace_hits * 10 >= most) set_cap_aside(CAP_ASIDE);
        else slide_update(g_pace_both * 10 >= most);
    }
    clear_pace_window();
}

/* Each frame after the first while FrameCap is on. inside: in the real Present; waited: FrameCap's wait after it (0 while
   aside); interval: frame start to frame start. While FrameCap runs, a frame looks paced by another limiter when Present
   took 1 ms or more and FrameCap had nothing left to wait for, and waited both when FrameCap still waited 1 ms or more;
   while aside, it looks paced when Present took 1 ms or more. */
static void pace_sample(long long inside, long long waited, long long interval) {
    if (inside >= g_ticks_per_ms) {
        if (g_cap_aside || waited < g_ticks_per_ms) g_pace_hits++;
        else g_pace_both++;
    }
    g_pace_inside[g_pace_frames >= PACE_WINDOW / 2] += inside;
    g_pace_span += interval;
    if (++g_pace_frames == PACE_WINDOW) pace_update();
}

/* ---- auto mode: measure, pick a target, ramp to it ---------------------------------------------------------------- */

static DWORD bucket_of(long long ticks, DWORD last) { /* 0.1 ms buckets, everything from `last` up in the last one */
    if (ticks <= 0) return 0;
    if (ticks >= (long long)(last * g_ticks_per_ms / 10)) return last;
    return ticks_to_units((DWORD)ticks, 10);
}
static DWORD bucket_top_us(DWORD bucket) { return (bucket + 1) * BUCKET_US; } /* a bucket's upper edge */

static DWORD hist_rank(const DWORD *hist, DWORD buckets, DWORD rank) { /* bucket of the rank-th smallest sample (1-based) */
    DWORD seen = 0;
    for (DWORD b = 0; b < buckets; b++) { seen += hist[b]; if (seen >= rank) return b; }
    return buckets - 1;
}
static DWORD hist_count(const DWORD *hist, DWORD first, DWORD end) { /* samples in buckets first .. end - 1 */
    DWORD n = 0;
    for (DWORD b = first; b < end; b++) n += hist[b];
    return n;
}
static void clear_window(void) {
    for (DWORD b = 0; b < WORK_BUCKETS; b++) g_work_hist[b] = 0;
    for (DWORD b = 0; b < INTERVAL_BUCKETS; b++) g_interval_hist[b] = 0;
    g_window_frames = g_window_quick = 0;
}

/* Logged for the first target, a change to or from 0 or of 1 ms or more, and otherwise at most once per HOLD_LOG_EVERY
   frames for 0.5 ms or more (each line is a file write on the render thread). */
static void auto_log_target(DWORD work_tenths, DWORD period_tenths, int raised) {
    DWORD goal = g_auto_goal_us, logged = g_auto_logged_us, change = goal > logged ? goal - logged : logged - goal;
    if (logged != AUTO_NOT_LOGGED && !goal == !logged && change < 1000 && (change < 500 || g_hold_frames - g_auto_log_frame < HOLD_LOG_EVERY)) return;
    Line line; begin(&line);
    put(&line, "present hold auto: target "); put_ms(&line, goal);
    if (raised) { put(&line, " ms (raised: frames took up to "); put_tenths(&line, work_tenths); put(&line, " ms of work)"); }
    else {
        put(&line, " ms (97th percentile of frame work "); put_tenths(&line, work_tenths);
        put(&line, " ms, frame time "); put_tenths(&line, period_tenths); put(&line, " ms)");
    }
    emit(&line);
    g_auto_logged_us = goal;
    g_auto_log_frame = g_hold_frames;
}

/* A frame-start cap waits inside Present in most of a window's frames (FrameCap: after it). 60 % / 40 % so that a share
   near half doesn't flip the state every window. */
static int window_capped(DWORD n) {
    if (g_cap_fps) return 1;
    return g_auto_capped ? g_window_quick * 10 <= n * 6 : g_window_quick * 10 < n * 4;
}

/* The highest hold a median frame time of `period` (0.1 ms) allows: HOLD_HEADROOM_US below it, at most HOLD_MAX_US,
   0 when that is under HOLD_MIN_US. */
static DWORD hold_ceiling_us(DWORD period) {
    DWORD period_us = period * BUCKET_US;
    DWORD ceiling = period_us >= HOLD_MIN_US + HOLD_HEADROOM_US ? period_us - HOLD_HEADROOM_US : 0;
    return ceiling > HOLD_MAX_US ? HOLD_MAX_US : ceiling;
}

/* A steady window wants `want`: raise the target to it at once, or lower it only to what the last AUTO_LOWER_WINDOWS
   steady windows all allow. */
static DWORD goal_after(DWORD goal, DWORD want) {
    for (DWORD i = AUTO_LOWER_WINDOWS - 1; i > 0; i--) g_auto_wants[i] = g_auto_wants[i - 1];
    g_auto_wants[0] = want;
    if (g_auto_want_count < AUTO_LOWER_WINDOWS) g_auto_want_count++;
    if (want > goal + AUTO_DEADBAND_US) return want;
    if (g_auto_want_count < AUTO_LOWER_WINDOWS) return goal;
    DWORD highest = 0;
    for (DWORD i = 0; i < AUTO_LOWER_WINDOWS; i++) if (g_auto_wants[i] > highest) highest = g_auto_wants[i];
    return highest + AUTO_DEADBAND_US < goal ? highest : goal;
}

/* Once per AUTO_WINDOW frames, so kept out of held_present. */
static COLD void auto_update(void) {
    DWORD n = g_window_frames, goal = g_auto_goal_us, period = 0, work = 0;
    int capped = window_capped(n);
    if (!capped) { goal = 0; g_auto_want_count = 0; g_auto_ceiling_us = g_auto_ceiling_bucket = 0; }
    else {
        period = hist_rank(g_interval_hist, INTERVAL_BUCKETS, (n + 1) / 2); /* median frame time, 0.1 ms */
        DWORD ceiling = hold_ceiling_us(period);
        g_auto_ceiling_us = ceiling;
        g_auto_ceiling_bucket = ceiling / BUCKET_US;
        if (goal > ceiling) goal = ceiling; /* the frame time got shorter (a higher cap): never hold past it */
        /* Frames whose work reaches the ceiling (loading, a stall) can't be held; leave them out. */
        DWORD holdable = hist_count(g_work_hist, 0, g_auto_ceiling_bucket);
        DWORD near_median = hist_count(g_interval_hist, period > AUTO_NEAR_MEDIAN ? period - AUTO_NEAR_MEDIAN : 0,
                                       period + AUTO_NEAR_MEDIAN + 1 < INTERVAL_BUCKETS ? period + AUTO_NEAR_MEDIAN + 1 : INTERVAL_BUCKETS);
        if (!ceiling) { goal = 0; g_auto_want_count = 0; }
        else if (holdable * 10 < n * AUTO_STEADY_SHARE || near_median * 10 < n * AUTO_STEADY_SHARE) { g_auto_skipped++; g_auto_want_count = 0; } /* unsteady: keep the target */
        else {
            work = hist_rank(g_work_hist, WORK_BUCKETS, (holdable * AUTO_PERCENTILE + 99) / 100) + 1; /* upper bucket edge, 0.1 ms */
            DWORD want = work * BUCKET_US + AUTO_MARGIN_US;
            if (want < HOLD_MIN_US) want = HOLD_MIN_US;
            if (want > ceiling) want = ceiling;
            goal = goal_after(goal, want);
        }
    }
    if (capped != g_auto_capped) {
        log_text(capped ? "present hold auto: frame-rate cap detected"
                        : "present hold auto: no frame-rate cap detected (Present returns at once); hold off until one is");
        g_auto_capped = capped;
        if (!capped) g_auto_logged_us = 0;
    }
    g_auto_goal_us = goal;
    if (work) auto_log_target(work, period, 0);
    clear_window();
}

/* Between windows: when the target no longer covers the frames (a heavier scene), a block raises it to the
   second-longest of them, so a single outlier doesn't set it. Only a steady block does that: at least
   AUTO_BLOCK_MIN_MISSES frames missed (not just a few late ones), and like a window's, most of its frames could be held
   (not a loading stretch, whose frames reach the ceiling). `w` is the frame's work bucket. */
static void block_sample(DWORD w) {
    if (g_auto_ceiling_bucket && w >= g_auto_ceiling_bucket) g_block.unholdable++;
    else if (g_auto_ceiling_bucket && bucket_top_us(w) + AUTO_MARGIN_US > g_auto_goal_us) {
        g_block.misses++;
        if (w > g_block.top1) { g_block.top2 = g_block.top1; g_block.top1 = w; }
        else if (w > g_block.top2) g_block.top2 = w;
    }
    if (++g_block.frames < AUTO_BLOCK) return;
    DWORD want = bucket_top_us(g_block.top2) + AUTO_MARGIN_US;
    if (want > g_auto_ceiling_us) want = g_auto_ceiling_us;
    int steady = g_block.misses >= AUTO_BLOCK_MIN_MISSES && g_block.unholdable * 10 <= AUTO_BLOCK * (10 - AUTO_STEADY_SHARE);
    if (steady && want > g_auto_goal_us) {
        g_auto_goal_us = want;
        g_auto_want_count = 0; /* lowering starts over from here */
        auto_log_target(g_block.top2 + 1, 0, 1);
    }
    g_block.frames = g_block.misses = g_block.unholdable = g_block.top1 = g_block.top2 = 0;
}

/* The hold in use follows the target by at most AUTO_RAMP_UP_US / AUTO_RAMP_DOWN_US per frame. */
static void ramp_hold(void) {
    DWORD hold = g_hold_us, goal = g_auto_goal_us;
    if (hold < goal) hold = goal - hold > AUTO_RAMP_UP_US ? hold + AUTO_RAMP_UP_US : goal;
    else if (hold > goal) hold = hold - goal > AUTO_RAMP_DOWN_US ? hold - AUTO_RAMP_DOWN_US : goal;
    g_hold_us = hold;
    g_hold_ticks = hold * g_ticks_per_ms / 1000;
}

/* Each frame after the first. work: previous frame start to when the frame could present; inside: in the real Present;
   interval: start to start. */
static void auto_sample(long long work, long long inside, long long interval) {
    DWORD w = bucket_of(work, WORK_BUCKETS - 1);
    g_work_hist[w]++;
    g_interval_hist[bucket_of(interval, INTERVAL_BUCKETS - 1)]++;
    if (inside < g_ticks_per_ms) g_window_quick++;
    block_sample(w);
    if (++g_window_frames == AUTO_WINDOW) auto_update();
    ramp_hold();
}

/* Auto mode from scratch (no target, nothing measured), as at the start. */
static void reset_auto(void) {
    clear_window();
    for (DWORD i = 0; i < AUTO_LOWER_WINDOWS; i++) g_auto_wants[i] = 0;
    g_block.frames = g_block.misses = g_block.unholdable = g_block.top1 = g_block.top2 = 0;
    g_auto_goal_us = g_auto_ceiling_us = g_auto_ceiling_bucket = g_auto_want_count = 0;
    g_auto_logged_us = AUTO_NOT_LOGGED;
    g_auto_capped = 1;
}

/* ---- running the settings ----------------------------------------------------------------------------------------- */

/* Render thread (or DllMain, before the game runs): run what g_asked comes to at this refresh rate. Auto mode, the
   frame grid, FrameCap's step-aside detection and the stats start afresh, so the next frame is treated like the first:
   not held, and FrameCap's grid starts there. (FrameCap starts out running even if it had stepped aside; a limiter
   that is still there makes it step aside again within a window.) */
COLD void apply_settings(DWORD refresh) {
    Settings run = decide(&g_asked, refresh);
    g_cap_fps = run.cap_fps;
    g_cap_ticks = run.cap_fps ? g_qpc_frequency / run.cap_fps : 0;
    g_cap_remainder = run.cap_fps ? g_qpc_frequency % run.cap_fps : 0;
    g_cap_carry = 0;
    g_hold_auto = run.hold_auto;
    g_hold_us = run.hold_us; /* auto: starts at 0 and ramps once the first window is measured */
    g_hold_ticks = g_hold_us * g_ticks_per_ms / 1000;
    g_flush_runs = run.flush == FLUSH_ON;
    g_cap_aside = g_pace_busy = g_pace_faster = g_cap_stays = 0; /* FrameCap starts out running; the step-aside detection starts over */
    g_cap_slide_ticks = 0;
    clear_pace_window();
    update_flush();
    g_active = run.cap_fps || run.hold_us || run.hold_auto;
    reset_auto();
    reset_stats();
    g_hold_last = g_frame_began = 0;
}

/* ---- HoldFlush ---------------------------------------------------------------------------------------------------- */
/*
 * Before a frame is held, Flush() the game's D3D11 immediate context so the GPU renders during the hold and Present only
 * has to queue the flip. With FrameCap the frames reach the screen later and less evenly after Present than under the
 * NVIDIA cap, as if the driver submitted most of the work in Present (after the hold) unless its own limiter runs. Only
 * frames that will be held are flushed; the Flush's time counts against the hold, and a frame whose work plus Flush
 * passes the hold point presents at once, late ("ran past the hold" in the stats line).
 * Auto mode counts a flushed frame's work up to the end of the Flush, since the frame can't present before that
 * (otherwise its target would sit below what flushed frames need, by the Flush time), and adds the typical Flush time to
 * the work of frames that arrived after the hold and so weren't flushed.
 * Safe here: the render thread is the one that drives this context, and Present flushes it on this same thread anyway;
 * the game is inside its Present call, so the swap chain and its device are alive. The device and context are looked up
 * for every held frame and released right after the Flush (a few COM calls, timed in the stats line) instead of kept: the
 * game may recreate its device and swap chain (resolution change, device loss), possibly at the same address, and a kept
 * context would flush the wrong device and keep the old one alive.
 * If the lookup fails (not a D3D11 swap chain, or no immediate context), HoldFlush stops for the session. The reason is
 * logged after this frame's real Present, so the file write doesn't delay the held frame.
 */

/* HoldFlush stops for the session: `reason` is FLUSH_STOPPED_*, `result` GetDevice's. Logged after this frame's Present. */
static COLD void stop_flush(int reason, HRESULT result) {
    g_flush_on = 0;
    __atomic_store_n(&g_flush_stopped, reason, __ATOMIC_RELAXED); /* the live-reload thread reads it */
    g_flush_failure = result;
    g_flush_failure_unlogged = 1;
}

static COLD void log_flush_failure(void) {
    Line line; begin(&line);
    if (g_flush_failure < 0) { put(&line, "hold flush: the swap chain's GetDevice(ID3D11Device) failed with "); put_hex(&line, (DWORD)g_flush_failure); }
    else put(&line, g_flush_stopped == FLUSH_STOPPED_NO_CONTEXT ? "hold flush: ID3D11Device::GetImmediateContext gave no context"
                                                                : "hold flush: the swap chain's GetDevice gave no device");
    put(&line, "; HoldFlush off for this session");
    emit(&line);
    g_flush_failure_unlogged = 0;
}

/* Returns the time after the Flush, from which the hold's wait goes on to the same target. */
static long long hold_flush(void *swapchain, long long arrived, long long target) {
    void *device = 0, *context = 0;
    long long flushing, flushed, done;
    HRESULT result = swap_chain_device(swapchain, &device);
    if (result < 0) device = 0; /* a failed call's output is not ours to release */
    if (device) ((GetImmediateContextFn)method(device, SLOT_GET_IMMEDIATE_CONTEXT))(device, &context);
    if (!context) {
        if (device) release(device);
        stop_flush(device ? FLUSH_STOPPED_NO_CONTEXT : FLUSH_STOPPED_NO_DEVICE, result);
        QueryPerformanceCounter(&done);
        return done;
    }
    QueryPerformanceCounter(&flushing);
    ((FlushFn)method(context, SLOT_FLUSH))(context);
    QueryPerformanceCounter(&flushed);
    release(context);
    release(device);
    QueryPerformanceCounter(&done);
    g_flushes++;
    if (done >= target) g_flush_late++;
    cost_add(&g_flush_cost, ticks_to_100ns(flushed - flushing));
    cost_add(&g_lookup_cost, ticks_to_100ns(flushing - arrived + (done - flushed)));
    /* The clamp keeps a rare long Flush (a load, deferred destruction) from inflating the estimate auto mode uses. */
    DWORD spent = done - arrived < g_ticks_per_ms ? (DWORD)(done - arrived) : g_ticks_per_ms;
    g_flush_typical_ticks = g_flushes == 1 ? spent : g_flush_typical_ticks - (g_flush_typical_ticks >> 3) + (spent >> 3);
    return done;
}

/* ---- live reload, the render thread's side ------------------------------------------------------------------------ */
/*
 * With LiveReload=1 the watcher thread (settings.c) re-reads VinceFix.ini when it changes. It does all the file I/O, the
 * display check and the logging, then publishes the new settings under g_reload_lock and bumps g_reload_seq. The render
 * thread only compares that number once per frame. When it moved, take_reload() copies the settings if it gets the lock
 * at once (else it tries again next frame: the watcher holds it only for the copy) and applies them before the frame is
 * held or timed, as at the first frame: auto mode starts over, FrameCap's grid starts at this frame and the stats line
 * counts from here.
 */
static void *g_reload_lock;     /* SRWLOCK over g_reload_settings, g_reload_refresh and g_reload_display_seq */
static Settings g_reload_settings;
static DWORD g_reload_refresh;  /* the refresh rate the watcher found for them (0 = unknown), or REFRESH_NOT_CHECKED */
static long g_reload_display_seq; /* g_display_seq as the watcher read it before that check */
static long g_reload_seq;       /* bumped under the lock for each published change; read without it once per frame */
static long g_reload_taken;     /* render thread: the g_reload_seq last applied */

void publish_settings(const Settings *asked, DWORD refresh, long display_seq) {
    AcquireSRWLockExclusive(&g_reload_lock);
    g_reload_settings = *asked;
    g_reload_refresh = refresh;
    g_reload_display_seq = display_seq;
    __atomic_store_n(&g_reload_seq, g_reload_seq + 1, __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&g_reload_lock);
}

static COLD void take_reload(void) {
    if (!TryAcquireSRWLockShared(&g_reload_lock)) return;
    Settings asked = g_reload_settings;
    DWORD refresh = g_reload_refresh;
    long display_seq = g_reload_display_seq;
    long seq = g_reload_seq;
    ReleaseSRWLockShared(&g_reload_lock);
    g_reload_taken = seq;
    g_asked = asked;
    /* Before the first frame, its setup (right after this) decides with the new g_asked. A change published just before
       that setup ran was not checked against the display; the render thread's own last check stands in, as it does when
       that check is newer than the watcher's (the display changed meanwhile, and the watcher may have seen the old one). */
    if (!g_setup_done) return;
    if (refresh == REFRESH_NOT_CHECKED || display_seq != g_display_seq) refresh = g_display_refresh;
    else __atomic_store_n(&g_display_refresh, refresh, __ATOMIC_RELAXED);
    apply_settings(refresh);
}

/* ---- the replacement Present -------------------------------------------------------------------------------------- */

/* Report the display, check the display rules, then run what they allow; the next frame starts afresh (not held, the
   grid starts there). The first frame does this before its real Present (nothing is held or timed yet); a frame whose
   display changed (`change` is DISPLAY_*) after it, so that the lines and the display query don't delay that frame. */
static COLD void run_display_rules(void *swapchain, int change) {
    HANDLE window;
    if (change == DISPLAY_NEW_SWAP_CHAIN) log_text("display: the game's swap chain changed; display rules checked again");
    if (change == DISPLAY_NEW_MONITOR) log_text("display: the game window moved to another monitor; display rules checked again");
    DWORD refresh = log_setup_report(swapchain, &window, change != DISPLAY_FIRST);
    log_display_rules(&g_asked, refresh);
    apply_settings(refresh);
    g_swap_chain = swapchain;
    g_display_frames = 0;
    __atomic_store_n(&g_game_window, window, __ATOMIC_RELAXED);
    __atomic_store_n(&g_display_refresh, refresh, __ATOMIC_RELAXED);
    __atomic_store_n(&g_display_seq, g_display_seq + 1, __ATOMIC_RELEASE);
}

static COLD void first_frame_setup(void *swapchain) {
    run_display_rules(swapchain, DISPLAY_FIRST);
    __atomic_store_n(&g_setup_done, 1, __ATOMIC_RELEASE); /* publishes the window and refresh rate to the live-reload thread */
}

/* Every DISPLAY_CHECK_FRAMES frames, or at once when the game presents through another swap chain: DISPLAY_SAME or
   what changed. */
static COLD int check_display(void *swapchain) {
    g_display_frames = 0;
    return swapchain != g_swap_chain ? DISPLAY_NEW_SWAP_CHAIN : display_change(swapchain);
}

/* A frame whose display changed goes straight to the real Present (the settings it would have been held under are about
   to change), and the display rules run after it. */
static COLD HRESULT present_then_recheck(void *swapchain, DWORD sync, DWORD flags, int change) {
    HRESULT result = ((PresentFn)method(swapchain, SLOT_PRESENT))(swapchain, sync, flags);
    run_display_rules(swapchain, change);
    return result;
}

/* Before the real Present of a frame that has a previous one: hold it until `previous` + the hold, flushing it first with
   HoldFlush. Sets *ready (when the frame could present: after its Flush) and *called (when Present gets called); returns
   1 if the frame was held. */
static int hold_frame(void *swapchain, long long previous, long long arrived, long long *ready, long long *called) {
    long long target = previous + g_hold_ticks;
    if (g_hold_ticks && arrived < target) {
        g_hold_held++;
        if (g_flush_on) *ready = hold_flush(swapchain, arrived, target);
        *called = wait_until(target, *ready);
        return 1;
    }
    g_hold_after++;
    /* Not flushed, as it is already late; held, it would have needed its Flush too. Counting the typical Flush time keeps
       auto's fast raise from first settling on the unflushed work of a heavier scene. */
    if (g_flush_on) *ready = arrived + g_flush_typical_ticks;
    return 0;
}

/* The stats line's counts for one frame: the time inside the real Present (`called` to `returned`); for a frame that
   has a previous one, from its start (`began`) to the real Present, and held, the hold's wait (`arrived` to `called`). */
static void count_present(int held, long long began, long long arrived, long long called, long long returned, long long previous) {
    long long inside = returned - called;
    cost_add(&g_present_cost, ticks_to_100ns(inside));
    g_present_frames++;
    if (held && inside < g_ticks_per_ms) g_hold_quick++; /* a frame slower than the cap leaves Present quickly anyway */
    if (!previous) return;
    latency_add(&g_start_to_present, called - began);
    if (held) latency_add(&g_hold_wait, called - arrived);
}

/* After the real Present with FrameCap on: its wait, unless it stepped aside (then the frame starts at Present's return,
   as under a driver cap), counted for the stats line and the step-aside detection. Sets the next frame's start. */
static void cap_frame(long long called, long long returned, long long previous) {
    if (g_cap_aside) g_hold_last = g_frame_began = returned;
    else {
        g_hold_last = cap_wait(returned);
        if (previous) { g_cap_wait_ticks += (unsigned long long)(g_frame_began - returned); g_cap_wait_frames++; }
    }
    if (previous) pace_sample(returned - called, g_frame_began - returned, g_hold_last - previous);
}

/* Replaces the game's swapchain->Present(SyncInterval, 0) call; render thread only. */
static HRESULT WINAPI held_present(void *swapchain, DWORD sync, DWORD flags) {
    if (__atomic_load_n(&g_reload_seq, __ATOMIC_ACQUIRE) != g_reload_taken) take_reload();
    if (!g_setup_done) first_frame_setup(swapchain);
    else if (swapchain != g_swap_chain || ++g_display_frames == DISPLAY_CHECK_FRAMES) {
        int change = check_display(swapchain);
        if (change != DISPLAY_SAME) return present_then_recheck(swapchain, sync, flags, change);
    }
    PresentFn present = (PresentFn)method(swapchain, SLOT_PRESENT); /* through the vtable, as the game does, so overlay hooks still run */
    if (!g_active) return present(swapchain, sync, flags);
    long long arrived, ready, called, returned, previous = g_hold_last, began = g_frame_began;
    QueryPerformanceCounter(&arrived);
    ready = called = arrived; /* ready: when the frame could present (after its Flush, if flushed) */
    int held = previous && hold_frame(swapchain, previous, arrived, &ready, &called);
    HRESULT result = present(swapchain, sync, flags);
    QueryPerformanceCounter(&returned);
    count_present(held, began, arrived, called, returned, previous);
    /* Set before the occasional stats write: the hold counts from the limiter's release, so the write uses the next
       frame's slack instead of moving its present off the frame-start spacing. */
    if (g_cap_ticks) cap_frame(called, returned, previous);
    else g_hold_last = g_frame_began = returned;
    /* Work counts from when the frame really began (so a late FrameCap wake isn't taken for work) to when it could
       present (so a Flush counts as work). Auto's cap detection still looks only at the real Present: a driver limiter
       that waited inside the Flush would look like no cap (none measured does; NVIDIA: Flush avg 0.12 ms). */
    if (g_hold_auto && previous) auto_sample(ready - began, returned - called, g_hold_last - previous);
    if (g_flush_failure_unlogged) log_flush_failure();
    if (++g_hold_frames == g_hold_next_log) { g_hold_next_log += HOLD_LOG_EVERY; log_hold_stats(); }
    return result;
}

/* ---- at load: patching the Present call --------------------------------------------------------------------------- */

/* Exactly one match of PRESENT_SITE in the host's executable sections, else 0. */
static BYTE *find_present_site(int *matches) {
    BYTE *base = (BYTE *)g_exe_base, *found = 0;
    const PeHeaders *nt = pe_headers(base);
    const PeSection *section = pe_sections(nt);
    *matches = 0;
    for (WORD i = 0; i < nt->section_count; i++, section++) {
        if (!(section->characteristics & PE_SECTION_EXECUTE)) continue;
        DWORD size = section->virtual_size ? section->virtual_size : section->raw_size;
        BYTE *code = base + section->virtual_address;
        for (DWORD j = 0; size >= sizeof PRESENT_SITE && j <= size - sizeof PRESENT_SITE; j++) {
            if (code[j] != PRESENT_SITE[0]) continue;
            unsigned k = 1;
            while (k < sizeof PRESENT_SITE && code[j + k] == PRESENT_SITE[k]) k++;
            if (k == sizeof PRESENT_SITE) { if (!found) found = code + j; ++*matches; }
        }
    }
    return *matches == 1 ? found : 0;
}

/* Points the game's Present call at held_present, once everything the hold and the cap need is there. Returns the
   patched address, or 0 after logging why not (`what` starts that line). DllMain only: the game isn't running yet. */
BYTE *patch_present_call(const char *what) {
    Line line; begin(&line);
    long long frequency = 0;
    QueryPerformanceFrequency(&frequency);
    if (frequency < 1000000 || frequency > 100000000) { /* keeps the 32-bit tick math in range */
        put(&line, what); put(&line, "unusual QueryPerformanceFrequency "); put_dec(&line, (unsigned long long)frequency, 0); put(&line, "; off");
        emit(&line); return 0;
    }
    int matches;
    BYTE *site = find_present_site(&matches);
    if (!site) {
        put(&line, what); put(&line, "the game's Present call was not found ("); put_dec(&line, (unsigned)matches, 0);
        put(&line, " matches; unknown Vince.exe version?); off"); emit(&line); return 0;
    }
    /* Without it (Windows 10 before 1803) every wait would be a busy-wait on the render thread. */
    g_hold_timer = CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_hold_timer) {
        put(&line, what); put(&line, "high-resolution timer unavailable (error "); put_dec(&line, GetLastError(), 0);
        put(&line, "; needs Windows 10 1803 or later); off"); emit(&line); return 0;
    }
    BYTE *patch = site + PRESENT_SITE_PATCH;
    DWORD protect;
    if (!VirtualProtect(patch, PRESENT_PATCH_SIZE, PAGE_EXECUTE_READWRITE, &protect)) {
        put(&line, what); put(&line, "VirtualProtect failed, error "); put_dec(&line, GetLastError(), 0); put(&line, "; off");
        emit(&line); CloseHandle(g_hold_timer); g_hold_timer = 0; return 0;
    }
    g_qpc_frequency = (DWORD)frequency;
    g_ticks_per_ms = (DWORD)frequency / 1000;
    g_spin_ticks = HOLD_SPIN_US * g_ticks_per_ms / 1000;
    g_bucket_scale = 0xFFFFFFFFu / (g_ticks_per_ms / 10) + 1; /* rounded up, so exactly 0.1 ms lands in bucket 1 */
    DWORD rel = (DWORD)((ULONG_PTR)held_present - (ULONG_PTR)(patch + PRESENT_PATCH_SIZE));
    patch[0] = X86_PUSH_EAX; /* the swap chain: Present's this */
    patch[1] = X86_CALL_REL32;
    for (int i = 0; i < 4; i++) patch[2 + i] = (BYTE)(rel >> (8 * i));
    VirtualProtect(patch, PRESENT_PATCH_SIZE, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), patch, PRESENT_PATCH_SIZE);
    return patch;
}
