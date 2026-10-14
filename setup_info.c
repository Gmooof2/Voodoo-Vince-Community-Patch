/*
 * The setup report, logged at the game's first frame: four lines, so that a tester's log shows what frame pacing
 * depends on: the GPU and its driver ("gpu:"), the display path ("display:": refresh rate, swap chain model, which GPU
 * drives the display), the power state ("power:") and the Windows build ("os:"). They contain no names or paths. Every
 * failed call is logged and skipped, and every reference is released at once.
 * Also the display's refresh rate, which decides what of the settings runs (settings.c), and the cheap check present.c
 * runs about once a second for a changed display: when the game recreates its swap chain (a resolution or display mode
 * change) or its window moves to another monitor, the refresh rate may differ, so the report's display: line (and the
 * gpu: line, if the game now renders on another adapter) is logged again and the display rules run again.
 */
#include "vincefix.h"

IMPORT HANDLE WINAPI MonitorFromWindow(HANDLE window, DWORD flags);
IMPORT BOOL WINAPI GetMonitorInfoW(HANDLE monitor, void *info);
IMPORT BOOL WINAPI EnumDisplaySettingsW(const WCHAR *device, DWORD mode, void *devmode);

/* ---- the game's display ------------------------------------------------------------------------------------------- */

typedef struct { long left, top, right, bottom; } Rect;
typedef struct {            /* MONITORINFOEXW (winuser.h) */
    DWORD size;             /* cbSize, set by the caller */
    Rect monitor, work;
    DWORD flags;
    WCHAR device[32];       /* szDevice, the display's name such as \\.\DISPLAY1 */
} MonitorInfoEx;
_Static_assert(sizeof(MonitorInfoEx) == 104 && offset_of(MonitorInfoEx, device) == 40, "MONITORINFOEXW layout");
typedef struct {            /* DEVMODEW (wingdi.h); only the fields VinceFix uses are named */
    WCHAR device_name[32];
    WORD spec_version, driver_version, size, driver_extra; /* dmSize: the struct's size, set by the caller */
    DWORD fields;
    BYTE position_to_pels_height[108];                     /* dmPosition .. dmPelsHeight */
    DWORD display_frequency;                               /* dmDisplayFrequency, Hz */
    BYTE icm_to_panning[32];                               /* dmICMMethod .. dmPanningHeight */
} DevMode;
_Static_assert(sizeof(DevMode) == 220 && offset_of(DevMode, size) == 68 && offset_of(DevMode, display_frequency) == 184,
               "DEVMODEW layout");
#define MONITOR_DEFAULTTONEAREST 2
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)

/* The current refresh rate of `monitor`, 0 when unknown. Any thread. */
static DWORD monitor_refresh(HANDLE monitor) {
    MonitorInfoEx info;
    DevMode mode;
    zero(&info, sizeof info);
    zero(&mode, sizeof mode);
    info.size = sizeof info;
    mode.size = sizeof mode;
    if (!monitor || !GetMonitorInfoW(monitor, &info) || !EnumDisplaySettingsW(info.device, ENUM_CURRENT_SETTINGS, &mode)) return 0;
    return mode.display_frequency > 1 ? mode.display_frequency : 0; /* 0 and 1 mean "hardware default" (some virtual and remote displays) */
}
static HANDLE window_monitor(HANDLE window) { return MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST); }

/* The current refresh rate of the display that shows `window` (the game's), 0 when unknown. Any thread. */
DWORD display_refresh(HANDLE window) { return monitor_refresh(window_monitor(window)); }

/* ---- DXGI: the slots and structures the report uses (counted in vincefix.h) ----------------------------------------- */

#define SLOT_GET_PARENT 6              /* IDXGIObject; used on an IDXGIOutput, whose parent is its adapter */
#define SLOT_SWAP_CHAIN_GET_DESC 12    /* IDXGISwapChain */
#define SLOT_GET_CONTAINING_OUTPUT 15  /* IDXGISwapChain */
#define SLOT_GET_ADAPTER 7             /* IDXGIDevice */
#define SLOT_ADAPTER_GET_DESC 8        /* IDXGIAdapter */
#define SLOT_CHECK_INTERFACE_SUPPORT 9 /* IDXGIAdapter */

static const GUID IID_IDXGIDevice = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
static const GUID IID_IDXGIAdapter = {0x2411e7e1, 0x12ac, 0x4ccf, {0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0}};

