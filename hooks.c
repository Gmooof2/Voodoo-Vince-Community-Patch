/*
 * Patching Vince.exe's imports, and the two fixes that need nothing else:
 *   Thread pinning: Vince.exe pins Main to CPU 0, WorkItem0-3 to CPUs 1-4 and both stream threads to CPU 4 through its
 *   import of kernel32!SetThreadAffinityMask. This replaces that one import slot in Vince.exe (not the kernel32
 *   function, so drivers and overlays in the process keep their own affinity handling) with a no-op that leaves the
 *   thread free to run on any CPU.
 *   DPI: the game ignores display scaling and renders below native resolution. This marks the process per-monitor DPI
 *   aware before the game creates its window, the same mode the "Override high DPI scaling: Application" compat setting
 *   gives.
 */
#include "vincefix.h"

IMPORT BOOL WINAPI GetProcessAffinityMask(HANDLE process, ULONG_PTR *process_mask, ULONG_PTR *system_mask);
IMPORT DWORD WINAPI GetThreadId(HANDLE thread);
IMPORT BOOL WINAPI SetProcessDPIAware(void); /* static user32 import: user32 initializes before this DLL */

/* ---- import patching ---------------------------------------------------------------------------------------------- */

static int names_equal(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* Points the host exe's import slot(s) for dll!function at replacement and returns the address the first patched slot
   held (0 if none was patched). `now` describes the replacement for the log. */
ULONG_PTR patch_import(const char *dll, const WCHAR *dll_wide, const char *function, void *replacement, const char *now) {
    BYTE *base = (BYTE *)g_exe_base;
    DWORD imports_rva = pe_headers(base)->directory[PE_IMPORT_DIRECTORY].rva;
    /* The loader resolves the exe's imports before running DLL initializers, so the slot should already hold this
       address; it is also how a slot is identified when the exe has no import-name table. */
    HMODULE module = GetModuleHandleW(dll_wide);
    ULONG_PTR resolved = module ? (ULONG_PTR)GetProcAddress(module, function) : 0;
    ULONG_PTR first_old = 0;
    Line line; begin(&line);
    for (const PeImport *d = (const PeImport *)(base + imports_rva); imports_rva && d->dll_name; d++) {
        if (!names_equal((const char *)(base + d->dll_name), dll)) continue;
        const DWORD *names = d->names ? (const DWORD *)(base + d->names) : 0; /* the slots are already resolved; never read names from them */
        ULONG_PTR *slots = (ULONG_PTR *)(base + d->slots);
        for (int i = 0; names ? names[i] != 0 : slots[i] != 0; i++) {
            int match = names ? !(names[i] & PE_IMPORT_BY_ORDINAL) && names_equal((const char *)(base + names[i] + PE_HINT_SIZE), function)
                              : resolved && slots[i] == resolved;
            if (!match) continue;
            ULONG_PTR old = slots[i];
            DWORD protect;
            if (VirtualProtect(&slots[i], sizeof slots[i], PAGE_READWRITE, &protect)) {
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

/* ---- thread pinning ----------------------------------------------------------------------------------------------- */

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

void unpin_threads(void) { patch_import("kernel32.dll", L"kernel32.dll", "SetThreadAffinityMask", (void *)no_affinity, "now a no-op"); }

/* ---- DPI ---------------------------------------------------------------------------------------------------------- */

#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE ((HANDLE)-3)
#define DPI_AWARENESS_PER_MONITOR 2 /* DPI_AWARENESS_PER_MONITOR_AWARE */
#define ERROR_ACCESS_DENIED 5       /* SetProcessDpiAwarenessContext: the awareness was set before */

/* The three are resolved at run time, as older Windows lacks them (Windows 10 1607 / 1703 added them); the statically
   imported SetProcessDPIAware is the fallback. */
void set_dpi_awareness(void) {
    typedef BOOL(WINAPI * SetContext)(HANDLE context);
    typedef HANDLE(WINAPI * GetContext)(void);
    typedef int(WINAPI * AwarenessOf)(HANDLE context);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    SetContext set_context = (SetContext)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    GetContext get_context = (GetContext)GetProcAddress(user32, "GetThreadDpiAwarenessContext");
    AwarenessOf awareness_of = (AwarenessOf)GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext");
    Line line; begin(&line);
    if (set_context) {
        if (set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) put(&line, "DPI: process set to per-monitor DPI aware");
        else {
            DWORD error = GetLastError();
            put(&line, error == ERROR_ACCESS_DENIED ? "DPI: awareness was already set (compat setting or manifest); left as is"
                                                    : "DPI: SetProcessDpiAwarenessContext failed, error ");
            if (error != ERROR_ACCESS_DENIED) put_dec(&line, error, 0);
        }
    } else {
        put(&line, SetProcessDPIAware() ? "DPI: process set to system DPI aware (older Windows)" : "DPI: SetProcessDPIAware failed");
    }
    emit(&line);
    /* Report what is actually in effect: a "System" or "System (Enhanced)" compat override keeps the process unaware even
       though the call above was refused politely. */
    if (get_context && awareness_of) {
        int awareness = awareness_of(get_context());
        begin(&line);
        put(&line, "DPI: effective awareness "); put_dec(&line, (unsigned)awareness, 0);
        put(&line, awareness == DPI_AWARENESS_PER_MONITOR ? " (per-monitor; game renders at native resolution)"
                   : " -- WARNING: not per-monitor; display scaling fix NOT active (another compatibility setting overrides it)");
        emit(&line);
    }
}
