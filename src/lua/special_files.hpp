#pragma once

#include "core/sha256.hpp"
#include "sim/replay.hpp"
#include "sim/saved_game.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace osc::lua {

class LuaState;

/// FA's per-profile "special files": replays and saved games. Each type has
/// a folder under the user's game folder, one subfolder per profile, and an
/// extension. Ours differ from FA's (.oscreplay, not .SCFAReplay), so the
/// two never read each other's files even where they share a folder.
class SpecialFiles {
public:
    struct Type {
        const char* name;      // as the scripts ask for it: "Replay"
        const char* folder;    // under the root: "replays"
        const char* extension; // without the dot
    };

    /// The running binary's identity, which a snapshot it takes carries:
    /// its linker build id, else its file's digest. None if neither can be
    /// read: its snapshots are then neither signed nor trusted.
    static std::optional<core::Sha256Digest> running_binary();

    /// Signing and trusting the running binary's snapshots.
    explicit SpecialFiles(std::filesystem::path root) : root_(std::move(root)) {}
    /// Signing and trusting `binary`'s instead (a test's other one; none:
    /// a binary that can't tell its identity).
    SpecialFiles(std::filesystem::path root, std::optional<core::Sha256Digest> binary)
        : root_(std::move(root)), binary_(binary), binary_given_(true) {}

    /// FA's own user folder: Documents/My Games/Gas Powered Games/Supreme
    /// Commander Forged Alliance.
    static std::filesystem::path default_root();
    /// The type called `name`, or null.
    static const Type* find_type(std::string_view name);

    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path directory(const Type& type) const { return root_ / type.folder; }
    /// Whether a profile or file name is one plain path component (not
    /// empty, ".", ".." or anything with a separator or drive colon).
    static bool plain_name(std::string_view name);
    /// <directory>/<profile>/<base>.<extension>; empty unless both names are
    /// plain, so no name -- from a script or a stored preference -- reaches
    /// outside the folder (an absolute one would replace it outright).
    std::filesystem::path path(const Type& type, std::string_view profile,
                               std::string_view base) const;
    /// Each profile's files of a type (base names, sorted).
    std::map<std::string, std::vector<std::string>> list(const Type& type) const;
    /// Whether `file` is one path() names: a file of this type, in a
    /// profile's folder. The engine writes a file a script names only there.
    bool holds(const Type& type, const std::filesystem::path& file) const;
    /// The key this installation signs its saves' snapshots with (M208c):
    /// made at random the first time, and kept in the folder. None when it
    /// can't be read or made: snapshots are then neither signed nor trusted.
    std::optional<sim::SnapshotKey> snapshot_key() const;
    /// Sign `save`'s snapshot as this installation's, taken by this binary.
    /// False (left unsigned: a load catches up) without a key or the
    /// binary's identity.
    bool sign_snapshot(sim::SavedGame& save) const;
    /// Whether `save`'s snapshot is this installation's and was taken by
    /// this binary, so it may be restored; if not, `why` says what it isn't.
    bool trusts_snapshot(const sim::SavedGame& save, std::string& why) const;

private:
    /// Whose snapshots it signs and trusts: the running binary's (read at
    /// the first save or load: where it has no build id, its file is read
    /// whole), or the one it was given.
    std::optional<core::Sha256Digest> binary() const {
        return binary_given_ ? binary_ : running_binary();
    }

    std::filesystem::path root_;
    std::optional<core::Sha256Digest> binary_;
    bool binary_given_ = false;
};

/// The UI's special-file globals: GetSpecialFiles, GetSpecialFilePath,
/// GetSpecialFileInfo and RemoveSpecialFile; the replays' CopyCurrentReplay
/// and LaunchReplaySession; the saved games' InternalSaveGame and
/// LoadSavedGame.
void register_special_file_bindings(LuaState& state, SpecialFiles* files);

/// The SpecialFiles registered for L's state, or null.
SpecialFiles* get_special_files(lua_State* L);

/// Write a replay file (creating its folder); false (logged) on failure.
bool write_replay_file(const sim::Replay& replay, const std::filesystem::path& path);
/// A replay file that can start its game, or nothing (the reason logged).
std::optional<sim::Replay> read_replay_file(const std::filesystem::path& path);

/// Write a saved game (creating its folder), replacing the file only once
/// the new one is whole, as Moho does; false (logged) on failure.
bool write_saved_game(const sim::SavedGame& save, const std::filesystem::path& path);
/// Read a saved game; on an error (logged) `out` is left empty.
sim::SaveLoadError read_saved_game(const std::filesystem::path& path, sim::SavedGame& out);

} // namespace osc::lua