typedef struct { DWORD low; long high; } Luid; /* LUID */
typedef struct {                               /* DXGI_ADAPTER_DESC; SIZE_T is 4 bytes in this 32-bit game */
    WCHAR description[128];
    DWORD vendor_id, device_id, subsys_id, revision;
    ULONG_PTR dedicated_video_memory, dedicated_system_memory, shared_system_memory;
    Luid adapter_luid;
} DxgiAdapterDesc;
_Static_assert(sizeof(DxgiAdapterDesc) == 292 && offset_of(DxgiAdapterDesc, vendor_id) == 256 &&
                   offset_of(DxgiAdapterDesc, dedicated_video_memory) == 272 && offset_of(DxgiAdapterDesc, adapter_luid) == 284,
               "DXGI_ADAPTER_DESC layout");
typedef struct {                                /* DXGI_SWAP_CHAIN_DESC */
    DWORD width, height, refresh_numerator, refresh_denominator, format, scanline_ordering, scaling; /* BufferDesc */
    DWORD sample_count, sample_quality;                                                           /* SampleDesc */
    DWORD buffer_usage, buffer_count;
    HANDLE output_window;
    BOOL windowed;
    DWORD swap_effect, flags;
} DxgiSwapChainDesc;
_Static_assert(sizeof(DxgiSwapChainDesc) == 60 && offset_of(DxgiSwapChainDesc, buffer_count) == 40 &&
                   offset_of(DxgiSwapChainDesc, output_window) == 44 && offset_of(DxgiSwapChainDesc, swap_effect) == 52,
               "DXGI_SWAP_CHAIN_DESC layout");
enum { SWAP_EFFECT_DISCARD = 0, SWAP_EFFECT_SEQUENTIAL = 1, SWAP_EFFECT_FLIP_SEQUENTIAL = 3, SWAP_EFFECT_FLIP_DISCARD = 4 };

typedef HRESULT(WINAPI *SwapChainGetDescFn)(void *swapchain, DxgiSwapChainDesc *desc);
typedef HRESULT(WINAPI *GetContainingOutputFn)(void *swapchain, void **output);
typedef HRESULT(WINAPI *GetAdapterFn)(void *dxgi_device, void **adapter);
typedef HRESULT(WINAPI *AdapterGetDescFn)(void *adapter, DxgiAdapterDesc *desc);
typedef HRESULT(WINAPI *CheckInterfaceSupportFn)(void *adapter, const GUID *iid, long long *driver_version);

/* ---- power and Windows -------------------------------------------------------------------------------------------- */

typedef struct {                    /* SYSTEM_POWER_STATUS (winbase.h) */
    BYTE ac_line_status;            /* 0 battery, 1 AC, 255 unknown */
    BYTE battery_flag;              /* bit 128: no system battery */
    BYTE battery_life_percent;      /* 0-100, 255 unknown */
    BYTE system_status_flag;        /* bit 1: battery saver on */
    DWORD battery_life_time, battery_full_life_time;
} SystemPowerStatus;
_Static_assert(sizeof(SystemPowerStatus) == 12, "SYSTEM_POWER_STATUS layout");
IMPORT BOOL WINAPI GetSystemPowerStatus(SystemPowerStatus *status);
#define AC_LINE_OFFLINE 0
#define AC_LINE_ONLINE 1
#define BATTERY_FLAG_NO_BATTERY 128
#define SYSTEM_STATUS_BATTERY_SAVER 1

/* KUSER_SHARED_DATA (ntddk.h) is the page Windows maps read-only into every process at 0x7FFE0000; its version fields
   start at 0x260 (NtBuildNumber, filled since Windows 10). Checked against ntdll!RtlGetVersion on this PC. */
#define KUSER_SHARED_DATA 0x7FFE0000u
#define KUSER_VERSION_FIELDS 0x260
typedef struct {
    DWORD nt_build_number;          /* 0x260 */
    DWORD nt_product_type;          /* 0x264 */
    BYTE product_type_is_valid, reserved;
    WORD native_processor_architecture;
    DWORD nt_major_version;         /* 0x26C */
    DWORD nt_minor_version;         /* 0x270 */
} KuserVersion;
_Static_assert(offset_of(KuserVersion, nt_major_version) == 0x26C - KUSER_VERSION_FIELDS &&
                   offset_of(KuserVersion, nt_minor_version) == 0x270 - KUSER_VERSION_FIELDS,
               "KUSER_SHARED_DATA version fields");
