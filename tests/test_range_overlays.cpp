#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/range_overlays.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using Catch::Approx;
using osc::f32;
using osc::u32;
using osc::renderer::blueprint_range;
using osc::renderer::extract_range;
using osc::renderer::RangeBlueprint;
using osc::renderer::RangeExtractor;
using osc::renderer::RangeOverlays;
using osc::renderer::RangeProfile;
using osc::renderer::RangeRing;
using osc::renderer::RangeScene;
using osc::renderer::RangeUnit;
using osc::renderer::WeaponRangeCategory;
using osc::sim::CategoryExpr;
using osc::sim::WeaponRangeRecord;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

/// A tank: a direct-fire gun (blueprint weapon 0, 3 to 20), an anti-air one
/// (1, 0 to 30), and a death weapon (2, uncategorised)
RangeBlueprint tank() {
    RangeBlueprint bp;
    bp.categories = {"LAND", "MOBILE", "DIRECTFIRE"};
    bp.weapons = {{WeaponRangeCategory::DirectFire, 3, 20, 0},
                  {WeaponRangeCategory::AntiAir, 0, 30, 0},
                  {WeaponRangeCategory::Undefined, 0, 0, 0}};
    return bp;
}

RangeUnit at(const RangeBlueprint& bp, std::span<const WeaponRangeRecord> weapons, f32 x = 10,
             f32 z = 20) {
    RangeUnit u;
    u.blueprint = &bp;
    u.x = x;
    u.z = z;
    u.weapons = weapons;
    return u;
}

RangeProfile profile(const std::string& name, CategoryExpr cats = CategoryExpr::all()) {
    RangeProfile p;
    p.name = name;
    p.categories = std::move(cats);
    p.normal = {1, 0, 0, 0};
    p.selected = {0, 1, 0, 0.03f};
    p.rollover = {0, 0, 1, 0.06f};
    p.inner = {0.02f, 2.0f};
    p.outer = {0.04f, 4.0f};
    return p;
}

} // namespace

TEST_CASE("Range colours decode as Moho's: AARRGGBB, a byte times 0.003921", "[range]") {
    const auto c = osc::renderer::range_color(0x08ff5253u);
    CHECK(c[0] == Approx(255 * 0.0039209998f));
    CHECK(c[1] == Approx(0x52 * 0.0039209998f));
    CHECK(c[2] == Approx(0x53 * 0.0039209998f));
    CHECK(c[3] == Approx(8 * 0.0039209998f)); // the glow
}

TEST_CASE("A profile's name picks its extractor", "[range]") {
    CHECK(osc::renderer::range_extractor("AntiAir") == RangeExtractor::AntiAir);
    CHECK(osc::renderer::range_extractor("CounterIntel") == RangeExtractor::CounterIntel);
    CHECK(osc::renderer::range_extractor("AllMilitary") == RangeExtractor::AllMilitary);
    CHECK_FALSE(osc::renderer::range_extractor("antiair").has_value());
    CHECK_FALSE(osc::renderer::range_extractor("Artillery").has_value());
}

TEST_CASE("A weapon range: the category's largest reach now and smallest minimum", "[range]") {
    const RangeBlueprint bp = tank();
    // The gun's reach changed (ChangeMaxRadius); the death weapon has none
    const std::array<WeaponRangeRecord, 3> now = {{{0, 3, 24}, {1, 0, 30}, {2, 0, 0}}};
    const auto direct = extract_range(RangeExtractor::DirectFire, at(bp, now));
    REQUIRE(direct);
    CHECK(direct->x == 10);
    CHECK(direct->z == 20);
    CHECK(direct->inner == 3);
    CHECK(direct->outer == 24);
    const auto aa = extract_range(RangeExtractor::AntiAir, at(bp, now));
    REQUIRE(aa);
    CHECK(aa->inner == 0);
    CHECK(aa->outer == 30);
    CHECK_FALSE(extract_range(RangeExtractor::IndirectFire, at(bp, now)));
    CHECK_FALSE(extract_range(RangeExtractor::AntiNavy, at(bp, now)));
    // Every weapon (AllMilitary): the largest reach, the smallest minimum
    const auto all = extract_range(RangeExtractor::AllMilitary, at(bp, now));
    REQUIRE(all);
    CHECK(all->inner == 0);
    CHECK(all->outer == 30);
    // A weapon the unit hasn't now counts for nothing
    const std::array<WeaponRangeRecord, 1> aa_only = {{{1, 0, 30}}};
    CHECK_FALSE(extract_range(RangeExtractor::DirectFire, at(bp, aa_only)));
}

