#pragma once
#include <stdint.h>
#include "LegacyWaterSimulation.h"

namespace vince {
namespace water {

// Defaults actually applied by preview WaterRegionMain::Load (0x51301d), not
// the raw WaterField constructor. Each authored TDF assignment overrides only
// its matching property. These constants apply to model-based WaterRegion,
// not the separate hand-built QuadWaterNode constructor path.
struct LegacyWaterRegionSettings {
    uint32_t fieldSize;
    float density;
    LegacyWaterSimulationParameters simulation;
    float verticalness;
    bool wrap;
    float color[4];
};

constexpr LegacyWaterRegionSettings kOriginalWaterRegionDefaults = {
    32,   1.0f, {0.004f, 0.5f, 0.9992f}, // turbulence,speed,elasticity
    0.8f, true, {0.475f, 0.447f, 0.243f, 0.5f}};

// Evidence in supplied default.exe:
// 0x5130b5 density1;0x5130e5 fieldsize32;
// 0x51326e speed=.5(double0x5b71a0),0x5132ab turbulence=.004(0x5e02f0),
// 0x5132f6 elasticity=.9992(0x5e02d8),0x513333 verticalness=.8(0x5e02b8).
// 0x53a9bf passes wrap=true. Color falls back to WaterField's constructor
// values at0x53a337..0x53a34d when no color record exists (branch0x51321e).
// Raw WaterField defaults4.1/.006/.99 are overwritten by the region loader.

} // namespace water
} // namespace vince
