#pragma once

#include "platform/paths.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace osc::platform {

/// Steam AppID of Supreme Commander: Forged Alliance.
inline constexpr std::uint32_t kForgedAllianceAppId = 9420;

/// One node of Valve's KeyValues text format ("VDF"): either a string value
/// or an object with ordered children.
struct VdfNode {
    std::string key;
    std::string value;
    std::vector<VdfNode> children;
    bool is_object = false;

    /// First child with this key (ASCII case-insensitive), or nullptr.
    const VdfNode* child(std::string_view name) const;
    /// String value of the named child, or "" if absent or an object.
    std::string value_of(std::string_view name) const;
};

/// Parse VDF text. Returns a synthetic root object whose children are the
/// top-level entries, or nullopt on malformed input.
std::optional<VdfNode> parse_vdf(std::string_view text);

/// Library folder paths listed in steamapps/libraryfolders.vdf (modern
/// nested format and the legacy "N" "path" format).
std::vector<std::filesystem::path> parse_library_folders_vdf(std::string_view text);

/// The "installdir" of an appmanifest_<id>.acf.
std::optional<std::string> parse_app_install_dir(std::string_view acf_text);

/// Candidate Steam installation roots for this user, most likely first,
/// with symlinked duplicates removed. Only existing directories are returned.
std::vector<std::filesystem::path> steam_roots(const EnvLookup& env);

/// Install directory of `app_id` across every library of every root, or
/// nullopt if it is not installed.
std::optional<std::filesystem::path> find_steam_app(
    std::uint32_t app_id, const std::vector<std::filesystem::path>& roots);

} // namespace osc::platform
