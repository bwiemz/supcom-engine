#pragma once

#include "core/types.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace osc::vfs {

struct FileInfo {
    u64 size_bytes = 0;
    bool is_folder = false;
};

/// Abstract base class for a mount source (directory or archive).
class MountPoint {
public:
    virtual ~MountPoint() = default;

    /// Read a file's entire contents. Returns nullopt if not found.
    virtual std::optional<std::vector<char>> read_file(
        std::string_view relative_path) const = 0;

    /// Check if a file exists at the given path (relative to mount root).
    virtual bool file_exists(std::string_view relative_path) const = 0;

    /// Find all files matching a glob pattern under a directory.
    /// Pattern supports '*' wildcard (e.g., "*.bp", "*_unit.bp").
    virtual std::vector<std::string> find_files(
        std::string_view directory,
        std::string_view pattern) const = 0;

    /// Get file info or nullopt if not found.
    virtual std::optional<FileInfo> get_file_info(
        std::string_view relative_path) const = 0;
};

} // namespace osc::vfs
