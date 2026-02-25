#pragma once

#include "vfs/mount_point.hpp"

#include <filesystem>
#include <unordered_map>

namespace osc::vfs {

/// Mount point backed by a ZIP archive (.scd, .nx2, .zip).
class ZipMount : public MountPoint {
public:
    /// Opens the ZIP file and reads its central directory.
    explicit ZipMount(const std::filesystem::path& archive_path);
    ~ZipMount() override;

    // Non-copyable
    ZipMount(const ZipMount&) = delete;
    ZipMount& operator=(const ZipMount&) = delete;

    std::optional<std::vector<char>> read_file(
        std::string_view relative_path) const override;
    bool file_exists(std::string_view relative_path) const override;
    std::vector<std::string> find_files(
        std::string_view directory,
        std::string_view pattern) const override;
    std::optional<FileInfo> get_file_info(
        std::string_view relative_path) const override;

private:
    struct ZipEntryInfo {
        std::string original_name; // as stored in the ZIP
        u64 uncompressed_size = 0;
    };

    std::filesystem::path archive_path_;
    void* zip_handle_ = nullptr; // unzFile from minizip

    /// Index of all files in the archive, keyed by lowercase normalized path.
    std::unordered_map<std::string, ZipEntryInfo> entries_;

    /// Normalize a path for lookup (lowercase, forward slashes, strip leading /).
    static std::string normalize_key(std::string_view path);
};

} // namespace osc::vfs
