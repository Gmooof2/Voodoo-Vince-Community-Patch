#pragma once
#include "LegacyWaterSimulation.h"

namespace vince::water {

// Authored [puddle] entities in the supplied Xbox retail build, identical in
// Remastered apart from signed zeros. Source: levels/<chapter>/area_<area>/
// world.hot -> data.hot/index.tdf. Main Tunnel holds the game's only puddle;
// Main Street's [gaspuddle] is a different script. Neither build authors the
// puddle's waterdensity/speed/turbulence/elasticity/verticalness or perturbs.
struct LegacyWaterPuddle {
    const char* area;
    float location[3];
    float rotation[4]; // Quaternion x,y,z,w.
    float scale[3];    // Plane region extents (node+0xc0..0xc8), NOT node scale.
    float color[4];
    unsigned fieldSize;
    int materialId; // Retail material 4 is empty: no textures, blend 0.
};

inline constexpr LegacyWaterPuddle kLegacyWaterPuddles[] = {
    {"maintunnel", {7.680558f, -0.395000f, -2.225962f}, {0.000000f, 0.292148f, 0.000000f, 0.956373f},
     {0.500000f, 0.010000f, 0.500000f}, {0.447059f, 0.294118f, 0.117647f, 0.650000f}, 32, 4},
};

// Values the preview applies when the properties above are absent.
struct LegacyPuddleDefaults {
    float density; // Also the quad's bump-UV range.
    LegacyWaterSimulationParameters simulation;
    float verticalness;
    bool wrap;
    float quadScale;    // Quad half-extent = region extent * quadScale.
    float reflectivity; // Empty material's loaded reflectivity.
    float perturbSplash, perturbMove, perturbStill;
};

inline constexpr LegacyPuddleDefaults kOriginalPuddleDefaults = {
    1.0f, {0.0f, 6.0f, 0.95f}, 0.25f, false, 0.5f, 0.0f, 1.5f, 1.75f, 0.08f};

// Evidence in supplied default.exe:
// PuddleRegionMain::Load 0x51389b: density default 1 (0x51392f), fieldsize 64,
// perturb splash/move/still 1.5/1.75/.08 (0x5b5e98,0x5e0490,0x5e0470).
// SwampPuddle ctor 0x53a6b0 passes wrap=false to its field (0x53a6c0), then sets
// turbulence 0, speed 6, elasticity .95, verticalness .25 (0x53a823..0x53a847);
// Load overrides each only when authored. Quad: (+-hx,0,+-hz) with h=extent*.5
// (0x5b3058), bump UVs 0..density, normal (0,1,0), stride 0x30 (0x53a71a..0x53a815).
// Region box setter 0x4b3e00 copies Scale to node+0xc0 instead of the transform.
// MaterialResource loader 0x4c127e replaces its 1.0 default with absent
// reflectivity 0. SwampPuddle::Render 0x53a8d4 enables depth writes (0x4035c=1)
// and draws overlay only for detail types outside 0 and 13..15.

} // namespace vince::water
