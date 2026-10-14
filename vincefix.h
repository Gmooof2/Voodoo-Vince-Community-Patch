/*
 * VinceFix: what its source files share. Built freestanding (no CRT, no Windows SDK headers), so the few Windows types,
 * functions and structures VinceFix uses are declared here or in the file that alone uses them; see build.ps1.
 *
 *   dllmain.c     DllMain: the order things start in
 *   common.c      the log (VinceFix.log), file paths, byte helpers
 *   hooks.c       patching Vince.exe's imports; thread unpinning; DPI awareness
 *   audio.c       X3DAudio forwarding (thunks.s) and the XAudio2 device-details cache
 *   settings.c    VinceFix.ini: reading it, what runs on which display, live reload's watcher thread
 *   present.c     the patched Present call: present hold, frame cap, hold flush, auto mode
 *   setup_info.c  the gpu: / display: / power: / os: lines, the display's refresh rate, the check for a new display
 */
#ifndef VINCEFIX_H
#define VINCEFIX_H

typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef long HRESULT;
typedef unsigned long ULONG_PTR;
typedef void *HANDLE;
typedef void *HMODULE;
typedef unsigned short WCHAR; /* type of L"" literals on this target */
typedef struct { DWORD data1; WORD data2, data3; BYTE data4[8]; } GUID;

#define WINAPI __stdcall
#define IMPORT __declspec(dllimport)
#define COLD __attribute__((noinline)) /* rarely run (load time, a log line): kept out of its callers */
#define offset_of(type, field) __builtin_offsetof(type, field)

/* kernel32 functions used by more than one file (the others are declared where they are used) */
IMPORT HMODULE WINAPI GetModuleHandleW(const WCHAR *name);
IMPORT void *WINAPI GetProcAddress(HMODULE module, const char *name);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT HANDLE WINAPI GetCurrentProcess(void);
IMPORT HANDLE WINAPI CreateFileW(const WCHAR *path, DWORD access, DWORD share, void *security, DWORD disposition, DWORD flags, HANDLE templ);
IMPORT BOOL WINAPI CloseHandle(HANDLE handle);
IMPORT BOOL WINAPI VirtualProtect(void *address, ULONG_PTR size, DWORD protect, DWORD *old);
IMPORT BOOL WINAPI QueryPerformanceCounter(long long *count);
IMPORT BOOL WINAPI QueryPerformanceFrequency(long long *frequency);
IMPORT void WINAPI AcquireSRWLockExclusive(void **lock);
IMPORT void WINAPI ReleaseSRWLockExclusive(void **lock);
IMPORT DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD milliseconds);

/* Their arguments and results as VinceFix uses them (winnt.h, winbase.h, winerror.h) */
#define INVALID_HANDLE ((HANDLE)-1) /* INVALID_HANDLE_VALUE */
#define GENERIC_READ 0x80000000u
#define FILE_SHARE_READ_WRITE 0x3
#define OPEN_EXISTING 3
#define FILE_ATTRIBUTE_NORMAL 0x80
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE_READWRITE 0x40
#define E_FAIL ((HRESULT)0x80004005)

/* ---- common.c: the log, paths and bytes ---------------------------------------------------------------------------- */

extern HMODULE g_self;        /* this DLL */
extern ULONG_PTR g_exe_base;  /* Vince.exe (the host), 0 if it is not a PE32 image */

/* One log line, built with the put functions and written by emit(). The longest, the stats line with auto, FrameCap,
   HoldFlush and the latency figures, is ~760 with every number at its largest. */
typedef struct { char text[800]; unsigned length; } Line;
void begin(Line *line); /* the time and pid prefix */
void put(Line *line, const char *s);
void put_dec(Line *line, unsigned long long v, int width); /* width: zero-padded to at least this many digits */
void put_hex(Line *line, unsigned long long v);
void put_wide(Line *line, const WCHAR *s); /* printable ASCII as is, anything else as '?' */
void put_ms(Line *line, DWORD us);         /* microseconds as ms with 2 decimals */
void emit(Line *line);                     /* appends the line to VinceFix.log */
void log_text(const char *text);           /* a log line that is one fixed text */
void init_log_path(void);

