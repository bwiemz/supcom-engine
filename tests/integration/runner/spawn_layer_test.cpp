// --spawn-layer-test: where a unit made by script stands, as the original
// game makes it (tools/moho_probes `layers`, 2026-10-14).
//
// Subs, a frigate and aircraft are made on SCMP_009, each on its own rows at
// the first spot as deep as it wants, with the script's height on the ground
// under it. In the original (Moho's Unit constructor: GetStartingLayer, then
// IUnit::CalcSpawnElevation):
// 1. a sub starts on the Sub layer at its depth, the surface less 1.5, even
//    in water too shallow for it;
// 2. a frigate starts on the Water, at the surface;
// 3. a T1 scout starts in the Air, 18 over the surface;
// 4. an experimental aircraft (Soul Ripper, CZAR) starts on Land at the
//    ground, and is in the Air, climbing, from its first tick.
// Before, the frigate stood on the seabed until it moved, and the
// experimentals started in the Air at full height.

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace osc::test {

namespace {

struct Scene {
    const char* name;
    const char* bp;
    f32 lo, hi;        ///< the depth it wants
    const char* layer; ///< the original's layer as it is made
};

constexpr std::array<Scene, 7> kScenes = {{
    {"sub", "ues0203", 8.0f, 30.0f, "Sub"},
    {"sub-shallow", "ues0203", 0.8f, 1.2f, "Sub"},
    {"frigate", "ues0103", 8.0f, 30.0f, "Water"},
    {"scout", "uea0101", 0.0f, 0.0f, "Air"},
    {"soulripper", "ura0401", 0.0f, 0.0f, "Land"},
    {"czar", "uaa0310", 0.0f, 0.0f, "Land"},
    {"tank", "uel0201", 0.0f, 0.0f, "Land"},
}};

} // namespace

void test_spawn_layer(TestContext& ctx) {
    spdlog::info("=== SPAWN LAYER TEST: where a unit made by script stands, as the original ===");
    Tally t;
    const map::Terrain* terrain = ctx.sim.terrain();
    if (!terrain) {
        t.check(false, "a map is loaded");
        return;
    }
    std::string scenes = "__osc_spawn_scenes = {";
    for (const Scene& s : kScenes) scenes += fmt::format("{{'{}', {}, {}}}, ", s.bp, s.lo, s.hi);
    scenes += "}\n";
    const auto set = ctx.lua_state.do_string(scenes);
    const auto made = ctx.lua_state.do_string(
        "local function depth(x, z) return GetSurfaceHeight(x, z) - GetTerrainHeight(x, z) end\n"
        "local function spot(lo, hi, z0)\n"
        "    for z = z0, z0 + 6 do\n"
        "        local x = 400\n"
        "        while x <= 900 do\n"
        "            local d = depth(x, z)\n"
        "            if d >= lo and d <= hi then return x, z end\n"
        "            x = x + 0.5\n"
        "        end\n"
        "    end\n"
        "end\n"
        "__osc_spawn = {}\n"
        "for i, s in __osc_spawn_scenes do\n"
        "    local x, z = spot(s[2], s[3], 300 + (i - 1) * 8)\n"
        "    if not x then error('no spot for ' .. s[1]) end\n"
        "    local u = CreateUnitHPR(s[1], 'ARMY_1', x, GetTerrainHeight(x, z), z, 0, 0, 0)\n"
        "    __osc_spawn[i] = tonumber(u:GetEntityId())\n"
        "end\n");
    t.check(set && made, made ? std::string("every scene made")
                              : fmt::format("the scenes made: {}", made.error().message));
    if (!set || !made) return;

    std::array<u32, kScenes.size()> ids{};
    lua_State* L = ctx.L;
    lua_pushstring(L, "__osc_spawn");
    lua_rawget(L, LUA_GLOBALSINDEX);
    for (size_t i = 0; i < ids.size(); ++i) {
        lua_rawgeti(L, -1, static_cast<int>(i + 1));
        ids[i] = osc::sim::to_entity_id(L, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    auto& registry = ctx.sim.entity_registry();
    const auto unit = [&](size_t i) -> const sim::Unit* {
        const sim::Entity* e = ids[i] != 0 ? registry.find(ids[i]) : nullptr;
        return e && e->is_unit() && !e->destroyed() ? static_cast<const sim::Unit*>(e) : nullptr;
    };
    const f32 surface = terrain->water_elevation();

    // As made: each one's layer and height
    std::array<f32, kScenes.size()> made_y{};
    for (size_t i = 0; i < kScenes.size(); ++i) {
        const Scene& s = kScenes[i];
        const sim::Unit* u = unit(i);
        if (!u) {
            t.check(false, fmt::format("{} is made", s.name));
            continue;
        }
        const sim::Vector3& p = u->position();
        made_y[i] = p.y;
        const f32 ground = terrain->get_terrain_height(p.x, p.z);
        f32 want_y = ground;
        const std::string layer = s.layer;
        if (layer == "Sub") want_y = surface - 1.5f;
        if (layer == "Water") want_y = surface;
        if (layer == "Air") want_y = std::max(ground, surface) + 18.0f;
        t.check(u->layer() == s.layer && std::abs(p.y - want_y) < 0.01f,
                fmt::format("{}: starts on the {} at {:.2f} (on the {}, at {:.2f})", s.name,
                            s.layer, want_y, u->layer(), p.y));
    }

    // An experimental aircraft is in the Air from its first tick, climbing
    ctx.sim.tick();
    for (const size_t i : {size_t{4}, size_t{5}}) {
        const sim::Unit* u = unit(i);
        t.check(u && u->layer() == "Air" && u->position().y > made_y[i],
                fmt::format("{}: in the Air after a tick, and climbing ({}, {:.2f} from {:.2f})",
                            kScenes[i].name, u ? u->layer() : std::string("none"),
                            u ? u->position().y : 0.0f, made_y[i]));
    }
    spdlog::info("Spawn layer test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
