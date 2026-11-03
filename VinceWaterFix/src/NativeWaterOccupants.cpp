#include "NativeWaterOccupants.h"

namespace vince {
namespace water {
namespace {

constexpr uint32_t kPuddleNodeVtable = 0x6e0f0c;
constexpr uint32_t kPuddleScriptVtable = 0x6e0fa4; // Stored at 0x537d28.
constexpr unsigned kMaxName = 63;

uint32_t U32(const NativeMemory& memory, uintptr_t address) {
    uint32_t value = 0;
    return memory.Value(address, value) ? value : 0;
}

bool IsNil(const NativeMemory& memory, uintptr_t node) {
    uint8_t nil = 1;
    return !memory.Value(node + 0xd, nil) || nil;
}

// MSVC std::string: inline buffer or pointer, +0x10 size, +0x14 capacity.
unsigned ReadString(const NativeMemory& memory, uintptr_t string, char* out, unsigned capacity) {
    out[0] = 0;
    const uint32_t length = U32(memory, string + 0x10), reserved = U32(memory, string + 0x14);
    if (!length || length >= capacity || reserved < length)
        return 0;
    const uintptr_t text = reserved >= 16 ? U32(memory, string) : string;
    if (!memory.Read(text, out, length))
        return 0;
    out[length] = 0;
    return length;
}

// std::map in-order successor (+0 left, +4 parent, +8 right, +0xd isnil).
uintptr_t MapNext(const NativeMemory& memory, uintptr_t node) {
    uintptr_t right = U32(memory, node + 8);
    if (right && !IsNil(memory, right)) {
        for (unsigned guard = 0; guard < 64; ++guard) {
            const uintptr_t left = U32(memory, right);
            if (!left || IsNil(memory, left))
                break;
            right = left;
        }
        return right;
    }
    uintptr_t parent = U32(memory, node + 4);
    for (unsigned guard = 0; parent && guard < 64; ++guard) {
        if (IsNil(memory, parent) || node != U32(memory, parent + 8))
            break;
        node = parent;
        parent = U32(memory, parent + 4);
    }
    return parent;
}

// GetScript 0x42d920: back() of the node's std::deque<Script*> at +0x94
// (+0x98 map, +0x9c map size, +0xa0 offset, +0xa4 size; 4 entries a block).
uintptr_t NodeScript(const NativeMemory& memory, uintptr_t node) {
    const uint32_t map = U32(memory, node + 0x98), mapSize = U32(memory, node + 0x9c);
    const uint32_t offset = U32(memory, node + 0xa0), size = U32(memory, node + 0xa4);
    if (!map || !size || !mapSize || (mapSize & (mapSize - 1)))
        return 0;
    const uint32_t index = offset + size - 1;
    const uint32_t block = U32(memory, map + 4 * ((index >> 2) & (mapSize - 1)));
    return block ? U32(memory, block + 4 * (index & 3)) : 0;
}

// CritterManager::Find 0x4cad40: lower_bound over std::map<string, Critter*>
// at [World 0xb631ec] (key +0x10 lowercased with tolower, value +0x28),
// rejecting critters whose byte +0x255 is set.
uintptr_t FindCritter(const NativeMemory& memory, const char* name) {
    char key[kMaxName + 1];
    unsigned length = 0;
    for (; name[length] && length < kMaxName; ++length)
        key[length] = name[length] >= 'A' && name[length] <= 'Z' ? char(name[length] + 32)
                                                                 : name[length];
    key[length] = 0;
    const uint32_t world = U32(memory, memory.Address(0xb631ec));
    const uint32_t head = world ? U32(memory, world) : 0;
    if (!length || !head)
        return 0;
    uintptr_t node = U32(memory, head + 4), best = head;
    for (unsigned guard = 0; node && guard < 64 && !IsNil(memory, node); ++guard) {
        char other[kMaxName + 1];
        const unsigned otherLength = ReadString(memory, node + 0x10, other, sizeof(other));
        int order = 0;
        for (unsigned i = 0; !order && i < otherLength && i < length; ++i)
            order = int(static_cast<unsigned char>(other[i])) - int(static_cast<unsigned char>(key[i]));
        if (!order)
            order = otherLength < length ? -1 : otherLength > length ? 1 : 0;
        if (order < 0) {
            node = U32(memory, node + 8);
        } else {
            best = node;
            node = U32(memory, node);
        }
    }
    if (best == head)
        return 0;
    char found[kMaxName + 1];
    if (ReadString(memory, best + 0x10, found, sizeof(found)) != length)
        return 0;
    for (unsigned i = 0; i < length; ++i)
        if (found[i] != key[i])
            return 0;
    const uint32_t critter = U32(memory, best + 0x28);
    uint8_t removed = 1;
    return critter && memory.Value(critter + 0x255, removed) && !removed ? critter : 0;
}

} // namespace

bool ValidNativePuddleRegion(const NativeMemory& memory, const NativePuddleRegion& region) {
    return region.node && region.script &&
           U32(memory, region.node) == memory.Address(kPuddleNodeVtable) &&
           U32(memory, region.script) == memory.Address(kPuddleScriptVtable) &&
           U32(memory, region.script + 0x40) == region.node;
}

bool FindNativePuddleRegion(const NativeMemory& memory, NativePuddleRegion* region) {
    *region = {};
    const uint32_t game = U32(memory, memory.Address(0xb653f4));
    const uint32_t manager = game ? U32(memory, game + 0x34) : 0;
    const uint32_t head = manager ? U32(memory, manager) : 0;
    const uint32_t total = manager ? U32(memory, manager + 4) : 0;
    if (!head || total > 4096)
        return false;
    uintptr_t entry = U32(memory, head);
    for (uint32_t guard = 0; entry && entry != head && guard <= total; ++guard) {
        const uint32_t regionNode = U32(memory, entry + 0x28);
        const uint32_t listHead = regionNode ? U32(memory, regionNode + 0x24) : 0;
        uintptr_t item = listHead ? U32(memory, listHead) : 0;
        for (unsigned count = 0; item && item != listHead && count < 64; ++count) {
            const uint32_t child = U32(memory, item + 8);
            if (child && U32(memory, child) == memory.Address(kPuddleNodeVtable)) {
                const NativePuddleRegion found = {child, NodeScript(memory, regionNode)};
                if (ValidNativePuddleRegion(memory, found)) {
                    *region = found;
                    return true;
                }
            }
            item = U32(memory, item);
        }
        entry = MapNext(memory, entry);
    }
    return false;
}

int ReadNativePuddleOccupants(const NativeMemory& memory, uintptr_t script,
                              NativePuddleOccupant* occupants, unsigned capacity) {
    // Entries 0x24 bytes at [+0x5c, +0x60): name, id +0x18, last u/v.
    uint32_t begin = 0, end = 0;
    if (!memory.Value(script + 0x5c, begin) || !memory.Value(script + 0x60, end) || end < begin ||
        (end - begin) % 0x24 || (end - begin) / 0x24 > 256)
        return -1;
    const uint32_t world = U32(memory, memory.Address(0xb631ec));
    const uint32_t vince = world ? U32(memory, world + 0x1c) : 0;
    unsigned count = 0;
    for (uint32_t entry = begin; entry < end && count < capacity; entry += 0x24) {
        NativePuddleOccupant& occupant = occupants[count];
        if (!ReadString(memory, entry, occupant.name, sizeof(occupant.name)))
            continue;
        // FrameTick drops critters it cannot find. The preview also skipped
        // node flag bit 0; Remastered's Find rejects via +0x255 instead, and
        // bit 0's meaning on Remastered critters is unverified, so only that.
        const uintptr_t critter = FindCritter(memory, occupant.name);
        if (!critter || !memory.Value(entry + 0x18, occupant.id) ||
            !memory.Read(critter + 0x2c, occupant.position, sizeof(occupant.position)))
            continue;
        occupant.isVince = critter == vince;
        occupant.critter = static_cast<uint32_t>(critter);
        ++count;
    }
    return static_cast<int>(count);
}

} // namespace water
} // namespace vince