#define WINDOWS_11_FIRST_BUILD 22000

/* ---- the lines ---------------------------------------------------------------------------------------------------- */

#define VENDOR_NVIDIA 0x10DE
#define VENDOR_AMD 0x1002
#define VENDOR_INTEL 0x8086
static const char *vendor_name(DWORD id) {
    return id == VENDOR_NVIDIA ? "NVIDIA" : id == VENDOR_AMD ? "AMD" : id == VENDOR_INTEL ? "Intel" : "other";
}

/* "<call> failed with 0x...", or "<call> gave nothing" when it succeeded without handing out an object. */
static void put_failure(Line *line, const char *call, HRESULT result) {
    put(line, call);
    if (result < 0) { put(line, " failed with "); put_hex(line, (DWORD)result); }
    else put(line, " gave nothing");
}

/* The adapter the game renders on (AddRef'd): swap chain -> ID3D11Device -> IDXGIDevice -> IDXGIAdapter. On failure
   *adapter is 0 and *failed names the call. */
static HRESULT render_adapter(void *swapchain, void **adapter, const char **failed) {
    void *device = 0, *dxgi_device = 0;
    *adapter = 0;
    HRESULT result = swap_chain_device(swapchain, &device);
    if (result < 0 || !device) { *failed = "the swap chain's GetDevice(ID3D11Device)"; return result; }
    result = ((GetByIidFn)method(device, SLOT_QUERY_INTERFACE))(device, &IID_IDXGIDevice, &dxgi_device);
    release(device);
    if (result < 0 || !dxgi_device) { *failed = "ID3D11Device::QueryInterface(IDXGIDevice)"; return result; }
    result = ((GetAdapterFn)method(dxgi_device, SLOT_GET_ADAPTER))(dxgi_device, adapter);
    release(dxgi_device);
    if (result < 0 || !*adapter) { *adapter = 0; *failed = "IDXGIDevice::GetAdapter"; }
    return result;
}

/* The user-mode driver version, as a.b.c.d (for NVIDIA the last five digits are the driver number: 32.0.16.1742 = 617.42). */
static void put_driver_version(Line *line, long long version) {
    DWORD high = (DWORD)((unsigned long long)version >> 32), low = (DWORD)version;
    put_dec(line, high >> 16, 0); put(line, "."); put_dec(line, high & 0xFFFF, 0); put(line, ".");
    put_dec(line, low >> 16, 0); put(line, "."); put_dec(line, low & 0xFFFF, 0);
}

/* The "gpu: ..." line, not yet written. Returns 1 and the render adapter's LUID when it is known. */
static COLD int gpu_line(void *swapchain, Line *line, Luid *luid) {
    void *adapter;
    const char *failed = 0;
    DxgiAdapterDesc desc;
    long long version = 0;
    begin(line);
    put(line, "gpu: ");
    HRESULT result = render_adapter(swapchain, &adapter, &failed);
    if (!adapter) { put(line, "unknown ("); put_failure(line, failed, result); put(line, ")"); return 0; }
    zero(&desc, sizeof desc);
    result = ((AdapterGetDescFn)method(adapter, SLOT_ADAPTER_GET_DESC))(adapter, &desc);
    HRESULT versioned = result >= 0 ? ((CheckInterfaceSupportFn)method(adapter, SLOT_CHECK_INTERFACE_SUPPORT))(adapter, &IID_IDXGIDevice, &version) : 0;
    release(adapter);
    if (result < 0) { put(line, "unknown ("); put_failure(line, "IDXGIAdapter::GetDesc", result); put(line, ")"); return 0; }
    desc.description[127] = 0;
    DWORD megabytes = (DWORD)(desc.dedicated_video_memory >> 20);
    put(line, "\""); put_wide(line, desc.description); put(line, "\" (vendor "); put_hex(line, desc.vendor_id); put(line, " ");
    put(line, vendor_name(desc.vendor_id)); put(line, ", device "); put_hex(line, desc.device_id); put(line, "), ");
    put_dec(line, megabytes, 0); put(line, " MB dedicated video memory");
    if (megabytes >= 3072) put(line, " (or more: Windows tells a 32-bit game at most about 3 GB)");
    put(line, ", driver ");
    if (versioned >= 0) put_driver_version(line, version);
    else { put(line, "version unknown ("); put_failure(line, "CheckInterfaceSupport", versioned); put(line, ")"); }
    *luid = desc.adapter_luid;
    return 1;
}