TEST_CASE("Placing: a blueprint's weapon range, by its EffectiveRadius", "[range]") {
    RangeBlueprint bp = tank();
    bp.weapons[0].effective_radius = 18;
    const auto direct = blueprint_range(RangeExtractor::DirectFire, bp, 5, 6);
    REQUIRE(direct);
    CHECK(direct->x == 5);
    CHECK(direct->z == 6);
    CHECK(direct->inner == 3);
    CHECK(direct->outer == 18);
    CHECK(blueprint_range(RangeExtractor::AntiAir, bp, 5, 6)->outer == 30);
    // The combined military profile has no blueprint range
    CHECK_FALSE(blueprint_range(RangeExtractor::AllMilitary, bp, 5, 6));
}

TEST_CASE("Defense: half the shield's size, else countermeasure weapons", "[range]") {
    RangeBlueprint bp;
    bp.weapons = {{WeaponRangeCategory::Countermeasure, 0, 25, 0}};
    const std::array<WeaponRangeRecord, 1> now = {{{0, 0, 25}}};
    CHECK(extract_range(RangeExtractor::Defense, at(bp, now))->outer == 25);
    bp.shield_size = 40;
    CHECK(extract_range(RangeExtractor::Defense, at(bp, now))->outer == 20);
    CHECK(blueprint_range(RangeExtractor::Defense, bp, 0, 0)->outer == 20);
}

TEST_CASE("Miscellaneous, and AllMilitary for an OVERLAYMISC unit: its AI reach", "[range]") {
    RangeBlueprint bp = tank();
    bp.guard_scan = 12;
    const std::array<WeaponRangeRecord, 1> now = {{{0, 3, 20}}};
    CHECK(extract_range(RangeExtractor::Miscellaneous, at(bp, now))->outer == 12);
    bp.staging_scan = 15; // preferred where positive
    CHECK(extract_range(RangeExtractor::Miscellaneous, at(bp, now))->outer == 15);
    // AllMilitary reads it only for an OVERLAYMISC unit
    CHECK(extract_range(RangeExtractor::AllMilitary, at(bp, now))->outer == 20);
    bp.categories.insert("OVERLAYMISC");
    CHECK(extract_range(RangeExtractor::AllMilitary, at(bp, now))->outer == 15);
}

TEST_CASE("Intel rings: the radii on or off, hidden by the intel toggle", "[range]") {
    RangeBlueprint bp;
    bp.toggle_caps = osc::renderer::kIntelToggle;
    RangeUnit u = at(bp, {});
    u.radar = 60;
    u.omni = 20;
    CHECK(extract_range(RangeExtractor::Radar, u)->outer == 60);
    CHECK(extract_range(RangeExtractor::Omni, u)->outer == 20);
    CHECK_FALSE(extract_range(RangeExtractor::Sonar, u));
    CHECK(extract_range(RangeExtractor::AllIntel, u)->outer == 60);
    u.script_bits = 1u << 3; // toggled off
    CHECK_FALSE(extract_range(RangeExtractor::Radar, u));
    CHECK_FALSE(extract_range(RangeExtractor::AllIntel, u));
    // Without the toggle the bit means nothing here
    bp.toggle_caps = 0;
    CHECK(extract_range(RangeExtractor::Radar, u)->outer == 60);
    // Placing one: the blueprint's radii (AllIntel with its stealth field)
    bp.radar = 55;
    bp.radar_stealth_field = 70;
    CHECK(blueprint_range(RangeExtractor::Radar, bp, 0, 0)->outer == 55);
    CHECK(blueprint_range(RangeExtractor::AllIntel, bp, 0, 0)->outer == 70);
}