int append_name(WCHAR *dir, unsigned capacity, const WCHAR *name); /* dir + '\' + name; 0 if it doesn't fit */
void self_dir(WCHAR *out, unsigned capacity);                       /* this DLL's folder with a trailing '\', or "" */
void zero(void *p, unsigned n);
void copy_bytes(void *to, const void *from, unsigned n); /* struct assignment would call memcpy, which isn't there */
int same_bytes(const void *a, const void *b, unsigned n);

/* ---- PE images (winnt.h): Vince.exe's imports and code, XAudio2's image size ------------------------------------------ */

typedef struct { WORD magic; BYTE unused[58]; DWORD nt_headers; } DosHeader;    /* IMAGE_DOS_HEADER: e_magic, e_lfanew */
typedef struct { DWORD rva, size; } DataDirectory;                               /* IMAGE_DATA_DIRECTORY */
typedef struct {                                                                 /* IMAGE_NT_HEADERS32 */
    DWORD signature;                                                             /* "PE\0\0" */
    WORD machine, section_count;                                                 /* IMAGE_FILE_HEADER ... */
    DWORD time_date_stamp, symbol_table, symbol_count;
    WORD optional_header_size, characteristics;
    WORD magic;                                                                  /* IMAGE_OPTIONAL_HEADER32 ... */
    BYTE linker_version[2];
    DWORD code_size, data_size, bss_size, entry_point, code_base, data_base, image_base, section_alignment, file_alignment;
    WORD versions[6];
    DWORD win32_version, image_size, headers_size, checksum;
    WORD subsystem, dll_characteristics;
    DWORD stack_reserve, stack_commit, heap_reserve, heap_commit, loader_flags, directory_count;
    DataDirectory directory[16];                                                 /* [1]: the import table */
} PeHeaders;
typedef struct {                                                                 /* IMAGE_SECTION_HEADER */
    BYTE name[8];
    DWORD virtual_size, virtual_address, raw_size, raw_pointer, relocations, line_numbers;
    WORD relocation_count, line_number_count;
    DWORD characteristics;
} PeSection;
typedef struct { DWORD names, time_date_stamp, forwarder_chain, dll_name, slots; } PeImport; /* IMAGE_IMPORT_DESCRIPTOR */
_Static_assert(offset_of(DosHeader, nt_headers) == 0x3C, "IMAGE_DOS_HEADER layout");
_Static_assert(offset_of(PeHeaders, magic) == 24 && offset_of(PeHeaders, image_size) == 24 + 56 &&
                   offset_of(PeHeaders, directory) == 24 + 96 && sizeof(PeHeaders) == 24 + 224,
               "IMAGE_NT_HEADERS32 layout");
_Static_assert(sizeof(PeSection) == 40 && offset_of(PeSection, characteristics) == 36, "IMAGE_SECTION_HEADER layout");
_Static_assert(sizeof(PeImport) == 20, "IMAGE_IMPORT_DESCRIPTOR layout");
#define PE_SIGNATURE 0x00004550   /* "PE\0\0" */
#define PE32_MAGIC 0x10B
#define PE_IMPORT_DIRECTORY 1
#define PE_SECTION_EXECUTE 0x20000000 /* IMAGE_SCN_MEM_EXECUTE */
#define PE_IMPORT_BY_ORDINAL 0x80000000u
#define PE_HINT_SIZE 2                 /* IMAGE_IMPORT_BY_NAME: a WORD hint before the name */

static inline const PeHeaders *pe_headers(const void *base) {
    return (const PeHeaders *)((const BYTE *)base + ((const DosHeader *)base)->nt_headers);
}
static inline const PeSection *pe_sections(const PeHeaders *nt) {
    return (const PeSection *)((const BYTE *)&nt->magic + nt->optional_header_size);
}