static const char *swap_effect_name(DWORD effect) {
    return effect == SWAP_EFFECT_DISCARD           ? "discard (blt model)"
           : effect == SWAP_EFFECT_SEQUENTIAL      ? "sequential (blt model)"
           : effect == SWAP_EFFECT_FLIP_SEQUENTIAL ? "flip sequential (flip model)"
           : effect == SWAP_EFFECT_FLIP_DISCARD    ? "flip discard (flip model)"
                                                   : 0;
}

/* Which GPU drives the display: the adapter of the output that shows the game (its parent), against the render adapter
   (a different one means hybrid graphics: the frames are copied across). `render` is 0 when that is unknown. */
static void put_display_gpu(Line *line, void *swapchain, const Luid *render) {
    void *output = 0, *adapter = 0;
    DxgiAdapterDesc desc;
    HRESULT result = ((GetContainingOutputFn)method(swapchain, SLOT_GET_CONTAINING_OUTPUT))(swapchain, &output);
    if (result < 0 || !output) { put(line, "output's GPU unknown ("); put_failure(line, "IDXGISwapChain::GetContainingOutput", result); put(line, ")"); return; }
    result = ((GetByIidFn)method(output, SLOT_GET_PARENT))(output, &IID_IDXGIAdapter, &adapter);
    release(output);
    if (result < 0 || !adapter) { put(line, "output's GPU unknown ("); put_failure(line, "IDXGIOutput::GetParent(IDXGIAdapter)", result); put(line, ")"); return; }
    zero(&desc, sizeof desc);
    result = ((AdapterGetDescFn)method(adapter, SLOT_ADAPTER_GET_DESC))(adapter, &desc);
    release(adapter);
    if (result < 0) { put(line, "output's GPU unknown ("); put_failure(line, "IDXGIAdapter::GetDesc", result); put(line, ")"); return; }
    desc.description[127] = 0;
    if (render && desc.adapter_luid.low == render->low && desc.adapter_luid.high == render->high) { put(line, "output on the same GPU as rendering"); return; }
    put(line, render ? "output on a different GPU (hybrid): \"" : "output on \"");
    put_wide(line, desc.description);
    put(line, render ? "\"" : "\" (render GPU unknown)");
}

/* "display: ..." `described` is GetDesc's result for `desc`. */
static COLD void log_display(void *swapchain, HRESULT described, const DxgiSwapChainDesc *desc, DWORD refresh, const Luid *render) {
    Line line; begin(&line);
    put(&line, "display: ");
    if (refresh) { put_dec(&line, refresh, 0); put(&line, " Hz"); }
    else put(&line, "refresh rate unknown");
    if (described < 0) { put(&line, "; swap chain unknown ("); put_failure(&line, "IDXGISwapChain::GetDesc", described); put(&line, ")"); }
    else {
        const char *effect = swap_effect_name(desc->swap_effect);
        put(&line, "; swap chain: ");
        if (effect) put(&line, effect);
        else { put(&line, "swap effect "); put_dec(&line, desc->swap_effect, 0); }
        put(&line, ", "); put_dec(&line, desc->buffer_count, 0); put(&line, " buffers of "); put_dec(&line, desc->width, 0); put(&line, "x");
        put_dec(&line, desc->height, 0); put(&line, desc->windowed ? ", windowed" : ", fullscreen"); put(&line, ", flags "); put_hex(&line, desc->flags);
    }
    put(&line, "; ");
    put_display_gpu(&line, swapchain, render);
    emit(&line);
}

/* "power: ..." */
static COLD void log_power(void) {
    SystemPowerStatus status;
    zero(&status, sizeof status);
    Line line; begin(&line);
    put(&line, "power: ");
    if (!GetSystemPowerStatus(&status)) {
        put(&line, "unknown (GetSystemPowerStatus failed, error "); put_dec(&line, GetLastError(), 0); put(&line, ")");
        emit(&line); return;
    }
    if (status.ac_line_status == AC_LINE_ONLINE) put(&line, status.battery_flag & BATTERY_FLAG_NO_BATTERY ? "on AC power (no battery)" : "on AC power");
    else if (status.ac_line_status == AC_LINE_OFFLINE) {
        put(&line, "on battery");
        if (status.battery_life_percent <= 100) { put(&line, " ("); put_dec(&line, status.battery_life_percent, 0); put(&line, " %)"); }
    } else put(&line, "power source unknown");
    put(&line, status.system_status_flag & SYSTEM_STATUS_BATTERY_SAVER ? ", battery saver on" : ", battery saver off");
    emit(&line);
}

