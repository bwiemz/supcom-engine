#pragma once

// FA's range overlays (Moho's RangeRenderer and its extractors): the
// profiles the UI registers, what each reads off a unit, and the ring
// batches a frame draws. No Vulkan here; range_renderer draws the batches.

#include "core/types.hpp"
#include "sim/category_expr.hpp"
#include "sim/world_snapshot.hpp"

#include <array>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

struct lua_State;

namespace osc::renderer {

/// One ring on the ground: its centre and its two radii (Moho's
/// SRangeExtractionPayload).
struct RangeRing {
    f32 x = 0, z = 0;
    f32 inner = 0, outer = 0;
};

/// A ring line's thickness (Moho's RangeRingRadiusParams): `near` zoomed all
/// the way in; zoomed out, `far` times the map's span and the convar's
/// coefficient. The Lua's {zoomed in, zoomed out} pair.
struct RingThickness {
    f32 near = 0, far = 0;
};

/// RGBA in [0, 1]; the alpha is the glow.
using RangeColor = std::array<f32, 4>;

/// A packed ARGB colour (SCR_DecodeColor's) as Moho's DecodePackedRgbaColor
/// reads it: each byte times 0.003921.
RangeColor range_color(u32 argb);

/// What a profile's name makes of a unit (Moho's sBlueprintExtractors).
enum class RangeExtractor : u8 {
    AllMilitary,
    DirectFire,
    IndirectFire,
    AntiAir,
    AntiNavy,
    Defense,
    Miscellaneous,
    AllIntel,
    Radar,
    Sonar,
    Omni,
    CounterIntel,
};

/// The extractor registered under `name`; nothing for another name.
std::optional<RangeExtractor> range_extractor(std::string_view name);

/// One overlay filter (SetOverlayFilter): its name, which picks the
/// extractor, the units it is for, its colours and its line thicknesses.
struct RangeProfile {
    std::string name;
    sim::CategoryExpr categories;
    RangeColor normal{};   ///< in view with the filter on, and at a placement
    RangeColor selected{}; ///< selected
    RangeColor rollover{}; ///< under the cursor
    RingThickness inner, outer;
};

/// A weapon blueprint's RangeCategory (UWRC_*).
enum class WeaponRangeCategory : u8 {
    Undefined,
    DirectFire,
    IndirectFire,
    AntiAir,
    AntiNavy,
    Countermeasure,
};

WeaponRangeCategory parse_weapon_range_category(std::string_view text);

/// The toggles that hide intel rings (RULEUTC_* bits, which are the script
/// bits' too).
constexpr u32 kJammingToggle = 1u << 2;
constexpr u32 kIntelToggle = 1u << 3;
constexpr u32 kStealthToggle = 1u << 5;

/// What the extractors read off a unit blueprint.
struct RangeBlueprint {
    std::unordered_set<std::string> categories;
    struct Weapon {
        WeaponRangeCategory category = WeaponRangeCategory::Undefined;
        f32 min_radius = 0, max_radius = 0, effective_radius = 0;
    };
    std::vector<Weapon> weapons; ///< in the blueprint's order
    f32 radar = 0, sonar = 0, omni = 0;
    f32 radar_stealth_field = 0, sonar_stealth_field = 0, cloak_field = 0;
    f32 jam_max = 0, spoof_max = 0;
    f32 shield_size = 0;
    f32 staging_scan = 0, guard_scan = 0; ///< AI.StagingPlatformScanRadius, GuardScanRadius
    u32 toggle_caps = 0;                  ///< General.ToggleCaps
};

/// The blueprint table at `index` (the stack is unchanged).
RangeBlueprint read_range_blueprint(lua_State* L, int index);

/// The UI state's unit blueprints as the extractors read them, read once
/// each (from __blueprints, by lowercase id).
class RangeBlueprints {
public:
    /// `id`'s, or nothing if the UI state has no such blueprint.
    const RangeBlueprint* find(const std::string& id, lua_State* L);
    void clear() { cache_.clear(); }

private:
    std::map<std::string, std::optional<RangeBlueprint>, std::less<>> cache_;
};

/// A unit as the extractors see it now: where it is and its reach.
struct RangeUnit {
    const RangeBlueprint* blueprint = nullptr;
    f32 x = 0, z = 0;
    std::span<const sim::WeaponRangeRecord> weapons;
    f32 radar = 0, sonar = 0, omni = 0; ///< its intel radii, on or off
    f32 counter_intel = 0;              ///< its largest stealth or cloak field or spoof
    u16 script_bits = 0;
};

/// `e` of `snap`, at (x, z), with its blueprint's `bp`.
RangeUnit range_unit(const sim::WorldSnapshot& snap, const sim::EntityRecord& e,
                     const RangeBlueprint& bp, f32 x, f32 z);

/// The ring a live unit shows for an extractor (Extract), if any.
std::optional<RangeRing> extract_range(RangeExtractor kind, const RangeUnit& unit);
/// The ring a blueprint shows at (x, z), placing it (Range), if any.
std::optional<RangeRing> blueprint_range(RangeExtractor kind, const RangeBlueprint& bp, f32 x,
                                         f32 z);

/// The profiles (Moho's RangeRenderer::mRangeProfiles, by name), the
/// session's active filters, and the range convars.
class RangeOverlays {
public:
    /// The convars, at Moho's defaults (retail's UI turns the first three
    /// on from the player's prefs).
    struct Settings {
        bool render_selected = false;               ///< range_RenderSelected
        bool render_highlighted = false;            ///< range_RenderHighlighted
        bool render_build = false;                  ///< range_RenderBuild
        bool fill = false;                          ///< range_Fill
        f32 inner_thickness_coeff = 1.0f / 1024.0f; ///< range_InnerThicknessCoeff
        f32 outer_thickness_coeff = 1.0f / 1024.0f; ///< range_OuterThicknessCoeff
        bool enabled = true;                        ///< ren_Ranges
    };

