#pragma once

#include "platform/paths.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace osc::platform {

/// Explicit locations from the command line; any subset may be set.
struct GameInstallHints {
    std::optional<std::filesystem::path> fa_path;       ///< --fa-path
    std::optional<std::filesystem::path> init_file;     ///< --init
    std::optional<std::filesystem::path> faf_data_path; ///< --faf-data
};

/// A resolved game installation.
struct GameInstall {
    std::filesystem::path fa_path;       ///< FA install dir (holds gamedata/)
    std::filesystem::path init_file;     ///< init script that builds the VFS
    std::filesystem::path faf_data_path; ///< FAF data dir; empty for retail
    std::string source;                  ///< "cli", "env", "faf" or "steam"
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
/// When FAF is chosen but its fa_path.lua is missing, FA itself is taken from
/// Steam. A lone FA path implies the retail init file inside it.
GameInstallSearch locate_game_install(const GameInstallHints& hints,
                                      const EnvLookup& env);

} // namespace osc::platform
