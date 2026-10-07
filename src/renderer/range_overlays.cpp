#include "renderer/range_overlays.hpp"

#include "sim/blueprint_categories.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

extern "C" {
#include <lua.h>
}

namespace osc::renderer {

namespace {

/// table[key] at `index` as a number (0 if it isn't one)
f32 number_at(lua_State* L, int index, const char* key) {
    lua_pushstring(L, key);
    lua_rawget(L, index);
    const f32 v = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : 0.0f;
    lua_pop(L, 1);
    return v;
}

/// Push table[key] at `index` if it is a table (true), else nothing
bool push_table(lua_State* L, int index, const char* key) {
    lua_pushstring(L, key);
    lua_rawget(L, index);
    if (lua_istable(L, -1)) return true;
    lua_pop(L, 1);
    return false;
}

/// SMinMax's Max at table[key] (a {Min, Max} table)
f32 max_of(lua_State* L, int index, const char* key) {
    if (!push_table(L, index, key)) return 0;
    const f32 v = number_at(L, lua_gettop(L), "Max");
    lua_pop(L, 1);
    return v;
}

/// RULEUTC_* names' bits, as the script bits number them
u32 toggle_cap_bit(std::string_view name) {
    static constexpr std::array<std::string_view, 9> kToggles = {
        "RULEUTC_ShieldToggle",  "RULEUTC_WeaponToggle",     "RULEUTC_JammingToggle",
        "RULEUTC_IntelToggle",   "RULEUTC_ProductionToggle", "RULEUTC_StealthToggle",
        "RULEUTC_GenericToggle", "RULEUTC_SpecialToggle",    "RULEUTC_CloakToggle"};
    for (u32 i = 0; i < kToggles.size(); ++i)
        if (kToggles[i] == name) return 1u << i;
    return 0;
}

/// Moho's ResolvePositiveRadius
f32 positive_or(f32 preferred, f32 fallback) {
    return preferred > 0.0f ? preferred : fallback;
}

std::optional<RangeRing> ring_at(f32 x, f32 z, f32 outer, f32 inner = 0.0f) {
    if (outer <= 0.0f) return std::nullopt;
    return RangeRing{x, z, inner, outer};
}

/// The weapon category an extractor reads (Countermeasure for Defense's
/// fallback); nothing for every weapon (Moho's 6).
std::optional<WeaponRangeCategory> weapon_category(RangeExtractor kind) {
    switch (kind) {
    case RangeExtractor::DirectFire: return WeaponRangeCategory::DirectFire;
    case RangeExtractor::IndirectFire: return WeaponRangeCategory::IndirectFire;
    case RangeExtractor::AntiAir: return WeaponRangeCategory::AntiAir;
    case RangeExtractor::AntiNavy: return WeaponRangeCategory::AntiNavy;
    case RangeExtractor::Defense: return WeaponRangeCategory::Countermeasure;
    default: return std::nullopt;
    }
}

/// UserUnit::FindWeaponBy: over the blueprint's weapons of the category (or
/// all), the largest reach now and the smallest minimum.
std::optional<RangeRing> live_weapon_ring(const RangeUnit& unit,
                                          std::optional<WeaponRangeCategory> category) {
    constexpr f32 kNoMin = std::numeric_limits<f32>::max();
    constexpr f32 kNoMax = std::numeric_limits<f32>::lowest();
    f32 min_radius = kNoMin;
    f32 max_radius = kNoMax;
    const auto& weapons = unit.blueprint->weapons;
    for (u32 i = 0; i < weapons.size(); ++i) {
        if (category && weapons[i].category != *category) continue;
        const auto now =
            std::find_if(unit.weapons.begin(), unit.weapons.end(),
                         [i](const sim::WeaponRangeRecord& w) { return w.index == i; });
        if (now == unit.weapons.end()) continue;
        max_radius = std::max(max_radius, now->max_radius);
        if (now->min_radius <= min_radius) min_radius = now->min_radius;
    }
    if (max_radius <= kNoMax) max_radius = 0;
    if (min_radius >= kNoMin) min_radius = 0;
    return ring_at(unit.x, unit.z, max_radius, min_radius);
}

/// WeaponExtractor::ResolveWeaponCategoryRange: a blueprint's, by its
/// EffectiveRadius where it has one.
std::optional<RangeRing> blueprint_weapon_ring(const RangeBlueprint& bp,
                                               WeaponRangeCategory category, f32 x, f32 z) {
    constexpr f32 kNoMin = std::numeric_limits<f32>::max();
    f32 inner = kNoMin;
    f32 outer = 0;
    for (const auto& w : bp.weapons) {
        if (w.category != category) continue;
        outer = std::max(outer, positive_or(w.effective_radius, w.max_radius));
        if (w.min_radius <= inner) inner = w.min_radius;
    }
    if (outer <= 0.0f) return std::nullopt;
    return RangeRing{x, z, inner >= kNoMin ? 0.0f : inner, outer};
}

void add_batch(std::vector<RangeBatch>& out, const RangeProfile& profile, const RangeColor& color,
               std::vector<RangeRing> rings) {
    if (rings.empty()) return;
    out.push_back({color, profile.inner, profile.outer, std::move(rings)});
}

/// The combined profiles draw only for the active filters (Moho's
/// "AllMilitary"/"AllIntel" test before every per-unit pass).
bool per_unit_profile(const RangeProfile& profile) {
    return profile.name != "AllMilitary" && profile.name != "AllIntel";
}

bool in_profile(const RangeProfile& profile, const RangeBlueprint& bp) {
    return profile.categories.matches(bp.categories);
}

} // namespace

RangeColor range_color(u32 argb) {
    constexpr f32 kByte = 0.0039209998f;
    return {static_cast<f32>((argb >> 16) & 0xFFu) * kByte,
            static_cast<f32>((argb >> 8) & 0xFFu) * kByte, static_cast<f32>(argb & 0xFFu) * kByte,
            static_cast<f32>((argb >> 24) & 0xFFu) * kByte};
}

std::optional<RangeExtractor> range_extractor(std::string_view name) {
    static constexpr std::array<std::pair<std::string_view, RangeExtractor>, 12> kNames = {{
        {"AllMilitary", RangeExtractor::AllMilitary},
        {"DirectFire", RangeExtractor::DirectFire},
        {"IndirectFire", RangeExtractor::IndirectFire},
        {"AntiAir", RangeExtractor::AntiAir},
        {"AntiNavy", RangeExtractor::AntiNavy},
        {"Defense", RangeExtractor::Defense},
        {"Miscellaneous", RangeExtractor::Miscellaneous},
        {"AllIntel", RangeExtractor::AllIntel},
        {"Radar", RangeExtractor::Radar},
        {"Sonar", RangeExtractor::Sonar},
        {"Omni", RangeExtractor::Omni},
        {"CounterIntel", RangeExtractor::CounterIntel},
    }};
    for (const auto& [n, kind] : kNames)
        if (n == name) return kind;
    return std::nullopt;
}

WeaponRangeCategory parse_weapon_range_category(std::string_view text) {
    if (text == "UWRC_DirectFire") return WeaponRangeCategory::DirectFire;
    if (text == "UWRC_IndirectFire") return WeaponRangeCategory::IndirectFire;
    if (text == "UWRC_AntiAir") return WeaponRangeCategory::AntiAir;
    if (text == "UWRC_AntiNavy") return WeaponRangeCategory::AntiNavy;
    if (text == "UWRC_Countermeasure") return WeaponRangeCategory::Countermeasure;
    return WeaponRangeCategory::Undefined;
}

RangeBlueprint read_range_blueprint(lua_State* L, int index) {
    RangeBlueprint bp;
    if (index < 0) index = lua_gettop(L) + index + 1;
    if (!lua_istable(L, index)) return bp;
    sim::collect_blueprint_categories(L, index, bp.categories);

    if (push_table(L, index, "Weapon")) {
        const int weapons = lua_gettop(L);
        for (int i = 1;; ++i) {
            lua_rawgeti(L, weapons, i);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                break;
            }
            RangeBlueprint::Weapon w;
            if (lua_istable(L, -1)) {
                const int t = lua_gettop(L);
                lua_pushstring(L, "RangeCategory");
                lua_rawget(L, t);
                if (lua_type(L, -1) == LUA_TSTRING)
                    w.category = parse_weapon_range_category(lua_tostring(L, -1));
                lua_pop(L, 1);
                w.min_radius = number_at(L, t, "MinRadius");
                w.max_radius = number_at(L, t, "MaxRadius");
                w.effective_radius = number_at(L, t, "EffectiveRadius");
            }
            // Indexed as the unit's weapons are, by place in the list
            bp.weapons.push_back(w);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    if (push_table(L, index, "Intel")) {
        const int t = lua_gettop(L);
        bp.radar = number_at(L, t, "RadarRadius");
        bp.sonar = number_at(L, t, "SonarRadius");
        bp.omni = number_at(L, t, "OmniRadius");
        bp.radar_stealth_field = number_at(L, t, "RadarStealthFieldRadius");
        bp.sonar_stealth_field = number_at(L, t, "SonarStealthFieldRadius");
        bp.cloak_field = number_at(L, t, "CloakFieldRadius");
        bp.jam_max = max_of(L, t, "JamRadius");
        bp.spoof_max = max_of(L, t, "SpoofRadius");
        lua_pop(L, 1);
    }
    if (push_table(L, index, "Defense")) {
        if (push_table(L, lua_gettop(L), "Shield")) {
            bp.shield_size = number_at(L, lua_gettop(L), "ShieldSize");
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    if (push_table(L, index, "AI")) {
        const int t = lua_gettop(L);
        bp.staging_scan = number_at(L, t, "StagingPlatformScanRadius");
        bp.guard_scan = number_at(L, t, "GuardScanRadius");
        lua_pop(L, 1);
    }
    if (push_table(L, index, "General")) {
        if (push_table(L, lua_gettop(L), "ToggleCaps")) {
            const int caps = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, caps) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING && lua_toboolean(L, -1))
                    bp.toggle_caps |= toggle_cap_bit(lua_tostring(L, -2));
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    return bp;
}

const RangeBlueprint* RangeBlueprints::find(const std::string& id, lua_State* L) {
    if (const auto it = cache_.find(id); it != cache_.end())
        return it->second ? &*it->second : nullptr;
    std::optional<RangeBlueprint>& out = cache_[id];
    if (!L) return nullptr;
    std::string key = id;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const int top = lua_gettop(L);
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, key.c_str());
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) out = read_range_blueprint(L, lua_gettop(L));
    }
    lua_settop(L, top);
    return out ? &*out : nullptr;
}

RangeUnit range_unit(const sim::WorldSnapshot& snap, const sim::EntityRecord& e,
                     const RangeBlueprint& bp, f32 x, f32 z) {
    RangeUnit u;
    u.blueprint = &bp;
    u.x = x;
    u.z = z;
    u.weapons = snap.weapon_ranges_of(e);
    u.script_bits = e.script_bits;
    for (const auto& intel : snap.intel_of(e)) {
        if (intel.type == "Radar") u.radar = intel.radius;
        else if (intel.type == "Sonar") u.sonar = intel.radius;
        else if (intel.type == "Omni") u.omni = intel.radius;
        else if (intel.type == "RadarStealthField" || intel.type == "SonarStealthField" ||
                 intel.type == "CloakField" || intel.type == "Spoof")
            u.counter_intel = std::max(u.counter_intel, intel.radius);
    }
    return u;
}

std::optional<RangeRing> extract_range(RangeExtractor kind, const RangeUnit& unit) {
    if (!unit.blueprint) return std::nullopt;
    const RangeBlueprint& bp = *unit.blueprint;
    const auto toggled_off = [&](u32 toggle) {
        return (bp.toggle_caps & toggle) != 0 && (unit.script_bits & toggle) != 0;
    };
    switch (kind) {
    case RangeExtractor::AllMilitary:
        // An OVERLAYMISC unit's assist reach, else every weapon's
        if (bp.categories.count("OVERLAYMISC") != 0) {
            const f32 radius = positive_or(bp.staging_scan, bp.guard_scan);
            if (radius > 0.0f) return ring_at(unit.x, unit.z, radius);
        }
        return live_weapon_ring(unit, std::nullopt);
    case RangeExtractor::DirectFire:
    case RangeExtractor::IndirectFire:
    case RangeExtractor::AntiAir:
    case RangeExtractor::AntiNavy: return live_weapon_ring(unit, weapon_category(kind));
    case RangeExtractor::Defense:
        if (bp.shield_size * 0.5f > 0.0f) return ring_at(unit.x, unit.z, bp.shield_size * 0.5f);
        return live_weapon_ring(unit, WeaponRangeCategory::Countermeasure);
    case RangeExtractor::Miscellaneous:
        return ring_at(unit.x, unit.z, positive_or(bp.staging_scan, bp.guard_scan));
    case RangeExtractor::AllIntel:
    case RangeExtractor::Radar:
    case RangeExtractor::Sonar:
    case RangeExtractor::Omni: {
        if (toggled_off(kIntelToggle)) return std::nullopt;
        if (unit.radar <= 0.0f && unit.sonar <= 0.0f && unit.omni <= 0.0f) return std::nullopt;
        const f32 radius = kind == RangeExtractor::Radar   ? unit.radar
                           : kind == RangeExtractor::Sonar ? unit.sonar
                           : kind == RangeExtractor::Omni
                               ? unit.omni
                               : std::max({unit.omni, unit.radar, unit.sonar});
        return ring_at(unit.x, unit.z, radius);
    }
    case RangeExtractor::CounterIntel:
        if (toggled_off(kJammingToggle) || toggled_off(kStealthToggle)) return std::nullopt;
        return ring_at(unit.x, unit.z, std::max({unit.counter_intel, bp.jam_max, bp.spoof_max}));
    }
    return std::nullopt;
}

std::optional<RangeRing> blueprint_range(RangeExtractor kind, const RangeBlueprint& bp, f32 x,
                                         f32 z) {
    switch (kind) {
    case RangeExtractor::AllMilitary: return std::nullopt; // Extract alone
    case RangeExtractor::DirectFire:
    case RangeExtractor::IndirectFire:
    case RangeExtractor::AntiAir:
    case RangeExtractor::AntiNavy: return blueprint_weapon_ring(bp, *weapon_category(kind), x, z);
    case RangeExtractor::Defense:
        if (bp.shield_size * 0.5f > 0.0f) return ring_at(x, z, bp.shield_size * 0.5f);
        return blueprint_weapon_ring(bp, WeaponRangeCategory::Countermeasure, x, z);
    case RangeExtractor::Miscellaneous:
        return ring_at(x, z, positive_or(bp.staging_scan, bp.guard_scan));
    case RangeExtractor::AllIntel:
        return ring_at(x, z, std::max({bp.radar, bp.sonar, bp.omni, bp.radar_stealth_field}));
    case RangeExtractor::Radar: return ring_at(x, z, bp.radar);
    case RangeExtractor::Sonar: return ring_at(x, z, bp.sonar);
    case RangeExtractor::Omni: return ring_at(x, z, bp.omni);
    case RangeExtractor::CounterIntel:
        return ring_at(x, z,
                       std::max({bp.jam_max, bp.spoof_max, bp.radar_stealth_field,
                                 bp.sonar_stealth_field, bp.cloak_field}));
    }
    return std::nullopt;
}

void RangeOverlays::set_profile(RangeProfile profile) {
    std::string name = profile.name;
    profiles_[std::move(name)] = std::move(profile);
}

std::vector<const RangeProfile*> RangeOverlays::visible() const {
    std::vector<const RangeProfile*> out;
    for (const auto& name : filters_) {
        const auto it = profiles_.find(name);
        if (it != profiles_.end()) out.push_back(&it->second);
    }
    return out;
}

std::vector<RangeBatch> range_batches(const RangeOverlays& overlays, const RangeScene& scene) {
    std::vector<RangeBatch> out;
    const auto& settings = overlays.settings();
    if (!settings.enabled) return out;

    // 1. A structure being placed, at the cursor (func_RenderBuildRings)
    if (settings.render_build && scene.placing) {
        for (const auto& [name, profile] : overlays.profiles()) {
            const auto kind = range_extractor(name);
            if (!kind || !per_unit_profile(profile) || !in_profile(profile, *scene.placing))
                continue;
            if (auto ring = blueprint_range(*kind, *scene.placing, scene.cursor_x, scene.cursor_z))
                add_batch(out, profile, profile.normal, {*ring});
        }
    }
    // 2. The active filters, over the units in view (func_ExtractRanges)
    for (const RangeProfile* profile : overlays.visible()) {
        const auto kind = range_extractor(profile->name);
        if (!kind) continue;
        std::vector<RangeRing> rings;
        for (const auto& unit : scene.in_view)
            if (in_profile(*profile, *unit.blueprint))
                if (auto ring = extract_range(*kind, unit)) rings.push_back(*ring);
        add_batch(out, *profile, profile->normal, std::move(rings));
    }
    // 3. The selection, each profile (RenderSelectedUnitsRange)
    if (settings.render_selected) {
        for (const auto& [name, profile] : overlays.profiles()) {
            const auto kind = range_extractor(name);
            if (!kind || !per_unit_profile(profile)) continue;
            std::vector<RangeRing> rings;
            for (const auto& unit : scene.selected)
                if (in_profile(profile, *unit.blueprint))
                    if (auto ring = extract_range(*kind, unit)) rings.push_back(*ring);
            add_batch(out, profile, profile.selected, std::move(rings));
        }
    }
    // 4. The unit under the cursor (RenderHighlightedUnitRange)
    if (settings.render_highlighted && scene.hovered) {
        for (const auto& [name, profile] : overlays.profiles()) {
            const auto kind = range_extractor(name);
            if (!kind || !per_unit_profile(profile) ||
                !in_profile(profile, *scene.hovered->blueprint))
                continue;
            if (auto ring = extract_range(*kind, *scene.hovered))
                add_batch(out, profile, profile.rollover, {*ring});
        }
    }
    // 5. The no-rush zone, a thin grey ring (ExtractFocusArmyNoRushRange)
    if (scene.no_rush) {
        out.push_back({{0.2f, 0.2f, 0.2f, 0.2f}, {0.1f, 1.0f}, {1.0f, 2.0f}, {*scene.no_rush}});
    }
    return out;
}

f32 ring_thickness(RingThickness t, f32 coeff, f32 span, f32 zoom_ratio) {
    return (t.far * coeff * span - t.near) * zoom_ratio + t.near;
}

void ring_bands(std::span<const RangeRing> rings, f32 inner_thickness, f32 outer_thickness,
                std::vector<RangeRing>& fills, std::vector<RangeRing>& edges) {
    for (const RangeRing& r : rings) {
        RangeRing fill = r;
        fill.inner = r.inner <= 0.0f ? 0.0f : r.inner + inner_thickness;
        fill.outer = r.outer - outer_thickness;
        fills.push_back(fill);

        RangeRing inner_edge = r;
        inner_edge.outer = r.inner + inner_thickness;
        edges.push_back(inner_edge);

        RangeRing outer_edge = r;
        outer_edge.inner = r.outer - outer_thickness;
        edges.push_back(outer_edge);
    }
}

std::vector<RingVertex> ring_volume_vertices(f32 bottom, f32 top) {
    // Four circles: inner and outer at the bottom, then at the top. Moho
    // writes them at the map's height range (FAF's Sim::Create_exxt: 5 below
    // the lowest point and its highest), the lower first; the strips'
    // windings close the volume that way round.
    constexpr f32 kStep = 6.2831853f / static_cast<f32>(kRingSides);
    std::vector<RingVertex> v(kRingSides * 4);
    for (u32 i = 0; i < kRingSides; ++i) {
        const f32 x = std::cos(static_cast<f32>(i) * kStep);
        const f32 z = std::sin(static_cast<f32>(i) * kStep);
        v[i] = {x, bottom, z, 1, 0};
        v[i + kRingSides] = {x, bottom, z, 0, 1};
        v[i + kRingSides * 2] = {x, top, z, 1, 0};
        v[i + kRingSides * 3] = {x, top, z, 0, 1};
    }
    return v;
}

std::vector<u16> ring_volume_indices() {
    std::vector<u16> out;
    out.reserve(kRingSides * 24);
    // RangeRenderer's AppendRingStripIndices: vertex i and the next round
    // the circle, joined to those `offset` on
    const auto strip = [&out](u16 start, u16 end, u16 offset, bool primary) {
        for (u16 i = start; i < end; ++i) {
            const u16 next = static_cast<u16>(i + 1 != end ? i + 1 : start);
            const u16 across = static_cast<u16>(i + offset);
            const u16 next_across = static_cast<u16>(next + offset);
            if (primary) out.insert(out.end(), {i, across, next, next_across, next, across});
            else out.insert(out.end(), {next, across, i, across, next, next_across});
        }
    };
    constexpr u16 n = kRingSides;
    strip(0, n, n, true);         // the bottom
    strip(2 * n, 3 * n, n, true); // the top
    strip(0, n, 2 * n, false);    // the inner wall
    strip(n, 2 * n, 2 * n, true); // the outer wall
    return out;
}

} // namespace osc::renderer