/* ---- COM: the slot numbers VinceFix calls, counted from the SDK headers' declaration order ----------------------------- */
/*
 * Built without the Windows SDK, so the COM vtable slots are counted here from the declaration order in dxgi.h and
 * d3d11.h, and the structures follow the documented 32-bit layouts (size-checked with _Static_assert). All of it was
 * checked at run time on this PC (2026-10-14): on a WARP device and swap chain, and on the real adapter and a small
 * hardware swap chain, by reading back known values at these offsets, confirming each returned interface with
 * QueryInterface, and comparing adapter LUIDs.
 *   IUnknown: QueryInterface 0, AddRef 1, Release 2.
 *   IDXGIObject (IUnknown 0-2): SetPrivateData 3, SetPrivateDataInterface 4, GetPrivateData 5, GetParent 6.
 *   IDXGIDeviceSubObject (IDXGIObject 0-6): GetDevice 7.
 *   IDXGISwapChain (IDXGIDeviceSubObject 0-7): Present 8, GetBuffer 9, SetFullscreenState 10, GetFullscreenState 11,
 *     GetDesc 12, ResizeBuffers 13, ResizeTarget 14, GetContainingOutput 15, GetFrameStatistics 16, GetLastPresentCount 17.
 *   IDXGIDevice (IDXGIObject 0-6): GetAdapter 7, CreateSurface 8, QueryResourceResidency 9, SetGPUThreadPriority 10,
 *     GetGPUThreadPriority 11.
 *   IDXGIAdapter (IDXGIObject 0-6): EnumOutputs 7, GetDesc 8, CheckInterfaceSupport 9.
 *   IDXGIOutput (IDXGIObject 0-6): GetDesc 7 ... GetFrameStatistics 18; only its GetParent (the adapter) is used.
 *   ID3D11Device, 43 slots: IUnknown 0-2, CreateBuffer 3, CreateTexture1D/2D/3D 4-6, Create{ShaderResource,
 *     UnorderedAccess,RenderTarget,DepthStencil}View 7-10, CreateInputLayout 11, Create{Vertex,Geometry,
 *     GeometryWithStreamOutput,Pixel,Hull,Domain,Compute}Shader 12-18, CreateClassLinkage 19, Create{Blend,
 *     DepthStencil,Rasterizer,Sampler}State 20-23, CreateQuery/Predicate/Counter 24-26, CreateDeferredContext 27,
 *     OpenSharedResource 28, Check{FormatSupport,MultisampleQualityLevels,CounterInfo,Counter,FeatureSupport} 29-33,
 *     GetPrivateData/SetPrivateData/SetPrivateDataInterface 34-36, GetFeatureLevel 37, GetCreationFlags 38,
 *     GetDeviceRemovedReason 39, GetImmediateContext 40 (void, AddRefs), SetExceptionMode 41, GetExceptionMode 42.
 *   ID3D11DeviceContext, 115 slots: IUnknown 0-2, ID3D11DeviceChild 3-6 (GetDevice, GetPrivateData, SetPrivateData,
 *     SetPrivateDataInterface), 65 set/draw/copy calls 7-71 (VSSetConstantBuffers 7, PSSetShaderResources 8 ...
 *     DrawIndexed 12, Draw 13, Map 14 ... CSSetConstantBuffers 71), their 38 getters 72-109 (VSGetConstantBuffers 72 ...
 *     CSGetConstantBuffers 109), ClearState 110, Flush 111 (void), GetType 112, GetContextFlags 113, FinishCommandList 114.
 *   IXAudio2 (2.7): IUnknown 0-2, GetDeviceCount 3, GetDeviceDetails 4, ...
 * The slots one file uses are defined there; these are shared.
 */
#define SLOT_QUERY_INTERFACE 0 /* IUnknown */
#define SLOT_RELEASE 2         /* IUnknown */
#define SLOT_GET_DEVICE 7      /* IDXGIDeviceSubObject, the base of IDXGISwapChain */

typedef DWORD(WINAPI *ReleaseFn)(void *self);
typedef HRESULT(WINAPI *GetByIidFn)(void *self, const GUID *iid, void **object); /* QueryInterface, GetDevice, GetParent */
extern const GUID IID_ID3D11Device;