TEST_CASE("Counter-intel: its fields and jamming, hidden by either toggle", "[range]") {
    RangeBlueprint bp;
    bp.jam_max = 30;
    bp.toggle_caps = osc::renderer::kJammingToggle | osc::renderer::kStealthToggle;
    RangeUnit u = at(bp, {});
    u.counter_intel = 25;
    CHECK(extract_range(RangeExtractor::CounterIntel, u)->outer == 30);
    u.counter_intel = 45;
    CHECK(extract_range(RangeExtractor::CounterIntel, u)->outer == 45);
    u.script_bits = 1u << 5; // stealth off
    CHECK_FALSE(extract_range(RangeExtractor::CounterIntel, u));
    u.script_bits = 1u << 2; // jamming off
    CHECK_FALSE(extract_range(RangeExtractor::CounterIntel, u));
    bp.cloak_field = 50;
    CHECK(blueprint_range(RangeExtractor::CounterIntel, bp, 0, 0)->outer == 50);
}

TEST_CASE("A blueprint table as the extractors read it", "[range]") {
    LuaGuard g;
    const char* source = R"lua(return {
        BlueprintId = 'uab0101',
        Categories = {'STRUCTURE', 'OVERLAYMISC'},
        Weapon = {
            {RangeCategory = 'UWRC_IndirectFire', MinRadius = 5, MaxRadius = 60, EffectiveRadius = 55},
            'not a weapon',
            {RangeCategory = 'UWRC_AntiNavy', MaxRadius = 40},
        },
        Intel = {RadarRadius = 115, SonarRadius = 20, OmniRadius = 10,
                 CloakFieldRadius = 12, JamRadius = {Min = 5, Max = 25},
                 SpoofRadius = {Min = 2, Max = 8}},
        Defense = {Shield = {ShieldSize = 46}},
        AI = {GuardScanRadius = 26, StagingPlatformScanRadius = 30},
        General = {ToggleCaps = {RULEUTC_IntelToggle = true, RULEUTC_StealthToggle = true}},
    })lua";
    REQUIRE(luaL_loadbuffer(g.L, source, std::strlen(source), "bp") == 0);
    REQUIRE(lua_pcall(g.L, 0, 1, 0) == 0);
    const int top = lua_gettop(g.L);
    const RangeBlueprint bp = osc::renderer::read_range_blueprint(g.L, -1);
    CHECK(lua_gettop(g.L) == top);
    CHECK(bp.categories.count("OVERLAYMISC") == 1);
    CHECK(bp.categories.count("uab0101") == 1); // its id, as Moho registers
    // Weapons keep their places: the unit's weapon 2 is the list's third
    REQUIRE(bp.weapons.size() == 3);
    CHECK(bp.weapons[0].category == WeaponRangeCategory::IndirectFire);
    CHECK(bp.weapons[0].min_radius == 5);
    CHECK(bp.weapons[0].max_radius == 60);
    CHECK(bp.weapons[0].effective_radius == 55);
    CHECK(bp.weapons[1].category == WeaponRangeCategory::Undefined);
    CHECK(bp.weapons[2].category == WeaponRangeCategory::AntiNavy);
    CHECK(bp.weapons[2].max_radius == 40);
    CHECK(bp.radar == 115);
    CHECK(bp.sonar == 20);
    CHECK(bp.omni == 10);
    CHECK(bp.cloak_field == 12);
    CHECK(bp.jam_max == 25);
    CHECK(bp.spoof_max == 8);
    CHECK(bp.shield_size == 46);
    CHECK(bp.guard_scan == 26);
    CHECK(bp.staging_scan == 30);
    CHECK(bp.toggle_caps == (osc::renderer::kIntelToggle | osc::renderer::kStealthToggle));
}

