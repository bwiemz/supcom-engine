#pragma once

#include <filesystem>
#include <random>
#include <string>

namespace osc::test {

inline std::filesystem::path unique_temp_path(const std::string& stem,
                                              const std::string& extension = {}) {
    std::random_device rd;
    return std::filesystem::temp_directory_path() /
           (stem + "_" + std::to_string(rd()) + std::to_string(rd()) + extension);
}

} // namespace osc::test
