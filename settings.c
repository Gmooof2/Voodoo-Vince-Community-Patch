/*
 * VinceFix.ini (next to this DLL, section [VinceFix]): reading it at load, deciding what of it runs on the game's
 * display, and with LiveReload=1 the watcher thread that reads it again while the game runs.
 *
 * The settings are read at load. What runs also depends on the game's display, checked at the first frame and again
 * when the game's swap chain or monitor changes (present.c; the game presents with VSync, SyncInterval 1):
 *   - FrameCap is off on a display no faster than the cap + 1 Hz: VSync already limits the game to about that, and a
 *     second clock at the same rate would drift against the refresh and stutter.
 *   - PresentHold is off at 61 Hz or less: frames are shown on the display's refresh ticks anyway, so the hold would only
 *     add latency (and VSync's wait inside Present would look like a frame cap to auto mode).
 *   - HoldFlush=auto flushes only while FrameCap runs: under the NVIDIA cap the flush made the driver's frame starts less
 *     even (2026-10-14 A/B: frames shown within 0.5 ms 97.0 -> 93.1 %), with FrameCap it helped (96.9 -> 98.1 %).
 * An unknown refresh rate changes nothing. decide() turns the settings and the refresh rate into what runs, and
 * apply_settings() (present.c) sets the render thread's state to it.
 */
#include "vincefix.h"

IMPORT DWORD WINAPI GetPrivateProfileStringW(const WCHAR *section, const WCHAR *key, const WCHAR *fallback, WCHAR *out, DWORD size, const WCHAR *path);
IMPORT HANDLE WINAPI CreateThread(void *security, ULONG_PTR stack, DWORD(WINAPI *start)(void *), void *argument, DWORD flags, DWORD *id);
IMPORT HANDLE WINAPI FindFirstChangeNotificationW(const WCHAR *folder, BOOL subtree, DWORD filter);
IMPORT BOOL WINAPI FindNextChangeNotification(HANDLE change);
IMPORT BOOL WINAPI GetFileAttributesExW(const WCHAR *path, DWORD level, void *data);
IMPORT void WINAPI Sleep(DWORD milliseconds);

#define HOLD_MIN_REFRESH_HZ 61

int hold_asked(const Settings *s) { return s->hold_us || s->hold_auto; }

Settings decide(const Settings *asked, DWORD refresh) {
    Settings run;
    int hold = hold_asked(asked) && !(refresh && refresh <= HOLD_MIN_REFRESH_HZ);
    run.cap_fps = asked->cap_fps && !(refresh && refresh <= asked->cap_fps + 1) ? asked->cap_fps : 0;
    run.hold_us = hold ? asked->hold_us : 0;
    run.hold_auto = hold && asked->hold_auto;
    run.flush = hold && (asked->flush == FLUSH_ON || (asked->flush == FLUSH_AUTO && run.cap_fps)) ? FLUSH_ON : FLUSH_OFF;
    return run;
}

/* What the display changes about the settings, one line per setting it concerns. */
COLD void log_display_rules(const Settings *asked, DWORD refresh) {
    Settings run = decide(asked, refresh);
    int hold_runs = run.hold_us || run.hold_auto;
    Line line;
    if (asked->cap_fps) {
        begin(&line);
        put(&line, "frame cap: the game's display runs at "); put_dec(&line, refresh, 0);
        put(&line, !refresh ? " Hz (unknown); cap on" : run.cap_fps ? " Hz; cap on" : " Hz, so VSync already limits the game to about that; cap off");
        emit(&line);
    }
    if (hold_asked(asked) && !hold_runs) {
        begin(&line);
        put(&line, "present hold: the game's display runs at "); put_dec(&line, refresh, 0);
        put(&line, " Hz; at 61 Hz or less frames are shown on its refresh ticks anyway, so the hold would only add latency; hold off");
        emit(&line);
    } else if (hold_asked(asked) && !refresh) {
        log_text("present hold: the game's display refresh rate is unknown; hold left on as set");
    }
    if (hold_runs && asked->flush == FLUSH_AUTO && asked->cap_fps && !run.cap_fps)
        log_text("hold flush: off (FrameCap turned itself off at this display, and HoldFlush=auto flushes only with FrameCap)");
}