TEST_CASE("The blueprint cache reads __blueprints by lowercase id, once", "[range]") {
    LuaGuard g;
    const char* source = "__blueprints = {uel0201 = {Intel = {RadarRadius = 40}}}";
    REQUIRE(luaL_loadbuffer(g.L, source, std::strlen(source), "bps") == 0);
    REQUIRE(lua_pcall(g.L, 0, 0, 0) == 0);
    osc::renderer::RangeBlueprints cache;
    const RangeBlueprint* bp = cache.find("UEL0201", g.L);
    REQUIRE(bp);
    CHECK(bp->radar == 40);
    CHECK(cache.find("UEL0201", g.L) == bp);
    CHECK(cache.find("xyz0000", g.L) == nullptr);
    CHECK(lua_gettop(g.L) == 0);
}

TEST_CASE("Profiles are kept by name; the filters name the visible ones, in order", "[range]") {
    RangeOverlays o;
    o.set_profile(profile("Radar"));
    o.set_profile(profile("AntiAir"));
    RangeProfile again = profile("Radar");
    again.normal = {0, 0, 0, 1};
    o.set_profile(again);
    REQUIRE(o.profiles().size() == 2);
    CHECK(o.profiles().at("Radar").normal[3] == 1);
    o.set_filters({"Radar", "Nothing", "AntiAir"});
    const auto visible = o.visible();
    REQUIRE(visible.size() == 2);
    CHECK(visible[0]->name == "Radar");
    CHECK(visible[1]->name == "AntiAir");
}

TEST_CASE("A frame's batches, in Moho's order and colours", "[range]") {
    RangeOverlays o;
    o.set_profile(profile("AllMilitary"));
    o.set_profile(profile("DirectFire"));
    o.set_profile(profile("AntiAir", CategoryExpr::name("AIR"))); // the tank isn't
    o.set_profile(profile("Radar"));
    o.set_filters({"AllMilitary"});
    const RangeBlueprint bp = tank();
    const std::array<WeaponRangeRecord, 2> now = {{{0, 3, 20}, {1, 0, 30}}};
    RangeScene scene;
    scene.in_view = {at(bp, now, 1, 1), at(bp, now, 2, 2)};
    scene.selected = {at(bp, now, 1, 1)};
    scene.hovered = at(bp, now, 2, 2);
    scene.placing = &bp;
    scene.cursor_x = 7;
    scene.cursor_z = 8;
    scene.no_rush = RangeRing{50, 60, 0, 75};

    // The convars off (Moho's defaults): the filters and the no-rush zone
    auto batches = osc::renderer::range_batches(o, scene);
    REQUIRE(batches.size() == 2);
    CHECK(batches[0].color == o.profiles().at("AllMilitary").normal);
    REQUIRE(batches[0].rings.size() == 2);
    CHECK(batches[0].rings[1].x == 2);
    CHECK(batches[0].rings[1].outer == 30);
    CHECK(batches[1].rings[0].outer == 75);
    CHECK(batches[1].color[0] == Approx(0.2f));
    CHECK(batches[1].inner.near == Approx(0.1f));
    CHECK(batches[1].outer.far == Approx(2.0f));

    // All on, as retail's UI sets them: placement, filters, selection, hover
    o.settings().render_build = true;
    o.settings().render_selected = true;
    o.settings().render_highlighted = true;
    batches = osc::renderer::range_batches(o, scene);
    // Placing: DirectFire only (AllMilitary is combined, AntiAir isn't for
    // it, the tank has no radar), at the cursor in its normal colour
    REQUIRE(batches.size() == 5);
    CHECK(batches[0].color == o.profiles().at("DirectFire").normal);
    CHECK(batches[0].rings.size() == 1);
    CHECK(batches[0].rings[0].x == 7);
    CHECK(batches[0].rings[0].z == 8);
    CHECK(batches[1].color == o.profiles().at("AllMilitary").normal);
    CHECK(batches[2].color == o.profiles().at("DirectFire").selected);
    CHECK(batches[2].rings.size() == 1);
    CHECK(batches[3].color == o.profiles().at("DirectFire").rollover);
    CHECK(batches[3].rings[0].x == 2);
    CHECK(batches[4].rings[0].outer == 75);

    // ren_Ranges off: nothing
    o.settings().enabled = false;
    CHECK(osc::renderer::range_batches(o, scene).empty());
}

