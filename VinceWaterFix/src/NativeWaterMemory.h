#pragma once
#include <stddef.h>
#include <stdint.h>

namespace vince {
namespace water {

// Read-only boundary between decoded PC objects and the loader. Tests can use
// byte arrays; a Windows host must verify readable pages before copying. Native
// object pointers are 32-bit even when the evidence tools themselves are x64.
using NativeReadMemory = bool (*)(void* user, uintptr_t address, void* output, size_t bytes);
struct NativeMemory {
    NativeReadMemory read;
    void* user;
    uintptr_t imageBase;

    bool Read(uintptr_t address, void* output, size_t bytes) const {
        return read && address && output && bytes && address <= UINTPTR_MAX - (bytes - 1) &&
               read(user, address, output, bytes);
    }
    template <class T> bool Value(uintptr_t address, T& output) const {
        return Read(address, &output, sizeof(output));
    }
    uintptr_t Address(uint32_t preferredVA) const {
        const uintptr_t rva = preferredVA >= 0x400000u ? preferredVA - 0x400000u : UINTPTR_MAX;
        return imageBase && rva <= UINTPTR_MAX - imageBase ? imageBase + rva : 0;
    }
};

} // namespace water
} // namespace vince