/* "os: ..." */
static COLD void log_os(void) {
    const volatile KuserVersion *version = (const volatile KuserVersion *)(KUSER_SHARED_DATA + KUSER_VERSION_FIELDS);
    DWORD major = version->nt_major_version, minor = version->nt_minor_version, build = version->nt_build_number;
    Line line; begin(&line);
    put(&line, "os: Windows "); put_dec(&line, major, 0); put(&line, "."); put_dec(&line, minor, 0);
    if (build) { put(&line, " build "); put_dec(&line, build, 0); }
    else put(&line, " (build unknown)");
    if (major == 10 && minor == 0 && build) put(&line, build >= WINDOWS_11_FIRST_BUILD ? " (Windows 11)" : " (Windows 10)");
    emit(&line);
}

/* ---- the report, and the check for a changed display (render thread) ----------------------------------------------- */

/* What the last report found, which the cheap check compares with. */
static struct {
    HRESULT described;          /* GetDesc's result, */
    DxgiSwapChainDesc desc;     /* and the description (zeroed when it failed) */
    HANDLE monitor;             /* the monitor showing the swap chain's window, 0 when GetDesc failed */
    int render_known;           /* the gpu: line named an adapter: */
    Luid render;                /* this one */
} g_reported;

static HRESULT get_desc(void *swapchain, DxgiSwapChainDesc *desc) {
    zero(desc, sizeof *desc);
    return ((SwapChainGetDescFn)method(swapchain, SLOT_SWAP_CHAIN_GET_DESC))(swapchain, desc);
}

/* At the first frame (before its real Present) the four lines; `again` (the display changed; after that frame's real
   Present) the display: line,
   and the gpu: line only when the game now renders on another adapter (a new device on the same adapter has the same
   GPU and driver, so its line would read the same). Returns the display's refresh rate (0 = unknown) and sets *window
   to the swap chain's output window (0 when GetDesc failed). */
COLD DWORD log_setup_report(void *swapchain, HANDLE *window, int again) {
    Luid render;
    Line gpu;
    g_reported.described = get_desc(swapchain, &g_reported.desc);
    g_reported.monitor = g_reported.described >= 0 ? window_monitor(g_reported.desc.output_window) : 0;
    DWORD refresh = monitor_refresh(g_reported.monitor);
    int known = gpu_line(swapchain, &gpu, &render);
    if (!again || known != g_reported.render_known || (known && !same_bytes(&render, &g_reported.render, sizeof render))) emit(&gpu);
    g_reported.render_known = known;
    if (known) g_reported.render = render;
    log_display(swapchain, g_reported.described, &g_reported.desc, refresh, known ? &render : 0);
    if (!again) { log_power(); log_os(); }
    *window = g_reported.described >= 0 ? g_reported.desc.output_window : 0;
    return refresh;
}

/* The cheap check, about once a second: GetDesc and MonitorFromWindow only (a few microseconds), no file or driver
   query. DISPLAY_NEW_SWAP_CHAIN when the swap chain's description differs from the report's (a new swap chain at the
   old one's address, new buffers after a resolution change, a switch to or from fullscreen), DISPLAY_NEW_MONITOR when
   its window is now on another monitor. */
COLD int display_change(void *swapchain) {
    DxgiSwapChainDesc desc;
    HRESULT described = get_desc(swapchain, &desc);
    if ((described < 0) != (g_reported.described < 0)) return DISPLAY_NEW_SWAP_CHAIN;
    if (described < 0) return DISPLAY_SAME; /* still no description: nothing to compare */
    if (!same_bytes(&desc, &g_reported.desc, sizeof desc)) return DISPLAY_NEW_SWAP_CHAIN;
    return window_monitor(desc.output_window) != g_reported.monitor ? DISPLAY_NEW_MONITOR : DISPLAY_SAME;
}
