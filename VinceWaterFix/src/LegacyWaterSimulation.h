#pragma once
#include <stddef.h>
#include <stdint.h>

namespace vince { namespace water {

struct LegacyWaterRandomState { uint32_t value; uint32_t carry; };

// Exact observed BeepLib multiply-with-carry step. The original RNG is shared
// with the rest of the game; the integrating host controls seeding and sharing.
uint32_t AdvanceLegacyWaterRandom(LegacyWaterRandomState& random);

struct LegacyWaterSimulation {
    float* fields[2];
    size_t capacities[2]; // Float elements, including inter-row padding.
    size_t width;
    size_t height;
    size_t rowStride;
    unsigned currentIndex;
    bool wrap;
    bool calm;
};

struct LegacyWaterSimulationParameters {
    float turbulence; // Original constructor .006; authored levels can override.
    float speed;      // Original constructor 4.1.
    float elasticity; // Original constructor .99.
};

enum class SimulationStatus {
    Ok,
    NoChange,
    InvalidDimensions,
    InvalidStride,
    InvalidBuffer,
    InvalidState,
    InvalidParameter,
    MissingRandomState,
    NonfiniteHeight,
    ArithmeticOverflow
};

// Zero both reusable buffers and reset currentIndex=0, calm=false. Padding and
// external RNG state are untouched. The original constructor zeros both fields.
SimulationStatus ResetLegacyWaterSimulation(LegacyWaterSimulation& simulation);

// Original one-step update. dt is the game's frame delta, not a wall-clock value
// chosen by this helper; negative finite dt is preserved. No timestep clamping,
// substeps or stabilizing changes are invented. Random is needed only if
// turbulence !=0. Finite overflow is rejected before buffers/state/RNG change.
SimulationStatus StepLegacyWaterSimulation(LegacyWaterSimulation& simulation,
    LegacyWaterSimulationParameters parameters, float dt,
    LegacyWaterRandomState* random);

// Original sparse initial disturbance. Only the current field changes, with
// reverse traversal and random skips of1..32 samples. Random is needed when
// turbulence !=0. This does not replace Reset or initialize the previous field.
SimulationStatus InitializeLegacySurfaceDistortions(LegacyWaterSimulation& simulation,
    float turbulence, LegacyWaterRandomState* random);

// Original single-cell impulse: truncate(width*u), truncate(height*v), wrap,
// then add amount to current height. Coordinates may lie outside [0,1].
// Even when wrap=false this operation wraps; only the update boundary changes.
SimulationStatus PerturbLegacyWater(LegacyWaterSimulation& simulation,
    float u, float v, float amount);

} }
