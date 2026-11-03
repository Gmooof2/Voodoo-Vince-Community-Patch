#include "NativeWaterHooks.h"
#include <string.h>

namespace vince {
namespace water {

bool ReadNativeWaterMemory(void*, uintptr_t address, void* output, size_t bytes) {
    if (!address || !output || !bytes || address > UINTPTR_MAX - bytes)
        return false;
    uintptr_t cursor = address;
    const uintptr_t end = address + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION page = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &page, sizeof(page)) ||
            page.State != MEM_COMMIT || (page.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const DWORD access = page.Protect & 0xffu;
        if (access != PAGE_READONLY && access != PAGE_READWRITE && access != PAGE_WRITECOPY &&
            access != PAGE_EXECUTE_READ && access != PAGE_EXECUTE_READWRITE &&
            access != PAGE_EXECUTE_WRITECOPY)
            return false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(page.BaseAddress);
        if (page.RegionSize > UINTPTR_MAX - base || base + page.RegionSize <= cursor)
            return false;
        cursor = base + page.RegionSize;
    }
    // The caller owns the native render thread/lifetime. Page checks do not make
    // arbitrary pointers safe against concurrent object destruction.
    memcpy(output, reinterpret_cast<const void*>(address), bytes);
    return true;
}

} // namespace water
} // namespace vince