/* ---- reading VinceFix.ini ----------------------------------------------------------------------------------------- */

/* "5", "4.5", "4,25" -> microseconds (digits past the third decimal are ignored); -1 if the value is not a plain
   decimal number. Blanks around it and a trailing ; comment are allowed. */
static long parse_ms(const WCHAR *s) {
    long whole = 0, fraction = 0, scale = 1000;
    int digits = 0;
    while (*s == L' ' || *s == L'\t') s++;
    for (; *s >= L'0' && *s <= L'9'; s++, digits++) { if (whole > 100000) return -1; whole = whole * 10 + (*s - L'0'); }
    if (*s == L'.' || *s == L',') for (s++; *s >= L'0' && *s <= L'9'; s++, digits++) { scale /= 10; fraction += (*s - L'0') * scale; }
    while (*s == L' ' || *s == L'\t') s++;
    if (!digits || (*s && *s != L';')) return -1;
    return whole * 1000 + fraction;
}

/* "auto" in any case, with the same blanks and ; comment allowance. */
static int is_auto(const WCHAR *s) {
    while (*s == L' ' || *s == L'\t') s++;
    for (const char *word = "auto"; *word; word++, s++) if ((*s | 0x20) != *word) return 0;
    while (*s == L' ' || *s == L'\t') s++;
    return !*s || *s == L';';
}

/* 0/1 switches: 0 or 1 (as a number, so "1.0" and "01" count too) -> 0 or 1; empty -> 0; anything else -1. */
static int parse_flag(const WCHAR *s) {
    long thousandths = *s ? parse_ms(s) : 0; /* parse_ms reads "1" as 1000 */
    return thousandths == 0 ? 0 : thousandths == 1000 ? 1 : -1;
}

/* The [VinceFix] values of VinceFix.ini as text, all read at once by read_ini(), then turned into settings below. */
typedef struct { WCHAR text[32]; int found; } IniValue; /* found: the file has the line (text may still be empty) */
typedef struct { IniValue hold, cap, flush, live; } IniValues;

#define FILE_SHARE_DELETE 0x4

static void read_key(const WCHAR *path, const WCHAR *key, IniValue *value) {
    GetPrivateProfileStringW(L"VinceFix", key, L"<none>", value->text, 32, path);
    value->found = value->text[0] != L'<';
}
/* VinceFix.ini, open for reading while others may still read, write, replace or delete it. */
static HANDLE open_ini(const WCHAR *path) {
    return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
}
/* Reads the four keys, 0 when the file can't be opened (missing, or an editor holds it without letting others read).
   GetPrivateProfileStringW opens the file again for every key and reads a key as missing when it can't, so VinceFix's
   own handle stays open meanwhile: an editor can't take the file for itself between the keys (which would read as
   "everything off"). A UTF-8 BOM in front of [VinceFix] hides the section; the shipped file starts with a comment line. */
static COLD int read_ini(const WCHAR *path, IniValues *values) {
    HANDLE held = open_ini(path);
    if (held == INVALID_HANDLE) return 0;
    zero(values, sizeof *values); /* so that same_values() can compare the whole buffers */
    read_key(path, L"PresentHold", &values->hold);
    read_key(path, L"FrameCap", &values->cap);
    read_key(path, L"HoldFlush", &values->flush);
    read_key(path, L"LiveReload", &values->live);
    CloseHandle(held);
    return 1;
}
static int same_values(const IniValues *a, const IniValues *b) { return same_bytes(a, b, sizeof *a); }

/* ---- what the values mean ----------------------------------------------------------------------------------------- */