    /// SetOverlayFilter: add or replace the profile of its name.
    void set_profile(RangeProfile profile);
    /// SetOverlayFilters: the active filters' names.
    void set_filters(std::vector<std::string> names) { filters_ = std::move(names); }

    const std::map<std::string, RangeProfile>& profiles() const { return profiles_; }
    const std::vector<std::string>& filters() const { return filters_; }
    /// The profiles the active filters name, in their order
    /// (MoveCategories).
    std::vector<const RangeProfile*> visible() const;

    Settings& settings() { return settings_; }
    const Settings& settings() const { return settings_; }

private:
    std::map<std::string, RangeProfile> profiles_;
    std::vector<std::string> filters_;
    Settings settings_;
};

/// Rings drawn together in one colour and thickness: they merge.
struct RangeBatch {
    RangeColor color{};
    RingThickness inner, outer;
    std::vector<RangeRing> rings;
};

/// What a frame's ranges are drawn for.
struct RangeScene {
    std::vector<RangeUnit> in_view;          ///< the focus army's units in view, alive and built
    std::vector<RangeUnit> selected;         ///< the focus army's selected units
    std::optional<RangeUnit> hovered;        ///< under the cursor, if the focus army's
    const RangeBlueprint* placing = nullptr; ///< a structure being placed,
    f32 cursor_x = 0, cursor_z = 0;          ///< at the cursor
    std::optional<RangeRing> no_rush;        ///< the focus army's no-rush zone
};

/// The batches Moho's RangeRenderer::Render draws, in its order: placement,
/// the active filters, the selection, the hovered unit, the no-rush zone.
std::vector<RangeBatch> range_batches(const RangeOverlays& overlays, const RangeScene& scene);

/// A line's thickness at a zoom (`zoom_ratio`: the target zoom over the
/// largest), for a map whose playable rect's larger side is `span`.
f32 ring_thickness(RingThickness t, f32 coeff, f32 span, f32 zoom_ratio);

/// Each ring's fill, between its two lines, and its two lines (Moho's
/// BuildRingPayloadEntry).
void ring_bands(std::span<const RangeRing> rings, f32 inner_thickness, f32 outer_thickness,
                std::vector<RangeRing>& fills, std::vector<RangeRing>& edges);

/// The ring volume: an annular cylinder of 45 sides, its vertices' inner and
/// outer weights picking the radius (range.fx), its inner circles weighted
/// (1, 0) and outer (0, 1). `bottom` and `top` are its ends' heights.
constexpr u32 kRingSides = 45;
struct RingVertex {
    f32 x = 0, y = 0, z = 0;
    f32 inner = 0, outer = 0;
};
std::vector<RingVertex> ring_volume_vertices(f32 bottom, f32 top);
/// Its triangles (RangeRenderer::Init's four strips).
std::vector<u16> ring_volume_indices();

} // namespace osc::renderer
