#include "blueprints/footprint.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace osc::blueprints {

u8 motion_type_caps(std::string_view motion) {
    if (motion == "RULEUMT_Land" || motion == "RULEUMT_Biped") return occupancy::kLand;
    if (motion == "RULEUMT_Air") return occupancy::kAir;
    if (motion == "RULEUMT_Water") return occupancy::kWater;
    if (motion == "RULEUMT_SurfacingSub") return occupancy::kSub | occupancy::kWater;
    if (motion == "RULEUMT_Amphibious") return occupancy::kLand | occupancy::kSeabed;
    if (motion == "RULEUMT_Hover" || motion == "RULEUMT_AmphibiousFloating")
        return occupancy::kLand | occupancy::kWater;
    return 0; // None, Special, or none given
}

const NamedFootprint* find_footprint(const std::vector<NamedFootprint>& classes,
                                     const Footprint& fp) {
    const NamedFootprint* best = nullptr;
    int best_distance = std::numeric_limits<i16>::max();
    for (const NamedFootprint& c : classes) {
        if (c.caps != fp.caps) continue;
        const int distance = std::max(std::abs(int{c.size_x} - int{fp.size_x}),
                                      std::abs(int{c.size_z} - int{fp.size_z}));
        if (distance < best_distance) {
            best_distance = distance;
            best = &c;
        }
    }
    return best;
}

namespace {

bool is_none(std::string_view motion) {
    return motion.empty() || motion == "RULEUMT_None";
}

} // namespace

UnitFootprints resolve_unit_footprints(const std::vector<NamedFootprint>& classes,
                                       const Footprint& own, std::string_view motion,
                                       std::string_view alt_motion, u8 build_on_layer_caps) {
    UnitFootprints out;
    out.main = own;
    out.alt = own;
    if (is_none(motion)) {
        // A structure: where it may be built.
        out.main.caps = build_on_layer_caps;
        if ((build_on_layer_caps & occupancy::kSeabed) != 0 && out.main.max_water_depth == 0.0f)
            out.main.max_water_depth = std::numeric_limits<f32>::max();
        out.alt = out.main;
        return out;
    }
    out.main.caps = motion_type_caps(motion);
    out.main.flags = 0; // every motion type's flags are Moho's default
    if ((out.main.caps & occupancy::kGround) != 0) {
        if (const NamedFootprint* c = find_footprint(classes, out.main)) {
            out.main = *c;
            out.main_class = c->index;
        }
    }
    if (is_none(alt_motion)) {
        out.alt = out.main;
        out.alt_class = out.main_class;
        return out;
    }
    out.alt.caps = motion_type_caps(alt_motion);
    out.alt.flags = 0;
    // Moho resolves the alt only when the main footprint is a ground one.
    if ((out.main.caps & occupancy::kGround) != 0) {
        if (const NamedFootprint* c = find_footprint(classes, out.alt)) {
            out.alt = *c;
            out.alt_class = c->index;
        } else {
            out.alt = out.main;
            out.alt_class = out.main_class;
        }
    }
    return out;
}

} // namespace osc::blueprints