/* The function in a COM object's vtable slot. */
static inline void *method(void *object, unsigned slot) { return (*(void ***)object)[slot]; }
static inline void release(void *object) { ((ReleaseFn)method(object, SLOT_RELEASE))(object); }
/* The game's D3D11 device, from its swap chain (AddRef'd: release it). */
static inline HRESULT swap_chain_device(void *swapchain, void **device) {
    *device = 0;
    return ((GetByIidFn)method(swapchain, SLOT_GET_DEVICE))(swapchain, &IID_ID3D11Device, device);
}

/* ---- hooks.c, audio.c ------------------------------------------------------------------------------------------------ */

ULONG_PTR patch_import(const char *dll, const WCHAR *dll_wide, const char *function, void *replacement, const char *now);
void unpin_threads(void);
void set_dpi_awareness(void);
void hook_xaudio(void);

/* ---- settings.c: what VinceFix.ini asks for, and what of it runs ------------------------------------------------------ */

enum { FLUSH_OFF, FLUSH_ON, FLUSH_AUTO }; /* HoldFlush=0, 1, auto */
typedef struct {
    DWORD hold_us;  /* PresentHold: the fixed hold, 0 when off or auto */
    int hold_auto;  /* PresentHold=auto */
    DWORD cap_fps;  /* FrameCap, 0 when off */
    int flush;      /* HoldFlush: FLUSH_* (decide() gives only FLUSH_ON or FLUSH_OFF) */
} Settings;
#define HOLD_MIN_US 1000
#define HOLD_MAX_US 12000
#define HOLD_HEADROOM_US 2000 /* a hold stays this far below the capped frame time */

int hold_asked(const Settings *s);
Settings decide(const Settings *asked, DWORD refresh); /* what runs of `asked` on a display of `refresh` Hz (0 = unknown) */
void log_display_rules(const Settings *asked, DWORD refresh);
void setup_present_hold(void); /* DllMain: reads VinceFix.ini, patches the Present call if anything asks for it */

/* ---- present.c: the render thread's side ---------------------------------------------------------------------------- */

#define REFRESH_NOT_CHECKED 0xFFFFFFFFu /* published before the game showed a frame: the first frame's check decides */
enum { FLUSH_RUNNING, FLUSH_STOPPED_NO_DEVICE, FLUSH_STOPPED_NO_CONTEXT };
extern Settings g_asked;         /* what VinceFix.ini asks for (render thread, and DllMain before the game runs) */
extern int g_setup_done;         /* the first frame's display check has run (read by the live-reload thread) */
extern int g_flush_stopped;      /* FLUSH_STOPPED_* once HoldFlush's lookup failed (read by the live-reload thread) */
/* Written by the render thread (the first frame before g_setup_done is released, then whenever it finds the display
   changed, and a live reload), read by the watcher after it saw g_setup_done set. */
extern HANDLE g_game_window;     /* the swap chain's output window */
extern DWORD g_display_refresh;  /* the refresh rate the settings run at now, 0 = unknown */
extern long g_display_seq;       /* bumped each time the render thread checks the display */

BYTE *patch_present_call(const char *what);
void apply_settings(DWORD refresh);
/* Live-reload thread: hand a change to the render thread, with the refresh rate found for it and the g_display_seq
   read before that check. */
void publish_settings(const Settings *asked, DWORD refresh, long display_seq);

/* ---- setup_info.c ---------------------------------------------------------------------------------------------------- */

enum { DISPLAY_SAME, DISPLAY_NEW_SWAP_CHAIN, DISPLAY_NEW_MONITOR, DISPLAY_FIRST }; /* what the display check found */
DWORD display_refresh(HANDLE window); /* Hz of the display that shows `window`, 0 when unknown */
/* The display report (the first frame's four lines; again after a change, display: and, if the adapter changed, gpu:).
   Returns the refresh rate and sets *window. Render thread. */
DWORD log_setup_report(void *swapchain, HANDLE *window, int again);
int display_change(void *swapchain); /* the cheap check against the last report: DISPLAY_SAME or what changed */

#endif
