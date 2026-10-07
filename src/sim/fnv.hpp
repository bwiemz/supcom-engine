#pragma once

// The sync checksum's hash (SimState::checksum_parts), shared with the
// parts that fingerprint their own state into it (the path code).

#include "core/types.hpp"

#include <cstring>

namespace osc::sim {

/// FNV-1a over 64-bit words.
struct Fnv {
    u64 h = 1469598103934665603ULL;
    void mix(u64 v) {
        h ^= v;
        h *= 1099511628211ULL;
    }
    void mix_f32(f32 f) {
        u32 bits;
        std::memcpy(&bits, &f, sizeof(bits));
        mix(bits);
    }
};

} // namespace osc::sim
