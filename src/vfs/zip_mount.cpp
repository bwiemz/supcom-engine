#include "vfs/zip_mount.hpp"

#include <algorithm>
#include <spdlog/spdlog.h>
#include <minizip/unzip.h>

namespace osc::vfs {

std::string ZipMount::normalize_key(std::string_view path) {
    std::string result(path);
    // Forward slashes
    std::replace(result.begin(), result.end(), '\\', '/');
    // Lowercase
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    // Strip leading slash
    if (!result.empty() && result[0] == '/') {
        result.erase(0, 1);
    }
    return result;
}

ZipMount::ZipMount(const std::filesystem::path& archive_path)
    : archive_path_(archive_path) {
    zip_handle_ = unzOpen(archive_path.string().c_str());
    if (!zip_handle_) {
        spdlog::error("Failed to open ZIP archive: {}", archive_path.string());
        return;
    }

    // Read central directory
    int ret = unzGoToFirstFile(static_cast<unzFile>(zip_handle_));
    while (ret == UNZ_OK) {
        unz_file_info file_info;
        char filename[512];
        unzGetCurrentFileInfo(static_cast<unzFile>(zip_handle_), &file_info,
                              filename, sizeof(filename), nullptr, 0, nullptr, 0);

        std::string name(filename);
        // Skip directories (entries ending with /)
        if (!name.empty() && name.back() != '/') {
            ZipEntryInfo entry;
            entry.original_name = name;
            entry.uncompressed_size = file_info.uncompressed_size;
            entries_[normalize_key(name)] = std::move(entry);
        }

        ret = unzGoToNextFile(static_cast<unzFile>(zip_handle_));
    }

    spdlog::debug("ZIP mount {}: {} files indexed",
                  archive_path.filename().string(), entries_.size());
}

ZipMount::~ZipMount() {
    if (zip_handle_) {
        unzClose(static_cast<unzFile>(zip_handle_));
    }
}

std::optional<std::vector<char>> ZipMount::read_file(
    std::string_view relative_path) const {
    if (!zip_handle_) return std::nullopt;

    auto key = normalize_key(relative_path);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return std::nullopt;
    }

    // Locate file in the ZIP by its original name
    if (unzLocateFile(static_cast<unzFile>(zip_handle_),
                      it->second.original_name.c_str(), 2) != UNZ_OK) {
        return std::nullopt;
    }

    if (unzOpenCurrentFile(static_cast<unzFile>(zip_handle_)) != UNZ_OK) {
        return std::nullopt;
    }

    std::vector<char> buffer(it->second.uncompressed_size);
    int bytes_read = unzReadCurrentFile(static_cast<unzFile>(zip_handle_),
                                         buffer.data(),
                                         static_cast<unsigned>(buffer.size()));
    unzCloseCurrentFile(static_cast<unzFile>(zip_handle_));

    if (bytes_read < 0 ||
        static_cast<u64>(bytes_read) != it->second.uncompressed_size) {
        return std::nullopt;
    }

    return buffer;
}

bool ZipMount::file_exists(std::string_view relative_path) const {
    return entries_.contains(normalize_key(relative_path));
}

std::vector<std::string> ZipMount::find_files(
    std::string_view directory, std::string_view pattern) const {
    std::vector<std::string> results;

    std::string dir_key = normalize_key(directory);
    if (!dir_key.empty() && dir_key.back() != '/') {
        dir_key += '/';
    }

    // Convert pattern to suffix match
    std::string pat(pattern);
    std::transform(pat.begin(), pat.end(), pat.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::string suffix;
    if (!pat.empty() && pat[0] == '*') {
        suffix = pat.substr(1);
    }

    for (const auto& [key, entry] : entries_) {
        // Check directory prefix
        if (!dir_key.empty() && key.compare(0, dir_key.size(), dir_key) != 0) {
            continue;
        }

        // Check pattern suffix
        bool match = false;
        if (suffix.empty()) {
            match = true;
        } else if (key.size() >= suffix.size()) {
            match = key.compare(key.size() - suffix.size(),
                                suffix.size(), suffix) == 0;
        }

        if (match) {
            results.push_back("/" + key);
        }
    }

    return results;
}

std::optional<FileInfo> ZipMount::get_file_info(
    std::string_view relative_path) const {
    auto key = normalize_key(relative_path);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return std::nullopt;
    }

    FileInfo info;
    info.size_bytes = it->second.uncompressed_size;
    info.is_folder = false;
    return info;
}

} // namespace osc::vfs
