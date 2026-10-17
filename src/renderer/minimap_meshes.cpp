#include "renderer/minimap_meshes.hpp"

#include <algorithm>

namespace osc::renderer {

f32 lod_reach(const std::vector<std::optional<f32>>& cutoffs) {
    f32 reach = 0.0f;
    for (const std::optional<f32>& cutoff : cutoffs) {
        const f32 c = cutoff.value_or(kDefaultLodCutoff);
        if (c <= 0.0f) {
            return kUnlimitedReach;
        }
        reach = std::max(reach, c);
    }
    return reach;
}

} // namespace osc::renderer
