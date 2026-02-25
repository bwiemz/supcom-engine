#pragma once

#include "vfs/mount_point.hpp"
#include <filesystem>

namespace osc::vfs {

/// Mount point backed by a real filesystem directory.
class DirectoryMount : public MountPoint {
public:
    explicit DirectoryMount(std::filesystem::path root);

    std::optional<std::vector<char>> read_file(
        std::string_view relative_path) const override;
    bool file_exists(std::string_view relative_path) const override;
    std::vector<std::string> find_files(
        std::string_view directory,
        std::string_view pattern) const override;
    std::optional<FileInfo> get_file_info(
        std::string_view relative_path) const override;

private:
    std::filesystem::path root_;

    /// Resolve a virtual relative path to an actual filesystem path,
    /// handling case-insensitive matching on Windows.
    std::filesystem::path resolve(std::string_view relative_path) const;
};

} // namespace osc::vfs
