// --placement-layers-test: where a structure may go, by Moho's OCCUPY_Check
// (faf-re sim/STIMap.cpp) with retail blueprints on Seton's Clutch:
// 1. the Cybran HARMS (XRB2308: BuildOnLayerCaps Sub alone, MinWaterDepth 2)
//    can be built in deep water, and not on land or in shallow water;
// 2. made, it stands on the Sub layer;
// 3. the Cybran naval factory (URB0103, MinWaterDepth 1.5) needs that depth;
// 4. a power generator (UEB1101) won't fit across a slope that a wall
//    (UEB5101, MaxGroundVariation 50) takes.
//
// Before, only LAYER_Land/Water/Seabed were read, so the HARMS could be
// built nowhere; water structures went in any depth, and slope was judged
// per 2-unit cell, for walls too.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <optional>
#include <string>

namespace osc::test {

namespace {

/// A spot whose ground, over the square of `half` around it, lies between
/// `lo` and `hi` below the water's surface (positive: under it).
std::optional<Spot> water_spot(const map::Terrain& t, f32 lo, f32 hi, i32 half) {
    const auto w = static_cast<i32>(t.map_width());
    const auto h = static_cast<i32>(t.map_height());
    for (i32 z = 20; z < h - 20; z += 4) {
        for (i32 x = 20; x < w - 20; x += 4) {
            bool fits = true;
            for (i32 dz = -half; dz <= half && fits; ++dz)
                for (i32 dx = -half; dx <= half && fits; ++dx) {
                    const f32 depth =
                        t.water_elevation() -
                        t.get_terrain_height(static_cast<f32>(x + dx), static_cast<f32>(z + dz));
                    fits = depth >= lo && depth <= hi;
                }
            if (fits) return Spot{static_cast<f32>(x), static_cast<f32>(z)};
        }
    }
    return std::nullopt;
}

} // namespace

void test_placement_layers(TestContext& ctx) {
    spdlog::info("=== PLACEMENT LAYERS TEST: Moho's OCCUPY_Check with retail blueprints ===");
    Tally t;
    const map::Terrain* terrain = ctx.sim.terrain();
    if (!terrain || !terrain->has_water()) {
        t.check(false, "a map with water");
        return;
    }
    const auto lua_bool = [&](const std::string& code) {
        auto r = ctx.lua_state.do_string(code);
        if (!r) {
            spdlog::warn("placement-layers: {}", r.error().message);
            return std::string("error");
        }
        lua_pushstring(ctx.L, "__osc_pl");
        lua_rawget(ctx.L, LUA_GLOBALSINDEX);
        const std::string v = lua_isstring(ctx.L, -1) ? lua_tostring(ctx.L, -1) : "nil";
        lua_pop(ctx.L, 1);
        return v;
    };
    const auto can_build = [&](const char* bp, f32 x, f32 z) {
        return lua_bool(fmt::format("__osc_pl = tostring(GetArmyBrain('ARMY_1'):"
                                    "CanBuildStructureAt('{}', {{{}, 0, {}}}))",
                                    bp, x, z)) == "true";
    };

    const auto deep = water_spot(*terrain, 8.0f, 1000.0f, 8);
    const auto shallow = water_spot(*terrain, 0.3f, 1.2f, 8);
    const auto land = quiet_spot(ctx.sim);
    if (!deep || !shallow || !land) {
        t.check(false, fmt::format("deep water ({}), shallow water ({}) and land ({}) on the map",
                                   deep.has_value(), shallow.has_value(), land.has_value()));
        return;
    }
    t.check(can_build("xrb2308", deep->x, deep->z) && !can_build("xrb2308", land->x, land->z) &&
                !can_build("xrb2308", shallow->x, shallow->z),
            "Test 1: the HARMS goes in deep water, not on land or in the shallows");

    t.check(can_build("urb0103", deep->x, deep->z) && !can_build("urb0103", shallow->x, shallow->z),
            "Test 3: the naval factory goes in deep water, not in water shallower than its "
            "MinWaterDepth");

    run_lua(ctx, fmt::format("__osc_pl_harms = CreateUnitHPR('xrb2308', 'ARMY_1', {0}, "
                             "GetSurfaceHeight({0}, {1}), {1}, 0, 0, 0)",
                             deep->x, deep->z));
    t.check(lua_bool("__osc_pl = __osc_pl_harms and __osc_pl_harms:GetCurrentLayer() or 'none'") ==
                "Sub",
            "Test 2: made, the HARMS stands on the Sub layer");

    // A slope a wall takes and a power generator doesn't: the first land
    // spot, on a lattice, where the two disagree.
    bool found = false;
    const auto w = static_cast<i32>(terrain->map_width());
    const auto h = static_cast<i32>(terrain->map_height());
    for (i32 iz = 30; iz < h - 30 && !found; iz += 6) {
        for (i32 ix = 30; ix < w - 30 && !found; ix += 6) {
            const auto x = static_cast<f32>(ix);
            const auto z = static_cast<f32>(iz);
            if (terrain->get_terrain_height(x, z) < terrain->water_elevation() + 1) continue;
            found = can_build("ueb5101", x + 0.5f, z + 0.5f) && !can_build("ueb1101", x, z);
        }
    }
    t.check(found, "Test 4: a slope a wall takes and a power generator doesn't");

    spdlog::info("=== PLACEMENT LAYERS TEST: {} passed, {} failed ===", t.pass, t.fail);
}

} // namespace osc::test
