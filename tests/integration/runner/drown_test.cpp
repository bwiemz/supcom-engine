// --drown-test: units made in water, as the original game plays them
// (tools/moho_probes `drown`, 2026-10-14).
//
// UEF T1 tanks (UEL0201, RULEUMT_Land) are made in water 0.3 to 4 deep, and
// in 4 deep an amphibious tank (URL0203), a hover tank (UEL0203) and an
// engineer (UEL0105, AmphibiousFloating). In the original:
// 1. every tank starts on the Seabed, however shallow the water, and so
//    does the amphibious tank; the hover tank and the engineer start on the
//    Water (Moho's Entity::GetStartingLayer);
// 2. a tank, on a layer it can't be on, loses 1% of its health a tick (300:
//    288 after 4 ticks, 180 after 40) and is gone by its 100th tick (Moho's
//    CUnitMotion::CalcMoveCommon);
// 3. the others are unharmed, and so is a tank on dry ground beside them.
// Before, every one started on Land, and none was harmed.

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cmath>
#include <string>

namespace osc::test {

namespace {

struct Scene {
    const char* name;
    const char* bp;
    f32 depth;
    const char* layer; ///< the original's layer as it is made
    bool drowns;
};

constexpr std::array<Scene, 9> kScenes = {{
    {"tank-dry", "uel0201", 0.0f, "Land", false}, // a control, on dry ground
    {"tank-0.2", "uel0201", 0.2f, "Seabed", true},
    {"tank-0.6", "uel0201", 0.6f, "Seabed", true},
    {"tank-1.2", "uel0201", 1.2f, "Seabed", true},
    {"tank-2.5", "uel0201", 2.5f, "Seabed", true},
    {"tank-4", "uel0201", 4.0f, "Seabed", true},
    {"amphibious-4", "url0203", 4.0f, "Seabed", false},
    {"hover-4", "uel0203", 4.0f, "Water", false},
    {"engineer-4", "uel0105", 4.0f, "Water", false},
}};

} // namespace

void test_drown(TestContext& ctx) {
    spdlog::info("=== DROWN TEST: units made in water, as the original ===");
    Tally t;
    auto& registry = ctx.sim.entity_registry();
    // Each scene on its own rows, 8 apart, at the first spot as deep as it
    // wants (the probe's scan)
    std::string scenes = "__osc_drown_scenes = {";
    for (const Scene& s : kScenes) scenes += fmt::format("{{'{}', {}}}, ", s.bp, s.depth);
    scenes += "}\n";
    const auto set = ctx.lua_state.do_string(scenes);
    const auto spawn = ctx.lua_state.do_string(
        "local function depth(x, z) return GetSurfaceHeight(x, z) - GetTerrainHeight(x, z) end\n"
        "local function spot(want, z0)\n"
        "    for z = z0, z0 + 6 do\n"
        "        local x = 540\n"
        "        while x <= 760 do\n"
        "            if math.abs(depth(x, z) - want) <= 0.1 then return x, z end\n"
        "            x = x + 0.5\n"
        "        end\n"
        "    end\n"
        "end\n"
        "__osc_drown = {}\n"
        "for i, s in __osc_drown_scenes do\n"
        "    local x, z = spot(s[2], 300 + (i - 1) * 8)\n"
        "    if not x then error('no spot ' .. s[2] .. ' deep') end\n"
        "    local u = CreateUnitHPR(s[1], 'ARMY_1', x, GetTerrainHeight(x, z), z, 0, 0, 0)\n"
        "    __osc_drown[i] = tonumber(u:GetEntityId())\n"
        "end\n");
    t.check(set && spawn, spawn ? std::string("every scene made, each as deep as it wants")
                                : fmt::format("the scenes made: {}", spawn.error().message));
    if (!set || !spawn) return;

    std::array<sim::EntityId, kScenes.size()> ids{};
    lua_State* L = ctx.L;
    lua_pushstring(L, "__osc_drown");
    lua_rawget(L, LUA_GLOBALSINDEX);
    for (size_t i = 0; i < ids.size(); ++i) {
        lua_rawgeti(L, -1, static_cast<int>(i + 1));
        ids[i] = osc::sim::to_entity_id(L, -1, registry);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    const auto unit = [&](sim::EntityId id) -> sim::Unit* {
        sim::Entity* e = id ? registry.find(id) : nullptr;
        return e && e->is_unit() && !e->destroyed() ? static_cast<sim::Unit*>(e) : nullptr;
    };

    // 1. As made
    for (size_t i = 0; i < kScenes.size(); ++i) {
        const sim::Unit* u = unit(ids[i]);
        t.check(u && u->layer() == kScenes[i].layer,
                fmt::format("1: {} starts on the {} ({})", kScenes[i].name, kScenes[i].layer,
                            u ? u->layer() : std::string("none")));
    }

    // 2. A tank's health over the ticks, and when it is gone; 3. the others'
    std::array<int, kScenes.size()> gone{};
    std::array<f32, kScenes.size()> at4{}, at40{};
    for (int tick = 1; tick <= 120; ++tick) {
        ctx.sim.tick();
        for (size_t i = 0; i < kScenes.size(); ++i) {
            const sim::Unit* u = unit(ids[i]);
            const bool alive = u && !u->is_dying();
            if (!alive && gone[i] == 0) gone[i] = tick;
            if (tick == 4) at4[i] = alive ? u->health() : -1.0f;
            if (tick == 40) at40[i] = alive ? u->health() : -1.0f;
        }
    }
    for (size_t i = 0; i < kScenes.size(); ++i) {
        const Scene& s = kScenes[i];
        if (s.drowns) {
            t.check(std::abs(at4[i] - 288.0f) < 0.01f && std::abs(at40[i] - 180.0f) < 0.01f,
                    fmt::format("2: {} loses 1% a tick: {:.1f} after 4 ticks, {:.1f} after 40 "
                                "(the original's 288, 180)",
                                s.name, at4[i], at40[i]));
            t.check(gone[i] >= 97 && gone[i] <= 101,
                    fmt::format("2: {} is gone by its 100th tick (on tick {})", s.name, gone[i]));
        } else {
            const sim::Unit* u = unit(ids[i]);
            t.check(u && !u->is_dying() && u->health() == u->max_health(),
                    fmt::format("3: {} is unharmed after 120 ticks ({:.1f})", s.name,
                                u ? u->health() : -1.0f));
        }
    }
    spdlog::info("Drown test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
