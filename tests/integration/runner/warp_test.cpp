// --warp-test: where Warp puts a unit, as the original game does
// (tools/moho_probes `warp`, 2026-10-15).
//
// Moho's Unit::Warp (faf-re) puts a unit on the layer GetStartingLayer gives
// it at the new place, with none asked for, at the height
// IUnit::CalcSpawnElevation does, and aborts its navigator's move. On
// SCMP_009 units are made on the dry ground to the east and warped, each on
// its own row; in the original:
// 1. a tank warped 10 over or 3 under the ground stands on the ground;
// 2. a tank warped into deep water is on the Seabed, on the ground under
//    it, and drowns (1% of its health a tick: 297, 285, 240);
// 3. an amphibious unit there is on the Seabed too;
// 4. a hover tank and a frigate warped to y 0 over deep water are on the
//    Water, at the surface;
// 5. a scout warped to y 0 is in the Air, 18 over the ground;
// 6. a CZAR warped to y 0 is on Land at the ground, and in the Air, climbing
//    (19.94, 24.88, 37.15), from its first tick;
// 7. a tank warped 20 aside on its way somewhere gives its move order up
//    within 10 ticks, and coasts to a stop.
// Before, each stood where Warp's height put it, on the layer it had, and
// the mover went on to its goal.

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
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
    bool wet;          ///< warped over deep water (else dry ground)
    const char* rel;   ///< the height asked: 'ground', 'surface' or 'zero'
    f32 dy;            ///< over it
    const char* layer; ///< the original's layer at once after the warp
};

constexpr std::array<Scene, 8> kScenes = {{
    {"tank-high", "uel0201", false, "ground", 10.0f, "Land"},
    {"tank-low", "uel0201", false, "ground", -3.0f, "Land"},
    {"tank-deep", "uel0201", true, "surface", 0.0f, "Seabed"},
    {"amphib-deep", "url0203", true, "surface", 0.0f, "Seabed"},
    {"hover-deep", "uel0203", true, "zero", 0.0f, "Water"},
    {"frigate", "ues0103", true, "zero", 0.0f, "Water"},
    {"scout", "uea0101", false, "zero", 0.0f, "Air"},
    {"czar", "uaa0310", false, "zero", 0.0f, "Land"},
}};

} // namespace

