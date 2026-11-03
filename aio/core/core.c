/*
 * AIO core: X3DAudio1_7.dll for Voodoo Vince Remastered (32-bit), built by `aio/build.py dll` from the three 1.3 trees
 * compiled with -DVINCE_AIO (their own DllMains, duplicate CRT stubs and exports compiled out):
 *   VinceFix   (VinceFix/release/github-1.3): log, DPI, thread unpinning, present hold; X3DAudio exports (thunks.s),
 *              forwarded to the system xaudio2_9.dll, else the system X3DAudio1_7.dll (audio.c)
 *   VinceAudio (Xaudio2 rework/src/adapter): create(), the one CoCreateInstance dispatcher on Vince.exe +0x2D63D0
 *   VinceSteam (VinceSteam/src): the heat shimmer install()
 * This file is the one DllMain; order: docs/aio/DLL_LAYOUT.md:168-170. No LoadLibrary under the loader lock: the
 * forwarding targets and the XAudio2 backend load later, on first use. Each subsystem keeps its own log; the settings
 * are one VinceAIO.ini next to this DLL (1.4 T7), a section per subsystem ([VinceFix], [VinceAudio], [VinceSteam]).
 */
#include "kernel32.h"

#define DLL_PROCESS_ATTACH 1
#define AIO_HOST_SIZE 0x87A000u /* SizeOfImage of the AIO Vince.exe and up (VinceAudio loader.c:24) */
#define COCREATE_SLOT 0x2D63D0u /* the dispatcher's import slot (VinceAudio loader.c:30) */

/* VinceFix (vincefix.h) */
extern void *g_self;
extern unsigned long g_exe_base;
void init_log_path(void);
void log_text(const char *text);
void set_dpi_awareness(void);
void unpin_threads(void);
void setup_present_hold(void);
void vincefix_set_ini(const u16 *path); /* settings.c, VINCE_AIO */
/* VinceAudio (bridge_engine.c, loader.c) */
void va_set_module(void *module);
int va_install(void *image);
void va_read_ini(const u16 *path); /* bridge_engine.c, VINCE_AIO: MasterVolume, read once */
/* VinceSteam (vincesteam.c) */
void vincesteam_attach(const u16 *ini);
/* Halo (halo.c, 1.6 T4) */
void halo_attach(u8 *image);

/* <this DLL's folder>VinceAIO.ini in ini[cap]; "" when the name fails, is truncated or has no folder (then every
   subsystem runs on its defaults). */
static void ini_path(void *module, u16 *ini, u32 cap) {
    static const char name[] = "VinceAIO.ini";
    u32 n = GetModuleFileNameW(module, ini, cap);
    if (n >= cap) n = 0;
    while (n && ini[n - 1] != '\\') n--;
    if (n + sizeof name > cap) n = 0;
    if (n) for (u32 i = 0; i < sizeof name; i++) ini[n + i] = (u16)name[i];
    else ini[0] = 0;
}

static int same_text(const char *a, const char *b, int fold) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (fold && x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (fold && y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* Read only: the RVA of the host's ole32!CoCreateInstance import slot (by name, through the import directory), 0 if none. */
static u32 cocreate_slot(const u8 *image, u32 pe) {
    u32 dir = *(const u32 *)(image + pe + 24 + 96 + 8); /* data directory 1 */
    if (!dir) return 0;
    /* IMAGE_IMPORT_DESCRIPTOR: OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name, FirstThunk */
    for (const u32 *d = (const u32 *)(image + dir); d[3]; d += 5) {
        if (!d[0] || !same_text((const char *)(image + d[3]), "ole32.dll", 1)) continue;
        const u32 *names = (const u32 *)(image + d[0]);
        for (u32 i = 0; names[i]; i++)
            if (!(names[i] & 0x80000000u) && same_text((const char *)(image + names[i] + 2), "CoCreateInstance", 0))
                return d[4] + 4 * i;
    }
    return 0;
}

int WINAPI DllMain(void *module, u32 reason, void *reserved) {
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return 1;
    /* 1. VinceFix's log */
    g_self = module;
    DisableThreadLibraryCalls(module);
    u16 ini[520]; /* VinceAIO.ini, handed to each subsystem before it runs */
    ini_path(module, ini, 520);
    init_log_path();
    log_text("VinceFix 1.3 loaded"); /* also the install.ps1 marker */
    /* 2. DPI awareness, before the game creates its window */
    set_dpi_awareness();
    /* 3. the host: PE32 check, thread unpinning (VinceFix's install_hooks without its CoCreateInstance hook) */
    u8 *image = GetModuleHandleW(0);
    u32 pe = 0, size = 0;
    if (*(const u16 *)image == 0x5A4D) pe = *(const u32 *)(image + 0x3C);
    if (!pe || *(const u32 *)(image + pe) != 0x4550 || *(const u16 *)(image + pe + 24) != 0x10B) {
        log_text("hook: host is not a PE32 image; not installed");
    } else {
        g_exe_base = (unsigned long)image;
        size = *(const u32 *)(image + pe + 24 + 56);
        unpin_threads();
    }
    /* 4. the CoCreateInstance dispatcher: VinceAudio's create() (XAudio2 2.7 -> VinceAudio, all else -> ole32) */
    if (size >= AIO_HOST_SIZE && cocreate_slot(image, pe) != COCREATE_SLOT) {
        log_text("core: Vince.exe's ole32!CoCreateInstance import slot is not at +0x2D63D0; not loaded");
        return 0;
    }
    va_set_module(module);
    va_read_ini(ini); /* [VinceAudio] MasterVolume, logged by every create() */
    if (!va_install(image)) { /* refusal kept (loader.c:25-29): wrong code at +0x5CDBF */
        log_text("core: VinceAudio refused this Vince.exe (+0x5CDBF); not loaded");
        return 0;
    }
    /* 5. VinceSteam */
    if (size >= AIO_HOST_SIZE) vincesteam_attach(ini);
    else log_text("core: host SizeOfImage is below 0x87A000; VinceSteam not installed");
    /* 6. halo (1.6): VoodooPowerFix's legacy pickup halo, only when SpookyPS9 is VoodooPowerFix's (C0033: AIO host only) */
    if (size >= AIO_HOST_SIZE) halo_attach(image);
    /* 7. VinceFix's present hold, frame cap and live reload ([VinceFix] of VinceAIO.ini) */
    vincefix_set_ini(ini);
    if (g_exe_base) setup_present_hold();
    return 1;
}
