#pragma once

#include "platform/paths.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace osc::platform {

/// Where to look first and last: the command line's locations (any subset may
/// be set), and the folder the player chose.
struct GameInstallHints {
    std::optional<std::filesystem::path> fa_path;       ///< --fa-path
    std::optional<std::filesystem::path> init_file;     ///< --init
    std::optional<std::filesystem::path> faf_data_path; ///< --faf-data
    /// The FA folder the player chose when none was found (the engine's
    /// settings, M228a): the last place looked, so an install found later
    /// (FAF, Steam) takes over.
    std::optional<std::filesystem::path> chosen_fa_path;
};

/// A resolved game installation.
struct GameInstall {
    std::filesystem::path fa_path;       ///< FA install dir (holds gamedata/)
    std::filesystem::path init_file;     ///< init script that builds the VFS
    std::filesystem::path faf_data_path; ///< FAF data dir; empty for retail
    std::string source;                  ///< "cli", "env", "faf", "steam" or "chosen"
};

struct GameInstallSearch {
    std::optional<GameInstall> install;
    /// Every location considered, in order, for diagnostics when nothing
    /// (or something unexpected) was found.
    std::vector<std::string> searched;
};

/// Extract the path from FAF's `fa_path.lua` (`fa_path = "C:\\..."`),
/// normalised to forward slashes.
std::optional<std::filesystem::path> parse_fa_path_lua(std::string_view text);

/// Find FA with this precedence:
///   1. CLI hints (--init / --fa-path / --faf-data)
///   2. Environment: OSC_INIT_FILE, OSC_FA_PATH, OSC_FAF_DATA
///   3. A FAForever data dir with bin/init_faf.lua (Windows:
///      %ProgramData%/FAForever, Linux: ~/.faforever)
///   4. Steam app 9420 in any Steam library -> retail bin/SupComDataPath.lua
///   5. The folder the player chose (hints.chosen_fa_path)
/// When FAF is chosen but its fa_path.lua is missing, FA itself is taken from
/// Steam. A lone FA path implies the retail init file inside it.
GameInstallSearch locate_game_install(const GameInstallHints& hints,
                                      const EnvLookup& env);

/// Why `dir` isn't an FA install, for the player to read; nullopt when it
/// is. FA's init file (bin/SupComDataPath.lua) and its main archive
/// (gamedata/lua.scd) must be there, in any case, as Windows wrote them.
std::optional<std::string> fa_install_problem(const std::filesystem::path& dir);

/// The FA install a folder the player picked means: that folder, or its
/// parent when they picked FA's bin or gamedata folder; nullopt if neither
/// holds FA.
std::optional<std::filesystem::path> resolve_fa_folder(const std::filesystem::path& picked);

} // namespace osc::platform
