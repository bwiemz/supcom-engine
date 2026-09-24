#pragma once

#include "core/types.hpp"

namespace osc::sim {

// Deterministic SplitMix64 RNG. It lives in sim state so that every client fed
// the same seed and the same call sequence produces identical values — the
// determinism lockstep multiplayer (and replays) depend on. Never advance it
// from render/UI code; only from the deterministic sim tick path.
class SimRandom {
public:
    /// The seed of a game that names none (tests, headless runs, captures).
    static constexpr u64 kDefaultSeed = 0x9E3779B97F4A7C15ull;

    explicit SimRandom(u64 seed = kDefaultSeed) : state_(seed) {}

    void seed(u64 s) { state_ = s; }

    u64 next_u64() {
        u64 z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= (z >> 31);
        if (draw_hook_) draw_hook_(draw_hook_ctx_, z);
        return z;
    }

    /// Called with every value drawn (--rng-trace); null when off.
    using DrawHook = void (*)(void* ctx, u64 value);
    void set_draw_hook(DrawHook hook, void* ctx) {
        draw_hook_ = hook;
        draw_hook_ctx_ = ctx;
    }
    /// The Lua state drawing now (Random, math.random), so a trace can say
    /// which script drew; null for the engine's own draws.
    void set_caller(void* lua_state) { caller_ = lua_state; }
    void* caller() const { return caller_; }

    u32 next_u32() { return static_cast<u32>(next_u64() >> 32); }

    // Uniform f32 in [lo, hi) using a 24-bit mantissa fraction.
    f32 range(f32 lo, f32 hi) {
        f32 unit = static_cast<f32>(next_u64() >> 40) * (1.0f / 16777216.0f);
        return lo + (hi - lo) * unit;
    }

    /// Uniform double in [0, 1), from the top 53 bits.
    double next_double() {
        return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
    }

    /// Uniform integer in [lo, hi] (inclusive; lo <= hi), without modulo
    /// bias: draws in the incomplete top slice are rejected. Integer
    /// arithmetic only, so every platform gets the same values.
    i64 next_int(i64 lo, i64 hi) {
        const u64 span = static_cast<u64>(hi) - static_cast<u64>(lo) + 1; // 0 = all 2^64
        if (span == 0) return static_cast<i64>(next_u64());
        const u64 reject_below = (0 - span) % span; // 2^64 mod span
        u64 r = next_u64();
        while (r < reject_below) r = next_u64();
        return static_cast<i64>(static_cast<u64>(lo) + r % span);
    }

    u64 state() const { return state_; }

private:
    u64 state_;
    DrawHook draw_hook_ = nullptr;
    void* draw_hook_ctx_ = nullptr;
    void* caller_ = nullptr;
};

} // namespace osc::sim
