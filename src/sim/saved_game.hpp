#pragma once

#include "core/sha256.hpp"
#include "core/types.hpp"
#include "sim/replay.hpp"

#include <array>

#include <string>
#include <vector>

namespace osc::sim {

class SimState;

/// Why a saved game won't load, in the words retail's Load dialog shows
/// (`SaveErrors` in ui/dialogs/saveload.lua).
enum class SaveLoadError {
    None,
    CantOpen,      ///< the file can't be read
    InvalidFormat, ///< not a saved game, or a damaged one
    WrongVersion,  ///< saved by another build, or in another save format
    InternalError, ///< it loaded, but didn't replay as it was played
};

/// The name retail's dialog looks the error up by ("CantOpen", ...).
const char* save_load_error_name(SaveLoadError error);

/// A saved game (M208a): the game's history, not its state. The recording
/// holds the setup, every order the sim applied with the tick it ran on,
/// the checksum after every tick, and the orders given before the save that
/// had not run yet. Loading replays it to the saved tick (see
/// SimState::start_resume); since a replay plays back identically on the
/// build that recorded it, the loaded game is the saved one.
struct SavedGame {
    /// 2: the game's snapshot (M208c); 3: its signature
    static constexpr u32 kVersion = 3;

    u32 version = kVersion;
    /// The build that saved it: only that build is known to replay it as it
    /// was played, so any other refuses it (as retail refuses another
    /// version's saves).
    std::string build;
    std::string name; ///< the name the player gave it
    u32 tick = 0;     ///< the tick it was saved on: the game resumes after it
    /// The game's state at `tick` (sim_snapshot.hpp; M208c): a load restores
    /// it at once, where the history would take as long as the game did.
    /// Empty when it couldn't be taken: the load catches up instead.
    std::vector<u8> snapshot;
    /// The snapshot's HMAC-SHA256 under the key of the installation that
    /// saved it (snapshot_key): a load restores only its own installation's
    /// snapshots. A snapshot names C functions by their place in the binary,
    /// so a crafted one could aim a call anywhere; any other save still
    /// loads, by catching up from its history.
    core::Sha256Digest snapshot_mac{};
    Replay game;

    std::vector<u8> serialize() const;
    /// Parse what serialize() wrote. On any error `out` is left empty.
    static SaveLoadError deserialize(const std::vector<u8>& bytes, SavedGame& out);
};

/// The game `sim` is playing, saved as it stands between ticks. `sim` must
/// be recording (SimState::set_recording) since its first tick. With
/// `snapshot`, the save carries the game's state too.
SavedGame save_game(SimState& sim, std::string name, bool snapshot = true);

/// An installation's key for signing its saves' snapshots.
using SnapshotKey = std::array<u8, 32>;
/// Sign `save`'s snapshot with `key`.
void sign_snapshot(SavedGame& save, const SnapshotKey& key);
/// Whether `save` has a snapshot `key` signed.
bool snapshot_signed(const SavedGame& save, const SnapshotKey& key);

} // namespace osc::sim
