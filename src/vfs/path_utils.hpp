#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace osc::vfs {

/// Case-insensitive glob match supporting '*' (any run) and '?' (one char).
/// FA's data paths were authored on Windows, so every match is ASCII
/// case-insensitive regardless of host filesystem.
bool wildcard_match(std::string_view pattern, std::string_view name);

/// True if `s` contains a glob character ('*' or '?').
bool has_wildcard(std::string_view s);

/// Resolve `path` against the real filesystem, matching each component
/// case-insensitively when the exact spelling does not exist. Returns the
/// on-disk spelling, or nullopt if no match exists. '.' and '..' components
/// are applied lexically. On case-insensitive filesystems this is effectively
/// an existence check.
std::optional<std::filesystem::path> resolve_case_insensitive(
    const std::filesystem::path& path);

/// Expand a path whose final component is a glob (e.g. ".../gamedata/*.scd")
/// into the matching directory entries (files and directories), sorted by
/// lowercase filename so mount order is deterministic on every platform.
/// The directory part is resolved case-insensitively. A pattern without
/// wildcards yields the resolved path itself if it exists.
std::vector<std::filesystem::path> expand_glob(
    const std::filesystem::path& pattern);

} // namespace osc::vfs
