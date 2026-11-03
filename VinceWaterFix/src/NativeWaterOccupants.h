#pragma once
#include "NativeWaterMemory.h"

namespace vince {
namespace water {

// Remastered still keeps PuddleRegionMain's occupant list (region entry/exit
// add and remove critters by name) but stubs the FrameTick that rippled the
// puddle for them. These read-only helpers recover the list for the Steam
// build; they never call game code or write game memory.
struct NativePuddleRegion {
    uintptr_t node;   // SwampPuddle node (vtable 0x6e0f0c).
    uintptr_t script; // PuddleRegionMain (vtable 0x6e0fa4), +0x40 = node.
};

// Searches every region node in the RegionManager map (Game 0xb653f4 +0x34)
// for a SwampPuddle child, then takes its region script.
bool FindNativePuddleRegion(const NativeMemory& memory, NativePuddleRegion* region);
// Cheap per-frame recheck of a cached region (vtables and back-pointer).
bool ValidNativePuddleRegion(const NativeMemory& memory, const NativePuddleRegion& region);

struct NativePuddleOccupant {
    uint32_t id;         // Native entry id (+0x18), unique per entry.
    float position[3];   // Critter +0x2c, world space.
    bool isVince;        // World +0x1c.
    char name[32];
    uint32_t critter;    // Native Critter address (a splash's sound source).
};

// Reads up to `capacity` occupants whose critter still exists (FrameTick
// drops the rest). Returns the count, or -1 when the
// list itself cannot be read.
int ReadNativePuddleOccupants(const NativeMemory& memory, uintptr_t script,
                              NativePuddleOccupant* occupants, unsigned capacity);

} // namespace water
} // namespace vince
