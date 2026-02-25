#include "vfs/directory_mount.hpp"

#include <algorithm>
#include <fstream>
#include <spdlog/spdlog.h>

namespace osc::vfs {

DirectoryMount::DirectoryMount(std::filesystem::path root)
    : root_(std::move(root)) {}

std::filesystem::path DirectoryMount::resolve(
    std::string_view relative_path) const {
    // Strip leading slash if present
    if (!relative_path.empty() && relative_path[0] == '/') {
        relative_path.remove_prefix(1);
    }
    return root_ / std::filesystem::path(relative_path);
}

std::optional<std::vector<char>> DirectoryMount::read_file(
    std::string_view relative_path) const {
    auto full_path = resolve(relative_path);

    std::ifstream file(full_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return std::nullopt;
    }

    auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> buffer(static_cast<size_t>(size));
    if (!file.read(buffer.data(), size)) {
        return std::nullopt;
    }

    return buffer;
}

bool DirectoryMount::file_exists(std::string_view relative_path) const {
    auto full_path = resolve(relative_path);
    return std::filesystem::exists(full_path);
}

std::vector<std::string> DirectoryMount::find_files(
    std::string_view directory, std::string_view pattern) const {
    auto dir_path = resolve(directory);
    std::vector<std::string> results;

    if (!std::filesystem::exists(dir_path) ||
        !std::filesystem::is_directory(dir_path)) {
        return results;
    }

    // Convert pattern to a simple suffix match (handles "*.bp", "*_unit.bp")
    std::string pat(pattern);
    std::transform(pat.begin(), pat.end(), pat.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Extract the suffix after '*'
    std::string suffix;
    if (!pat.empty() && pat[0] == '*') {
        suffix = pat.substr(1);
    }

    std::error_code ec;
    for (auto& entry :
         std::filesystem::recursive_directory_iterator(dir_path, ec)) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();
            std::string filename_lower = filename;
            std::transform(filename_lower.begin(), filename_lower.end(),
                           filename_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            bool match = false;
            if (suffix.empty()) {
                match = true; // "*" matches everything
            } else if (filename_lower.size() >= suffix.size()) {
                match = filename_lower.compare(filename_lower.size() - suffix.size(),
                                                suffix.size(), suffix) == 0;
            }

            if (match) {
                // Build the virtual path relative to the mount root
                auto rel = std::filesystem::relative(entry.path(), root_, ec);
                if (!ec) {
                    std::string virtual_path = "/" + rel.generic_string();
                    std::transform(virtual_path.begin(), virtual_path.end(),
                                   virtual_path.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    results.push_back(std::move(virtual_path));
                }
            }
        }
    }

    return results;
}

std::optional<FileInfo> DirectoryMount::get_file_info(
    std::string_view relative_path) const {
    auto full_path = resolve(relative_path);
    std::error_code ec;

    if (!std::filesystem::exists(full_path, ec)) {
        return std::nullopt;
    }

    FileInfo info;
    info.is_folder = std::filesystem::is_directory(full_path, ec);
    if (!info.is_folder) {
        info.size_bytes = std::filesystem::file_size(full_path, ec);
    }
    return info;
}

} // namespace osc::vfs