TEST_CASE("Ring lines thicken as the camera zooms out", "[range]") {
    // AntiAir's outer line {0.04, 4} on a 512 map, coefficient 1/1024
    const osc::renderer::RingThickness t{0.04f, 4.0f};
    constexpr f32 kCoeff = 1.0f / 1024.0f;
    CHECK(osc::renderer::ring_thickness(t, kCoeff, 512, 0) == Approx(0.04f));
    CHECK(osc::renderer::ring_thickness(t, kCoeff, 512, 1) == Approx(2.0f));
    CHECK(osc::renderer::ring_thickness(t, kCoeff, 512, 0.5f) == Approx(1.02f));
}

TEST_CASE("A ring's fill lies between its lines; no inner ring, no inner line", "[range]") {
    const std::array<RangeRing, 2> rings = {{{1, 2, 3, 20}, {4, 5, 0, 30}}};
    std::vector<RangeRing> fills;
    std::vector<RangeRing> edges;
    osc::renderer::ring_bands(rings, 0.5f, 1.0f, fills, edges);
    REQUIRE(fills.size() == 2);
    REQUIRE(edges.size() == 4);
    CHECK(fills[0].inner == 3.5f);
    CHECK(fills[0].outer == 19.0f);
    CHECK(edges[0].inner == 3.0f);
    CHECK(edges[0].outer == 3.5f);
    CHECK(edges[1].inner == 19.0f);
    CHECK(edges[1].outer == 20.0f);
    // No minimum: the fill reaches the centre (its line, a disc there, is
    // inside it)
    CHECK(fills[1].inner == 0.0f);
    CHECK(edges[2].outer == 0.5f);
    CHECK(fills[1].x == 4);
    CHECK(edges[3].z == 5);
}

TEST_CASE("The ring volume is closed, facing out but for its top", "[range]") {
    const auto v = osc::renderer::ring_volume_vertices(-5, 50);
    const auto idx = osc::renderer::ring_volume_indices();
    REQUIRE(v.size() == osc::renderer::kRingSides * 4);
    REQUIRE(idx.size() == osc::renderer::kRingSides * 24);
    // A ring of radii 10 and 20, as the vertex shader places it
    const auto place = [&](u32 i) {
        const f32 r = v[i].inner * 10 + v[i].outer * 20;
        return std::array<f32, 3>{v[i].x * r, v[i].y, v[i].z * r};
    };
    int bottom = 0, top = 0, walls = 0;
    for (size_t t = 0; t < idx.size(); t += 3) {
        const auto a = place(idx[t]);
        const auto b = place(idx[t + 1]);
        const auto c = place(idx[t + 2]);
        const std::array<f32, 3> e1 = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const std::array<f32, 3> e2 = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const std::array<f32, 3> n = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                      e1[0] * e2[1] - e1[1] * e2[0]};
        const f32 cx = (a[0] + b[0] + c[0]) / 3;
        const f32 cy = (a[1] + b[1] + c[1]) / 3;
        const f32 cz = (a[2] + b[2] + c[2]) / 3;
        const f32 radial = std::sqrt(cx * cx + cz * cz);
        if (std::abs(n[1]) > 1e-3f && std::abs(n[0]) + std::abs(n[2]) < 1e-3f * std::abs(n[1])) {
            // A cap: the bottom faces down, out of the volume; so does the
            // top, into it (Moho's strips; the top lies over the ground, so
            // no ground point counts it)
            CHECK(n[1] < 0);
            (cy < 0 ? bottom : top)++;
        } else {
            // A wall faces out of the volume: away from the axis on the
            // outer, toward it on the inner
            const f32 out = (n[0] * cx + n[2] * cz) / radial;
            CHECK(out * (radial > 15 ? 1.0f : -1.0f) > 0);
            ++walls;
        }
    }
    CHECK(bottom == 90);
    CHECK(top == 90);
    CHECK(walls == 180);
}
