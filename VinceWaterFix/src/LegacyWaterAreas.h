#pragma once

namespace vince::water {

// Values authored in the supplied Xbox retail build, verified against its HOT archives.
// Source: levels/<chapter>/area_<area>/world.hot -> data.hot/index.tdf.
// Consult presentFields before using a value: absent properties contain zero only
// as POD storage, not as an inferred engine default. Speed/turbulence/material IDs
// are original renderer properties, not Remaster WaveAnimSpeed/WaveStrength.
enum LegacyWaterFields : unsigned {
    HasColor = 1u << 0,
    HasSpeed = 1u << 1,
    HasTurbulence = 1u << 2,
    HasMaterialId = 1u << 3,
};

struct LegacyWaterArea {
    const char* area;
    const char* model;
    unsigned presentFields;
    float color[4];
    float speed;
    float turbulence;
    int materialId;
};

inline constexpr LegacyWaterArea kLegacyWaterAreas[] = {
    {"belowdecks", "water", HasColor | HasMaterialId, {0.443137f, 0.403922f, 0.239216f, 0.710740f}, 0.000000f, 0.000000f, 2},
    {"crawdadjimmys", "water", HasColor | HasMaterialId, {0.443137f, 0.403922f, 0.239216f, 0.710740f}, 0.000000f, 0.000000f, 2},
    {"earthwoodandwater", "water", 0, {0.000000f, 0.000000f, 0.000000f, 0.000000f}, 0.000000f, 0.000000f, 0},
    {"hurricanehannah", "water", 0, {0.000000f, 0.000000f, 0.000000f, 0.000000f}, 0.000000f, 0.000000f, 0},
    {"jeanlafittesship", "water", HasColor | HasMaterialId, {0.172549f, 0.443137f, 0.321569f, 0.892560f}, 0.000000f, 0.000000f, 2},
    {"jimmysfanboat", "water", HasColor | HasSpeed | HasMaterialId, {0.152941f, 0.164706f, 0.109804f, 0.545460f}, 0.100000f, 0.000000f, 2},
    {"thebogwallow", "water", HasColor | HasSpeed | HasMaterialId, {0.443137f, 0.403922f, 0.239216f, 0.710740f}, 0.400000f, 0.000000f, 2},
    {"midway", "water", HasColor | HasMaterialId, {0.035294f, 0.219608f, 0.180392f, 0.892560f}, 0.000000f, 0.000000f, 2},
    {"bigpig", "water", HasColor | HasSpeed | HasMaterialId, {0.035294f, 0.219608f, 0.180392f, 0.892560f}, 0.500000f, 0.000000f, 2},
    {"thesquare", "water", HasColor | HasSpeed | HasMaterialId, {0.035294f, 0.219608f, 0.180392f, 0.892560f}, 0.500000f, 0.000000f, 4},
    {"tintoyterror", "water", HasColor | HasSpeed | HasTurbulence | HasMaterialId, {0.000000f, 0.023529f, 0.023529f, 0.785120f}, 0.100000f, 0.001000f, 2},
    {"basketcase", "water", HasColor | HasSpeed | HasTurbulence | HasMaterialId, {0.184314f, 0.227451f, 0.000000f, 0.837600f}, 0.150000f, 0.015000f, 2},
    {"maintunnel", "water", HasColor | HasSpeed | HasMaterialId, {0.062745f, 0.380392f, 0.309804f, 0.685960f}, 0.300000f, 0.000000f, 5},
    {"propellerroom", "water", HasColor | HasSpeed | HasMaterialId, {0.439216f, 0.243137f, 0.388235f, 0.776860f}, 0.500000f, 0.000000f, 2},
    {"wrathofjanice", "water", HasColor | HasSpeed | HasMaterialId, {0.145098f, 0.180392f, 0.000000f, 1.000000f}, 0.200000f, 0.000000f, 2},
};

// Retail Below Decks has one colored water entity, matching the Remaster mapping.
// The preview had two entities; its extra uncolored surface is not retail content.
// Omitted colors in Earth Wood and Water/Hurricane Hannah require renderer defaults.

} // namespace vince::water
