#pragma once

#include "platform/paths.hpp"

#include <filesystem>
#include <optional>

namespace osc::platform {

/// The engine's own settings (M228a), apart from FA's Game.prefs:
/// <Config>/opensupcom/settings.json. What this version doesn't know of a
/// file another version wrote is kept when it saves.
struct EngineSettings {
    /// The FA folder the player chose when none was found.
    std::optional<std::filesystem::path> fa_path;
};

/// Where the settings live: <Config>/opensupcom/settings.json.
std::filesystem::path engine_settings_path(const EnvLookup& env);

/// A player's game's log (M228b): <State>/opensupcom/logs/opensupcom.log,
/// the last runs' beside it (opensupcom.1.log, ...).
std::filesystem::path engine_log_file(const EnvLookup& env);

/// Where crash reports go: <State>/opensupcom/crashes.
std::filesystem::path engine_crash_dir(const EnvLookup& env);

/// The settings in `file`: defaults when it is missing or can't be read.
EngineSettings load_engine_settings(const std::filesystem::path& file);

/// Write `settings` to `file`, making its folder if need be and keeping the
/// keys it holds that EngineSettings doesn't. The file is replaced whole (a
/// temporary, then a rename), never left half-written. False on failure.
bool save_engine_settings(const std::filesystem::path& file, const EngineSettings& settings);

} // namespace osc::platform