/* "<setting><value> in VinceFix.ini is not <expected>", e.g. "frame cap: FrameCap=" "61.5" "... a whole number ...; off". */
static COLD void log_bad_value(const char *setting, const WCHAR *value, const char *expected) {
    Line line; begin(&line);
    put(&line, setting); put_wide(&line, value); put(&line, " in VinceFix.ini is not "); put(&line, expected);
    emit(&line);
}
/* The settings below log what they make of a value only when `verbose` (at load, and on a reload whose values changed). */
static void note(int verbose, const char *text) { if (verbose) log_text(text); }
static void note_bad_value(int verbose, const char *setting, const WCHAR *value, const char *expected) { if (verbose) log_bad_value(setting, value, expected); }

/* PresentHold: sets s->hold_us (a fixed hold) or s->hold_auto; says why the hold is off otherwise. */
static void hold_setting(const IniValue *v, Settings *s, int verbose) {
    s->hold_us = 0;
    s->hold_auto = 0;
    if (!v->found) { note(verbose, "present hold: off (VinceFix.ini has no PresentHold line under [VinceFix])"); return; }
    if (is_auto(v->text)) { s->hold_auto = 1; return; }
    long us = v->text[0] ? parse_ms(v->text) : 0;
    if (us == 0) { note(verbose, v->text[0] ? "present hold: off (PresentHold=0)" : "present hold: off (PresentHold is empty)"); return; }
    if (us < HOLD_MIN_US || us > HOLD_MAX_US) { note_bad_value(verbose, "present hold: PresentHold=", v->text, "a number from 1 to 12 or auto; off"); return; }
    s->hold_us = (DWORD)us;
}

/* FrameCap: frames per second (a whole number from 20 to 500), 0 when off. */
#define CAP_MIN_FPS 20
#define CAP_MAX_FPS 500
static DWORD cap_setting(const IniValue *v, int verbose) {
    if (!v->found) { note(verbose, "frame cap: off (no FrameCap line)"); return 0; }
    long thousandths = v->text[0] ? parse_ms(v->text) : 0; /* parse_ms reads "60" as 60000 */
    if (thousandths == 0) { note(verbose, "frame cap: off (FrameCap=0 or empty)"); return 0; }
    if (thousandths < CAP_MIN_FPS * 1000 || thousandths > CAP_MAX_FPS * 1000 || thousandths % 1000) {
        note_bad_value(verbose, "frame cap: FrameCap=", v->text, "a whole number from 20 to 500; off");
        return 0;
    }
    return (DWORD)(thousandths / 1000);
}

/* HoldFlush: FLUSH_ON for 1, FLUSH_OFF for 0 or empty (said), FLUSH_AUTO for auto, no line, or anything else (said). */
static int flush_setting(const IniValue *v, int verbose) {
    if (!v->found || is_auto(v->text)) return FLUSH_AUTO;
    int flag = parse_flag(v->text);
    if (flag == 1) return FLUSH_ON;
    if (flag == 0) { note(verbose, "hold flush: off (HoldFlush=0 or empty)"); return FLUSH_OFF; }
    note_bad_value(verbose, "hold flush: HoldFlush=", v->text, "0, 1 or auto; using auto");
    return FLUSH_AUTO;
}

/* At load and on each live reload. */
static COLD void settings_from(const IniValues *values, Settings *s, int verbose) {
    hold_setting(&values->hold, s, verbose);
    s->cap_fps = cap_setting(&values->cap, verbose);
    s->flush = flush_setting(&values->flush, verbose);
}

/* With both a hold and a cap: the cap's frame time, and a warning when the hold leaves less than 2 ms of it. */
static void log_cap_with_hold(const Settings *s) {
    if (!hold_asked(s) || !s->cap_fps) return;
    Line line; begin(&line);
    put(&line, "frame cap: "); put_dec(&line, s->cap_fps, 0); put(&line, " FPS (frame starts "); put_ms(&line, 1000000 / s->cap_fps); put(&line, " ms apart)");
    emit(&line);
    if (s->hold_us && s->hold_us + HOLD_HEADROOM_US > 1000000 / s->cap_fps)
        log_text("present hold: WARNING: PresentHold is less than 2 ms below the frame cap's frame time; lower it or use auto");
}

