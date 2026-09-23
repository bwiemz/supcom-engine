#pragma once

#include "core/types.hpp"
#include "sim/command_scheduler.hpp"
#include "sim/sim_random.hpp"

#include <vector>

namespace osc::sim {

/// A recorded match: the ordered stream of scheduled commands plus enough
/// header to re-drive a fresh, deterministic simulation. Because the engine is
/// lockstep-deterministic, re-feeding this command stream reproduces the match
/// exactly (verified against `SimState::compute_sync_checksum`) -- provided
/// the new sim is seeded with `seed` before any of it runs, since boot
/// scripts roll numbers too.
struct Replay {
    static constexpr u32 kVersion = 3; // 2: the game's seed; 3: SimCallbacks

    u32 version = kVersion;
    u32 final_tick = 0;               // last tick the recording covers
    u32 command_delay = 0;            // scheduler delay in effect
    u64 seed = SimRandom::kDefaultSeed; // the game's random seed
    std::string victory_condition;    // game-mode context (informational)
    std::vector<ScheduledCommand> commands; // in submission order

    /// Serialize to a self-describing little-endian byte buffer.
    std::vector<u8> serialize() const;

    /// Parse a buffer produced by serialize(). Returns false on bad magic /
    /// version / truncation (out is left in a valid-but-empty state).
    static bool deserialize(const std::vector<u8>& bytes, Replay& out);
};

} // namespace osc::sim
