#pragma once

#include "core/types.hpp"
#include "sim/command_scheduler.hpp"
#include "sim/game_setup.hpp"
#include "sim/sim_random.hpp"

#include <string>
#include <vector>

namespace osc::sim {

/// A recorded match: the ordered stream of scheduled commands plus enough
/// header to re-drive a fresh, deterministic simulation. Because the engine is
/// lockstep-deterministic, re-feeding this command stream reproduces the match
/// exactly (verified against `SimState::compute_sync_checksum`) -- provided
/// the new sim is seeded with `seed` before any of it runs, since boot
/// scripts roll numbers too.
struct Replay {
    // 2: the game's seed; 3: SimCallbacks; 4: the game's setup, the build,
    // and a checksum trail; 5: formation orders; 6: a specific unload's
    // cargo; 7: factory commands (a player's rally orders)
    static constexpr u32 kVersion = 7;

    u32 version = kVersion;
    u32 final_tick = 0;               // last tick the recording covers
    u32 command_delay = 0;            // scheduler delay in effect
    u64 seed = SimRandom::kDefaultSeed; // the game's random seed
    std::string victory_condition;    // game-mode context (informational)
    std::string build;                // the build that recorded it
    /// The setup the game started from; a replay without one (before
    /// version 4, or recorded by a bare SimState) can't start a game.
    bool has_setup = false;
    GameSetup setup;
    /// The sync checksum after each tick from `checksum_from` on.
    u32 checksum_from = 0;
    std::vector<u32> checksums;
    std::vector<ScheduledCommand> commands; // as the sim applied them

    /// Serialize to a self-describing little-endian byte buffer.
    std::vector<u8> serialize() const;

    /// Parse a buffer produced by serialize(). Returns false on bad magic /
    /// version / truncation (out is left in a valid-but-empty state).
    static bool deserialize(const std::vector<u8>& bytes, Replay& out);
};

class SimState;

/// Plays a replay into a sim started from its setup: queues its commands
/// (the game's only input from then on), then checks the sim's checksum
/// after each tick against the recording's.
class ReplayPlayback {
public:
    explicit ReplayPlayback(Replay replay) : replay_(std::move(replay)) {}

    void start(SimState& sim) const;
    /// Start it as a saved game instead: the player takes over after its
    /// last tick (SimState::start_resume). check() works the same.
    void resume(SimState& sim) const;
    /// After a tick: false from the first tick whose checksum differs.
    bool check(const SimState& sim);
    /// Whether the sim has played every recorded tick.
    bool finished(const SimState& sim) const;
    /// The first tick whose checksum differed (0 = none yet).
    u32 diverged_at() const { return diverged_at_; }
    const Replay& replay() const { return replay_; }

private:
    Replay replay_;
    u32 diverged_at_ = 0;
};

} // namespace osc::sim
