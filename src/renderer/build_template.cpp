#include "renderer/build_template.hpp"

#include "sim/build_placement.hpp"

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

std::vector<TemplateSite> template_sites(const BuildTemplate& t, f32 x0, f32 z0, f32 x1, f32 z1,
                                         bool drag, const FootprintOf& footprint) {
    std::vector<TemplateSite> sites;
    if (t.entries.empty()) return sites;
    const auto foot = [&](const std::string& bp) {
        const std::array<f32, 2> f = footprint ? footprint(bp) : std::array<f32, 2>{1, 1};
        return std::array<f32, 2>{std::max(f[0], 1.0f), std::max(f[1], 1.0f)};
    };
    // The lead's corner cell at each end (SFootprint::ToCellPos)
    const auto lead = foot(t.entries.front().blueprint_id);
    const auto cell = [](f32 p, f32 size) { return static_cast<f32>(std::lrint(p - size * 0.5f)); };
    const f32 sx = cell(x0, lead[0]);
    const f32 sz = cell(z0, lead[1]);
    const f32 dx = drag ? cell(x1, lead[0]) - sx : 0.0f;
    const f32 dz = drag ? cell(z1, lead[1]) - sz : 0.0f;
    const f32 longest = std::max(std::abs(dx), std::abs(dz));
    const f32 span = std::abs(dx) > std::abs(dz) ? t.span_x : t.span_z;
    // A span under a cell lays one copy (Moho seems to hold the spans as
    // integers, where it would be 0), so no drag asks for millions.
    const int copies =
        longest > 0 && span >= 1 ? static_cast<int>(std::floor(longest / span)) + 1 : 1;
    sites.reserve(static_cast<size_t>(copies) * t.entries.size());
    for (int k = 0; k < copies; ++k) {
        const f32 along = longest > 0 ? static_cast<f32>(k) * span / longest : 0.0f;
        const f32 ax = static_cast<f32>(std::lrint(sx + dx * along)) + 0.5f;
        const f32 az = static_cast<f32>(std::lrint(sz + dz * along)) + 0.5f;
        for (const BuildTemplateEntry& e : t.entries) {
            const auto f = foot(e.blueprint_id);
            f32 x = ax + e.x;
            f32 z = az + e.z;
            sim::snap_structure_center(x, z, f[0], f[1]);
            sites.push_back({e.blueprint_id, x, z});
        }
    }
    return sites;
}

} // namespace osc::renderer
