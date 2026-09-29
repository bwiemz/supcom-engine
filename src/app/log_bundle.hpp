#pragma once

// What a player sends with a bug report (M228b): `opensupcom --collect-logs
// [file.zip]` zips the game's logs, its crash reports, the engine's settings
// and a description of the system.

#include <filesystem>
#include <optional>
#include <string>

namespace osc::app {

struct LogBundleSources {
    std::filesystem::path logs_dir;      ///< opensupcom*.log: logs/
    std::filesystem::path crash_dir;     ///< *.txt: crashes/
    std::filesystem::path settings_file; ///< settings.json, if there is one
    std::string system;                  ///< system.txt's text
};

/// Write the bundle to `zip` (replacing it). The number of files it holds,
/// or nullopt if the zip couldn't be written.
std::optional<int> write_log_bundle(const std::filesystem::path& zip,
                                    const LogBundleSources& sources);

/// system.txt: the build, the OS, and the GPU the newest log in `logs_dir`
/// names.
std::string system_report(const std::filesystem::path& logs_dir);

/// --collect-logs [file.zip]: the bundle from this user's folders, to
/// `out` (default: opensupcom-logs-<date>-<time>.zip here). The exit code.
int collect_logs(const std::string& out);

} // namespace osc::app
