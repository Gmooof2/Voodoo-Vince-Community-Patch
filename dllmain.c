/*
 * VinceFix: stand-in X3DAudio1_7.dll for Voodoo Vince Remastered (32-bit). What it does, file by file, is listed in
 * vincefix.h; DllMain below starts it in this order:
 *   1. the log (VinceFix.log next to this DLL, or in %TEMP% if that folder is not writable),
 *   2. DPI awareness, before the game creates its window (hooks.c),
 *   3. Vince.exe's import patches: thread unpinning (hooks.c) and the XAudio2 device-details cache (audio.c),
 *   4. VinceFix.ini, and with it the Present call's patch for the present hold, the frame cap and live reload
 *      (settings.c, present.c).
 * The X3DAudio exports themselves are thunks.s, which load the real DLL on first use (audio.c).
 * Built freestanding (no CRT, no SDK headers); see build.ps1.
 */
#include "vincefix.h"

IMPORT BOOL WINAPI DisableThreadLibraryCalls(HMODULE module);

#define DLL_PROCESS_ATTACH 1

int _fltused = 0; /* referenced by the compiler when floats appear; no CRT here */

HMODULE g_self;
ULONG_PTR g_exe_base;

static void install_hooks(void) {
    const BYTE *base = (const BYTE *)GetModuleHandleW(0);
    const PeHeaders *nt = pe_headers(base);
    if (nt->signature != PE_SIGNATURE || nt->magic != PE32_MAGIC) {
        log_text("hook: host is not a PE32 image; not installed"); return;
    }
    g_exe_base = (ULONG_PTR)base;
    unpin_threads();
    hook_xaudio();
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, void *reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        init_log_path();
        log_text("VinceFix 1.3 loaded"); /* also the install.ps1 marker */
        set_dpi_awareness();
        install_hooks();
        if (g_exe_base) setup_present_hold();
    }
    return 1;
}