/* The HoldFlush decision as far as the settings go (the first frame's display check may still turn FrameCap off). */
static void log_flush_decision(const Settings *s) {
    const char *text = 0;
    if (!hold_asked(s)) {
        if (s->flush == FLUSH_ON) text = "hold flush: HoldFlush=1 has no effect while PresentHold is off (it flushes only before a hold)";
    } else if (s->flush == FLUSH_ON) {
        text = "hold flush: on (HoldFlush=1: the game's D3D11 immediate context is flushed before each held frame)";
    } else if (s->flush == FLUSH_AUTO) {
        text = s->cap_fps ? "hold flush: on (HoldFlush=auto and FrameCap is on: the game's D3D11 immediate context is flushed before each held frame)"
                          : "hold flush: off (HoldFlush=auto flushes only with FrameCap; with a driver frame cap it made frame pacing worse)";
    }
    if (text) log_text(text);
}

/* LiveReload: 1 for on; 0, empty, no line or anything else is off (logged). Read at load only. */
static int live_setting(const IniValue *v) {
    if (!v->found) { log_text("live reload: off (no LiveReload line)"); return 0; }
    int flag = parse_flag(v->text);
    if (flag == 1) return 1;
    if (flag == 0) log_text("live reload: off (LiveReload=0 or empty)");
    else log_bad_value("live reload: LiveReload=", v->text, "0 or 1; off");
    return 0;
}

/* ---- live reload, the watcher thread ------------------------------------------------------------------------------ */
/*
 * Waits for a change in the DLL's folder (or 1 s, since change notifications can lag behind cached writes), then
 * compares VinceFix.ini's last-write time and size with the last version it read. Our own log writes wake it too, but
 * leave the ini's stamp alone. An edit counts as finished once the file stayed the same for WATCH_SETTLE_MS (editors may
 * write it in several steps, or replace it through a temporary file). Then the settings are read and, when their text
 * changed, logged as at load; a change is checked against the game's display and handed to the render thread with
 * publish_settings() (present.c). One "settings reloaded:" line per finished edit sums it up.
 */
#define WATCH_POLL_MS 1000
#define WATCH_SETTLE_MS 200
#define FILE_NOTIFY_CHANGE_FILE_NAME 0x1
#define FILE_NOTIFY_CHANGE_SIZE 0x8
#define FILE_NOTIFY_CHANGE_LAST_WRITE 0x10
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 0x102
#define GET_FILE_EX_INFO_STANDARD 0
typedef struct { DWORD attributes, created[2], accessed[2], written[2], size_high, size_low; } FileAttributeData; /* WIN32_FILE_ATTRIBUTE_DATA */
_Static_assert(sizeof(FileAttributeData) == 36 && offset_of(FileAttributeData, written) == 20 && offset_of(FileAttributeData, size_low) == 32,
               "WIN32_FILE_ATTRIBUTE_DATA layout");
typedef struct { DWORD written_low, written_high, size_low, size_high; } IniStamp; /* last-write time and size */

static WCHAR g_ini_path[520];   /* VinceFix.ini; set at load */
static IniStamp g_loaded_stamp; /* taken at load before the settings were read, so an edit during the read still counts */
/* Watcher thread only (set at load before it starts): the values it last read and logged, and what it last published. */
static IniValues g_watched_values;
static Settings g_watched;

static int ini_stamp(IniStamp *stamp) { /* 0 if the file is not there */
    FileAttributeData data;
    if (!GetFileAttributesExW(g_ini_path, GET_FILE_EX_INFO_STANDARD, &data)) return 0;
    stamp->written_low = data.written[0];
    stamp->written_high = data.written[1];
    stamp->size_low = data.size_low;
    stamp->size_high = data.size_high;
    return 1;
}
static int same_stamp(const IniStamp *a, const IniStamp *b) {
    return a->written_low == b->written_low && a->written_high == b->written_high && a->size_low == b->size_low && a->size_high == b->size_high;
}
static int same_settings(const Settings *a, const Settings *b) {
    return a->hold_us == b->hold_us && a->hold_auto == b->hold_auto && a->cap_fps == b->cap_fps && a->flush == b->flush;
}