void test_warp(TestContext& ctx) {
    spdlog::info("=== WARP TEST: where Warp puts a unit, as the original ===");
    Tally t;
    const map::Terrain* terrain = ctx.sim.terrain();
    if (!terrain) {
        t.check(false, "a map is loaded");
        return;
    }
    // The probe's scenes: made on the dry ground east of x 650 (the frigate
    // in the sea west of x 450), warped on the same row
    std::string scenes = "__osc_warp_scenes = {";
    for (const Scene& s : kScenes)
        scenes +=
            fmt::format("{{'{}', {}, '{}', {}}}, ", s.bp, s.wet ? "true" : "false", s.rel, s.dy);
    scenes += "}\n";
    const auto set = ctx.lua_state.do_string(scenes);
    const auto made = ctx.lua_state.do_string(
        "local function depth(x, z) return GetSurfaceHeight(x, z) - GetTerrainHeight(x, z) end\n"
        "local function spot(lo, hi, z0, x0, x1)\n"
        "    for z = z0, z0 + 6 do\n"
        "        local x = x0\n"
        "        while x <= x1 do\n"
        "            local d = depth(x, z)\n"
        "            if d >= lo and d <= hi then return x, z end\n"
        "            x = x + 0.5\n"
        "        end\n"
        "    end\n"
        "end\n"
        "__osc_warp = {}\n"
        "for i, s in __osc_warp_scenes do\n"
        "    local z0 = 100 + (i - 1) * 12\n"
        "    local naval = s[1] == 'ues0103'\n"
        "    local x, z\n"
        "    if naval then x, z = spot(8, 30, z0, 100, 450) else x, z = spot(-1, 0, z0, 650, 1000) "
        "end\n"
        "    local tx, tz\n"
        "    if s[2] then tx, tz = spot(8, 30, z0, 100, 450) else tx, tz = spot(-1, 0, z0, 650, "
        "1000) end\n"
        "    if not x or not tx then error('no spot for ' .. s[1]) end\n"
        "    if x == tx and z == tz then tx = tx + 12 end\n"
        "    local u = CreateUnitHPR(s[1], 'ARMY_1', x, GetTerrainHeight(x, z), z, 0, 0, 0)\n"
        "    __osc_warp[i] = {tonumber(u:GetEntityId()), tx, tz}\n"
        "end\n");
    t.check(set && made, made ? std::string("every scene made")
                              : fmt::format("the scenes made: {}", made.error().message));
    if (!set || !made) return;
    for (int i = 0; i < 10; ++i) ctx.sim.tick();

    const auto warp = ctx.lua_state.do_string(
        "for i, s in __osc_warp_scenes do\n"
        "    local w = __osc_warp[i]\n"
        "    local y = s[4]\n"
        "    if s[3] == 'ground' then y = GetTerrainHeight(w[2], w[3]) + s[4]\n"
        "    elseif s[3] == 'surface' then y = GetSurfaceHeight(w[2], w[3]) + s[4] end\n"
        "    Warp(GetEntityById(w[1]), Vector(w[2], y, w[3]))\n"
        "end\n");
    t.check(static_cast<bool>(warp), warp ? std::string("every scene warped")
                                          : fmt::format("warped: {}", warp.error().message));

    std::array<u32, kScenes.size()> ids{};
    lua_State* L = ctx.L;
    lua_pushstring(L, "__osc_warp");
    lua_rawget(L, LUA_GLOBALSINDEX);
    for (size_t i = 0; i < ids.size(); ++i) {
        lua_rawgeti(L, -1, static_cast<int>(i + 1));
        lua_rawgeti(L, -1, 1);
        ids[i] = lua_isnumber(L, -1) ? static_cast<u32>(lua_tonumber(L, -1)) : 0;
        lua_pop(L, 2);
    }
    lua_pop(L, 1);
    auto& registry = ctx.sim.entity_registry();
    const auto unit = [&](size_t i) -> const sim::Unit* {
        const sim::Entity* e = ids[i] != 0 ? registry.find(ids[i]) : nullptr;
        return e && e->is_unit() && !e->destroyed() ? static_cast<const sim::Unit*>(e) : nullptr;
    };
    const f32 surface = terrain->water_elevation();

    // At once after the warp: each one's layer and height
    for (size_t i = 0; i < kScenes.size(); ++i) {
        const Scene& s = kScenes[i];
        const sim::Unit* u = unit(i);
        if (!u) {
            t.check(false, fmt::format("{} is there", s.name));
            continue;
        }
        const sim::Vector3& p = u->position();
        const f32 ground = terrain->get_terrain_height(p.x, p.z);
        const std::string layer = s.layer;
        f32 want_y = ground;
        if (layer == "Water") want_y = surface;
        if (layer == "Air") want_y = std::max(ground, surface) + 18.0f;
        t.check(u->layer() == s.layer && std::abs(p.y - want_y) < 0.01f,
                fmt::format("{}: warped onto the {} at {:.2f} (on the {}, at {:.2f})", s.name,
                            s.layer, want_y, u->layer(), p.y));
    }

    // The drowning tank and the climbing CZAR, at ticks 1, 5 and 20 after.
    // The probe's own rows match the original's to the hundredth; warped
    // here between ticks, not in a script thread at a tick's start, the
    // climb runs a little ahead.
    const std::array<int, 3> at_tick = {1, 5, 20};
    const std::array<f32, 3> tank_health = {297.0f, 285.0f, 240.0f};
    const std::array<f32, 3> czar_y = {19.94f, 24.88f, 37.15f};
    int tick = 0;
    f32 last_czar_y = -1.0e9f;
    for (size_t k = 0; k < at_tick.size(); ++k) {
        while (tick < at_tick[k]) {
            ctx.sim.tick();
            ++tick;
        }
        const sim::Unit* tank = unit(2);
        t.check(tank && tank->layer() == "Seabed" &&
                    std::abs(tank->health() - tank_health[k]) < 0.05f,
                fmt::format("t={}: the tank in deep water drowns on the Seabed, {:.0f} ({:.1f})",
                            tick, tank_health[k], tank ? tank->health() : 0.0f));
        const sim::Unit* czar = unit(7);
        const f32 y = czar ? czar->position().y : 0.0f;
        t.check(czar && czar->layer() == "Air" && y > last_czar_y && std::abs(y - czar_y[k]) < 0.5f,
                fmt::format("t={}: the CZAR in the Air, climbing, at {:.2f} (the original {:.2f})",
                            tick, y, czar_y[k]));
        last_czar_y = y;
    }

    // A tank on its way, warped 20 aside: its move order ends, and it coasts
    const auto mover = ctx.lua_state.do_string(
        "local function spot(z0, x0, x1)\n"
        "    for z = z0, z0 + 6 do\n"
        "        local x = x0\n"
        "        while x <= x1 do\n"
        "            if GetSurfaceHeight(x, z) - GetTerrainHeight(x, z) <= 0 then return x, z end\n"
        "            x = x + 0.5\n"
        "        end\n"
        "    end\n"
        "end\n"
        "local x, z = spot(230, 680, 760)\n"
        "local gx, gz = spot(230, 760, 800)\n"
        "__osc_mover = CreateUnitHPR('uel0201', 'ARMY_1', x, GetTerrainHeight(x, z), z, 0, 0, 0)\n"
        "__osc_mover_goal = gx\n"
        "__osc_mover_id = tonumber(__osc_mover:GetEntityId())\n");
    if (!mover) {
        t.check(false, fmt::format("the mover made: {}", mover.error().message));
    } else {
        for (int i = 0; i < 5; ++i) ctx.sim.tick();
        (void)ctx.lua_state.do_string(
            "IssueMove({__osc_mover}, Vector(__osc_mover_goal, 0, 230))\n");
        for (int i = 0; i < 20; ++i) ctx.sim.tick();
        (void)ctx.lua_state.do_string(
            "local p = __osc_mover:GetPosition()\n"
            "Warp(__osc_mover, Vector(p[1] + 20, GetTerrainHeight(p[1] + 20, p[3]), p[3]))\n");
        lua_pushstring(L, "__osc_mover_id");
        lua_rawget(L, LUA_GLOBALSINDEX);
        const auto id = static_cast<u32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        const sim::Entity* e = registry.find(id);
        const auto* tank = e && e->is_unit() ? static_cast<const sim::Unit*>(e) : nullptr;
        const f32 warped_x = tank ? tank->position().x : 0.0f;
        const bool ordered = tank && !tank->command_queue().empty();
        for (int i = 0; i < 10; ++i) ctx.sim.tick();
        const f32 coast = tank ? tank->position().x - warped_x : -1.0f;
        t.check(ordered && tank && tank->command_queue().empty() && coast >= 0.0f && coast < 3.0f,
                fmt::format("the mover warped mid-move gives its order up within 10 ticks and "
                            "coasts {:.2f} (the original 2.04)",
                            coast));
    }
    // On its way, warped, its orders cleared and a new Move given in the same
    // step (a teleport's pattern): the abort reached only the order that was
    // running, so the new Move goes on (review)
    const auto after = ctx.lua_state.do_string(
        "local x = 690\n"
        "while GetSurfaceHeight(x, 262) > GetTerrainHeight(x, 262) do x = x + 0.5 end\n"
        "__osc_after = CreateUnitHPR('uel0201', 'ARMY_1', x, GetTerrainHeight(x, 262), 262, 0, "
        "0, 0)\n"
        "__osc_after_x = x\n");
    if (!after) {
        t.check(false, fmt::format("the late mover made: {}", after.error().message));
    } else {
        for (int i = 0; i < 5; ++i) ctx.sim.tick();
        (void)ctx.lua_state.do_string(
            "IssueMove({__osc_after}, Vector(__osc_after_x - 40, 0, 262))\n");
        for (int i = 0; i < 5; ++i) ctx.sim.tick();
        (void)ctx.lua_state.do_string(
            "Warp(__osc_after, Vector(__osc_after_x + 10, 0, 262))\n"
            "IssueClearCommands({__osc_after})\n"
            "IssueMove({__osc_after}, Vector(__osc_after_x + 40, 0, 262))\n");
        const sim::Vector3 start = [&] {
            lua_getglobal(L, "__osc_after");
            lua_pushstring(L, "_c_object");
            lua_rawget(L, -2);
            const auto* e = static_cast<const sim::Entity*>(lua_touserdata(L, -1));
            lua_pop(L, 2);
            return e ? e->position() : sim::Vector3{};
        }();
        // (it turns round from heading west first)
        for (int i = 0; i < 40; ++i) ctx.sim.tick();
        lua_getglobal(L, "__osc_after");
        lua_pushstring(L, "_c_object");
        lua_rawget(L, -2);
        const auto* e = static_cast<const sim::Entity*>(lua_touserdata(L, -1));
        lua_pop(L, 2);
        const f32 went = e ? e->position().x - start.x : 0.0f;
        t.check(went > 3.0f, fmt::format("a Move ordered just after a Warp goes on ({:.2f} east "
                                         "in 40 ticks)",
                                         went));
    }

    // A sub under the water warped onto dry ground stands on it: its dive is
    // over, not held under the surface (review)
    const auto sub = ctx.lua_state.do_string(
        "local x = 100\n"
        "while GetSurfaceHeight(x, 280) - GetTerrainHeight(x, 280) < 8 do x = x + 0.5 end\n"
        "local lx = 700\n"
        "while GetSurfaceHeight(lx, 280) > GetTerrainHeight(lx, 280) do lx = lx + 0.5 end\n"
        "__osc_sub = CreateUnitHPR('ues0203', 'ARMY_1', x, GetTerrainHeight(x, 280), 280, 0, 0, "
        "0)\n"
        "__osc_sub_land = lx\n");
    if (!sub) {
        t.check(false, fmt::format("the sub made: {}", sub.error().message));
    } else {
        for (int i = 0; i < 3; ++i) ctx.sim.tick();
        (void)ctx.lua_state.do_string("Warp(__osc_sub, Vector(__osc_sub_land, 0, 280))\n");
        for (int i = 0; i < 5; ++i) ctx.sim.tick();
        lua_getglobal(L, "__osc_sub");
        lua_pushstring(L, "_c_object");
        lua_rawget(L, -2);
        const auto* e = static_cast<const sim::Entity*>(lua_touserdata(L, -1));
        lua_pop(L, 2);
        const auto* u = e && e->is_unit() ? static_cast<const sim::Unit*>(e) : nullptr;
        const f32 ground = u ? terrain->get_terrain_height(u->position().x, u->position().z) : 0.0f;
        t.check(u && u->layer() == "Land" && std::abs(u->position().y - ground) < 0.01f &&
                    u->sub_elevation() == 0.0f,
                fmt::format("a dived sub warped onto dry ground stands on it ({}, {:.2f}, the "
                            "ground {:.2f})",
                            u ? u->layer() : std::string("gone"), u ? u->position().y : 0.0f,
                            ground));
    }
    spdlog::info("Warp test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
