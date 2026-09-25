#pragma once

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

    explicit SpecialFiles(std::filesystem::path root) : root_(std::move(root)) {}

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

private:
    std::filesystem::path root_;
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