/* What `asked` runs at `refresh`: "frame cap 60 FPS, hold 4.00 ms, hold flush", or "nothing; ...". */
static void put_running(Line *line, const Settings *asked, DWORD refresh) {
    Settings run = decide(asked, refresh);
    const char *next = "";
    if (run.cap_fps) { put(line, "frame cap "); put_dec(line, run.cap_fps, 0); put(line, " FPS"); next = ", "; }
    if (run.hold_auto) { put(line, next); put(line, "auto hold"); next = ", "; }
    else if (run.hold_us) { put(line, next); put(line, "hold "); put_ms(line, run.hold_us); put(line, " ms"); next = ", "; }
    if (run.flush == FLUSH_ON && !__atomic_load_n(&g_flush_stopped, __ATOMIC_RELAXED)) put(line, ", hold flush"); /* only with a hold */
    if (!*next) put(line, "nothing; Present passes straight through");
}

/* "settings reloaded: FrameCap=60, PresentHold=4.00 ms, HoldFlush=auto (now running: ...)" */
static void log_reload_summary(const Settings *asked, int changed, int setup_done, DWORD refresh) {
    Line line; begin(&line);
    put(&line, "settings reloaded: FrameCap="); put_dec(&line, asked->cap_fps, 0); put(&line, ", PresentHold=");
    if (asked->hold_auto) put(&line, "auto");
    else if (asked->hold_us) { put_ms(&line, asked->hold_us); put(&line, " ms"); }
    else put(&line, "off");
    put(&line, ", HoldFlush="); put(&line, asked->flush == FLUSH_ON ? "1" : asked->flush == FLUSH_OFF ? "0" : "auto");
    put(&line, changed ? " (" : " (unchanged; ");
    if (setup_done) { put(&line, "now running: "); put_running(&line, asked, refresh); }
    else put(&line, "the game has not shown a frame yet; decided at its first one");
    put(&line, ")");
    emit(&line);
}

/* Reads VinceFix.ini again and publishes a change (checked against the game's display, once it has shown a frame and
   something is asked that the display concerns). The values are logged as at load only when their text changed, so a
   save that changed nothing else in the file logs just the summary. Returns 0, with nothing logged or published, when
   the file could not be read whole: it can't be opened, or its stamp moved from `settled` during the read (the next
   check then reads it again). */
static COLD int reload_settings(const IniStamp *settled) {
    IniValues values;
    IniStamp after;
    if (!read_ini(g_ini_path, &values) || !ini_stamp(&after) || !same_stamp(&after, settled)) return 0;
    Settings asked;
    int reread = !same_values(&values, &g_watched_values);
    settings_from(&values, &asked, reread);
    if (reread) {
        log_cap_with_hold(&asked);
        log_flush_decision(&asked);
        copy_bytes(&g_watched_values, &values, sizeof values);
    }
    int setup_done = __atomic_load_n(&g_setup_done, __ATOMIC_ACQUIRE);
    int changed = !same_settings(&asked, &g_watched);
    DWORD refresh = REFRESH_NOT_CHECKED;
    if (changed) {
        long display_seq = __atomic_load_n(&g_display_seq, __ATOMIC_ACQUIRE); /* before the window it goes with */
        if (setup_done && (asked.cap_fps || hold_asked(&asked))) {
            refresh = display_refresh(__atomic_load_n(&g_game_window, __ATOMIC_RELAXED));
            log_display_rules(&asked, refresh);
        }
        publish_settings(&asked, refresh, display_seq);
        g_watched = asked;
    }
    /* What the render thread decides with, as take_reload() does (the refresh rate it runs at, unless checked here). */
    log_reload_summary(&asked, changed, setup_done, refresh != REFRESH_NOT_CHECKED ? refresh : __atomic_load_n(&g_display_refresh, __ATOMIC_RELAXED));
    return 1;
}

