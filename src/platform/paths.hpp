#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace osc::platform {

/// Reads an environment variable; injectable so path policy is testable
/// without mutating the process environment.
using EnvLookup = std::function<std::optional<std::string>(const char*)>;

/// EnvLookup over the real process environment.
EnvLookup system_env();

/// Per-user directories. On Windows these are the Known Folders; on POSIX
/// they follow the XDG Base Directory spec.
enum class KnownFolder {
    Documents,    ///< FA's SHGetFolderPath('PERSONAL'): "My Games/..." lives here
    LocalAppData, ///< FA's SHGetFolderPath('LOCAL_APPDATA'): prefs, cache
    Config,       ///< engine settings (XDG_CONFIG_HOME)
    Cache,        ///< regenerable data (XDG_CACHE_HOME)
    State,        ///< logs, crash reports (XDG_STATE_HOME)
};

/// The user's home directory. POSIX: $HOME, else the passwd entry.
std::filesystem::path home_dir(const EnvLookup& env);

/// Resolve a known folder. Always returns an absolute path; the directory
/// is not created.
std::filesystem::path known_folder(KnownFolder folder, const EnvLookup& env);
std::filesystem::path known_folder(KnownFolder folder);

} // namespace osc::platform
