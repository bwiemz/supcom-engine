#pragma once

#include "core/types.hpp"
#include "sim/fnv.hpp"

#include <cmath>
#include <optional>
#include <random>

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

    void seed(u64 s) {
        if (mt_) {
            mt_seed(static_cast<u32>(s));
        } else {
            state_ = s;
        }
    }

    /// std::mt19937, Moho's CRandomStream generator (faf-re src/sdk/moho/sim/CRandomStream.cpp),
    /// in place of SplitMix64; seed() after switching.
    void set_mt19937(bool on) { mt_ = on; }
    bool mt19937() const { return mt_; }

    u64 next_u64() {
        if (mt_) {
            const u64 hi = mt_word();
            return (hi << 32) | mt_word();
        }
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

    u32 next_u32() {
        if (mt_) {
            return mt_word();
        }
        return static_cast<u32>(next_u64() >> 32);
    }

    // Uniform f32 in [lo, hi) using a 24-bit mantissa fraction.
    f32 range(f32 lo, f32 hi) {
        if (mt_) {
            return lo + mul_word(hi - lo, mt_word()) * 0x1p-32f;
        }
        f32 unit = static_cast<f32>(next_u64() >> 40) * (1.0f / 16777216.0f);
        return lo + (hi - lo) * unit;
    }

    /// Uniform double in [0, 1), from the top 53 bits.
    double next_double() {
        if (mt_) {
            return static_cast<f32>(mt_word()) * 0x1p-32f;
        }
        return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
    }

    /// Uniform integer in [lo, hi] (inclusive; lo <= hi), without modulo
    /// bias: draws in the incomplete top slice are rejected. Integer
    /// arithmetic only, so every platform gets the same values.
    i64 next_int(i64 lo, i64 hi) {
        const u64 span = static_cast<u64>(hi) - static_cast<u64>(lo) + 1; // 0 = all 2^64
        if (mt_ && span != 0 && span <= 0x100000000ull) {
            return static_cast<i64>(static_cast<u64>(lo) + ((span * mt_word()) >> 32));
        }
        if (span == 0) return static_cast<i64>(next_u64());
        const u64 reject_below = (0 - span) % span; // 2^64 mod span
        u64 r = next_u64();
        while (r < reject_below) r = next_u64();
        return static_cast<i64>(static_cast<u64>(lo) + r % span);
    }

    u64 state() const { return state_; }

    u64 digest() const {
        if (!mt_) {
            return state_;
        }
        Fnv f;
        f.mix(mt_seed_);
        f.mix(mt_drawn_);
        return f.h;
    }

    u64 words_drawn() const { return mt_drawn_; }

    /// `factor * word` rounded to float once, as Moho's x87 code forms it at 24-bit precision.
    static f32 mul_word(f32 factor, u32 word) {
        const u64 w = word < 0x80000000u ? word : static_cast<u64>(static_cast<f32>(word));
        int exp = 0;
        const auto mantissa = static_cast<i64>(std::ldexp(std::frexp(factor, &exp), 24));
        return std::ldexp(static_cast<f32>(mantissa * static_cast<i64>(w)), exp - 24);
    }

private:
    friend struct StateIO;

    void mt_seed(u32 s) {
        mt_seed_ = s;
        mt_drawn_ = 0;
        mt_engine_.emplace(s);
    }

    u32 mt_word() {
        const auto y = static_cast<u32>((*mt_engine_)());
        ++mt_drawn_;
        if (draw_hook_) {
            draw_hook_(draw_hook_ctx_, y);
        }
        return y;
    }

    u64 state_;
    bool mt_ = false;
    std::optional<std::mt19937> mt_engine_;
    u32 mt_seed_ = 0;
    u64 mt_drawn_ = 0;
    DrawHook draw_hook_ = nullptr;
    void* draw_hook_ctx_ = nullptr;
    void* caller_ = nullptr;
};

} // namespace osc::sim
