#pragma once

#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace osc::test {

inline std::filesystem::path unique_temp_path(const std::string& stem,
                                              const std::string& extension = {}) {
    std::random_device rd;
    return std::filesystem::temp_directory_path() /
           (stem + "_" + std::to_string(rd()) + std::to_string(rd()) + extension);
}

class TempDir {
public:
    explicit TempDir(const std::string& stem) : path_(unique_temp_path(stem)) {
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace osc::test
