#pragma once

#include "core/types.hpp"

namespace osc::sim {

// Deterministic SplitMix64 RNG. It lives in sim state so that every client fed
// the same seed and the same call sequence produces identical values — the
// determinism lockstep multiplayer (and replays) depend on. Never advance it
// from render/UI code; only from the deterministic sim tick path.
class SimRandom {
public:
    explicit SimRandom(u64 seed = 0x9E3779B97F4A7C15ull) : state_(seed) {}

    void seed(u64 s) { state_ = s; }

    u64 next_u64() {
        u64 z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    u32 next_u32() { return static_cast<u32>(next_u64() >> 32); }

    // Uniform f32 in [lo, hi) using a 24-bit mantissa fraction.
    f32 range(f32 lo, f32 hi) {
        f32 unit = static_cast<f32>(next_u64() >> 40) * (1.0f / 16777216.0f);
        return lo + (hi - lo) * unit;
    }

    u64 state() const { return state_; }

private:
    u64 state_;
};

} // namespace osc::sim
