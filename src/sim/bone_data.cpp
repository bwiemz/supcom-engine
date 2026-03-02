#include "sim/bone_data.hpp"

#include <algorithm>
#include <cctype>

namespace osc::sim {

i32 BoneData::find_bone(const std::string& name) const {
    // Case-insensitive lookup: lowercase the input
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto it = name_to_index.find(lower);
    return (it != name_to_index.end()) ? it->second : -1;
}

} // namespace osc::sim
