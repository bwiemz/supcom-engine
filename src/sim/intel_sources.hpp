#pragma once

// The intel an entity paints into the armies' grids (M215g), as Moho's
// CIntel keeps it: one handle per kind (CIntelPosHandle), painted where the
// entity was when it last repainted (docs/plans/2026-10-01-m215g-intel-
// grids-design.md).

#include "core/types.hpp"
#include "map/intel_grid.hpp"
#include "sim/entity.hpp"

#include <array>
#include <string_view>

namespace osc::sim {

/// The kinds of intel that paint, by their blueprint and script names.
enum class IntelSource : u8 {
    Vision,
    WaterVision,
    Radar,
    Sonar,
    Omni,
    RadarStealthField,
    SonarStealthField,
    CloakField,
    Count,
};
constexpr size_t kIntelSources = static_cast<size_t>(IntelSource::Count);

/// Its name in EnableIntel, InitIntel and the like.
constexpr const char* intel_source_name(IntelSource s) {
    constexpr const char* kNames[] = {"Vision",
                                      "WaterVision",
                                      "Radar",
                                      "Sonar",
                                      "Omni",
                                      "RadarStealthField",
                                      "SonarStealthField",
                                      "CloakField"};
    return kNames[static_cast<size_t>(s)];
}

/// The source named `name`, or -1 for intel that doesn't paint (Cloak,
/// Jammer, the stealth toggles).
constexpr int intel_source_index(std::string_view name) {
    for (size_t s = 0; s < kIntelSources; ++s)
        if (name == intel_source_name(static_cast<IntelSource>(s))) return static_cast<int>(s);
    return -1;
}

/// The grid it paints.
constexpr map::IntelLayer intel_source_layer(IntelSource s) {
    constexpr map::IntelLayer kLayers[] = {
        map::IntelLayer::Vision,       map::IntelLayer::Water,        map::IntelLayer::Radar,
        map::IntelLayer::Sonar,        map::IntelLayer::Omni,         map::IntelLayer::RadarCounter,
        map::IntelLayer::SonarCounter, map::IntelLayer::VisionCounter};
    return kLayers[static_cast<size_t>(s)];
}

/// A field paints into every other army's counter grid (Moho's
/// CIntelCounterHandle skips only its own army's); the rest into their
/// army's own grid.
constexpr bool intel_source_is_field(IntelSource s) {
    return s == IntelSource::RadarStealthField || s == IntelSource::SonarStealthField ||
           s == IntelSource::CloakField;
}

/// One kind of an entity's intel as painted (CIntelPosHandle). While off it
/// follows its entity unpainted; on, it is painted at `pos` and repainted
/// there when its entity moves a third of its radius, or after 30 ticks of
/// moving, or when it stops.
struct IntelHandle {
    u32 radius = 0; ///< whole units (SetIntelRadius truncates)
    bool enabled = false;
    Vector3 pos;
    u32 last_tick = 0; ///< when it last repainted for moving
};

/// An entity's handles, and where it stood on the last pass.
struct PaintedIntel {
    i32 army = -1; ///< whose grids
    std::array<IntelHandle, kIntelSources> handles{};
    Vector3 last_pos;
    bool moved = false; ///< it moved on the last pass
    u32 pass = 0;       ///< the last pass that found it
};

} // namespace osc::sim
