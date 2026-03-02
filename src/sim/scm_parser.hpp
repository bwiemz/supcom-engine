#pragma once

#include "sim/bone_data.hpp"

#include <optional>
#include <vector>

namespace osc::sim {

/// Parse bone data from an SCM (Supreme Commander Model) v5 file.
/// Only reads header + bone names + bone entries; skips vertices/indices.
/// Returns nullopt on parse failure.
std::optional<BoneData> parse_scm_bones(const std::vector<char>& file_data);

} // namespace osc::sim