static DWORD WINAPI watch_ini(void *unused) {
    (void)unused;
    WCHAR folder[520];
    self_dir(folder, 520);
    HANDLE change = FindFirstChangeNotificationW(folder, 0, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
    int notified = change != INVALID_HANDLE; /* else (some network folders) or once the handle fails: poll */
    IniStamp done = g_loaded_stamp, now, seen;
    for (;;) {
        if (notified) {
            DWORD woke = WaitForSingleObject(change, WATCH_POLL_MS);
            /* If the handle stops working, poll from now on rather than spin on it (left open: it only failed). */
            if (woke != WAIT_TIMEOUT && (woke != WAIT_OBJECT_0 || !FindNextChangeNotification(change))) notified = 0;
        }
        if (!notified) Sleep(WATCH_POLL_MS / 2);
        if (!ini_stamp(&now) || same_stamp(&now, &done)) continue;
        int there;
        do { seen = now; Sleep(WATCH_SETTLE_MS); there = ini_stamp(&now); } while (there && !same_stamp(&now, &seen));
        /* Gone for now (replaced through a temporary file), locked or changing: `done` is unchanged, so a later check retries. */
        if (there && reload_settings(&now)) done = now;
    }
}

/* At load, after the patch: start the watcher with the values, settings and stamp just read. */
static void start_live_reload(const IniStamp *loaded, const IniValues *values, const Settings *asked) {
    g_loaded_stamp = *loaded;
    copy_bytes(&g_watched_values, values, sizeof *values);
    g_watched = *asked;
    HANDLE thread = CreateThread(0, 0, watch_ini, 0, 0, 0);
    DWORD error = GetLastError();
    if (thread) {
        CloseHandle(thread);
        log_text("live reload: on (changes to FrameCap, PresentHold and HoldFlush in VinceFix.ini apply while the game runs)");
        return;
    }
    Line line; begin(&line);
    put(&line, "live reload: could not start the watcher thread (error "); put_dec(&line, error, 0); put(&line, "); settings stay as loaded");
    emit(&line);
}

/* ---- at load ------------------------------------------------------------------------------------------------------ */

/* Reads VinceFix.ini and, if anything is on (or LiveReload=1), patches the Present call and starts running the
   settings (until the first frame checks the display). */
void setup_present_hold(void) {
    IniStamp loaded;
    IniValues values;
    self_dir(g_ini_path, 520);
    int named = g_ini_path[0] && append_name(g_ini_path, 520, L"VinceFix.ini");
    /* The stamp comes first, so an edit during the read still counts for the watcher. */
    if (!named || !ini_stamp(&loaded)) zero(&loaded, sizeof loaded); /* then the watcher's first check reads the file once more */
    if (!named || !read_ini(g_ini_path, &values)) { log_text("present hold: off (no VinceFix.ini next to the DLL)"); return; }
    Settings asked;
    settings_from(&values, &asked, 1);
    int live = live_setting(&values.live);
    int hold = hold_asked(&asked);
    if (!hold) log_flush_decision(&asked);
    if (!hold && !asked.cap_fps && !live) return;
    BYTE *patch = patch_present_call(hold ? "present hold: " : asked.cap_fps ? "frame cap: " : "live reload: ");
    if (!patch) {
        if (live) log_text("live reload: off (the Present call could not be patched)");
        return;
    }
    g_asked = asked;
    apply_settings(0); /* the refresh rate is not known yet */
    log_cap_with_hold(&asked);
    Line line; begin(&line);
    if (hold && asked.hold_auto) put(&line, "present hold: auto");
    else if (hold) { put(&line, "present hold: "); put_ms(&line, asked.hold_us); put(&line, " ms"); }
    else if (asked.cap_fps) { put(&line, "frame cap: "); put_dec(&line, asked.cap_fps, 0); put(&line, " FPS"); }
    else put(&line, "live reload: FrameCap and PresentHold are off for now");
    put(&line, "; patched Present call at +"); put_hex(&line, (ULONG_PTR)patch - g_exe_base);
    emit(&line);
    if (hold) log_flush_decision(&asked);
    if (live) start_live_reload(&loaded, &values, &asked);
}
