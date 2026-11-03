/* AIO core (1.4 T6): the kernel32 functions core.c and later core subsystems call. Freestanding: no SDK headers.
   Import libraries come from kernel32.def beside this file (aio/build.py dll). */
#ifndef AIO_KERNEL32_H
#define AIO_KERNEL32_H

typedef unsigned int u32; typedef unsigned short u16; typedef unsigned char u8;
#define WINAPI __stdcall

__declspec(dllimport) int WINAPI DisableThreadLibraryCalls(void *);
__declspec(dllimport) void *WINAPI GetModuleHandleW(const u16 *);
__declspec(dllimport) u32 WINAPI GetModuleFileNameW(void *, u16 *, u32); /* 1.4 T7: VinceAIO.ini's folder */
/* Halo (1.6): as VoodooPowerFix/pickup_halo_toggle.c:32-33 (def: VirtualAlloc@16, VirtualFree@12). */
__declspec(dllimport) void *WINAPI VirtualAlloc(void *, u32, u32, u32);
__declspec(dllimport) int WINAPI VirtualFree(void *, u32, u32);
__declspec(dllimport) u32 WINAPI VirtualQuery(const void *, void *, u32); /* 28-byte MEMORY_BASIC_INFORMATION */
__declspec(dllimport) int WINAPI VirtualProtect(void *, u32, u32, u32 *);
__declspec(dllimport) int WINAPI FlushInstructionCache(void *, const void *, u32);
__declspec(dllimport) void *WINAPI GetCurrentProcess(void);

#endif
