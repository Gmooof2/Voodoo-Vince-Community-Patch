#include "LegacyWaterSimulation.h"

namespace vince { namespace water {
namespace {
const double MaxFloat = 3.4028234663852886e+38;
bool Finite(float x) { return x >= -MaxFloat && x <= MaxFloat; }
bool FitsFloat(double x) { return x >= -MaxFloat && x <= MaxFloat; }
bool PowerOfTwo(size_t x) { return x && !(x & (x - 1)); }
double Abs(double x) { return x < 0.0 ? -x : x; }

SimulationStatus Validate(const LegacyWaterSimulation& s, bool inspectHeights) {
    const size_t maximum = static_cast<size_t>(-1);
    if (!PowerOfTwo(s.width) || !PowerOfTwo(s.height)
        || s.width > 0x40000000u || s.height > 0x40000000u
        || (!s.wrap && (s.width < 2 || s.height < 2)))
        return SimulationStatus::InvalidDimensions;
    if (s.rowStride < s.width || (s.height - 1) > (maximum - s.width) / s.rowStride)
        return SimulationStatus::InvalidStride;
    const size_t required = (s.height - 1) * s.rowStride + s.width;
    if (required > maximum / sizeof(float)
        || !s.fields[0] || !s.fields[1]
        || s.capacities[0] < required || s.capacities[1] < required)
        return SimulationStatus::InvalidBuffer;
    // A partial overlap also breaks the original double-buffer recurrence.
    const uintptr_t a = reinterpret_cast<uintptr_t>(s.fields[0]);
    const uintptr_t b = reinterpret_cast<uintptr_t>(s.fields[1]);
    const size_t bytes = required * sizeof(float);
    if (a > maximum - bytes || b > maximum - bytes
        || (a < b + bytes && b < a + bytes))
        return SimulationStatus::InvalidBuffer;
    if (s.currentIndex > 1) return SimulationStatus::InvalidState;
    if (inspectHeights)
        for (unsigned buffer = 0; buffer < 2; ++buffer)
            for (size_t z = 0; z < s.height; ++z)
                for (size_t x = 0; x < s.width; ++x)
                    if (!Finite(s.fields[buffer][z * s.rowStride + x]))
                        return SimulationStatus::NonfiniteHeight;
    return SimulationStatus::Ok;
}

double RandomRange(LegacyWaterRandomState& state, float turbulence) {
    return double(AdvanceLegacyWaterRandom(state)) * (2.0 * double(turbulence))
         * (1.0 / 4294967296.0) - double(turbulence);
}

size_t Before(size_t index, size_t dimension, bool wrap) {
    return wrap ? ((index - 1) & (dimension - 1)) : (index ? index - 1 : 0);
}
size_t After(size_t index, size_t dimension, bool wrap) {
    return wrap ? ((index + 1) & (dimension - 1)) : (index + 1 < dimension ? index + 1 : index);
}

double NextHeightDouble(const LegacyWaterSimulation& s, size_t x, size_t z,
                       double q, double centerWeight, double damping) {
    const float* current = s.fields[s.currentIndex];
    const float* previous = s.fields[s.currentIndex ^ 1];
    const size_t index = z * s.rowStride + x;
    // Sum order follows the original wrapped path. The bounded path changes
    // its summation order at edges, which can alter final rounding by a few ulps.
    const double left = current[z * s.rowStride + Before(x,s.width,s.wrap)];
    const double right = current[z * s.rowStride + After(x,s.width,s.wrap)];
    const double above = current[Before(z,s.height,s.wrap) * s.rowStride + x];
    const double below = current[After(z,s.height,s.wrap) * s.rowStride + x];
    return ((left + right + above + below) * q
        + centerWeight * current[index] - previous[index]) * damping;
}

} // namespace

uint32_t AdvanceLegacyWaterRandom(LegacyWaterRandomState& random) {
    const uint64_t product = uint64_t(random.value) * 0x7f13ac69u + random.carry;
    random.value = static_cast<uint32_t>(product);
    random.carry = static_cast<uint32_t>(product >> 32);
    return random.value;
}

SimulationStatus ResetLegacyWaterSimulation(LegacyWaterSimulation& s) {
    const SimulationStatus status = Validate(s,false);
    if (status != SimulationStatus::Ok) return status;
    for (unsigned buffer = 0; buffer < 2; ++buffer)
        for (size_t z = 0; z < s.height; ++z)
            for (size_t x = 0; x < s.width; ++x)
                s.fields[buffer][z * s.rowStride + x] = 0.f;
    s.currentIndex = 0;
    s.calm = false;
    return SimulationStatus::Ok;
}

SimulationStatus StepLegacyWaterSimulation(LegacyWaterSimulation& s,
    LegacyWaterSimulationParameters p, float dt, LegacyWaterRandomState* random) {
    const SimulationStatus status = Validate(s,true);
    if (status != SimulationStatus::Ok) return status;
    if (!Finite(p.turbulence) || p.turbulence < 0.f || !Finite(p.speed)
        || !Finite(p.elasticity) || !Finite(dt)) return SimulationStatus::InvalidParameter;
    if ((s.calm && p.turbulence == 0.f) || Abs(dt) < double(1.0e-9f))
        return SimulationStatus::NoChange;
    if (p.turbulence != 0.f && !random) return SimulationStatus::MissingRandomState;
    const double scaledStep = double(dt) * p.speed;
    const double q = scaledStep * scaledStep;
    const double centerWeight = 2.0 - 4.0 * q;
    const double damping = double(p.elasticity) - double(dt) * double(.01f);
    LegacyWaterRandomState finalRandom = random ? *random : LegacyWaterRandomState{0,0};
    // Preflight uses the same reverse RNG order as the original turbulence pass.
    // No buffer writes occur until every resulting float is known to be finite.
    for (size_t z = s.height; z-- > 0;)
        for (size_t x = s.width; x-- > 0;) {
            const double raw = NextHeightDouble(s,x,z,q,centerWeight,damping);
            if (!FitsFloat(raw)) return SimulationStatus::ArithmeticOverflow;
            const float height = static_cast<float>(raw);
            if (p.turbulence != 0.f && Abs(height) < 2.0 * p.turbulence) {
                const double disturbed = double(height) + RandomRange(finalRandom,p.turbulence) * dt;
                if (!FitsFloat(disturbed)) return SimulationStatus::ArithmeticOverflow;
            }
        }
    LegacyWaterRandomState writeRandom = random ? *random : LegacyWaterRandomState{0,0};
    bool calm = p.turbulence == 0.f;
    for (size_t z = s.height; z-- > 0;)
        for (size_t x = s.width; x-- > 0;) {
            float height = static_cast<float>(NextHeightDouble(s,x,z,q,centerWeight,damping));
            // Preserve the preview's calm-test quirk: bounded edge columns
            // are not included; wrapped mode includes every sample.
            if ((s.wrap || (x > 0 && x + 1 < s.width))
                && double(height) * height > double(5.0e-6f)) calm = false;
            if (p.turbulence != 0.f && Abs(height) < 2.0 * p.turbulence)
                height = static_cast<float>(double(height) + RandomRange(writeRandom,p.turbulence) * dt);
            s.fields[s.currentIndex ^ 1][z * s.rowStride + x] = height;
        }
    if (random && p.turbulence != 0.f) *random = writeRandom;
    s.currentIndex ^= 1;
    s.calm = calm;
    return SimulationStatus::Ok;
}

SimulationStatus InitializeLegacySurfaceDistortions(LegacyWaterSimulation& s,
    float turbulence, LegacyWaterRandomState* random) {
    const SimulationStatus status = Validate(s,true);
    if (status != SimulationStatus::Ok) return status;
    if (!Finite(turbulence) || turbulence < 0.f) return SimulationStatus::InvalidParameter;
    if (turbulence == 0.f) return SimulationStatus::NoChange;
    if (!random) return SimulationStatus::MissingRandomState;
    // Logical sample count cannot overflow after Validate's stride/storage check.
    size_t remaining = s.width * s.height;
    while (remaining) {
        const size_t sample = remaining - 1;
        s.fields[s.currentIndex][(sample / s.width) * s.rowStride + sample % s.width]
            = static_cast<float>(RandomRange(*random,turbulence) * .75);
        const size_t skip = 1 + (AdvanceLegacyWaterRandom(*random) & 31);
        remaining = remaining > skip ? remaining - skip : 0;
    }
    s.calm = false;
    return SimulationStatus::Ok;
}

SimulationStatus PerturbLegacyWater(LegacyWaterSimulation& s,
    float u, float v, float amount) {
    const SimulationStatus status = Validate(s,true);
    if (status != SimulationStatus::Ok) return status;
    if (!Finite(u) || !Finite(v) || !Finite(amount)) return SimulationStatus::InvalidParameter;
    const float scaledU = static_cast<float>(s.width) * u;
    const float scaledV = static_cast<float>(s.height) * v;
    if (!(double(scaledU) >= -2147483648.0 && double(scaledU) < 2147483648.0)
        || !(double(scaledV) >= -2147483648.0 && double(scaledV) < 2147483648.0))
        return SimulationStatus::InvalidParameter;
    const size_t x = static_cast<uint32_t>(static_cast<int32_t>(scaledU)) & (s.width - 1);
    const size_t z = static_cast<uint32_t>(static_cast<int32_t>(scaledV)) & (s.height - 1);
    float& height = s.fields[s.currentIndex][z * s.rowStride + x];
    const double next = double(height) + amount;
    if (!FitsFloat(next)) return SimulationStatus::ArithmeticOverflow;
    height = static_cast<float>(next);
    s.calm = false;
    return SimulationStatus::Ok;
}

} }
