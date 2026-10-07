#include "renderer/build_template.hpp"

#include <algorithm>
#include <cmath>

namespace osc::renderer {

std::array<f32, 4> skirt_rect(const TemplateStructure& s) {
    // The footprint's corner, rounded to the grid; the skirt from there by
    // its offset, else the footprint itself
    const auto x_lower = static_cast<f32>(static_cast<i16>(std::lrint(s.x - s.foot_x * 0.5f)));
    const auto z_lower = static_cast<f32>(static_cast<i16>(std::lrint(s.z - s.foot_z * 0.5f)));
    std::array<f32, 4> r{};
    if (s.skirt_x == 0.0f) {
        r[0] = x_lower;
        r[2] = r[0] + s.foot_x;
    } else {
        r[0] = s.skirt_off_x + x_lower;
        r[2] = r[0] + s.skirt_x;
    }
    if (s.skirt_z == 0.0f) {
        r[1] = z_lower;
        r[3] = r[1] + s.foot_z;
    } else {
        r[1] = s.skirt_off_z + z_lower;
        r[3] = r[1] + s.skirt_z;
    }
    return r;
}

std::optional<BuildTemplate>
generate_build_template(const std::vector<TemplateStructure>& structures) {
    if (structures.empty()) return std::nullopt;
    f32 min_x = 10000.0f, min_z = 10000.0f;
    f32 max_x = -10000.0f, max_z = -10000.0f;
    BuildTemplate t;
    for (const TemplateStructure& s : structures) {
        t.entries.push_back({s.blueprint_id, static_cast<i32>(s.creation_tick), s.x, s.z});
        const auto r = skirt_rect(s);
        min_x = std::min(min_x, r[0]);
        min_z = std::min(min_z, r[1]);
        max_x = std::max(max_x, r[2]);
        max_z = std::max(max_z, r[3]);
    }
    std::stable_sort(t.entries.begin(), t.entries.end(),
                     [](const BuildTemplateEntry& a, const BuildTemplateEntry& b) {
                         return a.build_order < b.build_order;
                     });
    t.span_x = max_x - min_x;
    t.span_z = max_z - min_z;
    // Each where it stands from the first in the order
    const f32 ox = t.entries.front().x;
    const f32 oz = t.entries.front().z;
    for (BuildTemplateEntry& e : t.entries) {
        e.x -= ox;
        e.z -= oz;
    }
    return t;
}

} // namespace osc::renderer
